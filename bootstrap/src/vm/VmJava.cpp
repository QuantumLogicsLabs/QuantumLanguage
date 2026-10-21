// Runtime of the Java front-end (src/java/): the J_* opcodes, dispatch on
// receivers that are not objects, Java exceptions and stack traces, classes
// and instances, console input/output. String / number / Math / Arrays
// library natives live in VmJavaLib.cpp.
#include "Vm.h"
#include "VmJava.h"
#include "JavaOps.h"
#include "Error.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string_view>
#include <thread>
#include <unordered_map>

extern std::vector<std::string> g_scriptArgv;

using QV = QuantumValue;

namespace
{
// class binary name -> source file, for stack traces
std::unordered_map<std::string, std::string> g_javaSourceFiles;
}

// ─── Values ──────────────────────────────────────────────────────────────────

[[noreturn]] void jthrow(const std::string &cls, const std::string &msg) { throw QuantumError(cls, msg); }

int64_t jint(const QV &v)
{
    if (auto p = std::get_if<int64_t>(&v.data))
        return *p;
    if (auto d = std::get_if<double>(&v.data))
        return (int64_t)*d;
    if (auto b = std::get_if<bool>(&v.data))
        return *b ? 1 : 0;
    if (auto s = std::get_if<std::string>(&v.data))
        return s->empty() ? 0 : (int64_t)u16At(*s, 0);
    if (v.isNil())
        jthrow("java.lang.NullPointerException", "");
    return 0;
}

double jdouble(const QV &v)
{
    if (auto d = std::get_if<double>(&v.data))
        return *d;
    if (auto p = std::get_if<int64_t>(&v.data))
        return (double)*p;
    if (auto b = std::get_if<bool>(&v.data))
        return *b ? 1 : 0;
    if (auto s = std::get_if<std::string>(&v.data))
        return s->empty() ? 0 : (double)u16At(*s, 0);
    if (v.isNil())
        jthrow("java.lang.NullPointerException", "");
    return 0;
}

// ─── Strings (UTF-16 semantics on UTF-8 storage) ─────────────────────────────

bool isAscii(const std::string &s)
{
    for (unsigned char c : s)
        if (c >= 0x80)
            return false;
    return true;
}

std::u16string toU16(const std::string &s)
{
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80)
        {
            cp = c;
            n = 1;
        }
        else if ((c >> 5) == 6 && i + 1 < s.size())
        {
            cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
            n = 2;
        }
        else if ((c >> 4) == 14 && i + 2 < s.size())
        {
            cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            n = 3;
        }
        else if ((c >> 3) == 30 && i + 3 < s.size())
        {
            cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
            n = 4;
        }
        else
        {
            cp = 0xFFFD;
            n = 1;
        }
        i += n;
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            out += (char16_t)(0xD800 + (cp >> 10));
            out += (char16_t)(0xDC00 + (cp & 0x3FF));
        }
        else
            out += (char16_t)cp;
    }
    return out;
}

std::string unitToUtf8(uint32_t u)
{
    std::string out;
    if (u < 0x80)
        out += (char)u;
    else if (u < 0x800)
    {
        out += (char)(0xC0 | (u >> 6));
        out += (char)(0x80 | (u & 0x3F));
    }
    else
    {
        // includes lone surrogates (WTF-8), which a char can hold
        out += (char)(0xE0 | ((u >> 12) & 0x0F));
        out += (char)(0x80 | ((u >> 6) & 0x3F));
        out += (char)(0x80 | (u & 0x3F));
    }
    return out;
}

std::string fromU16(const std::u16string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        uint32_t u = s[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
        {
            uint32_t cp = 0x10000 + ((u - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
            ++i;
        }
        else
            out += unitToUtf8(u);
    }
    return out;
}

int64_t u16Length(const std::string &s)
{
    int64_t n = 0;
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80)
        {
            ++n;
            ++i;
        }
        else if ((c >> 5) == 6)
        {
            ++n;
            i += 2;
        }
        else if ((c >> 4) == 14)
        {
            ++n;
            i += 3;
        }
        else if ((c >> 3) == 30)
        {
            n += 2;
            i += 4;
        }
        else
        {
            ++n;
            ++i;
        }
    }
    return n;
}

uint32_t u16At(const std::string &s, int64_t idx)
{
    if (idx >= 0 && (size_t)idx < s.size() && isAscii(s))
        return (unsigned char)s[(size_t)idx];
    std::u16string u = toU16(s);
    if (idx < 0 || idx >= (int64_t)u.size())
        jthrow("java.lang.StringIndexOutOfBoundsException",
               "Index " + std::to_string(idx) + " out of bounds for length " + std::to_string(u.size()));
    return u[(size_t)idx];
}

std::string u16Substr(const std::string &s, int64_t begin, int64_t end)
{
    bool ascii = isAscii(s);
    int64_t len = ascii ? (int64_t)s.size() : u16Length(s);
    if (begin < 0 || end > len || begin > end)
        jthrow("java.lang.StringIndexOutOfBoundsException",
               "Range [" + std::to_string(begin) + ", " + std::to_string(end) + ") out of bounds for length " +
                   std::to_string(len));
    if (ascii)
        return s.substr((size_t)begin, (size_t)(end - begin));
    std::u16string u = toU16(s);
    return fromU16(u.substr((size_t)begin, (size_t)(end - begin)));
}

// ─── Exceptions ──────────────────────────────────────────────────────────────

QV javaErrorDescriptor(const QuantumError &e, const std::vector<std::string> &trace)
{
    std::string kind = e.kind;
    std::string msg = e.what();
    if (kind.find('.') == std::string::npos)
    {
        // an error of the VM itself (a gap in the runtime, not Java semantics)
        if (kind == "IndexError")
            kind = "java.lang.IndexOutOfBoundsException";
        else
        {
            msg = kind + ": " + msg;
            kind = "java.lang.RuntimeException";
        }
    }
    std::string d = "\x1fJ" + kind + "\x1f" + msg + "\x1f";
    for (size_t i = 0; i < trace.size(); ++i)
        d += (i ? "\n" : "") + trace[i];
    return QV(d);
}

