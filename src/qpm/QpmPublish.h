#pragma once
// `qpm pack` / `qpm publish`: collects a project's publishable files (npm
// rules: the "files" whitelist, else everything not matched by
// .qpmignore/.npmignore/.gitignore), writes them into a "package/"-rooted
// .tgz, and either saves it or uploads it to the registry's /publish endpoint.

#include <string>

namespace qpm
{

    struct PublishOptions
    {
        std::string projectDir; // directory containing package.json
        bool dryRun = false;    // pack and report, but don't upload
        std::string token;      // overrides the saved/QPM_TOKEN token when non-empty
    };

    // Writes <name>-<version>.tgz into projectDir. Returns a process exit code.
    int runPack(const std::string &projectDir);

    // Returns a process exit code.
    int runPublish(const PublishOptions &opts);

} // namespace qpm
