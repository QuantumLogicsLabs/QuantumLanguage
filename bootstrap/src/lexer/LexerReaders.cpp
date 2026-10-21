#include "Lexer.h"
#include "Error.h"
#include <cctype>
#include <string>
#include <vector>

void Lexer::readTemplateLiteral(std::vector<Token> &out, int startLine, int startCol)
{
    advance(); // skip opening backtick

    // Collect alternating: text segment, then expression source string
    // We'll build a list of parts: {isExpr, content}
    struct Part
    {
        bool isExpr;
        std::string content;
    };
    std::vector<Part> parts;

    std::string seg;
    while (pos < src.size() && current() != '`')
    {
        if (current() == '\\')
        {
            advance();
            switch (current())
            {
            case 'n':
                seg += '\n';
                break;
            case 't':
                seg += '\t';
                break;
            case '`':
                seg += '`';
                break;
            case '\\':
                seg += '\\';
                break;
            case '$':
                seg += '$';
                break;
            default:
                seg += '\\';
                seg += current();
                break;
            }
            advance();
        }
        else if (current() == '$' && pos + 1 < src.size() && src[pos + 1] == '{')
        {
            // Flush current text segment
            parts.push_back({false, seg});
            seg.clear();
            advance(); // skip $
            advance(); // skip {
            // Collect expression source until matching }
            std::string expr;
            int depth = 1;
            while (pos < src.size() && depth > 0)
            {
                if (current() == '{')
                    depth++;
                else if (current() == '}')
                {
                    if (--depth == 0)
                    {
                        advance();
                        break;
                    }
                }
                expr += current();
                advance();
            }
            parts.push_back({true, expr});
        }
        else
        {
            seg += current();
            advance();
        }
    }
    if (pos < src.size())
        advance(); // skip closing backtick

    // Flush final text segment
    parts.push_back({false, seg});

    // Remove empty text-only parts at start/end to keep output clean
    // Build token sequence: part[0] + part[1] + ...
    // Text parts → STRING tokens, expr parts → re-lexed tokens wrapped in parens

    bool first = true;
    auto emitPlus = [&]()
    {
        if (!first)
            out.emplace_back(TokenType::PLUS, "+", startLine, startCol);
        first = false;
    };

    for (auto &p : parts)
    {
        if (!p.isExpr)
        {
            // Always emit text segments (even empty ones if they're the only part)
            if (!p.content.empty() || parts.size() == 1)
            {
                emitPlus();
                out.emplace_back(TokenType::STRING, p.content, startLine, startCol);
            }
        }
        else
        {
            // Re-lex the expression and wrap in parentheses
            // Use str() wrapper so numbers/bools get stringified in concat context
            emitPlus();
            // emit: str( <expr tokens> )
            out.emplace_back(TokenType::LPAREN, "(", startLine, startCol);
            Lexer subLex(p.content);
            auto subTokens = subLex.tokenize();
            for (auto &t : subTokens)
                if (t.type != TokenType::EOF_TOKEN && t.type != TokenType::NEWLINE)
                    out.push_back(t);
            out.emplace_back(TokenType::RPAREN, ")", startLine, startCol);
        }
    }

    // If the entire template was empty
    if (first)
        out.emplace_back(TokenType::STRING, "", startLine, startCol);
}

Token Lexer::readString(char quote)
{
    int startLine = line, startCol = col;
    advance(); // skip opening quote
    std::string str;
    while (pos < src.size() && current() != quote)
    {
        if (current() == '\\')
        {
            advance();
            switch (current())
            {
            case 'n':
                str += '\n';
                break;
            case 't':
                str += '\t';
                break;
            case 'r':
                str += '\r';
                break;
            case '\\':
                str += '\\';
                break;
            case '\'':
                str += '\'';
                break;
            case '"':
                str += '"';
                break;
            case '0':
                str += '\0';
                break;
            default:
                str += current();
            }
            advance();
        }
        else
        {
            str += advance();
        }
    }
    if (pos >= src.size())
        throw QuantumError("LexError", "Unterminated string literal", startLine);
    advance(); // skip closing quote
    return Token(TokenType::STRING, str, startLine, startCol);
}

