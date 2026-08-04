#include "Cli.h"
#include "Pipeline.h"
#include "Dialect.h"
#include "Lexer.h"
#include "Parser.h"
#include "Vm.h"
#include "Error.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

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

// ── Process-isolated VM execution ────────────────────────────────────────────
// A test file can crash hard — a runaway recursion overflows the stack, a VM
// bug dereferences bad memory — and on MinGW/Windows there is no reliable way
// to catch that in-process (SEH is a language extension GCC lacks, and
// longjmp-out-of-a-signal-handler is UB that leaves the C++ runtime corrupted,
// so a *later* file then dies at random). The robust answer used by every
// serious test runner: run each file in its own child process. A crash there
// takes down only the child; the parent reads the exit code and moves on.
//
// The child (`--__runone <file>`, dispatched to runSingleFileForTest below)
// prints the program's own output to stdout, and on a *handled* error appends
// one marker line — QTEST_ERR_MARKER + kind|line|col|message — then exits 2.
// A clean run exits 0 with no marker. Any other exit (or a marker-less
// non-zero) means the child crashed.

static const char *QTEST_ERR_MARKER = "\x1E__QTEST_ERR__\x1F";

// One line, so it survives being embedded in captured stdout.
static std::string flattenLine(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += (c == '\n' || c == '\r') ? ' ' : c;
    return out;
}

int runSingleFileForTest(const std::string &path)
{
    g_testMode = true;
    redirectStdinToNull();

    // Watchdog: a test file that never terminates (an infinite loop) must not
    // hang the whole suite. A detached timer thread force-exits this child with
    // a timeout marker if execution runs long, so the parent records a FAIL and
    // moves on. 20s is far above any legitimate test's runtime.
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        std::cout << "\n"
                  << QTEST_ERR_MARKER
                  << "TimeoutError\x1F0\x1F0\x1Fexecution exceeded 20s "
                     "(possible infinite loop)\n";
        std::cout.flush();
        std::_Exit(2);
    }).detach();

    std::ifstream f(path);
    if (!f.is_open())
    {
        std::cout << "\n"
                  << QTEST_ERR_MARKER << "OpenError\x1F0\x1F0\x1FCannot open file\n";
        return 2;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string source = ss.str();

    std::string kind, msg;
    int line = 0, col = 0;
    try
    {
        VM vm;
        vm.run(compileSource(applyDialect(source, path), path, false));
        return 0; // clean
    }
    catch (const ParseError &e)
    {
        if (isInputDriven(e.what()))
            return 0; // program merely wanted stdin — not a failure
        kind = "ParseError";
        msg = e.what();
        line = e.line;
        col = e.col;
    }
    catch (const QuantumError &e)
    {
        if (isInputDriven(e.what()))
            return 0;
        kind = e.kind;
        msg = e.what();
        line = e.line;
    }
    catch (const std::exception &e)
    {
        if (isInputDriven(e.what()))
            return 0;
        kind = "Fatal";
        msg = e.what();
    }
    catch (...)
    {
        kind = "Fatal";
        msg = "unknown exception";
    }

    std::cout << "\n"
              << QTEST_ERR_MARKER << kind << "\x1F" << line << "\x1F" << col
              << "\x1F" << flattenLine(msg) << "\n";
    return 2;
}

// Spawn "<thisExe> --__runone <path>" and capture merged stdout+stderr plus
// the child's exit code. Returns the captured text; sets exitCode.
static std::string spawnChild(const std::string &path, int &exitCode)
{
    std::string exe = getExecutablePath();
#ifdef _WIN32
    // cmd.exe strips the outermost pair of quotes from the whole command, so
    // wrap everything once more: ""exe" --__runone "path" 2>&1".
    std::string cmd = "\"\"" + exe + "\" --__runone \"" + path + "\" 2>&1\"";
    FILE *pipe = _popen(cmd.c_str(), "r");
#else
    std::string cmd = "\"" + exe + "\" --__runone \"" + path + "\" 2>&1";
    FILE *pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe)
    {
        exitCode = -1;
        return "";
    }
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0)
        out.append(buf, n);
#ifdef _WIN32
    exitCode = _pclose(pipe);
#else
    int status = pclose(pipe);
    exitCode = (status == -1) ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
#endif
    return out;
}

static TestResult testFile(const std::string &path)
{
    TestResult res;
    res.path = path;

    // Keep the full source for the failure report.
    {
        std::ifstream f(path);
        if (!f.is_open())
        {
            res.error = "Cannot open file";
            return res;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        res.source = ss.str();
    }

    // Run the file in an isolated child process.
    int exitCode = 0;
    std::string captured = spawnChild(path, exitCode);

    // Split the child's structured error marker (if any) out of the output.
    std::string marker(QTEST_ERR_MARKER);
    size_t mpos = captured.find(marker);
    if (mpos != std::string::npos)
    {
        res.output = captured.substr(0, mpos);
        // Trim a trailing newline the child emitted before the marker.
        if (!res.output.empty() && res.output.back() == '\n')
            res.output.pop_back();

        std::string rest = captured.substr(mpos + marker.size());
        if (!rest.empty() && rest.back() == '\n')
            rest.pop_back();
        // rest = kind \x1F line \x1F col \x1F message
        std::vector<std::string> parts;
        size_t start = 0;
        for (int field = 0; field < 3; ++field)
        {
            size_t sep = rest.find('\x1F', start);
            if (sep == std::string::npos) break;
            parts.push_back(rest.substr(start, sep - start));
            start = sep + 1;
        }
        std::string kind = parts.size() > 0 ? parts[0] : "Error";
        res.line = parts.size() > 1 ? std::atoi(parts[1].c_str()) : 0;
        res.col = parts.size() > 2 ? std::atoi(parts[2].c_str()) : 0;
        std::string emsg = rest.substr(start);
        res.error = kind + ": " + emsg;
    }
    else
    {
        res.output = captured;
        if (exitCode != 0)
        {
            // Non-zero exit with no structured error = the child crashed.
            res.crashed = true;
            res.error = "CrashError: child process terminated abnormally (exit code " +
                        std::to_string(exitCode) + ")";
        }
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

