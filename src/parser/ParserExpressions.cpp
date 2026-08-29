#include "Parser.h"
#include <sstream>
#include <unordered_set>
#include <cctype>

ASTNodePtr Parser::parseAssignment()
{
    int ln = current().line;
    auto left = parseOr();
    // Python inline ternary: expr IF condition ELSE other_expr
    // e.g.  high = number if number > 1 else 1
    if (check(TokenType::IF) && (pos == 0 || (tokens[pos - 1].type != TokenType::NEWLINE && tokens[pos - 1].type != TokenType::SEMICOLON)))
    {
        // Lookahead to ensure this is actually a ternary, not a list comprehension filter:
        // A ternary MUST have an 'else' somewhere before a closing bracket/paren/newline.
        bool hasElse = false;
        size_t checkPos = pos + 1;
        int depth = 0;
        while (checkPos < tokens.size())
        {
            TokenType t = tokens[checkPos].type;
            if (depth == 0 && t == TokenType::ELSE)
            {
                hasElse = true;
                break;
            }
            if (depth == 0 && (t == TokenType::LBRACE || t == TokenType::COLON || t == TokenType::INDENT || t == TokenType::NEWLINE || t == TokenType::SEMICOLON || t == TokenType::COMMA))
            {
                break;
            }
            if (t == TokenType::LPAREN || t == TokenType::LBRACKET || t == TokenType::LBRACE)
                depth++;
            else if (t == TokenType::RPAREN || t == TokenType::RBRACKET || t == TokenType::RBRACE)
            {
                if (depth == 0)
                    break;
                depth--;
            }
            checkPos++;
        }

        if (hasElse)
        {
            consume(); // eat 'if'
            auto condition = parseOr();
            expect(TokenType::ELSE, "Expected 'else' in Python ternary expression");
            auto elseExpr = parseAssignment();
            return std::make_unique<ASTNode>(TernaryExpr{std::move(condition), std::move(left), std::move(elseExpr)}, ln);
        }
        // If there's no 'else', it's likely a list comprehension filter like `[x for x in lst if x > 0]`
        // which will be handled by parseExpr/parseListComp, so we just return `left`.
    }
    // JS/C ternary: condition ? thenExpr : elseExpr
    if (check(TokenType::QUESTION))
    {
        consume();
        auto thenExpr = parseExpr();
        expect(TokenType::COLON, "Expected ':' in ternary expression");
        auto elseExpr = parseExpr();
        return std::make_unique<ASTNode>(TernaryExpr{std::move(left), std::move(thenExpr), std::move(elseExpr)}, ln);
    }

    // Tuple-unpacking assignment: a, b, c = expr
    // ONLY activate when we can confirm via non-consuming lookahead that the
    // pattern is:  IDENT , IDENT , ... IDENT =
    // This prevents false-positives inside argument lists like f(a, b).
    if (!inCallArgList && check(TokenType::COMMA) && left->is<Identifier>())
    {
        // Lookahead: scan forward to confirm all commas are followed by
        // identifiers and the sequence ends with '='
        bool isUnpack = false;
        {
            size_t scan = pos; // points at the first COMMA
            while (scan < tokens.size() && tokens[scan].type == TokenType::COMMA)
            {
                ++scan; // skip comma
                while (scan < tokens.size() && tokens[scan].type == TokenType::NEWLINE)
                    ++scan;
                // must be an identifier
                if (scan >= tokens.size() || tokens[scan].type != TokenType::IDENTIFIER)
                    break;
                ++scan; // skip identifier
                while (scan < tokens.size() && tokens[scan].type == TokenType::NEWLINE)
                    ++scan;
                // if next is '=' (not '=='), this is a tuple-unpack assignment
                if (scan < tokens.size() && tokens[scan].type == TokenType::ASSIGN)
                {
                    isUnpack = true;
                    break;
                }
                // if next is another comma, keep scanning; otherwise stop
                if (scan >= tokens.size() || tokens[scan].type != TokenType::COMMA)
                    break;
            }
        }

        if (isUnpack)
        {
            std::vector<std::string> targets;
            targets.push_back(left->as<Identifier>().name);
            while (match(TokenType::COMMA))
            {
                skipNewlines();
                if (check(TokenType::IDENTIFIER))
                    targets.push_back(consume().value);
                else
                    break;
            }
            expect(TokenType::ASSIGN, "Expected '=' in tuple unpacking");
            auto right = parseAssignment();
            TupleLiteral lhsTuple;
            for (auto &t : targets)
                lhsTuple.elements.push_back(std::make_unique<ASTNode>(Identifier{t}, ln));
            auto lhsNode = std::make_unique<ASTNode>(std::move(lhsTuple), ln);
            return std::make_unique<ASTNode>(AssignExpr{"unpack", std::move(lhsNode), std::move(right)}, ln);
        }
    }

    if (check(TokenType::ASSIGN) || check(TokenType::PLUS_ASSIGN) ||
        check(TokenType::MINUS_ASSIGN) || check(TokenType::STAR_ASSIGN) ||
        check(TokenType::SLASH_ASSIGN) || check(TokenType::AND_ASSIGN) ||
        check(TokenType::OR_ASSIGN) || check(TokenType::XOR_ASSIGN) ||
        check(TokenType::MOD_ASSIGN))
    {
        auto op = consume().value;
        auto right = parseAssignment();
        return std::make_unique<ASTNode>(AssignExpr{op, std::move(left), std::move(right)}, ln);
    }
    // >>= compound shift-right assign: lexer emits RSHIFT then ASSIGN
    if (check(TokenType::RSHIFT) && pos + 1 < tokens.size() && tokens[pos + 1].type == TokenType::ASSIGN)
    {
        consume(); // eat >>
        consume(); // eat =
        auto rightExpr = parseAssignment();
        return std::make_unique<ASTNode>(AssignExpr{">>", std::move(left), std::move(rightExpr)}, ln);
    }
    // <<= compound shift-left assign: lexer emits LSHIFT then ASSIGN
    if (check(TokenType::LSHIFT) && pos + 1 < tokens.size() && tokens[pos + 1].type == TokenType::ASSIGN)
    {
        consume(); // eat <<
        consume(); // eat =
        auto rightExpr = parseAssignment();
        return std::make_unique<ASTNode>(AssignExpr{"<<", std::move(left), std::move(rightExpr)}, ln);
    }
    return left;
}

