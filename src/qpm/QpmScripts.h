#pragma once
// npm-scripts runner: `qpm run <script>` / `qpm start`. Spawns the command
// string from package.json's "scripts" as a shell command — qpm doesn't
// execute JavaScript itself, exactly like npm doesn't either.

#include <string>

namespace qpm
{

    // Runs scripts[scriptName] from <projectDir>/package.json with
    // node_modules/.bin prepended to PATH. Returns the child process's exit
    // code, or 1 with an error printed to stderr if the script can't be found.
    int runScript(const std::string &projectDir, const std::string &scriptName);

} // namespace qpm
