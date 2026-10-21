#include "Cli.h"
#include "Pipeline.h"
#include "Dialect.h"
#include "Lexer.h"
#include "Parser.h"
#include "TypeChecker.h"
#include "Vm.h"
#include "Error.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

// ─── Banner ───────────────────────────────────────────────────────────────────

void printBanner()
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

void printAura()
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
// ─── ParseError report ────────────────────────────────────────────────────────

namespace
{
    std::string lineAt(const std::string &text, int n)
    {
        std::istringstream in(text);
        std::string line;
        for (int i = 1; std::getline(in, line); ++i)
            if (i == n)
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return line;
            }
        return std::string();
    }

    // The 1-based line of `text` equal to `code`, if exactly one is.
    int uniqueLine(const std::string &text, const std::string &code)
    {
        std::istringstream in(text);
        std::string line;
        int found = 0;
        for (int i = 1; std::getline(in, line); ++i)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line == code)
            {
                if (found)
                    return 0;
                found = i;
            }
        }
        return found;
    }

    // A line spliced in from an #include: the project file that holds it.
    // Searches the C/C++ sources of the project around `path` (up to its
    // CMakeLists.txt / Makefile / package.json / .git folder).
    bool findSourceLine(const std::string &path, const std::string &code, std::string &file, int &line)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dir = fs::absolute(fs::u8path(path), ec).parent_path();
        for (int up = 0; up < 6; ++up)
        {
            bool root = false;
            for (const char *m : {"CMakeLists.txt", "Makefile", "package.json", "package.qon", ".git"})
                root = root || fs::exists(dir / m, ec);
            if (root || dir.parent_path() == dir)
                break;
            dir = dir.parent_path();
        }
        int scanned = 0;
        fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end && scanned < 2000; it.increment(ec))
        {
            if (ec)
                break;
            const fs::path p = it->path();
            std::string name = p.filename().string();
            if (it->is_directory(ec))
            {
                if (name.empty() || name[0] == '.' || name == "build" || name == "node_modules" || name == "quantum_qubits")
                    it.disable_recursion_pending();
                continue;
            }
            std::string ext = fileExtLower(name);
            if (ext != ".h" && ext != ".hpp" && ext != ".hh" && ext != ".c" && ext != ".cpp" && ext != ".cc" &&
                ext != ".sa")
                continue;
            ++scanned;
            std::ifstream in(p, std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            if (int at = uniqueLine(text.str(), code))
            {
                file = fs::relative(p, fs::current_path(), ec).string();
                if (ec || file.empty())
                    file = p.string();
                line = at;
                return true;
            }
        }
        return false;
    }
}

namespace
{
    // Where a ParseError really is: file, line, and the code on that line.
    // `fromInclude` = spliced in from an #include we couldn't pin down.
    struct ErrorSite
    {
        std::string file, code;
        int line = 0;
        bool fromInclude = false;
    };

    ErrorSite locate(const std::string &path, const std::string &written, const std::string &compiled,
                     const ParseError &e)
    {
        ErrorSite at;
        at.file = path;
        at.line = e.line;
        at.code = lineAt(compiled.empty() ? written : compiled, e.line);
        if (!compiled.empty() && compiled != written && at.code.find_first_not_of(" \t") != std::string::npos)
        {
            if (int n = uniqueLine(written, at.code))
                at.line = n;
            else if (lineAt(written, e.line) != at.code)
                at.fromInclude = !findSourceLine(path, at.code, at.file, at.line);
        }
        return at;
    }
}

void printParseError(std::ostream &os, const std::string &path, const std::string &written,
                     const std::string &compiled, const ParseError &e)
{
    const ErrorSite at = locate(path, written, compiled, e);
    const std::string &code = at.code;
    const int line = at.line;
    const bool fromInclude = at.fromInclude;
    os << Colors::RED << Colors::BOLD << "\n  X ParseError" << Colors::RESET << " in " << at.file;
    if (fromInclude)
        os << " (in code it #includes)";
    else
        os << " at line " << line << ":" << e.col;
    os << "\n    " << e.what() << "\n";
    if (!code.empty() && code.size() < 300)
    {
        std::string gutter = fromInclude ? std::string("    ") : std::to_string(line);
        os << "\n  " << gutter << " | " << code << "\n  " << std::string(gutter.size(), ' ') << " | ";
        for (int i = 1; i < e.col && i <= (int)code.size(); ++i)
            os << (code[i - 1] == '\t' ? '\t' : ' ');
        os << Colors::RED << "^" << Colors::RESET << "\n";
    }
    os << "\n";
}

