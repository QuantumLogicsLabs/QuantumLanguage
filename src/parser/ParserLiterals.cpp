#include "Parser.h"
#include <sstream>
#include <unordered_set>
#include <cctype>

ASTNodePtr Parser::parsePrimary()
{
    int ln = current().line;
    auto &tok = current();

    if (tok.type == TokenType::NUMBER)
    {
        double v;
        if (tok.value.size() > 1 && tok.value[1] == 'x')
            v = (double)std::stoull(tok.value, nullptr, 16);
        else
            v = std::stod(tok.value);
        consume();
        if (check(TokenType::IDENTIFIER) &&
            (current().value == "f" || current().value == "F" ||
             current().value == "l" || current().value == "L" ||
             current().value == "d" || current().value == "D" ||
             current().value == "u" || current().value == "U"))
        {
            consume(); // skip C/C++ float suffixes
        }
        return std::make_unique<ASTNode>(NumberLiteral{v}, ln);
    }
    if (tok.type == TokenType::STRING)
    {
        auto s = tok.value;
        consume();
        return std::make_unique<ASTNode>(StringLiteral{s}, ln);
    }
    if (tok.type == TokenType::BOOL_TRUE)
    {
        consume();
        return std::make_unique<ASTNode>(BoolLiteral{true}, ln);
    }
    if (tok.type == TokenType::BOOL_FALSE)
    {
        consume();
        return std::make_unique<ASTNode>(BoolLiteral{false}, ln);
    }
    if (tok.type == TokenType::NIL)
    {
        consume();
        return std::make_unique<ASTNode>(NilLiteral{}, ln);
    }

    // this / self → Identifier{"self"}
    if (tok.type == TokenType::THIS)
    {
        consume();
        return std::make_unique<ASTNode>(Identifier{"self"}, ln);
    }

    // Python lambda: lambda x, y: expr
    // Only when a ':' follows on the same line — otherwise "lambda" stays a
    // plain identifier so variables named lambda keep working.
    if (tok.type == TokenType::IDENTIFIER && tok.value == "lambda")
    {
        size_t la = pos + 1;
        bool sawColon = false;
        while (la < tokens.size() && tokens[la].type != TokenType::NEWLINE &&
               tokens[la].type != TokenType::EOF_TOKEN)
        {
            if (tokens[la].type == TokenType::COLON)
            {
                sawColon = true;
                break;
            }
            if (tokens[la].type != TokenType::IDENTIFIER &&
                tokens[la].type != TokenType::COMMA)
                break; // params can only be names and commas
            la++;
        }
        if (sawColon)
        {
            consume(); // eat 'lambda'
            std::vector<std::string> params;
            while (!check(TokenType::COLON) && !atEnd())
            {
                if (check(TokenType::IDENTIFIER))
                    params.push_back(consume().value);
                else if (!match(TokenType::COMMA))
                    break;
            }
            expect(TokenType::COLON, "Expected ':' in lambda");
            auto bodyExpr = parseAssignment(); // single expression body
            int eln = bodyExpr->line;
            auto retStmt = std::make_unique<ASTNode>(ReturnStmt{std::move(bodyExpr)}, eln);
            BlockStmt block;
            block.statements.push_back(std::move(retStmt));
            LambdaExpr le;
            le.params = std::move(params);
            le.body = std::make_unique<ASTNode>(std::move(block), ln);
            return std::make_unique<ASTNode>(std::move(le), ln);
        }
    }

    // new int(100) / new ClassName(args) / new int[n]
    if (tok.type == TokenType::NEW)
    {
        consume();
        // Accept both identifier class names AND C++ primitive type keywords
        std::string name;
        if (check(TokenType::IDENTIFIER))
            name = consume().value;
        else if (isCTypeKeyword(current().type))
            name = consume().value;
        else
            throw ParseError("Expected type name after 'new'", current().line, current().col);

        // new char *[n] — pointer-to-type array: skip any STAR tokens after the type name
        // e.g. "new char *[size]" → treat as array of pointers (same as new char[size] at runtime)
        while (check(TokenType::STAR))
            consume(); // discard pointer qualifier

        // new float[n] — array allocation: allocate array of n default-zero elements
        if (check(TokenType::LBRACKET))
        {
            consume(); // eat '['
            auto sizeNode = parseExpr();
            expect(TokenType::RBRACKET, "Expected ']'");
            NewExpr ne;
            ne.typeName = name;
            ne.isArray = true;
            ne.sizeExpr = std::move(sizeNode);
            return std::make_unique<ASTNode>(std::move(ne), ln);
        }

        // new Node{val, nullptr} — C++ brace-initializer constructor arguments
        if (check(TokenType::LBRACE))
        {
            consume(); // eat '{'
            skipNewlines();
            NewExpr ne;
            ne.typeName = name;
            while (!check(TokenType::RBRACE) && !atEnd())
            {
                ne.args.push_back(parseExpr());
                skipNewlines();
                if (!match(TokenType::COMMA))
                    break;
                skipNewlines();
            }
            expect(TokenType::RBRACE, "Expected '}'");
            return std::make_unique<ASTNode>(std::move(ne), ln);
        }

        // new int(100) / new ClassName(args) — heap allocation, always returns a pointer
        auto argNodes = parseArgList();
        NewExpr ne;
        ne.typeName = name;
        for (auto &a : argNodes)
            ne.args.push_back(std::move(a));
        return std::make_unique<ASTNode>(std::move(ne), ln);
    }

    // super → super() or super.method()
    if (tok.type == TokenType::SUPER)
    {
        consume();
        if (check(TokenType::DOT))
        {
            consume(); // eat '.'
            auto method = expect(TokenType::IDENTIFIER, "Expected method name after 'super.'").value;
            return std::make_unique<ASTNode>(SuperExpr{method}, ln);
        }
        // bare super — used as super(args) which becomes a CallExpr wrapping SuperExpr
        return std::make_unique<ASTNode>(SuperExpr{""}, ln);
    }

    if (tok.type == TokenType::LBRACKET)
    {
        // C++ lambda: [captures](params) { body }
        // Detect by scanning: matching ']' then '(' ... ')' then '{'.
        // The capture list is discarded — Quantum closures capture automatically.
        size_t la = pos + 1;
        int bdepth = 1;
        while (la < tokens.size() && bdepth > 0)
        {
            if (tokens[la].type == TokenType::LBRACKET)
                bdepth++;
            else if (tokens[la].type == TokenType::RBRACKET)
                bdepth--;
            la++;
        }
        if (bdepth == 0 && la < tokens.size() && tokens[la].type == TokenType::LPAREN)
        {
            size_t la2 = la + 1;
            int pdepth = 1;
            while (la2 < tokens.size() && pdepth > 0)
            {
                if (tokens[la2].type == TokenType::LPAREN)
                    pdepth++;
                else if (tokens[la2].type == TokenType::RPAREN)
                    pdepth--;
                la2++;
            }
            while (la2 < tokens.size() && tokens[la2].type == TokenType::NEWLINE)
                la2++;
            if (pdepth == 0 && la2 < tokens.size() && tokens[la2].type == TokenType::LBRACE)
            {
                int ln2 = tok.line;
                while (pos < la)
                    consume(); // discard '[captures]'
                std::vector<ASTNodePtr> defaultArgs;
                std::vector<std::string> paramTypes;
                auto params = parseParamList(nullptr, &defaultArgs, &paramTypes);
                skipNewlines();
                auto body = parseBlock();
                LambdaExpr le;
                le.params = std::move(params);
                le.paramTypes = std::move(paramTypes);
                le.defaultArgs = std::move(defaultArgs);
                le.body = std::move(body);
                return std::make_unique<ASTNode>(std::move(le), ln2);
            }
        }
        return parseArrayLiteral();
    }
    if (tok.type == TokenType::LBRACE)
        return parseDictLiteral();

    if (tok.type == TokenType::FN || tok.type == TokenType::FUNCTION || tok.type == TokenType::DEF)
    {
        consume();
        return parseLambda();
    }

    if (tok.type == TokenType::LPAREN)
    {
        int ln = tok.line;
        consume();
        skipNewlines();

        // Check for arrow function: () => or (x, y) => or ([a,b]) =>
        bool isArrow = false;
        {
            size_t p = pos;
            int depth = 1;
            while (p < tokens.size() && depth > 0)
            {
                if (tokens[p].type == TokenType::LPAREN)
                    depth++;
                else if (tokens[p].type == TokenType::RPAREN)
                    depth--;
                p++;
            }
            if (depth == 0)
            {
                while (p < tokens.size() && tokens[p].type == TokenType::NEWLINE)
                    p++;
                if (p < tokens.size() && tokens[p].type == TokenType::FAT_ARROW)
                    isArrow = true;
            }
        }

        if (isArrow)
        {
            std::vector<std::string> arrowParams;
            while (!check(TokenType::RPAREN) && !atEnd())
            {
                if (isCTypeKeyword(current().type))
                    consume(); // skip type hints

                if (check(TokenType::LBRACKET))
                {
                    std::string paramName = "[";
                    consume(); // '['
                    while (!check(TokenType::RBRACKET) && !atEnd())
                    {
                        if (check(TokenType::IDENTIFIER))
                            paramName += consume().value;
                        if (match(TokenType::COMMA))
                            paramName += ",";
                        skipNewlines();
                    }
                    expect(TokenType::RBRACKET, "Expected ']'");
                    paramName += "]";
                    arrowParams.push_back(paramName);
                }
                else if (check(TokenType::IDENTIFIER))
                {
                    arrowParams.push_back(consume().value);
                }
                match(TokenType::COMMA);
                skipNewlines();
            }
            expect(TokenType::RPAREN, "Expected ')'");
            return parseArrowFunction(std::move(arrowParams), ln);
        }

        // Normal parenthesised expression (or tuple literal)
        // First, speculatively check for (TypeName *) or (TypeName*) C-style pointer casts
        // e.g. (Node *)(arena + arena_used), (MyClass *)ptr
        {
            size_t p = pos;
            // Skip optional 'struct'/'const'
            if (p < tokens.size() && tokens[p].type == TokenType::IDENTIFIER &&
                (tokens[p].value == "struct" || tokens[p].value == "const" || tokens[p].value == "unsigned"))
                p++;
            if (p < tokens.size() && tokens[p].type == TokenType::IDENTIFIER)
            {
                size_t p2 = p + 1;
                // Optional pointer star(s)
                while (p2 < tokens.size() && tokens[p2].type == TokenType::STAR)
                    p2++;
                // Also handle const after star: (Node * const)
                if (p2 < tokens.size() && tokens[p2].type == TokenType::CONST)
                    p2++;
                // Must be followed by ) and then a value-producing token
                if (p2 < tokens.size() && tokens[p2].type == TokenType::RPAREN)
                {
                    size_t after = p2 + 1;
                    while (after < tokens.size() && tokens[after].type == TokenType::NEWLINE)
                        after++;
                    bool valueFollows = after < tokens.size() &&
                                        tokens[after].type != TokenType::SEMICOLON &&
                                        tokens[after].type != TokenType::EOF_TOKEN &&
                                        tokens[after].type != TokenType::RBRACE &&
                                        tokens[after].type != TokenType::RBRACKET &&
                                        tokens[after].type != TokenType::COMMA;
                    // Distinguish from plain grouped expr: (Node) vs (Node *) — only fire if star present OR known type
                    bool hasStar = (p2 > p + 1); // at least one star was consumed
                    if (hasStar && valueFollows)
                    {
                        std::string castType = tokens[p].value;
                        // It's a pointer cast: skip type name, stars, optional const, and ')'
                        pos = p2 + 1; // jump past ')'
                        skipNewlines();
                        auto innerExpr = parseUnary();

                        // If it's a cast to a struct/class pointer (not a primitive), simulate malloc by creating a new instance
                        if (castType != "void" && castType != "char" && castType != "int" && castType != "float" &&
                            castType != "double" && castType != "long" && castType != "short" && castType != "unsigned")
                        {
                            // Generate ast: ClassName()
                            int castLn = tokens[p].line;
                            auto typeId = std::make_unique<ASTNode>(Identifier{castType}, castLn);
                            std::vector<ASTNodePtr> noArgs;
                            return std::make_unique<ASTNode>(CallExpr{std::move(typeId), std::move(noArgs)}, castLn);
                        }
                        return innerExpr;
                    }
                }
            }
        }
        bool prevInCallArgList = inCallArgList;
        inCallArgList = true;
        auto expr = parseExpr();
        inCallArgList = prevInCallArgList;
        skipNewlines();

        // ── C-style type cast: (int)x  (unsigned)time(NULL)  (char)c ───────
        // Also handles multi-word casts: (long long)x  (unsigned int)x
        // Detected when the expression inside parens was a single C type keyword
        // (parsed as an Identifier) AND the next token is NOT ')' — meaning the
        // ')' already closed the cast and the operand follows outside.
        // We simply discard the cast type and parse+return the operand.
        if (check(TokenType::RPAREN) && expr->is<Identifier>())
        {
            const std::string &idName = expr->as<Identifier>().name;
            // Known C type names used in casts (all lowercase — distinct from compound literals)
            static const std::unordered_set<std::string> cCastTypes = {
                "int", "unsigned", "long", "short", "char", "float", "double",
                "bool", "void", "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
                "int8_t", "int16_t", "int32_t", "int64_t", "uintptr_t", "ptrdiff_t"};
            if (cCastTypes.count(idName))
            {
                // Peek: if after ')' there is a value-producing token (not end/newline/;),
                // this is a cast — eat ')' and parse the cast operand.
                size_t peekPos = pos + 1;
                while (peekPos < tokens.size() && tokens[peekPos].type == TokenType::NEWLINE)
                    ++peekPos;
                if (peekPos < tokens.size() &&
                    tokens[peekPos].type != TokenType::SEMICOLON &&
                    tokens[peekPos].type != TokenType::EOF_TOKEN &&
                    tokens[peekPos].type != TokenType::RBRACE &&
                    tokens[peekPos].type != TokenType::RBRACKET &&
                    tokens[peekPos].type != TokenType::RPAREN &&
                    tokens[peekPos].type != TokenType::COMMA)
                {
                    consume(); // eat ')'
                    skipNewlines();
                    // Parse the operand (unary precedence — handles *ptr, &var, -x, etc.)
                    return parseUnary();
                }
            }
        }
        // Multi-word C-style cast: (long long)x  (unsigned int)x — inner expr is BinaryExpr or similar
        // Detect: inside parens we see TYPE TYPE (two type keywords) — these were lexed as separate
        // identifier/type tokens and combined by the expression parser. Check if expr is a BinaryExpr
        // or if we have a trailing type keyword before ')'.
        // Handle: (long long) — first token was 'long' (identifier), second is 'long' (identifier)
        // The parser sees 'long' as Identifier, then space, then 'long' which is another identifier.
        // Actually 'long long' is parsed as two identifier tokens which can't be a valid expr,
        // so we also handle the case where current() is a C-type keyword before ')'.
        if (isCTypeKeyword(current().type))
        {
            // consume trailing type keyword(s) (e.g. 'long' in '(long long)')
            while (isCTypeKeyword(current().type))
                consume();
            if (check(TokenType::STAR))
                consume(); // pointer qualifier in cast
            if (check(TokenType::RPAREN))
            {
                size_t peekPos = pos + 1;
                while (peekPos < tokens.size() && tokens[peekPos].type == TokenType::NEWLINE)
                    ++peekPos;
                if (peekPos < tokens.size() &&
                    tokens[peekPos].type != TokenType::SEMICOLON &&
                    tokens[peekPos].type != TokenType::EOF_TOKEN &&
                    tokens[peekPos].type != TokenType::RBRACE &&
                    tokens[peekPos].type != TokenType::COMMA)
                {
                    consume(); // eat ')'
                    skipNewlines();
                    return parseUnary();
                }
            }
        }

        // ── C compound literal: (TypeName){field, field, ...} ──────────────
        // Only fire when the identifier looks like a type name (starts uppercase,
        // e.g. Room, Entity, Point). This prevents mis-firing on normal control-flow
        // conditions like while(cond) { } or if(flag) { }.
        if (check(TokenType::RPAREN) && expr->is<Identifier>())
        {
            const std::string &idName = expr->as<Identifier>().name;
            bool looksLikeType = !idName.empty() && std::isupper((unsigned char)idName[0]);

            if (looksLikeType)
            {
                // peek: is the token after ')' a '{'?
                size_t peekPos = pos + 1;
                while (peekPos < tokens.size() && tokens[peekPos].type == TokenType::NEWLINE)
                    ++peekPos;
                if (peekPos < tokens.size() && tokens[peekPos].type == TokenType::LBRACE)
                {
                    consume(); // eat ')'  — discard the cast type
                    skipNewlines();
                    int bln = current().line;
                    expect(TokenType::LBRACE, "Expected '{' in compound literal");
                    skipNewlines();
                    ArrayLiteral arr;
                    while (!check(TokenType::RBRACE) && !atEnd())
                    {
                        arr.elements.push_back(parseExpr());
                        skipNewlines();
                        if (!match(TokenType::COMMA))
                            break;
                        skipNewlines();
                    }
                    expect(TokenType::RBRACE, "Expected '}' in compound literal");
                    return std::make_unique<ASTNode>(std::move(arr), bln);
                }
            }
        }

        // Tuple literal: (a, b, c)  — comma after first expr
        if (check(TokenType::COMMA))
        {
            TupleLiteral tup;
            tup.elements.push_back(std::move(expr));
            while (match(TokenType::COMMA))
            {
                skipNewlines();
                if (check(TokenType::RPAREN))
                    break; // trailing comma
                tup.elements.push_back(parseExpr());
                skipNewlines();
            }
            expect(TokenType::RPAREN, "Expected ')'");
            return std::make_unique<ASTNode>(std::move(tup), ln);
        }

        // Generator expression: (expr for var in iterable)
        if (check(TokenType::FOR))
        {
            consume();
            std::vector<std::string> vars;
            auto readVar = [&]()
            {
                if (check(TokenType::IDENTIFIER))
                    vars.push_back(consume().value);
                else if (isCTypeKeyword(current().type))
                    vars.push_back(consume().value);
                else
                    vars.push_back(expect(TokenType::IDENTIFIER, "Expected variable").value);
            };
            readVar();
            while (match(TokenType::COMMA))
                readVar();
            if (!match(TokenType::IN) && !match(TokenType::OF))
                throw ParseError("Expected 'in' in generator expression", current().line, current().col);
            auto iterable = parseExpr();
            skipNewlines();
            ASTNodePtr condition;
            if (check(TokenType::IF))
            {
                consume();
                condition = parseExpr();
                skipNewlines();
            }
            ListComp lc;
            lc.expr = std::move(expr);
            lc.vars = std::move(vars);
            lc.iterable = std::move(iterable);
            lc.condition = std::move(condition);
            expect(TokenType::RPAREN, "Expected ')'");
            return std::make_unique<ASTNode>(std::move(lc), ln);
        }

        expect(TokenType::RPAREN, "Expected ')'");
        return expr;
    }

    // Spread operator: ...expr
    if (tok.type == TokenType::IDENTIFIER && tok.value == "...")
    {
        consume(); // eat "..."
        auto operand = parseUnary();
        return std::make_unique<ASTNode>(UnaryExpr{"...", std::move(operand)}, ln);
    }

    // Single-param arrow without parens: x => expr
    if (tok.type == TokenType::IDENTIFIER)
    {
        // peek ahead for =>
        size_t j = pos + 1;
        while (j < tokens.size() && tokens[j].type == TokenType::NEWLINE)
            ++j;
        if (j < tokens.size() && tokens[j].type == TokenType::FAT_ARROW)
        {
            std::string paramName = tok.value;
            consume(); // eat identifier
            return parseArrowFunction({paramName}, tok.line);
        }
        auto name = tok.value;
        consume();

        // Glue C++ scope access into the identifier: Class::member.
        // Only when '::' is followed by an identifier — otherwise leave the
        // colons alone so Python slices like a[i::2] still parse.
        while (check(TokenType::COLON))
        {
            size_t la = pos + 1;
            if (la + 1 < tokens.size() && tokens[la].type == TokenType::COLON &&
                tokens[la + 1].type == TokenType::IDENTIFIER)
            {
                consume(); // colon
                consume(); // colon
                name += "::" + consume().value;
            }
            else
            {
                break;
            }
        }

        return std::make_unique<ASTNode>(Identifier{name}, ln);
    }

    // C-type keywords used as variable names (e.g. "string = 'hello'", "double = 3.14")
    if (isCTypeKeyword(tok.type))
    {
        auto name = tok.value;
        consume();

        while (check(TokenType::COLON))
        {
            size_t la = pos + 1;
            if (la + 1 < tokens.size() && tokens[la].type == TokenType::COLON &&
                tokens[la + 1].type == TokenType::IDENTIFIER)
            {
                consume(); // colon
                consume(); // colon
                name += "::" + consume().value;
            }
            else
            {
                break;
            }
        }

        return std::make_unique<ASTNode>(Identifier{name}, ln);
    }

    // Built-in keyword tokens that can appear as callable expressions in rhs context.
    // e.g.  x = input("prompt")   result = print(...)   val = len(arr)
    // We treat them as identifiers so parsePostfix can handle the call.
    switch (tok.type)
    {
    case TokenType::INPUT:
    case TokenType::PRINT:
    case TokenType::IMPORT:
    case TokenType::COUT:
    case TokenType::CIN:
    {
        auto name = tok.value;
        consume();
        return std::make_unique<ASTNode>(Identifier{name}, ln);
    }
    default:
        break;
    }

    throw ParseError("Unexpected token: '" + tok.value + "'", tok.line, tok.col);
}

