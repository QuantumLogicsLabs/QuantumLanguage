// Lowering for the typed C/C++ front-end: C++ AST → Quantum AST with C++
// semantics (see Cpp.h for the overall design, CppLib.cpp for the library).
//
// Runtime representation (all on the existing VM):
//   integers, char, enums, double → numbers (char holds its code)
//   bool → bool;  std::string / const char* → string (immutable)
//   class object → array [descriptor, field...]   (slot 0 = descriptor dict)
//   vector / array / deque / ... → array;  pair / tuple → array
//   map / set → dict of encoded key → [key, value] / [key]
//   optional → nil | [value];  variant → [index, value]
//   T* / unique_ptr<T> to an object → the object;  nullptr → nil
//   T* / T& to a scalar → Ref [container, key] (a boxed local is [value])
#include "CppLower.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>

extern bool g_strictMode; // src/Pipeline.cpp

namespace cpp
{

// ─── Types ─────────────────────────────────────────────────────────────────

TP mkT(T k, const std::string &name)
{
    auto t = std::make_shared<Type>();
    t->k = k;
    t->name = name;
    return t;
}
TP tInt()
{
    static TP t = mkT(T::Int, "int");
    return t;
}
TP tLL()
{
    static TP t = mkT(T::Int, "long long");
    return t;
}
TP tSize()
{
    static TP t = []
    {
        auto x = mkT(T::Int, "unsigned long long");
        x->isUnsigned = true;
        return x;
    }();
    return t;
}
TP tDouble()
{
    static TP t = mkT(T::Float, "double");
    return t;
}
TP tBool()
{
    static TP t = mkT(T::Bool, "bool");
    return t;
}
TP tChar()
{
    static TP t = mkT(T::Char, "char");
    return t;
}
TP tStr()
{
    static TP t = mkT(T::Str, "string");
    return t;
}
TP tCStr()
{
    static TP t = mkT(T::CStr, "const char*");
    return t;
}
TP tBuf(long n)
{
    TP t = mkT(T::Str, "char[]");
    t->n = n;
    return t;
}
TP tVoid()
{
    static TP t = mkT(T::Void, "void");
    return t;
}
TP tDyn()
{
    static TP t = mkT(T::Dyn, "");
    return t;
}

std::string Type::str() const
{
    auto args = [&]
    {
        std::string s = "<";
        for (size_t i = 0; i < a.size(); ++i)
            s += (i ? "," : "") + (a[i] ? a[i]->str() : std::string("?"));
        return s + ">";
    };
    switch (k)
    {
    case T::Dyn: return name.empty() ? "?" : "?" + name;
    case T::Void: return "void";
    case T::Bool: return "bool";
    case T::Char: case T::Int: case T::Float: return name;
    case T::Str: return "string";
    case T::CStr: return "cstr";
    case T::Nullptr: return "nullptr_t";
    case T::Ptr: return (a.empty() ? std::string("?") : a[0]->str()) + "*";
    case T::Class: return cls ? cls->qname : "class?";
    case T::Enum: return en ? en->qname : "enum?";
    case T::Vector: return name + args();
    case T::Array: return "array" + args() + std::to_string(n);
    case T::Map: return std::string(sorted ? "map" : "umap") + (multi ? "m" : "") + args();
    case T::Set: return std::string(sorted ? "set" : "uset") + (multi ? "m" : "") + args();
    case T::Pair: return "pair" + args();
    case T::Tuple: return "tuple" + args();
    case T::Optional: return "optional" + args();
    case T::Variant: return "variant" + args();
    case T::UPtr: return name + args();
    case T::Func: return "function" + args();
    case T::Stream: return name;
    case T::Iter: return "iter" + args();
    case T::Monostate: return "monostate";
    case T::Nullopt: return "nullopt_t";
    case T::InitList: return "initlist" + args();
    }
    return "?";
}

bool isIntegral(const TP &t) { return t && (t->k == T::Int || t->k == T::Char || t->k == T::Bool || t->k == T::Enum); }
bool isFloat(const TP &t) { return t && t->k == T::Float; }
bool isArith(const TP &t) { return isIntegral(t) || isFloat(t); }
bool isStringy(const TP &t) { return t && (t->k == T::Str || t->k == T::CStr); }
bool isCBuf(const TP &t) { return t && t->k == T::Str && t->name == "char[]"; }
bool isI64(const TP &t) { return t && t->k == T::Int && (t->name == "long long" || t->name == "unsigned long long"); }
bool isObjectRep(const TP &t)
{
    if (!t)
        return false;
    switch (t->k)
    {
    case T::Class: case T::Vector: case T::Array: case T::Map: case T::Set: case T::Pair: case T::Tuple:
    case T::Stream:
        return true;
    default:
        return false;
    }
}
bool needsCopy(const TP &t)
{
    if (!t)
        return false;
    switch (t->k)
    {
    case T::Class: case T::Vector: case T::Array: case T::Map: case T::Set: case T::Pair: case T::Tuple:
    case T::Optional: case T::Variant:
        return true;
    default:
        return false;
    }
}
bool sameType(const TP &a, const TP &b)
{
    if (!a || !b)
        return false;
    if (a.get() == b.get())
        return true;
    if (a->k != b->k)
        return false;
    if (a->k == T::Class)
        return a->cls == b->cls;
    if (a->k == T::Enum)
        return a->en == b->en;
    return a->str() == b->str();
}

// ─── Quantum AST builders ──────────────────────────────────────────────────

namespace q
{
ASTNodePtr num(double v, int ln) { return std::make_unique<ASTNode>(NumberLiteral{v}, ln); }
ASTNodePtr str(const std::string &s, int ln) { return std::make_unique<ASTNode>(StringLiteral{s}, ln); }
ASTNodePtr boolean(bool b, int ln) { return std::make_unique<ASTNode>(BoolLiteral{b}, ln); }
ASTNodePtr nil(int ln) { return std::make_unique<ASTNode>(NilLiteral{}, ln); }
ASTNodePtr id(const std::string &n, int ln) { return std::make_unique<ASTNode>(Identifier{n}, ln); }
ASTNodePtr call(ASTNodePtr callee, std::vector<ASTNodePtr> args, int ln)
{
    CallExpr c;
    c.callee = std::move(callee);
    c.args = std::move(args);
    return std::make_unique<ASTNode>(std::move(c), ln);
}
ASTNodePtr call(const std::string &fn, std::vector<ASTNodePtr> args, int ln) { return call(id(fn, ln), std::move(args), ln); }
ASTNodePtr index(ASTNodePtr obj, ASTNodePtr key, int ln)
{
    return std::make_unique<ASTNode>(IndexExpr{std::move(obj), std::move(key)}, ln);
}
ASTNodePtr assign(ASTNodePtr target, ASTNodePtr value, int ln)
{
    return std::make_unique<ASTNode>(AssignExpr{"=", std::move(target), std::move(value)}, ln);
}
ASTNodePtr bin(const std::string &op, ASTNodePtr l, ASTNodePtr r, int ln)
{
    return std::make_unique<ASTNode>(BinaryExpr{op, std::move(l), std::move(r)}, ln);
}
ASTNodePtr un(const std::string &op, ASTNodePtr x, int ln)
{
    return std::make_unique<ASTNode>(UnaryExpr{op, std::move(x)}, ln);
}
ASTNodePtr ternary(ASTNodePtr c, ASTNodePtr a, ASTNodePtr b, int ln)
{
    return std::make_unique<ASTNode>(TernaryExpr{std::move(c), std::move(a), std::move(b)}, ln);
}
ASTNodePtr arr(std::vector<ASTNodePtr> elems, int ln)
{
    ArrayLiteral a;
    a.elements = std::move(elems);
    return std::make_unique<ASTNode>(std::move(a), ln);
}
ASTNodePtr block(std::vector<ASTNodePtr> stmts, int ln)
{
    BlockStmt b;
    b.statements = std::move(stmts);
    return std::make_unique<ASTNode>(std::move(b), ln);
}
ASTNodePtr exprStmt(ASTNodePtr e, int ln) { return std::make_unique<ASTNode>(ExprStmt{std::move(e)}, ln); }
ASTNodePtr var(const std::string &n, ASTNodePtr init, int ln)
{
    return std::make_unique<ASTNode>(VarDecl{false, n, std::move(init), ""}, ln);
}
ASTNodePtr ret(ASTNodePtr e, int ln) { return std::make_unique<ASTNode>(ReturnStmt{std::move(e)}, ln); }
ASTNodePtr ifs(ASTNodePtr c, ASTNodePtr t, ASTNodePtr e, int ln)
{
    if (!t)
        t = block({}, ln); // if (x) ;
    return std::make_unique<ASTNode>(IfStmt{std::move(c), std::move(t), std::move(e)}, ln);
}
ASTNodePtr whiles(ASTNodePtr c, ASTNodePtr body, ASTNodePtr post, int ln)
{
    if (!body)
        body = block({}, ln); // while (x) ;
    return std::make_unique<ASTNode>(WhileStmt{std::move(c), std::move(body), std::move(post)}, ln);
}
ASTNodePtr brk(int ln) { return std::make_unique<ASTNode>(BreakStmt{}, ln); }
ASTNodePtr cont(int ln) { return std::make_unique<ASTNode>(ContinueStmt{}, ln); }
ASTNodePtr func(const std::string &name, std::vector<std::string> params, ASTNodePtr body, int ln)
{
    FunctionDecl f;
    f.name = name;
    f.params = std::move(params);
    f.paramIsRef.assign(f.params.size(), false);
    f.body = std::move(body);
    return std::make_unique<ASTNode>(std::move(f), ln);
}
ASTNodePtr lambda(std::vector<std::string> params, ASTNodePtr body, int ln)
{
    LambdaExpr l;
    l.params = std::move(params);
    l.body = std::move(body);
    return std::make_unique<ASTNode>(std::move(l), ln);
}

ASTNodePtr clone(const ASTNodePtr &n)
{
    if (!n)
        return nullptr;
    int ln = n->line;
    const auto &v = n->node;
    if (auto *x = std::get_if<NumberLiteral>(&v)) return num(x->value, ln);
    if (auto *x = std::get_if<StringLiteral>(&v)) return str(x->value, ln);
    if (auto *x = std::get_if<BoolLiteral>(&v)) return boolean(x->value, ln);
    if (std::get_if<NilLiteral>(&v)) return nil(ln);
    if (auto *x = std::get_if<Identifier>(&v)) return id(x->name, ln);
    if (auto *x = std::get_if<IndexExpr>(&v)) return index(clone(x->object), clone(x->index), ln);
    if (auto *x = std::get_if<BinaryExpr>(&v)) return bin(x->op, clone(x->left), clone(x->right), ln);
    if (auto *x = std::get_if<UnaryExpr>(&v)) return un(x->op, clone(x->operand), ln);
    if (auto *x = std::get_if<TernaryExpr>(&v))
        return ternary(clone(x->condition), clone(x->thenExpr), clone(x->elseExpr), ln);
    if (auto *x = std::get_if<CallExpr>(&v))
    {
        std::vector<ASTNodePtr> args;
        for (auto &a : x->args)
            args.push_back(clone(a));
        return call(clone(x->callee), std::move(args), ln);
    }
    if (auto *x = std::get_if<ArrayLiteral>(&v))
    {
        std::vector<ASTNodePtr> els;
        for (auto &a : x->elements)
            els.push_back(clone(a));
        return arr(std::move(els), ln);
    }
    if (auto *x = std::get_if<AssignExpr>(&v))
        return std::make_unique<ASTNode>(AssignExpr{x->op, clone(x->target), clone(x->value)}, ln);
    if (auto *x = std::get_if<MemberExpr>(&v))
        return std::make_unique<ASTNode>(MemberExpr{clone(x->object), x->member}, ln);
    throw Unsupported("internal: clone of an unexpected node");
}

bool isSimple(const ASTNodePtr &n)
{
    if (!n)
        return true;
    const auto &v = n->node;
    if (std::holds_alternative<NumberLiteral>(v) || std::holds_alternative<StringLiteral>(v) ||
        std::holds_alternative<BoolLiteral>(v) || std::holds_alternative<NilLiteral>(v) ||
        std::holds_alternative<Identifier>(v))
        return true;
    if (auto *x = std::get_if<IndexExpr>(&v))
        return isSimple(x->object) && isSimple(x->index);
    return false;
}
} // namespace q

// ─── Lowerer: setup ────────────────────────────────────────────────────────

Lowerer::Lowerer(Program &prog) : prog_(prog)
{
    global_ = newScope("", nullptr);
    std_ = newScope("std", global_);
    std_->isStd = true;
    Symbol s;
    s.kind = Symbol::Namespace;
    s.ns = std_;
    global_->syms["std"] = s;
    declareBuiltins();
}

Scope *Lowerer::newScope(const std::string &qname, Scope *parent)
{
    scopes_.push_back(std::make_unique<Scope>());
    Scope *s = scopes_.back().get();
    s->qname = qname;
    s->parent = parent;
    return s;
}

void Lowerer::unsupported(const std::string &msg, int line)
{
    throw Unsupported(msg, "", line ? line : line_);
}

std::string Lowerer::tempName() { return "__t" + std::to_string(++tempCounter_); }

std::string Lowerer::mangle(Scope *sc, const std::string &name)
{
    return sc && !sc->qname.empty() ? sc->qname + "::" + name : name;
}

ClassInfo *Lowerer::builtinExceptionClass(const std::string &n)
{
    auto it = builtinExc_.find(n);
    return it == builtinExc_.end() ? nullptr : it->second;
}

// std exception classes: layout [desc, what], constructible from a string.
void Lowerer::declareBuiltins()
{
    struct E
    {
        const char *name, *base;
    };
    static const E exc[] = {{"exception", nullptr},
                            {"bad_alloc", "exception"},
                            {"bad_cast", "exception"},
                            {"bad_variant_access", "exception"},
                            {"bad_optional_access", "exception"},
                            {"bad_function_call", "exception"},
                            {"logic_error", "exception"},
                            {"invalid_argument", "logic_error"},
                            {"out_of_range", "logic_error"},
                            {"length_error", "logic_error"},
                            {"domain_error", "logic_error"},
                            {"runtime_error", "exception"},
                            {"range_error", "runtime_error"},
                            {"overflow_error", "runtime_error"},
                            {"underflow_error", "runtime_error"},
                            {"system_error", "runtime_error"}};
    for (const E &e : exc)
    {
        classes_.push_back(std::make_unique<ClassInfo>());
        ClassInfo *c = classes_.back().get();
        c->name = e.name;
        c->qname = std::string("std::") + e.name;
        c->builtin = true;
        c->resolved = true;
        c->polymorphic = true;
        c->aggregate = false;
        c->hasUserCtor = true;
        c->nslots = 2;
        if (e.base)
            c->bases.push_back(builtinExc_[e.base]);
        c->scope = newScope(c->qname, std_);
        c->scope->cls = c;
        builtinExc_[e.name] = c;
        auto t = mkT(T::Class);
        t->cls = c;
        Symbol s;
        s.kind = Symbol::Type;
        s.type = t;
        std_->syms[e.name] = s;
    }
}

// ─── Pass 1: declarations ──────────────────────────────────────────────────

void Lowerer::collect(const std::vector<DeclP> &decls, Scope *sc)
{
    for (const auto &d : decls)
    {
        line_ = d->line;
        switch (d->k)
        {
        case DK::Namespace:
        {
            Scope *ns = sc;
            if (d->anonymous)
            {
                std::string n = "(anon" + std::to_string(d->tu) + ")";
                auto it = sc->syms.find(n);
                if (it == sc->syms.end())
                {
                    Symbol s;
                    s.kind = Symbol::Namespace;
                    s.ns = newScope(mangle(sc, n), sc);
                    sc->syms[n] = s;
                    sc->usings.push_back(s.ns);
                    it = sc->syms.find(n);
                }
                ns = it->second.ns;
            }
            else
            {
                for (const auto &part : d->name)
                {
                    auto it = ns->syms.find(part);
                    if (it == ns->syms.end() || it->second.kind != Symbol::Namespace)
                    {
                        Symbol s;
                        s.kind = Symbol::Namespace;
                        s.ns = part == "std" && ns == global_ ? std_ : newScope(mangle(ns, part), ns);
                        ns->syms[part] = s;
                        it = ns->syms.find(part);
                    }
                    ns = it->second.ns;
                }
            }
            collect(d->decls, ns);
            break;
        }
        case DK::Class:
            if (!d->forwardOnly)
                collectClass(d, sc);
            else if (!d->name.empty() && !d->name.back().empty() && !sc->syms.count(d->name.back()))
            {
                // forward declaration: the definition fills it in later
                collectClass(d, sc);
            }
            break;
        case DK::Enum:
            if (!d->forwardOnly)
                collectEnum(d, sc);
            break;
        case DK::Alias:
        {
            Symbol s;
            s.kind = Symbol::Type;
            s.aliasDecl = d;
            s.aliasScope = sc;
            sc->syms[d->name[0]] = s;
            break;
        }
        case DK::Func:
            funcDecls_.push_back({d.get(), sc});
            break;
        case DK::Var:
        {
            if (d->name.size() > 1)
            {
                // out-of-class static member definition: int Cls::n = 0;
                globalsInOrder_.push_back({d, sc});
                break;
            }
            if (d->isExtern && d->var.init == VarDeclarator::None)
                break;
            Symbol s;
            s.kind = Symbol::Var;
            s.global = mangle(sc, d->var.name);
            if (const auto &vt = d->var.type)
                s.isConst = (vt->isConst && vt->pointers == 0) || (!vt->constPtr.empty() && vt->constPtr.back()) ||
                            d->isConstexpr;
            sc->syms[d->var.name] = s; // type resolved in emitGlobals (aliases may come later)
            globalsInOrder_.push_back({d, sc});
            break;
        }
        case DK::UsingDirective:
        {
            Symbol *s = lookup(d->name, sc);
            if (s && s->kind == Symbol::Namespace)
                sc->usings.push_back(s->ns);
            break;
        }
        case DK::UsingDecl:
        {
            // using std::cout; using ns::f;  (resolved lazily at lookup)
            Symbol s;
            s.kind = Symbol::None;
            s.aliasDecl = d;
            s.aliasScope = sc;
            if (!d->inheritingCtor)
                sc->usingDecls[d->name.back()] = s;
            break;
        }
        default:
            break;
        }
    }
}

void Lowerer::collectClass(const DeclP &d, Scope *sc, const std::string &forcedName)
{
    std::string name = forcedName.empty() ? d->name.back() : forcedName;
    if (name.empty())
        name = "(anon" + std::to_string(++localClassCounter_) + ")";
    ClassInfo *c = nullptr;
    auto it = sc->syms.find(name);
    if (it != sc->syms.end() && it->second.kind == Symbol::Type && it->second.type &&
        it->second.type->k == T::Class && !it->second.type->cls->builtin)
        c = it->second.type->cls; // completes a forward declaration
    if (!c)
    {
        classes_.push_back(std::make_unique<ClassInfo>());
        c = classes_.back().get();
        c->name = name;
        c->qname = mangle(sc, name);
        c->outer = sc;
        c->scope = newScope(c->qname, sc);
        c->scope->cls = c;
        auto t = mkT(T::Class);
        t->cls = c;
        Symbol s;
        s.kind = Symbol::Type;
        s.type = t;
        sc->syms[name] = s;
    }
    if (d->forwardOnly)
        return;
    if (prog_.cMode)
        for (const DeclP &m : d->members)
            if (m->k == DK::Func)
                unsupported("field '" + (m->name.empty() ? std::string("?") : m->name.back()) +
                                "' declared as a function",
                            m->line);
    c->decl = d;
    c->isTemplate = d->isTemplate;
    if (d->isTemplate)
        for (const auto &tp : d->templateParams)
        {
            Symbol s;
            s.kind = Symbol::Type;
            s.type = tDyn();
            c->scope->syms[tp] = s;
        }
    classOrder_.push_back(c);
    // nested types and members' declarations
    for (const auto &m : d->members)
    {
        if (m->k == DK::Class && !m->forwardOnly)
            collectClass(m, c->scope);
        else if (m->k == DK::Enum && !m->forwardOnly)
            collectEnum(m, c->scope);
        else if (m->k == DK::Alias)
        {
            Symbol s;
            s.kind = Symbol::Type;
            s.aliasDecl = m;
            s.aliasScope = c->scope;
            c->scope->syms[m->name[0]] = s;
        }
        else if (m->k == DK::Func && m->isFriend && m->body)
            funcDecls_.push_back({m.get(), sc}); // friend function defined in the class: a free function
    }
}

void Lowerer::collectEnum(const DeclP &d, Scope *sc)
{
    enums_.push_back(std::make_unique<EnumInfo>());
    EnumInfo *e = enums_.back().get();
    std::string name = d->name.back().empty() ? "(anonenum" + std::to_string(++localClassCounter_) + ")" : d->name.back();
    e->qname = mangle(sc, name);
    e->scoped = d->scoped;
    auto t = mkT(T::Enum);
    t->en = e;
    t->name = e->qname;
    Scope *es = newScope(e->qname, sc);
    e->scope = es;
    Symbol ts;
    ts.kind = Symbol::Type;
    ts.type = t;
    ts.ns = es;
    if (!d->name.back().empty())
        sc->syms[name] = ts;
    long long next = 0;
    for (const auto &en : d->enumerators)
    {
        long long v = en.second ? evalConst(en.second, es) : next;
        e->values.push_back({en.first, v});
        next = v + 1;
        Symbol s;
        s.kind = Symbol::Enumerator;
        s.type = t;
        s.value = v;
        es->syms[en.first] = s;
        if (!d->scoped)
            sc->syms[en.first] = s;
    }
}

// ─── Lookup ────────────────────────────────────────────────────────────────

Symbol *Lowerer::lookupIn(Scope *s, const std::string &n, bool typesOnly)
{
    auto it = s->syms.find(n);
    if (it != s->syms.end() && (!typesOnly || it->second.kind == Symbol::Type || it->second.kind == Symbol::Namespace))
    {
        Symbol &sym = it->second;
        if (sym.kind == Symbol::Type && sym.aliasDecl && !sym.type)
        {
            DeclP ad = sym.aliasDecl;
            sym.aliasDecl = nullptr; // guard recursion
            sym.type = resolveType(ad->aliased, sym.aliasScope);
        }
        return &sym;
    }
    // class scope: members of base classes
    if (s->cls)
        for (ClassInfo *b : s->cls->bases)
            if (b->scope)
                if (Symbol *r = lookupIn(b->scope, n, typesOnly))
                    return r;
    auto ud = s->usingDecls.find(n);
    if (ud != s->usingDecls.end())
    {
        Symbol &u = ud->second;
        if (u.kind == Symbol::None && u.aliasDecl)
        {
            DeclP ad = u.aliasDecl;
            u.aliasDecl = nullptr;
            Symbol *target = lookup(ad->name, u.aliasScope, typesOnly);
            if (target)
                u = *target;
        }
        if (u.kind != Symbol::None)
            return &u;
    }
    return nullptr;
}

Symbol *Lowerer::lookupUnqualified(const std::string &n, Scope *sc, bool typesOnly)
{
    for (Scope *s = sc; s; s = s->parent)
    {
        if (Symbol *r = lookupIn(s, n, typesOnly))
            return r;
        for (Scope *u : s->usings)
            if (Symbol *r = lookupIn(u, n, typesOnly))
                return r;
    }
    return nullptr;
}

Symbol *Lowerer::lookup(const std::vector<std::string> &name, Scope *sc, bool typesOnly)
{
    if (name.empty())
        return nullptr;
    if (name.size() == 1)
        return lookupUnqualified(name[0], sc, typesOnly);
    Symbol *s = lookupUnqualified(name[0], sc, false);
    for (size_t i = 1; s && i < name.size(); ++i)
    {
        Scope *in = nullptr;
        if (s->kind == Symbol::Namespace)
            in = s->ns;
        else if (s->kind == Symbol::Type && s->type && s->type->k == T::Class && s->type->cls)
            in = s->type->cls->scope;
        else if (s->kind == Symbol::Type && s->type && s->type->k == T::Enum)
            in = s->ns ? s->ns : (s->type->en ? s->type->en->scope : nullptr);
        if (!in)
            return nullptr;
        Symbol *next = lookupIn(in, name[i], typesOnly && i + 1 == name.size());
        if (!next && in == std_)
            return nullptr;
        s = next;
    }
    return s;
}

// ─── Types from syntax ─────────────────────────────────────────────────────

TP Lowerer::builtinType(const TypeName &tn, Scope *sc, bool &ok)
{
    ok = true;
    std::vector<std::string> n = tn.name;
    bool isStdQual = !n.empty() && n[0] == "std";
    if (isStdQual)
        n.erase(n.begin());
    if (n.size() != 1)
    {
        // std::vector<int>::iterator, std::string::size_type, Cls::type
        if (n.size() == 2 && (n[1] == "iterator" || n[1] == "const_iterator" || n[1] == "reverse_iterator"))
        {
            TypeName inner = tn;
            inner.name = {tn.name.begin(), tn.name.end() - 1};
            inner.targs = {tn.targs.begin(), tn.targs.end() - 1};
            inner.pointers = 0;
            inner.ref = inner.rref = false;
            inner.arrayDims.clear();
            auto it = mkT(T::Iter);
            it->a.push_back(resolveType(std::make_shared<TypeName>(inner), sc));
            return it;
        }
        if (n.size() == 2 && (n[1] == "size_type" || n[1] == "difference_type"))
            return tSize();
        if (n.size() == 2 && (n[1] == "value_type" || n[1] == "mapped_type" || n[1] == "key_type"))
        {
            TypeName inner = tn;
            inner.name = {tn.name.begin(), tn.name.end() - 1};
            inner.targs = {tn.targs.begin(), tn.targs.end() - 1};
            TP ct = resolveType(std::make_shared<TypeName>(inner), sc);
            if (ct->k == T::Map)
                return n[1] == "mapped_type" ? ct->a[1] : n[1] == "key_type" ? ct->a[0] : [&]
                {
                    auto p = mkT(T::Pair);
                    p->a = ct->a;
                    return p;
                }();
            return elemType(ct);
        }
        ok = false;
        return nullptr;
    }
    const std::string &w = n[0];
    auto targ = [&](size_t i) -> TP
    {
        const auto &ta = tn.targs.back();
        if (i >= ta.size() || !ta[i].type)
            return tDyn();
        return resolveType(ta[i].type, sc);
    };
    auto intT = [](const char *name, bool u)
    {
        auto t = mkT(T::Int, name);
        t->isUnsigned = u;
        return t;
    };
    if (w == "va_list" || w == "__builtin_va_list")
        return mkT(T::Dyn, "va_list");
    // built-in arithmetic
    if (w == "void") return tVoid();
    if (w == "bool") return tBool();
    if (w == "char" || w == "signed char" || w == "wchar_t" || w == "char8_t" || w == "char16_t" || w == "char32_t")
        return mkT(T::Char, w);
    if (w == "unsigned char" || w == "uint8_t" || w == "byte")
    {
        auto t = mkT(T::Char, "unsigned char");
        t->isUnsigned = true;
        return t;
    }
    if (w == "int8_t")
        return mkT(T::Char, "signed char");
    if (w == "short" || w == "int" || w == "long" || w == "int16_t" || w == "int32_t" || w == "intptr_t" ||
        w == "ptrdiff_t" || w == "ssize_t")
        return intT(w == "int32_t" ? "int" : w.c_str(), false);
    if (w == "long long" || w == "int64_t" || w == "intmax_t" || w == "streamsize" || w == "time_t" ||
        w == "clock_t")
        return intT("long long", false);
    if (w == "unsigned short" || w == "uint16_t")
        return intT("unsigned short", true);
    if (w == "unsigned int" || w == "unsigned" || w == "uint32_t")
        return intT("unsigned int", true);
    if (w == "unsigned long")
        return intT("unsigned long", true);
    if (w == "unsigned long long" || w == "uint64_t" || w == "size_t" || w == "uintptr_t" || w == "uintmax_t")
        return intT("unsigned long long", true);
    if (w == "float")
        return mkT(T::Float, "float");
    if (w == "double" || w == "long double")
        return tDouble();
    if (w == "auto")
        return tDyn();
    // library types (std:: optional — `using namespace std` is common)
    bool userShadow = false;
    if (!isStdQual)
    {
        Symbol *s = lookupUnqualified(w, sc, true);
        if (s && s->kind == Symbol::Type && sc != std_)
        {
            // a user type of the same name wins
            if (s->type || s->aliasDecl)
                userShadow = true;
        }
    }
    if (userShadow)
    {
        ok = false;
        return nullptr;
    }
    if (!isStdQual && !prog_.cMode && w != "FILE" && w != "nullptr_t" && !stdVisible(w, sc ? sc : global_))
    {
        ok = false; // std's, but no `using namespace std` / `using std::...` here
        return nullptr;
    }
    if (w == "string" || w == "wstring" || w == "string_view")
        return tStr();
    if (w == "vector" || w == "deque" || w == "list" || w == "forward_list" || w == "stack" || w == "queue" ||
        w == "priority_queue" || w == "initializer_list" || w == "valarray")
    {
        auto t = mkT(T::Vector, w == "initializer_list" || w == "valarray" ? "vector" : w);
        t->a.push_back(targ(0));
        if (w == "priority_queue")
        {
            const auto &ta = tn.targs.back();
            // priority_queue<T, Container, Compare>: remember a greater<> comparator
            if (ta.size() >= 3 && ta[2].type && !ta[2].type->name.empty() && ta[2].type->name.back() == "greater")
                t->multi = true; // min-heap
        }
        return t;
    }
    if (w == "array")
    {
        auto t = mkT(T::Array, "array");
        t->a.push_back(targ(0));
        const auto &ta = tn.targs.back();
        if (ta.size() > 1 && ta[1].expr)
            t->n = (long)evalConst(ta[1].expr, sc);
        return t;
    }
    if (w == "map" || w == "unordered_map" || w == "multimap" || w == "unordered_multimap")
    {
        auto t = mkT(T::Map, w);
        t->sorted = w == "map" || w == "multimap";
        t->multi = w.find("multi") != std::string::npos;
        t->a = {targ(0), targ(1)};
        return t;
    }
    if (w == "set" || w == "unordered_set" || w == "multiset" || w == "unordered_multiset")
    {
        auto t = mkT(T::Set, w);
        t->sorted = w == "set" || w == "multiset";
        t->multi = w.find("multi") != std::string::npos;
        t->a = {targ(0)};
        return t;
    }
    if (w == "pair")
    {
        auto t = mkT(T::Pair);
        t->a = {targ(0), targ(1)};
        return t;
    }
    if (w == "tuple")
    {
        auto t = mkT(T::Tuple);
        for (size_t i = 0; i < tn.targs.back().size(); ++i)
            t->a.push_back(targ(i));
        return t;
    }
    if (w == "optional")
    {
        auto t = mkT(T::Optional);
        t->a = {targ(0)};
        return t;
    }
    if (w == "variant")
    {
        auto t = mkT(T::Variant);
        for (size_t i = 0; i < tn.targs.back().size(); ++i)
            t->a.push_back(targ(i));
        return t;
    }
    if (w == "monostate")
        return mkT(T::Monostate);
    if (w == "nullopt_t")
        return mkT(T::Nullopt);
    if (w == "nullptr_t")
        return mkT(T::Nullptr);
    if (w == "unique_ptr" || w == "shared_ptr" || w == "weak_ptr")
    {
        auto t = mkT(T::UPtr, w);
        t->a = {targ(0)};
        return t;
    }
    if (w == "function")
    {
        auto t = mkT(T::Func);
        const auto &ta = tn.targs.back();
        if (!ta.empty() && ta[0].type && ta[0].type->isFunction)
        {
            t->a.push_back(ta[0].type->ret ? resolveType(ta[0].type->ret, sc) : tVoid());
            for (auto &p : ta[0].type->params)
                t->a.push_back(resolveType(p, sc));
        }
        else
            t->a.push_back(tDyn());
        return t;
    }
    if (w == "ostream" || w == "istream" || w == "iostream" || w == "ostringstream" || w == "istringstream" ||
        w == "stringstream" || w == "ifstream" || w == "ofstream" || w == "fstream")
        return mkT(T::Stream, w);
    if (builtinExc_.count(w))
    {
        auto t = mkT(T::Class);
        t->cls = builtinExc_[w];
        return t;
    }
    if (w == "mt19937" || w == "mt19937_64" || w == "random_device" || w == "uniform_int_distribution" ||
        w == "uniform_real_distribution" || w == "FILE" || w == "any" || w == "bitset" || w == "reference_wrapper" ||
        w == "less" || w == "greater" || w == "hash" || w == "numeric_limits")
        return mkT(T::Dyn, w);
    ok = false;
    return nullptr;
}

TP Lowerer::resolveType(const TypeNameP &tn, Scope *sc)
{
    if (!tn)
        return tDyn();
    TP base;
    if (tn->isFunction)
    {
        auto f = mkT(T::Func);
        f->a.push_back(tn->ret ? resolveType(tn->ret, sc) : tVoid());
        for (auto &p : tn->params)
            f->a.push_back(resolveType(p, sc));
        base = f;
        // function pointers are just functions here
        TP t = base;
        for (size_t i = tn->arrayDims.size(); i-- > 0;)
        {
            auto a = mkT(T::Array, "carray");
            a->a.push_back(t);
            a->n = tn->arrayDims[i] ? (long)evalConst(tn->arrayDims[i], sc) : -1;
            t = a;
        }
        return t;
    }
    bool ok = false;
    // user types first when unqualified or user-qualified
    Symbol *s = nullptr;
    auto local = tn->name.size() == 1 ? localTypes_.find(tn->name[0]) : localTypes_.end();
    if (local != localTypes_.end())
        base = local->second;
    else if (!(tn->name.size() >= 1 && tn->name[0] == "std"))
        s = lookup(tn->name, sc, true);
    if (base)
        ;
    else if (s && s->kind == Symbol::Type && s->type)
    {
        base = s->type;
        if (base->k == T::Class && base->cls && base->cls->isTemplate && !tn->targs.empty() && !tn->targs.back().empty())
            base = instantiateClass(base->cls, tn->targs.back(), sc); // Matrix<int>
    }
    else
    {
        base = builtinType(*tn, sc, ok);
        if (!ok)
        {
            if (s && s->kind == Symbol::Type && !s->type)
                base = tDyn();
            else if (g_strictMode && !s)
            {
                // A type C/C++ has never heard of (`let x = 1;`) is a compile
                // error; a real library type outside the subset is reported
                // as such rather than run with guessed semantics.
                static const std::set<std::string> knownStd = {
                    "regex", "smatch", "cmatch", "thread", "mutex", "lock_guard", "unique_lock",
                    "condition_variable", "atomic", "future", "promise", "complex", "chrono",
                    "duration", "time_point", "steady_clock", "system_clock", "high_resolution_clock",
                    "filesystem", "path", "span", "ranges", "locale", "wostream", "wistream",
                    "basic_string", "allocator", "ratio", "type_info", "type_index", "jthread",
                    "barrier", "latch", "semaphore", "counting_semaphore", "binary_semaphore",
                    "default_random_engine", "minstd_rand", "normal_distribution",
                    "bernoulli_distribution", "discrete_distribution", "seed_seq", "va_list",
                    "jmp_buf", "fpos_t", "div_t", "ldiv_t", "tm", "wint_t", "mbstate_t"};
                std::string last = tn->name.empty() ? "" : tn->name.back();
                std::string full;
                for (const auto &part : tn->name)
                    full += (full.empty() ? "" : "::") + part;
                if (knownStd.count(last))
                    unsupported("unsupported: '" + full + "' is not supported by Quantum's C/C++ front-end", line_);
                if (tn->name.size() >= 2 && tn->name[0] == "std")
                    unsupported("unknown std member '" + last + "'", line_);
                unsupported("unknown type '" + full + "'", line_);
            }
            else
                base = tDyn(); // unknown type: dynamic semantics
        }
    }
    TP t = base;
    // a decayed array parameter's rows: T (*x)[M]
    for (size_t i = tn->pointeeDims.size(); i-- > 0;)
    {
        long n = tn->pointeeDims[i] ? (long)evalConst(tn->pointeeDims[i], sc) : -1;
        if (t->k == T::Char && i + 1 == tn->pointeeDims.size())
        {
            t = tBuf(n); // char rows are char buffers
            continue;
        }
        auto a = mkT(T::Array, "carray");
        a->a.push_back(t);
        a->n = n;
        t = a;
    }
    for (int i = 0; i < tn->pointers; ++i)
    {
        if (t->k == T::Char && i == 0)
        {
            t = tCStr(); // char* / const char*: strings
            continue;
        }
        auto p = mkT(T::Ptr);
        p->a.push_back(t);
        t = p;
    }
    // arrays: T x[N][M] → array<array<T,M>,N>; char x[N][M]: N char buffers
    for (size_t i = tn->arrayDims.size(); i-- > 0;)
    {
        long n = tn->arrayDims[i] ? (long)evalConst(tn->arrayDims[i], sc) : -1;
        if (t->k == T::Char && i + 1 == tn->arrayDims.size())
        {
            t = tBuf(n);
            continue;
        }
        auto a = mkT(T::Array, "carray");
        a->a.push_back(t);
        a->n = n;
        t = a;
    }
    return t;
}

long long Lowerer::evalConst(const ExprP &e, Scope *sc)
{
    if (!e)
        return 0;
    switch (e->k)
    {
    case EK::Int: case EK::Char: case EK::Float: case EK::Bool:
        return (long long)e->num;
    case EK::Name:
    {
        Symbol *s = lookup(e->name, sc);
        if (s && s->kind == Symbol::Enumerator)
            return s->value;
        if (s && s->kind == Symbol::Var)
        {
            // constexpr / const integral globals
            for (auto &g : globalsInOrder_)
                if (mangle(g.second, g.first->var.name) == s->global && !g.first->var.args.empty())
                    return evalConst(g.first->var.args[0], g.second);
        }
        if (e->name.size() == 2 && e->name[0] == "std" && e->name[1] == "npos")
            return -1;
        return 0;
    }
    case EK::Unary:
    {
        long long v = evalConst(e->kids[0], sc);
        if (e->op == "-") return -v;
        if (e->op == "~") return ~v;
        if (e->op == "!") return !v;
        return v;
    }
    case EK::Binary:
    {
        long long a = evalConst(e->kids[0], sc), b = evalConst(e->kids[1], sc);
        const std::string &op = e->op;
        if (op == "+") return a + b;
        if (op == "-") return a - b;
        if (op == "*") return a * b;
        if (op == "/") return b ? a / b : 0;
        if (op == "%") return b ? a % b : 0;
        if (op == "<<") return a << b;
        if (op == ">>") return a >> b;
        if (op == "&") return a & b;
        if (op == "|") return a | b;
        if (op == "^") return a ^ b;
        if (op == "<") return a < b;
        if (op == ">") return a > b;
        if (op == "<=") return a <= b;
        if (op == ">=") return a >= b;
        if (op == "==") return a == b;
        if (op == "!=") return a != b;
        if (op == "&&") return a && b;
        if (op == "||") return a || b;
        return 0;
    }
    case EK::Cond:
        return evalConst(e->kids[0], sc) ? evalConst(e->kids[1], sc) : evalConst(e->kids[2], sc);
    case EK::Cast:
    case EK::Construct:
        return e->kids.empty() ? 0 : evalConst(e->kids[0], sc);
    case EK::SizeofT:
    {
        TP t = resolveType(e->type, sc);
        if (t->k == T::Char || t->k == T::Bool) return 1;
        if (t->k == T::Int) return t->name.find("long long") != std::string::npos ? 8 : t->name.find("short") != std::string::npos ? 2 : 4;
        if (t->k == T::Float) return t->name == "float" ? 4 : 8;
        return 8;
    }
    default:
        return 0;
    }
}

// ─── Classes and functions: resolution ─────────────────────────────────────

void Lowerer::resolveClass(ClassInfo *c)
{
    if (c->resolved || c->resolving || !c->decl)
        return;
    c->resolving = true;
    const DeclP &d = c->decl;
    line_ = d->line;
    for (const auto &bt : d->bases)
    {
        TP b = resolveType(bt, c->outer);
        if (b->k == T::Class && b->cls)
        {
            resolveClass(b->cls);
            c->bases.push_back(b->cls);
        }
    }
    int slot = 1;
    c->isUnion = d->isUnion;
    for (const auto &fr : d->friendNames)
        c->friends.insert(fr);
    int unionSlot = -1;
    if (!c->bases.empty())
    {
        slot = c->bases[0]->nslots;
        c->polymorphic = c->bases[0]->polymorphic;
        c->aggregate = false;
        for (size_t i = 1; i < c->bases.size(); ++i)
            slot += c->bases[i]->nslots - 1; // further bases' fields follow (multiple inheritance)
    }
    for (const auto &m : d->members)
    {
        line_ = m->line;
        if (m->k == DK::Var)
        {
            FieldInfo f;
            f.name = m->var.name;
            f.type = resolveType(m->var.type, c->scope);
            f.init = m->var;
            f.hasInit = m->var.init != VarDeclarator::None;
            f.decl = m;
            f.access = m->access;
            f.isConst = m->var.type && m->var.type->isConst && m->var.type->pointers == 0 && !m->var.type->ref;
            if (m->isStatic)
            {
                f.isStatic = true;
                f.global = c->qname + "::" + f.name;
                Symbol s;
                s.kind = Symbol::Var;
                s.type = f.type;
                s.global = f.global;
                c->scope->syms[f.name] = s;
            }
            else if (c->isUnion)
            {
                // a union's members are one value, read as any of them
                if (unionSlot < 0)
                    unionSlot = slot++;
                f.slot = unionSlot;
            }
            else
                f.slot = slot++;
            c->fields.push_back(f);
        }
        else if (m->k == DK::Func)
        {
            if (m->isFriend)
                continue;
            funcs_.push_back(std::make_unique<FuncInfo>());
            FuncInfo *f = funcs_.back().get();
            f->name = m->name.back();
            f->decl = m;
            f->cls = c;
            f->isStatic = m->isStatic;
            f->isVirtual = m->isVirtual;
            f->isConst = m->isConst;
            f->isCtor = m->isCtor;
            f->isDtor = m->isDtor;
            if (f->isDtor)
                c->dtor = f;
            f->isPure = m->isPure;
            f->access = m->access;
            f->isTemplate = m->isTemplate;
            f->scope = c->scope;
            f->tu = m->tu;
            if (m->body)
                f->def = m;
            if (f->isVirtual)
                c->polymorphic = true;
            if (f->isCtor)
            {
                if (!m->isDeleted)
                {
                    c->ctors.push_back(f);
                    c->hasUserCtor = true;
                    c->aggregate = false;
                }
            }
            else if (f->name.rfind("operator ", 0) == 0)
                c->convOps.push_back(f);
            if (!f->isCtor && !f->isDtor)
                c->methods.push_back(f);
            allFuncs_.push_back(f);
        }
        else if (m->k == DK::UsingDecl && m->inheritingCtor)
        {
            for (ClassInfo *b : c->bases)
                if (b->name == m->name.back())
                    c->inheritCtorsFrom.push_back(b);
            c->aggregate = false;
            c->hasUserCtor = true;
        }
    }
    if (c->polymorphic)
        c->aggregate = false;
    c->nslots = slot;
    c->resolved = true;
    c->resolving = false;
}

void Lowerer::resolveFunc(FuncInfo *f)
{
    const DeclP &d = f->decl;
    Scope *sc = f->scope;
    if (f->isTemplate && d)
        for (const auto &tp : d->templateParams)
        {
            Symbol s;
            s.kind = Symbol::Type;
            s.type = tDyn();
            // template parameters live in a scope of their own
            if (!sc->syms.count(tp))
                sc->syms[tp] = s;
        }
    line_ = d->line;
    f->ret = d->ret ? resolveType(d->ret, sc) : tVoid();
    f->retRef = d->ret && d->ret->ref && !d->ret->isConst && !isObjectRep(f->ret);
    f->variadic = d->variadic;
    f->params.clear();
    f->paramRef.clear();
    f->paramConstRef.clear();
    f->defaults.clear();
    for (const auto &p : d->params)
    {
        TP pt = resolveType(p.type, sc);
        f->params.push_back(pt);
        bool ref = (p.type->ref || p.type->rref) && !p.type->isConst;
        f->paramRef.push_back(ref);
        f->paramConstRef.push_back(p.type->ref && p.type->isConst);
        f->defaults.push_back(p.def);
    }
}

// ─── Run ───────────────────────────────────────────────────────────────────

static std::string sigKey(const FuncInfo *f)
{
    std::string k = f->name + "(";
    for (size_t i = 0; i < f->params.size(); ++i)
        k += (i ? "," : "") + f->params[i]->str() + (f->paramRef[i] ? "&" : "");
    return k + (f->isConst ? ")c" : ")");
}

// A class template used with its arguments (Matrix<int>): a class of its own,
// collected again from the template's declaration with the parameters bound
// (and `Matrix` inside it naming this instance). Each phase run() has already
// passed is applied to it here; the later ones reach it in run()'s loops.
TP Lowerer::instantiateClass(ClassInfo *tc, const std::vector<TArg> &targs, Scope *sc)
{
    auto asType = [](ClassInfo *c)
    {
        auto t = mkT(T::Class);
        t->cls = c;
        return t;
    };
    const DeclP &d = tc->decl;
    if (!d || d->templateParams.size() != targs.size())
        return asType(tc); // default arguments: the dynamic version
    std::vector<Symbol> binds;
    std::string key = tc->qname + "<";
    for (size_t i = 0; i < targs.size(); ++i)
    {
        Symbol s;
        if (targs[i].type)
        {
            TP t = resolveType(targs[i].type, sc);
            if (!t || t->k == T::Dyn || (t->k == T::Class && t->cls && t->cls->isTemplate))
                return asType(tc);
            s.kind = Symbol::Type;
            s.type = t;
            key += (i ? "," : "") + t->str();
        }
        else if (targs[i].expr)
        {
            s.kind = Symbol::Enumerator;
            s.type = tInt();
            s.value = evalConst(targs[i].expr, sc);
            key += (i ? "," : "") + std::to_string(s.value);
        }
        else
            return asType(tc);
        binds.push_back(s);
    }
    key += ">";
    auto known = classInstances_.find(key);
    if (known != classInstances_.end())
        return asType(known->second);
    std::string uniq = tc->name + key.substr(tc->qname.size());
    Scope *outer = tc->outer ? tc->outer : global_;
    size_t friendsBefore = funcDecls_.size();
    collectClass(d, outer, uniq);
    funcDecls_.resize(friendsBefore); // its friends are made below, for this instance
    Symbol *sym = lookupIn(outer, uniq, true);
    if (!sym || !sym->type || !sym->type->cls)
        return asType(tc);
    ClassInfo *c = sym->type->cls;
    classInstances_[key] = c;
    instanceOf_[c] = tc;
    c->name = tc->name; // its constructors carry the template's name
    c->isTemplate = false;
    for (size_t i = 0; i < binds.size(); ++i)
        c->scope->syms[d->templateParams[i]] = binds[i];
    Symbol self;
    self.kind = Symbol::Type;
    self.type = asType(c);
    c->scope->syms[tc->name] = self;
    int savedLine = line_;
    if (phase_ >= 1)
        resolveClass(c);
    // friend functions defined in the class: free functions of this instance
    for (const auto &m : d->members)
        if (m->k == DK::Func && m->isFriend && m->body && !m->name.empty())
        {
            funcs_.push_back(std::make_unique<FuncInfo>());
            FuncInfo *f = funcs_.back().get();
            f->name = m->name.back();
            f->decl = m;
            f->def = m;
            f->scope = c->scope;
            f->tu = m->tu;
            resolveFunc(f);
            Symbol &fs = outer->syms[f->name];
            if (fs.kind != Symbol::None && fs.kind != Symbol::Funcs)
                continue;
            fs.kind = Symbol::Funcs;
            f->mangled = mangle(outer, f->name) + "@" + uniq;
            fs.funcs.push_back(f);
            allFuncs_.push_back(f);
        }
    if (phase_ >= 2)
    {
        for (FuncInfo *f : c->methods)
            resolveFunc(f);
        for (FuncInfo *f : c->ctors)
            resolveFunc(f);
        if (c->dtor)
            resolveFunc(c->dtor);
    }
    if (phase_ >= 3)
        copyTemplateDefs(c);
    if (phase_ >= 4)
        mangleMethods(c);
    if (phase_ >= 5)
    {
        FnCtx *savedFn = fn_;
        auto savedPre = pre_;
        bool savedHoist = canHoist_;
        lowerClass(c);
        fn_ = savedFn;
        pre_ = savedPre;
        canHoist_ = savedHoist;
    }
    line_ = savedLine;
    return asType(c);
}

void Lowerer::copyTemplateDefs(ClassInfo *c)
{
    // members the template defines outside its class (template<class T> void S<T>::f() {...})
    auto it = instanceOf_.find(c);
    if (it == instanceOf_.end())
        return;
    ClassInfo *tc = it->second;
    for (FuncInfo *f : c->methods)
        if (!f->def)
            for (FuncInfo *g : tc->methods)
                if (g->decl == f->decl && g->def)
                    f->def = g->def;
    for (FuncInfo *f : c->ctors)
        if (!f->def)
            for (FuncInfo *g : tc->ctors)
                if (g->decl == f->decl && g->def)
                    f->def = g->def;
    if (c->dtor && !c->dtor->def && tc->dtor)
        c->dtor->def = tc->dtor->def;
}

void Lowerer::mangleMethods(ClassInfo *c)
{
    std::map<std::string, int> count;
    for (FuncInfo *f : c->methods)
    {
        int n = ++count[f->name];
        f->mangled = c->qname + "::" + f->name + (n > 1 ? "#" + std::to_string(n) : "");
        f->vkey = f->name + "/" + std::to_string(f->params.size());
        // methods overriding a base virtual are virtual too
        for (ClassInfo *b = c->bases.empty() ? nullptr : c->bases[0]; b && !f->isVirtual;
             b = b->bases.empty() ? nullptr : b->bases[0])
            for (FuncInfo *bm : b->methods)
                if (bm->isVirtual && bm->vkey == f->vkey)
                    f->isVirtual = true;
        if (f->isVirtual)
            c->polymorphic = true;
        Symbol &s = c->scope->syms[f->name];
        s.kind = Symbol::Funcs;
        s.funcs.push_back(f);
    }
    int n = 0;
    for (FuncInfo *f : c->ctors)
        f->mangled = c->qname + "::" + c->name + (++n > 1 ? "#" + std::to_string(n) : "");
}

ASTNodePtr Lowerer::run()
{
    collect(prog_.decls, global_);
    phase_ = 1;
    for (size_t i = 0; i < classOrder_.size(); ++i)
        resolveClass(classOrder_[i]);
    for (size_t i = 0; i < classOrder_.size(); ++i)
        for (FuncInfo *f : std::vector<FuncInfo *>(classOrder_[i]->methods))
            resolveFunc(f);
    for (size_t i = 0; i < classOrder_.size(); ++i)
    {
        for (FuncInfo *f : std::vector<FuncInfo *>(classOrder_[i]->ctors))
            resolveFunc(f);
        if (classOrder_[i]->dtor)
            resolveFunc(classOrder_[i]->dtor);
    }
    phase_ = 2;

    // free functions and out-of-class member definitions
    for (auto &fd : funcDecls_)
    {
        const Decl *d = fd.first;
        Scope *sc = fd.second;
        line_ = d->line;
        DeclP dp;
        // find the shared_ptr owning d (we stored raw pointers to keep order)
        dp = std::shared_ptr<Decl>(std::shared_ptr<Decl>{}, const_cast<Decl *>(d));
        if (d->name.size() > 1)
        {
            // Cls::f / ns::f / Outer::Inner::f
            std::vector<std::string> owner(d->name.begin(), d->name.end() - 1);
            Symbol *os = lookup(owner, sc);
            if (os && os->kind == Symbol::Type && os->type && os->type->k == T::Class)
            {
                ClassInfo *c = os->type->cls;
                FuncInfo probe;
                probe.name = d->name.back();
                probe.decl = dp;
                probe.scope = c->scope;
                probe.cls = c;
                probe.isConst = d->isConst;
                probe.isTemplate = d->isTemplate;
                resolveFunc(&probe);
                std::string key = sigKey(&probe);
                FuncInfo *match = nullptr;
                auto &list = d->isCtor ? c->ctors : c->methods;
                for (FuncInfo *m : list)
                    if (m->name == probe.name && sigKey(m) == key)
                        match = m;
                if (!match)
                    for (FuncInfo *m : list)
                        if (m->name == probe.name && m->params.size() == probe.params.size() && !m->def)
                            match = m;
                if (!match && d->isDtor)
                {
                    // Cls::~Cls() { ... } defined outside the class
                    if (c->dtor)
                    {
                        c->dtor->def = dp;
                        c->dtor->tu = d->tu;
                    }
                    continue;
                }
                if (!match)
                    unsupported("definition of " + c->qname + "::" + probe.name + " matches no declaration", d->line);
                match->def = dp;
                match->tu = d->tu;
                continue;
            }
            if (os && os->kind == Symbol::Namespace)
                sc = os->ns;
            else
                unsupported("unknown scope for " + d->name.back(), d->line);
        }
        funcs_.push_back(std::make_unique<FuncInfo>());
        FuncInfo *f = funcs_.back().get();
        f->name = d->name.back();
        f->decl = dp;
        f->scope = sc;
        f->isTemplate = d->isTemplate;
        f->tu = d->tu;
        resolveFunc(f);
        std::string key = sigKey(f);
        Symbol &sym = sc->syms[f->name];
        if (sym.kind != Symbol::Funcs && sym.kind != Symbol::None)
        {
            if (sym.kind == Symbol::Type)
                continue; // a function named like a type (rare)
        }
        sym.kind = Symbol::Funcs;
        FuncInfo *existing = nullptr;
        for (FuncInfo *g : sym.funcs)
            if (sigKey(g) == key)
                existing = g;
        if (!existing && prog_.cMode && !sym.funcs.empty())
            unsupported("conflicting types for '" + f->name + "'", d->line); // C has no overloading
        if (existing)
        {
            if (d->body)
            {
                existing->def = dp;
                existing->tu = d->tu;
            }
            // later declarations may add default arguments
            for (size_t i = 0; i < existing->defaults.size() && i < f->defaults.size(); ++i)
                if (!existing->defaults[i] && f->defaults[i])
                    existing->defaults[i] = f->defaults[i];
            funcs_.pop_back();
            continue;
        }
        if (d->body)
            f->def = dp;
        f->mangled = mangle(sc, f->name) + (sym.funcs.empty() ? "" : "#" + std::to_string(sym.funcs.size() + 1));
        sym.funcs.push_back(f);
        allFuncs_.push_back(f);
    }

    // class template instances made so far get the template's out-of-class members
    phase_ = 3;
    for (auto &inst : classInstances_)
        copyTemplateDefs(inst.second);

    // method names: Cls::m, overloads numbered
    for (size_t i = 0; i < classOrder_.size(); ++i)
        mangleMethods(classOrder_[i]);
    phase_ = 4;

    // Global variables' types, before any function body refers to them.
    for (auto &g : globalsInOrder_)
    {
        const DeclP &d = g.first;
        Scope *sc = g.second;
        if (d->name.size() > 1)
            continue; // Cls::member definitions: typed by the class
        line_ = d->line;
        Symbol &s = sc->syms[d->var.name];
        TP t = resolveType(d->var.type, sc);
        if (isCBuf(t) && t->n < 0)
            t->n = bufInitSize(d->var.args, d->var.init == VarDeclarator::Brace);
        if (t->k == T::Array && t->n < 0 && !d->var.args.empty())
            t->n = arrayInitSize(t, d->var.args, d->var.init == VarDeclarator::Brace);
        if (t->k == T::Dyn && d->var.type->isAuto() && !d->var.args.empty())
        {
            FnCtx ctx;
            ctx.scope = sc;
            fn_ = &ctx;
            pushScope();
            try
            {
                t = probeType(d->var.args[0]);
            }
            catch (Unsupported &)
            {
            }
            popScope();
            fn_ = nullptr;
        }
        s.type = t;
    }

    // Lower: classes (ctors, methods), free functions. Lowering can add
    // entries (classes declared inside function bodies): index loops.
    phase_ = 5;
    for (size_t i = 0; i < classOrder_.size(); ++i)
    {
        ClassInfo *c = classOrder_[i];
        if (!c->isTemplate)
        {
            lowerClass(c);
            continue;
        }
        // a class template once, with dynamic parameters (for uses that can't be
        // instantiated): what can't be lowered without its types is left out
        FnCtx *savedFn = fn_;
        auto savedPre = pre_;
        bool savedHoist = canHoist_;
        try
        {
            lowerClass(c);
        }
        catch (Unsupported &)
        {
            fn_ = savedFn;
            pre_ = savedPre;
            canHoist_ = savedHoist;
        }
    }
    for (size_t i = 0; i < allFuncs_.size(); ++i)
    {
        FuncInfo *f = allFuncs_[i];
        if (!f->cls && f->def && !f->lowered)
        {
            if (!f->isTemplate)
            {
                lowerFunction(f);
                continue;
            }
            // a function template's dynamic version (for calls that can't be
            // instantiated): if it can't be lowered without its types, a stub
            auto savedPre = pre_;
            bool savedHoist = canHoist_;
            FnCtx *savedFn = fn_;
            try
            {
                lowerFunction(f);
            }
            catch (Unsupported &u)
            {
                pre_ = savedPre;
                canHoist_ = savedHoist;
                fn_ = savedFn;
                std::vector<std::string> ps;
                for (size_t k = 0; k < f->def->params.size(); ++k)
                    ps.push_back("__arg" + std::to_string(k));
                std::vector<ASTNodePtr> body;
                body.push_back(std::make_unique<ASTNode>(
                    RaiseStmt{q::str("template " + f->name + " needs its template arguments: " + u.what(), f->def->line)},
                    f->def->line));
                topFns_.push_back(q::func(f->mangled, ps, q::block(std::move(body), f->def->line), f->def->line));
            }
        }
    }

    emitDescriptors();
    emitGlobals();

    if (!calledUndefined_.empty())
    {
        // C++ would stop at link time: say which definitions are missing
        std::string msg;
        int n = 0;
        for (FuncInfo *f : calledUndefined_)
        {
            if (n++ == 3)
            {
                msg += ", ...";
                break;
            }
            std::string name = f->cls ? f->cls->qname + "::" + f->name : mangle(f->scope, f->name);
            msg += (msg.empty() ? "" : ", ") + std::string("'") + name + "'";
        }
        throw LinkError("undefined reference to " + msg +
                        ". The definition was not found: for a header x.h, quantum compiles the x.cpp next to it, "
                        "or src/x.cpp under the project folder (the one holding CMakeLists.txt, Makefile, "
                        "package.json or .git).");
    }

    // main
    std::vector<ASTNodePtr> prog;
    for (auto &f : topFns_)
        prog.push_back(std::move(f));
    prog.push_back(q::exprStmt(q::call("__cpp_runtime__", {}, 0), 0));
    for (auto &p : planInits_)
        prog.push_back(std::move(p));
    for (auto &s : topInit_)
        prog.push_back(std::move(s));
    Symbol *mainSym = lookupIn(global_, "main", false);
    if (mainSym && mainSym->kind == Symbol::Funcs && !mainSym->funcs.empty())
    {
        FuncInfo *m = mainSym->funcs[0];
        std::vector<ASTNodePtr> args;
        if (m->params.size() >= 2)
        {
            args.push_back(q::call("__cpp_argc__", {}, 0));
            // char** argv: a pointer to the first of the argument strings
            std::vector<ASTNodePtr> ref;
            ref.push_back(q::call("__cpp_argv__", {}, 0));
            ref.push_back(q::num(0, 0));
            args.push_back(m->params[1]->k == T::Ptr ? q::arr(std::move(ref), 0) : q::call("__cpp_argv__", {}, 0));
        }
        // try { __rc = main(...) } catch (e) { __cpp_terminate__(e) }  __cpp_exit__(__rc)
        prog.push_back(q::var("__cpp_rc", q::num(0, 0), 0));
        std::vector<ASTNodePtr> tryBody;
        tryBody.push_back(q::exprStmt(q::assign(q::id("__cpp_rc", 0), q::call(m->mangled, std::move(args), 0), 0), 0));
        TryStmt ts;
        ts.body = std::shared_ptr<ASTNode>(q::block(std::move(tryBody), 0).release());
        ExceptClause h;
        h.alias = "__cpp_uncaught";
        std::vector<ASTNodePtr> hb;
        hb.push_back(q::exprStmt(q::call("__cpp_terminate__", [&]
                                         {
                                             std::vector<ASTNodePtr> a;
                                             a.push_back(q::id("__cpp_uncaught", 0));
                                             return a; }(),
                                         0),
                                 0));
        h.body = std::shared_ptr<ASTNode>(q::block(std::move(hb), 0).release());
        ts.handlers.push_back(std::move(h));
        prog.push_back(std::make_unique<ASTNode>(std::move(ts), 0));
        std::vector<ASTNodePtr> ea;
        ea.push_back(q::id("__cpp_rc", 0));
        prog.push_back(q::exprStmt(q::call("__cpp_exit__", std::move(ea), 0), 0));
    }
    return q::block(std::move(prog), 0);
}

// ─── Plans (copy recipes) and default values ───────────────────────────────

ASTNodePtr Lowerer::planFor(const TP &t)
{
    if (!needsCopy(t))
        return q::num(0, line_);
    std::string key = t->str();
    auto it = plans_.find(key);
    if (it != plans_.end())
        return q::id(it->second, line_);
    std::string name = "__cpp_plan" + std::to_string(plans_.size() + 1);
    plans_[key] = name;
    auto shallow = [&]
    { return q::num(0, 0); };
    auto sub = [&](const TP &x) -> ASTNodePtr
    { return needsCopy(x) ? planFor(x) : shallow(); };
    std::vector<ASTNodePtr> parts;
    switch (t->k)
    {
    case T::Vector:
    case T::Array:
        parts.push_back(q::num(1, 0));
        parts.push_back(sub(t->a.empty() ? tDyn() : t->a[0]));
        break;
    case T::Map:
        parts.push_back(q::num(2, 0));
        parts.push_back(sub(t->a[0]));
        parts.push_back(sub(t->a[1]));
        break;
    case T::Set:
        parts.push_back(q::num(7, 0));
        break;
    case T::Optional:
        parts.push_back(q::num(4, 0));
        parts.push_back(sub(t->a[0]));
        break;
    case T::Variant:
    {
        parts.push_back(q::num(5, 0));
        std::vector<ASTNodePtr> alts;
        for (auto &a : t->a)
            alts.push_back(sub(a));
        parts.push_back(q::arr(std::move(alts), 0));
        break;
    }
    case T::Pair:
    case T::Tuple:
    {
        parts.push_back(q::num(6, 0));
        std::vector<ASTNodePtr> el;
        for (auto &a : t->a)
            el.push_back(sub(a));
        parts.push_back(q::arr(std::move(el), 0));
        break;
    }
    case T::Class:
    {
        // the slots may refer back to this plan (a class holding vector<Self>)
        planInits_.push_back(q::var(name, q::arr({}, 0), 0));
        ClassInfo *c = t->cls;
        std::vector<ASTNodePtr> slots(c->nslots);
        for (auto &s : slots)
            s = q::num(0, 0);
        std::function<void(ClassInfo *, int)> fill = [&](ClassInfo *k, int offset)
        {
            (void)offset;
            for (ClassInfo *b : k->bases)
                fill(b, 0);
            bool first = true;
            for (auto &f : k->fields)
                if (!f.isStatic && f.slot < (int)slots.size() && (first || !k->isUnion))
                {
                    slots[f.slot] = sub(f.type);
                    first = false;
                }
        };
        fill(c, 0);
        std::vector<ASTNodePtr> args;
        args.push_back(q::id(name, 0));
        args.push_back(q::num(3, 0));
        args.push_back(q::arr(std::move(slots), 0));
        planInits_.push_back(q::exprStmt(q::call("__cpp_plan_set__", std::move(args), 0), 0));
        return q::id(name, line_);
    }
    default:
        return q::num(0, line_);
    }
    planInits_.push_back(q::var(name, q::arr(std::move(parts), 0), 0));
    return q::id(name, line_);
}

ASTNodePtr Lowerer::defaultValue(const TP &t, int line)
{
    switch (t->k)
    {
    case T::Int: case T::Char: case T::Float: case T::Enum:
        return q::num(0, line);
    case T::Bool:
        return q::boolean(false, line);
    case T::Str:
        return q::str("", line);
    case T::Vector:
        return q::arr({}, line);
    case T::Array:
    {
        if (t->n <= 0)
            return q::arr({}, line);
        TP el = t->a[0];
        std::vector<ASTNodePtr> args;
        args.push_back(q::num((double)t->n, line));
        args.push_back(defaultValue(el, line));
        args.push_back(planFor(el));
        return q::call("__cpp_fill__", std::move(args), line);
    }
    case T::Map:
    case T::Set:
        return q::call("__cpp_dict__", {}, line);
    case T::Pair:
    case T::Tuple:
    {
        std::vector<ASTNodePtr> el;
        for (auto &a : t->a)
            el.push_back(defaultValue(a, line));
        return q::arr(std::move(el), line);
    }
    case T::Variant:
    {
        std::vector<ASTNodePtr> el;
        el.push_back(q::num(0, line));
        el.push_back(t->a.empty() || t->a[0]->k == T::Monostate ? q::nil(line) : defaultValue(t->a[0], line));
        return q::arr(std::move(el), line);
    }
    case T::Class:
    {
        std::vector<ExprP> none;
        return ctorCall(t->cls, none, false, line);
    }
    case T::Stream:
    {
        std::vector<ASTNodePtr> a;
        a.push_back(q::str(t->name, line));
        return q::call("__cpp_stream_new__", std::move(a), line);
    }
    default:
        return q::nil(line);
    }
}

// ─── Function lowering ─────────────────────────────────────────────────────

Local *Lowerer::findLocal(const std::string &n)
{
    for (FnCtx *f = fn_; f; f = f->parent)
        for (auto it = f->scopes.rbegin(); it != f->scopes.rend(); ++it)
        {
            auto l = it->find(n);
            if (l != it->end())
                return &l->second;
        }
    return nullptr;
}

Local &Lowerer::declareLocal(const std::string &n, const TP &t)
{
    // unique Quantum names per function (closures see their parents' locals)
    FnCtx *root = fn_;
    while (root->parent)
        root = root->parent;
    int &c = root->nameCount[n];
    ++c;
    std::string qn = c == 1 ? n : n + "#" + std::to_string(c);
    Local l;
    l.q = qn;
    l.t = t;
    l.name = n;
    l.boxed = root->boxed.count(qn) > 0 || isCBuf(t); // char buffers: in a cell a char* can point into
    fn_->scopes.back()[n] = l;
    return fn_->scopes.back()[n];
}

void Lowerer::pushScope()
{
    fn_->scopes.emplace_back();
    fn_->owned.emplace_back();
}
void Lowerer::popScope()
{
    fn_->scopes.pop_back();
    if (!fn_->owned.empty())
        fn_->owned.pop_back();
}

// ─── Object lifetimes ──────────────────────────────────────────────────────
// A class needs destruction when it (or a base / member) has a destructor
// with a body, or owns smart pointers / containers of such objects. Locals
// that own something are destroyed when their scope ends (also on return /
// break / continue), in reverse order of declaration; `delete` dispatches on
// the object's dynamic class (its descriptor's "#dtor").

bool Lowerer::needsDtor(ClassInfo *c)
{
    if (!c || c->builtin)
        return false;
    auto it = dtorMemo_.find(c);
    if (it != dtorMemo_.end())
        return it->second > 0;
    dtorMemo_[c] = 0; // a class holding vector<Self> is settled by its other members
    bool need = c->dtor && c->dtor->def && c->dtor->def->body && !c->dtor->def->body->body.empty();
    for (ClassInfo *b : c->bases)
        need = need || needsDtor(b);
    for (const auto &f : c->fields)
        if (!f.isStatic && f.type)
            need = need || ownsResource(f.type);
    dtorMemo_[c] = need ? 1 : 0;
    return need;
}

bool Lowerer::ownsResource(const TP &t)
{
    if (!t)
        return false;
    switch (t->k)
    {
    case T::Class:
        return t->cls && needsDtor(t->cls);
    case T::UPtr:
        if (t->name == "shared_ptr")
            return true;
        return t->name == "unique_ptr" && !t->a.empty() && t->a[0] && (t->a[0]->k != T::Class || needsDtor(t->a[0]->cls));
    case T::Vector:
    case T::Array:
        return !t->a.empty() && ownsResource(t->a[0]);
    default:
        return false;
    }
}

ASTNodePtr Lowerer::destroyValue(const TP &t, ASTNodePtr v, int line)
{
    std::vector<ASTNodePtr> a;
    a.push_back(std::move(v));
    if (t->k == T::UPtr && t->name == "shared_ptr")
        return q::call("__cpp_sp_release__", std::move(a), line);
    if (t->k == T::Vector || t->k == T::Array)
    {
        // elements: a vector's in order, an array's last first
        a.push_back(q::boolean(t->k == T::Array, line));
        return q::call("__cpp_destroy_each__", std::move(a), line);
    }
    return q::call("__cpp_delete__", std::move(a), line);
}

bool Lowerer::anyOwned(size_t fromDepth)
{
    for (size_t i = fromDepth; i < fn_->owned.size(); ++i)
        if (!fn_->owned[i].empty())
            return true;
    return false;
}

void Lowerer::emitDestroy(size_t fromDepth, std::vector<ASTNodePtr> &out, const std::string &except)
{
    for (size_t i = fn_->owned.size(); i-- > fromDepth;)
        for (size_t j = fn_->owned[i].size(); j-- > 0;)
        {
            Local &l = fn_->owned[i][j];
            if (l.q == except)
                continue;
            XV x = localXV(&l);
            x.constLv = false;
            out.push_back(q::exprStmt(destroyValue(l.t, read(x), line_), line_));
        }
}

LoopCtx Lowerer::loopCtx()
{
    LoopCtx l;
    l.scopeDepth = fn_->scopes.size();
    return l;
}

FuncInfo *Lowerer::userCopyCtor(ClassInfo *c)
{
    for (FuncInfo *f : c->ctors)
        if (f->params.size() == 1 && f->params[0]->k == T::Class && f->params[0]->cls == c && f->def &&
            !(f->def->params.size() == 1 && f->def->params[0].type && f->def->params[0].type->rref))
            return f;
    return nullptr;
}

void Lowerer::lowerFunction(FuncInfo *f)
{
    if (f->lowered || !f->def || f->isTemplate && false)
        return;
    f->lowered = true;
    std::vector<std::string> params;
    ASTNodePtr body = lowerFunctionBody(f, params);
    topFns_.push_back(q::func(f->mangled, params, std::move(body), f->def->line));
}

ASTNodePtr Lowerer::lowerFunctionBody(FuncInfo *f, std::vector<std::string> &params)
{
    struct CurFn
    {
        FuncInfo *&slot, *saved;
        CurFn(FuncInfo *&s, FuncInfo *f) : slot(s), saved(s) { slot = f; }
        ~CurFn() { slot = saved; }
    } curFn(curFn_, f);
    const DeclP &d = f->def;
    std::set<std::string> boxed;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        FnCtx ctx;
        ctx.ret = f->ret;
        ctx.retRef = f->retRef;
        ctx.retIsRef = d->ret && (d->ret->ref || d->ret->rref);
        ctx.cls = f->cls;
        ctx.hasThis = f->cls && !f->isStatic;
        ctx.boxed = boxed;
        ctx.scope = f->scope;
        FnCtx *saved = fn_;
        fn_ = &ctx;
        pushScope();
        params.clear();
        std::vector<ASTNodePtr> stmts;
        if (ctx.hasThis)
            params.push_back("__this");
        for (size_t i = 0; i < d->params.size(); ++i)
        {
            std::string n = d->params[i].name.empty() ? "__arg" + std::to_string(i) : d->params[i].name;
            TP pt = i < f->params.size() ? f->params[i] : tDyn();
            Local &l = declareLocal(n, pt);
            if (const auto &ptn = d->params[i].type)
                l.isConst = ptn->isConst && ptn->pointers == 0;
            if (!(i < f->paramRef.size() && f->paramRef[i]) && !(i < f->paramConstRef.size() && f->paramConstRef[i]) &&
                !(d->params[i].type && (d->params[i].type->ref || d->params[i].type->rref)) && ownsResource(pt))
                fn_->owned.back().push_back(l);
            if (i < f->paramRef.size() && f->paramRef[i] && !isObjectRep(pt))
            {
                l.refSlot = true;
                l.boxed = false;
            }
            params.push_back(l.q);
            if (l.boxed)
                stmts.push_back(q::exprStmt(q::assign(q::id(l.q, d->line), q::arr([&]
                                                                                   {
                                                                                       std::vector<ASTNodePtr> e;
                                                                                       e.push_back(q::id(l.q, d->line));
                                                                                       return e; }(),
                                                                                   d->line),
                                                      d->line),
                                            d->line));
        }
        if (f->variadic && !f->isTemplate)
            params.push_back("*__va"); // the extra arguments, for va_start
        if (f->isCtor)
        {
            // base / member initialization, then the body
            ClassInfo *c = f->cls;
            std::set<std::string> initialized;
            bool delegated = false;
            for (const auto &mi : d->inits)
                if (mi.name == c->name)
                {
                    std::vector<ExprP> args = mi.args;
                    stmts.push_back(q::exprStmt(ctorCall(c, args, mi.braced, d->line, q::id("__this", d->line)), d->line));
                    delegated = true;
                }
            if (!delegated)
            {
                for (ClassInfo *b : c->bases)
                {
                    const MemInit *found = nullptr;
                    for (const auto &mi : d->inits)
                        if (mi.name == b->name)
                            found = &mi;
                    std::vector<ExprP> args = found ? found->args : std::vector<ExprP>();
                    stmts.push_back(q::exprStmt(ctorCall(b, args, found && found->braced, d->line, q::id("__this", d->line)), d->line));
                }
                for (const auto &fld : c->fields)
                {
                    if (fld.isStatic)
                        continue;
                    const MemInit *found = nullptr;
                    for (const auto &mi : d->inits)
                        if (mi.name == fld.name)
                            found = &mi;
                    std::vector<ASTNodePtr> pre;
                    auto savedPre = pre_;
                    pre_ = &pre;
                    canHoist_ = true;
                    ASTNodePtr value;
                    line_ = d->line;
                    if (found)
                    {
                        std::vector<ExprP> args = found->args;
                        value = read(*std::make_shared<XV>(makeObjectFrom(fld.type, args, found->braced, d->line)));
                    }
                    else if (fld.hasInit)
                    {
                        std::vector<ExprP> args = fld.init.args;
                        value = read(*std::make_shared<XV>(makeObjectFrom(
                            fld.type, args, fld.init.init == VarDeclarator::Brace, d->line)));
                    }
                    else
                        value = defaultValue(fld.type, d->line);
                    pre_ = savedPre;
                    for (auto &p : pre)
                        stmts.push_back(std::move(p));
                    stmts.push_back(q::exprStmt(q::assign(q::index(q::id("__this", d->line), q::num(fld.slot, d->line), d->line),
                                                          std::move(value), d->line),
                                                d->line));
                }
            }
        }
        if (d->body)
        {
            for (const auto &s : d->body->body)
            {
                ASTNodePtr ls = lowerStmt(s);
                if (ls)
                    stmts.push_back(std::move(ls));
            }
        }
        emitDestroy(0, stmts);
        if (f->isCtor)
            stmts.push_back(q::ret(q::id("__this", d->line), d->line));
        popScope();
        fn_ = saved;
        bool more = false;
        for (const auto &w : ctx.wantBox)
            if (!boxed.count(w))
            {
                boxed.insert(w);
                more = true;
            }
        if (!more)
            return q::block(std::move(stmts), d->line);
        tempCounter_ += 0;
    }
    unsupported("could not settle which variables need storage cells", d->line);
}

