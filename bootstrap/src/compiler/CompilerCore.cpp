#include "Compiler.h"
#include "Error.h"
#include "Vm.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <unordered_map>

Compiler::Compiler() : current_(nullptr) {}

std::shared_ptr<Chunk> Compiler::compile(ASTNode &root) {
  CompilerState top("<script>");
  current_ = &top;
  collectCTypes(root);
  if (root.is<BlockStmt>())
    compileBlock(root.as<BlockStmt>());
  else
    compileNode(root);
  emit(Op::RETURN_NIL, 0, 0);
  return top.chunk;
}

// ─── C/C++ static types (see Compiler.h) ─────────────────────────────────────

// Return types of free functions and methods, and field types, declared
// anywhere in the program — calls may precede the definitions.
void Compiler::collectCTypes(ASTNode &node) {
  if (node.is<BlockStmt>()) {
    for (auto &st : node.as<BlockStmt>().statements)
      if (st)
        collectCTypes(*st);
  } else if (node.is<FunctionDecl>()) {
    auto &fd = node.as<FunctionDecl>();
    if (!fd.cReturnType.empty())
      fnReturnCTypes_[fd.name] = fd.cReturnType;
  } else if (node.is<ClassDecl>()) {
    auto &cd = node.as<ClassDecl>();
    auto &methods = classMethodCTypes_[cd.name];
    auto &fields = classFieldCTypes_[cd.name];
    auto member = [&](ASTNodePtr &m) {
      if (!m)
        return;
      if (m->is<FunctionDecl>() && !m->as<FunctionDecl>().cReturnType.empty())
        methods[m->as<FunctionDecl>().name] = m->as<FunctionDecl>().cReturnType;
      else if (m->is<VarDecl>() && !m->as<VarDecl>().typeHint.empty())
        fields[m->as<VarDecl>().name] = m->as<VarDecl>().typeHint;
      else if (m->is<ClassDecl>())
        collectCTypes(*m);
    };
    for (auto &f : cd.fields)
      member(f);
    for (auto &m : cd.methods)
      member(m);
    // Inherited members (a base class is declared before its subclasses).
    if (!cd.base.empty()) {
      for (auto &[k, v] : classMethodCTypes_[cd.base])
        methods.emplace(k, v);
      for (auto &[k, v] : classFieldCTypes_[cd.base])
        fields.emplace(k, v);
    }
  }
}

std::string Compiler::declaredCType(const std::string &name) {
  for (CompilerState *st = current_; st; st = st->enclosing)
    for (auto it = st->locals.rbegin(); it != st->locals.rend(); ++it)
      if (it->name == name)
        return it->cType;
  if (!classStack_.empty()) {
    auto c = classFieldCTypes_.find(classStack_.back());
    if (c != classFieldCTypes_.end()) {
      auto f = c->second.find(name);
      if (f != c->second.end())
        return f->second;
    }
  }
  auto g = globalCTypes_.find(name);
  return g == globalCTypes_.end() ? "" : g->second;
}

static char cTypeKind(std::string t) {
  for (const char *q : {"const ", "static ", "constexpr ", "volatile "})
    while (t.rfind(q, 0) == 0)
      t.erase(0, std::string(q).size());
  if (t.find('*') != std::string::npos || t.find('[') != std::string::npos)
    return 0;
  if (t == "double" || t == "float" || t == "long double")
    return 'f';
  static const std::set<std::string> ints = {
      "int", "long", "short", "unsigned", "signed", "size_t", "ssize_t", "ptrdiff_t",
      "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t"};
  if (ints.count(t) || t.rfind("unsigned ", 0) == 0 || t.rfind("signed ", 0) == 0 ||
      t.rfind("long ", 0) == 0 || t.rfind("short ", 0) == 0)
    return t.find("double") != std::string::npos ? 'f' : 'i';
  return 0;
}

