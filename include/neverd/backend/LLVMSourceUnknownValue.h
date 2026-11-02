//===- LLVMSourceUnknownValue.h - Unrecovered source values ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep missing machine state distinct from LLVM undef during source recovery.
/// These analysis-only calls have no runtime implementation. C projection
/// reports their observed uses; discarding an unused result needs no failure.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVMSOURCEUNKNOWNVALUE_H
#define NEVERD_BACKEND_LLVMSOURCEUNKNOWNVALUE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ModRef.h"

#include <string>

namespace neverd::llvm_source_unknown {

inline constexpr llvm::StringLiteral Attribute("neverd.source.unknown-value");

inline bool isFunction(const llvm::Function &Function) {
  return Function.isDeclaration() && Function.hasFnAttribute(Attribute) &&
         Function.arg_empty() && !Function.isVarArg() &&
         Function.getReturnType()->isIntegerTy();
}

inline bool isValue(const llvm::Value *Value) {
  const auto *Call = llvm::dyn_cast_or_null<llvm::CallInst>(Value);
  const auto *Function = Call ? Call->getCalledFunction() : nullptr;
  return Function && isFunction(*Function) && Call->arg_empty() &&
         !Call->hasOperandBundles() &&
         Call->getFunctionType() == Function->getFunctionType();
}

inline llvm::Value *create(llvm::IRBuilder<> &Builder,
                           llvm::IntegerType *Type) {
  auto &Module = *Builder.GetInsertBlock()->getModule();
  std::string Name =
      "__neverd_source_unknown_i" + std::to_string(Type->getBitWidth());
  // An input can legitimately own the same spelling. Never annotate it as an
  // unknown-value provider or reinterpret its calls.
  while (auto *Existing = Module.getNamedValue(Name)) {
    if (auto *Function = llvm::dyn_cast<llvm::Function>(Existing);
        Function && isFunction(*Function) && Function->getReturnType() == Type)
      return Builder.CreateCall(Function, {}, "unknown_state");
    Name += '_';
  }
  auto *Function =
      llvm::Function::Create(llvm::FunctionType::get(Type, false),
                             llvm::GlobalValue::ExternalLinkage, Name, Module);
  Function->addFnAttr(Attribute);
  Function->addFnAttr(llvm::Attribute::NoUnwind);
  Function->addFnAttr(llvm::Attribute::WillReturn);
  // Separate observations can differ, including consecutive calls and loop
  // iterations. readnone would allow CSE to invent equal machine values.
  // Inaccessible state keeps ordinary program-memory alias analysis intact.
  Function->setMemoryEffects(llvm::MemoryEffects::inaccessibleMemOnly());
  return Builder.CreateCall(Function, {}, "unknown_state");
}

/// One unrecovered value captured at function entry. A distinct declaration
/// per seed prevents CSE from equating unrelated registers, while memory(none)
/// lets optimization remove seeds whose storage is always overwritten before
/// use. Unlike freeze(poison), this opaque value cannot be chosen as zero.
inline llvm::Value *createEntrySeed(llvm::IRBuilder<> &Builder,
                                    llvm::IntegerType *Type) {
  auto &Module = *Builder.GetInsertBlock()->getModule();
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Type, false), llvm::GlobalValue::ExternalLinkage,
      "__neverd_source_entry_unknown_i" + std::to_string(Type->getBitWidth()),
      Module);
  Function->addFnAttr(Attribute);
  Function->addFnAttr(llvm::Attribute::NoUnwind);
  Function->addFnAttr(llvm::Attribute::WillReturn);
  Function->setMemoryEffects(llvm::MemoryEffects::none());
  return Builder.CreateCall(Function, {}, "unknown_entry_state");
}

} // namespace neverd::llvm_source_unknown

#endif // NEVERD_BACKEND_LLVMSOURCEUNKNOWNVALUE_H
