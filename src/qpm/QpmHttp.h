#pragma once
// Minimal HTTPS client built on WinHTTP (ships with Windows — no external DLL
// dependency, unlike libcurl). Enough to GET JSON metadata and download
// tarball bytes from registry.npmjs.org.

#include <string>

namespace qpm
{

    struct HttpResponse
    {
        int status = 0;
        std::string body;
        std::string error; // non-empty on a transport-level failure (DNS, TLS, timeout, ...)
        bool ok() const { return error.empty() && status >= 200 && status < 300; }
    };

    // GET an https:// URL. `acceptHeader`, if non-empty, is sent as `Accept: <value>`.
    // Redirects are followed automatically by WinHTTP.
    HttpResponse httpGet(const std::string &url, const std::string &acceptHeader = "");

    // Percent-encodes a single path component (e.g. turns "@scope/name" into
    // "@scope%2fname" for the npm registry's scoped-package URL convention).
    std::string urlEncodeComponent(const std::string &s);

} // namespace qpm
