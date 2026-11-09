//===- LLVMSourceSEHFilter.h - Captured source filter calls -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The private source module's explicit SEH filter captures. This is not a
/// Windows runtime callback ABI: native registration uses its own outliner.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_LLVMSOURCESEHFILTER_H
#define NEVERD_BACKEND_LLVM_LLVMSOURCESEHFILTER_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

namespace neverd {

/// A marked source-body definition returns i32 and takes an exception pointer
/// followed by pointers to captured parent storage. A catchpad names it in
/// operand zero and supplies the captures in the remaining operands. C
/// evaluates this call in its __except expression, interpreting the result as
/// signed i32.
inline constexpr llvm::StringLiteral
    LLVMSourceSEHFilter("neverd.source.seh-filter");

/// A filter returns an i32 independently of its parent return type. Keep that
/// value on a sideeffect immediately before the parent-shaped return until
/// outlining.
inline constexpr llvm::StringLiteral
    LLVMSourceSEHFilterResult("neverd.source.seh-filter-result");

inline llvm::Value *sourceSEHFilterResult(const llvm::ReturnInst &Return) {
  const auto *Call =
      llvm::dyn_cast_or_null<llvm::CallInst>(Return.getPrevNode());
  if (!Call || Call->getIntrinsicID() != llvm::Intrinsic::sideeffect ||
      !Call->arg_empty() || Call->getNumOperandBundles() != 1)
    return nullptr;
  const auto Bundle = Call->getOperandBundle(LLVMSourceSEHFilterResult);
  if (!Bundle || Bundle->Inputs.size() != 1 ||
      !Bundle->Inputs.front()->getType()->isIntegerTy(32))
    return nullptr;
  return Bundle->Inputs.front().get();
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_LLVMSOURCESEHFILTER_H
