//===- COFFRegistrationSEHStateDispatch.cpp - State selection -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks state selection without trusting emitter metadata or block order.

#include "COFFRegistrationSEHStateDispatch.h"

#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHJump.h"
#include "neverd/ir/med/X86/X86RegistrationSEHStateStore.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/IntrinsicsX86.h"

namespace neverd::coff_registration {

llvm::Expected<SEHCallbackStateSequence>
bindSEHCallbackStateSequence(const llvm::StoreInst &Store,
                             const X86RegistrationSEHStateStoreBinding &Bound,
                             const MedFunc &Source,
                             const llvm::Function &Parent) {
#ifndef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
  return rejectIR("LLVM lacks physical callback state-write receipts");
#else
  const auto *Function = Store.getFunction();
  const auto *Frame = getX86RegistrationSEHJumpParentFrame(*Function, Parent);
  if (Function == &Parent || !Frame || Bound.TargetLevels.empty() ||
      !Bound.FirstWriteID)
    return rejectIR("callback state write has no checked owner or outcomes");
  auto Bind = [&](const llvm::Instruction *I,
                  size_t Index) -> llvm::Expected<const llvm::CallInst *> {
    const auto *Write = llvm::dyn_cast_or_null<llvm::CallInst>(I);
    const int32_t Level = Bound.TargetLevels[Index];
    if (!Write ||
        Write->getIntrinsicID() != llvm::Intrinsic::x86_seh_setstate ||
        Write->arg_size() != 8 || Write->getNumOperandBundles() ||
        Write->getCallingConv() != llvm::CallingConv::C ||
        Write->isTailCall() || !Write->use_empty() ||
        !Write->getAttributes().isEmpty() || Write->getFunction() != Function ||
        Write->getArgOperand(0) != &Parent ||
        Write->getArgOperand(1) != Frame ||
        integer(Write->getArgOperand(2), 32) != uint32_t(Level) ||
        integer(Write->getArgOperand(7), 64) != Bound.FirstWriteID + Index)
      return rejectIR("callback state write changed its source or frame ABI");
    std::array<uint64_t, 4> Digest{};
    if (Level >= 0) {
      const auto Token = windows_eh_semantics::getSEHScopeSemanticToken(
          *Source.ExceptionMetadata, Arch::X86, Level);
      if (!Token)
        return rejectIR("callback state write has no source scope token");
      Digest = Token->Digest;
    } else if (Level != *Source.ExceptionMetadata->Registration->SeededTryLevel)
      return rejectIR("callback state write changed its inactive sentinel");
    for (unsigned J = 0; J != Digest.size(); ++J)
      if (integer(Write->getArgOperand(3 + J), 64) != Digest[J])
        return rejectIR("callback state write changed its scope digest");
    return Write;
  };
  SEHCallbackStateSequence Result;
  if (Bound.TargetLevels.size() == 1) {
    auto Write = Bind(next(Store), 0);
    if (!Write)
      return Write.takeError();
    Result.Writes.push_back(*Write);
    Result.Resume = next(**Write);
    return Result;
  }
  const auto *Read = llvm::dyn_cast_or_null<llvm::LoadInst>(next(Store));
  const auto *Dispatch =
      Read ? llvm::dyn_cast_or_null<llvm::SwitchInst>(next(*Read)) : nullptr;
  if (!Read || !Read->getType()->isIntegerTy(32) || Read->isAtomic() ||
      !Read->isVolatile() || Read->getAlign() != llvm::Align(1) ||
      Read->getPointerOperand() != Store.getPointerOperand() ||
      !Read->hasOneUse() || !Dispatch || Dispatch->getCondition() != Read ||
      Dispatch->getNumCases() + 1 != Bound.TargetLevels.size())
    return rejectIR(
        "callback state selection changed its complete source read");
  std::vector<const llvm::BasicBlock *> Cases;
  for (const auto &Case : Dispatch->cases()) {
    if (integer(Case.getCaseValue(), 32) !=
        uint32_t(Bound.TargetLevels[Cases.size()]))
      return rejectIR("callback state selection changed its result domain");
    Cases.push_back(Case.getCaseSuccessor());
  }
  Cases.push_back(Dispatch->getDefaultDest());
  const llvm::BasicBlock *Resume = nullptr;
  std::set<const llvm::BasicBlock *> Seen;
  for (size_t I = 0; I != Cases.size(); ++I) {
    const auto *Case = Cases[I];
    if (Case->getParent() != Function || Case->hasAddressTaken() ||
        !Seen.insert(Case).second ||
        Case->getSinglePredecessor() != Store.getParent() ||
        !Case->phis().empty() || Case->getFirstInsertionPt() == Case->end())
      return rejectIR("callback state selection changed a case entry");
    auto Write = Bind(&*Case->getFirstInsertionPt(), I);
    if (!Write)
      return Write.takeError();
    const auto *Branch =
        llvm::dyn_cast_or_null<llvm::UncondBrInst>(next(**Write));
    if (!Branch)
      return rejectIR("callback state selection has an extra case operation");
    const auto *Destination = Branch->getSuccessor(0);
    if (Resume && Resume != Destination)
      return rejectIR("callback state selection changed its continuation");
    Resume = Destination;
    Result.Writes.push_back(*Write);
  }
  if (!Resume || Resume->getParent() != Function || Resume->hasAddressTaken() ||
      !Resume->phis().empty() || Resume == Store.getParent() ||
      Seen.count(Resume) || llvm::pred_size(Resume) != Seen.size() ||
      Resume->getFirstInsertionPt() == Resume->end())
    return rejectIR("callback state selection has no closed continuation");
  for (const auto *Predecessor : llvm::predecessors(Resume))
    if (!Seen.count(Predecessor))
      return rejectIR("callback state selection bypasses a physical update");
  Result.Dispatch = Dispatch;
  Result.Resume = &*Resume->getFirstInsertionPt();
  return Result;
#endif
}

} // namespace neverd::coff_registration
