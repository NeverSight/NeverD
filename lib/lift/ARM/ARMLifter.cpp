//===- ARMLifter.cpp - ARM32 lifter dispatch & helpers ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Main dispatch for ARM32 instruction lifting, plus operand helpers and
/// flag/condition-code emission shared by every category file.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/ARMLifter.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

// ===----------------------------------------------------------------------===//
// ARMLifter construction
// ===----------------------------------------------------------------------===//

ARMLifter::ARMLifter(Arch A) : TargetArch(A) {}

// ===----------------------------------------------------------------------===//
// Operand read / write
// ===----------------------------------------------------------------------===//

NdVar ARMLifter::emitImmShift(LiftState &S, NdVar Val, unsigned ShType,
                                unsigned ShVal) {
  switch (ShType) {
  case ARM_SFT_LSL: {
    NdVar R = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, R, {Val, NdVar::cst(ShVal, 4)});
    return R;
  }
  case ARM_SFT_LSR: {
    NdVar R = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, R, {Val, NdVar::cst(ShVal, 4)});
    return R;
  }
  case ARM_SFT_ASR: {
    NdVar R = S.makeTemp(4);
    S.emit(NdOp::INT_ASHR, R, {Val, NdVar::cst(ShVal, 4)});
    return R;
  }
  case ARM_SFT_ROR: {
    // ROR #n = (Val >> n) | (Val << (32 - n)), n in 1..31.  The addressing
    // paths previously emitted a plain INT_LEFT here (treating ROR as LSL).
    NdVar Lo = S.makeTemp(4), Hi = S.makeTemp(4), R = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, Lo, {Val, NdVar::cst(ShVal, 4)});
    S.emit(NdOp::INT_LEFT, Hi, {Val, NdVar::cst(32 - ShVal, 4)});
    S.emit(NdOp::INT_OR, R, {Lo, Hi});
    return R;
  }
  case ARM_SFT_RRX: {
    // RRX = (Val >> 1) | (C << 31): rotate right through carry by one.  Was
    // dropped entirely (the operand passed through unshifted / as INT_LEFT).
    NdVar Lo = S.makeTemp(4), C32 = S.makeTemp(4), Hi = S.makeTemp(4),
            R = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, Lo, {Val, NdVar::cst(1, 4)});
    S.emit(NdOp::INT_ZEXT, C32, {NdVar::reg(armreg::CFLAG, 1)});
    S.emit(NdOp::INT_LEFT, Hi, {C32, NdVar::cst(31, 4)});
    S.emit(NdOp::INT_OR, R, {Lo, Hi});
    return R;
  }
  default:
    return Val;
  }
}

