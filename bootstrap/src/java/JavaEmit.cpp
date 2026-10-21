// The lowered program: Quantum AST builders, classes (methods, constructors,
// initializers, synthesized members), dispatch tables, and the entry point.
#include "JavaSema.h"

#include <algorithm>
#include <functional>

std::string javaDoubleToString(double d); // src/vm/VmJavaLib.cpp
std::string javaFloatToString(double f);

namespace java
{

using namespace jop;

std::string javaDoubleToStringC(double d, bool isFloat) { return isFloat ? ::javaFloatToString(d) : ::javaDoubleToString(d); }

namespace
{
Sema *g_activeSema = nullptr; // for q::inv: the dispatch keys the lowered code calls through
}

// ─── Quantum AST builders ────────────────────────────────────────────────────

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
ASTNodePtr lng(int64_t v, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(str(std::to_string(v), ln));
    return call("__jx_int__", std::move(a), ln);
}
ASTNodePtr assign(ASTNodePtr target, ASTNodePtr value, int ln)
{
    return std::make_unique<ASTNode>(AssignExpr{"=", std::move(target), std::move(value)}, ln);
}
ASTNodePtr seq(std::vector<ASTNodePtr> parts, int ln)
{
    if (parts.size() == 1)
        return std::move(parts[0]);
    return call("__jx_seq__", std::move(parts), ln);
}
ASTNodePtr bin(int op, ASTNodePtr l, ASTNodePtr r, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(num(op, ln));
    a.push_back(std::move(l));
    a.push_back(std::move(r));
    return call("__jx_bin__", std::move(a), ln);
}
ASTNodePtr un(int op, ASTNodePtr x, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(num(op, ln));
    a.push_back(std::move(x));
    return call("__jx_un__", std::move(a), ln);
}
ASTNodePtr qbin(const std::string &op, ASTNodePtr l, ASTNodePtr r, int ln)
{
    return std::make_unique<ASTNode>(BinaryExpr{op, std::move(l), std::move(r)}, ln);
}
ASTNodePtr qnot(ASTNodePtr x, int ln) { return std::make_unique<ASTNode>(UnaryExpr{"!", std::move(x)}, ln); }
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
    for (auto &s : stmts)
        if (s)
            b.statements.push_back(std::move(s));
    return std::make_unique<ASTNode>(std::move(b), ln);
}
ASTNodePtr exprStmt(ASTNodePtr e, int ln) { return std::make_unique<ASTNode>(ExprStmt{std::move(e)}, ln); }
ASTNodePtr var(const std::string &n, ASTNodePtr init, int ln)
{
    VarDecl v;
    v.isConst = false;
    v.name = n;
    v.initializer = init ? std::move(init) : nil(ln);
    return std::make_unique<ASTNode>(std::move(v), ln);
}
ASTNodePtr ret(ASTNodePtr e, int ln) { return std::make_unique<ASTNode>(ReturnStmt{std::move(e)}, ln); }
ASTNodePtr ifs(ASTNodePtr c, ASTNodePtr t, ASTNodePtr e, int ln)
{
    return std::make_unique<ASTNode>(IfStmt{std::move(c), t ? std::move(t) : block({}, ln), std::move(e)}, ln);
}
ASTNodePtr whiles(ASTNodePtr c, ASTNodePtr body, ASTNodePtr post, int ln)
{
    return std::make_unique<ASTNode>(WhileStmt{std::move(c), body ? std::move(body) : block({}, ln), std::move(post)}, ln);
}
ASTNodePtr brk(int ln) { return std::make_unique<ASTNode>(BreakStmt{}, ln); }
ASTNodePtr cont(int ln) { return std::make_unique<ASTNode>(ContinueStmt{}, ln); }
ASTNodePtr raise(ASTNodePtr e, int ln) { return std::make_unique<ASTNode>(RaiseStmt{std::move(e)}, ln); }
ASTNodePtr tryCatch(ASTNodePtr body, const std::string &v, ASTNodePtr handler, int ln)
{
    TryStmt t;
    t.body = std::shared_ptr<ASTNode>(body.release());
    ExceptClause h;
    h.alias = v;
    h.body = std::shared_ptr<ASTNode>(handler.release());
    t.handlers.push_back(std::move(h));
    return std::make_unique<ASTNode>(std::move(t), ln);
}
ASTNodePtr func(const std::string &name, std::vector<std::string> params, ASTNodePtr body, int ln)
{
    FunctionDecl f;
    f.name = name;
    f.paramIsRef.assign(params.size(), false);
    f.params = std::move(params);
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
ASTNodePtr getf(ASTNodePtr obj, const std::string &field, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(std::move(obj));
    a.push_back(str(field, ln));
    return call("__jx_getf__", std::move(a), ln);
}
ASTNodePtr putf(ASTNodePtr obj, const std::string &field, ASTNodePtr v, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(std::move(obj));
    a.push_back(str(field, ln));
    a.push_back(std::move(v));
    return call("__jx_putf__", std::move(a), ln);
}
ASTNodePtr fupd(ASTNodePtr obj, const std::string &field, ASTNodePtr upd, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(std::move(obj));
    a.push_back(str(field, ln));
    a.push_back(std::move(upd));
    return call("__jx_fupd__", std::move(a), ln);
}
ASTNodePtr aload(ASTNodePtr arrN, ASTNodePtr i, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(std::move(arrN));
    a.push_back(std::move(i));
    return call("__jx_aload__", std::move(a), ln);
}
ASTNodePtr astore(ASTNodePtr arrN, ASTNodePtr i, ASTNodePtr v, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(std::move(arrN));
    a.push_back(std::move(i));
    a.push_back(std::move(v));
    return call("__jx_astore__", std::move(a), ln);
}
ASTNodePtr aupd(ASTNodePtr arrN, ASTNodePtr i, ASTNodePtr upd, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(std::move(arrN));
    a.push_back(std::move(i));
    a.push_back(std::move(upd));
    return call("__jx_aupd__", std::move(a), ln);
}
ASTNodePtr old(int ln) { return call("__jx_old__", {}, ln); }
ASTNodePtr inv(const std::string &key, ASTNodePtr recv, std::vector<ASTNodePtr> args, int ln, const std::string &fallback)
{
    if (g_activeSema)
        g_activeSema->useKey(key);
    std::vector<ASTNodePtr> a;
    if (fallback.empty())
        a.push_back(str(key, ln));
    else
    {
        std::vector<ASTNodePtr> d;
        d.push_back(str(key, ln));
        d.push_back(str(fallback, ln));
        a.push_back(arr(std::move(d), ln));
    }
    a.push_back(std::move(recv));
    for (auto &x : args)
        a.push_back(std::move(x));
    return call("__jx_inv__", std::move(a), ln);
}
ASTNodePtr instOf(ASTNodePtr v, const std::string &desc, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(std::move(v));
    a.push_back(str(desc, ln));
    return call("__jx_instof__", std::move(a), ln);
}
ASTNodePtr cast(ASTNodePtr v, const std::string &desc, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(std::move(v));
    a.push_back(str(desc, ln));
    return call("__jx_cast__", std::move(a), ln);
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
    if (auto *x = std::get_if<BinaryExpr>(&v)) return qbin(x->op, clone(x->left), clone(x->right), ln);
    if (auto *x = std::get_if<UnaryExpr>(&v)) return std::make_unique<ASTNode>(UnaryExpr{x->op, clone(x->operand)}, ln);
    if (auto *x = std::get_if<TernaryExpr>(&v))
        return ternary(clone(x->condition), clone(x->thenExpr), clone(x->elseExpr), ln);
    if (auto *x = std::get_if<CallExpr>(&v))
    {
        std::vector<ASTNodePtr> a;
        for (auto &y : x->args)
            a.push_back(clone(y));
        return call(clone(x->callee), std::move(a), ln);
    }
    if (auto *x = std::get_if<ArrayLiteral>(&v))
    {
        std::vector<ASTNodePtr> e;
        for (auto &y : x->elements)
            e.push_back(clone(y));
        return arr(std::move(e), ln);
    }
    if (auto *x = std::get_if<AssignExpr>(&v))
        return std::make_unique<ASTNode>(AssignExpr{x->op, clone(x->target), clone(x->value)}, ln);
    if (auto *x = std::get_if<BlockStmt>(&v))
    {
        std::vector<ASTNodePtr> s;
        for (auto &y : x->statements)
            s.push_back(clone(y));
        return block(std::move(s), ln);
    }
    if (auto *x = std::get_if<ExprStmt>(&v)) return exprStmt(clone(x->expr), ln);
    if (auto *x = std::get_if<IfStmt>(&v)) return ifs(clone(x->condition), clone(x->thenBranch), clone(x->elseBranch), ln);
    if (auto *x = std::get_if<ReturnStmt>(&v)) return ret(clone(x->value), ln);
    if (auto *x = std::get_if<RaiseStmt>(&v)) return raise(clone(x->value), ln);
    if (std::get_if<BreakStmt>(&v)) return brk(ln);
    if (std::get_if<ContinueStmt>(&v)) return cont(ln);
    if (auto *x = std::get_if<VarDecl>(&v)) return var(x->name, clone(x->initializer), ln);
    if (auto *x = std::get_if<WhileStmt>(&v)) return whiles(clone(x->condition), clone(x->body), clone(x->post), ln);
    return nil(ln);
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
    if (auto *c = std::get_if<CallExpr>(&v))
        return c->callee->is<Identifier>() && c->callee->as<Identifier>().name == "__jx_int__";
    return false;
}
} // namespace q

