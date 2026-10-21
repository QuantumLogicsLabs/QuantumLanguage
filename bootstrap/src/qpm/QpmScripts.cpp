#include "QpmScripts.h"
#include "QpmJson.h"
#include "QpmManifest.h"
#include "QpmPaths.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>

namespace fs = std::filesystem;

namespace qpm
{
    namespace
    {
        std::wstring utf8ToWide(const std::string &s)
        {
            if (s.empty())
                return std::wstring();
            int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(len, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], len);
            return w;
        }
    } // namespace

    int runScript(const std::string &projectDirStr, const std::string &scriptName)
    {
        fs::path projectDir = fs::absolute(projectDirStr);
        fs::path manifestPath = findManifest(projectDir);
        if (manifestPath.empty())
        {
            std::cerr << "[qpm] no package.qon, package.json or requirements.txt found in "
                      << projectDir.string() << "\n"
                      << "[qpm] run `qpm init` to create one\n";
            return 1;
        }

        JsonValue pkgJson;
        try
        {
            pkgJson = loadManifest(manifestPath).data;
        }
        catch (const std::exception &e)
        {
            std::cerr << "[qpm] failed to parse " << e.what() << "\n";
            return 1;
        }

        JsonValue scripts = pkgJson.get("scripts");
        std::string command;
        if (scripts.isObject() && scripts.has(scriptName))
        {
            command = scripts.get(scriptName).asString();
        }
        else if (scriptName == "start" && fs::exists(projectDir / "server.js"))
        {
            command = "node server.js"; // npm's own fallback when no "start" script is defined
        }
        else
        {
            std::cerr << "[qpm] missing script: \"" << scriptName << "\"\n";
            if (scripts.isObject() && !scripts.object().empty())
            {
                std::cerr << "[qpm] available scripts:\n";
                for (const auto &m : scripts.object())
                    std::cerr << "  " << m.first << "\n";
            }
            return 1;
        }

        std::cout << "\n> " << scriptName << "\n> " << command << "\n\n";
        std::cout.flush(); // the child inherits our console handle and writes to it directly

        std::wstring wPath;
        {
            DWORD needed = GetEnvironmentVariableW(L"PATH", nullptr, 0);
            std::wstring current(needed, L'\0');
            if (needed > 0)
            {
                GetEnvironmentVariableW(L"PATH", &current[0], needed);
                current.resize(needed - 1);
            }
            // Commands of installed packages: Quantum ones first, then npm ones.
            std::wstring qubitsBin = utf8ToWide((projectDir / kQubitsDir / ".bin").string());
            std::wstring npmBin = utf8ToWide((projectDir / kNodeModulesDir / ".bin").string());
            wPath = qubitsBin + L";" + npmBin + L";" + current;
        }
        SetEnvironmentVariableW(L"PATH", wPath.c_str());

        std::wstring cmdLine = L"cmd.exe /d /s /c \"" + utf8ToWide(command) + L"\"";
        std::wstring workDir = utf8ToWide(projectDir.string());

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        std::vector<wchar_t> mutableCmdLine(cmdLine.begin(), cmdLine.end());
        mutableCmdLine.push_back(L'\0');

        BOOL ok = CreateProcessW(nullptr, mutableCmdLine.data(), nullptr, nullptr, TRUE,
                                  0, nullptr, workDir.c_str(), &si, &pi);
        if (!ok)
        {
            std::cerr << "[qpm] failed to launch script (error " << GetLastError() << ")\n";
            return 1;
        }

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return static_cast<int>(exitCode);
    }

} // namespace qpm
