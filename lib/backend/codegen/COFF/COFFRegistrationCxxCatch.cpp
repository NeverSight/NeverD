//===- COFFRegistrationCxxCatch.cpp ---------------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationCxxIRProof.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/backend/llvm/X86RegistrationLayout.h"

#include "llvm/IR/GlobalVariable.h"
#ifdef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
#include "llvm/IR/WinEHFrame.h"
#endif

namespace neverd::coff_registration {
llvm::Error bindCxxCatches(CxxIRControlProof &Proof, const MedFunc &Source,
                           const llvm::Function &Function,
                           const X86RegistrationFrameLayout &Layout) {
#ifndef LLVM_NEVERD_X86_CXX_FUNCTION_RECEIPTS
  return rejectIR("LLVM does not provide complete PE32 C++ catch receipts");
#else
  const auto &EH = *Source.ExceptionMetadata;

  const auto Owners = projectX86RegistrationCatchBlocks(Source);
  if (!Owners)
    return rejectIR("C++ callbacks have no disjoint invocation closure");
  for (uint32_t Region = 0; Region < EH.Cxx->TryBlocks.size(); ++Region)
    for (uint32_t Clause = 0;
         Clause < EH.Cxx->TryBlocks[Region].Handlers.size(); ++Clause)
      Proof.Catches.emplace(X86RegistrationCatchIdentity{Region, Clause},
                            CxxIRCatch{});
  std::map<std::pair<va_t, va_t>, int> SourceIDs;
  for (const auto &State : Source.RegistrationStates->Blocks)
    SourceIDs.emplace(std::make_pair(State.Range.Begin, State.Range.End),
                      State.BlockId);
  size_t Work = 0;
  for (const auto &Block : Source.Blocks) {
    const auto Owner = Owners->find(Block.Id);
    if (Owner == Owners->end())
      continue;
    const auto Id = SourceIDs.find({Block.StartAddr, Block.EndAddr});
    if (Id == SourceIDs.end() || !Proof.Segments.count(Id->second) ||
        !Proof.SourceCatchOwners.emplace(Id->second, Owner->second).second)
      return rejectIR("C++ callback lost its exact source segment");
    const auto &Segment = Proof.Segments.at(Id->second);
    auto &Catch = Proof.Catches[Owner->second];
    const auto *I = Segment.Enter;
    while (I != Segment.Exit) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("C++ callback body exceeded its work budget");
      Catch.Blocks.insert(I->getParent());
      I = I->isTerminator() ? &*llvm::cast<llvm::InvokeInst>(I)
                                    ->getNormalDest()
                                    ->getFirstInsertionPt()
                            : next(*I);
    }
    Catch.Blocks.insert(Segment.Exit->getParent());
  }
  std::set<const llvm::CatchSwitchInst *> Switches;
  for (const auto &Block : Function)
    for (const auto &I : Block) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return rejectIR("C++ catch identity exceeded its work budget");
      if (const auto *Candidate = llvm::dyn_cast<llvm::CatchSwitchInst>(&I))
        Switches.insert(Candidate);
      const auto *Pad = llvm::dyn_cast<llvm::CatchPadInst>(&I);
      if (!Pad)
        continue;
      bool Matched = false;
      for (auto &[Identity, Catch] : Proof.Catches) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return rejectIR("C++ catch row matching exceeded its work budget");
        const auto Token = windows_eh_semantics::getCxxCatchSemanticToken(
            EH, Arch::X86, Identity.first, Identity.second);
        if (!Token || !exactSemanticToken(*Pad, *Token))
          continue;
        if (Catch.Pad)
          return rejectIR("C++ catch row was duplicated");
        Catch.Pad = Pad;
        Matched = true;
      }
      if (!Matched)
        return rejectIR("C++ catch lost its source row identity");
    }
  for (uint32_t Region = 0; Region < EH.Cxx->TryBlocks.size(); ++Region) {
    const auto &Try = EH.Cxx->TryBlocks[Region];
    const auto *First = Proof.Catches.at({Region, 0}).Pad;
    const auto *Switch = First ? First->getCatchSwitch() : nullptr;
    if (!Switch || !Switches.erase(Switch) ||
        Switch->getNumHandlers() != Try.Handlers.size() ||
        Switch->getParentPad() !=
            llvm::ConstantTokenNone::get(Function.getContext()))
      return rejectIR("C++ dispatch changed its handler set or parent context");
    auto Entry = Switch->handler_begin();
    for (uint32_t Index = 0; Index < Try.Handlers.size(); ++Index, ++Entry) {
      auto &Catch = Proof.Catches.at({Region, Index});
      const auto &Handler = Try.Handlers[Index];
      const auto *Pad = Catch.Pad;
      auto Plan = projectX86RegistrationCatch(EH, *Source.RegistrationStates,
                                              Layout, Region, Index);
      const auto State =
          llvm::find_if(Source.RegistrationStates->Blocks, [&](const auto &S) {
            return S.Range.Begin == Handler.HandlerVA;
          });
      if (!Plan || !Pad || Pad->arg_size() != 3 ||
          integer(Pad->getArgOperand(1), 32) != Handler.Adjectives ||
          Pad->getCatchSwitch() != Switch || *Entry != Pad->getParent() ||
          State == Source.RegistrationStates->Blocks.end() ||
          !Proof.Segments.count(State->BlockId) ||
          Proof.Segments.at(State->BlockId).Enter->getParent() !=
              Pad->getParent() ||
          !Catch.Blocks.count(Pad->getParent()))
        return rejectIR("C++ catch changed its ordered runtime entry");
      Catch.Home = Plan->Home;
      auto Object = llvm::getRewriteWinX86CxxCatchFrameObject(*Pad);
      if (!Object)
        return Object.takeError();
      const auto *Type = llvm::dyn_cast<llvm::GlobalVariable>(
          Pad->getArgOperand(0)->stripPointerCasts());
      const bool MatchingType =
          Handler.TypeDescriptorVA
              ? Type && Type->isDeclaration() && Type->hasExternalLinkage() &&
                    Type->getName() ==
                        makeNdDataSymbol(Handler.TypeDescriptorVA)
              : llvm::isa<llvm::ConstantPointerNull>(Pad->getArgOperand(0));
      const bool MatchingHome =
          Catch.Home ? Object->Frame == Proof.Frame.Slot &&
                           Object->Offset == Catch.Home->Offset &&
                           Object->Size == Catch.Home->slotSize()
                     : !Object->Frame && !Object->Offset && !Object->Size;
      if (!MatchingType || !MatchingHome)
        return rejectIR("C++ catch changed its RTTI or checked object home");
    }
  }
  if (!Switches.empty())
    return rejectIR("C++ source gained an unrelated dispatch context");
  return llvm::Error::success();
#endif
}
} // namespace neverd::coff_registration
