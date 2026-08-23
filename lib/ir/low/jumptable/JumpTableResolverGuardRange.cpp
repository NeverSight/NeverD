//===- JumpTableResolverGuardRange.cpp - CircleRange guard analysis -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The CircleRange-based half of the guard analysis: turn a comparison into a
/// modular value range, intersect every range that constrains the live index
/// definition, and — when no direct comparison names the index — rewind a
/// guard's range through count-preserving reshapes until it lands on the index
/// register.  Comparison-value guard matching lives in
/// JumpTableResolverGuards.cpp.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/CircleRange.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

//===----------------------------------------------------------------------===//
// extractGuardRange — build a CircleRange from a comparison op
//===----------------------------------------------------------------------===//

CircleRange CFGBuilder::extractGuardRange(const std::vector<LowOp> &Ops,
                                          const LowOp &Op, int VarSize) const {
  if (Op.NumInputs < 2)
    return CircleRange::empty();
  // -O0 frequently materialises the comparison constant into a temp
  // (`COPY t,#k; cmp x,t`), so resolve the bound operand through it.
  uint64_t Bound;
  if (!resolveConstThroughCopy(Ops, static_cast<int>(Ops.size()) - 1,
                               Op.Inputs[1], Bound))
    return CircleRange::empty();
  switch (Op.Opcode) {
  case NdOp::INT_LESS:
    return CircleRange(uint64_t(0), Bound, VarSize);
  case NdOp::INT_LESSEQUAL:
    return CircleRange(uint64_t(0), Bound + 1, VarSize);
  case NdOp::INT_SLESS: {
    int64_t SBound = static_cast<int64_t>(Bound);
    if (SBound > 0)
      return CircleRange(uint64_t(0), static_cast<uint64_t>(SBound), VarSize);
    return CircleRange::empty();
  }
  case NdOp::INT_SLESSEQUAL: {
    int64_t SBound = static_cast<int64_t>(Bound);
    if (SBound >= 0)
      return CircleRange(uint64_t(0), static_cast<uint64_t>(SBound + 1),
                         VarSize);
    return CircleRange::empty();
  }
  case NdOp::INT_AND: {
    if (Bound > 0 && (Bound & (Bound + 1)) == 0)
      return CircleRange(uint64_t(0), Bound + 1, VarSize);
    return CircleRange::empty();
  }
  case NdOp::INT_EQUAL: {
    if (Bound > 0 && Bound + 1 <= limits::kMaxJumpTableEntries)
      return CircleRange(uint64_t(0), Bound + 1, VarSize);
    return CircleRange::empty();
  }
  default:
    return CircleRange::empty();
  }
}

//===----------------------------------------------------------------------===//
// refineRangeFromGuards — range-based guard analysis
//===----------------------------------------------------------------------===//

