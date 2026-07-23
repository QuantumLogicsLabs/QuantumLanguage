#pragma once
#include "AST.h"
#include "Opcode.h"
#include "Value.h"
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <functional>

// ─── Upvalue descriptor (used during compilation) ────────────────────────────
struct UpvalueDesc
{
    bool isLocal; // true = captured from directly enclosing scope's locals
    int index;    // slot index (local) or upvalue index (outer)
};

// ─── Compiler ────────────────────────────────────────────────────────────────
class Compiler
{
public:
    Compiler();

    // Compile an entire program (BlockStmt) and return the top-level Chunk.
    std::shared_ptr<Chunk> compile(ASTNode &root);

private:
    // ── Scope management ──────────────────────────────────────────────────────
    struct Local
    {
        std::string name;
        int depth;       // scope depth at declaration
        bool isCaptured; // promoted to upvalue
    };

    struct CompilerState
    {
        std::shared_ptr<Chunk> chunk;
        std::vector<Local> locals;
        std::vector<UpvalueDesc> upvalues;
        int scopeDepth = 0;
        CompilerState *enclosing = nullptr;
        bool isFunction = false;

        explicit CompilerState(const std::string &name, CompilerState *enc = nullptr)
            : chunk(std::make_shared<Chunk>()), enclosing(enc)
        {
            chunk->name = name;
        }
    };

    CompilerState *current_; // points to active compiler state

    // ── Helpers ───────────────────────────────────────────────────────────────
    Chunk &chunk() { return *current_->chunk; }

    size_t emit(Op op, int32_t operand = 0, int line = 0)
    {
        return chunk().emit(op, operand, line);
    }

    int32_t addConst(QuantumValue v) { return chunk().addConstant(std::move(v)); }
    int32_t addStr(const std::string &s) { return chunk().addString(s); }

    // Emit a placeholder JUMP and return its index for later patching.
    size_t emitJump(Op op, int line = 0)
    {
        return emit(op, 0, line);
    }

    void patchJump(size_t idx)
    {
        // operand = offset from *next* instruction
        int32_t offset = static_cast<int32_t>(chunk().code.size()) - static_cast<int32_t>(idx) - 1;
        chunk().patch(idx, offset);
    }

    // Scope helpers
    void beginScope();
    void endScope(int line = 0);
    int resolveLocal(CompilerState *state, const std::string &name);
    int resolveUpvalue(CompilerState *state, const std::string &name);
    int addUpvalue(CompilerState *state, int index, bool isLocal);
    void declareLocal(const std::string &name, int line = 0);

    // ── Node compilation ──────────────────────────────────────────────────────
    void compileNode(ASTNode &node);
    void compileBlock(BlockStmt &b);
    void compileExpr(ASTNode &node);

    // Statements
    void compileVarDecl(VarDecl &s, int line);
    void compileFunctionDecl(FunctionDecl &s, int line);
    void compileClassDecl(ClassDecl &s, int line);
    void compileIf(IfStmt &s, int line);
    void compileWhile(WhileStmt &s, int line);
    void compileFor(ForStmt &s, int line);
    void compileReturn(ReturnStmt &s, int line);
    void compilePrint(PrintStmt &s, int line);
    void compileInput(InputStmt &s, int line);
    void compileTry(TryStmt &s, int line);
    void compileRaise(RaiseStmt &s, int line);

    // Expressions
    void compileBinary(BinaryExpr &e, int line);
    void compileUnary(UnaryExpr &e, int line);
    void compileAssign(AssignExpr &e, int line);
    void compileCall(CallExpr &e, int line);
    void compileIndex(IndexExpr &e, int line);
    void compileMember(MemberExpr &e, int line);
    void compileArray(ArrayLiteral &e, int line);
    void compileDict(DictLiteral &e, int line);
    void compileTuple(TupleLiteral &e, int line);
    void compileLambda(LambdaExpr &e, int line);
    void compileTernary(TernaryExpr &e, int line);
    void compileListComp(ListComp &e, int line);
    void compileIdentifier(Identifier &e, int line);
    void compileSuper(SuperExpr &e, int line);
    void compileNew(NewExpr &e, int line);
    void compileSlice(SliceExpr &e, int line);
    void compileAddressOf(AddressOfExpr &e, int line);
    void compileDeref(DerefExpr &e, int line);
    void compileArrow(ArrowExpr &e, int line);

    // Helper: compile a function body into a new Chunk
    std::shared_ptr<Chunk> compileFunction(
        const std::string &name,
        const std::vector<std::string> &params,
        const std::vector<bool> &paramIsRef,
        const std::vector<ASTNodePtr> &defaultArgs,
        ASTNode *body,
        int line);

    // Helper: emit variable load/store based on scope resolution
    void emitLoad(const std::string &name, int line);
    void emitStore(const std::string &name, int line);

    // Loop stack for break/continue patching
    struct LoopInfo
    {
        std::vector<size_t> breakJumps;
        std::vector<size_t> continueJumps;
        int loopStart = 0;
        // Scope depth at the loop's own level. `break`/`continue` jump out of
        // the body without running its endScope(), so they must discard the
        // locals it pushed themselves — everything deeper than this.
        int scopeDepth = 0;
    };
    std::vector<LoopInfo> loops_;

    void beginLoop(int startIp);
    // Pops the loop body's locals before a break/continue jumps out of it.
    void emitLoopExitPops(int line);
    void emitBreak(int line);
    void emitContinue(int line);
    void endLoop();
};