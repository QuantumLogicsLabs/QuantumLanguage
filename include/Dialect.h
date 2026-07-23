#pragma once
// Dialect layer — the source-to-source front-end that lets Quantum accept
// several languages' syntax before the ordinary lexer/parser/compiler run.
//
//   .rb/.py/.js/.c/.cpp → strict single-dialect: the whole file is that
//                         language, so its rewrites apply unconditionally.
//   .sa                 → multi-dialect: Ruby, Python, JavaScript, C and C++
//                         syntax may be mixed in one file, so every Ruby-only
//                         rewrite is gated behind an ambiguity check and only
//                         fires on lines that are not already valid in one of
//                         the other styles.
//
// Set the QUANTUM_DEBUG_RUBY environment variable to dump the translated
// source to stderr before it is compiled — the primary debugging tool for
// this layer.

#include <string>
#include <vector>

// ─── Router ───────────────────────────────────────────────────────────────
// Applies whichever dialect passes the file extension calls for, returning
// source the normal Quantum pipeline can compile.
std::string applyDialect(std::string source, const std::string &path);

// Lower-cased file extension including the leading dot (".rb", ".sa", ...).
std::string fileExtLower(const std::string &path);

// True for a file type this compiler runs natively: .sa .js .py .rb .c .cpp
bool hasSupportedExt(const std::string &path);

// ─── Ruby dialect ─────────────────────────────────────────────────────────
// `strict` = true for real `.rb` files (convert unconditionally);
// false for `.sa`, where a construct only converts when it cannot be valid
// syntax in one of the other dialects sharing the file.
std::string applyRubyDialect(const std::string &source, bool strict);

// The synthesized Ruby `Enumerable` mixin, as Ruby source lines. Spliced in
// wherever a class does `include Enumerable`; every method is defined purely
// in terms of the including class's own `each`.
const std::vector<std::string> &rubyEnumerableModuleSource();

// ─── C/C++ dialect ────────────────────────────────────────────────────────
// True if the source defines a function named main ("main" followed by "(",
// not part of a longer identifier).
bool definesMainFunction(const std::string &src);
