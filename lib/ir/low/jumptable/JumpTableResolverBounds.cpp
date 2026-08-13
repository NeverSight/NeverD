//===- JumpTableResolverBounds.cpp - Entry-count bounds -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Entry-count bounds for jump tables that carry no comparison range guard:
/// relocation-run counting (absolute code-pointer and PC-relative-to-code
/// runs, capped at the next table anchor), index-mask bounds, modulo bounds
/// recovered from a magic-division remainder, and normalization pull-back of a
/// raw bound.  Comparison-guard bounds live in JumpTableResolverGuards.cpp.
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

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {

/// Count the run of consecutive absolute code-pointer relocation slots starting
/// at TableAddr, stepping by EntrySize.  The loader records such slots in
/// Img.CodePtrRelocSlots; the run length is the exact entry count of a
/// computed-goto / threaded-dispatch jump table.
uint32_t countCodePtrRelocRun(const BinaryImage &Img, va_t TableAddr,
                              uint16_t EntrySize) {
  if (EntrySize == 0 || Img.CodePtrRelocSlots.empty())
    return 0;
  uint32_t Run = 0;
  for (va_t VA = TableAddr; Run < limits::kMaxJumpTableEntries;
       VA += EntrySize) {
    if (!Img.CodePtrRelocSlots.count(VA))
      break;
    ++Run;
  }
  return Run;
}

/// Count the run of consecutive PC-relative-to-code relocation slots starting
/// at the table base — the entries of a PIC `switch` jump table.  The run
/// length is the exact entry count, which bounds a `switch(x % N)` table whose
/// modulus constrains the index with no `cmp` range guard.
uint32_t countRelCodeRelocRun(const BinaryImage &Img, va_t TableAddr,
                              uint16_t EntrySize) {
  if (EntrySize == 0 || Img.RelCodeRelocSlots.empty())
    return 0;
  uint32_t Run = 0;
  for (va_t VA = TableAddr; Run < limits::kMaxJumpTableEntries;
       VA += EntrySize) {
    if (!Img.RelCodeRelocSlots.count(VA))
      break;
    ++Run;
  }
  return Run;
}

/// Truncate a RelCodeReloc entry run so it stops at the next PIC jump-table
/// base anchor.  Two unguarded PIC tables laid out back-to-back in rodata share
/// one continuous RelCodeReloc entry run, so a raw run count from the first
/// table's base over-reads past its end into the second — recovering bogus
/// successor edges (each an entry of the second table decoded relative to the
/// first base) that misroute the first dispatch (§15.2 adjacent-unguarded-pic-
/// table).  The next table's own base anchor — a rodata VA a `lea`/`adrp+add`/
/// GOTOFF materializes AND itself a RelCodeReloc entry position (so a plain
/// string/constant `lea` never truncates a real table) — is this table's exact
/// end.  Returns the run capped to the distance to that anchor.
uint32_t boundRelRunByNextAnchor(const BinaryImage &Img, va_t BaseAddr,
                                 uint16_t EntrySize, uint32_t Run) {
  if (EntrySize == 0 || Run == 0)
    return Run;
  va_t NextAnchor = 0;
  for (auto It = Img.RelCodeTableAnchors.upper_bound(BaseAddr);
       It != Img.RelCodeTableAnchors.end(); ++It)
    if (Img.RelCodeRelocSlots.count(*It)) {
      NextAnchor = *It;
      break;
    }
  if (NextAnchor <= BaseAddr)
    return Run;
  uint32_t Cap = static_cast<uint32_t>((NextAnchor - BaseAddr) / EntrySize);
  return std::min(Run, Cap);
}

//===----------------------------------------------------------------------===//
// evalLinearMultiple — read the integer multiplier of a single base value
//===----------------------------------------------------------------------===//

/// Fold `V` (or its COPY/ZEXT/SEXT chain) to a constant value, if any.  Used to
/// read a modulus that was materialised in a register (`mov wN,#11; msub`) as
/// the back-multiply constant, not just an immediate operand.
static std::optional<int64_t> constValueOf(const std::vector<LowOp> &Ops,
                                           int FromIdx, NdVar V,
                                           int Depth = 0) {
  if (V.isConst())
    return static_cast<int64_t>(V.Offset);
  if ((!V.isReg() && !V.isTemp()) || Depth > limits::kMaxQuasiCopyDepth)
    return std::nullopt;
  int D = reachingDefIdx(Ops, FromIdx, V);
  if (D < 0)
    return std::nullopt;
  const LowOp &Op = Ops[D];
  if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
       Op.Opcode == NdOp::INT_SEXT) &&
      Op.NumInputs >= 1)
    return constValueOf(Ops, D - 1, Op.Inputs[0], Depth + 1);
  return std::nullopt;
}

