/*
 * Quantum Language v2.0.0 — Bytecode VM
 *
 * Build defines (set by CMakeLists.txt):
 *   QUANTUM_MODE_COMPILER  → quantum.exe      (compiles .sa → .exe, then runs it)
 *   QRUN_MODE              → qrun.exe         (always interprets, never bundles)
 *   neither                → quantum_stub.exe  (standalone bundled exe — hello.exe etc.)
 */

#include "Lexer.h"
#include "Parser.h"
#include "Compiler.h"
#include "VM.h"
#include "Disassembler.h"
#include "TypeChecker.h"
#include "Error.h"
#include "Value.h"
#include "Serializer.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <cstdlib>

// Windows-only — bundling and launching use Win32 API
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setjmp.h>
#include <signal.h>

namespace fs = std::filesystem;

// Shared with Vm.cpp
bool g_testMode = false;

// ─── Custom Extension Guard for Multi-Language Rules ───────────────────────
static void validateParadigmExtensions(const std::string &path, const std::string &sourceCode)
{
    // Extract file extension in lowercase
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // .sa is the Magic Extension: Skip validation and allow everything smoothly
    if (ext == ".sa") {
        return; 
    }

    // Strict Mode: Checking JavaScript Files
    if (ext == ".js") {
        std::vector<std::string> cppKeywords = { "int ", "float ", "cout", "printf", "scanf", "->" };
        for (const auto &keyword : cppKeywords) {
            if (sourceCode.find(keyword) != std::string::npos) {
                std::cerr << "\033[31m[Syntax Error]\033[0m C/C++ architecture element '" 
                          << keyword << "' is not supported inside a strict JavaScript (.js) module.\n";
                std::exit(1);
            }
        }
    }
    // Strict Mode: Checking C / C++ Files
    else if (ext == ".cpp" || ext == ".c") {
        std::vector<std::string> jsKeywords = { "let ", "const ", "fn ", "function", ".push(", ".map(" };
        for (const auto &keyword : jsKeywords) {
            if (sourceCode.find(keyword) != std::string::npos) {
                std::cerr << "\033[31m[Syntax Error]\033[0m Dynamic scripting element '" 
                          << keyword << "' is not supported inside a native C/C++ module.\n";
                std::exit(1);
            }
        }
    }
    // Handle unexpected/unconfigured extensions safely
    else {
        std::cerr << "\033[31m[Error]\033[0m Unsupported file extension: " << ext << "\n";
        std::exit(1);
    }
}

// ─── Executable path ──────────────────────────────────────────────────────────

static std::string getExecutablePath()
{
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    return std::string(buffer);
}

// ─── Embedded bytecode ────────────────────────────────────────────────────────
// Format (appended after the PE image):
//   [payload bytes ...] [payloadSize: uint32_t LE] [magic: "QNTM_VM!" 8 bytes]

static std::shared_ptr<Chunk> loadEmbeddedBytecode(const std::string &exePath)
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

// ─── Banner ───────────────────────────────────────────────────────────────────

static void printBanner()
{
    std::cout
        << Colors::CYAN << Colors::BOLD
        << "\n"
        << "  ██████╗ ██╗   ██╗ █████╗ ███╗   ██╗████████╗██╗   ██╗███╗   ███╗\n"
        << " ██╔═══██╗██║   ██║██╔══██╗████╗  ██║╚══██╔══╝██║   ██║████╗ ████║\n"
        << " ██║   ██║██║   ██║███████║██╔██╗ ██║   ██║   ██║   ██║██╔████╔██║\n"
        << " ██║▄▄ ██║██║   ██║██╔══██║██║╚██╗██║   ██║   ██║   ██║██║╚██╔╝██║\n"
        << " ╚██████╔╝╚██████╔╝██║  ██║██║ ╚████║   ██║   ╚██████╔╝██║ ╚═╝ ██║\n"
        << "  ╚══▀▀═╝  ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═══╝   ╚═╝    ╚═════╝ ╚═╝     ╚═╝\n"
        << Colors::RESET
        << Colors::YELLOW << "  Quantum Language v2.0.0 | Bytecode VM Edition\n"
        << Colors::RESET << "\n";
}

