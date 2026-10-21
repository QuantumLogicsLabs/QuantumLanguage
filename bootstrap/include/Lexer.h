#pragma once
#include "Token.h"
#include <string>
#include <vector>
#include <unordered_map>

class Lexer
{
public:
    explicit Lexer(const std::string &source);
    std::vector<Token> tokenize();

private:
    std::string src;
    size_t pos;
    int line, col;

    static const std::unordered_map<std::string, TokenType> keywords;

    std::vector<Token> pendingTokens_; // used for f-string expansion
    // C preprocessor #define macros: name → replacement token list
    std::unordered_map<std::string, std::vector<Token>> defines_;

    char current() const;
    char peek(int offset = 1) const;
    char advance();
    void skipWhitespace();
    void skipComment();      // single-line: // ...
    void skipBlockComment(); // multi-line:  /* ... */

    Token readNumber();
    Token readString(char quote);
    bool atTripleQuote(char quote) const;
    Token readTripleString(char quote, bool raw);
    // After a value token, whether the `//` at pos is a C/JS line comment
    // rather than Python floor division.
    bool slashSlashIsComment() const;
    void readTemplateLiteral(std::vector<Token> &out, int startLine, int startCol);
    Token readIdentifierOrKeyword();
    Token readOperator();
};