/* Quantum Language v2.0.0 — Bytecode VM
 *
 * Entry point only: this file selects the build mode and dispatches the
 * command line. Everything it calls lives in its own translation unit —
 *   src/dialect/  the multi-language source-to-source front-end
 *   src/cli/      file runners, REPL, batch test runner, .exe bundler
 *   src/Pipeline  source -> Chunk (lexer/parser/type-checker/compiler)
 *
 * Build defines (set by CMakeLists.txt):
 *   QUANTUM_MODE_COMPILER  → quantum.exe      (compiles .sa → .exe, then runs it)
 *   QRUN_MODE              → qrun.exe         (always interprets, never bundles)
 *   neither                → quantum_stub.exe (standalone bundled exe — hello.exe etc.)
 */

#include "Cli.h"
#include "Pipeline.h"
#include "Dialect.h"
#include "Disassembler.h"
#include "Vm.h"
#include "Error.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#endif

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

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
