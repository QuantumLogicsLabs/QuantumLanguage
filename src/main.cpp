 /* Quantum Language v2.0.0 — Bytecode VM
 *
 * Build defines (set by CMakeLists.txt):
 *   QUANTUM_MODE_COMPILER  → quantum.exe      (compiles .sa → .exe, then runs it)
 *   QRUN_MODE              → qrun.exe         (always interprets, never bundles)
 *   neither                → quantum_stub.exe  (standalone bundled exe — hello.exe etc.)
 */

#include "Lexer.h"
#include "Parser.h"
#include "Compiler.h"
#include "Vm.h"
#include "Disassembler.h"
#include "TypeChecker.h"
#include "Error.h"
#include "Value.h"
#include "Serializer.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <cstdlib>
#include <regex>
#include <functional>
#include <map>
#include <set>

// Windows-only — bundling and launching use Win32 API
// Platform-specific headers
#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
    #include <limits.h>
    #include <cstdlib>
    #include <sys/wait.h>
#endif
#include <setjmp.h>
#include <signal.h>

// Executable file extension: ".exe" on Windows, nothing elsewhere.
#ifdef _WIN32
    static const char *EXE_EXT = ".exe";
#else
    static const char *EXE_EXT = "";
#endif

// Quote a program path for system().  On POSIX the shell searches $PATH
// only -- never the current directory -- so a bare name like "hello" is
// not found even when ./hello exists.  Prefix "./" when there is no slash.
static std::string shellExec(const std::string &p)
{
#ifdef _WIN32
    return "\"" + p + "\"";
#else
    if (p.find('/') == std::string::npos)
        return "\"./" + p + "\"";
    return "\"" + p + "\"";
#endif
}

namespace fs = std::filesystem;

// Shared with Vm.cpp
bool g_testMode = false;

// ─── Executable path ──────────────────────────────────────────────────────────

static std::string getExecutablePath()
{
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    return std::string(buffer);
#else
    char buffer[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buffer, PATH_MAX - 1);
    if (n == -1) return std::string();
    buffer[n] = '\0';
    return std::string(buffer);
#endif
}

// ─── Embedded bytecode ────────────────────────────────────────────────────────
// Format (appended after the PE image):
//   [payload bytes ...] [payloadSize: uint32_t LE] [magic: "QNTM_VM!" 8 bytes]

static std::shared_ptr<Chunk> loadEmbeddedBytecode(const std::string &exePath)
{
    std::ifstream file(exePath, std::ios::binary | std::ios::ate);
    if (!file)
        return nullptr;

    auto size = (uint64_t)file.tellg();
    if (size < 12)
        return nullptr;

    // Check magic at the very end
    file.seekg(-(std::streamoff)8, std::ios::end);
    char magic[8];
    file.read(magic, 8);
    if (std::memcmp(magic, "QNTM_VM!", 8) != 0)
        return nullptr;

    // Read payload size
    file.seekg(-(std::streamoff)12, std::ios::end);
    uint32_t payloadSize = 0;
    file.read(reinterpret_cast<char *>(&payloadSize), 4);

    // Sanity: payload must fit in file and be non-zero, non-absurd
    if (payloadSize == 0 || payloadSize > 64u * 1024 * 1024)
        return nullptr;
    if ((uint64_t)(payloadSize + 12) > size)
        return nullptr;

    // Read payload
    file.seekg(-(std::streamoff)(payloadSize + 12), std::ios::end);
    std::vector<uint8_t> payload(payloadSize);
    file.read(reinterpret_cast<char *>(payload.data()), payloadSize);
    if (!file)
        return nullptr;

    try
    {
        return Serializer::deserialize(payload);
    }
    catch (...)
    {
        return nullptr;
    }
}

// ─── Banner ───────────────────────────────────────────────────────────────────

static void printBanner()
{
    std::cout
        << Colors::CYAN << Colors::BOLD
        << "\n"
        << "  ██████╗ ██╗   ██╗ █████╗ ███╗   ██╗████████╗██╗   ██╗███╗   ███╗\n"
        << " ██╔═══██╗██║   ██║██╔══██╗████╗  ██║╚══██╔══╝██║   ██║████╗ ████║\n"
        << " ██║   ██║██║   ██║███████║██╔██╗ ██║   ██║   ██║   ██║██╔████╔██║\n"
        << " ██║▄▄ ██║██║   ██║██╔══██║██║╚██╗██║   ██║   ██║   ██║██║╚██╔╝██║\n"
        << " ╚██████╔╝╚██████╔╝██║  ██║██║ ╚████║   ██║   ╚██████╔╝██║ ╚═╝ ██║\n"
        << "  ╚══▀▀═╝  ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═══╝   ╚═╝    ╚═════╝ ╚═╝     ╚═╝\n"
        << Colors::RESET
        << Colors::YELLOW << "  Quantum Language v2.0.0 | Bytecode VM Edition\n"
        << Colors::RESET << "\n";
}

static void printAura()
{
    std::cout
        << Colors::CYAN << Colors::BOLD
        << "\n╔══════════════════════════════════════════════════════════════════╗\n"
        << "║" << Colors::YELLOW << "                🌟 QUANTUM LANGUAGE ACHIEVEMENTS 🌟" << Colors::CYAN << "               ║\n"
        << "╠══════════════════════════════════════════════════════════════════╣\n"
        << "║" << Colors::GREEN << "  ✅ Complete C++17 Compiler + Bytecode VM" << Colors::CYAN << "                        ║\n"
        << "║" << Colors::GREEN << "  ✅ Multi-Syntax: Python + JavaScript + C/C++" << Colors::CYAN << "                    ║\n"
        << "║" << Colors::GREEN << "  ✅ Closures, Classes, Exceptions, Pointers" << Colors::CYAN << "                      ║\n"
        << "║" << Colors::GREEN << "  ✅ Self-bundling standalone .exe generation" << Colors::CYAN << "                     ║\n"
        << "╚══════════════════════════════════════════════════════════════════╝\n"
        << Colors::RESET;
}

// ─── Compile source → Chunk ───────────────────────────────────────────────────

static std::shared_ptr<Chunk> compileSource(const std::string &source,
                                            const std::string &sourcePath = "<input>",
                                            bool debug = false)
{
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    auto ast = parser.parse();

    try
    {
        TypeChecker tc;
        tc.check(ast);
    }
    catch (const StaticTypeError &e)
    {
        std::cerr << Colors::YELLOW << "[TypeWarning] " << Colors::RESET
                  << e.what() << " (line " << e.line << ")\n";
    }

    Compiler compiler;
    auto chunk = compiler.compile(*ast);

    if (debug)
    {
        std::cerr << Colors::CYAN << "[DEBUG] Bytecode — " << sourcePath << "\n"
                  << Colors::RESET;
        disassembleChunk(*chunk, std::cerr);
    }
    return chunk;
}

// ─── Dialect support ──────────────────────────────────────────────────────────
// .js / .py / .rb / .c / .cpp files run natively on the Quantum VM through its
// multi-syntax front-end — no node/python/gcc/g++ required.