int Compiler::cinKind(ASTNode &target) {
  std::string type;
  if (target.is<Identifier>())
    type = declaredCType(target.as<Identifier>().name);
  else if ((target.is<MemberExpr>() || target.is<ArrowExpr>()) && !classStack_.empty()) {
    auto &obj = target.is<MemberExpr>() ? *target.as<MemberExpr>().object : *target.as<ArrowExpr>().object;
    const std::string &member =
        target.is<MemberExpr>() ? target.as<MemberExpr>().member : target.as<ArrowExpr>().member;
    if (obj.is<Identifier>() && (obj.as<Identifier>().name == "self" || obj.as<Identifier>().name == "this")) {
      auto c = classFieldCTypes_.find(classStack_.back());
      if (c != classFieldCTypes_.end() && c->second.count(member))
        type = c->second.at(member);
    }
  } else if (target.is<IndexExpr>() && target.as<IndexExpr>().object->is<Identifier>()) {
    type = declaredCType(target.as<IndexExpr>().object->as<Identifier>().name);
    while (!type.empty() && (type.back() == '*' || type.back() == ']' || type.back() == '['))
      type.pop_back();
  }
  if (type.rfind("const ", 0) == 0)
    type.erase(0, 6);
  if (type == "char")
    return 3;
  switch (cTypeKind(type)) {
  case 'f':
    return 1;
  case 'i':
    return 2;
  default:
    return 0;
  }
}

char Compiler::numericKind(ASTNode &e) {
  // The class a (pointer-)typed object expression refers to, if known.
  auto classOf = [&](ASTNode &obj) -> std::string {
    if (!obj.is<Identifier>())
      return "";
    const std::string &n = obj.as<Identifier>().name;
    if (n == "self" || n == "this")
      return classStack_.empty() ? "" : classStack_.back();
    std::string t = declaredCType(n);
    while (!t.empty() && (t.back() == '*' || t.back() == '&'))
      t.pop_back();
    return t;
  };
  auto lookup = [](auto &table, const std::string &cls, const std::string &name) -> std::string {
    auto c = table.find(cls);
    if (c == table.end())
      return "";
    auto m = c->second.find(name);
    return m == c->second.end() ? "" : m->second;
  };

  if (e.is<Identifier>())
    return cTypeKind(declaredCType(e.as<Identifier>().name));
  if (e.is<NumberLiteral>()) {
    double v = e.as<NumberLiteral>().value;
    return std::isfinite(v) && std::floor(v) != v ? 'f' : 0;
  }
  if (e.is<BinaryExpr>()) {
    auto &b = e.as<BinaryExpr>();
    if (b.op != "+" && b.op != "-" && b.op != "*" && b.op != "/" && b.op != "%")
      return 0;
    char l = numericKind(*b.left), r = numericKind(*b.right);
    if (l == 'f' || r == 'f')
      return b.op == "%" ? 0 : 'f';
    return l == 'i' && r == 'i' ? 'i' : 0;
  }
  if (e.is<UnaryExpr>()) {
    auto &u = e.as<UnaryExpr>();
    return (u.op == "-" || u.op == "+") ? numericKind(*u.operand) : 0;
  }
  if (e.is<TernaryExpr>()) {
    auto &t = e.as<TernaryExpr>();
    char a = numericKind(*t.thenExpr), b = numericKind(*t.elseExpr);
    if (a == 'f' || b == 'f')
      return 'f';
    return a == 'i' && b == 'i' ? 'i' : 0;
  }
  if (e.is<MemberExpr>())
    return cTypeKind(lookup(classFieldCTypes_, classOf(*e.as<MemberExpr>().object),
                            e.as<MemberExpr>().member));
  if (e.is<ArrowExpr>())
    return cTypeKind(lookup(classFieldCTypes_, classOf(*e.as<ArrowExpr>().object),
                            e.as<ArrowExpr>().member));
  if (e.is<IndexExpr>() && e.as<IndexExpr>().object->is<Identifier>()) {
    // double arr[N] / double *p: the element type
    std::string t = declaredCType(e.as<IndexExpr>().object->as<Identifier>().name);
    while (!t.empty() && (t.back() == '*' || t.back() == ']' || t.back() == '['))
      t.pop_back();
    return cTypeKind(t);
  }
  if (e.is<CallExpr>()) {
    auto &callee = *e.as<CallExpr>().callee;
    if (callee.is<Identifier>()) {
      const std::string &n = callee.as<Identifier>().name;
      static const std::set<std::string> floating = {
          "_double_", "_float_", "sqrt", "pow", "exp", "log", "log10", "log2", "sin", "cos",
          "tan", "asin", "acos", "atan", "atan2", "floor", "ceil", "round", "fabs", "fmod",
          "hypot", "cbrt", "trunc", "stod", "stof", "atof"};
      static const std::set<std::string> integral = {
          "_int_", "_long_", "_short_", "__c_int__", "stoi", "stol", "stoll", "atoi", "atol"};
      if (floating.count(n))
        return 'f';
      if (integral.count(n))
        return 'i';
      auto f = fnReturnCTypes_.find(n);
      if (f != fnReturnCTypes_.end())
        return cTypeKind(f->second);
      if (!classStack_.empty()) // an implicit this->method()
        return cTypeKind(lookup(classMethodCTypes_, classStack_.back(), n));
      return 0;
    }
    if (callee.is<MemberExpr>())
      return cTypeKind(lookup(classMethodCTypes_, classOf(*callee.as<MemberExpr>().object),
                              callee.as<MemberExpr>().member));
    if (callee.is<ArrowExpr>())
      return cTypeKind(lookup(classMethodCTypes_, classOf(*callee.as<ArrowExpr>().object),
                              callee.as<ArrowExpr>().member));
  }
  return 0;
}

