#include "Compiler.h"
#include "Error.h"
#include "Vm.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_map>

std::shared_ptr<Chunk> Compiler::compileFunction(
    const std::string &name, const std::vector<std::string> &params,
    const std::vector<bool> &paramIsRef,
    const std::vector<ASTNodePtr> &defaultArgs, ASTNode *body, int line) {
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

  fnState.chunk->params = params;
  fnState.chunk->paramIsRef =
      paramIsRef.empty() ? std::vector<bool>(params.size(), false) : paramIsRef;

  for (size_t i = 0; i < defaultArgs.size(); ++i) {
    if (defaultArgs[i]) {
      emit(Op::LOAD_LOCAL, static_cast<int32_t>(i), line);
      emit(Op::LOAD_NIL, 0, line);
      emit(Op::EQ, 0, line);
      size_t jumpIfFalse = emitJump(Op::JUMP_IF_FALSE, line);
      emit(Op::POP, 0, line); // pop true
      compileExpr(*defaultArgs[i]);
      emit(Op::STORE_LOCAL, static_cast<int32_t>(i), line);
      emit(Op::POP, 0, line); // pop assigned value
      size_t jumpEnd = emitJump(Op::JUMP, line);
      patchJump(jumpIfFalse);
      emit(Op::POP, 0, line); // pop false
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