// ─── Reachability ────────────────────────────────────────────────────────────

void Sema::need(ClassSym *c)
{
    if (!c || c->reachable)
        return;
    c->reachable = true;
    reachable_.push_back(c);
    lowerQueue_.push_back(c);
    enterMembers(c);
    assignKeys(c);
    if (c->super && c->super->cls)
        need(c->super->cls);
    for (auto &i : c->ifaces)
        if (i->cls)
            need(i->cls);
    for (MethodSym *m : c->methods)
        if (!m->isStatic() && implementsUsedKey(m))
            queueMethod(m);
}

bool Sema::implementsUsedKey(const MethodSym *m) const
{
    if (calledKeys_.count(m->vkey))
        return true;
    for (auto &k : m->bridgeKeys)
        if (calledKeys_.count(k))
            return true;
    return false;
}

void Sema::queueMethod(MethodSym *m)
{
    if (!m || m->queued || m->lowered)
        return;
    if (m->isAbstract() && m->synth == Synth::None)
        return;
    if (!m->native.empty() && m->isStatic())
        return; // called as the native itself
    m->queued = true;
    methodQueue_.push_back(m);
}

void Sema::needMethod(MethodSym *m)
{
    if (!m || !m->owner)
        return;
    need(m->owner);
    queueMethod(m);
}

void Sema::useKey(const std::string &key)
{
    if (!calledKeys_.insert(key).second)
        return;
    for (size_t i = 0; i < reachable_.size(); ++i)
        for (MethodSym *m : reachable_[i]->methods)
            if (!m->isStatic() && (m->vkey == key || std::find(m->bridgeKeys.begin(), m->bridgeKeys.end(), key) !=
                                                         m->bridgeKeys.end()))
                queueMethod(m);
}

MethodSym *Sema::needNamed(const std::string &clsName, const std::string &method)
{
    ClassSym *c = cls(clsName);
    if (!c)
        return nullptr;
    enterMembers(c);
    assignKeys(c);
    for (MethodSym *m : c->methods)
        if (m->name == method)
        {
            needMethod(m);
            return m;
        }
    return nullptr;
}

// ─── Functions ───────────────────────────────────────────────────────────────

ASTNodePtr Sema::lowerFunctionBody(FnCtx &ctx, const std::function<void(std::vector<ASTNodePtr> &)> &body, int l)
{
    FnCtx *saved = fn_;
    fn_ = &ctx;
    std::vector<ASTNodePtr> stmts;
    body(stmts);
    while (ctx.scopes.size() > 1)
        popScope();
    if (!ctx.scopes.empty())
        popScope();
    fn_ = saved;
    std::vector<ASTNodePtr> all;
    for (auto &t : ctx.temps)
        all.push_back(q::var(t, q::nil(l), l));
    for (auto &s : stmts)
        all.push_back(std::move(s));
    return q::block(std::move(all), l);
}

