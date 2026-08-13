//===- JumpTableResolverGuards.cpp - Switch-bound guard analysis ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guard- and bounds-analysis strategies for jump-table resolution: recover a
/// switch's entry count from the range guard that bounds the index, reading it
/// both from the dispatching instruction's own micro-ops and from comparisons
/// in CFG predecessor blocks, including duplicated and dual-path guards.  All
/// strategies here are architecture-neutral.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// the top-level dispatch and JumpTableResolverDetail.h for the shared
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

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

//===----------------------------------------------------------------------===//
// quasiCopySource — trace a variable through copy-like operations
//===----------------------------------------------------------------------===//

/// Follow a quasi-copy chain backwards through the ops in a single
/// instruction record.  A quasi-copy is a sequence of ops that preserves
/// the least-significant bits of a value while potentially zeroing or
/// setting upper bits: COPY, INT_AND (power-of-2 mask), INT_OR (upper
/// bits only), INT_ZEXT, INT_SEXT, SUBBYTES(offset=0), CONCAT (low half).
///
/// Returns the RegOff of the earliest register input in the chain, or
/// the original RegOff if no chain was found.
uint64_t quasiCopySource(const std::vector<LowOp> &Ops, int StartIdx,
                         uint64_t RegOff) {
  uint64_t Cur = RegOff;
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    bool Advanced = false;
    for (int I = StartIdx; I >= 0; --I) {
      auto &Op = Ops[I];
      if (!Op.Output.isReg() || Op.Output.Offset != Cur)
        continue;
      switch (Op.Opcode) {
      case NdOp::COPY:
        if (Op.NumInputs >= 1 && Op.Inputs[0].isReg()) {
          Cur = Op.Inputs[0].Offset;
          StartIdx = I - 1;
          Advanced = true;
        }
        break;
      case NdOp::INT_AND:
        if (Op.NumInputs >= 2 && Op.Inputs[0].isReg() &&
            Op.Inputs[1].isConst()) {
          uint64_t Mask = Op.Inputs[1].Offset;
          if (Mask > 0 && (Mask & (Mask + 1)) == 0) {
            Cur = Op.Inputs[0].Offset;
            StartIdx = I - 1;
            Advanced = true;
          }
        }
        break;
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
        if (Op.NumInputs >= 1 && Op.Inputs[0].isReg()) {
          Cur = Op.Inputs[0].Offset;
          StartIdx = I - 1;
          Advanced = true;
        }
        break;
      case NdOp::SUBBYTES:
        if (Op.NumInputs >= 2 && Op.Inputs[0].isReg() &&
            Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0) {
          Cur = Op.Inputs[0].Offset;
          StartIdx = I - 1;
          Advanced = true;
        }
        break;
      case NdOp::INT_OR:
        if (Op.NumInputs >= 2 && Op.Inputs[0].isReg() &&
            Op.Inputs[1].isConst()) {
          Cur = Op.Inputs[0].Offset;
          StartIdx = I - 1;
          Advanced = true;
        }
        break;
      case NdOp::CONCAT:
        if (Op.NumInputs >= 2 && Op.Inputs[1].isReg()) {
          Cur = Op.Inputs[1].Offset;
          StartIdx = I - 1;
          Advanced = true;
        }
        break;
      default:
        break;
      }
      if (Advanced)
        break;
    }
    if (!Advanced)
      break;
  }
  return Cur;
}

//===----------------------------------------------------------------------===//
// extractBoundFromOp — extract a bound value from a comparison/mask op
//===----------------------------------------------------------------------===//