ASTNodePtr Parser::parseOr()
{
    auto left = parseAnd();
    while (true)
    {
        size_t savedPos = pos;
        skipNewlines();
        if (!check(TokenType::OR) && !check(TokenType::OR_OR) && !check(TokenType::NULL_COALESCE))
        {
            pos = savedPos;
            break;
        }
        int ln = current().line;
        auto opToken = consume(); // eat 'or', '||', or '??'
        std::string opStr = (opToken.type == TokenType::OR_OR) ? "or" : opToken.value;
        skipNewlines();
        auto right = parseAnd();
        left = std::make_unique<ASTNode>(BinaryExpr{opStr, std::move(left), std::move(right)}, ln);
    }
    return left;
}

ASTNodePtr Parser::parseAnd()
{
    auto left = parseBitwise();
    while (true)
    {
        size_t savedPos = pos;
        skipNewlines();
        if (!check(TokenType::AND) && !check(TokenType::AND_AND))
        {
            pos = savedPos;
            break;
        }
        int ln = current().line;
        auto opToken = consume(); // eat 'and' or '&&'
        std::string opStr = (opToken.type == TokenType::AND_AND) ? "and" : opToken.value;
        skipNewlines();
        auto right = parseBitwise();
        left = std::make_unique<ASTNode>(BinaryExpr{opStr, std::move(left), std::move(right)}, ln);
    }
    return left;
}

ASTNodePtr Parser::parseBitwise()
{
    auto left = parseEquality();
    while (check(TokenType::BIT_AND) || check(TokenType::BIT_OR) || check(TokenType::BIT_XOR))
    {
        int ln = current().line;
        auto op = consume().value;
        auto right = parseEquality();
        left = std::make_unique<ASTNode>(BinaryExpr{op, std::move(left), std::move(right)}, ln);
    }
    return left;
}

