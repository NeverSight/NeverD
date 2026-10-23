//===- MedLLVMRegistrationSEHStackRestore.cpp - Saved parent ESP ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind a saved-stack load whose precise parent coordinate survived replay.

#include "MedLLVMRegistrationSEHLocalUnwind.h"
#include "MedLLVMRegistrationSEHParentExit.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86RegistrationSEH4PhysicalFrame.h"

#include "llvm/IR/Instructions.h"

#include <set>
#include <tuple>

namespace neverd::x86_registration {

bool bindSEHLocalUnwindFrameReads(
    SEHLocalUnwindPlan &Plan, const MedFunc &Source, uint64_t EntrySP,
    const SEHParentExitPlan &ParentExits,
    llvm::ArrayRef<X86RegistrationCallbackRequest> Requests,
    const std::map<std::pair<va_t, int>, llvm::Instruction *> &Memory) {
  if (Source.SkippedSSA || !Source.RegistrationStates ||
      !Source.RegistrationStates->Complete || EntrySP < 4 ||
      EntrySP > UINT32_MAX)
    return false;
  const auto *States = getX86RegistrationSEHPhysicalStates(Source);
  if (!States)
    return false;
  using Key = std::tuple<MedVar::VarKind, int, int>;
  const auto Identity = [](const MedVar &Value) {
    return Key{Value.Kind, Value.Id, Value.SSAVer};
  };
  std::map<Key, const MedOp *> Definitions;
  const auto Define = [&](const MedVar &Value, const MedOp *Op) {
    if (!Value.Size || Value.isConst())
      return;
    const auto [It, Added] = Definitions.emplace(Identity(Value), Op);
    if (!Added)
      It->second = nullptr;
  };
  size_t Work = 0;
  for (const auto &Block : Source.Blocks) {
    for (const auto &Phi : Block.Phis) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      Define(Phi.Output, nullptr);
    }
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      Define(Op.Output, &Op);
      for (const auto &Output : Op.IntrinsicOutputs) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return false;
        Define(Output, nullptr);
      }
    }
  }
  for (const auto &Clobber : Source.CallClobbers) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    Define(Clobber.Value, nullptr);
  }
  for (const auto &[Call, Bound] : Plan.Calls) {
    if (!Bound.Source.Source || Bound.Callback >= Requests.size())
      return false;
    for (const auto &[Block, Exit] : ParentExits.Sources) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      if (Exit.EntryVA != Bound.Source.Source->EntryVA)
        continue;
      const MedOp *Read = Exit.Restore;
      std::set<const MedOp *> Seen;
      while (Read && Read->Opcode == NdOp::COPY) {
        const auto Definition = Definitions.find(Identity(Read->Output));
        if (++Work > limits::kMaxRegistrationEHStateWork ||
            !Seen.insert(Read).second || Read->NumInputs != 1 ||
            Read->Inputs[0].Size != 4 || Read->Output.Size != 4 || Read->Dead ||
            Definition == Definitions.end() || Definition->second != Read ||
            Read->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            Read->MemoryOrdering != NdMemoryOrdering::None)
          return false;
        const auto Found = Definitions.find(Identity(Read->Inputs[0]));
        Read = Found == Definitions.end() ? nullptr : Found->second;
      }
      if (!Read || Read->Opcode != NdOp::LOAD || Read->NumInputs != 1 ||
          Read->Output.Size != 4 || Read->OriginSeq < 0 || Read->Dead ||
          Read->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          Read->MemoryOrdering != NdMemoryOrdering::None ||
          int64_t(EntrySP) - 4 + Exit.StackOffset < 0)
        return false;
      const auto Definition = Definitions.find(Identity(Read->Output));
      if (Definition == Definitions.end() || Definition->second != Read)
        return false;
      unsigned Receipts = 0;
      for (const auto &Value : States->FrameValues) {
        if (++Work > limits::kMaxRegistrationEHStateWork)
          return false;
        if (Value.Address != Read->Addr || Value.OpSeq != Read->OriginSeq)
          continue;
        if (Value.EstablishedFrameOffset != Exit.StackOffset)
          return false;
        ++Receipts;
      }
      const auto Found = Memory.find({Read->Addr, Read->OriginSeq});
      auto *Load = Found != Memory.end()
                       ? llvm::dyn_cast<llvm::LoadInst>(Found->second)
                       : nullptr;
      // Native lowering marks checked source memory volatile when outlining
      // commits. Preflight runs before that mutation and accepts the original
      // ordinary load; atomic accesses still require a different contract.
      if (Receipts != 1 || !Load || !Load->getType()->isIntegerTy(32) ||
          Load->isAtomic() ||
          !llvm::is_contained(Requests[Bound.Callback].Blocks,
                              Load->getParent()))
        return false;
      const auto [It, Added] = Plan.FrameReads.emplace(
          Load,
          SEHLocalUnwindPlan::FrameRead{Bound.Callback, Exit.StackOffset});
      if (!Added && (It->second.Callback != Bound.Callback ||
                     It->second.Offset != Exit.StackOffset))
        return false;
    }
  }
  return true;
}

} // namespace neverd::x86_registration
