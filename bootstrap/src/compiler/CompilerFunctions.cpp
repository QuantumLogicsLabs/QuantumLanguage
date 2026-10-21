#include "Compiler.h"
#include "Error.h"
#include "Vm.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace {
// Python scoping for a `def` body: a name is local to the function when its
// first occurrence, in evaluation order, is a plain assignment (`x = ...`,
// `a, b = ...`, `with ... as x`). A name read first — `counter = counter + 1`,
// `total += 1` — keeps resolving outside the function, which forgives code
// that updates a global without declaring it; `global`/`nonlocal` names are
// never local. Nested functions, lambdas and classes are separate scopes and
// are not scanned.
struct PyLocalScan {
  std::unordered_set<std::string> seen;
  std::unordered_set<std::string> excluded; // global / nonlocal
  std::vector<std::string> locals;

  void write(const std::string &name) {
    if (!seen.count(name) && !excluded.count(name))
      locals.push_back(name);
    seen.insert(name);
  }
  void visit(ASTNode *n) {
    if (!n)
      return;
    std::visit(
        [&](auto &v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, Identifier>)
            seen.insert(v.name);
          else if constexpr (std::is_same_v<T, GlobalStmt>)
            for (auto &name : v.names) {
              excluded.insert(name);
              seen.insert(name);
            }
          else if constexpr (std::is_same_v<T, AssignExpr>) {
            visit(v.value.get()); // the right side runs first
            if (v.op == "kwarg")
              return; // f(key=value) binds a parameter, not a variable
            if (v.target && v.target->template is<Identifier>()) {
              const std::string &name = v.target->template as<Identifier>().name;
              if (v.op == "=")
                write(name);
              else
                seen.insert(name); // `x += 1` reads x first
            } else if (v.target && v.target->template is<TupleLiteral>()) {
              for (auto &el : v.target->template as<TupleLiteral>().elements)
                if (el->template is<Identifier>())
                  write(el->template as<Identifier>().name);
            } else
              visit(v.target.get()); // obj.f = ..., a[i] = ...
          } else if constexpr (std::is_same_v<T, BinaryExpr>) {
            visit(v.left.get());
            visit(v.right.get());
          } else if constexpr (std::is_same_v<T, UnaryExpr>)
            visit(v.operand.get());
          else if constexpr (std::is_same_v<T, CallExpr>) {
            visit(v.callee.get());
            for (auto &a : v.args)
              visit(a.get());
          } else if constexpr (std::is_same_v<T, IndexExpr>) {
            visit(v.object.get());
            visit(v.index.get());
          } else if constexpr (std::is_same_v<T, SliceExpr>) {
            visit(v.object.get());
            visit(v.start.get());
            visit(v.stop.get());
            visit(v.step.get());
          } else if constexpr (std::is_same_v<T, MemberExpr>)
            visit(v.object.get());
          else if constexpr (std::is_same_v<T, ArrowExpr>)
            visit(v.object.get());
          else if constexpr (std::is_same_v<T, ArrayLiteral> ||
                             std::is_same_v<T, TupleLiteral>)
            for (auto &e : v.elements)
              visit(e.get());
          else if constexpr (std::is_same_v<T, DictLiteral>)
            for (auto &kv : v.pairs) {
              visit(kv.first.get());
              visit(kv.second.get());
            }
          else if constexpr (std::is_same_v<T, ListComp>) {
            visit(v.iterable.get());
            visit(v.expr.get());
            visit(v.condition.get());
          } else if constexpr (std::is_same_v<T, TernaryExpr>) {
            visit(v.condition.get());
            visit(v.thenExpr.get());
            visit(v.elseExpr.get());
          } else if constexpr (std::is_same_v<T, AddressOfExpr> ||
                               std::is_same_v<T, DerefExpr>)
            visit(v.operand.get());
          else if constexpr (std::is_same_v<T, NewExpr>) {
            for (auto &a : v.args)
              visit(a.get());
            visit(v.sizeExpr.get());
          } else if constexpr (std::is_same_v<T, VarDecl>)
            visit(v.initializer.get());
          else if constexpr (std::is_same_v<T, ReturnStmt>)
            visit(v.value.get());
          else if constexpr (std::is_same_v<T, RaiseStmt>)
            visit(v.value.get());
          else if constexpr (std::is_same_v<T, IfStmt>) {
            visit(v.condition.get());
            visit(v.thenBranch.get());
            visit(v.elseBranch.get());
          } else if constexpr (std::is_same_v<T, WhileStmt>) {
            visit(v.condition.get());
            visit(v.body.get());
            visit(v.post.get());
          } else if constexpr (std::is_same_v<T, ForStmt>) {
            visit(v.iterable.get());
            visit(v.body.get());
          } else if constexpr (std::is_same_v<T, BlockStmt>)
            for (auto &s : v.statements)
              visit(s.get());
          else if constexpr (std::is_same_v<T, ExprStmt>)
            visit(v.expr.get());
          else if constexpr (std::is_same_v<T, PrintStmt>)
            for (auto &a : v.args)
              visit(a.get());
          else if constexpr (std::is_same_v<T, TryStmt>) {
            visit(v.body.get());
            for (auto &h : v.handlers)
              visit(h.body.get());
            visit(v.finallyBody.get());
          }
          // FunctionDecl / LambdaExpr / ClassDecl: their own scopes.
        },
        n->node);
  }
};
} // namespace

