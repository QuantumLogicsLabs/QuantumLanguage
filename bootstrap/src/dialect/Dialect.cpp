#include "Dialect.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <regex>
#include <string>

namespace fs = std::filesystem;

// multi-syntax front-end — no node/python/gcc/g++ required.

std::string fileExtLower(const std::string &path)
{
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

bool hasSupportedExt(const std::string &path)
{
    std::string ext = fileExtLower(path);
    return ext == ".sa" || ext == ".js" || ext == ".py" ||
           ext == ".rb" || ext == ".c" || ext == ".cpp" || ext == ".java";
}

// `int main(int argc, char** argv)` gets the command line: sys.argv holds the
// program followed by its arguments, the same layout as C's argv.
static std::string mainCall(const std::string &source)
{
    static const std::regex withParams(
        R"((^|\n)[ \t]*(static[ \t]+)?(int|auto)[ \t]+main[ \t]*\([ \t]*(int|const|unsigned)\b)");
    return std::regex_search(source, withParams) ? "main(len(sys.argv), sys.argv)" : "main()";
}

// C/C++ programs only define main() — append a call so the program executes
// after its top-level declarations load (for .sa, only in a multi-file C++
// project: a C-style `int main(` in a file that splices in local headers).
// Ruby files are first normalized into the common Quantum syntax and then use
// the normal compiler/VM pipeline.
std::string applyDialect(std::string source, const std::string &path)
{
    std::string ext = fileExtLower(path);

    // A UTF-8 byte-order mark (common from Windows editors) would otherwise
    // sit in front of the first line's code.
    if (source.size() >= 3 && (unsigned char)source[0] == 0xEF &&
        (unsigned char)source[1] == 0xBB && (unsigned char)source[2] == 0xBF)
        source.erase(0, 3);

    // C++ project structure first (local #includes, namespaces, out-of-class
    // member definitions), so the spliced-in files see every later pass.
    bool cppProject = false;
    if (ext == ".c" || ext == ".cpp" || ext == ".sa")
    {
        source = applyCppDialect(source, path, &cppProject);
        if (std::getenv("QUANTUM_DEBUG_CPP"))
            std::cerr << "----- C++ structure pass -----\n" << source << "----- end -----\n";
    }

    if (ext == ".rb")
        source = applyRubyDialect(source, /*strict=*/true);
    else if (ext == ".sa")
        source = applyRubyDialect(source, /*strict=*/false);
    if ((ext == ".rb" || ext == ".sa") && std::getenv("QUANTUM_DEBUG_RUBY"))
        std::cerr << "----- translated -----\n" << source << "----- end -----\n";

    // __cpp_runtime__() switches the VM to C/C++ arithmetic rules first.
    if ((ext == ".c" || ext == ".cpp") && definesMainFunction(source))
        source += "\n__cpp_runtime__()\n" + mainCall(source) + "\n";
    else if (ext == ".sa" && cppProject && definesCStyleMain(source))
        source += "\n__cpp_runtime__()\n" + mainCall(source) + "\n";

    return source;
}
