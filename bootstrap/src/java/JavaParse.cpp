// Java syntax (JLS §19) -> CompilationUnit. Recursive descent with javac's
// error conventions: "X expected" is reported just past the previous token,
// other errors at the offending token; the first error stops the parse.
#include "JavaAst.h"

#include <set>

namespace java
{

std::string TypeRef::str() const
{
    switch (kind)
    {
    case Prim:
        return prim;
    case Var:
        return "var";
    case Array:
        return (elem ? elem->str() : std::string("?")) + "[]";
    case Wildcard:
        return wildKind == 0 ? "?" : std::string(wildKind == 1 ? "? extends " : "? super ") + (bound ? bound->str() : "");
    case Named:
    {
        std::string s;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i)
                s += ".";
            s += parts[i].name;
            if (parts[i].hasArgs)
            {
                s += "<";
                for (size_t j = 0; j < parts[i].args.size(); ++j)
                    s += (j ? "," : "") + parts[i].args[j]->str();
                s += ">";
            }
        }
        return s;
    }
    }
    return "?";
}

namespace
{

bool isPrimName(const std::string &s)
{
    static const std::set<std::string> p = {"boolean", "byte", "short", "char", "int", "long", "float", "double"};
    return p.count(s) > 0;
}

class Parser
{
public:
    Parser(std::vector<Token> toks, int unit, const std::string &fileName)
        : t_(std::move(toks)), unit_(unit), fileName_(fileName) {}

    CompilationUnit run()
    {
        CompilationUnit cu;
        skipAnnotationsBeforePackage();
        if (isKw("package"))
        {
            next();
            cu.package = qualifiedName();
            expect(";");
        }
        while (isKw("import"))
        {
            ImportDecl im;
            im.pos = cur().pos;
            next();
            if (isKw("static"))
            {
                next();
                im.isStatic = true;
            }
            im.name.push_back(ident());
            while (isOp("."))
            {
                next();
                if (isOp("*"))
                {
                    next();
                    im.onDemand = true;
                    break;
                }
                im.name.push_back(ident());
            }
            expect(";");
            cu.imports.push_back(im);
        }
        std::shared_ptr<ClassDecl> implicitClass;
        while (!at(Tok::End))
        {
            if (isOp(";"))
            {
                next();
                continue;
            }
            Modifiers mods = modifiers();
            if (startsTypeDecl())
            {
                cu.types.push_back(typeDecl(mods));
                continue;
            }
            if (isKw("import") || isKw("package"))
                fail("class, interface, enum, or record expected");
            // A compact source file (Java 25): methods and fields at the top
            // level belong to an implicit class named after the file.
            if (looksLikeMember())
            {
                if (!implicitClass)
                {
                    implicitClass = std::make_shared<ClassDecl>();
                    implicitClass->kind = ClassDecl::Class;
                    implicitClass->isImplicit = true;
                    implicitClass->unit = unit_;
                    implicitClass->pos = cur().pos;
                    std::string base = fileName_;
                    size_t slash = base.find_last_of("/\\");
                    if (slash != std::string::npos)
                        base = base.substr(slash + 1);
                    size_t dot = base.rfind('.');
                    if (dot != std::string::npos)
                        base = base.substr(0, dot);
                    implicitClass->name = base.empty() ? "Main" : base;
                    implicitClass->mods.flags |= M_FINAL;
                    cu.types.push_back(implicitClass);
                }
                memberDecl(*implicitClass, mods);
                continue;
            }
            fail("class, interface, enum, or record expected");
        }
        return cu;
    }

private:
    std::vector<Token> t_;
    size_t p_ = 0;
    int unit_;
    std::string fileName_;
    std::vector<std::string> classNames_; // enclosing class names (constructor detection)

    // ── tokens ──

    const Token &cur() const { return t_[p_]; }
    const Token &peekTok(size_t k = 1) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
    const Token &prev() const { return t_[p_ ? p_ - 1 : 0]; }
    bool at(Tok k) const { return cur().kind == k; }
    bool isOp(const char *s) const { return cur().kind == Tok::Op && cur().text == s; }
    bool isKw(const char *s) const { return cur().kind == Tok::Keyword && cur().text == s; }
    bool isIdent(const char *s) const { return cur().kind == Tok::Ident && cur().text == s; }
    bool opAt(size_t k, const char *s) const { return peekTok(k).kind == Tok::Op && peekTok(k).text == s; }
    bool kwAt(size_t k, const char *s) const { return peekTok(k).kind == Tok::Keyword && peekTok(k).text == s; }
    void next()
    {
        if (p_ + 1 < t_.size())
            ++p_;
    }

    [[noreturn]] void fail(const std::string &msg) const
    {
        if (at(Tok::End))
            throw SyntaxError{"reached end of file while parsing", prev().pos};
        throw SyntaxError{msg, cur().pos};
    }
    [[noreturn]] void failAt(const std::string &msg, Pos p) const { throw SyntaxError{msg, p}; }
    // javac reports a missing token just past the previous one.
    [[noreturn]] void expected(const std::string &what) const
    {
        if (at(Tok::End) && what != "';'")
            throw SyntaxError{"reached end of file while parsing", prev().pos};
        throw SyntaxError{what + " expected", p_ ? prev().end : cur().pos};
    }
    void expect(const char *op)
    {
        if (isOp(op))
        {
            next();
            return;
        }
        expected(std::string("'") + op + "'");
    }
    std::string ident()
    {
        if (at(Tok::Ident))
        {
            std::string s = cur().text;
            next();
            return s;
        }
        if (at(Tok::Keyword) && cur().text == "_")
            fail("as of release 9, '_' is a keyword, and may not be used as an identifier");
        expected("<identifier>");
    }
    std::vector<std::string> qualifiedName()
    {
        std::vector<std::string> n{ident()};
        while (isOp(".") && peekTok().kind == Tok::Ident)
        {
            next();
            n.push_back(ident());
        }
        return n;
    }

    // ── modifiers / annotations ──

    void skipAnnotationsBeforePackage()
    {
        size_t save = p_;
        while (isOp("@") && !kwAt(1, "interface"))
            annotation();
        if (!isKw("package"))
            p_ = save;
    }

    std::string annotation()
    {
        expect("@");
        std::vector<std::string> n = qualifiedName();
        if (isOp("("))
            skipBalanced("(", ")");
        return n.back();
    }

    void skipBalanced(const char *open, const char *close)
    {
        int depth = 0;
        do
        {
            if (at(Tok::End))
                expected(std::string("'") + close + "'");
            if (isOp(open))
                ++depth;
            else if (isOp(close))
                --depth;
            next();
        } while (depth > 0);
    }

