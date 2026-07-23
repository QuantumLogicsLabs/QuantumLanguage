#include "Cli.h"
#include "Pipeline.h"
#include "Dialect.h"
#include "Lexer.h"
#include "Parser.h"
#include "TypeChecker.h"
#include "Vm.h"
#include "Error.h"

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
// ─── runFile — interpret a source file in-place (no exe created) ──────────────

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
              << "  qrun <file>                 Interpret directly (no .exe)\n\n"
              << "  Supported files: .sa .js .py .rb .c .cpp — all run natively on the\n"
              << "  Quantum VM (multi-syntax subset; node/python/gcc NOT required)\n\n"
              << "  quantum hello.sa            → hello.exe created and run\n"
              << "  quantum prog.c              → prog.exe created and run (no gcc)\n"
              << "  qrun    hello.py            → interpreted directly\n"
              << "  qrun    hello.rb            → Ruby-style subset interpreted directly\n";
}
