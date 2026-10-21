// C++ <algorithm>/<numeric> support: STL-style iterators into arrays and the
// free functions that take iterator ranges — sort(v.begin(), v.end(), cmp),
// *max_element(...), v.erase(remove(...), v.end()), and so on.
#include "Vm.h"
#include "Error.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
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

namespace
{
// Formatting state of cout, which persists across statements like the real
// stream: `cout << fixed << setprecision(2);` affects later insertions.
struct StreamState
{
    int precision = 6;
    bool fixed = false, scientific = false, boolalpha = false, leftAlign = false;
    int base = 10;
    int width = 0; // applies to the next insertion only
    char fill = ' ';
};

// `kind`: what the program declared the value as — 1 a double/float (always
// printed the floating-point way, so 1e6 → "1e+06"), 2 an integer (whole
// numbers ignore fixed/scientific, as for a C++ int), 0 unknown.
std::string formatNumber(double v, const StreamState &st, int kind = 0)
{
    bool integral = std::isfinite(v) && std::floor(v) == v && std::fabs(v) < 1e15;
    std::ostringstream out;
    if (st.base != 10 && integral && kind != 1)
        out << (st.base == 16 ? std::hex : std::oct) << static_cast<long long>(v);
    else if (kind == 2 && integral)
        out << static_cast<long long>(v);
    else if (st.fixed)
        out << std::fixed << std::setprecision(st.precision) << v;
    else if (st.scientific)
        out << std::scientific << std::setprecision(st.precision) << v;
    else if (integral && kind != 1)
        out << static_cast<long long>(v); // an int prints as an int
    else
        out << std::setprecision(st.precision) << v; // ostream default (%g)
    return out.str();
}
} // namespace

void VM::sortValues(Array &arr, const QuantumValue &key, const QuantumValue &cmp, bool reverse)
{
    auto callable = [](const QuantumValue &f)
    { return f.isClosure() || f.isBoundMethod() || f.isNative(); };
    std::vector<std::pair<QuantumValue, QuantumValue>> items; // (sort key, element)
    items.reserve(arr.size());
    for (auto &v : arr)
        items.emplace_back(callable(key) ? invokeCallable(key, {v}) : v, v);
    auto less = [&](const QuantumValue &a, const QuantumValue &b)
    {
        if (!callable(cmp))
            return stlLess(a, b);
        QuantumValue r = invokeCallable(cmp, {a, b});
        return r.isNumber() ? r.asNumber() < 0 : r.isTruthy();
    };
    auto before = [&](const auto &x, const auto &y)
    { return reverse ? less(y.first, x.first) : less(x.first, y.first); };

    // Bottom-up merge sort instead of std::stable_sort: the comparator is user
    // code, and std::stable_sort is undefined behaviour (it reads out of bounds
    // and crashed the VM) when that isn't a strict weak ordering — e.g. the
    // common `sort(() => Math.random() - 0.5)` shuffle. Every index here is
    // bounds-checked, so an inconsistent comparator only yields some order.
    std::vector<std::pair<QuantumValue, QuantumValue>> buf(items.size());
    const size_t n = items.size();
    for (size_t width = 1; width < n; width *= 2)
    {
        for (size_t lo = 0; lo < n; lo += 2 * width)
        {
            size_t mid = std::min(lo + width, n), hi = std::min(lo + 2 * width, n);
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                buf[k++] = before(items[j], items[i]) ? std::move(items[j++]) : std::move(items[i++]);
            while (i < mid)
                buf[k++] = std::move(items[i++]);
            while (j < hi)
                buf[k++] = std::move(items[j++]);
        }
        items.swap(buf);
    }
    for (size_t i = 0; i < items.size(); ++i)
        arr[i] = items[i].second;
}

bool VM::userStreamText(const QuantumValue &v, std::string &out)
{
    if (!freeOperators_ || !v.isInstance())
        return false;
    for (auto *k = v.asInstance()->klass.get(); k; k = k->base.get())
    {
        const std::string fn = "__op__lshift__" + k->name;
        if (!globals->has(fn))
            continue;
        QuantumValue stream = globals->get("ostringstream").asNative()->fn({});
        invokeCallable(globals->get(fn), {stream, v});
        out = (*stream.asDict())["str"].asNative()->fn({}).toString();
        return true;
    }
    return false;
}