bool CFGBuilder::refineRangeFromGuards(const InsnRecord &Rec,
                                       JumpTableInfo &Info) {
  int VarSize = getTargetRegInfo(CurrentImg ? CurrentImg->Arch : Arch::Unknown)
                    .PointerSize;

  CircleRange Best = CircleRange::full(VarSize);
  bool Found = false;

  // A bare `and idx, mask` only bounds the table when it forms the
  // masked-switch dispatch (`and; load; jmp` all in the INDIR_BR block).  An
  // incidental `and idx, k` in another block (e.g. computing `k & 1` for
  // arithmetic) operates on a different definition of the index and must not
  // constrain the range — otherwise its tiny mask intersects away the real `cmp
  // idx,N` bound.
  va_t BranchBlockStart = 0;
  {
    auto BIt = BlockStarts.upper_bound(Rec.Addr);
    if (BIt != BlockStarts.begin()) {
      --BIt;
      BranchBlockStart = *BIt;
    }
  }

  // The dispatch index can share a physical register with an unrelated value
  // (e.g. an enclosing switch's index, or an -O0 loop counter, reusing the same
  // GPR).  A guard that reads a definition of IndexReg that is overwritten
  // before the dispatch constrains a stale value, not the table index, so its
  // (often tighter) range must not narrow the bound.  The original cutoff is
  // the dispatch block start: any pre-block def is potentially stale.
  //
  // But an inner switch nested in an outer switch's arm re-derives its index
  // into the shared register *inside* the dispatch block by masking it to the
  // table width (`shr eax,2; and eax,7; movsxd rax,[tab+rax*4]`).  That
  // in-block low-bit mask (`and idx, 2^k-1`) is the definitive index bound and
  // makes the outer switch's guard on the pre-mask value (`cmp eax,1`) stale —
  // otherwise it collapses the inner table to a single entry.  So advance the
  // cutoff to the last in-block power-of-two mask that writes IndexReg.  A
  // plain reload
  // (`ldr idx,[sp,#k]`) or copy is NOT such a redefinition: it restores the
  // same spilled value the guard legitimately constrains (the -O0 shape), so it
  // must not advance the cutoff.
  va_t IndexDefCutoff = BranchBlockStart;
  if (Info.IndexReg != InvalidVA && BranchBlockStart != 0) {
    for (auto &[Addr, IRec] : Insns) {
      if (Addr >= Rec.Addr)
        break;
      if (Addr < BranchBlockStart)
        continue;
      for (auto &Op : IRec.Ops)
        if (Op.Opcode == NdOp::INT_AND && Op.Output.isReg() &&
            Op.Output.Offset == Info.IndexReg && Op.NumInputs >= 2 &&
            Op.Inputs[1].isConst()) {
          uint64_t M = Op.Inputs[1].Offset;
          if (M != 0 && (M & (M + 1)) == 0)
            IndexDefCutoff = Addr; // last in-block low-bit index mask
        }
    }
  }
  va_t LastIndexDef = 0;
  if (Info.IndexReg != InvalidVA && IndexDefCutoff != 0) {
    for (auto &[Addr, IRec] : Insns) {
      if (Addr >= IndexDefCutoff)
        break;
      for (auto &Op : IRec.Ops)
        if (Op.Output.isReg() && Op.Output.Offset == Info.IndexReg)
          LastIndexDef = Addr;
    }
  }

  for (auto &[Addr, IRec] : Insns) {
    if (Addr >= Rec.Addr)
      break;
    for (auto &Op : IRec.Ops) {
      if (!guardConstrainsIndex(IRec.Ops, Op, Info.IndexReg))
        continue;
      if (Info.IndexReg != InvalidVA && Addr < LastIndexDef)
        continue;
      if (Op.Opcode == NdOp::INT_AND && Addr < BranchBlockStart)
        continue;
      CircleRange R = extractGuardRange(IRec.Ops, Op, VarSize);
      if (R.isEmpty())
        continue;
      int Code = Best.intersect(R);
      if (Code == 2) {
        Best = R;
      }
      Found = true;
    }
  }

  if (!Found || Best.isEmpty())
    return false;

  if (Best.isFull())
    return false;

  uint64_t RangeSize = Best.getSize();

  // When the range is excessively large, assume the switch variable is
  // non-negative (common for enums and integer switch expressions).
  if (RangeSize > limits::kPositiveRangeThreshold && VarSize >= 4) {
    CircleRange Positive(uint64_t(0), (Best.getMask() >> 1) + 1, VarSize);
    Positive.intersect(Best);
    if (!Positive.isEmpty()) {
      Best = Positive;
      RangeSize = Best.getSize();
      LLVM_DEBUG(llvm::dbgs()
                 << "  range-guard: narrowed to positive range, size="
                 << RangeSize << "\n");
    }
  }

  if (RangeSize > 0 && RangeSize <= limits::kMaxJumpTableEntries) {
    // Reject 1-byte variables producing a 256-entry range unless the
    // guard explicitly constrains the value or a LOAD intervenes.
    if (RangeSize == limits::kByteVarFullRange && VarSize == 1) {
      bool HasExplicitGuard = false;
      for (auto &[Addr, IRec] : Insns) {
        if (Addr >= Rec.Addr)
          break;
        for (auto &Op : IRec.Ops) {
          if (Op.Opcode == NdOp::INT_LESS || Op.Opcode == NdOp::INT_SLESS ||
              Op.Opcode == NdOp::INT_LESSEQUAL ||
              Op.Opcode == NdOp::INT_SLESSEQUAL) {
            HasExplicitGuard = true;
            break;
          }
        }
        if (HasExplicitGuard)
          break;
      }
      if (!HasExplicitGuard)
        return false;
    }

    Info.GuardRange = Best;
    if (Info.MaxEntries == 0 ||
        static_cast<uint32_t>(RangeSize) < Info.MaxEntries)
      Info.MaxEntries = static_cast<uint32_t>(RangeSize);
    LLVM_DEBUG(llvm::dbgs() << "  range-guard: [" << Best.getMin() << ", "
                            << Best.getEnd() << ") size=" << RangeSize << "\n");
    return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// inferBoundsFromRangePullback — propagate a guard range onto the index
//===----------------------------------------------------------------------===//

/// Recover the entry count when the range guard constrains a *normalized* form
/// of the index that the direct comparison matchers cannot connect to the table
/// index register.
///
/// The direct matchers (findBestBound / refineRangeFromGuards) look for a
/// comparison whose operand is the index register or a value-preserving copy of
/// it.  They miss guards written against a value that was reshaped from the
/// index by a *chain* of operations — e.g. `t0 = idx + 3; t1 = t0 & 0xff;
/// cmp t1, N` — because no single copy-chain step bridges `t1` back to `idx`.
///
/// This strategy seeds the value range implied by each comparison and rewinds
/// it operation-by-operation back toward the index register, transforming the
/// range at every step.  Only *count-preserving* reshapes are followed —
/// copies, width changes (zero/sign-extend, low-half subpiece), constant
/// add/subtract offsets, and a contiguous low-bit mask — so the size of the
/// range that lands on the index register is exactly the number of distinct
/// index values, i.e. the table's entry count.  Scaling reshapes (shifts,
/// multiplies) are deliberately *not* followed: a shifted guard implies a
/// strided index whose distinct-value count is not the raw range size, and
/// mis-scaling it would fabricate wrong case edges.  The stride path is handled
/// separately by detectStride/detectNormalization.
bool CFGBuilder::inferBoundsFromRangePullback(const InsnRecord &Rec,
                                              JumpTableInfo &Info) {
  if (Info.IndexReg == InvalidVA ||
      (!Info.IndexValueAtUse.isReg() && !Info.IndexValueAtUse.isTemp()) ||
      Info.IndexValueAtUse.Size == 0 || Info.IndexUseAddr == InvalidVA ||
      Info.IndexUseSeq < 0 || Info.TableLoadAddr == InvalidVA)
    return false;

  int VarSize = getTargetRegInfo(CurrentImg ? CurrentImg->Arch : Arch::Unknown)
                    .PointerSize;

  // Flatten the function prefix through the dispatch so a guard and the index
  // normalization it constrains are both visible to the backward walk.
  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);

  auto sameVar = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Offset == B.Offset;
  };
  auto reachingDef = [&](const NdVar &V, int Before) -> int {
    for (int I = Before; I >= 0 && I < static_cast<int>(Ops.size()); --I)
      if (sameVar(Ops[I].Output, V))
        return I;
    return -1;
  };

  // Safety gate: only comparisons whose result reaches a COND_BR are guards.
  // An equality/range test buried in a case body (`if (idx==5)`) is not a
  // dispatch bound, and seeding from it would fabricate a wrong entry count.
  // Mark each comparison op index whose boolean output flows (through BOOL_*/
  // COPY) into some COND_BR condition earlier in the prefix.
  std::set<int> GuardCmp;
  for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
    if (Ops[I].Opcode != NdOp::COND_BR || Ops[I].NumInputs < 2)
      continue;
    if (!branchControlsTableLoad(Ops[I].Addr, Info))
      continue;
    std::vector<std::pair<NdVar, int>> Work{{Ops[I].Inputs[1], I - 1}};
    std::set<int> Seen;
    int Steps = 0;
    while (!Work.empty() && Steps++ < limits::kMaxGuardScanOps) {
      auto [V, Before] = Work.back();
      Work.pop_back();
      if (V.isConst())
        continue;
      int D = reachingDef(V, Before);
      if (D < 0 || !Seen.insert(D).second)
        continue;
      const LowOp &Def = Ops[D];
      switch (Def.Opcode) {
      case NdOp::INT_LESS:
      case NdOp::INT_SLESS:
      case NdOp::INT_LESSEQUAL:
      case NdOp::INT_SLESSEQUAL:
        GuardCmp.insert(D);
        break;
      case NdOp::BOOL_AND:
      case NdOp::BOOL_OR:
      case NdOp::BOOL_XOR:
      case NdOp::BOOL_NOT:
      case NdOp::COPY:
        for (int K = 0; K < Def.NumInputs; ++K)
          Work.push_back({Def.Inputs[K], D - 1});
        break;
      default:
        break;
      }
    }
  }
  if (GuardCmp.empty())
    return false;

  auto isIndexValue = [&](const NdVar &V, int Before) -> bool {
    int UseIdx = Before + 1;
    if (UseIdx < 0 || UseIdx >= static_cast<int>(Ops.size()))
      return false;
    return tableIndexMatchesValueAtUse(V, Ops[UseIdx].Addr, Ops[UseIdx].Seq,
                                       Info,
                                       /*AllowZeroExtension=*/true,
                                       /*AllowSignExtension=*/true);
  };

  uint32_t Best = 0;
  for (int GI : GuardCmp) {
    const LowOp &Cmp = Ops[GI];
    int I = GI;
    if (Cmp.NumInputs < 1 ||
        (!Cmp.Inputs[0].isReg() && !Cmp.Inputs[0].isTemp()))
      continue;
    int SeedSize = Cmp.Inputs[0].Size > 0 ? Cmp.Inputs[0].Size : VarSize;
    CircleRange R = extractGuardRange(Ops, Cmp, SeedSize);
    if (R.isEmpty() || R.isFull())
      continue;

    // Rewind R from the compared value back to the index register.
    NdVar V = Cmp.Inputs[0];
    int From = I - 1;
    bool Reached = false;
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (isIndexValue(V, From)) {
        Reached = true;
        break;
      }
      if (!V.isReg() && !V.isTemp())
        break;
      int D = reachingDef(V, From);
      if (D < 0)
        break;
      const LowOp &Def = Ops[D];
      int OutSize = Def.Output.Size > 0 ? Def.Output.Size : VarSize;
      bool HasSrc = Def.NumInputs >= 1 &&
                    (Def.Inputs[0].isReg() || Def.Inputs[0].isTemp());
      if ((Def.Opcode == NdOp::COPY || Def.Opcode == NdOp::INT_ZEXT ||
           Def.Opcode == NdOp::INT_SEXT) &&
          HasSrc) {
        int InSize = Def.Inputs[0].Size > 0 ? Def.Inputs[0].Size : VarSize;
        if (!R.pullBackUnary(Def.Opcode, InSize, OutSize))
          break;
        V = Def.Inputs[0];
        From = D - 1;
      } else if (Def.Opcode == NdOp::SUBBYTES && Def.NumInputs >= 2 &&
                 Def.Inputs[1].isConst() && Def.Inputs[1].Offset == 0 &&
                 (Def.Inputs[0].isReg() || Def.Inputs[0].isTemp())) {
        int InSize = Def.Inputs[0].Size > 0 ? Def.Inputs[0].Size : VarSize;
        if (!R.pullBackUnary(NdOp::SUBBYTES, InSize, OutSize))
          break;
        V = Def.Inputs[0];
        From = D - 1;
      } else if ((Def.Opcode == NdOp::INT_ADD || Def.Opcode == NdOp::INT_SUB ||
                  Def.Opcode == NdOp::INT_AND) &&
                 Def.NumInputs >= 2) {
        int CW =
            Def.Inputs[1].isConst() ? 1 : (Def.Inputs[0].isConst() ? 0 : -1);
        // Only a constant offset/mask reshape preserves the distinct-value
        // count; ADD is commutative but SUB/AND with the constant in slot 0
        // is a reverse subtract / masked-constant that we do not invert here.
        if (CW < 0)
          break;
        if ((Def.Opcode == NdOp::INT_SUB || Def.Opcode == NdOp::INT_AND) &&
            CW != 1)
          break;
        const NdVar &Keep = Def.Inputs[1 - CW];
        if (!Keep.isReg() && !Keep.isTemp())
          break;
        int InSize = Keep.Size > 0 ? Keep.Size : VarSize;
        if (!R.pullBackBinary(Def.Opcode, Def.Inputs[CW].Offset, 1 - CW, InSize,
                              OutSize))
          break;
        V = Keep;
        From = D - 1;
      } else {
        break; // a scaling or otherwise non-count-preserving op: stop
      }
      if (R.isEmpty() || R.isFull())
        break;
    }

    if (!Reached || R.isEmpty() || R.isFull())
      continue;
    uint64_t Sz = R.getSize();
    if (Sz >= limits::kMinJumpTableEntries &&
        Sz <= limits::kMaxJumpTableEntries) {
      if (Best == 0 || static_cast<uint32_t>(Sz) < Best)
        Best = static_cast<uint32_t>(Sz);
    }
  }

  if (Best == 0)
    return false;
  if (Info.MaxEntries == 0 || Best < Info.MaxEntries) {
    Info.MaxEntries = Best;
    LLVM_DEBUG(llvm::dbgs() << "  range-pullback: bound " << Best
                            << " propagated onto index register\n");
    return true;
  }
  return false;
}

} // namespace neverd
