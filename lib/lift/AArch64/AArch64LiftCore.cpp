//===- AArch64LiftCore.cpp - AArch64 core instruction lifter ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Core AArch64 instruction handlers: integer ALU, control flow, load/store,
/// barriers, system registers, pointer authentication, etc.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/AArch64Lifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-aarch64"

namespace neverd {

namespace {
// SIMD logical-immediate (`orr vD.<T>, #imm{,lsl #s}` / `bic vD.<T>, #imm`):
// broadcast the modified immediate across every lane and OR / AND-NOT it into
// the (read-modified) destination register.  Capstone surfaces these as a
// 2-operand non-alias ORR/BIC, which previously fell through the register-form
// `op_count < 3` guard and silently became a no-op.
void emitSimdImmLogic(AArch64Lifter::LiftState &S, const cs_aarch64 &ARM64,
                      bool IsBic) {
  NdVar Dst = AArch64Lifter::operandWrite(ARM64.operands[0]);
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
    ElemSz = 8;
    break;
  default:
    break;
  }
  if (ElemSz == 0 || Dst.Size == 0 || Dst.Size < ElemSz) {
    // Unknown layout: keep the value rather than corrupt it.
    S.emit(NdOp::COPY, Dst, {NdVar::reg(Dst.Offset, Dst.Size)});
    return;
  }
  uint64_t Imm = static_cast<uint64_t>(ARM64.operands[1].imm);
  if (ARM64.operands[1].shift.type == AARCH64_SFT_LSL)
    Imm <<= ARM64.operands[1].shift.value;
  unsigned NLanes = Dst.Size / ElemSz;
  NdVar Elem = NdVar::cst(Imm, ElemSz);
  NdVar Acc = Elem;
  for (unsigned I = 1; I < NLanes; ++I) {
    NdVar Next = S.makeTemp(Acc.Size + ElemSz);
    S.emit(NdOp::CONCAT, Next, {Elem, Acc});
    Acc = Next;
  }
  NdVar Bcast = S.makeTemp(Dst.Size);
  S.emit(NdOp::COPY, Bcast, {Acc});
  NdVar Cur = NdVar::reg(Dst.Offset, Dst.Size);
  if (IsBic) {
    NdVar NotB = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NotB, {Bcast});
    S.emit(NdOp::INT_AND, Dst, {Cur, NotB});
  } else {
    S.emit(NdOp::INT_OR, Dst, {Cur, Bcast});
  }
}
} // namespace

