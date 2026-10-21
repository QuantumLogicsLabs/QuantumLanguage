#include "../include/Value.h"
#include "../include/Vm.h"
#include "../include/Error.h"
#include <sstream>
#include <cmath>
#include <iomanip>
#include <cstdint>

bool g_pythonRepr = false;

// A string element of a printed array/dict: double-quoted by default;
// under Python repr, single-quoted unless it contains a ' and no ".
static std::string quoteForDisplay(const std::string &s)
{
    if (!g_pythonRepr)
        return "\"" + s + "\"";
    bool useDouble = s.find('\'') != std::string::npos && s.find('"') == std::string::npos;
    char q = useDouble ? '"' : '\'';
    std::string out(1, q);
    for (char c : s)
    {
        if (c == '\\' || c == q)
            out += '\\';
        if (c == '\n')
            out += "\\n";
        else if (c == '\t')
            out += "\\t";
        else
            out += c;
    }
    return out + q;
}

// ─── QuantumValue ─────────────────────────────────────────────────────────────

bool QuantumValue::isTruthy() const
{
    return std::visit([](const auto &v) -> bool
                      {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, QuantumNil>)    return false;
        if constexpr (std::is_same_v<T, bool>)          return v;
        if constexpr (std::is_same_v<T, double>)        return v != 0.0;
        if constexpr (std::is_same_v<T, int64_t>)       return v != 0;
        if constexpr (std::is_same_v<T, std::string>)   return !v.empty() && !(v.size() == 1 && v[0] == '\0');
        if constexpr (std::is_same_v<T, std::shared_ptr<Array>>) return !v->empty();
        if constexpr (std::is_same_v<T, std::shared_ptr<QuantumPointer>>) return v && !v->isNull();
        return true; }, data);
}

std::string QuantumValue::toString() const
{
    return std::visit([](const auto &v) -> std::string
                      {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, QuantumNil>)  return g_pythonRepr ? "None" : "nil";
        if constexpr (std::is_same_v<T, bool>)
            return g_pythonRepr ? (v ? "True" : "False") : (v ? "true" : "false");
        if constexpr (std::is_same_v<T, double>) {
            if (std::floor(v) == v && std::abs(v) < 1e15)
                return std::to_string((long long)v);
            std::ostringstream oss;
            oss << std::setprecision(10) << v;
            return oss.str();
        }
        if constexpr (std::is_same_v<T, std::string>) return v;
        if constexpr (std::is_same_v<T, std::shared_ptr<Array>>) {
            std::string s = "[";
            for (size_t i = 0; i < v->size(); i++) {
                if (i) s += ", ";
                if ((*v)[i].isString()) s += quoteForDisplay((*v)[i].asString());
                else s += (*v)[i].toString();
            }
            return s + "]";
        }
        if constexpr (std::is_same_v<T, std::shared_ptr<Dict>>) {
            bool special = false;
            std::string shown = jsDisplayString(v, special); // Date, URL, ...
            if (special) return shown;
            std::string s = "{";
            bool first = true;
            for (auto& [k, val] : *v) {
                if (!first) s += ", ";
                s += quoteForDisplay(k) + ": ";
                if (val.isString()) s += quoteForDisplay(val.asString());
                else s += val.toString();
                first = false;
            }
            return s + "}";
        }
        if constexpr (std::is_same_v<T, std::shared_ptr<Closure>>) return "<fn:" + v->name + ">";
        if constexpr (std::is_same_v<T, std::shared_ptr<QuantumNative>>)   return "<native:" + v->name + ">";
        if constexpr (std::is_same_v<T, std::shared_ptr<QuantumInstance>>) {
            // Call __str__ if defined
            auto k = v->klass.get();
            while (k) {
                auto mit = k->methods.find("__str__");
                if (mit != k->methods.end()) {
                    break;
                }
                k = k->base.get();
            }
            return "<instance:" + v->klass->name + ">";
        }
        if constexpr (std::is_same_v<T, std::shared_ptr<QuantumClass>>)    return "<class:" + v->name + ">";
        if constexpr (std::is_same_v<T, std::shared_ptr<QuantumBoundMethod>>) return "<bound method>";
        if constexpr (std::is_same_v<T, int64_t>) return std::to_string(v);
        if constexpr (std::is_same_v<T, std::shared_ptr<QuantumPointer>>) {
            if (!v || v->isNull()) return "0x0";
            // Show a deterministic fake address based on cell pointer value
            // so repeated prints of the same pointer give the same address
            std::ostringstream oss;
            oss << "0x" << std::hex << std::uppercase
                << (reinterpret_cast<uintptr_t>(v->cell.get()) + (size_t)v->offset * 8);
            return oss.str();
        }
        return "?"; }, data);
}

