// C++ <algorithm>/<numeric> support: STL-style iterators into arrays and the
// free functions that take iterator ranges — sort(v.begin(), v.end(), cmp),
// *max_element(...), v.erase(remove(...), v.end()), and so on.
#include "Vm.h"
#include "Error.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

static const char *IT_ARR = "__it_arr";
static const char *IT_POS = "__it_pos";

bool isStlIterator(const QuantumValue &v)
{
    if (!v.isDict())
        return false;
    const Dict &d = *v.asDict();
    return d.count(IT_ARR) && d.count(IT_POS);
}

QuantumValue makeStlIterator(std::shared_ptr<Array> arr, long pos)
{
    auto d = std::make_shared<Dict>();
    (*d)[IT_ARR] = QuantumValue(std::move(arr));
    (*d)[IT_POS] = QuantumValue(static_cast<double>(pos));
    return QuantumValue(d);
}

std::shared_ptr<Array> stlIteratorArray(const QuantumValue &v)
{
    const QuantumValue &a = v.asDict()->at(IT_ARR);
    return a.isArray() ? a.asArray() : std::make_shared<Array>();
}

long stlIteratorPos(const QuantumValue &v)
{
    return static_cast<long>(v.asDict()->at(IT_POS).asNumber());
}

QuantumValue stlIteratorDeref(const QuantumValue &v)
{
    auto arr = stlIteratorArray(v);
    long pos = stlIteratorPos(v);
    if (pos < 0 || pos >= static_cast<long>(arr->size()))
        return QuantumValue();
    return (*arr)[pos];
}

namespace
{
// [first, last) as an array and bounds. Accepts iterator pairs and, for the
// older `begin()`-returns-the-array convention, a bare array (whole range).
struct Range
{
    std::shared_ptr<Array> arr;
    long b = 0, e = 0;
};

bool toRange(const QuantumValue &first, const QuantumValue &last, Range &r)
{
    if (isStlIterator(first))
    {
        r.arr = stlIteratorArray(first);
        r.b = stlIteratorPos(first);
        r.e = (isStlIterator(last) && stlIteratorArray(last) == r.arr)
                  ? stlIteratorPos(last)
                  : static_cast<long>(r.arr->size());
    }
    else if (first.isArray())
    {
        r.arr = first.asArray();
        r.b = 0;
        r.e = static_cast<long>(r.arr->size());
    }
    else
        return false;
    long n = static_cast<long>(r.arr->size());
    r.b = std::clamp(r.b, 0L, n);
    r.e = std::clamp(r.e, r.b, n);
    return true;
}

QuantumValue arg(const std::vector<QuantumValue> &a, size_t i)
{
    return i < a.size() ? a[i] : QuantumValue();
}
} // namespace

bool VM::stlLess(const QuantumValue &a, const QuantumValue &b)
{
    if (a.isNumber() && b.isNumber())
        return a.asNumber() < b.asNumber();
    if (a.isBool() && b.isBool())
        return !a.asBool() && b.asBool();
    if (a.isArray() && b.isArray())
    {
        const Array &x = *a.asArray(), &y = *b.asArray();
        for (size_t i = 0; i < x.size() && i < y.size(); ++i)
        {
            if (stlLess(x[i], y[i]))
                return true;
            if (stlLess(y[i], x[i]))
                return false;
        }
        return x.size() < y.size();
    }
    if (a.isInstance())
    {
        for (auto *k = a.asInstance()->klass.get(); k; k = k->base.get())
        {
            auto it = k->methods.find("__lt__");
            if (it == k->methods.end())
                continue;
            auto bm = std::make_shared<QuantumBoundMethod>();
            bm->method = it->second;
            bm->self = a;
            return invokeCallable(QuantumValue(bm), {b}).isTruthy();
        }
    }
    if (a.isNumber() != b.isNumber())
        return a.isNumber(); // numbers order before everything else
    return a.toString() < b.toString();
}