ASTNodePtr Parser::parseArrayLiteral()
{
    int ln = current().line;
    expect(TokenType::LBRACKET, "Expected '['");
    skipNewlines();

    // Empty array
    if (check(TokenType::RBRACKET))
    {
        consume();
        return std::make_unique<ASTNode>(ArrayLiteral{}, ln);
    }

    // Parse first expression — then decide if it's a list comprehension
    auto firstExpr = parseExpr();
    skipNewlines();

    // List comprehension: [expr for var in iterable (if cond)?]
    if (check(TokenType::FOR))
    {
        consume(); // eat 'for'
        // Collect loop variable(s) — support tuple unpacking: for k, v in ...
        std::vector<std::string> vars;
        auto readVar = [&]()
        {
            if (check(TokenType::IDENTIFIER))
                vars.push_back(consume().value);
            else if (isCTypeKeyword(current().type))
                vars.push_back(consume().value);
            else
                vars.push_back(expect(TokenType::IDENTIFIER, "Expected variable in comprehension").value);
        };
        readVar();
        while (match(TokenType::COMMA))
            readVar();

        if (!match(TokenType::IN) && !match(TokenType::OF))
            throw ParseError("Expected 'in' in list comprehension", current().line, current().col);

        auto iterable = parseExpr();
        skipNewlines();

        // Optional filter: if condition
        ASTNodePtr condition;
        if (check(TokenType::IF))
        {
            consume();
            condition = parseExpr();
            skipNewlines();
        }

        expect(TokenType::RBRACKET, "Expected ']'");
        ListComp lc;
        lc.expr = std::move(firstExpr);
        lc.vars = std::move(vars);
        lc.iterable = std::move(iterable);
        lc.condition = std::move(condition);
        return std::make_unique<ASTNode>(std::move(lc), ln);
    }

    // Regular array literal
    ArrayLiteral arr;
    arr.elements.push_back(std::move(firstExpr));
    skipNewlines();
    while (match(TokenType::COMMA))
    {
        skipNewlines();
        if (check(TokenType::RBRACKET))
            break; // trailing comma
        arr.elements.push_back(parseExpr());
        skipNewlines();
    }
    expect(TokenType::RBRACKET, "Expected ']'");
    return std::make_unique<ASTNode>(std::move(arr), ln);
}

