// qpm — the Quantum Package Manager.
//
// A standalone, from-scratch npm-compatible installer + script runner: it
// talks to registry.npmjs.org directly over HTTPS (via WinHTTP) and unpacks
// tarballs itself (via zlib + a small tar reader), so it needs neither npm
// nor Node.js installed to download a JS project's dependencies. Running the
// dependencies' own scripts (`qpm run` / `qpm start`) still shells out to
// whatever the script names — same as npm, qpm doesn't include a JS engine.

#include "QpmResolver.h"
#include "QpmScripts.h"
#include "QpmUninstall.h"

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
            "  qpm install                installs all dependencies from package.json\n"
            "  qpm install <pkg> [...]     adds and installs one or more packages\n"
            "  qpm install --no-dev        skip devDependencies\n"
            "  qpm uninstall <pkg> [...]   removes one or more packages\n"
            "  qpm run <script>            runs a package.json \"scripts\" entry\n"
            "  qpm start                   shorthand for `qpm run start`\n"
            "  qpm --help                  show this help\n"
            "  qpm --version               show version\n"
            "\n"
            "qpm downloads packages straight from the npm registry — no npm or\n"
            "Node.js install required. Running a script (`qpm run`/`qpm start`) still\n"
            "shells out to whatever that script names (often `node ...`), so a script\n"
            "that itself invokes Node.js still needs Node.js present to execute.\n";
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

    if (args[0] == "install" || args[0] == "i")
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

    if (args[0] == "uninstall" || args[0] == "un" || args[0] == "remove" || args[0] == "rm")
    {
        qpm::UninstallOptions opts;
        opts.projectDir = cwd;
        for (size_t i = 1; i < args.size(); ++i)
            opts.packages.push_back(args[i]);
        return qpm::runUninstall(opts);
    }

    if (args[0] == "run")
    {
        if (args.size() < 2)
        {
            std::cerr << "[qpm] usage: qpm run <script>\n";
            return 1;
        }
        return qpm::runScript(cwd, args[1]);
    }

    if (args[0] == "start")
    {
        return qpm::runScript(cwd, "start");
    }

    std::cerr << "[qpm] unknown command: " << args[0] << "\n";
    printHelp();
    return 1;
}
