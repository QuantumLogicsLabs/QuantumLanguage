// C++ multi-file / structure front-end — source-to-source passes that turn a
// conventional header + implementation C++ project into the single-file shape
// the parser understands:
//
//   1. #include "local.h"   → the header's text is spliced in (once), and the
//                              header's implementation file next to it
//                              (local.sa / .cpp / .cc / .cxx / .c) is appended,
//                              so `quantum main.sa` builds the whole program.
//   2. namespace N { ... }  → its members become globals named N__member
//                              (`namespace a::b` too); N::member,
//                              `using namespace N;` and `using N::member;`
//                              are rewritten to match.
//   3. T Cls::m(...) { }    → out-of-class member definitions (a class
//                              template's `T Box<T>::get()` too) move into
//                              the class body, replacing the in-class
//                              prototype, whose default arguments and
//                              `static` they inherit; `int Cls::n = 0;` gives
//                              the static member its value.
//   4. operator<<(std::ostream&, const T&) and the other free / friend binary
//      operators become functions __op__<stem>__<T> the VM dispatches to.
//   5. prototypes of functions the file defines are dropped (each would
//      compile to an empty function), handing their default arguments to
//      the definition.
//
// Every pass is a no-op unless its construct is present, so sources that
// don't use these features (and the other dialects sharing .sa) are returned
// unchanged. QUANTUM_DEBUG_CPP=1 prints the result (see Dialect.cpp).

#include "Dialect.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

// ─── Includes ─────────────────────────────────────────────────────────────

std::string readTextFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
    return s;
}

std::string pathKey(const fs::path &p)
{
    std::error_code ec;
    std::string k = fs::weakly_canonical(p, ec).string();
    if (ec)
        k = fs::absolute(p).string();
    std::transform(k.begin(), k.end(), k.begin(), ::tolower);
    return k;
}

bool isHeaderExt(const fs::path &p)
{
    std::string ext = fileExtLower(p.string());
    return ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx" || ext == ".h++";
}

// A folder that holds a whole project: include lookups stop climbing here.
bool isProjectRoot(const fs::path &dir)
{
    std::error_code ec;
    for (const char *marker : {"CMakeLists.txt", "Makefile", "makefile", "package.json", "package.qon", ".git"})
        if (fs::exists(dir / marker, ec))
            return true;
    return false;
}

// `#include "x"` is looked up next to the including file, then (for the
// usual include/ + src/ layout, `#include "minidb/value.hpp"` from tests/)
// in each folder above it and that folder's include/, up to the project root.
fs::path findInclude(const std::string &name, const fs::path &from)
{
    std::error_code ec;
    const fs::path rel = fs::u8path(name);
    fs::path dir = fs::absolute(from, ec).parent_path();
    if (fs::is_regular_file(dir / rel, ec))
        return dir / rel;
    for (int up = 0; up < 8 && !dir.empty(); ++up)
    {
        if (up > 0 && fs::is_regular_file(dir / rel, ec))
            return dir / rel;
        if (fs::is_regular_file(dir / "include" / rel, ec))
            return dir / "include" / rel;
        fs::path parent = dir.parent_path();
        if (isProjectRoot(dir) || parent == dir)
            break;
        dir = parent;
    }
    return {};
}

// The implementation file of a header: next to it (local.h → local.cpp), or
// for include/<lib>/name.hpp in the project's src/ (src/name.cpp,
// src/<lib>/name.cpp).
fs::path findImplementation(const fs::path &header)
{
    static const char *const exts[] = {".sa", ".cpp", ".cc", ".cxx", ".c"};
    std::error_code ec;
    for (const char *ext : exts)
    {
        fs::path impl = header;
        impl.replace_extension(ext);
        if (fs::is_regular_file(impl, ec))
            return impl;
    }
    // Climb to the project root, remembering the path below an include/ folder.
    const fs::path abs = fs::absolute(header, ec);
    fs::path dir = abs.parent_path(), root, belowInclude;
    for (int up = 0; up < 8 && !dir.empty(); ++up)
    {
        if (belowInclude.empty() && dir.filename() == "include")
            belowInclude = abs.lexically_relative(dir);
        fs::path parent = dir.parent_path();
        if (isProjectRoot(dir) || parent == dir)
        {
            root = dir;
            break;
        }
        dir = parent;
    }
    if (root.empty() || belowInclude.empty())
        return {};
    std::vector<fs::path> stems = {belowInclude, belowInclude.filename()};
    for (const char *base : {"src", "source", "lib", ""})
        for (fs::path stem : stems)
            for (const char *ext : exts)
            {
                fs::path impl = (*base ? root / base : root) / stem.replace_extension(ext);
                if (fs::is_regular_file(impl, ec))
                    return impl;
            }
    return {};
}

struct IncludeState
{
    std::set<std::string> seen;  // files already spliced in (include-once)
    std::vector<fs::path> impls; // implementation files to append, in order
    std::set<std::string> queued;
};