std::vector<std::string> VM::javaStackTrace() const
{
    std::vector<std::string> out;
    for (size_t k = frames_.size(); k-- > 0 && out.size() < 1024;)
    {
        const CallFrame &f = frames_[k];
        std::string name = f.closure->chunk->name;
        size_t hash = name.find('#');
        if (hash != std::string::npos)
            name = name.substr(0, hash);
        size_t dot = name.rfind('.');
        if (dot == std::string::npos || dot == 0 || name[0] == '<')
            continue; // the script itself, not a Java method
        std::string cls = name.substr(0, dot);
        const auto &code = f.closure->chunk->code;
        int line = 0;
        size_t ip = f.ip ? f.ip - 1 : 0;
        if (ip < code.size())
            line = code[ip].line;
        std::string file = "Unknown Source";
        auto it = g_javaSourceFiles.find(cls);
        if (it != g_javaSourceFiles.end())
            file = it->second;
        out.push_back((name.rfind("java.", 0) == 0 ? "java.base/" : "") + name + "(" + file +
                      (line > 0 ? ":" + std::to_string(line) : std::string()) + ")");
    }
    return out;
}

// ─── Opcodes ─────────────────────────────────────────────────────────────────

namespace
{
const std::string &runtimeClassName(const QV &v);

[[noreturn]] void npe(const std::string &msg) { jthrow("java.lang.NullPointerException", msg); }

int64_t d2l(double d)
{
    if (std::isnan(d))
        return 0;
    if (d >= 9223372036854775807.0)
        return INT64_MAX;
    if (d <= -9223372036854775808.0)
        return INT64_MIN;
    return (int64_t)d;
}

int64_t d2i(double d)
{
    if (std::isnan(d))
        return 0;
    if (d >= 2147483647.0)
        return 2147483647;
    if (d <= -2147483648.0)
        return -2147483648LL;
    return (int64_t)d;
}

bool numericBoth(const QV &a, const QV &b, int64_t &x, int64_t &y)
{
    auto pa = std::get_if<int64_t>(&a.data);
    auto pb = std::get_if<int64_t>(&b.data);
    if (pa && pb)
    {
        x = *pa;
        y = *pb;
        return true;
    }
    return false;
}

bool sameRef(const QV &a, const QV &b)
{
    if (a.data.index() != b.data.index())
        return false;
    if (a.isNil())
        return true;
    if (auto p = std::get_if<int64_t>(&a.data))
        return *p == std::get<int64_t>(b.data);
    if (auto d = std::get_if<double>(&a.data))
        return *d == std::get<double>(b.data);
    if (auto s = std::get_if<std::string>(&a.data))
        return *s == std::get<std::string>(b.data);
    if (a.isBool())
        return a.asBool() == b.asBool();
    if (a.isInstance())
        return a.asInstance() == b.asInstance();
    if (a.isArray())
        return a.asArray() == b.asArray();
    if (a.isClosure())
        return a.asFunction() == b.asFunction();
    if (a.isClass())
        return a.asClass() == b.asClass();
    if (a.isNative())
        return a.asNative() == b.asNative();
    return false;
}

std::string identityHex(const void *p)
{
    char b[24];
    std::snprintf(b, sizeof b, "%x", (unsigned)((reinterpret_cast<uintptr_t>(p) >> 4) * 2654435761u));
    return b;
}
} // namespace

QV javaBin(int op, const QV &a, const QV &b)
{
    using namespace jop;
    switch (op)
    {
    case IADD:
        return jlong(wrap32(jint(a) + jint(b)));
    case ISUB:
        return jlong(wrap32(jint(a) - jint(b)));
    case IMUL:
        return jlong(wrap32((int64_t)((uint64_t)jint(a) * (uint64_t)jint(b))));
    case IDIV:
    {
        int64_t y = jint(b);
        if (y == 0)
            jthrow("java.lang.ArithmeticException", "/ by zero");
        return jlong(wrap32(jint(a) / y));
    }
    case IREM:
    {
        int64_t y = jint(b);
        if (y == 0)
            jthrow("java.lang.ArithmeticException", "/ by zero");
        return jlong(jint(a) % y);
    }
    case ISHL:
        return jlong(wrap32((int64_t)((uint32_t)jint(a) << (jint(b) & 31))));
    case ISHR:
        return jlong((int32_t)jint(a) >> (jint(b) & 31));
    case IUSHR:
        return jlong(wrap32((int64_t)((uint32_t)(int32_t)jint(a) >> (jint(b) & 31))));
    case IAND:
        return jlong(jint(a) & jint(b));
    case IOR:
        return jlong(jint(a) | jint(b));
    case IXOR:
        return jlong(jint(a) ^ jint(b));
    case LADD:
        return jlong((int64_t)((uint64_t)jint(a) + (uint64_t)jint(b)));
    case LSUB:
        return jlong((int64_t)((uint64_t)jint(a) - (uint64_t)jint(b)));
    case LMUL:
        return jlong((int64_t)((uint64_t)jint(a) * (uint64_t)jint(b)));
    case LDIV:
    {
        int64_t x = jint(a), y = jint(b);
        if (y == 0)
            jthrow("java.lang.ArithmeticException", "/ by zero");
        if (x == INT64_MIN && y == -1)
            return jlong(INT64_MIN);
        return jlong(x / y);
    }
    case LREM:
    {
        int64_t x = jint(a), y = jint(b);
        if (y == 0)
            jthrow("java.lang.ArithmeticException", "/ by zero");
        if (y == -1)
            return jlong(0);
        return jlong(x % y);
    }
    case LSHL:
        return jlong((int64_t)((uint64_t)jint(a) << (jint(b) & 63)));
    case LSHR:
        return jlong(jint(a) >> (jint(b) & 63));
    case LUSHR:
        return jlong((int64_t)((uint64_t)jint(a) >> (jint(b) & 63)));
    case LAND:
        return jlong(jint(a) & jint(b));
    case LOR:
        return jlong(jint(a) | jint(b));
    case LXOR:
        return jlong(jint(a) ^ jint(b));
    case FADD:
        return QV((double)(float)(jdouble(a) + jdouble(b)));
    case FSUB:
        return QV((double)(float)(jdouble(a) - jdouble(b)));
    case FMUL:
        return QV((double)(float)(jdouble(a) * jdouble(b)));
    case FDIV:
        return QV((double)(float)(jdouble(a) / jdouble(b)));
    case FREM:
        return QV((double)(float)std::fmod(jdouble(a), jdouble(b)));
    case DADD:
        return QV(jdouble(a) + jdouble(b));
    case DSUB:
        return QV(jdouble(a) - jdouble(b));
    case DMUL:
        return QV(jdouble(a) * jdouble(b));
    case DDIV:
        return QV(jdouble(a) / jdouble(b));
    case DREM:
        return QV(std::fmod(jdouble(a), jdouble(b)));
    case LT:
    case LE:
    case GT:
    case GE:
    case EQ:
    case NE:
    {
        int64_t x, y;
        if (numericBoth(a, b, x, y))
        {
            switch (op)
            {
            case LT: return QV(x < y);
            case LE: return QV(x <= y);
            case GT: return QV(x > y);
            case GE: return QV(x >= y);
            case EQ: return QV(x == y);
            default: return QV(x != y);
            }
        }
        if (a.isBool() && b.isBool())
            return QV(op == EQ ? a.asBool() == b.asBool() : a.asBool() != b.asBool());
        double dx = jdouble(a), dy = jdouble(b);
        switch (op)
        {
        case LT: return QV(dx < dy);
        case LE: return QV(dx <= dy);
        case GT: return QV(dx > dy);
        case GE: return QV(dx >= dy);
        case EQ: return QV(dx == dy);
        default: return QV(dx != dy);
        }
    }
    case REFEQ:
        return QV(sameRef(a, b));
    case REFNE:
        return QV(!sameRef(a, b));
    case BOXEQ:
    case BOXNE:
    {
        // two boxes are the same object when both are null, the same Boolean,
        // or an equal value from the valueOf caches
        bool same;
        if (a.isNil() || b.isNil())
            same = a.isNil() && b.isNil();
        else if (a.isBool() && b.isBool())
            same = a.asBool() == b.asBool();
        else if (a.isLong() && b.isLong())
            same = a.asLong() == b.asLong() && a.asLong() >= -128 && a.asLong() <= 127;
        else if (a.isInstance() && b.isInstance())
            same = a.asInstance() == b.asInstance();
        else if (a.isString() && b.isString())
            same = a.asString() == b.asString() && a.asString().size() == 1 && (unsigned char)a.asString()[0] < 128;
        else
            same = false;
        return QV(op == BOXEQ ? same : !same);
    }
    case SCAT:
    {
        const std::string *x = std::get_if<std::string>(&a.data);
        const std::string *y = std::get_if<std::string>(&b.data);
        std::string r;
        r.reserve((x ? x->size() : 4) + (y ? y->size() : 4));
        r += x ? *x : javaUn(VSTR, a).asString();
        r += y ? *y : javaUn(VSTR, b).asString();
        return QV(std::move(r));
    }
    case CHARAT:
    {
        if (a.isNil())
            npe("Cannot invoke \"String.charAt(int)\" because value is null");
        return jlong(u16At(a.asString(), jint(b)));
    }
    case BAND:
        return QV(a.isTruthy() && b.isTruthy());
    case BOR:
        return QV(a.isTruthy() || b.isTruthy());
    case BXOR:
        return QV(a.isTruthy() != b.isTruthy());
    default:
        throw RuntimeError("bad Java binary op " + std::to_string(op));
    }
}

