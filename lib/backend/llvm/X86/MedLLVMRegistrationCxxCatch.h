//===- MedLLVMRegistrationCxxCatch.h - Ordered PE32 catches ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_MEDLLVMREGISTRATIONCXXCATCH_H
#define NEVERD_MEDLLVMREGISTRATIONCXXCATCH_H

#include "MedLLVMRegistrationCxxStack.h"

#include "neverd/backend/llvm/X86RegistrationCatch.h"

#include <set>

namespace llvm {
class CatchPadInst;
class Function;
} // namespace llvm
namespace neverd {
struct RegistrationCxxCatchPlan {
  llvm::BasicBlock *Handler = nullptr;
  llvm::CatchPadInst *Pad = nullptr;
  X86RegistrationCatchPlan Object;
  std::optional<RegistrationCxxStackPlan> Stack;
  std::set<llvm::BasicBlock *> Blocks;
};
llvm::BasicBlock *emitRegistrationCxxCatches(
    const ExceptionFunction &EH,
    llvm::MutableArrayRef<RegistrationCxxCatchPlan> Plans,
    llvm::AllocaInst &Frame, llvm::Function &Parent);
} // namespace neverd
#endif
