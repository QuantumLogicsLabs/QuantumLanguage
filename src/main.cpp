 /* Quantum Language v2.0.0 — Bytecode VM
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
#include <cctype>
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

// ─── Dialect support ──────────────────────────────────────────────────────────
// .js / .py / .rb / .c / .cpp files run natively on the Quantum VM through its
// multi-syntax front-end — no node/python/gcc/g++ required.

static std::string fileExtLower(const std::string &path)
{
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

static bool hasSupportedExt(const std::string &path)
{
    std::string ext = fileExtLower(path);
    return ext == ".sa" || ext == ".js" || ext == ".py" ||
           ext == ".rb" || ext == ".c" || ext == ".cpp";
}

// True if the source defines a function named main ("main" followed by "(",
// not part of a longer identifier).
static bool definesMainFunction(const std::string &src)
{
    size_t p = 0;
    while ((p = src.find("main", p)) != std::string::npos)
    {
        bool leftOk = (p == 0) ||
                      (!std::isalnum((unsigned char)src[p - 1]) && src[p - 1] != '_');
        size_t q = p + 4;
        while (q < src.size() && std::isspace((unsigned char)src[q]))
            q++;
        if (leftOk && q < src.size() && src[q] == '(')
            return true;
        p += 4;
    }
    return false;
}

// Remove leading/trailing spaces while preserving the original line indentation
// separately. This is used only by the lightweight Ruby dialect normalizer.
static std::string trimCopy(const std::string &value)
{
    size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static bool startsWith(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

// Ruby-style syntax is translated into the brace-based common syntax already
// accepted by the Quantum lexer/parser. This intentionally implements a Ruby
// subset; it does not embed or invoke the official Ruby interpreter.
//
// Supported normalizations include:
//   puts expression       -> print(expression)
//   print expression      -> print(expression)
//   # comment             -> // comment
//   if/elsif/else/end     -> brace-based conditionals
//   unless condition      -> if (!(condition))
//   while/until/end       -> brace-based loops
//   def name(args)/end    -> function name(args) { ... }
static std::string applyRubyDialect(const std::string &source)
{
    std::istringstream input(source);
    std::ostringstream output;
    std::string line;

    while (std::getline(input, line))
    {
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos)
        {
            output << "\n";
            continue;
        }

        std::string indentation = line.substr(0, first);
        std::string code = trimCopy(line.substr(first));

        // Full-line Ruby comments. Inline # comments are left unchanged so a
        // # inside a string or interpolation-like text is never corrupted.
        if (!code.empty() && code[0] == '#')
        {
            output << indentation << "//" << code.substr(1) << "\n";
            continue;
        }

        // puts(...) and puts expression
        if (startsWith(code, "puts("))
        {
            output << indentation << "print" << code.substr(4) << "\n";
            continue;
        }
        if (startsWith(code, "puts "))
        {
            output << indentation << "print(" << trimCopy(code.substr(5)) << ")\n";
            continue;
        }

        // Ruby print expression (print(...) already matches the common syntax).
        if (startsWith(code, "print "))
        {
            output << indentation << "print(" << trimCopy(code.substr(6)) << ")\n";
            continue;
        }

        if (startsWith(code, "elsif "))
        {
            output << indentation << "} else if ("
                   << trimCopy(code.substr(6)) << ") {\n";
            continue;
        }
        if (code == "else")
        {
            output << indentation << "} else {\n";
            continue;
        }
        if (code == "end")
        {
            output << indentation << "}\n";
            continue;
        }

        if (startsWith(code, "unless "))
        {
            output << indentation << "if (!("
                   << trimCopy(code.substr(7)) << ")) {\n";
            continue;
        }
        if (startsWith(code, "if "))
        {
            output << indentation << "if ("
                   << trimCopy(code.substr(3)) << ") {\n";
            continue;
        }

        if (startsWith(code, "until "))
        {
            output << indentation << "while (!("
                   << trimCopy(code.substr(6)) << ")) {\n";
            continue;
        }
        if (startsWith(code, "while "))
        {
            output << indentation << "while ("
                   << trimCopy(code.substr(6)) << ") {\n";
            continue;
        }

        if (startsWith(code, "def "))
        {
            std::string signature = trimCopy(code.substr(4));
            output << indentation << "function " << signature << " {\n";
            continue;
        }

        // Common Ruby literals mapped to the Quantum common representation.
        if (code == "nil")
            code = "null";

        output << indentation << code << "\n";
    }

    return output.str();
}

// C/C++ programs only define main() — append a call so the program executes
// after its top-level declarations load. Ruby files are first normalized into
// the common Quantum syntax and then use the normal compiler/VM pipeline.
static std::string applyDialect(std::string source, const std::string &path)
{
    std::string ext = fileExtLower(path);

    if (ext == ".rb")
        source = applyRubyDialect(source);

    if ((ext == ".c" || ext == ".cpp") && definesMainFunction(source))
        source += "\nmain()\n";

    return source;
}

// ─── runFile — interpret a source file in-place (no exe created) ──────────────

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

    try
    {
        VM vm;
        vm.run(compileSource(applyDialect(ss.str(), path), path, debug));
    }
    catch (const ParseError &e)
    {
        std::cerr << Colors::RED << Colors::BOLD
                  << "\n  X ParseError" << Colors::RESET
                  << " in " << path << " at line " << e.line << ":" << e.col
                  << "\n    " << e.what() << "\n\n";
        std::exit(1);
    }
    catch (const QuantumError &e)
    {
        std::cerr << Colors::RED << Colors::BOLD
                  << "\n  X " << e.kind << Colors::RESET;
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
        Lexer l(applyDialect(ss.str(), path));
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
            vm.run(compileSource(applyDialect(source, path), path, false));
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

static void collectTestFiles(const fs::path &dir, std::vector<fs::path> &out)
{
    // Any file type the compiler runs natively is testable:
    // .sa .js .py .rb .c .cpp — all share the same multi-syntax pipeline.
    if (!fs::exists(dir) || !fs::is_directory(dir))
        return;
    for (auto &e : fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied))
        if (e.is_regular_file() && hasSupportedExt(e.path().string()))
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
    collectTestFiles(d, files);
    if (files.empty())
    {
        std::cout << "No testable files found (.sa .js .py .rb .c .cpp).\n";
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
              << "  " << prog << " <file>             Compile → <file>.exe then run it\n"
              << "  " << prog << " --run <file>       Interpret directly (no .exe)\n"
              << "  " << prog << " --check <file>     Parse + type-check only\n"
              << "  " << prog << " --debug <file>     Dump bytecode then run\n"
              << "  " << prog << " --dis   <file>     Dump bytecode only\n"
              << "  " << prog << " --test  [dir]      Batch-test all supported source files\n"
              << "  qrun <file>                 Interpret directly (no .exe)\n\n"
              << "  Supported files: .sa .js .py .rb .c .cpp — all run natively on the\n"
              << "  Quantum VM (multi-syntax subset; node/python/gcc NOT required)\n\n"
              << "  quantum hello.sa            → hello.exe created and run\n"
              << "  quantum prog.c              → prog.exe created and run (no gcc)\n"
              << "  qrun    hello.py            → interpreted directly\n"
              << "  qrun    hello.rb            → Ruby-style subset interpreted directly\n";
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
// Compiles a source file (.sa/.js/.py/.rb/.c/.cpp) → bytecode, appends it to a
// copy of quantum_stub.exe, writes <name>.exe next to the source file,
// then launches it and waits.

static int bundleAndRun(const std::string &path, const std::string &exePath)
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
        outName = (fs::current_path() / srcPath.stem()).string() + ".exe";
    else
        outName = (srcPath.parent_path() / srcPath.stem()).string() + ".exe";

    // Safety: never overwrite quantum.exe, qrun.exe, or quantum_stub.exe
    {
        std::string stemLower = fs::path(outName).stem().string();
        std::transform(stemLower.begin(), stemLower.end(), stemLower.begin(), ::tolower);
        if (stemLower == "quantum" || stemLower == "qrun" || stemLower == "quantum_stub")
            outName = (fs::path(outName).parent_path() /
                       (fs::path(outName).stem().string() + "_out"))
                          .string() +
                      ".exe";
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
        disassembleChunk(*compileSource(applyDialect(ss.str(), argv[2]), argv[2], false), std::cout);
        return 0;
    }
    if (a1 == "--test")
        return runTestExamples(argc >= 3 ? argv[2] : "examples");
    // Any supported source file runs natively on the Quantum VM —
    // .js/.py/.rb/.c/.cpp go through the same multi-syntax front-end as .sa,
    // so no node/python/gcc/g++ is required.
    if (hasSupportedExt(a1))
    {
        runFile(a1);
        return 0;
    }
    else
    {
        std::cerr << "[Error] Unsupported file type: " << a1 << "\n";
        std::cerr << "Supported: .sa, .js, .py, .rb, .c, .cpp (run natively on the Quantum VM)\n";
        return 1;
    }
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
            disassembleChunk(*compileSource(applyDialect(ss.str(), argv[2]), argv[2], false), std::cout);
        }
        catch (const std::exception &e)
        {
            std::cerr << Colors::RED << "[Error] " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // Default: compile any supported source file → <file>.exe → run.
    // .js/.py/.rb/.c/.cpp compile through the same multi-syntax front-end as .sa,
    // so no node/python/gcc/g++ is required and the produced .exe is standalone.
    if (hasSupportedExt(arg))
        return bundleAndRun(arg, exePath);

    std::cerr << "[Error] Unsupported file type: " << arg << "\n";
    std::cerr << "Supported: .sa, .js, .py, .rb, .c, .cpp (run natively on the Quantum VM)\n";
    return 1;
}