#include "Vm.h"
#include "Error.h"
#include <memory>
#include <string>
#include <vector>

QuantumValue VM::callDictMethod(std::shared_ptr<Dict> dict, const std::string &m,
                                std::vector<QuantumValue> args)
{
    // Ruby's `obj.respond_to?(:name)` reflection check. The native
    // objects this VM hands back for Thread/Queue/socket/... are dicts
    // whose methods are ordinary keys, so membership is the answer.
    if (m == "respond_to")
        return QuantumValue(!args.empty() && dict->count(args[0].toString()) > 0);
    if (m == "keys")
    {
        auto arr = std::make_shared<Array>();
        for (auto &[k, v] : *dict)
            arr->push_back(QuantumValue(k));
        return QuantumValue(arr);
    }
    if (m == "values")
    {
        auto arr = std::make_shared<Array>();
        for (auto &[k, v] : *dict)
            arr->push_back(v);
        return QuantumValue(arr);
    }
    if (m == "items" || m == "entries")
    {
        auto arr = std::make_shared<Array>();
        for (auto &[k, v] : *dict)
        {
            auto pair = std::make_shared<Array>();
            pair->push_back(QuantumValue(k));
            pair->push_back(v);
            arr->push_back(QuantumValue(pair));
        }
        return QuantumValue(arr);
    }
    if (m == "has" || m == "contains" || m == "hasOwnProperty")
    {
        if (args.empty())
            return QuantumValue(false);
        return QuantumValue(dict->count(args[0].toString()) > 0);
    }
    if (m == "get")
    {
        if (args.empty())
            return QuantumValue();
        auto it = dict->find(args[0].toString());
        return it != dict->end() ? it->second : (args.size() > 1 ? args[1] : QuantumValue());
    }
    if (m == "set")
    {
        if (args.size() >= 2)
            (*dict)[args[0].toString()] = args[1];
        return QuantumValue(dict);
    }
    if (m == "delete")
    {
        if (!args.empty())
            dict->erase(args[0].toString());
        return QuantumValue(true);
    }
    if (m == "clear")
    {
        dict->clear();
        return QuantumValue();
    }
    if (m == "size" || m == "length")
        return QuantumValue((double)dict->size());
    // Python dict.update(other) — merge keys from another dict
    if (m == "update")
    {
        if (!args.empty() && args[0].isDict())
            for (auto &kv : *args[0].asDict())
                (*dict)[kv.first] = kv.second;
        return QuantumValue();
    }
    // Python dict.pop(key[, default])
    if (m == "pop")
    {
        if (args.empty())
            return QuantumValue();
        auto it = dict->find(args[0].toString());
        if (it == dict->end())
            return args.size() > 1 ? args[1] : QuantumValue();
        QuantumValue v = it->second;
        dict->erase(it);
        return v;
    }
    // ── Ruby Hash iteration ───────────────────────────────────────────────
    // Delegated to the array implementations over a [key, value] pair list,
    // so block dispatch (closure / native / bound method) and the block
    // param destructuring (`|k, v|`) behave exactly as they do for arrays.
    if (m == "each" || m == "each_pair" || m == "map" || m == "select" ||
        m == "filter" || m == "reject" || m == "sort_by" || m == "min_by" ||
        m == "max_by" || m == "find" || m == "any" || m == "all" || m == "none" ||
        m == "count" || m == "sum" || m == "to_a" || m == "each_with_index" ||
        m == "sort" || m == "min" || m == "max" || m == "first")
    {
        auto pairs = std::make_shared<Array>();
        for (auto &[k, v] : *dict)
        {
            auto pair = std::make_shared<Array>();
            pair->push_back(QuantumValue(k));
            pair->push_back(v);
            pairs->push_back(QuantumValue(pair));
        }
        std::string target = (m == "each_pair") ? "each" : m;
        return callArrayMethod(pairs, target, args);
    }
    if (m == "each_key" || m == "each_value")
    {
        auto items = std::make_shared<Array>();
        for (auto &[k, v] : *dict)
            items->push_back(m == "each_key" ? QuantumValue(k) : v);
        return callArrayMethod(items, "each", args);
    }
    if (m == "empty")
        return QuantumValue(dict->empty());
    if (m == "key" || m == "has_key" || m == "include" || m == "member")
        return QuantumValue(!args.empty() && dict->count(args[0].toString()) > 0);
    throw TypeError("Dict has no method '" + m + "'");
}
