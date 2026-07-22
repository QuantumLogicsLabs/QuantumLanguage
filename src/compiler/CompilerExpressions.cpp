#include "Compiler.h"
#include "Error.h"
#include "Vm.h"
#include <stdexcept>
#include <algorithm>
#include <unordered_map>

void Compiler::compileBinary(BinaryExpr &e, int line)
{
    if (e.op == "and" || e.op == "&&")
    {
        compileExpr(*e.left);
        size_t sc = emitJump(Op::JUMP_IF_FALSE, line);
        emit(Op::POP, 0, line);
        compileExpr(*e.right);
        patchJump(sc);
        return;
    }
    if (e.op == "or" || e.op == "||" || e.op == "??")
    {
        compileExpr(*e.left);
        size_t sc = emitJump(Op::JUMP_IF_TRUE, line);
        emit(Op::POP, 0, line);
        compileExpr(*e.right);
        patchJump(sc);
        return;
    }
    if (e.op == "in" || e.op == "not in")
    {
        emit(Op::LOAD_GLOBAL, addStr("__contains__"), line);
        compileExpr(*e.left);
        compileExpr(*e.right);
        emit(Op::CALL, 2, line);
        if (e.op == "not in")
            emit(Op::NOT, 0, line);
        return;
    }

    compileExpr(*e.left);
    compileExpr(*e.right);

    static const std::unordered_map<std::string, Op> opMap = {
        {"+", Op::ADD},
        {"-", Op::SUB},
        {"*", Op::MUL},
        {"/", Op::DIV},
        {"%", Op::MOD},
        {"//", Op::FLOOR_DIV},
        {"**", Op::POW},
        {"==", Op::EQ},
        {"!=", Op::NEQ},
        {"===", Op::EQ},
        {"!==", Op::NEQ},
        {"<", Op::LT},
        {"<=", Op::LTE},
        {">", Op::GT},
        {">=", Op::GTE},
        {"&", Op::BIT_AND},
        {"|", Op::BIT_OR},
        {"^", Op::BIT_XOR},
        {"<<", Op::LSHIFT},
        {">>", Op::RSHIFT},
        {"is", Op::EQ},
        {"is not", Op::NEQ},
    };
    auto it = opMap.find(e.op);
    if (it != opMap.end())
        emit(it->second, 0, line);
    else
        throw std::runtime_error("Compiler: unknown binary op '" + e.op + "'");
}

void Compiler::compileUnary(UnaryExpr &e, int line)
{
    if (e.op == "...")
    {
        compileExpr(*e.operand);
        return;
    }

    compileExpr(*e.operand);
    if (e.op == "-")
        emit(Op::NEG, 0, line);
    else if (e.op == "!" ||
             e.op == "not")
        emit(Op::NOT, 0, line);
    else if (e.op == "~")
        emit(Op::BIT_NOT, 0, line);
    else if (e.op == "++" || e.op == "--")
    {
        emit(Op::LOAD_CONST, addConst(QuantumValue(1.0)), line);
        emit(e.op == "++" ? Op::ADD : Op::SUB, 0, line);
        if (e.operand->is<Identifier>())
        {
            emit(Op::DUP, 0, line);
            emitStore(e.operand->as<Identifier>().name, line);
            emit(Op::POP, 0, line);
        }
    }
    else
        throw std::runtime_error("Compiler: unknown unary op '" + e.op + "'");
}