void Compiler::beginScope() { current_->scopeDepth++; }

void Compiler::endScope(int line) {
  current_->scopeDepth--;
  while (!current_->locals.empty() &&
         current_->locals.back().depth > current_->scopeDepth) {
    if (current_->locals.back().isCaptured)
      emit(Op::CLOSE_UPVALUE, 0, line);
    else
      emit(Op::POP, 0, line);
    current_->locals.pop_back();
  }
}

int Compiler::resolveLocal(CompilerState *state, const std::string &name) {
  for (int i = static_cast<int>(state->locals.size()) - 1; i >= 0; --i)
    if (state->locals[i].name == name)
      return i;
  return -1;
}

int Compiler::addUpvalue(CompilerState *state, int index, bool isLocal) {
  for (int i = 0; i < static_cast<int>(state->upvalues.size()); ++i)
    if (state->upvalues[i].index == index &&
        state->upvalues[i].isLocal == isLocal)
      return i;
  state->upvalues.push_back({isLocal, index});
  state->chunk->upvalueCount++;
  return static_cast<int>(state->upvalues.size()) - 1;
}

int Compiler::resolveUpvalue(CompilerState *state, const std::string &name) {
  if (!state->enclosing)
    return -1;
  int local = resolveLocal(state->enclosing, name);
  if (local != -1) {
    state->enclosing->locals[local].isCaptured = true;
    return addUpvalue(state, local, true);
  }
  int upvalue = resolveUpvalue(state->enclosing, name);
  if (upvalue != -1)
    return addUpvalue(state, upvalue, false);
  return -1;
}

void Compiler::declareLocal(const std::string &name, int) {
  if (current_->scopeDepth == 0)
    return;
  current_->locals.push_back({name, current_->scopeDepth, false});
}

void Compiler::emitLoad(const std::string &name, int line) {
  // "this" is an alias for "self" (slot 0 in all methods)
  const std::string &resolved = (name == "this") ? std::string("self") : name;
  int local = resolveLocal(current_, resolved);
  if (local != -1) {
    emit(Op::LOAD_LOCAL, local, line);
    return;
  }
  int uv = resolveUpvalue(current_, resolved);
  if (uv != -1) {
    emit(Op::LOAD_UPVALUE, uv, line);
    return;
  }
  emit(Op::LOAD_GLOBAL, addStr(resolved), line);
}

void Compiler::emitStore(const std::string &name, int line) {
  // "this" is an alias for "self" (slot 0 in all methods)
  const std::string &resolved = (name == "this") ? std::string("self") : name;
  int local = resolveLocal(current_, resolved);
  if (local != -1) {
    emit(Op::STORE_LOCAL, local, line);
    return;
  }
  int uv = resolveUpvalue(current_, resolved);
  if (uv != -1) {
    emit(Op::STORE_UPVALUE, uv, line);
    return;
  }
  emit(Op::STORE_GLOBAL, addStr(resolved), line);
}

