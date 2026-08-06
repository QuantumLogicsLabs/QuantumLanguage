#include "QpmHttp.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <vector>
#include <cstdio>

namespace qpm
{
    namespace
    {
        std::wstring utf8ToWide(const std::string &s)
        {
            if (s.empty())
                return std::wstring();
            int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(len, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], len);
            return w;
        }

        struct HandleGuard
        {
            HINTERNET h;
            explicit HandleGuard(HINTERNET handle) : h(handle) {}
            ~HandleGuard() { if (h) WinHttpCloseHandle(h); }
        };
    } // namespace

    std::string urlEncodeComponent(const std::string &s)
    {
        static const char *hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : s)
        {
            bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || c == '@';
            if (safe)
            {
                out += static_cast<char>(c);
            }
            else if (c == '/')
            {
                out += "%2f";
            }
            else
            {
                out += '%';
                out += hex[(c >> 4) & 0xF];
                out += hex[c & 0xF];
            }
        }
        return out;
    }

    HttpResponse httpGet(const std::string &url, const std::string &acceptHeader)
    {
        HttpResponse result;

        std::wstring wurl = utf8ToWide(url);
        wchar_t scheme[16] = {0};
        wchar_t host[256] = {0};
        wchar_t path[2048] = {0};

        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        uc.lpszScheme = scheme;
        uc.dwSchemeLength = _countof(scheme);
        uc.lpszHostName = host;
        uc.dwHostNameLength = _countof(host);
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = _countof(path);
        uc.dwExtraInfoLength = 0; // merge query string into UrlPath

        if (!WinHttpCrackUrl(wurl.c_str(), 0, ICU_DECODE, &uc))
        {
            result.error = "invalid URL";
            return result;
        }

        bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
        INTERNET_PORT port = uc.nPort;

        HINTERNET hSession = WinHttpOpen(L"qpm/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession)
        {
            result.error = "WinHttpOpen failed";
            return result;
        }
        HandleGuard sessionGuard(hSession);

        WinHttpSetTimeouts(hSession, 15000, 15000, 30000, 30000);

        HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
        if (!hConnect)
        {
            result.error = "WinHttpConnect failed";
            return result;
        }
        HandleGuard connectGuard(hConnect);

        DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
                                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest)
        {
            result.error = "WinHttpOpenRequest failed";
            return result;
        }
        HandleGuard requestGuard(hRequest);

        if (!acceptHeader.empty())
        {
            std::wstring header = L"Accept: " + utf8ToWide(acceptHeader);
            WinHttpAddRequestHeaders(hRequest, header.c_str(), static_cast<DWORD>(-1),
                                      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }

        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        {
            result.error = "WinHttpSendRequest failed (network unreachable?)";
            return result;
        }

        if (!WinHttpReceiveResponse(hRequest, nullptr))
        {
            result.error = "WinHttpReceiveResponse failed";
            return result;
        }

        DWORD statusCode = 0, statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        result.status = static_cast<int>(statusCode);

        std::string body;
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &available))
            {
                result.error = "WinHttpQueryDataAvailable failed";
                return result;
            }
            if (available == 0)
                break;

            std::vector<char> buf(available);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, buf.data(), available, &read))
            {
                result.error = "WinHttpReadData failed";
                return result;
            }
            body.append(buf.data(), read);
        }

        result.body = std::move(body);
        return result;
    }

} // namespace qpm