void Compiler::compileAssign(AssignExpr &e, int line)
{
    const std::string normalizedOp =
        e.op == "post+=" ? "+=" : e.op == "post-=" ? "-="
                                                   : e.op;
    bool compound = (normalizedOp != "=");

    static const std::unordered_map<std::string, Op> cops = {
        {"+=", Op::ADD},
        {"-=", Op::SUB},
        {"*=", Op::MUL},
        {"/=", Op::DIV},
        {"%=", Op::MOD},
        {"&=", Op::BIT_AND},
        {"|=", Op::BIT_OR},
        {"^=", Op::BIT_XOR},
    };

    if (e.op == "unpack" && e.target->is<TupleLiteral>())
    {
        compileExpr(*e.value);
        for (size_t i = 0; i < e.target->as<TupleLiteral>().elements.size(); ++i)
        {
            auto &target = e.target->as<TupleLiteral>().elements[i];
            if (!target->is<Identifier>())
                continue;
            emit(Op::DUP, 0, line);
            emit(Op::LOAD_CONST, addConst(QuantumValue(static_cast<double>(i))), line);
            emit(Op::GET_INDEX, 0, line);
            emitStore(target->as<Identifier>().name, line);
            emit(Op::POP, 0, line);
        }
        return;
    }

    if (e.target->is<Identifier>())
    {
        const std::string &name = e.target->as<Identifier>().name;
        if (e.op == "post+=" || e.op == "post-=")
        {
            emitLoad(name, line);
            emit(Op::DUP, 0, line);
            compileExpr(*e.value);
            emit(e.op == "post+=" ? Op::ADD : Op::SUB, 0, line);
            emitStore(name, line);
            emit(Op::POP, 0, line);
            return;
        }
        if (compound)
            emitLoad(name, line);
        compileExpr(*e.value);
        if (compound)
        {
            auto it = cops.find(normalizedOp);
            if (it != cops.end())
                emit(it->second, 0, line);
        }
        emit(Op::DUP, 0, line);
        emitStore(name, line);
        emit(Op::POP, 0, line);
        return;
    }

    if (e.target->is<IndexExpr>())
    {
        auto &idx = e.target->as<IndexExpr>();
        // VM SET_INDEX expects stack: val (bottom), obj, key (top)
        compileExpr(*e.value);    // val  <- bottom
        compileExpr(*idx.object); // obj
        compileExpr(*idx.index);  // key  <- top
        emit(Op::SET_INDEX, 0, line);
        // SET_INDEX pushes val back as the expression result
        return;
    }

    if (e.target->is<MemberExpr>())
    {
        auto &mem = e.target->as<MemberExpr>();

        if (e.op == "post+=" || e.op == "post-=")
        {
            compileExpr(*mem.object);
            emit(Op::GET_MEMBER, addStr(mem.member), line); // old
            emit(Op::DUP, 0, line);                         // old old
            compileExpr(*e.value);
            emit(e.op == "post+=" ? Op::ADD : Op::SUB, 0, line); // old new
            compileExpr(*mem.object);                               // old new obj
            emit(Op::SWAP, 0, line);                                // old obj new
            emit(Op::SET_MEMBER, addStr(mem.member), line);         // old obj
            emit(Op::POP, 0, line);                                 // old
            return;
        }

        if (compound)
        {
            compileExpr(*mem.object);
            emit(Op::GET_MEMBER, addStr(mem.member), line); // old
            compileExpr(*e.value);
            auto it = cops.find(normalizedOp);
            if (it != cops.end())
                emit(it->second, 0, line);                  // new
            emit(Op::DUP, 0, line);                         // new new
            compileExpr(*mem.object);                       // new new obj
            emit(Op::SWAP, 0, line);                        // new obj new
            emit(Op::SET_MEMBER, addStr(mem.member), line); // new obj
            emit(Op::POP, 0, line);                         // new
            return;
        }

        compileExpr(*e.value);                              // val
        emit(Op::DUP, 0, line);                             // val val
        compileExpr(*mem.object);                           // val val obj
        emit(Op::SWAP, 0, line);                            // val obj val
        emit(Op::SET_MEMBER, addStr(mem.member), line);     // val obj
        emit(Op::POP, 0, line);                             // val
        return;
    }

    // Fallback: evaluate rhs and leave on stack
    compileExpr(*e.value);
}

