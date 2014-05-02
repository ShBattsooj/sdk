/**
 * @file win32/net.cpp
 * @brief Win32 network access layer (using WinHTTP)
 *
 * (c) 2013-2014 by Mega Limited, Wellsford, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "meganet.h"

namespace mega {
WinHttpIO::WinHttpIO()
{
    InitializeCriticalSection(&csHTTP);
    EnterCriticalSection(&csHTTP);

    hWakeupEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    waiter = NULL;
}

WinHttpIO::~WinHttpIO()
{
    WinHttpCloseHandle(hSession);
    LeaveCriticalSection(&csHTTP);
}

void WinHttpIO::setuseragent(string* useragent)
{
    string wuseragent;

    wuseragent.resize((useragent->size() + 1) * sizeof(wchar_t));
    wuseragent.resize(sizeof(wchar_t)
                      * (MultiByteToWideChar(CP_UTF8, 0, useragent->c_str(),
                                              -1, (wchar_t*)wuseragent.data(),
                                              wuseragent.size() / sizeof(wchar_t) + 1)
                          - 1));

    // create the session handle using the default settings.
    hSession = WinHttpOpen((LPCWSTR)wuseragent.data(),
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS,
                           WINHTTP_FLAG_ASYNC);
}

// trigger wakeup
void WinHttpIO::httpevent()
{
    SetEvent(hWakeupEvent);
}

// (WinHTTP unfortunately uses threads, hence the need for a mutex)
void WinHttpIO::entercs()
{
    EnterCriticalSection(&csHTTP);
}

void WinHttpIO::leavecs()
{
    LeaveCriticalSection(&csHTTP);
}

// ensure wakeup from WinHttpIO events
void WinHttpIO::addevents(Waiter* cwaiter, int flags)
{
    waiter = (WinWaiter*)cwaiter;

    waiter->addhandle(hWakeupEvent, flags);
    waiter->pcsHTTP = &csHTTP;
}

// handle WinHTTP callbacks (which can be in a worker thread context)
VOID CALLBACK WinHttpIO::asynccallback(HINTERNET hInternet, DWORD_PTR dwContext,
                                       DWORD dwInternetStatus,
                                       LPVOID lpvStatusInformation,
                                       DWORD dwStatusInformationLength)
{
    WinHttpContext* httpctx = (WinHttpContext*)dwContext;
    WinHttpIO* httpio = (WinHttpIO*)httpctx->httpio;

    if (dwInternetStatus == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
    {
        assert(!httpctx->req);

        delete httpctx;
        return;
    }

    httpio->entercs();

    HttpReq* req = httpctx->req;

    // request cancellations that occured after asynccallback() was entered are caught here
    if (!req)
    {
        httpio->leavecs();
        return;
    }

    switch (dwInternetStatus)
    {
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        {
            DWORD size = *(DWORD*)lpvStatusInformation;

            if (!size)
            {
                if (debug)
                {
                    if (req->binary)
                    {
                        cout << "[received " << req->bufpos << " bytes of raw data]" << endl;
                    }
                    else
                    {
                        cout << "Received: " << req->in.c_str() << endl;
                    }
                }

                req->status = req->httpstatus == 200 ? REQ_SUCCESS : REQ_FAILURE;
                httpio->success = true;
                httpio->httpevent();
            }
            else
            {
                char* ptr;
                string buf;

                if (httpctx->gzip)
                {
                    buf.resize(size);
                    ptr = (char*)buf.data();
                }
                else
                {
                    ptr = (char*)req->reserveput((unsigned*)&size);
                }

                if (WinHttpReadData(hInternet, ptr, size, NULL))
                {
                    if (httpctx->gzip)
                    {
                        httpctx->z.next_in = (Bytef*)ptr;
                        httpctx->z.avail_in = size;

                        int t = inflate(&httpctx->z, Z_NO_FLUSH);

                        if (t != Z_OK && (t != Z_STREAM_END || httpctx->z.avail_out))
                        {
                            httpio->cancel(req);
                        }
                    }
                }
                else
                {
                    httpio->cancel(req);
                }
            }

            httpio->httpevent();
            break;
        }

        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            if (dwStatusInformationLength)
            {
                req->completeput(dwStatusInformationLength);

                if (!WinHttpQueryDataAvailable(httpctx->hRequest, NULL))
                {
                    httpio->cancel(req);
                    httpio->httpevent();
                }
            }
            break;

        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        {
            DWORD statusCode;
            DWORD statusCodeSize = sizeof(statusCode);

            if (!WinHttpQueryHeaders(httpctx->hRequest,
                                     WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX,
                                     &statusCode,
                                     &statusCodeSize,
                                     WINHTTP_NO_HEADER_INDEX))
            {
                httpio->cancel(req);
                httpio->httpevent();
            }
            else
            {
                req->httpstatus = statusCode;

                if (req->buf)
                {
                    httpctx->gzip = false;
                }
                else
                {
                    // obtain original content length - always present if gzip is in use
                    DWORD contentLength;
                    DWORD contentLengthSize = sizeof(contentLength);

                    if (WinHttpQueryHeaders(httpctx->hRequest,
                                            WINHTTP_QUERY_CUSTOM | WINHTTP_QUERY_FLAG_NUMBER,
                                            L"Original-Content-Length",
                                            &contentLength,
                                            &contentLengthSize,
                                            WINHTTP_NO_HEADER_INDEX))
                    {
                        req->setcontentlength(contentLength);

                        // check for gzip content encoding
                        WCHAR contentEncoding[16];
                        DWORD contentEncodingSize = sizeof(contentEncoding);

                        httpctx->gzip = WinHttpQueryHeaders(httpctx->hRequest,
                                                WINHTTP_QUERY_CONTENT_ENCODING,
                                                WINHTTP_HEADER_NAME_BY_INDEX,
                                                &contentEncoding,
                                                &contentEncodingSize,
                                                WINHTTP_NO_HEADER_INDEX)
                                    && !wcscmp(contentEncoding,L"gzip");

                        if (httpctx->gzip)
                        {
                            httpctx->z.zalloc = Z_NULL;
                            httpctx->z.zfree = Z_NULL;
                            httpctx->z.opaque = Z_NULL;
                            httpctx->z.avail_in = 0;
                            httpctx->z.next_in = Z_NULL;

                            inflateInit2(&httpctx->z, MAX_WBITS+16);

                            req->in.resize(contentLength);
                            httpctx->z.avail_out = contentLength;
                            httpctx->z.next_out = (unsigned char*)req->in.data();
                        }
                    }
                }

                if (!WinHttpQueryDataAvailable(httpctx->hRequest, NULL))
                {
                    httpio->cancel(req);
                    httpio->httpevent();
                }
                else if (httpio->waiter && httpio->noinetds)
                {
                    httpio->inetstatus(true);
                }
            }
        }
        break;

        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            if (httpio->waiter && GetLastError() != ERROR_WINHTTP_TIMEOUT)
            {
                httpio->inetstatus(false);
            }

        // fall through
        case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
            httpio->cancel(req);
            httpio->httpevent();
            break;

        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
            if (httpctx->postpos < httpctx->postlen)
            {
                unsigned pos = httpctx->postpos;
                unsigned t = httpctx->postlen - pos;

                if (t > HTTP_POST_CHUNK_SIZE)
                {
                    t = HTTP_POST_CHUNK_SIZE;
                }

                httpctx->postpos += t;

                if (!WinHttpWriteData(httpctx->hRequest, (LPVOID)(httpctx->postdata + pos), t, NULL))
                {
                    req->httpio->cancel(req);
                }

                httpio->httpevent();
            }
            else
            {
                if (!WinHttpReceiveResponse(httpctx->hRequest, NULL))
                {
                    httpio->cancel(req);
                    httpio->httpevent();
                }
            }
    }

    httpio->leavecs();
}

// POST request to URL
void WinHttpIO::post(HttpReq* req, const char* data, unsigned len)
{
    if (debug)
    {
        cout << "POST target URL: " << req->posturl << endl;

        if (req->binary)
        {
            cout << "[sending " << req->out->size() << " bytes of raw data]" << endl;
        }
        else
        {
            cout << "Sending: " << *req->out << endl;
        }
    }

    WinHttpContext* httpctx;

    WCHAR szURL[8192];
    WCHAR szHost[256];
    URL_COMPONENTS urlComp = { sizeof urlComp };

    urlComp.lpszHostName = szHost;
    urlComp.dwHostNameLength = sizeof szHost / sizeof *szHost;
    urlComp.dwUrlPathLength = (DWORD)-1;
    urlComp.dwSchemeLength = (DWORD)-1;

    httpctx = new WinHttpContext;

    httpctx->httpio = this;
    httpctx->req = req;
    req->httpiohandle = (void*)httpctx;

    if (MultiByteToWideChar(CP_UTF8, 0, req->posturl.c_str(), -1, szURL,
                            sizeof szURL / sizeof *szURL)
            && WinHttpCrackUrl(szURL, 0, 0, &urlComp))
    {
        if ((httpctx->hConnect = WinHttpConnect(hSession, szHost, urlComp.nPort, 0)))
        {
            httpctx->hRequest = WinHttpOpenRequest(httpctx->hConnect, L"POST",
                                                   urlComp.lpszUrlPath, NULL,
                                                   WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   (urlComp.nScheme == INTERNET_SCHEME_HTTPS)
                                                   ? WINHTTP_FLAG_SECURE
                                                   : 0);

            if (httpctx->hRequest)
            {
                WinHttpSetTimeouts(httpctx->hRequest, 0, 20000, 20000, 1800000);

                WinHttpSetStatusCallback(httpctx->hRequest, asynccallback,
                                         WINHTTP_CALLBACK_FLAG_DATA_AVAILABLE
                                         | WINHTTP_CALLBACK_FLAG_READ_COMPLETE
                                         | WINHTTP_CALLBACK_FLAG_HEADERS_AVAILABLE
                                         | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR
                                         | WINHTTP_CALLBACK_FLAG_SECURE_FAILURE
                                         | WINHTTP_CALLBACK_FLAG_SENDREQUEST_COMPLETE
                                         | WINHTTP_CALLBACK_FLAG_WRITE_COMPLETE
                                         | WINHTTP_CALLBACK_FLAG_HANDLES,
                                         0);

                LPCWSTR pwszHeaders = (req->type == REQ_JSON || !req->buf)
                                      ? L"Content-Type: application/json\r\nAccept-Encoding: gzip"
                                      : L"Content-Type: application/octet-stream";

                // data is sent in HTTP_POST_CHUNK_SIZE instalments to ensure
                // semi-smooth UI progress info
                httpctx->postlen = data ? len : req->out->size();
                httpctx->postdata = data ? data : req->out->data();
                httpctx->postpos = (httpctx->postlen < HTTP_POST_CHUNK_SIZE)
                                   ? httpctx->postlen
                                   : HTTP_POST_CHUNK_SIZE;

                if (WinHttpSendRequest(httpctx->hRequest, pwszHeaders,
                                       wcslen(pwszHeaders),
                                       (LPVOID)httpctx->postdata,
                                       httpctx->postpos,
                                       httpctx->postlen,
                                       (DWORD_PTR)httpctx))
                {
                    req->status = REQ_INFLIGHT;
                    return;
                }
            }
        }
        else
        {
            httpctx->hRequest = NULL;
        }
    }
    else
    {
        httpctx->hRequest = NULL;
        httpctx->hConnect = NULL;
    }

    req->status = REQ_FAILURE;
}

// cancel pending HTTP request
void WinHttpIO::cancel(HttpReq* req)
{
    WinHttpContext* httpctx;

    if ((httpctx = (WinHttpContext*)req->httpiohandle))
    {
        httpctx->req = NULL;

        req->httpstatus = 0;
        req->status = REQ_FAILURE;
        req->httpiohandle = NULL;

        if (httpctx->hConnect)
        {
            WinHttpCloseHandle(httpctx->hConnect);
        }

        if (httpctx->hRequest)
        {
            WinHttpCloseHandle(httpctx->hRequest);
        }
    }
}

// supply progress information on POST data
m_off_t WinHttpIO::postpos(void* handle)
{
    return ((WinHttpContext*)handle)->postpos;
}

// process events
bool WinHttpIO::doio()
{
    return false;
}
} // namespace