    Modifiers modifiers()
    {
        Modifiers m;
        m.pos = cur().pos;
        while (true)
        {
            if (isOp("@") && !kwAt(1, "interface"))
            {
                m.annotations.push_back(annotation());
                continue;
            }
            unsigned f = 0;
            if (at(Tok::Keyword))
            {
                const std::string &w = cur().text;
                if (w == "public") f = M_PUBLIC;
                else if (w == "private") f = M_PRIVATE;
                else if (w == "protected") f = M_PROTECTED;
                else if (w == "static") f = M_STATIC;
                else if (w == "final") f = M_FINAL;
                else if (w == "abstract") f = M_ABSTRACT;
                else if (w == "native") f = M_NATIVE;
                else if (w == "synchronized" && !opAt(1, "(")) f = M_SYNCHRONIZED;
                else if (w == "transient") f = M_TRANSIENT;
                else if (w == "volatile") f = M_VOLATILE;
                else if (w == "strictfp") f = M_STRICTFP;
                else if (w == "default" && !opAt(1, ":") && !opAt(1, "->")) f = M_DEFAULT;
            }
            else if (isIdent("sealed") && (peekTok().kind == Tok::Keyword || peekTok().kind == Tok::Ident))
                f = M_SEALED;
            else if (isIdent("non") && opAt(1, "-") && peekTok(2).text == "sealed")
            {
                next();
                next();
                f = M_NON_SEALED;
            }
            if (!f)
                break;
            if (m.flags & f)
                fail("repeated modifier");
            m.flags |= f;
            next();
        }
        return m;
    }

    bool startsTypeDecl() const
    {
        if (isKw("class") || isKw("interface") || isKw("enum"))
            return true;
        if (isOp("@") && kwAt(1, "interface"))
            return true;
        if (isIdent("record") && peekTok().kind == Tok::Ident && (opAt(2, "(") || opAt(2, "<")))
            return true;
        return false;
    }

    // At the top level of a file: a method or field declaration?
    bool looksLikeMember()
    {
        size_t save = p_;
        bool ok = false;
        try
        {
            if (isOp("<"))
                typeParams();
            if (isKw("void"))
                next();
            else
                type();
            ok = at(Tok::Ident) && (opAt(1, "(") || opAt(1, "=") || opAt(1, ";") || opAt(1, ","));
        }
        catch (SyntaxError &)
        {
        }
        p_ = save;
        return ok;
    }

    // ── types ──

    std::vector<TypeParam> typeParams()
    {
        std::vector<TypeParam> ps;
        expect("<");
        do
        {
            while (isOp("@"))
                annotation();
            TypeParam tp;
            tp.pos = cur().pos;
            tp.name = ident();
            if (isKw("extends"))
            {
                next();
                tp.bounds.push_back(type());
                while (isOp("&"))
                {
                    next();
                    tp.bounds.push_back(type());
                }
            }
            ps.push_back(tp);
            if (!isOp(","))
                break;
            next();
        } while (true);
        expect(">");
        return ps;
    }

    TypeRefP typeArg()
    {
        while (isOp("@"))
            annotation();
        if (isOp("?"))
        {
            auto t = std::make_shared<TypeRef>();
            t->kind = TypeRef::Wildcard;
            t->pos = cur().pos;
            next();
            if (isKw("extends"))
            {
                next();
                t->wildKind = 1;
                t->bound = type();
            }
            else if (isKw("super"))
            {
                next();
                t->wildKind = 2;
                t->bound = type();
            }
            return t;
        }
        return type();
    }

    // Parses `<...>` after a type name; `diamond` allows `<>`.
    void typeArgs(TypeRef::Part &part, bool diamond)
    {
        expect("<");
        part.hasArgs = true;
        if (isOp(">"))
        {
            if (!diamond)
                fail("illegal start of type");
            next();
            return;
        }
        do
        {
            part.args.push_back(typeArg());
            if (!isOp(","))
                break;
            next();
        } while (true);
        expect(">");
    }

    TypeRefP arrayDims(TypeRefP t)
    {
        while (isOp("[") && opAt(1, "]"))
        {
            auto a = std::make_shared<TypeRef>();
            a->kind = TypeRef::Array;
            a->elem = t;
            a->pos = t->pos;
            next();
            next();
            t = a;
        }
        return t;
    }

    // A type: primitive or class type, with array dimensions.
    TypeRefP type(bool diamond = false)
    {
        while (isOp("@"))
            annotation();
        auto t = std::make_shared<TypeRef>();
        t->pos = cur().pos;
        if (at(Tok::Keyword) && isPrimName(cur().text))
        {
            t->kind = TypeRef::Prim;
            t->prim = cur().text;
            next();
            return arrayDims(t);
        }
        if (isIdent("var") && peekTok().kind == Tok::Ident)
        {
            t->kind = TypeRef::Var;
            next();
            return t;
        }
        if (!at(Tok::Ident))
        {
            if (isKw("void"))
                fail("'void' type not allowed here");
            fail(at(Tok::Keyword) || at(Tok::Op) ? "illegal start of type" : "<identifier> expected");
        }
        t->kind = TypeRef::Named;
        while (true)
        {
            TypeRef::Part part;
            part.pos = cur().pos;
            part.name = ident();
            if (isOp("<"))
                typeArgs(part, diamond);
            t->parts.push_back(part);
            if (isOp(".") && (peekTok().kind == Tok::Ident || opAt(1, "@")))
            {
                next();
                while (isOp("@"))
                    annotation();
                continue;
            }
            break;
        }
        return arrayDims(t);
    }

    // ── declarations ──

    ClassDeclP typeDecl(const Modifiers &mods)
    {
        auto d = std::make_shared<ClassDecl>();
        d->mods = mods;
        d->pos = mods.flags || !mods.annotations.empty() ? mods.pos : cur().pos;
        d->unit = unit_;
        if (isOp("@"))
        {
            next(); // @interface: an annotation type — parsed and ignored
            next();
            d->kind = ClassDecl::Annotation;
            d->namePos = cur().pos;
            d->name = ident();
            skipBalanced("{", "}");
            return d;
        }
        if (isKw("class"))
            d->kind = ClassDecl::Class;
        else if (isKw("interface"))
            d->kind = ClassDecl::Interface;
        else if (isKw("enum"))
            d->kind = ClassDecl::Enum;
        else
            d->kind = ClassDecl::Record;
        next();
        d->namePos = cur().pos;
        d->name = ident();
        if (isOp("<"))
            d->tparams = typeParams();
        if (d->kind == ClassDecl::Record)
        {
            expect("(");
            if (!isOp(")"))
                d->recordComponents = formalParams(false);
            expect(")");
        }
        if (isKw("extends"))
        {
            next();
            if (d->kind == ClassDecl::Interface)
            {
                d->implements.push_back(type());
                while (isOp(","))
                {
                    next();
                    d->implements.push_back(type());
                }
            }
            else if (d->kind == ClassDecl::Class)
                d->extends = type();
            else
                fail("'{' expected");
        }
        if (isKw("implements"))
        {
            if (d->kind == ClassDecl::Interface)
                fail("'{' expected");
            next();
            d->implements.push_back(type());
            while (isOp(","))
            {
                next();
                d->implements.push_back(type());
            }
        }
        if (isIdent("permits"))
        {
            next();
            d->permits.push_back(type());
            while (isOp(","))
            {
                next();
                d->permits.push_back(type());
            }
        }
        classBody(*d);
        return d;
    }

