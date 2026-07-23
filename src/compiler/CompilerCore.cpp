#include "Compiler.h"
#include "Error.h"
#include "Vm.h"
#include <stdexcept>
#include <algorithm>
#include <unordered_map>

Compiler::Compiler() : current_(nullptr) {}

std::shared_ptr<Chunk> Compiler::compile(ASTNode &root)
{
    CompilerState top("<script>");
    current_ = &top;
    if (root.is<BlockStmt>())
        compileBlock(root.as<BlockStmt>());
    else
        compileNode(root);
    emit(Op::RETURN_NIL, 0, 0);
    return top.chunk;
}

void Compiler::beginScope() { current_->scopeDepth++; }

void Compiler::endScope(int line)
{
    current_->scopeDepth--;
    while (!current_->locals.empty() &&
           current_->locals.back().depth > current_->scopeDepth)
    {
        if (current_->locals.back().isCaptured)
            emit(Op::CLOSE_UPVALUE, 0, line);
        else
            emit(Op::POP, 0, line);
        current_->locals.pop_back();
    }
}

int Compiler::resolveLocal(CompilerState *state, const std::string &name)
{
    for (int i = static_cast<int>(state->locals.size()) - 1; i >= 0; --i)
        if (state->locals[i].name == name)
            return i;
    return -1;
}

int Compiler::addUpvalue(CompilerState *state, int index, bool isLocal)
{
    for (int i = 0; i < static_cast<int>(state->upvalues.size()); ++i)
        if (state->upvalues[i].index == index &&
            state->upvalues[i].isLocal == isLocal)
            return i;
    state->upvalues.push_back({isLocal, index});
    state->chunk->upvalueCount++;
    return static_cast<int>(state->upvalues.size()) - 1;
}

int Compiler::resolveUpvalue(CompilerState *state, const std::string &name)
{
    if (!state->enclosing)
        return -1;
    int local = resolveLocal(state->enclosing, name);
    if (local != -1)
    {
        state->enclosing->locals[local].isCaptured = true;
        return addUpvalue(state, local, true);
    }
    int upvalue = resolveUpvalue(state->enclosing, name);
    if (upvalue != -1)
        return addUpvalue(state, upvalue, false);
    return -1;
}

void Compiler::declareLocal(const std::string &name, int)
{
    if (current_->scopeDepth == 0)
        return;
    current_->locals.push_back({name, current_->scopeDepth, false});
}

void Compiler::emitLoad(const std::string &name, int line)
{
    // "this" is an alias for "self" (slot 0 in all methods)
    const std::string &resolved = (name == "this") ? std::string("self") : name;
    int local = resolveLocal(current_, resolved);
    if (local != -1)
    {
        emit(Op::LOAD_LOCAL, local, line);
        return;
    }
    int uv = resolveUpvalue(current_, resolved);
    if (uv != -1)
    {
        emit(Op::LOAD_UPVALUE, uv, line);
        return;
    }
    emit(Op::LOAD_GLOBAL, addStr(resolved), line);
}

void Compiler::emitStore(const std::string &name, int line)
{
    // "this" is an alias for "self" (slot 0 in all methods)
    const std::string &resolved = (name == "this") ? std::string("self") : name;
    int local = resolveLocal(current_, resolved);
    if (local != -1)
    {
        emit(Op::STORE_LOCAL, local, line);
        return;
    }
    int uv = resolveUpvalue(current_, resolved);
    if (uv != -1)
    {
        emit(Op::STORE_UPVALUE, uv, line);
        return;
    }
    emit(Op::STORE_GLOBAL, addStr(resolved), line);
}

void Compiler::beginLoop(int startIp)
{
    loops_.push_back({});
    loops_.back().loopStart = startIp;
    loops_.back().scopeDepth = current_->scopeDepth;
}

// Locals live on the value stack, so leaving a loop body early has to discard
// everything the body pushed — otherwise those slots leak for the rest of the
// enclosing function. For a for-in loop that leak is silently destructive: the
// hidden `__iter__` sits directly under the loop variable, so a stale slot left
// by `break` makes the *next* for-in loop's FOR_ITER read the wrong iterator.
void Compiler::emitLoopExitPops(int line)
{
    if (loops_.empty())
        return;
    int depth = loops_.back().scopeDepth;
    for (int i = static_cast<int>(current_->locals.size()) - 1;
         i >= 0 && current_->locals[i].depth > depth; --i)
        emit(current_->locals[i].isCaptured ? Op::CLOSE_UPVALUE : Op::POP, 0, line);
}

void Compiler::emitBreak(int line)
{
    emitLoopExitPops(line);
    loops_.back().breakJumps.push_back(emitJump(Op::JUMP, line));
}

void Compiler::emitContinue(int line)
{
    emitLoopExitPops(line);
    loops_.back().continueJumps.push_back(emitJump(Op::JUMP, line));
}

void Compiler::endLoop()
{
    size_t after = chunk().code.size();
    for (size_t idx : loops_.back().breakJumps)
        chunk().patch(idx, static_cast<int32_t>(after) - static_cast<int32_t>(idx) - 1);
    loops_.pop_back();
}

// ─── Node dispatch ────────────────────────────────────────────────────────────

