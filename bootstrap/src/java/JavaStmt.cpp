// Java statements: lowering, with the flow analysis javac does on the way —
// reachability (§14.22), definite assignment (§16, simplified) and checked
// exceptions (§11.2).
#include "JavaSema.h"

#include <algorithm>

namespace java
{

using namespace jop;

// ─── Flow helpers ────────────────────────────────────────────────────────────

Flow Sema::joinFlow(const Flow &a, const Flow &b)
{
    if (!a.alive)
        return b;
    if (!b.alive)
        return a;
    Flow r;
    r.alive = true;
    size_t n = std::max(a.assigned.size(), b.assigned.size());
    r.assigned.assign(n, 0);
    for (size_t i = 0; i < n; ++i)
        r.assigned[i] = (i < a.assigned.size() && a.assigned[i]) && (i < b.assigned.size() && b.assigned[i]);
    return r;
}

void Sema::markAssigned(LocalVar *v)
{
    if (!v || !v->fn)
        return;
    Flow &f = v->fn->flow;
    if ((size_t)v->index >= f.assigned.size())
        f.assigned.resize(v->index + 1, 0);
    f.assigned[v->index] = 1;
}

bool Sema::isAssigned(LocalVar *v, FnCtx *owner)
{
    if (!v || !owner)
        return true;
    const Flow &f = owner->flow;
    if (!f.alive)
        return true;
    return (size_t)v->index < f.assigned.size() && f.assigned[v->index];
}

bool Sema::constTrue(const ExprP &e)
{
    if (!e)
        return true; // for (;;)
    size_t saved = diags_.size();
    FnCtx scratch;
    if (fn_)
    {
        scratch.cls = fn_->cls;
        scratch.isStatic = fn_->isStatic;
        scratch.outer = fn_;
    }
    scratch.scopes.emplace_back();
    FnCtx *s = fn_;
    fn_ = &scratch;
    ++probing_;
    Val v = expr(e);
    --probing_;
    fn_ = s;
    diags_.resize(saved);
    return v.cv.k == Const::Bool && v.cv.b;
}

bool Sema::constFalse(const ExprP &e)
{
    if (!e)
        return false;
    size_t saved = diags_.size();
    FnCtx scratch;
    if (fn_)
    {
        scratch.cls = fn_->cls;
        scratch.isStatic = fn_->isStatic;
        scratch.outer = fn_;
    }
    scratch.scopes.emplace_back();
    FnCtx *s = fn_;
    fn_ = &scratch;
    ++probing_;
    Val v = expr(e);
    --probing_;
    fn_ = s;
    diags_.resize(saved);
    return v.cv.k == Const::Bool && !v.cv.b;
}

// ─── Checked exceptions ──────────────────────────────────────────────────────

bool Sema::isChecked(const TypeP &t)
{
    if (!t || t->k != TK::Class || !t->cls)
        return false;
    ClassSym *thr = cls("java.lang.Throwable"), *rte = cls("java.lang.RuntimeException"), *err = cls("java.lang.Error");
    return thr && isSubClass(t->cls, thr) && !(rte && isSubClass(t->cls, rte)) && !(err && isSubClass(t->cls, err));
}

void Sema::thrown(const TypeP &t, Pos pos)
{
    if (!fn_ || !t || isError(t) || !isChecked(t) || !fn_->checkExceptions)
        return;
    for (size_t k = fn_->excScopes.size(); k-- > 0;)
    {
        ExcScope &sc = fn_->excScopes[k];
        bool caught = false;
        for (size_t i = 0; i < sc.catches.size(); ++i)
            for (auto &c : sc.catches[i])
            {
                if (isSubtype(t, c))
                {
                    sc.thrown[i] = true;
                    sc.caught[i].push_back(t);
                    caught = true;
                    break;
                }
                if (isSubtype(c, t))
                    sc.thrown[i] = true; // a subclass of what may be thrown
            }
        if (caught)
            return;
    }
    for (auto &a : fn_->throwsAllowed)
        if (isSubtype(t, a))
            return;
    error(pos, "unreported exception " + typeStr(t) + "; must be caught or declared to be thrown");
}

// ─── Blocks and simple statements ────────────────────────────────────────────

ASTNodePtr Sema::block(const std::vector<StmtP> &stmts, int l, bool newScope)
{
    if (newScope)
        pushScope();
    std::vector<ASTNodePtr> out;
    bool reportedUnreachable = false;
    for (auto &s : stmts)
    {
        if (!fn_->flow.alive && !reportedUnreachable && s->k != SK::Empty && s->k != SK::LocalClass)
        {
            error(s->pos, "unreachable statement");
            reportedUnreachable = true;
            fn_->flow.alive = true; // keep checking what follows
        }
        if (s->k == SK::LocalVar)
        {
            localVar(*s, out);
            continue;
        }
        if (ASTNodePtr n = stmt(s))
            out.push_back(std::move(n));
    }
    if (newScope)
        popScope();
    return q::block(std::move(out), l);
}

void Sema::localVar(const Stmt &s, std::vector<ASTNodePtr> &out)
{
    bool isFinal = s.mods.has(M_FINAL);
    bool isVar = s.type && s.type->kind == TypeRef::Var;
    TypeP base = isVar ? nullptr : resolveType(s.type);
    if (base && base->k == TK::Void)
    {
        error(s.type->pos, "'void' type not allowed here");
        base = tError();
    }
    for (auto &v : s.vars)
    {
        int l = v.pos.line;
        TypeP t = base;
        if (t)
            for (int k = 0; k < v.dims; ++k)
                t = arrayOf(t);
        // a local may not redeclare one in scope (javac's wording)
        if (LocalVar *prev = findLocal(v.name))
        {
            (void)prev;
            std::string where = fn_->method ? (fn_->method->isCtor ? "constructor " : "method ") + sigStr(fn_->method)
                                            : std::string("method main(String[])");
            if (fn_->method && fn_->method->isCtor)
                where = "constructor " + sigStr(fn_->method);
            error(v.pos, "variable " + v.name + " is already defined in " + where);
        }
        ASTNodePtr init;
        Const cv;
        if (v.init)
        {
            if (isVar)
            {
                if (v.init->k == EK::ArrayLit)
                {
                    error(v.pos, "cannot infer type for local variable " + v.name, {"  (array initializer needs an explicit target-type)"});
                    t = tError();
                }
                else if (v.init->k == EK::Lambda || v.init->k == EK::MethodRef)
                {
                    error(v.pos, "cannot infer type for local variable " + v.name,
                          {std::string("  (") + (v.init->k == EK::Lambda ? "lambda expression" : "method reference") + " needs an explicit target-type)"});
                    t = tError();
                }
                else
                {
                    Val iv = expr(v.init);
                    if (iv.type && iv.type->k == TK::Null)
                    {
                        error(v.pos, "cannot infer type for local variable " + v.name, {"  (variable initializer is 'null')"});
                        t = tError();
                    }
                    else if (iv.type && iv.type->k == TK::Void)
                    {
                        error(v.pos, "cannot infer type for local variable " + v.name, {"  (variable initializer is 'void')"});
                        t = tError();
                    }
                    else
                        t = iv.type && iv.type->k == TK::Wildcard ? upperBound(iv.type) : iv.type;
                    init = std::move(iv.node);
                    cv = iv.cv;
                }
            }
            else if (v.init->k == EK::ArrayLit)
            {
                if (t->k != TK::Array)
                {
                    error(v.init->pos, "illegal initializer for " + typeStr(t));
                    init = q::nil(l);
                }
                else
                    init = arrayLit(v.init, t).node;
            }
            else
            {
                Val iv = expr(v.init, t);
                cv = iv.cv;
                init = coerce(std::move(iv), t, v.init->pos);
                if (cv.ok() && isPrim(t))
                {
                    // the constant as the variable's type
                    if (init && (init->is<NumberLiteral>() || init->is<CallExpr>() || init->is<BoolLiteral>()))
                    {
                    }
                }
            }
        }
        else if (isVar)
        {
            error(v.pos, "cannot infer type for local variable " + v.name, {"  (cannot use 'var' on variable without initializer)"});
            t = tError();
        }
        LocalVar &lv = declareLocal(v.name, t ? t : tError(), v.pos, isFinal);
        lv.hasInit = v.init != nullptr;
        // a final local initialized with a constant is a constant variable
        if (isFinal && cv.ok() && t && (isPrim(t) || isStringType(t)))
        {
            Const c = cv;
            if (isPrim(t) && c.k != Const::Str && c.k != Const::Bool)
            {
                switch (t->k)
                {
                case TK::Long: c.k = Const::Long; c.i = c.k == Const::Float || c.k == Const::Double ? (int64_t)c.d : c.i; break;
                case TK::Double: if (c.k != Const::Double && c.k != Const::Float) { c.d = (double)c.i; } c.k = Const::Double; break;
                case TK::Float: if (c.k != Const::Double && c.k != Const::Float) { c.d = (double)(float)c.i; } c.k = Const::Float; break;
                case TK::Char: c.k = Const::Char; break;
                default: c.k = Const::Int; break;
                }
            }
            if (!(t->k == TK::Boolean && c.k != Const::Bool) && !(isStringType(t) && c.k != Const::Str))
                lv.cval = c;
        }
        if (v.init)
            markAssigned(&lv);
        out.push_back(defineLocal(lv.q, std::move(init), l));
    }
}

ASTNodePtr Sema::stmt(const StmtP &sp)
{
    if (!sp)
        return nullptr;
    const Stmt &s = *sp;
    int l = s.pos.line;
    switch (s.k)
    {
    case SK::Block:
        return block(s.stmts, l);
    case SK::LocalVar:
    {
        std::vector<ASTNodePtr> out;
        localVar(s, out);
        return q::block(std::move(out), l);
    }
    case SK::LocalClass:
        return localClass(s);
    case SK::Expr:
    {
        ExprP e = s.e;
        if (e->k == EK::PostIncDec)
        {
            // the value is unused: x++ is ++x
            auto pre = std::make_shared<Expr>(*e);
            pre->k = EK::PreIncDec;
            e = pre;
        }
        Val v = expr(e);
        return q::exprStmt(std::move(v.node), l);
    }
    case SK::If: return ifStmt(s);
    case SK::While: return whileStmt(s, "");
    case SK::Do: return doStmt(s, "");
    case SK::For: return forStmt(s, "");
    case SK::ForEach: return forEach(s, "");
    case SK::Return: return returnStmt(s);
    case SK::Break:
    case SK::Continue: return jump(s);
    case SK::Throw: return throwStmt(s);
    case SK::Try: return tryStmt(s);
    case SK::Switch: return switchStmt(s.sw, "", l);
    case SK::Labeled: return labeled(s);
    case SK::Empty: return nullptr;
    case SK::Sync:
    {
        Val lock = expr(s.e);
        if (isPrim(lock.type))
            error(s.e->pos, "unexpected type", {"  required: reference", "  found:    " + typeStr(lock.type)});
        std::vector<ASTNodePtr> out;
        out.push_back(q::exprStmt(q::un(NONNULL, std::move(lock.node), l), l));
        out.push_back(block(s.s1->stmts, l));
        return q::block(std::move(out), l);
    }
    case SK::Assert:
    {
        // assertions are disabled by default (java -ea enables them): attributed only
        Flow saved = fn_->flow;
        cond(s.e);
        if (s.e2)
            expr(s.e2);
        fn_->flow = saved;
        return nullptr;
    }
    case SK::Yield:
    {
        if (yieldTypes_.empty())
        {
            error(s.pos, "yield outside of switch expression");
            return nullptr;
        }
        // inside the switch expression's closure: yield returns the value
        ASTNodePtr v = exprTo(s.e, yieldTypes_.back(), s.e->pos);
        fn_->flow.alive = false;
        return q::ret(std::move(v), l);
    }
    case SK::CtorCall:
        error(s.pos, std::string("call to ") + (s.superCall ? "super" : "this") + " must be first statement in constructor");
        return nullptr;
    }
    return nullptr;
}

ASTNodePtr Sema::ifStmt(const Stmt &s)
{
    int l = s.pos.line;
    pushScope(); // pattern variables of the condition
    ASTNodePtr c = cond(s.e);
    bool cTrue = false, cFalse = false;
    (void)cTrue;
    (void)cFalse;
    Flow afterCond = fn_->flow;
    pushScope();
    ASTNodePtr t = stmt(s.s1);
    popScope();
    Flow afterThen = fn_->flow;
    fn_->flow = afterCond;
    ASTNodePtr e;
    if (s.s2)
    {
        pushScope();
        e = stmt(s.s2);
        popScope();
    }
    Flow afterElse = fn_->flow;
    fn_->flow = joinFlow(afterThen, afterElse);
    if (!s.s2)
        fn_->flow.alive = true; // if-then can always complete normally (JLS §14.22)
    // `if (!(o instanceof T x)) return;` — x stays in scope after the if
    std::vector<LocalVar> bindings;
    if (!afterThen.alive && fn_->scopes.size() >= 1)
        for (auto &v : fn_->scopes.back().vars)
            if (v.q.find("$p") != std::string::npos)
                bindings.push_back(v);
    popScope();
    if (!bindings.empty() && s.e->k == EK::Unary && s.e->name == "!")
        for (auto &b : bindings)
        {
            fn_->scopes.back().vars.push_back(b);
            markAssigned(&fn_->scopes.back().vars.back());
        }
    if (!t)
        t = q::block({}, l);
    return q::ifs(std::move(c), std::move(t), std::move(e), l);
}

// Statements after a Quantum loop: propagate flagged break / continue jumps
// that target loops further out (see jump()).
void Sema::afterQuantumLoop(std::vector<ASTNodePtr> &out, size_t targetIndex, int l)
{
    std::vector<FnCtx::PendingFlag> keep;
    for (auto &pf : fn_->pendingFlags)
    {
        if (pf.target >= targetIndex)
            continue; // targeted this loop (or an inner one): already handled
        JumpTarget &t = fn_->targets[pf.target];
        const std::string &flag = pf.isContinue ? t.continueFlag : t.breakFlag;
        bool immediate = t.quantumDepth == fn_->quantumLoops;
        if (immediate && pf.isContinue)
        {
            std::vector<ASTNodePtr> body;
            body.push_back(q::exprStmt(q::assign(q::id(flag, l), q::boolean(false, l), l), l));
            body.push_back(q::cont(l));
            out.push_back(q::ifs(q::id(flag, l), q::block(std::move(body), l), nullptr, l));
        }
        else
        {
            std::vector<ASTNodePtr> body;
            body.push_back(q::brk(l));
            out.push_back(q::ifs(q::id(flag, l), q::block(std::move(body), l), nullptr, l));
            if (!immediate)
                keep.push_back(pf);
        }
    }
    // dedupe what stays pending
    std::vector<FnCtx::PendingFlag> uniq;
    for (auto &pf : keep)
    {
        bool dup = false;
        for (auto &u : uniq)
            dup = dup || (u.target == pf.target && u.isContinue == pf.isContinue);
        if (!dup)
            uniq.push_back(pf);
    }
    fn_->pendingFlags = uniq;
    JumpTarget &self = fn_->targets[targetIndex];
    if (!self.breakFlag.empty())
        out.push_back(q::exprStmt(q::assign(q::id(self.breakFlag, l), q::boolean(false, l), l), l));
}

ASTNodePtr Sema::loopBody(const StmtP &body, JumpTarget &t)
{
    (void)t;
    pushScope();
    ASTNodePtr b = stmt(body);
    popScope();
    return b ? std::move(b) : q::block({}, 0);
}

namespace
{
struct LoopScope
{
    Sema &s;
    size_t index;
    size_t pendingMark;
    LoopScope(Sema &se, const std::string &label, bool isLoop, bool isSwitch) : s(se)
    {
        JumpTarget t;
        t.label = label;
        t.isLoop = isLoop;
        t.isSwitch = isSwitch;
        t.quantumDepth = ++s.fn_->quantumLoops;
        t.finallyDepth = (int)s.fn_->finallies.size();
        s.fn_->targets.push_back(t);
        index = s.fn_->targets.size() - 1;
        pendingMark = s.fn_->pendingFlags.size();
    }
    JumpTarget &target() { return s.fn_->targets[index]; }
    // closes the Quantum loop; returns the statements to put after it
    std::vector<ASTNodePtr> close(ASTNodePtr loop, int l, JumpTarget &done)
    {
        std::vector<ASTNodePtr> out;
        out.push_back(std::move(loop));
        --s.fn_->quantumLoops;
        s.afterQuantumLoop(out, index, l);
        done = s.fn_->targets[index];
        s.fn_->targets.pop_back();
        return out;
    }
};
} // namespace

ASTNodePtr Sema::whileStmt(const Stmt &s, const std::string &label)
{
    int l = s.pos.line;
    bool cTrue = constTrue(s.e), cFalse = constFalse(s.e);
    pushScope();
    ASTNodePtr c = cond(s.e);
    Flow afterCond = fn_->flow;
    LoopScope ls(*this, label, true, false);
    if (cFalse)
        fn_->flow.alive = false;
    ASTNodePtr body = loopBody(s.s1, ls.target());
    JumpTarget done;
    std::vector<ASTNodePtr> out = ls.close(q::whiles(std::move(c), std::move(body), nullptr, l), l, done);
    popScope();
    Flow exitFlow = afterCond;
    if (cTrue)
        exitFlow.alive = false;
    if (done.breakHit)
        exitFlow = joinFlow(exitFlow, done.breakFlow);
    fn_->flow = exitFlow;
    if (cTrue && !done.breakHit)
        fn_->flow.alive = false;
    return q::block(std::move(out), l);
}

ASTNodePtr Sema::doStmt(const Stmt &s, const std::string &label)
{
    int l = s.pos.line;
    bool cTrue = constTrue(s.e);
    LoopScope ls(*this, label, true, false);
    pushScope();
    ASTNodePtr body = loopBody(s.s1, ls.target());
    popScope();
    Flow afterBody = fn_->flow;
    if (ls.target().continueHit)
        fn_->flow.alive = true;
    ASTNodePtr c = cond(s.e);
    Flow afterCond = fn_->flow;
    // condition checked after each pass (and after `continue`): leave when false
    std::vector<ASTNodePtr> post;
    post.push_back(q::ifs(q::qnot(std::move(c), l), q::block([&] {
                              std::vector<ASTNodePtr> b;
                              b.push_back(q::brk(l));
                              return b;
                          }(), l),
                          nullptr, l));
    JumpTarget done;
    std::vector<ASTNodePtr> out =
        ls.close(q::whiles(q::boolean(true, l), std::move(body), q::block(std::move(post), l), l), l, done);
    Flow exitFlow = afterCond;
    exitFlow.alive = (afterBody.alive || done.continueHit) && !cTrue;
    if (done.breakHit)
        exitFlow = joinFlow(exitFlow, done.breakFlow), exitFlow.alive = true;
    fn_->flow = exitFlow;
    return q::block(std::move(out), l);
}

ASTNodePtr Sema::forStmt(const Stmt &s, const std::string &label)
{
    int l = s.pos.line;
    pushScope();
    std::vector<ASTNodePtr> out;
    for (auto &i : s.init)
    {
        if (i->k == SK::LocalVar)
            localVar(*i, out);
        else if (ASTNodePtr n = stmt(i))
            out.push_back(std::move(n));
    }
    bool cTrue = constTrue(s.e), cFalse = constFalse(s.e);
    ASTNodePtr c = s.e ? cond(s.e) : q::boolean(true, l);
    Flow afterCond = fn_->flow;
    LoopScope ls(*this, label, true, false);
    if (cFalse)
        fn_->flow.alive = false;
    ASTNodePtr body = loopBody(s.s1, ls.target());
    fn_->flow.alive = true;
    std::vector<ASTNodePtr> post;
    for (auto &u : s.update)
    {
        ExprP e = u;
        if (e->k == EK::PostIncDec)
        {
            auto pre = std::make_shared<Expr>(*e);
            pre->k = EK::PreIncDec;
            e = pre;
        }
        post.push_back(q::exprStmt(expr(e).node, l));
    }
    JumpTarget done;
    std::vector<ASTNodePtr> loop =
        ls.close(q::whiles(std::move(c), std::move(body), post.empty() ? nullptr : q::block(std::move(post), l), l), l,
                 done);
    for (auto &x : loop)
        out.push_back(std::move(x));
    popScope();
    Flow exitFlow = afterCond;
    if (cTrue)
        exitFlow.alive = false;
    if (done.breakHit)
        exitFlow = joinFlow(exitFlow, done.breakFlow);
    fn_->flow = exitFlow;
    if (cTrue && !done.breakHit)
        fn_->flow.alive = false;
    return q::block(std::move(out), l);
}

ASTNodePtr Sema::forEach(const Stmt &s, const std::string &label)
{
    int l = s.pos.line;
    Val it = expr(s.e);
    pushScope();
    std::vector<ASTNodePtr> out;
    const VarDeclarator &v = s.vars[0];
    bool isVar = s.type && s.type->kind == TypeRef::Var;
    TypeP declared = isVar ? nullptr : resolveType(s.type);
    TypeP elem;
    TypeP t = isError(it.type) ? it.type : upperBound(it.type);
    std::string seq = temp();
    out.push_back(q::exprStmt(q::assign(q::id(seq, l), q::un(NONNULL, std::move(it.node), l), l), l));
    ASTNodePtr condNode, fetch;
    ASTNodePtr post;
    if (!isError(t) && t->k == TK::Array)
    {
        elem = t->elem;
        std::string i = temp(), n = temp();
        out.push_back(q::exprStmt(q::assign(q::id(i, l), q::lng(0, l), l), l));
        out.push_back(q::exprStmt(q::assign(q::id(n, l), q::un(ALEN, q::id(seq, l), l), l), l));
        condNode = q::bin(LT, q::id(i, l), q::id(n, l), l);
        fetch = q::aload(q::id(seq, l), q::id(i, l), l);
        post = q::exprStmt(q::assign(q::id(i, l), q::bin(IADD, q::id(i, l), q::lng(1, l), l), l), l);
    }
    else
    {
        ClassSym *iterable = cls("java.lang.Iterable");
        TypeP view = isError(t) ? nullptr : asSuper(t, iterable);
        if (!isError(t) && !view)
        {
            error(s.e->pos, "for-each not applicable to expression type",
                  {"  required: array or java.lang.Iterable", "  found:    " + typeStr(it.type)});
            elem = tError();
        }
        else if (view)
        {
            elem = view->args.empty() ? tObject() : view->args[0];
            if (elem->k == TK::Wildcard)
                elem = upperBound(elem);
        }
        else
            elem = tError();
        std::string iter = temp();
        out.push_back(q::exprStmt(q::assign(q::id(iter, l), q::inv("iterator()", q::id(seq, l), {}, l), l), l));
        condNode = q::inv("hasNext()", q::id(iter, l), {}, l);
        fetch = q::inv("next()", q::id(iter, l), {}, l);
        need(cls("java.util.Iterator"));
    }
    TypeP vt = isVar ? elem : declared;
    LoopScope ls(*this, label, true, false);
    pushScope();
    LocalVar &lv = declareLocal(v.name, vt ? vt : tError(), v.pos, s.mods.has(M_FINAL));
    lv.hasInit = true;
    markAssigned(&lv);
    Val ev;
    ev.node = std::move(fetch);
    ev.type = elem;
    std::vector<ASTNodePtr> body;
    body.push_back(defineLocal(lv.q, coerce(std::move(ev), vt ? vt : tError(), v.pos), l));
    Flow before = fn_->flow;
    if (ASTNodePtr b = stmt(s.s1))
        body.push_back(std::move(b));
    popScope();
    JumpTarget done;
    std::vector<ASTNodePtr> loop = ls.close(q::whiles(std::move(condNode), q::block(std::move(body), l), std::move(post), l), l, done);
    for (auto &x : loop)
        out.push_back(std::move(x));
    popScope();
    Flow exitFlow = before;
    exitFlow.alive = true;
    if (done.breakHit)
        exitFlow = joinFlow(exitFlow, done.breakFlow);
    fn_->flow = exitFlow;
    fn_->flow.alive = true;
    return q::block(std::move(out), l);
}

ASTNodePtr Sema::labeled(const Stmt &s)
{
    int l = s.pos.line;
    for (auto &t : fn_->targets)
        if (t.label == s.label)
            error(s.pos, "label " + s.label + " already in use");
    const StmtP &inner = s.s1;
    switch (inner->k)
    {
    case SK::While: return whileStmt(*inner, s.label);
    case SK::Do: return doStmt(*inner, s.label);
    case SK::For: return forStmt(*inner, s.label);
    case SK::ForEach: return forEach(*inner, s.label);
    case SK::Switch: return switchStmt(inner->sw, s.label, inner->pos.line);
    default: break;
    }
    // a labeled block: a loop that runs once
    LoopScope ls(*this, s.label, false, false);
    std::vector<ASTNodePtr> body;
    if (ASTNodePtr b = stmt(inner))
        body.push_back(std::move(b));
    bool innerAlive = fn_->flow.alive;
    body.push_back(q::brk(l));
    JumpTarget done;
    std::vector<ASTNodePtr> out = ls.close(q::whiles(q::boolean(true, l), q::block(std::move(body), l), nullptr, l), l, done);
    if (done.breakHit)
        fn_->flow = joinFlow(fn_->flow, done.breakFlow), fn_->flow.alive = true;
    else
        fn_->flow.alive = innerAlive;
    return q::block(std::move(out), l);
}

ASTNodePtr Sema::finallyChain(size_t downTo, int l)
{
    if (fn_->finallies.size() <= downTo)
        return nullptr;
    std::vector<ASTNodePtr> out;
    std::vector<StmtP> saved = fn_->finallies;
    Flow savedFlow = fn_->flow;
    for (size_t i = saved.size(); i-- > downTo;)
    {
        fn_->finallies.assign(saved.begin(), saved.begin() + i);
        fn_->flow.alive = true;
        out.push_back(block(saved[i]->stmts, l));
    }
    fn_->finallies = saved;
    fn_->flow = savedFlow;
    return q::block(std::move(out), l);
}

ASTNodePtr Sema::jump(const Stmt &s)
{
    int l = s.pos.line;
    bool isBreak = s.k == SK::Break;
    // the target
    int ti = -1;
    for (int i = (int)fn_->targets.size() - 1; i >= 0; --i)
    {
        JumpTarget &t = fn_->targets[i];
        if (!s.label.empty())
        {
            if (t.label == s.label)
            {
                ti = i;
                break;
            }
        }
        else if (isBreak ? (t.isLoop || t.isSwitch) : t.isLoop)
        {
            ti = i;
            break;
        }
    }
    if (ti < 0)
    {
        if (!s.label.empty())
            error(s.pos, "undefined label: " + s.label);
        else
            error(s.pos, isBreak ? "break outside switch or loop" : "continue outside of loop");
        fn_->flow.alive = false;
        return nullptr;
    }
    JumpTarget &t = fn_->targets[ti];
    if (!isBreak && !t.isLoop)
    {
        error(s.pos, "not a loop label: " + s.label);
        return nullptr;
    }
    std::vector<ASTNodePtr> out;
    if (ASTNodePtr f = finallyChain((size_t)t.finallyDepth, l))
        out.push_back(std::move(f));
    JumpTarget &tt = fn_->targets[ti]; // (finallyChain may have grown targets)
    if (isBreak)
    {
        tt.breakFlow = tt.breakFlowSet ? joinFlow(tt.breakFlow, fn_->flow) : fn_->flow;
        tt.breakFlowSet = true;
        tt.breakHit = true;
    }
    else
        tt.continueHit = true;
    if (tt.quantumDepth == fn_->quantumLoops)
        out.push_back(isBreak ? q::brk(l) : q::cont(l));
    else
    {
        std::string &flag = isBreak ? tt.breakFlag : tt.continueFlag;
        if (flag.empty())
            flag = temp();
        out.push_back(q::exprStmt(q::assign(q::id(flag, l), q::boolean(true, l), l), l));
        out.push_back(q::brk(l));
        fn_->pendingFlags.push_back({(size_t)ti, !isBreak});
    }
    fn_->flow.alive = false;
    return q::block(std::move(out), l);
}

ASTNodePtr Sema::returnStmt(const Stmt &s)
{
    int l = s.pos.line;
    FnCtx *f = fn_;
    std::vector<ASTNodePtr> out;
    ASTNodePtr value;
    if (f->isLambda)
    {
        if (s.e)
        {
            if (f->lambdaReturns)
            {
                Val v = expr(s.e);
                f->lambdaReturns->push_back(v.type);
                value = std::move(v.node);
            }
            else if (f->ret && f->ret->k == TK::Void)
            {
                Val v = expr(s.e);
                error(s.e->pos, "incompatible types: bad return type in lambda expression",
                      {"    " + typeStr(v.type) + " cannot be converted to void"});
                value = std::move(v.node);
            }
            else
                value = exprTo(s.e, f->ret ? f->ret : tError(), s.e->pos);
        }
        else if (f->ret && f->ret->k != TK::Void && !isError(f->ret) && !f->lambdaReturns)
            error(s.pos, "incompatible types: bad return type in lambda expression", {"    missing return value"});
    }
    else if (!f->method)
    {
        if (f->isCtor)
            ;
        else
            error(s.pos, "return outside method");
    }
    else
    {
        TypeP rt = f->method->ret;
        if (f->method->isCtor)
        {
            if (s.e)
            {
                error(s.e->pos, "incompatible types: unexpected return value");
                expr(s.e);
            }
            value = thisNode(l);
        }
        else if (s.e)
        {
            if (rt->k == TK::Void)
            {
                Val v = expr(s.e);
                error(s.e->pos, "incompatible types: unexpected return value");
                value = std::move(v.node);
            }
            else
                value = exprTo(s.e, rt, s.e->pos);
        }
        else if (rt->k != TK::Void)
            error(s.pos, "incompatible types: missing return value");
    }
    if (!fn_->finallies.empty())
    {
        // (a variable too: the finally block may assign it)
        bool literal = value && (value->is<NumberLiteral>() || value->is<StringLiteral>() || value->is<BoolLiteral>() ||
                                 value->is<NilLiteral>());
        if (value && !literal)
        {
            std::string tv = temp();
            out.push_back(q::exprStmt(q::assign(q::id(tv, l), std::move(value), l), l));
            value = q::id(tv, l);
        }
        if (ASTNodePtr fin = finallyChain(0, l))
            out.push_back(std::move(fin));
    }
    out.push_back(q::ret(std::move(value), l));
    fn_->flow.alive = false;
    return q::block(std::move(out), l);
}

ASTNodePtr Sema::throwStmt(const Stmt &s)
{
    int l = s.pos.line;
    Val v = expr(s.e);
    ClassSym *thr = cls("java.lang.Throwable");
    if (!isError(v.type) && !(v.type->k == TK::Null) && !isSubtype(v.type, classType(thr)))
        error(s.e->pos, "incompatible types: " + typeStr(v.type) + " cannot be converted to Throwable");
    // `throw e` of a catch parameter rethrows only what its try can throw
    if (s.e->k == EK::Name && fn_->rethrowTypes.count(s.e->name) && !findLocal(s.e->name)->assignedLater)
    {
        for (auto &t : fn_->rethrowTypes[s.e->name])
            thrown(t, s.e->pos);
    }
    else
        thrown(v.type, s.e->pos);
    fn_->flow.alive = false;
    return q::raise(std::move(v.node), l);
}

// ─── try / catch / finally ───────────────────────────────────────────────────

// ─── try-with-resources ──────────────────────────────────────────────────────
// Desugared as JLS §14.20.3.1 specifies, then lowered as ordinary code:
//   try (R r = init; ...) B
//   =>
//   { final R r = init;
//     Throwable #p = null;
//     try { <the remaining resources> B }
//     catch (Throwable #t) { #p = #t; throw #t; }
//     finally {
//       if (r != null)
//         if (#p != null) { try { r.close(); } catch (Throwable #x) { #p.addSuppressed(#x); } }
//         else r.close();
//     } }
// The original catch / finally clauses wrap the whole.

namespace
{
ExprP mkName(const std::string &n, Pos p)
{
    auto e = std::make_shared<Expr>();
    e->k = EK::Name;
    e->name = n;
    e->pos = p;
    return e;
}

ExprP mkNull(Pos p)
{
    auto e = std::make_shared<Expr>();
    e->k = EK::Literal;
    e->lit = Expr::LNull;
    e->pos = p;
    return e;
}

ExprP mkBinary(const std::string &op, ExprP a, ExprP b, Pos p)
{
    auto e = std::make_shared<Expr>();
    e->k = EK::Binary;
    e->name = op;
    e->a = std::move(a);
    e->b = std::move(b);
    e->pos = p;
    e->opPos = p;
    return e;
}

ExprP mkAssign(ExprP a, ExprP b, Pos p)
{
    auto e = std::make_shared<Expr>();
    e->k = EK::Assign;
    e->name = "=";
    e->a = std::move(a);
    e->b = std::move(b);
    e->pos = p;
    e->opPos = p;
    return e;
}

ExprP mkCall(ExprP recv, const std::string &m, std::vector<ExprP> args, Pos p)
{
    auto e = std::make_shared<Expr>();
    e->k = EK::Call;
    e->name = m;
    e->a = std::move(recv);
    e->args = std::move(args);
    e->pos = p;
    e->opPos = p;
    return e;
}

StmtP mkStmt(SK k, Pos p)
{
    auto s = std::make_shared<Stmt>();
    s->k = k;
    s->pos = p;
    s->endPos = p;
    return s;
}

StmtP mkExprStmt(ExprP e, Pos p)
{
    auto s = mkStmt(SK::Expr, p);
    s->e = std::move(e);
    return s;
}

StmtP mkBlock(std::vector<StmtP> stmts, Pos p)
{
    auto s = mkStmt(SK::Block, p);
    s->stmts = std::move(stmts);
    return s;
}

StmtP mkIf(ExprP c, StmtP t, StmtP f, Pos p)
{
    auto s = mkStmt(SK::If, p);
    s->e = std::move(c);
    s->s1 = std::move(t);
    s->s2 = std::move(f);
    return s;
}

StmtP mkThrow(ExprP e, Pos p)
{
    auto s = mkStmt(SK::Throw, p);
    s->e = std::move(e);
    return s;
}

TypeRefP mkClassRef(const std::string &binaryName, Pos p)
{
    auto t = std::make_shared<TypeRef>();
    t->kind = TypeRef::Named;
    TypeRef::Part part;
    part.name = std::string("\x01") + binaryName;
    part.pos = p;
    t->parts.push_back(part);
    t->pos = p;
    return t;
}

StmtP mkLocal(TypeRefP type, const std::string &n, ExprP init, bool isFinal, Pos p)
{
    auto s = mkStmt(SK::LocalVar, p);
    if (isFinal)
        s->mods.flags = M_FINAL;
    s->type = std::move(type);
    VarDeclarator v;
    v.name = n;
    v.init = std::move(init);
    v.pos = p;
    s->vars.push_back(v);
    return s;
}

CatchClause mkCatch(TypeRefP type, const std::string &n, StmtP body, Pos p)
{
    CatchClause c;
    c.types.push_back(std::move(type));
    c.name = n;
    c.body = std::move(body);
    c.pos = p;
    return c;
}
} // namespace

StmtP Sema::desugarResources(const Stmt &s, size_t i)
{
    if (i == s.resources.size())
        return s.tryBlock;
    const Resource &r = s.resources[i];
    Pos p = r.pos;
    std::string n = std::to_string(++tempCounter_);
    std::vector<StmtP> out;
    std::string var = r.name;
    if (r.type)
        out.push_back(mkLocal(r.type, r.name, r.init, true, p));
    else
    {
        // try (existing) { ... }: the variable's value
        auto vt = std::make_shared<TypeRef>();
        vt->kind = TypeRef::Var;
        vt->pos = p;
        var = "#r" + n;
        out.push_back(mkLocal(vt, var, r.init, true, p));
    }
    std::string pv = "#p" + n, tv = "#t" + n, xv = "#x" + n;
    out.push_back(mkLocal(mkClassRef("java.lang.Throwable", p), pv, mkNull(p), false, p));
    // try { rest } catch (Throwable #t) { #p = #t; throw #t; } finally { close }
    auto t = mkStmt(SK::Try, s.pos);
    StmtP rest = desugarResources(s, i + 1);
    t->tryBlock = rest->k == SK::Block ? rest : mkBlock({rest}, p);
    t->catches.push_back(mkCatch(mkClassRef("java.lang.Throwable", p), tv,
                                 mkBlock({mkExprStmt(mkAssign(mkName(pv, p), mkName(tv, p), p), p), mkThrow(mkName(tv, p), p)}, p), p));
    auto inner = mkStmt(SK::Try, p);
    inner->tryBlock = mkBlock({mkExprStmt(mkCall(mkName(var, p), "close", {}, p), p)}, p);
    inner->catches.push_back(mkCatch(mkClassRef("java.lang.Throwable", p), xv,
                                     mkBlock({mkExprStmt(mkCall(mkName(pv, p), "addSuppressed", {mkName(xv, p)}, p), p)}, p), p));
    StmtP close = mkIf(mkBinary("!=", mkName(var, p), mkNull(p), p),
                       mkBlock({mkIf(mkBinary("!=", mkName(pv, p), mkNull(p), p), mkBlock({inner}, p),
                                     mkBlock({mkExprStmt(mkCall(mkName(var, p), "close", {}, p), p)}, p), p)}, p),
                       nullptr, p);
    t->finallyBlock = mkBlock({close}, p);
    out.push_back(t);
    return mkBlock(out, p);
}

ASTNodePtr Sema::tryStmt(const Stmt &s)
{
    if (!s.resources.empty())
    {
        StmtP body = desugarResources(s, 0);
        if (s.catches.empty() && !s.finallyBlock)
            return stmt(body);
        auto outer = std::make_shared<Stmt>(s);
        outer->resources.clear();
        outer->tryBlock = body;
        return tryStmt(*outer);
    }

    int l = s.pos.line;
    ClassSym *thr = cls("java.lang.Throwable");
    // try-with-resources: try (R r = e) B  =  { R r = e; try B finally { if (r != null) r.close(); } }
    std::vector<ASTNodePtr> resourceDecls;
    pushScope();
    std::vector<std::pair<std::string, TypeP>> resources;
    for (auto &r : s.resources)
    {
        TypeP declared = r.type && r.type->kind != TypeRef::Var ? resolveType(r.type) : nullptr;
        Val v = expr(r.init, declared);
        TypeP rt = declared ? declared : v.type;
        ClassSym *ac = cls("java.lang.AutoCloseable");
        if (!isError(rt) && ac && !isSubtype(rt, classType(ac)))
            error(r.init->pos, "incompatible types: try-with-resources not applicable to variable type",
                  {"    (" + typeStr(rt) + " cannot be converted to AutoCloseable)"});
        std::string name = r.name.empty() ? temp() : r.name;
        ASTNodePtr init = r.type ? coerce(std::move(v), rt, r.init->pos) : std::move(v.node);
        if (!r.name.empty())
        {
            LocalVar &lv = declareLocal(r.name, rt, r.pos, true);
            lv.hasInit = true;
            markAssigned(&lv);
            resourceDecls.push_back(defineLocal(lv.q, std::move(init), l));
            name = lv.q;
        }
        else
            resourceDecls.push_back(q::exprStmt(q::assign(q::id(name, l), std::move(init), l), l));
        resources.push_back({name, rt});
    }
    // checked exceptions: the catches in scope while the try block is lowered
    bool hasCatches = !s.catches.empty();
    std::vector<std::vector<TypeP>> catchTypes;
    for (auto &c : s.catches)
    {
        std::vector<TypeP> ts;
        for (auto &tr : c.types)
        {
            TypeP t = resolveType(tr);
            if (!isError(t) && thr && !isSubtype(t, classType(thr)))
                error(tr->pos, "incompatible types: " + typeStr(t) + " cannot be converted to Throwable");
            ts.push_back(t);
        }
        catchTypes.push_back(ts);
    }
    if (hasCatches)
    {
        ExcScope sc;
        sc.catches = catchTypes;
        sc.thrown.assign(catchTypes.size(), false);
        sc.caught.assign(catchTypes.size(), {});
        fn_->excScopes.push_back(sc);
    }
    if (s.finallyBlock)
        fn_->finallies.push_back(s.finallyBlock);
    Flow before = fn_->flow;
    // the resources' close() calls (in reverse order) run like a finally block
    ASTNodePtr body;
    {
        std::vector<ASTNodePtr> b;
        if (!resources.empty())
        {
            // body, then close the resources (also when the body throws)
            ASTNodePtr inner = block(s.tryBlock->stmts, l);
            std::vector<ASTNodePtr> closes;
            for (size_t i = resources.size(); i-- > 0;)
            {
                MethodSym *closeM = nullptr;
                TypeP rt = resources[i].second;
                if (!isError(rt) && upperBound(rt)->cls)
                {
                    std::vector<MethodSym *> cs;
                    collectMethods(upperBound(rt)->cls, "close", cs);
                    for (MethodSym *m : cs)
                        if (m->params.empty())
                            closeM = m;
                }
                if (closeM)
                    for (auto &t : closeM->throws)
                        thrown(subst(t, substOf(asSuper(rt, closeM->owner))), s.pos);
                ASTNodePtr call = closeM ? invoke(closeM, q::id(resources[i].first, l), {}, true, l, rt)
                                         : q::inv("close()", q::id(resources[i].first, l), {}, l);
                closes.push_back(q::ifs(q::bin(REFNE, q::id(resources[i].first, l), q::nil(l), l),
                                        q::block([&] {
                                            std::vector<ASTNodePtr> x;
                                            x.push_back(q::exprStmt(std::move(call), l));
                                            return x;
                                        }(), l),
                                        nullptr, l));
            }
            std::string ex = temp();
            std::vector<ASTNodePtr> handler;
            for (auto &c : closes)
                handler.push_back(q::clone(c));
            handler.push_back(q::raise(q::id(ex, l), l));
            b.push_back(q::tryCatch(std::move(inner), ex, q::block(std::move(handler), l), l));
            for (auto &c : closes)
                b.push_back(std::move(c));
        }
        else
            b.push_back(block(s.tryBlock->stmts, l));
        body = q::block(std::move(b), l);
    }
    Flow afterTry = fn_->flow;
    ExcScope scope;
    if (hasCatches)
    {
        scope = fn_->excScopes.back();
        fn_->excScopes.pop_back();
    }
    // catch clauses: one Quantum handler testing each clause in order
    ASTNodePtr result = std::move(body);
    Flow flowOut = afterTry;
    if (hasCatches)
    {
        std::string raw = temp();
        std::string ex = temp();
        needNamed("java.lang.Throwable", "$wrap");
        std::vector<ASTNodePtr> handler;
        std::vector<ASTNodePtr> wa;
        wa.push_back(q::id(raw, l));
        handler.push_back(q::exprStmt(q::assign(q::id(ex, l), q::call("java.lang.Throwable.$wrap", std::move(wa), l), l), l));
        ASTNodePtr chain = q::raise(q::id(ex, l), l);
        std::vector<ASTNodePtr> clauses;
        std::vector<TypeP> seenTypes;
        // build from the last clause backwards
        std::vector<ASTNodePtr> bodies(s.catches.size());
        std::vector<Flow> outs;
        for (size_t i = 0; i < s.catches.size(); ++i)
        {
            const CatchClause &c = s.catches[i];
            // a clause after one catching a supertype can never run
            for (auto &t : catchTypes[i])
                for (auto &prev : seenTypes)
                    if (!isError(t) && !isError(prev) && isSubtype(t, prev))
                        error(c.types[0]->pos, "exception " + typeStr(t) + " has already been caught");
            for (auto &t : catchTypes[i])
            {
                seenTypes.push_back(t);
                // a checked exception the try block cannot throw
                if (isChecked(t) && !scope.thrown[i] && fn_->checkExceptions)
                {
                    const std::string &n = t->cls ? t->cls->binaryName : "";
                    if (n != "java.lang.Exception" && n != "java.lang.Throwable")
                        error(c.types[0]->pos, "exception " + typeStr(t) + " is never thrown in body of corresponding try statement");
                }
            }
            pushScope();
            fn_->flow = before;
            fn_->flow.alive = true;
            TypeP paramType = catchTypes[i].size() == 1 ? catchTypes[i][0] : [&] {
                TypeP u = catchTypes[i][0];
                for (size_t k = 1; k < catchTypes[i].size(); ++k)
                    u = lub(u, catchTypes[i][k]);
                return u;
            }();
            LocalVar &lv = declareLocal(c.name, paramType, c.pos, catchTypes[i].size() > 1);
            lv.hasInit = true;
            markAssigned(&lv);
            lv.q = c.name + "$c" + std::to_string(++tempCounter_);
            fn_->rethrowTypes[c.name] = scope.caught[i];
            std::vector<ASTNodePtr> cb;
            cb.push_back(defineLocal(lv.q, q::id(ex, l), l));
            cb.push_back(block(c.body->stmts, l));
            fn_->rethrowTypes.erase(c.name);
            popScope();
            outs.push_back(fn_->flow);
            bodies[i] = q::block(std::move(cb), l);
        }
        for (size_t i = s.catches.size(); i-- > 0;)
        {
            ASTNodePtr test;
            for (auto &t : catchTypes[i])
            {
                ASTNodePtr one = isError(t) ? q::boolean(true, l) : q::instOf(q::id(ex, l), descriptor(t), l);
                test = test ? q::qbin("||", std::move(test), std::move(one), l) : std::move(one);
            }
            chain = q::ifs(std::move(test), std::move(bodies[i]), std::move(chain), l);
        }
        handler.push_back(std::move(chain));
        result = q::tryCatch(std::move(result), raw, q::block(std::move(handler), l), l);
        for (auto &o : outs)
            flowOut = joinFlow(flowOut, o);
    }
    if (s.finallyBlock)
    {
        fn_->finallies.pop_back();
        // exceptional path: run the finally block, then rethrow
        std::string raw = temp();
        Flow savedFlow = fn_->flow;
        fn_->flow = before;
        fn_->flow.alive = true;
        std::vector<ASTNodePtr> handler;
        handler.push_back(block(s.finallyBlock->stmts, l));
        handler.push_back(q::raise(q::id(raw, l), l));
        fn_->flow = savedFlow;
        std::vector<ASTNodePtr> wrapped;
        wrapped.push_back(q::tryCatch(std::move(result), raw, q::block(std::move(handler), l), l));
        // normal path
        fn_->flow = flowOut;
        bool aliveBefore = flowOut.alive;
        fn_->flow.alive = true;
        wrapped.push_back(block(s.finallyBlock->stmts, l));
        bool finallyAlive = fn_->flow.alive;
        fn_->flow.alive = aliveBefore && finallyAlive;
        result = q::block(std::move(wrapped), l);
    }
    else
        fn_->flow = flowOut;
    popScope();
    if (!resourceDecls.empty())
    {
        resourceDecls.push_back(std::move(result));
        return q::block(std::move(resourceDecls), l);
    }
    return result;
}

// ─── switch ──────────────────────────────────────────────────────────────────

namespace
{
enum class SelKind
{
    Integral,
    String,
    Enum,
    Other,
};
} // namespace

ASTNodePtr Sema::switchStmt(const std::shared_ptr<SwitchBody> &sw, const std::string &label, int l)
{
    Val sel = expr(sw->selector);
    TypeP st = sel.type;
    SelKind kind = SelKind::Other;
    TypeP nu = numericUnboxed(st);
    bool hasPatterns = false, hasNull = false, hasDefault = false;
    for (auto &c : sw->cases)
    {
        hasPatterns = hasPatterns || c.patternType;
        hasNull = hasNull || c.hasNullLabel;
        hasDefault = hasDefault || c.isDefault;
    }
    if (isError(st))
        kind = SelKind::Other;
    else if (nu && (nu->k == TK::Int || nu->k == TK::Char || nu->k == TK::Short || nu->k == TK::Byte))
        kind = SelKind::Integral;
    else if (isStringType(st))
        kind = SelKind::String;
    else if (st->k == TK::Class && st->cls && st->cls->isEnum())
        kind = SelKind::Enum;
    else if (nu || st->k == TK::Boolean)
    {
        if (!hasPatterns)
            error(sw->selector->pos, "constant label of type " + typeStr(st) + " is not compatible with switch selector");
    }
    std::string selVar = temp();
    std::vector<ASTNodePtr> out;
    ASTNodePtr selNode;
    if (kind == SelKind::Integral)
        selNode = coerce(std::move(sel), prim(TK::Int), sw->selector->pos);
    else
        selNode = std::move(sel.node);
    if (isRef(st) && !hasNull)
        selNode = q::un(NONNULL, std::move(selNode), l);
    out.push_back(q::exprStmt(q::assign(q::id(selVar, l), std::move(selNode), l), l));
    // labels -> match expressions
    std::set<std::string> seenLabels;
    auto labelMatch = [&](const ExprP &lab) -> ASTNodePtr
    {
        if (kind == SelKind::Enum && lab->k == EK::Name)
        {
            FieldSym *f = findField(st->cls, lab->name);
            if (!f || !f->isStatic)
            {
                error(lab->pos, "an enum switch case label must be the unqualified name of an enumeration constant");
                return q::boolean(false, l);
            }
            if (!seenLabels.insert(lab->name).second)
                error(lab->pos, "duplicate case label");
            return q::bin(REFEQ, q::id(selVar, l), q::id(f->key, l), l);
        }
        Val v = expr(lab);
        if (isError(v.type))
            return q::boolean(false, l);
        if (kind == SelKind::Enum && v.type->cls == st->cls)
            return q::bin(REFEQ, q::id(selVar, l), std::move(v.node), l);
        if (!v.cv.ok())
        {
            error(lab->pos, kind == SelKind::Enum ? "an enum switch case label must be the unqualified name of an enumeration constant"
                                                 : "constant expression required");
            return q::boolean(false, l);
        }
        std::string key = v.cv.k == Const::Str ? "s:" + v.cv.s : std::to_string(v.cv.i);
        if (!seenLabels.insert(key).second)
            error(lab->pos, "duplicate case label");
        if (kind == SelKind::String)
        {
            if (!isStringType(v.type))
                error(lab->pos, "incompatible types: " + typeStr(v.type) + " cannot be converted to String");
            return q::bin(REFEQ, q::id(selVar, l), std::move(v.node), l);
        }
        if (kind == SelKind::Integral)
        {
            if (!assignable(v.type, nu, &v.cv) && !(isPrim(v.type) && isIntegral(v.type)))
                error(lab->pos, "incompatible types: " + typeStr(v.type) + " cannot be converted to " + typeStr(nu));
            else if (!assignable(v.type, nu, &v.cv))
                error(lab->pos, "incompatible types: possible lossy conversion from " + typeStr(v.type) + " to " + typeStr(nu));
            return q::bin(EQ, q::id(selVar, l), q::lng(v.cv.i, l), l);
        }
        return q::bin(REFEQ, q::id(selVar, l), std::move(v.node), l);
    };
    // per case: its match condition (pattern bindings assigned inside)
    struct Group
    {
        ASTNodePtr match; // null for default-only
        bool isDefault = false;
        std::string bindVar, bindQ;
        TypeP bindType;
    };
    std::vector<Group> groups;
    pushScope();
    for (auto &c : sw->cases)
    {
        Group g;
        g.isDefault = c.isDefault;
        ASTNodePtr m;
        for (auto &lab : c.labels)
        {
            ASTNodePtr one = labelMatch(lab);
            m = m ? q::qbin("||", std::move(m), std::move(one), l) : std::move(one);
        }
        if (c.hasNullLabel)
        {
            ASTNodePtr one = q::bin(REFEQ, q::id(selVar, l), q::nil(l), l);
            m = m ? q::qbin("||", std::move(m), std::move(one), l) : std::move(one);
        }
        if (c.patternType)
        {
            TypeP pt = resolveType(c.patternType);
            if (!isError(st) && !isError(pt) && !castable(st, pt))
                error(c.patternType->pos, "incompatible types: " + typeStr(st) + " cannot be converted to " + typeStr(pt));
            g.bindType = pt;
            g.bindVar = c.patternVar;
            ASTNodePtr test = q::instOf(q::id(selVar, l), descriptor(pt), l);
            if (c.guard)
            {
                // the binding is visible in the guard
                pushScope();
                LocalVar &lv = declareLocal(c.patternVar == "_" ? temp() : c.patternVar, pt, c.pos, false);
                lv.hasInit = true;
                markAssigned(&lv);
                lv.q = lv.name + "$p" + std::to_string(++tempCounter_);
                fn_->temps.push_back(lv.q);
                g.bindQ = lv.q;
                std::vector<ASTNodePtr> gp;
                gp.push_back(q::assign(q::id(lv.q, l), q::id(selVar, l), l));
                gp.push_back(cond(c.guard));
                popScope();
                test = q::qbin("&&", std::move(test), q::seq(std::move(gp), l), l);
            }
            m = std::move(test);
        }
        g.match = std::move(m);
        groups.push_back(std::move(g));
    }
    popScope();
    // exhaustiveness: patterns over a sealed type / all enum constants
    if (hasPatterns && !hasDefault)
    {
        bool covered = false;
        TypeP ub = upperBound(st);
        if (ub->k == TK::Class && ub->cls)
        {
            ClassSym *sc = ub->cls;
            std::vector<ClassSym *> permitted;
            if (sc->decl && (sc->mods & M_SEALED))
                for (auto &pr : sc->decl->permits)
                {
                    TypeP pt = resolveType(pr);
                    if (pt->cls)
                        permitted.push_back(pt->cls);
                }
            covered = !permitted.empty();
            for (ClassSym *p : permitted)
            {
                bool hit = false;
                for (size_t i = 0; i < sw->cases.size(); ++i)
                    if (sw->cases[i].patternType && !sw->cases[i].guard && groups[i].bindType && groups[i].bindType->cls &&
                        isSubClass(p, groups[i].bindType->cls))
                        hit = true;
                covered = covered && hit;
            }
            for (size_t i = 0; i < sw->cases.size(); ++i)
                if (sw->cases[i].patternType && !sw->cases[i].guard && groups[i].bindType && isSubtype(st, groups[i].bindType))
                    covered = true;
        }
        if (!covered)
            error(sw->pos, "the switch statement does not cover all possible input values");
    }
    // the body: a loop that runs once (so `break` leaves the switch)
    LoopScope ls(*this, label, false, true);
    std::vector<ASTNodePtr> body;
    Flow entry = fn_->flow;
    Flow fallOut;
    bool anyAlive = false;
    bool lastAlive = true;
    if (sw->arrow)
    {
        // no fall through: an if / else-if chain
        ASTNodePtr chain;
        std::vector<ASTNodePtr> arms(sw->cases.size());
        std::vector<Flow> armFlows;
        int defaultIdx = -1;
        for (size_t i = 0; i < sw->cases.size(); ++i)
        {
            const SwitchCase &c = sw->cases[i];
            pushScope();
            fn_->flow = entry;
            std::vector<ASTNodePtr> arm;
            if (!groups[i].bindVar.empty())
            {
                if (!groups[i].bindQ.empty())
                {
                    LocalVar &lv = declareLocal(groups[i].bindVar, groups[i].bindType, c.pos, false);
                    lv.hasInit = true;
                    markAssigned(&lv);
                    lv.q = groups[i].bindQ;
                }
                else if (groups[i].bindVar != "_")
                {
                    LocalVar &lv = declareLocal(groups[i].bindVar, groups[i].bindType, c.pos, false);
                    lv.hasInit = true;
                    markAssigned(&lv);
                    arm.push_back(defineLocal(lv.q, q::id(selVar, l), l));
                }
            }
            if (c.arrowExpr)
            {
                Val v = expr(c.arrowExpr);
                arm.push_back(q::exprStmt(std::move(v.node), l));
            }
            else
                for (auto &x : c.body)
                    if (ASTNodePtr n = stmt(x))
                        arm.push_back(std::move(n));
            popScope();
            armFlows.push_back(fn_->flow);
            anyAlive = anyAlive || fn_->flow.alive;
            arms[i] = q::block(std::move(arm), l);
            if (c.isDefault && !groups[i].match)
                defaultIdx = (int)i;
        }
        chain = defaultIdx >= 0 ? std::move(arms[defaultIdx]) : nullptr;
        for (size_t i = sw->cases.size(); i-- > 0;)
        {
            if ((int)i == defaultIdx)
                continue;
            ASTNodePtr m = groups[i].match ? std::move(groups[i].match) : q::boolean(true, l);
            if (sw->cases[i].isDefault)
                m = q::boolean(true, l); // case null, default
            chain = q::ifs(std::move(m), std::move(arms[i]), std::move(chain), l);
        }
        if (chain)
            body.push_back(std::move(chain));
        Flow merged;
        merged.alive = false;
        for (auto &f : armFlows)
            merged = joinFlow(merged, f);
        if (!hasDefault && !hasPatterns)
            merged = joinFlow(merged, entry);
        fallOut = merged;
    }
    else
    {
        // fall through: which group to start at, then every group from there on
        std::string idx = temp();
        int defaultIdx = (int)sw->cases.size();
        for (size_t i = 0; i < sw->cases.size(); ++i)
            if (sw->cases[i].isDefault)
                defaultIdx = (int)i;
        ASTNodePtr pick = q::lng(defaultIdx, l);
        for (size_t i = sw->cases.size(); i-- > 0;)
            if (groups[i].match)
                pick = q::ternary(std::move(groups[i].match), q::lng((int64_t)i, l), std::move(pick), l);
        body.push_back(q::exprStmt(q::assign(q::id(idx, l), std::move(pick), l), l));
        Flow cur;
        cur.alive = false;
        pushScope(); // all groups share one scope (a local declared in one is visible in the next)
        for (size_t i = 0; i < sw->cases.size(); ++i)
        {
            const SwitchCase &c = sw->cases[i];
            fn_->flow = joinFlow(cur, entry);
            fn_->flow.alive = true;
            std::vector<ASTNodePtr> grp;
            if (!groups[i].bindVar.empty() && groups[i].bindVar != "_")
            {
                LocalVar &lv = declareLocal(groups[i].bindVar, groups[i].bindType, c.pos, false);
                lv.hasInit = true;
                markAssigned(&lv);
                if (!groups[i].bindQ.empty())
                    lv.q = groups[i].bindQ;
                else
                    grp.push_back(q::exprStmt(q::assign(q::id(lv.q, l), q::id(selVar, l), l), l));
                if (groups[i].bindQ.empty())
                    fn_->temps.push_back(lv.q);
            }
            bool unreachableReported = false;
            for (auto &x : c.body)
            {
                if (!fn_->flow.alive && !unreachableReported)
                {
                    error(x->pos, "unreachable statement");
                    unreachableReported = true;
                    fn_->flow.alive = true;
                }
                if (x->k == SK::LocalVar)
                {
                    localVar(*x, grp);
                    continue;
                }
                if (ASTNodePtr n = stmt(x))
                    grp.push_back(std::move(n));
            }
            cur = fn_->flow;
            body.push_back(q::ifs(q::bin(LE, q::id(idx, l), q::lng((int64_t)i, l), l), q::block(std::move(grp), l), nullptr, l));
        }
        popScope();
        lastAlive = cur.alive || sw->cases.empty();
        fallOut = cur;
        if (!hasDefault && !hasPatterns)
            fallOut = joinFlow(fallOut, entry);
        anyAlive = lastAlive;
    }
    body.push_back(q::brk(l));
    JumpTarget done;
    std::vector<ASTNodePtr> loop = ls.close(q::whiles(q::boolean(true, l), q::block(std::move(body), l), nullptr, l), l, done);
    for (auto &x : loop)
        out.push_back(std::move(x));
    Flow exitFlow = fallOut;
    if (!hasDefault && !hasPatterns)
        exitFlow.alive = true;
    if (done.breakHit)
        exitFlow = joinFlow(exitFlow, done.breakFlow), exitFlow.alive = true;
    fn_->flow = exitFlow;
    (void)anyAlive;
    return q::block(std::move(out), l);
}

// switch expression: arms of expressions (and throws) become a conditional
// chain; arms with blocks run in an immediately-invoked closure where
// `yield v` returns v.
Val Sema::switchExpr(const ExprP &e, const TypeP &target)
{
    int l = e->pos.line;
    auto &sw = e->sw;
    bool blocks = false;
    bool hasDefault = false, hasPatterns = false;
    for (auto &c : sw->cases)
    {
        if (!c.arrowExpr)
            blocks = true;
        hasDefault = hasDefault || c.isDefault;
        hasPatterns = hasPatterns || c.patternType;
        if (!c.body.empty() && c.body[0]->k == SK::Throw && c.body.size() == 1 && sw->arrow)
            ; // a throw arm
    }
    // the result type: the target, else what the arms produce
    TypeP rt = target && !isError(target) ? target : nullptr;
    if (!rt)
    {
        size_t saved = diags_.size();
        TypeP acc;
        bool allBool = true, allNumeric = true;
        std::vector<TypeP> armTypes;
        FnCtx probe;
        probe.cls = curClass();
        probe.isStatic = inStaticContext();
        probe.outer = fn_;
        probe.scopes.emplace_back();
        FnCtx *savedFn = fn_;
        fn_ = &probe;
        ++probing_;
        Val sel = expr(sw->selector);
        (void)sel;
        for (auto &c : sw->cases)
        {
            std::vector<TypeP> ts;
            if (c.arrowExpr)
                ts.push_back(expr(c.arrowExpr).type);
            else
            {
                // yields inside a block
                std::function<void(const StmtP &)> walk = [&](const StmtP &st) {
                    if (!st)
                        return;
                    if (st->k == SK::Yield && st->e)
                    {
                        ts.push_back(expr(st->e).type);
                        return;
                    }
                    for (auto &x : st->stmts)
                        walk(x);
                    walk(st->s1);
                    walk(st->s2);
                };
                for (auto &x : c.body)
                    walk(x);
            }
            for (auto &t : ts)
                armTypes.push_back(t);
        }
        --probing_;
        fn_ = savedFn;
        diags_.resize(saved);
        for (auto &t : armTypes)
        {
            if (isError(t))
                continue;
            allBool = allBool && isBooleanish(t);
            allNumeric = allNumeric && numericUnboxed(t);
        }
        for (auto &t : armTypes)
        {
            if (isError(t))
                continue;
            if (allNumeric && !allBool)
                acc = acc ? binaryPromote(acc, t) : numericUnboxed(t);
            else if (allBool)
                acc = prim(TK::Boolean);
            else
                acc = acc ? lub(acc, t) : (isPrim(t) ? boxOf(t) : t);
        }
        rt = acc ? acc : tObject();
    }
    // exhaustiveness
    TypeP st0;
    {
        size_t saved = diags_.size();
        FnCtx probe;
        probe.cls = curClass();
        probe.isStatic = inStaticContext();
        probe.outer = fn_;
        probe.scopes.emplace_back();
        FnCtx *savedFn = fn_;
        fn_ = &probe;
        ++probing_;
        st0 = expr(sw->selector).type;
        --probing_;
        fn_ = savedFn;
        diags_.resize(saved);
    }
    if (!hasDefault && !hasPatterns && st0 && st0->k == TK::Class && st0->cls && st0->cls->isEnum())
    {
        std::set<std::string> names;
        for (auto &c : sw->cases)
            for (auto &lab : c.labels)
                if (lab->k == EK::Name)
                    names.insert(lab->name);
        bool all = true;
        if (st0->cls->decl)
            for (auto &ec : st0->cls->decl->enumConsts)
                all = all && names.count(ec.name);
        if (!all)
            error(e->pos, "the switch expression does not cover all possible input values");
        hasDefault = true; // complete
    }
    else if (!hasDefault && !hasPatterns && !isError(st0))
        error(e->pos, "the switch expression does not cover all possible input values");
    // lower as a statement switch inside a closure: arms return their value
    auto stmtSw = std::make_shared<SwitchBody>(*sw);
    for (auto &c : stmtSw->cases)
    {
        if (c.arrowExpr)
        {
            auto y = std::make_shared<Stmt>();
            y->k = SK::Yield;
            y->pos = c.arrowExpr->pos;
            y->e = c.arrowExpr;
            c.body.clear();
            c.body.push_back(y);
            c.arrowExpr = nullptr;
        }
    }
    // the closure: its own loop/finally state; yields are returns
    FnCtx ctx;
    ctx.cls = curClass();
    ctx.isStatic = inStaticContext();
    ctx.isLambda = true;
    ctx.transparent = true;
    ctx.outer = fn_;
    ctx.ret = rt;
    ctx.flow = fn_->flow;
    ctx.excScopes = fn_->excScopes;
    ctx.throwsAllowed = fn_->throwsAllowed;
    ctx.checkExceptions = fn_->checkExceptions;
    ctx.name = fn_->name;
    FnCtx *saved = fn_;
    fn_ = &ctx;
    ctx.scopes.emplace_back();
    yieldTypes_.push_back(rt);
    ASTNodePtr inner = switchStmt(stmtSw, "", l);
    yieldTypes_.pop_back();
    std::vector<ASTNodePtr> stmts;
    stmts.push_back(std::move(inner));
    // (unreachable: every path yields or throws)
    stmts.push_back(q::ret(q::nil(l), l));
    ASTNodePtr body = lowerFunctionBody(ctx, [&](std::vector<ASTNodePtr> &out) {
        for (auto &s : stmts)
            out.push_back(std::move(s));
    }, l);
    saved->flow = joinFlow(saved->flow, saved->flow);
    // propagate checked-exception bookkeeping
    for (size_t i = 0; i < saved->excScopes.size() && i < ctx.excScopes.size(); ++i)
        for (size_t k = 0; k < saved->excScopes[i].thrown.size(); ++k)
        {
            saved->excScopes[i].thrown[k] = saved->excScopes[i].thrown[k] || ctx.excScopes[i].thrown[k];
            for (auto &t : ctx.excScopes[i].caught[k])
                saved->excScopes[i].caught[k].push_back(t);
        }
    fn_ = saved;
    (void)blocks;
    Val r;
    r.type = rt;
    r.node = q::call(q::lambda({}, std::move(body), l), {}, l);
    return r;
}

ASTNodePtr Sema::localClass(const Stmt &s)
{
    ClassDecl *d = s.cls.get();
    ClassSym *host = curClass();
    ClassSym *top = host;
    while (top && top->outer)
        top = top->outer;
    ClassSym *c = enterClass(d, host, unit_, host ? host->pkg : "");
    c->isLocal = true;
    c->binaryName = top->binaryName + "$" + std::to_string(++top->localCount) + d->name;
    byBinary_[c->binaryName] = c;
    c->enclosingFn = fn_;
    bool implicitlyStatic = d->kind != ClassDecl::Class;
    c->hasOuterInstance = !implicitlyStatic && !inStaticContext();
    if (!implicitlyStatic)
        for (FnCtx *f = fn_; f; f = f->outer)
            for (auto &sc : f->scopes)
                for (auto &v : sc.vars)
                {
                    bool dup = false;
                    for (auto &cap : c->captures)
                        dup = dup || cap.name == v.name;
                    if (!dup)
                        c->captures.push_back(Capture{v.name, v.type, "val$" + v.name});
                }
    if (!fn_->scopes.empty())
    {
        if (fn_->scopes.back().classes.count(d->name))
            error(d->namePos, "duplicate class: " + d->name);
        fn_->scopes.back().classes[d->name] = c;
    }
    enterMembers(c);
    assignKeys(c);
    checkClass(c);
    lowerClass(c);
    return nullptr;
}

} // namespace java