ASTNodePtr Parser::parseDictLiteral()
{
    int ln = current().line;
    expect(TokenType::LBRACE, "Expected '{'");
    skipNewlines();

    // C++ brace-initializer list: {"apple", "banana"}, {1, 2, 3}, or nested
    // {{"a", x}, {"b", y}} — a literal first element followed by ',' or '}'
    // (or an immediately nested '{') can't be a dict; parse as an array.
    bool braceInitList = false;
    if (check(TokenType::LBRACE))
        braceInitList = true; // nested init list → array of arrays
    else if (check(TokenType::STRING) || check(TokenType::NUMBER))
    {
        size_t la = pos + 1;
        while (la < tokens.size() && tokens[la].type == TokenType::NEWLINE)
            la++;
        if (la < tokens.size() && (tokens[la].type == TokenType::COMMA ||
                                   tokens[la].type == TokenType::RBRACE))
            braceInitList = true;
    }
    {
        if (braceInitList)
        {
            ArrayLiteral arr;
            while (!check(TokenType::RBRACE) && !atEnd())
            {
                arr.elements.push_back(parseExpr());
                skipNewlines();
                if (!match(TokenType::COMMA))
                    break;
                skipNewlines();
            }
            skipNewlines();
            expect(TokenType::RBRACE, "Expected '}'");
            return std::make_unique<ASTNode>(std::move(arr), ln);
        }
    }

    DictLiteral dict;
    while (!check(TokenType::RBRACE) && !atEnd())
    {
        // Spread: {...obj, key: val}
        if (check(TokenType::IDENTIFIER) && current().value == "...")
        {
            consume(); // eat "..."
            auto spreadExpr = parseUnary();
            // We'll use a special sentinel: key = nullptr means spread
            dict.pairs.emplace_back(nullptr, std::move(spreadExpr));
            skipNewlines();
            if (!match(TokenType::COMMA))
                break;
            skipNewlines();
            if (check(TokenType::RBRACE))
                break;
            continue;
        }
        // Key: accept quoted string, number, bare identifier, or type keyword
        // e.g.  "name": ...   or   firstName: ...   or   42: ...
        ASTNodePtr key;
        bool isShorthand = false;
        if (check(TokenType::IDENTIFIER) || isCTypeKeyword(current().type) || check(TokenType::TYPE_STRING))
        {
            // Peek ahead — if next token after this is COLON, treat as bare string key
            size_t la = pos + 1;
            while (la < tokens.size() && tokens[la].type == TokenType::NEWLINE)
                la++;

            if (la < tokens.size() && tokens[la].type == TokenType::COLON)
            {
                // Bare identifier key: firstName → StringLiteral "firstName"
                auto keyName = consume().value;
                key = std::make_unique<ASTNode>(StringLiteral{keyName}, ln);
            }
            else if (la < tokens.size() && (tokens[la].type == TokenType::COMMA || tokens[la].type == TokenType::RBRACE))
            {
                // Shorthand property: { x } or { x, y }
                auto keyName = consume().value;
                key = std::make_unique<ASTNode>(StringLiteral{keyName}, ln);
                isShorthand = true;
            }
            else
                key = parseExpr();
        }
        else
            key = parseExpr();

        ASTNodePtr val;
        if (isShorthand)
        {
            val = std::make_unique<ASTNode>(Identifier{key->as<StringLiteral>().value}, ln);
        }
        else
        {
            if (check(TokenType::COMMA) || check(TokenType::RBRACE))
            {
                // Set literal element:  { "a", "b", 3 }
                // Map to dict with value "true" for membership testing
                val = std::make_unique<ASTNode>(BoolLiteral{true}, ln);
            }
            else
            {
                expect(TokenType::COLON, "Expected ':'");
                skipNewlines();
                val = parseExpr();
            }
        }

        dict.pairs.emplace_back(std::move(key), std::move(val));
        skipNewlines();
        if (!match(TokenType::COMMA))
            break;
        skipNewlines();
        // Allow trailing comma: { a: 1, b: 2, }
        if (check(TokenType::RBRACE))
            break;
    }
    expect(TokenType::RBRACE, "Expected '}'");
    return std::make_unique<ASTNode>(std::move(dict), ln);
}

