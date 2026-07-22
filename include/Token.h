#pragma once
#include <string>

enum class TokenType
{
    // Literals
    NUMBER,
    STRING,
    TEMPLATE_STRING, // backtick template literal segment (text before ${)
    BOOL_TRUE,
    BOOL_FALSE,
    NIL,

    // Identifiers & Keywords
    IDENTIFIER,
    LET,
    CONST,
    FN,
    DEF,      // Python: def
    FUNCTION, // JavaScript: function
    CLASS,    // class keyword
    EXTENDS,  // extends / inherits
    NEW,      // new keyword
    THIS,     // this (JS alias for self)
    SUPER,    // super keyword
    RETURN,
    IF,
    ELSE,
    ELIF,
    WHILE,
    FOR,
    IN,
    OF, // JavaScript for...of
    BREAK,
    CONTINUE,
    RAISE,
    TRY,
    EXCEPT,
    FINALLY,
    AS,
    WITH,
    PRINT,
    INPUT,
    COUT, // cout
    CIN,  // cin
    FROM,
    IMPORT,
    // C/C++ style type keywords
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_DOUBLE,
    TYPE_CHAR,
    TYPE_STRING,
    TYPE_BOOL,
    TYPE_VOID,
    TYPE_LONG,
    TYPE_SHORT,
    TYPE_UNSIGNED,

    // Operators
    PLUS,
    MINUS,
    STAR,
    SLASH,
    FLOOR_DIV, // // integer division (Python)
    PERCENT,
    POWER,
    EQ,            // ==
    NEQ,           // !=
    STRICT_EQ,     // ===
    STRICT_NEQ,    // !==
    NULL_COALESCE, // ??
    LT,
    GT,
    LTE,
    GTE,
    AND,
    OR,
    NOT,
    IS,
    ASSIGN,
    PLUS_ASSIGN,
    MINUS_ASSIGN,
    STAR_ASSIGN,
    SLASH_ASSIGN,
    AND_ASSIGN, // &=
    OR_ASSIGN,  // |=
    XOR_ASSIGN, // ^=
    MOD_ASSIGN, // %=
    FAT_ARROW,  // =>
    PLUS_PLUS,
    MINUS_MINUS,
    BIT_AND,
    BIT_OR,
    BIT_XOR,
    BIT_NOT,
    LSHIFT,
    RSHIFT,
    AND_AND, // &&
    OR_OR,   // ||

    // Delimiters
    LPAREN,
    RPAREN,
    LBRACE,
    RBRACE,
    LBRACKET,
    RBRACKET,
    COMMA,
    SEMICOLON,
    COLON,
    DOT,
    ARROW,
    QUESTION,
    DECORATOR,
    NEWLINE,

    // Special
    EOF_TOKEN,
    UNKNOWN,
    INDENT, // Python-style indentation block start
    DEDENT, // Python-style indentation block end
};

struct Token
{
    TokenType type;
    std::string value;
    int line;
    int col;

    Token(TokenType t, std::string v, int ln, int c)
        : type(t), value(std::move(v)), line(ln), col(c) {}

    std::string toString() const;
};