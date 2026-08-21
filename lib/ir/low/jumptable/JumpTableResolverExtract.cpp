//===- JumpTableResolverExtract.cpp - Jump-table result construction ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Construction of the resolver's results: inverse-normalizing table positions
/// back into the case labels the emitter dispatches on, aligning branches that
/// share one table so a short copy adopts its most complete sibling, and
/// collecting the per-branch metadata into LowFunc::JumpTables.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch.
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <map>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

//===----------------------------------------------------------------------===//
// recoverCaseLabels — inverse-normalize table indices to original case values
//===----------------------------------------------------------------------===//

void CFGBuilder::recoverCaseLabels(JumpTable &JT,
                                   const JumpTableInfo &Info) const {
  // The recovered case labels must be expressed in the SAME coordinate the
  // emitter dispatches on, which depends on the table kind:
  //
  //   * Resolver-register dispatch (AArch64 compact byte/halfword tables and
  //     pre-scaled computed gotos) dispatches on the recovered index register,
  //     the *pre-normalization* switch variable — so the labels invert the
  //     normalization: label = (position * Stride << NormShift) + NormBase.
  //     This mirrors MedLLVMEmitter::emitJumpTableSwitch's own path selection
  //     `(TargetBase != 0 && EntrySize <= 2) || PreScaledIndex`.
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
      (Info.TargetBase != 0 && Info.EntrySize <= 2) || Info.PreScaledIndex;
  int64_t NormBase = DispatchesPreNormIndex ? Info.NormBase : 0;
  uint32_t NormShift = DispatchesPreNormIndex ? Info.NormShift : 0;

  if (!HasGap && NormBase == 0 && NormShift == 0 && Info.Stride <= 1)
    return;

  JT.CaseLabels.reserve(JT.Targets.size());
  for (size_t I = 0; I < JT.Targets.size(); ++I) {
    int64_t Label = (HasGap && I < Info.EntryIndices.size())
                        ? static_cast<int64_t>(Info.EntryIndices[I])
                        : static_cast<int64_t>(I);
    if (Info.Stride > 1)
      Label *= static_cast<int64_t>(Info.Stride);
    if (NormShift > 0)
      Label <<= NormShift;
    Label += NormBase;
    JT.CaseLabels.push_back(Label);
  }

  LLVM_DEBUG({
    llvm::dbgs() << "  case-labels: recovered " << JT.CaseLabels.size()
                 << " labels (base=" << NormBase << ", shift=" << NormShift
                 << ", stride=" << Info.Stride << ")\n";
  });
}

//===----------------------------------------------------------------------===//
// reconcileSharedTables — align branches dispatching through the same table
//===----------------------------------------------------------------------===//

bool CFGBuilder::reconcileSharedTables(const BinaryImage &Img, Decoder &Dec) {
  bool Changed = false;
  // A clang-peeled first loop iteration and the loop body dispatch through the
  // *same* rodata jump table.  The peeled copy lives in the large
  // function-prologue block, where pre-SSA register reuse can leave the bound /
  // normalization analysis short (e.g. a case body's `and x,15` intersecting
  // the index's `and x,31`), while the loop body — sitting in a small, clean
  // block — recovers fully.  Group resolved branches by table base and let
  // every short copy adopt the most complete sibling, so a peeled copy never
  // drops cases.
  std::map<va_t, va_t> BestByBase;
  for (auto &[Addr, Rec] : Insns) {
    if (Rec.JumpTableTargets.empty())
      continue;
    auto It = ResolvedTableInfo.find(Addr);
    if (It == ResolvedTableInfo.end() || !It->second.HasBaseAddr)
      continue;
    auto B = BestByBase.find(It->second.BaseAddr);
    if (B == BestByBase.end() ||
        Insns[B->second].JumpTableTargets.size() < Rec.JumpTableTargets.size())
      BestByBase[It->second.BaseAddr] = Addr;
  }

  // Collect the branches to upgrade first; applying them calls explore(), which
  // mutates Insns and would invalidate an in-flight iterator.
  std::vector<va_t> ToUpgrade;
  for (auto &[Addr, Rec] : Insns) {
    if (Rec.JumpTableTargets.empty())
      continue;
    auto It = ResolvedTableInfo.find(Addr);
    if (It == ResolvedTableInfo.end() || !It->second.HasBaseAddr)
      continue;
    auto B = BestByBase.find(It->second.BaseAddr);
    if (B == BestByBase.end() || B->second == Addr)
      continue;
    const auto &BestInfo = ResolvedTableInfo[B->second];
    if (BestInfo.EntrySize != It->second.EntrySize ||
        BestInfo.IsRelative != It->second.IsRelative ||
        BestInfo.TargetBase != It->second.TargetBase)
      continue;
    if (Insns[B->second].JumpTableTargets.size() <= Rec.JumpTableTargets.size())
      continue;
    ToUpgrade.push_back(Addr);
  }

  for (va_t Addr : ToUpgrade) {
    auto RecIt = Insns.find(Addr);
    if (RecIt == Insns.end())
      continue;
    InsnRecord &Rec = RecIt->second;
    va_t Base = ResolvedTableInfo[Addr].BaseAddr;

    // Re-read with the sibling's (more complete) table parameters, keeping this
    // branch's own index register so its switch variable stays correct.
    JumpTableInfo Adopted = ResolvedTableInfo[BestByBase[Base]];
    Adopted.IndexReg = ResolvedTableInfo[Addr].IndexReg;
    auto NewTargets = readTableEntries(Img, Adopted);
    if (NewTargets.size() <= Rec.JumpTableTargets.size())
      continue;

    Rec.JumpTableTargets = NewTargets;
    ResolvedTableInfo[Addr] = Adopted;
    Changed = true;
    for (va_t T : NewTargets) {
      if (!ExploredAddrs.count(T)) {
        BlockStarts.insert(T);
        explore(Img, Dec, T);
      }
    }
  }
  return Changed;
}

//===----------------------------------------------------------------------===//
// extractJumpTables — collect jump-table metadata from decoded instructions
//===----------------------------------------------------------------------===//

void CFGBuilder::extractJumpTables(LowFunc &Func) {
  for (auto &[Addr, Rec] : Insns) {
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
      JT.IsRelative = Info.IsRelative;
      JT.IsSigned = Info.IsSigned;
      JT.TargetBase = Info.TargetBase;
      JT.PreScaledIndex = Info.PreScaledIndex;
      JT.TwoTableSelect = Info.TwoTableSelect;
      JT.TwoTableOffset = Info.TwoTableOffset;
      JT.TwoTableHiPositive = Info.TwoTableHiPositive;
      JT.TwoLevelIndex = Info.TwoLevelIndex;
      JT.MutatedUnsafe = Info.MutatedUnsafe;
      if (Info.IndexReg != InvalidVA)
        JT.IndexRegOff = static_cast<int>(Info.IndexReg);
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
    if (CachedIt2 != ResolvedTableInfo.end())
      recoverCaseLabels(JT, CachedIt2->second);

    Func.JumpTables.push_back(JT);
  }
}

} // namespace neverd
