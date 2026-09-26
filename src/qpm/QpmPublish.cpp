#include "QpmPublish.h"
#include "QpmGzip.h"
#include "QpmHttp.h"
#include "QpmJson.h"
#include "QpmRegistry.h"
#include "QpmSemver.h"
#include "QpmTar.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace qpm
{
    namespace
    {
        bool readFile(const fs::path &p, std::string &out)
        {
            std::ifstream f(p, std::ios::binary);
            if (!f)
                return false;
            std::ostringstream ss;
            ss << f.rdbuf();
            out = ss.str();
            return true;
        }

        std::string toUpper(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        }

        // Never packed, at any depth: VCS dirs, installed deps, lockfiles,
        // credentials and ignore files (npm's built-in list, plus qpm's own files).
        const std::set<std::string> kAlwaysIgnored = {
            ".git", ".svn", ".hg", "CVS", "node_modules", ".DS_Store", "npm-debug.log",
            ".npmrc", ".qpmrc", ".npmignore", ".qpmignore", ".gitignore",
            "package-lock.json", "qpm-lock.json", "yarn.lock", "pnpm-lock.yaml"};

        // '*' and '?' stay within one path segment; '**' also crosses '/'.
        bool globMatch(const char *p, const char *s)
        {
            for (; *p; ++p, ++s)
            {
                if (*p == '*')
                {
                    bool deep = p[1] == '*';
                    while (*p == '*')
                        ++p;
                    if (deep && *p == '/')
                        ++p; // "**/x" also matches a top-level "x"
                    for (;; ++s)
                    {
                        if (globMatch(p, s))
                            return true;
                        if (!*s || (!deep && *s == '/'))
                            return false;
                    }
                }
                if (!*s || (*p == '?' ? *s == '/' : *p != *s))
                    return false;
            }
            return !*s;
        }

        // True if `pattern` matches `relPath` or any of its parent directories,
        // so naming a directory selects everything inside it.
        bool matchesPathOrParent(const std::string &pattern, const std::string &relPath)
        {
            std::string candidate = relPath;
            for (;;)
            {
                if (globMatch(pattern.c_str(), candidate.c_str()))
                    return true;
                size_t slash = candidate.find_last_of('/');
                if (slash == std::string::npos)
                    return false;
                candidate.resize(slash);
            }
        }

        // "./lib/" -> "lib": the form "files" entries and ignore patterns are matched in.
        std::string normalizeEntry(std::string e)
        {
            std::replace(e.begin(), e.end(), '\\', '/');
            while (e.rfind("./", 0) == 0)
                e.erase(0, 2);
            while (!e.empty() && e.front() == '/')
                e.erase(0, 1);
            while (!e.empty() && e.back() == '/')
                e.pop_back();
            return e;
        }

        struct IgnoreRule
        {
            std::string pattern;
            bool anchored; // had a '/' before its end, so it matches from the project root
        };

        // Like npm, the first ignore file found wins. Negated ("!") patterns
        // aren't supported and are skipped.
        std::vector<IgnoreRule> loadIgnoreRules(const fs::path &projectDir)
        {
            std::vector<IgnoreRule> rules;
            for (const char *file : {".qpmignore", ".npmignore", ".gitignore"})
            {
                std::ifstream f(projectDir / file);
                if (!f)
                    continue;
                std::string line;
                while (std::getline(f, line))
                {
                    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
                        line.pop_back();
                    if (line.empty() || line[0] == '#' || line[0] == '!')
                        continue;
                    std::string pattern = normalizeEntry(line);
                    if (!pattern.empty())
                        rules.push_back({pattern, pattern.find('/') != std::string::npos || line[0] == '/'});
                }
                break;
            }
            return rules;
        }

        bool isIgnored(const std::vector<IgnoreRule> &rules, const std::string &relPath)
        {
            for (const auto &r : rules)
            {
                if (r.anchored)
                {
                    if (matchesPathOrParent(r.pattern, relPath))
                        return true;
                    continue;
                }
                // Unanchored patterns match any single path segment.
                size_t start = 0;
                for (;;)
                {
                    size_t slash = relPath.find('/', start);
                    std::string segment = relPath.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                    if (globMatch(r.pattern.c_str(), segment.c_str()))
                        return true;
                    if (slash == std::string::npos)
                        break;
                    start = slash + 1;
                }
            }
            return false;
        }

        // Every regular file under projectDir as a sorted '/'-separated relative
        // path. Fails (rather than silently leaving a file out of the package)
        // on any entry it can't inspect, e.g. one past Windows' MAX_PATH.
        bool listFiles(const fs::path &projectDir, std::vector<std::string> &out)
        {
            std::error_code ec;
            fs::path current = projectDir;
            fs::recursive_directory_iterator it(projectDir, fs::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec))
            {
                current = it->path();
                if (kAlwaysIgnored.count(current.filename().u8string()))
                {
                    it.disable_recursion_pending();
                    continue;
                }
                bool regular = it->is_regular_file(ec);
                if (ec)
                    break;
                if (regular)
                    out.push_back(current.lexically_relative(projectDir).generic_u8string());
            }
            if (ec)
            {
                std::cerr << "[qpm] can't read " << current.u8string() << ": " << ec.message() << "\n";
                return false;
            }
            std::sort(out.begin(), out.end());
            return true;
        }

        bool isReadme(const std::string &rel)
        {
            return rel.find('/') == std::string::npos && toUpper(rel).rfind("README", 0) == 0;
        }

        // npm always ships package.json, the README, the LICENSE and the "main" file.
        bool isAlwaysIncluded(const std::string &rel, const std::string &mainFile)
        {
            if (rel == "package.json" || rel == mainFile || isReadme(rel))
                return true;
            if (rel.find('/') != std::string::npos)
                return false;
            std::string upper = toUpper(rel);
            return upper.rfind("LICENSE", 0) == 0 || upper.rfind("LICENCE", 0) == 0;
        }

        bool selectFiles(const fs::path &projectDir, const JsonValue &manifest,
                         const std::string &tarballName, std::vector<std::string> &selected)
        {
            std::vector<std::string> allFiles;
            if (!listFiles(projectDir, allFiles))
                return false;

            std::string mainFile = normalizeEntry(manifest.get("main").asString());
            const JsonValue *filesList = manifest.find("files");
            bool whitelist = filesList && filesList->isArray();

            // With a "files" whitelist the root ignore file doesn't apply (npm behavior).
            std::vector<std::string> patterns;
            std::vector<IgnoreRule> rules;
            if (whitelist)
            {
                for (const auto &e : filesList->array())
                {
                    std::string pattern = normalizeEntry(e.asString());
                    if (!pattern.empty())
                        patterns.push_back(pattern);
                }
            }
            else
            {
                rules = loadIgnoreRules(projectDir);
            }
            std::vector<bool> patternUsed(patterns.size(), false);

            for (const auto &rel : allFiles)
            {
                if (rel == tarballName)
                    continue; // left over from an earlier `qpm pack`

                bool keep = isAlwaysIncluded(rel, mainFile);
                if (whitelist)
                {
                    for (size_t i = 0; i < patterns.size(); ++i)
                    {
                        if (matchesPathOrParent(patterns[i], rel))
                        {
                            patternUsed[i] = true;
                            keep = true;
                        }
                    }
                }
                else if (!keep)
                {
                    keep = !isIgnored(rules, rel);
                }
                if (keep)
                    selected.push_back(rel);
            }

            for (size_t i = 0; i < patterns.size(); ++i)
                if (!patternUsed[i])
                    std::cerr << "[qpm] warning: \"files\" entry \"" << patterns[i] << "\" matched nothing\n";
            if (!mainFile.empty() && std::find(selected.begin(), selected.end(), mainFile) == selected.end())
                std::cerr << "[qpm] warning: \"main\" file \"" << mainFile << "\" does not exist\n";
            return true;
        }

        // Empty if `name` is a valid npm-style package name, else the reason it isn't.
        std::string nameProblem(const std::string &name)
        {
            if (name.empty() || name.size() > 214)
                return "must be 1 to 214 characters long";

            size_t scopeSlash = std::string::npos;
            size_t bareStart = 0;
            if (name[0] == '@')
            {
                scopeSlash = name.find('/');
                if (scopeSlash == std::string::npos || scopeSlash == 1 || scopeSlash + 1 == name.size())
                    return "scoped names must look like @scope/name";
                bareStart = scopeSlash + 1;
            }
            if (name[bareStart] == '.' || name[bareStart] == '_')
                return "must not start with '.' or '_'";

            for (size_t i = (name[0] == '@' ? 1 : 0); i < name.size(); ++i)
            {
                char c = name[i];
                if (i == scopeSlash)
                    continue;
                bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == '-' || c == '.' || c == '_' || c == '~';
                if (!ok)
                    return std::string("contains '") + c + "' (use lowercase letters, digits and - . _ ~)";
            }
            return "";
        }

        std::string formatSize(size_t bytes)
        {
            if (bytes < 1000)
                return std::to_string(bytes) + " B";
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(1);
            if (bytes < 1000 * 1000)
                ss << bytes / 1000.0 << " kB";
            else
                ss << bytes / 1000000.0 << " MB";
            return ss.str();
        }

        std::string base64Encode(const std::string &in)
        {
            static const char *table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve((in.size() + 2) / 3 * 4);
            size_t i = 0;
            for (; i + 2 < in.size(); i += 3)
            {
                unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                             (static_cast<unsigned char>(in[i + 1]) << 8) |
                             static_cast<unsigned char>(in[i + 2]);
                out += table[(v >> 18) & 63];
                out += table[(v >> 12) & 63];
                out += table[(v >> 6) & 63];
                out += table[v & 63];
            }
            size_t rem = in.size() - i;
            if (rem)
            {
                unsigned v = static_cast<unsigned char>(in[i]) << 16;
                if (rem == 2)
                    v |= static_cast<unsigned char>(in[i + 1]) << 8;
                out += table[(v >> 18) & 63];
                out += table[(v >> 12) & 63];
                out += rem == 2 ? table[(v >> 6) & 63] : '=';
                out += '=';
            }
            return out;
        }

        struct Package
        {
            JsonValue manifest;
            std::string name, version;
            std::string tarballName; // "<name>-<version>.tgz", scope folded in
            std::string tgz;
            std::string readme;
        };

        // Reads and validates package.json. Prints the problem and returns false on failure.
        bool readManifest(const fs::path &projectDir, Package &pkg)
        {
            std::string text;
            if (!readFile(projectDir / "package.json", text))
            {
                std::cerr << "[qpm] no package.json found in " << projectDir.string() << "\n";
                return false;
            }
            try
            {
                pkg.manifest = JsonValue::parse(text);
            }
            catch (const std::exception &e)
            {
                std::cerr << "[qpm] failed to parse package.json: " << e.what() << "\n";
                return false;
            }

            pkg.name = pkg.manifest.get("name").asString();
            pkg.version = pkg.manifest.get("version").asString();
            if (pkg.name.empty())
            {
                std::cerr << "[qpm] package.json needs a \"name\"\n";
                return false;
            }
            std::string problem = nameProblem(pkg.name);
            if (!problem.empty())
            {
                std::cerr << "[qpm] invalid package name \"" << pkg.name << "\": " << problem << "\n";
                return false;
            }
            SemVer sv;
            if (!SemVer::tryParse(pkg.version, sv))
            {
                std::cerr << "[qpm] package.json \"version\" must be a semver version like 1.0.0 (got \""
                          << pkg.version << "\")\n";
                return false;
            }

            pkg.tarballName = pkg.name[0] == '@' ? pkg.name.substr(1) : pkg.name;
            std::replace(pkg.tarballName.begin(), pkg.tarballName.end(), '/', '-');
            pkg.tarballName += "-" + pkg.version + ".tgz";
            return true;
        }

        // Collects the publishable files into pkg.tgz, listing each one.
        bool buildTarball(const fs::path &projectDir, Package &pkg)
        {
            std::cout << "[qpm] packing " << pkg.name << "@" << pkg.version << "\n";

            std::vector<std::string> files;
            if (!selectFiles(projectDir, pkg.manifest, pkg.tarballName, files))
                return false;

            std::vector<TarEntry> entries;
            size_t unpackedSize = 0;
            for (const auto &rel : files)
            {
                TarEntry entry{rel, ""};
                if (!readFile(projectDir / fs::u8path(rel), entry.data))
                {
                    std::cerr << "[qpm] can't read " << rel << "\n";
                    return false;
                }
                unpackedSize += entry.data.size();
                std::cout << "  " << std::setw(9) << formatSize(entry.data.size()) << "  " << rel << "\n";
                if (pkg.readme.empty() && isReadme(rel))
                    pkg.readme = entry.data;
                entries.push_back(std::move(entry));
            }

            std::string gzError;
            if (!gzipDeflate(tarCreate(entries), pkg.tgz, gzError))
            {
                std::cerr << "[qpm] failed to compress tarball: " << gzError << "\n";
                return false;
            }
            std::cout << "[qpm] " << entries.size() << " file(s), " << formatSize(unpackedSize)
                      << " unpacked, " << formatSize(pkg.tgz.size()) << " packed\n";
            return true;
        }
    } // namespace

    int runPack(const std::string &projectDirStr)
    {
        fs::path projectDir = fs::absolute(projectDirStr);
        Package pkg;
        if (!readManifest(projectDir, pkg) || !buildTarball(projectDir, pkg))
            return 1;

        std::ofstream out(projectDir / pkg.tarballName, std::ios::binary | std::ios::trunc);
        out.write(pkg.tgz.data(), static_cast<std::streamsize>(pkg.tgz.size()));
        if (!out)
        {
            std::cerr << "[qpm] failed to write " << pkg.tarballName << "\n";
            return 1;
        }
        std::cout << "[qpm] wrote " << pkg.tarballName << "\n";
        return 0;
    }

    int runPublish(const PublishOptions &opts)
    {
        fs::path projectDir = fs::absolute(opts.projectDir);
        Package pkg;
        if (!readManifest(projectDir, pkg))
            return 1;
        if (pkg.manifest.get("private").asBool())
        {
            std::cerr << "[qpm] " << pkg.name << " has \"private\": true in package.json, so it can't be published\n";
            return 1;
        }
        if (!buildTarball(projectDir, pkg))
            return 1;

        std::string registry = registryBaseUrl();
        std::string token = opts.token.empty() ? authToken() : opts.token;

        if (opts.dryRun)
        {
            std::cout << "[qpm] dry run: would publish " << pkg.name << "@" << pkg.version << " to " << registry
                      << (token.empty() ? " anonymously" : "") << "\n";
            return 0;
        }

        // The registry quietly treats a rejected token as anonymous, so check
        // it first rather than publishing an unowned package by accident.
        std::string publisher = "anonymous (run `qpm login` to own the package name)";
        if (!token.empty())
        {
            std::string error;
            if (!lookupUser(token, publisher, error))
            {
                std::cerr << "[qpm] your login for " << registry << " was rejected (" << error
                          << ") — run `qpm login` again\n";
                return 1;
            }
        }

        const JsonValue &m = pkg.manifest;
        JsonValue body = JsonValue::makeObject();
        body.set("name", pkg.name);
        body.set("version", pkg.version);
        body.set("description", m.get("description").asString());
        if (m.get("keywords").isArray() || m.get("keywords").isString())
            body.set("keywords", m.get("keywords"));
        if (m.get("license").isString())
            body.set("license", m.get("license"));
        JsonValue repo = m.get("repository");
        std::string repoUrl = repo.isObject() ? repo.get("url").asString() : repo.asString();
        if (!repoUrl.empty())
            body.set("repository", repoUrl);
        if (m.get("homepage").isString())
            body.set("homepage", m.get("homepage"));
        if (!pkg.readme.empty())
            body.set("readme", pkg.readme);
        if (m.get("dependencies").isObject())
            body.set("dependencies", m.get("dependencies"));
        body.set("fileBase64", base64Encode(pkg.tgz));

        std::vector<std::string> headers = {"Content-Type: application/json", "Accept: application/json"};
        if (!token.empty())
            headers.push_back("Authorization: Bearer " + token);

        std::cout << "[qpm] publishing to " << registry << " as " << publisher << "\n";
        HttpResponse resp = httpRequest("POST", registry + "publish", body.stringify(), headers);
        if (!resp.ok())
        {
            std::cerr << "[qpm] publish failed (" << describeFailure(resp) << ")\n";
            return 1;
        }
        std::cout << "+ " << pkg.name << "@" << pkg.version << "\n";
        return 0;
    }

} // namespace qpm