ASTNodePtr Parser::parseLambda()
{
    // Called after consuming fn / function / def keyword (anonymous form)
    int ln = current().line;
    std::vector<ASTNodePtr> defaultArgs;
    std::vector<std::string> paramTypes;
    auto params = parseParamList(nullptr, &defaultArgs, &paramTypes);
    match(TokenType::COLON); // Python: def style
    if (!match(TokenType::FAT_ARROW))
        match(TokenType::ARROW); // JS => or Quantum ->
    skipNewlines();
    auto body = parseBlock();
    LambdaExpr le;
    le.params = std::move(params);
    le.paramTypes = std::move(paramTypes);
    le.defaultArgs = std::move(defaultArgs);
    le.body = std::move(body);
    return std::make_unique<ASTNode>(std::move(le), ln);
}

// Arrow function: already consumed '(' params ')' as an expression,
// then caller detects '=>' and calls this.
ASTNodePtr Parser::parseArrowFunction(std::vector<std::string> params, int ln)
{
    // consume => (FAT_ARROW) or -> (ARROW)
    if (!match(TokenType::FAT_ARROW) && !match(TokenType::ARROW))
        throw ParseError("Expected '=>' or '->'", current().line, current().col);
    skipNewlines();
    // Body can be a block OR a single expression (implicit return)
    if (check(TokenType::LBRACE) || check(TokenType::INDENT))
    {
        auto body = parseBlock();
        LambdaExpr le;
        le.params = std::move(params);
        le.body = std::move(body);
        return std::make_unique<ASTNode>(std::move(le), ln);
    }
    // Expression body: (x) => x * 2  →  wrap in implicit return block
    auto expr = parseExpr();
    int eln = expr->line;
    auto retStmt = std::make_unique<ASTNode>(ReturnStmt{std::move(expr)}, eln);
    BlockStmt block;
    block.statements.push_back(std::move(retStmt));
    auto body = std::make_unique<ASTNode>(std::move(block), ln);
    LambdaExpr le;
    le.params = std::move(params);
    le.body = std::move(body);
    return std::make_unique<ASTNode>(std::move(le), ln);
}