static void printAura()
{
    std::cout
        << Colors::CYAN << Colors::BOLD
        << "\n╔══════════════════════════════════════════════════════════════════╗\n"
        << "║" << Colors::YELLOW << "                🌟 QUANTUM LANGUAGE ACHIEVEMENTS 🌟" << Colors::CYAN << "               ║\n"
        << "╠══════════════════════════════════════════════════════════════════╣\n"
        << "║" << Colors::GREEN << "  ✅ Complete C++17 Compiler + Bytecode VM" << Colors::CYAN << "                        ║\n"
        << "║" << Colors::GREEN << "  ✅ Multi-Syntax: Python + JavaScript + C/C++" << Colors::CYAN << "                    ║\n"
        << "║" << Colors::GREEN << "  ✅ Closures, Classes, Exceptions, Pointers" << Colors::CYAN << "                      ║\n"
        << "║" << Colors::GREEN << "  ✅ Self-bundling standalone .exe generation" << Colors::CYAN << "                     ║\n"
        << "╚══════════════════════════════════════════════════════════════════╝\n"
        << Colors::RESET;
}

// ─── Compile source → Chunk ───────────────────────────────────────────────────

static std::shared_ptr<Chunk> compileSource(const std::string &source,
                                            const std::string &sourcePath = "<input>",
                                            bool debug = false)
{
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    auto ast = parser.parse();

    try
    {
        TypeChecker tc;
        tc.check(ast);
    }
    catch (const StaticTypeError &e)
    {
        std::cerr << Colors::YELLOW << "[TypeWarning] " << Colors::RESET
                  << e.what() << " (line " << e.line << ")\n";
    }

    Compiler compiler;
    auto chunk = compiler.compile(*ast);

    if (debug)
    {
        std::cerr << Colors::CYAN << "[DEBUG] Bytecode — " << sourcePath << "\n"
                  << Colors::RESET;
        disassembleChunk(*chunk, std::cerr);
    }
    return chunk;
}

// ─── runFile — interpret a file in-place (no exe created) ─────────────────
static void runFile(const std::string &path, bool debug = false)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        std::cerr << Colors::RED << "[Error] " << Colors::RESET
                  << "Cannot open file: " << path << "\n";
        std::exit(1);
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    
    // ─── HOOK VALIDATOR HERE ───
    validateParadigmExtensions(path, ss.str());

    try
    {
        VM vm;
        vm.run(compileSource(ss.str(), path, debug));
    }
    catch (const ParseError &e)
    {
        std::cerr << Colors::RED << Colors::BOLD
                  << "\n   X ParseError" << Colors::RESET
                  << " in " << path << " at line " << e.line << ":" << e.col
                  << "\n    " << e.what() << "\n\n";
        std::exit(1);
    }
    catch (const QuantumError &e)
    {
        std::cerr << Colors::RED << Colors::BOLD
                  << "\n   X " << e.kind << Colors::RESET;
        if (e.line > 0)
            std::cerr << " at line " << e.line;
        std::cerr << "\n    " << e.what() << "\n\n";
        std::exit(1);
    }
    catch (const std::exception &e)
    {
        std::cerr << Colors::RED << "[Fatal] " << Colors::RESET << e.what() << "\n";
        std::exit(1);
    }
}

// ─── checkFile ────────────────────────────────────────────────────────────────

static int checkFile(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        std::cerr << path << ":1:1: error: Cannot open\n";
        return 1;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    try
    {
        Lexer l(ss.str());
        auto tok = l.tokenize();
        Parser p(std::move(tok));
        auto ast = p.parse();
        try
        {
            TypeChecker tc;
            tc.check(ast);
        }
        catch (const StaticTypeError &e)
        {
            std::cerr << path << ":" << e.line << ":1: warning: " << e.what() << "\n";
        }
        std::cout << Colors::GREEN << "[OK] " << Colors::RESET << path << "\n";
        return 0;
    }
    catch (const ParseError &e)
    {
        std::cerr << path << ":" << e.line << ":" << e.col << ": error: " << e.what() << "\n";
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << path << ":1:1: error: " << e.what() << "\n";
        return 1;
    }
}

// ─── Batch test ───────────────────────────────────────────────────────────────

