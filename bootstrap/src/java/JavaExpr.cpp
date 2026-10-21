// Java expressions: attribution (types, constants, javac's checks) and
// lowering to the Quantum AST. Calls, object creation, lambdas and method
// references are in JavaCall.cpp.
#include "JavaSema.h"

#include <algorithm>

#include <cmath>
#include <cstring>

namespace java
{

using namespace jop;

namespace
{
int ln(const ExprP &e) { return e ? e->pos.line : 0; }

ClassSym *topLevel(ClassSym *c)
{
    while (c && c->outer)
        c = c->outer;
    return c;
}

Const cInt(int64_t v)
{
    Const c;
    c.k = Const::Int;
    c.i = v;
    return c;
}
} // namespace

// ─── Dispatch ────────────────────────────────────────────────────────────────

Val Sema::expr(const ExprP &e, const TypeP &target)
{
    if (!e)
    {
        Val v;
        v.node = q::nil(0);
        v.type = tError();
        return v;
    }
    switch (e->k)
    {
    case EK::Literal: return literal(e);
    case EK::Name: return name(e, true);
    case EK::Field: return fieldAccess(e, true);
    case EK::Call: return call(e, target);
    case EK::New: return newObject(e, target);
    case EK::NewArray: return newArray(e, target);
    case EK::ArrayLit:
        if (target && target->k == TK::Array)
            return arrayLit(e, target);
        error(e->pos, "illegal initializer for " + (target ? typeStr(target) : std::string("<any>")));
        return Val{q::nil(ln(e)), tError()};
    case EK::Index: return index(e);
    case EK::Unary: return unary(e);
    case EK::PreIncDec:
    case EK::PostIncDec: return incdec(e);
    case EK::Binary: return binary(e);
    case EK::Assign: return assign(e);
    case EK::Cond: return conditional(e, target);
    case EK::Cast: return castExpr(e);
    case EK::InstanceOf: return instanceOf(e);
    case EK::Lambda: return lambda(e, target);
    case EK::MethodRef: return methodRef(e, target);
    case EK::This: return thisExpr(e);
    case EK::ClassLit: return classLit(e);
    case EK::Switch: return switchExpr(e, target);
    }
    return Val{q::nil(ln(e)), tError()};
}

ASTNodePtr Sema::exprTo(const ExprP &e, const TypeP &to, Pos pos, const char *what)
{
    (void)what;
    Val v = expr(e, to);
    return coerce(std::move(v), to, pos);
}

ASTNodePtr Sema::cond(const ExprP &e)
{
    Val v = expr(e, prim(TK::Boolean));
    if (!isBooleanish(v.type))
    {
        error(e->pos, "incompatible types: " + typeStr(v.type) + " cannot be converted to boolean");
        return std::move(v.node);
    }
    return coerce(std::move(v), prim(TK::Boolean), e->pos);
}

// ─── Literals and constants ──────────────────────────────────────────────────

Val Sema::literal(const ExprP &e)
{
    Val v;
    int l = ln(e);
    switch (e->lit)
    {
    case Expr::LInt:
        v.type = prim(TK::Int);
        v.cv = cInt(e->ival);
        v.node = q::lng(e->ival, l);
        break;
    case Expr::LLong:
        v.type = prim(TK::Long);
        v.cv.k = Const::Long;
        v.cv.i = e->ival;
        v.node = q::lng(e->ival, l);
        break;
    case Expr::LFloat:
        v.type = prim(TK::Float);
        v.cv.k = Const::Float;
        v.cv.d = (double)(float)e->dval;
        v.node = q::num(v.cv.d, l);
        break;
    case Expr::LDouble:
        v.type = prim(TK::Double);
        v.cv.k = Const::Double;
        v.cv.d = e->dval;
        v.node = q::num(e->dval, l);
        break;
    case Expr::LChar:
        v.type = prim(TK::Char);
        v.cv.k = Const::Char;
        v.cv.i = e->ival;
        v.node = q::lng(e->ival, l);
        break;
    case Expr::LString:
        v.type = tString();
        v.cv.k = Const::Str;
        v.cv.s = e->sval;
        v.node = q::str(e->sval, l);
        break;
    case Expr::LBool:
        v.type = prim(TK::Boolean);
        v.cv.k = Const::Bool;
        v.cv.b = e->bval;
        v.node = q::boolean(e->bval, l);
        break;
    case Expr::LNull:
        v.type = tNull();
        v.node = q::nil(l);
        break;
    }
    return v;
}

namespace
{
TK constKindType(const Const &c)
{
    switch (c.k)
    {
    case Const::Int: return TK::Int;
    case Const::Long: return TK::Long;
    case Const::Float: return TK::Float;
    case Const::Double: return TK::Double;
    case Const::Char: return TK::Char;
    case Const::Bool: return TK::Boolean;
    default: return TK::Error;
    }
}

double constD(const Const &c) { return c.k == Const::Float || c.k == Const::Double ? c.d : (double)c.i; }
int64_t constI(const Const &c) { return c.k == Const::Float || c.k == Const::Double ? (int64_t)c.d : c.i; }

std::string constStr(const Const &c);
} // namespace

std::string javaDoubleToStringC(double d, bool isFloat); // JavaEmit.cpp

namespace
{
std::string constStr(const Const &c)
{
    switch (c.k)
    {
    case Const::Str: return c.s;
    case Const::Int:
    case Const::Long: return std::to_string(c.i);
    case Const::Char:
    {
        std::string out;
        uint32_t u = (uint32_t)c.i;
        if (u < 0x80)
            out += (char)u;
        else if (u < 0x800)
        {
            out += (char)(0xC0 | (u >> 6));
            out += (char)(0x80 | (u & 0x3F));
        }
        else
        {
            out += (char)(0xE0 | (u >> 12));
            out += (char)(0x80 | ((u >> 6) & 0x3F));
            out += (char)(0x80 | (u & 0x3F));
        }
        return out;
    }
    case Const::Bool: return c.b ? "true" : "false";
    case Const::Float: return javaDoubleToStringC(c.d, true);
    case Const::Double: return javaDoubleToStringC(c.d, false);
    default: return "";
    }
}
} // namespace

// Folds `a op b` for constants of result type `rt` (JLS §15.29).
bool Sema::foldConst(const std::string &op, const Const &a, const Const &b, const TypeP &rt, Const &out)
{
    if (!a.ok() || !b.ok())
        return false;
    if (op == "+" && (a.k == Const::Str || b.k == Const::Str))
    {
        out.k = Const::Str;
        out.s = constStr(a) + constStr(b);
        return true;
    }
    if (op == "&&" || op == "||")
    {
        if (a.k != Const::Bool || b.k != Const::Bool)
            return false;
        out.k = Const::Bool;
        out.b = op == "&&" ? (a.b && b.b) : (a.b || b.b);
        return true;
    }
    if (a.k == Const::Bool || b.k == Const::Bool)
    {
        if (a.k != Const::Bool || b.k != Const::Bool)
            return false;
        out.k = Const::Bool;
        if (op == "==") out.b = a.b == b.b;
        else if (op == "!=") out.b = a.b != b.b;
        else if (op == "&") out.b = a.b && b.b;
        else if (op == "|") out.b = a.b || b.b;
        else if (op == "^") out.b = a.b != b.b;
        else return false;
        return true;
    }
    if (a.k == Const::Str || b.k == Const::Str)
        return false;
    // comparisons
    if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "!=")
    {
        bool fl = a.k == Const::Float || a.k == Const::Double || b.k == Const::Float || b.k == Const::Double;
        out.k = Const::Bool;
        if (fl)
        {
            double x = constD(a), y = constD(b);
            out.b = op == "<" ? x < y : op == ">" ? x > y : op == "<=" ? x <= y : op == ">=" ? x >= y : op == "==" ? x == y : x != y;
        }
        else
        {
            int64_t x = a.i, y = b.i;
            out.b = op == "<" ? x < y : op == ">" ? x > y : op == "<=" ? x <= y : op == ">=" ? x >= y : op == "==" ? x == y : x != y;
        }
        return true;
    }
    if (!rt)
        return false;
    if (rt->k == TK::Double || rt->k == TK::Float)
    {
        double x = constD(a), y = constD(b), r;
        if (op == "+") r = x + y;
        else if (op == "-") r = x - y;
        else if (op == "*") r = x * y;
        else if (op == "/") r = x / y;
        else if (op == "%") r = std::fmod(x, y);
        else return false;
        out.k = rt->k == TK::Float ? Const::Float : Const::Double;
        out.d = rt->k == TK::Float ? (double)(float)r : r;
        return true;
    }
    bool isLong = rt->k == TK::Long;
    int64_t x = constI(a), y = constI(b);
    uint64_t ux = (uint64_t)x, uy = (uint64_t)y;
    int64_t r;
    auto w = [&](int64_t v) { return isLong ? v : (int64_t)(int32_t)(uint32_t)(uint64_t)v; };
    if (op == "+") r = w((int64_t)(ux + uy));
    else if (op == "-") r = w((int64_t)(ux - uy));
    else if (op == "*") r = w((int64_t)(ux * uy));
    else if (op == "/" || op == "%")
    {
        if (y == 0)
            return false; // evaluated at run time: ArithmeticException
        if (isLong && x == INT64_MIN && y == -1)
            r = op == "/" ? INT64_MIN : 0;
        else
            r = w(op == "/" ? x / y : x % y);
    }
    else if (op == "&") r = x & y;
    else if (op == "|") r = x | y;
    else if (op == "^") r = x ^ y;
    else if (op == "<<") r = isLong ? (int64_t)(ux << (y & 63)) : w((int64_t)((uint32_t)x << (y & 31)));
    else if (op == ">>") r = isLong ? (x >> (y & 63)) : (int64_t)((int32_t)x >> (y & 31));
    else if (op == ">>>") r = isLong ? (int64_t)(ux >> (y & 63)) : (int64_t)(uint32_t)((uint32_t)(int32_t)x >> (y & 31));
    else return false;
    out.k = isLong ? Const::Long : Const::Int;
    out.i = isLong ? r : (int64_t)(int32_t)r;
    return true;
}

