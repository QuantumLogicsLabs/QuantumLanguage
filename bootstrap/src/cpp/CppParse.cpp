// Parser for the typed C/C++ front-end (see Cpp.h): tokens → C++ AST.
//
// Declarations and expressions are told apart the way a C++ compiler does it,
// approximately: a table of names known to denote types (built-in, standard
// library, and every class / enum / alias / template parameter declared so
// far), plus speculative parsing with backtracking for statements.
#include "Cpp.h"

#include <cstring>
#include <functional>

namespace cpp
{

std::string TypeName::str() const
{
    std::string s = isConst ? "const " : "";
    if (isFunction)
    {
        s += (ret ? ret->str() : "void") + "(";
        for (size_t i = 0; i < params.size(); ++i)
            s += (i ? "," : "") + params[i]->str();
        s += ")";
    }
    for (size_t i = 0; i < name.size(); ++i)
    {
        s += (i ? "::" : "") + name[i];
        if (i < targs.size() && !targs[i].empty())
        {
            s += "<";
            for (size_t j = 0; j < targs[i].size(); ++j)
                s += (j ? "," : "") + (targs[i][j].type ? targs[i][j].type->str() : std::string("#"));
            s += ">";
        }
    }
    for (int i = 0; i < pointers; ++i)
        s += "*";
    if (ref)
        s += "&";
    if (rref)
        s += "&&";
    for (size_t i = 0; i < arrayDims.size(); ++i)
        s += "[]";
    return s;
}

namespace
{

struct ParseFail
{
    std::string msg;
    int line;
    int file = 0;
    int col = 0;
};

const std::set<std::string> &builtinWords()
{
    static const std::set<std::string> w = {"void", "bool", "char", "short", "int", "long", "float", "double",
                                            "signed", "unsigned", "wchar_t", "char8_t", "char16_t", "char32_t",
                                            "auto"};
    return w;
}

const std::set<std::string> &specifierWords()
{
    static const std::set<std::string> w = {"const", "volatile", "static", "extern", "inline", "constexpr",
                                            "consteval", "constinit", "mutable", "virtual", "explicit",
                                            "friend", "typedef", "thread_local", "register", "typename"};
    return w;
}

class Parser
{
public:
    explicit Parser(const Preprocessed &pp) : t_(pp.tokens), files_(pp.files), cMode_(pp.cMode)
    {
        if (cMode_)
        {
            // C: only the typedef names its standard headers declare.
            static const char *c_types[] = {
                "size_t", "ptrdiff_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
                "uint32_t", "uint64_t", "intptr_t", "uintptr_t", "intmax_t", "uintmax_t", "FILE", "time_t",
                "clock_t", "ssize_t", "wchar_t", "va_list", "nullptr_t"};
            for (const char *n : c_types)
                types_.insert(n);
            return;
        }
        static const char *std_types[] = {
            "string", "wstring", "string_view", "vector", "map", "unordered_map", "set", "unordered_set",
            "multimap", "multiset", "pair", "tuple", "optional", "variant", "monostate", "unique_ptr",
            "shared_ptr", "weak_ptr", "function", "array", "deque", "list", "forward_list", "stack", "queue",
            "priority_queue", "ostream", "istream", "iostream", "ostringstream", "istringstream", "stringstream",
            "ifstream", "ofstream", "fstream", "exception", "runtime_error", "logic_error", "invalid_argument",
            "out_of_range", "length_error", "domain_error", "range_error", "overflow_error", "underflow_error",
            "bad_alloc", "bad_variant_access", "bad_optional_access", "bad_cast", "size_t", "ptrdiff_t",
            "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
            "intptr_t", "uintptr_t", "intmax_t", "uintmax_t", "nullptr_t", "initializer_list", "numeric_limits",
            "nullopt_t", "streamsize", "bitset", "less", "greater", "hash", "mt19937", "mt19937_64",
            "random_device", "uniform_int_distribution", "uniform_real_distribution", "FILE", "time_t",
            "clock_t", "any", "reference_wrapper", "ssize_t", "byte"};
        for (const char *n : std_types)
            types_.insert(n);
        static const char *std_templates[] = {
            "vector", "map", "unordered_map", "set", "unordered_set", "multimap", "multiset", "pair", "tuple",
            "optional", "variant", "unique_ptr", "shared_ptr", "weak_ptr", "function", "array", "deque", "list",
            "forward_list", "stack", "queue", "priority_queue", "numeric_limits", "initializer_list", "bitset",
            "less", "greater", "hash", "uniform_int_distribution", "uniform_real_distribution",
            "reference_wrapper", "basic_string",
            // function templates called with explicit arguments
            "static_cast", "dynamic_cast", "const_cast", "reinterpret_cast", "make_unique", "make_shared",
            "get", "get_if", "holds_alternative", "max", "min", "make_pair", "make_tuple", "any_cast",
            "accumulate", "to_underlying", "clamp", "swap", "forward", "move", "declval", "bit_cast"};
        for (const char *n : std_templates)
            templates_.insert(n);
    }

    Program run()
    {
        Program prog;
        prog.files = files_;
        prog.cMode = cMode_;
        while (!at(TK::End))
        {
            auto ds = parseDecl(false, "");
            prog.decls.insert(prog.decls.end(), ds.begin(), ds.end());
        }
        return prog;
    }

private:
    std::vector<Token> t_;
    const std::vector<std::string> &files_;
    bool cMode_ = false; // parsing C, not C++ (Preprocessed::cMode)
    std::set<std::string> cTags_; // C struct/union tags: types only after their keyword
    size_t p_ = 0;
    std::set<std::string> types_;     // last name component of every known type
    std::set<std::string> templates_; // names that take <template arguments>
    std::vector<std::string> classStack_; // enclosing class names while parsing members

    // ─── Token helpers ─────────────────────────────────────────────────────

    const Token &cur() const { return t_[p_]; }
    const Token &peekTok(size_t k = 1) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
    bool at(TK k) const { return cur().kind == k; }
    bool is(const char *s) const
    {
        return (cur().kind == TK::Punct || cur().kind == TK::Ident) && cur().text == s;
    }
    bool isAt(size_t k, const char *s) const
    {
        const Token &x = peekTok(k);
        return (x.kind == TK::Punct || x.kind == TK::Ident) && x.text == s;
    }
    bool accept(const char *s)
    {
        if (is(s))
        {
            ++p_;
            return true;
        }
        return false;
    }
    // The parser backtracks (declaration first, then expression), so the
    // failure that propagates is often a later, shallower attempt. The one
    // that got furthest into the tokens is the parse the author meant: it is
    // what a syntax error is reported as (see parse()).
    [[noreturn]] void fail(const std::string &msg) const
    {
        ParseFail f{msg, cur().line, cur().file, cur().col};
        if (!haveFurthest_ || p_ > furthestPos_)
        {
            furthest_ = f;
            furthestPos_ = p_;
            haveFurthest_ = true;
        }
        throw f;
    }

public:
    const ParseFail &furthestFailure(const ParseFail &last) const { return haveFurthest_ ? furthest_ : last; }

private:
    mutable ParseFail furthest_{"", 0};
    mutable size_t furthestPos_ = 0;
    mutable bool haveFurthest_ = false;

public:
    void expect(const char *s)
    {
        if (!accept(s))
            fail(std::string("expected '") + s + "' near '" + cur().text + "'");
    }
    std::string ident()
    {
        if (!at(TK::Ident))
            fail("expected identifier near '" + cur().text + "'");
        return t_[p_++].text;
    }
    // `>>` closing two template argument lists at once.
    void expectCloseAngle()
    {
        if (is(">"))
        {
            ++p_;
            return;
        }
        if (is(">>"))
        {
            t_[p_].text = ">";
            return; // the first '>' is consumed by turning '>>' into '>'
        }
        if (is(">="))
        {
            t_[p_].text = "=";
            return;
        }
        if (is(">>="))
        {
            t_[p_].text = ">=";
            return;
        }
        fail("expected '>'");
    }

    bool isKeyword(const std::string &w) const
    {
        static const std::set<std::string> kw = {
            "if", "else", "while", "do", "for", "switch", "case", "default", "break", "continue", "return",
            "try", "catch", "throw", "goto", "new", "delete", "sizeof", "this", "true", "false", "nullptr",
            "struct", "class", "union", "enum", "namespace", "using", "typedef", "template", "operator",
            "public", "private", "protected", "static_cast", "dynamic_cast", "const_cast", "reinterpret_cast",
            "static_assert", "decltype", "noexcept", "alignof", "alignas"};
        return kw.count(w) > 0;
    }

    bool isTypeWord(const Token &x) const
    {
        if (x.kind != TK::Ident)
            return false;
        return builtinWords().count(x.text) || types_.count(x.text);
    }

