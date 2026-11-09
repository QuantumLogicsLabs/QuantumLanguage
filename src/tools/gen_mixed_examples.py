#!/usr/bin/env python3
"""Generate (and check) mixed-syntax Quantum example programs.

Each program is written once in a small neutral form (the node classes
below) and rendered twice:

  * as a Quantum .sa file in which every block picks its own syntax: Python
    (colon + indentation), brace-style JavaScript/Quantum, C-style
    parenthesised headers, Ruby (do/end), C++ range-for and cout, printf,
    console.log, puts, arrow functions, `fn`, typed C declarations, Python,
    JavaScript and Ruby classes (constructed with `new X()`, `X()` or
    `X.new()`), try/catch, try/except and begin/rescue;
  * as plain Python, which CPython runs to give the expected output.

  python src/tools/gen_mixed_examples.py                 write examples/mixed_syntax/
  python src/tools/gen_mixed_examples.py --check [QRUN]  also run every file and
                                                         compare with CPython

The output is deterministic: the same count and seed give the same files.
"""
import argparse
import os
import random
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

# ─── the neutral program form ────────────────────────────────────────────────
# Expressions are strings in a Python subset that both renderings accept:
# integer arithmetic (no bare `/` except under floor() or in a %.2f value),
# `and`/`or`/`not (...)`, len/str/abs/ord/chr/min/max/sum/sorted, list
# indexing and .append, dicts keyed by strings or numbers. Three markers are
# rendered per target: `new:Name(` (object creation), `NONE` (null) and
# `self.` inside class methods.


class S:  # a simple statement (assignment, call, augmented assignment)
    def __init__(self, text):
        self.text = text


class Let:  # a variable's first assignment
    def __init__(self, name, expr, hint="any"):
        self.name, self.expr, self.hint = name, expr, hint


class If:
    def __init__(self, branches, els=None):
        self.branches, self.els = branches, els


class For:  # for var in range(lo, hi)
    def __init__(self, var, lo, hi, body):
        self.var, self.lo, self.hi, self.body = var, lo, hi, body


class Each:  # for var in iterable (a list)
    def __init__(self, var, it, body):
        self.var, self.it, self.body = var, it, body


class While:
    def __init__(self, cond, body):
        self.cond, self.body = cond, body


class Def:
    def __init__(self, name, params, body, sig=None):
        # sig: optional C types, ("int", ["int", "int"]), for a typed header
        self.name, self.params, self.body, self.sig = name, params, body, sig


class Ret:
    def __init__(self, expr=None):
        self.expr = expr


class Out:  # print one value: kind i (integer), s (string), l (list of ints)
    def __init__(self, expr, kind="s"):
        self.expr, self.kind = expr, kind


class OutF:  # print an interpolated string: parts are literals or E(expr)
    def __init__(self, *parts):
        self.parts = parts


class E:
    def __init__(self, expr):
        self.expr = expr


class Printf:  # print a %-format (with %d %s %.Nf) and its arguments
    def __init__(self, fmt, *args):
        self.fmt, self.args = fmt, args


class Brk:
    pass


class Cont:
    pass


class Class:
    def __init__(self, name, params, init, methods, base=None):
        # init: list of nodes (assigning self.field); methods: list of Def
        self.name, self.params, self.init, self.methods, self.base = name, params, init, methods, base


class Try:  # runs body; on an exception runs handler
    def __init__(self, body, handler):
        self.body, self.handler = body, handler


class Raise:
    def __init__(self, msg):
        self.msg = msg  # a string literal's text


class MapL:  # dst = [expr for var in src]
    def __init__(self, dst, src, var, expr):
        self.dst, self.src, self.var, self.expr = dst, src, var, expr


class FilterL:  # dst = [var for var in src if cond]
    def __init__(self, dst, src, var, cond):
        self.dst, self.src, self.var, self.cond = dst, src, var, cond


class Comment:
    def __init__(self, text):
        self.text = text


BLOCKS = (If, For, Each, While, Def, Class, Try)


def children(n):
    if isinstance(n, If):
        out = [b for _, b in n.branches]
        if n.els:
            out.append(n.els)
        return out
    if isinstance(n, (For, Each, While)):
        return [n.body]
    if isinstance(n, Try):
        return [n.body, n.handler]
    return []


def contains(nodes, kinds):
    """True when nodes hold one of kinds, not looking into nested defs/classes."""
    for n in nodes:
        if isinstance(n, kinds):
            return True
        for body in children(n):
            if contains(body, kinds):
                return True
    return False


# ─── rendering as Python (the oracle) ────────────────────────────────────────

def py_expr(e):
    e = re.sub(r"\bnew:", "", e)
    return re.sub(r"\bNONE\b", "None", e)


def py_lines(nodes, ind=0, in_class=False):
    pad = "    " * ind
    out = []
    for n in nodes:
        if isinstance(n, Comment):
            out.append(pad + "# " + n.text)
        elif isinstance(n, S):
            out.append(pad + py_expr(n.text))
        elif isinstance(n, Let):
            out.append(f"{pad}{n.name} = {py_expr(n.expr)}")
        elif isinstance(n, If):
            for k, (c, b) in enumerate(n.branches):
                out.append(f"{pad}{'if' if k == 0 else 'elif'} {py_expr(c)}:")
                out += py_lines(b, ind + 1)
            if n.els:
                out.append(pad + "else:")
                out += py_lines(n.els, ind + 1)
        elif isinstance(n, For):
            out.append(f"{pad}for {n.var} in range({py_expr(n.lo)}, {py_expr(n.hi)}):")
            out += py_lines(n.body, ind + 1)
        elif isinstance(n, Each):
            out.append(f"{pad}for {n.var} in {py_expr(n.it)}:")
            out += py_lines(n.body, ind + 1)
        elif isinstance(n, While):
            out.append(f"{pad}while {py_expr(n.cond)}:")
            out += py_lines(n.body, ind + 1)
        elif isinstance(n, Def):
            params = (["self"] if in_class else []) + n.params
            out.append(f"{pad}def {n.name}({', '.join(params)}):")
            out += py_lines(n.body, ind + 1)
        elif isinstance(n, Ret):
            out.append(pad + "return" + (" " + py_expr(n.expr) if n.expr else ""))
        elif isinstance(n, Out):
            out.append(f"{pad}print({py_expr(n.expr)})")
        elif isinstance(n, OutF):
            pieces = [repr(p) if isinstance(p, str) else f"str({py_expr(p.expr)})" for p in n.parts]
            out.append(f"{pad}print({' + '.join(pieces)})")
        elif isinstance(n, Printf):
            args = ", ".join(py_expr(a) for a in n.args)
            out.append(f"{pad}print({n.fmt!r} % ({args},))")
        elif isinstance(n, Brk):
            out.append(pad + "break")
        elif isinstance(n, Cont):
            out.append(pad + "continue")
        elif isinstance(n, Class):
            out.append(f"{pad}class {n.name}{'(' + n.base + ')' if n.base else ''}:")
            if n.params is not None:
                out.append(f"{pad}    def __init__({', '.join(['self'] + n.params)}):")
                out += py_lines(n.init, ind + 2) or [pad + "        pass"]
            for m in n.methods:
                out += py_lines([m], ind + 1, in_class=True)
        elif isinstance(n, Try):
            out.append(pad + "try:")
            out += py_lines(n.body, ind + 1)
            out.append(pad + "except Exception:")
            out += py_lines(n.handler, ind + 1)
        elif isinstance(n, Raise):
            out.append(f"{pad}raise Exception({n.msg!r})")
        elif isinstance(n, MapL):
            out.append(f"{pad}{n.dst} = [{py_expr(n.expr)} for {n.var} in {py_expr(n.src)}]")
        elif isinstance(n, FilterL):
            out.append(f"{pad}{n.dst} = [{n.var} for {n.var} in {py_expr(n.src)} if {py_expr(n.cond)}]")
        else:
            raise TypeError(n)
    return out


def render_python(nodes):
    return "from math import floor, sqrt\n" + "\n".join(py_lines(nodes)) + "\n"


# ─── rendering as mixed-syntax Quantum ───────────────────────────────────────

STYLE_NAMES = {
    "py": "Python blocks", "brace": "brace blocks", "c": "C-style headers", "rb": "Ruby do/end",
    "js": "JavaScript", "cpp": "C++", "fn": "Quantum fn", "arrow": "arrow functions",
}