void Lowerer::lowerClass(ClassInfo *c)
{
    if (c->lowered || c->builtin || !c->decl)
        return;
    c->lowered = true;
    int ln = c->decl->line;
    // Implicit default construction: bases, then fields with their initializers.
    {
        FnCtx ctx;
        ctx.cls = c;
        ctx.hasThis = true;
        ctx.scope = c->scope;
        FnCtx *saved = fn_;
        fn_ = &ctx;
        pushScope();
        std::vector<ASTNodePtr> stmts;
        for (ClassInfo *b : c->bases)
        {
            std::vector<ExprP> none;
            stmts.push_back(q::exprStmt(ctorCall(b, none, false, ln, q::id("__this", ln)), ln));
        }
        bool unionDone = false;
        for (const auto &fld : c->fields)
        {
            if (fld.isStatic || (c->isUnion && unionDone))
                continue;
            unionDone = true;
            std::vector<ASTNodePtr> pre;
            auto savedPre = pre_;
            pre_ = &pre;
            canHoist_ = true;
            line_ = fld.init.line ? fld.init.line : ln;
            ASTNodePtr value;
            if (fld.hasInit)
            {
                std::vector<ExprP> args = fld.init.args;
                XV x = makeObjectFrom(fld.type, args, fld.init.init == VarDeclarator::Brace, line_);
                value = read(x);
            }
            else
                value = defaultValue(fld.type, ln);
            pre_ = savedPre;
            for (auto &p : pre)
                stmts.push_back(std::move(p));
            stmts.push_back(q::exprStmt(q::assign(q::index(q::id("__this", ln), q::num(fld.slot, ln), ln), std::move(value), ln), ln));
        }
        stmts.push_back(q::ret(q::id("__this", ln), ln));
        popScope();
        fn_ = saved;
        topFns_.push_back(q::func(c->defaultCtorFn(), {"__this"}, q::block(std::move(stmts), ln), ln));
    }
    // Aggregate initialization: fields in order (missing ones get their defaults at the call site)
    {
        std::vector<std::string> ps = {"__this"};
        std::vector<ASTNodePtr> stmts;
        int i = 0;
        for (const auto &fld : c->fields)
        {
            if (fld.isStatic || (c->isUnion && i > 0))
                continue;
            std::string p = "__f" + std::to_string(i++);
            ps.push_back(p);
            stmts.push_back(q::exprStmt(q::assign(q::index(q::id("__this", ln), q::num(fld.slot, ln), ln), q::id(p, ln), ln), ln));
        }
        stmts.push_back(q::ret(q::id("__this", ln), ln));
        topFns_.push_back(q::func(c->aggFn(), ps, q::block(std::move(stmts), ln), ln));
    }
    for (FuncInfo *f : c->ctors)
        if (f->def || f->decl->isDefault)
        {
            if (!f->def)
            {
                // T() = default: the implicit default construction
                std::vector<std::string> ps = {"__this"};
                std::vector<ASTNodePtr> st;
                std::vector<ASTNodePtr> a;
                a.push_back(q::id("__this", ln));
                st.push_back(q::ret(q::call(c->defaultCtorFn(), std::move(a), ln), ln));
                for (size_t i = 0; i < f->params.size(); ++i)
                    ps.push_back("__a" + std::to_string(i));
                topFns_.push_back(q::func(f->mangled, ps, q::block(std::move(st), ln), ln));
                f->lowered = true;
                continue;
            }
            lowerFunction(f);
        }
    for (FuncInfo *f : c->methods)
        if (f->def && !f->isTemplate)
            lowerFunction(f);
    for (FuncInfo *f : c->methods)
        if (f->def && f->isTemplate)
            lowerFunction(f);
    if (needsDtor(c))
    {
        std::vector<ASTNodePtr> st;
        if (c->dtor && c->dtor->def)
        {
            c->dtor->mangled = c->qname + "::~" + c->name;
            lowerFunction(c->dtor);
            std::vector<ASTNodePtr> a;
            a.push_back(q::id("__this", ln));
            st.push_back(q::exprStmt(q::call(c->dtor->mangled, std::move(a), ln), ln));
        }
        for (size_t i = c->fields.size(); i-- > 0;)
        {
            const FieldInfo &f = c->fields[i];
            if (!f.isStatic && f.type && ownsResource(f.type))
                st.push_back(q::exprStmt(destroyValue(f.type, q::index(q::id("__this", ln), q::num(f.slot, ln), ln), ln), ln));
            if (c->isUnion)
                break;
        }
        for (size_t i = c->bases.size(); i-- > 0;)
            if (needsDtor(c->bases[i]))
            {
                std::vector<ASTNodePtr> a;
                a.push_back(q::id("__this", ln));
                st.push_back(q::exprStmt(q::call(c->bases[i]->dtorFn(), std::move(a), ln), ln));
            }
        topFns_.push_back(q::func(c->dtorFn(), {"__this"}, q::block(std::move(st), ln), ln));
    }
}

