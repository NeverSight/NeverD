//===- MedLLVMRegistrationSEHParentExit.h - Parent continuations -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declares transactional native parent context projection.

#ifndef NEVERD_MEDLLVMREGISTRATIONSEHPARENTEXIT_H
#define NEVERD_MEDLLVMREGISTRATIONSEHPARENTEXIT_H

#include "neverd/ir/med/X86/X86RegistrationSEHParentExit.h"

#include <map>
#include <optional>
#include <vector>

namespace llvm {
class AllocaInst;
class BasicBlock;
class CallInst;
class Function;
class Instruction;
class StoreInst;
class UncondBrInst;
} // namespace llvm

namespace neverd::x86_registration {

struct SEHParentExitPlan {
  std::map<int, RegistrationSEHParentExit> Sources;
  std::map<int, llvm::UncondBrInst *> Branches;
  std::map<std::pair<va_t, int>, llvm::StoreInst *> ChainRemovals;
};

std::optional<SEHParentExitPlan> prepareSEHParentExits(
    const MedFunc &Source,
    const std::map<int, llvm::BasicBlock *> &OriginalBlocks,
    const std::map<llvm::BasicBlock *, std::pair<int, int>> &CopyEdges,
    const std::map<std::pair<va_t, int>, llvm::Instruction *> *ChainIR =
        nullptr);

/// Temporarily close source callbacks with a private nonlocal continuation.
/// Outlining recovers these exact allocations. Failure restores every original
/// branch and removes the temporary context, before source metadata rollback.
class SEHParentExitProjection {
public:
  SEHParentExitProjection(llvm::Function &Parent, va_t Owner,
                          const SEHParentExitPlan &Plan);
  ~SEHParentExitProjection();
  SEHParentExitProjection(const SEHParentExitProjection &) = delete;
  SEHParentExitProjection &operator=(const SEHParentExitProjection &) = delete;

  const std::map<int, llvm::CallInst *> &calls() const { return Calls; }
  llvm::AllocaInst *chain() const { return Chain; }
  void rollback();
  void commit();
  void emitCapture(const std::map<int, llvm::BasicBlock *> &OriginalBlocks);

private:
  llvm::Function &Parent;
  va_t Owner;
  const SEHParentExitPlan &Plan;
  llvm::AllocaInst *Context = nullptr;
  llvm::AllocaInst *Chain = nullptr;
  llvm::AllocaInst *Destination = nullptr;
  llvm::Function *CreatedLongjmp = nullptr;
  std::map<int, llvm::CallInst *> Calls;
  std::map<int, llvm::BasicBlock *> OriginalTargets;
  std::map<llvm::StoreInst *, llvm::Instruction *> DetachedRemovals;
  bool Committed = false;
};

} // namespace neverd::x86_registration

#endif // NEVERD_MEDLLVMREGISTRATIONSEHPARENTEXIT_H