static std::string fileExtLower(const std::string &path)
{
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

static bool hasSupportedExt(const std::string &path)
{
    std::string ext = fileExtLower(path);
    return ext == ".sa" || ext == ".js" || ext == ".py" ||
           ext == ".rb" || ext == ".c" || ext == ".cpp";
}

// True if the source defines a function named main ("main" followed by "(",
// not part of a longer identifier).
static bool definesMainFunction(const std::string &src)
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

// Remove leading/trailing spaces while preserving the original line indentation
// separately. This is used only by the lightweight Ruby dialect normalizer.
static std::string trimCopy(const std::string &value)
{
    size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static bool startsWith(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

// ─── Ruby dialect support ──────────────────────────────────────────────────
// Ruby-style syntax is translated into the brace-based common syntax already
// accepted by the Quantum lexer/parser. This intentionally implements a Ruby
// subset; it does not embed or invoke the official Ruby interpreter.
//
// This is a multi-pass source-to-source transpiler (not a single regex pass):
//   1. Modules are flattened: `module M ... end` bodies are captured and
//      spliced in wherever a class does `include M`.
//   2. A quick symbol-table scan collects every class's ivar (`@x`) and
//      method names so bare self-references inside string interpolation
//      (`"#{name}"` inside a method whose class has an ivar/method `name`)
//      can be qualified to `self.name` / `self.name()`.
//   3. The main line-by-line pass rewrites control flow, blocks
//      (`do |x| ... end` / `{ |x| ... }` -> `fn(x) { ... }`), ranges
//      (`(a..b)` -> `range(a, b+1)`), multiple assignment (via temporaries,
//      since Quantum's native unpack assignment only supports plain
//      identifier targets), `case/when`, classes/`initialize`/`@ivar`,
//      `<<` array push, string interpolation, and Ruby's `?`/`!` method-name
//      suffixes (stripped, since Quantum identifiers don't allow them).
//   4. A tail-position pass inserts `return` before the final expression of
//      each method/block body, mirroring Ruby's implicit-return semantics.

// Index just past the string literal starting at s[pos] (s[pos] must be a
// quote character: ' " or the backtick used by Quantum template literals).
static size_t rbSkipString(const std::string &s, size_t pos)
{
    char quote = s[pos];
    size_t i = pos + 1;
    while (i < s.size() && s[i] != quote)
    {
        if (s[i] == '\\' && i + 1 < s.size())
            i += 2;
        else
            i++;
    }
    return i < s.size() ? i + 1 : i;
}

static bool rbIsQuote(char c) { return c == '"' || c == '\'' || c == '`'; }
static bool rbIsIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
static bool rbIsIdentChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// Find `needle` at bracket-depth 0, outside string literals.
static size_t rbFindTopLevel(const std::string &s, const std::string &needle, size_t from = 0)
{
    int depth = 0;
    for (size_t i = from; i < s.size();)
    {
        char c = s[i];
        if (rbIsQuote(c))
        {
            i = rbSkipString(s, i);
            continue;
        }
        // Check the needle BEFORE adjusting depth for this character: this
        // matters when the needle itself is a bracket char (e.g. "{"), since
        // otherwise it would only ever match one level too deep.
        if (depth <= 0 && s.compare(i, needle.size(), needle) == 0)
            return i;
        if (c == '(' || c == '[' || c == '{')
        {
            depth++;
            i++;
            continue;
        }
        if (c == ')' || c == ']' || c == '}')
        {
            depth--;
            i++;
            continue;
        }
        i++;
    }
    return std::string::npos;
}

// Index of the character matching the bracket at s[openPos].
static size_t rbMatchBracket(const std::string &s, size_t openPos)
{
    int depth = 0;
    for (size_t i = openPos; i < s.size();)
    {
        char c = s[i];
        if (rbIsQuote(c))
        {
            i = rbSkipString(s, i);
            continue;
        }
        if (c == '(' || c == '[' || c == '{')
        {
            depth++;
            i++;
            continue;
        }
        if (c == ')' || c == ']' || c == '}')
        {
            depth--;
            if (depth == 0)
                return i;
            i++;
            continue;
        }
        i++;
    }
    return std::string::npos;
}

// Split on `sep` at bracket-depth 0, outside string literals. Trims pieces.
static std::vector<std::string> rbSplitTopLevel(const std::string &s, char sep)
{
    std::vector<std::string> parts;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size();)
    {
        char c = s[i];
        if (rbIsQuote(c))
        {
            i = rbSkipString(s, i);
            continue;
        }
        if (c == '(' || c == '[' || c == '{')
        {
            depth++;
            i++;
            continue;
        }
        if (c == ')' || c == ']' || c == '}')
        {
            depth--;
            i++;
            continue;
        }
        if (depth == 0 && c == sep)
        {
            parts.push_back(s.substr(start, i - start));
            i++;
            start = i;
            continue;
        }
        i++;
    }
    parts.push_back(s.substr(start));
    for (auto &p : parts)
        p = trimCopy(p);
    return parts;
}

// Find a bare top-level '=' (assignment), skipping ==, !=, <=, >=, +=, -=,
// *=, /=, %=, &=, |=, ^=. Returns npos if none.
static size_t rbFindTopLevelAssign(const std::string &s)
{
    int depth = 0;
    for (size_t i = 0; i < s.size();)
    {
        char c = s[i];
        if (rbIsQuote(c))
        {
            i = rbSkipString(s, i);
            continue;
        }
        if (c == '(' || c == '[' || c == '{')
        {
            depth++;
            i++;
            continue;
        }
        if (c == ')' || c == ']' || c == '}')
        {
            depth--;
            i++;
            continue;
        }
        if (depth == 0 && c == '=')
        {
            char prev = i > 0 ? s[i - 1] : '\0';
            char next = i + 1 < s.size() ? s[i + 1] : '\0';
            if (next != '=' && prev != '=' && prev != '!' && prev != '<' && prev != '>' &&
                prev != '+' && prev != '-' && prev != '*' && prev != '/' && prev != '%' &&
                prev != '&' && prev != '|' && prev != '^')
                return i;
        }
        i++;
    }
    return std::string::npos;
}

// Known zero-argument Ruby-style method names that our examples call without
// parens (idiomatic Ruby). Quantum requires explicit call parens, so these
// get "()" appended when used bare (e.g. `str.reverse` -> `str.reverse()`).
// In mixed (.sa) mode the collision-prone names — ones that commonly appear
// as plain instance FIELDS in existing Python/C++-style .sa classes
// (`self.size = 0`, `block.length`, `Date.now`) — are excluded, because
// auto-parenizing a field read breaks it ("No method 'size' on HashTable").
static const std::set<std::string> &rbZeroArgMethods(bool strict)
{
    static const std::set<std::string> strictNames = {
        "reverse", "chomp", "downcase", "upcase", "dup", "clone", "sort",
        "strip", "trim", "to_i", "to_f", "to_s", "zero", "chars", "length",
        "size", "now", "message"};
    static const std::set<std::string> mixedNames = {
        "reverse", "chomp", "downcase", "upcase", "dup", "clone",
        "to_i", "to_f", "to_s", "chars"};
    return strict ? strictNames : mixedNames;
}

// Symbol table (per Ruby file): every ivar / method name seen anywhere.
// Deliberately global (not per-class) — our target scripts have at most one
// small class family per file, so a whole-file union is simple and safe.
struct RubySymbolTable
{
    std::set<std::string> fields;
    std::set<std::string> methods;
};

static const RubySymbolTable &rbEmptySymbolTable()
{
    static RubySymbolTable t;
    return t;
}

// Combined token-level normalization pass, applied to plain code text
// (never string-literal contents, which are always skipped intact):
//   - `@ivar`               -> `self.ivar`
//   - `identifier?` / `!`   -> `identifier`   (Ruby predicate/bang suffix)
//   - bare zero-arg method calls (`.reverse`, `.chomp`, user-defined
//     methods from `st`, ...) get `()`
//   - bare `gets` (not already followed by `(`) -> `gets()`
static std::string rbNormalizeAtoms(const std::string &s,
                                    const RubySymbolTable &st = rbEmptySymbolTable(),
                                    bool strict = true)
{
    // Whole-line decorator invocations (`@property`, `@abstractmethod`,
    // `@name.setter`, `@app.route("/x")`) are legitimate, existing Python-
    // style .sa syntax — never rewrite `@ident` to `self.ident` for these.
    // Only applies in mixed mode: strict Ruby has no decorators, and the
    // interpolation pipeline passes bare `@ivar` snippets through here.
    if (!strict)
    {
        static const std::regex decoratorRe("^@[A-Za-z_][A-Za-z0-9_.]*(\\(.*\\))?$");
        if (std::regex_match(trimCopy(s), decoratorRe))
            return s;
    }

    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();)
    {
        char c = s[i];
        if (rbIsQuote(c))
        {
            size_t end = rbSkipString(s, i);
            out += s.substr(i, end - i);
            i = end;
            continue;
        }
        if (c == '@' && i + 1 < s.size() && rbIsIdentStart(s[i + 1]))
        {
            size_t j = i + 1;
            while (j < s.size() && rbIsIdentChar(s[j]))
                j++;
            out += "self.";
            out += s.substr(i + 1, j - i - 1);
            i = j;
            continue;
        }
        if (rbIsIdentStart(c))
        {
            size_t j = i + 1;
            while (j < s.size() && rbIsIdentChar(s[j]))
                j++;
            std::string ident = s.substr(i, j - i);
            bool hadDotBefore = !out.empty() && out.back() == '.';
            bool suffixStripped = false;
            if (j < s.size() && (s[j] == '?' || s[j] == '!'))
            {
                j++;
                suffixStripped = true;
            }
            bool followedByParen = (j < s.size() && s[j] == '(');
            // "input" is a plain, common Ruby variable name but a reserved
            // statement keyword/native function in Quantum (the scanf/cin
            // -style input statement, and the native input() call already
            // used across many existing .sa scripts) — only rename the bare,
            // paren-less variable-reference form to avoid a parse collision;
            // leave input(...) calls and function definitions untouched.
            if (ident == "input" && !followedByParen)
                ident = "__rb_input";
            out += ident;
            if (!followedByParen && ident == "gets" && !hadDotBefore)
                out += "()";
            // User-defined method names (st.methods) are only auto-parenized
            // in strict mode: in mixed .sa files the symbol table also picks
            // up Python-style `def` methods, and parenizing a bare
            // `obj.method` there would turn a bound-method reference into a
            // call — a silent behavior change.
            else if (!followedByParen && hadDotBefore &&
                     (suffixStripped || rbZeroArgMethods(strict).count(ident) ||
                      (strict && st.methods.count(ident))))
                out += "()";
            i = j;
            continue;
        }
        out += c;
        i++;
    }
    return out;
}

// True only when `code` carries no existing-style markers: no top-level '{'
// anywhere (already brace-style — whether self-closed on one line, like
// `if found { break }`, or opening a multi-line block) and no trailing ':'
// (Python-colon-style). Used to gate Ruby-only block-opener conversions in
// mixed (.sa) mode so already-valid Python/brace/C-style constructs are left
// completely untouched — only unambiguously bare Ruby syntax gets converted.
static bool rbUnambiguous(const std::string &code)
{
    if (!code.empty() && code.back() == ':')
        return false;
    return rbFindTopLevel(code, "{") == std::string::npos;
}

// True when the line contains a C-style comment marker (`//`, `/*`, `*/`)
// outside string literals. Mixed (.sa) mode leaves such lines completely
// untouched — comment prose ("Check if key already exists") would otherwise
// match Ruby patterns like the modifier-if and get mangled.
static bool rbHasCStyleComment(const std::string &s)
{
    for (size_t i = 0; i + 1 < s.size();)
    {
        char c = s[i];
        if (rbIsQuote(c))
        {
            i = rbSkipString(s, i);
            continue;
        }
        if (c == '/' && (s[i + 1] == '/' || s[i + 1] == '*'))
            return true;
        if (c == '*' && s[i + 1] == '/')
            return true;
        i++;
    }
    return false;
}

// True when the line opens a /* block comment it does not close.
static bool rbOpensBlockComment(const std::string &s)
{
    size_t open = s.rfind("/*");
    if (open == std::string::npos)
        return false;
    return s.find("*/", open + 2) == std::string::npos;
}

// Rough opener detection used only by the forward `end`-scan below.
static bool rbLineIsRubyOpener(const std::string &code)
{
    if (!rbUnambiguous(code))
        return false;
    if (startsWith(code, "if ") || startsWith(code, "unless ") ||
        startsWith(code, "while ") || startsWith(code, "until ") ||
        startsWith(code, "def ") || startsWith(code, "class ") ||
        startsWith(code, "case ") || startsWith(code, "module ") ||
        code == "loop" || code == "loop do" || code == "begin")
        return true;
    static const std::regex doRe("\\bdo(\\s*\\|[^|]*\\|)?\\s*$");
    return std::regex_search(code, doRe);
}

// The decisive mixed-mode signal: a genuine Ruby block in a .sa file is
// always terminated by a bare `end` line. Scanning forward with depth
// tracking distinguishes a Ruby opener from the look-alikes that plain
// text can't otherwise separate — C's one-line `while (*s) writeChar(*s++);`,
// a C-style `while (cond)` whose `{` sits on the next line, or Quantum's
// single-line `if x > 0 print("x")` — none of which are followed by a
// matching `end`.
static bool rbHasMatchingEnd(const std::vector<std::string> &lines, size_t openerIdx)
{
    int depth = 1;
    for (size_t i = openerIdx + 1; i < lines.size(); i++)
    {
        std::string t = trimCopy(lines[i]);
        if (t == "end")
        {
            if (--depth == 0)
                return true;
        }
        else if (rbLineIsRubyOpener(t))
            depth++;
    }
    return false;
}

static bool rbLooksLikeReturnable(const std::string &line)
{
    std::string t = trimCopy(line);
    if (t.empty() || t == "}" || t.front() == '}')
        return false;
    static const std::vector<std::string> skip = {
        "return", "raise", "break", "continue", "if (", "if(", "while (",
        "while(", "for (", "for(", "//", "function ", "fn(", "class "};
    for (auto &p : skip)
        if (startsWith(t, p))
            return false;
    if (t.back() == '{')
        return false;
    if (rbFindTopLevelAssign(t) != std::string::npos)
        return false;
    return true;
}

static void rbCollectSymbols(const std::vector<std::string> &lines, RubySymbolTable &st)
{
    static const std::regex ivarRe("@([A-Za-z_][A-Za-z0-9_]*)");
    static const std::regex defRe("^\\s*def\\s+([A-Za-z_][A-Za-z0-9_]*[?!]?)");
    for (auto &line : lines)
    {
        for (auto it = std::sregex_iterator(line.begin(), line.end(), ivarRe);
             it != std::sregex_iterator(); ++it)
            st.fields.insert((*it)[1].str());
        std::smatch m;
        if (std::regex_search(line, m, defRe))
        {
            std::string name = m[1].str();
            if (!name.empty() && (name.back() == '?' || name.back() == '!'))
                name.pop_back();
            if (name != "initialize")
                st.methods.insert(name);
        }
    }
}

// Qualifies bare identifiers that are known class fields/methods with
// `self.` (fields) or `self.name()` (zero-arg methods), and rewrites the
// common `X.class.name` idiom to `classname(X)`. Scoped to a single
// expression snippet (interpolation content or an inline block body) —
// never touches string-literal contents inside that snippet.
static std::string rbQualifySelf(const std::string &expr, const RubySymbolTable &st)
{
    static const std::regex classNameRe("\\b([A-Za-z_][A-Za-z0-9_]*)\\.class\\.name\\b");
    std::string s = std::regex_replace(expr, classNameRe, "classname($1)");

    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();)
    {
        char c = s[i];
        if (rbIsQuote(c))
        {
            size_t end = rbSkipString(s, i);
            out += s.substr(i, end - i);
            i = end;
            continue;
        }
        if (rbIsIdentStart(c))
        {
            size_t j = i + 1;
            while (j < s.size() && rbIsIdentChar(s[j]))
                j++;
            std::string ident = s.substr(i, j - i);
            // Look past an optional `?`/`!` suffix (still present at this
            // stage — rbNormalizeAtoms strips it later) so `palindrome?(x)`
            // is correctly seen as already-parenthesized, not a bare ref.
            size_t afterSuffix = j;
            if (afterSuffix < s.size() && (s[afterSuffix] == '?' || s[afterSuffix] == '!'))
                afterSuffix++;
            bool hadDotBefore = !out.empty() && out.back() == '.';
            bool followedByParen = (afterSuffix < s.size() && s[afterSuffix] == '(');
            if (!hadDotBefore && !followedByParen && st.fields.count(ident))
                out += "self." + ident;
            else if (!hadDotBefore && !followedByParen && st.methods.count(ident))
                out += "self." + ident + "()";
            else
                out += ident;
            i = j;
            continue;
        }
        out += c;
        i++;
    }
    return out;
}

