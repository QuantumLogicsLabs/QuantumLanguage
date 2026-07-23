#include "Dialect.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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
           ext == ".rb" || ext == ".c" || ext == ".cpp";
}

// C/C++ programs only define main() — append a call so the program executes
// after its top-level declarations load. Ruby files are first normalized into
// the common Quantum syntax and then use the normal compiler/VM pipeline.
std::string applyDialect(std::string source, const std::string &path)
{
    std::string ext = fileExtLower(path);

    if (ext == ".rb")
        source = applyRubyDialect(source, /*strict=*/true);
    else if (ext == ".sa")
        source = applyRubyDialect(source, /*strict=*/false);
    if ((ext == ".rb" || ext == ".sa") && std::getenv("QUANTUM_DEBUG_RUBY"))
        std::cerr << "----- translated -----\n" << source << "----- end -----\n";

    if ((ext == ".c" || ext == ".cpp") && definesMainFunction(source))
        source += "\nmain()\n";

    return source;
}