struct TestResult
{
    std::string path, source, error, output;
    int line = 0, col = 0;
    bool passed = false;
    bool crashed = false; // true when a Win32 SEH fault was caught
};

static void redirectStdinToNull()
{
    FILE *n = nullptr;
    freopen_s(&n, "NUL", "r", stdin);
}

static bool isInputDriven(const std::string &m)
{
    return m.find("got string") != m.npos || m.find("got nil") != m.npos ||
           m.find("Cannot convert ''") != m.npos;
}

// ── Crash-guarded VM execution ───────────────────────────────────────────────
// MinGW/GCC does not support __try/__except.  Instead we use POSIX signals
// (SIGSEGV / SIGFPE / SIGILL / SIGABRT) combined with setjmp/longjmp to
// intercept hard crashes without killing the whole process.
//
// The pattern:
//   1. Install signal handlers that longjmp back to a safe point.
//   2. setjmp() — if a signal fires, longjmp brings us back here with a
//      non-zero value that encodes which signal hit.
//   3. Run the VM.
//   4. Restore the original signal handlers.
//
// Limitation: longjmp out of a signal handler is technically UB in C++, but
// it is the standard approach on MinGW/GCC Windows where SEH is unavailable,
// and works reliably in practice for our use-case (test runner, not production).

static jmp_buf g_crashJmpBuf;
static int g_crashSignal = 0; // signal number that fired, 0 = none

static void crashSignalHandler(int sig)
{
    g_crashSignal = sig;
    // Re-install the handler so repeated signals work (required on some targets)
    signal(sig, crashSignalHandler);
    longjmp(g_crashJmpBuf, sig);
}

static std::string runVmGuarded(const std::string &source,
                                const std::string &path,
                                std::string &outCapture)
{
    // --- set up output capture ---
    std::ostringstream sink;
    std::streambuf *savedOut = std::cout.rdbuf(sink.rdbuf());
    std::streambuf *savedErr = std::cerr.rdbuf(sink.rdbuf());

    // --- install crash signal handlers ---
    g_crashSignal = 0;
    auto prevSEGV = signal(SIGSEGV, crashSignalHandler);
    auto prevFPE = signal(SIGFPE, crashSignalHandler);
    auto prevILL = signal(SIGILL, crashSignalHandler);
    auto prevABRT = signal(SIGABRT, crashSignalHandler);

    std::string errorMsg;

    int jumpVal = setjmp(g_crashJmpBuf);
    if (jumpVal == 0)
    {
        // Normal path — run the VM
        try
        {
            VM vm;
            vm.run(compileSource(source, path, false));
        }
        catch (...)
        {
            // Restore before re-throwing so the caller's catch blocks work
            signal(SIGSEGV, prevSEGV);
            signal(SIGFPE, prevFPE);
            signal(SIGILL, prevILL);
            signal(SIGABRT, prevABRT);
            std::cout.rdbuf(savedOut);
            std::cerr.rdbuf(savedErr);
            outCapture = sink.str();
            throw;
        }
    }
    else
    {
        // Signal fired — longjmp landed here
        switch (jumpVal)
        {
        case SIGSEGV:
            errorMsg = "CrashError: Segmentation fault (stack overflow or bad memory access)";
            break;
        case SIGFPE:
            errorMsg = "CrashError: Floating point exception";
            break;
        case SIGILL:
            errorMsg = "CrashError: Illegal instruction";
            break;
        case SIGABRT:
            errorMsg = "CrashError: Abort signal (assertion or OOM)";
            break;
        default:
            errorMsg = "CrashError: Unknown signal " + std::to_string(jumpVal);
            break;
        }
    }

    // Restore signal handlers and streams
    signal(SIGSEGV, prevSEGV);
    signal(SIGFPE, prevFPE);
    signal(SIGILL, prevILL);
    signal(SIGABRT, prevABRT);
    std::cout.rdbuf(savedOut);
    std::cerr.rdbuf(savedErr);
    outCapture = sink.str();
    return errorMsg;
}