ASTNodePtr Parser::parseEquality()
{
    auto left = parseComparison();
    while (check(TokenType::EQ) || check(TokenType::NEQ) || check(TokenType::STRICT_EQ) || check(TokenType::STRICT_NEQ))
    {
        int ln = current().line;
        auto op = consume();
        // Treat === as == and !== as != (Quantum is dynamically typed)
        std::string opStr = (op.type == TokenType::STRICT_EQ) ? "==" : (op.type == TokenType::STRICT_NEQ) ? "!="
                                                                                                          : op.value;
        auto right = parseComparison();
        left = std::make_unique<ASTNode>(BinaryExpr{opStr, std::move(left), std::move(right)}, ln);
    }
    return left;
}

ASTNodePtr Parser::parseComparison()
{
    auto left = parseShift();
    while (check(TokenType::LT) || check(TokenType::GT) || check(TokenType::LTE) || check(TokenType::GTE) || check(TokenType::IN) || check(TokenType::NOT) || check(TokenType::IS))
    {
        int ln = current().line;

        // 'is not' / 'is'
        if (check(TokenType::IS))
        {
            consume(); // eat 'is'
            if (match(TokenType::NOT))
            {
                auto right = parseShift();
                left = std::make_unique<ASTNode>(BinaryExpr{"is not", std::move(left), std::move(right)}, ln);
            }
            else
            {
                auto right = parseShift();
                left = std::make_unique<ASTNode>(BinaryExpr{"is", std::move(left), std::move(right)}, ln);
            }
            continue;
        }

        // 'not in' — two-token operator
        if (check(TokenType::NOT))
        {
            consume(); // eat 'not'
            if (!match(TokenType::IN))
                throw ParseError("Expected 'in' after 'not'", current().line, current().col);
            auto right = parseShift();
            left = std::make_unique<ASTNode>(BinaryExpr{"not in", std::move(left), std::move(right)}, ln);
            continue;
        }

        // 'in' — membership test
        if (check(TokenType::IN))
        {
            consume();
            auto right = parseShift();
            left = std::make_unique<ASTNode>(BinaryExpr{"in", std::move(left), std::move(right)}, ln);
            continue;
        }

        auto op = consume().value;
        auto right = parseShift();
        left = std::make_unique<ASTNode>(BinaryExpr{op, std::move(left), std::move(right)}, ln);
    }
    return left;
}

ASTNodePtr Parser::parseShift()
{
    auto left = parseAddSub();
    while (check(TokenType::LSHIFT) || check(TokenType::RSHIFT))
    {
        // Don't consume >>= or <<= here — parseAssignment handles those as compound ops
        if (pos + 1 < tokens.size() && tokens[pos + 1].type == TokenType::ASSIGN)
            break;
        int ln = current().line;
        auto op = consume().value;
        auto right = parseAddSub();
        left = std::make_unique<ASTNode>(BinaryExpr{op, std::move(left), std::move(right)}, ln);
    }
    return left;
}

ASTNodePtr Parser::parseAddSub()
{
    auto left = parseMulDiv();
    while (check(TokenType::PLUS) || check(TokenType::MINUS))
    {
        int ln = current().line;
        auto op = consume().value;
        auto right = parseMulDiv();
        left = std::make_unique<ASTNode>(BinaryExpr{op, std::move(left), std::move(right)}, ln);
    }
    return left;
}

ASTNodePtr Parser::parseMulDiv()
{
    auto left = parsePower();
    while (check(TokenType::STAR) || check(TokenType::SLASH) || check(TokenType::PERCENT) || check(TokenType::FLOOR_DIV))
    {
        int ln = current().line;
        auto op = consume().value;
        auto right = parsePower();
        left = std::make_unique<ASTNode>(BinaryExpr{op, std::move(left), std::move(right)}, ln);
    }
    return left;
}

ASTNodePtr Parser::parsePower()
{
    auto left = parseUnary();
    if (check(TokenType::POWER))
    {
        int ln = current().line;
        consume();
        auto right = parsePower(); // right-associative
        return std::make_unique<ASTNode>(BinaryExpr{"**", std::move(left), std::move(right)}, ln);
    }
    return left;
}

