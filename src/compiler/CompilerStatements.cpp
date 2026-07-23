#include "Compiler.h"
#include "Error.h"
#include "Vm.h"
#include <stdexcept>
#include <algorithm>
#include <unordered_map>

void Compiler::compileVarDecl(VarDecl &s, int line)
{
    if (s.initializer)
        compileExpr(*s.initializer);
    // C/C++ typed declarations default like value-initialisation:
    // string/char → "", bool → false, numeric types → 0
    else if (!s.isPointer && (s.typeHint == "string" || s.typeHint == "char"))
        emit(Op::LOAD_CONST, addConst(QuantumValue(std::string(""))), line);
    else if (!s.isPointer && s.typeHint == "bool")
        emit(Op::LOAD_FALSE, 0, line);
    else if (!s.isPointer && !s.typeHint.empty() &&
             s.typeHint.find("[]") == std::string::npos &&
             (s.typeHint.rfind("int", 0) == 0 || s.typeHint == "float" ||
              s.typeHint == "double" || s.typeHint.rfind("long", 0) == 0 ||
              s.typeHint == "short" || s.typeHint.rfind("unsigned", 0) == 0 ||
              s.typeHint == "size_t"))
        emit(Op::LOAD_CONST, addConst(QuantumValue(0.0)), line);
    else
        emit(Op::LOAD_NIL, 0, line);

    if (current_->scopeDepth == 0)
    {
        emit(s.isConst ? Op::DEFINE_CONST : Op::DEFINE_GLOBAL, addStr(s.name), line);
    }
    else
    {
        declareLocal(s.name, line);
        emit(Op::DEFINE_LOCAL, static_cast<int>(current_->locals.size()) - 1, line);
    }
}

void Compiler::compileFunctionDecl(FunctionDecl &s, int line)
{
    auto fnChunk = compileFunction(s.name, s.params, s.paramIsRef, s.defaultArgs, s.body.get(), line);
    auto closureTpl = std::make_shared<Closure>(fnChunk);
    emit(Op::LOAD_CONST, addConst(QuantumValue(closureTpl)), line);
    emit(fnChunk->upvalueCount > 0 ? Op::MAKE_CLOSURE : Op::MAKE_FUNCTION, 0, line);
    if (current_->scopeDepth == 0)
    {
        emit(Op::DEFINE_GLOBAL, addStr(s.name), line);
    }
    else
    {
        int existingSlot = resolveLocal(current_, s.name);
        if (existingSlot != -1 && current_->locals[existingSlot].depth == current_->scopeDepth)
        {
            emit(Op::STORE_LOCAL, existingSlot, line);
            emit(Op::POP, 0, line);
        }
        else
        {
            declareLocal(s.name, line);
            emit(Op::DEFINE_LOCAL, static_cast<int>(current_->locals.size()) - 1, line);
        }
    }
}