void Lowerer::emitDescriptors()
{
    for (ClassInfo *c : classOrder_)
    {
        if (c->builtin)
            continue;
        int ln = c->decl ? c->decl->line : 0;
        std::vector<ASTNodePtr> bases;
        std::function<void(ClassInfo *)> add = [&](ClassInfo *k)
        {
            bases.push_back(q::str(k->qname, ln));
            for (ClassInfo *b : k->bases)
                add(b);
        };
        add(c);
        std::vector<ASTNodePtr> args;
        args.push_back(q::str(c->qname, ln));
        args.push_back(q::arr(std::move(bases), ln));
        args.push_back(c->bases.empty() ? q::nil(ln) : q::id(c->bases[0]->desc(), ln));
        topInit_.push_back(q::var(c->desc(), q::call("__cpp_class__", std::move(args), ln), ln));
        if (needsDtor(c))
        {
            std::vector<ASTNodePtr> a;
            a.push_back(q::id(c->desc(), ln));
            a.push_back(q::str("#dtor", ln));
            a.push_back(q::id(c->dtorFn(), ln));
            topInit_.push_back(q::exprStmt(q::call("__cpp_vt_set__", std::move(a), ln), ln));
        }
        // virtual table: own methods override the base's entries
        for (FuncInfo *f : c->methods)
            if (f->isVirtual && f->def && !f->isPure)
            {
                std::vector<ASTNodePtr> a;
                a.push_back(q::id(c->desc(), ln));
                a.push_back(q::str(f->vkey, ln));
                a.push_back(q::id(f->mangled, ln));
                topInit_.push_back(q::exprStmt(q::call("__cpp_vt_set__", std::move(a), ln), ln));
            }
    }
}

