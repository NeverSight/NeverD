//===- X86AmxStackLeaf.h - AMX stack leaf ownership ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate the isolated frame used by the older-SDK AMX assembly route.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86AMXSTACKLEAF_H
#define NEVERD_BACKEND_LLVM_X86_X86AMXSTACKLEAF_H

#include "neverd/backend/llvm/X86/X86AmxAsm.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

namespace neverd {

inline bool x86AmxIsStackLeaf(const llvm::CallInst &Call) {
  const auto *F = Call.getFunction();
  return F && F->hasLocalLinkage() &&
         F->hasFnAttribute(X86AmxStackLeafAttribute) &&
         F->hasFnAttribute(llvm::Attribute::NoInline) &&
         F->getFnAttribute("frame-pointer").getValueAsString() == "none" &&
         !F->hasFnAttribute("stackrealign") &&
         !F->hasFnAttribute(llvm::Attribute::StackAlignment) &&
         F->getCallingConv() == llvm::CallingConv::PreserveAll &&
         F->getReturnType()->isVoidTy() && F->arg_size() <= 2 &&
         llvm::all_of(F->args(),
                      [](const llvm::Argument &A) {
                        return A.getType()->isIntegerTy(64);
                      }) &&
         F->size() == 1 && F->front().size() == 2 &&
         &F->front().front() == &Call &&
         llvm::isa<llvm::ReturnInst>(F->front().back()) &&
         Call.arg_size() == 2 &&
         Call.getArgOperand(0)->getType()->isIntegerTy(64) &&
         Call.getArgOperand(1)->getType()->isIntegerTy(64);
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86AMXSTACKLEAF_H
