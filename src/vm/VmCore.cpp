#include "Vm.h"
#include "Error.h"
#include <iostream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <functional>
#include <regex>
#include <random>
#include <chrono>
#include <thread>
#include <limits>
#include <cassert>
#include <unordered_set>

// #define DEBUG_TRACE_EXECUTION

#include "Disassembler.h"
#include <iomanip>

// Defined in main.cpp — true during --test runs so input() returns ""

// immediately instead of blocking on stdin.
extern bool g_testMode;

// Defined in Compiler.cpp — maps QuantumNative* chunk-holders to their Chunk

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

// ─── Iterator state tag stored inside a QuantumNative ────────────────────────
// We encode iterators as a QuantumNative whose fn() never gets called;
// the VM identifies them by name prefix "__iter__" and stores an IterState
// keyed by raw pointer.

// ─── Constructor ─────────────────────────────────────────────────────────────

VM::VM()
{
    globals = std::make_shared<Environment>();
    registerNatives();
}

// ─── Run ─────────────────────────────────────────────────────────────────────

void VM::run(std::shared_ptr<Chunk> chunk)
{
    stepCount_ = 0;
    pendingInstances_.clear();
    stack_.clear();
    if (stack_.capacity() < 65536)
        stack_.reserve(65536);
    frames_.clear();
    handlers_.clear();

    // Create a top-level closure and push it to stack as a dummy callee
    auto closure = std::make_shared<Closure>(chunk);
    push(QuantumValue(closure));
    frames_.push_back({closure, 0, 1}); // locals start at stack index 1
    runFrame(0);
}

// ─── Stack helpers ────────────────────────────────────────────────────────────

void VM::push(QuantumValue v)
{
    stack_.push_back(std::move(v));
}

QuantumValue VM::pop()
{
    if (stack_.empty())
        throw RuntimeError("VM stack underflow");
    QuantumValue v = std::move(stack_.back());
    stack_.pop_back();
    return v;
}

QuantumValue &VM::peek(int offset)
{
    return stack_[stack_.size() - 1 - offset];
}

void VM::runtimeError(const std::string &msg, int line)
{
    throw RuntimeError(msg, line);
}

double VM::toNumber(const QuantumValue &v, const std::string &ctx, int line)
{
    if (v.isNumber())
        return v.asNumber();
    if (v.isString())
    {
        try
        {
            return std::stod(v.asString());
        }
        catch (...)
        {
        }
    }
    throw TypeError("Expected number in " + ctx + ", got " + v.typeName(), line);
}

// ─── Value equality ───────────────────────────────────────────────────────────

bool VM::valuesEqual(const QuantumValue &a, const QuantumValue &b)
{
    if (a.isNil() && b.isNil())
        return true;
    if (a.isBool() && b.isBool())
        return a.asBool() == b.asBool();
    if (a.isNumber() && b.isNumber())
        return a.asNumber() == b.asNumber();
    if (a.isString() && b.isString())
        return a.asString() == b.asString();
    if (a.isArray() && b.isArray())
        return a.asArray() == b.asArray(); // ptr eq
    return false;
}

// ─── Binary / unary execution ────────────────────────────────────────────────

