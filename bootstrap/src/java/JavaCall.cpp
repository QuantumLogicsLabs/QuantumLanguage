// Method and constructor invocation (JLS §15.12, §15.9): lookup, overload
// resolution, type inference; lambdas and method references (§15.27, §15.13).
#include "JavaSema.h"

#include <algorithm>

namespace java
{

using namespace jop;

namespace
{
ClassSym *topLevelOf(ClassSym *c)
{
    while (c && c->outer)
        c = c->outer;
    return c;
}

bool isPoly(const ExprP &e) { return e && (e->k == EK::Lambda || e->k == EK::MethodRef); }

// A generic method call or a diamond: its type may depend on the target.
bool polyish(const ExprP &a)
{
    return a && (a->k == EK::Call || (a->k == EK::New && !a->body && a->type && !a->type->parts.empty() &&
                                      a->type->parts.back().hasArgs && a->type->parts.back().args.empty()));
}

std::string argTypesStr(Sema &s, const std::vector<Val> &vals, const std::vector<ExprP> &args)
{
    std::string out;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i)
            out += ",";
        if (isPoly(args[i]))
            out += args[i]->k == EK::Lambda ? "(" + std::string() + ")->{ }" : "method reference";
        else
            out += i < vals.size() && vals[i].type ? s.typeStr(vals[i].type) : "<any>";
    }
    return out;
}
} // namespace

// ─── Applicability ───────────────────────────────────────────────────────────

namespace
{
struct Infer
{
    Sema &s;
    std::vector<TypeVar *> vars;
    std::map<const TypeVar *, std::vector<TypeP>> eq, lo, up;

    bool isVar(const TypeP &t) const
    {
        return t && t->k == TK::TypeVar && std::find(vars.begin(), vars.end(), t->tv) != vars.end();
    }
    // formal (with inference variables) vs actual: actual <: formal (mode 0),
    // formal == actual (mode 1), formal <: actual (mode 2)
    void unify(const TypeP &formal, const TypeP &actual0, int mode)
    {
        if (!formal || !actual0 || actual0->k == TK::Error || actual0->k == TK::Null)
            return;
        TypeP actual = Sema::isPrim(actual0) ? s.boxOf(actual0) : actual0;
        if (isVar(formal))
        {
            if (actual->k == TK::Wildcard)
            {
                if (actual->elem && actual->wild == 1)
                    up[formal->tv].push_back(actual->elem);
                else if (actual->elem && actual->wild == 2)
                    lo[formal->tv].push_back(actual->elem);
                return;
            }
            (mode == 0 ? lo : mode == 1 ? eq : up)[formal->tv].push_back(actual);
            return;
        }
        if (formal->k == TK::Wildcard)
        {
            if (formal->elem)
                unify(formal->elem, actual->k == TK::Wildcard ? actual->elem : actual, formal->wild == 1 ? 0 : 2);
            return;
        }
        if (formal->k == TK::Array && actual->k == TK::Array)
        {
            unify(formal->elem, actual->elem, Sema::isPrim(actual->elem) ? 1 : mode);
            return;
        }
        if (formal->k == TK::Class && !formal->args.empty())
        {
            TypeP sup = mode == 2 ? s.asSuper(formal, actual->cls ? actual->cls : nullptr) : s.asSuper(actual, formal->cls);
            if (mode == 2)
            {
                // formal <: actual: view the formal as the actual's class
                TypeP fv = actual->k == TK::Class && actual->cls ? s.asSuper(formal, actual->cls) : nullptr;
                if (fv && fv->args.size() == actual->args.size())
                    for (size_t i = 0; i < fv->args.size(); ++i)
                        unify(fv->args[i], actual->args[i], 1);
                return;
            }
            if (!sup || sup->args.size() != formal->args.size())
                return;
            for (size_t i = 0; i < formal->args.size(); ++i)
            {
                const TypeP &fa = formal->args[i];
                TypeP aa = sup->args[i];
                if (fa && fa->k == TK::Wildcard)
                {
                    if (fa->elem)
                        unify(fa->elem, aa && aa->k == TK::Wildcard ? aa->elem : aa, fa->wild == 1 ? 0 : 2);
                }
                else
                    unify(fa, aa && aa->k == TK::Wildcard && aa->elem ? aa->elem : aa, 1);
            }
        }
    }
    TypeP solve(const TypeVar *tv)
    {
        auto e = eq.find(tv);
        if (e != eq.end() && !e->second.empty())
        {
            for (auto &t : e->second)
                if (!Sema::isError(t) && !isVar(t))
                    return t;
        }
        auto l = lo.find(tv);
        if (l != lo.end() && !l->second.empty())
        {
            TypeP r;
            for (auto &t : l->second)
                if (!isVar(t))
                    r = r ? s.lub(r, t) : t;
            if (r)
                return r;
        }
        auto u = up.find(tv);
        if (u != up.end() && !u->second.empty())
            for (auto &t : u->second)
                if (!isVar(t))
                    return t;
        return nullptr;
    }
};

// Replace inference variables by their bounds (for applicability checks).
TypeP eraseVars(Sema &s, const TypeP &t, const std::vector<TypeVar *> &vars)
{
    if (!t)
        return t;
    if (t->k == TK::TypeVar && std::find(vars.begin(), vars.end(), t->tv) != vars.end())
        return t->tv->bounds.empty() ? s.tObject() : s.erasure(t->tv->bounds[0]);
    if (t->k == TK::Array)
        return s.arrayOf(eraseVars(s, t->elem, vars));
    if (t->k == TK::Class && !t->args.empty())
    {
        bool has = false;
        std::function<bool(const TypeP &)> mentions = [&](const TypeP &x) -> bool {
            if (!x)
                return false;
            if (x->k == TK::TypeVar)
                return std::find(vars.begin(), vars.end(), x->tv) != vars.end();
            if (x->k == TK::Array || x->k == TK::Wildcard)
                return mentions(x->elem);
            for (auto &a : x->args)
                if (mentions(a))
                    return true;
            return false;
        };
        has = mentions(t);
        return has ? s.erasure(t) : t;
    }
    return t;
}

bool mentionsVars(const TypeP &t, const std::vector<TypeVar *> &vars)
{
    if (!t)
        return false;
    if (t->k == TK::TypeVar)
        return std::find(vars.begin(), vars.end(), t->tv) != vars.end();
    if (t->k == TK::Array || t->k == TK::Wildcard)
        return mentionsVars(t->elem, vars);
    for (auto &a : t->args)
        if (mentionsVars(a, vars))
            return true;
    return false;
}

// A target with the inference variables still unsolved replaced by `?`: a
// nested generic call typed against it is not constrained by them
// (stream.collect(Collectors.toList()): Collector<? super Integer, ?, ?>).
TypeP wildVars(Sema &s, const TypeP &t, const std::vector<TypeVar *> &vars)
{
    if (!t)
        return t;
    auto anyWild = []
    {
        auto w = std::make_shared<Type>();
        w->k = TK::Wildcard;
        w->wild = 0;
        return TypeP(w);
    };
    if (t->k == TK::TypeVar)
        return std::find(vars.begin(), vars.end(), t->tv) != vars.end() ? anyWild() : t;
    if (t->k == TK::Array)
        return s.arrayOf(wildVars(s, t->elem, vars));
    if (t->k == TK::Wildcard)
    {
        if (!t->elem || !mentionsVars(t->elem, vars))
            return t;
        TypeP inner = wildVars(s, t->elem, vars);
        if (inner->k == TK::Wildcard)
            return anyWild();
        auto w = std::make_shared<Type>(*t);
        w->elem = inner;
        return TypeP(w);
    }
    if (t->k == TK::Class && !t->args.empty())
    {
        std::vector<TypeP> a;
        for (auto &x : t->args)
            a.push_back(wildVars(s, x, vars));
        return s.classType(t->cls, a);
    }
    return t;
}
} // namespace