ASTNodePtr Parser::parseUnary()
{
    int ln = current().line;
    // Prefix ++ and --
    if (check(TokenType::PLUS_PLUS))
    {
        consume();
        auto operand = parseUnary();
        auto one = std::make_unique<ASTNode>(NumberLiteral{1.0}, ln);
        return std::make_unique<ASTNode>(AssignExpr{"+=", std::move(operand), std::move(one)}, ln);
    }
    if (check(TokenType::MINUS_MINUS))
    {
        consume();
        auto operand = parseUnary();
        auto one = std::make_unique<ASTNode>(NumberLiteral{1.0}, ln);
        return std::make_unique<ASTNode>(AssignExpr{"-=", std::move(operand), std::move(one)}, ln);
    }
    if (check(TokenType::MINUS))
    {
        consume();
        return std::make_unique<ASTNode>(UnaryExpr{"-", parseUnary()}, ln);
    }
    // Unary + is a no-op: +1 == 1, +x == x
    if (check(TokenType::PLUS))
    {
        consume();
        return parseUnary(); // discard the +
    }
    if (check(TokenType::NOT))
    {
        consume();
        return std::make_unique<ASTNode>(UnaryExpr{"not", parseUnary()}, ln);
    }
    if (check(TokenType::BIT_NOT))
    {
        consume();
        return std::make_unique<ASTNode>(UnaryExpr{"~", parseUnary()}, ln);
    }
    // C-style address-of: &var → AddressOfExpr
    if (check(TokenType::BIT_AND))
    {
        consume();
        int ln2 = current().line;
        if (check(TokenType::IDENTIFIER) && current().value.find("::") != std::string::npos)
        {
            std::string val = consume().value;
            size_t pos = val.rfind("::");
            std::string member = val.substr(pos + 2);
            return std::make_unique<ASTNode>(StringLiteral{member}, ln2);
        }
        auto operand = parseUnary();
        return std::make_unique<ASTNode>(AddressOfExpr{std::move(operand)}, ln2);
    }
    // C-style dereference: *ptr → DerefExpr
    if (check(TokenType::STAR))
    {
        consume();
        int ln2 = current().line;
        auto operand = parseUnary();
        return std::make_unique<ASTNode>(DerefExpr{std::move(operand)}, ln2);
    }
    return parsePostfix();
}