void Lowerer::emitGlobals()
{
    for (auto &g : globalsInOrder_)
    {
        const DeclP &d = g.first;
        Scope *sc = g.second;
        line_ = d->line;
        FnCtx ctx;
        ctx.scope = sc;
        FnCtx *saved = fn_;
        fn_ = &ctx;
        pushScope();
        std::vector<ASTNodePtr> pre;
        auto savedPre = pre_;
        pre_ = &pre;
        canHoist_ = true;
        std::string gname;
        TP t;
        if (d->name.size() > 1)
        {
            std::vector<std::string> owner(d->name.begin(), d->name.end() - 1);
            Symbol *os = lookup(owner, sc);
            if (!os || os->kind != Symbol::Type || !os->type || os->type->k != T::Class)
                unsupported("unknown class for " + d->name.back(), d->line);
            ClassInfo *c = os->type->cls;
            ctx.cls = c;
            ctx.scope = c->scope;
            Symbol *fs = lookupIn(c->scope, d->name.back(), false);
            if (!fs || fs->kind != Symbol::Var)
                unsupported("unknown static member " + d->name.back(), d->line);
            gname = fs->global;
            t = fs->type;
        }
        else
        {
            Symbol &s = sc->syms[d->var.name];
            t = s.type ? s.type : resolveType(d->var.type, sc);
            gname = s.global;
        }
        std::vector<ExprP> args = d->var.args;
        ASTNodePtr value;
        if (d->var.init == VarDeclarator::None)
            value = defaultValue(t, d->line);
        else
        {
            XV x = makeObjectFrom(t, args, d->var.init == VarDeclarator::Brace, d->line);
            value = copyOf(std::move(x), d->line);
        }
        pre_ = savedPre;
        for (auto &p : pre)
            topInit_.push_back(std::move(p));
        if (isCBuf(t))
        {
            std::vector<ASTNodePtr> cell;
            cell.push_back(std::move(value));
            value = q::arr(std::move(cell), d->line);
        }
        topInit_.push_back(q::var(gname, std::move(value), d->line));
        popScope();
        fn_ = saved;
    }
    // static data members declared in a class without an out-of-class definition
    for (ClassInfo *c : classOrder_)
        for (auto &f : c->fields)
        {
            if (!f.isStatic)
                continue;
            bool defined = false;
            for (auto &g : globalsInOrder_)
                if (g.first->name.size() > 1 && g.first->name.back() == f.name)
                    defined = true;
            if (defined)
                continue;
            FnCtx ctx;
            ctx.cls = c;
            ctx.scope = c->scope;
            FnCtx *saved = fn_;
            fn_ = &ctx;
            pushScope();
            std::vector<ASTNodePtr> pre;
            auto savedPre = pre_;
            pre_ = &pre;
            canHoist_ = true;
            ASTNodePtr value;
            if (f.hasInit)
            {
                std::vector<ExprP> args = f.init.args;
                value = copyOf(makeObjectFrom(f.type, args, f.init.init == VarDeclarator::Brace, f.init.line), f.init.line);
            }
            else
                value = defaultValue(f.type, 0);
            if (isCBuf(f.type))
            {
                std::vector<ASTNodePtr> cell;
                cell.push_back(std::move(value));
                value = q::arr(std::move(cell), 0);
            }
            pre_ = savedPre;
            for (auto &p : pre)
                topInit_.push_back(std::move(p));
            // initialized before the other globals may use it
            topInit_.insert(topInit_.begin(), q::var(f.global, std::move(value), 0));
            popScope();
            fn_ = saved;
        }
}