// Can lambda / method reference `arg` target `param` (a functional interface)?
bool Sema::lambdaCompatible(const ExprP &arg, const TypeP &param)
{
    TypeP p = upperBound(param);
    if (!p || isError(p))
        return true;
    if (p->k != TK::Class || !p->cls || !p->cls->isInterface())
        return false;
    MethodSym *sam = functionalMethod(p->cls);
    if (!sam)
        return false;
    if (arg->k == EK::Lambda)
    {
        if (arg->params.size() != sam->params.size())
            return false;
        bool voidSam = sam->ret && sam->ret->k == TK::Void;
        if (arg->a)
        {
            // expression body: value-compatible always; void-compatible when a statement expression
            if (voidSam)
            {
                EK k = arg->a->k;
                return k == EK::Call || k == EK::Assign || k == EK::New || k == EK::PreIncDec || k == EK::PostIncDec;
            }
            return true;
        }
        // block body: void-compatible unless it returns a value
        std::function<int(const StmtP &)> returns = [&](const StmtP &st) -> int {
            if (!st)
                return 0;
            if (st->k == SK::Return)
                return st->e ? 2 : 1;
            if (st->k == SK::LocalClass)
                return 0;
            int r = 0;
            for (auto &x : st->stmts)
                r |= returns(x);
            r |= returns(st->s1) | returns(st->s2) | returns(st->tryBlock) | returns(st->finallyBlock);
            for (auto &c : st->catches)
                r |= returns(c.body);
            if (st->sw)
                for (auto &c : st->sw->cases)
                    for (auto &x : c.body)
                        r |= returns(x);
            return r;
        };
        int r = returns(arg->block);
        if (voidSam)
            return !(r & 2);
        // a value-returning SAM needs `return v` (or a body that cannot complete: throw)
        return (r & 2) || r == 0;
    }
    return true; // method references: checked when lowered
}

namespace
{
// Is `v` (with static type) compatible with `formal` in phase 1 (strict) or 2 (loose)?
bool argOk(Sema &s, const Val &v, const TypeP &formal, int phase)
{
    TypeP t = v.type;
    if (Sema::isError(t) || Sema::isError(formal))
        return true;
    if (t->k == TK::Void)
        return false;
    if (phase == 1)
    {
        if (Sema::isPrim(t) != Sema::isPrim(formal) && t->k != TK::Null)
            return false;
        if (Sema::isPrim(t) && Sema::isPrim(formal))
            return s.assignable(t, formal); // widening only (no constant narrowing in invocation)
        return s.isSubtype(t, formal);
    }
    if (Sema::isPrim(t) && Sema::isPrim(formal))
        return s.assignable(t, formal);
    return s.assignable(t, formal);
}
} // namespace