QV javaUn(int op, const QV &v)
{
    using namespace jop;
    switch (op)
    {
    case INEG:
        return jlong(wrap32(-jint(v)));
    case LNEG:
        return jlong((int64_t)(0 - (uint64_t)jint(v)));
    case DNEG:
        return QV(-jdouble(v));
    case INOT:
    case LNOT:
        return jlong(~jint(v));
    case ZNOT:
        return QV(!v.isTruthy());
    case I2L:
        return jlong(jint(v));
    case L2I:
        return jlong(wrap32(jint(v)));
    case I2D:
        return QV((double)jint(v));
    case D2I:
        return jlong(d2i(jdouble(v)));
    case D2L:
        if (v.isLong())
            return v; // already one (C/C++ conversions to a 64-bit integer)
        return jlong(d2l(jdouble(v)));
    case D2F:
        return QV((double)(float)jdouble(v));
    case I2F:
        return QV((double)(float)jint(v));
    case I2B:
        return jlong((int8_t)(uint8_t)(uint64_t)jint(v));
    case I2S:
        return jlong((int16_t)(uint16_t)(uint64_t)jint(v));
    case I2C:
        return jlong((uint16_t)(uint64_t)jint(v));
    case ISTR:
        return QV(std::to_string(jint(v)));
    case DSTR:
        return QV(javaDoubleToString(jdouble(v)));
    case FSTR:
        return QV(javaFloatToString(jdouble(v)));
    case CSTR:
        return QV(unitToUtf8((uint32_t)(jint(v) & 0xFFFF)));
    case ZSTR:
        return QV(std::string(v.isTruthy() ? "true" : "false"));
    case SSTR:
        if (v.isNil())
            return QV(std::string("null"));
        return v.isString() ? v : javaUn(VSTR, v);
    case VSTR:
    {
        if (v.isNil())
            return QV(std::string("null"));
        if (v.isString())
            return v;
        if (auto p = std::get_if<int64_t>(&v.data))
            return QV(std::to_string(*p));
        if (auto d = std::get_if<double>(&v.data))
            return QV(javaDoubleToString(*d));
        if (v.isBool())
            return QV(std::string(v.asBool() ? "true" : "false"));
        if (v.isArray())
            return QV("[Ljava.lang.Object;@" + identityHex(v.asArray().get()));
        if (v.isInstance())
            return QV(v.asInstance()->klass->name + "@" + identityHex(v.asInstance().get()));
        if (v.isClosure())
            return QV("Lambda@" + identityHex(v.asFunction().get()));
        return QV(v.toString());
    }
    case ALEN:
        if (v.isArray())
            return jlong((int64_t)v.asArray()->size());
        npe("Cannot read the array length because value is null");
    case NONNULL:
        if (v.isNil())
            npe("");
        return v;
    case UNBOXN:
        if (v.isNil())
            npe("Cannot unbox a null value");
        if (v.isInstance())
        {
            auto it = v.asInstance()->fields.find("value");
            if (it != v.asInstance()->fields.end())
                return it->second;
        }
        return v;
    case UNBOXC:
        if (v.isNil())
            npe("Cannot unbox a null Character");
        if (v.isInstance())
        {
            auto it = v.asInstance()->fields.find("value");
            if (it != v.asInstance()->fields.end())
                return it->second;
        }
        return jlong(jint(v));
    case SLEN:
        if (v.isNil())
            npe("Cannot invoke \"String.length()\" because value is null");
        return jlong(isAscii(v.asString()) ? (int64_t)v.asString().size() : u16Length(v.asString()));
    default:
        throw RuntimeError("bad Java unary op " + std::to_string(op));
    }
}

QV javaGetField(const QV &obj, const std::string &name)
{
    if (auto inst = std::get_if<std::shared_ptr<QuantumInstance>>(&obj.data))
    {
        auto &f = (*inst)->fields;
        auto it = f.find(name);
        return it != f.end() ? it->second : QV();
    }
    if (obj.isNil())
        npe("Cannot read field \"" + name + "\" because value is null");
    throw RuntimeError("field " + name + " of a " + obj.typeName());
}