    void classBody(ClassDecl &d)
    {
        expect("{");
        classNames_.push_back(d.name);
        if (d.kind == ClassDecl::Enum)
            enumConstants(d);
        while (!isOp("}"))
        {
            if (at(Tok::End))
                fail("reached end of file while parsing");
            if (isOp(";"))
            {
                next();
                continue;
            }
            Modifiers mods = modifiers();
            memberDecl(d, mods);
        }
        d.endPos = cur().pos;
        next();
        classNames_.pop_back();
    }

    void enumConstants(ClassDecl &d)
    {
        while (at(Tok::Ident) || isOp("@"))
        {
            while (isOp("@"))
                annotation();
            EnumConst c;
            c.pos = cur().pos;
            c.name = ident();
            if (isOp("("))
            {
                next();
                if (!isOp(")"))
                    c.args = exprList(")");
                expect(")");
            }
            if (isOp("{"))
            {
                auto body = std::make_shared<ClassDecl>();
                body->kind = ClassDecl::Class;
                body->isAnonymous = true;
                body->unit = unit_;
                body->pos = cur().pos;
                classBody(*body);
                c.body = body;
            }
            d.enumConsts.push_back(c);
            if (isOp(","))
            {
                next();
                continue;
            }
            break;
        }
        if (isOp(";"))
            next();
        else if (!isOp("}"))
            expected("',', '}', or ';'");
    }

    void memberDecl(ClassDecl &d, const Modifiers &mods)
    {
        Member m;
        if (startsTypeDecl())
        {
            m.kind = Member::Class;
            m.cls = typeDecl(mods);
            d.members.push_back(m);
            return;
        }
        if (isOp("{"))
        {
            m.kind = Member::Init;
            m.isStatic = mods.has(M_STATIC);
            m.init = block();
            d.members.push_back(m);
            return;
        }
        auto md = std::make_shared<MethodDecl>();
        md->mods = mods;
        md->pos = mods.flags || !mods.annotations.empty() ? mods.pos : cur().pos;
        if (isOp("<"))
            md->tparams = typeParams();
        // constructor: Name ( ... )   compact record constructor: Name {
        if (at(Tok::Ident) && !classNames_.empty() && cur().text == classNames_.back() &&
            (opAt(1, "(") || (d.kind == ClassDecl::Record && opAt(1, "{"))))
        {
            md->isCtor = true;
            md->namePos = cur().pos;
            md->name = ident();
            if (isOp("{"))
                md->compactCtor = true;
            else
                methodRest(*md, d);
            if (md->compactCtor)
                md->body = block();
            m.kind = Member::Method;
            m.method = md;
            d.members.push_back(m);
            return;
        }
        if (at(Tok::Ident) && opAt(1, "(") && !d.isImplicit)
        {
            // a method without a return type (javac's message)
            failAt("invalid method declaration; return type required", cur().pos);
        }
        TypeRefP t;
        if (isKw("void"))
        {
            t = std::make_shared<TypeRef>();
            t->kind = TypeRef::Prim;
            t->prim = "void";
            t->pos = cur().pos;
            next();
        }
        else
            t = type();
        Pos namePos = cur().pos;
        std::string name = ident();
        if (isOp("("))
        {
            md->ret = t;
            md->name = name;
            md->namePos = namePos;
            methodRest(*md, d);
            m.kind = Member::Method;
            m.method = md;
            d.members.push_back(m);
            return;
        }
        if (t->kind == TypeRef::Prim && t->prim == "void")
            failAt("'(' expected", cur().pos);
        auto f = std::make_shared<FieldDecl>();
        f->mods = mods;
        f->type = t;
        f->pos = md->pos;
        VarDeclarator v;
        v.name = name;
        v.pos = namePos;
        declaratorRest(v);
        f->vars.push_back(v);
        while (isOp(","))
        {
            next();
            VarDeclarator w;
            w.pos = cur().pos;
            w.name = ident();
            declaratorRest(w);
            f->vars.push_back(w);
        }
        expect(";");
        m.kind = Member::Field;
        m.field = f;
        d.members.push_back(m);
    }

    void declaratorRest(VarDeclarator &v)
    {
        while (isOp("[") && opAt(1, "]"))
        {
            next();
            next();
            ++v.dims;
        }
        if (isOp("="))
        {
            next();
            v.init = isOp("{") ? arrayLiteral() : expr();
        }
    }

    void methodRest(MethodDecl &md, ClassDecl &d)
    {
        expect("(");
        if (!isOp(")"))
            md.params = formalParams(true);
        expect(")");
        while (isOp("[") && opAt(1, "]"))
        {
            next();
            next();
            auto a = std::make_shared<TypeRef>();
            a->kind = TypeRef::Array;
            a->elem = md.ret;
            md.ret = a;
        }
        if (isKw("throws"))
        {
            next();
            md.throws.push_back(type());
            while (isOp(","))
            {
                next();
                md.throws.push_back(type());
            }
        }
        if (d.kind == ClassDecl::Annotation && isKw("default"))
        {
            next();
            expr();
        }
        if (isOp("{"))
            md.body = block();
        else
            expect(";");
    }

    std::vector<Param> formalParams(bool allowVarargs)
    {
        std::vector<Param> ps;
        while (true)
        {
            Param p;
            p.mods = modifiers();
            p.pos = cur().pos;
            p.type = type();
            if (isOp("..."))
            {
                if (!allowVarargs)
                    fail("',', ')', or '[' expected");
                next();
                p.varargs = true;
                auto a = std::make_shared<TypeRef>();
                a->kind = TypeRef::Array;
                a->elem = p.type;
                a->pos = p.type->pos;
                p.type = a;
            }
            if (isKw("this")) // receiver parameter: Foo this
            {
                next();
                if (!isOp(","))
                    break;
                next();
                continue;
            }
            p.pos = cur().pos;
            p.name = ident();
            while (isOp("[") && opAt(1, "]"))
            {
                next();
                next();
                auto a = std::make_shared<TypeRef>();
                a->kind = TypeRef::Array;
                a->elem = p.type;
                a->pos = p.type->pos;
                p.type = a;
            }
            ps.push_back(p);
            if (!isOp(","))
                break;
            if (p.varargs)
                fail("varargs parameter must be the last parameter");
            next();
        }
        return ps;
    }

    // ── statements ──

    StmtP mkStmt(SK k)
    {
        auto s = std::make_shared<Stmt>();
        s->k = k;
        s->pos = cur().pos;
        return s;
    }

    StmtP block()
    {
        auto s = mkStmt(SK::Block);
        expect("{");
        while (!isOp("}"))
        {
            if (at(Tok::End))
                fail("reached end of file while parsing");
            s->stmts.push_back(blockStmt());
        }
        s->endPos = cur().pos;
        next();
        return s;
    }

    // A local variable declaration starting here? (type followed by a name)
    bool startsLocalVarDecl()
    {
        if (isKw("final") || (isOp("@") && !kwAt(1, "interface")))
            return true;
        if (at(Tok::Keyword) && isPrimName(cur().text))
            return true;
        if (!at(Tok::Ident))
            return false;
        if (isIdent("var") && peekTok().kind == Tok::Ident)
            return true;
        size_t save = p_;
        bool ok = false;
        try
        {
            type();
            ok = at(Tok::Ident) || (at(Tok::Keyword) && cur().text == "_");
        }
        catch (SyntaxError &)
        {
        }
        p_ = save;
        return ok;
    }

