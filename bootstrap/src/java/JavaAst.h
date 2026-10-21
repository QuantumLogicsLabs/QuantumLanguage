#pragma once
// The Java front-end (src/java/): tokens and syntax tree.
//
//   JavaLex.cpp     source text -> tokens (javac-style lexical errors)
//   JavaParse.cpp   tokens -> CompilationUnit (javac-style syntax errors)
//   JavaSema*.cpp   names, types, overloads, generics and the checks javac
//                   makes, then the ordinary Quantum AST (see JavaSema.h)
//   JavaPrelude.cpp the subset of the JDK the runtime provides, written in
//                   Java and compiled together with the program
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace java
{

struct Pos
{
    int line = 0;
    int col = 0;
};

// ─── Tokens ────────────────────────────────────────────────────────────────

enum class Tok
{
    Ident,
    Keyword, // reserved words, plus the literals true / false / null
    IntLit,
    LongLit,
    FloatLit,
    DoubleLit,
    CharLit,
    StringLit, // also text blocks
    Op,        // operators and separators; '>' is always a token of its own
    End,
};

struct Token
{
    Tok kind = Tok::End;
    std::string text;       // spelling as written
    std::string str;        // String literal: decoded, UTF-8
    uint32_t ch = 0;        // char literal: UTF-16 code unit
    uint64_t ival = 0;      // integer literal magnitude
    double dval = 0;        // floating literal value
    bool tooLarge = false;  // integer literal beyond int/long (2147483648 is fine after '-')
    Pos pos;                // first character
    Pos end;                // just past the last character
    bool adjNext = false;   // no space before the next token (`> >` vs `>>`)
};

// A syntax or lexical error, reported javac-style at `pos`.
struct SyntaxError
{
    std::string msg;
    Pos pos;
};

std::vector<Token> lex(const std::string &source); // throws SyntaxError

// ─── Syntax tree ───────────────────────────────────────────────────────────

struct TypeRef;
struct Expr;
struct Stmt;
struct ClassDecl;
struct MethodDecl;
struct SwitchBody;
using TypeRefP = std::shared_ptr<TypeRef>;
using ExprP = std::shared_ptr<Expr>;
using StmtP = std::shared_ptr<Stmt>;
using ClassDeclP = std::shared_ptr<ClassDecl>;
using MethodDeclP = std::shared_ptr<MethodDecl>;

// A type as written: `int`, `String[]`, `Map.Entry<K, List<V>>`, `? super T`.
struct TypeRef
{
    enum Kind
    {
        Prim,     // boolean byte short char int long float double void
        Named,    // a (possibly qualified, possibly parameterized) class type
        Array,    // elem[]
        Wildcard, // ? / ? extends B / ? super B
        Var,      // `var` (local variable type inference)
    } kind = Named;
    std::string prim;
    struct Part
    {
        std::string name;
        std::vector<TypeRefP> args;
        bool hasArgs = false; // <...> written (empty = diamond <>)
        Pos pos;
    };
    std::vector<Part> parts;
    TypeRefP elem;
    int wildKind = 0; // Wildcard: 0 unbounded, 1 extends, 2 super
    TypeRefP bound;
    std::vector<TypeRefP> also; // intersection casts / bounds: A & B & C
    Pos pos;
    std::string str() const; // as written (for messages)
};

struct TypeParam
{
    std::string name;
    std::vector<TypeRefP> bounds;
    Pos pos;
};

enum Mod : unsigned
{
    M_PUBLIC = 1u << 0,
    M_PRIVATE = 1u << 1,
    M_PROTECTED = 1u << 2,
    M_STATIC = 1u << 3,
    M_FINAL = 1u << 4,
    M_ABSTRACT = 1u << 5,
    M_NATIVE = 1u << 6,
    M_SYNCHRONIZED = 1u << 7,
    M_TRANSIENT = 1u << 8,
    M_VOLATILE = 1u << 9,
    M_STRICTFP = 1u << 10,
    M_DEFAULT = 1u << 11,
    M_SEALED = 1u << 12,
    M_NON_SEALED = 1u << 13,
};

struct Modifiers
{
    unsigned flags = 0;
    std::vector<std::string> annotations; // simple names: Override, FunctionalInterface, ...
    Pos pos;
    bool has(unsigned m) const { return (flags & m) != 0; }
};

enum class EK
{
    Literal,
    Name,       // identifier
    Field,      // a.name (also package / type qualification, resolved later)
    Call,       // [a.]name(args), super.name(args) (superCall)
    New,        // new T(args) [body], a.new T(args) (a = outer instance)
    NewArray,   // new T[d1][d2][] / new T[]{...} (init in a)
    ArrayLit,   // { e1, e2 } in an initializer
    Index,      // a[b]
    Unary,      // name: + - ! ~
    PreIncDec,  // name: ++ --
    PostIncDec, // name: ++ --
    Binary,     // name: operator
    Assign,     // name: = += -= ... >>>=
    Cond,       // a ? b : c
    Cast,       // (type) a
    InstanceOf, // a instanceof type [bindName]
    Lambda,
    MethodRef,  // a::name, Type::name, Type::new, super::name
    This,       // this / Outer.this (type)
    ClassLit,   // Type.class
    Switch,     // switch expression
};

struct LambdaParam
{
    TypeRefP type; // null when implicitly typed
    std::string name;
    Pos pos;
};

struct Expr
{
    EK k = EK::Literal;
    Pos pos;
    enum Lit
    {
        LInt,
        LLong,
        LFloat,
        LDouble,
        LChar,
        LString,
        LBool,
        LNull,
    } lit = LInt;
    int64_t ival = 0;
    double dval = 0;
    std::string sval;   // String literal (UTF-8)
    bool bval = false;
    std::string name;   // identifier, member / method name, or operator
    ExprP a, b, c;
    std::vector<ExprP> args;      // call / new arguments, array elements, array dimensions
    std::vector<TypeRefP> targs;  // explicit type arguments: this.<T>m()
    TypeRefP type;
    int extraDims = 0;            // new int[3][][] -> dims {3}, extraDims 2
    ClassDeclP body;              // anonymous class body
    bool superCall = false;       // super.m(...) / super::m / Outer.super.m(...)
    bool parenthesized = false;
    // Lambda
    std::vector<LambdaParam> params;
    StmtP block;                  // block body (else `a` is the expression body)
    // InstanceOf pattern variable
    std::string bindName;
    // Switch expression
    std::shared_ptr<SwitchBody> sw;
    Pos opPos;                    // the operator token (javac points binary errors there)
};

struct VarDeclarator
{
    std::string name;
    int dims = 0; // int a[] = ...
    ExprP init;
    Pos pos;
};

struct CatchClause
{
    std::vector<TypeRefP> types; // multi-catch: A | B
    std::string name;
    StmtP body;
    Pos pos;
};

struct Resource
{
    TypeRefP type; // null: an existing variable (`try (r) {...}`)
    std::string name;
    ExprP init;
    Pos pos;
};

struct SwitchCase
{
    std::vector<ExprP> labels; // case A, B ->   (empty + isDefault = default)
    bool isDefault = false;
    bool hasNullLabel = false;
    TypeRefP patternType;      // case String s ->
    std::string patternVar;
    ExprP guard;               // when <expr>
    std::vector<StmtP> body;   // statements (`:` group, or the arrow's block / throw / expression statement)
    ExprP arrowExpr;           // arrow form with an expression body
    Pos pos;
};

struct SwitchBody
{
    ExprP selector;
    std::vector<SwitchCase> cases;
    bool arrow = false;
    Pos pos;
};

enum class SK
{
    Block,
    LocalVar,
    LocalClass,
    Expr,
    If,
    While,
    Do,
    For,
    ForEach,
    Return,
    Break,
    Continue,
    Throw,
    Try,
    Switch,
    Labeled,
    Empty,
    Sync,
    Assert,
    Yield,
    CtorCall, // this(...) / super(...) as the first statement of a constructor
};

struct Stmt
{
    SK k = SK::Empty;
    Pos pos;
    Pos endPos;                    // Block: the closing brace
    std::vector<StmtP> stmts;      // Block
    Modifiers mods;                // LocalVar / ForEach variable
    TypeRefP type;                 // LocalVar / ForEach variable type
    std::vector<VarDeclarator> vars;
    ClassDeclP cls;                // LocalClass
    ExprP e;                       // Expr, conditions, Return / Throw / Yield value, Sync lock, ForEach iterable
    ExprP e2;                      // Assert message
    StmtP s1, s2;                  // If then / else; loop bodies; Labeled statement
    std::vector<StmtP> init;       // For
    std::vector<ExprP> update;     // For
    std::string label;             // Labeled / Break / Continue
    std::vector<Resource> resources;
    StmtP tryBlock;
    std::vector<CatchClause> catches;
    StmtP finallyBlock;
    std::shared_ptr<SwitchBody> sw;
    bool superCall = false;        // CtorCall
    ExprP qualifier;               // CtorCall: outer.super(...)
    std::vector<ExprP> args;
};

struct Param
{
    Modifiers mods;
    TypeRefP type;
    std::string name;
    bool varargs = false;
    Pos pos;
};

struct MethodDecl
{
    Modifiers mods;
    std::vector<TypeParam> tparams;
    TypeRefP ret; // null for a constructor
    std::string name;
    std::vector<Param> params;
    std::vector<TypeRefP> throws;
    StmtP body; // null: abstract / native / interface method
    bool isCtor = false;
    bool compactCtor = false; // record Point { Point { ... } }
    Pos pos;                  // start of the declaration
    Pos namePos;
};

struct FieldDecl
{
    Modifiers mods;
    TypeRefP type;
    std::vector<VarDeclarator> vars;
    Pos pos;
};

struct EnumConst
{
    std::string name;
    std::vector<ExprP> args;
    ClassDeclP body;
    Pos pos;
};

struct Member
{
    enum Kind
    {
        Field,
        Method,
        Class,
        Init, // initializer block (static when isStatic)
    } kind = Field;
    std::shared_ptr<FieldDecl> field;
    MethodDeclP method;
    ClassDeclP cls;
    StmtP init;
    bool isStatic = false;
};

struct ClassDecl
{
    enum Kind
    {
        Class,
        Interface,
        Enum,
        Record,
        Annotation,
    } kind = Class;
    Modifiers mods;
    std::string name; // empty for an anonymous class
    Pos pos;
    Pos namePos;
    Pos endPos;       // closing brace
    std::vector<TypeParam> tparams;
    TypeRefP extends;                 // class: superclass
    std::vector<TypeRefP> implements; // class: interfaces; interface: superinterfaces
    std::vector<TypeRefP> permits;
    std::vector<Param> recordComponents;
    std::vector<EnumConst> enumConsts;
    std::vector<Member> members;
    bool isAnonymous = false;
    bool isLocal = false;
    bool isImplicit = false; // a compact source file's implicit class
    int unit = 0;            // index of its compilation unit
};

struct ImportDecl
{
    std::vector<std::string> name;
    bool isStatic = false;
    bool onDemand = false;
    Pos pos;
};

struct CompilationUnit
{
    std::vector<std::string> package;
    std::vector<ImportDecl> imports;
    std::vector<ClassDeclP> types;
    std::string fileName; // as reported in diagnostics and stack traces
    std::string source;
    bool isPrelude = false;
};

// Parses one source file. Throws SyntaxError.
CompilationUnit parse(const std::string &fileName, const std::string &source, int unitIndex, bool isPrelude);

} // namespace java