void javaPutField(const QV &obj, const std::string &name, const QV &v)
{
    if (auto inst = std::get_if<std::shared_ptr<QuantumInstance>>(&obj.data))
    {
        (*inst)->fields[name] = v;
        return;
    }
    if (obj.isNil())
        npe("Cannot assign field \"" + name + "\" because value is null");
    throw RuntimeError("field " + name + " of a " + obj.typeName());
}

QV javaArrayLoad(const QV &arr, const QV &idx)
{
    auto a = std::get_if<std::shared_ptr<Array>>(&arr.data);
    if (!a)
    {
        if (arr.isNil())
            npe("Cannot load from array because value is null");
        throw RuntimeError("indexing a " + arr.typeName());
    }
    int64_t i = jint(idx);
    if (i < 0 || i >= (int64_t)(*a)->size())
        jthrow("java.lang.ArrayIndexOutOfBoundsException",
               "Index " + std::to_string(i) + " out of bounds for length " + std::to_string((*a)->size()));
    return (**a)[(size_t)i];
}

void javaArrayStore(const QV &arr, const QV &idx, const QV &v)
{
    auto a = std::get_if<std::shared_ptr<Array>>(&arr.data);
    if (!a)
    {
        if (arr.isNil())
            npe("Cannot store to array because value is null");
        throw RuntimeError("indexing a " + arr.typeName());
    }
    int64_t i = jint(idx);
    if (i < 0 || i >= (int64_t)(*a)->size())
        jthrow("java.lang.ArrayIndexOutOfBoundsException",
               "Index " + std::to_string(i) + " out of bounds for length " + std::to_string((*a)->size()));
    (**a)[(size_t)i] = v;
}

// Type descriptors (from the front-end): "C:<binary name>" a class,
// "I:<binary name>" an interface, "[" an array type.
bool javaInstanceOf(const QV &v, const std::string &desc)
{
    if (v.isNil())
        return false;
    if (desc.empty() || desc[0] == '[')
        return v.isArray();
    const bool iface = desc[0] == 'I';
    const std::string name = desc.size() > 2 ? desc.substr(2) : desc;
    if (name == "java.lang.Object")
        return true;
    if (auto inst = std::get_if<std::shared_ptr<QuantumInstance>>(&v.data))
    {
        auto &k = (*inst)->klass;
        auto it = k->staticFields.find("$supers");
        if (it != k->staticFields.end() && it->second.isDict())
            return it->second.asDict()->count(name) > 0;
        for (auto *c = k.get(); c; c = c->base.get())
            if (c->name == name)
                return true;
        return false;
    }
    if (v.isClosure() || v.isNative())
        return iface; // a lambda: assume it implements the interface asked about
    if (v.isLong())
        return name == "java.lang.Integer" || name == "java.lang.Number" || name == "java.lang.Comparable" ||
               name == "java.io.Serializable" || name == "java.lang.constant.Constable";
    if (v.isNumber())
        return name == "java.lang.Double" || name == "java.lang.Float" || name == "java.lang.Number" ||
               name == "java.lang.Comparable" || name == "java.io.Serializable";
    if (v.isBool())
        return name == "java.lang.Boolean" || name == "java.lang.Comparable" || name == "java.io.Serializable";
    if (v.isString())
        return name == "java.lang.String" || name == "java.lang.CharSequence" || name == "java.lang.Comparable" ||
               name == "java.io.Serializable" || name == "java.lang.constant.Constable";
    if (v.isArray())
        return name == "java.lang.Cloneable" || name == "java.io.Serializable";
    return false;
}

namespace
{
const std::string &runtimeClassName(const QV &v)
{
    static const std::string I = "java.lang.Integer", D = "java.lang.Double", B = "java.lang.Boolean",
                             S = "java.lang.String", A = "[Ljava.lang.Object;", L = "Lambda", N = "null";
    if (auto inst = std::get_if<std::shared_ptr<QuantumInstance>>(&v.data))
        return (*inst)->klass->name;
    if (v.isLong())
        return I;
    if (v.isNumber())
        return D;
    if (v.isBool())
        return B;
    if (v.isString())
        return S;
    if (v.isArray())
        return A;
    if (v.isNil())
        return N;
    return L;
}
} // namespace

void javaCheckCast(const QV &v, const std::string &desc)
{
    if (v.isNil() || javaInstanceOf(v, desc))
        return;
    std::string from = runtimeClassName(v);
    std::string to = desc.size() > 2 && desc[1] == ':' ? desc.substr(2) : desc;
    auto where = [](const std::string &n)
    { return n.rfind("java.", 0) == 0 ? std::string("module java.base of loader 'bootstrap'")
                                      : std::string("unnamed module of loader 'app'"); };
    std::string tail = where(from) == where(to)
                           ? from + " and " + to + " are in " + where(from)
                           : from + " is in " + where(from) + "; " + to + " is in " + where(to);
    jthrow("java.lang.ClassCastException", "class " + from + " cannot be cast to class " + to + " (" + tail + ")");
}

// ─── Dispatch ────────────────────────────────────────────────────────────────

void VM::eraseFromStack(size_t pos)
{
    stack_.erase(stack_.begin() + pos);
    for (auto &uv : openUpvalues_)
        if (uv->index > pos)
            --uv->index;
}

