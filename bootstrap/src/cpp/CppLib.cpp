// Standard library for the typed C/C++ front-end: names (std::cout, endl,
// npos, ...), free functions (<algorithm>, <cmath>, <cctype>, <string>
// conversions, C stdio/string), methods of library types and their
// constructors. Everything lowers to __cpp_* runtime natives (VmCpp.cpp).
#include "CppLower.h"

#include <cmath>
#include <functional>

namespace cpp
{

// The C standard header declaring library function `fn`, or nullptr when C
// has no such function (a C++-only name like max / to_string / getline).
const char *cLibraryHeader(const std::string &fn)
{
    static const std::map<std::string, const char *> h = [] {
        std::map<std::string, const char *> m;
        auto add = [&](const char *header, std::initializer_list<const char *> names) {
            for (const char *n : names)
                m[n] = header;
        };
        add("stdio.h", {"printf", "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf", "vsprintf",
                        "vsnprintf", "scanf", "fscanf", "sscanf", "getchar", "putchar", "puts", "fputs",
                        "fgets", "gets", "fgetc", "fputc", "getc", "putc", "ungetc", "fopen", "fclose",
                        "fflush", "fread", "fwrite", "fseek", "ftell", "rewind", "feof", "ferror", "clearerr",
                        "remove", "rename", "tmpfile", "perror", "setbuf", "setvbuf", "freopen"});
        add("stdlib.h", {"malloc", "calloc", "realloc", "free", "abs", "labs", "llabs", "div", "ldiv",
                         "atoi", "atol", "atoll", "atof", "strtol", "strtoll", "strtoul", "strtoull", "strtod",
                         "strtof", "strtold", "rand", "srand", "qsort", "bsearch", "exit", "_Exit", "abort",
                         "atexit", "system", "getenv", "aligned_alloc"});
        add("string.h", {"strlen", "strcpy", "strncpy", "strcat", "strncat", "strcmp", "strncmp", "strchr",
                         "strrchr", "strstr", "strtok", "memcpy", "memmove", "memset", "memcmp", "memchr",
                         "strdup", "strndup", "strspn", "strcspn", "strpbrk", "strerror", "strcoll",
                         "strxfrm", "strnlen"});
        add("ctype.h", {"isalpha", "isdigit", "isalnum", "isspace", "isupper", "islower", "ispunct",
                        "isprint", "isxdigit", "iscntrl", "isgraph", "isblank", "toupper", "tolower"});
        add("math.h", {"sqrt", "cbrt", "exp", "exp2", "log", "log2", "log10", "log1p", "expm1", "sin", "cos",
                       "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh",
                       "floor", "ceil", "round", "trunc", "fabs", "nearbyint", "rint", "erf", "erfc",
                       "tgamma", "lgamma", "sqrtf", "floorf", "ceilf", "fabsf", "roundf", "sinf", "cosf",
                       "powf", "pow", "atan2", "fmod", "hypot", "fmax", "fmin", "copysign", "remainder",
                       "fdim", "lround", "llround", "lrint", "llrint", "isnan", "isinf", "isfinite",
                       "signbit", "isnormal", "frexp", "ldexp", "modf", "fma"});
        add("time.h", {"time", "clock", "difftime", "localtime", "gmtime", "strftime", "mktime", "asctime",
                       "ctime"});
        add("assert.h", {"assert"});
        add("stdarg.h", {"va_start", "va_arg", "va_end", "va_copy"});
        add("setjmp.h", {"setjmp", "longjmp"});
        add("signal.h", {"signal", "raise"});
        add("unistd.h", {"sleep", "usleep", "getpid"});
        add("conio.h", {"getch", "getche", "kbhit"});
        return m;
    }();
    auto it = h.find(fn);
    return it == h.end() ? nullptr : it->second;
}

namespace
{
std::vector<ASTNodePtr> args1(ASTNodePtr a)
{
    std::vector<ASTNodePtr> v;
    v.push_back(std::move(a));
    return v;
}
std::vector<ASTNodePtr> args2(ASTNodePtr a, ASTNodePtr b)
{
    std::vector<ASTNodePtr> v;
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    return v;
}
std::vector<ASTNodePtr> args3(ASTNodePtr a, ASTNodePtr b, ASTNodePtr c)
{
    std::vector<ASTNodePtr> v;
    v.push_back(std::move(a));
    v.push_back(std::move(b));
    v.push_back(std::move(c));
    return v;
}
TP manipT()
{
    static TP t = mkT(T::Dyn, "manip");
    return t;
}
TP iterOf(const TP &container)
{
    auto it = mkT(T::Iter);
    it->a.push_back(container);
    return it;
}
} // namespace

ASTNodePtr Lowerer::streamKind(const TP &t)
{
    std::string k = "?";
    switch (t->k)
    {
    case T::Int: case T::Enum: k = isI64(t) ? (t->isUnsigned ? "L" : "l") : t->isUnsigned ? "u" : "i"; break;
    case T::Float: k = t->name == "float" ? "f" : "d"; break;
    case T::Char: k = "c"; break;
    case T::Bool: k = "b"; break;
    case T::Str: k = isCBuf(t) ? "z" : "s"; break;
    case T::CStr: k = "z"; break; // a C string: up to its NUL
    case T::Ptr: case T::UPtr: case T::Nullptr: k = "p"; break;
    case T::Dyn: k = t->name == "manip" ? "m" : t->name == "rdbuf" ? "r" : "?"; break;
    case T::Iter: k = "I"; break;
    default: break;
    }
    return q::str(k, line_);
}

// ─── Names ─────────────────────────────────────────────────────────────────

XV Lowerer::libName(const std::vector<std::string> &nameIn, int line, bool &found)
{
    found = true;
    if (prog_.cMode && !(nameIn.size() == 1 && (nameIn[0] == "stdout" || nameIn[0] == "stderr" ||
                                                 nameIn[0] == "stdin")))
    {
        found = false; // C's library has no streams, manipulators or npos
        return XV();
    }
    std::vector<std::string> name = nameIn;
    bool qualified = !name.empty() && name[0] == "std";
    if (qualified)
        name.erase(name.begin());
    std::string n;
    for (size_t i = 0; i < name.size(); ++i)
        n += (i ? "::" : "") + name[i];
    bool cGlobal = n == "stdout" || n == "stderr" || n == "stdin";
    if (!prog_.cMode && !qualified && !cGlobal && !name.empty() && !stdVisible(name[0], currentScope()))
    {
        found = false; // std's, but no `using namespace std` / `using std::...` here
        return XV();
    }
    if (!prog_.cMode && (n == "cout" || n == "cerr" || n == "clog" || n == "cin" || n == "endl"))
    {
        static const char *io[] = {"iostream", "ostream", "istream", "fstream", "sstream", "bits/stdc++.h"};
        bool have = false;
        for (const char *h : io)
            have = have || prog_.systemHeaders.count(h) > 0;
        if (!have)
        {
            if (qualified)
                unsupported("'" + n + "' is not a member of 'std'", line);
            found = false;
            return XV();
        }
    }
    if (n == "cout" || n == "cerr" || n == "clog")
        return mkxv(q::id(n == "cout" ? "__cpp_cout" : "__cpp_cerr", line), mkT(T::Stream, "ostream"), false);
    if (n == "cin")
        return mkxv(q::id("__cpp_cin", line), mkT(T::Stream, "istream"), false);
    static const std::set<std::string> manips = {
        "endl", "flush", "ends", "fixed", "scientific", "defaultfloat", "hexfloat", "left", "right", "internal",
        "boolalpha", "noboolalpha", "showpoint", "noshowpoint", "showpos", "noshowpos", "uppercase",
        "nouppercase", "hex", "dec", "oct", "skipws", "noskipws", "unitbuf", "nounitbuf", "showbase",
        "noshowbase", "ws"};
    if (manips.count(n))
    {
        std::vector<ASTNodePtr> a;
        a.push_back(q::str(n, line));
        return mkxv(q::call("__cpp_manip__", std::move(a), line), manipT());
    }
    if (n == "string::npos" || n == "npos" || n == "wstring::npos" || n == "string_view::npos")
        return mkxv(q::num(18446744073709551615.0, line), tSize());
    if (n == "nullopt")
        return mkxv(q::nil(line), mkT(T::Nullopt));
    if (n == "stdout" || n == "stderr" || n == "stdin")
        return mkxv(q::str(n, line), mkT(T::Dyn, "FILE"));
    if (n == "ios::app" || n == "ios_base::app")
        return mkxv(q::num(1, line), tInt());
    static const std::set<std::string> iosFlags = {
        "fixed", "scientific", "floatfield", "showpoint", "showpos", "boolalpha", "left", "right", "internal",
        "adjustfield", "uppercase", "hex", "dec", "oct", "basefield", "showbase", "skipws", "unitbuf"};
    if (n.rfind("ios::", 0) == 0 || n.rfind("ios_base::", 0) == 0)
    {
        // ios::fixed ...: the manipulator (setf / unsetf apply it)
        std::string f = n.substr(n.find("::") + 2);
        if (iosFlags.count(f))
        {
            std::vector<ASTNodePtr> a;
            a.push_back(q::str(f, line));
            return mkxv(q::call("__cpp_manip__", std::move(a), line), manipT());
        }
    }
    if (n == "ios::out" || n == "ios::in" || n == "ios::binary" || n == "ios::trunc" || n == "ios_base::out" ||
        n == "ios_base::in" || n == "ios_base::binary" || n == "ios::ate")
        return mkxv(q::num(0, line), tInt());
    found = false;
    return XV();
}

// ─── Constructors of library types ─────────────────────────────────────────

XV Lowerer::constructBuiltinType(const TP &t, std::vector<ExprP> &args, bool braced, int line)
{
    if (braced)
        return listInit(t, args, line);
    auto lower = [&](size_t i, const TP &to = nullptr) -> XV
    {
        XV x = to ? lowerExprTo(args[i], to) : lowerExpr(args[i]);
        return to ? convert(std::move(x), to, line) : std::move(x);
    };
    switch (t->k)
    {
    case T::Str:
    {
        if (args.empty())
            return mkxv(q::str("", line), t);
        XV a = lower(0);
        if (args.size() == 1)
            return convert(std::move(a), t, line);
        XV b = lower(1);
        if (isIntegral(a.t) && (b.t->k == T::Char || isIntegral(b.t)))
            return mkxv(q::call("__cpp_str_repeat__", args2(read(a), read(b)), line), t); // string(n, c)
        if (a.t->k == T::Iter)
            return mkxv(q::call("__cpp_str_range__", args2(read(a), read(b)), line), t);
        if (isStringy(a.t) && args.size() == 2)
        {
            // string(s, pos) for std::string; string(cstr, n) for a C string: the first n
            if (a.t->k == T::CStr)
                return mkxv(q::call("__cpp_substr__", args3(read(a), q::num(0, line), read(b)), line), t);
            return mkxv(q::call("__cpp_substr__", args3(read(a), read(b), q::num(-1, line)), line), t);
        }
        if (isStringy(a.t) && args.size() == 3)
        {
            XV c = lower(2);
            return mkxv(q::call("__cpp_substr__", args3(read(a), read(b), read(c)), line), t);
        }
        break;
    }
    case T::Vector:
    case T::Array:
    {
        TP et = t->a.empty() ? tDyn() : t->a[0];
        if (args.empty())
            return mkxv(defaultValue(t, line), t);
        XV a = lower(0);
        if (args.size() == 1)
        {
            if (isIntegral(a.t))
            {
                std::vector<ASTNodePtr> f;
                f.push_back(read(a));
                f.push_back(defaultValue(et, line));
                f.push_back(planFor(et));
                return mkxv(q::call("__cpp_fill__", std::move(f), line), t);
            }
            if (a.t->k == t->k || a.t->k == T::Vector)
                return mkxv(copyOf(std::move(a), line), t);
            return convert(std::move(a), t, line);
        }
        if (a.t->k == T::Iter || a.t->k == T::Ptr)
        {
            XV b = lower(1);
            return mkxv(q::call("__cpp_range_array__", args2(read(a), read(b)), line), t);
        }
        XV b = lower(1, et);
        std::vector<ASTNodePtr> f;
        f.push_back(read(a));
        f.push_back(copyOf(std::move(b), line));
        f.push_back(planFor(et));
        return mkxv(q::call("__cpp_fill__", std::move(f), line), t);
    }
    case T::Map:
    case T::Set:
    {
        if (args.empty())
            return mkxv(defaultValue(t, line), t);
        XV a = lower(0);
        if (args.size() == 1)
            return mkxv(copyOf(std::move(a), line), t);
        XV b = lower(1);
        std::vector<ASTNodePtr> f;
        f.push_back(q::call("__cpp_range_array__", args2(read(a), read(b)), line));
        f.push_back(q::boolean(t->multi, line));
        return mkxv(q::call(t->k == T::Map ? "__cpp_map_from__" : "__cpp_set_from__", std::move(f), line), t);
    }
    case T::Pair:
    case T::Tuple:
        return listInit(t, args, line);
    case T::Optional:
        if (args.empty())
            return mkxv(q::nil(line), t);
        return convert(lower(0, t->a[0]), t, line);
    case T::Variant:
        if (args.empty())
            return mkxv(defaultValue(t, line), t);
        return convert(lower(0), t, line);
    case T::UPtr:
        if (args.empty())
            return mkxv(q::nil(line), t);
        {
            XV a = lower(0);
            if (t->name == "shared_ptr" && a.t->k == T::UPtr)
                return mkxv(copyOf(std::move(a), line), t); // shared_ptr<T> b(a): one more owner
            if (t->name == "shared_ptr" && a.t->k != T::Nullptr)
                return mkxv(q::call("__cpp_sp_new__", args1(read(a)), line), t); // shared_ptr<T>(new T)
            if (a.t->k == T::UPtr)
                return mkxv(copyOf(std::move(a), line), t);
            a.t = t;
            return a;
        }
    case T::Func:
        if (args.empty())
            return mkxv(q::nil(line), t);
        return convert(lower(0, t), t, line);
    case T::Stream:
    {
        std::vector<ASTNodePtr> f;
        f.push_back(q::str(t->name, line));
        if (!args.empty())
        {
            XV a = lower(0);
            f.push_back(read(a));
        }
        if (args.size() > 1)
        {
            XV b = lower(1);
            f.push_back(read(b));
        }
        return mkxv(q::call("__cpp_stream_new__", std::move(f), line), t);
    }
    default:
        break;
    }
    unsupported("constructing " + t->str(), line);
}

// ─── Free functions ────────────────────────────────────────────────────────

XV Lowerer::libCall(const std::string &fnIn, const ExprP &e, std::vector<ExprP> &args, const std::vector<TArg> &targs,
                    bool &handled)
{
    handled = true;
    int ln = e->line;
    std::string fn = fnIn;
    if (prog_.cMode)
    {
        // A C program has only C's library, and (C99 on, an error since gcc
        // 14) must include the header that declares a function it calls.
        const char *header = cLibraryHeader(fn);
        if (!header)
        {
            handled = false;
            return XV();
        }
        if (!prog_.systemHeaders.count(header))
            unsupported("implicit declaration of function '" + fn + "'; include <" + header + ">", ln);
    }
    std::vector<XV> early; // arguments evaluated ahead (right to left)
    auto lower = [&](size_t i) -> XV
    {
        if (i < early.size() && early[i].t)
            return std::move(early[i]);
        return lowerExpr(args[i]);
    };
    auto lowerTo = [&](size_t i, const TP &t) -> XV
    { return convert(i < early.size() && early[i].t ? std::move(early[i]) : lowerExprTo(args[i], t), t, ln); };
    auto targType = [&](size_t i) -> TP
    { return i < targs.size() && targs[i].type ? resolveType(targs[i].type, currentScope()) : nullptr; };
    auto need = [&](size_t n)
    {
        if (args.size() < n)
            unsupported(fn + ": too few arguments", ln);
    };

    // <cmath>
    static const std::set<std::string> math1 = {
        "sqrt", "cbrt", "exp", "exp2", "log", "log2", "log10", "log1p", "expm1", "sin", "cos", "tan", "asin",
        "acos", "atan", "sinh", "cosh", "tanh", "asinh", "acosh", "atanh", "floor", "ceil", "round", "trunc",
        "fabs", "nearbyint", "rint", "erf", "erfc", "tgamma", "lgamma", "sqrtf", "floorf", "ceilf", "fabsf",
        "roundf", "sinf", "cosf", "powf"};
    static const std::set<std::string> math2 = {"pow", "atan2", "fmod", "hypot", "fmax", "fmin", "copysign",
                                                "remainder", "fdim"};
    if (math1.count(fn) || math2.count(fn))
    {
        need(math2.count(fn) ? 2 : 1);
        std::vector<ASTNodePtr> a;
        a.push_back(q::str(fn, ln));
        XV x = lowerTo(0, tDouble());
        a.push_back(read(x));
        if (math2.count(fn))
        {
            XV y = lowerTo(1, tDouble());
            a.push_back(read(y));
        }
        return mkxv(q::call("__cpp_math__", std::move(a), ln), tDouble());
    }
    if (fn == "lround" || fn == "llround" || fn == "lrint" || fn == "llrint")
    {
        XV x = lowerTo(0, tDouble());
        std::vector<ASTNodePtr> a;
        a.push_back(q::str("round", ln));
        a.push_back(read(x));
        return mkxv(q::call("__cpp_math__", std::move(a), ln), tLL());
    }
    if (fn == "isnan" || fn == "isinf" || fn == "isfinite" || fn == "signbit" || fn == "isnormal")
    {
        XV x = lowerTo(0, tDouble());
        std::vector<ASTNodePtr> a;
        a.push_back(q::str(fn, ln));
        a.push_back(read(x));
        return mkxv(q::call("__cpp_math__", std::move(a), ln), tBool());
    }
    if (fn == "abs" || fn == "labs" || fn == "llabs")
    {
        need(1);
        XV x = lower(0);
        TP t = isFloat(x.t) ? x.t : (x.t->k == T::Dyn ? tDyn() : arithResult(x.t, tInt()));
        std::vector<ASTNodePtr> a;
        a.push_back(q::str("fabs", ln));
        a.push_back(read(x));
        return mkxv(q::call("__cpp_math__", std::move(a), ln), t);
    }
    if (fn == "min" || fn == "max")
    {
        TP forced = targType(0);
        if (args.size() == 1 && args[0]->k == EK::InitList)
        {
            // max({a, b, c})
            std::vector<ExprP> kids = args[0]->kids;
            auto vt = mkT(T::Vector, "vector");
            XV first = lowerExpr(kids[0]);
            vt->a.push_back(forced ? forced : first.t);
            XV list = listInit(vt, kids, ln);
            std::vector<ASTNodePtr> a;
            a.push_back(read(list));
            a.push_back(q::boolean(fn == "max", ln));
            return mkxv(q::call("__cpp_minmax_list__", std::move(a), ln), vt->a[0]);
        }
        need(2);
        XV a = lower(0), b = lower(1);
        TP t = forced ? forced : (sameType(a.t, b.t) ? a.t : commonType(a.t, b.t));
        a = convert(std::move(a), t, ln);
        b = convert(std::move(b), t, ln);
        std::vector<ASTNodePtr> v;
        v.push_back(read(a));
        v.push_back(read(b));
        if (args.size() > 2)
        {
            XV c = lower(2);
            v.push_back(read(c));
        }
        return mkxv(q::call(fn == "min" ? "__cpp_min__" : "__cpp_max__", std::move(v), ln), t, false);
    }
    if (fn == "clamp")
    {
        need(3);
        XV a = lower(0);
        TP t = a.t;
        XV lo = lowerTo(1, t), hi = lowerTo(2, t);
        std::vector<ASTNodePtr> v;
        v.push_back(read(a));
        v.push_back(read(lo));
        v.push_back(read(hi));
        return mkxv(q::call("__cpp_clamp__", std::move(v), ln), t);
    }
    if (fn == "swap" || fn == "iter_swap")
    {
        need(2);
        XV a = lower(0), b = lower(1);
        if (fn == "iter_swap")
        {
            a = derefXV(std::move(a), ln);
            b = derefXV(std::move(b), ln);
        }
        if (isObjectRep(a.t))
            return mkxv(q::call("__cpp_swap_obj__", args2(read(a), read(b)), ln), tVoid());
        if ((a.lk == XV::StrCh || b.lk == XV::StrCh) && a.lk != XV::RV && b.lk != XV::RV)
        {
            // characters of strings: through a temporary
            ASTNodePtr t = simple(peekRead(a), ln);
            std::vector<ASTNodePtr> s;
            s.push_back(store(a, peekRead(b), ln));
            s.push_back(store(b, std::move(t), ln));
            return mkxv(q::call("__cpp_seq__", std::move(s), ln), tVoid());
        }
        XV ra = addressOf(std::move(a), ln), rb = addressOf(std::move(b), ln);
        return mkxv(q::call("__cpp_swap__", args2(read(ra), read(rb)), ln), tVoid());
    }
    if (fn == "move" || fn == "forward" || fn == "as_const")
    {
        need(1);
        XV x = lower(0);
        x.moved = fn == "move";
        return x;
    }
    if (fn == "exchange")
    {
        need(2);
        XV a = lower(0);
        XV b = lowerTo(1, a.t);
        XV r = addressOf(std::move(a), ln);
        TP t = deref(r.t);
        return mkxv(q::call("__cpp_exchange__", args2(read(r), read(b)), ln), t);
    }
    if (fn == "setw" || fn == "setprecision" || fn == "setfill" || fn == "setbase")
    {
        need(1);
        XV x = lower(0);
        std::vector<ASTNodePtr> a;
        a.push_back(q::str(fn, ln));
        a.push_back(read(x));
        return mkxv(q::call("__cpp_manip__", std::move(a), ln), manipT());
    }
    // conversions
    if (fn == "to_string" || fn == "to_wstring")
    {
        need(1);
        XV x = lower(0);
        ASTNodePtr k = streamKind(x.t);
        return mkxv(q::call("__cpp_to_string__", args2(read(x), std::move(k)), ln), tStr());
    }
    if (fn == "stoi" || fn == "stol" || fn == "stoll" || fn == "stoul" || fn == "stoull" || fn == "stof" ||
        fn == "stod" || fn == "stold" || fn == "atoi" || fn == "atol" || fn == "atoll" || fn == "atof" ||
        fn == "strtol" || fn == "strtoll" || fn == "strtod" || fn == "strtoul")
    {
        need(1);
        XV s = lower(0);
        ASTNodePtr pos = q::nil(ln);
        ASTNodePtr base = q::num(10, ln);
        bool isAto = fn.rfind("ato", 0) == 0;
        bool isStrto = fn.rfind("strto", 0) == 0;
        if (args.size() > 1 && !isAto)
        {
            XV p = lower(1);
            if (isStrto)
                pos = q::nil(ln); // char** end: not tracked
            else
                pos = read(p);
        }
        if (args.size() > 2)
        {
            XV b = lower(2);
            base = read(b);
        }
        bool fl = fn == "stof" || fn == "stod" || fn == "stold" || fn == "atof" || fn == "strtod";
        std::vector<ASTNodePtr> a;
        a.push_back(read(s));
        a.push_back(std::move(pos));
        a.push_back(std::move(base));
        a.push_back(q::str(fn, ln));
        TP rt = fl ? tDouble() : (fn == "stoi" || fn == "atoi") ? tInt() : (fn == "stoul" || fn == "stoull" || fn == "strtoul") ? tSize() : tLL();
        return mkxv(q::call("__cpp_sto__", std::move(a), ln), rt);
    }
    if (fn == "getline")
    {
        need(2);
        XV s = lower(0);
        TP st = s.t;
        XV dst = lower(1);
        ASTNodePtr stream = simple(read(s), ln);
        std::vector<ASTNodePtr> a;
        a.push_back(q::clone(stream));
        if (args.size() > 2)
        {
            XV d = lower(2);
            a.push_back(read(d));
        }
        ASTNodePtr got = q::call("__cpp_getline__", std::move(a), ln);
        ASTNodePtr st2 = store(dst, std::move(got), ln);
        return mkxv(q::call("__cpp_seq__", args2(std::move(st2), std::move(stream)), ln), st, false);
    }
    // <cctype>
    static const std::set<std::string> ctype = {"isalpha", "isdigit", "isalnum", "isspace", "isupper", "islower",
                                                "ispunct", "isxdigit", "iscntrl", "isprint", "isgraph", "isblank",
                                                "tolower", "toupper"};
    if (ctype.count(fn))
    {
        need(1);
        XV c = lowerTo(0, tInt());
        TP rt = fn == "tolower" || fn == "toupper" ? tInt() : tInt();
        return mkxv(q::call("__cpp_ctype__", args2(q::str(fn, ln), read(c)), ln), rt);
    }
    // C stdio / string
    if (fn == "printf" || fn == "fprintf" || fn == "sprintf" || fn == "snprintf")
    {
        size_t first = fn == "printf" ? 0 : fn == "snprintf" ? 2 : 1;
        need(first + 1);
        preEvalArgs(args, early, first + 1, nullptr);
        std::vector<ASTNodePtr> kinds, vals;
        ASTNodePtr target = q::str("stdout", ln);
        XV dstBuf;
        bool toBuf = fn == "sprintf" || fn == "snprintf";
        if (fn == "fprintf")
        {
            XV f = lower(0);
            target = read(f);
        }
        XV limit;
        if (toBuf)
            dstBuf = lower(0);
        if (fn == "snprintf")
            limit = lower(1);
        XV fmt = lower(first);
        for (size_t i = first + 1; i < args.size(); ++i)
        {
            XV x = lower(i);
            kinds.push_back(streamKind(x.t));
            vals.push_back(read(x));
        }
        std::vector<ASTNodePtr> a;
        a.push_back(toBuf ? q::str("string", ln) : std::move(target));
        a.push_back(read(fmt));
        a.push_back(q::arr(std::move(kinds), ln));
        a.push_back(q::arr(std::move(vals), ln));
        ASTNodePtr c = q::call("__cpp_printf__", std::move(a), ln);
        if (toBuf)
            return mkxv(q::call("__cpp_cs_sprintf__", args3(charPtr(dstBuf, ln), std::move(c), fn == "snprintf" ? read(limit) : q::num(-1, ln)), ln), tInt());
        return mkxv(std::move(c), tInt());
    }
    if (fn == "puts")
    {
        need(1);
        XV s = lower(0);
        return mkxv(q::call("__cpp_puts__", args1(charPtr(s, ln)), ln), tInt());
    }
    if (fn == "fputs")
    {
        need(2);
        XV s = lower(0), f = lower(1);
        return mkxv(q::call("__cpp_fputs__", args2(charPtr(s, ln), read(f)), ln), tInt());
    }
    if (fn == "fputc" || fn == "putc")
    {
        need(2);
        XV c = lowerTo(0, tInt()), f = lower(1);
        return mkxv(q::call("__cpp_fputc__", args2(read(c), read(f)), ln), tInt());
    }
    if (fn == "fgetc" || fn == "getc")
    {
        need(1);
        lower(0);
        return mkxv(q::call("__cpp_getchar__", {}, ln), tInt());
    }
    if (fn == "fgets" || fn == "gets")
    {
        need(1);
        XV d = lower(0);
        ASTNodePtr n = q::num(-1, ln);
        if (fn == "fgets")
        {
            need(3);
            XV nx = lowerTo(1, tInt());
            n = read(nx);
            lower(2); // stdin
        }
        return mkxv(q::call("__cpp_fgets__", args2(charPtr(d, ln), std::move(n)), ln), tCStr());
    }
    if (fn == "putchar")
    {
        XV c = lowerTo(0, tInt());
        return mkxv(q::call("__cpp_putchar__", args1(read(c)), ln), tInt());
    }
    if (fn == "getchar")
        return mkxv(q::call("__cpp_getchar__", {}, ln), tInt());
    // <stdarg.h>: a va_list is [rest arguments, next index]
    if (fn == "va_start")
    {
        need(1);
        XV ap = lower(0);
        std::vector<ASTNodePtr> cur;
        cur.push_back(q::id("__va", ln));
        cur.push_back(q::num(0, ln));
        return mkxv(store(ap, q::arr(std::move(cur), ln), ln), tVoid());
    }
    if (fn == "va_arg")
    {
        need(2);
        XV ap = lower(0);
        TP t = args.size() > 1 && args[1]->type ? resolveType(args[1]->type, currentScope()) : tDyn();
        XV v = mkxv(q::call("__cpp_va_arg__", args1(read(ap)), ln), tDyn());
        v.t = t;
        return v;
    }
    if (fn == "va_end")
    {
        need(1);
        XV ap = lower(0);
        return mkxv(q::call("__cpp_seq__", args2(read(ap), q::nil(ln)), ln), tVoid());
    }
    if (fn == "va_copy")
    {
        need(2);
        XV d = lower(0), s = lower(1);
        return mkxv(store(d, q::call("__cpp_va_copy__", args1(read(s)), ln), ln), tVoid());
    }
    if (fn == "vprintf" || fn == "vfprintf" || fn == "vsprintf" || fn == "vsnprintf")
    {
        size_t first = fn == "vprintf" ? 0 : fn == "vsnprintf" ? 2 : 1;
        need(first + 2);
        bool toBuf = fn == "vsprintf" || fn == "vsnprintf";
        ASTNodePtr target = q::str("stdout", ln);
        XV dstBuf;
        if (fn == "vfprintf")
        {
            XV f = lower(0);
            target = read(f);
        }
        if (toBuf)
            dstBuf = lower(0);
        XV fmt = lower(first), ap = lower(first + 1);
        std::vector<ASTNodePtr> a;
        a.push_back(toBuf ? q::str("string", ln) : std::move(target));
        a.push_back(read(fmt));
        a.push_back(q::arr({}, ln));
        a.push_back(q::call("__cpp_va_rest__", args1(read(ap)), ln));
        ASTNodePtr c = q::call("__cpp_printf__", std::move(a), ln);
        if (toBuf)
        {
            ASTNodePtr lim = q::num(-1, ln);
            if (fn == "vsnprintf")
            {
                XV l = lower(1);
                lim = read(l);
            }
            return mkxv(q::call("__cpp_cs_sprintf__", args3(charPtr(dstBuf, ln), std::move(c), std::move(lim)), ln), tInt());
        }
        return mkxv(std::move(c), tInt());
    }
    // scanf / sscanf / fscanf(stdin, ...): each argument is where a value goes
    if (fn == "scanf" || fn == "sscanf" || fn == "fscanf")
    {
        size_t first = fn == "scanf" ? 0 : 1;
        need(first + 1);
        ASTNodePtr source = q::nil(ln);
        if (fn == "sscanf")
        {
            XV s = lower(0);
            source = charPtr(s, ln);
        }
        else if (fn == "fscanf")
            lower(0); // stdin
        XV fmt = lower(first);
        std::vector<ASTNodePtr> refs;
        for (size_t i = first + 1; i < args.size(); ++i)
        {
            XV x = lower(i);
            if (x.t->k == T::Str || x.t->k == T::CStr)
            {
                refs.push_back(charPtr(x, ln)); // char buf[N]: a char* into the buffer
                continue;
            }
            refs.push_back(read(x));
        }
        std::vector<ASTNodePtr> a;
        a.push_back(std::move(source));
        a.push_back(read(fmt));
        a.push_back(q::arr(std::move(refs), ln));
        return mkxv(q::call("__cpp_scanf__", std::move(a), ln), tInt());
    }
    if (fn == "strtok")
    {
        need(2);
        XV s = lower(0), d = lower(1);
        return mkxv(q::call("__cpp_strtok__", args2(charPtr(s, ln), charPtr(d, ln)), ln), tCStr());
    }
    if (fn == "strdup" || fn == "strndup")
    {
        need(1);
        XV s = lower(0);
        ASTNodePtr n = q::num(-1, ln);
        if (fn == "strndup")
        {
            XV nx = lower(1);
            n = read(nx);
        }
        return mkxv(q::call("__cpp_cs_dup__", args2(charPtr(s, ln), std::move(n)), ln), tCStr());
    }
    // memcpy / memmove / memset / memcmp on arrays and blocks: by element
    if (fn == "memcpy" || fn == "memmove" || fn == "memset" || fn == "memcmp")
    {
        need(3);
        XV d = lower(0);
        TP dt = d.t;
        TP elem = dt->k == T::Ptr || dt->k == T::Array ? (dt->a.empty() ? tDyn() : dt->a[0]) : dt;
        long es = std::max(1L, sizeOfType(elem));
        if (dt->k == T::Str || dt->k == T::CStr)
        {
            // char buffers: bytes
            bool viaStore = d.lk == XV::Var && dt->k == T::Str; // a std::string variable
            ASTNodePtr dp = viaStore ? peekRead(d) : charPtr(d, ln);
            XV s = lower(1), n = lower(2);
            ASTNodePtr c;
            if (fn == "memset")
                c = q::call("__cpp_cs_memset__", args3(std::move(dp), read(s), read(n)), ln);
            else if (fn == "memcmp")
                return mkxv(q::call("__cpp_cs_memcmp__", args3(std::move(dp), charPtr(s, ln), read(n)), ln), tInt());
            else
                c = q::call("__cpp_cs_memcpy__", args3(std::move(dp), charPtr(s, ln), read(n)), ln);
            if (viaStore)
                c = store(d, std::move(c), ln);
            return mkxv(std::move(c), tCStr());
        }
        auto pt = mkT(T::Ptr);
        pt->a.push_back(elem);
        if (dt->k == T::Array)
            d = convert(std::move(d), pt, ln);
        if (fn == "memset")
        {
            XV v = lower(1), n = lower(2);
            std::vector<ASTNodePtr> a;
            a.push_back(read(d));
            a.push_back(read(v));
            a.push_back(q::num((double)es, ln));
            a.push_back(read(n));
            a.push_back(q::boolean(isFloat(elem), ln));
            return mkxv(q::call("__cpp_memset__", std::move(a), ln), pt);
        }
        XV s = lower(1);
        if (s.t->k == T::Array)
            s = convert(std::move(s), pt, ln);
        XV n = lower(2);
        std::vector<ASTNodePtr> a;
        a.push_back(read(d));
        a.push_back(read(s));
        a.push_back(q::num((double)es, ln));
        a.push_back(read(n));
        if (fn == "memcmp")
            return mkxv(q::call("__cpp_memcmp__", std::move(a), ln), tInt());
        a.push_back(planFor(elem));
        return mkxv(q::call("__cpp_memcpy__", std::move(a), ln), pt);
    }
    // the heap: blocks are typed by the pointer they are converted to
    if (fn == "malloc" || fn == "calloc" || fn == "realloc" || fn == "aligned_alloc")
    {
        need(fn == "malloc" ? 1 : 2);
        XV r;
        r.t = mkT(T::Ptr);
        r.t->a.push_back(tVoid());
        r.alloc = std::make_shared<XV::Alloc>();
        ASTNodePtr bytes;
        if (fn == "calloc")
        {
            XV n = lower(0), s = lower(1);
            bytes = q::bin("*", read(n), read(s), ln);
        }
        else
        {
            XV b = lower(fn == "malloc" ? 0 : 1);
            bytes = read(b);
        }
        if (fn == "realloc")
        {
            XV old = lower(0);
            r.alloc->old = std::make_shared<ASTNodePtr>(read(old));
            r.v = q::nil(ln);
        }
        else
            r.v = q::str("", ln); // used as raw bytes: a char block
        r.alloc->bytes = std::make_shared<ASTNodePtr>(std::move(bytes));
        return r;
    }
    if (fn == "free")
    {
        need(1);
        XV p = lower(0);
        return mkxv(q::call("__cpp_seq__", args2(read(p), q::nil(ln)), ln), tVoid());
    }
    if (fn == "strlen")
    {
        need(1);
        XV s = lower(0);
        return mkxv(q::call("__cpp_strlen__", args1(charPtr(s, ln)), ln), tSize());
    }
    if (fn == "strcmp" || fn == "strncmp" || fn == "strcasecmp" || fn == "_stricmp")
    {
        need(2);
        XV a = lower(0), b = lower(1);
        std::vector<ASTNodePtr> v;
        v.push_back(charPtr(a, ln));
        v.push_back(charPtr(b, ln));
        if (fn == "strncmp")
        {
            XV n = lower(2);
            v.push_back(read(n));
        }
        else
            v.push_back(q::num(-1, ln));
        v.push_back(q::boolean(fn == "strcasecmp" || fn == "_stricmp", ln));
        return mkxv(q::call("__cpp_strcmp__", std::move(v), ln), tInt());
    }
    if (fn == "strcpy" || fn == "strncpy" || fn == "strcat" || fn == "strncat")
    {
        need(2);
        XV d = lower(0), s = lower(1);
        bool viaStore = d.lk == XV::Var && d.t->k == T::Str; // a std::string variable
        std::vector<ASTNodePtr> v;
        v.push_back(viaStore ? peekRead(d) : charPtr(d, ln));
        v.push_back(charPtr(s, ln));
        if (fn == "strncpy" || fn == "strncat")
        {
            need(3);
            XV n = lower(2);
            v.push_back(read(n));
        }
        else
            v.push_back(q::num(-1, ln));
        v.push_back(q::boolean(fn == "strcat" || fn == "strncat", ln));
        ASTNodePtr c = q::call("__cpp_cs_strcpy__", std::move(v), ln);
        if (viaStore)
            c = store(d, std::move(c), ln);
        return mkxv(std::move(c), tCStr());
    }
    if (fn == "strchr" || fn == "strrchr")
    {
        need(2);
        XV s = lower(0), c = lowerTo(1, tInt());
        return mkxv(q::call("__cpp_cs_chr__", args3(charPtr(s, ln), read(c), q::boolean(fn == "strrchr", ln)), ln), tCStr());
    }
    if (fn == "strstr")
    {
        need(2);
        XV s = lower(0), n = lower(1);
        return mkxv(q::call("__cpp_cs_strstr__", args2(charPtr(s, ln), charPtr(n, ln)), ln), tCStr());
    }
    if (fn == "strspn" || fn == "strcspn" || fn == "strpbrk")
    {
        need(2);
        XV s = lower(0), set = lower(1);
        int mode = fn == "strspn" ? 0 : fn == "strcspn" ? 1 : 2;
        return mkxv(q::call("__cpp_cs_span__", args3(charPtr(s, ln), charPtr(set, ln), q::num(mode, ln)), ln),
                    mode == 2 ? tCStr() : tSize());
    }
    if (fn == "qsort" || fn == "bsearch")
    {
        // (base, n, size, cmp) / (key, base, n, size, cmp): elements reach the comparator by pointer
        size_t b0 = fn == "bsearch" ? 1 : 0;
        need(b0 + 4);
        ASTNodePtr key;
        if (fn == "bsearch")
        {
            XV k = lower(0);
            key = read(k);
        }
        XV base = lower(b0);
        TP et = base.t->k == T::Array || base.t->k == T::Ptr ? (base.t->a.empty() ? tDyn() : base.t->a[0]) : tDyn();
        ASTNodePtr bp;
        if (base.t->k == T::Array)
        {
            std::vector<ASTNodePtr> r;
            r.push_back(read(base));
            r.push_back(q::num(0, ln));
            bp = q::arr(std::move(r), ln); // [array, 0], whatever the elements are
        }
        else
            bp = read(base);
        XV n = lower(b0 + 1);
        lower(b0 + 2);
        XV cmp = lower(b0 + 3);
        if (cmp.isFuncName)
            cmp = convert(std::move(cmp), mkT(T::Func), ln);
        int mode = isCBuf(et) ? 2 : (isObjectRep(et) && et->k != T::Array) ? 1 : 0;
        std::vector<ASTNodePtr> v;
        if (fn == "bsearch")
            v.push_back(std::move(key));
        v.push_back(std::move(bp));
        v.push_back(read(n));
        v.push_back(read(cmp));
        v.push_back(q::num(mode, ln));
        auto pt = mkT(T::Ptr);
        pt->a.push_back(mkT(T::Void, "void"));
        return mkxv(q::call(fn == "qsort" ? "__cpp_qsort__" : "__cpp_bsearch__", std::move(v), ln),
                    fn == "qsort" ? tVoid() : pt);
    }
    if (fn == "exit" || fn == "_Exit" || fn == "quick_exit")
    {
        XV c = args.empty() ? mkxv(q::num(0, ln), tInt()) : lower(0);
        return mkxv(q::call("__cpp_exit__", args1(read(c)), ln), tVoid());
    }
    if (fn == "abort" || fn == "terminate")
        return mkxv(q::call("__cpp_exit__", args1(q::num(3, ln)), ln), tVoid());
    if (fn == "assert")
    {
        need(1);
        XV c = lower(0);
        std::string text = "assertion";
        return mkxv(q::call("__cpp_assert__", args2(toBool(std::move(c), ln), q::str(text, ln)), ln), tVoid());
    }
    if (fn == "rand")
        return mkxv(q::call("__cpp_rand__", {}, ln), tInt());
    if (fn == "srand")
    {
        XV s = lower(0);
        return mkxv(q::call("__cpp_srand__", args1(read(s)), ln), tVoid());
    }
    if (fn == "time")
    {
        if (!args.empty())
        {
            XV a = lower(0);
            (void)a;
        }
        return mkxv(q::call("__cpp_time__", {}, ln), tLL());
    }
    if (fn == "clock")
        return mkxv(q::call("__cpp_clock__", {}, ln), tLL());
    if (fn == "system")
    {
        XV a = lower(0);
        return mkxv(q::call("__cpp_system__", args1(read(a)), ln), tInt());
    }
    if (fn == "fflush" || fn == "setvbuf" || fn == "ios::sync_with_stdio" || fn == "ios_base::sync_with_stdio")
    {
        for (size_t i = 0; i < args.size(); ++i)
            lower(i);
        return mkxv(q::num(0, ln), tInt());
    }
    // pair / tuple / smart pointers / variant / optional
    if (fn == "make_pair" || fn == "make_tuple" || fn == "tuple" || fn == "pair")
    {
        auto t = mkT(fn.find("pair") != std::string::npos ? T::Pair : T::Tuple);
        std::vector<ASTNodePtr> el;
        for (size_t i = 0; i < args.size(); ++i)
        {
            XV x = lower(i);
            TP et = x.t->k == T::CStr ? tCStr() : x.t;
            t->a.push_back(et);
            el.push_back(copyOf(std::move(x), ln));
        }
        return mkxv(q::arr(std::move(el), ln), t);
    }
    if (fn == "tie")
    {
        // tie(a, b) = pair: lvalues collected as refs
        std::vector<ASTNodePtr> el;
        auto t = mkT(T::Tuple);
        for (size_t i = 0; i < args.size(); ++i)
        {
            XV x = lower(i);
            t->a.push_back(x.t);
            XV r = addressOf(std::move(x), ln);
            el.push_back(read(r));
        }
        auto tt = mkT(T::Dyn, "tie");
        return mkxv(q::call("__cpp_tie__", args1(q::arr(std::move(el), ln)), ln), tt);
    }
    if (fn == "make_unique" || fn == "make_shared")
    {
        TP t = targType(0);
        if (!t)
            unsupported(fn + " without a type", ln);
        auto pt = mkT(T::UPtr, fn == "make_unique" ? "unique_ptr" : "shared_ptr");
        pt->a.push_back(t);
        if (t->k == T::Class && t->cls)
        {
            std::vector<ExprP> a = args;
            ASTNodePtr obj = ctorCall(t->cls, a, false, ln);
            if (fn == "make_shared")
                obj = q::call("__cpp_sp_new__", args1(std::move(obj)), ln); // one owner
            return mkxv(std::move(obj), pt);
        }
        if (isObjectRep(t))
        {
            std::vector<ExprP> a = args;
            XV x = constructBuiltinType(t, a, false, ln);
            x.t = pt;
            return x;
        }
        ASTNodePtr v = args.empty() ? defaultValue(t, ln) : read(*std::make_shared<XV>(lowerTo(0, t)));
        std::vector<ASTNodePtr> cell;
        cell.push_back(std::move(v));
        std::vector<ASTNodePtr> r;
        r.push_back(q::arr(std::move(cell), ln));
        r.push_back(q::num(0, ln));
        return mkxv(q::arr(std::move(r), ln), pt);
    }
    auto variantIndex = [&](const TP &vt, const TP &alt) -> int
    {
        for (size_t i = 0; i < vt->a.size(); ++i)
            if (sameType(vt->a[i], alt))
                return (int)i;
        return -1;
    };
    if (fn == "holds_alternative")
    {
        need(1);
        XV v = lower(0);
        TP alt = targType(0);
        if (!alt || v.t->k != T::Variant)
            unsupported("holds_alternative needs a variant and a type", ln);
        int idx = variantIndex(v.t, alt);
        return mkxv(q::bin("==", q::index(read(v), q::num(0, ln), ln), q::num(idx, ln), ln), tBool());
    }
    if (fn == "get_if")
    {
        need(1);
        ExprP a0 = args[0];
        XV v = (a0->k == EK::Unary && a0->op == "&") ? lowerExpr(a0->kids[0]) : derefXV(lowerExpr(a0), ln);
        TP alt = targType(0);
        if (!alt || v.t->k != T::Variant)
            unsupported("get_if needs a variant pointer and a type", ln);
        int idx = variantIndex(v.t, alt);
        auto pt = mkT(T::Ptr);
        pt->a.push_back(alt);
        if (isObjectRep(alt))
            return mkxv(q::call("__cpp_vget_obj_if__", args2(read(v), q::num(idx, ln)), ln), pt);
        return mkxv(q::call("__cpp_vget_if__", args2(read(v), q::num(idx, ln)), ln), pt);
    }
    if (fn == "get")
    {
        need(1);
        XV v = lower(0);
        if (v.t->k == T::Variant)
        {
            int idx = -1;
            TP alt = targType(0);
            if (alt)
                idx = variantIndex(v.t, alt);
            else if (!targs.empty() && targs[0].expr)
                idx = (int)evalConst(targs[0].expr, currentScope());
            if (idx < 0)
                unsupported("std::get: type not in the variant", ln);
            ASTNodePtr var = simple(read(v), ln);
            XV x;
            x.t = v.t->a[idx];
            x.lk = XV::Idx;
            x.obj = q::call("__cpp_vcheck__", args2(std::move(var), q::num(idx, ln)), ln);
            x.obj = simple(std::move(x.obj), ln);
            x.key = q::num(1, ln);
            return x;
        }
        if (v.t->k == T::Pair || v.t->k == T::Tuple || v.t->k == T::Array)
        {
            int idx = !targs.empty() && targs[0].expr ? (int)evalConst(targs[0].expr, currentScope()) : 0;
            TP alt = targType(0);
            if (alt)
                for (size_t i = 0; i < v.t->a.size(); ++i)
                    if (sameType(v.t->a[i], alt))
                        idx = (int)i;
            XV x;
            x.t = v.t->k == T::Array ? v.t->a[0] : v.t->a[idx];
            x.lk = XV::Idx;
            x.obj = simple(read(v), ln);
            x.key = q::num(idx, ln);
            return x;
        }
        unsupported("std::get on " + v.t->str(), ln);
    }
    if (fn == "visit")
    {
        need(2);
        XV f = lower(0);
        XV v = lower(1);
        TP rt = f.t->k == T::Func && !f.t->a.empty() ? f.t->a[0] : tDyn();
        return mkxv(q::call("__cpp_visit__", args2(read(f), read(v)), ln), rt);
    }
    // numeric_limits<T>::max() etc.
    if (fn.rfind("numeric_limits::", 0) == 0)
    {
        TP t = nullptr;
        if (e->kids[0]->type && e->kids[0]->type->targs.size() >= 1 && !e->kids[0]->type->targs[0].empty() &&
            e->kids[0]->type->targs[e->kids[0]->type->name[0] == "std" ? 1 : 0].size())
        {
            const auto &tn = e->kids[0]->type;
            size_t comp = tn->name[0] == "std" ? 1 : 0;
            if (comp < tn->targs.size() && !tn->targs[comp].empty() && tn->targs[comp][0].type)
                t = resolveType(tn->targs[comp][0].type, currentScope());
        }
        if (!t)
            t = tInt();
        std::string m = fn.substr(16);
        double v = 0;
        bool fl = isFloat(t);
        if (m == "max")
            v = fl ? (t->name == "float" ? 3.402823466e38 : 1.7976931348623157e308)
                   : t->k == T::Char ? (t->isUnsigned ? 255 : 127)
                   : t->name.find("long long") != std::string::npos ? (t->isUnsigned ? 18446744073709551615.0 : 9223372036854775807.0)
                   : t->name.find("short") != std::string::npos ? (t->isUnsigned ? 65535 : 32767)
                   : (t->isUnsigned ? 4294967295.0 : 2147483647.0);
        else if (m == "min")
            v = fl ? (t->name == "float" ? 1.175494351e-38 : 2.2250738585072014e-308)
                   : t->isUnsigned ? 0
                   : t->k == T::Char ? -128
                   : t->name.find("long long") != std::string::npos ? -9223372036854775808.0
                   : t->name.find("short") != std::string::npos ? -32768 : -2147483648.0;
        else if (m == "lowest")
            v = fl ? (t->name == "float" ? -3.402823466e38 : -1.7976931348623157e308)
                   : t->isUnsigned ? 0
                   : t->name.find("long long") != std::string::npos ? -9223372036854775808.0 : -2147483648.0;
        else if (m == "epsilon")
            v = t->name == "float" ? 1.192092896e-07 : 2.220446049250313e-16;
        else if (m == "infinity")
            v = HUGE_VAL;
        else if (m == "quiet_NaN")
            return mkxv(q::bin("/", q::num(0, ln), q::num(0, ln), ln), t);
        else if (m == "digits10")
            v = fl ? 15 : 9;
        return mkxv(q::num(v, ln), t);
    }
    // iterators
    if (fn == "begin" || fn == "end" || fn == "cbegin" || fn == "cend" || fn == "rbegin" || fn == "rend" ||
        fn == "size" || fn == "empty" || fn == "data")
    {
        need(1);
        XV c = lower(0);
        bool h = false;
        std::vector<ExprP> none;
        XV r = libMethod(c, fn, e, none, h);
        if (h)
            return r;
    }
    if (fn == "next" || fn == "prev")
    {
        XV it = lower(0);
        ASTNodePtr n = q::num(1, ln);
        if (args.size() > 1)
        {
            XV k = lower(1);
            n = read(k);
        }
        if (fn == "prev")
            n = q::un("-", std::move(n), ln);
        TP t = it.t;
        return mkxv(q::call("__cpp_it_add__", args2(read(it), std::move(n)), ln), t);
    }
    if (fn == "advance")
    {
        XV it = lower(0);
        XV n = lower(1);
        ASTNodePtr cur = peekRead(it);
        return mkxv(store(it, q::call("__cpp_it_add__", args2(std::move(cur), read(n)), ln), ln), tVoid());
    }
    if (fn == "distance")
    {
        XV a = lower(0), b = lower(1);
        return mkxv(q::call("__cpp_it_diff__", args2(read(b), read(a)), ln), tLL());
    }

    // <algorithm> / <numeric>
    // A range written c.begin(), c.end() over a string: the string is rebuilt.
    auto stringRangeTarget = [&](size_t i) -> std::shared_ptr<XV>
    {
        const ExprP &x = args[i];
        if (x->k == EK::Call && x->kids[0]->k == EK::Member &&
            (x->kids[0]->op == "begin" || x->kids[0]->op == "rbegin"))
        {
            XV obj = lowerExpr(x->kids[0]->kids[0]);
            if (obj.t->k == T::Str && obj.lk != XV::RV)
            {
                auto p = std::make_shared<XV>(std::move(obj));
                if (p->lk == XV::Idx)
                {
                    p->obj = simple(std::move(p->obj), ln);
                    p->key = simple(std::move(p->key), ln);
                }
                p->v.reset();
                return p;
            }
        }
        return nullptr;
    };
    auto cmpArg = [&](size_t i) -> ASTNodePtr
    {
        if (i >= args.size())
            return q::nil(ln);
        XV c = lower(i);
        if (c.t->k == T::Dyn && (c.t->name == "less" || c.t->name == "greater"))
            return q::str(c.t->name, ln);
        if (c.isFuncName)
            c = convert(std::move(c), mkT(T::Func), ln);
        return read(c);
    };
    static const std::set<std::string> mutating = {"sort", "stable_sort", "reverse", "transform", "fill", "iota",
                                                   "unique", "remove", "remove_if", "rotate", "shuffle",
                                                   "random_shuffle", "next_permutation", "prev_permutation",
                                                   "replace", "replace_if", "partial_sort", "nth_element"};
    if ((fn == "next_permutation" || fn == "prev_permutation") && !args.empty())
    {
        if (auto s = stringRangeTarget(0))
        {
            XV r = addressOf(std::move(*s), ln);
            return mkxv(q::call("__cpp_str_perm__", args2(read(r), q::boolean(fn == "next_permutation", ln)), ln), tBool());
        }
    }
    if (mutating.count(fn) && !args.empty())
    {
        if (auto s = stringRangeTarget(0))
        {
            // on a string: run on its characters, then store the string back
            std::vector<ASTNodePtr> a;
            a.push_back(q::str(fn, ln));
            a.push_back(peekRead(*s));
            for (size_t i = 2; i < args.size(); ++i)
            {
                if (fn == "transform" && i == 2)
                    continue; // destination (same string)
                XV x = lower(i);
                if (x.isFuncName)
                    x = convert(std::move(x), mkT(T::Func), ln);
                a.push_back(x.t->k == T::Char ? read(x) : read(x));
            }
            ASTNodePtr r = q::call("__cpp_str_algo__", std::move(a), ln);
            return mkxv(store(*s, std::move(r), ln), tVoid());
        }
    }
    if (fn == "sort" || fn == "stable_sort" || fn == "partial_sort")
    {
        need(2);
        size_t lastIdx = fn == "partial_sort" ? 2 : 1;
        auto memberCall = [&](const ExprP &x, const char *m) -> ExprP
        {
            if (x->k == EK::Call && x->kids.size() == 1 && x->kids[0]->k == EK::Member && x->kids[0]->op == m &&
                !x->kids[0]->kids.empty())
                return x->kids[0]->kids[0];
            return nullptr;
        };
        ExprP rb = memberCall(args[0], "rbegin"), re = memberCall(args[lastIdx], "rend");
        if (fn != "partial_sort" && rb && re && rb->k == EK::Name && re->k == EK::Name && rb->name == re->name)
        {
            // sort(v.rbegin(), v.rend()): the container, in descending order
            XV c = lowerExpr(rb);
            TP el = elemType(c.t);
            ASTNodePtr cv = simple(read(c), ln);
            std::vector<ASTNodePtr> f, l, v;
            f.push_back(q::clone(cv));
            f.push_back(q::num(0, ln));
            l.push_back(q::clone(cv));
            l.push_back(q::call("__cpp_size__", args1(std::move(cv)), ln));
            v.push_back(q::arr(std::move(f), ln));
            v.push_back(q::arr(std::move(l), ln));
            if (el->k != T::Dyn)
                lambdaHints_ = {el, el};
            v.push_back(cmpArg(lastIdx + 1));
            lambdaHints_.clear();
            v.push_back(q::boolean(true, ln));
            return mkxv(q::call("__cpp_sort__", std::move(v), ln), tVoid());
        }
        XV a = lower(0);
        XV b = lower(lastIdx);
        std::vector<ASTNodePtr> v;
        TP el = deref(a.t);
        v.push_back(read(a));
        v.push_back(read(b));
        if (el->k != T::Dyn)
            lambdaHints_ = {el, el};
        v.push_back(cmpArg(lastIdx + 1));
        lambdaHints_.clear();
        return mkxv(q::call("__cpp_sort__", std::move(v), ln), tVoid());
    }
    if (fn == "reverse" || fn == "shuffle" || fn == "random_shuffle")
    {
        XV a = lower(0), b = lower(1);
        return mkxv(q::call(fn == "reverse" ? "__cpp_reverse__" : "__cpp_shuffle__", args2(read(a), read(b)), ln), tVoid());
    }
    if (fn == "count" || fn == "find" || fn == "count_if" || fn == "find_if" || fn == "find_if_not" ||
        fn == "all_of" || fn == "any_of" || fn == "none_of" || fn == "for_each" || fn == "binary_search" ||
        fn == "lower_bound" || fn == "upper_bound" || fn == "remove" || fn == "remove_if" || fn == "fill" ||
        fn == "iota" || fn == "unique" || fn == "next_permutation" || fn == "prev_permutation" ||
        fn == "min_element" || fn == "max_element" || fn == "accumulate" || fn == "replace" ||
        fn == "replace_if" || fn == "adjacent_find" || fn == "is_sorted" || fn == "equal" || fn == "copy" ||
        fn == "copy_if" || fn == "fill_n" || fn == "rotate" || fn == "nth_element" || fn == "search" ||
        fn == "reduce" || fn == "partial_sum" || fn == "minmax_element" || fn == "includes" ||
        fn == "inner_product" || fn == "mismatch")
    {
        need(2);
        XV a = lower(0);
        TP itT = a.t;
        TP elT = deref(itT);
        std::vector<ASTNodePtr> v;
        v.push_back(q::str(fn, ln));
        v.push_back(read(a));
        TP rt = tDyn();
        for (size_t i = 1; i < args.size(); ++i)
        {
            if (args[i]->k == EK::Lambda && elT->k != T::Dyn)
                lambdaHints_ = {fn == "accumulate" || fn == "reduce" ? tDyn() : elT, elT};
            XV x = lower(i);
            lambdaHints_.clear();
            if (x.isFuncName)
                x = convert(std::move(x), mkT(T::Func), ln);
            if ((fn == "count" || fn == "find" || fn == "remove" || fn == "fill" || fn == "binary_search" ||
                 fn == "lower_bound" || fn == "upper_bound" || fn == "replace") &&
                i >= 2 && elT->k != T::Dyn)
                x = convert(std::move(x), elT, ln);
            if (x.t->k == T::Dyn && (x.t->name == "less" || x.t->name == "greater"))
            {
                v.push_back(q::str(x.t->name, ln));
                continue;
            }
            v.push_back(read(x));
        }
        if (fn == "count" || fn == "count_if")
            rt = tLL();
        else if (fn == "all_of" || fn == "any_of" || fn == "none_of" || fn == "binary_search" ||
                 fn == "next_permutation" || fn == "prev_permutation" || fn == "is_sorted" || fn == "equal" ||
                 fn == "includes")
            rt = tBool();
        else if (fn == "accumulate" || fn == "reduce" || fn == "inner_product")
        {
            // the type of the initial value
            size_t initIdx = fn == "inner_product" ? 3 : 2;
            if (args.size() > initIdx)
            {
                auto saved = pre_;
                std::vector<ASTNodePtr> scratch;
                pre_ = &scratch;
                XV init = lower(initIdx);
                pre_ = saved;
                rt = init.t->k == T::CStr ? tStr() : init.t;
            }
            else
                rt = elT;
        }
        else if (fn == "for_each" || fn == "fill" || fn == "iota" || fn == "replace" || fn == "replace_if" ||
                 fn == "fill_n" || fn == "nth_element")
            rt = tVoid();
        else
            rt = itT; // iterator results
        if (fn == "accumulate" && rt->k == T::Str)
            v[0] = q::str("accumulate_str", ln);
        return mkxv(q::call("__cpp_algo__", std::move(v), ln), rt);
    }
    if (fn == "transform")
    {
        need(4);
        std::vector<ASTNodePtr> v;
        TP el = tDyn();
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (args[i]->k == EK::Lambda && el->k != T::Dyn)
                lambdaHints_ = {el, el};
            XV x = lower(i);
            lambdaHints_.clear();
            if (i == 0)
                el = deref(x.t);
            if (x.isFuncName)
                x = convert(std::move(x), mkT(T::Func), ln);
            v.push_back(read(x));
        }
        return mkxv(q::call("__cpp_transform__", std::move(v), ln), tDyn());
    }
    if (fn == "this_thread::sleep_for" || fn == "this_thread::yield")
    {
        for (size_t i = 0; i < args.size(); ++i)
            lower(i);
        return mkxv(q::nil(ln), tVoid());
    }
    handled = false;
    return XV();
}