void Compiler::compileCall(CallExpr &e, int line)
{
    auto emitArgValues = [&](ASTNode &arg) -> int
    {
        if (arg.is<AssignExpr>())
        {
            auto &assign = arg.as<AssignExpr>();
            if (assign.op == "=" && assign.target->is<Identifier>())
            {
                compileExpr(*assign.value);
                return 1;
            }
            if (assign.op == "unpack" && assign.target->is<TupleLiteral>())
            {
                auto &targets = assign.target->as<TupleLiteral>().elements;
                if (!targets.empty())
                {
                    for (size_t i = 0; i + 1 < targets.size(); ++i)
                        compileExpr(*targets[i]);
                    compileExpr(*assign.value);
                    return static_cast<int>(targets.size());
                }
            }
        }
        compileExpr(arg);
        return 1;
    };

    bool hasSpread = false;
    for (auto &arg : e.args)
    {
        if (arg->is<UnaryExpr>())
        {
            const auto &unary = arg->as<UnaryExpr>();
            if (unary.op == "..." || unary.op == "**")
            {
                hasSpread = true;
                break;
            }
        }
    }

    if (hasSpread)
    {
        emit(Op::LOAD_GLOBAL, addStr("__call_spread__"), line);
        compileExpr(*e.callee);
        emit(Op::MAKE_ARRAY, 0, line);
        for (auto &arg : e.args)
        {
            bool isSpread = arg->is<UnaryExpr>() &&
                            (arg->as<UnaryExpr>().op == "..." || arg->as<UnaryExpr>().op == "**");
            emit(Op::LOAD_GLOBAL, addStr(isSpread ? "__array_extend__" : "__listcomp_push__"), line);
            emit(Op::SWAP, 0, line);
            if (isSpread)
                compileExpr(*arg->as<UnaryExpr>().operand);
            else
                emitArgValues(*arg);
            emit(Op::CALL, 2, line);
        }
        emit(Op::CALL, 2, line);
        return;
    }

    // super.method(args) -- special case
    if (e.callee->is<SuperExpr>())
    {
        auto &superExpr = e.callee->as<SuperExpr>();
        if (superExpr.method.empty())
        {
            emitLoad("self", line);
            emit(Op::GET_SUPER, addStr("__init__"), line);
            int argCount = 0;
            for (auto &arg : e.args)
                argCount += emitArgValues(*arg);
            emit(Op::CALL, argCount, line);
            return;
        }
    }

    if (e.callee->is<MemberExpr>())
    {
        auto &mem = e.callee->as<MemberExpr>();
        if (mem.object->is<SuperExpr>())
        {
            // Load self (slot 0), GET_SUPER method, push args, CALL
            emitLoad("self", line);
            emit(Op::GET_SUPER, addStr(mem.member), line);
            int argCount = 0;
            for (auto &arg : e.args)
                argCount += emitArgValues(*arg);
            emit(Op::CALL, argCount, line);
            return;
        }
        if (mem.object->is<CallExpr>())
        {
            auto &superCall = mem.object->as<CallExpr>();
            if (superCall.callee->is<SuperExpr>() && superCall.callee->as<SuperExpr>().method.empty())
            {
                emitLoad("self", line);
                emit(Op::GET_SUPER, addStr(mem.member), line);
                int argCount = 0;
                for (auto &arg : e.args)
                    argCount += emitArgValues(*arg);
                emit(Op::CALL, argCount, line);
                return;
            }
        }
        // Regular method call: obj.method(args)
        compileExpr(*mem.object);
        emit(Op::GET_MEMBER, addStr(mem.member), line);
        int argCount = 0;
        for (auto &arg : e.args)
            argCount += emitArgValues(*arg);
        emit(Op::CALL, argCount, line);
        return;
    }
    // Regular call
    compileExpr(*e.callee);
    int argCount = 0;
    for (auto &arg : e.args)
        argCount += emitArgValues(*arg);
    emit(Op::CALL, argCount, line);
}

void Compiler::compileIndex(IndexExpr &e, int line)
{
    compileExpr(*e.object);
    compileExpr(*e.index);
    emit(Op::GET_INDEX, 0, line);
}

void Compiler::compileSlice(SliceExpr &e, int line)
{
    emit(Op::LOAD_GLOBAL, addStr("__slice__"), line);
    compileExpr(*e.object);
    if (e.start)
        compileExpr(*e.start);
    else
        emit(Op::LOAD_NIL, 0, line);
    if (e.stop)
        compileExpr(*e.stop);
    else
        emit(Op::LOAD_NIL, 0, line);
    if (e.step)
        compileExpr(*e.step);
    else
        emit(Op::LOAD_NIL, 0, line);
    emit(Op::CALL, 4, line);
}