void Compiler::beginLoop(int startIp) {
  loops_.push_back({});
  loops_.back().loopStart = startIp;
  loops_.back().scopeDepth = current_->scopeDepth;
  loops_.back().tryDepth = current_->tryDepth;
}

// Locals live on the value stack, so leaving a loop body early has to discard
// everything the body pushed — otherwise those slots leak for the rest of the
// enclosing function. For a for-in loop that leak is silently destructive: the
// hidden `__iter__` sits directly under the loop variable, so a stale slot left
// by `break` makes the *next* for-in loop's FOR_ITER read the wrong iterator.
void Compiler::emitLoopExitPops(int line) {
  if (loops_.empty())
    return;
  int depth = loops_.back().scopeDepth;
  for (int i = static_cast<int>(current_->locals.size()) - 1;
       i >= 0 && current_->locals[i].depth > depth; --i)
    emit(current_->locals[i].isCaptured ? Op::CLOSE_UPVALUE : Op::POP, 0, line);
}

void Compiler::emitBreak(int line) {
  if (loops_.empty()) {
    emit(Op::RETURN_NIL, 0, line);
    return;
  }
  emitLoopExitPops(line);
  // leaving try bodies inside the loop: drop their handlers
  for (int i = loops_.back().tryDepth; i < current_->tryDepth; ++i)
    emit(Op::POP_HANDLER, 0, line);
  loops_.back().breakJumps.push_back(emitJump(Op::JUMP, line));
}

void Compiler::emitContinue(int line) {
  if (loops_.empty()) {
    emit(Op::RETURN_NIL, 0, line);
    return;
  }
  emitLoopExitPops(line);
  for (int i = loops_.back().tryDepth; i < current_->tryDepth; ++i)
    emit(Op::POP_HANDLER, 0, line);
  loops_.back().continueJumps.push_back(emitJump(Op::JUMP, line));
}

void Compiler::endLoop() {
  size_t after = chunk().code.size();
  for (size_t idx : loops_.back().breakJumps)
    chunk().patch(idx,
                  static_cast<int32_t>(after) - static_cast<int32_t>(idx) - 1);
  loops_.pop_back();
}

// ─── Node dispatch
// ────────────────────────────────────────────────────────────

void Compiler::compileNode(ASTNode &node) {
  int ln = node.line;
  std::visit(
      [&](auto &n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, BlockStmt>)
          compileBlock(n);
        else if constexpr (std::is_same_v<T, VarDecl>)
          compileVarDecl(n, ln);
        else if constexpr (std::is_same_v<T, FunctionDecl>)
          compileFunctionDecl(n, ln);
        else if constexpr (std::is_same_v<T, ClassDecl>)
          compileClassDecl(n, ln);
        else if constexpr (std::is_same_v<T, IfStmt>)
          compileIf(n, ln);
        else if constexpr (std::is_same_v<T, WhileStmt>)
          compileWhile(n, ln);
        else if constexpr (std::is_same_v<T, ForStmt>)
          compileFor(n, ln);
        else if constexpr (std::is_same_v<T, ReturnStmt>)
          compileReturn(n, ln);
        else if constexpr (std::is_same_v<T, PrintStmt>)
          compilePrint(n, ln);
        else if constexpr (std::is_same_v<T, InputStmt>)
          compileInput(n, ln);
        else if constexpr (std::is_same_v<T, TryStmt>)
          compileTry(n, ln);
        else if constexpr (std::is_same_v<T, RaiseStmt>)
          compileRaise(n, ln);
        else if constexpr (std::is_same_v<T, BreakStmt>)
          emitBreak(ln);
        else if constexpr (std::is_same_v<T, ContinueStmt>)
          emitContinue(ln);
        else if constexpr (std::is_same_v<T, ImportStmt>)
          compileImport(n, ln);
        else if constexpr (std::is_same_v<T, GlobalStmt>) {
          // scoping only — read by compileFunction's local analysis
        }
        else if constexpr (std::is_same_v<T, ExprStmt>) {
          compileExpr(*n.expr);
          if (!n.expr->template is<ReturnStmt>())
            emit(Op::POP, 0, ln);
        } else {
          compileExpr(node);
          emit(Op::POP, 0, ln);
        }
      },
      node.node);
}