static TestResult testFile(const std::string &path)
{
    TestResult res;
    res.path = path;

    // ── Read source ──────────────────────────────────────────────────────────
    std::ifstream f(path);
    if (!f.is_open())
    {
        res.error = "Cannot open file";
        return res;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    res.source = ss.str();

    // ── Lex + parse ──────────────────────────────────────────────────────────
    try
    {
        Lexer l(res.source);
        auto tok = l.tokenize();
        Parser p(std::move(tok));
        auto ast = p.parse();
        (void)ast;
    }
    catch (const ParseError &e)
    {
        res.error = "ParseError: " + std::string(e.what());
        res.line = e.line;
        res.col = e.col;
        return res;
    }
    catch (const std::exception &e)
    {
        res.error = "LexError: " + std::string(e.what());
        res.line = 1;
        return res;
    }
    catch (...)
    {
        res.error = "LexError: unknown";
        return res;
    }

    // ── Compile + run (SEH-guarded so a crash can't kill the process) ────────
    std::string sehError;
    try
    {
        sehError = runVmGuarded(res.source, path, res.output);
    }
    catch (const ParseError &e)
    {
        if (!isInputDriven(e.what()))
        {
            res.error = "ParseError: " + std::string(e.what());
            res.line = e.line;
        }
    }
    catch (const QuantumError &e)
    {
        if (!isInputDriven(e.what()))
        {
            res.error = e.kind + ": " + std::string(e.what());
            res.line = e.line;
        }
    }
    catch (const std::exception &e)
    {
        if (!isInputDriven(e.what()))
            res.error = "Fatal: " + std::string(e.what());
    }
    catch (...)
    {
        res.error = "Fatal: unknown exception";
    }

    // SEH error takes priority if set
    if (!sehError.empty())
    {
        res.error = sehError;
        res.crashed = true;
    }

    res.passed = res.error.empty();
    return res;
}

static void collectSaFiles(const fs::path &dir, std::vector<fs::path> &out)
{
    if (!fs::exists(dir) || !fs::is_directory(dir))
        return;
    for (auto &e : fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied))
        if (e.is_regular_file() && e.path().extension() == ".sa")
            out.push_back(e.path());
}

// ── Write test_results.txt ────────────────────────────────────────────────────
// • All files listed (PASS / FAIL)
// • For every FAIL: error, location, captured output, and the FULL source code
// ── Progressive report — written incrementally so crashes don't lose results ──
static std::ofstream g_reportStream;
static int g_reportPassed = 0;
static int g_reportFailed = 0;
static int g_reportTotal = 0;

static void openProgressiveReport(const std::string &dir, int totalFiles)
{
    fs::path rp = fs::path(dir) / "test_results.txt";
    g_reportStream.open(rp);
    g_reportTotal = totalFiles;

    if (!g_reportStream.is_open())
        return;

    g_reportStream << "Quantum Language — Test Results (in progress)\n";
    g_reportStream << "Generated : ";
    {
        std::time_t t = std::time(nullptr);
        char buf[64];
        struct tm tm_i;
        localtime_s(&tm_i, &t);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_i);
        g_reportStream << buf;
    }
    g_reportStream << "\nDirectory : " << fs::absolute(fs::path(dir)).string() << "\n";
    g_reportStream << "Total     : " << totalFiles << "   (running...)\n";
    g_reportStream << std::string(72, '=') << "\n\n";
    g_reportStream.flush();
}

static void appendResultToReport(const TestResult &r)
{
    if (!g_reportStream.is_open())
        return;

    if (r.passed)
    {
        ++g_reportPassed;
        g_reportStream << "[PASS] " << r.path << "\n\n";
        g_reportStream.flush();
        return;
    }

    ++g_reportFailed;

    g_reportStream << "[FAIL] " << r.path << "\n";
    g_reportStream << std::string(72, '-') << "\n";
    g_reportStream << "Error  : " << r.error << "\n";
    if (r.line > 0)
    {
        g_reportStream << "Line   : " << r.line;
        if (r.col > 0)
            g_reportStream << "   Col : " << r.col;
        g_reportStream << "\n";
    }
    if (r.crashed)
        g_reportStream << "Note   : Process-level crash — SEH exception caught\n";

    if (!r.output.empty())
    {
        g_reportStream << "\n--- Program Output ---\n";
        std::istringstream os(r.output);
        std::string ln;
        while (std::getline(os, ln))
            g_reportStream << "  " << ln << "\n";
        g_reportStream << "--- End Output ---\n";
    }

    // Full numbered source with error-line marker
    g_reportStream << "\n--- Source Code (" << r.path << ") ---\n";
    {
        std::istringstream src(r.source);
        std::string ln;
        int lineNo = 1;
        while (std::getline(src, ln))
        {
            if (r.line > 0 && lineNo == r.line)
                g_reportStream << ">>> ";
            else
                g_reportStream << "    ";
            g_reportStream << std::setw(4) << lineNo++ << " | " << ln << "\n";
        }
    }
    g_reportStream << "--- End Source ---\n\n";
    g_reportStream << std::string(72, '=') << "\n\n";
    g_reportStream.flush();
}

