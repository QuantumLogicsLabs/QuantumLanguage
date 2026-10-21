#include "QpmJson.h"
#include <cctype>
#include <cmath>
#include <sstream>

namespace qpm
{
    namespace
    {
        class Parser
        {
        public:
            explicit Parser(const std::string &text) : s_(text) {}

            JsonValue parseDocument()
            {
                skipWs();
                JsonValue v = parseValue();
                skipWs();
                if (pos_ != s_.size())
                    fail("trailing data after JSON value");
                return v;
            }

        private:
            const std::string &s_;
            size_t pos_ = 0;

            [[noreturn]] void fail(const std::string &msg)
            {
                size_t line = 1, col = 1;
                for (size_t i = 0; i < pos_ && i < s_.size(); ++i)
                {
                    if (s_[i] == '\n') { ++line; col = 1; }
                    else ++col;
                }
                std::ostringstream oss;
                oss << "JSON parse error at line " << line << " col " << col << ": " << msg;
                throw std::runtime_error(oss.str());
            }

            char peek() { return pos_ < s_.size() ? s_[pos_] : '\0'; }
            char next() { return pos_ < s_.size() ? s_[pos_++] : '\0'; }

            void skipWs()
            {
                while (pos_ < s_.size())
                {
                    char c = s_[pos_];
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                        ++pos_;
                    else
                        break;
                }
            }

            bool consumeLiteral(const char *lit)
            {
                size_t len = std::char_traits<char>::length(lit);
                if (s_.compare(pos_, len, lit) == 0)
                {
                    pos_ += len;
                    return true;
                }
                return false;
            }

            JsonValue parseValue()
            {
                skipWs();
                if (pos_ >= s_.size())
                    fail("unexpected end of input");
                char c = peek();
                if (c == '{') return parseObject();
                if (c == '[') return parseArray();
                if (c == '"') return JsonValue(parseString());
                if (c == 't') { if (consumeLiteral("true")) return JsonValue(true); fail("invalid literal"); }
                if (c == 'f') { if (consumeLiteral("false")) return JsonValue(false); fail("invalid literal"); }
                if (c == 'n') { if (consumeLiteral("null")) return JsonValue(nullptr); fail("invalid literal"); }
                if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
                fail(std::string("unexpected character '") + c + "'");
            }

            JsonValue parseObject()
            {
                JsonValue obj = JsonValue::makeObject();
                ++pos_; // '{'
                skipWs();
                if (peek() == '}') { ++pos_; return obj; }
                while (true)
                {
                    skipWs();
                    if (peek() != '"')
                        fail("expected string key");
                    std::string key = parseString();
                    skipWs();
                    if (next() != ':')
                        fail("expected ':' after object key");
                    JsonValue val = parseValue();
                    obj.object().emplace_back(std::move(key), std::move(val));
                    skipWs();
                    char c = next();
                    if (c == ',') continue;
                    if (c == '}') break;
                    fail("expected ',' or '}' in object");
                }
                return obj;
            }

            JsonValue parseArray()
            {
                JsonValue arr = JsonValue::makeArray();
                ++pos_; // '['
                skipWs();
                if (peek() == ']') { ++pos_; return arr; }
                while (true)
                {
                    JsonValue val = parseValue();
                    arr.array().push_back(std::move(val));
                    skipWs();
                    char c = next();
                    if (c == ',') continue;
                    if (c == ']') break;
                    fail("expected ',' or ']' in array");
                }
                return arr;
            }

