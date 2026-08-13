//===- ARMLiftFlags.cpp - ARM32 lifter flag helpers ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Condition-code evaluation and NZCV/GE flag emission shared by every
/// ARM32 category file, including the barrel-shifter carry-out rules for
/// the flag-setting data-processing forms.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/ARMLifter.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

// ===----------------------------------------------------------------------===//
// Condition code + flags
// ===----------------------------------------------------------------------===//

NdVar ARMLifter::buildCondCode(ARMCC_CondCodes CC, LiftState &S) {
  NdVar Cond = S.makeTemp(1);
  switch (CC) {
  case ARMCC_EQ:
    S.emit(NdOp::COPY, Cond, {NdVar::reg(armreg::ZFLAG, 1)});
    break;
  case ARMCC_NE:
    S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(armreg::ZFLAG, 1)});
    break;
  case ARMCC_HS:
    S.emit(NdOp::COPY, Cond, {NdVar::reg(armreg::CFLAG, 1)});
    break;
  case ARMCC_LO:
    S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(armreg::CFLAG, 1)});
    break;
  case ARMCC_MI:
    S.emit(NdOp::COPY, Cond, {NdVar::reg(armreg::NFLAG, 1)});
    break;
  case ARMCC_PL:
    S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(armreg::NFLAG, 1)});
    break;
  case ARMCC_VS:
    S.emit(NdOp::COPY, Cond, {NdVar::reg(armreg::VFLAG, 1)});
    break;
  case ARMCC_VC:
    S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(armreg::VFLAG, 1)});
    break;
  case ARMCC_HI: {
    NdVar NZ = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(armreg::ZFLAG, 1)});
    S.emit(NdOp::BOOL_AND, Cond, {NdVar::reg(armreg::CFLAG, 1), NZ});
    break;
  }
  case ARMCC_LS: {
    NdVar NC = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(armreg::CFLAG, 1)});
    S.emit(NdOp::BOOL_OR, Cond, {NC, NdVar::reg(armreg::ZFLAG, 1)});
    break;
  }
  case ARMCC_GE:
    S.emit(NdOp::INT_EQUAL, Cond,
           {NdVar::reg(armreg::NFLAG, 1), NdVar::reg(armreg::VFLAG, 1)});
    break;
  case ARMCC_LT:
    S.emit(NdOp::INT_NOTEQUAL, Cond,
           {NdVar::reg(armreg::NFLAG, 1), NdVar::reg(armreg::VFLAG, 1)});
    break;
  case ARMCC_GT: {
    NdVar NZ = S.makeTemp(1);
    NdVar NVEq = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(armreg::ZFLAG, 1)});
    S.emit(NdOp::INT_EQUAL, NVEq,
           {NdVar::reg(armreg::NFLAG, 1), NdVar::reg(armreg::VFLAG, 1)});
    S.emit(NdOp::BOOL_AND, Cond, {NZ, NVEq});
    break;
  }
  case ARMCC_LE: {
    NdVar NVNe = S.makeTemp(1);
    S.emit(NdOp::INT_NOTEQUAL, NVNe,
           {NdVar::reg(armreg::NFLAG, 1), NdVar::reg(armreg::VFLAG, 1)});
    S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(armreg::ZFLAG, 1), NVNe});
    break;
  }
  default:
    S.emit(NdOp::COPY, Cond, {NdVar::cst(1, 1)});
    break;
  }
  return Cond;
}

void ARMLifter::emitNZCV(LiftState &S, NdVar Result, NdVar A, NdVar B,
                         bool IsSub) {
  S.emit(NdOp::INT_SLESS, NdVar::reg(armreg::NFLAG, 1),
         {Result, NdVar::cst(0, Result.Size)});
  S.emit(NdOp::INT_EQUAL, NdVar::reg(armreg::ZFLAG, 1),
         {Result, NdVar::cst(0, Result.Size)});
  if (IsSub) {
    NdVar Borrow = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, Borrow, {A, B});
    S.emit(NdOp::BOOL_NOT, NdVar::reg(armreg::CFLAG, 1), {Borrow});
    S.emit(NdOp::INT_SBOR, NdVar::reg(armreg::VFLAG, 1), {A, B});
  } else {
    S.emit(NdOp::INT_CARRY, NdVar::reg(armreg::CFLAG, 1), {A, B});
    S.emit(NdOp::INT_SOVF, NdVar::reg(armreg::VFLAG, 1), {A, B});
  }
}

