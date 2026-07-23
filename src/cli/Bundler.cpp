#include "Cli.h"
#include "Pipeline.h"
#include "Dialect.h"
#include "Serializer.h"
#include "Parser.h"
#include "Vm.h"
#include "Error.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
    #include <limits.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
    static const char *EXE_EXT = ".exe";
#else
    static const char *EXE_EXT = "";
#endif


// Quote a program path for system().  On POSIX the shell searches $PATH
// only -- never the current directory -- so a bare name like "hello" is
// not found even when ./hello exists.  Prefix "./" when there is no slash.
static std::string shellExec(const std::string &p)
{
#ifdef _WIN32
    return "\"" + p + "\"";
#else
    if (p.find('/') == std::string::npos)
        return "\"./" + p + "\"";
    return "\"" + p + "\"";
#endif
}

namespace fs = std::filesystem;

// ─── Executable path ──────────────────────────────────────────────────────────

std::string getExecutablePath()
{
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    return std::string(buffer);
#else
    char buffer[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buffer, PATH_MAX - 1);
    if (n == -1) return std::string();
    buffer[n] = '\0';
    return std::string(buffer);
#endif
}

// ─── Embedded bytecode ────────────────────────────────────────────────────────
// Format (appended after the PE image):
//   [payload bytes ...] [payloadSize: uint32_t LE] [magic: "QNTM_VM!" 8 bytes]

std::shared_ptr<Chunk> loadEmbeddedBytecode(const std::string &exePath)
{
    std::ifstream file(exePath, std::ios::binary | std::ios::ate);
    if (!file)
        return nullptr;

    auto size = (uint64_t)file.tellg();
    if (size < 12)
        return nullptr;

    // Check magic at the very end
    file.seekg(-(std::streamoff)8, std::ios::end);
    char magic[8];
    file.read(magic, 8);
    if (std::memcmp(magic, "QNTM_VM!", 8) != 0)
        return nullptr;

    // Read payload size
    file.seekg(-(std::streamoff)12, std::ios::end);
    uint32_t payloadSize = 0;
    file.read(reinterpret_cast<char *>(&payloadSize), 4);

    // Sanity: payload must fit in file and be non-zero, non-absurd
    if (payloadSize == 0 || payloadSize > 64u * 1024 * 1024)
        return nullptr;
    if ((uint64_t)(payloadSize + 12) > size)
        return nullptr;

    // Read payload
    file.seekg(-(std::streamoff)(payloadSize + 12), std::ios::end);
    std::vector<uint8_t> payload(payloadSize);
    file.read(reinterpret_cast<char *>(payload.data()), payloadSize);
    if (!file)
        return nullptr;

    try
    {
        return Serializer::deserialize(payload);
    }
    catch (...)
    {
        return nullptr;
    }
}
// ─── findStubPath ─────────────────────────────────────────────────────────────
// Searches for quantum_stub.exe next to quantum.exe (or in build/ subdirs).
// All messages go to stdout so the user always sees them.

static std::string findStubPath(const std::string &quantumExePath)
{
    fs::path base = fs::path(quantumExePath).parent_path();

    std::vector<fs::path> candidates = {
        base / (std::string("quantum_stub") + EXE_EXT),
        base / "build" / (std::string("quantum_stub") + EXE_EXT),
        base / "build" / "Release" / (std::string("quantum_stub") + EXE_EXT),
        base / "build" / "Debug" / (std::string("quantum_stub") + EXE_EXT),
    };

    for (auto &p : candidates)
    {
        if (fs::exists(p))
            return p.string();
    }

    // Nothing found — tell the user exactly where we looked
    std::cout << Colors::RED << "[Error] " << Colors::RESET
              << "quantum_stub" << EXE_EXT << " not found. Searched:\n";
    for (auto &p : candidates)
        std::cout << "  " << p.string() << "\n";
#ifdef _WIN32
    std::cout << "Run build.bat to rebuild all three binaries.\n";
#else
    std::cout << "Rebuild with: cmake --build .\n";
#endif
    return "";
}

// ─── bundleAndRun ─────────────────────────────────────────────────────────────
// Compiles a source file (.sa/.js/.py/.rb/.c/.cpp) → bytecode, appends it to a
// copy of quantum_stub.exe, writes <name>.exe next to the source file,
// then launches it and waits.