    // Starts a declaration: specifiers, a built-in or known type, `std::`
    // followed by a known type, or `struct X` style elaborated names.
    bool startsType(size_t k = 0) const
    {
        const Token &x = peekTok(k);
        if (x.kind != TK::Ident)
            return x.kind == TK::Punct && x.text == "::" && peekTok(k + 1).kind == TK::Ident &&
                   startsType(k + 1);
        if (specifierWords().count(x.text) || builtinWords().count(x.text))
            return true;
        if (x.text == "struct" || x.text == "class" || x.text == "enum" || x.text == "union" ||
            x.text == "decltype")
            return true;
        if (types_.count(x.text))
        {
            // `T::member` where member isn't a type would be an expression,
            // but in practice T::x after a type name is a nested type or a
            // static member call — let the speculative parse decide.
            return true;
        }
        // ns::Type
        size_t j = k;
        while (peekTok(j).kind == TK::Ident && peekTok(j + 1).kind == TK::Punct && peekTok(j + 1).text == "::")
            j += 2;
        if (j != k && peekTok(j).kind == TK::Ident && types_.count(peekTok(j).text))
            return true;
        return false;
    }

    // ─── Types ─────────────────────────────────────────────────────────────

    std::vector<TArg> parseTemplateArgs()
    {
        std::vector<TArg> args;
        expect("<");
        if (is(">") || is(">>"))
        {
            expectCloseAngle();
            return args;
        }
        while (true)
        {
            TArg a;
            size_t save = p_;
            bool asType = false;
            if (startsType() || (at(TK::Ident) && !isKeyword(cur().text) &&
                                 (isAt(1, ">") || isAt(1, ",") || isAt(1, ">>") || isAt(1, "*") || isAt(1, "&") ||
                                  isAt(1, "<") || isAt(1, "("))))
            {
                try
                {
                    a.type = parseTypeId();
                    if (is(">") || is(",") || is(">>") || is(">=") || is(">>=") || is("..."))
                        asType = true;
                    else
                        p_ = save;
                }
                catch (ParseFail &)
                {
                    p_ = save;
                }
            }
            if (!asType)
            {
                a.type.reset();
                a.expr = parseCondExpr(true);
            }
            accept("...");
            args.push_back(a);
            if (accept(","))
                continue;
            expectCloseAngle();
            break;
        }
        return args;
    }

    // Specifier words before/around the type; returns the flags it saw.
    struct Specs
    {
        bool isConst = false, isStatic = false, isExtern = false, isInline = false, isConstexpr = false;
        bool isVirtual = false, isExplicit = false, isFriend = false, isTypedef = false, isMutable = false;
    };

    void parseSpecWords(Specs &s)
    {
        while (at(TK::Ident) && specifierWords().count(cur().text))
        {
            const std::string &w = cur().text;
            if (w == "const" || w == "constexpr")
                s.isConst = s.isConst || w == "const";
            if (w == "constexpr" || w == "consteval")
                s.isConstexpr = true;
            if (w == "static")
                s.isStatic = true;
            if (w == "extern")
                s.isExtern = true;
            if (w == "inline")
                s.isInline = true;
            if (w == "virtual")
                s.isVirtual = true;
            if (w == "explicit")
                s.isExplicit = true;
            if (w == "friend")
                s.isFriend = true;
            if (w == "typedef")
                s.isTypedef = true;
            if (w == "mutable")
                s.isMutable = true;
            ++p_;
            if (w == "extern" && at(TK::String))
                ++p_; // extern "C"
            if (w == "explicit" && is("("))
                skipBalanced("(", ")");
        }
    }

    void skipAttributes()
    {
        while (true)
        {
            if (is("[") && isAt(1, "["))
            {
                int depth = 0;
                do
                {
                    if (is("["))
                        ++depth;
                    else if (is("]"))
                        --depth;
                    ++p_;
                } while (depth > 0 && !at(TK::End));
                continue;
            }
            if (is("__attribute__") || is("__declspec") || is("alignas"))
            {
                ++p_;
                if (is("("))
                    skipBalanced("(", ")");
                continue;
            }
            break;
        }
    }

    void skipBalanced(const char *open, const char *close)
    {
        int depth = 0;
        do
        {
            if (is(open))
                ++depth;
            else if (is(close))
                --depth;
            ++p_;
        } while (depth > 0 && !at(TK::End));
    }

    // The base type of a declaration (decl-specifiers), without declarators.
    TypeNameP parseBaseType(Specs &specs)
    {
        auto ty = std::make_shared<TypeName>();
        skipAttributes();
        parseSpecWords(specs);
        skipAttributes();
        // built-in multiword
        std::vector<std::string> words;
        while (at(TK::Ident) && (builtinWords().count(cur().text) || cur().text == "const" ||
                                 cur().text == "volatile"))
        {
            if (cur().text == "const")
                ty->isConst = true;
            else if (cur().text != "volatile")
                words.push_back(cur().text);
            ++p_;
            parseSpecWords(specs);
        }
        if (!words.empty())
        {
            ty->name.push_back(normalizeBuiltin(words));
            ty->targs.emplace_back();
        }
        else
        {
            if (is("struct") || is("class") || is("union") || is("enum") || is("typename"))
            {
                ty->elaborated = !is("typename");
                ++p_;
            }
            if (is("decltype"))
            {
                ++p_;
                skipBalanced("(", ")");
                ty->name.push_back("auto"); // dynamic
                ty->targs.emplace_back();
            }
            else
            {
                accept("::");
                while (true)
                {
                    if (!at(TK::Ident) || isKeyword(cur().text))
                        fail("expected a type near '" + cur().text + "'");
                    std::string n = ident();
                    ty->name.push_back(n);
                    ty->targs.emplace_back();
                    if (is("<") && (templates_.count(n) || types_.count(n)))
                    {
                        ty->targs.back() = parseTemplateArgs();
                        ty->hasTargs = true;
                    }
                    if (is("::") && peekTok().kind == TK::Ident)
                    {
                        ++p_;
                        accept("template");
                        continue;
                    }
                    break;
                }
            }
        }
        // trailing cv / specifiers: `int const`, `std::string const&`
        while (at(TK::Ident) && (cur().text == "const" || cur().text == "volatile" ||
                                 specifierWords().count(cur().text)))
        {
            if (cur().text == "const")
                ty->isConst = true;
            else
                parseSpecWords(specs);
            if (cur().text == "const" || cur().text == "volatile")
                ++p_;
        }
        if (specs.isConst)
            ty->isConst = true;
        return ty;
    }

    static std::string normalizeBuiltin(const std::vector<std::string> &w)
    {
        int longs = 0;
        bool isUnsigned = false, isSigned = false, isShort = false, isChar = false, isInt = false;
        std::string other;
        for (const auto &x : w)
        {
            if (x == "long")
                ++longs;
            else if (x == "unsigned")
                isUnsigned = true;
            else if (x == "signed")
                isSigned = true;
            else if (x == "short")
                isShort = true;
            else if (x == "char")
                isChar = true;
            else if (x == "int")
                isInt = true;
            else
                other = x;
        }
        if (!other.empty())
        {
            if (other == "double" && longs)
                return "long double";
            return other; // void bool float double auto wchar_t ...
        }
        if (isChar)
            return isUnsigned ? "unsigned char" : isSigned ? "signed char" : "char";
        std::string base = isShort ? "short" : longs >= 2 ? "long long" : longs == 1 ? "long" : "int";
        (void)isInt;
        return isUnsigned ? "unsigned " + base : base;
    }

    // Pointer / reference part of a declarator.
    void parsePtrOps(TypeName &ty)
    {
        while (true)
        {
            skipAttributes();
            if (accept("*"))
            {
                ++ty.pointers;
                bool c = false;
                while (is("const") || is("volatile") || is("__restrict") || is("restrict"))
                {
                    c = c || is("const");
                    ++p_;
                }
                ty.constPtr.push_back(c);
                continue;
            }
            if (cMode_ && (is("&") || is("&&")))
                fail("expected ';', ',' or ')' near '" + cur().text + "'"); // C has no references
            if (accept("&"))
            {
                ty.ref = true;
                continue;
            }
            if (accept("&&"))
            {
                ty.rref = true;
                continue;
            }
            break;
        }
    }

    // A type with an abstract declarator: template args, casts, sizeof, new.
    TypeNameP parseTypeId()
    {
        Specs s;
        auto ty = parseBaseType(s);
        parsePtrOps(*ty);
        // function type: R(Args)  or function pointer R(*)(Args)
        if (is("(") && (isAt(1, ")") || isAt(1, "*") || startsTypeAt(1)))
        {
            size_t save = p_;
            try
            {
                if (isAt(1, "*"))
                {
                    expect("(");
                    expect("*");
                    expect(")");
                }
                auto fn = std::make_shared<TypeName>();
                fn->isFunction = true;
                fn->ret = ty;
                bool variadic = false;
                for (auto &prm : parseParams(variadic))
                    fn->params.push_back(prm.type);
                if (isAt(0, "const"))
                    ++p_;
                accept("noexcept");
                parsePtrOps(*fn);
                return fn;
            }
            catch (ParseFail &)
            {
                p_ = save;
            }
        }
        while (is("[") )
        {
            ++p_;
            if (is("]"))
                ty->arrayDims.push_back(nullptr);
            else
                ty->arrayDims.push_back(parseExpr());
            expect("]");
        }
        return ty;
    }