NdVar ARMLifter::operandRead(LiftState &S, const cs_arm_op &Op) {
  auto ApplyShift = [&](NdVar Val, const cs_arm_op &O) -> NdVar {
    // RRX carries no shift amount (capstone reports value 0) but must still
    // rotate right through carry; every other type is a no-op at amount 0.
    if (O.shift.type == ARM_SFT_INVALID ||
        (O.shift.value == 0 && O.shift.type != ARM_SFT_RRX))
      return Val;
    if (O.shift.type == ARM_SFT_RRX)
      return emitImmShift(S, Val, ARM_SFT_RRX, 0);

    bool IsRegShift =
        (O.shift.type == ARM_SFT_LSL_REG || O.shift.type == ARM_SFT_LSR_REG ||
         O.shift.type == ARM_SFT_ASR_REG || O.shift.type == ARM_SFT_ROR_REG);

    NdVar ShiftAmt;
    if (IsRegShift) {
      auto RI = mapCapstoneReg(static_cast<arm_reg>(O.shift.value));
      if (RI.Size == 0) {
        ShiftAmt = NdVar::cst(0, 4);
      } else {
        NdVar Raw = NdVar::reg(RI.Offset, 4);
        ShiftAmt = S.makeTemp(4);
        // ARM32 register shift uses Rs[7:0]; LSL/LSR/ASR amounts >= 32 saturate
        // (via the shift ops) so mask the full byte, only ROR is mod 32.
        uint64_t AmtMask = (O.shift.type == ARM_SFT_ROR_REG) ? 31 : 0xFF;
        S.emit(NdOp::INT_AND, ShiftAmt, {Raw, NdVar::cst(AmtMask, 4)});
      }
    } else {
      ShiftAmt = NdVar::cst(O.shift.value, Val.Size);
    }

    NdVar Shifted = S.makeTemp(Val.Size);
    NdOp ShiftOp = NdOp::INT_LEFT;
    switch (O.shift.type) {
    case ARM_SFT_LSL:
    case ARM_SFT_LSL_REG:
      ShiftOp = NdOp::INT_LEFT;
      break;
    case ARM_SFT_LSR:
    case ARM_SFT_LSR_REG:
      ShiftOp = NdOp::INT_RIGHT;
      break;
    case ARM_SFT_ASR:
    case ARM_SFT_ASR_REG:
      ShiftOp = NdOp::INT_ASHR;
      break;
    case ARM_SFT_ROR:
    case ARM_SFT_ROR_REG: {
      NdVar Lo = S.makeTemp(4);
      NdVar Hi = S.makeTemp(4);
      S.emit(NdOp::INT_RIGHT, Lo, {Val, ShiftAmt});
      if (IsRegShift) {
        NdVar Inv = S.makeTemp(4);
        S.emit(NdOp::INT_SUB, Inv, {NdVar::cst(32, 4), ShiftAmt});
        S.emit(NdOp::INT_LEFT, Hi, {Val, Inv});
      } else {
        S.emit(NdOp::INT_LEFT, Hi, {Val, NdVar::cst(32 - O.shift.value, 4)});
      }
      S.emit(NdOp::INT_OR, Shifted, {Lo, Hi});
      return Shifted;
    }
    default:
      return Val;
    }
    S.emit(ShiftOp, Shifted, {Val, ShiftAmt});
    return Shifted;
  };

  switch (Op.type) {
  case ARM_OP_REG: {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(Op.reg));
    if (RI.Size == 0)
      return NdVar::cst(0, 4);
    NdVar RegVal = NdVar::reg(RI.Offset, RI.Size);
    return ApplyShift(RegVal, Op);
  }
  case ARM_OP_IMM: {
    uint64_t Val = static_cast<uint64_t>(static_cast<uint32_t>(Op.imm));
    return NdVar::cst(Val, 4);
  }
  case ARM_OP_MEM: {
    NdVar EA = S.makeTemp(4);
    bool First = true;
    auto Acc = [&](NdVar V) {
      if (First) {
        S.emit(NdOp::COPY, EA, {V});
        First = false;
      } else {
        S.emit(NdOp::INT_ADD, EA, {EA, V});
      }
    };
    if (Op.mem.base != ARM_REG_INVALID) {
      auto RI = mapCapstoneReg(static_cast<arm_reg>(Op.mem.base));
      Acc(NdVar::reg(RI.Offset, 4));
    }
    if (Op.mem.index != ARM_REG_INVALID) {
      auto RI = mapCapstoneReg(static_cast<arm_reg>(Op.mem.index));
      NdVar IdxVal = NdVar::reg(RI.Offset, 4);
      if (Op.mem.scale != 1 && Op.mem.scale != 0) {
        NdVar Scaled = S.makeTemp(4);
        S.emit(NdOp::INT_MULT, Scaled, {IdxVal, NdVar::cst(Op.mem.scale, 4)});
        IdxVal = Scaled;
      }
      IdxVal = ApplyShift(IdxVal, Op);
      if (Op.subtracted)
        Acc(NdVar::cst(0, 4));
      Acc(Op.subtracted ? [&]() -> NdVar {
        NdVar Neg = S.makeTemp(4);
        S.emit(NdOp::INT_NEG2, Neg, {IdxVal});
        return Neg;
      }()
                        : IdxVal);
    }
    if (Op.mem.disp != 0) {
      int64_t SignedDisp = Op.mem.disp;
      if (Op.subtracted)
        SignedDisp = -(SignedDisp < 0 ? -SignedDisp : SignedDisp);
      Acc(NdVar::cst(static_cast<uint64_t>(SignedDisp), 4));
    }
    if (First)
      Acc(NdVar::cst(0, 4));
    NdVar Result = S.makeTemp(4);
    S.emit(NdOp::LOAD, Result, {EA});
    return Result;
  }
  case ARM_OP_FP: {
    double FPVal = Op.fp;
    union {
      double D;
      float F;
      uint64_t I64;
      uint32_t I32;
    } U;
    U.I64 = 0;
    U.D = FPVal;
    return NdVar::cst(U.I64, 8);
  }
  default:
    return NdVar::cst(0, 4);
  }
}

