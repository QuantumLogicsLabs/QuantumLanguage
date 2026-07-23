#include "DialectScan.h"

#include <cctype>

// Remove leading/trailing spaces while preserving the original line indentation
// separately. This is used only by the lightweight Ruby dialect normalizer.
std::string trimCopy(const std::string &value)
{
    size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool startsWith(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}
// Index just past the string literal starting at s[pos] (s[pos] must be a
// quote character: ' " or the backtick used by Quantum template literals).
size_t rbSkipString(const std::string &s, size_t pos)
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

bool rbIsQuote(char c) { return c == '"' || c == '\'' || c == '`'; }
bool rbIsIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool rbIsIdentChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// Find `needle` at bracket-depth 0, outside string literals.
size_t rbFindTopLevel(const std::string &s, const std::string &needle, size_t from)
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
size_t rbMatchBracket(const std::string &s, size_t openPos)
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
std::vector<std::string> rbSplitTopLevel(const std::string &s, char sep)
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
size_t rbFindTopLevelAssign(const std::string &s)
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
