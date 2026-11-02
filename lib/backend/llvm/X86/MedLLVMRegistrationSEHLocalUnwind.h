//===- MedLLVMRegistrationSEHLocalUnwind.h - Local unwind calls -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Project source full unwinds into an outlined callback's native parent.

#ifndef NEVERD_MEDLLVMREGISTRATIONSEHLOCALUNWIND_H
#define NEVERD_MEDLLVMREGISTRATIONSEHLOCALUNWIND_H

#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/ir/med/X86/X86RegistrationSEHLocalUnwind.h"

#include <map>
#include <optional>

namespace llvm {
class LoadInst;
} // namespace llvm

namespace neverd::x86_registration {

struct SEHParentExitPlan;

struct SEHLocalUnwindPlan {
  struct Call {
    X86RegistrationSEHLocalUnwind Source;
    size_t Callback;
  };
  std::map<llvm::CallInst *, Call> Calls;
  std::map<va_t, llvm::Function *> Runtimes;
  struct FrameRead {
    size_t Callback;
    int32_t Offset;
  };
  std::map<llvm::LoadInst *, FrameRead> FrameReads;
};

bool bindSEHLocalUnwindFrameReads(
    SEHLocalUnwindPlan &Plan, const MedFunc &Source, uint64_t EntrySP,
    const SEHParentExitPlan &ParentExits,
    llvm::ArrayRef<X86RegistrationCallbackRequest> Requests,
    const std::map<std::pair<va_t, int>, llvm::Instruction *> &Memory);

std::optional<SEHLocalUnwindPlan> prepareSEHLocalUnwinds(
    const MedFunc &Source, llvm::Function &Parent, llvm::AllocaInst &Frame,
    uint64_t EntrySP, const SEHParentExitPlan &ParentExits,
    const std::map<std::pair<va_t, bool>, size_t> &CallbackIndices,
    llvm::ArrayRef<X86RegistrationCallbackRequest> Requests,
    const std::map<const llvm::CallInst *, va_t> &Calls,
    const std::map<std::pair<va_t, int>, llvm::Instruction *> &Memory);

/// Commit only after outlining has recovered the private parent chain and
/// synthetic frame. Original callback blocks are subsequently discarded.
void emitSEHLocalUnwinds(
    const SEHLocalUnwindPlan &Plan, llvm::Function &Parent,
    llvm::AllocaInst &Frame, uint64_t EntrySP, llvm::AllocaInst &ParentChain,
    va_t Owner, llvm::ArrayRef<llvm::Function *> Callbacks,
    llvm::ArrayRef<X86RegistrationCallbackInstructionMap> Instructions);

} // namespace neverd::x86_registration

#endif // NEVERD_MEDLLVMREGISTRATIONSEHLOCALUNWIND_H
