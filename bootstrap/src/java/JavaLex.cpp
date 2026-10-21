// Java lexical structure (JLS §3): identifiers, keywords, literals (with
// javac's range checks), text blocks, comments and operators.
#include "JavaAst.h"

#include <cmath>
#include <cstdlib>
#include <set>

namespace java
{

namespace
{

const std::set<std::string> &keywords()
{
    static const std::set<std::string> k = {
        "abstract", "assert", "boolean", "break", "byte", "case", "catch", "char", "class", "const",
        "continue", "default", "do", "double", "else", "enum", "extends", "final", "finally", "float",
        "for", "goto", "if", "implements", "import", "instanceof", "int", "interface", "long", "native",
        "new", "package", "private", "protected", "public", "return", "short", "static", "strictfp",
        "super", "switch", "synchronized", "this", "throw", "throws", "transient", "try", "void",
        "volatile", "while", "true", "false", "null", "_"};
    return k;
}

void appendUtf8(std::string &out, uint32_t cp)
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

// UTF-16 code units from string/char literal escapes become UTF-8; a
// surrogate pair is joined into one code point first.
struct Utf16Sink
{
    std::string out;
    uint32_t high = 0;
    void unit(uint32_t u)
    {
        if (high)
        {
            if (u >= 0xDC00 && u <= 0xDFFF)
            {
                appendUtf8(out, 0x10000 + ((high - 0xD800) << 10) + (u - 0xDC00));
                high = 0;
                return;
            }
            appendUtf8(out, high);
            high = 0;
        }
        if (u >= 0xD800 && u <= 0xDBFF)
            high = u;
        else
            appendUtf8(out, u);
    }
    void raw(const std::string &bytes)
    {
        flush();
        out += bytes;
    }
    void flush()
    {
        if (high)
        {
            appendUtf8(out, high);
            high = 0;
        }
    }
};

class Lexer
{
public:
    explicit Lexer(const std::string &s) : s_(s) {}

    std::vector<Token> run()
    {
        std::vector<Token> out;
        while (true)
        {
            skipSpaceAndComments();
            Token t;
            t.pos = here();
            if (i_ >= s_.size())
            {
                t.kind = Tok::End;
                t.end = t.pos;
                out.push_back(t);
                break;
            }
            lexOne(t);
            t.end = here();
            out.push_back(std::move(t));
        }
        for (size_t k = 0; k + 1 < out.size(); ++k)
            out[k].adjNext = out[k].end.line == out[k + 1].pos.line && out[k].end.col == out[k + 1].pos.col;
        return out;
    }

private:
    const std::string &s_;
    size_t i_ = 0;
    int line_ = 1;
    size_t lineStart_ = 0;

    Pos here() const { return Pos{line_, (int)(i_ - lineStart_) + 1}; }
    char peek(size_t k = 0) const { return i_ + k < s_.size() ? s_[i_ + k] : '\0'; }
    [[noreturn]] void fail(const std::string &msg, Pos p) const { throw SyntaxError{msg, p}; }
    [[noreturn]] void fail(const std::string &msg) const { throw SyntaxError{msg, here()}; }

    void newline()
    {
        // consumes one line terminator (\n, \r\n or \r)
        if (s_[i_] == '\r' && peek(1) == '\n')
            ++i_;
        ++i_;
        ++line_;
        lineStart_ = i_;
    }

    void skipSpaceAndComments()
    {
        while (i_ < s_.size())
        {
            char c = s_[i_];
            if (c == '\n' || c == '\r')
                newline();
            else if (c == ' ' || c == '\t' || c == '\f')
                ++i_;
            else if (c == '/' && peek(1) == '/')
            {
                while (i_ < s_.size() && s_[i_] != '\n' && s_[i_] != '\r')
                    ++i_;
            }
            else if (c == '/' && peek(1) == '*')
            {
                Pos start = here();
                i_ += 2;
                while (true)
                {
                    if (i_ >= s_.size())
                        fail("unclosed comment", start);
                    if (s_[i_] == '*' && peek(1) == '/')
                    {
                        i_ += 2;
                        break;
                    }
                    if (s_[i_] == '\n' || s_[i_] == '\r')
                        newline();
                    else
                        ++i_;
                }
            }
            else if ((unsigned char)c == 0xEF && (unsigned char)peek(1) == 0xBB && (unsigned char)peek(2) == 0xBF)
                i_ += 3; // byte-order mark
            else
                break;
        }
    }

