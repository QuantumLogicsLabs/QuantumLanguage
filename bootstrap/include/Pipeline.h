#pragma once
// The shared compile path: source text -> Chunk, running the full
// lexer -> parser -> type-checker (warnings only) -> bytecode compiler.
// Callers are expected to have already run the source through applyDialect().

#include "Opcode.h"
#include <memory>
#include <string>
#include <vector>

std::shared_ptr<Chunk> compileSource(const std::string &source,
                                     const std::string &sourcePath = "<input>",
                                     bool debug = false);

// Set while the batch test runner is active: makes input() return canned
// values instead of blocking on stdin. Read by src/vm/VmNatives.cpp.
extern bool g_testMode;

// --strict: a .c/.cpp file must be valid C/C++. The typed front-end's
// verdict is final — a program outside it is a compile error instead of
// falling back to the multi-syntax pipeline that accepts Python/JS/Ruby
// style code. (.java is always strict; .sa is never strict.)
extern bool g_strictMode;

// The running script's path followed by its command-line arguments
// (`qrun tool.sa --json` -> {"tool.sa", "--json"}). Set by main() before the
// VM is constructed; exposed to scripts as sys.argv by src/vm/VmNatives.cpp.
extern std::vector<std::string> g_scriptArgv;
