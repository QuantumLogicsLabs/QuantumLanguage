#include "Disassembler.h"
#include "Vm.h"
#include <iomanip>
#include <ostream>

static const char *opName(Op op)
{
    switch (op)
    {
    case Op::LOAD_CONST:
        return "LOAD_CONST";
    case Op::LOAD_NIL:
        return "LOAD_NIL";
    case Op::LOAD_TRUE:
        return "LOAD_TRUE";
    case Op::LOAD_FALSE:
        return "LOAD_FALSE";
    case Op::POP:
        return "POP";
    case Op::DUP:
        return "DUP";
    case Op::DUP_TWO:
        return "DUP_TWO";
    case Op::SET_INDEX_COMPOUND:
        return "SET_INDEX_COMPOUND";
    case Op::SWAP:
        return "SWAP";
    case Op::NOP:
        return "NOP";
    case Op::DEFINE_GLOBAL:
        return "DEFINE_GLOBAL";
    case Op::LOAD_GLOBAL:
        return "LOAD_GLOBAL";
    case Op::STORE_GLOBAL:
        return "STORE_GLOBAL";
    case Op::DEFINE_LOCAL:
        return "DEFINE_LOCAL";
    case Op::LOAD_LOCAL:
        return "LOAD_LOCAL";
    case Op::STORE_LOCAL:
        return "STORE_LOCAL";
    case Op::DEFINE_CONST:
        return "DEFINE_CONST";
    case Op::LOAD_UPVALUE:
        return "LOAD_UPVALUE";
    case Op::STORE_UPVALUE:
        return "STORE_UPVALUE";
    case Op::CLOSE_UPVALUE:
        return "CLOSE_UPVALUE";
    case Op::ADD:
        return "ADD";
    case Op::SUB:
        return "SUB";
    case Op::MUL:
        return "MUL";
    case Op::DIV:
        return "DIV";
    case Op::MOD:
        return "MOD";
    case Op::FLOOR_DIV:
        return "FLOOR_DIV";
    case Op::POW:
        return "POW";
    case Op::NEG:
        return "NEG";
    case Op::BIT_AND:
        return "BIT_AND";
    case Op::BIT_OR:
        return "BIT_OR";
    case Op::BIT_XOR:
        return "BIT_XOR";
    case Op::BIT_NOT:
        return "BIT_NOT";
    case Op::LSHIFT:
        return "LSHIFT";
    case Op::RSHIFT:
        return "RSHIFT";
    case Op::EQ:
        return "EQ";
    case Op::NEQ:
        return "NEQ";
    case Op::LT:
        return "LT";
    case Op::LTE:
        return "LTE";
    case Op::GT:
        return "GT";
    case Op::GTE:
        return "GTE";
    case Op::NOT:
        return "NOT";
    case Op::AND:
        return "AND";
    case Op::OR:
        return "OR";
    case Op::CONCAT:
        return "CONCAT";
    case Op::JUMP:
        return "JUMP";
    case Op::JUMP_IF_FALSE:
        return "JUMP_IF_FALSE";
    case Op::JUMP_IF_TRUE:
        return "JUMP_IF_TRUE";
    case Op::LOOP:
        return "LOOP";
    case Op::JUMP_ABSOLUTE:
        return "JUMP_ABSOLUTE";
    case Op::CALL:
        return "CALL";
    case Op::RETURN:
        return "RETURN";
    case Op::RETURN_NIL:
        return "RETURN_NIL";
    case Op::MAKE_FUNCTION:
        return "MAKE_FUNCTION";
    case Op::MAKE_CLOSURE:
        return "MAKE_CLOSURE";
    case Op::MAKE_ARRAY:
        return "MAKE_ARRAY";
    case Op::MAKE_DICT:
        return "MAKE_DICT";
    case Op::MAKE_TUPLE:
        return "MAKE_TUPLE";
    case Op::GET_INDEX:
        return "GET_INDEX";
    case Op::SET_INDEX:
        return "SET_INDEX";
    case Op::GET_MEMBER:
        return "GET_MEMBER";
    case Op::SET_MEMBER:
        return "SET_MEMBER";
    case Op::GET_SUPER:
        return "GET_SUPER";
    case Op::FOR_ITER:
        return "FOR_ITER";
    case Op::MAKE_ITER:
        return "MAKE_ITER";
    case Op::MAKE_CLASS:
        return "MAKE_CLASS";
    case Op::INHERIT:
        return "INHERIT";
    case Op::BIND_METHOD:
        return "BIND_METHOD";
    case Op::INSTANCE_NEW:
        return "INSTANCE_NEW";
    case Op::PUSH_HANDLER:
        return "PUSH_HANDLER";
    case Op::POP_HANDLER:
        return "POP_HANDLER";
    case Op::RAISE:
        return "RAISE";
    case Op::RERAISE:
        return "RERAISE";
    case Op::ADDRESS_OF:
        return "ADDRESS_OF";
    case Op::DEREF:
        return "DEREF";
    case Op::ARROW:
        return "ARROW";
    case Op::PRINT:
        return "PRINT";
    default:
        return "??";
    }
}

