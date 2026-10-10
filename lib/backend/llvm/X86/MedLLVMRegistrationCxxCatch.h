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
void emitRegistrationCxxCatches(
    const ExceptionFunction &EH,
    std::map<X86RegistrationCatchIdentity, RegistrationCxxCatchPlan> &Plans,
    llvm::ArrayRef<llvm::BasicBlock *> Dispatches,
    llvm::ArrayRef<llvm::BasicBlock *> OuterUnwinds,
    llvm::ArrayRef<std::optional<X86RegistrationCatchIdentity>> Parents,
    llvm::AllocaInst &Frame, llvm::Function &Parent);
} // namespace neverd
#endif
