//===- LLVMWin64CallStackInput.h - CALL input operands ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Source-only observations take the original CALL and stack offset as SSA
/// operands. Optimizers that merge two calls must preserve their coordinates.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_LLVMWIN64CALLSTACKINPUT_H
#define NEVERD_BACKEND_LLVM_X86_LLVMWIN64CALLSTACKINPUT_H

#include "neverd/backend/llvm/X86/LLVMWin64CallStackProvider.h"
#include "neverd/ir/med/X86/Win64CallStackInput.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ModRef.h"

#include <string>

namespace neverd::llvm_win64_call_stack {
inline llvm::Value *create(llvm::IRBuilder<> &B, Win64CallStackInput Input) {
  auto &M = *B.GetInsertBlock()->getModule();
  std::string Symbol = Name.str();
  llvm::Function *Provider = nullptr;
  while (auto *Existing = M.getNamedValue(Symbol)) {
    Provider = llvm::dyn_cast<llvm::Function>(Existing);
    if (Provider && isProvider(*Provider))
      break;
    Provider = nullptr;
    Symbol += '_';
  }
  if (!Provider) {
    auto *Type = llvm::FunctionType::get(
        B.getInt64Ty(), {B.getInt64Ty(), B.getInt32Ty()}, false);
    Provider = llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                                      Symbol, M);
    Provider->addFnAttr(Attribute);
    Provider->addFnAttr(llvm::Attribute::NoUnwind);
    Provider->addFnAttr(llvm::Attribute::WillReturn);
    // The producer may supply a different word on each dynamic occurrence.
    Provider->setMemoryEffects(llvm::MemoryEffects::inaccessibleMemOnly());
  }
  return B.CreateCall(Provider,
                      {B.getInt64(Input.Site), B.getInt32(Input.Offset)},
                      "original_stack_word");
}
} // namespace neverd::llvm_win64_call_stack

#endif // NEVERD_BACKEND_LLVM_X86_LLVMWIN64CALLSTACKINPUT_H