// The value of a constant variable (a final field initialized with a
// constant expression), evaluated in its own class.
Const Sema::fieldConst(FieldSym *f)
{
    if (!f || !(f->mods & M_FINAL) || !f->var || !f->var->init)
        return Const{};
    if (f->constState == 2)
        return f->cval;
    if (f->constState == 1)
        return Const{}; // circular
    TypeP ft = f->type;
    if (!(isPrim(ft) || isStringType(ft)))
    {
        f->constState = 2;
        return Const{};
    }
    f->constState = 1;
    // attribute the initializer in the field's class, discarding the code
    FnCtx *savedFn = fn_;
    ClassSym *savedCtx = ctxClass_;
    int savedUnit = unit_;
    size_t savedDiags = diags_.size();
    FnCtx ctx;
    ctx.cls = f->owner;
    ctx.isStatic = true;
    ctx.scopes.emplace_back();
    fn_ = &ctx;
    ctxClass_ = f->owner;
    unit_ = f->owner->unit;
    Const result;
    if (f->var->init->k != EK::ArrayLit && f->var->init->k != EK::Lambda)
    {
        Val v = expr(f->var->init, ft);
        if (v.cv.ok())
        {
            // convert to the field's type (int constant in a long / double field, ...)
            result = v.cv;
            if (isPrim(ft))
            {
                switch (ft->k)
                {
                case TK::Long: result.k = Const::Long; result.i = constI(v.cv); break;
                case TK::Int: result.k = Const::Int; result.i = (int32_t)constI(v.cv); break;
                case TK::Short: result.k = Const::Int; result.i = (int16_t)constI(v.cv); break;
                case TK::Byte: result.k = Const::Int; result.i = (int8_t)constI(v.cv); break;
                case TK::Char: result.k = Const::Char; result.i = (uint16_t)constI(v.cv); break;
                case TK::Double: result.k = Const::Double; result.d = constD(v.cv); break;
                case TK::Float: result.k = Const::Float; result.d = (double)(float)constD(v.cv); break;
                case TK::Boolean: if (v.cv.k != Const::Bool) result = Const{}; break;
                default: break;
                }
                if (ft->k != TK::Boolean && v.cv.k == Const::Str)
                    result = Const{};
            }
            else if (v.cv.k != Const::Str)
                result = Const{};
        }
    }
    diags_.resize(savedDiags); // reported again where the initializer is lowered
    fn_ = savedFn;
    ctxClass_ = savedCtx;
    unit_ = savedUnit;
    f->cval = result;
    f->constState = 2;
    return result;
}

// ─── Conversions ─────────────────────────────────────────────────────────────

ASTNodePtr Sema::convertPrim(ASTNodePtr n, const TypeP &from, const TypeP &to, int l)
{
    if (!from || !to || from->k == to->k || isError(from) || isError(to))
        return n;
    TK f = from->k, t = to->k;
    bool fInt = f >= TK::Byte && f <= TK::Long;
    bool tInt = t >= TK::Byte && t <= TK::Long;
    if (fInt && tInt)
    {
        switch (t)
        {
        case TK::Long: return n; // all integral values are int64
        case TK::Int:
            if (f == TK::Long)
                return q::un(L2I, std::move(n), l);
            return n;
        case TK::Short:
            if (f == TK::Byte)
                return n;
            return q::un(I2S, std::move(n), l);
        case TK::Byte: return q::un(I2B, std::move(n), l);
        case TK::Char: return q::un(I2C, std::move(n), l);
        default: return n;
        }
    }
    if (fInt && t == TK::Double)
        return q::un(I2D, std::move(n), l);
    if (fInt && t == TK::Float)
        return q::un(I2F, std::move(n), l);
    if (!fInt && tInt)
    {
        if (t == TK::Long)
            return q::un(D2L, std::move(n), l);
        ASTNodePtr i = q::un(D2I, std::move(n), l);
        if (t == TK::Int)
            return i;
        return q::un(t == TK::Short ? I2S : t == TK::Byte ? I2B : I2C, std::move(i), l);
    }
    if (f == TK::Double && t == TK::Float)
        return q::un(D2F, std::move(n), l);
    return n; // float -> double, boolean -> boolean
}

namespace
{
Const convertConst(const Const &c, TK to)
{
    Const r;
    if (!c.ok() || c.k == Const::Str || c.k == Const::Bool)
        return (to == TK::Boolean && c.k == Const::Bool) ? c : r;
    double d = constD(c);
    int64_t i = constI(c);
    bool fromFloat = c.k == Const::Float || c.k == Const::Double;
    auto d2i = [](double x) -> int64_t {
        if (std::isnan(x)) return 0;
        if (x >= 2147483647.0) return 2147483647;
        if (x <= -2147483648.0) return -2147483648LL;
        return (int64_t)x;
    };
    auto d2l = [](double x) -> int64_t {
        if (std::isnan(x)) return 0;
        if (x >= 9223372036854775807.0) return INT64_MAX;
        if (x <= -9223372036854775808.0) return INT64_MIN;
        return (int64_t)x;
    };
    switch (to)
    {
    case TK::Int: r.k = Const::Int; r.i = fromFloat ? d2i(d) : (int32_t)i; break;
    case TK::Long: r.k = Const::Long; r.i = fromFloat ? d2l(d) : i; break;
    case TK::Short: r.k = Const::Int; r.i = (int16_t)(fromFloat ? d2i(d) : i); break;
    case TK::Byte: r.k = Const::Int; r.i = (int8_t)(fromFloat ? d2i(d) : i); break;
    case TK::Char: r.k = Const::Char; r.i = (uint16_t)(fromFloat ? d2i(d) : i); break;
    case TK::Double: r.k = Const::Double; r.d = fromFloat ? d : (double)i; break;
    case TK::Float: r.k = Const::Float; r.d = (double)(float)(fromFloat ? d : (double)i); break;
    default: break;
    }
    return r;
}

ASTNodePtr constNode(const Const &c, int l)
{
    switch (c.k)
    {
    case Const::Int:
    case Const::Long:
    case Const::Char: return q::lng(c.i, l);
    case Const::Float:
    case Const::Double: return q::num(c.d, l);
    case Const::Bool: return q::boolean(c.b, l);
    case Const::Str: return q::str(c.s, l);
    default: return q::nil(l);
    }
}
} // namespace

// Assignment / method-invocation conversion of `v` to `to` (casting
// conversion when isCast), with javac's errors.
// The J_UN op that unboxes a box of primitive kind k.
static int unboxOp(TK k)
{
    if (k == TK::Char)
        return UNBOXC;
    if (k == TK::Long || k == TK::Short || k == TK::Byte)
        return UNBOXN;
    return NONNULL;
}