Sema::Resolved Sema::resolveCall(std::vector<MethodSym *> &cands, const std::vector<ExprP> &args,
                                 const TypeP &recvType, const TypeP &target, Pos pos, const std::string &name,
                                 const std::vector<TypeRefP> &explicitTargs, std::vector<Val> &lowered,
                                 std::string &failNotes)
{
    Resolved res;
    (void)name;
    // 1. attribute the standalone arguments; a generic call / diamond is typed
    //    again once its target is known, so its diagnostics wait until then
    std::vector<std::vector<Diag>> deferred(args.size());
    if (lowered.empty())
    {
        lowered.resize(args.size());
        for (size_t i = 0; i < args.size(); ++i)
            if (!isPoly(args[i]))
            {
                size_t d0 = diags_.size();
                lowered[i] = expr(args[i]);
                if (lowered[i].polyCall && diags_.size() > d0)
                {
                    deferred[i].assign(diags_.begin() + (std::ptrdiff_t)d0, diags_.end());
                    diags_.resize(d0);
                }
            }
    }
    auto flushDeferred = [&]()
    {
        for (auto &d : deferred)
            for (auto &x : d)
                diags_.push_back(x);
        deferred.assign(args.size(), {});
    };
    std::vector<TypeP> explicitT;
    for (auto &t : explicitTargs)
        explicitT.push_back(resolveType(t));

    struct Cand
    {
        MethodSym *m;
        Subst cs;                // class type arguments (receiver view)
        std::vector<TypeP> formals;
        bool varargs = false;    // applicable by variable arity only
    };
    auto instantiate = [&](MethodSym *m) -> Cand
    {
        Cand c;
        c.m = m;
        if (recvType && !m->isStatic() && !m->isCtor)
            if (TypeP view = asSuper(recvType, m->owner))
                c.cs = substOf(view);
        if (m->isCtor && recvType && recvType->k == TK::Class && !recvType->args.empty())
            c.cs = substOf(recvType);
        if (!explicitT.empty() && explicitT.size() == m->tparams.size())
            for (size_t i = 0; i < explicitT.size(); ++i)
                c.cs[m->tparams[i]] = explicitT[i];
        for (auto &p : m->params)
            c.formals.push_back(subst(p, c.cs));
        return c;
    };
    std::vector<TypeVar *> noVars;
    auto inferVarsOf = [&](MethodSym *m) -> std::vector<TypeVar *>
    {
        std::vector<TypeVar *> v;
        if (explicitT.empty())
            v = m->tparams;
        // diamond: the class's own type parameters are inferred too
        if (m->isCtor && recvType && recvType->k == TK::Class && recvType->args.empty())
            for (auto *tv : m->owner->tparams)
                v.push_back(tv);
        return v;
    };
    // a generic call / diamond argument attributed with the formal as its
    // target (inference from the target), for applicability only
    auto retargetOk = [&](const ExprP &a, const TypeP &f, int phase) -> bool
    {
        if (!f || isError(f))
            return false;
        size_t saved = diags_.size();
        ++probing_;
        Val v = expr(a, f);
        --probing_;
        bool bad = diags_.size() > saved;
        diags_.resize(saved);
        return !bad && !isError(v.type) && argOk(*this, v, f, phase);
    };
    auto applicable = [&](Cand &c, int phase) -> bool
    {
        MethodSym *m = c.m;
        std::vector<TypeVar *> vars = inferVarsOf(m);
        size_t n = c.formals.size();
        if (phase < 3)
        {
            if (args.size() != n)
                return false;
            for (size_t i = 0; i < n; ++i)
            {
                TypeP f = eraseVars(*this, c.formals[i], vars);
                if (isPoly(args[i]))
                {
                    if (!lambdaCompatible(args[i], f))
                        return false;
                    continue;
                }
                if (!argOk(*this, lowered[i], f, phase) && !(lowered[i].polyCall && retargetOk(args[i], f, phase)))
                    return false;
            }
            return true;
        }
        if (!m->varargs || args.size() + 1 < n)
            return false;
        for (size_t i = 0; i + 1 < n; ++i)
        {
            TypeP f = eraseVars(*this, c.formals[i], vars);
            if (isPoly(args[i]) ? !lambdaCompatible(args[i], f) : !argOk(*this, lowered[i], f, 2))
                return false;
        }
        TypeP elem = eraseVars(*this, c.formals[n - 1], vars);
        elem = elem && elem->k == TK::Array ? elem->elem : tError();
        for (size_t i = n - 1; i < args.size(); ++i)
            if (isPoly(args[i]) ? !lambdaCompatible(args[i], elem) : !argOk(*this, lowered[i], elem, 2))
                return false;
        c.varargs = true;
        return true;
    };
    std::vector<Cand> app;
    for (int phase = 1; phase <= 3 && app.empty(); ++phase)
        for (MethodSym *m : cands)
        {
            Cand c = instantiate(m);
            if (applicable(c, phase))
                app.push_back(c);
        }
    if (app.empty())
    {
        // javac's explanation
        for (MethodSym *m : cands)
        {
            Cand c = instantiate(m);
            std::string why;
            if (!m->varargs && c.formals.size() != args.size())
                why = "actual and formal argument lists differ in length";
            else
            {
                for (size_t i = 0; i < args.size() && i < c.formals.size(); ++i)
                {
                    TypeP f = eraseVars(*this, c.formals[i], inferVarsOf(m));
                    if (isPoly(args[i]))
                    {
                        if (!lambdaCompatible(args[i], f))
                        {
                            why = "argument mismatch; " + std::string(args[i]->k == EK::Lambda ? "incompatible parameter types in lambda expression" : "invalid method reference");
                            break;
                        }
                    }
                    else if (!argOk(*this, lowered[i], f, 2))
                    {
                        bool lossy = isPrim(lowered[i].type) && isPrim(f) && isNumeric(lowered[i].type) && isNumeric(f);
                        why = "argument mismatch; " + (lossy ? "possible lossy conversion from " + typeStr(lowered[i].type) + " to " + typeStr(f)
                                                             : typeStr(lowered[i].type) + " cannot be converted to " + typeStr(f));
                        break;
                    }
                }
                if (why.empty())
                    why = "actual and formal argument lists differ in length";
            }
            std::string req;
            for (size_t i = 0; i < m->params.size(); ++i)
            {
                std::string p = typeStr(m->params[i]);
                if (m->varargs && i + 1 == m->params.size() && p.size() > 2)
                    p = p.substr(0, p.size() - 2) + "...";
                req += (i ? "," : "") + p;
            }
            if (cands.size() == 1)
                failNotes = "  required: " + (req.empty() ? std::string("no arguments") : req) + "\n  found:    " +
                            (args.empty() ? std::string("no arguments") : argTypesStr(*this, lowered, args)) +
                            "\n  reason: " + why;
            else
                failNotes += std::string(failNotes.empty() ? "" : "\n") + "    " + std::string(m->isCtor ? "constructor " : "method ") +
                             typeStr(classType(m->owner)) + "." + (m->isCtor ? m->owner->name : m->name) + "(" + req +
                             ") is not applicable\n      (" + why + ")";
        }
        flushDeferred();
        return res;
    }
    // most specific
    auto moreSpecific = [&](const Cand &a, const Cand &b) -> bool
    {
        size_t n = std::max(a.formals.size(), b.formals.size());
        if (args.empty())
            return a.m->owner != b.m->owner ? isSubClass(a.m->owner, b.m->owner) : true;
        for (size_t i = 0; i < args.size() && i < n; ++i)
        {
            auto at = [&](const Cand &c) -> TypeP {
                size_t k = std::min(i, c.formals.size() - 1);
                TypeP f = c.formals.empty() ? tError() : c.formals[k];
                if (c.varargs && k == c.formals.size() - 1 && f->k == TK::Array && !(args.size() == c.formals.size() && !isPoly(args[i]) && lowered[i].type && lowered[i].type->k == TK::Array))
                    f = f->elem;
                return eraseVars(*this, f, c.m->tparams);
            };
            TypeP fa = at(a), fb = at(b);
            if (isPoly(args[i]))
            {
                // a functional interface whose method returns a value beats a void one
                MethodSym *sa = fa->k == TK::Class && fa->cls ? functionalMethod(fa->cls) : nullptr;
                MethodSym *sb = fb->k == TK::Class && fb->cls ? functionalMethod(fb->cls) : nullptr;
                if (sa && sb && sa->ret && sb->ret && sa->ret->k == TK::Void && sb->ret->k != TK::Void)
                    return false;
                continue;
            }
            if (isPrim(fa) && isPrim(fb))
            {
                if (!assignable(fa, fb))
                    return false;
            }
            else if (isPrim(fa) != isPrim(fb))
            {
                // with a primitive argument the primitive parameter wins, and vice versa
                bool argPrim = isPrim(lowered[i].type);
                if (isPrim(fa) != argPrim)
                    return false;
            }
            else if (!isSubtype(fa, fb))
                return false;
        }
        return true;
    };
    std::vector<Cand> best;
    for (auto &c : app)
    {
        bool dominated = false;
        for (auto &o : app)
            if (&o != &c && moreSpecific(o, c) && !moreSpecific(c, o))
            {
                dominated = true;
                break;
            }
        if (!dominated)
            best.push_back(c);
    }
    // an argument whose type is already an error matches anything: no
    // ambiguity is reported on top of that error
    bool erroneousArg = false;
    for (size_t i = 0; i < args.size(); ++i)
        if (!isPoly(args[i]) && i < lowered.size() && lowered[i].type && isError(lowered[i].type))
            erroneousArg = true;
    if (best.size() > 1 && erroneousArg)
        best.resize(1);
    if (best.size() > 1)
    {
        // same signature inherited along several paths: prefer a concrete one / the most derived class
        std::vector<Cand> concrete;
        for (auto &c : best)
            if (!c.m->isAbstract())
                concrete.push_back(c);
        if (concrete.size() == 1)
            best = concrete;
        else
        {
            bool sameSig = true;
            for (auto &c : best)
                if (c.m->params.size() != best[0].m->params.size())
                    sameSig = false;
                else
                    for (size_t i = 0; i < c.m->params.size(); ++i)
                        if (erasedName(c.m->params[i]) != erasedName(best[0].m->params[i]))
                            sameSig = false;
            if (!sameSig)
            {
                res.ambiguous = true;
                failNotes = "  both " + std::string(best[0].m->isCtor ? "constructor " : "method ") + sigStr(best[0].m) + " in " +
                            best[0].m->owner->name + " and " + std::string(best[1].m->isCtor ? "constructor " : "method ") +
                            sigStr(best[1].m) + " in " + best[1].m->owner->name + " match";
                flushDeferred();
                return res;
            }
        }
    }
    Cand chosen = best[0];
    res.m = chosen.m;
    res.varargsCall = chosen.varargs;
    res.subst = chosen.cs;

    // 2. inference
    std::vector<TypeVar *> vars = inferVarsOf(chosen.m);
    auto formalFor = [&](size_t i) -> TypeP
    {
        size_t n = chosen.formals.size();
        if (chosen.varargs && i + 1 >= n)
        {
            TypeP f = chosen.formals[n - 1];
            return f->k == TK::Array ? f->elem : f;
        }
        return i < n ? chosen.formals[i] : tError();
    };
    if (!vars.empty())
    {
        Infer inf{*this, vars};
        // poly arguments (generic calls, diamonds) are typed against their
        // targets below; the others constrain the variables first
        for (size_t i = 0; i < args.size(); ++i)
            if (!isPoly(args[i]) && !lowered[i].polyCall)
                inf.unify(formalFor(i), lowered[i].type, 0);
        Subst s = res.subst;
        auto solveAll = [&]()
        {
            for (auto *tv : vars)
                if (TypeP t = inf.solve(tv))
                    s[tv] = t;
        };
        // the target type: its equality constraints (List<E> := List<Shape>)
        // take precedence over the lub of the arguments
        if (target && !isError(target) && chosen.m->ret && !chosen.m->isCtor && mentionsVars(chosen.m->ret, vars))
        {
            inf.unify(chosen.m->ret, target, 2);
            if (chosen.m->ret->k == TK::TypeVar)
                inf.up[chosen.m->ret->tv].push_back(target);
        }
        solveAll();
        if (chosen.m->isCtor && target && target->k == TK::Class && recvType && recvType->args.empty())
        {
            // diamond: new ArrayList<>() assigned to List<String>
            TypeP self = classType(chosen.m->owner, [&] {
                std::vector<TypeP> a;
                for (auto *tv : chosen.m->owner->tparams)
                    a.push_back(typeVar(tv));
                return a;
            }());
            if (TypeP view = asSuper(self, target->cls))
                for (size_t i = 0; i < view->args.size() && i < target->args.size(); ++i)
                    inf.unify(view->args[i], target->args[i], 1);
            solveAll();
        }
        // poly arguments typed against what is known of their targets (the
        // variables still open stand as `?`), and lambdas / method references
        // given their parameter types; twice, since each can settle the other
        std::vector<char> retyped(args.size(), 0);
        for (int round = 0; round < 2; ++round)
        {
            for (size_t i = 0; i < args.size(); ++i)
            {
                if (isPoly(args[i]) || !lowered[i].polyCall || retyped[i])
                    continue;
                TypeP f = wildVars(*this, subst(formalFor(i), s), vars);
                if (!f || f->k == TK::Wildcard)
                    continue;
                size_t savedDiags = diags_.size();
                Val again = expr(args[i], f);
                if (diags_.size() > savedDiags)
                {
                    diags_.resize(savedDiags);
                    continue;
                }
                lowered[i] = std::move(again);
                deferred[i].clear();
                retyped[i] = 1;
                inf.unify(formalFor(i), lowered[i].type, 0);
            }
            solveAll();
            for (size_t i = 0; i < args.size(); ++i)
            {
                if (!isPoly(args[i]))
                    continue;
                TypeP f = formalFor(i);
                TypeP fi = subst(f, s);
                TypeP got = inferFromLambda(args[i], fi, s, vars);
                if (got)
                    inf.unify(f, got, 0);
                solveAll();
            }
        }
        // a poly argument its target could not settle: its own type counts
        for (size_t i = 0; i < args.size(); ++i)
            if (!isPoly(args[i]) && lowered[i].polyCall && !retyped[i])
                inf.unify(formalFor(i), lowered[i].type, 0);
        solveAll();
        for (auto *tv : vars)
            if (!s.count(tv) || !s[tv])
                s[tv] = tv->bounds.empty() ? tObject() : subst(tv->bounds[0], s);
        res.subst = s;
    }
    else
    {
        // generic calls / diamonds as arguments of a non-generic method
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (isPoly(args[i]) || !lowered[i].polyCall)
                continue;
            TypeP f = formalFor(i);
            if (argOk(*this, lowered[i], f, 3))
                continue;
            size_t savedDiags = diags_.size();
            Val again = expr(args[i], f);
            if (diags_.size() > savedDiags)
                diags_.resize(savedDiags);
            else
            {
                lowered[i] = std::move(again);
                deferred[i].clear();
            }
        }
    }
    flushDeferred();
    for (size_t i = 0; i < chosen.formals.size(); ++i)
        res.paramTypes.push_back(subst(chosen.m->params[i], res.subst));
    res.ret = subst(chosen.m->ret, res.subst);
    return res;
}