void Compiler::compileClassDecl(ClassDecl &s, int line)
{
    emit(Op::LOAD_CONST, addConst(QuantumValue(s.name)), line);
    emit(Op::MAKE_CLASS, 0, line);

    if (!s.base.empty())
    {
        emitLoad(s.base, line);
        emit(Op::INHERIT, 0, line);
    }

    auto bindClassField = [&](ASTNodePtr &member)
    {
        if (member->is<ClassDecl>())
        {
            auto &nested = member->as<ClassDecl>();
            compileClassDecl(nested, member->line);
            emitLoad(nested.name, member->line);
            emit(Op::BIND_METHOD, addStr(nested.name), member->line);
            return true;
        }
        if (member->is<VarDecl>())
        {
            auto &field = member->as<VarDecl>();
            if (field.initializer)
                compileExpr(*field.initializer);
            else
                emit(Op::LOAD_NIL, 0, member->line);
            emit(Op::BIND_METHOD, addStr(field.name), member->line);
            return true;
        }
        if (member->is<ExprStmt>() &&
            member->as<ExprStmt>().expr &&
            member->as<ExprStmt>().expr->is<AssignExpr>())
        {
            auto &assign = member->as<ExprStmt>().expr->as<AssignExpr>();
            if (assign.target->is<Identifier>())
            {
                compileExpr(*assign.value);
                emit(Op::BIND_METHOD, addStr(assign.target->as<Identifier>().name), member->line);
                return true;
            }
        }
        return false;
    };

    for (auto &field : s.fields)
        bindClassField(field);

    for (auto &method : s.methods)
    {
        if (bindClassField(method))
            continue;
        if (!method->is<FunctionDecl>())
            continue;
        auto &fd = method->as<FunctionDecl>();

        // Prepend "self" as slot 0 so the instance is always at the first local.
        // The VM calls methods with the instance as the first argument (argCount+1).
        // "this" references are resolved to "self" by emitLoad/emitStore.
        std::vector<std::string> methodParams;
        std::vector<bool> methodRefs;
        size_t startIndex = 0;
        if (fd.params.empty() || (fd.params[0] != "self" && fd.params[0] != "this"))
        {
            methodParams.push_back("self");
            methodRefs.push_back(false);
        }
        else
        {
            methodParams.push_back("self");
            methodRefs.push_back(!fd.paramIsRef.empty() && fd.paramIsRef[0]);
            startIndex = 1;
        }
        for (size_t i = startIndex; i < fd.params.size(); ++i)
        {
            methodParams.push_back(fd.params[i]);
            methodRefs.push_back(i < fd.paramIsRef.size() ? fd.paramIsRef[i] : false);
        }

        auto fnChunk = compileFunction(fd.name, methodParams, methodRefs, fd.defaultArgs, fd.body.get(), method->line);
        auto closureTpl = std::make_shared<Closure>(fnChunk);
        emit(Op::LOAD_CONST, addConst(QuantumValue(closureTpl)), method->line);
        // A method body may itself contain a nested closure capturing the
        // method's locals as upvalues (e.g. `arr.each(fn(x){ ...local... })`);
        // that only works if the method is created with MAKE_CLOSURE so its
        // upvalue cells exist. Plain methods keep the cheaper MAKE_FUNCTION.
        emit(fnChunk->upvalueCount > 0 ? Op::MAKE_CLOSURE : Op::MAKE_FUNCTION, 0, method->line);
        emit(Op::BIND_METHOD, addStr(fd.name), method->line);
    }

    if (current_->scopeDepth == 0)
    {
        emit(Op::DEFINE_GLOBAL, addStr(s.name), line);
    }
    else
    {
        declareLocal(s.name, line);
        emit(Op::DEFINE_LOCAL, static_cast<int>(current_->locals.size()) - 1, line);
    }
}

void Compiler::compileIf(IfStmt &s, int line)
{
    compileExpr(*s.condition);
    size_t thenJump = emitJump(Op::JUMP_IF_FALSE, line);
    emit(Op::POP, 0, line);
    beginScope();
    compileNode(*s.thenBranch);
    endScope(line);
    size_t elseJump = emitJump(Op::JUMP, line);
    patchJump(thenJump);
    emit(Op::POP, 0, line);
    if (s.elseBranch)
    {
        beginScope();
        compileNode(*s.elseBranch);
        endScope(line);
    }
    patchJump(elseJump);
}

void Compiler::compileWhile(WhileStmt &s, int line)
{
    int loopStart = static_cast<int>(chunk().code.size());
    beginLoop(loopStart);

    compileExpr(*s.condition);
    size_t exitJump = emitJump(Op::JUMP_IF_FALSE, line);
    emit(Op::POP, 0, line);

    beginScope();
    compileNode(*s.body);
    endScope(line);

    for (size_t ci : loops_.back().continueJumps)
        chunk().patch(ci, static_cast<int32_t>(chunk().code.size()) - static_cast<int32_t>(ci) - 1);

    emit(Op::LOOP, static_cast<int>(chunk().code.size()) - loopStart + 1, line);
    patchJump(exitJump);
    emit(Op::POP, 0, line);
    endLoop();
}