// Converts every double-quoted string in `line` that contains `#{...}` into
// a Quantum backtick template literal (`text${expr}text`), applying
// self-qualification to each embedded expression. Strings without `#{` and
// single-quoted strings are left untouched.
static std::string rbConvertInterpolation(const std::string &line, const RubySymbolTable &st)
{
    std::string out;
    size_t i = 0;
    while (i < line.size())
    {
        char c = line[i];
        if (c == '"')
        {
            size_t end = rbSkipString(line, i);
            std::string raw = line.substr(i, end - i); // includes quotes
            if (raw.find("#{") == std::string::npos)
            {
                out += raw;
                i = end;
                continue;
            }
            // Rebuild as a backtick template: #{expr} -> ${qualified(expr)}
            std::string inner = raw.substr(1, raw.size() >= 2 ? raw.size() - 2 : 0);
            std::string rebuilt = "`";
            size_t p = 0;
            while (p < inner.size())
            {
                size_t hashBrace = inner.find("#{", p);
                if (hashBrace == std::string::npos)
                {
                    rebuilt += inner.substr(p);
                    break;
                }
                rebuilt += inner.substr(p, hashBrace - p);
                size_t exprStart = hashBrace + 2;
                int depth = 1;
                size_t q = exprStart;
                while (q < inner.size() && depth > 0)
                {
                    if (inner[q] == '{')
                        depth++;
                    else if (inner[q] == '}')
                    {
                        depth--;
                        if (depth == 0)
                            break;
                    }
                    q++;
                }
                std::string exprText = inner.substr(exprStart, q - exprStart);
                // Normalize first so `@ivar` becomes `self.ivar` before the
                // qualify pass (qualifying first would produce `@self.ivar`).
                rebuilt += "${" + rbQualifySelf(rbNormalizeAtoms(trimCopy(exprText), st), st) + "}";
                p = (q < inner.size()) ? q + 1 : inner.size();
            }
            rebuilt += "`";
            out += rebuilt;
            i = end;
            continue;
        }
        if (c == '\'' || c == '`')
        {
            size_t end = rbSkipString(line, i);
            out += line.substr(i, end - i);
            i = end;
            continue;
        }
        out += c;
        i++;
    }
    return out;
}

// Converts a standalone parenthesized Ruby range `(A..B)` / `(A...B)` used
// as a method-call receiver into `range(A, B+1)` / `range(A, B)`. Only
// matches parens NOT preceded by an identifier char (so `foo(1..5)`, a
// normal call whose argument happens to be a range, is left alone).
static std::string rbConvertRangesOnce(const std::string &line, bool &changed)
{
    changed = false;
    for (size_t i = 0; i < line.size(); i++)
    {
        if (line[i] != '(')
            continue;
        if (i > 0 && rbIsIdentChar(line[i - 1]))
            continue;
        size_t close = rbMatchBracket(line, i);
        if (close == std::string::npos)
            continue;
        std::string content = line.substr(i + 1, close - i - 1);
        if (rbSplitTopLevel(content, ',').size() != 1)
            continue;
        size_t dotdot = rbFindTopLevel(content, "..");
        if (dotdot == std::string::npos)
            continue;
        bool exclusive = dotdot + 2 < content.size() && content[dotdot + 2] == '.';
        std::string left = trimCopy(content.substr(0, dotdot));
        std::string right = trimCopy(content.substr(dotdot + (exclusive ? 3 : 2)));
        // Inclusive upper bound: "i <= right" as an exclusive bound is
        // floor(right)+1, not right+1 — right may be a non-integer
        // expression (e.g. Math.sqrt(n)), and right+1 would wrongly admit
        // one extra integer whenever right isn't a whole number.
        std::string replacement = exclusive
            ? ("range(" + left + ", " + right + ")")
            : ("range(" + left + ", floor(" + right + ")+1)");
        changed = true;
        return line.substr(0, i) + replacement + line.substr(close + 1);
    }
    return line;
}

static std::string rbConvertRanges(std::string line)
{
    for (int guard = 0; guard < 8; guard++)
    {
        bool changed = false;
        line = rbConvertRangesOnce(line, changed);
        if (!changed)
            break;
    }
    // Ruby's `ClassName.new(args)` constructor call -> Quantum's `ClassName(args)`.
    static const std::regex newCallRe("\\b([A-Za-z_][A-Za-z0-9_]*)\\.new\\(");
    line = std::regex_replace(line, newCallRe, "$1(");
    return line;
}

// Ruby's `arr << expr` append idiom, recognized only when it is the entire
// statement (avoids misfiring on a genuine bitwise left-shift expression).
static bool rbTryPushAppend(const std::string &code, std::string &outResult)
{
    size_t pos = rbFindTopLevel(code, "<<");
    if (pos == std::string::npos)
        return false;
    // `x <<= 1` is a compound shift-assign, never a push.
    if (pos + 2 < code.size() && code[pos + 2] == '=')
        return false;
    std::string lhs = trimCopy(code.substr(0, pos));
    std::string rhs = trimCopy(code.substr(pos + 2));
    if (lhs.empty() || rhs.empty())
        return false;
    // C++ stream insertion (`cout << x << endl`) is valid existing .sa
    // syntax — never a Ruby array push. Chained `<<` in the rhs is the
    // giveaway (Ruby pushes here are single), as is a stream lhs.
    if (lhs == "cout" || lhs == "cerr" || lhs == "clog")
        return false;
    if (rbFindTopLevel(rhs, "<<") != std::string::npos)
        return false;
    static const std::regex lvalRe("^[A-Za-z_][A-Za-z0-9_]*(\\[[^\\]]*\\])?$");
    if (!std::regex_match(lhs, lvalRe))
        return false;
    outResult = lhs + ".push(" + rhs + ")";
    return true;
}

// Ruby's `raise ClassName, "message"` comma form -> `raise ClassName("message")`.
static std::string rbConvertRaiseComma(const std::string &code)
{
    if (!startsWith(code, "raise "))
        return code;
    std::string rest = code.substr(6);
    size_t comma = rbFindTopLevel(rest, ",");
    if (comma == std::string::npos)
        return code;
    std::string ident = trimCopy(rest.substr(0, comma));
    static const std::regex identRe("^[A-Za-z_][A-Za-z0-9_]*$");
    if (!std::regex_match(ident, identRe))
        return code;
    std::string msg = trimCopy(rest.substr(comma + 1));
    return "raise " + ident + "(" + msg + ")";
}

// Ruby's bare `.split` (no args, whitespace-run splitting) -> a trimmed
// regex split, since Quantum's split() with an empty separator splits into
// individual characters instead.
static std::string rbConvertBareSplit(const std::string &code)
{
    static const std::regex bareSplitRe("\\.split(?:\\(\\s*\\))?(?!\\()");
    std::smatch m;
    std::string s = code;
    std::string result;
    size_t last = 0;
    auto begin = std::sregex_iterator(s.begin(), s.end(), bareSplitRe);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it)
    {
        // The trailing (?!\() guards against `.split(pattern)` — a real
        // argument means the optional empty-parens group can't consume it,
        // so the lookahead fails and this call is left untouched.
        result += s.substr(last, it->position() - last);
        result += ".trim().split(" + std::string(R"("/\\s+/")") + ")";
        last = it->position() + it->length();
    }
    result += s.substr(last);
    return result;
}

struct RubyBlockCall
{
    std::string prefix; // receiver+method(+args) before the block
    std::string params;
    std::string body;   // only for single-line `{ ... }` form
    std::string suffix; // text after the block (rare)
};

// Detects a trailing single-line `{ |params| body }` Ruby block attached to
// a call. Ruby hashes are not in scope for this dialect, so any top-level
// `{` on a statement line is treated as a block.
// True only when `prefix` (the text immediately before a `{`) genuinely
// ends in a call to a known block-accepting Ruby method (or is the bare
// `loop` keyword). This is what distinguishes a real Ruby block —
// `arr.each { |x| ... }`, `(2..n).none? { ... }` — from a `{` that belongs
// to something else entirely: a dict/hash literal (`x = {"a": 1}`), a
// brace-style if/else continuation (`} else if (...) {`), or a bare
// single-line brace-complete statement (`if found { break }`). Without
// this check every one of those non-Ruby cases would be misread as a block.
static bool rbPrefixEndsWithBlockMethod(const std::string &prefix)
{
    static const std::regex blockMethodRe(
        "(^loop$)|(\\.(each|each_with_index|map|select|filter|reduce|none|any|"
        "all|every|some|times|sort_by|reject|find|detect)[?!]?(\\([^()]*\\))?$)");
    return std::regex_search(prefix, blockMethodRe);
}