ASTNodePtr Sema::defineLocal(const std::string &qn, ASTNodePtr init, int l)
{
    if (fn_ && std::find(fn_->temps.begin(), fn_->temps.end(), qn) == fn_->temps.end())
        fn_->temps.push_back(qn);
    return q::exprStmt(q::assign(q::id(qn, l), init ? std::move(init) : q::nil(l), l), l);
}

namespace
{
std::string fileOf(const CompilationUnit &u)
{
    std::string f = u.fileName;
    size_t slash = f.find_last_of("/\\");
    return slash == std::string::npos ? f : f.substr(slash + 1);
}
} // namespace

void Sema::lowerMethod(MethodSym *m)
{
    if (m->lowered || !m->owner)
        return;
    ClassSym *c = m->owner;
    if (m->isAbstract() && m->synth == Synth::None)
        return;
    m->lowered = true;
    int savedUnit = unit_;
    ClassSym *savedCtx = ctxClass_;
    FnCtx *savedFn = fn_;
    unit_ = c->unit;
    ctxClass_ = c;
    int l = m->decl ? m->decl->pos.line : (c->decl ? c->decl->pos.line : 0);

    FnCtx ctx;
    ctx.cls = c;
    ctx.method = m;
    ctx.isStatic = m->isStatic();
    ctx.isCtor = m->isCtor;
    ctx.name = m->isCtor ? "new" : m->name;
    ctx.throwsAllowed = m->throws;
    ctx.checkExceptions = !units_[c->unit].isPrelude;
    ctx.scopes.emplace_back();
    fn_ = &ctx;
    for (auto *tv : m->tparams)
        methodTvars_.push_back(tv);

    std::vector<std::string> params;
    std::vector<ASTNodePtr> pre;
    if (!m->isStatic())
        params.push_back("__this");
    bool enumCtor = m->isCtor && (c->isEnum() || (c->isAnon && c->super && c->super->cls && c->super->cls->isEnum()));
    if (m->isCtor)
    {
        if (c->hasOuterInstance)
        {
            params.push_back("__outer");
            pre.push_back(q::exprStmt(q::putf(thisNode(l), "this$0", q::id("__outer", l), l), l));
        }
        for (auto &cap : c->captures)
        {
            std::string p = "__cap_" + cap.name;
            params.push_back(p);
            pre.push_back(q::exprStmt(q::putf(thisNode(l), cap.field, q::id(p, l), l), l));
        }
        if (enumCtor)
        {
            params.push_back("__name");
            params.push_back("__ordinal");
        }
    }
    // the declared parameters are locals
    for (size_t i = 0; i < m->params.size(); ++i)
    {
        std::string pn = i < m->paramNames.size() && !m->paramNames[i].empty() ? m->paramNames[i] : "p" + std::to_string(i);
        Pos pp = m->pos;
        if (m->decl && i < m->decl->params.size())
            pp = m->decl->params[i].pos;
        if (findLocal(pn))
            error(pp, "variable " + pn + " is already defined in method " + sigStr(m));
        bool fin = m->decl && i < m->decl->params.size() && m->decl->params[i].mods.has(M_FINAL);
        LocalVar &v = declareLocal(pn, m->params[i], pp, fin);
        v.hasInit = true;
        markAssigned(&v);
        params.push_back(v.q);
    }

    ASTNodePtr body = lowerFunctionBody(ctx, [&](std::vector<ASTNodePtr> &out) {
        for (auto &p : pre)
            out.push_back(std::move(p));
        // ── synthesized members ──
        switch (m->synth)
        {
        case Synth::RecordAccessor:
            out.push_back(q::ret(q::getf(thisNode(l), m->field->key, l), l));
            return;
        case Synth::RecordEquals:
        {
            // o instanceof R r && fields equal
            std::string other = "o";
            ASTNodePtr test = q::instOf(q::id(other, l), descriptor(classType(c)), l);
            for (FieldSym *f : c->fields)
            {
                if (!f->synthetic)
                    continue;
                ASTNodePtr a = q::getf(thisNode(l), f->key, l), b = q::getf(q::id(other, l), f->key, l);
                ASTNodePtr eq;
                if (isPrim(f->type))
                    eq = f->type->k == TK::Float || f->type->k == TK::Double
                             ? q::bin(EQ, q::call("__jn_Double_compare", [&] {
                                                      std::vector<ASTNodePtr> x;
                                                      x.push_back(std::move(a));
                                                      x.push_back(std::move(b));
                                                      return x;
                                                  }(), l),
                                      q::lng(0, l), l)
                             : q::bin(EQ, std::move(a), std::move(b), l);
                else
                {
                    needNamed("java.util.Objects", "equals");
                    std::vector<ASTNodePtr> x;
                    x.push_back(std::move(a));
                    x.push_back(std::move(b));
                    eq = q::call("java.util.Objects.equals", std::move(x), l);
                }
                test = q::qbin("&&", std::move(test), std::move(eq), l);
            }
            out.push_back(q::ret(std::move(test), l));
            return;
        }
        case Synth::RecordHashCode:
        {
            // 31 * h + hash(component), from 0
            ASTNodePtr h = q::lng(0, l);
            for (FieldSym *f : c->fields)
            {
                if (!f->synthetic)
                    continue;
                ASTNodePtr fv = q::getf(thisNode(l), f->key, l);
                ASTNodePtr hv;
                if (f->type->k == TK::Boolean)
                    hv = q::ternary(std::move(fv), q::lng(1231, l), q::lng(1237, l), l);
                else if (isIntegral(f->type) && f->type->k != TK::Long)
                    hv = std::move(fv);
                else
                {
                    needNamed("java.util.Objects", "hashCode");
                    std::vector<ASTNodePtr> x;
                    x.push_back(std::move(fv));
                    hv = q::call("java.util.Objects.hashCode", std::move(x), l);
                }
                h = q::bin(IADD, q::bin(IMUL, std::move(h), q::lng(31, l), l), std::move(hv), l);
            }
            out.push_back(q::ret(std::move(h), l));
            return;
        }
        case Synth::RecordToString:
        {
            // Name[x=1, y=2]
            ASTNodePtr s = q::str(c->name + "[", l);
            bool first = true;
            for (FieldSym *f : c->fields)
            {
                if (!f->synthetic)
                    continue;
                Val fv;
                fv.node = q::getf(thisNode(l), f->key, l);
                fv.type = f->type;
                s = q::bin(SCAT, std::move(s), q::str((first ? "" : ", ") + f->name + "=", l), l);
                s = q::bin(SCAT, std::move(s), stringOf(std::move(fv), l), l);
                first = false;
            }
            s = q::bin(SCAT, std::move(s), q::str("]", l), l);
            out.push_back(q::ret(std::move(s), l));
            return;
        }
        case Synth::EnumValues:
        {
            std::vector<ASTNodePtr> a;
            a.push_back(q::id(c->valuesGlobal(), l));
            out.push_back(q::ret(q::call("__j_aclone", std::move(a), l), l));
            return;
        }
        case Synth::EnumValueOf:
        {
            std::string i = temp();
            std::vector<ASTNodePtr> body2;
            ASTNodePtr el = q::aload(q::id(c->valuesGlobal(), l), q::id(i, l), l);
            std::vector<ASTNodePtr> nameArgs;
            nameArgs.push_back(q::getf(q::clone(el), "name", l));
            nameArgs.push_back(q::id("name", l));
            body2.push_back(q::ifs(q::bin(REFEQ, q::getf(std::move(el), "name", l), q::un(NONNULL, q::id("name", l), l), l),
                                   q::block([&] {
                                       std::vector<ASTNodePtr> r;
                                       r.push_back(q::ret(q::aload(q::id(c->valuesGlobal(), l), q::id(i, l), l), l));
                                       return r;
                                   }(), l),
                                   nullptr, l));
            out.push_back(q::exprStmt(q::assign(q::id(i, l), q::lng(0, l), l), l));
            out.push_back(q::whiles(q::bin(LT, q::id(i, l), q::un(ALEN, q::id(c->valuesGlobal(), l), l), l),
                                    q::block(std::move(body2), l),
                                    q::exprStmt(q::assign(q::id(i, l), q::bin(IADD, q::id(i, l), q::lng(1, l), l), l), l), l));
            // No enum constant Main.Color.PURPLE
            std::string shown = c->binaryName;
            std::replace(shown.begin(), shown.end(), '$', '.');
            ClassSym *iae = cls("java.lang.IllegalArgumentException");
            need(iae);
            MethodSym *k = nullptr;
            for (MethodSym *x : iae->ctors)
                if (x->params.size() == 1 && isStringType(x->params[0]))
                    k = x;
            std::vector<ASTNodePtr> msg;
            msg.push_back(q::bin(SCAT, q::str("No enum constant " + shown + ".", l), q::un(SSTR, q::id("name", l), l), l));
            out.push_back(q::raise(construct(iae, k, std::move(msg), nullptr, l), l));
            return;
        }
        default:
            break;
        }
        if (!m->isCtor)
        {
            if (!m->native.empty())
            {
                // a virtual native method: its body calls the native
                std::vector<ASTNodePtr> a;
                for (auto &p : params)
                    a.push_back(q::id(p, l));
                out.push_back(q::ret(q::call(m->native, std::move(a), l), l));
                return;
            }
            if (!m->decl || !m->decl->body)
                return;
            out.push_back(block(m->decl->body->stmts, l, false));
            if (m->ret && m->ret->k != TK::Void && fn_->flow.alive && !isError(m->ret))
                error(m->decl->body->endPos, "missing return statement");
            return;
        }
        // ── constructors ──
        const std::vector<StmtP> *stmts = m->decl && m->decl->body ? &m->decl->body->stmts : nullptr;
        size_t start = 0;
        const Stmt *ctorCall = stmts && !stmts->empty() && (*stmts)[0]->k == SK::CtorCall ? (*stmts)[0].get() : nullptr;
        if (ctorCall)
            start = 1;
        if (c->isRecord() && ctorCall && !ctorCall->superCall && m->decl && m->decl->compactCtor)
            error(ctorCall->pos, "canonical constructor must not contain explicit constructor invocation");
        Pos at = m->decl ? m->decl->namePos : m->pos;
        bool delegates = ctorCall && !ctorCall->superCall;
        auto superArgsFor = [&](MethodSym *target, const std::vector<ExprP> &args, Pos p) -> std::vector<ASTNodePtr>
        {
            std::vector<MethodSym *> one{target};
            std::vector<Val> lowered;
            std::string notes;
            Resolved r = resolveCall(one, args, nullptr, nullptr, p, target->name, {}, lowered, notes);
            if (!r.m)
                return {};
            return lowerArgs(r, args, lowered, p);
        };
        if (delegates)
        {
            // this(...)
            std::vector<MethodSym *> cands = c->ctors;
            std::vector<Val> lowered;
            std::string notes;
            Resolved r = resolveCall(cands, ctorCall->args, classType(c), nullptr, ctorCall->pos, c->name, {}, lowered, notes);
            if (!r.m)
                error(ctorCall->pos, "constructor " + c->name + " in class " + c->name + " cannot be applied to given types;", {notes});
            else
            {
                if (r.m == m)
                    error(ctorCall->pos, "recursive constructor invocation");
                for (auto &t : r.m->throws)
                    thrown(t, ctorCall->pos);
                std::vector<ASTNodePtr> a;
                a.push_back(thisNode(l));
                if (c->hasOuterInstance)
                    a.push_back(q::id("__outer", l));
                for (auto &cap : c->captures)
                    a.push_back(q::id("__cap_" + cap.name, l));
                if (enumCtor)
                {
                    a.push_back(q::id("__name", l));
                    a.push_back(q::id("__ordinal", l));
                }
                for (auto &x : lowerArgs(r, ctorCall->args, lowered, ctorCall->pos))
                    a.push_back(std::move(x));
                needMethod(r.m);
                out.push_back(q::exprStmt(q::call(r.m->impl, std::move(a), l), l));
            }
        }
        else
        {
            ClassSym *sup = c->super ? c->super->cls : nullptr;
            if (sup && sup->binaryName != "java.lang.Object" && sup->binaryName != "java.lang.Record")
            {
                enterMembers(sup);
                assignKeys(sup);
                MethodSym *sk = nullptr;
                std::vector<ASTNodePtr> args;
                ASTNodePtr superOuter;
                if (c->isAnon && anonSuperCtor_.count(c))
                {
                    sk = anonSuperCtor_[c];
                    // the anonymous class's constructor passes its arguments through
                    size_t first = params.size() - m->params.size(), i0 = 0;
                    if (sup->hasOuterInstance && !m->params.empty() && m->paramNames[0] == "__souter")
                    {
                        superOuter = q::id(params[first], l);
                        i0 = 1;
                    }
                    for (size_t i = i0; i < m->params.size(); ++i)
                        args.push_back(q::id(params[first + i], l));
                }
                else if (sup->binaryName == "java.lang.Enum")
                {
                    for (MethodSym *x : sup->ctors)
                        if (x->params.size() == 2)
                            sk = x;
                    args.push_back(q::id("__name", l));
                    args.push_back(q::id("__ordinal", l));
                }
                else
                {
                    std::vector<MethodSym *> cands = sup->ctors;
                    std::vector<Val> lowered;
                    std::string notes;
                    static const std::vector<ExprP> none;
                    const std::vector<ExprP> &sargs = ctorCall ? ctorCall->args : none;
                    Pos p = ctorCall ? ctorCall->pos : at;
                    Resolved r = resolveCall(cands, sargs, c->super, nullptr, p, sup->name, {}, lowered, notes);
                    if (!r.m)
                    {
                        if (ctorCall)
                            error(p, "constructor " + sup->name + " in class " + typeStr(classType(sup)) + " cannot be applied to given types;", {notes});
                        else
                            error(p, "constructor " + sup->name + " in class " + typeStr(classType(sup)) + " cannot be applied to given types;", {notes});
                    }
                    else
                    {
                        if (r.m->isPrivate() && topLevelHelper(c) != topLevelHelper(sup))
                            error(p, sigStr(r.m) + " has private access in " + sup->name);
                        sk = r.m;
                        for (auto &t : r.m->throws)
                            thrown(t, p);
                        args = lowerArgs(r, sargs, lowered, p);
                    }
                }
                if (sk)
                {
                    std::vector<ASTNodePtr> a;
                    a.push_back(thisNode(l));
                    if (sup->hasOuterInstance)
                    {
                        if (superOuter)
                            a.push_back(std::move(superOuter));
                        else if (ctorCall && ctorCall->qualifier)
                            a.push_back(expr(ctorCall->qualifier).node);
                        else
                            a.push_back(outerInstance(sup->outer, at));
                    }
                    for (auto &cap : sup->captures)
                    {
                        // a local superclass's captures: the values seen here
                        Val cv = name([&] {
                            auto e = std::make_shared<Expr>();
                            e->k = EK::Name;
                            e->name = cap.name;
                            e->pos = at;
                            return e;
                        }(), true);
                        a.push_back(std::move(cv.node));
                    }
                    bool superIsEnum = sup->isEnum();
                    if (superIsEnum && sup->binaryName != "java.lang.Enum")
                    {
                        a.push_back(q::id("__name", l));
                        a.push_back(q::id("__ordinal", l));
                    }
                    for (auto &x : args)
                        a.push_back(std::move(x));
                    needMethod(sk);
                    out.push_back(q::exprStmt(q::call(sk->impl, std::move(a), l), l));
                }
            }
            else if (ctorCall && !ctorCall->args.empty())
                error(ctorCall->pos, "constructor Object in class Object cannot be applied to given types;");
            if (c->hasInstanceInit || std::any_of(c->fields.begin(), c->fields.end(), [](FieldSym *f) {
                    return !f->isStatic && f->var && f->var->init;
                }))
            {
                std::vector<ASTNodePtr> a;
                a.push_back(thisNode(l));
                out.push_back(q::exprStmt(q::call(c->fieldInitFn(), std::move(a), l), l));
            }
        }
        if (m->synth == Synth::RecordCtor)
        {
            for (FieldSym *f : c->fields)
                if (f->synthetic)
                    out.push_back(q::exprStmt(q::putf(thisNode(l), f->key, q::id(f->name, l), l), l));
        }
        else if (stmts)
        {
            for (size_t i = start; i < stmts->size(); ++i)
            {
                const StmtP &s = (*stmts)[i];
                if (!fn_->flow.alive)
                {
                    error(s->pos, "unreachable statement");
                    fn_->flow.alive = true;
                }
                if (s->k == SK::LocalVar)
                {
                    localVar(*s, out);
                    continue;
                }
                if (ASTNodePtr n = stmt(s))
                    out.push_back(std::move(n));
            }
            if (m->decl && m->decl->compactCtor)
                for (FieldSym *f : c->fields)
                    if (f->synthetic)
                        if (LocalVar *v = findLocal(f->name))
                            out.push_back(q::exprStmt(q::putf(thisNode(l), f->key, q::id(v->q, l), l), l));
        }
        out.push_back(q::ret(thisNode(l), l));
    }, l);
    methodTvars_.resize(methodTvars_.size() - m->tparams.size());
    functions_.push_back(q::func(m->impl, params, std::move(body), l));
    fn_ = savedFn;
    unit_ = savedUnit;
    ctxClass_ = savedCtx;
}