ASTNodePtr Sema::coerce(Val v, const TypeP &to, Pos pos, bool isCast, bool report)
{
    int l = pos.line;
    TypeP from = v.type;
    if (!v.node)
        v.node = q::nil(l);
    if (isError(from) || isError(to) || !to)
        return std::move(v.node);
    if (to->k == TK::Void)
        return std::move(v.node);
    if (from->k == TK::Void)
    {
        if (report)
            error(pos, "'void' type not allowed here");
        return std::move(v.node);
    }
    bool ok = isCast ? castable(from, to) : assignable(from, to, v.cv.ok() ? &v.cv : nullptr);
    if (!ok)
    {
        if (report)
        {
            bool lossy = !isCast && isNumeric(numericUnboxed(from) ? numericUnboxed(from) : from) && isPrim(to) &&
                         isNumeric(to) && numericUnboxed(from);
            if (lossy && isPrim(from))
                error(pos, "incompatible types: possible lossy conversion from " + typeStr(from) + " to " + typeStr(to));
            else
                error(pos, "incompatible types: " + typeStr(from) + " cannot be converted to " + typeStr(to));
        }
        return std::move(v.node);
    }
    if (isPrim(from) && isPrim(to))
    {
        if (v.cv.ok())
        {
            Const c = convertConst(v.cv, to->k);
            if (c.ok())
                return constNode(c, l);
        }
        return convertPrim(std::move(v.node), from, to, l);
    }
    if (isPrim(from) && isRef(to))
    {
        // boxing (Character boxes are one-character strings), after constant
        // narrowing for Byte / Short / Character targets
        TypeP target = unboxed(to);
        TK boxK = target ? target->k : from->k;
        if (!target)
            boxK = from->k; // Object o = 5: Integer
        ASTNodePtr n = v.cv.ok() ? constNode(convertConst(v.cv, boxK), l) : convertPrim(std::move(v.node), from, prim(boxK), l);
        if (!n)
            n = q::nil(l);
        if (boxK == TK::Char)
        {
            need(cls("java.lang.Character"));
            return q::un(BOXC, std::move(n), l);
        }
        if (boxK == TK::Long || boxK == TK::Short || boxK == TK::Byte)
        {
            // a Long / Short / Byte box is an object of its class
            need(cls(boxK == TK::Long ? "java.lang.Long" : boxK == TK::Short ? "java.lang.Short" : "java.lang.Byte"));
            return q::un(boxK == TK::Long ? BOXJ : boxK == TK::Short ? BOXS : BOXB, std::move(n), l);
        }
        return n;
    }
    if (isRef(from) && isPrim(to))
    {
        TypeP u = unboxed(from);
        if (!u)
        {
            // cast Object -> int: check the box, then unbox
            TypeP box = boxOf(to);
            ASTNodePtr checked = q::cast(std::move(v.node), descriptor(box), l);
            return q::un(unboxOp(to->k), std::move(checked), l);
        }
        ASTNodePtr n = q::un(unboxOp(u->k), std::move(v.node), l);
        return convertPrim(std::move(n), u, to, l);
    }
    // reference -> reference
    if (isCast)
    {
        TypeP et = erasure(to);
        bool needCheck = !(isSubtype(from, to)) && et->k != TK::TypeVar && to->k != TK::TypeVar &&
                         !(et->k == TK::Class && et->cls && et->cls->binaryName == "java.lang.Object");
        if (needCheck)
            return q::cast(std::move(v.node), descriptor(to), l);
    }
    return std::move(v.node);
}

// String conversion for `+` (JLS §5.1.11).
ASTNodePtr Sema::stringOf(Val v, int l)
{
    if (v.cv.ok())
        return q::str(constStr(v.cv), l);
    TypeP t = v.type;
    if (!t)
        return q::un(VSTR, std::move(v.node), l);
    switch (t->k)
    {
    case TK::Byte:
    case TK::Short:
    case TK::Int:
    case TK::Long: return q::un(ISTR, std::move(v.node), l);
    case TK::Char: return q::un(CSTR, std::move(v.node), l);
    case TK::Boolean: return q::un(ZSTR, std::move(v.node), l);
    case TK::Double: return q::un(DSTR, std::move(v.node), l);
    case TK::Float: return q::un(FSTR, std::move(v.node), l);
    case TK::Null: return q::str("null", l);
    default: break;
    }
    if (isStringType(t))
        return q::un(SSTR, std::move(v.node), l);
    if (TypeP u = unboxed(t))
    {
        if (u->k == TK::Float)
        {
            // a boxed float is held as a double: format it as a float
            std::vector<ASTNodePtr> a;
            std::string tmpv = temp();
            return q::seq([&] {
                std::vector<ASTNodePtr> p;
                p.push_back(q::assign(q::id(tmpv, l), std::move(v.node), l));
                p.push_back(q::ternary(q::bin(REFEQ, q::id(tmpv, l), q::nil(l), l), q::str("null", l),
                                       q::un(FSTR, q::id(tmpv, l), l), l));
                return p;
            }(), l);
        }
        if (u->k == TK::Long || u->k == TK::Short || u->k == TK::Byte || u->k == TK::Char)
            return q::inv("#str", std::move(v.node), {}, l);
        return q::un(VSTR, std::move(v.node), l);
    }
    // any other object: String.valueOf(o) (toString with dynamic dispatch)
    return q::inv("#str", std::move(v.node), {}, l);
}

ASTNodePtr Sema::defaultValue(const TypeP &t, int l)
{
    if (!t)
        return q::nil(l);
    switch (t->k)
    {
    case TK::Boolean: return q::boolean(false, l);
    case TK::Byte:
    case TK::Short:
    case TK::Char:
    case TK::Int:
    case TK::Long: return q::lng(0, l);
    case TK::Float:
    case TK::Double: return q::num(0.0, l);
    default: return q::nil(l);
    }
}

// ─── Names ───────────────────────────────────────────────────────────────────

ASTNodePtr Sema::thisNode(int l) { return q::id("__this", l); }

// `Target.this` as seen from the current code: this, then this$0 links.
ASTNodePtr Sema::outerInstance(ClassSym *target, Pos pos)
{
    int l = pos.line;
    ClassSym *c = curClass();
    if (!fn_ || fn_->isStatic)
    {
        error(pos, "non-static variable this cannot be referenced from a static context");
        return q::nil(l);
    }
    ASTNodePtr node = thisNode(l);
    while (c && c != target && !isSubClass(c, target))
    {
        if (!c->hasOuterInstance)
        {
            error(pos, "non-static variable this cannot be referenced from a static context");
            return node;
        }
        node = q::getf(std::move(node), "this$0", l);
        c = c->outer;
    }
    return node;
}

namespace
{
bool sameTopLevel(ClassSym *a, ClassSym *b) { return topLevel(a) == topLevel(b); }
} // namespace

Val Sema::loadField(FieldSym *f, ASTNodePtr obj, const TypeP &objType, Pos pos)
{
    int l = pos.line;
    Val v;
    TypeP ft = f->type;
    if (!f->isStatic && objType && objType->k != TK::Error)
    {
        TypeP view = asSuper(objType, f->owner);
        if (view)
            ft = subst(ft, substOf(view));
    }
    v.type = ft;
    // access control
    if ((f->mods & M_PRIVATE) && curClass() && !sameTopLevel(curClass(), f->owner))
        error(pos, f->name + " has private access in " + typeStr(classType(f->owner)));
    Const c = fieldConst(f);
    if (c.ok() && (f->isStatic || !obj || q::isSimple(obj)))
    {
        v.cv = c;
        v.node = constNode(c, l);
        return v;
    }
    if (f->isStatic)
    {
        ClassSym *owner = f->owner;
        need(owner);
        ASTNodePtr g = q::id(f->key, l);
        bool inside = curClass() && (curClass() == owner || isSubClass(curClass(), owner));
        if (owner->needsClinit && !inside)
        {
            std::vector<ASTNodePtr> p;
            p.push_back(q::call(owner->clinitFn(), {}, l));
            p.push_back(std::move(g));
            g = q::seq(std::move(p), l);
        }
        if (obj && !q::isSimple(obj))
        {
            std::vector<ASTNodePtr> p;
            p.push_back(std::move(obj));
            p.push_back(std::move(g));
            g = q::seq(std::move(p), l);
        }
        v.node = std::move(g);
        return v;
    }
    v.node = q::getf(std::move(obj), f->key, l);
    return v;
}

// A local of an enclosing method, read from a local / anonymous class:
// the class's copy, captured when the instance was created.
namespace
{
struct FoundLocal
{
    LocalVar *var = nullptr;
    FnCtx *owner = nullptr;
    ClassSym *capturingClass = nullptr; // set when read through a class boundary
};
} // namespace