// ─── Statements ────────────────────────────────────────────────────────────

ASTNodePtr Lowerer::withPre(ASTNodePtr stmt, std::vector<ASTNodePtr> &pre)
{
    if (pre.empty())
        return stmt;
    int ln = stmt ? stmt->line : line_;
    if (stmt)
        pre.push_back(std::move(stmt));
    return q::block(std::move(pre), ln);
}

ASTNodePtr Lowerer::lowerBlock(const std::vector<StmtP> &body, bool newScope)
{
    if (newScope)
        pushScope();
    std::vector<ASTNodePtr> out;
    for (const auto &s : body)
    {
        if (s->k == SK::Decl)
        {
            lowerDecl(*s, out);
            continue;
        }
        if (s->k == SK::Block && s->isStatic)
        {
            // declarations that don't open a scope (struct X {...} x;)
            for (const auto &x : s->body)
            {
                if (x->k == SK::Decl)
                    lowerDecl(*x, out);
                else if (auto ls = lowerStmt(x))
                    out.push_back(std::move(ls));
            }
            continue;
        }
        if (auto ls = lowerStmt(s))
            out.push_back(std::move(ls));
    }
    if (newScope)
    {
        emitDestroy(fn_->scopes.size() - 1, out);
        popScope();
    }
    return q::block(std::move(out), body.empty() ? line_ : body[0]->line);
}