NdVar ARMLifter::operandEffAddr(LiftState &S, const cs_arm_op &Op) {
  assert(Op.type == ARM_OP_MEM && "operandEffAddr requires MEM operand");
  NdVar EA = S.makeTemp(4);
  bool First = true;
  auto Acc = [&](NdVar V) {
    if (First) {
      S.emit(NdOp::COPY, EA, {V});
      First = false;
    } else {
      S.emit(NdOp::INT_ADD, EA, {EA, V});
    }
  };
  if (Op.mem.base != ARM_REG_INVALID) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(Op.mem.base));
    Acc(NdVar::reg(RI.Offset, 4));
  }
  if (Op.mem.index != ARM_REG_INVALID) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(Op.mem.index));
    NdVar IdxVal = NdVar::reg(RI.Offset, 4);
    if (Op.mem.scale != 1 && Op.mem.scale != 0) {
      NdVar Scaled = S.makeTemp(4);
      S.emit(NdOp::INT_MULT, Scaled, {IdxVal, NdVar::cst(Op.mem.scale, 4)});
      IdxVal = Scaled;
    }
    // ROR/RRX index shifts must rotate, not left-shift (the old code mapped
    // anything that was not LSR/ASR to INT_LEFT, so `[Rn, Rm, ror #k]` scaled
    // the index by a left shift).
    if (Op.shift.type != ARM_SFT_INVALID &&
        (Op.shift.value != 0 || Op.shift.type == ARM_SFT_RRX))
      IdxVal = emitImmShift(S, IdxVal, Op.shift.type, Op.shift.value);
    if (Op.subtracted) {
      NdVar Neg = S.makeTemp(4);
      S.emit(NdOp::INT_NEG2, Neg, {IdxVal});
      Acc(Neg);
    } else {
      Acc(IdxVal);
    }
  }
  if (Op.mem.disp != 0) {
    int64_t SignedDisp = Op.mem.disp;
    if (Op.subtracted)
      SignedDisp = -(SignedDisp < 0 ? -SignedDisp : SignedDisp);
    Acc(NdVar::cst(static_cast<uint64_t>(SignedDisp), 4));
  }
  if (First)
    Acc(NdVar::cst(0, 4));
  return EA;
}

NdVar ARMLifter::operandWrite(const cs_arm_op &Op) {
  if (Op.type == ARM_OP_REG) {
    auto RI = mapCapstoneReg(static_cast<arm_reg>(Op.reg));
    if (RI.Size == 0)
      return NdVar::cst(0, 4);
    return NdVar::reg(RI.Offset, RI.Size);
  }
  return NdVar::ram(0, 4);
}

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

// ===----------------------------------------------------------------------===//
// Main dispatch
// ===----------------------------------------------------------------------===//

