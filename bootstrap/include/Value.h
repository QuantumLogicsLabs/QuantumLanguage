#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <functional>
#include <variant>
#include <stdexcept>

class Environment;

// ─── Value Types ──────────────────────────────────────────────────────────────

struct QuantumNil
{
};

struct Closure;
struct QuantumClass;
struct QuantumInstance;
struct QuantumBoundMethod;

using QuantumNativeFunc = std::function<struct QuantumValue(std::vector<struct QuantumValue>)>;

struct QuantumNative
{
    std::string name;
    QuantumNativeFunc fn;
};

struct QuantumValue;

using Array = std::vector<QuantumValue>;
class Dict; // insertion-ordered string → value map, defined below

// ─── Pointer Type ─────────────────────────────────────────────────────────────

struct QuantumPointer
{
    std::shared_ptr<QuantumValue> cell; // live reference to variable storage
    std::string varName;                // for display/debug
    int offset = 0;                     // for pointer arithmetic

    QuantumPointer() : cell(nullptr), varName(""), offset(0) {}

    bool isNull() const { return cell == nullptr; }

    QuantumValue &deref() const
    {
        if (!cell)
            throw std::runtime_error("Null pointer dereference");
        return *cell;
    }
};

struct QuantumValue
{
    using Data = std::variant<
        QuantumNil,
        bool,
        double,
        std::string,
        std::shared_ptr<Array>,
        std::shared_ptr<Dict>,
        std::shared_ptr<Closure>,
        std::shared_ptr<QuantumNative>,
        std::shared_ptr<QuantumInstance>,
        std::shared_ptr<QuantumClass>,
        std::shared_ptr<QuantumBoundMethod>,
        std::shared_ptr<QuantumPointer>,
        // An exact 64-bit integer: a Java `long` (src/java/). Ordinary numbers
        // are doubles, exact only up to 2^53, which `long` math goes past.
        int64_t>;

    Data data;

    // Constructors
    QuantumValue() : data(QuantumNil{}) {}
    explicit QuantumValue(bool b) : data(b) {}
    explicit QuantumValue(double d) : data(d) {}
    explicit QuantumValue(const std::string &s) : data(s) {}
    explicit QuantumValue(std::string &&s) : data(std::move(s)) {}
    explicit QuantumValue(std::shared_ptr<Array> a) : data(std::move(a)) {}
    explicit QuantumValue(std::shared_ptr<Dict> d) : data(std::move(d)) {}
    explicit QuantumValue(std::shared_ptr<Closure> f) : data(std::move(f)) {}
    explicit QuantumValue(std::shared_ptr<QuantumNative> n) : data(std::move(n)) {}
    explicit QuantumValue(std::shared_ptr<QuantumInstance> i) : data(std::move(i)) {}
    explicit QuantumValue(std::shared_ptr<QuantumClass> c) : data(std::move(c)) {}
    explicit QuantumValue(std::shared_ptr<QuantumBoundMethod> bm) : data(std::move(bm)) {}
    explicit QuantumValue(std::shared_ptr<QuantumPointer> p) : data(std::move(p)) {}
    // No constructor from integers (an int would be ambiguous with bool and
    // double): 64-bit integers are made explicitly.
    static QuantumValue ofLong(int64_t v)
    {
        QuantumValue q;
        q.data = v;
        return q;
    }

    // Type checks
    bool isNil() const { return std::holds_alternative<QuantumNil>(data); }
    bool isBool() const { return std::holds_alternative<bool>(data); }
    bool isNumber() const { return std::holds_alternative<double>(data); }
    bool isString() const { return std::holds_alternative<std::string>(data); }
    bool isArray() const { return std::holds_alternative<std::shared_ptr<Array>>(data); }
    bool isDict() const { return std::holds_alternative<std::shared_ptr<Dict>>(data); }
    bool isFunction() const { return std::holds_alternative<std::shared_ptr<Closure>>(data) || std::holds_alternative<std::shared_ptr<QuantumNative>>(data); }
    bool isClosure() const { return std::holds_alternative<std::shared_ptr<Closure>>(data); }
    bool isInstance() const { return std::holds_alternative<std::shared_ptr<QuantumInstance>>(data); }
    bool isClass() const { return std::holds_alternative<std::shared_ptr<QuantumClass>>(data); }
    bool isBoundMethod() const { return std::holds_alternative<std::shared_ptr<QuantumBoundMethod>>(data); }
    bool isPointer() const { return std::holds_alternative<std::shared_ptr<QuantumPointer>>(data); }
    bool isLong() const { return std::holds_alternative<int64_t>(data); }

