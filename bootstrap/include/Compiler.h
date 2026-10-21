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
        std::string cType = ""; // declared C/C++ type ("double", "Calculator"), if any
    };

    struct CompilerState
    {
        std::shared_ptr<Chunk> chunk;
        std::vector<Local> locals;
        std::vector<UpvalueDesc> upvalues;
        int scopeDepth = 0;
        CompilerState *enclosing = nullptr;
        bool isFunction = false;
        int tryDepth = 0; // enclosing try bodies (each holds a pushed handler)

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
    void compileImport(ImportStmt &s, int line);
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
    // Java front-end intrinsics: calls to __jx_*__ names compiled to the
    // J_* opcodes (src/java/ emits them). False when `e` is not one.
    bool compileJavaIntrinsic(CallExpr &e, int line);

    // Helper: compile a function body into a new Chunk
    std::shared_ptr<Chunk> compileFunction(
        const std::string &name,
        const std::vector<std::string> &params,
        const std::vector<bool> &paramIsRef,
        const std::vector<ASTNodePtr> &defaultArgs,
        ASTNode *body,
        int line,
        bool pythonScope = false); // `def`: assigned-first names are locals

    // Helper: emit variable load/store based on scope resolution
    void emitLoad(const std::string &name, int line);
    void emitStore(const std::string &name, int line);

    // ── C/C++ static types ────────────────────────────────────────────────────
    // Numbers carry no int/double distinction at runtime, but an ostream
    // prints them differently (a double holding 1e6 as "1e+06", an int as
    // "1000000"), so `cout << x` / `ss << x` pass along what the declarations
    // say about x.
    std::vector<std::string> pendingParamCTypes_; // for the next compileFunction
    std::unordered_map<std::string, std::string> globalCTypes_;
    std::unordered_map<std::string, std::string> fnReturnCTypes_; // free functions
    // class → method → return type, and class → field → type
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classMethodCTypes_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classFieldCTypes_;
    std::vector<std::string> classStack_; // classes whose methods are being compiled
    void collectCTypes(ASTNode &node);
    // The declared C/C++ type of a variable or (inside a method) a field, or "".
    std::string declaredCType(const std::string &name);
    // 'f' = statically a C/C++ floating-point value, 'i' = an integer, 0 = unknown.
    char numericKind(ASTNode &expr);
    // How `cin >> target` should extract (see the __cin__ native): 1 a
    // double, 2 an integer, 3 a single char, 0 by the target's current value.
    int cinKind(ASTNode &target);

    // GET_SUPER operand: "<class>\x1f<method>" inside a method of <class>, so
    // the VM starts from that class's base (not the instance's), which keeps
    // super chains through three or more classes from recursing.
    std::string superKey(const std::string &method) const
    {
        return classStack_.empty() ? method : classStack_.back() + '\x1f' + method;
    }

    // Unpacks a "[a,b]" loop-variable pattern held in local `slot`.
    void emitPatternLocals(int slot, const std::string &pattern, int line);

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
        int tryDepth = 0; // try bodies open where the loop starts
    };
    std::vector<LoopInfo> loops_;

    void beginLoop(int startIp);
    // Pops the loop body's locals before a break/continue jumps out of it.
    void emitLoopExitPops(int line);
    void emitBreak(int line);
    void emitContinue(int line);
    void endLoop();
};