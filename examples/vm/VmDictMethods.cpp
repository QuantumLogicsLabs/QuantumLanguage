#include "Vm.h"
#include "Error.h"
#include <memory>
#include <string>
#include <vector>

QuantumValue VM::callDictMethod(std::shared_ptr<Dict> dict, const std::string &m,
                                std::vector<QuantumValue> args)
{
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
    throw TypeError("Dict has no method '" + m + "'");
}
