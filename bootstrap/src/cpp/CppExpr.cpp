// Expression lowering for the typed C/C++ front-end: names, operators,
// assignment, members, indexing, casts, lambdas, construction, calls with
// overload resolution, conversions and copies. (See CppLower.cpp.)
#include "CppLower.h"
#include "JavaOps.h"

#include <cstring>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <functional>

namespace cpp
{

// 64-bit integers run as exact int64 values (narrower ones as doubles)
static ASTNodePtr i64Const(int64_t v, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(q::str(std::to_string(v), ln));
    return q::call("__jx_int__", std::move(a), ln);
}

static ASTNodePtr jop1(int op, ASTNodePtr x, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(q::num(op, ln));
    a.push_back(std::move(x));
    return q::call("__jx_un__", std::move(a), ln);
}

static ASTNodePtr jop2(int op, ASTNodePtr x, ASTNodePtr y, int ln)
{
    std::vector<ASTNodePtr> a;
    a.push_back(q::num(op, ln));
    a.push_back(std::move(x));
    a.push_back(std::move(y));
    return q::call("__jx_bin__", std::move(a), ln);
}

// The exact value of an integer literal's text (0x / 0b / octal, ' separators, suffixes)
static bool literalValue(std::string s, uint64_t &out)
{
    s.erase(std::remove(s.begin(), s.end(), '\''), s.end());
    while (!s.empty() && std::strchr("uUlLzZ", s.back()))
        s.pop_back();
    if (s.empty())
        return false;
    int base = 10;
    size_t i = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        base = 16, i = 2;
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
        base = 2, i = 2;
    else if (s.size() > 1 && s[0] == '0')
        base = 8, i = 1;
    uint64_t v = 0;
    for (; i < s.size(); ++i)
    {
        int d = std::isdigit((unsigned char)s[i]) ? s[i] - '0' : std::isxdigit((unsigned char)s[i]) ? std::tolower(s[i]) - 'a' + 10 : 99;
        if (d >= base)
            return false;
        v = v * base + d;
    }
    out = v;
    return true;
}

XV Lowerer::mkxv(ASTNodePtr v, TP t, bool temp)
{
    XV x;
    x.v = std::move(v);
    x.t = t ? t : tDyn();
    x.temp = temp;
    return x;
}

ASTNodePtr Lowerer::chr(ASTNodePtr code, int line)
{
    std::vector<ASTNodePtr> a;
    a.push_back(std::move(code));
    return q::call("__cpp_chr__", std::move(a), line);
}

ASTNodePtr Lowerer::simple(ASTNodePtr e, int line)
{
    if (q::isSimple(e))
        return e;
    if (canHoist_ && pre_)
    {
        std::string t = tempName();
        pre_->push_back(q::var(t, std::move(e), line));
        return q::id(t, line);
    }
    return e;
}

ASTNodePtr Lowerer::peekRead(const XV &x)
{
    int ln = line_;
    switch (x.lk)
    {
    case XV::Var:
        return q::id(x.var, ln);
    case XV::Idx:
        return q::index(q::clone(x.obj), q::clone(x.key), ln);
    case XV::StrCh:
    {
        std::vector<ASTNodePtr> a;
        if (x.base->lk == XV::Idx && x.base->t->k == T::Str)
        {
            // a string held in a slot: read in place
            a.push_back(q::clone(x.base->obj));
            a.push_back(q::clone(x.base->key));
            a.push_back(q::clone(x.key));
            return q::call("__cpp_str_get__", std::move(a), ln);
        }
        a.push_back(peekRead(*x.base));
        a.push_back(q::clone(x.key));
        return q::call("__cpp_str_at__", std::move(a), ln);
    }
    case XV::RV:
        if (x.v)
            return q::clone(x.v);
        unsupported("internal: value read twice");
    }
    return nullptr;
}

ASTNodePtr Lowerer::read(XV &x)
{
    if (x.v)
        return std::move(x.v);
    return peekRead(x);
}

ASTNodePtr Lowerer::store(XV &t, ASTNodePtr value, int line)
{
    if (isObjectRep(t.t) && t.t->k != T::Stream)
    {
        // C++ assignment keeps the object's identity: replace its contents
        std::vector<ASTNodePtr> a;
        a.push_back(t.lk == XV::RV ? read(t) : peekRead(t));
        a.push_back(std::move(value));
        return q::call("__cpp_assign__", std::move(a), line);
    }
    switch (t.lk)
    {
    case XV::Var:
        return q::assign(q::id(t.var, line), std::move(value), line);
    case XV::Idx:
        return q::assign(q::index(q::clone(t.obj), q::clone(t.key), line), std::move(value), line);
    case XV::StrCh:
    {
        std::vector<ASTNodePtr> a;
        XV &b = *t.base;
        if (b.t->k == T::CStr)
        {
            // through a char*: into the buffer it points into
            a.push_back(peekRead(b));
            a.push_back(q::clone(t.key));
            a.push_back(std::move(value));
            return q::call("__cpp_cs_put__", std::move(a), line);
        }
        if (b.lk == XV::Idx)
        {
            // a string held in a slot: written in place
            a.push_back(q::clone(b.obj));
            a.push_back(q::clone(b.key));
            a.push_back(q::clone(t.key));
            a.push_back(std::move(value));
            return q::call("__cpp_str_put__", std::move(a), line);
        }
        a.push_back(peekRead(b));
        a.push_back(q::clone(t.key));
        a.push_back(std::move(value));
        return store(b, q::call("__cpp_str_set__", std::move(a), line), line);
    }
    case XV::RV:
        break;
    }
    if (t.t->k == T::Dyn && t.v)
    {
        // dynamic target (an unknown member): plain assignment
        return q::assign(std::move(t.v), std::move(value), line);
    }
    unsupported("assignment to a value that is not an lvalue", line);
}

ASTNodePtr Lowerer::trackTemp(ASTNodePtr v, const TP &t, int line)
{
    if (tempsTracked_ < 0 || !t || t->k != T::Class || !t->cls || !needsDtor(t->cls))
        return v;
    ++tempsTracked_;
    std::vector<ASTNodePtr> a;
    a.push_back(std::move(v));
    return q::call("__cpp_temp__", std::move(a), line);
}

void Lowerer::untrackTemp(XV &x)
{
    // the temporary is owned from here (a variable, a return value, a by-value parameter)
    if (!x.v)
        return;
    if (auto *c = std::get_if<CallExpr>(&x.v->node))
        if (c->callee && std::holds_alternative<Identifier>(c->callee->node) &&
            std::get<Identifier>(c->callee->node).name == "__cpp_temp__" && c->args.size() == 1)
        {
            ASTNodePtr inner = std::move(c->args[0]);
            x.v = std::move(inner);
        }
}

ASTNodePtr Lowerer::endTemps(ASTNodePtr stmt, const std::string &mark, int line)
{
    std::vector<ASTNodePtr> st;
    st.push_back(q::var(mark, q::call("__cpp_temps_mark__", {}, line), line));
    if (stmt)
        st.push_back(std::move(stmt));
    std::vector<ASTNodePtr> a;
    a.push_back(q::id(mark, line));
    st.push_back(q::exprStmt(q::call("__cpp_temps_end__", std::move(a), line), line));
    return q::block(std::move(st), line);
}

ASTNodePtr Lowerer::copyOf(XV x, int line)
{
    if (x.t->k == T::UPtr && !x.temp)
    {
        // a moved smart pointer leaves its source empty; a shared_ptr copy is one more owner
        if (x.moved && x.lk != XV::RV)
        {
            std::vector<ASTNodePtr> a;
            a.push_back(peekRead(x));
            a.push_back(store(x, q::nil(line), line));
            return q::call("__cpp_first__", std::move(a), line);
        }
        if (x.t->name == "shared_ptr" && !x.moved)
        {
            std::vector<ASTNodePtr> a;
            a.push_back(read(x));
            return q::call("__cpp_sp_copy__", std::move(a), line);
        }
    }
    if (needsCopy(x.t) && !x.temp && !x.moved && x.t->k == T::Class && x.t->cls)
        if (FuncInfo *cc = userCopyCtor(x.t->cls))
        {
            // a user copy constructor runs for every copy
            ClassInfo *c = x.t->cls;
            std::vector<ExprP> one(1);
            std::vector<XV> lowered;
            lowered.push_back(std::move(x));
            std::vector<ASTNodePtr> al;
            al.push_back(q::id(c->desc(), line));
            al.push_back(q::num(c->nslots, line));
            XV r = callArgs(cc, one, q::call("__cpp_alloc__", std::move(al), line), line, false, &lowered);
            return read(r);
        }
    if (needsCopy(x.t) && !x.temp && !x.moved)
    {
        std::vector<ASTNodePtr> a;
        a.push_back(read(x));
        a.push_back(planFor(x.t));
        return q::call("__cpp_copy__", std::move(a), line);
    }
    return read(x);
}

TP Lowerer::elemType(const TP &t)
{
    if (!t)
        return tDyn();
    switch (t->k)
    {
    case T::Vector: case T::Array: case T::Set: case T::InitList: case T::Ptr:
        return t->a.empty() ? tDyn() : t->a[0];
    case T::Map:
    {
        auto p = mkT(T::Pair);
        p->a = t->a;
        return p;
    }
    case T::Str: case T::CStr:
        return tChar();
    default:
        return tDyn();
    }
}

TP Lowerer::deref(const TP &t)
{
    if (!t)
        return tDyn();
    switch (t->k)
    {
    case T::Ptr: case T::UPtr: case T::Optional:
        return t->a.empty() ? tDyn() : t->a[0];
    case T::Iter:
        return t->a.empty() ? tDyn() : elemType(t->a[0]);
    case T::CStr:
        return tChar();
    default:
        return tDyn();
    }
}

TP Lowerer::arithResult(const TP &a, const TP &b)
{
    if (a->k == T::Dyn || b->k == T::Dyn)
        return tDyn();
    if (isFloat(a) || isFloat(b))
        return tDouble();
    auto rank = [](const TP &t) -> int
    {
        if (t->k != T::Int)
            return 1; // char, bool, enum, short → int
        if (t->name.find("long long") != std::string::npos)
            return 4;
        if (t->name.find("long") != std::string::npos)
            return 3;
        if (t->name.find("short") != std::string::npos)
            return 1;
        return 2;
    };
    int ra = rank(a), rb = rank(b);
    TP big = ra >= rb ? a : b;
    if (big->k != T::Int || rank(big) <= 1)
        return (a->isUnsigned && a->k == T::Int && rank(a) == 2) || (b->isUnsigned && b->k == T::Int && rank(b) == 2) ? mkT(T::Int, "unsigned int") : tInt();
    bool u = (ra == rb) ? (a->isUnsigned || b->isUnsigned) : big->isUnsigned;
    if (u && !big->isUnsigned)
    {
        auto t = mkT(T::Int, "unsigned " + big->name);
        t->isUnsigned = true;
        return t;
    }
    return big;
}

TP Lowerer::commonType(const TP &a, const TP &b)
{
    if (sameType(a, b))
        return a;
    if (isArith(a) && isArith(b))
        return arithResult(a, b);
    if (isStringy(a) && isStringy(b))
        return tStr();
    if (a->k == T::Nullptr)
        return b;
    if (b->k == T::Nullptr)
        return a;
    if (a->k == T::Dyn)
        return b;
    return a;
}

// ─── Conversions ───────────────────────────────────────────────────────────

int Lowerer::convCost(const TP &from, const TP &to, const XV *x)
{
    if (!from || !to)
        return 1;
    if (to->k == T::Dyn || from->k == T::Dyn)
        return 1;
    if (sameType(from, to))
        return 0;
    if (from->k == T::InitList)
        return (isObjectRep(to) || to->k == T::Variant || to->k == T::Optional || to->k == T::Str) ? 2 : 3;
    if (x && x->isFuncName)
        return to->k == T::Func ? 2 : 9;
    if (isArith(from) && isArith(to))
    {
        if (to->k == T::Enum && from->k != T::Enum)
            return 9;
        if (from->k == T::Enum && from->en && from->en->scoped && to->k != T::Enum)
            return 9;
        if (from->k == to->k)
            return 1;
        return 2;
    }
    if (from->k == T::CStr && to->k == T::Str)
        return 2;
    if (isCBuf(from) && to->k == T::CStr)
        return 1; // array-to-pointer
    if (from->k == T::Str && to->k == T::CStr)
        return 9;
    if (from->k == T::Nullptr && (to->k == T::Ptr || to->k == T::UPtr || to->k == T::Func || to->k == T::CStr ||
                                  to->k == T::Optional))
        return 1;
    if (from->k == T::Int && x && x->v && std::holds_alternative<NumberLiteral>(x->v->node) &&
        std::get<NumberLiteral>(x->v->node).value == 0 && (to->k == T::Ptr || to->k == T::CStr))
        return 2;
    if (from->k == T::Class && to->k == T::Class)
    {
        std::function<bool(ClassInfo *)> derives = [&](ClassInfo *c)
        {
            if (c == to->cls)
                return true;
            for (ClassInfo *b : c->bases)
                if (derives(b))
                    return true;
            return false;
        };
        if (derives(from->cls))
            return 2;
    }
    if (to->k == T::Class && to->cls)
    {
        for (FuncInfo *c : to->cls->ctors)
            if (c->params.size() >= 1 && (c->params.size() == 1 || (c->defaults.size() > 1 && c->defaults[1])) &&
                !(c->decl && c->decl->isExplicit) && convCost(from, c->params[0]) < 4)
                return 4;
        if (to->cls->builtin && isStringy(from))
            return 4;
    }
    if (from->k == T::Ptr && to->k == T::Ptr)
    {
        if (from->a[0]->k == T::Class && to->a[0]->k == T::Class && convCost(from->a[0], to->a[0]) <= 2)
            return 2;
        if (to->a[0]->k == T::Void)
            return 2;
    }
    if (from->k == T::UPtr && to->k == T::UPtr)
        return 2;
    if (from->k == T::Array && to->k == T::Ptr)
        return 2;
    if (from->k == T::Func && to->k == T::Func)
        return 2;
    if (to->k == T::Variant)
    {
        for (auto &alt : to->a)
            if (convCost(from, alt) < 4)
                return 3;
        return 9;
    }
    if (to->k == T::Optional)
        return from->k == T::Nullopt ? 1 : (convCost(from, to->a[0]) < 4 ? 3 : 9);
    if (to->k == T::Bool && (isArith(from) || from->k == T::Ptr || from->k == T::UPtr))
        return 3;
    if (from->k == T::Class && to->k != T::Class)
    {
        for (FuncInfo *op : from->cls ? from->cls->convOps : std::vector<FuncInfo *>())
            if (convCost(op->ret, to) < 4)
                return 4;
    }
    if (from->k == to->k && (from->k == T::Vector || from->k == T::Map || from->k == T::Set || from->k == T::Pair ||
                             from->k == T::Tuple || from->k == T::Iter || from->k == T::Stream))
        return 2;
    return 9;
}

XV Lowerer::convert(XV x, const TP &to, int line, bool forInit)
{
    (void)forInit;
    if (x.alloc && to)
    {
        if (to->k == T::Ptr && !to->a.empty() && to->a[0]->k != T::Void && to->a[0]->k != T::Dyn)
            return materializeAlloc(x, to->a[0], line);
        if (to->k == T::CStr)
            return materializeAlloc(x, tChar(), line);
    }
    if (!to || to->k == T::Dyn || to->k == T::Void)
        return x;
    const TP from = x.t;
    if (from->k == T::Dyn && !(x.isFuncName && to->k == T::Func))
    {
        x.t = to;
        return x;
    }
    // a char buffer decays to a char* into it; a char* or char buffer read
    // as a std::string is the C string up to its NUL
    if (isCBuf(from) && to->k == T::CStr)
        return mkxv(charPtr(x, line), to);
    if (to->k == T::Str && !isCBuf(to) &&
        (isCBuf(from) || (from->k == T::CStr && !(x.v && std::holds_alternative<StringLiteral>(x.v->node)))))
    {
        std::vector<ASTNodePtr> a;
        a.push_back(read(x));
        return mkxv(q::call("__cpp_cs_str__", std::move(a), line), to);
    }
    if (x.isFuncName && to->k == T::Func)
    {
        FuncInfo *pick = nullptr;
        for (FuncInfo *f : x.overloads)
            if (f->params.size() + 1 == to->a.size() || !pick)
                pick = f;
        if (!pick)
            unsupported("no function to convert to a function pointer", line);
        if (pick->cls && !pick->isStatic)
            unsupported("pointer to member function", line);
        if (!pick->lowered && pick->def && !pick->cls)
            ; // lowered later with the rest
        return mkxv(q::id(pick->mangled, line), to);
    }
    if (sameType(from, to))
        return x;
    // to / from a 64-bit integer: an exact int64 value
    if (isI64(to) && !isI64(from) && isArith(from))
    {
        if (x.v && std::holds_alternative<NumberLiteral>(x.v->node) && !isFloat(from))
            return mkxv(i64Const((int64_t)std::get<NumberLiteral>(x.v->node).value, line), to);
        return mkxv(jop1(jop::D2L, read(x), line), to);
    }
    if (isI64(from) && !isI64(to) && isArith(to) && to->k != T::Bool && to->k != T::Enum)
    {
        ASTNodePtr v = read(x);
        long bytes = sizeOfType(to);
        if (isFloat(to))
            return mkxv(jop1(jop::I2D, std::move(v), line), to);
        if (to->isUnsigned && bytes < 8)
            v = jop2(jop::LAND, std::move(v), i64Const(bytes >= 4 ? 4294967295LL : bytes == 2 ? 65535 : 255, line), line);
        else if (bytes == 1)
            v = jop1(jop::I2B, std::move(v), line);
        else if (bytes == 2)
            v = jop1(jop::I2S, std::move(v), line);
        else if (bytes == 4)
            v = jop1(jop::L2I, std::move(v), line);
        if (bytes < 8)
            v = jop1(jop::I2D, std::move(v), line);
        return mkxv(std::move(v), to);
    }
    auto num = [&](XV &v) { return read(v); };
    if ((isArith(to) && to->k != T::Bool) && !explicitConv_ &&
        (from->k == T::CStr || from->k == T::Str || from->k == T::Ptr || from->k == T::Vector || from->k == T::Map ||
         from->k == T::Set || from->k == T::Pair || from->k == T::Tuple || from->k == T::Func || from->k == T::Iter ||
         from->k == T::Array || from->k == T::Stream))
    {
        bool ptrish = from->k == T::CStr || from->k == T::Ptr || from->k == T::Array || (isCBuf(from));
        if (prog_.cMode)
            unsupported(ptrish ? "initialization of '" + diagType(to) + "' from '" + diagType(from) +
                                     "' makes integer from pointer without a cast [-Wint-conversion]"
                               : "incompatible types when initializing type '" + diagType(to) + "' using type '" +
                                     diagType(from) + "'",
                        line);
        unsupported(ptrish ? "invalid conversion from '" + diagType(from) + "' to '" + diagType(to) + "' [-fpermissive]"
                           : "cannot convert '" + diagType(from) + "' to '" + diagType(to) + "'",
                    line);
    }
    switch (to->k)
    {
    case T::Int:
    case T::Char:
    case T::Enum:
        if (isFloat(from))
        {
            std::vector<ASTNodePtr> a;
            a.push_back(num(x));
            return mkxv(q::call("__cpp_trunc__", std::move(a), line), to);
        }
        if (from->k == T::Bool)
        {
            std::vector<ASTNodePtr> a;
            a.push_back(num(x));
            return mkxv(q::call("__cpp_b2i__", std::move(a), line), to);
        }
        if (isIntegral(from))
        {
            if (to->k == T::Char && to->isUnsigned && from->k != T::Char)
            {
                std::vector<ASTNodePtr> a;
                a.push_back(num(x));
                a.push_back(q::num(256, line));
                return mkxv(q::call("__cpp_wrap__", std::move(a), line), to);
            }
            x.t = to;
            return x;
        }
        if (from->k == T::Class)
            break;
        x.t = to;
        return x;
    case T::Float:
        if (from->k == T::Bool)
        {
            std::vector<ASTNodePtr> a;
            a.push_back(num(x));
            return mkxv(q::call("__cpp_b2i__", std::move(a), line), to);
        }
        if (isArith(from))
        {
            x.t = to;
            return x;
        }
        break;
    case T::Bool:
        if (from->k == T::Class)
            break;
        return mkxv(toBool(std::move(x), line), to);
    case T::Str:
        if (from->k == T::CStr)
        {
            x.t = to;
            return x;
        }
        if (from->k == T::Char)
            return mkxv(chr(num(x), line), to);
        if (from->k == T::Nullptr)
            return mkxv(q::str("", line), to);
        break;
    case T::CStr:
        if (from->k == T::Str || from->k == T::Nullptr)
        {
            x.t = to;
            if (from->k == T::Nullptr)
                return mkxv(q::nil(line), to);
            return x;
        }
        if (from->k == T::Int)
            return mkxv(q::nil(line), to);
        break;
    case T::Ptr:
        if (from->k == T::Nullptr || (from->k == T::Int))
            return mkxv(q::nil(line), to);
        if (from->k == T::Array)
        {
            // array → pointer to its first element
            std::vector<ASTNodePtr> e;
            e.push_back(read(x));
            e.push_back(q::num(0, line));
            if (isObjectRep(to->a[0]) && to->a[0]->k != T::Array)
            {
                std::vector<ASTNodePtr> a;
                a.push_back(q::arr(std::move(e), line));
                return mkxv(q::call("__cpp_ptr_first__", std::move(a), line), to);
            }
            return mkxv(q::arr(std::move(e), line), to);
        }
        if (from->k == T::UPtr || from->k == T::Ptr)
        {
            x.t = to;
            return x;
        }
        break;
    case T::UPtr:
    case T::Func:
        if (from->k == T::Nullptr)
            return mkxv(q::nil(line), to);
        x.t = to;
        return x;
    case T::Optional:
        if (from->k == T::Nullopt || from->k == T::Nullptr)
            return mkxv(q::nil(line), to);
        if (from->k == T::Optional)
        {
            x.t = to;
            return x;
        }
        {
            XV inner = convert(std::move(x), to->a[0], line);
            std::vector<ASTNodePtr> e;
            e.push_back(copyOf(std::move(inner), line));
            return mkxv(q::arr(std::move(e), line), to);
        }
    case T::Variant:
    {
        if (from->k == T::Variant)
        {
            x.t = to;
            return x;
        }
        int best = -1, bestCost = 99;
        for (size_t i = 0; i < to->a.size(); ++i)
        {
            int c = convCost(from, to->a[i], &x);
            // C++17: a string literal prefers std::string over bool
            if (from->k == T::CStr && to->a[i]->k == T::Str)
                c = 0;
            if (from->k == T::CStr && to->a[i]->k == T::Bool)
                c = 8;
            if (c < bestCost)
            {
                bestCost = c;
                best = (int)i;
            }
        }
        if (best < 0 || bestCost >= 9)
            unsupported("no variant alternative for " + from->str(), line);
        XV inner = convert(std::move(x), to->a[best], line);
        std::vector<ASTNodePtr> e;
        e.push_back(q::num(best, line));
        e.push_back(to->a[best]->k == T::Monostate ? q::nil(line) : copyOf(std::move(inner), line));
        return mkxv(q::arr(std::move(e), line), to);
    }
    case T::Class:
    {
        if (from->k == T::Class && convCost(from, to) <= 2)
        {
            x.t = to; // slicing: the object keeps its own fields
            return x;
        }
        ClassInfo *c = to->cls;
        if (c->builtin && isStringy(from))
        {
            std::vector<ASTNodePtr> a;
            a.push_back(q::id(c->desc(), line));
            a.push_back(read(x));
            return mkxv(q::call("__cpp_exc__", std::move(a), line), to);
        }
        // converting constructor
        for (FuncInfo *ctor : c->ctors)
        {
            if (ctor->params.empty())
                continue;
            bool oneArg = ctor->params.size() == 1 ||
                          (ctor->defaults.size() > 1 && ctor->defaults[1] != nullptr);
            if (oneArg && convCost(from, ctor->params[0], &x) < 4)
            {
                // re-enter through ctorCall with a synthetic argument
                std::string tmp = tempName();
                ASTNodePtr val = read(x);
                if (pre_ && canHoist_)
                {
                    pre_->push_back(q::var(tmp, std::move(val), line));
                    auto e = std::make_shared<Expr>();
                    e->k = EK::Name;
                    e->line = line;
                    e->name = {"#tmp:" + tmp};
                    localTypes_["#tmp:" + tmp] = from;
                    std::vector<ExprP> args = {e};
                    return mkxv(ctorCall(c, args, false, line), to);
                }
                std::vector<ASTNodePtr> a;
                a.push_back(q::call("__cpp_alloc__", [&]
                                 {
                                     std::vector<ASTNodePtr> aa;
                                     aa.push_back(q::id(c->desc(), line));
                                     aa.push_back(q::num(c->nslots, line));
                                     return aa; }(),
                                 line));
                a.push_back(std::move(val));
                return mkxv(q::call(ctor->mangled, std::move(a), line), to);
            }
        }
        break;
    }
    default:
        break;
    }
    // class → non-class through a conversion operator
    if (from->k == T::Class && from->cls)
    {
        for (FuncInfo *op : from->cls->convOps)
            if (convCost(op->ret, to) < 4)
            {
                std::vector<ExprP> none;
                std::vector<ASTNodePtr> a;
                a.push_back(read(x));
                XV r = mkxv(q::call(op->mangled, std::move(a), line), op->ret);
                return convert(std::move(r), to, line);
            }
    }
    x.t = to;
    return x;
}

ASTNodePtr Lowerer::toBool(XV x, int line)
{
    switch (x.t->k)
    {
    case T::Bool: case T::Int: case T::Char: case T::Float: case T::Enum: case T::Dyn:
        return read(x);
    case T::Ptr: case T::UPtr: case T::Func: case T::Optional: case T::CStr: case T::Nullptr:
    {
        std::vector<ASTNodePtr> a;
        a.push_back(read(x));
        return q::un("!", q::call("__cpp_isnull__", std::move(a), line), line);
    }
    case T::Stream:
    {
        std::vector<ASTNodePtr> a;
        a.push_back(read(x));
        return q::call("__cpp_stream_good__", std::move(a), line);
    }
    case T::Class:
        if (x.t->cls)
            for (FuncInfo *op : x.t->cls->convOps)
                if (op->ret->k == T::Bool || isArith(op->ret))
                {
                    std::vector<ASTNodePtr> a;
                    a.push_back(read(x));
                    return q::call(op->mangled, std::move(a), line);
                }
        break;
    default:
        break;
    }
    return read(x);
}

// ─── Names ─────────────────────────────────────────────────────────────────

XV Lowerer::localXV(Local *l)
{
    XV x = localXVRaw(l);
    if (l->isConst)
    {
        x.constLv = true;
        x.constName = "variable '" + l->name + "'";
    }
    return x;
}

bool Lowerer::stdVisible(const std::string &name, Scope *sc)
{
    for (Scope *s = sc; s; s = s->parent)
    {
        if (s == std_)
            return true;
        for (Scope *u : s->usings)
            if (u == std_)
                return true;
        auto ud = s->usingDecls.find(name);
        if (ud != s->usingDecls.end() && ud->second.aliasDecl && !ud->second.aliasDecl->name.empty() &&
            ud->second.aliasDecl->name[0] == "std")
            return true;
    }
    return false;
}

void Lowerer::checkAccess(ClassInfo *owner, const std::string &access, const std::string &what, int line)
{
    if (access == "public" || !owner)
        return;
    std::function<bool(ClassInfo *)> derives = [&](ClassInfo *c)
    {
        for (ClassInfo *b : c->bases)
            if (b == owner || derives(b))
                return true;
        return false;
    };
    // members of the class (and of classes nested in it), and for protected
    // ones of derived classes; friends
    for (FnCtx *f = fn_; f; f = f->parent)
        for (ClassInfo *c = f->cls; c; c = c->outer ? c->outer->cls : nullptr)
        {
            if (c == owner || (access == "protected" && derives(c)) || owner->friends.count(c->name))
                return;
        }
    if (curFn_ && owner->friends.count(curFn_->name))
        return;
    unsupported("'" + what + "' is " + access + " within this context", line);
}

std::string Lowerer::diagType(const TP &t)
{
    if (!t)
        return "?";
    switch (t->k)
    {
    case T::Str:
        return isCBuf(t) ? "char [" + std::to_string(std::max(0L, t->n)) + "]" : "std::string";
    case T::CStr:
        return prog_.cMode ? "char *" : "const char*";
    case T::Ptr:
        return (t->a.empty() ? std::string("void") : diagType(t->a[0])) + "*";
    case T::Class:
        return t->cls ? t->cls->name : "?";
    case T::Void:
        return "void";
    case T::Bool:
        return "bool";
    case T::Vector: case T::Map: case T::Set:
        return "std::" + t->name + "<...>";
    default:
        return t->name.empty() ? t->str() : t->name;
    }
}

XV Lowerer::localXVRaw(Local *l)
{
    XV x;
    x.t = l->t;
    if (l->alias)
    {
        const XV &a = *l->aliasOf;
        x.lk = a.lk;
        x.var = a.var;
        x.obj = q::clone(a.obj);
        x.key = q::clone(a.key);
        x.base = a.base;
        if (a.lk == XV::RV && a.v)
            x.v = q::clone(a.v);
        x.t = a.t ? a.t : l->t;
        return x;
    }
    if (l->boxed)
    {
        x.lk = XV::Idx;
        x.obj = q::id(l->q, line_);
        x.key = q::num(0, line_);
        return x;
    }
    if (l->refSlot)
    {
        x.lk = XV::Idx;
        x.obj = q::index(q::id(l->q, line_), q::num(0, line_), line_);
        x.key = q::index(q::id(l->q, line_), q::num(1, line_), line_);
        return x;
    }
    x.lk = XV::Var;
    x.var = l->q;
    return x;
}

FuncInfo *Lowerer::findMethod(ClassInfo *c, const std::string &name, std::vector<FuncInfo *> &out)
{
    std::function<void(ClassInfo *)> walk = [&](ClassInfo *k)
    {
        bool own = false;
        for (FuncInfo *f : k->methods)
            if (f->name == name)
            {
                // a derived class's method hides the base's of the same name
                bool hidden = false;
                for (FuncInfo *g : out)
                    if (g->vkey == f->vkey)
                        hidden = true;
                if (!hidden)
                    out.push_back(f);
                own = true;
            }
        if (!own || out.empty())
            for (ClassInfo *b : k->bases)
                walk(b);
    };
    walk(c);
    return out.empty() ? nullptr : out[0];
}

void Lowerer::lookupFuncs(const std::vector<std::string> &name, std::vector<FuncInfo *> &out)
{
    Symbol *s = lookup(name, currentScope());
    if (s && s->kind == Symbol::Funcs)
        out = s->funcs;
}

XV Lowerer::lowerName(const ExprP &e)
{
    int ln = e->line;
    const auto &name = e->name;
    if (name.size() == 1)
    {
        const std::string &n = name[0];
        if (n.rfind("#tmp:", 0) == 0)
            return mkxv(q::id(n.substr(5), ln), localTypes_[n], false);
        if (Local *l = findLocal(n))
            return localXV(l);
        ClassInfo *c = currentClass();
        if (c)
        {
            // members (implicit this)
            std::function<FieldInfo *(ClassInfo *)> field = [&](ClassInfo *k) -> FieldInfo *
            {
                for (auto &f : k->fields)
                    if (f.name == n)
                        return &f;
                for (ClassInfo *b : k->bases)
                    if (FieldInfo *f = field(b))
                        return f;
                return nullptr;
            };
            if (FieldInfo *f = field(c))
            {
                XV x;
                x.t = f->type;
                if (f->isStatic && isCBuf(f->type))
                {
                    x.lk = XV::Idx;
                    x.obj = q::id(f->global, ln);
                    x.key = q::num(0, ln);
                }
                else if (f->isStatic)
                {
                    x.lk = XV::Var;
                    x.var = f->global;
                }
                else
                {
                    if (!fn_->hasThis)
                        unsupported("member " + n + " used without an object", ln);
                    x.lk = XV::Idx;
                    x.obj = q::id("__this", ln);
                    x.key = q::num(f->slot, ln);
                }
                return x;
            }
            std::vector<FuncInfo *> ms;
            findMethod(c, n, ms);
            if (!ms.empty())
            {
                XV x;
                x.isFuncName = true;
                x.overloads = ms;
                x.t = tDyn();
                return x;
            }
        }
    }
    Symbol *s = lookup(name, currentScope());
    if (s)
    {
        switch (s->kind)
        {
        case Symbol::Var:
        {
            XV x;
            x.t = s->type ? s->type : tDyn();
            x.lk = XV::Var;
            x.var = s->global;
            if (s->isConst)
            {
                x.constLv = true;
                x.constName = "variable '" + name.back() + "'";
            }
            if (isCBuf(x.t))
            {
                // a char buffer: in a cell
                x.lk = XV::Idx;
                x.obj = q::id(s->global, ln);
                x.key = q::num(0, ln);
            }
            return x;
        }
        case Symbol::Enumerator:
            return mkxv(q::num((double)s->value, ln), s->type);
        case Symbol::Funcs:
        {
            XV x;
            x.isFuncName = true;
            x.overloads = s->funcs;
            x.t = tDyn();
            return x;
        }
        default:
            break;
        }
    }
    bool found = false;
    XV lib = libName(name, ln, found);
    if (found)
        return lib;
    // unknown: dynamic
    std::string full;
    for (size_t i = 0; i < name.size(); ++i)
        full += (i ? "::" : "") + name[i];
    if (std::getenv("QUANTUM_DEBUG_CPPFE"))
        std::cerr << "[cppfe] unknown name " << full << " (line " << ln << ")\n";
    unsupported("unknown name '" + full + "'", ln);
}

// ─── Sizes and the heap ────────────────────────────────────────────────────

long Lowerer::alignOfType(const TP &t)
{
    switch (t->k)
    {
    case T::Char:
    case T::Bool:
        return 1;
    case T::Str:
        return isCBuf(t) ? 1 : 8;
    case T::Int:
    case T::Float:
    case T::Enum:
        return std::min(sizeOfType(t), 16L);
    case T::Array:
        return t->a.empty() ? 1 : alignOfType(t->a[0]);
    case T::Class:
    {
        long al = 1;
        if (t->cls)
        {
            for (ClassInfo *b : t->cls->bases)
            {
                auto bt = mkT(T::Class);
                bt->cls = b;
                al = std::max(al, alignOfType(bt));
            }
            for (auto &f : t->cls->fields)
                if (!f.isStatic && f.type)
                    al = std::max(al, alignOfType(f.type));
            if (t->cls->polymorphic)
                al = std::max(al, 8L);
        }
        return al;
    }
    case T::Pair:
    case T::Tuple:
    {
        long al = 1;
        for (auto &x : t->a)
            al = std::max(al, alignOfType(x));
        return al;
    }
    default:
        return 8;
    }
}

long Lowerer::sizeOfType(const TP &t)
{
    auto roundUp = [](long n, long a) { return a > 1 ? (n + a - 1) / a * a : n; };
    switch (t->k)
    {
    case T::Char:
    case T::Bool:
        return 1;
    case T::Int:
        if (t->name.find("long long") != std::string::npos || t->name == "size_t" || t->name.find("int64") != std::string::npos ||
            t->name.find("ptrdiff") != std::string::npos)
            return 8;
        if (t->name.find("short") != std::string::npos || t->name.find("int16") != std::string::npos)
            return 2;
        if (t->name.find("int8") != std::string::npos)
            return 1;
        return 4; // int, long (LLP64)
    case T::Float:
        return t->name == "float" ? 4 : t->name == "long double" ? 16 : 8;
    case T::Enum:
        return 4;
    case T::Array:
        return t->n > 0 && !t->a.empty() ? t->n * sizeOfType(t->a[0]) : 0;
    case T::Str:
        return isCBuf(t) ? std::max(0L, t->n) : 32;
    case T::Vector:
        return t->name == "vector" ? 24 : t->name == "deque" ? 80 : t->name == "list" ? 24 : 40;
    case T::Map:
    case T::Set:
        return t->sorted ? 48 : 56;
    case T::Pair:
    case T::Tuple:
    {
        long off = 0;
        for (auto &x : t->a)
            off = roundUp(off, alignOfType(x)) + sizeOfType(x);
        return std::max(1L, roundUp(off, alignOfType(t)));
    }
    case T::Class:
    {
        if (!t->cls)
            return 1;
        long off = t->cls->polymorphic && t->cls->bases.empty() ? 8 : 0;
        for (ClassInfo *b : t->cls->bases)
        {
            auto bt = mkT(T::Class);
            bt->cls = b;
            off = roundUp(off, alignOfType(bt)) + sizeOfType(bt);
        }
        bool any = off > 0;
        long start = off;
        for (auto &f : t->cls->fields)
        {
            if (f.isStatic || !f.type)
                continue;
            if (t->cls->isUnion)
                off = std::max(off, roundUp(start, alignOfType(f.type)) + sizeOfType(f.type)); // overlaid
            else
                off = roundUp(off, alignOfType(f.type)) + sizeOfType(f.type);
            any = true;
        }
        return any ? roundUp(off, alignOfType(t)) : 1;
    }
    default:
        return 8; // pointers, references, functions, smart pointers
    }
}

XV Lowerer::materializeAlloc(XV &x, const TP &elem, int line)
{
    std::shared_ptr<XV::Alloc> a = x.alloc;
    x.alloc = nullptr;
    TP pt = mkT(T::Ptr);
    pt->a.push_back(elem);
    // char blocks are strings in this model
    if (elem->k == T::Char)
    {
        std::vector<ASTNodePtr> s;
        if (a->old)
        {
            s.push_back(std::move(*a->old));
            return mkxv(q::call("__cpp_cs_realloc__", std::move(s), line), tCStr());
        }
        std::vector<ASTNodePtr> e;
        e.push_back(q::str("", line));
        s.push_back(std::move(*a->bytes));
        s.push_back(q::call("__cpp_cs_new__", std::move(e), line));
        return mkxv(q::call("__cpp_seq__", std::move(s), line), tCStr());
    }
    std::vector<ASTNodePtr> args;
    if (a->old)
        args.push_back(std::move(*a->old));
    args.push_back(std::move(*a->bytes));
    args.push_back(q::num((double)std::max(1L, sizeOfType(elem)), line));
    args.push_back(defaultValue(elem, line));
    args.push_back(planFor(elem));
    args.push_back(q::boolean(elem->k == T::Class, line));
    return mkxv(q::call(a->old ? "__cpp_realloc__" : "__cpp_malloc__", std::move(args), line), pt);
}

// ─── Operators ─────────────────────────────────────────────────────────────

ASTNodePtr Lowerer::charPtr(XV &x, int line)
{
    // a string held in a slot: [container, key, 0] points into it
    if (x.t->k == T::Str && x.lk == XV::Idx)
    {
        std::vector<ASTNodePtr> e;
        e.push_back(q::clone(x.obj));
        e.push_back(q::clone(x.key));
        e.push_back(q::num(0, line));
        return q::arr(std::move(e), line);
    }
    return read(x);
}

long Lowerer::bufInitSize(const std::vector<ExprP> &args, bool braced)
{
    // char s[] = "abc" / {"abc"}: 4;  char s[] = {'a', 'b'}: 2
    if (args.size() == 1 && args[0]->k == EK::Str)
        return (long)args[0]->str.size() + 1;
    if (args.size() == 1 && args[0]->k == EK::InitList)
        return bufInitSize(args[0]->kids, true);
    return braced ? (long)args.size() : -1;
}

long Lowerer::initSlots(const TP &t)
{
    if (t->k == T::Array && !t->a.empty())
        return std::max(t->n, 1L) * initSlots(t->a[0]);
    if (t->k == T::Class && t->cls && t->cls->aggregate && t->cls->ctors.empty())
    {
        long n = 0;
        for (ClassInfo *b : t->cls->bases)
        {
            auto bt = mkT(T::Class);
            bt->cls = b;
            n += initSlots(bt);
        }
        for (auto &f : t->cls->fields)
            if (!f.isStatic)
                n += initSlots(f.type);
        return std::max(n, 1L);
    }
    return 1;
}

long Lowerer::arrayInitSize(const TP &t, const std::vector<ExprP> &args, bool braced)
{
    // int a[] = {1, 2, 3}: 3;  int m[][2] = {{1, 2}, {3, 4}} or {1, 2, 3, 4}: 2 rows
    const std::vector<ExprP> *kids = &args;
    if (!braced && args.size() == 1 && args[0]->k == EK::InitList)
        kids = &args[0]->kids;
    else if (!braced)
        return -1;
    TP et = t->a.empty() ? tDyn() : t->a[0];
    bool designated = false;
    for (auto &k : *kids)
        designated = designated || k->designIndex;
    if (designated)
    {
        long pos = 0, end = 0;
        for (auto &k : *kids)
        {
            if (k->designIndex)
                pos = (long)evalConst(k->designIndex, currentScope());
            end = std::max(end, ++pos);
        }
        return end;
    }
    if (et->k != T::Array)
        return (long)kids->size();
    long perRow = 1;
    for (TP r = et; r->k == T::Array && !r->a.empty(); r = r->a[0])
        perRow *= std::max(r->n, 1L);
    long rows = 0, loose = 0;
    for (auto &k : *kids)
    {
        if (k->k == EK::InitList)
        {
            rows += (loose + perRow - 1) / perRow + 1;
            loose = 0;
        }
        else
            ++loose;
    }
    return rows + (loose + perRow - 1) / perRow;
}

XV Lowerer::addressOf(XV x, int line)
{
    if (x.isFuncName)
        return convert(std::move(x), mkT(T::Func), line);
    auto pt = mkT(T::Ptr);
    pt->a.push_back(x.t);
    if (x.t->k == T::Array)
    {
        // pointers to arrays are [block, offset] references (a decayed 2-D
        // array is one too): &arr is a one-element block holding it
        std::vector<ASTNodePtr> cell;
        cell.push_back(read(x));
        std::vector<ASTNodePtr> e;
        e.push_back(q::arr(std::move(cell), line));
        e.push_back(q::num(0, line));
        return mkxv(q::arr(std::move(e), line), pt);
    }
    if (isObjectRep(x.t))
    {
        XV r = mkxv(read(x), pt);
        return r;
    }
    switch (x.lk)
    {
    case XV::Var:
    {
        // a local stored in a cell from now on (the function is lowered again)
        bool isLocal = false;
        for (FnCtx *f = fn_; f; f = f->parent)
            for (auto &sc : f->scopes)
                for (auto &kv : sc)
                    if (kv.second.q == x.var)
                        isLocal = true;
        if (!isLocal)
            unsupported("address of a global scalar", line);
        FnCtx *root = fn_;
        while (root->parent)
            root = root->parent;
        root->wantBox.insert(x.var);
        std::vector<ASTNodePtr> e;
        std::vector<ASTNodePtr> cell;
        cell.push_back(q::id(x.var, line));
        e.push_back(q::arr(std::move(cell), line));
        e.push_back(q::num(0, line));
        return mkxv(q::arr(std::move(e), line), pt);
    }
    case XV::Idx:
    {
        std::vector<ASTNodePtr> e;
        e.push_back(q::clone(x.obj));
        e.push_back(q::clone(x.key));
        return mkxv(q::arr(std::move(e), line), pt);
    }
    case XV::StrCh:
    {
        // &buf[i]: a char* into the buffer
        XV &b = *x.base;
        std::vector<ASTNodePtr> e;
        if (b.t->k == T::CStr)
        {
            e.push_back(peekRead(b));
            e.push_back(q::clone(x.key));
            return mkxv(q::call("__cpp_cs_add__", std::move(e), line), tCStr());
        }
        if (b.lk == XV::Idx)
        {
            e.push_back(q::clone(b.obj));
            e.push_back(q::clone(b.key));
            e.push_back(q::clone(x.key));
            return mkxv(q::arr(std::move(e), line), tCStr());
        }
        unsupported("pointer into a std::string", line);
    }
    case XV::RV:
    {
        std::vector<ASTNodePtr> cell;
        cell.push_back(read(x));
        std::vector<ASTNodePtr> e;
        e.push_back(q::arr(std::move(cell), line));
        e.push_back(q::num(0, line));
        return mkxv(q::arr(std::move(e), line), pt);
    }
    }
    return x;
}

XV Lowerer::derefXV(XV x, int line)
{
    TP t = x.t;
    TP inner = deref(t);
    switch (t->k)
    {
    case T::Ptr:
    case T::UPtr:
        if ((isObjectRep(inner) && !(t->k == T::Ptr && inner->k == T::Array)) || inner->k == T::Func)
        {
            XV r = mkxv(read(x), inner, false);
            return r;
        }
        [[fallthrough]];
    case T::Iter:
    {
        if (t->k == T::Iter && !t->a.empty() && isStringy(t->a[0]))
        {
            ASTNodePtr r = simple(read(x), line);
            std::vector<ASTNodePtr> a;
            a.push_back(q::index(q::clone(r), q::num(0, line), line));
            a.push_back(q::index(std::move(r), q::num(1, line), line));
            return mkxv(q::call("__cpp_str_at__", std::move(a), line), tChar());
        }
        ASTNodePtr r = simple(read(x), line);
        XV res;
        res.t = inner;
        res.lk = XV::Idx;
        res.obj = q::index(q::clone(r), q::num(0, line), line);
        res.key = q::index(std::move(r), q::num(1, line), line);
        return res;
    }
    case T::Optional:
    {
        XV res;
        res.t = inner;
        res.lk = XV::Idx;
        res.obj = simple(read(x), line);
        res.key = q::num(0, line);
        return res;
    }
    case T::CStr:
    {
        XV res;
        res.t = tChar();
        res.lk = XV::StrCh;
        res.base = std::make_shared<XV>(mkxv(simple(read(x), line), t));
        res.key = q::num(0, line);
        return res;
    }
    case T::Func:
        return x;
    case T::Class:
        return x; // *this when `this` is typed as the object
    default:
        break;
    }
    std::vector<ASTNodePtr> a;
    a.push_back(read(x));
    return mkxv(q::call("__cpp_deref__", std::move(a), line), tDyn(), false);
}

XV Lowerer::incdec(XV x, bool inc, bool postfix, int line)
{
    TP t = x.t;
    const char *op = inc ? "+" : "-";
    const char *undo = inc ? "-" : "+";
    if (x.lk == XV::Idx)
    {
        x.obj = simple(std::move(x.obj), line);
        x.key = simple(std::move(x.key), line);
    }
    ASTNodePtr cur = peekRead(x);
    ASTNodePtr next;
    if (t->k == T::Iter || t->k == T::Ptr)
    {
        std::vector<ASTNodePtr> a;
        a.push_back(std::move(cur));
        a.push_back(q::num(inc ? 1 : -1, line));
        next = q::call(t->k == T::Iter ? "__cpp_it_add__" : "__cpp_ref_add__", std::move(a), line);
        ASTNodePtr st = store(x, std::move(next), line);
        if (!postfix)
            return mkxv(std::move(st), t);
        std::vector<ASTNodePtr> b;
        b.push_back(std::move(st));
        b.push_back(q::num(inc ? -1 : 1, line));
        return mkxv(q::call(t->k == T::Iter ? "__cpp_it_add__" : "__cpp_ref_add__", std::move(b), line), t);
    }
    if (t->k == T::CStr)
    {
        // char*: p++ yields the saved old pointer
        std::vector<ASTNodePtr> a;
        a.push_back(std::move(cur));
        a.push_back(q::num(inc ? 1 : -1, line));
        ASTNodePtr st = store(x, q::call("__cpp_cs_add__", std::move(a), line), line);
        if (!postfix)
            return mkxv(std::move(st), t);
        std::vector<ASTNodePtr> b;
        b.push_back(peekRead(x));
        b.push_back(std::move(st));
        return mkxv(q::call("__cpp_first__", std::move(b), line), t);
    }
    if (t->k == T::Bool)
        next = q::boolean(inc, line);
    else
        next = q::bin(op, std::move(cur), q::num(1, line), line);
    if (isIntegral(t) && t->isUnsigned && t->k != T::Bool)
    {
        // unsigned arithmetic wraps around (unsigned u = 0; u--;)
        long bytes = sizeOfType(t);
        if (bytes < 8)
        {
            std::vector<ASTNodePtr> w;
            w.push_back(std::move(next));
            w.push_back(q::num(std::ldexp(1.0, (int)(8 * bytes)), line));
            next = q::call("__cpp_wrap__", std::move(w), line);
        }
        else
        {
            std::vector<ASTNodePtr> w;
            w.push_back(std::move(next));
            w.push_back(q::str(t->name, line));
            next = q::call("__cpp_uwrap__", std::move(w), line);
        }
    }
    ASTNodePtr st = store(x, std::move(next), line);
    if (!postfix)
        return mkxv(std::move(st), t);
    return mkxv(q::bin(undo, std::move(st), q::num(1, line), line), t);
}

XV Lowerer::userOperator(const std::string &op, std::vector<ExprP> &operands, std::vector<XV> &vals, int line,
                         bool &handled)
{
    handled = false;
    std::string fname = "operator" + op;
    std::vector<FuncInfo *> cands;
    // member operator of the left operand's class
    bool memberFound = false;
    if (!vals.empty() && vals[0].t->k == T::Class && vals[0].t->cls)
    {
        std::vector<FuncInfo *> ms;
        findMethod(vals[0].t->cls, fname, ms);
        for (FuncInfo *f : ms)
            if (f->params.size() + 1 == vals.size() || (f->params.size() == vals.size() - 1))
            {
                cands.push_back(f);
                memberFound = true;
            }
    }
    // free operators: from here, and from the operands' namespaces (ADL)
    std::vector<FuncInfo *> frees;
    lookupFuncs({fname}, frees);
    std::set<Scope *> seen;
    for (auto &v : vals)
    {
        if (v.t->k == T::Class && v.t->cls && v.t->cls->outer && !seen.count(v.t->cls->outer))
        {
            seen.insert(v.t->cls->outer);
            if (Symbol *s = lookupIn(v.t->cls->outer, fname, false))
                if (s->kind == Symbol::Funcs)
                    for (FuncInfo *f : s->funcs)
                        if (std::find(frees.begin(), frees.end(), f) == frees.end())
                            frees.push_back(f);
        }
        if (v.t->k == T::Enum && v.t->en)
        {
            // operators on enums are found next to the enum
            for (auto &kv : global_->syms)
                (void)kv;
        }
    }
    for (FuncInfo *f : frees)
        if (f->params.size() == vals.size())
            cands.push_back(f);
    if (cands.empty())
        return XV();
    // pick by argument types
    int best = 999;
    FuncInfo *pick = nullptr;
    for (FuncInfo *f : cands)
    {
        bool member = f->cls && !f->isStatic && std::find(f->cls->methods.begin(), f->cls->methods.end(), f) != f->cls->methods.end() && f->params.size() + 1 == vals.size();
        int cost = 0;
        size_t off = member ? 1 : 0;
        if (member)
            cost += convCost(vals[0].t, [&]
                             {
                                 auto t = mkT(T::Class);
                                 t->cls = f->cls;
                                 return t; }());
        for (size_t i = off; i < vals.size(); ++i)
            cost += convCost(vals[i].t, f->params[i - off], &vals[i]);
        if (cost < best)
        {
            best = cost;
            pick = f;
        }
    }
    if (!pick || best >= 9)
        return XV();
    (void)memberFound;
    handled = true;
    bool member = pick->cls && !pick->isStatic && pick->params.size() + 1 == vals.size();
    std::vector<ExprP> rest;
    std::vector<XV> restVals;
    ASTNodePtr self;
    if (member)
    {
        self = read(vals[0]);
        for (size_t i = 1; i < operands.size(); ++i)
        {
            rest.push_back(operands[i]);
            restVals.push_back(std::move(vals[i]));
        }
    }
    else
    {
        rest = operands;
        for (auto &v : vals)
            restVals.push_back(std::move(v));
    }
    return callArgs(pick, rest, std::move(self), line, pick->isVirtual, &restVals);
}

XV Lowerer::arith(const std::string &op, XV a, XV b, int line)
{
    TP ta = a.t, tb = b.t;
    // char* / char buffers: pointer arithmetic
    auto cptrT = [](const TP &t) { return t->k == T::CStr || isCBuf(t); };
    if (cptrT(ta) && isIntegral(tb) && (op == "+" || op == "-"))
    {
        a = convert(std::move(a), tCStr(), line);
        std::vector<ASTNodePtr> args;
        args.push_back(read(a));
        args.push_back(op == "-" ? q::un("-", read(b), line) : read(b));
        return mkxv(q::call("__cpp_cs_add__", std::move(args), line), tCStr());
    }
    if (cptrT(tb) && isIntegral(ta) && ta->k != T::Char && op == "+")
        return arith(op, std::move(b), std::move(a), line);
    if (cptrT(ta) && cptrT(tb) && op == "-")
    {
        a = convert(std::move(a), tCStr(), line);
        b = convert(std::move(b), tCStr(), line);
        std::vector<ASTNodePtr> args;
        args.push_back(read(a));
        args.push_back(read(b));
        return mkxv(q::call("__cpp_cs_diff__", std::move(args), line), mkT(T::Int, "long long"));
    }
    auto stdStr = [](const TP &t) { return t->k == T::Str && !isCBuf(t); };
    if ((stdStr(ta) || stdStr(tb)) && ta->k != T::Dyn && tb->k != T::Dyn && ta->k != T::Class && tb->k != T::Class &&
        !(op == "+" && (isStringy(ta) || ta->k == T::Char) && (isStringy(tb) || tb->k == T::Char)))
        unsupported("no match for 'operator" + op + "' (operand types are '" + diagType(ta) + "' and '" + diagType(tb) +
                        "')",
                    line);
    // strings
    if (op == "+" && (isStringy(ta) || isStringy(tb)))
    {
        auto text = [&](XV &x) -> ASTNodePtr
        {
            if (x.t->k == T::Char)
                return chr(read(x), line);
            if (isCBuf(x.t) || (x.t->k == T::CStr && !(x.v && std::holds_alternative<StringLiteral>(x.v->node))))
            {
                std::vector<ASTNodePtr> c;
                c.push_back(read(x));
                return q::call("__cpp_cs_str__", std::move(c), line);
            }
            return read(x);
        };
        ASTNodePtr l = text(a);
        ASTNodePtr r = text(b);
        return mkxv(q::bin("+", std::move(l), std::move(r), line), tStr());
    }
    // a C array in arithmetic is a pointer to its first element
    if (ta->k == T::Array && !ta->a.empty() && (op == "+" || op == "-") && isIntegral(tb))
    {
        auto pt = mkT(T::Ptr);
        pt->a.push_back(ta->a[0]);
        a = convert(std::move(a), pt, line);
        ta = a.t;
    }
    if (tb->k == T::Array && !tb->a.empty() && op == "+" && isIntegral(ta))
        return arith(op, std::move(b), std::move(a), line);
    // pointers / iterators
    if ((ta->k == T::Ptr || ta->k == T::Iter) && (op == "+" || op == "-") && isIntegral(tb))
    {
        std::vector<ASTNodePtr> args;
        args.push_back(read(a));
        args.push_back(op == "-" ? q::un("-", read(b), line) : read(b));
        return mkxv(q::call(ta->k == T::Iter ? "__cpp_it_add__" : "__cpp_ref_add__", std::move(args), line), ta);
    }
    if (tb->k == T::Iter && op == "+" && isIntegral(ta))
        return arith(op, std::move(b), std::move(a), line);
    if (op == "-" && ((ta->k == T::Ptr && tb->k == T::Array) || (ta->k == T::Array && tb->k == T::Ptr)))
    {
        // p - arr: the array is a pointer to its first element
        auto pt = mkT(T::Ptr);
        pt->a.push_back(ta->k == T::Ptr ? (ta->a.empty() ? tDyn() : ta->a[0]) : tb->a.empty() ? tDyn() : tb->a[0]);
        if (ta->k == T::Array)
            a = convert(std::move(a), pt, line);
        else
            b = convert(std::move(b), pt, line);
        ta = a.t;
        tb = b.t;
    }
    if ((ta->k == T::Ptr || ta->k == T::Iter) && op == "-" && (tb->k == T::Ptr || tb->k == T::Iter))
    {
        std::vector<ASTNodePtr> args;
        args.push_back(read(a));
        args.push_back(read(b));
        return mkxv(q::call("__cpp_it_diff__", std::move(args), line), mkT(T::Int, "long long"));
    }
    if (ta->k == T::Dyn || tb->k == T::Dyn)
        return mkxv(q::bin(op, read(a), read(b), line), ta->k == T::Dyn ? tb : ta);
    TP rt = arithResult(ta, tb);
    if (op == "<<" || op == ">>")
        rt = arithResult(ta, tInt());
    auto num = [&](XV &x, const TP &t) -> ASTNodePtr
    {
        if (t->k == T::Bool)
        {
            std::vector<ASTNodePtr> aa;
            aa.push_back(read(x));
            return q::call("__cpp_b2i__", std::move(aa), line);
        }
        return read(x);
    };
    ASTNodePtr l = num(a, ta), r = num(b, tb);
    if (isIntegral(rt) && (op == "/" || op == "%"))
    {
        std::vector<ASTNodePtr> args;
        args.push_back(std::move(l));
        args.push_back(std::move(r));
        return mkxv(q::call(op == "/" ? "__cpp_idiv__" : "__cpp_imod__", std::move(args), line), rt);
    }
    if (op == "%" && isFloat(rt))
    {
        std::vector<ASTNodePtr> args;
        args.push_back(std::move(l));
        args.push_back(std::move(r));
        return mkxv(q::call("__cpp_fmod__", std::move(args), line), rt);
    }
    ASTNodePtr res = q::bin(op, std::move(l), std::move(r), line);
    if (isIntegral(rt) && rt->isUnsigned && op == "-")
    {
        // unsigned subtraction wraps (size_t i = 0; i - 1)
        std::vector<ASTNodePtr> args;
        args.push_back(std::move(res));
        args.push_back(q::str(rt->name, line));
        res = q::call("__cpp_uwrap__", std::move(args), line);
    }
    return mkxv(std::move(res), rt);
}

XV Lowerer::compareOp(const std::string &op, XV a, XV b, int line)
{
    TP ta = a.t, tb = b.t;
    auto viaNative = [&](const char *fn, bool negate) -> XV
    {
        std::vector<ASTNodePtr> args;
        args.push_back(read(a));
        args.push_back(read(b));
        ASTNodePtr c = q::call(fn, std::move(args), line);
        return mkxv(negate ? q::un("!", std::move(c), line) : std::move(c), tBool());
    };
    bool ptrish = [&](const TP &t)
    { return t->k == T::Ptr || t->k == T::UPtr || t->k == T::Func || t->k == T::Nullptr || t->k == T::Optional; }(ta) ||
                  [&](const TP &t)
    { return t->k == T::Ptr || t->k == T::UPtr || t->k == T::Func || t->k == T::Nullptr || t->k == T::Optional; }(tb);
    if (ptrish && (op == "==" || op == "!="))
        return viaNative("__cpp_same__", op == "!="); // refs compare by (container, key)
    if ((ta->k == T::CStr && tb->k == T::Nullptr) || (tb->k == T::CStr && ta->k == T::Nullptr))
        return viaNative("__cpp_same__", op == "!=");
    // char* / char buffers: by position
    auto cptrT = [](const TP &t) { return t->k == T::CStr || isCBuf(t); };
    if (cptrT(ta) && cptrT(tb) && (ta->k == T::CStr || tb->k == T::CStr || prog_.cMode))
    {
        a = convert(std::move(a), tCStr(), line);
        b = convert(std::move(b), tCStr(), line);
        if (op == "==" || op == "!=")
            return viaNative("__cpp_same__", op == "!=");
        std::vector<ASTNodePtr> args;
        args.push_back(read(a));
        args.push_back(read(b));
        return mkxv(q::bin(op, q::call("__cpp_cs_diff__", std::move(args), line), q::num(0, line), line), tBool());
    }
    // std::string vs char*: by text
    if (ta->k == T::Str && !isCBuf(ta) && tb->k == T::CStr)
        b = convert(std::move(b), ta, line);
    if (tb->k == T::Str && !isCBuf(tb) && ta->k == T::CStr)
        a = convert(std::move(a), tb, line);
    // pointers into the same block: compared by offset
    if ((ta->k == T::Ptr || ta->k == T::Array) && (tb->k == T::Ptr || tb->k == T::Array) &&
        (op == "<" || op == "<=" || op == ">" || op == ">="))
    {
        auto pt = mkT(T::Ptr);
        pt->a.push_back(ta->k == T::Ptr ? (ta->a.empty() ? tDyn() : ta->a[0]) : ta->a[0]);
        if (ta->k == T::Array)
            a = convert(std::move(a), pt, line);
        if (tb->k == T::Array)
            b = convert(std::move(b), pt, line);
        std::vector<ASTNodePtr> args;
        args.push_back(read(a));
        args.push_back(read(b));
        return mkxv(q::bin(op, q::call("__cpp_it_diff__", std::move(args), line), q::num(0, line), line), tBool());
    }
    if (ta->k == T::Iter || tb->k == T::Iter)
    {
        if (op == "==" || op == "!=")
            return viaNative("__cpp_it_eq__", op == "!=");
        std::vector<ASTNodePtr> args;
        args.push_back(read(a));
        args.push_back(read(b));
        return mkxv(q::bin(op, q::call("__cpp_it_diff__", std::move(args), line), q::num(0, line), line), tBool());
    }
    bool deep = isObjectRep(ta) || ta->k == T::Variant || isObjectRep(tb);
    if (deep)
    {
        if (op == "==" || op == "!=")
            return viaNative("__cpp_eq__", op == "!=");
        std::vector<ASTNodePtr> args;
        args.push_back(read(a));
        args.push_back(read(b));
        return mkxv(q::bin(op, q::call("__cpp_cmp__", std::move(args), line), q::num(0, line), line), tBool());
    }
    ASTNodePtr l = read(a), r = read(b);
    if (ta->k == T::Bool && isArith(tb) && tb->k != T::Bool)
    {
        std::vector<ASTNodePtr> aa;
        aa.push_back(std::move(l));
        l = q::call("__cpp_b2i__", std::move(aa), line);
    }
    if (tb->k == T::Bool && isArith(ta) && ta->k != T::Bool)
    {
        std::vector<ASTNodePtr> aa;
        aa.push_back(std::move(r));
        r = q::call("__cpp_b2i__", std::move(aa), line);
    }
    if (isStringy(ta) && tb->k == T::Char)
        r = chr(std::move(r), line);
    if (isStringy(tb) && ta->k == T::Char)
        l = chr(std::move(l), line);
    return mkxv(q::bin(op, std::move(l), std::move(r), line), tBool());
}

XV Lowerer::lowerUnary(const ExprP &e)
{
    int ln = e->line;
    const std::string &op = e->op;
    if (op == "++" || op == "--")
    {
        XV x = lowerExpr(e->kids[0]);
        if (x.constLv)
            unsupported(std::string(op == "++" ? "increment" : "decrement") + " of read-only " + x.constName, ln);
        if (x.t->k == T::Class)
        {
            std::vector<ExprP> ops = {e->kids[0]};
            std::vector<XV> vals;
            vals.push_back(std::move(x));
            bool h = false;
            XV r = userOperator(op, ops, vals, ln, h);
            if (h)
                return r;
            unsupported("no operator" + op, ln);
        }
        return incdec(std::move(x), op == "++", false, ln);
    }
    if (op == "&")
    {
        XV x = lowerExpr(e->kids[0]);
        return addressOf(std::move(x), ln);
    }
    if (op == "*")
    {
        XV x = lowerExpr(e->kids[0]);
        if (x.t->k == T::Class && x.t->cls)
        {
            std::vector<ExprP> ops = {e->kids[0]};
            std::vector<XV> vals;
            vals.push_back(std::move(x));
            bool h = false;
            XV r = userOperator("*", ops, vals, ln, h);
            if (h)
                return r;
            unsupported("unary * on a class", ln);
        }
        return derefXV(std::move(x), ln);
    }
    XV x = lowerExpr(e->kids[0]);
    if (x.t->k == T::Class)
    {
        std::vector<ExprP> ops = {e->kids[0]};
        std::vector<XV> vals;
        vals.push_back(std::move(x));
        bool h = false;
        XV r = userOperator(op, ops, vals, ln, h);
        if (h)
            return r;
        unsupported("no operator" + op + " for " + vals[0].t->str(), ln);
    }
    if (op == "!")
        return mkxv(q::un("!", toBool(std::move(x), ln), ln), tBool());
    if (op == "-")
    {
        TP rt = x.t->k == T::Dyn ? tDyn() : arithResult(x.t, tInt());
        if (isFloat(x.t))
            rt = x.t;
        ASTNodePtr v = x.t->k == T::Bool ? q::call("__cpp_b2i__", [&]
                                                   {
                                                       std::vector<ASTNodePtr> a;
                                                       a.push_back(read(x));
                                                       return a; }(),
                                                   ln)
                                         : read(x);
        return mkxv(q::un("-", std::move(v), ln), rt);
    }
    if (op == "+")
    {
        TP rt = isFloat(x.t) || x.t->k == T::Dyn ? x.t : arithResult(x.t, tInt());
        ASTNodePtr v = read(x);
        return mkxv(std::move(v), rt);
    }
    if (op == "~")
    {
        TP rt = arithResult(x.t, tInt());
        ASTNodePtr v = read(x);
        if (rt->isUnsigned)
        {
            std::vector<ASTNodePtr> a;
            a.push_back(q::un("~", std::move(v), ln));
            a.push_back(q::str(rt->name, ln));
            return mkxv(q::call("__cpp_uwrap__", std::move(a), ln), rt);
        }
        return mkxv(q::un("~", std::move(v), ln), rt);
    }
    unsupported("unary " + op, ln);
}

XV Lowerer::lowerBinary(const ExprP &e)
{
    int ln = e->line;
    const std::string &op = e->op;
    if (op == "&&" || op == "||")
    {
        XV a = lowerExpr(e->kids[0]);
        ASTNodePtr l = toBool(std::move(a), ln);
        // the right side is only evaluated sometimes: no hoisting into it
        bool saved = canHoist_;
        canHoist_ = false;
        XV b = lowerExpr(e->kids[1]);
        ASTNodePtr r = toBool(std::move(b), ln);
        canHoist_ = saved;
        return mkxv(q::bin(op, std::move(l), std::move(r), ln), tBool());
    }
    XV a = lowerExpr(e->kids[0]);
    // streams
    if (a.t->k == T::Stream && (op == "<<" || op == ">>"))
    {
        if (op == "<<")
        {
            XV b = lowerExpr(e->kids[1]);
            if (b.t->k == T::Class || (b.t->k == T::Enum && false))
            {
                std::vector<ExprP> ops = {e->kids[0], e->kids[1]};
                std::vector<XV> vals;
                vals.push_back(std::move(a));
                vals.push_back(std::move(b));
                bool h = false;
                XV r = userOperator("<<", ops, vals, ln, h);
                if (h)
                {
                    r.t = mkT(T::Stream, "ostream");
                    return r;
                }
                unsupported("no operator<< for " + vals[1].t->str(), ln);
            }
            if (b.isFuncName)
            {
                // std::endl and friends written as functions
                unsupported("stream manipulator function", ln);
            }
            std::vector<ASTNodePtr> args;
            TP st = a.t;
            args.push_back(read(a));
            ASTNodePtr kind = streamKind(b.t);
            args.push_back(read(b));
            args.push_back(std::move(kind));
            return mkxv(q::call("__cpp_out__", std::move(args), ln), st, false);
        }
        // s >> x : x = __cpp_in__(s, kind); value is the stream
        XV b = lowerExpr(e->kids[1]);
        TP st = a.t;
        ASTNodePtr s = simple(read(a), ln);
        std::vector<ASTNodePtr> args;
        args.push_back(q::clone(s));
        args.push_back(streamKind(b.t));
        ASTNodePtr in = q::call("__cpp_in__", std::move(args), ln);
        XV conv = convert(mkxv(std::move(in), b.t->k == T::Dyn ? tDyn() : b.t), b.t, ln);
        ASTNodePtr st2 = store(b, read(conv), ln);
        std::vector<ASTNodePtr> seq;
        seq.push_back(q::call("__cpp_in_ok__", [&]
                              {
                                  std::vector<ASTNodePtr> x;
                                  x.push_back(q::clone(s));
                                  return x; }(),
                              ln));
        seq.push_back(std::move(st2));
        seq.push_back(std::move(s));
        // only store when the read succeeded: __cpp_in_seq__(ok, store, stream)
        return mkxv(q::call("__cpp_seq3__", std::move(seq), ln), st, false);
    }
    XV b = lowerExpr(e->kids[1]);
    if (a.t->k == T::Class || b.t->k == T::Class ||
        (a.t->k == T::Enum && a.t->en && a.t->en->scoped && op != "==" && op != "!=" && op != "<" && op != ">" &&
         op != "<=" && op != ">="))
    {
        std::vector<ExprP> ops = {e->kids[0], e->kids[1]};
        std::vector<XV> vals;
        vals.push_back(std::move(a));
        vals.push_back(std::move(b));
        bool h = false;
        XV r = userOperator(op, ops, vals, ln, h);
        if (h)
            return r;
        // comparisons of objects without operators: by value
        a = std::move(vals[0]);
        b = std::move(vals[1]);
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=")
            return compareOp(op, std::move(a), std::move(b), ln);
        if (op == "!=" )
            return compareOp(op, std::move(a), std::move(b), ln);
        unsupported("no operator" + op + " for " + a.t->str(), ln);
    }
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=")
        return compareOp(op, std::move(a), std::move(b), ln);
    if (op == "<=>")
    {
        std::vector<ASTNodePtr> args;
        args.push_back(read(a));
        args.push_back(read(b));
        return mkxv(q::call("__cpp_cmp__", std::move(args), ln), tInt());
    }
    return arith(op, std::move(a), std::move(b), ln);
}

XV Lowerer::lowerAssign(const ExprP &e)
{
    int ln = e->line;
    std::string op = e->op;
    XV t = lowerExpr(e->kids[0]);
    if (t.constLv)
        unsupported("assignment of read-only " + t.constName, ln);
    if (t.t->k == T::Dyn && t.t->name == "tie" && op == "=")
    {
        // std::tie(a, b) = pair / tuple
        XV v = lowerExpr(e->kids[1]);
        std::vector<ASTNodePtr> a;
        a.push_back(read(t));
        a.push_back(read(v));
        return mkxv(q::call("__cpp_tie_assign__", std::move(a), ln), tVoid());
    }
    if (t.lk == XV::Idx)
    {
        t.obj = simple(std::move(t.obj), ln);
        t.key = simple(std::move(t.key), ln);
    }
    if (t.t->k == T::Class && t.t->cls)
    {
        // user operator= / op=
        std::vector<FuncInfo *> ms;
        findMethod(t.t->cls, "operator" + op, ms);
        if (!ms.empty() && !(op == "=" && ms.size() == 1 && ms[0]->decl && ms[0]->decl->isDefault))
        {
            std::vector<ExprP> ops = {e->kids[0], e->kids[1]};
            std::vector<XV> vals;
            vals.push_back(std::move(t));
            vals.push_back(lowerExpr(e->kids[1]));
            bool h = false;
            XV r = userOperator(op, ops, vals, ln, h);
            if (h)
                return r;
            t = std::move(vals[0]);
        }
        else if (op != "=")
        {
            // a += b with only operator+ : t = t + b
            std::string bop = op.substr(0, op.size() - 1);
            std::vector<ExprP> ops = {e->kids[0], e->kids[1]};
            std::vector<XV> vals;
            vals.push_back(mkxv(peekRead(t), t.t, false));
            vals.push_back(lowerExpr(e->kids[1]));
            bool h = false;
            XV r = userOperator(bop, ops, vals, ln, h);
            if (h)
                return mkxv(store(t, read(r), ln), t.t, false);
        }
    }
    if (op == "=")
    {
        XV v = lowerExprTo(e->kids[1], t.t);
        v = convert(std::move(v), t.t, ln);
        ASTNodePtr val = copyOf(std::move(v), ln);
        TP tt = t.t;
        if (tt->k == T::UPtr && ownsResource(tt) && canHoist_ && pre_)
        {
            // the old pointee is released after the new one is in place
            ASTNodePtr old = simple(peekRead(t), ln);
            std::vector<ASTNodePtr> a;
            a.push_back(store(t, std::move(val), ln));
            a.push_back(destroyValue(tt, std::move(old), ln));
            return mkxv(q::call("__cpp_first__", std::move(a), ln), tt, false);
        }
        return mkxv(store(t, std::move(val), ln), tt, false);
    }
    // compound: a op= b  →  a = a op b
    std::string bop = op.substr(0, op.size() - 1);
    if (t.t->k == T::Str && bop == "+")
    {
        XV v = lowerExprTo(e->kids[1], tStr());
        ASTNodePtr r = v.t->k == T::Char ? chr(read(v), ln) : read(v);
        ASTNodePtr cur = peekRead(t);
        TP tt = t.t;
        return mkxv(store(t, q::bin("+", std::move(cur), std::move(r), ln), ln), tt, false);
    }
    XV cur = mkxv(peekRead(t), t.t, false);
    XV v = lowerExpr(e->kids[1]);
    XV res = arith(bop, std::move(cur), std::move(v), ln);
    res = convert(std::move(res), t.t, ln);
    TP tt = t.t;
    return mkxv(store(t, read(res), ln), tt, false);
}

XV Lowerer::fieldOf(XV obj, ClassInfo *c, const std::string &name, int line, bool &found)
{
    found = false;
    ClassInfo *owner = nullptr;
    std::function<FieldInfo *(ClassInfo *)> field = [&](ClassInfo *k) -> FieldInfo *
    {
        for (auto &f : k->fields)
            if (f.name == name)
            {
                owner = k;
                return &f;
            }
        for (ClassInfo *b : k->bases)
            if (FieldInfo *f = field(b))
                return f;
        return nullptr;
    };
    FieldInfo *f = field(c);
    if (!f)
        return obj;
    found = true;
    checkAccess(owner, f->access, diagType(f->type) + " " + owner->name + "::" + name, line);
    XV x;
    x.t = f->type;
    if (f->isConst)
    {
        x.constLv = true;
        x.constName = "member '" + owner->name + "::" + name + "'";
    }
    if (f->isStatic)
    {
        x.lk = XV::Var;
        x.var = f->global;
        return x;
    }
    x.lk = XV::Idx;
    x.obj = simple(read(obj), line);
    x.key = q::num(f->slot, line);
    return x;
}

XV Lowerer::lowerMember(const ExprP &e)
{
    int ln = e->line;
    XV obj = lowerExpr(e->kids[0]);
    if (e->arrow)
    {
        if (obj.t->k == T::Class && obj.t->cls)
        {
            // smart-pointer-like class with operator->
            std::vector<FuncInfo *> ms;
            findMethod(obj.t->cls, "operator->", ms);
            if (!ms.empty())
            {
                std::vector<ExprP> none;
                XV r = callArgs(ms[0], none, read(obj), ln, false);
                obj = derefXV(std::move(r), ln);
            }
        }
        else
            obj = derefXV(std::move(obj), ln);
    }
    const std::string &m = e->op;
    TP t = obj.t;
    if (t->k == T::Class && t->cls)
    {
        bool found = false;
        XV f = fieldOf(std::move(obj), t->cls, m, ln, found);
        if (found)
            return f;
        if (t->cls->builtin)
            unsupported("member " + m + " of " + t->cls->qname, ln);
        unsupported("no member " + m + " in " + t->cls->qname, ln);
    }
    if (t->k == T::Pair && (m == "first" || m == "second"))
    {
        XV x;
        x.t = t->a[m == "first" ? 0 : 1];
        x.lk = XV::Idx;
        x.obj = simple(read(obj), ln);
        x.key = q::num(m == "first" ? 0 : 1, ln);
        return x;
    }
    if (t->k == T::Dyn)
    {
        XV x;
        x.t = tDyn();
        x.v = std::make_unique<ASTNode>(MemberExpr{read(obj), m}, ln);
        return x;
    }
    unsupported("member " + m + " of " + t->str(), ln);
}

XV Lowerer::lowerIndex(const ExprP &e)
{
    int ln = e->line;
    XV obj = lowerExpr(e->kids[0]);
    TP t = obj.t;
    if (t->k == T::Class && t->cls)
    {
        std::vector<ExprP> ops = {e->kids[0], e->kids[1]};
        std::vector<XV> vals;
        vals.push_back(std::move(obj));
        vals.push_back(lowerExpr(e->kids[1]));
        bool h = false;
        XV r = userOperator("[]", ops, vals, ln, h);
        if (h)
            return r;
        unsupported("no operator[] for " + vals[0].t->str(), ln);
    }
    if (t->k == T::Map)
    {
        XV k = lowerExprTo(e->kids[1], t->a[0]);
        k = convert(std::move(k), t->a[0], ln);
        std::vector<ASTNodePtr> args;
        args.push_back(read(obj));
        args.push_back(copyOf(std::move(k), ln));
        args.push_back(defaultValue(t->a[1], ln));
        ASTNodePtr pair = simple(q::call("__cpp_map_get__", std::move(args), ln), ln);
        XV x;
        x.t = t->a[1];
        x.lk = XV::Idx;
        x.obj = std::move(pair);
        x.key = q::num(1, ln);
        return x;
    }
    XV i = lowerExpr(e->kids[1]);
    if (t->k == T::Str || t->k == T::CStr)
    {
        XV x;
        x.t = tChar();
        ASTNodePtr key = simple(read(i), ln);
        if (t->k == T::CStr)
        {
            // through a char*: writes reach the buffer it points into
            x.lk = XV::StrCh;
            x.base = std::make_shared<XV>(mkxv(simple(read(obj), ln), t));
            x.key = std::move(key);
            return x;
        }
        if (obj.lk != XV::RV && t->k == T::Str)
        {
            auto base = std::make_shared<XV>(std::move(obj));
            if (base->lk == XV::Idx)
            {
                base->obj = simple(std::move(base->obj), ln);
                base->key = simple(std::move(base->key), ln);
            }
            base->v.reset();
            x.lk = XV::StrCh;
            x.base = base;
            x.key = std::move(key);
            return x;
        }
        std::vector<ASTNodePtr> a;
        a.push_back(read(obj));
        a.push_back(std::move(key));
        return mkxv(q::call("__cpp_str_at__", std::move(a), ln), tChar());
    }
    if (t->k == T::Ptr || t->k == T::UPtr)
    {
        TP inner = deref(t);
        ASTNodePtr r = simple(read(obj), ln);
        if (isObjectRep(inner))
        {
            std::vector<ASTNodePtr> a;
            a.push_back(std::move(r));
            a.push_back(read(i));
            return mkxv(q::call("__cpp_ptr_index__", std::move(a), ln), inner, false);
        }
        XV x;
        x.t = inner;
        x.lk = XV::Idx;
        x.obj = q::index(q::clone(r), q::num(0, ln), ln);
        x.key = simple(q::bin("+", q::index(std::move(r), q::num(1, ln), ln), read(i), ln), ln);
        return x;
    }
    XV x;
    x.t = elemType(t);
    x.lk = XV::Idx;
    x.obj = simple(read(obj), ln);
    x.key = simple(read(i), ln);
    return x;
}

XV Lowerer::lowerCast(const ExprP &e)
{
    int ln = e->line;
    TP to = resolveType(e->type, currentScope());
    XV x = lowerExpr(e->kids[0]);
    if (to->k == T::Void)
    {
        std::vector<ASTNodePtr> a;
        a.push_back(read(x));
        a.push_back(q::nil(ln));
        return mkxv(q::call("__cpp_seq__", std::move(a), ln), tVoid());
    }
    if (e->op == "dynamic_cast" && (to->k == T::Ptr || to->k == T::Class) )
    {
        TP target = to->k == T::Ptr ? to->a[0] : to;
        if (target->k == T::Class && target->cls)
        {
            std::vector<ASTNodePtr> a;
            a.push_back(read(x));
            a.push_back(q::str(target->cls->qname, ln));
            a.push_back(q::boolean(to->k == T::Class, ln)); // reference cast throws
            return mkxv(q::call("__cpp_dyncast__", std::move(a), ln), to);
        }
    }
    if (e->op == "reinterpret_cast" || e->op == "const_cast")
    {
        x.t = to;
        return x;
    }
    if (to->k == T::Class && x.t->k != T::Class)
    {
        std::vector<ExprP> args = e->kids;
        return mkxv(trackTemp(ctorCall(to->cls, args, false, ln), to, ln), to);
    }
    if (to->k == T::Ptr && x.t->k == T::Ptr)
    {
        x.t = to;
        return x;
    }
    if (isIntegral(to) && to->k != T::Bool && x.t->k == T::Char)
    {
        x.t = to;
        return x;
    }
    bool savedExplicit = explicitConv_;
    explicitConv_ = true;
    XV r = convert(std::move(x), to, ln);
    explicitConv_ = savedExplicit;
    r.t = to;
    r.temp = true;
    return r;
}

TP Lowerer::lambdaReturn(const LambdaP &l)
{
    return l->ret ? resolveType(l->ret, currentScope()) : tDyn();
}

// The single names an expression / statement mentions (nested lambdas included)
static void namesUsed(const ExprP &e, std::set<std::string> &out);
static void namesUsed(const StmtP &s, std::set<std::string> &out)
{
    if (!s)
        return;
    namesUsed(s->expr, out);
    for (const auto &d : s->decls)
        for (const auto &a : d.args)
            namesUsed(a, out);
    for (const auto &b : s->body)
        namesUsed(b, out);
    namesUsed(s->init, out);
    namesUsed(s->then, out);
    namesUsed(s->els, out);
    namesUsed(s->cond2, out);
    namesUsed(s->step, out);
    namesUsed(s->condDecl, out);
    for (const auto &c : s->catches)
        namesUsed(c.body, out);
}
static void namesUsed(const ExprP &e, std::set<std::string> &out)
{
    if (!e)
        return;
    if (e->k == EK::Name && e->name.size() == 1)
        out.insert(e->name[0]);
    for (const auto &k : e->kids)
        namesUsed(k, out);
    if (e->lambda)
    {
        namesUsed(e->lambda->body, out);
        for (const auto &c : e->lambda->captures)
            if (!c.empty() && c[0] != '&')
                out.insert(c);
    }
}

XV Lowerer::lowerLambda(const ExprP &e)
{
    int ln = e->line;
    const Lambda &l = *e->lambda;
    // Captured by value ([x], [=]): copies taken now, bound under the same
    // names by a function called at once, so the closure sees the copies.
    std::vector<Local> byValue;
    {
        std::set<std::string> used;
        if (l.captureAllByValue)
            namesUsed(l.body, used);
        for (const auto &c : l.captures)
            if (!c.empty() && c[0] != '&' && c != "this" && c != "*this" && c != "=")
                used.insert(c);
        for (const auto &c : l.captures)
            if (!c.empty() && c[0] == '&')
                used.erase(c.substr(1));
        for (const auto &n : used)
            if (Local *loc = fn_ ? findLocal(n) : nullptr)
                if (!loc->alias)
                    byValue.push_back(*loc);
    }
    XV lam = lowerLambdaBody(e);
    if (byValue.empty())
        return lam;
    std::vector<std::string> ps;
    std::vector<ASTNodePtr> args;
    for (Local &loc : byValue)
    {
        ps.push_back(loc.q);
        XV cur = localXV(&loc);
        cur.constLv = false;
        ASTNodePtr v = copyOf(std::move(cur), ln);
        if (loc.boxed || loc.refSlot)
        {
            // the closure reads it through a cell: a cell of its own
            std::vector<ASTNodePtr> cell, ref;
            cell.push_back(std::move(v));
            v = q::arr(std::move(cell), ln);
            if (loc.refSlot)
            {
                ref.push_back(std::move(v));
                ref.push_back(q::num(0, ln));
                v = q::arr(std::move(ref), ln);
            }
        }
        args.push_back(std::move(v));
    }
    std::vector<ASTNodePtr> body;
    body.push_back(q::ret(read(lam), ln));
    return mkxv(q::call(q::lambda(ps, q::block(std::move(body), ln), ln), std::move(args), ln), lam.t);
}

XV Lowerer::lowerLambdaBody(const ExprP &e)
{
    int ln = e->line;
    const Lambda &l = *e->lambda;
    FnCtx ctx;
    ctx.parent = fn_;
    ctx.isLambda = true;
    ctx.cls = fn_ ? fn_->cls : nullptr;
    ctx.hasThis = fn_ && fn_->hasThis;
    ctx.scope = currentScope();
    ctx.ret = l.ret ? resolveType(l.ret, currentScope()) : tDyn();
    ctx.retIsRef = l.ret && (l.ret->ref || l.ret->rref);
    FnCtx *saved = fn_;
    auto savedPre = pre_;
    bool savedHoist = canHoist_;
    fn_ = &ctx;
    pushScope();
    std::vector<std::string> params;
    std::vector<ASTNodePtr> stmts;
    auto ft = mkT(T::Func);
    ft->lambda = true;
    ft->a.push_back(ctx.ret);
    std::vector<TP> hints;
    hints.swap(lambdaHints_);
    for (size_t i = 0; i < l.params.size(); ++i)
    {
        TP pt = l.params[i].type ? resolveType(l.params[i].type, currentScope()) : tDyn();
        if (pt->k == T::Dyn && i < hints.size() && hints[i])
            pt = hints[i]; // auto parameter: the type the caller passes
        std::string n = l.params[i].name.empty() ? "__arg" + std::to_string(i) : l.params[i].name;
        Local &loc = declareLocal(n, pt);
        params.push_back(loc.q);
        ft->a.push_back(pt);
        if (loc.boxed)
        {
            std::vector<ASTNodePtr> cell;
            cell.push_back(q::id(loc.q, ln));
            stmts.push_back(q::exprStmt(q::assign(q::id(loc.q, ln), q::arr(std::move(cell), ln), ln), ln));
        }
    }
    for (const auto &s : l.body->body)
        if (auto ls = lowerStmt(s))
            stmts.push_back(std::move(ls));
    emitDestroy(0, stmts);
    popScope();
    if (ctx.ret->k == T::Dyn && ctx.deducedRet)
        ft->a[0] = ctx.deducedRet;
    else if (ctx.ret->k == T::Dyn && !ctx.deducedRet)
        ft->a[0] = tVoid();
    // boxing requests of the enclosing function's locals were recorded on the root
    fn_ = saved;
    pre_ = savedPre;
    canHoist_ = savedHoist;
    return mkxv(q::lambda(params, q::block(std::move(stmts), ln), ln), ft);
}

ASTNodePtr Lowerer::defaultCtor(ClassInfo *c, ASTNodePtr into, int line)
{
    // a user default constructor, else the implicit one
    for (FuncInfo *f : c->ctors)
    {
        size_t required = 0;
        for (size_t i = 0; i < f->params.size(); ++i)
            if (!(i < f->defaults.size() && f->defaults[i]))
                ++required;
        if (required == 0)
        {
            std::vector<ExprP> none;
            XV r = callArgs(f, none, std::move(into), line, false);
            return read(r);
        }
    }
    std::vector<ASTNodePtr> a;
    a.push_back(std::move(into));
    return q::call(c->defaultCtorFn(), std::move(a), line);
}

ASTNodePtr Lowerer::ctorCall(ClassInfo *c, std::vector<ExprP> &args, bool braced, int line, ASTNodePtr into)
{
    if (!c->resolved)
        resolveClass(c);
    if (c->builtin)
    {
        ASTNodePtr msg = q::str("", line);
        if (!args.empty())
        {
            XV m = lowerExpr(args[0]);
            msg = read(m);
        }
        std::vector<ASTNodePtr> a;
        if (into)
        {
            a.push_back(std::move(into));
            a.push_back(std::move(msg));
            a.push_back(q::id(c->desc(), line));
            return q::call("__cpp_exc_init__", std::move(a), line);
        }
        a.push_back(q::id(c->desc(), line));
        a.push_back(std::move(msg));
        return q::call("__cpp_exc__", std::move(a), line);
    }
    bool inPlace = into != nullptr;
    auto alloc = [&]() -> ASTNodePtr
    {
        if (into)
            return std::move(into);
        std::vector<ASTNodePtr> a;
        a.push_back(q::id(c->desc(), line));
        a.push_back(q::num(c->nslots, line));
        return q::call("__cpp_alloc__", std::move(a), line);
    };
    if (args.empty())
        return defaultCtor(c, alloc(), line);
    // copy / move construction from the same class
    if (args.size() == 1 && args[0]->k != EK::InitList && probeType(args[0])->k == T::Class)
    {
        bool userCopy = false;
        XV a0 = lowerExpr(args[0]);
        if (a0.t->k == T::Class && convCost(a0.t, [&]
                                            {
                                                auto t = mkT(T::Class);
                                                t->cls = c;
                                                return t; }()) <= 2)
        {
            for (FuncInfo *f : c->ctors)
                if (f->params.size() == 1 && f->params[0]->k == T::Class && f->params[0]->cls == c && f->def)
                    userCopy = true;
            if (!userCopy)
            {
                auto ct = mkT(T::Class);
                ct->cls = c;
                ASTNodePtr v = copyOf(std::move(a0), line);
                if (inPlace)
                {
                    std::vector<ASTNodePtr> aa;
                    aa.push_back(alloc());
                    aa.push_back(std::move(v));
                    return q::call("__cpp_assign__", std::move(aa), line);
                }
                return v;
            }
        }
    }
    std::vector<FuncInfo *> cands = c->ctors;
    for (ClassInfo *b : c->inheritCtorsFrom)
        for (FuncInfo *f : b->ctors)
            cands.push_back(f);
    if (cands.empty())
    {
        // `using std::runtime_error::runtime_error;`: built from the message
        for (ClassInfo *b : c->inheritCtorsFrom)
            if (b->builtin)
            {
                XV m = lowerExpr(args[0]);
                std::vector<ASTNodePtr> a;
                a.push_back(defaultCtor(c, alloc(), line));
                a.push_back(read(m));
                a.push_back(q::id(b->desc(), line));
                return q::call("__cpp_exc_init__", std::move(a), line);
            }
    }
    if (braced && cands.empty())
    {
        // aggregate: fields in order (or as designated), the rest default; a
        // scalar where an array / struct member starts takes as many scalars
        // as the member holds (brace elision: `Queue q = {0}`)
        std::vector<const FieldInfo *> fields;
        for (const auto &f : c->fields)
            if (!f.isStatic)
                fields.push_back(&f);
        std::vector<ExprP> given(fields.size());
        size_t fi = 0;
        for (size_t k = 0; k < args.size(); ++k)
        {
            const ExprP &x = args[k];
            if (!x->designator.empty())
                for (size_t j = 0; j < fields.size(); ++j)
                    if (fields[j]->name == x->designator)
                        fi = j;
            if (fi >= fields.size())
                break;
            TP ft = fields[fi]->type;
            long slots = x->k == EK::InitList ? 1 : initSlots(ft);
            if (slots > 1 && x->k != EK::InitList && isArith(probeType(x)))
            {
                auto row = std::make_shared<Expr>();
                row->k = EK::InitList;
                row->line = x->line;
                row->kids.push_back(x);
                while ((long)row->kids.size() < slots && k + 1 < args.size() && args[k + 1]->k != EK::InitList &&
                       args[k + 1]->designator.empty() && !args[k + 1]->designIndex)
                    row->kids.push_back(args[++k]);
                given[fi++] = row;
            }
            else
                given[fi++] = x;
        }
        std::vector<ASTNodePtr> a;
        a.push_back(alloc());
        if (c->isUnion && !fields.empty())
        {
            // a union: the one member given (the first by default)
            size_t g = 0;
            for (size_t j = 0; j < given.size(); ++j)
                if (given[j])
                {
                    g = j;
                    break;
                }
            if (given[g])
            {
                XV v = lowerExprTo(given[g], fields[g]->type);
                v = convert(std::move(v), fields[g]->type, line);
                a.push_back(copyOf(std::move(v), line));
            }
            else
                a.push_back(defaultValue(fields[0]->type, line));
            return q::call(c->aggFn(), std::move(a), line);
        }
        size_t i = 0;
        for (const FieldInfo *fp : fields)
        {
            const FieldInfo &f = *fp;
            if (given[i])
            {
                XV v = lowerExprTo(given[i], f.type);
                v = convert(std::move(v), f.type, line);
                a.push_back(copyOf(std::move(v), line));
            }
            else if (f.hasInit)
            {
                std::vector<ExprP> fa = f.init.args;
                a.push_back(copyOf(makeObjectFrom(f.type, fa, f.init.init == VarDeclarator::Brace, line), line));
            }
            else
                a.push_back(defaultValue(f.type, line));
            ++i;
        }
        return q::call(c->aggFn(), std::move(a), line);
    }
    if (cands.empty())
    {
        // parenthesized aggregate init (C++20)
        std::vector<ExprP> a2 = args;
        return ctorCall(c, a2, true, line, inPlace ? alloc() : nullptr); // braced: handled above
    }
    std::vector<XV> vals;
    for (auto &a : args)
    {
        XV x;
        x.t = probeType(a);
        vals.push_back(std::move(x));
    }
    FuncInfo *f = pickOverload(cands, vals, line, false);
    if (!f)
        unsupported("no constructor of " + c->qname + " takes these arguments", line);
    ASTNodePtr self = alloc();
    if (f->cls != c)
    {
        // inherited constructor: default-initialize, then run the base's
        std::vector<ASTNodePtr> a;
        a.push_back(std::move(self));
        self = q::call(c->defaultCtorFn(), std::move(a), line);
    }
    XV r = callArgs(f, args, std::move(self), line, false);
    return read(r);
}

XV Lowerer::lowerConstruct(const ExprP &e)
{
    int ln = e->line;
    TP t = resolveType(e->type, currentScope());
    std::vector<ExprP> args = e->kids;
    if (t->k == T::Class && t->cls)
    {
        if (e->braced)
        {
            XV r = listInit(t, args, ln);
            r.v = trackTemp(read(r), t, ln);
            return r;
        }
        return mkxv(trackTemp(ctorCall(t->cls, args, false, ln), t, ln), t);
    }
    if (t->k == T::Dyn && (t->name == "less" || t->name == "greater" || t->name == "hash"))
        return mkxv(q::str(t->name, ln), t); // std::greater<int>(): a comparator tag
    if (isArith(t) || t->k == T::Bool)
    {
        if (args.empty())
            return mkxv(defaultValue(t, ln), t);
        XV x = lowerExpr(args[0]);
        if (t->k == T::Char && isIntegral(x.t))
        {
            x.t = t;
            return x;
        }
        XV r = convert(std::move(x), t, ln);
        r.t = t;
        r.temp = true;
        return r;
    }
    return constructBuiltinType(t, args, e->braced, ln);
}

XV Lowerer::lowerNew(const ExprP &e)
{
    int ln = e->line;
    TP t = resolveType(e->type, currentScope());
    auto pt = mkT(T::Ptr);
    pt->a.push_back(t);
    if (e->newArray)
    {
        XV n = lowerExpr(e->kids[0]);
        std::vector<ASTNodePtr> a;
        a.push_back(read(n));
        a.push_back(defaultValue(t, ln));
        a.push_back(planFor(t));
        ASTNodePtr arr = q::call("__cpp_fill__", std::move(a), ln);
        if (e->kids.size() > 1)
        {
            // new T[n]{a, b, c}
            std::vector<ASTNodePtr> init;
            for (size_t i = 1; i < e->kids.size(); ++i)
            {
                XV v = lowerExprTo(e->kids[i], t);
                v = convert(std::move(v), t, ln);
                init.push_back(read(v));
            }
            std::vector<ASTNodePtr> b;
            b.push_back(std::move(arr));
            b.push_back(q::arr(std::move(init), ln));
            arr = q::call("__cpp_fill_prefix__", std::move(b), ln);
        }
        std::vector<ASTNodePtr> r;
        r.push_back(std::move(arr));
        r.push_back(q::num(0, ln));
        return mkxv(q::arr(std::move(r), ln), pt);
    }
    std::vector<ExprP> args = e->kids;
    if (t->k == T::Class && t->cls)
    {
        if (e->braced)
        {
            XV x = listInit(t, args, ln);
            x.t = pt;
            return x;
        }
        return mkxv(ctorCall(t->cls, args, false, ln), pt);
    }
    if (isObjectRep(t))
    {
        XV x = constructBuiltinType(t, args, e->braced, ln);
        x.t = pt;
        return x;
    }
    // new int(5): a cell
    ASTNodePtr v;
    if (args.empty())
        v = defaultValue(t, ln);
    else
    {
        XV x = lowerExprTo(args[0], t);
        x = convert(std::move(x), t, ln);
        v = read(x);
    }
    std::vector<ASTNodePtr> cell;
    cell.push_back(std::move(v));
    std::vector<ASTNodePtr> r;
    r.push_back(q::arr(std::move(cell), ln));
    r.push_back(q::num(0, ln));
    return mkxv(q::arr(std::move(r), ln), pt);
}

// ─── Initialization ────────────────────────────────────────────────────────

XV Lowerer::lowerExprTo(const ExprP &e, const TP &target)
{
    if (e->k == EK::Lambda && target && target->k == T::Func && target->a.size() > 1)
        lambdaHints_.assign(target->a.begin() + 1, target->a.end());
    if (e->k == EK::InitList && target && target->k != T::Dyn)
    {
        std::vector<ExprP> kids = e->kids;
        return listInit(target, kids, e->line);
    }
    return lowerExpr(e);
}

// The static type of `e`, lowering it into a scratch buffer that is discarded.
TP Lowerer::probeType(const ExprP &e)
{
    if (e->k == EK::InitList)
        return mkT(T::InitList);
    auto savedPre = pre_;
    std::vector<ASTNodePtr> scratch;
    pre_ = &scratch;
    bool savedHoist = canHoist_;
    canHoist_ = true;
    auto savedHints = lambdaHints_;
    TP t;
    try
    {
        XV x = lowerExpr(e);
        t = x.isFuncName ? mkT(T::Func) : x.t;
    }
    catch (Unsupported &)
    {
        pre_ = savedPre;
        canHoist_ = savedHoist;
        throw;
    }
    pre_ = savedPre;
    canHoist_ = savedHoist;
    lambdaHints_ = savedHints;
    return t ? t : tDyn();
}

XV Lowerer::makeObjectFrom(const TP &t, std::vector<ExprP> &args, bool braced, int line)
{
    if (braced)
    {
        if (t->k == T::Dyn)
        {
            if (args.size() == 1)
                return lowerExpr(args[0]);
            std::vector<ASTNodePtr> el;
            for (auto &a : args)
            {
                XV x = lowerExpr(a);
                el.push_back(read(x));
            }
            auto vt = mkT(T::Vector, "vector");
            vt->a.push_back(tDyn());
            return mkxv(q::arr(std::move(el), line), vt);
        }
        return listInit(t, args, line);
    }
    if (args.size() == 1)
    {
        if (t->k == T::Class && t->cls)
        {
            // T x(arg): a constructor call, or a copy when arg is already a T
            if (args[0]->k == EK::InitList)
                return mkxv(ctorCall(t->cls, args, false, line), t);
            TP at = probeType(args[0]);
            if (at->k != T::Class && at->k != T::Dyn)
                return mkxv(ctorCall(t->cls, args, false, line), t);
        }
        XV x = lowerExprTo(args[0], t);
        if (t->k == T::Dyn)
            return x;
        if (isObjectRep(t) && t->k != T::Class && !sameType(x.t, t) && x.t->k != T::Dyn &&
            !(t->k == x.t->k && t->k != T::Array))
        {
            // vector<int> v(5): a constructor, not a conversion
            std::vector<ExprP> a2 = args;
            return constructBuiltinType(t, a2, false, line);
        }
        if (t->k == T::Str && x.t->k == T::Int)
            return constructBuiltinType(t, args, false, line);
        return convert(std::move(x), t, line);
    }
    if (args.empty())
        return mkxv(defaultValue(t, line), t);
    if (t->k == T::Class && t->cls)
        return mkxv(ctorCall(t->cls, args, false, line), t);
    return constructBuiltinType(t, args, false, line);
}

XV Lowerer::listInit(const TP &t, std::vector<ExprP> &kids, int line)
{
    switch (t->k)
    {
    case T::Class:
    {
        ClassInfo *c = t->cls;
        if (kids.size() == 1 && kids[0]->k != EK::InitList)
        {
            XV x = lowerExpr(kids[0]);
            if (x.t->k == T::Class && convCost(x.t, t) <= 2)
                return x;
            std::vector<ExprP> a = kids;
            return mkxv(ctorCall(c, a, true, line), t);
        }
        std::vector<ExprP> a = kids;
        return mkxv(ctorCall(c, a, true, line), t);
    }
    case T::Vector:
    case T::Array:
    {
        TP et = t->a.empty() ? tDyn() : t->a[0];
        // Brace elision: `int m[2][3] = {1, 2, 3, 4}` fills the rows in order,
        // as `{{1, 2, 3}, {4}}` would — a scalar where a row is expected starts
        // an unbraced row that takes as many scalars as the row holds.
        std::vector<ExprP> elided;
        if (t->k == T::Array && et->k == T::Array)
        {
            long perRow = 1;
            for (TP r = et; r->k == T::Array && !r->a.empty(); r = r->a[0])
                perRow *= std::max(r->n, 1L);
            std::vector<bool> scalar;
            for (auto &k : kids)
                scalar.push_back(k->k != EK::InitList && isArith(probeType(k)));
            for (size_t i = 0; i < kids.size();)
            {
                if (!scalar[i])
                {
                    elided.push_back(kids[i++]);
                    continue;
                }
                auto row = std::make_shared<Expr>();
                row->k = EK::InitList;
                row->line = kids[i]->line;
                while (i < kids.size() && scalar[i] && (long)row->kids.size() < perRow)
                    row->kids.push_back(kids[i++]);
                elided.push_back(row);
            }
        }
        std::vector<ExprP> &items = elided.empty() ? kids : elided;
        bool designated = false;
        for (auto &k : items)
            designated = designated || k->designIndex;
        if (designated && t->k == T::Array)
        {
            std::map<long, ASTNodePtr> placed;
            long pos = 0, end = 0;
            for (auto &k : items)
            {
                if (k->designIndex)
                    pos = (long)evalConst(k->designIndex, currentScope());
                XV x = lowerExprTo(k, et);
                x = convert(std::move(x), et, line);
                placed[pos] = copyOf(std::move(x), line);
                end = std::max(end, ++pos);
            }
            std::vector<ASTNodePtr> el;
            long n = t->n >= 0 ? t->n : end;
            for (long i = 0; i < n; ++i)
                el.push_back(placed.count(i) ? std::move(placed[i]) : defaultValue(et, line));
            return mkxv(q::arr(std::move(el), line), t);
        }
        std::vector<ASTNodePtr> el;
        for (auto &k : items)
        {
            XV x = lowerExprTo(k, et);
            if (et->k == T::Dyn && x.t->k != T::Dyn && t->a.empty())
                et = x.t;
            x = convert(std::move(x), et, line);
            el.push_back(copyOf(std::move(x), line));
        }
        if (t->k == T::Array && t->n > (long)items.size())
        {
            // remaining elements value-initialized
            std::vector<ASTNodePtr> a;
            a.push_back(q::arr(std::move(el), line));
            a.push_back(q::num((double)t->n, line));
            a.push_back(defaultValue(et, line));
            a.push_back(planFor(et));
            return mkxv(q::call("__cpp_fill_to__", std::move(a), line), t);
        }
        return mkxv(q::arr(std::move(el), line), t);
    }
    case T::Map:
    case T::Set:
    {
        std::vector<ASTNodePtr> el;
        for (auto &k : kids)
        {
            if (t->k == T::Map)
            {
                auto pt = mkT(T::Pair);
                pt->a = t->a;
                XV x = lowerExprTo(k, pt);
                x = convert(std::move(x), pt, line);
                el.push_back(copyOf(std::move(x), line));
            }
            else
            {
                XV x = lowerExprTo(k, t->a[0]);
                x = convert(std::move(x), t->a[0], line);
                el.push_back(copyOf(std::move(x), line));
            }
        }
        std::vector<ASTNodePtr> a;
        a.push_back(q::arr(std::move(el), line));
        a.push_back(q::boolean(t->multi, line));
        return mkxv(q::call(t->k == T::Map ? "__cpp_map_from__" : "__cpp_set_from__", std::move(a), line), t);
    }
    case T::Pair:
    case T::Tuple:
    {
        if (kids.size() == 1 && kids[0]->k != EK::InitList)
        {
            XV x = lowerExpr(kids[0]);
            if (x.t->k == t->k)
                return convert(std::move(x), t, line);
        }
        std::vector<ASTNodePtr> el;
        for (size_t i = 0; i < kids.size(); ++i)
        {
            TP et = i < t->a.size() ? t->a[i] : tDyn();
            XV x = lowerExprTo(kids[i], et);
            x = convert(std::move(x), et, line);
            el.push_back(copyOf(std::move(x), line));
        }
        return mkxv(q::arr(std::move(el), line), t);
    }
    case T::Str:
        if (kids.empty())
            return mkxv(q::str("", line), t);
        if (kids.size() == 1)
        {
            XV x = lowerExpr(kids[0]);
            if (isCBuf(t) && isIntegral(x.t))
                return mkxv(chr(read(x), line), t); // char s[8] = {0}
            return convert(std::move(x), t, line);
        }
        {
            // {'a', 'b'}: characters
            ASTNodePtr s = q::str("", line);
            for (auto &k : kids)
            {
                XV x = lowerExpr(k);
                s = q::bin("+", std::move(s), x.t->k == T::Char ? chr(read(x), line) : read(x), line);
            }
            return mkxv(std::move(s), t);
        }
    case T::Optional:
        if (kids.empty())
            return mkxv(q::nil(line), t);
        return convert(lowerExprTo(kids[0], t->a[0]), t, line);
    case T::Variant:
        if (kids.empty())
            return mkxv(defaultValue(t, line), t);
        return convert(lowerExpr(kids[0]), t, line);
    default:
        if (kids.empty())
            return mkxv(defaultValue(t, line), t);
        if (kids.size() == 1)
            return convert(lowerExprTo(kids[0], t), t, line);
        unsupported("brace initializer for " + t->str(), line);
    }
}

// ─── Calls ─────────────────────────────────────────────────────────────────

FuncInfo *Lowerer::pickOverload(const std::vector<FuncInfo *> &cands, std::vector<XV> &args, int line, bool hasSelf)
{
    (void)hasSelf;
    FuncInfo *best = nullptr;
    int bestCost = 1 << 20;
    for (FuncInfo *f : cands)
    {
        if (f->decl && f->decl->isDeleted)
            continue;
        size_t required = 0;
        for (size_t i = 0; i < f->params.size(); ++i)
            if (!(i < f->defaults.size() && f->defaults[i]))
                required = i + 1;
        if (args.size() < required || (args.size() > f->params.size() && !f->variadic))
            continue;
        int cost = 0;
        for (size_t i = 0; i < args.size() && i < f->params.size(); ++i)
        {
            int c = convCost(args[i].t, f->params[i], &args[i]);
            if (f->paramRef[i] && !isObjectRep(f->params[i]) && args[i].lk == XV::RV && args[i].t->k != T::Dyn &&
                !(f->decl && f->decl->params.size() > i && f->decl->params[i].type->rref))
                c += 9; // a non-const reference needs an lvalue
            cost += c >= 9 ? 1000 : c;
        }
        if (cost < bestCost)
        {
            bestCost = cost;
            best = f;
        }
    }
    if (best && bestCost >= 1000)
    {
        if (cands.size() == 1)
            return best;
        // nothing viable by our (approximate) rules: the closest by arity
        return best;
    }
    (void)line;
    return best;
}

// ─── Function templates ────────────────────────────────────────────────────
// A call of a function template is a call of its instance for the template
// arguments given or deduced from the arguments' types, lowered with them
// bound; a template whose arguments can't all be found stays dynamic.

void Lowerer::deduce(const TypeNameP &p, const TP &a, const std::vector<std::string> &tps, std::map<std::string, TP> &bind)
{
    if (!p || !a || a->k == T::Dyn || a->k == T::InitList)
        return;
    if (p->name.size() == 1 && std::find(tps.begin(), tps.end(), p->name[0]) != tps.end() &&
        (p->targs.empty() || p->targs.back().empty()))
    {
        // T, const T&, T&&, T*
        TP v = a;
        if (p->pointers > 0)
        {
            if (a->k == T::Ptr && !a->a.empty())
                v = a->a[0];
            else if (a->k == T::CStr && p->pointers == 1)
                v = tChar();
            else
                return;
        }
        if (isCBuf(v))
            v = tCStr(); // an array argument decays
        if (!bind.count(p->name[0]))
            bind[p->name[0]] = v;
        return;
    }
    // vector<T>, map<K, V>, pair<A, B>, ...: the argument's type arguments
    if (!p->targs.empty() && !p->targs.back().empty())
    {
        const auto &ta = p->targs.back();
        for (size_t i = 0; i < ta.size() && i < a->a.size(); ++i)
            if (ta[i].type)
                deduce(ta[i].type, a->a[i], tps, bind);
    }
}

FuncInfo *Lowerer::instantiate(FuncInfo *f, const std::vector<XV> &vals, const std::vector<TArg> &targs, int line)
{
    const DeclP &d = f->decl;
    if (!d || d->templateParams.empty() || f->cls || !f->def)
        return f;
    const auto &tps = d->templateParams;
    std::map<std::string, TP> bind;
    for (size_t i = 0; i < targs.size() && i < tps.size(); ++i)
        if (targs[i].type)
            bind[tps[i]] = resolveType(targs[i].type, currentScope());
    for (size_t i = 0; i < vals.size() && i < d->params.size(); ++i)
        if (vals[i].t)
            deduce(d->params[i].type, vals[i].t, tps, bind);
    std::string key = f->mangled + "<";
    for (size_t i = 0; i < tps.size(); ++i)
    {
        auto it = bind.find(tps[i]);
        if (it == bind.end() || !it->second || it->second->k == T::Dyn)
            return f; // not all known: the dynamic version
        key += (i ? "," : "") + it->second->str();
    }
    key += ">";
    auto known = instances_.find(key);
    if (known != instances_.end())
        return known->second;
    funcs_.push_back(std::make_unique<FuncInfo>(*f));
    FuncInfo *g = funcs_.back().get();
    g->isTemplate = false;
    g->lowered = false;
    g->mangled = key;
    g->scope = newScope(f->scope ? f->scope->qname : "", f->scope);
    for (const auto &b : bind)
    {
        Symbol s;
        s.kind = Symbol::Type;
        s.type = b.second;
        g->scope->syms[b.first] = s;
    }
    instances_[key] = g;
    int savedLine = line_;
    resolveFunc(g);
    line_ = savedLine;
    allFuncs_.push_back(g);
    (void)line;
    return g;
}

static bool hasSideEffects(const ExprP &e)
{
    if (!e)
        return false;
    switch (e->k)
    {
    case EK::Call: case EK::Assign: case EK::Postfix: case EK::New: case EK::Delete: case EK::Throw:
        return true;
    case EK::Unary:
        if (e->op == "++" || e->op == "--")
            return true;
        break;
    case EK::Lambda: case EK::SizeofT: case EK::SizeofE:
        return false;
    default:
        break;
    }
    for (auto &k : e->kids)
        if (hasSideEffects(k))
            return true;
    return false;
}

void Lowerer::preEvalArgs(const std::vector<ExprP> &args, std::vector<XV> &out, size_t from,
                          const std::function<TP(size_t)> &target)
{
    out.clear();
    out.resize(args.size());
    if (!canHoist_ || !pre_)
        return;
    int n = 0;
    for (size_t i = from; i < args.size(); ++i)
        n += hasSideEffects(args[i]) ? 1 : 0;
    if (n < 2)
        return;
    for (size_t i = args.size(); i-- > from;)
    {
        if (!hasSideEffects(args[i]))
            continue;
        TP t = target ? target(i) : nullptr;
        XV x = t ? lowerExprTo(args[i], t) : lowerExpr(args[i]);
        if (x.isFuncName || x.alloc)
            continue; // no value yet: lowered again in order
        if (x.lk == XV::RV && x.v)
            x.v = simple(std::move(x.v), args[i]->line);
        else if (x.lk == XV::Idx)
        {
            x.obj = simple(std::move(x.obj), args[i]->line);
            x.key = simple(std::move(x.key), args[i]->line);
        }
        else if (x.lk != XV::Var)
            continue;
        out[i] = std::move(x);
    }
}

XV Lowerer::callArgs(FuncInfo *f, std::vector<ExprP> &args, ASTNodePtr self, int line, bool virt,
                     std::vector<XV> *lowered)
{
    std::vector<XV> early;
    if (!lowered)
    {
        preEvalArgs(args, early, 0, [&](size_t i) -> TP
                    { return i < f->params.size() && !(f->paramRef[i] && !isObjectRep(f->params[i]) && f->params[i]->k != T::Dyn)
                                 ? f->params[i]
                                 : nullptr; });
        lowered = &early;
    }
    // arguments already lowered by the caller are used as they are
    auto argXV = [&](size_t i, const TP &target) -> XV
    {
        if (lowered && i < lowered->size() && (*lowered)[i].t && ((*lowered)[i].v || (*lowered)[i].lk != XV::RV))
            return std::move((*lowered)[i]);
        return target ? lowerExprTo(args[i], target) : lowerExpr(args[i]);
    };
    std::vector<ASTNodePtr> a;
    ASTNodePtr selfTmp;
    if (self)
    {
        if (virt)
        {
            self = simple(std::move(self), line);
            selfTmp = q::clone(self);
        }
        a.push_back(std::move(self));
    }
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (i >= f->params.size())
        {
            XV x = argXV(i, nullptr);
            a.push_back(read(x));
            continue;
        }
        TP pt = f->params[i];
        if (f->paramRef[i] && !isObjectRep(pt) && pt->k != T::Dyn)
        {
            XV x = argXV(i, nullptr);
            if (x.lk == XV::RV)
            {
                // a temporary bound to T&& / const-less ref: a fresh cell
                XV c = convert(std::move(x), pt, line);
                XV r = addressOf(std::move(c), line);
                a.push_back(read(r));
            }
            else
            {
                XV r = addressOf(std::move(x), line);
                a.push_back(read(r));
            }
            continue;
        }
        XV x = argXV(i, pt);
        x = convert(std::move(x), pt, line);
        bool byValue = !(i < f->paramConstRef.size() && f->paramConstRef[i]) && !f->paramRef[i];
        if (byValue)
            untrackTemp(x);
        a.push_back(byValue ? copyOf(std::move(x), line) : read(x));
    }
    // default arguments, read in the function's own scope
    for (size_t i = args.size(); i < f->params.size(); ++i)
    {
        if (!(i < f->defaults.size() && f->defaults[i]))
        {
            a.push_back(defaultValue(f->params[i], line));
            continue;
        }
        Scope *saved = fn_ ? fn_->scope : nullptr;
        if (fn_)
            fn_->scope = f->scope ? f->scope : saved;
        XV x = lowerExprTo(f->defaults[i], f->params[i]);
        x = convert(std::move(x), f->params[i], line);
        if (fn_)
            fn_->scope = saved;
        a.push_back(copyOf(std::move(x), line));
    }
    ASTNodePtr callee;
    if (virt && selfTmp && !f->vkey.empty())
        callee = q::index(q::index(q::index(std::move(selfTmp), q::num(0, line), line), q::str("vt", line), line),
                          q::str(f->vkey, line), line);
    else
    {
        if (f->mangled.empty())
            unsupported("call of " + f->name + " before its declaration was resolved", line);
        if (!f->def && !(f->decl && (f->decl->isDefault || f->decl->isPure)))
            calledUndefined_.insert(f);
        callee = q::id(f->mangled, line);
    }
    ASTNodePtr c = q::call(std::move(callee), std::move(a), line);
    if (f->retRef && !isObjectRep(f->ret))
    {
        ASTNodePtr r = simple(std::move(c), line);
        XV x;
        x.t = f->ret;
        x.lk = XV::Idx;
        x.obj = q::index(q::clone(r), q::num(0, line), line);
        x.key = q::index(std::move(r), q::num(1, line), line);
        return x;
    }
    bool returnsRef = f->def && f->def->ret && (f->def->ret->ref || f->def->ret->rref);
    if (!f->def && f->decl && f->decl->ret)
        returnsRef = f->decl->ret->ref || f->decl->ret->rref;
    if (!returnsRef && !f->isCtor)
        c = trackTemp(std::move(c), f->ret, line); // a class returned by value: a temporary
    return mkxv(std::move(c), f->ret, !returnsRef);
}

