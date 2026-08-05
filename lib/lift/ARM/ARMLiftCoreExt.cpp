//===- ARMLiftCoreExt.cpp - ARM32 extended integer instruction lifter -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Additional ARM32 integer instruction handlers: ADR, BFC, BFI, REV16,
/// REVSH, RBIT, RRX, PKH*, SDIV, UDIV, SEL, and remaining
/// integer/system instructions.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARMLifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool ARMLifter::liftCoreExt(LiftState &S, const cs_insn *Insn,
                            const cs_arm &ARM) {
  switch (Insn->id) {

  // ========================================================================
  // Additional integer: ADR, BFC, BFI, REV16, REVSH, RBIT, RRX, CLZ variants
  // ========================================================================
  case ARM_INS_ADR: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }
  case ARM_INS_BFC: {
    // BFC Rd, #LSB, #width — clear Bits [LSB+width-1:LSB] in Rd
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    uint32_t LSB = static_cast<uint32_t>(ARM.operands[1].imm);
    uint32_t Width = static_cast<uint32_t>(ARM.operands[2].imm);
    if (Width == 0 || Width > 32 || LSB >= 32 || Width > 32 - LSB)
      break;
    uint32_t FieldMask =
        Width == 32 ? ~0u : ((1u << Width) - 1) << LSB;
    uint32_t Mask = ~FieldMask;
    S.emit(NdOp::INT_AND, Dst,
           {NdVar::reg(Dst.Offset, 4), NdVar::cst(Mask, 4)});
    break;
  }
  case ARM_INS_BFI: {
    // BFI Rd, Rn, #LSB, #width — insert Width Bits from Rn[width-1:0] into
    // Rd[LSB+width-1:LSB]
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    uint32_t LSB = static_cast<uint32_t>(ARM.operands[2].imm);
    uint32_t Width = static_cast<uint32_t>(ARM.operands[3].imm);
    if (Width == 0 || Width > 32 || LSB >= 32 || Width > 32 - LSB)
      break;
    uint32_t FieldMask =
        Width == 32 ? ~0u : ((1u << Width) - 1) << LSB;
    NdVar ShiftedSrc = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, ShiftedSrc, {Src, NdVar::cst(LSB, 4)});
    NdVar MaskedSrc = S.makeTemp(4);
    S.emit(NdOp::INT_AND, MaskedSrc, {ShiftedSrc, NdVar::cst(FieldMask, 4)});
    NdVar ClearedDst = S.makeTemp(4);
    S.emit(NdOp::INT_AND, ClearedDst,
           {NdVar::reg(Dst.Offset, 4), NdVar::cst(~FieldMask, 4)});
    S.emit(NdOp::INT_OR, Dst, {ClearedDst, MaskedSrc});
    break;
  }
  case ARM_INS_REV16: {
    // REV16 — reverse bytes within each 16-bit halfword
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    // Low halfword: swap bytes 0,1
    NdVar LoHi = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, LoHi, {Src, NdVar::cst(8, 4)});
    S.emit(NdOp::INT_AND, LoHi, {LoHi, NdVar::cst(0xFF00, 4)});
    NdVar LoLo = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, LoLo, {Src, NdVar::cst(8, 4)});
    S.emit(NdOp::INT_AND, LoLo, {LoLo, NdVar::cst(0x00FF, 4)});
    NdVar Lo = S.makeTemp(4);
    S.emit(NdOp::INT_OR, Lo, {LoHi, LoLo});
    // High halfword: swap bytes 2,3
    NdVar HiHi = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, HiHi, {Src, NdVar::cst(8, 4)});
    S.emit(NdOp::INT_AND, HiHi, {HiHi, NdVar::cst(0xFF000000u, 4)});
    NdVar HiLo = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, HiLo, {Src, NdVar::cst(8, 4)});
    S.emit(NdOp::INT_AND, HiLo, {HiLo, NdVar::cst(0x00FF0000u, 4)});
    NdVar Hi = S.makeTemp(4);
    S.emit(NdOp::INT_OR, Hi, {HiHi, HiLo});
    S.emit(NdOp::INT_OR, Dst, {Lo, Hi});
    break;
  }
  case ARM_INS_REVSH: {
    // REVSH — reverse bytes in low halfword, sign-extend to 32 Bits
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    NdVar Byte0 = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Byte0, {Src, NdVar::cst(0xFF, 4)});
    NdVar Byte0_shifted = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, Byte0_shifted, {Byte0, NdVar::cst(8, 4)});
    NdVar Byte1 = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, Byte1, {Src, NdVar::cst(8, 4)});
    S.emit(NdOp::INT_AND, Byte1, {Byte1, NdVar::cst(0xFF, 4)});
    NdVar Swapped16 = S.makeTemp(4);
    S.emit(NdOp::INT_OR, Swapped16, {Byte0_shifted, Byte1});
    NdVar Trunc = S.makeTemp(2);
    S.emit(NdOp::SUBBYTES, Trunc, {Swapped16, NdVar::cst(0, 4)});
    S.emit(NdOp::INT_SEXT, Dst, {Trunc});
    break;
  }
  case ARM_INS_RBIT: {
    // RBIT — reverse all 32 Bits (use intrinsic for exact semantics)
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    S.emitIntrinsic(Intrinsic::ArmRbit, Dst, {Src});
    break;
  }
  case ARM_INS_RRX: {
    // RRX — rotate right by 1 through carry: Result = (CF << 31) | (Src >> 1)
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    NdVar CfExt = S.makeTemp(4);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(armreg::CFLAG, 1)});
    NdVar CfShifted = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, CfShifted, {CfExt, NdVar::cst(31, 4)});
    NdVar SrcShifted = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, SrcShifted, {Src, NdVar::cst(1, 4)});
    S.emit(NdOp::INT_OR, Dst, {CfShifted, SrcShifted});
    if (ARM.update_flags) {
      NdVar Bit0Word = S.makeTemp(4);
      S.emit(NdOp::INT_AND, Bit0Word, {Src, NdVar::cst(1, 4)});
      S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(armreg::CFLAG, 1),
             {Bit0Word, NdVar::cst(0, 4)});
    }
    break;
  }
  case ARM_INS_RSC: {
    // RSC — Reverse Subtract with Carry: Dst = NOT(a) + b + C
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    if (ARM.update_flags) {
      // Snapshot sources before the result write so the carry/overflow flags do
      // not alias-resolve to the post-write Dst (rscs rD,rN,rD).
      A = snapForFlags(S, Dst, A);
      B = snapForFlags(S, Dst, B);
    }
    NdVar NotA = S.makeTemp(4);
    S.emit(NdOp::INT_NOT, NotA, {A});
    NdVar Sum = S.makeTemp(4);
    S.emit(NdOp::INT_ADD, Sum, {B, NotA});
    NdVar CfExt = S.makeTemp(4);
    S.emit(NdOp::INT_ZEXT, CfExt, {NdVar::reg(armreg::CFLAG, 1)});
    S.emit(NdOp::INT_ADD, Dst, {Sum, CfExt});
    if (ARM.update_flags) {
      S.emit(NdOp::INT_SLESS, NdVar::reg(armreg::NFLAG, 1),
             {Dst, NdVar::cst(0, 4)});
      S.emit(NdOp::INT_EQUAL, NdVar::reg(armreg::ZFLAG, 1),
             {Dst, NdVar::cst(0, 4)});
      NdVar C1 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C1, {B, NotA});
      NdVar C2 = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, C2, {Sum, CfExt});
      S.emit(NdOp::BOOL_OR, NdVar::reg(armreg::CFLAG, 1), {C1, C2});
      NdVar V1 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V1, {B, NotA});
      NdVar V2 = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, V2, {Sum, CfExt});
      S.emit(NdOp::BOOL_XOR, NdVar::reg(armreg::VFLAG, 1), {V1, V2});
    }
    break;
  }

  // SXTAB: Rd = Rn + SignExtend(Byte(Rm)); SXTAH: Rd = Rn +
  // SignExtend(Half(Rm))
  case ARM_INS_SXTAB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Base = operandRead(S, ARM.operands[1]);
    NdVar Src = operandRead(S, ARM.operands[2]);
    NdVar ByteV = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, ByteV, {Src, NdVar::cst(0, 4)});
    NdVar Ext = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, Ext, {ByteV});
    S.emit(NdOp::INT_ADD, Dst, {Base, Ext});
    break;
  }
  case ARM_INS_SXTAH: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Base = operandRead(S, ARM.operands[1]);
    NdVar Src = operandRead(S, ARM.operands[2]);
    NdVar HalfV = S.makeTemp(2);
    S.emit(NdOp::SUBBYTES, HalfV, {Src, NdVar::cst(0, 4)});
    NdVar Ext = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, Ext, {HalfV});
    S.emit(NdOp::INT_ADD, Dst, {Base, Ext});
    break;
  }
  case ARM_INS_SXTAB16:
  case ARM_INS_SXTB16: {
    // SXTB16: sign-extend Byte[0]→halfword[0], Byte[2]→halfword[1]
    // SXTAB16: Rd = Rn + SXTB16(Rm) (packed halfword add)
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[ARM.op_count - 1]);
    // Extract Byte[0], sign-extend to 16 Bits, pack into low halfword
    NdVar B0 = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, B0, {Src, NdVar::cst(0, 4)});
    NdVar Ext0 = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, Ext0, {B0});
    NdVar Lo = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Lo, {Ext0, NdVar::cst(0xFFFF, 4)});
    // Extract Byte[2], sign-extend to 16 Bits, pack into high halfword
    NdVar Shifted = S.makeTemp(4);
    S.emit(NdOp::INT_RIGHT, Shifted, {Src, NdVar::cst(16, 4)});
    NdVar B2 = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, B2, {Shifted, NdVar::cst(0, 4)});
    NdVar Ext2 = S.makeTemp(4);
    S.emit(NdOp::INT_SEXT, Ext2, {B2});
    NdVar Hi = S.makeTemp(4);
    S.emit(NdOp::INT_LEFT, Hi, {Ext2, NdVar::cst(16, 4)});
    NdVar Result = S.makeTemp(4);
    S.emit(NdOp::INT_OR, Result, {Lo, Hi});
    if (Insn->id == ARM_INS_SXTAB16 && ARM.op_count >= 3) {
      NdVar Base = operandRead(S, ARM.operands[1]);
      // Packed halfword add: each halfword independently
      NdVar BaseLo = S.makeTemp(4);
      S.emit(NdOp::INT_AND, BaseLo, {Base, NdVar::cst(0xFFFF, 4)});
      NdVar BaseHi = S.makeTemp(4);
      S.emit(NdOp::INT_AND, BaseHi, {Base, NdVar::cst(0xFFFF0000u, 4)});
      NdVar SumLo = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, SumLo, {BaseLo, Lo});
      S.emit(NdOp::INT_AND, SumLo, {SumLo, NdVar::cst(0xFFFF, 4)});
      NdVar SumHi = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, SumHi, {BaseHi, Hi});
      S.emit(NdOp::INT_AND, SumHi, {SumHi, NdVar::cst(0xFFFF0000u, 4)});
      S.emit(NdOp::INT_OR, Dst, {SumLo, SumHi});
    } else {
      S.emit(NdOp::COPY, Dst, {Result});
    }
    break;
  }
  // UXTAB: Rd = Rn + ZeroExtend(Byte(Rm)); UXTAH: Rd = Rn +
  // ZeroExtend(Half(Rm))
  case ARM_INS_UXTAB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Base = operandRead(S, ARM.operands[1]);
    NdVar Src = operandRead(S, ARM.operands[2]);
    NdVar Masked = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Masked, {Src, NdVar::cst(0xFF, 4)});
    S.emit(NdOp::INT_ADD, Dst, {Base, Masked});
    break;
  }
  case ARM_INS_UXTAH: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Base = operandRead(S, ARM.operands[1]);
    NdVar Src = operandRead(S, ARM.operands[2]);
    NdVar Masked = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Masked, {Src, NdVar::cst(0xFFFF, 4)});
    S.emit(NdOp::INT_ADD, Dst, {Base, Masked});
    break;
  }
  case ARM_INS_UXTAB16:
  case ARM_INS_UXTB16: {
    // UXTB16: Zero-extend Byte[0]→halfword[0], Byte[2]→halfword[1]
    // UXTAB16: Rd = Rn + UXTB16(Rm) (packed halfword add)
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[ARM.op_count - 1]);
    NdVar Lo = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Lo, {Src, NdVar::cst(0xFF, 4)});
    NdVar HiByte = S.makeTemp(4);
    S.emit(NdOp::INT_AND, HiByte, {Src, NdVar::cst(0x00FF0000u, 4)});
    NdVar Result = S.makeTemp(4);
    S.emit(NdOp::INT_OR, Result, {Lo, HiByte});
    if (Insn->id == ARM_INS_UXTAB16 && ARM.op_count >= 3) {
      NdVar Base = operandRead(S, ARM.operands[1]);
      NdVar BaseLo = S.makeTemp(4);
      S.emit(NdOp::INT_AND, BaseLo, {Base, NdVar::cst(0xFFFF, 4)});
      NdVar BaseHi = S.makeTemp(4);
      S.emit(NdOp::INT_AND, BaseHi, {Base, NdVar::cst(0xFFFF0000u, 4)});
      NdVar SumLo = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, SumLo, {BaseLo, Lo});
      S.emit(NdOp::INT_AND, SumLo, {SumLo, NdVar::cst(0xFFFF, 4)});
      NdVar SumHi = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, SumHi, {BaseHi, HiByte});
      S.emit(NdOp::INT_AND, SumHi, {SumHi, NdVar::cst(0xFFFF0000u, 4)});
      S.emit(NdOp::INT_OR, Dst, {SumLo, SumHi});
    } else {
      S.emit(NdOp::COPY, Dst, {Result});
    }
    break;
  }

  // Saturating arithmetic (QADD/QSUB/QDADD/QDSUB).  Compute in 64 bits and
  // clamp to the signed 32-bit range, then truncate — no library/intrinsic
  // dependency, semantics exact.
  case ARM_INS_QADD:
  case ARM_INS_QDADD:
  case ARM_INS_QSUB:
  case ARM_INS_QDSUB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]); // Rm
    NdVar B = operandRead(S, ARM.operands[2]); // Rn
    bool IsSub = (Insn->id == ARM_INS_QSUB || Insn->id == ARM_INS_QDSUB);
    bool IsDouble = (Insn->id == ARM_INS_QDADD || Insn->id == ARM_INS_QDSUB);

    // Clamp a 64-bit signed value to [INT32_MIN, INT32_MAX], yield 4 bytes.
    auto sat32 = [&](NdVar Wide) -> NdVar {
      NdVar MaxC = NdVar::cst(0x7FFFFFFFull, 8);
      NdVar MinC = NdVar::cst(static_cast<uint64_t>(-(1LL << 31)), 8);
      NdVar GtMax = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, GtMax, {MaxC, Wide});
      NdVar C1 = S.makeTemp(8);
      S.emit(NdOp::SELECT, C1, {GtMax, MaxC, Wide});
      NdVar LtMin = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, LtMin, {C1, MinC});
      NdVar C2 = S.makeTemp(8);
      S.emit(NdOp::SELECT, C2, {LtMin, MinC, C1});
      NdVar Narrow = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Narrow, {C2, NdVar::cst(0, 4)});
      return Narrow;
    };

    NdVar WA = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, WA, {A});
    NdVar Addend = S.makeTemp(8);
    S.emit(NdOp::INT_SEXT, Addend, {B});
    if (IsDouble) {
      // doubled = SignedSat(2*Rn, 32), then sign-extend back to 64 bits.
      NdVar Dbl = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, Dbl, {Addend, Addend});
      NdVar DblSat = sat32(Dbl);
      Addend = S.makeTemp(8);
      S.emit(NdOp::INT_SEXT, Addend, {DblSat});
    }
    NdVar Sum = S.makeTemp(8);
    S.emit(IsSub ? NdOp::INT_SUB : NdOp::INT_ADD, Sum, {WA, Addend});
    S.emit(NdOp::COPY, Dst, {sat32(Sum)});
    break;
  }
  // SIMD lane-parallel add/sub (ARMv6 GE-setting forms): 16-bit (2 lanes) or
  // 8-bit (4 lanes) packed in a 32-bit GPR, with plain / saturating (Q) /
  // halving (H) variants, plus the add-subtract-with-exchange forms.  ASX swaps
  // the second operand's halves with low=sub/high=add; SAX with
  // low=add/high=sub.
  case ARM_INS_QADD16:
  case ARM_INS_UQADD16:
  case ARM_INS_QADD8:
  case ARM_INS_UQADD8:
  case ARM_INS_QASX:
  case ARM_INS_UQASX:
  case ARM_INS_SADD16:
  case ARM_INS_UADD16:
  case ARM_INS_SADD8:
  case ARM_INS_UADD8:
  case ARM_INS_SASX:
  case ARM_INS_UASX:
  case ARM_INS_SHADD16:
  case ARM_INS_UHADD16:
  case ARM_INS_SHADD8:
  case ARM_INS_UHADD8:
  case ARM_INS_SHASX:
  case ARM_INS_UHASX:
  case ARM_INS_QSUB16:
  case ARM_INS_UQSUB16:
  case ARM_INS_QSUB8:
  case ARM_INS_UQSUB8:
  case ARM_INS_QSAX:
  case ARM_INS_UQSAX:
  case ARM_INS_SSUB16:
  case ARM_INS_USUB16:
  case ARM_INS_SSUB8:
  case ARM_INS_USUB8:
  case ARM_INS_SSAX:
  case ARM_INS_USAX:
  case ARM_INS_SHSUB16:
  case ARM_INS_UHSUB16:
  case ARM_INS_SHSUB8:
  case ARM_INS_UHSUB8:
  case ARM_INS_SHSAX:
  case ARM_INS_UHSAX: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    auto is = [&](std::initializer_list<int> L) {
      for (int V : L)
        if (static_cast<int>(Insn->id) == V)
          return true;
      return false;
    };
    bool Is8 =
        is({ARM_INS_QADD8, ARM_INS_UQADD8, ARM_INS_SADD8, ARM_INS_UADD8,
            ARM_INS_SHADD8, ARM_INS_UHADD8, ARM_INS_QSUB8, ARM_INS_UQSUB8,
            ARM_INS_SSUB8, ARM_INS_USUB8, ARM_INS_SHSUB8, ARM_INS_UHSUB8});
    bool IsUnsigned =
        is({ARM_INS_UQADD16, ARM_INS_UQADD8, ARM_INS_UQASX, ARM_INS_UADD16,
            ARM_INS_UADD8, ARM_INS_UASX, ARM_INS_UHADD16, ARM_INS_UHADD8,
            ARM_INS_UHASX, ARM_INS_UQSUB16, ARM_INS_UQSUB8, ARM_INS_UQSAX,
            ARM_INS_USUB16, ARM_INS_USUB8, ARM_INS_USAX, ARM_INS_UHSUB16,
            ARM_INS_UHSUB8, ARM_INS_UHSAX});
    bool IsSat =
        is({ARM_INS_QADD16, ARM_INS_UQADD16, ARM_INS_QADD8, ARM_INS_UQADD8,
            ARM_INS_QASX, ARM_INS_UQASX, ARM_INS_QSUB16, ARM_INS_UQSUB16,
            ARM_INS_QSUB8, ARM_INS_UQSUB8, ARM_INS_QSAX, ARM_INS_UQSAX});
    bool IsHalving =
        is({ARM_INS_SHADD16, ARM_INS_UHADD16, ARM_INS_SHADD8, ARM_INS_UHADD8,
            ARM_INS_SHASX, ARM_INS_UHASX, ARM_INS_SHSUB16, ARM_INS_UHSUB16,
            ARM_INS_SHSUB8, ARM_INS_UHSUB8, ARM_INS_SHSAX, ARM_INS_UHSAX});
    bool IsAsx = is({ARM_INS_QASX, ARM_INS_UQASX, ARM_INS_SASX, ARM_INS_UASX,
                     ARM_INS_SHASX, ARM_INS_UHASX});
    bool IsSax = is({ARM_INS_QSAX, ARM_INS_UQSAX, ARM_INS_SSAX, ARM_INS_USAX,
                     ARM_INS_SHSAX, ARM_INS_UHSAX});
    bool BaseSub =
        is({ARM_INS_QSUB16, ARM_INS_UQSUB16, ARM_INS_QSUB8, ARM_INS_UQSUB8,
            ARM_INS_SSUB16, ARM_INS_USUB16, ARM_INS_SSUB8, ARM_INS_USUB8,
            ARM_INS_SHSUB16, ARM_INS_UHSUB16, ARM_INS_SHSUB8, ARM_INS_UHSUB8});
    unsigned LaneBytes = Is8 ? 1 : 2;
    unsigned LaneBits = LaneBytes * 8;
    // Plain (non-saturating, non-halving) forms set the APSR.GE lane flags that
    // a following SEL consumes; the Q/H variants leave GE unchanged.
    bool SetsGE = !IsSat && !IsHalving;

    // Compute one lane: extend, add/sub in 32 bits, then
    // halve/saturate/truncate. When GEBase >= 0 also set the lane's GE flag(s)
    // from the ARM ARM rule.
    auto laneCompute = [&](NdVar LA, NdVar LB, bool Sub,
                           int GEBase) -> NdVar {
      NdVar EA = S.makeTemp(4);
      NdVar EB = S.makeTemp(4);
      NdOp Ext = IsUnsigned ? NdOp::INT_ZEXT : NdOp::INT_SEXT;
      S.emit(Ext, EA, {LA});
      S.emit(Ext, EB, {LB});
      NdVar Raw = S.makeTemp(4);
      S.emit(Sub ? NdOp::INT_SUB : NdOp::INT_ADD, Raw, {EA, EB});
      if (GEBase >= 0) {
        // unsigned add -> carry (Raw >= 2^n); unsigned sub -> no borrow
        // (EA >= EB); signed add/sub -> (Raw >= 0).  16-bit lanes duplicate the
        // flag across the two adjacent GE bits.
        NdVar Ge = S.makeTemp(1);
        if (IsUnsigned && Sub) {
          S.emit(NdOp::INT_LESSEQUAL, Ge, {EB, EA});
        } else if (IsUnsigned) {
          S.emit(NdOp::INT_LESSEQUAL, Ge,
                 {NdVar::cst(1ULL << LaneBits, 4), Raw});
        } else {
          NdVar Neg = S.makeTemp(1);
          S.emit(NdOp::INT_SLESS, Neg, {Raw, NdVar::cst(0, 4)});
          S.emit(NdOp::BOOL_NOT, Ge, {Neg});
        }
        S.emit(NdOp::COPY, NdVar::reg(armreg::GEFLAG(GEBase), 1), {Ge});
        if (LaneBytes == 2)
          S.emit(NdOp::COPY, NdVar::reg(armreg::GEFLAG(GEBase + 1), 1), {Ge});
      }
      NdVar Out = S.makeTemp(LaneBytes);
      if (IsHalving) {
        // The result is bits[LaneBits:1] of the signed intermediate, i.e. an
        // arithmetic shift right by one (keeps the sign for negative diffs).
        NdVar Half = S.makeTemp(4);
        S.emit(NdOp::INT_ASHR, Half, {Raw, NdVar::cst(1, 4)});
        S.emit(NdOp::SUBBYTES, Out, {Half, NdVar::cst(0, 4)});
      } else if (IsSat) {
        S.emitIntrinsic(IsUnsigned ? Intrinsic::ArmUsat : Intrinsic::ArmSsat,
                        Out, {Raw, NdVar::cst(LaneBits, 4)});
      } else {
        S.emit(NdOp::SUBBYTES, Out, {Raw, NdVar::cst(0, 4)});
      }
      return Out;
    };

    NdVar Acc = S.makeTemp(0);
    if (IsAsx || IsSax) {
      // 16-bit only: second operand halves swapped, per-lane op alternates.
      NdVar A0 = S.makeTemp(2), A1 = S.makeTemp(2);
      NdVar B0 = S.makeTemp(2), B1 = S.makeTemp(2);
      S.emit(NdOp::SUBBYTES, A0, {A, NdVar::cst(0, 4)});
      S.emit(NdOp::SUBBYTES, A1, {A, NdVar::cst(2, 4)});
      S.emit(NdOp::SUBBYTES, B0, {B, NdVar::cst(0, 4)});
      S.emit(NdOp::SUBBYTES, B1, {B, NdVar::cst(2, 4)});
      // Low halfword -> GE[1:0], high halfword -> GE[3:2].
      NdVar Lo = laneCompute(A0, B1, /*Sub=*/IsAsx, SetsGE ? 0 : -1);
      NdVar Hi = laneCompute(A1, B0, /*Sub=*/IsSax, SetsGE ? 2 : -1);
      NdVar Next = S.makeTemp(4);
      S.emit(NdOp::CONCAT, Next, {Hi, Lo});
      Acc = Next;
    } else {
      unsigned NLanes = 4 / LaneBytes;
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar LA = S.makeTemp(LaneBytes), LB = S.makeTemp(LaneBytes);
        S.emit(NdOp::SUBBYTES, LA, {A, NdVar::cst(I * LaneBytes, 4)});
        S.emit(NdOp::SUBBYTES, LB, {B, NdVar::cst(I * LaneBytes, 4)});
        NdVar Out =
            laneCompute(LA, LB, BaseSub, SetsGE ? (int)(I * LaneBytes) : -1);
        if (I == 0) {
          Acc = Out;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneBytes);
          S.emit(NdOp::CONCAT, Next, {Out, Acc});
          Acc = Next;
        }
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // SSAT/USAT — saturate to specified bit width.  Saturation is an exact clamp,
  // so lift it directly to compare+select instead of an intrinsic (the emitter
  // had no handler and fell back to a malformed `ssat` inline-asm string that
  // failed to reassemble).  SSAT clamps to the signed range [-2^(n-1),
  // 2^(n-1)-1]; USAT clamps the signed input to the unsigned range [0, 2^n-1].
  // The "16" variants apply the same clamp to each 16-bit half independently.
  case ARM_INS_SSAT:
  case ARM_INS_USAT:
  case ARM_INS_SSAT16:
  case ARM_INS_USAT16: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    uint32_t Sat = static_cast<uint32_t>(ARM.operands[1].imm);
    NdVar Src = operandRead(S, ARM.operands[ARM.op_count - 1]);
    bool IsSigned = (Insn->id == ARM_INS_SSAT || Insn->id == ARM_INS_SSAT16);
    bool IsHalf = (Insn->id == ARM_INS_SSAT16 || Insn->id == ARM_INS_USAT16);

    // Clamp the `Width`-byte signed value in `In` to the saturation range and
    // write the result into `Out` (same width).
    // SSAT/USAT clamp.  Uses a single-SELECT pattern to avoid the fork's
    // InstCombine mis-fold on chained INT_SLESS+SELECT clamp.  Check
    // out-of-range in one go, then pick the saturation value based on sign.
    auto clamp = [&](NdVar In, NdVar Out, unsigned Width) {
      int64_t Max =
          IsSigned ? ((Sat >= 32) ? 0x7FFFFFFFLL : ((1LL << (Sat - 1)) - 1))
                   : ((Sat >= 32) ? 0xFFFFFFFFLL : ((1LL << Sat) - 1));
      int64_t Min =
          IsSigned ? ((Sat >= 32) ? -(1LL << 31) : -(1LL << (Sat - 1))) : 0;
      NdVar MaxC = NdVar::cst(static_cast<uint64_t>(Max), Width);
      NdVar MinC = NdVar::cst(static_cast<uint64_t>(Min), Width);
      NdVar GtMax = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, GtMax, {MaxC, In});
      NdVar LtMin = S.makeTemp(1);
      if (IsSigned)
        S.emit(NdOp::INT_SLESS, LtMin, {In, MinC});
      else
        S.emit(NdOp::INT_SLESS, LtMin, {In, NdVar::cst(0, Width)});
      NdVar OutOfRange = S.makeTemp(1);
      S.emit(NdOp::BOOL_OR, OutOfRange, {GtMax, LtMin});
      NdVar IsPos = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, IsPos, {NdVar::cst(0, Width), In});
      NdVar SatVal = S.makeTemp(Width);
      S.emit(NdOp::SELECT, SatVal, {IsPos, MaxC, MinC});
      S.emit(NdOp::SELECT, Out, {OutOfRange, SatVal, In});
    };

    if (IsHalf) {
      // Two independent 16-bit halves; sign-extend each to 32 bits, clamp, then
      // truncate back to 16 bits and reassemble.
      NdVar Acc = S.makeTemp(0);
      for (unsigned H = 0; H < 2; ++H) {
        NdVar Half = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Half, {Src, NdVar::cst(H * 2, 4)});
        NdVar Wide = S.makeTemp(4);
        S.emit(NdOp::INT_SEXT, Wide, {Half});
        NdVar Clamped = S.makeTemp(4);
        clamp(Wide, Clamped, 4);
        NdVar Narrow = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Narrow, {Clamped, NdVar::cst(0, 4)});
        if (H == 0)
          Acc = Narrow;
        else {
          NdVar Next = S.makeTemp(4);
          S.emit(NdOp::CONCAT, Next, {Narrow, Acc});
          Acc = Next;
        }
      }
      S.emit(NdOp::COPY, Dst, {Acc});
    } else {
      clamp(Src, Dst, Dst.Size > 0 ? Dst.Size : 4);
    }
    break;
  }

  // PKHBT: pack bottom Half from Rn, top Half from Rm (Shifted)
  case ARM_INS_PKHBT: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Lo = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Lo, {A, NdVar::cst(0xFFFF, 4)});
    NdVar Hi = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Hi, {B, NdVar::cst(0xFFFF0000u, 4)});
    S.emit(NdOp::INT_OR, Dst, {Lo, Hi});
    break;
  }
  // PKHTB: pack top Half from Rn, bottom Half from Rm (Shifted)
  case ARM_INS_PKHTB: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Hi = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Hi, {A, NdVar::cst(0xFFFF0000u, 4)});
    NdVar Lo = S.makeTemp(4);
    S.emit(NdOp::INT_AND, Lo, {B, NdVar::cst(0xFFFF, 4)});
    S.emit(NdOp::INT_OR, Dst, {Hi, Lo});
    break;
  }
  // SEL — per-byte select on the APSR.GE flags: Rd.byte[i] = GE[i] ? Rn : Rm.
  // The GE flags are produced by a preceding GE-setting parallel add/sub (the
  // plain SADD8/UADD16/SSUB8/SASX/... handled above).  The old code emitted an
  // unhandled `ArmSel` intrinsic which the emitter turned into a bare `sel`
  // inline-asm string (too few operands) that aborted codegen.
  case ARM_INS_SEL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < 4; ++I) {
      NdVar ByteA = S.makeTemp(1), ByteB = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, ByteA, {A, NdVar::cst(I, 4)});
      S.emit(NdOp::SUBBYTES, ByteB, {B, NdVar::cst(I, 4)});
      NdVar Sel = S.makeTemp(1);
      S.emit(NdOp::SELECT, Sel,
             {NdVar::reg(armreg::GEFLAG(I), 1), ByteA, ByteB});
      if (I == 0) {
        Acc = Sel;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 1);
        S.emit(NdOp::CONCAT, Next, {Sel, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case ARM_INS_USAD8: {
    // USAD8: |a[7:0]-b[7:0]| + |a[15:8]-b[15:8]| + |a[23:16]-b[23:16]| +
    // |a[31:24]-b[31:24]|
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar Acc = S.makeTemp(4);
    S.emit(NdOp::COPY, Acc, {NdVar::cst(0, 4)});
    for (int I = 0; I < 4; I++) {
      NdVar ByteA = S.makeTemp(4);
      NdVar ByteB = S.makeTemp(4);
      S.emit(NdOp::INT_RIGHT, ByteA, {A, NdVar::cst(I * 8, 4)});
      S.emit(NdOp::INT_AND, ByteA, {ByteA, NdVar::cst(0xFF, 4)});
      S.emit(NdOp::INT_RIGHT, ByteB, {B, NdVar::cst(I * 8, 4)});
      S.emit(NdOp::INT_AND, ByteB, {ByteB, NdVar::cst(0xFF, 4)});
      NdVar GE = S.makeTemp(1);
      S.emit(NdOp::INT_LESSEQUAL, GE, {ByteB, ByteA});
      NdVar DiffPos = S.makeTemp(4);
      S.emit(NdOp::INT_SUB, DiffPos, {ByteA, ByteB});
      NdVar DiffNeg = S.makeTemp(4);
      S.emit(NdOp::INT_SUB, DiffNeg, {ByteB, ByteA});
      NdVar AbsDiff = S.makeTemp(4);
      S.emit(NdOp::SELECT, AbsDiff, {GE, DiffPos, DiffNeg});
      S.emit(NdOp::INT_ADD, Acc, {Acc, AbsDiff});
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }
  case ARM_INS_USADA8: {
    if (ARM.op_count < 4)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdVar C = operandRead(S, ARM.operands[3]);
    NdVar Acc = S.makeTemp(4);
    S.emit(NdOp::COPY, Acc, {NdVar::cst(0, 4)});
    for (int I = 0; I < 4; I++) {
      NdVar ByteA = S.makeTemp(4);
      NdVar ByteB = S.makeTemp(4);
      S.emit(NdOp::INT_RIGHT, ByteA, {A, NdVar::cst(I * 8, 4)});
      S.emit(NdOp::INT_AND, ByteA, {ByteA, NdVar::cst(0xFF, 4)});
      S.emit(NdOp::INT_RIGHT, ByteB, {B, NdVar::cst(I * 8, 4)});
      S.emit(NdOp::INT_AND, ByteB, {ByteB, NdVar::cst(0xFF, 4)});
      NdVar GE = S.makeTemp(1);
      S.emit(NdOp::INT_LESSEQUAL, GE, {ByteB, ByteA});
      NdVar DiffPos = S.makeTemp(4);
      S.emit(NdOp::INT_SUB, DiffPos, {ByteA, ByteB});
      NdVar DiffNeg = S.makeTemp(4);
      S.emit(NdOp::INT_SUB, DiffNeg, {ByteB, ByteA});
      NdVar AbsDiff = S.makeTemp(4);
      S.emit(NdOp::SELECT, AbsDiff, {GE, DiffPos, DiffNeg});
      S.emit(NdOp::INT_ADD, Acc, {Acc, AbsDiff});
    }
    S.emit(NdOp::INT_ADD, Dst, {Acc, C});
    break;
  }

  // CRC32 — checksum accumulate (polynomial, not plain XOR): Rd = crc(Rn, Rm).
  // The b/h forms consume the low 8/16 bits of Rm.  The bare no-operand form
  // dropped both inputs and the result, folding callers to a constant 0; route
  // through the (accumulator, data) intrinsic like the AArch64 path.
  case ARM_INS_CRC32B:
  case ARM_INS_CRC32H:
  case ARM_INS_CRC32W:
  case ARM_INS_CRC32CB:
  case ARM_INS_CRC32CH:
  case ARM_INS_CRC32CW: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    Intrinsic Id;
    switch (Insn->id) {
    case ARM_INS_CRC32B:
      Id = Intrinsic::ArmCrc32b;
      break;
    case ARM_INS_CRC32H:
      Id = Intrinsic::ArmCrc32h;
      break;
    case ARM_INS_CRC32W:
      Id = Intrinsic::ArmCrc32w;
      break;
    case ARM_INS_CRC32CB:
      Id = Intrinsic::ArmCrc32cb;
      break;
    case ARM_INS_CRC32CH:
      Id = Intrinsic::ArmCrc32ch;
      break;
    default:
      Id = Intrinsic::ArmCrc32cw;
      break;
    }
    S.emitIntrinsic(Id, Dst, {A, B});
    break;
  }

  // Conditional select (ARMv8.1-M): CSEL/CSINC/CSINV/CSNEG
  case ARM_INS_CSEL:
  case ARM_INS_CSINC:
  case ARM_INS_CSINV:
  case ARM_INS_CSNEG: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    // Build condition from ARM.cc
    NdVar Cond = buildCondCode(ARM.cc, S);
    NdVar TrueVal = operandRead(S, ARM.operands[1]);
    NdVar FalseVal;
    NdVar Src2 = operandRead(S, ARM.operands[2]);
    switch (Insn->id) {
    case ARM_INS_CSEL:
      FalseVal = Src2;
      break;
    case ARM_INS_CSINC:
      FalseVal = S.makeTemp(4);
      S.emit(NdOp::INT_ADD, FalseVal, {Src2, NdVar::cst(1, 4)});
      break;
    case ARM_INS_CSINV:
      FalseVal = S.makeTemp(4);
      S.emit(NdOp::INT_NOT, FalseVal, {Src2});
      break;
    case ARM_INS_CSNEG:
      FalseVal = S.makeTemp(4);
      S.emit(NdOp::INT_NEG2, FalseVal, {Src2});
      break;
    default:
      FalseVal = Src2;
      break;
    }
    S.emit(NdOp::SELECT, Dst, {Cond, TrueVal, FalseVal});
    break;
  }

  // Shifts (MVE)
  case ARM_INS_ASRL:
  case ARM_INS_LSLL:
  case ARM_INS_LSRL: {
    if (ARM.op_count < 3)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar A = operandRead(S, ARM.operands[1]);
    NdVar B = operandRead(S, ARM.operands[2]);
    NdOp Opc = NdOp::INT_ASHR;
    if (Insn->id == ARM_INS_LSLL)
      Opc = NdOp::INT_LEFT;
    else if (Insn->id == ARM_INS_LSRL)
      Opc = NdOp::INT_RIGHT;
    S.emit(Opc, Dst, {A, B});
    break;
  }
  case ARM_INS_SQSHL:
  case ARM_INS_SQSHLL:
  case ARM_INS_UQSHL:
  case ARM_INS_UQSHLL: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    if (ARM.op_count >= 3) {
      NdVar Amt = operandRead(S, ARM.operands[2]);
      S.emit(NdOp::INT_LEFT, Dst, {Src, Amt});
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }
  case ARM_INS_UQRSHL:
  case ARM_INS_UQRSHLL:
  case ARM_INS_SQRSHR:
  case ARM_INS_SQRSHRL:
  case ARM_INS_SRSHR:
  case ARM_INS_SRSHRL:
  case ARM_INS_URSHR:
  case ARM_INS_URSHRL: {
    if (ARM.op_count < 2)
      break;
    NdVar Dst = operandWrite(ARM.operands[0]);
    NdVar Src = operandRead(S, ARM.operands[1]);
    if (ARM.op_count >= 3) {
      NdVar Amt = operandRead(S, ARM.operands[2]);
      S.emit(NdOp::INT_RIGHT, Dst, {Src, Amt});
    } else {
      S.emit(NdOp::COPY, Dst, {Src});
    }
    break;
  }

  // Table branch
  case ARM_INS_TBB:
  case ARM_INS_TBH: {
    if (ARM.op_count >= 1) {
      NdVar Target = operandRead(S, ARM.operands[0]);
      S.emit(NdOp::INDIR_BR, {}, {Target});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