// The type a lambda / method reference argument gives its functional
// interface: it is attributed against `fiType` (whose parameter types are
// known) with its return type left open, and the type it returns fills the
// interface's return position. Used for inference only (code discarded).
TypeP Sema::inferFromLambda(const ExprP &lam, const TypeP &fiType, Subst &s, const std::vector<TypeVar *> &vars)
{
    (void)s;
    TypeP ub = upperBound(fiType);
    if (!ub || ub->k != TK::Class || !ub->cls)
        return nullptr;
    MethodSym *sam = functionalMethod(ub->cls);
    if (!sam || !sam->ret || sam->ret->k == TK::Void || sam->ret->k != TK::TypeVar)
        return nullptr;
    TypeP view = asSuper(ub, sam->owner);
    if (!view)
        return nullptr;
    Subst fs = fiSubst(view);
    if (!mentionsVars(subst(sam->ret, fs), vars))
        return nullptr; // the return type is already known
    if (lam->k == EK::Lambda)
    {
        bool explicitTypes = !lam->params.empty() && lam->params[0].type;
        for (auto &p : sam->params)
            if (mentionsVars(subst(p, fs), vars) && !explicitTypes)
                return nullptr;
    }
    size_t savedDiags = diags_.size();
    std::vector<TypeP> returns;
    std::vector<TypeP> *savedOverride = lambdaReturnsOverride_;
    lambdaReturnsOverride_ = &returns;
    if (lam->k == EK::Lambda)
        lambda(lam, fiType);
    else
        methodRef(lam, fiType);
    lambdaReturnsOverride_ = savedOverride;
    diags_.resize(savedDiags);
    TypeP found;
    for (auto &t : returns)
        if (!isError(t) && t->k != TK::Void)
            found = found ? lub(found, t) : t;
    if (!found)
        return nullptr;
    if (lam->k == EK::MethodRef && lam->name == "new" && found->k == TK::Class && !found->args.empty())
        found = erasure(found);
    Subst os = fs;
    os[sam->ret->tv] = isPrim(found) ? boxOf(found) : found;
    std::vector<TypeP> args;
    for (auto *tp : sam->owner->tparams)
    {
        auto it = os.find(tp);
        args.push_back(it != os.end() && it->second ? it->second : tObject());
    }
    return classType(sam->owner, args);
}

// ─── Lowering a resolved call ────────────────────────────────────────────────

std::vector<ASTNodePtr> Sema::lowerArgs(Resolved &r, const std::vector<ExprP> &args, std::vector<Val> &pre, Pos pos)
{
    std::vector<ASTNodePtr> out;
    MethodSym *m = r.m;
    size_t n = r.paramTypes.size();
    auto lowerOne = [&](size_t i, const TypeP &to) -> ASTNodePtr
    {
        if (isPoly(args[i]))
        {
            Val v = expr(args[i], to);
            return coerce(std::move(v), to, args[i]->pos);
        }
        return coerce(std::move(pre[i]), to, args[i]->pos);
    };
    if (!r.varargsCall)
    {
        for (size_t i = 0; i < args.size() && i < n; ++i)
            out.push_back(lowerOne(i, r.paramTypes[i]));
        return out;
    }
    for (size_t i = 0; i + 1 < n; ++i)
        out.push_back(lowerOne(i, r.paramTypes[i]));
    TypeP arrT = r.paramTypes[n - 1];
    TypeP elem = arrT && arrT->k == TK::Array ? arrT->elem : tObject();
    std::vector<ASTNodePtr> rest;
    for (size_t i = n - 1; i < args.size(); ++i)
        rest.push_back(lowerOne(i, elem));
    out.push_back(q::arr(std::move(rest), pos.line));
    (void)m;
    return out;
}

ASTNodePtr Sema::invoke(MethodSym *m, ASTNodePtr recv, std::vector<ASTNodePtr> args, bool virt, int l,
                        const TypeP &recvType)
{
    (void)recvType;
    needMethod(m);
    // a native method is called as itself unless a subclass may override it
    bool overridable = !m->isStatic() && !m->isPrivate() && !(m->mods & M_FINAL) &&
                       !((m->owner->mods & M_FINAL) && !m->owner->isInterface());
    if (!m->native.empty() && (!overridable || !virt))
    {
        std::vector<ASTNodePtr> a;
        if (recv)
            a.push_back(std::move(recv));
        for (auto &x : args)
            a.push_back(std::move(x));
        return q::call(m->native, std::move(a), l);
    }
    if (m->isStatic())
    {
        ClassSym *owner = m->owner;
        // code of the class (or a subclass) runs only once the class is initialized
        bool inside = curClass() && (curClass() == owner || isSubClass(curClass(), owner));
        ASTNodePtr c = q::call(m->impl, std::move(args), l);
        if (owner->needsClinit && !inside)
        {
            std::vector<ASTNodePtr> p;
            p.push_back(q::call(owner->clinitFn(), {}, l));
            p.push_back(std::move(c));
            return q::seq(std::move(p), l);
        }
        return c;
    }
    bool direct = !virt || m->isPrivate() || (m->mods & M_FINAL) || (m->owner->mods & M_FINAL && !m->owner->isInterface());
    if (direct && m->owner->isInterface() && !m->isDefault() && !m->isPrivate())
        direct = false;
    if (direct && !m->isAbstract())
    {
        std::vector<ASTNodePtr> a;
        a.push_back(std::move(recv));
        for (auto &x : args)
            a.push_back(std::move(x));
        // a direct call skips the NullPointerException of a virtual one
        if (!q::isSimple(a[0]) || !(a[0]->is<Identifier>() && a[0]->as<Identifier>().name == "__this"))
            a[0] = q::un(NONNULL, std::move(a[0]), l);
        return q::call(m->impl, std::move(a), l);
    }
    // interface default method: a lambda receiver uses the default body
    std::string fallback = m->owner->isInterface() && m->isDefault() ? m->impl : "";
    return q::inv(m->vkey, std::move(recv), std::move(args), l, fallback);
}

// ─── Method invocation ───────────────────────────────────────────────────────