    bool startsTypeAt(size_t k) const { return startsType(k); }

    std::vector<Param> parseParams(bool &variadic)
    {
        std::vector<Param> ps;
        expect("(");
        if (is("void") && isAt(1, ")"))
            ++p_;
        while (!is(")"))
        {
            if (accept("..."))
            {
                variadic = true;
                break;
            }
            Param prm;
            Specs s;
            auto base = parseBaseType(s);
            auto ty = std::make_shared<TypeName>(*base);
            parsePtrOps(*ty);
            if (is("(") && isAt(1, "*"))
            {
                // function pointer parameter: R (*name)(Args)
                expect("(");
                expect("*");
                if (at(TK::Ident))
                    prm.name = ident();
                expect(")");
                auto fn = std::make_shared<TypeName>();
                fn->isFunction = true;
                fn->ret = ty;
                bool v2 = false;
                for (auto &x : parseParams(v2))
                    fn->params.push_back(x.type);
                fn->pointers = 1;
                ty = fn;
            }
            else
            {
                accept("...");
                if (at(TK::Ident) && !isKeyword(cur().text))
                    prm.name = ident();
                bool decayed = false;
                while (is("["))
                {
                    ++p_;
                    ExprP dim = is("]") ? nullptr : parseExpr();
                    expect("]");
                    if (!decayed)
                    {
                        // the array decays to a pointer to its first element
                        ++ty->pointers;
                        ty->constPtr.push_back(false);
                        decayed = true;
                    }
                    else
                        ty->pointeeDims.push_back(dim);
                }
            }
            skipAttributes();
            prm.type = ty;
            if (cMode_ && is("="))
                fail("expected ';', ',' or ')' near '='"); // C has no default arguments
            if (accept("="))
                prm.def = parseAssignExpr();
            ps.push_back(prm);
            if (!accept(","))
                break;
        }
        expect(")");
        return ps;
    }

    // ─── Declarations ──────────────────────────────────────────────────────

    std::vector<std::string> parseTemplateHeader(bool &isTemplate)
    {
        std::vector<std::string> params;
        isTemplate = false;
        while (is("template"))
        {
            isTemplate = true;
            ++p_;
            expect("<");
            while (!is(">"))
            {
                if (is("typename") || is("class"))
                {
                    ++p_;
                    accept("...");
                    if (at(TK::Ident))
                    {
                        std::string n = ident();
                        params.push_back(n);
                        types_.insert(n);
                    }
                    if (accept("="))
                        parseTypeId();
                }
                else
                {
                    // non-type parameter: int N = 3
                    Specs s;
                    parseBaseType(s);
                    accept("...");
                    if (at(TK::Ident))
                        params.push_back(ident());
                    if (accept("="))
                        parseCondExpr(true);
                }
                if (!accept(","))
                    break;
            }
            expectCloseAngle();
        }
        return params;
    }

