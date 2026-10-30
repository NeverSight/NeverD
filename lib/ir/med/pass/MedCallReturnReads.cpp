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
#include "../X86/RegistrationCallStacks.h"
#include "../X86/SEHCallStackRanges.h"
#include "../abi/MedABIPassDetail.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/MedCallLeftovers.h"
#include "neverd/ir/med/MedCallingConvDetail.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/med/MedProvenStackOffsets.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86/X86RegistrationCallback.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionEdge.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MathExtras.h"

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

// Decode only potential represented writes, never every call in an ordinary
// function. Instruction length belongs to the decoder, not to a C spelling or
// the next recovered operation's address.
std::optional<va_t> callFallthrough(const BinaryImage &Image, va_t Address,
                                    Decoder &Decode) {
  const auto Bias = sourceCallReturnAddressBias(&Image, Image.Arch, Address);
  if (!Bias || *Bias != getTargetRegInfo(Image.Arch).PointerSize)
    return std::nullopt;
  const uint8_t *Bytes = nullptr;
  size_t Size = 15;
  for (; Size && !(Bytes = Image.readVA(Address, Size)); --Size) {
  }
  DecodedInsn I{};
  if (!Size || !Decode.decodeOne(Bytes, Size, Address, I) ||
      I.Id != X86_INS_CALL || !I.Raw->detail ||
      I.Raw->detail->x86.op_count != 1 ||
      I.Raw->detail->x86.operands[0].size !=
          getTargetRegInfo(Image.Arch).PointerSize ||
      Address >= InvalidVA - I.Size)
    return std::nullopt;
  if (Image.Arch == Arch::X86) {
    if (Address > UINT32_MAX)
      return std::nullopt;
    return uint32_t(Address + I.Size);
  }
  return Address + I.Size;
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

/// Without the complete state proof, the EH3/EH4 frame protocol still places
/// the stacks a handler restores and those a runtime callback runs on.
RegistrationCalls protocolCalls(
    const MedFunc &Func, const LowFunc &Source, const BinaryImage &Image,
    const std::map<const MedOp *, MedVar> &Stacks,
    const std::map<const MedOp *, int64_t> &Accesses, size_t &Remaining) {
  RegistrationCalls Result;
  const auto Seeds = x86_registration::savedStackReloads(Func, Source, Image,
                                                         Accesses, Remaining);
  if (!Seeds.empty()) {
    std::vector<MedVar> Values;
    for (const auto &[Call, Stack] : Stacks)
      Values.push_back(Stack);
    if (!charge(Remaining, Values.size()))
      return {};
    const auto Restored = entryStackOffsetsFromProvenDefinitions(
        Func, Values, Seeds, Image.Arch, Image.abiFormat(), Func.EntryKind);
    size_t I = 0;
    for (const auto &[Call, Stack] : Stacks)
      if (const auto Offset = Restored[I++])
        Result.Offsets.emplace(Call, *Offset);
  }
  std::map<const MedOp *, MedVar> Unplaced;
  for (const auto &[Call, Stack] : Stacks)
    if (!Result.Offsets.count(Call))
      Unplaced.emplace(Call, Stack);
  Result.PrivateCallbacks =
      x86_registration::privateCallbackCalls(Func, Image, Unplaced, Remaining);
  return Result;
}

RegistrationCalls registrationCalls(
    const MedFunc &Func, const LowFunc *Source, const BinaryImage &Image,
    const std::map<const MedOp *, MedVar> &Stacks,
    const std::map<const MedOp *, int64_t> &Accesses, size_t &Remaining) {
  if (!Source || Image.Arch != Arch::X86 ||
      Image.abiFormat() != BinaryFormat::COFF || Func.AnalystStackFrame ||
      Func.SkippedSSA || Source->Entry != Func.Entry ||
      !Source->RegistrationStates || !Source->ExceptionMetadata)
    return {};
  const auto &States = *Source->RegistrationStates;
  const auto &EH = *Source->ExceptionMetadata;
  if (EH.ParseStatus != ExceptionParseStatus::Complete || !EH.Registration ||
      EH.Registration->RealignedFrame || Func.Entry != EH.CodeRange.Begin ||
      (EH.Encoding != ExceptionEncoding::X86ScopeTableEH3 &&
       EH.Encoding != ExceptionEncoding::X86ScopeTableEH4))
    return {};
  if (!States.Complete || !States.CallbackStatesComplete ||
      !States.RegistrationLifetimeComplete)
    return protocolCalls(Func, *Source, Image, Stacks, Accesses, Remaining);
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

static bool checkSourceCallReturnReads(
    const MedFunc &Func, const BinaryImage &Image, std::string &Diagnostic,
    size_t &Remaining, const LowFunc *FreshSource,
    std::set<const MedOp *> *ObservedCalls,
    const MedCallReturnRanges *CallReturns,
    std::vector<MedCallReturnRead> *Deferred, bool *NeedsCalleeFacts) {
  Diagnostic.clear();
  if (NeedsCalleeFacts)
    *NeedsCalleeFacts = false;
  // Link-register targets have no implicit stack write. The current source
  // call encoding authority authenticates x86 near CALL/JMP occurrences.
  if (!getTargetRegInfo(Image.Arch).CallPushesReturnAddress ||
      Func.Blocks.empty())
    return true;
  auto Explain = [&](const std::string &Reason) {
    return "source frame in " + Func.Name + ": " + Reason;
  };
  auto Refuse = [&](const std::string &Reason) {
    Diagnostic = Explain(Reason);
    return false;
  };
  auto Exhausted = [&] {
    return Refuse("CALL return-slot observation analysis exceeded its work "
                  "budget; refusing an unproved frame projection");
  };
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
          (med_calling_conv_detail::stackMemoryRead(Op) ||
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
  const int64_t MinimumOffset = llvm::minIntN(Width * 8);
  const int64_t MaximumOffset = llvm::maxIntN(Width * 8);
  std::vector<MedVar> Values;
  for (const auto *Op : Memory) {
    const auto Read = med_calling_conv_detail::stackMemoryRead(*Op);
    Values.push_back(Op->Inputs[Read ? Read->AddressInput : 0]);
  }
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
    const auto Read = med_calling_conv_detail::stackMemoryRead(*Op);
    const auto Size = Read ? Read->Bytes : Op->Inputs[1].Size;
    if (!Offsets[I] || !Size)
      continue;
    // The beginning fits the native coordinate by entryStackOffsets. Keep
    // the whole access in that coordinate too: skipping a wrapping load
    // would remove an observer, while a wrapping store proves no overwrite.
    if (*Offsets[I] > MaximumOffset - Size) {
      if (Read)
        return Refuse("load at 0x" + llvm::utohexstr(Op->Addr) +
                      " exceeds the native stack-offset interval; refusing "
                      "an unproved frame projection");
      continue;
    }
    const auto [Block, Position] = Positions.at(Op);
    Access A{Op, Block, Position, *Offsets[I], *Offsets[I] + Size};
    if (Read)
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
  // An explicit store of this CALL's actual next PC already represents its
  // implicit write. Check the current operations and stack coordinates anew;
  // no persisted annotation may exempt a changed source or changed image.
  std::optional<Decoder> Decode;
  std::map<std::pair<int, size_t>, const Access *> StoresByPosition;
  for (const auto &[Op, A] : Stores)
    StoresByPosition.emplace(std::pair{A.Block, A.Position}, &A);
  for (auto It = CallEnds.begin(); It != CallEnds.end();) {
    const MedOp *Call = It->first;
    const auto [BlockId, Position] = Positions.at(Call);
    const MedOp *Store = nullptr;
    if (Position && It->second) {
      const auto Previous = StoresByPosition.find({BlockId, Position - 1});
      if (Previous != StoresByPosition.end()) {
        const auto &A = *Previous->second;
        if (It->second->Min >= std::numeric_limits<int64_t>::min() + Width &&
            A.Begin == It->second->Min - Width && A.End == It->second->Max &&
            A.Op->Inputs[1].isConst())
          Store = A.Op;
      }
    }
    if (Store) {
      if (!Decode) {
        Decode.emplace();
        if (!Decode->init(Image))
          return Refuse("CALL return-slot encoding decoder is unavailable");
      }
      if (!charge(Remaining, 1))
        return Exhausted();
      const auto PC = callFallthrough(Image, Call->Addr, *Decode);
      if (PC && Store->Inputs[1].ConstVal == *PC) {
        It = CallEnds.erase(It);
        continue;
      }
    }
    ++It;
  }
  const auto CallSlotBegin =
      [&](const EntryStackOffsetRange &End) -> std::optional<int64_t> {
    // SP can fit while the implicit push crosses the native coordinate.
    // Such a write has no representable interval in this proof.
    if (End.Min < MinimumOffset + Width || End.Max > MaximumOffset ||
        End.Min > End.Max)
      return std::nullopt;
    return End.Min - Width;
  };
  // Whether every load's history reaches no CALL write in Ends, which are
  // bounded from above by Bounds where they have no range. With Defer, a load
  // such a write reaches is recorded there and leaves the proof instead.
  const auto Observe = [&](const std::map<const MedOp *,
                                          std::optional<EntryStackOffsetRange>>
                               &Ends,
                           const std::map<const MedOp *, int64_t> &Bounds,
                           size_t &Budget,
                           std::vector<MedCallReturnRead> *Defer) {
    std::vector<std::pair<int64_t, int64_t>> Ranges;
    bool UnknownLocation = false;
    for (const auto &[Call, End] : Ends) {
      if (!End)
        if (const auto Below = Bounds.find(Call); Below != Bounds.end()) {
          Ranges.emplace_back(std::numeric_limits<int64_t>::min(),
                              Below->second);
          continue;
        }
      const auto Begin = End ? CallSlotBegin(*End) : std::nullopt;
      if (!Begin) {
        UnknownLocation = true;
        continue;
      }
      Ranges.emplace_back(*Begin, End->Max);
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
      const auto It = std::lower_bound(Merged.begin(), Merged.end(), Load.Begin,
                                       [](const auto &Range, int64_t Begin) {
                                         return Range.second <= Begin;
                                       });
      return It != Merged.end() && It->first < Load.End;
    };
    if (llvm::none_of(Loads, OverlapsCall))
      return true;
    for (const MedBlock &Block : Func.Blocks)
      if (!charge(Budget, Block.Preds.size()) ||
          !charge(Budget, Block.Succs.size()) ||
          !charge(Budget, Block.ExceptionalPreds.size()) ||
          !charge(Budget, Block.ExceptionalSuccs.size()))
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
        if (!charge(Budget, Edge.contexts().size()))
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
    for (const auto &[Op, End] : Ends) {
      const auto [Block, Position] = Positions.at(Op);
      Events.at(Block).push_back(Position);
    }
    if (!charge(Budget, Stores.size() + Ends.size()))
      return Exhausted();
    for (auto &[Block, Positions] : Events)
      llvm::sort(Positions);
    const auto Prefix = [&](int Block, size_t Position) {
      const auto &Positions = Events.at(Block);
      return size_t(
          std::lower_bound(Positions.begin(), Positions.end(), Position) -
          Positions.begin());
    };
    // A byte's history changes only at a STORE of the byte and at a CALL whose
    // return slot may cover it. Bytes between consecutive ends of those writes
    // and of the observing loads meet every such test alike, so one bit
    // carries the history of a whole interval.
    std::vector<const Access *> Observers;
    std::vector<std::pair<int64_t, int64_t>> Spans;
    for (const Access &Load : Loads)
      if (OverlapsCall(Load)) {
        Observers.push_back(&Load);
        Spans.emplace_back(Load.Begin, Load.End);
      }
    llvm::sort(Spans);
    size_t SpanCount = 0;
    for (const auto &Span : Spans)
      if (SpanCount && Spans[SpanCount - 1].second >= Span.first)
        Spans[SpanCount - 1].second =
            std::max(Spans[SpanCount - 1].second, Span.second);
      else
        Spans[SpanCount++] = Span;
    Spans.resize(SpanCount);
    const auto Inside = [&](int64_t Byte) {
      const auto It =
          llvm::upper_bound(Spans, Byte, [](int64_t Value, const auto &Span) {
            return Value < Span.first;
          });
      return It != Spans.begin() && Byte < std::prev(It)->second;
    };
    std::vector<int64_t> Cuts;
    for (const auto &[Begin, End] : Spans) {
      Cuts.push_back(Begin);
      Cuts.push_back(End);
    }
    const auto Cut = [&](int64_t Byte) {
      if (Inside(Byte))
        Cuts.push_back(Byte);
    };
    for (const auto &[Op, Store] : Stores) {
      Cut(Store.Begin);
      Cut(Store.End);
    }
    for (const auto &[Op, End] : Ends) {
      if (End)
        if (const auto Begin = CallSlotBegin(*End)) {
          Cut(*Begin);
          Cut(End->Max);
        }
      if (const auto Below = Bounds.find(Op); Below != Bounds.end())
        Cut(Below->second);
    }
    if (!charge(Budget, Cuts.size()))
      return Exhausted();
    llvm::sort(Cuts);
    Cuts.erase(std::unique(Cuts.begin(), Cuts.end()), Cuts.end());
    std::vector<int64_t> SegmentBegins, SegmentEnds;
    for (size_t I = 0; I + 1 < Cuts.size(); ++I)
      if (Inside(Cuts[I])) {
        SegmentBegins.push_back(Cuts[I]);
        SegmentEnds.push_back(Cuts[I + 1]);
      }
    using Intervals = std::pair<size_t, size_t>;
    // Every cut range covers each interval wholly or not at all.
    const auto Covered = [&](int64_t Begin, int64_t End) {
      const size_t Low =
          llvm::upper_bound(SegmentEnds, Begin) - SegmentEnds.begin();
      const size_t High =
          llvm::lower_bound(SegmentBegins, End) - SegmentBegins.begin();
      return Intervals{Low, std::max(Low, High)};
    };
    const Intervals Everything{0, SegmentBegins.size()};
    const auto IndexOf = [&](const MedBlock *Block) {
      return size_t(Block - Func.Blocks.data());
    };
    const size_t BlockCount = Func.Blocks.size();
    // The events of each block in order: the intervals a STORE ends and those
    // a CALL's return slot may write.
    struct Write {
      const MedOp *Op = nullptr;
      bool Call = false;
      Intervals Range;
    };
    std::vector<std::vector<Write>> Table(BlockCount);
    for (const auto &[Id, Ordered] : Events) {
      const MedBlock *Block = Blocks.first(Id);
      if (!Block)
        return Refuse("CALL return-slot observation has a missing block");
      auto &Row = Table[IndexOf(Block)];
      for (size_t Position : Ordered) {
        if (Position >= Block->Ops.size())
          return Refuse("CALL return-slot observation has a missing block");
        Write Item;
        Item.Op = &Block->Ops[Position];
        if (const auto Store = Stores.find(Item.Op); Store != Stores.end()) {
          Item.Range = Covered(Store->second.Begin, Store->second.End);
        } else if (const auto Call = Ends.find(Item.Op); Call != Ends.end()) {
          Item.Call = true;
          const auto Below = Bounds.find(Item.Op);
          if (!Call->second)
            Item.Range = Below == Bounds.end()
                             ? Everything
                             : Covered(std::numeric_limits<int64_t>::min(),
                                       Below->second);
          else if (const auto Begin = CallSlotBegin(*Call->second))
            Item.Range = Covered(*Begin, Call->second->Max);
          else
            Item.Range = Everything;
        }
        Row.push_back(Item);
      }
    }
    // Histories run backward along the predecessors each block lists.
    std::vector<std::vector<size_t>> Predecessors(BlockCount),
        Successors(BlockCount);
    std::vector<char> MissingPredecessor(BlockCount, 0);
    for (size_t Index = 0; Index < BlockCount; ++Index) {
      const MedBlock &Block = Func.Blocks[Index];
      if (Blocks.first(Block.Id) != &Block)
        continue;
      for (int Id : Block.Preds)
        if (const auto *Pred = Blocks.first(Id)) {
          Predecessors[Index].push_back(IndexOf(Pred));
          Successors[IndexOf(Pred)].push_back(Index);
        } else {
          MissingPredecessor[Index] = 1;
        }
    }
    // An exception can leave before a later store executes, so a handler's
    // history enters every operation prefix of its protected source.
    struct Raise {
      size_t First, Last, Handler;
    };
    std::vector<std::vector<Raise>> Raises(BlockCount);
    std::vector<std::vector<size_t>> Protected(BlockCount);
    // Loads observe their intervals at their operation prefixes.
    struct Observation {
      size_t State;
      Intervals Range;
      const Access *Load;
    };
    std::vector<std::vector<Observation>> Observations(BlockCount);
    for (const Access *Load : Observers) {
      const auto *Block = Blocks.first(Load->Block);
      if (!Block)
        return Refuse("CALL return-slot observation has a missing block");
      Observations[IndexOf(Block)].push_back(
          {Prefix(Load->Block, Load->Position), Covered(Load->Begin, Load->End),
           Load});
    }
    for (auto &Loaded : Observations)
      llvm::stable_sort(Loaded, [](const auto &Left, const auto &Right) {
        return Left.State < Right.State;
      });
    const auto Mask = [](const Intervals &Range, size_t Word) -> uint64_t {
      const size_t From = std::max(Range.first, Word * 64);
      const size_t To = std::min(Range.second, Word * 64 + 64);
      if (From >= To)
        return 0;
      const uint64_t Ones =
          To - From == 64 ? ~uint64_t(0) : (uint64_t(1) << (To - From)) - 1;
      return Ones << (From - Word * 64);
    };
    // Bits of one word of intervals live at each block start.
    std::vector<uint64_t> Words(BlockCount);
    // A refusal names a load that the write reaches: follow one of the
    // intervals forward from the write to a load of it.
    const auto Witness = [&](size_t Word, size_t Index, size_t State,
                             uint64_t Bits) -> const Access * {
      const uint64_t Bit = Bits & (~Bits + 1);
      std::vector<char> Reached(BlockCount, 0);
      std::vector<std::pair<size_t, size_t>> Pending{{Index, State}};
      while (!Pending.empty()) {
        const auto [Block, Start] = Pending.back();
        Pending.pop_back();
        const auto &Row = Table[Block];
        bool Alive = true;
        for (size_t At = Start; Alive && At <= Row.size(); ++At) {
          for (const Observation &Seen : Observations[Block])
            if (Seen.State == At && (Mask(Seen.Range, Word) & Bit))
              return Seen.Load;
          for (const Raise &Into : Raises[Block])
            if (Into.First <= At && At <= Into.Last && !Reached[Into.Handler]) {
              Reached[Into.Handler] = 1;
              Pending.emplace_back(Into.Handler, 0);
            }
          if (At < Row.size() && !Row[At].Call &&
              (Mask(Row[At].Range, Word) & Bit))
            Alive = false;
        }
        if (Alive)
          for (size_t Next : Successors[Block])
            if (!Reached[Next]) {
              Reached[Next] = 1;
              Pending.emplace_back(Next, 0);
            }
      }
      return nullptr;
    };
    const auto Observed = [&](const Access *Load) {
      return Load ? "load at 0x" + llvm::utohexstr(Load->Op->Addr) + " "
                  : std::string("a load ");
    };
    std::vector<char> Entered(BlockCount, 0);
    const auto Enter = [&](size_t Word, size_t Index) {
      const MedBlock &Block = Func.Blocks[Index];
      auto UnprovedEntry = [&] {
        const Access *Load = Witness(Word, Index, 0, Words[Index]);
        return Refuse("CALL return-slot observation" +
                      (Load ? " at 0x" + llvm::utohexstr(Load->Op->Addr)
                            : std::string()) +
                      " reaches an unproved exceptional entry at 0x" +
                      llvm::utohexstr(Block.StartAddr));
      };
      std::set<ExceptionalEdge> Declared;
      for (const ExceptionalEdge &Edge : Block.ExceptionalPreds) {
        if (!charge(Budget, Edge.contexts().size()))
          return Exhausted();
        Declared.insert(Edge);
      }
      if (Declared != ExceptionalIncoming[Block.Id])
        return UnprovedEntry();
      for (const ExceptionalEdge &Edge : Declared) {
        const auto *Pred = Blocks.first(Edge.BlockId);
        if (!Pred || Edge.TargetVA != Block.StartAddr)
          return UnprovedEntry();
        size_t Begin = 0, End = Pred->Ops.size();
        if (Edge.SourceVA != InvalidVA) {
          if (!charge(Budget, Pred->Ops.size()))
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
        // Include both sides of a throwing instruction: its CALL may push,
        // whereas its STORE need not complete. A protected range
        // conservatively includes every operation prefix in the block.
        if (!charge(Budget, 1))
          return Exhausted();
        Raises[IndexOf(Pred)].push_back(
            {Prefix(Pred->Id, Begin), Prefix(Pred->Id, End), Index});
        Protected[Index].push_back(IndexOf(Pred));
      }
      if (MissingPredecessor[Index])
        return Refuse("CALL return-slot observation has a missing predecessor");
      return true;
    };
    std::vector<size_t> Work;
    std::vector<char> Queued(BlockCount, 0);
    const auto Queue = [&](size_t Index) {
      if (!Queued[Index]) {
        Queued[Index] = 1;
        Work.push_back(Index);
      }
    };
    // The load a CALL write reached, which Defer recorded: it leaves the
    // observations once the pass that found it unwinds.
    const Access *Leaving = nullptr;
    // Recompute one block backward from its successors' starts, the handlers
    // its operation prefixes raise into and its own loads.
    const auto Pass = [&](size_t Word, size_t Index) {
      const auto &Row = Table[Index];
      const auto &Into = Raises[Index];
      const auto &Loaded = Observations[Index];
      if (!charge(Budget, (Row.size() + 1) * (Into.size() + 1) + Loaded.size() +
                              Successors[Index].size()))
        return Exhausted();
      // States are visited from the last down, as the loads are sorted.
      size_t Unread = Loaded.size();
      const auto Raised = [&](size_t State) {
        uint64_t Result = 0;
        for (; Unread && Loaded[Unread - 1].State == State; --Unread)
          Result |= Mask(Loaded[Unread - 1].Range, Word);
        for (const Raise &From : Into)
          if (From.First <= State && State <= From.Last)
            Result |= Words[From.Handler];
        return Result;
      };
      uint64_t Current = Raised(Row.size());
      for (size_t Successor : Successors[Index])
        Current |= Words[Successor];
      for (size_t State = Row.size(); State; --State) {
        const Write &Item = Row[State - 1];
        if (const uint64_t Hit = Current & Mask(Item.Range, Word)) {
          if (Item.Call) {
            if (ObservedCalls) {
              ObservedCalls->insert(Item.Op);
            } else {
              const Access *Load = Witness(Word, Index, State, Hit);
              const std::string Reason =
                  Observed(Load) +
                  "observes a physical CALL return slot from 0x" +
                  llvm::utohexstr(Item.Op->Addr) +
                  "; the ordinary C frame does not represent that write; "
                  "refusing a stale frame value";
              if (!Defer || !Load || Load->Op->Opcode != NdOp::LOAD)
                return Refuse(Reason);
              Defer->push_back(
                  {Load->Op->Addr, Load->Op->OriginSeq, Explain(Reason)});
              Leaving = Load;
              return false;
            }
          }
          Current &= ~Hit;
        }
        Current |= Raised(State - 1);
      }
      if (!(Current & ~Words[Index]))
        return true;
      Words[Index] |= Current;
      if (!Entered[Index]) {
        Entered[Index] = 1;
        if (!Enter(Word, Index))
          return false;
      }
      for (size_t Source : Predecessors[Index])
        Queue(Source);
      for (size_t Source : Protected[Index])
        Queue(Source);
      return true;
    };
    // Every load's intervals flow backward together, one word at a time, to a
    // fixed point; a write that a live interval meets refuses the projection.
    // A load Defer takes leaves, and its word reaches a fixed point again
    // without it. A settled word met no write, and fewer loads meet none.
    for (size_t Word = 0; Word * 64 < Everything.second;) {
      if (!charge(Budget, BlockCount))
        return Exhausted();
      llvm::fill(Words, 0);
      for (size_t Index = BlockCount; Index--;)
        for (const Observation &Seen : Observations[Index])
          if (Mask(Seen.Range, Word))
            Queue(Index);
      while (!Work.empty() && !Leaving) {
        const size_t Next = Work.back();
        Work.pop_back();
        Queued[Next] = 0;
        if (!Pass(Word, Next) && !Leaving)
          return false;
      }
      if (!Leaving) {
        ++Word;
        continue;
      }
      for (const size_t Index : Work)
        Queued[Index] = 0;
      Work.clear();
      llvm::erase_if(
          Observations[IndexOf(Blocks.first(Leaving->Block))],
          [&](const Observation &Seen) { return Seen.Load == Leaving; });
      Leaving = nullptr;
    }
    return true;
  };
  if (llvm::any_of(CallEnds, [](const auto &Entry) { return !Entry.second; })) {
    // A CALL without an exact slot may write any byte. When the loads stay
    // separated from every such write, no bound below can change the
    // answer, and those bounds cost far more than this proof. Its attempt
    // has its own allowance: a refusal or exhaustion leaves the bounded
    // proof its whole budget.
    size_t Allowance = Remaining;
    if (ObservedCalls) {
      const bool Complete = Observe(CallEnds, {}, Allowance, nullptr);
      const bool HasWrites = !ObservedCalls->empty();
      ObservedCalls->clear();
      if (Complete && !HasWrites)
        return true;
    } else if (Observe(CallEnds, {}, Allowance, nullptr))
      return true;
    Diagnostic.clear();
    // Only a PE32 registration frame reads the exact accesses by operation.
    const bool ByOperation = Image.Arch == Arch::X86 && FreshSource &&
                             FreshSource->RegistrationStates;
    std::map<const MedOp *, int64_t> Accesses;
    if (ByOperation)
      for (const Access &Load : Loads)
        Accesses.emplace(Load.Op, Load.Begin);
    for (size_t I = 0; ByOperation && I < Memory.size(); ++I)
      if (Memory[I]->Opcode == NdOp::STORE && Offsets[I] &&
          *Offsets[I] <=
              std::numeric_limits<int64_t>::max() - Memory[I]->Inputs[1].Size)
        Accesses.emplace(Memory[I], *Offsets[I]);
    const auto Registration = registrationCalls(
        Func, FreshSource, Image, StackPointers, Accesses, Remaining);
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
    const auto Bounds =
        entryStackOffsetRanges(Func, UnboundedStacks, Image.Arch,
                               Image.abiFormat(), Func.EntryKind, CallReturns);
    for (size_t I = 0; I < Bounds.size(); ++I) {
      CallEnds.at(UnboundedCalls[I]) = Bounds[I];
      if (!Bounds[I] && !CallReturns && NeedsCalleeFacts)
        *NeedsCalleeFacts = true;
    }
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
      // Callee facts already served the complete range query above. The
      // remaining values need a local one-sided proof; retrying the same
      // failed range with callee facts cannot establish native-width bounds.
      const auto Bounds = entryStackOffsetUpperBounds(
          Func, PendingStacks, Image.Arch, Image.abiFormat(), Func.EntryKind);
      for (size_t I = 0; I < Pending.size(); ++I)
        if (Bounds[I])
          CallBelow.emplace(Pending[I], *Bounds[I]);
    }
  }
  return Observe(CallEnds, CallBelow, Remaining, Deferred);
}

namespace {
bool overlapsRegister(const MedVar &View, uint64_t RegOff, uint16_t Size) {
  return View.Kind == MedVar::Reg && View.Size && Size &&
         (View.RegOff >= RegOff ? View.RegOff - RegOff < Size
                                : RegOff - View.RegOff < View.Size);
}

/// Whether a RETURN reads the \p Size bytes at \p RegOff: as part of the
/// settled result, or as a register the caller expects back. The integer
/// result register is always kept, as code generation keeps it for a void
/// source function. A second integer return register carries a result only
/// as a register pair, which a 64-bit target publishes as MultiReturn.
bool returnReads(const MedFunc &Func, const TargetRegInfo &TRI,
                 BinaryFormat Format, uint64_t RegOff, uint16_t Size) {
  MedVar View;
  View.Kind = MedVar::Reg;
  View.RegOff = RegOff;
  View.Size = Size;
  auto Holds = [&](uint64_t Register, uint16_t Bytes) {
    return overlapsRegister(View, Register, Bytes);
  };
  for (const MedReturnReg &Return : Func.MultiReturn)
    if (Holds(Return.RegOff, Return.Size ? Return.Size : TRI.PointerSize))
      return true;
  if (Holds(TRI.IntReturnReg, TRI.PointerSize) ||
      (TRI.FPReturnReg && Holds(TRI.FPReturnReg, 16)))
    return true;
  for (uint64_t Register : TRI.FPReturnRegs)
    if (Holds(Register, 16))
      return true;
  if (TRI.isCallPreserved(RegOff, Size, Format) ||
      (Func.CC == CallingConv::Win64 &&
       TRI.isCallPreserved(RegOff, Size, BinaryFormat::COFF)) ||
      TRI.isStackPointer(RegOff) || TRI.isFramePointer(RegOff) ||
      TRI.isLinkRegister(RegOff))
    return true;
  for (uint64_t Register : TRI.IntReturnRegs)
    if (Register != TRI.IntReturnReg && Holds(Register, TRI.PointerSize))
      return TRI.PointerSize < 8;
  return TRI.IntReturnReg2 && Holds(TRI.IntReturnReg2, TRI.PointerSize) &&
         TRI.PointerSize < 8;
}
} // namespace

bool validateObservedCallReturnReads(
    const MedFunc &Func, const BinaryImage &Image, std::string &Diagnostic,
    size_t WorkLimit, const std::map<va_t, unsigned> *CalleeIntegerArity) {
  Diagnostic.clear();
  if (Func.CallReturnReads.empty())
    return true;
  const auto &TRI = getTargetRegInfo(Image.Arch);
  const BinaryFormat Format = Image.abiFormat();
  const CallArgumentConvention *Convention =
      callArgumentConvention(Image.Arch, Format);
  const llvm::ArrayRef<uint64_t> IntegerArguments =
      TRI.integerParamRegs(Format);
  // The integer argument position of the register holding \p RegOff, or -1.
  auto IntegerPosition = [&](uint64_t RegOff, uint16_t Size) {
    MedVar View;
    View.Kind = MedVar::Reg;
    View.RegOff = RegOff;
    View.Size = Size;
    for (size_t K = 0; K < IntegerArguments.size(); ++K)
      if (overlapsRegister(View, IntegerArguments[K], TRI.PointerSize))
        return static_cast<int>(K);
    return -1;
  };
  // How many integer arguments direct call \p Call can pass, if bounded.
  auto CalleeArity = [&](const MedOp &Call) -> const unsigned * {
    if (!CalleeIntegerArity || Call.Opcode != NdOp::CALL ||
        Call.NumInputs < 1 || !Call.Inputs[0].isConst())
      return nullptr;
    const auto It = CalleeIntegerArity->find(Call.Inputs[0].ConstVal);
    return It == CalleeIntegerArity->end() ? nullptr : &It->second;
  };
  size_t Remaining = WorkLimit;
  using Key = std::pair<int, int>;
  auto KeyOf = [](const MedVar &V) { return Key{V.Id, V.SSAVer}; };
  auto Named = [](const MedVar &V) { return !V.isConst() && V.Id >= 0; };
  auto At = [](va_t Addr) { return "0x" + llvm::utohexstr(Addr); };
  auto RegisterName = [&](uint64_t RegOff, uint16_t Size) -> std::string {
    const char *Name = TRI.GetRegName ? TRI.GetRegName(RegOff, Size) : nullptr;
    return Name && Name[0] != '?' ? Name : "r0x" + llvm::utohexstr(RegOff);
  };
  // An operation by its block and position.
  struct Site {
    size_t Block = 0, Index = 0;
  };
  auto OpAt = [&](const Site &Where) -> const MedOp & {
    return Func.Blocks[Where.Block].Ops[Where.Index];
  };
  // A reader no operation names, described only for a diagnostic.
  struct Implicit {
    enum class Kind : uint8_t { ExceptionalEntry, Clobber, Argument } What;
    size_t Block = 0;
    const MedCallInfo *Call = nullptr;
  };
  // Every reader of each value; the recorded loads where they are.
  llvm::DenseMap<Key, llvm::SmallVector<Site, 2>> Readers;
  llvm::DenseMap<Key, llvm::SmallVector<const PhiNode *, 1>> PhiReaders;
  llvm::DenseMap<Key, Implicit> ImplicitReads;
  llvm::DenseMap<int, size_t> BlockIndex;
  llvm::DenseMap<std::pair<int, int>, const MedCallInfo *> CallInfos;
  std::vector<llvm::SmallVector<Site, 1>> Recorded(Func.CallReturnReads.size());
  bool Exhausted = false;
  auto Charge = [&](size_t Count) {
    Exhausted |= !charge(Remaining, Count);
    return !Exhausted;
  };
  // What observes the value, once the proof finds it observed.
  std::string Observer;
  auto Observe = [&](std::string Why) {
    Observer = std::move(Why);
    return true;
  };
  for (size_t B = 0; B < Func.Blocks.size(); ++B) {
    const MedBlock &Block = Func.Blocks[B];
    // An exhausted index still finds each recorded load, which it then
    // cannot clear.
    const bool Indexed = Charge(1 + Block.Ops.size() + Block.Phis.size());
    if (Indexed) {
      BlockIndex.try_emplace(Block.Id, B);
      for (const PhiNode &Phi : Block.Phis) {
        for (const auto &Incoming : Phi.Args)
          if (Named(Incoming.second))
            PhiReaders[KeyOf(Incoming.second)].push_back(&Phi);
        if (Phi.ExceptionalEntry && Named(*Phi.ExceptionalEntry))
          ImplicitReads.try_emplace(
              KeyOf(*Phi.ExceptionalEntry),
              Implicit{Implicit::Kind::ExceptionalEntry, B});
      }
    }
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const MedOp &Op = Block.Ops[I];
      if (Op.Dead)
        continue;
      if (Indexed)
        for (unsigned K = 0; K < Op.NumInputs; ++K)
          if (Named(Op.Inputs[K]))
            Readers[KeyOf(Op.Inputs[K])].push_back({B, I});
      if (Op.Opcode == NdOp::LOAD)
        for (size_t R = 0; R < Func.CallReturnReads.size(); ++R)
          if (Op.Addr == Func.CallReturnReads[R].Load &&
              Op.OriginSeq == Func.CallReturnReads[R].OriginSeq)
            Recorded[R].push_back({B, I});
    }
  }
  for (const MedCallClobber &Clobber : Func.CallClobbers)
    if (Named(Clobber.PreservedInput))
      ImplicitReads.try_emplace(KeyOf(Clobber.PreservedInput),
                                Implicit{Implicit::Kind::Clobber});
  for (const MedCallInfo &Info : Func.CallInfos) {
    CallInfos.try_emplace(std::pair{Info.BlockId, Info.OpIdx}, &Info);
    for (const MedVar &Argument : Info.Args)
      if (Named(Argument))
        ImplicitReads.try_emplace(KeyOf(Argument),
                                  Implicit{Implicit::Kind::Argument, 0, &Info});
  }
  auto Describe = [&](const Implicit &Read) -> std::string {
    switch (Read.What) {
    case Implicit::Kind::ExceptionalEntry:
      return "an exceptional entry into " +
             At(Func.Blocks[Read.Block].StartAddr) + " receives the value";
    case Implicit::Kind::Clobber:
      return "a call clobber keeps the value as a register's preserved bytes";
    case Implicit::Kind::Argument:
      break;
    }
    const auto Block = BlockIndex.find(Read.Call->BlockId);
    if (Block == BlockIndex.end() || Read.Call->OpIdx < 0 ||
        static_cast<size_t>(Read.Call->OpIdx) >=
            Func.Blocks[Block->second].Ops.size())
      return "a settled call takes the value as an argument";
    return "the call at " +
           At(Func.Blocks[Block->second].Ops[Read.Call->OpIdx].Addr) +
           " takes the value as an argument";
  };

  // A register holding the value may be read where no operation names it:
  // a call whose arguments were not settled, a RETURN that reads it, or an
  // exit to code outside the function. Follow every path until a complete
  // overwrite or a clobbering call ends the register's copy of the value.
  llvm::DenseSet<size_t> Entered;
  auto HeldObserved = [&](const MedVar &Value, const Site &Definition) {
    const uint64_t RegOff = Value.RegOff;
    const uint16_t Size = Value.Size;
    // Another variable over these bytes may view the value. The register's
    // own variable names it only in its own version, a reader the closure
    // judges; any other version is a different value.
    auto Overlaps = [&](const MedVar &Other) {
      return Other.Id != Value.Id && overlapsRegister(Other, RegOff, Size);
    };
    auto Held = [&] { return RegisterName(RegOff, Size); };
    auto Over = [&](const MedVar &Other) {
      return RegisterName(Other.RegOff, Other.Size) + " over " + Held() +
             ", which holds the value";
    };
    const int Slot = IntegerPosition(RegOff, Size);
    const bool Argument = TRI.isParamReg(RegOff);
    const bool Returned = returnReads(Func, TRI, Format, RegOff, Size);
    std::vector<Site> Work{{Definition.Block, Definition.Index + 1}};
    Entered.clear();
    while (!Work.empty()) {
      const Site From = Work.back();
      Work.pop_back();
      const MedBlock &Block = Func.Blocks[From.Block];
      // An exception can leave before any later overwrite, and a handler can
      // see the registers it left with.
      if (!Block.ExceptionalSuccs.empty())
        return Observe("an exception may leave " + At(Block.StartAddr) +
                       " with the value in " + Held());
      bool Ends = false;
      for (size_t I = From.Index; I < Block.Ops.size() && !Ends; ++I) {
        if (!Charge(1))
          return true;
        const MedOp &Op = Block.Ops[I];
        if (Op.Dead)
          continue;
        for (unsigned K = 0; K < Op.NumInputs; ++K)
          if (Overlaps(Op.Inputs[K]))
            return Observe("the operation at " + At(Op.Addr) + " reads " +
                           Over(Op.Inputs[K]));
        if (Op.Opcode == NdOp::RETURN) {
          if (Returned)
            return Observe("the RETURN at " + At(Op.Addr) +
                           " reads the value in " + Held());
          Ends = true;
          continue;
        }
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
          // A call's settled arguments are its call record's, where call ABI
          // recovery made one, or else the operands a summarized callee's
          // reads published at conversion (checked above with every input).
          const auto Info = CallInfos.find({Block.Id, static_cast<int>(I)});
          const bool Published =
              Convention && Convention->RegisterArgumentsFromCalleeSummary &&
              Op.CalleeRegisterArgs >= 0;
          if (Info == CallInfos.end()) {
            // A direct call to a function of this run passes at most its
            // emitted integer parameters; a register past them is none.
            const unsigned *Arity =
                Argument && !Published ? CalleeArity(Op) : nullptr;
            if (Argument && !Published && (!Arity || Slot < 0))
              return Observe("the call at " + At(Op.Addr) +
                             ", whose arguments are not settled, may read "
                             "the value in " +
                             Held());
            if (Arity && Slot >= 0 && static_cast<unsigned>(Slot) < *Arity)
              return Observe("the call at " + At(Op.Addr) + " passes " +
                             std::to_string(*Arity) +
                             " integer arguments, the value in " + Held() +
                             " among them");
          } else if (const auto Passed =
                         llvm::find_if(Info->second->Args, Overlaps);
                     Passed != Info->second->Args.end()) {
            return Observe("the call at " + At(Op.Addr) + " takes " +
                           Over(*Passed));
          }
          if (Op.DoesNotReturn ||
              callClobbersRegister(Op, RegOff, Size, TRI, Format)) {
            Ends = true;
            continue;
          }
        }
        if (Op.Output.Kind == MedVar::Reg && Op.Output.RegOff == RegOff &&
            Op.Output.Size >= Size)
          Ends = true;
      }
      if (Ends)
        continue;
      // An exit without a RETURN, such as a tail jump, keeps its own
      // register contract.
      if (Block.Succs.empty() && (Argument || Returned))
        return Observe(
            "the exit at " +
            At(Block.Ops.empty() ? Block.StartAddr : Block.Ops.back().Addr) +
            " leaves the value in " + Held());
      for (int Succ : Block.Succs) {
        const auto Next = BlockIndex.find(Succ);
        if (Next == BlockIndex.end())
          return Observe("a successor of " + At(Block.StartAddr) +
                         " is missing, so the value cannot be followed in " +
                         Held());
        if (Entered.insert(Next->second).second)
          Work.push_back({Next->second, 0});
      }
    }
    return false;
  };

  // The loaded value and every copy, view or join of it. A copy, or a view
  // of some of its bytes, only moves the value; any other reader uses it.
  llvm::DenseSet<Key> Seen;
  auto Observed = [&](const Site &Load) {
    std::vector<std::pair<MedVar, std::optional<Site>>> Pending{
        {OpAt(Load).Output, Load}};
    Seen.clear();
    while (!Pending.empty()) {
      if (!Charge(1))
        return true;
      const auto [Value, Definition] = Pending.back();
      Pending.pop_back();
      if (!Named(Value) || !Seen.insert(KeyOf(Value)).second)
        continue;
      if (const auto It = ImplicitReads.find(KeyOf(Value));
          It != ImplicitReads.end())
        return Observe(Describe(It->second));
      if (const auto It = Readers.find(KeyOf(Value)); It != Readers.end())
        for (const Site &Where : It->second) {
          const MedOp &Reader = OpAt(Where);
          const bool Moves = Reader.Opcode == NdOp::COPY
                                 ? Reader.NumInputs == 1
                                 : Reader.Opcode == NdOp::SUBBYTES &&
                                       Reader.NumInputs == 2 &&
                                       Reader.Inputs[1].isConst();
          if (!Moves)
            return Observe(Reader.Opcode == NdOp::RETURN
                               ? "the RETURN at " + At(Reader.Addr) +
                                     " returns the value"
                               : "the operation at " + At(Reader.Addr) +
                                     " uses the value");
          Pending.emplace_back(Reader.Output, Where);
        }
      if (const auto It = PhiReaders.find(KeyOf(Value)); It != PhiReaders.end())
        for (const PhiNode *Phi : It->second)
          Pending.emplace_back(Phi->Output, std::nullopt);
      // A join's register is covered by the walk from the definition that
      // reaches it.
      if (Value.Kind == MedVar::Reg && Definition &&
          HeldObserved(Value, *Definition))
        return true;
    }
    return false;
  };

  for (size_t R = 0; R < Func.CallReturnReads.size(); ++R)
    for (const Site &Load : Recorded[R]) {
      if (!Exhausted && !Observed(Load))
        continue;
      Diagnostic = Func.CallReturnReads[R].Diagnostic + "; " +
                   (Exhausted ? "the settled observation proof exceeded "
                                "its work budget"
                              : Observer);
      return false;
    }
  return true;
}

bool validateSourceCallReturnReads(const MedFunc &Func,
                                   const BinaryImage &Image,
                                   std::string &Diagnostic, size_t WorkLimit,
                                   const LowFunc *FreshSource,
                                   const MedCallReturnRanges *CallReturns,
                                   std::vector<MedCallReturnRead> *Deferred,
                                   bool *NeedsCalleeFacts) {
  size_t Remaining = WorkLimit;
  return checkSourceCallReturnReads(Func, Image, Diagnostic, Remaining,
                                    FreshSource, nullptr, CallReturns, Deferred,
                                    NeedsCalleeFacts);
}

bool preserveSourceCallReturnWrites(MedFunc &Func, const BinaryImage &Image,
                                    std::string &Diagnostic, size_t WorkLimit,
                                    const LowFunc *FreshSource,
                                    const MedCallReturnRanges *CallReturns,
                                    std::vector<MedCallReturnRead> *Deferred,
                                    bool *NeedsCalleeFacts) {
  std::set<const MedOp *> Observed;
  size_t Remaining = WorkLimit;
  if (!checkSourceCallReturnReads(Func, Image, Diagnostic, Remaining,
                                  FreshSource, &Observed, CallReturns, nullptr,
                                  NeedsCalleeFacts))
    return false;
  if (Observed.empty())
    return true;
  const auto Refuse = [&] {
    return validateSourceCallReturnReads(Func, Image, Diagnostic, WorkLimit,
                                         FreshSource, CallReturns, Deferred,
                                         NeedsCalleeFacts);
  };
  if (!Func.CallInfos.empty())
    return Refuse();
  const auto Stacks = sourceCallStackPointers(Func, Image.Arch, Remaining);
  std::vector<MedVar> Values;
  for (const MedOp *Call : Observed) {
    const auto Found = Stacks.find(Call);
    if (Found == Stacks.end())
      return Refuse();
    Values.push_back(Found->second);
  }
  if (!charge(Remaining, Observed.size()))
    return Refuse();
  const auto Offsets = entryStackOffsets(Func, Values, Image.Arch,
                                         Image.abiFormat(), Func.EntryKind);
  Decoder Decode;
  if (!Decode.init(Image))
    return Refuse();
  struct Write {
    MedVar Stack;
    va_t PC;
  };
  std::map<const MedOp *, Write> Writes;
  const auto Width = getTargetRegInfo(Image.Arch).PointerSize;
  int64_t FrameSize = Func.FrameSize;
  size_t Index = 0;
  for (const MedOp *Call : Observed) {
    const auto Offset = Offsets[Index++];
    if (!Offset || *Offset > 0 ||
        *Offset < -limits::ir::kSourceStackFrameMaxFrame + Width)
      return Refuse();
    const auto PC = callFallthrough(Image, Call->Addr, Decode);
    if (!PC)
      return Refuse();
    Writes.emplace(Call, Write{Stacks.at(Call), *PC});
    FrameSize = std::max(FrameSize, Width - *Offset);
  }
  int MaximumId = 0;
  const auto Visit = [&](const MedVar &V) {
    if (!V.isConst())
      MaximumId = std::max(MaximumId, V.Id);
  };
  for (const MedVar &V : Func.Params)
    Visit(V);
  for (const MedVar &V : Func.Locals)
    Visit(V);
  for (const MedCallClobber &Clobber : Func.CallClobbers) {
    Visit(Clobber.Value);
    Visit(Clobber.PreservedInput);
  }
  for (const MedBlock &Block : Func.Blocks) {
    if (!charge(Remaining, Block.Ops.size() + Block.Phis.size()))
      return Refuse();
    for (const PhiNode &Phi : Block.Phis) {
      Visit(Phi.Output);
      for (const auto &[Pred, V] : Phi.Args)
        Visit(V);
      if (Phi.ExceptionalEntry)
        Visit(*Phi.ExceptionalEntry);
    }
    for (const MedOp &Op : Block.Ops) {
      Visit(Op.Output);
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        Visit(Op.Inputs[I]);
    }
  }
  if (Observed.size() > size_t(std::numeric_limits<int>::max() - MaximumId))
    return Refuse();
  // All proof and budget failures precede mutation. Each inserted address is
  // the current SSA stack value; the real target load still precedes the push.
  for (MedBlock &Block : Func.Blocks) {
    std::vector<MedOp> Ops;
    for (const MedOp &Op : Block.Ops) {
      if (const auto Found = Writes.find(&Op); Found != Writes.end()) {
        MedVar Address;
        Address.Kind = MedVar::Temp;
        Address.TheArch = Image.Arch;
        Address.Id = ++MaximumId;
        Address.Size = Width;
        MedOp Subtract;
        Subtract.Opcode = NdOp::INT_SUB;
        Subtract.Output = Address;
        Subtract.Addr = Op.Addr;
        Subtract.addInput(Found->second.Stack);
        Subtract.addInput(MedVar::makeConst(Width, Width));
        Ops.push_back(std::move(Subtract));
        MedOp Store;
        Store.Opcode = NdOp::STORE;
        Store.Addr = Op.Addr;
        Store.addInput(Address);
        Store.addInput(MedVar::makeConst(Found->second.PC, Width,
                                         ConstantAddressProvenance::Scalar));
        Ops.push_back(std::move(Store));
      }
      Ops.push_back(Op);
    }
    Block.Ops = std::move(Ops);
  }
  Func.FrameSize = FrameSize;
  return true;
}
} // namespace neverd
