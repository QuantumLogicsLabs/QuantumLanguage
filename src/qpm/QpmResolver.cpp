#include "QpmResolver.h"
#include "QpmJson.h"
#include "QpmHttp.h"
#include "QpmSemver.h"
#include "QpmGzip.h"
#include "QpmTar.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <set>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>

namespace fs = std::filesystem;

namespace qpm
{
    namespace
    {
        std::string readFile(const fs::path &p)
        {
            std::ifstream f(p, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }

        void writeFile(const fs::path &p, const std::string &content)
        {
            std::error_code ec;
            fs::create_directories(p.parent_path(), ec);
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f << content;
        }

        std::string toPosix(fs::path p)
        {
            std::string s = p.generic_string();
            return s;
        }

        // Splits "name@range" / "@scope/name@range" / "name" into (name, range).
        void splitNameRange(const std::string &spec, std::string &name, std::string &range)
        {
            size_t searchFrom = (!spec.empty() && spec[0] == '@') ? 1 : 0;
            size_t at = spec.find('@', searchFrom);
            if (at == std::string::npos)
            {
                name = spec;
                range = "";
            }
            else
            {
                name = spec.substr(0, at);
                range = spec.substr(at + 1);
            }
        }

        // Writes a Windows .cmd shim that forwards to `node <script>`.
        void writeBinShim(const fs::path &binDir, const std::string &cmdName,
                           const std::string &pkgFolderName, const std::string &relScript)
        {
            std::error_code ec;
            fs::create_directories(binDir, ec);
            std::string rel = relScript;
            for (char &c : rel)
                if (c == '/')
                    c = '\\';
            std::string content = "@ECHO off\r\nnode \"%~dp0\\..\\" + pkgFolderName + "\\" + rel + "\" %*\r\n";
            writeFile(binDir / (cmdName + ".cmd"), content);
        }

        void writeBinShimsForPackage(const fs::path &destDir, const JsonValue &pkg)
        {
            const JsonValue *bin = pkg.find("bin");
            if (!bin)
                return;

            fs::path binDir = destDir.parent_path() / ".bin";
            std::string pkgFolderName = destDir.filename().string();
            std::string pkgName = pkg.get("name").asString(pkgFolderName);
            // Bare package name (strip scope) is used as the shim's default command name.
            std::string defaultCmd = pkgName;
            auto slash = defaultCmd.find_last_of('/');
            if (slash != std::string::npos)
                defaultCmd = defaultCmd.substr(slash + 1);

            if (bin->isString())
            {
                writeBinShim(binDir, defaultCmd, pkgFolderName, bin->asString());
            }
            else if (bin->isObject())
            {
                for (const auto &m : bin->object())
                    if (m.second.isString())
                        writeBinShim(binDir, m.first, pkgFolderName, m.second.asString());
            }
        }

        // A single-line terminal spinner that ticks on a background thread
        // while the calling thread blocks on a network call. stop() always
        // joins the thread before returning, so by the time the caller does
        // its own console output (e.g. an error to stderr), the spinner
        // thread is guaranteed dead — no concurrent-stdout hazard.
        class Spinner
        {
        public:
            explicit Spinner(std::string label) : label_(std::move(label))
            {
                running_.store(true);
                thread_ = std::thread([this]
                {
                    static const char frames[] = {'|', '/', '-', '\\'};
                    size_t i = 0;
                    while (running_.load(std::memory_order_relaxed))
                    {
                        std::cout << '\r' << "  " << frames[i % 4] << ' ' << label_ << std::flush;
                        ++i;
                        std::this_thread::sleep_for(std::chrono::milliseconds(90));
                    }
                });
            }

            // Stops the spinner and overwrites its line with `finalLine` (or
            // just clears it, cursor back at column 0, if `finalLine` is empty).
            void stop(const std::string &finalLine)
            {
                if (!running_.exchange(false))
                    return;
                if (thread_.joinable())
                    thread_.join();
                size_t width = label_.size() + 4;
                std::string padded = finalLine;
                if (padded.size() < width)
                    padded.append(width - padded.size(), ' ');
                std::cout << '\r' << padded << '\r';
                if (!finalLine.empty())
                    std::cout << finalLine << '\n';
                std::cout.flush();
            }

            ~Spinner() { stop(""); }

        private:
            std::string label_;
            std::atomic<bool> running_{false};
            std::thread thread_;
        };

        struct ResolveState
        {
            fs::path projectDir;
            fs::path rootNodeModules;
            std::map<std::string, std::string> rootChosen;      // name -> version
            std::map<std::string, JsonValue> metaCache;          // name -> registry doc
            std::map<std::string, std::string> tarballCache;     // "name@version" -> inflated tar bytes
            std::set<std::string> expandedDeps;                  // "name@version" already had children enqueued
            std::map<std::string, std::string> previousResolved; // lock key -> resolved URL, from a prior qpm-lock.json
            JsonValue lockPackages = JsonValue::makeObject();
            int installedCount = 0;
            int upToDateCount = 0;
            int failedCount = 0;
        };

        const JsonValue *fetchMeta(ResolveState &st, const std::string &name, std::string &error)
        {
            auto it = st.metaCache.find(name);
            if (it != st.metaCache.end())
                return &it->second;

            const char *regEnv = std::getenv("QPM_REGISTRY");
            std::string registryBase = (regEnv && *regEnv) ? std::string(regEnv) : "http://localhost:8000/api/registry/";
            if (registryBase.back() != '/')
            {
                registryBase += "/";
            }
            std::string url = registryBase + urlEncodeComponent(name);
            HttpResponse resp = httpGet(url, "application/vnd.npm.install-v1+json");
            if (!resp.ok())
            {
                error = "failed to fetch metadata for " + name + " (" +
                        (resp.error.empty() ? ("HTTP " + std::to_string(resp.status)) : resp.error) + ")";
                return nullptr;
            }
            try
            {
                JsonValue doc = JsonValue::parse(resp.body);
                auto [ins, ok] = st.metaCache.emplace(name, std::move(doc));
                return &ins->second;
            }
            catch (const std::exception &e)
            {
                error = "bad registry response for " + name + ": " + e.what();
                return nullptr;
            }
        }

        std::string resolveVersion(const JsonValue &meta, const std::string &rangeIn)
        {
            std::string range = rangeIn;
            const JsonValue *versionsObj = meta.find("versions");
            if (!versionsObj || !versionsObj->isObject())
                return "";

            if (range.empty() || range == "latest" || range == "*")
            {
                const JsonValue *tags = meta.find("dist-tags");
                if (tags)
                {
                    std::string latest = tags->get("latest").asString();
                    if (!latest.empty() && versionsObj->has(latest))
                        return latest;
                }
            }

            std::vector<std::string> versions;
            for (const auto &m : versionsObj->object())
                versions.push_back(m.first);
            return maxSatisfying(versions, range);
        }

        bool downloadAndExtract(ResolveState &st, const std::string &name, const std::string &version,
                                 const std::string &tarballUrl, const fs::path &destDir, std::string &error)
        {
            std::string key = name + "@" + version;
            auto cacheIt = st.tarballCache.find(key);
            std::string tarBytes;
            if (cacheIt != st.tarballCache.end())
            {
                tarBytes = cacheIt->second;
            }
            else
            {
                HttpResponse resp = httpGet(tarballUrl);
                if (!resp.ok())
                {
                    error = "failed to download " + key + " (" +
                            (resp.error.empty() ? ("HTTP " + std::to_string(resp.status)) : resp.error) + ")";
                    return false;
                }
                std::string inflated, gzErr;
                if (!gzipInflate(resp.body, inflated, gzErr))
                {
                    error = "failed to decompress " + key + ": " + gzErr;
                    return false;
                }
                st.tarballCache[key] = inflated;
                tarBytes = inflated;
            }

            std::error_code ec;
            fs::remove_all(destDir, ec);
            std::string tarErr;
            if (!tarExtract(tarBytes, destDir.string(), tarErr))
            {
                error = "failed to extract " + key + ": " + tarErr;
                return false;
            }

            JsonValue pkg;
            try
            {
                pkg = JsonValue::parse(readFile(destDir / "package.json"));
            }
            catch (...)
            {
            }
            writeBinShimsForPackage(destDir, pkg);
            return true;
        }

        // Returns true and fills `versionOut` if `destDir` already holds a
        // package.json whose version satisfies `range` — lets the caller skip
        // the network entirely for a package that's already correctly installed.
        bool trySatisfyFromDisk(const std::string &range, const fs::path &destDir, std::string &versionOut)
        {
            fs::path pkgJsonPath = destDir / "package.json";
            if (!fs::exists(pkgJsonPath))
                return false;
            JsonValue pkg;
            try
            {
                pkg = JsonValue::parse(readFile(pkgJsonPath));
            }
            catch (...)
            {
                return false;
            }
            std::string version = pkg.get("version").asString();
            SemVer sv;
            if (version.empty() || !SemVer::tryParse(version, sv))
                return false;
            if (!SemVerRange(range).satisfies(sv))
                return false;
            versionOut = version;
            return true;
        }

        void installTransitive(ResolveState &st, const std::string &name, const std::string &range,
                                const fs::path &parentPkgDir);

        // Places `name` satisfying `range` either by reusing an already-chosen
        // root version, claiming the root slot, or nesting under parentPkgDir.
        bool placeAndExpand(ResolveState &st, const std::string &name, const std::string &range,
                             const fs::path &parentPkgDir, bool isRootLevel)
        {
            auto rootIt = st.rootChosen.find(name);
            if (!isRootLevel && rootIt != st.rootChosen.end())
            {
                SemVer chosenVer;
                SemVerRange r(range);
                if (SemVer::tryParse(rootIt->second, chosenVer) && r.satisfies(chosenVer))
                    return true; // shared with the already-installed root copy
            }

            bool useRoot = isRootLevel || rootIt == st.rootChosen.end();
            fs::path destDir = useRoot ? (st.rootNodeModules / name) : (parentPkgDir / "node_modules" / name);
            std::string lockKey = toPosix(fs::relative(destDir, st.projectDir));

            auto recordLock = [&](const std::string &version, const std::string &resolvedUrl)
            {
                JsonValue entry = JsonValue::makeObject();
                entry.set("version", version);
                entry.set("resolved", resolvedUrl);
                st.lockPackages.set(lockKey, entry);
            };

            auto expandChildren = [&](const std::string &version, const JsonValue *depsObj)
            {
                std::string key = name + "@" + version;
                if (st.expandedDeps.count(key))
                    return;
                st.expandedDeps.insert(key);
                if (depsObj && depsObj->isObject())
                    for (const auto &m : depsObj->object())
                        installTransitive(st, m.first, m.second.asString(), destDir);
            };

            // Fast path: already correctly installed on disk from a previous
            // run — skip metadata fetch + download entirely.
            std::string diskVersion;
            if (trySatisfyFromDisk(range, destDir, diskVersion))
            {
                if (useRoot)
                    st.rootChosen[name] = diskVersion;
                ++st.upToDateCount;

                JsonValue diskPkg;
                try
                {
                    diskPkg = JsonValue::parse(readFile(destDir / "package.json"));
                }
                catch (...)
                {
                }
                auto prev = st.previousResolved.find(lockKey);
                recordLock(diskVersion, prev != st.previousResolved.end() ? prev->second : "");
                expandChildren(diskVersion, diskPkg.find("dependencies"));
                return true;
            }

            Spinner spinner("resolving " + name + (range.empty() ? "" : " " + range) + " ...");
            std::string opError;

            const JsonValue *meta = fetchMeta(st, name, opError);
            if (!meta)
            {
                spinner.stop("");
                std::cerr << "[qpm] " << opError << "\n";
                ++st.failedCount;
                return false;
            }
            std::string version = resolveVersion(*meta, range);
            if (version.empty())
            {
                spinner.stop("");
                std::cerr << "[qpm] no version of " << name << " satisfies \"" << range << "\"\n";
                ++st.failedCount;
                return false;
            }

            const JsonValue *verMeta = meta->find("versions");
            verMeta = verMeta ? verMeta->find(version) : nullptr;
            std::string tarballUrl = verMeta ? verMeta->get("dist").get("tarball").asString() : "";
            if (tarballUrl.empty())
            {
                spinner.stop("");
                std::cerr << "[qpm] no tarball URL for " << name << "@" << version << "\n";
                ++st.failedCount;
                return false;
            }

            if (!downloadAndExtract(st, name, version, tarballUrl, destDir, opError))
            {
                spinner.stop("");
                std::cerr << "[qpm] " << opError << "\n";
                ++st.failedCount;
                return false;
            }

            if (useRoot)
                st.rootChosen[name] = version;

            ++st.installedCount;
            std::string resultLine = "  + " + name + "@" + version +
                                      (useRoot ? "" : "  (nested under " + parentPkgDir.filename().string() + ")");
            spinner.stop(resultLine);

            recordLock(version, tarballUrl);
            const JsonValue *deps = verMeta ? verMeta->find("dependencies") : nullptr;
            expandChildren(version, deps);
            return true;
        }

        void installTransitive(ResolveState &st, const std::string &name, const std::string &range,
                                const fs::path &parentPkgDir)
        {
            placeAndExpand(st, name, range, parentPkgDir, /*isRootLevel=*/false);
        }

        bool installRootLevel(ResolveState &st, const std::string &name, const std::string &range)
        {
            return placeAndExpand(st, name, range, st.projectDir, /*isRootLevel=*/true);
        }
    } // namespace