    StmtP localVarDecl(bool needSemi)
    {
        auto s = mkStmt(SK::LocalVar);
        s->mods = modifiers();
        s->type = type();
        do
        {
            VarDeclarator v;
            v.pos = cur().pos;
            v.name = ident();
            declaratorRest(v);
            s->vars.push_back(v);
            if (!isOp(","))
                break;
            next();
        } while (true);
        if (needSemi)
            expect(";");
        return s;
    }

    StmtP blockStmt()
    {
        // local class / record / interface / enum
        {
            size_t save = p_;
            Modifiers mods = modifiers();
            if (startsTypeDecl())
            {
                auto s = mkStmt(SK::LocalClass);
                s->cls = typeDecl(mods);
                s->cls->isLocal = true;
                return s;
            }
            p_ = save;
        }
        if (at(Tok::Ident) && opAt(1, ":") && !opAt(2, ":"))
        {
            auto s = mkStmt(SK::Labeled);
            s->label = ident();
            next();
            s->s1 = statement();
            return s;
        }
        if (startsLocalVarDecl() && !isYieldStmt())
            return localVarDecl(true);
        return statement();
    }

    bool isYieldStmt() const
    {
        if (!isIdent("yield"))
            return false;
        const Token &n = peekTok();
        if (n.kind == Tok::Op)
        {
            // `yield = 1;` assigns a variable named yield; `yield (x);` yields
            static const std::set<std::string> notYield = {"=", ".", "[", "++", "--", "->", "+=", "-=",
                                                           "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
                                                           ">>>=", ")", ";", ",", "::"};
            if (notYield.count(n.text))
                return false;
        }
        return true;
    }

    StmtP statement()
    {
        if (isOp("{"))
            return block();
        if (isOp(";"))
        {
            auto s = mkStmt(SK::Empty);
            next();
            return s;
        }
        if (isYieldStmt())
        {
            auto s = mkStmt(SK::Yield);
            next();
            s->e = expr();
            expect(";");
            return s;
        }
        if (at(Tok::Keyword))
        {
            const std::string w = cur().text;
            if (w == "if")
            {
                auto s = mkStmt(SK::If);
                next();
                parenCond(s->e);
                s->s1 = statement();
                if (isKw("else"))
                {
                    next();
                    s->s2 = statement();
                }
                return s;
            }
            if (w == "while")
            {
                auto s = mkStmt(SK::While);
                next();
                parenCond(s->e);
                s->s1 = statement();
                return s;
            }
            if (w == "do")
            {
                auto s = mkStmt(SK::Do);
                next();
                s->s1 = statement();
                if (!isKw("while"))
                    expected("'while'");
                next();
                parenCond(s->e);
                expect(";");
                return s;
            }
            if (w == "for")
                return forStmt();
            if (w == "return")
            {
                auto s = mkStmt(SK::Return);
                next();
                if (!isOp(";"))
                    s->e = expr();
                expect(";");
                return s;
            }
            if (w == "break" || w == "continue")
            {
                auto s = mkStmt(w == "break" ? SK::Break : SK::Continue);
                next();
                if (at(Tok::Ident))
                    s->label = ident();
                expect(";");
                return s;
            }
            if (w == "throw")
            {
                auto s = mkStmt(SK::Throw);
                next();
                s->e = expr();
                expect(";");
                return s;
            }
            if (w == "try")
                return tryStmt();
            if (w == "switch")
            {
                auto s = mkStmt(SK::Switch);
                s->sw = switchBody();
                return s;
            }
            if (w == "synchronized")
            {
                auto s = mkStmt(SK::Sync);
                next();
                parenCond(s->e);
                s->s1 = block();
                return s;
            }
            if (w == "assert")
            {
                auto s = mkStmt(SK::Assert);
                next();
                s->e = expr();
                if (isOp(":"))
                {
                    next();
                    s->e2 = expr();
                }
                expect(";");
                return s;
            }
            if (w == "else")
                fail("'else' without 'if'");
            if (w == "catch")
                fail("'catch' without 'try'");
            if (w == "finally")
                fail("'finally' without 'try'");
            if (w == "case" || w == "default")
                fail(std::string("orphaned ") + w);
            if ((w == "this" || w == "super") && opAt(1, "("))
            {
                auto s = mkStmt(SK::CtorCall);
                s->superCall = w == "super";
                next();
                next();
                if (!isOp(")"))
                    s->args = exprList(")");
                expect(")");
                expect(";");
                return s;
            }
            if (w == "class" || w == "interface" || w == "enum")
            {
                auto s = mkStmt(SK::LocalClass);
                s->cls = typeDecl(Modifiers{});
                s->cls->isLocal = true;
                return s;
            }
        }
        auto s = mkStmt(SK::Expr);
        s->e = expr();
        // outer.super(...): a qualified superclass constructor call
        if (s->e->k == EK::Call && s->e->superCall && s->e->name == "<init>")
        {
            s->k = SK::CtorCall;
            s->superCall = true;
            s->qualifier = s->e->a;
            s->args = s->e->args;
            expect(";");
            return s;
        }
        checkStatementExpr(*s->e);
        expect(";");
        return s;
    }

    // Only some expressions may stand alone as statements (JLS §14.8).
    void checkStatementExpr(const Expr &e)
    {
        switch (e.k)
        {
        case EK::Assign:
        case EK::PreIncDec:
        case EK::PostIncDec:
        case EK::Call:
        case EK::New:
            if (!e.parenthesized)
                return;
            break;
        default:
            break;
        }
        failAt("not a statement", e.pos);
    }

    void parenCond(ExprP &out)
    {
        expect("(");
        out = expr();
        expect(")");
    }

    StmtP forStmt()
    {
        Pos start = cur().pos;
        next();
        expect("(");
        // enhanced for: (Type name : expr)
        {
            size_t save = p_;
            bool isEach = false;
            try
            {
                if (startsLocalVarDecl())
                {
                    modifiers();
                    type();
                    ident();
                    isEach = isOp(":");
                }
            }
            catch (SyntaxError &)
            {
            }
            p_ = save;
            if (isEach)
            {
                auto s = mkStmt(SK::ForEach);
                s->pos = start;
                s->mods = modifiers();
                s->type = type();
                VarDeclarator v;
                v.pos = cur().pos;
                v.name = ident();
                s->vars.push_back(v);
                expect(":");
                s->e = expr();
                expect(")");
                s->s1 = statement();
                return s;
            }
        }
        auto s = mkStmt(SK::For);
        s->pos = start;
        if (!isOp(";"))
        {
            if (startsLocalVarDecl())
                s->init.push_back(localVarDecl(false));
            else
            {
                do
                {
                    auto st = mkStmt(SK::Expr);
                    st->e = expr();
                    checkStatementExpr(*st->e);
                    s->init.push_back(st);
                    if (!isOp(","))
                        break;
                    next();
                } while (true);
            }
        }
        expect(";");
        if (!isOp(";"))
            s->e = expr();
        expect(";");
        if (!isOp(")"))
        {
            do
            {
                ExprP u = expr();
                checkStatementExpr(*u);
                s->update.push_back(u);
                if (!isOp(","))
                    break;
                next();
            } while (true);
        }
        expect(")");
        s->s1 = statement();
        return s;
    }

