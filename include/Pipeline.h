#pragma once
// The shared compile path: source text -> Chunk, running the full
// lexer -> parser -> type-checker (warnings only) -> bytecode compiler.
// Callers are expected to have already run the source through applyDialect().

#include "Opcode.h"
#include <memory>
#include <string>

std::shared_ptr<Chunk> compileSource(const std::string &source,
                                     const std::string &sourcePath = "<input>",
                                     bool debug = false);

// Set while the batch test runner is active: makes input() return canned
// values instead of blocking on stdin. Read by src/vm/VmNatives.cpp.
extern bool g_testMode;
