//===- AArch64LiftArith.cpp - AArch64 integer add/sub and logic -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ADD/ADDS/SUB/SUBS (including the CMP/CMN aliases) with full
/// NZCV computation, and the bitwise AND/ANDS/ORR/EOR forms
/// including the SIMD logical-immediate encodings.
///
//===----------------------------------------------------------------------===//

#include "AArch64LiftDetail.h"

#include "neverd/lift/AArch64Lifter.h"

namespace neverd {

bool liftArith(AArch64Lifter &L, AArch64Lifter::LiftState &S,
               const cs_insn *Insn, const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  // --- ADD / SUB ---
  case AARCH64_INS_ADD:
  case AARCH64_INS_ADDS: {
    // Capstone 6 alias: CMN Rn, Rm → id=ADDS, op_count=2 (dest=XZR implicit)
    if (Insn->is_alias && ARM64.op_count == 2 && Insn->id == AARCH64_INS_ADDS) {
      NdVar A = L.operandRead(S, ARM64.operands[0]);
      NdVar B = L.operandRead(S, ARM64.operands[1]);
      uint16_t Sz = A.Size;
      B = L.narrowToWidth(S, B, Sz);
      NdVar Result = S.makeTemp(Sz);
      S.emit(NdOp::INT_ADD, Result, {A, B});
      S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
             {Result, NdVar::cst(0, Sz)});
      S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
             {Result, NdVar::cst(0, Sz)});
      S.emit(NdOp::INT_CARRY, NdVar::reg(a64reg::CFLAG, 1), {A, B});
      S.emit(NdOp::INT_SOVF, NdVar::reg(a64reg::VFLAG, 1), {A, B});
      break;
    }
    // Capstone 6 alias: MOV Xd, Xn → id=ADD, op_count=2 (ADD Xd, Xn, #0)
    if (Insn->is_alias && ARM64.op_count == 2 && Insn->id == AARCH64_INS_ADD) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
      // W/X view sync is handled by the lift() zero-extension post-pass and
      // the table-driven sub-register fixup; an explicit narrow `COPY Wd, Ws`
      // here is redundant and clobbers Xd when Ws is later rewritten (#157g).
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    B = L.narrowToWidth(S, B, Dst.Size);
    // Snapshot the source operands BEFORE the INT_ADD writes Dst: ADDS often
    // has dst==src (`adds w8,w8,#imm`), and the C/V flags must use the
    // *pre-add* operands.  Without the snapshot, SSA resolves the flag's read
    // of `A` to the post-add result, so CF/VF (and any `cs/hs/hi/...` consumer)
    // are wrong.
    NdVar FlagA = S.makeTemp(A.Size);
    S.emit(NdOp::COPY, FlagA, {A});
    NdVar FlagB = S.makeTemp(B.Size);
    S.emit(NdOp::COPY, FlagB, {B});
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
          NdVar Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_ADD, Lr, {La, Lb});
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
        S.emit(NdOp::INT_ADD, Dst, {A, B});
      }
    }
    if (Insn->id == AARCH64_INS_ADDS) {
      S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::INT_CARRY, NdVar::reg(a64reg::CFLAG, 1), {FlagA, FlagB});
      S.emit(NdOp::INT_SOVF, NdVar::reg(a64reg::VFLAG, 1), {FlagA, FlagB});
    }
    break;
  }
  case AARCH64_INS_SUB:
  case AARCH64_INS_SUBS: {
    // Capstone 6 aliases:
    //   NEG Rd, Rm → id=SUB, op_count=2 (Src1=XZR implicit)
    //   CMP Rn, Rm → id=SUBS, op_count=2 (dest=XZR implicit)
    //   NEGS Rd, Rm → id=SUBS, op_count=2
    if (Insn->is_alias && ARM64.op_count == 2) {
      const char *Mn = Insn->mnemonic;
      if ((Mn[0] == 'c' || Mn[0] == 'C') && (Mn[1] == 'm' || Mn[1] == 'M') &&
          (Mn[2] == 'p' || Mn[2] == 'P')) {
        NdVar A = L.operandRead(S, ARM64.operands[0]);
        NdVar B = L.operandRead(S, ARM64.operands[1]);
        uint16_t Sz = A.Size;
        B = L.narrowToWidth(S, B, Sz);
        NdVar Result = S.makeTemp(Sz);
        S.emit(NdOp::INT_SUB, Result, {A, B});
        S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
               {Result, NdVar::cst(0, Sz)});
        S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
               {Result, NdVar::cst(0, Sz)});
        NdVar Borrow = S.makeTemp(1);
        S.emit(NdOp::INT_LESS, Borrow, {A, B});
        S.emit(NdOp::BOOL_NOT, NdVar::reg(a64reg::CFLAG, 1), {Borrow});
        S.emit(NdOp::INT_SBOR, NdVar::reg(a64reg::VFLAG, 1), {A, B});
      } else {
        NdVar Dst = L.operandWrite(ARM64.operands[0]);
        NdVar Src = L.operandRead(S, ARM64.operands[1]);
        uint16_t Sz = Dst.Size;
        S.emit(NdOp::INT_NEG2, Dst, {Src});
        if (Insn->id == AARCH64_INS_SUBS) {
          NdVar Zero = NdVar::cst(0, Sz);
          S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
                 {Dst, NdVar::cst(0, Sz)});
          S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
                 {Dst, NdVar::cst(0, Sz)});
          NdVar Borrow = S.makeTemp(1);
          S.emit(NdOp::INT_LESS, Borrow, {Zero, Src});
          S.emit(NdOp::BOOL_NOT, NdVar::reg(a64reg::CFLAG, 1), {Borrow});
          S.emit(NdOp::INT_SBOR, NdVar::reg(a64reg::VFLAG, 1), {Zero, Src});
        }
      }
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    B = L.narrowToWidth(S, B, Dst.Size);
    // Snapshot operands BEFORE the INT_SUB writes Dst (SUBS often has dst==src,
    // e.g. `subs w8,w8,#imm`): the C/V flags must compare the *pre-sub* values.
    // Otherwise SSA folds the borrow's read of `A` to the subtraction result,
    // making `cs/hs/hi/ls/...` consumers (e.g. unsigned-saturation idioms)
    // wrong.
    NdVar FlagA = S.makeTemp(A.Size);
    S.emit(NdOp::COPY, FlagA, {A});
    NdVar FlagB = S.makeTemp(B.Size);
    S.emit(NdOp::COPY, FlagB, {B});
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
          NdVar Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_SUB, Lr, {La, Lb});
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
        S.emit(NdOp::INT_SUB, Dst, {A, B});
      }
    }
    if (Insn->id == AARCH64_INS_SUBS) {
      S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      NdVar Borrow = S.makeTemp(1);
      S.emit(NdOp::INT_LESS, Borrow, {FlagA, FlagB});
      S.emit(NdOp::BOOL_NOT, NdVar::reg(a64reg::CFLAG, 1), {Borrow});
      S.emit(NdOp::INT_SBOR, NdVar::reg(a64reg::VFLAG, 1), {FlagA, FlagB});
    }
    break;
  }

  // CMP → SUBS XZR (handled by SUBS case above)
  // CMN → ADDS XZR (handled by ADDS case above)

  // --- AND / ORR / EOR ---
  case AARCH64_INS_AND:
  case AARCH64_INS_ANDS: {
    // Capstone 6 alias: TST Rn, Rm → id=ANDS, op_count=2 (dest=XZR implicit)
    if (Insn->is_alias && ARM64.op_count == 2 && Insn->id == AARCH64_INS_ANDS) {
      NdVar A = L.operandRead(S, ARM64.operands[0]);
      NdVar B = L.operandRead(S, ARM64.operands[1]);
      uint16_t Sz = A.Size;
      NdVar Result = S.makeTemp(Sz);
      S.emit(NdOp::INT_AND, Result, {A, B});
      S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
             {Result, NdVar::cst(0, Sz)});
      S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
             {Result, NdVar::cst(0, Sz)});
      S.emit(NdOp::COPY, NdVar::reg(a64reg::CFLAG, 1), {NdVar::cst(0, 1)});
      S.emit(NdOp::COPY, NdVar::reg(a64reg::VFLAG, 1), {NdVar::cst(0, 1)});
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_AND, Dst, {A, B});
    if (Insn->id == AARCH64_INS_ANDS) {
      S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::COPY, NdVar::reg(a64reg::CFLAG, 1), {NdVar::cst(0, 1)});
      S.emit(NdOp::COPY, NdVar::reg(a64reg::VFLAG, 1), {NdVar::cst(0, 1)});
    }
    break;
  }
  case AARCH64_INS_ORR: {
    // Capstone 6 alias: MOV Rd, Rn → id=ORR, op_count=2 (Src1=XZR implicit)
    if (Insn->is_alias && ARM64.op_count == 2) {
      NdVar Dst = L.operandWrite(ARM64.operands[0]);
      NdVar Src = L.operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
      // W/X view sync is handled by the lift() zero-extension post-pass and
      // the table-driven sub-register fixup; an explicit narrow `COPY Wd, Ws`
      // here is redundant and clobbers Xd when Ws is later rewritten (#157g).
      break;
    }
    // SIMD ORR (vector, immediate): `orr vD.<T>, #imm{, lsl #s}` reads AND
    // writes vD (vD |= broadcast(imm)).  Capstone surfaces only 2 operands and
    // is_alias is false, so this used to fall into `op_count < 3` and become a
    // silent no-op — dropping clang's per-lane OR-immediate (e.g. in a matmul).
    if (ARM64.op_count == 2 && ARM64.operands[0].type == AARCH64_OP_REG &&
        ARM64.operands[0].vas != AARCH64LAYOUT_INVALID &&
        ARM64.operands[1].type == AARCH64_OP_IMM) {
      emitSimdImmLogic(S, ARM64, /*IsBic=*/false);
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_OR, Dst, {A, B});
    break;
  }
  case AARCH64_INS_EOR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(ARM64.operands[0]);
    NdVar A = L.operandRead(S, ARM64.operands[1]);
    NdVar B = L.operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