    StmtP tryStmt()
    {
        auto s = mkStmt(SK::Try);
        next();
        if (isOp("("))
        {
            next();
            while (!isOp(")"))
            {
                Resource r;
                r.pos = cur().pos;
                if (startsLocalVarDecl())
                {
                    modifiers();
                    r.type = type();
                    r.name = ident();
                    expect("=");
                    r.init = expr();
                }
                else
                    r.init = expr(); // an effectively final variable / field
                s->resources.push_back(r);
                if (isOp(";"))
                {
                    next();
                    continue;
                }
                break;
            }
            expect(")");
        }
        s->tryBlock = block();
        while (isKw("catch"))
        {
            CatchClause c;
            c.pos = cur().pos;
            next();
            expect("(");
            modifiers();
            c.types.push_back(type());
            while (isOp("|"))
            {
                next();
                c.types.push_back(type());
            }
            c.name = ident();
            expect(")");
            c.body = block();
            s->catches.push_back(c);
        }
        if (isKw("finally"))
        {
            next();
            s->finallyBlock = block();
        }
        if (s->catches.empty() && !s->finallyBlock && s->resources.empty())
            failAt("'try' without 'catch', 'finally' or resource declarations", s->pos);
        return s;
    }

    std::shared_ptr<SwitchBody> switchBody()
    {
        auto sw = std::make_shared<SwitchBody>();
        sw->pos = cur().pos;
        next(); // switch
        expect("(");
        sw->selector = expr();
        expect(")");
        expect("{");
        bool sawArrow = false, sawColon = false;
        while (!isOp("}"))
        {
            if (at(Tok::End))
                fail("reached end of file while parsing");
            SwitchCase c;
            c.pos = cur().pos;
            if (isKw("default"))
            {
                next();
                c.isDefault = true;
            }
            else if (isKw("case"))
            {
                next();
                caseLabels(c);
            }
            else
                fail("case, default, or '}' expected");
            if (isOp("->"))
            {
                if (sawColon)
                    fail("different case kinds used in the switch");
                sawArrow = true;
                next();
                if (isOp("{"))
                    c.body.push_back(block());
                else if (isKw("throw"))
                    c.body.push_back(statement());
                else
                {
                    c.arrowExpr = expr();
                    expect(";");
                }
            }
            else
            {
                if (!isOp(":"))
                    expected(sawArrow ? "'->'" : "':' or '->'");
                if (sawArrow)
                    fail("different case kinds used in the switch");
                sawColon = true;
                next();
                while (!isKw("case") && !isKw("default") && !isOp("}"))
                {
                    if (at(Tok::End))
                        fail("reached end of file while parsing");
                    c.body.push_back(blockStmt());
                }
            }
            sw->cases.push_back(c);
        }
        next();
        sw->arrow = sawArrow;
        return sw;
    }

    void caseLabels(SwitchCase &c)
    {
        while (true)
        {
            if (isKw("null"))
            {
                next();
                c.hasNullLabel = true;
                if (isOp(",") && kwAt(1, "default"))
                {
                    next();
                    next();
                    c.isDefault = true;
                }
            }
            else if (isKw("default"))
            {
                next();
                c.isDefault = true;
            }
            else if (startsTypePattern())
            {
                modifiers();
                c.patternType = type();
                c.patternVar = at(Tok::Keyword) && cur().text == "_" ? (next(), std::string("_")) : ident();
                if (isIdent("when"))
                {
                    next();
                    c.guard = expr();
                }
                return;
            }
            else
                c.labels.push_back(ternary());
            if (!isOp(","))
                break;
            next();
        }
    }

    // `case String s ->` / `case Circle c when ...`
    bool startsTypePattern()
    {
        size_t save = p_;
        bool ok = false;
        try
        {
            modifiers();
            type();
            ok = at(Tok::Ident) && cur().text != "when";
            if (!ok && at(Tok::Keyword) && cur().text == "_")
                ok = true;
        }
        catch (SyntaxError &)
        {
        }
        p_ = save;
        return ok;
    }

    // ── expressions ──

    ExprP mk(EK k, Pos pos)
    {
        auto e = std::make_shared<Expr>();
        e->k = k;
        e->pos = pos;
        return e;
    }

    std::vector<ExprP> exprList(const char *close)
    {
        std::vector<ExprP> v;
        while (true)
        {
            if (isOp(close))
                fail("illegal start of expression");
            v.push_back(expr());
            if (!isOp(","))
                break;
            next();
        }
        return v;
    }

    ExprP arrayLiteral()
    {
        auto e = mk(EK::ArrayLit, cur().pos);
        expect("{");
        while (!isOp("}"))
        {
            e->args.push_back(isOp("{") ? arrayLiteral() : expr());
            if (!isOp(","))
                break;
            next();
        }
        expect("}");
        return e;
    }

    // Lambda ahead? `x ->`, `(...) ->`
    bool lambdaAhead() const
    {
        if ((at(Tok::Ident) || (at(Tok::Keyword) && cur().text == "_")) && opAt(1, "->"))
            return true;
        if (!isOp("("))
            return false;
        int depth = 0;
        for (size_t k = p_; k < t_.size(); ++k)
        {
            const Token &x = t_[k];
            if (x.kind == Tok::End)
                return false;
            if (x.kind == Tok::Op && x.text == "(")
                ++depth;
            else if (x.kind == Tok::Op && x.text == ")")
            {
                if (--depth == 0)
                    return k + 1 < t_.size() && t_[k + 1].kind == Tok::Op && t_[k + 1].text == "->";
            }
            else if (x.kind == Tok::Op && (x.text == ";" || x.text == "{" || x.text == "}"))
                return false;
        }
        return false;
    }

    ExprP lambda()
    {
        auto e = mk(EK::Lambda, cur().pos);
        if (at(Tok::Ident) || at(Tok::Keyword))
        {
            LambdaParam p;
            p.pos = cur().pos;
            p.name = cur().text;
            next();
            e->params.push_back(p);
        }
        else
        {
            expect("(");
            while (!isOp(")"))
            {
                LambdaParam p;
                modifiers();
                p.pos = cur().pos;
                if ((at(Tok::Ident) || (at(Tok::Keyword) && cur().text == "_")) && (opAt(1, ",") || opAt(1, ")")))
                {
                    p.name = cur().text;
                    next();
                }
                else
                {
                    p.type = type();
                    if (isOp("..."))
                    {
                        next();
                        auto a = std::make_shared<TypeRef>();
                        a->kind = TypeRef::Array;
                        a->elem = p.type;
                        p.type = a;
                    }
                    p.pos = cur().pos;
                    p.name = at(Tok::Keyword) && cur().text == "_" ? (next(), std::string("_")) : ident();
                }
                e->params.push_back(p);
                if (!isOp(","))
                    break;
                next();
            }
            expect(")");
        }
        e->opPos = cur().pos;
        expect("->");
        if (isOp("{"))
            e->block = block();
        else
            e->a = expr();
        return e;
    }