Val Sema::call(const ExprP &e, const TypeP &target)
{
    int l = e->pos.line;
    Pos at = e->opPos.line ? e->opPos : e->pos;
    const std::string &mname = e->name;
    std::vector<MethodSym *> cands;
    ASTNodePtr recv;
    TypeP recvType;
    bool staticOnly = false, virt = true, implicitThis = false;
    ClassSym *foundIn = nullptr;
    ClassSym *searchClass = nullptr;
    std::vector<Val> lowered;

    if (e->superCall)
    {
        // super.m(...) / Iface.super.m(...): the superclass's / interface's body, non-virtually
        ClassSym *c = curClass();
        if (e->type)
        {
            TypeP it = resolveType(e->type);
            searchClass = it->k == TK::Class ? it->cls : nullptr;
            if (searchClass && !searchClass->isInterface())
            {
                // Outer.super.m(): the outer class's superclass
                recv = outerInstance(searchClass, e->pos);
                searchClass = searchClass->super ? searchClass->super->cls : nullptr;
            }
            else
                recv = thisNode(l);
        }
        else
        {
            if (inStaticContext())
            {
                error(e->pos, "non-static variable super cannot be referenced from a static context");
                return Val{q::nil(l), tError()};
            }
            searchClass = c && c->super ? c->super->cls : nullptr;
            recv = thisNode(l);
        }
        if (!searchClass)
            return Val{q::nil(l), tError()};
        collectMethods(searchClass, mname, cands);
        recvType = c ? c->super : nullptr;
        if (e->type)
        {
            // Iface.super.m(): the superinterface as the class declares it
            recvType = selfType(searchClass);
            if (c)
                for (auto &i : c->ifaces)
                    if (i && i->cls == searchClass)
                        recvType = i;
        }
        virt = false;
        foundIn = searchClass;
    }
    else if (!e->a)
    {
        // unqualified: the innermost enclosing class with a member of that name
        for (ClassSym *c = curClass(); c; c = c->outer)
        {
            collectMethods(c, mname, cands);
            if (!cands.empty())
            {
                foundIn = c;
                break;
            }
        }
        if (cands.empty())
        {
            for (auto &im : units_[unit_].imports)
            {
                if (!im.isStatic || (!im.onDemand && (im.name.empty() || im.name.back() != mname)))
                    continue;
                std::vector<std::string> typeParts(im.name.begin(), im.name.end() - (im.onDemand ? 0 : 1));
                if (ClassSym *tc = resolveTypeName(typeParts, im.pos, false))
                {
                    std::vector<MethodSym *> all;
                    collectMethods(tc, mname, all);
                    for (MethodSym *m : all)
                        if (m->isStatic())
                            cands.push_back(m);
                }
            }
            staticOnly = true;
        }
        else
            implicitThis = true;
    }
    else
    {
        Val base;
        if (e->a->k == EK::Name)
            base = name(e->a, false);
        else if (e->a->k == EK::Field)
            base = fieldAccess(e->a, false);
        else
            base = expr(e->a);
        if (base.isPackage())
        {
            error(e->a->pos, "package " + base.package + " does not exist");
            return Val{q::nil(l), tError()};
        }
        if (base.isType())
        {
            searchClass = base.typeSym;
            collectMethods(searchClass, mname, cands);
            staticOnly = true;
            foundIn = searchClass;
        }
        else
        {
            recvType = base.type;
            if (isError(recvType))
            {
                for (auto &a : e->args)
                    if (!isPoly(a))
                        expr(a);
                return Val{q::nil(l), tError()};
            }
            if (isPrim(recvType))
            {
                error(at, typeStr(recvType) + " cannot be dereferenced");
                return Val{q::nil(l), tError()};
            }
            if (recvType->k == TK::Void)
            {
                error(at, "void cannot be dereferenced");
                return Val{q::nil(l), tError()};
            }
            TypeP ub = upperBound(recvType);
            if (ub->k == TK::Array)
            {
                // arrays: clone() is public and returns the array type
                if (mname == "clone" && e->args.empty())
                {
                    Val r;
                    r.type = recvType;
                    std::vector<ASTNodePtr> a;
                    a.push_back(std::move(base.node));
                    r.node = q::call("__j_aclone", std::move(a), l);
                    return r;
                }
                searchClass = cls("java.lang.Object");
            }
            else if (ub->k == TK::Null)
            {
                error(at, "<null> cannot be dereferenced");
                return Val{q::nil(l), tError()};
            }
            else
                searchClass = ub->cls;
            collectMethods(searchClass, mname, cands);
            recv = std::move(base.node);
            recvType = ub;
            foundIn = searchClass;
        }
    }
    if (cands.empty())
    {
        for (auto &a : e->args)
            if (!isPoly(a))
                lowered.push_back(expr(a));
            else
                lowered.emplace_back();
        ClassSym *loc = foundIn ? foundIn : curClass();
        std::string where = loc ? std::string(loc->isInterface() ? "interface " : "class ") + typeStr(classType(loc)) : "class";
        if (recv && recvType && e->a && e->a->k == EK::Name)
            where = "variable " + e->a->name + " of type " + typeStr(recvType);
        error(at, "cannot find symbol",
              {"  symbol:   method " + mname + "(" + argTypesStr(*this, lowered, e->args) + ")", "  location: " + where});
        return Val{q::nil(l), tError()};
    }
    std::string notes;
    Resolved r = resolveCall(cands, e->args, recvType ? recvType : (foundIn ? selfType(foundIn) : nullptr), target, at,
                             mname, e->targs, lowered, notes);
    if (!r.m)
    {
        if (r.ambiguous)
            error(at, "reference to " + mname + " is ambiguous", {notes});
        else if (cands.size() == 1)
            error(at, "method " + mname + " in " + std::string(cands[0]->owner->isInterface() ? "interface " : "class ") +
                          typeStr(classType(cands[0]->owner)) + " cannot be applied to given types;",
                  {notes});
        else
            error(at, "no suitable method found for " + mname + "(" + argTypesStr(*this, lowered, e->args) + ")", {notes});
        return Val{q::nil(l), tError()};
    }
    MethodSym *m = r.m;
    // access
    if (m->isPrivate() && curClass() && topLevelOf(curClass()) != topLevelOf(m->owner))
        error(at, sigStr(m) + " has private access in " + typeStr(classType(m->owner)));
    // the checked exceptions it declares
    for (auto &t : m->throws)
        thrown(subst(t, r.subst), at);
    if (!m->isStatic())
    {
        if (staticOnly)
        {
            error(at, "non-static method " + sigStr(m) + " cannot be referenced from a static context");
            return Val{q::nil(l), r.ret ? r.ret : tError()};
        }
        if (implicitThis)
        {
            if (foundIn == curClass() && inStaticContext())
            {
                error(at, "non-static method " + sigStr(m) + " cannot be referenced from a static context");
                return Val{q::nil(l), r.ret ? r.ret : tError()};
            }
            recv = outerInstance(foundIn, e->pos);
            recvType = selfType(foundIn);
        }
    }
    else if (recv && !q::isSimple(recv))
    {
        // obj.staticMethod(): obj is evaluated and discarded
        std::vector<ASTNodePtr> args = lowerArgs(r, e->args, lowered, at);
        ASTNodePtr c = invoke(m, nullptr, std::move(args), false, l);
        std::vector<ASTNodePtr> p;
        p.push_back(std::move(recv));
        p.push_back(std::move(c));
        Val v;
        v.type = r.ret;
        v.node = q::seq(std::move(p), l);
        return v;
    }
    else
        recv = nullptr;
    // getClass() is final in Object: java.lang.Class.$of(receiver)
    if (m->name == "getClass" && m->params.empty() && m->owner->binaryName == "java.lang.Object")
    {
        ClassSym *k = cls("java.lang.Class");
        needNamed("java.lang.Class", "$of");
        Val v;
        v.type = classType(k, {recvType ? erasure(recvType) : tObject()});
        std::vector<ASTNodePtr> a;
        a.push_back(recv ? std::move(recv) : thisNode(l));
        v.node = q::call("java.lang.Class.$of", std::move(a), l);
        return v;
    }
    Val out;
    out.type = r.ret ? r.ret : tError();
    if (out.type->k == TK::Wildcard || out.type->k == TK::TypeVar)
    {
        // capture: a wildcard or a type variable of the receiver
        if (out.type->k == TK::Wildcard)
            out.type = upperBound(out.type);
    }
    // a raw receiver erases the result
    if (recvType && recvType->k == TK::Class && recvType->args.empty() && recvType->cls && !recvType->cls->tparams.empty() &&
        !m->isStatic())
        out.type = erasure(out.type);
    std::vector<Val> argVals; // the arguments' own static types (library lowering formats by them)
    for (size_t i = 0; i < e->args.size(); ++i)
    {
        Val tv;
        if (!isPoly(e->args[i]) && i < lowered.size() && lowered[i].type)
            tv.type = lowered[i].type;
        else if (i < r.paramTypes.size())
            tv.type = r.paramTypes[i];
        argVals.push_back(std::move(tv));
    }
    std::vector<ASTNodePtr> args = lowerArgs(r, e->args, lowered, at);
    bool polyCall = !m->tparams.empty() && e->targs.empty() && mentionsVars(m->ret, m->tparams);
    Val lib;
    if (libraryCall(m, args, recv, argVals, recvType, l, lib))
    {
        if (!lib.type)
            lib.type = out.type;
        lib.polyCall = polyCall;
        return lib;
    }
    out.node = invoke(m, std::move(recv), std::move(args), virt, l, recvType);
    out.polyCall = polyCall;
    return out;
}

// ─── Object creation ─────────────────────────────────────────────────────────

ASTNodePtr Sema::construct(ClassSym *c, MethodSym *ctor, std::vector<ASTNodePtr> args, ASTNodePtr outer, int l)
{
    need(c);
    needMethod(ctor);
    std::vector<ASTNodePtr> a;
    std::vector<ASTNodePtr> alloc;
    alloc.push_back(q::id(c->binaryName, l));
    a.push_back(q::call("__j_new", std::move(alloc), l));
    if (c->hasOuterInstance)
        a.push_back(outer ? std::move(outer) : q::nil(l));
    // captured locals of a local / anonymous class
    for (auto &cap : c->captures)
    {
        FnCtx *owner = nullptr;
        LocalVar *v = findLocal(cap.name, &owner);
        bool fromEnclosing = false;
        for (FnCtx *f = c->enclosingFn; f && v; f = f->outer)
            if (f == owner)
                fromEnclosing = true;
        if (v && fromEnclosing)
            a.push_back(q::id(v->q, l));
        else
        {
            // created inside the class itself (or a class nested in it): pass its own copy on
            a.push_back(q::getf(outerInstance(c, Pos{l, 0}), cap.field, l));
        }
    }
    for (auto &x : args)
        a.push_back(std::move(x));
    ASTNodePtr init = q::call(ctor->impl, std::move(a), l);
    // the first class up the superclass chain with static initialization
    // (its <clinit> initializes its own superclasses first)
    ClassSym *initCls = nullptr;
    for (ClassSym *k = c; k && !initCls; k = k->super ? k->super->cls : nullptr)
        if (k->needsClinit)
            initCls = k;
    bool inside = curClass() && initCls && (curClass() == initCls || isSubClass(curClass(), initCls));
    if (initCls && !inside)
    {
        std::vector<ASTNodePtr> p;
        p.push_back(q::call(initCls->clinitFn(), {}, l));
        p.push_back(std::move(init));
        return q::seq(std::move(p), l);
    }
    return init;
}