// ─── runFile — interpret a source file in-place (no exe created) ──────────────

bool g_stageMarker = false;

std::vector<char *> takeGlobalFlags(int argc, char *argv[])
{
    std::vector<char *> rest;
    bool flagsDone = false;
    for (int i = 0; i < argc; ++i)
    {
        std::string a = argv[i];
        if (i > 0 && !flagsDone && a == "--strict")
            g_strictMode = true;
        else if (i > 0 && !flagsDone && a == "--stage-marker")
            g_stageMarker = true;
        else
        {
            if (i > 0 && hasSupportedExt(a))
                flagsDone = true; // the script's own arguments follow
            rest.push_back(argv[i]);
        }
    }
    rest.push_back(nullptr);
    return rest;
}

void runFile(const std::string &path, bool debug)
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

    std::string compiled;
    try
    {
        compiled = applyDialect(ss.str(), path);
        auto chunk = compileSource(compiled, path, debug);
        if (g_stageMarker)
        {
            std::cerr << kStageMarker;
            std::cerr.flush();
        }
        VM vm;
        vm.run(chunk);
    }
    catch (const ParseError &e)
    {
        printParseError(std::cerr, path, ss.str(), compiled, e);
        std::exit(1);
    }
    catch (const CompileError &e)
    {
        std::cerr << e.what();
        std::cerr.flush();
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

int checkFile(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        std::cerr << path << ":1:1: error: Cannot open\n";
        return 1;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    // C/C++: whatever compileSource would use — the typed front-end when the
    // program is within its subset
    const std::string ext = fileExtLower(path);
    if ((ext == ".cpp" || ext == ".c" || ext == ".cc" || ext == ".cxx" || ext == ".java") && !std::getenv("QUANTUM_OLD_CPP"))
    {
        try
        {
            compileSource(ss.str(), path, false);
            std::cout << Colors::GREEN << "[OK] " << Colors::RESET << path << "\n";
            return 0;
        }
        catch (const CompileError &e)
        {
            std::cerr << e.what();
            return 1;
        }
        catch (const QuantumError &e)
        {
            std::cerr << path << ":" << (e.line > 0 ? e.line : 1) << ":1: error: " << e.what() << "\n";
            return 1;
        }
        catch (const ParseError &)
        {
            // the fallback pipeline's own report follows
        }
    }
    std::string compiled;
    try
    {
        compiled = applyDialect(ss.str(), path);
        Lexer l(compiled);
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
        const ErrorSite at = locate(path, ss.str(), compiled, e);
        std::cerr << at.file << ":" << at.line << ":" << e.col << ": error: " << e.what()
                  << (at.fromInclude ? " (in code it #includes)" : "") << "\n";
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << path << ":1:1: error: " << e.what() << "\n";
        return 1;
    }
}
// ─── printHelp ────────────────────────────────────────────────────────────────

void printHelp(const char *prog)
{
    std::cout << Colors::BOLD << "Usage:\n"
              << Colors::RESET
              << "  " << prog << " <file>             Compile → <file>.exe then run it\n"
              << "  " << prog << " --run <file>       Interpret directly (no .exe)\n"
              << "  " << prog << " --check <file>     Parse + type-check only\n"
              << "  " << prog << " --debug <file>     Dump bytecode then run\n"
              << "  " << prog << " --dis   <file>     Dump bytecode only\n"
              << "  " << prog << " --test  [dir]      Batch-test all supported source files\n"
              << "  " << prog << " build [name] [--run]  Build a C/C++ project natively (= qpm build)\n"
              << "  qrun <file>                 Interpret directly (no .exe)\n\n"
              << "  Supported files: .sa .js .py .rb .c .cpp .java — all run natively on the\n"
              << "  Quantum VM (multi-syntax subset; node/python/gcc NOT required)\n\n"
              << "  quantum hello.sa            → hello.exe created and run\n"
              << "  quantum prog.c              → prog.exe created and run (no gcc)\n"
              << "  qrun    hello.py            → interpreted directly\n"
              << "  qrun    hello.rb            → Ruby-style subset interpreted directly\n";
}