std::vector<ASTNodePtr> Parser::parseArgList()
{
    expect(TokenType::LPAREN, "Expected '('");
    std::vector<ASTNodePtr> args;
    skipNewlines();
    while (!check(TokenType::RPAREN) && !atEnd())
    {
        int argLn = current().line;
        skipNewlines();

        // kwargs unpacking: **expr
        if (check(TokenType::POWER)) // ** is POWER
        {
            int pLn = current().line;
            consume(); // eat **
            auto expr = parseExpr();
            // Wrap in a UnaryExpr{"**", ...} so evalCall knows it's a spread
            args.push_back(std::make_unique<ASTNode>(UnaryExpr{"**", std::move(expr)}, pLn));
            skipNewlines();
            if (!match(TokenType::COMMA))
                break;
            skipNewlines();
            continue;
        }

        // keyword argument: name=expr — skip the name= and just use the value
        if (check(TokenType::IDENTIFIER))
        {
            size_t la = pos + 1;
            while (la < tokens.size() && tokens[la].type == TokenType::NEWLINE)
                ++la;
            if (la < tokens.size() && tokens[la].type == TokenType::ASSIGN)
            {
                consume(); // name
                while (check(TokenType::NEWLINE))
                    consume();
                consume(); // '='
                skipNewlines();
            }
        }

        auto expr = parseExpr();
        skipNewlines();

        // Generator expression: f(expr for var in iterable)
        if (check(TokenType::FOR))
        {
            consume();
            std::vector<std::string> vars;
            auto readVar = [&]()
            {
                if (check(TokenType::IDENTIFIER))
                    vars.push_back(consume().value);
                else if (isCTypeKeyword(current().type))
                    vars.push_back(consume().value);
                else
                    vars.push_back(expect(TokenType::IDENTIFIER, "Expected variable").value);
            };
            readVar();
            while (match(TokenType::COMMA))
                readVar();
            if (!match(TokenType::IN) && !match(TokenType::OF))
                throw ParseError("Expected 'in' in generator expression", current().line, current().col);
            auto iterable = parseExpr();
            skipNewlines();
            ASTNodePtr condition;
            if (check(TokenType::IF))
            {
                consume();
                condition = parseExpr();
                skipNewlines();
            }
            ListComp lc;
            lc.expr = std::move(expr);
            lc.vars = std::move(vars);
            lc.iterable = std::move(iterable);
            lc.condition = std::move(condition);
            args.push_back(std::make_unique<ASTNode>(std::move(lc), argLn));
            skipNewlines();
            break; // generator consumes entire arg list
        }

        args.push_back(std::move(expr));
        skipNewlines();
        if (!match(TokenType::COMMA))
            break;
        skipNewlines();
    }
    expect(TokenType::RPAREN, "Expected ')'");
    return args;
}