class Quantum:
    def __init__(self, rng):
        self.r = rng
        self.used = set()

    def pick(self, options):
        choice = self.r.choice(options)
        return choice

    # expressions ------------------------------------------------------------
    def expr(self, e, c_ops=False, cls=None):
        r = self.r

        def new(m):
            name = m.group(1)
            style = r.choice(["new", "call", "rb"])
            self.used.add({"new": "js", "call": "py", "rb": "rb"}[style])
            return {"new": f"new {name}(", "call": f"{name}(", "rb": f"{name}.new("}[style]

        e = re.sub(r"\bnew:(\w+)\(", new, e)
        e = re.sub(r"\bNONE\b", lambda m: r.choice(["null", "None", "nil"]), e)
        e = re.sub(r"\.append\(", lambda m: r.choice([".append(", ".push(", ".push_back("]), e)
        if cls is not None:
            e = self.self_refs(e, cls)
        if c_ops and r.random() < 0.6:
            e = re.sub(r"\bnot \(", "!(", e)
            e = re.sub(r"\band\b", "&&", e)
            e = re.sub(r"\bor\b", "||", e)
        return e

    def self_refs(self, e, cls):
        style, fields = cls
        if style == "js":
            return re.sub(r"\bself\.", "this.", e)
        if style == "rb":
            return re.sub(r"\bself\.(\w+)\b(?!\s*\()", lambda m: "@" + m.group(1) if m.group(1) in fields else m.group(0), e)
        return e

    def semi(self, brace):
        return ";" if brace and self.r.random() < 0.4 else ""

    # statements -------------------------------------------------------------
    def lines(self, nodes, ind, brace, cls=None):
        out = []
        for n in nodes:
            out += self.node(n, ind, brace, cls)
        return out

    def block(self, header, body, ind, style, cls, end=None):
        """A block header and its body in one of the block styles."""
        pad = "    " * ind
        if style == "py":
            return [pad + header + ":"] + self.lines(body, ind + 1, False, cls)
        if style == "rb":
            return [pad + header] + self.lines(body, ind + 1, False, cls) + [pad + "end"]
        return [pad + header + " {"] + self.lines(body, ind + 1, True, cls) + [pad + (end or "}")]

    def node(self, n, ind, brace, cls):
        r = self.r
        pad = "    " * ind
        X = lambda e, c=False: self.expr(e, c, cls)
        if isinstance(n, Comment):
            return [pad + r.choice(["# ", "// "]) + n.text]
        if isinstance(n, S):
            return [pad + X(n.text) + self.semi(brace)]
        if isinstance(n, Let):
            forms = ["plain", "let"]
            if n.hint in ("int", "str", "float"):
                forms.append("typed")
            form = r.choice(forms)
            if form == "plain":
                return [f"{pad}{n.name} = {X(n.expr)}"]
            if form == "let":
                self.used.add("js")
                return [f"{pad}let {n.name} = {X(n.expr)}{self.semi(True)}"]
            self.used.add("cpp")
            ctype = {"int": "int", "str": "string", "float": "double"}[n.hint]
            return [f"{pad}{ctype} {n.name} = {X(n.expr)}{self.semi(True)}"]
        if isinstance(n, If):
            style = r.choice(["py", "brace", "c", "rb"])
            self.used.add(style)
            out = []
            for k, (c, body) in enumerate(n.branches):
                cond = X(c, style in ("brace", "c"))
                if style == "py":
                    out += self.block(("if " if k == 0 else "elif ") + cond, body, ind, "py", cls)
                elif style == "rb":
                    out.append(pad + ("if " if k == 0 else "elsif ") + cond)
                    out += self.lines(body, ind + 1, False, cls)
                else:
                    head = f"if ({cond})" if style == "c" else f"if {cond}"
                    out.append(pad + ("" if k == 0 else "} else ") + head + " {")
                    out += self.lines(body, ind + 1, True, cls)
            if n.els:
                if style == "py":
                    out += self.block("else", n.els, ind, "py", cls)
                elif style == "rb":
                    out.append(pad + "else")
                    out += self.lines(n.els, ind + 1, False, cls)
                else:
                    out.append(pad + "} else {")
                    out += self.lines(n.els, ind + 1, True, cls)
            if style == "rb":
                out.append(pad + "end")
            elif style in ("brace", "c"):
                out.append(pad + "}")
            return out
        if isinstance(n, For):
            styles = ["py", "brace", "c", "cpp"]
            if not contains(n.body, (Brk, Cont, Ret)):
                styles.append("rb")  # a Ruby block is a closure: no break/return through it
            style = r.choice(styles)
            self.used.add(style)
            lo, hi = X(n.lo), X(n.hi)
            if style == "py":
                return self.block(f"for {n.var} in range({lo}, {hi})", n.body, ind, "py", cls)
            if style == "brace":
                return self.block(f"for {n.var} in range({lo}, {hi})", n.body, ind, "brace", cls)
            if style == "c":
                kw = r.choice(["let ", "int ", ""])
                step = r.choice([f"{n.var}++", f"{n.var} += 1", f"++{n.var}"])
                return self.block(f"for ({kw}{n.var} = {lo}; {n.var} < {hi}; {step})", n.body, ind, "brace", cls)
            if style == "cpp":
                return self.block(f"for (int {n.var} = {lo}; {n.var} < {hi}; {n.var}++)", n.body, ind, "brace", cls)
            return self.block(f"({lo}...{hi}).each do |{n.var}|", n.body, ind, "rb", cls)
        if isinstance(n, Each):
            styles = ["py", "brace", "js", "cpp"]
            if not contains(n.body, (Brk, Cont, Ret)):
                styles.append("rb")
            style = r.choice(styles)
            self.used.add(style)
            it = X(n.it)
            if style == "py":
                return self.block(f"for {n.var} in {it}", n.body, ind, "py", cls)
            if style == "brace":
                return self.block(f"for {n.var} in {it}", n.body, ind, "brace", cls)
            if style == "js":
                return self.block(f"for ({r.choice(['let', 'const'])} {n.var} of {it})", n.body, ind, "brace", cls)
            if style == "cpp":
                return self.block(f"for ({r.choice(['auto', 'auto&', 'const auto&'])} {n.var} : {it})", n.body, ind, "brace", cls)
            target = it if re.fullmatch(r"[\w.]+(\(\))?", it) else f"({it})"
            return self.block(f"{target}.each do |{n.var}|", n.body, ind, "rb", cls)
        if isinstance(n, While):
            style = r.choice(["py", "brace", "c", "rb"])
            self.used.add(style)
            cond = X(n.cond, style in ("brace", "c"))
            if style == "py":
                return self.block(f"while {cond}", n.body, ind, "py", cls)
            if style == "rb":
                return self.block(f"while {cond}", n.body, ind, "rb", cls)
            return self.block(f"while ({cond})" if style == "c" else f"while {cond}", n.body, ind, "brace", cls)
        if isinstance(n, Def):
            if cls is not None:
                return self.method(n, ind, cls)
            styles = ["py", "js", "fn", "fnpy", "rb", "arrow"]
            if n.sig:
                styles.append("cpp")
            style = r.choice(styles)
            self.used.add({"fnpy": "fn", "py": "py", "js": "js", "fn": "fn", "rb": "rb", "arrow": "arrow", "cpp": "cpp"}[style])
            ps = ", ".join(n.params)
            if style == "py":
                return self.block(f"def {n.name}({ps})", n.body, ind, "py", None)
            if style == "fnpy":
                return self.block(f"fn {n.name}({ps})", n.body, ind, "py", None)
            if style == "js":
                return self.block(f"function {n.name}({ps})", n.body, ind, "brace", None)
            if style == "fn":
                return self.block(f"fn {n.name}({ps})", n.body, ind, "brace", None)
            if style == "rb":
                return self.block(f"def {n.name}({ps})", n.body, ind, "rb", None)
            if style == "arrow":
                kw = r.choice(["const", "let"])
                return self.block(f"{kw} {n.name} = ({ps}) =>", n.body, ind, "brace", None)
            ret, types = n.sig
            typed = ", ".join(f"{t} {p}" for t, p in zip(types, n.params))
            return self.block(f"{ret} {n.name}({typed})", n.body, ind, "brace", None)
        if isinstance(n, Ret):
            if n.expr is None:
                return [pad + "return" + self.semi(brace)]
            return [pad + "return " + X(n.expr) + self.semi(brace)]
        if isinstance(n, Out):
            return [pad + self.out(X(n.expr), n.kind) + self.semi(brace)]
        if isinstance(n, OutF):
            return [pad + self.outf(n.parts, cls) + self.semi(brace)]
        if isinstance(n, Printf):
            args = ", ".join(X(a) for a in n.args)
            form = r.choice(["printf", "printf", "format", "sprintf", "percent"])
            if form == "printf":
                self.used.add("c")
                return [pad + f'printf("{n.fmt}\\n", {args})' + self.semi(brace)]
            if form == "format":
                return [pad + f'print(format("{n.fmt}", {args}))' + self.semi(brace)]
            if form == "percent":
                self.used.add("py")
                return [pad + f'print("{n.fmt}" % ({args}))' + self.semi(brace)]
            self.used.add("js")
            return [pad + f'console.log(sprintf("{n.fmt}", {args}))' + self.semi(brace)]
        if isinstance(n, Brk):
            return [pad + "break" + self.semi(brace)]
        if isinstance(n, Cont):
            return [pad + "continue" + self.semi(brace)]
        if isinstance(n, Class):
            return self.klass(n, ind)
        if isinstance(n, Try):
            style = r.choice(["py", "js", "rb"])
            self.used.add(style)
            if style == "py":
                return ([pad + "try:"] + self.lines(n.body, ind + 1, False, cls) +
                        [pad + r.choice(["except Exception:", "except Exception as err:", "except:"])] +
                        self.lines(n.handler, ind + 1, False, cls))
            if style == "js":
                return ([pad + "try {"] + self.lines(n.body, ind + 1, True, cls) +
                        [pad + r.choice(["} catch (err) {", "} catch (e) {"])] +
                        self.lines(n.handler, ind + 1, True, cls) + [pad + "}"])
            return ([pad + "begin"] + self.lines(n.body, ind + 1, False, cls) +
                    [pad + r.choice(["rescue => e", "rescue StandardError => e", "rescue"])] +
                    self.lines(n.handler, ind + 1, False, cls) + [pad + "end"])
        if isinstance(n, Raise):
            form = r.choice(["py", "js", "jsnew", "rb"])
            self.used.add({"py": "py", "js": "js", "jsnew": "js", "rb": "rb"}[form])
            msg = '"' + n.msg + '"'
            return [pad + {"py": f"raise Exception({msg})", "js": f"throw {msg}" + self.semi(brace),
                           "jsnew": f"throw new Error({msg})" + self.semi(brace), "rb": f"raise {msg}"}[form]]
        if isinstance(n, MapL):
            src, e, v = X(n.src), X(n.expr, True), n.var
            form = r.choice(["comp", "arrow", "fn", "rbblock", "lambda", "function"])
            self.used.add({"comp": "py", "arrow": "arrow", "fn": "fn", "rbblock": "rb", "lambda": "py", "function": "js"}[form])
            rhs = {"comp": f"[{X(n.expr)} for {v} in {src}]", "arrow": f"{src}.map({v} => {e})",
                   "fn": f"{src}.map(fn({v}) {{ return {e} }})", "rbblock": f"{src}.map {{ |{v}| {e} }}",
                   "lambda": f"list(map(lambda {v}: {X(n.expr)}, {src}))",
                   "function": f"{src}.map(function({v}) {{ return {e}; }})"}[form]
            return [f"{pad}{n.dst} = {rhs}"]
        if isinstance(n, FilterL):
            src, c, v = X(n.src), X(n.cond, True), n.var
            form = r.choice(["comp", "arrow", "fn", "rbblock", "lambda"])
            self.used.add({"comp": "py", "arrow": "arrow", "fn": "fn", "rbblock": "rb", "lambda": "py"}[form])
            rhs = {"comp": f"[{v} for {v} in {src} if {X(n.cond)}]", "arrow": f"{src}.filter({v} => {c})",
                   "fn": f"{src}.filter(fn({v}) {{ return {c} }})", "rbblock": f"{src}.select {{ |{v}| {c} }}",
                   "lambda": f"list(filter(lambda {v}: {X(n.cond)}, {src}))"}[form]
            return [f"{pad}{n.dst} = {rhs}"]
        raise TypeError(n)

    def out(self, e, kind):
        r = self.r
        simple = re.fullmatch(r"[\w.\[\]]+(\([^()]*\))?", e) is not None
        forms = ["print", "console", "cout", "puts"]
        if kind == "i":
            forms.append("printf_d")
        if kind == "s":
            forms.append("printf_s")
        if kind == "l":
            forms = ["print", "console"]
        form = r.choice(forms)
        self.used.add({"print": "py", "console": "js", "cout": "cpp", "puts": "rb",
                       "printf_d": "c", "printf_s": "c"}[form])
        if form == "print":
            return f"print({e})"
        if form == "console":
            return f"console.log({e})"
        if form == "cout":
            v = e if simple else f"({e})"
            return r.choice([f"cout << {v} << endl", f"std::cout << {v} << std::endl", f'cout << {v} << "\\n"'])
        if form == "puts":
            return f"puts {e}" if simple else f"puts({e})"
        if form == "printf_d":
            return f'printf("%d\\n", {e})'
        return f'printf("%s\\n", {e})'

    def outf(self, parts, cls):
        r = self.r
        X = lambda e: self.expr(e, False, cls)
        form = r.choice(["fstr", "template", "rbinterp", "concat", "cout", "multi"])
        exprs_simple = all(isinstance(p, str) or '"' not in p.expr for p in parts)
        if not exprs_simple and form in ("fstr", "template", "rbinterp"):
            form = "concat"
        if form == "multi" and not all(isinstance(p, str) or True for p in parts):
            form = "concat"
        if form == "fstr":
            self.used.add("py")
            return "print(f\"" + "".join(p if isinstance(p, str) else "{" + X(p.expr) + "}" for p in parts) + "\")"
        if form == "template":
            self.used.add("js")
            return "console.log(`" + "".join(p if isinstance(p, str) else "${" + X(p.expr) + "}" for p in parts) + "`)"
        if form == "rbinterp":
            self.used.add("rb")
            return "puts \"" + "".join(p if isinstance(p, str) else "#{" + X(p.expr) + "}" for p in parts) + "\""
        if form == "cout":
            self.used.add("cpp")
            items = ['"' + p + '"' if isinstance(p, str) else "(" + X(p.expr) + ")" for p in parts]
            return "cout << " + " << ".join(items) + " << endl"
        if form == "multi" and len(parts) >= 2:
            # print("a", x) puts one space between its arguments: keep the
            # literal's trailing space out of the argument
            items, ok = [], True
            for p in parts:
                if isinstance(p, str):
                    if items and not p.startswith(" "):
                        ok = False
                    if p.endswith(" ") and p is not parts[-1]:
                        p = p[:-1]
                    elif p is not parts[-1]:
                        ok = False
                    if items:
                        p = p[1:] if p.startswith(" ") else p
                    if p:
                        items.append('"' + p + '"')
                else:
                    items.append(X(p.expr))
            if ok:
                self.used.add("py")
                return r.choice(["print", "console.log"]) + "(" + ", ".join(items) + ")"
        items = ['"' + p + '"' if isinstance(p, str) else "str(" + X(p.expr) + ")" for p in parts]
        return r.choice(["print", "console.log"]) + "(" + " + ".join(items) + ")"

    def method(self, m, ind, cls):
        style, fields = cls
        pad = "    " * ind
        ps = ", ".join(m.params)
        if style == "py":
            return self.block(f"def {m.name}({', '.join(['self'] + m.params)})", m.body, ind, "py", cls)
        if style == "qpy":
            return self.block(f"fn {m.name}({ps})", m.body, ind, self.r.choice(["py", "brace"]), cls)
        if style == "js":
            return self.block(f"{m.name}({ps})", m.body, ind, "brace", cls)
        return self.block(f"def {m.name}({ps})", m.body, ind, "rb", cls)

    def klass(self, n, ind):
        r = self.r
        pad = "    " * ind
        style = r.choice(["py", "qpy", "js", "rb"])
        self.used.add({"py": "py", "qpy": "fn", "js": "js", "rb": "rb"}[style])
        fields = set(re.findall(r"\bself\.(\w+)\s*=", "\n".join(s.text for s in n.init if isinstance(s, S))))
        fields |= getattr(n, "inherited_fields", set())
        cls = (style, fields)
        out = []
        if style in ("py", "qpy"):
            if n.base:
                head = f"class {n.name}({n.base}):" if style == "py" else f"class {n.name} extends {n.base}:"
            else:
                head = f"class {n.name}:"
            out.append(pad + head)
            if n.params is not None:
                if style == "py":
                    out += self.block(f"def __init__({', '.join(['self'] + n.params)})", n.init, ind + 1, "py", cls)
                else:
                    out += self.block(f"fn init({', '.join(n.params)})", n.init, ind + 1, r.choice(["py", "brace"]), cls)
            for m in n.methods:
                out += self.method(m, ind + 1, cls)
            return out
        if style == "js":
            out.append(pad + f"class {n.name}" + (f" extends {n.base}" if n.base else "") + " {")
            if n.params is not None:
                out += self.block(f"constructor({', '.join(n.params)})", n.init, ind + 1, "brace", cls)
            for m in n.methods:
                out += self.method(m, ind + 1, cls)
            out.append(pad + "}")
            return out
        out.append(pad + f"class {n.name}" + (f" < {n.base}" if n.base else ""))
        if fields and n.params is not None:
            out.append(pad + "    attr_accessor " + ", ".join(":" + f for f in sorted(fields)))
        if n.params is not None:
            out += self.block(f"def initialize({', '.join(n.params)})", n.init, ind + 1, "rb", cls)
        for m in n.methods:
            out += self.method(m, ind + 1, cls)
        out.append(pad + "end")
        return out


