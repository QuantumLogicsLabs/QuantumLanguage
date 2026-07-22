#include "Lexer.h"
#include "Error.h"
#include <stdexcept>
#include <cctype>
#include <sstream>

const std::unordered_map<std::string, TokenType> Lexer::keywords = {
    {"let", TokenType::LET},
    {"const", TokenType::CONST},
    {"fn", TokenType::FN},
    {"def", TokenType::DEF},
    {"function", TokenType::FUNCTION},
    {"class", TokenType::CLASS},
    {"extends", TokenType::EXTENDS},
    {"new", TokenType::NEW},
    {"this", TokenType::THIS},
    {"self", TokenType::THIS}, // Quantum alias → same token
    {"super", TokenType::SUPER},
    {"return", TokenType::RETURN},
    {"if", TokenType::IF},
    {"else", TokenType::ELSE},
    {"elif", TokenType::ELIF},
    {"while", TokenType::WHILE},
    {"for", TokenType::FOR},
    {"in", TokenType::IN},
    {"of", TokenType::OF},
    {"break", TokenType::BREAK},
    {"continue", TokenType::CONTINUE},
    {"raise", TokenType::RAISE},
    {"throw", TokenType::RAISE},
    {"try", TokenType::TRY},
    {"except", TokenType::EXCEPT},
    {"catch", TokenType::EXCEPT}, // JS alias
    {"finally", TokenType::FINALLY},
    {"as", TokenType::AS}, // JS alias
    {"with", TokenType::WITH},
    {"print", TokenType::PRINT},
    {"printf", TokenType::PRINT},
    {"input", TokenType::INPUT},
    {"scanf", TokenType::INPUT},
    {"cout", TokenType::COUT},
    {"cin", TokenType::CIN},
    {"import", TokenType::IMPORT},
    {"from", TokenType::FROM},
    {"true", TokenType::BOOL_TRUE},
    {"True", TokenType::BOOL_TRUE}, // Python
    {"false", TokenType::BOOL_FALSE},
    {"False", TokenType::BOOL_FALSE}, // Python
    {"nil", TokenType::NIL},
    {"null", TokenType::NIL},      // JavaScript alias
    {"undefined", TokenType::NIL}, // JavaScript alias
    {"None", TokenType::NIL},      // Python alias
    {"nullptr", TokenType::NIL},   // C++ alias
    {"NULL", TokenType::NIL},      // C alias
    {"and", TokenType::AND},
    {"or", TokenType::OR},
    {"not", TokenType::NOT},
    {"is", TokenType::IS},

    // C/C++ style type keywords
    {"int", TokenType::TYPE_INT},
    {"float", TokenType::TYPE_FLOAT},
    {"double", TokenType::TYPE_DOUBLE},
    {"char", TokenType::TYPE_CHAR},
    {"string", TokenType::TYPE_STRING},
    {"bool", TokenType::TYPE_BOOL},
    {"void", TokenType::TYPE_VOID},
    {"long", TokenType::TYPE_LONG},
    {"short", TokenType::TYPE_SHORT},
    {"unsigned", TokenType::TYPE_UNSIGNED},
};

Lexer::Lexer(const std::string &source)
    : src(source), pos(0), line(1), col(1) {}

char Lexer::current() const
{
    return pos < src.size() ? src[pos] : '\0';
}

char Lexer::peek(int offset) const
{
    size_t p = pos + offset;
    return p < src.size() ? src[p] : '\0';
}

char Lexer::advance()
{
    char c = src[pos++];
    if (c == '\n')
    {
        line++;
        col = 1;
    }
    else
        col++;
    return c;
}

void Lexer::skipWhitespace()
{
    while (pos < src.size() && (current() == ' ' || current() == '\t' || current() == '\r'))
        advance();
}

void Lexer::skipComment()
{
    while (pos < src.size() && current() != '\n')
        advance();
}

void Lexer::skipBlockComment()
{
    // We've already consumed '/*' — skip until '*/'
    while (pos < src.size())
    {
        if (current() == '*' && peek() == '/')
        {
            advance(); // skip *
            advance(); // skip /
            return;
        }
        advance();
    }
    // Unterminated block comment — just silently reach EOF
}

Token Lexer::readNumber()
{
    int startLine = line, startCol = col;
    std::string num;
    bool hasDot = false;

    if (current() == '0' && (peek() == 'x' || peek() == 'X'))
    {
        num += advance();
        num += advance(); // 0x
        while (pos < src.size() && std::isxdigit(current()))
            num += advance();
    }
    else
    {
        while (pos < src.size() && (std::isdigit(current()) || current() == '.'))
        {
            if (current() == '.')
            {
                if (hasDot)
                    break;
                hasDot = true;
            }
            num += advance();
        }
        // Strip C integer/float suffixes: LL, ULL, LU, L, U, F, f (e.g. 1000000007LL, 3.14f)
        while (pos < src.size() && (current() == 'L' || current() == 'l' ||
                                    current() == 'U' || current() == 'u' ||
                                    current() == 'F' || current() == 'f'))
            advance(); // consume but don't add to num
    }
    return Token(TokenType::NUMBER, num, startLine, startCol);
}

// Template literal: `Hello ${name}, you are ${age} years old!`
// Emits: STRING("Hello ") PLUS LPAREN STRING(name-as-expr) RPAREN PLUS STRING(", you are ") ...
// We expand into: "seg0" + (expr1) + "seg1" + (expr2) + "seg2" ...