    ExprP expr()
    {
        if (lambdaAhead())
            return lambda();
        ExprP lhs = ternary();
        static const std::set<std::string> assignOps = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<="};
        std::string op;
        int extra = 0;
        if (at(Tok::Op) && assignOps.count(cur().text))
            op = cur().text;
        else if (isOp(">") && cur().adjNext && opAt(1, ">") && peekTok().adjNext && opAt(2, ">") &&
                 peekTok(2).adjNext && opAt(3, "="))
        {
            op = ">>>=";
            extra = 3;
        }
        else if (isOp(">") && cur().adjNext && opAt(1, ">") && peekTok().adjNext && opAt(2, "="))
        {
            op = ">>=";
            extra = 2;
        }
        if (op.empty())
            return lhs;
        Pos opPos = cur().pos;
        for (int k = 0; k < extra; ++k)
            next();
        next();
        if (lhs->k != EK::Name && lhs->k != EK::Field && lhs->k != EK::Index)
            failAt("unexpected type", lhs->pos); // javac: required variable, found value
        auto e = mk(EK::Assign, lhs->pos);
        e->name = op;
        e->opPos = opPos;
        e->a = lhs;
        e->b = isOp("{") ? (fail("illegal start of expression"), nullptr) : expr();
        return e;
    }

    ExprP ternary()
    {
        ExprP c = binary(0);
        if (!isOp("?"))
            return c;
        auto e = mk(EK::Cond, c->pos);
        e->opPos = cur().pos;
        next();
        e->a = c;
        e->b = lambdaAhead() ? lambda() : ternary();
        expect(":");
        e->c = lambdaAhead() ? lambda() : ternary();
        return e;
    }

    // Binary operator at the current position: (operator, precedence, tokens).
    bool binOp(std::string &op, int &prec, int &ntok) const
    {
        ntok = 1;
        if (at(Tok::Keyword) && cur().text == "instanceof")
        {
            op = "instanceof";
            prec = 7;
            return true;
        }
        if (!at(Tok::Op))
            return false;
        const std::string &t = cur().text;
        if (t == ">")
        {
            // > >> >>> >= (the lexer never joins '>')
            if (cur().adjNext && opAt(1, ">"))
            {
                if (peekTok().adjNext && opAt(2, ">"))
                {
                    if (peekTok(2).adjNext && opAt(3, "="))
                        return false; // >>>=
                    op = ">>>";
                    ntok = 3;
                    prec = 8;
                    return true;
                }
                if (peekTok().adjNext && opAt(2, "="))
                    return false; // >>=
                op = ">>";
                ntok = 2;
                prec = 8;
                return true;
            }
            if (cur().adjNext && opAt(1, "="))
            {
                op = ">=";
                ntok = 2;
                prec = 7;
                return true;
            }
            op = ">";
            prec = 7;
            return true;
        }
        static const std::pair<const char *, int> table[] = {
            {"||", 1}, {"&&", 2}, {"|", 3}, {"^", 4}, {"&", 5}, {"==", 6}, {"!=", 6}, {"<", 7}, {"<=", 7},
            {"<<", 8}, {"+", 9}, {"-", 9}, {"*", 10}, {"/", 10}, {"%", 10}};
        for (auto &[o, p] : table)
            if (t == o)
            {
                op = o;
                prec = p;
                return true;
            }
        return false;
    }

    ExprP binary(int minPrec)
    {
        ExprP lhs = unary();
        while (true)
        {
            std::string op;
            int prec, ntok;
            if (!binOp(op, prec, ntok) || prec <= minPrec)
                break;
            Pos opPos = cur().pos;
            for (int k = 0; k < ntok; ++k)
                next();
            if (op == "instanceof")
            {
                auto e = mk(EK::InstanceOf, lhs->pos);
                e->opPos = opPos;
                e->a = lhs;
                bool fin = false;
                if (isKw("final"))
                {
                    next();
                    fin = true;
                }
                e->type = type();
                if (at(Tok::Ident) && cur().text != "when")
                    e->bindName = ident();
                else if (fin)
                    expected("<identifier>");
                lhs = e;
                continue;
            }
            ExprP rhs = binary(prec);
            auto e = mk(EK::Binary, lhs->pos);
            e->name = op;
            e->opPos = opPos;
            e->a = lhs;
            e->b = rhs;
            lhs = e;
        }
        return lhs;
    }

    // Can the token after a parenthesized reference type start the operand of a cast?
    bool castOperandStart(const Token &x) const
    {
        switch (x.kind)
        {
        case Tok::Ident:
        case Tok::IntLit:
        case Tok::LongLit:
        case Tok::FloatLit:
        case Tok::DoubleLit:
        case Tok::CharLit:
        case Tok::StringLit:
            return true;
        case Tok::Keyword:
            return x.text == "this" || x.text == "super" || x.text == "new" || x.text == "true" ||
                   x.text == "false" || x.text == "null" || x.text == "switch" || isPrimName(x.text);
        case Tok::Op:
            return x.text == "(" || x.text == "!" || x.text == "~";
        default:
            return false;
        }
    }

    ExprP unary()
    {
        Pos pos = cur().pos;
        if (isOp("+") || isOp("-"))
        {
            std::string op = cur().text;
            next();
            // -2147483648 / -9223372036854775808L
            if (op == "-" && (at(Tok::IntLit) || at(Tok::LongLit)))
            {
                ExprP lit = primary();
                if (lit->k == EK::Literal && (lit->lit == Expr::LInt || lit->lit == Expr::LLong) &&
                    !lit->parenthesized)
                {
                    lit->ival = (int64_t)(0 - (uint64_t)lit->ival);
                    lit->pos = pos;
                    lit->name = "-";
                    return postfix(lit);
                }
                auto e = mk(EK::Unary, pos);
                e->name = op;
                e->a = postfix(lit);
                return e;
            }
            auto e = mk(EK::Unary, pos);
            e->name = op;
            e->a = unary();
            return e;
        }
        if (isOp("++") || isOp("--"))
        {
            auto e = mk(EK::PreIncDec, pos);
            e->name = cur().text;
            next();
            e->a = unary();
            return e;
        }
        if (isOp("!") || isOp("~"))
        {
            auto e = mk(EK::Unary, pos);
            e->name = cur().text;
            next();
            e->a = unary();
            return e;
        }
        if (isOp("(") && !lambdaAhead())
        {
            // cast?
            size_t save = p_;
            TypeRefP t;
            bool isCast = false;
            try
            {
                next();
                t = type();
                while (isOp("&"))
                {
                    next();
                    t->also.push_back(type());
                }
                if (isOp(")"))
                {
                    const Token &after = peekTok();
                    bool prim = t->kind == TypeRef::Prim;
                    isCast = prim || castOperandStart(after);
                }
            }
            catch (SyntaxError &)
            {
            }
            if (isCast)
            {
                next(); // )
                auto e = mk(EK::Cast, pos);
                e->type = t;
                e->a = lambdaAhead() ? lambda() : unary();
                return e;
            }
            p_ = save;
        }
        return postfix(primary());
    }