# ─── the programs ────────────────────────────────────────────────────────────

WORDS = ["apple", "river", "stone", "cloud", "tiger", "maple", "ocean", "piano", "quartz", "lemon",
         "comet", "delta", "ember", "frost", "grape", "harbor", "island", "jungle", "kettle", "lantern",
         "meadow", "nectar", "orbit", "pepper", "quiver", "raven", "saddle", "timber", "velvet", "willow"]
PALS = ["level", "radar", "civic", "kayak", "refer", "rotor", "madam", "noon", "stats", "tenet"]


def ints(r, n, lo, hi):
    return [r.randint(lo, hi) for _ in range(n)]


def lit(xs):
    return "[" + ", ".join(repr(x) if not isinstance(x, str) else '"' + x + '"' for x in xs) + "]"


def t_fizzbuzz(r):
    a, b = r.choice([(3, 5), (2, 7), (3, 4), (4, 6), (5, 7)])
    words = r.choice([("Fizz", "Buzz"), ("Ping", "Pong"), ("Foo", "Bar"), ("Tick", "Tock")])
    n = r.randint(12, 30)
    return f"{words[0]}{words[1]} up to {n}", [
        Def("label", ["i"], [
            If([(f"i % {a} == 0 and i % {b} == 0", [Ret(f'"{words[0]}{words[1]}"')]),
                (f"i % {a} == 0", [Ret(f'"{words[0]}"')]),
                (f"i % {b} == 0", [Ret(f'"{words[1]}"')])]),
            Ret("str(i)")]),
        Let("special", "0", "int"),
        For("i", "1", str(n + 1), [
            Out("label(i)", "s"),
            If([("label(i) != str(i)", [S("special += 1")])])]),
        OutF("special values: ", E("special")),
    ]


def t_factorial(r):
    m = r.randint(6, 14)
    return f"factorials up to {m}", [
        Def("fact_iter", ["n"], [
            Let("acc", "1", "int"),
            For("k", "2", "n + 1", [S("acc *= k")]),
            Ret("acc")], sig=("int", ["int"])),
        Def("fact_rec", ["n"], [
            If([("n <= 1", [Ret("1")])]),
            Ret("n * fact_rec(n - 1)")], sig=("int", ["int"])),
        Let("mismatches", "0", "int"),
        For("k", "0", str(m + 1), [
            OutF(E("k"), "! = ", E("fact_iter(k)")),
            If([("fact_iter(k) != fact_rec(k)", [S("mismatches += 1")])])]),
        OutF("mismatches: ", E("mismatches")),
    ]


def t_fibonacci(r):
    n = r.randint(10, 25)
    k = r.randint(8, 15)
    return f"Fibonacci sequence of {n} terms", [
        Def("fib_list", ["n"], [
            Let("seq", "[0, 1]"),
            While("len(seq) < n", [S("seq.append(seq[len(seq) - 1] + seq[len(seq) - 2])")]),
            Ret("seq")]),
        Def("fib_rec", ["n"], [
            If([("n < 2", [Ret("n")])]),
            Ret("fib_rec(n - 1) + fib_rec(n - 2)")], sig=("int", ["int"])),
        Let("seq", f"fib_list({n})"),
        Out("seq", "l"),
        OutF("fib_rec(", E(str(k)), ") = ", E(f"fib_rec({k})")),
        Let("evens", "0", "int"),
        Each("v", "seq", [If([("v % 2 == 0", [S("evens += 1")])])]),
        OutF("even terms: ", E("evens")),
    ]