    // One declaration at namespace or class scope (`cls` = enclosing class
    // name). A declaration with several declarators yields several Decls.
    std::vector<DeclP> parseDecl(bool inClass, const std::string &cls)
    {
        int line = cur().line;
        int file = cur().file, tu = cur().tu;
        auto mk = [&](DK k)
        {
            auto d = std::make_shared<Decl>();
            d->k = k;
            d->line = line;
            d->file = file;
            d->tu = tu;
            return d;
        };
        skipAttributes();
        if (accept(";"))
            return {};
        if (inClass && (is("public") || is("private") || is("protected")) && isAt(1, ":"))
        {
            p_ += 2;
            return {};
        }
        bool isTemplate = false;
        auto tparams = parseTemplateHeader(isTemplate);
        if (is("namespace"))
        {
            ++p_;
            auto d = mk(DK::Namespace);
            if (at(TK::Ident))
            {
                d->name.push_back(ident());
                while (accept("::"))
                {
                    accept("inline");
                    d->name.push_back(ident());
                }
            }
            else
                d->anonymous = true;
            if (accept("="))
            {
                // namespace alias: namespace fs = std::filesystem;
                while (!is(";"))
                    ++p_;
                expect(";");
                return {};
            }
            expect("{");
            while (!is("}") && !at(TK::End))
            {
                auto ds = parseDecl(false, "");
                d->decls.insert(d->decls.end(), ds.begin(), ds.end());
            }
            expect("}");
            return {d};
        }
        if (is("inline") && isAt(1, "namespace"))
        {
            ++p_;
            return parseDecl(inClass, cls);
        }
        if (is("extern") && peekTok().kind == TK::String && isAt(2, "{"))
        {
            p_ += 3;
            std::vector<DeclP> all;
            while (!is("}") && !at(TK::End))
            {
                auto ds = parseDecl(false, "");
                all.insert(all.end(), ds.begin(), ds.end());
            }
            expect("}");
            return all;
        }
        if (is("using"))
        {
            ++p_;
            if (accept("namespace"))
            {
                auto d = mk(DK::UsingDirective);
                accept("::");
                d->name.push_back(ident());
                while (accept("::"))
                    d->name.push_back(ident());
                expect(";");
                return {d};
            }
            if (at(TK::Ident) && isAt(1, "="))
            {
                auto d = mk(DK::Alias);
                d->name.push_back(ident());
                expect("=");
                d->aliased = parseTypeId();
                expect(";");
                types_.insert(d->name[0]);
                d->isTemplate = isTemplate;
                d->templateParams = tparams;
                if (isTemplate)
                    templates_.insert(d->name[0]);
                return {d};
            }
            auto d = mk(DK::UsingDecl);
            accept("typename");
            accept("::");
            d->name.push_back(ident());
            while (accept("::"))
                d->name.push_back(ident());
            expect(";");
            if (d->name.size() >= 2 && d->name[d->name.size() - 1] == d->name[d->name.size() - 2])
                d->inheritingCtor = true;
            // using std::string; → `string` names a type here too
            if (types_.count(d->name.back()))
                types_.insert(d->name.back());
            return {d};
        }
        if (is("static_assert"))
        {
            ++p_;
            skipBalanced("(", ")");
            expect(";");
            return {};
        }
        if (is("typedef"))
        {
            ++p_;
            Specs s;
            // typedef struct X { ... } Y;
            if ((is("struct") || is("class") || is("union") || is("enum")) &&
                (isAt(1, "{") || (peekTok().kind == TK::Ident && isAt(2, "{"))))
            {
                auto inner = parseClassOrEnum(inClass, mk);
                // typedef struct { ... } Name;  the anonymous struct takes the typedef's name
                bool anon = inner->name.empty() || inner->name.back().empty();
                if (anon)
                {
                    size_t k = 0;
                    while (peekTok(k).kind == TK::Punct && peekTok(k).text == "*")
                        ++k;
                    inner->name.clear();
                    inner->name.push_back(peekTok(k).text);
                }
                std::vector<DeclP> out = {inner};
                do
                {
                    auto ty = std::make_shared<TypeName>();
                    ty->name.push_back(inner->name.back());
                    ty->targs.emplace_back();
                    parsePtrOps(*ty);
                    std::string aliasName = ident();
                    types_.insert(aliasName);
                    if (aliasName == inner->name.back() && ty->pointers == 0)
                        continue; // the struct itself (typedef struct Node {...} Node;)
                    auto a = mk(DK::Alias);
                    a->name.push_back(aliasName);
                    a->aliased = ty;
                    out.push_back(a);
                } while (accept(","));
                expect(";");
                return out;
            }
            auto base = parseBaseType(s);
            std::vector<DeclP> out;
            do
            {
                auto ty = std::make_shared<TypeName>(*base);
                parsePtrOps(*ty);
                auto a = mk(DK::Alias);
                if (is("(") && isAt(1, "*"))
                {
                    // typedef R (*Name)(Args);
                    expect("(");
                    expect("*");
                    a->name.push_back(ident());
                    expect(")");
                    auto fn = std::make_shared<TypeName>();
                    fn->isFunction = true;
                    fn->ret = ty;
                    bool v = false;
                    for (auto &x : parseParams(v))
                        fn->params.push_back(x.type);
                    fn->pointers = 1;
                    ty = fn;
                }
                else
                {
                    a->name.push_back(ident());
                    while (is("["))
                    {
                        ++p_;
                        ty->arrayDims.push_back(is("]") ? nullptr : parseExpr());
                        expect("]");
                    }
                }
                a->aliased = ty;
                types_.insert(a->name[0]);
                out.push_back(a);
            } while (accept(","));
            expect(";");
            return out;
        }
        if ((is("struct") || is("class") || is("union") || is("enum")) &&
            (isAt(1, "{") || (peekTok().kind == TK::Ident &&
                              (isAt(2, "{") || isAt(2, ":") || isAt(2, ";") || isAt(2, "final") ||
                               (isAt(1, "class") && isAt(3, "{")) || (isAt(1, "class") && isAt(3, ":")) ||
                               (isAt(1, "struct") && (isAt(3, "{") || isAt(3, ":")))))))
        {
            auto d = parseClassOrEnum(inClass, mk);
            d->isTemplate = isTemplate;
            d->templateParams = tparams;
            if (isTemplate)
                templates_.insert(d->name.back());
            std::vector<DeclP> out = {d};
            // struct X { ... } a, b;
            if (!is(";"))
            {
                auto base = std::make_shared<TypeName>();
                base->name.push_back(d->name.back());
                base->targs.emplace_back();
                Specs s;
                parseVarDeclarators(base, s, out, mk, inClass);
                return out;
            }
            expect(";");
            return out;
        }

        // Function or variable.
        Specs specs;
        skipAttributes();
        parseSpecWords(specs);
        skipAttributes();
        bool noreturn = false;
        // constructor / destructor / conversion operator have no return type
        bool ctorLike = false;
        if (inClass && at(TK::Ident) && cur().text == cls && isAt(1, "("))
            ctorLike = true;
        if (inClass && is("~"))
            ctorLike = true;
        if (is("operator"))
            ctorLike = true; // conversion operator: operator bool()
        // out-of-class ctor/dtor: Cls::Cls( / Cls::~Cls(
        if (!inClass && at(TK::Ident))
        {
            size_t j = 0;
            std::vector<std::string> q;
            while (peekTok(j).kind == TK::Ident && isAt(j + 1, "::"))
            {
                q.push_back(peekTok(j).text);
                j += 2;
                if (isAt(j, "<"))
                    break;
            }
            if (!q.empty() && ((peekTok(j).kind == TK::Ident && peekTok(j).text == q.back() && isAt(j + 1, "(")) ||
                               isAt(j, "~") || (isAt(j, "operator") && !startsAfterOperatorIsSymbol(j))))
                ctorLike = true;
        }
        TypeNameP base;
        if (!ctorLike)
            base = parseBaseType(specs);
        else
        {
            base = std::make_shared<TypeName>();
            base->name.push_back("void");
            base->targs.emplace_back();
        }
        std::vector<DeclP> out;
        // Declarator(s)
        auto ty = std::make_shared<TypeName>(*base);
        parsePtrOps(*ty);
        skipAttributes();
        // function pointer variable: R (*name)(Args) = ...;
        if (is("(") && isAt(1, "*"))
        {
            expect("(");
            expect("*");
            auto d = mk(DK::Var);
            std::string n = ident();
            std::vector<ExprP> dims; // R (*name[N])(Args): an array of them
            while (is("["))
            {
                ++p_;
                dims.push_back(is("]") ? nullptr : parseExpr());
                expect("]");
            }
            expect(")");
            auto fn = std::make_shared<TypeName>();
            fn->isFunction = true;
            fn->ret = ty;
            bool v = false;
            for (auto &x : parseParams(v))
                fn->params.push_back(x.type);
            fn->pointers = 1;
            fn->arrayDims = dims;
            d->name.push_back(n);
            d->var.name = n;
            d->var.type = fn;
            d->var.line = line;
            d->isStatic = specs.isStatic;
            if (accept("="))
            {
                d->var.init = VarDeclarator::Copy;
                d->var.args.push_back(parseAssignExpr());
            }
            expect(";");
            return {d};
        }
        // name (qualified, operator, destructor)
        std::vector<std::string> qname;
        bool isOperator = false, isDtor = false;
        while (true)
        {
            if (is("~"))
            {
                ++p_;
                isDtor = true;
                qname.push_back("~" + ident());
                break;
            }
            if (is("operator"))
            {
                ++p_;
                isOperator = true;
                qname.push_back("operator" + parseOperatorName());
                break;
            }
            if (!at(TK::Ident) || isKeyword(cur().text))
                fail("expected a declarator name near '" + cur().text + "'");
            qname.push_back(ident());
            if (is("<") && !qname.empty() && templates_.count(qname.back()) && isAt(0, "<"))
            {
                // explicit specialization / class template member: Cls<T>::f
                size_t save = p_;
                try
                {
                    parseTemplateArgs();
                }
                catch (ParseFail &)
                {
                    p_ = save;
                }
            }
            if (is("::"))
            {
                ++p_;
                continue;
            }
            break;
        }
        skipAttributes();
        bool isFunc = is("(") && !(inClass == false && looksLikeDirectInit());
        if (isFunc)
        {
            auto d = mk(DK::Func);
            d->name = qname;
            d->ret = ty;
            d->isOperator = isOperator;
            d->isDtor = isDtor;
            d->isStatic = specs.isStatic;
            d->isVirtual = specs.isVirtual;
            d->isExplicit = specs.isExplicit;
            d->isFriend = specs.isFriend;
            d->isInline = specs.isInline;
            d->isConstexpr = specs.isConstexpr;
            d->noreturn = noreturn;
            d->isTemplate = isTemplate;
            d->templateParams = tparams;
            if (isTemplate)
                templates_.insert(qname.back());
            std::string owner = inClass ? cls : (qname.size() >= 2 ? qname[qname.size() - 2] : "");
            d->isCtor = !isDtor && !isOperator && !owner.empty() && qname.back() == owner;
            if (d->isCtor || isDtor)
            {
                d->ret = std::make_shared<TypeName>();
                d->ret->name.push_back("void");
                d->ret->targs.emplace_back();
            }
            if (isOperator && ctorLike && !d->isCtor)
            {
                // conversion operator: the type is in the name
                d->ret = conversionType_ ? conversionType_ : d->ret;
            }
            d->params = parseParams(d->variadic);
            // qualifiers
            while (true)
            {
                if (accept("const"))
                    d->isConst = true;
                else if (is("volatile") || is("&") || is("&&") || is("override") || is("final"))
                    ++p_;
                else if (accept("noexcept"))
                {
                    if (is("("))
                        skipBalanced("(", ")");
                }
                else if (is("throw") && isAt(1, "("))
                {
                    ++p_;
                    skipBalanced("(", ")");
                }
                else if (is("->"))
                {
                    ++p_;
                    d->ret = parseTypeId(); // trailing return type
                }
                else
                    break;
            }
            skipAttributes();
            if (accept("="))
            {
                if (accept("0"))
                    d->isPure = true;
                else if (accept("default"))
                    d->isDefault = true;
                else if (accept("delete"))
                    d->isDeleted = true;
                else if (at(TK::Int))
                {
                    ++p_;
                    d->isPure = true;
                }
                expect(";");
                return {d};
            }
            if (accept(";"))
                return {d};
            if (is("try"))
                ++p_; // function-try-block (rare): treat the body as normal
            if (accept(":"))
            {
                // member initializer list
                while (true)
                {
                    MemInit mi;
                    accept("::");
                    mi.name = ident();
                    while (accept("::"))
                        mi.name = ident();
                    if (is("<"))
                        parseTemplateArgs();
                    if (is("{"))
                    {
                        mi.braced = true;
                        mi.args = parseBraceList();
                    }
                    else
                    {
                        expect("(");
                        if (!is(")"))
                            mi.args = parseExprList(")");
                        expect(")");
                    }
                    d->inits.push_back(mi);
                    if (!accept(","))
                        break;
                }
            }
            classStack_.push_back(owner);
            d->body = parseBlock();
            classStack_.pop_back();
            if (is("catch"))
                parseCatches(); // discard handlers of a function-try-block
            return {d};
        }
        // variables
        auto first = mk(DK::Var);
        first->name = qname;
        finishVarDeclarator(*first, ty, specs);
        out.push_back(first);
        while (accept(","))
        {
            auto t2 = std::make_shared<TypeName>(*base);
            parsePtrOps(*t2);
            auto d = mk(DK::Var);
            d->name.push_back(ident());
            finishVarDeclarator(*d, t2, specs);
            out.push_back(d);
        }
        if (inClass && accept(":"))
            parseCondExpr(false); // bit-field width
        expect(";");
        return out;
    }

    TypeNameP conversionType_;

    bool startsAfterOperatorIsSymbol(size_t j) const
    {
        const Token &x = peekTok(j + 1);
        return x.kind == TK::Punct;
    }