void VM::javaInvoke(const QV &desc, size_t recvIdx, int argc, int line, Chunk::InvokeCache *cache)
{
    if (cache && cache->klass)
        if (auto inst = std::get_if<std::shared_ptr<QuantumInstance>>(&stack_[recvIdx].data))
            if ((*inst)->klass.get() == cache->klass)
            {
                std::shared_ptr<Closure> fn = cache->method;
                stack_[recvIdx - 1] = QV(fn);
                callClosure(std::move(fn), argc + 1, line);
                return;
            }
    const std::string *key;
    const std::string *fallback = nullptr;
    if (desc.isString())
        key = &std::get<std::string>(desc.data);
    else
    {
        const Array &d = *desc.asArray();
        key = &std::get<std::string>(d[0].data);
        fallback = &std::get<std::string>(d[1].data);
    }
    QV &recv = stack_[recvIdx];
    if (auto inst = std::get_if<std::shared_ptr<QuantumInstance>>(&recv.data))
    {
        // "#str" (string conversion) is the object's toString()
        static const std::string toStringKey = "toString()";
        const std::string &lookup = *key == "#str" ? toStringKey : *key;
        for (QuantumClass *k = (*inst)->klass.get(); k; k = k->base.get())
        {
            auto it = k->methods.find(lookup);
            if (it != k->methods.end())
            {
                std::shared_ptr<Closure> fn = it->second;
                if (cache)
                {
                    cache->klass = (*inst)->klass.get();
                    cache->method = fn;
                }
                stack_[recvIdx - 1] = QV(fn);
                callClosure(fn, argc + 1, line);
                return;
            }
        }
        if (fallback)
        {
            QV fn = globals->get(*fallback);
            stack_[recvIdx - 1] = fn;
            callClosure(fn.asFunction(), argc + 1, line);
            return;
        }
        if (*key == "#str" || *key == "toString()")
        {
            QV s = javaUn(jop::VSTR, recv);
            for (int i = 0; i < argc + 2; ++i)
                stack_.pop_back();
            push(s);
            return;
        }
        jthrow("java.lang.AbstractMethodError", (*inst)->klass->name + "." + *key);
    }
    if (recv.isClosure())
    {
        if (fallback)
        {
            QV fn = globals->get(*fallback);
            stack_[recvIdx - 1] = fn;
            callClosure(fn.asFunction(), argc + 1, line);
            return;
        }
        if (*key != "#str" && key->rfind("toString(", 0) != 0 && key->rfind("equals(", 0) != 0 &&
            key->rfind("hashCode(", 0) != 0)
        {
            std::shared_ptr<Closure> fn = recv.asFunction();
            eraseFromStack(recvIdx);
            stack_[recvIdx - 1] = QV(fn);
            callClosure(fn, argc, line);
            return;
        }
    }
    if (recv.isNative() && !fallback)
    {
        auto fn = recv.asNative();
        std::vector<QV> args(stack_.end() - argc, stack_.end());
        for (int i = 0; i < argc + 2; ++i)
            stack_.pop_back();
        push(fn->fn(args));
        return;
    }
    if (recv.isNil())
    {
        if (*key == "#str")
        {
            for (int i = 0; i < argc + 2; ++i)
                stack_.pop_back();
            push(QV(std::string("null")));
            return;
        }
        std::string m = key->substr(0, key->find('('));
        npe("Cannot invoke \"" + m + "()\" because value is null");
    }
    QV r = recv;
    std::vector<QV> args(stack_.end() - argc, stack_.end());
    for (int i = 0; i < argc + 2; ++i)
        stack_.pop_back();
    push(javaValueInvoke(r, *key, args, line));
}

namespace
{
int64_t javaStringHash(const std::string &s)
{
    int32_t h = 0;
    if (isAscii(s))
        for (unsigned char c : s)
            h = (int32_t)((uint32_t)h * 31u + c);
    else
        for (char16_t c : toU16(s))
            h = (int32_t)((uint32_t)h * 31u + (uint32_t)c);
    return h;
}

int64_t doubleHash(double d)
{
    if (d == 0 && std::signbit(d))
        d = -0.0;
    uint64_t bits;
    if (std::isnan(d))
        bits = 0x7ff8000000000000ULL;
    else
        std::memcpy(&bits, &d, 8);
    return (int32_t)(uint32_t)(bits ^ (bits >> 32));
}

int doubleCompare(double a, double b)
{
    if (a < b)
        return -1;
    if (a > b)
        return 1;
    uint64_t x, y;
    double na = std::isnan(a) ? NAN : a, nb = std::isnan(b) ? NAN : b;
    std::memcpy(&x, &na, 8);
    std::memcpy(&y, &nb, 8);
    int64_t sx = (int64_t)x, sy = (int64_t)y;
    return sx == sy ? 0 : (sx < sy ? -1 : 1);
}
} // namespace

QV VM::javaValueInvoke(const QV &recv, const std::string &key, std::vector<QV> &args, int line)
{
    (void)line;
    if (key == "#str")
        return javaUn(jop::VSTR, recv);
    const std::string_view m = std::string_view(key).substr(0, key.find('('));
    if (m == "getClass")
    {
        QV fn = globals->get("java.lang.Class.$of");
        return invokeCallable(fn, {recv});
    }
    if (recv.isString())
    {
        if (m == "toString" || m == "intern")
            return recv;
        if (m == "charValue")
            return jlong(jint(recv));
        std::string native = "__jn_String_" + std::string(m);
        if (globals->has(native))
        {
            std::vector<QV> a;
            a.reserve(args.size() + 1);
            a.push_back(recv);
            a.insert(a.end(), args.begin(), args.end());
            return globals->get(native).asNative()->fn(a);
        }
    }
    else if (auto p = std::get_if<int64_t>(&recv.data))
    {
        int64_t x = *p;
        if (m == "toString")
            return QV(std::to_string(x));
        if (m == "hashCode")
            return jlong(x == wrap32(x) ? x : (int64_t)(int32_t)(uint32_t)((uint64_t)x ^ ((uint64_t)x >> 32)));
        if (m == "equals")
            return QV(!args.empty() && args[0].isLong() && args[0].asLong() == x);
        if (m == "compareTo" || m == "compare")
        {
            if (args.empty() || args[0].isNil())
                npe("");
            int64_t y = jint(args[0]);
            return jlong(x < y ? -1 : x > y ? 1 : 0);
        }
        if (m == "intValue")
            return jlong(wrap32(x));
        if (m == "longValue")
            return recv;
        if (m == "shortValue")
            return jlong((int16_t)x);
        if (m == "byteValue")
            return jlong((int8_t)x);
        if (m == "doubleValue")
            return QV((double)x);
        if (m == "floatValue")
            return QV((double)(float)x);
    }
    else if (auto d = std::get_if<double>(&recv.data))
    {
        double x = *d;
        if (m == "toString")
            return QV(javaDoubleToString(x));
        if (m == "hashCode")
            return jlong(doubleHash(x));
        if (m == "equals")
            return QV(!args.empty() && args[0].isNumber() && doubleCompare(x, args[0].asNumber()) == 0);
        if (m == "compareTo" || m == "compare")
        {
            if (args.empty() || args[0].isNil())
                npe("");
            return jlong(doubleCompare(x, jdouble(args[0])));
        }
        if (m == "intValue")
            return javaUn(jop::D2I, recv);
        if (m == "longValue")
            return javaUn(jop::D2L, recv);
        if (m == "doubleValue")
            return recv;
        if (m == "floatValue")
            return QV((double)(float)x);
        if (m == "isNaN")
            return QV(std::isnan(x));
        if (m == "isInfinite")
            return QV(std::isinf(x));
    }
    else if (recv.isBool())
    {
        bool x = recv.asBool();
        if (m == "toString")
            return QV(std::string(x ? "true" : "false"));
        if (m == "hashCode")
            return jlong(x ? 1231 : 1237);
        if (m == "equals")
            return QV(!args.empty() && args[0].isBool() && args[0].asBool() == x);
        if (m == "compareTo")
            return jlong(x == args[0].isTruthy() ? 0 : (x ? 1 : -1));
        if (m == "booleanValue")
            return recv;
    }
    else if (recv.isArray())
    {
        if (m == "clone")
            return QV(std::make_shared<Array>(*recv.asArray()));
        if (m == "equals")
            return QV(!args.empty() && args[0].isArray() && args[0].asArray() == recv.asArray());
        if (m == "hashCode")
            return jlong((int32_t)(reinterpret_cast<uintptr_t>(recv.asArray().get()) >> 4));
        if (m == "toString")
            return javaUn(jop::VSTR, recv);
    }
    else if (recv.isClosure() || recv.isNative())
    {
        if (m == "equals")
            return QV(!args.empty() && sameRef(recv, args[0]));
        if (m == "hashCode")
            return jlong((int32_t)(reinterpret_cast<uintptr_t>(recv.isClosure() ? (void *)recv.asFunction().get()
                                                                                 : (void *)recv.asNative().get()) >>
                                   4));
        if (m == "toString")
            return javaUn(jop::VSTR, recv);
    }
    if (m == "hashCode")
        return jlong(0);
    throw RuntimeError("no method " + key + " on a " + runtimeClassName(recv));
}

