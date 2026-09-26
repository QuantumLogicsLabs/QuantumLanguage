// qpm — the Quantum Package Manager.
//
// A standalone, from-scratch npm-compatible installer, publisher + script
// runner: it talks to the (npm-compatible) QPM registry directly over HTTPS
// (via WinHTTP) and packs/unpacks tarballs itself (via zlib + a small tar
// reader/writer), so it needs neither npm nor Node.js installed to install or
// publish packages. Running the
// dependencies' own scripts (`qpm run` / `qpm start`) still shells out to
// whatever the script names — same as npm, qpm doesn't include a JS engine.

#include "QpmPublish.h"
#include "QpmRegistry.h"
#include "QpmResolver.h"
#include "QpmScripts.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    void printHelp()
    {
        std::cout <<
            "qpm - Quantum Package Manager\n"
            "\n"
            "Usage:\n"
            "  qpm install                 installs all dependencies from package.json\n"
            "  qpm install <pkg> [...]     adds and installs one or more packages\n"
            "  qpm install --no-dev        skip devDependencies\n"
            "  qpm i / qpm add             aliases for `qpm install`\n"
            "  qpm run <script>            runs a package.json \"scripts\" entry\n"
            "  qpm start                   shorthand for `qpm run start`\n"
            "  qpm publish                 packs this package and uploads it to the registry\n"
            "  qpm publish --dry-run       shows what would be published, uploads nothing\n"
            "  qpm publish --token <t>     publish with this token instead of the saved login\n"
            "  qpm pack                    writes <name>-<version>.tgz without publishing\n"
            "  qpm login                   logs in to the registry and saves the token\n"
            "  qpm logout                  forgets the saved token\n"
            "  qpm whoami                  shows which account you're logged in as\n"
            "  qpm --help                  show this help\n"
            "  qpm --version               show version\n"
            "\n"
            "Environment:\n"
            "  QPM_REGISTRY   registry API base (default http://localhost:8000/api/registry/)\n"
            "  QPM_TOKEN      auth token, overrides the one saved by `qpm login`\n"
            "\n"
            "qpm downloads packages straight from the registry — no npm or Node.js\n"
            "install required. Running a script (`qpm run`/`qpm start`) still shells\n"
            "out to whatever that script names (often `node ...`), so a script that\n"
            "itself invokes Node.js still needs Node.js present to execute.\n";
    }

    int runPublishCommand(const std::vector<std::string> &args, const std::string &cwd)
    {
        qpm::PublishOptions opts;
        opts.projectDir = cwd;
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (args[i] == "--dry-run")
            {
                opts.dryRun = true;
            }
            else if (args[i] == "--token" && i + 1 < args.size())
            {
                opts.token = args[++i];
            }
            else
            {
                std::cerr << "[qpm] unknown publish option: " << args[i] << "\n"
                          << "[qpm] usage: qpm publish [--dry-run] [--token <token>]\n";
                return 1;
            }
        }
        return qpm::runPublish(opts);
    }
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::vector<std::string> args(argv + 1, argv + argc);
    std::string cwd = fs::current_path().string();

    if (args.empty() || args[0] == "--help" || args[0] == "-h")
    {
        printHelp();
        return 0;
    }
    if (args[0] == "--version" || args[0] == "-v")
    {
        std::cout << "qpm 1.0.0\n";
        return 0;
    }

    const std::string &cmd = args[0];

    if (cmd == "install" || cmd == "i" || cmd == "add")
    {
        qpm::InstallOptions opts;
        opts.projectDir = cwd;
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (args[i] == "--no-dev")
                opts.includeDev = false;
            else
                opts.addPackages.push_back(args[i]);
        }
        return qpm::runInstall(opts);
    }

    if (cmd == "run" || cmd == "run-script")
    {
        if (args.size() < 2)
        {
            std::cerr << "[qpm] usage: qpm run <script>\n";
            return 1;
        }
        return qpm::runScript(cwd, args[1]);
    }

    if (cmd == "start")
        return qpm::runScript(cwd, "start");
    if (cmd == "publish")
        return runPublishCommand(args, cwd);
    if (cmd == "pack")
        return qpm::runPack(cwd);
    if (cmd == "login")
        return qpm::runLogin();
    if (cmd == "logout")
        return qpm::runLogout();
    if (cmd == "whoami")
        return qpm::runWhoami();

    std::cerr << "[qpm] unknown command: " << cmd << "\n";
    printHelp();
    return 1;
}