int bundleAndRun(const std::string &path, const std::string &exePath)
{
    // 1. Read source
    std::ifstream src(path);
    if (!src.is_open())
    {
        std::cout << Colors::RED << "[Error] " << Colors::RESET
                  << "Cannot open: " << path << "\n";
        std::cout.flush();
        return 1;
    }
    std::ostringstream ss;
    ss << src.rdbuf();

    // 2. Compile
    std::shared_ptr<Chunk> chunk;
    try
    {
        chunk = compileSource(applyDialect(ss.str(), path), path, false);
    }
    catch (const ParseError &e)
    {
        std::cout << Colors::RED << Colors::BOLD << "\n  X ParseError" << Colors::RESET
                  << " in " << path << " at line " << e.line << ":" << e.col
                  << "\n    " << e.what() << "\n\n";
        std::cout.flush();
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cout << Colors::RED << "[Compile Error] " << Colors::RESET << e.what() << "\n";
        std::cout.flush();
        return 1;
    }

    // 3. Serialize bytecode
    auto payload = Serializer::serialize(chunk);
    uint32_t payloadSize = (uint32_t)payload.size();

    // 4. Find quantum_stub.exe (the template runtime)
    std::string stub = findStubPath(exePath);
    if (stub.empty())
    {
        std::cout.flush();
        return 1;
    }

    // 5. Determine output path: hello.sa → hello.exe
    fs::path srcPath(path);
    std::string outName;
    if (srcPath.parent_path().empty())
        outName = (fs::current_path() / srcPath.stem()).string() + EXE_EXT;
    else
        outName = (srcPath.parent_path() / srcPath.stem()).string() + EXE_EXT;

    // Safety: never overwrite quantum.exe, qrun.exe, or quantum_stub.exe
    {
        std::string stemLower = fs::path(outName).stem().string();
        std::transform(stemLower.begin(), stemLower.end(), stemLower.begin(), ::tolower);
        if (stemLower == "quantum" || stemLower == "qrun" || stemLower == "quantum_stub")
            outName = (fs::path(outName).parent_path() /
                       (fs::path(outName).stem().string() + "_out"))
                          .string() +
                      EXE_EXT;
    }

    // 6. Copy stub → output exe
    std::error_code copyErr;
    fs::copy_file(stub, outName, fs::copy_options::overwrite_existing, copyErr);
    if (copyErr)
    {
        std::cout << Colors::RED << "[Error] " << Colors::RESET
                  << "Cannot create " << outName << ": " << copyErr.message() << "\n";
        std::cout.flush();
        return 1;
    }

    // 7. Append payload: [bytes] [size: uint32 LE] [magic: "QNTM_VM!" 8 bytes]
    {
        std::ofstream out(outName, std::ios::binary | std::ios::app);
        if (!out)
        {
            std::cout << Colors::RED << "[Error] " << Colors::RESET
                      << "Cannot open " << outName << " for appending\n";
            std::cout.flush();
            return 1;
        }
        out.write(reinterpret_cast<const char *>(payload.data()), payloadSize);
        out.write(reinterpret_cast<const char *>(&payloadSize), 4);
        out.write("QNTM_VM!", 8);
        out.flush();
        if (!out)
        {
            std::cout << Colors::RED << "[Error] " << Colors::RESET
                      << "Write failed on " << outName << "\n";
            std::cout.flush();
            return 1;
        }
    }

    std::cout << Colors::GREEN << "[Compiled] " << Colors::RESET
              << path << "  ->  " << outName << "  (" << payloadSize << " bytes)\n";
    std::cout.flush();

    // 8. Launch the produced .exe and wait for it to finish
    std::cout << Colors::CYAN << "[Running]  " << Colors::RESET << outName << "\n\n";
    std::cout.flush();

#ifdef _WIN32
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + outName + "\"";

    if (!CreateProcessA(NULL, const_cast<char *>(cmd.c_str()),
                        NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
    {
        std::cout << Colors::RED << "[Error] " << Colors::RESET
                  << "Could not launch " << outName
                  << "  (Windows error " << GetLastError() << ")\n";
        std::cout.flush();
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exitCode;
#else
    std::string cmd = shellExec(outName);
    int rc = std::system(cmd.c_str());
    if (rc == -1)
    {
        std::cout << Colors::RED << "[Error] " << Colors::RESET
                  << "Could not launch " << outName << "\n";
        std::cout.flush();
        return 1;
    }
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    return 1;
#endif
}

