#include "Parser.h"
#include <sstream>
#include <unordered_set>
#include <cctype>

ASTNodePtr Parser::parseStatement()
{
    skipNewlines();

    // Skip Python-style decorators (e.g. @property, @dataclass)
    while (check(TokenType::DECORATOR))
    {
        consume(); // eat @
        if (check(TokenType::IDENTIFIER))
        {
            consume(); // eat decorator name
            // Optional call parens e.g. @decorator(args)
            if (check(TokenType::LPAREN))
            {
                consume(); // eat (
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
        skipNewlines();
    }

    int ln = current().line;
    // Skip C/C++ storage class specifiers like static, extern, inline, volatile, register
    while (!atEnd() && check(TokenType::IDENTIFIER) &&
           (current().value == "static" || current().value == "extern" ||
            current().value == "inline" || current().value == "volatile" ||
            current().value == "register" || current().value == "mutable" ||
            current().value == "constexpr"))
    {
        consume();
    }
    switch (current().type)
    {
    case TokenType::LET:
    {
        consume();
        return parseVarDecl(false);
    }
    case TokenType::CONST:
    {
        consume(); // eat 'const'
        // C-style: "const int* p = &a" — const followed by a type keyword
        // In this case treat as a C-type var decl (the const is just a qualifier, ignore it)
        if (isCTypeKeyword(current().type))
        {
            // Re-use the TYPE_INT/etc. branch logic inline:
            // eat all type keywords (e.g. "const int", "const unsigned long")
            std::string typeHint = consume().value;
            while (isCTypeKeyword(current().type))
                typeHint += " " + consume().value;
            // eat trailing const after type: "int* const ptr"
            if (check(TokenType::CONST))
                consume();
            auto firstDecl = parseCTypeVarDecl(typeHint);
            while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                consume();
            if (!check(TokenType::COMMA))
                return firstDecl;
            auto block = std::make_unique<ASTNode>(BlockStmt{}, ln);
            block->as<BlockStmt>().statements.push_back(std::move(firstDecl));
            while (check(TokenType::COMMA))
            {
                consume();
                std::string hint = typeHint;
                if (isCTypeKeyword(current().type) || check(TokenType::CONST))
                {
                    hint = consume().value;
                    while (isCTypeKeyword(current().type) || check(TokenType::CONST))
                        hint += " " + consume().value;
                }
                block->as<BlockStmt>().statements.push_back(parseCTypeVarDecl(hint));
                while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                    consume();
            }
            while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                consume();
            return block;
        }
        // C++ style: "const vector<K>& keys = ..." / "const Node* p = ..."
        // const followed by a class-type name — drop the const qualifier and
        // let the identifier-declaration path handle the rest.
        if (check(TokenType::IDENTIFIER) && pos + 1 < tokens.size() &&
            (tokens[pos + 1].type == TokenType::LT ||
             tokens[pos + 1].type == TokenType::IDENTIFIER ||
             tokens[pos + 1].type == TokenType::STAR ||
             tokens[pos + 1].type == TokenType::BIT_AND))
            return parseStatement();
        // Quantum/JS style: "const x = 5"
        return parseVarDecl(true);
    }
    case TokenType::FN:
    case TokenType::DEF:
    case TokenType::FUNCTION:
    {
        consume();
        if (current().type == TokenType::IDENTIFIER)
            return parseFunctionDecl();
        auto lam = parseLambda();
        while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
            consume();
        return std::make_unique<ASTNode>(ExprStmt{std::move(lam)}, lam->line);
    }
    case TokenType::CLASS:
    {
        consume();
        return parseClassDecl();
    }
    case TokenType::IF:
    {
        consume();
        return parseIfStmt();
    }
    case TokenType::WHILE:
    {
        consume();
        return parseWhileStmt();
    }
    case TokenType::FOR:
    {
        consume();
        return parseForStmt();
    }
    case TokenType::RETURN:
    {
        consume();
        return parseReturnStmt();
    }
    case TokenType::PRINT:
    {
        // If followed by '(' it could be a user-defined function named "print"
        // Only treat as print statement if NOT followed by ( with comma-separated args
        // that look like a function call (i.e. not the Quantum print keyword usage)
        if (pos + 1 < tokens.size() && tokens[pos + 1].type == TokenType::LPAREN)
            return parseExprStmt();
        consume();
        return parsePrintStmt();
    }
    case TokenType::INPUT:
    {
        if (pos + 1 < tokens.size() && tokens[pos + 1].type == TokenType::LPAREN)
        {
            size_t scan = pos + 2;
            while (scan < tokens.size() && tokens[scan].type == TokenType::NEWLINE)
                ++scan;
            bool looksLikeScanf = false;
            if (scan < tokens.size() && tokens[scan].type == TokenType::STRING)
            {
                const std::string &fmt = tokens[scan].value;
                looksLikeScanf = fmt.find('%') != std::string::npos;
            }
            if (!looksLikeScanf)
                return parseExprStmt();
        }
        consume();
        return parseInputStmt();
    }
    case TokenType::CIN:
    {
        consume();
        return parseCinStmt();
    }
    case TokenType::COUT:
    {
        consume();
        return parseCoutStmt();
    }
    case TokenType::IMPORT:
    case TokenType::FROM:
    {
        // For FROM, we don't consume here because parseImportStmt needs to know if it's 'from' or 'import'
        // But the current implementation consumes. Let's consume and pass a bool or just let parseImportStmt do it.
        // Actually, the current code just calls parseImportStmt().
        bool isFrom = (current().type == TokenType::FROM);
        consume();
        return parseImportStmt(isFrom);
    }
    case TokenType::BREAK:
    {
        consume();
        while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
            consume();
        return std::make_unique<ASTNode>(BreakStmt{}, ln);
    }
    case TokenType::CONTINUE:
    {
        consume();
        while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
            consume();
        return std::make_unique<ASTNode>(ContinueStmt{}, ln);
    }
    case TokenType::RAISE:
    {
        consume();
        ASTNodePtr val;
        // As with `return`, a bare re-raise can be closed by its enclosing
        // block on the same line: `rescue => e { raise }`.
        if (!check(TokenType::NEWLINE) && !check(TokenType::SEMICOLON) &&
            !check(TokenType::RBRACE) && !atEnd())
            val = parseExpr();
        while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
            consume();
        return std::make_unique<ASTNode>(RaiseStmt{std::move(val)}, ln);
    }
    case TokenType::WITH:
    {
        // Python `with EXPR as NAME:` (optionally comma-separated) desugars to:
        //     NAME = EXPR
        //     try { <body> } finally { NAME.close() }
        // Quantum has no context-manager protocol, so cleanup is a hardcoded
        // .close() — correct for file objects (open()), which is the case that
        // exists. Bindings without `as` still run the body; nothing to close.
        consume(); // eat 'with'

        struct Item { std::string name; ASTNodePtr expr; };
        std::vector<Item> items;

        while (true)
        {
            ASTNodePtr expr = parseExpr();
            std::string name;
            if (check(TokenType::AS))
            {
                consume();
                if (check(TokenType::IDENTIFIER))
                    name = consume().value;
            }
            items.push_back({std::move(name), std::move(expr)});
            if (check(TokenType::COMMA)) { consume(); continue; }
            break;
        }

        match(TokenType::COLON);
        skipNewlines();
        auto body = parseBlock();

        // Build the finally block: NAME.close() for each named item (reverse order)
        BlockStmt finallyBlock;
        for (auto it = items.rbegin(); it != items.rend(); ++it)
        {
            if (it->name.empty()) continue;
            auto target = std::make_unique<ASTNode>(Identifier{it->name}, ln);
            auto member = std::make_unique<ASTNode>(
                MemberExpr{std::move(target), "close"}, ln);
            CallExpr call;
            call.callee = std::move(member);
            auto callNode = std::make_unique<ASTNode>(std::move(call), ln);
            finallyBlock.statements.push_back(
                std::make_unique<ASTNode>(ExprStmt{std::move(callNode)}, ln));
        }

        // Assemble: [ NAME = EXPR ; ... ; try { body } finally { closes } ]
        BlockStmt outer;
        for (auto &item : items)
        {
            if (item.name.empty())
            {
                // No binding — just evaluate the expression for its effect
                outer.statements.push_back(
                    std::make_unique<ASTNode>(ExprStmt{std::move(item.expr)}, ln));
                continue;
            }
            auto lhs = std::make_unique<ASTNode>(Identifier{item.name}, ln);
            AssignExpr assign;
            assign.op = "=";
            assign.target = std::move(lhs);
            assign.value = std::move(item.expr);
            auto assignNode = std::make_unique<ASTNode>(std::move(assign), ln);
            outer.statements.push_back(
                std::make_unique<ASTNode>(ExprStmt{std::move(assignNode)}, ln));
        }

        TryStmt ts;
        ts.body = std::move(body);
        if (!finallyBlock.statements.empty())
            ts.finallyBody = std::make_unique<ASTNode>(std::move(finallyBlock), ln);
        outer.statements.push_back(std::make_unique<ASTNode>(std::move(ts), ln));

        return std::make_unique<ASTNode>(std::move(outer), ln);
    }
    case TokenType::TRY:
    {
        consume();
        match(TokenType::COLON);
        skipNewlines();
        auto body = parseBlock();
        TryStmt ts;
        ts.body = std::move(body);
        skipNewlines(); // skip blank lines between try body and except/finally
        // Parse except/catch clauses
        while (check(TokenType::EXCEPT))
        {
            consume(); // eat 'except'
            ExceptClause clause;
            // Optional error type: except ValueError or except (ValueError, TypeError)
            if (!check(TokenType::COLON) && !check(TokenType::NEWLINE) && !check(TokenType::INDENT))
            {
                // skip optional '(' around type list
                match(TokenType::LPAREN);
                // C++: catch (const exception& e) — strip const, take the type
                if (check(TokenType::CONST))
                    consume();
                if (check(TokenType::IDENTIFIER))
                    clause.errorType = consume().value;
                else if (isCTypeKeyword(current().type))
                    clause.errorType = consume().value;
                // C++ reference/pointer qualifiers, then the binding name
                while (check(TokenType::BIT_AND) || check(TokenType::STAR))
                    consume();
                if (check(TokenType::IDENTIFIER) && clause.alias.empty() &&
                    current().value != "as")
                    clause.alias = consume().value;
                // skip extra types: except (A, B) — just use first
                while (check(TokenType::COMMA))
                {
                    consume();
                    if (check(TokenType::IDENTIFIER))
                        consume();
                }
                match(TokenType::RPAREN);
                // optional 'as e'
                if (check(TokenType::AS))
                {
                    consume();
                    if (check(TokenType::IDENTIFIER))
                        clause.alias = consume().value;
                }
            }
            match(TokenType::COLON);
            skipNewlines();
            clause.body = parseBlock();
            ts.handlers.push_back(std::move(clause));
            skipNewlines();
        }
        // Python try/except/else — else runs only when no exception was
        // raised; appending it to the try body gives exactly that behaviour.
        if (check(TokenType::ELSE))
        {
            consume();
            match(TokenType::COLON);
            skipNewlines();
            auto elseBody = parseBlock();
            if (ts.body && ts.body->is<BlockStmt>() && elseBody->is<BlockStmt>())
            {
                auto &dst = ts.body->as<BlockStmt>().statements;
                for (auto &st : elseBody->as<BlockStmt>().statements)
                    dst.push_back(std::move(st));
            }
            skipNewlines();
        }
        // Optional finally
        if (check(TokenType::FINALLY))
        {
            consume();
            match(TokenType::COLON);
            skipNewlines();
            ts.finallyBody = parseBlock();
        }
        return std::make_unique<ASTNode>(std::move(ts), ln);
    }
    case TokenType::LBRACE:
        return parseBlock();
    // ── C/C++ style typed declarations ────────────────────────────────────
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
    {
        // Only treat as a C-type declaration if followed by an identifier.
        // e.g.  "int x = 5"  → declaration
        //       "int main()" → function declaration
        //       "string = x" → plain assignment (string used as variable name)
        // Peek past any chained type qualifiers to find the next real token.
        size_t lookahead = pos + 1;
        while (lookahead < tokens.size() && (isCTypeKeyword(tokens[lookahead].type) ||
                                             tokens[lookahead].type == TokenType::CONST))
            ++lookahead;
        // Skip pointer/reference qualifiers between type and name: int* funcName, int& ref
        while (lookahead < tokens.size() &&
               (tokens[lookahead].type == TokenType::STAR || tokens[lookahead].type == TokenType::BIT_AND ||
                tokens[lookahead].type == TokenType::CONST))
            ++lookahead;
        // Also accept keyword tokens used as function names (e.g. "input", "display", "print")
        auto isValidFuncName = [&](TokenType t)
        {
            return t == TokenType::IDENTIFIER || t == TokenType::INPUT || t == TokenType::PRINT;
        };
        if (lookahead < tokens.size() && isValidFuncName(tokens[lookahead].type))
        {
            // Check if this is actually a function definition: ReturnType funcName(params)
            size_t la2 = lookahead + 1;
            while (la2 < tokens.size() && tokens[la2].type == TokenType::NEWLINE)
                la2++;
            if (la2 < tokens.size() && tokens[la2].type == TokenType::LPAREN)
            {
                // It's a function definition — consume return type and parse as function
                while (isCTypeKeyword(current().type) || check(TokenType::CONST))
                    consume(); // eat return type keywords + const
                // Also eat any pointer/reference qualifiers: int* funcName → eat *
                while (check(TokenType::STAR) || check(TokenType::BIT_AND) || check(TokenType::CONST))
                    consume();
                return parseFunctionDecl();
            }
            auto typeHint = consume().value;
            while (isCTypeKeyword(current().type) || check(TokenType::CONST))
                typeHint += " " + consume().value;

            // Consume pointer stars that come BEFORE any name, e.g. "int* a, *b, c"
            // We let parseCTypeVarDecl handle per-name stars; here we just check
            // if this is a multi-variable declaration: int a, b, c;
            // Build a block of VarDecls for comma-separated names.
            {
                auto firstDecl = parseCTypeVarDecl(typeHint);
                while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                    consume();
                if (!check(TokenType::COMMA))
                    return firstDecl;
                // Multi-var: int a, b, c;  or  int a, *b, c = 5;  or  int a, int b, int c;
                auto block = std::make_unique<ASTNode>(BlockStmt{}, ln);
                block->as<BlockStmt>().statements.push_back(std::move(firstDecl));
                while (check(TokenType::COMMA))
                {
                    consume(); // eat ','
                    // Each var may repeat the type keyword: "int a, int b" → eat optional type
                    std::string hint = typeHint;
                    if (isCTypeKeyword(current().type) || check(TokenType::CONST))
                    {
                        hint = consume().value;
                        while (isCTypeKeyword(current().type) || check(TokenType::CONST))
                            hint += " " + consume().value;
                    }
                    block->as<BlockStmt>().statements.push_back(parseCTypeVarDecl(hint));
                    while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                        consume();
                }
                while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                    consume();
                return block;
            }
        }
        // Function pointer declaration: void (*fp)() or void (Class::*fp)()
        // lookahead points to '(' after the type keyword(s)
        if (lookahead < tokens.size() && tokens[lookahead].type == TokenType::LPAREN)
        {
            auto typeHint = consume().value;
            while (isCTypeKeyword(current().type) || check(TokenType::CONST))
                typeHint += " " + consume().value;
            auto fpDecl = parseCTypeVarDecl(typeHint);
            while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                consume();
            return fpDecl;
        }
        // Not a declaration — fall through to expression statement
        return parseExprStmt();
    }
    default:
        // Handle C++ "delete ptr" and "delete[] ptr" as no-ops (GC handles memory)
        if (check(TokenType::IDENTIFIER) && current().value == "delete")
        {
            consume(); // eat 'delete'
            if (check(TokenType::LBRACKET))
            {
                consume();
                if (check(TokenType::RBRACKET))
                    consume();
            }
            if (!check(TokenType::NEWLINE) && !check(TokenType::SEMICOLON) && !atEnd())
                parseExpr();
            while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                consume();
            return std::make_unique<ASTNode>(BlockStmt{}, ln);
        }
        // Handle C++ "using namespace X;" as a no-op
        if (check(TokenType::IDENTIFIER) && current().value == "using")
        {
            while (!atEnd() && !check(TokenType::NEWLINE) && !check(TokenType::SEMICOLON))
                consume();
            while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                consume();
            // Return an empty block as a no-op
            return std::make_unique<ASTNode>(BlockStmt{}, ln);
        }
        // C/C++/JS switch statement — desugared to an if-chain with C
        // fallthrough semantics inside a breakable loop.
        if (check(TokenType::IDENTIFIER) && current().value == "switch" &&
            pos + 1 < tokens.size() && tokens[pos + 1].type == TokenType::LPAREN)
            return parseSwitchStmt();

        // Handle C++ "template<typename T, ...>" prefix as a no-op — skip the
        // angle-bracket list so the class/function that follows parses normally
        // (Quantum is dynamically typed, so the type parameters are irrelevant).
        if (check(TokenType::IDENTIFIER) && current().value == "template" &&
            pos + 1 < tokens.size() && tokens[pos + 1].type == TokenType::LT)
        {
            consume(); // eat 'template'
            consume(); // eat '<'
            int tdepth = 1;
            while (!atEnd() && tdepth > 0)
            {
                if (check(TokenType::LT))
                    tdepth++;
                else if (check(TokenType::GT))
                    tdepth--;
                else if (check(TokenType::RSHIFT))
                    tdepth -= 2;
                consume();
            }
            skipNewlines();
            return parseStatement();
        }
        // Handle C++ "struct Name { ... };" / "union Name { ... };" as a class
        // declaration — fields, constructors (incl. initializer lists), and
        // methods all go through parseClassDecl.
        if (check(TokenType::IDENTIFIER) &&
            (current().value == "struct" || current().value == "union") &&
            pos + 1 < tokens.size() && tokens[pos + 1].type == TokenType::IDENTIFIER)
        {
            size_t laS = pos + 2;
            while (laS < tokens.size() && tokens[laS].type == TokenType::NEWLINE)
                laS++;
            if (laS < tokens.size() && tokens[laS].type == TokenType::LBRACE)
            {
                consume(); // eat 'struct' / 'union'
                return parseClassDecl();
            }
        }
        // Handle GCC/Clang __asm__ volatile (...) — skip as a no-op
        if (check(TokenType::IDENTIFIER) && (current().value == "__asm__" || current().value == "asm"))
        {
            consume(); // eat __asm__
            if (check(TokenType::IDENTIFIER) && current().value == "volatile")
                consume(); // eat volatile
            if (check(TokenType::LPAREN))
            {
                consume(); // eat (
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
            while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                consume();
            return std::make_unique<ASTNode>(BlockStmt{}, ln);
        }
        // Handle C "typedef ..." as a near-no-op — skip the declaration but
        // register the alias name as an empty class so later uses like
        // "Cell board[N]" or "Point snake[N]" resolve without NameError.
        if (check(TokenType::IDENTIFIER) && current().value == "typedef")
        {
            consume(); // eat 'typedef'

            // Detect "typedef enum { ... } Alias;" and extract enumerator constants.
            bool isEnum = (check(TokenType::IDENTIFIER) &&
                           (current().value == "enum" || current().value == "struct" || current().value == "union"));
            bool isRawEnum = isEnum && current().value == "enum";
            if (isEnum)
                consume(); // eat 'enum'/'struct'/'union'

            // Optional tag name before '{'
            if (check(TokenType::IDENTIFIER) && !check(TokenType::LBRACE))
            {
                // peek: if next is '{' it's the body, otherwise it's a tag name
                if (pos + 1 < tokens.size() && tokens[pos + 1].type == TokenType::LBRACE)
                    consume(); // eat optional tag name
                // If no '{' follows, this is a forward decl or typedef of existing type
            }

            // Parse enum body { A=0, B, C=5, ... } to collect enumerator constants
            std::vector<std::pair<std::string, double>> enumerators;
            if (isRawEnum && check(TokenType::LBRACE))
            {
                consume(); // eat '{'
                double nextVal = 0.0;
                while (!check(TokenType::RBRACE) && !atEnd())
                {
                    if (check(TokenType::NEWLINE))
                    {
                        consume();
                        continue;
                    }
                    if (!check(TokenType::IDENTIFIER))
                    {
                        consume();
                        continue;
                    }
                    std::string eName = consume().value;
                    double eVal = nextVal;
                    if (match(TokenType::ASSIGN))
                    {
                        // parse a simple numeric constant (possibly negative)
                        bool neg = false;
                        if (check(TokenType::MINUS))
                        {
                            neg = true;
                            consume();
                        }
                        if (check(TokenType::NUMBER))
                            eVal = std::stod(consume().value) * (neg ? -1 : 1);
                        else if (check(TokenType::IDENTIFIER))
                        {
                            // reference to a previously defined enumerator
                            std::string ref = consume().value;
                            for (auto &p : enumerators)
                                if (p.first == ref)
                                {
                                    eVal = p.second;
                                    break;
                                }
                        }
                    }
                    enumerators.push_back({eName, eVal});
                    nextVal = eVal + 1.0;
                    if (check(TokenType::COMMA))
                        consume();
                }
                if (check(TokenType::RBRACE))
                    consume(); // eat '}'
            }
            else
            {
                // Non-enum typedef: skip to matching '}' if there's a body, else to ';'
                int depth = 0;
                while (!atEnd())
                {
                    if (check(TokenType::LBRACE))
                    {
                        depth++;
                        consume();
                    }
                    else if (check(TokenType::RBRACE))
                    {
                        depth--;
                        consume();
                        if (depth == 0)
                            break;
                    }
                    else if (check(TokenType::SEMICOLON) && depth == 0)
                        break;
                    else if (check(TokenType::NEWLINE) && depth == 0)
                        break;
                    else
                        consume();
                }
            }

            // Collect tokens up to ';' to find the alias name (last identifier before ';')
            std::string aliasName;
            {
                std::string lastIdent;
                while (!atEnd())
                {
                    if (check(TokenType::SEMICOLON) || (check(TokenType::NEWLINE)))
                    {
                        aliasName = lastIdent;
                        consume();
                        break;
                    }
                    if (check(TokenType::IDENTIFIER))
                        lastIdent = current().value;
                    consume();
                }
            }

            // Build result: emit enumerator VarDecls + ClassDecl alias in a BlockStmt
            if (!enumerators.empty() || !aliasName.empty())
            {
                auto block = std::make_unique<ASTNode>(BlockStmt{}, ln);
                // Emit one VarDecl for each enumerator constant
                for (auto &[eName, eVal] : enumerators)
                {
                    auto numNode = std::make_unique<ASTNode>(NumberLiteral{eVal}, ln);
                    VarDecl vd;
                    vd.name = eName;
                    vd.initializer = std::move(numNode);
                    block->as<BlockStmt>().statements.push_back(
                        std::make_unique<ASTNode>(std::move(vd), ln));
                }
                // Emit ClassDecl for the alias name
                if (!aliasName.empty())
                {
                    ClassDecl cd;
                    cd.name = aliasName;
                    block->as<BlockStmt>().statements.push_back(
                        std::make_unique<ASTNode>(std::move(cd), ln));
                }
                return block;
            }
            return std::make_unique<ASTNode>(BlockStmt{}, ln);
        }
        // Handle C++ class-type variable declaration: "ClassName varName;" or "ClassName varName(args);"
        // Also handles: "struct TypeName var1, var2;" where 'struct' is an IDENTIFIER token.
        if (check(TokenType::IDENTIFIER))
        {
            // Detect optional leading 'struct' / 'union' / 'enum' keyword
            bool hasStructKeyword = (current().value == "struct" ||
                                     current().value == "union" ||
                                     current().value == "enum");

            size_t la = pos + (hasStructKeyword ? 1 : 0); // index of the type-name token
            size_t laName = la + 1;                       // index of the first var-name token

            // Skip template arguments: unique_ptr<int[]>, shared_ptr<Foo>, vector<int>, etc.
            if (laName < tokens.size() && tokens[laName].type == TokenType::LT)
            {
                int tdepth = 0;
                while (laName < tokens.size())
                {
                    if (tokens[laName].type == TokenType::LT)
                        tdepth++;
                    else if (tokens[laName].type == TokenType::GT)
                    {
                        tdepth--;
                        laName++;
                        if (tdepth <= 0)
                            break;
                        continue;
                    }
                    else if (tokens[laName].type == TokenType::RSHIFT)
                    {
                        tdepth -= 2;
                        laName++;
                        if (tdepth <= 0)
                            break;
                        continue;
                    }
                    laName++;
                }
            }

            // Skip pointer/ref qualifiers between type and first var name
            while (laName < tokens.size() &&
                   (tokens[laName].type == TokenType::STAR ||
                    tokens[laName].type == TokenType::BIT_AND ||
                    tokens[laName].type == TokenType::CONST))
                ++laName;

            if (la < tokens.size() && tokens[la].type == TokenType::IDENTIFIER &&
                laName < tokens.size() && tokens[laName].type == TokenType::IDENTIFIER)
            {
                // Two identifiers in a row (optionally prefixed by struct/union/enum):
                // could be "ClassName varName" or "ClassName varName(args)" or
                // "struct TermType oldt, newt;"
                size_t la2 = laName + 1;
                // If varName is followed by ( and the ( contains typed parameters → function decl
                if (la2 < tokens.size() && tokens[la2].type == TokenType::LPAREN)
                {
                    // Peek inside the parens: if first non-paren token is a C-type keyword
                    // OR an identifier followed by * or another identifier (typed param pattern),
                    // this is a function declaration, not a constructor call.
                    size_t innerPos = la2 + 1;
                    bool looksLikeFuncDecl = false;
                    if (innerPos < tokens.size())
                    {
                        auto &iTok = tokens[innerPos];
                        if (isCTypeKeyword(iTok.type))
                            looksLikeFuncDecl = true;
                        else if (iTok.type == TokenType::CONST)
                            looksLikeFuncDecl = true; // const-qualified param → function decl
                        else if (iTok.type == TokenType::RPAREN)
                            looksLikeFuncDecl = true; // empty params → function decl
                        else if (iTok.type == TokenType::IDENTIFIER)
                        {
                            // Identifier param: "Node *head" → IDENTIFIER STAR IDENTIFIER
                            // or "Node head" → IDENTIFIER IDENTIFIER
                            // Distinguish from constructor call: "Foo(b1)" → IDENTIFIER RPAREN or Identifier that's a var
                            size_t i2 = innerPos + 1;
                            if (i2 < tokens.size() &&
                                (tokens[i2].type == TokenType::STAR ||
                                 tokens[i2].type == TokenType::BIT_AND ||
                                 tokens[i2].type == TokenType::IDENTIFIER ||
                                 tokens[i2].type == TokenType::CONST))
                                looksLikeFuncDecl = true;
                        }
                    }
                    if (looksLikeFuncDecl)
                    {
                        // It's a function definition: ReturnType* funcName(typeArgs)
                        if (hasStructKeyword)
                            consume(); // eat 'struct'
                        consume();     // eat return type (e.g. 'Node')
                        while (check(TokenType::STAR) || check(TokenType::BIT_AND) || check(TokenType::CONST))
                            consume(); // eat pointer/ref qualifiers
                        return parseFunctionDecl();
                    }
                }
                // If varName is followed by ; or = or ( or , or [ it's a variable declaration
                if (la2 < tokens.size() &&
                    (tokens[la2].type == TokenType::SEMICOLON ||
                     tokens[la2].type == TokenType::NEWLINE ||
                     tokens[la2].type == TokenType::ASSIGN ||
                     tokens[la2].type == TokenType::LPAREN ||
                     tokens[la2].type == TokenType::COMMA ||
                     tokens[la2].type == TokenType::LBRACKET))
                {
                    if (hasStructKeyword)
                        consume(); // eat 'struct' / 'union' / 'enum'

                    std::string typeName = consume().value; // eat type name
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

                    // Helper: parse one variable name (with optional pointer stars) and its initializer
                    auto parseOneVar = [&]() -> ASTNodePtr
                    {
                        bool isPtr = false;
                        while (check(TokenType::STAR) || check(TokenType::CONST) || check(TokenType::BIT_AND))
                        {
                            if (check(TokenType::STAR))
                                isPtr = true;
                            consume();
                        }
                        std::string varName = expect(TokenType::IDENTIFIER, "Expected variable name").value;
                        // C array dimension brackets: board[ROWS][COLS], Shape* shapes[3], etc.
                        // Capture the sizes so the variable is allocated as an array.
                        std::vector<ASTNodePtr> dims;
                        while (check(TokenType::LBRACKET))
                        {
                            consume(); // eat '['
                            if (!check(TokenType::RBRACKET))
                            {
                                try
                                {
                                    dims.push_back(parseExpr());
                                }
                                catch (...)
                                {
                                }
                            }
                            int depth = 1;
                            while (!atEnd() && depth > 0)
                            {
                                if (check(TokenType::LBRACKET))
                                    depth++;
                                else if (check(TokenType::RBRACKET))
                                    depth--;
                                consume();
                            }
                        }
                        ASTNodePtr init;
                        if (!dims.empty() && !check(TokenType::ASSIGN))
                        {
                            // Type name[N]; — zero-filled array, NOT a constructor call
                            CallExpr mk;
                            mk.callee = std::make_unique<ASTNode>(Identifier{"__c_array__"}, ln);
                            mk.args = std::move(dims);
                            init = std::make_unique<ASTNode>(std::move(mk), ln);
                        }
                        else if (check(TokenType::LPAREN))
                        {
                            // ClassName varName(args) — constructor call as initializer
                            auto callee = std::make_unique<ASTNode>(Identifier{typeName}, ln);
                            CallExpr ce;
                            ce.callee = std::move(callee);
                            consume(); // eat (
                            while (!check(TokenType::RPAREN) && !atEnd())
                            {
                                ce.args.push_back(parseExpr());
                                if (!match(TokenType::COMMA))
                                    break;
                            }
                            if (check(TokenType::RPAREN))
                                consume();
                            init = std::make_unique<ASTNode>(std::move(ce), ln);
                        }
                        else if (match(TokenType::ASSIGN))
                        {
                            init = parseExpr();
                        }
                        else
                        {
                            // ClassName varName; — call default constructor
                            auto callee = std::make_unique<ASTNode>(Identifier{typeName}, ln);
                            CallExpr ce;
                            ce.callee = std::move(callee);
                            init = std::make_unique<ASTNode>(std::move(ce), ln);
                        }
                        auto decl = VarDecl{false, varName, std::move(init), typeName};
                        decl.isPointer = isPtr;
                        return std::make_unique<ASTNode>(std::move(decl), ln);
                    };

                    auto firstDecl = parseOneVar();

                    // If no comma follows, return single declaration
                    if (!check(TokenType::COMMA))
                    {
                        while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                            consume();
                        return firstDecl;
                    }

                    // Multi-var: struct termios oldt, newt;
                    auto block = std::make_unique<ASTNode>(BlockStmt{}, ln);
                    block->as<BlockStmt>().statements.push_back(std::move(firstDecl));
                    while (check(TokenType::COMMA))
                    {
                        consume(); // eat ','
                        block->as<BlockStmt>().statements.push_back(parseOneVar());
                    }
                    while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                        consume();
                    return block;
                }
            }
        }
        // Handle C++ "do { } while (cond);" loop
        if (check(TokenType::IDENTIFIER) && current().value == "do")
        {
            consume(); // eat 'do'
            skipNewlines();
            auto body = parseBlock();
            skipNewlines();
            if (check(TokenType::WHILE))
                consume();
            else if (check(TokenType::IDENTIFIER) && current().value == "while")
                consume();
            expect(TokenType::LPAREN, "Expected '(' after 'while' in do-while");
            auto condition = parseExpr();
            expect(TokenType::RPAREN, "Expected ')' after do-while condition");
            while (check(TokenType::SEMICOLON) || check(TokenType::NEWLINE))
                consume();

            // Desugar to: while(true) { body; if(!cond) break; }
            BlockStmt whileBody;
            for (auto &s : body->as<BlockStmt>().statements)
                whileBody.statements.push_back(std::move(const_cast<ASTNodePtr &>(s)));
            // Add if(!cond){break;} at end
            UnaryExpr notCond;
            notCond.op = "!";
            notCond.operand = std::move(condition);
            BlockStmt breakBlock;
            breakBlock.statements.push_back(std::make_unique<ASTNode>(BreakStmt{}, ln));
            IfStmt ifBreak;
            ifBreak.condition = std::make_unique<ASTNode>(std::move(notCond), ln);
            ifBreak.thenBranch = std::make_unique<ASTNode>(std::move(breakBlock), ln);
            whileBody.statements.push_back(std::make_unique<ASTNode>(std::move(ifBreak), ln));

            auto whileBodyNode = std::make_unique<ASTNode>(std::move(whileBody), ln);
            auto trueNode = std::make_unique<ASTNode>(BoolLiteral{true}, ln);
            return std::make_unique<ASTNode>(WhileStmt{std::move(trueNode), std::move(whileBodyNode)}, ln);
        }
        return parseExprStmt();
    }
}

ASTNodePtr Parser::parseBlock()
{
    // Helper: tokens that terminate a block because they belong to the parent stmt
    auto isBlockTerminator = [&]() -> bool
    {
        switch (current().type)
        {
        case TokenType::EXCEPT:
        case TokenType::FINALLY:
        case TokenType::ELSE:
        case TokenType::ELIF:
            return true;
        default:
            return false;
        }
    };

    // Brace-style: { statements }
    if (check(TokenType::LBRACE))
    {
        int ln = current().line;
        expect(TokenType::LBRACE, "Expected '{'");
        skipNewlines();
        BlockStmt block;
        while (!check(TokenType::RBRACE) && !atEnd())
        {
            block.statements.push_back(parseStatement());
            skipNewlines();
        }
        expect(TokenType::RBRACE, "Expected '}'");
        return std::make_unique<ASTNode>(std::move(block), ln);
    }
    // Python-style: INDENT statements DEDENT
    if (check(TokenType::INDENT))
    {
        int ln = current().line;
        consume(); // eat INDENT
        skipNewlines();
        BlockStmt block;
        while (!check(TokenType::DEDENT) && !atEnd() && !isBlockTerminator())
        {
            block.statements.push_back(parseStatement());
            skipNewlines();
        }
        if (check(TokenType::DEDENT))
            consume(); // eat DEDENT
        return std::make_unique<ASTNode>(std::move(block), ln);
    }
    throw ParseError("Expected '{' or indented block", current().line, current().col);
}

// Accepts { block }, INDENT block, a single statement, or a bare ';' (empty body)
ASTNodePtr Parser::parseBodyOrStatement()
{
    // C empty-body: while(cond); or for(...);
    if (check(TokenType::SEMICOLON))
    {
        int ln = current().line;
        consume(); // eat ';'
        return std::make_unique<ASTNode>(BlockStmt{}, ln);
    }
    if (check(TokenType::LBRACE) || check(TokenType::INDENT))
        return parseBlock();
    int ln = current().line;
    BlockStmt block;
    block.statements.push_back(parseStatement());
    return std::make_unique<ASTNode>(std::move(block), ln);
}

ASTNodePtr Parser::parseVarDecl(bool isConst)
{
    int ln = current().line;
    std::string name;
    if (check(TokenType::IDENTIFIER))
        name = consume().value;
    else if (isCTypeKeyword(current().type))
        name = consume().value;
    else
        throw ParseError("Expected variable name (got '" + current().value + "')", current().line, current().col);

    std::string typeHint;
    if (check(TokenType::COLON))
    {
        consume(); // eat :
        if (check(TokenType::IDENTIFIER) || isCTypeKeyword(current().type))
            typeHint = consume().value;
    }

    ASTNodePtr init;
    if (match(TokenType::ASSIGN))
        init = parseExpr();

    // Multi-var: const W = 60, H = 24  /  let x = 1, y = 2
    if (check(TokenType::COMMA))
    {
        auto block = std::make_unique<ASTNode>(BlockStmt{}, ln);
        block->as<BlockStmt>().statements.push_back(
            std::make_unique<ASTNode>(VarDecl{isConst, name, std::move(init), typeHint}, ln));
        while (match(TokenType::COMMA))
        {
            skipNewlines();
            if (atEnd() || check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                break;
            std::string n2;
            if (check(TokenType::IDENTIFIER))
                n2 = consume().value;
            else if (isCTypeKeyword(current().type))
                n2 = consume().value;
            else
                break;
            
            std::string h2;
            if (check(TokenType::COLON))
            {
                consume();
                if (check(TokenType::IDENTIFIER) || isCTypeKeyword(current().type))
                    h2 = consume().value;
            }

            ASTNodePtr init2;
            if (match(TokenType::ASSIGN))
                init2 = parseExpr();
            block->as<BlockStmt>().statements.push_back(
                std::make_unique<ASTNode>(VarDecl{isConst, n2, std::move(init2), h2}, ln));
        }
        while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
            consume();
        return block;
    }

    while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
        consume();
    return std::make_unique<ASTNode>(VarDecl{isConst, name, std::move(init), typeHint}, ln);
}

ASTNodePtr Parser::parseFunctionDecl()
{
    int ln = current().line;
    // Accept IDENTIFIER or keyword tokens used as function names (e.g. "input", "display")
    Token nameToken = check(TokenType::IDENTIFIER) ? consume() : (check(TokenType::INPUT) || check(TokenType::PRINT)) ? consume()
                                                                                                                      : expect(TokenType::IDENTIFIER, "Expected function name");
    std::vector<bool> paramIsRef;
    std::vector<ASTNodePtr> defaultArgs;
    std::vector<std::string> paramTypes;
    auto params = parseParamList(&paramIsRef, &defaultArgs, &paramTypes);

    std::string returnType;
    // Skip optional return type annotation: -> type  or  -> SomeType
    if (check(TokenType::ARROW) || check(TokenType::FAT_ARROW))
    {
        consume(); // eat -> or =>
        if (check(TokenType::IDENTIFIER) || isCTypeKeyword(current().type))
            returnType = consume().value;
        else {
            // consume tokens until we hit : or { or NEWLINE or INDENT
            while (!atEnd() && !check(TokenType::COLON) && !check(TokenType::LBRACE) && !check(TokenType::NEWLINE) && !check(TokenType::INDENT))
                consume();
        }
    }

    match(TokenType::COLON); // optional Python-style colon
    // C++ constructor initializer list: skip  member(val), member2(val2)  before body
    if (check(TokenType::IDENTIFIER))
    {
        size_t la = pos + 1;
        while (la < tokens.size() && tokens[la].type == TokenType::NEWLINE) la++;
        if (la < tokens.size() &&
            (tokens[la].type == TokenType::LPAREN || tokens[la].type == TokenType::LBRACE))
        {
            while (!atEnd() && !check(TokenType::LBRACE) && !check(TokenType::INDENT))
            {
                if (check(TokenType::NEWLINE)) {
                    size_t la2 = pos + 1;
                    while (la2 < tokens.size() && tokens[la2].type == TokenType::NEWLINE) la2++;
                    if (la2 >= tokens.size() || tokens[la2].type == TokenType::LBRACE
                        || tokens[la2].type == TokenType::INDENT) break;
                }
                consume();
            }
        }
    }
    skipNewlines();

    // ── C++ forward declaration / prototype support ───────────────────────────
    if (check(TokenType::SEMICOLON) || check(TokenType::NEWLINE) || atEnd())
    {
        while (check(TokenType::SEMICOLON) || check(TokenType::NEWLINE))
            consume();
        auto emptyBody = std::make_unique<ASTNode>(BlockStmt{}, ln);
        FunctionDecl fd;
        fd.name = nameToken.value;
        fd.params = std::move(params);
        fd.paramTypes = std::move(paramTypes);
        fd.paramIsRef = std::move(paramIsRef);
        fd.defaultArgs = std::move(defaultArgs);
        fd.returnType = returnType;
        fd.body = std::move(emptyBody);
        return std::make_unique<ASTNode>(std::move(fd), ln);
    }

    auto body = parseBlock();
    FunctionDecl fd;
    fd.name = nameToken.value;
    fd.params = std::move(params);
    fd.paramTypes = std::move(paramTypes);
    fd.paramIsRef = std::move(paramIsRef);
    fd.defaultArgs = std::move(defaultArgs);
    fd.returnType = returnType;
    fd.body = std::move(body);
    return std::make_unique<ASTNode>(std::move(fd), ln);
}

ASTNodePtr Parser::parseClassDecl()
{
    int ln = current().line;
    auto name = expect(TokenType::IDENTIFIER, "Expected class name").value;
    std::string base;

    // Python-style: class Foo(Bar): or class Foo(Bar, ABC):
    if (check(TokenType::LPAREN))
    {
        consume(); // eat '('
        // grab first base class name
        if (check(TokenType::IDENTIFIER))
            base = consume().value;
        else if (isCTypeKeyword(current().type))
            base = consume().value;
        // skip any additional bases: (A, B, C) — just use first
        while (check(TokenType::COMMA))
        {
            consume();
            if (check(TokenType::IDENTIFIER) || isCTypeKeyword(current().type))
                consume(); // discard extra bases
        }
        expect(TokenType::RPAREN, "Expected ')' after base class list");
    }
    // Quantum/JS-style: class Foo extends Bar
    else if (check(TokenType::EXTENDS))
    {
        consume();
        base = expect(TokenType::IDENTIFIER, "Expected base class name").value;
    }
    // C++-style: class Dog : public Animal {  (also private/protected/plain)
    else if (check(TokenType::COLON) && pos + 1 < tokens.size() &&
             tokens[pos + 1].type == TokenType::IDENTIFIER)
    {
        size_t la = pos + 1;
        if (tokens[la].value == "public" || tokens[la].value == "private" ||
            tokens[la].value == "protected")
            la++;
        size_t la2 = la + 1;
        while (la2 < tokens.size() && tokens[la2].type == TokenType::NEWLINE)
            la2++;
        if (la < tokens.size() && tokens[la].type == TokenType::IDENTIFIER &&
            la2 < tokens.size() &&
            (tokens[la2].type == TokenType::LBRACE || tokens[la2].type == TokenType::COMMA))
        {
            consume(); // eat ':'
            if (current().value == "public" || current().value == "private" ||
                current().value == "protected")
                consume();
            base = consume().value;
            // Extra bases: class C : public A, public B — keep the first only
            while (check(TokenType::COMMA))
            {
                consume();
                if (check(TokenType::IDENTIFIER) &&
                    (current().value == "public" || current().value == "private" ||
                     current().value == "protected"))
                    consume();
                if (check(TokenType::IDENTIFIER))
                    consume();
            }
        }
    }

    match(TokenType::COLON);
    skipNewlines();

    ClassDecl cd;
    cd.name = name;
    cd.base = base;

    auto parseClassBody = [&]()
    {
        skipNewlines();
        // Check for 'pass' which just means an empty body
        if (check(TokenType::IDENTIFIER) && current().value == "pass")
        {
            consume();
            skipNewlines();
            return;
        }

        while (!check(TokenType::RBRACE) && !check(TokenType::DEDENT) && !atEnd())
        {
            skipNewlines();
            if (check(TokenType::RBRACE) || check(TokenType::DEDENT))
                break;

            // Handle decorators on methods
            while (check(TokenType::DECORATOR))
            {
                consume(); // eat @
                if (check(TokenType::IDENTIFIER))
                {
                    consume(); // eat decorator name
                    if (check(TokenType::LPAREN))
                    {
                        consume(); // eat (
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
                skipNewlines();
            }

            bool isStatic = false;

            // Skip access modifiers and C++ qualifiers
            while (check(TokenType::IDENTIFIER) &&
                   (current().value == "public" || current().value == "private" ||
                    current().value == "protected" || current().value == "static" ||
                    current().value == "async" || current().value == "virtual" ||
                    current().value == "inline" || current().value == "explicit" ||
                    current().value == "override" || current().value == "final"))
            {
                if (current().value == "static")
                    isStatic = true;
                consume();
            }
            // Skip leading const (e.g. const int foo = 5;)
            if (check(TokenType::CONST))
                consume();

            // Optional fn/def/function keyword
            if (check(TokenType::FN) || check(TokenType::DEF) || check(TokenType::FUNCTION))
                consume();

            // Handle ~ClassName destructor syntax (C++ style)
            if (check(TokenType::BIT_NOT))
            {
                consume(); // eat ~
                if (check(TokenType::IDENTIFIER))
                    consume(); // eat class name
                std::vector<bool> destructorParamIsRef;
                auto params = parseParamList(&destructorParamIsRef);

                // Skip optional return type annotation: -> ReturnType
                if (check(TokenType::ARROW))
                {
                    consume();
                    while (!atEnd() && !check(TokenType::COLON) && !check(TokenType::LBRACE) && !check(TokenType::NEWLINE) && !check(TokenType::INDENT))
                        consume();
                }

                match(TokenType::COLON);
                skipNewlines();
                auto body = parseBlock();
                FunctionDecl delFd;
                delFd.name = "__del__";
                delFd.params = std::move(params);
                delFd.body = std::move(body);
                auto fn = std::make_unique<ASTNode>(std::move(delFd), ln);
                cd.methods.push_back(std::move(fn));
                skipNewlines();
                continue;
            }

            // Handle nested class definitions: class Node: ...
            if (check(TokenType::CLASS))
            {
                consume(); // eat 'class'
                // Parse the nested class and add it to the fields array
                cd.fields.push_back(parseClassDecl());
                skipNewlines();
                continue;
            }

            // Handle nested C++ struct definitions: struct Entry { ... };
            if (check(TokenType::IDENTIFIER) &&
                (current().value == "struct" || current().value == "union") &&
                pos + 1 < tokens.size() && tokens[pos + 1].type == TokenType::IDENTIFIER)
            {
                size_t laS = pos + 2;
                while (laS < tokens.size() && tokens[laS].type == TokenType::NEWLINE)
                    laS++;
                if (laS < tokens.size() && tokens[laS].type == TokenType::LBRACE)
                {
                    consume(); // eat 'struct' / 'union'
                    cd.fields.push_back(parseClassDecl());
                    skipNewlines();
                    continue;
                }
            }

            auto isMethodName = [&](TokenType t)
            {
                return t >= TokenType::IDENTIFIER && t <= TokenType::TYPE_UNSIGNED;
            };

            if (!isMethodName(current().type))
            {
                skipNewlines();
                // If it's a pass inside the class but not the first thing
                if (check(TokenType::IDENTIFIER) && current().value == "pass")
                {
                    consume();
                    skipNewlines();
                }
                else if (!check(TokenType::RBRACE) && !check(TokenType::DEDENT))
                {
                    consume(); // skip whatever garbage token this is to avoid infinite loops
                }
                continue;
            }
            // --- Detect C++ member variable declarations ---
            // Pattern: TypeName varName; OR TypeKeyword varName;
            // This is when we have IDENTIFIER followed by IDENTIFIER (then ; or =)
            // e.g.  string heroName;   OR   TownsvilleGuardian obj;
            // Also handles C type keywords: int x; double y;
            {
                bool isMemberVar = false;
                std::string typeToken;

                if (isCTypeKeyword(current().type))
                {
                    // e.g. int energyLevel; OR int getEnergyLevel()
                    // Peek ahead to determine if this is a field or method
                    size_t la = pos + 1;
                    while (la < tokens.size() && isCTypeKeyword(tokens[la].type))
                        la++;
                    // skip & or *
                    while (la < tokens.size() && (tokens[la].type == TokenType::BIT_AND || tokens[la].type == TokenType::STAR))
                        la++;
                    if (la < tokens.size() && tokens[la].type == TokenType::IDENTIFIER)
                    {
                        // Check if method name is followed by '('
                        size_t la2 = la + 1;
                        while (la2 < tokens.size() && tokens[la2].type == TokenType::NEWLINE)
                            la2++;
                        if ((la2 < tokens.size() && tokens[la2].type == TokenType::LPAREN) ||
                            tokens[la].value == "operator") // bool operator<(...), int& operator[](...)
                        {
                            // It's a method with return type: "int getEnergyLevel()"
                            // Consume the type tokens as return type and fall through to method parsing
                            consume(); // eat first type keyword
                            while (isCTypeKeyword(current().type))
                                consume(); // multi-word types
                            while (check(TokenType::BIT_AND) || check(TokenType::STAR))
                                consume();
                            // Fall through to method name parsing
                        }
                        else
                        {
                            // It's a field: "int energyLevel;"
                            typeToken = consume().value;
                            while (isCTypeKeyword(current().type))
                                typeToken += " " + consume().value;
                            while (check(TokenType::BIT_AND) || check(TokenType::STAR))
                                consume();
                            isMemberVar = true;
                        }
                    }
                    else
                    {
                        // No identifier after type — skip
                        consume();
                    }
                }
                else if (check(TokenType::IDENTIFIER))
                {
                    // Peek: if the token after this identifier is another identifier (var name)
                    // AND that identifier is NOT followed by '(' (i.e. it's a field, not a method)
                    size_t la = pos + 1;
                    // Skip template arguments on the return type: vector<int>, shared_ptr<Foo>, etc.
                    if (la < tokens.size() && tokens[la].type == TokenType::LT)
                    {
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
                            else if (tokens[la].type == TokenType::SEMICOLON ||
                                     tokens[la].type == TokenType::NEWLINE ||
                                     tokens[la].type == TokenType::LBRACE)
                                break;
                            la++;
                        }
                    }
                    while (la < tokens.size() && tokens[la].type == TokenType::BIT_AND)
                        la++;
                    // Also skip pointer stars between return type and method name
                    while (la < tokens.size() && tokens[la].type == TokenType::STAR)
                        la++;
                    if (la < tokens.size() && tokens[la].type == TokenType::IDENTIFIER && tokens[la].value != cd.name) // not constructor pattern
                    {
                        // Check if the name token is followed by '(' → it's a method, not a field
                        size_t la2 = la + 1;
                        while (la2 < tokens.size() && tokens[la2].type == TokenType::NEWLINE)
                            la2++;
                        if ((la2 < tokens.size() && tokens[la2].type == TokenType::LPAREN) ||
                            tokens[la].value == "operator") // Iterator& operator++()
                        {
                            // This is a return-type + method-name pattern (e.g. "string getHeroName()"
                            // or "vector<int> getNeighbors()")
                            // Consume the return type identifier and any template args
                            consume(); // eat return type (e.g. "string", "vector")
                            // Skip template arguments if present: <int>, <Node*>, etc.
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
                            while (check(TokenType::BIT_AND) || check(TokenType::STAR))
                                consume();
                            // Fall through to method name parsing below
                        }
                        else
                        {
                            typeToken = consume().value; // eat type name
                            // Skip template args on field type too
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
                            while (check(TokenType::BIT_AND) || check(TokenType::STAR))
                                consume();
                            isMemberVar = true;
                        }
                    }
                }

                if (isMemberVar)
                {
                    // It's a field declaration: varName [= init];
                    if (!isMethodName(current().type))
                    {
                        // skip garbage
                        while (!atEnd() && !check(TokenType::NEWLINE) && !check(TokenType::SEMICOLON))
                            consume();
                        while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                            consume();
                        continue;
                    }
                    // Parses one field (name, optional [dims], optional = init)
                    // and appends it — supports "int arr[MAX], top;" comma lists.
                    auto parseOneField = [&]()
                    {
                        while (check(TokenType::STAR) || check(TokenType::BIT_AND))
                            consume();
                        if (!isMethodName(current().type))
                            return false;
                        std::string fieldName = consume().value;
                        ASTNodePtr init;
                        std::vector<ASTNodePtr> dims;
                        while (check(TokenType::LBRACKET))
                        {
                            consume(); // eat '['
                            if (!check(TokenType::RBRACKET))
                            {
                                try
                                {
                                    dims.push_back(parseExpr());
                                }
                                catch (...)
                                {
                                }
                            }
                            if (check(TokenType::RBRACKET))
                                consume();
                        }
                        if (match(TokenType::ASSIGN))
                            init = parseExpr();
                        else if (!dims.empty())
                        {
                            // int arr[MAX]; — allocate a zero-filled array
                            CallExpr mk;
                            mk.callee = std::make_unique<ASTNode>(Identifier{"__c_array__"}, ln);
                            mk.args = std::move(dims);
                            init = std::make_unique<ASTNode>(std::move(mk), ln);
                        }
                        cd.fields.push_back(std::make_unique<ASTNode>(
                            VarDecl{false, fieldName, std::move(init), typeToken}, ln));
                        return true;
                    };
                    parseOneField();
                    while (check(TokenType::COMMA))
                    {
                        consume();
                        if (!parseOneField())
                            break;
                    }
                    while (!atEnd() && !check(TokenType::NEWLINE) && !check(TokenType::SEMICOLON) && !check(TokenType::RBRACE) && !check(TokenType::DEDENT))
                        consume(); // skip anything remaining on line
                    while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                        consume();
                    continue;
                }
            }

            // Accept keyword tokens as method names (print, from, etc.)
            std::string methodName = current().value;
            consume(); // eat the method name token regardless of its type

            // C++ operator overloading: operator[] operator() operator< operator++ ...
            // Map to Quantum magic-method names so the VM can dispatch them.
            if (methodName == "operator")
            {
                if (check(TokenType::LBRACKET) && pos + 1 < tokens.size() &&
                    tokens[pos + 1].type == TokenType::RBRACKET)
                {
                    consume(); // eat [
                    consume(); // eat ]
                    methodName = "__getitem__";
                }
                else if (check(TokenType::LPAREN) && pos + 1 < tokens.size() &&
                         tokens[pos + 1].type == TokenType::RPAREN &&
                         pos + 2 < tokens.size() && tokens[pos + 2].type == TokenType::LPAREN)
                {
                    consume(); // eat (
                    consume(); // eat )
                    methodName = "__call__";
                }
                else if (!check(TokenType::LPAREN))
                {
                    // Symbolic operator: consume operator tokens until the param list
                    std::string sym;
                    while (!atEnd() && !check(TokenType::LPAREN) && !check(TokenType::NEWLINE) &&
                           !check(TokenType::LBRACE))
                        sym += consume().value;
                    if (sym == "<")       methodName = "__lt__";
                    else if (sym == ">")  methodName = "__gt__";
                    else if (sym == "<=") methodName = "__le__";
                    else if (sym == ">=") methodName = "__ge__";
                    else if (sym == "==") methodName = "__eq__";
                    else if (sym == "!=") methodName = "__ne__";
                    else if (sym == "+")  methodName = "__add__";
                    else if (sym == "-")  methodName = "__sub__";
                    else if (sym == "*")  methodName = "__mul__";
                    else if (sym == "/")  methodName = "__div__";
                    else if (sym == "++") methodName = "__inc__";
                    else if (sym == "--") methodName = "__dec__";
                    else if (sym == "<<") methodName = "__lshift__";
                    else if (sym == ">>") methodName = "__rshift__";
                    else                  methodName = "operator" + sym;
                }
            }

            // Normalize constructor names
            if (methodName == "constructor" || methodName == "__init__" || methodName == cd.name)
                methodName = "init";
            // Normalize destructor names
            if (methodName == "destructor")
                methodName = "__del__";
            // Normalize toString
            if (methodName == "toString" || methodName == "to_string" || methodName == "to_str")
                methodName = "__str__";

            // Check for method vs field: if next token is NOT '(' it might be a field
            if (!check(TokenType::LPAREN))
            {
                // If followed by '=' or ':' it's a class-level attribute assignment
                if (check(TokenType::ASSIGN) || check(TokenType::COLON))
                {
                    if (check(TokenType::COLON))
                        consume(); // eat optional type hint colon (skip type)
                    if (check(TokenType::ASSIGN))
                        consume(); // eat '='
                    skipNewlines();
                    ASTNodePtr init;
                    try
                    {
                        init = parseExpr();
                    }
                    catch (...)
                    {
                    }
                    auto fld = std::make_unique<ASTNode>(
                        VarDecl{false, methodName, std::move(init), ""}, ln);
                    cd.fields.push_back(std::move(fld));
                    while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                        consume();
                    skipNewlines();
                    continue;
                }
                // Skip the rest of the line as a field/statement we can't parse
                while (!atEnd() && !check(TokenType::NEWLINE) && !check(TokenType::SEMICOLON) && !check(TokenType::RBRACE))
                    consume();
                while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
                    consume();
                skipNewlines();
                continue;
            }

            std::vector<bool> methodParamIsRef;
            auto params = parseParamList(&methodParamIsRef);

            // Skip trailing C++ const: method() const { }
            if (check(TokenType::CONST))
                consume();
            // Also handle if 'const' appears as identifier token somehow
            if (check(TokenType::IDENTIFIER) && current().value == "const")
                consume();
            // Skip noexcept, override, final qualifiers
            while (check(TokenType::IDENTIFIER) &&
                   (current().value == "noexcept" || current().value == "override" ||
                    current().value == "final"))
                consume();

            // Skip optional return type annotation: -> ReturnType  (Python style)
            if (check(TokenType::ARROW))
            {
                consume(); // eat ->
                // consume tokens until we hit : or { or NEWLINE or INDENT
                while (!atEnd() && !check(TokenType::COLON) && !check(TokenType::LBRACE) && !check(TokenType::NEWLINE) && !check(TokenType::INDENT))
                    consume();
            }

            // Allow a C++ initializer list that starts on the next line:
            //   Node(const K& k, shared_ptr<Node> n = nullptr)
            //       : key(k), next(n) {}
            if (check(TokenType::NEWLINE))
            {
                size_t laN = pos;
                while (laN < tokens.size() && tokens[laN].type == TokenType::NEWLINE)
                    ++laN;
                if (laN + 2 < tokens.size() && tokens[laN].type == TokenType::COLON &&
                    tokens[laN + 1].type == TokenType::IDENTIFIER &&
                    (tokens[laN + 2].type == TokenType::LPAREN ||
                     tokens[laN + 2].type == TokenType::LBRACE))
                {
                    while (check(TokenType::NEWLINE))
                        consume();
                }
            }

            // Only consume ':' here if it's a Python-style body colon (NOT a C++ initializer list).
            // A C++ initializer list looks like:  ) : memberName(  or  ) : memberName{
            // A Python body colon looks like:      ) :  NEWLINE/INDENT/{
            {
                bool isInitList = false;
                if (check(TokenType::COLON))
                {
                    // Look ahead: skip ':' and check if next non-whitespace is IDENTIFIER followed by '(' or '{'
                    size_t la = pos + 1;
                    while (la < tokens.size() && tokens[la].type == TokenType::NEWLINE)
                        ++la;
                    if (la < tokens.size() && tokens[la].type == TokenType::IDENTIFIER)
                    {
                        size_t la2 = la + 1;
                        if (la2 < tokens.size() &&
                            (tokens[la2].type == TokenType::LPAREN || tokens[la2].type == TokenType::LBRACE))
                            isInitList = true;
                    }
                }
                if (!isInitList)
                    match(TokenType::COLON);
            }

            std::vector<ASTNodePtr> initAssignments;
            // C++ constructor initializer list: ClassName() : member(val), member2(val2) { ... }
            // Detect this by checking if the current token is ':' (after params + qualifiers)
            if (check(TokenType::COLON))
            {
                consume(); // eat :
                // Parse comma-separated member initializers
                while (!atEnd() && !check(TokenType::LBRACE) && !check(TokenType::INDENT) && !check(TokenType::NEWLINE))
                {
                    if (check(TokenType::IDENTIFIER))
                    {
                        std::string memName = consume().value;
                        if (check(TokenType::LPAREN) || check(TokenType::LBRACE))
                        {
                            consume(); // eat ( or {
                            int depth = 1;
                            ASTNodePtr initExpr;
                            if (!check(TokenType::RPAREN) && !check(TokenType::RBRACE))
                            {
                                try
                                {
                                    initExpr = parseExpr();
                                }
                                catch (...)
                                {
                                }
                                // Skip remaining tokens in arg list
                                while (!atEnd() && depth > 0)
                                {
                                    if (check(TokenType::LPAREN) || check(TokenType::LBRACE))
                                        depth++;
                                    else if (check(TokenType::RPAREN) || check(TokenType::RBRACE))
                                    {
                                        depth--;
                                        if (depth == 0)
                                            break;
                                    }
                                    consume();
                                }
                            }
                            if (check(TokenType::RPAREN) || check(TokenType::RBRACE))
                                consume();
                            if (initExpr)
                            {
                                auto selfNode = std::make_unique<ASTNode>(Identifier{"self"}, ln);
                                auto memExpr = std::make_unique<ASTNode>(MemberExpr{std::move(selfNode), memName}, ln);
                                auto assign = std::make_unique<ASTNode>(AssignExpr{"=", std::move(memExpr), std::move(initExpr)}, ln);
                                initAssignments.push_back(std::make_unique<ASTNode>(ExprStmt{std::move(assign)}, ln));
                            }
                        }
                    }
                    else
                    {
                        consume();
                    }
                    if (check(TokenType::COMMA))
                        consume();
                }
            }
            skipNewlines();
            auto body = parseBlock();

            if (!initAssignments.empty())
            {
                if (body->is<BlockStmt>())
                {
                    auto &stmts = body->as<BlockStmt>().statements;
                    stmts.insert(stmts.begin(), std::make_move_iterator(initAssignments.begin()), std::make_move_iterator(initAssignments.end()));
                }
            }

            FunctionDecl methodFd;
            methodFd.name = methodName;
            methodFd.params = std::move(params);
            methodFd.paramIsRef = std::move(methodParamIsRef);
            methodFd.body = std::move(body);
            auto fn = std::make_unique<ASTNode>(std::move(methodFd), ln);

            if (isStatic)
                cd.staticMethods.push_back(std::move(fn));
            else
                cd.methods.push_back(std::move(fn));
            skipNewlines();
        }
    };

    if (check(TokenType::LBRACE))
    {
        consume();
        parseClassBody();
        expect(TokenType::RBRACE, "Expected \'}\'");
        // C++ style: class Foo { }; — skip trailing semicolon
        while (check(TokenType::SEMICOLON))
            consume();
    }
    else if (check(TokenType::INDENT))
    {
        consume();
        parseClassBody();
        if (check(TokenType::DEDENT))
            consume();
    }
    else if (check(TokenType::IDENTIFIER) && current().value == "pass")
    {
        // One-liner: class X: pass
        consume();
    }
    else if (check(TokenType::NEWLINE) || atEnd())
    {
        // Empty class with nothing but newlines (Python will fail but we can be lenient)
        skipNewlines();
    }
    else
        throw ParseError("Expected \'{\' or indented class body", current().line, current().col);

    return std::make_unique<ASTNode>(std::move(cd), ln);
}

ASTNodePtr Parser::parseIfStmt()
{
    int ln = current().line;

    // C++17 if-with-initializer: if (auto name = expr) { ... }
    // Token stream after 'if' is consumed: LPAREN IDENTIFIER("auto") IDENTIFIER(name) ASSIGN expr RPAREN
    if (check(TokenType::LPAREN) &&
        pos + 2 < tokens.size() &&
        tokens[pos + 1].type == TokenType::IDENTIFIER && tokens[pos + 1].value == "auto" &&
        tokens[pos + 2].type == TokenType::IDENTIFIER)
    {
        // Check that tokens[pos+3] is ASSIGN (and not == comparison)
        if (pos + 3 < tokens.size() && tokens[pos + 3].type == TokenType::ASSIGN)
        {
            consume();                             // eat '('
            consume();                             // eat 'auto'
            std::string varName = consume().value; // eat variable name
            consume();                             // eat '='
            auto initExpr = parseExpr();
            expect(TokenType::RPAREN, "Expected ')'");
            match(TokenType::COLON);
            skipNewlines();
            auto then = parseBodyOrStatement();
            skipNewlines();
            ASTNodePtr elseBranch;
            if (check(TokenType::ELSE))
            {
                consume();
                skipNewlines();
                if (check(TokenType::IF))
                {
                    consume();
                    elseBranch = parseIfStmt();
                }
                else
                {
                    match(TokenType::COLON);
                    skipNewlines();
                    elseBranch = parseBodyOrStatement();
                }
            }
            // Emit: { auto varName = initExpr; if (varName) { then } else { elseBranch } }
            auto varDecl = std::make_unique<ASTNode>(
                VarDecl{false, varName, std::move(initExpr), "auto"}, ln);
            auto condExpr = std::make_unique<ASTNode>(Identifier{varName}, ln);
            auto ifNode = std::make_unique<ASTNode>(
                IfStmt{std::move(condExpr), std::move(then), std::move(elseBranch)}, ln);
            BlockStmt blk;
            blk.statements.push_back(std::move(varDecl));
            blk.statements.push_back(std::move(ifNode));
            return std::make_unique<ASTNode>(std::move(blk), ln);
        }
    }

    auto cond = parseExpr();
    // consume optional colon (Python style: "if x > 0:")
    match(TokenType::COLON);
    skipNewlines();
    auto then = parseBodyOrStatement();
    skipNewlines();
    ASTNodePtr elseBranch;
    if (check(TokenType::ELIF))
    {
        consume();
        // optional colon after elif condition is handled inside recursive call
        elseBranch = parseIfStmt();
    }
    else if (check(TokenType::ELSE))
    {
        consume();
        skipNewlines();
        if (check(TokenType::IF))
        {
            consume();
            elseBranch = parseIfStmt();
        }
        else
        {
            match(TokenType::COLON); // optional colon: "else:"
            skipNewlines();
            elseBranch = parseBodyOrStatement();
        }
    }
    return std::make_unique<ASTNode>(IfStmt{std::move(cond), std::move(then), std::move(elseBranch)}, ln);
}

ASTNodePtr Parser::parseWhileStmt()
{
    int ln = current().line;
    auto cond = parseExpr();
    match(TokenType::COLON); // optional Python-style colon
    skipNewlines();
    auto body = parseBodyOrStatement();
    return std::make_unique<ASTNode>(WhileStmt{std::move(cond), std::move(body)}, ln);
}

// switch (x) { case a: ... case b: ... default: ... }
// Desugars to:
//   { let __sw_val = x
//     let __sw_hit = false
//     while (true) {
//         if (__sw_hit or __sw_val == a) { __sw_hit = true; <case-a stmts> }
//         if (__sw_hit or __sw_val == b) { __sw_hit = true; <case-b stmts> }
//         <default stmts>          // fallthrough and no-match both reach here
//         break
//     } }
// A 'break' inside a case body exits the while — matching C switch semantics,
// and omitting it falls through to the next case, also matching C.
ASTNodePtr Parser::parseSwitchStmt()
{
    int ln = current().line;
    consume(); // eat 'switch'
    expect(TokenType::LPAREN, "Expected '(' after switch");
    auto subject = parseExpr();
    expect(TokenType::RPAREN, "Expected ')' after switch expression");
    skipNewlines();
    expect(TokenType::LBRACE, "Expected '{' to open switch body");
    skipNewlines();

    static int swCounter = 0;
    std::string valName = "__sw_val_" + std::to_string(swCounter);
    std::string hitName = "__sw_hit_" + std::to_string(swCounter);
    swCounter++;

    BlockStmt outer;
    outer.statements.push_back(std::make_unique<ASTNode>(
        VarDecl{false, valName, std::move(subject), ""}, ln));
    outer.statements.push_back(std::make_unique<ASTNode>(
        VarDecl{false, hitName, std::make_unique<ASTNode>(BoolLiteral{false}, ln), ""}, ln));

    BlockStmt loopBody;

    auto atCaseKeyword = [&](const char *kw)
    {
        return check(TokenType::IDENTIFIER) && current().value == kw;
    };
    auto atAnyCase = [&]()
    {
        return atCaseKeyword("case") || atCaseKeyword("default");
    };

    while (!atEnd() && !check(TokenType::RBRACE))
    {
        skipNewlines();
        if (check(TokenType::RBRACE) || atEnd())
            break;
        if (atCaseKeyword("case"))
        {
            int cln = current().line;
            consume(); // eat 'case'
            auto caseVal = parseExpr();
            expect(TokenType::COLON, "Expected ':' after case value");
            skipNewlines();

            BlockStmt caseBody;
            // __sw_hit = true  (records the match so later cases fall through)
            caseBody.statements.push_back(std::make_unique<ASTNode>(ExprStmt{
                std::make_unique<ASTNode>(AssignExpr{"=",
                    std::make_unique<ASTNode>(Identifier{hitName}, cln),
                    std::make_unique<ASTNode>(BoolLiteral{true}, cln)}, cln)}, cln));
            while (!atEnd() && !check(TokenType::RBRACE) && !atAnyCase())
            {
                caseBody.statements.push_back(parseStatement());
                skipNewlines();
            }
            // if (__sw_hit or __sw_val == caseVal) { <caseBody> }
            auto eq = std::make_unique<ASTNode>(BinaryExpr{"==",
                std::make_unique<ASTNode>(Identifier{valName}, cln),
                std::move(caseVal)}, cln);
            auto cond = std::make_unique<ASTNode>(BinaryExpr{"or",
                std::make_unique<ASTNode>(Identifier{hitName}, cln),
                std::move(eq)}, cln);
            loopBody.statements.push_back(std::make_unique<ASTNode>(IfStmt{
                std::move(cond),
                std::make_unique<ASTNode>(std::move(caseBody), cln),
                nullptr}, cln));
        }
        else if (atCaseKeyword("default"))
        {
            consume(); // eat 'default'
            expect(TokenType::COLON, "Expected ':' after default");
            skipNewlines();
            // Default runs whenever control reaches it (fallthrough or no match)
            while (!atEnd() && !check(TokenType::RBRACE) && !atAnyCase())
            {
                loopBody.statements.push_back(parseStatement());
                skipNewlines();
            }
        }
        else
        {
            consume(); // skip stray token to avoid an infinite loop
        }
    }
    expect(TokenType::RBRACE, "Expected '}' to close switch");

    loopBody.statements.push_back(std::make_unique<ASTNode>(BreakStmt{}, ln));
    outer.statements.push_back(std::make_unique<ASTNode>(WhileStmt{
        std::make_unique<ASTNode>(BoolLiteral{true}, ln),
        std::make_unique<ASTNode>(std::move(loopBody), ln)}, ln));
    return std::make_unique<ASTNode>(std::move(outer), ln);
}

ASTNodePtr Parser::parseForStmt()
{
    int ln = current().line;

    // C-style for: for (init; condition; post) { body }
    // Detected by: for ( ...
    if (check(TokenType::LPAREN))
    {
        consume(); // eat (

        // ── Init ──────────────────────────────────────────────────────────
        ASTNodePtr initNode;
        std::string forOfVar; // set if this turns out to be for-of/for-in
        if (!check(TokenType::SEMICOLON))
        {
            // ── C++ range-based for (generic): for ([const] Type[&*] name : xs) ──
            //    for (const auto& x : xs)  for (int v : c)  for (pair<int,int> p : v)
            {
                size_t la = pos;
                if (la < tokens.size() && tokens[la].type == TokenType::CONST)
                    la++;
                bool hasType = false;
                if (la < tokens.size() && isCTypeKeyword(tokens[la].type))
                {
                    while (la < tokens.size() && isCTypeKeyword(tokens[la].type))
                        la++;
                    hasType = true;
                }
                else if (la < tokens.size() && tokens[la].type == TokenType::IDENTIFIER)
                {
                    la++;
                    hasType = true;
                    // Skip template arguments after the type name: pair<int,int>
                    if (la < tokens.size() && tokens[la].type == TokenType::LT)
                    {
                        int tdepth = 1;
                        la++;
                        while (la < tokens.size() && tdepth > 0)
                        {
                            if (tokens[la].type == TokenType::LT)
                                tdepth++;
                            else if (tokens[la].type == TokenType::GT)
                                tdepth--;
                            else if (tokens[la].type == TokenType::RSHIFT)
                                tdepth -= 2;
                            la++;
                        }
                    }
                }
                // Skip pointer/ref/const qualifiers: auto& x, Node* n
                while (la < tokens.size() &&
                       (tokens[la].type == TokenType::STAR ||
                        tokens[la].type == TokenType::BIT_AND ||
                        tokens[la].type == TokenType::CONST))
                    la++;
                if (hasType && la + 1 < tokens.size() &&
                    tokens[la].type == TokenType::IDENTIFIER &&
                    tokens[la + 1].type == TokenType::COLON &&
                    (la + 2 >= tokens.size() || tokens[la + 2].type != TokenType::COLON))
                {
                    // Consume everything up to the variable name → for-in loop
                    while (pos < la)
                        consume();
                    std::string rangeVar = consume().value;
                    consume(); // eat ':'
                    auto iterable = parseExpr();
                    expect(TokenType::RPAREN, "Expected ')'");
                    match(TokenType::COLON);
                    skipNewlines();
                    auto body = parseBodyOrStatement();
                    return std::make_unique<ASTNode>(ForStmt{rangeVar, "", std::move(iterable), std::move(body)}, ln);
                }
            }
            // let/const/type-keyword declaration OR plain expression
            if (check(TokenType::LET) || check(TokenType::CONST))
            {
                bool isConst = current().type == TokenType::CONST;
                consume();
                // Peek: if next is identifier/type-kw followed by 'in'/'of' → for-in/for-of
                size_t la = pos;
                bool isDestruct = false;
                if (la < tokens.size() && tokens[la].type == TokenType::LBRACKET)
                {
                    isDestruct = true;
                    int depth = 1;
                    la++;
                    while (la < tokens.size() && depth > 0)
                    {
                        if (tokens[la].type == TokenType::LBRACKET)
                            depth++;
                        else if (tokens[la].type == TokenType::RBRACKET)
                            depth--;
                        la++;
                    }
                }

                if ((la < tokens.size() && (tokens[la].type == TokenType::IDENTIFIER || isCTypeKeyword(tokens[la].type))) || isDestruct)
                {
                    size_t la2 = isDestruct ? la : la + 1;
                    if (la2 < tokens.size() &&
                        (tokens[la2].type == TokenType::IN || tokens[la2].type == TokenType::OF))
                    {
                        // for (let x of iterable) — treat as for-of
                        if (isDestruct)
                        {
                            std::string varStr = consume().value; // [
                            while (!check(TokenType::RBRACKET) && !atEnd())
                                varStr += consume().value;
                            varStr += consume().value; // ]
                            forOfVar = varStr;
                        }
                        else
                        {
                            forOfVar = consume().value; // var name
                        }
                        consume(); // eat 'in' or 'of'
                        auto iterable = parseExpr();
                        expect(TokenType::RPAREN, "Expected ')'");
                        match(TokenType::COLON);
                        skipNewlines();
                        auto body = parseBodyOrStatement();
                        return std::make_unique<ASTNode>(ForStmt{forOfVar, "", std::move(iterable), std::move(body)}, ln);
                    }
                }
                initNode = parseVarDecl(isConst);
            }
            else if (isCTypeKeyword(current().type))
            {
                size_t la = pos + 1;
                while (la < tokens.size() && isCTypeKeyword(tokens[la].type))
                    ++la;
                if (la < tokens.size() && tokens[la].type == TokenType::IDENTIFIER)
                {
                    // ── C++ range-based for: for (int v : container) ─────────────────
                    // Detect by peeking past the variable name for a bare COLON (not ::)
                    size_t la2 = la + 1;
                    // Skip pointer/ref qualifiers between type and name: int* v, int& v
                    while (la < tokens.size() &&
                           (tokens[la].type == TokenType::STAR ||
                            tokens[la].type == TokenType::BIT_AND ||
                            tokens[la].type == TokenType::CONST))
                        la++;
                    // Recalculate var-name position after qualifiers
                    if (la < tokens.size() && tokens[la].type == TokenType::IDENTIFIER)
                        la2 = la + 1;
                    bool isRangeFor = (la2 < tokens.size() &&
                                       tokens[la2].type == TokenType::COLON &&
                                       (la2 + 1 >= tokens.size() ||
                                        tokens[la2 + 1].type != TokenType::COLON)); // exclude ::
                    if (isRangeFor)
                    {
                        // Consume type keyword(s) and optional qualifiers
                        while (isCTypeKeyword(current().type))
                            consume();
                        while (check(TokenType::STAR) || check(TokenType::BIT_AND) || check(TokenType::CONST))
                            consume();
                        // Consume variable name
                        std::string rangeVar = consume().value; // IDENTIFIER
                        consume(); // eat ':'
                        auto iterable = parseExpr();
                        expect(TokenType::RPAREN, "Expected ')'");
                        match(TokenType::COLON);
                        skipNewlines();
                        auto body = parseBodyOrStatement();
                        return std::make_unique<ASTNode>(ForStmt{rangeVar, "", std::move(iterable), std::move(body)}, ln);
                    }
                    // ── Reset la in case the qualifier scan above advanced it ─────────
                    la = pos + 1;
                    while (la < tokens.size() && isCTypeKeyword(tokens[la].type))
                        ++la;
                    // ── Normal C-style for init: for (int i = 0; ...) ────────────────
                    auto hint = consume().value;
                    while (isCTypeKeyword(current().type))
                        hint += " " + consume().value;

                    auto firstDecl = parseCTypeVarDecl(hint);
                    // Eat any trailing semicolon that parseCTypeVarDecl left behind
                    // (it no longer consumes semicolons itself — for-loop separator
                    //  is consumed by the "while (check SEMICOLON) consume" below)
                    if (check(TokenType::COMMA))
                    {
                        auto block = std::make_unique<ASTNode>(BlockStmt{}, ln);
                        block->as<BlockStmt>().statements.push_back(std::move(firstDecl));
                        while (match(TokenType::COMMA))
                        {
                            std::string nextHint = hint;
                            if (isCTypeKeyword(current().type) || check(TokenType::CONST))
                            {
                                nextHint = consume().value;
                                while (isCTypeKeyword(current().type) || check(TokenType::CONST))
                                    nextHint += " " + consume().value;
                            }
                            block->as<BlockStmt>().statements.push_back(parseCTypeVarDecl(nextHint));
                        }
                        initNode = std::move(block);
                    }
                    else
                    {
                        initNode = std::move(firstDecl);
                    }
                }
                else
                    initNode = std::make_unique<ASTNode>(ExprStmt{parseExpr()}, ln);
            }
            else if (check(TokenType::IDENTIFIER))
            {
                // ── C++ identifier-typed for init: for (size_t i = 0; ...) ────
                //    also  for (Node* curr = head; ...)
                size_t la = pos + 1;
                // Skip template arguments after the type name
                if (la < tokens.size() && tokens[la].type == TokenType::LT)
                {
                    int tdepth = 1;
                    la++; // past '<'
                    while (la < tokens.size() && tdepth > 0)
                    {
                        if (tokens[la].type == TokenType::LT)
                            tdepth++;
                        else if (tokens[la].type == TokenType::GT)
                            tdepth--;
                        else if (tokens[la].type == TokenType::RSHIFT)
                            tdepth -= 2;
                        la++;
                    }
                }
                // Skip pointer/ref/const qualifiers
                while (la < tokens.size() &&
                       (tokens[la].type == TokenType::STAR ||
                        tokens[la].type == TokenType::BIT_AND ||
                        tokens[la].type == TokenType::CONST))
                    la++;
                bool isTypedInit = la + 1 < tokens.size() &&
                                   tokens[la].type == TokenType::IDENTIFIER &&
                                   tokens[la + 1].type == TokenType::ASSIGN;
                if (isTypedInit)
                {
                    std::string hint = consume().value; // type name
                    // Consume template args / qualifiers up to the var name
                    while (pos < la)
                        consume();
                    initNode = parseCTypeVarDecl(hint);
                }
                else
                {
                    // Plain expression init
                    auto expr = parseExpr();
                    initNode = std::make_unique<ASTNode>(ExprStmt{std::move(expr)}, ln);
                }
            }
            else
            {
                auto expr = parseExpr();
                initNode = std::make_unique<ASTNode>(ExprStmt{std::move(expr)}, ln);
            }
        }
        while (check(TokenType::SEMICOLON))
            consume();

        // ── Condition ─────────────────────────────────────────────────────
        ASTNodePtr condition;
        if (!check(TokenType::SEMICOLON))
            condition = parseExpr();
        else
            condition = std::make_unique<ASTNode>(BoolLiteral{true}, ln); // infinite if omitted
        while (check(TokenType::SEMICOLON))
            consume();

        // ── Post expression ───────────────────────────────────────────────
        ASTNodePtr postNode;
        if (!check(TokenType::RPAREN))
        {
            auto firstExpr = parseExpr();
            if (check(TokenType::COMMA))
            {
                auto block = std::make_unique<ASTNode>(BlockStmt{}, ln);
                block->as<BlockStmt>().statements.push_back(std::make_unique<ASTNode>(ExprStmt{std::move(firstExpr)}, ln));
                while (match(TokenType::COMMA))
                {
                    block->as<BlockStmt>().statements.push_back(std::make_unique<ASTNode>(ExprStmt{parseExpr()}, ln));
                }
                postNode = std::move(block);
            }
            else
            {
                postNode = std::make_unique<ASTNode>(ExprStmt{std::move(firstExpr)}, ln);
            }
        }
        expect(TokenType::RPAREN, "Expected ')'");
        match(TokenType::COLON);
        skipNewlines();

        // ── Body ──────────────────────────────────────────────────────────
        auto rawBody = parseBodyOrStatement();

        // Append post expression at end of body block
        BlockStmt loopBlock;
        // Copy existing body statements
        for (auto &s : rawBody->as<BlockStmt>().statements)
            loopBlock.statements.push_back(std::move(const_cast<ASTNodePtr &>(s)));
        if (postNode)
            loopBlock.statements.push_back(std::move(postNode));
        auto loopBody = std::make_unique<ASTNode>(std::move(loopBlock), ln);

        // Build: while (condition) { body; post }
        auto whileNode = std::make_unique<ASTNode>(
            WhileStmt{std::move(condition), std::move(loopBody)}, ln);

        // Wrap in block: { init; while(...){...} }
        BlockStmt outer;
        if (initNode)
            outer.statements.push_back(std::move(initNode));
        outer.statements.push_back(std::move(whileNode));
        return std::make_unique<ASTNode>(std::move(outer), ln);
    }

    // Python / Quantum / JS for-in / for-of
    // Supports tuple unpacking: for k, v in dict.items()
    // Also accepts type keywords as variable names
    auto readLoopVar = [&]() -> std::string
    {
        if (check(TokenType::IDENTIFIER))
            return consume().value;
        if (isCTypeKeyword(current().type))
            return consume().value;
        throw ParseError("Expected variable in for loop (got '" + current().value + "')", current().line, current().col);
        return "";
    };

    std::string var = readLoopVar();
    std::string var2; // second variable for tuple unpacking

    // Tuple unpacking: for k, v in ...
    if (check(TokenType::COMMA))
    {
        consume();
        var2 = readLoopVar();
    }

    // Accept both 'in' and 'of' (JavaScript for...of)
    if (!match(TokenType::IN) && !match(TokenType::OF))
        throw ParseError("Expected 'in' or 'of' in for loop", current().line, current().col);

    auto iterable = parseExpr();
    match(TokenType::COLON);
    skipNewlines();
    auto body = parseBodyOrStatement();
    return std::make_unique<ASTNode>(ForStmt{var, var2, std::move(iterable), std::move(body)}, ln);
}

ASTNodePtr Parser::parseReturnStmt()
{
    int ln = current().line;
    ASTNodePtr val;
    // A value-less `return` can also be closed by the end of its enclosing
    // block on the same line — `if (x) { return }` — not just by a newline
    // or semicolon.
    if (!check(TokenType::NEWLINE) && !check(TokenType::SEMICOLON) &&
        !check(TokenType::RBRACE) && !atEnd())
    {
        val = parseExpr();
        // Tuple return: return a, b  or  return a, b, c
        if (check(TokenType::COMMA))
        {
            TupleLiteral tup;
            tup.elements.push_back(std::move(val));
            while (match(TokenType::COMMA))
            {
                if (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON) || atEnd())
                    break;
                tup.elements.push_back(parseExpr());
            }
            val = std::make_unique<ASTNode>(std::move(tup), ln);
        }
    }
    while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
        consume();
    return std::make_unique<ASTNode>(ReturnStmt{std::move(val)}, ln);
}

ASTNodePtr Parser::parsePrintStmt()
{
    int ln = current().line;
    bool newline = true;
    std::string sep = " ";
    std::string end_str = "\n";
    std::vector<ASTNodePtr> args;
    if (check(TokenType::LPAREN))
    {
        consume();
        skipNewlines();
        while (!check(TokenType::RPAREN) && !atEnd())
        {
            // Detect keyword arguments: sep=, end=, file=, flush=
            if (check(TokenType::IDENTIFIER) &&
                (current().value == "sep" || current().value == "end" ||
                 current().value == "file" || current().value == "flush") &&
                pos + 1 < tokens.size() && tokens[pos + 1].type == TokenType::ASSIGN)
            {
                std::string kw = consume().value; // eat keyword name
                consume();                        // eat '='
                if (kw == "sep")
                {
                    if (check(TokenType::STRING))
                        sep = consume().value;
                    else
                        parseExpr(); // consume but discard non-literal
                }
                else if (kw == "end")
                {
                    if (check(TokenType::STRING))
                    {
                        end_str = consume().value;
                        newline = false; // end= overrides default newline
                    }
                    else
                        parseExpr();
                }
                else
                {
                    parseExpr(); // file= / flush= — consume and discard
                }
            }
            else
            {
                args.push_back(parseExpr());
            }
            skipNewlines();
            if (!match(TokenType::COMMA))
                break;
            skipNewlines();
        }
        expect(TokenType::RPAREN, "Expected ')'");
    }
    else
    {
        args.push_back(parseExpr());
        while (match(TokenType::COMMA))
            args.push_back(parseExpr());
    }
    while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
        consume();
    PrintStmt ps;
    ps.args = std::move(args);
    ps.newline = newline;
    ps.sep = sep;
    ps.end = end_str;
    return std::make_unique<ASTNode>(std::move(ps), ln);
}

ASTNodePtr Parser::parseInputStmt()
{
    // Handles:
    //   scanf("%d", &var)     — C-style: format string + address-of target
    //   input("prompt", var)  — prompt + target variable
    //   input(var)            — just target variable
    int ln = current().line;
    std::string target;
    ASTNodePtr prompt;

    if (check(TokenType::LPAREN))
    {
        consume();
        if (check(TokenType::STRING))
        {
            // First arg is a string: either a format string or a prompt
            auto fmtTok = current();
            consume();
            if (match(TokenType::COMMA))
            {
                // scanf("%d", &var) or input("prompt", var)
                prompt = std::make_unique<ASTNode>(StringLiteral{fmtTok.value}, ln);
                if (check(TokenType::BIT_AND))
                    consume(); // strip optional &
                target = expect(TokenType::IDENTIFIER, "Expected variable name after ','").value;
            }
            else
            {
                // input("prompt") — prompt only, no target
                prompt = std::make_unique<ASTNode>(StringLiteral{fmtTok.value}, ln);
                target = "";
            }
        }
        else
        {
            // input(&var) or input(var)
            if (check(TokenType::BIT_AND))
                consume();
            target = expect(TokenType::IDENTIFIER, "Expected variable name").value;
        }
        expect(TokenType::RPAREN, "Expected ')'");
    }
    while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
        consume();
    return std::make_unique<ASTNode>(InputStmt{target, std::move(prompt)}, ln);
}

ASTNodePtr Parser::parseCoutStmt()
{
    // cout << expr1 << expr2 << endl;
    // We must NOT call parseExpr() here because parseShift() inside it would
    // greedily consume << as a bitwise-shift operator.
    // Instead we call parseAddSub() — one level below shift — so each <<
    // stays available as the stream-insertion separator.
    int ln = current().line;
    std::vector<ASTNodePtr> args;
    bool newline = false;

    while (true)
    {
        skipNewlines();
        if (!check(TokenType::LSHIFT))
            break;
        consume(); // eat <<

        skipNewlines();

        // "endl" triggers a newline (no value pushed)
        if (check(TokenType::IDENTIFIER) && current().value == "endl")
        {
            consume();
            newline = true;
            continue;
        }

        // Parse the next segment at add/sub precedence so << isn't swallowed
        auto expr = parseAddSub();

        // If the segment is a string ending with \n, keep it as-is (contains the newline)
        // Only treat a bare "\n" string as endl
        if (expr->is<StringLiteral>() && expr->as<StringLiteral>().value == "\n")
            newline = true;
        else
            args.push_back(std::move(expr));
    }

    while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
        consume();

    return std::make_unique<ASTNode>(PrintStmt{std::move(args), newline}, ln);
}

ASTNodePtr Parser::parseCinStmt()
{
    // cin >> var  /  cin >> arr[i]  /  cin >> *(ptr+i)  /  cin >> var1 >> var2;
    // We cannot use parseExpr() because >> would be consumed as shift operator.
    // Strategy: if the target starts with * and (, parse the full parenthesised
    // expression inside, then wrap in DerefExpr. Otherwise parse postfix only.
    int ln = current().line;

    // Handle cin.ignore() / cin.get(...) / cin.getline(...) etc. — treat as no-ops.
    if (check(TokenType::DOT))
    {
        consume(); // eat '.'
        if (check(TokenType::IDENTIFIER))
            consume(); // eat method name (ignore, get, getline, peek, ...)
        // eat argument list if present
        if (check(TokenType::LPAREN))
        {
            consume(); // eat '('
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
        while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
            consume();
        return std::make_unique<ASTNode>(BlockStmt{}, ln);
    }

    BlockStmt block;
    while (true)
    {
        skipNewlines();
        if (!check(TokenType::RSHIFT))
            break;
        consume(); // eat >>
        if (check(TokenType::BIT_AND))
            consume(); // strip optional &

        ASTNodePtr lval;

        if (check(TokenType::STAR))
        {
            // *(expr) — dereference of an arbitrary expression in parens
            consume(); // eat *
            if (check(TokenType::LPAREN))
            {
                consume();                // eat (
                auto inner = parseExpr(); // safe: balanced parens, >> inside parens is shift
                expect(TokenType::RPAREN, "Expected ')' after dereferenced expression");
                DerefExpr de;
                de.operand = std::move(inner);
                lval = std::make_unique<ASTNode>(std::move(de), ln);
            }
            else
            {
                // *var — simple dereference
                auto operand = parsePrimary();
                DerefExpr de;
                de.operand = std::move(operand);
                lval = std::make_unique<ASTNode>(std::move(de), ln);
            }
        }
        else
        {
            // Simple lvalue: var, arr[i], obj.field
            lval = parsePrimary();
            // Handle postfix: arr[i], obj.field, ->member
            while (true)
            {
                if (check(TokenType::LBRACKET))
                {
                    consume();
                    auto idx = parseExpr();
                    expect(TokenType::RBRACKET, "Expected ']'");
                    IndexExpr ie;
                    ie.object = std::move(lval);
                    ie.index = std::move(idx);
                    lval = std::make_unique<ASTNode>(std::move(ie), ln);
                }
                else if (check(TokenType::DOT))
                {
                    consume();
                    auto member = expect(TokenType::IDENTIFIER, "Expected member name").value;
                    lval = std::make_unique<ASTNode>(MemberExpr{std::move(lval), member}, ln);
                }
                else if (check(TokenType::ARROW))
                {
                    consume();
                    auto member = expect(TokenType::IDENTIFIER, "Expected member name").value;
                    lval = std::make_unique<ASTNode>(ArrowExpr{std::move(lval), member}, ln);
                }
                else
                    break;
            }
        }

        InputStmt is;
        is.prompt = nullptr;
        if (lval->is<Identifier>())
            is.target = lval->as<Identifier>().name;
        else
            is.lvalueTarget = std::move(lval);

        block.statements.push_back(std::make_unique<ASTNode>(std::move(is), ln));
    }
    while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
        consume();

    if (block.statements.size() == 1)
        return std::move(block.statements[0]);
    return std::make_unique<ASTNode>(std::move(block), ln);
}

ASTNodePtr Parser::parseImportStmt(bool isFrom)
{
    int ln = current().line;
    ImportStmt stmt;

    if (isFrom)
    {
        // from module.sub import A, B
        // Actually, we'll just read an identifier (maybe with dots in the future)
        stmt.module = expect(TokenType::IDENTIFIER, "Expected module name after 'from'").value;
        expect(TokenType::IMPORT, "Expected 'import' after module name in 'from' statement");

        do
        {
            ImportStmt::Item item;
            item.name = expect(TokenType::IDENTIFIER, "Expected item name to import").value;
            if (match(TokenType::AS))
            {
                item.alias = expect(TokenType::IDENTIFIER, "Expected alias after 'as'").value;
            }
            stmt.imports.push_back(item);
        } while (match(TokenType::COMMA));
    }
    else
    {
        // import A as B, C
        stmt.module = ""; // No base module, importing directly
        do
        {
            ImportStmt::Item item;
            item.name = expect(TokenType::IDENTIFIER, "Expected module name to import").value;
            if (match(TokenType::AS))
            {
                item.alias = expect(TokenType::IDENTIFIER, "Expected alias after 'as'").value;
            }
            stmt.imports.push_back(item);
        } while (match(TokenType::COMMA));
    }

    while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
        consume();
    return std::make_unique<ASTNode>(std::move(stmt), ln);
}

ASTNodePtr Parser::parseExprStmt()
{
    int ln = current().line;
    auto expr = parseExpr();

    // C comma expression: "a += 3, b = a" — execute left-to-right as multiple stmts
    if (check(TokenType::COMMA))
    {
        BlockStmt block;
        block.statements.push_back(std::make_unique<ASTNode>(ExprStmt{std::move(expr)}, ln));
        while (match(TokenType::COMMA))
        {
            skipNewlines();
            if (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON) || atEnd())
                break;
            int eln = current().line;
            auto next = parseExpr();
            block.statements.push_back(std::make_unique<ASTNode>(ExprStmt{std::move(next)}, eln));
        }
        while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
            consume();
        return std::make_unique<ASTNode>(std::move(block), ln);
    }

    while (check(TokenType::NEWLINE) || check(TokenType::SEMICOLON))
        consume();
    return std::make_unique<ASTNode>(ExprStmt{std::move(expr)}, ln);
}

// ─── Expression Parsing (Pratt precedence) ───────────────────────────────────

ASTNodePtr Parser::parseExpr() { return parseAssignment(); }
