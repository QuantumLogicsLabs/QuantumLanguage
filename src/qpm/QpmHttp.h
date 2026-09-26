#pragma once
// Minimal HTTPS client built on WinHTTP (ships with Windows — no external DLL
// dependency, unlike libcurl). Enough to GET JSON metadata, download tarball
// bytes, and POST a publish/login request to the registry.

#include <string>
#include <vector>

namespace qpm
{

    struct HttpResponse
    {
        int status = 0;
        std::string body;
        std::string error; // non-empty on a transport-level failure (DNS, TLS, timeout, ...)
        bool ok() const { return error.empty() && status >= 200 && status < 300; }
    };

    // Sends `method` to an http(s):// URL with `body` as the request payload
    // (may be empty) and each of `headers` as a raw "Name: value" line.
    // Redirects are followed automatically by WinHTTP.
    HttpResponse httpRequest(const std::string &method, const std::string &url,
                             const std::string &body = "",
                             const std::vector<std::string> &headers = {});

    // GET an https:// URL. `acceptHeader`, if non-empty, is sent as `Accept: <value>`.
    HttpResponse httpGet(const std::string &url, const std::string &acceptHeader = "");

    // Human-readable one-line reason for a failed response: the transport
    // error, the registry's JSON {"error": "..."} message, or "HTTP <status>".
    std::string describeFailure(const HttpResponse &resp);

    // Percent-encodes a single path component (e.g. turns "@scope/name" into
    // "@scope%2fname" for the npm registry's scoped-package URL convention).
    std::string urlEncodeComponent(const std::string &s);

} // namespace qpm
