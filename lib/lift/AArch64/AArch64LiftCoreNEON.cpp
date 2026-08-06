//===- AArch64LiftCoreNEON.cpp - AArch64 NEON bulk dataflow lifter --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// NEON (Advanced SIMD) bulk dataflow instruction handlers for AArch64.
/// Covers vector moves, arithmetic, logic, shifts, compares, min/max,
/// extend, narrow, table lookups, zip/unzip, transpose, and structured
/// load/store (LD1..LD4, ST1..ST4, STP).
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-aarch64"

namespace neverd {

// Effective immediate for a MOVI/MVNI/ORR/BIC vector-immediate operand,
// applying the LSL (zero-fill) or MSL (one-fill, "modified shift left") shift.
// capstone reports the raw immediate and the shift separately; ignoring the
// shift produced wrong constants (e.g. `movi v.4s,#0x1f,msl #8` should be
// 0x1FFF = (0x1f<<8)|0xFF, not 0x1f).
static uint64_t moviImmWithShift(const cs_aarch64_op &Op) {
  uint64_t Imm = static_cast<uint64_t>(Op.imm);
  if (Op.shift.value > 0) {
    unsigned N = Op.shift.value;
    if (Op.shift.type == AARCH64_SFT_LSL)
      Imm <<= N;
    else if (Op.shift.type == AARCH64_SFT_MSL)
      Imm = (Imm << N) | ((1ULL << N) - 1);
  }
  return Imm;
}

// Element size (in bytes) for an AArch64 vector arrangement specifier.
// Returns 0 if the layout is not a recognized packed vector arrangement.
static unsigned neonElemSize(AArch64Layout_VectorLayout VAS) {
  switch (VAS) {
  // Bare element layouts (no lane-count prefix): used by the single-element
  // indexed load/store forms `ld1/st1 {v.<T>}[idx]` and scalar forms.  capstone
  // reports e.g. `ld1 {v0.s}[1]` with vas = AARCH64LAYOUT_VL_S (not VL_4S), so
  // these must be recognized or the indexed branch falls back to a full-vector
  // load that clobbers the other lanes.
  case AARCH64LAYOUT_VL_B:
    return 1;
  case AARCH64LAYOUT_VL_H:
    return 2;
  case AARCH64LAYOUT_VL_S:
    return 4;
  case AARCH64LAYOUT_VL_D:
    return 8;
  case AARCH64LAYOUT_VL_Q:
    return 16;
  case AARCH64LAYOUT_VL_16B:
  case AARCH64LAYOUT_VL_8B:
    return 1;
  case AARCH64LAYOUT_VL_8H:
  case AARCH64LAYOUT_VL_4H:
    return 2;
  case AARCH64LAYOUT_VL_4S:
  case AARCH64LAYOUT_VL_2S:
    return 4;
  case AARCH64LAYOUT_VL_2D:
  case AARCH64LAYOUT_VL_1D:
    return 8;
  case AARCH64LAYOUT_VL_1Q:
    return 16;
  default:
    return 0;
  }
}

bool AArch64Lifter::liftCoreNEON(LiftState &S, const cs_insn *Insn,
                                 const cs_aarch64 &ARM64) {
  switch (Insn->id) {

  // ====================================================================
  // NEON / SIMD — bulk dataflow, same approach as x86 SSE/AVX
  // (preserves shape for decompilation; per-lane semantics would need
  // dedicated VEC_* ops — see intrinsics for per-lane NEON ops)
  // ====================================================================
  case AARCH64_INS_MOVI: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    if (ARM64.operands[1].type == AARCH64_OP_IMM) {
      uint64_t Imm = moviImmWithShift(ARM64.operands[1]);
      Src = NdVar::cst(Imm, (Imm <= 0xFFFFFFFFULL) ? 4 : 8);
    }
    uint16_t ElemSz = 0;
    auto VAS = ARM64.operands[0].vas;
    if (VAS == AARCH64LAYOUT_VL_16B || VAS == AARCH64LAYOUT_VL_8B)
      ElemSz = 1;
    else if (VAS == AARCH64LAYOUT_VL_8H || VAS == AARCH64LAYOUT_VL_4H)
      ElemSz = 2;
    else if (VAS == AARCH64LAYOUT_VL_4S || VAS == AARCH64LAYOUT_VL_2S)
      ElemSz = 4;
    else if (VAS == AARCH64LAYOUT_VL_2D)
      ElemSz = 8;
    if (ElemSz > 0 && Dst.Size > ElemSz) {
      unsigned NLanes = Dst.Size / ElemSz;
      NdVar Elem = Src;
      if (Elem.Size > ElemSz) {
        NdVar Trunc = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, Trunc, {Elem, NdVar::cst(0, 4)});
        Elem = Trunc;
      } else if (Elem.Size < ElemSz) {
        NdVar Ext = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_ZEXT, Ext, {Elem});
        Elem = Ext;
      }
      NdVar Acc = Elem;
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Next = S.makeTemp(Acc.Size + ElemSz);
        S.emit(NdOp::CONCAT, Next, {Elem, Acc});
        Acc = Next;
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_MVNI: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    if (ARM64.operands[1].type == AARCH64_OP_IMM) {
      uint64_t Imm = moviImmWithShift(ARM64.operands[1]);
      Src = NdVar::cst(Imm, (Imm <= 0xFFFFFFFFULL) ? 4 : 8);
    }
    uint16_t ElemSz = 0;
    auto VAS = ARM64.operands[0].vas;
    if (VAS == AARCH64LAYOUT_VL_16B || VAS == AARCH64LAYOUT_VL_8B)
      ElemSz = 1;
    else if (VAS == AARCH64LAYOUT_VL_8H || VAS == AARCH64LAYOUT_VL_4H)
      ElemSz = 2;
    else if (VAS == AARCH64LAYOUT_VL_4S || VAS == AARCH64LAYOUT_VL_2S)
      ElemSz = 4;
    else if (VAS == AARCH64LAYOUT_VL_2D)
      ElemSz = 8;
    if (ElemSz > 0 && Dst.Size > ElemSz) {
      unsigned NLanes = Dst.Size / ElemSz;
      NdVar Elem = Src;
      if (Elem.Size > ElemSz) {
        NdVar Trunc = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, Trunc, {Elem, NdVar::cst(0, 4)});
        Elem = Trunc;
      } else if (Elem.Size < ElemSz) {
        NdVar Ext = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_ZEXT, Ext, {Elem});
        Elem = Ext;
      }
      NdVar Acc = Elem;
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Next = S.makeTemp(Acc.Size + ElemSz);
        S.emit(NdOp::CONCAT, Next, {Elem, Acc});
        Acc = Next;
      }
      NdVar Full = S.makeTemp(Dst.Size);
      S.emit(NdOp::COPY, Full, {Acc});
      S.emit(NdOp::INT_NOT, Dst, {Full});
    } else {
      S.emit(NdOp::INT_NOT, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_DUP:
  case AARCH64_INS_INS: {
    if (ARM64.op_count < 2)
      break;
    const auto &DstOp = ARM64.operands[0];
    NdVar Src = operandRead(S, ARM64.operands[1]);

    if (Insn->id == AARCH64_INS_INS && DstOp.vector_index >= 0 &&
        DstOp.type == AARCH64_OP_REG) {
      unsigned ElemSz = 0;
      switch (DstOp.vas) {
      case AARCH64LAYOUT_VL_B:
        ElemSz = 1;
        break;
      case AARCH64LAYOUT_VL_H:
        ElemSz = 2;
        break;
      case AARCH64LAYOUT_VL_S:
        ElemSz = 4;
        break;
      case AARCH64LAYOUT_VL_D:
        ElemSz = 8;
        break;
      default:
        break;
      }
      auto RI = mapCapstoneReg(static_cast<aarch64_reg>(DstOp.reg));
      if (ElemSz > 0 && RI.Size > ElemSz) {
        unsigned ByteOff = DstOp.vector_index * ElemSz;
        // vector_index is only checked as >= 0; a malformed encoding can report
        // an index whose lane spills past the register, which would underflow
        // the RI.Size - (ByteOff + ElemSz) high-slice size below into a huge
        // makeTemp.  Skip lifting the (invalid) lane insert instead.
        if (ByteOff + ElemSz > RI.Size)
          break;
        NdVar QReg = NdVar::reg(RI.Offset, RI.Size);
        NdVar Trunc = Src;
        if (Src.Size > ElemSz) {
          Trunc = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, Trunc, {Src, NdVar::cst(0, 4)});
        }
        // Build new Q value: [hi | new_elem | lo]
        if (ByteOff == 0) {
          NdVar Hi = S.makeTemp(RI.Size - ElemSz);
          S.emit(NdOp::SUBBYTES, Hi, {QReg, NdVar::cst(ElemSz, 4)});
          NdVar New = S.makeTemp(RI.Size);
          S.emit(NdOp::CONCAT, New, {Hi, Trunc});
          S.emit(NdOp::COPY, QReg, {New});
        } else if (ByteOff + ElemSz == RI.Size) {
          NdVar Lo = S.makeTemp(ByteOff);
          S.emit(NdOp::SUBBYTES, Lo, {QReg, NdVar::cst(0, 4)});
          NdVar New = S.makeTemp(RI.Size);
          S.emit(NdOp::CONCAT, New, {Trunc, Lo});
          S.emit(NdOp::COPY, QReg, {New});
        } else {
          NdVar Lo = S.makeTemp(ByteOff);
          S.emit(NdOp::SUBBYTES, Lo, {QReg, NdVar::cst(0, 4)});
          unsigned HiOff = ByteOff + ElemSz;
          unsigned HiSz = RI.Size - HiOff;
          NdVar Hi = S.makeTemp(HiSz);
          S.emit(NdOp::SUBBYTES, Hi, {QReg, NdVar::cst(HiOff, 4)});
          NdVar MidLo = S.makeTemp(ByteOff + ElemSz);
          S.emit(NdOp::CONCAT, MidLo, {Trunc, Lo});
          NdVar New = S.makeTemp(RI.Size);
          S.emit(NdOp::CONCAT, New, {Hi, MidLo});
          S.emit(NdOp::COPY, QReg, {New});
        }
        break;
      }
    }
    NdVar Dst = operandWrite(DstOp);
    if (Insn->id == AARCH64_INS_DUP && DstOp.vas != AARCH64LAYOUT_INVALID) {
      uint16_t ElemSz = 0;
      auto VAS = DstOp.vas;
      if (VAS == AARCH64LAYOUT_VL_16B || VAS == AARCH64LAYOUT_VL_8B)
        ElemSz = 1;
      else if (VAS == AARCH64LAYOUT_VL_8H || VAS == AARCH64LAYOUT_VL_4H)
        ElemSz = 2;
      else if (VAS == AARCH64LAYOUT_VL_4S || VAS == AARCH64LAYOUT_VL_2S)
        ElemSz = 4;
      else if (VAS == AARCH64LAYOUT_VL_2D)
        ElemSz = 8;
      if (ElemSz > 0 && Dst.Size > ElemSz) {
        unsigned NLanes = Dst.Size / ElemSz;
        NdVar Elem = Src;
        if (Elem.Size > ElemSz) {
          NdVar Trunc = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, Trunc, {Elem, NdVar::cst(0, 4)});
          Elem = Trunc;
        } else if (Elem.Size < ElemSz) {
          NdVar Ext = S.makeTemp(ElemSz);
          S.emit(NdOp::INT_ZEXT, Ext, {Elem});
          Elem = Ext;
        }
        NdVar Acc = Elem;
        for (unsigned I = 1; I < NLanes; ++I) {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {Elem, Acc});
          Acc = Next;
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        S.emit(NdOp::COPY, Dst, {Src});
      }
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_UMOV:
  case AARCH64_INS_SMOV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    if (Insn->id == AARCH64_INS_SMOV)
      S.emit(NdOp::INT_SEXT, Dst, {Src});
    else
      S.emit(NdOp::INT_ZEXT, Dst, {Src});
    break;
  }

  // NEON vector arithmetic — preserve per-lane semantics via intrinsics.
  case AARCH64_INS_ADDP: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);

    // Scalar form: ADDP <V><d>, <Vn>.<T> (2 operands) — sum all lanes of the
    // single source vector pairwise into a scalar.  The only architectural
    // form is ADDP D<d>, V<n>.2D (two 64-bit lanes), but derive the lane size
    // from the source layout to be safe.
    if (ARM64.op_count == 2) {
      unsigned SrcLaneSz = 8;
      switch (ARM64.operands[1].vas) {
      case AARCH64LAYOUT_VL_2D:
        SrcLaneSz = 8;
        break;
      case AARCH64LAYOUT_VL_4S:
      case AARCH64LAYOUT_VL_2S:
        SrcLaneSz = 4;
        break;
      case AARCH64LAYOUT_VL_8H:
      case AARCH64LAYOUT_VL_4H:
        SrcLaneSz = 2;
        break;
      default:
        SrcLaneSz = 8;
        break;
      }
      NdVar Lo = S.makeTemp(SrcLaneSz);
      NdVar Hi = S.makeTemp(SrcLaneSz);
      S.emit(NdOp::SUBBYTES, Lo, {A, NdVar::cst(0, 4)});
      S.emit(NdOp::SUBBYTES, Hi, {A, NdVar::cst(SrcLaneSz, 4)});
      S.emit(NdOp::INT_ADD, Dst, {Lo, Hi});
      break;
    }

    NdVar B = operandRead(S, ARM64.operands[2]);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;

    if (LaneSz > 0 && A.Size > LaneSz) {
      unsigned NPairs = A.Size / (LaneSz * 2);
      NdVar Acc = S.makeTemp(0);
      for (unsigned H = 0; H < 2; ++H) {
        NdVar Src = (H == 0) ? A : B;
        for (unsigned I = 0; I < NPairs; ++I) {
          NdVar Lo = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(I * 2 * LaneSz, 4)});
          NdVar Hi = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Hi,
                 {Src, NdVar::cst(I * 2 * LaneSz + LaneSz, 4)});
          NdVar Sum = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_ADD, Sum, {Lo, Hi});
          unsigned Idx = H * NPairs + I;
          if (Idx == 0) {
            Acc = Sum;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Sum, Acc});
            Acc = Next;
          }
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emitIntrinsic(Intrinsic::A64Addp, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_SMAX:
  case AARCH64_INS_SMIN:
  case AARCH64_INS_UMAX:
  case AARCH64_INS_UMIN: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;

    bool IsSigned =
        (Insn->id == AARCH64_INS_SMAX || Insn->id == AARCH64_INS_SMIN);
    bool IsMax = (Insn->id == AARCH64_INS_SMAX || Insn->id == AARCH64_INS_UMAX);

    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Cmp = S.makeTemp(1);
        NdOp CmpOp = IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
        if (IsMax)
          S.emit(CmpOp, Cmp, {Lb, La});
        else
          S.emit(CmpOp, Cmp, {La, Lb});
        NdVar Sel = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Sel, {Cmp, La, Lb});
        if (I == 0) {
          Acc = Sel;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Sel, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdOp CmpOp = IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
      NdVar Cmp = S.makeTemp(1);
      if (IsMax)
        S.emit(CmpOp, Cmp, {B, A});
      else
        S.emit(CmpOp, Cmp, {A, B});
      S.emit(NdOp::SELECT, Dst, {Cmp, A, B});
    }
    break;
  }
  case AARCH64_INS_SABD:
  case AARCH64_INS_UABD: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    bool IsSigned = (Insn->id == AARCH64_INS_SABD);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;

    if (LaneSz > 0 && A.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Diff = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_SUB, Diff, {La, Lb});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS, IsNeg, {La, Lb});
        NdVar NegDiff = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, NegDiff, {Diff});
        NdVar AbsDiff = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, AbsDiff, {IsNeg, NegDiff, Diff});
        if (I == 0) {
          Acc = AbsDiff;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {AbsDiff, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emitIntrinsic(IsSigned ? Intrinsic::A64Sabd : Intrinsic::A64Uabd, Dst,
                      {A, B});
    }
    break;
  }
  // Halving add (optionally rounding): dst[i] = (a[i]+b[i](+1)) >> 1, per lane.
  // Previously these used unimplemented intrinsics (result became 0).
  case AARCH64_INS_SHADD:
  case AARCH64_INS_SRHADD:
  case AARCH64_INS_UHADD:
  case AARCH64_INS_URHADD: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SHADD || Insn->id == AARCH64_INS_SRHADD);
    bool IsRound =
        (Insn->id == AARCH64_INS_SRHADD || Insn->id == AARCH64_INS_URHADD);
    unsigned LaneSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      LaneSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      LaneSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      LaneSz = 4;
      break;
    default:
      break;
    }
    if (LaneSz > 0 && Dst.Size >= LaneSz) {
      unsigned WideSz = LaneSz * 2;
      unsigned NLanes = Dst.Size / LaneSz;
      NdOp ExtOp = IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdOp ShOp = IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Al = S.makeTemp(LaneSz), Bl = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Al, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Bl, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Aw = S.makeTemp(WideSz), Bw = S.makeTemp(WideSz);
        S.emit(ExtOp, Aw, {Al});
        S.emit(ExtOp, Bw, {Bl});
        NdVar Sum = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Sum, {Aw, Bw});
        if (IsRound) {
          NdVar Sum1 = S.makeTemp(WideSz);
          S.emit(NdOp::INT_ADD, Sum1, {Sum, NdVar::cst(1, WideSz)});
          Sum = Sum1;
        }
        NdVar Sh = S.makeTemp(WideSz);
        S.emit(ShOp, Sh, {Sum, NdVar::cst(1, WideSz)});
        NdVar Res = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Res, {Sh, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Res;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Res, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }
  // Halving subtract (no rounding form): dst[i] = (a[i]-b[i]) >> 1, per lane.
  // Was grouped with SVE2 widening subtracts as a full-width INT_SUB
  // placeholder (no halving, cross-lane borrow).  SHSUBR/UHSUBR (SVE2 reversed)
  // stay there.
  case AARCH64_INS_SHSUB:
  case AARCH64_INS_UHSUB: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    bool IsSigned = (Insn->id == AARCH64_INS_SHSUB);
    unsigned LaneSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      LaneSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      LaneSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      LaneSz = 4;
      break;
    default:
      break;
    }
    if (LaneSz > 0 && Dst.Size >= LaneSz) {
      unsigned WideSz = LaneSz * 2;
      unsigned NLanes = Dst.Size / LaneSz;
      NdOp ExtOp = IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      NdOp ShOp = IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Al = S.makeTemp(LaneSz), Bl = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Al, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Bl, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Aw = S.makeTemp(WideSz), Bw = S.makeTemp(WideSz);
        S.emit(ExtOp, Aw, {Al});
        S.emit(ExtOp, Bw, {Bl});
        NdVar Diff = S.makeTemp(WideSz);
        S.emit(NdOp::INT_SUB, Diff, {Aw, Bw});
        NdVar Sh = S.makeTemp(WideSz);
        S.emit(ShOp, Sh, {Diff, NdVar::cst(1, WideSz)});
        NdVar Res = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Res, {Sh, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Res;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Res, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_SQADD:
  case AARCH64_INS_UQADD:
  case AARCH64_INS_SQSUB:
  case AARCH64_INS_UQSUB: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;

    bool IsSigned =
        (Insn->id == AARCH64_INS_SQADD || Insn->id == AARCH64_INS_SQSUB);
    bool IsSub =
        (Insn->id == AARCH64_INS_SQSUB || Insn->id == AARCH64_INS_UQSUB);
    Intrinsic SatII = IsSigned
                          ? (IsSub ? Intrinsic::A64Sqsub : Intrinsic::A64Sqadd)
                          : (IsSub ? Intrinsic::A64Uqsub : Intrinsic::A64Uqadd);

    if (LaneSz > 0 && LaneSz <= 4 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      unsigned WideSz = LaneSz * 2;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Wa = S.makeTemp(WideSz);
        NdVar Wb = S.makeTemp(WideSz);
        if (IsSigned) {
          S.emit(NdOp::INT_SEXT, Wa, {La});
          S.emit(NdOp::INT_SEXT, Wb, {Lb});
        } else {
          S.emit(NdOp::INT_ZEXT, Wa, {La});
          S.emit(NdOp::INT_ZEXT, Wb, {Lb});
        }
        NdVar Wide = S.makeTemp(WideSz);
        if (IsSub)
          S.emit(NdOp::INT_SUB, Wide, {Wa, Wb});
        else
          S.emit(NdOp::INT_ADD, Wide, {Wa, Wb});

        NdVar Result = S.makeTemp(LaneSz);
        if (IsSigned) {
          uint64_t MaxVal = (1ULL << (LaneSz * 8 - 1)) - 1;
          uint64_t MinVal = ~MaxVal;
          NdVar WMax = NdVar::cst(MaxVal, WideSz);
          NdVar WMin = NdVar::cst(MinVal, WideSz);
          NdVar IsOver = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, IsOver, {WMax, Wide});
          NdVar Clamped1 = S.makeTemp(WideSz);
          S.emit(NdOp::SELECT, Clamped1, {IsOver, WMax, Wide});
          NdVar IsUnder = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, IsUnder, {Clamped1, WMin});
          NdVar Clamped2 = S.makeTemp(WideSz);
          S.emit(NdOp::SELECT, Clamped2, {IsUnder, WMin, Clamped1});
          S.emit(NdOp::SUBBYTES, Result, {Clamped2, NdVar::cst(0, 4)});
        } else {
          if (IsSub) {
            NdVar IsNeg = S.makeTemp(1);
            NdVar Zero = NdVar::cst(0, WideSz);
            S.emit(NdOp::INT_SLESS, IsNeg, {Wide, Zero});
            NdVar Clamped = S.makeTemp(WideSz);
            S.emit(NdOp::SELECT, Clamped, {IsNeg, Zero, Wide});
            S.emit(NdOp::SUBBYTES, Result, {Clamped, NdVar::cst(0, 4)});
          } else {
            uint64_t UMax = (LaneSz < 8) ? ((1ULL << (LaneSz * 8)) - 1) : ~0ULL;
            NdVar WUMax = NdVar::cst(UMax, WideSz);
            NdVar IsOver = S.makeTemp(1);
            S.emit(NdOp::INT_LESS, IsOver, {WUMax, Wide});
            NdVar Clamped = S.makeTemp(WideSz);
            S.emit(NdOp::SELECT, Clamped, {IsOver, WUMax, Wide});
            S.emit(NdOp::SUBBYTES, Result, {Clamped, NdVar::cst(0, 4)});
          }
        }

        if (I == 0) {
          Acc = Result;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Result, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else if (LaneSz == 8 && Dst.Size > LaneSz) {
      // 64-bit lanes (.2d): per-lane saturating intrinsic (a manual i128 clamp
      // can't represent the signed 64-bit bounds).
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        NdVar Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar R = S.makeTemp(LaneSz);
        S.emitIntrinsic(SatII, R, {La, Lb});
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      // Scalar form (b/h/s/d): single saturating intrinsic.
      S.emitIntrinsic(SatII, Dst, {A, B});
    }
    break;
  }
  // SMIN/UMIN handled above in the combined SMAX/SMIN/UMAX/UMIN case
  // NEON integer multiply-accumulate.  Must be per-lane: a single full-width
  // INT_MULT multiplies the whole 128-bit register (cross-lane carry) and a
  // full-width INT_ADD then carries across lanes.  Supports the indexed form
  // (`mla v.4s, v.4s, v.s[idx]`) where the multiplier is a scalar broadcast.
  case AARCH64_INS_MLA:
  case AARCH64_INS_MLS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar DstR = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSub = (Insn->id == AARCH64_INS_MLS);
    int BLane = ARM64.operands[2].vector_index;
    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      bool AScalar = (A.Size <= LaneSz);
      bool BScalar = (B.Size <= LaneSz);
      // By-element `mla/mls v.T, v.T, vN.<ty>[idx]` broadcasts one source lane.
      if (BLane >= 0 && !BScalar) {
        NdVar BFull = operandWrite(ARM64.operands[2]);
        NdVar BElem = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, BElem,
               {BFull, NdVar::cst(static_cast<uint64_t>(BLane) * LaneSz, 4)});
        B = BElem;
        BScalar = true;
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = AScalar ? A : S.makeTemp(LaneSz);
        if (!AScalar)
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = BScalar ? B : S.makeTemp(LaneSz);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {DstR, NdVar::cst(I * LaneSz, 4)});
        NdVar P = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_MULT, P, {La, Lb});
        NdVar R = S.makeTemp(LaneSz);
        S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, R, {Ld, P});
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {A, B});
      S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Dst, {DstR, Prod});
    }
    break;
  }
  case AARCH64_INS_FMLA: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar DstR = NdVar::reg(Dst.Offset, Dst.Size);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;

    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      bool AScalar = (A.Size <= LaneSz);
      bool BScalar = (B.Size <= LaneSz);
      // By-element `fmla v.T, v.T, vN.<ty>[idx]` broadcasts one source lane.
      if (ARM64.operands[2].vector_index >= 0 && !BScalar) {
        NdVar BFull = operandWrite(ARM64.operands[2]);
        NdVar BElem = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, BElem,
               {BFull, NdVar::cst(static_cast<uint64_t>(
                                        ARM64.operands[2].vector_index) *
                                        LaneSz,
                                    4)});
        B = BElem;
        BScalar = true;
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = AScalar ? A : S.makeTemp(LaneSz);
        if (!AScalar)
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = BScalar ? B : S.makeTemp(LaneSz);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {DstR, NdVar::cst(I * LaneSz, 4)});
        // FMLA is fused (single rounding): Ld + La*Lb = fma(La,Lb,Ld).
        NdVar R = S.makeTemp(LaneSz);
        S.emit(NdOp::FLOAT_FMA, R, {La, Lb, Ld});
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::FLOAT_FMA, Dst, {A, B, DstR});
    }
    break;
  }
  case AARCH64_INS_FMLS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar DstR = NdVar::reg(Dst.Offset, Dst.Size);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;

    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      bool AScalar = (A.Size <= LaneSz);
      bool BScalar = (B.Size <= LaneSz);
      // By-element `fmls v.T, v.T, vN.<ty>[idx]` broadcasts one source lane.
      if (ARM64.operands[2].vector_index >= 0 && !BScalar) {
        NdVar BFull = operandWrite(ARM64.operands[2]);
        NdVar BElem = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, BElem,
               {BFull, NdVar::cst(static_cast<uint64_t>(
                                        ARM64.operands[2].vector_index) *
                                        LaneSz,
                                    4)});
        B = BElem;
        BScalar = true;
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = AScalar ? A : S.makeTemp(LaneSz);
        if (!AScalar)
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        NdVar Lb = BScalar ? B : S.makeTemp(LaneSz);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {DstR, NdVar::cst(I * LaneSz, 4)});
        // FMLS is fused (single rounding): Ld - La*Lb = fma(-La,Lb,Ld).
        NdVar NLa = S.makeTemp(LaneSz);
        S.emit(NdOp::FLOAT_NEG, NLa, {La});
        NdVar R = S.makeTemp(LaneSz);
        S.emit(NdOp::FLOAT_FMA, R, {NLa, Lb, Ld});
        if (I == 0) {
          Acc = R;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {R, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar NA = S.makeTemp(Dst.Size);
      S.emit(NdOp::FLOAT_NEG, NA, {A});
      S.emit(NdOp::FLOAT_FMA, Dst, {NA, B, DstR});
    }
    break;
  }
  // NEON FNMADD / FNMSUB
  case AARCH64_INS_FNMADD: {
    // FNMADD Dd, Dn, Dm, Da: Dd = -(Da + Dn*Dm)
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar C = operandRead(S, ARM64.operands[3]);
    // Fused: -(C + A*B) = -fma(A,B,C).
    NdVar Sum = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_FMA, Sum, {A, B, C});
    S.emit(NdOp::FLOAT_NEG, Dst, {Sum});
    break;
  }
  case AARCH64_INS_FNMSUB: {
    // FNMSUB Dd, Dn, Dm, Da: Dd = Dn*Dm - Da
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar C = operandRead(S, ARM64.operands[3]);
    // Fused: A*B - C = fma(A,B,-C).
    NdVar NC = S.makeTemp(Dst.Size);
    S.emit(NdOp::FLOAT_NEG, NC, {C});
    S.emit(NdOp::FLOAT_FMA, Dst, {A, B, NC});
    break;
  }
  // NEON vector compare — output is all-ones or all-zeros Mask
  case AARCH64_INS_CMEQ:
  case AARCH64_INS_CMGE:
  case AARCH64_INS_CMGT:
  case AARCH64_INS_CMHI:
  case AARCH64_INS_CMHS:
  case AARCH64_INS_CMLE:
  case AARCH64_INS_CMLT:
  case AARCH64_INS_CMTST:
  case AARCH64_INS_FACGE:
  case AARCH64_INS_FACGT:
  case AARCH64_INS_FCMEQ:
  case AARCH64_INS_FCMGE:
  case AARCH64_INS_FCMGT:
  case AARCH64_INS_FCMLE:
  case AARCH64_INS_FCMLT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A, B;
    if (ARM64.op_count >= 3) {
      A = operandRead(S, ARM64.operands[1]);
      B = operandRead(S, ARM64.operands[2]);
    } else {
      A = operandRead(S, ARM64.operands[1]);
      B = NdVar::cst(0, A.Size);
    }
    NdOp CmpOp = NdOp::INT_EQUAL;
    // FP compares (FCM*/FAC*) must use FLOAT_* ops; comparing the float bit
    // patterns as signed integers is only right for non-negative values (the
    // sign bit makes negatives look "large").  FACGE/FACGT compare absolute
    // values.
    bool IsAbs = false;
    switch (Insn->id) {
    case AARCH64_INS_CMEQ:
      CmpOp = NdOp::INT_EQUAL;
      break;
    case AARCH64_INS_FCMEQ:
      CmpOp = NdOp::FLOAT_EQUAL;
      break;
    case AARCH64_INS_CMGE:
      CmpOp = NdOp::INT_SLESSEQUAL;
      std::swap(A, B);
      break;
    case AARCH64_INS_FACGE:
      IsAbs = true;
      [[fallthrough]];
    case AARCH64_INS_FCMGE:
      CmpOp = NdOp::FLOAT_LESSEQUAL;
      std::swap(A, B);
      break;
    case AARCH64_INS_CMGT:
      CmpOp = NdOp::INT_SLESS;
      std::swap(A, B);
      break;
    case AARCH64_INS_FACGT:
      IsAbs = true;
      [[fallthrough]];
    case AARCH64_INS_FCMGT:
      CmpOp = NdOp::FLOAT_LESS;
      std::swap(A, B);
      break;
    case AARCH64_INS_CMHI:
      CmpOp = NdOp::INT_LESS;
      std::swap(A, B);
      break;
    case AARCH64_INS_CMHS:
      CmpOp = NdOp::INT_LESSEQUAL;
      std::swap(A, B);
      break;
    case AARCH64_INS_CMLE:
      CmpOp = NdOp::INT_SLESSEQUAL;
      break;
    case AARCH64_INS_FCMLE:
      CmpOp = NdOp::FLOAT_LESSEQUAL;
      break;
    case AARCH64_INS_CMLT:
      CmpOp = NdOp::INT_SLESS;
      break;
    case AARCH64_INS_FCMLT:
      CmpOp = NdOp::FLOAT_LESS;
      break;
    case AARCH64_INS_CMTST: {
      // Set each lane to all-ones when (a & b) != 0 in that lane.  The bitwise
      // AND is the same full-width or per-lane; rewrite the operands so the
      // shared per-lane mask logic below produces the correct per-lane result
      // (the old code emitted a single full-width 0/1 boolean).
      NdVar Anded = S.makeTemp(A.Size);
      S.emit(NdOp::INT_AND, Anded, {A, B});
      A = Anded;
      B = NdVar::cst(0, Anded.Size);
      CmpOp = NdOp::INT_NOTEQUAL;
      break;
    }
    default:
      break;
    }
    {
      auto Vas = ARM64.operands[0].vas;
      unsigned LaneSz = 0;
      if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
        LaneSz = 4;
      else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
        LaneSz = 2;
      else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
        LaneSz = 1;
      else if (Vas == AARCH64LAYOUT_VL_2D)
        LaneSz = 8;
      if (LaneSz > 0 && Dst.Size > LaneSz) {
        unsigned NLanes = Dst.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          NdVar Lb = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
          S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
          if (IsAbs) {
            NdVar AbsA = S.makeTemp(LaneSz), AbsB = S.makeTemp(LaneSz);
            S.emit(NdOp::FLOAT_ABS, AbsA, {La});
            S.emit(NdOp::FLOAT_ABS, AbsB, {Lb});
            La = AbsA;
            Lb = AbsB;
          }
          NdVar CmpRes = S.makeTemp(1);
          S.emit(CmpOp, CmpRes, {La, Lb});
          uint64_t AllOnes = (LaneSz == 1)   ? 0xFF
                             : (LaneSz == 2) ? 0xFFFF
                             : (LaneSz == 4) ? 0xFFFFFFFFULL
                                             : 0xFFFFFFFFFFFFFFFFULL;
          NdVar Mask = S.makeTemp(LaneSz);
          S.emit(
              NdOp::SELECT, Mask,
              {CmpRes, NdVar::cst(AllOnes, LaneSz), NdVar::cst(0, LaneSz)});
          if (I == 0) {
            Acc = Mask;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Mask, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, Dst, {Acc});
      } else {
        if (IsAbs) {
          NdVar AbsA = S.makeTemp(A.Size), AbsB = S.makeTemp(B.Size);
          S.emit(NdOp::FLOAT_ABS, AbsA, {A});
          S.emit(NdOp::FLOAT_ABS, AbsB, {B});
          A = AbsA;
          B = AbsB;
        }
        S.emit(CmpOp, Dst, {A, B});
      }
    }
    break;
  }
  // NEON vector shift
  case AARCH64_INS_SHL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    NdVar Amt = operandRead(S, ARM64.operands[2]);
    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar LaneAmt = Amt;
      if (Amt.Size > LaneSz) {
        LaneAmt = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, LaneAmt, {Amt, NdVar::cst(0, 4)});
      }
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Ls = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ls, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_LEFT, Lr, {Ls, LaneAmt});
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_LEFT, Dst, {Src, Amt});
    }
    break;
  }
  // Saturating shift left (register form: per-lane signed amount; immediate
  // form: splat).  SQSHLU additionally saturates to the unsigned range.  Map to
  // the AArch64 NEON intrinsic for bit-exact saturation; the old code was a
  // plain full-width INT_LEFT (no saturation, no per-lane, no right-shift).
  case AARCH64_INS_SQSHL:
  case AARCH64_INS_UQSHL:
  case AARCH64_INS_SQSHLU: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    unsigned ElemSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      ElemSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      ElemSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      ElemSz = 4;
      break;
    case AARCH64LAYOUT_VL_2D:
    case AARCH64LAYOUT_VL_1D:
      ElemSz = 8;
      break;
    default:
      break;
    }
    if (ElemSz == 0)
      ElemSz = Dst.Size; // scalar form
    unsigned NLanes = ElemSz ? Dst.Size / ElemSz : 1;
    NdVar ShiftVec;
    if (ARM64.operands[2].type == AARCH64_OP_IMM) {
      NdVar C =
          NdVar::cst(static_cast<uint64_t>(ARM64.operands[2].imm), ElemSz);
      NdVar Acc = C;
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Next = S.makeTemp(Acc.Size + ElemSz);
        S.emit(NdOp::CONCAT, Next, {C, Acc});
        Acc = Next;
      }
      ShiftVec = Acc;
    } else {
      ShiftVec = operandRead(S, ARM64.operands[2]);
    }
    Intrinsic II = (Insn->id == AARCH64_INS_SQSHL)   ? Intrinsic::A64_Sqshl
                   : (Insn->id == AARCH64_INS_UQSHL) ? Intrinsic::A64_Uqshl
                                                     : Intrinsic::A64_Sqshlu;
    S.emitIntrinsic(II, Dst, {Src, ShiftVec, NdVar::cst(ElemSz, 4)});
    break;
  }
  case AARCH64_INS_SSHL:
  case AARCH64_INS_USHL: {
    // Per-lane variable shift.  Each lane is shifted by the *signed* value in
    // the least-significant byte of the corresponding amount lane: positive =>
    // left, negative => right (logical for USHL, arithmetic for SSHL).  The
    // old code emitted a single full-width INT_LEFT, which is wrong both in
    // direction and in lane independence (a clang -O2 nibble extractor uses
    // `ushl v.4s` by {0,-4,-8,-12}).
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    NdVar Amt = operandRead(S, ARM64.operands[2]);
    bool IsSigned = (Insn->id == AARCH64_INS_SSHL);
    unsigned ElemSz = 0;
    switch (ARM64.operands[0].vas) {
    case AARCH64LAYOUT_VL_16B:
    case AARCH64LAYOUT_VL_8B:
      ElemSz = 1;
      break;
    case AARCH64LAYOUT_VL_8H:
    case AARCH64LAYOUT_VL_4H:
      ElemSz = 2;
      break;
    case AARCH64LAYOUT_VL_4S:
    case AARCH64LAYOUT_VL_2S:
      ElemSz = 4;
      break;
    case AARCH64LAYOUT_VL_2D:
    case AARCH64LAYOUT_VL_1D:
      ElemSz = 8;
      break;
    default:
      break;
    }
    // Scalar d-form (SSHL/USHL Dd,Dn,Dm) decodes with vas == INVALID, so ElemSz
    // stays 0.  Treat the whole 64-bit register as a single lane (matching the
    // SQSHL/UQSHL/SQSHLU and SLI/SRI handlers) so the signed-amount left/right
    // SELECT below runs; without it the bare INT_LEFT fallback turns a NEGATIVE
    // (right-shift) amount into a giant left shift that flushes the lane to 0.
    if (ElemSz == 0)
      ElemSz = Dst.Size;
    if (ElemSz > 0 && Dst.Size >= ElemSz && Amt.Size >= Dst.Size) {
      unsigned NLanes = Dst.Size / ElemSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar SLane = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, SLane, {Src, NdVar::cst(I * ElemSz, 4)});
        NdVar AByte = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, AByte, {Amt, NdVar::cst(I * ElemSz, 4)});
        NdVar ShAmt = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_SEXT, ShAmt, {AByte});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsNeg, {ShAmt, NdVar::cst(0, ElemSz)});
        NdVar NegAmt = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_NEG2, NegAmt, {ShAmt});
        NdVar LeftRes = S.makeTemp(ElemSz);
        S.emit(NdOp::INT_LEFT, LeftRes, {SLane, ShAmt});
        NdVar RightRes = S.makeTemp(ElemSz);
        S.emit(IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT, RightRes,
               {SLane, NegAmt});
        NdVar LaneRes = S.makeTemp(ElemSz);
        S.emit(NdOp::SELECT, LaneRes, {IsNeg, RightRes, LeftRes});
        if (I == 0) {
          Acc = LaneRes;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {LaneRes, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_LEFT, Dst, {Src, Amt});
    }
    break;
  }
  // Immediate/vector right shifts.  Must be per-lane: a single full-width
  // INT_(S)RIGHT pulls each higher lane's low bits into the lane below
  // (off-by multiples of 2^shift across the lane boundary).
  // Rounding variants (SRSHR/URSHR) add 1<<(n-1) before shifting.
  case AARCH64_INS_SSHR:
  case AARCH64_INS_SRSHR:
  case AARCH64_INS_USHR:
  case AARCH64_INS_URSHR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    NdVar Amt = operandRead(S, ARM64.operands[2]);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SSHR || Insn->id == AARCH64_INS_SRSHR);
    bool IsRounding =
        (Insn->id == AARCH64_INS_SRSHR || Insn->id == AARCH64_INS_URSHR);
    NdOp ShOp = IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar LaneAmt = Amt;
      if (Amt.Size > LaneSz) {
        LaneAmt = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, LaneAmt, {Amt, NdVar::cst(0, 4)});
      }
      NdVar Acc = S.makeTemp(0);
      bool Round = IsRounding && Amt.isConst() && Amt.Offset > 0 &&
                   Amt.Offset <= LaneSz * 8;
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Ls = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ls, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Lr = Round ? S.emitRoundedShr(Ls, LaneSz, Amt.Offset, IsSigned)
                           : S.makeTemp(LaneSz);
        if (!Round)
          S.emit(ShOp, Lr, {Ls, LaneAmt});
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      if (IsRounding && Amt.isConst() && Amt.Offset > 0 &&
          Amt.Offset <= Dst.Size * 8) {
        NdVar R = S.emitRoundedShr(Src, Dst.Size, Amt.Offset, IsSigned);
        S.emit(NdOp::COPY, Dst, {R});
      } else {
        S.emit(ShOp, Dst, {Src, Amt});
      }
    }
    break;
  }
  case AARCH64_INS_SLI:
  case AARCH64_INS_SRI: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSli = (Insn->id == AARCH64_INS_SLI);
    unsigned Sh = static_cast<unsigned>(ARM64.operands[2].imm);
    // Per-lane shift-and-insert (a full-width shift would bleed bits across
    // lane boundaries on vector forms).
    unsigned LaneSz = 0;
    auto Vas = ARM64.operands[0].vas;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    if (LaneSz == 0)
      LaneSz = Dst.Size; // scalar form: single lane
    unsigned NLanes = LaneSz ? Dst.Size / LaneSz : 1;
    unsigned LaneBits = LaneSz * 8;
    // Mask of the bits the inserted (shifted) value occupies.
    uint64_t InsMask;
    if (IsSli)
      InsMask = (Sh >= LaneBits) ? 0 : (~0ULL << Sh);
    else
      InsMask = (Sh >= LaneBits)
                    ? 0
                    : ((LaneBits >= 64) ? (~0ULL >> Sh)
                                        : (((1ULL << LaneBits) - 1) >> Sh));
    NdVar ShCst = NdVar::cst(Sh, LaneSz);
    NdVar InsC = NdVar::cst(InsMask, LaneSz);
    NdVar KeepC = NdVar::cst(~InsMask, LaneSz);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar SLane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, SLane, {Src, NdVar::cst(I * LaneSz, 4)});
      NdVar OLane = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, OLane, {OldDst, NdVar::cst(I * LaneSz, 4)});
      NdVar Shifted = S.makeTemp(LaneSz);
      S.emit(IsSli ? NdOp::INT_LEFT : NdOp::INT_RIGHT, Shifted, {SLane, ShCst});
      NdVar Ins = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_AND, Ins, {Shifted, InsC});
      NdVar Kept = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_AND, Kept, {OLane, KeepC});
      NdVar Res = S.makeTemp(LaneSz);
      S.emit(NdOp::INT_OR, Res, {Ins, Kept});
      if (I == 0) {
        Acc = Res;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Res, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // NEON shift-right-and-accumulate.  Must be per-lane: a single full-width
  // INT_(S)RIGHT pulls each higher lane's low bits into the lane below, and a
  // full-width INT_ADD lets the accumulate carry across lane boundaries.
  // Rounding variants (SRSRA/URSRA) add 1<<(n-1) before shifting.
  case AARCH64_INS_SSRA:
  case AARCH64_INS_SRSRA:
  case AARCH64_INS_USRA:
  case AARCH64_INS_URSRA: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    NdVar Amt = operandRead(S, ARM64.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SSRA || Insn->id == AARCH64_INS_SRSRA);
    bool IsRounding =
        (Insn->id == AARCH64_INS_SRSRA || Insn->id == AARCH64_INS_URSRA);
    NdOp ShOp = IsSigned ? NdOp::INT_ASHR : NdOp::INT_RIGHT;
    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar LaneAmt = Amt;
      if (Amt.Size > LaneSz) {
        LaneAmt = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, LaneAmt, {Amt, NdVar::cst(0, 4)});
      }
      NdVar Acc = S.makeTemp(0);
      bool Round = IsRounding && Amt.isConst() && Amt.Offset > 0 &&
                   Amt.Offset <= LaneSz * 8;
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Ls = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ls, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Sh = Round ? S.emitRoundedShr(Ls, LaneSz, Amt.Offset, IsSigned)
                           : S.makeTemp(LaneSz);
        if (!Round)
          S.emit(ShOp, Sh, {Ls, LaneAmt});
        NdVar Ld = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ld, {OldDst, NdVar::cst(I * LaneSz, 4)});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_ADD, Lr, {Ld, Sh});
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Shifted;
      if (IsRounding && Amt.isConst() && Amt.Offset > 0 &&
          Amt.Offset <= Dst.Size * 8) {
        Shifted = S.emitRoundedShr(Src, Dst.Size, Amt.Offset, IsSigned);
      } else {
        Shifted = S.makeTemp(Dst.Size);
        S.emit(ShOp, Shifted, {Src, Amt});
      }
      S.emit(NdOp::INT_ADD, Dst, {OldDst, Shifted});
    }
    break;
  }
  // NEON vector bitwise insert
  case AARCH64_INS_BSL: {
    // BSL Vd, Vn, Vm: Vd = (Vn & Vd_old) | (Vm & ~Vd_old)
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Mask = NdVar::reg(Dst.Offset, Dst.Size);
    NdVar T1 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T1, {A, Mask});
    NdVar NMask = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NMask, {Mask});
    NdVar T2 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T2, {B, NMask});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }
  case AARCH64_INS_BIT: {
    // BIT Vd, Vn, Vm: Vd = (Vn & Vm) | (Vd_old & ~Vm)
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar T1 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T1, {A, B});
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    NdVar T2 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T2, {NdVar::reg(Dst.Offset, Dst.Size), NB});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }
  case AARCH64_INS_BIF: {
    // BIF Vd, Vn, Vm: Vd = (Vd_old & Vm) | (Vn & ~Vm)
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar T1 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T1, {NdVar::reg(Dst.Offset, Dst.Size), B});
    NdVar NB = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, NB, {B});
    NdVar T2 = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_AND, T2, {A, NB});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }
  // NEON permute / interleave (ZIP1/2, UZP1/2, TRN1/2) — real per-lane
  // permutation built from SUBBYTES element reads + CONCAT concatenation.
  // (Previously emitted as intrinsics with no backend handler, which
  //  silently returned 0 — breaking e.g. mulhi's uzp2-based >>16 extraction.)
  case AARCH64_INS_TRN1:
  case AARCH64_INS_TRN2:
  case AARCH64_INS_ZIP1:
  case AARCH64_INS_ZIP2:
  case AARCH64_INS_UZP1:
  case AARCH64_INS_UZP2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    if (ElemSz == 0 || Dst.Size < ElemSz * 2) {
      // Unknown arrangement — fall back to copying the first source so the
      // value is at least defined rather than a placeholder zero.
      S.emit(NdOp::COPY, Dst, {A});
      break;
    }
    unsigned NLanes = Dst.Size / ElemSz;
    unsigned Half = NLanes / 2;
    auto extractElem = [&](const NdVar &Src, unsigned Idx) -> NdVar {
      NdVar E = S.makeTemp(ElemSz);
      S.emit(NdOp::SUBBYTES, E,
             {Src, NdVar::cst(static_cast<uint64_t>(Idx) * ElemSz, 4)});
      return E;
    };
    NdVar Acc;
    bool First = true;
    for (unsigned I = 0; I < NLanes; ++I) {
      bool FromB = false;
      unsigned SrcIdx = I;
      switch (Insn->id) {
      case AARCH64_INS_UZP1:
        FromB = (I >= Half);
        SrcIdx = 2 * (FromB ? I - Half : I);
        break;
      case AARCH64_INS_UZP2:
        FromB = (I >= Half);
        SrcIdx = 2 * (FromB ? I - Half : I) + 1;
        break;
      case AARCH64_INS_ZIP1:
        FromB = (I & 1);
        SrcIdx = I / 2;
        break;
      case AARCH64_INS_ZIP2:
        FromB = (I & 1);
        SrcIdx = Half + I / 2;
        break;
      case AARCH64_INS_TRN1:
        FromB = (I & 1);
        SrcIdx = I & ~1u;
        break;
      case AARCH64_INS_TRN2:
        FromB = (I & 1);
        SrcIdx = (I & ~1u) + 1;
        break;
      default:
        break;
      }
      NdVar E = extractElem(FromB ? B : A, SrcIdx);
      if (First) {
        Acc = E;
        First = false;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + ElemSz);
        S.emit(NdOp::CONCAT, Next, {E, Acc}); // E = high lane, Acc = low lanes
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // NEON EXT (extract from pair) — ext Vd, Vn, Vm, #index concatenates Vn:Vm
  // (Vn in the low bytes) and extracts a register-width window starting at byte
  // `index`.  Implemented as CONCAT(Vm,Vn) then SUBBYTES at byte `index`.
  // (Previously an intrinsic with no backend handler -> returned 0.)
  case AARCH64_INS_EXT: {
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]); // Vn -> low bytes
    NdVar B = operandRead(S, ARM64.operands[2]); // Vm -> high bytes
    unsigned RegBytes = Dst.Size;
    uint64_t Index = static_cast<uint64_t>(ARM64.operands[3].imm);
    // Normalize source operands to the register width.
    if (A.Size > RegBytes) {
      NdVar T = S.makeTemp(RegBytes);
      S.emit(NdOp::SUBBYTES, T, {A, NdVar::cst(0, 4)});
      A = T;
    }
    if (B.Size > RegBytes) {
      NdVar T = S.makeTemp(RegBytes);
      S.emit(NdOp::SUBBYTES, T, {B, NdVar::cst(0, 4)});
      B = T;
    }
    if (Index == 0) {
      S.emit(NdOp::COPY, Dst, {A});
      break;
    }
    if (Index >= RegBytes) {
      // Out of range (shouldn't happen for valid encodings) -> copy Vm.
      S.emit(NdOp::COPY, Dst, {B});
      break;
    }
    NdVar Combined = S.makeTemp(RegBytes * 2);
    S.emit(NdOp::CONCAT, Combined, {B, A}); // B high, A low
    S.emit(NdOp::SUBBYTES, Dst, {Combined, NdVar::cst(Index, 4)});
    break;
  }
  // NEON TBL/TBX — per-byte table lookup using a SELECT chain.
  //   TBL: result[i] = (idx[i] < table_len) ? table[idx[i]] : 0
  //   TBX: result[i] = (idx[i] < table_len) ? table[idx[i]] : old_dst[i]
  // The table may span 1-4 consecutive vector registers.  Capstone expands the
  // register list `{Vn.16b, Vn+1.16b, ...}` into separate operands, so the
  // index is always the *last* operand and the table registers are everything
  // between the destination and the index.
  case AARCH64_INS_TBL:
  case AARCH64_INS_TBX: {
    if (ARM64.op_count < 3)
      break;
    bool IsTbx = (Insn->id == AARCH64_INS_TBX);
    NdVar Dst = operandWrite(ARM64.operands[0]);
    unsigned IdxOp = ARM64.op_count - 1;
    NdVar Idx = operandRead(S, ARM64.operands[IdxOp]);
    // Concatenate every table register's bytes into one flat lookup table.
    std::vector<NdVar> TblBytes;
    for (unsigned R = 1; R < IdxOp; ++R) {
      NdVar T = operandRead(S, ARM64.operands[R]);
      for (unsigned J = 0; J < T.Size; ++J) {
        NdVar B = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, B, {T, NdVar::cst(J, 4)});
        TblBytes.push_back(B);
      }
    }
    unsigned TblLen = TblBytes.size();
    unsigned NBytes = Dst.Size;
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NBytes; ++I) {
      NdVar IdxByte = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, IdxByte, {Idx, NdVar::cst(I, 4)});
      // Default when no index matches: 0 (TBL) or the original dst byte (TBX).
      NdVar Res = S.makeTemp(1);
      if (IsTbx)
        S.emit(NdOp::SUBBYTES, Res, {OldDst, NdVar::cst(I, 4)});
      else
        S.emit(NdOp::COPY, Res, {NdVar::cst(0, 1)});
      for (unsigned J = 0; J < TblLen; ++J) {
        NdVar IsJ = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsJ, {IdxByte, NdVar::cst(J, 1)});
        NdVar NewRes = S.makeTemp(1);
        S.emit(NdOp::SELECT, NewRes, {IsJ, TblBytes[J], Res});
        Res = NewRes;
      }
      if (I == 0) {
        Acc = Res;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 1);
        S.emit(NdOp::CONCAT, Next, {Res, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // NEON widening add/sub
  case AARCH64_INS_SADDL:
  case AARCH64_INS_SADDL2:
  case AARCH64_INS_UADDL:
  case AARCH64_INS_UADDL2:
  case AARCH64_INS_SADDW:
  case AARCH64_INS_SADDW2:
  case AARCH64_INS_UADDW:
  case AARCH64_INS_UADDW2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);

    auto DstVas = ARM64.operands[0].vas;
    unsigned DstLane = 0;
    if (DstVas == AARCH64LAYOUT_VL_4S)
      DstLane = 4;
    else if (DstVas == AARCH64LAYOUT_VL_8H)
      DstLane = 2;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      DstLane = 8;

    bool IsSigned =
        (Insn->id == AARCH64_INS_SADDL || Insn->id == AARCH64_INS_SADDL2 ||
         Insn->id == AARCH64_INS_SADDW || Insn->id == AARCH64_INS_SADDW2);
    bool IsWideningBoth =
        (Insn->id == AARCH64_INS_SADDL || Insn->id == AARCH64_INS_SADDL2 ||
         Insn->id == AARCH64_INS_UADDL || Insn->id == AARCH64_INS_UADDL2);
    // The "2" variants read the *upper* 64-bit half of the 128-bit source
    // register(s) for their narrow operands.
    bool IsUpper =
        (Insn->id == AARCH64_INS_SADDL2 || Insn->id == AARCH64_INS_UADDL2 ||
         Insn->id == AARCH64_INS_SADDW2 || Insn->id == AARCH64_INS_UADDW2);
    if (DstLane > 0 && Dst.Size > DstLane) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned NarrowLane = DstLane / 2;
      unsigned NarrowBase = IsUpper ? 8 : 0; // high 64 bits for "2" variants
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La, Lb;
        if (IsWideningBoth) {
          NdVar NarrA = S.makeTemp(NarrowLane);
          S.emit(NdOp::SUBBYTES, NarrA,
                 {A, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
          La = S.makeTemp(DstLane);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, La, {NarrA});
          NdVar NarrB = S.makeTemp(NarrowLane);
          S.emit(NdOp::SUBBYTES, NarrB,
                 {B, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
          Lb = S.makeTemp(DstLane);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Lb, {NarrB});
        } else {
          // SADDW/UADDW: A is already wide (read full lane), B is narrow and
          // taken from the upper half for the "2" variant.
          La = S.makeTemp(DstLane);
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * DstLane, 4)});
          NdVar NarrB = S.makeTemp(NarrowLane);
          S.emit(NdOp::SUBBYTES, NarrB,
                 {B, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
          Lb = S.makeTemp(DstLane);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Lb, {NarrB});
        }
        NdVar Lr = S.makeTemp(DstLane);
        S.emit(NdOp::INT_ADD, Lr, {La, Lb});
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLane);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_ADD, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_SSUBL:
  case AARCH64_INS_SSUBL2:
  case AARCH64_INS_USUBL:
  case AARCH64_INS_USUBL2:
  case AARCH64_INS_SSUBW:
  case AARCH64_INS_SSUBW2:
  case AARCH64_INS_USUBW:
  case AARCH64_INS_USUBW2: {
    // Per-lane widening subtract (mirror of the SADDL family).  A plain
    // full-width INT_SUB ignores the sign/zero extension and propagates borrows
    // across the lane boundaries.
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    auto DstVas = ARM64.operands[0].vas;
    unsigned DstLane = 0;
    if (DstVas == AARCH64LAYOUT_VL_4S)
      DstLane = 4;
    else if (DstVas == AARCH64LAYOUT_VL_8H)
      DstLane = 2;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      DstLane = 8;
    bool IsSigned =
        (Insn->id == AARCH64_INS_SSUBL || Insn->id == AARCH64_INS_SSUBL2 ||
         Insn->id == AARCH64_INS_SSUBW || Insn->id == AARCH64_INS_SSUBW2);
    bool IsWideningBoth =
        (Insn->id == AARCH64_INS_SSUBL || Insn->id == AARCH64_INS_SSUBL2 ||
         Insn->id == AARCH64_INS_USUBL || Insn->id == AARCH64_INS_USUBL2);
    bool IsUpper =
        (Insn->id == AARCH64_INS_SSUBL2 || Insn->id == AARCH64_INS_USUBL2 ||
         Insn->id == AARCH64_INS_SSUBW2 || Insn->id == AARCH64_INS_USUBW2);
    if (DstLane > 0 && Dst.Size > DstLane) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned NarrowLane = DstLane / 2;
      unsigned NarrowBase = IsUpper ? 8 : 0;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La, Lb;
        if (IsWideningBoth) {
          NdVar NarrA = S.makeTemp(NarrowLane);
          S.emit(NdOp::SUBBYTES, NarrA,
                 {A, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
          La = S.makeTemp(DstLane);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, La, {NarrA});
        } else {
          La = S.makeTemp(DstLane);
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * DstLane, 4)});
        }
        NdVar NarrB = S.makeTemp(NarrowLane);
        S.emit(NdOp::SUBBYTES, NarrB,
               {B, NdVar::cst(NarrowBase + I * NarrowLane, 4)});
        Lb = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Lb, {NarrB});
        NdVar Lr = S.makeTemp(DstLane);
        S.emit(NdOp::INT_SUB, Lr, {La, Lb});
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLane);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_SUB, Dst, {A, B});
    }
    break;
  }
  // NEON {ADD,SUB}HN{2} / R{ADD,SUB}HN{2} — add/subtract returning high narrow:
  // each wide lane is added/subtracted at full width, then the HIGH half of the
  // result is taken and packed into the narrow destination.  The "2" variants
  // write the narrowed lanes into the UPPER half (preserving the low half); the
  // rounding variants add 1<<(narrowBits-1) before taking the high half.  The
  // old placeholder did a full-width INT_ADD/INT_SUB with no narrowing.
  case AARCH64_INS_ADDHN:
  case AARCH64_INS_ADDHN2:
  case AARCH64_INS_RADDHN:
  case AARCH64_INS_RADDHN2:
  case AARCH64_INS_SUBHN:
  case AARCH64_INS_SUBHN2:
  case AARCH64_INS_RSUBHN:
  case AARCH64_INS_RSUBHN2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    bool IsSub =
        (Insn->id == AARCH64_INS_SUBHN || Insn->id == AARCH64_INS_SUBHN2 ||
         Insn->id == AARCH64_INS_RSUBHN || Insn->id == AARCH64_INS_RSUBHN2);
    bool IsRound =
        (Insn->id == AARCH64_INS_RADDHN || Insn->id == AARCH64_INS_RADDHN2 ||
         Insn->id == AARCH64_INS_RSUBHN || Insn->id == AARCH64_INS_RSUBHN2);
    bool IsUpper =
        (Insn->id == AARCH64_INS_ADDHN2 || Insn->id == AARCH64_INS_RADDHN2 ||
         Insn->id == AARCH64_INS_SUBHN2 || Insn->id == AARCH64_INS_RSUBHN2);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned WideSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_2D)
      WideSz = 8;
    else if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      WideSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      WideSz = 2;
    if (WideSz < 2) {
      S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Dst, {A, B});
      break;
    }
    unsigned NarrowSz = WideSz / 2;
    unsigned NLanes = A.Size / WideSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar AL = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, AL, {A, NdVar::cst(I * WideSz, 4)});
      NdVar BL = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, BL, {B, NdVar::cst(I * WideSz, 4)});
      NdVar Sum = S.makeTemp(WideSz);
      S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Sum, {AL, BL});
      if (IsRound) {
        NdVar Rounded = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Rounded,
               {Sum, NdVar::cst(1ULL << (NarrowSz * 8 - 1), WideSz)});
        Sum = Rounded;
      }
      NdVar Narrow = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Sum, NdVar::cst(NarrowSz, 4)});
      if (I == 0) {
        Acc = Narrow;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + NarrowSz);
        S.emit(NdOp::CONCAT, Next, {Narrow, Acc});
        Acc = Next;
      }
    }
    if (IsUpper && Dst.Size > Acc.Size) {
      unsigned LoSz = Dst.Size - Acc.Size;
      NdVar Lo = S.makeTemp(LoSz);
      S.emit(NdOp::SUBBYTES, Lo,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Acc, Lo});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  // NEON extend
  // {S,U}SHLL{,2} — widening shift-left-long: each narrow lane is sign/zero
  // extended to the wide element and shifted left by an immediate.  A single
  // full-width INT_SEXT/INT_ZEXT (the old behaviour) is not per-lane and drops
  // the shift.  The "2" variants take the upper 64 bits of the source.
  case AARCH64_INS_SSHLL:
  case AARCH64_INS_SSHLL2:
  case AARCH64_INS_USHLL:
  case AARCH64_INS_USHLL2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SSHLL || Insn->id == AARCH64_INS_SSHLL2);
    bool IsUpper =
        (Insn->id == AARCH64_INS_SSHLL2 || Insn->id == AARCH64_INS_USHLL2);
    uint64_t Shift = 0;
    if (ARM64.op_count >= 3 && ARM64.operands[2].type == AARCH64_OP_IMM)
      Shift = static_cast<uint64_t>(ARM64.operands[2].imm);
    unsigned SrcLaneSz = 0;
    switch (ARM64.operands[1].vas) {
    case AARCH64LAYOUT_VL_8B:
    case AARCH64LAYOUT_VL_16B:
      SrcLaneSz = 1;
      break;
    case AARCH64LAYOUT_VL_4H:
    case AARCH64LAYOUT_VL_8H:
      SrcLaneSz = 2;
      break;
    case AARCH64LAYOUT_VL_2S:
    case AARCH64LAYOUT_VL_4S:
      SrcLaneSz = 4;
      break;
    default:
      break;
    }
    if (SrcLaneSz > 0 && SrcLaneSz < 8) {
      unsigned DstLaneSz = SrcLaneSz * 2;
      unsigned NLanes = Dst.Size / DstLaneSz;
      unsigned Base = IsUpper ? 8 : 0;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar SLane = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, SLane,
               {Src, NdVar::cst(Base + I * SrcLaneSz, 4)});
        NdVar Wide = S.makeTemp(DstLaneSz);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Wide, {SLane});
        NdVar Shifted = Wide;
        if (Shift > 0) {
          Shifted = S.makeTemp(DstLaneSz);
          S.emit(NdOp::INT_LEFT, Shifted,
                 {Wide, NdVar::cst(Shift, DstLaneSz)});
        }
        if (I == 0) {
          Acc = Shifted;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
          S.emit(NdOp::CONCAT, Next, {Shifted, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Dst, {Src});
    }
    break;
  }
  // XTN — extract narrow: truncate each wide lane to half width (no
  // saturation). XTN2 writes the narrowed lanes into the UPPER half, preserving
  // the low half. (Was an intrinsic with no backend handler ->
  // silently returned 0.)
  case AARCH64_INS_XTN:
  case AARCH64_INS_XTN2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned WideSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      WideSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      WideSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_2D)
      WideSz = 8;
    if (WideSz == 0) {
      S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
      break;
    }
    unsigned NarrowSz = WideSz / 2;
    unsigned NLanes = Src.Size / WideSz;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * WideSz, 4)});
      NdVar Narrow = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Lane, NdVar::cst(0, 4)});
      if (I == 0) {
        Acc = Narrow;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + NarrowSz);
        S.emit(NdOp::CONCAT, Next, {Narrow, Acc});
        Acc = Next;
      }
    }
    if (Insn->id == AARCH64_INS_XTN2 && Dst.Size > Acc.Size) {
      unsigned LoSz = Dst.Size - Acc.Size;
      NdVar Lo = S.makeTemp(LoSz);
      S.emit(NdOp::SUBBYTES, Lo,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Acc, Lo});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  // SQXTN — signed saturating narrow: each wide lane clamped to signed narrow
  // range.
  case AARCH64_INS_SQXTN:
  case AARCH64_INS_SQXTN2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned WideSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      WideSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      WideSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_2D)
      WideSz = 8;
    if (WideSz == 0) {
      // Scalar form (sqxtn s,d / h,s / b,h): the source's full width is the
      // single wide lane; saturate it instead of plain truncation.
      if (Src.Size == 2 || Src.Size == 4 || Src.Size == 8)
        WideSz = Src.Size;
      else {
        S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
        break;
      }
    }
    unsigned NarrowSz = WideSz / 2;
    unsigned NLanes = Src.Size / WideSz;
    int64_t SMax = (1LL << (NarrowSz * 8 - 1)) - 1;
    int64_t SMin = -(1LL << (NarrowSz * 8 - 1));
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * WideSz, 4)});
      // Narrowing saturate via trunc+sext overflow detect (avoids fork's
      // InstCombine mis-fold on INT_SLESS+SELECT clamp chains).
      NdVar Narrow = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Lane, NdVar::cst(0, 4)});
      NdVar BackWide = S.makeTemp(WideSz);
      S.emit(NdOp::INT_SEXT, BackWide, {Narrow});
      NdVar Fits = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, Fits, {Lane, BackWide});
      NdVar IsPos = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, IsPos, {NdVar::cst(0, WideSz), Lane});
      NdVar OvfVal = S.makeTemp(NarrowSz);
      S.emit(NdOp::SELECT, OvfVal,
             {IsPos, NdVar::cst(static_cast<uint64_t>(SMax), NarrowSz),
              NdVar::cst(static_cast<uint64_t>(SMin), NarrowSz)});
      NdVar Result = S.makeTemp(NarrowSz);
      S.emit(NdOp::SELECT, Result, {Fits, Narrow, OvfVal});
      if (I == 0) {
        Acc = Result;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + NarrowSz);
        S.emit(NdOp::CONCAT, Next, {Result, Acc});
        Acc = Next;
      }
    }
    // The "2" form writes the narrowed lanes into the UPPER half of Dst and
    // preserves the lower half (the prior narrow result); the base form
    // replaces the whole register.  Without this SQXTN2 dropped the SQXTN low
    // half.
    if (Insn->id == AARCH64_INS_SQXTN2 && Dst.Size > Acc.Size) {
      unsigned LoSz = Dst.Size - Acc.Size;
      NdVar Lo = S.makeTemp(LoSz);
      S.emit(NdOp::SUBBYTES, Lo,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Acc, Lo});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  // UQXTN — unsigned saturating narrow.
  case AARCH64_INS_UQXTN:
  case AARCH64_INS_UQXTN2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned WideSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      WideSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      WideSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_2D)
      WideSz = 8;
    if (WideSz == 0) {
      // Scalar form (uqxtn s,d / h,s / b,h): saturate the single wide lane.
      if (Src.Size == 2 || Src.Size == 4 || Src.Size == 8)
        WideSz = Src.Size;
      else {
        S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
        break;
      }
    }
    unsigned NarrowSz = WideSz / 2;
    unsigned NLanes = Src.Size / WideSz;
    uint64_t UMax = (1ULL << (NarrowSz * 8)) - 1;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * WideSz, 4)});
      NdVar GtMax = S.makeTemp(1);
      S.emit(NdOp::INT_LESS, GtMax, {NdVar::cst(UMax, WideSz), Lane});
      NdVar Clamped = S.makeTemp(WideSz);
      S.emit(NdOp::SELECT, Clamped, {GtMax, NdVar::cst(UMax, WideSz), Lane});
      NdVar Narrow = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Clamped, NdVar::cst(0, 4)});
      if (I == 0) {
        Acc = Narrow;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + NarrowSz);
        S.emit(NdOp::CONCAT, Next, {Narrow, Acc});
        Acc = Next;
      }
    }
    // The "2" form writes the narrowed lanes into Dst's UPPER half, preserving
    // the lower half (the prior narrow result).
    if (Insn->id == AARCH64_INS_UQXTN2 && Dst.Size > Acc.Size) {
      unsigned LoSz = Dst.Size - Acc.Size;
      NdVar Lo = S.makeTemp(LoSz);
      S.emit(NdOp::SUBBYTES, Lo,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Acc, Lo});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  // SQXTUN — signed-to-unsigned saturating narrow.
  case AARCH64_INS_SQXTUN:
  case AARCH64_INS_SQXTUN2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned WideSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      WideSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      WideSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_2D)
      WideSz = 8;
    if (WideSz == 0) {
      // Scalar form (sqxtun s,d / h,s / b,h): saturate the single wide lane.
      if (Src.Size == 2 || Src.Size == 4 || Src.Size == 8)
        WideSz = Src.Size;
      else {
        S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
        break;
      }
    }
    unsigned NarrowSz = WideSz / 2;
    unsigned NLanes = Src.Size / WideSz;
    uint64_t UMax = (1ULL << (NarrowSz * 8)) - 1;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * WideSz, 4)});
      NdVar IsNeg = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, IsNeg, {Lane, NdVar::cst(0, WideSz)});
      NdVar Clamped1 = S.makeTemp(WideSz);
      S.emit(NdOp::SELECT, Clamped1, {IsNeg, NdVar::cst(0, WideSz), Lane});
      NdVar GtMax = S.makeTemp(1);
      S.emit(NdOp::INT_LESS, GtMax, {NdVar::cst(UMax, WideSz), Clamped1});
      NdVar Clamped2 = S.makeTemp(WideSz);
      S.emit(NdOp::SELECT, Clamped2,
             {GtMax, NdVar::cst(UMax, WideSz), Clamped1});
      NdVar Narrow = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Narrow, {Clamped2, NdVar::cst(0, 4)});
      if (I == 0) {
        Acc = Narrow;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + NarrowSz);
        S.emit(NdOp::CONCAT, Next, {Narrow, Acc});
        Acc = Next;
      }
    }
    // The "2" form writes the narrowed lanes into Dst's UPPER half, preserving
    // the lower half (the prior narrow result).
    if (Insn->id == AARCH64_INS_SQXTUN2 && Dst.Size > Acc.Size) {
      unsigned LoSz = Dst.Size - Acc.Size;
      NdVar Lo = S.makeTemp(LoSz);
      S.emit(NdOp::SUBBYTES, Lo,
             {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {Acc, Lo});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }
  // NEON abs / Neg — per-lane absolute value
  case AARCH64_INS_ABS:
  case AARCH64_INS_SQABS: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    // SQABS saturates abs(INT_MIN) to INT_MAX; plain ABS leaves it as INT_MIN.
    bool IsSat = (Insn->id == AARCH64_INS_SQABS);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;

    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      unsigned Bits = LaneSz * 8;
      uint64_t MinV = static_cast<uint64_t>(1) << (Bits - 1);
      uint64_t MaxV = MinV - 1;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Neg = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, Neg, {Lane});
        NdVar IsNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsNeg, {Lane, NdVar::cst(0, LaneSz)});
        NdVar Sel = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Sel, {IsNeg, Neg, Lane});
        if (IsSat) {
          NdVar IsMin = S.makeTemp(1);
          S.emit(NdOp::INT_EQUAL, IsMin, {Lane, NdVar::cst(MinV, LaneSz)});
          NdVar Sat = S.makeTemp(LaneSz);
          S.emit(NdOp::SELECT, Sat, {IsMin, NdVar::cst(MaxV, LaneSz), Sel});
          Sel = Sat;
        }
        if (I == 0) {
          Acc = Sel;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Sel, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      NdVar Neg = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_NEG2, Neg, {Src});
      NdVar IsNeg = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, IsNeg, {Src, NdVar::cst(0, Src.Size)});
      NdVar Sel = S.makeTemp(Src.Size);
      S.emit(NdOp::SELECT, Sel, {IsNeg, Neg, Src});
      if (IsSat && Src.Size > 0 && Src.Size <= 8) {
        unsigned Bits = Src.Size * 8;
        uint64_t MinV = static_cast<uint64_t>(1) << (Bits - 1);
        NdVar IsMin = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsMin, {Src, NdVar::cst(MinV, Src.Size)});
        S.emit(NdOp::SELECT, Dst,
               {IsMin, NdVar::cst(MinV - 1, Src.Size), Sel});
      } else {
        S.emit(NdOp::COPY, Dst, {Sel});
      }
    }
    break;
  }
  case AARCH64_INS_SQNEG: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    // Signed saturating negate is PER-LANE and clamps `-INT_MIN` to INT_MAX;
    // the old whole-register INT_NEG2 propagated borrows across lanes and
    // never saturated.
    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      unsigned Bits = LaneSz * 8;
      uint64_t MinV = static_cast<uint64_t>(1) << (Bits - 1);
      uint64_t MaxV = MinV - 1;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Neg = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, Neg, {Lane});
        NdVar IsMin = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsMin, {Lane, NdVar::cst(MinV, LaneSz)});
        NdVar Sel = S.makeTemp(LaneSz);
        S.emit(NdOp::SELECT, Sel, {IsMin, NdVar::cst(MaxV, LaneSz), Neg});
        if (I == 0) {
          Acc = Sel;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Sel, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      // Scalar form (b/h/s/d): negate and saturate -INT_MIN -> INT_MAX.
      NdVar Neg = S.makeTemp(Src.Size);
      S.emit(NdOp::INT_NEG2, Neg, {Src});
      if (Src.Size > 0 && Src.Size <= 8) {
        unsigned Bits = Src.Size * 8;
        uint64_t MinV = static_cast<uint64_t>(1) << (Bits - 1);
        NdVar IsMin = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsMin, {Src, NdVar::cst(MinV, Src.Size)});
        S.emit(NdOp::SELECT, Dst,
               {IsMin, NdVar::cst(MinV - 1, Src.Size), Neg});
      } else {
        S.emit(NdOp::COPY, Dst, {Neg});
      }
    }
    break;
  }
  // NEON CNT — per-byte population count.
  case AARCH64_INS_CNT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    unsigned NBytes = Dst.Size;
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NBytes; ++I) {
      NdVar Byte = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, Byte, {Src, NdVar::cst(I, 4)});
      NdVar Cnt = S.makeTemp(1);
      S.emit(NdOp::POPCOUNT, Cnt, {Byte});
      if (I == 0) {
        Acc = Cnt;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 1);
        S.emit(NdOp::CONCAT, Next, {Cnt, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case AARCH64_INS_NOT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::INT_NOT, Dst, {Src});
    break;
  }
  // NEON horizontal reductions — sum all lanes.
  case AARCH64_INS_ADDV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned LaneSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_16B || SrcVas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    if (LaneSz > 0 && Src.Size > LaneSz) {
      unsigned NLanes = Src.Size / LaneSz;
      NdVar Sum = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, Sum, {Src, NdVar::cst(0, 4)});
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar NewSum = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_ADD, NewSum, {Sum, Lane});
        Sum = NewSum;
      }
      if (Dst.Size > LaneSz) {
        S.emit(NdOp::INT_ZEXT, Dst, {Sum});
      } else {
        S.emit(NdOp::COPY, Dst, {Sum});
      }
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case AARCH64_INS_SADDLV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned LaneSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_16B || SrcVas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    if (LaneSz > 0 && Src.Size > LaneSz) {
      unsigned NLanes = Src.Size / LaneSz;
      unsigned DstLaneSz = LaneSz * 2;
      if (DstLaneSz > Dst.Size)
        DstLaneSz = Dst.Size;
      NdVar First = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, First, {Src, NdVar::cst(0, 4)});
      NdVar Sum = S.makeTemp(DstLaneSz);
      S.emit(NdOp::INT_SEXT, Sum, {First});
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Wide = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_SEXT, Wide, {Lane});
        NdVar NewSum = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_ADD, NewSum, {Sum, Wide});
        Sum = NewSum;
      }
      if (Dst.Size > DstLaneSz) {
        S.emit(NdOp::INT_SEXT, Dst, {Sum});
      } else {
        S.emit(NdOp::COPY, Dst, {Sum});
      }
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  // UADDLV — unsigned add-long-across-lanes (each lane zero-extended then
  // summed).
  case AARCH64_INS_UADDLV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned LaneSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_16B || SrcVas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    if (LaneSz > 0 && Src.Size > LaneSz) {
      unsigned NLanes = Src.Size / LaneSz;
      unsigned DstLaneSz = LaneSz * 2;
      if (DstLaneSz > Dst.Size)
        DstLaneSz = Dst.Size;
      NdVar First = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, First, {Src, NdVar::cst(0, 4)});
      NdVar Sum = S.makeTemp(DstLaneSz);
      S.emit(NdOp::INT_ZEXT, Sum, {First});
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Wide = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_ZEXT, Wide, {Lane});
        NdVar NewSum = S.makeTemp(DstLaneSz);
        S.emit(NdOp::INT_ADD, NewSum, {Sum, Wide});
        Sum = NewSum;
      }
      if (Dst.Size > DstLaneSz) {
        S.emit(NdOp::INT_ZEXT, Dst, {Sum});
      } else {
        S.emit(NdOp::COPY, Dst, {Sum});
      }
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  // {S,U}{MIN,MAX}V <V><d>, <Vn>.<T> — reduce min/max across all lanes into a
  // scalar.  Previously these emitted an unimplemented intrinsic that
  // the backend dropped (result became 0).  Lower to an explicit per-lane
  // compare/select reduction chain.
  case AARCH64_INS_SMAXV:
  case AARCH64_INS_UMAXV:
  case AARCH64_INS_SMINV:
  case AARCH64_INS_UMINV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    unsigned LaneSz = 0, NLanes = 0;
    switch (ARM64.operands[1].vas) {
    case AARCH64LAYOUT_VL_16B:
      LaneSz = 1;
      NLanes = 16;
      break;
    case AARCH64LAYOUT_VL_8B:
      LaneSz = 1;
      NLanes = 8;
      break;
    case AARCH64LAYOUT_VL_8H:
      LaneSz = 2;
      NLanes = 8;
      break;
    case AARCH64LAYOUT_VL_4H:
      LaneSz = 2;
      NLanes = 4;
      break;
    case AARCH64LAYOUT_VL_4S:
      LaneSz = 4;
      NLanes = 4;
      break;
    case AARCH64LAYOUT_VL_2S:
      LaneSz = 4;
      NLanes = 2;
      break;
    default:
      break;
    }
    if (LaneSz == 0 || NLanes < 2) {
      S.emit(NdOp::COPY, Dst, {Src});
      break;
    }
    bool IsMin =
        (Insn->id == AARCH64_INS_SMINV || Insn->id == AARCH64_INS_UMINV);
    bool IsSigned =
        (Insn->id == AARCH64_INS_SMINV || Insn->id == AARCH64_INS_SMAXV);
    NdOp LessOp = IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
    NdVar Acc = S.makeTemp(LaneSz);
    S.emit(NdOp::SUBBYTES, Acc, {Src, NdVar::cst(0, 4)});
    for (unsigned I = 1; I < NLanes; ++I) {
      NdVar L = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, L, {Src, NdVar::cst(I * LaneSz, 4)});
      NdVar Cmp = S.makeTemp(1);
      if (IsMin)
        S.emit(LessOp, Cmp, {L, Acc}); // pick L when L < Acc
      else
        S.emit(LessOp, Cmp, {Acc, L}); // pick L when Acc < L
      NdVar NewAcc = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, NewAcc, {Cmp, L, Acc});
      Acc = NewAcc;
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // FMAXV/FMINV (FMIN/FMAX semantics) / FMAXNMV/FMINNMV (minNum/maxNum) —
  // horizontal FP max/min reduction across lanes to a scalar.  The V/NMV split
  // is NaN handling: V propagates NaN (llvm.minimum/maximum), NMV suppresses it
  // (llvm.minnum/maxnum); a plain FLOAT_LESS+SELECT got both wrong.
  case AARCH64_INS_FMAXV:
  case AARCH64_INS_FMINV:
  case AARCH64_INS_FMAXNMV:
  case AARCH64_INS_FMINNMV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    bool IsMin =
        (Insn->id == AARCH64_INS_FMINV || Insn->id == AARCH64_INS_FMINNMV);
    bool IsNM =
        (Insn->id == AARCH64_INS_FMINNMV || Insn->id == AARCH64_INS_FMAXNMV);
    NdOp MM = IsNM ? (IsMin ? NdOp::FLOAT_MINNUM : NdOp::FLOAT_MAXNUM)
                   : (IsMin ? NdOp::FLOAT_MIN : NdOp::FLOAT_MAX);
    unsigned ElemSz = neonElemSize(ARM64.operands[1].vas);
    if (ElemSz < 4 || Src.Size <= ElemSz) {
      // Unknown / half-precision / scalar: just take the low element.
      NdVar Lo = S.makeTemp(ElemSz ? ElemSz : Dst.Size);
      S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
      S.emit(NdOp::COPY, Dst, {Lo});
      break;
    }
    unsigned NLanes = Src.Size / ElemSz;
    NdVar Acc = S.makeTemp(ElemSz);
    S.emit(NdOp::SUBBYTES, Acc, {Src, NdVar::cst(0, 4)});
    for (unsigned I = 1; I < NLanes; ++I) {
      NdVar Lane = S.makeTemp(ElemSz);
      S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * ElemSz, 4)});
      NdVar New = S.makeTemp(ElemSz);
      S.emit(MM, New, {Acc, Lane});
      Acc = New;
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case AARCH64_INS_FADDP: {
    NdVar Dst = operandWrite(ARM64.operands[0]);
    if (ARM64.op_count == 2) {
      // Scalar pairwise: faddp Sd, Vn.2S  →  Sd = Vn[0] + Vn[1]
      NdVar Src = operandRead(S, ARM64.operands[1]);
      auto SrcVas = ARM64.operands[1].vas;
      unsigned LaneSz = 0;
      if (SrcVas == AARCH64LAYOUT_VL_2S)
        LaneSz = 4;
      else if (SrcVas == AARCH64LAYOUT_VL_2D)
        LaneSz = 8;
      if (LaneSz > 0 && Src.Size >= 2 * LaneSz) {
        NdVar Lo = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        NdVar Hi = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Hi, {Src, NdVar::cst(LaneSz, 4)});
        S.emit(NdOp::FLOAT_ADD, Dst, {Lo, Hi});
      } else {
        S.emit(NdOp::COPY, Dst, {Src});
      }
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);

    auto Vas = ARM64.operands[0].vas;
    unsigned LaneSz = 0;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;

    if (LaneSz > 0 && A.Size > LaneSz) {
      unsigned NPairs = A.Size / (LaneSz * 2);
      NdVar Acc = S.makeTemp(0);
      for (unsigned H = 0; H < 2; ++H) {
        NdVar Src = (H == 0) ? A : B;
        for (unsigned I = 0; I < NPairs; ++I) {
          NdVar Lo = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(I * 2 * LaneSz, 4)});
          NdVar Hi = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Hi,
                 {Src, NdVar::cst(I * 2 * LaneSz + LaneSz, 4)});
          NdVar Sum = S.makeTemp(LaneSz);
          S.emit(NdOp::FLOAT_ADD, Sum, {Lo, Hi});
          unsigned Idx = H * NPairs + I;
          if (Idx == 0) {
            Acc = Sum;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Sum, Acc});
            Acc = Next;
          }
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::FLOAT_ADD, Dst, {A, B});
    }
    break;
  }
  // NEON widening multiply (long).  SMULL2/UMULL2 take the UPPER half of the
  // 128-bit source operands; SQDMULL/SQDMULL2 additionally double the product
  // and saturate.  All are per-lane widening (narrow*narrow -> wide); the
  // previous full-width INT_MULT i128 propagated carries across lanes and was
  // silently wrong (broke e.g. mulhi's smull2-based high-half products).
  case AARCH64_INS_SMULL2:
  case AARCH64_INS_UMULL2:
  case AARCH64_INS_SQDMULL:
  case AARCH64_INS_SQDMULL2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    bool IsUpper =
        (Insn->id == AARCH64_INS_SMULL2 || Insn->id == AARCH64_INS_UMULL2 ||
         Insn->id == AARCH64_INS_SQDMULL2);
    bool IsDoubling =
        (Insn->id == AARCH64_INS_SQDMULL || Insn->id == AARCH64_INS_SQDMULL2);
    bool IsSigned = (Insn->id != AARCH64_INS_UMULL2);
    unsigned DstLane = 0;
    auto DstVas = ARM64.operands[0].vas;
    if (DstVas == AARCH64LAYOUT_VL_4S)
      DstLane = 4;
    else if (DstVas == AARCH64LAYOUT_VL_8H)
      DstLane = 2;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      DstLane = 8;
    if (DstLane > 0 && Dst.Size > DstLane) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned NarrowLane = DstLane / 2;
      unsigned Base = IsUpper ? (NLanes * NarrowLane) : 0; // upper 64 bits
      // By-element `smull2/sqdmull v.Ts, v.Th, vN.<ty>[idx]`: operandRead
      // returns just the selected element (no Base/upper-half offset), so
      // broadcast it.
      bool BScalar = (B.Size <= NarrowLane);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar NarrA = S.makeTemp(NarrowLane);
        S.emit(NdOp::SUBBYTES, NarrA,
               {A, NdVar::cst(Base + I * NarrowLane, 4)});
        NdVar WA = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WA, {NarrA});
        NdVar NarrB = BScalar ? B : S.makeTemp(NarrowLane);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, NarrB,
                 {B, NdVar::cst(Base + I * NarrowLane, 4)});
        NdVar WB = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WB, {NarrB});
        NdVar Lr = S.makeTemp(DstLane);
        S.emit(NdOp::INT_MULT, Lr, {WA, WB});
        if (IsDoubling && DstLane <= 4) {
          // result = saturate(2 * prod).  Only the (MIN,MIN) product overflows;
          // compute in a 2x-wide temp, double, clamp to the signed range.
          unsigned Wide = DstLane * 2;
          unsigned Bits = DstLane * 8;
          int64_t MaxV = (1LL << (Bits - 1)) - 1;
          int64_t MinV = -(1LL << (Bits - 1));
          NdVar P2 = S.makeTemp(Wide);
          S.emit(NdOp::INT_SEXT, P2, {Lr});
          NdVar Dbl = S.makeTemp(Wide);
          S.emit(NdOp::INT_LEFT, Dbl, {P2, NdVar::cst(1, Wide)});
          NdVar TooHi = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, TooHi,
                 {NdVar::cst((uint64_t)MaxV, Wide), Dbl});
          NdVar C1 = S.makeTemp(Wide);
          S.emit(NdOp::SELECT, C1,
                 {TooHi, NdVar::cst((uint64_t)MaxV, Wide), Dbl});
          NdVar TooLo = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, TooLo,
                 {C1, NdVar::cst((uint64_t)MinV, Wide)});
          NdVar C2 = S.makeTemp(Wide);
          S.emit(NdOp::SELECT, C2,
                 {TooLo, NdVar::cst((uint64_t)MinV, Wide), C1});
          NdVar Narrowed = S.makeTemp(DstLane);
          S.emit(NdOp::SUBBYTES, Narrowed, {C2, NdVar::cst(0, 4)});
          Lr = Narrowed;
        } else if (IsDoubling) {
          // 64-bit dest: doubling the i64 product can only overflow when both
          // narrow sources are INT_MIN (2*MIN*MIN == 2^63); saturate that lane
          // to INT64_MAX (a 2x-wide i128 clamp can't represent the bound).
          NdVar Dbl = S.makeTemp(DstLane);
          S.emit(NdOp::INT_LEFT, Dbl, {Lr, NdVar::cst(1, DstLane)});
          unsigned NBits = NarrowLane * 8;
          uint64_t NMin = 1ULL << (NBits - 1);
          NdVar AMin = S.makeTemp(1), BMin = S.makeTemp(1);
          S.emit(NdOp::INT_EQUAL, AMin,
                 {NarrA, NdVar::cst(NMin, NarrowLane)});
          S.emit(NdOp::INT_EQUAL, BMin,
                 {NarrB, NdVar::cst(NMin, NarrowLane)});
          NdVar Both = S.makeTemp(1);
          S.emit(NdOp::INT_AND, Both, {AMin, BMin});
          NdVar Sat = S.makeTemp(DstLane);
          S.emit(NdOp::SELECT, Sat,
                 {Both, NdVar::cst(0x7FFFFFFFFFFFFFFFULL, DstLane), Dbl});
          Lr = Sat;
        }
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLane);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      // Scalar / unknown-arrangement fallback: widen then multiply.
      NdVar ExtA = S.makeTemp(8), ExtB = S.makeTemp(8);
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, ExtA, {A});
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, ExtB, {B});
      S.emit(NdOp::INT_MULT, Dst, {ExtA, ExtB});
    }
    break;
  }
  // PMUL — polynomial (carry-less) multiply, same element width (i8 lanes,
  // `.8b`/`.16b`).  GF(2)[x] multiply, NOT integer multiply; map to the AArch64
  // NEON intrinsic.  Was grouped with SVE2 widening mults as a bare INT_MULT.
  case AARCH64_INS_PMUL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emitIntrinsic(Intrinsic::A64_Pmul, Dst, {A, B});
    break;
  }
  // PMULL/PMULL2 — polynomial (carry-less) multiply long.  Two element widths:
  // p8 (`.8b`→`.8h`, 8 byte-pairs → 8 halfwords) and p64 (`.1d`→`.1q`, one
  // doubleword → 128-bit).  The "2" form multiplies the *upper* 64-bit lane.
  // The element width is passed to the emitter as a trailing constant so it can
  // pick @llvm.aarch64.neon.pmull (p8) vs pmull64 (p64); the old handler
  // emitted a bare intrinsic with no handler and silently returned 0.
  case AARCH64_INS_PMULL:
  case AARCH64_INS_PMULL2: {
    if (ARM64.op_count < 3)
      break;
    bool IsUpper = (Insn->id == AARCH64_INS_PMULL2);
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    unsigned ElemSz = neonElemSize(ARM64.operands[1].vas);
    if (ElemSz != 1 && ElemSz != 8) {
      unsigned DstElem = neonElemSize(ARM64.operands[0].vas);
      ElemSz = (DstElem >= 16) ? 8 : 1; // 1Q dst -> p64, else p8
    }
    auto lowOrHigh = [&](NdVar V) -> NdVar {
      if (V.Size <= 8)
        return V;
      NdVar H = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, H, {V, NdVar::cst(IsUpper ? 8 : 0, 4)});
      return H;
    };
    A = lowOrHigh(A);
    B = lowOrHigh(B);
    S.emitIntrinsic(Intrinsic::A64_Pmull, Dst, {A, B, NdVar::cst(ElemSz, 4)});
    break;
  }
  // NEON widening multiply-accumulate (long).  The wide accumulator Dst is
  // updated per-lane: Dst.lane += / -= widen(narrow A.lane) * widen(narrow
  // B.lane).  The "2" variants take the UPPER half of the 128-bit sources;
  // SQDMLAL/SQDMLSL double+saturate the product and saturate the accumulate;
  // the indexed form (`smlal v.4s, v.4h, v.h[idx]`) broadcasts a single B lane.
  // The previous full-width INT_MULT i128 + INT_ADD ignored widening, lane
  // structure, signedness and the "2" upper-half read (broke q15 sums).
  case AARCH64_INS_SMLAL:
  case AARCH64_INS_SMLAL2:
  case AARCH64_INS_UMLAL:
  case AARCH64_INS_UMLAL2:
  case AARCH64_INS_SQDMLAL:
  case AARCH64_INS_SQDMLAL2:
  case AARCH64_INS_SMLSL:
  case AARCH64_INS_SMLSL2:
  case AARCH64_INS_UMLSL:
  case AARCH64_INS_UMLSL2:
  case AARCH64_INS_SQDMLSL:
  case AARCH64_INS_SQDMLSL2: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    bool IsSub =
        (Insn->id == AARCH64_INS_SMLSL || Insn->id == AARCH64_INS_SMLSL2 ||
         Insn->id == AARCH64_INS_UMLSL || Insn->id == AARCH64_INS_UMLSL2 ||
         Insn->id == AARCH64_INS_SQDMLSL || Insn->id == AARCH64_INS_SQDMLSL2);
    bool IsUpper =
        (Insn->id == AARCH64_INS_SMLAL2 || Insn->id == AARCH64_INS_UMLAL2 ||
         Insn->id == AARCH64_INS_SQDMLAL2 || Insn->id == AARCH64_INS_SMLSL2 ||
         Insn->id == AARCH64_INS_UMLSL2 || Insn->id == AARCH64_INS_SQDMLSL2);
    bool IsDoubling =
        (Insn->id == AARCH64_INS_SQDMLAL || Insn->id == AARCH64_INS_SQDMLAL2 ||
         Insn->id == AARCH64_INS_SQDMLSL || Insn->id == AARCH64_INS_SQDMLSL2);
    bool IsSigned =
        (Insn->id != AARCH64_INS_UMLAL && Insn->id != AARCH64_INS_UMLAL2 &&
         Insn->id != AARCH64_INS_UMLSL && Insn->id != AARCH64_INS_UMLSL2);
    unsigned DstLane = 0;
    auto DstVas = ARM64.operands[0].vas;
    if (DstVas == AARCH64LAYOUT_VL_4S)
      DstLane = 4;
    else if (DstVas == AARCH64LAYOUT_VL_8H)
      DstLane = 2;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      DstLane = 8;
    if (DstLane > 0 && Dst.Size > DstLane) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned NarrowLane = DstLane / 2;
      unsigned Base = IsUpper ? (NLanes * NarrowLane) : 0; // upper 64 bits
      bool BScalar = (B.Size <= NarrowLane); // indexed scalar broadcast
      NdOp ExtOp = IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT;
      // Signed saturating add/sub at the lane width (no i128): used for the
      // SQDMLAL/SQDMLSL ACCUMULATE, which clamps Vd +/- product to the dest
      // element's signed range.  Overflow is detected from operand/result signs
      // (ADD overflows only when addends share a sign, SUB only when they
      // differ, and in either case the result sign flips away from the first
      // operand's), then the lane is forced to MIN (first operand negative) or
      // MAX.  Plain SMLAL/UMLAL do not saturate, so this is gated on
      // IsDoubling.
      auto satAddSub = [&](NdVar AVal, NdVar BVal, unsigned W,
                           bool Sub) -> NdVar {
        unsigned WBits = W * 8;
        uint64_t MaxV =
            (WBits >= 64) ? 0x7FFFFFFFFFFFFFFFULL : ((1ULL << (WBits - 1)) - 1);
        uint64_t MinV =
            (WBits >= 64) ? 0x8000000000000000ULL : (1ULL << (WBits - 1));
        NdVar Res = S.makeTemp(W);
        S.emit(Sub ? NdOp::INT_SUB : NdOp::INT_ADD, Res, {AVal, BVal});
        NdVar ANeg = S.makeTemp(1), BNeg = S.makeTemp(1),
                RNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, ANeg, {AVal, NdVar::cst(0, W)});
        S.emit(NdOp::INT_SLESS, BNeg, {BVal, NdVar::cst(0, W)});
        S.emit(NdOp::INT_SLESS, RNeg, {Res, NdVar::cst(0, W)});
        NdVar SignCond = S.makeTemp(1);
        if (Sub)
          S.emit(NdOp::BOOL_XOR, SignCond, {ANeg, BNeg}); // operands differ
        else
          S.emit(NdOp::INT_EQUAL, SignCond, {ANeg, BNeg}); // operands match
        NdVar SignFlip = S.makeTemp(1);
        S.emit(NdOp::BOOL_XOR, SignFlip, {ANeg, RNeg});
        NdVar Ovf = S.makeTemp(1);
        S.emit(NdOp::INT_AND, Ovf, {SignCond, SignFlip});
        NdVar SatVal = S.makeTemp(W);
        S.emit(NdOp::SELECT, SatVal,
               {ANeg, NdVar::cst(MinV, W), NdVar::cst(MaxV, W)});
        NdVar Out = S.makeTemp(W);
        S.emit(NdOp::SELECT, Out, {Ovf, SatVal, Res});
        return Out;
      };
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar NarrA = S.makeTemp(NarrowLane);
        S.emit(NdOp::SUBBYTES, NarrA,
               {A, NdVar::cst(Base + I * NarrowLane, 4)});
        NdVar WA = S.makeTemp(DstLane);
        S.emit(ExtOp, WA, {NarrA});
        // Keep the narrow B lane (indexed forms broadcast the scalar B) so the
        // 64-bit doubling-saturation corner can test it for INT_MIN.
        NdVar NarrB = BScalar ? B : S.makeTemp(NarrowLane);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, NarrB,
                 {B, NdVar::cst(Base + I * NarrowLane, 4)});
        NdVar WB = S.makeTemp(DstLane);
        S.emit(ExtOp, WB, {NarrB});
        NdVar Prod = S.makeTemp(DstLane);
        S.emit(NdOp::INT_MULT, Prod, {WA, WB});
        if (IsDoubling && DstLane <= 4) {
          // SQDMLAL: product = saturate(2 * A*B) computed in a 2x-wide temp.
          unsigned Wide = DstLane * 2;
          unsigned Bits = DstLane * 8;
          int64_t MaxV = (1LL << (Bits - 1)) - 1;
          int64_t MinV = -(1LL << (Bits - 1));
          NdVar P2 = S.makeTemp(Wide);
          S.emit(NdOp::INT_SEXT, P2, {Prod});
          NdVar Dbl = S.makeTemp(Wide);
          S.emit(NdOp::INT_LEFT, Dbl, {P2, NdVar::cst(1, Wide)});
          NdVar TooHi = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, TooHi,
                 {NdVar::cst((uint64_t)MaxV, Wide), Dbl});
          NdVar C1 = S.makeTemp(Wide);
          S.emit(NdOp::SELECT, C1,
                 {TooHi, NdVar::cst((uint64_t)MaxV, Wide), Dbl});
          NdVar TooLo = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, TooLo,
                 {C1, NdVar::cst((uint64_t)MinV, Wide)});
          NdVar C2 = S.makeTemp(Wide);
          S.emit(NdOp::SELECT, C2,
                 {TooLo, NdVar::cst((uint64_t)MinV, Wide), C1});
          NdVar Narrowed = S.makeTemp(DstLane);
          S.emit(NdOp::SUBBYTES, Narrowed, {C2, NdVar::cst(0, 4)});
          Prod = Narrowed;
        } else if (IsDoubling) {
          // 64-bit dest: doubling the i64 product overflows only when both
          // narrow sources are INT_MIN (2*MIN*MIN == 2^63); saturate that lane
          // to INT64_MAX (an i128 clamp can't represent the bound).  Mirrors
          // the SQDMULL handler.
          NdVar Dbl = S.makeTemp(DstLane);
          S.emit(NdOp::INT_LEFT, Dbl, {Prod, NdVar::cst(1, DstLane)});
          unsigned NBits = NarrowLane * 8;
          uint64_t NMin = 1ULL << (NBits - 1);
          NdVar AMin = S.makeTemp(1), BMin = S.makeTemp(1);
          S.emit(NdOp::INT_EQUAL, AMin,
                 {NarrA, NdVar::cst(NMin, NarrowLane)});
          S.emit(NdOp::INT_EQUAL, BMin,
                 {NarrB, NdVar::cst(NMin, NarrowLane)});
          NdVar Both = S.makeTemp(1);
          S.emit(NdOp::INT_AND, Both, {AMin, BMin});
          NdVar Sat = S.makeTemp(DstLane);
          S.emit(NdOp::SELECT, Sat,
                 {Both, NdVar::cst(0x7FFFFFFFFFFFFFFFULL, DstLane), Dbl});
          Prod = Sat;
        }
        NdVar LaneDst = S.makeTemp(DstLane);
        S.emit(NdOp::SUBBYTES, LaneDst, {OldDst, NdVar::cst(I * DstLane, 4)});
        NdVar Lr;
        if (IsDoubling) {
          // SQDMLAL/SQDMLSL saturate Vd +/- product to the dest element range.
          Lr = satAddSub(LaneDst, Prod, DstLane, IsSub);
        } else {
          Lr = S.makeTemp(DstLane);
          S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Lr, {LaneDst, Prod});
        }
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLane);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      // Scalar / unknown-arrangement fallback: widen then multiply-accumulate.
      NdVar ExtA = S.makeTemp(8), ExtB = S.makeTemp(8);
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, ExtA, {A});
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, ExtB, {B});
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {ExtA, ExtB});
      if (IsDoubling) {
        NdVar Dbl = S.makeTemp(Dst.Size);
        S.emit(NdOp::INT_LEFT, Dbl, {Prod, NdVar::cst(1, Dst.Size)});
        Prod = Dbl;
      }
      S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Dst, {OldDst, Prod});
    }
    break;
  }
  // NEON saturating doubling multiply returning the high half.
  // SQDMULH:  Dst[i] = sat((2*A[i]*B[i]) >> N)              (saturates
  // A==B==MIN) SQRDMULH: Dst[i] = sat((2*A[i]*B[i] + 2^(N-1)) >> N) (rounding
  // variant) SQRDMLAH/SQRDMLSH: Dst[i] = sat(OldDst[i] +/- sqrdmulh). Old code
  // was a plain full-width INT_MULT placeholder (no doubling, high half,
  // rounding, saturation, per-lane, nor accumulate).
  case AARCH64_INS_SQDMULH:
  case AARCH64_INS_SQRDMULH:
  case AARCH64_INS_SQRDMLAH:
  case AARCH64_INS_SQRDMLSH: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    bool IsRounding =
        (Insn->id == AARCH64_INS_SQRDMULH || Insn->id == AARCH64_INS_SQRDMLAH ||
         Insn->id == AARCH64_INS_SQRDMLSH);
    bool IsAccum =
        (Insn->id == AARCH64_INS_SQRDMLAH || Insn->id == AARCH64_INS_SQRDMLSH);
    bool IsAccSub = (Insn->id == AARCH64_INS_SQRDMLSH);
    bool ByElem = (ARM64.operands[2].vector_index >= 0);

    unsigned LaneSz = 0;
    auto Vas = ARM64.operands[0].vas;
    if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    if (LaneSz == 0)
      LaneSz = (Dst.Size == 2 || Dst.Size == 4) ? Dst.Size : 4; // scalar h/s
    unsigned NLanes = Dst.Size / LaneSz;
    unsigned Wide = LaneSz * 2;
    unsigned LaneBits = LaneSz * 8;
    uint64_t MinV = 1ULL << (LaneBits - 1);
    uint64_t MaxV = MinV - 1;
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);

    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar La = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
      NdVar Lb = S.makeTemp(LaneSz);
      if (ByElem)
        Lb = B; // by-element: same selected lane broadcast to all
      else
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
      NdVar WA = S.makeTemp(Wide), WB = S.makeTemp(Wide);
      S.emit(NdOp::INT_SEXT, WA, {La});
      S.emit(NdOp::INT_SEXT, WB, {Lb});
      NdVar Prod = S.makeTemp(Wide);
      S.emit(NdOp::INT_MULT, Prod, {WA, WB});
      NdVar Dbl = S.makeTemp(Wide);
      S.emit(NdOp::INT_LEFT, Dbl, {Prod, NdVar::cst(1, Wide)});
      if (IsRounding) {
        NdVar Rnd = S.makeTemp(Wide);
        S.emit(NdOp::INT_ADD, Rnd, {Dbl, NdVar::cst(MinV, Wide)});
        Dbl = Rnd;
      }
      NdVar High = S.makeTemp(Wide);
      S.emit(NdOp::INT_ASHR, High, {Dbl, NdVar::cst(LaneBits, Wide)});
      NdVar HighN = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, HighN, {High, NdVar::cst(0, 4)});
      // Saturation: the only overflow is A==B==INT_MIN -> INT_MAX.
      NdVar AIsMin = S.makeTemp(1), BIsMin = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, AIsMin, {La, NdVar::cst(MinV, LaneSz)});
      S.emit(NdOp::INT_EQUAL, BIsMin, {Lb, NdVar::cst(MinV, LaneSz)});
      NdVar BothMin = S.makeTemp(1);
      S.emit(NdOp::INT_AND, BothMin, {AIsMin, BIsMin});
      NdVar MulRes = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, MulRes,
             {BothMin, NdVar::cst(MaxV, LaneSz), HighN});

      NdVar LaneRes = MulRes;
      if (IsAccum) {
        NdVar OLane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, OLane, {OldDst, NdVar::cst(I * LaneSz, 4)});
        NdVar WOld = S.makeTemp(Wide), WMul = S.makeTemp(Wide);
        S.emit(NdOp::INT_SEXT, WOld, {OLane});
        S.emit(NdOp::INT_SEXT, WMul, {MulRes});
        NdVar Sum = S.makeTemp(Wide);
        S.emit(IsAccSub ? NdOp::INT_SUB : NdOp::INT_ADD, Sum, {WOld, WMul});
        NdVar GtMax = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, GtMax, {NdVar::cst(MaxV, Wide), Sum});
        NdVar C1 = S.makeTemp(Wide);
        S.emit(NdOp::SELECT, C1, {GtMax, NdVar::cst(MaxV, Wide), Sum});
        NdVar LtMin = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, LtMin,
               {C1, NdVar::cst(static_cast<uint64_t>(-(int64_t)MinV), Wide)});
        NdVar C2 = S.makeTemp(Wide);
        S.emit(NdOp::SELECT, C2,
               {LtMin,
                NdVar::cst(static_cast<uint64_t>(-(int64_t)MinV), Wide), C1});
        LaneRes = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, LaneRes, {C2, NdVar::cst(0, 4)});
      }
      if (I == 0) {
        Acc = LaneRes;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {LaneRes, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // NEON pairwise add long — adjacent pairs of narrow lanes summed into wider
  // lanes.
  case AARCH64_INS_SADDLP:
  case AARCH64_INS_UADDLP: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    bool IsSigned = (Insn->id == AARCH64_INS_SADDLP);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned NarrowSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      NarrowSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      NarrowSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_16B || SrcVas == AARCH64LAYOUT_VL_8B)
      NarrowSz = 1;
    if (NarrowSz == 0) {
      S.emit(NdOp::COPY, Dst, {Src});
      break;
    }
    unsigned WideSz = NarrowSz * 2;
    unsigned NPairs = Src.Size / (NarrowSz * 2);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NPairs; ++I) {
      NdVar Lo = S.makeTemp(NarrowSz);
      NdVar Hi = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(I * 2 * NarrowSz, 4)});
      S.emit(NdOp::SUBBYTES, Hi,
             {Src, NdVar::cst(I * 2 * NarrowSz + NarrowSz, 4)});
      NdVar WLo = S.makeTemp(WideSz);
      NdVar WHi = S.makeTemp(WideSz);
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WLo, {Lo});
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WHi, {Hi});
      NdVar Sum = S.makeTemp(WideSz);
      S.emit(NdOp::INT_ADD, Sum, {WLo, WHi});
      if (I == 0) {
        Acc = Sum;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + WideSz);
        S.emit(NdOp::CONCAT, Next, {Sum, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // SADALP/UADALP — pairwise add long AND ACCUMULATE: Dst[j] += widen(Src[2j])
  // + widen(Src[2j+1]).  Same as SADDLP/UADDLP plus accumulation into the
  // existing destination lanes.  Was an intrinsic placeholder returning 0.
  case AARCH64_INS_SADALP:
  case AARCH64_INS_UADALP: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    bool IsSigned = (Insn->id == AARCH64_INS_SADALP);
    auto SrcVas = ARM64.operands[1].vas;
    unsigned NarrowSz = 0;
    if (SrcVas == AARCH64LAYOUT_VL_4S || SrcVas == AARCH64LAYOUT_VL_2S)
      NarrowSz = 4;
    else if (SrcVas == AARCH64LAYOUT_VL_8H || SrcVas == AARCH64LAYOUT_VL_4H)
      NarrowSz = 2;
    else if (SrcVas == AARCH64LAYOUT_VL_16B || SrcVas == AARCH64LAYOUT_VL_8B)
      NarrowSz = 1;
    if (NarrowSz == 0) {
      S.emit(NdOp::COPY, Dst, {Src});
      break;
    }
    unsigned WideSz = NarrowSz * 2;
    unsigned NPairs = Src.Size / (NarrowSz * 2);
    NdVar OldDst = NdVar::reg(Dst.Offset, Dst.Size);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NPairs; ++I) {
      NdVar Lo = S.makeTemp(NarrowSz);
      NdVar Hi = S.makeTemp(NarrowSz);
      S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(I * 2 * NarrowSz, 4)});
      S.emit(NdOp::SUBBYTES, Hi,
             {Src, NdVar::cst(I * 2 * NarrowSz + NarrowSz, 4)});
      NdVar WLo = S.makeTemp(WideSz);
      NdVar WHi = S.makeTemp(WideSz);
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WLo, {Lo});
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WHi, {Hi});
      NdVar Sum = S.makeTemp(WideSz);
      S.emit(NdOp::INT_ADD, Sum, {WLo, WHi});
      NdVar OldLane = S.makeTemp(WideSz);
      S.emit(NdOp::SUBBYTES, OldLane, {OldDst, NdVar::cst(I * WideSz, 4)});
      NdVar Acc2 = S.makeTemp(WideSz);
      S.emit(NdOp::INT_ADD, Acc2, {Sum, OldLane});
      if (I == 0) {
        Acc = Acc2;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + WideSz);
        S.emit(NdOp::CONCAT, Next, {Acc2, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // NEON FP width convert: FCVTL/FCVTL2 widen single->double, FCVTN/FCVTN2
  // narrow double->single.  Must be per-lane with the real per-lane source
  // width, otherwise the emitter infers the type from the full 16-byte vector
  // and picks the wrong direction (a widen becomes an fptrunc).
  case AARCH64_INS_FCVTN:
  case AARCH64_INS_FCVTN2:
  case AARCH64_INS_FCVTL:
  case AARCH64_INS_FCVTL2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    bool Widen =
        (Insn->id == AARCH64_INS_FCVTL || Insn->id == AARCH64_INS_FCVTL2);
    bool Hi =
        (Insn->id == AARCH64_INS_FCVTL2 || Insn->id == AARCH64_INS_FCVTN2);
    unsigned DstLane = neonElemSize(ARM64.operands[0].vas);
    unsigned SrcLane = neonElemSize(ARM64.operands[1].vas);
    // Supported conversions: half<->float and float<->double (per-lane).
    bool ValidWiden = Widen && ((SrcLane == 4 && DstLane == 8) ||
                                (SrcLane == 2 && DstLane == 4));
    bool ValidNarrow = !Widen && ((SrcLane == 8 && DstLane == 4) ||
                                  (SrcLane == 4 && DstLane == 2));
    if (!ValidWiden && !ValidNarrow) {
      S.emit(NdOp::FLOAT_FLOAT2FLOAT, Dst, {Src});
      break;
    }
    if (Widen) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned SrcBase = Hi ? NLanes * SrcLane : 0;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar E = S.makeTemp(SrcLane);
        S.emit(NdOp::SUBBYTES, E,
               {Src, NdVar::cst(SrcBase + I * SrcLane, 4)});
        NdVar L = S.makeTemp(DstLane);
        S.emit(NdOp::FLOAT_FLOAT2FLOAT, L, {E});
        if (I == 0) {
          Acc = L;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLane);
          S.emit(NdOp::CONCAT, Next, {L, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      // Narrow: N wide lanes -> N narrow lanes per-lane.
      // FCVTN zeroes upper half, FCVTN2 writes upper half preserving lower.
      unsigned NLanes = Src.Size / SrcLane;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar E = S.makeTemp(SrcLane);
        S.emit(NdOp::SUBBYTES, E, {Src, NdVar::cst(I * SrcLane, 4)});
        NdVar L = S.makeTemp(DstLane);
        S.emit(NdOp::FLOAT_FLOAT2FLOAT, L, {E});
        if (I == 0) {
          Acc = L;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + DstLane);
          S.emit(NdOp::CONCAT, Next, {L, Acc});
          Acc = Next;
        }
      }
      unsigned PairSz = NLanes * DstLane;
      if (Hi) {
        NdVar Lo = S.makeTemp(PairSz);
        S.emit(NdOp::SUBBYTES, Lo,
               {NdVar::reg(Dst.Offset, Dst.Size), NdVar::cst(0, 4)});
        S.emit(NdOp::CONCAT, Dst, {Acc, Lo});
      } else if (Dst.Size > PairSz) {
        NdVar ZHi = S.makeTemp(Dst.Size - PairSz);
        S.emit(NdOp::COPY, ZHi,
               {NdVar::cst(0, (uint16_t)(Dst.Size - PairSz))});
        S.emit(NdOp::CONCAT, Dst, {ZHi, Acc});
      } else {
        S.emit(NdOp::COPY, Dst, {Acc});
      }
    }
    break;
  }
  // NEON float max/min
  // FMINNM/FMAXNM — element-wise IEEE minNum/maxNum (NaN-suppressing: return
  // the numeric operand when one input is NaN).  A naive (a<b)?a:b select
  // returns the wrong operand on NaN and on signed zeros, so use the dedicated
  // ops.
  case AARCH64_INS_FMAXNM:
  case AARCH64_INS_FMINNM: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdOp MM = (Insn->id == AARCH64_INS_FMINNM) ? NdOp::FLOAT_MINNUM
                                               : NdOp::FLOAT_MAXNUM;
    unsigned LaneSz = neonElemSize(ARM64.operands[0].vas);
    if ((LaneSz == 4 || LaneSz == 8) && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(MM, Lr, {La, Lb});
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(MM, Dst, {A, B});
    }
    break;
  }

  // FMINP/FMAXP (FMIN/FMAX semantics) and FMINNMP/FMAXNMP (minNum/maxNum) —
  // pairwise reduce: each result lane is the min/max of an ADJACENT pair drawn
  // from the concatenation of the two source vectors (low half from Vn, high
  // half from Vm), or from the single source for the scalar 2-operand form.
  // The previous code reduced element-wise (min(A[i],B[i])), which is wrong.
  case AARCH64_INS_FMAXNMP:
  case AARCH64_INS_FMINNMP:
  case AARCH64_INS_FMAXP:
  case AARCH64_INS_FMINP: {
    NdVar Dst = operandWrite(ARM64.operands[0]);
    bool IsMin =
        (Insn->id == AARCH64_INS_FMINP || Insn->id == AARCH64_INS_FMINNMP);
    bool IsNM =
        (Insn->id == AARCH64_INS_FMINNMP || Insn->id == AARCH64_INS_FMAXNMP);
    NdOp MM = IsNM ? (IsMin ? NdOp::FLOAT_MINNUM : NdOp::FLOAT_MAXNUM)
                   : (IsMin ? NdOp::FLOAT_MIN : NdOp::FLOAT_MAX);
    if (ARM64.op_count == 2) {
      NdVar Src = operandRead(S, ARM64.operands[1]);
      unsigned LaneSz = neonElemSize(ARM64.operands[1].vas);
      if ((LaneSz == 4 || LaneSz == 8) && Src.Size >= 2 * LaneSz) {
        NdVar Lo = S.makeTemp(LaneSz), Hi = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(0, 4)});
        S.emit(NdOp::SUBBYTES, Hi, {Src, NdVar::cst(LaneSz, 4)});
        S.emit(MM, Dst, {Lo, Hi});
      } else {
        S.emit(NdOp::COPY, Dst, {Src});
      }
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    unsigned LaneSz = neonElemSize(ARM64.operands[0].vas);
    if ((LaneSz == 4 || LaneSz == 8) && A.Size >= 2 * LaneSz) {
      unsigned NPairs = A.Size / (LaneSz * 2);
      NdVar Acc = S.makeTemp(0);
      for (unsigned H = 0; H < 2; ++H) {
        NdVar Src = (H == 0) ? A : B;
        for (unsigned I = 0; I < NPairs; ++I) {
          NdVar Lo = S.makeTemp(LaneSz), Hi = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Lo, {Src, NdVar::cst(I * 2 * LaneSz, 4)});
          S.emit(NdOp::SUBBYTES, Hi,
                 {Src, NdVar::cst(I * 2 * LaneSz + LaneSz, 4)});
          NdVar Pr = S.makeTemp(LaneSz);
          S.emit(MM, Pr, {Lo, Hi});
          if (H == 0 && I == 0) {
            Acc = Pr;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + LaneSz);
            S.emit(NdOp::CONCAT, Next, {Pr, Acc});
            Acc = Next;
          }
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(MM, Dst, {A, B});
    }
    break;
  }
  // NEON reciprocal estimate / step.  The element width (f32 vs f64) is encoded
  // in the operand layout (4S/2S vs 2D) and is ambiguous from the register size
  // alone (16 bytes = 4×f32 or 2×f64), so pass it to the emitter as a trailing
  // constant; scalar S/D forms fall back to the destination size.
  case AARCH64_INS_FRECPE: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    if (ElemSz != 2 && ElemSz != 4 && ElemSz != 8)
      ElemSz = (Dst.Size == 2) ? 2 : (Dst.Size >= 8) ? 8 : 4;
    S.emitIntrinsic(Intrinsic::A64_Frecpe, Dst, {Src, NdVar::cst(ElemSz, 4)});
    break;
  }
  case AARCH64_INS_FRSQRTE: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    if (ElemSz != 2 && ElemSz != 4 && ElemSz != 8)
      ElemSz = (Dst.Size == 2) ? 2 : (Dst.Size >= 8) ? 8 : 4;
    S.emitIntrinsic(Intrinsic::A64_Frsqrte, Dst,
                    {Src, NdVar::cst(ElemSz, 4)});
    break;
  }
  case AARCH64_INS_URECPE: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Urecpe, Dst, {Src});
    break;
  }
  case AARCH64_INS_URSQRTE: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Ursqrte, Dst, {Src});
    break;
  }
  case AARCH64_INS_FRECPS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    if (ElemSz != 2 && ElemSz != 4 && ElemSz != 8)
      ElemSz = (Dst.Size == 2) ? 2 : (Dst.Size >= 8) ? 8 : 4;
    S.emitIntrinsic(Intrinsic::A64_Frecps, Dst,
                    {A, B, NdVar::cst(ElemSz, 4)});
    break;
  }
  case AARCH64_INS_FRSQRTS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    if (ElemSz != 2 && ElemSz != 4 && ElemSz != 8)
      ElemSz = (Dst.Size == 2) ? 2 : (Dst.Size >= 8) ? 8 : 4;
    S.emitIntrinsic(Intrinsic::A64_Frsqrts, Dst,
                    {A, B, NdVar::cst(ElemSz, 4)});
    break;
  }
  // NEON / FP misc
  case AARCH64_INS_FRECPX: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Frecpe, Dst, {Src});
    break;
  }
  // NEON structure load/store.  The last operand is the memory ADDRESS — it
  // must go through operandEffAddr (compute EA) rather than operandRead (which
  // performs a LOAD and returns the loaded value, causing a double indirection
  // that reads array data as a pointer -> wild address / UC_ERR_READ_UNMAPPED).
  case AARCH64_INS_LD1:
  case AARCH64_INS_LD2:
  case AARCH64_INS_LD3:
  case AARCH64_INS_LD4:
  case AARCH64_INS_LD1R:
  case AARCH64_INS_LD2R:
  case AARCH64_INS_LD3R:
  case AARCH64_INS_LD4R: {
    if (ARM64.op_count < 2)
      break;
    bool IsReplicate =
        (Insn->id == AARCH64_INS_LD1R || Insn->id == AARCH64_INS_LD2R ||
         Insn->id == AARCH64_INS_LD3R || Insn->id == AARCH64_INS_LD4R);
    int MemIdx = -1;
    for (int I = 0; I < ARM64.op_count; ++I)
      if (ARM64.operands[I].type == AARCH64_OP_MEM) {
        MemIdx = I;
        break;
      }
    if (MemIdx < 1) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar EA = operandEffAddr(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::LOAD, Dst, {EA});
      break;
    }
    unsigned NRegs = static_cast<unsigned>(MemIdx);
    unsigned Struct =
        (Insn->id == AARCH64_INS_LD2 || Insn->id == AARCH64_INS_LD2R)   ? 2
        : (Insn->id == AARCH64_INS_LD3 || Insn->id == AARCH64_INS_LD3R) ? 3
        : (Insn->id == AARCH64_INS_LD4 || Insn->id == AARCH64_INS_LD4R) ? 4
                                                                        : 1;
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    int LaneIdx = ARM64.operands[0].vector_index;
    // See ST1-4: structure addressing is `[Xn]`; take base register directly so
    // a post-index disp (`[x9], #4`) is not folded into the load address.
    NdVar Base;
    {
      const auto &MO = ARM64.operands[MemIdx].mem;
      if (MO.base != AARCH64_REG_INVALID) {
        auto RI = mapCapstoneReg(static_cast<aarch64_reg>(MO.base));
        Base = S.makeTemp(8);
        S.emit(NdOp::COPY, Base, {NdVar::reg(RI.Offset, 8)});
      } else {
        Base = operandEffAddr(S, ARM64.operands[ARM64.op_count - 1]);
      }
    }
    NdVar Dst0 = operandWrite(ARM64.operands[0]);
    unsigned RegSize = Dst0.Size;
    auto addrAt = [&](unsigned Off) -> NdVar {
      if (Off == 0)
        return Base;
      NdVar A = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, A, {Base, NdVar::cst(Off, 8)});
      return A;
    };
    unsigned TotalBytes = 0;
    if (IsReplicate && ElemSz > 0 && ElemSz < RegSize &&
        RegSize % ElemSz == 0) {
      // LDxR: load one element per register and replicate across all lanes.
      unsigned NLanes = RegSize / ElemSz;
      for (unsigned R = 0; R < NRegs; ++R) {
        NdVar Elem = S.makeTemp(ElemSz);
        S.emit(NdOp::LOAD, Elem, {addrAt(R * ElemSz)});
        NdVar Acc = Elem;
        for (unsigned I = 1; I < NLanes; ++I) {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {Elem, Acc});
          Acc = Next;
        }
        S.emit(NdOp::COPY, operandWrite(ARM64.operands[R]), {Acc});
      }
      TotalBytes = NRegs * ElemSz;
    } else if (LaneIdx >= 0 && ElemSz > 0) {
      // Indexed single-element form: read-modify-write lane LaneIdx of each
      // reg.
      for (unsigned R = 0; R < NRegs; ++R) {
        // Read the *whole* register as the merge base.  operandRead would
        // honour the operand's vector_index and hand back just the indexed
        // lane, which breaks the read-modify-write (the kept lanes come out
        // wrong).
        NdVar Out = operandWrite(ARM64.operands[R]);
        NdVar Cur = Out;
        NdVar El = S.makeTemp(ElemSz);
        S.emit(NdOp::LOAD, El, {addrAt(R * ElemSz)});
        unsigned ByteOff = static_cast<unsigned>(LaneIdx) * ElemSz;
        // LaneIdx is only checked as >= 0; guard against a malformed index
        // whose lane spills past the register, which would underflow the
        // Cur.Size - ByteOff - ElemSz high-slice size below into a huge
        // makeTemp.
        if (ByteOff + ElemSz > Cur.Size)
          break;
        if (ByteOff == 0) {
          NdVar Hi = S.makeTemp(Cur.Size - ElemSz);
          S.emit(NdOp::SUBBYTES, Hi, {Cur, NdVar::cst(ElemSz, 4)});
          S.emit(NdOp::CONCAT, Out, {Hi, El});
        } else if (ByteOff + ElemSz == Cur.Size) {
          NdVar Lo = S.makeTemp(ByteOff);
          S.emit(NdOp::SUBBYTES, Lo, {Cur, NdVar::cst(0, 4)});
          S.emit(NdOp::CONCAT, Out, {El, Lo});
        } else {
          NdVar Lo = S.makeTemp(ByteOff);
          S.emit(NdOp::SUBBYTES, Lo, {Cur, NdVar::cst(0, 4)});
          NdVar Hi = S.makeTemp(Cur.Size - ByteOff - ElemSz);
          S.emit(NdOp::SUBBYTES, Hi, {Cur, NdVar::cst(ByteOff + ElemSz, 4)});
          NdVar Mid = S.makeTemp(ByteOff + ElemSz);
          S.emit(NdOp::CONCAT, Mid, {El, Lo});
          S.emit(NdOp::CONCAT, Out, {Hi, Mid});
        }
      }
      TotalBytes = NRegs * ElemSz;
    } else if (Struct == 1 || ElemSz == 0 || RegSize == 0 ||
               RegSize % ElemSz != 0) {
      // Contiguous: load each whole register back-to-back.
      for (unsigned R = 0; R < NRegs; ++R)
        S.emit(NdOp::LOAD, operandWrite(ARM64.operands[R]),
               {addrAt(R * RegSize)});
      TotalBytes = NRegs * RegSize;
    } else {
      // De-interleaved load (LD2/LD3/LD4): Regs[R][L] = in[L*NRegs + R].
      unsigned NLanes = RegSize / ElemSz;
      for (unsigned R = 0; R < NRegs; ++R) {
        NdVar Acc = S.makeTemp(0);
        bool First = true;
        for (unsigned L = 0; L < NLanes; ++L) {
          NdVar El = S.makeTemp(ElemSz);
          S.emit(NdOp::LOAD, El, {addrAt((L * NRegs + R) * ElemSz)});
          if (First) {
            Acc = El;
            First = false;
          } else {
            NdVar Next = S.makeTemp(Acc.Size + ElemSz);
            S.emit(NdOp::CONCAT, Next, {El, Acc});
            Acc = Next;
          }
        }
        S.emit(NdOp::COPY, operandWrite(ARM64.operands[R]), {Acc});
      }
      TotalBytes = NLanes * NRegs * ElemSz;
    }
    if (Insn->detail->writeback &&
        ARM64.operands[MemIdx].mem.base != AARCH64_REG_INVALID) {
      const auto &M = ARM64.operands[MemIdx];
      auto BI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
      NdVar BaseReg = NdVar::reg(BI.Offset, 8);
      aarch64_reg PostReg = AARCH64_REG_INVALID;
      if (M.mem.index != AARCH64_REG_INVALID)
        PostReg = static_cast<aarch64_reg>(M.mem.index);
      else if (MemIdx + 1 < ARM64.op_count &&
               ARM64.operands[MemIdx + 1].type == AARCH64_OP_REG)
        PostReg = static_cast<aarch64_reg>(ARM64.operands[MemIdx + 1].reg);
      if (PostReg != AARCH64_REG_INVALID) {
        auto PI = mapCapstoneReg(PostReg);
        S.emit(NdOp::INT_ADD, BaseReg, {BaseReg, NdVar::reg(PI.Offset, 8)});
      } else {
        int64_t WB = M.mem.disp != 0 ? M.mem.disp : (int64_t)TotalBytes;
        if (MemIdx + 1 < ARM64.op_count &&
            ARM64.operands[MemIdx + 1].type == AARCH64_OP_IMM)
          WB = ARM64.operands[MemIdx + 1].imm;
        if (WB != 0)
          S.emit(NdOp::INT_ADD, BaseReg,
                 {BaseReg, NdVar::cst(static_cast<uint64_t>(WB), 8)});
      }
    }
    break;
  }
  case AARCH64_INS_ST1:
  case AARCH64_INS_ST2:
  case AARCH64_INS_ST3:
  case AARCH64_INS_ST4: {
    if (ARM64.op_count < 2)
      break;
    // `stN {v0,...,vk}.<T>, [Xn]{, <wb>}` transfers a *list* of registers.
    // ST1 is contiguous; ST2/ST3/ST4 INTERLEAVE the registers element-by-
    // element (out[lane*N + reg] = reg[lane]).  The previous handler stored
    // only operands[0] contiguously, so `st4 {v0-v3}` (clang's transposed
    // matrix store) wrote one register and zero-filled the rest.
    int MemIdx = -1;
    for (int I = 0; I < ARM64.op_count; ++I)
      if (ARM64.operands[I].type == AARCH64_OP_MEM) {
        MemIdx = I;
        break;
      }
    if (MemIdx < 1) {
      NdVar Src = operandRead(S, ARM64.operands[0]);
      NdVar EA = operandEffAddr(S, ARM64.operands[ARM64.op_count - 1]);
      S.emit(NdOp::STORE, {}, {EA, Src});
      break;
    }
    unsigned NRegs = static_cast<unsigned>(MemIdx);
    unsigned Struct = (Insn->id == AARCH64_INS_ST2)   ? 2
                      : (Insn->id == AARCH64_INS_ST3) ? 3
                      : (Insn->id == AARCH64_INS_ST4) ? 4
                                                      : 1;
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    int LaneIdx = ARM64.operands[0].vector_index;
    // NEON structure addressing is always `[Xn]`; a post-index disp/reg is a
    // writeback amount, NOT part of the access EA.  operandEffAddr would add
    // mem.disp (e.g. `[x9], #4` -> x9+4), shifting every element by one — so
    // take the base register directly.
    NdVar Base;
    {
      const auto &MO = ARM64.operands[MemIdx].mem;
      if (MO.base != AARCH64_REG_INVALID) {
        auto RI = mapCapstoneReg(static_cast<aarch64_reg>(MO.base));
        Base = S.makeTemp(8);
        S.emit(NdOp::COPY, Base, {NdVar::reg(RI.Offset, 8)});
      } else {
        Base = operandEffAddr(S, ARM64.operands[MemIdx]);
      }
    }
    std::vector<NdVar> Regs;
    for (unsigned R = 0; R < NRegs; ++R)
      Regs.push_back(operandRead(S, ARM64.operands[R]));
    unsigned RegSize = Regs.empty() ? 0 : Regs[0].Size;
    auto addrAt = [&](unsigned Off) -> NdVar {
      if (Off == 0)
        return Base;
      NdVar A = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, A, {Base, NdVar::cst(Off, 8)});
      return A;
    };
    unsigned TotalBytes = 0;
    if (LaneIdx >= 0 && ElemSz > 0) {
      // Indexed single-element form: store lane LaneIdx of each register.
      // Read the whole register (operandRead would already extract the indexed
      // lane, making the SUBBYTES below double-extract / read out of range).
      for (unsigned R = 0; R < NRegs; ++R) {
        NdVar FullReg = operandWrite(ARM64.operands[R]);
        NdVar El = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, El,
               {FullReg, NdVar::cst(LaneIdx * ElemSz, 4)});
        S.emit(NdOp::STORE, {}, {addrAt(R * ElemSz), El});
      }
      TotalBytes = NRegs * ElemSz;
    } else if (Struct == 1 || ElemSz == 0 || RegSize == 0 ||
               RegSize % ElemSz != 0) {
      // Contiguous: store each whole register back-to-back.
      for (unsigned R = 0; R < NRegs; ++R)
        S.emit(NdOp::STORE, {}, {addrAt(R * RegSize), Regs[R]});
      TotalBytes = NRegs * RegSize;
    } else {
      // Interleaved store (ST2/ST3/ST4): out[(L*NRegs + R)] = Regs[R][L].
      unsigned NLanes = RegSize / ElemSz;
      for (unsigned L = 0; L < NLanes; ++L)
        for (unsigned R = 0; R < NRegs; ++R) {
          NdVar El = S.makeTemp(ElemSz);
          S.emit(NdOp::SUBBYTES, El, {Regs[R], NdVar::cst(L * ElemSz, 4)});
          S.emit(NdOp::STORE, {}, {addrAt((L * NRegs + R) * ElemSz), El});
        }
      TotalBytes = NLanes * NRegs * ElemSz;
    }
    (void)Struct;
    if (Insn->detail->writeback &&
        ARM64.operands[MemIdx].mem.base != AARCH64_REG_INVALID) {
      const auto &M = ARM64.operands[MemIdx];
      auto BI = mapCapstoneReg(static_cast<aarch64_reg>(M.mem.base));
      NdVar BaseReg = NdVar::reg(BI.Offset, 8);
      aarch64_reg PostReg = AARCH64_REG_INVALID;
      if (M.mem.index != AARCH64_REG_INVALID)
        PostReg = static_cast<aarch64_reg>(M.mem.index);
      else if (MemIdx + 1 < ARM64.op_count &&
               ARM64.operands[MemIdx + 1].type == AARCH64_OP_REG)
        PostReg = static_cast<aarch64_reg>(ARM64.operands[MemIdx + 1].reg);
      if (PostReg != AARCH64_REG_INVALID) {
        auto PI = mapCapstoneReg(PostReg);
        S.emit(NdOp::INT_ADD, BaseReg, {BaseReg, NdVar::reg(PI.Offset, 8)});
      } else {
        int64_t WB = M.mem.disp != 0 ? M.mem.disp : (int64_t)TotalBytes;
        if (MemIdx + 1 < ARM64.op_count &&
            ARM64.operands[MemIdx + 1].type == AARCH64_OP_IMM)
          WB = ARM64.operands[MemIdx + 1].imm;
        if (WB != 0)
          S.emit(NdOp::INT_ADD, BaseReg,
                 {BaseReg, NdVar::cst(static_cast<uint64_t>(WB), 8)});
      }
    }
    break;
  }
  // NEON misc single-operand
  // REV64 (vector) — reverse the order of elements within each 64-bit group.
  // Element width from the arrangement (.16b/.8h/.4s).  Was an intrinsic
  // intrinsic with no backend handler -> silently returned 0.
  case AARCH64_INS_REV64: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    unsigned ElemSz = neonElemSize(ARM64.operands[0].vas);
    if (ElemSz == 0 || ElemSz >= 8 || Src.Size < 8) {
      S.emit(NdOp::COPY, Dst, {Src});
      break;
    }
    unsigned ElemsPerGroup = 8 / ElemSz;
    unsigned NGroups = Src.Size / 8;
    NdVar Acc = S.makeTemp(0);
    bool First = true;
    for (unsigned G = 0; G < NGroups; ++G) {
      for (unsigned E = 0; E < ElemsPerGroup; ++E) {
        unsigned SrcIdx = G * ElemsPerGroup + (ElemsPerGroup - 1 - E);
        NdVar El = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, El, {Src, NdVar::cst(SrcIdx * ElemSz, 4)});
        if (First) {
          Acc = El;
          First = false;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + ElemSz);
          S.emit(NdOp::CONCAT, Next, {El, Acc});
          Acc = Next;
        }
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case AARCH64_INS_SQXTNB: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emitIntrinsic(Intrinsic::A64_Sqxtn, Dst, {Src});
    break;
  }
  // SUQADD / USQADD — saturating accumulate of an opposite-signedness operand.
  //   SUQADD: signed accumulator Vd + unsigned addend Vn, signed-saturating.
  //   USQADD: unsigned accumulator Vd + signed addend Vn, unsigned-saturating.
  // Vd (operands[0]) is both destination and first source.  Per-lane: extend
  // each operand by its own signedness to a wider lane, add, clamp to the
  // destination signedness range.  Was a full-width INT_ADD placeholder (no
  // saturation, cross-lane carry, wrong signedness).
  case AARCH64_INS_SUQADD:
  case AARCH64_INS_USQADD: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Vd = NdVar::reg(Dst.Offset, Dst.Size);
    NdVar Vn = operandRead(S, ARM64.operands[1]);

    bool DstSigned = (Insn->id == AARCH64_INS_SUQADD);
    unsigned LaneSz = neonElemSize(ARM64.operands[0].vas);
    if (LaneSz == 0)
      LaneSz = Dst.Size; // scalar b/h/s/d form: whole register is one lane
    unsigned NLanes = (LaneSz && Dst.Size >= LaneSz) ? Dst.Size / LaneSz : 1;
    unsigned WideSz = (LaneSz <= 4) ? LaneSz * 2 : 16;

    NdVar Acc = S.makeTemp(0);
    for (unsigned Idx = 0; Idx < NLanes; ++Idx) {
      NdVar La = S.makeTemp(LaneSz);
      NdVar Lb = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {Vd, NdVar::cst(Idx * LaneSz, 4)});
      S.emit(NdOp::SUBBYTES, Lb, {Vn, NdVar::cst(Idx * LaneSz, 4)});
      NdVar Wa = S.makeTemp(WideSz);
      NdVar Wb = S.makeTemp(WideSz);
      S.emit(DstSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Wa, {La});
      S.emit(DstSigned ? NdOp::INT_ZEXT : NdOp::INT_SEXT, Wb, {Lb});
      NdVar Sum = S.makeTemp(WideSz);
      S.emit(NdOp::INT_ADD, Sum, {Wa, Wb});

      // Narrowing saturate via trunc+extend overflow detect (avoids fork's
      // InstCombine mis-fold on INT_SLESS+SELECT clamp chains).
      NdVar Trunc = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, Trunc, {Sum, NdVar::cst(0, 4)});
      NdVar BackWide = S.makeTemp(WideSz);
      S.emit(DstSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, BackWide, {Trunc});
      NdVar Fits = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, Fits, {Sum, BackWide});
      // The scalar form takes LaneSz from the destination register, so a `d`
      // lane puts the sign bit at position 63 — clamp the shift instead of
      // running it off the end of the type.  ~MaxVal is the same bit pattern
      // as -(1 << (bits - 1)) without the signed-overflow.
      uint64_t MaxVal, MinVal;
      if (DstSigned) {
        MaxVal = (LaneSz == 0 || LaneSz >= 8)
                     ? (~0ULL >> 1)
                     : ((1ULL << (LaneSz * 8 - 1)) - 1);
        MinVal = ~MaxVal;
      } else {
        MaxVal = (LaneSz < 8) ? ((1ULL << (LaneSz * 8)) - 1) : ~0ULL;
        MinVal = 0;
      }
      NdVar IsPos = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, IsPos, {NdVar::cst(0, WideSz), Sum});
      NdVar OvfVal = S.makeTemp(LaneSz);
      S.emit(
          NdOp::SELECT, OvfVal,
          {IsPos, NdVar::cst(MaxVal, LaneSz), NdVar::cst(MinVal, LaneSz)});
      NdVar Result = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, Result, {Fits, Trunc, OvfVal});

      if (Idx == 0) {
        Acc = Result;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Result, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // SHLL / SHLL2 — shift left long by element size: each source element is
  // zero-extended to twice its width and shifted left by the source element's
  // bit-width (source bits land in the high half, low half zero).  SHLL2 reads
  // the high half of the source.  Was a full-width INT_ZEXT placeholder (no
  // shift, not per-lane).
  case AARCH64_INS_SHLL:
  case AARCH64_INS_SHLL2: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    unsigned SrcLaneSz = neonElemSize(ARM64.operands[1].vas);
    if (SrcLaneSz == 0 || SrcLaneSz > 4) {
      S.emit(NdOp::INT_ZEXT, Dst, {Src});
      break;
    }
    unsigned DstLaneSz = SrcLaneSz * 2;
    unsigned NLanes = Dst.Size / DstLaneSz;
    unsigned SrcOff = (Insn->id == AARCH64_INS_SHLL2) ? NLanes * SrcLaneSz : 0;
    unsigned ShiftBits = SrcLaneSz * 8;
    NdVar Acc = S.makeTemp(0);
    for (unsigned Idx = 0; Idx < NLanes; ++Idx) {
      NdVar L = S.makeTemp(SrcLaneSz);
      S.emit(NdOp::SUBBYTES, L,
             {Src, NdVar::cst(SrcOff + Idx * SrcLaneSz, 4)});
      NdVar W = S.makeTemp(DstLaneSz);
      S.emit(NdOp::INT_ZEXT, W, {L});
      NdVar Sh = S.makeTemp(DstLaneSz);
      S.emit(NdOp::INT_LEFT, Sh, {W, NdVar::cst(ShiftBits, DstLaneSz)});
      if (Idx == 0) {
        Acc = Sh;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + DstLaneSz);
        S.emit(NdOp::CONCAT, Next, {Sh, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  // ADC / SBC (add/subtract with carry)
  case AARCH64_INS_ADC:
  case AARCH64_INS_ADCS: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar AB = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_ADD, AB, {A, B});
    NdVar CfExt = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(a64reg::CFLAG, 1)});
    bool SetFlags = (Insn->id == AARCH64_INS_ADCS);
    // carry/overflow from the source operands must be computed before the
    // result write, else A/b alias-resolve to the post-write Dst (adcs
    // xD,xD,xM).
    NdVar C1, V1;
    if (SetFlags) {
      C1 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C1, {A, B});
      V1 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V1, {A, B});
    }
    S.emit(NdOp::INT_ADD, Dst, {AB, CfExt});
    if (SetFlags) {
      S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      // C_out = carry(A,b) | carry(a+b, cin)
      NdVar C2 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C2, {AB, CfExt});
      S.emit(NdOp::BOOL_OR, NdVar::reg(a64reg::CFLAG, 1), {C1, C2});
      // V_out = scarry(A,b) ^ scarry(a+b, cin)
      NdVar V2 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V2, {AB, CfExt});
      S.emit(NdOp::BOOL_XOR, NdVar::reg(a64reg::VFLAG, 1), {V1, V2});
    }
    break;
  }
  case AARCH64_INS_SBC:
  case AARCH64_INS_SBCS: {
    // Capstone 6 alias: NGC Rd, Rm → id=SBC, op_count=2 (Src1=XZR implicit)
    // NGC: Dst = NOT(Rm) + C = ~Rm + CF
    if (Insn->is_alias && ARM64.op_count == 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar B = operandRead(S, ARM64.operands[1]);
      uint16_t Sz = Dst.Size;
      NdVar NotB = S.makeTemp(Sz);
      S.emit(NdOp::INT_NOT, NotB, {B});
      NdVar CfExt = S.makeTemp(Sz);
      S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(a64reg::CFLAG, 1)});
      S.emit(NdOp::INT_ADD, Dst, {NotB, CfExt});
      if (Insn->id == AARCH64_INS_SBCS) {
        // NGCS sets NZCV.  First operand is XZR=0:
        // carry(0,~Rm)=scarry(0,~Rm)=0, so the ~Rm + C addition alone
        // determines C/V.
        S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
               {Dst, NdVar::cst(0, Sz)});
        S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
               {Dst, NdVar::cst(0, Sz)});
        S.emit(NdOp::INT_CARRY, NdVar::reg(a64reg::CFLAG, 1), {NotB, CfExt});
        S.emit(NdOp::INT_SOVF, NdVar::reg(a64reg::VFLAG, 1), {NotB, CfExt});
      }
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar NotB = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NotB, {B});
    NdVar AB = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_ADD, AB, {A, NotB});
    NdVar CfExt = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(a64reg::CFLAG, 1)});
    bool SetFlags = (Insn->id == AARCH64_INS_SBCS);
    // carry/overflow from the source operands must be computed before the
    // result write, else A alias-resolves to the post-write Dst (sbcs
    // xD,xD,xM).
    NdVar C1, V1;
    if (SetFlags) {
      C1 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C1, {A, NotB});
      V1 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V1, {A, NotB});
    }
    S.emit(NdOp::INT_ADD, Dst, {AB, CfExt});
    if (SetFlags) {
      S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      // C_out = carry(A, ~b) | carry(a+~b, cin)
      NdVar C2 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C2, {AB, CfExt});
      S.emit(NdOp::BOOL_OR, NdVar::reg(a64reg::CFLAG, 1), {C1, C2});
      // V_out = scarry(A, ~b) ^ scarry(a+~b, cin)
      NdVar V2 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V2, {AB, CfExt});
      S.emit(NdOp::BOOL_XOR, NdVar::reg(a64reg::VFLAG, 1), {V1, V2});
    }
    break;
  }
  // SMSUBL / UMSUBL
  // Capstone 6 alias: SMNEGL/UMNEGL → id=SMSUBL/UMSUBL, op_count=3
  case AARCH64_INS_SMSUBL:
  case AARCH64_INS_UMSUBL: {
    if (Insn->is_alias && ARM64.op_count == 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar N = operandRead(S, ARM64.operands[1]);
      NdVar M = operandRead(S, ARM64.operands[2]);
      NdVar ExtN = S.makeTemp(8), ExtM = S.makeTemp(8);
      if (Insn->id == AARCH64_INS_SMSUBL) {
        S.emit(NdOp::INT_SEXT, ExtN, {N});
        S.emit(NdOp::INT_SEXT, ExtM, {M});
      } else {
        S.emit(NdOp::INT_ZEXT, ExtN, {N});
        S.emit(NdOp::INT_ZEXT, ExtM, {M});
      }
      NdVar Prod = S.makeTemp(8);
      S.emit(NdOp::INT_MULT, Prod, {ExtN, ExtM});
      S.emit(NdOp::INT_NEG2, Dst, {Prod});
      break;
    }
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar N = operandRead(S, ARM64.operands[1]);
    NdVar M = operandRead(S, ARM64.operands[2]);
    NdVar A = operandRead(S, ARM64.operands[3]);
    NdVar ExtN = S.makeTemp(8);
    NdVar ExtM = S.makeTemp(8);
    if (Insn->id == AARCH64_INS_SMSUBL) {
      S.emit(NdOp::INT_SEXT, ExtN, {N});
      S.emit(NdOp::INT_SEXT, ExtM, {M});
    } else {
      S.emit(NdOp::INT_ZEXT, ExtN, {N});
      S.emit(NdOp::INT_ZEXT, ExtM, {M});
    }
    NdVar Prod = S.makeTemp(8);
    S.emit(NdOp::INT_MULT, Prod, {ExtN, ExtM});
    S.emit(NdOp::INT_SUB, Dst, {A, Prod});
    break;
  }
  // MNEG → MSUB Xd, Xn, Xm, XZR (handled by MSUB case above)
  // LDNP / STNP (non-temporal pair)
  case AARCH64_INS_LDNP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst1 = operandWrite(ARM64.operands[0]);
    NdVar Dst2 = operandWrite(ARM64.operands[1]);
    // Use the effective address (base+disp); operandRead would dereference the
    // `[Xn, #imm]` memory operand and use the loaded value as the address.
    NdVar EA = operandEffAddr(S, ARM64.operands[2]);
    S.emit(NdOp::LOAD, Dst1, {EA});
    NdVar EA2 = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(Dst1.Size, 8)});
    S.emit(NdOp::LOAD, Dst2, {EA2});
    break;
  }
  case AARCH64_INS_STNP: {
    if (ARM64.op_count < 3)
      break;
    NdVar Src1 = operandRead(S, ARM64.operands[0]);
    NdVar Src2 = operandRead(S, ARM64.operands[1]);
    NdVar EA = operandEffAddr(S, ARM64.operands[2]);
    S.emit(NdOp::STORE, {}, {EA, Src1});
    NdVar EA2 = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, EA2, {EA, NdVar::cst(Src1.Size, 8)});
    S.emit(NdOp::STORE, {}, {EA2, Src2});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