void Compiler::compileFor(ForStmt &s, int line)
{
    // Outer scope: holds the iterator as a hidden local (survives loop iterations)
    compileExpr(*s.iterable);
    emit(Op::MAKE_ITER, 0, line);

    beginScope(); // outer scope — iterator lives here
    declareLocal("__iter__", line);
    int iterSlot = static_cast<int>(current_->locals.size()) - 1;
    emit(Op::DEFINE_LOCAL, iterSlot, line);

    int loopStart = static_cast<int>(chunk().code.size());
    beginLoop(loopStart);

    // FOR_ITER: peeks iterator from stack top (it's the last outer-scope local).
    // If exhausted, jump past the loop. Otherwise push the next element.
    size_t exitJump = emitJump(Op::FOR_ITER, line);

    // Inner scope: holds the loop variable (popped at end of each iteration)
    beginScope();
    declareLocal(s.var, line);
    int varSlot = static_cast<int>(current_->locals.size()) - 1;
    emit(Op::DEFINE_LOCAL, varSlot, line);

    if (!s.var2.empty())
    {
        emit(Op::LOAD_LOCAL, varSlot, line);
        emit(Op::LOAD_CONST, addConst(QuantumValue(1.0)), line);
        emit(Op::GET_INDEX, 0, line);
        declareLocal(s.var2, line);
        emit(Op::DEFINE_LOCAL, static_cast<int>(current_->locals.size()) - 1, line);
        emit(Op::LOAD_LOCAL, varSlot, line);
        emit(Op::LOAD_CONST, addConst(QuantumValue(0.0)), line);
        emit(Op::GET_INDEX, 0, line);
        emit(Op::STORE_LOCAL, varSlot, line);
        emit(Op::POP, 0, line);
    }

    compileNode(*s.body);

    // End inner scope: pops loop variable(s) only, leaving iterator on stack
    endScope(line);

    // `continue` lands *after* that cleanup, matching the stack state at the
    // back-edge below — it discarded the body's locals itself (emitContinue),
    // so letting it fall through endScope's POPs would double-pop them.
    for (size_t ci : loops_.back().continueJumps)
        chunk().patch(ci, static_cast<int32_t>(chunk().code.size()) - static_cast<int32_t>(ci) - 1);

    // Jump back to FOR_ITER (iterator is still on stack top)
    emit(Op::LOOP, static_cast<int>(chunk().code.size()) - loopStart + 1, line);

    patchJump(exitJump);
    // Patch `break` to land here — *before* the outer scope ends — so both
    // the exhausted path and the break path fall into the same endScope()
    // and the hidden iterator is popped exactly once either way. (The body's
    // own locals were already discarded at the break site by emitBreak.)
    endLoop();
    // End outer scope: pops the iterator
    endScope(line);
}

void Compiler::compileReturn(ReturnStmt &s, int line)
{
    if (s.value)
    {
        compileExpr(*s.value);
        emit(Op::RETURN, 0, line);
    }
    else
        emit(Op::RETURN_NIL, 0, line);
}

void Compiler::compilePrint(PrintStmt &s, int line)
{
    for (auto &arg : s.args)
        compileExpr(*arg);
    emit(Op::LOAD_CONST, addStr(s.sep), line);
    emit(Op::LOAD_CONST, addStr(s.end), line);
    emit(Op::PRINT, static_cast<int32_t>(s.args.size()), line);
}

void Compiler::compileInput(InputStmt &s, int line)
{
    emit(Op::LOAD_GLOBAL, addStr("__input__"), line);
    if (s.prompt)
        compileExpr(*s.prompt);
    else
        emit(Op::LOAD_CONST, addStr(""), line);
    emit(Op::CALL, 1, line);
    if (!s.target.empty())
    {
        emitStore(s.target, line);
        emit(Op::POP, 0, line);
    }
    else
        emit(Op::POP, 0, line);
}

void Compiler::compileTry(TryStmt &s, int line)
{
    size_t handlerJump = emitJump(Op::PUSH_HANDLER, line);
    if (s.body)
        compileNode(*s.body);
    emit(Op::POP_HANDLER, 0, line);
    size_t afterHandlers = emitJump(Op::JUMP, line);
    patchJump(handlerJump);

    for (auto &h : s.handlers)
    {
        beginScope();
        // alias = 'as e' syntax; errorType used as var name for 'except (e)' syntax
        std::string varName = h.alias.empty() ? h.errorType : h.alias;
        if (!varName.empty())
        {
            declareLocal(varName, line);
            emit(Op::DEFINE_LOCAL, static_cast<int>(current_->locals.size()) - 1, line);
        }
        else
        {
            emit(Op::POP, 0, line);
        }
        if (h.body)
            compileNode(*h.body);
        endScope(line);
    }

    patchJump(afterHandlers);
    if (s.finallyBody)
        compileNode(*s.finallyBody);
}

void Compiler::compileRaise(RaiseStmt &s, int line)
{
    if (s.value)
        compileExpr(*s.value);
    else
        emit(Op::LOAD_NIL, 0, line);
    emit(Op::RAISE, 0, line);
}

// ─── Expressions ─────────────────────────────────────────────────────────────

void Compiler::compileIdentifier(Identifier &e, int line) { emitLoad(e.name, line); }

