#pragma once
// Dependency resolution + install orchestration: reads package.json, walks
// the npm registry to resolve a dependency tree, downloads + extracts
// tarballs into node_modules, writes .bin shims and qpm-lock.json.

#include <string>
#include <vector>

namespace qpm
{

    struct InstallOptions
    {
        std::string projectDir;             // directory containing package.json
        bool includeDev = true;             // also install devDependencies
        std::vector<std::string> addPackages; // "name" or "name@range" from `qpm install <pkg>...`
    };

    // Returns a process exit code (0 = success, nonzero if anything failed to resolve/download).
    int runInstall(const InstallOptions &opts);

} // namespace qpm