Val Sema::name(const ExprP &e, bool wantValue)
{
    const std::string &n = e->name;
    int l = ln(e);
    if (!n.empty() && n[0] == '\x01')
    {
        // a class named by binary name (a synthesized method reference)
        Val r;
        auto it = byBinary_.find(n.substr(1));
        if (it != byBinary_.end())
        {
            r.typeSym = it->second;
            r.type = classType(it->second);
        }
        else
            r.type = tError();
        return r;
    }
    // 1. locals of this function and of the functions enclosing it (lambdas)
    FnCtx *owner = nullptr;
    if (LocalVar *v = findLocal(n, &owner))
    {
        Val r;
        r.type = v->type;
        r.cv = v->cval;
        if (!isAssigned(v, owner))
            error(e->pos, "variable " + n + " might not have been initialized");
        if (owner != fn_ && !probing_)
        {
            bool viaLambda = false;
            for (FnCtx *f = fn_; f && f != owner; f = f->outer)
                if (!f->transparent)
                {
                    viaLambda = true;
                    if (std::find(f->captured.begin(), f->captured.end(), v->q) == f->captured.end())
                        f->captured.push_back(v->q);
                }
            if (viaLambda)
                v->captureSites.push_back({e->pos, true}), v->captureUnit = unit_;
        }
        r.node = r.cv.ok() ? constNode(r.cv, l) : q::id(v->q, l);
        return r;
    }
    // 2. fields of the enclosing classes, and the enclosing methods' locals
    //    that local / anonymous classes capture
    for (ClassSym *c = curClass(); c; c = c->outer)
    {
        if (FieldSym *f = findField(c, n))
        {
            if (!f->isStatic)
            {
                if (c == curClass() && inStaticContext())
                {
                    error(e->pos, "non-static variable " + n + " cannot be referenced from a static context");
                    return Val{q::nil(l), f->type};
                }
                return loadField(f, outerInstance(c, e->pos), selfType(c), e->pos);
            }
            return loadField(f, nullptr, nullptr, e->pos);
        }
        if (c->enclosingFn)
        {
            FnCtx *saved = fn_;
            fn_ = c->enclosingFn;
            FnCtx *o = nullptr;
            LocalVar *v = findLocal(n, &o);
            fn_ = saved;
            if (v)
            {
                if (!probing_)
                {
                    v->captureSites.push_back({e->pos, false});
                    v->captureUnit = unit_;
                }
                Val r;
                r.type = v->type;
                r.cv = v->cval;
                if (r.cv.ok())
                {
                    r.node = constNode(r.cv, l);
                    return r;
                }
                std::string field = "val$" + n;
                bool known = false;
                for (auto &cap : c->captures)
                    known = known || cap.name == n;
                if (!known)
                    c->captures.push_back(Capture{n, v->type, field});
                r.node = q::getf(outerInstance(c, e->pos), field, l);
                return r;
            }
        }
    }
    // 3. static imports
    for (auto &im : units_[unit_].imports)
    {
        if (!im.isStatic)
            continue;
        if (!im.onDemand && (im.name.empty() || im.name.back() != n))
            continue;
        std::vector<std::string> typeParts(im.name.begin(), im.name.end() - (im.onDemand ? 0 : 1));
        if (ClassSym *tc = resolveTypeName(typeParts, im.pos, false))
            if (FieldSym *f = findField(tc, n))
                if (f->isStatic)
                    return loadField(f, nullptr, nullptr, e->pos);
    }
    if (!wantValue)
    {
        if (ClassSym *tc = lookupClassSimple(n))
        {
            Val r;
            r.typeSym = tc;
            r.type = classType(tc);
            return r;
        }
        for (auto &kv : byQual_)
            if (kv.first.compare(0, n.size() + 1, n + ".") == 0)
            {
                Val r;
                r.package = n;
                return r;
            }
    }
    ClassSym *loc = curClass();
    error(e->pos, "cannot find symbol",
          {"  symbol:   variable " + n,
           "  location: " + (loc ? std::string(loc->isInterface() ? "interface " : "class ") + typeStr(classType(loc))
                                 : std::string("class"))});
    return Val{q::nil(l), tError()};
}

Val Sema::fieldAccess(const ExprP &e, bool wantValue)
{
    int l = ln(e);
    const std::string &n = e->name;
    Pos at = e->opPos.line ? e->opPos : e->pos;
    if (e->superCall)
    {
        // super.field / Outer.super.field
        ClassSym *c = curClass();
        if (!c || !c->super || !c->super->cls)
            return Val{q::nil(l), tError()};
        FieldSym *f = findField(c->super->cls, n);
        if (!f)
        {
            error(at, "cannot find symbol", {"  symbol: variable " + n});
            return Val{q::nil(l), tError()};
        }
        return loadField(f, f->isStatic ? nullptr : thisNode(l), c->super, at);
    }
    Val base;
    if (e->a->k == EK::Name)
        base = name(e->a, false);
    else if (e->a->k == EK::Field)
        base = fieldAccess(e->a, false);
    else
        base = expr(e->a);
    if (base.isPackage())
    {
        std::string q = base.package + "." + n;
        if (ClassSym *c = cls(q))
        {
            Val r;
            r.typeSym = c;
            r.type = classType(c);
            return r;
        }
        if (!wantValue)
        {
            Val r;
            r.package = q;
            return r;
        }
        error(e->a->pos, "package " + base.package + " does not exist");
        return Val{q::nil(l), tError()};
    }
    if (base.isType())
    {
        ClassSym *c = base.typeSym;
        if (FieldSym *f = findField(c, n))
        {
            if (!f->isStatic)
            {
                error(at, "non-static variable " + n + " cannot be referenced from a static context");
                return Val{q::nil(l), f->type};
            }
            return loadField(f, nullptr, nullptr, at);
        }
        if (!wantValue)
            if (ClassSym *m = memberType(c, n))
            {
                Val r;
                r.typeSym = m;
                r.type = classType(m);
                return r;
            }
        error(at, "cannot find symbol",
              {"  symbol:   variable " + n, "  location: " + std::string(c->isInterface() ? "interface " : "class ") +
                                                typeStr(classType(c))});
        return Val{q::nil(l), tError()};
    }
    TypeP t = base.type;
    if (isError(t))
        return Val{std::move(base.node), tError()};
    if (isPrim(t))
    {
        error(at, typeStr(t) + " cannot be dereferenced");
        return Val{std::move(base.node), tError()};
    }
    TypeP ub = upperBound(t);
    if (ub->k == TK::Array)
    {
        if (n == "length")
        {
            Val r;
            r.type = prim(TK::Int);
            r.node = q::un(ALEN, std::move(base.node), l);
            return r;
        }
        error(at, "cannot find symbol", {"  symbol:   variable " + n, "  location: class " + typeStr(t)});
        return Val{q::nil(l), tError()};
    }
    if (ub->k != TK::Class || !ub->cls)
    {
        error(at, "cannot find symbol", {"  symbol: variable " + n});
        return Val{q::nil(l), tError()};
    }
    FieldSym *f = findField(ub->cls, n);
    if (!f)
    {
        error(at, "cannot find symbol",
              {"  symbol:   variable " + n, "  location: " + std::string(base.node && base.node->is<Identifier>() ? "variable " + base.node->as<Identifier>().name + " of type " : "class ") + typeStr(t)});
        return Val{q::nil(l), tError()};
    }
    return loadField(f, std::move(base.node), ub, at);
}

// ─── Unary / binary operators ────────────────────────────────────────────────

Val Sema::unary(const ExprP &e)
{
    int l = ln(e);
    Val a = expr(e->a);
    const std::string &op = e->name;
    Val r;
    if (op == "!")
    {
        if (!isBooleanish(a.type))
        {
            error(e->pos, "bad operand type " + typeStr(a.type) + " for unary operator '!'");
            return Val{std::move(a.node), tError()};
        }
        r.type = prim(TK::Boolean);
        if (a.cv.k == Const::Bool)
        {
            r.cv = a.cv;
            r.cv.b = !a.cv.b;
            r.node = q::boolean(r.cv.b, l);
            return r;
        }
        r.node = q::qnot(coerce(std::move(a), prim(TK::Boolean), e->pos), l);
        return r;
    }
    TypeP pt = unaryPromote(a.type);
    if (isError(a.type))
        return Val{std::move(a.node), tError()};
    if (isError(pt) || (op == "~" && !isIntegral(pt)))
    {
        error(e->pos, "bad operand type " + typeStr(a.type) + " for unary operator '" + op + "'");
        return Val{std::move(a.node), tError()};
    }
    r.type = pt;
    Const cv = a.cv;
    ASTNodePtr x = coerce(std::move(a), pt, e->pos);
    if (cv.ok())
    {
        Const c = convertConst(cv, pt->k);
        if (op == "+")
        {
            r.cv = c;
            r.node = constNode(c, l);
            return r;
        }
        if (op == "-")
        {
            if (c.k == Const::Double || c.k == Const::Float)
                r.cv = c, r.cv.d = -c.d;
            else if (c.k == Const::Long)
                r.cv = c, r.cv.i = (int64_t)(0 - (uint64_t)c.i);
            else
                r.cv = c, r.cv.i = (int32_t)(0 - (uint32_t)c.i);
            r.node = constNode(r.cv, l);
            return r;
        }
        if (op == "~")
        {
            r.cv = c;
            r.cv.i = ~c.i;
            r.node = constNode(r.cv, l);
            return r;
        }
    }
    if (op == "+")
        r.node = std::move(x);
    else if (op == "-")
        r.node = q::un(pt->k == TK::Int ? INEG : pt->k == TK::Long ? LNEG : DNEG, std::move(x), l);
    else
        r.node = q::un(pt->k == TK::Long ? LNOT : INOT, std::move(x), l);
    return r;
}

namespace
{
int arithOp(const std::string &op, TK t)
{
    int base = t == TK::Int ? IADD : t == TK::Long ? LADD : t == TK::Float ? FADD : DADD;
    int off = op == "+" ? 0 : op == "-" ? 1 : op == "*" ? 2 : op == "/" ? 3 : 4;
    return base + off;
}
} // namespace