    static bool identStart(unsigned char c) { return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80; }
    static bool identPart(unsigned char c) { return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

    void lexOne(Token &t)
    {
        unsigned char c = (unsigned char)s_[i_];
        if (identStart(c))
        {
            size_t start = i_;
            while (i_ < s_.size() && identPart((unsigned char)s_[i_]))
                ++i_;
            t.text = s_.substr(start, i_ - start);
            t.kind = keywords().count(t.text) ? Tok::Keyword : Tok::Ident;
            return;
        }
        if (std::isdigit(c) || (c == '.' && std::isdigit((unsigned char)peek(1))))
        {
            number(t);
            return;
        }
        if (c == '"')
        {
            if (peek(1) == '"' && peek(2) == '"')
                textBlock(t);
            else
                stringLit(t);
            return;
        }
        if (c == '\'')
        {
            charLit(t);
            return;
        }
        op(t);
    }

    // ── numbers ──

    void number(Token &t)
    {
        size_t start = i_;
        Pos p = here();
        auto digitsWithUnderscores = [&](auto isDigit) -> std::string
        {
            std::string d;
            bool lastUnderscore = false;
            bool any = false;
            while (i_ < s_.size() && (isDigit((unsigned char)s_[i_]) || s_[i_] == '_'))
            {
                if (s_[i_] == '_')
                {
                    if (!any)
                        fail("illegal underscore");
                    lastUnderscore = true;
                }
                else
                {
                    d += s_[i_];
                    lastUnderscore = false;
                    any = true;
                }
                ++i_;
            }
            if (lastUnderscore)
                fail("illegal underscore");
            return d;
        };
        auto isDec = [](unsigned char ch) { return std::isdigit(ch) != 0; };
        auto isHex = [](unsigned char ch) { return std::isxdigit(ch) != 0; };
        auto isBin = [](unsigned char ch) { return ch == '0' || ch == '1'; };

        if (s_[i_] == '0' && (peek(1) == 'x' || peek(1) == 'X'))
        {
            i_ += 2;
            std::string d = digitsWithUnderscores(isHex);
            if (peek() == '.' || peek() == 'p' || peek() == 'P')
            {
                // hexadecimal floating point: 0x1.8p3
                std::string frac;
                if (peek() == '.')
                {
                    ++i_;
                    frac = digitsWithUnderscores(isHex);
                }
                if (peek() != 'p' && peek() != 'P')
                    fail("malformed floating-point literal", p);
                ++i_;
                std::string ex;
                if (peek() == '+' || peek() == '-')
                    ex += s_[i_++];
                ex += digitsWithUnderscores(isDec);
                std::string lit = "0x" + (d.empty() ? std::string("0") : d) + "." + frac + "p" + ex;
                t.dval = std::strtod(lit.c_str(), nullptr);
                t.kind = Tok::DoubleLit;
                if (peek() == 'f' || peek() == 'F')
                {
                    ++i_;
                    t.kind = Tok::FloatLit;
                }
                else if (peek() == 'd' || peek() == 'D')
                    ++i_;
                t.text = s_.substr(start, i_ - start);
                return;
            }
            if (d.empty())
                fail("hexadecimal numbers must contain at least one hexadecimal digit", p);
            integer(t, d, 16, start, p);
            return;
        }
        if (s_[i_] == '0' && (peek(1) == 'b' || peek(1) == 'B'))
        {
            i_ += 2;
            std::string d = digitsWithUnderscores(isBin);
            if (d.empty())
                fail("binary numbers must contain at least one binary digit", p);
            if (std::isdigit((unsigned char)peek()))
                fail("illegal digit in a binary literal");
            integer(t, d, 2, start, p);
            return;
        }
        std::string intPart = s_[i_] == '.' ? "" : digitsWithUnderscores(isDec);
        bool isFloat = false;
        std::string frac, ex;
        if (peek() == '.' && std::isdigit((unsigned char)peek(1)))
        {
            ++i_;
            frac = digitsWithUnderscores(isDec);
            isFloat = true;
        }
        else if (peek() == '.' && !identStart((unsigned char)peek(1)) && peek(1) != '.')
        {
            ++i_; // `1.` is a double
            isFloat = true;
        }
        if (peek() == 'e' || peek() == 'E')
        {
            size_t save = i_;
            ++i_;
            if (peek() == '+' || peek() == '-')
                ex += s_[i_++];
            if (!std::isdigit((unsigned char)peek()))
            {
                i_ = save;
                fail("malformed floating-point literal", p);
            }
            ex += digitsWithUnderscores(isDec);
            isFloat = true;
        }
        char suffix = peek();
        if (suffix == 'f' || suffix == 'F' || suffix == 'd' || suffix == 'D')
        {
            ++i_;
            isFloat = true;
        }
        if (isFloat)
        {
            std::string lit = (intPart.empty() ? "0" : intPart) + "." + (frac.empty() ? "0" : frac) +
                              (ex.empty() ? "" : "e" + ex);
            double v = std::strtod(lit.c_str(), nullptr);
            bool isF = suffix == 'f' || suffix == 'F';
            bool nonzero = false;
            for (char ch : intPart + frac)
                nonzero = nonzero || (ch != '0');
            if (isF)
            {
                float fv = (float)v;
                if (std::isinf(fv))
                    fail("floating-point number too large", p);
                if (fv == 0 && nonzero)
                    fail("floating-point number too small", p);
                t.dval = fv;
                t.kind = Tok::FloatLit;
            }
            else
            {
                if (std::isinf(v))
                    fail("floating-point number too large", p);
                if (v == 0 && nonzero)
                    fail("floating-point number too small", p);
                t.dval = v;
                t.kind = Tok::DoubleLit;
            }
            t.text = s_.substr(start, i_ - start);
            if (identPart((unsigned char)peek()))
                fail("';' expected", here());
            return;
        }
        if (intPart.size() > 1 && intPart[0] == '0')
        {
            for (char ch : intPart)
                if (ch > '7')
                    fail("integer number too large", p); // javac: an 8/9 in an octal literal
            integer(t, intPart.substr(1), 8, start, p);
            return;
        }
        integer(t, intPart, 10, start, p);
    }

    void integer(Token &t, const std::string &digits, int base, size_t start, Pos p)
    {
        bool isLong = peek() == 'l' || peek() == 'L';
        if (isLong)
            ++i_;
        t.text = s_.substr(start, i_ - start);
        t.kind = isLong ? Tok::LongLit : Tok::IntLit;
        if (identPart((unsigned char)peek()))
            fail("';' expected", here());
        // magnitude with overflow detection (64-bit)
        unsigned __int128 v = 0;
        bool overflow = false;
        for (char ch : digits)
        {
            int d = std::isdigit((unsigned char)ch) ? ch - '0' : std::tolower((unsigned char)ch) - 'a' + 10;
            v = v * base + d;
            if (v > (unsigned __int128)UINT64_MAX)
                overflow = true;
        }
        if (overflow)
        {
            t.tooLarge = true;
            return;
        }
        uint64_t m = (uint64_t)v;
        t.ival = m;
        if (base == 10)
        {
            // 2147483648 / 9223372036854775808 are legal only after unary minus
            if (!isLong && m > 2147483648ull)
                t.tooLarge = true;
            if (isLong && m > 9223372036854775808ull)
                t.tooLarge = true;
        }
        else
        {
            if (!isLong && m > 0xFFFFFFFFull)
                t.tooLarge = true;
        }
        (void)p;
    }

    // ── characters and strings ──

    // One escape sequence after the backslash at s_[i_]; appends to `sink`.
    void escape(Utf16Sink &sink, bool inTextBlock)
    {
        Pos p = here();
        ++i_; // backslash
        if (i_ >= s_.size())
            fail("illegal escape character", p);
        char c = s_[i_];
        switch (c)
        {
        case 'b': sink.unit(8); ++i_; return;
        case 't': sink.unit(9); ++i_; return;
        case 'n': sink.unit(10); ++i_; return;
        case 'f': sink.unit(12); ++i_; return;
        case 'r': sink.unit(13); ++i_; return;
        case 's': sink.unit(32); ++i_; return;
        case '"': sink.unit('"'); ++i_; return;
        case '\'': sink.unit('\''); ++i_; return;
        case '\\': sink.unit('\\'); ++i_; return;
        case 'u':
        {
            while (i_ < s_.size() && s_[i_] == 'u')
                ++i_;
            uint32_t v = 0;
            for (int k = 0; k < 4; ++k)
            {
                if (i_ >= s_.size() || !std::isxdigit((unsigned char)s_[i_]))
                    fail("illegal unicode escape", p);
                char h = s_[i_++];
                v = v * 16 + (std::isdigit((unsigned char)h) ? h - '0' : std::tolower((unsigned char)h) - 'a' + 10);
            }
            sink.unit(v);
            return;
        }
        default:
            break;
        }
        if (c >= '0' && c <= '7')
        {
            int maxDigits = c <= '3' ? 3 : 2;
            uint32_t v = 0;
            for (int k = 0; k < maxDigits && i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '7'; ++k)
                v = v * 8 + (s_[i_++] - '0');
            sink.unit(v);
            return;
        }
        if (inTextBlock && (c == '\n' || c == '\r'))
        {
            newline(); // \<line terminator>: no newline in the value
            return;
        }
        fail("illegal escape character", here());
    }

    // The next source character as UTF-8 bytes (one code point).
    std::string codePointBytes()
    {
        unsigned char c = (unsigned char)s_[i_];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        std::string b = s_.substr(i_, n);
        i_ += n;
        return b;
    }

    static uint32_t decodeUtf8(const std::string &b)
    {
        unsigned char c = (unsigned char)b[0];
        if (c < 0x80 || b.size() == 1)
            return c;
        if (b.size() == 2)
            return ((c & 0x1F) << 6) | (b[1] & 0x3F);
        if (b.size() == 3)
            return ((c & 0x0F) << 12) | ((b[1] & 0x3F) << 6) | (b[2] & 0x3F);
        return ((c & 0x07) << 18) | ((b[1] & 0x3F) << 12) | ((b[2] & 0x3F) << 6) | (b[3] & 0x3F);
    }

    void charLit(Token &t)
    {
        Pos p = here();
        size_t start = i_;
        ++i_;
        if (peek() == '\'')
            fail("empty character literal", p);
        if (peek() == '\n' || peek() == '\r' || i_ >= s_.size())
            fail("illegal line end in character literal", p);
        Utf16Sink sink;
        uint32_t unit;
        if (peek() == '\\')
        {
            // decode the escape to a code unit directly
            Utf16Sink one;
            escape(one, false);
            one.flush();
            unit = decodeUtf8(one.out);
        }
        else
        {
            std::string b = codePointBytes();
            unit = decodeUtf8(b);
            if (unit > 0xFFFF)
                fail("unclosed character literal", p);
        }
        if (peek() != '\'')
            fail("unclosed character literal", p);
        ++i_;
        t.kind = Tok::CharLit;
        t.ch = unit;
        t.text = s_.substr(start, i_ - start);
    }

    void stringLit(Token &t)
    {
        Pos p = here();
        size_t start = i_;
        ++i_;
        Utf16Sink sink;
        while (true)
        {
            if (i_ >= s_.size() || s_[i_] == '\n' || s_[i_] == '\r')
                fail("unclosed string literal", p);
            char c = s_[i_];
            if (c == '"')
            {
                ++i_;
                break;
            }
            if (c == '\\')
                escape(sink, false);
            else
                sink.raw(codePointBytes());
        }
        sink.flush();
        t.kind = Tok::StringLit;
        t.str = sink.out;
        t.text = s_.substr(start, i_ - start);
    }

    // """ ... """ (JLS §3.10.6): line terminators normalized to \n, then
    // incidental indentation and trailing spaces stripped, then escapes
    // interpreted (so \<newline>, \s and \040 survive the stripping).
    void textBlock(Token &t)
    {
        Pos p = here();
        size_t start = i_;
        i_ += 3;
        while (peek() == ' ' || peek() == '\t' || peek() == '\f')
            ++i_;
        if (peek() != '\n' && peek() != '\r')
            fail("illegal text block open delimiter sequence, missing line terminator", here());
        newline();
        std::string raw; // escapes still unprocessed
        while (true)
        {
            if (i_ >= s_.size())
                fail("unclosed text block", p);
            char c = s_[i_];
            if (c == '"' && peek(1) == '"' && peek(2) == '"')
            {
                i_ += 3;
                break;
            }
            if (c == '\n' || c == '\r')
            {
                raw += '\n';
                newline();
                continue;
            }
            raw += c;
            ++i_;
            if (c == '\\' && i_ < s_.size())
            {
                if (s_[i_] == '\n' || s_[i_] == '\r')
                {
                    raw += '\n';
                    newline();
                }
                else
                    raw += s_[i_++];
            }
        }
        std::vector<std::string> lines;
        size_t from = 0;
        while (true)
        {
            size_t nl = raw.find('\n', from);
            lines.push_back(raw.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
            if (nl == std::string::npos)
                break;
            from = nl + 1;
        }
        auto isWs = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\f'; };
        auto blank = [&](const std::string &l)
        {
            for (char ch : l)
                if (!isWs(ch))
                    return false;
            return true;
        };
        auto indentOf = [&](const std::string &l)
        {
            size_t n = 0;
            while (n < l.size() && isWs(l[n]))
                ++n;
            return n;
        };
        const size_t last = lines.size() - 1;
        const bool closingOwnLine = blank(lines[last]);
        size_t minIndent = std::string::npos;
        for (size_t k = 0; k <= last; ++k)
        {
            if (k == last && closingOwnLine)
                minIndent = std::min(minIndent, lines[k].size());
            else if (!blank(lines[k]))
                minIndent = std::min(minIndent, indentOf(lines[k]));
        }
        if (minIndent == std::string::npos)
            minIndent = 0;
        std::string joined;
        for (size_t k = 0; k <= last; ++k)
        {
            if (k == last && closingOwnLine)
                break;
            std::string l = blank(lines[k]) ? "" : lines[k].substr(std::min(minIndent, lines[k].size()));
            while (!l.empty() && isWs(l.back()))
                l.pop_back();
            joined += l;
            if (k < last)
                joined += '\n';
        }
        Utf16Sink sink;
        for (size_t k = 0; k < joined.size();)
        {
            if (joined[k] == '\\' && k + 1 < joined.size())
            {
                char e = joined[k + 1];
                k += 2;
                switch (e)
                {
                case 'n': sink.unit(10); break;
                case 't': sink.unit(9); break;
                case 'b': sink.unit(8); break;
                case 'f': sink.unit(12); break;
                case 'r': sink.unit(13); break;
                case 's': sink.unit(32); break;
                case '"': sink.unit('"'); break;
                case '\'': sink.unit('\''); break;
                case '\\': sink.unit('\\'); break;
                case '\n': break; // line continuation
                default:
                    if (e >= '0' && e <= '7')
                    {
                        uint32_t v = e - '0';
                        int more = e <= '3' ? 2 : 1;
                        while (more-- > 0 && k < joined.size() && joined[k] >= '0' && joined[k] <= '7')
                            v = v * 8 + (joined[k++] - '0');
                        sink.unit(v);
                    }
                    else
                        fail("illegal escape character", p);
                }
                continue;
            }
            unsigned char c = (unsigned char)joined[k];
            size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
            sink.raw(joined.substr(k, n));
            k += n;
        }
        sink.flush();
        t.kind = Tok::StringLit;
        t.str = sink.out;
        t.text = s_.substr(start, i_ - start);
    }

    // ── operators ──

    void op(Token &t)
    {
        static const char *ops[] = {
            ">>>=", "<<=", "...", "->", "::", "++", "--", "&&", "||", "==", "!=", "<=", "+=", "-=", "*=",
            "/=", "%=", "&=", "|=", "^=", "<<", "(", ")", "{", "}", "[", "]", ";", ",", ".", "@", "=", ">",
            "<", "!", "~", "?", ":", "+", "-", "*", "/", "&", "|", "^", "%"};
        // '>' never combines here: the parser joins adjacent '>' tokens into
        // >> >>> >= >>= >>>= where an operator (not a closing '>') is meant.
        if (s_[i_] == '>')
        {
            t.kind = Tok::Op;
            t.text = ">";
            ++i_;
            return;
        }
        for (const char *o : ops)
        {
            size_t n = std::char_traits<char>::length(o);
            if (n > 1 && o[0] == '>')
                continue;
            if (s_.compare(i_, n, o) == 0)
            {
                t.kind = Tok::Op;
                t.text = o;
                i_ += n;
                return;
            }
        }
        unsigned char c = (unsigned char)s_[i_];
        std::string shown = c >= 0x20 && c < 0x7F ? std::string(1, (char)c) : "\\u" + [&] {
            char b[8];
            std::snprintf(b, sizeof b, "%04x", c);
            return std::string(b);
        }();
        fail("illegal character: '" + shown + "'");
    }
};

} // namespace

std::vector<Token> lex(const std::string &source)
{
    Lexer lx(source);
    return lx.run();
}

} // namespace java