Val Sema::newObject(const ExprP &e, const TypeP &target)
{
    int l = e->pos.line;
    ASTNodePtr outerNode;
    TypeP t;
    if (e->c)
    {
        // outer.new Inner(...)
        Val o = expr(e->c);
        if (isError(o.type) || !o.type->cls)
            return Val{q::nil(l), tError()};
        ClassSym *inner = memberType(o.type->cls, e->type->parts.back().name);
        if (!inner)
        {
            error(e->type->pos, "cannot find symbol", {"  symbol: class " + e->type->parts.back().name});
            return Val{q::nil(l), tError()};
        }
        std::vector<TypeP> targs;
        for (auto &a : e->type->parts.back().args)
            targs.push_back(resolveType(a));
        t = classType(inner, targs);
        outerNode = std::move(o.node);
    }
    else
        t = resolveType(e->type);
    if (isError(t))
    {
        for (auto &a : e->args)
            if (!isPoly(a))
                expr(a);
        return Val{q::nil(l), tError()};
    }
    if (t->k == TK::TypeVar)
    {
        error(e->type->pos, "unexpected type", {"  required: class", "  found:    type parameter " + typeStr(t)});
        return Val{q::nil(l), tError()};
    }
    ClassSym *c = t->cls;
    bool diamond = e->type && !e->type->parts.empty() && e->type->parts.back().hasArgs && e->type->parts.back().args.empty();
    enterMembers(c);
    assignKeys(c);
    // anonymous class
    if (e->body)
    {
        ClassSym *anon = nullptr;
        for (auto &kv : anonClasses_)
            if (kv.first == e->body.get())
                anon = kv.second;
        if (!anon)
        {
            ClassSym *host = curClass();
            ClassSym *top = topLevelOf(host);
            e->body->name = "";
            anon = enterClass(e->body.get(), host, unit_, host ? host->pkg : "");
            anon->binaryName = top->binaryName + "$" + std::to_string(++top->anonCount);
            byBinary_[anon->binaryName] = anon;
            anon->isAnon = true;
            anon->enclosingFn = fn_;
            anon->hasOuterInstance = !inStaticContext();
            anonClasses_.push_back({e->body.get(), anon});
            anon->state = 1;
            if (c->isInterface())
            {
                anon->super = tObject();
                anon->ifaces.push_back(diamond ? classType(c) : t);
            }
            else
                anon->super = diamond ? classType(c) : t;
            anon->state = 2;
            // locals the body may read, captured at creation
            for (FnCtx *f = fn_; f; f = f->outer)
                for (auto &sc : f->scopes)
                    for (auto &v : sc.vars)
                        if (v.q.find("$p") == std::string::npos || true)
                        {
                            bool dup = false;
                            for (auto &cap : anon->captures)
                                dup = dup || cap.name == v.name;
                            if (!dup)
                                anon->captures.push_back(Capture{v.name, v.type, "val$" + v.name});
                        }
        }
        // the superclass constructor the anonymous class's constructor calls
        MethodSym *superCtor = nullptr;
        std::vector<Val> lowered;
        Resolved r;
        if (!c->isInterface())
        {
            std::string notes;
            std::vector<MethodSym *> ctors = c->ctors;
            r = resolveCall(ctors, e->args, t, target, e->pos, c->name, {}, lowered, notes);
            if (!r.m)
            {
                error(e->pos, "constructor " + c->name + " in class " + typeStr(classType(c)) + " cannot be applied to given types;", {notes});
                return Val{q::nil(l), tError()};
            }
            superCtor = r.m;
            for (auto &t : r.m->throws)
                thrown(subst(t, r.subst), e->pos);
        }
        else if (!e->args.empty())
        {
            error(e->pos, "anonymous class implements interface; cannot have arguments");
            return Val{q::nil(l), tError()};
        }
        if (!anon->lowered)
        {
            anonSuperCtor_[anon] = superCtor;
            if (superCtor)
                anonSuperParams_[anon] = r.paramTypes;
            enterMembers(anon);
            assignKeys(anon);
            checkClass(anon);
            lowerClass(anon);
        }
        std::vector<ASTNodePtr> args;
        if (superCtor)
            args = lowerArgs(r, e->args, lowered, e->pos);
        MethodSym *ctor = anon->ctors.empty() ? nullptr : anon->ctors[0];
        Val v;
        v.type = classType(anon);
        if (!ctor)
            return Val{q::nil(l), tError()};
        // outer instance of the superclass, when it is an inner class
        ASTNodePtr superOuter;
        if (c->hasOuterInstance)
            superOuter = outerNode ? std::move(outerNode) : outerInstance(c->outer, e->pos);
        if (superOuter)
            args.insert(args.begin(), std::move(superOuter));
        v.node = construct(anon, ctor, std::move(args), anon->hasOuterInstance ? thisNode(l) : nullptr, l);
        return v;
    }
    if (c->isInterface() || (c->mods & M_ABSTRACT))
    {
        error(e->pos, typeStr(classType(c)) + " is abstract; cannot be instantiated");
        for (auto &a : e->args)
            if (!isPoly(a))
                expr(a);
        return Val{q::nil(l), tError()};
    }
    if (c->kind == ClassDecl::Enum)
    {
        error(e->pos, "enum classes may not be instantiated");
        return Val{q::nil(l), tError()};
    }
    // library types with their own representation (String, boxes, ...)
    {
        Val lib;
        std::vector<Val> lowered;
        for (auto &a : e->args)
            lowered.push_back(isPoly(a) ? Val{} : expr(a));
        auto bn = c->binaryName;
        if (bn == "java.lang.String" || bn == "java.lang.Integer" || bn == "java.lang.Double" ||
            bn == "java.lang.Boolean" || bn == "java.lang.Float")
        {
            std::vector<ASTNodePtr> args;
            std::string notes;
            std::vector<MethodSym *> ctors = c->ctors;
            Resolved r = resolveCall(ctors, e->args, t, target, e->pos, c->name, {}, lowered, notes);
            if (!r.m)
            {
                error(e->pos, "no suitable constructor found for " + c->name + "(" + argTypesStr(*this, lowered, e->args) + ")", {notes});
                return Val{q::nil(l), tError()};
            }
            args = lowerArgs(r, e->args, lowered, e->pos);
            Val v;
            v.type = t;
            if (bn == "java.lang.String")
                v.node = q::call("__jn_String_new", std::move(args), l);
            else
            {
                // new Integer(5) / new Integer("5"): the value (through valueOf)
                std::vector<MethodSym *> vo;
                collectMethods(c, "valueOf", vo);
                std::vector<Val> again;
                Resolved r2 = resolveCall(vo, e->args, nullptr, nullptr, e->pos, "valueOf", {}, again, notes);
                if (!r2.m)
                    return Val{q::nil(l), t};
                std::vector<ASTNodePtr> a2 = lowerArgs(r2, e->args, again, e->pos);
                ASTNodePtr none;
                Val lv;
                std::vector<Val> types;
                for (auto &p : r2.paramTypes)
                {
                    Val tv;
                    tv.type = p;
                    types.push_back(std::move(tv));
                }
                if (!libraryCall(r2.m, a2, none, types, nullptr, l, lv))
                    lv.node = invoke(r2.m, nullptr, std::move(a2), false, l);
                v.node = std::move(lv.node);
            }
            return v;
        }
        // keep the attributed arguments for the general path
        std::string notes;
        std::vector<MethodSym *> ctors = c->ctors;
        Resolved r = resolveCall(ctors, e->args, t, target, e->pos, c->name, {}, lowered, notes);
        if (!r.m)
        {
            if (r.ambiguous)
                error(e->pos, "reference to " + c->name + " is ambiguous", {notes});
            else if (ctors.size() == 1)
                error(e->pos, "constructor " + c->name + " in " + std::string(c->kind == ClassDecl::Enum ? "enum " : c->kind == ClassDecl::Record ? "record " : "class ") +
                                  typeStr(classType(c)) + " cannot be applied to given types;",
                      {notes});
            else
                error(e->pos, "no suitable constructor found for " + c->name + "(" + argTypesStr(*this, lowered, e->args) + ")", {notes});
            return Val{q::nil(l), tError()};
        }
        if (r.m->isPrivate() && curClass() && topLevelOf(curClass()) != topLevelOf(c))
            error(e->pos, sigStr(r.m) + " has private access in " + typeStr(classType(c)));
        for (auto &t : r.m->throws)
            thrown(subst(t, r.subst), e->pos);
        std::vector<ASTNodePtr> args = lowerArgs(r, e->args, lowered, e->pos);
        Val v;
        if (diamond || (t->args.empty() && !c->tparams.empty() && target && target->k == TK::Class))
        {
            std::vector<TypeP> targs;
            for (auto *tv : c->tparams)
            {
                auto it = r.subst.find(tv);
                targs.push_back(it != r.subst.end() && it->second ? it->second : tObject());
            }
            v.type = diamond ? classType(c, targs) : t;
        }
        else
            v.type = t;
        if (c->hasOuterInstance && !outerNode)
        {
            if (inStaticContext() && !(curClass() && curClass() != c->outer && isSubClass(curClass(), c->outer)))
            {
                bool reachable = false;
                for (ClassSym *k = curClass(); k; k = k->hasOuterInstance ? k->outer : nullptr)
                    if (k == c->outer || isSubClass(k, c->outer))
                        reachable = !inStaticContext();
                if (!reachable)
                {
                    error(e->pos, "non-static variable this cannot be referenced from a static context");
                    return Val{q::nil(l), v.type};
                }
            }
            outerNode = outerInstance(c->outer, e->pos);
        }
        if (c->isLocal && c->enclosingFn && !c->enclosingFn->isStatic && !outerNode)
            outerNode = thisNode(l);
        v.node = construct(c, r.m, std::move(args), std::move(outerNode), l);
        v.polyCall = diamond && !c->tparams.empty();
        return v;
    }
}

