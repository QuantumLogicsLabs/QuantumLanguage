#include "QpmGzip.h"
#include <zlib.h>
#include <vector>

namespace qpm
{

    bool gzipInflate(const std::string &input, std::string &output, std::string &error)
    {
        output.clear();

        z_stream zs{};
        // windowBits = 15 + 32 tells zlib to auto-detect gzip or zlib headers.
        if (inflateInit2(&zs, 15 + 32) != Z_OK)
        {
            error = "inflateInit2 failed";
            return false;
        }

        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
        zs.avail_in = static_cast<uInt>(input.size());

        const size_t chunkSize = 256 * 1024;
        std::vector<char> chunk(chunkSize);
        int ret = Z_OK;

        do
        {
            zs.next_out = reinterpret_cast<Bytef *>(chunk.data());
            zs.avail_out = static_cast<uInt>(chunk.size());

            ret = inflate(&zs, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR)
            {
                error = zs.msg ? zs.msg : "inflate failed";
                inflateEnd(&zs);
                return false;
            }

            size_t produced = chunk.size() - zs.avail_out;
            if (produced > 0)
                output.append(chunk.data(), produced);

            if (ret == Z_BUF_ERROR && zs.avail_in == 0)
                break;
        } while (ret != Z_STREAM_END);

        inflateEnd(&zs);

        if (ret != Z_STREAM_END)
        {
            error = "truncated or corrupt gzip stream";
            return false;
        }
        return true;
    }

} // namespace qpm