void Lowerer::lowerDecl(const Stmt &s, std::vector<ASTNodePtr> &out)
{
    for (const auto &v : s.decls)
    {
        int savedTemps = tempsTracked_;
        tempsTracked_ = s.isStatic ? -1 : 0;
        std::vector<ASTNodePtr> one;
        lowerVar(v, s.isStatic, one);
        if (tempsTracked_ > 0)
        {
            // the declaration's other temporaries end with it (the variables stay in scope)
            std::string mark = tempName();
            out.push_back(q::var(mark, q::call("__cpp_temps_mark__", {}, s.line), s.line));
            for (auto &x : one)
                out.push_back(std::move(x));
            std::vector<ASTNodePtr> a;
            a.push_back(q::id(mark, s.line));
            out.push_back(q::exprStmt(q::call("__cpp_temps_end__", std::move(a), s.line), s.line));
        }
        else
            for (auto &x : one)
                out.push_back(std::move(x));
        tempsTracked_ = savedTemps;
    }
}

void Lowerer::lowerVar(const VarDeclarator &v, bool isStatic, std::vector<ASTNodePtr> &out)
{
    line_ = v.line;
    std::vector<ASTNodePtr> pre;
    auto savedPre = pre_;
    pre_ = &pre;
    canHoist_ = true;
    int ln = v.line;
    TypeNameP tn = v.type;
    TP t = resolveType(tn, fn_->scope);
    std::vector<ExprP> args = v.args;
    if (tn->isAuto() && tn->pointers > 0 && !args.empty())
        t = tDyn(); // auto *p = e: e's (pointer) type
    if (isCBuf(t) && t->n < 0)
        t->n = bufInitSize(args, v.init == VarDeclarator::Brace);
    if (t->k == T::Array && t->n < 0 && !args.empty())
        t->n = arrayInitSize(t, args, v.init == VarDeclarator::Brace);
    if (!v.name.empty() && !fn_->scopes.empty() && fn_->scopes.back().count(v.name))
        unsupported(prog_.cMode ? "redefinition of '" + v.name + "'"
                                : "redeclaration of '" + diagType(t) + " " + v.name + "'",
                    ln);
    const bool constVar = (tn->isConst && tn->pointers == 0) || (!tn->constPtr.empty() && tn->constPtr.back());

    // structured binding: auto [a, b] = e;  auto& [k, v] = e;
    if (!v.bindings.empty())
    {
        if (args.empty())
            unsupported("structured binding without initializer", ln);
        XV src = lowerExpr(args[0]);
        TP st = src.t;
        bool byRef = tn->ref || tn->rref;
        std::string tmp = tempName();
        ASTNodePtr init = byRef ? read(src) : copyOf(std::move(src), ln);
        pre_ = savedPre;
        for (auto &p : pre)
            out.push_back(std::move(p));
        out.push_back(q::var(tmp, std::move(init), ln));
        for (size_t i = 0; i < v.bindings.size(); ++i)
        {
            TP et = tDyn();
            if ((st->k == T::Pair || st->k == T::Tuple || st->k == T::Array) && i < st->a.size())
                et = st->a[i];
            else if (st->k == T::Array && !st->a.empty())
                et = st->a[0];
            else if (st->k == T::Class && st->cls)
            {
                int k = 0;
                for (auto &f : st->cls->fields)
                    if (!f.isStatic && k++ == (int)i)
                        et = f.type;
            }
            int key = (int)i;
            if (st->k == T::Class && st->cls)
            {
                int k = 0;
                for (auto &f : st->cls->fields)
                    if (!f.isStatic && k++ == (int)i)
                        key = f.slot;
            }
            Local &l = declareLocal(v.bindings[i], et);
            l.alias = true;
            auto a = std::make_shared<XV>();
            a->t = et;
            a->lk = XV::Idx;
            a->obj = q::id(tmp, ln);
            a->key = q::num(key, ln);
            l.aliasOf = a;
        }
        return;
    }

    if (t->k == T::Dyn && tn->isAuto())
    {
        // auto: the initializer's type
        if (args.empty())
            unsupported("auto without initializer", ln);
        if (v.init == VarDeclarator::Brace && args.size() != 1)
        {
            auto il = mkT(T::Vector, "vector");
            il->a.push_back(tDyn());
            t = il;
        }
    }

    // static local: a global initialized once
    if (isStatic)
    {
        std::string g = "__static" + std::to_string(++staticCounter_) + "_" + v.name;
        std::string guard = g + "#init";
        ASTNodePtr value;
        XV x = (v.init == VarDeclarator::None) ? mkxv(defaultValue(t, ln), t) : makeObjectFrom(t, args, v.init == VarDeclarator::Brace, ln);
        if (t->k == T::Dyn)
            t = x.t;
        value = copyOf(std::move(x), ln);
        std::vector<ASTNodePtr> thenStmts;
        for (auto &p : pre)
            thenStmts.push_back(std::move(p));
        if (isCBuf(t))
        {
            std::vector<ASTNodePtr> cell;
            cell.push_back(std::move(value));
            value = q::arr(std::move(cell), ln);
        }
        thenStmts.push_back(q::exprStmt(q::assign(q::id(g, ln), std::move(value), ln), ln));
        thenStmts.push_back(q::exprStmt(q::assign(q::id(guard, ln), q::boolean(true, ln), ln), ln));
        pre_ = savedPre;
        // the guard and the value are globals, initialized on first execution
        topInit_.insert(topInit_.begin(), q::var(guard, q::boolean(false, ln), ln));
        topInit_.insert(topInit_.begin(), q::var(g, q::nil(ln), ln));
        out.push_back(q::ifs(q::un("!", q::id(guard, ln), ln), q::block(std::move(thenStmts), ln), nullptr, ln));
        Local &l = declareLocal(v.name, t);
        l.isConst = constVar;
        l.alias = true;
        auto a = std::make_shared<XV>();
        a->t = t;
        a->lk = XV::Var;
        a->var = g;
        if (isCBuf(t))
        {
            a->lk = XV::Idx;
            a->obj = q::id(g, ln);
            a->key = q::num(0, ln);
        }
        l.aliasOf = a;
        return;
    }

    // references: T& r = lvalue
    if (tn->ref || tn->rref)
    {
        if (args.empty())
            unsupported("reference without initializer", ln);
        XV x = lowerExpr(args[0]);
        if (t->k == T::Dyn)
            t = x.t;
        if (isObjectRep(t) || x.t->k == T::Dyn || tn->isConst || x.lk == XV::RV)
        {
            // objects are held by reference already; const T& of a scalar: its value
            ASTNodePtr value = isObjectRep(t) ? read(x) : read(*std::make_shared<XV>(convert(std::move(x), t, ln)));
            pre_ = savedPre;
            for (auto &p : pre)
                out.push_back(std::move(p));
            Local &l = declareLocal(v.name, t);
            l.isConst = constVar;
            if (l.boxed)
            {
                std::vector<ASTNodePtr> e;
                e.push_back(std::move(value));
                value = q::arr(std::move(e), ln);
            }
            out.push_back(q::var(l.q, std::move(value), ln));
            return;
        }
        // a modifiable alias of a scalar lvalue: re-evaluable parts
        auto a = std::make_shared<XV>();
        a->t = t;
        a->lk = x.lk;
        a->var = x.var;
        if (x.lk == XV::Idx)
        {
            a->obj = simple(std::move(x.obj), ln);
            a->key = simple(std::move(x.key), ln);
        }
        else if (x.lk == XV::StrCh)
        {
            a->base = x.base;
            a->key = simple(std::move(x.key), ln);
        }
        pre_ = savedPre;
        for (auto &p : pre)
            out.push_back(std::move(p));
        Local &l = declareLocal(v.name, t);
        l.isConst = constVar;
        l.alias = true;
        l.aliasOf = a;
        return;
    }

    if (t->k == T::Func && !tn->isAuto() && v.init != VarDeclarator::None)
    {
        // std::function<...> f = [&](...) { ... f(...) ... }: the name is in
        // scope in its own initializer, so a recursive lambda can call it
        for (auto &p : pre)
            out.push_back(std::move(p));
        pre.clear();
        {
            Local &l = declareLocal(v.name, t);
            l.isConst = constVar;
            std::vector<ASTNodePtr> cell;
            cell.push_back(q::nil(ln));
            out.push_back(q::var(l.q, l.boxed ? q::arr(std::move(cell), ln) : q::nil(ln), ln));
        }
        XV x = makeObjectFrom(t, args, v.init == VarDeclarator::Brace, ln);
        ASTNodePtr value = copyOf(std::move(x), ln);
        pre_ = savedPre;
        for (auto &p : pre)
            out.push_back(std::move(p));
        XV target = localXV(findLocal(v.name));
        target.constLv = false;
        out.push_back(q::exprStmt(store(target, std::move(value), ln), ln));
        return;
    }
    ASTNodePtr value;
    if (v.init == VarDeclarator::None)
        value = defaultValue(t, ln);
    else
    {
        XV x = makeObjectFrom(t, args, v.init == VarDeclarator::Brace, ln);
        untrackTemp(x);
        if (t->k == T::Dyn)
            t = x.t;
        value = copyOf(std::move(x), ln);
    }
    pre_ = savedPre;
    for (auto &p : pre)
        out.push_back(std::move(p));
    Local &l = declareLocal(v.name, t);
    l.isConst = constVar;
    if (l.boxed)
    {
        std::vector<ASTNodePtr> e;
        e.push_back(std::move(value));
        value = q::arr(std::move(e), ln);
    }
    out.push_back(q::var(l.q, std::move(value), ln));
    if (!fn_->owned.empty() && ownsResource(t))
        fn_->owned.back().push_back(l); // destroyed when the scope ends
}