ClassSym *Sema::topLevelHelper(ClassSym *c)
{
    while (c && c->outer)
        c = c->outer;
    return c;
}

// ─── Classes ─────────────────────────────────────────────────────────────────

void Sema::lowerClass(ClassSym *c)
{
    if (!c || c->lowered)
        return;
    need(c);
    enterMembers(c);
    assignKeys(c);
    c->lowered = true;
    int savedUnit = unit_;
    ClassSym *savedCtx = ctxClass_;
    FnCtx *savedFn = fn_;
    unit_ = c->unit;
    ctxClass_ = c;
    const bool prelude = units_[c->unit].isPrelude;
    if (prelude)
        c->needsClinit = false; // prelude statics are set up before main runs
    int l = c->decl ? c->decl->pos.line : 0;
    // instance initializers: fields and blocks, in order
    bool anyInstanceInit = c->hasInstanceInit;
    for (FieldSym *f : c->fields)
        anyInstanceInit = anyInstanceInit || (!f->isStatic && f->var && f->var->init);
    if (anyInstanceInit && c->decl)
    {
        FnCtx ctx;
        ctx.cls = c;
        ctx.isStatic = false;
        ctx.name = "<init>";
        ctx.checkExceptions = !prelude;
        // checked exceptions every constructor declares are allowed
        if (!c->ctors.empty() && c->ctors[0]->decl)
            ctx.throwsAllowed = c->ctors[0]->throws;
        ctx.scopes.emplace_back();
        fn_ = &ctx;
        ASTNodePtr body = lowerFunctionBody(ctx, [&](std::vector<ASTNodePtr> &out) {
            for (auto &mem : c->decl->members)
            {
                if (mem.kind == Member::Field && !mem.field->mods.has(M_STATIC) && !c->isInterface())
                    for (auto &v : mem.field->vars)
                    {
                        if (!v.init)
                            continue;
                        FieldSym *f = nullptr;
                        for (FieldSym *x : c->fields)
                            if (x->var == &v)
                                f = x;
                        if (!f)
                            continue;
                        ASTNodePtr init = v.init->k == EK::ArrayLit ? (f->type->k == TK::Array ? arrayLit(v.init, f->type).node : q::nil(l))
                                                                    : exprTo(v.init, f->type, v.init->pos);
                        out.push_back(q::exprStmt(q::putf(thisNode(v.pos.line), f->key, std::move(init), v.pos.line), v.pos.line));
                    }
                else if (mem.kind == Member::Init && !mem.isStatic)
                {
                    out.push_back(block(mem.init->stmts, mem.init->pos.line));
                    fn_->flow.alive = true;
                }
            }
        }, l);
        functions_.push_back(q::func(c->fieldInitFn(), {"__this"}, std::move(body), l));
    }
    // static initialization: enum constants, static fields and blocks, in order
    bool anyStatic = c->isEnum();
    for (FieldSym *f : c->fields)
        anyStatic = anyStatic || (f->isStatic && f->var && f->var->init);
    if (c->decl)
        for (auto &mem : c->decl->members)
            anyStatic = anyStatic || (mem.kind == Member::Init && mem.isStatic);
    if (anyStatic && c->decl)
    {
        FnCtx ctx;
        ctx.cls = c;
        ctx.isStatic = true;
        ctx.name = "<clinit>";
        ctx.checkExceptions = !prelude;
        ctx.scopes.emplace_back();
        fn_ = &ctx;
        std::vector<ASTNodePtr> stmtsOut;
        ASTNodePtr body = lowerFunctionBody(ctx, [&](std::vector<ASTNodePtr> &out) {
            if (!prelude)
            {
                // run once; the superclass first
                std::vector<ASTNodePtr> once;
                once.push_back(q::ret(nullptr, l));
                out.push_back(q::ifs(q::id(c->initedFlag(), l), q::block(std::move(once), l), nullptr, l));
                out.push_back(q::exprStmt(q::assign(q::id(c->initedFlag(), l), q::boolean(true, l), l), l));
                if (c->super && c->super->cls && c->super->cls->needsClinit)
                    out.push_back(q::exprStmt(q::call(c->super->cls->clinitFn(), {}, l), l));
            }
            if (c->isEnum())
            {
                int ordinal = 0;
                std::vector<ASTNodePtr> all;
                for (auto &ec : c->decl->enumConsts)
                {
                    int el = ec.pos.line;
                    FieldSym *f = findField(c, ec.name);
                    std::vector<Val> lowered;
                    std::string notes;
                    std::vector<MethodSym *> ctors = c->ctors;
                    Resolved r = resolveCall(ctors, ec.args, classType(c), nullptr, ec.pos, c->name, {}, lowered, notes);
                    if (!r.m)
                    {
                        error(ec.pos, "constructor " + c->name + " in enum " + c->name + " cannot be applied to given types;", {notes});
                        ++ordinal;
                        continue;
                    }
                    std::vector<ASTNodePtr> args = lowerArgs(r, ec.args, lowered, ec.pos);
                    ClassSym *instCls = c;
                    MethodSym *ctor = r.m;
                    if (ec.body)
                    {
                        // a constant with a body: an anonymous subclass
                        ClassSym *anon = enterClass(ec.body.get(), c, c->unit, c->pkg);
                        ClassSym *top = topLevelHelper(c);
                        anon->binaryName = top->binaryName + "$" + std::to_string(++top->anonCount);
                        byBinary_[anon->binaryName] = anon;
                        anon->isAnon = true;
                        anon->state = 1;
                        anon->super = classType(c);
                        anon->state = 2;
                        anonSuperCtor_[anon] = r.m;
                        anonSuperParams_[anon] = r.paramTypes;
                        enterMembers(anon);
                        assignKeys(anon);
                        checkClass(anon);
                        lowerClass(anon);
                        instCls = anon;
                        ctor = anon->ctors.empty() ? r.m : anon->ctors[0];
                    }
                    need(instCls);
                    std::vector<ASTNodePtr> a;
                    std::vector<ASTNodePtr> alloc;
                    alloc.push_back(q::id(instCls->binaryName, el));
                    a.push_back(q::call("__j_new", std::move(alloc), el));
                    a.push_back(q::str(ec.name, el));
                    a.push_back(q::lng(ordinal, el));
                    for (auto &x : args)
                        a.push_back(std::move(x));
                    needMethod(ctor);
                    if (f)
                        out.push_back(q::exprStmt(q::assign(q::id(f->key, el), q::call(ctor->impl, std::move(a), el), el), el));
                    if (f)
                        all.push_back(q::id(f->key, el));
                    ++ordinal;
                }
                out.push_back(q::exprStmt(q::assign(q::id(c->valuesGlobal(), l), q::arr(std::move(all), l), l), l));
            }
            for (auto &mem : c->decl->members)
            {
                if (mem.kind == Member::Field && (mem.field->mods.has(M_STATIC) || c->isInterface()))
                    for (auto &v : mem.field->vars)
                    {
                        if (!v.init)
                            continue;
                        FieldSym *f = nullptr;
                        for (FieldSym *x : c->fields)
                            if (x->var == &v)
                                f = x;
                        if (!f)
                            continue;
                        if (fieldConst(f).ok())
                        {
                            out.push_back(q::exprStmt(q::assign(q::id(f->key, v.pos.line), exprTo(v.init, f->type, v.init->pos), v.pos.line), v.pos.line));
                            continue;
                        }
                        ASTNodePtr init = v.init->k == EK::ArrayLit ? (f->type->k == TK::Array ? arrayLit(v.init, f->type).node : q::nil(l))
                                                                    : exprTo(v.init, f->type, v.init->pos);
                        out.push_back(q::exprStmt(q::assign(q::id(f->key, v.pos.line), std::move(init), v.pos.line), v.pos.line));
                    }
                else if (mem.kind == Member::Init && mem.isStatic)
                {
                    out.push_back(block(mem.init->stmts, mem.init->pos.line));
                    fn_->flow.alive = true;
                }
            }
        }, l);
        if (prelude)
            preludeInit_.push_back({c, std::move(body)});
        else
        {
            c->needsClinit = true;
            functions_.push_back(q::func(c->clinitFn(), {}, std::move(body), l));
        }
    }
    else if (!prelude)
        c->needsClinit = false;
    // methods and constructors: local and anonymous classes now, while the
    // enclosing method's scopes are alive; the rest of the user's code through
    // the queue; prelude methods once something can call them
    std::vector<MethodSym *> all = c->methods;
    all.insert(all.end(), c->ctors.begin(), c->ctors.end());
    for (MethodSym *m : all)
    {
        if (m->isAbstract() && m->synth == Synth::None)
            continue;
        if (!m->native.empty() && m->isStatic())
            continue; // called directly
        if (c->isAnon || c->isLocal)
            lowerMethod(m);
        else if (!prelude)
            queueMethod(m);
    }
    fn_ = savedFn;
    unit_ = savedUnit;
    ctxClass_ = savedCtx;
}