void VM::registerStlNatives()
{
    // ── C++ ostream formatting (see Parser::parseStreamInsertions) ─────────
    {
        auto st = std::make_shared<StreamState>();
        auto nat = [&](const std::string &name, QuantumNativeFunc fn)
        {
            auto n = std::make_shared<QuantumNative>();
            n->name = name;
            n->fn = std::move(fn);
            globals->define(name, QuantumValue(n));
        };
        nat("__cout__", [st, this](std::vector<QuantumValue> a) -> QuantumValue
            {
            QuantumValue v = arg(a, 0);
            // A bare manipulator name that is not a variable: cout << fixed
            if (v.isNil() && a.size() > 1)
            {
                std::string m = a[1].toString();
                bool handled = true;
                if (m == "fixed") { st->fixed = true; st->scientific = false; }
                else if (m == "scientific") { st->scientific = true; st->fixed = false; }
                else if (m == "defaultfloat") st->fixed = st->scientific = false;
                else if (m == "left") st->leftAlign = true;
                else if (m == "right" || m == "internal") st->leftAlign = false;
                else if (m == "boolalpha") st->boolalpha = true;
                else if (m == "noboolalpha") st->boolalpha = false;
                else if (m == "hex") st->base = 16;
                else if (m == "oct") st->base = 8;
                else if (m == "dec") st->base = 10;
                else if (m == "flush" || m == "showpoint" || m == "noshowpoint" ||
                         m == "showpos" || m == "noshowpos" || m == "uppercase" ||
                         m == "nouppercase") {}
                else if (m == "ends") return QuantumValue(std::string(1, '\0'));
                else handled = false;
                if (handled) return QuantumValue(std::string());
            }
            std::string s;
            if (v.isBool())
                s = st->boolalpha ? (v.asBool() ? "true" : "false") : (v.asBool() ? "1" : "0");
            else if (v.isNumber())
            {
                // a[2]: the declared kind (Compiler::compileCall) — 1 double, 2 integer
                int kind = a.size() > 2 && a[2].isNumber() ? (int)a[2].asNumber() : 0;
                s = formatNumber(v.asNumber(), *st, kind);
            }
            else if (!userStreamText(v, s)) // the class's own operator<<, if any
                s = v.toString();
            if ((int)s.size() < st->width)
            {
                std::string pad(st->width - s.size(), st->fill);
                s = st->leftAlign ? s + pad : pad + s;
            }
            st->width = 0;
            return QuantumValue(s); });
        nat("__cout_manip__", [st](std::vector<QuantumValue> a) -> QuantumValue
            {
            std::string m = arg(a, 0).toString();
            QuantumValue v = arg(a, 1);
            if (m == "setw") st->width = v.isNumber() ? (int)v.asNumber() : 0;
            else if (m == "setprecision") st->precision = v.isNumber() ? (int)v.asNumber() : 6;
            else if (m == "setfill") st->fill = v.toString().empty() ? ' ' : v.toString()[0];
            return QuantumValue(std::string()); });
        nat("__cerr__", [](std::vector<QuantumValue> a) -> QuantumValue
            {
            for (auto &v : a) std::cerr << v.toString();
            std::cerr.flush();
            return QuantumValue(); });

        // std::ostringstream / std::stringstream (output side): a Dict with a
        // "write" native, which `ss << a << b` streams into (see Op::LSHIFT),
        // formatting values the way cout does; ss.str() returns the text and
        // ss.str("") replaces it.
        auto stringStream = [this](std::vector<QuantumValue> a) -> QuantumValue
        {
            auto buf = std::make_shared<std::string>(
                a.empty() || a[0].isNil() ? std::string() : a[0].toString());
            auto method = [](const std::string &name, QuantumNativeFunc fn)
            {
                auto n = std::make_shared<QuantumNative>();
                n->name = name;
                n->fn = std::move(fn);
                return QuantumValue(n);
            };
            auto d = std::make_shared<Dict>();
            (*d)["write"] = method("ostringstream.write", [buf, this](std::vector<QuantumValue> v) -> QuantumValue
                                   {
                if (v.empty() || v[0].isNil()) return QuantumValue();
                std::string text;
                if (v[0].isBool()) *buf += v[0].asBool() ? "1" : "0";
                else if (v[0].isNumber()) *buf += formatNumber(v[0].asNumber(), StreamState{});
                else if (userStreamText(v[0], text)) *buf += text; // the class's operator<<
                else *buf += v[0].toString();
                return QuantumValue(); });
            (*d)["str"] = method("ostringstream.str", [buf](std::vector<QuantumValue> v) -> QuantumValue
                                 {
                if (!v.empty()) { *buf = v[0].toString(); return QuantumValue(); }
                return QuantumValue(*buf); });
            (*d)["clear"] = method("ostringstream.clear", [](std::vector<QuantumValue>) -> QuantumValue
                                   { return QuantumValue(); });
            return QuantumValue(d);
        };
        nat("ostringstream", stringStream);
        nat("stringstream", stringStream);

        // `L << x` where x is declared a C/C++ double (Compiler::compileBinary):
        // a stream gets x formatted as a double; anything else behaves like
        // Op::LSHIFT.
        nat("__shl_float__", [this](std::vector<QuantumValue> a) -> QuantumValue
            {
            QuantumValue L = arg(a, 0), R = arg(a, 1);
            if (L.isDict())
            {
                auto d = L.asDict();
                auto it = d->find("write");
                if (it != d->end() && it->second.isNative())
                {
                    QuantumValue text = R.isNumber()
                        ? QuantumValue(formatNumber(R.asNumber(), StreamState{}, 1)) : R;
                    it->second.asNative()->fn({text});
                    return L;
                }
            }
            if (L.isArray())
            {
                L.asArray()->push_back(R);
                return L;
            }
            return execBinary(Op::LSHIFT, L, R, 0); });
    }

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

    // See Compiler::compileCall: `s = __str_inplace__(s, reverse(s.begin(), s.end()))`.
    reg("__str_inplace__", [](std::vector<QuantumValue> a) -> QuantumValue
        {
        if (a.size() > 1 && a[0].isString() && a[1].isString()) return a[1];
        return arg(a, 0); },
        /*weak=*/false);

    for (const char *name : {"sort", "stable_sort"})
        reg(name, [lessBy](std::vector<QuantumValue> a) -> QuantumValue
            {
            if (!a.empty() && a[0].isString())
            {
                std::string s = a[0].asString(); // sort(s.begin(), s.end()) on a string
                std::sort(s.begin(), s.end());
                return QuantumValue(s);
            }
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