void Compiler::compileNode(ASTNode &node)
{
    int ln = node.line;
    std::visit([&](auto &n)
               {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, BlockStmt>)      compileBlock(n);
        else if constexpr (std::is_same_v<T, VarDecl>)        compileVarDecl(n, ln);
        else if constexpr (std::is_same_v<T, FunctionDecl>)   compileFunctionDecl(n, ln);
        else if constexpr (std::is_same_v<T, ClassDecl>)      compileClassDecl(n, ln);
        else if constexpr (std::is_same_v<T, IfStmt>)         compileIf(n, ln);
        else if constexpr (std::is_same_v<T, WhileStmt>)      compileWhile(n, ln);
        else if constexpr (std::is_same_v<T, ForStmt>)        compileFor(n, ln);
        else if constexpr (std::is_same_v<T, ReturnStmt>)     compileReturn(n, ln);
        else if constexpr (std::is_same_v<T, PrintStmt>)      compilePrint(n, ln);
        else if constexpr (std::is_same_v<T, InputStmt>)      compileInput(n, ln);
        else if constexpr (std::is_same_v<T, TryStmt>)        compileTry(n, ln);
        else if constexpr (std::is_same_v<T, RaiseStmt>)      compileRaise(n, ln);
        else if constexpr (std::is_same_v<T, BreakStmt>)      emitBreak(ln);
        else if constexpr (std::is_same_v<T, ContinueStmt>)   emitContinue(ln);
        else if constexpr (std::is_same_v<T, ImportStmt>)     { /* natives handle imports */ }
        else if constexpr (std::is_same_v<T, ExprStmt>)
        {
            compileExpr(*n.expr);
            emit(Op::POP, 0, ln);
        }
        else { compileExpr(node); emit(Op::POP, 0, ln); } }, node.node);
}

void Compiler::compileBlock(BlockStmt &b)
{
    if (current_->scopeDepth > 0)
    {
        for (auto &stmt : b.statements)
        {
            if (!stmt->is<FunctionDecl>())
                continue;
            auto &fn = stmt->as<FunctionDecl>();
            if (resolveLocal(current_, fn.name) != -1)
                continue;
            emit(Op::LOAD_NIL, 0, stmt->line);
            declareLocal(fn.name, stmt->line);
            emit(Op::DEFINE_LOCAL, static_cast<int>(current_->locals.size()) - 1, stmt->line);
        }
    }

    for (auto &stmt : b.statements)
        compileNode(*stmt);
}

void Compiler::compileExpr(ASTNode &node)
{
    int ln = node.line;
    std::visit([&](auto &n)
               {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, NumberLiteral>)
            emit(Op::LOAD_CONST, addConst(QuantumValue(n.value)), ln);
        else if constexpr (std::is_same_v<T, StringLiteral>)
            emit(Op::LOAD_CONST, addConst(QuantumValue(n.value)), ln);
        else if constexpr (std::is_same_v<T, BoolLiteral>)
            emit(n.value ? Op::LOAD_TRUE : Op::LOAD_FALSE, 0, ln);
        else if constexpr (std::is_same_v<T, NilLiteral>)
            emit(Op::LOAD_NIL, 0, ln);
        else if constexpr (std::is_same_v<T, Identifier>)    compileIdentifier(n, ln);
        else if constexpr (std::is_same_v<T, BinaryExpr>)    compileBinary(n, ln);
        else if constexpr (std::is_same_v<T, UnaryExpr>)     compileUnary(n, ln);
        else if constexpr (std::is_same_v<T, AssignExpr>)    compileAssign(n, ln);
        else if constexpr (std::is_same_v<T, CallExpr>)      compileCall(n, ln);
        else if constexpr (std::is_same_v<T, IndexExpr>)     compileIndex(n, ln);
        else if constexpr (std::is_same_v<T, SliceExpr>)     compileSlice(n, ln);
        else if constexpr (std::is_same_v<T, MemberExpr>)    compileMember(n, ln);
        else if constexpr (std::is_same_v<T, ArrayLiteral>)  compileArray(n, ln);
        else if constexpr (std::is_same_v<T, DictLiteral>)   compileDict(n, ln);
        else if constexpr (std::is_same_v<T, TupleLiteral>)  compileTuple(n, ln);
        else if constexpr (std::is_same_v<T, LambdaExpr>)    compileLambda(n, ln);
        else if constexpr (std::is_same_v<T, TernaryExpr>)   compileTernary(n, ln);
        else if constexpr (std::is_same_v<T, ListComp>)      compileListComp(n, ln);
        else if constexpr (std::is_same_v<T, SuperExpr>)     compileSuper(n, ln);
        else if constexpr (std::is_same_v<T, NewExpr>)       compileNew(n, ln);
        else if constexpr (std::is_same_v<T, AddressOfExpr>) compileAddressOf(n, ln);
        else if constexpr (std::is_same_v<T, DerefExpr>)     compileDeref(n, ln);
        else if constexpr (std::is_same_v<T, ArrowExpr>)     compileArrow(n, ln);
        else throw std::runtime_error("Compiler: unhandled expression node"); }, node.node);
}

// ─── Statements ──────────────────────────────────────────────────────────────

