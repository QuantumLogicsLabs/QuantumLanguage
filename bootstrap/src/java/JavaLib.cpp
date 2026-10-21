// Library calls lowered to runtime operations directly instead of through
// their prelude bodies: console output, String.format / printf, String
// conversions, String.length / charAt, StringBuilder.append.
#include "JavaSema.h"

namespace java
{

using namespace jop;

namespace
{
// The kind letter java.util.Formatter needs for an argument's static type.
char formatKind(Sema &s, const TypeP &t)
{
    if (!t)
        return 'O';
    TypeP u = Sema::isPrim(t) ? t : s.unboxed(t);
    if (u)
        switch (u->k)
        {
        case TK::Byte:
        case TK::Short:
        case TK::Int: return 'I';
        case TK::Long: return 'J';
        case TK::Char: return 'C';
        case TK::Double: return 'D';
        case TK::Float: return 'F';
        case TK::Boolean: return 'Z';
        default: break;
        }
    if (s.isStringType(t))
        return 'S';
    return 'O';
}

bool isCharArray(const TypeP &t) { return t && t->k == TK::Array && t->elem && t->elem->k == TK::Char; }
} // namespace

// printf / format: (format, [args...], "kinds")
ASTNodePtr Sema::formatCall(std::vector<Val> &args, size_t fmtIndex, int l)
{
    std::vector<ASTNodePtr> a;
    a.push_back(std::move(args[fmtIndex].node));
    std::string kinds;
    std::vector<ASTNodePtr> rest;
    for (size_t i = fmtIndex + 1; i < args.size(); ++i)
    {
        kinds += formatKind(*this, args[i].type);
        rest.push_back(std::move(args[i].node));
    }
    a.push_back(q::arr(std::move(rest), l));
    a.push_back(q::str(kinds, l));
    return q::call("__j_format", std::move(a), l);
}

ASTNodePtr Sema::printCall(const std::string &native, std::vector<Val> &args, int l)
{
    std::vector<ASTNodePtr> a;
    if (!args.empty())
    {
        Val &v = args[0];
        if (isCharArray(v.type))
        {
            std::vector<ASTNodePtr> ca;
            ca.push_back(std::move(v.node));
            a.push_back(q::call("__jn_String_valueOfChars", std::move(ca), l));
        }
        else
            a.push_back(stringOf(std::move(v), l));
    }
    return q::call(native, std::move(a), l);
}

bool Sema::libraryCall(MethodSym *m, std::vector<ASTNodePtr> &args, ASTNodePtr &recv, const std::vector<Val> &argVals,
                       const TypeP &recvType, int l, Val &out)
{
    (void)recvType;
    const std::string &owner = m->owner->binaryName;
    const std::string &n = m->name;
    // System.out / System.err
    if (owner == "java.io.PrintStream" && recv && recv->is<Identifier>())
    {
        const std::string &g = recv->as<Identifier>().name;
        bool isOut = g == "java.lang.System::out", isErr = g == "java.lang.System::err";
        if (isOut || isErr)
        {
            std::vector<Val> vals;
            for (size_t i = 0; i < args.size(); ++i)
            {
                Val v;
                v.node = std::move(args[i]);
                v.type = i < argVals.size() ? argVals[i].type : nullptr;
                vals.push_back(std::move(v));
            }
            if (n == "println" || n == "print")
            {
                bool ln = n == "println";
                out.type = prim(TK::Void);
                out.node = printCall(isOut ? (ln ? "__j_println" : "__j_print") : (ln ? "__j_eprintln" : "__j_eprint"), vals, l);
                return true;
            }
            if ((n == "printf" || n == "format") && !vals.empty())
            {
                // printf(String, Object...) arrives with the varargs packed: unpack them
                std::vector<Val> flat;
                size_t fmtAt = 0;
                if (!vals.empty() && vals[0].type && vals[0].type->k == TK::Class && vals[0].type->cls &&
                    vals[0].type->cls->binaryName == "java.util.Locale")
                    fmtAt = 1; // printf(Locale, String, Object...)
                for (size_t i = 0; i <= fmtAt && i < vals.size(); ++i)
                    flat.push_back(std::move(vals[i]));
                ASTNodePtr packed = vals.size() > fmtAt + 1 ? std::move(vals[fmtAt + 1].node) : nullptr;
                ASTNodePtr fmtNode;
                std::vector<ASTNodePtr> a;
                a.push_back(std::move(flat[fmtAt].node));
                std::string kinds;
                // the actual argument types (after the format) come from argVals
                for (size_t i = fmtAt + 1; i < argVals.size(); ++i)
                    kinds += formatKind(*this, argVals[i].type);
                a.push_back(packed ? std::move(packed) : q::arr({}, l));
                a.push_back(q::str(kinds, l));
                std::vector<ASTNodePtr> pa;
                pa.push_back(q::call("__j_format", std::move(a), l));
                out.type = classType(m->owner);
                std::vector<ASTNodePtr> seq;
                seq.push_back(q::call(isOut ? "__j_print" : "__j_eprint", std::move(pa), l));
                seq.push_back(q::id(g, l));
                out.node = q::seq(std::move(seq), l);
                return true;
            }
            if (n == "flush")
            {
                out.type = prim(TK::Void);
                out.node = q::call("__j_flush", {}, l);
                return true;
            }
            // anything else: the PrintStream object itself
            for (size_t i = 0; i < vals.size(); ++i)
                args[i] = std::move(vals[i].node);
            return false;
        }
    }
    if (owner == "java.lang.String")
    {
        if (n == "length" && args.empty() && recv)
        {
            out.type = prim(TK::Int);
            out.node = q::un(SLEN, std::move(recv), l);
            return true;
        }
        if (n == "charAt" && args.size() == 1 && recv)
        {
            out.type = prim(TK::Char);
            out.node = q::bin(CHARAT, std::move(recv), std::move(args[0]), l);
            return true;
        }
        if ((n == "format" || n == "formatted") && m->varargs)
        {
            // String.format(fmt, args...) / fmt.formatted(args...)
            std::vector<ASTNodePtr> a;
            std::string kinds;
            size_t first = n == "formatted" ? 0 : 1;
            bool locale = n == "format" && !argVals.empty() && argVals[0].type && argVals[0].type->k == TK::Class &&
                          argVals[0].type->cls && argVals[0].type->cls->binaryName == "java.util.Locale";
            if (locale)
                first = 2;
            if (n == "formatted")
                a.push_back(std::move(recv));
            else
                a.push_back(std::move(args[locale ? 1 : 0]));
            for (size_t i = first; i < argVals.size(); ++i)
                kinds += formatKind(*this, argVals[i].type);
            a.push_back(std::move(args.back()));
            a.push_back(q::str(kinds, l));
            out.type = tString();
            out.node = q::call("__j_format", std::move(a), l);
            return true;
        }
        if (n == "valueOf" && m->isStatic() && args.size() == 1 && !argVals.empty())
        {
            Val v;
            v.node = std::move(args[0]);
            v.type = argVals[0].type;
            out.type = tString();
            if (isCharArray(v.type))
            {
                std::vector<ASTNodePtr> a;
                a.push_back(std::move(v.node));
                out.node = q::call("__jn_String_valueOfChars", std::move(a), l);
            }
            else
                out.node = stringOf(std::move(v), l);
            return true;
        }
    }
    if (owner == "java.lang.StringBuilder" && n == "append" && args.size() == 1 && recv && !argVals.empty())
    {
        Val v;
        v.node = std::move(args[0]);
        v.type = argVals[0].type;
        std::vector<ASTNodePtr> a;
        a.push_back(std::move(recv));
        if (isCharArray(v.type))
        {
            std::vector<ASTNodePtr> ca;
            ca.push_back(std::move(v.node));
            a.push_back(q::call("__jn_String_valueOfChars", std::move(ca), l));
        }
        else
            a.push_back(stringOf(std::move(v), l));
        out.type = classType(m->owner);
        out.node = q::call("__jn_StringBuilder_append", std::move(a), l);
        return true;
    }
    return false;
}

} // namespace java