QV VM::javaCallFunctional(const QV &fn, const std::string &key, const std::vector<QV> &args)
{
    if (fn.isNil())
        npe("");
    if (fn.isClosure() || fn.isNative())
        return invokeCallable(fn, args);
    if (fn.isInstance())
    {
        for (QuantumClass *k = fn.asInstance()->klass.get(); k; k = k->base.get())
        {
            auto it = k->methods.find(key);
            if (it != k->methods.end())
            {
                auto bm = std::make_shared<QuantumBoundMethod>();
                bm->method = it->second;
                bm->self = fn;
                return invokeCallable(QV(bm), args);
            }
        }
    }
    throw RuntimeError("not callable as " + key);
}

std::string VM::javaToString(const QV &v)
{
    if (v.isInstance())
    {
        for (QuantumClass *k = v.asInstance()->klass.get(); k; k = k->base.get())
        {
            auto it = k->methods.find("toString()");
            if (it != k->methods.end())
            {
                auto bm = std::make_shared<QuantumBoundMethod>();
                bm->method = it->second;
                bm->self = v;
                QV r = invokeCallable(QV(bm), {});
                return r.isString() ? r.asString() : "null";
            }
        }
    }
    return javaUn(jop::VSTR, v).asString();
}

// ─── Console input ───────────────────────────────────────────────────────────
// One buffer behind every reader of System.in (Scanner, BufferedReader,
// System.in.read): filled a line at a time, so an interactive program sees
// input as it is typed.

namespace
{
struct JavaInput
{
    std::string buf;
    size_t pos = 0;
    bool eof = false;

    bool fill()
    {
        if (eof)
            return false;
        std::cout.flush();
        std::string line;
        if (!std::getline(std::cin, line))
        {
            eof = true;
            return false;
        }
        if (pos > 0 && pos == buf.size())
        {
            buf.clear();
            pos = 0;
        }
        buf += line;
        buf += '\n';
        return true;
    }
    static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
    // Moves past whitespace; false at end of input.
    bool skipSpace()
    {
        while (true)
        {
            while (pos < buf.size() && space(buf[pos]))
                ++pos;
            if (pos < buf.size())
                return true;
            if (!fill())
                return false;
        }
    }
    // The next token, without consuming it (empty at end of input).
    std::string peekToken()
    {
        if (!skipSpace())
            return "";
        size_t end = pos;
        while (true)
        {
            while (end < buf.size() && !space(buf[end]))
                ++end;
            if (end < buf.size() || eof || !fill())
                break;
        }
        return buf.substr(pos, end - pos);
    }
    bool nextToken(std::string &out)
    {
        out = peekToken();
        if (out.empty())
            return false;
        pos += out.size();
        return true;
    }
    bool hasLine()
    {
        if (pos < buf.size())
            return true;
        return fill();
    }
    bool nextLine(std::string &out)
    {
        if (pos >= buf.size() && !fill())
            return false;
        size_t nl = buf.find('\n', pos);
        if (nl == std::string::npos)
            nl = buf.size();
        out = buf.substr(pos, nl - pos);
        if (!out.empty() && out.back() == '\r')
            out.pop_back();
        pos = nl < buf.size() ? nl + 1 : nl;
        return true;
    }
    int read()
    {
        if (pos >= buf.size() && !fill())
            return -1;
        return (unsigned char)buf[pos++];
    }
};

JavaInput g_in;

// Console output is flushed at most every 50 ms while a program prints, so a
// watcher sees it promptly without a system call per line; reading input,
// writing to stderr and exiting flush it right away.
void javaFlushSoon()
{
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::milliseconds(50))
    {
        std::cout.flush();
        last = now;
    }
}
} // namespace

QV VM::javaBox(int op, const QV &v)
{
    if (!v.isLong())
        return v; // already a box (or null)
    static const char *names[4] = {"java.lang.Long", "java.lang.Short", "java.lang.Byte", "java.lang.Character"};
    int k = op - jop::BOXJ;
    if (!javaBoxClass_[k])
    {
        QV c = globals->get(names[k]);
        if (!c.isClass())
            return v; // the class is not part of the program: keep the integer
        javaBoxClass_[k] = c.asClass();
    }
    int64_t x = v.asLong();
    auto make = [&]()
    {
        auto inst = std::make_shared<QuantumInstance>();
        inst->klass = javaBoxClass_[k];
        inst->fields["value"] = v;
        return QV(inst);
    };
    if (x < -128 || x > 127 || (op == jop::BOXC && x < 0))
        return make();
    auto &cache = javaBoxCache_[k];
    if (cache.empty())
        cache.resize(256);
    QV &slot = cache[(size_t)(x + 128)];
    if (slot.isNil())
        slot = make();
    return slot;
}

QV VM::javaInvokeKey(const QV &recv, const std::string &key, std::vector<QV> args)
{
    if (recv.isInstance())
    {
        for (QuantumClass *k = recv.asInstance()->klass.get(); k; k = k->base.get())
        {
            auto it = k->methods.find(key);
            if (it != k->methods.end())
            {
                auto bm = std::make_shared<QuantumBoundMethod>();
                bm->method = it->second;
                bm->self = recv;
                return invokeCallable(QV(bm), args);
            }
        }
        jthrow("java.lang.AbstractMethodError", recv.asInstance()->klass->name + "." + key);
    }
    if (recv.isNil())
        npe("Cannot invoke \"" + key.substr(0, key.find('(')) + "()\" because value is null");
    return javaValueInvoke(recv, key, args, 0);
}

// ─── Natives ─────────────────────────────────────────────────────────────────

void registerJavaLibNatives(const std::function<void(const std::string &, QuantumNativeFunc)> &reg, VM *vm);

