//===- ARMLiftOperands.cpp - ARM32 lifter operand helpers ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Operand decoding shared by every ARM32 category file: the barrel-shifter
/// application, register/immediate/memory reads, effective-address
/// computation and destination resolution.
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

} // namespace neverd
