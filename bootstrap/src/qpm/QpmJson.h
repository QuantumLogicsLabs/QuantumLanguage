#pragma once
// Minimal self-contained JSON value type + parser/serializer for qpm.
// Used for npm registry responses, package.json, and qpm-lock.json.
// Objects preserve insertion order (so package.json round-trips predictably).

#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>

namespace qpm
{

    class JsonValue
    {
    public:
        enum class Type
        {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object
        };

        using Array = std::vector<JsonValue>;
        using Member = std::pair<std::string, JsonValue>;
        using Object = std::vector<Member>;

        JsonValue() : type_(Type::Null) {}
        JsonValue(std::nullptr_t) : type_(Type::Null) {}
        JsonValue(bool b) : type_(Type::Bool), bool_(b) {}
        JsonValue(double n) : type_(Type::Number), num_(n) {}
        JsonValue(int n) : type_(Type::Number), num_(n) {}
        JsonValue(const std::string &s) : type_(Type::String), str_(s) {}
        JsonValue(const char *s) : type_(Type::String), str_(s) {}

        static JsonValue makeArray() { JsonValue v; v.type_ = Type::Array; return v; }
        static JsonValue makeObject() { JsonValue v; v.type_ = Type::Object; return v; }

        Type type() const { return type_; }
        bool isNull() const { return type_ == Type::Null; }
        bool isBool() const { return type_ == Type::Bool; }
        bool isNumber() const { return type_ == Type::Number; }
        bool isString() const { return type_ == Type::String; }
        bool isArray() const { return type_ == Type::Array; }
        bool isObject() const { return type_ == Type::Object; }

        bool asBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
        double asNumber(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
        const std::string &asString() const
        {
            static const std::string empty;
            return type_ == Type::String ? str_ : empty;
        }
        std::string asString(const std::string &def) const { return type_ == Type::String ? str_ : def; }

        const Array &array() const { return arr_; }
        Array &array() { type_ = Type::Array; return arr_; }
        const Object &object() const { return obj_; }
        Object &object() { type_ = Type::Object; return obj_; }

        // Object lookup — returns nullptr if not an object or key absent.
        const JsonValue *find(const std::string &key) const
        {
            if (type_ != Type::Object)
                return nullptr;
            for (const auto &m : obj_)
                if (m.first == key)
                    return &m.second;
            return nullptr;
        }

        // Object lookup with a default; safe to call on any type.
        JsonValue get(const std::string &key, const JsonValue &def = JsonValue()) const
        {
            const JsonValue *v = find(key);
            return v ? *v : def;
        }

        bool has(const std::string &key) const { return find(key) != nullptr; }

        // Insert-or-replace a member (object types only; converts Null->Object).
        void set(const std::string &key, JsonValue value)
        {
            if (type_ == Type::Null)
                type_ = Type::Object;
            for (auto &m : obj_)
            {
                if (m.first == key)
                {
                    m.second = std::move(value);
                    return;
                }
            }
            obj_.emplace_back(key, std::move(value));
        }

        void push(JsonValue value)
        {
            if (type_ == Type::Null)
                type_ = Type::Array;
            arr_.push_back(std::move(value));
        }

        void appendTo(std::string &out, int indent, int depth) const;
        std::string stringify(int indent = 0) const
        {
            std::string out;
            appendTo(out, indent, 0);
            return out;
        }

        // Throws std::runtime_error with a position-annotated message on bad input.
        static JsonValue parse(const std::string &text);

    private:
        Type type_;
        bool bool_ = false;
        double num_ = 0.0;
        std::string str_;
        Array arr_;
        Object obj_;
    };

} // namespace qpm
