#pragma once
// Java semantics: symbols, types, and the Sema that attributes the syntax
// tree (names, types, overloads, generics, javac's compile-time checks) while
// lowering it to the ordinary Quantum AST.
//
//   JavaTypes.cpp   types: subtyping, substitution, conversions, resolution
//   JavaEnter.cpp   classes and members: entering, signatures, dispatch keys
//   JavaExpr.cpp    expressions
//   JavaCall.cpp    method / constructor calls, inference, lambdas, method refs
//   JavaStmt.cpp    statements, control flow, definite assignment
//   JavaLib.cpp     library calls lowered specially (String, Math, printing)
//   JavaEmit.cpp    the program: classes, statics, entry point
//
// Runtime representation: include/JavaOps.h. Naming in the lowered program:
//   class value       global <binary name>            (Main$Inner)
//   method body       global <binary name>.<name>[#n] (Main.fib, Main.<init>#2)
//   static field      global <binary name>::<field>
//   instance field    instance field <name> (<name>$<Owner> when hidden)
//   dispatch key      <name>(<erased params>)          (compareTo(Object))
#include "JavaAst.h"
#include "AST.h"
#include "JavaOps.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace java
{

struct ClassSym;
struct MethodSym;
struct FieldSym;
struct TypeVar;

// ─── Types ─────────────────────────────────────────────────────────────────

enum class TK : uint8_t
{
    Void,
    Boolean,
    Byte,
    Short,
    Char,
    Int,
    Long,
    Float,
    Double,
    Null,
    Class,
    Array,
    TypeVar,
    Wildcard,
    Error, // after a reported error: compatible with everything
};

struct Type;
using TypeP = std::shared_ptr<const Type>;

struct Type
{
    TK k = TK::Error;
    ClassSym *cls = nullptr;   // Class
    std::vector<TypeP> args;   // Class: type arguments (empty: raw or not generic)
    TypeP elem;                // Array: element; Wildcard: bound
    TypeVar *tv = nullptr;     // TypeVar
    int wild = 0;              // Wildcard: 0 ?, 1 ? extends, 2 ? super
};

struct TypeVar
{
    std::string name;
    std::vector<TypeP> bounds; // empty = Object
    std::vector<TypeRefP> boundRefs;
    bool ofMethod = false;
};

using Subst = std::map<const TypeVar *, TypeP>;

// A compile-time constant (JLS §15.29).
struct Const
{
    enum K
    {
        None,
        Int,  // int / short / byte
        Long,
        Float,
        Double,
        Char,
        Bool,
        Str,
    } k = None;
    int64_t i = 0;
    double d = 0;
    bool b = false;
    std::string s;
    bool ok() const { return k != None; }
};

// ─── Symbols ───────────────────────────────────────────────────────────────

struct FieldSym
{
    std::string name;
    ClassSym *owner = nullptr;
    TypeP type;
    unsigned mods = 0;
    FieldDecl *decl = nullptr;
    const VarDeclarator *var = nullptr;
    std::string key; // instance: field key; static: global name
    bool isStatic = false;
    bool synthetic = false;
    Const cval;      // a constant variable's value
    int constState = 0; // 0 unknown, 1 evaluating, 2 done
    Pos pos;
};

enum class Synth
{
    None,
    DefaultCtor,
    RecordCtor,
    RecordAccessor,
    RecordEquals,
    RecordHashCode,
    RecordToString,
    EnumValues,
    EnumValueOf,
};

struct MethodSym
{
    std::string name;
    ClassSym *owner = nullptr;
    unsigned mods = 0;
    std::vector<TypeVar *> tparams;
    std::vector<TypeP> params;
    std::vector<std::string> paramNames;
    bool varargs = false;
    TypeP ret;
    std::vector<TypeP> throws;
    MethodDecl *decl = nullptr;
    bool isCtor = false;
    Synth synth = Synth::None;
    FieldSym *field = nullptr; // record accessor
    std::string vkey;          // dispatch key
    std::string impl;          // global name of the lowered body
    std::string native;        // `native` prelude method: the VM native implementing it
    std::vector<std::string> bridgeKeys; // other keys it implements (generic bridges)
    bool lowered = false;
    bool queued = false;
    Pos pos;
    bool isStatic() const { return (mods & M_STATIC) != 0; }
    bool isAbstract() const { return (mods & M_ABSTRACT) != 0; }
    bool isPrivate() const { return (mods & M_PRIVATE) != 0; }
    bool isDefault() const { return (mods & M_DEFAULT) != 0; }
};

struct Capture
{
    std::string name; // the captured local
    TypeP type;
    std::string field; // val$<name>
};

struct ClassSym
{
    std::string name;       // simple name ("" for anonymous)
    std::string binaryName; // Main$Inner, java.util.ArrayList
    std::string pkg;
    ClassDecl *decl = nullptr;
    ClassDeclP declHolder;  // keeps synthesized declarations alive
    int unit = 0;
    ClassDecl::Kind kind = ClassDecl::Class;
    unsigned mods = 0;
    ClassSym *outer = nullptr;
    bool hasOuterInstance = false; // an inner class: this$0
    bool isLocal = false, isAnon = false;
    std::vector<TypeVar *> tparams;
    TypeP super;
    std::vector<TypeP> ifaces;
    std::vector<FieldSym *> fields;
    std::vector<MethodSym *> methods;
    std::vector<MethodSym *> ctors;
    std::map<std::string, ClassSym *> memberTypes;
    std::vector<Capture> captures; // local / anonymous classes
    struct FnCtx *enclosingFn = nullptr;
    int state = 0; // 1 resolving header, 2 header done, 3 members entered, 4 keys assigned
    bool reachable = false;
    bool lowered = false;
    bool needsClinit = false;
    bool hasInstanceInit = false;
    int anonCount = 0, lambdaCount = 0, localCount = 0;
    bool isInterface() const { return kind == ClassDecl::Interface || kind == ClassDecl::Annotation; }
    bool isEnum() const { return kind == ClassDecl::Enum; }
    bool isRecord() const { return kind == ClassDecl::Record; }
    std::string clinitFn() const { return binaryName + ".<clinit>"; }
    std::string initedFlag() const { return binaryName + "::$inited"; }
    std::string valuesGlobal() const { return binaryName + "::$VALUES"; } // an enum's constants
    std::string fieldInitFn() const { return binaryName + ".$finit"; }
};

// ─── Lowering contexts ─────────────────────────────────────────────────────

struct LocalVar
{
    std::string name;
    std::string q; // its name in the lowered code
    TypeP type;
    bool isFinal = false;
    bool hasInit = false;       // declared with an initializer (or a parameter)
    bool assignedLater = false; // assigned after its declaration
    int assignCount = 0;        // assignments after the declaration (effectively final: JLS §4.12.4)
    Const cval;                 // a constant variable
    struct FnCtx *fn = nullptr; // the function that declared it
    Pos pos;
    int index = 0;              // declaration order within the function (definite assignment)
    // where a lambda / inner class reads it: an error if it is not effectively final
    std::vector<std::pair<Pos, bool>> captureSites; // (position, from a lambda)
    int captureUnit = 0;
};

struct BlockScope
{
    std::deque<LocalVar> vars; // deque: LocalVar pointers stay valid as more are declared
    std::map<std::string, ClassSym *> classes;
};

// Definite assignment (JLS §16) and reachability (§14.22) at a program point.
struct Flow
{
    bool alive = true;          // the point can be reached
    std::vector<char> assigned; // by LocalVar::index of the function's locals
};

// A break / continue target: a loop, a switch, or a labeled statement.
struct JumpTarget
{
    std::string label;
    bool isLoop = false;      // `continue` may target it
    bool isSwitch = false;
    int quantumDepth = 0;     // its own Quantum loop: FnCtx::quantumLoops inside it
    int finallyDepth = 0;     // FnCtx::finallies.size() where it starts
    std::string breakFlag;    // set by a break that crosses inner Quantum loops
    std::string continueFlag; // set by a continue that crosses inner Quantum loops
    bool breakHit = false;    // some break reaches it (it can complete normally)
    bool continueHit = false;
    bool breakFlowSet = false;
    Flow breakFlow;           // definite assignment where its breaks leave
};

// A try statement with catch clauses, around the code being lowered: the
// checked exceptions it catches (JLS §11.2).
struct ExcScope
{
    std::vector<std::vector<TypeP>> catches; // per catch clause: its types
    std::vector<bool> thrown;                // per catch clause: something it catches may be thrown
    std::vector<std::vector<TypeP>> caught;  // per catch clause: the checked types it catches (precise rethrow)
};

struct FnCtx
{
    ClassSym *cls = nullptr;
    MethodSym *method = nullptr; // null in an initializer / lambda
    bool isStatic = false;
    bool isLambda = false;
    bool isCtor = false;
    bool transparent = false;    // a closure of the lowering (switch expression), not a Java lambda
    std::vector<std::string> captured; // a lambda: the enclosing locals it reads (copied when it is created)
    FnCtx *outer = nullptr;      // lexically enclosing function (lambdas, local classes)
    TypeP ret;                   // declared return type (lambdas: from the target, may be null)
    std::vector<TypeP> *lambdaReturns = nullptr; // inference: types of returned values
    bool lambdaReturnsValue = false;
    std::deque<BlockScope> scopes;
    std::vector<std::string> temps;
    std::vector<JumpTarget> targets;
    std::vector<StmtP> finallies;  // finally blocks enclosing the current statement
    std::vector<std::string> catchVars; // innermost first: rethrow targets
    int quantumLoops = 0;           // Quantum while-loops currently open
    int localCount = 0;
    std::string name;               // for lambda naming: the enclosing method
    Flow flow;                      // definite assignment / reachability here
    std::vector<ExcScope> excScopes;
    std::vector<TypeP> throwsAllowed; // the method's (or lambda's interface method's) throws clause
    bool checkExceptions = true;
    // catch parameters: their try's checked exceptions (precise rethrow)
    std::map<std::string, std::vector<TypeP>> rethrowTypes;
    struct PendingFlag
    {
        size_t target;
        bool isContinue;
    };
    std::vector<PendingFlag> pendingFlags; // flagged jumps not yet propagated outward
};

// A lowered expression.
struct Val
{
    ASTNodePtr node;
    TypeP type;
    Const cv;
    // A generic method call or a diamond whose type was inferred (a poly
    // expression): typed again when it turns out to be an argument whose
    // target is known.
    bool polyCall = false;
    // An ambiguous name (JLS §6.5.2) that resolved to a type or a package.
    ClassSym *typeSym = nullptr;
    std::string package;
    bool isType() const { return typeSym != nullptr; }
    bool isPackage() const { return !package.empty(); }
};

// ─── Diagnostics ───────────────────────────────────────────────────────────

struct Diag
{
    int unit = 0;
    Pos pos;
    std::string msg;
    std::vector<std::string> notes;
};

// ─── Sema ──────────────────────────────────────────────────────────────────

class Sema
{
public:
    explicit Sema(std::vector<CompilationUnit> &units);
    // The lowered program, or (with diagnostics()) nothing on errors.
    ASTNodePtr run();
    const std::vector<Diag> &diagnostics() const { return diags_; }
    const std::vector<CompilationUnit> &units() const { return units_; }

    // ── types (JavaTypes.cpp) ──
    TypeP prim(TK k) const;
    TypeP tObject();
    TypeP tString();
    TypeP tError() const;
    TypeP tNull() const;
    TypeP classType(ClassSym *c, std::vector<TypeP> args = {});
    TypeP arrayOf(const TypeP &elem);
    TypeP typeVar(TypeVar *tv);
    TypeP selfType(ClassSym *c); // C<T1..Tn>: the class parameterized by its own type variables
    ClassSym *cls(const std::string &qualName); // a prelude class by qualified name
    static bool isPrim(const TypeP &t);
    static bool isNumeric(const TypeP &t);
    static bool isIntegral(const TypeP &t);
    static bool isRef(const TypeP &t);
    static bool isError(const TypeP &t) { return !t || t->k == TK::Error; }
    bool isBooleanish(const TypeP &t); // boolean or Boolean
    bool isStringType(const TypeP &t);
    TypeP boxOf(const TypeP &prim);
    TypeP unboxed(const TypeP &t); // the primitive behind a box type, else null
    TypeP numericUnboxed(const TypeP &t); // t itself if primitive numeric, its primitive if a numeric box
    std::string typeStr(const TypeP &t) const;
    std::string sigStr(const MethodSym *m) const; // f(int,String)
    TypeP subst(const TypeP &t, const Subst &s);
    TypeP erasure(const TypeP &t);
    std::string erasedName(const TypeP &t); // for dispatch keys
    Subst substOf(const TypeP &classType);    // class type params -> its arguments
    // The function type of a functional interface type (JLS §9.9): ? extends X
    // and ? super X both give X.
    Subst fiSubst(const TypeP &classType);
    TypeP asSuper(const TypeP &t, ClassSym *target);
    bool isSubClass(ClassSym *a, ClassSym *b);
    bool isSubtype(const TypeP &s, const TypeP &t);
    bool sameType(const TypeP &a, const TypeP &b);
    // Assignment conversion (JLS §5.2); `cv` permits constant narrowing.
    bool assignable(const TypeP &from, const TypeP &to, const Const *cv = nullptr);
    bool castable(const TypeP &from, const TypeP &to);
    TypeP binaryPromote(const TypeP &a, const TypeP &b);
    TypeP unaryPromote(const TypeP &t);
    TypeP upperBound(const TypeP &t); // wildcard / type variable -> a class type
    TypeP lub(const TypeP &a, const TypeP &b);
    TypeP resolveType(const TypeRefP &r, bool allowVar = false);
    ClassSym *resolveTypeName(const std::vector<std::string> &parts, Pos pos, bool report = true);
    ClassSym *lookupClassSimple(const std::string &name);
    TypeVar *lookupTypeVar(const std::string &name);
    MethodSym *functionalMethod(ClassSym *iface);
    std::string descriptor(const TypeP &t); // for instanceof / checkcast

    // ── entering (JavaEnter.cpp) ──
    void enterUnit(int u);
    ClassSym *enterClass(ClassDecl *d, ClassSym *outer, int unit, const std::string &pkg);
    void resolveHeader(ClassSym *c);
    void enterMembers(ClassSym *c);
    void assignKeys(ClassSym *c);
    void checkClass(ClassSym *c);
    FieldSym *findField(ClassSym *c, const std::string &name);
    void collectMethods(ClassSym *c, const std::string &name, std::vector<MethodSym *> &out);
    std::vector<MethodSym *> allAbstract(ClassSym *c);
    bool overrides(MethodSym *m, MethodSym *base, ClassSym *in);
    ClassSym *memberType(ClassSym *c, const std::string &name);

    // ── lowering ──
    void lowerClass(ClassSym *c);
    void lowerMethod(MethodSym *m);
    // Reachability (rapid type analysis): user code is lowered whole; of the
    // prelude only the classes it uses and the methods those can call.
    void need(ClassSym *c);   // reachable: lower it
    void needMethod(MethodSym *m);
    void queueMethod(MethodSym *m);
    void useKey(const std::string &key); // a virtual call through this dispatch key exists
    bool implementsUsedKey(const MethodSym *m) const;
    MethodSym *needNamed(const std::string &cls, const std::string &method); // a prelude method called by name
    std::set<std::string> calledKeys_;
    std::vector<ClassSym *> reachable_;

    // expressions (JavaExpr.cpp)
    Val expr(const ExprP &e, const TypeP &target = nullptr);
    ASTNodePtr exprTo(const ExprP &e, const TypeP &to, Pos pos, const char *what = nullptr);
    ASTNodePtr cond(const ExprP &e);
    ASTNodePtr coerce(Val v, const TypeP &to, Pos pos, bool isCast = false, bool report = true);
    ASTNodePtr convertPrim(ASTNodePtr n, const TypeP &from, const TypeP &to, int ln);
    ASTNodePtr stringOf(Val v, int ln);
    Val name(const ExprP &e, bool wantValue);
    Val fieldAccess(const ExprP &e, bool wantValue);
    Val literal(const ExprP &e);
    Val unary(const ExprP &e);
    Val binary(const ExprP &e);
    Val assign(const ExprP &e);
    Val incdec(const ExprP &e);
    Val conditional(const ExprP &e, const TypeP &target);
    Val castExpr(const ExprP &e);
    Val instanceOf(const ExprP &e);
    Val newArray(const ExprP &e, const TypeP &target);
    Val arrayLit(const ExprP &e, const TypeP &arrType);
    Val index(const ExprP &e);
    Val thisExpr(const ExprP &e);
    Val classLit(const ExprP &e);
    Val switchExpr(const ExprP &e, const TypeP &target);
    ASTNodePtr defaultValue(const TypeP &t, int ln);
    Val loadField(FieldSym *f, ASTNodePtr obj, const TypeP &objType, Pos pos);
    ASTNodePtr outerInstance(ClassSym *target, Pos pos); // `Target.this` from the current context
    ASTNodePtr thisNode(int ln);
    bool foldConst(const std::string &op, const Const &a, const Const &b, const TypeP &rt, Const &out);
    Const fieldConst(FieldSym *f);

    // calls (JavaCall.cpp)
    Val call(const ExprP &e, const TypeP &target);
    Val newObject(const ExprP &e, const TypeP &target);
    Val lambda(const ExprP &e, const TypeP &target);
    Val methodRef(const ExprP &e, const TypeP &target);
    struct Resolved
    {
        MethodSym *m = nullptr;
        Subst subst;          // class + method type arguments
        bool varargsCall = false;
        std::vector<TypeP> paramTypes; // instantiated
        TypeP ret;
        bool ambiguous = false;
    };
    Resolved resolveCall(std::vector<MethodSym *> &cands, const std::vector<ExprP> &args,
                         const TypeP &recvType, const TypeP &target, Pos pos, const std::string &name,
                         const std::vector<TypeRefP> &explicitTargs, std::vector<Val> &lowered,
                         std::string &failNotes);
    std::vector<ASTNodePtr> lowerArgs(Resolved &r, const std::vector<ExprP> &args, std::vector<Val> &pre, Pos pos);
    ASTNodePtr invoke(MethodSym *m, ASTNodePtr recv, std::vector<ASTNodePtr> args, bool virt, int ln,
                      const TypeP &recvType = nullptr);
    ASTNodePtr construct(ClassSym *c, MethodSym *ctor, std::vector<ASTNodePtr> args, ASTNodePtr outer, int ln);
    bool lambdaCompatible(const ExprP &arg, const TypeP &param);
    TypeP inferFromLambda(const ExprP &lam, const TypeP &fiType, Subst &s, const std::vector<TypeVar *> &vars);

    // statements (JavaStmt.cpp)
    ASTNodePtr stmt(const StmtP &s);
    ASTNodePtr block(const std::vector<StmtP> &stmts, int ln, bool newScope = true);
    void localVar(const Stmt &s, std::vector<ASTNodePtr> &out);
    ASTNodePtr ifStmt(const Stmt &s);
    ASTNodePtr whileStmt(const Stmt &s, const std::string &label);
    ASTNodePtr doStmt(const Stmt &s, const std::string &label);
    ASTNodePtr forStmt(const Stmt &s, const std::string &label);
    ASTNodePtr forEach(const Stmt &s, const std::string &label);
    ASTNodePtr switchStmt(const std::shared_ptr<SwitchBody> &sw, const std::string &label, int ln);
    ASTNodePtr tryStmt(const Stmt &s);
    StmtP desugarResources(const Stmt &s, size_t i); // try-with-resources as plain try / finally
    ASTNodePtr returnStmt(const Stmt &s);
    ASTNodePtr jump(const Stmt &s);
    ASTNodePtr throwStmt(const Stmt &s);
    ASTNodePtr labeled(const Stmt &s);
    ASTNodePtr localClass(const Stmt &s);
    ASTNodePtr loopBody(const StmtP &body, JumpTarget &t);
    ASTNodePtr finallyChain(size_t downTo, int ln); // inlined finally blocks for an exit
    void afterQuantumLoop(std::vector<ASTNodePtr> &out, size_t targetIndex, int ln);
    // checked exceptions (JLS §11.2)
    bool isChecked(const TypeP &t);
    void thrown(const TypeP &t, Pos pos);
    // definite assignment
    void markAssigned(LocalVar *v);
    bool isAssigned(LocalVar *v, FnCtx *owner);
    Flow joinFlow(const Flow &a, const Flow &b);
    bool constTrue(const ExprP &e);
    bool constFalse(const ExprP &e);

    // library (JavaLib.cpp)
    bool libraryCall(MethodSym *m, std::vector<ASTNodePtr> &args, ASTNodePtr &recv, const std::vector<Val> &argVals,
                     const TypeP &recvType, int ln, Val &out);
    ASTNodePtr printCall(const std::string &native, std::vector<Val> &args, int ln);
    ASTNodePtr formatCall(std::vector<Val> &args, size_t fmtIndex, int ln);

    // program (JavaEmit.cpp)
    ASTNodePtr emitProgram();
    ClassSym *topLevelHelper(ClassSym *c);
    std::vector<std::pair<ClassSym *, ASTNodePtr>> preludeInit_; // prelude static initializers, run before main

    // ── context and naming ──
    FnCtx *fn_ = nullptr;
    ClassSym *ctxClass_ = nullptr; // the class whose scope type names resolve in
    ClassSym *curClass() const { return fn_ ? fn_->cls : ctxClass_; }
    int unit_ = 0; // compilation unit of the code being processed
    std::string temp();
    LocalVar *findLocal(const std::string &n, FnCtx **owner = nullptr);
    LocalVar &declareLocal(const std::string &n, const TypeP &t, Pos pos, bool isFinal);
    void pushScope();
    void popScope();
    std::string uniqueLocal(const std::string &n);
    void error(Pos p, const std::string &msg, std::vector<std::string> notes = {});
    bool inStaticContext() const;
    ASTNodePtr lowerFunctionBody(FnCtx &ctx, const std::function<void(std::vector<ASTNodePtr> &)> &body, int ln);
    // A local's declaration as an assignment: Java locals are hoisted to the
    // top of their function (a Quantum block has no scope of its own, and a
    // slot defined inside a try body would not survive the handler's unwinding).
    ASTNodePtr defineLocal(const std::string &q, ASTNodePtr init, int ln);

    std::vector<CompilationUnit> &units_;
    std::vector<std::unique_ptr<ClassSym>> classes_;
    std::vector<std::unique_ptr<MethodSym>> methods_;
    std::vector<std::unique_ptr<FieldSym>> fields_;
    std::vector<std::unique_ptr<TypeVar>> tvars_;
    std::map<std::string, ClassSym *> byQual_;            // qualified name -> class
    std::map<std::string, std::vector<ClassSym *>> byPkg_; // package -> its top-level classes
    std::vector<ClassSym *> order_;                        // every class, in entering order
    std::vector<ClassSym *> lowerQueue_;
    std::vector<MethodSym *> methodQueue_;
    std::vector<ASTNodePtr> functions_;  // lowered function declarations
    std::vector<Diag> diags_;
    int tempCounter_ = 0;
    std::map<std::string, int> implNames_; // impl name -> count (overload numbering)
    std::vector<TypeVar *> methodTvars_;   // type variables of the method being entered / lowered
    ClassSym *mainClass_ = nullptr;
    MethodSym *mainMethod_ = nullptr;
    TypeP tObject_, tString_;
    std::map<std::pair<ClassSym *, std::string>, TypeP> typeCache_;
    std::map<std::string, ClassSym *> byBinary_; // binary name -> class
    std::vector<TypeP> *lambdaReturnsOverride_ = nullptr; // inference probe of a lambda's return type
    std::vector<std::pair<const ClassDecl *, ClassSym *>> anonClasses_;
    std::map<ClassSym *, MethodSym *> anonSuperCtor_;
    std::map<ClassSym *, std::vector<TypeP>> anonSuperParams_;
    LocalVar *findLocalOuter(const std::string &n);
    std::vector<TypeP> yieldTypes_; // result types of the switch expressions being lowered
    // > 0 while an expression is attributed only to learn its type or constant
    // value (the code is discarded): no captures or assignments are recorded
    int probing_ = 0;
};

// Quantum AST builders (JavaEmit.cpp).
namespace q
{
ASTNodePtr num(double v, int ln);
ASTNodePtr lng(int64_t v, int ln); // an exact int64 constant
ASTNodePtr str(const std::string &s, int ln);
ASTNodePtr boolean(bool b, int ln);
ASTNodePtr nil(int ln);
ASTNodePtr id(const std::string &n, int ln);
ASTNodePtr call(ASTNodePtr callee, std::vector<ASTNodePtr> args, int ln);
ASTNodePtr call(const std::string &fn, std::vector<ASTNodePtr> args, int ln);
ASTNodePtr assign(ASTNodePtr target, ASTNodePtr value, int ln);
ASTNodePtr seq(std::vector<ASTNodePtr> parts, int ln); // evaluates all, yields the last
ASTNodePtr bin(int op, ASTNodePtr l, ASTNodePtr r, int ln);  // J_BIN
ASTNodePtr un(int op, ASTNodePtr x, int ln);                  // J_UN
ASTNodePtr qbin(const std::string &op, ASTNodePtr l, ASTNodePtr r, int ln); // plain Quantum operator
ASTNodePtr qnot(ASTNodePtr x, int ln);
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
ASTNodePtr raise(ASTNodePtr e, int ln);
ASTNodePtr tryCatch(ASTNodePtr body, const std::string &var, ASTNodePtr handler, int ln);
ASTNodePtr func(const std::string &name, std::vector<std::string> params, ASTNodePtr body, int ln);
ASTNodePtr lambda(std::vector<std::string> params, ASTNodePtr body, int ln);
ASTNodePtr getf(ASTNodePtr obj, const std::string &field, int ln);
ASTNodePtr putf(ASTNodePtr obj, const std::string &field, ASTNodePtr v, int ln);
ASTNodePtr fupd(ASTNodePtr obj, const std::string &field, ASTNodePtr upd, int ln);
ASTNodePtr aload(ASTNodePtr a, ASTNodePtr i, int ln);
ASTNodePtr astore(ASTNodePtr a, ASTNodePtr i, ASTNodePtr v, int ln);
ASTNodePtr aupd(ASTNodePtr a, ASTNodePtr i, ASTNodePtr upd, int ln);
ASTNodePtr old(int ln);
ASTNodePtr inv(const std::string &key, ASTNodePtr recv, std::vector<ASTNodePtr> args, int ln,
               const std::string &fallback = "");
ASTNodePtr instOf(ASTNodePtr v, const std::string &desc, int ln);
ASTNodePtr cast(ASTNodePtr v, const std::string &desc, int ln);
ASTNodePtr clone(const ASTNodePtr &n);
bool isSimple(const ASTNodePtr &n); // re-evaluating it is free and side-effect free
} // namespace q

// Entry point (JavaMain.cpp): parses `source` plus the prelude, attributes
// and lowers; throws CompileError (javac-formatted) on errors.
ASTNodePtr compileProgram(const std::string &path, const std::string &source);

// The prelude sources (JavaPrelude*.cpp): (file name, Java source).
const std::vector<std::pair<std::string, std::string>> &preludeSources();

} // namespace java
