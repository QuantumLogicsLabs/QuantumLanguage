#include "QpmUninstall.h"
#include "QpmJson.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

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
            return p.generic_string();
        }

        // Deletes the .bin/<cmd>.cmd shim(s) that writeBinShimsForPackage
        // (QpmResolver.cpp) would have written for this package.
        void removeBinShimsForPackage(const fs::path &nodeModules, const JsonValue &pkg,
                                       const std::string &pkgFolderName)
        {
            const JsonValue *bin = pkg.find("bin");
            if (!bin)
                return;

            fs::path binDir = nodeModules / ".bin";
            std::error_code ec;

            auto removeCmd = [&](const std::string &cmdName)
            {
                fs::remove(binDir / (cmdName + ".cmd"), ec);
            };

            if (bin->isString())
            {
                std::string pkgName = pkg.get("name").asString(pkgFolderName);
                auto slash = pkgName.find_last_of('/');
                if (slash != std::string::npos)
                    pkgName = pkgName.substr(slash + 1);
                removeCmd(pkgName);
            }
            else if (bin->isObject())
            {
                for (const auto &m : bin->object())
                    if (m.second.isString())
                        removeCmd(m.first);
            }
        }

        // Drops every member named `name` from the "dependencies" and
        // "devDependencies" maps of pkgJson. Returns true if anything moved.
        bool removeFromDependencyMaps(JsonValue &pkgJson, const std::string &name)
        {
            bool removed = false;
            for (const char *field : {"dependencies", "devDependencies"})
            {
                if (!pkgJson.has(field))
                    continue;
                JsonValue deps = pkgJson.get(field);
                if (!deps.isObject())
                    continue;
                auto &members = deps.object();
                size_t before = members.size();
                members.erase(
                    std::remove_if(members.begin(), members.end(),
                                    [&](const JsonValue::Member &m)
                                    { return m.first == name; }),
                    members.end());
                if (members.size() != before)
                {
                    removed = true;
                    pkgJson.set(field, deps);
                }
            }
            return removed;
        }

        // Removes the qpm-lock.json "packages" entry at `lockKeyPrefix` and
        // any entry nested beneath it (their on-disk subtree is removed
        // together with the parent, so their lock entries go together too).
        void removeFromLock(JsonValue &lockPackages, const std::string &lockKeyPrefix)
        {
            if (!lockPackages.isObject())
                return;
            auto &members = lockPackages.object();
            std::string nestedPrefix = lockKeyPrefix + "/";
            members.erase(
                std::remove_if(members.begin(), members.end(),
                                [&](const JsonValue::Member &m)
                                {
                                    return m.first == lockKeyPrefix ||
                                           m.first.compare(0, nestedPrefix.size(), nestedPrefix) == 0;
                                }),
                members.end());
        }
    } // namespace

    int runUninstall(const UninstallOptions &opts)
    {
        if (opts.packages.empty())
        {
            std::cerr << "[qpm] usage: qpm uninstall <pkg> [...]\n";
            return 1;
        }

        fs::path projectDir = fs::absolute(opts.projectDir);
        fs::path pkgJsonPath = projectDir / "package.json";
        fs::path nodeModules = projectDir / "node_modules";
        fs::path lockPath = projectDir / "qpm-lock.json";

        if (!fs::exists(pkgJsonPath))
        {
            std::cerr << "[qpm] no package.json found in " << projectDir.string() << "\n";
            return 1;
        }

        JsonValue pkgJson;
        try
        {
            pkgJson = JsonValue::parse(readFile(pkgJsonPath));
        }
        catch (const std::exception &e)
        {
            std::cerr << "[qpm] failed to parse package.json: " << e.what() << "\n";
            return 1;
        }

        JsonValue lockDoc = JsonValue::makeObject();
        JsonValue lockPackages = JsonValue::makeObject();
        if (fs::exists(lockPath))
        {
            try
            {
                lockDoc = JsonValue::parse(readFile(lockPath));
                JsonValue existing = lockDoc.get("packages");
                if (existing.isObject())
                    lockPackages = existing;
            }
            catch (...)
            {
                // Corrupt/unreadable lockfile: proceed and rewrite it fresh
                // from whatever we can determine below, same as runInstall
                // does when qpm-lock.json fails to parse.
            }
        }

        int removedCount = 0;
        int notFoundCount = 0;
        std::error_code ec;

        for (const auto &name : opts.packages)
        {
            fs::path destDir = nodeModules / name;
            bool onDisk = fs::exists(destDir);
            bool inPkgJson = removeFromDependencyMaps(pkgJson, name);

            if (!onDisk && !inPkgJson)
            {
                std::cerr << "[qpm] " << name << " is not installed\n";
                ++notFoundCount;
                continue;
            }

            if (onDisk)
            {
                JsonValue pkg;
                try
                {
                    pkg = JsonValue::parse(readFile(destDir / "package.json"));
                }
                catch (...)
                {
                }
                removeBinShimsForPackage(nodeModules, pkg, destDir.filename().string());
                fs::remove_all(destDir, ec);

                // Clean up a now-empty @scope directory, e.g. node_modules/@foo/.
                auto slash = name.find_last_of('/');
                if (slash != std::string::npos)
                {
                    fs::path scopeDir = nodeModules / name.substr(0, slash);
                    if (fs::exists(scopeDir) && fs::is_empty(scopeDir))
                        fs::remove(scopeDir, ec);
                }
            }

            std::string lockKey = toPosix(fs::path("node_modules") / name);
            removeFromLock(lockPackages, lockKey);

            std::cout << "  - " << name << "\n";
            ++removedCount;
        }

        writeFile(pkgJsonPath, pkgJson.stringify(2) + "\n");

        lockDoc.set("lockfileVersion", lockDoc.has("lockfileVersion") ? lockDoc.get("lockfileVersion") : JsonValue(1));
        lockDoc.set("packages", lockPackages);
        writeFile(lockPath, lockDoc.stringify(2) + "\n");

        std::cout << "\n[qpm] " << removedCount << " package(s) removed";
        if (notFoundCount > 0)
            std::cout << ", " << notFoundCount << " not found";
        std::cout << "\n";

        return notFoundCount > 0 && removedCount == 0 ? 1 : 0;
    }

} // namespace qpm
