#pragma once
// `qpm uninstall <pkg> [...]`: the inverse of QpmResolver's runInstall.
// Removes packages from package.json, deletes their node_modules folder,
// cleans up any .bin shims they registered, and updates qpm-lock.json.
//
// Note: like a bare `npm uninstall`, this only removes what you named —
// it does not walk the tree to prune transitive dependencies that are now
// orphaned. Shared root-level packages may still be relied on by other
// installed packages, so pruning them here could break those.

#include <string>
#include <vector>

namespace qpm
{

    struct UninstallOptions
    {
        std::string projectDir;          // directory containing package.json
        std::vector<std::string> packages; // package names to remove, from `qpm uninstall <pkg>...`
    };

    // Returns a process exit code (0 = success, nonzero on usage/parse errors).
    int runUninstall(const UninstallOptions &opts);

} // namespace qpm
