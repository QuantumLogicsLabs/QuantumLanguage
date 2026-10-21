#include "Lexer.h"
#include "Error.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> rawTokens;
    // Index of the token that directly follows a dropped "std::" qualifier.
    size_t afterStdQualifier = std::string::npos;

    while (pos < src.size())
    {
        skipWhitespace();
        if (pos >= src.size())
            break;

        char c = current();
        int startLine = line, startCol = col;

        if (c == '\n')
        {
            rawTokens.emplace_back(TokenType::NEWLINE, "\\n", startLine, startCol);
            advance();
            continue;
        }

        if (c == '#')
        {
            advance(); // consume the '#' itself
            // Skip horizontal whitespace after '#'
            while (pos < src.size() && (current() == ' ' || current() == '\t'))
                advance();
            // Read the directive name
            std::string directive;
            while (pos < src.size() && std::isalpha(current()))
                directive += advance();
            if (directive == "define")
            {
                // Skip spaces
                while (pos < src.size() && (current() == ' ' || current() == '\t'))
                    advance();
                // Read macro name
                std::string macroName;
                while (pos < src.size() && (std::isalnum(current()) || current() == '_'))
                    macroName += advance();
                // Skip spaces between name and value
                while (pos < src.size() && (current() == ' ' || current() == '\t'))
                    advance();
                // Read replacement value tokens until end of line
                std::vector<Token> replacement;
                while (pos < src.size() && current() != '\n' && current() != '\r')
                {
                    // Skip whitespace between tokens
                    while (pos < src.size() && (current() == ' ' || current() == '\t'))
                        advance();
                    if (pos >= src.size() || current() == '\n' || current() == '\r')
                        break;
                    int tl = line, tc = col;
                    char rc = current();
                    if (std::isdigit(rc) || (rc == '-' && pos + 1 < src.size() && std::isdigit(src[pos + 1])))
                    {
                        replacement.push_back(readNumber());
                    }
                    else if (rc == '\'' || rc == '"')
                    {
                        replacement.push_back(readString(rc));
                    }
                    else if (std::isalpha(rc) || rc == '_')
                    {
                        // Read identifier — may itself be a previously defined macro
                        std::string id;
                        while (pos < src.size() && (std::isalnum(current()) || current() == '_'))
                            id += advance();
                        // Check if this identifier is itself a macro (simple one-level expansion)
                        auto dit = defines_.find(id);
                        if (dit != defines_.end())
                        {
                            for (auto &t : dit->second)
                                replacement.push_back(t);
                        }
                        else
                        {
                            replacement.emplace_back(TokenType::IDENTIFIER, id, tl, tc);
                        }
                    }
                    else
                    {
                        // Operator/punctuation — skip rest of line to keep things simple
                        while (pos < src.size() && current() != '\n' && current() != '\r')
                            advance();
                        break;
                    }
                }
                if (!macroName.empty())
                    defines_[macroName] = std::move(replacement);
            }
            // Skip rest of directive line (#include, #ifdef, #ifndef, #endif, #else, #undef, etc.)
            while (pos < src.size() && current() != '\n' && current() != '\r')
                advance();
            continue;
        }

        if (std::isdigit(c))
        {
            rawTokens.push_back(readNumber());
            continue;
        }
        if (c == '"' || c == '\'')
        {
            rawTokens.push_back(atTripleQuote(c) ? readTripleString(c, false) : readString(c));
            continue;
        }
        if (c == '`')
        {
            readTemplateLiteral(rawTokens, startLine, startCol);
            continue;
        }
        if (std::isalpha(c) || c == '_')
        {
            auto tok = readIdentifierOrKeyword();
            if (tok.type == TokenType::UNKNOWN && tok.value == "__fstring__")
            {
                // f-string expansion — flush pending tokens
                for (auto &pt : pendingTokens_)
                    rawTokens.push_back(pt);
                pendingTokens_.clear();
            }
            else
            {
                rawTokens.push_back(tok);
                // Flush any extra tokens from multi-token macro expansion
                if (!pendingTokens_.empty())
                {
                    for (auto &pt : pendingTokens_)
                        rawTokens.push_back(pt);
                    pendingTokens_.clear();
                }
            }
            continue;
        }

        // Operators & delimiters
        advance();
        switch (c)
        {
        case '+':
            if (current() == '+')
            {
                advance();
                rawTokens.emplace_back(TokenType::PLUS_PLUS, "++", startLine, startCol);
            }
            else if (current() == '=')
            {
                advance();
                rawTokens.emplace_back(TokenType::PLUS_ASSIGN, "+=", startLine, startCol);
            }
            else
                rawTokens.emplace_back(TokenType::PLUS, "+", startLine, startCol);
            break;
        case '-':
            if (current() == '-')
            {
                advance();
                rawTokens.emplace_back(TokenType::MINUS_MINUS, "--", startLine, startCol);
            }
            else if (current() == '>')
            {
                advance();
                rawTokens.emplace_back(TokenType::ARROW, "->", startLine, startCol);
            }
            else if (current() == '=')
            {
                advance();
                rawTokens.emplace_back(TokenType::MINUS_ASSIGN, "-=", startLine, startCol);
            }
            else
                rawTokens.emplace_back(TokenType::MINUS, "-", startLine, startCol);
            break;
        case '*':
            if (current() == '*')
            {
                advance();
                rawTokens.emplace_back(TokenType::POWER, "**", startLine, startCol);
            }
            else if (current() == '=')
            {
                advance();
                rawTokens.emplace_back(TokenType::STAR_ASSIGN, "*=", startLine, startCol);
            }
            else
                rawTokens.emplace_back(TokenType::STAR, "*", startLine, startCol);
            break;
        case '/':
            // Regex literal detection: /pattern/flags
            // Only when prev token is NOT a value (number/string/ident/)/])
            if (current() != '/' && current() != '*' && current() != '=')
            {
                bool prevIsVal = false;
                if (!rawTokens.empty())
                {
                    TokenType ptt = rawTokens.back().type;
                    prevIsVal = (ptt == TokenType::NUMBER || ptt == TokenType::STRING ||
                                 ptt == TokenType::IDENTIFIER || ptt == TokenType::RPAREN ||
                                 ptt == TokenType::RBRACKET || ptt == TokenType::BOOL_TRUE ||
                                 ptt == TokenType::BOOL_FALSE);
                }
                if (!prevIsVal)
                {
                    // Lex regex: scan to unescaped '/' respecting character classes [...]
                    std::string regStr = "/";
                    bool inCls = false;
                    while (pos < src.size() && current() != '\n')
                    {
                        char rc = src[pos];
                        if (rc == '\\' && pos + 1 < src.size())
                        {
                            regStr += rc;
                            regStr += src[pos + 1];
                            pos += 2;
                            col += 2;
                            continue;
                        }
                        if (rc == '[')
                            inCls = true;
                        if (rc == ']')
                            inCls = false;
                        if (rc == '/' && !inCls)
                        {
                            advance();
                            regStr += '/';
                            break;
                        }
                        regStr += rc;
                        advance();
                    }
                    // eat flags: g i m s u y
                    while (pos < src.size() && std::isalpha(current()))
                        regStr += advance();
                    rawTokens.emplace_back(TokenType::STRING, regStr, startLine, startCol);
                    continue;
                }
            }
            if (current() == '/')
            {
                // Distinguish Python floor-division // from C/JS // line comment.
                // Strategy: only treat // as floor-div when the immediately preceding
                // token is a value-producing token (number, string, identifier, ), ]).
                // In all other positions (start of line, after operator, etc.) it is
                // a comment.  This matches real-world usage across languages.
                {
                    bool prevIsValue = false;
                    if (!rawTokens.empty())
                    {
                        TokenType pt = rawTokens.back().type;
                        prevIsValue = (pt == TokenType::NUMBER ||
                                       pt == TokenType::STRING ||
                                       pt == TokenType::BOOL_TRUE ||
                                       pt == TokenType::BOOL_FALSE ||
                                       pt == TokenType::NIL ||
                                       pt == TokenType::IDENTIFIER ||
                                       pt == TokenType::RPAREN ||
                                       pt == TokenType::RBRACKET);
                    }
                    if (prevIsValue && !slashSlashIsComment())
                    {
                        advance(); // consume second '/'
                        rawTokens.emplace_back(TokenType::FLOOR_DIV, "//", startLine, startCol);
                    }
                    else
                        skipComment();
                }
            }
            else if (current() == '*')
            {
                advance(); // consume the *
                skipBlockComment();
            }
            else if (current() == '=')
            {
                advance();
                rawTokens.emplace_back(TokenType::SLASH_ASSIGN, "/=", startLine, startCol);
            }
            else
                rawTokens.emplace_back(TokenType::SLASH, "/", startLine, startCol);
            break;
        case '=':
            if (current() == '=')
            {
                advance();
                if (current() == '=')
                {
                    advance();
                    rawTokens.emplace_back(TokenType::STRICT_EQ, "===", startLine, startCol);
                }
                else
                    rawTokens.emplace_back(TokenType::EQ, "==", startLine, startCol);
            }
            else if (current() == '>')
            {
                advance();
                rawTokens.emplace_back(TokenType::FAT_ARROW, "=>", startLine, startCol);
            }
            else
                rawTokens.emplace_back(TokenType::ASSIGN, "=", startLine, startCol);
            break;
        case '!':
            if (current() == '=')
            {
                advance();
                if (current() == '=')
                {
                    advance();
                    rawTokens.emplace_back(TokenType::STRICT_NEQ, "!==", startLine, startCol);
                }
                else
                    rawTokens.emplace_back(TokenType::NEQ, "!=", startLine, startCol);
            }
            else
                rawTokens.emplace_back(TokenType::NOT, "!", startLine, startCol);
            break;
        case '<':
            if (current() == '=')
            {
                advance();
                rawTokens.emplace_back(TokenType::LTE, "<=", startLine, startCol);
            }
            else if (current() == '<')
            {
                advance();
                rawTokens.emplace_back(TokenType::LSHIFT, "<<", startLine, startCol);
            }
            else
                rawTokens.emplace_back(TokenType::LT, "<", startLine, startCol);
            break;
        case '>':
            if (current() == '=')
            {
                advance();
                rawTokens.emplace_back(TokenType::GTE, ">=", startLine, startCol);
            }
            else if (current() == '>')
            {
                advance();
                // >>= compound assignment
                if (current() == '=')
                {
                    advance();
                    rawTokens.emplace_back(TokenType::RSHIFT, ">>", startLine, startCol);
                    rawTokens.emplace_back(TokenType::ASSIGN, "=", startLine, startCol);
                }
                else
                    rawTokens.emplace_back(TokenType::RSHIFT, ">>", startLine, startCol);
            }
            else
                rawTokens.emplace_back(TokenType::GT, ">", startLine, startCol);
            break;
        case '&':
            if (current() == '&')
            {
                advance();
                rawTokens.emplace_back(TokenType::AND_AND, "&&", startLine, startCol);
            }
            else if (current() == '=')
            {
                advance();
                rawTokens.emplace_back(TokenType::AND_ASSIGN, "&=", startLine, startCol);
            }
            else
                rawTokens.emplace_back(TokenType::BIT_AND, "&", startLine, startCol);
            break;
        case '|':
            if (current() == '|')
            {
                advance();
                rawTokens.emplace_back(TokenType::OR_OR, "||", startLine, startCol);
            }
            else if (current() == '=')
            {
                advance();
                rawTokens.emplace_back(TokenType::OR_ASSIGN, "|=", startLine, startCol);
            }
            else
                rawTokens.emplace_back(TokenType::BIT_OR, "|", startLine, startCol);
            break;
        case '^':
            if (current() == '=')
            {
                advance();
                rawTokens.emplace_back(TokenType::XOR_ASSIGN, "^=", startLine, startCol);
            }
            else
                rawTokens.emplace_back(TokenType::BIT_XOR, "^", startLine, startCol);
            break;
        case '~':
            rawTokens.emplace_back(TokenType::BIT_NOT, "~", startLine, startCol);
            break;
        case '%':
            if (current() == '=')
            {
                advance();
                rawTokens.emplace_back(TokenType::MOD_ASSIGN, "%=", startLine, startCol);
            }
            else
                rawTokens.emplace_back(TokenType::PERCENT, "%", startLine, startCol);
            break;
        case '(':
            rawTokens.emplace_back(TokenType::LPAREN, "(", startLine, startCol);
            break;
        case ')':
            rawTokens.emplace_back(TokenType::RPAREN, ")", startLine, startCol);
            break;
        case '{':
            rawTokens.emplace_back(TokenType::LBRACE, "{", startLine, startCol);
            break;
        case '}':
            rawTokens.emplace_back(TokenType::RBRACE, "}", startLine, startCol);
            break;
        case '[':
            rawTokens.emplace_back(TokenType::LBRACKET, "[", startLine, startCol);
            break;
        case ']':
            rawTokens.emplace_back(TokenType::RBRACKET, "]", startLine, startCol);
            break;
        case ',':
            rawTokens.emplace_back(TokenType::COMMA, ",", startLine, startCol);
            break;
        case ';':
            rawTokens.emplace_back(TokenType::SEMICOLON, ";", startLine, startCol);
            break;
        case ':':
            if (current() == ':' &&
                !rawTokens.empty() && rawTokens.back().type == TokenType::IDENTIFIER &&
                (rawTokens.back().value == "std" ||
                 rawTokens.size() - 1 == afterStdQualifier))
            {
                // C++ scope resolution "std::" — drop the qualifier so
                // std::cout / std::string lex as plain cout / string, along
                // with nested namespaces right after it (std::chrono::,
                // std::this_thread::). Only a chain rooted at "std" is treated
                // this way: a bare "ident::" must stay two COLONs so Python
                // slices like a[i::2] keep working.
                advance();
                rawTokens.pop_back();
                afterStdQualifier = rawTokens.size();
            }
            else
                rawTokens.emplace_back(TokenType::COLON, ":", startLine, startCol);
            break;
        case '.':
            if (current() == '.' && pos + 1 < src.size() && src[pos + 1] == '.')
            {
                advance();
                advance(); // eat the remaining two dots
                rawTokens.emplace_back(TokenType::IDENTIFIER, "...", startLine, startCol);
            }
            else
            {
                rawTokens.emplace_back(TokenType::DOT, ".", startLine, startCol);
            }
            break;
        case '?':
            // Optional chaining: ?.  → treat as DOT (values are always valid in our interpreter)
            if (current() == '.')
            {
                advance(); // consume the '.'
                rawTokens.emplace_back(TokenType::DOT, ".", startLine, startCol);
            }
            else if (current() == '?')
            {
                advance(); // consume another '?'
                rawTokens.emplace_back(TokenType::NULL_COALESCE, "??", startLine, startCol);
            }
            else if (current() == '[')
            {
                // ?.[  — optional index, treat as LBRACKET
                rawTokens.emplace_back(TokenType::QUESTION, "?", startLine, startCol);
            }
            else
                rawTokens.emplace_back(TokenType::QUESTION, "?", startLine, startCol);
            break;
        case '@':
            rawTokens.emplace_back(TokenType::DECORATOR, "@", startLine, startCol);
            break;
        default:
            throw QuantumError("LexError", std::string("Unexpected character: ") + c, startLine);
        }
    }

    rawTokens.emplace_back(TokenType::EOF_TOKEN, "", line, col);

    // ── Python-style INDENT/DEDENT post-processing ───────────────────────────
    // Scan for COLON + NEWLINE + more-indented line → inject INDENT/DEDENT tokens.
    // Brace-style { } files are completely unaffected.

    std::vector<Token> tokens;
    tokens.reserve(rawTokens.size() + 32);

    // Precompute leading-space count for each line (tabs = 4 spaces)
    std::vector<int> indentOf(line + 2, 0);
    {
        int curLine = 1, curIndent = 0;
        bool lineStart = true;
        for (size_t i = 0; i < src.size(); ++i)
        {
            char ch = src[i];
            if (ch == '\n')
            {
                indentOf[curLine] = curIndent;
                curLine++;
                curIndent = 0;
                lineStart = true;
            }
            else if (lineStart)
            {
                if (ch == ' ')
                    curIndent++;
                else if (ch == '\t')
                    curIndent += 4;
                else
                    lineStart = false;
            }
        }
        indentOf[curLine] = curIndent;
    }

    std::vector<int> indentStack = {0};
    int bracketDepth = 0;      // track ( { [ depth — never emit INDENT/DEDENT inside these
    int parenBracketDepth = 0; // track ( [ depth only — entirely drop NEWLINE inside these

    for (size_t i = 0; i < rawTokens.size(); ++i)
    {
        Token &tok = rawTokens[i];

        // Track bracket/brace/paren depth
        if (tok.type == TokenType::LBRACE ||
            tok.type == TokenType::LBRACKET ||
            tok.type == TokenType::LPAREN)
        {
            bracketDepth++;
            if (tok.type != TokenType::LBRACE)
                parenBracketDepth++;
        }
        else if (tok.type == TokenType::RBRACE ||
                 tok.type == TokenType::RBRACKET ||
                 tok.type == TokenType::RPAREN)
        {
            bracketDepth = std::max(0, bracketDepth - 1);
            if (tok.type != TokenType::RBRACE)
                parenBracketDepth = std::max(0, parenBracketDepth - 1);
        }

        if (tok.type == TokenType::NEWLINE && parenBracketDepth > 0)
            continue; // Drop NEWLINE entirely inside ( ) and [ ]

        // COLON followed by NEWLINE + deeper indent → open Python block
        // But NOT inside brackets/braces/parens (dict literal, array, call args)
        if (tok.type == TokenType::COLON && bracketDepth == 0)
        {
            size_t j = i + 1;
            while (j < rawTokens.size() && rawTokens[j].type == TokenType::NEWLINE)
                ++j;
            if (j < rawTokens.size() && rawTokens[j].type != TokenType::EOF_TOKEN)
            {
                int nextIndent = indentOf[rawTokens[j].line];
                if (nextIndent > indentStack.back())
                {
                    tokens.push_back(tok);
                    for (size_t k = i + 1; k < j; ++k)
                        tokens.push_back(rawTokens[k]);
                    i = j - 1;
                    indentStack.push_back(nextIndent);
                    tokens.emplace_back(TokenType::INDENT, "INDENT", tok.line, tok.col);
                    continue;
                }
            }
            tokens.push_back(tok);
            continue;
        }

        // After NEWLINE, emit DEDENTs if next line is less indented
        // But NOT inside brackets/braces/parens
        if (tok.type == TokenType::NEWLINE && bracketDepth == 0)
        {
            tokens.push_back(tok);
            size_t j = i + 1;
            while (j < rawTokens.size() && rawTokens[j].type == TokenType::NEWLINE)
                ++j;
            if (j < rawTokens.size() && rawTokens[j].type != TokenType::EOF_TOKEN)
            {
                int nextIndent = indentOf[rawTokens[j].line];
                while (indentStack.size() > 1 && nextIndent < indentStack.back())
                {
                    indentStack.pop_back();
                    tokens.emplace_back(TokenType::DEDENT, "DEDENT", tok.line, tok.col);
                }
            }
            else
            {
                while (indentStack.size() > 1)
                {
                    indentStack.pop_back();
                    tokens.emplace_back(TokenType::DEDENT, "DEDENT", tok.line, tok.col);
                }
            }
            continue;
        }

        tokens.push_back(tok);
    }

    return tokens;
}