ASTNodePtr Lowerer::lowerStmt(const StmtP &s)
{
    int savedTemps = tempsTracked_;
    std::string savedMark = tempsMark_;
    bool tracks = s->k == SK::Expr || s->k == SK::Return;
    tempsTracked_ = tracks ? 0 : -1;
    tempsMark_ = tracks ? tempName() : std::string();
    ASTNodePtr r = lowerStmtInner(s);
    if (tracks && tempsTracked_ > 0)
    {
        if (s->k == SK::Return)
        {
            // the return ends them itself, before returning
            std::vector<ASTNodePtr> st;
            st.push_back(q::var(tempsMark_, q::call("__cpp_temps_mark__", {}, s->line), s->line));
            st.push_back(std::move(r));
            r = q::block(std::move(st), s->line);
        }
        else
            r = endTemps(std::move(r), tempsMark_, s->line);
    }
    tempsTracked_ = savedTemps;
    tempsMark_ = savedMark;
    return r;
}

ASTNodePtr Lowerer::lowerStmtInner(const StmtP &s)
{
    line_ = s->line;
    int ln = s->line;
    std::vector<ASTNodePtr> pre;
    auto savedPre = pre_;
    pre_ = &pre;
    canHoist_ = true;
    ASTNodePtr out;
    switch (s->k)
    {
    case SK::Empty:
        break;
    case SK::Expr:
    {
        if (s->expr->k == EK::Throw)
        {
            const ExprP &e = s->expr;
            if (e->kids.empty())
            {
                if (fn_->excVars.empty())
                    unsupported("throw; outside a handler", ln);
                out = std::make_unique<ASTNode>(RaiseStmt{q::id(fn_->excVars.back(), ln)}, ln);
            }
            else
            {
                XV x = lowerExpr(e->kids[0]);
                untrackTemp(x);
                ASTNodePtr val;
                if (x.t->k == T::Class)
                    val = copyOf(std::move(x), ln);
                else
                {
                    std::vector<ASTNodePtr> a;
                    a.push_back(q::str(x.t->k == T::CStr ? "const char*" : x.t->str(), ln));
                    a.push_back(read(x));
                    val = q::call("__cpp_boxexc__", std::move(a), ln);
                }
                out = std::make_unique<ASTNode>(RaiseStmt{std::move(val)}, ln);
            }
            break;
        }
        // postfix ++/-- as a statement: no old value needed
        ExprP e = s->expr;
        if (e->k == EK::Postfix)
        {
            auto pe = std::make_shared<Expr>(*e);
            pe->k = EK::Unary;
            e = pe;
        }
        XV x = lowerExpr(e);
        out = q::exprStmt(read(x), ln);
        break;
    }
    case SK::Decl:
    {
        std::vector<ASTNodePtr> decls;
        pre_ = savedPre;
        lowerDecl(*s, decls);
        return q::block(std::move(decls), ln);
    }
    case SK::Block:
        pre_ = savedPre;
        return lowerBlock(s->body, !s->isStatic);
    case SK::If:
        pre_ = savedPre;
        return lowerIf(*s);
    case SK::While:
    {
        pre_ = savedPre;
        pushScope();
        fn_->loops.push_back(loopCtx());
        std::vector<ASTNodePtr> outer;
        ASTNodePtr loop;
        if (s->condDecl)
        {
            // while (T x = e) body  →  while (true) { T x = e; if (!x) break; body }
            std::vector<ASTNodePtr> inner;
            pushScope();
            lowerDecl(*s->condDecl, inner);
            const auto &dv = s->condDecl->decls[0];
            Local *l = findLocal(dv.name);
            XV cx = lowerExpr([&]
                              {
                                  auto e = std::make_shared<Expr>();
                                  e->k = EK::Name;
                                  e->line = ln;
                                  e->name = {dv.name};
                                  return e; }());
            (void)l;
            inner.push_back(q::ifs(q::un("!", toBool(std::move(cx), ln), ln), q::brk(ln), nullptr, ln));
            inner.push_back(lowerStmt(s->then));
            popScope();
            loop = q::whiles(q::boolean(true, ln), q::block(std::move(inner), ln), nullptr, ln);
        }
        else
        {
            std::vector<ASTNodePtr> cpre;
            pre_ = &cpre;
            canHoist_ = false;
            XV c = lowerExpr(s->expr);
            ASTNodePtr cond = toBool(std::move(c), ln);
            pre_ = savedPre;
            ASTNodePtr body = lowerStmt(s->then);
            loop = q::whiles(std::move(cond), std::move(body), nullptr, ln);
        }
        fn_->loops.pop_back();
        popScope();
        return loop;
    }
    case SK::Do:
    {
        pre_ = savedPre;
        fn_->loops.push_back(loopCtx());
        ASTNodePtr body = lowerStmt(s->then);
        std::vector<ASTNodePtr> cpre;
        pre_ = &cpre;
        canHoist_ = false;
        XV c = lowerExpr(s->expr);
        ASTNodePtr cond = toBool(std::move(c), ln);
        pre_ = savedPre;
        fn_->loops.pop_back();
        ASTNodePtr post = q::ifs(q::un("!", std::move(cond), ln), q::brk(ln), nullptr, ln);
        return q::whiles(q::boolean(true, ln), std::move(body), std::move(post), ln);
    }
    case SK::For:
        pre_ = savedPre;
        return lowerFor(*s);
    case SK::RangeFor:
        pre_ = savedPre;
        return lowerRangeFor(*s);
    case SK::Switch:
        pre_ = savedPre;
        return lowerSwitch(*s);
    case SK::Break:
        if (!fn_->loops.empty() && anyOwned(fn_->loops.back().scopeDepth))
        {
            std::vector<ASTNodePtr> st;
            emitDestroy(fn_->loops.back().scopeDepth, st);
            st.push_back(q::brk(ln));
            out = q::block(std::move(st), ln);
            break;
        }
        out = q::brk(ln);
        break;
    case SK::Continue:
    {
        // inside a switch (a one-pass loop here), `continue` belongs to the loop around it
        int depth = (int)fn_->loops.size() - 1;
        if (depth >= 0 && fn_->loops[depth].isSwitch)
        {
            std::vector<ASTNodePtr> st;
            for (int i = depth; i >= 0 && fn_->loops[i].isSwitch; --i)
            {
                if (fn_->loops[i].contFlag.empty())
                    fn_->loops[i].contFlag = tempName();
            }
            st.push_back(q::exprStmt(q::assign(q::id(fn_->loops[depth].contFlag, ln), q::boolean(true, ln), ln), ln));
            st.push_back(q::brk(ln));
            out = q::block(std::move(st), ln);
        }
        else if (!fn_->loops.empty() && anyOwned(fn_->loops.back().scopeDepth))
        {
            std::vector<ASTNodePtr> st;
            emitDestroy(fn_->loops.back().scopeDepth, st);
            st.push_back(q::cont(ln));
            out = q::block(std::move(st), ln);
        }
        else
            out = q::cont(ln);
        break;
    }
    case SK::Return:
    {
        if (!s->expr)
        {
            if (!fn_->isLambda && fn_->ret && fn_->ret->k != T::Void && fn_->ret->k != T::Dyn)
                unsupported(prog_.cMode ? std::string("'return' with no value, in function returning non-void")
                                        : "return-statement with no value, in function returning '" + diagType(fn_->ret) +
                                              "' [-fpermissive]",
                            ln);
            if (anyOwned(0))
            {
                std::vector<ASTNodePtr> st;
                emitDestroy(0, st);
                st.push_back(q::ret(nullptr, ln));
                out = q::block(std::move(st), ln);
                break;
            }
            out = q::ret(nullptr, ln);
            break;
        }
        TP rt = fn_->isLambda && fn_->ret->k == T::Dyn ? tDyn() : fn_->ret;
        XV x;
        if (s->expr->k == EK::InitList && rt->k != T::Dyn)
        {
            std::vector<ExprP> kids = s->expr->kids;
            x = listInit(rt, kids, ln);
        }
        else
            x = lowerExpr(s->expr);
        untrackTemp(x);
        if (fn_->isLambda && !fn_->deducedRet)
            fn_->deducedRet = x.t;
        if (!fn_->isLambda && fn_->ret && fn_->ret->k == T::Void && x.t->k != T::Void && x.t->k != T::Dyn)
            unsupported(prog_.cMode ? std::string("'return' with a value, in function returning void")
                                    : std::string("return-statement with a value, in function returning 'void' [-fpermissive]"),
                        ln);
        if (fn_->retRef)
        {
            // returns T& of a scalar: a Ref to the lvalue
            XV r = addressOf(std::move(x), ln);
            out = q::ret(read(r), ln);
            break;
        }
        if (rt->k != T::Dyn && rt->k != T::Void)
            x = convert(std::move(x), rt, ln);
        // returning a local by value moves it; members / references are copied
        bool local = false;
        if (x.lk == XV::Var)
            for (auto &sc : fn_->scopes)
                for (auto &kv : sc)
                    if (kv.second.q == x.var && !kv.second.alias)
                        local = true;
        std::string returnedLocal = local ? x.var : std::string();
        ASTNodePtr val = (local || fn_->retIsRef) ? read(x) : copyOf(std::move(x), ln);
        if (anyOwned(0) || tempsTracked_ > 0)
        {
            std::string tmp = tempName();
            std::vector<ASTNodePtr> st;
            st.push_back(q::var(tmp, std::move(val), ln));
            if (tempsTracked_ > 0)
            {
                std::vector<ASTNodePtr> a;
                a.push_back(q::id(tempsMark_, ln));
                st.push_back(q::exprStmt(q::call("__cpp_temps_end__", std::move(a), ln), ln));
            }
            emitDestroy(0, st, returnedLocal);
            st.push_back(q::ret(q::id(tmp, ln), ln));
            out = q::block(std::move(st), ln);
            break;
        }
        out = q::ret(std::move(val), ln);
        break;
    }
    case SK::Try:
        pre_ = savedPre;
        return lowerTry(*s);
    case SK::Case:
    case SK::Default:
        unsupported("case label outside a switch", ln);
    case SK::LocalClass:
        pre_ = savedPre;
        if (s->local)
            localClass(s->local);
        return nullptr;
    }
    pre_ = savedPre;
    return withPre(std::move(out), pre);
}

ASTNodePtr Lowerer::lowerIf(const Stmt &s)
{
    int ln = s.line;
    pushScope();
    std::vector<ASTNodePtr> outer;
    if (s.init)
    {
        if (s.init->k == SK::Decl)
            lowerDecl(*s.init, outer);
        else if (auto x = lowerStmt(s.init))
            outer.push_back(std::move(x));
    }
    std::vector<ASTNodePtr> pre;
    auto savedPre = pre_;
    pre_ = &pre;
    canHoist_ = true;
    ASTNodePtr cond;
    if (s.condDecl)
    {
        lowerDecl(*s.condDecl, outer);
        const auto &dv = s.condDecl->decls[0];
        auto e = std::make_shared<Expr>();
        e->k = EK::Name;
        e->line = ln;
        e->name = {dv.name};
        cond = toBool(lowerExpr(e), ln);
    }
    else
        cond = toBool(lowerExpr(s.expr), ln);
    pre_ = savedPre;
    for (auto &p : pre)
        outer.push_back(std::move(p));
    ASTNodePtr then = lowerStmt(s.then);
    ASTNodePtr els = s.els ? lowerStmt(s.els) : nullptr;
    outer.push_back(q::ifs(std::move(cond), then ? std::move(then) : q::block({}, ln), std::move(els), ln));
    popScope();
    if (outer.size() == 1)
        return std::move(outer[0]);
    return q::block(std::move(outer), ln);
}

ASTNodePtr Lowerer::lowerFor(const Stmt &s)
{
    int ln = s.line;
    pushScope();
    std::vector<ASTNodePtr> outer;
    if (s.init)
    {
        if (s.init->k == SK::Decl)
            lowerDecl(*s.init, outer);
        else if (auto x = lowerStmt(s.init))
            outer.push_back(std::move(x));
    }
    std::vector<ASTNodePtr> cpre;
    auto savedPre = pre_;
    pre_ = &cpre;
    canHoist_ = false;
    ASTNodePtr cond = s.cond2 ? toBool(lowerExpr(s.cond2), ln) : q::boolean(true, ln);
    ASTNodePtr step;
    if (s.step)
    {
        ExprP st = s.step;
        if (st->k == EK::Postfix)
        {
            auto pe = std::make_shared<Expr>(*st);
            pe->k = EK::Unary;
            st = pe;
        }
        XV x = lowerExpr(st);
        step = q::exprStmt(read(x), ln);
    }
    pre_ = savedPre;
    fn_->loops.push_back(loopCtx());
    ASTNodePtr body = lowerStmt(s.then);
    fn_->loops.pop_back();
    outer.push_back(q::whiles(std::move(cond), body ? std::move(body) : q::block({}, ln), std::move(step), ln));
    emitDestroy(fn_->scopes.size() - 1, outer);
    popScope();
    return q::block(std::move(outer), ln);
}

