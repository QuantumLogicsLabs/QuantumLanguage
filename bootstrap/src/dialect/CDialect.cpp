#include "Dialect.h"

#include <cctype>
#include <string>

// True if the source defines a function named main ("main" followed by "(",
// not part of a longer identifier).
bool definesMainFunction(const std::string &src)
{
    size_t p = 0;
    while ((p = src.find("main", p)) != std::string::npos)
    {
        bool leftOk = (p == 0) ||
                      (!std::isalnum((unsigned char)src[p - 1]) && src[p - 1] != '_');
        size_t q = p + 4;
        while (q < src.size() && std::isspace((unsigned char)src[q]))
            q++;
        if (leftOk && q < src.size() && src[q] == '(')
            return true;
        p += 4;
    }
    return false;
}