    // Accessors
    bool asBool() const { return std::get<bool>(data); }
    double asNumber() const { return std::get<double>(data); }
    std::string asString() const { return std::get<std::string>(data); }
    std::shared_ptr<Array> asArray() const { return std::get<std::shared_ptr<Array>>(data); }
    std::shared_ptr<Dict> asDict() const { return std::get<std::shared_ptr<Dict>>(data); }
    std::shared_ptr<Closure> asFunction() const { return std::get<std::shared_ptr<Closure>>(data); }
    std::shared_ptr<QuantumInstance> asInstance() const { return std::get<std::shared_ptr<QuantumInstance>>(data); }
    std::shared_ptr<QuantumClass> asClass() const { return std::get<std::shared_ptr<QuantumClass>>(data); }
    std::shared_ptr<QuantumBoundMethod> asBoundMethod() const { return std::get<std::shared_ptr<QuantumBoundMethod>>(data); }
    std::shared_ptr<QuantumPointer> asPointer() const { return std::get<std::shared_ptr<QuantumPointer>>(data); }
    int64_t asLong() const { return std::get<int64_t>(data); }

    bool isNative() const;
    std::shared_ptr<QuantumNative> asNative() const;

    bool isTruthy() const;
    std::string toString() const;
    std::string typeName() const;
};

// ─── Dict ─────────────────────────────────────────────────────────────────────
// Python dicts, JS objects and Ruby hashes all iterate in insertion order, so
// the map behind them does too: entries are kept in order, with a
// hash index for lookup. The interface is the subset of std::unordered_map
// the VM uses. Entries live in a deque so a reference from operator[] stays
// valid across later insertions, as with unordered_map; erasing shifts later
// entries (dict deletes are rare next to reads and inserts).
class Dict
{
public:
    using value_type = std::pair<std::string, QuantumValue>;
    using iterator = std::deque<value_type>::iterator;
    using const_iterator = std::deque<value_type>::const_iterator;

    Dict() = default;

    iterator begin() { return entries_.begin(); }
    iterator end() { return entries_.end(); }
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }
    size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }

    iterator find(const std::string &key)
    {
        auto it = index_.find(key);
        return it == index_.end() ? entries_.end() : entries_.begin() + it->second;
    }
    const_iterator find(const std::string &key) const
    {
        auto it = index_.find(key);
        return it == index_.end() ? entries_.end() : entries_.begin() + it->second;
    }
    size_t count(const std::string &key) const { return index_.count(key); }

    QuantumValue &operator[](const std::string &key)
    {
        auto it = index_.find(key);
        if (it != index_.end())
            return entries_[it->second].second;
        index_.emplace(key, entries_.size());
        entries_.emplace_back(key, QuantumValue());
        return entries_.back().second;
    }
    QuantumValue &at(const std::string &key)
    {
        auto it = index_.find(key);
        if (it == index_.end())
            throw std::out_of_range("Dict::at: missing key '" + key + "'");
        return entries_[it->second].second;
    }
    const QuantumValue &at(const std::string &key) const
    {
        auto it = index_.find(key);
        if (it == index_.end())
            throw std::out_of_range("Dict::at: missing key '" + key + "'");
        return entries_[it->second].second;
    }

    std::pair<iterator, bool> insert(const value_type &kv)
    {
        auto it = find(kv.first);
        if (it != end())
            return {it, false};
        index_.emplace(kv.first, entries_.size());
        entries_.push_back(kv);
        return {entries_.end() - 1, true};
    }
    template <class V>
    std::pair<iterator, bool> emplace(const std::string &key, V &&value)
    {
        auto it = find(key);
        if (it != end())
            return {it, false};
        index_.emplace(key, entries_.size());
        entries_.emplace_back(key, QuantumValue(std::forward<V>(value)));
        return {entries_.end() - 1, true};
    }

    size_t erase(const std::string &key)
    {
        auto it = index_.find(key);
        if (it == index_.end())
            return 0;
        erase(entries_.begin() + it->second);
        return 1;
    }
    iterator erase(const_iterator pos)
    {
        size_t at = static_cast<size_t>(pos - entries_.cbegin());
        index_.erase(entries_[at].first);
        auto next = entries_.erase(entries_.begin() + at);
        for (size_t i = at; i < entries_.size(); ++i)
            index_[entries_[i].first] = i;
        return next;
    }
    void clear()
    {
        entries_.clear();
        index_.clear();
    }
    void reserve(size_t n) { index_.reserve(n); }