ASTNodePtr Lowerer::lowerRangeFor(const Stmt &s)
{
    int ln = s.line;
    pushScope();
    std::vector<ASTNodePtr> outer;
    std::vector<ASTNodePtr> pre;
    auto savedPre = pre_;
    pre_ = &pre;
    canHoist_ = true;
    const VarDeclarator &v = s.decls[0];
    TypeNameP tn = v.type;
    TP declT = resolveType(tn, fn_->scope);
    XV c;
    TP ct;
    if (s.expr->k == EK::InitList)
    {
        std::vector<ExprP> kids = s.expr->kids;
        auto lt = mkT(T::Vector, "vector");
        XV first = kids.empty() ? mkxv(q::nil(ln), tDyn()) : lowerExpr(kids[0]);
        lt->a.push_back(first.t->k == T::CStr ? tCStr() : first.t);
        c = listInit(lt, kids, ln);
        ct = lt;
    }
    else
    {
        c = lowerExpr(s.expr);
        ct = c.t;
    }
    std::string cn = tempName(), in = tempName();
    bool strLv = ct->k == T::Str && c.lk != XV::RV;
    std::shared_ptr<XV> strBase;
    ASTNodePtr init;
    if (strLv)
    {
        strBase = std::make_shared<XV>();
        strBase->t = ct;
        strBase->lk = c.lk;
        strBase->var = c.var;
        if (c.lk == XV::Idx)
        {
            strBase->obj = simple(q::clone(c.obj), ln);
            strBase->key = simple(q::clone(c.key), ln);
        }
        init = read(c);
    }
    else if (ct->k == T::Map || ct->k == T::Set)
    {
        std::vector<ASTNodePtr> a;
        a.push_back(read(c));
        a.push_back(q::boolean(ct->sorted, ln));
        init = q::call(ct->k == T::Map ? "__cpp_map_items__" : "__cpp_set_items__", std::move(a), ln);
    }
    else
        init = read(c);
    pre_ = savedPre;
    for (auto &p : pre)
        outer.push_back(std::move(p));
    outer.push_back(q::var(cn, std::move(init), ln));
    outer.push_back(q::var(in, q::num(0, ln), ln));
    TP et = ct->k == T::Map ? [&]
    {
        auto p = mkT(T::Pair);
        p->a = ct->a;
        return p;
    }()
                           : ct->k == T::Str ? tChar() : elemType(ct);
    if (declT->k == T::Dyn && !(tn->isAuto() && false))
        declT = et;
    // the element as an lvalue
    auto elem = std::make_shared<XV>();
    elem->t = et;
    if (ct->k == T::Str)
    {
        elem->lk = strLv ? XV::StrCh : XV::RV;
        if (strLv)
        {
            elem->base = strBase;
            elem->key = q::id(in, ln);
        }
        std::vector<ASTNodePtr> a;
        a.push_back(q::id(cn, ln));
        a.push_back(q::id(in, ln));
        elem->v = q::call("__cpp_str_at__", std::move(a), ln);
    }
    else
    {
        elem->lk = XV::Idx;
        elem->obj = q::id(cn, ln);
        elem->key = q::id(in, ln);
    }
    pushScope();
    std::vector<ASTNodePtr> body;
    std::vector<ASTNodePtr> bpre;
    pre_ = &bpre;
    canHoist_ = true;
    bool byRef = tn->ref || tn->rref;
    if (!v.bindings.empty())
    {
        std::string en = tempName();
        body.push_back(q::var(en, read(*elem), ln));
        for (size_t i = 0; i < v.bindings.size(); ++i)
        {
            TP bt = (et->k == T::Pair || et->k == T::Tuple) && i < et->a.size() ? et->a[i] : tDyn();
            int key = (int)i;
            if (et->k == T::Class && et->cls)
            {
                int k = 0;
                for (auto &f : et->cls->fields)
                    if (!f.isStatic && k++ == (int)i)
                    {
                        key = f.slot;
                        bt = f.type;
                    }
            }
            Local &l = declareLocal(v.bindings[i], bt);
            if (byRef)
            {
                l.alias = true;
                auto a = std::make_shared<XV>();
                a->t = bt;
                a->lk = XV::Idx;
                a->obj = q::id(en, ln);
                a->key = q::num(key, ln);
                l.aliasOf = a;
            }
            else
            {
                XV x;
                x.t = bt;
                x.lk = XV::Idx;
                x.obj = q::id(en, ln);
                x.key = q::num(key, ln);
                body.push_back(q::var(l.q, copyOf(std::move(x), ln), ln));
            }
        }
    }
    else if (byRef && !isObjectRep(declT) && elem->lk != XV::RV && !tn->isConst)
    {
        Local &l = declareLocal(v.name, declT);
        l.alias = true;
        l.aliasOf = elem;
    }
    else
    {
        Local &l = declareLocal(v.name, declT);
        XV x;
        x.t = elem->t;
        x.lk = elem->lk == XV::StrCh ? XV::RV : elem->lk;
        if (elem->lk == XV::StrCh || ct->k == T::Str)
            x.v = q::clone(elem->v);
        x.obj = q::clone(elem->obj);
        x.key = q::clone(elem->key);
        x = convert(std::move(x), declT, ln);
        ASTNodePtr val = (byRef || isObjectRep(declT) && byRef) ? read(x) : copyOf(std::move(x), ln);
        if (l.boxed)
        {
            std::vector<ASTNodePtr> e;
            e.push_back(std::move(val));
            val = q::arr(std::move(e), ln);
        }
        body.push_back(q::var(l.q, std::move(val), ln));
    }
    pre_ = savedPre;
    for (auto &p : bpre)
        body.insert(body.begin(), std::move(p));
    fn_->loops.push_back(loopCtx());
    if (ASTNodePtr b = lowerStmt(s.then))
        body.push_back(std::move(b));
    fn_->loops.pop_back();
    popScope();
    std::vector<ASTNodePtr> sz;
    sz.push_back(q::id(cn, ln));
    ASTNodePtr cond = q::bin("<", q::id(in, ln), q::call("__cpp_size__", std::move(sz), ln), ln);
    ASTNodePtr step = q::exprStmt(q::assign(q::id(in, ln), q::bin("+", q::id(in, ln), q::num(1, ln), ln), ln), ln);
    outer.push_back(q::whiles(std::move(cond), q::block(std::move(body), ln), std::move(step), ln));
    popScope();
    return q::block(std::move(outer), ln);
}

ASTNodePtr Lowerer::lowerSwitch(const Stmt &s)
{
    int ln = s.line;
    pushScope();
    std::vector<ASTNodePtr> outer;
    if (s.init)
    {
        if (s.init->k == SK::Decl)
            lowerDecl(*s.init, outer);
        else if (auto x = lowerStmt(s.init))
            outer.push_back(std::move(x));
    }
    std::vector<ASTNodePtr> pre;
    auto savedPre = pre_;
    pre_ = &pre;
    canHoist_ = true;
    XV v = lowerExpr(s.expr);
    std::string sv = tempName(), st = tempName();
    ASTNodePtr val = read(v);
    // sections: statements between labels
    struct Section
    {
        std::vector<ExprP> labels;
        bool isDefault = false;
        std::vector<StmtP> stmts;
    };
    std::vector<Section> secs;
    for (const auto &x : s.body)
    {
        if (x->k == SK::Case || x->k == SK::Default)
        {
            if (secs.empty() || !secs.back().stmts.empty())
                secs.emplace_back();
            if (x->k == SK::Case)
                secs.back().labels.push_back(x->expr);
            else
                secs.back().isDefault = true;
            continue;
        }
        if (secs.empty())
            secs.emplace_back();
        secs.back().stmts.push_back(x);
    }
    // __st = (sv == a) ? 0 : (sv == b) ? 1 : ... : default / N
    int defIdx = (int)secs.size();
    for (size_t i = 0; i < secs.size(); ++i)
        if (secs[i].isDefault)
            defIdx = (int)i;
    ASTNodePtr sel = q::num(defIdx, ln);
    for (size_t i = secs.size(); i-- > 0;)
        for (size_t j = secs[i].labels.size(); j-- > 0;)
        {
            XV lv = lowerExpr(secs[i].labels[j]);
            sel = q::ternary(q::bin("==", q::id(sv, ln), read(lv), ln), q::num((double)i, ln), std::move(sel), ln);
        }
    pre_ = savedPre;
    for (auto &p : pre)
        outer.push_back(std::move(p));
    outer.push_back(q::var(sv, std::move(val), ln));
    outer.push_back(q::var(st, std::move(sel), ln));
    LoopCtx lc;
    lc.isSwitch = true;
    lc.scopeDepth = fn_->scopes.size();
    fn_->loops.push_back(lc);
    std::vector<ASTNodePtr> body;
    pushScope(); // one scope for the whole switch body, as in C++
    for (size_t i = 0; i < secs.size(); ++i)
    {
        std::vector<ASTNodePtr> sb;
        for (const auto &x : secs[i].stmts)
        {
            if (x->k == SK::Decl)
            {
                lowerDecl(*x, body); // declarations stay visible to later sections
                continue;
            }
            if (auto ls = lowerStmt(x))
                sb.push_back(std::move(ls));
        }
        body.push_back(q::ifs(q::bin("<=", q::id(st, ln), q::num((double)i, ln), ln), q::block(std::move(sb), ln), nullptr, ln));
    }
    popScope();
    body.push_back(q::brk(ln));
    std::string flag = fn_->loops.back().contFlag;
    fn_->loops.pop_back();
    if (!flag.empty())
        outer.push_back(q::var(flag, q::boolean(false, ln), ln));
    outer.push_back(q::whiles(q::boolean(true, ln), q::block(std::move(body), ln), nullptr, ln));
    if (!flag.empty())
    {
        // `continue` inside the switch: continue the enclosing loop (or the
        // enclosing switch's loop, which re-dispatches the flag)
        bool outerIsSwitch = !fn_->loops.empty() && fn_->loops.back().isSwitch;
        ASTNodePtr act;
        if (outerIsSwitch)
        {
            if (fn_->loops.back().contFlag.empty())
                fn_->loops.back().contFlag = tempName();
            std::vector<ASTNodePtr> st2;
            st2.push_back(q::exprStmt(q::assign(q::id(fn_->loops.back().contFlag, ln), q::boolean(true, ln), ln), ln));
            st2.push_back(q::brk(ln));
            act = q::block(std::move(st2), ln);
        }
        else
            act = q::cont(ln);
        outer.push_back(q::ifs(q::id(flag, ln), std::move(act), nullptr, ln));
    }
    popScope();
    return q::block(std::move(outer), ln);
}

// The VM unwinds a caught exception's stack to its depth at the try, while
// the compiler numbers the handler's locals after every local declared in
// the try body: those are declared before the try instead (`var x = e`
// inside becomes `x = e`), so the slots agree however far the body got.
static void hoistTryLocals(ASTNode &n, std::vector<std::string> &names)
{
    if (auto *b = std::get_if<BlockStmt>(&n.node))
    {
        for (auto &st : b->statements)
            if (st)
                hoistTryLocals(*st, names);
        return;
    }
    if (auto *v = std::get_if<VarDecl>(&n.node))
    {
        std::string name = v->name;
        names.push_back(name);
        ASTNodePtr init = v->initializer ? std::move(v->initializer) : q::nil(n.line);
        n.node = ExprStmt{q::assign(q::id(name, n.line), std::move(init), n.line)};
        return;
    }
    if (auto *i = std::get_if<IfStmt>(&n.node))
    {
        if (i->thenBranch)
            hoistTryLocals(*i->thenBranch, names);
        if (i->elseBranch)
            hoistTryLocals(*i->elseBranch, names);
        return;
    }
    if (auto *w = std::get_if<WhileStmt>(&n.node))
    {
        if (w->body)
            hoistTryLocals(*w->body, names);
        if (w->post)
            hoistTryLocals(*w->post, names);
        return;
    }
    if (auto *t = std::get_if<TryStmt>(&n.node))
    {
        if (t->body)
            hoistTryLocals(*t->body, names);
        for (auto &h : t->handlers)
            if (h.body)
                hoistTryLocals(*h.body, names);
    }
}

ASTNodePtr Lowerer::lowerTry(const Stmt &s)
{
    int ln = s.line;
    TryStmt ts;
    ts.body = std::shared_ptr<ASTNode>(lowerStmt(s.then).release());
    std::vector<std::string> hoisted;
    if (ts.body)
        hoistTryLocals(*ts.body, hoisted);
    std::string ex = tempName();
    ExceptClause h;
    h.alias = ex;
    // if-chain over the catch clauses; no match → rethrow
    ASTNodePtr chain = std::make_unique<ASTNode>(RaiseStmt{q::id(ex, ln)}, ln);
    for (size_t i = s.catches.size(); i-- > 0;)
    {
        const Catch &c = s.catches[i];
        pushScope();
        fn_->excVars.push_back(ex);
        std::vector<ASTNodePtr> body;
        ASTNodePtr cond;
        if (!c.type)
            cond = q::boolean(true, ln);
        else
        {
            TP ct = resolveType(c.type, fn_->scope);
            std::string tname;
            if (ct->k == T::Class && ct->cls)
                tname = ct->cls->qname;
            else
                tname = ct->k == T::CStr ? "const char*" : ct->str();
            std::vector<ASTNodePtr> a;
            a.push_back(q::id(ex, ln));
            a.push_back(q::str(tname, ln));
            cond = q::call("__cpp_isa__", std::move(a), ln);
            if (!c.name.empty())
            {
                Local &l = declareLocal(c.name, ct);
                ASTNodePtr val = ct->k == T::Class ? q::id(ex, ln) : q::index(q::id(ex, ln), q::num(1, ln), ln);
                if (l.boxed)
                {
                    std::vector<ASTNodePtr> e;
                    e.push_back(std::move(val));
                    val = q::arr(std::move(e), ln);
                }
                body.push_back(q::var(l.q, std::move(val), ln));
            }
        }
        if (ASTNodePtr b = lowerStmt(c.body))
            body.push_back(std::move(b));
        fn_->excVars.pop_back();
        popScope();
        chain = q::ifs(std::move(cond), q::block(std::move(body), ln), std::move(chain), ln);
    }
    std::vector<ASTNodePtr> hb;
    hb.push_back(std::move(chain));
    h.body = std::shared_ptr<ASTNode>(q::block(std::move(hb), ln).release());
    ts.handlers.push_back(std::move(h));
    if (hoisted.empty())
        return std::make_unique<ASTNode>(std::move(ts), ln);
    std::vector<ASTNodePtr> out;
    for (const auto &n : hoisted)
        out.push_back(q::var(n, q::nil(ln), ln));
    out.push_back(std::make_unique<ASTNode>(std::move(ts), ln));
    return q::block(std::move(out), ln);
}

void Lowerer::localClass(const DeclP &d)
{
    // A class / enum / alias declared inside a function: hoisted to the
    // namespace level under a unique name, visible from this block.
    if (d->k == DK::Class)
    {
        std::string uniq = d->name.back() + "#local" + std::to_string(++localClassCounter_);
        Scope *sc = fn_->scope;
        collectClass(d, sc, uniq);
        Symbol *s = lookupIn(sc, uniq, true);
        ClassInfo *c = s->type->cls;
        c->name = d->name.back();
        resolveClass(c);
        for (FuncInfo *f : c->methods)
            resolveFunc(f);
        for (FuncInfo *f : c->ctors)
            resolveFunc(f);
        std::map<std::string, int> count;
        for (FuncInfo *f : c->methods)
        {
            int n = ++count[f->name];
            f->mangled = c->qname + "::" + f->name + (n > 1 ? "#" + std::to_string(n) : "");
            f->vkey = f->name + "/" + std::to_string(f->params.size());
            Symbol &ms = c->scope->syms[f->name];
            ms.kind = Symbol::Funcs;
            ms.funcs.push_back(f);
        }
        int n = 0;
        for (FuncInfo *f : c->ctors)
            f->mangled = c->qname + "::" + c->name + (++n > 1 ? "#" + std::to_string(n) : "");
        // visible under its own name inside this function
        Symbol ts;
        ts.kind = Symbol::Type;
        ts.type = s->type;
        fn_->scopes.back()["#type:" + d->name.back()];
        localTypes_[d->name.back()] = s->type;
        FnCtx *saved = fn_;
        auto savedPre = pre_;
        bool savedHoist = canHoist_;
        lowerClass(c);
        fn_ = saved;
        pre_ = savedPre;
        canHoist_ = savedHoist;
        return; // its descriptor is created with the others, before main runs
    }
    if (d->k == DK::Enum)
    {
        collectEnum(d, fn_->scope);
        return;
    }
    if (d->k == DK::Alias)
    {
        localTypes_[d->name[0]] = resolveType(d->aliased, fn_->scope);
        return;
    }
    if (d->k == DK::UsingDirective)
    {
        Symbol *s = lookup(d->name, fn_->scope);
        if (s && s->kind == Symbol::Namespace)
            fn_->scope->usings.push_back(s->ns);
    }
}

ASTNodePtr compileProgram(const std::string &path, const std::string &source)
{
    Preprocessed pp = preprocess(path, source);
    if (std::getenv("QUANTUM_DEBUG_CPPFE"))
    {
        for (const auto &f : pp.files)
            std::cerr << "[cppfe] file " << f << std::endl;
        if (std::getenv("QUANTUM_DEBUG_CPPTOKENS"))
            for (const auto &t : pp.tokens)
                std::cerr << t.file << ":" << t.line << " " << t.text << std::endl;
    }
    Program prog = parse(pp);
    Lowerer lw(prog);
    try
    {
        return lw.run();
    }
    catch (Unsupported &u)
    {
        if (u.file.empty() && !pp.files.empty())
            u.file = "?";
        throw;
    }
}

} // namespace cpp