            std::string parseString()
            {
                ++pos_; // opening quote
                std::string out;
                while (true)
                {
                    if (pos_ >= s_.size())
                        fail("unterminated string");
                    char c = s_[pos_++];
                    if (c == '"')
                        break;
                    if (c == '\\')
                    {
                        if (pos_ >= s_.size())
                            fail("unterminated escape");
                        char e = s_[pos_++];
                        switch (e)
                        {
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        case '/': out += '/'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break;
                        case 't': out += '\t'; break;
                        case 'u':
                        {
                            unsigned cp = parseHex4();
                            // Basic UTF-16 -> UTF-8. Handles surrogate pairs.
                            if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size() &&
                                s_[pos_] == '\\' && s_[pos_ + 1] == 'u')
                            {
                                pos_ += 2;
                                unsigned lo = parseHex4();
                                if (lo >= 0xDC00 && lo <= 0xDFFF)
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                            appendUtf8(out, cp);
                            break;
                        }
                        default:
                            fail("invalid escape sequence");
                        }
                    }
                    else
                    {
                        out += c;
                    }
                }
                return out;
            }

            unsigned parseHex4()
            {
                if (pos_ + 4 > s_.size())
                    fail("truncated \\u escape");
                unsigned v = 0;
                for (int i = 0; i < 4; ++i)
                {
                    char c = s_[pos_++];
                    v <<= 4;
                    if (c >= '0' && c <= '9') v |= (c - '0');
                    else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
                    else fail("invalid hex digit in \\u escape");
                }
                return v;
            }

            static void appendUtf8(std::string &out, unsigned cp)
            {
                if (cp <= 0x7F)
                {
                    out += static_cast<char>(cp);
                }
                else if (cp <= 0x7FF)
                {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                else if (cp <= 0xFFFF)
                {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                else
                {
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
            }

            JsonValue parseNumber()
            {
                size_t start = pos_;
                if (peek() == '-') ++pos_;
                while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
                if (peek() == '.')
                {
                    ++pos_;
                    while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
                }
                if (peek() == 'e' || peek() == 'E')
                {
                    ++pos_;
                    if (peek() == '+' || peek() == '-') ++pos_;
                    while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
                }
                std::string tok = s_.substr(start, pos_ - start);
                if (tok.empty() || tok == "-")
                    fail("invalid number");
                try
                {
                    return JsonValue(std::stod(tok));
                }
                catch (...)
                {
                    fail("invalid number literal");
                }
            }
        };

        void escapeInto(std::string &out, const std::string &s)
        {
            out += '"';
            for (unsigned char c : s)
            {
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    }
                    else
                    {
                        out += static_cast<char>(c);
                    }
                }
            }
            out += '"';
        }

        void formatNumber(std::string &out, double n)
        {
            if (std::isfinite(n) && n == std::floor(n) &&
                std::abs(n) < 1e15)
            {
                out += std::to_string(static_cast<long long>(n));
            }
            else
            {
                std::ostringstream oss;
                oss.precision(17);
                oss << n;
                out += oss.str();
            }
        }
    } // namespace

    JsonValue JsonValue::parse(const std::string &text)
    {
        Parser p(text);
        return p.parseDocument();
    }

    void JsonValue::appendTo(std::string &out, int indent, int depth) const
    {
        auto newline = [&](int d)
        {
            if (indent > 0)
            {
                out += '\n';
                out.append(static_cast<size_t>(indent) * d, ' ');
            }
        };

        switch (type_)
        {
        case Type::Null:
            out += "null";
            break;
        case Type::Bool:
            out += bool_ ? "true" : "false";
            break;
        case Type::Number:
            formatNumber(out, num_);
            break;
        case Type::String:
            escapeInto(out, str_);
            break;
        case Type::Array:
            if (arr_.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i)
            {
                newline(depth + 1);
                arr_[i].appendTo(out, indent, depth + 1);
                if (i + 1 < arr_.size()) out += ',';
            }
            newline(depth);
            out += ']';
            break;
        case Type::Object:
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i)
            {
                newline(depth + 1);
                escapeInto(out, obj_[i].first);
                out += indent > 0 ? ": " : ":";
                obj_[i].second.appendTo(out, indent, depth + 1);
                if (i + 1 < obj_.size()) out += ',';
            }
            newline(depth);
            out += '}';
            break;
        }
    }

} // namespace qpm