def t_gcd(r):
    pairs = [(r.randint(2, 200), r.randint(2, 200)) for _ in range(r.randint(4, 7))]
    return "gcd and lcm of number pairs", [
        Def("gcd", ["a", "b"], [
            While("b != 0", [Let("t", "b"), S("b = a % b"), S("a = t")]),
            Ret("a")], sig=("int", ["int", "int"])),
        Def("lcm", ["a", "b"], [Ret("floor(a * b / gcd(a, b))")]),
        Let("pairs", "[" + ", ".join(f"[{a}, {b}]" for a, b in pairs) + "]"),
        Each("p", "pairs", [
            OutF("gcd(", E("p[0]"), ", ", E("p[1]"), ") = ", E("gcd(p[0], p[1])"),
                 ", lcm = ", E("lcm(p[0], p[1])"))]),
    ]


def t_sieve(r):
    n = r.randint(30, 150)
    return f"prime sieve up to {n}", [
        Def("sieve", ["n"], [
            Let("flags", "[]"),
            For("i", "0", "n + 1", [S("flags.append(1)")]),
            S("flags[0] = 0"), S("flags[1] = 0"),
            For("p", "2", "n + 1", [
                If([("flags[p] == 1", [
                    Let("m", "p * p", "int"),
                    While("m <= n", [S("flags[m] = 0"), S("m += p")])])])]),
            Let("primes", "[]"),
            For("i", "2", "n + 1", [If([("flags[i] == 1", [S("primes.append(i)")])])]),
            Ret("primes")]),
        Let("primes", f"sieve({n})"),
        Out("primes", "l"),
        OutF("count: ", E("len(primes)"), ", largest: ", E("primes[len(primes) - 1]")),
        Let("twins", "0", "int"),
        For("i", "1", "len(primes)", [If([("primes[i] - primes[i - 1] == 2", [S("twins += 1")])])]),
        OutF("twin pairs: ", E("twins")),
    ]


def copy_list(dst, src):
    return [Let(dst, "[]"), Each("v", src, [S(f"{dst}.append(v)")])]


def t_bubble(r):
    data = ints(r, r.randint(6, 12), -20, 99)
    return "bubble sort", [
        Def("bubble_sort", ["xs"], copy_list("a", "xs") + [
            Let("n", "len(a)", "int"),
            For("i", "0", "n", [
                Let("swapped", "0", "int"),
                For("j", "0", "n - i - 1", [
                    If([("a[j] > a[j + 1]", [Let("t", "a[j]"), S("a[j] = a[j + 1]"), S("a[j + 1] = t"),
                                              S("swapped = 1")])])]),
                If([("swapped == 0", [Brk()])])]),
            Ret("a")]),
        Let("data", lit(data)),
        Let("result", "bubble_sort(data)"),
        Out("data", "l"), Out("result", "l"),
        If([("result == sorted(data)", [Out('"sorted correctly"', "s")])], [Out('"wrong order"', "s")]),
    ]


def t_insertion(r):
    data = ints(r, r.randint(6, 12), 0, 500)
    return "insertion sort", [
        Def("insertion_sort", ["xs"], copy_list("a", "xs") + [
            For("i", "1", "len(a)", [
                Let("key", "a[i]"), Let("j", "i - 1"),
                While("j >= 0 and a[j] > key", [S("a[j + 1] = a[j]"), S("j -= 1")]),
                S("a[j + 1] = key")]),
            Ret("a")]),
        Let("result", f"insertion_sort({lit(data)})"),
        Out("result", "l"),
        OutF("min ", E("result[0]"), " max ", E("result[len(result) - 1]")),
    ]


def t_selection(r):
    data = ints(r, r.randint(6, 12), 1, 60)
    return "selection sort (descending)", [
        Def("selection_sort_desc", ["xs"], copy_list("a", "xs") + [
            For("i", "0", "len(a)", [
                Let("best", "i", "int"),
                For("j", "i + 1", "len(a)", [If([("a[j] > a[best]", [S("best = j")])])]),
                If([("best != i", [Let("t", "a[i]"), S("a[i] = a[best]"), S("a[best] = t")])])]),
            Ret("a")]),
        Let("result", f"selection_sort_desc({lit(data)})"),
        Out("result", "l"),
        OutF("top three: ", E("result[0]"), ", ", E("result[1]"), ", ", E("result[2]")),
    ]


def t_bsearch(r):
    data = sorted(set(ints(r, 14, 1, 120)))
    targets = [r.choice(data) for _ in range(3)] + [r.randint(1, 120) for _ in range(3)]
    return "binary search", [
        Def("bsearch", ["xs", "target"], [
            Let("lo", "0", "int"), Let("hi", "len(xs) - 1", "int"),
            While("lo <= hi", [
                Let("mid", "floor((lo + hi) / 2)"),
                If([("xs[mid] == target", [Ret("mid")]), ("xs[mid] < target", [S("lo = mid + 1")])],
                   [S("hi = mid - 1")])]),
            Ret("-1")]),
        Let("data", lit(data)),
        Each("t", lit(targets), [
            Let("pos", "bsearch(data, t)"),
            If([("pos >= 0", [OutF("found ", E("t"), " at index ", E("pos"))])],
               [OutF(E("t"), " is not in the list")])]),
    ]


def t_collatz(r):
    lo = r.randint(2, 30)
    hi = lo + r.randint(6, 15)
    return f"Collatz steps for {lo}..{hi - 1}", [
        Def("steps", ["n"], [
            Let("count", "0", "int"),
            While("n != 1", [
                If([("n % 2 == 0", [S("n = floor(n / 2)")])], [S("n = 3 * n + 1")]),
                S("count += 1")]),
            Ret("count")], sig=("int", ["int"])),
        Let("best", "0", "int"), Let("best_n", str(lo), "int"),
        For("n", str(lo), str(hi), [
            Let("s", "steps(n)"),
            OutF(E("n"), " -> ", E("s"), " steps"),
            If([("s > best", [S("best = s"), S("best_n = n")])])]),
        OutF("longest: ", E("best_n"), " with ", E("best"), " steps"),
    ]


def t_digits(r):
    nums = ints(r, r.randint(5, 8), 10, 999999)
    return "digit sums and digital roots", [
        Def("digit_sum", ["n"], [
            Let("s", "0", "int"),
            While("n > 0", [S("s += n % 10"), S("n = floor(n / 10)")]),
            Ret("s")], sig=("int", ["int"])),
        Def("digital_root", ["n"], [
            While("n >= 10", [S("n = digit_sum(n)")]),
            Ret("n")]),
        Each("n", lit(nums), [OutF(E("n"), ": sum ", E("digit_sum(n)"), ", root ", E("digital_root(n)"))]),
    ]


def t_palnum(r):
    lo = r.randint(10, 900)
    hi = lo + r.randint(60, 200)
    return f"palindromic numbers in [{lo}, {hi})", [
        Def("reverse_num", ["n"], [
            Let("rev", "0", "int"),
            While("n > 0", [S("rev = rev * 10 + n % 10"), S("n = floor(n / 10)")]),
            Ret("rev")], sig=("int", ["int"])),
        Let("found", "[]"),
        For("n", str(lo), str(hi), [If([("reverse_num(n) == n", [S("found.append(n)")])])]),
        Out("found", "l"),
        OutF("how many: ", E("len(found)")),
        OutF("reverse of ", E(str(lo)), " is ", E(f"reverse_num({lo})")),
    ]


def t_strrev(r):
    words = r.sample(WORDS, 4) + r.sample(PALS, 3)
    r.shuffle(words)
    return "string reversal and palindromes", [
        Def("reverse", ["s"], [
            Let("out", '""', "str"),
            Let("i", "len(s) - 1", "int"),
            While("i >= 0", [S("out = out + s[i]"), S("i -= 1")]),
            Ret("out")]),
        Let("count", "0", "int"),
        Each("w", lit(words), [
            Let("rw", "reverse(w)"),
            If([("rw == w", [OutF(E("w"), " is a palindrome"), S("count += 1")])],
               [OutF(E("w"), " reversed is ", E("rw"))])]),
        OutF("palindromes: ", E("count")),
    ]


def t_caesar(r):
    msgs = [" ".join(r.sample(WORDS, 3)) for _ in range(2)] + [r.choice(WORDS).upper() + " " + r.choice(WORDS)]
    k = r.randint(1, 25)
    return f"Caesar cipher with shift {k}", [
        Def("shift", ["text", "k"], [
            Let("out", '""', "str"),
            For("i", "0", "len(text)", [
                Let("ch", "text[i]"),
                Let("code", "ord(ch)"),
                If([("code >= 97 and code <= 122", [S("out = out + chr((code - 97 + k) % 26 + 97)")]),
                    ("code >= 65 and code <= 90", [S("out = out + chr((code - 65 + k) % 26 + 65)")])],
                   [S("out = out + ch")])]),
            Ret("out")]),
        Each("m", lit(msgs), [
            Let("enc", f"shift(m, {k})"),
            Let("dec", f"shift(enc, {26 - k})"),
            OutF(E("m"), " -> ", E("enc")),
            If([("dec == m", [Out('"round trip ok"', "s")])], [Out('"round trip FAILED"', "s")])]),
    ]


