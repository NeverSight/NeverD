//===- MedCallReturnReads.cpp - Source call-stack observations -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep omitted physical CALL writes from becoming stale source-frame reads.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedCallReturnReads.h"

#include "../../../safety/StackSlotFlow.h"
#include "../X86/SEHCallStackRanges.h"
#include "../abi/MedABIPassDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedProvenStackOffsets.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86/X86RegistrationCallback.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionEdge.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd {
namespace {
struct Access {
  const MedOp *Op;
  int Block;
  size_t Position;
  int64_t Begin;
  int64_t End;

  bool contains(int64_t Byte) const { return Begin <= Byte && Byte < End; }
};

bool charge(size_t &Remaining, size_t Count) {
  if (Count > Remaining) {
    Remaining = 0;
    return false;
  }
  Remaining -= Count;
  return true;
}

/// The registration owner follows SavedESP through memory and runtime entry
/// states. Consume it only at fresh conversion, while the exact LowIR source
/// still owns those facts. Later MedIR offset queries must reprove their own
/// memory state instead of inheriting a source occurrence's old value.
struct RegistrationCalls {
  std::map<const MedOp *, int64_t> Offsets;
  std::set<const MedOp *> Unreached;
  std::set<const MedOp *> PrivateCallbacks;
};

RegistrationCalls registrationCalls(
    const MedFunc &Func, const LowFunc *Source, const BinaryImage &Image,
    const std::map<const MedOp *, MedVar> &Stacks, size_t &Remaining) {
  if (!Source || Image.Arch != Arch::X86 ||
      Image.abiFormat() != BinaryFormat::COFF || Func.AnalystStackFrame ||
      Func.SkippedSSA || Source->Entry != Func.Entry ||
      !Source->RegistrationStates || !Source->ExceptionMetadata)
    return {};
  const auto &States = *Source->RegistrationStates;
  const auto &EH = *Source->ExceptionMetadata;
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete ||
      EH.ParseStatus != ExceptionParseStatus::Complete || !EH.Registration ||
      EH.Registration->RealignedFrame || Func.Entry != EH.CodeRange.Begin ||
      (EH.Encoding != ExceptionEncoding::X86ScopeTableEH3 &&
       EH.Encoding != ExceptionEncoding::X86ScopeTableEH4))
    return {};
  using Identity = std::pair<va_t, int>;
  using Value = std::tuple<MedVar::VarKind, int, int>;
  const auto Key = [](const MedVar &V) {
    return Value{V.Kind, V.Id, V.SSAVer};
  };
  std::map<Identity, const LowOp *> Original;
  std::map<int, const LowBlock *> SourceBlocks;
  std::map<int, const RegistrationBlockState *> StatesByBlock;
  std::map<Identity, std::optional<int32_t>> Frames;
  std::map<Value, const MedOp *> Definitions;
  for (const auto &Frame : States.FrameValues)
    if (!charge(Remaining, 1) ||
        !Frames
             .emplace(Identity{Frame.Address, Frame.OpSeq},
                      Frame.EstablishedFrameOffset)
             .second)
      return {};
  for (const auto &State : States.Blocks)
    if (!charge(Remaining, 1) ||
        !StatesByBlock.emplace(State.BlockId, &State).second)
      return {};
  for (const LowBlock &Block : Source->Blocks) {
    if (!charge(Remaining, 1) || !SourceBlocks.emplace(Block.Id, &Block).second)
      return {};
    for (const LowOp &Op : Block.Ops)
      if (!charge(Remaining, 1) ||
          !Original.emplace(Identity{Op.Addr, Op.Seq}, &Op).second)
        return {};
  }
  const auto Matches = [&](const MedOp &Op) {
    const auto It = Original.find({Op.Addr, Op.OriginSeq});
    if (It == Original.end() || Op.OriginSeq < 0 || Op.Dead ||
        Op.MemoryOrdering != NdMemoryOrdering::None ||
        Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !Op.IntrinsicOutputs.empty())
      return false;
    const LowOp &Low = *It->second;
    if (Op.Opcode != Low.Opcode || Op.NumInputs != Low.NumInputs ||
        Op.Output.Size != Low.Output.Size ||
        (Low.Output.isReg() && (Op.Output.Kind != MedVar::Reg ||
                                Op.Output.RegOff != Low.Output.Offset)))
      return false;
    for (unsigned I = 0; I < Op.NumInputs; ++I) {
      if (Op.Inputs[I].Size != Low.Inputs[I].Size)
        return false;
      if (Low.Inputs[I].isConst() &&
          (!Op.Inputs[I].isConst() ||
           Op.Inputs[I].ConstVal != Low.Inputs[I].Offset))
        return false;
    }
    return true;
  };
  const auto &TRI = getTargetRegInfo(Image.Arch);
  RegistrationCalls Result;
  std::vector<MedVar> Values;
  std::vector<int32_t> FrameOffsets;
  for (const MedBlock &Block : Func.Blocks) {
    const auto SourceBlock = SourceBlocks.find(Block.Id);
    const auto State = StatesByBlock.find(Block.Id);
    const bool Unreached = Block.StartAddr != Func.Entry &&
                           SourceBlock != SourceBlocks.end() &&
                           State != StatesByBlock.end() &&
                           !State->second->Reached && !State->second->Unknown &&
                           SourceBlock->second->StartAddr == Block.StartAddr &&
                           SourceBlock->second->EndAddr == Block.EndAddr &&
                           State->second->Range.Begin == Block.StartAddr &&
                           State->second->Range.End == Block.EndAddr;
    for (const PhiNode &Phi : Block.Phis)
      if (!charge(Remaining, 1) ||
          !Definitions.emplace(Key(Phi.Output), nullptr).second)
        return {};
    for (const MedOp &Op : Block.Ops) {
      if (!charge(Remaining, 1))
        return {};
      // Complete registration replay also proves unreachable ordinary arms.
      // Such an arm has no dynamic frame value; it cannot overwrite a live
      // observation merely because the source CFG still retains its block.
      if (Unreached &&
          (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Matches(Op))
        Result.Unreached.insert(&Op);
      if (Op.Output.isConst() || !Op.Output.Size)
        continue;
      if (!Definitions.emplace(Key(Op.Output), &Op).second)
        return {};
      const auto Frame = Frames.find({Op.Addr, Op.OriginSeq});
      if (Op.Output.Kind != MedVar::Reg ||
          Op.Output.RegOff != TRI.StackPointer ||
          Op.Output.Size != TRI.PointerSize || Frame == Frames.end() ||
          !Frame->second || !Matches(Op))
        continue;
      Values.push_back(Op.Output);
      FrameOffsets.push_back(*Frame->second);
    }
  }
  // Establish the relation between entry SP and the runtime's frame from
  // current affine definitions. Do not assign the callback's private ESP the
  // parent's origin or assume a particular prologue allocation.
  const auto Offsets = entryStackOffsets(Func, Values, Image.Arch,
                                         Image.abiFormat(), Func.EntryKind);
  std::optional<int64_t> Base;
  std::map<Value, int32_t> Bound;
  for (size_t I = 0; I < Values.size(); ++I) {
    Bound.emplace(Key(Values[I]), FrameOffsets[I]);
    if (!Offsets[I])
      continue;
    const int64_t Candidate = *Offsets[I] - int64_t(FrameOffsets[I]);
    if (Base && *Base != Candidate)
      return {};
    Base = Candidate;
  }
  if (!Base)
    return Result;
  std::vector<ProvenStackOffset> Seeds;
  for (const auto &[Value, Offset] : Bound)
    Seeds.push_back({Definitions.at(Value), *Base + Offset});
  Values.clear();
  for (const auto &[Call, Stack] : Stacks)
    Values.push_back(Stack);
  const auto Restored = entryStackOffsetsFromProvenDefinitions(
      Func, Values, Seeds, Image.Arch, Image.abiFormat(), Func.EntryKind);
  size_t I = 0;
  for (const auto &[Call, Stack] : Stacks) {
    const auto Offset = Restored[I++];
    if (!charge(Remaining, 1))
      return {};
    if (Offset && Matches(*Call))
      Result.Offsets.emplace(Call, *Offset);
  }
  const auto Owners = registrationSEHCallbackOwners(Func);
  if (!Owners)
    return Result;
  std::map<va_t, const MedOp *> Roots;
  std::map<va_t, std::vector<const MedOp *>> CallbackCalls;
  for (const MedBlock &Block : Func.Blocks) {
    const auto Owner = Owners->find(Block.Id);
    if (Owner == Owners->end())
      continue;
    for (const MedOp &Op : Block.Ops) {
      if (!charge(Remaining, 1))
        return {};
      if (Op.RegistrationRoot ==
              MedOp::RegistrationRootKind::CallbackStackPointer &&
          Op.Addr == Owner->second.first && Op.Addr == Block.StartAddr &&
          hasValidRegistrationRootShape(Op) && !Op.Dead) {
        if (!Roots.emplace(Op.Addr, &Op).second)
          return {};
      }
      if (Stacks.count(&Op) && !Result.Offsets.count(&Op) &&
          !Result.Unreached.count(&Op) && Matches(Op))
        CallbackCalls[Owner->second.first].push_back(&Op);
    }
  }
  for (const auto &[Entry, Calls] : CallbackCalls) {
    const auto Root = Roots.find(Entry);
    if (Root == Roots.end())
      continue;
    Values.clear();
    for (const MedOp *Call : Calls)
      Values.push_back(Stacks.at(Call));
    const auto Private = stackOffsetsRelativeToDefinition(
        Func, Values, *Root->second, Image.Arch, Image.abiFormat());
    for (size_t I = 0; I < Calls.size(); ++I) {
      if (!charge(Remaining, 1))
        return {};
      // The checked callback-memory owner separates this invocation's
      // allocation below its return PC from the established parent frame.
      // A CALL allocates its return slot below the same private ESP. This
      // gives no separation after restoring ESP from the parent frame.
      if (Private[I] && *Private[I] <= 0 &&
          *Private[I] >= -int64_t(limits::kMaxSourceCallBorrowedBytes))
        Result.PrivateCallbacks.insert(Calls[I]);
    }
  }
  return Result;
}
} // namespace

