#pragma once
// Internal to the typed C/C++ front-end: static types, scopes and the
// Lowerer shared by CppLower.cpp (language) and CppLib.cpp (library).
#include "Cpp.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace cpp
{

// ─── Types ─────────────────────────────────────────────────────────────────

enum class T
{
    Dyn, // unknown: dynamic (old-pipeline) semantics
    Void,
    Bool,
    Char,
    Int,
    Float,
    Str,     // std::string
    CStr,    // const char* / string literal
    Nullptr,
    Ptr,     // T*  (a[0] = pointee)
    Class,
    Enum,
    Vector,  // vector / deque / list / stack / queue / priority_queue (name says which)
    Array,   // std::array<T,N> / C array (n)
    Map,     // map / unordered_map / multimap (sorted / multi)
    Set,     // set / unordered_set / multiset
    Pair,
    Tuple,
    Optional,
    Variant,
    UPtr,    // unique_ptr / shared_ptr / weak_ptr
    Func,    // std::function / lambda / function pointer: a[0] = return, rest = params
    Stream,
    Iter,    // a[0] = container type
    Monostate,
    Nullopt,
    InitList,
};

struct ClassInfo;
struct EnumInfo;
struct Type;
using TP = std::shared_ptr<Type>;

struct Type
{
    T k = T::Dyn;
    std::string name; // Int: "int"/"long long"/...; Char; Float; Stream kind; container kind
    bool isUnsigned = false;
    bool sorted = true; // Map/Set: std::map vs unordered_map
    bool multi = false;
    std::vector<TP> a;
    long n = -1;            // Array size
    ClassInfo *cls = nullptr;
    EnumInfo *en = nullptr;
    bool lambda = false;    // Func: a lambda closure (return type may be deduced)
    std::string str() const;
};

TP mkT(T k, const std::string &name = "");
TP tInt();
TP tLL();
TP tSize();
TP tDouble();
TP tBool();
TP tChar();
TP tStr();
TP tCStr();
TP tBuf(long n); // char buf[N]: a C char buffer (a Str held in a slot)
TP tVoid();
TP tDyn();

bool isIntegral(const TP &t); // char, ints, bool, enums
bool isArith(const TP &t);
bool isFloat(const TP &t);
bool isStringy(const TP &t); // Str or CStr
bool isCBuf(const TP &t);    // a C char array
bool isI64(const TP &t);     // long long / unsigned long long: exact int64 at run time
bool isObjectRep(const TP &t); // a mutable runtime object (class, containers, streams)
bool needsCopy(const TP &t);
bool sameType(const TP &a, const TP &b);

// ─── Program entities ──────────────────────────────────────────────────────

struct Scope; // namespace / class / block lookup scope

struct FuncInfo
{
    std::string name;     // unqualified
    std::string mangled;  // Quantum global name
    DeclP decl;           // declaration carrying defaults (first seen)
    DeclP def;            // definition (with body)
    std::vector<TP> params;
    std::vector<bool> paramRef;      // declared T& / T&& (not const)
    std::vector<bool> paramConstRef; // const T&
    std::vector<ExprP> defaults;
    TP ret;
    bool retRef = false;   // returns T& (non-const)
    bool variadic = false;
    ClassInfo *cls = nullptr; // member function
    bool isStatic = false, isVirtual = false, isConst = false, isCtor = false, isDtor = false;
    bool isPure = false;
    bool isTemplate = false;
    bool builtinExcCtor = false; // std::runtime_error(msg) style
    Scope *scope = nullptr;      // where the body's names are looked up
    std::string vkey;            // virtual dispatch key
    bool lowered = false;
    int tu = 0;
    std::string access = "public"; // members
};

struct FieldInfo
{
    std::string name;
    TP type;
    int slot = 0;
    VarDeclarator init; // default member initializer
    bool hasInit = false;
    bool isStatic = false;
    std::string global; // static members
    DeclP decl;
    bool isConst = false;
    std::string access = "public";
};

struct ClassInfo
{
    std::string name;  // unqualified
    std::string qname; // qualified, the descriptor's global is qname + "#desc"
    std::vector<ClassInfo *> bases;
    std::vector<FieldInfo> fields; // own fields
    int nslots = 1;                // including slot 0 (descriptor) and base fields
    std::vector<FuncInfo *> methods;
    std::vector<FuncInfo *> ctors;
    std::vector<FuncInfo *> convOps;
    bool polymorphic = false;
    bool hasUserCtor = false;
    bool aggregate = true;
    bool isUnion = false;       // members share one slot
    bool builtin = false;       // std::exception family
    bool resolved = false, resolving = false;
    bool isTemplate = false;
    DeclP decl;
    Scope *scope = nullptr;     // members + nested types
    Scope *outer = nullptr;
    bool lowered = false;
    std::vector<ClassInfo *> inheritCtorsFrom;
    std::set<std::string> friends; // functions and classes declared friend
    FuncInfo *dtor = nullptr;      // user-declared destructor
    std::string dtorFn() const { return qname + "#dtor"; }
    std::string aggFn() const { return qname + "#agg"; }
    std::string defaultCtorFn() const { return qname + "#ctor"; }
    std::string desc() const { return qname + "#desc"; }
};

struct EnumInfo
{
    std::string qname;
    bool scoped = false;
    std::vector<std::pair<std::string, long long>> values;
    TP underlying;
    Scope *scope = nullptr; // its enumerators (scoped enums)
};

struct Symbol
{
    enum Kind
    {
        None,
        Namespace,
        Type,
        Funcs,
        Var,
        Enumerator,
    } kind = None;
    Scope *ns = nullptr;
    TP type; // Type: the type; Var: variable type; Enumerator: enum type
    std::vector<FuncInfo *> funcs;
    std::string global;  // Var: global name
    long long value = 0; // Enumerator
    DeclP aliasDecl;     // unresolved alias
    Scope *aliasScope = nullptr;
    bool isConst = false; // Var: a read-only global
};

struct Scope
{
    std::string qname; // "" for global, "minidb", "minidb::Table", ...
    Scope *parent = nullptr;
    ClassInfo *cls = nullptr; // class scope
    std::map<std::string, Symbol> syms;
    std::vector<Scope *> usings;              // using-directives
    std::map<std::string, Symbol> usingDecls; // using-declarations
    bool isStd = false;
};

// ─── Lowered expressions ───────────────────────────────────────────────────

struct XV
{
    ASTNodePtr v; // the value
    TP t;
    enum LK
    {
        RV,    // not assignable
        Var,   // variable `var`
        Idx,   // obj[key]
        StrCh, // character `key` of the string lvalue `base`
    } lk = RV;
    std::string var;
    ASTNodePtr obj, key;
    std::shared_ptr<XV> base;
    bool temp = false;  // a fresh value (no copy needed when stored)
    bool moved = false; // std::move(x)
    bool isFuncName = false;
    std::vector<FuncInfo *> overloads; // an unresolved function name
    ExprP self;                         // member function name: the object expression
    bool constLv = false;   // a read-only lvalue: assigning it is an error
    std::string constName;  // "variable 'x'" / "member 'C::x'" (for that error)
    // malloc / calloc / realloc: what to allocate is known once the result
    // converts to a pointer type (`int *a = malloc(n * sizeof(int))`)
    struct Alloc
    {
        std::shared_ptr<ASTNodePtr> bytes; // byte count
        std::shared_ptr<ASTNodePtr> old;   // realloc: the block resized
    };
    std::shared_ptr<Alloc> alloc;
};

// ─── Lowerer ───────────────────────────────────────────────────────────────

struct Local
{
    std::string q; // Quantum name
    TP t;
    bool boxed = false;   // stored as [value]
    bool refSlot = false; // holds a Ref [container, key] (non-const T& of a scalar)
    bool alias = false;   // a reference bound to an lvalue
    std::shared_ptr<XV> aliasOf;
    std::string name;     // as declared
    bool isConst = false; // const T x / const T& x: read-only
};

struct LoopCtx
{
    bool isSwitch = false;
    std::string contFlag; // switch inside a loop: `continue` sets this, then breaks
    size_t scopeDepth = 0; // scopes open outside the loop body (break / continue leave the rest)
};

struct FnCtx
{
    TP ret;
    bool retRef = false;      // returns T& of a scalar: a Ref
    bool retIsRef = false;    // declared to return any reference: no copy on return
    ClassInfo *cls = nullptr;
    bool hasThis = false;
    bool isLambda = false;
    TP deducedRet; // lambdas without a declared return type
    std::vector<std::map<std::string, Local>> scopes;
    std::vector<std::vector<Local>> owned; // per scope: objects destroyed when it ends
    std::set<std::string> boxed;   // Quantum local names stored boxed
    std::set<std::string> wantBox; // discovered this pass
    std::map<std::string, int> nameCount;
    std::vector<LoopCtx> loops;
    std::vector<std::string> excVars;
    Scope *scope = nullptr; // namespace/class scope for lookups
    FnCtx *parent = nullptr;
};

class Lowerer
{
public:
    explicit Lowerer(Program &prog);
    ASTNodePtr run();

    // ── shared by CppLib.cpp ──
    [[noreturn]] void unsupported(const std::string &msg, int line = 0);
    TP resolveType(const TypeNameP &tn, Scope *sc);
    XV lowerExpr(const ExprP &e);
    XV convert(XV x, const TP &to, int line, bool forInit = true);
    ASTNodePtr toBool(XV x, int line);
    ASTNodePtr read(XV &x);             // value of x (clones lvalue parts)
    ASTNodePtr store(XV &target, ASTNodePtr value, int line); // assignment expression
    ASTNodePtr copyOf(XV x, int line);  // value with copy semantics applied
    ASTNodePtr defaultValue(const TP &t, int line);
    ASTNodePtr planFor(const TP &t);
    ASTNodePtr simple(ASTNodePtr e, int line); // hoists into a temp when not trivially re-evaluable
    XV lowerCall(const ExprP &e);
    XV libCall(const std::string &name, const ExprP &e, std::vector<ExprP> &args, const std::vector<TArg> &targs,
               bool &handled);
    XV libMethod(XV &obj, const std::string &name, const ExprP &e, std::vector<ExprP> &args, bool &handled);
    XV makeObjectFrom(const TP &t, std::vector<ExprP> &args, bool braced, int line);
    XV listInit(const TP &t, std::vector<ExprP> &kids, int line);
    XV mkxv(ASTNodePtr v, TP t, bool temp = true);
    ASTNodePtr streamKind(const TP &t);
    ASTNodePtr callFn(FuncInfo *f, std::vector<ExprP> &args, ASTNodePtr self, int line, bool virt);
    FuncInfo *pickOverload(const std::vector<FuncInfo *> &cands, std::vector<XV> &args, int line, bool hasSelf);
    ASTNodePtr userStreamOut(XV &stream, XV &val, int line, bool &handled);
    std::string tempName();
    TP elemType(const TP &t);
    TP deref(const TP &t);
    // sizeof / alignof as gcc on x86-64 Windows lays types out (LLP64)
    long sizeOfType(const TP &t);
    long alignOfType(const TP &t);
    XV materializeAlloc(XV &x, const TP &elem, int line);
    int line_ = 0;
    FnCtx *fn_ = nullptr;
    std::vector<ASTNodePtr> *pre_ = nullptr; // hoisted temps of the current statement
    bool canHoist_ = false;
    Scope *global_ = nullptr, *std_ = nullptr;

private:
    Program &prog_;
    std::vector<std::unique_ptr<Scope>> scopes_;
    std::vector<std::unique_ptr<ClassInfo>> classes_;
    std::vector<std::unique_ptr<FuncInfo>> funcs_;
    std::vector<std::unique_ptr<EnumInfo>> enums_;
    std::vector<ClassInfo *> classOrder_;
    std::vector<std::pair<DeclP, Scope *>> globalsInOrder_;
    std::vector<FuncInfo *> allFuncs_;
    std::vector<ASTNodePtr> topFns_;   // lowered functions
    std::vector<ASTNodePtr> topInit_;  // descriptors, plans, globals
    std::map<std::string, std::string> plans_; // type key → plan global
    std::vector<ASTNodePtr> planInits_;
    std::map<std::string, ClassInfo *> builtinExc_;
    int tempCounter_ = 0;
    int staticCounter_ = 0;
    int localClassCounter_ = 0;
    std::vector<std::pair<const Decl *, Scope *>> funcDecls_;

    Scope *newScope(const std::string &qname, Scope *parent);
    void declareBuiltins();
    void collect(const std::vector<DeclP> &decls, Scope *sc);
    void collectClass(const DeclP &d, Scope *sc, const std::string &forcedName = "");
    void collectEnum(const DeclP &d, Scope *sc);
    void collectFunc(const DeclP &d, Scope *sc);
    void resolveClass(ClassInfo *c);
    void resolveFunc(FuncInfo *f);
    long long evalConst(const ExprP &e, Scope *sc);
    Symbol *lookup(const std::vector<std::string> &name, Scope *sc, bool typesOnly = false);
    Symbol *lookupIn(Scope *s, const std::string &n, bool typesOnly);
    Symbol *lookupUnqualified(const std::string &n, Scope *sc, bool typesOnly);
    TP builtinType(const TypeName &tn, Scope *sc, bool &ok);
    std::string mangle(Scope *sc, const std::string &name);

    void lowerFunction(FuncInfo *f);
    void lowerClass(ClassInfo *c);
    ASTNodePtr lowerFunctionBody(FuncInfo *f, std::vector<std::string> &params);
    void emitGlobals();
    void emitDescriptors();

    ASTNodePtr lowerStmt(const StmtP &s);
    ASTNodePtr lowerBlock(const std::vector<StmtP> &body, bool newScope = true);
    void lowerDecl(const Stmt &s, std::vector<ASTNodePtr> &out);
    void lowerVar(const VarDeclarator &v, bool isStatic, std::vector<ASTNodePtr> &out);
    ASTNodePtr lowerIf(const Stmt &s);
    ASTNodePtr lowerFor(const Stmt &s);
    ASTNodePtr lowerRangeFor(const Stmt &s);
    ASTNodePtr lowerSwitch(const Stmt &s);
    ASTNodePtr lowerTry(const Stmt &s);
    ASTNodePtr withPre(ASTNodePtr stmt, std::vector<ASTNodePtr> &pre);
    void localClass(const DeclP &d);

    XV lowerName(const ExprP &e);
    XV lowerUnary(const ExprP &e);
    XV lowerBinary(const ExprP &e);
    XV lowerAssign(const ExprP &e);
    XV lowerMember(const ExprP &e);
    XV lowerIndex(const ExprP &e);
    XV lowerCast(const ExprP &e);
    XV lowerLambda(const ExprP &e);
    XV lowerConstruct(const ExprP &e);
    XV lowerNew(const ExprP &e);
    XV arith(const std::string &op, XV a, XV b, int line);
    XV compareOp(const std::string &op, XV a, XV b, int line);
    XV userOperator(const std::string &op, std::vector<ExprP> &operands, std::vector<XV> &vals, int line,
                    bool &handled);
    XV incdec(XV x, bool inc, bool postfix, int line);
    XV addressOf(XV x, int line);
    ASTNodePtr charPtr(XV &x, int line); // a char buffer / char* argument as a char*
    long bufInitSize(const std::vector<ExprP> &args, bool braced);
    long arrayInitSize(const TP &t, const std::vector<ExprP> &args, bool braced);
    long initSlots(const TP &t); // the scalars a member takes under brace elision
    // strictness: what g++ / gcc reject
    bool stdVisible(const std::string &name, Scope *sc); // `using namespace std` / `using std::name`
    void checkAccess(ClassInfo *owner, const std::string &access, const std::string &what, int line);
    std::string diagType(const TP &t);
    bool explicitConv_ = false; // converting for an explicit cast
    FuncInfo *curFn_ = nullptr; // the function being lowered
    // gcc evaluates a call's arguments right to left: with more than one
    // argument having side effects, those are lowered first, last to first
    void preEvalArgs(const std::vector<ExprP> &args, std::vector<XV> &out, size_t from,
                     const std::function<TP(size_t)> &target);
    XV derefXV(XV x, int line);
    XV fieldOf(XV obj, ClassInfo *c, const std::string &name, int line, bool &found);
    XV memberFuncRef(XV obj, ClassInfo *c, const std::string &name, int line, bool &found);
    ASTNodePtr ctorCall(ClassInfo *c, std::vector<ExprP> &args, bool braced, int line, ASTNodePtr into = nullptr);
    XV constructBuiltinType(const TP &t, std::vector<ExprP> &args, bool braced, int line);

    Local *findLocal(const std::string &n);
    Local &declareLocal(const std::string &n, const TP &t);
    void pushScope();
    void popScope();
    ClassInfo *builtinExceptionClass(const std::string &n);
    TP lambdaReturn(const LambdaP &l);
    std::map<std::string, TP> localTypes_; // classes / aliases declared inside function bodies

public:
    std::vector<TP> lambdaHints_; // parameter types for the next generic lambda (auto params)
    std::set<FuncInfo *> calledUndefined_; // called, but no definition anywhere (a link error)

public:
    XV lowerExprTo(const ExprP &e, const TP &target);
    XV libName(const std::vector<std::string> &name, int line, bool &found);
    ASTNodePtr peekRead(const XV &x);
    XV localXV(Local *l);
    XV localXVRaw(Local *l);
    XV lowerLambdaBody(const ExprP &e);
    // function templates: an instance per template-argument binding
    void deduce(const TypeNameP &p, const TP &a, const std::vector<std::string> &tps, std::map<std::string, TP> &bind);
    FuncInfo *instantiate(FuncInfo *f, const std::vector<XV> &vals, const std::vector<TArg> &targs, int line);
    std::map<std::string, FuncInfo *> instances_;
    // object lifetimes: destructors at scope exit / delete, smart pointers
    bool needsDtor(ClassInfo *c);
    bool ownsResource(const TP &t);
    ASTNodePtr destroyValue(const TP &t, ASTNodePtr v, int line);
    void emitDestroy(size_t fromDepth, std::vector<ASTNodePtr> &out, const std::string &except = "");
    bool anyOwned(size_t fromDepth);
    LoopCtx loopCtx();
    FuncInfo *userCopyCtor(ClassInfo *c);
    std::map<ClassInfo *, int> dtorMemo_;
    // class templates: an instance per argument list (phase_: how far run() is)
    TP instantiateClass(ClassInfo *tc, const std::vector<TArg> &targs, Scope *sc);
    void copyTemplateDefs(ClassInfo *c);
    void mangleMethods(ClassInfo *c);
    std::map<std::string, ClassInfo *> classInstances_;
    std::map<ClassInfo *, ClassInfo *> instanceOf_;
    int phase_ = 0;
    // temporaries of classes with destructors: destroyed when their full
    // expression ends (-1: not inside one that destroys them)
    int tempsTracked_ = -1;
    std::string tempsMark_; // the statement's mark (returns end their temporaries before returning)
    ASTNodePtr lowerStmtInner(const StmtP &s);
    ASTNodePtr trackTemp(ASTNodePtr v, const TP &t, int line);
    void untrackTemp(XV &x);
    ASTNodePtr endTemps(ASTNodePtr stmt, const std::string &mark, int line);
    XV callArgs(FuncInfo *f, std::vector<ExprP> &args, ASTNodePtr self, int line, bool virt,
                std::vector<XV> *lowered = nullptr);
    TP probeType(const ExprP &e);
    int convCost(const TP &from, const TP &to, const XV *x = nullptr);
    TP commonType(const TP &a, const TP &b);
    TP arithResult(const TP &a, const TP &b);
    ClassInfo *currentClass() { return fn_ ? fn_->cls : nullptr; }
    Scope *currentScope() { return fn_ ? fn_->scope : global_; }
    FuncInfo *findMethod(ClassInfo *c, const std::string &name, std::vector<FuncInfo *> &out);
    void lookupFuncs(const std::vector<std::string> &name, std::vector<FuncInfo *> &out);
    XV callValue(XV f, std::vector<ExprP> &args, int line);
    XV dynCall(ASTNodePtr callee, std::vector<ExprP> &args, int line);
    ASTNodePtr chr(ASTNodePtr code, int line);
    Symbol *lookupPublic(const std::vector<std::string> &name, Scope *sc) { return lookup(name, sc); }
    ASTNodePtr defaultCtor(ClassInfo *c, ASTNodePtr into, int line);
};

// Quantum AST builders.
namespace q
{
ASTNodePtr num(double v, int ln);
ASTNodePtr str(const std::string &s, int ln);
ASTNodePtr boolean(bool b, int ln);
ASTNodePtr nil(int ln);
ASTNodePtr id(const std::string &n, int ln);
ASTNodePtr call(ASTNodePtr callee, std::vector<ASTNodePtr> args, int ln);
ASTNodePtr call(const std::string &fn, std::vector<ASTNodePtr> args, int ln);
ASTNodePtr index(ASTNodePtr obj, ASTNodePtr key, int ln);
ASTNodePtr assign(ASTNodePtr target, ASTNodePtr value, int ln);
ASTNodePtr bin(const std::string &op, ASTNodePtr l, ASTNodePtr r, int ln);
ASTNodePtr un(const std::string &op, ASTNodePtr x, int ln);
ASTNodePtr ternary(ASTNodePtr c, ASTNodePtr a, ASTNodePtr b, int ln);
ASTNodePtr arr(std::vector<ASTNodePtr> elems, int ln);
ASTNodePtr block(std::vector<ASTNodePtr> stmts, int ln);
ASTNodePtr exprStmt(ASTNodePtr e, int ln);
ASTNodePtr var(const std::string &n, ASTNodePtr init, int ln);
ASTNodePtr ret(ASTNodePtr e, int ln);
ASTNodePtr ifs(ASTNodePtr c, ASTNodePtr t, ASTNodePtr e, int ln);
ASTNodePtr whiles(ASTNodePtr c, ASTNodePtr body, ASTNodePtr post, int ln);
ASTNodePtr brk(int ln);
ASTNodePtr cont(int ln);
ASTNodePtr func(const std::string &name, std::vector<std::string> params, ASTNodePtr body, int ln);
ASTNodePtr lambda(std::vector<std::string> params, ASTNodePtr body, int ln);
ASTNodePtr clone(const ASTNodePtr &n);
bool isSimple(const ASTNodePtr &n);
} // namespace q

} // namespace cpp
