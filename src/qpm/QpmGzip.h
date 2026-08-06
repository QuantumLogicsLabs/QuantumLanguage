#pragma once
// gzip/deflate decompression via zlib (statically linked) — turns a
// downloaded .tgz's bytes into a raw tar byte stream.

#include <string>

namespace qpm
{

    // Decompresses gzip- or zlib-wrapped `input` into `output`. Returns false
    // and fills `error` on failure.
    bool gzipInflate(const std::string &input, std::string &output, std::string &error);

} // namespace qpm
