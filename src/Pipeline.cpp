#include "Pipeline.h"
#include "Lexer.h"
#include "Parser.h"
#include "Compiler.h"
#include "TypeChecker.h"
#include "Disassembler.h"
#include "Error.h"
#include <iostream>

// Shared with src/vm/VmNatives.cpp (declared extern there).
bool g_testMode = false;

// ─── Compile source → Chunk ───────────────────────────────────────────────────

std::shared_ptr<Chunk> compileSource(const std::string &source,
                                     const std::string &sourcePath,
                                     bool debug)
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

// Dialect front-end (Ruby/C/C++ source-to-source passes) lives in
// src/dialect/ and is reached through applyDialect() in Dialect.h.