bool validateSourceCallReturnReads(const MedFunc &Func,
                                   const BinaryImage &Image,
                                   std::string &Diagnostic, size_t WorkLimit,
                                   const LowFunc *FreshSource) {
  Diagnostic.clear();
  // Link-register targets have no implicit stack write. The current source
  // call encoding authority authenticates x86 near CALL/JMP occurrences.
  if (!getTargetRegInfo(Image.Arch).CallPushesReturnAddress ||
      Func.Blocks.empty())
    return true;
  auto Refuse = [&](const std::string &Reason) {
    Diagnostic = "source frame in " + Func.Name + ": " + Reason;
    return false;
  };
  auto Exhausted = [&] {
    return Refuse("CALL return-slot observation analysis exceeded its work "
                  "budget; refusing an unproved frame projection");
  };
  size_t Remaining = WorkLimit;
  std::vector<const MedOp *> Calls;
  std::vector<const MedOp *> Memory;
  std::map<const MedOp *, std::pair<int, size_t>> Positions;
  for (const MedBlock &Block : Func.Blocks) {
    if (!charge(Remaining, 1) || !charge(Remaining, Block.Ops.size()))
      return Exhausted();
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const auto &Op = Block.Ops[I];
      if (Op.Dead)
        continue;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        const auto Bias =
            sourceCallReturnAddressBias(&Image, Image.Arch, Op.Addr);
        // Synthetic tail calls must never acquire a physical push. Unknown
        // original encodings remain possible writes, not zero-byte writes.
        if (!Bias || *Bias != 0) {
          Calls.push_back(&Op);
          Positions.emplace(&Op, std::pair{Block.Id, I});
        }
      }
      if (Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          ((Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1) ||
           (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2))) {
        Memory.push_back(&Op);
        Positions.emplace(&Op, std::pair{Block.Id, I});
      }
    }
  }
  if (Calls.empty() || Memory.empty())
    return true;
  std::map<int, MedVar> BlockEntries;
  const auto StackPointers =
      sourceCallStackPointers(Func, Image.Arch, Remaining, BlockEntries);
  if (!Remaining)
    return Exhausted();
  const auto Width = getTargetRegInfo(Image.Arch).PointerSize;
  std::vector<MedVar> Values;
  for (const auto *Op : Memory)
    Values.push_back(Op->Inputs[0]);
  for (const auto *Op : Calls) {
    const auto It = StackPointers.find(Op);
    Values.push_back(It == StackPointers.end()
                         ? MedVar::makeUnspecified(Width, Image.Arch)
                         : It->second);
  }
  const auto Offsets = entryStackOffsets(Func, Values, Image.Arch,
                                         Image.abiFormat(), Func.EntryKind);
  std::vector<Access> Loads;
  std::map<const MedOp *, Access> Stores;
  for (size_t I = 0; I < Memory.size(); ++I) {
    const auto *Op = Memory[I];
    const auto Size =
        Op->Opcode == NdOp::LOAD ? Op->Output.Size : Op->Inputs[1].Size;
    if (!Offsets[I] || !Size ||
        *Offsets[I] > std::numeric_limits<int64_t>::max() - Size)
      continue;
    const auto [Block, Position] = Positions.at(Op);
    Access A{Op, Block, Position, *Offsets[I], *Offsets[I] + Size};
    if (Op->Opcode == NdOp::LOAD)
      Loads.push_back(A);
    else if (Op->MemoryOrdering == NdMemoryOrdering::None &&
             Op->MemoryAddressSpace == NdMemoryAddressSpace::Default)
      Stores.emplace(Op, A);
  }
  std::map<const MedOp *, std::optional<EntryStackOffsetRange>> CallEnds;
  for (size_t I = 0; I < Calls.size(); ++I)
    if (const auto Offset = Offsets[Memory.size() + I])
      CallEnds.emplace(Calls[I], EntryStackOffsetRange{*Offset, *Offset});
    else
      CallEnds.emplace(Calls[I], std::nullopt);
  if (llvm::any_of(CallEnds, [](const auto &Entry) { return !Entry.second; })) {
    const auto Registration =
        registrationCalls(Func, FreshSource, Image, StackPointers, Remaining);
    if (!Remaining)
      return Exhausted();
    for (auto &[Call, End] : CallEnds)
      if (!End)
        if (const auto Found = Registration.Offsets.find(Call);
            Found != Registration.Offsets.end())
          End = EntryStackOffsetRange{Found->second, Found->second};
    for (const MedOp *Call : Registration.Unreached)
      CallEnds.erase(Call);
    for (const MedOp *Call : Registration.PrivateCallbacks)
      CallEnds.erase(Call);
  }
  std::vector<const MedOp *> UnboundedCalls;
  std::vector<MedVar> UnboundedStacks;
  for (const auto &[Call, End] : CallEnds)
    if (!End)
      if (const auto Found = StackPointers.find(Call);
          Found != StackPointers.end()) {
        UnboundedCalls.push_back(Call);
        UnboundedStacks.push_back(Found->second);
      }
  if (!UnboundedCalls.empty()) {
    const auto Bounds = entryStackOffsetRanges(
        Func, UnboundedStacks, Image.Arch, Image.abiFormat(), Func.EntryKind);
    for (size_t I = 0; I < Bounds.size(); ++I)
      CallEnds.at(UnboundedCalls[I]) = Bounds[I];
  }
  if (FreshSource &&
      llvm::any_of(CallEnds, [](const auto &Entry) { return !Entry.second; })) {
    const auto HandlerBounds = x86_seh::sourceHandlerCallStackRanges(
        Func, *FreshSource, Image, StackPointers, BlockEntries, Remaining);
    if (!Remaining)
      return Exhausted();
    for (auto &[Call, End] : CallEnds)
      if (!End)
        if (const auto Found = HandlerBounds.find(Call);
            Found != HandlerBounds.end())
          End = Found->second;
  }
  // A CALL writes its return slot below the stack pointer it runs at. Where
  // that position has no bounded range, as below a dynamic allocation in a
  // loop, an upper bound alone still keeps the slot off every byte above it.
  std::map<const MedOp *, int64_t> CallBelow;
  {
    std::vector<const MedOp *> Pending;
    std::vector<MedVar> PendingStacks;
    for (const auto &[Call, End] : CallEnds)
      if (!End)
        if (const auto Found = StackPointers.find(Call);
            Found != StackPointers.end()) {
          Pending.push_back(Call);
          PendingStacks.push_back(Found->second);
        }
    if (!Pending.empty()) {
      if (!charge(Remaining, Pending.size()))
        return Exhausted();
      const auto Bounds = entryStackOffsetUpperBounds(
          Func, PendingStacks, Image.Arch, Image.abiFormat(), Func.EntryKind);
      for (size_t I = 0; I < Pending.size(); ++I)
        if (Bounds[I])
          CallBelow.emplace(Pending[I], *Bounds[I]);
    }
  }
  std::vector<std::pair<int64_t, int64_t>> Ranges;
  bool UnknownLocation = false;
  for (const auto &[Call, End] : CallEnds) {
    if (!End)
      if (const auto Below = CallBelow.find(Call); Below != CallBelow.end()) {
        Ranges.emplace_back(std::numeric_limits<int64_t>::min(), Below->second);
        continue;
      }
    if (!End || End->Min < std::numeric_limits<int64_t>::min() + Width) {
      UnknownLocation = true;
      continue;
    }
    Ranges.emplace_back(End->Min - Width, End->Max);
  }
  llvm::sort(Ranges);
  std::vector<std::pair<int64_t, int64_t>> Merged;
  for (const auto &Range : Ranges) {
    if (Merged.empty() || Merged.back().second < Range.first)
      Merged.push_back(Range);
    else
      Merged.back().second = std::max(Merged.back().second, Range.second);
  }
  auto OverlapsCall = [&](const Access &Load) {
    if (UnknownLocation)
      return true;
    const auto It = std::lower_bound(
        Merged.begin(), Merged.end(), Load.Begin,
        [](const auto &Range, int64_t Begin) { return Range.second <= Begin; });
    return It != Merged.end() && It->first < Load.End;
  };
  if (llvm::none_of(Loads, OverlapsCall))
    return true;
  for (const MedBlock &Block : Func.Blocks)
    if (!charge(Remaining, Block.Preds.size()) ||
        !charge(Remaining, Block.Succs.size()) ||
        !charge(Remaining, Block.ExceptionalPreds.size()) ||
        !charge(Remaining, Block.ExceptionalSuccs.size()))
      return Exhausted();
  const MedBlockIdIndex Blocks(Func);
  const safety::detail::ExactPhiGraph Graph(Func);
  for (const MedBlock &Block : Func.Blocks)
    if (!Graph.hasCompleteNormalPredecessors(Block))
      return Refuse("CALL return-slot observation requires a complete normal "
                    "control-flow graph");
  std::map<int, std::set<ExceptionalEdge>> ExceptionalIncoming;
  for (const MedBlock &Block : Func.Blocks)
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs) {
      if (!charge(Remaining, Edge.contexts().size()))
        return Exhausted();
      if (Edge.BlockId >= 0) {
        auto Reverse = Edge;
        Reverse.BlockId = Block.Id;
        ExceptionalIncoming[Edge.BlockId].insert(std::move(Reverse));
      }
    }
  // Once stack addresses are fixed, only a proven STORE or a possible CALL
  // write changes this byte history. Canonicalize operation prefixes to these
  // events so arithmetic, loads and repeated exception prefixes share states.
  std::map<int, std::vector<size_t>> Events;
  for (const MedBlock &Block : Func.Blocks)
    Events.try_emplace(Block.Id);
  for (const auto &[Op, Store] : Stores)
    Events.at(Store.Block).push_back(Store.Position);
  for (const auto &[Op, End] : CallEnds) {
    const auto [Block, Position] = Positions.at(Op);
    Events.at(Block).push_back(Position);
  }
  if (!charge(Remaining, Stores.size() + CallEnds.size()))
    return Exhausted();
  for (auto &[Block, Positions] : Events)
    llvm::sort(Positions);
  const auto Prefix = [&](int Block, size_t Position) {
    const auto &Positions = Events.at(Block);
    return size_t(
        std::lower_bound(Positions.begin(), Positions.end(), Position) -
        Positions.begin());
  };
  using State = std::pair<int, size_t>;
  std::map<int, std::vector<State>> ExceptionalPrefixes;
  std::map<int64_t, std::set<State>> ProvenBytes;
  // Walk only candidate bytes. A store on one predecessor must not hide an
  // implicit CALL write reaching the load through another predecessor or loop.
  for (const Access &Load : Loads) {
    if (!OverlapsCall(Load))
      continue;
    for (int64_t Byte = Load.Begin; Byte < Load.End; ++Byte) {
      auto &Proven = ProvenBytes[Byte];
      std::vector<State> Pending{
          {Load.Block, Prefix(Load.Block, Load.Position)}};
      std::set<State> Seen;
      while (!Pending.empty()) {
        if (!charge(Remaining, 1))
          return Exhausted();
        const auto Current = Pending.back();
        Pending.pop_back();
        if (Proven.count(Current) || !Seen.insert(Current).second)
          continue;
        const auto *Block = Blocks.first(Current.first);
        if (!Block)
          return Refuse("CALL return-slot observation has a missing block");
        if (Current.second) {
          const auto &Op = Block->Ops[Events.at(Block->Id)[Current.second - 1]];
          if (const auto It = Stores.find(&Op);
              It != Stores.end() && It->second.contains(Byte))
            continue;
          const auto Call = CallEnds.find(&Op);
          const auto Below = CallBelow.find(&Op);
          if (Call != CallEnds.end() &&
              (!Call->second
                   ? Below == CallBelow.end() || Byte < Below->second
                   : Call->second->Min <
                             std::numeric_limits<int64_t>::min() + Width ||
                         (Byte < Call->second->Max &&
                          Byte >= Call->second->Min - Width)))
            return Refuse("load at 0x" + llvm::utohexstr(Load.Op->Addr) +
                          " observes a physical CALL return slot from 0x" +
                          llvm::utohexstr(Op.Addr) +
                          "; the ordinary C frame does not represent that "
                          "write; refusing a stale frame value");
          Pending.emplace_back(Block->Id, Current.second - 1);
          continue;
        }
        // An exception can leave before a later store executes. Follow every
        // possible source prefix, not the predecessor's final memory state.
        // This remains unnecessary when a local store ended the byte history.
        auto [Prefixes, New] = ExceptionalPrefixes.try_emplace(Block->Id);
        if (New) {
          auto UnprovedEntry = [&] {
            return Refuse("CALL return-slot observation at 0x" +
                          llvm::utohexstr(Load.Op->Addr) +
                          " reaches an unproved exceptional entry at 0x" +
                          llvm::utohexstr(Block->StartAddr));
          };
          std::set<ExceptionalEdge> Declared;
          for (const ExceptionalEdge &Edge : Block->ExceptionalPreds) {
            if (!charge(Remaining, Edge.contexts().size()))
              return Exhausted();
            Declared.insert(Edge);
          }
          if (Declared != ExceptionalIncoming[Block->Id])
            return UnprovedEntry();
          for (const ExceptionalEdge &Edge : Declared) {
            const auto *Pred = Blocks.first(Edge.BlockId);
            if (!Pred || Edge.TargetVA != Block->StartAddr)
              return UnprovedEntry();
            size_t Begin = 0, End = Pred->Ops.size();
            if (Edge.SourceVA != InvalidVA) {
              if (!charge(Remaining, Pred->Ops.size()))
                return Exhausted();
              Begin = Pred->Ops.size();
              End = 0;
              for (size_t I = 0; I < Pred->Ops.size(); ++I)
                if (Pred->Ops[I].Addr == Edge.SourceVA) {
                  Begin = std::min(Begin, I);
                  End = I + 1;
                }
              if (Begin == Pred->Ops.size())
                return UnprovedEntry();
            }
            // Include both sides of a throwing instruction: its CALL may
            // push, whereas its STORE need not complete. A protected range
            // conservatively includes every operation prefix in the block.
            const size_t First = Prefix(Pred->Id, Begin);
            const size_t Last = Prefix(Pred->Id, End);
            if (!charge(Remaining, Last - First + 1))
              return Exhausted();
            for (size_t I = First; I <= Last; ++I)
              Prefixes->second.emplace_back(Pred->Id, I);
          }
        }
        if (!charge(Remaining, Prefixes->second.size()))
          return Exhausted();
        Pending.insert(Pending.end(), Prefixes->second.begin(),
                       Prefixes->second.end());
        if (!charge(Remaining, Block->Preds.size()))
          return Exhausted();
        for (int Id : Block->Preds) {
          const auto *Pred = Blocks.first(Id);
          if (!Pred)
            return Refuse(
                "CALL return-slot observation has a missing predecessor");
          Pending.emplace_back(Id, Events.at(Id).size());
        }
      }
      // All histories from these states have now completed without a CALL
      // hazard. Repeated reads and exceptional prefixes share the proof only
      // for this byte; no partially explored cycle is published.
      if (!charge(Remaining, Seen.size()))
        return Exhausted();
      Proven.insert(Seen.begin(), Seen.end());
    }
  }
  return true;
}
} // namespace neverd