Val Sema::binary(const ExprP &e)
{
    int l = ln(e);
    const std::string &op = e->name;
    Pos at = e->opPos.line ? e->opPos : e->pos;
    Val r;
    if (op == "&&" || op == "||")
    {
        Val a = expr(e->a);
        Val b = expr(e->b);
        if (!isBooleanish(a.type) || !isBooleanish(b.type))
        {
            if (!isError(a.type) && !isError(b.type))
                error(at, "bad operand types for binary operator '" + op + "'",
                      {"  first type:  " + typeStr(a.type), "  second type: " + typeStr(b.type)});
            return Val{q::nil(l), tError()};
        }
        r.type = prim(TK::Boolean);
        Const ca = a.cv, cb = b.cv;
        if (foldConst(op, ca, cb, r.type, r.cv))
        {
            r.node = q::boolean(r.cv.b, l);
            return r;
        }
        r.node = q::qbin(op, coerce(std::move(a), r.type, e->a->pos), coerce(std::move(b), r.type, e->b->pos), l);
        return r;
    }
    Val a = expr(e->a);
    Val b = expr(e->b);
    if (isError(a.type) || isError(b.type))
        return Val{q::nil(l), tError()};
    auto bad = [&]() -> Val
    {
        error(at, "bad operand types for binary operator '" + op + "'",
              {"  first type:  " + typeStr(a.type), "  second type: " + typeStr(b.type)});
        return Val{q::nil(l), tError()};
    };
    if (a.type->k == TK::Void || b.type->k == TK::Void)
    {
        error(a.type->k == TK::Void ? e->a->pos : e->b->pos, "'void' type not allowed here");
        return Val{q::nil(l), tError()};
    }
    // string concatenation
    if (op == "+" && (isStringType(a.type) || isStringType(b.type)))
    {
        r.type = tString();
        if (foldConst("+", a.cv, b.cv, r.type, r.cv))
        {
            r.node = q::str(r.cv.s, l);
            return r;
        }
        ASTNodePtr sa = stringOf(std::move(a), l);
        ASTNodePtr sb = stringOf(std::move(b), l);
        r.node = q::bin(SCAT, std::move(sa), std::move(sb), l);
        return r;
    }
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%")
    {
        TypeP pt = binaryPromote(a.type, b.type);
        if (isError(pt))
            return bad();
        r.type = pt;
        if (foldConst(op, convertConst(a.cv, pt->k), convertConst(b.cv, pt->k), pt, r.cv))
        {
            r.node = constNode(r.cv, l);
            return r;
        }
        ASTNodePtr x = coerce(std::move(a), pt, e->a->pos);
        ASTNodePtr y = coerce(std::move(b), pt, e->b->pos);
        r.node = q::bin(arithOp(op, pt->k), std::move(x), std::move(y), l);
        return r;
    }
    if (op == "<<" || op == ">>" || op == ">>>")
    {
        TypeP lt = unaryPromote(a.type), rt = unaryPromote(b.type);
        if (!isIntegral(lt) || !isIntegral(rt))
            return bad();
        r.type = lt;
        if (foldConst(op, convertConst(a.cv, lt->k), convertConst(b.cv, rt->k), lt, r.cv))
        {
            r.node = constNode(r.cv, l);
            return r;
        }
        ASTNodePtr x = coerce(std::move(a), lt, e->a->pos);
        ASTNodePtr y = coerce(std::move(b), rt, e->b->pos);
        bool isLong = lt->k == TK::Long;
        int code = op == "<<" ? (isLong ? LSHL : ISHL) : op == ">>" ? (isLong ? LSHR : ISHR) : (isLong ? LUSHR : IUSHR);
        r.node = q::bin(code, std::move(x), std::move(y), l);
        return r;
    }
    if (op == "<" || op == ">" || op == "<=" || op == ">=")
    {
        TypeP pt = binaryPromote(a.type, b.type);
        if (isError(pt))
            return bad();
        r.type = prim(TK::Boolean);
        if (foldConst(op, a.cv, b.cv, r.type, r.cv))
        {
            r.node = q::boolean(r.cv.b, l);
            return r;
        }
        ASTNodePtr x = coerce(std::move(a), pt, e->a->pos);
        ASTNodePtr y = coerce(std::move(b), pt, e->b->pos);
        r.node = q::bin(op == "<" ? LT : op == ">" ? GT : op == "<=" ? LE : GE, std::move(x), std::move(y), l);
        return r;
    }
    if (op == "==" || op == "!=")
    {
        r.type = prim(TK::Boolean);
        TypeP na = numericUnboxed(a.type), nb = numericUnboxed(b.type);
        bool numeric = na && nb && (isPrim(a.type) || isPrim(b.type));
        bool boolean = isBooleanish(a.type) && isBooleanish(b.type) && (isPrim(a.type) || isPrim(b.type));
        if (numeric)
        {
            TypeP pt = binaryPromote(a.type, b.type);
            if (foldConst(op, a.cv, b.cv, r.type, r.cv))
            {
                r.node = q::boolean(r.cv.b, l);
                return r;
            }
            ASTNodePtr x = coerce(std::move(a), pt, e->a->pos);
            ASTNodePtr y = coerce(std::move(b), pt, e->b->pos);
            r.node = q::bin(op == "==" ? EQ : NE, std::move(x), std::move(y), l);
            return r;
        }
        if (boolean)
        {
            if (foldConst(op, a.cv, b.cv, r.type, r.cv))
            {
                r.node = q::boolean(r.cv.b, l);
                return r;
            }
            ASTNodePtr x = coerce(std::move(a), prim(TK::Boolean), e->a->pos);
            ASTNodePtr y = coerce(std::move(b), prim(TK::Boolean), e->b->pos);
            r.node = q::bin(op == "==" ? EQ : NE, std::move(x), std::move(y), l);
            return r;
        }
        if (isPrim(a.type) || isPrim(b.type))
            return bad();
        // references: must be castable one to the other
        if (!castable(a.type, b.type) && a.type->k != TK::Null && b.type->k != TK::Null)
        {
            error(at, "incomparable types: " + typeStr(a.type) + " and " + typeStr(b.type));
            return Val{q::nil(l), r.type};
        }
        if (a.cv.k == Const::Str && b.cv.k == Const::Str)
        {
            r.cv.k = Const::Bool;
            r.cv.b = (a.cv.s == b.cv.s) == (op == "==");
            r.node = q::boolean(r.cv.b, l);
            return r;
        }
        // two boxes: object identity, as autoboxing's caches give it
        bool boxes = unboxed(a.type) && unboxed(b.type);
        r.node = q::bin(boxes ? (op == "==" ? BOXEQ : BOXNE) : (op == "==" ? REFEQ : REFNE), std::move(a.node),
                        std::move(b.node), l);
        return r;
    }
    if (op == "&" || op == "|" || op == "^")
    {
        if (isBooleanish(a.type) && isBooleanish(b.type))
        {
            r.type = prim(TK::Boolean);
            if (foldConst(op, a.cv, b.cv, r.type, r.cv))
            {
                r.node = q::boolean(r.cv.b, l);
                return r;
            }
            ASTNodePtr x = coerce(std::move(a), r.type, e->a->pos);
            ASTNodePtr y = coerce(std::move(b), r.type, e->b->pos);
            r.node = q::bin(op == "&" ? BAND : op == "|" ? BOR : BXOR, std::move(x), std::move(y), l);
            return r;
        }
        TypeP pt = binaryPromote(a.type, b.type);
        if (isError(pt) || !isIntegral(pt))
            return bad();
        r.type = pt;
        if (foldConst(op, convertConst(a.cv, pt->k), convertConst(b.cv, pt->k), pt, r.cv))
        {
            r.node = constNode(r.cv, l);
            return r;
        }
        ASTNodePtr x = coerce(std::move(a), pt, e->a->pos);
        ASTNodePtr y = coerce(std::move(b), pt, e->b->pos);
        bool isLong = pt->k == TK::Long;
        int code = op == "&" ? (isLong ? LAND : IAND) : op == "|" ? (isLong ? LOR : IOR) : (isLong ? LXOR : IXOR);
        r.node = q::bin(code, std::move(x), std::move(y), l);
        return r;
    }
    return bad();
}

// ─── Assignment ──────────────────────────────────────────────────────────────

namespace
{
struct LValue
{
    enum Kind
    {
        Local,
        Static,
        Field,
        Array,
        Error,
    } kind = Error;
    std::string name; // local's lowered name / static global
    ASTNodePtr obj, idx;
    std::string key;
    TypeP type;
    LocalVar *local = nullptr;
    FieldSym *field = nullptr;
    ClassSym *clinitOf = nullptr;
};
} // namespace