    std::string parseOperatorName()
    {
        conversionType_.reset();
        if (accept("("))
        {
            expect(")");
            return "()";
        }
        if (accept("["))
        {
            expect("]");
            return "[]";
        }
        if (is("new") || is("delete"))
        {
            std::string w = ident();
            if (accept("["))
            {
                expect("]");
                w += "[]";
            }
            return " " + w;
        }
        if (at(TK::Punct))
            return t_[p_++].text;
        if (at(TK::String) && cur().value.empty())
        {
            ++p_; // operator"" _suffix
            return "\"\"" + ident();
        }
        // conversion operator: operator bool / operator int / operator std::string
        Specs s;
        auto ty = parseBaseType(s);
        parsePtrOps(*ty);
        conversionType_ = ty;
        return " " + ty->str();
    }

    // At `(` after a declarator name: `T x(args)` is a variable built from
    // arguments when the parentheses hold expressions rather than parameter
    // declarations.
    bool looksLikeDirectInit() const
    {
        // inside a function body we're called from parseLocalDecl, not here
        if (isAt(1, ")"))
            return false;
        size_t k = 1;
        const Token &x = peekTok(k);
        if (x.kind == TK::Int || x.kind == TK::Float || x.kind == TK::String || x.kind == TK::Char)
            return true;
        if (x.kind == TK::Punct && x.text != "::")
            return !(x.text == "." && isAt(2, "."));
        if (x.kind == TK::Ident && (x.text == "true" || x.text == "false" || x.text == "nullptr" ||
                                    x.text == "this" || x.text == "new"))
            return true;
        return !startsType(k);
    }

    void finishVarDeclarator(Decl &d, TypeNameP ty, const Specs &specs)
    {
        d.var.name = d.name.empty() ? "" : d.name.back();
        d.var.line = d.line;
        d.isStatic = specs.isStatic;
        d.isExtern = specs.isExtern;
        d.isConstexpr = specs.isConstexpr;
        while (is("["))
        {
            ++p_;
            ty->arrayDims.push_back(is("]") ? nullptr : parseExpr());
            expect("]");
        }
        d.var.type = ty;
        parseInitializer(d.var);
    }

    void parseInitializer(VarDeclarator &v)
    {
        if (accept("="))
        {
            if (is("{"))
            {
                v.init = VarDeclarator::Brace;
                v.args = parseBraceList();
            }
            else
            {
                v.init = VarDeclarator::Copy;
                v.args.push_back(parseAssignExpr());
            }
        }
        else if (is("{"))
        {
            v.init = VarDeclarator::Brace;
            v.args = parseBraceList();
        }
        else if (is("("))
        {
            ++p_;
            v.init = VarDeclarator::Direct;
            if (!is(")"))
                v.args = parseExprList(")");
            expect(")");
        }
    }

    template <class MK>
    void parseVarDeclarators(TypeNameP base, Specs &specs, std::vector<DeclP> &out, MK mk, bool inClass)
    {
        do
        {
            auto ty = std::make_shared<TypeName>(*base);
            parsePtrOps(*ty);
            auto d = mk(DK::Var);
            d->name.push_back(ident());
            finishVarDeclarator(*d, ty, specs);
            out.push_back(d);
        } while (accept(","));
        (void)inClass;
        expect(";");
    }

    template <class MK>
    DeclP parseClassOrEnum(bool inClass, MK mk)
    {
        (void)inClass;
        if (accept("enum"))
        {
            auto d = mk(DK::Enum);
            if (accept("class") || accept("struct"))
                d->scoped = true;
            if (at(TK::Ident))
                d->name.push_back(ident());
            else
                d->name.push_back("");
            if (!d->name.back().empty() && !cMode_) // a C enum tag is not a type name
                types_.insert(d->name.back());
            if (accept(":"))
            {
                Specs s;
                d->underlying = parseBaseType(s);
            }
            if (accept(";"))
            {
                p_--; // forward declaration; caller expects ';'
                d->forwardOnly = true;
                return d;
            }
            expect("{");
            while (!is("}"))
            {
                std::string n = ident();
                skipAttributes();
                ExprP v;
                if (accept("="))
                    v = parseCondExpr(false);
                d->enumerators.emplace_back(n, v);
                if (!accept(","))
                    break;
            }
            expect("}");
            return d;
        }
        auto d = mk(DK::Class);
        d->isStruct = is("struct");
        d->isUnion = is("union");
        ++p_;
        skipAttributes();
        if (at(TK::Ident) && !is("final"))
            d->name.push_back(ident());
        else
            d->name.push_back("");
        if (!d->name.back().empty() && !cMode_) // C: `struct X` names a type only with its keyword
            types_.insert(d->name.back());
        if (!d->name.back().empty() && cMode_)
            cTags_.insert(d->name.back());
        accept("final");
        if (is(";"))
        {
            d->forwardOnly = true;
            return d;
        }
        if (accept(":"))
        {
            do
            {
                while (is("public") || is("private") || is("protected") || is("virtual"))
                    ++p_;
                d->bases.push_back(parseTypeId());
            } while (accept(","));
        }
        expect("{");
        classStack_.push_back(d->name.back());
        std::string access = d->isStruct || d->isUnion ? "public" : "private";
        while (!is("}") && !at(TK::End))
        {
            if ((is("public") || is("private") || is("protected")) && isAt(1, ":"))
            {
                access = cur().text;
                p_ += 2;
                continue;
            }
            if (is("friend") && (isAt(1, "class") || isAt(1, "struct")) && peekTok(2).kind == TK::Ident)
            {
                // friend class X;
                d->friendNames.push_back(peekTok(2).text);
                while (!is(";") && !at(TK::End))
                    ++p_;
                accept(";");
                continue;
            }
            auto ms = parseDecl(true, d->name.back());
            for (auto &m : ms)
            {
                m->access = access;
                if (m->isFriend && !m->name.empty())
                    d->friendNames.push_back(m->name.back());
                if (m->k == DK::Func && m->isFriend && !m->body)
                    continue; // friend declaration only
                d->members.push_back(m);
            }
        }
        classStack_.pop_back();
        expect("}");
        return d;
    }

    // ─── Statements ────────────────────────────────────────────────────────

    StmtP mkStmt(SK k)
    {
        auto s = std::make_shared<Stmt>();
        s->k = k;
        s->line = cur().line;
        return s;
    }

    StmtP parseBlock()
    {
        auto b = mkStmt(SK::Block);
        expect("{");
        while (!is("}") && !at(TK::End))
            b->body.push_back(parseStmt());
        expect("}");
        return b;
    }

    std::vector<Catch> parseCatches()
    {
        std::vector<Catch> cs;
        while (accept("catch"))
        {
            Catch c;
            expect("(");
            if (accept("..."))
            {
            }
            else
            {
                Specs s;
                auto ty = parseBaseType(s);
                parsePtrOps(*ty);
                if (at(TK::Ident))
                    c.name = ident();
                c.type = ty;
            }
            expect(")");
            c.body = parseBlock();
            cs.push_back(c);
        }
        return cs;
    }

