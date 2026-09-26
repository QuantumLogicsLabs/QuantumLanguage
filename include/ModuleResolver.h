#pragma once
#include "AST.h"
#include <string>

// Walks the top-level statements of `root` (a BlockStmt — the parsed
// program), finds every ImportStmt, locates the referenced module file
// on disk, parses it, and splices its *exported* declarations directly
// into `root` in place of the ImportStmt node.
//
// After this runs, the Compiler sees real FunctionDecl/VarDecl nodes for
// anything imported — no separate "import" concept exists at compile time
// at all, which is why nothing further needs to change in Compiler*.cpp.
//
// `sourcePath` is the path of the file being resolved (used to find
// qpm_modules/ next to it, and to resolve relative imports). Throws
// ParseError on: module not found, circular imports, or `from X import Y`
// where Y isn't an exported member of X.
void resolveImports(ASTNode &root, const std::string &sourcePath);

// qpm `.sa` packages declare dependencies with `# @use <spec>` comment lines
// (the convention quantum-bundle inlines ahead of time). This resolves them
// natively with the same rules, so an unbundled file runs directly: every
// dependency's top-level statements are spliced in front of `root`'s, each
// file once and dependencies first; a circular `@use` is skipped with a
// warning.
//
//   # @use ./helpers.sa        relative to the using file
//   # @use quantum-strings     <root>/node_modules/<name>, then each of the
//                              root package.json's "quantum.paths" dirs;
//                              the package's "main", else index.sa
//   # @use quantum-strings/x.sa  a file inside that package
//
// <root> is the nearest directory at or above `sourcePath` holding a
// package.json. `source` is the (dialect-processed) text of `root`. Only
// `.sa` files are resolved, and a file already produced by quantum-bundle
// is left alone — its `@use` lines are inert copies of what it inlined.
// Throws ParseError for a dependency that cannot be found or read.
void resolveUseDirectives(ASTNode &root, const std::string &source,
                          const std::string &sourcePath);

// Drops a leading UTF-8 byte-order mark, which Windows editors often write.
void stripUtf8Bom(std::string &text);