std::vector<std::string> Parser::parseParamList(std::vector<bool> *outIsRef, std::vector<ASTNodePtr> *outDefaultArgs, std::vector<std::string> *outParamTypes)
{
    expect(TokenType::LPAREN, "Expected '('");
    std::vector<std::string> params;
    while (!check(TokenType::RPAREN) && !atEnd())
    {
        // C++ style: "const" modifier before type
        if (check(TokenType::CONST))
            consume(); // eat const

        // C-style: "int x" — type keyword before name
        if (isCTypeKeyword(current().type))
        {
            consume();
            while (isCTypeKeyword(current().type))
                consume(); // multi-word types
        }

        // C++ style: identifier type before name (e.g. "string name", "Entity *m", "Room &r")
        // Detect: IDENTIFIER followed by (BIT_AND or STAR or IDENTIFIER) — means it's a type name
        if (check(TokenType::IDENTIFIER))
        {
            // Peek ahead past any * and & qualifiers to find the actual name token
            size_t la = pos + 1;
            // Skip template arguments <...> in lookahead
            if (la < tokens.size() && tokens[la].type == TokenType::LT)
            {
                // Skip over template: unique_ptr<int[]>, shared_ptr<Foo>, etc.
                int tdepth = 0;
                while (la < tokens.size())
                {
                    if (tokens[la].type == TokenType::LT)
                        tdepth++;
                    else if (tokens[la].type == TokenType::GT)
                    {
                        tdepth--;
                        la++;
                        if (tdepth <= 0)
                            break;
                        continue;
                    }
                    else if (tokens[la].type == TokenType::RSHIFT)
                    {
                        tdepth -= 2;
                        la++;
                        if (tdepth <= 0)
                            break;
                        continue;
                    }
                    la++;
                }
            }
            while (la < tokens.size() &&
                   (tokens[la].type == TokenType::BIT_AND ||
                    tokens[la].type == TokenType::STAR ||
                    tokens[la].type == TokenType::CONST))
                la++;
            if (la < tokens.size() && tokens[la].type == TokenType::IDENTIFIER)
            {
                std::string tName = consume().value; // eat type name (e.g. "Entity", "Room", "Cell", "string", "unique_ptr")
                // Skip template arguments: unique_ptr<int[]>, shared_ptr<Foo>, etc.
                if (check(TokenType::LT))
                {
                    consume(); // eat '<'
                    int tdepth = 1;
                    while (!atEnd() && tdepth > 0)
                    {
                        if (check(TokenType::LT))
                            tdepth++;
                        else if (check(TokenType::GT))
                            tdepth--;
                        else if (check(TokenType::RSHIFT))
                        {
                            tdepth -= 2;
                            consume();
                            continue;
                        }
                        consume();
                    }
                }
                // Store C++ type name if needed
            }
        }

        // Detect whether this param is a pointer (*) or reference (&)
        bool isRef = false;
        while (check(TokenType::BIT_AND) || check(TokenType::STAR) || check(TokenType::POWER))
        {
            if (check(TokenType::BIT_AND))
                isRef = true; // & = pass-by-reference
            consume();
        }

        if (check(TokenType::IDENTIFIER))
        {
            params.push_back(consume().value);
            if (outIsRef)
                outIsRef->push_back(isRef);
            // Skip C-style array dimension brackets: arr[], M[N][N], etc.
            while (check(TokenType::LBRACKET))
            {
                consume(); // eat [
                int bdepth = 1;
                while (!atEnd() && bdepth > 0)
                {
                    if (check(TokenType::LBRACKET))
                        bdepth++;
                    else if (check(TokenType::RBRACKET))
                        bdepth--;
                    consume();
                }
            }
        }
        else if (check(TokenType::THIS))
        {
            params.push_back(consume().value);
            if (outIsRef)
                outIsRef->push_back(false);
        }
        else if (check(TokenType::INPUT) || check(TokenType::PRINT) ||
                 check(TokenType::COUT) || check(TokenType::CIN))
        {
            // keyword tokens used as param names: e.g. void foo(int input, int* cout)
            params.push_back(consume().value);
            if (outIsRef)
                outIsRef->push_back(isRef);
        }
        else if (check(TokenType::COMMA) || check(TokenType::RPAREN))
        {
            // Unnamed parameter: e.g. void foo(int*, int) — just skip, no name to bind
            // Generate a placeholder name so param count stays consistent
            params.push_back("__unnamed_" + std::to_string(params.size()));
            if (outIsRef)
                outIsRef->push_back(isRef);
        }
        else
        {
            throw ParseError("Expected parameter name", current().line, current().col);
        }

        // Python-style annotation: "x: int" or "x: str" — skip ": type"
        if (check(TokenType::COLON))
        {
            consume(); // eat :
            std::string typeName;
            if (check(TokenType::IDENTIFIER) || isCTypeKeyword(current().type))
                typeName = consume().value;
            
            if (outParamTypes) {
               while (outParamTypes->size() < params.size() - 1) outParamTypes->push_back("");
               outParamTypes->push_back(typeName);
            }

            // consume the rest of the type — could be generic like List[X]
            int depth = 0;
            while (!atEnd())
            {
                if (check(TokenType::LBRACKET) || check(TokenType::LPAREN))
                    depth++;
                else if (check(TokenType::RBRACKET) || check(TokenType::RPAREN))
                {
                    if (depth == 0)
                        break;
                    depth--;
                }
                else if (depth == 0 && (check(TokenType::COMMA) || check(TokenType::ASSIGN)))
                    break;
                consume();
            }
        }
        else if (outParamTypes)
        {
            while (outParamTypes->size() < params.size())
                outParamTypes->push_back("");
        }

        // Default value: "x = 5" or "x: int = 5"
        if (check(TokenType::ASSIGN))
        {
            consume(); // eat =
            auto expr = parseExpr();
            if (outDefaultArgs)
            {
                while (outDefaultArgs->size() < params.size() - 1)
                    outDefaultArgs->push_back(nullptr);
                outDefaultArgs->push_back(std::move(expr));
            }
        }
        else if (outDefaultArgs)
        {
            while (outDefaultArgs->size() < params.size())
                outDefaultArgs->push_back(nullptr);
        }

        if (!match(TokenType::COMMA))
            break;
    }
    expect(TokenType::RPAREN, "Expected ')'");
    return params;
}

