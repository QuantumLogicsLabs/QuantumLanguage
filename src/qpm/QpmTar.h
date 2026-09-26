#pragma once
// Minimal USTAR/PAX/GNU-longname tar reader that extracts a byte stream to
// disk, plus a writer for `qpm pack`/`qpm publish`. npm tarballs always wrap
// their contents in a single "package/" root directory; that prefix is
// stripped during extraction and added during creation.

#include <string>
#include <vector>

namespace qpm
{

    // Extracts `tarBytes` (already gzip-decompressed) into `destDir`, which is
    // created if missing. Symlinks/hardlinks and any entry whose path would
    // escape destDir are skipped (defensive against malicious archives).
    // Returns false and fills `error` only on a structural read failure.
    bool tarExtract(const std::string &tarBytes, const std::string &destDir, std::string &error);

    struct TarEntry
    {
        std::string path; // '/'-separated, relative to the package root
        std::string data;
    };

    // Builds an uncompressed tar archive with every entry placed under
    // "package/". Paths too long for a USTAR header get a GNU longname entry.
    std::string tarCreate(const std::vector<TarEntry> &entries);

} // namespace qpm
