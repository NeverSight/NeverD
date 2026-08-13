//===- AArch64LiftBitManip.cpp - Multiply-accumulate and reversal ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MADD/MSUB, CLZ, and the reversal family REV/REV16/REV32/RBIT.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftBitManip(AArch64Lifter &L, AArch64Lifter::LiftState &S,
                  const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // --- MADD / MSUB ---
  // Capstone 6 decodes `MUL Rd, Rn, Rm` as MADD with op_count=3 (alias).
  // Canonical MADD has 4 operands: Rd, Rn, Rm, Ra (Rd = Ra + Rn*Rm).
  case AARCH64_INS_MADD: {
    if (ARM64.op_count >= 4) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar N =
          L.narrowToWidth(S, L.operandRead(S, ARM64.operands[1]), Dst.Size);
      NdVar M =
          L.narrowToWidth(S, L.operandRead(S, ARM64.operands[2]), Dst.Size);
      NdVar A =
          L.narrowToWidth(S, L.operandRead(S, ARM64.operands[3]), Dst.Size);
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {N, M});
      S.emit(NdOp::INT_ADD, Dst, {A, Prod});
    } else if (ARM64.op_count == 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar N = L.operandRead(S, ARM64.operands[1]);
      NdVar M = L.operandRead(S, ARM64.operands[2]);
      S.emit(NdOp::INT_MULT, Dst, {N, M});
    }
    break;
  }
  case AARCH64_INS_MSUB: {
    if (ARM64.op_count >= 4) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar N =
          L.narrowToWidth(S, L.operandRead(S, ARM64.operands[1]), Dst.Size);
      NdVar M =
          L.narrowToWidth(S, L.operandRead(S, ARM64.operands[2]), Dst.Size);
      NdVar A =
          L.narrowToWidth(S, L.operandRead(S, ARM64.operands[3]), Dst.Size);
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {N, M});
      S.emit(NdOp::INT_SUB, Dst, {A, Prod});
    } else if (ARM64.op_count == 3) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar N = L.operandRead(S, ARM64.operands[1]);
      NdVar M = L.operandRead(S, ARM64.operands[2]);
      S.emit(NdOp::INT_MULT, Dst, {N, M});
      S.emit(NdOp::INT_NEG2, Dst, {Dst});
    }
    break;
  }

  // --- CLZ (count leading zeros) ---
  case AARCH64_INS_CLZ: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    // Vector `clz v.<T>` counts leading zeros PER LANE; a single LZCOUNT on the
    // whole i128 counts zeros of the entire register and collapses the
    // reduction.
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
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar La = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(NdOp::LZCOUNT, Lr, {La});
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
      S.emit(NdOp::LZCOUNT, Dst, {Src});
    }
    break;
  }

  // --- REV (full Byte reverse) ---
  case AARCH64_INS_REV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    uint16_t Sz = Dst.Size;
    if (Sz == 4) {
      NdVar B0 = S.makeTemp(4);
      NdVar B1 = S.makeTemp(4);
      NdVar B2 = S.makeTemp(4);
      NdVar B3 = S.makeTemp(4);
      S.emit(NdOp::INT_RIGHT, B0, {Src, NdVar::cst(24, 4)});
      NdVar T1 = S.makeTemp(4);
      S.emit(NdOp::INT_RIGHT, T1, {Src, NdVar::cst(8, 4)});
      S.emit(NdOp::INT_AND, B1, {T1, NdVar::cst(0xFF00, 4)});
      NdVar T2 = S.makeTemp(4);
      S.emit(NdOp::INT_LEFT, T2, {Src, NdVar::cst(8, 4)});
      S.emit(NdOp::INT_AND, B2, {T2, NdVar::cst(0xFF0000, 4)});
      S.emit(NdOp::INT_LEFT, B3, {Src, NdVar::cst(24, 4)});
      NdVar R01 = S.makeTemp(4);
      NdVar R23 = S.makeTemp(4);
      S.emit(NdOp::INT_OR, R01, {B0, B1});
      S.emit(NdOp::INT_OR, R23, {B2, B3});
      S.emit(NdOp::INT_OR, Dst, {R01, R23});
    } else {
      // 64-bit full bswap via shift-and-Mask
      NdVar B0 = S.makeTemp(8);
      NdVar B1 = S.makeTemp(8);
      NdVar B2 = S.makeTemp(8);
      NdVar B3 = S.makeTemp(8);
      NdVar B4 = S.makeTemp(8);
      NdVar B5 = S.makeTemp(8);
      NdVar B6 = S.makeTemp(8);
      NdVar B7 = S.makeTemp(8);
      S.emit(NdOp::INT_RIGHT, B0, {Src, NdVar::cst(56, 8)});
      NdVar T1 = S.makeTemp(8);
      S.emit(NdOp::INT_RIGHT, T1, {Src, NdVar::cst(40, 8)});
      S.emit(NdOp::INT_AND, B1, {T1, NdVar::cst(0xFF00ULL, 8)});
      NdVar T2 = S.makeTemp(8);
      S.emit(NdOp::INT_RIGHT, T2, {Src, NdVar::cst(24, 8)});
      S.emit(NdOp::INT_AND, B2, {T2, NdVar::cst(0xFF0000ULL, 8)});
      NdVar T3 = S.makeTemp(8);
      S.emit(NdOp::INT_RIGHT, T3, {Src, NdVar::cst(8, 8)});
      S.emit(NdOp::INT_AND, B3, {T3, NdVar::cst(0xFF000000ULL, 8)});
      NdVar T4 = S.makeTemp(8);
      S.emit(NdOp::INT_LEFT, T4, {Src, NdVar::cst(8, 8)});
      S.emit(NdOp::INT_AND, B4, {T4, NdVar::cst(0xFF00000000ULL, 8)});
      NdVar T5 = S.makeTemp(8);
      S.emit(NdOp::INT_LEFT, T5, {Src, NdVar::cst(24, 8)});
      S.emit(NdOp::INT_AND, B5, {T5, NdVar::cst(0xFF0000000000ULL, 8)});
      NdVar T6 = S.makeTemp(8);
      S.emit(NdOp::INT_LEFT, T6, {Src, NdVar::cst(40, 8)});
      S.emit(NdOp::INT_AND, B6, {T6, NdVar::cst(0xFF000000000000ULL, 8)});
      S.emit(NdOp::INT_LEFT, B7, {Src, NdVar::cst(56, 8)});
      NdVar R01 = S.makeTemp(8);
      NdVar R23 = S.makeTemp(8);
      NdVar R45 = S.makeTemp(8);
      NdVar R67 = S.makeTemp(8);
      S.emit(NdOp::INT_OR, R01, {B0, B1});
      S.emit(NdOp::INT_OR, R23, {B2, B3});
      S.emit(NdOp::INT_OR, R45, {B4, B5});
      S.emit(NdOp::INT_OR, R67, {B6, B7});
      NdVar R03 = S.makeTemp(8);
      NdVar R47 = S.makeTemp(8);
      S.emit(NdOp::INT_OR, R03, {R01, R23});
      S.emit(NdOp::INT_OR, R47, {R45, R67});
      S.emit(NdOp::INT_OR, Dst, {R03, R47});
    }
    break;
  }
  case AARCH64_INS_REV16: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    // Swap the two bytes within each 16-bit halfword across the full operand
    // width.  The old mask path stored masks in a uint64_t, so the vector form
    // (rev16 v.16b, 16 bytes) zero-extended the mask and cleared the high 8
    // bytes.  Per-halfword reconstruction is correct for Xn/Dn/Qn alike.
    unsigned NHalf = Dst.Size / 2;
    if (NHalf == 0) {
      S.emit(NdOp::COPY, Dst, {Src});
      break;
    }
    NdVar Acc = S.makeTemp(0);
    for (unsigned H = 0; H < NHalf; ++H) {
      NdVar B0 = S.makeTemp(1), B1 = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, B0, {Src, NdVar::cst(H * 2 + 0, 4)});
      S.emit(NdOp::SUBBYTES, B1, {Src, NdVar::cst(H * 2 + 1, 4)});
      NdVar RevH = S.makeTemp(2);
      S.emit(NdOp::CONCAT, RevH, {B0, B1});
      if (H == 0) {
        Acc = RevH;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 2);
        S.emit(NdOp::CONCAT, Next, {RevH, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case AARCH64_INS_REV32: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    // Byte-reverse within each 32-bit word across the *full* operand width.
    // Handles GP Xn (8 bytes = 2 words) and NEON Dn/Qn (8/16 bytes); the old
    // code only reversed the low 8 bytes, leaving the high lanes of a Q
    // register (rev32 v.16b) untouched.
    unsigned NWords = Dst.Size / 4;
    if (NWords == 0) {
      S.emit(NdOp::COPY, Dst, {Src});
      break;
    }
    NdVar Acc = S.makeTemp(0);
    for (unsigned W = 0; W < NWords; ++W) {
      NdVar B0 = S.makeTemp(1), B1 = S.makeTemp(1);
      NdVar B2 = S.makeTemp(1), B3 = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, B0, {Src, NdVar::cst(W * 4 + 0, 4)});
      S.emit(NdOp::SUBBYTES, B1, {Src, NdVar::cst(W * 4 + 1, 4)});
      S.emit(NdOp::SUBBYTES, B2, {Src, NdVar::cst(W * 4 + 2, 4)});
      S.emit(NdOp::SUBBYTES, B3, {Src, NdVar::cst(W * 4 + 3, 4)});
      // Reversed word low→high = [B3, B2, B1, B0].
      NdVar P0 = S.makeTemp(2);
      S.emit(NdOp::CONCAT, P0, {B2, B3});
      NdVar P1 = S.makeTemp(3);
      S.emit(NdOp::CONCAT, P1, {B1, P0});
      NdVar RevW = S.makeTemp(4);
      S.emit(NdOp::CONCAT, RevW, {B0, P1});
      if (W == 0) {
        Acc = RevW;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 4);
        S.emit(NdOp::CONCAT, Next, {RevW, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case AARCH64_INS_RBIT: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar Src = L.operandRead(S, ARM64.operands[1]);
    // Vector `rbit v.8b/.16b` reverses the bits within each byte
    // independently; only the scalar Wn/Xn form reverses the whole register.
    if (ARM64.operands[0].vas != AARCH64LAYOUT_INVALID) {
      NdVar Acc = S.makeTemp(0);
      for (unsigned B = 0; B < Dst.Size; ++B) {
        NdVar Byte = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, Byte, {Src, NdVar::cst(B, 4)});
        NdVar Rev = S.makeTemp(1);
        S.emitIntrinsic(Intrinsic::A64_Rbit, Rev, {Byte});
        if (B == 0) {
          Acc = Rev;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + 1);
          S.emit(NdOp::CONCAT, Next, {Rev, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
      break;
    }
    S.emitIntrinsic(Intrinsic::A64_Rbit, Dst, {Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
