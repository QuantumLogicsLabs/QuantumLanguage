#include "QpmBuild.h"
#include "QpmJson.h"
#include "QpmManifest.h"
#include "QpmScripts.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace qpm
{
    namespace
    {
        // ─── Strings and files ───────────────────────────────────────────────

        std::wstring widen(const std::string &s)
        {
            if (s.empty())
                return std::wstring();
            int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
            std::wstring w(len, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], len);
            return w;
        }

        std::string narrow(const std::wstring &w)
        {
            if (w.empty())
                return std::string();
            int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], len, nullptr, nullptr);
            return s;
        }

        std::string u8(const fs::path &p) { return narrow(p.wstring()); }
        fs::path fromU8(const std::string &s) { return fs::path(widen(s)); }

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        std::string readFile(const fs::path &p)
        {
            std::ifstream in(p, std::ios::binary);
            if (!in)
                return std::string();
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        void writeFile(const fs::path &p, const std::string &text)
        {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out << text;
        }

        // `p` relative to `root` with forward slashes, for messages.
        std::string shown(const fs::path &p, const fs::path &root)
        {
            fs::path rel = p.lexically_relative(root);
            if (rel.empty() || *rel.begin() == "..")
                return u8(p.generic_wstring());
            return u8(rel.generic_wstring());
        }

        fs::file_time_type mtime(const fs::path &p)
        {
            std::error_code ec;
            auto t = fs::last_write_time(p, ec);
            return ec ? fs::file_time_type::min() : t;
        }

        // ─── Processes ───────────────────────────────────────────────────────

        // One argument quoted the way CommandLineToArgvW / the MSVC CRT read it.
        std::wstring quoteArg(const std::wstring &arg)
        {
            if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
                return arg;
            std::wstring out = L"\"";
            for (size_t i = 0;; ++i)
            {
                size_t backslashes = 0;
                while (i < arg.size() && arg[i] == L'\\')
                {
                    ++i;
                    ++backslashes;
                }
                if (i == arg.size())
                {
                    out.append(backslashes * 2, L'\\');
                    break;
                }
                if (arg[i] == L'"')
                    out.append(backslashes * 2 + 1, L'\\');
                else
                    out.append(backslashes, L'\\');
                out.push_back(arg[i]);
            }
            out.push_back(L'"');
            return out;
        }

        std::wstring commandLine(const std::vector<std::string> &argv)
        {
            std::wstring cmd;
            for (const auto &a : argv)
            {
                if (!cmd.empty())
                    cmd += L' ';
                cmd += quoteArg(widen(a));
            }
            return cmd;
        }

        std::string shownCommand(const std::vector<std::string> &argv)
        {
            return narrow(commandLine(argv));
        }

        // Starts argv[0]. With `log`, the child's stdout and stderr go to that
        // file (so parallel compiles don't interleave on the console);
        // otherwise it shares this console. Returns the process handle or null.
        HANDLE startProcess(const std::vector<std::string> &argv, const fs::path &cwd, const fs::path *log)
        {
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            HANDLE logHandle = INVALID_HANDLE_VALUE;
            if (log)
            {
                SECURITY_ATTRIBUTES sa{};
                sa.nLength = sizeof(sa);
                sa.bInheritHandle = TRUE;
                logHandle = CreateFileW(log->wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (logHandle == INVALID_HANDLE_VALUE)
                    return nullptr;
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                si.hStdOutput = logHandle;
                si.hStdError = logHandle;
            }
            std::wstring cmd = commandLine(argv);
            std::vector<wchar_t> buf(cmd.begin(), cmd.end());
            buf.push_back(L'\0');
            std::wstring dir = cwd.wstring();
            PROCESS_INFORMATION pi{};
            BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                     dir.empty() ? nullptr : dir.c_str(), &si, &pi);
            if (logHandle != INVALID_HANDLE_VALUE)
                CloseHandle(logHandle); // the child holds its own copy
            if (!ok)
                return nullptr;
            CloseHandle(pi.hThread);
            return pi.hProcess;
        }

        int waitProcess(HANDLE h)
        {
            WaitForSingleObject(h, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(h, &code);
            CloseHandle(h);
            return static_cast<int>(code);
        }

        // Runs a command on this console; -1 if it couldn't be started.
        int runProcess(const std::vector<std::string> &argv, const fs::path &cwd)
        {
            std::cout.flush();
            HANDLE h = startProcess(argv, cwd, nullptr);
            if (!h)
            {
                std::cerr << "[qpm] could not start " << argv[0] << " (error " << GetLastError() << ")\n";
                return -1;
            }
            return waitProcess(h);
        }

        // Runs a command and returns its exit code and combined output.
        std::pair<int, std::string> captureProcess(const std::vector<std::string> &argv, const fs::path &logPath)
        {
            HANDLE h = startProcess(argv, fs::path(), &logPath);
            if (!h)
                return {-1, std::string()};
            int code = waitProcess(h);
            std::string out = readFile(logPath);
            std::error_code ec;
            fs::remove(logPath, ec);
            return {code, out};
        }

        bool stderrIsConsole()
        {
            DWORD mode = 0;
            return GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &mode) != 0;
        }

        // ─── Toolchain ───────────────────────────────────────────────────────

        std::vector<fs::path> pathDirs()
        {
            std::vector<fs::path> dirs;
            DWORD n = GetEnvironmentVariableW(L"PATH", nullptr, 0);
            if (n == 0)
                return dirs;
            std::wstring path(n, L'\0');
            GetEnvironmentVariableW(L"PATH", &path[0], n);
            path.resize(n - 1);
            size_t start = 0;
            while (start <= path.size())
            {
                size_t end = path.find(L';', start);
                if (end == std::wstring::npos)
                    end = path.size();
                std::wstring d = path.substr(start, end - start);
                d.erase(std::remove(d.begin(), d.end(), L'"'), d.end());
                if (!d.empty())
                    dirs.emplace_back(d);
                start = end + 1;
            }
            return dirs;
        }

        fs::path findOnPath(const std::string &name)
        {
            std::wstring exe = widen(name);
            if (fs::path(exe).extension().empty())
                exe += L".exe";
            std::error_code ec;
            for (const auto &d : pathDirs())
                if (fs::is_regular_file(d / exe, ec))
                    return d / exe;
            return fs::path();
        }

        // Puts `dir` first on PATH (moving it if it's already further down).
        // The compiler's own folder must win: cc1plus loads its DLLs (libgmp,
        // zlib, ...) through PATH, and another MinGW earlier on PATH — Git
        // Bash's /mingw64/bin, say — makes it die without a message.
        void prependToPath(const fs::path &dir)
        {
            if (dir.empty())
                return;
            const std::string want = lower(u8(dir.lexically_normal()));
            std::wstring path = dir.wstring();
            for (const auto &d : pathDirs())
                if (lower(u8(d.lexically_normal())) != want)
                    path += L";" + d.wstring();
            SetEnvironmentVariableW(L"PATH", path.c_str());
        }

        // Where MSYS2 / MinGW / LLVM usually live when they're not on PATH.
        const std::vector<fs::path> &knownToolDirs()
        {
            static const std::vector<fs::path> dirs = {
                L"C:/msys64/ucrt64/bin", L"C:/msys64/mingw64/bin", L"C:/msys64/clang64/bin",
                L"C:/mingw64/bin", L"C:/MinGW/bin", L"C:/TDM-GCC-64/bin", L"C:/Program Files/LLVM/bin"};
            return dirs;
        }

        struct Toolchain
        {
            fs::path cxx, cc; // C++ and C compiler drivers
            fs::path make, cmake, ninja;
            bool clang = false;
        };

        fs::path resolveTool(const std::string &spec)
        {
            if (spec.empty())
                return fs::path();
            fs::path p = fromU8(spec);
            std::error_code ec;
            if (p.has_parent_path() && fs::is_regular_file(p, ec))
                return p;
            return findOnPath(spec);
        }

        Toolchain findToolchain()
        {
            Toolchain tc;
            std::error_code ec;
            if (const char *e = std::getenv("CXX"))
                tc.cxx = resolveTool(e);
            for (const char *name : {"g++", "clang++", "c++"})
                if (tc.cxx.empty())
                    tc.cxx = findOnPath(name);
            for (const auto &dir : knownToolDirs())
                for (const wchar_t *name : {L"g++.exe", L"clang++.exe"})
                    if (tc.cxx.empty() && fs::is_regular_file(dir / name, ec))
                        tc.cxx = dir / name;

            if (const char *e = std::getenv("CC"))
                tc.cc = resolveTool(e);
            if (tc.cc.empty() && !tc.cxx.empty())
            {
                // x86_64-w64-mingw32-g++ → x86_64-w64-mingw32-gcc, clang++ → clang
                std::string stem = u8(tc.cxx.stem());
                std::string c = stem;
                size_t at;
                if ((at = c.rfind("clang++")) != std::string::npos)
                    c.replace(at, 7, "clang");
                else if ((at = c.rfind("g++")) != std::string::npos)
                    c.replace(at, 3, "gcc");
                else if ((at = c.rfind("c++")) != std::string::npos)
                    c.replace(at, 3, "cc");
                fs::path cand = tc.cxx.parent_path() / fromU8(c + ".exe");
                tc.cc = fs::is_regular_file(cand, ec) ? cand : tc.cxx;
            }
            tc.clang = lower(u8(tc.cxx.stem())).find("clang") != std::string::npos;

            const fs::path binDir = tc.cxx.parent_path();
            auto findTool = [&](std::initializer_list<const char *> names) -> fs::path
            {
                for (const char *name : names)
                {
                    fs::path p = findOnPath(name);
                    if (p.empty() && !binDir.empty() && fs::is_regular_file(binDir / fromU8(std::string(name) + ".exe"), ec))
                        p = binDir / fromU8(std::string(name) + ".exe");
                    if (!p.empty())
                        return p;
                }
                return fs::path();
            };
            tc.make = findTool({"mingw32-make", "make"});
            tc.ninja = findTool({"ninja"});
            tc.cmake = findTool({"cmake"});
            if (tc.cmake.empty() && fs::is_regular_file(L"C:/Program Files/CMake/bin/cmake.exe", ec))
                tc.cmake = L"C:/Program Files/CMake/bin/cmake.exe";
            return tc;
        }

        const char *kNoCompiler =
            "[qpm] no C/C++ compiler found (looked for CXX, g++, clang++ on PATH and in C:\\msys64).\n"
            "[qpm] install one, e.g. MSYS2 from https://www.msys2.org then:\n"
            "[qpm]   pacman -S mingw-w64-ucrt-x86_64-gcc\n"
            "[qpm] or point CXX at your compiler.\n";

        int jobCount(const BuildOptions &opts)
        {
            int n = opts.jobs > 0 ? opts.jobs : static_cast<int>(std::thread::hardware_concurrency());
            return std::max(1, std::min(n, static_cast<int>(MAXIMUM_WAIT_OBJECTS)));
        }

        // ─── Manifest "build" section ───────────────────────────────────────

        std::vector<std::string> strings(const JsonValue &v)
        {
            std::vector<std::string> out;
            if (v.isString() && !v.asString().empty())
                out.push_back(v.asString());
            else if (v.isArray())
                for (const auto &e : v.array())
                    if (e.isString() && !e.asString().empty())
                        out.push_back(e.asString());
            return out;
        }

        struct BuildConfig
        {
            std::string name, std, cstd, backend;
            std::string outDir = "build";
            std::vector<std::string> sources, exclude, include, defines, libs, libDirs, flags, linkFlags;
            bool autoLibs = true;
            bool linkStatic = false;
        };

        BuildConfig readConfig(const JsonValue &b)
        {
            BuildConfig c;
            if (!b.isObject())
                return c;
            c.name = b.get("name").asString();
            c.std = b.get("std").asString();
            c.cstd = b.get("cstd").asString();
            c.backend = lower(b.get("backend").asString());
            if (!b.get("outDir").asString().empty())
                c.outDir = b.get("outDir").asString();
            c.sources = strings(b.get("sources"));
            c.exclude = strings(b.get("exclude"));
            c.include = strings(b.get("include"));
            c.defines = strings(b.get("defines"));
            c.libs = strings(b.get("libs"));
            c.libDirs = strings(b.get("libDirs"));
            c.flags = strings(b.get("flags"));
            c.linkFlags = strings(b.get("linkFlags"));
            c.autoLibs = b.get("autoLibs").asBool(true);
            c.linkStatic = b.get("static").asBool(false);
            return c;
        }

        // ─── Project scan ────────────────────────────────────────────────────

        enum class Lang
        {
            C,
            Cxx
        };

        struct Source
        {
            fs::path path;
            Lang lang = Lang::Cxx;
            bool sa = false;   // C++ written in a .sa file: compiled with -x c++
            bool entry = false; // defines main() / WinMain()
            bool gui = false;   // WinMain → -mwindows
        };

        struct Project
        {
            fs::path root;
            std::vector<Source> sources;
            std::set<std::string> includes; // every #include target, lowercased
            std::vector<fs::path> includeDirs, libDirs;
            bool saWithLocalHeaders = false; // .sa files that #include a project header
        };

        bool isSkippedDir(const std::string &name)
        {
            static const std::set<std::string> skip = {
                "build", "out", "bin", "obj", "dist", "node_modules", "quantum_qubits", "debug", "release", "x64", "x86"};
            std::string n = lower(name);
            return n.empty() || n[0] == '.' || skip.count(n) || n.rfind("cmake-build", 0) == 0;
        }

        bool hasExt(const fs::path &p, std::initializer_list<const char *> exts)
        {
            std::string e = lower(u8(p.extension()));
            for (const char *x : exts)
                if (e == x)
                    return true;
            return false;
        }

        std::string ltrim(const std::string &s)
        {
            size_t i = s.find_first_not_of(" \t");
            return i == std::string::npos ? std::string() : s.substr(i);
        }

        struct FileFacts
        {
            std::vector<std::string> includes; // in order, as written
            std::vector<std::string> quotedIncludes;
            bool entry = false, gui = false;
        };

        // Line-based: #include targets and whether the file defines main().
        FileFacts scanFile(const std::string &text)
        {
            static const std::regex mainRe(R"(^(static\s+|extern\s+"C"\s+)?(int|void|auto)\s+(main|wmain)\s*\()");
            static const std::regex winMainRe(R"(\b(WinMain|wWinMain)\s*\()");
            FileFacts f;
            std::istringstream in(text);
            std::string line;
            bool inBlockComment = false;
            while (std::getline(in, line))
            {
                std::string t = ltrim(line);
                if (inBlockComment)
                {
                    size_t e = t.find("*/");
                    if (e == std::string::npos)
                        continue;
                    inBlockComment = false;
                    t = ltrim(t.substr(e + 2));
                }
                if (t.rfind("/*", 0) == 0 && t.find("*/") == std::string::npos)
                {
                    inBlockComment = true;
                    continue;
                }
                if (t.empty() || t.rfind("//", 0) == 0)
                    continue;
                if (t[0] == '#')
                {
                    std::string d = ltrim(t.substr(1));
                    if (d.rfind("include", 0) != 0)
                        continue;
                    d = ltrim(d.substr(7));
                    if (d.empty() || (d[0] != '<' && d[0] != '"'))
                        continue;
                    char close = d[0] == '<' ? '>' : '"';
                    size_t e = d.find(close, 1);
                    if (e == std::string::npos)
                        continue;
                    std::string name = d.substr(1, e - 1);
                    f.includes.push_back(name);
                    if (close == '"')
                        f.quotedIncludes.push_back(name);
                    continue;
                }
                if (t.find("main") == std::string::npos)
                    continue;
                if (std::regex_search(t, mainRe))
                    f.entry = true;
                else if (t.find("WinMain") != std::string::npos && std::regex_search(t, winMainRe) &&
                         t.find(';') == std::string::npos)
                    f.entry = f.gui = true;
            }
            return f;
        }

        bool underAny(const fs::path &p, const std::vector<fs::path> &dirs)
        {
            std::string s = lower(u8(p.lexically_normal().generic_wstring()));
            for (const auto &d : dirs)
            {
                std::string ds = lower(u8(d.lexically_normal().generic_wstring()));
                if (s == ds || (s.size() > ds.size() && s.compare(0, ds.size(), ds) == 0 && s[ds.size()] == '/'))
                    return true;
            }
            return false;
        }

        Project scanProject(const fs::path &root, const BuildConfig &cfg)
        {
            Project proj;
            proj.root = root;
            std::error_code ec;

            std::vector<fs::path> excluded = {root / fromU8(cfg.outDir)};
            for (const auto &e : cfg.exclude)
                excluded.push_back(root / fromU8(e));

            std::vector<fs::path> files;
            std::set<std::string> seenDirs;
            auto walk = [&](const fs::path &start)
            {
                if (fs::is_regular_file(start, ec))
                {
                    files.push_back(start);
                    return;
                }
                fs::recursive_directory_iterator it(start, fs::directory_options::skip_permission_denied, ec), end;
                for (; it != end; it.increment(ec))
                {
                    if (ec)
                        break;
                    const fs::path p = it->path();
                    if (it->is_directory(ec))
                    {
                        if (isSkippedDir(u8(p.filename())) || underAny(p, excluded))
                        {
                            it.disable_recursion_pending();
                            continue;
                        }
                        // Vendored libraries: <x>/include → -I, <x>/lib with libraries → -L.
                        std::string fname = lower(u8(p.filename()));
                        if (fname == "include" && p.parent_path() != root)
                            proj.includeDirs.push_back(p);
                        if (fname == "lib" && seenDirs.insert(lower(u8(p))).second)
                        {
                            for (const auto &f : fs::directory_iterator(p, ec))
                                if (hasExt(f.path(), {".a", ".lib"}))
                                {
                                    proj.libDirs.push_back(p);
                                    break;
                                }
                        }
                        continue;
                    }
                    if (!underAny(p, excluded))
                        files.push_back(p);
                }
            };
            if (cfg.sources.empty())
                walk(root);
            else
                for (const auto &s : cfg.sources)
                    walk(root / fromU8(s));

            for (const auto &p : files)
            {
                const bool header = hasExt(p, {".h", ".hpp", ".hh", ".hxx", ".inl"});
                const bool cSrc = hasExt(p, {".c"});
                const bool cxxSrc = hasExt(p, {".cpp", ".cc", ".cxx", ".c++", ".cp"});
                const bool sa = hasExt(p, {".sa"});
                if (!header && !cSrc && !cxxSrc && !sa)
                    continue;
                FileFacts facts = scanFile(readFile(p));
                for (const auto &inc : facts.includes)
                    proj.includes.insert(lower(inc));
                if (header)
                    continue;
                if (sa)
                {
                    // A .sa file is C++ when it includes something; one that
                    // includes a header of this project marks a C++ project.
                    if (facts.includes.empty())
                        continue;
                    for (const auto &q : facts.quotedIncludes)
                        if (fs::is_regular_file(p.parent_path() / fromU8(q), ec))
                            proj.saWithLocalHeaders = true;
                }
                Source s;
                s.path = p;
                s.lang = cSrc ? Lang::C : Lang::Cxx;
                s.sa = sa;
                s.entry = facts.entry;
                s.gui = facts.gui;
                proj.sources.push_back(s);
            }
            std::sort(proj.sources.begin(), proj.sources.end(), [](const Source &a, const Source &b)
                      { return a.path < b.path; });
            return proj;
        }

        // ─── Libraries for recognised headers ────────────────────────────────

        struct LibRule
        {
            const char *header;
            std::vector<const char *> libs;
        };

        std::vector<std::string> autoLibraries(const std::set<std::string> &includes)
        {
            static const std::vector<LibRule> rules = {
                {"raylib.h", {"raylib", "opengl32", "gdi32", "winmm"}},
                {"sdl2/sdl.h", {"mingw32", "SDL2main", "SDL2"}},
                {"sdl.h", {"mingw32", "SDL2main", "SDL2"}},
                {"sdl2/sdl_image.h", {"SDL2_image"}},
                {"sdl_image.h", {"SDL2_image"}},
                {"sdl2/sdl_mixer.h", {"SDL2_mixer"}},
                {"sdl_mixer.h", {"SDL2_mixer"}},
                {"sdl2/sdl_ttf.h", {"SDL2_ttf"}},
                {"sdl_ttf.h", {"SDL2_ttf"}},
                {"sdl3/sdl.h", {"SDL3"}},
                {"sdl3_image/sdl_image.h", {"SDL3_image"}},
                {"sdl3_ttf/sdl_ttf.h", {"SDL3_ttf"}},
                {"sdl3_mixer/sdl_mixer.h", {"SDL3_mixer"}},
                {"sfml/graphics.hpp", {"sfml-graphics", "sfml-window", "sfml-system"}},
                {"sfml/window.hpp", {"sfml-window", "sfml-system"}},
                {"sfml/audio.hpp", {"sfml-audio", "sfml-system"}},
                {"sfml/network.hpp", {"sfml-network", "sfml-system"}},
                {"sfml/system.hpp", {"sfml-system"}},
                {"glfw/glfw3.h", {"glfw3", "opengl32", "gdi32"}},
                {"gl/glew.h", {"glew32", "opengl32"}},
                {"gl/glut.h", {"freeglut", "glu32", "opengl32"}},
                {"gl/freeglut.h", {"freeglut", "glu32", "opengl32"}},
                {"gl/glu.h", {"glu32", "opengl32"}},
                {"gl/gl.h", {"opengl32"}},
                {"glad/glad.h", {"opengl32"}},
                {"d3d11.h", {"d3d11", "dxgi"}},
                {"d3d9.h", {"d3d9"}},
                {"dsound.h", {"dsound"}},
                {"xinput.h", {"xinput"}},
                {"windows.h", {"gdi32", "winmm"}},
                {"mmsystem.h", {"winmm"}},
                {"commdlg.h", {"comdlg32"}},
                {"shlwapi.h", {"shlwapi"}},
                {"winsock2.h", {"ws2_32"}},
                {"ws2tcpip.h", {"ws2_32"}},
                {"pthread.h", {"pthread"}},
            };
            // Link order: a library before the ones it depends on.
            static const std::vector<std::string> order = {
                "mingw32", "SDL2main", "SDL2_image", "SDL2_mixer", "SDL2_ttf", "SDL2",
                "SDL3_image", "SDL3_mixer", "SDL3_ttf", "SDL3", "raylib",
                "sfml-graphics", "sfml-window", "sfml-audio", "sfml-network", "sfml-system",
                "glfw3", "glew32", "freeglut", "glu32", "opengl32", "d3d11", "dxgi", "d3d9",
                "dsound", "xinput", "comdlg32", "shlwapi", "gdi32", "winmm", "ws2_32", "pthread"};
            std::set<std::string> wanted;
            for (const auto &r : rules)
                if (includes.count(r.header))
                    for (const char *l : r.libs)
                        wanted.insert(l);
            std::vector<std::string> out;
            for (const auto &l : order)
                if (wanted.count(l))
                    out.push_back(l);
            return out;
        }

        // ─── Direct build ────────────────────────────────────────────────────

        struct Target
        {
            std::string name;
            fs::path exe;
            size_t entry = 0; // index into Project::sources
            bool primary = false;
        };

        std::string safeName(std::string n)
        {
            if (!n.empty() && n[0] == '@')
            {
                size_t slash = n.find('/');
                n = slash == std::string::npos ? n.substr(1) : n.substr(slash + 1);
            }
            for (char &c : n)
                if (std::string("<>:\"/\\|?* ").find(c) != std::string::npos)
                    c = '_';
            return n;
        }

        std::vector<Target> planTargets(const Project &proj, const BuildConfig &cfg, const JsonValue &manifest,
                                        const fs::path &outDir)
        {
            std::vector<size_t> entries;
            for (size_t i = 0; i < proj.sources.size(); ++i)
                if (proj.sources[i].entry)
                    entries.push_back(i);

            // A main() in the project folder or its src/ is the program itself;
            // others (tests/, tools/, examples/) build alongside under their own name.
            auto isPrimaryDir = [&](const fs::path &p)
            {
                fs::path dir = p.parent_path();
                if (dir == proj.root)
                    return true;
                std::string n = lower(u8(dir.filename()));
                return dir.parent_path() == proj.root && (n == "src" || n == "source" || n == "app");
            };
            std::vector<size_t> primary;
            for (size_t i : entries)
                if (isPrimaryDir(proj.sources[i].path))
                    primary.push_back(i);
            if (primary.empty() && entries.size() == 1)
                primary = entries; // the only program there is, wherever it lives

            std::string mainName = !cfg.name.empty() ? cfg.name : manifest.get("name").asString();
            if (mainName.empty())
                mainName = u8(proj.root.filename());
            mainName = safeName(mainName);

            std::vector<Target> targets;
            std::set<std::string> used;
            auto add = [&](size_t entry, std::string name, bool isPrimary)
            {
                if (used.count(lower(name)))
                    name = safeName(u8(proj.sources[entry].path.parent_path().filename())) + "_" + name;
                for (int n = 2; used.count(lower(name)); ++n)
                    name += std::to_string(n);
                used.insert(lower(name));
                Target t;
                t.name = name;
                t.exe = outDir / fromU8(name + ".exe");
                t.entry = entry;
                t.primary = isPrimary;
                targets.push_back(t);
            };
            if (primary.size() == 1)
                add(primary[0], mainName, true);
            for (size_t i : entries)
                if (!(primary.size() == 1 && i == primary[0]))
                    add(i, safeName(u8(proj.sources[i].path.stem())), false);
            return targets;
        }

        // Header dependencies from a gcc/clang -MMD file (first rule only).
        bool readDeps(const fs::path &depFile, std::vector<fs::path> &deps)
        {
            std::string text = readFile(depFile);
            if (text.empty())
                return false;
            std::string joined;
            for (size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] == '\\' && i + 1 < text.size() && (text[i + 1] == '\n' || text[i + 1] == '\r'))
                {
                    i += (text[i + 1] == '\r' && i + 2 < text.size() && text[i + 2] == '\n') ? 2 : 1;
                    joined += ' ';
                    continue;
                }
                if (text[i] == '\n')
                    break;
                joined += text[i];
            }
            size_t colon = joined.find(": ");
            if (colon == std::string::npos)
                return false;
            std::string cur;
            auto flush = [&]
            {
                if (!cur.empty())
                    deps.push_back(fromU8(cur));
                cur.clear();
            };
            for (size_t i = colon + 2; i < joined.size(); ++i)
            {
                char c = joined[i];
                if (c == '\\' && i + 1 < joined.size() && (joined[i + 1] == ' ' || joined[i + 1] == '#'))
                    cur += joined[++i];
                else if (c == '$' && i + 1 < joined.size() && joined[i + 1] == '$')
                    cur += joined[++i];
                else if (c == ' ' || c == '\t' || c == '\r')
                    flush();
                else
                    cur += c;
            }
            flush();
            return true;
        }

        struct CompileJob
        {
            size_t source;
            fs::path obj, dep, stamp, log;
            std::vector<std::string> argv;
            std::string stampText;
        };

        bool objectUpToDate(const CompileJob &job, const fs::path &src)
        {
            std::error_code ec;
            if (!fs::exists(job.obj, ec) || readFile(job.stamp) != job.stampText)
                return false;
            auto t = mtime(job.obj);
            if (mtime(src) > t)
                return false;
            std::vector<fs::path> deps;
            if (!readDeps(job.dep, deps))
                return false;
            for (const auto &d : deps)
                if (!fs::exists(d, ec) || mtime(d) > t)
                    return false;
            return true;
        }

        // Runs the jobs, `jobs` at a time; each one's output is printed when it
        // finishes. Stops starting new ones after the first failure.
        bool runCompileJobs(std::vector<CompileJob> &todo, const Project &proj, const BuildOptions &opts,
                            bool color)
        {
            struct Running
            {
                HANDLE h;
                size_t job;
            };
            std::vector<Running> running;
            const size_t maxJobs = static_cast<size_t>(jobCount(opts));
            size_t next = 0;
            bool failed = false;
            while ((next < todo.size() && !failed) || !running.empty())
            {
                while (!failed && next < todo.size() && running.size() < maxJobs)
                {
                    CompileJob &job = todo[next];
                    std::vector<std::string> argv = job.argv;
                    if (color)
                        argv.insert(argv.begin() + 1, "-fdiagnostics-color=always");
                    std::cout << "[qpm] [" << (next + 1) << "/" << todo.size() << "] compile "
                              << shown(proj.sources[job.source].path, proj.root) << "\n";
                    if (opts.verbose)
                        std::cout << "      " << shownCommand(argv) << "\n";
                    HANDLE h = startProcess(argv, proj.root, &job.log);
                    if (!h)
                    {
                        std::cerr << "[qpm] could not start " << job.argv[0] << " (error " << GetLastError() << ")\n";
                        failed = true;
                        break;
                    }
                    running.push_back({h, next});
                    ++next;
                }
                if (running.empty())
                    break;
                std::vector<HANDLE> handles;
                for (const auto &r : running)
                    handles.push_back(r.h);
                DWORD w = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
                size_t idx = w - WAIT_OBJECT_0;
                if (idx >= running.size())
                {
                    std::cerr << "[qpm] waiting for the compiler failed (error " << GetLastError() << ")\n";
                    return false;
                }
                Running r = running[idx];
                running.erase(running.begin() + static_cast<std::ptrdiff_t>(idx));
                int code = waitProcess(r.h);
                CompileJob &job = todo[r.job];
                std::string out = readFile(job.log);
                std::error_code ec;
                fs::remove(job.log, ec);
                if (!out.empty())
                {
                    std::cout << out;
                    if (out.back() != '\n')
                        std::cout << "\n";
                }
                if (code != 0)
                {
                    failed = true;
                    if (out.empty())
                        std::cerr << "[qpm] " << shown(proj.sources[job.source].path, proj.root)
                                  << ": the compiler exited with code " << code << " without printing an error\n";
                }
                else
                    writeFile(job.stamp, job.stampText);
            }
            return !failed;
        }

        // Picks the target `--run` / a name refers to.
        const Target *pickTarget(const std::vector<Target> &targets, const std::string &want, const Project &proj)
        {
            if (!want.empty())
            {
                for (const auto &t : targets)
                    if (lower(t.name) == lower(want) || lower(u8(proj.sources[t.entry].path.stem())) == lower(want))
                        return &t;
                return nullptr;
            }
            for (const auto &t : targets)
                if (t.primary)
                    return &t;
            return targets.empty() ? nullptr : &targets[0];
        }

        int runExecutable(const fs::path &exe, const BuildOptions &opts, const fs::path &root)
        {
            std::cout << "[qpm] run " << shown(exe, root) << "\n\n";
            std::vector<std::string> argv = {u8(exe)};
            argv.insert(argv.end(), opts.runArgs.begin(), opts.runArgs.end());
            int code = runProcess(argv, root);
            return code < 0 ? 1 : code;
        }

        int buildDirect(const fs::path &root, const BuildConfig &cfg, const JsonValue &manifest, const Project &proj,
                        const Toolchain &tc, const BuildOptions &opts)
        {
            std::error_code ec;
            const fs::path outDir = root / fromU8(cfg.outDir);
            const fs::path objDir = outDir / "obj" / (opts.release ? "release" : "debug");

            if (proj.sources.empty())
            {
                std::cerr << "[qpm] nothing to build: no C/C++ sources, CMakeLists.txt or Makefile in " << u8(root)
                          << (manifest.isObject() ? ", and no \"build\" script in the manifest" : "") << "\n";
                return 1;
            }
            std::vector<Target> targets = planTargets(proj, cfg, manifest, outDir);
            if (targets.empty())
            {
                std::cerr << "[qpm] nothing to build: no source file defines main() (or WinMain())\n";
                return 1;
            }

            if (opts.clean)
            {
                fs::remove_all(outDir / "obj", ec);
                for (const auto &t : targets)
                    if (fs::remove(t.exe, ec))
                        std::cout << "[qpm] removed " << shown(t.exe, root) << "\n";
                std::cout << "[qpm] removed " << shown(outDir / "obj", root) << "\n";
                return 0;
            }
            if (tc.cxx.empty())
            {
                std::cerr << kNoCompiler;
                return 1;
            }

            std::vector<Target> selected = targets;
            if (!opts.target.empty())
            {
                const Target *t = pickTarget(targets, opts.target, proj);
                if (!t)
                {
                    std::cerr << "[qpm] no executable named \"" << opts.target << "\"; this project builds:";
                    for (const auto &x : targets)
                        std::cerr << " " << x.name;
                    std::cerr << "\n";
                    return 1;
                }
                selected = {*t};
            }

            // Flags shared by every translation unit.
            std::vector<std::string> common;
            if (opts.release)
                common.insert(common.end(), {"-O2", "-DNDEBUG"});
            else
                common.insert(common.end(), {"-g", "-O0"});
            std::vector<fs::path> incDirs = {root};
            for (const char *d : {"include", "src"})
                if (fs::is_directory(root / d, ec))
                    incDirs.push_back(root / d);
            for (const auto &d : cfg.include)
                incDirs.push_back(root / fromU8(d));
            incDirs.insert(incDirs.end(), proj.includeDirs.begin(), proj.includeDirs.end());
            // <SDL.h> without the SDL2/ prefix: MSYS2 keeps it in include/SDL2.
            if (proj.includes.count("sdl.h"))
            {
                fs::path sdl = tc.cxx.parent_path().parent_path() / "include" / "SDL2";
                if (fs::is_directory(sdl, ec))
                    incDirs.push_back(sdl);
            }
            std::set<std::string> seenInc;
            for (const auto &d : incDirs)
                if (seenInc.insert(lower(u8(d.lexically_normal()))).second)
                    common.push_back("-I" + u8(d.lexically_normal()));
            for (const auto &d : cfg.defines)
                common.push_back("-D" + d);
            common.insert(common.end(), cfg.flags.begin(), cfg.flags.end());

            // Which sources the selected targets need.
            std::set<size_t> needed;
            for (const auto &t : selected)
                needed.insert(t.entry);
            for (size_t i = 0; i < proj.sources.size(); ++i)
                if (!proj.sources[i].entry)
                    needed.insert(i);

            std::vector<CompileJob> jobs, todo;
            std::map<size_t, fs::path> objOf;
            for (size_t i : needed)
            {
                const Source &s = proj.sources[i];
                fs::path rel = s.path.lexically_relative(root);
                const bool inside = !rel.empty() && *rel.begin() != "..";
                if (!inside)
                    rel = fs::path("_external") / s.path.filename();
                CompileJob job;
                job.source = i;
                job.obj = objDir / rel;
                job.obj += ".o";
                job.dep = job.obj;
                job.dep += ".d";
                job.stamp = job.obj;
                job.stamp += ".cmd";
                job.log = job.obj;
                job.log += ".log";
                fs::create_directories(job.obj.parent_path(), ec);

                std::vector<std::string> argv = {u8(s.lang == Lang::C ? tc.cc : tc.cxx)};
                argv.insert(argv.end(), common.begin(), common.end());
                const std::string &stdv = s.lang == Lang::C ? cfg.cstd : cfg.std;
                if (!stdv.empty())
                    argv.push_back("-std=" + stdv);
                argv.insert(argv.end(), {"-MMD", "-MP", "-MF", u8(job.dep), "-c"});
                if (s.sa)
                    argv.insert(argv.end(), {"-x", "c++"});
                // Compiles run in the project folder: a relative source path
                // gives short diagnostics (`src/main.cpp:3:13: error: ...`).
                argv.push_back(inside ? u8(rel.generic_wstring()) : u8(s.path));
                argv.insert(argv.end(), {"-o", u8(job.obj)});
                job.argv = argv;
                job.stampText = shownCommand(argv);
                objOf[i] = job.obj;
                if (!objectUpToDate(job, s.path))
                    todo.push_back(job);
            }

            const bool color = stderrIsConsole();
            if (!todo.empty() && !runCompileJobs(todo, proj, opts, color))
            {
                std::cerr << "[qpm] build failed\n";
                return 1;
            }

            // Link.
            std::vector<std::string> libs = cfg.libs;
            if (cfg.autoLibs)
                for (const auto &l : autoLibraries(proj.includes))
                    if (std::find(libs.begin(), libs.end(), l) == libs.end())
                        libs.push_back(l);
            std::vector<fs::path> libDirs;
            for (const auto &d : cfg.libDirs)
                libDirs.push_back(root / fromU8(d));
            libDirs.insert(libDirs.end(), proj.libDirs.begin(), proj.libDirs.end());

            bool anyCxx = false;
            for (size_t i : needed)
                anyCxx = anyCxx || proj.sources[i].lang == Lang::Cxx;

            std::vector<fs::path> built;
            bool linked = false;
            for (const auto &t : selected)
            {
                std::vector<fs::path> objs = {objOf[t.entry]};
                for (size_t i = 0; i < proj.sources.size(); ++i)
                    if (!proj.sources[i].entry)
                        objs.push_back(objOf[i]);

                std::vector<std::string> argv = {u8(anyCxx ? tc.cxx : tc.cc)};
                for (const auto &o : objs)
                    argv.push_back(u8(o));
                argv.insert(argv.end(), {"-o", u8(t.exe)});
                for (const auto &d : libDirs)
                    argv.push_back("-L" + u8(d.lexically_normal()));
                if (!opts.release)
                    argv.push_back("-g");
                if (cfg.linkStatic)
                    argv.push_back("-static");
                else if (!tc.clang)
                {
                    // MinGW: the GCC runtime goes into the .exe, so it doesn't
                    // load whichever libstdc++-6.dll comes first on PATH (Git's
                    // own is incompatible) and runs on machines without MSYS2.
                    argv.push_back("-static-libgcc");
                    if (anyCxx)
                        argv.push_back("-static-libstdc++");
                }
                if (proj.sources[t.entry].gui)
                    argv.push_back("-mwindows");
                argv.insert(argv.end(), cfg.linkFlags.begin(), cfg.linkFlags.end());
                for (const auto &l : libs)
                    argv.push_back("-l" + l);
                if (!cfg.linkStatic && !tc.clang)
                {
                    // libstdc++'s threads use winpthread: static too (whole
                    // archive, so g++'s own trailing -lwinpthread finds it)
                    argv.push_back("-Wl,-Bstatic,--whole-archive");
                    argv.push_back("-lwinpthread");
                    argv.push_back("-Wl,--no-whole-archive,-Bdynamic");
                }

                fs::path stamp = objDir / fromU8(t.name + ".link");
                std::string stampText = shownCommand(argv);
                bool upToDate = fs::exists(t.exe, ec) && readFile(stamp) == stampText;
                if (upToDate)
                {
                    auto te = mtime(t.exe);
                    for (const auto &o : objs)
                        if (mtime(o) > te)
                            upToDate = false;
                }
                if (upToDate)
                {
                    built.push_back(t.exe);
                    continue;
                }

                fs::create_directories(outDir, ec);
                std::cout << "[qpm] link " << shown(t.exe, root) << "\n";
                if (opts.verbose)
                    std::cout << "      " << stampText << "\n";
                if (color)
                    argv.insert(argv.begin() + 1, "-fdiagnostics-color=always");
                auto result = captureProcess(argv, objDir / fromU8(t.name + ".link.log"));
                if (!result.second.empty())
                {
                    std::cout << result.second;
                    if (result.second.back() != '\n')
                        std::cout << "\n";
                }
                if (result.first != 0)
                {
                    if (result.second.find("Permission denied") != std::string::npos)
                        std::cerr << "[qpm] " << shown(t.exe, root)
                                  << " couldn't be replaced. Is it still running? Close it and build again.\n";
                    std::cerr << "[qpm] build failed\n";
                    return 1;
                }
                writeFile(stamp, stampText);
                built.push_back(t.exe);
                linked = true;
            }

            std::cout << "[qpm] " << (todo.empty() && !linked ? "up to date:" : "built");
            for (size_t i = 0; i < built.size(); ++i)
                std::cout << (i ? ", " : " ") << shown(built[i], root);
            std::cout << "\n";

            if (!opts.run)
                return 0;
            const Target *t = pickTarget(selected, "", proj);
            return t ? runExecutable(t->exe, opts, root) : 0;
        }

        // ─── CMake ───────────────────────────────────────────────────────────

        std::string cacheValue(const std::string &cache, const std::string &key)
        {
            std::istringstream in(cache);
            std::string line;
            while (std::getline(in, line))
            {
                if (line.rfind(key + ":", 0) != 0)
                    continue;
                size_t eq = line.find('=');
                if (eq == std::string::npos)
                    return std::string();
                std::string v = line.substr(eq + 1);
                while (!v.empty() && (v.back() == '\r' || v.back() == ' '))
                    v.pop_back();
                return v;
            }
            return std::string();
        }

        // add_executable() names in a CMakeLists.txt (${PROJECT_NAME} resolved).
        std::vector<std::string> cmakeExecutables(const fs::path &root)
        {
            std::string text = readFile(root / "CMakeLists.txt");
            std::string project;
            std::smatch m;
            static const std::regex projectRe(R"(project\s*\(\s*([A-Za-z0-9_.+-]+))", std::regex::icase);
            if (std::regex_search(text, m, projectRe))
                project = m[1].str();
            static const std::regex exeRe(R"(add_executable\s*\(\s*([A-Za-z0-9_.+${}-]+)\s+(\w*))", std::regex::icase);
            std::vector<std::string> names;
            for (auto it = std::sregex_iterator(text.begin(), text.end(), exeRe); it != std::sregex_iterator(); ++it)
            {
                std::string name = (*it)[1].str();
                std::string next = (*it)[2].str();
                if (next == "IMPORTED" || next == "ALIAS")
                    continue;
                size_t at = name.find("${PROJECT_NAME}");
                if (at != std::string::npos)
                    name.replace(at, 15, project);
                if (name.find('$') == std::string::npos)
                    names.push_back(name);
            }
            return names;
        }

        fs::path findBuiltExe(const fs::path &dir, const std::string &name)
        {
            std::error_code ec;
            const std::string want = lower(name + ".exe");
            fs::path best;
            fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
            for (; it != end; it.increment(ec))
            {
                if (ec)
                    break;
                if (it->is_directory(ec))
                {
                    if (u8(it->path().filename()) == "CMakeFiles")
                        it.disable_recursion_pending();
                    continue;
                }
                if (lower(u8(it->path().filename())) == want && (best.empty() || mtime(it->path()) > mtime(best)))
                    best = it->path();
            }
            return best;
        }

        int buildCMake(const fs::path &root, const BuildConfig &cfg, const Toolchain &tc, const BuildOptions &opts)
        {
            std::error_code ec;
            if (tc.cmake.empty())
            {
                std::cerr << "[qpm] this project uses CMake, but cmake isn't installed (https://cmake.org/download/)\n"
                          << "[qpm] or build it without CMake: qpm build --direct\n";
                return 1;
            }
            const fs::path bdir = root / fromU8(cfg.outDir);
            const std::string buildType = opts.release ? "Release" : "Debug";
            const std::string cmake = u8(tc.cmake);
            const std::string cache = readFile(bdir / "CMakeCache.txt");
            const bool configured = !cache.empty();

            if (opts.clean)
            {
                if (!configured)
                {
                    std::cout << "[qpm] nothing to clean\n";
                    return 0;
                }
                return runProcess({cmake, "--build", u8(bdir), "--target", "clean"}, root) == 0 ? 0 : 1;
            }

            bool generated = fs::exists(bdir / "Makefile", ec) || fs::exists(bdir / "build.ninja", ec);
            if (!generated && fs::is_directory(bdir, ec))
                for (const auto &e : fs::directory_iterator(bdir, ec))
                    if (hasExt(e.path(), {".sln"}))
                        generated = true;
            const std::string generator = cacheValue(cache, "CMAKE_GENERATOR");
            const bool multiConfig = generator.find("Visual Studio") != std::string::npos ||
                                     generator.find("Multi-Config") != std::string::npos || generator == "Xcode";

            if (!configured || !generated)
            {
                // A cache from a configure that never finished (e.g. one that
                // picked NMake without MSVC) is started over with --fresh.
                std::vector<std::string> argv = {cmake};
                if (configured)
                    argv.push_back("--fresh");
                argv.insert(argv.end(), {"-S", u8(root), "-B", u8(bdir)});
                std::string gen;
                if (!tc.cxx.empty() && lower(u8(tc.make.stem())) == "mingw32-make")
                {
                    gen = "MinGW Makefiles";
                    argv.insert(argv.end(), {"-G", gen, "-DCMAKE_MAKE_PROGRAM=" + u8(tc.make)});
                }
                else if (!tc.cxx.empty() && !tc.ninja.empty())
                {
                    gen = "Ninja";
                    argv.insert(argv.end(), {"-G", gen, "-DCMAKE_MAKE_PROGRAM=" + u8(tc.ninja)});
                }
                // (The C compiler next to it is found through PATH; naming it
                // here would warn in C++-only projects.)
                if (!gen.empty())
                    argv.push_back("-DCMAKE_CXX_COMPILER=" + u8(tc.cxx));
                argv.push_back("-DCMAKE_BUILD_TYPE=" + buildType);
                std::cout << "[qpm] configure " << shown(bdir, root) << " (cmake" << (gen.empty() ? "" : ", " + gen)
                          << ")\n";
                if (opts.verbose)
                    std::cout << "      " << shownCommand(argv) << "\n";
                if (runProcess(argv, root) != 0)
                {
                    std::cerr << "[qpm] cmake configure failed\n";
                    return 1;
                }
            }
            else if (!multiConfig && cacheValue(cache, "CMAKE_BUILD_TYPE") != buildType)
            {
                if (runProcess({cmake, "-S", u8(root), "-B", u8(bdir), "-DCMAKE_BUILD_TYPE=" + buildType}, root) != 0)
                    return 1;
            }

            std::vector<std::string> argv = {cmake, "--build", u8(bdir), "--parallel", std::to_string(jobCount(opts))};
            if (multiConfig)
                argv.insert(argv.end(), {"--config", buildType});
            if (!opts.target.empty())
                argv.insert(argv.end(), {"--target", opts.target});
            std::cout << "[qpm] build " << shown(bdir, root) << " (cmake, " << buildType << ")\n";
            if (opts.verbose)
                std::cout << "      " << shownCommand(argv) << "\n";
            if (runProcess(argv, root) != 0)
            {
                std::cerr << "[qpm] build failed\n";
                return 1;
            }

            std::vector<std::string> names = cmakeExecutables(root);
            std::vector<fs::path> exes;
            for (const auto &n : names)
            {
                fs::path exe = findBuiltExe(bdir, n);
                if (!exe.empty() && (opts.target.empty() || lower(n) == lower(opts.target)))
                    exes.push_back(exe);
            }
            if (!exes.empty())
            {
                std::cout << "[qpm] built";
                for (size_t i = 0; i < exes.size(); ++i)
                    std::cout << (i ? ", " : " ") << shown(exes[i], root);
                std::cout << "\n";
            }
            if (!opts.run)
                return 0;
            if (exes.empty())
            {
                std::cerr << "[qpm] built, but couldn't find an executable to run\n";
                return 1;
            }
            return runExecutable(exes[0], opts, root);
        }

        // ─── make ────────────────────────────────────────────────────────────

        fs::path findMakefile(const fs::path &root)
        {
            std::error_code ec;
            for (const char *n : {"GNUmakefile", "Makefile", "makefile"})
                if (fs::is_regular_file(root / n, ec))
                    return root / n;
            return fs::path();
        }

        int buildMake(const fs::path &root, const Toolchain &tc, const BuildOptions &opts)
        {
            if (tc.make.empty())
            {
                std::cerr << "[qpm] this project has a Makefile, but make isn't installed\n"
                          << "[qpm]   pacman -S mingw-w64-ucrt-x86_64-make   (MSYS2)\n"
                          << "[qpm] or build it without make: qpm build --direct\n";
                return 1;
            }
            const std::string make = u8(tc.make);
            if (opts.clean)
                return runProcess({make, "clean"}, root) == 0 ? 0 : 1;

            // The compiler's folder is on PATH (see runBuild), so a Makefile's
            // own `CXX = g++` finds it.
            std::vector<std::string> argv = {make, "-j" + std::to_string(jobCount(opts))};
            if (!opts.target.empty())
                argv.push_back(opts.target);
            std::cout << "[qpm] build (" << u8(tc.make.filename()) << ")\n";
            if (opts.verbose)
                std::cout << "      " << shownCommand(argv) << "\n";
            if (runProcess(argv, root) != 0)
            {
                std::cerr << "[qpm] build failed\n";
                return 1;
            }
            if (!opts.run)
                return 0;

            // The program to run: the named target, else the first of `all:`.
            std::string name = opts.target;
            if (name.empty())
            {
                static const std::regex allRe(R"((^|\n)all\s*:\s*([^\s#]+))");
                std::string text = readFile(findMakefile(root));
                std::smatch m;
                if (std::regex_search(text, m, allRe))
                    name = m[2].str();
            }
            std::error_code ec;
            for (const fs::path &cand : {root / fromU8(name + ".exe"), root / fromU8(name),
                                         root / "build" / fromU8(name + ".exe")})
                if (!name.empty() && fs::is_regular_file(cand, ec))
                    return runExecutable(cand, opts, root);
            std::cerr << "[qpm] built, but couldn't tell which program to run; try `qpm build <name> --run`\n";
            return 1;
        }
    } // namespace

    int runBuild(const BuildOptions &opts)
    {
        const fs::path root = fs::absolute(fromU8(opts.projectDir));

        JsonValue manifest;
        fs::path manifestPath = findManifest(root);
        if (!manifestPath.empty())
        {
            try
            {
                manifest = loadManifest(manifestPath).data;
            }
            catch (const std::exception &e)
            {
                std::cerr << "[qpm] failed to parse " << e.what() << "\n";
                return 1;
            }
        }

        // Like `npm run build`: the project's own build script wins.
        JsonValue scripts = manifest.get("scripts");
        if (opts.backend == BuildBackend::Auto && !opts.clean && scripts.isObject() && scripts.has("build"))
        {
            if (opts.run || !opts.target.empty())
                std::cerr << "[qpm] note: running the \"build\" script; --run and <target> apply to native builds"
                             " (force one with --cmake, --make or --direct)\n";
            return runScript(u8(root), "build");
        }

        BuildConfig cfg = readConfig(manifest.get("build"));
        Toolchain tc = findToolchain();
        // Compilers outside PATH (C:\msys64\...) need their folder for cc1plus,
        // as and the runtime DLLs — for the build and for `--run`.
        prependToPath(tc.cxx.parent_path());

        BuildBackend backend = opts.backend;
        if (backend == BuildBackend::Auto)
        {
            if (cfg.backend == "cmake")
                backend = BuildBackend::CMake;
            else if (cfg.backend == "make")
                backend = BuildBackend::Make;
            else if (cfg.backend == "direct")
                backend = BuildBackend::Direct;
        }

        Project proj;
        if (backend == BuildBackend::Auto || backend == BuildBackend::Direct)
            proj = scanProject(root, cfg);

        std::error_code ec;
        if (backend == BuildBackend::Auto)
        {
            const bool hasCMake = fs::is_regular_file(root / "CMakeLists.txt", ec);
            const fs::path makefile = findMakefile(root);
            if (proj.saWithLocalHeaders)
            {
                backend = BuildBackend::Direct;
                if (hasCMake || !makefile.empty())
                    std::cout << "[qpm] C++ sources are .sa files, which " << (hasCMake ? "CMakeLists.txt" : "the Makefile")
                              << " can't compile; building them directly\n";
            }
            else if (hasCMake)
                backend = BuildBackend::CMake;
            else if (!makefile.empty() && !tc.make.empty())
                backend = BuildBackend::Make;
            else
                backend = BuildBackend::Direct;
        }

        if (!tc.cxx.empty() && !opts.clean)
            std::cout << "[qpm] compiler " << shown(tc.cxx, root) << "\n";

        switch (backend)
        {
        case BuildBackend::CMake:
            return buildCMake(root, cfg, tc, opts);
        case BuildBackend::Make:
            return buildMake(root, tc, opts);
        default:
            return buildDirect(root, cfg, manifest, proj, tc, opts);
        }
    }

} // namespace qpm