static void finalizeReport(const std::string &dir)
{
    if (!g_reportStream.is_open())
        return;

    int total = g_reportPassed + g_reportFailed;
    g_reportStream << std::string(72, '=') << "\n";
    if (g_reportFailed == 0)
        g_reportStream << "Result: ALL PASSED (" << total << "/" << g_reportTotal << ")\n";
    else
        g_reportStream << "Result: FAILED " << g_reportFailed
                       << "/" << g_reportTotal << " files\n";
    g_reportStream << "Passed : " << g_reportPassed
                   << "   Failed : " << g_reportFailed
                   << "   Total : " << g_reportTotal << "\n";
    g_reportStream.close();

    fs::path rp = fs::path(dir) / "test_results.txt";
    std::cout << Colors::CYAN << "  Report  : " << Colors::RESET
              << fs::absolute(rp).string() << "\n";
}

static int runTestExamples(const std::string &dir)
{
    fs::path d(dir);
    if (!fs::exists(d) || !fs::is_directory(d))
    {
        std::cerr << Colors::RED << "[Error] " << Colors::RESET
                  << "Not found: " << dir << "\n";
        return 1;
    }

    redirectStdinToNull();
    g_testMode = true;

    std::vector<fs::path> files;
    collectSaFiles(d, files);
    if (files.empty())
    {
        std::cout << "No .sa files found.\n";
        return 0;
    }
    std::sort(files.begin(), files.end());

    const int total = (int)files.size();

    std::cout << Colors::CYAN << Colors::BOLD
              << "\n═══════════════ Quantum Test Runner ═══════════════\n"
              << Colors::RESET
              << "  Directory : " << fs::absolute(d).string() << "\n"
              << "  Files     : " << total << "\n\n";
    std::cout.flush();

    // Open the report file immediately — results are streamed in as they finish
    // so even if the process crashes partway through, we have a partial report.
    openProgressiveReport(dir, total);

    int passed = 0;

    for (int i = 0; i < total; ++i)
    {
        const fs::path &fp = files[i];
        std::string ps = fp.string();
        std::string disp = ps;
        try
        {
            disp = fs::relative(fp).string();
        }
        catch (...)
        {
        }

        // Progress counter so the user can see we haven't hung
        std::cout << Colors::CYAN << "  [" << std::setw(3) << (i + 1)
                  << "/" << total << "] " << Colors::RESET << disp << " ... ";
        std::cout.flush();

        TestResult tr = testFile(ps);
        tr.path = disp;

        if (tr.passed)
        {
            std::cout << Colors::GREEN << "PASS\n"
                      << Colors::RESET;
            ++passed;
        }
        else
        {
            std::cout << Colors::RED << "FAIL\n"
                      << Colors::RESET;
            if (tr.line > 0)
            {
                std::cout << "            Line " << tr.line;
                if (tr.col > 0)
                    std::cout << ", Col " << tr.col;
                std::cout << "\n";
            }
            std::cout << "            " << Colors::RED << tr.error
                      << Colors::RESET << "\n";
            if (tr.crashed)
                std::cout << "            "
                          << Colors::YELLOW << "(process-level crash caught — continuing)\n"
                          << Colors::RESET;
        }
        std::cout.flush();

        appendResultToReport(tr);
    }

    int failed = total - passed;

    // ── Console summary ───────────────────────────────────────────────────────
    std::cout << "\n"
              << std::string(51, '=') << "\n";
    if (failed == 0)
        std::cout << Colors::GREEN << "  ✓ All " << total << " files passed!\n"
                  << Colors::RESET;
    else
        std::cout << Colors::GREEN << "  ✓ " << passed << " passed  "
                  << Colors::RED << "✗ " << failed << " failed"
                  << "  (total " << total << ")\n"
                  << Colors::RESET;

    finalizeReport(dir);

    return failed > 0 ? 1 : 0;
}

