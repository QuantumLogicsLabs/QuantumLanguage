#pragma once
// gzip/deflate via zlib (statically linked) — turns a downloaded .tgz's
// bytes into a raw tar byte stream, and a packed tar back into a .tgz.

#include <string>

namespace qpm
{

    // Decompresses gzip- or zlib-wrapped `input` into `output`. Returns false
    // and fills `error` on failure.
    bool gzipInflate(const std::string &input, std::string &output, std::string &error);

    // Compresses `input` into a gzip-wrapped `output`. Returns false and
    // fills `error` on failure.
    bool gzipDeflate(const std::string &input, std::string &output, std::string &error);

} // namespace qpm
