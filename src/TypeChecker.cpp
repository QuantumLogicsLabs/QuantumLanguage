#include "TypeChecker.h"
#include <iostream>
#include "Error.h"

TypeChecker::TypeChecker() : globalEnv(std::make_shared<TypeEnv>()) {
    // Define built-ins
    globalEnv->define("print", "any");
    globalEnv->define("input", "string");
    globalEnv->define("len", "int");
    globalEnv->define("sha256", "string");
    globalEnv->define("aes128", "string");
}

void TypeChecker::check(const std::vector<ASTNodePtr>& nodes) {
    for (auto& node : nodes) {
        checkNode(node, globalEnv);
    }
}

void TypeChecker::check(const ASTNodePtr& node) {
    if (node && node->is<BlockStmt>()) {
        check(node->as<BlockStmt>().statements);
    } else {
        checkNode(node, globalEnv);
    }
}

// Whether a value of type `found` may initialize a variable declared `declared`.
// Numbers widen freely across the C numeric types (an integer literal is a
// fine `double`, `long`, `size_t`, ...), since the VM has one number type.
static bool typeCompatible(const std::string& declared, const std::string& found) {
    if (declared == found) return true;
    if ((declared == "str" || declared == "String") && found == "string") return true;
    static const char* numeric[] = {"int", "float", "double", "long", "short", "unsigned",
                                    "unsigned int", "long long", "unsigned long", "size_t",
                                    "char", "number"};
    bool declNum = false, foundNum = false;
    for (const char* t : numeric) {
        if (declared == t) declNum = true;
        if (found == t) foundNum = true;
    }
    return declNum && foundNum;
}

std::string TypeChecker::checkNode(const ASTNodePtr& node, std::shared_ptr<TypeEnv> env) {
    if (!node) return "void";

    if (node->is<NumberLiteral>()) {
        double v = node->as<NumberLiteral>().value;
        return (v == static_cast<long long>(v)) ? "int" : "float";
    }
    if (node->is<StringLiteral>()) return "string";
    if (node->is<BoolLiteral>()) return "bool";
    
    if (node->is<Identifier>()) {
        return env->resolve(node->as<Identifier>().name);
    }

    if (node->is<VarDecl>()) {
        auto& vd = node->as<VarDecl>();
        std::string initType = "any";
        if (vd.initializer) initType = checkNode(vd.initializer, env);
        
        std::string declaredType = vd.typeHint.empty() ? initType : vd.typeHint;
        
        // Basic type check
        if (!vd.typeHint.empty() && vd.typeHint != "any" && initType != "any" &&
            !typeCompatible(vd.typeHint, initType)) {
            std::cerr << Colors::YELLOW << "[StaticTypeWarning] " << Colors::RESET 
                      << "Type mismatch for '" << vd.name << "'. Found " << initType 
                      << " but expected " << vd.typeHint << " (line " << node->line << ")\n";
        }
        
        env->define(vd.name, declaredType);
        return "void";
    }

    if (node->is<FunctionDecl>()) {
        auto& fd = node->as<FunctionDecl>();
        auto subEnv = std::make_shared<TypeEnv>(env);
        for (size_t i = 0; i < fd.params.size(); ++i) {
            std::string pType = "any";
            if (i < fd.paramTypes.size() && !fd.paramTypes[i].empty()) pType = fd.paramTypes[i];
            subEnv->define(fd.params[i], pType);
        }
        checkNode(fd.body, subEnv);
        
        std::string retType = fd.returnType.empty() ? "any" : fd.returnType;
        env->define(fd.name, "fn"); // Simplification
        return "void";
    }

    if (node->is<BlockStmt>()) {
        auto& block = node->as<BlockStmt>();
        auto subEnv = std::make_shared<TypeEnv>(env);
        for (auto& stmt : block.statements) {
            checkNode(stmt, subEnv);
        }
        return "void";
    }

    if (node->is<BinaryExpr>()) {
        auto& be = node->as<BinaryExpr>();
        std::string left = checkNode(be.left, env);
        std::string right = checkNode(be.right, env);
        if (be.op == "+" || be.op == "-" || be.op == "*" || be.op == "/") return "float";
        if (be.op == "==" || be.op == "!=" || be.op == "<" || be.op == ">") return "bool";
        return "any";
    }

    if (node->is<CallExpr>()) {
        auto& ce = node->as<CallExpr>();
        checkNode(ce.callee, env);
        for (auto& arg : ce.args) checkNode(arg, env);
        // We could look up the function signature here if we had a more complex type system
        return "any";
    }

    if (node->is<ExprStmt>()) {
        return checkNode(node->as<ExprStmt>().expr, env);
    }

    if (node->is<ReturnStmt>()) {
        return checkNode(node->as<ReturnStmt>().value, env);
    }

    return "any";
}