// ─── REPL ─────────────────────────────────────────────────────────────────────

static void runREPL(bool debug = false)
{
    printBanner();
    std::cout << Colors::GREEN << "  REPL — type 'exit' to quit\n"
              << Colors::RESET << "\n";
    VM vm;
    int n = 1;
    std::string line;
    while (true)
    {
        std::cout << Colors::CYAN << "quantum[" << n++ << "]> " << Colors::RESET;
        if (!std::getline(std::cin, line))
            break;
        if (line == "exit" || line == "quit")
            break;
        if (line.empty())
            continue;
        try
        {
            vm.run(compileSource(line, "<repl>", debug));
        }
        catch (const ParseError &e)
        {
            std::cerr << Colors::RED << "[ParseError] " << Colors::RESET << e.what() << "\n";
        }
        catch (const QuantumError &e)
        {
            std::cerr << Colors::RED << "[" << e.kind << "] " << Colors::RESET << e.what() << "\n";
        }
        catch (const std::exception &e)
        {
            std::cerr << Colors::RED << "[Error] " << Colors::RESET << e.what() << "\n";
        }
    }
    std::cout << Colors::YELLOW << "\n  Goodbye! 👋\n"
              << Colors::RESET;
}

// ─── printHelp ────────────────────────────────────────────────────────────────

static void printHelp(const char *prog)
{
    std::cout << Colors::BOLD << "Usage:\n"
              << Colors::RESET
              << "  " << prog << " <file.sa>          Compile → <file>.exe then run it\n"
              << "  " << prog << " --run <file.sa>    Interpret directly (no .exe)\n"
              << "  " << prog << " --check <file.sa>  Parse + type-check only\n"
              << "  " << prog << " --debug <file.sa>  Dump bytecode then run\n"
              << "  " << prog << " --dis   <file.sa>  Dump bytecode only\n"
              << "  " << prog << " --test  [dir]      Batch-test all .sa files\n"
              << "  qrun <file.sa>              Interpret directly (no .exe)\n\n"
              << "  quantum hello.sa            → hello.exe created and run\n"
              << "  qrun    hello.sa            → interpreted directly\n";
}

// ─── findStubPath ─────────────────────────────────────────────────────────────
// Searches for quantum_stub.exe next to quantum.exe (or in build/ subdirs).
// All messages go to stdout so the user always sees them.

static std::string findStubPath(const std::string &quantumExePath)
{
    fs::path base = fs::path(quantumExePath).parent_path();

    std::vector<fs::path> candidates = {
        base / "quantum_stub.exe",
        base / "build" / "quantum_stub.exe",
        base / "build" / "Release" / "quantum_stub.exe",
        base / "build" / "Debug" / "quantum_stub.exe",
    };

    for (auto &p : candidates)
    {
        if (fs::exists(p))
            return p.string();
    }

    // Nothing found — tell the user exactly where we looked
    std::cout << Colors::RED << "[Error] " << Colors::RESET
              << "quantum_stub.exe not found. Searched:\n";
    for (auto &p : candidates)
        std::cout << "  " << p.string() << "\n";
    std::cout << "Run build.bat to rebuild all three binaries.\n";
    return "";
}