// ─── The program ─────────────────────────────────────────────────────────────

ASTNodePtr Sema::emitProgram()
{
    const int l = 0;
    std::vector<ASTNodePtr> prog;
    prog.push_back(q::exprStmt(q::call("__j_runtime", {}, l), l));
    for (auto &f : functions_)
        prog.push_back(std::move(f));
    functions_.clear();

    // classes, superclasses first
    std::set<ClassSym *> done;
    std::vector<ClassSym *> ordered;
    std::function<void(ClassSym *)> visit = [&](ClassSym *c)
    {
        if (!c || done.count(c) || !c->lowered)
            return;
        done.insert(c);
        if (c->super && c->super->cls)
            visit(c->super->cls);
        ordered.push_back(c);
    };
    for (ClassSym *c : order_)
        visit(c);
    for (auto &kv : anonClasses_)
        visit(kv.second);
    for (ClassSym *c : ordered)
    {
        // every supertype, for instanceof / checkcast
        std::vector<ASTNodePtr> supers;
        std::set<ClassSym *> seen;
        std::function<void(ClassSym *)> addSupers = [&](ClassSym *k)
        {
            if (!k || !seen.insert(k).second)
                return;
            supers.push_back(q::str(k->binaryName, l));
            if (k->super && k->super->cls)
                addSupers(k->super->cls);
            for (auto &i : k->ifaces)
                addSupers(i->cls);
        };
        addSupers(c);
        DictLiteral defaults;
        for (FieldSym *f : c->fields)
            if (!f->isStatic)
                defaults.pairs.emplace_back(q::str(f->key, l), defaultValue(f->type, l));
        if (c->hasOuterInstance)
            defaults.pairs.emplace_back(q::str("this$0", l), q::nil(l));
        for (auto &cap : c->captures)
            defaults.pairs.emplace_back(q::str(cap.field, l), q::nil(l));
        std::vector<ASTNodePtr> a;
        a.push_back(q::str(c->binaryName, l));
        a.push_back(c->super && c->super->cls && c->super->cls->lowered ? q::id(c->super->cls->binaryName, l) : q::nil(l));
        a.push_back(q::arr(std::move(supers), l));
        a.push_back(std::make_unique<ASTNode>(std::move(defaults), l));
        a.push_back(q::str(fileOf(units_[c->unit]), l));
        a.push_back(q::str(c->isAnon ? "" : c->name, l));
        prog.push_back(q::var(c->binaryName, q::call("__j_class", std::move(a), l), l));
        auto bind = [&](const std::string &key, const std::string &impl)
        {
            std::vector<ASTNodePtr> b;
            b.push_back(q::id(c->binaryName, l));
            b.push_back(q::str(key, l));
            b.push_back(q::id(impl, l));
            prog.push_back(q::exprStmt(q::call("__j_bind", std::move(b), l), l));
        };
        std::set<std::string> boundKeys;
        for (MethodSym *m : c->methods)
        {
            if (m->isStatic() || m->isAbstract() || !m->lowered)
                continue;
            bind(m->vkey, m->impl);
            boundKeys.insert(m->vkey);
            for (auto &k : m->bridgeKeys)
                if (boundKeys.insert(k).second)
                    bind(k, m->impl);
        }
        // interface default methods the class does not override
        if (!c->isInterface())
        {
            std::set<ClassSym *> seenI;
            std::function<void(ClassSym *)> walk = [&](ClassSym *k)
            {
                if (!k)
                    return;
                for (auto &i : k->ifaces)
                {
                    ClassSym *ic = i->cls;
                    if (!ic || !seenI.insert(ic).second)
                        continue;
                    for (MethodSym *d : ic->methods)
                    {
                        if (!d->isDefault() || !d->lowered)
                            continue;
                        bool overridden = false;
                        for (ClassSym *x = c; x && !overridden; x = x->super ? x->super->cls : nullptr)
                            for (MethodSym *m : x->methods)
                                if (!m->isAbstract() && !m->isStatic() && overrides(m, d, x))
                                    overridden = true;
                        if (!overridden && boundKeys.insert(d->vkey).second)
                        {
                            bind(d->vkey, d->impl);
                            for (auto &bk : d->bridgeKeys)
                                if (boundKeys.insert(bk).second)
                                    bind(bk, d->impl);
                        }
                    }
                    walk(ic);
                }
                if (k->super && k->super->cls)
                    walk(k->super->cls);
            };
            walk(c);
        }
    }
    // static fields start at their defaults; run-once flags of lazily initialized classes
    for (ClassSym *c : ordered)
    {
        for (FieldSym *f : c->fields)
            if (f->isStatic)
                prog.push_back(q::var(f->key, defaultValue(f->type, l), l));
        if (c->isEnum())
            prog.push_back(q::var(c->valuesGlobal(), q::arr({}, l), l));
        if (c->needsClinit)
            prog.push_back(q::var(c->initedFlag(), q::boolean(false, l), l));
    }
    // the prelude's static initializers (superclasses first)
    for (ClassSym *c : ordered)
        for (auto &pi : preludeInit_)
            if (pi.first == c && pi.second)
                prog.push_back(std::move(pi.second));
    // main
    std::vector<ASTNodePtr> body;
    if (mainClass_ && mainMethod_)
    {
        if (mainClass_->needsClinit)
            body.push_back(q::exprStmt(q::call(mainClass_->clinitFn(), {}, l), l));
        std::vector<ASTNodePtr> args;
        if (!mainMethod_->params.empty())
            args.push_back(q::call("__j_argv", {}, l));
        if (mainMethod_->isStatic())
            body.push_back(q::exprStmt(q::call(mainMethod_->impl, std::move(args), l), l));
        else
        {
            // an instance main (Java 25): on a new instance of its class
            MethodSym *k = nullptr;
            for (MethodSym *x : mainClass_->ctors)
                if (x->params.empty())
                    k = x;
            std::vector<ASTNodePtr> alloc;
            alloc.push_back(q::id(mainClass_->binaryName, l));
            std::vector<ASTNodePtr> ka;
            ka.push_back(q::call("__j_new", std::move(alloc), l));
            ASTNodePtr obj = k ? q::call(k->impl, std::move(ka), l) : std::move(ka[0]);
            std::vector<ASTNodePtr> ma;
            ma.push_back(std::move(obj));
            for (auto &x : args)
                ma.push_back(std::move(x));
            body.push_back(q::exprStmt(q::call(mainMethod_->impl, std::move(ma), l), l));
        }
    }
    else
    {
        std::string name = mainClass_ ? mainClass_->binaryName : "Main";
        std::vector<ASTNodePtr> m;
        m.push_back(q::str("Error: Main method not found in class " + name +
                               ", please define the main method as:\n   public static void main(String[] args)\n"
                               "or a JavaFX application class must extend javafx.application.Application",
                           l));
        body.push_back(q::exprStmt(q::call("__j_eprintln", std::move(m), l), l));
        std::vector<ASTNodePtr> code;
        code.push_back(q::lng(1, l));
        body.push_back(q::exprStmt(q::call("__j_exit", std::move(code), l), l));
    }
    std::vector<ASTNodePtr> handler;
    std::vector<ASTNodePtr> ua;
    ua.push_back(q::id("__uncaught", l));
    handler.push_back(q::exprStmt(q::call("java.lang.Throwable.$uncaught", std::move(ua), l), l));
    prog.push_back(q::tryCatch(q::block(std::move(body), l), "__uncaught", q::block(std::move(handler), l), l));
    return q::block(std::move(prog), l);
}

