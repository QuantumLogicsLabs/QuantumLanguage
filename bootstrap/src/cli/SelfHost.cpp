// Support for the Quantum-written compiler in src/: the C++ front-end's
// tokens and bytecode to check it against, and a runner for the bytecode
// files it writes (qrun --dump-tokens / --emit-qbc / --exec).
#include "Cli.h"
#include "Dialect.h"
#include "Error.h"
#include "Lexer.h"
#include "Parser.h"
#include "Pipeline.h"
#include "Serializer.h"
#include "Vm.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

namespace
{
bool readText(const std::string &path, std::string &out)
{
    std::ifstream in(path); // text mode, as runFile reads a program
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// A token value on one line: \ \n \t \r and other control bytes escaped.
std::string escaped(const std::string &s)
{
    std::string out;
    for (unsigned char c : s)
    {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\t')
            out += "\\t";
        else if (c == '\r')
            out += "\\r";
        else if (c < 32 || c == 127)
        {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", c);
            out += buf;
        }
        else
            out += (char)c;
    }
    return out;
}
} // namespace

int dumpTokens(const std::string &path)
{
    std::string source;
    if (!readText(path, source))
    {
        std::cerr << "[Error] Cannot open file: " << path << "\n";
        return 1;
    }
    try
    {
        Lexer lexer(source);
        for (const Token &t : lexer.tokenize())
            std::cout << static_cast<int>(t.type) << "\t" << t.line << "\t" << t.col << "\t" << escaped(t.value)
                      << "\n";
    }
    catch (const QuantumError &e)
    {
        std::cout << "ERROR\t" << e.kind << ": " << e.what() << "\n";
        return 1;
    }
    return 0;
}

namespace
{
// One node per line, children indented under `.field` labels. Field order
// and spelling follow src/ast.sa, so src/tools/astdump.sa prints
// the Quantum parser's tree the same way.
class AstDumper
{
  public:
    explicit AstDumper(std::ostream &out) : out_(out) {}