QuantumValue VM::execBinary(Op op, const QuantumValue &L, const QuantumValue &R, int line)
{
    // Operator overloading: dispatch to instance magic methods (__add__,
    // __lt__, ...) when the left operand is a class instance that defines one.
    // C++ operator methods (operator+, operator<, ...) are parsed to these names.
    if (L.isInstance())
    {
        const char *magic = nullptr;
        switch (op)
        {
        case Op::ADD:    magic = "__add__";    break;
        case Op::SUB:    magic = "__sub__";    break;
        case Op::MUL:    magic = "__mul__";    break;
        case Op::DIV:    magic = "__div__";    break;
        case Op::MOD:    magic = "__mod__";    break;
        case Op::LT:     magic = "__lt__";     break;
        case Op::GT:     magic = "__gt__";     break;
        case Op::LTE:    magic = "__le__";     break;
        case Op::GTE:    magic = "__ge__";     break;
        case Op::EQ:     magic = "__eq__";     break;
        case Op::NEQ:    magic = "__ne__";     break;
        case Op::LSHIFT: magic = "__lshift__"; break;
        case Op::RSHIFT: magic = "__rshift__"; break;
        default:
            break;
        }
        if (magic)
        {
            auto inst = L.asInstance();
            for (auto *k = inst->klass.get(); k; k = k->base.get())
            {
                auto mit = k->methods.find(magic);
                if (mit == k->methods.end())
                    continue;
                auto bm = std::make_shared<QuantumBoundMethod>();
                bm->method = mit->second;
                bm->self = L;
                push(QuantumValue(bm));
                push(L); // self
                push(R); // other operand
                callClosure(mit->second, 2, line);
                runFrame(frames_.size() - 1);
                return pop();
            }
        }
    }

    // String concatenation
    if (op == Op::ADD && (L.isString() || R.isString()))
        return QuantumValue(L.toString() + R.toString());

    // Array concatenation
    if (op == Op::ADD && L.isArray() && R.isArray())
    {
        auto arr = std::make_shared<Array>(*L.asArray());
        for (auto &v : *R.asArray())
            arr->push_back(v);
        return QuantumValue(arr);
    }

    // Comparison operators — allow mixed types
    if (op == Op::EQ)
        return QuantumValue(valuesEqual(L, R));
    if (op == Op::NEQ)
        return QuantumValue(!valuesEqual(L, R));

    // Numeric arithmetic
    double l = 0, r = 0;
    bool hasNum = L.isNumber() || R.isNumber();

    if (L.isNumber())
        l = L.asNumber();
    else if (L.isString())
    {
        try
        {
            l = std::stod(L.asString());
        }
        catch (...)
        {
            l = 0;
        }
    }
    else if (L.isBool())
        l = L.asBool() ? 1.0 : 0.0;

    if (R.isNumber())
        r = R.asNumber();
    else if (R.isString())
    {
        try
        {
            r = std::stod(R.asString());
        }
        catch (...)
        {
            r = 0;
        }
    }
    else if (R.isBool())
        r = R.asBool() ? 1.0 : 0.0;

    switch (op)
    {
    case Op::ADD:
        return QuantumValue(l + r);
    case Op::SUB:
        return QuantumValue(l - r);
    case Op::MUL:
        // String repeat: "abc" * 3
        if (L.isString() && R.isNumber())
        {
            std::string s;
            for (int i = 0; i < (int)r; i++)
                s += L.asString();
            return QuantumValue(s);
        }
        if (L.isArray() && R.isNumber())
        {
            auto out = std::make_shared<Array>();
            int count = std::max(0, static_cast<int>(r));
            for (int i = 0; i < count; ++i)
                out->insert(out->end(), L.asArray()->begin(), L.asArray()->end());
            return QuantumValue(out);
        }
        if (L.isNumber() && R.isArray())
        {
            auto out = std::make_shared<Array>();
            int count = std::max(0, static_cast<int>(l));
            for (int i = 0; i < count; ++i)
                out->insert(out->end(), R.asArray()->begin(), R.asArray()->end());
            return QuantumValue(out);
        }
        return QuantumValue(l * r);
    case Op::DIV:
        if (r == 0)
            throw RuntimeError("Division by zero", line);
        return QuantumValue(l / r);
    case Op::MOD:
        if (r == 0)
            throw RuntimeError("Modulo by zero", line);
        return QuantumValue(std::fmod(l, r));
    case Op::FLOOR_DIV:
        if (r == 0)
            throw RuntimeError("Division by zero", line);
        return QuantumValue(std::floor(l / r));
    case Op::POW:
        return QuantumValue(std::pow(l, r));
    case Op::LT:
        return QuantumValue(l < r);
    case Op::LTE:
        return QuantumValue(l <= r);
    case Op::GT:
        return QuantumValue(l > r);
    case Op::GTE:
        return QuantumValue(l >= r);
    case Op::BIT_AND:
        return QuantumValue((double)((long long)l & (long long)r));
    case Op::BIT_OR:
        return QuantumValue((double)((long long)l | (long long)r));
    case Op::BIT_XOR:
        return QuantumValue((double)((long long)l ^ (long long)r));
    case Op::LSHIFT:
        return QuantumValue((double)((long long)l << (int)r));
    case Op::RSHIFT:
        return QuantumValue((double)((long long)l >> (int)r));
    default:
        throw RuntimeError("Unknown binary op", line);
    }
    (void)hasNum;
}

QuantumValue VM::execUnary(Op op, const QuantumValue &v, int line)
{
    switch (op)
    {
    case Op::NEG:
        if (v.isNumber())
            return QuantumValue(-v.asNumber());
        throw TypeError("Unary - on " + v.typeName(), line);
    case Op::NOT:
        return QuantumValue(!v.isTruthy());
    case Op::BIT_NOT:
        if (v.isNumber())
            return QuantumValue((double)(~(long long)v.asNumber()));
        throw TypeError("Bitwise ~ on " + v.typeName(), line);
    default:
        throw RuntimeError("Unknown unary op", line);
    }
}

// ─── Upvalue management ───────────────────────────────────────────────────────

std::shared_ptr<Upvalue> VM::captureUpvalue(size_t stackIdx)
{
    // Check if we already have an open upvalue for this slot
    for (auto &uv : openUpvalues_)
        if (uv->cell.get() == &stack_[stackIdx])
            return uv;

    // Create a new open upvalue pointing directly into the stack
    // We use a shared_ptr alias to avoid copying
    auto cell = std::shared_ptr<QuantumValue>(
        std::shared_ptr<QuantumValue>(), &stack_[stackIdx]);
    auto uv = std::make_shared<Upvalue>(cell);
    openUpvalues_.push_back(uv);
    return uv;
}