/// Decompose `V` into `base * Coef`, where Coef is built from the shift /
/// small-constant-multiply / add / subtract terms of one conceptual base.
/// Recovers the modulus N out of the `quotient * N` back-multiply clang emits
/// for `x % N` (rendered as shift/add/sub trees, e.g. q*7=(q<<3)-q,
/// q*9=(q<<3)+q, q*10=(q<<3)+(q<<1), or a direct `q*N` where N may live in a
/// register).  Any op that is not a multiplier-tree node (the magic
/// `(x*recip)>>s` quotient, a load, a param) terminates a branch as the base
/// with coefficient 1; the caller gates on a multiply being present and on the
/// recovered N matching the table's real entry count so this leniency cannot
/// misread an ordinary table.
static bool evalLinearMultiple(const std::vector<LowOp> &Ops, int FromIdx,
                               NdVar V, int Depth, int64_t &Coef) {
  if (Depth > limits::kMaxModuloDecompDepth)
    return false;
  if (!V.isReg() && !V.isTemp())
    return false;
  int D = reachingDefIdx(Ops, FromIdx, V);
  if (D < 0) {
    Coef = 1; // No definition in the slice: the base itself.
    return true;
  }
  const LowOp &Op = Ops[D];
  auto isVar = [](const NdVar &X) { return X.isReg() || X.isTemp(); };
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    if (Op.NumInputs >= 1 && isVar(Op.Inputs[0]))
      return evalLinearMultiple(Ops, D - 1, Op.Inputs[0], Depth + 1, Coef);
    Coef = 1; // COPY of a constant: a materialised base.
    return true;
  case NdOp::SUBBYTES:
    if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
        Op.Inputs[1].Offset == 0 && isVar(Op.Inputs[0]))
      return evalLinearMultiple(Ops, D - 1, Op.Inputs[0], Depth + 1, Coef);
    return false;
  case NdOp::INT_LEFT:
    if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
        Op.Inputs[1].Offset < 32 && isVar(Op.Inputs[0])) {
      int64_t C;
      if (!evalLinearMultiple(Ops, D - 1, Op.Inputs[0], Depth + 1, C))
        return false;
      Coef = C << Op.Inputs[1].Offset;
      return true;
    }
    return false;
  case NdOp::INT_MULT: {
    // base * const, where the const may be an immediate or a register/temp that
    // resolves to one (e.g. `msub` with the modulus in a register).
    for (int CK = 0; CK < Op.NumInputs && CK < 2; ++CK) {
      int BK = 1 - CK;
      if (BK >= Op.NumInputs || !isVar(Op.Inputs[BK]))
        continue;
      std::optional<int64_t> C = constValueOf(Ops, D - 1, Op.Inputs[CK]);
      if (!C)
        continue;
      int64_t Bc;
      if (!evalLinearMultiple(Ops, D - 1, Op.Inputs[BK], Depth + 1, Bc))
        return false;
      Coef = Bc * (*C);
      return true;
    }
    Coef = 1; // q*recip (the magic quotient itself): base.
    return true;
  }
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
    if (Op.NumInputs >= 2 && isVar(Op.Inputs[0]) && isVar(Op.Inputs[1])) {
      int64_t A, B;
      if (!evalLinearMultiple(Ops, D - 1, Op.Inputs[0], Depth + 1, A) ||
          !evalLinearMultiple(Ops, D - 1, Op.Inputs[1], Depth + 1, B))
        return false;
      Coef = (Op.Opcode == NdOp::INT_ADD) ? (A + B) : (A - B);
      return true;
    }
    return false;
  default:
    Coef = 1; // Quotient produced by a non-multiplier op (shift/divide): base.
    return true;
  }
}