def t_vowels(r):
    sentences = [" ".join(r.sample(WORDS, r.randint(3, 6))) for _ in range(3)]
    return "vowel and consonant counts", [
        Def("is_vowel", ["ch"], [
            Each("v", '["a", "e", "i", "o", "u"]', [If([("ch == v", [Ret("1")])])]),
            Ret("0")]),
        Def("count_vowels", ["s"], [
            Let("n", "0", "int"),
            For("i", "0", "len(s)", [S("n += is_vowel(s[i])")]),
            Ret("n")]),
        Each("s", lit(sentences), [
            Let("v", "count_vowels(s)"),
            Let("letters", "0", "int"),
            For("i", "0", "len(s)", [If([("s[i] != \" \"", [S("letters += 1")])])]),
            OutF(E("s"), ": ", E("v"), " vowels, ", E("letters - v"), " consonants")]),
    ]


def t_wordfreq(r):
    pool = r.sample(WORDS, 5)
    text = " ".join(r.choice(pool) for _ in range(r.randint(10, 20)))
    return "word frequency count", [
        Let("text", f'"{text}"', "str"),
        Let("words", 'text.split(" ")'),
        Let("freq", "{}"), Let("order", "[]"),
        Each("w", "words", [
            If([("w in freq", [S("freq[w] += 1")])], [S("freq[w] = 1"), S("order.append(w)")])]),
        Each("w", "order", [OutF(E("w"), ": ", E("freq[w]"))]),
        Let("top", "order[0]"),
        Each("w", "order", [If([("freq[w] > freq[top]", [S("top = w")])])]),
        OutF("most common: ", E("top"), " (", E("freq[top]"), ")"),
    ]


def t_matmul(r):
    n = r.choice([2, 3])
    a = [ints(r, n, -5, 9) for _ in range(n)]
    b = [ints(r, n, -5, 9) for _ in range(n)]
    return f"{n}x{n} matrix multiplication", [
        Def("matmul", ["a", "b"], [
            Let("result", "[]"),
            For("i", "0", "len(a)", [
                Let("row", "[]"),
                For("j", "0", "len(b[0])", [
                    Let("s", "0", "int"),
                    For("t", "0", "len(b)", [S("s += a[i][t] * b[t][j]")]),
                    S("row.append(s)")]),
                S("result.append(row)")]),
            Ret("result")]),
        Let("A", "[" + ", ".join(lit(x) for x in a) + "]"),
        Let("B", "[" + ", ".join(lit(x) for x in b) + "]"),
        Let("C", "matmul(A, B)"),
        Each("row", "C", [Out("row", "l")]),
        Let("trace", "0", "int"),
        For("i", "0", "len(C)", [S("trace += C[i][i]")]),
        OutF("trace: ", E("trace")),
    ]


def t_transpose(r):
    rows, cols = r.randint(2, 4), r.randint(2, 5)
    m = [ints(r, cols, 0, 50) for _ in range(rows)]
    return f"transpose of a {rows}x{cols} matrix", [
        Def("transpose", ["m"], [
            Let("out", "[]"),
            For("j", "0", "len(m[0])", [
                Let("col", "[]"),
                For("i", "0", "len(m)", [S("col.append(m[i][j])")]),
                S("out.append(col)")]),
            Ret("out")]),
        Let("M", "[" + ", ".join(lit(x) for x in m) + "]"),
        Let("T", "transpose(M)"),
        Each("row", "T", [Out("row", "l")]),
        Each("row", "M", [OutF("row sum ", E("sum(row)"))]),
    ]


def t_stack(r):
    tests = r.sample(["(()[]{})", "([)]", "{[()()]}", "((())", "[]{}()", "(((", "{[]}[]", "}{", "([{}])()"], 5)
    return "bracket matching with a stack class", [
        Class("Stack", [], [S("self.items = []")], [
            Def("push", ["v"], [S("self.items.append(v)")]),
            Def("pop", [], [Ret("self.items.pop()")]),
            Def("peek", [], [Ret("self.items[len(self.items) - 1]")]),
            Def("size", [], [Ret("len(self.items)")])]),
        Def("balanced", ["s"], [
            Let("st", "new:Stack()"),
            Let("pairs", '{")": "(", "]": "[", "}": "{"}'),
            For("i", "0", "len(s)", [
                Let("ch", "s[i]"),
                If([('ch == "(" or ch == "[" or ch == "{"', [S("st.push(ch)")])],
                   [If([("st.size() == 0", [Ret('"no"')])]),
                    If([("st.pop() != pairs[ch]", [Ret('"no"')])])])]),
            If([("st.size() == 0", [Ret('"yes"')])]),
            Ret('"no"')]),
        Each("t", lit(tests), [OutF(E("t"), " balanced: ", E("balanced(t)"))]),
    ]


def t_queue(r):
    names = r.sample(["ann", "bob", "cid", "dee", "eve", "fay", "gus", "hal"], r.randint(4, 7))
    k = r.randint(2, 5)
    return f"round-robin elimination with a queue class (every {k})", [
        Class("Queue", [], [S("self.items = []"), S("self.head = 0")], [
            Def("enqueue", ["v"], [S("self.items.append(v)")]),
            Def("dequeue", [], [Let("v", "self.items[self.head]"), S("self.head += 1"), Ret("v")]),
            Def("size", [], [Ret("len(self.items) - self.head")])]),
        Let("q", "new:Queue()"),
        Each("n", lit(names), [S("q.enqueue(n)")]),
        Let("round", "1", "int"),
        While("q.size() > 1", [
            For("i", "0", str(k - 1), [S("q.enqueue(q.dequeue())")]),
            OutF("round ", E("round"), ": out goes ", E("q.dequeue()")),
            S("round += 1")]),
        OutF("winner: ", E("q.dequeue()")),
    ]


def t_bank(r):
    start = r.randint(50, 500)
    ops = [(r.choice(["deposit", "withdraw"]), r.randint(10, 300)) for _ in range(r.randint(5, 8))]
    return "bank account with overdraft errors", [
        Class("Account", ["owner", "balance"], [S("self.owner = owner"), S("self.balance = balance"),
                                                 S("self.log = []")], [
            Def("deposit", ["amount"], [S("self.balance += amount"), S('self.log.append("+" + str(amount))')]),
            Def("withdraw", ["amount"], [
                If([("amount > self.balance", [Raise("insufficient funds")])]),
                S("self.balance -= amount"), S('self.log.append("-" + str(amount))')]),
            Def("report", [], [Ret('self.owner + " has " + str(self.balance)')])]),
        Let("acct", f'new:Account("{r.choice(["ada", "lin", "max", "zoe"])}", {start})'),
        Let("failed", "0", "int"),
        Each("op", "[" + ", ".join(f'["{o}", {a}]' for o, a in ops) + "]", [
            Try([If([('op[0] == "deposit"', [S("acct.deposit(op[1])")])], [S("acct.withdraw(op[1])")]),
                 OutF(E("op[0]"), " ", E("op[1]"), " ok")],
                [OutF(E("op[0]"), " ", E("op[1]"), " refused"), S("failed += 1")])]),
        Out("acct.report()", "s"),
        OutF("refused operations: ", E("failed"), ", log size: ", E("len(acct.log)")),
    ]


def t_shapes(r):
    shapes = [(r.choice(["Rect", "Tri"]), r.randint(1, 12), r.randint(1, 12)) for _ in range(r.randint(3, 6))]
    base = Class("Shape", ["name", "w", "h"], [S("self.name = name"), S("self.w = w"), S("self.h = h")], [
        Def("area", [], [Ret("0")]),
        Def("describe", [], [Ret('self.name + " " + str(self.w) + "x" + str(self.h) + " area " + str(self.area())')])])
    rect = Class("Rect", None, [], [Def("area", [], [Ret("self.w * self.h")])], base="Shape")
    tri = Class("Tri", None, [], [Def("area", [], [Ret("floor(self.w * self.h / 2)")])], base="Shape")
    rect.inherited_fields = tri.inherited_fields = {"name", "w", "h"}
    items = ", ".join(f'new:{k}("{k.lower()}", {w}, {h})' for k, w, h in shapes)
    return "shape classes with inheritance across syntaxes", [
        base, rect, tri,
        Let("shapes", "[" + items + "]"),
        Let("total", "0", "int"),
        Each("s", "shapes", [Out("s.describe()", "s"), S("total += s.area()")]),
        OutF("total area: ", E("total")),
    ]


def t_counter(r):
    steps = ints(r, r.randint(5, 9), 1, 9)
    return "counter class with history", [
        Class("Counter", ["start"], [S("self.value = start"), S("self.history = [start]")], [
            Def("add", ["n"], [S("self.value += n"), S("self.history.append(self.value)"), Ret("self.value")]),
            Def("undo", [], [
                If([("len(self.history) > 1", [S("self.history.pop()"),
                                                 S("self.value = self.history[len(self.history) - 1]")])]),
                Ret("self.value")])]),
        Let("c", f"new:Counter({r.randint(0, 20)})"),
        Each("n", lit(steps), [S("c.add(n)")]),
        Out("c.history", "l"),
        OutF("after undo: ", E("c.undo()")),
        OutF("after undo: ", E("c.undo()")),
        Out("c.value", "i"),
    ]


def t_linked(r):
    vals = ints(r, r.randint(4, 8), 1, 99)
    return "singly linked list", [
        Class("Node", ["value"], [S("self.value = value"), S("self.next = NONE")], []),
        Class("LinkedList", [], [S("self.head = NONE"), S("self.size = 0")], [
            Def("push_front", ["v"], [Let("n", "new:Node(v)"), S("n.next = self.head"), S("self.head = n"),
                                      S("self.size += 1")]),
            Def("to_list", [], [
                Let("out", "[]"), Let("cur", "self.head"),
                While("cur != NONE", [S("out.append(cur.value)"), S("cur = cur.next")]),
                Ret("out")]),
            Def("reverse", [], [
                Let("prev", "NONE"), Let("cur", "self.head"),
                While("cur != NONE", [Let("nxt", "cur.next"), S("cur.next = prev"), S("prev = cur"), S("cur = nxt")]),
                S("self.head = prev")])]),
        Let("lst", "new:LinkedList()"),
        Each("v", lit(vals), [S("lst.push_front(v)")]),
        Out("lst.to_list()", "l"),
        S("lst.reverse()"),
        Out("lst.to_list()", "l"),
        OutF("size: ", E("lst.size")),
    ]


