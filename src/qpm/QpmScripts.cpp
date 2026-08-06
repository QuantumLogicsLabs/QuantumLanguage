#include "QpmScripts.h"
#include "QpmJson.h"

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
        std::string readFile(const fs::path &p)
        {
            std::ifstream f(p, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        }

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
        fs::path pkgJsonPath = projectDir / "package.json";
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
            std::wstring binDir = utf8ToWide((projectDir / "node_modules" / ".bin").string());
            wPath = binDir + L";" + current;
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