ASTNodePtr Lowerer::callFn(FuncInfo *f, std::vector<ExprP> &args, ASTNodePtr self, int line, bool virt)
{
    XV r = callArgs(f, args, std::move(self), line, virt);
    return read(r);
}

XV Lowerer::callValue(XV fv, std::vector<ExprP> &args, int line)
{
    TP ft = fv.t;
    std::vector<ASTNodePtr> a;
    for (size_t i = 0; i < args.size(); ++i)
    {
        TP pt = ft->k == T::Func && i + 1 < ft->a.size() ? ft->a[i + 1] : tDyn();
        XV x = lowerExprTo(args[i], pt);
        x = convert(std::move(x), pt, line);
        a.push_back(read(x));
    }
    TP rt = ft->k == T::Func && !ft->a.empty() ? ft->a[0] : tDyn();
    return mkxv(q::call(read(fv), std::move(a), line), rt);
}

XV Lowerer::dynCall(ASTNodePtr callee, std::vector<ExprP> &args, int line)
{
    std::vector<ASTNodePtr> a;
    for (auto &arg : args)
    {
        XV x = lowerExpr(arg);
        a.push_back(read(x));
    }
    return mkxv(q::call(std::move(callee), std::move(a), line), tDyn());
}

XV Lowerer::lowerCall(const ExprP &e)
{
    int ln = e->line;
    const ExprP &callee = e->kids[0];
    std::vector<ExprP> args(e->kids.begin() + 1, e->kids.end());
    if (callee->k == EK::Name)
    {
        const auto &name = callee->name;
        bool stdQual = name.size() >= 2 && name[0] == "std";
        if (!stdQual)
        {
            if (name.size() == 1)
                if (Local *l = findLocal(name[0]))
                {
                    XV fv = localXV(l);
                    if (fv.t->k == T::Class && fv.t->cls)
                    {
                        // functor object
                        std::vector<FuncInfo *> ms;
                        findMethod(fv.t->cls, "operator()", ms);
                        if (ms.empty())
                            unsupported("calling an object without operator()", ln);
                        std::vector<XV> vals;
                        for (auto &a : args)
                            vals.push_back(a->k == EK::InitList ? XV() : lowerExpr(a));
                        for (auto &v : vals)
                            if (!v.t)
                                v.t = mkT(T::InitList);
                        FuncInfo *f = pickOverload(ms, vals, ln, true);
                        return callArgs(f, args, read(fv), ln, false);
                    }
                    return callValue(std::move(fv), args, ln);
                }
            XV fn = [&]() -> XV
            {
                try
                {
                    return lowerName(callee);
                }
                catch (Unsupported &)
                {
                    XV none;
                    none.t = nullptr;
                    return none;
                }
            }();
            if (fn.t && !fn.isFuncName)
            {
                if (fn.t->k == T::Class && fn.t->cls)
                {
                    std::vector<FuncInfo *> ms;
                    findMethod(fn.t->cls, "operator()", ms);
                    if (!ms.empty())
                    {
                        std::vector<XV> vals;
                        for (auto &a : args)
                        {
                            if (a->k == EK::InitList)
                            {
                                XV x;
                                x.t = mkT(T::InitList);
                                vals.push_back(std::move(x));
                            }
                            else
                                vals.push_back(lowerExpr(a));
                        }
                        FuncInfo *f = pickOverload(ms, vals, ln, true);
                        return callArgs(f, args, read(fn), ln, false);
                    }
                }
                return callValue(std::move(fn), args, ln);
            }
            if (fn.isFuncName)
            {
                std::vector<XV> vals;
                for (auto &a : args)
                {
                    if (a->k == EK::InitList || a->k == EK::Lambda)
                    {
                        XV x;
                        x.t = a->k == EK::Lambda ? mkT(T::Func) : mkT(T::InitList);
                        vals.push_back(std::move(x));
                    }
                    else
                    {
                        // a lowered argument is discarded: lowering again is side-effect free
                        auto savedPre = pre_;
                        std::vector<ASTNodePtr> scratch;
                        pre_ = &scratch;
                        bool savedHoist = canHoist_;
                        canHoist_ = true;
                        int savedTemp = tempCounter_;
                        XV x = lowerExpr(a);
                        XV probe;
                        probe.t = x.t;
                        probe.lk = x.lk;
                        probe.isFuncName = x.isFuncName;
                        if (x.v && std::holds_alternative<NumberLiteral>(x.v->node))
                            probe.v = q::clone(x.v);
                        vals.push_back(std::move(probe));
                        pre_ = savedPre;
                        canHoist_ = savedHoist;
                        tempCounter_ = savedTemp + 0;
                        tempCounter_ = std::max(tempCounter_, savedTemp);
                    }
                }
                FuncInfo *f = pickOverload(fn.overloads, vals, ln, false);
                if (!f)
                    unsupported("no matching function " + name.back(), ln);
                if (f->isTemplate)
                    f = instantiate(f, vals, callee->targs, ln);
                if (f->cls && !f->isStatic)
                {
                    if (!fn_ || !fn_->hasThis)
                        unsupported("calling member " + f->name + " without an object", ln);
                    bool qualified = name.size() > 1;
                    return callArgs(f, args, q::id("__this", ln), ln, f->isVirtual && !qualified);
                }
                return callArgs(f, args, nullptr, ln, false);
            }
        }
        bool handled = false;
        std::vector<TArg> targs = callee->targs;
        std::string fname;
        for (size_t i = stdQual ? 1 : 0; i < name.size(); ++i)
            fname += (fname.empty() ? "" : "::") + name[i];
        XV r = libCall(fname, e, args, targs, handled);
        if (handled)
            return r;
        unsupported("unknown function '" + fname + "'", ln);
    }
    if (callee->k == EK::Member)
    {
        XV obj = lowerExpr(callee->kids[0]);
        if (callee->arrow)
        {
            if (obj.t->k == T::Class && obj.t->cls)
            {
                std::vector<FuncInfo *> ms;
                findMethod(obj.t->cls, "operator->", ms);
                if (!ms.empty())
                {
                    std::vector<ExprP> none;
                    XV r = callArgs(ms[0], none, read(obj), ln, false);
                    obj = derefXV(std::move(r), ln);
                }
            }
            else
                obj = derefXV(std::move(obj), ln);
        }
        const std::string &m = callee->op;
        if (obj.t->k == T::Class && obj.t->cls)
        {
            ClassInfo *c = obj.t->cls;
            std::vector<FuncInfo *> ms;
            findMethod(c, m, ms);
            if (ms.empty() && c->builtin && m == "what")
            {
                std::vector<ASTNodePtr> a;
                a.push_back(read(obj));
                return mkxv(q::call("__cpp_what__", std::move(a), ln), tCStr());
            }
            if (ms.empty())
            {
                // a field holding a callable
                bool found = false;
                XV f = fieldOf(std::move(obj), c, m, ln, found);
                if (found)
                    return callValue(std::move(f), args, ln);
                if (m == "what")
                {
                    std::vector<ASTNodePtr> a;
                    a.push_back(read(f));
                    return mkxv(q::call("__cpp_what__", std::move(a), ln), tCStr());
                }
                unsupported("no method " + m + " in " + c->qname, ln);
            }
            std::vector<XV> vals;
            for (auto &a : args)
            {
                if (a->k == EK::InitList || a->k == EK::Lambda)
                {
                    XV x;
                    x.t = a->k == EK::Lambda ? mkT(T::Func) : mkT(T::InitList);
                    vals.push_back(std::move(x));
                    continue;
                }
                auto savedPre = pre_;
                std::vector<ASTNodePtr> scratch;
                pre_ = &scratch;
                bool savedHoist = canHoist_;
                canHoist_ = true;
                XV x = lowerExpr(a);
                XV probe;
                probe.t = x.t;
                probe.lk = x.lk;
                if (x.v && std::holds_alternative<NumberLiteral>(x.v->node))
                    probe.v = q::clone(x.v);
                vals.push_back(std::move(probe));
                pre_ = savedPre;
                canHoist_ = savedHoist;
            }
            FuncInfo *f = pickOverload(ms, vals, ln, true);
            if (!f)
                unsupported("no matching method " + m, ln);
            if (f->cls)
                checkAccess(f->cls, f->access, f->cls->name + "::" + m, ln);
            if (f->isStatic)
                return callArgs(f, args, nullptr, ln, false);
            bool qualified = false;
            return callArgs(f, args, read(obj), ln, f->isVirtual && !qualified);
        }
        if (obj.t->k == T::Dyn)
        {
            ASTNodePtr callee2 = std::make_unique<ASTNode>(MemberExpr{read(obj), m}, ln);
            return dynCall(std::move(callee2), args, ln);
        }
        bool handled = false;
        XV r = libMethod(obj, m, e, args, handled);
        if (handled)
            return r;
        unsupported("unknown method " + m + " of " + obj.t->str(), ln);
    }
    // any other callable expression
    XV fv = lowerExpr(callee);
    if (fv.t->k == T::Class && fv.t->cls)
    {
        std::vector<FuncInfo *> ms;
        findMethod(fv.t->cls, "operator()", ms);
        if (!ms.empty())
        {
            std::vector<XV> vals;
            for (auto &a : args)
                vals.push_back(lowerExpr(a));
            FuncInfo *f = pickOverload(ms, vals, ln, true);
            return callArgs(f, args, read(fv), ln, false);
        }
    }
    return callValue(std::move(fv), args, ln);
}