ASTNodePtr Parser::parsePostfix()
{
    auto expr = parsePrimary();
    while (true)
    {
        // Peek past newlines to support method chaining across lines:
        //   obj
        //     .filter(...)
        //     .map(...)
        size_t savedPos = pos;
        while (check(TokenType::NEWLINE))
            consume();

        int ln = current().line; // declared here so it's in scope for all branches below

        // C++ value-initialization: V{} → V()  (only in expression position:
        // the token after '}' must continue/terminate an expression, so an
        // empty block after an identifier is never misread as a call)
        if (check(TokenType::LBRACE) && expr->is<Identifier>() &&
            pos + 1 < tokens.size() && tokens[pos + 1].type == TokenType::RBRACE &&
            pos + 2 < tokens.size() &&
            (tokens[pos + 2].type == TokenType::SEMICOLON ||
             tokens[pos + 2].type == TokenType::RPAREN ||
             tokens[pos + 2].type == TokenType::COMMA))
        {
            consume(); // eat {
            consume(); // eat }
            expr = std::make_unique<ASTNode>(CallExpr{std::move(expr), std::vector<ASTNodePtr>{}}, ln);
            continue;
        }

        if (!check(TokenType::DOT) && !check(TokenType::ARROW) && !check(TokenType::LBRACKET) && !check(TokenType::LPAREN) && !check(TokenType::PLUS_PLUS) && !check(TokenType::MINUS_MINUS))
        {
            // Handle C++ scope resolution: ClassName::member or ClassName::method(args)
            // A number after '::' can never be scope access — leave the colons
            // alone so Python slices like a[i::2] parse as slices.
            if (check(TokenType::COLON) && pos + 1 < tokens.size() && tokens[pos + 1].type == TokenType::COLON &&
                !(pos + 2 < tokens.size() && tokens[pos + 2].type == TokenType::NUMBER))
            {
                consume(); // eat first :
                consume(); // eat second :
                std::string mem;
                if (!atEnd() && current().type != TokenType::NEWLINE && current().type != TokenType::SEMICOLON)
                {
                    mem = consume().value;
                    while (!atEnd() && (check(TokenType::IDENTIFIER) || check(TokenType::THIS) || isCTypeKeyword(current().type)))
                        mem += consume().value;
                }
                if (check(TokenType::LPAREN))
                {
                    auto args = parseArgList();
                    // Wrap as method call on the object
                    auto memExpr = std::make_unique<ASTNode>(MemberExpr{std::move(expr), mem}, ln);
                    expr = std::make_unique<ASTNode>(CallExpr{std::move(memExpr), std::move(args)}, ln);
                }
                else
                {
                    expr = std::make_unique<ASTNode>(MemberExpr{std::move(expr), mem}, ln);
                }
                continue;
            }
            // Check for C++ template call: identifier<Type>(args) e.g. make_unique<int[]>(n)
            // Only attempt if current expr is a simple identifier and next token is LESS
            if (check(TokenType::LT) && expr->is<Identifier>())
            {
                // Speculatively consume <...> — if followed by '(' it's a template call
                size_t preTpl = pos;
                consume(); // eat '<'
                int tdepth = 1;
                bool ok = false;
                while (!atEnd() && tdepth > 0)
                {
                    if (check(TokenType::LT))
                        tdepth++;
                    else if (check(TokenType::GT))
                    {
                        tdepth--;
                        if (tdepth == 0)
                        {
                            consume();
                            ok = true;
                            break;
                        }
                    }
                    else if (check(TokenType::RSHIFT))
                    {
                        tdepth -= 2;
                        consume();
                        if (tdepth <= 0)
                        {
                            ok = true;
                            break;
                        }
                        continue;
                    }
                    else if (check(TokenType::LPAREN) || check(TokenType::SEMICOLON) || check(TokenType::NEWLINE) || check(TokenType::EOF_TOKEN))
                        break;
                    consume();
                }
                if (ok && check(TokenType::LPAREN))
                {
                    // It's a template call — parse args and build CallExpr
                    auto args = parseArgList();
                    expr = std::make_unique<ASTNode>(CallExpr{std::move(expr), std::move(args)}, ln);
                    continue;
                }
                if (ok && check(TokenType::COLON) && pos + 1 < tokens.size() &&
                    tokens[pos + 1].type == TokenType::COLON)
                {
                    // Templated type followed by scope access: numeric_limits<T>::max()
                    // Template args are erased; loop again so the '::' member handler
                    // above turns this into numeric_limits.max().
                    continue;
                }
                // Not a template call — restore position and break
                pos = preTpl;
            }
            pos = savedPos; // restore so newline terminates the statement
            break;
        }

        if (check(TokenType::PLUS_PLUS))
        {
            consume();
            auto one = std::make_unique<ASTNode>(NumberLiteral{1.0}, ln);
            expr = std::make_unique<ASTNode>(AssignExpr{"post+=", std::move(expr), std::move(one)}, ln);
        }
        else if (check(TokenType::MINUS_MINUS))
        {
            consume();
            auto one = std::make_unique<ASTNode>(NumberLiteral{1.0}, ln);
            expr = std::make_unique<ASTNode>(AssignExpr{"post-=", std::move(expr), std::move(one)}, ln);
        }
        else if (check(TokenType::LPAREN))
        {
            if (savedPos != pos)
            {
                pos = savedPos; // newline separates statement from '('
                break;
            }
            auto args = parseArgList();
            expr = std::make_unique<ASTNode>(CallExpr{std::move(expr), std::move(args)}, ln);
        }
        else if (check(TokenType::LBRACKET))
        {
            consume(); // eat '['
            // Detect slice: [start:stop:step] — any part optional
            // A leading ':' means start is omitted
            bool isSlice = false;
            ASTNodePtr idxOrStart;

            if (check(TokenType::COLON))
                isSlice = true; // [:...]  — start omitted
            else
            {
                idxOrStart = parseExpr();
                if (check(TokenType::COLON))
                    isSlice = true; // [expr:...]
            }

            if (isSlice)
            {
                // consume the first ':'
                consume();
                ASTNodePtr stop, step;
                if (!check(TokenType::RBRACKET) && !check(TokenType::COLON))
                    stop = parseExpr();
                if (match(TokenType::COLON))
                {
                    if (!check(TokenType::RBRACKET))
                        step = parseExpr();
                }
                expect(TokenType::RBRACKET, "Expected ']'");
                SliceExpr sl;
                sl.object = std::move(expr);
                sl.start = std::move(idxOrStart);
                sl.stop = std::move(stop);
                sl.step = std::move(step);
                expr = std::make_unique<ASTNode>(std::move(sl), ln);
            }
            else
            {
                expect(TokenType::RBRACKET, "Expected ']'");
                expr = std::make_unique<ASTNode>(IndexExpr{std::move(expr), std::move(idxOrStart)}, ln);
            }
        }
        else if (check(TokenType::ARROW))
        {
            consume(); // eat ->
            if (check(TokenType::STAR))
            {
                consume(); // eat *
                auto ptrExpr = parseUnary();
                auto deref = std::make_unique<ASTNode>(DerefExpr{std::move(expr)}, ln);
                expr = std::make_unique<ASTNode>(IndexExpr{std::move(deref), std::move(ptrExpr)}, ln);
                continue;
            }
            std::string mem;
            if (check(TokenType::IDENTIFIER))
                mem = consume().value;
            else if (isCTypeKeyword(current().type))
                mem = consume().value;
            else
                mem = expect(TokenType::IDENTIFIER, "Expected member name after ->").value;
            if (check(TokenType::LPAREN))
            {
                // ptr->method(args) — a method call through `->`. Use the
                // tolerant ArrowExpr access (Op::ARROW derefs a real pointer
                // but passes a plain object/array through), so `->` works as a
                // general member-access operator the way these polyglot files
                // use it (`node.left`, `node->left`, `arr[0]->size()`), not
                // only on genuine pointers. compileCall binds the receiver.
                auto arrowExpr = std::make_unique<ASTNode>(ArrowExpr{std::move(expr), mem}, ln);
                auto args = parseArgList();
                expr = std::make_unique<ASTNode>(CallExpr{std::move(arrowExpr), std::move(args)}, ln);
            }
            else
            {
                expr = std::make_unique<ASTNode>(ArrowExpr{std::move(expr), mem}, ln);
            }
        }
        else if (check(TokenType::DOT))
        {
            consume();
            if (check(TokenType::STAR))
            {
                consume(); // eat *
                auto ptrExpr = parseUnary();
                expr = std::make_unique<ASTNode>(IndexExpr{std::move(expr), std::move(ptrExpr)}, ln);
                continue;
            }
            // Accept ANY token as member name: from, import, print, length, etc.
            std::string mem;
            if (!atEnd() &&
                current().type != TokenType::NEWLINE &&
                current().type != TokenType::SEMICOLON &&
                current().type != TokenType::EOF_TOKEN &&
                current().type != TokenType::LPAREN &&
                current().type != TokenType::LBRACKET &&
                current().type != TokenType::LBRACE &&
                current().type != TokenType::DOT)
            {
                mem = consume().value;
            }
            else
            {
                mem = expect(TokenType::IDENTIFIER, "Expected member name").value;
            }

            if (check(TokenType::LPAREN))
            {
                auto memExpr = std::make_unique<ASTNode>(MemberExpr{std::move(expr), mem}, ln);
                auto args = parseArgList();
                expr = std::make_unique<ASTNode>(CallExpr{std::move(memExpr), std::move(args)}, ln);
            }
            else
            {
                expr = std::make_unique<ASTNode>(MemberExpr{std::move(expr), mem}, ln);
            }
        }
        else
            break;
    }
    return expr;
}