void VM::registerStlNatives()
{
    auto reg = [&](const std::string &name, QuantumNativeFunc fn, bool weak = true)
    {
        auto nat = std::make_shared<QuantumNative>();
        nat->name = name;
        nat->fn = std::move(fn);
        globals->define(name, QuantumValue(nat));
        if (weak)
            weakGlobals_.insert(name);
    };
    auto truthy = [this](const QuantumValue &fn, std::vector<QuantumValue> a)
    { return invokeCallable(fn, a).isTruthy(); };
    auto lessBy = [this](const QuantumValue &cmp)
    {
        return [this, cmp](const QuantumValue &x, const QuantumValue &y)
        {
            if (cmp.isNil())
                return stlLess(x, y);
            return invokeCallable(cmp, {x, y}).isTruthy();
        };
    };
    auto endOf = [](const Range &r)
    { return makeStlIterator(r.arr, r.e); };

    for (const char *name : {"sort", "stable_sort"})
        reg(name, [lessBy](std::vector<QuantumValue> a) -> QuantumValue
            {
            Range r;
            if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue();
            std::stable_sort(r.arr->begin() + r.b, r.arr->begin() + r.e, lessBy(arg(a, 2)));
            return QuantumValue(); });

    // reverse(first, last) reverses in place; reverse(array) / reverse(string)
    // keep their existing behaviour (reverse the array, return the string).
    reg("reverse", [](std::vector<QuantumValue> a) -> QuantumValue
        {
        if (!a.empty() && a[0].isString())
        {
            std::string s = a[0].toString();
            std::reverse(s.begin(), s.end());
            return QuantumValue(s);
        }
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue();
        std::reverse(r.arr->begin() + r.b, r.arr->begin() + r.e);
        return a[0].isArray() ? a[0] : QuantumValue(); },
        /*weak=*/false);

    reg("fill", [](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue();
        std::fill(r.arr->begin() + r.b, r.arr->begin() + r.e, arg(a, 2));
        return QuantumValue(); });

    reg("iota", [](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue();
        double v = arg(a, 2).isNumber() ? arg(a, 2).asNumber() : 0.0;
        for (long i = r.b; i < r.e; ++i) (*r.arr)[i] = QuantumValue(v++);
        return QuantumValue(); });

    reg("find", [endOf](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue();
        for (long i = r.b; i < r.e; ++i)
            if (VM::valuesEqual((*r.arr)[i], arg(a, 2))) return makeStlIterator(r.arr, i);
        return endOf(r); });

    reg("find_if", [endOf, truthy](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue();
        for (long i = r.b; i < r.e; ++i)
            if (truthy(arg(a, 2), {(*r.arr)[i]})) return makeStlIterator(r.arr, i);
        return endOf(r); });

    reg("count", [](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue(0.0);
        double n = 0;
        for (long i = r.b; i < r.e; ++i)
            if (VM::valuesEqual((*r.arr)[i], arg(a, 2))) n++;
        return QuantumValue(n); });

    reg("count_if", [truthy](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue(0.0);
        double n = 0;
        for (long i = r.b; i < r.e; ++i)
            if (truthy(arg(a, 2), {(*r.arr)[i]})) n++;
        return QuantumValue(n); });

    for (const char *name : {"any_of", "all_of", "none_of"})
    {
        std::string which = name;
        reg(name, [truthy, which](std::vector<QuantumValue> a) -> QuantumValue
            {
            Range r;
            if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue(which != "any_of");
            bool any = false, all = true;
            for (long i = r.b; i < r.e; ++i) {
                bool t = truthy(arg(a, 2), {(*r.arr)[i]});
                any = any || t;
                all = all && t;
            }
            return QuantumValue(which == "any_of" ? any : which == "all_of" ? all : !any); });
    }

    reg("accumulate", [this](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        QuantumValue acc = arg(a, 2);
        if (!toRange(arg(a, 0), arg(a, 1), r)) return acc;
        for (long i = r.b; i < r.e; ++i)
            acc = a.size() > 3 ? invokeCallable(a[3], {acc, (*r.arr)[i]})
                               : execBinary(Op::ADD, acc, (*r.arr)[i], 0);
        return acc; });

    for (const char *name : {"max_element", "min_element"})
    {
        bool wantMax = std::string(name) == "max_element";
        reg(name, [lessBy, endOf, wantMax](std::vector<QuantumValue> a) -> QuantumValue
            {
            Range r;
            if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue();
            if (r.b == r.e) return endOf(r);
            auto less = lessBy(arg(a, 2));
            long best = r.b;
            for (long i = r.b + 1; i < r.e; ++i)
                if (wantMax ? less((*r.arr)[best], (*r.arr)[i]) : less((*r.arr)[i], (*r.arr)[best]))
                    best = i;
            return makeStlIterator(r.arr, best); });
    }

    // remove / remove_if / unique move the kept elements to the front and
    // return the new logical end, for the erase-remove idiom.
    auto compact = [](Range &r, const std::function<bool(long)> &drop)
    {
        long out = r.b;
        for (long i = r.b; i < r.e; ++i)
            if (!drop(i))
                (*r.arr)[out++] = (*r.arr)[i];
        return makeStlIterator(r.arr, out);
    };
    reg("remove", [compact](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue();
        QuantumValue v = arg(a, 2);
        return compact(r, [&](long i) { return VM::valuesEqual((*r.arr)[i], v); }); });
    reg("remove_if", [compact, truthy](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue();
        QuantumValue pred = arg(a, 2);
        return compact(r, [&](long i) { return truthy(pred, {(*r.arr)[i]}); }); });
    reg("unique", [compact](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue();
        Array orig(r.arr->begin(), r.arr->end());
        return compact(r, [&](long i) {
            return i > r.b && VM::valuesEqual(orig[i], orig[i - 1]); }); });

    reg("for_each", [this](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue();
        for (long i = r.b; i < r.e; ++i) invokeCallable(arg(a, 2), {(*r.arr)[i]});
        return arg(a, 2); });

    reg("distance", [](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue(0.0);
        return QuantumValue(static_cast<double>(r.e - r.b)); });

    for (const char *name : {"next", "prev"})
    {
        double sign = std::string(name) == "next" ? 1.0 : -1.0;
        reg(name, [sign](std::vector<QuantumValue> a) -> QuantumValue
            {
            if (a.empty() || !isStlIterator(a[0])) return arg(a, 0);
            double n = a.size() > 1 && a[1].isNumber() ? a[1].asNumber() : 1.0;
            return makeStlIterator(stlIteratorArray(a[0]),
                                   stlIteratorPos(a[0]) + static_cast<long>(sign * n)); });
    }

    // Sorted-range searches.
    reg("lower_bound", [lessBy](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue();
        auto it = std::lower_bound(r.arr->begin() + r.b, r.arr->begin() + r.e, arg(a, 2), lessBy(arg(a, 3)));
        return makeStlIterator(r.arr, static_cast<long>(it - r.arr->begin())); });
    reg("upper_bound", [lessBy](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue();
        auto it = std::upper_bound(r.arr->begin() + r.b, r.arr->begin() + r.e, arg(a, 2), lessBy(arg(a, 3)));
        return makeStlIterator(r.arr, static_cast<long>(it - r.arr->begin())); });
    reg("binary_search", [lessBy](std::vector<QuantumValue> a) -> QuantumValue
        {
        Range r;
        if (!toRange(arg(a, 0), arg(a, 1), r)) return QuantumValue(false);
        return QuantumValue(std::binary_search(r.arr->begin() + r.b, r.arr->begin() + r.e,
                                               arg(a, 2), lessBy(arg(a, 3)))); });
}