// ─── Dispatch ──────────────────────────────────────────────────────────────

XV Lowerer::lowerExpr(const ExprP &e)
{
    int ln = e->line;
    line_ = ln;
    switch (e->k)
    {
    case EK::Int:
    {
        TP t = tInt();
        if (e->isUnsigned)
            t = e->isLong || e->num > 4294967295.0 ? tSize() : [&]
            {
                auto u = mkT(T::Int, "unsigned int");
                u->isUnsigned = true;
                return u;
            }();
        else if (e->isLong || e->num > 2147483647.0)
            t = tLL();
        uint64_t exact = 0;
        if (isI64(t) && literalValue(e->str, exact))
            return mkxv(i64Const((int64_t)exact, ln), t);
        return mkxv(q::num(e->num, ln), t);
    }
    case EK::Float:
        return mkxv(q::num(e->num, ln), e->isFloatSuffix ? mkT(T::Float, "float") : tDouble());
    case EK::Char:
        return mkxv(q::num(e->num, ln), tChar());
    case EK::Str:
        return mkxv(q::str(e->str, ln), tCStr());
    case EK::Bool:
        return mkxv(q::boolean(e->num != 0, ln), tBool());
    case EK::Nullptr:
        return mkxv(q::nil(ln), mkT(T::Nullptr));
    case EK::This:
    {
        if (!fn_ || !fn_->hasThis)
            unsupported("this outside a member function", ln);
        auto pt = mkT(T::Ptr);
        auto ct = mkT(T::Class);
        ct->cls = fn_->cls;
        pt->a.push_back(ct);
        return mkxv(q::id("__this", ln), pt, false);
    }
    case EK::Name:
        return lowerName(e);
    case EK::Unary:
        return lowerUnary(e);
    case EK::Postfix:
    {
        XV x = lowerExpr(e->kids[0]);
        if (x.constLv)
            unsupported(std::string(e->op == "++" ? "increment" : "decrement") + " of read-only " + x.constName, ln);
        if (x.t->k == T::Class)
        {
            std::vector<ExprP> ops = {e->kids[0]};
            std::vector<XV> vals;
            vals.push_back(std::move(x));
            bool h = false;
            XV r = userOperator(e->op, ops, vals, ln, h);
            if (h)
                return r;
            unsupported("no postfix operator" + e->op, ln);
        }
        return incdec(std::move(x), e->op == "++", true, ln);
    }
    case EK::Binary:
        return lowerBinary(e);
    case EK::Assign:
        return lowerAssign(e);
    case EK::Cond:
    {
        XV c = lowerExpr(e->kids[0]);
        ASTNodePtr cond = toBool(std::move(c), ln);
        bool saved = canHoist_;
        canHoist_ = false;
        XV a = lowerExpr(e->kids[1]);
        XV b = lowerExpr(e->kids[2]);
        canHoist_ = saved;
        TP t = commonType(a.t, b.t);
        if (e->kids[1]->k == EK::Throw)
            t = b.t;
        if (e->kids[2]->k == EK::Throw)
            t = a.t;
        bool lv = a.lk != XV::RV && b.lk != XV::RV;
        a = convert(std::move(a), t, ln);
        b = convert(std::move(b), t, ln);
        bool temp = a.temp && b.temp;
        XV r = mkxv(q::ternary(std::move(cond), read(a), read(b), ln), t, temp);
        (void)lv;
        return r;
    }
    case EK::Call:
        return lowerCall(e);
    case EK::Member:
        return lowerMember(e);
    case EK::Index:
        return lowerIndex(e);
    case EK::Cast:
        return lowerCast(e);
    case EK::SizeofT:
        return mkxv(q::num((double)sizeOfType(resolveType(e->type, currentScope())), ln), tSize());
    case EK::SizeofE:
    {
        XV x = lowerExpr(e->kids[0]);
        if (x.t->k == T::CStr && x.v && std::holds_alternative<StringLiteral>(x.v->node))
            return mkxv(q::num((double)std::get<StringLiteral>(x.v->node).value.size() + 1, ln), tSize());
        return mkxv(q::num((double)sizeOfType(x.t), ln), tSize());
    }
    case EK::New:
        return lowerNew(e);
    case EK::Delete:
    {
        // the object's destructor (its dynamic class's), then it is gone
        XV x = lowerExpr(e->kids[0]);
        std::vector<ASTNodePtr> a;
        a.push_back(read(x));
        if (e->newArray)
        {
            a.push_back(q::boolean(true, ln));
            return mkxv(q::call("__cpp_destroy_each__", std::move(a), ln), tVoid());
        }
        return mkxv(q::call("__cpp_delete__", std::move(a), ln), tVoid());
    }
    case EK::Lambda:
        return lowerLambda(e);
    case EK::InitList:
    {
        auto vt = mkT(T::Vector, "vector");
        std::vector<ASTNodePtr> el;
        TP et = tDyn();
        for (auto &k : e->kids)
        {
            XV x = lowerExpr(k);
            if (et->k == T::Dyn)
                et = x.t;
            el.push_back(read(x));
        }
        vt->a.push_back(et);
        auto r = mkxv(q::arr(std::move(el), ln), vt);
        return r;
    }
    case EK::Construct:
        return lowerConstruct(e);
    case EK::Comma:
    {
        XV a = lowerExpr(e->kids[0]);
        XV b = lowerExpr(e->kids[1]);
        TP t = b.t;
        std::vector<ASTNodePtr> args;
        args.push_back(read(a));
        args.push_back(read(b));
        return mkxv(q::call("__cpp_seq__", std::move(args), ln), t);
    }
    case EK::Throw:
    {
        if (e->kids.empty())
        {
            if (fn_->excVars.empty())
                unsupported("throw; outside a handler", ln);
            std::vector<ASTNodePtr> a;
            a.push_back(q::id(fn_->excVars.back(), ln));
            return mkxv(q::call("__cpp_throw__", std::move(a), ln), tVoid());
        }
        XV x = lowerExpr(e->kids[0]);
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
        std::vector<ASTNodePtr> a;
        a.push_back(std::move(val));
        return mkxv(q::call("__cpp_throw__", std::move(a), ln), tVoid());
    }
    }
    unsupported("expression", ln);
}

} // namespace cpp