// ─── Methods of library types ──────────────────────────────────────────────

XV Lowerer::libMethod(XV &obj, const std::string &m, const ExprP &e, std::vector<ExprP> &args, bool &handled)
{
    handled = true;
    int ln = e->line;
    TP t = obj.t;
    auto lower = [&](size_t i) -> XV { return lowerExpr(args[i]); };
    auto lowerTo = [&](size_t i, const TP &to) -> XV
    { return convert(lowerExprTo(args[i], to), to, ln); };
    // the object as a re-readable expression
    if (obj.lk == XV::Idx)
    {
        obj.obj = simple(std::move(obj.obj), ln);
        obj.key = simple(std::move(obj.key), ln);
    }
    auto self = [&]() -> ASTNodePtr
    {
        if (obj.lk == XV::RV)
        {
            if (obj.v)
            {
                obj.v = simple(std::move(obj.v), ln);
                return q::clone(obj.v);
            }
        }
        return peekRead(obj);
    };
    auto call = [&](const char *fn, std::vector<ASTNodePtr> a, TP rt, bool temp = true)
    { return mkxv(q::call(fn, std::move(a), ln), rt, temp); };

    switch (t->k)
    {
    case T::Str:
    case T::CStr:
    {
        if (m == "size" || m == "length")
            return call("__cpp_size__", args1(self()), tSize());
        if (m == "empty")
            return mkxv(q::bin("==", q::call("__cpp_size__", args1(self()), ln), q::num(0, ln), ln), tBool());
        if (m == "c_str" || m == "data" || m == "str")
        {
            XV x = mkxv(self(), tCStr(), false);
            return x;
        }
        if (m == "substr")
        {
            ASTNodePtr pos = q::num(0, ln), len = q::num(-1, ln);
            if (args.size() > 0)
                pos = read(*std::make_shared<XV>(lower(0)));
            if (args.size() > 1)
                len = read(*std::make_shared<XV>(lower(1)));
            return call("__cpp_substr__", args3(self(), std::move(pos), std::move(len)), tStr());
        }
        if (m == "find" || m == "rfind" || m == "find_first_of" || m == "find_last_of" ||
            m == "find_first_not_of" || m == "find_last_not_of" || m == "contains" || m == "starts_with" ||
            m == "ends_with")
        {
            XV needle = lower(0);
            ASTNodePtr nd = needle.t->k == T::Char ? chr(read(needle), ln) : read(needle);
            std::vector<ASTNodePtr> a;
            a.push_back(q::str(m, ln));
            a.push_back(self());
            a.push_back(std::move(nd));
            if (args.size() > 1)
                a.push_back(read(*std::make_shared<XV>(lower(1))));
            else
                a.push_back(q::nil(ln));
            if (args.size() > 2)
                a.push_back(read(*std::make_shared<XV>(lower(2))));
            TP rt = (m == "contains" || m == "starts_with" || m == "ends_with") ? tBool() : tSize();
            return call("__cpp_str_find__", std::move(a), rt);
        }
        if (m == "compare")
        {
            if (args.size() == 1)
            {
                XV o = lower(0);
                return call("__cpp_str_compare__", args2(self(), read(o)), tInt());
            }
            // compare(pos, len, s)
            XV p = lower(0), l = lower(1), o = lower(2);
            ASTNodePtr sub = q::call("__cpp_substr__", args3(self(), read(p), read(l)), ln);
            return call("__cpp_str_compare__", args2(std::move(sub), read(o)), tInt());
        }
        if (m == "at" || m == "front" || m == "back")
        {
            ASTNodePtr i;
            if (m == "at")
            {
                XV k = lower(0);
                i = q::call("__cpp_str_check__", args2(self(), read(k)), ln);
            }
            else if (m == "front")
                i = q::num(0, ln);
            else
                i = q::bin("-", q::call("__cpp_size__", args1(self()), ln), q::num(1, ln), ln);
            if (obj.lk != XV::RV && t->k == T::Str)
            {
                XV x;
                x.t = tChar();
                x.lk = XV::StrCh;
                auto base = std::make_shared<XV>();
                base->t = t;
                base->lk = obj.lk;
                base->var = obj.var;
                base->obj = q::clone(obj.obj);
                base->key = q::clone(obj.key);
                base->base = obj.base;
                x.base = base;
                x.key = simple(std::move(i), ln);
                return x;
            }
            return call("__cpp_str_at__", args2(self(), std::move(i)), tChar());
        }
        if (m == "begin" || m == "cbegin" || m == "end" || m == "cend")
        {
            std::vector<ASTNodePtr> el;
            el.push_back(self());
            el.push_back(m == "begin" || m == "cbegin" ? q::num(0, ln) : q::call("__cpp_size__", args1(self()), ln));
            return mkxv(q::arr(std::move(el), ln), iterOf(tStr()));
        }
        if (m == "rbegin" || m == "rend")
        {
            std::vector<ASTNodePtr> el;
            el.push_back(q::call("__cpp_str_reversed__", args1(self()), ln));
            el.push_back(m == "rbegin" ? q::num(0, ln) : q::call("__cpp_size__", args1(self()), ln));
            return mkxv(q::arr(std::move(el), ln), iterOf(tStr()));
        }
        if (obj.lk == XV::RV && (m == "push_back" || m == "append" || m == "pop_back" || m == "clear" ||
                                 m == "insert" || m == "erase" || m == "replace" || m == "resize"))
            unsupported("modifying a temporary string", ln);
        if (m == "push_back" || m == "append" || m == "operator+=")
        {
            ASTNodePtr add;
            if (m == "append" && args.size() >= 2)
            {
                XV a0 = lower(0), a1 = lower(1);
                if (isIntegral(a0.t) && (a1.t->k == T::Char))
                    add = q::call("__cpp_str_repeat__", args2(read(a0), read(a1)), ln); // append(n, c)
                else if (args.size() == 3)
                {
                    XV a2 = lower(2);
                    add = q::call("__cpp_substr__", args3(read(a0), read(a1), read(a2)), ln);
                }
                else
                    add = q::call("__cpp_substr__", args3(read(a0), q::num(0, ln), read(a1)), ln);
            }
            else
            {
                XV a = lower(0);
                add = a.t->k == T::Char || (m == "push_back" && isIntegral(a.t)) ? chr(read(a), ln) : read(a);
            }
            ASTNodePtr cur = peekRead(obj);
            return mkxv(store(obj, q::bin("+", std::move(cur), std::move(add), ln), ln), tVoid());
        }
        if (m == "pop_back")
        {
            ASTNodePtr cur = peekRead(obj);
            return mkxv(store(obj, q::call("__cpp_str_pop__", args1(std::move(cur)), ln), ln), tVoid());
        }
        if (m == "clear")
            return mkxv(store(obj, q::str("", ln), ln), tVoid());
        if (m == "erase" && !args.empty() && args[0]->k == EK::Call && args[0]->kids[0]->k == EK::Name &&
            (args[0]->kids[0]->name.back() == "remove" || args[0]->kids[0]->name.back() == "remove_if" ||
             args[0]->kids[0]->name.back() == "unique"))
        {
            // erase-remove idiom on a string: the algorithm drops the characters itself
            XV r = lowerExpr(args[0]);
            (void)r;
            return mkxv(q::nil(ln), tVoid());
        }
        if (m == "insert" || m == "erase" || m == "replace" || m == "resize" || m == "assign" || m == "swap")
        {
            std::vector<ASTNodePtr> a;
            a.push_back(q::str(m, ln));
            a.push_back(peekRead(obj));
            std::vector<ASTNodePtr> rest;
            std::vector<ASTNodePtr> kinds;
            if (m == "swap")
            {
                XV o = lower(0);
                ASTNodePtr mine = peekRead(obj);
                ASTNodePtr theirs = peekRead(o);
                // tmp = a; a = b; b = tmp
                std::string tmp = tempName();
                if (pre_ && canHoist_)
                {
                    pre_->push_back(q::var(tmp, std::move(mine), ln));
                    ASTNodePtr s1 = store(obj, std::move(theirs), ln);
                    ASTNodePtr s2 = store(o, q::id(tmp, ln), ln);
                    return mkxv(q::call("__cpp_seq__", args2(std::move(s1), std::move(s2)), ln), tVoid());
                }
                unsupported("string::swap here", ln);
            }
            for (size_t i = 0; i < args.size(); ++i)
            {
                XV x = lower(i);
                kinds.push_back(streamKind(x.t));
                if (x.t->k == T::Iter)
                {
                    // iterator position: its index
                    rest.push_back(q::index(read(x), q::num(1, ln), ln));
                    continue;
                }
                rest.push_back(read(x));
            }
            a.push_back(q::arr(std::move(rest), ln));
            a.push_back(q::arr(std::move(kinds), ln));
            return mkxv(store(obj, q::call("__cpp_str_edit__", std::move(a), ln), ln), tVoid());
        }
        break;
    }
    case T::Vector:
    case T::Array:
    {
        TP et = t->a.empty() ? tDyn() : t->a[0];
        const std::string &kind = t->name;
        if (m == "size")
            return call("__cpp_size__", args1(self()), tSize());
        if (m == "empty")
            return mkxv(q::bin("==", q::call("__cpp_size__", args1(self()), ln), q::num(0, ln), ln), tBool());
        if (m == "capacity" || m == "max_size")
            return call("__cpp_size__", args1(self()), tSize());
        if (m == "reserve" || m == "shrink_to_fit")
        {
            for (size_t i = 0; i < args.size(); ++i)
                lower(i);
            return mkxv(q::nil(ln), tVoid());
        }
        if (kind == "priority_queue")
        {
            if (m == "push" || m == "emplace")
            {
                XV x = m == "push" ? lowerTo(0, et) : [&]
                {
                    std::vector<ExprP> a = args;
                    return makeObjectFrom(et, a, false, ln);
                }();
                return call("__cpp_pq_push__", args3(self(), copyOf(std::move(x), ln), q::boolean(t->multi, ln)), tVoid());
            }
            if (m == "pop")
                return call("__cpp_pq_pop__", args2(self(), q::boolean(t->multi, ln)), tVoid());
            if (m == "top")
                return mkxv(q::index(self(), q::num(0, ln), ln), et, false);
        }
        if (m == "push_back" || m == "emplace_back" || (m == "push" && (kind == "stack" || kind == "queue")) ||
            (m == "emplace" && (kind == "stack" || kind == "queue")))
        {
            XV x;
            if (m == "push_back" || m == "push")
                x = lowerTo(0, et);
            else
            {
                std::vector<ExprP> a = args;
                if (a.size() == 1)
                {
                    x = lowerTo(0, et);
                }
                else
                    x = makeObjectFrom(et, a, false, ln);
            }
            return call("__cpp_push__", args2(self(), copyOf(std::move(x), ln)), tVoid());
        }
        if (m == "push_front" || m == "emplace_front")
        {
            XV x = lowerTo(0, et);
            return call("__cpp_push_front__", args2(self(), copyOf(std::move(x), ln)), tVoid());
        }
        if (m == "pop_back" || (m == "pop" && kind == "stack"))
            return call("__cpp_pop__", args1(self()), tVoid());
        if (m == "pop_front" || (m == "pop" && kind == "queue"))
            return call("__cpp_pop_front__", args1(self()), tVoid());
        if (m == "clear")
            return call("__cpp_clear__", args1(self()), tVoid());
        if (m == "back" || m == "front" || m == "top" || m == "at")
        {
            XV x;
            x.t = et;
            x.lk = XV::Idx;
            x.obj = self();
            if (m == "front" || (m == "front" && kind == "queue"))
                x.key = q::num(0, ln);
            else if (m == "at")
            {
                XV k = lower(0);
                x.key = simple(q::call("__cpp_check_index__", args2(self(), read(k)), ln), ln);
            }
            else
                x.key = simple(q::bin("-", q::call("__cpp_size__", args1(self()), ln), q::num(1, ln), ln), ln);
            return x;
        }
        if (m == "begin" || m == "cbegin" || m == "end" || m == "cend")
        {
            std::vector<ASTNodePtr> el;
            el.push_back(self());
            el.push_back(m == "begin" || m == "cbegin" ? q::num(0, ln) : q::call("__cpp_size__", args1(self()), ln));
            return mkxv(q::arr(std::move(el), ln), iterOf(t));
        }
        if (m == "rbegin" || m == "rend" || m == "crbegin" || m == "crend")
        {
            // reverse iterators: a reversed view (read-only use)
            std::vector<ASTNodePtr> el;
            el.push_back(q::call("__cpp_reversed__", args1(self()), ln));
            el.push_back(m == "rbegin" || m == "crbegin" ? q::num(0, ln) : q::call("__cpp_size__", args1(self()), ln));
            return mkxv(q::arr(std::move(el), ln), iterOf(t));
        }
        if (m == "data")
        {
            std::vector<ASTNodePtr> el;
            el.push_back(self());
            el.push_back(q::num(0, ln));
            auto pt = mkT(T::Ptr);
            pt->a.push_back(et);
            return mkxv(q::arr(std::move(el), ln), pt);
        }
        if (m == "insert" || m == "emplace")
        {
            if (args.size() < 2)
                unsupported("insert needs a position", ln);
            XV pos = lower(0);
            if (args.size() == 2)
            {
                if (args[1]->k == EK::InitList)
                {
                    std::vector<ExprP> kids = args[1]->kids;
                    XV list = listInit(t, kids, ln);
                    return call("__cpp_insert_range__", args3(self(), read(pos), q::arr([&]
                                                                                        {
                                                                                            std::vector<ASTNodePtr> v;
                                                                                            v.push_back(read(list));
                                                                                            v.push_back(q::num(0, ln));
                                                                                            return v; }(),
                                                                                        ln)),
                                iterOf(t));
                }
                XV x = m == "insert" ? lowerTo(1, et) : lowerTo(1, et);
                return call("__cpp_insert__", args3(self(), read(pos), copyOf(std::move(x), ln)), iterOf(t));
            }
            XV a1 = lower(1);
            XV a2 = lower(2);
            if (a1.t->k == T::Iter || a1.t->k == T::Ptr)
            {
                std::vector<ASTNodePtr> v;
                v.push_back(self());
                v.push_back(read(pos));
                v.push_back(read(a1));
                v.push_back(read(a2));
                return call("__cpp_insert_range2__", std::move(v), iterOf(t));
            }
            // insert(pos, n, value)
            a2 = convert(std::move(a2), et, ln);
            std::vector<ASTNodePtr> v;
            v.push_back(self());
            v.push_back(read(pos));
            v.push_back(read(a1));
            v.push_back(read(a2));
            v.push_back(planFor(et));
            return call("__cpp_insert_n__", std::move(v), iterOf(t));
        }
        if (m == "erase")
        {
            XV a = lower(0);
            if (args.size() == 1)
                return call("__cpp_erase__", args3(self(), read(a), q::nil(ln)), iterOf(t));
            XV b = lower(1);
            return call("__cpp_erase__", args3(self(), read(a), read(b)), iterOf(t));
        }
        if (m == "resize")
        {
            XV n = lower(0);
            ASTNodePtr v = args.size() > 1 ? read(*std::make_shared<XV>(lowerTo(1, et))) : defaultValue(et, ln);
            std::vector<ASTNodePtr> a;
            a.push_back(self());
            a.push_back(read(n));
            a.push_back(std::move(v));
            a.push_back(planFor(et));
            return call("__cpp_resize__", std::move(a), tVoid());
        }
        if (m == "assign")
        {
            XV a = lower(0);
            XV b = lower(1);
            ASTNodePtr fresh;
            if (a.t->k == T::Iter || a.t->k == T::Ptr)
                fresh = q::call("__cpp_range_array__", args2(read(a), read(b)), ln);
            else
            {
                b = convert(std::move(b), et, ln);
                std::vector<ASTNodePtr> f;
                f.push_back(read(a));
                f.push_back(read(b));
                f.push_back(planFor(et));
                fresh = q::call("__cpp_fill__", std::move(f), ln);
            }
            return call("__cpp_assign__", args2(self(), std::move(fresh)), tVoid());
        }
        if (m == "fill")
        {
            XV v = lowerTo(0, et);
            std::vector<ASTNodePtr> a;
            a.push_back(self());
            a.push_back(read(v));
            a.push_back(planFor(et));
            return call("__cpp_fill_all__", std::move(a), tVoid());
        }
        if (m == "swap")
        {
            XV o = lower(0);
            return call("__cpp_swap_obj__", args2(self(), read(o)), tVoid());
        }
        break;
    }
    case T::Map:
    case T::Set:
    {
        bool isMap = t->k == T::Map;
        TP kt = t->a[0];
        TP mt = isMap ? t->a[1] : kt;
        if (m == "size")
            return call("__cpp_size__", args1(self()), tSize());
        if (m == "empty")
            return mkxv(q::bin("==", q::call("__cpp_size__", args1(self()), ln), q::num(0, ln), ln), tBool());
        if (m == "clear")
            return call("__cpp_clear__", args1(self()), tVoid());
        if (m == "count" || m == "contains")
        {
            XV k = lowerTo(0, kt);
            XV r = call("__cpp_dict_count__", args3(self(), read(k), q::boolean(t->multi, ln)), m == "count" ? tSize() : tBool());
            return r;
        }
        if (m == "find" || m == "lower_bound" || m == "upper_bound")
        {
            XV k = lowerTo(0, kt);
            std::vector<ASTNodePtr> a;
            a.push_back(q::str(m, ln));
            a.push_back(self());
            a.push_back(read(k));
            a.push_back(q::boolean(t->sorted, ln));
            return call("__cpp_dict_find__", std::move(a), iterOf(t));
        }
        if (m == "begin" || m == "end" || m == "cbegin" || m == "cend" || m == "rbegin" || m == "rend")
        {
            std::vector<ASTNodePtr> a;
            a.push_back(q::str(m, ln));
            a.push_back(self());
            a.push_back(q::boolean(t->sorted, ln));
            return call("__cpp_dict_iter__", std::move(a), iterOf(t));
        }
        if (m == "at" && isMap)
        {
            XV k = lowerTo(0, kt);
            ASTNodePtr pair = simple(q::call("__cpp_map_at__", args2(self(), read(k)), ln), ln);
            XV x;
            x.t = mt;
            x.lk = XV::Idx;
            x.obj = std::move(pair);
            x.key = q::num(1, ln);
            return x;
        }
        if (m == "erase")
        {
            XV a = lower(0);
            if (a.t->k == T::Iter)
            {
                std::vector<ASTNodePtr> v;
                v.push_back(self());
                v.push_back(read(a));
                v.push_back(q::boolean(t->sorted, ln));
                return call("__cpp_dict_erase_it__", std::move(v), iterOf(t));
            }
            a = convert(std::move(a), kt, ln);
            return call("__cpp_dict_erase__", args2(self(), read(a)), tSize());
        }
        if (m == "insert" || m == "emplace" || m == "try_emplace" || m == "insert_or_assign")
        {
            ASTNodePtr k, v;
            if (isMap)
            {
                if (args.size() >= 2 && m != "insert")
                {
                    XV kx = lowerTo(0, kt);
                    k = copyOf(std::move(kx), ln);
                    std::vector<ExprP> rest(args.begin() + 1, args.end());
                    XV vx = rest.size() == 1 ? lowerTo(1, mt) : makeObjectFrom(mt, rest, false, ln);
                    v = copyOf(std::move(vx), ln);
                }
                else
                {
                    auto pt = mkT(T::Pair);
                    pt->a = t->a;
                    XV p = lowerTo(args.size() == 2 ? 1 : 0, pt);
                    ASTNodePtr pv = simple(copyOf(std::move(p), ln), ln);
                    k = q::index(q::clone(pv), q::num(0, ln), ln);
                    v = q::index(std::move(pv), q::num(1, ln), ln);
                }
            }
            else
            {
                XV kx = lowerTo(args.size() == 2 ? 1 : 0, kt);
                k = copyOf(std::move(kx), ln);
                v = q::nil(ln);
            }
            auto rt = mkT(T::Pair);
            rt->a.push_back(iterOf(t));
            rt->a.push_back(tBool());
            std::vector<ASTNodePtr> a;
            a.push_back(self());
            a.push_back(std::move(k));
            a.push_back(std::move(v));
            a.push_back(q::boolean(t->multi, ln));
            a.push_back(q::boolean(m == "insert_or_assign", ln));
            a.push_back(q::boolean(t->sorted, ln));
            return call(isMap ? "__cpp_map_insert__" : "__cpp_set_insert__", std::move(a), rt);
        }
        if (m == "swap")
        {
            XV o = lower(0);
            return call("__cpp_swap_obj__", args2(self(), read(o)), tVoid());
        }
        break;
    }
    case T::Pair:
    case T::Tuple:
        if (m == "swap")
        {
            XV o = lower(0);
            return call("__cpp_swap_obj__", args2(self(), read(o)), tVoid());
        }
        break;
    case T::Optional:
    {
        TP vt = t->a[0];
        if (m == "has_value")
            return mkxv(q::un("!", q::call("__cpp_isnull__", args1(self()), ln), ln), tBool());
        if (m == "value")
        {
            XV x;
            x.t = vt;
            x.lk = XV::Idx;
            x.obj = simple(q::call("__cpp_opt_check__", args1(self()), ln), ln);
            x.key = q::num(0, ln);
            return x;
        }
        if (m == "value_or")
        {
            XV d = lowerTo(0, vt);
            return call("__cpp_opt_or__", args2(self(), read(d)), vt);
        }
        if (m == "reset")
            return mkxv(store(obj, q::nil(ln), ln), tVoid());
        if (m == "emplace")
        {
            std::vector<ExprP> a = args;
            XV v = a.size() == 1 ? lowerTo(0, vt) : makeObjectFrom(vt, a, false, ln);
            std::vector<ASTNodePtr> box;
            box.push_back(copyOf(std::move(v), ln));
            return mkxv(store(obj, q::arr(std::move(box), ln), ln), tVoid());
        }
        break;
    }
    case T::Variant:
        if (m == "index")
            return mkxv(q::index(self(), q::num(0, ln), ln), tSize());
        if (m == "valueless_by_exception")
            return mkxv(q::boolean(false, ln), tBool());
        break;
    case T::UPtr:
    {
        TP vt = t->a[0];
        auto pt = mkT(T::Ptr);
        pt->a.push_back(vt);
        if (m == "get")
            return mkxv(self(), pt, false);
        if (m == "release")
        {
            std::string tmp = tempName();
            if (pre_ && canHoist_)
            {
                pre_->push_back(q::var(tmp, self(), ln));
                ASTNodePtr st = store(obj, q::nil(ln), ln);
                return mkxv(q::call("__cpp_seq__", args2(std::move(st), q::id(tmp, ln)), ln), pt);
            }
            return mkxv(self(), pt);
        }
        if (m == "reset")
        {
            ASTNodePtr v = args.empty() ? q::nil(ln) : read(*std::make_shared<XV>(lower(0)));
            if (t->name == "shared_ptr" && !args.empty())
                v = q::call("__cpp_sp_new__", args1(std::move(v)), ln);
            if (ownsResource(t) && canHoist_ && pre_)
            {
                // the old pointee is let go
                ASTNodePtr old = simple(peekRead(obj), ln);
                std::vector<ASTNodePtr> a;
                a.push_back(store(obj, std::move(v), ln));
                a.push_back(destroyValue(t, std::move(old), ln));
                return mkxv(q::call("__cpp_seq__", std::move(a), ln), tVoid());
            }
            return mkxv(store(obj, std::move(v), ln), tVoid());
        }
        if (m == "use_count")
            return mkxv(q::call("__cpp_sp_count__", args1(self()), ln), tLL());
        if (m == "swap")
        {
            XV o = lower(0);
            XV ra = addressOf(std::move(obj), ln), rb = addressOf(std::move(o), ln);
            return mkxv(q::call("__cpp_swap__", args2(read(ra), read(rb)), ln), tVoid());
        }
        break;
    }
    case T::Stream:
    {
        if (m == "str")
        {
            if (args.empty())
                return call("__cpp_ss_str__", args1(self()), tStr());
            XV s = lower(0);
            return call("__cpp_ss_setstr__", args2(self(), read(s)), tVoid());
        }
        static const std::set<std::string> simpleMethods = {
            "good", "fail", "eof", "bad", "clear", "close", "is_open", "flush", "get", "peek", "ignore",
            "unget", "putback", "put", "precision", "width", "fill", "tellg", "seekg", "write", "rdbuf",
            "sync_with_stdio", "tie", "setf", "unsetf", "exceptions"};
        if (simpleMethods.count(m))
        {
            if (m == "get" && args.size() == 1)
            {
                // get(c): read one char into c
                XV dst = lower(0);
                ASTNodePtr s = simple(self(), ln);
                ASTNodePtr got = q::call("__cpp_stream_call__", args3(q::clone(s), q::str("get", ln), q::arr({}, ln)), ln);
                ASTNodePtr st = store(dst, std::move(got), ln);
                return mkxv(q::call("__cpp_seq__", args2(std::move(st), std::move(s)), ln), t, false);
            }
            std::vector<ASTNodePtr> rest;
            for (size_t i = 0; i < args.size(); ++i)
            {
                XV x = lower(i);
                rest.push_back(read(x));
            }
            TP rt = tVoid();
            if (m == "good" || m == "fail" || m == "eof" || m == "bad" || m == "is_open")
                rt = tBool();
            else if (m == "get" || m == "peek")
                rt = tInt();
            else if (m == "precision" || m == "width" || m == "tellg")
                rt = tLL();
            else if (m == "rdbuf")
                rt = mkT(T::Dyn, "rdbuf");
            else if (m == "ignore" || m == "put" || m == "write" || m == "flush" || m == "seekg" || m == "unget" ||
                     m == "putback")
                rt = t;
            return call("__cpp_stream_call__", args3(self(), q::str(m, ln), q::arr(std::move(rest), ln)), rt, false);
        }
        if (m == "getline")
        {
            // is.getline(buf, n): into a string buffer
            XV dst = lower(0);
            ASTNodePtr s = simple(self(), ln);
            ASTNodePtr got = q::call("__cpp_getline__", args1(q::clone(s)), ln);
            ASTNodePtr st = store(dst, std::move(got), ln);
            return mkxv(q::call("__cpp_seq__", args2(std::move(st), std::move(s)), ln), t, false);
        }
        break;
    }
    case T::Func:
        if (m == "operator()")
            return callValue(std::move(obj), args, ln);
        break;
    case T::Iter:
        break;
    default:
        break;
    }
    handled = false;
    return XV();
}

} // namespace cpp