private:
    std::deque<value_type> entries_; // deque: references survive insertion
    std::unordered_map<std::string, size_t> index_;
};

// Python display conventions for the running program (a .py source; set by
// the __python_repr__ native the compiler emits first): True / False / None,
// and 'single-quoted' strings inside printed lists and dicts.
extern bool g_pythonRepr;

// How a tagged JS runtime dict (Date, URL, URLSearchParams) prints; sets
// `handled` to false for an ordinary dict. Defined in VmJsRuntime.cpp.
std::string jsDisplayString(const std::shared_ptr<Dict> &d, bool &handled);

// ─── Environment ──────────────────────────────────────────────────────────────

class Environment : public std::enable_shared_from_this<Environment>
{
public:
    explicit Environment(std::shared_ptr<Environment> parent = nullptr);

    void define(const std::string &name, QuantumValue val, bool isConst = false);
    // defineRef: bind a parameter name directly to a shared cell (pass-by-reference)
    void defineRef(const std::string &name, std::shared_ptr<QuantumValue> cell);
    QuantumValue get(const std::string &name) const;
    void set(const std::string &name, QuantumValue val);
    bool has(const std::string &name) const;
    const std::unordered_map<std::string, QuantumValue> &getVars() const { return vars; }

    // Pointer support: get a shared cell for a variable so &var returns a live reference
    std::shared_ptr<QuantumValue> getCell(const std::string &name);

    // The storage of `name` in this environment (not its parents), or null.
    // Bindings are never removed, so the pointer stays valid; null too when a
    // pointer cell (getCell / defineRef) holds the live value instead.
    QuantumValue *slot(const std::string &name)
    {
        if (!cells.empty() && cells.count(name))
            return nullptr;
        auto it = vars.find(name);
        return it == vars.end() ? nullptr : &it->second;
    }

    std::shared_ptr<Environment> parent;

private:
    std::unordered_map<std::string, QuantumValue> vars;
    std::unordered_map<std::string, bool> constants;
    // Shared cells — created on first &var, keeps pointer alive
    std::unordered_map<std::string, std::shared_ptr<QuantumValue>> cells;
};

// ─── Class & Instance ────────────────────────────────────────────────────────

// BIND_METHOD name prefix marking a per-instance field initializer (a C++
// member object such as `vector<int> v;`), stored in QuantumClass::fieldInits.
inline const std::string kFieldInitPrefix = "__fieldinit__:";
// BIND_METHOD name prefix marking a `static` member: a method goes to
// QuantumClass::staticMethods, a data member to staticFields + staticNames.
inline const std::string kStaticPrefix = "__static__:";

struct QuantumClass
{
    std::string name;
    std::shared_ptr<QuantumClass> base;
    std::unordered_map<std::string, std::shared_ptr<Closure>> methods;
    std::unordered_map<std::string, std::shared_ptr<Closure>> staticMethods;
    std::unordered_map<std::string, QuantumValue> staticFields;
    // (field, fn(self) -> initial value), run in declaration order for every
    // new instance before its constructor.
    std::vector<std::pair<std::string, std::shared_ptr<Closure>>> fieldInits;
    // staticFields entries that are C++ `static` data members (shared by all
    // instances) rather than default values of per-instance fields.
    std::unordered_set<std::string> staticNames;
};

struct QuantumInstance
{
    std::shared_ptr<QuantumClass> klass;
    std::unordered_map<std::string, QuantumValue> fields;
    std::shared_ptr<Environment> env;

    QuantumValue getField(const std::string &name) const;
    void setField(const std::string &name, QuantumValue val);
};

struct QuantumBoundMethod
{
    std::shared_ptr<Closure> method;
    QuantumValue self;
};

// ─── Control Flow Signals ────────────────────────────────────────────────────

struct ReturnSignal
{
    QuantumValue value;
    explicit ReturnSignal(QuantumValue v) : value(std::move(v)) {}
};

struct BreakSignal
{
};
struct ContinueSignal
{
};