static bool rbTryInlineBraceBlock(const std::string &code, RubyBlockCall &out)
{
    if (!code.empty() && code[0] == '}')
        return false; // continuation of an earlier brace construct, not a fresh block
    size_t open = rbFindTopLevel(code, "{");
    if (open == std::string::npos)
        return false;
    size_t close = rbMatchBracket(code, open);
    if (close == std::string::npos)
        return false;
    out.prefix = trimCopy(code.substr(0, open));
    if (out.prefix.empty() || !rbPrefixEndsWithBlockMethod(out.prefix))
        return false;
    std::string content = trimCopy(code.substr(open + 1, close - open - 1));
    out.suffix = trimCopy(code.substr(close + 1));
    if (!content.empty() && content[0] == '|')
    {
        size_t p2 = content.find('|', 1);
        if (p2 != std::string::npos)
        {
            out.params = trimCopy(content.substr(1, p2 - 1));
            out.body = trimCopy(content.substr(p2 + 1));
            return true;
        }
    }
    out.params = "";
    out.body = content;
    return true;
}

// Detects a trailing multi-line `do |params|` (or bare `do`) block opener.
static bool rbTryTrailingDoOpener(const std::string &code, std::string &prefix, std::string &params)
{
    static const std::regex doRe("^(.*?)\\bdo(?:\\s*\\|([^|]*)\\|)?\\s*$");
    std::smatch m;
    if (!std::regex_match(code, m, doRe))
        return false;
    prefix = trimCopy(m[1].str());
    params = trimCopy(m[2].str());
    return true;
}

// Builds the call-opening text for a Ruby block: appends the closure as an
// extra call argument, e.g. "arr.each" + "x" -> "arr.each(fn(x) {" and
// "arr.reduce(0)" + "acc,x" -> "arr.reduce(0, fn(acc,x) {".
static std::string rbBuildBlockOpenText(const std::string &prefix, const std::string &params)
{
    if (!prefix.empty() && prefix.back() == ')')
    {
        std::string base = prefix.substr(0, prefix.size() - 1);
        bool emptyArgs = !base.empty() && base.back() == '(';
        return base + (emptyArgs ? "" : ", ") + "fn(" + params + ") {";
    }
    return prefix + "(fn(" + params + ") {";
}

// ── Block-stack frame kinds for the main transform pass ───────────────────
enum class RBFrameKind { Other, Def, ClosureDo, Branch, Loop };
enum class RBTailKind { None, Statement, Chain };

struct RBTailInfo
{
    RBTailKind kind = RBTailKind::None;
    int stmtIndex = -1;
    int chainGroup = -1;
};

struct RBFrame
{
    RBFrameKind kind = RBFrameKind::Other;
    int chainGroupId = -1;
    RBTailInfo last;
};

// Extracts `module Name ... end` blocks from `lines`, storing their raw body
// text keyed by module name, and removes them from the line list (replaced
// with blanks to preserve line numbers). Any `include Name` line still
// present afterward is expanded to the stored module body in-place.
static std::vector<std::string> rbFlattenModules(std::vector<std::string> lines)
{
    static const std::regex moduleRe("^module\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*$");
    static const std::regex includeRe("^include\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*$");

    auto isOpener = [](const std::string &t) -> bool
    {
        if (startsWith(t, "module ") || startsWith(t, "class ") || startsWith(t, "def ") ||
            t == "begin" || startsWith(t, "if ") || startsWith(t, "unless ") ||
            startsWith(t, "while ") || startsWith(t, "until ") || startsWith(t, "case "))
            return true;
        static const std::regex doRe("\\bdo(\\s*\\|[^|]*\\|)?\\s*$");
        return std::regex_search(t, doRe);
    };

    std::map<std::string, std::vector<std::string>> modules;

    for (size_t i = 0; i < lines.size(); i++)
    {
        std::string t = trimCopy(lines[i]);
        std::smatch m;
        if (!std::regex_match(t, m, moduleRe))
            continue;
        std::string name = m[1].str();
        int depth = 1;
        size_t j = i + 1;
        std::vector<std::string> body;
        while (j < lines.size() && depth > 0)
        {
            std::string tj = trimCopy(lines[j]);
            if (tj == "end")
            {
                depth--;
                if (depth == 0)
                    break;
            }
            else if (isOpener(tj))
                depth++;
            body.push_back(lines[j]);
            j++;
        }
        modules[name] = body;
        for (size_t k = i; k <= j && k < lines.size(); k++)
            lines[k] = "";
    }

    std::vector<std::string> result;
    for (auto &line : lines)
    {
        std::smatch m;
        std::string t = trimCopy(line);
        if (std::regex_match(t, m, includeRe) && modules.count(m[1].str()))
            for (auto &bl : modules[m[1].str()])
                result.push_back(bl);
        else
            result.push_back(line);
    }
    return result;
}

// Expands Ruby multiple assignment (`a, b = b, a`, `arr[i], arr[j] = arr[j],
// arr[i]`, `a, op, b = x, y, z`) into temporary-variable assignments.
// Quantum's native unpack-assignment only recognizes plain identifier
// targets and doesn't reliably support this idiom, so we sidestep it
// entirely rather than depend on it.
static bool rbTryMultiAssign(const std::string &code, std::vector<std::string> &outLines)
{
    size_t eq = rbFindTopLevelAssign(code);
    if (eq == std::string::npos)
        return false;
    std::string lhs = trimCopy(code.substr(0, eq));
    std::string rhs = trimCopy(code.substr(eq + 1));
    auto targets = rbSplitTopLevel(lhs, ',');
    auto values = rbSplitTopLevel(rhs, ',');
    if (targets.size() < 2 || targets.size() != values.size())
        return false;
    static const std::regex targetRe("^[A-Za-z_][A-Za-z0-9_]*(\\[[^\\]]*\\]|\\.[A-Za-z_][A-Za-z0-9_]*)*$");
    for (auto &t : targets)
        if (!std::regex_match(t, targetRe))
            return false;

    static int counter = 0;
    std::string tag = "__ma" + std::to_string(counter++) + "_";
    for (size_t k = 0; k < values.size(); k++)
        outLines.push_back(tag + std::to_string(k) + " = " + values[k]);
    for (size_t k = 0; k < targets.size(); k++)
        outLines.push_back(targets[k] + " = " + tag + std::to_string(k));
    return true;
}