bool AArch64Lifter::liftCore(LiftState &S, const cs_insn *Insn,
                             const cs_aarch64 &ARM64) {
  switch (Insn->id) {
  case AARCH64_INS_HINT:
    if (Insn->is_alias) {
      switch (Insn->alias_id) {
      case AARCH64_INS_ALIAS_YIELD:
        S.emitVoidIntrinsic(Intrinsic::Yield_A64);
        break;
      case AARCH64_INS_ALIAS_WFE:
        S.emitVoidIntrinsic(Intrinsic::Wfe);
        break;
      case AARCH64_INS_ALIAS_WFI:
        S.emitVoidIntrinsic(Intrinsic::Wfi);
        break;
      case AARCH64_INS_ALIAS_SEV:
        S.emitVoidIntrinsic(Intrinsic::Sev);
        break;
      case AARCH64_INS_ALIAS_SEVL:
        S.emitVoidIntrinsic(Intrinsic::Sevl);
        break;
      default:
        S.emit(NdOp::NOP, {}, {});
        break;
      }
    } else {
      S.emit(NdOp::NOP, {}, {});
    }
    break;

  // --- MOV / MVN ---
  case AARCH64_INS_MOV: {
    if (ARM64.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM64.operands[1]);
    NdVar Dst = operandWrite(ARM64.operands[0]);
    S.emit(NdOp::COPY, Dst, {Src});
    // W/X view synchronization is handled uniformly by the W→X zero-extension
    // post-pass in lift() plus the table-driven sub-register fixup.  Emitting
    // an extra `COPY Wd, Ws` here for a 64-bit `mov Xd, Xs` is redundant (the
    // 64-bit COPY already defines the Wd view) and actively harmful: it reads
    // Ws independently, so a later write to Ws gets the wrong SSA version and
    // can clobber Xd (bug #157g, e.g. `mov x8,x0; mov w0,#1`).
    break;
  }
  case AARCH64_INS_ORN: {
    if (ARM64.op_count < 2)
      break;
    NdVar Src = operandRead(S, ARM64.operands[ARM64.op_count - 1]);
    NdVar Dst = operandWrite(ARM64.operands[0]);
    if (ARM64.op_count == 2) {
      S.emit(NdOp::INT_NOT, Dst, {Src});
    } else {
      NdVar A = operandRead(S, ARM64.operands[1]);
      NdVar Inv = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_NOT, Inv, {Src});
      S.emit(NdOp::INT_OR, Dst, {A, Inv});
    }
    break;
  }
  case AARCH64_INS_MOVZ: {
    // MOVZ: Dst = imm16 << shift (zeroing other Bits)
    // Capstone 6 alias: may report MOV with final value directly.
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    uint16_t Sz = Dst.Size;
    uint64_t Val;
    if (Insn->is_alias) {
      Val = static_cast<uint64_t>(ARM64.operands[1].imm);
    } else {
      uint64_t Imm16 = static_cast<uint64_t>(ARM64.operands[1].imm) & 0xFFFF;
      uint32_t Shift = 0;
      if (ARM64.operands[1].shift.type == AARCH64_SFT_LSL)
        Shift = ARM64.operands[1].shift.value;
      Val = Imm16 << Shift;
    }
    S.emit(NdOp::COPY, Dst, {NdVar::cst(Val, Sz)});
    break;
  }
  case AARCH64_INS_MOVN: {
    // MOVN: Dst = ~(Imm16 << Shift)
    // Capstone 6 alias: reports MOV with final value as Imm.
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    uint16_t Sz = Dst.Size;
    uint64_t Val;
    if (Insn->is_alias) {
      Val = static_cast<uint64_t>(ARM64.operands[1].imm);
    } else {
      uint64_t Imm16 = static_cast<uint64_t>(ARM64.operands[1].imm) & 0xFFFF;
      uint32_t Shift = 0;
      if (ARM64.operands[1].shift.type == AARCH64_SFT_LSL)
        Shift = ARM64.operands[1].shift.value;
      Val = ~(Imm16 << Shift);
    }
    if (Sz == 4)
      Val &= 0xFFFFFFFF;
    S.emit(NdOp::COPY, Dst, {NdVar::cst(Val, Sz)});
    break;
  }
  case AARCH64_INS_MOVK: {
    // MOVK: insert 16 bits at Shift position, keep other bits
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    uint16_t Sz = Dst.Size;
    uint64_t Imm16 = static_cast<uint64_t>(ARM64.operands[1].imm) & 0xFFFF;
    uint32_t Shift = 0;
    if (ARM64.operands[1].shift.type == AARCH64_SFT_LSL)
      Shift = ARM64.operands[1].shift.value;
    uint64_t Mask = ~(static_cast<uint64_t>(0xFFFF) << Shift);
    if (Sz == 4)
      Mask &= 0xFFFFFFFF;
    NdVar Cleared = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cleared,
           {NdVar::reg(Dst.Offset, Sz), NdVar::cst(Mask, Sz)});
    uint64_t Inserted = Imm16 << Shift;
    if (Sz == 4)
      Inserted &= 0xFFFFFFFF;
    S.emit(NdOp::INT_OR, Dst, {Cleared, NdVar::cst(Inserted, Sz)});
    break;
  }

  // --- ADD / SUB ---
  case AARCH64_INS_ADD:
  case AARCH64_INS_ADDS: {
    // Capstone 6 alias: CMN Rn, Rm → id=ADDS, op_count=2 (dest=XZR implicit)
    if (Insn->is_alias && ARM64.op_count == 2 && Insn->id == AARCH64_INS_ADDS) {
      NdVar A = operandRead(S, ARM64.operands[0]);
      NdVar B = operandRead(S, ARM64.operands[1]);
      uint16_t Sz = A.Size;
      B = narrowToWidth(S, B, Sz);
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
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[1]);
      S.emit(NdOp::COPY, Dst, {Src});
      // W/X view sync is handled by the lift() zero-extension post-pass and
      // the table-driven sub-register fixup; an explicit narrow `COPY Wd, Ws`
      // here is redundant and clobbers Xd when Ws is later rewritten (#157g).
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    B = narrowToWidth(S, B, Dst.Size);
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
        NdVar A = operandRead(S, ARM64.operands[0]);
        NdVar B = operandRead(S, ARM64.operands[1]);
        uint16_t Sz = A.Size;
        B = narrowToWidth(S, B, Sz);
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
        NdVar Dst = operandWrite(ARM64.operands[0]);
        NdVar Src = operandRead(S, ARM64.operands[1]);
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
          S.emit(NdOp::INT_SBOR, NdVar::reg(a64reg::VFLAG, 1),
                 {Zero, Src});
        }
      }
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    B = narrowToWidth(S, B, Dst.Size);
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
      NdVar A = operandRead(S, ARM64.operands[0]);
      NdVar B = operandRead(S, ARM64.operands[1]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
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
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[1]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_OR, Dst, {A, B});
    break;
  }
  case AARCH64_INS_EOR: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_XOR, Dst, {A, B});
    break;
  }

  // --- LSL / LSR / ASR ---
  // Capstone 6 alias: LSLV/LSRV/ASRV Xd,Xn,Xm may be reported as
  // LSL/LSR/ASR with only 2 operands (Rd,Rn) — Rm is dropped.
  // Extract Rn (bits [9:5]) and Rm (bits [20:16]) from the encoding.
  case AARCH64_INS_LSL: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A, B;
    bool IsRegShift = false;
    if (ARM64.op_count >= 3) {
      A = operandRead(S, ARM64.operands[1]);
      B = operandRead(S, ARM64.operands[2]);
      IsRegShift = ARM64.operands[2].type == AARCH64_OP_REG;
    } else if (ARM64.op_count == 2 &&
               ARM64.operands[0].type == AARCH64_OP_REG &&
               ARM64.operands[1].type == AARCH64_OP_REG && Insn->size == 4) {
      uint32_t Enc;
      std::memcpy(&Enc, Insn->bytes, 4);
      bool Is64 = (Enc >> 31) & 1;
      unsigned RnIdx = (Enc >> 5) & 0x1F;
      unsigned RmIdx = (Enc >> 16) & 0x1F;
      auto RnReg = static_cast<aarch64_reg>(Is64 ? AARCH64_REG_X0 + RnIdx
                                                 : AARCH64_REG_W0 + RnIdx);
      auto RmReg = static_cast<aarch64_reg>(Is64 ? AARCH64_REG_X0 + RmIdx
                                                 : AARCH64_REG_W0 + RmIdx);
      auto RnRI = mapCapstoneReg(RnReg);
      auto RmRI = mapCapstoneReg(RmReg);
      A = (RnRI.Size > 0) ? NdVar::reg(RnRI.Offset, RnRI.Size)
                          : operandRead(S, ARM64.operands[0]);
      B = (RmRI.Size > 0) ? NdVar::reg(RmRI.Offset, RmRI.Size)
                          : NdVar::cst(0, Dst.Size);
      IsRegShift = true;
    } else {
      A = operandRead(S, ARM64.operands[0]);
      B = operandRead(S, ARM64.operands[1]);
    }
    if (IsRegShift) {
      uint64_t Mask = (Dst.Size == 8) ? 63 : 31;
      NdVar Masked = S.makeTemp(B.Size);
      S.emit(NdOp::INT_AND, Masked, {B, NdVar::cst(Mask, B.Size)});
      S.emit(NdOp::INT_LEFT, Dst, {A, Masked});
    } else {
      S.emit(NdOp::INT_LEFT, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_LSR: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A, B;
    bool IsRegShift = false;
    if (ARM64.op_count >= 3) {
      A = operandRead(S, ARM64.operands[1]);
      B = operandRead(S, ARM64.operands[2]);
      IsRegShift = ARM64.operands[2].type == AARCH64_OP_REG;
    } else if (ARM64.op_count == 2 &&
               ARM64.operands[0].type == AARCH64_OP_REG &&
               ARM64.operands[1].type == AARCH64_OP_REG && Insn->size == 4) {
      uint32_t Enc;
      std::memcpy(&Enc, Insn->bytes, 4);
      bool Is64 = (Enc >> 31) & 1;
      unsigned RnIdx = (Enc >> 5) & 0x1F;
      unsigned RmIdx = (Enc >> 16) & 0x1F;
      auto RnReg = static_cast<aarch64_reg>(Is64 ? AARCH64_REG_X0 + RnIdx
                                                 : AARCH64_REG_W0 + RnIdx);
      auto RmReg = static_cast<aarch64_reg>(Is64 ? AARCH64_REG_X0 + RmIdx
                                                 : AARCH64_REG_W0 + RmIdx);
      auto RnRI = mapCapstoneReg(RnReg);
      auto RmRI = mapCapstoneReg(RmReg);
      A = (RnRI.Size > 0) ? NdVar::reg(RnRI.Offset, RnRI.Size)
                          : operandRead(S, ARM64.operands[0]);
      B = (RmRI.Size > 0) ? NdVar::reg(RmRI.Offset, RmRI.Size)
                          : NdVar::cst(0, Dst.Size);
      IsRegShift = true;
    } else {
      A = operandRead(S, ARM64.operands[0]);
      B = operandRead(S, ARM64.operands[1]);
    }
    if (IsRegShift) {
      uint64_t Mask = (Dst.Size == 8) ? 63 : 31;
      NdVar Masked = S.makeTemp(B.Size);
      S.emit(NdOp::INT_AND, Masked, {B, NdVar::cst(Mask, B.Size)});
      S.emit(NdOp::INT_RIGHT, Dst, {A, Masked});
    } else {
      S.emit(NdOp::INT_RIGHT, Dst, {A, B});
    }
    break;
  }
  case AARCH64_INS_ASR: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A, B;
    bool IsRegShift = false;
    if (ARM64.op_count >= 3) {
      A = operandRead(S, ARM64.operands[1]);
      B = operandRead(S, ARM64.operands[2]);
      IsRegShift = ARM64.operands[2].type == AARCH64_OP_REG;
    } else if (ARM64.op_count == 2 &&
               ARM64.operands[0].type == AARCH64_OP_REG &&
               ARM64.operands[1].type == AARCH64_OP_REG && Insn->size == 4) {
      uint32_t Enc;
      std::memcpy(&Enc, Insn->bytes, 4);
      bool Is64 = (Enc >> 31) & 1;
      unsigned RnIdx = (Enc >> 5) & 0x1F;
      unsigned RmIdx = (Enc >> 16) & 0x1F;
      auto RnReg = static_cast<aarch64_reg>(Is64 ? AARCH64_REG_X0 + RnIdx
                                                 : AARCH64_REG_W0 + RnIdx);
      auto RmReg = static_cast<aarch64_reg>(Is64 ? AARCH64_REG_X0 + RmIdx
                                                 : AARCH64_REG_W0 + RmIdx);
      auto RnRI = mapCapstoneReg(RnReg);
      auto RmRI = mapCapstoneReg(RmReg);
      A = (RnRI.Size > 0) ? NdVar::reg(RnRI.Offset, RnRI.Size)
                          : operandRead(S, ARM64.operands[0]);
      B = (RmRI.Size > 0) ? NdVar::reg(RmRI.Offset, RmRI.Size)
                          : NdVar::cst(0, Dst.Size);
      IsRegShift = true;
    } else {
      A = operandRead(S, ARM64.operands[0]);
      B = operandRead(S, ARM64.operands[1]);
    }
    if (IsRegShift) {
      uint64_t Mask = (Dst.Size == 8) ? 63 : 31;
      NdVar Masked = S.makeTemp(B.Size);
      S.emit(NdOp::INT_AND, Masked, {B, NdVar::cst(Mask, B.Size)});
      S.emit(NdOp::INT_ASHR, Dst, {A, Masked});
    } else {
      S.emit(NdOp::INT_ASHR, Dst, {A, B});
    }
    break;
  }

  // --- MUL / SDIV / UDIV ---
  case AARCH64_INS_MUL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    // By-element form `mul v.T, v.T, vN.<ty>[idx]` broadcasts a single source
    // lane to every destination lane.  capstone leaves vas non-element so
    // operandRead returns the whole register (high lanes unset) — reading B
    // per-lane would multiply most lanes by 0.
    int BLane = ARM64.operands[2].vector_index;
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
        NdVar BElem;
        if (BLane >= 0) {
          NdVar BFull = operandWrite(ARM64.operands[2]);
          BElem = S.makeTemp(LaneSz);
          S.emit(
              NdOp::SUBBYTES, BElem,
              {BFull, NdVar::cst(static_cast<uint64_t>(BLane) * LaneSz, 4)});
        }
        unsigned NLanes = Dst.Size / LaneSz;
        NdVar Acc = S.makeTemp(0);
        for (unsigned I = 0; I < NLanes; ++I) {
          NdVar La = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
          NdVar Lb;
          if (BLane >= 0) {
            Lb = BElem;
          } else {
            Lb = S.makeTemp(LaneSz);
            S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
          }
          NdVar Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_MULT, Lr, {La, Lb});
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
        S.emit(NdOp::INT_MULT, Dst, {A, B});
      }
    }
    break;
  }
  case AARCH64_INS_SDIV: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_SDIV, Dst, {A, B});
    break;
  }
  case AARCH64_INS_UDIV: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    S.emit(NdOp::INT_DIV, Dst, {A, B});
    break;
  }

  // --- ADRP / ADR ---
  case AARCH64_INS_ADRP:
  case AARCH64_INS_ADR: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // --- SXTW / UXTW / SXTH / UXTH ---
  case AARCH64_INS_SXTW: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    S.emit(NdOp::INT_SEXT, Dst, {Src});
    break;
  }

  // --- Conditional select family: CSEL/CSINC/CSINV/CSNEG ---
  // Capstone 6 resolves pseudo-mnemonics (CINC/CINV/CNEG/CSET/CSETM)
  // to their Base forms (CSINC/CSINV/CSNEG) with explicit operands.
  case AARCH64_INS_CSEL:
  case AARCH64_INS_CSINC:
  case AARCH64_INS_CSINV:
  case AARCH64_INS_CSNEG: {
    // Alias forms carry 1 (CSET/CSETM) or 2 (CINC/CINV/CNEG) operands;
    // everything else falls through to the canonical form below, which reads
    // operands[1] and operands[2].
    if (ARM64.op_count < 1 || (ARM64.op_count < 3 && !Insn->is_alias))
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    uint16_t Sz = Dst.Size;

    // Build condition from ARM64.cc
    NdVar Cond = S.makeTemp(1);
    switch (ARM64.cc) {
    case AArch64CC_EQ:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
      break;
    case AArch64CC_NE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
      break;
    case AArch64CC_LT:
      S.emit(NdOp::INT_NOTEQUAL, Cond,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_GE:
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_GT: {
      NdVar NZ = S.makeTemp(1);
      NdVar EqFlags = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(a64reg::ZFLAG, 1)});
      S.emit(NdOp::INT_EQUAL, EqFlags,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NZ, EqFlags});
      break;
    }
    case AArch64CC_LE: {
      NdVar NeFlags = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, NeFlags,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(a64reg::ZFLAG, 1), NeFlags});
      break;
    }
    case AArch64CC_HS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
      break;
    case AArch64CC_LO:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
      break;
    case AArch64CC_HI: {
      NdVar NZ = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(a64reg::ZFLAG, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NdVar::reg(a64reg::CFLAG, 1), NZ});
      break;
    }
    case AArch64CC_LS: {
      NdVar NC = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(a64reg::CFLAG, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(a64reg::ZFLAG, 1), NC});
      break;
    }
    case AArch64CC_MI:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::NFLAG, 1)});
      break;
    case AArch64CC_PL:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::NFLAG, 1)});
      break;
    case AArch64CC_VS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_VC:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::VFLAG, 1)});
      break;
    default:
      S.emit(NdOp::COPY, Cond, {NdVar::cst(1, 1)});
      break;
    }

    // Compute TrueVal and FalseVal based on instruction variant
    // Capstone 6 alias forms: CSET (1 op), CINC/CINV/CNEG (2 ops),
    // canonical CSEL/CSINC/CSINV/CSNEG (3 ops).
    NdVar TrueVal, FalseVal;

    if (Insn->is_alias && ARM64.op_count == 1) {
      // CSET/CSETM: Rd = Cond ? 1 : 0 (CSINC) or ~0 (CSINV)
      if (Insn->id == AARCH64_INS_CSINC) {
        TrueVal = NdVar::cst(1, Sz);
        FalseVal = NdVar::cst(0, Sz);
      } else {
        // CSETM all-ones mask; avoid shift-by-bitwidth UB when Sz==8.
        uint64_t AllOnes = (Sz >= 8) ? ~0ULL : ((1ULL << (Sz * 8)) - 1);
        TrueVal = NdVar::cst(AllOnes, Sz);
        FalseVal = NdVar::cst(0, Sz);
      }
    } else if (Insn->is_alias && ARM64.op_count == 2) {
      // CINC/CINV/CNEG: Rd = Cond ? op(Rn) : Rn
      NdVar Src = operandRead(S, ARM64.operands[1]);
      if (Insn->id == AARCH64_INS_CSINC) {
        TrueVal = S.makeTemp(Sz);
        S.emit(NdOp::INT_ADD, TrueVal, {Src, NdVar::cst(1, Sz)});
        FalseVal = Src;
      } else if (Insn->id == AARCH64_INS_CSINV) {
        TrueVal = S.makeTemp(Sz);
        S.emit(NdOp::INT_NOT, TrueVal, {Src});
        FalseVal = Src;
      } else {
        TrueVal = S.makeTemp(Sz);
        S.emit(NdOp::INT_NEG2, TrueVal, {Src});
        FalseVal = Src;
      }
    } else {
      // Canonical 3-operand form
      switch (Insn->id) {
      case AARCH64_INS_CSEL:
        TrueVal = operandRead(S, ARM64.operands[1]);
        FalseVal = operandRead(S, ARM64.operands[2]);
        break;
      case AARCH64_INS_CSINC: {
        TrueVal = operandRead(S, ARM64.operands[1]);
        NdVar Src2 = operandRead(S, ARM64.operands[2]);
        FalseVal = S.makeTemp(Sz);
        S.emit(NdOp::INT_ADD, FalseVal, {Src2, NdVar::cst(1, Sz)});
        break;
      }
      case AARCH64_INS_CSINV: {
        TrueVal = operandRead(S, ARM64.operands[1]);
        NdVar Src2 = operandRead(S, ARM64.operands[2]);
        FalseVal = S.makeTemp(Sz);
        S.emit(NdOp::INT_NOT, FalseVal, {Src2});
        break;
      }
      case AARCH64_INS_CSNEG: {
        TrueVal = operandRead(S, ARM64.operands[1]);
        NdVar Src2 = operandRead(S, ARM64.operands[2]);
        FalseVal = S.makeTemp(Sz);
        S.emit(NdOp::INT_NEG2, FalseVal, {Src2});
        break;
      }
      default:
        break;
      }
    }

    S.emit(NdOp::SELECT, Dst, {Cond, TrueVal, FalseVal});
    break;
  }

  // --- NEG (SUB from Zero; NEGS is now SUBS XZR handled above) ---
  case AARCH64_INS_NEG: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    // NEON vector form `neg v.4s` negates each lane independently.  A single
    // full-width INT_NEG2 would propagate borrows across lane boundaries and
    // corrupt the upper lanes (the scalar form is just one lane and is fine).
    unsigned LaneSz = 0;
    auto Vas = ARM64.operands[0].vas;
    if (Vas == AARCH64LAYOUT_VL_2D)
      LaneSz = 8;
    else if (Vas == AARCH64LAYOUT_VL_4S || Vas == AARCH64LAYOUT_VL_2S)
      LaneSz = 4;
    else if (Vas == AARCH64LAYOUT_VL_8H || Vas == AARCH64LAYOUT_VL_4H)
      LaneSz = 2;
    else if (Vas == AARCH64LAYOUT_VL_16B || Vas == AARCH64LAYOUT_VL_8B)
      LaneSz = 1;
    if (LaneSz > 0 && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Ls = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Ls, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Ln = S.makeTemp(LaneSz);
        S.emit(NdOp::INT_NEG2, Ln, {Ls});
        if (I == 0) {
          Acc = Ln;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Ln, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      S.emit(NdOp::INT_NEG2, Dst, {Src});
    }
    break;
  }

  // --- UBFX / SBFX / UBFM / SBFM ---
  case AARCH64_INS_UBFM: {
    // Capstone 6 alias: LSL/LSR/UXTB/UXTH → id=UBFM, op_count=2
    if (Insn->is_alias && ARM64.op_count < 4 && ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      uint16_t Sz = Dst.Size;
      auto ShT = ARM64.operands[1].shift.type;
      uint32_t ShV = ARM64.operands[1].shift.value;
      bool HasShift = (ShT != AARCH64_SFT_INVALID && ShV != 0);
      // Read raw register value (without shift) to avoid double-shift
      NdVar Src;
      if (HasShift && ARM64.operands[1].type == AARCH64_OP_REG) {
        auto RI =
            mapCapstoneReg(static_cast<aarch64_reg>(ARM64.operands[1].reg));
        Src = (RI.Size == 0)               ? NdVar::cst(0, 8)
              : (RI.Offset == a64reg::XZR) ? NdVar::cst(0, RI.Size)
                                           : NdVar::reg(RI.Offset, RI.Size);
      } else {
        Src = operandRead(S, ARM64.operands[1]);
      }
      if (ShT == AARCH64_SFT_LSL && ShV > 0) {
        S.emit(NdOp::INT_LEFT, Dst, {Src, NdVar::cst(ShV, Sz)});
      } else if (ShT == AARCH64_SFT_LSR && ShV > 0) {
        S.emit(NdOp::INT_RIGHT, Dst, {Src, NdVar::cst(ShV, Sz)});
      } else {
        uint16_t ExtSz = Src.Size;
        if (Insn->alias_id == AARCH64_INS_ALIAS_UXTB)
          ExtSz = 1;
        else if (Insn->alias_id == AARCH64_INS_ALIAS_UXTH)
          ExtSz = 2;

        if (ExtSz < Src.Size) {
          NdVar Narrow = S.makeTemp(ExtSz);
          S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
          S.emit(NdOp::INT_ZEXT, Dst, {Narrow});
        } else if (Src.Size > Dst.Size) {
          S.emit(NdOp::SUBBYTES, Dst, {Src, NdVar::cst(0, 4)});
        } else if (Src.Size < Dst.Size) {
          S.emit(NdOp::INT_ZEXT, Dst, {Src});
        } else {
          S.emit(NdOp::COPY, Dst, {Src});
        }
      }
      break;
    }
    if (ARM64.op_count < 4)
      break;
    {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[1]);
      uint16_t Sz = Dst.Size;
      uint32_t Bits = Sz * 8;

      if (Insn->is_alias) {
        // Alias form: operands are (Rd, Rn, #lsb, #Width) for UBFIZ/UBFX
        uint64_t LSB = static_cast<uint64_t>(ARM64.operands[2].imm);
        uint64_t Width = static_cast<uint64_t>(ARM64.operands[3].imm);
        const char *Mn = Insn->mnemonic;
        if (Mn[0] == 'u' && Mn[1] == 'b' && Mn[2] == 'f' && Mn[3] == 'i') {
          uint64_t Mask = (Width >= 64) ? ~0ULL : ((1ULL << Width) - 1);
          NdVar Masked = S.makeTemp(Sz);
          S.emit(NdOp::INT_AND, Masked, {Src, NdVar::cst(Mask, Sz)});
          S.emit(NdOp::INT_LEFT, Dst, {Masked, NdVar::cst(LSB, Sz)});
        } else {
          NdVar Shifted = S.makeTemp(Sz);
          S.emit(NdOp::INT_RIGHT, Shifted, {Src, NdVar::cst(LSB, Sz)});
          uint64_t Mask = (Width >= 64) ? ~0ULL : ((1ULL << Width) - 1);
          S.emit(NdOp::INT_AND, Dst, {Shifted, NdVar::cst(Mask, Sz)});
        }
      } else {
        uint64_t ImmR = static_cast<uint64_t>(ARM64.operands[2].imm);
        uint64_t ImmS = static_cast<uint64_t>(ARM64.operands[3].imm);
        if (ImmS >= ImmR) {
          uint64_t Width = ImmS - ImmR + 1;
          NdVar Shifted = S.makeTemp(Sz);
          S.emit(NdOp::INT_RIGHT, Shifted, {Src, NdVar::cst(ImmR, Sz)});
          uint64_t MaskVal = (Width >= 64) ? ~0ULL : ((1ULL << Width) - 1);
          S.emit(NdOp::INT_AND, Dst, {Shifted, NdVar::cst(MaskVal, Sz)});
        } else {
          NdVar Lo = S.makeTemp(Sz);
          NdVar Hi = S.makeTemp(Sz);
          S.emit(NdOp::INT_RIGHT, Lo, {Src, NdVar::cst(ImmR, Sz)});
          S.emit(NdOp::INT_LEFT, Hi, {Src, NdVar::cst(Bits - ImmR, Sz)});
          NdVar Rotated = S.makeTemp(Sz);
          S.emit(NdOp::INT_OR, Rotated, {Lo, Hi});
          uint64_t LSBPos = Bits - ImmR;
          uint64_t Width = ImmS + 1;
          uint64_t MaskVal = ((Width >= 64) ? ~0ULL : ((1ULL << Width) - 1))
                             << LSBPos;
          S.emit(NdOp::INT_AND, Dst, {Rotated, NdVar::cst(MaskVal, Sz)});
        }
      }
      break;
    } // end UBFM 4-op scope
  } // end case UBFM
  case AARCH64_INS_SBFM: {
    // Capstone 6 alias: ASR/SXTB/SXTH/SXTW → id=SBFM, op_count=2
    if (Insn->is_alias && ARM64.op_count < 4 && ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      uint16_t Sz = Dst.Size;
      auto ShT = ARM64.operands[1].shift.type;
      uint32_t ShV = ARM64.operands[1].shift.value;
      bool HasShift = (ShT != AARCH64_SFT_INVALID && ShV != 0);
      // Read raw register value (without shift) to avoid double-shift
      NdVar Src;
      if (HasShift && ARM64.operands[1].type == AARCH64_OP_REG) {
        auto RI =
            mapCapstoneReg(static_cast<aarch64_reg>(ARM64.operands[1].reg));
        Src = (RI.Size == 0)               ? NdVar::cst(0, 8)
              : (RI.Offset == a64reg::XZR) ? NdVar::cst(0, RI.Size)
                                           : NdVar::reg(RI.Offset, RI.Size);
      } else {
        Src = operandRead(S, ARM64.operands[1]);
      }
      if (ShT == AARCH64_SFT_ASR && ShV > 0) {
        S.emit(NdOp::INT_ASHR, Dst, {Src, NdVar::cst(ShV, Sz)});
      } else {
        // Determine actual extension width from alias_id:
        // SXTB → 1 byte, SXTH → 2 bytes, SXTW → 4 bytes
        uint16_t ExtSz = Src.Size;
        if (Insn->alias_id == AARCH64_INS_ALIAS_SXTB)
          ExtSz = 1;
        else if (Insn->alias_id == AARCH64_INS_ALIAS_SXTH)
          ExtSz = 2;

        if (ExtSz < Src.Size) {
          NdVar Narrow = S.makeTemp(ExtSz);
          S.emit(NdOp::SUBBYTES, Narrow, {Src, NdVar::cst(0, 4)});
          S.emit(NdOp::INT_SEXT, Dst, {Narrow});
        } else if (Src.Size < Dst.Size) {
          S.emit(NdOp::INT_SEXT, Dst, {Src});
        } else {
          S.emit(NdOp::COPY, Dst, {Src});
        }
      }
      break;
    }
    if (ARM64.op_count < 4)
      break;
    {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar Src = operandRead(S, ARM64.operands[1]);
      uint16_t Sz = Dst.Size;
      uint32_t Bits = Sz * 8;

      if (Insn->is_alias) {
        uint64_t LSB = static_cast<uint64_t>(ARM64.operands[2].imm);
        uint64_t Width = static_cast<uint64_t>(ARM64.operands[3].imm);
        const char *Mn = Insn->mnemonic;
        if (Mn[0] == 's' && Mn[1] == 'b' && Mn[2] == 'f' && Mn[3] == 'i') {
          uint32_t ShiftAmt = Bits - static_cast<uint32_t>(Width + LSB);
          NdVar ShlV = S.makeTemp(Sz);
          S.emit(NdOp::INT_LEFT, ShlV, {Src, NdVar::cst(ShiftAmt + LSB, Sz)});
          S.emit(NdOp::INT_ASHR, Dst, {ShlV, NdVar::cst(ShiftAmt, Sz)});
        } else {
          NdVar Shifted = S.makeTemp(Sz);
          S.emit(NdOp::INT_RIGHT, Shifted, {Src, NdVar::cst(LSB, Sz)});
          uint32_t ShiftAmt = Bits - static_cast<uint32_t>(Width);
          NdVar ShlV = S.makeTemp(Sz);
          S.emit(NdOp::INT_LEFT, ShlV, {Shifted, NdVar::cst(ShiftAmt, Sz)});
          S.emit(NdOp::INT_ASHR, Dst, {ShlV, NdVar::cst(ShiftAmt, Sz)});
        }
      } else {
        uint64_t ImmR = static_cast<uint64_t>(ARM64.operands[2].imm);
        uint64_t ImmS = static_cast<uint64_t>(ARM64.operands[3].imm);

        if (ImmS >= ImmR) {
          uint64_t Width = ImmS - ImmR + 1;
          NdVar Shifted = S.makeTemp(Sz);
          S.emit(NdOp::INT_RIGHT, Shifted, {Src, NdVar::cst(ImmR, Sz)});
          uint32_t ShiftAmt = Bits - static_cast<uint32_t>(Width);
          NdVar ShlV = S.makeTemp(Sz);
          S.emit(NdOp::INT_LEFT, ShlV, {Shifted, NdVar::cst(ShiftAmt, Sz)});
          S.emit(NdOp::INT_ASHR, Dst, {ShlV, NdVar::cst(ShiftAmt, Sz)});
        } else {
          // SBFIZ: extract Src[ImmS:0], place at [Bits-ImmR+ImmS : Bits-ImmR],
          // sign-extend from the top of the inserted field.
          uint64_t Width = ImmS + 1;
          uint64_t LSBPos = Bits - ImmR;
          uint64_t ExtractMask = (Width >= 64) ? ~0ULL : ((1ULL << Width) - 1);
          NdVar Extracted = S.makeTemp(Sz);
          S.emit(NdOp::INT_AND, Extracted,
                 {Src, NdVar::cst(ExtractMask, Sz)});
          NdVar Shifted = S.makeTemp(Sz);
          S.emit(NdOp::INT_LEFT, Shifted,
                 {Extracted, NdVar::cst(LSBPos, Sz)});
          uint32_t TopBit = static_cast<uint32_t>(LSBPos + ImmS);
          uint32_t SignShift = Bits - 1 - TopBit;
          NdVar ShlV = S.makeTemp(Sz);
          S.emit(NdOp::INT_LEFT, ShlV, {Shifted, NdVar::cst(SignShift, Sz)});
          S.emit(NdOp::INT_ASHR, Dst, {ShlV, NdVar::cst(SignShift, Sz)});
        }
      } // end !is_alias
      break;
    } // end SBFM 4-op scope
  } // end case SBFM

  // --- MADD / MSUB ---
  // Capstone 6 decodes `MUL Rd, Rn, Rm` as MADD with op_count=3 (alias).
  // Canonical MADD has 4 operands: Rd, Rn, Rm, Ra (Rd = Ra + Rn*Rm).
  case AARCH64_INS_MADD: {
    if (ARM64.op_count >= 4) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar N = narrowToWidth(S, operandRead(S, ARM64.operands[1]), Dst.Size);
      NdVar M = narrowToWidth(S, operandRead(S, ARM64.operands[2]), Dst.Size);
      NdVar A = narrowToWidth(S, operandRead(S, ARM64.operands[3]), Dst.Size);
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {N, M});
      S.emit(NdOp::INT_ADD, Dst, {A, Prod});
    } else if (ARM64.op_count == 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar N = operandRead(S, ARM64.operands[1]);
      NdVar M = operandRead(S, ARM64.operands[2]);
      S.emit(NdOp::INT_MULT, Dst, {N, M});
    }
    break;
  }
  case AARCH64_INS_MSUB: {
    if (ARM64.op_count >= 4) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar N = narrowToWidth(S, operandRead(S, ARM64.operands[1]), Dst.Size);
      NdVar M = narrowToWidth(S, operandRead(S, ARM64.operands[2]), Dst.Size);
      NdVar A = narrowToWidth(S, operandRead(S, ARM64.operands[3]), Dst.Size);
      NdVar Prod = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_MULT, Prod, {N, M});
      S.emit(NdOp::INT_SUB, Dst, {A, Prod});
    } else if (ARM64.op_count == 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar N = operandRead(S, ARM64.operands[1]);
      NdVar M = operandRead(S, ARM64.operands[2]);
      S.emit(NdOp::INT_MULT, Dst, {N, M});
      S.emit(NdOp::INT_NEG2, Dst, {Dst});
    }
    break;
  }

  // --- CLZ (count leading zeros) ---
  case AARCH64_INS_CLZ: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
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
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
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

  // --- BIC (bit clear: AND NOT) ---
  case AARCH64_INS_BIC:
  case AARCH64_INS_BICS: {
    // SIMD BIC (vector, immediate): `bic vD.<T>, #imm{, lsl #s}` -> vD &=
    // ~bcast.
    if (ARM64.op_count == 2 && ARM64.operands[0].type == AARCH64_OP_REG &&
        ARM64.operands[0].vas != AARCH64LAYOUT_INVALID &&
        ARM64.operands[1].type == AARCH64_OP_IMM) {
      emitSimdImmLogic(S, ARM64, /*IsBic=*/true);
      break;
    }
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Inv = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, Inv, {B});
    S.emit(NdOp::INT_AND, Dst, {A, Inv});
    if (Insn->id == AARCH64_INS_BICS) {
      S.emit(NdOp::INT_EQUAL, NdVar::reg(a64reg::ZFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      S.emit(NdOp::INT_SLESS, NdVar::reg(a64reg::NFLAG, 1),
             {Dst, NdVar::cst(0, Dst.Size)});
      // BICS, like ANDS, clears C and V (only N/Z reflect the result).
      S.emit(NdOp::COPY, NdVar::reg(a64reg::CFLAG, 1), {NdVar::cst(0, 1)});
      S.emit(NdOp::COPY, NdVar::reg(a64reg::VFLAG, 1), {NdVar::cst(0, 1)});
    }
    break;
  }

  // --- EON (exclusive OR NOT) ---
  case AARCH64_INS_EON: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar Inv = S.makeTemp(B.Size);
    S.emit(NdOp::INT_NOT, Inv, {B});
    S.emit(NdOp::INT_XOR, Dst, {A, Inv});
    break;
  }

  // ORN is handled above (Merged with former MVN case)

  // --- ROR (rotate right) ---
  // Capstone 6 alias: RORV Xd,Xn,Xm may be reported as ROR with 2
  // operands — extract Rn and Rm from the encoding.
  case AARCH64_INS_ROR: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src, Amt;
    bool IsRegRot = false;
    if (ARM64.op_count >= 3) {
      Src = operandRead(S, ARM64.operands[1]);
      Amt = operandRead(S, ARM64.operands[2]);
      IsRegRot = ARM64.operands[2].type == AARCH64_OP_REG;
    } else if (ARM64.op_count == 2 &&
               ARM64.operands[0].type == AARCH64_OP_REG &&
               ARM64.operands[1].type == AARCH64_OP_REG && Insn->size == 4) {
      uint32_t Enc;
      std::memcpy(&Enc, Insn->bytes, 4);
      bool Is64 = (Enc >> 31) & 1;
      unsigned RnIdx = (Enc >> 5) & 0x1F;
      unsigned RmIdx = (Enc >> 16) & 0x1F;
      auto RnReg = static_cast<aarch64_reg>(Is64 ? AARCH64_REG_X0 + RnIdx
                                                 : AARCH64_REG_W0 + RnIdx);
      auto RmReg = static_cast<aarch64_reg>(Is64 ? AARCH64_REG_X0 + RmIdx
                                                 : AARCH64_REG_W0 + RmIdx);
      auto RnRI = mapCapstoneReg(RnReg);
      auto RmRI = mapCapstoneReg(RmReg);
      Src = (RnRI.Size > 0) ? NdVar::reg(RnRI.Offset, RnRI.Size)
                            : operandRead(S, ARM64.operands[0]);
      Amt = (RmRI.Size > 0) ? NdVar::reg(RmRI.Offset, RmRI.Size)
                            : NdVar::cst(0, Dst.Size);
      IsRegRot = true;
    } else {
      Src = operandRead(S, ARM64.operands[0]);
      Amt = operandRead(S, ARM64.operands[1]);
    }
    uint16_t Bits = Dst.Size * 8;
    NdVar MaskedAmt = Amt;
    if (IsRegRot) {
      uint64_t Mask = (Dst.Size == 8) ? 63 : 31;
      MaskedAmt = S.makeTemp(Amt.Size);
      S.emit(NdOp::INT_AND, MaskedAmt, {Amt, NdVar::cst(Mask, Amt.Size)});
    }
    NdVar RightPart = S.makeTemp(Dst.Size);
    NdVar LeftPart = S.makeTemp(Dst.Size);
    NdVar Comp = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_RIGHT, RightPart, {Src, MaskedAmt});
    S.emit(NdOp::INT_SUB, Comp, {NdVar::cst(Bits, Dst.Size), MaskedAmt});
    S.emit(NdOp::INT_LEFT, LeftPart, {Src, Comp});
    S.emit(NdOp::INT_OR, Dst, {RightPart, LeftPart});
    break;
  }

  // --- EXTR (extract from pair) ---
  case AARCH64_INS_EXTR: {
    // Capstone 6 alias: ROR Xd, Xn, #shift → id=EXTR, op_count=2
    if (Insn->is_alias && ARM64.op_count < 4 && ARM64.op_count >= 2) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      uint16_t Sz = Dst.Size;
      uint32_t Bits = Sz * 8;
      auto ShT = ARM64.operands[1].shift.type;
      uint32_t ShV = ARM64.operands[1].shift.value;
      bool HasShift = (ShT != AARCH64_SFT_INVALID && ShV != 0);
      NdVar Src;
      if (HasShift && ARM64.operands[1].type == AARCH64_OP_REG) {
        auto RI =
            mapCapstoneReg(static_cast<aarch64_reg>(ARM64.operands[1].reg));
        Src = (RI.Size == 0)               ? NdVar::cst(0, 8)
              : (RI.Offset == a64reg::XZR) ? NdVar::cst(0, RI.Size)
                                           : NdVar::reg(RI.Offset, RI.Size);
      } else {
        Src = operandRead(S, ARM64.operands[1]);
      }
      if (ShV > 0 && ShV < Bits) {
        NdVar Lo = S.makeTemp(Sz);
        NdVar Hi = S.makeTemp(Sz);
        S.emit(NdOp::INT_RIGHT, Lo, {Src, NdVar::cst(ShV, Sz)});
        S.emit(NdOp::INT_LEFT, Hi, {Src, NdVar::cst(Bits - ShV, Sz)});
        S.emit(NdOp::INT_OR, Dst, {Lo, Hi});
      } else {
        S.emit(NdOp::COPY, Dst, {Src});
      }
      break;
    }
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Hi = operandRead(S, ARM64.operands[1]);
    NdVar Lo = operandRead(S, ARM64.operands[2]);
    NdVar LSB = operandRead(S, ARM64.operands[3]);
    uint16_t Bits = Dst.Size * 8;
    NdVar LoShifted = S.makeTemp(Dst.Size);
    NdVar HiShifted = S.makeTemp(Dst.Size);
    NdVar Comp = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_RIGHT, LoShifted, {Lo, LSB});
    S.emit(NdOp::INT_SUB, Comp, {NdVar::cst(Bits, Dst.Size), LSB});
    S.emit(NdOp::INT_LEFT, HiShifted, {Hi, Comp});
    S.emit(NdOp::INT_OR, Dst, {LoShifted, HiShifted});
    break;
  }

  // --- BFM (BFI/BFXIL resolved to BFM in Capstone 6) ---
  case AARCH64_INS_BFM: {
    // BFC Xd,#lsb,#width is a BFM alias whose source is the implicit zero
    // register, so Capstone surfaces only THREE operands (Xd,#lsb,#width) with
    // no Rn.  It clears `width` bits starting at `lsb`.  Without this the
    // generic
    // >=4-operand path below `break`s out on op_count==3 and leaves Rd
    // unchanged (BFC was silently a no-op).
    if (Insn->is_alias && Insn->mnemonic[0] == 'b' &&
        Insn->mnemonic[1] == 'f' && Insn->mnemonic[2] == 'c' &&
        ARM64.op_count >= 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      uint16_t Sz = Dst.Size;
      uint32_t Bits = Sz * 8;
      uint64_t Lsb =
          static_cast<uint64_t>(ARM64.operands[ARM64.op_count - 2].imm);
      uint64_t Width =
          static_cast<uint64_t>(ARM64.operands[ARM64.op_count - 1].imm);
      uint64_t FieldMask = (Width >= 64) ? ~0ULL : ((1ULL << Width) - 1);
      uint64_t ClearMask = (Lsb >= Bits) ? 0 : (FieldMask << Lsb);
      S.emit(NdOp::INT_AND, Dst,
             {NdVar::reg(Dst.Offset, Sz), NdVar::cst(~ClearMask, Sz)});
      break;
    }
    if (ARM64.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    uint16_t Sz = Dst.Size;
    uint32_t Bits = Sz * 8;

    uint64_t ImmR, ImmS;
    if (Insn->is_alias) {
      // Capstone 6 alias: BFI/BFXIL provide (lsb, width) not (ImmR, ImmS).
      uint64_t Lsb = static_cast<uint64_t>(ARM64.operands[2].imm);
      uint64_t Width = static_cast<uint64_t>(ARM64.operands[3].imm);
      const char *Mn = Insn->mnemonic;
      if (Mn[0] == 'b' && Mn[1] == 'f' && Mn[2] == 'i') {
        // BFI Xd,Xn,#lsb,#width → BFM Xd,Xn,#(-lsb MOD Bits),#(width-1)
        ImmR = (Bits - Lsb) % Bits;
        ImmS = Width - 1;
      } else {
        // BFXIL Xd,Xn,#lsb,#width → BFM Xd,Xn,#lsb,#(lsb+width-1)
        ImmR = Lsb;
        ImmS = Lsb + Width - 1;
      }
    } else {
      ImmR = static_cast<uint64_t>(ARM64.operands[2].imm);
      ImmS = static_cast<uint64_t>(ARM64.operands[3].imm);
    }

    {
      // Both cases use ROR(Src, ImmR) then Mask at the correct position.
      // ARM pseudocode: bot = (Xd & ~wmask) | (ROR(Xn, ImmR) & wmask)
      NdVar Rotated;
      if (ImmR == 0) {
        Rotated = Src;
      } else {
        NdVar Lo = S.makeTemp(Sz);
        NdVar Hi = S.makeTemp(Sz);
        S.emit(NdOp::INT_RIGHT, Lo, {Src, NdVar::cst(ImmR, Sz)});
        S.emit(NdOp::INT_LEFT, Hi, {Src, NdVar::cst(Bits - ImmR, Sz)});
        Rotated = S.makeTemp(Sz);
        S.emit(NdOp::INT_OR, Rotated, {Lo, Hi});
      }

      uint64_t Mask;
      if (ImmS >= ImmR) {
        // BFXIL: extract Src[ImmS:ImmR] → Dst[Width-1:0]
        uint64_t Width = ImmS - ImmR + 1;
        Mask = (Width >= 64) ? ~0ULL : ((1ULL << Width) - 1);
      } else {
        // BFI: insert Src[ImmS:0] → Dst[lsb+ImmS:lsb] where lsb=Bits-ImmR
        uint64_t LSBPos = Bits - ImmR;
        uint64_t Width = ImmS + 1;
        Mask = ((Width >= 64) ? ~0ULL : ((1ULL << Width) - 1)) << LSBPos;
      }
      NdVar MaskedSrc = S.makeTemp(Sz);
      S.emit(NdOp::INT_AND, MaskedSrc, {Rotated, NdVar::cst(Mask, Sz)});
      NdVar ClearedDst = S.makeTemp(Sz);
      S.emit(NdOp::INT_AND, ClearedDst,
             {NdVar::reg(Dst.Offset, Sz), NdVar::cst(~Mask, Sz)});
      S.emit(NdOp::INT_OR, Dst, {ClearedDst, MaskedSrc});
    }
    break;
  }

  // --- SMULL / UMULL ---
  case AARCH64_INS_SMULL:
  case AARCH64_INS_UMULL: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    bool IsSigned = (Insn->id == AARCH64_INS_SMULL);

    auto DstVas = ARM64.operands[0].vas;
    unsigned DstLane = 0;
    if (DstVas == AARCH64LAYOUT_VL_4S)
      DstLane = 4;
    else if (DstVas == AARCH64LAYOUT_VL_8H)
      DstLane = 2;
    else if (DstVas == AARCH64LAYOUT_VL_2D)
      DstLane = 8;

    if (DstLane > 0 && Dst.Size > DstLane) {
      unsigned NLanes = Dst.Size / DstLane;
      unsigned NarrowLane = DstLane / 2;
      // By-element `smull v.Ts, v.Th, vN.<ty>[idx]`: operandRead returns just
      // the selected element, so broadcast it to every lane instead of walking
      // B (a per-lane SUBBYTES would read past the element and zero
      // lanes 1..N).
      bool BScalar = (B.Size <= NarrowLane);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar NarrA = S.makeTemp(NarrowLane);
        S.emit(NdOp::SUBBYTES, NarrA, {A, NdVar::cst(I * NarrowLane, 4)});
        NdVar WA = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WA, {NarrA});
        NdVar NarrB = BScalar ? B : S.makeTemp(NarrowLane);
        if (!BScalar)
          S.emit(NdOp::SUBBYTES, NarrB, {B, NdVar::cst(I * NarrowLane, 4)});
        NdVar WB = S.makeTemp(DstLane);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WB, {NarrB});
        NdVar Lr = S.makeTemp(DstLane);
        S.emit(NdOp::INT_MULT, Lr, {WA, WB});
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
      NdVar ExtA = S.makeTemp(8);
      NdVar ExtB = S.makeTemp(8);
      if (IsSigned) {
        S.emit(NdOp::INT_SEXT, ExtA, {A});
        S.emit(NdOp::INT_SEXT, ExtB, {B});
      } else {
        S.emit(NdOp::INT_ZEXT, ExtA, {A});
        S.emit(NdOp::INT_ZEXT, ExtB, {B});
      }
      S.emit(NdOp::INT_MULT, Dst, {ExtA, ExtB});
    }
    break;
  }

  // --- SMADDL / UMADDL (multiply-add long) ---
  // Capstone 6 alias: SMULL/UMULL Xd, Wn, Wm → id=SMADDL/UMADDL, op_count=3
  case AARCH64_INS_SMADDL:
  case AARCH64_INS_UMADDL: {
    if (Insn->is_alias && ARM64.op_count == 3) {
      NdVar Dst = operandWrite(ARM64.operands[0]);
      NdVar N = operandRead(S, ARM64.operands[1]);
      NdVar M = operandRead(S, ARM64.operands[2]);
      NdVar ExtN = S.makeTemp(8), ExtM = S.makeTemp(8);
      if (Insn->id == AARCH64_INS_SMADDL) {
        S.emit(NdOp::INT_SEXT, ExtN, {N});
        S.emit(NdOp::INT_SEXT, ExtM, {M});
      } else {
        S.emit(NdOp::INT_ZEXT, ExtN, {N});
        S.emit(NdOp::INT_ZEXT, ExtM, {M});
      }
      S.emit(NdOp::INT_MULT, Dst, {ExtN, ExtM});
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
    NdVar Prod = S.makeTemp(8);
    if (Insn->id == AARCH64_INS_SMADDL) {
      S.emit(NdOp::INT_SEXT, ExtN, {N});
      S.emit(NdOp::INT_SEXT, ExtM, {M});
    } else {
      S.emit(NdOp::INT_ZEXT, ExtN, {N});
      S.emit(NdOp::INT_ZEXT, ExtM, {M});
    }
    S.emit(NdOp::INT_MULT, Prod, {ExtN, ExtM});
    S.emit(NdOp::INT_ADD, Dst, {A, Prod});
    break;
  }

  // --- UMULH / SMULH (upper Half of 64×64 → 128 multiply) ---
  case AARCH64_INS_UMULH:
  case AARCH64_INS_SMULH: {
    if (ARM64.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar A = operandRead(S, ARM64.operands[1]);
    NdVar B = operandRead(S, ARM64.operands[2]);
    NdVar AExt = S.makeTemp(16);
    NdVar BExt = S.makeTemp(16);
    if (Insn->id == AARCH64_INS_SMULH) {
      S.emit(NdOp::INT_SEXT, AExt, {A});
      S.emit(NdOp::INT_SEXT, BExt, {B});
    } else {
      S.emit(NdOp::INT_ZEXT, AExt, {A});
      S.emit(NdOp::INT_ZEXT, BExt, {B});
    }
    NdVar Prod = S.makeTemp(16);
    S.emit(NdOp::INT_MULT, Prod, {AExt, BExt});
    S.emit(NdOp::SUBBYTES, Dst, {Prod, NdVar::cst(8, 4)});
    break;
  }

  // --- CLS (count leading sign Bits) ---
  // CLS(x) = CLZ(x ^ (x ASR 1)) - 1
  case AARCH64_INS_CLS: {
    if (ARM64.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM64.operands[0]);
    NdVar Src = operandRead(S, ARM64.operands[1]);
    NdVar Shifted = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_ASHR, Shifted, {Src, NdVar::cst(1, Src.Size)});
    NdVar Xored = S.makeTemp(Src.Size);
    S.emit(NdOp::INT_XOR, Xored, {Src, Shifted});
    NdVar Clz = S.makeTemp(Src.Size);
    S.emit(NdOp::LZCOUNT, Clz, {Xored});
    S.emit(NdOp::INT_SUB, Dst, {Clz, NdVar::cst(1, Src.Size)});
    break;
  }

  // --- CCMP / CCMN (conditional compare) ---
  // If condition true: set NZCV from CMP(A,b) or CMN(A,b)
  // If condition false: set NZCV = immediate nzcv Bits
  case AARCH64_INS_CCMP:
  case AARCH64_INS_CCMN: {
    // operands: Rn, Rm-or-#imm, #nzcv; the condition is in ARM64.cc (not an
    // operand), so op_count is 3 (matching FCCMP).  The old `< 4` guard
    // silently dropped every ccmp, leaving consumers to read the prior cmp's
    // stale flags (AArch64 `cmp; ccmp; csinc` branchless idiom).
    if (ARM64.op_count < 3)
      break;
    NdVar A = operandRead(S, ARM64.operands[0]);
    NdVar B = operandRead(S, ARM64.operands[1]);
    uint64_t NZCVImm = 0;
    if (ARM64.operands[2].type == AARCH64_OP_IMM)
      NZCVImm = ARM64.operands[2].imm;

    // Compute compare Result
    NdVar TmpR = S.makeTemp(A.Size);
    if (Insn->id == AARCH64_INS_CCMP)
      S.emit(NdOp::INT_SUB, TmpR, {A, B});
    else
      S.emit(NdOp::INT_ADD, TmpR, {A, B});

    // Z, N from Result
    NdVar CmpZ = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, CmpZ, {TmpR, NdVar::cst(0, TmpR.Size)});
    NdVar CmpN = S.makeTemp(1);
    S.emit(NdOp::INT_SLESS, CmpN, {TmpR, NdVar::cst(0, TmpR.Size)});

    // C, V
    NdVar CmpC = S.makeTemp(1);
    NdVar CmpV = S.makeTemp(1);
    if (Insn->id == AARCH64_INS_CCMP) {
      NdVar Borrow = S.makeTemp(1);
      S.emit(NdOp::INT_LESS, Borrow, {A, B});
      S.emit(NdOp::BOOL_NOT, CmpC, {Borrow});
      S.emit(NdOp::INT_SBOR, CmpV, {A, B});
    } else {
      S.emit(NdOp::INT_CARRY, CmpC, {A, B});
      S.emit(NdOp::INT_SOVF, CmpV, {A, B});
    }

    // Evaluate condition
    NdVar Cond = S.makeTemp(1);
    switch (ARM64.cc) {
    case AArch64CC_EQ:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
      break;
    case AArch64CC_NE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::ZFLAG, 1)});
      break;
    case AArch64CC_HS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
      break;
    case AArch64CC_LO:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::CFLAG, 1)});
      break;
    case AArch64CC_MI:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::NFLAG, 1)});
      break;
    case AArch64CC_PL:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::NFLAG, 1)});
      break;
    case AArch64CC_VS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_VC:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(a64reg::VFLAG, 1)});
      break;
    case AArch64CC_LT: {
      S.emit(NdOp::INT_NOTEQUAL, Cond,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      break;
    }
    case AArch64CC_GE: {
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      break;
    }
    case AArch64CC_GT: {
      NdVar NZ = S.makeTemp(1);
      NdVar EqFlags = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(a64reg::ZFLAG, 1)});
      S.emit(NdOp::INT_EQUAL, EqFlags,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NZ, EqFlags});
      break;
    }
    case AArch64CC_LE: {
      NdVar NeFlags = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, NeFlags,
             {NdVar::reg(a64reg::NFLAG, 1), NdVar::reg(a64reg::VFLAG, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(a64reg::ZFLAG, 1), NeFlags});
      break;
    }
    case AArch64CC_HI: {
      NdVar NZ = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(a64reg::ZFLAG, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NdVar::reg(a64reg::CFLAG, 1), NZ});
      break;
    }
    case AArch64CC_LS: {
      NdVar NC = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(a64reg::CFLAG, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(a64reg::ZFLAG, 1), NC});
      break;
    }
    default:
      S.emit(NdOp::COPY, Cond, {NdVar::cst(1, 1)});
      break;
    }

    // Immediate fallback NZCV
    NdVar ImmN = NdVar::cst((NZCVImm >> 3) & 1, 1);
    NdVar ImmZ = NdVar::cst((NZCVImm >> 2) & 1, 1);
    NdVar ImmC = NdVar::cst((NZCVImm >> 1) & 1, 1);
    NdVar ImmV = NdVar::cst(NZCVImm & 1, 1);

    // SELECT based on condition
    S.emit(NdOp::SELECT, NdVar::reg(a64reg::NFLAG, 1), {Cond, CmpN, ImmN});
    S.emit(NdOp::SELECT, NdVar::reg(a64reg::ZFLAG, 1), {Cond, CmpZ, ImmZ});
    S.emit(NdOp::SELECT, NdVar::reg(a64reg::CFLAG, 1), {Cond, CmpC, ImmC});
    S.emit(NdOp::SELECT, NdVar::reg(a64reg::VFLAG, 1), {Cond, CmpV, ImmV});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
