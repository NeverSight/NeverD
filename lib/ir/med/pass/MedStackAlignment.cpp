//===- MedStackAlignment.cpp - Proven entry-stack alignment -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Entry-stack offset proofs and alignment rewrites.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedStackAlignment.h"

#include "../../../safety/StackSlotFlow.h"
#include "MedStackRecurrence.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedProvenStackOffsets.h"
#include "neverd/ir/med/X86/X86RegistrationFrame.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/Support/SaveAndRestore.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {
namespace {

using Key = std::tuple<MedVar::VarKind, int, int>;

Key key(const MedVar &V) { return {V.Kind, V.Id, V.SSAVer}; }

bool sameOccurrence(const MedVar &A, const MedVar &B) {
  return key(A) == key(B) && A.Size == B.Size && A.TheArch == B.TheArch &&
         A.RenameTag == B.RenameTag &&
         (A.Kind != MedVar::Reg || A.RegOff == B.RegOff);
}

struct AlignmentResult {
  unsigned BaseIndex = 0;
  uint64_t Remainder = 0;
  int64_t Offset = 0;
};

struct AlignmentMask {
  unsigned BaseIndex = 0;
  uint64_t Alignment = 0;
};

std::optional<AlignmentMask> alignmentMask(const MedOp &Op,
                                           unsigned PointerSize) {
  if ((PointerSize != 4 && PointerSize != 8) || Op.Opcode != NdOp::INT_AND ||
      Op.NumInputs != 2 || Op.Output.Size != PointerSize ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Op.IntrinsicOutputs.empty())
    return std::nullopt;
  for (unsigned I = 0; I != 2; ++I) {
    const MedVar &Base = Op.Inputs[I];
    const MedVar &Mask = Op.Inputs[I ^ 1u];
    if (Base.isConst() || Base.Size != PointerSize || !Mask.isConst() ||
        Mask.Size != PointerSize || isAddressProvenance(Mask.Provenance))
      continue;
    const uint64_t WidthMask =
        PointerSize == 4 ? uint64_t{UINT32_MAX} : UINT64_MAX;
    if (Mask.ConstVal == 0 || (Mask.ConstVal & ~WidthMask))
      continue;
    const uint64_t Cleared = ~Mask.ConstVal & WidthMask;
    const uint64_t Alignment = Cleared + 1;
    if (Cleared && Alignment && (Alignment & (Alignment - 1)) == 0)
      return AlignmentMask{I, Alignment};
  }
  return std::nullopt;
}

unsigned definitionCapacity(const MedFunc &Func) {
  unsigned Count = 0;
  for (const MedBlock &Block : Func.Blocks) {
    Count += Block.Phis.size();
    for (const MedOp &Op : Block.Ops)
      if (!Op.Output.isConst() && Op.Output.Size != 0)
        ++Count;
  }
  return Count;
}

class StackOffsetProof {
  struct CompletedProof {
    MedVar Occurrence;
    int64_t Offset;
    size_t Steps;
    unsigned Depth;
  };

  struct ValueDefinition {
    const MedOp *Op = nullptr;
    const PhiNode *Join = nullptr;
    bool Ambiguous = false;
    size_t ActiveContext = 0;
    bool EntryChecked = false;
    bool EntryLiveIn = false;
    std::optional<int64_t> ProvenOffset;
    std::optional<EntryStackOffsetRange> ProvenRange;
  };

public:
  /// \p FollowJoins also takes a join whose incoming offsets agree, and an
  /// entry stack pointer no operation defines, which SSA makes the incoming
  /// value.  The alignment rewrite keeps the stricter proof.
  StackOffsetProof(const MedFunc &Func, Arch Architecture, BinaryFormat Format,
                   StackEntryKind Entry, bool FollowJoins = false,
                   llvm::ArrayRef<ProvenStackOffset> Seeds = {},
                   bool OnlySeededRoots = false,
                   llvm::ArrayRef<ProvenStackOffsetRange> RangeSeeds = {})
      : Func(Func), TRI(getTargetRegInfo(Architecture)),
        Architecture(Architecture), Format(Format), Entry(Entry),
        FollowJoins(FollowJoins), OnlySeededRoots(OnlySeededRoots),
        Definitions(definitionCapacity(Func)) {
    for (const MedBlock &Block : Func.Blocks) {
      for (const PhiNode &Phi : Block.Phis) {
        auto &Definition = Definitions[key(Phi.Output)];
        if (!FollowJoins || Definition.Join)
          Definition.Ambiguous = true;
        else
          Definition.Join = &Phi;
      }
      for (const MedOp &Op : Block.Ops) {
        if (Op.Output.isConst() || Op.Output.Size == 0)
          continue;
        auto &Definition = Definitions[key(Op.Output)];
        if (Definition.Op || Definition.Join)
          Definition.Ambiguous = true;
        if (!Definition.Op)
          Definition.Op = &Op;
      }
    }
    for (const auto &Seed : Seeds) {
      if (!Seed.Definition)
        continue;
      const auto Found = Definitions.find(key(Seed.Definition->Output));
      if (Found == Definitions.end() || Found->second.Ambiguous ||
          Found->second.Op != Seed.Definition || Seed.Definition->Dead ||
          Seed.Definition->Output.Size != TRI.PointerSize ||
          !fitsPointerOffset(Seed.Offset))
        continue;
      auto &Definition = Found->second;
      if (Definition.ProvenOffset && Definition.ProvenOffset != Seed.Offset)
        Definition.Ambiguous = true;
      else
        Definition.ProvenOffset = Seed.Offset;
    }
    for (const auto &Seed : RangeSeeds) {
      if (bool(Seed.Definition) == bool(Seed.Join) ||
          Seed.Range.Min > Seed.Range.Max ||
          !fitsPointerOffset(Seed.Range.Min) ||
          !fitsPointerOffset(Seed.Range.Max))
        continue;
      const MedVar &Output =
          Seed.Definition ? Seed.Definition->Output : Seed.Join->Output;
      const auto Found = Definitions.find(key(Output));
      if (Found == Definitions.end() || Found->second.Ambiguous ||
          Output.Size != TRI.PointerSize ||
          (Seed.Definition &&
           (Found->second.Op != Seed.Definition || Seed.Definition->Dead)) ||
          (Seed.Join && Found->second.Join != Seed.Join))
        continue;
      auto &Definition = Found->second;
      if (Definition.ProvenRange &&
          (Definition.ProvenRange->Min != Seed.Range.Min ||
           Definition.ProvenRange->Max != Seed.Range.Max))
        Definition.Ambiguous = true;
      else
        Definition.ProvenRange = Seed.Range;
    }
  }