void Compiler::compileMember(MemberExpr &e, int line)
{
    compileExpr(*e.object);
    emit(Op::GET_MEMBER, addStr(e.member), line);
}

void Compiler::compileArray(ArrayLiteral &e, int line)
{
    bool hasSpread = false;
    for (auto &el : e.elements)
    {
        if (el->is<UnaryExpr>() && el->as<UnaryExpr>().op == "...")
        {
            hasSpread = true;
            break;
        }
    }

    if (hasSpread)
    {
        emit(Op::MAKE_ARRAY, 0, line);
        for (auto &el : e.elements)
        {
            bool isSpread = el->is<UnaryExpr>() && el->as<UnaryExpr>().op == "...";
            emit(Op::LOAD_GLOBAL, addStr(isSpread ? "__array_extend__" : "__listcomp_push__"), line);
            emit(Op::SWAP, 0, line);
            if (isSpread)
                compileExpr(*el->as<UnaryExpr>().operand);
            else
                compileExpr(*el);
            emit(Op::CALL, 2, line);
        }
        return;
    }

    for (auto &el : e.elements)
        compileExpr(*el);
    emit(Op::MAKE_ARRAY, static_cast<int32_t>(e.elements.size()), line);
}

void Compiler::compileDict(DictLiteral &e, int line)
{
    bool hasSpread = false;
    for (auto &[k, v] : e.pairs)
    {
        if (!k)
        {
            hasSpread = true;
            break;
        }
    }

    if (hasSpread)
    {
        emit(Op::MAKE_DICT, 0, line);
        for (auto &[k, v] : e.pairs)
        {
            if (!k)
            {
                emit(Op::LOAD_GLOBAL, addStr("__dict_merge__"), line);
                emit(Op::SWAP, 0, line);
                compileExpr(*v);
                emit(Op::CALL, 2, line);
                continue;
            }
            emit(Op::LOAD_GLOBAL, addStr("__dict_set__"), line);
            emit(Op::SWAP, 0, line);
            compileExpr(*k);
            compileExpr(*v);
            emit(Op::CALL, 3, line);
        }
        return;
    }

    for (auto &[k, v] : e.pairs)
    {
        compileExpr(*k);
        compileExpr(*v);
    }
    emit(Op::MAKE_DICT, static_cast<int32_t>(e.pairs.size()), line);
}

void Compiler::compileTuple(TupleLiteral &e, int line)
{
    for (auto &el : e.elements)
        compileExpr(*el);
    emit(Op::MAKE_TUPLE, static_cast<int32_t>(e.elements.size()), line);
}

void Compiler::compileLambda(LambdaExpr &e, int line)
{
    std::vector<bool> noRef(e.params.size(), false);
    auto fnChunk = compileFunction("lambda", e.params, noRef, e.defaultArgs, e.body.get(), line);
    auto closureTpl = std::make_shared<Closure>(fnChunk);
    emit(Op::LOAD_CONST, addConst(QuantumValue(closureTpl)), line);
    emit(fnChunk->upvalueCount > 0 ? Op::MAKE_CLOSURE : Op::MAKE_FUNCTION, 0, line);
}

void Compiler::compileTernary(TernaryExpr &e, int line)
{
    compileExpr(*e.condition);
    size_t elseJump = emitJump(Op::JUMP_IF_FALSE, line);
    emit(Op::POP, 0, line);
    compileExpr(*e.thenExpr);
    size_t endJump = emitJump(Op::JUMP, line);
    patchJump(elseJump);
    emit(Op::POP, 0, line);
    compileExpr(*e.elseExpr);
    patchJump(endJump);
}

