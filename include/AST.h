#pragma once
#include <memory>
#include <string>
#include <vector>
#include <variant>

// Forward declarations
struct ASTNode;
using ASTNodePtr = std::unique_ptr<ASTNode>;

// ─── Expression Types ───────────────────────────────────────────────────────

struct NumberLiteral
{
    double value;
};
struct StringLiteral
{
    std::string value;
};
struct BoolLiteral
{
    bool value;
};
struct NilLiteral
{
};

struct Identifier
{
    std::string name;
};

struct BinaryExpr
{
    std::string op;
    ASTNodePtr left, right;
};

struct UnaryExpr
{
    std::string op;
    ASTNodePtr operand;
};

struct AssignExpr
{
    std::string op; // = += -= *= /=
    ASTNodePtr target;
    ASTNodePtr value;
};

struct CallExpr
{
    ASTNodePtr callee;
    std::vector<ASTNodePtr> args;
};

struct IndexExpr
{
    ASTNodePtr object;
    ASTNodePtr index;
};

// Python slice: obj[start:stop:step]  — any part may be null (omitted)
struct SliceExpr
{
    ASTNodePtr object;
    ASTNodePtr start; // may be null → 0
    ASTNodePtr stop;  // may be null → end
    ASTNodePtr step;  // may be null → 1
};

struct MemberExpr
{
    ASTNodePtr object;
    std::string member;
};

struct ArrayLiteral
{
    std::vector<ASTNodePtr> elements;
};

struct DictLiteral
{
    std::vector<std::pair<ASTNodePtr, ASTNodePtr>> pairs;
};

struct LambdaExpr
{
    std::vector<std::string> params;
    std::vector<std::string> paramTypes; // NEW: [param: type]
    std::vector<ASTNodePtr> defaultArgs;
    std::string returnType;              // NEW: fn(...) -> type
    ASTNodePtr body;
};

struct TernaryExpr
{
    ASTNodePtr condition;
    ASTNodePtr thenExpr;
    ASTNodePtr elseExpr;
};

// super(args) or super.method(args)
struct SuperExpr
{
    std::string method; // empty = super constructor call
};

// ─── C++ Pointer Expression Types ────────────────────────────────────────────

// &var — address-of operator
struct AddressOfExpr
{
    ASTNodePtr operand;
};

// *ptr — dereference operator
struct DerefExpr
{
    ASTNodePtr operand;
};

// ptr->member — arrow (member access through pointer)
struct ArrowExpr
{
    ASTNodePtr object;
    std::string member;
};

// ─── Statement Types ─────────────────────────────────────────────────────────

struct VarDecl
{
    bool isConst;
    std::string name;
    ASTNodePtr initializer; // may be null
    std::string typeHint;   // e.g. "int", "float", "char", "" = none
    bool isPointer = false; // int* p = ...
    bool isExported = false;
};

struct FunctionDecl
{
    std::string name;
    std::vector<std::string> params;
    std::vector<std::string> paramTypes; // NEW: [param: type]
    std::vector<bool> paramIsRef; // true = pass-by-reference (int& r), false = by-value
    std::vector<ASTNodePtr> defaultArgs;
    std::string returnType;              // NEW: fn name(...) -> int
    ASTNodePtr body;              // BlockStmt
    bool isExported = false;
};

struct ReturnStmt
{
    ASTNodePtr value; // may be null
};

struct IfStmt
{
    ASTNodePtr condition;
    ASTNodePtr thenBranch;
    // elif chains stored as else-if
    ASTNodePtr elseBranch; // may be null
};

struct WhileStmt
{
    ASTNodePtr condition;
    ASTNodePtr body;
};

struct ForStmt
{
    std::string var;
    std::string var2; // optional second variable for tuple unpacking: for k, v in ...
    ASTNodePtr iterable;
    ASTNodePtr body;
};

// List comprehension: [expr for var in iterable (if cond)?]
struct ListComp
{
    ASTNodePtr expr;               // the value expression
    std::vector<std::string> vars; // loop variable(s) — supports tuple unpacking
    ASTNodePtr iterable;
    ASTNodePtr condition; // optional if-filter (may be null)
};

struct TupleLiteral
{
    std::vector<ASTNodePtr> elements;
};

struct BlockStmt
{
    std::vector<ASTNodePtr> statements;
};

struct ExprStmt
{
    ASTNodePtr expr;
};

struct PrintStmt
{
    std::vector<ASTNodePtr> args;
    bool newline;
    std::string sep = " ";  // Python default: space between args
    std::string end = "\n"; // Python default: newline at end
};

struct InputStmt
{
    std::string target; // simple variable name (legacy)
    ASTNodePtr prompt;
    ASTNodePtr lvalueTarget; // optional: complex lvalue like arr[i]
};

struct BreakStmt
{
};
struct ContinueStmt
{
};
struct RaiseStmt
{
    ASTNodePtr value; // the exception value/message
};

struct ExceptClause
{
    std::string errorType; // e.g. "ValueError" — empty = bare except
    std::string alias;     // "as e" — empty if none
    std::shared_ptr<ASTNode> body;
};

struct TryStmt
{
    std::shared_ptr<ASTNode> body;
    std::vector<ExceptClause> handlers;
    std::shared_ptr<ASTNode> finallyBody; // may be null
};

struct ImportStmt
{
    std::string module; // e.g. "abc" for `from abc import...`, empty for `import sys`
    struct Item
    {
        std::string name;
        std::string alias; // empty if no alias
    };
    std::vector<Item> imports;
};

struct ClassDecl
{
    std::string name;
    std::string base; // optional
    std::vector<ASTNodePtr> methods;
    std::vector<ASTNodePtr> staticMethods;
    std::vector<ASTNodePtr> fields;
};

struct NewExpr
{
    std::string typeName;         // "int", "float", "MyClass", etc.
    std::vector<ASTNodePtr> args; // constructor args for new T(args)
    bool isArray = false;         // true for new T[n]
    ASTNodePtr sizeExpr;          // size expression for new T[n]
};

// ─── ASTNode variant ─────────────────────────────────────────────────────────

using NodeVariant = std::variant<
    NumberLiteral, StringLiteral, BoolLiteral, NilLiteral,
    Identifier,
    BinaryExpr, UnaryExpr, AssignExpr,
    CallExpr, IndexExpr, SliceExpr, MemberExpr,
    ArrayLiteral, DictLiteral, LambdaExpr, ListComp, TupleLiteral,
    VarDecl, FunctionDecl, ReturnStmt,
    IfStmt, WhileStmt, ForStmt,
    BlockStmt, ExprStmt,
    PrintStmt, InputStmt,
    BreakStmt, ContinueStmt, SuperExpr,
    RaiseStmt, TryStmt,
    ImportStmt, ClassDecl,
    TernaryExpr,
    AddressOfExpr, DerefExpr, ArrowExpr,
    NewExpr>;

struct ASTNode
{
    NodeVariant node;
    int line = 0;

    template <typename T>
    ASTNode(T &&n, int ln = 0) : node(std::forward<T>(n)), line(ln) {}

    template <typename T>
    T &as() { return std::get<T>(node); }

    template <typename T>
    const T &as() const { return std::get<T>(node); }

    template <typename T>
    bool is() const { return std::holds_alternative<T>(node); }
};