  std::optional<AlignmentResult> alignment(const MedOp &Op,
                                           unsigned Depth = 0) {
    if (Depth == 0)
      Steps = 0;
    const auto Mask = alignmentMask(Op, TRI.PointerSize);
    if (!Mask || Mask->Alignment >
                     guaranteedEntryStackAlignment(Architecture, Format, Entry))
      return std::nullopt;
    auto Offset = resolve(Op.Inputs[Mask->BaseIndex], Depth + 1);
    if (!Offset)
      return std::nullopt;
    const uint64_t Remainder =
        (uint64_t(*Offset) +
         syntheticEntryStackResidue(Architecture, Format, Entry)) &
        (Mask->Alignment - 1);
    auto Aligned = safety::detail::checkedStackOffset(
        *Offset, static_cast<int64_t>(Remainder), true);
    if (!Aligned || !fitsPointerOffset(*Aligned))
      return std::nullopt;
    return AlignmentResult{Mask->BaseIndex, Remainder, *Aligned};
  }

  bool realignmentRoot(const MedOp &Op,
                       const std::set<const MedOp *> &EarlierEntryOps) {
    const auto Mask = alignmentMask(Op, TRI.PointerSize);
    if (!Mask || Op.Output.Kind != MedVar::Reg ||
        Op.Output.TheArch != Architecture ||
        Op.Output.RegOff != TRI.StackPointer ||
        Op.RegistrationRoot != MedOp::RegistrationRootKind::None)
      return false;
    const auto Definition = Definitions.find(key(Op.Output));
    if (Definition == Definitions.end() || Definition->second.Ambiguous ||
        Definition->second.Op != &Op)
      return false;
    AllowedDefinitions = &EarlierEntryOps;
    return offset(Op.Inputs[Mask->BaseIndex]).has_value();
  }

  /// The offset of \p Value from the authenticated entry stack pointer.
  std::optional<int64_t> offset(const MedVar &Value) {
    Steps = 0;
    return resolve(Value, 0);
  }

  std::optional<EntryStackOffsetRange> range(const MedVar &Value) {
    Steps = 0;
    return resolveRange(Value, 0);
  }

