#pragma once
// Command-line surface: the file-running entry points, the REPL, the batch
// test runner, and the standalone-.exe bundler.

#include "Opcode.h"
#include <memory>
#include <string>

// ─── Console output ───────────────────────────────────────────────────────
void printBanner();
void printAura();
void printHelp(const char *prog);

// ─── Running a single file ────────────────────────────────────────────────
// Interpret in place (no .exe produced). Exits non-zero on error.
void runFile(const std::string &path, bool debug = false);
// Parse + type-check only. Returns a process exit code.
int checkFile(const std::string &path);

// ─── REPL ─────────────────────────────────────────────────────────────────
void runREPL(bool debug = false);

// ─── Batch test runner (--test) ───────────────────────────────────────────
// Runs every supported source file under `dir`, crash-guarded per file, and
// writes test_results.txt. Returns a process exit code (non-zero if any FAIL).
int runTestExamples(const std::string &dir);

// ─── Standalone .exe bundling ─────────────────────────────────────────────
// Path of the currently running executable.
std::string getExecutablePath();
// Reads bytecode appended to an .exe by the bundler, or nullptr if absent.
std::shared_ptr<Chunk> loadEmbeddedBytecode(const std::string &exePath);
// Compiles `path`, copies quantum_stub.exe, appends the bytecode payload,
// then runs the result. Returns a process exit code.
int bundleAndRun(const std::string &path, const std::string &exePath);
