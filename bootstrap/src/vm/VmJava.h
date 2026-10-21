#pragma once
// Internal to the Java runtime (VmJava.cpp, VmJavaLib.cpp): the operations
// behind the J_* opcodes, Java exceptions, and Java string semantics on the
// VM's UTF-8 strings. See include/JavaOps.h for how Java values are held.
#include "Value.h"
#include "Error.h"

#include <cstdint>
#include <string>
#include <vector>

struct QuantumValue;

// ── opcodes ──
QuantumValue javaBin(int op, const QuantumValue &a, const QuantumValue &b);
QuantumValue javaUn(int op, const QuantumValue &v);
QuantumValue javaGetField(const QuantumValue &obj, const std::string &name);
void javaPutField(const QuantumValue &obj, const std::string &name, const QuantumValue &v);
QuantumValue javaArrayLoad(const QuantumValue &arr, const QuantumValue &idx);
void javaArrayStore(const QuantumValue &arr, const QuantumValue &idx, const QuantumValue &v);
bool javaInstanceOf(const QuantumValue &v, const std::string &desc);
void javaCheckCast(const QuantumValue &v, const std::string &desc);

// ── exceptions ──
// Raises Java exception `cls` (binary name, "java.lang.ArithmeticException")
// from native code; it reaches the program's catch blocks via
// javaErrorDescriptor.
[[noreturn]] void jthrow(const std::string &cls, const std::string &msg);
// What a Java catch block receives for an error raised by the VM: the
// string "\x1fJ<class>\x1f<message>\x1f<frame>\n<frame>..." (java.lang.Throwable.$wrap
// in the prelude turns it into an exception object).
QuantumValue javaErrorDescriptor(const QuantumError &e, const std::vector<std::string> &trace);

// ── values ──
inline QuantumValue jlong(int64_t v) { return QuantumValue::ofLong(v); }
// An integral Java value as int64 (also accepts a double, a bool or a char box).
int64_t jint(const QuantumValue &v);
double jdouble(const QuantumValue &v);
inline int64_t wrap32(int64_t x) { return (int64_t)(int32_t)(uint32_t)(uint64_t)x; }

// ── strings: Java indexes UTF-16 code units; the VM holds UTF-8 ──
bool isAscii(const std::string &s);
std::u16string toU16(const std::string &s);
std::string fromU16(const std::u16string &s);
std::string unitToUtf8(uint32_t unit); // a char (possibly a lone surrogate)
int64_t u16Length(const std::string &s);
uint32_t u16At(const std::string &s, int64_t i); // throws StringIndexOutOfBoundsException
std::string u16Substr(const std::string &s, int64_t begin, int64_t end);

// ── number formatting ──
std::string javaDoubleToString(double d);
std::string javaFloatToString(double f);
std::string javaLongToString(int64_t v, int radix = 10);
// java.lang.String.format / PrintStream.printf. `kinds` holds, per argument,
// the static type of the call site's argument: I int-like, J long, C char,
// D double, F float, Z boolean, S String, O anything else; `str` converts an
// object argument for %s.
std::string javaFormat(const std::string &fmt, const std::vector<QuantumValue> &args, const std::string &kinds,
                       const std::function<std::string(const QuantumValue &)> &str);
