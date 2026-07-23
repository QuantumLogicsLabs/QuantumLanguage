#include "Cli.h"
#include "Pipeline.h"
#include "Dialect.h"
#include "Lexer.h"
#include "Parser.h"
#include "Vm.h"
#include "Error.h"

#include <algorithm>
#include <csignal>
#include <csetjmp>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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
#ifdef _WIN32
    FILE *n = nullptr;
    freopen_s(&n, "NUL", "r", stdin);
#else
    if (!freopen("/dev/null", "r", stdin)) { /* ignore */ }
#endif
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
    // Dialect-translate first (.rb/.c/.cpp) — parsing the raw source here would
    // reject valid Ruby/C/C++ files before the real (dialect-aware) run below.
    try
    {
        Lexer l(applyDialect(res.source, path));
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

// Lists one directory level, then recurses into its subdirectories
// separately (rather than using a single fs::recursive_directory_iterator).
// This matters because a nested checkout's .git/ directory (long hashed
// object paths, reparse points) can throw a filesystem_error that isn't a
// plain permission-denied — recursive_directory_iterator has no way to skip
// just that one subtree, so the error silently aborts every sibling
// directory that would have been visited afterward in the same lazy walk.
// Gathering this level's subdirectories up front and recursing into each
// independently means a failure in one subtree only loses that subtree.
static void collectTestFilesRecursive(const fs::path &dir, std::vector<fs::path> &out)
{
    std::vector<fs::path> subdirs;
    try
    {
        for (auto &e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied))
        {
            if (e.is_directory())
                subdirs.push_back(e.path());
            else if (e.is_regular_file() && hasSupportedExt(e.path().string()))
                out.push_back(e.path());
        }
    }
    catch (const fs::filesystem_error &)
    {
        return; // this subtree is unreadable — skip it, siblings are unaffected
    }
    for (auto &sub : subdirs)
        collectTestFilesRecursive(sub, out);
}

static void collectTestFiles(const fs::path &dir, std::vector<fs::path> &out)
{
    // Any file type the compiler runs natively is testable:
    // .sa .js .py .rb .c .cpp — all share the same multi-syntax pipeline.
    if (!fs::exists(dir) || !fs::is_directory(dir))
        return;
    collectTestFilesRecursive(dir, out);
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
#ifdef _WIN32
        localtime_s(&tm_i, &t);
#else
        localtime_r(&t, &tm_i);
#endif
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

int runTestExamples(const std::string &dir)
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