  /// The largest offset \p Value can have: the longest way to it from exact
  /// offsets through copies, displacements, alignment masks and joins. A
  /// dynamic allocation in a loop has such a bound without a lower one. A
  /// cycle that can raise the offset, or an input with no way from an exact
  /// offset, leaves no bound.
  std::optional<int64_t> upperBound(const MedVar &Value) {
    Steps = 0;
    struct Edge {
      Key From;
      int64_t Delta;
    };
    struct Node {
      std::optional<int64_t> Bound;
      std::vector<Edge> In;
      bool Exact = false;
    };
    std::map<Key, Node> Nodes;
    std::vector<MedVar> Work{Value};
    while (!Work.empty()) {
      const MedVar Current = Work.back();
      Work.pop_back();
      if (++Steps > kMaxProofSteps || Current.Size != TRI.PointerSize ||
          (Current.Kind != MedVar::Reg && Current.Kind != MedVar::Temp))
        return std::nullopt;
      const Key K = key(Current);
      if (Nodes.count(K))
        continue;
      Node &N = Nodes[K];
      const auto Found = Definitions.find(K);
      ValueDefinition *Definition =
          Found == Definitions.end() ? nullptr : &Found->second;
      if (Definition && Definition->Ambiguous)
        return std::nullopt;
      // Only the entry stack pointer and proven seeds have an offset of their
      // own. Every other value is reached through the graph, so a join many
      // ways share is visited once instead of proved again for each way in.
      if (!OnlySeededRoots && Current.Kind == MedVar::Reg &&
          Current.RegOff == TRI.StackPointer &&
          (authenticatedEntryLiveIn(Current, Definition) ||
           (FollowJoins && Current.SSAVer == 0 &&
            (!Definition || !Definition->Op)))) {
        N.Bound = 0;
        N.Exact = true;
        continue;
      }
      if (!Definition)
        return std::nullopt;
      if (Definition->ProvenOffset && Definition->Op &&
          sameOccurrence(Definition->Op->Output, Current)) {
        N.Bound = Definition->ProvenOffset;
        N.Exact = true;
        continue;
      }
      if (Definition->ProvenRange) {
        const MedVar &Output =
            Definition->Op ? Definition->Op->Output : Definition->Join->Output;
        if (!sameOccurrence(Output, Current))
          return std::nullopt;
        N.Bound = Definition->ProvenRange->Max;
        N.Exact = true;
        continue;
      }
      const auto Follow = [&](const MedVar &From, int64_t Delta) {
        N.In.push_back({key(From), Delta});
        Work.push_back(From);
      };
      if (Definition->Join) {
        const PhiNode &Phi = *Definition->Join;
        if (!sameOccurrence(Phi.Output, Current) || Phi.Args.empty() ||
            Phi.ExceptionalEntry)
          return std::nullopt;
        for (const auto &[Pred, Arg] : Phi.Args)
          Follow(Arg, 0);
        continue;
      }
      const MedOp *Op = Definition->Op;
      if (!Op || !sameOccurrence(Op->Output, Current) || Op->Dead ||
          Op->MemoryOrdering != NdMemoryOrdering::None ||
          Op->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          !Op->IntrinsicOutputs.empty())
        return std::nullopt;
      // A registration root names its own frame coordinate.
      if (Op->RegistrationRoot != MedOp::RegistrationRootKind::None) {
        const auto Exact = resolve(Current, 0);
        if (!Exact)
          return std::nullopt;
        N.Bound = *Exact;
        N.Exact = true;
        continue;
      }
      if (Op->Opcode == NdOp::COPY && Op->NumInputs == 1) {
        Follow(Op->Inputs[0], 0);
        continue;
      }
      // Clearing low bits only lowers the address.
      if (const auto Mask = alignmentMask(*Op, TRI.PointerSize)) {
        Follow(Op->Inputs[Mask->BaseIndex], 0);
        continue;
      }
      if ((Op->Opcode != NdOp::INT_ADD && Op->Opcode != NdOp::INT_SUB) ||
          Op->NumInputs != 2)
        return std::nullopt;
      const bool Subtract = Op->Opcode == NdOp::INT_SUB;
      const unsigned BaseIndex = !Subtract && Op->Inputs[0].isConst() ? 1 : 0;
      const MedVar &Delta = Op->Inputs[1 - BaseIndex];
      if (Delta.Size != TRI.PointerSize)
        return std::nullopt;
      const auto Displacement = scalarRange(Delta, 1);
      if (Steps > kMaxProofSteps ||
          Displacement.getUnsignedMax().ugt(INT64_MAX))
        return std::nullopt;
      const int64_t Raise =
          Subtract ? -int64_t(Displacement.getUnsignedMin().getZExtValue())
                   : int64_t(Displacement.getUnsignedMax().getZExtValue());
      Follow(Op->Inputs[BaseIndex], Raise);
    }
    // Relax every way in until nothing rises. Without a cycle that raises the
    // offset this settles within one round per value.
    bool Changed = true;
    for (size_t Round = 0; Changed; ++Round) {
      if (Round > Nodes.size())
        return std::nullopt;
      Changed = false;
      for (auto &[K, N] : Nodes) {
        if (N.Exact)
          continue;
        for (const Edge &E : N.In) {
          if (++Steps > kMaxProofSteps)
            return std::nullopt;
          const auto From = Nodes.find(E.From);
          if (From == Nodes.end() || !From->second.Bound)
            continue;
          const auto Raised = safety::detail::checkedStackOffset(
              *From->second.Bound, E.Delta, false);
          if (!Raised || !fitsPointerOffset(*Raised))
            return std::nullopt;
          if (!N.Bound || *Raised > *N.Bound) {
            N.Bound = Raised;
            Changed = true;
          }
        }
      }
    }
    for (const auto &[K, N] : Nodes)
      for (const Edge &E : N.In)
        if (!Nodes.at(E.From).Bound)
          return std::nullopt;
    return Nodes.at(key(Value)).Bound;
  }

private:
  const std::optional<CountedStackRecurrence> &recurrence(const PhiNode &Phi) {
    const auto K = key(Phi.Output);
    const auto [It, New] = Recurrences.emplace(K, std::nullopt);
    if (New && Steps < kMaxProofSteps) {
      size_t Remaining = kMaxProofSteps - Steps;
      It->second = countedStackRecurrence(Func, Phi, Remaining);
      Steps = kMaxProofSteps - Remaining;
    }
    return It->second;
  }
  std::map<Key, std::optional<CountedStackRecurrence>> Recurrences;

  /// Scalar bounds retain machine-width wrapping. Unknown scalar values have
  /// their full bit range; only a narrower extension or mask can bound them.
  llvm::ConstantRange scalarRange(const MedVar &Value, unsigned Depth) {
    const unsigned Bits = Value.Size * 8;
    llvm::ConstantRange Full(Bits, true);
    if (++Steps > kMaxProofSteps ||
        Depth > limits::ir::kMedStackAlignmentResolveDepthLimit)
      return Full;
    if (Value.isConst())
      return llvm::ConstantRange(llvm::APInt(Bits, Value.ConstVal));
    const auto Found = Definitions.find(key(Value));
    if (Found == Definitions.end() || Found->second.Ambiguous ||
        !RangeActive.insert(key(Value)).second)
      return Full;
    const auto Clear = llvm::scope_exit([&] { RangeActive.erase(key(Value)); });
    const auto &Definition = Found->second;
    if (Definition.Join) {
      const PhiNode &Phi = *Definition.Join;
      if (!sameOccurrence(Phi.Output, Value) || Phi.Args.empty() ||
          Phi.ExceptionalEntry)
        return Full;
      llvm::ConstantRange Result(Bits, false);
      for (const auto &[Pred, Arg] : Phi.Args) {
        if (Arg.Size != Value.Size)
          return Full;
        Result = Result.unionWith(scalarRange(Arg, Depth + 1));
      }
      return Result;
    }
    const MedOp *Op = Definition.Op;
    if (!Op || !sameOccurrence(Op->Output, Value) || Op->Dead ||
        Op->MemoryOrdering != NdMemoryOrdering::None ||
        Op->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !Op->IntrinsicOutputs.empty())
      return Full;
    if (Op->NumInputs == 1 && Op->Inputs[0].Size &&
        Op->Inputs[0].Size <= Value.Size) {
      if (Op->Opcode == NdOp::COPY && Op->Inputs[0].Size == Value.Size)
        return scalarRange(Op->Inputs[0], Depth + 1);
      if (Op->Opcode == NdOp::INT_ZEXT)
        return scalarRange(Op->Inputs[0], Depth + 1).zeroExtend(Bits);
    }
    if (Op->NumInputs == 2 && Op->Inputs[0].Size == Value.Size &&
        Op->Inputs[1].Size == Value.Size) {
      const auto Left = scalarRange(Op->Inputs[0], Depth + 1);
      const auto Right = scalarRange(Op->Inputs[1], Depth + 1);
      switch (Op->Opcode) {
      case NdOp::INT_ADD:
        return Left.add(Right);
      case NdOp::INT_SUB:
        return Left.sub(Right);
      case NdOp::INT_MULT:
        return Left.multiply(Right);
      case NdOp::INT_AND:
        return Left.binaryAnd(Right);
      case NdOp::INT_LEFT:
        // LLVM's range operation models oversized shifts as poison, while
        // machine-value proofs cannot derive a bound from that assumption.
        return Right.getUnsignedMax().ult(Bits) ? Left.shl(Right) : Full;
      default:
        break;
      }
    }
    return Full;
  }

