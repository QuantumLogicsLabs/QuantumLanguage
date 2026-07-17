#include "Vm.h"
#include "Error.h"
#include "Disassembler.h"
#include <iostream>
#include <string>
#include <unordered_set>

void VM::runFrame(size_t stopDepth)
{
    // Outer retry loop: a QuantumError thrown by any opcode (division by
    // zero, calling nil, bad index, ...) is routed to the innermost active
    // try/catch handler exactly like an explicit RAISE would be. Without a
    // handler at or above stopDepth the error propagates to the caller.
    while (frames_.size() > stopDepth)
    {
        try
        {
    while (frames_.size() > stopDepth)
    {
        CallFrame &frame = frames_.back();
        auto &code = frame.closure->chunk->code;
        auto &consts = frame.closure->chunk->constants;

        if (frame.ip >= code.size())
        {
            // Function fell off the end
            size_t base = frame.stackBase;
            frames_.pop_back();
            // Trim stack back to base, push nil
            while (stack_.size() > base)
                stack_.pop_back();
            push(QuantumValue());
            continue;
        }

        if (++stepCount_ > MAX_STEPS)
            throw RuntimeError("Execution step limit exceeded (possible infinite loop)");

        const Instruction &instr = code[frame.ip++];
        int line = instr.line;

#ifdef DEBUG_TRACE_EXECUTION
        std::cout << "          ";
        for (const auto &v : stack_)
        {
            std::cout << "[ ";
            if (v.isString())
                std::cout << "\"" << v.asString() << "\"";
            else if (v.isNative())
                std::cout << "<native " << v.asNative()->name << ">";
            else if (v.isFunction())
                std::cout << "<fn>";
            else
                std::cout << v.toString();
            std::cout << " ]";
        }
        std::cout << "\n";
        disassembleInstruction(*frame.closure->chunk, frame.ip - 1, std::cout);
        std::cout << "\n";
#endif

        switch (instr.op)
        {
        // ── Constants & stack ──────────────────────────────────────────────
        case Op::LOAD_CONST:
            push(consts[instr.operand]);
            break;
        case Op::LOAD_NIL:
            push(QuantumValue());
            break;
        case Op::LOAD_TRUE:
            push(QuantumValue(true));
            break;
        case Op::LOAD_FALSE:
            push(QuantumValue(false));
            break;
        case Op::POP:
            pop();
            break;
        case Op::DUP:
            push(peek(0));
            break;
        case Op::SWAP:
        {
            QuantumValue a = pop(), b = pop();
            push(a);
            push(b);
            break;
        }
        case Op::NOP:
            break;

        // ── Globals ───────────────────────────────────────────────────────
        case Op::DEFINE_GLOBAL:
        {
            const std::string &name = consts[instr.operand].asString();
            globals->define(name, pop());
            break;
        }
        case Op::DEFINE_CONST:
        {
            const std::string &name = consts[instr.operand].asString();
            globals->define(name, pop(), true);
            break;
        }
        case Op::LOAD_GLOBAL:
        {
            const std::string &name = consts[instr.operand].asString();
            try
            {
                push(globals->get(name));
            }
            catch (NameError &)
            {
                // Implicit self: only inside a method (params[0] == "self"),
                // fall back to reading the field from the instance at slot 0.
                bool found = false;
                auto &params = frame.closure->chunk->params;
                if (!params.empty() && params[0] == "self" &&
                    frame.stackBase < stack_.size())
                {
                    QuantumValue &slot0 = stack_[frame.stackBase];
                    if (slot0.isInstance())
                    {
                        auto inst = slot0.asInstance();
                        auto fit = inst->fields.find(name);
                        if (fit != inst->fields.end())
                        {
                            push(fit->second);
                            found = true;
                        }
                        else
                        {
                            // Check class methods in hierarchy
                            auto *k = inst->klass.get();
                            while (k && !found)
                            {
                                auto mit = k->methods.find(name);
                                if (mit != k->methods.end())
                                {
                                    auto bm = std::make_shared<QuantumBoundMethod>();
                                    bm->method = mit->second;
                                    bm->self = slot0;
                                    push(QuantumValue(bm));
                                    found = true;
                                }
                                else
                                {
                                    auto sit = k->staticFields.find(name);
                                    if (sit != k->staticFields.end())
                                    {
                                        push(sit->second);
                                        found = true;
                                    }
                                }
                                if (!found)
                                    k = k->base.get();
                            }
                        }
                    }
                }
                if (!found)
                    push(QuantumValue()); // return nil for missing
            }
            break;
        }
        case Op::STORE_GLOBAL:
        {
            const std::string &name = consts[instr.operand].asString();
            // Implicit self: only inside a method (params[0] == "self"),
            // and only when the name is not already a known global.
            auto &params = frame.closure->chunk->params;
            bool inMethod = !params.empty() && params[0] == "self" &&
                            frame.stackBase < stack_.size() &&
                            stack_[frame.stackBase].isInstance();
            if (inMethod && !globals->has(name))
                stack_[frame.stackBase].asInstance()->setField(name, peek(0));
            else if (globals->has(name))
                globals->set(name, peek(0));
            else
                globals->define(name, peek(0));
            break;
        }

        // ── Locals ────────────────────────────────────────────────────────
        case Op::DEFINE_LOCAL:
            // Value already on stack; slot is stack_[stackBase + operand]
            // Nothing to do — the value stays at its position.
            break;
        case Op::LOAD_LOCAL:
        {
            size_t idx = frame.stackBase + instr.operand;
            if (idx < stack_.size())
                push(stack_[idx]);
            else
                push(QuantumValue());
            break;
        }
        case Op::STORE_LOCAL:
        {
            size_t idx = frame.stackBase + instr.operand;
            while (stack_.size() <= idx)
                stack_.push_back(QuantumValue());
            stack_[idx] = peek(0);
            break;
        }

        // ── Upvalues ──────────────────────────────────────────────────────
        case Op::LOAD_UPVALUE:
        {
            auto &uv = frame.closure->upvalues[instr.operand];
            push(uv->get());
            break;
        }
        case Op::STORE_UPVALUE:
        {
            auto &uv = frame.closure->upvalues[instr.operand];
            uv->set(peek(0));
            break;
        }
        case Op::CLOSE_UPVALUE:
        {
            closeUpvalues(stack_.size() - 1);
            pop();
            break;
        }

        // ── Arithmetic ────────────────────────────────────────────────────
        case Op::ADD:
        case Op::SUB:
        case Op::MUL:
        case Op::DIV:
        case Op::MOD:
        case Op::FLOOR_DIV:
        case Op::POW:
        case Op::BIT_AND:
        case Op::BIT_OR:
        case Op::BIT_XOR:
        case Op::LSHIFT:
        case Op::RSHIFT:
        case Op::EQ:
        case Op::NEQ:
        case Op::LT:
        case Op::LTE:
        case Op::GT:
        case Op::GTE:
        {
            QuantumValue R = pop(), L = pop();
            push(execBinary(instr.op, L, R, line));
            break;
        }

        case Op::NEG:
        case Op::NOT:
        case Op::BIT_NOT:
        {
            QuantumValue v = pop();
            push(execUnary(instr.op, v, line));
            break;
        }

        // ── Control flow ──────────────────────────────────────────────────
        case Op::JUMP:
            frame.ip += instr.operand;
            break;
        case Op::JUMP_IF_FALSE:
            if (!peek(0).isTruthy())
                frame.ip += instr.operand;
            break;
        case Op::JUMP_IF_TRUE:
            if (peek(0).isTruthy())
                frame.ip += instr.operand;
            break;
        case Op::LOOP:
            frame.ip -= instr.operand;
            break;
        case Op::JUMP_ABSOLUTE:
            frame.ip = instr.operand;
            break;

        // ── Functions ─────────────────────────────────────────────────────
        case Op::MAKE_FUNCTION:
        case Op::MAKE_CLOSURE:
        {
            QuantumValue top = pop();
            if (!top.isFunction())
                throw RuntimeError("Expected closure template", line);

            auto tpl = top.asFunction();
            auto closure = std::make_shared<Closure>(tpl->chunk);

            // Capture upvalues if MAKE_CLOSURE
            if (instr.op == Op::MAKE_CLOSURE && closure->chunk->upvalueCount > 0)
            {
                auto &constants = closure->chunk->constants;
                // Last constant is the upvalue descriptor array
                if (!constants.empty() && constants.back().isArray())
                {
                    auto &descs = *constants.back().asArray();
                    for (auto &desc : descs)
                    {
                        if (!desc.isArray())
                            continue;
                        auto &d = *desc.asArray();
                        bool isLocal = d[0].asNumber() != 0.0;
                        int idx2 = (int)d[1].asNumber();
                        if (isLocal)
                            closure->upvalues.push_back(
                                captureUpvalue(frame.stackBase + idx2));
                        else if (idx2 < (int)frame.closure->upvalues.size())
                            closure->upvalues.push_back(
                                frame.closure->upvalues[idx2]);
                    }
                }
            }

            push(QuantumValue(closure));
            break;
        }

        case Op::CALL:
        {
            int argCount = instr.operand;
            QuantumValue callee = stack_[stack_.size() - argCount - 1];

            // C++ dialect tolerance: properties like .size/.length yield a
            // number directly, so "v.size()" ends up calling that number —
            // a zero-arg call on a number is the number itself.
            if (callee.isNumber() && argCount == 0)
                break; // callee stays on the stack as the result

            if (callee.isNative())
            {
                std::vector<QuantumValue> args;
                for (int i = 0; i < argCount; ++i)
                    args.push_back(stack_[stack_.size() - argCount + i]);
                for (int i = 0; i <= argCount; ++i)
                    stack_.pop_back(); // pop args and callee
                try
                {
                    push(callee.asNative()->fn(args));
                }
                catch (QuantumError &)
                {
                    throw;
                }
                catch (std::exception &e)
                {
                    throw RuntimeError(e.what(), line);
                }
                break;
            }

            if (callee.isDict())
            {
                auto dict = callee.asDict();
                auto it = dict->find("__call__");
                if (it != dict->end())
                {
                    stack_[stack_.size() - argCount - 1] = it->second;
                    frame.ip--;
                    break;
                }
            }

            if (callee.isClass())
            {
                auto klass = callee.asClass();
                auto inst = std::make_shared<QuantumInstance>();
                inst->klass = klass;
                inst->env = std::make_shared<Environment>(globals);
                QuantumValue instVal(inst);

                auto *k = klass.get();
                bool initFound = false;
                while (k && !initFound)
                {
                    for (const char *initName : {"__init__", "init", "constructor"})
                    {
                        auto it = k->methods.find(initName);
                        if (it != k->methods.end())
                        {
                            size_t calleeIndex = stack_.size() - argCount - 1;
                            stack_.insert(stack_.begin() + calleeIndex + 1, instVal);
                            pendingInstances_.push_back({instVal, frames_.size()});
                            callClosure(it->second, argCount + 1, line);
                            initFound = true;
                            break;
                        }
                    }
                    if (!initFound)
                        k = k->base.get();
                }

                if (!initFound)
                {
                    size_t calleeIndex = stack_.size() - argCount - 1;
                    stack_[calleeIndex] = instVal;
                    for (int i = 0; i < argCount; ++i)
                        stack_.pop_back();
                }
                break;
            }

            if (callee.isInstance())
            {
                auto inst = callee.asInstance();
                try
                {
                    auto callMethod = inst->getField("__call__");
                    stack_[stack_.size() - argCount - 1] = callMethod;
                    frame.ip--; // re-execute CALL
                    break;
                }
                catch (...)
                {
                }
            }

            if (callee.isBoundMethod())
            {
                auto bm = callee.asBoundMethod();
                size_t calleeIndex = stack_.size() - argCount - 1;
                stack_.insert(stack_.begin() + calleeIndex + 1, bm->self);
                callClosure(bm->method, argCount + 1, line);
                break;
            }

            if (callee.isFunction())
            {
                callClosure(callee.asFunction(), argCount, line);
                break;
            }

            throw TypeError("Cannot call value of type " + callee.typeName(), line);
        }

        case Op::RETURN:
        {
            QuantumValue result = pop();
            size_t base = frame.stackBase;
            closeUpvalues(base);
            frames_.pop_back();
            // Trim stack back to base - 1 to remove the callee slot
            while (stack_.size() > base - 1)
                stack_.pop_back();
            push(std::move(result));
            break;
        }
        case Op::RETURN_NIL:
        {
            size_t base = frame.stackBase;
            closeUpvalues(base);
            frames_.pop_back();
            // Trim stack back to base - 1 to remove the callee slot
            while (stack_.size() > base - 1)
                stack_.pop_back();
            if (!pendingInstances_.empty() &&
                frames_.size() == pendingInstances_.back().second)
            {
                push(pendingInstances_.back().first);
                pendingInstances_.pop_back();
            }
            else
                push(QuantumValue());
            break;
        }

        // ── Collections ───────────────────────────────────────────────────
        case Op::MAKE_ARRAY:
        {
            int n = instr.operand;
            auto arr = std::make_shared<Array>(n);
            for (int i = n - 1; i >= 0; --i)
                (*arr)[i] = pop();
            push(QuantumValue(arr));
            break;
        }
        case Op::MAKE_DICT:
        {
            int n = instr.operand; // number of pairs
            auto dict = std::make_shared<Dict>();
            // Stack has n*2 values: k0,v0,k1,v1,...
            std::vector<std::pair<QuantumValue, QuantumValue>> pairs(n);
            for (int i = n - 1; i >= 0; --i)
            {
                pairs[i].second = pop();
                pairs[i].first = pop();
            }
            for (auto &[k, v] : pairs)
                (*dict)[k.toString()] = v;
            push(QuantumValue(dict));
            break;
        }
        case Op::MAKE_TUPLE:
        {
            int n = instr.operand;
            auto arr = std::make_shared<Array>(n);
            for (int i = n - 1; i >= 0; --i)
                (*arr)[i] = pop();
            push(QuantumValue(arr)); // tuples stored as arrays
            break;
        }

        // ── Index / member access ─────────────────────────────────────────
        case Op::GET_INDEX:
        {
            QuantumValue idx = pop();
            QuantumValue obj = pop();

            if (obj.isArray())
            {
                auto &arr = *obj.asArray();
                int i = (int)toNumber(idx, "index", line);
                if (i < 0)
                    i += (int)arr.size();
                if (i < 0 || i >= (int)arr.size())
                    push(QuantumValue());
                else
                    push(arr[i]);
            }
            else if (obj.isString())
            {
                const std::string &s = obj.asString();
                int i = (int)toNumber(idx, "index", line);
                if (i < 0)
                    i += (int)s.size();
                if (i < 0 || i >= (int)s.size())
                    push(QuantumValue());
                else
                    push(QuantumValue(std::string(1, s[i])));
            }
            else if (obj.isDict())
            {
                auto &d = *obj.asDict();
                auto it = d.find(idx.toString());
                push(it != d.end() ? it->second : QuantumValue());
            }
            else
                throw TypeError("Cannot index into " + obj.typeName(), line);
            break;
        }

        case Op::SET_INDEX:
        {
            // Stack: ... value, obj, key  (key on top)
            QuantumValue key = pop();
            QuantumValue obj = pop();
            QuantumValue val = pop();

            if (obj.isArray())
            {
                int i = (int)toNumber(key, "index", line);
                auto &arr = *obj.asArray();
                if (i < 0)
                    i += (int)arr.size();
                if (i < 0 || i >= (int)arr.size())
                    throw IndexError("Array index out of range", line);
                arr[i] = val;
            }
            else if (obj.isDict())
                (*obj.asDict())[key.toString()] = val;
            else
                throw TypeError("Cannot index-assign " + obj.typeName(), line);

            push(val); // assignment is an expression
            break;
        }

        case Op::GET_MEMBER:
        {
            const std::string &name = consts[instr.operand].asString();
            QuantumValue obj = pop();

            if (obj.isNil())
            {
                static const std::unordered_set<std::string> nilChainMethods = {
                    "map", "filter", "reduce", "forEach", "flatMap",
                    "join", "split", "has", "add"};
                if (nilChainMethods.count(name))
                {
                    auto native = std::make_shared<QuantumNative>();
                    native->name = "__nil_chain__" + name;
                    native->fn = [](std::vector<QuantumValue>) -> QuantumValue
                    {
                        return QuantumValue();
                    };
                    push(QuantumValue(native));
                }
                else
                {
                    push(QuantumValue());
                }
                break;
            }

            if (obj.isInstance())
            {
                auto inst = obj.asInstance();
                // 1. Instance fields
                auto fit = inst->fields.find(name);
                if (fit != inst->fields.end())
                {
                    push(fit->second);
                    break;
                }

                // 2. Class methods: return a BoundMethod
                auto *k = inst->klass.get();
                bool found = false;
                while (k && !found)
                {
                    auto mit = k->methods.find(name);
                    if (mit != k->methods.end())
                    {
                        auto bm = std::make_shared<QuantumBoundMethod>();
                        bm->method = mit->second;
                        bm->self = obj;
                        push(QuantumValue(bm));
                        found = true;
                    }
                    else
                    {
                        auto sit = k->staticFields.find(name);
                        if (sit != k->staticFields.end())
                        {
                            push(sit->second);
                            found = true;
                        }
                    }
                    if (!found)
                        k = k->base.get();
                }
                if (found)
                    break;
            }

            if (obj.isClass())
            {
                auto klass = obj.asClass();
                auto cm = klass->staticFields.find("__m__" + name);
                if (cm != klass->staticFields.end())
                {
                    push(cm->second);
                    break;
                }
                auto fi = klass->staticFields.find(name);
                if (fi != klass->staticFields.end())
                {
                    push(fi->second);
                    break;
                }
            }

            // Dict: check stored keys first (enables console.log, module objects, etc.)
            if (obj.isDict())
            {
                auto &d = *obj.asDict();
                auto it = d.find(name);
                if (it != d.end())
                {
                    push(it->second);
                    break;
                }
            }

            if (name == "length" || name == "size")
            {
                if (obj.isArray())
                {
                    push(QuantumValue((double)obj.asArray()->size()));
                    break;
                }
                if (obj.isString())
                {
                    push(QuantumValue((double)obj.asString().size()));
                    break;
                }
                if (obj.isDict())
                {
                    push(QuantumValue((double)obj.asDict()->size()));
                    break;
                }
            }

            // C++ pair emulation: {a, b}.first / .second → arr[0] / arr[1]
            if (obj.isArray() && (name == "first" || name == "second"))
            {
                auto &a = *obj.asArray();
                size_t idx = (name == "first") ? 0 : 1;
                if (idx < a.size())
                {
                    push(a[idx]);
                    break;
                }
            }

            // Built-in method (array/string/dict methods)
            {
                auto native = std::make_shared<QuantumNative>();
                native->name = "__method__" + name;
                auto objCap = obj;
                auto vmCap = this;
                auto nameCap = name;
                native->fn = [vmCap, objCap, nameCap](std::vector<QuantumValue> args) mutable -> QuantumValue
                {
                    return vmCap->callBuiltinMethod(objCap, nameCap, args, 0);
                };
                push(QuantumValue(native));
                break;
            }
        }

        case Op::SET_MEMBER:
        {
            const std::string &name = consts[instr.operand].asString();
            QuantumValue val = pop();
            QuantumValue obj = peek(0);

            if (obj.isInstance())
                obj.asInstance()->setField(name, val);
            else if (obj.isClass())
                obj.asClass()->staticFields[name] = val;
            else if (obj.isDict())
                (*obj.asDict())[name] = val;
            else
                throw TypeError("Cannot set member on " + obj.typeName(), line);
            break;
        }

        // ── Iteration ─────────────────────────────────────────────────────
        case Op::MAKE_ITER:
        {
            QuantumValue iterable = pop();
            std::shared_ptr<Array> src;

            if (iterable.isArray())
                src = iterable.asArray();
            else if (iterable.isString())
            {
                src = std::make_shared<Array>();
                for (char c : iterable.asString())
                    src->push_back(QuantumValue(std::string(1, c)));
            }
            else if (iterable.isDict())
            {
                src = std::make_shared<Array>();
                for (auto &[k, v] : *iterable.asDict())
                    src->push_back(QuantumValue(k));
            }
            else
                throw TypeError("Value is not iterable: " + iterable.typeName(), line);

            // Store iterator state INSIDE the native closure — no external map needed
            auto idx = std::make_shared<size_t>(0);
            auto cap_src = src;
            auto cap_idx = idx;

            auto state = std::make_shared<QuantumNative>();
            state->name = "__iter__";
            state->fn = [cap_src, cap_idx](std::vector<QuantumValue> args) -> QuantumValue
            {
                // Called with no args: return {done, value} pair
                if (cap_idx && *cap_idx < cap_src->size())
                {
                    QuantumValue v = (*cap_src)[(*cap_idx)++];
                    return v; // return next element
                }
                return QuantumValue(); // exhausted: nil signals done
            };
            push(QuantumValue(state));
            break;
        }

        case Op::FOR_ITER:
        {
            // Peek at iterator (top of stack = iterator native)
            QuantumValue &iter = peek(0);
            if (!iter.isNative())
            {
                frame.ip += instr.operand;
                break;
            }
            auto nat = iter.asNative();
            if (nat->name != "__iter__")
            {
                frame.ip += instr.operand;
                break;
            }
            // Call fn() with no args to get next value
            QuantumValue next = nat->fn({});
            if (next.isNil())
            {
                // Exhausted — jump past loop
                frame.ip += instr.operand;
                break;
            }
            push(next); // push loop variable value
            break;
        }

        // ── Classes ───────────────────────────────────────────────────────
        case Op::MAKE_CLASS:
        {
            // Operand 0: class name was pushed as LOAD_CONST just before
            QuantumValue namePushed = pop();
            std::string className = namePushed.isString() ? namePushed.asString() : "Class";
            auto klass = std::make_shared<QuantumClass>();
            klass->name = className;
            push(QuantumValue(klass));
            break;
        }

        case Op::INHERIT:
        {
            QuantumValue base = pop();
            if (base.isClass())
                peek(0).asClass()->base = base.asClass();
            break;
        }

        case Op::BIND_METHOD:
        {
            const std::string &methodName = consts[instr.operand].asString();
            QuantumValue fn = pop();
            QuantumValue &classVal = peek(0);
            if (!classVal.isClass())
                throw RuntimeError("BIND_METHOD: top is not a class", line);

            if (fn.isFunction())
                classVal.asClass()->methods[methodName] = fn.asFunction();
            else
                classVal.asClass()->staticFields[methodName] = fn;
            break;
        }

        case Op::INSTANCE_NEW:
        {
            int argCount = instr.operand;
            QuantumValue callee = stack_[stack_.size() - argCount - 1];
            if (callee.isNative())
            {
                std::vector<QuantumValue> args;
                args.reserve(argCount);
                for (int i = 0; i < argCount; ++i)
                    args.push_back(stack_[stack_.size() - argCount + i]);
                for (int i = 0; i <= argCount; ++i)
                    stack_.pop_back();
                push(callee.asNative()->fn(args));
                break;
            }
            if (callee.isDict())
            {
                auto dict = callee.asDict();
                auto ctorIt = dict->find("__new__");
                if (ctorIt != dict->end())
                {
                    std::vector<QuantumValue> args;
                    args.reserve(argCount);
                    for (int i = 0; i < argCount; ++i)
                        args.push_back(stack_[stack_.size() - argCount + i]);
                    for (int i = 0; i <= argCount; ++i)
                        stack_.pop_back();

                    QuantumValue ctor = ctorIt->second;
                    if (ctor.isNative())
                        push(ctor.asNative()->fn(args));
                    else if (ctor.isFunction())
                    {
                        push(ctor);
                        for (auto &arg : args)
                            push(arg);
                        callClosure(ctor.asFunction(), static_cast<int>(args.size()), line);
                    }
                    else
                        throw TypeError("new: __new__ is not callable", line);
                    break;
                }
            }
            if (!callee.isClass())
                throw TypeError("new: expected class, got " + callee.typeName(), line);

            auto klass = callee.asClass();
            auto inst = std::make_shared<QuantumInstance>();
            inst->klass = klass;
            inst->env = std::make_shared<Environment>(globals);
            QuantumValue instVal(inst);

            auto *k = klass.get();
            bool initFound = false;
            while (k && !initFound)
            {
                for (const char *initName : {"__init__", "init", "constructor"})
                {
                    auto it = k->methods.find(initName);
                    if (it != k->methods.end())
                    {
                        size_t calleeIndex = stack_.size() - argCount - 1;
                        stack_.insert(stack_.begin() + calleeIndex + 1, instVal);
                        pendingInstances_.push_back({instVal, frames_.size()});
                        callClosure(it->second, argCount + 1, line);
                        initFound = true;
                        break;
                    }
                }
                if (!initFound)
                    k = k->base.get();
            }

            if (!initFound)
            {
                size_t calleeIndex = stack_.size() - argCount - 1;
                stack_[calleeIndex] = instVal;
                for (int i = 0; i < argCount; ++i)
                    stack_.pop_back();
            }
            break;
        }

        case Op::GET_SUPER:
        {
            const std::string &method = consts[instr.operand].asString();
            QuantumValue selfVal = pop();
            if (!selfVal.isInstance())
                throw RuntimeError("super: self is not an instance", line);

            auto base = selfVal.asInstance()->klass->base;
            if (!base)
                throw RuntimeError("super: class has no base class", line);

            std::string lookupMethod = method;
            if (method == "__init__")
                lookupMethod = "init";
            else if (method == "__str__" || method == "toString")
                lookupMethod = "__str__";

            auto *k = base.get();
            bool superFound = false;
            while (k && !superFound)
            {
                auto it = k->methods.find(lookupMethod);
                if (it == k->methods.end())
                    it = k->methods.find(method);
                if (it != k->methods.end())
                {
                    auto bm = std::make_shared<QuantumBoundMethod>();
                    bm->method = it->second;
                    bm->self = selfVal;
                    push(QuantumValue(bm));
                    superFound = true;
                }
                if (!superFound)
                    k = k->base.get();
            }
            if (!superFound)
                push(QuantumValue());
            break;
        }

        // ── Exceptions ────────────────────────────────────────────────────
        case Op::PUSH_HANDLER:
        {
            int32_t catchIp = static_cast<int32_t>(frame.ip) + instr.operand;
            handlers_.push_back({catchIp, frames_.size(), stack_.size()});
            break;
        }
        case Op::POP_HANDLER:
            if (!handlers_.empty())
                handlers_.pop_back();
            break;
        case Op::RAISE:
        {
            QuantumValue val = pop();
            if (handlers_.empty())
            {
                std::string msg = val.isString() ? val.asString() : val.toString();
                throw RuntimeError(msg, line);
            }
            ExceptionHandler h = handlers_.back();
            handlers_.pop_back();
            while (frames_.size() > h.frameDepth)
                frames_.pop_back();
            while (stack_.size() > h.stackDepth)
                stack_.pop_back();
            push(val);
            if (!frames_.empty())
                frames_.back().ip = h.catchIp;
            break;
        }
        case Op::RERAISE:
        {
            if (!handlers_.empty())
            {
                ExceptionHandler h = handlers_.back();
                handlers_.pop_back();
                if (!frames_.empty())
                    frames_.back().ip = h.catchIp;
            }
            break;
        }

        // ── Pointers ──────────────────────────────────────────────────────
        case Op::ADDRESS_OF:
        {
            // For named variables this creates a pointer cell;
            // simplified: wrap in QuantumPointer
            QuantumValue v = pop();
            auto cell = std::make_shared<QuantumValue>(v);
            auto ptr = std::make_shared<QuantumPointer>();
            ptr->cell = cell;
            push(QuantumValue(ptr));
            break;
        }
        case Op::DEREF:
        {
            QuantumValue v = pop();
            if (!v.isPointer())
                throw TypeError("Cannot dereference non-pointer", line);
            push(v.asPointer()->deref());
            break;
        }
        case Op::ARROW:
        {
            // ptr->member: handled by compileArrow as DEREF + GET_MEMBER
            break;
        }

        // ── Print ─────────────────────────────────────────────────────────
        case Op::PRINT:
        {
            int n = instr.operand;
            // Stack: arg0..argN-1, sep, end   (end on top)
            QuantumValue endStr = pop();
            QuantumValue sepStr = pop();
            std::string sep = sepStr.isString() ? sepStr.asString() : " ";
            std::string end = endStr.isString() ? endStr.asString() : "\n";

            std::vector<QuantumValue> args(n);
            for (int i = n - 1; i >= 0; --i)
                args[i] = pop();

            for (int i = 0; i < n; ++i)
            {
                if (i > 0)
                    std::cout << sep;
                std::cout << args[i].toString();
            }
            std::cout << end;
            std::cout.flush();
            break;
        }

        // ── Unhandled ─────────────────────────────────────────────────────
        default:
            throw RuntimeError("VM: unhandled opcode " +
                                   std::to_string(static_cast<int>(instr.op)),
                               line);
        } // switch
    } // while frames
        }
        catch (QuantumError &e)
        {
            // A handler below stopDepth belongs to an outer runFrame
            // invocation — rethrow so the C++ stack unwinds through it.
            if (handlers_.empty() || handlers_.back().frameDepth < stopDepth)
                throw;
            ExceptionHandler h = handlers_.back();
            handlers_.pop_back();
            while (frames_.size() > h.frameDepth)
                frames_.pop_back();
            while (stack_.size() > h.stackDepth)
                stack_.pop_back();
            push(QuantumValue(std::string(e.what())));
            if (!frames_.empty())
                frames_.back().ip = h.catchIp;
        }
    }
}

// ─── Register natives (same set as the old Interpreter) ──────────────────────

static double toNum2(const QuantumValue &v, const std::string &ctx)
{
    if (v.isNumber())
        return v.asNumber();
    throw TypeError("Expected number in " + ctx + ", got " + v.typeName());
}