namespace
{
LValue makeLValue(Sema &s, const ExprP &e);

void checkFinalField(Sema &s, FieldSym *f, Pos pos)
{
    if (!(f->mods & M_FINAL))
        return;
    // a blank final may be assigned in its class's constructors / initializers
    FnCtx *fn = s.fn_;
    bool inInit = fn && fn->cls == f->owner && !fn->isLambda &&
                  (f->isStatic ? (fn->method == nullptr && fn->isStatic) : (fn->isCtor || (fn->method == nullptr && !fn->isStatic)));
    if (!inInit || (f->var && f->var->init))
        s.error(pos, "cannot assign a value to final variable " + f->name);
}

LValue makeLValue(Sema &s, const ExprP &e)
{
    LValue lv;
    int l = e->pos.line;
    if (e->k == EK::Name)
    {
        FnCtx *owner = nullptr;
        if (LocalVar *v = s.findLocal(e->name, &owner))
        {
            if (owner != s.fn_)
            {
                s.error(e->pos, "local variables referenced from a lambda expression must be final or effectively final");
            }
            lv.kind = LValue::Local;
            lv.name = v->q;
            lv.type = v->type;
            lv.local = v;
            return lv;
        }
        for (ClassSym *c = s.curClass(); c; c = c->outer)
        {
            if (FieldSym *f = s.findField(c, e->name))
            {
                lv.field = f;
                lv.type = f->type;
                checkFinalField(s, f, e->pos);
                if (f->isStatic)
                {
                    lv.kind = LValue::Static;
                    lv.name = f->key;
                    s.need(f->owner);
                    if (f->owner->needsClinit && !(s.curClass() == f->owner || s.isSubClass(s.curClass(), f->owner)))
                        lv.clinitOf = f->owner;
                    return lv;
                }
                if (c == s.curClass() && s.inStaticContext())
                {
                    s.error(e->pos, "non-static variable " + e->name + " cannot be referenced from a static context");
                    return lv;
                }
                lv.kind = LValue::Field;
                lv.obj = s.outerInstance(c, e->pos);
                lv.key = f->key;
                return lv;
            }
            if (c->enclosingFn)
            {
                FnCtx *saved = s.fn_;
                s.fn_ = c->enclosingFn;
                LocalVar *v = s.findLocal(e->name);
                s.fn_ = saved;
                if (v)
                {
                    s.error(e->pos, "local variables referenced from an inner class must be final or effectively final");
                    lv.type = v->type;
                    return lv;
                }
            }
        }
        s.error(e->pos, "cannot find symbol", {"  symbol:   variable " + e->name,
                                                "  location: class " + (s.curClass() ? s.typeStr(s.classType(s.curClass())) : std::string("?"))});
        return lv;
    }
    if (e->k == EK::Field)
    {
        if (e->superCall)
        {
            ClassSym *c = s.curClass();
            FieldSym *f = c && c->super ? s.findField(c->super->cls, e->name) : nullptr;
            if (!f)
                return lv;
            lv.field = f;
            lv.type = f->type;
            lv.kind = f->isStatic ? LValue::Static : LValue::Field;
            lv.name = f->key;
            lv.key = f->key;
            if (!f->isStatic)
                lv.obj = s.thisNode(l);
            return lv;
        }
        Val base;
        if (e->a->k == EK::Name)
            base = s.name(e->a, false);
        else if (e->a->k == EK::Field)
            base = s.fieldAccess(e->a, false);
        else
            base = s.expr(e->a);
        Pos at = e->opPos.line ? e->opPos : e->pos;
        ClassSym *c = nullptr;
        if (base.isType())
            c = base.typeSym;
        else if (!Sema::isError(base.type))
        {
            TypeP ub = s.upperBound(base.type);
            if (ub->k == TK::Array && e->name == "length")
            {
                s.error(at, "cannot assign a value to final variable length");
                return lv;
            }
            if (Sema::isPrim(base.type))
            {
                s.error(at, s.typeStr(base.type) + " cannot be dereferenced");
                return lv;
            }
            c = ub->k == TK::Class ? ub->cls : nullptr;
        }
        if (!c)
            return lv;
        FieldSym *f = s.findField(c, e->name);
        if (!f)
        {
            s.error(at, "cannot find symbol", {"  symbol:   variable " + e->name, "  location: class " + s.typeStr(s.classType(c))});
            return lv;
        }
        if ((f->mods & M_PRIVATE) && s.curClass() && topLevel(s.curClass()) != topLevel(f->owner))
            s.error(at, f->name + " has private access in " + s.typeStr(s.classType(f->owner)));
        checkFinalField(s, f, at);
        lv.field = f;
        TypeP ft = f->type;
        if (!base.isType() && !f->isStatic)
            if (TypeP view = s.asSuper(base.type, f->owner))
                ft = s.subst(ft, s.substOf(view));
        lv.type = ft;
        if (f->isStatic)
        {
            if (base.isType() == false && base.node && !q::isSimple(base.node))
            {
                // the receiver expression is evaluated, then discarded
            }
            lv.kind = LValue::Static;
            lv.name = f->key;
            s.need(f->owner);
            if (f->owner->needsClinit && !(s.curClass() == f->owner || s.isSubClass(s.curClass(), f->owner)))
                lv.clinitOf = f->owner;
            return lv;
        }
        if (base.isType())
        {
            s.error(at, "non-static variable " + e->name + " cannot be referenced from a static context");
            return lv;
        }
        lv.kind = LValue::Field;
        lv.obj = std::move(base.node);
        lv.key = f->key;
        return lv;
    }
    if (e->k == EK::Index)
    {
        Val a = s.expr(e->a);
        Val i = s.expr(e->b);
        if (Sema::isError(a.type))
            return lv;
        TypeP at = s.upperBound(a.type);
        if (at->k != TK::Array)
        {
            s.error(e->pos, "array required, but " + s.typeStr(a.type) + " found");
            return lv;
        }
        TypeP it = s.unaryPromote(i.type);
        if (!Sema::isError(i.type) && (Sema::isError(it) || it->k != TK::Int))
        {
            if (it && it->k == TK::Long)
                s.error(e->b->pos, "incompatible types: possible lossy conversion from long to int");
            else
                s.error(e->b->pos, "incompatible types: " + s.typeStr(i.type) + " cannot be converted to int");
        }
        lv.kind = LValue::Array;
        lv.obj = std::move(a.node);
        lv.idx = s.coerce(std::move(i), s.prim(TK::Int), e->b->pos);
        lv.type = at->elem;
        return lv;
    }
    s.error(e->pos, "unexpected type", {"  required: variable", "  found:    value"});
    return lv;
}
} // namespace

namespace
{
// Wraps the lvalue's update: the old value (as the first evaluated operand of
// `upd`) is `oldFor()`; the result is the expression storing f(old).
struct Updater
{
    Sema &s;
    LValue &lv;
    int l;
    // A node reading the current value inside an update expression.
    ASTNodePtr oldNode()
    {
        switch (lv.kind)
        {
        case LValue::Local:
        case LValue::Static: return q::id(lv.name, l);
        default: return q::old(l);
        }
    }
    // Stores `upd` (built with oldNode()).
    ASTNodePtr store(ASTNodePtr upd)
    {
        switch (lv.kind)
        {
        case LValue::Local: return q::assign(q::id(lv.name, l), std::move(upd), l);
        case LValue::Static:
        {
            ASTNodePtr a = q::assign(q::id(lv.name, l), std::move(upd), l);
            if (lv.clinitOf)
            {
                std::vector<ASTNodePtr> p;
                p.push_back(q::call(lv.clinitOf->clinitFn(), {}, l));
                p.push_back(std::move(a));
                return q::seq(std::move(p), l);
            }
            return a;
        }
        case LValue::Field: return q::fupd(std::move(lv.obj), lv.key, std::move(upd), l);
        case LValue::Array: return q::aupd(std::move(lv.obj), std::move(lv.idx), std::move(upd), l);
        default: return upd;
        }
    }
    ASTNodePtr plainStore(ASTNodePtr v)
    {
        switch (lv.kind)
        {
        case LValue::Local:
        case LValue::Static: return store(std::move(v));
        case LValue::Field: return q::putf(std::move(lv.obj), lv.key, std::move(v), l);
        case LValue::Array: return q::astore(std::move(lv.obj), std::move(lv.idx), std::move(v), l);
        default: return v;
        }
    }
};
} // namespace