// ─── Lambdas ─────────────────────────────────────────────────────────────────

Val Sema::lambda(const ExprP &e, const TypeP &target)
{
    int l = e->pos.line;
    TypeP ft = target ? upperBound(target) : nullptr;
    MethodSym *sam = nullptr;
    Subst fs;
    if (ft && ft->k == TK::Class && ft->cls)
    {
        sam = functionalMethod(ft->cls);
        if (!sam)
        {
            error(e->pos, "incompatible types: " + typeStr(target) + " is not a functional interface");
            return Val{q::nil(l), tError()};
        }
        if (TypeP view = asSuper(ft, sam->owner))
            fs = fiSubst(view);
    }
    else if (ft && !isError(ft))
    {
        error(e->pos, "incompatible types: " + typeStr(ft) + " is not a functional interface");
        return Val{q::nil(l), tError()};
    }
    else if (!ft)
    {
        error(e->pos, "lambda expression not expected here");
        return Val{q::nil(l), tError()};
    }
    if (sam && sam->params.size() != e->params.size())
    {
        error(e->pos, "incompatible types: incompatible parameter types in lambda expression");
        return Val{q::nil(l), tError()};
    }
    FnCtx ctx;
    ctx.cls = curClass();
    ctx.method = nullptr;
    ctx.isStatic = inStaticContext();
    ctx.isLambda = true;
    ctx.outer = fn_;
    ctx.name = fn_ ? fn_->name : "lambda";
    TypeP ret = sam ? subst(sam->ret, fs) : tError();
    // a return type still mentioning inference variables is left open
    std::function<bool(const TypeP &)> open = [&](const TypeP &t) -> bool {
        if (!t)
            return false;
        if (t->k == TK::TypeVar)
            return t->tv && t->tv->ofMethod ? true : false;
        if (t->k == TK::Array || t->k == TK::Wildcard)
            return open(t->elem);
        for (auto &a : t->args)
            if (open(a))
                return true;
        return false;
    };
    std::vector<TypeP> collected;
    // an inference probe collects what this lambda returns (not nested ones)
    std::vector<TypeP> *override = lambdaReturnsOverride_;
    lambdaReturnsOverride_ = nullptr;
    struct RestoreOverride
    {
        std::vector<TypeP> *&slot;
        std::vector<TypeP> *value;
        ~RestoreOverride() { slot = value; }
    } restoreOverride{lambdaReturnsOverride_, override};
    if (override || open(ret))
    {
        ctx.lambdaReturns = override ? override : &collected;
        ret = nullptr;
    }
    if (ret && ret->k == TK::Wildcard)
        ret = upperBound(ret);
    ctx.ret = ret;
    std::vector<std::string> params;
    FnCtx *saved = fn_;
    fn_ = &ctx;
    ctx.scopes.emplace_back();
    for (size_t i = 0; i < e->params.size(); ++i)
    {
        TypeP pt;
        if (e->params[i].type && e->params[i].type->kind != TypeRef::Var)
            pt = resolveType(e->params[i].type);
        else if (sam)
        {
            pt = subst(sam->params[i], fs);
            // a method type variable still being inferred stands for its bound
            // (one of an enclosing method being lowered is a proper type)
            if (pt && pt->k == TK::TypeVar && pt->tv->ofMethod &&
                std::find(methodTvars_.begin(), methodTvars_.end(), pt->tv) == methodTvars_.end())
                pt = upperBound(pt);
        }
        else
            pt = tError();
        if (findLocalOuter(e->params[i].name))
            error(e->params[i].pos, "variable " + e->params[i].name + " is already defined in method " +
                                        (saved && saved->method ? sigStr(saved->method) : std::string("main(String[])")));
        LocalVar &v = declareLocal(e->params[i].name, pt, e->params[i].pos, false);
        v.hasInit = true;
        markAssigned(&v);
        v.q = e->params[i].name == "_" ? "__unused" + std::to_string(i) : e->params[i].name;
        params.push_back(v.q);
    }
    ASTNodePtr body;
    if (e->a)
    {
        bool voidSam = ret && ret->k == TK::Void;
        Val v = expr(e->a, ret && ret->k != TK::Void ? ret : nullptr);
        std::vector<ASTNodePtr> stmts;
        if (voidSam || (!ret && !ctx.lambdaReturns))
        {
            if (voidSam)
            {
                EK k = e->a->k;
                bool stmtExpr = k == EK::Call || k == EK::Assign || k == EK::New || k == EK::PreIncDec || k == EK::PostIncDec;
                if (!stmtExpr && !isError(v.type))
                    error(e->a->pos, "incompatible types: bad return type in lambda expression",
                          {"    " + typeStr(v.type) + " cannot be converted to void"});
            }
            stmts.push_back(q::exprStmt(std::move(v.node), l));
        }
        else if (ctx.lambdaReturns)
        {
            ctx.lambdaReturns->push_back(v.type);
            stmts.push_back(q::ret(std::move(v.node), l));
        }
        else
        {
            if (v.type && v.type->k == TK::Void)
                error(e->a->pos, "incompatible types: bad return type in lambda expression", {"    void cannot be converted to " + typeStr(ret)});
            stmts.push_back(q::ret(coerce(std::move(v), ret, e->a->pos), l));
        }
        body = lowerFunctionBody(ctx, [&](std::vector<ASTNodePtr> &out) {
            for (auto &s : stmts)
                out.push_back(std::move(s));
        }, l);
    }
    else
    {
        body = lowerFunctionBody(ctx, [&](std::vector<ASTNodePtr> &out) {
            for (auto &s : e->block->stmts)
                if (ASTNodePtr n = stmt(s))
                    out.push_back(std::move(n));
        }, l);
    }
    fn_ = saved;
    Val r;
    r.type = target;
    r.node = q::lambda(params, std::move(body), l);
    if (!ctx.captured.empty())
    {
        // Java captures values: each evaluation of the lambda gets its own
        // copies, ((x, y) -> lambda)(x, y), not the enclosing variables
        std::vector<ASTNodePtr> args, inner;
        for (auto &c : ctx.captured)
            args.push_back(q::id(c, l));
        inner.push_back(q::ret(std::move(r.node), l));
        r.node = q::call(q::lambda(ctx.captured, q::block(std::move(inner), l), l), std::move(args), l);
    }
    return r;
}

// ─── Method references ───────────────────────────────────────────────────────

