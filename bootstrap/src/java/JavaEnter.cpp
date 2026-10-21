// Classes and their members: entering declarations, resolving supertypes and
// signatures, synthesized members (default constructors, records, enums),
// dispatch keys and the class-level checks javac makes.
#include "JavaSema.h"

#include <algorithm>

namespace java
{

Sema::Sema(std::vector<CompilationUnit> &units) : units_(units) {}

void Sema::error(Pos p, const std::string &msg, std::vector<std::string> notes)
{
    // prelude code is trusted: an error there is a front-end bug, still reported
    for (const Diag &d : diags_)
        if (d.unit == unit_ && d.pos.line == p.line && d.pos.col == p.col && d.msg == msg)
            return;
    diags_.push_back(Diag{unit_, p, msg, std::move(notes)});
}

// ─── Entering classes ────────────────────────────────────────────────────────

void Sema::enterUnit(int u)
{
    CompilationUnit &cu = units_[u];
    std::string pkg;
    for (auto &p : cu.package)
        pkg += (pkg.empty() ? "" : ".") + p;
    for (auto &t : cu.types)
    {
        if (t->kind == ClassDecl::Annotation)
            continue;
        std::string q = pkg.empty() ? t->name : pkg + "." + t->name;
        if (byQual_.count(q))
        {
            int saved = unit_;
            unit_ = u;
            error(t->namePos, "duplicate class: " + t->name);
            unit_ = saved;
            continue;
        }
        enterClass(t.get(), nullptr, u, pkg);
    }
}

ClassSym *Sema::enterClass(ClassDecl *d, ClassSym *outer, int unit, const std::string &pkg)
{
    classes_.push_back(std::make_unique<ClassSym>());
    ClassSym *c = classes_.back().get();
    c->decl = d;
    c->unit = unit;
    c->kind = d->kind;
    c->mods = d->mods.flags;
    c->name = d->name;
    c->pkg = pkg;
    c->outer = outer;
    c->isLocal = d->isLocal;
    c->isAnon = d->isAnonymous;
    if (outer)
    {
        if (!c->isLocal && !c->isAnon)
            c->binaryName = outer->binaryName + "$" + d->name;
    }
    else
        c->binaryName = pkg.empty() ? d->name : pkg + "." + d->name;
    if (c->isInterface())
        c->mods |= M_ABSTRACT;
    if (c->kind == ClassDecl::Record)
        c->mods |= M_FINAL;
    // nested interfaces, enums and records are implicitly static; members of
    // an interface are implicitly static too
    bool implicitlyStatic = c->isInterface() || c->kind == ClassDecl::Enum || c->kind == ClassDecl::Record ||
                            (outer && outer->isInterface());
    if (implicitlyStatic && outer)
        c->mods |= M_STATIC;
    c->hasOuterInstance = outer && !(c->mods & M_STATIC) && !c->isLocal && !c->isAnon;
    if (!c->isLocal && !c->isAnon)
    {
        std::string q;
        if (outer)
        {
            // qualified names of nested classes use '.' (java.util.Map.Entry)
            for (auto &kv : byQual_)
                if (kv.second == outer)
                {
                    q = kv.first + "." + d->name;
                    break;
                }
        }
        else
            q = c->binaryName;
        if (!q.empty())
            byQual_[q] = c;
        if (outer)
        {
            if (outer->memberTypes.count(d->name))
            {
                int saved = unit_;
                unit_ = unit;
                error(d->namePos, "class " + d->name + " is already defined in class " + outer->name);
                unit_ = saved;
            }
            outer->memberTypes[d->name] = c;
        }
    }
    order_.push_back(c);
    if (!c->binaryName.empty())
        byBinary_[c->binaryName] = c;
    for (auto &m : d->members)
        if (m.kind == Member::Class && m.cls && m.cls->kind != ClassDecl::Annotation)
            enterClass(m.cls.get(), c, unit, pkg);
    return c;
}

// ─── Headers: type parameters and supertypes ─────────────────────────────────

void Sema::resolveHeader(ClassSym *c)
{
    if (!c || c->state >= 1)
        return;
    c->state = 1;
    int savedUnit = unit_;
    ClassSym *savedCtx = ctxClass_;
    unit_ = c->unit;
    ctxClass_ = c->isAnon ? c->outer : c;
    ClassDecl *d = c->decl;
    if (d)
    {
        for (auto &tp : d->tparams)
        {
            tvars_.push_back(std::make_unique<TypeVar>());
            TypeVar *tv = tvars_.back().get();
            tv->name = tp.name;
            tv->boundRefs = tp.bounds;
            c->tparams.push_back(tv);
        }
        ctxClass_ = c;
        for (auto *tv : c->tparams)
            for (auto &b : tv->boundRefs)
                tv->bounds.push_back(resolveType(b));
        ctxClass_ = c->isAnon || c->isLocal ? c->outer : c;
        if (c->isLocal)
            ctxClass_ = c;
    }
    const std::string &bn = c->binaryName;
    if (c->isAnon)
    {
        // super / interface come from the creation expression (JavaCall.cpp)
    }
    else if (c->kind == ClassDecl::Enum)
    {
        ClassSym *e = cls("java.lang.Enum");
        c->super = classType(e, {classType(c)});
        for (auto &i : d->implements)
            c->ifaces.push_back(resolveType(i));
    }
    else if (c->kind == ClassDecl::Record)
    {
        c->super = classType(cls("java.lang.Record"));
        for (auto &i : d->implements)
            c->ifaces.push_back(resolveType(i));
    }
    else if (c->isInterface())
    {
        for (auto &i : d->implements)
        {
            TypeP t = resolveType(i);
            if (t->k == TK::Class && t->cls && !t->cls->isInterface())
                error(i->pos, "interface expected here");
            c->ifaces.push_back(t);
        }
    }
    else if (bn != "java.lang.Object")
    {
        if (d && d->extends)
        {
            TypeP t = resolveType(d->extends);
            if (t->k == TK::Class && t->cls)
            {
                resolveHeader(t->cls);
                if (t->cls->isInterface())
                    error(d->extends->pos, "no interface expected here");
                else if (t->cls->mods & M_FINAL)
                    error(d->extends->pos, "cannot inherit from final " + typeStr(classType(t->cls)));
                else if (t->cls->kind == ClassDecl::Enum)
                    error(d->extends->pos, "cannot inherit from final " + typeStr(classType(t->cls)));
                else if (isSubClass(t->cls, c))
                    error(d->namePos, "cyclic inheritance involving " + c->name);
            }
            c->super = t;
        }
        else
            c->super = tObject();
        if (d)
            for (auto &i : d->implements)
            {
                TypeP t = resolveType(i);
                if (t->k == TK::Class && t->cls && !t->cls->isInterface())
                    error(i->pos, "interface expected here");
                c->ifaces.push_back(t);
            }
    }
    c->state = 2;
    unit_ = savedUnit;
    ctxClass_ = savedCtx;
}

// ─── Members ─────────────────────────────────────────────────────────────────

namespace
{
std::string nativeName(const ClassSym *c, const std::string &m) { return "__jn_" + c->name + "_" + m; }
} // namespace

void Sema::enterMembers(ClassSym *c)
{
    if (!c || c->state >= 3)
        return;
    resolveHeader(c);
    if (c->state >= 3)
        return;
    c->state = 3;
    if (c->super && c->super->cls)
        enterMembers(c->super->cls);
    for (auto &i : c->ifaces)
        if (i->cls)
            enterMembers(i->cls);
    int savedUnit = unit_;
    ClassSym *savedCtx = ctxClass_;
    unit_ = c->unit;
    ctxClass_ = c;
    ClassDecl *d = c->decl;
    const bool iface = c->isInterface();

    auto addField = [&](const std::string &name, TypeP type, unsigned mods, FieldDecl *fd, const VarDeclarator *v,
                        Pos pos) -> FieldSym *
    {
        for (FieldSym *f : c->fields)
            if (f->name == name)
            {
                error(pos, "variable " + name + " is already defined in " +
                               std::string(c->isInterface() ? "interface " : "class ") + c->name);
                return f;
            }
        fields_.push_back(std::make_unique<FieldSym>());
        FieldSym *f = fields_.back().get();
        f->name = name;
        f->owner = c;
        f->type = type;
        f->mods = mods;
        f->decl = fd;
        f->var = v;
        f->pos = pos;
        f->isStatic = (mods & M_STATIC) != 0;
        if (f->isStatic)
            f->key = c->binaryName + "::" + name;
        else
        {
            // a field hiding one of a superclass gets its own key
            f->key = name;
            for (ClassSym *s = c->super ? c->super->cls : nullptr; s; s = s->super ? s->super->cls : nullptr)
                for (FieldSym *g : s->fields)
                    if (!g->isStatic && g->key == name)
                        f->key = name + "$" + c->binaryName;
        }
        c->fields.push_back(f);
        return f;
    };

    auto newMethod = [&](const std::string &name) -> MethodSym *
    {
        methods_.push_back(std::make_unique<MethodSym>());
        MethodSym *m = methods_.back().get();
        m->name = name;
        m->owner = c;
        return m;
    };

    if (d)
    {
        // records: the components are private final fields
        if (c->kind == ClassDecl::Record)
            for (auto &p : d->recordComponents)
                addField(p.name, resolveType(p.type), M_PRIVATE | M_FINAL, nullptr, nullptr, p.pos)->synthetic = true;
        // enums: the constants are public static final fields
        if (c->kind == ClassDecl::Enum)
            for (auto &ec : d->enumConsts)
                addField(ec.name, classType(c), M_PUBLIC | M_STATIC | M_FINAL, nullptr, nullptr, ec.pos);
        for (auto &mem : d->members)
        {
            if (mem.kind == Member::Field)
            {
                FieldDecl *fd = mem.field.get();
                unsigned mods = fd->mods.flags;
                if (iface)
                    mods |= M_PUBLIC | M_STATIC | M_FINAL;
                TypeP base = resolveType(fd->type);
                if (base->k == TK::Void)
                    error(fd->type->pos, "'void' type not allowed here");
                for (auto &v : fd->vars)
                {
                    TypeP t = base;
                    for (int k = 0; k < v.dims; ++k)
                        t = arrayOf(t);
                    if (c->kind == ClassDecl::Record && !(mods & M_STATIC))
                        error(v.pos, "field declaration must be static",
                              {"  (consider replacing field with record component)"});
                    addField(v.name, t, mods, fd, &v, v.pos);
                }
            }
            else if (mem.kind == Member::Init)
            {
                if (mem.isStatic)
                    c->needsClinit = true;
                else
                    c->hasInstanceInit = true;
            }
            else if (mem.kind == Member::Method)
            {
                MethodDecl *md = mem.method.get();
                MethodSym *m = newMethod(md->isCtor ? "<init>" : md->name);
                m->decl = md;
                m->isCtor = md->isCtor;
                m->pos = md->namePos;
                m->mods = md->mods.flags;
                if (iface && !m->isCtor)
                {
                    if (!(m->mods & (M_DEFAULT | M_STATIC | M_PRIVATE)) && !md->body)
                        m->mods |= M_ABSTRACT;
                    if (!(m->mods & M_PRIVATE))
                        m->mods |= M_PUBLIC;
                    if (md->body && !(m->mods & (M_DEFAULT | M_STATIC | M_PRIVATE)))
                        error(md->namePos, "interface abstract methods cannot have body");
                }
                if (c->kind == ClassDecl::Enum && m->isCtor)
                    m->mods |= M_PRIVATE;
                for (auto &tp : md->tparams)
                {
                    tvars_.push_back(std::make_unique<TypeVar>());
                    TypeVar *tv = tvars_.back().get();
                    tv->name = tp.name;
                    tv->boundRefs = tp.bounds;
                    tv->ofMethod = true;
                    m->tparams.push_back(tv);
                }
                for (auto *tv : m->tparams)
                    methodTvars_.push_back(tv);
                for (auto *tv : m->tparams)
                    for (auto &b : tv->boundRefs)
                        tv->bounds.push_back(resolveType(b));
                if (md->compactCtor)
                {
                    for (auto &p : d->recordComponents)
                    {
                        m->params.push_back(resolveType(p.type));
                        m->paramNames.push_back(p.name);
                    }
                }
                else
                    for (auto &p : md->params)
                    {
                        TypeP pt = resolveType(p.type);
                        if (pt->k == TK::Void)
                        {
                            error(p.type->pos, "'void' type not allowed here");
                            pt = tError();
                        }
                        m->params.push_back(pt);
                        m->paramNames.push_back(p.name);
                        if (p.varargs)
                            m->varargs = true;
                    }
                m->ret = m->isCtor ? prim(TK::Void) : resolveType(md->ret);
                for (auto &t : md->throws)
                    m->throws.push_back(resolveType(t));
                methodTvars_.resize(methodTvars_.size() - m->tparams.size());
                if ((m->mods & M_NATIVE) && units_[c->unit].isPrelude)
                    m->native = nativeName(c, m->name);
                if (m->isAbstract() && md->body && !iface)
                    error(md->namePos, "abstract methods cannot have a body");
                if (!m->isAbstract() && !md->body && !(m->mods & M_NATIVE) && !iface)
                    error(md->namePos, "missing method body, or declare abstract");
                if (m->isAbstract() && !iface && !(c->mods & M_ABSTRACT) && c->kind != ClassDecl::Enum)
                {
                    // reported once, with the class (javac's wording)
                }
                // a duplicate signature?
                std::vector<MethodSym *> &list = m->isCtor ? c->ctors : c->methods;
                for (MethodSym *o : list)
                    if (o->name == m->name && o->params.size() == m->params.size())
                    {
                        bool same = true;
                        for (size_t i = 0; i < o->params.size() && same; ++i)
                            same = erasedName(o->params[i]) == erasedName(m->params[i]);
                        if (same)
                            error(md->namePos, (m->isCtor ? "constructor " : "method ") + sigStr(m) +
                                                   " is already defined in " +
                                                   std::string(iface ? "interface " : "class ") + c->name);
                    }
                list.push_back(m);
            }
        }
    }
    // static state with initializers needs a <clinit>
    for (FieldSym *f : c->fields)
        if (f->isStatic && f->var && f->var->init)
            c->needsClinit = true;
    if (c->kind == ClassDecl::Enum)
        c->needsClinit = true;
    // the prelude's static state is set up before main runs (JavaEmit.cpp)
    if (units_[c->unit].isPrelude)
        c->needsClinit = false;

    // ── synthesized members ──
    if (c->kind == ClassDecl::Record && d)
    {
        bool hasCanonical = false;
        for (MethodSym *k : c->ctors)
        {
            if (k->params.size() != d->recordComponents.size())
                continue;
            bool same = true;
            for (size_t i = 0; i < k->params.size() && same; ++i)
                same = sameType(k->params[i], resolveType(d->recordComponents[i].type));
            if (same)
            {
                hasCanonical = true;
                if (!k->decl || !k->decl->compactCtor)
                    k->synth = Synth::None;
            }
        }
        if (!hasCanonical)
        {
            MethodSym *k = newMethod("<init>");
            k->isCtor = true;
            k->mods = M_PUBLIC;
            k->synth = Synth::RecordCtor;
            k->ret = prim(TK::Void);
            for (auto &p : d->recordComponents)
            {
                k->params.push_back(resolveType(p.type));
                k->paramNames.push_back(p.name);
            }
            k->pos = d->namePos;
            c->ctors.push_back(k);
        }
        for (FieldSym *f : c->fields)
        {
            if (!f->synthetic)
                continue;
            bool declared = false;
            for (MethodSym *m : c->methods)
                if (m->name == f->name && m->params.empty())
                    declared = true;
            if (!declared)
            {
                MethodSym *a = newMethod(f->name);
                a->mods = M_PUBLIC;
                a->synth = Synth::RecordAccessor;
                a->field = f;
                a->ret = f->type;
                a->pos = f->pos;
                c->methods.push_back(a);
            }
        }
        auto declared = [&](const std::string &n, size_t arity)
        {
            for (MethodSym *m : c->methods)
                if (m->name == n && m->params.size() == arity)
                    return true;
            return false;
        };
        if (!declared("equals", 1))
        {
            MethodSym *m = newMethod("equals");
            m->mods = M_PUBLIC | M_FINAL;
            m->synth = Synth::RecordEquals;
            m->params = {tObject()};
            m->paramNames = {"o"};
            m->ret = prim(TK::Boolean);
            c->methods.push_back(m);
        }
        if (!declared("hashCode", 0))
        {
            MethodSym *m = newMethod("hashCode");
            m->mods = M_PUBLIC | M_FINAL;
            m->synth = Synth::RecordHashCode;
            m->ret = prim(TK::Int);
            c->methods.push_back(m);
        }
        if (!declared("toString", 0))
        {
            MethodSym *m = newMethod("toString");
            m->mods = M_PUBLIC;
            m->synth = Synth::RecordToString;
            m->ret = tString();
            c->methods.push_back(m);
        }
    }
    if (c->kind == ClassDecl::Enum)
    {
        MethodSym *v = newMethod("values");
        v->mods = M_PUBLIC | M_STATIC;
        v->synth = Synth::EnumValues;
        v->ret = arrayOf(classType(c));
        c->methods.push_back(v);
        MethodSym *vo = newMethod("valueOf");
        vo->mods = M_PUBLIC | M_STATIC;
        vo->synth = Synth::EnumValueOf;
        vo->params = {tString()};
        vo->paramNames = {"name"};
        vo->ret = classType(c);
        c->methods.push_back(vo);
    }
    if (c->isAnon && c->ctors.empty())
    {
        // an anonymous class's constructor takes its superclass constructor's
        // arguments (after the superclass's outer instance, when it has one)
        MethodSym *k = newMethod("<init>");
        k->isCtor = true;
        k->synth = Synth::DefaultCtor;
        k->ret = prim(TK::Void);
        k->pos = d ? d->pos : Pos{};
        ClassSym *sup = c->super ? c->super->cls : nullptr;
        if (sup && sup->hasOuterInstance && anonSuperCtor_.count(c) && anonSuperCtor_[c])
        {
            k->params.push_back(classType(sup->outer));
            k->paramNames.push_back("__souter");
        }
        auto it = anonSuperParams_.find(c);
        if (it != anonSuperParams_.end())
            for (size_t i = 0; i < it->second.size(); ++i)
            {
                k->params.push_back(it->second[i]);
                k->paramNames.push_back("__a" + std::to_string(i));
            }
        c->ctors.push_back(k);
    }
    if (c->ctors.empty() && !iface && !c->isAnon)
    {
        MethodSym *k = newMethod("<init>");
        k->isCtor = true;
        k->mods = c->kind == ClassDecl::Enum ? M_PRIVATE : (c->mods & (M_PUBLIC | M_PROTECTED | M_PRIVATE));
        k->synth = Synth::DefaultCtor;
        k->ret = prim(TK::Void);
        k->pos = d ? d->namePos : Pos{};
        c->ctors.push_back(k);
    }
    unit_ = savedUnit;
    ctxClass_ = savedCtx;
}

// ─── Lookup ──────────────────────────────────────────────────────────────────

FieldSym *Sema::findField(ClassSym *c, const std::string &name)
{
    if (!c)
        return nullptr;
    enterMembers(c);
    for (FieldSym *f : c->fields)
        if (f->name == name)
            return f;
    if (c->super && c->super->cls)
        if (FieldSym *f = findField(c->super->cls, name))
            return f;
    for (auto &i : c->ifaces)
        if (i->cls)
            if (FieldSym *f = findField(i->cls, name))
                return f;
    return nullptr;
}

bool Sema::overrides(MethodSym *m, MethodSym *base, ClassSym *in)
{
    if (m == base || m->name != base->name || m->params.size() != base->params.size() || m->isCtor || base->isCtor)
        return false;
    if (base->isPrivate() || base->isStatic())
        return false;
    TypeP view = asSuper(classType(in, [&] {
                             std::vector<TypeP> a;
                             for (auto *tv : in->tparams)
                                 a.push_back(typeVar(tv));
                             return a;
                         }()),
                         base->owner);
    Subst s = view ? substOf(view) : Subst{};
    // method type parameters line up by position
    for (size_t i = 0; i < base->tparams.size() && i < m->tparams.size(); ++i)
        s[base->tparams[i]] = typeVar(m->tparams[i]);
    // m may itself be inherited (Enum.compareTo(E) seen from enum Color)
    Subst ms;
    if (m->owner != in)
    {
        std::vector<TypeP> own;
        for (auto *tv : in->tparams)
            own.push_back(typeVar(tv));
        if (TypeP mview = asSuper(classType(in, own), m->owner))
            ms = substOf(mview);
    }
    for (size_t i = 0; i < m->params.size(); ++i)
    {
        TypeP bp = subst(base->params[i], s);
        TypeP mp = ms.empty() ? m->params[i] : subst(m->params[i], ms);
        if (sameType(mp, bp) || sameType(m->params[i], bp))
            continue;
        if (erasedName(m->params[i]) == erasedName(bp) || erasedName(mp) == erasedName(bp))
            continue;
        if (erasedName(m->params[i]) == erasedName(base->params[i]))
            continue;
        return false;
    }
    return true;
}

void Sema::collectMethods(ClassSym *c, const std::string &name, std::vector<MethodSym *> &out)
{
    if (!c)
        return;
    enterMembers(c);
    std::vector<ClassSym *> work{c};
    std::set<ClassSym *> seen;
    std::vector<MethodSym *> found;
    // breadth-first: subclasses before their supertypes, so overriders win
    for (size_t k = 0; k < work.size(); ++k)
    {
        ClassSym *x = work[k];
        if (!seen.insert(x).second)
            continue;
        enterMembers(x);
        for (MethodSym *m : x->methods)
        {
            if (m->name != name)
                continue;
            bool hidden = false;
            for (MethodSym *f : found)
                if (overrides(f, m, f->owner) ||
                    (f->params.size() == m->params.size() && [&] {
                        for (size_t i = 0; i < f->params.size(); ++i)
                            if (erasedName(f->params[i]) != erasedName(m->params[i]))
                                return false;
                        return true;
                    }() && (f->owner == x || isSubClass(f->owner, x))))
                {
                    hidden = true;
                    break;
                }
            if (!hidden)
                found.push_back(m);
        }
        if (x->super && x->super->cls)
            work.push_back(x->super->cls);
        for (auto &i : x->ifaces)
            if (i->cls)
                work.push_back(i->cls);
        if (x->isInterface() && x->ifaces.empty())
            if (ClassSym *o = cls("java.lang.Object"))
                work.push_back(o);
    }
    out.insert(out.end(), found.begin(), found.end());
}

std::vector<MethodSym *> Sema::allAbstract(ClassSym *c)
{
    // abstract methods inherited and not implemented anywhere in c's hierarchy
    std::vector<MethodSym *> abstracts, concrete;
    std::set<ClassSym *> seen;
    std::function<void(ClassSym *)> walk = [&](ClassSym *x)
    {
        if (!x || !seen.insert(x).second)
            return;
        enterMembers(x);
        for (MethodSym *m : x->methods)
        {
            if (m->isStatic() || m->isCtor)
                continue;
            (m->isAbstract() ? abstracts : concrete).push_back(m);
        }
        if (x->super && x->super->cls)
            walk(x->super->cls);
        for (auto &i : x->ifaces)
            walk(i->cls);
    };
    walk(c);
    std::vector<MethodSym *> missing;
    for (MethodSym *a : abstracts)
    {
        bool impl = false;
        for (MethodSym *m : concrete)
            if (m->name == a->name && overrides(m, a, c))
            {
                impl = true;
                break;
            }
        if (!impl)
        {
            bool dup = false;
            for (MethodSym *x : missing)
                if (x->name == a->name && x->params.size() == a->params.size())
                    dup = true;
            if (!dup)
                missing.push_back(a);
        }
    }
    return missing;
}

// ─── Dispatch keys ───────────────────────────────────────────────────────────

void Sema::assignKeys(ClassSym *c)
{
    if (!c || c->state >= 4)
        return;
    enterMembers(c);
    c->state = 4;
    if (c->super && c->super->cls)
        assignKeys(c->super->cls);
    for (auto &i : c->ifaces)
        if (i->cls)
            assignKeys(i->cls);
    std::map<std::string, int> count;
    for (MethodSym *m : c->methods)
        ++count[m->name];
    std::map<std::string, int> seenName;
    for (MethodSym *m : c->methods)
    {
        std::string key = m->name + "(";
        for (size_t i = 0; i < m->params.size(); ++i)
            key += (i ? "," : "") + erasedName(m->params[i]);
        m->vkey = key + ")";
        int n = ++seenName[m->name];
        m->impl = c->binaryName + "." + m->name + (n > 1 ? "#" + std::to_string(n) : "");
        if (m->isStatic() || m->isPrivate())
            continue;
        // generic bridges: the keys of the methods it overrides
        std::set<ClassSym *> seen;
        std::function<void(const TypeP &)> walk = [&](const TypeP &st)
        {
            if (!st || !st->cls || !seen.insert(st->cls).second)
                return;
            ClassSym *s = st->cls;
            assignKeys(s);
            for (MethodSym *b : s->methods)
                if (b->name == m->name && overrides(m, b, c) && b->vkey != m->vkey &&
                    std::find(m->bridgeKeys.begin(), m->bridgeKeys.end(), b->vkey) == m->bridgeKeys.end())
                    m->bridgeKeys.push_back(b->vkey);
            if (s->super)
                walk(s->super);
            for (auto &i : s->ifaces)
                walk(i);
        };
        if (c->super)
            walk(c->super);
        for (auto &i : c->ifaces)
            walk(i);
    }
    int k = 0;
    for (MethodSym *m : c->ctors)
    {
        m->vkey = "<init>";
        m->impl = c->binaryName + ".<init>" + (++k > 1 ? "#" + std::to_string(k) : "");
    }
}

// ─── Class-level checks (user code) ──────────────────────────────────────────

void Sema::checkClass(ClassSym *c)
{
    assignKeys(c);
    int savedUnit = unit_;
    unit_ = c->unit;
    ClassDecl *d = c->decl;
    Pos at = d ? d->namePos : Pos{};
    if (c->isAnon && d)
        at = d->pos;
    if (!c->isInterface() && !(c->mods & M_ABSTRACT))
    {
        auto missing = allAbstract(c);
        // enum constants with bodies implement the enum's abstract methods
        bool enumWithBodies = c->kind == ClassDecl::Enum && d &&
                              std::any_of(d->enumConsts.begin(), d->enumConsts.end(),
                                          [](const EnumConst &e) { return e.body != nullptr; });
        if (!missing.empty() && !enumWithBodies)
        {
            MethodSym *a = missing[0];
            std::string who = c->isAnon ? typeStr(classType(c)) : c->name;
            error(at, who + " is not abstract and does not override abstract method " + sigStr(a) + " in " +
                          typeStr(classType(a->owner)));
        }
    }
    // overriding rules and @Override
    for (MethodSym *m : c->methods)
    {
        if (m->isStatic() || m->isCtor || !m->decl)
            continue;
        bool overridesSomething = false;
        std::set<ClassSym *> seen;
        std::function<void(const TypeP &)> walk = [&](const TypeP &st)
        {
            if (!st || !st->cls || !seen.insert(st->cls).second)
                return;
            ClassSym *s = st->cls;
            enterMembers(s);
            for (MethodSym *b : s->methods)
            {
                if (!overrides(m, b, c))
                    continue;
                overridesSomething = true;
                if (b->mods & M_FINAL)
                    error(m->pos, sigStr(m) + " in " + c->name + " cannot override " + sigStr(b) + " in " +
                                      b->owner->name,
                          {"  overridden method is final"});
                TypeP br = subst(b->ret, [&] {
                    TypeP view = asSuper(classType(c), b->owner);
                    return view ? substOf(view) : Subst{};
                }());
                if (m->ret && br && !isError(m->ret) && !isError(br) &&
                    !(sameType(m->ret, br) || (isRef(m->ret) && isRef(br) && isSubtype(m->ret, br)) ||
                      br->k == TK::TypeVar || m->ret->k == TK::TypeVar))
                    error(m->pos, sigStr(m) + " in " + c->name + " cannot " +
                                      (s->isInterface() && !c->isInterface() ? "implement " : "override ") +
                                      sigStr(b) + " in " + b->owner->name,
                          {"  return type " + typeStr(m->ret) + " is not compatible with " + typeStr(br)});
                auto rank = [](unsigned f) { return f & M_PUBLIC ? 3 : f & M_PROTECTED ? 2 : f & M_PRIVATE ? 0 : 1; };
                unsigned bm = b->mods | (b->owner->isInterface() ? M_PUBLIC : 0);
                if (rank(m->mods) < rank(bm))
                    error(m->pos, sigStr(m) + " in " + c->name + " cannot " +
                                      (s->isInterface() && !c->isInterface() ? "implement " : "override ") +
                                      sigStr(b) + " in " + b->owner->name,
                          {"  attempting to assign weaker access privileges; was " +
                           std::string(rank(bm) == 3 ? "public" : rank(bm) == 2 ? "protected" : "package")});
            }
            if (s->super)
                walk(s->super);
            for (auto &i : s->ifaces)
                walk(i);
        };
        if (c->super)
            walk(c->super);
        for (auto &i : c->ifaces)
            walk(i);
        if (c->isInterface() || c->ifaces.empty())
            walk(tObject());
        bool hasOverride = std::find(m->decl->mods.annotations.begin(), m->decl->mods.annotations.end(), "Override") !=
                           m->decl->mods.annotations.end();
        if (hasOverride && !overridesSomething)
            error(m->decl->mods.pos, "method does not override or implement a method from a supertype");
    }
    unit_ = savedUnit;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

std::string Sema::temp()
{
    std::string n = "__t" + std::to_string(++tempCounter_);
    if (fn_)
        fn_->temps.push_back(n);
    return n;
}

void Sema::pushScope()
{
    if (fn_)
        fn_->scopes.emplace_back();
}

void Sema::popScope()
{
    if (!fn_ || fn_->scopes.empty())
        return;
    // a local read by a lambda / inner class must be (effectively) final
    for (auto &v : fn_->scopes.back().vars)
    {
        if (v.captureSites.empty() || v.isFinal)
            continue;
        bool effectivelyFinal = v.hasInit ? v.assignCount == 0 : v.assignCount <= 1;
        if (effectivelyFinal)
            continue;
        int saved = unit_;
        unit_ = v.captureUnit;
        for (auto &site : v.captureSites)
            error(site.first, std::string("local variables referenced from ") +
                                  (site.second ? "a lambda expression" : "an inner class") +
                                  " must be final or effectively final");
        unit_ = saved;
    }
    fn_->scopes.pop_back();
}

LocalVar *Sema::findLocal(const std::string &n, FnCtx **owner)
{
    for (FnCtx *f = fn_; f; f = f->outer)
    {
        for (size_t i = f->scopes.size(); i-- > 0;)
            for (auto it = f->scopes[i].vars.rbegin(); it != f->scopes[i].vars.rend(); ++it)
                if (it->name == n) // the latest declaration (pattern variables may repeat a name)
                {
                    if (owner)
                        *owner = f;
                    return &*it;
                }
        // a local / anonymous class body sees its enclosing method's locals
        // through captures (handled by the caller)
    }
    return nullptr;
}

std::string Sema::uniqueLocal(const std::string &n)
{
    // Java names can collide with globals the runtime defines (print, len, ...)
    // only through locals, which shadow them — kept as written.
    return n;
}

LocalVar &Sema::declareLocal(const std::string &n, const TypeP &t, Pos pos, bool isFinal)
{
    LocalVar v;
    v.name = n;
    v.q = uniqueLocal(n);
    v.type = t;
    v.isFinal = isFinal;
    v.fn = fn_;
    v.pos = pos;
    v.index = fn_ ? fn_->localCount++ : 0;
    fn_->scopes.back().vars.push_back(v);
    return fn_->scopes.back().vars.back();
}

bool Sema::inStaticContext() const { return !fn_ || fn_->isStatic; }

} // namespace java
