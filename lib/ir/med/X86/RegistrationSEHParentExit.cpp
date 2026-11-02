//===- RegistrationSEHParentExit.cpp - Checked finally parent branches ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks immutable SEH invocation, restore and parent branch evidence.

#include "RegistrationSEHParentFallthrough.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/X86/X86RegistrationSEHParentExit.h"
#include "neverd/ir/med/X86RegistrationSEH4PhysicalFrame.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLExtras.h"

#include <set>
#include <tuple>

namespace neverd {

std::optional<std::map<int, RegistrationSEHParentExit>>
getRegistrationSEHParentExits(const MedFunc &Function) {
  if (!Function.RegistrationStates || Function.SkippedSSA)
    return std::nullopt;
  const auto *Physical = getX86RegistrationSEHPhysicalStates(Function);
  if (!Physical)
    return std::nullopt;
  const auto &States = *Physical;
  if (!States.SEHInvocationExitsComplete)
    return std::nullopt;
  std::map<int, RegistrationSEHParentExit> Result;
  if (States.SEHInvocationExits.size() > limits::kMaxRegistrationEHStateWork)
    return std::nullopt;
  if (llvm::none_of(States.SEHInvocationExits,
                    [](const auto &Exit) { return Exit.isParentExit(); }))
    return Result;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !Function.ExceptionMetadata)
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  if (!EH.Registration || EH.Cxx ||
      EH.ParseStatus != ExceptionParseStatus::Complete ||
      (EH.Encoding != ExceptionEncoding::X86ScopeTableEH3 &&
       EH.Encoding != ExceptionEncoding::X86ScopeTableEH4) ||
      !EH.Registration->RegistrationOffset || EH.Registration->RealignedFrame)
    return std::nullopt;

  using Identity = std::pair<va_t, int>;
  std::map<int, const MedBlock *> Blocks;
  std::map<va_t, const MedBlock *> Entries;
  std::map<int, const RegistrationBlockState *> Reaching;
  std::map<Identity, std::pair<const MedBlock *, const MedOp *>> Operations;
  std::map<va_t, const MedOp *> Calls;
  std::map<Identity, std::optional<int32_t>> Frames;
  std::set<va_t> FinallyEntries, OtherEntries{Function.Entry};
  size_t Work = States.SEHInvocationExits.size();
  const auto Charge = [&] {
    return ++Work <= limits::kMaxRegistrationEHStateWork;
  };
  for (const auto &Scope : EH.Registration->Scopes) {
    if (!Charge())
      return std::nullopt;
    if (Scope.IsFinally)
      FinallyEntries.insert(Scope.HandlerVA);
    else {
      OtherEntries.insert(Scope.FilterVA);
      OtherEntries.insert(Scope.HandlerVA);
    }
  }
  for (const auto &State : States.Blocks)
    if (!Charge() || !Reaching.emplace(State.BlockId, &State).second)
      return std::nullopt;
  for (const auto &Frame : States.FrameValues)
    if (!Charge() || !Frames
                          .emplace(Identity{Frame.Address, Frame.OpSeq},
                                   Frame.EstablishedFrameOffset)
                          .second)
      return std::nullopt;
  for (const auto &Block : Function.Blocks) {
    const auto State = Reaching.find(Block.Id);
    if (!Charge() || !Blocks.emplace(Block.Id, &Block).second ||
        !Entries.emplace(Block.StartAddr, &Block).second ||
        State == Reaching.end() || State->second->Unknown ||
        State->second->Range.Begin != Block.StartAddr ||
        State->second->Range.End != Block.EndAddr)
      return std::nullopt;
    for (const auto &Op : Block.Ops) {
      if (!Charge())
        return std::nullopt;
      if (Op.OriginSeq >= 0 && !Operations
                                    .emplace(Identity{Op.Addr, Op.OriginSeq},
                                             std::make_pair(&Block, &Op))
                                    .second)
        return std::nullopt;
      if (Op.Opcode == NdOp::CALL && !Calls.emplace(Op.Addr, &Op).second)
        return std::nullopt;
    }
  }
  if (Blocks.size() != Reaching.size())
    return std::nullopt;