def t_bst(r):
    keys = list(dict.fromkeys(ints(r, r.randint(6, 12), 1, 100)))
    return "binary search tree", [
        Class("TreeNode", ["key"], [S("self.key = key"), S("self.left = NONE"), S("self.right = NONE")], []),
        Def("insert", ["node", "key"], [
            If([("node == NONE", [Ret("new:TreeNode(key)")])]),
            If([("key < node.key", [S("node.left = insert(node.left, key)")])],
               [S("node.right = insert(node.right, key)")]),
            Ret("node")]),
        Def("inorder", ["node", "out"], [
            If([("node != NONE", [S("inorder(node.left, out)"), S("out.append(node.key)"),
                                  S("inorder(node.right, out)")])])]),
        Def("height", ["node"], [
            If([("node == NONE", [Ret("0")])]),
            Ret("1 + max(height(node.left), height(node.right))")]),
        Let("root", "NONE"),
        Each("k", lit(keys), [S("root = insert(root, k)")]),
        Let("keys", "[]"),
        S("inorder(root, keys)"),
        Out("keys", "l"),
        OutF("height: ", E("height(root)"), ", root: ", E("root.key")),
    ]


def t_temperature(r):
    temps = ints(r, r.randint(4, 7), -30, 45)
    return "temperature conversion table", [
        Def("to_f", ["c"], [Ret("c * 9 / 5 + 32")]),
        Def("to_k", ["c"], [Ret("c + 273.15")]),
        Each("c", lit(temps), [Printf("%4d C = %7.2f F = %7.2f K", "c", "to_f(c)", "to_k(c)")]),
        Let("hot", "0", "int"),
        Each("c", lit(temps), [If([("to_f(c) > 80", [S("hot += 1")])])]),
        OutF("hot days: ", E("hot")),
    ]


def t_interest(r):
    p = r.randint(100, 5000)
    rate = r.choice([2, 3, 4, 5, 6, 7, 8])
    years = r.randint(3, 8)
    return f"compound interest at {rate}% over {years} years", [
        Let("balance", f"{p}"),
        For("y", "1", str(years + 1), [
            S(f"balance = balance * (100 + {rate}) / 100"),
            Printf("year %d: %.2f", "y", "balance")]),
        Printf("growth: %.2f", f"balance - {p}"),
    ]


def t_power(r):
    cases = [(r.randint(2, 9), r.randint(0, 30), r.choice([7, 13, 97, 1000, 1009])) for _ in range(5)]
    return "fast modular exponentiation", [
        Def("power_mod", ["b", "e", "m"], [
            Let("result", "1", "int"),
            S("b = b % m"),
            While("e > 0", [
                If([("e % 2 == 1", [S("result = result * b % m")])]),
                S("b = b * b % m"),
                S("e = floor(e / 2)")]),
            Ret("result")], sig=("int", ["int", "int", "int"])),
        Each("c", "[" + ", ".join(lit(list(x)) for x in cases) + "]", [
            OutF(E("c[0]"), "^", E("c[1]"), " mod ", E("c[2]"), " = ", E("power_mod(c[0], c[1], c[2])"))]),
    ]


def t_patterns(r):
    n = r.randint(3, 7)
    ch = r.choice(["*", "#", "+", "o"])
    return f"text patterns of height {n}", [
        Def("repeat", ["s", "k"], [
            Let("out", '""', "str"),
            For("i", "0", "k", [S("out = out + s")]),
            Ret("out")]),
        Comment("right triangle"),
        For("i", "1", str(n + 1), [Out(f'repeat("{ch}", i)', "s")]),
        Comment("pyramid"),
        For("i", "0", str(n), [Out(f'repeat(" ", {n} - i - 1) + repeat("{ch}", 2 * i + 1)', "s")]),
        Comment("inverted"),
        Let("k", str(n), "int"),
        While("k > 0", [Out(f'repeat("{ch}", k)', "s"), S("k -= 1")]),
    ]


def t_pascal(r):
    n = r.randint(5, 10)
    return f"Pascal's triangle, {n} rows", [
        Def("next_row", ["row"], [
            Let("out", "[1]"),
            For("i", "1", "len(row)", [S("out.append(row[i - 1] + row[i])")]),
            S("out.append(1)"),
            Ret("out")]),
        Let("row", "[1]"),
        For("i", "0", str(n), [Out("row", "l"), S("row = next_row(row)")]),
        OutF("sum of next row: ", E("sum(row)")),
    ]


def t_twosum(r):
    nums = list(dict.fromkeys(ints(r, 9, 1, 40)))
    targets = [nums[0] + nums[-1], nums[1] + nums[2], 999, r.randint(5, 60)]
    return "two-sum with a dictionary", [
        Def("two_sum", ["nums", "target"], [
            Let("seen", "{}"),
            For("i", "0", "len(nums)", [
                Let("need", "target - nums[i]"),
                If([("need in seen", [Ret("[seen[need], i]")])]),
                S("seen[nums[i]] = i")]),
            Ret("[-1, -1]")]),
        Let("nums", lit(nums)),
        Each("t", lit(targets), [
            Let("ans", "two_sum(nums, t)"),
            If([("ans[0] >= 0", [OutF("target ", E("t"), ": ", E("nums[ans[0]]"), " + ", E("nums[ans[1]]"))])],
               [OutF("target ", E("t"), ": no pair")])]),
    ]


def t_kadane(r):
    data = ints(r, r.randint(8, 14), -15, 15)
    return "maximum subarray (Kadane)", [
        Def("max_subarray", ["xs"], [
            Let("best", "xs[0]"), Let("cur", "0", "int"), Let("start", "0", "int"),
            Let("best_lo", "0", "int"), Let("best_hi", "0", "int"),
            For("i", "0", "len(xs)", [
                If([("cur <= 0", [S("cur = xs[i]"), S("start = i")])], [S("cur += xs[i]")]),
                If([("cur > best", [S("best = cur"), S("best_lo = start"), S("best_hi = i")])])]),
            Ret("[best, best_lo, best_hi]")]),
        Let("data", lit(data)),
        Let("res", "max_subarray(data)"),
        OutF("best sum ", E("res[0]"), " from index ", E("res[1]"), " to ", E("res[2]")),
        Let("part", "[]"),
        For("i", "res[1]", "res[2] + 1", [S("part.append(data[i])")]),
        Out("part", "l"),
    ]


def t_average(r):
    data = ints(r, r.randint(5, 10), 1, 100)
    return "running average", [
        Let("total", "0", "int"),
        Let("count", "0", "int"),
        Each("x", lit(data), [
            S("total += x"), S("count += 1"),
            Printf("after %d values (last %d): average %.2f", "count", "x", "total / count")]),
    ]


def t_histogram(r):
    data = ints(r, r.randint(10, 20), 0, 49)
    return "histogram of values in buckets of ten", [
        Let("buckets", "[0, 0, 0, 0, 0]"),
        Each("x", lit(data), [S("buckets[floor(x / 10)] += 1")]),
        For("b", "0", "5", [
            Let("bar", '""', "str"),
            For("i", "0", "buckets[b]", [S('bar = bar + "#"')]),
            OutF(E("b * 10"), "-", E("b * 10 + 9"), ": ", E("bar"))]),
        OutF("values: ", E("sum(buckets)")),
    ]


def t_anagram(r):
    pairs = [("listen", "silent"), ("evil", "vile"), ("night", "thing"), ("apple", "paper"),
             ("dusty", "study"), ("state", "taste"), ("lemon", "melon"), ("tiger", "grit")]
    chosen = r.sample(pairs, 4)
    return "anagram check with letter counts", [
        Def("counts", ["s"], [
            Let("c", "[]"),
            For("i", "0", "26", [S("c.append(0)")]),
            For("i", "0", "len(s)", [S("c[ord(s[i]) - 97] += 1")]),
            Ret("c")]),
        Def("is_anagram", ["a", "b"], [
            Let("ca", "counts(a)"), Let("cb", "counts(b)"),
            For("i", "0", "26", [If([("ca[i] != cb[i]", [Ret('"no"')])])]),
            Ret('"yes"')]),
        Each("p", "[" + ", ".join(lit(list(p)) for p in chosen) + "]", [
            OutF(E("p[0]"), " / ", E("p[1]"), ": ", E("is_anagram(p[0], p[1])"))]),
    ]


def t_roman(r):
    nums = ints(r, 6, 1, 3999)
    return "Roman numerals", [
        Def("to_roman", ["n"], [
            Let("vals", "[1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1]"),
            Let("syms", '["M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"]'),
            Let("out", '""', "str"),
            For("i", "0", "len(vals)", [While("n >= vals[i]", [S("out = out + syms[i]"), S("n -= vals[i]")])]),
            Ret("out")]),
        Each("n", lit(nums), [OutF(E("n"), " = ", E("to_roman(n)"))]),
    ]


def t_binary(r):
    nums = ints(r, 6, 0, 4096)
    return "binary conversion and bit counts", [
        Def("to_bin", ["n"], [
            If([("n == 0", [Ret('"0"')])]),
            Let("s", '""', "str"),
            While("n > 0", [S("s = str(n % 2) + s"), S("n = floor(n / 2)")]),
            Ret("s")]),
        Def("popcount", ["n"], [
            Let("c", "0", "int"),
            While("n > 0", [S("c += n & 1"), S("n = n >> 1")]),
            Ret("c")], sig=("int", ["int"])),
        Each("n", lit(nums), [OutF(E("n"), " = 0b", E("to_bin(n)"), " (", E("popcount(n)"), " ones)")]),
    ]