    StmtP parseStmt()
    {
        skipAttributes();
        if (is("{"))
            return parseBlock();
        if (accept(";"))
            return mkStmt(SK::Empty);
        if (is("if"))
        {
            auto s = mkStmt(SK::If);
            ++p_;
            accept("constexpr");
            expect("(");
            parseCondition(*s);
            expect(")");
            s->then = parseStmt();
            if (accept("else"))
                s->els = parseStmt();
            return s;
        }
        if (is("while"))
        {
            auto s = mkStmt(SK::While);
            ++p_;
            expect("(");
            parseCondition(*s);
            expect(")");
            s->then = parseStmt();
            return s;
        }
        if (is("do"))
        {
            auto s = mkStmt(SK::Do);
            ++p_;
            s->then = parseStmt();
            expect("while");
            expect("(");
            s->expr = parseExpr();
            expect(")");
            expect(";");
            return s;
        }
        if (is("for"))
        {
            ++p_;
            expect("(");
            // range-for: for (decl : expr)
            size_t save = p_;
            if (startsType() && !cMode_)
            {
                try
                {
                    auto s = mkStmt(SK::RangeFor);
                    Specs specs;
                    auto base = parseBaseType(specs);
                    auto ty = std::make_shared<TypeName>(*base);
                    parsePtrOps(*ty);
                    VarDeclarator v;
                    v.line = cur().line;
                    if (is("["))
                    {
                        ++p_;
                        do
                            v.bindings.push_back(ident());
                        while (accept(","));
                        expect("]");
                    }
                    else
                        v.name = ident();
                    v.type = ty;
                    if (accept(":"))
                    {
                        s->decls.push_back(v);
                        if (is("{"))
                        {
                            auto il = std::make_shared<Expr>();
                            il->k = EK::InitList;
                            il->line = cur().line;
                            il->kids = parseBraceList();
                            s->expr = il;
                        }
                        else
                            s->expr = parseExpr();
                        expect(")");
                        s->then = parseStmt();
                        return s;
                    }
                }
                catch (ParseFail &)
                {
                }
                p_ = save;
            }
            auto s = mkStmt(SK::For);
            if (!accept(";"))
            {
                s->init = parseSimpleStmt();
            }
            if (!is(";"))
                s->cond2 = parseExpr();
            expect(";");
            if (!is(")"))
                s->step = parseExpr();
            expect(")");
            s->then = parseStmt();
            return s;
        }
        if (is("switch"))
        {
            auto s = mkStmt(SK::Switch);
            ++p_;
            expect("(");
            parseCondition(*s);
            expect(")");
            auto body = parseStmt();
            if (body->k == SK::Block)
                s->body = body->body;
            else
                s->body.push_back(body);
            return s;
        }
        if (is("case"))
        {
            auto s = mkStmt(SK::Case);
            ++p_;
            s->expr = parseCondExpr(false);
            if (accept("..."))
                parseCondExpr(false); // GNU case range: treated as its first value
            expect(":");
            return s;
        }
        if (is("default") && isAt(1, ":"))
        {
            auto s = mkStmt(SK::Default);
            p_ += 2;
            return s;
        }
        if (is("break"))
        {
            auto s = mkStmt(SK::Break);
            ++p_;
            expect(";");
            return s;
        }
        if (is("continue"))
        {
            auto s = mkStmt(SK::Continue);
            ++p_;
            expect(";");
            return s;
        }
        if (is("return"))
        {
            auto s = mkStmt(SK::Return);
            ++p_;
            if (is("{"))
            {
                auto il = std::make_shared<Expr>();
                il->k = EK::InitList;
                il->line = cur().line;
                il->kids = parseBraceList();
                s->expr = il;
            }
            else if (!is(";"))
                s->expr = parseExpr();
            expect(";");
            return s;
        }
        if (is("try"))
        {
            auto s = mkStmt(SK::Try);
            ++p_;
            s->then = parseBlock();
            s->catches = parseCatches();
            return s;
        }
        if (is("goto"))
            fail("goto is not supported");
        if (at(TK::Ident) && isAt(1, ":") && !isAt(2, ":") && !isKeyword(cur().text) && !types_.count(cur().text))
            fail("labels are not supported");
        if ((is("struct") || is("class") || is("union") || is("enum") || is("using") || is("typedef") ||
             is("static_assert") || is("template")) &&
            !(is("using") && isAt(1, "namespace") && false))
        {
            // local type / alias / using-directive
            auto s = mkStmt(SK::LocalClass);
            auto ds = parseDecl(false, "");
            auto blk = mkStmt(SK::Block);
            for (auto &d : ds)
            {
                auto ls = mkStmt(SK::LocalClass);
                ls->local = d;
                if (d->k == DK::Var)
                {
                    // struct X {...} x; → a declaration of x as well
                    auto vs = mkStmt(SK::Decl);
                    vs->decls.push_back(d->var);
                    vs->isStatic = d->isStatic;
                    blk->body.push_back(vs);
                    continue;
                }
                blk->body.push_back(ls);
            }
            if (blk->body.size() == 1)
                return blk->body[0];
            blk->k = SK::Block;
            blk->expr = nullptr;
            // a pseudo block that does not open a scope
            auto seq = mkStmt(SK::Block);
            seq->body = blk->body;
            seq->isStatic = true; // marker: no new scope
            return seq;
        }
        return parseSimpleStmt();
    }

    // Declaration or expression statement (also a for-init), with ';'.
    StmtP parseSimpleStmt()
    {
        // C: `Point p;` for a `struct Point` (no typedef) — gcc's own advice.
        if (cMode_ && at(TK::Ident) && cTags_.count(cur().text) && !types_.count(cur().text) &&
            (peekTok().kind == TK::Ident || (peekTok().kind == TK::Punct && peekTok().text == "*")))
            fail("unknown type name '" + cur().text + "'; use 'struct' keyword to refer to the type");
        if (startsType())
        {
            size_t save = p_;
            try
            {
                return parseLocalDecl(true);
            }
            catch (ParseFail &)
            {
                p_ = save;
            }
        }
        auto s = mkStmt(SK::Expr);
        s->expr = parseExpr();
        expect(";");
        return s;
    }

    StmtP parseLocalDecl(bool needSemi)
    {
        auto s = mkStmt(SK::Decl);
        Specs specs;
        auto base = parseBaseType(specs);
        s->isStatic = specs.isStatic;
        do
        {
            auto ty = std::make_shared<TypeName>(*base);
            parsePtrOps(*ty);
            VarDeclarator v;
            v.line = cur().line;
            if (is("[") && base->isAuto())
            {
                ++p_;
                do
                    v.bindings.push_back(ident());
                while (accept(","));
                expect("]");
                v.type = ty;
                parseInitializer(v);
                s->decls.push_back(v);
                continue;
            }
            if (is("(") && isAt(1, "*"))
            {
                expect("(");
                expect("*");
                v.name = ident();
                std::vector<ExprP> dims; // R (*name[N])(Args): an array of them
                while (is("["))
                {
                    ++p_;
                    dims.push_back(is("]") ? nullptr : parseExpr());
                    expect("]");
                }
                expect(")");
                auto fn = std::make_shared<TypeName>();
                fn->isFunction = true;
                fn->ret = ty;
                bool vv = false;
                for (auto &x : parseParams(vv))
                    fn->params.push_back(x.type);
                fn->pointers = 1;
                fn->arrayDims = dims;
                v.type = fn;
                parseInitializer(v);
                s->decls.push_back(v);
                continue;
            }
            v.name = ident();
            if (isKeyword(v.name))
                fail("keyword as a name");
            while (is("["))
            {
                ++p_;
                ty->arrayDims.push_back(is("]") ? nullptr : parseExpr());
                expect("]");
            }
            v.type = ty;
            if (is("(") && !looksLikeDirectInit())
            {
                // a local function declaration (`T f();`, `int g(int)`): skip
                bool vv = false;
                parseParams(vv);
                while (is("const") || is("noexcept"))
                    ++p_;
                continue;
            }
            parseInitializer(v);
            s->decls.push_back(v);
        } while (accept(","));
        if (needSemi)
            expect(";");
        if (s->decls.empty())
            s->k = SK::Empty;
        return s;
    }

    // if/while/switch condition: an expression, or `T x = e` / init-statement.
    void parseCondition(Stmt &s)
    {
        if (startsType())
        {
            size_t save = p_;
            try
            {
                auto d = parseLocalDecl(false);
                if (accept(";"))
                {
                    s.init = d; // if (init; cond)
                    s.expr = parseExpr();
                    return;
                }
                if (is(")") && d->k == SK::Decl && d->decls.size() == 1 && d->decls[0].init != VarDeclarator::None)
                {
                    s.condDecl = d;
                    return;
                }
            }
            catch (ParseFail &)
            {
            }
            p_ = save;
        }
        s.expr = parseExpr();
        if (accept(";"))
        {
            auto init = mkStmt(SK::Expr);
            init->expr = s.expr;
            s.init = init;
            s.expr = parseExpr();
        }
    }

    // ─── Expressions ───────────────────────────────────────────────────────

    ExprP mk(EK k, int line)
    {
        auto e = std::make_shared<Expr>();
        e->k = k;
        e->line = line;
        return e;
    }

    std::vector<ExprP> parseExprList(const char *close)
    {
        std::vector<ExprP> v;
        while (!is(close))
        {
            if (is("{"))
            {
                auto il = mk(EK::InitList, cur().line);
                il->kids = parseBraceList();
                v.push_back(il);
            }
            else
                v.push_back(parseAssignExpr());
            accept("...");
            if (!accept(","))
                break;
        }
        return v;
    }

    // `[i] = e` in a brace list (not a lambda's `[captures](...)`)
    bool designatorAhead()
    {
        int depth = 0;
        for (size_t k = 0; p_ + k < t_.size(); ++k)
        {
            const auto &t = t_[p_ + k];
            if (t.kind == TK::Punct && (t.text == "[" || t.text == "(" || t.text == "{"))
                ++depth;
            else if (t.kind == TK::Punct && (t.text == "]" || t.text == ")" || t.text == "}"))
            {
                if (--depth == 0)
                    return isAt(k + 1, "=");
            }
            else if (t.kind == TK::End)
                return false;
        }
        return false;
    }

    std::vector<ExprP> parseBraceList()
    {
        expect("{");
        std::vector<ExprP> v;
        while (!is("}"))
        {
            // designated initializers: .x = 1, [2] = 3
            std::string field;
            ExprP index;
            if (is(".") && peekTok().kind == TK::Ident && isAt(2, "="))
            {
                field = peekTok().text;
                p_ += 3;
            }
            else if (is("[") && designatorAhead())
            {
                ++p_;
                index = parseAssignExpr();
                expect("]");
                expect("=");
            }
            if (is("{"))
            {
                auto il = mk(EK::InitList, cur().line);
                il->kids = parseBraceList();
                v.push_back(il);
            }
            else
                v.push_back(parseAssignExpr());
            v.back()->designator = field;
            v.back()->designIndex = index;
            if (!accept(","))
                break;
        }
        expect("}");
        return v;
    }

