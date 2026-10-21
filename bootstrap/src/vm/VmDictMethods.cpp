#include "Error.h"
#include "Vm.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

QuantumValue VM::callDictMethod(std::shared_ptr<Dict> dict,
                                const std::string &m,
                                std::vector<QuantumValue> args) {
  // JS Date / URL / URLSearchParams objects (VmJsRuntime.cpp)
  {
    QuantumValue out;
    if (callJsObjectMethod(dict, m, args, out))
      return out;
  }
  // Ruby's `obj.respond_to?(:name)` reflection check. The native
  // objects this VM hands back for Thread/Queue/socket/... are dicts
  // whose methods are ordinary keys, so membership is the answer.
  if (m == "respond_to")
    return QuantumValue(!args.empty() && dict->count(args[0].toString()) > 0);
  if (m == "keys") {
    auto arr = std::make_shared<Array>();
    for (auto &[k, v] : *dict)
      arr->push_back(QuantumValue(k));
    return QuantumValue(arr);
  }
  if (m == "values") {
    auto arr = std::make_shared<Array>();
    for (auto &[k, v] : *dict)
      arr->push_back(v);
    return QuantumValue(arr);
  }
  if (m == "items" || m == "entries" || m == "sort") {
    auto arr = std::make_shared<Array>();
    for (auto &[k, v] : *dict) {
      auto pair = std::make_shared<Array>();
      pair->push_back(QuantumValue(k));
      pair->push_back(v);
      arr->push_back(QuantumValue(pair));
    }
    if (m == "sort") {
      std::sort(arr->begin(), arr->end(),
                [](const QuantumValue &a, const QuantumValue &b) {
                  return a.toString() < b.toString();
                });
    }
    return QuantumValue(arr);
  }
  if (m == "has" || m == "contains" || m == "hasOwnProperty") {
    if (args.empty())
      return QuantumValue(false);
    return QuantumValue(dict->count(args[0].toString()) > 0);
  }
  if (m == "get") {
    if (args.empty())
      return QuantumValue();
    auto it = dict->find(args[0].toString());
    return it != dict->end() ? it->second
                             : (args.size() > 1 ? args[1] : QuantumValue());
  }
  if (m == "set") {
    if (args.size() >= 2)
      (*dict)[args[0].toString()] = args[1];
    return QuantumValue(dict);
  }
  if (m == "delete") {
    if (!args.empty())
      dict->erase(args[0].toString());
    return QuantumValue(true);
  }
  // Python sets are dicts of members mapped to true (as set literals
  // compile); C++ std::set / std::map spell the same operations
  // insert / erase / count(key).
  if (m == "add" || m == "insert") {
    // map.insert({k, v}) / insert(make_pair(k, v)) keeps an existing key
    if (m == "insert" && args.size() == 1 && args[0].isArray() && args[0].asArray()->size() == 2) {
      const Array &kv = *args[0].asArray();
      dict->insert({kv[0].toString(), kv[1]});
    } else if (!args.empty())
      (*dict)[args[0].toString()] = args.size() > 1 ? args[1] : QuantumValue(true);
    return QuantumValue();
  }
  if (m == "discard" || m == "remove" || m == "erase") {
    size_t n = args.empty() ? 0 : dict->erase(args[0].toString());
    if (m == "remove" && n == 0)
      throw RuntimeError("KeyError: " + (args.empty() ? std::string() : args[0].toString()));
    return QuantumValue((double)n);
  }
  if (m == "count" && args.size() == 1 && !args[0].isFunction() && !args[0].isBoundMethod())
    return QuantumValue((double)dict->count(args[0].toString()));
  if (m == "copy")
    return QuantumValue(std::make_shared<Dict>(*dict));
  if (m == "union" || m == "intersection" || m == "difference" ||
      m == "symmetric_difference") {
    auto out = std::make_shared<Dict>(m == "intersection" ? Dict() : *dict);
    for (auto &other : args) {
      Dict members;
      if (other.isDict())
        members = *other.asDict();
      else if (other.isArray())
        for (auto &v : *other.asArray())
          members[v.toString()] = QuantumValue(true);
      if (m == "union")
        for (auto &kv : members)
          out->insert(kv);
      else if (m == "intersection")
        for (auto &kv : *dict)
          if (members.count(kv.first))
            out->insert(kv);
      else if (m == "difference")
        for (auto &kv : members)
          out->erase(kv.first);
      else
        for (auto &kv : members)
          if (!out->erase(kv.first))
            out->insert(kv);
    }
    return QuantumValue(out);
  }
  if (m == "issubset" || m == "issuperset" || m == "isdisjoint") {
    if (args.empty() || !args[0].isDict())
      return QuantumValue(false);
    const Dict &other = *args[0].asDict();
    const Dict &small = m == "issuperset" ? other : *dict;
    const Dict &big = m == "issuperset" ? *dict : other;
    for (auto &kv : small) {
      bool in = big.count(kv.first) > 0;
      if (m == "isdisjoint" ? in : !in)
        return QuantumValue(false);
    }
    return QuantumValue(true);
  }
  if (m == "clear") {
    dict->clear();
    return QuantumValue();
  }
  if (m == "size" || m == "length")
    return QuantumValue((double)dict->size());
  // Python dict.update(other) — merge keys from another dict; on a set,
  // set.update(iterable) adds each element.
  if (m == "update") {
    for (auto &a : args) {
      if (a.isDict())
        for (auto &kv : *a.asDict())
          (*dict)[kv.first] = kv.second;
      else if (a.isArray())
        for (auto &v : *a.asArray())
          (*dict)[v.toString()] = QuantumValue(true);
    }
    return QuantumValue();
  }
  // Python dict.pop(key[, default])
  if (m == "pop") {
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
      m == "with_index" || m == "sort" || m == "min" || m == "max" ||
      m == "first") {
    auto pairs = std::make_shared<Array>();
    for (auto &[k, v] : *dict) {
      auto pair = std::make_shared<Array>();
      pair->push_back(QuantumValue(k));
      pair->push_back(v);
      pairs->push_back(QuantumValue(pair));
    }
    std::string target = (m == "each_pair") ? "each" : m;
    return callArrayMethod(pairs, target, args);
  }
  if (m == "each_key" || m == "each_value") {
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