std::string expandIncludes(const std::string &src, const fs::path &file, IncludeState &st)
{
    static const std::regex includeRe(R"(^\s*#\s*include\s*"([^"]+)\"\s*(//.*|/\*.*\*/\s*)?\r?$)");
    std::istringstream in(src);
    std::string out, line;
    while (std::getline(in, line))
    {
        std::smatch m;
        if (std::regex_match(line, m, includeRe))
        {
            fs::path target = findInclude(m[1].str(), file);
            std::error_code ec;
            if (!target.empty())
            {
                std::string key = pathKey(target);
                if (!st.seen.count(key))
                {
                    st.seen.insert(key);
                    out += expandIncludes(readTextFile(target), target, st);
                    out += "\n";
                    if (isHeaderExt(target))
                    {
                        fs::path impl = findImplementation(target);
                        if (!impl.empty())
                        {
                            std::string ik = pathKey(impl);
                            if (!st.queued.count(ik))
                            {
                                st.queued.insert(ik);
                                st.impls.push_back(impl);
                            }
                        }
                    }
                }
                else
                    out += "\n";
                continue;
            }
        }
        out += line;
        out += "\n";
    }
    return out;
}

// ─── Token scan for the structural passes ─────────────────────────────────

struct Tok
{
    enum Kind
    {
        Ident,
        Punct,
        Literal
    } kind;
    std::string text;
    size_t begin, end; // byte range in the source
};

bool identStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool identChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// Identifiers, literals and punctuation ("::" and "->" as single tokens).
// Comments and preprocessor lines are skipped.
std::vector<Tok> scan(const std::string &s)
{
    std::vector<Tok> out;
    size_t i = 0, n = s.size();
    bool lineStart = true;
    while (i < n)
    {
        char c = s[i];
        if (c == '\n')
        {
            lineStart = true;
            ++i;
            continue;
        }
        if (std::isspace((unsigned char)c))
        {
            ++i;
            continue;
        }
        if (c == '#' && lineStart)
        {
            while (i < n && s[i] != '\n')
            {
                if (s[i] == '\\' && i + 1 < n)
                    ++i;
                ++i;
            }
            continue;
        }
        lineStart = false;
        if (c == '/' && i + 1 < n && s[i + 1] == '/')
        {
            while (i < n && s[i] != '\n')
                ++i;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*')
        {
            size_t e = s.find("*/", i + 2);
            i = e == std::string::npos ? n : e + 2;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            size_t b = i++;
            while (i < n && s[i] != c && s[i] != '\n')
            {
                if (s[i] == '\\')
                    ++i;
                ++i;
            }
            if (i < n && s[i] == c)
                ++i;
            out.push_back({Tok::Literal, s.substr(b, i - b), b, i});
            continue;
        }
        if (identStart(c))
        {
            size_t b = i;
            while (i < n && identChar(s[i]))
                ++i;
            std::string id = s.substr(b, i - b);
            // Raw string literal: R"delim( ... )delim"
            if (i < n && s[i] == '"' && !id.empty() && id.back() == 'R')
            {
                size_t open = s.find('(', i);
                if (open != std::string::npos)
                {
                    std::string close = ")" + s.substr(i + 1, open - i - 1) + "\"";
                    size_t e = s.find(close, open);
                    i = e == std::string::npos ? n : e + close.size();
                    out.push_back({Tok::Literal, s.substr(b, i - b), b, i});
                    continue;
                }
            }
            out.push_back({Tok::Ident, id, b, i});
            continue;
        }
        if (std::isdigit((unsigned char)c))
        {
            size_t b = i;
            while (i < n && (identChar(s[i]) || s[i] == '.' || s[i] == '\'' ||
                             ((s[i] == '+' || s[i] == '-') &&
                              (s[i - 1] == 'e' || s[i - 1] == 'E' || s[i - 1] == 'p' || s[i - 1] == 'P'))))
                ++i;
            out.push_back({Tok::Literal, s.substr(b, i - b), b, i});
            continue;
        }
        if (c == ':' && i + 1 < n && s[i + 1] == ':')
        {
            out.push_back({Tok::Punct, "::", i, i + 2});
            i += 2;
            continue;
        }
        if (c == '-' && i + 1 < n && s[i + 1] == '>')
        {
            out.push_back({Tok::Punct, "->", i, i + 2});
            i += 2;
            continue;
        }
        out.push_back({Tok::Punct, std::string(1, c), i, i + 1});
        ++i;
    }
    return out;
}

// ─── Names that are Quantum keywords but plain identifiers in C/C++ ───────
//
// `std::ifstream in(path)`, `enum class Kind { None }`, `const Node& self`:
// the shared lexer would read these names as keywords. A word is renamed
// (everywhere, `obj.in` included) only when the C/C++ code declares it — a
// variable, parameter, member or enumerator — so Quantum syntax that a .cpp
// file may also use (`let a = 10`, `for x in xs`) keeps its meaning.

void renameKeywordClashes(std::string &s)
{
    static const std::set<std::string> clashes = {
        "let", "fn", "def", "extends", "self", "elif", "in", "of", "raise",
        "except", "finally", "as", "with", "from", "is", "nil", "null",
        "undefined", "None", "True", "False"};
    // Words before a name that make it an expression, not a declaration.
    static const std::set<std::string> notTypes = {
        "return", "case", "else", "throw", "new", "delete", "sizeof", "typeid", "goto",
        "using", "namespace", "operator", "co_return", "co_yield", "co_await", "do"};
    if (s.find_first_of("abcdefghijklmnopqrstuvwxyzNTF") == std::string::npos)
        return;
    std::vector<Tok> toks = scan(s);
    auto is = [&](size_t i, const char *text)
    { return i < toks.size() && toks[i].text == text; };

    std::set<std::string> declared;
    for (size_t i = 0; i < toks.size(); ++i)
    {
        // enum [class|struct] [Name] [: type] { a, b = 1, ... }
        if (toks[i].kind == Tok::Ident && toks[i].text == "enum")
        {
            size_t j = i + 1;
            while (j < toks.size() && !is(j, "{") && !is(j, ";"))
                ++j;
            if (!is(j, "{"))
                continue;
            for (++j; j < toks.size() && !is(j, "}"); ++j)
                if (toks[j].kind == Tok::Ident && clashes.count(toks[j].text) && (is(j - 1, "{") || is(j - 1, ",")))
                    declared.insert(toks[j].text);
            continue;
        }
        if (toks[i].kind != Tok::Ident || !clashes.count(toks[i].text) || i == 0 || i + 1 >= toks.size())
            continue;
        // Type before it (`ifstream in`, `Node* self`, `vector<int> of`), then
        // ( ) ; = , [ { after it.
        const Tok &prev = toks[i - 1];
        bool typeBefore = (prev.kind == Tok::Ident && !notTypes.count(prev.text) && !clashes.count(prev.text)) ||
                          prev.text == "*" || prev.text == "&" || prev.text == ">";
        const std::string &next = toks[i + 1].text;
        bool declEnd = next == "(" || next == ")" || next == ";" || next == "=" || next == "," || next == "[" ||
                       next == "{";
        bool pythonFor = i >= 2 && toks[i - 2].text == "for"; // for x in (...)
        if (typeBefore && declEnd && !pythonFor)
            declared.insert(toks[i].text);
    }
    if (declared.empty())
        return;

    std::string out;
    size_t last = 0;
    for (const Tok &t : toks)
    {
        if (t.kind != Tok::Ident || !declared.count(t.text))
            continue;
        out.append(s, last, t.begin - last);
        out += "__kw_" + t.text;
        last = t.end;
    }
    out.append(s, last, std::string::npos);
    s = std::move(out);
}

struct Scanned
{
    const std::string &src;
    std::vector<Tok> t;
    std::vector<long> match; // index of the matching bracket, or -1
    std::vector<int> depth;  // brace depth before each token

    explicit Scanned(const std::string &s) : src(s), t(scan(s))
    {
        match.assign(t.size(), -1);
        depth.assign(t.size(), 0);
        std::vector<size_t> stack;
        int d = 0;
        for (size_t i = 0; i < t.size(); ++i)
        {
            const std::string &x = t[i].text;
            if (t[i].kind == Tok::Punct && (x == "{" || x == "(" || x == "["))
            {
                depth[i] = d;
                if (x == "{")
                    ++d;
                stack.push_back(i);
                continue;
            }
            if (t[i].kind == Tok::Punct && (x == "}" || x == ")" || x == "]"))
            {
                if (x == "}" && d > 0)
                    --d;
                depth[i] = d;
                char want = x == "}" ? '{' : x == ")" ? '(' : '[';
                // Unbalanced input: drop unmatched openers of another kind.
                while (!stack.empty() && t[stack.back()].text[0] != want)
                    stack.pop_back();
                if (!stack.empty())
                {
                    match[stack.back()] = (long)i;
                    match[i] = (long)stack.back();
                    stack.pop_back();
                }
                continue;
            }
            depth[i] = d;
        }
    }

    size_t size() const { return t.size(); }
    bool is(size_t i, const char *x) const { return i < t.size() && t[i].text == x && t[i].kind != Tok::Literal; }
    bool ident(size_t i) const { return i < t.size() && t[i].kind == Tok::Ident; }
    // Index just past the bracket group opening at i (or i + 1 if unmatched).
    size_t past(size_t i) const { return match[i] > (long)i ? (size_t)match[i] + 1 : i + 1; }
    std::string text(size_t first, size_t last) const // tokens [first, last] inclusive
    {
        return src.substr(t[first].begin, t[last].end - t[first].begin);
    }
};

struct Edit
{
    size_t begin, end;
    std::string text;
};

std::string applyEdits(const std::string &src, std::vector<Edit> edits)
{
    std::stable_sort(edits.begin(), edits.end(),
                     [](const Edit &a, const Edit &b)
                     { return a.begin > b.begin; });
    std::string out = src;
    size_t limit = std::string::npos;
    for (auto &e : edits)
    {
        if (e.end > limit) // overlaps an edit already applied — skip it
            continue;
        out.replace(e.begin, e.end - e.begin, e.text);
        limit = e.begin;
    }
    return out;
}

bool isKeyword(const std::string &w)
{
    static const std::set<std::string> kw = {
        "return", "using", "typedef", "operator", "template", "typename", "class", "struct",
        "union", "enum", "namespace", "public", "private", "protected", "virtual", "static",
        "inline", "const", "constexpr", "extern", "friend", "explicit", "if", "while", "for",
        "switch", "do", "else", "new", "delete", "sizeof", "throw", "case", "default"};
    return kw.count(w) > 0;
}

// Statements at one brace level, as token ranges [first, last]. A statement
// ends at ';' or at the '}' of a function/namespace body; a class/enum body or
// a brace initialiser keeps the statement going until its ';'. "public:" and
// friends are statements of their own.
std::vector<std::pair<size_t, size_t>> splitStatements(const Scanned &s, size_t b, size_t e)
{
    std::vector<std::pair<size_t, size_t>> out;
    size_t j = b;
    while (j < e)
    {
        size_t start = j;
        if ((s.is(j, "public") || s.is(j, "private") || s.is(j, "protected")) && s.is(j + 1, ":"))
        {
            out.push_back({j, j + 1});
            j += 2;
            continue;
        }
        bool typeBody = false; // statement declares a class/struct/union/enum
        bool sawAssign = false;
        bool sawParens = false; // a `{` after a parameter list opens a function body
        size_t last = start;
        while (j < e)
        {
            const Tok &tk = s.t[j];
            if (tk.kind == Tok::Ident && (tk.text == "class" || tk.text == "struct" ||
                                          tk.text == "union" || tk.text == "enum"))
                typeBody = true;
            // An initialiser's `=` — not one of `==`, `!=`, `<=`, `>=`, nor
            // the name `operator=` (the scanner splits those into single
            // characters).
            if (s.is(j, "=") && !s.is(j + 1, "=") && !s.is(j - 1, "=") && !s.is(j - 1, "!") &&
                !s.is(j - 1, "<") && !s.is(j - 1, ">") && !s.is(j - 1, "operator"))
                sawAssign = true;
            if (s.is(j, "(") || s.is(j, "["))
            {
                sawParens = sawParens || s.is(j, "(");
                j = s.past(j);
                continue;
            }
            if (s.is(j, "{"))
            {
                size_t close = s.past(j) - 1;
                j = close + 1;
                if ((typeBody || sawAssign) && !sawParens)
                    continue;
                last = close;
                if (s.is(j, ";")) // stray ';' after a function body
                    last = j++;
                break;
            }
            if (s.is(j, ";"))
            {
                last = j++;
                break;
            }
            last = j++;
        }
        if (j > e)
            last = std::min(last, e - 1);
        out.push_back({start, last});
    }
    return out;
}

// Parameter token ranges inside the parens at [open, close].
std::vector<std::pair<size_t, size_t>> splitParams(const Scanned &s, size_t open, size_t close)
{
    std::vector<std::pair<size_t, size_t>> out;
    size_t j = open + 1, start = j;
    int angle = 0;
    while (j < close)
    {
        if (s.is(j, "(") || s.is(j, "[") || s.is(j, "{"))
        {
            j = s.past(j);
            continue;
        }
        if (s.is(j, "<"))
            ++angle;
        else if (s.is(j, ">") && angle > 0)
            --angle;
        else if (s.is(j, ",") && angle == 0)
        {
            if (j > start)
                out.push_back({start, j - 1});
            start = j + 1;
        }
        ++j;
    }
    if (close > start)
        out.push_back({start, close - 1});
    // "(void)" declares no parameters
    if (out.size() == 1 && out[0].first == out[0].second && s.is(out[0].first, "void"))
        out.clear();
    return out;
}

// Index of a top-level '=' inside a parameter range, or 0 when there is none.
size_t paramDefaultEq(const Scanned &s, std::pair<size_t, size_t> p)
{
    for (size_t j = p.first; j <= p.second; ++j)
    {
        if (s.is(j, "(") || s.is(j, "[") || s.is(j, "{"))
        {
            j = s.past(j) - 1;
            continue;
        }
        if (s.is(j, "="))
            return j;
    }
    return 0;
}

// Edits that give a definition's parameters the defaults its prototype has.
void inheritDefaults(const Scanned &s, size_t protoOpen, size_t defOpen, std::vector<Edit> &edits)
{
    auto pp = splitParams(s, protoOpen, (size_t)s.match[protoOpen]);
    auto dp = splitParams(s, defOpen, (size_t)s.match[defOpen]);
    for (size_t i = 0; i < pp.size() && i < dp.size(); ++i)
    {
        size_t eq = paramDefaultEq(s, pp[i]);
        if (!eq || paramDefaultEq(s, dp[i]))
            continue;
        edits.push_back({s.t[dp[i].second].end, s.t[dp[i].second].end,
                         " = " + s.text(eq + 1, pp[i].second)});
    }
}

// Index of the declarator's '(' in a function statement — the first '('
// directly after a name at the statement's own level — or 0.
size_t functionParen(const Scanned &s, std::pair<size_t, size_t> st)
{
    for (size_t j = st.first; j <= st.second; ++j)
    {
        if (s.is(j, "=") || s.is(j, "{"))
            return 0;
        if (s.is(j, "("))
            return j > st.first && (s.ident(j - 1) || s.is(j - 1, ">")) ? j : 0;
        if (s.is(j, "["))
            j = s.past(j) - 1;
    }
    return 0;
}

// The '{' opening the body of a function whose parameter list opens at
// `paramsOpen` — past cv/ref qualifiers, noexcept(...), a trailing return type
// and a constructor initialiser list — or 0 for a body-less declaration.
size_t bodyAfterParams(const Scanned &s, size_t paramsOpen)
{
    size_t k = s.past(paramsOpen);
    while (k < s.size() && !s.is(k, "{") && !s.is(k, ":") && !s.is(k, ";") && !s.is(k, "=") &&
           !s.is(k, "}"))
        k = s.is(k, "(") ? s.past(k) : k + 1;
    if (s.is(k, ":"))
    {
        ++k;
        while (k < s.size())
        {
            while (k < s.size() && !s.is(k, "(") && !s.is(k, "{") && !s.is(k, ";"))
                ++k;
            if (!s.is(k, "(") && !s.is(k, "{"))
                return 0;
            k = s.past(k); // one member's initialiser
            if (!s.is(k, ","))
                break;
            ++k;
        }
    }
    return s.is(k, "{") && s.match[k] > 0 ? k : 0;
}

// ─── Namespaces ───────────────────────────────────────────────────────────

// Names a namespace body declares (functions, variables, types, nested
// namespaces) at its own level.
void collectMembers(const Scanned &s, size_t b, size_t e, std::set<std::string> &out)
{
    for (auto st : splitStatements(s, b, e))
    {
        size_t j = st.first;
        if (s.is(j, "template") && s.is(j + 1, "<"))
        {
            int angle = 0;
            for (++j; j <= st.second; ++j)
            {
                if (s.is(j, "<"))
                    ++angle;
                else if (s.is(j, ">") && --angle == 0)
                {
                    ++j;
                    break;
                }
            }
        }
        if (s.is(j, "namespace") && s.ident(j + 1))
        {
            out.insert(s.t[j + 1].text);
            continue;
        }
        if (s.is(j, "using") || s.is(j, "public") || s.is(j, "private") || s.is(j, "protected"))
            continue;
        bool typeDecl = false;
        for (size_t k = j; k <= st.second; ++k)
        {
            if (s.is(k, "class") || s.is(k, "struct") || s.is(k, "union") || s.is(k, "enum"))
            {
                size_t nm = k + 1;
                if (s.is(nm, "class") || s.is(nm, "struct"))
                    ++nm; // enum class X
                if (s.ident(nm) && (s.is(nm + 1, "{") || s.is(nm + 1, ":") || s.is(nm + 1, ";") ||
                                    s.is(nm + 1, "final")))
                {
                    out.insert(s.t[nm].text);
                    typeDecl = true;
                }
                // An unscoped enum's constants belong to the enclosing scope.
                if (s.is(k, "enum") && nm == k + 1)
                {
                    size_t open = nm;
                    while (open <= st.second && !s.is(open, "{") && !s.is(open, ";"))
                        ++open;
                    if (s.is(open, "{") && s.match[open] > 0)
                        for (size_t c = open + 1; c < (size_t)s.match[open]; ++c)
                            if (s.ident(c) && (s.is(c - 1, "{") || s.is(c - 1, ",")))
                                out.insert(s.t[c].text);
                }
                break;
            }
            if (s.is(k, "(") || s.is(k, "=") || s.is(k, "{"))
                break;
        }
        if (typeDecl)
            continue;
        // Declarators: an identifier followed by ( = ; [ , or {, skipping
        // initialisers between commas. A qualified declarator (`Cls::f(`,
        // `int Cls::count = 0;`) defines a class member, not a name of this
        // scope; nothing inside parentheses (parameters, ctor initialisers)
        // is a declarator of this scope either.
        for (size_t k = j; k <= st.second; ++k)
        {
            if (s.is(k, "{") || s.is(k, "[") || s.is(k, "("))
            {
                k = s.past(k) - 1;
                continue;
            }
            if (!s.ident(k) || isKeyword(s.t[k].text) || s.is(k - 1, ".") || s.is(k - 1, "->"))
                continue;
            const bool qualified = s.is(k - 1, "::");
            size_t nx = k + 1;
            if (s.is(nx, "("))
            {
                if (!qualified)
                    out.insert(s.t[k].text);
                break; // a function, or a direct-initialised variable
            }
            if (s.is(nx, "=") || s.is(nx, ";") || s.is(nx, "[") || s.is(nx, ",") || s.is(nx, "{"))
            {
                if (qualified)
                    break;
                out.insert(s.t[k].text);
                if (s.is(nx, "="))
                {
                    // skip the initialiser up to the next declarator
                    for (k = nx + 1; k <= st.second && !s.is(k, ","); ++k)
                        if (s.is(k, "(") || s.is(k, "[") || s.is(k, "{"))
                            k = s.past(k) - 1;
                }
            }
        }
    }
}

// C++17 `namespace a::b { ... }` → `namespace a { namespace b { ... } }`.
void splitNestedNamespaces(std::string &src)
{
    Scanned s(src);
    std::vector<Edit> edits;
    for (size_t i = 0; i + 3 < s.size(); ++i)
    {
        if (!s.is(i, "namespace") || !s.ident(i + 1) || !s.is(i + 2, "::"))
            continue;
        std::string header = "namespace " + s.t[i + 1].text;
        int levels = 1;
        size_t j = i + 2;
        while (s.is(j, "::") && s.ident(j + 1))
        {
            header += " { namespace " + s.t[j + 1].text;
            ++levels;
            j += 2;
        }
        if (!s.is(j, "{") || s.match[j] < 0)
            continue;
        edits.push_back({s.t[i].begin, s.t[j - 1].end, header});
        edits.push_back({s.t[s.match[j]].end, s.t[s.match[j]].end, std::string(levels - 1, '}')});
        i = j;
    }
    if (!edits.empty())
        src = applyEdits(src, edits);
}

// Lowers every block of the first namespace found (a namespace may be
// reopened, e.g. prototypes in a header and definitions in its .cpp).
// Returns false when the source has no namespace left to lower.
bool lowerNamespace(std::string &src)
{
    Scanned s(src);
    size_t first = std::string::npos;
    for (size_t i = 0; i + 1 < s.size(); ++i)
    {
        if (!s.is(i, "namespace") || s.is(i - 1, "using") || (i > 0 && s.is(i - 1, "::")))
            continue;
        if (s.is(i + 1, "{") || (s.ident(i + 1) && s.is(i + 2, "{")))
        {
            first = i;
            break;
        }
    }
    if (first == std::string::npos)
        return false;

    std::vector<Edit> edits;
    if (s.is(first + 1, "{"))
    {
        // Anonymous namespace: its members are simply file-level.
        size_t open = first + 1;
        edits.push_back({s.t[first].begin, s.t[open].end, ""});
        if (s.match[open] > 0)
            edits.push_back({s.t[s.match[open]].begin, s.t[s.match[open]].end, ""});
        src = applyEdits(src, edits);
        return true;
    }

    const std::string name = s.t[first + 1].text;
    const std::string prefix = name + "__";
    const int level = s.depth[first];

    std::vector<std::pair<size_t, size_t>> bodies; // (open, close) brace indices
    for (size_t i = first; i + 2 < s.size(); ++i)
        if (s.is(i, "namespace") && s.depth[i] == level && s.is(i + 1, name.c_str()) &&
            s.is(i + 2, "{") && s.match[i + 2] > 0 && !s.is(i - 1, "using"))
        {
            bodies.push_back({i + 2, (size_t)s.match[i + 2]});
            edits.push_back({s.t[i].begin, s.t[i + 2].end, ""});
            edits.push_back({s.t[s.match[i + 2]].begin, s.t[s.match[i + 2]].end, ""});
        }

    std::set<std::string> members;
    for (auto &b : bodies)
        collectMembers(s, b.first + 1, b.second, members);

    std::vector<char> rename(s.size(), 0); // bare member names to prefix
    auto markRange = [&](size_t b, size_t e)
    {
        for (size_t k = b; k < e && k < s.size(); ++k)
            rename[k] = 1;
    };
    for (auto &b : bodies)
        markRange(b.first + 1, b.second);

    // Class scope: inside a class body, and in an out-of-class member
    // definition (`T Cls::f(...) {...}`), the class's own members hide the
    // namespace's names, so those tokens keep their names.
    std::map<std::string, std::set<std::string>> classMembers;
    struct Shadow
    {
        size_t b, e;
        const std::set<std::string> *names;
    };
    std::vector<Shadow> shadows;
    for (auto &b : bodies)
        for (auto st : splitStatements(s, b.first + 1, b.second))
            for (size_t k = st.first; k + 1 <= st.second; ++k)
            {
                if (!(s.is(k, "class") || s.is(k, "struct")) || !s.ident(k + 1) || s.is(k - 1, "enum"))
                    continue;
                size_t open = k + 2;
                while (open <= st.second && !s.is(open, "{") && !s.is(open, ";"))
                    ++open;
                if (s.is(open, "{") && s.match[open] > 0)
                {
                    auto &names = classMembers[s.t[k + 1].text];
                    collectMembers(s, open + 1, (size_t)s.match[open], names);
                    names.erase(s.t[k + 1].text); // the class name itself is the namespace's
                    shadows.push_back({open + 1, (size_t)s.match[open], &names});
                }
                break;
            }
    for (auto &b : bodies)
        for (auto st : splitStatements(s, b.first + 1, b.second))
            for (size_t k = st.first; k + 1 <= st.second; ++k)
            {
                if (s.is(k, "(") || s.is(k, "{") || s.is(k, "="))
                    break;
                if (s.ident(k) && s.is(k + 1, "::") && classMembers.count(s.t[k].text))
                {
                    shadows.push_back({st.first, st.second + 1, &classMembers[s.t[k].text]});
                    break;
                }
            }
    auto shadowed = [&](size_t k)
    {
        for (auto &sh : shadows)
            if (k >= sh.b && k < sh.e && sh.names->count(s.t[k].text))
                return true;
        return false;
    };

    std::set<std::string> usedAll; // `using N::m;` brings m into scope
    for (size_t i = 0; i < s.size(); ++i)
    {
        // using namespace N;
        if (s.is(i, "using") && s.is(i + 1, "namespace") && s.is(i + 2, name.c_str()) &&
            s.is(i + 3, ";"))
        {
            edits.push_back({s.t[i].begin, s.t[i + 3].end, ""});
            markRange(i + 4, s.size());
            i += 3;
            continue;
        }
        // using N::m;
        if (s.is(i, "using") && s.is(i + 1, name.c_str()) && s.is(i + 2, "::") && s.ident(i + 3) &&
            s.is(i + 4, ";"))
        {
            edits.push_back({s.t[i].begin, s.t[i + 4].end, ""});
            usedAll.insert(s.t[i + 3].text);
            for (size_t k = i + 5; k < s.size(); ++k)
                if (s.t[k].text == s.t[i + 3].text)
                    rename[k] = 1;
            i += 4;
            continue;
        }
        // N::m — and for an out-of-namespace definition `T N::f(...) {` or
        // `T N::Cls::f(...) {`, the parameters and body see the namespace's
        // names unqualified (and, for a member, its class's).
        if (s.is(i, name.c_str()) && s.is(i + 1, "::") && s.ident(i + 2) && !s.is(i - 1, "::") &&
            !s.is(i - 1, ".") && !s.is(i - 1, "->"))
        {
            edits.push_back({s.t[i].begin, s.t[i + 2].end, prefix + s.t[i + 2].text});
            size_t last = i + 2; // the declarator name at the end of N::A::f
            std::string cls;
            while (s.is(last + 1, "::") && (s.ident(last + 2) || (s.is(last + 2, "~") && s.ident(last + 3))))
            {
                if (cls.empty())
                    cls = s.t[last].text;
                last += s.is(last + 2, "~") ? 3 : 2;
            }
            if (s.depth[i] == 0 && s.is(last + 1, "("))
            {
                size_t body = bodyAfterParams(s, last + 1);
                if (body)
                {
                    markRange(last + 1, (size_t)s.match[body]);
                    if (classMembers.count(cls))
                        shadows.push_back({last + 1, (size_t)s.match[body], &classMembers[cls]});
                }
            }
            i += 2;
            continue;
        }
    }

    for (size_t k = 0; k < s.size(); ++k)
    {
        if (!rename[k] || !s.ident(k) || !(members.count(s.t[k].text) || usedAll.count(s.t[k].text)))
            continue;
        if (s.is(k - 1, "::") || s.is(k - 1, ".") || s.is(k - 1, "->") || shadowed(k))
            continue;
        if (s.is(k - 1, "namespace"))
            continue; // handled as a nested namespace name below
        edits.push_back({s.t[k].begin, s.t[k].end, prefix + s.t[k].text});
    }
    // Nested namespaces keep their blocks, renamed so the next round lowers
    // them with the combined prefix.
    for (auto &b : bodies)
        for (size_t k = b.first + 1; k < b.second; ++k)
            if (s.is(k, "namespace") && s.ident(k + 1) && s.depth[k] == level + 1)
                edits.push_back({s.t[k + 1].begin, s.t[k + 1].end, prefix + s.t[k + 1].text});

    src = applyEdits(src, edits);
    return true;
}

// ─── Out-of-class member definitions ──────────────────────────────────────

bool paramSig(const Scanned &s, size_t open, std::vector<std::string> &sig);

struct ClassInfo
{
    size_t open, close; // brace token indices
};

std::map<std::string, ClassInfo> findClasses(const Scanned &s)
{
    std::map<std::string, ClassInfo> out;
    for (size_t i = 0; i + 1 < s.size(); ++i)
    {
        if (!(s.is(i, "class") || s.is(i, "struct")) || !s.ident(i + 1) || s.is(i - 1, "enum"))
            continue;
        size_t j = i + 2;
        while (j < s.size() && !s.is(j, "{") && !s.is(j, ";") && !s.is(j, "(") && !s.is(j, "=") &&
               !s.is(j, ")") && !s.is(j, ","))
            ++j;
        if (s.is(j, "{") && s.match[j] > 0 && !out.count(s.t[i + 1].text))
            out[s.t[i + 1].text] = {j, (size_t)s.match[j]};
    }
    return out;
}

// ─── Enums nested in a class ──────────────────────────────────────────────
//
//   struct Expr { enum class Kind { A, B }; Kind k = Kind::A; };
//     → enum class Expr__Kind { A, B };
//       struct Expr { Expr__Kind k = Expr__Kind::A; };
//
// and Expr::Kind becomes Expr__Kind everywhere, the way namespaces lower.

bool hoistNestedEnums(std::string &src)
{
    if (src.find("enum") == std::string::npos)
        return false;
    Scanned s(src);
    std::vector<Edit> edits;
    std::set<std::pair<std::string, std::string>> qualified; // (class, enum)
    for (const auto &c : findClasses(s))
    {
        const std::string &cls = c.first;
        const size_t open = c.second.open, close = c.second.close;
        const int level = s.depth[open] + 1;
        std::map<std::string, std::string> local; // Kind → Expr__Kind
        std::vector<std::pair<size_t, size_t>> removed;
        std::string hoisted;
        for (size_t k = open + 1; k < close; ++k)
        {
            if (!s.is(k, "enum") || s.depth[k] != level)
                continue;
            size_t n = k + 1;
            if (s.is(n, "class") || s.is(n, "struct"))
                ++n;
            if (!s.ident(n))
                continue; // anonymous enum: its enumerators stay members
            size_t b = n + 1;
            while (b < close && !s.is(b, "{") && !s.is(b, ";"))
                ++b;
            if (!s.is(b, "{") || s.match[b] < 0 || !s.is((size_t)s.match[b] + 1, ";"))
                continue; // `enum E { ... } e;` declares a member too: leave it
            const size_t e = (size_t)s.match[b], semi = e + 1;
            const std::string flat = cls + "__" + s.t[n].text;
            local[s.t[n].text] = flat;
            qualified.insert({cls, s.t[n].text});
            hoisted += s.text(k, n - 1) + " " + flat + " " + s.text(n + 1, e) + ";\n";
            edits.push_back({s.t[k].begin, s.t[semi].end, ""});
            removed.push_back({k, semi});
            k = semi;
        }
        if (local.empty())
            continue;
        // Unqualified uses inside the class (`Kind kind = Kind::A;`) and in
        // its out-of-class member definitions (`... Expr::f(Kind k) { ... }`).
        auto renameIn = [&](size_t first, size_t last)
        {
            for (size_t k = first; k <= last && k < s.size(); ++k)
            {
                bool inRemoved = false;
                for (const auto &r : removed)
                    inRemoved = inRemoved || (k >= r.first && k <= r.second);
                if (inRemoved || !s.ident(k) || !local.count(s.t[k].text) || s.is(k - 1, "::") ||
                    s.is(k - 1, ".") || s.is(k - 1, "->"))
                    continue;
                edits.push_back({s.t[k].begin, s.t[k].end, local[s.t[k].text]});
            }
        };
        renameIn(open + 1, close - 1);
        for (size_t k = 0; k + 3 < s.size(); ++k)
        {
            if (s.depth[k] != 0 || !s.is(k, cls.c_str()) || !s.is(k + 1, "::") || s.is(k - 1, "::"))
                continue;
            size_t paren = k + 2;
            while (paren < k + 6 && paren < s.size() && !s.is(paren, "(") && !s.is(paren, ";") &&
                   !s.is(paren, "{"))
                ++paren;
            if (!s.is(paren, "("))
                continue;
            size_t body = bodyAfterParams(s, paren);
            if (body)
                renameIn(paren, (size_t)s.match[body]);
        }
        // The hoisted enums go right before `struct Expr`.
        size_t kw = open;
        while (kw > 0 && !((s.is(kw, "struct") || s.is(kw, "class")) && s.is(kw + 1, cls.c_str())))
            --kw;
        edits.push_back({s.t[kw].begin, s.t[kw].begin, hoisted});
    }
    if (qualified.empty())
        return false;
    // Qualified uses anywhere: Expr::Kind → Expr__Kind
    for (size_t k = 0; k + 2 < s.size(); ++k)
        if (s.ident(k) && s.is(k + 1, "::") && s.ident(k + 2) && qualified.count({s.t[k].text, s.t[k + 2].text}) &&
            !s.is(k - 1, "::"))
            edits.push_back({s.t[k].begin, s.t[k + 2].end, s.t[k].text + "__" + s.t[k + 2].text});
    src = applyEdits(src, edits);
    return true;
}

// The member name a definition names after "Cls::" at index i — "f", "~Cls",
// or "operator+" — and the index just past it.
std::pair<std::string, size_t> memberNameAt(const Scanned &s, size_t i)
{
    if (s.is(i, "~") && s.ident(i + 1))
        return {"~" + s.t[i + 1].text, i + 2};
    if (s.is(i, "operator"))
    {
        std::string nm = "operator";
        size_t j = i + 1;
        if (s.is(j, "(") && s.is(j + 1, ")"))
            return {nm + "()", j + 2};
        while (j < s.size() && !s.is(j, "("))
            nm += s.t[j++].text;
        return {nm, j};
    }
    if (s.ident(i))
        return {s.t[i].text, i + 1};
    return {"", i};
}

bool mergeOutOfClassMembers(std::string &src)
{
    Scanned s(src);
    auto classes = findClasses(s);
    if (classes.empty())
        return false;

    std::vector<Edit> edits;
    std::map<std::string, std::string> moved; // class → member text to add
    std::set<std::pair<size_t, size_t>> removedProtos;

    for (auto st : splitStatements(s, 0, s.size()))
    {
        // Locate "Cls::member" (or a class template's "Cls<T>::member") at
        // the statement's own level; `colons` is the index of its "::".
        size_t q = std::string::npos, colons = 0;
        for (size_t j = st.first; j + 2 <= st.second; ++j)
        {
            if (s.is(j, "(") || s.is(j, "{") || s.is(j, "=") || s.is(j, ";"))
                break;
            if (!s.ident(j) || !classes.count(s.t[j].text) || s.is(j - 1, "::"))
                continue;
            size_t c = j + 1;
            if (s.is(c, "<"))
            {
                int angle = 0;
                for (; c <= st.second; ++c)
                    if (s.is(c, "<"))
                        ++angle;
                    else if (s.is(c, ">") && --angle == 0)
                        break;
                ++c;
            }
            if (!s.is(c, "::"))
                continue;
            // Skip a class-qualified return type: "Cls::Inner Cls::f()"
            auto nm = memberNameAt(s, c + 1);
            if (nm.first.empty() || s.is(nm.second, "::"))
                continue;
            q = j;
            colons = c;
            break;
        }
        if (q == std::string::npos || s.depth[q] != 0)
            continue;
        const std::string cls = s.t[q].text;
        const ClassInfo &ci = classes[cls];
        if (st.first > ci.open && st.first < ci.close)
            continue;
        auto [member, after] = memberNameAt(s, colons + 1);
        // A class lowered out of a namespace (geo__Point) keeps its short
        // name in an out-of-class constructor / destructor (geo__Point::Point).
        std::string name = member;
        {
            const bool dtor = !member.empty() && member[0] == '~';
            const std::string bare = dtor ? member.substr(1) : member;
            if (bare != cls && cls.size() > bare.size() + 2 &&
                cls.compare(cls.size() - bare.size() - 2, std::string::npos, "__" + bare) == 0)
                name = (dtor ? "~" : "") + cls;
        }

        auto protos = splitStatements(s, ci.open + 1, ci.close);
        auto protoParen = [&](std::pair<size_t, size_t> p) -> size_t
        {
            // the '(' right after the member's name in a body-less declaration
            if (!s.is(p.second, ";"))
                return 0;
            for (size_t j = p.first; j < p.second; ++j)
            {
                if (s.is(j, "{") || s.is(j, "="))
                    return 0;
                auto nm = memberNameAt(s, j);
                if (nm.first == name && !s.is(j - 1, "::") && s.is(nm.second, "("))
                    return nm.second;
                if (s.is(j, "("))
                    return 0;
            }
            return 0;
        };
        auto hasWord = [&](std::pair<size_t, size_t> p, const char *w)
        {
            for (size_t j = p.first; j <= p.second; ++j)
                if (s.is(j, w))
                    return true;
            return false;
        };

        std::vector<Edit> local; // edits inside the moved text (absolute offsets)
        local.push_back({s.t[q].begin, s.t[colons].end, ""});
        // `template <typename T>` before a class template's member: the
        // member needs no prefix inside the class.
        if (s.is(st.first, "template") && s.is(st.first + 1, "<"))
        {
            size_t gt = st.first + 1;
            for (int angle = 0; gt < q; ++gt)
                if (s.is(gt, "<"))
                    ++angle;
                else if (s.is(gt, ">") && --angle == 0)
                    break;
            if (gt < q)
                local.push_back({s.t[st.first].begin, s.t[gt].end, ""});
        }
        if (name != member)
        {
            size_t nameTok = s.is(colons + 1, "~") ? colons + 2 : colons + 1;
            local.push_back({s.t[nameTok].begin, s.t[nameTok].end, cls});
        }
        std::string qualifiers;
        size_t end;

        if (s.is(after, "("))
        {
            // Member function: params, qualifiers, optional ctor-initialiser, body.
            size_t k = bodyAfterParams(s, after);
            if (!k)
                continue; // declaration only, = default, etc. — leave as is
            end = (size_t)s.match[k];
            // The in-class declaration this defines: the one with the same
            // parameter types, else the same parameter count (other
            // overloads are matched by their own definitions).
            std::vector<std::string> defSig, protoSig;
            bool defSigOk = paramSig(s, after, defSig);
            const size_t defCount = splitParams(s, after, (size_t)s.match[after]).size();
            std::pair<size_t, size_t> pick{0, 0};
            size_t pickParen = 0;
            for (int pass = 0; pass < 2 && !pickParen; ++pass)
                for (auto p : protos)
                {
                    size_t pp = protoParen(p);
                    if (!pp || removedProtos.count(p))
                        continue;
                    bool fits = pass == 0
                                    ? defSigOk && paramSig(s, pp, protoSig) && protoSig == defSig
                                    : splitParams(s, pp, (size_t)s.match[pp]).size() == defCount;
                    if (fits)
                    {
                        pick = p;
                        pickParen = pp;
                        break;
                    }
                }
            if (pickParen)
            {
                inheritDefaults(s, pickParen, after, local);
                if (hasWord(pick, "static"))
                    qualifiers = "static ";
                removedProtos.insert(pick);
            }
        }
        else if (s.is(after, "=") || s.is(after, ";") || s.is(after, "{"))
        {
            // Static data member definition: T Cls::count = 0;
            end = st.second;
            bool merged = false;
            for (auto p : protos)
            {
                if (!s.is(p.second, ";") || !hasWord(p, "static"))
                    continue;
                for (size_t j = p.first; j < p.second; ++j)
                    if (s.is(j, member.c_str()) && (s.is(j + 1, ";") || s.is(j + 1, "[") ||
                                                    s.is(j + 1, "=") || s.is(j + 1, "{")))
                    {
                        // Only an in-class declaration without a value takes
                        // the definition's (`static const int N = 3;` has one).
                        if (!s.is(after, ";") && (s.is(j + 1, ";") || s.is(j + 1, "[")))
                        {
                            size_t initEnd = s.is(end, ";") ? end - 1 : end;
                            std::string init = s.is(after, "=") ? s.text(after, initEnd)
                                                                : "= " + s.text(after, initEnd);
                            edits.push_back({s.t[p.second].begin, s.t[p.second].begin, " " + init});
                        }
                        merged = true;
                        break;
                    }
                if (merged)
                    break;
            }
            edits.push_back({s.t[st.first].begin, s.t[end].end, ""});
            if (!merged)
            {
                std::string text = s.text(st.first, end);
                text = applyEdits(text, {{s.t[q].begin - s.t[st.first].begin,
                                          s.t[colons].end - s.t[st.first].begin, ""}});
                moved[cls] += "\n    static " + text;
            }
            continue;
        }
        else
            continue;

        size_t base = s.t[st.first].begin;
        std::string text = s.text(st.first, end);
        for (auto &e : local)
        {
            e.begin -= base;
            e.end -= base;
        }
        moved[cls] += "\n" + qualifiers + applyEdits(text, local) + "\n";
        edits.push_back({s.t[st.first].begin, s.t[end].end, ""});
    }

    if (moved.empty() && edits.empty())
        return false;
    for (auto &p : removedProtos)
        edits.push_back({s.t[p.first].begin, s.t[p.second].end, ""});
    for (auto &[cls, text] : moved)
    {
        size_t at = s.t[classes[cls].close].begin;
        // Members land in the public section the definitions came from.
        edits.push_back({at, at, "\npublic:" + text});
    }
    src = applyEdits(src, edits);
    return true;
}

// ─── Free operator functions ──────────────────────────────────────────────

// The VM's name stem for an overloadable binary operator, or "".
std::string operatorStem(const std::string &op)
{
    static const std::map<std::string, std::string> stems = {
        {"<<", "lshift"}, {">>", "rshift"}, {"+", "add"}, {"-", "sub"}, {"*", "mul"},
        {"/", "div"},     {"%", "mod"},     {"==", "eq"}, {"!=", "ne"}, {"<", "lt"},
        {">", "gt"},      {"<=", "le"},     {">=", "ge"}};
    auto it = stems.find(op);
    return it == stems.end() ? "" : it->second;
}

// The type a parameter is declared as: `const Item& item` → Item,
// `std::ostream& os` → ostream, an unnamed `const Item&` → Item.
std::string paramClass(const Scanned &s, std::pair<size_t, size_t> p)
{
    std::vector<std::string> idents;
    int angle = 0;
    for (size_t j = p.first; j <= p.second; ++j)
    {
        if (s.is(j, "="))
            break;
        if (s.is(j, "<"))
            ++angle;
        else if (s.is(j, ">") && angle > 0)
            --angle;
        else if (angle == 0 && s.ident(j) && !s.is(j + 1, "::") && s.t[j].text != "const" &&
                 s.t[j].text != "volatile" && s.t[j].text != "struct" && s.t[j].text != "class")
            idents.push_back(s.t[j].text);
    }
    if (idents.size() >= 2)
        return idents[idents.size() - 2];
    return idents.empty() ? "" : idents[0];
}

// `std::ostream& operator<<(std::ostream& os, const Item& item)` and the other
// free (or friend) binary operators become ordinary functions named
// __op__<stem>__<Class> — Class being the streamed type for << and >>, the
// left operand's type otherwise — which the VM calls for instances of that
// class or its subclasses (cout << item, a == b). A friend declaration inside
// a class is dropped; a friend defined there moves out after the class.
bool lowerFreeOperators(std::string &src)
{
    if (src.find("operator") == std::string::npos)
        return false;
    Scanned s(src);
    auto classes = findClasses(s);
    std::vector<Edit> edits;
    std::map<size_t, std::string> movedOut; // insertion offset → text

    for (size_t i = 0; i < s.size(); ++i)
    {
        if (!s.is(i, "operator") || s.is(i - 1, "::") || s.is(i - 1, ".") || s.is(i - 1, "->"))
            continue;
        size_t j = i + 1;
        std::string op;
        while (j < s.size() && j < i + 3 && s.t[j].kind == Tok::Punct && !s.is(j, "("))
            op += s.t[j++].text;
        if (!s.is(j, "(") || s.match[j] < 0)
            continue;
        const std::string stem = operatorStem(op);
        auto params = splitParams(s, j, (size_t)s.match[j]);
        if (stem.empty() || params.size() != 2)
            continue; // a member operator (one parameter) stays with its class
        const std::string cls =
            paramClass(s, params[stem == "lshift" || stem == "rshift" ? 1 : 0]);
        if (cls.empty())
            continue;
        const std::string newName = "__op__" + stem + "__" + cls;
        Edit rename{s.t[i].begin, s.t[j - 1].end, newName};

        // Inside a class body: only a friend is a free function.
        const ClassInfo *owner = nullptr;
        for (auto &[name, ci] : classes)
            if (i > ci.open && i < ci.close && (!owner || ci.open > owner->open))
                owner = &ci;
        if (!owner)
        {
            if (s.depth[i] == 0)
                edits.push_back(rename);
            continue;
        }
        for (auto st : splitStatements(s, owner->open + 1, owner->close))
        {
            if (i < st.first || i > st.second)
                continue;
            size_t fr = st.first;
            while (fr < i && !s.is(fr, "friend"))
                ++fr;
            if (fr == i)
                break; // a member operator
            edits.push_back({s.t[st.first].begin, s.t[st.second].end, ""});
            if (s.is(st.second, "}"))
            {
                // friend defined in the class: the same function, after it
                std::string text = s.text(st.first, st.second);
                size_t base = s.t[st.first].begin;
                text = applyEdits(text, {{s.t[fr].begin - base, s.t[fr].end - base, ""},
                                         {rename.begin - base, rename.end - base, newName}});
                size_t after = owner->close;
                if (s.is(after + 1, ";"))
                    ++after;
                movedOut[s.t[after].end] += "\n" + text + "\n";
            }
            break;
        }
    }
    for (auto &[at, text] : movedOut)
        edits.push_back({at, at, text});
    if (edits.empty())
        return false;
    src = applyEdits(src, edits);
    return true;
}

// ─── Free-function prototypes ─────────────────────────────────────────────

bool isBuiltinTypeWord(const std::string &w)
{
    static const std::set<std::string> words = {
        "int", "char", "double", "float", "long", "short", "unsigned", "signed", "bool",
        "void", "auto", "size_t", "wchar_t"};
    return words.count(w) > 0;
}

// A parameter list's type signature: each parameter's tokens before its
// default, without the parameter name (`const std::string& title` and
// `const std::string&` both give "const std :: string &"). Empty when a
// parameter isn't a declaration (a bare `x`, as in a call `f(x)`).
bool paramSig(const Scanned &s, size_t open, std::vector<std::string> &sig)
{
    sig.clear();
    for (auto p : splitParams(s, open, (size_t)s.match[open]))
    {
        size_t last = p.second;
        if (size_t eq = paramDefaultEq(s, p))
        {
            if (eq == p.first)
                return false;
            last = eq - 1;
        }
        if (last == p.first && !isBuiltinTypeWord(s.t[last].text) && !s.is(last, "..."))
            return false; // a lone identifier: an argument, not a declaration
        if (last > p.first && s.ident(last) && !isBuiltinTypeWord(s.t[last].text))
            --last; // drop the parameter name
        std::string t;
        for (size_t j = p.first; j <= last; ++j)
            t += s.t[j].text + " ";
        sig.push_back(t);
    }
    return true;
}

// The return type of a function statement whose name precedes `paren`,
// without storage/inline specifiers.
std::string returnTypeText(const Scanned &s, size_t first, size_t paren)
{
    static const std::set<std::string> specifiers = {
        "static", "inline", "extern", "virtual", "friend", "constexpr", "explicit"};
    std::string t;
    for (size_t j = first; j + 1 < paren; ++j)
        if (!specifiers.count(s.t[j].text))
            t += s.t[j].text + " ";
    return t;
}

// Prototypes of functions the file defines are dropped — a body-less
// declaration would otherwise compile to an empty function, which replaces
// the real one when it comes later (a header spliced in after the
// definition). A prototype's default arguments move onto the definition.
// Declarations match by name, return type and parameter types, so calls and
// other statements are never taken for prototypes.
void reconcilePrototypes(std::string &src)
{
    if (src.find(';') == std::string::npos)
        return;
    Scanned s(src);
    struct Fn
    {
        std::pair<size_t, size_t> st;
        size_t paren;
        std::string ret;
        std::vector<std::string> sig;
    };
    std::multimap<std::string, Fn> defs;
    std::vector<std::pair<std::string, Fn>> protos;
    for (auto st : splitStatements(s, 0, s.size()))
    {
        size_t paren = functionParen(s, st);
        if (!paren || s.match[paren] < 0 || paren < st.first + 2)
            continue; // no return type before the name
        // `obj.f(...)` is a call and `Cls::f` a member; a qualified return
        // type (`std::string f(`) is fine.
        bool memberAccess = s.is(paren - 2, "::");
        for (size_t j = st.first; j < paren; ++j)
            memberAccess = memberAccess || s.is(j, ".") || s.is(j, "->");
        if (memberAccess)
            continue;
        Fn fn{st, paren, returnTypeText(s, st.first, paren), {}};
        if (!paramSig(s, paren, fn.sig))
            continue;
        const std::string &nm = s.t[paren - 1].text;
        if (s.is(st.second, "}"))
            defs.insert({nm, fn});
        else if (s.is(st.second, ";") && s.is((size_t)s.match[paren] + 1, ";"))
            protos.push_back({nm, fn});
    }

    std::vector<Edit> edits;
    for (auto &[nm, proto] : protos)
    {
        auto range = defs.equal_range(nm);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (it->second.ret != proto.ret || it->second.sig != proto.sig)
                continue;
            inheritDefaults(s, proto.paren, it->second.paren, edits);
            edits.push_back({s.t[proto.st.first].begin, s.t[proto.st.second].end, ""});
            break;
        }
    }
    if (!edits.empty())
        src = applyEdits(src, edits);
}

} // namespace

// ─── Entry point ──────────────────────────────────────────────────────────

std::string applyCppDialect(const std::string &source, const std::string &path, bool *multiFile)
{
    std::string out = source;
    if (multiFile)
        *multiFile = false;

    if (out.find("#include") != std::string::npos && out.find('"') != std::string::npos &&
        path.size() && path[0] != '<')
    {
        IncludeState st;
        fs::path root = fs::u8path(path);
        st.seen.insert(pathKey(root));
        st.queued.insert(pathKey(root));
        std::string expanded = expandIncludes(out, root, st);
        for (size_t k = 0; k < st.impls.size(); ++k)
        {
            std::string key = pathKey(st.impls[k]);
            if (st.seen.count(key))
                continue;
            st.seen.insert(key);
            expanded += "\n" + expandIncludes(readTextFile(st.impls[k]), st.impls[k], st) + "\n";
        }
        if (st.seen.size() > 1)
        {
            out = expanded;
            if (multiFile)
                *multiFile = true;
        }
    }

    const std::string ext = fileExtLower(path);
    if (ext == ".c" || ext == ".cpp")
        renameKeywordClashes(out);

    if (out.find("namespace") != std::string::npos)
    {
        splitNestedNamespaces(out);
        // Each round lowers one namespace (nested ones resurface renamed).
        for (int guard = 0; guard < 64 && lowerNamespace(out); ++guard)
        {
        }
    }

    // Before the member merge, so `Expr::Kind Expr::kind()` reads as one
    // return type.
    if (out.find("enum") != std::string::npos)
        hoistNestedEnums(out);

    if (out.find("::") != std::string::npos)
    {
        for (int guard = 0; guard < 4 && mergeOutOfClassMembers(out); ++guard)
        {
        }
    }

    lowerFreeOperators(out);
    reconcilePrototypes(out);
    return out;
}

bool definesCStyleMain(const std::string &src)
{
    static const std::regex mainDef(R"((^|\n)[ \t]*(static[ \t]+)?(int|void|auto)[ \t]+main[ \t]*\()");
    if (!std::regex_search(src, mainDef))
        return false;
    // ...and never calls main() at file level itself (a call inside a
    // function, e.g. a "restart" that re-enters main, doesn't count).
    Scanned s(src);
    for (size_t i = 0; i + 1 < s.size(); ++i)
        if (s.is(i, "main") && s.is(i + 1, "(") && s.depth[i] == 0 && !s.ident(i - 1) &&
            !s.is(i - 1, "*") && !s.is(i - 1, "&") && !s.is(i - 1, "::"))
            return false;
    return true;
}

// ─── Shared with the typed front-end (src/cpp/CppLex.cpp) ─────────────────

std::filesystem::path cppResolveInclude(const std::string &name, const std::filesystem::path &from)
{
    return findInclude(name, from);
}

std::filesystem::path cppImplementationOf(const std::filesystem::path &header)
{
    return isHeaderExt(header) ? findImplementation(header) : std::filesystem::path();
}