void ARMLifter::emitNZ(LiftState &S, NdVar Result) {
  S.emit(NdOp::INT_SLESS, NdVar::reg(armreg::NFLAG, 1),
         {Result, NdVar::cst(0, Result.Size)});
  S.emit(NdOp::INT_EQUAL, NdVar::reg(armreg::ZFLAG, 1),
         {Result, NdVar::cst(0, Result.Size)});
}

void ARMLifter::emitMrsNzcv(LiftState &S, NdVar Dst) {
  auto zextFlag = [&](uint64_t Off) {
    NdVar T = S.makeTemp(4);
    S.emit(NdOp::INT_ZEXT, T, {NdVar::reg(Off, 1)});
    return T;
  };
  NdVar Packed = S.makeTemp(4);
  S.emit(NdOp::INT_LEFT, Packed,
         {zextFlag(armreg::NFLAG), NdVar::cst(armreg::CpsrNBit, 4)});
  auto orShifted = [&](NdVar Acc, NdVar F, unsigned Bit) {
    NdVar Sh = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, Sh, {F, NdVar::cst(Bit, 4)});
    NdVar Out = S.makeTemp(4);
    S.emit(NdOp::INT_OR, Out, {Acc, Sh});
    return Out;
  };
  Packed = orShifted(Packed, zextFlag(armreg::ZFLAG), armreg::CpsrZBit);
  Packed = orShifted(Packed, zextFlag(armreg::CFLAG), armreg::CpsrCBit);
  Packed = orShifted(Packed, zextFlag(armreg::VFLAG), armreg::CpsrVBit);
  if (Dst.Size >= 4)
    S.emit(NdOp::COPY, Dst, {Packed});
  else
    S.emit(NdOp::SUBBYTES, Dst, {Packed, NdVar::cst(0, 4)});
}

void ARMLifter::emitMsrNzcv(LiftState &S, NdVar Src) {
  NdVar Word = Src;
  if (Src.Size < 4) {
    Word = S.makeTemp(4);
    S.emit(NdOp::INT_ZEXT, Word, {Src});
  }
  auto setFlag = [&](unsigned Bit, uint64_t Off) {
    NdVar Sh = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, Sh, {Word, NdVar::cst(Bit, 4)});
    NdVar Bit0 = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Bit0, {Sh, NdVar::cst(1, 4)});
    NdVar Lo = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, Lo, {Bit0, NdVar::cst(0, 4)});
    S.emit(NdOp::COPY, NdVar::reg(Off, 1), {Lo});
  };
  setFlag(armreg::CpsrNBit, armreg::NFLAG);
  setFlag(armreg::CpsrZBit, armreg::ZFLAG);
  setFlag(armreg::CpsrCBit, armreg::CFLAG);
  setFlag(armreg::CpsrVBit, armreg::VFLAG);
}

NdVar ARMLifter::snapForFlags(LiftState &S, const NdVar &Dst,
                                const NdVar &Op) {
  if (Dst.Space == VnodeSpace::REG && Op.Space == VnodeSpace::REG &&
      Dst.Offset == Op.Offset) {
    NdVar T = S.makeTemp(Op.Size);
    S.emit(NdOp::COPY, T, {Op});
    return T;
  }
  return Op;
}