// `strict` = true for pure `.rb` files: every recognized Ruby construct
// converts unconditionally, since the whole file is Ruby.
// `strict` = false for `.sa` files, where Ruby is one of several accepted
// styles in the same file: block-openers only convert when `rbUnambiguous`
// confirms the line isn't already valid Python-colon or brace style.
static std::string applyRubyDialect(const std::string &source, bool strict)
{
    // Split into raw lines up front (module flattening needs random access).
    std::vector<std::string> rawLines;
    {
        std::istringstream input(source);
        std::string line;
        while (std::getline(input, line))
            rawLines.push_back(line);
    }
    rawLines = rbFlattenModules(std::move(rawLines));

    RubySymbolTable symbols;
    rbCollectSymbols(rawLines, symbols);

    std::vector<std::string> outLines;
    std::vector<RBFrame> stack;
    stack.push_back(RBFrame{}); // file-scope sentinel, never resolved
    std::map<int, std::vector<RBTailInfo>> branchGroups;
    int nextChainGroup = 0;
    std::map<size_t, std::string> caseSubjects;    // stack-depth -> subject, before first `when`
    std::map<int, std::string> caseSubjectByGroup; // chainGroupId -> subject, after first `when`

    auto markStatement = [&](int idx)
    {
        if (!stack.empty())
            stack.back().last = RBTailInfo{RBTailKind::Statement, idx, -1};
    };
    auto emit = [&](const std::string &indentation, const std::string &code, bool trackAsStatement)
    {
        outLines.push_back(indentation + code);
        if (trackAsStatement)
            markStatement((int)outLines.size() - 1);
    };

    std::function<void(const RBTailInfo &)> resolveTail = [&](const RBTailInfo &info)
    {
        if (info.kind == RBTailKind::Statement)
        {
            std::string &ln = outLines[info.stmtIndex];
            // `case/when ... then EXPR` (and modifier-if) share their line
            // with an opener, e.g. `if (cond) { a + b` — target the
            // statement text after the last "{ ", not the whole line.
            size_t braceSpace = ln.rfind("{ ");
            size_t stmtStart = (braceSpace != std::string::npos)
                                    ? braceSpace + 2
                                    : ln.find_first_not_of(" \t");
            if (stmtStart == std::string::npos)
                stmtStart = ln.size();
            if (rbLooksLikeReturnable(ln.substr(stmtStart)))
                ln.insert(stmtStart, "return ");
        }
        else if (info.kind == RBTailKind::Chain)
        {
            for (auto &b : branchGroups[info.chainGroup])
                resolveTail(b);
        }
    };

    // Transforms a single "leaf" statement fragment: raise-comma, puts/print,
    // inline blocks, `<<` push, interpolation, bare-split, atom normalization.
    std::function<std::string(const std::string &)> transformCore =
        [&](const std::string &codeIn) -> std::string
    {
        std::string code = codeIn;

        if (startsWith(code, "attr_reader") || startsWith(code, "attr_accessor") ||
            startsWith(code, "attr_writer"))
            return ""; // direct field access already works; no getter needed

        code = rbConvertRaiseComma(code);

        if (startsWith(code, "puts("))
            code = "print" + code.substr(4);
        else if (startsWith(code, "puts "))
            code = "print(" + trimCopy(code.substr(5)) + ")";
        else if (startsWith(code, "print "))
            code = "print(" + trimCopy(code.substr(6)) + ")";
        else
        {
            // Ruby also allows paren-less calls on a receiver, e.g.
            // `file.puts "text"` — wrap the argument in parens.
            static const std::regex dotPutsRe("^(.+\\.puts)\\s+([^(].*)$");
            std::smatch m;
            if (std::regex_match(code, m, dotPutsRe))
                code = m[1].str() + "(" + m[2].str() + ")";
        }

        RubyBlockCall block;
        if (rbTryInlineBraceBlock(code, block))
        {
            std::string prefix = rbNormalizeAtoms(rbConvertRanges(block.prefix), symbols, strict);
            std::string body = transformCore(block.body);
            if (rbLooksLikeReturnable(body))
                body = "return " + body;
            std::string opened = rbBuildBlockOpenText(prefix, block.params);
            code = opened + " " + body + " })" + block.suffix;
        }

        std::string pushed;
        if (rbTryPushAppend(code, pushed))
            code = pushed;

        code = rbConvertBareSplit(code);
        code = rbConvertRanges(code);
        code = rbConvertInterpolation(code, symbols);
        code = rbNormalizeAtoms(code, symbols, strict);
        if (code == "nil")
            code = "null";
        return code;
    };

    bool inBlockComment = false;
    for (size_t li = 0; li < rawLines.size(); li++)
    {
        const std::string &rawLine = rawLines[li];
        size_t first = rawLine.find_first_not_of(" \t");
        if (first == std::string::npos)
        {
            outLines.push_back("");
            continue;
        }
        std::string indentation = rawLine.substr(0, first);
        std::string code = trimCopy(rawLine.substr(first));

        // Mixed (.sa) mode: pass comment-bearing lines through verbatim.
        // Comment prose routinely contains words like "if"/"unless" that
        // would otherwise trip the Ruby line patterns below. (Strict .rb
        // files use # comments, handled separately underneath.)
        if (!strict)
        {
            if (inBlockComment)
            {
                if (code.find("*/") != std::string::npos)
                    inBlockComment = false;
                outLines.push_back(rawLine);
                continue;
            }
            if (rbHasCStyleComment(code))
            {
                if (rbOpensBlockComment(code))
                    inBlockComment = true;
                outLines.push_back(rawLine);
                continue;
            }
        }

        if (!code.empty() && code[0] == '#')
        {
            outLines.push_back(indentation + "//" + code.substr(1));
            continue;
        }

        // ── Multiple assignment (whole-statement only) ──────────────────
        {
            std::vector<std::string> expanded;
            if (rbTryMultiAssign(code, expanded))
            {
                for (size_t k = 0; k < expanded.size(); k++)
                {
                    std::string ln = rbConvertInterpolation(rbConvertRanges(expanded[k]), symbols);
                    ln = rbNormalizeAtoms(ln, symbols, strict);
                    emit(indentation, ln, k + 1 == expanded.size());
                }
                continue;
            }
        }

        // ── Modifier if/unless (trailing, never at line-start) ──────────
        {
            static const std::regex modIfRe("^(.*\\S)\\s+(if|unless)\\s+(.+)$");
            std::smatch m;
            if (!startsWith(code, "if ") && !startsWith(code, "unless ") &&
                // A Ruby modifier-if line is a plain brace-less statement;
                // anything containing a top-level `{`, starting with `}`
                // (e.g. `} else if cond {` continuations), or containing
                // `else if` (C/JS chaining) is native syntax — skip it.
                rbUnambiguous(code) && code[0] != '}' &&
                rbFindTopLevel(code, "else if ") == std::string::npos &&
                std::regex_match(code, m, modIfRe) &&
                rbFindTopLevel(code, " " + m[2].str() + " ") != std::string::npos &&
                // Python inline ternary (`x = a if cond else b`) also matches
                // this shape — its giveaway is a top-level ` else `, which a
                // Ruby modifier-if never has. Leave it to the native parser.
                rbFindTopLevel(m[3].str(), " else ") == std::string::npos)
            {
                std::string stmt = transformCore(trimCopy(m[1].str()));
                std::string cond = rbNormalizeAtoms(rbConvertInterpolation(rbConvertRanges(trimCopy(m[3].str())), symbols), symbols, strict);
                std::string wrappedCond = m[2].str() == "unless" ? "!(" + cond + ")" : cond;
                if (!stmt.empty())
                {
                    emit(indentation, "if (" + wrappedCond + ") { " + stmt + " }", true);
                }
                continue;
            }
        }

        // elsif/else/end only transform when the innermost open frame is one
        // Ruby-mode itself pushed (stack beyond the file-scope sentinel) —
        // otherwise the line belongs to native syntax and falls through.
        if (startsWith(code, "elsif ") && stack.size() > 1 &&
            stack.back().kind == RBFrameKind::Branch)
        {
            branchGroups[stack.back().chainGroupId].push_back(stack.back().last);
            int gid = stack.back().chainGroupId;
            stack.pop_back();
            std::string cond = rbNormalizeAtoms(rbConvertInterpolation(rbConvertRanges(trimCopy(code.substr(6))), symbols), symbols, strict);
            outLines.push_back(indentation + "} else if (" + cond + ") {");
            stack.push_back(RBFrame{RBFrameKind::Branch, gid, RBTailInfo{}});
            continue;
        }
        if (code == "else" && stack.size() > 1 &&
            stack.back().kind == RBFrameKind::Branch)
        {
            branchGroups[stack.back().chainGroupId].push_back(stack.back().last);
            int gid = stack.back().chainGroupId;
            stack.pop_back();
            outLines.push_back(indentation + "} else {");
            stack.push_back(RBFrame{RBFrameKind::Branch, gid, RBTailInfo{}});
            continue;
        }
        if (code == "end" && stack.size() > 1)
        {
            {
                RBFrame top = stack.back();
                stack.pop_back();
                if (top.kind == RBFrameKind::Branch)
                {
                    branchGroups[top.chainGroupId].push_back(top.last);
                    outLines.push_back(indentation + "}");
                    if (!stack.empty())
                        stack.back().last = RBTailInfo{RBTailKind::Chain, -1, top.chainGroupId};
                }
                else if (top.kind == RBFrameKind::ClosureDo)
                {
                    outLines.push_back(indentation + "})");
                    resolveTail(top.last);
                    if (!stack.empty())
                        stack.back().last = RBTailInfo{};
                }
                else if (top.kind == RBFrameKind::Def)
                {
                    outLines.push_back(indentation + "}");
                    resolveTail(top.last);
                    if (!stack.empty())
                        stack.back().last = RBTailInfo{};
                }
                else
                {
                    outLines.push_back(indentation + "}");
                    if (!stack.empty())
                        stack.back().last = RBTailInfo{};
                }
            }
            continue;
        }

        if (startsWith(code, "unless ") && (strict || (rbUnambiguous(code) && rbHasMatchingEnd(rawLines, li))))
        {
            std::string cond = rbNormalizeAtoms(rbConvertInterpolation(rbConvertRanges(trimCopy(code.substr(7))), symbols), symbols, strict);
            outLines.push_back(indentation + "if (!(" + cond + ")) {");
            stack.push_back(RBFrame{RBFrameKind::Branch, nextChainGroup++, RBTailInfo{}});
            continue;
        }
        if (startsWith(code, "if ") && (strict || (rbUnambiguous(code) && rbHasMatchingEnd(rawLines, li))))
        {
            std::string cond = rbNormalizeAtoms(rbConvertInterpolation(rbConvertRanges(trimCopy(code.substr(3))), symbols), symbols, strict);
            outLines.push_back(indentation + "if (" + cond + ") {");
            stack.push_back(RBFrame{RBFrameKind::Branch, nextChainGroup++, RBTailInfo{}});
            continue;
        }
        if (startsWith(code, "until ") && (strict || (rbUnambiguous(code) && rbHasMatchingEnd(rawLines, li))))
        {
            std::string cond = rbNormalizeAtoms(rbConvertInterpolation(rbConvertRanges(trimCopy(code.substr(6))), symbols), symbols, strict);
            outLines.push_back(indentation + "while (!(" + cond + ")) {");
            stack.push_back(RBFrame{RBFrameKind::Loop, -1, RBTailInfo{}});
            continue;
        }
        if (startsWith(code, "while ") && (strict || (rbUnambiguous(code) && rbHasMatchingEnd(rawLines, li))))
        {
            std::string cond = rbNormalizeAtoms(rbConvertInterpolation(rbConvertRanges(trimCopy(code.substr(6))), symbols), symbols, strict);
            outLines.push_back(indentation + "while (" + cond + ") {");
            stack.push_back(RBFrame{RBFrameKind::Loop, -1, RBTailInfo{}});
            continue;
        }
        if ((code == "loop" || code == "loop do") && (strict || (rbUnambiguous(code) && rbHasMatchingEnd(rawLines, li))))
        {
            outLines.push_back(indentation + "while (true) {");
            stack.push_back(RBFrame{RBFrameKind::Loop, -1, RBTailInfo{}});
            continue;
        }
        if (startsWith(code, "begin") && trimCopy(code) == "begin" && (strict || (rbUnambiguous(code) && rbHasMatchingEnd(rawLines, li))))
        {
            outLines.push_back(indentation + "try {");
            stack.push_back(RBFrame{RBFrameKind::Other, -1, RBTailInfo{}});
            continue;
        }
        // rescue/ensure only continue a begin-block that Ruby-mode itself
        // opened (stack beyond the file-scope sentinel).
        if (stack.size() > 1)
        {
            static const std::regex rescueRe("^rescue(\\s+[A-Za-z_][A-Za-z0-9_:]*)?\\s*(=>\\s*(\\w+))?\\s*$");
            std::smatch m;
            if (std::regex_match(code, m, rescueRe))
            {
                std::string var = m[3].matched ? m[3].str() : "e";
                stack.pop_back();
                outLines.push_back(indentation + "} catch (" + var + ") {");
                stack.push_back(RBFrame{RBFrameKind::Other, -1, RBTailInfo{}});
                continue;
            }
        }
        if (code == "ensure" && stack.size() > 1)
        {
            stack.pop_back();
            outLines.push_back(indentation + "} finally {");
            stack.push_back(RBFrame{RBFrameKind::Other, -1, RBTailInfo{}});
            continue;
        }

        if (startsWith(code, "case ") && (strict || (rbUnambiguous(code) && rbHasMatchingEnd(rawLines, li))))
        {
            std::string subject = rbNormalizeAtoms(rbConvertInterpolation(rbConvertRanges(trimCopy(code.substr(5))), symbols), symbols, strict);
            // Stash the subject via a marker frame; the first `when` opens
            // the actual branch. We reuse Other with chainGroupId encoding
            // deferred via a side map keyed by stack depth.
            outLines.push_back("");
            RBFrame f{RBFrameKind::Other, -2, RBTailInfo{}};
            caseSubjects[stack.size()] = subject;
            stack.push_back(f);
            continue;
        }
        {
            static const std::regex whenRe("^when\\s+(.+?)(\\s+then\\s+(.+))?$");
            std::smatch m;
            if (std::regex_match(code, m, whenRe) && stack.size() > 1 &&
                (stack.back().chainGroupId == -2 || stack.back().kind == RBFrameKind::Branch))
            {
                bool isFirst = stack.back().chainGroupId == -2;
                std::string subject = isFirst ? caseSubjects[stack.size() - 1] : "";
                int gid;
                if (isFirst)
                {
                    stack.pop_back();
                    gid = nextChainGroup++;
                }
                else
                {
                    branchGroups[stack.back().chainGroupId].push_back(stack.back().last);
                    gid = stack.back().chainGroupId;
                    stack.pop_back();
                }
                std::string valuesText = rbNormalizeAtoms(rbConvertInterpolation(rbConvertRanges(m[1].str()), symbols), symbols, strict);
                auto values = rbSplitTopLevel(valuesText, ',');
                std::string subj = isFirst ? subject : caseSubjectByGroup[gid];
                caseSubjectByGroup[gid] = subj;
                std::string cond;
                for (size_t k = 0; k < values.size(); k++)
                {
                    if (k)
                        cond += " || ";
                    cond += "(" + subj + " == " + values[k] + ")";
                }
                std::string opener = (isFirst ? "if (" : "} else if (") + cond + ") {";
                if (m[3].matched)
                {
                    std::string stmt = transformCore(trimCopy(m[3].str()));
                    outLines.push_back(indentation + opener + " " + stmt);
                    stack.push_back(RBFrame{RBFrameKind::Branch, gid, RBTailInfo{RBTailKind::Statement, (int)outLines.size() - 1, -1}});
                }
                else
                {
                    outLines.push_back(indentation + opener);
                    stack.push_back(RBFrame{RBFrameKind::Branch, gid, RBTailInfo{}});
                }
                continue;
            }
        }

        if (startsWith(code, "class ") && (strict || (rbUnambiguous(code) && rbHasMatchingEnd(rawLines, li))))
        {
            std::string rest = trimCopy(code.substr(6));
            static const std::regex classRe("^([A-Za-z_][A-Za-z0-9_]*)\\s*(<\\s*([A-Za-z_][A-Za-z0-9_]*))?\\s*$");
            std::smatch m;
            std::string header;
            if (std::regex_match(rest, m, classRe))
                header = m[3].matched ? ("class " + m[1].str() + " extends " + m[3].str())
                                       : ("class " + m[1].str());
            else
                header = "class " + rest;
            outLines.push_back(indentation + header + " {");
            stack.push_back(RBFrame{RBFrameKind::Other, -1, RBTailInfo{}});
            continue;
        }
        {
            // Only the exact Ruby `include ModuleName` marker is dropped
            // (already spliced by rbFlattenModules); anything else that
            // happens to start with "include " falls through untouched.
            static const std::regex includeMarkerRe("^include\\s+[A-Za-z_][A-Za-z0-9_]*$");
            if (std::regex_match(code, includeMarkerRe))
                continue;
        }

        if (startsWith(code, "def ") && (strict || (rbUnambiguous(code) && rbHasMatchingEnd(rawLines, li))))
        {
            std::string signature = trimCopy(code.substr(4));
            static const std::regex initRe("^initialize(\\s*\\(.*)?$");
            signature = std::regex_replace(signature, initRe, "init$1");
            signature = rbNormalizeAtoms(signature, symbols, strict);
            if (signature.find('(') == std::string::npos)
                signature += "()"; // Ruby allows parameter-less `def name`
            outLines.push_back(indentation + "function " + signature + " {");
            stack.push_back(RBFrame{RBFrameKind::Def, -1, RBTailInfo{}});
            continue;
        }

        std::string prefix, params;
        if (rbTryTrailingDoOpener(code, prefix, params) && !prefix.empty() &&
            (strict || (rbUnambiguous(code) && rbHasMatchingEnd(rawLines, li))))
        {
            std::string convertedPrefix = rbNormalizeAtoms(rbConvertRanges(prefix), symbols, strict);
            outLines.push_back(indentation + rbBuildBlockOpenText(convertedPrefix, params));
            stack.push_back(RBFrame{RBFrameKind::ClosureDo, -1, RBTailInfo{}});
            continue;
        }

        std::string finalCode = transformCore(code);
        if (finalCode.empty())
        {
            outLines.push_back("");
            continue;
        }
        emit(indentation, finalCode, true);
    }

    std::string result;
    for (auto &l : outLines)
    {
        result += l;
        result += "\n";
    }
    return result;
}

