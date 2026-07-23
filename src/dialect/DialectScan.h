#pragma once
// Shared line/expression scanning helpers used by the dialect passes.
// All of them are string-literal aware: they never match inside a quoted
// string, which is what makes them safe to run over arbitrary source text.

#include <string>
#include <vector>

std::string trimCopy(const std::string &value);
bool startsWith(const std::string &value, const std::string &prefix);

bool rbIsQuote(char c);
bool rbIsIdentStart(char c);
bool rbIsIdentChar(char c);

// Index just past the string literal starting at s[pos].
size_t rbSkipString(const std::string &s, size_t pos);
// Find `needle` at bracket-depth 0, outside string literals.
size_t rbFindTopLevel(const std::string &s, const std::string &needle, size_t from = 0);
// Index of the character matching the bracket at s[openPos].
size_t rbMatchBracket(const std::string &s, size_t openPos);
// Split on `sep` at bracket-depth 0, outside string literals. Trims pieces.
std::vector<std::string> rbSplitTopLevel(const std::string &s, char sep);
// Find a bare top-level '=' (assignment), skipping ==, !=, +=, etc.
size_t rbFindTopLevelAssign(const std::string &s);