bool Lexer::atTripleQuote(char quote) const
{
    return current() == quote && peek(1) == quote && peek(2) == quote;
}

// Python """...""" / '''...''' — may span lines and contain lone quotes.
// Escapes are processed as in readString unless `raw` (r"""...""").
Token Lexer::readTripleString(char quote, bool raw)
{
    int startLine = line, startCol = col;
    advance();
    advance();
    advance(); // opening quotes
    std::string str;
    while (pos < src.size() && !atTripleQuote(quote))
    {
        if (current() == '\\' && !raw && pos + 1 < src.size())
        {
            advance();
            switch (current())
            {
            case 'n':
                str += '\n';
                break;
            case 't':
                str += '\t';
                break;
            case 'r':
                str += '\r';
                break;
            case '0':
                str += '\0';
                break;
            case '\n':
                break; // backslash-newline continues the line
            default:
                str += current();
            }
            advance();
        }
        else
            str += advance();
    }
    if (pos >= src.size())
        throw QuantumError("LexError", "Unterminated triple-quoted string literal", startLine);
    advance();
    advance();
    advance(); // closing quotes
    return Token(TokenType::STRING, str, startLine, startCol);
}

bool Lexer::slashSlashIsComment() const
{
    // pos is at the second '/'. Prose gives a comment away: nothing
    // operand-like follows, or two bare words sit side by side
    // ("// Remove HTML tags"), which no expression has.
    size_t p = pos + 1;
    while (p < src.size() && (src[p] == ' ' || src[p] == '\t'))
        p++;
    if (p >= src.size() || src[p] == '\n' || src[p] == '\r')
        return true;
    char c = src[p];
    if (!(std::isalnum((unsigned char)c) || c == '_' || c == '(' || c == '[' ||
          c == '-' || c == '+' || c == '"' || c == '\'' || c == '.'))
        return true;

    auto isOperatorWord = [](const std::string &w)
    {
        return w == "and" || w == "or" || w == "not" || w == "if" || w == "else" ||
               w == "in" || w == "is" || w == "for" || w == "lambda";
    };
    bool prevWasWord = false;
    std::string prevWord;
    while (p < src.size() && src[p] != '\n')
    {
        char ch = src[p];
        if (ch == '#' || ch == ';')
            break; // trailing comment / statement end
        if (ch == '"' || ch == '\'')
        {
            p++;
            while (p < src.size() && src[p] != ch && src[p] != '\n')
                p += (src[p] == '\\') ? 2 : 1;
            p++;
            prevWasWord = false;
            continue;
        }
        if (std::isalnum((unsigned char)ch) || ch == '_')
        {
            std::string w;
            while (p < src.size() && (std::isalnum((unsigned char)src[p]) || src[p] == '_' || src[p] == '.'))
                w += src[p++];
            if (prevWasWord && !isOperatorWord(prevWord) && !isOperatorWord(w))
                return true;
            prevWord = w;
            prevWasWord = true;
            continue;
        }
        if (ch != ' ' && ch != '\t' && ch != '\r')
            prevWasWord = false;
        p++;
    }
    return false;
}