std::shared_ptr<Chunk> Compiler::compileFunction(
    const std::string &name, const std::vector<std::string> &params,
    const std::vector<bool> &paramIsRef,
    const std::vector<ASTNodePtr> &defaultArgs, ASTNode *body, int line,
    bool pythonScope) {
  CompilerState fnState(name, current_);
  fnState.isFunction = true;
  CompilerState *prev = current_;
  current_ = &fnState;

  beginScope();
  for (auto &p : params) {
    std::string localName = p;
    while (!localName.empty() && localName[0] == '*')
      localName.erase(0, 1);
    declareLocal(localName, line);
  }
  // Declared C/C++ parameter types (set by the caller for this function only).
  for (size_t i = 0; i < pendingParamCTypes_.size() && i < fnState.locals.size(); ++i)
    fnState.locals[i].cType = pendingParamCTypes_[i];
  pendingParamCTypes_.clear();

  fnState.chunk->params = params;
  fnState.chunk->paramIsRef =
      paramIsRef.empty() ? std::vector<bool>(params.size(), false) : paramIsRef;

  for (size_t i = 0; i < defaultArgs.size(); ++i) {
    if (defaultArgs[i]) {
      // Only an argument the caller omitted takes the default; an explicit
      // nil is a real value (`inorder(node.left, acc)` with a nil child).
      emit(Op::ARG_PASSED, static_cast<int32_t>(i), line);
      size_t jumpIfPassed = emitJump(Op::JUMP_IF_TRUE, line);
      emit(Op::POP, 0, line); // pop false
      compileExpr(*defaultArgs[i]);
      emit(Op::STORE_LOCAL, static_cast<int32_t>(i), line);
      emit(Op::POP, 0, line); // pop assigned value
      size_t jumpEnd = emitJump(Op::JUMP, line);
      patchJump(jumpIfPassed);
      emit(Op::POP, 0, line); // pop true
      patchJump(jumpEnd);
    }
  }

  for (size_t paramIndex = 0; paramIndex < params.size(); ++paramIndex) {
    const std::string &param = params[paramIndex];
    if (param.size() < 2 || param.front() != '[' || param.back() != ']')
      continue;

    std::string currentName;
    int elementIndex = 0;
    auto flushElement = [&]() {
      if (currentName.empty()) {
        ++elementIndex;
        return;
      }
      emit(Op::LOAD_LOCAL, static_cast<int32_t>(paramIndex), line);
      emit(Op::LOAD_CONST,
           addConst(QuantumValue(static_cast<double>(elementIndex))), line);
      emit(Op::GET_INDEX, 0, line);
      declareLocal(currentName, line);
      emit(Op::DEFINE_LOCAL, static_cast<int>(current_->locals.size()) - 1,
           line);
      currentName.clear();
      ++elementIndex;
    };

    for (size_t i = 1; i + 1 < param.size(); ++i) {
      char ch = param[i];
      if (ch == ',') {
        flushElement();
        continue;
      }
      if (!std::isspace(static_cast<unsigned char>(ch)))
        currentName += ch;
    }
    flushElement();
  }

  if (pythonScope && body) {
    PyLocalScan scan;
    for (auto &p : params) {
      std::string plain = p;
      while (!plain.empty() && plain[0] == '*')
        plain.erase(0, 1);
      scan.seen.insert(plain); // parameters are locals already
    }
    scan.visit(body);
    for (auto &local : scan.locals) {
      if (resolveLocal(current_, local) != -1)
        continue;
      emit(Op::LOAD_NIL, 0, line);
      declareLocal(local, line);
      emit(Op::DEFINE_LOCAL, static_cast<int>(current_->locals.size()) - 1,
           line);
    }
  }

  if (body) {
    if (body->is<BlockStmt>())
      compileBlock(body->as<BlockStmt>());
    else {
      compileExpr(*body);
      emit(Op::RETURN, 0, line);
    }
  }
  emit(Op::RETURN_NIL, 0, line);
  endScope(line);

  auto result = fnState.chunk;
  result->upvalueCount = static_cast<int>(fnState.upvalues.size());

  // Pack upvalue descriptors as the last constant for MAKE_CLOSURE
  auto uvDescs = std::make_shared<Array>();
  for (auto &uv : fnState.upvalues) {
    auto desc = std::make_shared<Array>();
    desc->push_back(QuantumValue(uv.isLocal ? 1.0 : 0.0));
    desc->push_back(QuantumValue(static_cast<double>(uv.index)));
    uvDescs->push_back(QuantumValue(desc));
  }
  result->constants.push_back(QuantumValue(uvDescs));

  current_ = prev;
  return result;
}