void Compiler::compileBlock(BlockStmt &b) {
  if (current_->scopeDepth > 0) {
    for (auto &stmt : b.statements) {
      if (!stmt->is<FunctionDecl>())
        continue;
      auto &fn = stmt->as<FunctionDecl>();
      if (resolveLocal(current_, fn.name) != -1)
        continue;
      emit(Op::LOAD_NIL, 0, stmt->line);
      declareLocal(fn.name, stmt->line);
      emit(Op::DEFINE_LOCAL, static_cast<int>(current_->locals.size()) - 1,
           stmt->line);
    }
  }

  for (auto &stmt : b.statements)
    compileNode(*stmt);
}

void Compiler::compileExpr(ASTNode &node) {
  int ln = node.line;
  std::visit(
      [&](auto &n) {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, NumberLiteral>)
          emit(Op::LOAD_CONST, addConst(QuantumValue(n.value)), ln);
        else if constexpr (std::is_same_v<T, StringLiteral>)
          emit(Op::LOAD_CONST, addConst(QuantumValue(n.value)), ln);
        else if constexpr (std::is_same_v<T, BoolLiteral>)
          emit(n.value ? Op::LOAD_TRUE : Op::LOAD_FALSE, 0, ln);
        else if constexpr (std::is_same_v<T, NilLiteral>)
          emit(Op::LOAD_NIL, 0, ln);
        else if constexpr (std::is_same_v<T, Identifier>)
          compileIdentifier(n, ln);
        else if constexpr (std::is_same_v<T, BinaryExpr>)
          compileBinary(n, ln);
        else if constexpr (std::is_same_v<T, UnaryExpr>)
          compileUnary(n, ln);
        else if constexpr (std::is_same_v<T, AssignExpr>)
          compileAssign(n, ln);
        else if constexpr (std::is_same_v<T, CallExpr>)
          compileCall(n, ln);
        else if constexpr (std::is_same_v<T, IndexExpr>)
          compileIndex(n, ln);
        else if constexpr (std::is_same_v<T, SliceExpr>)
          compileSlice(n, ln);
        else if constexpr (std::is_same_v<T, MemberExpr>)
          compileMember(n, ln);
        else if constexpr (std::is_same_v<T, ArrayLiteral>)
          compileArray(n, ln);
        else if constexpr (std::is_same_v<T, DictLiteral>)
          compileDict(n, ln);
        else if constexpr (std::is_same_v<T, TupleLiteral>)
          compileTuple(n, ln);
        else if constexpr (std::is_same_v<T, LambdaExpr>)
          compileLambda(n, ln);
        else if constexpr (std::is_same_v<T, TernaryExpr>)
          compileTernary(n, ln);
        else if constexpr (std::is_same_v<T, ListComp>)
          compileListComp(n, ln);
        else if constexpr (std::is_same_v<T, SuperExpr>)
          compileSuper(n, ln);
        else if constexpr (std::is_same_v<T, NewExpr>)
          compileNew(n, ln);
        else if constexpr (std::is_same_v<T, AddressOfExpr>)
          compileAddressOf(n, ln);
        else if constexpr (std::is_same_v<T, DerefExpr>)
          compileDeref(n, ln);
        else if constexpr (std::is_same_v<T, ArrowExpr>)
          compileArrow(n, ln);
        else if constexpr (std::is_same_v<T, ReturnStmt>)
          compileReturn(n, ln);
        else if constexpr (std::is_same_v<T, RaiseStmt>) {
          // A raise expression never produces its value; the nil only
          // keeps the enclosing expression's stack shape balanced.
          compileRaise(n, ln);
          emit(Op::LOAD_NIL, 0, ln);
        } else
          throw std::runtime_error("Compiler: unhandled expression node");
      },
      node.node);
}

// ─── Statements ──────────────────────────────────────────────────────────────
