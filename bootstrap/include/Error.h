#pragma once
#include <stdexcept>
#include <string>

class QuantumError : public std::runtime_error
{
public:
    int line;
    std::string kind;

    QuantumError(const std::string &kind, const std::string &msg, int line = -1)
        : std::runtime_error(msg), line(line), kind(kind) {}
};

class RuntimeError : public QuantumError
{
public:
    RuntimeError(const std::string &msg, int line = -1)
        : QuantumError("RuntimeError", msg, line) {}
};

class TypeError : public QuantumError
{
public:
    TypeError(const std::string &msg, int line = -1)
        : QuantumError("TypeError", msg, line) {}
};

class NameError : public QuantumError
{
public:
    NameError(const std::string &msg, int line = -1)
        : QuantumError("NameError", msg, line) {}
};

class IndexError : public QuantumError
{
public:
    IndexError(const std::string &msg, int line = -1)
        : QuantumError("IndexError", msg, line) {}
};

// A program rejected at compile time by a strict front-end (Java, or C/C++
// under --strict). `what()` is the complete diagnostic, already in the real
// compiler's format (gcc's `file:line:col: error: ...`, javac's
// `File.java:line: error: ...`), so the CLI prints it verbatim.
class CompileError : public QuantumError
{
public:
    explicit CompileError(const std::string &report)
        : QuantumError("CompileError", report) {}
};

namespace Colors
{
    inline const char *RED = "\033[31m";
    inline const char *YELLOW = "\033[33m";
    inline const char *WHITE = "\033[37m";
    inline const char *CYAN = "\033[36m";
    inline const char *GREEN = "\033[32m";
    inline const char *BLUE = "\033[34m";
    inline const char *BOLD = "\033[1m";
    inline const char *RESET = "\033[0m";
    inline const char *MAGENTA = "\033[35m";
}