  std::optional<EntryStackOffsetRange> resolveRange(const MedVar &Value,
                                                    unsigned Depth) {
    if (++Steps > kMaxProofSteps ||
        Depth > limits::ir::kMedStackAlignmentResolveDepthLimit ||
        Value.Size != TRI.PointerSize ||
        (Value.Kind != MedVar::Reg && Value.Kind != MedVar::Temp))
      return std::nullopt;
    const auto Found = Definitions.find(key(Value));
    if (Found != Definitions.end() && !Found->second.Ambiguous &&
        Found->second.ProvenRange) {
      const auto &Definition = Found->second;
      const MedVar &Output =
          Definition.Op ? Definition.Op->Output : Definition.Join->Output;
      if (sameOccurrence(Output, Value))
        return Definition.ProvenRange;
    }
    if (const auto Exact = resolve(Value, Depth))
      return EntryStackOffsetRange{*Exact, *Exact};
    if (Found == Definitions.end() || Found->second.Ambiguous ||
        !RangeActive.insert(key(Value)).second)
      return std::nullopt;
    const auto Clear = llvm::scope_exit([&] { RangeActive.erase(key(Value)); });
    const auto &Definition = Found->second;
    if (Definition.Join) {
      if (!sameOccurrence(Definition.Join->Output, Value))
        return std::nullopt;
      if (const auto &Loop = recurrence(*Definition.Join)) {
        const auto Base = resolveRange(Loop->Base, Depth + 1);
        if (Base) {
          const auto Lower = safety::detail::checkedStackOffset(
              Base->Min, Loop->MinimumChange, false);
          const auto Upper = safety::detail::checkedStackOffset(
              Base->Max, Loop->MaximumChange, false);
          if (Lower && Upper && fitsPointerOffset(*Lower) &&
              fitsPointerOffset(*Upper))
            return EntryStackOffsetRange{*Lower, *Upper};
        }
        return std::nullopt;
      }
      bool HasCycle = false;
      const auto Boundaries =
          phiComponentBoundaries(Value, Depth, HasCycle, /*RangeQuery=*/true);
      if (!Boundaries)
        return std::nullopt;
      std::optional<EntryStackOffsetRange> Result;
      for (const auto &[Boundary, AtDepth] : *Boundaries) {
        const auto Incoming = resolveRange(Boundary, AtDepth);
        if (!Incoming)
          return std::nullopt;
        if (!Result)
          Result = Incoming;
        else {
          Result->Min = std::min(Result->Min, Incoming->Min);
          Result->Max = std::max(Result->Max, Incoming->Max);
        }
      }
      return Result;
    }
    const MedOp *Op = Definition.Op;
    if (!Op || !sameOccurrence(Op->Output, Value) || Op->Dead ||
        Op->MemoryOrdering != NdMemoryOrdering::None ||
        Op->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        !Op->IntrinsicOutputs.empty())
      return std::nullopt;
    if (Op->Opcode == NdOp::COPY && Op->NumInputs == 1)
      return resolveRange(Op->Inputs[0], Depth + 1);
    if ((Op->Opcode != NdOp::INT_ADD && Op->Opcode != NdOp::INT_SUB) ||
        Op->NumInputs != 2)
      return std::nullopt;
    const bool Subtract = Op->Opcode == NdOp::INT_SUB;
    const unsigned BaseIndex = !Subtract && Op->Inputs[0].isConst() ? 1 : 0;
    const auto Base = resolveRange(Op->Inputs[BaseIndex], Depth + 1);
    const MedVar &Delta = Op->Inputs[1 - BaseIndex];
    if (!Base || Delta.Size != TRI.PointerSize)
      return std::nullopt;
    const auto Displacement = scalarRange(Delta, Depth + 1);
    if (Steps > kMaxProofSteps || Displacement.getUnsignedMax().ugt(INT64_MAX))
      return std::nullopt;
    const int64_t Min = Displacement.getUnsignedMin().getZExtValue();
    const int64_t Max = Displacement.getUnsignedMax().getZExtValue();
    const auto Lower = safety::detail::checkedStackOffset(
        Base->Min, Subtract ? Max : Min, Subtract);
    const auto Upper = safety::detail::checkedStackOffset(
        Base->Max, Subtract ? Min : Max, Subtract);
    if (!Lower || !Upper || !fitsPointerOffset(*Lower) ||
        !fitsPointerOffset(*Upper))
      return std::nullopt;
    return EntryStackOffsetRange{*Lower, *Upper};
  }

  bool authenticatedEntryLiveIn(const MedVar &Value,
                                ValueDefinition *Definition) {
    // Alignment rewrites can change the entry COPY prefix. Only read-only
    // offset queries reuse authentication, and only for the exact occurrence.
    if (!FollowJoins)
      return safety::detail::isAuthenticatedEntryRegisterLiveIn(Func, Value);
    if (!Definition || !Definition->Op)
      return false;
    if (!Definition->EntryChecked) {
      Definition->EntryLiveIn =
          safety::detail::isAuthenticatedEntryRegisterLiveIn(
              Func, Definition->Op->Output);
      Definition->EntryChecked = true;
    }
    return Definition->EntryLiveIn && safety::detail::sameRegisterOccurrence(
                                          Definition->Op->Output, Value);
  }