Val Sema::assign(const ExprP &e)
{
    int l = ln(e);
    const std::string &op = e->name;
    Pos at = e->opPos.line ? e->opPos : e->pos;
    LValue lv = makeLValue(*this, e->a);
    Val r;
    r.type = lv.type ? lv.type : tError();
    if (lv.kind == LValue::Local && lv.local)
    {
        // a final local may be assigned once, when declared without a value
        if (lv.local->isFinal && (op != "=" || lv.local->hasInit || lv.local->assignedLater))
            error(e->a->pos, "cannot assign a value to final variable " + lv.local->name);
        if (op != "=" && !isAssigned(lv.local, lv.local->fn))
            error(e->a->pos, "variable " + lv.local->name + " might not have been initialized");
        if (!probing_)
        {
            lv.local->assignedLater = true;
            ++lv.local->assignCount;
        }
    }
    Updater u{*this, lv, l};
    if (op == "=")
    {
        Val v = expr(e->b, lv.type);
        ASTNodePtr value = lv.kind == LValue::Error ? std::move(v.node) : coerce(std::move(v), lv.type, e->b->pos);
        if (lv.kind == LValue::Error)
        {
            r.node = std::move(value);
            return r;
        }
        r.node = u.plainStore(std::move(value));
        if (lv.kind == LValue::Local && lv.local)
            markAssigned(lv.local);
        return r;
    }
    // compound: T x = (T)(x op y), x evaluated once
    std::string bop = op.substr(0, op.size() - 1);
    Val rhs = expr(e->b);
    if (lv.kind == LValue::Error || isError(lv.type) || isError(rhs.type))
    {
        r.node = std::move(rhs.node);
        return r;
    }
    ASTNodePtr updated;
    TypeP lt = lv.type;
    if (bop == "+" && isStringType(lt))
    {
        Val old;
        old.node = u.oldNode();
        old.type = lt;
        ASTNodePtr so = stringOf(std::move(old), l);
        updated = q::bin(SCAT, std::move(so), stringOf(std::move(rhs), l), l);
    }
    else
    {
        bool shift = bop == "<<" || bop == ">>" || bop == ">>>";
        bool logical = (bop == "&" || bop == "|" || bop == "^") && isBooleanish(lt) && isBooleanish(rhs.type);
        TypeP pt = logical ? prim(TK::Boolean) : shift ? unaryPromote(lt) : binaryPromote(lt, rhs.type);
        if (isError(pt) || (shift && (!isIntegral(pt) || !isIntegral(unaryPromote(rhs.type)))) ||
            ((bop == "&" || bop == "|" || bop == "^") && !logical && !isIntegral(pt)))
        {
            error(at, "bad operand types for binary operator '" + bop + "'",
                  {"  first type:  " + typeStr(lt), "  second type: " + typeStr(rhs.type)});
            r.node = std::move(rhs.node);
            return r;
        }
        Val old;
        old.node = u.oldNode();
        old.type = lt;
        ASTNodePtr x = coerce(std::move(old), pt, e->a->pos);
        int code;
        ASTNodePtr y;
        if (logical)
        {
            y = coerce(std::move(rhs), pt, e->b->pos);
            code = bop == "&" ? BAND : bop == "|" ? BOR : BXOR;
        }
        else if (shift)
        {
            TypeP rt = unaryPromote(rhs.type);
            y = coerce(std::move(rhs), rt, e->b->pos);
            bool isLong = pt->k == TK::Long;
            code = bop == "<<" ? (isLong ? LSHL : ISHL) : bop == ">>" ? (isLong ? LSHR : ISHR) : (isLong ? LUSHR : IUSHR);
        }
        else if (bop == "&" || bop == "|" || bop == "^")
        {
            y = coerce(std::move(rhs), pt, e->b->pos);
            bool isLong = pt->k == TK::Long;
            code = bop == "&" ? (isLong ? LAND : IAND) : bop == "|" ? (isLong ? LOR : IOR) : (isLong ? LXOR : IXOR);
        }
        else
        {
            y = coerce(std::move(rhs), pt, e->b->pos);
            code = arithOp(bop, pt->k);
        }
        ASTNodePtr res = q::bin(code, std::move(x), std::move(y), l);
        // implicit narrowing back to the variable's type
        Val rv;
        rv.node = std::move(res);
        rv.type = pt;
        if (isPrim(lt))
            updated = coerce(std::move(rv), lt, e->pos, true, false);
        else
        {
            // a boxed variable: unboxed above, boxed again
            TypeP ub = unboxed(lt);
            Val nv;
            nv.node = ub ? convertPrim(std::move(rv.node), pt, ub, l) : std::move(rv.node);
            nv.type = ub ? ub : pt;
            updated = coerce(std::move(nv), lt, e->pos, false, false);
        }
    }
    r.node = u.store(std::move(updated));
    return r;
}

Val Sema::incdec(const ExprP &e)
{
    int l = ln(e);
    bool inc = e->name == "++";
    bool post = e->k == EK::PostIncDec;
    LValue lv = makeLValue(*this, e->a);
    Val r;
    r.type = lv.type ? lv.type : tError();
    if (lv.kind == LValue::Error)
    {
        r.node = q::nil(l);
        return r;
    }
    if (lv.kind == LValue::Local && lv.local)
    {
        if (lv.local->isFinal)
            error(e->a->pos, "cannot assign a value to final variable " + lv.local->name);
        if (!isAssigned(lv.local, lv.local->fn))
            error(e->a->pos, "variable " + lv.local->name + " might not have been initialized");
        if (!probing_)
        {
            lv.local->assignedLater = true;
            ++lv.local->assignCount;
        }
    }
    TypeP lt = lv.type;
    TypeP nt = numericUnboxed(lt);
    if (!nt)
    {
        if (!isError(lt))
            error(e->pos, "bad operand type " + typeStr(lt) + " for unary operator '" + e->name + "'");
        r.node = q::nil(l);
        return r;
    }
    TypeP pt = unaryPromote(nt);
    Updater u{*this, lv, l};
    auto bump = [&](ASTNodePtr oldv, int sign) -> ASTNodePtr
    {
        Val old;
        old.node = std::move(oldv);
        old.type = lt;
        ASTNodePtr x = coerce(std::move(old), pt, e->pos);
        ASTNodePtr one = pt->k == TK::Float || pt->k == TK::Double ? q::num(1.0, l) : q::lng(1, l);
        ASTNodePtr res = q::bin(arithOp(sign > 0 ? "+" : "-", pt->k), std::move(x), std::move(one), l);
        Val rv;
        rv.node = convertPrim(std::move(res), pt, nt, l);
        rv.type = nt;
        return coerce(std::move(rv), lt, e->pos, false, false);
    };
    bool floating = nt->k == TK::Float || nt->k == TK::Double;
    if (!post)
    {
        r.node = u.store(bump(u.oldNode(), inc ? 1 : -1));
        return r;
    }
    if (!floating && isPrim(lt))
    {
        // the old value of an integral variable: the new one, stepped back
        ASTNodePtr stored = u.store(bump(u.oldNode(), inc ? 1 : -1));
        Val nv;
        nv.node = std::move(stored);
        nv.type = lt;
        ASTNodePtr back = q::bin(arithOp(inc ? "-" : "+", pt->k), coerce(std::move(nv), pt, e->pos), q::lng(1, l), l);
        r.node = convertPrim(std::move(back), pt, nt, l);
        return r;
    }
    // general case: through temporaries
    std::string tv = temp();
    std::vector<ASTNodePtr> parts;
    switch (lv.kind)
    {
    case LValue::Local:
    case LValue::Static:
        parts.push_back(q::assign(q::id(tv, l), q::id(lv.name, l), l));
        parts.push_back(u.store(bump(q::id(tv, l), inc ? 1 : -1)));
        break;
    case LValue::Field:
    {
        std::string to = temp();
        parts.push_back(q::assign(q::id(to, l), std::move(lv.obj), l));
        parts.push_back(q::assign(q::id(tv, l), q::getf(q::id(to, l), lv.key, l), l));
        parts.push_back(q::putf(q::id(to, l), lv.key, bump(q::id(tv, l), inc ? 1 : -1), l));
        break;
    }
    case LValue::Array:
    {
        std::string ta = temp(), ti = temp();
        parts.push_back(q::assign(q::id(ta, l), std::move(lv.obj), l));
        parts.push_back(q::assign(q::id(ti, l), std::move(lv.idx), l));
        parts.push_back(q::assign(q::id(tv, l), q::aload(q::id(ta, l), q::id(ti, l), l), l));
        parts.push_back(q::astore(q::id(ta, l), q::id(ti, l), bump(q::id(tv, l), inc ? 1 : -1), l));
        break;
    }
    default: break;
    }
    parts.push_back(q::id(tv, l));
    r.node = q::seq(std::move(parts), l);
    return r;
}

// ─── Conditional, cast, instanceof ───────────────────────────────────────────

Val Sema::conditional(const ExprP &e, const TypeP &target)
{
    int l = ln(e);
    ASTNodePtr c = cond(e->a);
    Val a = expr(e->b, target);
    Val b = expr(e->c, target);
    Val r;
    TypeP at = a.type, bt = b.type;
    if (isError(at) || isError(bt))
        r.type = isError(at) ? bt : at;
    else if (at->k == TK::Void || bt->k == TK::Void)
    {
        error(at->k == TK::Void ? e->b->pos : e->c->pos, "'void' type not allowed here");
        r.type = tError();
    }
    else if (isBooleanish(at) && isBooleanish(bt) && (isPrim(at) || isPrim(bt)))
        r.type = prim(TK::Boolean);
    else if (numericUnboxed(at) && numericUnboxed(bt) && (isPrim(at) || isPrim(bt) || (unboxed(at) && unboxed(bt))))
    {
        TypeP na = numericUnboxed(at), nb = numericUnboxed(bt);
        // int constant fitting the other operand's narrower type keeps that type
        auto fits = [&](const Val &v, TK k) {
            Const cc;
            cc.k = Const::Int;
            cc.i = v.cv.i;
            return v.cv.k == Const::Int && assignable(prim(TK::Int), prim(k), &cc);
        };
        if (sameType(na, nb))
            r.type = na;
        else if ((nb->k == TK::Byte || nb->k == TK::Short || nb->k == TK::Char) && fits(a, nb->k))
            r.type = nb;
        else if ((na->k == TK::Byte || na->k == TK::Short || na->k == TK::Char) && fits(b, na->k))
            r.type = na;
        else
            r.type = binaryPromote(na, nb);
    }
    else if (target && isRef(target) && (e->b->k == EK::Lambda || e->c->k == EK::Lambda || e->b->k == EK::MethodRef))
        r.type = target;
    else
        r.type = lub(at, bt);
    ASTNodePtr x = coerce(std::move(a), r.type, e->b->pos);
    ASTNodePtr y = coerce(std::move(b), r.type, e->c->pos);
    r.node = q::ternary(std::move(c), std::move(x), std::move(y), l);
    return r;
}

