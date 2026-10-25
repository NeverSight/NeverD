//===- RegistrationEntryABI.cpp - PE32 parent return contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate parent returns independently of the CRT's callback returns.
//===----------------------------------------------------------------------===//
#include "RegistrationABIPrivate.h"
#include "RegistrationSourceCall.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/RegistrationCallEffects.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/support/BinaryEncoding.h"

#include <map>

namespace neverd::registration_abi {
namespace {
bool hasClosedThrowExits(
    const LowFunc &Function, const BinaryImage &Image,
    const std::map<int, const RegistrationBlockState *> &States, size_t &Work) {
  const auto &Proof = *Function.RegistrationStates;
  if (!Function.ExceptionMetadata->Cxx ||
      !Function.ExceptionMetadata->Cxx->TryBlocks.empty() ||
      !Proof.CxxContinuations.empty() || !Proof.CxxCatchObjectsComplete ||
      !Proof.CleanupFrameEffectsComplete ||
      !Proof.IncomingFrameAccessesComplete ||
      !Proof.IncomingFrameAccesses.empty() ||
      !Proof.RuntimeObjectAccessesComplete || !Proof.ImageReadsComplete)
    return false;
  std::map<int, const LowBlock *> Blocks;
  std::map<int, size_t> Remaining;
  std::map<int, std::vector<int>> Predecessors;
  std::vector<int> Pending;
  std::set<int> Closed;
  const LowBlock *Entry = nullptr;
  for (const auto &Block : Function.Blocks) {
    if (!chargeCalleeWork(Work, 1) || !Blocks.emplace(Block.Id, &Block).second)
      return false;
    if (Block.StartAddr == Function.Entry)
      Entry = &Block;
  }
  if (!Entry || !States.at(Entry->Id)->Reached ||
      States.at(Entry->Id)->CallbackOnly)
    return false;
  for (const auto &Block : Function.Blocks) {
    const auto &State = *States.at(Block.Id);
    if (!State.Reached || State.CallbackOnly)
      continue;
    if (!chargeCalleeWork(Work, Block.Ops.size() + Block.Succs.size() + 1))
      return false;
    bool Throws = false;
    for (const auto &Op : Block.Ops) {
      const auto *Call = Proof.callFrameEffect(Op.Addr, Op.Seq);
      if (!Call || !Call->DoesNotReturn)
        continue;
      const auto *Boundary = registration_state::checkedRegistrationDirectCall(
          Image, Function, Op, Work);
      if (!Boundary || Call->EndAddress != Op.Addr + Boundary->Size ||
          Call->Target != Op.Inputs[0].Offset || Call->StackPopBytes ||
          Call->CalleeIndex >= Proof.CalleeContracts.size() ||
          Call->ECXFrameOffset || Call->ECXRuntimeObject ||
          Call->RuntimeThrow || !Call->FrameReads.empty() ||
          !Call->FrameWrites.empty())
        return false;
      const auto &Saved = Proof.CalleeContracts[Call->CalleeIndex];
      if (Saved.isCxxParent()) {
        const auto Current =
            getCheckedX86RegistrationCxxCalleeABI(Image, Call->Target, &Work);
        if (!Current || !Current->DoesNotReturn ||
            !hasSameX86RegistrationCxxEffects(Saved, *Current, Work))
          return false;
      } else {
        const auto Current =
            getCheckedX86RegistrationThrowCalleeABI(Image, Call->Target, &Work);
        if (!Current || Current->IsRethrow ||
            !hasSameX86RegistrationThrowEffects(Saved, *Current, Work))
          return false;
      }
      Throws = true;
      break;
    }
    if (Throws) {
      Pending.push_back(Block.Id);
      continue;
    }
    const auto &Tail = Block.InstructionBoundaries.back();
    std::set<va_t> Expected;
    if (Tail.Control == LowInstructionControl::Branch) {
      if (!Tail.Immediate ||
          hasLowInstructionControlFlag(Tail.ControlFlags,
                                       LowInstructionControlFlag::Indirect))
        return false;
      Expected.insert(*Tail.Immediate);
      if (hasLowInstructionControlFlag(Tail.ControlFlags,
                                       LowInstructionControlFlag::Conditional))
        Expected.insert(Block.EndAddr);
    } else if (Tail.Control == LowInstructionControl::None ||
               Tail.Control == LowInstructionControl::Call) {
      Expected.insert(Block.EndAddr);
    } else {
      return false;
    }
    std::set<va_t> Actual;
    for (int Next : Block.Succs) {
      const auto Found = Blocks.find(Next);
      if (Found == Blocks.end() || !States.at(Next)->Reached ||
          States.at(Next)->CallbackOnly ||
          !Actual.insert(Found->second->StartAddr).second)
        return false;
      Predecessors[Next].push_back(Block.Id);
    }
    if (Actual != Expected)
      return false;
    Remaining.emplace(Block.Id, Block.Succs.size());
  }
  // A closed parent path must end in an authenticated private throw. Neither
  // an unresolved destination nor a cycle can invent a returning entry ABI.
  while (!Pending.empty()) {
    const int Id = Pending.back();
    Pending.pop_back();
    if (!chargeCalleeWork(Work, 1) || !Closed.insert(Id).second)
      return false;
    for (int Previous : Predecessors[Id]) {
      if (!chargeCalleeWork(Work, 1) || !Remaining.at(Previous))
        return false;
      if (!--Remaining.at(Previous))
        Pending.push_back(Previous);
    }
  }
  for (const auto &[Id, State] : States)
    if (!chargeCalleeWork(Work, 1) ||
        (State->Reached && !State->CallbackOnly && !Closed.count(Id)))
      return false;
  return Closed.count(Entry->Id) != 0;
}
} // namespace

std::optional<ParentEntryABI> parentEntryABI(const LowFunc &Function,
                                             const BinaryImage &Image,
                                             size_t *CumulativeWork) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || !Function.ExceptionMetadata ||
      !Function.ExceptionMetadata->Registration ||
      Function.Entry != Function.ExceptionMetadata->CodeRange.Begin ||
      !Function.hasCompleteLiftCoverage() || !Function.RegistrationStates)
    return std::nullopt;
  const auto &EH = *Function.ExceptionMetadata;
  const bool Cxx = EH.Cxx.has_value();
  if (!Cxx && EH.Personality != ExceptionPersonality::ExceptHandler3 &&
      EH.Personality != ExceptionPersonality::ExceptHandler4)
    return std::nullopt;
  const auto &States = *Function.RegistrationStates;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete || !States.ChainOperationsComplete ||
      (Cxx ? !States.CxxContinuationsComplete ||
                 !States.CallFrameEffectsComplete
           : !States.SEHInvocationExitsComplete ||
                 !States.SEHParentChainComplete) ||
      States.Blocks.size() != Function.Blocks.size())
    return std::nullopt;
  if (auto Error = validateLowInstructionBoundaries(
          Function, LowInstructionBoundaryRequirement::Required)) {
    llvm::consumeError(std::move(Error));
    return std::nullopt;
  }
  size_t LocalWork = 0;
  size_t &Work = CumulativeWork ? *CumulativeWork : LocalWork;
  std::map<int, const RegistrationBlockState *> ByBlock;
  for (const auto &State : States.Blocks)
    if (!chargeCalleeWork(Work, 1) ||
        !ByBlock.emplace(State.BlockId, &State).second)
      return std::nullopt;
  std::optional<uint16_t> Pop;
  for (const auto &Block : Function.Blocks) {
    if (!chargeCalleeWork(Work, Block.Ops.size() +
                                    Block.InstructionBoundaries.size() + 1))
      return std::nullopt;
    const auto It = ByBlock.find(Block.Id);
    if (It == ByBlock.end() || It->second->Range.Begin != Block.StartAddr ||
        It->second->Range.End != Block.EndAddr)
      return std::nullopt;
    const auto &State = *It->second;
    if (!State.Reached)
      continue;
    if (State.Unknown)
      return std::nullopt;
    va_t End = Block.EndAddr;
    for (const auto &Op : Block.Ops)
      if (const auto *Call = States.callFrameEffect(Op.Addr, Op.Seq);
          Call && Call->DoesNotReturn)
        End = std::min(End, Call->EndAddress);
    for (const auto &Boundary : Block.InstructionBoundaries) {
      if (Boundary.Address >= End ||
          Boundary.Control != LowInstructionControl::Return)
        continue;
      if (Boundary.Address + Boundary.Size != Block.EndAddr ||
          !Block.Succs.empty() || Block.Ops.empty() ||
          Block.Ops.back().Opcode != NdOp::RETURN ||
          Block.Ops.back().Addr != Boundary.Address)
        return std::nullopt;
      const auto Bytes =
          readImmutableCodeBytes(Image, Boundary.Address, Boundary.Size);
      if (!Bytes || !((Bytes->size() == 1 && (*Bytes)[0] == 0xc3) ||
                      (Bytes->size() == 3 && (*Bytes)[0] == 0xc2)))
        return std::nullopt;
      const uint16_t Count =
          Bytes->size() == 1 ? 0 : readLE<uint16_t>(Bytes->data() + 1);
      if (Boundary.Immediate.value_or(0) != Count || Count % 4 ||
          (State.CallbackOnly && Count))
        return std::nullopt;
      if (!State.CallbackOnly) {
        if (Pop && *Pop != Count)
          return std::nullopt;
        Pop = Count;
      }
    }
  }
  if (Pop)
    return ParentEntryABI{*Pop, true};
  if (hasClosedThrowExits(Function, Image, ByBlock, Work))
    return ParentEntryABI{0, false};
  return std::nullopt;
}
} // namespace neverd::registration_abi