    int runInstall(const InstallOptions &opts)
    {
        fs::path projectDir = fs::absolute(opts.projectDir);
        fs::path pkgJsonPath = projectDir / "package.json";
        bool existed = fs::exists(pkgJsonPath);

        JsonValue pkgJson;
        if (existed)
        {
            try
            {
                pkgJson = JsonValue::parse(readFile(pkgJsonPath));
            }
            catch (const std::exception &e)
            {
                std::cerr << "[qpm] failed to parse package.json: " << e.what() << "\n";
                return 1;
            }
        }
        else
        {
            if (opts.addPackages.empty())
            {
                std::cerr << "[qpm] no package.json found in " << projectDir.string() << "\n";
                return 1;
            }
            pkgJson = JsonValue::makeObject();
            pkgJson.set("name", projectDir.filename().string());
            pkgJson.set("version", "1.0.0");
            std::cout << "[qpm] no package.json found — creating a minimal one\n";
        }

        bool pkgJsonDirty = !existed;

        fs::path nodeModules = projectDir / "node_modules";
        fs::create_directories(nodeModules);

        ResolveState st;
        st.projectDir = projectDir;
        st.rootNodeModules = nodeModules;

        fs::path lockPath = projectDir / "qpm-lock.json";
        if (fs::exists(lockPath))
        {
            try
            {
                JsonValue oldLock = JsonValue::parse(readFile(lockPath));
                const JsonValue *packages = oldLock.find("packages");
                if (packages && packages->isObject())
                    for (const auto &m : packages->object())
                        st.previousResolved[m.first] = m.second.get("resolved").asString();
            }
            catch (...)
            {
            }
        }

        for (const auto &spec : opts.addPackages)
        {
            std::string name, range;
            splitNameRange(spec, name, range);

            std::string recordedRange = range;
            if (recordedRange.empty())
            {
                std::string ignoredError;
                const JsonValue *meta = fetchMeta(st, name, ignoredError);
                if (meta)
                {
                    std::string latest = resolveVersion(*meta, "latest");
                    if (!latest.empty())
                        recordedRange = "^" + latest;
                }
                if (recordedRange.empty())
                    recordedRange = "latest";
            }

            if (!pkgJson.has("dependencies"))
                pkgJson.set("dependencies", JsonValue::makeObject());
            JsonValue deps = pkgJson.get("dependencies");
            deps.set(name, recordedRange);
            pkgJson.set("dependencies", deps);
            pkgJsonDirty = true;
        }

        if (pkgJsonDirty)
            writeFile(pkgJsonPath, pkgJson.stringify(2) + "\n");

        JsonValue dependencies = pkgJson.get("dependencies");
        JsonValue devDependencies = pkgJson.get("devDependencies");

        std::cout << "[qpm] installing dependencies for " << pkgJson.get("name").asString(projectDir.filename().string()) << "\n";

        if (dependencies.isObject())
            for (const auto &m : dependencies.object())
                installRootLevel(st, m.first, m.second.asString());

        if (opts.includeDev && devDependencies.isObject())
            for (const auto &m : devDependencies.object())
                if (!st.rootChosen.count(m.first))
                    installRootLevel(st, m.first, m.second.asString());

        JsonValue lockDoc = JsonValue::makeObject();
        lockDoc.set("lockfileVersion", 1);
        lockDoc.set("packages", st.lockPackages);
        writeFile(lockPath, lockDoc.stringify(2) + "\n");

        std::cout << "\n[qpm] " << st.installedCount << " package(s) installed, "
                  << st.upToDateCount << " already up to date";
        if (st.failedCount > 0)
            std::cout << ", " << st.failedCount << " failed to resolve";
        std::cout << "\n";

        return st.failedCount > 0 ? 1 : 0;
    }

} // namespace qpm