//===----------------------------------------------------------------------===//
// inferBoundsFromModulo — bound a `switch(x % N)` table from its remainder
//===----------------------------------------------------------------------===//

/// A power-of-two modulo switch (`switch(x % 2^k)` / `switch(x & (2^k-1))`)
/// lowers the index to `and $(2^k-1)` with no `cmp` range guard.  When such a
/// table sits adjacent to another in rodata the two form one continuous
/// relocation run, so the run-length count over-reads the first table into the
/// second — fabricating bogus successor edges (and, with x87 residents, an
/// unbalanced stack the TOP propagation cannot reconcile).  The mask is a hard
/// upper bound on the index: it confines it to [0, M].  A following `-c` (clang
/// emits `dec` when a peeled iteration proved the low cases dead) lowers the
/// top index to M-c, so the table holds at most (M + Offset) + 1 entries.
/// Returns that bound, or 0 when the index does not reduce to a clean low-bit
/// mask.
uint32_t CFGBuilder::inferBoundsFromMask(const InsnRecord &Rec,
                                         const JumpTableInfo &Info,
                                         bool AllowNonContiguous) const {
  if (Info.IndexReg == InvalidVA)
    return 0;

  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);

  // The index register is frequently reused to hold the loaded entry after the
  // table LOAD, so trace from just before the last load.
  int LastLoad = -1;
  for (int I = 0; I < static_cast<int>(Ops.size()); ++I)
    if (Ops[I].Opcode == NdOp::LOAD)
      LastLoad = I;
  if (LastLoad < 0)
    return 0;

  // Sign-extend an operand-width constant so a 32-bit `dec` (add 0xFFFFFFFF)
  // reads back as -1 rather than a 4-billion offset.
  auto signedConst = [](const NdVar &C) -> int64_t {
    uint64_t U = C.Offset;
    int Sz = C.Size;
    if (Sz > 0 && Sz < 8) {
      uint64_t Mask = (1ULL << (Sz * 8)) - 1;
      U &= Mask;
      if (U & (1ULL << (Sz * 8 - 1)))
        return static_cast<int64_t>(U | ~Mask);
    }
    return static_cast<int64_t>(U);
  };

  const TargetRegInfo &TRImask =
      getTargetRegInfo(CurrentImg ? CurrentImg->Arch : Arch::Unknown);

  NdVar V = NdVar::reg(Info.IndexReg, 8);
  int From = LastLoad - 1;
  int64_t Offset = 0;
  for (int Step = 0; Step < limits::kMaxQuasiCopyDepth; ++Step) {
    int D = reachingDefIdx(Ops, From, V);
    if (D < 0)
      return 0;
    const LowOp &Op = Ops[D];

    if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
         Op.Opcode == NdOp::INT_SEXT) &&
        Op.NumInputs >= 1 && (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
      V = Op.Inputs[0];
      From = D - 1;
      continue;
    }
    // Trace through an -O0 stack spill/reload: the masked index is written into
    // a frame slot (`and idx,7; str idx,[sp,#k]`) and reloaded into a fresh
    // register right before the dispatch (`ldr idx,[sp,#k]; ldr
    // t,[tab,idx,4]`). Hop from the reload LOAD to the value the matching STORE
    // saved, so the trace reaches the mask that would otherwise be hidden
    // behind the slot.
    if (Op.Opcode == NdOp::LOAD && Op.NumInputs >= 1) {
      const NdVar &LAddr = (Op.NumInputs >= 2) ? Op.Inputs[1] : Op.Inputs[0];
      uint64_t SlotReg = InvalidVA;
      int64_t SlotOff = 0;
      if (frameSlotKey(Ops, D - 1, LAddr, TRImask, SlotReg, SlotOff)) {
        int StoreIdx = -1;
        for (int I = D - 1; I >= 0; --I) {
          const LowOp &S = Ops[I];
          if (S.Opcode != NdOp::STORE || S.NumInputs < 2)
            continue;
          uint64_t SB = InvalidVA;
          int64_t SO = 0;
          if (frameSlotKey(Ops, I - 1, S.Inputs[0], TRImask, SB, SO) &&
              SB == SlotReg && SO == SlotOff) {
            StoreIdx = I;
            break;
          }
        }
        if (StoreIdx >= 0 && (Ops[StoreIdx].Inputs[1].isReg() ||
                              Ops[StoreIdx].Inputs[1].isTemp())) {
          V = Ops[StoreIdx].Inputs[1];
          From = StoreIdx - 1;
          continue;
        }
      }
      return 0;
    }
    if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
        Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0 &&
        (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
      V = Op.Inputs[0];
      From = D - 1;
      continue;
    }
    // index = masked +/- constant: accumulate the offset, keep tracing.
    if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
        Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
        (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
      int64_t C = signedConst(Op.Inputs[1]);
      Offset += (Op.Opcode == NdOp::INT_ADD) ? C : -C;
      if (Offset < -64 || Offset > 64)
        return 0;
      V = Op.Inputs[0];
      From = D - 1;
      continue;
    }
    // The binding mask: a contiguous low-bit mask (2^k - 1) caps the index.  It
    // may be EITHER operand (AND is commutative) and the size optimizer may
    // materialise it in a register hoisted to the loop preheader (ARM32 -Oz
    // `mov rM,#7; ... and idx,rM,x`), so resolve each operand to a constant —
    // directly or through a COPY chain visible in the function-wide Ops.
    if (Op.Opcode == NdOp::INT_AND && Op.NumInputs >= 2) {
      auto constOf = [&](NdVar In) -> std::optional<uint64_t> {
        for (int G = 0, F = D - 1; G < limits::kMaxQuasiCopyDepth; ++G) {
          if (In.isConst())
            return In.Offset;
          if (!In.isReg() && !In.isTemp())
            return std::nullopt;
          int DD = reachingDefIdx(Ops, F, In);
          if (DD < 0 || Ops[DD].Opcode != NdOp::COPY || Ops[DD].NumInputs < 1)
            return std::nullopt;
          In = Ops[DD].Inputs[0];
          F = DD - 1;
        }
        return std::nullopt;
      };
      for (int W = 0; W < 2; ++W) {
        auto MOpt = constOf(Op.Inputs[W]);
        if (!MOpt)
          continue;
        uint64_t M = *MOpt;
        if (M == 0)
          continue;
        // A contiguous low-bit mask (2^k - 1) bounds the index to [0, M]
        // exactly.  A non-contiguous mask (e.g. 0x1e) still bounds the raw
        // masked value, but its maximum is the mask's covering value — every
        // bit below the top set bit filled — since any subset of the mask's
        // bits can be simultaneously set.  The table is then dense over that
        // raw index with default filler in the unused (gap) slots, so the
        // covering value + 1 is the physical entry count.  Fold the covering
        // mask so a `switch(x & M)` with an arbitrary M is still bounded.
        if ((M & (M + 1)) != 0) {
          if (!AllowNonContiguous)
            continue;
          uint64_t Cover = M;
          Cover |= Cover >> 1;
          Cover |= Cover >> 2;
          Cover |= Cover >> 4;
          Cover |= Cover >> 8;
          Cover |= Cover >> 16;
          Cover |= Cover >> 32;
          M = Cover;
        }
        int64_t Hi = static_cast<int64_t>(M) + Offset;
        if (Hi < static_cast<int64_t>(limits::kMinJumpTableEntries) - 1)
          continue;
        uint64_t Bound = static_cast<uint64_t>(Hi) + 1;
        if (Bound <= limits::kMaxJumpTableEntries)
          return static_cast<uint32_t>(Bound);
      }
      return 0;
    }
    return 0;
  }
  return 0;
}