void ARMLifter::lift(const cs_insn *Insn, std::vector<LowOp> &Ops) {
  auto *Detail = Insn->detail;
  if (!Detail)
    return;

  auto &ARM = Detail->arm;
  LiftState S(Insn->address, static_cast<uint16_t>(Insn->size), Ops);

  // ARM32 pipeline: PC reads as current_addr + 8 during execution.
  S.emit(NdOp::COPY, NdVar::reg(armreg::PC, 4),
         {NdVar::cst(Insn->address + 8, 4)});

  // A genuinely predicated instruction carries a real condition (EQ..LE).
  // ARMCC_AL and the "execute always" sentinel capstone surfaces for some
  // unconditional forms (BL/BLX immediates and BX/BLX-register both report one
  // past ARMCC_AL) all mean unconditional, so the EQ..LE range test catches
  // every spelling.  Calls must be included: an unconditional BL wrapped in a
  // never-taken predicate guard emits a mid-instruction COND_BR, after which
  // the emitter drops the remaining ops — silently deleting the CALL itself
  // (e.g. a self-recursive `bl`).  The same guard would suppress a BX indirect
  // branch's computed-goto recovery.
  bool IsCond = (ARM.cc >= ARMCC_EQ && ARM.cc <= ARMCC_LE);
  NdVar ArmCondVar;
  bool IsBranchInsn = (Insn->id == ARM_INS_B || Insn->id == ARM_INS_BL ||
                       Insn->id == ARM_INS_BX || Insn->id == ARM_INS_BLX ||
                       Insn->id == ARM_INS_CBZ || Insn->id == ARM_INS_CBNZ);

  if (IsCond && IsBranchInsn) {
    NdVar Cond = buildCondCode(ARM.cc, S);
    NdVar InvCond = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, InvCond, {Cond});
    va_t NextAddr = S.Addr + Insn->size;
    S.emit(NdOp::COND_BR, {}, {NdVar::cst(NextAddr, 4), InvCond});
  } else if (IsCond && !IsBranchInsn) {
    ArmCondVar = buildCondCode(ARM.cc, S);
  }

  size_t CondOpsStart = Ops.size();

  bool Handled = liftCore(S, Insn, ARM) || liftCoreExt(S, Insn, ARM) ||
                 liftControl(S, Insn, ARM) || liftMem(S, Insn, ARM) ||
                 liftMul(S, Insn, ARM) || liftSIMD(S, Insn, ARM) ||
                 liftSIMDNEON(S, Insn, ARM);

  if (!Handled) {
    if (Strict) {
      Ops.resize(S.OpsStart);
      throw UnliftedInstruction(S.Addr, Insn->mnemonic, Insn->op_str);
    }
    LLVM_DEBUG(llvm::dbgs()
               << "ARM: unlifted " << Insn->mnemonic << " " << Insn->op_str
               << " at 0x" << llvm::utohexstr(S.Addr) << "\n");
    S.emit(NdOp::NOP, {}, {});
  }

  // Conditional non-branch wrapping.  Every predicated effect is modeled
  // branchlessly, so no COND_BR appears in the middle of an instruction's op
  // list: the CFG builder works at instruction granularity and cannot split a
  // block at such a COND_BR, so any op after it (e.g. a predicated store) would
  // be silently dropped by the emitter.
  //   * register/flag write `Out = expr`  →  `Out = cond ? expr : Out`
  //   * memory access `M[addr]`             →  `M[cond ? addr : SP]`
  //   * memory store `M[addr] = val`        →  `M[safe] = cond ? val : M[safe]`
  //     (read-modify-write)
  // A false predicated load/store must not even dereference its architectural
  // address: ARM code commonly leaves that address invalid on the untaken path
  // (for example `ldrne r2, [r0, #-60]` where r0 is unrelated when Z is set).
  // Keep the branchless representation, but redirect the speculative access to
  // the ABI-valid stack pointer.  The register SELECT below discards a false
  // load and the read-modify-write preserves the fallback word for a false
  // store.
  if (IsCond && !IsBranchInsn && ArmCondVar.Size > 0) {
    // Guard the address of every memory effect before wrapping its result.  Do
    // this as a separate pass so insertion does not interfere with the
    // register/store transformations below.
    for (size_t I = CondOpsStart; I < Ops.size(); ++I) {
      auto &Op = Ops[I];
      if ((Op.Opcode != NdOp::LOAD && Op.Opcode != NdOp::STORE) ||
          Op.NumInputs == 0)
        continue;
      NdVar SafeEA = S.makeTemp(4);
      LowOp SelEA;
      SelEA.Opcode = NdOp::SELECT;
      SelEA.Addr = S.Addr;
      SelEA.Output = SafeEA;
      SelEA.addInput(ArmCondVar);
      SelEA.addInput(Op.Inputs[0]);
      SelEA.addInput(NdVar::reg(armreg::SP, 4));
      Op.Inputs[0] = SafeEA;
      Ops.insert(Ops.begin() + static_cast<long>(I), SelEA);
      ++I;
    }

    // Wrap each register/flag write `Out = expr` as `Out = cond ? expr : Out`.
    // The SELECT is inserted immediately after the defining op (not appended
    // at the end) so later ops in the same instruction that read `Out` observe
    // the predicated value — this matters for flag-setting instructions whose
    // N/Z flags read the result register.
    llvm::DenseSet<uint64_t> SeenRegs;
    for (size_t I = CondOpsStart; I < Ops.size(); ++I) {
      auto &Op = Ops[I];
      if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2) {
        NdVar EA = Op.Inputs[0];
        NdVar Val = Op.Inputs[1];
        NdVar Old = S.makeTemp(Val.Size);
        NdVar Merged = S.makeTemp(Val.Size);
        Op.Inputs[1] = Merged;
        LowOp LoadOld;
        LoadOld.Opcode = NdOp::LOAD;
        LoadOld.Addr = S.Addr;
        LoadOld.Output = Old;
        LoadOld.addInput(EA);
        LowOp SelVal;
        SelVal.Opcode = NdOp::SELECT;
        SelVal.Addr = S.Addr;
        SelVal.Output = Merged;
        SelVal.addInput(ArmCondVar);
        SelVal.addInput(Val);
        SelVal.addInput(Old);
        Ops.insert(Ops.begin() + static_cast<long>(I), SelVal);
        Ops.insert(Ops.begin() + static_cast<long>(I), LoadOld);
        I += 2; // step over the inserted LOAD + SELECT
        continue;
      }
      if (Op.Output.Space == VnodeSpace::REG && Op.Output.Size > 0 &&
          Op.Output.Offset < armreg::RegSpaceEnd &&
          SeenRegs.insert(Op.Output.Offset).second) {
        NdVar OldVal = NdVar::reg(Op.Output.Offset, Op.Output.Size);
        NdVar NewVal = S.makeTemp(Op.Output.Size);
        NdVar FinalDst = Op.Output;
        Op.Output = NewVal;
        LowOp Sel;
        Sel.Opcode = NdOp::SELECT;
        Sel.Addr = S.Addr;
        Sel.Seq = 0;
        Sel.Output = FinalDst;
        Sel.addInput(ArmCondVar);
        Sel.addInput(NewVal);
        Sel.addInput(OldVal);
        Ops.insert(Ops.begin() + static_cast<long>(I) + 1, Sel);
        ++I; // step over the inserted SELECT
      }
    }
  }

  // An arithmetic write to PC (e.g. `add pc, base, table[idx]`) is an
  // indirect branch (jump-table dispatch).  COPY-to-PC (`mov pc, lr`) and
  // LOAD-to-PC (`ldr pc, ...`) are returns handled by their own lifters, so
  // only a computed PC write becomes an INDIR_BR.
  if (!IsCond) {
    bool HasTerminator = false;
    bool ArithPCWrite = false;
    for (auto &Op : Ops) {
      switch (Op.Opcode) {
      case NdOp::BRANCH:
      case NdOp::COND_BR:
      case NdOp::INDIR_BR:
      case NdOp::CALL:
      case NdOp::INDIR_CALL:
      case NdOp::RETURN:
        HasTerminator = true;
        break;
      case NdOp::INT_ADD:
      case NdOp::INT_SUB:
      case NdOp::INT_OR:
      case NdOp::INT_XOR:
      case NdOp::INT_AND:
      case NdOp::INT_MULT:
      case NdOp::INT_LEFT:
      case NdOp::INT_RIGHT:
      case NdOp::INT_ASHR:
        if (Op.Output.isReg() && Op.Output.Offset == armreg::PC)
          ArithPCWrite = true;
        break;
      default:
        break;
      }
    }
    if (ArithPCWrite && !HasTerminator)
      S.emit(NdOp::INDIR_BR, {}, {NdVar::reg(armreg::PC, 4)});
  }
}

// ===----------------------------------------------------------------------===//
// Decode-time instruction classification
// ===----------------------------------------------------------------------===//

void ARMLifter::fixupDecodedInsn(cs_insn * /*I*/) {
  // No capstone decode-id quirks to correct for ARM (yet).
}

bool ARMLifter::isFunctionTerminator(const cs_insn *I) {
  switch (I->id) {
  case ARM_INS_BX:
  case ARM_INS_B:
  case ARM_INS_POP:
    return true;
  default:
    return false;
  }
}

va_t ARMLifter::directCallTarget(const cs_insn *I) {
  if (!I->detail)
    return InvalidVA;
  const cs_arm &A = I->detail->arm;
  if ((I->id == ARM_INS_BL || I->id == ARM_INS_BLX) && A.op_count >= 1 &&
      A.operands[0].type == ARM_OP_IMM)
    return static_cast<va_t>(A.operands[0].imm);
  return InvalidVA;
}

} // namespace neverd
