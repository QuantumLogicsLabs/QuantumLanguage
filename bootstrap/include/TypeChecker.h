#pragma once
#include "AST.h"
#include <stdexcept>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>

class StaticTypeError : public std::runtime_error
{
public:
    int line;
    StaticTypeError(const std::string &msg, int l)
        : std::runtime_error(msg), line(l) {}
};

struct TypeEnv {
    std::map<std::string, std::string> vars;
    std::shared_ptr<TypeEnv> parent;

    TypeEnv(std::shared_ptr<TypeEnv> p = nullptr) : parent(p) {}

    void define(const std::string& name, const std::string& type) {
        vars[name] = type;
    }

    std::string resolve(const std::string& name) {
        if (vars.count(name)) return vars[name];
        if (parent) return parent->resolve(name);
        return "any";
    }
};

class TypeChecker
{
public:
    TypeChecker();
    void check(const std::vector<ASTNodePtr>& nodes);
    void check(const ASTNodePtr& node);
    std::string checkNode(const ASTNodePtr& node, std::shared_ptr<TypeEnv> env);

private:
    std::shared_ptr<TypeEnv> globalEnv;
    // C/C++ enum type names (seen through their constants, `Color::Red`):
    // numbers in the VM, so an int initializes them.
    std::set<std::string> enumTypes;
};
