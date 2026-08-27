//===- JumpTableResolverTwoLevel.cpp - Two-level index tables -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognizer for the two-level index-byte jump table — the classic MSVC
/// sparse-switch lowering `jmptab[idxtab[switchvar]]`, where a narrow index
/// table maps the switch variable onto a slot of the real address table.  It
/// composes the per-case targets explicitly, since dispatching on the
/// intermediate table index instead of the switch variable would collapse the
/// case set.  Single-level composite shapes live in
/// JumpTableResolverShapes.cpp.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

//===----------------------------------------------------------------------===//
// tryTwoLevelIndexTable — index-byte (MSVC-style) two-level table
//===----------------------------------------------------------------------===//

/// Count the run of consecutive relocation slots in \p Slots starting at
/// \p TableAddr, stepping by \p EntrySize.  Mirrors the code/rel-code run
/// counters in JumpTableResolver.cpp for a locally-supplied slot set.
struct BoundedRelocationRun {
  uint32_t Count = 0;
  bool Complete = true;
};

template <typename ConsumeLookup>
static std::optional<BoundedRelocationRun>
relocRunIn(const std::set<uint64_t> &Slots, va_t TableAddr,
           uint16_t EntrySize, uint64_t OwnerEntries,
           ConsumeLookup &&Consume) {
  if (EntrySize == 0 || Slots.empty() || OwnerEntries == 0)
    return BoundedRelocationRun{};
  const uint32_t ScanLimit = static_cast<uint32_t>(std::min<uint64_t>(
      OwnerEntries, limits::kMaxJumpTableEntries));
  uint32_t Run = 0;
  va_t VA = TableAddr;
  for (; Run < ScanLimit;) {
    if (!Consume())
      return std::nullopt;
    if (!Slots.count(VA))
      return BoundedRelocationRun{Run, true};
    ++Run;
    if (EntrySize > InvalidVA - VA)
      return BoundedRelocationRun{Run, true};
    VA += EntrySize;
  }
  if (uint64_t(Run) == OwnerEntries)
    return BoundedRelocationRun{Run, true};
  // The owner extends past the bounded identity ceiling.  Probe one more slot
  // using the caller's account: a continuing relocation run is a lower bound,
  // not a complete physical identity.
  if (!Consume())
    return std::nullopt;
  return BoundedRelocationRun{Run, !Slots.count(VA)};
}

/// Decompose the address of an *index-table* load (`idxtab + switchvar[*s1]`)
/// into its constant table base and the index register.  Unlike
/// analyzeTableLoadAddr this tolerates an unscaled index (a byte index table
/// has scale 1, so there is no INT_MULT/INT_LEFT to key on) and folds one
/// operand to a constant read-only VA to identify the table base.  Returns
/// true and sets \p TableAddr (folded base), \p IndexReg (traced to a plain
/// register), and \p Scale (1 or the entry width) on success.
bool CFGBuilder::decomposeIndexTableLoadAddr(
    const BinaryImage &Img, const InsnRecord &Rec,
    const std::vector<LowOp> &Ops, int LoadIdx, uint16_t EntryWidth,
    va_t &TableAddr, uint64_t &IndexReg, uint32_t &Scale, NdVar *IndexValue,
    va_t *IndexUseAddr, int *IndexUseSeq,
    std::function<bool(size_t)> ConsumeWork) const {
  if (LoadIdx <= 0 || LoadIdx >= static_cast<int>(Ops.size()))
    return false;
  // The decomposition performs several full backward scans: the address COPY
  // chain, both possible base/index partitions, scaled-index recovery and the
  // final scale trace.  Charge their bounded worst case before the first raw
  // reaching-definition lookup; foldRegConstant debits its own prefix work
  // through the same callback below.
  if (ConsumeWork) {
    constexpr size_t ScanFactor =
        size_t(limits::kMaxQuasiCopyDepth) * 8;
    if (Ops.size() > std::numeric_limits<size_t>::max() / ScanFactor) {
      ConsumeWork(std::numeric_limits<size_t>::max());
      return false;
    }
    if (!ConsumeWork(Ops.size() * ScanFactor))
      return false;
  }
  const LowOp &L = Ops[LoadIdx];
  const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
  int AddIdx = reachingDefIdx(Ops, LoadIdx - 1, AddrV);
  for (int G = 0;
       AddIdx >= 0 && Ops[AddIdx].Opcode == NdOp::COPY &&
       Ops[AddIdx].NumInputs >= 1 &&
       (Ops[AddIdx].Inputs[0].isReg() || Ops[AddIdx].Inputs[0].isTemp()) &&
       G < limits::kMaxQuasiCopyDepth;
       ++G)
    AddIdx = reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[0]);
  if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
      Ops[AddIdx].NumInputs < 2)
    return false;
  va_t LoadAddr = L.Addr;

  // One operand is the (constant / foldable) table base; the other is the
  // switch-variable index, optionally scaled by the entry width.
  for (int BaseW = 0; BaseW < 2; ++BaseW) {
    const NdVar &BaseV = Ops[AddIdx].Inputs[BaseW];
    const NdVar &IdxV = Ops[AddIdx].Inputs[1 - BaseW];

    va_t Base = 0;
    if (BaseV.isConst()) {
      Base = BaseV.Offset;
    } else if (BaseV.isReg() || BaseV.isTemp()) {
      uint64_t BaseReg = traceToRegister(Ops, AddIdx - 1, BaseV);
      if (BaseReg == InvalidVA)
        continue;
      auto Folded =
          foldRegConstant(Img, Rec, BaseReg, LoadAddr, ConsumeWork);
      if (!Folded)
        continue;
      Base = *Folded;
    } else {
      continue;
    }
    if (!Img.getSegmentFor(Base))
      continue;

    // The index may be scaled (halfword index table: `idx*2`) or plain (byte
    // index table: scale 1).  Require the scale to equal the entry width.
    uint32_t S = 1;
    NdVar CandidateValue;
    va_t CandidateUseAddr = InvalidVA;
    int CandidateUseSeq = -1;
    uint64_t IdxReg = scaledIndexReg(Ops, AddIdx - 1, IdxV, &CandidateValue,
                                     &CandidateUseAddr, &CandidateUseSeq);
    if (IdxReg != InvalidVA) {
      // Recover the concrete scale so it can be validated against EntryWidth.
      int SD = reachingDefIdx(Ops, AddIdx - 1, IdxV);
      for (int G = 0;
           SD >= 0 &&
           (Ops[SD].Opcode == NdOp::COPY || Ops[SD].Opcode == NdOp::INT_ZEXT ||
            Ops[SD].Opcode == NdOp::INT_SEXT) &&
           Ops[SD].NumInputs >= 1 && G < limits::kMaxQuasiCopyDepth;
           ++G)
        SD = reachingDefIdx(Ops, SD - 1, Ops[SD].Inputs[0]);
      if (SD < 0)
        continue;
      if (Ops[SD].Opcode == NdOp::INT_MULT && Ops[SD].NumInputs >= 2 &&
          Ops[SD].Inputs[1].isConst())
        S = static_cast<uint32_t>(Ops[SD].Inputs[1].Offset);
      else if (Ops[SD].Opcode == NdOp::INT_LEFT && Ops[SD].NumInputs >= 2 &&
               Ops[SD].Inputs[1].isConst() && Ops[SD].Inputs[1].Offset < 6)
        S = 1u << Ops[SD].Inputs[1].Offset;
      else
        continue;
    } else {
      IdxReg = traceToRegister(Ops, AddIdx - 1, IdxV);
      if (IdxReg == InvalidVA)
        continue;
      CandidateValue = IdxV;
      CandidateUseAddr = Ops[AddIdx].Addr;
      CandidateUseSeq = Ops[AddIdx].Seq;
    }
    if (S != EntryWidth)
      continue;

    TableAddr = Base;
    IndexReg = IdxReg;
    Scale = S;
    if (IndexValue)
      *IndexValue = CandidateValue;
    if (IndexUseAddr)
      *IndexUseAddr = CandidateUseAddr;
    if (IndexUseSeq)
      *IndexUseSeq = CandidateUseSeq;
    return true;
  }
  return false;
}