def t_perfect(r):
    hi = r.randint(30, 600)
    return f"perfect, abundant and deficient numbers below {hi}", [
        Def("divisor_sum", ["n"], [
            Let("s", "0", "int"),
            For("d", "1", "n", [If([("n % d == 0", [S("s += d")])])]),
            Ret("s")], sig=("int", ["int"])),
        Let("perfect", "[]"), Let("abundant", "0", "int"), Let("deficient", "0", "int"),
        For("n", "2", str(hi), [
            Let("s", "divisor_sum(n)"),
            If([("s == n", [S("perfect.append(n)")]), ("s > n", [S("abundant += 1")])], [S("deficient += 1")])]),
        Out("perfect", "l"),
        OutF("abundant: ", E("abundant"), ", deficient: ", E("deficient")),
    ]


def t_armstrong(r):
    hi = r.choice([200, 500, 1000, 2000])
    return f"Armstrong numbers below {hi}", [
        Def("is_armstrong", ["n"], [
            Let("digits", "len(str(n))"),
            Let("s", "0", "int"), Let("m", "n", "int"),
            While("m > 0", [S("s += (m % 10) ** digits"), S("m = floor(m / 10)")]),
            Ret("s == n")]),
        Let("found", "[]"),
        For("n", "1", str(hi), [If([("is_armstrong(n)", [S("found.append(n)")])])]),
        Out("found", "l"),
    ]


def t_hanoi(r):
    n = r.randint(2, 5)
    return f"Towers of Hanoi with {n} disks", [
        Def("hanoi", ["n", "src", "dst", "via", "moves"], [
            If([("n == 0", [Ret()])]),
            S("hanoi(n - 1, src, via, dst, moves)"),
            S('moves.append(src + "->" + dst)'),
            S("hanoi(n - 1, via, dst, src, moves)")]),
        Let("moves", "[]"),
        S(f'hanoi({n}, "A", "C", "B", moves)'),
        OutF("moves: ", E("len(moves)")),
        Each("m", "moves", [Out("m", "s")]),
    ]


def t_queens(r):
    n = r.randint(4, 6)
    return f"N-queens solutions for n = 4..{n}", [
        Def("safe", ["cols", "row", "col"], [
            For("r", "0", "row", [
                Let("c", "cols[r]"),
                If([("c == col or abs(c - col) == row - r", [Ret("0")])])]),
            Ret("1")]),
        Def("solve", ["cols", "row", "n"], [
            If([("row == n", [Ret("1")])]),
            Let("count", "0", "int"),
            For("col", "0", "n", [
                If([("safe(cols, row, col) == 1", [
                    S("cols[row] = col"),
                    S("count += solve(cols, row + 1, n)")])])]),
            Ret("count")]),
        For("n", "4", str(n + 1), [
            Let("cols", "[]"),
            For("i", "0", "n", [S("cols.append(0)")]),
            OutF(E("n"), " queens: ", E("solve(cols, 0, n)"), " solutions")]),
    ]


def t_stats(r):
    data = ints(r, r.choice([5, 7, 9, 11]), 1, 100)
    return "basic statistics", [
        Let("data", lit(data)),
        Let("s", "sorted(data)"),
        OutF("min ", E("min(data)"), ", max ", E("max(data)"), ", sum ", E("sum(data)")),
        OutF("median ", E("s[floor(len(s) / 2)]")),
        Printf("mean %.2f", "sum(data) / len(data)"),
        Let("var", "0"),
        Each("x", "data", [S("var += (x - sum(data) / len(data)) ** 2")]),
        Printf("variance %.2f, std dev %.2f", "var / len(data)", "sqrt(var / len(data))"),
    ]


def t_dedupe(r):
    data = [r.randint(1, 12) for _ in range(r.randint(10, 18))]
    return "removing duplicates while keeping order", [
        Def("dedupe", ["xs"], [
            Let("seen", "{}"), Let("out", "[]"),
            Each("x", "xs", [If([("not (x in seen)", [S("seen[x] = 1"), S("out.append(x)")])])]),
            Ret("out")]),
        Let("data", lit(data)),
        Let("unique", "dedupe(data)"),
        Out("unique", "l"),
        OutF("removed ", E("len(data) - len(unique)"), " duplicates"),
    ]


def t_merge(r):
    a = sorted(ints(r, r.randint(3, 7), 0, 50))
    b = sorted(ints(r, r.randint(3, 7), 0, 50))
    return "merging two sorted lists", [
        Def("merge", ["a", "b"], [
            Let("out", "[]"), Let("i", "0", "int"), Let("j", "0", "int"),
            While("i < len(a) and j < len(b)", [
                If([("a[i] <= b[j]", [S("out.append(a[i])"), S("i += 1")])], [S("out.append(b[j])"), S("j += 1")])]),
            While("i < len(a)", [S("out.append(a[i])"), S("i += 1")]),
            While("j < len(b)", [S("out.append(b[j])"), S("j += 1")]),
            Ret("out")]),
        Let("merged", f"merge({lit(a)}, {lit(b)})"),
        Out("merged", "l"),
        OutF("length ", E("len(merged)")),
    ]


def t_rle(r):
    strs = ["".join(r.choice("abc") * r.randint(1, 4) for _ in range(r.randint(3, 6))) for _ in range(4)]
    return "run-length encoding", [
        Def("rle", ["s"], [
            Let("out", '""', "str"), Let("i", "0", "int"),
            While("i < len(s)", [
                Let("j", "i"),
                While("j < len(s) and s[j] == s[i]", [S("j += 1")]),
                S("out = out + s[i] + str(j - i)"),
                S("i = j")]),
            Ret("out")]),
        Def("unrle", ["s"], [
            Let("out", '""', "str"),
            For("k", "0", "floor(len(s) / 2)", [
                For("t", "0", "int(s[2 * k + 1])", [S("out = out + s[2 * k]")])]),
            Ret("out")]),
        Each("s", lit(strs), [
            Let("enc", "rle(s)"),
            OutF(E("s"), " -> ", E("enc")),
            If([("unrle(enc) != s", [Out('"decode mismatch"', "s")])])]),
    ]


def t_inventory(r):
    items = r.sample(["bolts", "nuts", "gears", "springs", "chains", "valves", "pipes"], 4)
    ops = [(r.choice(items), r.randint(-5, 12)) for _ in range(r.randint(6, 10))]
    return "inventory updates in a dictionary", [
        Let("stock", "{" + ", ".join(f'"{i}": {r.randint(0, 10)}' for i in items) + "}"),
        Each("op", "[" + ", ".join(f'["{i}", {d}]' for i, d in ops) + "]", [
            Let("name", "op[0]"), Let("delta", "op[1]"),
            If([("stock[name] + delta < 0", [OutF("cannot take ", E("0 - delta"), " ", E("name"))])],
               [S("stock[name] += delta")])]),
        Let("total", "0", "int"),
        Each("k", "stock.keys()", [OutF(E("k"), ": ", E("stock[k]")), S("total += stock[k]")]),
        OutF("total items: ", E("total")),
    ]


def t_grades(r):
    scores = ints(r, r.randint(5, 9), 35, 100)
    names = r.sample(["ali", "bea", "cam", "dan", "eli", "fin", "gia", "hen", "ivy"], len(scores))
    return "grade classification", [
        Def("grade", ["s"], [
            If([("s >= 90", [Ret('"A"')]), ("s >= 80", [Ret('"B"')]), ("s >= 70", [Ret('"C"')]),
                ("s >= 60", [Ret('"D"')])], [Ret('"F"')])]),
        Let("names", lit(names)), Let("scores", lit(scores)),
        Let("tally", '{"A": 0, "B": 0, "C": 0, "D": 0, "F": 0}'),
        For("i", "0", "len(names)", [
            Let("g", "grade(scores[i])"),
            S("tally[g] += 1"),
            OutF(E("names[i]"), ": ", E("scores[i]"), " -> ", E("g"))]),
        Each("g", '["A", "B", "C", "D", "F"]', [OutF(E("g"), ": ", E("tally[g]"))]),
    ]


def t_leap(r):
    years = [r.randint(1600, 2400) for _ in range(5)] + [1900, 2000]
    return "leap years", [
        Def("is_leap", ["y"], [
            If([("y % 400 == 0", [Ret("1")]), ("y % 100 == 0", [Ret("0")]), ("y % 4 == 0", [Ret("1")])]),
            Ret("0")]),
        Each("y", lit(years), [
            If([("is_leap(y) == 1", [OutF(E("y"), " is a leap year")])], [OutF(E("y"), " is not a leap year")])]),
        Let("count", "0", "int"),
        For("y", "2000", "2101", [S("count += is_leap(y)")]),
        OutF("leap years 2000-2100: ", E("count")),
    ]


def t_safediv(r):
    pairs = [(r.randint(0, 100), r.choice([0, r.randint(1, 9), r.randint(1, 9)])) for _ in range(6)]
    return "exceptions from a safe division helper", [
        Def("safe_div", ["a", "b"], [
            If([("b == 0", [Raise("division by zero")])]),
            Ret("floor(a / b)")]),
        Let("errors", "0", "int"),
        Each("p", "[" + ", ".join(lit(list(p)) for p in pairs) + "]", [
            Try([OutF(E("p[0]"), " / ", E("p[1]"), " = ", E("safe_div(p[0], p[1])"))],
                [OutF(E("p[0]"), " / ", E("p[1]"), " failed"), S("errors += 1")])]),
        OutF("errors: ", E("errors")),
    ]


def t_higher(r):
    data = ints(r, r.randint(6, 10), 1, 30)
    k = r.randint(2, 5)
    return "map and filter in many styles", [
        Let("data", lit(data)),
        MapL("scaled", "data", "x", f"x * {k}"),
        FilterL("odd", "data", "x", "x % 2 == 1"),
        MapL("squares", "odd", "v", "v * v"),
        FilterL("big", "scaled", "y", "y > 20 and y < 100"),
        Out("scaled", "l"), Out("odd", "l"), Out("squares", "l"), Out("big", "l"),
        OutF("sum of squares: ", E("sum(squares)")),
    ]


