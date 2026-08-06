#include "QpmTar.h"
#include <filesystem>
#include <fstream>
#include <cstring>
#include <vector>

namespace fs = std::filesystem;

namespace qpm
{
    namespace
    {
        constexpr size_t BLOCK = 512;

        unsigned long long parseNumericField(const char *field, size_t len)
        {
            // GNU base-256 extension: high bit of the first byte set.
            if (len > 0 && (static_cast<unsigned char>(field[0]) & 0x80))
            {
                unsigned long long v = 0;
                for (size_t i = 0; i < len; ++i)
                    v = (v << 8) | (static_cast<unsigned char>(field[i]) & (i == 0 ? 0x7F : 0xFF));
                return v;
            }
            std::string s(field, len);
            // Trim trailing NUL/space and leading space.
            size_t end = s.find('\0');
            if (end != std::string::npos)
                s = s.substr(0, end);
            size_t start = 0;
            while (start < s.size() && s[start] == ' ')
                ++start;
            size_t stop = s.size();
            while (stop > start && (s[stop - 1] == ' ' || s[stop - 1] == '\0'))
                --stop;
            s = s.substr(start, stop - start);
            if (s.empty())
                return 0;
            try
            {
                return std::stoull(s, nullptr, 8);
            }
            catch (...)
            {
                return 0;
            }
        }

        std::string fieldToString(const char *field, size_t len)
        {
            size_t n = 0;
            while (n < len && field[n] != '\0')
                ++n;
            return std::string(field, n);
        }

        // Strips the leading "package/" (or first path component, defensively)
        // and rejects paths that would escape the extraction root.
        bool sanitizeRelPath(std::string path, std::string &out)
        {
            for (char &c : path)
                if (c == '\\')
                    c = '/';

            std::vector<std::string> parts;
            size_t i = 0;
            while (i < path.size())
            {
                size_t j = path.find('/', i);
                if (j == std::string::npos)
                    j = path.size();
                std::string comp = path.substr(i, j - i);
                if (!comp.empty() && comp != ".")
                    parts.push_back(comp);
                i = j + 1;
            }
            if (parts.empty())
                return false;

            // Drop the npm "package/" wrapper directory.
            parts.erase(parts.begin());
            if (parts.empty())
                return false;

            int depth = 0;
            for (const auto &p : parts)
            {
                if (p == "..")
                {
                    --depth;
                    if (depth < 0)
                        return false;
                }
                else
                {
                    ++depth;
                }
            }

            std::string joined;
            for (size_t k = 0; k < parts.size(); ++k)
            {
                if (k) joined += '/';
                joined += parts[k];
            }
            out = joined;
            return true;
        }

        // Parses PAX extended-header records ("<len> key=value\n") looking for
        // a "path" override. Other keys are ignored.
        void parsePaxOverride(const std::string &data, std::string &pathOverride)
        {
            size_t pos = 0;
            while (pos < data.size())
            {
                size_t sp = data.find(' ', pos);
                if (sp == std::string::npos)
                    break;
                std::string lenStr = data.substr(pos, sp - pos);
                long recLen = 0;
                try { recLen = std::stol(lenStr); } catch (...) { break; }
                if (recLen <= 0 || pos + static_cast<size_t>(recLen) > data.size())
                    break;
                std::string record = data.substr(pos, recLen);
                size_t eq = record.find('=');
                if (eq != std::string::npos)
                {
                    std::string key = record.substr(sp - pos + 1, eq - (sp - pos + 1));
                    std::string val = record.substr(eq + 1);
                    if (!val.empty() && val.back() == '\n')
                        val.pop_back();
                    if (key == "path")
                        pathOverride = val;
                }
                pos += recLen;
            }
        }
    } // namespace

    bool tarExtract(const std::string &tarBytes, const std::string &destDir, std::string &error)
    {
        std::error_code ec;
        fs::create_directories(destDir, ec);

        size_t pos = 0;
        std::string pendingLongName;
        bool havePendingLongName = false;

        while (pos + BLOCK <= tarBytes.size())
        {
            const char *header = tarBytes.data() + pos;

            bool allZero = true;
            for (size_t i = 0; i < BLOCK; ++i)
                if (header[i] != '\0') { allZero = false; break; }
            if (allZero)
                break; // end-of-archive marker

            std::string name = fieldToString(header + 0, 100);
            unsigned long long size = parseNumericField(header + 124, 12);
            char typeflag = header[156];
            std::string prefix = fieldToString(header + 345, 155);
            if (!prefix.empty())
                name = prefix + "/" + name;

            pos += BLOCK;
            size_t dataStart = pos;
            size_t paddedSize = ((size + BLOCK - 1) / BLOCK) * BLOCK;
            if (dataStart + size > tarBytes.size())
            {
                error = "truncated tar entry: " + name;
                return false;
            }

            if (havePendingLongName)
            {
                name = pendingLongName;
                havePendingLongName = false;
            }

            if (typeflag == 'L')
            {
                pendingLongName = std::string(tarBytes.data() + dataStart, static_cast<size_t>(size));
                if (!pendingLongName.empty() && pendingLongName.back() == '\0')
                    pendingLongName.pop_back();
                havePendingLongName = true;
                pos = dataStart + paddedSize;
                continue;
            }
            if (typeflag == 'x' || typeflag == 'g')
            {
                std::string paxData(tarBytes.data() + dataStart, static_cast<size_t>(size));
                std::string override_;
                parsePaxOverride(paxData, override_);
                if (!override_.empty())
                {
                    pendingLongName = override_;
                    havePendingLongName = true;
                }
                pos = dataStart + paddedSize;
                continue;
            }

            std::string rel;
            bool safe = sanitizeRelPath(name, rel);

            if (safe && (typeflag == '0' || typeflag == '\0'))
            {
                fs::path outPath = fs::path(destDir) / fs::u8path(rel);
                fs::create_directories(outPath.parent_path(), ec);
                std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
                if (out)
                {
                    out.write(tarBytes.data() + dataStart, static_cast<std::streamsize>(size));
                }
            }
            else if (safe && typeflag == '5')
            {
                fs::path outPath = fs::path(destDir) / fs::u8path(rel);
                fs::create_directories(outPath, ec);
            }
            // Symlinks ('2'), hardlinks ('1'), device/fifo entries, and unsafe
            // paths are intentionally skipped.

            pos = dataStart + paddedSize;
        }

        return true;
    }

} // namespace qpm