    ExprP parseExpr()
    {
        auto e = parseAssignExpr();
        while (is(","))
        {
            int line = cur().line;
            ++p_;
            auto c = mk(EK::Comma, line);
            c->kids = {e, parseAssignExpr()};
            e = c;
        }
        return e;
    }

    ExprP parseAssignExpr()
    {
        int line = cur().line;
        if (is("throw"))
        {
            ++p_;
            auto e = mk(EK::Throw, line);
            if (!is(";") && !is(")") && !is(",") && !is(":"))
                e->kids.push_back(parseAssignExpr());
            return e;
        }
        auto l = parseCondExpr(false);
        static const char *ops[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
        for (const char *op : ops)
        {
            if (is(op))
            {
                ++p_;
                auto e = mk(EK::Assign, line);
                e->op = op;
                ExprP r;
                if (is("{"))
                {
                    r = mk(EK::InitList, cur().line);
                    r->kids = parseBraceList();
                }
                else
                    r = parseAssignExpr();
                e->kids = {l, r};
                return e;
            }
        }
        return l;
    }

    // `inTArgs`: inside template arguments, where '>' ends the expression.
    ExprP parseCondExpr(bool inTArgs)
    {
        int line = cur().line;
        auto c = parseBin(0, inTArgs);
        if (is("?"))
        {
            ++p_;
            auto e = mk(EK::Cond, line);
            auto a = parseExpr();
            expect(":");
            auto b = parseAssignExpr();
            e->kids = {c, a, b};
            return e;
        }
        return c;
    }

    static int precOf(const std::string &op, bool inTArgs)
    {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "|") return 3;
        if (op == "^") return 4;
        if (op == "&") return 5;
        if (op == "==" || op == "!=") return 6;
        if (op == "<=>") return 7;
        if (op == "<" || op == "<=" || op == ">=") return 7;
        if (op == ">") return inTArgs ? -1 : 7;
        if (op == "<<") return 8;
        if (op == ">>") return inTArgs ? -1 : 8;
        if (op == "+" || op == "-") return 9;
        if (op == "*" || op == "/" || op == "%") return 10;
        return -1;
    }

    ExprP parseBin(int minPrec, bool inTArgs)
    {
        auto l = parseUnary();
        while (at(TK::Punct) || (at(TK::Ident) && (is("and") || is("or") || is("bitand") || is("bitor") ||
                                                   is("xor") || is("not_eq"))))
        {
            std::string op = cur().text;
            if (op == "and") op = "&&";
            else if (op == "or") op = "||";
            else if (op == "bitand") op = "&";
            else if (op == "bitor") op = "|";
            else if (op == "xor") op = "^";
            else if (op == "not_eq") op = "!=";
            int p = precOf(op, inTArgs);
            if (p < 0 || p < minPrec)
                break;
            int line = cur().line;
            ++p_;
            auto r = parseBin(p + 1, inTArgs);
            auto e = mk(EK::Binary, line);
            e->op = op;
            e->kids = {l, r};
            l = e;
        }
        return l;
    }

    ExprP parseUnary()
    {
        int line = cur().line;
        static const char *pre[] = {"++", "--", "+", "-", "!", "~", "*", "&"};
        for (const char *op : pre)
        {
            if (is(op))
            {
                ++p_;
                auto e = mk(EK::Unary, line);
                e->op = op;
                e->kids.push_back(parseUnary());
                return e;
            }
        }
        if (is("not"))
        {
            ++p_;
            auto e = mk(EK::Unary, line);
            e->op = "!";
            e->kids.push_back(parseUnary());
            return e;
        }
        if (is("sizeof") || is("alignof"))
        {
            ++p_;
            accept("...");
            if (is("(") && startsTypeAt(1))
            {
                size_t save = p_;
                try
                {
                    ++p_;
                    auto ty = parseTypeId();
                    expect(")");
                    auto e = mk(EK::SizeofT, line);
                    e->type = ty;
                    return e;
                }
                catch (ParseFail &)
                {
                    p_ = save;
                }
            }
            auto e = mk(EK::SizeofE, line);
            e->kids.push_back(parseUnary());
            return e;
        }
        if (is("new"))
        {
            ++p_;
            auto e = mk(EK::New, line);
            if (is("("))
                fail("placement new is not supported");
            Specs s;
            auto ty = parseBaseType(s);
            parsePtrOps(*ty);
            e->type = ty;
            if (accept("["))
            {
                e->newArray = true;
                e->kids.push_back(parseExpr());
                expect("]");
                if (is("{"))
                {
                    e->braced = true;
                    auto init = parseBraceList();
                    e->kids.insert(e->kids.end(), init.begin(), init.end());
                }
                else if (is("("))
                {
                    ++p_;
                    expect(")");
                }
            }
            else if (is("("))
            {
                ++p_;
                if (!is(")"))
                    e->kids = parseExprList(")");
                expect(")");
            }
            else if (is("{"))
            {
                e->braced = true;
                e->kids = parseBraceList();
            }
            return e;
        }
        if (is("delete"))
        {
            ++p_;
            if (is("[") && isAt(1, "]"))
                p_ += 2;
            auto e = mk(EK::Delete, line);
            e->kids.push_back(parseUnary());
            return e;
        }
        // C-style cast: (T)expr
        if (is("(") && startsTypeAt(1))
        {
            size_t save = p_;
            try
            {
                ++p_;
                auto ty = parseTypeId();
                expect(")");
                if (is("{"))
                {
                    // compound literal (T){...}
                    auto e = mk(EK::Construct, line);
                    e->type = ty;
                    e->braced = true;
                    e->kids = parseBraceList();
                    return parsePostfix(e);
                }
                // must be followed by an operand
                if (at(TK::Ident) || at(TK::Int) || at(TK::Float) || at(TK::Char) || at(TK::String) ||
                    is("(") || is("-") || is("+") || is("!") || is("~") || is("*") || is("&") || is("::"))
                {
                    auto e = mk(EK::Cast, line);
                    e->op = "c";
                    e->type = ty;
                    e->kids.push_back(parseUnary());
                    return e;
                }
            }
            catch (ParseFail &)
            {
            }
            p_ = save;
        }
        return parsePostfix(parsePrimary());
    }

    ExprP parsePostfix(ExprP e)
    {
        while (true)
        {
            int line = cur().line;
            if (is("(") && e->k == EK::Name && e->name.size() == 1 && e->name[0] == "va_arg")
            {
                // va_arg(ap, T): the type is carried by a sizeof node
                ++p_;
                auto c = mk(EK::Call, line);
                c->kids.push_back(e);
                c->kids.push_back(parseAssignExpr());
                expect(",");
                auto t = mk(EK::SizeofT, line);
                Specs s;
                t->type = parseBaseType(s);
                parsePtrOps(*t->type);
                c->kids.push_back(t);
                expect(")");
                e = c;
                continue;
            }
            if (is("("))
            {
                ++p_;
                auto c = mk(EK::Call, line);
                c->kids.push_back(e);
                auto args = parseExprList(")");
                c->kids.insert(c->kids.end(), args.begin(), args.end());
                expect(")");
                e = c;
                continue;
            }
            if (is("["))
            {
                ++p_;
                auto c = mk(EK::Index, line);
                ExprP idx;
                if (is("{"))
                {
                    idx = mk(EK::InitList, cur().line);
                    idx->kids = parseBraceList();
                }
                else
                    idx = parseExpr();
                c->kids = {e, idx};
                expect("]");
                e = c;
                continue;
            }
            if (is(".") || is("->"))
            {
                bool arrow = is("->");
                ++p_;
                accept("template");
                auto m = mk(EK::Member, line);
                m->arrow = arrow;
                m->kids.push_back(e);
                if (is("~"))
                {
                    ++p_;
                    m->op = "~" + ident();
                }
                else if (is("operator"))
                {
                    ++p_;
                    m->op = "operator" + parseOperatorName();
                }
                else
                {
                    m->op = ident();
                    // qualified member: obj.Base::f()
                    while (is("::"))
                    {
                        ++p_;
                        m->op = ident();
                    }
                }
                if (is("<") && templates_.count(m->op))
                {
                    size_t save = p_;
                    try
                    {
                        m->targs = parseTemplateArgs();
                        m->hasTargs = true;
                    }
                    catch (ParseFail &)
                    {
                        p_ = save;
                    }
                }
                e = m;
                continue;
            }
            if (is("++") || is("--"))
            {
                auto c = mk(EK::Postfix, line);
                c->op = cur().text;
                ++p_;
                c->kids.push_back(e);
                e = c;
                continue;
            }
            break;
        }
        return e;
    }

