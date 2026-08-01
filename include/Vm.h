#pragma once
#include "Opcode.h"
#include "Value.h"
#include "Error.h"
#include <vector>
#include <deque>
#include <memory>
#include <unordered_map>
#include <functional>
#include <string>

// ─── Upvalue (heap cell for captured variables) ───────────────────────────────
struct Upvalue
{
    std::shared_ptr<QuantumValue> cell; // points to the live value
    QuantumValue closed;                // storage after variable leaves stack

    explicit Upvalue(std::shared_ptr<QuantumValue> c) : cell(c) {}

    QuantumValue get() const { return *cell; }
    void set(QuantumValue v) { *cell = std::move(v); }
};

// ─── Closure ──────────────────────────────────────────────────────────────────
struct Closure
{
    std::shared_ptr<Chunk> chunk;
    std::vector<std::shared_ptr<Upvalue>> upvalues;
    std::string name;

    explicit Closure(std::shared_ptr<Chunk> c)
        : chunk(std::move(c)), name(chunk->name) {}
};

// ─── CallFrame ────────────────────────────────────────────────────────────────
struct CallFrame
{
    std::shared_ptr<Closure> closure;
    size_t ip;        // instruction pointer
    size_t stackBase; // where locals start on the value stack
};

// ─── ExceptionHandler ─────────────────────────────────────────────────────────
struct ExceptionHandler
{
    int32_t catchIp;   // IP to jump to on exception
    size_t frameDepth; // call-frame depth to unwind to
    size_t stackDepth; // value stack depth to restore
};

// ─── VM ───────────────────────────────────────────────────────────────────────
class VM
{
public:
    VM();

    // Execute a compiled chunk (top-level script).
    void run(std::shared_ptr<Chunk> chunk);

    // Expose globals so the REPL can persist state across calls.
    std::shared_ptr<Environment> globals;

private:
    // Value stack
    std::deque<QuantumValue> stack_;
    std::vector<CallFrame> frames_;
    std::vector<ExceptionHandler> handlers_;

    // Open upvalues linked list (for closing)
    std::vector<std::shared_ptr<Upvalue>> openUpvalues_;

    long long stepCount_ = 0;
    static constexpr long long MAX_STEPS = 200'000'000;
    std::vector<std::pair<QuantumValue, size_t>> pendingInstances_;

    // ── Native registration ───────────────────────────────────────────────────
    void registerNatives();

    // ── Execution ────────────────────────────────────────────────────────────
    void runFrame(size_t stopDepth = 0);

    // ── Stack helpers ─────────────────────────────────────────────────────────
    void push(QuantumValue v);
    QuantumValue pop();
    QuantumValue &peek(int offset = 0);

    // ── Call helpers ──────────────────────────────────────────────────────────
    void callValue(QuantumValue callee, int argCount, int line);
    void callClosure(std::shared_ptr<Closure> closure, int argCount, int line);
    void callNativeFn(std::shared_ptr<QuantumNative> fn, int argCount, int line);
    void callClass(std::shared_ptr<QuantumClass> klass, int argCount, int line);
    QuantumValue callBuiltinMethod(QuantumValue &obj,
                                   const std::string &method,
                                   std::vector<QuantumValue> args,
                                   int line);
    QuantumValue callArrayMethod(std::shared_ptr<Array> arr,
                                 const std::string &method,
                                 std::vector<QuantumValue> args);
    QuantumValue callStringMethod(const std::string &s,
                                  const std::string &method,
                                  std::vector<QuantumValue> args);
    QuantumValue callDictMethod(std::shared_ptr<Dict> d,
                                const std::string &method,
                                std::vector<QuantumValue> args);

    // ── Upvalue helpers ───────────────────────────────────────────────────────
    std::shared_ptr<Upvalue> captureUpvalue(size_t stackIdx);
    void closeUpvalues(size_t fromIdx);

    // ── Binary / unary ops ────────────────────────────────────────────────────
    QuantumValue execBinary(Op op, const QuantumValue &left, const QuantumValue &right, int line);
    QuantumValue execUnary(Op op, const QuantumValue &val, int line);

    // Iterator state is stored inside each iterator native's fn closure

    // ── Misc helpers ──────────────────────────────────────────────────────────
    static std::string valueEq(const QuantumValue &a, const QuantumValue &b);
    static bool valuesEqual(const QuantumValue &a, const QuantumValue &b);
    double toNumber(const QuantumValue &v, const std::string &ctx, int line);
    void runtimeError(const std::string &msg, int line);
};