static bool extractBoundFromOp(const LowOp &Op, uint64_t &Bound) {
  switch (Op.Opcode) {
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
    if (Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
      Bound = Op.Inputs[1].Offset;
      return true;
    }
    break;

  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
    if (Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
      Bound = Op.Inputs[1].Offset + 1;
      return true;
    }
    break;

  case NdOp::INT_SUB:
    if (Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
      Bound = Op.Inputs[1].Offset + 1;
      return true;
    }
    break;

  case NdOp::INT_AND:
    if (Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
      uint64_t Mask = Op.Inputs[1].Offset;
      if (Mask > 0 && (Mask & (Mask + 1)) == 0) {
        Bound = Mask + 1;
        return true;
      }
    }
    break;

  case NdOp::INT_EQUAL:
    if (Op.NumInputs >= 2 && Op.Inputs[1].isConst()) {
      uint64_t Val = Op.Inputs[1].Offset;
      if (Val > 0 && Val + 1 <= limits::kMaxJumpTableEntries) {
        Bound = Val + 1;
        return true;
      }
    }
    break;

  case NdOp::INT_NOTEQUAL:
    break;

  default:
    break;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// guardConstrainsIndex — confine a bound to the known switch variable
//===----------------------------------------------------------------------===//

/// True when the guard op constrains the table index register, so that
/// unrelated masks (e.g. a parity-flag `and x,1`) are not mistaken for the
/// table bound.  When the index register is unknown the op always qualifies.
// traceValueToReg — temp-aware variant of quasiCopySource.  -O0 routes a
// comparison through temps (`COPY t,reg; cmp _,t`), which the register-only
// quasiCopySource cannot bridge, so follow copy-like ops across both temp and
// register outputs back to the underlying register.
static uint64_t traceValueToReg(const std::vector<LowOp> &Ops, NdVar V) {
  for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
    if (V.isReg())
      return V.Offset;
    if (!V.isTemp())
      return InvalidVA;
    int D = -1;
    for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I)
      if (Ops[I].Output.Space == V.Space && Ops[I].Output.Offset == V.Offset) {
        D = I;
        break;
      }
    if (D < 0)
      return InvalidVA;
    const LowOp &O = Ops[D];
    bool Step = false;
    if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
         O.Opcode == NdOp::INT_SEXT) &&
        O.NumInputs >= 1 && (O.Inputs[0].isReg() || O.Inputs[0].isTemp()))
      Step = true;
    else if (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
             O.Inputs[1].isConst() && O.Inputs[1].Offset == 0 &&
             (O.Inputs[0].isReg() || O.Inputs[0].isTemp()))
      Step = true;
    else if (O.Opcode == NdOp::INT_AND && O.NumInputs >= 2 &&
             O.Inputs[1].isConst() &&
             (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
      uint64_t M = O.Inputs[1].Offset;
      if (M > 0 && (M & (M + 1)) == 0)
        Step = true;
    }
    if (!Step)
      return InvalidVA;
    V = O.Inputs[0];
  }
  return InvalidVA;
}

// resolveConstThroughCopy — read a constant operand, seeing through a -O0
// const-into-temp/reg materialization (`COPY t,#k; cmp x,t`).  `Before` is the
// index to scan backward from in `Ops`.
static bool resolveConstThroughCopy(const std::vector<LowOp> &Ops, int Before,
                                    const NdVar &V, uint64_t &Out) {
  if (V.isConst()) {
    Out = V.Offset;
    return true;
  }
  if (!V.isReg() && !V.isTemp())
    return false;
  for (int I = Before; I >= 0 && I < static_cast<int>(Ops.size()); --I) {
    const LowOp &D = Ops[I];
    if (D.Output.Space != V.Space || D.Output.Offset != V.Offset)
      continue;
    if (D.Opcode == NdOp::COPY && D.NumInputs >= 1 && D.Inputs[0].isConst()) {
      Out = D.Inputs[0].Offset;
      return true;
    }
    return false;
  }
  return false;
}