void Compiler::compileListComp(ListComp &e, int line)
{
    CompilerState fnState("<listcomp>", current_);
    fnState.isFunction = true;
    CompilerState *prev = current_;
    current_ = &fnState;

    beginScope();
    const std::string resultName = "__result__";

    emit(Op::MAKE_ARRAY, 0, line);
    declareLocal(resultName, line);
    emit(Op::DEFINE_LOCAL, static_cast<int>(current_->locals.size()) - 1, line);

    beginScope();
    compileExpr(*e.iterable);
    emit(Op::MAKE_ITER, 0, line);
    declareLocal("__iter__", line);
    emit(Op::DEFINE_LOCAL, static_cast<int>(current_->locals.size()) - 1, line);

    int loopStart = static_cast<int>(chunk().code.size());
    beginLoop(loopStart);
    size_t exitJump = emitJump(Op::FOR_ITER, line);

    beginScope();
    for (auto &v : e.vars)
    {
        declareLocal(v, line);
        emit(Op::DEFINE_LOCAL, static_cast<int>(current_->locals.size()) - 1, line);
    }

    auto pushToResult = [&]()
    {
        emitLoad(resultName, line);
        emit(Op::GET_MEMBER, addStr("push"), line);
        emit(Op::SWAP, 0, line);
        emit(Op::CALL, 1, line);
        emit(Op::POP, 0, line);
    };

    if (e.condition)
    {
        compileExpr(*e.condition);
        size_t skipJump = emitJump(Op::JUMP_IF_FALSE, line);
        emit(Op::POP, 0, line);
        compileExpr(*e.expr);
        pushToResult();
        size_t jmp = emitJump(Op::JUMP, line);
        patchJump(skipJump);
        emit(Op::POP, 0, line);
        patchJump(jmp);
    }
    else
    {
        compileExpr(*e.expr);
        pushToResult();
    }

    for (size_t ci : loops_.back().continueJumps)
        chunk().patch(ci, static_cast<int32_t>(chunk().code.size()) -
                              static_cast<int32_t>(ci) - 1);

    endScope(line);
    emit(Op::LOOP, static_cast<int>(chunk().code.size()) - loopStart + 1, line);
    patchJump(exitJump);
    endScope(line);
    endLoop();

    emitLoad(resultName, line);
    endScope(line);
    emit(Op::RETURN, 0, line);
    emit(Op::RETURN_NIL, 0, line);

    auto result = fnState.chunk;
    result->upvalueCount = static_cast<int>(fnState.upvalues.size());
    auto uvDescs = std::make_shared<Array>();
    for (auto &uv : fnState.upvalues)
    {
        auto desc = std::make_shared<Array>();
        desc->push_back(QuantumValue(uv.isLocal ? 1.0 : 0.0));
        desc->push_back(QuantumValue(static_cast<double>(uv.index)));
        uvDescs->push_back(QuantumValue(desc));
    }
    result->constants.push_back(QuantumValue(uvDescs));

    current_ = prev;

    auto closureTpl = std::make_shared<Closure>(result);
    emit(Op::LOAD_CONST, addConst(QuantumValue(closureTpl)), line);
    emit(result->upvalueCount > 0 ? Op::MAKE_CLOSURE : Op::MAKE_FUNCTION, 0, line);
    emit(Op::CALL, 0, line);
}

void Compiler::compileSuper(SuperExpr &e, int line)
{
    // Standalone super() or super.method access (not a call)
    // For super.method() calls, compileCall handles it directly.
    emitLoad("self", line);
    if (!e.method.empty())
        emit(Op::GET_SUPER, addStr(e.method), line);
}

void Compiler::compileNew(NewExpr &e, int line)
{
    emitLoad(e.typeName, line);
    for (auto &arg : e.args)
        compileExpr(*arg);
    emit(Op::INSTANCE_NEW, static_cast<int32_t>(e.args.size()), line);
}

void Compiler::compileAddressOf(AddressOfExpr &e, int line)
{
    compileExpr(*e.operand);
    emit(Op::ADDRESS_OF, 0, line);
}

void Compiler::compileDeref(DerefExpr &e, int line)
{
    compileExpr(*e.operand);
    emit(Op::DEREF, 0, line);
}

void Compiler::compileArrow(ArrowExpr &e, int line)
{
    compileExpr(*e.object);
    emit(Op::ARROW, 0, line);
    emit(Op::GET_MEMBER, addStr(e.member), line);
}
