#include "QpmResolver.h"
#include "QpmColor.h"
#include "QpmJson.h"
#include "QpmManifest.h"
#include "QpmHttp.h"
#include "QpmPaths.h"
#include "QpmRegistry.h"
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

        // Writes a Windows .cmd shim that forwards to `qrun <script>` for a
        // Quantum (.sa) bin, or `node <script>` for anything else.
        void writeBinShim(const fs::path &binDir, const std::string &cmdName,
                           const std::string &pkgFolderName, const std::string &relScript)
        {
            std::error_code ec;
            fs::create_directories(binDir, ec);
            std::string rel = relScript;
            for (char &c : rel)
                if (c == '/')
                    c = '\\';
            bool isQuantum = rel.size() >= 3 && rel.compare(rel.size() - 3, 3, ".sa") == 0;
            std::string runner = isQuantum ? "qrun" : "node";
            std::string content = "@ECHO off\r\n" + runner + " \"%~dp0\\..\\" + pkgFolderName + "\\" + rel + "\" %*\r\n";
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

        // This machine as Node's process.platform / process.arch name it — the
        // values npm packages list in their "os" / "cpu" fields.
        std::string hostOs()
        {
#if defined(_WIN32)
            return "win32";
#elif defined(__APPLE__)
            return "darwin";
#elif defined(__linux__)
            return "linux";
#else
            return "";
#endif
        }

        std::string hostCpu()
        {
#if defined(_M_ARM64) || defined(__aarch64__)
            return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
            return "x64";
#elif defined(_M_IX86) || defined(__i386__)
            return "ia32";
#else
            return "";
#endif
        }

        // npm's check for an "os" / "cpu" field (npm-install-checks): the first
        // entry naming `current` decides ("!name" excludes it); otherwise only a
        // list made entirely of exclusions allows it. A missing field allows all.
        bool platformListAllows(const JsonValue *field, const std::string &current)
        {
            std::vector<std::string> list;
            if (field && field->isString())
                list.push_back(field->asString());
            else if (field && field->isArray())
                for (const auto &e : field->array())
                    list.push_back(e.asString());

            if (list.size() == 1 && list[0] == "any")
                return true;
            size_t negated = 0;
            for (const auto &entry : list)
            {
                bool negate = !entry.empty() && entry[0] == '!';
                if ((negate ? entry.substr(1) : entry) == current)
                    return !negate;
                if (negate)
                    ++negated;
            }
            return negated == list.size();
        }

        bool platformAllows(const JsonValue &versionMeta)
        {
            return platformListAllows(versionMeta.find("os"), hostOs()) &&
                   platformListAllows(versionMeta.find("cpu"), hostCpu());
        }

        // A single-line terminal spinner that ticks on a background thread
        // while the calling thread blocks on a network call. stop() always
        // joins the thread before returning, so by the time the caller does
        // its own console output (e.g. an error to stderr), the spinner
        // thread is guaranteed dead — no concurrent-stdout hazard. Through a
        // pipe or into a file it doesn't animate (that would only leave
        // frames behind) and just prints the final line.
        class Spinner
        {
        public:
            explicit Spinner(std::string label) : label_(std::move(label))
            {
                if (!stdoutIsConsole())
                    return;
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
            // `finalLine` may carry color codes.
            void stop(const std::string &finalLine)
            {
                if (stopped_)
                    return;
                stopped_ = true;
                if (running_.exchange(false))
                {
                    if (thread_.joinable())
                        thread_.join();
                    std::cout << '\r' << std::string(label_.size() + 4, ' ') << '\r';
                }
                if (!finalLine.empty())
                    std::cout << finalLine << '\n';
                std::cout.flush();
            }

            ~Spinner() { stop(""); }

        private:
            std::string label_;
            std::atomic<bool> running_{false};
            bool stopped_ = false;
            std::thread thread_;
        };

        // A registry document, and whether a QPM registry served it (a Quantum
        // package, installed into quantum_qubits) or the npm upstream did (an npm
        // package, installed into node_modules).
        struct PackageMeta
        {
            JsonValue doc;
            bool quantum = false;
        };

        struct ResolveState
        {
            fs::path projectDir;
            std::map<std::string, std::string> rootChosen;      // name -> version
            std::map<std::string, PackageMeta> metaCache;        // name -> registry doc
            std::map<std::string, std::string> tarballCache;     // "name@version" -> inflated tar bytes
            std::set<std::string> expandedDeps;                  // "name@version" already had children enqueued
            std::map<std::string, JsonValue> previousLock;       // lock key -> entry, from a prior qpm-lock.json
            JsonValue lockPackages = JsonValue::makeObject();
            int installedCount = 0;
            int upToDateCount = 0;
            int failedCount = 0;
            int movedCount = 0;                                  // Quantum packages moved out of node_modules
            bool usedQubits = false;                             // anything is in a quantum_qubits folder
            std::set<std::string> platformSkipped;               // lock keys of optional deps built for another os/cpu
        };

        const PackageMeta *fetchMeta(ResolveState &st, const std::string &name, std::string &error)
        {
            auto it = st.metaCache.find(name);
            if (it != st.metaCache.end())
                return &it->second;

            const char *accept = "application/vnd.npm.install-v1+json";
            std::string registry = registryBaseUrl();
            HttpResponse resp = httpGet(registry + urlEncodeComponent(name), accept);
            // The QPM registry hosts Quantum packages; a package it has never
            // heard of (react, vite, ...) comes from the upstream npm registry.
            std::string upstream = upstreamRegistryUrl();
            if (resp.error.empty() && resp.status == 404 && !upstream.empty() && upstream != registry)
            {
                registry = upstream;
                resp = httpGet(registry + urlEncodeComponent(name), accept);
            }
            if (!resp.ok())
            {
                error = "failed to fetch metadata for " + name + " (" +
                        (resp.error.empty() ? ("HTTP " + std::to_string(resp.status)) : resp.error) + ")";
                return nullptr;
            }
            try
            {
                PackageMeta meta;
                meta.doc = JsonValue::parse(resp.body);
                meta.quantum = isQpmRegistry(registry);
                auto [ins, ok] = st.metaCache.emplace(name, std::move(meta));
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

        void installDependencies(ResolveState &st, const JsonValue &pkg, const fs::path &pkgDir);

        // Removes the copy of a Quantum package an older qpm left at
        // node_modules/<name>, the .bin shims that pointed into it, and the
        // node_modules folder itself if nothing else is in it. True if removed.
        bool removeOldInstall(const fs::path &oldDir)
        {
            std::error_code ec;
            if (fs::remove_all(oldDir, ec) == 0 || ec)
                return false;

            fs::path modulesDir = oldDir.parent_path();
            fs::path binDir = modulesDir / ".bin";
            std::string target = "\\..\\" + oldDir.filename().string() + "\\"; // see writeBinShim
            for (const auto &entry : fs::directory_iterator(binDir, ec))
                if (entry.path().extension() == ".cmd" && readFile(entry.path()).find(target) != std::string::npos)
                    fs::remove(entry.path(), ec);
            if (fs::is_directory(binDir, ec) && fs::is_empty(binDir, ec))
                fs::remove(binDir, ec);
            if (fs::is_empty(modulesDir, ec) && !ec)
                fs::remove(modulesDir, ec);
            return true;
        }

        // True if the last install recorded the package at `lockKey` as downloaded
        // from a QPM registry, whose tarball URLs live under .../api/registry/.
        bool lockedAsQuantum(const ResolveState &st, const std::string &lockKey)
        {
            auto prev = st.previousLock.find(lockKey);
            return prev != st.previousLock.end() &&
                   prev->second.get("resolved").asString().find("/api/registry/") != std::string::npos;
        }

        // Places `name` satisfying `range` either by reusing an already-chosen
        // root version, claiming the root slot, or nesting under parentPkgDir.
        // A Quantum package (from the QPM registry) goes into a quantum_qubits
        // folder, an npm package into node_modules, where Node.js looks for it.
        // An `optional` dependency that is built for another os/cpu is skipped,
        // and one that fails to install is a warning instead of a failure (npm).
        bool placeAndExpand(ResolveState &st, const std::string &name, const std::string &range,
                             const fs::path &parentPkgDir, bool isRootLevel, bool optional)
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
            fs::path base = useRoot ? st.projectDir : parentPkgDir;
            fs::path qubitsDir = base / kQubitsDir / name;
            fs::path npmDir = base / kNodeModulesDir / name;
            auto keyOf = [&](const fs::path &dir) { return toPosix(fs::relative(dir, st.projectDir)); };
            const std::string npmKey = keyOf(npmDir);
            fs::path destDir;    // which of the two, once known
            std::string lockKey; // destDir relative to the project

            auto recordLock = [&](const std::string &version, const std::string &resolvedUrl)
            {
                JsonValue entry = JsonValue::makeObject();
                entry.set("version", version);
                entry.set("resolved", resolvedUrl);
                st.lockPackages.set(lockKey, entry);
            };

            auto expandChildren = [&](const std::string &version, const JsonValue *pkgMeta)
            {
                std::string key = name + "@" + version;
                if (st.expandedDeps.count(key))
                    return;
                st.expandedDeps.insert(key);
                if (pkgMeta)
                    installDependencies(st, *pkgMeta, destDir);
            };

            // Fast path: already correctly installed on disk from a previous
            // run — skip metadata fetch + download entirely. Before quantum_qubits
            // existed, qpm put Quantum packages in node_modules too; one the
            // lockfile says came from the QPM registry is reinstalled below,
            // which moves it.
            std::string diskVersion;
            if (trySatisfyFromDisk(range, qubitsDir, diskVersion))
                destDir = qubitsDir;
            else if (!lockedAsQuantum(st, npmKey) && trySatisfyFromDisk(range, npmDir, diskVersion))
                destDir = npmDir;
            if (!destDir.empty())
            {
                lockKey = keyOf(destDir);
                if (useRoot)
                    st.rootChosen[name] = diskVersion;
                ++st.upToDateCount;
                st.usedQubits = st.usedQubits || destDir == qubitsDir;

                JsonValue diskPkg;
                try
                {
                    diskPkg = JsonValue::parse(readFile(destDir / "package.json"));
                }
                catch (...)
                {
                }
                auto prev = st.previousLock.find(lockKey);
                recordLock(diskVersion, prev != st.previousLock.end() ? prev->second.get("resolved").asString() : "");
                expandChildren(diskVersion, &diskPkg);
                return true;
            }

            // An optional dependency the last install already found to be for
            // another os/cpu (always an npm package): skip it without asking the
            // registry again.
            if (optional)
            {
                auto prev = st.previousLock.find(npmKey);
                SemVer prevVersion;
                if (prev != st.previousLock.end() && prev->second.get("optional").asBool() &&
                    !platformAllows(prev->second) &&
                    SemVer::tryParse(prev->second.get("version").asString(), prevVersion) &&
                    SemVerRange(range).satisfies(prevVersion))
                {
                    st.lockPackages.set(npmKey, prev->second);
                    st.platformSkipped.insert(npmKey);
                    return false;
                }
            }

            Spinner spinner("resolving " + name + (range.empty() ? "" : " " + range) + " ...");
            std::string opError;

            auto fail = [&](const std::string &message)
            {
                spinner.stop("");
                if (optional)
                {
                    std::cerr << "[qpm] skipping optional dependency: " << message << "\n";
                    return false;
                }
                std::cerr << "[qpm] " << message << "\n";
                ++st.failedCount;
                return false;
            };

            const PackageMeta *meta = fetchMeta(st, name, opError);
            if (!meta)
                return fail(opError);
            destDir = meta->quantum ? qubitsDir : npmDir;
            lockKey = keyOf(destDir);
            std::string version = resolveVersion(meta->doc, range);
            if (version.empty())
                return fail("no version of " + name + " satisfies \"" + range + "\"");

            const JsonValue *verMeta = meta->doc.find("versions");
            verMeta = verMeta ? verMeta->find(version) : nullptr;
            if (optional && verMeta && !platformAllows(*verMeta))
            {
                // e.g. esbuild lists a binary package for every os/cpu and
                // expects only the one matching this machine to be installed.
                // Locked with its os/cpu (as npm does) so the next install can
                // skip it offline.
                spinner.stop("");
                JsonValue entry = JsonValue::makeObject();
                entry.set("version", version);
                entry.set("resolved", verMeta->get("dist").get("tarball").asString());
                entry.set("optional", true);
                if (const JsonValue *os = verMeta->find("os"))
                    entry.set("os", *os);
                if (const JsonValue *cpu = verMeta->find("cpu"))
                    entry.set("cpu", *cpu);
                st.lockPackages.set(lockKey, entry);
                st.platformSkipped.insert(lockKey);
                return false;
            }

            std::string tarballUrl = verMeta ? verMeta->get("dist").get("tarball").asString() : "";
            if (tarballUrl.empty())
                return fail("no tarball URL for " + name + "@" + version);

            if (!downloadAndExtract(st, name, version, tarballUrl, destDir, opError))
                return fail(opError);

            // The copy an older qpm put in node_modules is replaced by this one.
            bool moved = meta->quantum && lockedAsQuantum(st, npmKey) && removeOldInstall(npmDir);
            st.movedCount += moved ? 1 : 0;

            if (useRoot)
                st.rootChosen[name] = version;

            ++st.installedCount;
            st.usedQubits = st.usedQubits || meta->quantum;
            std::string resultLine = "  " + paint("+", "1;32") + " " + paint(name, "1") + paint("@" + version, "90") +
                                      "  " + paint(lockKey, meta->quantum ? "38;5;208" : "36") +
                                      (moved ? paint("  (moved from node_modules)", "90") : "");
            spinner.stop(resultLine);

            recordLock(version, tarballUrl);
            expandChildren(version, verMeta);
            return true;
        }

        // Installs a package's "dependencies" and "optionalDependencies". As in
        // npm, an optionalDependencies entry overrides a same-named dependencies one.
        void installDependencies(ResolveState &st, const JsonValue &pkg, const fs::path &pkgDir)
        {
            const JsonValue *deps = pkg.find("dependencies");
            const JsonValue *optDeps = pkg.find("optionalDependencies");
            bool hasOptDeps = optDeps && optDeps->isObject();

            if (deps && deps->isObject())
                for (const auto &m : deps->object())
                    if (!(hasOptDeps && optDeps->has(m.first)))
                        placeAndExpand(st, m.first, m.second.asString(), pkgDir, /*isRootLevel=*/false, /*optional=*/false);
            if (hasOptDeps)
                for (const auto &m : optDeps->object())
                    placeAndExpand(st, m.first, m.second.asString(), pkgDir, /*isRootLevel=*/false, /*optional=*/true);
        }

        bool installRootLevel(ResolveState &st, const std::string &name, const std::string &range, bool optional)
        {
            return placeAndExpand(st, name, range, st.projectDir, /*isRootLevel=*/true, optional);
        }
    } // namespace

    int runInstall(const InstallOptions &opts)
    {
        fs::path projectDir = fs::absolute(opts.projectDir);
        fs::path manifestPath = findManifest(projectDir);
        bool existed = !manifestPath.empty();

        Manifest manifest;
        if (existed)
        {
            try
            {
                manifest = loadManifest(manifestPath);
            }
            catch (const std::exception &e)
            {
                std::cerr << "[qpm] failed to parse " << e.what() << "\n";
                return 1;
            }
        }
        else
        {
            if (opts.addPackages.empty())
            {
                std::cerr << "[qpm] no package.qon, package.json or requirements.txt found in "
                          << projectDir.string() << "\n"
                          << "[qpm] run `qpm init` to create one, or `qpm i <pkg>` to add a dependency\n";
                return 1;
            }
            manifest.path = projectDir / "package.json";
            manifest.data = JsonValue::makeObject();
            manifest.data.set("name", projectDir.filename().string());
            manifest.data.set("version", "1.0.0");
            std::cout << "[qpm] no manifest found — creating a minimal package.json\n";
        }

        bool pkgJsonDirty = !existed;

        ResolveState st;
        st.projectDir = projectDir;

        fs::path lockPath = projectDir / "qpm-lock.json";
        if (fs::exists(lockPath))
        {
            try
            {
                JsonValue oldLock = JsonValue::parse(readFile(lockPath));
                const JsonValue *packages = oldLock.find("packages");
                if (packages && packages->isObject())
                    for (const auto &m : packages->object())
                        st.previousLock[m.first] = m.second;
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
                const PackageMeta *meta = fetchMeta(st, name, ignoredError);
                if (meta)
                {
                    std::string latest = resolveVersion(meta->doc, "latest");
                    if (!latest.empty())
                        recordedRange = "^" + latest;
                }
                if (recordedRange.empty())
                    recordedRange = "latest";
            }

            setDependency(manifest, name, recordedRange);
            pkgJsonDirty = true;
        }

        if (pkgJsonDirty)
            writeFile(manifest.path, serializeManifest(manifest));

        const JsonValue &pkgJson = manifest.data;

        JsonValue dependencies = pkgJson.get("dependencies");
        JsonValue optionalDependencies = pkgJson.get("optionalDependencies");
        JsonValue devDependencies = pkgJson.get("devDependencies");

        std::cout << "[qpm] installing dependencies for " << pkgJson.get("name").asString(projectDir.filename().string()) << "\n";

        if (dependencies.isObject())
            for (const auto &m : dependencies.object())
                if (!optionalDependencies.has(m.first))
                    installRootLevel(st, m.first, m.second.asString(), /*optional=*/false);

        if (optionalDependencies.isObject())
            for (const auto &m : optionalDependencies.object())
                installRootLevel(st, m.first, m.second.asString(), /*optional=*/true);

        if (opts.includeDev && devDependencies.isObject())
            for (const auto &m : devDependencies.object())
                if (!st.rootChosen.count(m.first))
                    installRootLevel(st, m.first, m.second.asString(), /*optional=*/false);

        JsonValue lockDoc = JsonValue::makeObject();
        lockDoc.set("lockfileVersion", 1);
        lockDoc.set("packages", st.lockPackages);
        writeFile(lockPath, lockDoc.stringify(2) + "\n");

        std::cout << "\n[qpm] " << st.installedCount << " package(s) installed, "
                  << st.upToDateCount << " already up to date";
        if (st.movedCount > 0)
            std::cout << ", " << st.movedCount << " moved from node_modules to " << kQubitsDir;
        if (!st.platformSkipped.empty())
            std::cout << ", " << st.platformSkipped.size() << " optional skipped (other platforms)";
        if (st.failedCount > 0)
            std::cout << ", " << st.failedCount << " failed to resolve";
        std::cout << "\n";
        if (st.usedQubits)
            std::cout << "[qpm] Quantum packages are in " << paint((projectDir / kQubitsDir).string(), "38;5;208")
                      << "; `import <name>` or `# @use <name>` finds them\n";

        return st.failedCount > 0 ? 1 : 0;
    }

} // namespace qpm
