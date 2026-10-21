#include "QpmPublish.h"
#include "QpmGzip.h"
#include "QpmHttp.h"
#include "QpmJson.h"
#include "QpmManifest.h"
#include "QpmRegistry.h"
#include "QpmSemver.h"
#include "QpmTar.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
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
            ".git", ".svn", ".hg", "CVS", "node_modules", "quantum_qubits", ".DS_Store", "npm-debug.log",
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

        // The folder name as a valid package name ("My Lib" -> "my-lib").
        std::string defaultPackageName(const fs::path &projectDir)
        {
            std::string name;
            for (char c : projectDir.filename().u8string())
            {
                char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                bool ok = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') ||
                          lower == '-' || lower == '.' || lower == '_' || lower == '~';
                name += ok ? lower : '-';
            }
            while (!name.empty() && (name[0] == '.' || name[0] == '_'))
                name.erase(0, 1);
            return nameProblem(name).empty() ? name : "my-package";
        }

        // index.sa, else main.sa, else the first .sa file in the folder, else index.sa.
        std::string defaultEntryPoint(const fs::path &projectDir)
        {
            for (const char *candidate : {"index.sa", "main.sa"})
                if (fs::is_regular_file(projectDir / candidate))
                    return candidate;
            std::vector<std::string> saFiles;
            std::error_code ec;
            for (const auto &entry : fs::directory_iterator(projectDir, ec))
                if (entry.path().extension() == ".sa" && entry.is_regular_file(ec))
                    saFiles.push_back(entry.path().filename().u8string());
            std::sort(saFiles.begin(), saFiles.end());
            return saFiles.empty() ? "index.sa" : saFiles.front();
        }

        std::string trim(const std::string &s)
        {
            size_t start = 0, stop = s.size();
            while (start < stop && std::isspace(static_cast<unsigned char>(s[start])))
                ++start;
            while (stop > start && std::isspace(static_cast<unsigned char>(s[stop - 1])))
                --stop;
            return s.substr(start, stop - start);
        }

        // Prints "label: (default) " and returns the typed answer, or `def` on
        // an empty line or end of input.
        std::string ask(const std::string &label, const std::string &def)
        {
            std::cout << label << ": " << (def.empty() ? "" : "(" + def + ") ") << std::flush;
            std::string line;
            if (!std::getline(std::cin, line))
            {
                std::cout << "\n";
                return def;
            }
            line = trim(line);
            return line.empty() ? def : line;
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

        std::string toLower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string join(const std::vector<std::string> &parts, const std::string &sep)
        {
            std::string out;
            for (size_t i = 0; i < parts.size(); ++i)
                out += (i ? sep : "") + parts[i];
            return out;
        }

        // Keywords as typed at `qpm init`: separated by commas and/or spaces, like npm.
        std::vector<std::string> splitKeywords(const std::string &text)
        {
            std::vector<std::string> out;
            std::string word;
            for (size_t i = 0; i <= text.size(); ++i)
            {
                char c = i < text.size() ? text[i] : ' ';
                if (c == ',' || std::isspace(static_cast<unsigned char>(c)))
                {
                    if (!word.empty())
                        out.push_back(word);
                    word.clear();
                }
                else
                {
                    word += c;
                }
            }
            return out;
        }

        // A manifest's "keywords": an array, or (hand-written) one comma-separated string.
        std::vector<std::string> keywordList(const JsonValue &v)
        {
            std::vector<std::string> out;
            if (v.isArray())
            {
                for (const auto &e : v.array())
                    if (!trim(e.asString()).empty())
                        out.push_back(trim(e.asString()));
            }
            else if (v.isString())
            {
                std::istringstream in(v.asString());
                std::string part;
                while (std::getline(in, part, ','))
                    if (!trim(part).empty())
                        out.push_back(trim(part));
            }
            return out;
        }

        // "author" as npm writes it: a string, or {name, email, url} -> "name <email> (url)".
        std::string personString(const JsonValue &v)
        {
            if (!v.isObject())
                return v.asString();
            std::string out = v.get("name").asString();
            if (!v.get("email").asString().empty())
                out += " <" + v.get("email").asString() + ">";
            if (!v.get("url").asString().empty())
                out += " (" + v.get("url").asString() + ")";
            return out;
        }

        // "repository" as a string or as {type, url}.
        std::string repositoryUrl(const JsonValue &v)
        {
            return v.isObject() ? v.get("url").asString() : v.asString();
        }

        // The "origin" remote of the git repository in projectDir, or "".
        std::string gitOriginUrl(const fs::path &projectDir)
        {
            std::ifstream f(projectDir / ".git" / "config");
            std::string line;
            bool inOrigin = false;
            while (std::getline(f, line))
            {
                line = trim(line);
                if (!line.empty() && line[0] == '[')
                    inOrigin = line == "[remote \"origin\"]";
                else if (inOrigin && line.compare(0, 3, "url") == 0 && line.find('=') != std::string::npos)
                    return trim(line.substr(line.find('=') + 1));
            }
            return "";
        }

        // A repository URL as a browsable https:// page: "git+https://h/u/r.git",
        // "git@h:u/r.git", "ssh://git@h/u/r" and "github:u/r" all become "https://h/u/r".
        std::string repoWebUrl(std::string url)
        {
            url = trim(url);
            if (url.compare(0, 4, "git+") == 0)
                url.erase(0, 4);
            if (url.compare(0, 7, "github:") == 0)
                url = "https://github.com/" + url.substr(7);
            else if (url.compare(0, 4, "git@") == 0 && url.find(':') != std::string::npos)
                url = "https://" + url.substr(4, url.find(':') - 4) + "/" + url.substr(url.find(':') + 1);
            else if (url.compare(0, 10, "ssh://git@") == 0)
                url = "https://" + url.substr(10);
            else if (url.compare(0, 6, "git://") == 0)
                url = "https://" + url.substr(6);
            if (url.size() > 4 && url.compare(url.size() - 4, 4, ".git") == 0)
                url.resize(url.size() - 4);
            while (!url.empty() && url.back() == '/')
                url.pop_back();
            return url;
        }

        // "https://github.com/u/r" for a GitHub repository URL, else "".
        std::string githubWebUrl(const std::string &url)
        {
            std::string web = repoWebUrl(url);
            return web.compare(0, 19, "https://github.com/") == 0 && web.size() > 19 ? web : "";
        }

        // multipart/form-data, the way the website uploads packages.
        std::string makeBoundary()
        {
            std::random_device rd;
            std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^
                                static_cast<std::uint64_t>(std::time(nullptr)));
            std::string boundary = "----qpm-form-";
            for (int i = 0; i < 32; ++i)
                boundary += "0123456789abcdef"[gen() & 15];
            return boundary;
        }

        void addFormField(std::string &body, const std::string &boundary, const std::string &name,
                          const std::string &value)
        {
            body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" +
                    value + "\r\n";
        }

        void addFormFile(std::string &body, const std::string &boundary, const std::string &name,
                         const std::string &fileName, const std::string &data)
        {
            body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"; filename=\"" +
                    fileName + "\"\r\nContent-Type: application/gzip\r\n\r\n";
            body += data;
            body += "\r\n";
        }

        struct Package
        {
            JsonValue manifest;
            std::string manifestFile; // "package.qon", "package.json" or "requirements.txt"
            std::string name, version;
            std::string tarballName; // "<name>-<version>.tgz", scope folded in
            std::string tgz;
            std::string readme;
        };

        // Reads and validates the project manifest. Prints the problem and returns false on failure.
        bool readManifest(const fs::path &projectDir, Package &pkg)
        {
            fs::path manifestPath = findManifest(projectDir);
            if (manifestPath.empty())
            {
                std::cerr << "[qpm] no package.qon, package.json or requirements.txt found in "
                          << projectDir.string() << "\n"
                          << "[qpm] run `qpm init` to create one\n";
                return false;
            }
            try
            {
                pkg.manifest = loadManifest(manifestPath).data;
            }
            catch (const std::exception &e)
            {
                std::cerr << "[qpm] failed to parse " << e.what() << "\n";
                return false;
            }
            pkg.manifestFile = manifestPath.filename().string();

            pkg.name = pkg.manifest.get("name").asString();
            pkg.version = pkg.manifest.get("version").asString();
            if (pkg.name.empty())
            {
                std::cerr << "[qpm] " << pkg.manifestFile << " needs a \"name\""
                          << (pkg.manifestFile == "package.json" ? "" : " (`name = ...` under [package])") << "\n";
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
                std::cerr << "[qpm] " << pkg.manifestFile << " \"version\" must be a semver version like 1.0.0 (got \""
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
            // Registry consumers (npm, qpm) only read package.json, so a
            // package.qon / requirements.txt project ships a generated one.
            bool generatePkgJson = pkg.manifestFile != "package.json";
            if (generatePkgJson)
            {
                TarEntry entry{"package.json", pkg.manifest.stringify(2) + "\n"};
                unpackedSize += entry.data.size();
                std::cout << "  " << std::setw(9) << formatSize(entry.data.size()) << "  package.json (from "
                          << pkg.manifestFile << ")\n";
                entries.push_back(std::move(entry));
            }
            for (const auto &rel : files)
            {
                if (generatePkgJson && rel == "package.json")
                    continue;
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

        enum class Publishable
        {
            Yes,
            No,         // `problem` says why
            Unreachable // the registry couldn't be asked; `problem` says why
        };

        // Asks the registry whether it would take this version before anything
        // is uploaded: the version has to be new, and a package with an owner
        // only takes versions from that account. (The registry lets a guest
        // publish over an owned name, so that check has to happen here.)
        Publishable checkPublishable(const std::string &registry, const Package &pkg,
                                     const std::string &publisher, std::string &problem)
        {
            HttpResponse resp = httpGet(registry + urlEncodeComponent(pkg.name), "application/json");
            if (resp.error.empty() && resp.status == 404)
                return Publishable::Yes; // the first version of a new package
            if (!resp.ok())
            {
                problem = describeFailure(resp);
                return Publishable::Unreachable;
            }
            JsonValue doc;
            try
            {
                doc = JsonValue::parse(resp.body);
            }
            catch (const std::exception &e)
            {
                problem = std::string("unreadable answer from the registry: ") + e.what();
                return Publishable::Unreachable;
            }

            if (doc.get("versions").has(pkg.version))
            {
                problem = pkg.name + "@" + pkg.version + " is already published; versions can't be replaced, so bump "
                          "\"version\" in " + pkg.manifestFile + " and publish again";
                return Publishable::No;
            }
            std::string owner = doc.get("owner").asString();
            if (!owner.empty() && owner != "anonymous")
            {
                if (publisher.empty())
                {
                    problem = pkg.name + " belongs to " + owner + "; log in as " + owner +
                              " with `qpm login` to publish a new version";
                    return Publishable::No;
                }
                if (toLower(owner) != toLower(publisher))
                {
                    problem = pkg.name + " belongs to " + owner + ", but you're logged in as " + publisher;
                    return Publishable::No;
                }
            }

            std::string latest = doc.get("dist-tags").get("latest").asString();
            SemVer mine, theirs;
            if (SemVer::tryParse(pkg.version, mine) && SemVer::tryParse(latest, theirs) && compareSemVer(mine, theirs) < 0)
                std::cerr << "[qpm] warning: " << pkg.version << " is lower than the latest version (" << latest
                          << "), and the registry will make it the new latest\n";
            return Publishable::Yes;
        }
    } // namespace

    int runInit(const std::string &projectDirStr, bool acceptDefaults)
    {
        fs::path projectDir = fs::absolute(projectDirStr);
        fs::path pkgJsonPath = projectDir / "package.json";

        // package.qon and requirements.txt are read before package.json, so a
        // new package.json next to one of them would be ignored.
        fs::path existing = findManifest(projectDir);
        if (!existing.empty() && existing.filename() != "package.json")
        {
            std::cerr << "[qpm] " << existing.filename().string() << " already describes this project (qpm reads it "
                      << "before package.json), so edit that instead\n";
            return 1;
        }

        // Like npm, an existing package.json supplies the defaults, and every
        // field init doesn't ask about stays as it is.
        JsonValue pkg = JsonValue::makeObject();
        if (!existing.empty())
        {
            try
            {
                pkg = loadManifest(pkgJsonPath).data;
            }
            catch (const std::exception &e)
            {
                std::cerr << "[qpm] failed to parse " << e.what() << "\n";
                return 1;
            }
            if (!pkg.isObject())
                pkg = JsonValue::makeObject();
        }
        auto current = [&pkg](const char *key, const std::string &def)
        {
            std::string value = pkg.get(key).asString();
            return value.empty() ? def : value;
        };

        std::string name = current("name", defaultPackageName(projectDir));
        std::string version = current("version", "1.0.0");
        std::string description = current("description", "");
        std::string mainFile = current("main", defaultEntryPoint(projectDir));
        std::string testCommand = pkg.get("scripts").get("test").asString();
        std::string repository = repositoryUrl(pkg.get("repository"));
        if (repository.empty())
            repository = gitOriginUrl(projectDir);
        std::string keywords = join(keywordList(pkg.get("keywords")), ", ");
        const std::string originalAuthor = personString(pkg.get("author"));
        std::string author = originalAuthor;
        std::string license = current("license", "MIT");

        if (!acceptDefaults)
        {
            std::cout << "This utility walks you through creating a package.json for a Quantum package.\n"
                         "It only covers the most common fields. Press Enter to keep the value in ( ),\n"
                         "^C to quit; `qpm init -y` skips the questions.\n\n";
            const std::string suggestedName = name;
            for (;;)
            {
                name = ask("package name", suggestedName);
                std::string problem = nameProblem(name);
                if (problem.empty())
                    break;
                std::cout << "  invalid name: " << problem << "\n";
            }
            const std::string suggestedVersion = version;
            for (;;)
            {
                version = ask("version", suggestedVersion);
                SemVer sv;
                if (SemVer::tryParse(version, sv))
                    break;
                std::cout << "  must be a semver version like 1.0.0\n";
            }
            description = ask("description", description);
            mainFile = ask("entry point", mainFile);
            testCommand = ask("test command", testCommand);
            repository = ask("git repository", repository);
            keywords = ask("keywords", keywords);
            author = ask("author", author);
            license = ask("license", license);
        }

        pkg.set("name", name);
        pkg.set("version", version);
        pkg.set("description", description);
        pkg.set("main", mainFile);
        JsonValue scripts = pkg.get("scripts");
        if (!scripts.isObject())
            scripts = JsonValue::makeObject();
        if (!testCommand.empty())
            scripts.set("test", testCommand);
        if (!scripts.has("start"))
            scripts.set("start", "qrun " + mainFile);
        pkg.set("scripts", scripts);
        std::string github = githubWebUrl(repository);
        if (!repository.empty() && repository != repositoryUrl(pkg.get("repository")))
        {
            JsonValue repo = JsonValue::makeObject();
            repo.set("type", "git");
            repo.set("url", github.empty() ? repository : "git+" + github + ".git");
            pkg.set("repository", repo);
        }
        JsonValue keywordArray = JsonValue::makeArray();
        for (const auto &k : splitKeywords(keywords))
            keywordArray.push(k);
        pkg.set("keywords", keywordArray);
        if (author != originalAuthor || !pkg.has("author")) // an untouched {name, email} object stays an object
            pkg.set("author", author);
        pkg.set("license", license);
        if (!github.empty() && !pkg.has("bugs"))
        {
            JsonValue bugs = JsonValue::makeObject();
            bugs.set("url", github + "/issues");
            pkg.set("bugs", bugs);
        }
        if (!github.empty() && !pkg.has("homepage"))
            pkg.set("homepage", github + "#readme");

        std::string text = pkg.stringify(2) + "\n";
        if (!acceptDefaults)
        {
            std::cout << "\nAbout to write to " << pkgJsonPath.string() << ":\n\n"
                      << text << "\nIs this OK? (yes) " << std::flush;
            std::string answer;
            if (!std::getline(std::cin, answer))
                std::cout << "\n";
            answer = toLower(trim(answer));
            if (!answer.empty() && answer != "y" && answer != "yes")
            {
                std::cout << "Aborted.\n";
                return 0;
            }
        }

        std::ofstream out(pkgJsonPath, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out)
        {
            std::cerr << "[qpm] failed to write " << pkgJsonPath.string() << "\n";
            return 1;
        }
        if (acceptDefaults)
            std::cout << "Wrote to " << pkgJsonPath.string() << ":\n\n" << text << "\n";

        std::string page = packagePageUrl(registryBaseUrl(), name);
        std::cout << "[qpm] next: `qpm i <pkg>` adds a dependency; `qpm publish` puts the package on the registry"
                  << (page.empty() ? "" : " at " + page) << " (`qpm publish --dry-run` previews it)\n";
        return 0;
    }

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
            std::cerr << "[qpm] " << pkg.name << " has \"private\": true in " << pkg.manifestFile << ", so it can't be published\n";
            return 1;
        }
        std::string registry = registryBaseUrl();
        if (!isQpmRegistry(registry))
        {
            std::cerr << "[qpm] qpm publishes through the QPM registry's upload API, and " << registry
                      << " isn't a QPM registry (an .../api/registry/ URL)\n";
            return 1;
        }
        if (pkg.name[0] == '@')
        {
            std::cerr << "[qpm] the QPM registry doesn't take scoped names like " << pkg.name
                      << " yet; publish it under a plain name\n";
            return 1;
        }
        if (!buildTarball(projectDir, pkg))
            return 1;

        // The registry quietly treats a rejected token as anonymous, so check
        // it first rather than publishing an unowned package by accident.
        std::string token = opts.token.empty() ? authToken() : opts.token;
        std::string publisher;
        if (!token.empty())
        {
            std::string error;
            if (!lookupUser(token, publisher, error))
            {
                std::cerr << "[qpm] your login for " << registry << " was rejected (" << error
                          << "); run `qpm login` again\n";
                return 1;
            }
        }
        std::string as = publisher.empty() ? "anonymously (`qpm login` first to own the name)" : "as " + publisher;

        std::string problem;
        switch (checkPublishable(registry, pkg, publisher, problem))
        {
        case Publishable::Yes:
            break;
        case Publishable::No:
            std::cerr << "[qpm] can't publish: " << problem << "\n";
            return 1;
        case Publishable::Unreachable:
            if (!opts.dryRun)
            {
                std::cerr << "[qpm] can't reach " << registry << " (" << problem << ")\n";
                return 1;
            }
            std::cerr << "[qpm] warning: couldn't ask " << registry << " whether it would take this version ("
                      << problem << ")\n";
            break;
        }

        if (opts.dryRun)
        {
            std::cout << "[qpm] dry run: would publish " << pkg.name << "@" << pkg.version << " to " << registry
                      << " " << as << "\n";
            return 0;
        }

        // The same multipart form the website's publish page sends.
        const JsonValue &m = pkg.manifest;
        std::string boundary = makeBoundary();
        std::string body;
        addFormField(body, boundary, "name", pkg.name);
        addFormField(body, boundary, "version", pkg.version);
        if (!m.get("description").asString().empty())
            addFormField(body, boundary, "description", m.get("description").asString());
        std::vector<std::string> keywords = keywordList(m.get("keywords"));
        if (!keywords.empty())
            addFormField(body, boundary, "keywords", join(keywords, ","));
        if (!m.get("license").asString().empty())
            addFormField(body, boundary, "license", m.get("license").asString());
        std::string repoUrl = repoWebUrl(repositoryUrl(m.get("repository")));
        if (!repoUrl.empty())
            addFormField(body, boundary, "repository", repoUrl);
        if (!m.get("homepage").asString().empty())
            addFormField(body, boundary, "homepage", m.get("homepage").asString());
        if (!pkg.readme.empty())
            addFormField(body, boundary, "readme", pkg.readme);
        if (m.get("dependencies").isObject() && !m.get("dependencies").object().empty())
            addFormField(body, boundary, "dependencies", m.get("dependencies").stringify());
        addFormFile(body, boundary, "file", pkg.tarballName, pkg.tgz);
        body += "--" + boundary + "--\r\n";

        std::vector<std::string> headers = {"Content-Type: multipart/form-data; boundary=" + boundary,
                                            "Accept: application/json"};
        if (!token.empty())
            headers.push_back("Authorization: Bearer " + token);

        std::cout << "[qpm] publishing to " << registry << " " << as << "\n";
        HttpResponse resp = httpRequest("POST", registry + "publish", body, headers);
        if (!resp.ok())
        {
            if (resp.status == 413)
                std::cerr << "[qpm] publish failed: the registry turned the upload down as too large ("
                          << formatSize(body.size()) << ")\n"
                          << "[qpm] leave build outputs, binaries and big assets out with a \"files\" list or a .qpmignore\n";
            else
                std::cerr << "[qpm] publish failed (" << describeFailure(resp) << ")\n";
            return 1;
        }
        std::cout << "+ " << pkg.name << "@" << pkg.version << "\n";
        std::string page = packagePageUrl(registry, pkg.name);
        if (!page.empty())
            std::cout << "[qpm] " << page << "\n";
        return 0;
    }

} // namespace qpm