bool Parser::isCTypeKeyword(TokenType t) const
{
    switch (t)
    {
    case TokenType::TYPE_INT:
    case TokenType::TYPE_FLOAT:
    case TokenType::TYPE_DOUBLE:
    case TokenType::TYPE_CHAR:
    case TokenType::TYPE_STRING:
    case TokenType::TYPE_BOOL:
    case TokenType::TYPE_VOID:
    case TokenType::TYPE_LONG:
    case TokenType::TYPE_SHORT:
    case TokenType::TYPE_UNSIGNED:
        return true;
    default:
        return false;
    }
}

ASTNodePtr Parser::parseCTypeVarDecl(const std::string &typeHint)
{
    int ln = current().line;
    // Consume any pointer stars and const qualifiers between type and name:
    // int* p  /  int *p  /  int* const p  /  const int* const p
    bool isPointer = false;
    while (check(TokenType::STAR) || check(TokenType::CONST))
    {
        if (check(TokenType::STAR))
            isPointer = true;
        consume();
    }

    // Check for optional '(' for function pointers like void (*fp)() or void (Class::*fp)()
    bool hasParen = false;
    if (check(TokenType::LPAREN))
    {
        hasParen = true;
        consume();
        while (check(TokenType::STAR) || check(TokenType::CONST))
        {
            if (check(TokenType::STAR))
                isPointer = true;
            consume();
        }
    }

    // Now it could be `ClassName::*varName` or just `varName`
    std::string prefix = "";
    if (check(TokenType::IDENTIFIER))
    {
        size_t la = pos + 1;
        if (la < tokens.size() && tokens[la].type == TokenType::COLON)
        {
            size_t la2 = la + 1;
            if (la2 < tokens.size() && tokens[la2].type == TokenType::COLON)
            {
                std::string className = consume().value; // eat ClassName (discard for naming)
                consume();                               // colon
                consume();                               // colon
                // prefix is discarded — variable gets just its short name
                while (check(TokenType::STAR))
                {
                    isPointer = true;
                    consume();
                }
            }
        }
    }

    // Accept IDENTIFIER or keyword tokens used as variable/param names
    Token nameToken = check(TokenType::IDENTIFIER) ? consume() : (check(TokenType::INPUT) || check(TokenType::PRINT)) ? consume()
                                                                                                                      : expect(TokenType::IDENTIFIER, "Expected variable name after type");

    if (hasParen)
    {
        expect(TokenType::RPAREN, "Expected ')' inside function pointer declaration");
        // skip the arg list "()" of the function pointer
        if (check(TokenType::LPAREN))
        {
            consume();
            int depth = 1;
            while (!atEnd() && depth > 0)
            {
                if (check(TokenType::LPAREN))
                    depth++;
                else if (check(TokenType::RPAREN))
                    depth--;
                consume();
            }
        }
    }

    // Skip C array dimension brackets: char map[ROWS][COLS], int arr[N], etc.
    bool isArray = false;
    std::vector<ASTNodePtr> arrayDims;
    while (check(TokenType::LBRACKET))
    {
        isArray = true;
        consume(); // eat '['
        // Capture the dimension expression so "int arr[N]" can allocate
        if (!check(TokenType::RBRACKET))
        {
            try
            {
                arrayDims.push_back(parseExpr());
            }
            catch (...)
            {
            }
        }
        int bdepth = 1;
        while (!atEnd() && bdepth > 0)
        {
            if (check(TokenType::LBRACKET))
                bdepth++;
            else if (check(TokenType::RBRACKET))
                bdepth--;
            consume();
        }
    }
    std::string finalTypeHint = typeHint;
    if (isArray)
        finalTypeHint += "[]";
    ASTNodePtr init;
    if (match(TokenType::ASSIGN))
    {
        init = parseExpr();
        // C aggregate zero-init: int m[10][10] = {0}; — "{0}" means
        // "zero-fill the whole array", not a one-element array.
        bool zeroFillIdiom = false;
        if (!arrayDims.empty() && init && init->is<ArrayLiteral>())
        {
            auto &els = init->as<ArrayLiteral>().elements;
            zeroFillIdiom = els.empty() ||
                            (els.size() == 1 && els[0]->is<NumberLiteral>() &&
                             els[0]->as<NumberLiteral>().value == 0.0);
        }
        if (zeroFillIdiom)
        {
            CallExpr mk;
            mk.callee = std::make_unique<ASTNode>(Identifier{"__c_array__"}, ln);
            mk.args = std::move(arrayDims);
            init = std::make_unique<ASTNode>(std::move(mk), ln);
        }
    }
    else if (!arrayDims.empty())
    {
        // int arr[10] / char grid[R][C] — allocate a zero-filled (nested) array
        CallExpr mk;
        mk.callee = std::make_unique<ASTNode>(Identifier{"__c_array__"}, ln);
        mk.args = std::move(arrayDims);
        init = std::make_unique<ASTNode>(std::move(mk), ln);
    }
    auto decl = VarDecl{false, nameToken.value, std::move(init), finalTypeHint};
    decl.isPointer = isPointer;
    auto node = std::make_unique<ASTNode>(std::move(decl), ln);
    // Do NOT consume trailing semicolons here — callers that need them (for-loop init,
    // multi-var comma lists) handle their own terminators.  Eating ';' here would
    // swallow the separator in  for (int i = 0; i < n; i++)  and break the parse.
    // Only skip bare newlines that are purely cosmetic whitespace.
    while (check(TokenType::NEWLINE))
        consume();
    return node;
}