    static double parseNumber(const std::string &txt, bool &isUnsigned, bool &isLong, bool &isFloatSuffix,
                              bool isFloat)
    {
        std::string s = txt;
        isUnsigned = isLong = isFloatSuffix = false;
        if (isFloat)
        {
            while (!s.empty() && (s.back() == 'f' || s.back() == 'F' || s.back() == 'l' || s.back() == 'L'))
            {
                if (s.back() == 'f' || s.back() == 'F')
                    isFloatSuffix = true;
                s.pop_back();
            }
            return std::strtod(s.c_str(), nullptr);
        }
        while (!s.empty() && std::strchr("uUlLzZ", s.back()))
        {
            if (s.back() == 'u' || s.back() == 'U')
                isUnsigned = true;
            else
                isLong = true;
            s.pop_back();
        }
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
        {
            double v = 0;
            for (size_t i = 2; i < s.size(); ++i)
                v = v * 2 + (s[i] - '0');
            return v;
        }
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            return (double)std::strtoull(s.c_str(), nullptr, 16);
        if (s.size() > 1 && s[0] == '0')
            return (double)std::strtoull(s.c_str(), nullptr, 8);
        return std::strtod(s.c_str(), nullptr);
    }

    LambdaP parseLambda()
    {
        auto lam = std::make_shared<Lambda>();
        expect("[");
        while (!is("]"))
        {
            if (is("&") && (isAt(1, ",") || isAt(1, "]")))
            {
                ++p_;
                lam->captureAllByRef = true;
            }
            else if (is("=") && (isAt(1, ",") || isAt(1, "]")))
            {
                ++p_;
                lam->captureAllByValue = true;
            }
            else if (accept("&"))
                lam->captures.push_back("&" + ident());
            else if (accept("this"))
                lam->captures.push_back("this");
            else if (is("*") && isAt(1, "this"))
            {
                p_ += 2;
                lam->captures.push_back("this");
            }
            else
            {
                std::string n = ident();
                if (accept("="))
                    parseAssignExpr(); // init-capture: value taken from the enclosing scope
                lam->captures.push_back(n);
            }
            if (!accept(","))
                break;
        }
        expect("]");
        if (is("<"))
        {
            // template lambda: []<typename T>(T x)
            bool t = false;
            --p_;
            ++p_;
            parseTemplateHeaderInline(t);
        }
        if (is("("))
        {
            bool v = false;
            lam->params = parseParams(v);
        }
        while (true)
        {
            if (accept("mutable"))
                lam->isMutable = true;
            else if (accept("constexpr") || accept("consteval"))
            {
            }
            else if (accept("noexcept"))
            {
                if (is("("))
                    skipBalanced("(", ")");
            }
            else if (accept("->"))
                lam->ret = parseTypeId();
            else
                break;
        }
        lam->body = parseBlock();
        return lam;
    }

    void parseTemplateHeaderInline(bool &)
    {
        expect("<");
        while (!is(">"))
        {
            if (is("typename") || is("class"))
            {
                ++p_;
                if (at(TK::Ident))
                    types_.insert(ident());
            }
            else
                ++p_;
            accept(",");
        }
        expect(">");
    }

    ExprP parsePrimary()
    {
        int line = cur().line;
        const Token &tk = cur();
        switch (tk.kind)
        {
        case TK::Int:
        case TK::Float:
        {
            auto e = mk(tk.kind == TK::Int ? EK::Int : EK::Float, line);
            e->num = parseNumber(tk.text, e->isUnsigned, e->isLong, e->isFloatSuffix, tk.kind == TK::Float);
            if (tk.kind == TK::Int)
                e->str = tk.text; // the exact digits (64-bit literals)
            ++p_;
            return e;
        }
        case TK::Char:
        {
            auto e = mk(EK::Char, line);
            e->str = tk.value;
            // multi-character constants: value of the bytes, big-endian
            double v = 0;
            for (unsigned char ch : tk.value)
                v = v * 256 + ch;
            if (tk.value.size() == 1)
                v = (signed char)tk.value[0] < 0 ? (unsigned char)tk.value[0] : (unsigned char)tk.value[0];
            e->num = v;
            ++p_;
            return e;
        }
        case TK::String:
        {
            auto e = mk(EK::Str, line);
            while (at(TK::String))
                e->str += t_[p_++].value; // adjacent literals concatenate
            if (at(TK::Ident) && cur().text == "s" && !cur().spaceBefore)
                ++p_; // "text"s
            return e;
        }
        case TK::Punct:
            if (tk.text == "(")
            {
                ++p_;
                auto e = parseExpr();
                expect(")");
                return e;
            }
            if (tk.text == "[")
            {
                if (cMode_)
                    fail("expected expression near '['"); // C has no lambdas
                auto e = mk(EK::Lambda, line);
                e->lambda = parseLambda();
                return e;
            }
            if (tk.text == "{")
            {
                auto e = mk(EK::InitList, line);
                e->kids = parseBraceList();
                return e;
            }
            if (tk.text == "::")
            {
                ++p_;
                return parsePrimary();
            }
            fail("unexpected '" + tk.text + "'");
        case TK::Ident:
            break;
        case TK::End:
            fail("unexpected end of input");
        }
        const std::string &w = tk.text;
        if (w == "true" || w == "false")
        {
            auto e = mk(EK::Bool, line);
            e->num = w == "true";
            ++p_;
            return e;
        }
        if (w == "nullptr")
        {
            ++p_;
            return mk(EK::Nullptr, line);
        }
        if (w == "this")
        {
            ++p_;
            return mk(EK::This, line);
        }
        if (w == "static_cast" || w == "dynamic_cast" || w == "const_cast" || w == "reinterpret_cast")
        {
            ++p_;
            auto e = mk(EK::Cast, line);
            e->op = w;
            expect("<");
            e->type = parseTypeId();
            expectCloseAngle();
            expect("(");
            e->kids.push_back(parseExpr());
            expect(")");
            return e;
        }
        if (w == "operator")
        {
            ++p_;
            auto e = mk(EK::Name, line);
            e->name.push_back("operator" + parseOperatorName());
            return e;
        }
        // Functional cast / construction with a built-in type: int(x), unsigned(x)
        if (builtinWords().count(w) && w != "auto")
        {
            Specs s;
            auto ty = parseBaseType(s);
            auto e = mk(EK::Construct, line);
            e->type = ty;
            if (is("{"))
            {
                e->braced = true;
                e->kids = parseBraceList();
            }
            else
            {
                expect("(");
                if (!is(")"))
                    e->kids = parseExprList(")");
                expect(")");
            }
            return e;
        }
        // (qualified) name, maybe with template args
        auto e = mk(EK::Name, line);
        std::vector<std::vector<TArg>> allTargs;
        while (true)
        {
            std::string n = ident();
            e->name.push_back(n);
            allTargs.emplace_back();
            if (is("<") && templates_.count(n))
            {
                size_t save = p_;
                try
                {
                    allTargs.back() = parseTemplateArgs();
                    e->hasTargs = true;
                }
                catch (ParseFail &)
                {
                    p_ = save;
                }
            }
            if (is("::") && (peekTok().kind == TK::Ident || isAt(1, "~") || isAt(1, "operator")))
            {
                ++p_;
                accept("template");
                if (is("~"))
                {
                    ++p_;
                    e->name.push_back("~" + ident());
                    allTargs.emplace_back();
                    break;
                }
                if (is("operator"))
                {
                    ++p_;
                    e->name.push_back("operator" + parseOperatorName());
                    allTargs.emplace_back();
                    break;
                }
                continue;
            }
            break;
        }
        e->targs = allTargs.back();
        // A type used as a value: T(args) / T{args}
        bool isType = types_.count(e->name.back()) > 0;
        if (isType && (is("(") || is("{")))
        {
            auto ty = std::make_shared<TypeName>();
            ty->name = e->name;
            ty->targs = allTargs;
            ty->hasTargs = e->hasTargs;
            auto c = mk(EK::Construct, line);
            c->type = ty;
            if (is("{"))
            {
                c->braced = true;
                c->kids = parseBraceList();
            }
            else
            {
                ++p_;
                if (!is(")"))
                    c->kids = parseExprList(")");
                expect(")");
            }
            return c;
        }
        // keep earlier components' template args (Cls<int>::f) on the type path
        if (e->hasTargs && allTargs.size() > 1)
        {
            auto ty = std::make_shared<TypeName>();
            ty->name = e->name;
            ty->targs = allTargs;
            ty->hasTargs = true;
            e->type = ty;
        }
        return e;
    }
};

} // namespace

Program parse(const Preprocessed &pp)
{
    Parser p(pp);
    try
    {
        Program prog = p.run();
        prog.systemHeaders = pp.systemHeaders;
        return prog;
    }
    catch (ParseFail &last)
    {
        const ParseFail &f = p.furthestFailure(last);
        Unsupported u("parse: " + f.msg, f.file < (int)pp.files.size() ? pp.files[f.file] : "", f.line);
        u.col = f.col;
        u.syntax = true;
        throw u;
    }
}

} // namespace cpp