void VM::closeUpvalues(size_t fromIdx)
{
    for (auto it = openUpvalues_.begin(); it != openUpvalues_.end();)
    {
        auto &uv = *it;
        // If the cell points into stack at or above fromIdx, close it
        if (uv->cell.get() >= &stack_[fromIdx])
        {
            uv->closed = *uv->cell;
            uv->cell = std::shared_ptr<QuantumValue>(
                std::shared_ptr<QuantumValue>(), &uv->closed);
            it = openUpvalues_.erase(it);
        }
        else
            ++it;
    }
}

// ─── Call helpers ─────────────────────────────────────────────────────────────

void VM::callValue(QuantumValue callee, int argCount, int line)
{
    // C++ dialect tolerance: properties like .size/.length yield a number
    // directly, so "v.size()" ends up calling that number — treat a zero-arg
    // call on a number as the number itself.
    if (callee.isNumber() && argCount == 0)
    {
        pop(); // the callee
        push(callee);
        return;
    }
    if (callee.isDict())
    {
        auto dict = callee.asDict();
        auto it = dict->find("__call__");
        if (it != dict->end())
        {
            size_t calleeIndex = stack_.size() - argCount - 1;
            stack_[calleeIndex] = it->second;
            callValue(it->second, argCount, line);
            return;
        }
    }
    if (callee.isNative())
    {
        callNativeFn(callee.asNative(), argCount, line);
        return;
    }
    if (callee.isClass())
    {
        callClass(callee.asClass(), argCount, line);
        return;
    }
    if (callee.isFunction())
    {
        callClosure(callee.asFunction(), argCount, line);
        return;
    }
    if (callee.isBoundMethod())
    {
        auto bm = callee.asBoundMethod();
        size_t calleeIndex = stack_.size() - argCount - 1;
        stack_.insert(stack_.begin() + calleeIndex + 1, bm->self);
        callClosure(bm->method, argCount + 1, line);
        return;
    }
    throw TypeError("Cannot call value of type " + callee.typeName(), line);
}

void VM::callClosure(std::shared_ptr<Closure> closure, int argCount, int line)
{
    auto &ch = *closure->chunk;

    while (argCount < (int)ch.params.size())
    {
        // Fill missing args with nil (default arg logic simplified)
        push(QuantumValue());
        argCount++;
    }

    size_t stackBase = stack_.size() - argCount;
    frames_.push_back({closure, 0, stackBase});
}

void VM::callNativeFn(std::shared_ptr<QuantumNative> fn, int argCount, int line)
{
    std::vector<QuantumValue> args;
    args.reserve(argCount);
    for (int i = 0; i < argCount; ++i)
        args.push_back(stack_[stack_.size() - argCount + i]);

    for (int i = 0; i < argCount; ++i)
        stack_.pop_back();

    QuantumValue result;
    try
    {
        result = fn->fn(args);
    }
    catch (QuantumError &)
    {
        throw;
    }
    catch (std::exception &e)
    {
        throw RuntimeError(e.what(), line);
    }

    push(std::move(result));
}

void VM::callClass(std::shared_ptr<QuantumClass> klass, int argCount, int line)
{
    auto inst = std::make_shared<QuantumInstance>();
    inst->klass = klass;
    inst->env = std::make_shared<Environment>(globals);

    // Look for __init__ / init / constructor
    auto *k = klass.get();
    std::shared_ptr<Closure> initFn;
    while (k)
    {
        for (const char *initName : {"__init__", "init", "constructor"})
        {
            auto it = k->methods.find(initName);
            if (it != k->methods.end())
            {
                initFn = it->second;
                break;
            }
        }
        if (initFn)
            break;
        k = k->base.get();
    }

    QuantumValue instVal(inst);

    if (initFn)
    {
        size_t calleeIndex = stack_.size() - argCount - 1;
        stack_.insert(stack_.begin() + calleeIndex + 1, instVal);
        pendingInstances_.push_back({instVal, frames_.size()});
        callClosure(initFn, argCount + 1, line);
        return;
    }

    size_t calleeIndex = stack_.size() - argCount - 1;
    stack_[calleeIndex] = instVal;
    for (int i = 0; i < argCount; ++i)
        stack_.pop_back();
}

// ─── Built-in method dispatch ─────────────────────────────────────────────────

