#include "Vm.h"
#include "Error.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

QuantumValue VM::callArrayMethod(std::shared_ptr<Array> arr, const std::string &m,
                                 std::vector<QuantumValue> args)
{
    if (m == "push" || m == "append")
    {
        arr->push_back(args.empty() ? QuantumValue() : args[0]);
        return QuantumValue(arr);
    }
    if (m == "pop")
    {
        if (arr->empty())
            throw RuntimeError("pop() on empty array");
        int idx = args.empty() ? (int)arr->size() - 1 : (int)args[0].asNumber();
        if (idx < 0)
            idx += (int)arr->size();
        if (idx < 0 || idx >= (int)arr->size())
            throw RuntimeError("pop() index out of range");
        QuantumValue v = (*arr)[idx];
        arr->erase(arr->begin() + idx);
        return v;
    }
    if (m == "length" || m == "size")
        return QuantumValue((double)arr->size());
    // C++ compatibility: a.begin()/a.end() return the array itself so
    // iterator-style calls like sort(a.begin(), a.end()) see the array.
    if (m == "begin" || m == "end")
        return QuantumValue(arr);
    // C++ std::vector / stack / queue method names
    if (m == "push_back" || m == "emplace_back" || m == "push_front")
    {
        for (auto &v : args)
        {
            if (m == "push_front")
                arr->insert(arr->begin(), v);
            else
                arr->push_back(v);
        }
        return QuantumValue((double)arr->size());
    }
    if (m == "pop_back")
    {
        if (arr->empty())
            return QuantumValue();
        QuantumValue v = arr->back();
        arr->pop_back();
        return v;
    }
    if (m == "pop_front")
    {
        if (arr->empty())
            return QuantumValue();
        QuantumValue v = arr->front();
        arr->erase(arr->begin());
        return v;
    }
    if (m == "empty")
        return QuantumValue(arr->empty());
    if (m == "front")
        return arr->empty() ? QuantumValue() : arr->front();
    if (m == "back" || m == "top")
        return arr->empty() ? QuantumValue() : arr->back();
    if (m == "at")
    {
        if (args.empty())
            throw RuntimeError("at() requires an index");
        int i = (int)args[0].asNumber();
        if (i < 0)
            i += (int)arr->size();
        if (i < 0 || i >= (int)arr->size())
            throw RuntimeError("at(): index out of range");
        return (*arr)[i];
    }
    if (m == "shift")
    {
        if (arr->empty())
            return QuantumValue();
        QuantumValue v = arr->front();
        arr->erase(arr->begin());
        return v;
    }
    if (m == "unshift")
    {
        if (!args.empty())
            arr->insert(arr->begin(), args[0]);
        return QuantumValue((double)arr->size());
    }
    if (m == "reverse")
    {
        std::reverse(arr->begin(), arr->end());
        return QuantumValue(arr);
    }
    if (m == "sort")
    {
        std::sort(arr->begin(), arr->end(), [](const QuantumValue &a, const QuantumValue &b)
                  { return a.isNumber() && b.isNumber() ? a.asNumber() < b.asNumber() : a.toString() < b.toString(); });
        return QuantumValue(arr);
    }
    if (m == "join")
    {
        std::string sep = args.empty() ? "," : args[0].toString();
        std::string s;
        for (size_t i = 0; i < arr->size(); i++)
        {
            if (i)
                s += sep;
            s += (*arr)[i].toString();
        }
        return QuantumValue(s);
    }
    if (m == "includes" || m == "contains")
    {
        if (args.empty())
            return QuantumValue(false);
        for (auto &v : *arr)
            if (VM::valuesEqual(v, args[0]))
                return QuantumValue(true);
        return QuantumValue(false);
    }
    if (m == "indexOf")
    {
        if (args.empty())
            return QuantumValue(-1.0);
        for (size_t i = 0; i < arr->size(); i++)
            if (VM::valuesEqual((*arr)[i], args[0]))
                return QuantumValue((double)i);
        return QuantumValue(-1.0);
    }
    if (m == "slice")
    {
        int start = args.empty() ? 0 : (int)args[0].asNumber();
        int stop = args.size() > 1 ? (int)args[1].asNumber() : (int)arr->size();
        int len = (int)arr->size();
        if (start < 0)
            start = std::max(0, len + start);
        if (stop < 0)
            stop = std::max(0, len + stop);
        stop = std::min(stop, len);
        auto r = std::make_shared<Array>(arr->begin() + start, arr->begin() + stop);
        return QuantumValue(r);
    }
    if (m == "splice")
    {
        if (args.empty())
            return QuantumValue(std::make_shared<Array>());
        int idx = (int)args[0].asNumber();
        int deleteCount = args.size() > 1 ? (int)args[1].asNumber() : (int)arr->size() - idx;
        if (idx < 0)
            idx = std::max(0, (int)arr->size() + idx);
        idx = std::min(idx, (int)arr->size());
        deleteCount = std::max(0, std::min(deleteCount, (int)arr->size() - idx));
        auto removed = std::make_shared<Array>(arr->begin() + idx, arr->begin() + idx + deleteCount);
        arr->erase(arr->begin() + idx, arr->begin() + idx + deleteCount);
        for (size_t i = 2; i < args.size(); i++)
            arr->insert(arr->begin() + idx + (i - 2), args[i]);
        return QuantumValue(removed);
    }
    if (m == "concat")
    {
        auto r = std::make_shared<Array>(*arr);
        for (auto &a : args)
            if (a.isArray())
                for (auto &v : *a.asArray())
                    r->push_back(v);
        return QuantumValue(r);
    }
    if (m == "flat" || m == "flatten")
    {
        auto r = std::make_shared<Array>();
        for (auto &v : *arr)
        {
            if (v.isArray())
                for (auto &inner : *v.asArray())
                    r->push_back(inner);
            else
                r->push_back(v);
        }
        return QuantumValue(r);
    }
    if (m == "fill")
    {
        if (args.empty())
            return QuantumValue(arr);
        QuantumValue val = args[0];
        for (auto &v : *arr)
            v = val;
        return QuantumValue(arr);
    }
    if (m == "count")
    {
        if (args.empty())
            return QuantumValue((double)arr->size());
        int c = 0;
        for (auto &v : *arr)
            if (VM::valuesEqual(v, args[0]))
                c++;
        return QuantumValue((double)c);
    }
    if (m == "insert")
    {
        if (args.size() >= 2)
        {
            int idx = (int)args[0].asNumber();
            if (idx < 0)
                idx = std::max(0, (int)arr->size() + idx);
            idx = std::min(idx, (int)arr->size());
            arr->insert(arr->begin() + idx, args[1]);
        }
        return QuantumValue();
    }
    if (m == "remove")
    {
        if (!args.empty())
        {
            for (auto it = arr->begin(); it != arr->end(); ++it)
                if (VM::valuesEqual(*it, args[0]))
                {
                    arr->erase(it);
                    break;
                }
        }
        return QuantumValue();
    }
    if (m == "clear")
    {
        arr->clear();
        return QuantumValue();
    }
    if (m == "copy" || m == "dup" || m == "clone")
    {
        return QuantumValue(std::make_shared<Array>(*arr));
    }
    if (m == "take")
    {
        int n = args.empty() ? 0 : (int)args[0].asNumber();
        n = std::max(0, std::min(n, (int)arr->size()));
        return QuantumValue(std::make_shared<Array>(arr->begin(), arr->begin() + n));
    }
    if (m == "extend")
    {
        if (!args.empty() && args[0].isArray())
            for (auto &v : *args[0].asArray())
                arr->push_back(v);
        return QuantumValue();
    }

    // ── Higher-order array methods: map, filter, reduce, forEach ──────────
    // Helper: call a QuantumValue (closure, native, or bound method) with given args
    auto callFn = [&](QuantumValue fn, std::vector<QuantumValue> fnArgs) -> QuantumValue
    {
        if (fn.isNative())
            return fn.asNative()->fn(fnArgs);
        if (fn.isFunction())
        {
            push(fn);
            for (auto &a : fnArgs)
                push(a);
            callClosure(fn.asFunction(), (int)fnArgs.size(), 0);
            size_t depth = frames_.size() - 1;
            runFrame(depth);
            return pop();
        }
        if (fn.isBoundMethod())
        {
            auto bm = fn.asBoundMethod();
            push(fn);
            push(bm->self);
            for (auto &a : fnArgs)
                push(a);
            callClosure(bm->method, (int)fnArgs.size() + 1, 0);
            size_t depth = frames_.size() - 1;
            runFrame(depth);
            return pop();
        }
        throw TypeError("map/filter/reduce: callback is not callable");
    };

    if (m == "map")
    {
        if (args.empty())
            throw RuntimeError("map() requires a callback");
        QuantumValue fn = args[0];
        auto result = std::make_shared<Array>();
        for (size_t i = 0; i < arr->size(); ++i)
            result->push_back(callFn(fn, {(*arr)[i], QuantumValue((double)i)}));
        return QuantumValue(result);
    }
    if (m == "filter" || m == "select")
    {
        if (args.empty())
            throw RuntimeError("filter() requires a callback");
        QuantumValue fn = args[0];
        auto result = std::make_shared<Array>();
        for (auto &v : *arr)
            if (callFn(fn, {v}).isTruthy())
                result->push_back(v);
        return QuantumValue(result);
    }
    if (m == "remove_if")
    {
        // C++ std::list::remove_if — in-place removal of matching elements
        if (args.empty())
            throw RuntimeError("remove_if() requires a callback");
        QuantumValue fn = args[0];
        Array kept;
        for (auto &v : *arr)
            if (!callFn(fn, {v}).isTruthy())
                kept.push_back(v);
        size_t removed = arr->size() - kept.size();
        *arr = std::move(kept);
        return QuantumValue((double)removed);
    }
    if (m == "reduce")
    {
        if (args.empty())
            throw RuntimeError("reduce() requires a callback");
        QuantumValue fn = args[0];
        if (arr->empty())
        {
            if (args.size() > 1)
                return args[1];
            throw RuntimeError("reduce() on empty array with no initial value");
        }
        QuantumValue acc = args.size() > 1 ? args[1] : (*arr)[0];
        size_t start = args.size() > 1 ? 0 : 1;
        for (size_t i = start; i < arr->size(); ++i)
            acc = callFn(fn, {acc, (*arr)[i], QuantumValue((double)i)});
        return acc;
    }
    if (m == "forEach" || m == "each")
    {
        if (args.empty())
            throw RuntimeError("forEach() requires a callback");
        QuantumValue fn = args[0];
        for (size_t i = 0; i < arr->size(); ++i)
            callFn(fn, {(*arr)[i], QuantumValue((double)i)});
        return QuantumValue();
    }
    if (m == "find")
    {
        if (args.empty())
            throw RuntimeError("find() requires a callback");
        QuantumValue fn = args[0];
        for (auto &v : *arr)
            if (callFn(fn, {v}).isTruthy())
                return v;
        return QuantumValue();
    }
    if (m == "every" || m == "all")
    {
        if (args.empty())
            throw RuntimeError("every() requires a callback");
        QuantumValue fn = args[0];
        for (auto &v : *arr)
            if (!callFn(fn, {v}).isTruthy())
                return QuantumValue(false);
        return QuantumValue(true);
    }
    if (m == "some" || m == "any")
    {
        if (args.empty())
            throw RuntimeError("some() requires a callback");
        QuantumValue fn = args[0];
        for (auto &v : *arr)
            if (callFn(fn, {v}).isTruthy())
                return QuantumValue(true);
        return QuantumValue(false);
    }
    if (m == "none")
    {
        if (args.empty())
            throw RuntimeError("none() requires a callback");
        QuantumValue fn = args[0];
        for (auto &v : *arr)
            if (callFn(fn, {v}).isTruthy())
                return QuantumValue(false);
        return QuantumValue(true);
    }

    throw TypeError("Array has no method '" + m + "'");
}
