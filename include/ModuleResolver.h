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