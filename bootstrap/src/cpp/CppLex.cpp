// Lexer + preprocessor for the typed C/C++ front-end (see Cpp.h).
#include "Cpp.h"
#include "Dialect.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace fs = std::filesystem;

namespace cpp
{
namespace
{

// ─── Lexer ─────────────────────────────────────────────────────────────────

bool identStart(char c) { return std::isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }
bool identChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80; }

void appendUtf8(std::string &out, unsigned long cp)
{
    if (cp < 0x80)
        out += (char)cp;
    else if (cp < 0x800)
    {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

class Lexer
{
public:
    Lexer(const std::string &src, int file) : s_(src), file_(file) {}

    std::vector<Token> run()
    {
        std::vector<Token> out;
        bool bol = true, space = false;
        while (true)
        {
            // whitespace, comments, line continuations
            bool progressed = true;
            while (progressed && i_ < s_.size())
            {
                progressed = false;
                char c = s_[i_];
                if (c == '\\' && (peek(1) == '\n' || (peek(1) == '\r' && peek(2) == '\n')))
                {
                    i_ += peek(1) == '\n' ? 2 : 3;
                    ++line_;
                    progressed = true;
                }
                else if (c == '\n')
                {
                    ++i_;
                    ++line_;
                    bol = true;
                    space = true;
                    progressed = true;
                }
                else if (std::isspace((unsigned char)c))
                {
                    ++i_;
                    space = true;
                    progressed = true;
                }
                else if (c == '/' && peek(1) == '/')
                {
                    while (i_ < s_.size() && s_[i_] != '\n')
                    {
                        if (s_[i_] == '\\' && peek(1) == '\n')
                        {
                            ++line_;
                            ++i_;
                        }
                        ++i_;
                    }
                    space = true;
                    progressed = true;
                }
                else if (c == '/' && peek(1) == '*')
                {
                    i_ += 2;
                    while (i_ < s_.size() && !(s_[i_] == '*' && peek(1) == '/'))
                    {
                        if (s_[i_] == '\n')
                            ++line_;
                        ++i_;
                    }
                    i_ = std::min(s_.size(), i_ + 2);
                    space = true;
                    progressed = true;
                }
            }
            if (i_ >= s_.size())
                break;
            Token t;
            t.file = file_;
            t.line = line_;
            t.col = column();
            t.bol = bol;
            t.spaceBefore = space;
            bol = false;
            space = false;
            lexOne(t);
            out.push_back(std::move(t));
        }
        Token end;
        end.kind = TK::End;
        end.file = file_;
        end.line = line_;
        end.bol = true;
        out.push_back(end);
        return out;
    }

private:
    const std::string &s_;
    int file_;
    size_t i_ = 0;
    int line_ = 1;

    char peek(size_t k) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }

    // 1-based column of s_[i_] on its line (for compiler-style diagnostics).
    int column() const
    {
        if (i_ == 0)
            return 1;
        size_t nl = s_.rfind('\n', i_ - 1);
        return nl == std::string::npos ? (int)i_ + 1 : (int)(i_ - nl);
    }

    // An escape sequence after the backslash at s_[i_] (which is consumed).
    void escape(std::string &out)
    {
        ++i_; // backslash
        char c = i_ < s_.size() ? s_[i_++] : '\\';
        switch (c)
        {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7':
        {
            int v = c - '0';
            for (int k = 0; k < 2 && i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '7'; ++k)
                v = v * 8 + (s_[i_++] - '0');
            out += (char)v;
            break;
        }
        case 'x':
        {
            unsigned long v = 0;
            while (i_ < s_.size() && std::isxdigit((unsigned char)s_[i_]))
            {
                char h = s_[i_++];
                v = v * 16 + (std::isdigit((unsigned char)h) ? h - '0' : (std::tolower(h) - 'a' + 10));
            }
            out += (char)(v & 0xFF);
            break;
        }
        case 'u':
        case 'U':
        {
            int n = c == 'u' ? 4 : 8;
            unsigned long v = 0;
            for (int k = 0; k < n && i_ < s_.size() && std::isxdigit((unsigned char)s_[i_]); ++k)
            {
                char h = s_[i_++];
                v = v * 16 + (std::isdigit((unsigned char)h) ? h - '0' : (std::tolower(h) - 'a' + 10));
            }
            appendUtf8(out, v);
            break;
        }
        case 'a': out += '\a'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'v': out += '\v'; break;
        case 'e': out += '\x1b'; break;
        default: out += c; break; // \\ \' \" \?
        }
    }

    void quoted(Token &t, char q)
    {
        size_t start = i_;
        ++i_; // opening quote
        std::string v;
        while (i_ < s_.size() && s_[i_] != q && s_[i_] != '\n')
        {
            if (s_[i_] == '\\')
                escape(v);
            else
                v += s_[i_++];
        }
        if (i_ < s_.size() && s_[i_] == q)
            ++i_;
        t.kind = q == '"' ? TK::String : TK::Char;
        t.value = v;
        t.text = s_.substr(start, i_ - start);
    }

    void lexOne(Token &t)
    {
        char c = s_[i_];
        // Literal prefixes: u8"..", L'x', R"(...)", u8R"x(...)x"
        if (identStart(c))
        {
            size_t j = i_;
            while (j < s_.size() && identChar(s_[j]))
                ++j;
            std::string id = s_.substr(i_, j - i_);
            if (j < s_.size() && (s_[j] == '"' || s_[j] == '\''))
            {
                bool raw = !id.empty() && id.back() == 'R' && s_[j] == '"';
                std::string pre = raw ? id.substr(0, id.size() - 1) : id;
                if (pre.empty() || pre == "u8" || pre == "u" || pre == "U" || pre == "L")
                {
                    i_ = j;
                    if (raw)
                    {
                        size_t open = s_.find('(', i_);
                        std::string delim = s_.substr(i_ + 1, open - i_ - 1);
                        std::string close = ")" + delim + "\"";
                        size_t e = s_.find(close, open);
                        if (e == std::string::npos)
                            e = s_.size();
                        std::string v = s_.substr(open + 1, e - open - 1);
                        for (char ch : v)
                            if (ch == '\n')
                                ++line_;
                        t.kind = TK::String;
                        t.value = v;
                        i_ = std::min(s_.size(), e + close.size());
                        t.text = "R\"" + v + "\"";
                        return;
                    }
                    quoted(t, s_[i_]);
                    return;
                }
            }
            t.kind = TK::Ident;
            t.text = id;
            i_ = j;
            return;
        }
        if (std::isdigit((unsigned char)c) || (c == '.' && std::isdigit((unsigned char)peek(1))))
        {
            size_t j = i_;
            bool isFloat = false;
            bool hex = c == '0' && (peek(1) == 'x' || peek(1) == 'X');
            while (j < s_.size())
            {
                char d = s_[j];
                if (identChar(d) || d == '.' || (d == '\'' && j + 1 < s_.size() && identChar(s_[j + 1])))
                {
                    if (d == '.')
                        isFloat = true;
                    if (!hex && (d == 'e' || d == 'E') && j + 1 < s_.size() &&
                        (s_[j + 1] == '+' || s_[j + 1] == '-'))
                    {
                        isFloat = true;
                        j += 2;
                        continue;
                    }
                    if (!hex && (d == 'e' || d == 'E'))
                        isFloat = true;
                    if (hex && (d == 'p' || d == 'P'))
                    {
                        isFloat = true;
                        if (j + 1 < s_.size() && (s_[j + 1] == '+' || s_[j + 1] == '-'))
                            ++j;
                    }
                    ++j;
                }
                else
                    break;
            }
            t.text = s_.substr(i_, j - i_);
            t.text.erase(std::remove(t.text.begin(), t.text.end(), '\''), t.text.end());
            t.kind = isFloat ? TK::Float : TK::Int;
            i_ = j;
            return;
        }
        if (c == '"' || c == '\'')
        {
            quoted(t, c);
            return;
        }
        static const char *puncts[] = {
            ">>=", "<<=", "...", "->*", "<=>", "::", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
            "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##", ".*"};
        for (const char *p : puncts)
        {
            size_t n = std::char_traits<char>::length(p);
            if (s_.compare(i_, n, p) == 0)
            {
                t.kind = TK::Punct;
                t.text = p;
                i_ += n;
                return;
            }
        }
        t.kind = TK::Punct;
        t.text = std::string(1, c);
        ++i_;
    }
};

// ─── Preprocessor ──────────────────────────────────────────────────────────

struct Macro
{
    bool func = false;
    bool variadic = false;
    std::vector<std::string> params;
    std::vector<Token> body;
};

std::string readFile(const fs::path &p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
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

class Preprocessor
{
public:
    Preprocessed out;

    Preprocessor()
    {
        auto def = [&](const std::string &name, const std::string &body)
        {
            Macro m;
            Lexer lx(body, 0);
            m.body = lx.run();
            m.body.pop_back(); // End
            macros_[name] = m;
        };
        def("__cplusplus", "201703L");
        def("_WIN32", "1");
        def("_WIN64", "1");
        def("NULL", "nullptr");
        def("EOF", "(-1)");
        def("EXIT_SUCCESS", "0");
        def("EXIT_FAILURE", "1");
        def("RAND_MAX", "32767");
        def("CHAR_BIT", "8");
        def("SCHAR_MAX", "127");
        def("CHAR_MAX", "127");
        def("UCHAR_MAX", "255");
        def("SHRT_MAX", "32767");
        def("INT_MAX", "2147483647");
        def("INT_MIN", "(-2147483647-1)");
        def("UINT_MAX", "4294967295u");
        def("LONG_MAX", "2147483647L");
        def("LONG_MIN", "(-2147483647L-1)");
        def("LLONG_MAX", "9223372036854775807LL");
        def("LLONG_MIN", "(-9223372036854775807LL-1)");
        def("ULLONG_MAX", "18446744073709551615ULL");
        def("SIZE_MAX", "18446744073709551615ULL");
        def("INT32_MAX", "2147483647");
        def("INT32_MIN", "(-2147483647-1)");
        def("INT64_MAX", "9223372036854775807LL");
        def("INT64_MIN", "(-9223372036854775807LL-1)");
        def("UINT32_MAX", "4294967295u");
        def("DBL_MAX", "1.7976931348623157e308");
        def("DBL_MIN", "2.2250738585072014e-308");
        def("DBL_EPSILON", "2.220446049250313e-16");
        def("FLT_MAX", "3.402823466e38");
        def("FLT_EPSILON", "1.192092896e-07");
        def("M_PI", "3.14159265358979323846");
        def("M_E", "2.71828182845904523536");
        def("M_SQRT2", "1.41421356237309504880");
        def("INFINITY", "(1.0/0.0)");
        def("NAN", "(0.0/0.0)");
    }

    bool cMode_ = false;

    void runMain(const fs::path &path, const std::string &source)
    {
        out.cMode = cMode_;
        mainKey_ = pathKey(path);
        included_.insert(mainKey_);
        queued_.insert(mainKey_);
        processFile(path, source, 0);
        for (size_t k = 0; k < impls_.size(); ++k)
        {
            const fs::path impl = impls_[k]; // a copy: processing may grow impls_
            std::string key = pathKey(impl);
            if (included_.count(key))
                continue;
            included_.insert(key);
            ++tu_;
            processFile(impl, readFile(impl), 0);
        }
        Token end;
        end.kind = TK::End;
        end.tu = tu_;
        out.tokens.push_back(end);
        if (cMode_)
            toCTokens();
    }

    // C has none of C++'s extra keywords, and no `::` operator: `class`,
    // `new`, `this`, ... are ordinary identifiers (int class = 3; is C), so
    // C++ constructs fail exactly as they do in a C compiler.
    void toCTokens()
    {
        static const std::set<std::string> cppOnly = {
            "catch", "class", "concept", "consteval", "constinit", "const_cast", "co_await", "co_return",
            "co_yield", "decltype", "delete", "dynamic_cast", "explicit", "export", "friend", "mutable",
            "namespace", "new", "noexcept", "operator", "private", "protected", "public",
            "reinterpret_cast", "requires", "static_cast", "template", "this", "throw", "try", "typeid",
            "typename", "using", "virtual", "and", "and_eq", "bitand", "bitor", "compl", "not", "not_eq",
            "or", "or_eq", "xor", "xor_eq", "char16_t", "char32_t", "char8_t", "wchar_t"};
        std::vector<Token> toks;
        toks.reserve(out.tokens.size());
        for (Token &t : out.tokens)
        {
            if (t.kind == TK::Ident && cppOnly.count(t.text))
                t.text = std::string(1, kPlainIdent) + t.text;
            if (t.kind == TK::Punct && t.text == "::")
            {
                Token a = t, b = t;
                a.text = b.text = ":";
                b.col = t.col + 1;
                b.bol = false;
                b.spaceBefore = false;
                toks.push_back(a);
                toks.push_back(b);
                continue;
            }
            toks.push_back(std::move(t));
        }
        out.tokens.swap(toks);
    }

    static bool isCStandardHeader(const std::string &h)
    {
        static const std::set<std::string> c = {
            // ISO C (through C23)
            "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h", "inttypes.h", "iso646.h",
            "limits.h", "locale.h", "math.h", "setjmp.h", "signal.h", "stdalign.h", "stdarg.h",
            "stdatomic.h", "stdbit.h", "stdbool.h", "stdckdint.h", "stddef.h", "stdint.h", "stdio.h",
            "stdlib.h", "stdnoreturn.h", "string.h", "tgmath.h", "threads.h", "time.h", "uchar.h",
            "wchar.h", "wctype.h",
            // POSIX / platform headers a C toolchain on Windows (MinGW) ships
            "unistd.h", "strings.h", "fcntl.h", "dirent.h", "malloc.h", "memory.h", "conio.h", "windows.h",
            "process.h", "io.h", "direct.h", "getopt.h", "pthread.h", "libgen.h", "search.h", "utime.h",
            "alloca.h", "sys/types.h", "sys/stat.h", "sys/time.h", "sys/timeb.h", "sys/utime.h"};
        return c.count(h) > 0;
    }

private:
    std::map<std::string, Macro> macros_;
    std::set<std::string> included_, queued_;
    std::vector<fs::path> impls_;
    std::string mainKey_;
    int tu_ = 0;

    struct Cond
    {
        bool active;      // tokens in this branch are kept
        bool taken;       // some branch of this #if has been taken
        bool parentActive;
    };

    [[noreturn]] void fail(const std::string &msg, const Token &t)
    {
        throw Unsupported(msg, t.file < (int)out.files.size() ? out.files[t.file] : "", t.line);
    }

    void processFile(const fs::path path, const std::string &source, int depth)
    {
        if (depth > 64)
            throw Unsupported("#include nested too deeply");
        int fileIdx = (int)out.files.size();
        out.files.push_back(path.string());
        Lexer lx(source, fileIdx);
        std::vector<Token> toks = lx.run();
        std::vector<Cond> conds;
        auto active = [&]
        { return conds.empty() || conds.back().active; };
        std::vector<Token> chunk;
        auto flush = [&]
        {
            if (chunk.empty())
                return;
            std::set<std::string> disabled;
            std::vector<Token> expanded;
            expand(chunk, 0, chunk.size(), expanded, disabled);
            for (auto &t : expanded)
            {
                t.tu = tu_;
                out.tokens.push_back(std::move(t));
            }
            chunk.clear();
        };
        size_t i = 0;
        while (i < toks.size() && toks[i].kind != TK::End)
        {
            const Token &t = toks[i];
            if (t.bol && t.kind == TK::Punct && t.text == "#")
            {
                size_t j = i + 1;
                while (j < toks.size() && !toks[j].bol)
                    ++j;
                std::vector<Token> dir(toks.begin() + i + 1, toks.begin() + j);
                i = j;
                flush();
                directive(dir, path, conds, active(), depth, t);
                continue;
            }
            if (active())
                chunk.push_back(t);
            ++i;
        }
        flush();
    }

    void directive(const std::vector<Token> &d, const fs::path &path, std::vector<Cond> &conds, bool active,
                   int depth, const Token &hash)
    {
        if (d.empty())
            return;
        const std::string &name = d[0].text;
        if (name == "if" || name == "ifdef" || name == "ifndef")
        {
            bool v = false;
            if (active)
            {
                if (name == "if")
                    v = evalCondition(d, 1) != 0;
                else
                {
                    bool defined = d.size() > 1 && macros_.count(d[1].text);
                    v = name == "ifdef" ? defined : !defined;
                }
            }
            conds.push_back({active && v, v, active});
            return;
        }
        if (name == "elif")
        {
            if (conds.empty())
                return;
            Cond &c = conds.back();
            if (c.taken || !c.parentActive)
                c.active = false;
            else
            {
                bool v = evalCondition(d, 1) != 0;
                c.active = v;
                c.taken = v;
            }
            return;
        }
        if (name == "else")
        {
            if (conds.empty())
                return;
            Cond &c = conds.back();
            c.active = c.parentActive && !c.taken;
            c.taken = true;
            return;
        }
        if (name == "endif")
        {
            if (!conds.empty())
                conds.pop_back();
            return;
        }
        if (!active)
            return;
        if (name == "define")
        {
            if (d.size() < 2)
                return;
            Macro m;
            size_t k = 2;
            // Function-like only when '(' touches the name.
            if (d.size() > 2 && d[2].text == "(" && !d[2].spaceBefore)
            {
                m.func = true;
                k = 3;
                while (k < d.size() && d[k].text != ")")
                {
                    if (d[k].text == "...")
                    {
                        m.variadic = true;
                        m.params.push_back("__VA_ARGS__");
                    }
                    else if (d[k].kind == TK::Ident)
                    {
                        m.params.push_back(d[k].text);
                        if (k + 1 < d.size() && d[k + 1].text == "...")
                        {
                            m.variadic = true;
                            ++k;
                        }
                    }
                    ++k;
                }
                ++k; // ')'
            }
            for (; k < d.size(); ++k)
                m.body.push_back(d[k]);
            macros_[d[1].text] = m;
            return;
        }
        if (name == "undef")
        {
            if (d.size() > 1)
                macros_.erase(d[1].text);
            return;
        }
        if (name == "include")
        {
            if (d.size() < 2)
                return;
            if (d[1].kind == TK::String)
            {
                fs::path target = cppResolveInclude(d[1].value, path);
                if (target.empty())
                    return; // not part of this project (a precompiled header, ...)
                std::string key = pathKey(target);
                if (!included_.count(key))
                {
                    included_.insert(key);
                    processFile(target, readFile(target), depth + 1);
                }
                fs::path impl = cppImplementationOf(target);
                if (!impl.empty())
                {
                    std::string ik = pathKey(impl);
                    if (!queued_.count(ik))
                    {
                        queued_.insert(ik);
                        impls_.push_back(impl);
                    }
                }
            }
            // <system> headers: the library is built in. Remember which ones
            // the program asked for; a C program only has C's.
            std::string header;
            size_t k = 1;
            if (d[1].kind == TK::Punct && d[1].text == "<")
                for (k = 2; k < d.size() && !(d[k].kind == TK::Punct && d[k].text == ">"); ++k)
                    header += d[k].text;
            if (!header.empty())
            {
                out.systemHeaders.insert(header);
                if (cMode_ && !isCStandardHeader(header))
                {
                    Unsupported u("fatal: " + header + ": No such file or directory",
                                  d[1].file < (int)out.files.size() ? out.files[d[1].file] : "", d[1].line);
                    u.col = d[1].col;
                    u.syntax = true;
                    throw u;
                }
            }
            return;
        }
        if (name == "error")
            fail("#error in source", hash);
        // pragma, line, warning, ident: nothing to do
    }

    // ─── Macro expansion ───────────────────────────────────────────────────

    static Token makeInt(long long v, const Token &at)
    {
        Token t = at;
        t.kind = TK::Int;
        t.text = std::to_string(v);
        t.value.clear();
        return t;
    }

    void expand(const std::vector<Token> &in, size_t from, size_t to, std::vector<Token> &out,
                std::set<std::string> &disabled)
    {
        for (size_t i = from; i < to; ++i)
        {
            const Token &t = in[i];
            if (t.kind == TK::Ident)
            {
                if (t.text == "__LINE__")
                {
                    out.push_back(makeInt(t.line, t));
                    continue;
                }
                if (t.text == "__FILE__")
                {
                    Token s = t;
                    s.kind = TK::String;
                    s.value = t.file < (int)this->out.files.size() ? this->out.files[t.file] : "";
                    s.text = "\"" + s.value + "\"";
                    out.push_back(s);
                    continue;
                }
                auto it = macros_.find(t.text);
                if (it != macros_.end() && !disabled.count(t.text))
                {
                    const Macro &m = it->second;
                    if (!m.func)
                    {
                        std::vector<Token> body = m.body;
                        for (auto &b : body)
                        {
                            b.line = t.line;
                            b.file = t.file;
                            b.bol = false;
                        }
                        if (!body.empty())
                            body[0].spaceBefore = t.spaceBefore;
                        disabled.insert(t.text);
                        expand(body, 0, body.size(), out, disabled);
                        disabled.erase(t.text);
                        continue;
                    }
                    if (i + 1 < to && in[i + 1].kind == TK::Punct && in[i + 1].text == "(")
                    {
                        std::vector<std::vector<Token>> args(1);
                        int depth = 0;
                        size_t k = i + 2;
                        for (; k < to; ++k)
                        {
                            const Token &a = in[k];
                            if (a.kind == TK::Punct && (a.text == "(" || a.text == "[" || a.text == "{"))
                                ++depth;
                            else if (a.kind == TK::Punct && (a.text == "]" || a.text == "}"))
                                --depth;
                            else if (a.kind == TK::Punct && a.text == ")")
                            {
                                if (depth == 0)
                                    break;
                                --depth;
                            }
                            if (depth == 0 && a.kind == TK::Punct && a.text == "," &&
                                !(m.variadic && args.size() >= m.params.size()))
                            {
                                args.emplace_back();
                                continue;
                            }
                            args.back().push_back(a);
                        }
                        if (m.params.empty() && args.size() == 1 && args[0].empty())
                            args.clear();
                        std::vector<Token> body = substitute(m, args, t, disabled);
                        disabled.insert(t.text);
                        expand(body, 0, body.size(), out, disabled);
                        disabled.erase(t.text);
                        i = k; // the ')'
                        continue;
                    }
                }
            }
            out.push_back(t);
        }
    }

    std::vector<Token> substitute(const Macro &m, const std::vector<std::vector<Token>> &args, const Token &at,
                                  std::set<std::string> &disabled)
    {
        auto paramIndex = [&](const Token &t) -> int
        {
            if (t.kind != TK::Ident)
                return -1;
            for (size_t p = 0; p < m.params.size(); ++p)
                if (m.params[p] == t.text)
                    return (int)p;
            return -1;
        };
        auto argOf = [&](int p) -> std::vector<Token>
        {
            if (m.variadic && p == (int)m.params.size() - 1)
            {
                std::vector<Token> all;
                for (size_t a = p; a < args.size(); ++a)
                {
                    if (a > (size_t)p)
                    {
                        Token comma = at;
                        comma.kind = TK::Punct;
                        comma.text = ",";
                        all.push_back(comma);
                    }
                    all.insert(all.end(), args[a].begin(), args[a].end());
                }
                return all;
            }
            return p < (int)args.size() ? args[p] : std::vector<Token>();
        };
        std::vector<Token> res;
        const auto &b = m.body;
        for (size_t j = 0; j < b.size(); ++j)
        {
            const Token &t = b[j];
            if (t.kind == TK::Punct && t.text == "#" && j + 1 < b.size() && paramIndex(b[j + 1]) >= 0)
            {
                std::string s;
                for (const Token &a : argOf(paramIndex(b[j + 1])))
                {
                    if (!s.empty() && a.spaceBefore)
                        s += ' ';
                    s += a.kind == TK::String || a.kind == TK::Char ? a.text : a.text;
                }
                Token st = at;
                st.kind = TK::String;
                st.value = s;
                st.text = "\"" + s + "\"";
                st.bol = false;
                st.spaceBefore = t.spaceBefore;
                res.push_back(st);
                ++j;
                continue;
            }
            if (t.kind == TK::Punct && t.text == "##" && !res.empty() && j + 1 < b.size())
            {
                const Token &next = b[j + 1];
                std::string rhs;
                int p = paramIndex(next);
                if (p >= 0)
                    for (const Token &a : argOf(p))
                        rhs += a.text;
                else
                    rhs = next.text;
                Token pasted = res.back();
                res.pop_back();
                Lexer lx(pasted.text + rhs, at.file);
                auto toks = lx.run();
                toks.pop_back();
                for (auto &x : toks)
                {
                    x.line = at.line;
                    x.bol = false;
                    res.push_back(x);
                }
                ++j;
                continue;
            }
            int p = paramIndex(t);
            if (p >= 0)
            {
                bool pasteNext = j + 1 < b.size() && b[j + 1].text == "##";
                std::vector<Token> a = argOf(p);
                if (pasteNext)
                    res.insert(res.end(), a.begin(), a.end());
                else
                {
                    std::vector<Token> ex;
                    expand(a, 0, a.size(), ex, disabled);
                    if (!ex.empty())
                        ex[0].spaceBefore = t.spaceBefore;
                    res.insert(res.end(), ex.begin(), ex.end());
                }
                continue;
            }
            Token c = t;
            c.line = at.line;
            c.file = at.file;
            c.bol = false;
            res.push_back(c);
        }
        return res;
    }

    // ─── #if expressions ───────────────────────────────────────────────────

    long long evalCondition(const std::vector<Token> &d, size_t from)
    {
        std::vector<Token> pre;
        for (size_t k = from; k < d.size(); ++k)
        {
            if (d[k].kind == TK::Ident && d[k].text == "defined")
            {
                bool paren = k + 1 < d.size() && d[k + 1].text == "(";
                size_t n = paren ? k + 2 : k + 1;
                bool def = n < d.size() && macros_.count(d[n].text);
                pre.push_back(makeInt(def ? 1 : 0, d[k]));
                k = paren ? n + 1 : n;
                continue;
            }
            pre.push_back(d[k]);
        }
        std::vector<Token> ex;
        std::set<std::string> disabled;
        expand(pre, 0, pre.size(), ex, disabled);
        size_t pos = 0;
        std::function<long long(int)> parseBin;
        std::function<long long()> parseUnary = [&]() -> long long
        {
            if (pos >= ex.size())
                return 0;
            const Token &t = ex[pos];
            if (t.kind == TK::Punct)
            {
                if (t.text == "(")
                {
                    ++pos;
                    long long v = parseBin(0);
                    if (pos < ex.size() && ex[pos].text == ")")
                        ++pos;
                    return v;
                }
                if (t.text == "!")
                {
                    ++pos;
                    return !parseUnary();
                }
                if (t.text == "-")
                {
                    ++pos;
                    return -parseUnary();
                }
                if (t.text == "+")
                {
                    ++pos;
                    return parseUnary();
                }
                if (t.text == "~")
                {
                    ++pos;
                    return ~parseUnary();
                }
            }
            ++pos;
            if (t.kind == TK::Int)
            {
                std::string s = t.text;
                while (!s.empty() && std::strchr("uUlL", s.back()))
                    s.pop_back();
                try
                {
                    return std::stoll(s, nullptr, 0);
                }
                catch (...)
                {
                    return 0;
                }
            }
            if (t.kind == TK::Char)
                return t.value.empty() ? 0 : (unsigned char)t.value[0];
            if (t.kind == TK::Ident && t.text == "true")
                return 1;
            return 0; // unknown identifier
        };
        auto prec = [](const std::string &op) -> int
        {
            if (op == "*" || op == "/" || op == "%") return 10;
            if (op == "+" || op == "-") return 9;
            if (op == "<<" || op == ">>") return 8;
            if (op == "<" || op == "<=" || op == ">" || op == ">=") return 7;
            if (op == "==" || op == "!=") return 6;
            if (op == "&") return 5;
            if (op == "^") return 4;
            if (op == "|") return 3;
            if (op == "&&") return 2;
            if (op == "||") return 1;
            return -1;
        };
        parseBin = [&](int minPrec) -> long long
        {
            long long l = parseUnary();
            while (pos < ex.size())
            {
                const std::string &op = ex[pos].text;
                if (ex[pos].kind == TK::Punct && op == "?" && minPrec == 0)
                {
                    ++pos;
                    long long a = parseBin(0);
                    if (pos < ex.size() && ex[pos].text == ":")
                        ++pos;
                    long long b = parseBin(0);
                    l = l ? a : b;
                    continue;
                }
                int p = ex[pos].kind == TK::Punct ? prec(op) : -1;
                if (p < 0 || p < minPrec)
                    break;
                ++pos;
                long long r = parseBin(p + 1);
                if (op == "*") l *= r;
                else if (op == "/") l = r ? l / r : 0;
                else if (op == "%") l = r ? l % r : 0;
                else if (op == "+") l += r;
                else if (op == "-") l -= r;
                else if (op == "<<") l <<= r;
                else if (op == ">>") l >>= r;
                else if (op == "<") l = l < r;
                else if (op == "<=") l = l <= r;
                else if (op == ">") l = l > r;
                else if (op == ">=") l = l >= r;
                else if (op == "==") l = l == r;
                else if (op == "!=") l = l != r;
                else if (op == "&") l &= r;
                else if (op == "^") l ^= r;
                else if (op == "|") l |= r;
                else if (op == "&&") l = l && r;
                else if (op == "||") l = l || r;
            }
            return l;
        };
        return parseBin(0);
    }
};

} // namespace

Preprocessed preprocess(const std::string &path, const std::string &source)
{
    Preprocessor pp;
    pp.cMode_ = fileExtLower(path) == ".c";
    std::string src = source;
    if (src.size() >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF)
        src.erase(0, 3);
    pp.runMain(fs::u8path(path), src);
    return std::move(pp.out);
}

} // namespace cpp
