#include "ModuleResolver.h"
#include "Dialect.h"
#include "Lexer.h"
#include "Parser.h"
#include "qpm/QpmJson.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <algorithm>

namespace fs = std::filesystem;

namespace
{
    // Forward declaration -- loadModuleExports() below needs to recurse
    // into a module's own imports before harvesting its exports.
    void resolveImportsInternal(ASTNode &root, const std::string &sourcePath,
                                 std::vector<std::string> &resolving);

    // Reads a whole file into a string. Returns false if it can't be opened.
    bool readFile(const fs::path &p, std::string &out)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f)
            return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    // Resolution order for `module`, searched from the importing file's
    // own directory upward (like Node's node_modules walk):
    //   1. <dir>/<module>.sa                          (local sibling file)
    //   2. <dir>/node_modules/<module>/<module>.sa      (installed package)
    //   3. <dir>/node_modules/<module>/index.sa         (installed package, index convention)
    //   4. repeat 2-3 in each parent directory up to the filesystem root
    fs::path resolveModulePath(const std::string &module, const fs::path &fromDir)
    {
        fs::path local = fromDir / (module + ".sa");
        if (fs::exists(local))
            return local;

        fs::path dir = fromDir;
        for (;;)
        {
            fs::path pkgMain = dir / "node_modules" / module / (module + ".sa");
            if (fs::exists(pkgMain))
                return pkgMain;

            fs::path pkgIndex = dir / "node_modules" / module / "index.sa";
            if (fs::exists(pkgIndex))
                return pkgIndex;

            if (!dir.has_parent_path() || dir == dir.parent_path())
                break;
            dir = dir.parent_path();
        }
        return {};
    }

    // Parses one module file and returns its exported top-level
    // declarations, keyed by name. Non-exported declarations and any
    // other top-level statements (loose expressions, prints, etc.) are
    // discarded -- only `export function`/`export let` survive an import.
    std::unordered_map<std::string, ASTNodePtr>
    loadModuleExports(const fs::path &path, std::vector<std::string> &resolving)
    {
        std::string canon = fs::weakly_canonical(path).string();

        if (std::find(resolving.begin(), resolving.end(), canon) != resolving.end())
        {
            std::string chain;
            for (auto &p : resolving)
                chain += p + " -> ";
            throw ParseError("Circular import detected: " + chain + canon, 0, 0);
        }

        std::string source;
        if (!readFile(path, source))
            throw ParseError("Could not read module file: " + path.string(), 0, 0);

        Lexer lexer(source);
        auto tokens = lexer.tokenize();
        Parser parser(std::move(tokens));
        ASTNodePtr moduleRoot = parser.parse();

        resolving.push_back(canon);
        // Modules can import other modules too -- resolve those first so
        // nested imports are fully expanded before we harvest exports.
        resolveImportsInternal(*moduleRoot, path.string(), resolving);
        resolving.pop_back();

        std::unordered_map<std::string, ASTNodePtr> exported;
        auto &stmts = moduleRoot->as<BlockStmt>().statements;
        for (auto &stmt : stmts)
        {
            if (stmt->is<FunctionDecl>() && stmt->as<FunctionDecl>().isExported)
            {
                std::string name = stmt->as<FunctionDecl>().name;
                exported.emplace(name, std::move(stmt));
            }
            else if (stmt->is<VarDecl>() && stmt->as<VarDecl>().isExported)
            {
                std::string name = stmt->as<VarDecl>().name;
                exported.emplace(name, std::move(stmt));
            }
        }
        return exported;
    }

    void resolveImportsInternal(ASTNode &root, const std::string &sourcePath,
                                 std::vector<std::string> &resolving)
    {
        fs::path dir = fs::path(sourcePath).parent_path();
        if (dir.empty())
            dir = fs::current_path();

        auto &stmts = root.as<BlockStmt>().statements;
        std::vector<ASTNodePtr> newStmts;
        newStmts.reserve(stmts.size());

        for (auto &stmt : stmts)
        {
            if (!stmt->is<ImportStmt>())
            {
                newStmts.push_back(std::move(stmt));
                continue;
            }

            const ImportStmt &imp = stmt->as<ImportStmt>();

            // A module with no file on disk is a host-language library
            // (`import math`, `from abc import ABC`, `import numpy as np`).
            // The VM either provides it as a native global or it has no
            // runtime meaning, so it compiles to a no-op -- the behaviour
            // before module resolution existed.
            if (!imp.module.empty())
            {
                // from <module> import a, b as c, ...
                fs::path modPath = resolveModulePath(imp.module, dir);
                if (modPath.empty())
                {
                    newStmts.push_back(std::move(stmt));
                    continue;
                }

                auto exports = loadModuleExports(modPath, resolving);

                for (const auto &item : imp.imports)
                {
                    auto it = exports.find(item.name);
                    if (it == exports.end())
                        throw ParseError("Module \"" + imp.module + "\" has no exported member \"" +
                                              item.name + "\"",
                                          stmt->line, 0);

                    ASTNodePtr decl = std::move(it->second);
                    const std::string &finalName = item.alias.empty() ? item.name : item.alias;
                    if (decl->is<FunctionDecl>())
                        decl->as<FunctionDecl>().name = finalName;
                    else if (decl->is<VarDecl>())
                        decl->as<VarDecl>().name = finalName;

                    newStmts.push_back(std::move(decl));
                }
            }
            else
            {
                // bare `import moduleA, moduleB` -- pulls in every exported
                // symbol from each named module (no per-symbol selection,
                // no module namespace object).
                for (const auto &item : imp.imports)
                {
                    fs::path modPath = resolveModulePath(item.name, dir);
                    if (modPath.empty())
                        continue; // host-language library, see above

                    auto exports = loadModuleExports(modPath, resolving);
                    for (auto &kv : exports)
                        newStmts.push_back(std::move(kv.second));
                }
            }
        }

        stmts = std::move(newStmts);
    }
} // namespace