// ─── bundleAndRun ─────────────────────────────────────────────────────────────
// Compiles .sa → bytecode, appends it to a copy of quantum_stub.exe,
// writes <name>.exe next to the .sa file, then launches it and waits.
static int bundleAndRun(const std::string &path, const std::string &exePath)
{
    // 1. Read source [cite: 703]
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

    // ─── HOOK VALIDATOR HERE ───
    validateParadigmExtensions(path, ss.str());

    // 2. Compile [cite: 714]
    std::shared_ptr<Chunk> chunk;
    try
    {
        chunk = compileSource(ss.str(), path, false);
    }
    catch (const ParseError &e)
    {
        std::cout << Colors::RED << Colors::BOLD << "\n   X ParseError" << Colors::RESET
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
    // 3. Serialize bytecode [cite: 734]
    auto payload = Serializer::serialize(chunk);
    uint32_t payloadSize = (uint32_t)payload.size();
    // 4. Find quantum_stub.exe (the template runtime) [cite: 737]
    std::string stub = findStubPath(exePath);
    if (stub.empty())
    {
        std::cout.flush();
        return 1;
    }
    // 5. Determine output path: hello.sa → hello.exe [cite: 744]
    fs::path srcPath(path);
    std::string outName;
    if (srcPath.parent_path().empty())
        outName = (fs::current_path() / srcPath.stem()).string() + ".exe";
    else
        outName = (srcPath.parent_path() / srcPath.stem()).string() + ".exe";
    // Safety: never overwrite quantum.exe, qrun.exe, or quantum_stub.exe [cite: 751]
    {
        std::string stemLower = fs::path(outName).stem().string();
        std::transform(stemLower.begin(), stemLower.end(), stemLower.begin(), ::tolower);
        if (stemLower == "quantum" || stemLower == "qrun" || stemLower == "quantum_stub")
            outName = (fs::path(outName).parent_path() /
                      (fs::path(outName).stem().string() + "_out"))
                          .string() +
                      ".exe";
    }
    // 6. Copy stub → output exe [cite: 761]
    std::error_code copyErr;
    fs::copy_file(stub, outName, fs::copy_options::overwrite_existing, copyErr);
    if (copyErr)
    {
        std::cout << Colors::RED << "[Error] " << Colors::RESET
                  << "Cannot create " << outName << ": " << copyErr.message() << "\n";
        std::cout.flush();
        return 1;
    }
    // 7. Append payload: [bytes] [size: uint32 LE] [magic: "QNTM_VM!" 8 bytes] [cite: 771]
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
    // 8. Launch the produced .exe and wait for it to finish [cite: 796]
    std::cout << Colors::CYAN << "[Running]  " << Colors::RESET << outName << "\n\n";
    std::cout.flush();
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
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::string exePath = getExecutablePath();

    // ══════════════════════════════════════════════════════════════
    //  STANDALONE BUNDLED EXE  (hello.exe etc.) — quantum_stub mode
    // ══════════════════════════════════════════════════════════════
#if !defined(QRUN_MODE) && !defined(QUANTUM_MODE_COMPILER)
    {
        auto embedded = loadEmbeddedBytecode(exePath);
        if (embedded)
        {
            try
            {
                VM vm;
                vm.run(embedded);
                return 0;
            }
            catch (const QuantumError &e)
            {
                std::cerr << Colors::RED << "[" << e.kind << "] " << Colors::RESET << e.what() << "\n";
                return 1;
            }
            catch (const std::exception &e)
            {
                std::cerr << Colors::RED << "[Fatal] " << Colors::RESET << e.what() << "\n";
                return 1;
            }
        }
        // No embedded bytecode — user ran quantum_stub.exe directly
        std::cout << Colors::YELLOW
                  << "[quantum_stub] This is the Quantum standalone runtime.\n"
                  << "  Run:  quantum hello.sa   to compile hello.sa into hello.exe\n"
                  << Colors::RESET;
        return 1;
    }
#endif

    // ══════════════════════════════════════════════════════════════
    //  QRUN MODE  (qrun.exe) — always interpret, never bundle
    // ══════════════════════════════════════════════════════════════
#ifdef QRUN_MODE
    if (argc == 1)
    {
        runREPL();
        return 0;
    }
    std::string a1 = argv[1];
    if (a1 == "--help" || a1 == "-h")
    {
        printBanner();
        printHelp(argv[0]);
        return 0;
    }
    if (a1 == "--version" || a1 == "-v")
    {
        std::cout << "Quantum Language v2.0.0\n";
        return 0;
    }
    if (a1 == "--check" && argc >= 3)
        return checkFile(argv[2]);
    if (a1 == "--debug" && argc >= 3)
    {
        runFile(argv[2], true);
        return 0;
    }
    if (a1 == "--dis" && argc >= 3)
    {
        std::ifstream f(argv[2]);
        std::ostringstream ss;
        ss << f.rdbuf();
        disassembleChunk(*compileSource(ss.str(), argv[2], false), std::cout);
        return 0;
    }
    if (a1 == "--test")
        return runTestExamples(argc >= 3 ? argv[2] : "examples");
    runFile(a1);
    return 0;
#endif

    // ══════════════════════════════════════════════════════════════
    //  QUANTUM COMPILER MODE  (quantum.exe)
    // ══════════════════════════════════════════════════════════════
    if (argc == 1)
    {
        runREPL();
        return 0;
    }

    std::string arg = argv[1];

    if (arg == "--help" || arg == "-h")
    {
        printBanner();
        printHelp(argv[0]);
        return 0;
    }
    if (arg == "--aura")
    {
        printBanner();
        printAura();
        return 0;
    }
    if (arg == "--version" || arg == "-v")
    {
        std::cout << "Quantum Language v2.0.0\nRuntime: Bytecode VM\nBy Muhammad Saad Amin\n";
        return 0;
    }
    if (arg == "--check" && argc >= 3)
        return checkFile(argv[2]);
    if (arg == "--test")
        return runTestExamples(argc >= 3 ? argv[2] : "examples");
    if (arg == "--debug" && argc >= 3)
    {
        runFile(argv[2], true);
        return 0;
    }
    if (arg == "--run" && argc >= 3)
    {
        runFile(argv[2]);
        return 0;
    }
    if (arg == "--dis" && argc >= 3)
    {
        std::ifstream f(argv[2]);
        if (!f.is_open())
        {
            std::cerr << Colors::RED << "[Error] Cannot open: " << argv[2] << "\n";
            return 1;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        try
        {
            disassembleChunk(*compileSource(ss.str(), argv[2], false), std::cout);
        }
        catch (const std::exception &e)
        {
            std::cerr << Colors::RED << "[Error] " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // Default: compile .sa → hello.exe (using quantum_stub as template) → run
    // Default: run based on file extension
    if (arg.size() >= 3 && arg.substr(arg.size() - 3) == ".sa")
    {
        // Only .sa supports Quantum multi-syntax
        return bundleAndRun(arg, exePath);
    }
    else if (arg.size() >= 3 && arg.substr(arg.size() - 3) == ".js")
    {
        // JavaScript syntax check first
        std::string checkCmd = "node --check \"" + arg + "\"";
        int result = system(checkCmd.c_str());

        if (result != 0)
        {
            std::cerr << "[Quantum Error] Invalid JavaScript syntax in " << arg << "\n";
            return result;
        }

        // If syntax is valid, run JS
        std::string runCmd = "node \"" + arg + "\"";
        return system(runCmd.c_str());
    }
    else if (arg.size() >= 2 && arg.substr(arg.size() - 2) == ".c")
    {
        std::string out = arg.substr(0, arg.size() - 2) + ".exe";

        // C compiler will give C syntax errors
        std::string compileCmd = "gcc \"" + arg + "\" -o \"" + out + "\"";
        int result = system(compileCmd.c_str());

        if (result != 0)
        {
            std::cerr << "[Quantum Error] Invalid C syntax in " << arg << "\n";
            return result;
        }

        std::string runCmd = "\"" + out + "\"";
        return system(runCmd.c_str());
    }
    else if (arg.size() >= 4 && arg.substr(arg.size() - 4) == ".cpp")
    {
        std::string out = arg.substr(0, arg.size() - 4) + ".exe";

        // C++ compiler will give C++ syntax errors
        std::string compileCmd = "g++ \"" + arg + "\" -o \"" + out + "\"";
        int result = system(compileCmd.c_str());

        if (result != 0)
        {
            std::cerr << "[Quantum Error] Invalid C++ syntax in " << arg << "\n";
            return result;
        }

        std::string runCmd = "\"" + out + "\"";
        return system(runCmd.c_str());
    }
    else
    {
        std::cerr << "[Error] Unsupported file type: " << arg << "\n";
        std::cerr << "Supported: .sa, .js, .c, .cpp\n";
        return 1;
    }
}