bool CFGBuilder::tryTwoLevelIndexTable(const BinaryImage &Img,
                                       const InsnRecord &Rec,
                                       JumpTableInfo &Info,
                                       size_t *CandidateEvidenceBudget) {
  if (!CurrentImg)
    return false;

  bool ClampAnalysisIncomplete = false;
  bool ClaimCompositeOnExhaustion = false;
  auto consumeEvidence = [&](size_t Amount = 1) {
    if (!CandidateEvidenceBudget)
      return true;
    if (Amount > *CandidateEvidenceBudget) {
      *CandidateEvidenceBudget = 0;
      ClampAnalysisIncomplete = true;
      if (ClaimCompositeOnExhaustion) {
        Info.CompositeShapeClaimed = true;
        Info.IncompleteGuardDomain = true;
      }
      return false;
    }
    *CandidateEvidenceBudget -= Amount;
    return true;
  };
  auto consumeProduct = [&](size_t Count, size_t Cost) {
    if (Count != 0 && Cost > std::numeric_limits<size_t>::max() / Count)
      return consumeEvidence(std::numeric_limits<size_t>::max());
    return consumeEvidence(Count * Cost);
  };
  auto consumeProducts =
      [&](std::initializer_list<std::pair<size_t, size_t>> Products) {
        size_t Total = 0;
        for (const auto &[Count, Cost] : Products) {
          if (Count != 0 && Cost >
                                (std::numeric_limits<size_t>::max() - Total) /
                                    Count)
            return consumeEvidence(std::numeric_limits<size_t>::max());
          Total += Count * Cost;
        }
        return consumeEvidence(Total);
      };
  auto consumeFactorProduct = [&](std::initializer_list<size_t> Factors) {
    size_t Product = 1;
    for (size_t Factor : Factors) {
      if (Factor != 0 &&
          Product > std::numeric_limits<size_t>::max() / Factor)
        return consumeEvidence(std::numeric_limits<size_t>::max());
      Product *= Factor;
    }
    return consumeEvidence(Product);
  };
  auto orderedLookupWork = [](size_t Count) {
    size_t Work = 1;
    for (size_t N = Count; N > 1; N = N / 2 + N % 2)
      ++Work;
    return Work;
  };

  if (!consumeEvidence(Rec.Ops.size()))
    return false;
  bool HasIndBranch = false;
  for (const LowOp &Op : Rec.Ops)
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1) {
      HasIndBranch = true;
      break;
    }
  if (!HasIndBranch)
    return false;

  // Flatten the dispatch block plus its single-predecessor path so both chained
  // loads (the index-table load in a predecessor goto-site block and the
  // address-table load at the branch) are visible to one backward scan.  The
  // shared collector owns several temporary vectors/sets and may scan the
  // whole instruction map once per bounded predecessor depth.  Prepay a
  // conservative inventory before its first lookup/allocation.  This is still
  // pre-shape work: exhaustion here must not suppress ordinary callbacks.
  const size_t BlockLookup = orderedLookupWork(BlockStarts.size());
  if (!consumeEvidence(BlockLookup))
    return false;
  va_t BlkStart = CurrentFuncEntry;
  auto BIt = BlockStarts.upper_bound(Rec.Addr);
  if (BIt != BlockStarts.begin()) {
    --BIt;
    BlkStart = *BIt;
  }

  if (!consumeEvidence(Insns.size()))
    return false;
  size_t PathOpInventory = 0;
  for (const auto &[Addr, Insn] : Insns) {
    (void)Addr;
    if (Insn.Ops.size() >
        std::numeric_limits<size_t>::max() - PathOpInventory) {
      consumeEvidence(std::numeric_limits<size_t>::max());
      return false;
    }
    PathOpInventory += Insn.Ops.size();
  }
  constexpr size_t PathDepth = limits::kMaxPathEmulationDepth;
  const size_t InsnLookup = orderedLookupWork(Insns.size());
  const size_t VisitedLookup = orderedLookupWork(PathDepth + 1);
  if (!consumeProducts({{PathDepth + 2, BlockLookup},
                        {PathDepth + 1, InsnLookup},
                        {PathDepth + 1, 8},
                        {1, 2}}) ||
      !consumeFactorProduct(
          {PathDepth, Insns.size(), 1 + VisitedLookup + BlockLookup + 3}) ||
      !consumeFactorProduct(
          {PathDepth, PathDepth + 1, VisitedLookup + 4}) ||
      !consumeFactorProduct({PathOpInventory, PathDepth + 1, 8}))
    return false;

  std::vector<LowOp> Ops = collectPathOps(BlkStart, Rec.Addr);
  if (Ops.empty())
    return false;

  // Mirror resolveJumpTable's proof-root preflight for the provisional
  // composite table.  This detector runs before the outer helper installs its
  // ordinary candidate roots, so suppressing relocation-discovered case roots
  // here must debit the same candidate transaction before any set/vector work.
  auto budgetedProofRoots =
      [&](const JumpTableInfo &Candidate)
      -> std::optional<std::set<va_t>> {
    if (!CurrentImg)
      return std::nullopt;
    size_t StorageCount = Candidate.StorageRanges.size();
    size_t SuppressibleSlotCount =
        Candidate.SuppressibleRelocationSlots.size();
    if (!consumeEvidence(PriorStrongJumpTableProposals.size()))
      return std::nullopt;
    for (const auto &[Addr, Proposal] : PriorStrongJumpTableProposals) {
      if (Addr == ActiveJumpTableCandidateAddr ||
          (!ActiveJumpTableConsumerAudit &&
           Proposal.ProofRank >= ActiveJumpTableCandidateProofRank))
        continue;
      if (Proposal.StorageRanges.size() >
              std::numeric_limits<size_t>::max() - StorageCount ||
          Proposal.SuppressibleRelocationSlots.size() >
              std::numeric_limits<size_t>::max() - SuppressibleSlotCount) {
        consumeEvidence(std::numeric_limits<size_t>::max());
        return std::nullopt;
      }
      StorageCount += Proposal.StorageRanges.size();
      SuppressibleSlotCount +=
          Proposal.SuppressibleRelocationSlots.size();
    }
    if (!consumeProducts(
            {{PersistentCFGRoots.size(), 2},
             {RelocatedInstructionAddressOccurrences.size(), 2},
             {StorageCount, 2},
             {Candidate.ExplicitTargets.size(), 2},
             {SuppressibleSlotCount, 4},
             {ProtectedJumpTableRelocationSlots
                  ? ProtectedJumpTableRelocationSlots->size()
                  : 0,
              1},
             {SuppressibleSlotCount, StorageCount},
             {SuppressibleSlotCount, CurrentImg->Segments.size()},
             {RelocationCFGRootSources.size(), 2}}))
      return std::nullopt;
    if (StorageCount > std::numeric_limits<size_t>::max() - 2) {
      consumeEvidence(std::numeric_limits<size_t>::max());
      return std::nullopt;
    }
    const size_t SourceCost = StorageCount + 2;
    for (const auto &[Target, Sources] : RelocationCFGRootSources) {
      (void)Target;
      if (!consumeProducts({{Sources.size(), SourceCost}}))
        return std::nullopt;
    }
    const std::set<va_t> NoDecodedTableAnchors;
    return jumpTableProofRoots(Candidate, &NoDecodedTableAnchors);
  };
  auto sameValue = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
  };
  auto reachingDef = [&](const NdVar &Value, int From) -> std::optional<int> {
    if (!consumeEvidence(Ops.size()))
      return std::nullopt;
    const int Def = reachingDefIdx(Ops, From, Value);
    if (Def < 0)
      return std::nullopt;
    return Def;
  };
  auto maskToSize = [](uint64_t Value, uint16_t Size) {
    if (Size == 0)
      return uint64_t{0};
    if (Size >= sizeof(uint64_t))
      return Value;
    return Value & ((uint64_t{1} << (Size * CHAR_BIT)) - 1);
  };

  // A clamped index is parsed from the dispatch block only, so per-instruction
  // temporary identifiers cannot be captured from a predecessor added by a
  // later fixed-point stage.  Its compiler-materialized bitmap and fallback
  // constants can, however, be live-in registers.  Recover only a strict
  // lexical scalar candidate for such a register.  Simple transports are
  // resolved directly; split-immediate expressions such as ARM MOVW/MOVT use
  // the bounded prefix emulator.  This candidate is never authority by itself;
  // the completed proof-context replay below proves the exact value at the
  // actual use.
  auto prefixScalarConstant =
      [&](const NdVar &Requested, va_t Cutoff,
          JumpTableValueOccurrence *Source) -> std::optional<uint64_t> {
    if (!Requested.isReg() || Requested.Size == 0 ||
        Requested.Size > sizeof(uint64_t))
      return std::nullopt;

    if (!consumeEvidence(Insns.size()))
      return std::nullopt;
    size_t PrefixOpCount = 0;
    for (const auto &[Addr, Insn] : Insns) {
      if (Addr >= BlkStart)
        continue;
      if (Insn.Ops.size() >
          std::numeric_limits<size_t>::max() - PrefixOpCount) {
        consumeEvidence(std::numeric_limits<size_t>::max());
        return std::nullopt;
      }
      PrefixOpCount += Insn.Ops.size();
    }
    if (!consumeProducts({{Insns.size(), 1}, {PrefixOpCount, 1}}))
      return std::nullopt;

    // ARM materialises a 32-bit scalar bitmap as MOVW/MOVT, which LowIR
    // represents as a constant expression across two instructions rather than
    // a single COPY.  Reuse the fully budgeted prefix emulator to evaluate
    // that scalar, but keep it as a candidate only: the immutable proof-graph
    // query below must still prove the exact value at the real use.
    const std::optional<uint64_t> Emulated = foldRegConstant(
        Img, Rec, Requested.Offset, Cutoff,
        [&](size_t Amount) { return consumeEvidence(Amount); },
        /*RequireMappedValue=*/false);
    if (Emulated) {
      if (Source) {
        for (auto It = Insns.rbegin(); It != Insns.rend(); ++It) {
          if (It->first >= BlkStart || It->first >= Cutoff)
            continue;
          const std::vector<LowOp> &RecordOps = It->second.Ops;
          for (int I = static_cast<int>(RecordOps.size()) - 1; I >= 0; --I)
            if (sameValue(RecordOps[I].Output, Requested)) {
              *Source = {RecordOps[I].Output, RecordOps[I].Addr,
                         RecordOps[I].Seq, /*DefinedAtPoint=*/true};
              return maskToSize(*Emulated, Requested.Size);
            }
        }
        *Source = {};
      }
      return maskToSize(*Emulated, Requested.Size);
    }

    for (auto It = Insns.rbegin(); It != Insns.rend(); ++It) {
      if (It->first >= BlkStart)
        continue;
      const std::vector<LowOp> &RecordOps = It->second.Ops;
      for (int I = static_cast<int>(RecordOps.size()) - 1; I >= 0; --I) {
        if (!sameValue(RecordOps[I].Output, Requested))
          continue;

        NdVar Value = Requested;
        int Def = I;
        for (size_t Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
          if (!consumeEvidence(RecordOps.size()))
            return std::nullopt;
          const LowOp &Op = RecordOps[Def];
          if (!sameValue(Op.Output, Value) || Op.NumInputs < 1)
            return std::nullopt;
          if (Op.Opcode != NdOp::COPY && Op.Opcode != NdOp::INT_ZEXT &&
              Op.Opcode != NdOp::SUBBYTES)
            return std::nullopt;
          if (Op.Opcode == NdOp::SUBBYTES &&
              (Op.NumInputs < 2 || !Op.Inputs[1].isConst() ||
               Op.Inputs[1].Offset != 0))
            return std::nullopt;

          const NdVar Input = Op.Inputs[0];
          if (Input.isConst()) {
            if (Source)
              *Source = {RecordOps[I].Output, RecordOps[I].Addr,
                         RecordOps[I].Seq, /*DefinedAtPoint=*/true};
            return maskToSize(Input.Offset, Requested.Size);
          }
          if (!Input.isReg())
            return std::nullopt;

          int InputDef = -1;
          for (int J = Def - 1; J >= 0; --J)
            if (sameValue(RecordOps[J].Output, Input)) {
              InputDef = J;
              break;
            }
          if (InputDef < 0)
            return std::nullopt;
          Value = Input;
          Def = InputDef;
        }
        ClampAnalysisIncomplete = true;
        return std::nullopt;
      }
    }
    return std::nullopt;
  };
  auto localConstant =
      [&](NdVar Value, int From, va_t Cutoff,
          JumpTableValueOccurrence *Source) -> std::optional<uint64_t> {
    if (Source)
      *Source = {};
    for (size_t Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!consumeEvidence())
        return std::nullopt;
      if (Value.isConst())
        return maskToSize(Value.Offset, Value.Size);
      if (!Value.isReg() && !Value.isTemp())
        return std::nullopt;
      const std::optional<int> Def = reachingDef(Value, From);
      if (!Def) {
        if (ClampAnalysisIncomplete || !Value.isReg())
          return std::nullopt;
        return prefixScalarConstant(Value, Cutoff, Source);
      }
      const LowOp &Op = Ops[*Def];
      if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT) &&
          Op.NumInputs >= 1) {
        Value = Op.Inputs[0];
        From = *Def - 1;
        continue;
      }
      if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
          Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0) {
        Value = Op.Inputs[0];
        From = *Def - 1;
        continue;
      }
      return std::nullopt;
    }
    ClampAnalysisIncomplete = true;
    return std::nullopt;
  };
  auto operationDef = [&](NdVar Value, int From) -> std::optional<int> {
    for (size_t Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      const std::optional<int> Def = reachingDef(Value, From);
      if (!Def)
        return std::nullopt;
      const LowOp &Op = Ops[*Def];
      if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
           Op.Opcode == NdOp::INT_SEXT) &&
          Op.NumInputs >= 1) {
        Value = Op.Inputs[0];
        From = *Def - 1;
        continue;
      }
      if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
          Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0) {
        Value = Op.Inputs[0];
        From = *Def - 1;
        continue;
      }
      return Def;
    }
    ClampAnalysisIncomplete = true;
    return std::nullopt;
  };
  auto narrowLoadDef = [&](NdVar Value, int From, int &LoadIndex,
                           uint16_t &LoadWidth) {
    const std::optional<int> Def = operationDef(Value, From);
    if (!Def || Ops[*Def].Opcode != NdOp::LOAD)
      return false;
    const uint16_t Width = Ops[*Def].Output.Size;
    if (Width != 1 && Width != 2)
      return false;
    LoadIndex = *Def;
    LoadWidth = Width;
    return true;
  };
  auto structurallyReachesNarrowLoad = [&](NdVar Value, int From) {
    for (size_t Step = 0; Step < Ops.size(); ++Step) {
      const std::optional<int> Def = reachingDef(Value, From);
      if (!Def)
        return false;
      const LowOp &Producer = Ops[*Def];
      if (Producer.Opcode == NdOp::LOAD)
        return Producer.Output.Size == 1 || Producer.Output.Size == 2;
      if ((Producer.Opcode != NdOp::COPY && Producer.Opcode != NdOp::INT_ZEXT &&
           Producer.Opcode != NdOp::INT_SEXT &&
           Producer.Opcode != NdOp::SUBBYTES) ||
          Producer.NumInputs < 1)
        return false;
      if (Producer.Opcode == NdOp::SUBBYTES &&
          (Producer.NumInputs < 2 || !Producer.Inputs[1].isConst() ||
           Producer.Inputs[1].Offset != 0))
        return false;
      Value = Producer.Inputs[0];
      From = *Def - 1;
    }
    return false;
  };

  struct ClampedIndexSelect {
    bool Present = false;
    bool DirectSelect = false;
    uint64_t FallbackSlot = 0;
    uint64_t Bitmap = 0;
    uint16_t BitmapBits = 0;
    bool FallbackWhenBitSet = false;
    uint64_t PredicateIndexReg = InvalidVA;
    int PredicateIndexBefore = -1;
    NdVar PredicateIndexValue = {};
    va_t PredicateIndexUseAddr = InvalidVA;
    int PredicateIndexUseSeq = -1;
    NdVar FallbackValue = {};
    va_t FallbackUseAddr = InvalidVA;
    int FallbackUseSeq = -1;
    JumpTableValueOccurrence FallbackSource = {};
    NdVar BitmapValue = {};
    va_t BitmapUseAddr = InvalidVA;
    int BitmapUseSeq = -1;
    JumpTableValueOccurrence BitmapSource = {};
  } Clamp;
  bool SawClampedIndexSkeleton = false;

  auto completeClampedIndex =
      [&](int CandidateLoad, uint16_t CandidateWidth, bool FallbackOnPredicate,
          uint64_t Fallback, const NdVar &FallbackValue, va_t FallbackUseAddr,
          int FallbackUseSeq, const JumpTableValueOccurrence &FallbackSource,
          NdVar ConditionValue, int ConditionBefore, int &LoadIndex,
          uint16_t &LoadWidth) {
        bool Negated = false;
        bool FoundComparison = false;
        std::optional<int> Condition;
        for (size_t Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
          Condition = operationDef(ConditionValue, ConditionBefore);
          if (!Condition)
            return false;
          const LowOp &Producer = Ops[*Condition];
          if (Producer.Opcode != NdOp::BOOL_NOT) {
            FoundComparison = true;
            break;
          }
          if (Producer.NumInputs < 1)
            return false;
          Negated = !Negated;
          ConditionValue = Producer.Inputs[0];
          ConditionBefore = *Condition - 1;
        }
        if (!Condition || !FoundComparison) {
          ClampAnalysisIncomplete = true;
          return false;
        }
        const LowOp &Compare = Ops[*Condition];
        if ((Compare.Opcode != NdOp::INT_NOTEQUAL &&
             Compare.Opcode != NdOp::INT_EQUAL) ||
            Compare.NumInputs < 2)
          return false;
        int ZeroSide = -1;
        for (int Side = 0; Side < 2; ++Side) {
          const std::optional<uint64_t> Constant = localConstant(
              Compare.Inputs[Side], *Condition - 1, Compare.Addr, nullptr);
          if (Constant && *Constant == 0) {
            ZeroSide = Side;
            break;
          }
        }
        if (ZeroSide < 0)
          return false;
        const bool PredicateWhenBitSet =
            (Compare.Opcode == NdOp::INT_NOTEQUAL) != Negated;

        const std::optional<int> BitAnd =
            operationDef(Compare.Inputs[1 - ZeroSide], *Condition - 1);
        if (!BitAnd || Ops[*BitAnd].Opcode != NdOp::INT_AND ||
            Ops[*BitAnd].NumInputs < 2)
          return false;
        int OneSide = -1;
        for (int Side = 0; Side < 2; ++Side) {
          const std::optional<uint64_t> Constant =
              localConstant(Ops[*BitAnd].Inputs[Side], *BitAnd - 1,
                            Ops[*BitAnd].Addr, nullptr);
          if (Constant && *Constant == 1) {
            OneSide = Side;
            break;
          }
        }
        if (OneSide < 0)
          return false;

        const std::optional<int> Shift =
            operationDef(Ops[*BitAnd].Inputs[1 - OneSide], *BitAnd - 1);
        if (!Shift || Ops[*Shift].Opcode != NdOp::INT_RIGHT ||
            Ops[*Shift].NumInputs < 2 || Ops[*Shift].Inputs[0].Size == 0 ||
            Ops[*Shift].Inputs[0].Size > sizeof(uint64_t))
          return false;
        const uint16_t BitmapBits = Ops[*Shift].Inputs[0].Size * CHAR_BIT;
        JumpTableValueOccurrence BitmapSource;
        const std::optional<uint64_t> Bitmap = localConstant(
            Ops[*Shift].Inputs[0], *Shift - 1, Ops[*Shift].Addr, &BitmapSource);
        const std::optional<int> ShiftMask =
            operationDef(Ops[*Shift].Inputs[1], *Shift - 1);
        if (!Bitmap || !ShiftMask || Ops[*ShiftMask].Opcode != NdOp::INT_AND ||
            Ops[*ShiftMask].NumInputs < 2)
          return false;
        int ModuloMaskSide = -1;
        for (int Side = 0; Side < 2; ++Side) {
          const std::optional<uint64_t> Constant =
              localConstant(Ops[*ShiftMask].Inputs[Side], *ShiftMask - 1,
                            Ops[*ShiftMask].Addr, nullptr);
          // x86 variable shifts expose the architectural low-bit mask (31 or
          // 63), while ARM exposes its low-byte register-shift mask (255).
          // Either is identity on the exact outer object domain proved below;
          // require every bitmap-index bit to survive rather than accepting an
          // arbitrary wider mask.
          if (Constant &&
              (*Constant & uint64_t(BitmapBits - 1)) == BitmapBits - 1) {
            ModuloMaskSide = Side;
            break;
          }
        }
        if (ModuloMaskSide < 0)
          return false;
        const NdVar PredicateIndex = Ops[*ShiftMask].Inputs[1 - ModuloMaskSide];
        if (!consumeProduct(Ops.size(), limits::kMaxQuasiCopyDepth))
          return false;
        const uint64_t PredicateIndexReg =
            traceToRegister(Ops, *ShiftMask - 1, PredicateIndex);
        if (PredicateIndexReg == InvalidVA)
          return false;

        LoadIndex = CandidateLoad;
        LoadWidth = CandidateWidth;
        Clamp.Present = true;
        Clamp.FallbackSlot = Fallback;
        Clamp.Bitmap = *Bitmap;
        Clamp.BitmapBits = BitmapBits;
        Clamp.FallbackWhenBitSet = FallbackOnPredicate == PredicateWhenBitSet;
        Clamp.PredicateIndexReg = PredicateIndexReg;
        Clamp.PredicateIndexBefore = *ShiftMask - 1;
        Clamp.PredicateIndexValue = PredicateIndex;
        Clamp.PredicateIndexUseAddr = Ops[*ShiftMask].Addr;
        Clamp.PredicateIndexUseSeq = Ops[*ShiftMask].Seq;
        Clamp.FallbackValue = FallbackValue;
        Clamp.FallbackUseAddr = FallbackUseAddr;
        Clamp.FallbackUseSeq = FallbackUseSeq;
        Clamp.FallbackSource = FallbackSource;
        Clamp.BitmapValue = Ops[*Shift].Inputs[0];
        Clamp.BitmapUseAddr = Ops[*Shift].Addr;
        Clamp.BitmapUseSeq = Ops[*Shift].Seq;
        Clamp.BitmapSource = BitmapSource;
        return true;
      };

  auto parseClampedIndex = [&](int OrIndex, int &LoadIndex,
                               uint16_t &LoadWidth) {
    const LowOp &Or = Ops[OrIndex];
    if (Or.Opcode == NdOp::SELECT && Or.NumInputs >= 3) {
      int TrueLoad = -1;
      int FalseLoad = -1;
      uint16_t TrueWidth = 0;
      uint16_t FalseWidth = 0;
      const bool TrueIsLoad =
          narrowLoadDef(Or.Inputs[1], OrIndex - 1, TrueLoad, TrueWidth);
      const bool FalseIsLoad =
          narrowLoadDef(Or.Inputs[2], OrIndex - 1, FalseLoad, FalseWidth);
      if (ClampAnalysisIncomplete) {
        const bool TrueReachesLoad =
            structurallyReachesNarrowLoad(Or.Inputs[1], OrIndex - 1);
        const bool FalseReachesLoad =
            structurallyReachesNarrowLoad(Or.Inputs[2], OrIndex - 1);
        SawClampedIndexSkeleton = TrueReachesLoad != FalseReachesLoad;
        return false;
      }
      if (TrueIsLoad == FalseIsLoad)
        return false;
      SawClampedIndexSkeleton = true;

      JumpTableValueOccurrence TrueSource;
      JumpTableValueOccurrence FalseSource;
      const std::optional<uint64_t> TrueConstant =
          TrueIsLoad
              ? std::nullopt
              : localConstant(Or.Inputs[1], OrIndex - 1, Or.Addr, &TrueSource);
      const std::optional<uint64_t> FalseConstant =
          FalseIsLoad
              ? std::nullopt
              : localConstant(Or.Inputs[2], OrIndex - 1, Or.Addr, &FalseSource);
      if (TrueConstant && FalseIsLoad) {
        const bool Complete = completeClampedIndex(
            FalseLoad, FalseWidth, /*FallbackOnPredicate=*/true, *TrueConstant,
            Or.Inputs[1], Or.Addr, Or.Seq, TrueSource, Or.Inputs[0],
            OrIndex - 1, LoadIndex, LoadWidth);
        Clamp.DirectSelect = Complete;
        return Complete;
      }
      if (FalseConstant && TrueIsLoad) {
        const bool Complete = completeClampedIndex(
            TrueLoad, TrueWidth, /*FallbackOnPredicate=*/false, *FalseConstant,
            Or.Inputs[2], Or.Addr, Or.Seq, FalseSource, Or.Inputs[0],
            OrIndex - 1, LoadIndex, LoadWidth);
        Clamp.DirectSelect = Complete;
        return Complete;
      }
      return false;
    }
    if (Or.Opcode != NdOp::INT_OR || Or.NumInputs < 2)
      return false;
    for (int PositiveArm = 0; PositiveArm < 2; ++PositiveArm) {
      const std::optional<int> PositiveAnd =
          reachingDef(Or.Inputs[PositiveArm], OrIndex - 1);
      const std::optional<int> NegativeAnd =
          reachingDef(Or.Inputs[1 - PositiveArm], OrIndex - 1);
      if (!PositiveAnd || !NegativeAnd ||
          Ops[*PositiveAnd].Opcode != NdOp::INT_AND ||
          Ops[*NegativeAnd].Opcode != NdOp::INT_AND ||
          Ops[*PositiveAnd].NumInputs < 2 || Ops[*NegativeAnd].NumInputs < 2)
        continue;

      for (int PositiveMaskSide = 0; PositiveMaskSide < 2; ++PositiveMaskSide) {
        const NdVar PositiveMask = Ops[*PositiveAnd].Inputs[PositiveMaskSide];
        const std::optional<int> Negate =
            reachingDef(PositiveMask, *PositiveAnd - 1);
        if (!Negate || Ops[*Negate].Opcode != NdOp::INT_NEG2 ||
            Ops[*Negate].NumInputs < 1)
          continue;

        int NegativeMaskSide = -1;
        for (int Side = 0; Side < 2; ++Side) {
          const std::optional<int> Not =
              reachingDef(Ops[*NegativeAnd].Inputs[Side], *NegativeAnd - 1);
          if (Not && Ops[*Not].Opcode == NdOp::INT_NOT &&
              Ops[*Not].NumInputs >= 1 &&
              sameValue(Ops[*Not].Inputs[0], PositiveMask)) {
            NegativeMaskSide = Side;
            break;
          }
        }
        if (NegativeMaskSide < 0)
          continue;
        const NdVar PositiveData =
            Ops[*PositiveAnd].Inputs[1 - PositiveMaskSide];
        const NdVar NegativeData =
            Ops[*NegativeAnd].Inputs[1 - NegativeMaskSide];
        int PositiveLoad = -1;
        int NegativeLoad = -1;
        uint16_t PositiveWidth = 0;
        uint16_t NegativeWidth = 0;
        const bool PositiveIsLoad = narrowLoadDef(
            PositiveData, *PositiveAnd - 1, PositiveLoad, PositiveWidth);
        const bool NegativeIsLoad = narrowLoadDef(
            NegativeData, *NegativeAnd - 1, NegativeLoad, NegativeWidth);
        if (ClampAnalysisIncomplete) {
          // The bounded semantic trace may stop before reaching a genuine
          // narrow LOAD.  Continue only as a budgeted structural inventory so
          // a later relocation-identity probe can distinguish an incomplete
          // composite from an ordinary masked callback.  This scan grants no
          // table authority and accepts exactly one narrow arm.
          const bool PositiveReachesLoad =
              structurallyReachesNarrowLoad(PositiveData, *PositiveAnd - 1);
          const bool NegativeReachesLoad =
              structurallyReachesNarrowLoad(NegativeData, *NegativeAnd - 1);
          SawClampedIndexSkeleton = PositiveReachesLoad != NegativeReachesLoad;
          return false;
        }
        if (PositiveIsLoad == NegativeIsLoad)
          continue;
        SawClampedIndexSkeleton = true;
        LoadIndex = PositiveIsLoad ? PositiveLoad : NegativeLoad;
        LoadWidth = PositiveIsLoad ? PositiveWidth : NegativeWidth;

        JumpTableValueOccurrence PositiveSource;
        JumpTableValueOccurrence NegativeSource;
        const std::optional<uint64_t> PositiveConstant =
            PositiveIsLoad
                ? std::nullopt
                : localConstant(PositiveData, *PositiveAnd - 1,
                                Ops[*PositiveAnd].Addr, &PositiveSource);
        const std::optional<uint64_t> NegativeConstant =
            NegativeIsLoad
                ? std::nullopt
                : localConstant(NegativeData, *NegativeAnd - 1,
                                Ops[*NegativeAnd].Addr, &NegativeSource);
        int CandidateLoad = -1;
        uint16_t CandidateWidth = 0;
        bool FallbackOnPredicate = false;
        uint64_t Fallback = 0;
        NdVar FallbackValue;
        va_t FallbackUseAddr = InvalidVA;
        int FallbackUseSeq = -1;
        if (PositiveConstant && NegativeIsLoad) {
          CandidateLoad = NegativeLoad;
          CandidateWidth = NegativeWidth;
          FallbackOnPredicate = true;
          Fallback = *PositiveConstant;
          FallbackValue = PositiveData;
          FallbackUseAddr = Ops[*PositiveAnd].Addr;
          FallbackUseSeq = Ops[*PositiveAnd].Seq;
          Clamp.FallbackSource = PositiveSource;
        } else if (NegativeConstant && PositiveIsLoad) {
          CandidateLoad = PositiveLoad;
          CandidateWidth = PositiveWidth;
          FallbackOnPredicate = false;
          Fallback = *NegativeConstant;
          FallbackValue = NegativeData;
          FallbackUseAddr = Ops[*NegativeAnd].Addr;
          FallbackUseSeq = Ops[*NegativeAnd].Seq;
          Clamp.FallbackSource = NegativeSource;
        } else {
          continue;
        }

        if (completeClampedIndex(
                CandidateLoad, CandidateWidth, FallbackOnPredicate, Fallback,
                FallbackValue, FallbackUseAddr, FallbackUseSeq,
                PositiveIsLoad ? NegativeSource : PositiveSource,
                Ops[*Negate].Inputs[0], *Negate - 1, LoadIndex, LoadWidth))
          return true;
      }
    }
    return false;
  };

  // collectPathOps deliberately follows a single-predecessor chain so the
  // classic two-level shape may place its narrow LOAD before the dispatch
  // block.  A clamped index is different: the narrow LOAD, membership test,
  // SELECT/mask blend, and address-table LOAD form one instruction-local
  // dataflow.
  // Parse that suffix in isolation so a newly decoded predecessor cannot
  // contribute an unrelated definition of an architecturally reused register
  // or per-instruction temporary on the next fixed-point replay.
  size_t LocalBegin = Ops.size();
  if (!consumeEvidence(Ops.size()))
    return false;
  for (size_t I = Ops.size(); I > 0; --I) {
    const va_t Addr = Ops[I - 1].Addr;
    if (Addr >= BlkStart && Addr <= Rec.Addr) {
      LocalBegin = I - 1;
      continue;
    }
    if (LocalBegin != Ops.size())
      break;
  }
  if (LocalBegin != Ops.size()) {
    const size_t LocalCount = Ops.size() - LocalBegin;
    if (LocalCount > (std::numeric_limits<size_t>::max() - 2) / 3 ||
        !consumeEvidence(LocalCount * 3 + 2))
      return false;
    std::vector<LowOp> LocalOps(Ops.begin() + LocalBegin, Ops.end());
    bool HasLocalClampedIndex = false;
    // The suffix probe may ask analyzeTableLoadAddr about each pointer-width
    // LOAD.  That helper performs several depth-bounded reaching-definition
    // and register traces; inventory the eligible loads and reserve their full
    // per-candidate search envelope before entering the first raw scan.
    // Count the pointer-width LOAD candidates before reserving their nested
    // address-analysis work.  Charging LocalCount candidates would turn an
    // unrelated vector-heavy block into a quadratic false exhaustion even
    // when only a handful of operations can enter analyzeTableLoadAddr.
    if (!consumeProduct(LocalCount, 2))
      return false;
    size_t LocalPointerLoads = 0;
    for (const LowOp &Op : LocalOps)
      LocalPointerLoads +=
          Op.Opcode == NdOp::LOAD &&
          Op.Output.Size == Img.getPointerSize() && Op.NumInputs >= 1;
    if (!consumeFactorProduct({LocalPointerLoads, LocalCount,
                               size_t(limits::kMaxQuasiCopyDepth), 16}))
      return false;
    for (int I = static_cast<int>(LocalOps.size()) - 1;
         I >= 0 && !HasLocalClampedIndex; --I) {
      const LowOp &Load = LocalOps[I];
      if (Load.Opcode != NdOp::LOAD ||
          Load.Output.Size != Img.getPointerSize() || Load.NumInputs < 1)
        continue;
      const NdVar &Address = Load.Inputs[Load.NumInputs >= 2 ? 1 : 0];
      uint64_t BaseReg = InvalidVA;
      uint64_t IndexReg = InvalidVA;
      uint64_t Disp = 0;
      bool Scaled = false;
      if (!analyzeTableLoadAddr(LocalOps, I - 1, Address, BaseReg, IndexReg,
                                Scaled, Disp) ||
          !Scaled || IndexReg == InvalidVA)
        continue;
      NdVar Value = NdVar::reg(IndexReg, 8);
      int From = I - 1;
      for (size_t Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
        const int Def = reachingDefIdx(LocalOps, From, Value);
        if (Def < 0)
          break;
        const LowOp &Producer = LocalOps[Def];
        if (Producer.Opcode == NdOp::INT_OR ||
            Producer.Opcode == NdOp::SELECT) {
          HasLocalClampedIndex = true;
          break;
        }
        if ((Producer.Opcode == NdOp::COPY ||
             Producer.Opcode == NdOp::INT_ZEXT ||
             Producer.Opcode == NdOp::INT_SEXT) &&
            Producer.NumInputs >= 1) {
          Value = Producer.Inputs[0];
          From = Def - 1;
          continue;
        }
        if (Producer.Opcode == NdOp::SUBBYTES && Producer.NumInputs >= 2 &&
            Producer.Inputs[1].isConst() && Producer.Inputs[1].Offset == 0) {
          Value = Producer.Inputs[0];
          From = Def - 1;
          continue;
        }
        break;
      }
    }
    if (HasLocalClampedIndex)
      Ops = std::move(LocalOps);
  }

  // 1) Locate the address-table (jmptab) load: the last pointer-width scaled
  //    load feeding the branch, `jmptab + entryIdx*W2`.
  uint64_t JmpBaseReg = InvalidVA, EntryIdxReg = InvalidVA;
  uint16_t W2 = 0;
  int JmpLoadIdx = -1;
  {
    // The full path can contain thousands of SIMD/arithmetic operations but
    // only 4/8-byte LOADs can be the address-table access.  Inventory that
    // exact candidate count first, then reserve one bounded address-analysis
    // envelope per candidate before the first analysis call.
    if (!consumeProduct(Ops.size(), 2))
      return false;
    size_t AddressTableLoads = 0;
    for (const LowOp &Op : Ops)
      AddressTableLoads += Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1 &&
                           (Op.Output.Size == 4 || Op.Output.Size == 8);
    if (!consumeFactorProduct({AddressTableLoads, Ops.size(),
                               size_t(limits::kMaxQuasiCopyDepth), 16}))
      return false;
    uint64_t Disp = 0;
    bool Scaled = false;
    for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I) {
      const LowOp &L = Ops[I];
      if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
        continue;
      uint16_t W = L.Output.Size;
      if (W != 4 && W != 8)
        continue;
      const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
      if (!AddrV.isReg() && !AddrV.isTemp())
        continue;
      if (analyzeTableLoadAddr(Ops, I - 1, AddrV, JmpBaseReg, EntryIdxReg,
                               Scaled, Disp) &&
          Scaled) {
        W2 = W;
        JmpLoadIdx = I;
        break;
      }
    }
  }
  if (JmpLoadIdx < 0 || EntryIdxReg == InvalidVA || W2 == 0)
    return false;

  // 2) The jmptab index must itself be the *value loaded* by a compact
  //    byte/halfword index-table load — trace it (through value-preserving
  //    reshapes only) to a LOAD of width 1 or 2.  Anything else (arithmetic on
  //    the index, a plain register) is not a two-level table.
  int IdxLoadIdx = -1;
  uint16_t W1 = 0;
  {
    NdVar V = NdVar::reg(EntryIdxReg, 8);
    int From = JmpLoadIdx - 1;
    const std::optional<int> Def = operationDef(V, From);
    if (Def && Ops[*Def].Opcode == NdOp::LOAD) {
      W1 = Ops[*Def].Output.Size;
      IdxLoadIdx = *Def;
    } else if (Def && (Ops[*Def].Opcode == NdOp::INT_OR ||
                       Ops[*Def].Opcode == NdOp::SELECT)) {
      parseClampedIndex(*Def, IdxLoadIdx, W1);
    }
  }
  // A byte/halfword index table is the hallmark of the compaction; a wider
  // "index" is indistinguishable from an ordinary single-level table entry.
  if (IdxLoadIdx < 0 || (W1 != 1 && W1 != 2)) {
    // A depth-limited clamped selection is not yet a composite certificate.
    // Before preserving it as incomplete, authenticate the already-located
    // inner pointer LOAD against an actual code-relocation prefix.  Raw masked
    // callback tables without relocation identity therefore retain ordinary
    // tail-call lowering, while a genuine two-level table cannot fall through
    // to publishing the inner table on its intermediate index.
    if (SawClampedIndexSkeleton && ClampAnalysisIncomplete && JmpLoadIdx >= 0 &&
        JmpBaseReg != InvalidVA && W2 != 0) {
      const va_t FoldAt = Ops[JmpLoadIdx].Addr;
      const std::optional<uint64_t> Folded =
          foldRegConstant(Img, Rec, JmpBaseReg, FoldAt, [&](size_t Amount) {
            return consumeEvidence(Amount);
          });
      if (Folded && consumeEvidence(Img.Segments.size()) &&
          Img.getSegmentFor(*Folded)) {
        auto hasRelocationPrefix = [&](const std::set<uint64_t> &Slots) {
          const size_t Lookup = orderedLookupWork(Slots.size());
          va_t Slot = *Folded;
          for (uint32_t I = 0; I < limits::kMinJumpTableEntries; ++I) {
            if (!consumeEvidence(Lookup) || !consumeEvidence(1) ||
                !Slots.count(Slot))
              return false;
            if (I + 1 != limits::kMinJumpTableEntries) {
              if (W2 > InvalidVA - Slot)
                return false;
              Slot += W2;
            }
          }
          return true;
        };
        if (hasRelocationPrefix(Img.CodePtrRelocSlots) ||
            hasRelocationPrefix(Img.RelCodeRelocSlots)) {
          Info.CompositeShapeClaimed = true;
          Info.IncompleteGuardDomain = true;
        }
      }
    }
    return false;
  }

  // 3) Decompose the index-table load address into idxtab base + switch var.
  va_t IdxTab = 0;
  uint64_t SwitchIdxReg = InvalidVA;
  NdVar SwitchIndexValue;
  va_t SwitchIndexUseAddr = InvalidVA;
  int SwitchIndexUseSeq = -1;
  uint32_t IdxScale = 1;
  // decomposeIndexTableLoadAddr may test both ADD operands before finding the
  // mapped base.  Include every nested getSegmentFor/getSectionFor performed
  // by mappedObjectOwnerEnd and isCodeAddress, plus the sectionless-symbol
  // fallback.  The fold emulation itself is charged through ConsumeWork.
  if (!consumeProducts(
          {{Img.Segments.size(), 12},
           {Img.Sections.size(), 6},
           {Img.Symbols.size(), 1}}))
    return false;
  if (!decomposeIndexTableLoadAddr(Img, Rec, Ops, IdxLoadIdx, W1, IdxTab,
                                   SwitchIdxReg, IdxScale, &SwitchIndexValue,
                                   &SwitchIndexUseAddr, &SwitchIndexUseSeq,
                                   [&](size_t Amount) {
                                     return consumeEvidence(Amount);
                                   }))
    return false;

  // 4) Fold the address-table base and confirm it is distinct from idxtab.
  va_t JmpTab = 0;
  {
    va_t FoldAt = Ops[JmpLoadIdx].Addr;
    auto Folded = foldRegConstant(
        Img, Rec, JmpBaseReg, FoldAt,
        [&](size_t Amount) { return consumeEvidence(Amount); });
    if (!Folded || !Img.getSegmentFor(*Folded))
      return false;
    JmpTab = *Folded;
  }
  if (JmpTab == IdxTab)
    return false;
  const auto *JmpSeg = Img.getSegmentFor(JmpTab);
  const auto *IdxSeg = Img.getSegmentFor(IdxTab);
  if (!JmpSeg || JmpSeg->Data.empty() || !IdxSeg || IdxSeg->Data.empty())
    return false;
  const std::optional<va_t> JmpOwnerEnd = Img.mappedObjectOwnerEnd(JmpTab);
  const std::optional<va_t> IdxOwnerEnd = Img.mappedObjectOwnerEnd(IdxTab);
  if (!JmpOwnerEnd || !IdxOwnerEnd || *JmpOwnerEnd <= JmpTab ||
      *IdxOwnerEnd <= IdxTab)
    return false;
  // The index table lives in read-only data; a writable/executable "idxtab"
  // would not be a compiler-emitted constant index table.
  if (IdxSeg->isWritable() || Img.isCodeAddress(IdxTab))
    return false;
  // 5) The address table's signature: a run of loader-applied code-pointer
  //    relocations (absolute) or PC-relative-to-code relocations (relative).
  //    The bounded run length M is authenticated physical capacity, and every
  //    idxtab byte must be < M — the constraint that distinguishes a genuine
  //    two-level table from an unrelated pair of chained loads.
  const uint64_t OwnerEntries = (*JmpOwnerEnd - JmpTab) / W2;
  bool Relative = false;
  const size_t CodePtrRelocLookup =
      orderedLookupWork(Img.CodePtrRelocSlots.size()) + 1;
  const auto AbsoluteRun =
      relocRunIn(Img.CodePtrRelocSlots, JmpTab, W2, OwnerEntries, [&] {
        return consumeEvidence(CodePtrRelocLookup);
      });
  if (!AbsoluteRun)
    return false;
  uint32_t M = AbsoluteRun->Count;
  bool RelocationIdentityComplete = AbsoluteRun->Complete;
  if (M < limits::kMinJumpTableEntries) {
    const size_t RelCodeRelocLookup =
        orderedLookupWork(Img.RelCodeRelocSlots.size()) + 1;
    const auto RelativeRun =
        relocRunIn(Img.RelCodeRelocSlots, JmpTab, W2, OwnerEntries, [&] {
          return consumeEvidence(RelCodeRelocLookup);
        });
    if (!RelativeRun)
      return false;
    const uint32_t RM = RelativeRun->Count;
    if (RM >= limits::kMinJumpTableEntries) {
      M = RM;
      Relative = true;
      RelocationIdentityComplete = RelativeRun->Complete;
    }
  }
  if (M < limits::kMinJumpTableEntries)
    return false;
  M = static_cast<uint32_t>(std::min<uint64_t>(
      {M, std::numeric_limits<uint32_t>::max(), OwnerEntries}));
  if (M < limits::kMinJumpTableEntries)
    return false;
  if (!RelocationIdentityComplete) {
    Info.CompositeShapeClaimed = true;
    Info.IncompleteGuardDomain = true;
    return false;
  }
  // Only a relocation-backed inner table with a valid physical capacity is a
  // distinguishing composite candidate.  Earlier resource failures may still
  // describe an ordinary callback table and must not suppress tail-call
  // recovery.  From here, however, incomplete bounded evidence must preserve
  // the machine branch rather than fall through to that generic strategy.
  ClaimCompositeOnExhaustion = true;

  // Flatten the whole function prefix so the outer range guard and any
  // comparison of the loaded index value are both visible.
  if (!consumeProducts(
          {{1, orderedLookupWork(Insns.size())}, {Insns.size(), 1}}))
    return false;
  size_t PrefixRecordCount = 0;
  size_t PrefixOpCount = 0;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It) {
    ++PrefixRecordCount;
    if (It->second.Ops.size() >
        std::numeric_limits<size_t>::max() - PrefixOpCount) {
      consumeEvidence(std::numeric_limits<size_t>::max());
      return false;
    }
    PrefixOpCount += It->second.Ops.size();
  }
  if (!consumeProducts(
          {{1, orderedLookupWork(Insns.size())},
           {PrefixRecordCount, 1},
           {PrefixOpCount, 3},
           {1, 2}}))
    return false;
  std::vector<LowOp> Pre;
  Pre.reserve(PrefixOpCount);
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    Pre.insert(Pre.end(), It->second.Ops.begin(), It->second.Ops.end());

  // Locate the index-table load in the function-prefix ops (by address and
  // output nd-var) so the discriminator below can reason about its result.
  if (!consumeProducts({{Pre.size(), 2}}) ||
      !consumeFactorProduct({Pre.size(), Pre.size(),
                             size_t(limits::kMaxQuasiCopyDepth), 2}))
    return false;
  int IdxLoadInPre = -1;
  {
    va_t L1Addr = Ops[IdxLoadIdx].Addr;
    const NdVar &L1Out = Ops[IdxLoadIdx].Output;
    for (int I = 0; I < static_cast<int>(Pre.size()); ++I)
      if (Pre[I].Opcode == NdOp::LOAD && Pre[I].Addr == L1Addr &&
          Pre[I].Output.Space == L1Out.Space &&
          Pre[I].Output.Offset == L1Out.Offset &&
          Pre[I].Output.Size == L1Out.Size &&
          Pre[I].Output.isTemp() == L1Out.isTemp())
        IdxLoadInPre = I; // last match at that address wins
  }

  // Discriminator — distinguish a genuine two-level index table from an
  // ordinary `switch(user_array[i])`, which lowers to the *identical* shape
  // (load a value, then index the compiler's jump table by it).  In the latter
  // the loaded value IS the switch variable and is range-guarded as the switch
  // condition (`cmp k, hi; ja default`); dispatching on the outer array index
  // would be wrong.  A compiler-generated index table's value, by contrast, is
  // an opaque index used *only* to address the address table and is never
  // compared.  So bail when the idxtab-loaded value reaches a constant
  // comparison: that marks it as the real switch variable (single-level).
  if (IdxLoadInPre >= 0) {
    auto tracesToIdxLoad = [&](NdVar V, int From) -> bool {
      for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
        if (!V.isReg() && !V.isTemp())
          return false;
        int D = reachingDefIdx(Pre, From, V);
        if (D < 0)
          return false;
        if (D == IdxLoadInPre)
          return true;
        const LowOp &O = Pre[D];
        if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
             O.Opcode == NdOp::INT_SEXT) &&
            O.NumInputs >= 1 && (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
          V = O.Inputs[0];
          From = D - 1;
          continue;
        }
        if (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
            O.Inputs[1].isConst() && O.Inputs[1].Offset == 0 &&
            (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
          V = O.Inputs[0];
          From = D - 1;
          continue;
        }
        return false;
      }
      return false;
    };
    for (int I = 0; I < static_cast<int>(Pre.size()); ++I) {
      const LowOp &Op = Pre[I];
      bool IsCompare =
          Op.Opcode == NdOp::INT_LESS || Op.Opcode == NdOp::INT_SLESS ||
          Op.Opcode == NdOp::INT_LESSEQUAL ||
          Op.Opcode == NdOp::INT_SLESSEQUAL || Op.Opcode == NdOp::INT_EQUAL ||
          Op.Opcode == NdOp::INT_NOTEQUAL || Op.Opcode == NdOp::INT_SUB;
      if (!IsCompare || Op.NumInputs < 2)
        continue;
      int CW = Op.Inputs[1].isConst() ? 1 : (Op.Inputs[0].isConst() ? 0 : -1);
      if (CW < 0)
        continue;
      if (tracesToIdxLoad(Op.Inputs[1 - CW], I - 1))
        return false; // loaded value is the switch variable — single-level
    }
  }

  // Once the clamped selection, narrow outer LOAD, and both physical table
  // identities are known, unsupported or depth-limited clamp semantics are a
  // claimed-but-incomplete composite candidate.  Do not fall through to a
  // generic inner-table resolver that could convert the machine jump to CALL.
  if (SawClampedIndexSkeleton &&
      (!Clamp.Present || ClampAnalysisIncomplete)) {
    Info.CompositeShapeClaimed = true;
    Info.IncompleteGuardDomain = true;
    return false;
  }

  // The two chained physical tables, narrow outer LOAD and relocation-backed
  // inner address table are a distinguishing composite shape.  From here a
  // failed outer-domain or occurrence certificate must not fall through to a
  // generic resolver that would publish only jmptab on the intermediate index.
  Info.CompositeShapeClaimed = true;

  // 6) Bound the number of switch cases (idxtab length).  Prefer an explicit
  //    range guard on the switch variable; otherwise self-bound by the idxtab
  //    entries themselves (each must index a valid jmptab slot).
  //
  // Anchor the switch-variable trace at the index-table load: the register that
  // addresses idxtab (e.g. `rax`) is routinely *reused* after the load to hold
  // the loaded index byte's zero-extension, so tracing from the end of the op
  // list would follow that later reuse to the byte value instead of the real
  // switch variable.  Its reaching definition at the load is the true switch
  // variable (the guarded `x` copied into the address register).
  if (!consumeFactorProduct(
          {Ops.size(), size_t(limits::kMaxQuasiCopyDepth), 2}))
    return false;
  uint64_t SwitchSrc = traceRegSource(Ops, IdxLoadIdx - 1, SwitchIdxReg);
  if (SwitchSrc == InvalidVA)
    return false;
  uint16_t SelectorLaneSize = 0;
  if (Clamp.Present) {
    const uint64_t PredicateSrc =
        traceRegSource(Ops, Clamp.PredicateIndexBefore,
                       Clamp.PredicateIndexReg);
    if (PredicateSrc == InvalidVA || PredicateSrc != SwitchSrc ||
        Clamp.BitmapBits == 0 ||
        Clamp.FallbackSlot > std::numeric_limits<uint32_t>::max())
      return false;
    auto localSelectorRoot = [&](NdVar Value, int From)
        -> std::optional<std::pair<NdVar, bool>> {
      bool ZeroExtended = false;
      for (size_t Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
        if (!consumeEvidence(Ops.size()))
          return std::nullopt;
        const int Def = reachingDefIdx(Ops, From, Value);
        if (Def < 0)
          return Value.isReg()
                     ? std::optional<std::pair<NdVar, bool>>(
                           std::in_place, Value, ZeroExtended)
                     : std::nullopt;
        const LowOp &Producer = Ops[Def];
        if (Producer.NumInputs < 1)
          return std::nullopt;
        if (Producer.Opcode == NdOp::INT_ZEXT)
          ZeroExtended = true;
        else if (Producer.Opcode != NdOp::COPY &&
                 Producer.Opcode != NdOp::SUBBYTES)
          return std::nullopt;
        if (Producer.Opcode == NdOp::SUBBYTES &&
            (Producer.NumInputs < 2 || !Producer.Inputs[1].isConst() ||
             Producer.Inputs[1].Offset != 0))
          return std::nullopt;
        Value = Producer.Inputs[0];
        From = Def - 1;
      }
      ClampAnalysisIncomplete = true;
      return std::nullopt;
    };
    int SwitchUseIndex = -1;
    if (!consumeProduct(Ops.size(), 4)) {
      Info.IncompleteGuardDomain = true;
      return false;
    }
    for (int I = static_cast<int>(Ops.size()) - 1;
         I >= 0 && SwitchUseIndex < 0; --I)
      if (Ops[I].Addr == SwitchIndexUseAddr &&
          Ops[I].Seq == SwitchIndexUseSeq)
        for (uint8_t Input = 0; Input < Ops[I].NumInputs; ++Input)
          if (sameValue(Ops[I].Inputs[Input], SwitchIndexValue)) {
            SwitchUseIndex = I;
            break;
          }
    if (SwitchUseIndex < 0) {
      Info.IncompleteGuardDomain = true;
      return false;
    }
    const std::optional<std::pair<NdVar, bool>> SwitchRoot =
        localSelectorRoot(SwitchIndexValue, SwitchUseIndex - 1);
    const std::optional<std::pair<NdVar, bool>> PredicateRoot =
        localSelectorRoot(Clamp.PredicateIndexValue,
                          Clamp.PredicateIndexBefore);
    if (!SwitchRoot || !PredicateRoot || !SwitchRoot->first.isReg() ||
        !PredicateRoot->first.isReg() ||
        SwitchRoot->first.Offset != PredicateRoot->first.Offset ||
        SwitchRoot->first.Offset != SwitchSrc ||
        (SwitchRoot->first.Size != PredicateRoot->first.Size &&
         !(SwitchRoot->second &&
           SwitchRoot->first.Size < PredicateRoot->first.Size)))
    {
      Info.IncompleteGuardDomain = true;
      return false;
    }
    SelectorLaneSize = SwitchRoot->first.Size;
  }
  Info.IndexReg = SwitchSrc;
  Info.IndexValueAtUse = SwitchIndexValue;
  Info.IndexUseAddr = SwitchIndexUseAddr;
  Info.IndexUseSeq = SwitchIndexUseSeq;
  Info.TableLoadAddr = Ops[IdxLoadIdx].Addr;
  Info.TableLoadSeq = Ops[IdxLoadIdx].Seq;
  if (!consumeEvidence(5)) {
    Info.IncompleteGuardDomain = true;
    return false;
  }
  Info.TargetLoads = {{Ops[JmpLoadIdx].Output, Ops[JmpLoadIdx].Addr,
                       Ops[JmpLoadIdx].Seq, /*DefinedAtPoint=*/true}};
  // The detector deliberately runs before the resolver has an immutable proof
  // graph.  A clamped two-level shape needs point-sensitive equality for its
  // live-in bitmap/fallback values and outer selector, so the first proposal
  // stage may only claim the shape and request a replay.  Never substitute a
  // lexical constant candidate for that completed CFG proof.
  if (Clamp.Present && !JumpTableProofContextComplete) {
    Info.IncompleteGuardDomain = true;
    return false;
  }
  // The outer domain must be proved at the exact index-table LOAD use.  A
  // lexical compare on an older lifetime, or scanning until a byte happens to
  // index past jmptab, does not constrain the runtime switch value.  Reuse the
  // same CFG/lane/polarity proof as ordinary tables and require an exact bound
  // before publishing the composed target vector.
  const bool OrdinaryGuardProven =
      CandidateEvidenceBudget &&
      inferBoundsFromPreciseGuards(Rec, Info, CandidateEvidenceBudget);
  const uint32_t OrdinaryGuardBound = Info.MaxEntries;
  bool GuardProven = OrdinaryGuardProven;
  uint32_t IdxCap = static_cast<uint32_t>(std::min<uint64_t>(
      (*IdxOwnerEnd - IdxTab) / W1, std::numeric_limits<uint32_t>::max()));
  if (Clamp.Present) {
    if (!consumeEvidence(Img.Symbols.size()))
      return false;
    const uint64_t ExactObjectSize = Img.dataObjectSizeAt(IdxTab);
    const bool ExactObject =
        ExactObjectSize >= uint64_t(W1) * limits::kMinJumpTableEntries &&
        ExactObjectSize <= uint64_t(W1) * 64 && ExactObjectSize % W1 == 0 &&
        ExactObjectSize <= InvalidVA - IdxTab &&
        IdxTab + ExactObjectSize <= *IdxOwnerEnd;
    if (!ExactObject)
      return false;
    IdxCap = static_cast<uint32_t>(ExactObjectSize / W1);
    if (M > std::numeric_limits<uint64_t>::max() / W2 ||
        !consumeEvidence(Img.Symbols.size()))
      return false;
    const uint64_t ExactInnerSize = uint64_t(M) * W2;
    if (Img.dataObjectSizeAt(JmpTab) != ExactInnerSize ||
        ExactInnerSize > InvalidVA - JmpTab ||
        JmpTab + ExactInnerSize > *JmpOwnerEnd)
      return false;

    // Pay the four query objects, their nested vector ownership, and retained
    // alternatives before the first Alternatives assignment allocates.
    constexpr size_t QueryConstructionWork = 64;
    if (!consumeEvidence(QueryConstructionWork))
      return false;
    JumpTableValueQuery SameOuterSelector;
    SameOuterSelector.Candidate = Clamp.PredicateIndexValue;
    SameOuterSelector.UseAddr = Clamp.PredicateIndexUseAddr;
    SameOuterSelector.UseSeq = Clamp.PredicateIndexUseSeq;
    SameOuterSelector.Alternatives = {
        {SwitchIndexValue, SwitchIndexUseAddr, SwitchIndexUseSeq,
         /*DefinedAtPoint=*/false}};
    SameOuterSelector.Relation = JumpTableValueRelation::MustEqual;
    SameOuterSelector.AllowZeroExtension = true;

    JumpTableValueQuery InExactObject;
    InExactObject.Candidate = SwitchIndexValue;
    InExactObject.UseAddr = SwitchIndexUseAddr;
    InExactObject.UseSeq = SwitchIndexUseSeq;
    InExactObject.Relation = JumpTableValueRelation::UnsignedFeasibleSet;
    InExactObject.UnsignedUpperBound = IdxCap;

    auto constantProof = [](const NdVar &Value, va_t UseAddr, int UseSeq,
                            const JumpTableValueOccurrence &Source,
                            uint64_t Constant) {
      JumpTableValueQuery Query;
      Query.Candidate = Value;
      Query.UseAddr = UseAddr;
      Query.UseSeq = UseSeq;
      if (Source.Addr != InvalidVA && Source.DefinedAtPoint) {
        Query.Alternatives = {Source};
        Query.UseDefinedAlternativesAsOccurrenceRoots = true;
      } else {
        Query.Alternatives = {
            {NdVar::cst(Constant, Value.Size), InvalidVA, -1, false},
            {NdVar::scalar(Constant, Value.Size), InvalidVA, -1, false}};
      }
      Query.Relation = JumpTableValueRelation::MustEqual;
      return Query;
    };
    JumpTableValueQuery FallbackIsConstant =
        constantProof(Clamp.FallbackValue, Clamp.FallbackUseAddr,
                      Clamp.FallbackUseSeq, Clamp.FallbackSource,
                      Clamp.FallbackSlot);
    JumpTableValueQuery BitmapIsConstant =
        constantProof(Clamp.BitmapValue, Clamp.BitmapUseAddr,
                      Clamp.BitmapUseSeq, Clamp.BitmapSource, Clamp.Bitmap);

    std::vector<JumpTableValueQuery> Queries;
    Queries.reserve(4);
    Queries.push_back(std::move(SameOuterSelector));
    Queries.push_back(std::move(InExactObject));
    Queries.push_back(std::move(FallbackIsConstant));
    Queries.push_back(std::move(BitmapIsConstant));

    const size_t KnownEntryLookup =
        KnownFuncEntries ? orderedLookupWork(KnownFuncEntries->size()) : 0;
    const size_t RuntimeEntryLookup =
        orderedLookupWork(Img.RuntimeFunctionAddrs.size());
    const size_t VerifiedEntryLookup =
        orderedLookupWork(Img.VerifiedFunctionEntries.size());
    const size_t ImportStubLookup =
        orderedLookupWork(Img.ImportStubIndices.size());
    const size_t InsnLookup = orderedLookupWork(Insns.size());
    const size_t FragmentLookup =
        orderedLookupWork(Img.ExceptionMetadata.Functions.size());
    const size_t FragmentWorkPerEntry =
        FragmentLookup <= (std::numeric_limits<size_t>::max() - 3) / 2
            ? FragmentLookup * 2 + 3
            : std::numeric_limits<size_t>::max();
    // Decode the exact inner object only after paying the same attacker-shaped
    // loader/ownership inventories as ordinary physical target reads.  The two
    // M-element vectors below retain decoded targets and suppressible slots;
    // their allocation, elements, and eventual cleanup are all prepaid before
    // the first reserve or push.
    if (!consumeProducts({{M, 16}, {M, 6}, {1, 9}}) ||
        !consumeFactorProduct({M, Img.Symbols.size(), 4}) ||
        !consumeFactorProduct({M, Img.Segments.size(), 16}) ||
        !consumeFactorProduct({M, Img.Sections.size(), 8}) ||
        !consumeFactorProduct(
            {M, Img.ExceptionMetadata.Functions.size(),
             FragmentWorkPerEntry}) ||
        !consumeFactorProduct({M, Img.ImportStubRanges.size(), 3}) ||
        !consumeFactorProduct({M, Img.Imports.size(), 2}) ||
        !consumeFactorProduct({M, Img.KnownCodeRanges.size(), 2}) ||
        !consumeFactorProduct({M, Img.Imports.size(), Img.Segments.size(), 4}) ||
        !consumeFactorProduct({M, Img.Imports.size(), Img.Sections.size(), 4}) ||
        !consumeFactorProduct({M, RuntimeEntryLookup, 2}) ||
        !consumeFactorProduct({M, VerifiedEntryLookup, 2}) ||
        !consumeFactorProduct({M, ImportStubLookup, 2}) ||
        !consumeFactorProduct({M, InsnLookup}) ||
        !consumeFactorProduct({M, KnownEntryLookup, 2}))
      return false;
    std::vector<va_t> ProofTargets;
    ProofTargets.reserve(M);
    for (uint32_t Slot = 0; Slot < M; ++Slot) {
      const uint8_t *Entry =
          Img.readVA(JmpTab + static_cast<uint64_t>(Slot) * W2, W2);
      if (!Entry)
        return false;
      std::optional<va_t> Target = decodeTableEntry(
          Entry, W2, Relative, Relative, JmpTab,
          /*HasTargetBase=*/false, /*TargetBase=*/0, /*Scale=*/1,
          Img.getPointerSize());
      if (!Target)
        return false;
      if (!Relative)
        Target = canonicalizeAbsoluteTableCodeTarget(Img, *Target);
      if (!Target || !isValidTarget(Img, *Target, CurrentFuncEntry))
        return false;
      ProofTargets.push_back(*Target);
    }
    Info.setBaseAddr(JmpTab);
    Info.EntrySize = W2;
    Info.PhysicalCapacity = M;
    Info.RelocAbsolute = !Relative;
    Info.ExplicitTargets = std::move(ProofTargets);
    Info.StorageRanges = {
        JumpTableStorageRange{JmpTab, W2, W2, M}};
    if (!Relative) {
      Info.SuppressibleRelocationSlots.reserve(M);
      for (uint32_t Slot = 0; Slot < M; ++Slot)
        Info.SuppressibleRelocationSlots.push_back(
            JmpTab + static_cast<uint64_t>(Slot) * W2);
    }
    struct RestoreProofRoots {
      std::optional<std::set<va_t>> &Slot;
      std::optional<std::set<va_t>> Saved;
      ~RestoreProofRoots() { Slot = std::move(Saved); }
    } RestoreRoots{ActiveJumpTableProofRoots,
                   std::move(ActiveJumpTableProofRoots)};
    std::optional<std::set<va_t>> CandidateRoots = budgetedProofRoots(Info);
    if (!CandidateRoots) {
      Info.IncompleteGuardDomain = true;
      return false;
    }
    ActiveJumpTableProofRoots = std::move(*CandidateRoots);
    bool Complete = false;
    std::vector<bool> QueryComplete;
    std::vector<uint64_t> FeasibleMasks;
    const std::vector<va_t> NoCandidateTargets;
    const std::vector<bool> Results = tableValuesMatchAtUses(
        Queries, &Complete, &QueryComplete, Rec.Addr, &NoCandidateTargets,
        CandidateEvidenceBudget, 0, nullptr, &FeasibleMasks);
    const bool EntryProofComplete =
        Complete && QueryComplete.size() == Queries.size() &&
        Results.size() == Queries.size() &&
        FeasibleMasks.size() == Queries.size() &&
        std::all_of(QueryComplete.begin(), QueryComplete.end(),
                    [](bool Value) { return Value; });
    const bool EntrySemanticsProven =
        EntryProofComplete && Results[0] && Results[2] && Results[3];
    const bool EntryDomainProven =
        OrdinaryGuardProven
            ? OrdinaryGuardBound >= limits::kMinJumpTableEntries &&
                  OrdinaryGuardBound <= IdxCap
            : EntryProofComplete && Results[1] && FeasibleMasks[1] != 0 &&
                  (IdxCap == 64 ||
                   (FeasibleMasks[1] >> IdxCap) == uint64_t{0});
    const bool EntryProven = EntrySemanticsProven && EntryDomainProven;

    // The empty-edge query above proves only the entry value (normally zero).
    // Complete the candidate-local fixed point without treating the 32-byte
    // physical index object as selector authority: every candidate target must
    // be decoded, the unique lexical fallthrough must reset the selector into
    // that entry set, and every other edge back to the dispatch must be guarded
    // by the same selector's unsigned object-capacity bound.  Adding all inner
    // targets to candidateReachableInstructions is an over-approximation and
    // therefore can only make these universal checks harder.
    auto clampedSelectorClosure =
        [&](uint64_t EntryMask) -> std::optional<bool> {
      if (!ActiveJumpTableProofRoots)
        return std::nullopt;
      bool ReachabilityComplete = false;
      const std::set<va_t> Reachable = candidateReachableInstructions(
          Rec, Info.ExplicitTargets, *ActiveJumpTableProofRoots,
          Info.StorageRanges, CandidateEvidenceBudget,
          &ReachabilityComplete);
      if (!ReachabilityComplete) {
        Info.IncompleteGuardDomain = true;
        return std::nullopt;
      }

      if (!consumeEvidence(orderedLookupWork(BlockStarts.size())))
        return std::nullopt;
      auto DispatchBlock = BlockStarts.find(BlkStart);
      if (DispatchBlock == BlockStarts.end() ||
          DispatchBlock == BlockStarts.begin())
        return false;
      const va_t EntryBlockStart = *std::prev(DispatchBlock);

      if (!consumeProducts(
              {{Info.ExplicitTargets.size(),
                orderedLookupWork(Reachable.size())},
               {Info.ExplicitTargets.size(), 1},
               {Reachable.size(), 1}}))
        return std::nullopt;
      for (va_t Target : Info.ExplicitTargets)
        if (Target == BlkStart ||
            (Target > EntryBlockStart && Target < BlkStart) ||
            !Reachable.count(Target))
          return false;

      auto collectBlockOps = [&](va_t Start, va_t End)
          -> std::optional<std::vector<LowOp>> {
        if (!consumeProducts(
                {{1, orderedLookupWork(Insns.size())}, {Insns.size(), 1}}))
          return std::nullopt;
        size_t RecordCount = 0;
        size_t OpCount = 0;
        for (auto It = Insns.lower_bound(Start);
             It != Insns.end() && It->first < End; ++It) {
          ++RecordCount;
          if (It->second.Ops.size() >
              std::numeric_limits<size_t>::max() - OpCount) {
            consumeEvidence(std::numeric_limits<size_t>::max());
            return std::nullopt;
          }
          OpCount += It->second.Ops.size();
        }
        if (!consumeProducts(
                {{1, orderedLookupWork(Insns.size())},
                 {RecordCount, 1},
                 {OpCount, 4},
                 {1, 2}}))
          return std::nullopt;
        std::vector<LowOp> Result;
        Result.reserve(OpCount);
        for (auto It = Insns.lower_bound(Start);
             It != Insns.end() && It->first < End; ++It)
          Result.insert(Result.end(), It->second.Ops.begin(),
                        It->second.Ops.end());
        return Result;
      };

      std::optional<std::vector<LowOp>> EntryOps =
          collectBlockOps(EntryBlockStart, BlkStart);
      if (!EntryOps)
        return std::nullopt;
      NdVar EntryValue =
          NdVar::reg(SwitchSrc, Clamp.PredicateIndexValue.Size);
      int EntryFrom = static_cast<int>(EntryOps->size()) - 1;
      std::optional<uint64_t> EntryCoordinate;
      for (size_t Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
        if (!consumeEvidence(EntryOps->size()))
          return std::nullopt;
        const int Def = reachingDefIdx(*EntryOps, EntryFrom, EntryValue);
        if (Def < 0)
          return false;
        const LowOp &Producer = (*EntryOps)[Def];
        if (Producer.NumInputs < 1)
          return false;
        const NdVar Input = Producer.Inputs[0];
        if (Input.isConst()) {
          EntryCoordinate = maskToSize(Input.Offset, EntryValue.Size);
          break;
        }
        if ((Producer.Opcode != NdOp::COPY &&
             Producer.Opcode != NdOp::INT_ZEXT &&
             Producer.Opcode != NdOp::SUBBYTES) ||
            (!Input.isReg() && !Input.isTemp()))
          return false;
        if (Producer.Opcode == NdOp::SUBBYTES &&
            (Producer.NumInputs < 2 || !Producer.Inputs[1].isConst() ||
             Producer.Inputs[1].Offset != 0))
          return false;
        EntryValue = Input;
        EntryFrom = Def - 1;
      }
      if (!EntryCoordinate) {
        Info.IncompleteGuardDomain = true;
        return std::nullopt;
      }
      if (*EntryCoordinate >= 64 ||
          (EntryMask & (uint64_t{1} << *EntryCoordinate)) == 0)
        return false;

      auto branchIsBoundedReentry = [&](const LowOp &Branch)
          -> std::optional<bool> {
        if (Branch.Opcode != NdOp::COND_BR || Branch.NumInputs < 2 ||
            !Branch.Inputs[0].isConst() ||
            Branch.Inputs[0].Offset != BlkStart)
          return false;
        if (!consumeEvidence(orderedLookupWork(BlockStarts.size())))
          return std::nullopt;
        auto GuardStart = BlockStarts.upper_bound(Branch.Addr);
        if (GuardStart == BlockStarts.begin())
          return false;
        --GuardStart;
        std::optional<std::vector<LowOp>> GuardOps =
            collectBlockOps(*GuardStart,
                            Branch.Addr == InvalidVA ? Branch.Addr
                                                     : Branch.Addr + 1);
        if (!GuardOps)
          return std::nullopt;
        int BranchIndex = -1;
        if (!consumeEvidence(GuardOps->size()))
          return std::nullopt;
        for (int I = static_cast<int>(GuardOps->size()) - 1; I >= 0; --I)
          if ((*GuardOps)[I].Addr == Branch.Addr &&
              (*GuardOps)[I].Seq == Branch.Seq) {
            BranchIndex = I;
            break;
          }
        if (BranchIndex < 0)
          return false;
        NdVar Condition = Branch.Inputs[1];
        int From = BranchIndex - 1;
        int CompareIndex = -1;
        for (size_t Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
          if (!consumeEvidence(GuardOps->size()))
            return std::nullopt;
          const int Def = reachingDefIdx(*GuardOps, From, Condition);
          if (Def < 0)
            return false;
          const LowOp &Producer = (*GuardOps)[Def];
          if (Producer.Opcode == NdOp::INT_LESS) {
            CompareIndex = Def;
            break;
          }
          if ((Producer.Opcode != NdOp::COPY &&
               Producer.Opcode != NdOp::SUBBYTES) ||
              Producer.NumInputs < 1)
            return false;
          if (Producer.Opcode == NdOp::SUBBYTES &&
              (Producer.NumInputs < 2 || !Producer.Inputs[1].isConst() ||
               Producer.Inputs[1].Offset != 0))
            return false;
          Condition = Producer.Inputs[0];
          From = Def - 1;
        }
        if (CompareIndex < 0) {
          Info.IncompleteGuardDomain = true;
          return std::nullopt;
        }
        const LowOp &Compare = (*GuardOps)[CompareIndex];
        if (Compare.NumInputs < 2 || !Compare.Inputs[0].isReg() ||
            Compare.Inputs[0].Offset != SwitchSrc ||
            Compare.Inputs[0].Size != SelectorLaneSize ||
            !Compare.Inputs[1].isConst() ||
            Compare.Inputs[1].Offset > IdxCap)
          return false;
        if (!consumeEvidence(GuardOps->size()))
          return std::nullopt;
        const NdVar WideSelector =
            NdVar::reg(SwitchSrc, Clamp.PredicateIndexValue.Size);
        const int WideDef =
            reachingDefIdx(*GuardOps, CompareIndex - 1, WideSelector);
        if (WideDef < 0 || GuardOps->at(WideDef).Opcode != NdOp::INT_ZEXT ||
            GuardOps->at(WideDef).NumInputs < 1 ||
            !GuardOps->at(WideDef).Inputs[0].isReg() ||
            GuardOps->at(WideDef).Inputs[0].Offset != SwitchSrc ||
            GuardOps->at(WideDef).Inputs[0].Size != SelectorLaneSize)
          return false;
        if (!consumeEvidence(GuardOps->size()))
          return std::nullopt;
        for (int I = WideDef + 1; I < BranchIndex; ++I)
          if (GuardOps->at(I).Output.isReg() &&
              GuardOps->at(I).Output.Offset == SwitchSrc)
            return false;
        return true;
      };

      bool SawBoundedReentry = false;
      if (!consumeEvidence(Reachable.size()))
        return std::nullopt;
      for (va_t Addr : Reachable) {
        if (!consumeEvidence(orderedLookupWork(Insns.size())))
          return std::nullopt;
        auto It = Insns.find(Addr);
        if (It == Insns.end())
          return false;
        const InsnRecord &Insn = It->second;
        if (!consumeProducts(
                {{Insn.Ops.size(), 1}, {Insn.JumpTableTargets.size(), 1}}))
          return std::nullopt;
        for (va_t Target : Insn.JumpTableTargets)
          if (Target == BlkStart ||
              (Target > EntryBlockStart && Target < BlkStart))
            return false;
        for (const LowOp &Op : Insn.Ops) {
          if ((Op.Opcode != NdOp::BRANCH && Op.Opcode != NdOp::COND_BR) ||
              Op.NumInputs < 1 || !Op.Inputs[0].isConst())
            continue;
          const va_t Target = Op.Inputs[0].Offset;
          if (Target > EntryBlockStart && Target < BlkStart)
            return false;
          if (Target != BlkStart)
            continue;
          const std::optional<bool> Bounded = branchIsBoundedReentry(Op);
          if (!Bounded)
            return std::nullopt;
          if (!*Bounded)
            return false;
          SawBoundedReentry = true;
        }
      }
      return SawBoundedReentry;
    };

    uint64_t EntryDomainMask = FeasibleMasks[1];
    if (OrdinaryGuardProven)
      EntryDomainMask = OrdinaryGuardBound >= 64
                            ? std::numeric_limits<uint64_t>::max()
                            : (uint64_t{1} << OrdinaryGuardBound) - 1;
    // A guard proved on the pre-expansion graph only contributes the entry
    // domain.  Every decoded inner target must still participate in the
    // candidate-local closure, otherwise a newly introduced case could jump
    // back into the dispatch after clobbering the selector or constants.
    std::optional<bool> Closure;
    if (EntryProven) {
      // A direct SELECT retains the selector as a first-class value, so the
      // final candidate-graph replay below can prove selector equality and the
      // complete unsigned domain on every entry/re-entry.  The x86 mask-blend
      // form loses that structural identity across Merge(ZExt) normalization;
      // keep its stricter lane/re-entry closure instead.
      if (Clamp.DirectSelect)
        Closure = true;
      else
        Closure = clampedSelectorClosure(EntryDomainMask);
    }
    // Replay the immutable fallback/bitmap certificates on the final graph,
    // after every physical inner-table target has been added.  Case bodies may
    // redefine those live-in registers; an entry-only proof must never be
    // reused to compose targets from stale lexical constants.  SameOuter is
    // deliberately not required here: localSelectorRoot proved both entry
    // uses share the same exact low lane, and clampedSelectorClosure proves the
    // lane, its widening, and absence of clobbers on every re-entry.  The
    // generic MustEqual relation is structural and can reject numerically
    // identical Merge(ZExt(...)) and ZExt(Merge(...)) forms after expansion;
    // that false negative must not replace the stronger shape-local proof.
    bool FinalComplete = false;
    std::vector<bool> FinalQueryComplete;
    std::vector<uint64_t> FinalFeasibleMasks;
    std::vector<bool> FinalResults;
    if (EntryProven && Closure && *Closure)
      FinalResults = tableValuesMatchAtUses(
          Queries, &FinalComplete, &FinalQueryComplete, Rec.Addr,
          &Info.ExplicitTargets, CandidateEvidenceBudget, 0, nullptr,
          &FinalFeasibleMasks);
    const bool FinalConstantSemanticsComplete =
        FinalComplete && FinalQueryComplete.size() == Queries.size() &&
        FinalResults.size() == Queries.size() && FinalQueryComplete[2] &&
        FinalQueryComplete[3];
    const bool FinalSelectorSemanticsComplete =
        !Clamp.DirectSelect ||
        (FinalConstantSemanticsComplete && FinalQueryComplete[0] &&
         FinalQueryComplete[1] && FinalFeasibleMasks.size() == Queries.size());
    const bool FinalSemanticsComplete =
        FinalConstantSemanticsComplete && FinalSelectorSemanticsComplete;
    const bool FinalConstantSemanticsProven =
        FinalConstantSemanticsComplete && FinalResults[2] && FinalResults[3];
    const bool FinalSelectorSemanticsProven =
        !Clamp.DirectSelect ||
        (FinalSelectorSemanticsComplete && FinalResults[0] && FinalResults[1] &&
         FinalFeasibleMasks[1] != 0);
    const bool FinalSemanticsProven =
        FinalConstantSemanticsProven && FinalSelectorSemanticsProven;
    GuardProven = EntryProven && Closure && *Closure && FinalSemanticsProven;
    if (GuardProven) {
      Info.MaxEntries = OrdinaryGuardProven ? OrdinaryGuardBound : IdxCap;
      Info.IncompleteGuardDomain = false;
      Info.SemanticGuardDomainAmbiguous = false;
    } else if (!EntryProofComplete || (EntryProven && !Closure) ||
               (EntryProven && Closure && *Closure &&
                !FinalSemanticsComplete)) {
      Info.IncompleteGuardDomain = true;
    } else {
      Info.SemanticGuardDomainAmbiguous = true;
    }
  }
  if (!GuardProven || Info.MaxEntries < limits::kMinJumpTableEntries ||
      Info.MaxEntries > limits::kMaxJumpTableEntries) {
    if (Clamp.Present)
      Info.IncompleteGuardDomain = true;
    return false;
  }
  const uint32_t GuardBound = Info.MaxEntries;

  if (GuardBound > IdxCap)
    return false;
  const uint32_t Scan = GuardBound;
  if (Clamp.Present && Clamp.FallbackSlot >= M)
    return false;

  // 7) Compose one target per switch value: idxtab[v] indexes jmptab.
  const size_t InnerSlotUpper = std::min<size_t>(Scan, M);
  const size_t InnerSlotLookup = orderedLookupWork(InnerSlotUpper);
  // Pay the coordinate scan, outer-object reads, target vector, and worst-case
  // ordered inner-slot nodes before the first retained element is allocated.
  // The provisional clamp proof already decoded and validated all M physical
  // targets, so that path only indexes its immutable vector here.
  if (!consumeProducts(
          {{Scan, 1},
           {Scan, Img.Segments.size()},
           {Scan, 3},
           {Scan, InnerSlotLookup + 3},
           {1, 4}}))
    return false;
  if (!Clamp.Present) {
    const size_t KnownEntryLookup =
        KnownFuncEntries ? orderedLookupWork(KnownFuncEntries->size()) : 0;
    const size_t RuntimeEntryLookup =
        orderedLookupWork(Img.RuntimeFunctionAddrs.size());
    const size_t VerifiedEntryLookup =
        orderedLookupWork(Img.VerifiedFunctionEntries.size());
    const size_t ImportStubLookup =
        orderedLookupWork(Img.ImportStubIndices.size());
    const size_t InsnLookup = orderedLookupWork(Insns.size());
    const size_t FragmentLookup =
        orderedLookupWork(Img.ExceptionMetadata.Functions.size());
    const size_t FragmentWorkPerEntry =
        FragmentLookup <= (std::numeric_limits<size_t>::max() - 3) / 2
            ? FragmentLookup * 2 + 3
            : std::numeric_limits<size_t>::max();
    if (!consumeProducts({{Scan, 16}}) ||
        !consumeFactorProduct({Scan, Img.Symbols.size(), 4}) ||
        !consumeFactorProduct({Scan, Img.Segments.size(), 16}) ||
        !consumeFactorProduct({Scan, Img.Sections.size(), 8}) ||
        !consumeFactorProduct(
            {Scan, Img.ExceptionMetadata.Functions.size(),
             FragmentWorkPerEntry}) ||
        !consumeFactorProduct({Scan, Img.ImportStubRanges.size(), 3}) ||
        !consumeFactorProduct({Scan, Img.Imports.size(), 2}) ||
        !consumeFactorProduct({Scan, Img.KnownCodeRanges.size(), 2}) ||
        !consumeFactorProduct(
            {Scan, Img.Imports.size(), Img.Segments.size(), 4}) ||
        !consumeFactorProduct(
            {Scan, Img.Imports.size(), Img.Sections.size(), 4}) ||
        !consumeFactorProduct({Scan, RuntimeEntryLookup, 2}) ||
        !consumeFactorProduct({Scan, VerifiedEntryLookup, 2}) ||
        !consumeFactorProduct({Scan, ImportStubLookup, 2}) ||
        !consumeFactorProduct({Scan, InsnLookup}) ||
        !consumeFactorProduct({Scan, KnownEntryLookup, 2}))
      return false;
  }
  std::vector<va_t> Targets;
  std::set<uint32_t> InnerSlots;
  Targets.reserve(Scan);
  for (uint32_t V = 0; V < Scan; ++V) {
    const uint8_t *IP = Img.readVA(IdxTab + static_cast<uint64_t>(V) * W1, W1);
    if (!IP)
      return false;
    uint32_t Iidx = 0;
    std::memcpy(&Iidx, IP, W1);
    if (Clamp.Present) {
      const uint32_t BitIndex = V % Clamp.BitmapBits;
      const bool BitSet = ((Clamp.Bitmap >> BitIndex) & uint64_t{1}) != 0;
      if (BitSet == Clamp.FallbackWhenBitSet)
        Iidx = static_cast<uint32_t>(Clamp.FallbackSlot);
    }
    if (Iidx >= M)
      return false;
    InnerSlots.insert(Iidx);
    va_t Target = 0;
    if (Clamp.Present) {
      if (Iidx >= Info.ExplicitTargets.size())
        return false;
      Target = Info.ExplicitTargets[Iidx];
    } else {
      const uint8_t *EP =
          Img.readVA(JmpTab + static_cast<uint64_t>(Iidx) * W2, W2);
      if (!EP)
        return false;
      std::optional<va_t> TargetOpt = decodeTableEntry(
          EP, W2, Relative, Relative, JmpTab, /*HasTargetBase=*/false,
          /*TargetBase=*/0, /*Scale=*/1, Img.getPointerSize());
      if (!TargetOpt)
        return false;
      Target = *TargetOpt;
      if (!Relative) {
        std::optional<va_t> Canonical =
            canonicalizeAbsoluteTableCodeTarget(Img, Target);
        if (!Canonical)
          return false;
        Target = *Canonical;
      }
      if (!isValidTarget(Img, Target, CurrentFuncEntry))
        return false;
    }
    Targets.push_back(Target);
  }

  if (Targets.size() != Scan)
    return false;

  const size_t StorageUpper = InnerSlotUpper + 1;
  const size_t SuppressibleUpper =
      Clamp.Present && !Relative ? InnerSlotUpper : 0;
  // Build all final dynamic metadata locally.  Three work units per retained
  // element cover vector storage, element construction, and future cleanup;
  // the fixed terms cover the vector objects.  One source traversal of the
  // selected-slot set populates both storage and suppression metadata.
  if (!consumeProducts({{StorageUpper, 3},
                        {SuppressibleUpper, 3},
                        {InnerSlots.size(), 1},
                        {1, 36}}))
    return false;
  std::vector<JumpTableStorageRange> FinalStorageRanges;
  FinalStorageRanges.reserve(StorageUpper);
  FinalStorageRanges.push_back(
      JumpTableStorageRange{IdxTab, W1, W1, Targets.size()});
  std::vector<va_t> FinalSuppressibleSlots;
  if (SuppressibleUpper)
    FinalSuppressibleSlots.reserve(SuppressibleUpper);
  for (uint32_t InnerSlot : InnerSlots) {
    if (InnerSlot != 0 && W2 > (InvalidVA - JmpTab) / InnerSlot)
      return false;
    FinalStorageRanges.push_back(
        JumpTableStorageRange{JmpTab + uint64_t(InnerSlot) * W2, W2, W2, 1});
    if (SuppressibleUpper)
      FinalSuppressibleSlots.push_back(
          JmpTab + static_cast<uint64_t>(InnerSlot) * W2);
  }

  JumpTableLoadRole OuterRole;
  OuterRole.Load = {Ops[IdxLoadIdx].Output, Ops[IdxLoadIdx].Addr,
                    Ops[IdxLoadIdx].Seq, /*DefinedAtPoint=*/true};
  OuterRole.LoadWidth = W1;
  OuterRole.AllowedBases = {IdxTab};
  OuterRole.Indices = {{SwitchIndexValue, SwitchIndexUseAddr, SwitchIndexUseSeq,
                        /*DefinedAtPoint=*/false}};
  OuterRole.AddressScale = IdxScale;

  JumpTableLoadRole InnerRole;
  InnerRole.Load = {Ops[JmpLoadIdx].Output, Ops[JmpLoadIdx].Addr,
                    Ops[JmpLoadIdx].Seq, /*DefinedAtPoint=*/true};
  InnerRole.LoadWidth = W2;
  InnerRole.AllowedBases = {JmpTab};
  if (Clamp.Present)
    InnerRole.Indices = {{NdVar::reg(EntryIdxReg, Img.getPointerSize()),
                          Ops[JmpLoadIdx].Addr, Ops[JmpLoadIdx].Seq,
                          /*DefinedAtPoint=*/false}};
  else
    InnerRole.Indices = {{Ops[IdxLoadIdx].Output, Ops[IdxLoadIdx].Addr,
                          Ops[IdxLoadIdx].Seq, /*DefinedAtPoint=*/true}};
  InnerRole.AddressScale = W2;
  InnerRole.AllowZeroExtension = true;
  InnerRole.AllowSignExtension = false;
  std::vector<JumpTableLoadRole> FinalLoadRoles;
  FinalLoadRoles.reserve(2);
  FinalLoadRoles.push_back(std::move(OuterRole));
  FinalLoadRoles.push_back(std::move(InnerRole));

  Info.setBaseAddr(JmpTab);
  Info.EntrySize = W2;
  Info.IsRelative = Relative;
  Info.IsSigned = Relative;
  Info.RelocAbsolute = Clamp.Present && !Relative;
  Info.IndexReg = SwitchSrc;
  Info.TwoLevelIndex = true;
  Info.IndexDomainAuthenticated = true;
  Info.ExplicitTargets = std::move(Targets);
  Info.StorageRanges = std::move(FinalStorageRanges);
  Info.SuppressibleRelocationSlots = std::move(FinalSuppressibleSlots);
  Info.LoadRoles = std::move(FinalLoadRoles);
  LLVM_DEBUG(llvm::dbgs() << "  two-level: idxtab 0x" << llvm::utohexstr(IdxTab)
                          << " (W1=" << W1 << ") -> jmptab 0x"
                          << llvm::utohexstr(JmpTab) << " (W2=" << W2
                          << ", M=" << M << "), " << Info.ExplicitTargets.size()
                          << " cases" << (Clamp.Present ? ", clamped" : "")
                          << "\n");
  return true;
}

} // namespace neverd