def t_closure(r):
    adds = ints(r, 3, 1, 20)
    return "functions returning functions", [
        Def("make_adder", ["n"], [
            Def("add", ["x"], [Ret("x + n")]),
            Ret("add")]),
        Def("apply_twice", ["f", "x"], [Ret("f(f(x))")]),
        Each("k", lit(adds), [
            Let("f", "make_adder(k)"),
            OutF("add ", E("k"), ": ", E("f(10)"), ", twice: ", E("apply_twice(f, 10)"))]),
    ]


def t_graph(r):
    nodes = ["a", "b", "c", "d", "e", "f", "g"][:r.randint(5, 7)]
    edges = set()
    for i in range(1, len(nodes)):
        edges.add((nodes[r.randint(0, i - 1)], nodes[i]))
    for _ in range(r.randint(1, 3)):
        x, y = r.sample(nodes, 2)
        edges.add((x, y))
    edges = sorted(edges)
    return "breadth-first search on a graph", [
        Let("nodes", lit(nodes)),
        Let("adj", "{}"),
        Each("n", "nodes", [S("adj[n] = []")]),
        Each("e", "[" + ", ".join(lit(list(e)) for e in edges) + "]", [
            S("adj[e[0]].append(e[1])"), S("adj[e[1]].append(e[0])")]),
        Let("dist", "{}"),
        S(f'dist["{nodes[0]}"] = 0'),
        Let("queue", f'["{nodes[0]}"]'),
        Let("head", "0", "int"),
        While("head < len(queue)", [
            Let("cur", "queue[head]"),
            S("head += 1"),
            Each("nb", "adj[cur]", [
                If([("not (nb in dist)", [S("dist[nb] = dist[cur] + 1"), S("queue.append(nb)")])])])]),
        Each("n", "queue", [OutF(E("n"), ": distance ", E("dist[n]"))]),
        OutF("visited ", E("len(queue)"), " of ", E("len(nodes)")),
    ]


def t_charclass(r):
    samples = ["Hello World 42", "Quantum2026 Rocks!", "abc DEF 123 xyz", "MiXeD sYnTaX 7", "e = mc^2"]
    chosen = r.sample(samples, 3)
    return "character classes in strings", [
        Def("classify", ["s"], [
            Let("up", "0", "int"), Let("low", "0", "int"), Let("dig", "0", "int"), Let("other", "0", "int"),
            For("i", "0", "len(s)", [
                Let("c", "ord(s[i])"),
                If([("c >= 65 and c <= 90", [S("up += 1")]), ("c >= 97 and c <= 122", [S("low += 1")]),
                    ("c >= 48 and c <= 57", [S("dig += 1")])], [S("other += 1")])]),
            Ret("[up, low, dig, other]")]),
        Each("s", lit(chosen), [
            Let("c", "classify(s)"),
            OutF(E("s"), ": upper ", E("c[0]"), ", lower ", E("c[1]"), ", digits ", E("c[2]"), ", other ", E("c[3]"))]),
    ]


def t_triangle(r):
    tris = [tuple(ints(r, 3, 1, 12)) for _ in range(6)]
    return "triangle classification", [
        Def("kind", ["a", "b", "c"], [
            If([("a + b <= c or a + c <= b or b + c <= a", [Ret('"not a triangle"')]),
                ("a == b and b == c", [Ret('"equilateral"')]),
                ("a == b or b == c or a == c", [Ret('"isosceles"')])], [Ret('"scalene"')])]),
        Each("t", "[" + ", ".join(lit(list(t)) for t in tris) + "]", [
            OutF(E("t[0]"), ", ", E("t[1]"), ", ", E("t[2]"), ": ", E("kind(t[0], t[1], t[2])"))]),
    ]


def t_matrix_search(r):
    n = r.randint(3, 5)
    grid = [ints(r, n, 0, 9) for _ in range(n)]
    return f"searching a {n}x{n} grid", [
        Let("grid", "[" + ", ".join(lit(x) for x in grid) + "]"),
        Let("best", "-1"), Let("br", "0", "int"), Let("bc", "0", "int"),
        Let("zeros", "0", "int"),
        For("i", "0", "len(grid)", [
            For("j", "0", "len(grid[i])", [
                If([("grid[i][j] > best", [S("best = grid[i][j]"), S("br = i"), S("bc = j")])]),
                If([("grid[i][j] == 0", [S("zeros += 1")])])])]),
        OutF("largest ", E("best"), " at row ", E("br"), ", column ", E("bc")),
        OutF("zeros: ", E("zeros")),
        Let("diag", "0", "int"),
        For("i", "0", "len(grid)", [S("diag += grid[i][i] + grid[i][len(grid) - 1 - i]")]),
        OutF("diagonal sum: ", E("diag")),
    ]


def t_student(r):
    names = r.sample(["Ayla", "Bram", "Cleo", "Dev", "Esme", "Faro", "Gwen"], 4)
    return "students and courses (classes holding lists)", [
        Class("Student", ["name"], [S("self.name = name"), S("self.marks = []")], [
            Def("add_mark", ["m"], [S("self.marks.append(m)")]),
            Def("average", [], [
                If([("len(self.marks) == 0", [Ret("0")])]),
                Ret("floor(sum(self.marks) / len(self.marks))")]),
            Def("best", [], [Ret("max(self.marks)")])]),
        Let("roster", "[]"),
        Each("n", lit(names), [
            Let("s", "new:Student(n)"),
            For("k", "0", str(r.randint(2, 5)), [S(f"s.add_mark((len(n) * 7 + k * {r.randint(3, 17)}) % 41 + 60)")]),
            S("roster.append(s)")]),
        Let("top", "roster[0]"),
        Each("s", "roster", [
            OutF(E("s.name"), ": average ", E("s.average()"), ", best ", E("s.best()")),
            If([("s.average() > top.average()", [S("top = s")])])]),
        OutF("top student: ", E("top.name")),
    ]


THEMES = [t_fizzbuzz, t_factorial, t_fibonacci, t_gcd, t_sieve, t_bubble, t_insertion, t_selection,
          t_bsearch, t_collatz, t_digits, t_palnum, t_strrev, t_caesar, t_vowels, t_wordfreq, t_matmul,
          t_transpose, t_stack, t_queue, t_bank, t_shapes, t_counter, t_linked, t_bst, t_temperature,
          t_interest, t_power, t_patterns, t_pascal, t_twosum, t_kadane, t_average, t_histogram,
          t_anagram, t_roman, t_binary, t_perfect, t_armstrong, t_hanoi, t_queens, t_stats, t_dedupe,
          t_merge, t_rle, t_inventory, t_grades, t_leap, t_safediv, t_higher, t_closure, t_graph,
          t_charclass, t_triangle, t_matrix_search, t_student]


# ─── generation ──────────────────────────────────────────────────────────────

def make_program(index, seed):
    theme = THEMES[index % len(THEMES)]
    r = random.Random(seed * 100003 + index)
    title, nodes = theme(r)
    for attempt in range(40):
        q = Quantum(random.Random(seed * 7919 + index * 31 + attempt))
        body = q.lines(nodes, 0, False)
        if len(q.used) >= 4:
            break
    used = ", ".join(STYLE_NAMES[s] for s in sorted(q.used) if s in STYLE_NAMES)
    header = [f"// Mixed-syntax example {index + 1:03d}: {title}",
              f"# Syntax used: {used}"]
    slug = theme.__name__[2:]
    return f"{index + 1:03d}_{slug}.sa", "\n".join(header + [""] + body) + "\n", render_python(nodes)


def expected_output(py_src):
    res = subprocess.run([sys.executable, "-I", "-c", py_src], capture_output=True, text=True, timeout=60)
    if res.returncode != 0:
        raise RuntimeError("the Python rendering failed:\n" + res.stderr + "\n" + py_src)
    return res.stdout


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(here, "..", ".."))
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=500)
    ap.add_argument("--seed", type=int, default=2026)
    ap.add_argument("--out", default=os.path.join(root, "examples", "mixed_syntax"))
    ap.add_argument("--check", nargs="?", const=os.path.join(root, "build", "qrun.exe"), default=None,
                    help="run each file with this qrun and compare with CPython")
    ap.add_argument("--only", type=int, default=None, help="generate/check one example (1-based)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    programs = []
    seen = set()
    for i in range(args.count):
        if args.only is not None and i + 1 != args.only:
            continue
        name, sa, py = make_program(i, args.seed)
        if sa in seen:
            raise SystemExit(f"duplicate program at {name}")
        seen.add(sa)
        with open(os.path.join(args.out, name), "w", newline="\n", encoding="utf-8") as f:
            f.write(sa)
        programs.append((name, py))
    print(f"wrote {len(programs)} files to {args.out}")
    if not args.check:
        return

    def check(item):
        name, py = item
        want = expected_output(py)
        try:
            res = subprocess.run([args.check, os.path.join(args.out, name)], capture_output=True,
                                 timeout=60, stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            return name, want, "", "timed out after 60s"
        got = res.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
        if res.returncode != 0 or got != want:
            return name, want, got, res.stderr.decode("utf-8", "replace")
        return None

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        failures = [f for f in pool.map(check, programs) if f]
    for name, want, got, err in failures:
        print(f"FAIL {name}")
        wl, gl = want.splitlines(), got.splitlines()
        for k in range(max(len(wl), len(gl))):
            a = wl[k] if k < len(wl) else "<missing>"
            b = gl[k] if k < len(gl) else "<missing>"
            if a != b:
                print(f"  line {k + 1}: expected {a!r}\n  {'':>{len(str(k + 1)) + 6}}got      {b!r}")
                break
        if err.strip():
            print("  stderr: " + err.strip().splitlines()[-1][:300])
    print(f"{len(programs) - len(failures)}/{len(programs)} match CPython")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