QuantumValue VM::callBuiltinMethod(QuantumValue &obj, const std::string &method,
                                   std::vector<QuantumValue> args, int line)
{
    if (obj.isNumber())
    {
        if (method == "zero")
            return QuantumValue(obj.asNumber() == 0.0);
        if (method == "even")
            return QuantumValue(std::fmod(obj.asNumber(), 2.0) == 0.0);
        if (method == "odd")
            return QuantumValue(std::fmod(obj.asNumber(), 2.0) != 0.0);
        if (method == "positive")
            return QuantumValue(obj.asNumber() > 0.0);
        if (method == "negative")
            return QuantumValue(obj.asNumber() < 0.0);
        if (method == "abs")
            return QuantumValue(std::fabs(obj.asNumber()));
        if (method == "to_i" || method == "floor")
            return QuantumValue(std::floor(obj.asNumber()));
        if (method == "ceil")
            return QuantumValue(std::ceil(obj.asNumber()));
        if (method == "round")
            return QuantumValue(std::round(obj.asNumber()));
        if (method == "to_f")
            return QuantumValue(obj.asNumber());
        if (method == "to_s")
            return QuantumValue(obj.toString());
        // Ruby Integer#times / #upto — iterate by delegating to the array
        // higher-order dispatch, which already handles every callable kind.
        if (method == "times" || method == "upto" || method == "downto")
        {
            auto range = std::make_shared<Array>();
            if (method == "times")
                for (int i = 0; i < (int)obj.asNumber(); ++i)
                    range->push_back(QuantumValue((double)i));
            else if (method == "upto")
            {
                int stop = args.empty() ? (int)obj.asNumber() : (int)args[0].asNumber();
                for (int i = (int)obj.asNumber(); i <= stop; ++i)
                    range->push_back(QuantumValue((double)i));
            }
            else
            {
                int stop = args.empty() ? 0 : (int)args[0].asNumber();
                for (int i = (int)obj.asNumber(); i >= stop; --i)
                    range->push_back(QuantumValue((double)i));
            }
            std::vector<QuantumValue> cbArgs;
            for (auto &a : args)
                if (a.isFunction() || a.isNative() || a.isBoundMethod())
                    cbArgs.push_back(a);
            if (cbArgs.empty())
                return QuantumValue(range);
            return callArrayMethod(range, "each", cbArgs);
        }
        if (method == "toFixed")
        {
            int places = args.empty() ? 0 : static_cast<int>(args[0].asNumber());
            if (places < 0)
                places = 0;
            std::ostringstream out;
            out << std::fixed << std::setprecision(places) << obj.asNumber();
            return QuantumValue(out.str());
        }
        if (method == "toString")
            return QuantumValue(obj.toString());
    }
    if (obj.isNative())
    {
        auto native = obj.asNative();
        if (native->name == "str" && method == "maketrans")
        {
            auto table = std::make_shared<Dict>();
            if (args.size() >= 2)
            {
                std::string from = args[0].toString();
                std::string to = args[1].toString();
                size_t count = std::min(from.size(), to.size());
                for (size_t i = 0; i < count; ++i)
                    (*table)[std::to_string((int)(unsigned char)from[i])] = QuantumValue(std::string(1, to[i]));
            }
            return QuantumValue(table);
        }
        if (method == "then" || method == "catch" || method == "json")
        {
            if (native->fn)
                return native->fn(args);
            return method == "json" ? QuantumValue(std::make_shared<Dict>()) : obj;
        }
        if (method == "receive_email" || method == "list_emails" ||
            method == "read_email" || method == "delete_email")
            return QuantumValue();
    }
    if (obj.isArray())
        return callArrayMethod(obj.asArray(), method, args);
    if (obj.isString())
        return callStringMethod(obj.asString(), method, args);
    if (obj.isDict())
        return callDictMethod(obj.asDict(), method, args);
    if (obj.isInstance())
    {
        auto inst = obj.asInstance();
        // Find method in class hierarchy
        auto *k = inst->klass.get();
        std::vector<std::string> lookupNames{method};
        if (method == "toString")
            lookupNames.push_back("__str__");
        else if (method == "__str__")
            lookupNames.push_back("toString");
        while (k)
        {
            std::shared_ptr<Closure> fn;
            for (const auto &lookup : lookupNames)
            {
                auto it = k->methods.find(lookup);
                if (it != k->methods.end())
                {
                    fn = it->second;
                    break;
                }
            }
            if (fn)
            {
                push(QuantumValue(fn));
                push(obj);
                for (auto &a : args)
                    push(a);
                callClosure(fn, (int)args.size() + 1, line);
                size_t savedDepth = frames_.size() - 1;
                runFrame(savedDepth);
                return pop();
            }
            k = k->base.get();
        }
        // Check fields
        try
        {
            return inst->getField(method);
        }
        catch (...)
        {
        }
    }
    throw TypeError("No method '" + method + "' on " + obj.typeName(), line);
}

// ─── Main execution loop ──────────────────────────────────────────────────────