    void node(const ASTNode *n, int depth)
    {
        if (!n)
        {
            line(depth) << "nil\n";
            return;
        }
        const ASTNode &a = *n;
        if (a.is<NumberLiteral>())
            head(a, "Number", depth) << " value=" << QuantumValue(a.as<NumberLiteral>().value).toString() << "\n";
        else if (a.is<StringLiteral>())
            head(a, "String", depth) << " value=\"" << escaped(a.as<StringLiteral>().value) << "\"\n";
        else if (a.is<BoolLiteral>())
            head(a, "Bool", depth) << " value=" << flag(a.as<BoolLiteral>().value) << "\n";
        else if (a.is<NilLiteral>())
            head(a, "Nil", depth) << "\n";
        else if (a.is<Identifier>())
            head(a, "Ident", depth) << " name=" << str(a.as<Identifier>().name) << "\n";
        else if (a.is<BinaryExpr>())
        {
            auto &e = a.as<BinaryExpr>();
            head(a, "Binary", depth) << " op=" << str(e.op) << "\n";
            child("left", e.left.get(), depth);
            child("right", e.right.get(), depth);
        }
        else if (a.is<UnaryExpr>())
        {
            auto &e = a.as<UnaryExpr>();
            head(a, "Unary", depth) << " op=" << str(e.op) << "\n";
            child("operand", e.operand.get(), depth);
        }
        else if (a.is<AssignExpr>())
        {
            auto &e = a.as<AssignExpr>();
            head(a, "Assign", depth) << " op=" << str(e.op) << "\n";
            child("target", e.target.get(), depth);
            child("value", e.value.get(), depth);
        }
        else if (a.is<CallExpr>())
        {
            auto &e = a.as<CallExpr>();
            head(a, "Call", depth) << "\n";
            child("callee", e.callee.get(), depth);
            list("args", e.args, depth);
        }
        else if (a.is<IndexExpr>())
        {
            auto &e = a.as<IndexExpr>();
            head(a, "Index", depth) << "\n";
            child("object", e.object.get(), depth);
            child("index", e.index.get(), depth);
        }
        else if (a.is<SliceExpr>())
        {
            auto &e = a.as<SliceExpr>();
            head(a, "Slice", depth) << "\n";
            child("object", e.object.get(), depth);
            child("start", e.start.get(), depth);
            child("stop", e.stop.get(), depth);
            child("step", e.step.get(), depth);
        }
        else if (a.is<MemberExpr>())
        {
            auto &e = a.as<MemberExpr>();
            head(a, "Member", depth) << " member=" << str(e.member) << "\n";
            child("object", e.object.get(), depth);
        }
        else if (a.is<ArrayLiteral>())
        {
            head(a, "Array", depth) << "\n";
            list("elements", a.as<ArrayLiteral>().elements, depth);
        }
        else if (a.is<DictLiteral>())
        {
            auto &e = a.as<DictLiteral>();
            head(a, "Dict", depth) << "\n";
            line(depth + 1) << ".pairs[" << e.pairs.size() << "]\n";
            for (auto &p : e.pairs)
            {
                child("key", p.first.get(), depth + 1);
                child("value", p.second.get(), depth + 1);
            }
        }
        else if (a.is<LambdaExpr>())
        {
            auto &e = a.as<LambdaExpr>();
            head(a, "Lambda", depth) << " params=" << strs(e.params) << " paramTypes=" << strs(e.paramTypes)
                                     << " returnType=" << str(e.returnType) << "\n";
            list("defaultArgs", e.defaultArgs, depth);
            child("body", e.body.get(), depth);
        }
        else if (a.is<ListComp>())
        {
            auto &e = a.as<ListComp>();
            head(a, "ListComp", depth) << " vars=" << strs(e.vars) << "\n";
            child("expr", e.expr.get(), depth);
            child("iterable", e.iterable.get(), depth);
            child("condition", e.condition.get(), depth);
        }
        else if (a.is<TupleLiteral>())
        {
            head(a, "Tuple", depth) << "\n";
            list("elements", a.as<TupleLiteral>().elements, depth);
        }
        else if (a.is<VarDecl>())
        {
            auto &e = a.as<VarDecl>();
            head(a, "VarDecl", depth) << " name=" << str(e.name) << " isConst=" << flag(e.isConst)
                                      << " typeHint=" << str(e.typeHint) << " isPointer=" << flag(e.isPointer)
                                      << " isExported=" << flag(e.isExported) << " cTyped=" << flag(e.cTyped)
                                      << " perInstance=" << flag(e.perInstance) << " isStatic=" << flag(e.isStatic)
                                      << "\n";
            child("initializer", e.initializer.get(), depth);
        }
        else if (a.is<FunctionDecl>())
        {
            auto &e = a.as<FunctionDecl>();
            head(a, "FunctionDecl", depth) << " name=" << str(e.name) << " params=" << strs(e.params)
                                           << " paramTypes=" << strs(e.paramTypes) << " paramIsRef=" << flags(e.paramIsRef)
                                           << " returnType=" << str(e.returnType) << " isExported=" << flag(e.isExported)
                                           << " cppParamTypes=" << strs(e.cppParamTypes)
                                           << " cReturnType=" << str(e.cReturnType)
                                           << " pythonScope=" << flag(e.pythonScope) << "\n";
            list("defaultArgs", e.defaultArgs, depth);
            child("body", e.body.get(), depth);
        }
        else if (a.is<GlobalStmt>())
        {
            auto &e = a.as<GlobalStmt>();
            head(a, "Global", depth) << " names=" << strs(e.names) << " isNonlocal=" << flag(e.isNonlocal) << "\n";
        }
        else if (a.is<ReturnStmt>())
        {
            head(a, "Return", depth) << "\n";
            child("value", a.as<ReturnStmt>().value.get(), depth);
        }
        else if (a.is<IfStmt>())
        {
            auto &e = a.as<IfStmt>();
            head(a, "If", depth) << "\n";
            child("condition", e.condition.get(), depth);
            child("thenBranch", e.thenBranch.get(), depth);
            child("elseBranch", e.elseBranch.get(), depth);
        }
        else if (a.is<WhileStmt>())
        {
            auto &e = a.as<WhileStmt>();
            head(a, "While", depth) << "\n";
            child("condition", e.condition.get(), depth);
            child("body", e.body.get(), depth);
            child("post", e.post.get(), depth);
        }
        else if (a.is<ForStmt>())
        {
            auto &e = a.as<ForStmt>();
            head(a, "For", depth) << " var=" << str(e.var) << " var2=" << str(e.var2) << "\n";
            child("iterable", e.iterable.get(), depth);
            child("body", e.body.get(), depth);
        }
        else if (a.is<BlockStmt>())
        {
            head(a, "Block", depth) << "\n";
            list("statements", a.as<BlockStmt>().statements, depth);
        }
        else if (a.is<ExprStmt>())
        {
            head(a, "ExprStmt", depth) << "\n";
            child("expr", a.as<ExprStmt>().expr.get(), depth);
        }
        else if (a.is<PrintStmt>())
        {
            auto &e = a.as<PrintStmt>();
            head(a, "Print", depth) << " newline=" << flag(e.newline) << " sep=" << str(e.sep)
                                    << " end=" << str(e.end) << "\n";
            list("args", e.args, depth);
        }
        else if (a.is<InputStmt>())
        {
            auto &e = a.as<InputStmt>();
            head(a, "Input", depth) << " target=" << str(e.target) << "\n";
            child("prompt", e.prompt.get(), depth);
            child("lvalueTarget", e.lvalueTarget.get(), depth);
        }
        else if (a.is<BreakStmt>())
            head(a, "Break", depth) << "\n";
        else if (a.is<ContinueStmt>())
            head(a, "Continue", depth) << "\n";
        else if (a.is<SuperExpr>())
            head(a, "Super", depth) << " method=" << str(a.as<SuperExpr>().method) << "\n";
        else if (a.is<RaiseStmt>())
        {
            head(a, "Raise", depth) << "\n";
            child("value", a.as<RaiseStmt>().value.get(), depth);
        }
        else if (a.is<TryStmt>())
        {
            auto &e = a.as<TryStmt>();
            head(a, "Try", depth) << "\n";
            child("body", e.body.get(), depth);
            line(depth + 1) << ".handlers[" << e.handlers.size() << "]\n";
            for (auto &h : e.handlers)
            {
                line(depth + 2) << "Handler errorType=" << str(h.errorType) << " alias=" << str(h.alias) << "\n";
                child("body", h.body.get(), depth + 2);
            }
            child("finallyBody", e.finallyBody.get(), depth);
        }
        else if (a.is<ImportStmt>())
        {
            auto &e = a.as<ImportStmt>();
            head(a, "Import", depth) << " module=" << str(e.module) << " imports=[";
            for (size_t i = 0; i < e.imports.size(); ++i)
                out_ << (i ? ", " : "") << str(e.imports[i].name) << " as " << str(e.imports[i].alias);
            out_ << "]\n";
        }
        else if (a.is<ClassDecl>())
        {
            auto &e = a.as<ClassDecl>();
            head(a, "Class", depth) << " name=" << str(e.name) << " base=" << str(e.base) << "\n";
            list("methods", e.methods, depth);
            list("staticMethods", e.staticMethods, depth);
            list("fields", e.fields, depth);
        }
        else if (a.is<TernaryExpr>())
        {
            auto &e = a.as<TernaryExpr>();
            head(a, "Ternary", depth) << "\n";
            child("condition", e.condition.get(), depth);
            child("thenExpr", e.thenExpr.get(), depth);
            child("elseExpr", e.elseExpr.get(), depth);
        }
        else if (a.is<AddressOfExpr>())
        {
            head(a, "AddressOf", depth) << "\n";
            child("operand", a.as<AddressOfExpr>().operand.get(), depth);
        }
        else if (a.is<DerefExpr>())
        {
            head(a, "Deref", depth) << "\n";
            child("operand", a.as<DerefExpr>().operand.get(), depth);
        }
        else if (a.is<ArrowExpr>())
        {
            auto &e = a.as<ArrowExpr>();
            head(a, "Arrow", depth) << " member=" << str(e.member) << "\n";
            child("object", e.object.get(), depth);
        }
        else if (a.is<NewExpr>())
        {
            auto &e = a.as<NewExpr>();
            head(a, "New", depth) << " typeName=" << str(e.typeName) << " isArray=" << flag(e.isArray) << "\n";
            list("args", e.args, depth);
            child("sizeExpr", e.sizeExpr.get(), depth);
        }
    }