void VM::registerJavaNatives()
{
    auto reg = [this](const std::string &name, QuantumNativeFunc fn)
    {
        auto n = std::make_shared<QuantumNative>();
        n->name = name;
        n->fn = std::move(fn);
        globals->define(name, QuantumValue(n));
    };
    auto arg = [](const std::vector<QV> &a, size_t i) -> const QV &
    {
        static const QV nil;
        return i < a.size() ? a[i] : nil;
    };

    reg("__j_runtime", [this](std::vector<QV>) -> QV
        {
        javaMode_ = true;
        cppNumerics_ = true;
        maxSteps_ = INT64_MAX;
        return QV(); });

    // __j_class(name, base class | nil, [supertype names], {field: default}, file, simpleName)
    reg("__j_class", [arg](std::vector<QV> a) -> QV
        {
        auto k = std::make_shared<QuantumClass>();
        k->name = arg(a, 0).asString();
        if (arg(a, 1).isClass())
            k->base = arg(a, 1).asClass();
        auto supers = std::make_shared<Dict>();
        if (arg(a, 2).isArray())
            for (auto &s : *arg(a, 2).asArray())
                (*supers)[s.asString()] = QV(true);
        k->staticFields["$supers"] = QV(supers);
        // field defaults: own fields over the base class's
        auto defaults = std::make_shared<Dict>();
        if (k->base)
        {
            auto it = k->base->staticFields.find("$fields");
            if (it != k->base->staticFields.end())
                for (auto &kv : *it->second.asDict())
                    (*defaults)[kv.first] = kv.second;
        }
        if (arg(a, 3).isDict())
            for (auto &kv : *arg(a, 3).asDict())
                (*defaults)[kv.first] = kv.second;
        k->staticFields["$fields"] = QV(defaults);
        if (arg(a, 4).isString())
            g_javaSourceFiles[k->name] = arg(a, 4).asString();
        if (arg(a, 5).isString())
            k->staticFields["$simple"] = arg(a, 5);
        return QV(k); });

    reg("__j_bind", [arg](std::vector<QV> a) -> QV
        {
        if (arg(a, 0).isClass() && arg(a, 2).isClosure())
            arg(a, 0).asClass()->methods[arg(a, 1).asString()] = arg(a, 2).asFunction();
        return QV(); });

    reg("__j_new", [arg](std::vector<QV> a) -> QV
        {
        auto inst = std::make_shared<QuantumInstance>();
        inst->klass = arg(a, 0).asClass();
        auto it = inst->klass->staticFields.find("$fields");
        if (it != inst->klass->staticFields.end())
            for (auto &kv : *it->second.asDict())
                inst->fields.emplace(kv.first, kv.second);
        return QV(inst); });

    reg("__j_className", [arg](std::vector<QV> a) -> QV
        { return QV(runtimeClassName(arg(a, 0))); });
    reg("__j_simpleName", [arg](std::vector<QV> a) -> QV
        {
        const QV &v = arg(a, 0);
        if (v.isInstance())
        {
            auto it = v.asInstance()->klass->staticFields.find("$simple");
            if (it != v.asInstance()->klass->staticFields.end())
                return it->second;
        }
        std::string n = runtimeClassName(v);
        size_t dot = n.find_last_of(".$");
        return QV(dot == std::string::npos ? n : n.substr(dot + 1)); });
    reg("__j_isInstance", [arg](std::vector<QV> a) -> QV
        { return QV(javaInstanceOf(arg(a, 0), arg(a, 1).asString())); });
    reg("__j_identityHash", [arg](std::vector<QV> a) -> QV
        {
        const QV &v = arg(a, 0);
        const void *p = v.isInstance() ? (const void *)v.asInstance().get()
                        : v.isArray()  ? (const void *)v.asArray().get()
                        : v.isClosure() ? (const void *)v.asFunction().get()
                                        : nullptr;
        if (!p)
            return jlong(v.isNil() ? 0 : 7);
        return jlong((int32_t)(((reinterpret_cast<uintptr_t>(p) >> 4) * 2654435761u) & 0x7fffffff)); });

    reg("__j_stack", [this](std::vector<QV>) -> QV
        {
        // the trace for `new Throwable()`: without the exception's own constructors
        auto frames = javaStackTrace();
        size_t skip = 0;
        while (skip < frames.size() && frames[skip].find(".<init>(") != std::string::npos)
            ++skip;
        // and without Throwable.$fill itself
        auto arr = std::make_shared<Array>();
        for (size_t i = skip; i < frames.size(); ++i)
            arr->push_back(QV(frames[i]));
        return QV(arr); });

    // ── output ──
    reg("__j_print", [arg](std::vector<QV> a) -> QV
        {
        const QV &v = arg(a, 0);
        std::cout << (v.isString() ? v.asString() : javaUn(jop::VSTR, v).asString());
        javaFlushSoon();
        return QV(); });
    reg("__j_println", [arg](std::vector<QV> a) -> QV
        {
        if (!a.empty())
        {
            const QV &v = a[0];
            std::cout << (v.isString() ? v.asString() : javaUn(jop::VSTR, v).asString());
        }
        std::cout << '\n';
        javaFlushSoon();
        return QV(); });
    reg("__j_eprint", [arg](std::vector<QV> a) -> QV
        {
        const QV &v = arg(a, 0);
        std::cout.flush();
        std::cerr << (v.isString() ? v.asString() : javaUn(jop::VSTR, v).asString());
        std::cerr.flush();
        return QV(); });
    reg("__j_eprintln", [arg](std::vector<QV> a) -> QV
        {
        std::cout.flush();
        if (!a.empty())
            std::cerr << (a[0].isString() ? a[0].asString() : javaUn(jop::VSTR, a[0]).asString());
        std::cerr << '\n';
        std::cerr.flush();
        return QV(); });
    reg("__j_flush", [](std::vector<QV>) -> QV
        {
        std::cout.flush();
        std::cerr.flush();
        return QV(); });
    reg("__j_exit", [arg](std::vector<QV> a) -> QV
        {
        std::cout.flush();
        std::cerr.flush();
        std::exit((int)jint(arg(a, 0))); });

    // ── input (System.in) ──
    reg("__j_in_token", [](std::vector<QV>) -> QV
        {
        std::string t;
        return g_in.nextToken(t) ? QV(t) : QV(); });
    reg("__j_in_peek", [](std::vector<QV>) -> QV
        {
        std::string t = g_in.peekToken();
        return t.empty() ? QV() : QV(t); });
    reg("__j_in_line", [](std::vector<QV>) -> QV
        {
        std::string l;
        return g_in.nextLine(l) ? QV(l) : QV(); });
    reg("__j_in_hasLine", [](std::vector<QV>) -> QV { return QV(g_in.hasLine()); });
    reg("__j_in_read", [](std::vector<QV>) -> QV { return jlong(g_in.read()); });

    // ── arrays ──
    reg("__j_newarray", [arg](std::vector<QV> a) -> QV
        {
        int64_t n = jint(arg(a, 0));
        if (n < 0)
            jthrow("java.lang.NegativeArraySizeException", std::to_string(n));
        return QV(std::make_shared<Array>((size_t)n, arg(a, 1))); });
    // __j_newarray_multi([d1, d2, ...], default for the innermost dimension, total dimensions)
    reg("__j_newarray_multi", [arg](std::vector<QV> a) -> QV
        {
        const Array &dims = *arg(a, 0).asArray();
        const QV &def = arg(a, 1);
        int64_t total = jint(arg(a, 2));
        std::function<QV(size_t)> make = [&](size_t level) -> QV
        {
            int64_t n = jint(dims[level]);
            if (n < 0)
                jthrow("java.lang.NegativeArraySizeException", std::to_string(n));
            auto arr = std::make_shared<Array>();
            arr->reserve((size_t)n);
            for (int64_t i = 0; i < n; ++i)
                arr->push_back(level + 1 < dims.size() ? make(level + 1)
                                                       : ((int64_t)dims.size() < total ? QV() : def));
            return QV(arr);
        };
        return make(0); });
    reg("__j_aclone", [arg](std::vector<QV> a) -> QV
        {
        if (!arg(a, 0).isArray())
            npe("");
        return QV(std::make_shared<Array>(*arg(a, 0).asArray())); });
    reg("__j_arraycopy", [arg](std::vector<QV> a) -> QV
        {
        if (arg(a, 0).isNil() || arg(a, 2).isNil())
            npe("");
        auto src = arg(a, 0).asArray(), dst = arg(a, 2).asArray();
        int64_t sp = jint(arg(a, 1)), dp = jint(arg(a, 3)), n = jint(arg(a, 4));
        if (sp < 0 || dp < 0 || n < 0 || sp + n > (int64_t)src->size() || dp + n > (int64_t)dst->size())
            jthrow("java.lang.ArrayIndexOutOfBoundsException",
                   "arraycopy: last source index " + std::to_string(sp + n) + " out of bounds for length " +
                       std::to_string(src->size()));
        if (src == dst && sp < dp)
            for (int64_t i = n; i-- > 0;)
                (*dst)[(size_t)(dp + i)] = (*src)[(size_t)(sp + i)];
        else
            for (int64_t i = 0; i < n; ++i)
                (*dst)[(size_t)(dp + i)] = (*src)[(size_t)(sp + i)];
        return QV(); });
    // the program's command-line arguments as a String[]
    reg("__j_argv", [](std::vector<QV>) -> QV
        {
        auto arr = std::make_shared<Array>();
        for (size_t i = 1; i < g_scriptArgv.size(); ++i)
            arr->push_back(QV(g_scriptArgv[i]));
        return QV(arr); });

    // ── time ──
    reg("__j_nanoTime", [](std::vector<QV>) -> QV
        {
        return jlong(std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count()); });
    reg("__j_currentTimeMillis", [](std::vector<QV>) -> QV
        {
        return jlong(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count()); });
    reg("__j_sleep", [arg](std::vector<QV> a) -> QV
        {
        int64_t ms = jint(arg(a, 0));
        if (ms < 0)
            jthrow("java.lang.IllegalArgumentException", "timeout value is negative");
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return QV(); });

    // String.format / printf: (format, [args], kinds)
    reg("__j_format", [this, arg](std::vector<QV> a) -> QV
        {
        if (arg(a, 0).isNil())
            npe("");
        std::vector<QV> args;
        if (arg(a, 1).isArray())
            args = *arg(a, 1).asArray();
        std::string kinds = arg(a, 2).isString() ? arg(a, 2).asString() : "";
        return QV(javaFormat(arg(a, 0).asString(), args, kinds,
                             [this](const QV &v) { return javaToString(v); })); });
    reg("__j_toString", [this, arg](std::vector<QV> a) -> QV
        { return QV(javaToString(arg(a, 0))); });
    // calls a functional object (lambda or implementing instance): (fn, key, args...)
    reg("__j_callf", [this](std::vector<QV> a) -> QV
        {
        QV fn = a.size() > 0 ? a[0] : QV();
        std::string key = a.size() > 1 ? a[1].asString() : "";
        std::vector<QV> rest(a.begin() + std::min<size_t>(2, a.size()), a.end());
        return javaCallFunctional(fn, key, rest); });

    registerJavaLibNatives(reg, this);

    // ── prelude natives (`native` methods of src/java/prelude) ──
    reg("__jn_String_$format", [this, arg](std::vector<QV> a) -> QV
        {
        if (arg(a, 0).isNil())
            npe("");
        std::vector<QV> args;
        if (arg(a, 1).isArray())
            args = *arg(a, 1).asArray();
        return QV(javaFormat(arg(a, 0).asString(), args, "", [this](const QV &v) { return javaToString(v); })); });
    reg("__jn_$In_available", [](std::vector<QV>) -> QV
        { return jlong((int64_t)(g_in.buf.size() - std::min(g_in.pos, g_in.buf.size()))); });
    auto alias = [this](const char *name, const char *existing) { globals->define(name, globals->get(existing)); };
    alias("__jn_Class_$className", "__j_className");
    alias("__jn_Class_$simpleName", "__j_simpleName");
    alias("__jn_Class_$isInstance", "__j_isInstance");
    alias("__jn_Throwable_$stack", "__j_stack");
    alias("__jn_System_currentTimeMillis", "__j_currentTimeMillis");
    alias("__jn_System_nanoTime", "__j_nanoTime");
    alias("__jn_System_arraycopy", "__j_arraycopy");
    alias("__jn_System_identityHashCode", "__j_identityHash");
    alias("__jn_System_$exit", "__j_exit");
    alias("__jn_Thread_sleep", "__j_sleep");
    alias("__jn_$In_token", "__j_in_token");
    alias("__jn_$In_peek", "__j_in_peek");
    alias("__jn_$In_line", "__j_in_line");
    alias("__jn_$In_hasLine", "__j_in_hasLine");
    alias("__jn_$In_read", "__j_in_read");
    alias("__jn_Random_next0", "__jn_Random_next");
    for (const char *m : {"append", "length", "charAt", "toString", "reverse", "insert", "delete", "deleteCharAt",
                          "setCharAt", "setLength", "replace"})
        alias((std::string("__jn_StringBuffer_") + m).c_str(), (std::string("__jn_StringBuilder_") + m).c_str());
}