    ExprP postfix(ExprP e)
    {
        while (true)
        {
            if (isOp("."))
            {
                Pos dot = cur().pos;
                next();
                if (isOp("<"))
                {
                    // explicit type arguments: a.<T>m(...)
                    TypeRef::Part part;
                    typeArgs(part, false);
                    auto c = mk(EK::Call, e->pos);
                    c->a = e;
                    c->targs = part.args;
                    c->opPos = cur().pos;
                    c->name = ident();
                    callArgs(*c);
                    e = c;
                    continue;
                }
                if (isKw("new"))
                {
                    ExprP n = creator();
                    n->c = e; // outer instance
                    e = n;
                    continue;
                }
                if (isKw("this"))
                {
                    auto t = mk(EK::This, e->pos);
                    t->type = nameToType(e);
                    next();
                    e = t;
                    continue;
                }
                if (isKw("class"))
                {
                    auto c = mk(EK::ClassLit, e->pos);
                    c->type = nameToType(e);
                    next();
                    e = c;
                    continue;
                }
                if (isKw("super"))
                {
                    // Interface.super.m() / Outer.super.m()
                    Pos sp = cur().pos;
                    next();
                    if (isOp("::"))
                    {
                        next();
                        auto r = mk(EK::MethodRef, e->pos);
                        r->superCall = true;
                        r->type = nameToType(e);
                        r->name = ident();
                        e = r;
                        continue;
                    }
                    if (isOp("("))
                    {
                        auto c = mk(EK::Call, e->pos);
                        c->a = e;
                        c->superCall = true;
                        c->name = "<init>";
                        callArgs(*c);
                        e = c;
                        continue;
                    }
                    expect(".");
                    auto c = mk(EK::Call, e->pos);
                    c->superCall = true;
                    c->type = nameToType(e);
                    c->opPos = cur().pos;
                    c->name = ident();
                    (void)sp;
                    if (isOp("("))
                        callArgs(*c);
                    else
                    {
                        // Outer.super.field
                        c->k = EK::Field;
                    }
                    e = c;
                    continue;
                }
                if (!at(Tok::Ident))
                {
                    if (at(Tok::Keyword) && cur().text == "_")
                        ident();
                    failAt("<identifier> expected", dot.line == cur().pos.line ? cur().pos : prev().end);
                }
                Pos namePos = cur().pos;
                std::string name = ident();
                if (isOp("("))
                {
                    auto c = mk(EK::Call, e->pos);
                    c->a = e;
                    c->name = name;
                    c->opPos = namePos;
                    callArgs(*c);
                    e = c;
                }
                else
                {
                    auto f = mk(EK::Field, e->pos);
                    f->a = e;
                    f->name = name;
                    f->opPos = namePos;
                    e = f;
                }
                continue;
            }
            if (isOp("["))
            {
                // Type[]::new / Type[].class handled in primary; here: indexing
                if (opAt(1, "]"))
                {
                    // array type in expression position: int[].class, String[]::new
                    TypeRefP t = nameToType(e);
                    t = arrayDims(t);
                    if (isOp("::"))
                    {
                        next();
                        auto r = mk(EK::MethodRef, e->pos);
                        r->type = t;
                        r->name = isKw("new") ? (next(), std::string("new")) : ident();
                        e = r;
                        continue;
                    }
                    if (isOp(".") && kwAt(1, "class"))
                    {
                        next();
                        next();
                        auto c = mk(EK::ClassLit, e->pos);
                        c->type = t;
                        e = c;
                        continue;
                    }
                    fail("illegal start of expression");
                }
                auto ix = mk(EK::Index, e->pos);
                ix->opPos = cur().pos;
                next();
                ix->a = e;
                ix->b = expr();
                expect("]");
                e = ix;
                continue;
            }
            if (isOp("++") || isOp("--"))
            {
                auto p = mk(EK::PostIncDec, e->pos);
                p->name = cur().text;
                p->opPos = cur().pos;
                next();
                p->a = e;
                e = p;
                continue;
            }
            if (isOp("::"))
            {
                next();
                auto r = mk(EK::MethodRef, e->pos);
                r->a = e;
                if (isOp("<"))
                {
                    TypeRef::Part part;
                    typeArgs(part, false);
                    r->targs = part.args;
                }
                r->name = isKw("new") ? (next(), std::string("new")) : ident();
                e = r;
                continue;
            }
            // generic type in a method reference: List<String>::size
            if (isOp("<") && (e->k == EK::Name || e->k == EK::Field))
            {
                size_t save = p_;
                try
                {
                    TypeRef::Part part;
                    typeArgs(part, false);
                    if (isOp("::"))
                    {
                        TypeRefP t = nameToType(e);
                        t->parts.back().args = part.args;
                        t->parts.back().hasArgs = true;
                        next();
                        auto r = mk(EK::MethodRef, e->pos);
                        r->type = t;
                        r->name = isKw("new") ? (next(), std::string("new")) : ident();
                        e = r;
                        continue;
                    }
                }
                catch (SyntaxError &)
                {
                }
                p_ = save;
            }
            return e;
        }
    }

    // A Name / Field chain (a.b.C) as a type.
    TypeRefP nameToType(const ExprP &e)
    {
        auto t = std::make_shared<TypeRef>();
        t->kind = TypeRef::Named;
        t->pos = e->pos;
        std::vector<std::string> names;
        const Expr *x = e.get();
        while (x && x->k == EK::Field)
        {
            names.push_back(x->name);
            x = x->a.get();
        }
        if (!x || x->k != EK::Name)
        {
            if (x && x->k == EK::Literal && x->name == "prim")
            {
                t->kind = TypeRef::Prim;
                t->prim = x->sval;
                return t;
            }
            failAt("<identifier> expected", e->pos);
        }
        names.push_back(x->name);
        for (size_t i = names.size(); i-- > 0;)
        {
            TypeRef::Part part;
            part.name = names[i];
            part.pos = e->pos;
            t->parts.push_back(part);
        }
        return t;
    }

    void callArgs(Expr &c)
    {
        expect("(");
        if (!isOp(")"))
            c.args = exprList(")");
        if (!isOp(")"))
        {
            if (at(Tok::End))
                fail("reached end of file while parsing");
            expected("')'");
        }
        next();
    }