static bool guardConstrainsIndex(const std::vector<LowOp> &RecOps,
                                 const LowOp &Op, uint64_t IndexReg) {
  if (IndexReg == InvalidVA)
    return true;
  if (Op.Output.isReg() && Op.Output.Offset == IndexReg)
    return true;
  if (Op.NumInputs >= 1 && Op.Inputs[0].isReg() &&
      Op.Inputs[0].Offset == IndexReg)
    return true;
  if (Op.NumInputs >= 1 && (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
    uint64_t Src = quasiCopySource(RecOps, static_cast<int>(RecOps.size()) - 1,
                                   Op.Inputs[0].Offset);
    if (Src == IndexReg)
      return true;
    if (traceValueToReg(RecOps, Op.Inputs[0]) == IndexReg)
      return true;
  }
  return false;
}

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
  int VarSize =
      getTargetRegInfo(CurrentImg ? CurrentImg->Arch : Arch::Unknown)
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
// guardUsesInclusiveCompare — COND_BR-polarity-aware off-by-one recovery
//===----------------------------------------------------------------------===//

/// clang lowers `idx > N -> default` as `cmp idx,N; ja default`, so the table
/// covers idx in [0, N] = N+1 entries.  The lifted CF flag is `idx < N`, which
/// the range analysis reports as only N.  The strict `ja`/`jbe` family also
/// consumes the ZF equality `idx == N`; the `jae`/`jb` family consumes only CF.
/// Return true when the guarding COND_BR transitively consumes both, so the
/// inclusive upper bound is Bound+1.
bool CFGBuilder::guardUsesInclusiveCompare(const InsnRecord &Rec,
                                           uint64_t IndexReg,
                                           uint64_t Bound) const {
  std::vector<LowOp> Ops;
  for (auto &[A, IR] : Insns) {
    if (A >= Rec.Addr)
      break;
    for (auto &Op : IR.Ops)
      Ops.push_back(Op);
  }

  auto sameVar = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Offset == B.Offset;
  };
  auto reachingDef = [&](const NdVar &V, int Before) -> int {
    for (int I = Before; I >= 0 && I < static_cast<int>(Ops.size()); --I)
      if (sameVar(Ops[I].Output, V))
        return I;
    return -1;
  };
  // Trace a register/temp back to the index register through value-preserving
  // ops (copy / extend / low-half subpiece / mask).
  auto isIndex = [&](NdVar V, int Before) -> bool {
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (V.isReg() && V.Offset == IndexReg)
        return true;
      if (!V.isReg() && !V.isTemp())
        return false;
      int D = reachingDef(V, Before);
      if (D < 0)
        return false;
      const LowOp &Op = Ops[D];
      switch (Op.Opcode) {
      case NdOp::COPY:
      case NdOp::INT_ZEXT:
      case NdOp::INT_SEXT:
      case NdOp::INT_AND:
        if (Op.NumInputs < 1)
          return false;
        V = Op.Inputs[0];
        Before = D - 1;
        break;
      case NdOp::SUBBYTES:
        if (Op.NumInputs < 2 || !Op.Inputs[1].isConst() ||
            Op.Inputs[1].Offset != 0)
          return false;
        V = Op.Inputs[0];
        Before = D - 1;
        break;
      default:
        return false;
      }
    }
    return false;
  };
  auto isLessBound = [&](const LowOp &Op, int At) -> bool {
    if ((Op.Opcode != NdOp::INT_LESS && Op.Opcode != NdOp::INT_SLESS) ||
        Op.NumInputs < 2)
      return false;
    uint64_t C;
    if (!resolveConstThroughCopy(Ops, At - 1, Op.Inputs[1], C))
      return false;
    return C == Bound && isIndex(Op.Inputs[0], At - 1);
  };
  auto isEqualBound = [&](const LowOp &Op, int At) -> bool {
    if (Op.Opcode != NdOp::INT_EQUAL || Op.NumInputs < 2 ||
        !Op.Inputs[1].isConst())
      return false;
    if (Op.Inputs[1].Offset == Bound && isIndex(Op.Inputs[0], At - 1))
      return true;
    // ZF of `cmp idx,Bound` is `(idx - Bound) == 0`; the subtraction result may
    // live in a temp or, on x86 where `cmp` overwrites the register, a
    // register.
    if (Op.Inputs[1].Offset == 0 &&
        (Op.Inputs[0].isTemp() || Op.Inputs[0].isReg())) {
      int D = reachingDef(Op.Inputs[0], At - 1);
      if (D >= 0 && Ops[D].Opcode == NdOp::INT_SUB && Ops[D].NumInputs >= 2 &&
          Ops[D].Inputs[1].isConst() && Ops[D].Inputs[1].Offset == Bound &&
          isIndex(Ops[D].Inputs[0], D - 1))
        return true;
    }
    return false;
  };

  for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
    if (Ops[I].Opcode != NdOp::COND_BR || Ops[I].NumInputs < 2)
      continue;
    bool SawLess = false, SawEqual = false;
    std::vector<std::pair<NdVar, int>> Work{{Ops[I].Inputs[1], I - 1}};
    std::set<int> SeenDefs;
    int Steps = 0;
    while (!Work.empty() && Steps++ < limits::kMaxGuardScanOps) {
      auto [V, Before] = Work.back();
      Work.pop_back();
      if (V.isConst())
        continue;
      int D = reachingDef(V, Before);
      // Dedup by definition site, not nd-var identity: a temp offset may be
      // reused for distinct defs (ARM flag chains), each needing its own walk.
      if (D < 0 || !SeenDefs.insert(D).second)
        continue;
      const LowOp &Def = Ops[D];
      if (isLessBound(Def, D)) {
        SawLess = true;
        continue;
      }
      if (isEqualBound(Def, D)) {
        SawEqual = true;
        continue;
      }
      switch (Def.Opcode) {
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
    if (SawLess && SawEqual)
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// findBestBound — scan a range of ops for the tightest bound
//===----------------------------------------------------------------------===//

uint32_t findBestBound(const std::vector<LowOp> &Ops, uint32_t Current,
                       uint64_t IndexReg) {
  uint32_t Best = Current;
  for (auto &Op : Ops) {
    uint64_t Bound = 0;
    if (!extractBoundFromOp(Op, Bound))
      continue;
    if (!guardConstrainsIndex(Ops, Op, IndexReg))
      continue;
    // An `INT_SUB idx, const` that writes the index register itself is the
    // index's own computation (case-base normalization `idx-1`, or the index
    // register reused as a loop counter `subs idx,idx,1` after the table load),
    // not a range guard on it.  extractBoundFromOp would otherwise read its
    // constant as a phantom bound of `const+1` (a peeled `switch` whose
    // dispatch has no `cmp` guard then collapses to that bogus 2-entry bound).
    // A genuine `cmp idx, N` lowers to a SUB whose output is a flag temp, never
    // the index.
    if (Op.Opcode == NdOp::INT_SUB && IndexReg != InvalidVA &&
        Op.Output.isReg() && Op.Output.Offset == IndexReg)
      continue;
    if (Bound > 1 && Bound <= limits::kMaxJumpTableEntries) {
      if (Best == 0 || Bound < Best)
        Best = static_cast<uint32_t>(Bound);
    }
  }
  return Best;
}

//===----------------------------------------------------------------------===//
// traceCompoundGuard — trace comparison input through normalization ops
//===----------------------------------------------------------------------===//

/// Detect compound guard patterns where the switch variable is normalized
/// (INT_SUB, INT_ADD, INT_RIGHT) before a comparison.  For example:
///
///   INT_SUB  x, 5    → tmp
///   INT_LESS tmp, 10 → bool     // raw bound = 10
///   COND_BR  bool
///
/// The actual bound on x is 5+10 = 15 (table has 15 entries starting from 0,
/// or 10 entries starting from case 5).
///
/// Propagate value ranges backward through the computation chain.
uint32_t CFGBuilder::traceCompoundGuard(const std::vector<LowOp> &Ops) const {
  for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I) {
    auto &CmpOp = Ops[I];
    uint64_t RawBound = 0;
    if (!extractBoundFromOp(CmpOp, RawBound))
      continue;
    if (RawBound <= 1 || RawBound > limits::kMaxJumpTableEntries)
      continue;

    // The comparison constrains CmpOp.Inputs[0].  Trace that register
    // backward through the ops to find normalization (SUB/ADD/SHIFT).
    if (CmpOp.NumInputs < 1 || !CmpOp.Inputs[0].isReg())
      continue;

    uint64_t CmpInputReg = CmpOp.Inputs[0].Offset;
    uint32_t Adjusted = static_cast<uint32_t>(RawBound);

    for (int J = I - 1; J >= 0; --J) {
      auto &PrevOp = Ops[J];
      if (!PrevOp.Output.isReg() || PrevOp.Output.Offset != CmpInputReg)
        continue;

      if (PrevOp.Opcode == NdOp::INT_SUB && PrevOp.NumInputs >= 2 &&
          PrevOp.Inputs[1].isConst()) {
        uint64_t Base = PrevOp.Inputs[1].Offset;
        if (Base > 0 && Base + Adjusted <= limits::kMaxJumpTableEntries) {
          Adjusted = static_cast<uint32_t>(Base + Adjusted);
          LLVM_DEBUG(llvm::dbgs()
                     << "  compound-guard: INT_SUB " << Base << " + bound "
                     << RawBound << " = " << Adjusted << "\n");
        }
        if (PrevOp.Inputs[0].isReg()) {
          CmpInputReg = PrevOp.Inputs[0].Offset;
          continue; // keep walking the chain to the next normalization
        }
      } else if (PrevOp.Opcode == NdOp::INT_ADD && PrevOp.NumInputs >= 2 &&
                 PrevOp.Inputs[1].isConst()) {
        int64_t Offset = static_cast<int64_t>(PrevOp.Inputs[1].Offset);
        // Widen before subtracting: on uint32_t, `Adjusted - Delta >= 2` wraps
        // to a huge value whenever Delta exceeds the bound, which then escapes
        // as the result and aborts the search over the remaining compares.
        uint64_t Delta = 0ULL - static_cast<uint64_t>(Offset);
        if (Offset < 0 && Adjusted >= Delta + 2) {
          Adjusted -= static_cast<uint32_t>(Delta);
          LLVM_DEBUG(llvm::dbgs()
                     << "  compound-guard: INT_ADD " << Offset << " adjusted "
                     << "bound to " << Adjusted << "\n");
        }
        if (PrevOp.Inputs[0].isReg()) {
          CmpInputReg = PrevOp.Inputs[0].Offset;
          continue;
        }
      } else if (PrevOp.Opcode == NdOp::COPY && PrevOp.NumInputs >= 1 &&
                 PrevOp.Inputs[0].isReg()) {
        CmpInputReg = PrevOp.Inputs[0].Offset;
        continue;
      }
      // Chain ends here: the definition is not a recognised normalization, or
      // its source is not a register we can keep following.
      break;
    }

    if (Adjusted != static_cast<uint32_t>(RawBound))
      return Adjusted;
  }
  return 0;
}

//===----------------------------------------------------------------------===//
// inferBoundsFromGuard — local COND_BR guard range analysis
//===----------------------------------------------------------------------===//

bool CFGBuilder::inferBoundsFromGuard(const InsnRecord &Rec,
                                      JumpTableInfo &Info) {
  uint32_t Best = 0;
  int OpsScanned = 0;

  uint64_t SwitchReg = InvalidVA;
  for (auto &Op : Rec.Ops) {
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1 &&
        Op.Inputs[0].isReg()) {
      SwitchReg = Op.Inputs[0].Offset;
      break;
    }
  }

  // The SwitchReg-keyed fallbacks below match a guard by comparing the guard's
  // (quasi-copied) source register against the INDIR_BR target register.  That
  // is only meaningful when the real index register is unknown: two unrelated
  // values that merely share a physical register offset at different program
  // points then falsely match (e.g. x86 EAX and RAX both live at offset 0, so
  // an outer switch's `and eax,3` guard aliases an inner switch's `jmp rax`
  // target and collapses the inner table to 4 entries).  When Info.IndexReg is
  // known, findBestBound already matches guards against the genuine index, so
  // gate the looser heuristics off to avoid the false positive.
  const bool IndexKnown = Info.IndexReg != InvalidVA;
  for (auto &[Addr, IRec] : Insns) {
    if (Addr >= Rec.Addr)
      break;

    Best = findBestBound(IRec.Ops, Best, Info.IndexReg);

    // Compound guard: trace comparison inputs through normalization ops.
    if (SwitchReg != InvalidVA) {
      uint32_t Compound = traceCompoundGuard(IRec.Ops);
      if (Compound > 1 && Compound <= limits::kMaxJumpTableEntries) {
        if (Best == 0 || Compound < Best)
          Best = Compound;
      }
    }

    // Quasi-copy chain matching: a guard op may constrain a register
    // that is copy-chained to the switch variable.
    if (Best == 0 && SwitchReg != InvalidVA && !IndexKnown) {
      for (auto &Op : IRec.Ops) {
        uint64_t Bound = 0;
        if (!extractBoundFromOp(Op, Bound))
          continue;
        if (Bound <= 1 || Bound > limits::kMaxJumpTableEntries)
          continue;
        if (Op.NumInputs >= 1 && Op.Inputs[0].isReg()) {
          uint64_t GuardSrc =
              quasiCopySource(IRec.Ops, static_cast<int>(IRec.Ops.size()) - 1,
                              Op.Inputs[0].Offset);
          uint64_t SwitchSrc = quasiCopySource(
              Rec.Ops, static_cast<int>(Rec.Ops.size()) - 1, SwitchReg);
          if (GuardSrc == SwitchSrc) {
            if (Best == 0 || Bound < Best)
              Best = static_cast<uint32_t>(Bound);
          }
        }
      }
    }

    // LOAD alias matching: when the guard variable and switch variable
    // were both loaded from the same memory address, they hold the same
    // value even if they reside in different registers.
    if (Best == 0 && SwitchReg != InvalidVA && !IndexKnown) {
      uint64_t SwitchLoadAddr = InvalidVA;
      for (auto &Op : Rec.Ops) {
        if (Op.Opcode == NdOp::LOAD && Op.Output.isReg() &&
            Op.Output.Offset == SwitchReg && Op.NumInputs >= 1 &&
            Op.Inputs[0].isConst()) {
          SwitchLoadAddr = Op.Inputs[0].Offset;
          break;
        }
      }
      if (SwitchLoadAddr != InvalidVA) {
        for (auto &Op : IRec.Ops) {
          uint64_t Bound = 0;
          if (!extractBoundFromOp(Op, Bound))
            continue;
          if (Bound <= 1 || Bound > limits::kMaxJumpTableEntries)
            continue;
          if (Op.NumInputs < 1 || !Op.Inputs[0].isReg())
            continue;
          uint64_t GuardReg = Op.Inputs[0].Offset;
          for (auto &LOp : IRec.Ops) {
            if (LOp.Opcode == NdOp::LOAD && LOp.Output.isReg() &&
                LOp.Output.Offset == GuardReg && LOp.NumInputs >= 1 &&
                LOp.Inputs[0].isConst() &&
                LOp.Inputs[0].Offset == SwitchLoadAddr) {
              if (Best == 0 || Bound < Best)
                Best = static_cast<uint32_t>(Bound);
              break;
            }
          }
        }
      }
    }

    OpsScanned += static_cast<int>(IRec.Ops.size());
    if (OpsScanned > limits::kMaxGuardScanOps)
      break;
  }

  if (Best > 0) {
    Info.MaxEntries = Best;
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
  if (Info.IndexReg == InvalidVA)
    return false;

  int VarSize =
      getTargetRegInfo(CurrentImg ? CurrentImg->Arch : Arch::Unknown)
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

  // The index register may itself be aliased from an earlier source register
  // (`mov ecx,edi`); normalize both sides to their ultimate source so a guard
  // on the original register still matches.
  uint64_t IndexSrc =
      traceRegSource(Ops, static_cast<int>(Ops.size()) - 1, Info.IndexReg);
  auto isIndexReg = [&](uint64_t RegOff, int Before) -> bool {
    if (RegOff == Info.IndexReg)
      return true;
    return traceRegSource(Ops, Before, RegOff) == IndexSrc;
  };

  uint32_t Best = 0;
  for (int GI : GuardCmp) {
    const LowOp &Cmp = Ops[GI];
    int I = GI;
    if (Cmp.NumInputs < 1 || (!Cmp.Inputs[0].isReg() && !Cmp.Inputs[0].isTemp()))
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
      if (V.isReg() && isIndexReg(V.Offset, From)) {
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
        int CW = Def.Inputs[1].isConst() ? 1 : (Def.Inputs[0].isConst() ? 0 : -1);
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
    LLVM_DEBUG(llvm::dbgs()
               << "  range-pullback: bound " << Best
               << " propagated onto index register\n");
    return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// inferBoundsFromLoadAliasGuard — bound via same-location reload equivalence
//===----------------------------------------------------------------------===//

bool CFGBuilder::inferBoundsFromLoadAliasGuard(const InsnRecord &Rec,
                                               JumpTableInfo &Info) {
  if (Info.IndexReg == InvalidVA || !CurrentImg)
    return false;

  const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);
  int VarSize = TRI.PointerSize;

  // Flatten the function prefix through the dispatch so the guard, its compared
  // reload, and the index's own reload are all visible to the backward walk.
  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);
  if (Ops.empty())
    return false;

  // A location key for the value feeding a register: either a fixed address
  // (Kind 0, read-only source) or a stack/frame slot (Kind 1).
  struct MemKey {
    int Kind = -1;
    uint64_t Addr = 0;
    uint64_t Base = 0;
    int64_t Off = 0;
    int LoadIdx = -1;
  };

  // Trace a value backward through value-preserving reshapes (copy / extend /
  // low-half subpiece) to the LOAD that produced it, and key that load's
  // address.  Returns nullopt if the value is not a plain reload.
  auto keyOfLoadFeeding = [&](NdVar V, int From) -> std::optional<MemKey> {
    for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
      int D = reachingDefIdx(Ops, From, V);
      if (D < 0)
        return std::nullopt;
      const LowOp &O = Ops[D];
      if (O.Opcode == NdOp::LOAD && O.NumInputs >= 1) {
        const NdVar &A = (O.NumInputs >= 2) ? O.Inputs[1] : O.Inputs[0];
        MemKey K;
        K.LoadIdx = D;
        if (A.isConst()) {
          K.Kind = 0;
          K.Addr = A.Offset;
          return K;
        }
        uint64_t B = InvalidVA;
        int64_t Off = 0;
        if (frameSlotKey(Ops, D - 1, A, TRI, B, Off)) {
          K.Kind = 1;
          K.Base = B;
          K.Off = Off;
          return K;
        }
        return std::nullopt;
      }
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
      return std::nullopt;
    }
    return std::nullopt;
  };

  auto sameKey = [](const MemKey &A, const MemKey &B) {
    if (A.Kind != B.Kind)
      return false;
    return A.Kind == 0 ? (A.Addr == B.Addr)
                       : (A.Base == B.Base && A.Off == B.Off);
  };

  std::optional<MemKey> IdxKey = keyOfLoadFeeding(
      NdVar::reg(Info.IndexReg, VarSize), static_cast<int>(Ops.size()) - 1);
  if (!IdxKey)
    return false;

  // A fixed-address source is only a stable value if it lives in a non-writable
  // segment (a store could otherwise change it between the two reads); a frame
  // slot's stability is checked per-guard by the no-intervening-store test.
  if (IdxKey->Kind == 0) {
    const auto *Seg = CurrentImg->getSegmentFor(IdxKey->Addr);
    if (!Seg || Seg->isWritable() || Seg->isExecutable())
      return false;
  }

  auto sameSlotStoreBetween = [&](int A, int B) -> bool {
    int Lo = std::min(A, B);
    int Hi = std::max(A, B);
    for (int I = Lo + 1; I < Hi; ++I) {
      const LowOp &S = Ops[I];
      if (S.Opcode != NdOp::STORE || S.NumInputs < 2)
        continue;
      uint64_t B2 = InvalidVA;
      int64_t Off2 = 0;
      if (frameSlotKey(Ops, I - 1, S.Inputs[0], TRI, B2, Off2) &&
          B2 == IdxKey->Base && Off2 == IdxKey->Off)
        return true;
    }
    return false;
  };

  // Only comparisons whose boolean reaches a conditional branch are guards; an
  // equality/range test buried in a case body is not a dispatch bound.  Mark
  // each range-compare op index whose result flows (through BOOL_*/COPY) into a
  // COND_BR condition.
  std::set<int> GuardCmp;
  for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
    if (Ops[I].Opcode != NdOp::COND_BR || Ops[I].NumInputs < 2)
      continue;
    std::vector<std::pair<NdVar, int>> Work{{Ops[I].Inputs[1], I - 1}};
    std::set<int> Seen;
    int Steps = 0;
    while (!Work.empty() && Steps++ < limits::kMaxGuardScanOps) {
      auto [V, Before] = Work.back();
      Work.pop_back();
      if (V.isConst())
        continue;
      int D = -1;
      for (int K = Before; K >= 0 && K < static_cast<int>(Ops.size()); --K)
        if (Ops[K].Output.Space == V.Space && Ops[K].Output.Offset == V.Offset) {
          D = K;
          break;
        }
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
        for (int L = 0; L < Def.NumInputs; ++L)
          Work.push_back({Def.Inputs[L], D - 1});
        break;
      default:
        break;
      }
    }
  }
  if (GuardCmp.empty())
    return false;

  auto boundFromCmp = [&](int GI) -> uint32_t {
    const LowOp &Op = Ops[GI];
    if (Op.NumInputs < 2)
      return 0;
    uint64_t C;
    if (!resolveConstThroughCopy(Ops, GI - 1, Op.Inputs[1], C))
      return 0;
    uint64_t Bound;
    switch (Op.Opcode) {
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
      Bound = C;
      break;
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
      Bound = C + 1;
      break;
    default:
      return 0;
    }
    if (Bound < limits::kMinJumpTableEntries ||
        Bound > limits::kMaxJumpTableEntries)
      return 0;
    return static_cast<uint32_t>(Bound);
  };

  uint32_t Best = 0;
  for (int GI : GuardCmp) {
    const LowOp &Cmp = Ops[GI];
    if (Cmp.NumInputs < 1 ||
        (!Cmp.Inputs[0].isReg() && !Cmp.Inputs[0].isTemp()))
      continue;
    std::optional<MemKey> GKey = keyOfLoadFeeding(Cmp.Inputs[0], GI - 1);
    if (!GKey || !sameKey(*GKey, *IdxKey))
      continue;
    // A store to the shared frame slot between the two reloads breaks the value
    // equivalence, so the guard no longer bounds the index.
    if (IdxKey->Kind == 1 &&
        sameSlotStoreBetween(GKey->LoadIdx, IdxKey->LoadIdx))
      continue;
    uint32_t Bnd = boundFromCmp(GI);
    if (Bnd == 0)
      continue;
    if (Best == 0 || Bnd < Best)
      Best = Bnd;
  }

  if (Best == 0)
    return false;
  if (Info.MaxEntries == 0 || Best < Info.MaxEntries) {
    Info.MaxEntries = Best;
    LLVM_DEBUG(llvm::dbgs()
               << "  load-alias-guard: bound " << Best
               << " from same-location reload of the index\n");
    return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// inferBoundsFromCFGGuards — walk CFG predecessor chain for guards
//===----------------------------------------------------------------------===//

/// Collect block-start addresses of all predecessor blocks that branch to
/// the given target address (either via direct branch or conditional
/// fall-through).
void CFGBuilder::collectPredBlocks(va_t TargetBlockStart,
                                   const std::set<va_t> &Visited,
                                   std::vector<va_t> &Out) const {
  for (auto &[Addr, IRec] : Insns) {
    if (!IRec.IsBranch || IRec.IsCall)
      continue;
    bool Targets = (IRec.BranchTarget == TargetBlockStart);
    if (IRec.IsCond && !Targets) {
      va_t Fall = Addr + IRec.Size;
      if (Fall == TargetBlockStart)
        Targets = true;
    }
    if (!Targets)
      continue;
    if (Visited.count(Addr))
      continue;
    auto PB = BlockStarts.upper_bound(Addr);
    if (PB != BlockStarts.begin()) {
      --PB;
      Out.push_back(*PB);
    }
  }
}

bool CFGBuilder::inferBoundsFromCFGGuards(const InsnRecord &Rec,
                                          JumpTableInfo &Info) {
  auto BlockIt = BlockStarts.upper_bound(Rec.Addr);
  if (BlockIt == BlockStarts.begin())
    return false;
  --BlockIt;
  va_t BranchBlockStart = *BlockIt;

  std::set<va_t> Visited;
  Visited.insert(BranchBlockStart);

  uint32_t Best = 0;

  std::vector<va_t> Worklist;
  collectPredBlocks(BranchBlockStart, Visited, Worklist);

  int Depth = 0;
  while (!Worklist.empty() && Depth < limits::kMaxGuardPredDepth) {
    std::vector<va_t> NextWorklist;
    for (va_t PredStart : Worklist) {
      if (!Visited.insert(PredStart).second)
        continue;

      auto NextBlock = BlockStarts.upper_bound(PredStart);
      va_t PredEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

      for (auto It = Insns.lower_bound(PredStart); It != Insns.end(); ++It) {
        if (It->first >= PredEnd)
          break;
        Best = findBestBound(It->second.Ops, Best, Info.IndexReg);
        uint32_t Compound = traceCompoundGuard(It->second.Ops);
        if (Compound > 1 && Compound <= limits::kMaxJumpTableEntries) {
          if (Best == 0 || Compound < Best)
            Best = Compound;
        }
      }

      if (Best == 0)
        collectPredBlocks(PredStart, Visited, NextWorklist);
    }
    Worklist = std::move(NextWorklist);
    ++Depth;
  }

  if (Best > 0 && (Info.MaxEntries == 0 || Best < Info.MaxEntries)) {
    Info.MaxEntries = Best;
    return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// tryDualPathRecovery — default-value path detection
//===----------------------------------------------------------------------===//

/// When the standard guard analysis fails to produce a bound, check for
/// a dual-path pattern: the block containing the INDIR_BR has two
/// predecessor paths, one carrying a default constant and one carrying
/// the real switch computation.  A COND_BR at the split point acts as
/// the guard for switches with an explicit default path.
bool CFGBuilder::tryDualPathRecovery(const InsnRecord &Rec,
                                     JumpTableInfo &Info) {
  auto BlockIt = BlockStarts.upper_bound(Rec.Addr);
  if (BlockIt == BlockStarts.begin())
    return false;
  --BlockIt;
  va_t BranchBlockStart = *BlockIt;

  // Collect all predecessor blocks that branch into our switch block.
  std::set<va_t> Visited;
  Visited.insert(BranchBlockStart);
  std::vector<va_t> Preds;
  collectPredBlocks(BranchBlockStart, Visited, Preds);

  if (Preds.size() < 2 || Preds.size() > limits::kMaxDualPathPreds)
    return false;

  // Look for the pattern: one predecessor ends with a COND_BR that
  // gates a constant-producing path vs. a computation path.
  // The COND_BR predecessor that has a bound comparison is our guard.
  uint32_t BestBound = 0;
  for (va_t PredStart : Preds) {
    auto NextBlock = BlockStarts.upper_bound(PredStart);
    va_t PredEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

    for (auto It = Insns.lower_bound(PredStart); It != Insns.end(); ++It) {
      if (It->first >= PredEnd)
        break;
      auto &IRec = It->second;
      if (!IRec.IsBranch || !IRec.IsCond)
        continue;

      // This pred has a COND_BR — scan its ops for a guard bound.
      uint32_t Bound = findBestBound(IRec.Ops, 0, Info.IndexReg);
      if (Bound > 0 && (BestBound == 0 || Bound < BestBound))
        BestBound = Bound;

      // Also scan the ops preceding the COND_BR in this block.
      for (auto InnerIt = Insns.lower_bound(PredStart); InnerIt != It;
           ++InnerIt) {
        Bound = findBestBound(InnerIt->second.Ops, BestBound, Info.IndexReg);
        if (Bound > 0 && (BestBound == 0 || Bound < BestBound))
          BestBound = Bound;
      }
    }
  }

  if (BestBound == 0)
    return false;

  Info.MaxEntries = BestBound;
  LLVM_DEBUG(llvm::dbgs() << "  dual-path: found guard bound " << BestBound
                          << " from " << Preds.size() << " predecessors\n");
  return true;
}

//===----------------------------------------------------------------------===//
// inferBoundsFromUnrolledGuard — detect duplicated guard across preds
//===----------------------------------------------------------------------===//

/// When multiple predecessor blocks each terminate with a COND_BR, and
/// each carries a guard comparison on the switch variable, the guard
/// has been "unrolled" (duplicated).  This detects that pattern and
/// extracts the tightest common bound.
bool CFGBuilder::inferBoundsFromUnrolledGuard(const InsnRecord &Rec,
                                              JumpTableInfo &Info) {
  auto BlockIt = BlockStarts.upper_bound(Rec.Addr);
  if (BlockIt == BlockStarts.begin())
    return false;
  --BlockIt;
  va_t BranchBlockStart = *BlockIt;

  std::set<va_t> Visited;
  Visited.insert(BranchBlockStart);
  std::vector<va_t> Preds;
  collectPredBlocks(BranchBlockStart, Visited, Preds);

  if (Preds.size() < 2 ||
      static_cast<int>(Preds.size()) > limits::kMaxUnrolledGuardPreds)
    return false;

  // Every predecessor must end with a COND_BR for this to be an
  // unrolled guard pattern.
  uint32_t CommonBound = 0;
  int CBranchCount = 0;

  for (va_t PredStart : Preds) {
    auto NextBlock = BlockStarts.upper_bound(PredStart);
    va_t PredEnd = (NextBlock != BlockStarts.end()) ? *NextBlock : InvalidVA;

    bool FoundCBranch = false;
    for (auto It = Insns.lower_bound(PredStart); It != Insns.end(); ++It) {
      if (It->first >= PredEnd)
        break;
      auto &IRec = It->second;
      if (!IRec.IsBranch || !IRec.IsCond)
        continue;
      FoundCBranch = true;
      ++CBranchCount;

      uint32_t PredBound = findBestBound(IRec.Ops, 0, Info.IndexReg);
      if (PredBound == 0) {
        for (auto InnerIt = Insns.lower_bound(PredStart); InnerIt != It;
             ++InnerIt)
          PredBound =
              findBestBound(InnerIt->second.Ops, PredBound, Info.IndexReg);
      }

      if (PredBound > 0) {
        if (CommonBound == 0 || PredBound < CommonBound)
          CommonBound = PredBound;
      }
    }
    if (!FoundCBranch)
      return false;
  }

  if (CBranchCount < 2 || CommonBound == 0)
    return false;

  if (Info.MaxEntries == 0 || CommonBound < Info.MaxEntries) {
    Info.MaxEntries = CommonBound;
    LLVM_DEBUG(llvm::dbgs()
               << "  unrolled-guard: found common bound " << CommonBound
               << " across " << Preds.size() << " predecessor COND_BRs\n");
    return true;
  }
  return false;
}

} // namespace neverd