void resolveImports(ASTNode &root, const std::string &sourcePath)
{
    std::vector<std::string> resolving;
    resolving.push_back(fs::weakly_canonical(sourcePath).string());
    resolveImportsInternal(root, sourcePath, resolving);
}

void stripUtf8Bom(std::string &text)
{
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);
}

// ─── `# @use` directives ──────────────────────────────────────────────────────
// Mirrors quantum-bundle's lib/bundler.sa (parseUses / resolve / packageMain /
// visit) so a bundled and an unbundled run see the same modules in the same
// order.
namespace
{
    struct UseDirective
    {
        std::string spec;
        int line;
    };

    std::string trimmed(const std::string &s)
    {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    bool startsWithText(const std::string &s, const std::string &prefix)
    {
        return s.compare(0, prefix.size(), prefix) == 0;
    }

    // The `# @use <spec>` lines of a source, in order. The Ruby dialect turns
    // a `#` comment line into `//`, so both spellings are accepted.
    std::vector<UseDirective> parseUseDirectives(const std::string &source)
    {
        std::vector<UseDirective> uses;
        std::istringstream in(source);
        std::string raw;
        int line = 0;
        while (std::getline(in, raw))
        {
            ++line;
            std::string t = trimmed(raw);
            std::string rest;
            if (startsWithText(t, "# @use "))
                rest = t.substr(7);
            else if (startsWithText(t, "// @use "))
                rest = t.substr(8);
            else
                continue;
            std::string spec = trimmed(rest);
            if (spec.size() >= 2 && (spec[0] == '"' || spec[0] == '\''))
                spec = spec.substr(1, spec.size() - 2);
            if (!spec.empty())
                uses.push_back({spec, line});
        }
        return uses;
    }

    // quantum-bundle output keeps each module's `@use` lines verbatim; they
    // are already satisfied by the modules inlined above them.
    bool isBundleOutput(const std::string &source)
    {
        std::istringstream in(source);
        std::string raw;
        while (std::getline(in, raw))
        {
            std::string t = trimmed(raw);
            if (!t.empty())
                return t.find("Generated by quantum-bundle") != std::string::npos;
        }
        return false;
    }

    bool isFile(const fs::path &p)
    {
        std::error_code ec;
        return fs::is_regular_file(p, ec);
    }

    struct UseOptions
    {
        fs::path root;                    // project directory
        std::vector<fs::path> searchDirs; // package directories, in order
    };

    // <root>/node_modules, then each of the root package.json's
    // "quantum.paths" entries (relative to <root>).
    UseOptions useOptionsFor(const fs::path &entryDir)
    {
        UseOptions opts;
        opts.root = entryDir;
        for (fs::path dir = entryDir;; dir = dir.parent_path())
        {
            if (isFile(dir / "package.json"))
            {
                opts.root = dir;
                break;
            }
            if (!dir.has_parent_path() || dir == dir.parent_path())
                break;
        }
        opts.searchDirs.push_back(opts.root / "node_modules");
        std::string text;
        if (readFile(opts.root / "package.json", text))
        {
            stripUtf8Bom(text);
            try
            {
                auto manifest = qpm::JsonValue::parse(text);
                auto paths = manifest.get("quantum").get("paths");
                if (paths.isArray())
                    for (const auto &p : paths.array())
                        if (p.isString())
                            opts.searchDirs.push_back((opts.root / p.asString()).lexically_normal());
            }
            catch (const std::exception &)
            {
                // A broken manifest only loses the extra search paths.
            }
        }
        return opts;
    }

    // A package's entry file: its package.json "main", else index.sa.
    fs::path packageMain(const fs::path &pkgDir)
    {
        fs::path manifestPath = pkgDir / "package.json";
        std::string text;
        if (!isFile(manifestPath) || !readFile(manifestPath, text))
            return pkgDir / "index.sa";
        stripUtf8Bom(text);
        qpm::JsonValue meta;
        try
        {
            meta = qpm::JsonValue::parse(text);
        }
        catch (const std::exception &e)
        {
            throw ParseError("invalid package.json at " + manifestPath.generic_string() + ": " + e.what(), 0, 0);
        }
        auto main = meta.get("main");
        if (main.isString())
            return (pkgDir / main.asString()).lexically_normal();
        return pkgDir / "index.sa";
    }

    fs::path resolveUse(const UseDirective &use, const fs::path &fromDir, const UseOptions &opts)
    {
        const std::string &spec = use.spec;
        bool relative = startsWithText(spec, "./") || startsWithText(spec, "../") ||
                        fs::path(spec).is_absolute() || startsWithText(spec, "/");
        if (relative)
        {
            fs::path target = (fromDir / spec).lexically_normal();
            if (isFile(target))
                return target;
            throw ParseError("cannot find '" + spec + "' (looked for " + target.generic_string() + ")", use.line, 0);
        }

        // "name/sub/file.sa" or "@scope/name/file.sa" -> package + subpath
        std::vector<std::string> parts;
        std::stringstream ss(spec);
        for (std::string part; std::getline(ss, part, '/');)
            parts.push_back(part);
        size_t nameParts = (spec[0] == '@' && parts.size() > 1) ? 2 : 1;
        fs::path name, sub;
        for (size_t i = 0; i < parts.size(); ++i)
            (i < nameParts ? name : sub) /= parts[i];

        std::string tried;
        for (const auto &dir : opts.searchDirs)
        {
            fs::path pkgDir = dir / name;
            fs::path target = sub.empty() ? packageMain(pkgDir) : (pkgDir / sub).lexically_normal();
            if (isFile(target))
                return target;
            tried += (tried.empty() ? "" : ", ") + target.generic_string();
        }
        throw ParseError("cannot find package '" + spec + "' (tried: " + tried + ")", use.line, 0);
    }

    std::string pathKey(const fs::path &p)
    {
        std::string key = fs::weakly_canonical(p).generic_string();
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        return key;
    }

    struct UseWalk
    {
        UseOptions opts;
        std::set<std::string> done, visiting;
        std::vector<ASTNodePtr> statements; // dependencies' code, in order
    };

    // Depth-first: a module's statements follow all of its dependencies'.
    void visitUse(const fs::path &path, UseWalk &walk)
    {
        std::string key = pathKey(path);
        if (walk.done.count(key))
            return;
        if (walk.visiting.count(key))
        {
            std::cerr << "[UseWarning] circular @use skipped: " << path.generic_string() << "\n";
            return;
        }
        walk.visiting.insert(key);

        std::string source;
        if (!readFile(path, source))
            throw ParseError("cannot read " + path.generic_string(), 0, 0);
        stripUtf8Bom(source);

        for (const auto &use : parseUseDirectives(source))
            visitUse(resolveUse(use, path.parent_path(), walk.opts), walk);

        std::string pathText = path.string();
        ASTNodePtr module;
        try
        {
            Lexer lexer(applyDialect(source, pathText));
            Parser parser(lexer.tokenize());
            module = parser.parse();
            resolveImports(*module, pathText);
        }
        catch (const ParseError &e)
        {
            // The line is this module's, not the entry file's — say which.
            throw ParseError(std::string(e.what()) + " (in @use module " + path.generic_string() + ")",
                             e.line, e.col);
        }
        for (auto &stmt : module->as<BlockStmt>().statements)
            walk.statements.push_back(std::move(stmt));

        walk.visiting.erase(key);
        walk.done.insert(key);
    }
} // namespace

void resolveUseDirectives(ASTNode &root, const std::string &source,
                          const std::string &sourcePath)
{
    if (sourcePath.empty() || fileExtLower(sourcePath) != ".sa" || isBundleOutput(source))
        return;
    auto uses = parseUseDirectives(source);
    if (uses.empty())
        return;

    fs::path entry = fs::absolute(sourcePath);
    UseWalk walk;
    walk.opts = useOptionsFor(entry.parent_path());
    walk.visiting.insert(pathKey(entry)); // a dependency using the entry is a cycle
    for (const auto &use : uses)
        visitUse(resolveUse(use, entry.parent_path(), walk.opts), walk);

    auto &stmts = root.as<BlockStmt>().statements;
    for (auto &stmt : stmts)
        walk.statements.push_back(std::move(stmt));
    stmts = std::move(walk.statements);
}