  /// The offset every way into \p Phi brings.  A way back through the join
  /// itself is checked against the offset the other ways agree on.
  std::optional<int64_t> join(const PhiNode &Phi, unsigned Depth) {
    if (Phi.ExceptionalEntry)
      return std::nullopt;
    std::optional<int64_t> Agreed;
    std::vector<const MedVar *> Cyclic;
    for (const auto &[Pred, Arg] : Phi.Args) {
      (void)Pred;
      const bool OuterCycle = CycleHit;
      CycleHit = false;
      const std::optional<int64_t> Offset = resolve(Arg, Depth + 1);
      const bool ThroughCycle = !Offset && CycleHit;
      CycleHit = OuterCycle;
      if (ThroughCycle) {
        Cyclic.push_back(&Arg);
        continue;
      }
      if (!Offset || (Agreed && *Agreed != *Offset))
        return std::nullopt;
      Agreed = Offset;
    }
    if (!Agreed)
      return std::nullopt;
    if (Cyclic.empty())
      return Agreed;
    const Key K = key(Phi.Output);
    Assumed.emplace(K, *Agreed);
    bool Holds = true;
    for (const MedVar *Arg : Cyclic) {
      const std::optional<int64_t> Offset = resolve(*Arg, Depth + 1);
      if (!Offset || *Offset != *Agreed) {
        Holds = false;
        break;
      }
    }
    Assumed.erase(K);
    return Holds ? Agreed : std::nullopt;
  }

  bool fitsPointerOffset(int64_t Offset) const {
    return TRI.PointerSize == 8 ||
           Offset == static_cast<int64_t>(static_cast<int32_t>(Offset));
  }

  /// A set of mutually dependent PHIs denotes one offset only when every
  /// value entering the set proves that same offset. Proving one PHI at a
  /// time cannot establish this invariant for overlapping loop backedges:
  /// each nested proof can encounter another still-active PHI. Copies carry
  /// the same value around these cycles; other definitions remain boundaries
  /// and retain resolve()'s semantics.
  std::optional<std::vector<std::pair<MedVar, unsigned>>>
  phiComponentBoundaries(const MedVar &Root, unsigned Depth, bool &HasCycle,
                         bool RangeQuery = false) {
    std::set<Key> Visited, Active;
    std::vector<std::pair<MedVar, unsigned>> Boundaries;
    std::set<Key> Seeded;
    std::map<Key, std::set<Key>> Users;
    const Key RootKey = key(Root);
    const auto Visit = [&](const auto &Self, const MedVar &Value,
                           unsigned AtDepth,
                           std::optional<Key> Parent) -> bool {
      Deepest = std::max(Deepest, AtDepth);
      if (++Steps > kMaxProofSteps ||
          AtDepth > limits::ir::kMedStackAlignmentResolveDepthLimit ||
          Value.Size != TRI.PointerSize ||
          (Value.Kind != MedVar::Reg && Value.Kind != MedVar::Temp))
        return false;
      const Key K = key(Value);
      const auto Found = Definitions.find(K);
      ValueDefinition *Definition =
          Found == Definitions.end() ? nullptr : &Found->second;
      if (Definition && Definition->Ambiguous)
        return false;
      const MedOp *Op = Definition ? Definition->Op : nullptr;
      const bool EntryOrigin = !OnlySeededRoots && Value.Kind == MedVar::Reg &&
                               Value.RegOff == TRI.StackPointer &&
                               (authenticatedEntryLiveIn(Value, Definition) ||
                                (Value.SSAVer == 0 && !Op));
      const bool Copy =
          Op && sameOccurrence(Op->Output, Value) && !Op->Dead &&
          Op->Opcode == NdOp::COPY && Op->NumInputs == 1 &&
          Op->Inputs[0].Size == Value.Size &&
          Op->RegistrationRoot == MedOp::RegistrationRootKind::None &&
          Op->MemoryOrdering == NdMemoryOrdering::None &&
          Op->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
          Op->IntrinsicOutputs.empty();
      if (!Definition || (!Definition->Join && !Copy) || Assumed.count(K) ||
          Definition->ProvenOffset || Definition->ProvenRange || EntryOrigin ||
          (RangeQuery && K != RootKey && Definition->Join &&
           sameOccurrence(Definition->Join->Output, Value) &&
           recurrence(*Definition->Join))) {
        Boundaries.emplace_back(Value, AtDepth);
        if (Parent)
          Seeded.insert(*Parent);
        return true;
      }
      const PhiNode *Phi = Definition->Join;
      if ((Phi && (!sameOccurrence(Phi->Output, Value) || Phi->Args.empty() ||
                   Phi->ExceptionalEntry)) ||
          (Definition->ActiveContext == Assumed.size() + 1 && K != RootKey))
        return false;
      if (Parent)
        Users[K].insert(*Parent);
      if (Active.count(K)) {
        HasCycle = true;
        return true;
      }
      if (!Visited.insert(K).second)
        return true;
      Active.insert(K);
      if (Phi) {
        for (const auto &[Pred, Arg] : Phi->Args) {
          (void)Pred;
          if (!Self(Self, Arg, AtDepth + 1, K))
            return false;
        }
      } else {
        if (!Self(Self, Op->Inputs[0], AtDepth + 1, K))
          return false;
      }
      Active.erase(K);
      return true;
    };
    if (!Visit(Visit, Root, Depth, std::nullopt) || Boundaries.empty())
      return std::nullopt;
    // Every PHI in the reachable component must itself reach an outside
    // origin. A separate unseeded cycle is still unknown even if another
    // arm of the queried PHI has a valid stack origin.
    std::vector<Key> Pending(Seeded.begin(), Seeded.end());
    while (!Pending.empty()) {
      const auto Current = Pending.back();
      Pending.pop_back();
      for (const auto &User : Users[Current]) {
        if (++Steps > kMaxProofSteps)
          return std::nullopt;
        if (Seeded.insert(User).second)
          Pending.push_back(User);
      }
    }
    if (Seeded.size() != Visited.size())
      return std::nullopt;
    return Boundaries;
  }

