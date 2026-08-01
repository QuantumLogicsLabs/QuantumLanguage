#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Value.h"

// ─── Opcodes ──────────────────────────────────────────────────────────────────
enum class Op : uint8_t
{
    // Stack manipulation
    LOAD_CONST, // push constants[operand]
    LOAD_NIL,   // push nil
    LOAD_TRUE,  // push true
    LOAD_FALSE, // push false
    POP,        // discard top of stack

    // Variables
    DEFINE_GLOBAL, // globals[constants[operand]] = pop()
    LOAD_GLOBAL,   // push globals[constants[operand]]
    STORE_GLOBAL,  // globals[constants[operand]] = peek(0)
    DEFINE_LOCAL,  // locals[operand] = pop()
    LOAD_LOCAL,    // push locals[operand]
    STORE_LOCAL,   // locals[operand] = peek(0)
    DEFINE_CONST,  // define constant global (isConst=true)

    // Upvalue / closure captures
    LOAD_UPVALUE,  // push upvalues[operand]
    STORE_UPVALUE, // upvalues[operand] = peek(0)
    CLOSE_UPVALUE, // promote local to heap cell

    // Arithmetic
    ADD,
    SUB,
    MUL,
    DIV,
    MOD,
    FLOOR_DIV,
    POW,
    NEG,

    // Bitwise
    BIT_AND,
    BIT_OR,
    BIT_XOR,
    BIT_NOT,
    LSHIFT,
    RSHIFT,

    // Comparison
    EQ,
    NEQ,
    LT,
    LTE,
    GT,
    GTE,

    // Logical
    NOT,
    AND, // short-circuit: if !peek(0) jump
    OR,  // short-circuit: if peek(0) jump

    // String
    CONCAT, // dedicated string concat (+)

    // Control flow
    JUMP,          // ip += operand (unconditional)
    JUMP_IF_FALSE, // if !peek(0): ip += operand  (leaves value on stack)
    JUMP_IF_TRUE,  // if  peek(0): ip += operand
    LOOP,          // ip -= operand (backward jump)
    JUMP_ABSOLUTE, // ip = operand

    // Function / Call
    CALL,          // call top-of-stack function with operand args
    RETURN,        // return top of stack
    RETURN_NIL,    // return nil
    MAKE_FUNCTION, // wrap Chunk* constants[operand] into QuantumFunction
    MAKE_CLOSURE,  // wrap + capture upvalues

    // Collections
    MAKE_ARRAY, // pop operand values → array
    MAKE_DICT,  // pop operand*2 key/value pairs → dict
    MAKE_TUPLE, // pop operand values → tuple (stored as array)

    // Member / index access
    GET_INDEX,  // obj[key]
    SET_INDEX,  // obj[key] = value
    GET_MEMBER, // obj.name  (name = constants[operand])
    SET_MEMBER, // obj.name = value
    GET_SUPER,  // super.method

    // Iteration
    FOR_ITER,  // advance iterator; jump if done
    MAKE_ITER, // wrap value in IteratorState, push

    // Classes
    MAKE_CLASS,   // define class constants[operand]
    INHERIT,      // set base class
    BIND_METHOD,  // bind function as method
    INSTANCE_NEW, // call class constructor

    // Exceptions
    PUSH_HANDLER, // push exception handler (catch ip = operand)
    POP_HANDLER,
    RAISE,   // throw top of stack
    RERAISE, // rethrow current exception

    // Pointers (C++ extensions)
    ADDRESS_OF, // &var
    DEREF,      // *ptr
    ARROW,      // ptr->member

    // Misc
    PRINT, // print operand args (optimised for print stmt)
    DUP,   // duplicate top of stack
    DUP_TWO, // duplicate top two items of stack [A, B] -> [A, B, A, B]
    SET_INDEX_COMPOUND, // obj[key] = value where stack is [obj, key, value]
    SWAP,  // swap top two
    NOP,
};

// ─── Instruction ─────────────────────────────────────────────────────────────
struct Instruction
{
    Op op;
    int32_t operand = 0; // index or offset
    int line = 0;        // source line for error messages
};

// ─── Chunk ────────────────────────────────────────────────────────────────────
// A compiled unit: a function body (or top-level script).
struct Chunk
{
    std::string name; // "<script>", function name, etc.
    std::vector<Instruction> code;
    std::vector<QuantumValue> constants;

    // Parameter info (for functions)
    std::vector<std::string> params;
    std::vector<bool> paramIsRef;
    int upvalueCount = 0;

    // Append an instruction; returns its index.
    size_t emit(Op op, int32_t operand = 0, int line = 0)
    {
        code.push_back({op, operand, line});
        return code.size() - 1;
    }

    // Patch the operand of a previously-emitted instruction.
    void patch(size_t idx, int32_t operand)
    {
        code[idx].operand = operand;
    }

    // Add a constant; returns its index.
    int32_t addConstant(QuantumValue val)
    {
        constants.push_back(std::move(val));
        return static_cast<int32_t>(constants.size()) - 1;
    }

    // Convenience: add a string constant and return its index.
    int32_t addString(const std::string &s)
    {
        return addConstant(QuantumValue(s));
    }
};