#include "ModuleResolver.h"
#include "Lexer.h"
#include "Parser.h"
#include <filesystem>
#include <fstream>
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
        fs::path dir = fromDir;
        for (;;)
        {
            fs::path local = dir / (module + ".sa");
            if (fs::exists(local))
                return local;

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

            if (!imp.module.empty())
            {
                // from <module> import a, b as c, ...
                fs::path modPath = resolveModulePath(imp.module, dir);
                if (modPath.empty())
                    throw ParseError("Cannot resolve module \"" + imp.module +
                                          "\" (searched local files and node_modules/)",
                                      stmt->line, 0);

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
                        throw ParseError("Cannot resolve module \"" + item.name +
                                              "\" (searched local files and node_modules/)",
                                          stmt->line, 0);

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