  std::optional<int64_t> joinPhiComponent(const MedVar &Root, unsigned Depth) {
    bool HasCycle = false;
    const auto Boundaries = phiComponentBoundaries(Root, Depth, HasCycle);
    if (!Boundaries || !HasCycle)
      return std::nullopt;
    std::optional<int64_t> Agreed;
    for (const auto &[Value, AtDepth] : *Boundaries) {
      const auto Offset = resolve(Value, AtDepth);
      if (!Offset || (Agreed && *Agreed != *Offset))
        return std::nullopt;
      Agreed = Offset;
    }
    return Agreed;
  }

  std::optional<int64_t> resolveJoin(const MedVar &Value,
                                     ValueDefinition &Definition,
                                     unsigned Depth) {
    // Shared joins branch into the same inputs repeatedly. Linear definitions
    // keep their ordinary traversal; only this read-only join proof looks up
    // completed work. The current resolve() has already charged its own step
    // and checked the occurrence, depth and active definition.
    const Key K = key(Value);
    if (auto It = Completed.find(K); It != Completed.end()) {
      const CompletedProof &Proof = It->second;
      if (sameOccurrence(Value, Proof.Occurrence) &&
          Proof.Steps <= kMaxProofSteps - Steps &&
          Proof.Depth <=
              ::neverd::limits::ir::kMedStackAlignmentResolveDepthLimit -
                  Depth) {
        // Charge every original input traversal, including repeated DAG edges.
        Steps += Proof.Steps;
        Deepest = std::max(Deepest, Depth + Proof.Depth);
        return Proof.Offset;
      }
    }
    const size_t Before = Steps;
    const size_t BeforeContext = ContextUses;
    const unsigned OuterDepth = Deepest;
    Deepest = Depth;
    auto Result = join(*Definition.Join, Depth);
    if (!Result && ContextUses != BeforeContext && !JoiningComponent &&
        Steps < kMaxProofSteps) {
      JoiningComponent = true;
      Result = joinPhiComponent(Value, Depth);
      JoiningComponent = false;
    }
    const unsigned InnerDepth = Deepest;
    Deepest = std::max(OuterDepth, InnerDepth);
    // A successful traversal that never reads a loop assumption or an active
    // definition is independent of its ancestors. Failed or cyclic proofs
    // remain live queries, including joins that establish an assumed offset.
    if (Result && ContextUses == BeforeContext)
      Completed.insert_or_assign(
          K,
          CompletedProof{Value, *Result, Steps - Before, InnerDepth - Depth});
    return Result;
  }