/// A modulo switch (`switch(x % N)`, N not a power of two) carries no `cmp`
/// range guard — the remainder is already in [0, N) — so the entry count must
/// come from the modulus N itself.  clang computes `x % N` as
/// `idx = x - (x / N) * N` with a magic-reciprocal division for the quotient
/// and a shift/add/sub tree for the `* N` back-multiply.  This recovers N by
/// decomposing that back-multiply, which bounds tables that carry no entry
/// relocations (AArch64 byte/halfword compact tables, ARM32 inline `.text` word
/// tables) and so cannot use the relocation-run count (#403).  Returns true and
/// sets Info.MaxEntries when a magic-division remainder yields a sane modulus.
bool CFGBuilder::inferBoundsFromModulo(const BinaryImage &Img,
                                       const InsnRecord &Rec,
                                       JumpTableInfo &Info) {
  if (Info.BaseAddr == 0 || Info.EntrySize == 0 || Info.IndexReg == InvalidVA)
    return false;

  // Flatten from the function entry through the dispatch so both the
  // remainder computation (in the loop body) and the modulus constant (often
  // materialised once in the prologue, e.g. AArch64 `mov w11,#N` before an
  // `msub`) are visible to the backward trace.  Reaching-definition scans pick
  // the nearest def, so the wider range only supplies the otherwise-missing
  // prologue constant.
  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);

  // Gate: a magic-reciprocal division always leaves a multiply in the block
  // (the `x * recip`).  The modulus may live in a register, so the presence of
  // a multiply is the structural gate; the recovered N is then confirmed below
  // against the table's real entry count.
  bool SawMul = false;
  int LastLoad = -1;
  for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
    if (Ops[I].Opcode == NdOp::INT_MULT)
      SawMul = true;
    if (Ops[I].Opcode == NdOp::LOAD)
      LastLoad = I;
  }
  if (!SawMul || LastLoad < 0)
    return false;

  // Follow the index register back from just before the table LOAD (the index
  // register is frequently reused to hold the loaded entry afterwards) through
  // value-preserving reshapes to the `idx = dividend - backMul` remainder
  // subtraction that computes `x % N`.
  NdVar V = NdVar::reg(Info.IndexReg, 8);
  int From = LastLoad - 1;
  for (int Step = 0; Step < limits::kMaxQuasiCopyDepth; ++Step) {
    int D = reachingDefIdx(Ops, From, V);
    if (D < 0)
      return false;
    const LowOp &Op = Ops[D];
    if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
         Op.Opcode == NdOp::INT_SEXT) &&
        Op.NumInputs >= 1 && (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
      V = Op.Inputs[0];
      From = D - 1;
      continue;
    }
    if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
        Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0 &&
        (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
      V = Op.Inputs[0];
      From = D - 1;
      continue;
    }
    if ((Op.Opcode != NdOp::INT_SUB && Op.Opcode != NdOp::INT_ADD) ||
        Op.NumInputs < 2)
      return false;

    // The remainder is `dividend - quotient*N` (INT_SUB) or, when the modulus
    // folds into the multiply, `dividend + quotient*(-N)` (INT_ADD, e.g. an
    // AArch64 `msub`).  One operand is the dividend (coefficient ~1); the other
    // is the back-multiply whose |coefficient| is the modulus N.  Decompose
    // each and take the term whose magnitude is a sane entry count.
    for (int Which : {1, 0}) {
      const NdVar &Cand = Op.Inputs[Which];
      if (!Cand.isReg() && !Cand.isTemp())
        continue;
      int64_t Coef = 0;
      if (!evalLinearMultiple(Ops, D - 1, Cand, 0, Coef))
        continue;
      int64_t N = Coef < 0 ? -Coef : Coef;
      if (N < static_cast<int64_t>(limits::kMinJumpTableEntries) ||
          N > static_cast<int64_t>(limits::kMaxJumpTableEntries))
        continue;
      // Confirm N against the table: a real `x % N` dispatch has at least N
      // valid consecutive entries (the in-range cases), which rejects a
      // coincidental linear index that is not actually a modulo remainder.
      JumpTableInfo Probe = Info;
      Probe.MaxEntries = 0;
      if (static_cast<int64_t>(readTableEntries(Img, Probe).size()) < N)
        continue;
      Info.MaxEntries = static_cast<uint32_t>(N);
      Info.RelocBounded = true;
      Info.NormBase = 0;
      Info.NormShift = 0;
      Info.Stride = 1;
      return true;
    }
    return false;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// pullBackBound — adjust a guard bound through normalization operations
//===----------------------------------------------------------------------===//

uint32_t CFGBuilder::pullBackBound(uint32_t RawBound,
                                   const JumpTableInfo &Info) const {
  uint32_t Adjusted = RawBound;

  if (Info.NormBase > 0 && Adjusted > static_cast<uint32_t>(Info.NormBase))
    Adjusted -= static_cast<uint32_t>(Info.NormBase);

  if (Info.NormShift > 0)
    Adjusted >>= Info.NormShift;

  if (Adjusted == 0)
    return RawBound;
  return Adjusted;
}

} // namespace neverd
