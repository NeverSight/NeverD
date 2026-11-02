//===- COFFRegistrationSEHParentExitSource.cpp - Parent target replay ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replays source identities and exact parent scope destinations.

#include "COFFRegistrationSEHParentExit.h"

#include "neverd/ir/med/X86/X86RegistrationSEHParentExit.h"

namespace neverd::coff_registration {

llvm::Error
bindSEHParentExitSources(RegistrationSEHParentExitContract &Contract,
                         const MedFunc &Source) {
  const auto Exits = getRegistrationSEHParentExits(Source);
  if (!Exits ||
      (!Exits->empty() && !Source.RegistrationStates->SEHParentChainComplete) ||
      !Contract.Targets.empty() || !Contract.SourceChainRemovals.empty() ||
      Exits->size() != Contract.Transfers.size())
    return rejectIR("parent transfers have no complete source exit set");
  for (const auto &[Id, Exit] : *Exits) {
    const auto Found = Contract.Transfers.find(Id);
    if (Found == Contract.Transfers.end() ||
        Found->second.Callback != Exit.EntryVA ||
        Found->second.Address != Exit.Operation->Addr)
      return rejectIR("parent transfer changed its source instruction");
    Contract.Targets.emplace(Id, Exit.TargetBlock);
    if (Exit.Operation->Opcode == NdOp::STORE &&
        !Contract.SourceChainRemovals
             .emplace(Exit.Operation->Addr, Exit.Operation->OriginSeq)
             .second)
      return rejectIR("parent transfer duplicates a source chain removal");
  }
  return llvm::Error::success();
}

llvm::Error validateSEHParentExitTargets(
    const RegistrationSEHParentExitContract &Contract,
    const std::map<int, const llvm::BasicBlock *> &Entries) {
  if (Contract.Transfers.empty())
    return llvm::Error::success();
  if (!Contract.Dispatch ||
      Contract.Targets.size() != Contract.Transfers.size())
    return rejectIR("parent continuation has no checked source targets");
  for (const auto &Case : Contract.Dispatch->cases()) {
    const auto Target =
        Contract.Targets.find(Case.getCaseValue()->getZExtValue());
    if (Target == Contract.Targets.end() || !Entries.count(Target->second) ||
        Case.getCaseSuccessor() != Entries.at(Target->second))
      return rejectIR("parent transfer bypasses its source scope boundary");
  }
  return llvm::Error::success();
}

} // namespace neverd::coff_registration