  private:
    std::ostream &out_;

    std::ostream &line(int depth)
    {
        for (int i = 0; i < depth; ++i)
            out_ << "  ";
        return out_;
    }
    std::ostream &head(const ASTNode &a, const char *kind, int depth)
    {
        return line(depth) << kind << " L" << a.line;
    }
    void child(const char *field, const ASTNode *n, int depth)
    {
        line(depth + 1) << "." << field << "\n";
        node(n, depth + 2);
    }
    void list(const char *field, const std::vector<ASTNodePtr> &v, int depth)
    {
        line(depth + 1) << "." << field << "[" << v.size() << "]\n";
        for (auto &n : v)
            node(n.get(), depth + 2);
    }
    static std::string str(const std::string &s) { return "\"" + escaped(s) + "\""; }
    static const char *flag(bool b) { return b ? "true" : "false"; }
    static std::string strs(const std::vector<std::string> &v)
    {
        std::string s = "[";
        for (size_t i = 0; i < v.size(); ++i)
            s += (i ? ", " : "") + str(v[i]);
        return s + "]";
    }
    static std::string flags(const std::vector<bool> &v)
    {
        std::string s = "[";
        for (size_t i = 0; i < v.size(); ++i)
            s += std::string(i ? ", " : "") + flag(v[i]);
        return s + "]";
    }
};
} // namespace

int dumpAst(const std::string &path)
{
    std::string source;
    if (!readText(path, source))
    {
        std::cerr << "[Error] Cannot open file: " << path << "\n";
        return 1;
    }
    try
    {
        Lexer lexer(source);
        Parser parser(lexer.tokenize());
        const std::string ext = path.size() >= 3 ? path.substr(path.size() - 3) : "";
        parser.setPythonSource(ext == ".py" || ext == ".PY");
        auto ast = parser.parse();
        AstDumper(std::cout).node(ast.get(), 0);
    }
    catch (const ParseError &e)
    {
        std::cout << "ERROR\tParseError: " << e.what() << " (line " << e.line << ")\n";
        return 1;
    }
    catch (const QuantumError &e)
    {
        std::cout << "ERROR\t" << e.kind << ": " << e.what() << " (line " << e.line << ")\n";
        return 1;
    }
    return 0;
}

int emitBytecodeFile(const std::string &path, const std::string &outPath)
{
    std::string source;
    if (!readText(path, source))
    {
        std::cerr << "[Error] Cannot open file: " << path << "\n";
        return 1;
    }
    try
    {
        auto chunk = compileSource(applyDialect(source, path), path, false);
        auto bytes = Serializer::serialize(chunk);
        std::ofstream out(outPath, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return out ? 0 : 1;
    }
    catch (const ParseError &e)
    {
        std::cerr << path << ":" << e.line << ": ParseError: " << e.what() << "\n";
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Error] " << e.what() << "\n";
        return 1;
    }
}

int execBytecodeFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        std::cerr << "[Error] Cannot open bytecode file: " << path << "\n";
        return 1;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try
    {
        auto chunk = Serializer::deserialize(bytes);
        VM vm;
        vm.run(chunk);
        return 0;
    }
    catch (const QuantumError &e)
    {
        std::cerr << Colors::RED << Colors::BOLD << "\n  X " << e.kind << Colors::RESET;
        if (e.line > 0)
            std::cerr << " at line " << e.line;
        std::cerr << "\n    " << e.what() << "\n\n";
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Fatal] " << e.what() << "\n";
        return 1;
    }
}
