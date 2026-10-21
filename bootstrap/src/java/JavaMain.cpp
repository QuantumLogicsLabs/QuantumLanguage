// The Java front-end's entry point: parse the program and the prelude, run
// the semantic analysis, and report errors the way javac prints them.
#include "JavaSema.h"
#include "Diagnostics.h"
#include "Error.h"

namespace java
{

namespace
{
std::string baseName(const std::string &path)
{
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string errorCount(size_t n) { return std::to_string(n) + (n == 1 ? " error\n" : " errors\n"); }
} // namespace

ASTNodePtr compileProgram(const std::string &path, const std::string &source)
{
    const std::string file = baseName(path);
    std::vector<CompilationUnit> units;
    try
    {
        units.push_back(parse(file, source, 0, false));
    }
    catch (const SyntaxError &e)
    {
        throw CompileError(diag::javac(file, source, e.pos.line, e.pos.col, e.msg) + errorCount(1));
    }
    const auto &prelude = preludeSources();
    for (size_t i = 0; i < prelude.size(); ++i)
    {
        try
        {
            units.push_back(parse(prelude[i].first, prelude[i].second, (int)i + 1, true));
        }
        catch (const SyntaxError &e)
        {
            throw CompileError("internal error in the Java library:\n" +
                               diag::javac(prelude[i].first, prelude[i].second, e.pos.line, e.pos.col, e.msg));
        }
    }
    Sema sema(units);
    ASTNodePtr program = sema.run();
    const auto &diags = sema.diagnostics();
    if (diags.empty() && program)
        return program;
    // javac stops listing after 100 errors
    std::string report;
    size_t shown = 0;
    for (const Diag &d : diags)
    {
        if (shown == 100)
            break;
        const CompilationUnit &u = units[d.unit];
        report += diag::javac(u.isPrelude ? "(library) " + u.fileName : u.fileName, u.source, d.pos.line, d.pos.col, d.msg,
                              d.notes);
        ++shown;
    }
    if (diags.empty())
        report = file + ": error: compilation failed\n";
    report += errorCount(diags.empty() ? 1 : diags.size());
    throw CompileError(report);
}

} // namespace java
