#pragma once
// Minimal USTAR/PAX/GNU-longname tar reader that extracts a byte stream to
// disk. npm tarballs always wrap their contents in a single "package/"
// root directory; that prefix is stripped during extraction.

#include <string>

namespace qpm
{

    // Extracts `tarBytes` (already gzip-decompressed) into `destDir`, which is
    // created if missing. Symlinks/hardlinks and any entry whose path would
    // escape destDir are skipped (defensive against malicious archives).
    // Returns false and fills `error` only on a structural read failure.
    bool tarExtract(const std::string &tarBytes, const std::string &destDir, std::string &error);

} // namespace qpm
