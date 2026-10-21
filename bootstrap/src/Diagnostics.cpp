#include "Diagnostics.h"

#include <cctype>
#include <sstream>

namespace diag
{

std::string sourceLine(const std::string &source, int line)
{
    if (line < 1)
        return "";
    size_t pos = 0;
    for (int l = 1; l < line; ++l)
    {
        pos = source.find('\n', pos);
        if (pos == std::string::npos)
            return "";
        ++pos;
    }
    size_t end = source.find('\n', pos);
    std::string text = source.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (!text.empty() && text.back() == '\r')
        text.pop_back();
    return text;
}

static bool identChar(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; }

int findWord(const std::string &text, const std::string &word)
{
    if (word.empty())
        return 0;
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i)
    {
        char c = text[i];
        if (quote)
        {
            if (c == '\\')
                ++i;
            else if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            quote = c;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
            break;
        if (text.compare(i, word.size(), word) == 0 && (i == 0 || !identChar(text[i - 1])) &&
            (i + word.size() >= text.size() || !identChar(text[i + word.size()]) || !identChar(word.back())))
            return (int)i + 1;
    }
    return 0;
}

std::string quotedName(const std::string &msg)
{
    size_t a = msg.find('\'');
    if (a == std::string::npos)
        return "";
    size_t b = msg.find('\'', a + 1);
    if (b == std::string::npos)
        return "";
    return msg.substr(a + 1, b - a - 1);
}

// The caret line under `code`: every character before column `col` becomes
// a space, except tabs, which stay tabs so the caret lines up in any editor.
static std::string caretPad(const std::string &code, int col)
{
    std::string pad;
    for (int i = 1; i < col; ++i)
        pad += (i - 1 < (int)code.size() && code[i - 1] == '\t') ? '\t' : ' ';
    return pad;
}

std::string gcc(const std::string &file, const std::string &source, int line, int col,
                const std::string &msg, int width, const std::string &severity)
{
    std::ostringstream os;
    os << file << ":" << (line > 0 ? line : 1);
    if (col > 0)
        os << ":" << col;
    os << ": " << severity << ": " << msg << "\n";
    std::string code = sourceLine(source, line);
    if (line > 0 && !code.empty() && code.size() < 400)
    {
        std::string num = std::to_string(line);
        std::string gutter = num.size() < 5 ? std::string(5 - num.size(), ' ') + num : num;
        os << gutter << " | " << code << "\n";
        if (col > 0)
        {
            os << std::string(gutter.size(), ' ') << " | " << caretPad(code, col) << "^";
            for (int i = 1; i < width && col - 1 + i < (int)code.size(); ++i)
                os << "~";
            os << "\n";
        }
    }
    return os.str();
}

std::string javac(const std::string &file, const std::string &source, int line, int col,
                  const std::string &msg, const std::vector<std::string> &notes)
{
    std::ostringstream os;
    os << file << ":" << (line > 0 ? line : 1) << ": error: " << msg << "\n";
    std::string code = sourceLine(source, line);
    if (line > 0 && code.size() < 400)
    {
        os << code << "\n";
        if (col > 0)
            os << caretPad(code, col) << "^\n";
    }
    for (const auto &n : notes)
        os << n << "\n";
    return os.str();
}

} // namespace diag