  std::optional<int64_t> resolve(const MedVar &Query, unsigned Depth) {
    // A copy or constant displacement continues the same proof. Follow such a
    // run without recursion, so a long push/discard sequence is one chain and
    // not nested proofs, then replay its displacements from the base value
    // with the same per-step checks a recursive traversal applies.
    llvm::SmallVector<std::pair<int64_t, bool>, 8> Links;
    llvm::SmallVector<std::pair<ValueDefinition *, size_t>, 8> Activated;
    auto Restore = llvm::make_scope_exit([&] {
      for (auto It = Activated.rbegin(); It != Activated.rend(); ++It)
        It->first->ActiveContext = It->second;
    });
    const auto Replay =
        [&](std::optional<int64_t> Offset) -> std::optional<int64_t> {
      for (auto It = Links.rbegin(); Offset && It != Links.rend(); ++It) {
        Offset =
            safety::detail::checkedStackOffset(*Offset, It->first, It->second);
        if (Offset && !fitsPointerOffset(*Offset))
          Offset.reset();
      }
      return Offset;
    };
    const MedVar *Current = &Query;
    for (;;) {
      const MedVar &Value = *Current;
      Deepest = std::max(Deepest, Depth);
      const bool WideAddress =
          FollowJoins && TRI.PointerSize == 4 && Value.Size == sizeof(uint64_t);
      // Keep the per-query work limit even when shared inputs reuse a proof.
      if (++Steps > kMaxProofSteps)
        return std::nullopt;
      if (Depth > ::neverd::limits::ir::kMedStackAlignmentResolveDepthLimit ||
          (Value.Size != TRI.PointerSize && !WideAddress) ||
          (Value.Kind != MedVar::Reg && Value.Kind != MedVar::Temp) ||
          (AllowedDefinitions && Value.TheArch != Architecture))
        return std::nullopt;
      const Key K = key(Value);
      const auto It = Definitions.find(K);
      ValueDefinition *Definition =
          It == Definitions.end() ? nullptr : &It->second;
      // An induction assumption changes the query: an arithmetic definition
      // active outside that assumption must be checked again on the backedge.
      // The scoped marker still rejects cycles within the same context. Zero
      // means inactive; nested assumptions are pushed and popped in call
      // order.
      const size_t Context = Assumed.size() + 1;
      if (Definition && Definition->Ambiguous)
        return std::nullopt;
      // A range is not one executable stack value. Only a singleton may
      // participate in an exact query through the same current definition.
      if (Definition && Definition->ProvenRange) {
        const MedVar &Output =
            Definition->Op ? Definition->Op->Output : Definition->Join->Output;
        if (!sameOccurrence(Output, Value) ||
            Definition->ProvenRange->Min != Definition->ProvenRange->Max)
          return std::nullopt;
        return Replay(Definition->ProvenRange->Min);
      }
      if (AllowedDefinitions &&
          (!Definition || !Definition->Op ||
           !AllowedDefinitions->count(Definition->Op) ||
           Definition->Op->MemoryOrdering != NdMemoryOrdering::None ||
           Definition->Op->MemoryAddressSpace !=
               NdMemoryAddressSpace::Default ||
           !Definition->Op->IntrinsicOutputs.empty()))
        return std::nullopt;
      if (WideAddress) {
        // The lifter carries native 32-bit memory addresses in a zero-extended
        // 64-bit address container. This preserves the target pointer; it is
        // not a truncation of a 64-bit SP or an eight-byte incoming SP seed.
        const MedOp *Op = Definition ? Definition->Op : nullptr;
        if (!Op || Definition->ActiveContext == Context ||
            !sameOccurrence(Op->Output, Value) ||
            Op->Opcode != NdOp::INT_ZEXT || Op->NumInputs != 1 ||
            Op->Inputs[0].Size != TRI.PointerSize ||
            Op->MemoryOrdering != NdMemoryOrdering::None ||
            Op->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
            !Op->IntrinsicOutputs.empty())
          return std::nullopt;
        llvm::SaveAndRestore Active(Definition->ActiveContext, Context);
        return Replay(resolve(Op->Inputs[0], Depth + 1));
      }
      if (!OnlySeededRoots && Value.Kind == MedVar::Reg &&
          Value.RegOff == TRI.StackPointer &&
          (authenticatedEntryLiveIn(Value, Definition) ||
           (FollowJoins && Value.SSAVer == 0 &&
            (!Definition || !Definition->Op))))
        return Replay(0);
      if (Definition && Definition->ProvenOffset && Definition->Op &&
          sameOccurrence(Definition->Op->Output, Value))
        return Replay(Definition->ProvenOffset);
      if (auto It = Assumed.find(K); It != Assumed.end()) {
        ++ContextUses;
        return Replay(It->second);
      }
      if (!Definition)
        return std::nullopt;
      if (Definition->ActiveContext == Context) {
        ++ContextUses;
        CycleHit = true;
        return std::nullopt;
      }
      Activated.emplace_back(Definition, Definition->ActiveContext);
      Definition->ActiveContext = Context;
      if (Definition->Join)
        return Replay(resolveJoin(Value, *Definition, Depth));
      std::optional<int64_t> Result;
      if (Definition->Op) {
        const MedOp &Op = *Definition->Op;
        if (sameOccurrence(Op.Output, Value) &&
            Op.MemoryOrdering == NdMemoryOrdering::None &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            Op.IntrinsicOutputs.empty()) {
          if (Op.RegistrationRoot != MedOp::RegistrationRootKind::None) {
            if (!OnlySeededRoots && Architecture == Arch::X86 &&
                Format == BinaryFormat::COFF) {
              const auto Coordinate = registrationRootFrameCoordinate(Func, Op);
              if (Coordinate && Coordinate->Alignment == 1)
                Result = int64_t(Coordinate->EntryOffset) +
                         Coordinate->AlignedOffset;
            }
          } else if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1) {
            Links.emplace_back(0, false);
            Current = &Op.Inputs[0];
            continue;
          } else if ((Op.Opcode == NdOp::INT_ADD ||
                      Op.Opcode == NdOp::INT_SUB) &&
                     Op.NumInputs == 2) {
            unsigned BaseIndex =
                Op.Opcode == NdOp::INT_ADD && Op.Inputs[0].isConst() ? 1 : 0;
            const MedVar &Base = Op.Inputs[BaseIndex];
            const MedVar &Delta = Op.Inputs[1 - BaseIndex];
            auto Signed = safety::detail::signedStackConstant(Delta);
            // ARM64 immediates can retain their encoded 32-bit carrier in a
            // pointer-width ADD/SUB. A nonnegative narrow constant has the
            // same value under either extension. Do not guess the extension
            // of a negative narrow carrier or admit a wider/truncated
            // displacement.
            if (Signed && (Delta.Size == TRI.PointerSize ||
                           (Delta.Size < TRI.PointerSize && *Signed >= 0))) {
              Links.emplace_back(*Signed, Op.Opcode == NdOp::INT_SUB);
              Current = &Base;
              continue;
            }
          } else if (!OnlySeededRoots && Op.Opcode == NdOp::INT_AND) {
            if (auto Aligned = alignment(Op, Depth + 1))
              Result = Aligned->Offset;
          }
        }
      }
      if (Result && !fitsPointerOffset(*Result))
        Result.reset();
      return Replay(Result);
    }
  }

  const MedFunc &Func;
  const TargetRegInfo &TRI;
  Arch Architecture;
  BinaryFormat Format;
  StackEntryKind Entry;
  bool FollowJoins;
  bool OnlySeededRoots;
  // A realignment root must be established by the exact entry prefix already
  // executed before its mask. Ordinary entry-offset queries also follow joins.
  const std::set<const MedOp *> *AllowedDefinitions = nullptr;
  // All definitions are inserted before queries begin, so their addresses stay
  // stable during traversal. Only read-only proofs follow joins. Their
  // completed traversals retain the exact occurrence, work and depth proved.
  llvm::DenseMap<Key, ValueDefinition> Definitions;
  llvm::DenseMap<Key, CompletedProof> Completed;
  std::set<Key> RangeActive;
  size_t ContextUses = 0;
  unsigned Deepest = 0;
  std::map<Key, int64_t> Assumed;
  bool CycleHit = false;
  bool JoiningComponent = false;
  static constexpr size_t kMaxProofSteps =
      ::neverd::limits::ir::kMedStackAlignmentMaxProofSteps;
  size_t Steps = 0;
};

} // namespace

std::vector<MedVar> entryStackRealignmentRoots(const MedFunc &Func,
                                               Arch Architecture,
                                               BinaryFormat Format) {
  const MedBlock *Entry = nullptr;
  for (const auto &Block : Func.Blocks)
    if (Block.StartAddr == Func.Entry) {
      if (Entry)
        return {};
      Entry = &Block;
    }
  if (!Entry || !Entry->Preds.empty() || !Entry->ExceptionalPreds.empty())
    return {};
  for (const auto &Block : Func.Blocks) {
    for (int Successor : Block.Succs)
      if (Successor == Entry->Id)
        return {};
    for (const auto &Successor : Block.ExceptionalSuccs)
      if (Successor.BlockId == Entry->Id)
        return {};
  }
  StackOffsetProof Proof(Func, Architecture, Format, StackEntryKind::Call);
  std::vector<MedVar> Roots;
  std::set<const MedOp *> EarlierEntryOps;
  for (const auto &Op : Entry->Ops) {
    if (Proof.realignmentRoot(Op, EarlierEntryOps))
      Roots.push_back(Op.Output);
    EarlierEntryOps.insert(&Op);
  }
  return Roots;
}

