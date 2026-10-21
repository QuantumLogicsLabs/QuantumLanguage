#pragma once
// Package authoring. `qpm init` writes a starter package.json. `qpm pack` /
// `qpm publish` collect a project's publishable files (npm rules: the "files"
// whitelist, else everything not matched by .qpmignore/.npmignore/.gitignore),
// write them into a "package/"-rooted .tgz, and either save it or upload it to
// the registry's /publish endpoint.

#include <string>

namespace qpm
{

    struct PublishOptions
    {
        std::string projectDir; // directory containing package.json
        bool dryRun = false;    // pack and report, but don't upload
        std::string token;      // overrides the saved/QPM_TOKEN token when non-empty
    };

    // Creates projectDir/package.json, asking for each field unless
    // `acceptDefaults` (`qpm init -y`). Returns a process exit code.
    int runInit(const std::string &projectDir, bool acceptDefaults);

    // Writes <name>-<version>.tgz into projectDir. Returns a process exit code.
    int runPack(const std::string &projectDir);

    // Returns a process exit code.
    int runPublish(const PublishOptions &opts);

} // namespace qpm