ASTNodePtr Sema::run()
{
    for (size_t u = 0; u < units_.size(); ++u)
        enterUnit((int)u);
    for (size_t i = 0; i < order_.size(); ++i)
        resolveHeader(order_[i]);
    for (size_t i = 0; i < order_.size(); ++i)
        enterMembers(order_[i]);
    for (size_t i = 0; i < order_.size(); ++i)
        assignKeys(order_[i]);
    std::vector<ClassSym *> user;
    for (ClassSym *c : order_)
        if (c->unit == 0 && !c->isLocal && !c->isAnon)
            user.push_back(c);
    for (ClassSym *c : user)
        checkClass(c);
    g_activeSema = this;
    // dispatch keys the runtime calls by itself (string conversion, hashing, ordering)
    for (const char *k : {"toString()", "hashCode()", "equals(Object)", "compareTo(Object)", "compare(Object,Object)"})
        useKey(k);
    // the runtime always needs these
    for (const char *n : {"java.lang.Object", "java.lang.Throwable", "java.lang.System", "java.io.PrintStream",
                          "java.lang.String", "java.lang.Class"})
        need(cls(n));
    needNamed("java.lang.Throwable", "$uncaught");
    for (ClassSym *c : user)
        need(c);
    // main: main(String[]) first, then Java 25's main()
    for (int pass = 0; pass < 2 && !mainMethod_; ++pass)
        for (ClassSym *c : user)
        {
            if (c->outer)
                continue;
            for (MethodSym *m : c->methods)
            {
                if (m->name != "main" || m->ret->k != TK::Void)
                    continue;
                bool strArray = m->params.size() == 1 && m->params[0]->k == TK::Array && isStringType(m->params[0]->elem);
                if (pass == 0 && strArray && m->isStatic())
                    mainClass_ = c, mainMethod_ = m;
                else if (pass == 1 && (strArray || m->params.empty()))
                    mainClass_ = c, mainMethod_ = m;
                if (mainMethod_)
                    break;
            }
            if (mainMethod_)
                break;
        }
    if (!mainClass_)
        for (ClassSym *c : user)
            if (!c->outer && (c->mods & M_PUBLIC))
                mainClass_ = c;
    if (!mainClass_ && !user.empty())
        mainClass_ = user[0];
    if (mainMethod_)
        needMethod(mainMethod_);
    for (size_t ci = 0, mi = 0; ci < lowerQueue_.size() || mi < methodQueue_.size();)
    {
        if (ci < lowerQueue_.size())
            lowerClass(lowerQueue_[ci++]);
        else
            lowerMethod(methodQueue_[mi++]);
    }
    g_activeSema = nullptr;
    if (!diags_.empty())
        return nullptr;
    return emitProgram();
}

} // namespace java