void disassembleInstruction(const Chunk &chunk, size_t idx, std::ostream &out)
{
    const Instruction &instr = chunk.code[idx];
    out << std::setw(5) << idx << "  ";
    out << std::left << std::setw(18) << opName(instr.op);

    // Show operand with context
    auto showConst = [&]()
    {
        out << " [" << instr.operand << "] ";
        if (instr.operand >= 0 && instr.operand < (int)chunk.constants.size())
            out << "'" << chunk.constants[instr.operand].toString() << "'";
    };

    switch (instr.op)
    {
    case Op::LOAD_CONST:
    case Op::DEFINE_GLOBAL:
    case Op::LOAD_GLOBAL:
    case Op::STORE_GLOBAL:
    case Op::DEFINE_CONST:
    case Op::GET_MEMBER:
    case Op::SET_MEMBER:
    case Op::BIND_METHOD:
    case Op::GET_SUPER:
        showConst();
        break;
    case Op::LOAD_LOCAL:
    case Op::STORE_LOCAL:
    case Op::DEFINE_LOCAL:
        out << " slot[" << instr.operand << "]";
        if (instr.operand < (int)chunk.params.size())
            out << " (" << chunk.params[instr.operand] << ")";
        break;
    case Op::LOAD_UPVALUE:
    case Op::STORE_UPVALUE:
        out << " uv[" << instr.operand << "]";
        break;
    case Op::JUMP:
    case Op::JUMP_IF_FALSE:
    case Op::JUMP_IF_TRUE:
        out << " +" << instr.operand << " → " << (idx + 1 + instr.operand);
        break;
    case Op::LOOP:
        out << " -" << instr.operand << " → " << (idx + 1 - instr.operand);
        break;
    case Op::CALL:
        out << " argc=" << instr.operand;
        break;
    case Op::MAKE_ARRAY:
        out << " n=" << instr.operand;
        break;
    case Op::MAKE_DICT:
        out << " pairs=" << instr.operand;
        break;
    case Op::PRINT:
        out << " argc=" << instr.operand;
        break;
    default:
        if (instr.operand != 0)
            out << " " << instr.operand;
        break;
    }

    if (instr.line > 0)
        out << "  ; line " << instr.line;

    out << "\n";
}

void disassembleChunk(const Chunk &chunk, std::ostream &out)
{
    out << "=== " << chunk.name << " ===\n";
    out << "  constants: " << chunk.constants.size()
        << "  params: " << chunk.params.size()
        << "  upvalues: " << chunk.upvalueCount << "\n";

    for (size_t i = 0; i < chunk.code.size(); ++i)
        disassembleInstruction(chunk, i, out);

    out << "\n";

    for (const auto &constant : chunk.constants)
    {
        if (!constant.isFunction())
            continue;
        auto fn = constant.asFunction();
        if (!fn || !fn->chunk || fn->chunk.get() == &chunk)
            continue;
        disassembleChunk(*fn->chunk, out);
    }
}