// C/C++ programs only define main() — append a call so the program executes
// after its top-level declarations load. Ruby files are first normalized into
// the common Quantum syntax and then use the normal compiler/VM pipeline.
static std::string applyDialect(std::string source, const std::string &path)
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

// ─── runFile — interpret a source file in-place (no exe created) ──────────────

static void runFile(const std::string &path, bool debug = false)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        std::cerr << Colors::RED << "[Error] " << Colors::RESET
                  << "Cannot open file: " << path << "\n";
        std::exit(1);
    }
    std::ostringstream ss;
    ss << file.rdbuf();

    try
    {
        VM vm;
        vm.run(compileSource(applyDialect(ss.str(), path), path, debug));
    }
    catch (const ParseError &e)
    {
        std::cerr << Colors::RED << Colors::BOLD
                  << "\n  X ParseError" << Colors::RESET
                  << " in " << path << " at line " << e.line << ":" << e.col
                  << "\n    " << e.what() << "\n\n";
        std::exit(1);
    }
    catch (const QuantumError &e)
    {
        std::cerr << Colors::RED << Colors::BOLD
                  << "\n  X " << e.kind << Colors::RESET;
        if (e.line > 0)
            std::cerr << " at line " << e.line;
        std::cerr << "\n    " << e.what() << "\n\n";
        std::exit(1);
    }
    catch (const std::exception &e)
    {
        std::cerr << Colors::RED << "[Fatal] " << Colors::RESET << e.what() << "\n";
        std::exit(1);
    }
}

// ─── checkFile ────────────────────────────────────────────────────────────────

static int checkFile(const std::string &path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        std::cerr << path << ":1:1: error: Cannot open\n";
        return 1;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    try
    {
        Lexer l(applyDialect(ss.str(), path));
        auto tok = l.tokenize();
        Parser p(std::move(tok));
        auto ast = p.parse();
        try
        {
            TypeChecker tc;
            tc.check(ast);
        }
        catch (const StaticTypeError &e)
        {
            std::cerr << path << ":" << e.line << ":1: warning: " << e.what() << "\n";
        }
        std::cout << Colors::GREEN << "[OK] " << Colors::RESET << path << "\n";
        return 0;
    }
    catch (const ParseError &e)
    {
        std::cerr << path << ":" << e.line << ":" << e.col << ": error: " << e.what() << "\n";
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << path << ":1:1: error: " << e.what() << "\n";
        return 1;
    }
}

// ─── Batch test ───────────────────────────────────────────────────────────────

struct TestResult
{
    std::string path, source, error, output;
    int line = 0, col = 0;
    bool passed = false;
    bool crashed = false; // true when a Win32 SEH fault was caught
};

static void redirectStdinToNull()
{
#ifdef _WIN32
    FILE *n = nullptr;
    freopen_s(&n, "NUL", "r", stdin);
#else
    if (!freopen("/dev/null", "r", stdin)) { /* ignore */ }
#endif
}

static bool isInputDriven(const std::string &m)
{
    return m.find("got string") != m.npos || m.find("got nil") != m.npos ||
           m.find("Cannot convert ''") != m.npos;
}

// ── Crash-guarded VM execution ───────────────────────────────────────────────
// MinGW/GCC does not support __try/__except.  Instead we use POSIX signals
// (SIGSEGV / SIGFPE / SIGILL / SIGABRT) combined with setjmp/longjmp to
// intercept hard crashes without killing the whole process.
//
// The pattern:
//   1. Install signal handlers that longjmp back to a safe point.
//   2. setjmp() — if a signal fires, longjmp brings us back here with a
//      non-zero value that encodes which signal hit.
//   3. Run the VM.
//   4. Restore the original signal handlers.
//
// Limitation: longjmp out of a signal handler is technically UB in C++, but
// it is the standard approach on MinGW/GCC Windows where SEH is unavailable,
// and works reliably in practice for our use-case (test runner, not production).

static jmp_buf g_crashJmpBuf;
static int g_crashSignal = 0; // signal number that fired, 0 = none

static void crashSignalHandler(int sig)
{
    g_crashSignal = sig;
    // Re-install the handler so repeated signals work (required on some targets)
    signal(sig, crashSignalHandler);
    longjmp(g_crashJmpBuf, sig);
}

static std::string runVmGuarded(const std::string &source,
                                const std::string &path,
                                std::string &outCapture)
{
    // --- set up output capture ---
    std::ostringstream sink;
    std::streambuf *savedOut = std::cout.rdbuf(sink.rdbuf());
    std::streambuf *savedErr = std::cerr.rdbuf(sink.rdbuf());

    // --- install crash signal handlers ---
    g_crashSignal = 0;
    auto prevSEGV = signal(SIGSEGV, crashSignalHandler);
    auto prevFPE = signal(SIGFPE, crashSignalHandler);
    auto prevILL = signal(SIGILL, crashSignalHandler);
    auto prevABRT = signal(SIGABRT, crashSignalHandler);

    std::string errorMsg;

    int jumpVal = setjmp(g_crashJmpBuf);
    if (jumpVal == 0)
    {
        // Normal path — run the VM
        try
        {
            VM vm;
            vm.run(compileSource(applyDialect(source, path), path, false));
        }
        catch (...)
        {
            // Restore before re-throwing so the caller's catch blocks work
            signal(SIGSEGV, prevSEGV);
            signal(SIGFPE, prevFPE);
            signal(SIGILL, prevILL);
            signal(SIGABRT, prevABRT);
            std::cout.rdbuf(savedOut);
            std::cerr.rdbuf(savedErr);
            outCapture = sink.str();
            throw;
        }
    }
    else
    {
        // Signal fired — longjmp landed here
        switch (jumpVal)
        {
        case SIGSEGV:
            errorMsg = "CrashError: Segmentation fault (stack overflow or bad memory access)";
            break;
        case SIGFPE:
            errorMsg = "CrashError: Floating point exception";
            break;
        case SIGILL:
            errorMsg = "CrashError: Illegal instruction";
            break;
        case SIGABRT:
            errorMsg = "CrashError: Abort signal (assertion or OOM)";
            break;
        default:
            errorMsg = "CrashError: Unknown signal " + std::to_string(jumpVal);
            break;
        }
    }

    // Restore signal handlers and streams
    signal(SIGSEGV, prevSEGV);
    signal(SIGFPE, prevFPE);
    signal(SIGILL, prevILL);
    signal(SIGABRT, prevABRT);
    std::cout.rdbuf(savedOut);
    std::cerr.rdbuf(savedErr);
    outCapture = sink.str();
    return errorMsg;
}

static TestResult testFile(const std::string &path)
{
    TestResult res;
    res.path = path;

    // ── Read source ──────────────────────────────────────────────────────────
    std::ifstream f(path);
    if (!f.is_open())
    {
        res.error = "Cannot open file";
        return res;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    res.source = ss.str();

    // ── Lex + parse ──────────────────────────────────────────────────────────
    // Dialect-translate first (.rb/.c/.cpp) — parsing the raw source here would
    // reject valid Ruby/C/C++ files before the real (dialect-aware) run below.
    try
    {
        Lexer l(applyDialect(res.source, path));
        auto tok = l.tokenize();
        Parser p(std::move(tok));
        auto ast = p.parse();
        (void)ast;
    }
    catch (const ParseError &e)
    {
        res.error = "ParseError: " + std::string(e.what());
        res.line = e.line;
        res.col = e.col;
        return res;
    }
    catch (const std::exception &e)
    {
        res.error = "LexError: " + std::string(e.what());
        res.line = 1;
        return res;
    }
    catch (...)
    {
        res.error = "LexError: unknown";
        return res;
    }

    // ── Compile + run (SEH-guarded so a crash can't kill the process) ────────
    std::string sehError;
    try
    {
        sehError = runVmGuarded(res.source, path, res.output);
    }
    catch (const ParseError &e)
    {
        if (!isInputDriven(e.what()))
        {
            res.error = "ParseError: " + std::string(e.what());
            res.line = e.line;
        }
    }
    catch (const QuantumError &e)
    {
        if (!isInputDriven(e.what()))
        {
            res.error = e.kind + ": " + std::string(e.what());
            res.line = e.line;
        }
    }
    catch (const std::exception &e)
    {
        if (!isInputDriven(e.what()))
            res.error = "Fatal: " + std::string(e.what());
    }
    catch (...)
    {
        res.error = "Fatal: unknown exception";
    }

    // SEH error takes priority if set
    if (!sehError.empty())
    {
        res.error = sehError;
        res.crashed = true;
    }

    res.passed = res.error.empty();
    return res;
}

// Lists one directory level, then recurses into its subdirectories
// separately (rather than using a single fs::recursive_directory_iterator).
// This matters because a nested checkout's .git/ directory (long hashed
// object paths, reparse points) can throw a filesystem_error that isn't a
// plain permission-denied — recursive_directory_iterator has no way to skip
// just that one subtree, so the error silently aborts every sibling
// directory that would have been visited afterward in the same lazy walk.
// Gathering this level's subdirectories up front and recursing into each
// independently means a failure in one subtree only loses that subtree.
static void collectTestFilesRecursive(const fs::path &dir, std::vector<fs::path> &out)
{
    std::vector<fs::path> subdirs;
    try
    {
        for (auto &e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied))
        {
            if (e.is_directory())
                subdirs.push_back(e.path());
            else if (e.is_regular_file() && hasSupportedExt(e.path().string()))
                out.push_back(e.path());
        }
    }
    catch (const fs::filesystem_error &)
    {
        return; // this subtree is unreadable — skip it, siblings are unaffected
    }
    for (auto &sub : subdirs)
        collectTestFilesRecursive(sub, out);
}