std::string QuantumValue::typeName() const
{
    return std::visit([](const auto &v) -> std::string
                      {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, QuantumNil>)   return "nil";
        if constexpr (std::is_same_v<T, bool>)         return "bool";
        if constexpr (std::is_same_v<T, double>)       return "number";
        if constexpr (std::is_same_v<T, std::string>)  return "string";
        if constexpr (std::is_same_v<T, std::shared_ptr<Array>>)           return "array";
        if constexpr (std::is_same_v<T, std::shared_ptr<Dict>>)            return "dict";
        if constexpr (std::is_same_v<T, std::shared_ptr<Closure>>)         return "function";
        if constexpr (std::is_same_v<T, std::shared_ptr<QuantumNative>>)   return "native";
        if constexpr (std::is_same_v<T, std::shared_ptr<QuantumInstance>>) return v->klass->name;
        if constexpr (std::is_same_v<T, std::shared_ptr<QuantumClass>>)    return "class";
        if constexpr (std::is_same_v<T, std::shared_ptr<QuantumBoundMethod>>) return "method";
        if constexpr (std::is_same_v<T, std::shared_ptr<QuantumPointer>>)  return "pointer";
        if constexpr (std::is_same_v<T, int64_t>)      return "long";
        return "unknown"; }, data);
}

// ─── Environment ─────────────────────────────────────────────────────────────

Environment::Environment(std::shared_ptr<Environment> p) : parent(std::move(p)) {}

void Environment::define(const std::string &name, QuantumValue val, bool isConst)
{
    vars[name] = std::move(val);
    if (isConst)
        constants[name] = true;
}

void Environment::defineRef(const std::string &name, std::shared_ptr<QuantumValue> cell)
{
    // Bind name directly to the shared cell — reads/writes go through it automatically
    cells[name] = cell;
    vars[name] = *cell; // keep vars in sync for iteration (e.g. getVars())
}

QuantumValue Environment::get(const std::string &name) const
{
    // Check cells first: if a pointer has written through &var, cells holds the live value
    auto cit = cells.find(name);
    if (cit != cells.end())
        return *cit->second;
    auto it = vars.find(name);
    if (it != vars.end())
        return it->second;
    if (parent)
        return parent->get(name);
    throw NameError("Undefined variable: '" + name + "'");
}

void Environment::set(const std::string &name, QuantumValue val)
{
    auto it = vars.find(name);
    if (it != vars.end())
    {
        if (constants.count(name))
            throw RuntimeError("Cannot reassign constant '" + name + "'");
        it->second = val; // update local vars map
        // Sync to any live shared cell (covers both pointer and ref cases)
        auto cit = cells.find(name);
        if (cit != cells.end())
            *cit->second = val;
        return;
    }
    if (parent)
    {
        parent->set(name, std::move(val));
        return;
    }
    throw NameError("Undefined variable: '" + name + "'");
}

bool Environment::has(const std::string &name) const
{
    if (vars.count(name))
        return true;
    if (parent)
        return parent->has(name);
    return false;
}

std::shared_ptr<QuantumValue> Environment::getCell(const std::string &name)
{
    // Look for existing cell in this scope
    auto cit = cells.find(name);
    if (cit != cells.end())
        return cit->second;

    // Look for the variable in this scope
    auto it = vars.find(name);
    if (it != vars.end())
    {
        // Create a shared cell synced to the current value
        auto cell = std::make_shared<QuantumValue>(it->second);
        cells[name] = cell;
        return cell;
    }

    // Walk parent scopes
    if (parent)
        return parent->getCell(name);
    return nullptr;
}

// ─── QuantumInstance ─────────────────────────────────────────────────────────

QuantumValue QuantumInstance::getField(const std::string &name) const
{
    auto it = fields.find(name);
    if (it != fields.end())
        return it->second;
    // Check methods
    auto k = klass.get();
    while (k)
    {
        auto mit = k->methods.find(name);
        if (mit != k->methods.end())
            return QuantumValue(mit->second);
        k = k->base.get();
    }
    throw NameError("No field/method '" + name + "' on instance of " + klass->name);
}

void QuantumInstance::setField(const std::string &name, QuantumValue val)
{
    fields[name] = std::move(val);
}

bool QuantumValue::isNative() const
{
    return std::holds_alternative<std::shared_ptr<QuantumNative>>(data);
}

std::shared_ptr<QuantumNative> QuantumValue::asNative() const
{
    if (!isNative())
        throw RuntimeError("Value is not a native function");
    return std::get<std::shared_ptr<QuantumNative>>(data);
}