StackEntryKind functionEntryKind(const MedFunc &Func, StackEntryKind AtEntry) {
  if (AtEntry != StackEntryKind::ProcessEntry)
    return AtEntry;
  for (const MedBlock &Block : Func.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::RETURN)
        return StackEntryKind::Call;
  return StackEntryKind::ProcessEntry;
}

std::optional<int64_t> entryStackOffset(const MedFunc &Func,
                                        const MedVar &Value, Arch Architecture,
                                        BinaryFormat Format,
                                        StackEntryKind Entry) {
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return std::nullopt;
  return StackOffsetProof(Func, Architecture, Format, Entry,
                          /*FollowJoins=*/true)
      .offset(Value);
}

std::vector<std::optional<int64_t>>
entryStackOffsets(const MedFunc &Func, llvm::ArrayRef<MedVar> Values,
                  Arch Architecture, BinaryFormat Format,
                  StackEntryKind Entry) {
  std::vector<std::optional<int64_t>> Offsets(Values.size());
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return Offsets;
  StackOffsetProof Proof(Func, Architecture, Format, Entry,
                         /*FollowJoins=*/true);
  for (size_t I = 0; I < Values.size(); ++I)
    Offsets[I] = Proof.offset(Values[I]);
  return Offsets;
}

std::vector<std::optional<EntryStackOffsetRange>>
entryStackOffsetRanges(const MedFunc &Func, llvm::ArrayRef<MedVar> Values,
                       Arch Architecture, BinaryFormat Format,
                       StackEntryKind Entry) {
  std::vector<std::optional<EntryStackOffsetRange>> Ranges(Values.size());
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return Ranges;
  StackOffsetProof Proof(Func, Architecture, Format, Entry, true);
  for (size_t I = 0; I < Values.size(); ++I)
    Ranges[I] = Proof.range(Values[I]);
  return Ranges;
}

std::vector<std::optional<int64_t>>
entryStackOffsetUpperBounds(const MedFunc &Func, llvm::ArrayRef<MedVar> Values,
                            Arch Architecture, BinaryFormat Format,
                            StackEntryKind Entry) {
  std::vector<std::optional<int64_t>> Bounds(Values.size());
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return Bounds;
  StackOffsetProof Proof(Func, Architecture, Format, Entry, true);
  for (size_t I = 0; I < Values.size(); ++I)
    Bounds[I] = Proof.upperBound(Values[I]);
  return Bounds;
}

std::vector<std::optional<int64_t>> entryStackOffsetsFromProvenDefinitions(
    const MedFunc &Func, llvm::ArrayRef<MedVar> Values,
    llvm::ArrayRef<ProvenStackOffset> Seeds, Arch Architecture,
    BinaryFormat Format, StackEntryKind Entry) {
  std::vector<std::optional<int64_t>> Offsets(Values.size());
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return Offsets;
  StackOffsetProof Proof(Func, Architecture, Format, Entry,
                         /*FollowJoins=*/true, Seeds);
  for (size_t I = 0; I < Values.size(); ++I)
    Offsets[I] = Proof.offset(Values[I]);
  return Offsets;
}

std::vector<std::optional<EntryStackOffsetRange>>
entryStackOffsetRangesFromProvenDefinitions(
    const MedFunc &Func, llvm::ArrayRef<MedVar> Values,
    llvm::ArrayRef<ProvenStackOffsetRange> Seeds, Arch Architecture,
    BinaryFormat Format, StackEntryKind Entry) {
  std::vector<std::optional<EntryStackOffsetRange>> Ranges(Values.size());
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return Ranges;
  StackOffsetProof Proof(Func, Architecture, Format, Entry,
                         /*FollowJoins=*/true, {},
                         /*OnlySeededRoots=*/false, Seeds);
  for (size_t I = 0; I < Values.size(); ++I)
    Ranges[I] = Proof.range(Values[I]);
  return Ranges;
}

std::vector<std::optional<int64_t>> stackOffsetsRelativeToDefinition(
    const MedFunc &Func, llvm::ArrayRef<MedVar> Values, const MedOp &Root,
    Arch Architecture, BinaryFormat Format) {
  std::vector<std::optional<int64_t>> Offsets(Values.size());
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return Offsets;
  const ProvenStackOffset Seed{&Root, 0};
  StackOffsetProof Proof(Func, Architecture, Format, StackEntryKind::Call,
                         /*FollowJoins=*/true, {Seed},
                         /*OnlySeededRoots=*/true);
  for (size_t I = 0; I < Values.size(); ++I)
    Offsets[I] = Proof.offset(Values[I]);
  return Offsets;
}

void simplifyProvenStackAlignment(MedFunc &Func, Arch Architecture,
                                  BinaryFormat Format, StackEntryKind Entry) {
  const auto &TRI = getTargetRegInfo(Architecture);
  if ((TRI.PointerSize != 4 && TRI.PointerSize != 8) || Func.Blocks.empty())
    return;
  StackOffsetProof Proof(Func, Architecture, Format, Entry);
  for (MedBlock &Block : Func.Blocks)
    for (MedOp &Op : Block.Ops)
      if (const auto Aligned = Proof.alignment(Op)) {
        const MedVar Base = Op.Inputs[Aligned->BaseIndex];
        Op.Inputs[0] = Base;
        if (Aligned->Remainder == 0) {
          Op.Opcode = NdOp::COPY;
          Op.NumInputs = 1;
        } else {
          Op.Opcode = NdOp::INT_SUB;
          Op.Inputs[1] = MedVar::makeConst(Aligned->Remainder, TRI.PointerSize,
                                           ConstantAddressProvenance::Scalar);
          Op.NumInputs = 2;
        }
      }
}

} // namespace neverd
