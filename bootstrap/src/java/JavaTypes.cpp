// Java types: construction, printing, substitution, erasure, subtyping and
// conversions, and resolving type names written in the source (JLS §6.5.5).
#include "JavaSema.h"

#include <algorithm>

namespace java
{

namespace
{
TypeP mkPrim(TK k)
{
    auto t = std::make_shared<Type>();
    t->k = k;
    return t;
}
} // namespace

TypeP Sema::prim(TK k) const
{
    static const TypeP table[] = {mkPrim(TK::Void),  mkPrim(TK::Boolean), mkPrim(TK::Byte),   mkPrim(TK::Short),
                                  mkPrim(TK::Char),  mkPrim(TK::Int),     mkPrim(TK::Long),   mkPrim(TK::Float),
                                  mkPrim(TK::Double), mkPrim(TK::Null),   nullptr,            nullptr,
                                  nullptr,           nullptr,             mkPrim(TK::Error)};
    return table[(int)k];
}

TypeP Sema::tError() const { return prim(TK::Error); }
TypeP Sema::tNull() const { return prim(TK::Null); }

TypeP Sema::tObject()
{
    if (!tObject_)
        tObject_ = classType(cls("java.lang.Object"));
    return tObject_;
}

TypeP Sema::tString()
{
    if (!tString_)
        tString_ = classType(cls("java.lang.String"));
    return tString_;
}

ClassSym *Sema::cls(const std::string &qualName)
{
    auto it = byQual_.find(qualName);
    return it == byQual_.end() ? nullptr : it->second;
}

TypeP Sema::classType(ClassSym *c, std::vector<TypeP> args)
{
    if (!c)
        return tError();
    if (args.empty())
    {
        auto key = std::make_pair(c, std::string());
        auto it = typeCache_.find(key);
        if (it != typeCache_.end())
            return it->second;
        auto t = std::make_shared<Type>();
        t->k = TK::Class;
        t->cls = c;
        typeCache_[key] = t;
        return t;
    }
    auto t = std::make_shared<Type>();
    t->k = TK::Class;
    t->cls = c;
    t->args = std::move(args);
    return t;
}

TypeP Sema::arrayOf(const TypeP &elem)
{
    auto t = std::make_shared<Type>();
    t->k = TK::Array;
    t->elem = elem;
    return t;
}

TypeP Sema::typeVar(TypeVar *tv)
{
    auto t = std::make_shared<Type>();
    t->k = TK::TypeVar;
    t->tv = tv;
    return t;
}

TypeP Sema::selfType(ClassSym *c)
{
    std::vector<TypeP> args;
    for (auto *tv : c->tparams)
        args.push_back(typeVar(tv));
    return classType(c, args);
}

bool Sema::isPrim(const TypeP &t)
{
    return t && t->k >= TK::Boolean && t->k <= TK::Double;
}

bool Sema::isNumeric(const TypeP &t)
{
    return t && t->k >= TK::Byte && t->k <= TK::Double;
}

bool Sema::isIntegral(const TypeP &t)
{
    return t && t->k >= TK::Byte && t->k <= TK::Long;
}

bool Sema::isRef(const TypeP &t)
{
    return t && (t->k == TK::Class || t->k == TK::Array || t->k == TK::TypeVar || t->k == TK::Null ||
                 t->k == TK::Wildcard);
}

bool Sema::isStringType(const TypeP &t)
{
    return t && t->k == TK::Class && t->cls && t->cls->binaryName == "java.lang.String";
}

bool Sema::isBooleanish(const TypeP &t)
{
    if (!t)
        return false;
    if (t->k == TK::Boolean || t->k == TK::Error)
        return true;
    TypeP u = unboxed(t);
    return u && u->k == TK::Boolean;
}

TypeP Sema::boxOf(const TypeP &p)
{
    const char *n = nullptr;
    switch (p ? p->k : TK::Error)
    {
    case TK::Boolean: n = "java.lang.Boolean"; break;
    case TK::Byte: n = "java.lang.Byte"; break;
    case TK::Short: n = "java.lang.Short"; break;
    case TK::Char: n = "java.lang.Character"; break;
    case TK::Int: n = "java.lang.Integer"; break;
    case TK::Long: n = "java.lang.Long"; break;
    case TK::Float: n = "java.lang.Float"; break;
    case TK::Double: n = "java.lang.Double"; break;
    default: return p;
    }
    return classType(cls(n));
}

TypeP Sema::unboxed(const TypeP &t0)
{
    TypeP t = t0;
    if (t && (t->k == TK::TypeVar || t->k == TK::Wildcard))
        t = upperBound(t);
    if (!t || t->k != TK::Class || !t->cls)
        return nullptr;
    const std::string &n = t->cls->binaryName;
    if (n == "java.lang.Integer") return prim(TK::Int);
    if (n == "java.lang.Long") return prim(TK::Long);
    if (n == "java.lang.Double") return prim(TK::Double);
    if (n == "java.lang.Boolean") return prim(TK::Boolean);
    if (n == "java.lang.Character") return prim(TK::Char);
    if (n == "java.lang.Float") return prim(TK::Float);
    if (n == "java.lang.Short") return prim(TK::Short);
    if (n == "java.lang.Byte") return prim(TK::Byte);
    return nullptr;
}

TypeP Sema::numericUnboxed(const TypeP &t)
{
    if (isNumeric(t))
        return t;
    TypeP u = unboxed(t);
    return u && isNumeric(u) ? u : nullptr;
}

std::string Sema::typeStr(const TypeP &t) const
{
    if (!t)
        return "<any>";
    switch (t->k)
    {
    case TK::Void: return "void";
    case TK::Boolean: return "boolean";
    case TK::Byte: return "byte";
    case TK::Short: return "short";
    case TK::Char: return "char";
    case TK::Int: return "int";
    case TK::Long: return "long";
    case TK::Float: return "float";
    case TK::Double: return "double";
    case TK::Null: return "<null>";
    case TK::Error: return "<any>";
    case TK::Array: return typeStr(t->elem) + "[]";
    case TK::TypeVar: return t->tv ? t->tv->name : "?";
    case TK::Wildcard:
        return t->wild == 0 ? "?" : std::string(t->wild == 1 ? "? extends " : "? super ") + typeStr(t->elem);
    case TK::Class:
    {
        std::string s;
        // nested types print as Outer.Inner (javac)
        std::vector<std::string> names;
        for (ClassSym *c = t->cls; c; c = c->outer)
        {
            names.push_back(c->isAnon ? "<anonymous " + (c->ifaces.empty() ? typeStr(c->super) : typeStr(c->ifaces[0])) + ">"
                                      : c->name);
            if (c->isLocal || c->isAnon)
                break;
        }
        for (size_t i = names.size(); i-- > 0;)
            s += names[i] + (i ? "." : "");
        if (!t->args.empty())
        {
            s += "<";
            for (size_t i = 0; i < t->args.size(); ++i)
                s += (i ? "," : "") + typeStr(t->args[i]);
            s += ">";
        }
        return s;
    }
    }
    return "?";
}

std::string Sema::sigStr(const MethodSym *m) const
{
    std::string s = (m->isCtor ? m->owner->name : m->name) + "(";
    for (size_t i = 0; i < m->params.size(); ++i)
    {
        std::string p = typeStr(m->params[i]);
        if (m->varargs && i + 1 == m->params.size() && p.size() > 2)
            p = p.substr(0, p.size() - 2) + "...";
        s += (i ? "," : "") + p;
    }
    return s + ")";
}

TypeP Sema::subst(const TypeP &t, const Subst &s)
{
    if (!t || s.empty())
        return t;
    switch (t->k)
    {
    case TK::TypeVar:
    {
        auto it = s.find(t->tv);
        return it != s.end() && it->second ? it->second : t;
    }
    case TK::Array:
    {
        TypeP e = subst(t->elem, s);
        return e == t->elem ? t : arrayOf(e);
    }
    case TK::Wildcard:
    {
        if (!t->elem)
            return t;
        TypeP b = subst(t->elem, s);
        if (b == t->elem)
            return t;
        auto w = std::make_shared<Type>(*t);
        w->elem = b;
        return w;
    }
    case TK::Class:
    {
        if (t->args.empty())
            return t;
        std::vector<TypeP> a;
        bool changed = false;
        for (auto &x : t->args)
        {
            a.push_back(subst(x, s));
            changed = changed || a.back() != x;
        }
        return changed ? classType(t->cls, std::move(a)) : t;
    }
    default:
        return t;
    }
}

TypeP Sema::erasure(const TypeP &t)
{
    if (!t)
        return tObject();
    switch (t->k)
    {
    case TK::Class:
        return t->args.empty() ? t : classType(t->cls);
    case TK::Array:
        return arrayOf(erasure(t->elem));
    case TK::TypeVar:
        return t->tv && !t->tv->bounds.empty() ? erasure(t->tv->bounds[0]) : tObject();
    case TK::Wildcard:
        return t->wild == 1 && t->elem ? erasure(t->elem) : tObject();
    default:
        return t;
    }
}

std::string Sema::erasedName(const TypeP &t)
{
    TypeP e = erasure(t);
    if (e->k == TK::Array)
        return erasedName(e->elem) + "[]";
    if (e->k == TK::Class)
        return e->cls->name.empty() ? e->cls->binaryName : e->cls->name;
    return typeStr(e);
}

Subst Sema::substOf(const TypeP &ct)
{
    Subst s;
    if (!ct || ct->k != TK::Class || !ct->cls)
        return s;
    const auto &tps = ct->cls->tparams;
    if (ct->args.size() == tps.size())
    {
        for (size_t i = 0; i < tps.size(); ++i)
        {
            TypeP a = ct->args[i];
            // capture conversion, approximated: ? extends X -> X, ? / ? super X -> the bound
            if (a && a->k == TK::Wildcard)
                a = a->wild == 1 && a->elem ? a->elem : (tps[i]->bounds.empty() ? tObject() : tps[i]->bounds[0]);
            s[tps[i]] = a;
        }
    }
    else
    {
        // raw type: members are erased
        for (auto *tv : tps)
            s[tv] = tv->bounds.empty() ? tObject() : erasure(tv->bounds[0]);
    }
    return s;
}

Subst Sema::fiSubst(const TypeP &ct)
{
    Subst s;
    if (!ct || ct->k != TK::Class || !ct->cls)
        return s;
    const auto &tps = ct->cls->tparams;
    if (ct->args.size() != tps.size())
        return substOf(ct);
    for (size_t i = 0; i < tps.size(); ++i)
    {
        TypeP a = ct->args[i];
        if (a && a->k == TK::Wildcard)
            a = a->elem ? a->elem : (tps[i]->bounds.empty() ? tObject() : tps[i]->bounds[0]);
        s[tps[i]] = a;
    }
    return s;
}

TypeP Sema::asSuper(const TypeP &t, ClassSym *target)
{
    if (!t || !target)
        return nullptr;
    if (t->k == TK::TypeVar || t->k == TK::Wildcard)
    {
        if (t->k == TK::TypeVar && t->tv)
            for (auto &b : t->tv->bounds)
                if (TypeP r = asSuper(b, target))
                    return r;
        TypeP ub = upperBound(t);
        return ub != t ? asSuper(ub, target) : nullptr;
    }
    if (t->k == TK::Array)
    {
        const std::string &n = target->binaryName;
        if (n == "java.lang.Object" || n == "java.lang.Cloneable" || n == "java.io.Serializable")
            return classType(target);
        return nullptr;
    }
    if (t->k != TK::Class || !t->cls)
        return nullptr;
    if (t->cls == target)
        return t;
    resolveHeader(t->cls);
    Subst s = substOf(t);
    if (t->cls->super)
        if (TypeP r = asSuper(subst(t->cls->super, s), target))
            return r;
    for (auto &i : t->cls->ifaces)
        if (TypeP r = asSuper(subst(i, s), target))
            return r;
    if (target->binaryName == "java.lang.Object")
        return classType(target);
    return nullptr;
}

bool Sema::isSubClass(ClassSym *a, ClassSym *b)
{
    if (!a || !b)
        return false;
    if (a == b || b->binaryName == "java.lang.Object")
        return true;
    resolveHeader(a);
    if (a->super && a->super->cls && isSubClass(a->super->cls, b))
        return true;
    for (auto &i : a->ifaces)
        if (i->cls && isSubClass(i->cls, b))
            return true;
    return false;
}

bool Sema::sameType(const TypeP &a, const TypeP &b)
{
    if (a == b)
        return true;
    if (!a || !b || a->k != b->k)
        return false;
    switch (a->k)
    {
    case TK::Class:
        if (a->cls != b->cls || a->args.size() != b->args.size())
            return a->cls == b->cls && (a->args.empty() || b->args.empty());
        for (size_t i = 0; i < a->args.size(); ++i)
            if (!sameType(a->args[i], b->args[i]))
                return false;
        return true;
    case TK::Array:
        return sameType(a->elem, b->elem);
    case TK::TypeVar:
        return a->tv == b->tv;
    case TK::Wildcard:
        return a->wild == b->wild && (a->wild == 0 || sameType(a->elem, b->elem));
    default:
        return true;
    }
}

TypeP Sema::upperBound(const TypeP &t)
{
    if (!t)
        return tObject();
    if (t->k == TK::TypeVar)
    {
        if (!t->tv || t->tv->bounds.empty())
            return tObject();
        // <T extends Object & Comparable<? super T>>: members come from the interface
        const TypeP &b0 = t->tv->bounds[0];
        if (t->tv->bounds.size() > 1 && b0 && b0->k == TK::Class && b0->cls && b0->cls->binaryName == "java.lang.Object")
            return upperBound(t->tv->bounds[1]);
        return upperBound(b0);
    }
    if (t->k == TK::Wildcard)
        return t->wild == 1 && t->elem ? upperBound(t->elem) : tObject();
    return t;
}

bool Sema::isSubtype(const TypeP &s, const TypeP &t)
{
    if (!s || !t || s->k == TK::Error || t->k == TK::Error)
        return true;
    if (sameType(s, t))
        return true;
    if (isPrim(s) || isPrim(t) || s->k == TK::Void || t->k == TK::Void)
        return false;
    if (s->k == TK::Null)
        return isRef(t);
    if (t->k == TK::TypeVar)
        return s->k == TK::TypeVar && s->tv == t->tv;
    if (t->k == TK::Wildcard)
        return t->wild == 2 ? isSubtype(s, t->elem) : true;
    if (s->k == TK::TypeVar || s->k == TK::Wildcard)
    {
        if (s->k == TK::TypeVar && s->tv)
            for (auto &b : s->tv->bounds)
                if (isSubtype(b, t))
                    return true;
        return isSubtype(upperBound(s), t);
    }
    if (t->k == TK::Class && t->cls && t->cls->binaryName == "java.lang.Object")
        return true;
    if (s->k == TK::Array)
    {
        if (t->k == TK::Array)
        {
            if (isPrim(s->elem) || isPrim(t->elem))
                return sameType(s->elem, t->elem);
            return isSubtype(s->elem, t->elem);
        }
        return t->k == TK::Class && t->cls &&
               (t->cls->binaryName == "java.lang.Cloneable" || t->cls->binaryName == "java.io.Serializable");
    }
    if (s->k == TK::Class && t->k == TK::Class)
    {
        if (!isSubClass(s->cls, t->cls))
            return false;
        // type arguments: only clear contradictions are rejected
        if (t->args.empty() || s->args.empty())
            return true;
        TypeP sup = asSuper(s, t->cls);
        if (!sup || sup->args.size() != t->args.size())
            return true;
        for (size_t i = 0; i < t->args.size(); ++i)
        {
            const TypeP &ta = t->args[i], &sa = sup->args[i];
            if (!ta || !sa || ta->k == TK::Wildcard || sa->k == TK::Wildcard || ta->k == TK::TypeVar ||
                sa->k == TK::TypeVar || ta->k == TK::Error || sa->k == TK::Error)
                continue;
            if (ta->k == TK::Class && sa->k == TK::Class && ta->cls != sa->cls)
                return false;
        }
        return true;
    }
    return false;
}

namespace
{
int primRank(TK k)
{
    switch (k)
    {
    case TK::Byte: return 1;
    case TK::Short: return 2;
    case TK::Char: return 2;
    case TK::Int: return 3;
    case TK::Long: return 4;
    case TK::Float: return 5;
    case TK::Double: return 6;
    default: return 0;
    }
}

bool primWidens(TK from, TK to)
{
    if (from == to)
        return true;
    if (from == TK::Boolean || to == TK::Boolean)
        return false;
    if (to == TK::Char)
        return false; // nothing widens to char
    if (from == TK::Char)
        return to == TK::Int || to == TK::Long || to == TK::Float || to == TK::Double;
    if (from == TK::Byte && to == TK::Short)
        return true;
    if (to == TK::Short)
        return false;
    return primRank(from) < primRank(to);
}

bool fitsConst(const Const &cv, TK to)
{
    if (cv.k != Const::Int && cv.k != Const::Char)
        return false;
    int64_t v = cv.i;
    switch (to)
    {
    case TK::Byte: return v >= -128 && v <= 127;
    case TK::Short: return v >= -32768 && v <= 32767;
    case TK::Char: return v >= 0 && v <= 65535;
    default: return false;
    }
}
} // namespace

bool Sema::assignable(const TypeP &from, const TypeP &to, const Const *cv)
{
    if (isError(from) || isError(to))
        return true;
    if (sameType(from, to))
        return true;
    if (to->k == TK::Void || from->k == TK::Void)
        return false;
    if (isPrim(from) && isPrim(to))
    {
        if (primWidens(from->k, to->k))
            return true;
        return cv && fitsConst(*cv, to->k) && (to->k == TK::Byte || to->k == TK::Short || to->k == TK::Char);
    }
    if (isPrim(from) && isRef(to))
    {
        // boxing, then widening reference conversion
        TypeP box = boxOf(from);
        if (isSubtype(box, to))
            return true;
        // constant narrowing then boxing: Byte b = 10; Character c = 65;
        TypeP u = unboxed(to);
        if (cv && u && (u->k == TK::Byte || u->k == TK::Short || u->k == TK::Char) && fitsConst(*cv, u->k))
            return true;
        return false;
    }
    if (isRef(from) && isPrim(to))
    {
        TypeP u = unboxed(from);
        return u && primWidens(u->k, to->k);
    }
    return isSubtype(from, to);
}

bool Sema::castable(const TypeP &from, const TypeP &to)
{
    if (isError(from) || isError(to))
        return true;
    if (isPrim(from) && isPrim(to))
        return (from->k == TK::Boolean) == (to->k == TK::Boolean);
    if (isPrim(from))
        return assignable(from, to) || (to->k == TK::Class && to->cls && to->cls->binaryName == "java.lang.Object");
    if (isPrim(to))
    {
        TypeP u = unboxed(from);
        if (u)
            return (u->k == TK::Boolean) == (to->k == TK::Boolean) && (u->k == to->k || primWidens(u->k, to->k));
        // Object / Number / Comparable / Serializable -> a primitive via its box
        TypeP box = boxOf(to);
        return isSubtype(box, from);
    }
    if (from->k == TK::Null)
        return true;
    if (isSubtype(from, to) || isSubtype(to, from))
        return true;
    TypeP f = upperBound(from), t = upperBound(to);
    if (f->k == TK::Class && t->k == TK::Class && f->cls && t->cls)
    {
        bool fi = f->cls->isInterface(), ti = t->cls->isInterface();
        if (fi && ti)
            return true;
        if (fi)
            return !(t->cls->mods & M_FINAL) || isSubClass(t->cls, f->cls);
        if (ti)
            return !(f->cls->mods & M_FINAL) || isSubClass(f->cls, t->cls);
        return isSubClass(f->cls, t->cls) || isSubClass(t->cls, f->cls);
    }
    if (f->k == TK::Array && t->k == TK::Array)
        return castable(f->elem, t->elem) && isPrim(f->elem) == isPrim(t->elem);
    return from->k == TK::TypeVar || to->k == TK::TypeVar;
}

TypeP Sema::unaryPromote(const TypeP &t0)
{
    TypeP t = numericUnboxed(t0);
    if (!t)
        return tError();
    if (t->k == TK::Byte || t->k == TK::Short || t->k == TK::Char)
        return prim(TK::Int);
    return t;
}

TypeP Sema::binaryPromote(const TypeP &a0, const TypeP &b0)
{
    TypeP a = numericUnboxed(a0), b = numericUnboxed(b0);
    if (!a || !b)
        return tError();
    if (a->k == TK::Double || b->k == TK::Double)
        return prim(TK::Double);
    if (a->k == TK::Float || b->k == TK::Float)
        return prim(TK::Float);
    if (a->k == TK::Long || b->k == TK::Long)
        return prim(TK::Long);
    return prim(TK::Int);
}

TypeP Sema::lub(const TypeP &a, const TypeP &b)
{
    if (isError(a))
        return b;
    if (isError(b))
        return a;
    if (sameType(a, b))
        return a;
    if (a->k == TK::Null)
        return isPrim(b) ? boxOf(b) : b;
    if (b->k == TK::Null)
        return isPrim(a) ? boxOf(a) : a;
    TypeP x = isPrim(a) ? boxOf(a) : a, y = isPrim(b) ? boxOf(b) : b;
    if (isSubtype(x, y))
        return y;
    if (isSubtype(y, x))
        return x;
    TypeP ux = upperBound(x), uy = upperBound(y);
    if (ux->k == TK::Class && uy->k == TK::Class && ux->cls && uy->cls)
    {
        // the nearest common superclass, else a shared interface
        for (ClassSym *c = ux->cls; c; c = c->super ? c->super->cls : nullptr)
        {
            resolveHeader(c);
            if (isSubClass(uy->cls, c) && c->binaryName != "java.lang.Object")
                return asSuper(ux, c) ? asSuper(ux, c) : classType(c);
            if (!c->super)
                break;
        }
        std::function<TypeP(const TypeP &)> findIface = [&](const TypeP &t) -> TypeP
        {
            if (!t || t->k != TK::Class || !t->cls)
                return nullptr;
            resolveHeader(t->cls);
            Subst s = substOf(t);
            for (auto &i : t->cls->ifaces)
            {
                TypeP it = subst(i, s);
                if (it->cls && isSubClass(uy->cls, it->cls) && it->cls->binaryName != "java.io.Serializable" &&
                    it->cls->binaryName != "java.lang.constant.Constable")
                    return it;
            }
            if (t->cls->super)
                return findIface(subst(t->cls->super, s));
            return nullptr;
        };
        if (TypeP i = findIface(ux))
            return i;
    }
    return tObject();
}

// ─── Resolving written types ─────────────────────────────────────────────────

TypeVar *Sema::lookupTypeVar(const std::string &name)
{
    for (size_t i = methodTvars_.size(); i-- > 0;)
        if (methodTvars_[i]->name == name)
            return methodTvars_[i];
    for (FnCtx *f = fn_; f; f = f->outer)
        if (f->method)
            for (auto *tv : f->method->tparams)
                if (tv->name == name)
                    return tv;
    for (ClassSym *c = ctxClass_; c; c = c->outer)
        for (auto *tv : c->tparams)
            if (tv->name == name)
                return tv;
    return nullptr;
}

ClassSym *Sema::memberType(ClassSym *c, const std::string &name)
{
    if (!c)
        return nullptr;
    auto it = c->memberTypes.find(name);
    if (it != c->memberTypes.end())
        return it->second;
    resolveHeader(c);
    if (c->super && c->super->cls)
        if (ClassSym *m = memberType(c->super->cls, name))
            return m;
    for (auto &i : c->ifaces)
        if (i->cls)
            if (ClassSym *m = memberType(i->cls, name))
                return m;
    return nullptr;
}

ClassSym *Sema::lookupClassSimple(const std::string &name)
{
    // local classes, innermost first
    for (FnCtx *f = fn_; f; f = f->outer)
        for (size_t i = f->scopes.size(); i-- > 0;)
        {
            auto it = f->scopes[i].classes.find(name);
            if (it != f->scopes[i].classes.end())
                return it->second;
        }
    // member types of the enclosing classes (and their supertypes)
    for (ClassSym *c = ctxClass_; c; c = c->outer)
    {
        if (c->name == name && !c->isAnon)
            return c;
        if (ClassSym *m = memberType(c, name))
            return m;
        if (c->enclosingFn)
        {
            // a local class sees the local classes of its enclosing method
            for (FnCtx *f = c->enclosingFn; f; f = f->outer)
                for (size_t i = f->scopes.size(); i-- > 0;)
                {
                    auto it = f->scopes[i].classes.find(name);
                    if (it != f->scopes[i].classes.end())
                        return it->second;
                }
        }
    }
    const CompilationUnit &cu = units_[unit_];
    // top-level classes of this unit
    for (auto &t : cu.types)
        if (t->name == name)
        {
            std::string pkg;
            for (auto &p : cu.package)
                pkg += (pkg.empty() ? "" : ".") + p;
            if (ClassSym *c = cls(pkg.empty() ? name : pkg + "." + name))
                return c;
        }
    if (cu.isPrelude)
    {
        // prelude code sees the whole prelude by simple name
        static const char *pkgs[] = {"java.lang", "java.util", "java.util.function", "java.util.stream", "java.io",
                                     "java.math", "java.util.concurrent", "java.util.concurrent.atomic",
                                     "java.lang.constant", "java.text", "java.util.regex", "java.time"};
        for (const char *p : pkgs)
            if (ClassSym *c = cls(std::string(p) + "." + name))
                return c;
        return nullptr;
    }
    // single-type imports
    for (auto &im : cu.imports)
        if (!im.isStatic && !im.onDemand && !im.name.empty() && im.name.back() == name)
        {
            std::string q;
            for (auto &p : im.name)
                q += (q.empty() ? "" : ".") + p;
            if (ClassSym *c = cls(q))
                return c;
            // an imported nested type: java.util.Map.Entry
            std::vector<std::string> parts = im.name;
            if (ClassSym *c = resolveTypeName(parts, im.pos, false))
                return c;
        }
    // same package (the user's file is its own package)
    {
        std::string pkg;
        for (auto &p : cu.package)
            pkg += (pkg.empty() ? "" : ".") + p;
        if (!pkg.empty())
            if (ClassSym *c = cls(pkg + "." + name))
                return c;
    }
    // on-demand imports, then java.lang
    for (auto &im : cu.imports)
        if (!im.isStatic && im.onDemand)
        {
            std::string q;
            for (auto &p : im.name)
                q += (q.empty() ? "" : ".") + p;
            if (ClassSym *c = cls(q + "." + name))
                return c;
            // import Outer.*; (member types)
            if (ClassSym *outer = cls(q))
                if (ClassSym *m = memberType(outer, name))
                    return m;
        }
    if (ClassSym *c = cls("java.lang." + name))
        return c;
    return nullptr;
}

ClassSym *Sema::resolveTypeName(const std::vector<std::string> &parts, Pos pos, bool report)
{
    if (parts.empty())
        return nullptr;
    if (!parts[0].empty() && parts[0][0] == '\x01')
    {
        // a class named by binary name (code the front-end synthesized)
        auto it = byBinary_.find(parts[0].substr(1));
        return it == byBinary_.end() ? nullptr : it->second;
    }
    ClassSym *c = lookupClassSimple(parts[0]);
    size_t i = 1;
    if (!c)
    {
        // a package prefix: java.util.Map.Entry
        std::string q = parts[0];
        for (; i < parts.size(); ++i)
        {
            q += "." + parts[i];
            if ((c = cls(q)))
            {
                ++i;
                break;
            }
        }
        if (!c)
        {
            if (report)
                error(pos, "cannot find symbol",
                      {"  symbol:   class " + parts[0],
                       "  location: " + (ctxClass_ ? std::string(ctxClass_->isInterface() ? "interface " : "class ") +
                                                         typeStr(classType(ctxClass_))
                                                   : std::string("package"))});
            return nullptr;
        }
    }
    for (; i < parts.size(); ++i)
    {
        ClassSym *m = memberType(c, parts[i]);
        if (!m)
        {
            if (report)
                error(pos, "cannot find symbol", {"  symbol:   class " + parts[i], "  location: class " + c->name});
            return nullptr;
        }
        c = m;
    }
    return c;
}

TypeP Sema::resolveType(const TypeRefP &r, bool allowVar)
{
    if (!r)
        return tError();
    switch (r->kind)
    {
    case TypeRef::Prim:
    {
        static const std::map<std::string, TK> k = {{"void", TK::Void},   {"boolean", TK::Boolean}, {"byte", TK::Byte},
                                                     {"short", TK::Short}, {"char", TK::Char},       {"int", TK::Int},
                                                     {"long", TK::Long},   {"float", TK::Float},     {"double", TK::Double}};
        return prim(k.at(r->prim));
    }
    case TypeRef::Var:
        if (!allowVar)
            error(r->pos, "'var' is not allowed here");
        return nullptr;
    case TypeRef::Array:
    {
        TypeP e = resolveType(r->elem);
        if (e->k == TK::Void)
        {
            error(r->pos, "'void' type not allowed here");
            return tError();
        }
        return arrayOf(e);
    }
    case TypeRef::Wildcard:
    {
        auto w = std::make_shared<Type>();
        w->k = TK::Wildcard;
        w->wild = r->wildKind;
        if (r->bound)
            w->elem = resolveType(r->bound);
        return w;
    }
    case TypeRef::Named:
    {
        if (r->parts.size() == 1 && !r->parts[0].hasArgs)
            if (TypeVar *tv = lookupTypeVar(r->parts[0].name))
                return typeVar(tv);
        std::vector<std::string> names;
        for (auto &p : r->parts)
            names.push_back(p.name);
        ClassSym *c = resolveTypeName(names, r->pos, true);
        if (!c)
            return tError();
        resolveHeader(c);
        const auto &last = r->parts.back();
        std::vector<TypeP> args;
        if (last.hasArgs)
        {
            for (auto &a : last.args)
            {
                TypeP at = resolveType(a);
                if (isPrim(at))
                {
                    error(a->pos, "unexpected type", {"  required: reference", "  found:    " + typeStr(at)});
                    at = tError();
                }
                args.push_back(at);
            }
            if (args.empty())
                return classType(c); // diamond: inferred later
            if (args.size() != c->tparams.size())
            {
                error(r->pos, "wrong number of type arguments; required " + std::to_string(c->tparams.size()));
                return classType(c);
            }
        }
        return classType(c, std::move(args));
    }
    }
    return tError();
}

MethodSym *Sema::functionalMethod(ClassSym *iface)
{
    if (!iface || !iface->isInterface())
        return nullptr;
    enterMembers(iface);
    std::vector<MethodSym *> abstracts;
    std::set<std::string> seen;
    std::function<void(ClassSym *)> walk = [&](ClassSym *c)
    {
        enterMembers(c);
        for (MethodSym *m : c->methods)
        {
            if (!m->isAbstract() || m->isStatic())
                continue;
            if ((m->name == "equals" && m->params.size() == 1) || (m->name == "hashCode" && m->params.empty()) ||
                (m->name == "toString" && m->params.empty()))
                continue;
            std::string key = m->name + "/" + std::to_string(m->params.size());
            if (seen.insert(key).second)
                abstracts.push_back(m);
        }
        for (auto &i : c->ifaces)
            if (i->cls)
                walk(i->cls);
    };
    walk(iface);
    // a default method in the interface itself may implement an inherited abstract one
    if (abstracts.size() > 1)
    {
        std::vector<MethodSym *> left;
        for (MethodSym *m : abstracts)
        {
            bool implemented = false;
            for (MethodSym *d : iface->methods)
                if (d->isDefault() && d->name == m->name && d->params.size() == m->params.size())
                    implemented = true;
            if (!implemented)
                left.push_back(m);
        }
        abstracts = left;
    }
    return abstracts.size() == 1 ? abstracts[0] : nullptr;
}

std::string Sema::descriptor(const TypeP &t0)
{
    TypeP t = erasure(t0);
    if (t->k == TK::Array)
        return "[";
    if (t->k == TK::Class && t->cls)
        return std::string(t->cls->isInterface() ? "I:" : "C:") + t->cls->binaryName;
    return "C:java.lang.Object";
}

} // namespace java