Token Lexer::readIdentifierOrKeyword()
{
    int startLine = line, startCol = col;
    std::string id;
    while (pos < src.size() && (std::isalnum(current()) || current() == '_'))
        id += advance();

    // Raw string prefix: r"..." or r'...' — literal string with no escape sequences
    if ((id == "r" || id == "R") && pos < src.size() && (current() == '"' || current() == '\''))
    {
        char quote = current();
        if (atTripleQuote(quote))
            return readTripleString(quote, /*raw=*/true);
        int strStartLine = line, strStartCol = col;
        advance(); // skip opening quote
        std::string raw;
        while (pos < src.size() && current() != quote)
        {
            raw += advance();
        }
        if (pos < src.size())
            advance(); // skip closing quote
        return Token(TokenType::STRING, raw, strStartLine, strStartCol);
    }

    // f-string prefix: f"..." or f'...'  — treat like a backtick template literal
    if ((id == "f" || id == "F") && pos < src.size() && (current() == '"' || current() == '\''))
    {
        char quote = current();
        // f"""...""" spans lines and ends only at the matching triple quote.
        const bool triple = atTripleQuote(quote);
        auto atClose = [&]()
        { return triple ? atTripleQuote(quote) : current() == quote; };
        advance(); // skip opening quote
        if (triple)
        {
            advance();
            advance();
        }
        // Convert {expr} → ${expr} then re-lex as template
        std::string raw;
        while (pos < src.size() && !atClose())
        {
            if (current() == '{')
            {
                advance(); // skip {
                std::string exprPart;
                std::string fmtPart;
                int depth = 1;
                bool inFormat = false;
                int parenDepth = 0;
                bool inSingleQ = false;
                bool inDoubleQ = false;
                while (pos < src.size() && depth > 0)
                {
                    char ch = current();
                    // Track string literals inside the expression to avoid false colon matches
                    if (!inDoubleQ && ch == '\'' && !inFormat)
                        inSingleQ = !inSingleQ;
                    else if (!inSingleQ && ch == '"' && !inFormat)
                        inDoubleQ = !inDoubleQ;
                    bool insideStr = inSingleQ || inDoubleQ;

                    if (!insideStr)
                    {
                        if (ch == '(')
                            parenDepth++;
                        else if (ch == ')')
                        {
                            if (parenDepth > 0)
                                parenDepth--;
                        }
                        else if (ch == '{')
                            depth++;
                        else if (ch == '}')
                        {
                            depth--;
                            if (depth == 0)
                            {
                                advance();
                                break;
                            }
                        }
                        // Only treat ':' as format separator at top-level (no nested parens/braces)
                        if (depth == 1 && parenDepth == 0 && ch == ':' && !inFormat)
                        {
                            inFormat = true;
                            advance();
                            continue;
                        }
                    }

                    if (inFormat)
                        fmtPart += ch;
                    else
                        exprPart += ch;
                    advance();
                }

                if (inFormat)
                {
                    raw += "${__format__(" + exprPart + ", \"" + fmtPart + "\")}";
                }
                else
                {
                    raw += "${" + exprPart + "}";
                }
            }
            else if (current() == '\\')
            {
                raw += current();
                advance();
                if (pos < src.size())
                {
                    raw += current();
                    advance();
                }
            }
            else
            {
                raw += current();
                advance();
            }
        }
        for (int q = triple ? 3 : 1; q > 0 && pos < src.size(); --q)
            advance(); // skip closing quote(s)
        // Re-lex wrapped in backticks using the existing template literal engine
        std::string backtickSrc = "`" + raw + "`";
        Lexer subLex(backtickSrc);
        // We can't return multiple tokens from here — caller handles this
        // Store pending tokens and return a sentinel; instead, use a different approach:
        // Return a special STRING token with the expanded value by evaluating immediately
        // Actually the cleanest: tokenize the backtick source and push into the caller's stream
        // Since we can't do that from here, return a placeholder and let tokenize() handle it
        // INSTEAD: we set a member flag with the pending tokens
        pendingTokens_ = subLex.tokenize();
        // Remove EOF from pending
        if (!pendingTokens_.empty() && pendingTokens_.back().type == TokenType::EOF_TOKEN)
            pendingTokens_.pop_back();
        // Return a dummy that tokenize() will replace with pendingTokens_
        return Token(TokenType::UNKNOWN, "__fstring__", startLine, startCol);
    }

    auto it = keywords.find(id);
    TokenType type = (it != keywords.end()) ? it->second : TokenType::IDENTIFIER;

    // C preprocessor macro expansion: if this identifier was #defined, substitute it
    auto dit = defines_.find(id);
    if (dit != defines_.end() && !dit->second.empty())
    {
        // Single-token macro (most common: #define ROWS 18, #define RIGHT 3)
        if (dit->second.size() == 1)
            return dit->second[0]; // return the replacement token directly
        // Multi-token macro: push all but first into pendingTokens_
        pendingTokens_.insert(pendingTokens_.begin(),
                              dit->second.begin() + 1,
                              dit->second.end());
        return dit->second[0];
    }

    return Token(type, id, startLine, startCol);
}

