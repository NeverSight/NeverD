//===- JumpTableResolverGuards.cpp - Switch-bound guard analysis ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guard- and bounds-analysis strategies for jump-table resolution: recover a
/// switch's entry count from the range guard that bounds the index, reading it
/// from the dispatching instruction's own micro-ops and from the comparisons
/// that precede it.  This file holds the shared guard primitives — copy-chain
/// tracing, bound extraction from a comparison/mask op, and index ownership —
/// plus the direct and compound-normalization bound scans built on them.  The
/// remaining strategies live in sibling translation units: CircleRange range
/// analysis and range pull-back in JumpTableResolverGuardRange.cpp,
/// inclusive-compare polarity and reload-alias matching in
/// JumpTableResolverGuardAlias.cpp, and predecessor-block, dual-path and
/// duplicated guards in JumpTableResolverGuardCFG.cpp.  All strategies are
/// architecture-neutral.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// the top-level dispatch and JumpTableResolverDetail.h for the shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
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
bool resolveConstThroughCopy(const std::vector<LowOp> &Ops, int Before,
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

bool guardConstrainsIndex(const std::vector<LowOp> &RecOps, const LowOp &Op,
                          uint64_t IndexReg) {
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

} // namespace neverd