static void collectTestFiles(const fs::path &dir, std::vector<fs::path> &out)
{
    // Any file type the compiler runs natively is testable:
    // .sa .js .py .rb .c .cpp — all share the same multi-syntax pipeline.
    if (!fs::exists(dir) || !fs::is_directory(dir))
        return;
    collectTestFilesRecursive(dir, out);
}

// ── Write test_results.txt ────────────────────────────────────────────────────
// • All files listed (PASS / FAIL)
// • For every FAIL: error, location, captured output, and the FULL source code
// ── Progressive report — written incrementally so crashes don't lose results ──
static std::ofstream g_reportStream;
static int g_reportPassed = 0;
static int g_reportFailed = 0;
static int g_reportTotal = 0;

static void openProgressiveReport(const std::string &dir, int totalFiles)
{
    fs::path rp = fs::path(dir) / "test_results.txt";
    g_reportStream.open(rp);
    g_reportTotal = totalFiles;

    if (!g_reportStream.is_open())
        return;

    g_reportStream << "Quantum Language — Test Results (in progress)\n";
    g_reportStream << "Generated : ";
    {
        std::time_t t = std::time(nullptr);
        char buf[64];
        struct tm tm_i;
#ifdef _WIN32
        localtime_s(&tm_i, &t);
#else
        localtime_r(&t, &tm_i);
#endif
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_i);
        g_reportStream << buf;
    }
    g_reportStream << "\nDirectory : " << fs::absolute(fs::path(dir)).string() << "\n";
    g_reportStream << "Total     : " << totalFiles << "   (running...)\n";
    g_reportStream << std::string(72, '=') << "\n\n";
    g_reportStream.flush();
}

static void appendResultToReport(const TestResult &r)
{
    if (!g_reportStream.is_open())
        return;

    if (r.passed)
    {
        ++g_reportPassed;
        g_reportStream << "[PASS] " << r.path << "\n\n";
        g_reportStream.flush();
        return;
    }

    ++g_reportFailed;

    g_reportStream << "[FAIL] " << r.path << "\n";
    g_reportStream << std::string(72, '-') << "\n";
    g_reportStream << "Error  : " << r.error << "\n";
    if (r.line > 0)
    {
        g_reportStream << "Line   : " << r.line;
        if (r.col > 0)
            g_reportStream << "   Col : " << r.col;
        g_reportStream << "\n";
    }
    if (r.crashed)
        g_reportStream << "Note   : Process-level crash — SEH exception caught\n";

    if (!r.output.empty())
    {
        g_reportStream << "\n--- Program Output ---\n";
        std::istringstream os(r.output);
        std::string ln;
        while (std::getline(os, ln))
            g_reportStream << "  " << ln << "\n";
        g_reportStream << "--- End Output ---\n";
    }

    // Full numbered source with error-line marker
    g_reportStream << "\n--- Source Code (" << r.path << ") ---\n";
    {
        std::istringstream src(r.source);
        std::string ln;
        int lineNo = 1;
        while (std::getline(src, ln))
        {
            if (r.line > 0 && lineNo == r.line)
                g_reportStream << ">>> ";
            else
                g_reportStream << "    ";
            g_reportStream << std::setw(4) << lineNo++ << " | " << ln << "\n";
        }
    }
    g_reportStream << "--- End Source ---\n\n";
    g_reportStream << std::string(72, '=') << "\n\n";
    g_reportStream.flush();
}

static void finalizeReport(const std::string &dir)
{
    if (!g_reportStream.is_open())
        return;

    int total = g_reportPassed + g_reportFailed;
    g_reportStream << std::string(72, '=') << "\n";
    if (g_reportFailed == 0)
        g_reportStream << "Result: ALL PASSED (" << total << "/" << g_reportTotal << ")\n";
    else
        g_reportStream << "Result: FAILED " << g_reportFailed
                       << "/" << g_reportTotal << " files\n";
    g_reportStream << "Passed : " << g_reportPassed
                   << "   Failed : " << g_reportFailed
                   << "   Total : " << g_reportTotal << "\n";
    g_reportStream.close();

    fs::path rp = fs::path(dir) / "test_results.txt";
    std::cout << Colors::CYAN << "  Report  : " << Colors::RESET
              << fs::absolute(rp).string() << "\n";
}

static int runTestExamples(const std::string &dir)
{
    fs::path d(dir);
    if (!fs::exists(d) || !fs::is_directory(d))
    {
        std::cerr << Colors::RED << "[Error] " << Colors::RESET
                  << "Not found: " << dir << "\n";
        return 1;
    }

    redirectStdinToNull();
    g_testMode = true;

    std::vector<fs::path> files;
    collectTestFiles(d, files);
    if (files.empty())
    {
        std::cout << "No testable files found (.sa .js .py .rb .c .cpp).\n";
        return 0;
    }
    std::sort(files.begin(), files.end());

    const int total = (int)files.size();

    std::cout << Colors::CYAN << Colors::BOLD
              << "\n═══════════════ Quantum Test Runner ═══════════════\n"
              << Colors::RESET
              << "  Directory : " << fs::absolute(d).string() << "\n"
              << "  Files     : " << total << "\n\n";
    std::cout.flush();

    // Open the report file immediately — results are streamed in as they finish
    // so even if the process crashes partway through, we have a partial report.
    openProgressiveReport(dir, total);

    int passed = 0;

    for (int i = 0; i < total; ++i)
    {
        const fs::path &fp = files[i];
        std::string ps = fp.string();
        std::string disp = ps;
        try
        {
            disp = fs::relative(fp).string();
        }
        catch (...)
        {
        }

        // Progress counter so the user can see we haven't hung
        std::cout << Colors::CYAN << "  [" << std::setw(3) << (i + 1)
                  << "/" << total << "] " << Colors::RESET << disp << " ... ";
        std::cout.flush();

        TestResult tr = testFile(ps);
        tr.path = disp;

        if (tr.passed)
        {
            std::cout << Colors::GREEN << "PASS\n"
                      << Colors::RESET;
            ++passed;
        }
        else
        {
            std::cout << Colors::RED << "FAIL\n"
                      << Colors::RESET;
            if (tr.line > 0)
            {
                std::cout << "            Line " << tr.line;
                if (tr.col > 0)
                    std::cout << ", Col " << tr.col;
                std::cout << "\n";
            }
            std::cout << "            " << Colors::RED << tr.error
                      << Colors::RESET << "\n";
            if (tr.crashed)
                std::cout << "            "
                          << Colors::YELLOW << "(process-level crash caught — continuing)\n"
                          << Colors::RESET;
        }
        std::cout.flush();

        appendResultToReport(tr);
    }

    int failed = total - passed;

    // ── Console summary ───────────────────────────────────────────────────────
    std::cout << "\n"
              << std::string(51, '=') << "\n";
    if (failed == 0)
        std::cout << Colors::GREEN << "  ✓ All " << total << " files passed!\n"
                  << Colors::RESET;
    else
        std::cout << Colors::GREEN << "  ✓ " << passed << " passed  "
                  << Colors::RED << "✗ " << failed << " failed"
                  << "  (total " << total << ")\n"
                  << Colors::RESET;

    finalizeReport(dir);

    return failed > 0 ? 1 : 0;
}

// ─── REPL ─────────────────────────────────────────────────────────────────────

static void runREPL(bool debug = false)
{
    printBanner();
    std::cout << Colors::GREEN << "  REPL — type 'exit' to quit\n"
              << Colors::RESET << "\n";
    VM vm;
    int n = 1;
    std::string line;
    while (true)
    {
        std::cout << Colors::CYAN << "quantum[" << n++ << "]> " << Colors::RESET;
        if (!std::getline(std::cin, line))
            break;
        if (line == "exit" || line == "quit")
            break;
        if (line.empty())
            continue;
        try
        {
            vm.run(compileSource(line, "<repl>", debug));
        }
        catch (const ParseError &e)
        {
            std::cerr << Colors::RED << "[ParseError] " << Colors::RESET << e.what() << "\n";
        }
        catch (const QuantumError &e)
        {
            std::cerr << Colors::RED << "[" << e.kind << "] " << Colors::RESET << e.what() << "\n";
        }
        catch (const std::exception &e)
        {
            std::cerr << Colors::RED << "[Error] " << Colors::RESET << e.what() << "\n";
        }
    }
    std::cout << Colors::YELLOW << "\n  Goodbye! 👋\n"
              << Colors::RESET;
}

// ─── printHelp ────────────────────────────────────────────────────────────────

static void printHelp(const char *prog)
{
    std::cout << Colors::BOLD << "Usage:\n"
              << Colors::RESET
              << "  " << prog << " <file>             Compile → <file>.exe then run it\n"
              << "  " << prog << " --run <file>       Interpret directly (no .exe)\n"
              << "  " << prog << " --check <file>     Parse + type-check only\n"
              << "  " << prog << " --debug <file>     Dump bytecode then run\n"
              << "  " << prog << " --dis   <file>     Dump bytecode only\n"
              << "  " << prog << " --test  [dir]      Batch-test all supported source files\n"
              << "  qrun <file>                 Interpret directly (no .exe)\n\n"
              << "  Supported files: .sa .js .py .rb .c .cpp — all run natively on the\n"
              << "  Quantum VM (multi-syntax subset; node/python/gcc NOT required)\n\n"
              << "  quantum hello.sa            → hello.exe created and run\n"
              << "  quantum prog.c              → prog.exe created and run (no gcc)\n"
              << "  qrun    hello.py            → interpreted directly\n"
              << "  qrun    hello.rb            → Ruby-style subset interpreted directly\n";
}

