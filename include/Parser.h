#pragma once
#include "Token.h"
#include "AST.h"
#include <vector>
#include <stdexcept>

class ParseError : public std::runtime_error
{
public:
    int line, col;
    ParseError(const std::string &msg, int l, int c)
        : std::runtime_error(msg), line(l), col(c) {}
};

class Parser
{
public:
    explicit Parser(std::vector<Token> tokens);
    ASTNodePtr parse();

private:
    std::vector<Token> tokens;
    size_t pos;
    bool inCallArgList = false;

    // > 0 while parsing a C/C++ initializer (`vector<P> v = {...}`, a return
    // in a C++ function): there `{a, b}` is a brace-init list, not a JS
    // shorthand object or Python set.
    int braceInitDepth_ = 0;
    struct BraceInitScope
    {
        int &depth;
        explicit BraceInitScope(int &d) : depth(d) { ++depth; }
        ~BraceInitScope() { --depth; }
    };
    // Whether each enclosing function body was declared C/C++-style (with a
    // return type). Set for the next parseFunctionDecl by nextFnIsCpp_.
    std::vector<bool> fnIsCpp_;
    bool nextFnIsCpp_ = false;
    // True when the '{' at pos holds no top-level `key: value` colon.
    bool braceIsInitList() const;
    // Merges same-named C++ methods into one dispatcher that picks an
    // overload by argument count and type at call time.
    void buildOverloadDispatchers(ClassDecl &cd, int ln);

    // Token helpers
    Token &current();
    Token &peek(int offset = 1);
    Token &consume();
    Token &expect(TokenType t, const std::string &msg);
    bool check(TokenType t) const;
    bool match(TokenType t);
    bool atEnd() const;
    void skipNewlines();

    // Parsing methods
    ASTNodePtr parseStatement();
    ASTNodePtr parseBlock();
    ASTNodePtr parseBodyOrStatement(); // block OR single statement (brace-optional)
    ASTNodePtr parseVarDecl(bool isConst);
    ASTNodePtr parseDestructuringDecl(bool isConst);
    ASTNodePtr parseFunctionDecl();
    ASTNodePtr parseClassDecl();
    ASTNodePtr parseIfStmt();
    ASTNodePtr parseWhileStmt();
    ASTNodePtr parseForStmt();
    ASTNodePtr parseSwitchStmt(); // C/C++/JS switch — desugared to if-chain
    ASTNodePtr parseReturnStmt();
    ASTNodePtr parsePrintStmt();
    ASTNodePtr parseInputStmt();
    ASTNodePtr parseCoutStmt(); // cout << x << y << endl
    ASTNodePtr parseCinStmt();  // cin >> x >> y
    ASTNodePtr parseImportStmt(bool isFrom = false);
    ASTNodePtr parseExprStmt();
    ASTNodePtr parseCTypeVarDecl(const std::string &typeHint); // int x = ...  / int* p = ...
    bool isCTypeKeyword(TokenType t) const;

    // Expression parsing (Pratt-style precedence)
    ASTNodePtr parseExpr();
    ASTNodePtr parseAssignment();
    ASTNodePtr parseOr();
    ASTNodePtr parseAnd();
    ASTNodePtr parseBitwise();
    ASTNodePtr parseEquality();
    ASTNodePtr parseComparison();
    ASTNodePtr parseShift();
    ASTNodePtr parseAddSub();
    ASTNodePtr parseMulDiv();
    ASTNodePtr parsePower();
    ASTNodePtr parseUnary();
    ASTNodePtr parsePostfix();
    ASTNodePtr parsePrimary();

    ASTNodePtr parseArrayLiteral();
    ASTNodePtr parseDictLiteral();
    ASTNodePtr parseLambda();
    ASTNodePtr parseArrowFunction(std::vector<std::string> params, int ln);
    std::vector<ASTNodePtr> parseArgList();
    // Returns param names; populates outIsRef with true for each & (reference) param
    // outCppTypes receives each parameter's C/C++ declared type ("string",
    // "RopeNode*", "int"; "" when untyped) for overload resolution.
    std::vector<std::string> parseParamList(std::vector<bool> *outIsRef = nullptr, std::vector<ASTNodePtr> *outDefaultArgs = nullptr, std::vector<std::string> *outParamTypes = nullptr, std::vector<std::string> *outCppTypes = nullptr);
};