void ARMLifter::emitRegShifterCarry(LiftState &S, unsigned ShType, NdVar Src,
                                    NdVar Amt) {
  // n = Amt[7:0].  The bit of Src that ends up in C depends on the shift type
  // (logical INT_RIGHT saturates an out-of-range amount to 0, which matches the
  // ARM "amount > 32 -> C = 0" rule for LSL/LSR/ROR; ASR clamps to bit 31 so an
  // amount >= 32 yields the sign bit):
  //   LSL: Src[32 - n]   LSR: Src[n - 1]   ASR: Src[min(n-1, 31)]
  //   ROR: Src[(n - 1) mod 32]
  NdVar N = S.makeTemp(4);
  S.emit(NdOp::INT_AND, N, {Amt, NdVar::cst(0xFF, 4)});
  NdVar Pos = S.makeTemp(4);
  if (ShType == 0) {
    S.emit(NdOp::INT_SUB, Pos, {NdVar::cst(32, 4), N});
  } else if (ShType == 1) {
    S.emit(NdOp::INT_SUB, Pos, {N, NdVar::cst(1, 4)});
  } else if (ShType == 2) {
    NdVar Nm1 = S.makeTemp(4);
    S.emit(NdOp::INT_SUB, Nm1, {N, NdVar::cst(1, 4)});
    NdVar InRange = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, InRange, {Nm1, NdVar::cst(32, 4)});
    S.emit(NdOp::SELECT, Pos, {InRange, Nm1, NdVar::cst(31, 4)});
  } else {
    NdVar Nm1 = S.makeTemp(4);
    S.emit(NdOp::INT_SUB, Nm1, {N, NdVar::cst(1, 4)});
    S.emit(NdOp::INT_AND, Pos, {Nm1, NdVar::cst(31, 4)});
  }
  NdVar Shifted = S.makeTemp(4);
  S.emit(NdOp::INT_RIGHT, Shifted, {Src, Pos});
  NdVar CBit = S.makeTemp(1);
  S.emit(NdOp::SUBBYTES, CBit, {Shifted, NdVar::cst(0, 4)});
  S.emit(NdOp::INT_AND, CBit, {CBit, NdVar::cst(1, 1)});
  // A zero shift amount leaves C unchanged.
  NdVar IsZero = S.makeTemp(1);
  S.emit(NdOp::INT_EQUAL, IsZero, {N, NdVar::cst(0, 4)});
  NdVar NewC = S.makeTemp(1);
  S.emit(NdOp::SELECT, NewC, {IsZero, NdVar::reg(armreg::CFLAG, 1), CBit});
  S.emit(NdOp::COPY, NdVar::reg(armreg::CFLAG, 1), {NewC});
}

void ARMLifter::emitLogicalOpCarry(LiftState &S, const cs_insn *Insn,
                                   const cs_arm_op &Op) {
  if (Op.type == ARM_OP_REG && Op.shift.type != ARM_SFT_INVALID) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(Op.reg));
    if (RI.Size == 0)
      return;
    NdVar Src = NdVar::reg(RI.Offset, 4);
    auto RegAmt = [&](unsigned V) {
      auto AI = mapCapstoneReg(static_cast<arm_reg>(V));
      return NdVar::reg(AI.Offset, 4);
    };
    switch (Op.shift.type) {
    case ARM_SFT_LSL:
      emitRegShifterCarry(S, 0, Src, NdVar::cst(Op.shift.value, 4));
      break;
    case ARM_SFT_LSR:
      emitRegShifterCarry(S, 1, Src, NdVar::cst(Op.shift.value, 4));
      break;
    case ARM_SFT_ASR:
      emitRegShifterCarry(S, 2, Src, NdVar::cst(Op.shift.value, 4));
      break;
    case ARM_SFT_ROR:
      emitRegShifterCarry(S, 3, Src, NdVar::cst(Op.shift.value, 4));
      break;
    case ARM_SFT_LSL_REG:
      emitRegShifterCarry(S, 0, Src, RegAmt(Op.shift.value));
      break;
    case ARM_SFT_LSR_REG:
      emitRegShifterCarry(S, 1, Src, RegAmt(Op.shift.value));
      break;
    case ARM_SFT_ASR_REG:
      emitRegShifterCarry(S, 2, Src, RegAmt(Op.shift.value));
      break;
    case ARM_SFT_ROR_REG:
      emitRegShifterCarry(S, 3, Src, RegAmt(Op.shift.value));
      break;
    case ARM_SFT_RRX:
      // RRX: shifter carry-out = Src[0].
      S.emit(NdOp::INT_AND, NdVar::reg(armreg::CFLAG, 1),
             {Src, NdVar::cst(1, 4)});
      break;
    default:
      break;
    }
    return;
  }
  if (Op.type == ARM_OP_IMM && Insn->size == 4) {
    // A modified immediate encoded with a nonzero rotation sets C to bit 31 of
    // the constant; a plain 0-255 immediate (rotation 0) leaves C unchanged.
    uint32_t Enc;
    std::memcpy(&Enc, Insn->bytes, 4);
    if (((Enc >> 25) & 0x1) == 0x1 && ((Enc >> 8) & 0xF) != 0) {
      unsigned Cbit = (static_cast<uint32_t>(Op.imm) >> 31) & 0x1;
      S.emit(NdOp::COPY, NdVar::reg(armreg::CFLAG, 1),
             {NdVar::cst(Cbit, 1)});
    }
  }
}

} // namespace neverd