Val Sema::methodRef(const ExprP &e, const TypeP &target)
{
    int l = e->pos.line;
    TypeP ft = target ? upperBound(target) : nullptr;
    if (!ft || ft->k != TK::Class || !ft->cls)
    {
        if (!ft || !isError(ft))
            error(e->pos, "method reference not expected here");
        return Val{q::nil(l), tError()};
    }
    MethodSym *sam = functionalMethod(ft->cls);
    if (!sam)
    {
        error(e->pos, "incompatible types: " + typeStr(target) + " is not a functional interface");
        return Val{q::nil(l), tError()};
    }
    TypeP view = asSuper(ft, sam->owner);
    Subst fs = view ? substOf(view) : Subst{};
    size_t n = sam->params.size();
    // synthesize the equivalent lambda: (p0, p1, ...) -> <call>
    auto lam = std::make_shared<Expr>();
    lam->k = EK::Lambda;
    lam->pos = e->pos;
    std::string base = "__mr" + std::to_string(++tempCounter_) + "_";
    for (size_t i = 0; i < n; ++i)
    {
        LambdaParam p;
        p.name = base + std::to_string(i);
        p.pos = e->pos;
        lam->params.push_back(p);
    }
    auto paramName = [&](size_t i) {
        auto x = std::make_shared<Expr>();
        x->k = EK::Name;
        x->pos = e->pos;
        x->name = base + std::to_string(i);
        return x;
    };
    auto typeExpr = [&](ClassSym *c) {
        auto x = std::make_shared<Expr>();
        x->k = EK::Name;
        x->pos = e->pos;
        x->name = std::string("\x01") + c->binaryName; // resolves to the class itself
        return x;
    };
    std::vector<ASTNodePtr> prelude;
    ExprP body;
    if (e->name == "new")
    {
        TypeP t = e->type ? resolveType(e->type) : nullptr;
        if (!t && e->a)
        {
            Val q0 = e->a->k == EK::Name ? name(e->a, false) : fieldAccess(e->a, false);
            t = q0.isType() ? classType(q0.typeSym) : tError();
        }
        if (isError(t))
            return Val{q::nil(l), tError()};
        if (t->k == TK::Array)
        {
            auto na = std::make_shared<Expr>();
            na->k = EK::NewArray;
            na->pos = e->pos;
            // element base type and dimensions
            TypeP elemT = t;
            int dims = 0;
            while (elemT->k == TK::Array)
            {
                elemT = elemT->elem;
                ++dims;
            }
            auto tr = std::make_shared<TypeRef>();
            if (isPrim(elemT))
            {
                tr->kind = TypeRef::Prim;
                tr->prim = typeStr(elemT);
            }
            else
            {
                tr->kind = TypeRef::Named;
                TypeRef::Part p;
                p.name = std::string("\x01") + elemT->cls->binaryName;
                tr->parts.push_back(p);
            }
            na->type = tr;
            na->args.push_back(paramName(0));
            na->extraDims = dims - 1;
            body = na;
        }
        else
        {
            auto ne = std::make_shared<Expr>();
            ne->k = EK::New;
            ne->pos = e->pos;
            auto tr = std::make_shared<TypeRef>();
            tr->kind = TypeRef::Named;
            TypeRef::Part p;
            p.name = std::string("\x01") + t->cls->binaryName;
            if (!t->cls->tparams.empty() && t->args.empty())
                p.hasArgs = true; // diamond
            tr->parts.push_back(p);
            ne->type = tr;
            for (size_t i = 0; i < n; ++i)
                ne->args.push_back(paramName(i));
            body = ne;
        }
    }
    else
    {
        // the qualifier: a type, super, or an expression evaluated once now
        ClassSym *typeQual = nullptr;
        ExprP recvExpr;
        if (e->superCall)
        {
            auto c = std::make_shared<Expr>();
            c->k = EK::Call;
            c->superCall = true;
            c->name = e->name;
            c->pos = e->pos;
            for (size_t i = 0; i < n; ++i)
                c->args.push_back(paramName(i));
            body = c;
        }
        else
        {
            if (e->type)
            {
                TypeP t = resolveType(e->type);
                typeQual = t->k == TK::Class ? t->cls : nullptr;
                if (t->k == TK::Array)
                    typeQual = cls("java.lang.Object");
            }
            else if (e->a->k == EK::Name || e->a->k == EK::Field)
            {
                size_t savedDiags = diags_.size();
                Val q0 = e->a->k == EK::Name ? name(e->a, false) : fieldAccess(e->a, false);
                if (q0.isType())
                    typeQual = q0.typeSym;
                else
                {
                    diags_.resize(savedDiags);
                    recvExpr = e->a;
                }
            }
            else
                recvExpr = e->a;
            if (typeQual)
            {
                // static: T.m(p0..), else unbound receiver: p0.m(p1..)
                std::vector<MethodSym *> all;
                collectMethods(typeQual, e->name, all);
                bool staticFits = false, instanceFits = false;
                for (MethodSym *m : all)
                {
                    if (m->isStatic() && (m->params.size() == n || (m->varargs && n + 1 >= m->params.size())))
                        staticFits = true;
                    if (!m->isStatic() && n >= 1 && (m->params.size() == n - 1 || (m->varargs && n >= m->params.size())))
                        instanceFits = true;
                }
                auto c = std::make_shared<Expr>();
                c->k = EK::Call;
                c->name = e->name;
                c->pos = e->pos;
                if (staticFits && !instanceFits)
                {
                    c->a = typeExpr(typeQual);
                    for (size_t i = 0; i < n; ++i)
                        c->args.push_back(paramName(i));
                }
                else if (instanceFits && [&] {
                             Subst ffs = view ? fiSubst(view) : Subst{};
                             TypeP p0 = n >= 1 ? subst(sam->params[0], ffs) : nullptr;
                             return p0 && p0->k == TK::Class && asSuper(p0, typeQual) != nullptr;
                         }())
                {
                    // the receiver parameter already has the qualifying type
                    c->a = paramName(0);
                    for (size_t i = 1; i < n; ++i)
                        c->args.push_back(paramName(i));
                }
                else if (instanceFits)
                {
                    // the receiver parameter is cast to the qualifying type
                    auto cast = std::make_shared<Expr>();
                    cast->k = EK::Cast;
                    cast->pos = e->pos;
                    auto tr = std::make_shared<TypeRef>();
                    tr->kind = TypeRef::Named;
                    TypeRef::Part p;
                    p.name = std::string("\x01") + typeQual->binaryName;
                    tr->parts.push_back(p);
                    cast->type = tr;
                    cast->a = paramName(0);
                    cast->parenthesized = true;
                    c->a = cast;
                    for (size_t i = 1; i < n; ++i)
                        c->args.push_back(paramName(i));
                }
                else
                {
                    error(e->pos, "invalid method reference", {"  cannot find symbol", "    symbol:   method " + e->name + "()", "    location: class " + typeQual->name});
                    return Val{q::nil(l), tError()};
                }
                body = c;
            }
            else
            {
                // bound receiver: evaluated once, when the reference is created
                Val rv = expr(recvExpr);
                if (isError(rv.type))
                    return Val{q::nil(l), tError()};
                std::string tmp = base + "recv";
                LocalVar &lv = declareLocal(tmp, rv.type, e->pos, true);
                lv.hasInit = true;
                markAssigned(&lv);
                fn_->temps.push_back(tmp);
                prelude.push_back(q::assign(q::id(tmp, l), q::un(NONNULL, std::move(rv.node), l), l));
                auto c = std::make_shared<Expr>();
                c->k = EK::Call;
                c->name = e->name;
                c->pos = e->pos;
                auto rn = std::make_shared<Expr>();
                rn->k = EK::Name;
                rn->pos = e->pos;
                rn->name = tmp;
                c->a = rn;
                for (size_t i = 0; i < n; ++i)
                    c->args.push_back(paramName(i));
                body = c;
            }
        }
    }
    lam->a = body;
    Val v = lambda(lam, target);
    if (!prelude.empty())
    {
        prelude.push_back(std::move(v.node));
        v.node = q::seq(std::move(prelude), l);
    }
    return v;
}

// A Name "\x01<binary name>" (from a synthesized method reference) is the class itself.
LocalVar *Sema::findLocalOuter(const std::string &n)
{
    for (FnCtx *f = fn_ ? fn_->outer : nullptr; f; f = f->outer)
        for (size_t i = f->scopes.size(); i-- > 0;)
            for (auto &v : f->scopes[i].vars)
                if (v.name == n)
                    return &v;
    if (fn_)
        for (size_t i = fn_->scopes.size(); i-- > 0;)
            for (auto &v : fn_->scopes[i].vars)
                if (v.name == n)
                    return &v;
    return nullptr;
}

} // namespace java