  std::set<std::tuple<va_t, va_t, va_t, int>> Seen;
  const auto Ordinary = [](const MedOp &Op) {
    return !Op.Dead && Op.OriginSeq >= 0 && !Op.ExactArguments &&
           !Op.SourceCallHint &&
           Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
           Op.MemoryOrdering == NdMemoryOrdering::None;
  };
  for (const auto &Exit : States.SEHInvocationExits) {
    if (!Exit.isParentExit())
      continue;
    const auto Branch = Operations.find({Exit.Address, Exit.OpSeq});
    const auto Restore =
        Operations.find({Exit.StackRestoreVA, Exit.StackRestoreSeq});
    const auto Target = Entries.find(Exit.TargetVA);
    const auto Frame = Frames.find({Exit.StackRestoreVA, Exit.StackRestoreSeq});
    if (!Charge() || !FinallyEntries.count(Exit.EntryVA) ||
        OtherEntries.count(Exit.EntryVA) || Branch == Operations.end() ||
        Restore == Operations.end() || Target == Entries.end() ||
        Frame == Frames.end() || Frame->second != Exit.StackOffset ||
        int64_t(Exit.StackOffset) >=
            int64_t(*EH.Registration->RegistrationOffset) - 8 ||
        FinallyEntries.count(Exit.TargetVA) ||
        OtherEntries.count(Exit.TargetVA) ||
        !EH.CodeRange.contains(Exit.TargetVA) ||
        !Seen.emplace(Exit.EntryVA, Exit.CallVA, Exit.Address, Exit.OpSeq)
             .second)
      return std::nullopt;
    const auto &[Block, Op] = Branch->second;
    const auto &[RestoreBlock, RestoreOp] = Restore->second;
    const auto *SourceState = Reaching.at(Block->Id);
    const auto *TargetState = Reaching.at(Target->second->Id);
    const auto *RestoreState = Reaching.at(RestoreBlock->Id);
    const bool Jump =
        Exit.Exit == RegistrationSEHInvocationExit::Kind::ParentJump;
    const bool TransferShape =
        Jump
            ? Ordinary(*Op) && Op->Opcode == NdOp::BRANCH && !Op->Output.Size &&
                  Op->NumInputs == 1 && Op->Inputs[0].isConst() &&
                  (Op->Inputs[0].Size == 4 || Op->Inputs[0].Size == 8) &&
                  Op->Inputs[0].ConstVal == Exit.TargetVA
            : isRegistrationSEHParentFallthrough(*Block, *Op, *Target->second,
                                                 States, Exit, Work);
    if (!TransferShape || &Block->Ops.back() != Op ||
        Block->EndAddr != Exit.EndAddress || Op->Addr < Block->StartAddr ||
        Op->Addr >= Block->EndAddr ||
        Block->Succs != std::vector<int>{Target->second->Id} ||
        !SourceState->Reached || !SourceState->CallbackOnly ||
        !TargetState->Reached || TargetState->CallbackOnly ||
        !RestoreState->Reached || !RestoreState->CallbackOnly ||
        !Ordinary(*RestoreOp) || RestoreOp->NumInputs != 1 ||
        (RestoreOp->Opcode != NdOp::LOAD && RestoreOp->Opcode != NdOp::COPY) ||
        RestoreOp->Output.Kind != MedVar::Reg ||
        RestoreOp->Output.TheArch != Arch::X86 ||
        RestoreOp->Output.RegOff != x86reg::RSP ||
        RestoreOp->Output.Size != 4 || RestoreOp->Inputs[0].Size != 4 ||
        RestoreOp->Inputs[0].isConst() ||
        RestoreOp->Addr < RestoreBlock->StartAddr ||
        RestoreOp->Addr >= RestoreBlock->EndAddr)
      return std::nullopt;
    if (Exit.CallVA) {
      const auto Call = Calls.find(Exit.CallVA);
      if (Call == Calls.end() || !Ordinary(*Call->second) ||
          Call->second->NumInputs != 1 || !Call->second->Inputs[0].isConst() ||
          Call->second->Inputs[0].Size != 4 ||
          Call->second->Inputs[0].ConstVal != Exit.EntryVA)
        return std::nullopt;
    }
    auto [It, New] = Result.try_emplace(
        Block->Id, RegistrationSEHParentExit{Exit.EntryVA,
                                             Target->second->Id,
                                             RestoreBlock->Id,
                                             Exit.StackOffset,
                                             Op,
                                             RestoreOp,
                                             {}});
    auto &Bound = It->second;
    if (!New && (Bound.EntryVA != Exit.EntryVA || Bound.Operation != Op ||
                 Bound.Restore != RestoreOp ||
                 Bound.TargetBlock != Target->second->Id ||
                 Bound.StackOffset != Exit.StackOffset))
      return std::nullopt;
    Bound.Invocations.push_back(&Exit);
  }

  // Stop at authenticated parent exits. A source jump must belong to its own
  // finally, and its exact SavedESP restore must dominate it inside that body.
  // A second traversal with the restore removed proves dominance even in loops.
  for (const auto &[Id, Exit] : Result) {
    const auto Entry = Entries.find(Exit.EntryVA);
    if (Entry == Entries.end())
      return std::nullopt;
    for (bool SkipRestore : {false, true}) {
      std::set<int> Reached;
      std::vector<int> Pending{Entry->second->Id};
      while (!Pending.empty()) {
        const int Current = Pending.back();
        Pending.pop_back();
        if (!Charge() || !Blocks.count(Current))
          return std::nullopt;
        if ((SkipRestore && Current == Exit.RestoreBlock) ||
            !Reached.insert(Current).second)
          continue;
        const auto *Block = Blocks.at(Current);
        if (!Reaching.at(Current)->CallbackOnly ||
            (Block->StartAddr != Exit.EntryVA &&
             (FinallyEntries.count(Block->StartAddr) ||
              OtherEntries.count(Block->StartAddr))))
          return std::nullopt;
        if (Result.count(Current))
          continue;
        for (int Next : Block->Succs) {
          if (!Charge())
            return std::nullopt;
          Pending.push_back(Next);
        }
      }
      if (Reached.count(Id) == SkipRestore)
        return std::nullopt;
    }
    if (Exit.RestoreBlock == Id) {
      const auto &Ops = Blocks.at(Id)->Ops;
      if (Exit.Restore >= Exit.Operation || Exit.Restore < Ops.data())
        return std::nullopt;
    }
  }
  return Result;
}

} // namespace neverd