// ─── findStubPath ─────────────────────────────────────────────────────────────
// Searches for quantum_stub.exe next to quantum.exe (or in build/ subdirs).
// All messages go to stdout so the user always sees them.

static std::string findStubPath(const std::string &quantumExePath)
{
    fs::path base = fs::path(quantumExePath).parent_path();

    std::vector<fs::path> candidates = {
        base / (std::string("quantum_stub") + EXE_EXT),
        base / "build" / (std::string("quantum_stub") + EXE_EXT),
        base / "build" / "Release" / (std::string("quantum_stub") + EXE_EXT),
        base / "build" / "Debug" / (std::string("quantum_stub") + EXE_EXT),
    };

    for (auto &p : candidates)
    {
        if (fs::exists(p))
            return p.string();
    }

    // Nothing found — tell the user exactly where we looked
    std::cout << Colors::RED << "[Error] " << Colors::RESET
              << "quantum_stub" << EXE_EXT << " not found. Searched:\n";
    for (auto &p : candidates)
        std::cout << "  " << p.string() << "\n";
#ifdef _WIN32
    std::cout << "Run build.bat to rebuild all three binaries.\n";
#else
    std::cout << "Rebuild with: cmake --build .\n";
#endif
    return "";
}

// ─── bundleAndRun ─────────────────────────────────────────────────────────────
// Compiles a source file (.sa/.js/.py/.rb/.c/.cpp) → bytecode, appends it to a
// copy of quantum_stub.exe, writes <name>.exe next to the source file,
// then launches it and waits.

static int bundleAndRun(const std::string &path, const std::string &exePath)
{
    // 1. Read source
    std::ifstream src(path);
    if (!src.is_open())
    {
        std::cout << Colors::RED << "[Error] " << Colors::RESET
                  << "Cannot open: " << path << "\n";
        std::cout.flush();
        return 1;
    }
    std::ostringstream ss;
    ss << src.rdbuf();

    // 2. Compile
    std::shared_ptr<Chunk> chunk;
    try
    {
        chunk = compileSource(applyDialect(ss.str(), path), path, false);
    }
    catch (const ParseError &e)
    {
        std::cout << Colors::RED << Colors::BOLD << "\n  X ParseError" << Colors::RESET
                  << " in " << path << " at line " << e.line << ":" << e.col
                  << "\n    " << e.what() << "\n\n";
        std::cout.flush();
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cout << Colors::RED << "[Compile Error] " << Colors::RESET << e.what() << "\n";
        std::cout.flush();
        return 1;
    }

    // 3. Serialize bytecode
    auto payload = Serializer::serialize(chunk);
    uint32_t payloadSize = (uint32_t)payload.size();

    // 4. Find quantum_stub.exe (the template runtime)
    std::string stub = findStubPath(exePath);
    if (stub.empty())
    {
        std::cout.flush();
        return 1;
    }

    // 5. Determine output path: hello.sa → hello.exe
    fs::path srcPath(path);
    std::string outName;
    if (srcPath.parent_path().empty())
        outName = (fs::current_path() / srcPath.stem()).string() + EXE_EXT;
    else
        outName = (srcPath.parent_path() / srcPath.stem()).string() + EXE_EXT;

    // Safety: never overwrite quantum.exe, qrun.exe, or quantum_stub.exe
    {
        std::string stemLower = fs::path(outName).stem().string();
        std::transform(stemLower.begin(), stemLower.end(), stemLower.begin(), ::tolower);
        if (stemLower == "quantum" || stemLower == "qrun" || stemLower == "quantum_stub")
            outName = (fs::path(outName).parent_path() /
                       (fs::path(outName).stem().string() + "_out"))
                          .string() +
                      EXE_EXT;
    }

    // 6. Copy stub → output exe
    std::error_code copyErr;
    fs::copy_file(stub, outName, fs::copy_options::overwrite_existing, copyErr);
    if (copyErr)
    {
        std::cout << Colors::RED << "[Error] " << Colors::RESET
                  << "Cannot create " << outName << ": " << copyErr.message() << "\n";
        std::cout.flush();
        return 1;
    }

    // 7. Append payload: [bytes] [size: uint32 LE] [magic: "QNTM_VM!" 8 bytes]
    {
        std::ofstream out(outName, std::ios::binary | std::ios::app);
        if (!out)
        {
            std::cout << Colors::RED << "[Error] " << Colors::RESET
                      << "Cannot open " << outName << " for appending\n";
            std::cout.flush();
            return 1;
        }
        out.write(reinterpret_cast<const char *>(payload.data()), payloadSize);
        out.write(reinterpret_cast<const char *>(&payloadSize), 4);
        out.write("QNTM_VM!", 8);
        out.flush();
        if (!out)
        {
            std::cout << Colors::RED << "[Error] " << Colors::RESET
                      << "Write failed on " << outName << "\n";
            std::cout.flush();
            return 1;
        }
    }

    std::cout << Colors::GREEN << "[Compiled] " << Colors::RESET
              << path << "  ->  " << outName << "  (" << payloadSize << " bytes)\n";
    std::cout.flush();

    // 8. Launch the produced .exe and wait for it to finish
    std::cout << Colors::CYAN << "[Running]  " << Colors::RESET << outName << "\n\n";
    std::cout.flush();

#ifdef _WIN32
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + outName + "\"";

    if (!CreateProcessA(NULL, const_cast<char *>(cmd.c_str()),
                        NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
    {
        std::cout << Colors::RED << "[Error] " << Colors::RESET
                  << "Could not launch " << outName
                  << "  (Windows error " << GetLastError() << ")\n";
        std::cout.flush();
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exitCode;
#else
    std::string cmd = shellExec(outName);
    int rc = std::system(cmd.c_str());
    if (rc == -1)
    {
        std::cout << Colors::RED << "[Error] " << Colors::RESET
                  << "Could not launch " << outName << "\n";
        std::cout.flush();
        return 1;
    }
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    return 1;
#endif
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::string exePath = getExecutablePath();

    // ══════════════════════════════════════════════════════════════
    //  STANDALONE BUNDLED EXE  (hello.exe etc.) — quantum_stub mode
    // ══════════════════════════════════════════════════════════════
#if !defined(QRUN_MODE) && !defined(QUANTUM_MODE_COMPILER)
    {
        auto embedded = loadEmbeddedBytecode(exePath);
        if (embedded)
        {
            try
            {
                VM vm;
                vm.run(embedded);
                return 0;
            }
            catch (const QuantumError &e)
            {
                std::cerr << Colors::RED << "[" << e.kind << "] " << Colors::RESET << e.what() << "\n";
                return 1;
            }
            catch (const std::exception &e)
            {
                std::cerr << Colors::RED << "[Fatal] " << Colors::RESET << e.what() << "\n";
                return 1;
            }
        }
        // No embedded bytecode — user ran quantum_stub.exe directly
        std::cout << Colors::YELLOW
                  << "[quantum_stub] This is the Quantum standalone runtime.\n"
                  << "  Run:  quantum hello.sa   to compile hello.sa into hello.exe\n"
                  << Colors::RESET;
        return 1;
    }
#endif

    // ══════════════════════════════════════════════════════════════
    //  QRUN MODE  (qrun.exe) — always interpret, never bundle
    // ══════════════════════════════════════════════════════════════
#ifdef QRUN_MODE
    if (argc == 1)
    {
        runREPL();
        return 0;
    }
    std::string a1 = argv[1];
    if (a1 == "--help" || a1 == "-h")
    {
        printBanner();
        printHelp(argv[0]);
        return 0;
    }
    if (a1 == "--version" || a1 == "-v")
    {
        std::cout << "Quantum Language v2.0.0\n";
        return 0;
    }
    if (a1 == "--check" && argc >= 3)
        return checkFile(argv[2]);
    if (a1 == "--debug" && argc >= 3)
    {
        runFile(argv[2], true);
        return 0;
    }
    if (a1 == "--dis" && argc >= 3)
    {
        std::ifstream f(argv[2]);
        std::ostringstream ss;
        ss << f.rdbuf();
        disassembleChunk(*compileSource(applyDialect(ss.str(), argv[2]), argv[2], false), std::cout);
        return 0;
    }
    if (a1 == "--test")
        return runTestExamples(argc >= 3 ? argv[2] : "examples");
    // Any supported source file runs natively on the Quantum VM —
    // .js/.py/.rb/.c/.cpp go through the same multi-syntax front-end as .sa,
    // so no node/python/gcc/g++ is required.
    if (hasSupportedExt(a1))
    {
        runFile(a1);
        return 0;
    }
    else
    {
        std::cerr << "[Error] Unsupported file type: " << a1 << "\n";
        std::cerr << "Supported: .sa, .js, .py, .rb, .c, .cpp (run natively on the Quantum VM)\n";
        return 1;
    }
#endif

    // ══════════════════════════════════════════════════════════════
    //  QUANTUM COMPILER MODE  (quantum.exe)
    // ══════════════════════════════════════════════════════════════
    if (argc == 1)
    {
        runREPL();
        return 0;
    }

    std::string arg = argv[1];

    if (arg == "--help" || arg == "-h")
    {
        printBanner();
        printHelp(argv[0]);
        return 0;
    }
    if (arg == "--aura")
    {
        printBanner();
        printAura();
        return 0;
    }
    if (arg == "--version" || arg == "-v")
    {
        std::cout << "Quantum Language v2.0.0\nRuntime: Bytecode VM\nBy Muhammad Saad Amin\n";
        return 0;
    }
    if (arg == "--check" && argc >= 3)
        return checkFile(argv[2]);
    if (arg == "--test")
        return runTestExamples(argc >= 3 ? argv[2] : "examples");
    if (arg == "--debug" && argc >= 3)
    {
        runFile(argv[2], true);
        return 0;
    }
    if (arg == "--run" && argc >= 3)
    {
        runFile(argv[2]);
        return 0;
    }
    if (arg == "--dis" && argc >= 3)
    {
        std::ifstream f(argv[2]);
        if (!f.is_open())
        {
            std::cerr << Colors::RED << "[Error] Cannot open: " << argv[2] << "\n";
            return 1;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        try
        {
            disassembleChunk(*compileSource(applyDialect(ss.str(), argv[2]), argv[2], false), std::cout);
        }
        catch (const std::exception &e)
        {
            std::cerr << Colors::RED << "[Error] " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // Default: compile any supported source file → <file>.exe → run.
    // .js/.py/.rb/.c/.cpp compile through the same multi-syntax front-end as .sa,
    // so no node/python/gcc/g++ is required and the produced .exe is standalone.
    if (hasSupportedExt(arg))
        return bundleAndRun(arg, exePath);

    std::cerr << "[Error] Unsupported file type: " << arg << "\n";
    std::cerr << "Supported: .sa, .js, .py, .rb, .c, .cpp (run natively on the Quantum VM)\n";
    return 1;
}