Val Sema::castExpr(const ExprP &e)
{
    TypeP to = resolveType(e->type);
    Val v = expr(e->a, to);
    Val r;
    r.type = to;
    if (isError(v.type) || isError(to))
    {
        r.node = std::move(v.node);
        return r;
    }
    if (!castable(v.type, to))
    {
        error(e->pos, "incompatible types: " + typeStr(v.type) + " cannot be converted to " + typeStr(to));
        r.node = std::move(v.node);
        return r;
    }
    if (v.cv.ok() && (isPrim(to) || isStringType(to)))
    {
        Const c = isPrim(to) ? convertConst(v.cv, to->k) : v.cv;
        if (c.ok())
        {
            r.cv = c;
            r.node = constNode(c, e->pos.line);
            return r;
        }
    }
    r.node = coerce(std::move(v), to, e->pos, true);
    return r;
}

Val Sema::instanceOf(const ExprP &e)
{
    int l = ln(e);
    Val v = expr(e->a);
    TypeP t = resolveType(e->type);
    Val r;
    r.type = prim(TK::Boolean);
    if (isError(v.type) || isError(t))
    {
        r.node = q::boolean(false, l);
        return r;
    }
    if (isPrim(v.type))
    {
        error(e->a->pos, "unexpected type", {"  required: reference", "  found:    " + typeStr(v.type)});
        r.node = q::boolean(false, l);
        return r;
    }
    if (!castable(v.type, t))
    {
        error(e->opPos.line ? e->opPos : e->pos,
              "incompatible types: " + typeStr(v.type) + " cannot be converted to " + typeStr(t));
        r.node = q::boolean(false, l);
        return r;
    }
    std::string desc = descriptor(t);
    if (e->bindName.empty())
    {
        r.node = q::instOf(std::move(v.node), desc, l);
        return r;
    }
    // pattern: `v instanceof T x` binds x when it matches
    std::string tv = temp();
    LocalVar &x = declareLocal(e->bindName, t, e->pos, false);
    x.hasInit = true;
    markAssigned(&x);
    x.q = e->bindName + "$p" + std::to_string(++tempCounter_);
    fn_->temps.push_back(x.q);
    std::vector<ASTNodePtr> parts;
    parts.push_back(q::assign(q::id(tv, l), std::move(v.node), l));
    std::vector<ASTNodePtr> bind;
    bind.push_back(q::assign(q::id(x.q, l), q::id(tv, l), l));
    bind.push_back(q::boolean(true, l));
    parts.push_back(q::ternary(q::instOf(q::id(tv, l), desc, l), q::seq(std::move(bind), l), q::boolean(false, l), l));
    r.node = q::seq(std::move(parts), l);
    return r;
}

// ─── Arrays ──────────────────────────────────────────────────────────────────

Val Sema::arrayLit(const ExprP &e, const TypeP &arrType)
{
    int l = ln(e);
    TypeP elem = arrType->elem;
    std::vector<ASTNodePtr> elems;
    for (auto &x : e->args)
    {
        if (x->k == EK::ArrayLit)
        {
            if (!elem || elem->k != TK::Array)
            {
                error(x->pos, "illegal initializer for " + typeStr(elem));
                continue;
            }
            elems.push_back(std::move(arrayLit(x, elem).node));
        }
        else
            elems.push_back(exprTo(x, elem, x->pos));
    }
    Val r;
    r.type = arrType;
    r.node = q::arr(std::move(elems), l);
    return r;
}

Val Sema::newArray(const ExprP &e, const TypeP &target)
{
    (void)target;
    int l = ln(e);
    TypeP base = resolveType(e->type);
    int dims = (int)e->args.size() + e->extraDims;
    TypeP full = base;
    for (int i = 0; i < dims; ++i)
        full = arrayOf(full);
    if (e->a)
        return arrayLit(e->a, full);
    if (isError(base))
        return Val{q::nil(l), tError()};
    // arrays of a parameterized type need unbounded wildcards (new List<?>[n])
    bool reifiable = base->k != TK::TypeVar;
    if (base->k == TK::Class)
        for (auto &a : base->args)
            reifiable = reifiable && a && a->k == TK::Wildcard && a->wild == 0;
    if (!reifiable)
        error(e->pos, "generic array creation");
    std::vector<ASTNodePtr> sizes;
    for (auto &d : e->args)
    {
        Val v = expr(d);
        TypeP pt = unaryPromote(v.type);
        if (!isError(v.type) && (isError(pt) || pt->k != TK::Int))
        {
            if (pt && pt->k == TK::Long)
                error(d->pos, "incompatible types: possible lossy conversion from long to int");
            else
                error(d->pos, "incompatible types: " + typeStr(v.type) + " cannot be converted to int");
        }
        sizes.push_back(coerce(std::move(v), prim(TK::Int), d->pos));
    }
    // the innermost created dimension holds its element type's default
    TypeP innerElem = base;
    for (int i = 0; i < e->extraDims; ++i)
        innerElem = arrayOf(innerElem);
    ASTNodePtr def = e->extraDims > 0 ? q::nil(l) : defaultValue(base, l);
    Val r;
    r.type = full;
    if (sizes.size() == 1)
    {
        std::vector<ASTNodePtr> a;
        a.push_back(std::move(sizes[0]));
        a.push_back(std::move(def));
        r.node = q::call("__j_newarray", std::move(a), l);
        return r;
    }
    std::vector<ASTNodePtr> a;
    a.push_back(q::arr(std::move(sizes), l));
    a.push_back(std::move(def));
    a.push_back(q::lng((int64_t)e->args.size(), l));
    r.node = q::call("__j_newarray_multi", std::move(a), l);
    return r;
}

Val Sema::index(const ExprP &e)
{
    int l = ln(e);
    Val a = expr(e->a);
    Val i = expr(e->b);
    if (isError(a.type))
        return Val{q::nil(l), tError()};
    TypeP at = upperBound(a.type);
    if (at->k != TK::Array)
    {
        error(e->opPos.line ? e->opPos : e->pos, "array required, but " + typeStr(a.type) + " found");
        return Val{q::nil(l), tError()};
    }
    TypeP it = unaryPromote(i.type);
    if (!isError(i.type) && (isError(it) || it->k != TK::Int))
    {
        if (it && it->k == TK::Long)
            error(e->b->pos, "incompatible types: possible lossy conversion from long to int");
        else
            error(e->b->pos, "incompatible types: " + typeStr(i.type) + " cannot be converted to int");
    }
    Val r;
    r.type = at->elem;
    r.node = q::aload(std::move(a.node), coerce(std::move(i), prim(TK::Int), e->b->pos), l);
    return r;
}

// ─── this, class literals ────────────────────────────────────────────────────

Val Sema::thisExpr(const ExprP &e)
{
    int l = ln(e);
    Val r;
    if (e->type)
    {
        // Outer.this
        TypeP t = resolveType(e->type);
        if (isError(t) || !t->cls)
            return Val{q::nil(l), tError()};
        r.type = selfType(t->cls);
        r.node = outerInstance(t->cls, e->pos);
        return r;
    }
    if (inStaticContext())
    {
        error(e->pos, "non-static variable this cannot be referenced from a static context");
        return Val{q::nil(l), tError()};
    }
    ClassSym *c = curClass();
    std::vector<TypeP> args;
    for (auto *tv : c->tparams)
        args.push_back(typeVar(tv));
    r.type = classType(c, args);
    r.node = thisNode(l);
    return r;
}

Val Sema::classLit(const ExprP &e)
{
    int l = ln(e);
    TypeP t = resolveType(e->type);
    ClassSym *k = cls("java.lang.Class");
    needNamed("java.lang.Class", "$named");
    std::string name;
    if (t->k == TK::Class && t->cls)
        name = t->cls->binaryName;
    else if (t->k == TK::Array)
        name = "[" + typeStr(t->elem);
    else
        name = typeStr(t);
    std::string simple = t->k == TK::Class && t->cls ? t->cls->name : typeStr(t);
    Val r;
    r.type = classType(k, {isPrim(t) ? boxOf(t) : t});
    std::vector<ASTNodePtr> a;
    a.push_back(q::str(name, l));
    a.push_back(q::str(simple, l));
    r.node = q::call("java.lang.Class.$named", std::move(a), l);
    return r;
}

} // namespace java