    ExprP creator()
    {
        Pos pos = cur().pos;
        next(); // new
        std::vector<TypeRefP> ctorTargs;
        if (isOp("<"))
        {
            TypeRef::Part part;
            typeArgs(part, false);
            ctorTargs = part.args;
        }
        while (isOp("@"))
            annotation();
        auto base = std::make_shared<TypeRef>();
        base->pos = cur().pos;
        if (at(Tok::Keyword) && isPrimName(cur().text))
        {
            base->kind = TypeRef::Prim;
            base->prim = cur().text;
            next();
            if (!isOp("["))
                expected("'['");
        }
        else
        {
            base->kind = TypeRef::Named;
            while (true)
            {
                TypeRef::Part part;
                part.pos = cur().pos;
                part.name = ident();
                if (isOp("<"))
                    typeArgs(part, true);
                base->parts.push_back(part);
                if (isOp("."))
                {
                    next();
                    continue;
                }
                break;
            }
        }
        if (isOp("["))
        {
            auto e = mk(EK::NewArray, pos);
            e->type = base; // element base type; dims below
            int dims = 0;
            while (isOp("["))
            {
                next();
                if (isOp("]"))
                {
                    next();
                    ++dims;
                    while (isOp("[") && opAt(1, "]"))
                    {
                        next();
                        next();
                        ++dims;
                    }
                    break;
                }
                if (dims != (int)e->args.size())
                    fail("']' expected");
                e->args.push_back(expr());
                expect("]");
                ++dims;
            }
            e->extraDims = dims - (int)e->args.size();
            if (isOp("{"))
            {
                if (!e->args.empty())
                    fail("';' expected");
                e->a = arrayLiteral();
            }
            else if (e->args.empty())
                fail("array dimension missing");
            return e;
        }
        auto e = mk(EK::New, pos);
        e->type = base;
        e->targs = ctorTargs;
        if (!isOp("("))
            expected("'(' or '['");
        callArgs(*e);
        if (isOp("{"))
        {
            auto body = std::make_shared<ClassDecl>();
            body->kind = ClassDecl::Class;
            body->isAnonymous = true;
            body->unit = unit_;
            body->pos = cur().pos;
            classNames_.push_back("");
            classBody(*body);
            classNames_.pop_back();
            e->body = body;
        }
        return e;
    }

    ExprP primary()
    {
        Pos pos = cur().pos;
        const Token &t = cur();
        switch (t.kind)
        {
        case Tok::IntLit:
        case Tok::LongLit:
        {
            auto e = mk(EK::Literal, pos);
            e->lit = t.kind == Tok::IntLit ? Expr::LInt : Expr::LLong;
            if (t.tooLarge)
            {
                // the magnitude 2147483648 is legal only as `-2147483648`
                bool minEdge = (t.kind == Tok::IntLit && t.ival == 2147483648ull) ||
                               (t.kind == Tok::LongLit && t.ival == 9223372036854775808ull);
                if (!(minEdge && p_ > 0 && prev().kind == Tok::Op && prev().text == "-"))
                    fail("integer number too large");
            }
            if (t.kind == Tok::IntLit)
            {
                // hex/octal/binary int literals are 32-bit two's complement
                uint64_t m = t.ival;
                e->ival = (t.text.size() > 1 && t.text[0] == '0' && m > 0x7FFFFFFFull && m <= 0xFFFFFFFFull)
                              ? (int64_t)(int32_t)(uint32_t)m
                              : (int64_t)m;
            }
            else
                e->ival = (int64_t)t.ival;
            e->sval = t.text;
            next();
            return e;
        }
        case Tok::FloatLit:
        case Tok::DoubleLit:
        {
            auto e = mk(EK::Literal, pos);
            e->lit = t.kind == Tok::FloatLit ? Expr::LFloat : Expr::LDouble;
            e->dval = t.dval;
            e->sval = t.text;
            next();
            return e;
        }
        case Tok::CharLit:
        {
            auto e = mk(EK::Literal, pos);
            e->lit = Expr::LChar;
            e->ival = t.ch;
            next();
            return e;
        }
        case Tok::StringLit:
        {
            auto e = mk(EK::Literal, pos);
            e->lit = Expr::LString;
            e->sval = t.str;
            next();
            return e;
        }
        case Tok::Ident:
        {
            // (`x -> ...` was taken as a lambda by expr(); here, as in
            // `case FOO ->`, an identifier before -> is just a name.)
            auto e = mk(EK::Name, pos);
            e->name = t.text;
            next();
            if (isOp("("))
            {
                auto c = mk(EK::Call, pos);
                c->name = e->name;
                c->opPos = pos;
                callArgs(*c);
                return c;
            }
            return e;
        }
        case Tok::Keyword:
        {
            const std::string w = t.text;
            if (w == "true" || w == "false")
            {
                auto e = mk(EK::Literal, pos);
                e->lit = Expr::LBool;
                e->bval = w == "true";
                next();
                return e;
            }
            if (w == "null")
            {
                auto e = mk(EK::Literal, pos);
                e->lit = Expr::LNull;
                next();
                return e;
            }
            if (w == "this")
            {
                next();
                if (isOp("("))
                    fail("call to this must be first statement in constructor");
                return mk(EK::This, pos);
            }
            if (w == "super")
            {
                next();
                if (isOp("::"))
                {
                    next();
                    auto r = mk(EK::MethodRef, pos);
                    r->superCall = true;
                    r->name = ident();
                    return r;
                }
                if (isOp("("))
                    fail("call to super must be first statement in constructor");
                expect(".");
                Pos namePos = cur().pos;
                std::string name = ident();
                if (isOp("("))
                {
                    auto c = mk(EK::Call, pos);
                    c->superCall = true;
                    c->name = name;
                    c->opPos = namePos;
                    callArgs(*c);
                    return c;
                }
                auto f = mk(EK::Field, pos);
                f->superCall = true;
                f->name = name;
                f->opPos = namePos;
                return f;
            }
            if (w == "new")
                return creator();
            if (w == "switch")
            {
                auto e = mk(EK::Switch, pos);
                e->sw = switchBody();
                return e;
            }
            if (isPrimName(w) || w == "void")
            {
                // int.class, int[].class, int[]::new
                auto pt = std::make_shared<TypeRef>();
                pt->kind = TypeRef::Prim;
                pt->prim = w;
                pt->pos = pos;
                next();
                pt = arrayDims(pt);
                if (isOp("::"))
                {
                    next();
                    auto r = mk(EK::MethodRef, pos);
                    r->type = pt;
                    r->name = isKw("new") ? (next(), std::string("new")) : ident();
                    return r;
                }
                if (isOp(".") && kwAt(1, "class"))
                {
                    next();
                    next();
                    auto c = mk(EK::ClassLit, pos);
                    c->type = pt;
                    return c;
                }
                fail(w == "void" ? "illegal start of expression" : "'.class' expected");
            }
            if (w == "_")
                fail("as of release 9, '_' is a keyword, and may not be used as an identifier");
            fail("illegal start of expression");
        }
        case Tok::Op:
        {
            if (t.text == "(")
            {
                if (lambdaAhead())
                    return lambda();
                next();
                ExprP e = expr();
                expect(")");
                e->parenthesized = true;
                return e;
            }
            if (t.text == "{")
                fail("illegal start of expression");
            fail("illegal start of expression");
        }
        default:
            fail("illegal start of expression");
        }
    }
};

} // namespace

CompilationUnit parse(const std::string &fileName, const std::string &source, int unitIndex, bool isPrelude)
{
    Parser p(lex(source), unitIndex, fileName);
    CompilationUnit cu = p.run();
    cu.fileName = fileName;
    cu.source = source;
    cu.isPrelude = isPrelude;
    for (auto &t : cu.types)
        t->unit = unitIndex;
    return cu;
}

} // namespace java
