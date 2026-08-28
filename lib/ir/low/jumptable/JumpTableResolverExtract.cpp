//===- JumpTableResolverExtract.cpp - Jump-table result construction ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Construction of the resolver's results: inverse-normalizing table positions
/// back into the case labels the emitter dispatches on and collecting the
/// per-branch metadata into LowFunc::JumpTables.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <climits>
#include <cstdint>
#include <numeric>
#include <set>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

std::optional<bool> CFGBuilder::resolvedJumpTableOwnsStorageAddress(
    va_t Address, const std::set<va_t> *ReachableInsnFilter,
    size_t *EvidenceBudget) const {
  auto orderedLookupWork = [](size_t Count) {
    size_t Work = 1;
    for (size_t N = Count; N > 1; N = N / 2 + N % 2)
      ++Work;
    return Work;
  };
  auto consumeEvidence = [&](size_t Amount = 1) {
    if (!EvidenceBudget)
      return true;
    if (Amount > *EvidenceBudget) {
      *EvidenceBudget = 0;
      return false;
    }
    *EvidenceBudget -= Amount;
    return true;
  };
  for (const auto &[BranchAddr, Info] : ResolvedTableInfo) {
    if (!consumeEvidence() || !consumeEvidence(orderedLookupWork(Insns.size())))
      return std::nullopt;
    auto Rec = Insns.find(BranchAddr);
    if (Rec == Insns.end() || Rec->second.JumpTableTargets.empty())
      continue;
    if (ReachableInsnFilter) {
      if (!consumeEvidence(orderedLookupWork(ReachableInsnFilter->size())))
        return std::nullopt;
      if (!ReachableInsnFilter->count(BranchAddr))
        continue;
    } else if (!PublishedReachableInsns.empty()) {
      if (!consumeEvidence(orderedLookupWork(PublishedReachableInsns.size())))
        return std::nullopt;
      if (!PublishedReachableInsns.count(BranchAddr))
        continue;
    }

    if (!Info.StorageRanges.empty()) {
      for (const JumpTableStorageRange &Range : Info.StorageRanges) {
        if (!consumeEvidence())
          return std::nullopt;
        if (Range.ownsStorageAddress(Address))
          return true;
      }
      continue;
    }

    if (!Info.HasBaseAddr || Info.EntrySize == 0)
      continue;
    uint64_t SlotCount = Info.MaxEntries;
    if (SlotCount == 0)
      SlotCount = !Info.EntryIndices.empty()
                      ? static_cast<uint64_t>(Info.EntryIndices.back()) + 1
                      : Rec->second.JumpTableTargets.size();
    JumpTableStorageRange Range{
        Info.BaseAddr, Info.EntrySize,
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize, SlotCount};
    if (!consumeEvidence())
      return std::nullopt;
    if (Range.ownsStorageAddress(Address))
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// recoverCaseLabels — inverse-normalize table indices to original case values
//===----------------------------------------------------------------------===//

std::optional<uint64_t> recoverCaseLabelBitPattern(uint64_t EntryIndex,
                                                   uint32_t Stride,
                                                   uint32_t NormShift,
                                                   int64_t NormBase) {
  if (NormShift >= 64)
    return std::nullopt;
  llvm::APInt Label(64, EntryIndex, /*isSigned=*/false,
                    /*implicitTrunc=*/true);
  Label *= llvm::APInt(64, Stride, /*isSigned=*/false,
                       /*implicitTrunc=*/true);
  Label <<= NormShift;
  Label += llvm::APInt(64, static_cast<uint64_t>(NormBase),
                       /*isSigned=*/false, /*implicitTrunc=*/true);
  return Label.getZExtValue();
}

bool CFGBuilder::recoverCaseLabels(JumpTable &JT,
                                   const JumpTableInfo &Info) const {
  JT.CaseLabels.clear();
  if (!Info.RuntimeCaseLabels.empty()) {
    if (Info.RuntimeCaseLabels.size() != JT.Targets.size()) {
      return false;
    }
    std::set<uint32_t> Seen;
    JT.CaseLabels.reserve(Info.RuntimeCaseLabels.size());
    for (uint32_t Label : Info.RuntimeCaseLabels) {
      if (!Seen.insert(Label).second) {
        JT.CaseLabels.clear();
        return false;
      }
      JT.CaseLabels.push_back(static_cast<int64_t>(Label));
    }
    return true;
  }
  // The recovered case labels must be expressed in the SAME coordinate the
  // emitter dispatches on, which depends on the table kind:
  //
  //   * Resolver-register dispatch (AArch64 compact byte/halfword tables and
  //     pre-scaled computed gotos) dispatches on the recovered index register,
  //     the *pre-normalization* switch variable — so the labels invert the
  //     normalization: label = (position * Stride << NormShift) + NormBase.
  //     This mirrors MedLLVMEmitter::emitJumpTableSwitch's own path selection
  //     `(HasTargetBase && EntrySize <= 2) || PreScaledIndex`.
  //
  //   * Every other (regular relative/absolute) table is dispatched via
  //     MedLLVMEmitter::findSwitchIndex, which returns the value feeding the
  //     table-address scale — the *post-normalization* index, after any
  //     `sub base` / mask / shift that computed it.  Its labels are therefore
  //     the raw table positions 0..N-1 (Stride still scales a pre-scaled byte
  //     index).  Folding NormBase/NormShift in here would shift them into the
  //     pre-normalization coordinate the emitter never compares against,
  //     sending every arm to the wrong target — e.g. an ARM32 inline
  //     PC-relative word table for `switch(st)`, st in [1,5], with index
  //     st-1 in [0,4]: the emitter dispatches on st-1 but labels {1..5} would
  //     then match nothing (a masked index already zeroes NormBase in
  //     traceIndexTransform, so only the non-masked `sub` form was affected).
  //
  // EntryIndices (a bounded sparse table's kept slot indices) is always the
  // real table position and so is coordinate-correct for either path.
  bool HasGap = !Info.EntryIndices.empty();
  bool DispatchesPreNormIndex =
      (Info.HasTargetBase && Info.EntrySize <= 2) || Info.PreScaledIndex;
  int64_t NormBase = DispatchesPreNormIndex ? Info.NormBase : 0;
  uint32_t NormShift = DispatchesPreNormIndex ? Info.NormShift : 0;

  if (!HasGap && NormBase == 0 && NormShift == 0 && Info.Stride <= 1)
    return true;

  std::set<uint64_t> Seen;
  JT.CaseLabels.reserve(JT.Targets.size());
  for (size_t I = 0; I < JT.Targets.size(); ++I) {
    uint64_t EntryIndex = (HasGap && I < Info.EntryIndices.size())
                              ? Info.EntryIndices[I]
                              : static_cast<uint64_t>(I);
    auto Label = recoverCaseLabelBitPattern(EntryIndex, Info.Stride, NormShift,
                                            NormBase);
    if (!Label || !Seen.insert(*Label).second) {
      JT.CaseLabels.clear();
      return false;
    }
    llvm::APInt SignedBits(64, *Label, /*isSigned=*/false,
                           /*implicitTrunc=*/true);
    JT.CaseLabels.push_back(SignedBits.getSExtValue());
  }

  LLVM_DEBUG({
    llvm::dbgs() << "  case-labels: recovered " << JT.CaseLabels.size()
                 << " labels (base=" << NormBase << ", shift=" << NormShift
                 << ", stride=" << Info.Stride << ")\n";
  });
  return true;
}

//===----------------------------------------------------------------------===//
// extractJumpTables — collect jump-table metadata from decoded instructions
//===----------------------------------------------------------------------===//

void CFGBuilder::extractJumpTables(LowFunc &Func) {
  std::set<va_t> ReachableInsns;
  for (const LowBlock &Block : Func.Blocks)
    for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries)
      ReachableInsns.insert(Boundary.Address);

  auto selectorUseRef = [&](const JumpTableValueOccurrence &Occurrence)
      -> std::optional<JumpTableSelectorUseRef> {
    if (Occurrence.Addr == InvalidVA || Occurrence.Seq < 0 ||
        Occurrence.Value.Size == 0 || !ReachableInsns.count(Occurrence.Addr))
      return std::nullopt;
    auto InsnIt = Insns.find(Occurrence.Addr);
    if (InsnIt == Insns.end())
      return std::nullopt;

    const LowOp *UseOp = nullptr;
    std::optional<uint8_t> MatchedInputNo;
    for (const LowOp &Op : InsnIt->second.Ops) {
      if (Op.Addr != Occurrence.Addr || Op.Seq != Occurrence.Seq)
        continue;

      std::optional<uint8_t> CandidateInputNo;
      if (Occurrence.DefinedAtPoint) {
        if (Op.Output != Occurrence.Value)
          continue;
      } else {
        for (uint8_t I = 0; I < Op.NumInputs; ++I) {
          if (Op.Inputs[I] != Occurrence.Value)
            continue;
          if (CandidateInputNo)
            return std::nullopt;
          CandidateInputNo = I;
        }
        if (!CandidateInputNo)
          continue;
      }
      if (UseOp)
        return std::nullopt;
      UseOp = &Op;
      MatchedInputNo = CandidateInputNo;
    }
    if (!UseOp)
      return std::nullopt;

    JumpTableSelectorUseRef Ref;
    Ref.Addr = UseOp->Addr;
    Ref.Seq = UseOp->Seq;
    Ref.ExpectedOpcode = UseOp->Opcode;
    Ref.ExpectedSize = Occurrence.Value.Size;
    if (Occurrence.DefinedAtPoint) {
      Ref.Role = JumpTableSelectorUseRef::ValueRole::Output;
      return Ref;
    }
    if (!MatchedInputNo)
      return std::nullopt;
    Ref.Role = JumpTableSelectorUseRef::ValueRole::Input;
    Ref.InputNo = *MatchedInputNo;
    return Ref;
  };

  for (auto &[Addr, Rec] : Insns) {
    if (!ReachableInsns.count(Addr))
      continue;
    if (Rec.JumpTableTargets.empty())
      continue;

    JumpTable JT;
    JT.InsnAddr = Addr;
    JT.Targets = Rec.JumpTableTargets;

    // Reuse the cached analysis from resolveJumpTable when available,
    // avoiding the duplicate backward-slicing logic.
    auto CachedIt = ResolvedTableInfo.find(Addr);
    if (CachedIt != ResolvedTableInfo.end()) {
      auto &Info = CachedIt->second;
      JT.BaseAddr = Info.BaseAddr;
      JT.HasBaseAddr = Info.HasBaseAddr;
      JT.EntrySize = Info.EntrySize;
      JT.EntryStride =
          Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
      JT.StorageRanges = Info.StorageRanges;
      JT.SuppressibleRelocationSlots = Info.SuppressibleRelocationSlots;
      for (const auto &Role : Info.LoadRoles)
        JT.AuthenticatedTableLoads.push_back(
            {Role.Load.Addr, Role.Load.Seq, Role.LoadWidth});
      std::sort(JT.AuthenticatedTableLoads.begin(),
                JT.AuthenticatedTableLoads.end());
      JT.AuthenticatedTableLoads.erase(
          std::unique(JT.AuthenticatedTableLoads.begin(),
                      JT.AuthenticatedTableLoads.end()),
          JT.AuthenticatedTableLoads.end());
      JT.HasDispatchSlotMap = !Info.TwoTableSelect && !Info.TwoLevelIndex;
      if (JT.HasDispatchSlotMap) {
        if (!Info.RuntimeSlotIndices.empty())
          JT.SlotIndices = Info.RuntimeSlotIndices;
        else if (!Info.EntryIndices.empty())
          JT.SlotIndices = Info.EntryIndices;
        else {
          JT.SlotIndices.resize(JT.Targets.size());
          std::iota(JT.SlotIndices.begin(), JT.SlotIndices.end(), 0u);
        }
      }
      if (JT.StorageRanges.empty() && Info.HasBaseAddr && Info.EntrySize != 0) {
        uint64_t SlotCount = Info.MaxEntries;
        if (SlotCount == 0)
          SlotCount = !Info.EntryIndices.empty()
                          ? static_cast<uint64_t>(Info.EntryIndices.back()) + 1
                          : JT.Targets.size();
        JT.StorageRanges.push_back(JumpTableStorageRange{
            Info.BaseAddr, Info.EntrySize, JT.EntryStride, SlotCount});
      }
      JT.IsRelative = Info.IsRelative;
      JT.IsSigned = Info.IsSigned;
      JT.TargetBase = Info.TargetBase;
      JT.HasTargetBase = Info.HasTargetBase;
      JT.IsPEImageRelativeRVA = Info.IsPEImageRelativeRVA;
      JT.TableLoadAddr = Info.TableLoadAddr;
      JT.PreScaledIndex = Info.PreScaledIndex;
      JT.TwoTableSelect = Info.TwoTableSelect;
      JT.TwoTableOffset = Info.TwoTableOffset;
      JT.TwoTableHiPositive = Info.TwoTableHiPositive;
      JT.TwoLevelIndex = Info.TwoLevelIndex;
      JT.MutatedUnsafe = Info.MutatedUnsafe;
      if (!Info.UseSharedDispatchSelector && Info.IndexReg != InvalidVA)
        JT.IndexRegOff = static_cast<int>(Info.IndexReg);
      if (Info.TwoTableSelect) {
        const JumpTableLoadRole *CompositeRole = nullptr;
        for (const JumpTableLoadRole &Role : Info.LoadRoles) {
          if (Role.HasBaseSelect == Role.HasBaseMaskBlend ||
              Role.AddressIndex.Value.Size == 0 ||
              Role.SelectCondition.Value.Size == 0)
            continue;
          if (CompositeRole) {
            CompositeRole = nullptr;
            break;
          }
          CompositeRole = &Role;
        }
        if (CompositeRole) {
          auto ByteIndex = selectorUseRef(CompositeRole->AddressIndex);
          auto Condition = selectorUseRef(CompositeRole->SelectCondition);
          if (ByteIndex && Condition && ByteIndex->ExpectedSize != 0) {
            JumpTableCompositeSelectorUseRef Composite;
            Composite.ByteIndex = *ByteIndex;
            Composite.Condition = *Condition;
            Composite.ResultSize = ByteIndex->ExpectedSize;
            Composite.TrueOffset =
                Info.TwoTableHiPositive ? Info.TwoTableOffset : 0;
            Composite.FalseOffset =
                Info.TwoTableHiPositive ? 0 : Info.TwoTableOffset;
            const unsigned ResultBits = Composite.ResultSize * CHAR_BIT;
            const bool OffsetsFit =
                ResultBits >= 64 ||
                ((Composite.TrueOffset >> ResultBits) == 0 &&
                 (Composite.FalseOffset >> ResultBits) == 0);
            if (OffsetsFit)
              JT.CompositeSelectorUseRef = std::move(Composite);
          }
        }
      } else if (!Info.UseSharedDispatchSelector) {
        std::vector<JumpTableValueOccurrence> IndexOccurrences =
            Info.IndexValueAlternatives;
        if (IndexOccurrences.empty())
          IndexOccurrences.push_back({Info.IndexValueAtUse, Info.IndexUseAddr,
                                      Info.IndexUseSeq,
                                      Info.IndexValueDefinedAtUse});
        bool CompleteSelectorRefs = !IndexOccurrences.empty();
        for (const JumpTableValueOccurrence &IndexOccurrence :
             IndexOccurrences) {
          auto Ref = selectorUseRef(IndexOccurrence);
          if (!Ref) {
            CompleteSelectorRefs = false;
            break;
          }
          if (std::find(JT.SelectorUseRefs.begin(), JT.SelectorUseRefs.end(),
                        *Ref) == JT.SelectorUseRefs.end())
            JT.SelectorUseRefs.push_back(*Ref);
        }
        if (!CompleteSelectorRefs)
          JT.SelectorUseRefs.clear();
      }
    } else {
      // Fallback: quick extraction from NdOp ops.
      bool FoundBase = false;
      bool FoundSize = false;
      bool SawLoad = false;
      uint16_t LoadWidth = 0;
      bool SawSext = false;

      for (auto &Op : Rec.Ops) {
        switch (Op.Opcode) {
        case NdOp::INT_ADD:
          if (!FoundBase && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
            JT.BaseAddr = Op.Inputs[1].Offset;
            JT.HasBaseAddr = true;
            FoundBase = true;
          }
          break;
        case NdOp::INT_MULT:
          if (!FoundSize && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
            JT.EntrySize = static_cast<uint16_t>(Op.Inputs[1].Offset);
            FoundSize = true;
          }
          break;
        case NdOp::INT_LEFT:
          if (!FoundSize && Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
            uint64_t Shift = Op.Inputs[1].Offset;
            if (Shift <= limits::kMaxShiftForEntrySize) {
              JT.EntrySize = static_cast<uint16_t>(1u << Shift);
              FoundSize = true;
            }
          }
          break;
        case NdOp::LOAD:
          SawLoad = true;
          LoadWidth = Op.Output.Size;
          break;
        case NdOp::INT_SEXT:
          SawSext = true;
          break;
        default:
          break;
        }
      }

      if (SawLoad && !FoundSize && LoadWidth > 0 &&
          LoadWidth <= limits::kMaxEntryBytes) {
        JT.EntrySize = LoadWidth;
        FoundSize = true;
      }
      if (SawLoad && FoundBase && LoadWidth > 0 &&
          LoadWidth < limits::kMaxEntryBytes)
        JT.IsRelative = true;
      JT.IsSigned = SawSext || (SawLoad && LoadWidth < limits::kMaxEntryBytes);
    }

    auto CachedIt2 = ResolvedTableInfo.find(Addr);
    if (CachedIt2 != ResolvedTableInfo.end()) {
      const JumpTableInfo &Info = CachedIt2->second;
      if ((!Info.RuntimeCaseLabels.empty() ||
           !Info.RuntimeSlotIndices.empty()) &&
          (Info.RuntimeCaseLabels.size() != JT.Targets.size() ||
           Info.RuntimeSlotIndices.size() != JT.Targets.size()))
        continue;
      if (!recoverCaseLabels(JT, CachedIt2->second))
        continue;
    }

    if (JT.EntryStride == 0)
      JT.EntryStride = JT.EntrySize;
    if (JT.SlotIndices.empty() && !JT.Targets.empty() && !JT.TwoTableSelect &&
        !JT.TwoLevelIndex) {
      JT.SlotIndices.resize(JT.Targets.size());
      std::iota(JT.SlotIndices.begin(), JT.SlotIndices.end(), 0u);
      JT.HasDispatchSlotMap = true;
    }
    if (JT.StorageRanges.empty() && JT.HasBaseAddr && !JT.Targets.empty() &&
        JT.EntrySize != 0 && JT.EntryStride != 0) {
      uint64_t SlotCount =
          !JT.SlotIndices.empty()
              ? static_cast<uint64_t>(JT.SlotIndices.back()) + 1
              : JT.Targets.size();
      JT.StorageRanges.push_back(JumpTableStorageRange{
          JT.BaseAddr, JT.EntrySize, JT.EntryStride, SlotCount});
    }

    Func.JumpTables.push_back(JT);
  }
}

} // namespace neverd
