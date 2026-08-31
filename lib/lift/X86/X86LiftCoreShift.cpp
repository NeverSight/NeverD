//===- X86LiftCoreShift.cpp - x86/x64 shift and rotate lifter -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SHL/SAL/SHR/SAR, ROL/ROR, the double-precision shifts
/// SHLD/SHRD, and the rotate-through-carry RCL/RCR.
///
//===----------------------------------------------------------------------===//

#include "X86LiftAPXValidation.h"
#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

struct ApxSingleShift {
  bool Present = false;
  bool Valid = false;
  bool NDD = false;
  bool NF = false;
  unsigned SourceIndex = 0;
  unsigned CountIndex = 1;
};

struct ApxDoubleShift {
  bool Present = false;
  bool Valid = false;
  bool NDD = false;
  bool NF = false;
  unsigned BaseIndex = 0;
  unsigned SourceIndex = 1;
  unsigned CountIndex = 2;
};

ApxSingleShift decodeApxSingleShift(const cs_insn *Insn, const cs_x86 &X86) {
  ApxSingleShift Result;
  switch (Insn ? Insn->id : X86_INS_INVALID) {
  case X86_INS_ROL:
  case X86_INS_ROR:
  case X86_INS_RCL:
  case X86_INS_RCR:
  case X86_INS_SHL:
  case X86_INS_SAL:
  case X86_INS_SHR:
  case X86_INS_SAR:
    break;
  default:
    return Result;
  }
  Result.Present = apxvalidation::isPresent(Insn, X86);
  if (!Result.Present)
    return Result;

  size_t Marker = 0;
  while (Marker < Insn->size && Insn->bytes[Marker] != 0x62)
    ++Marker;
  if (Marker + 5 > Insn->size)
    return Result;
  const uint8_t RawOpcode = Insn->bytes[Marker + 4];
  const unsigned ImmediateBytes =
      RawOpcode == 0xc0 || RawOpcode == 0xc1 ? 1 : 0;
  apxvalidation::Header H;
  if (!apxvalidation::decodeHeader(Insn, X86, H, ImmediateBytes) ||
      (H.P0 & 7) != 4)
    return Result;

  unsigned ExpectedGroup = 0;
  switch (Insn->id) {
  case X86_INS_ROL:
    ExpectedGroup = 0;
    break;
  case X86_INS_ROR:
    ExpectedGroup = 1;
    break;
  case X86_INS_RCL:
    ExpectedGroup = 2;
    break;
  case X86_INS_RCR:
    ExpectedGroup = 3;
    break;
  case X86_INS_SHL:
  case X86_INS_SAL:
    ExpectedGroup = (H.ModRM >> 3) & 7;
    if (ExpectedGroup != 4 && ExpectedGroup != 6)
      return Result;
    break;
  case X86_INS_SHR:
    ExpectedGroup = 5;
    break;
  case X86_INS_SAR:
    ExpectedGroup = 7;
    break;
  default:
    return Result;
  }
  if (((H.ModRM >> 3) & 7) != ExpectedGroup ||
      (H.Opcode != 0xc0 && H.Opcode != 0xc1 && H.Opcode != 0xd0 &&
       H.Opcode != 0xd1 && H.Opcode != 0xd2 && H.Opcode != 0xd3) ||
      (H.P2 & 0xe3) != 0 || (!H.Memory && (H.P1 & 4) == 0))
    return Result;

  unsigned Width = 0;
  if ((H.Opcode & 1) == 0) {
    if ((H.P1 & 3) != 0)
      return Result;
    Width = 1;
  } else if (H.P1 & 0x80) {
    if ((H.P1 & 3) > 1)
      return Result;
    Width = 8;
  } else if ((H.P1 & 3) == 1) {
    Width = 2;
  } else if ((H.P1 & 3) == 0) {
    Width = 4;
  } else {
    return Result;
  }

  Result.NDD = (H.P2 & 0x10) != 0;
  Result.NF = (H.P2 & 4) != 0;
  const bool ReadsCarry = Insn->id == X86_INS_RCL || Insn->id == X86_INS_RCR;
  if ((!Result.NDD && apxvalidation::vvvvv(H) != 0) ||
      (ReadsCarry && Result.NF))
    return Result;
  const unsigned ExpectedOperands = Result.NDD ? 3 : 2;
  Result.SourceIndex = Result.NDD ? 1 : 0;
  Result.CountIndex = ExpectedOperands - 1;
  if (X86.op_count != ExpectedOperands)
    return Result;
  if (Result.NDD &&
      !apxvalidation::registerOperand(X86.operands[0], apxvalidation::vvvvv(H),
                                      Width, CS_AC_WRITE))
    return Result;
  if (!apxvalidation::validateRM(
          Insn, X86, H, X86.operands[Result.SourceIndex], Width,
          Result.NDD ? CS_AC_READ : CS_AC_READ | CS_AC_WRITE, ImmediateBytes))
    return Result;

  const cs_x86_op &Count = X86.operands[Result.CountIndex];
  if (H.Opcode == 0xd2 || H.Opcode == 0xd3) {
    if (Count.type != X86_OP_REG || Count.reg != X86_REG_CL ||
        Count.size != 1 || Count.access != CS_AC_READ)
      return Result;
  } else {
    const uint8_t Expected =
        H.Opcode == 0xd0 || H.Opcode == 0xd1 ? 1 : Insn->bytes[Insn->size - 1];
    if (!apxvalidation::immediateOperand(Insn, Count, 1, 1, Expected))
      return Result;
  }

  const bool Rotate = Insn->id == X86_INS_ROL || Insn->id == X86_INS_ROR ||
                      Insn->id == X86_INS_RCL || Insn->id == X86_INS_RCR;
  const uint64_t ExpectedFlags =
      Result.NF ? 0
      : Rotate  ? X86_EFLAGS_UNDEFINED_OF | X86_EFLAGS_MODIFY_CF
                : X86_EFLAGS_MODIFY_OF | X86_EFLAGS_MODIFY_SF |
                      X86_EFLAGS_MODIFY_ZF | X86_EFLAGS_UNDEFINED_AF |
                      X86_EFLAGS_MODIFY_PF | X86_EFLAGS_MODIFY_CF;
  if (!apxvalidation::implicitDetail(
          Insn, X86, ExpectedFlags,
          ReadsCarry ? std::initializer_list<x86_reg>{X86_REG_EFLAGS}
                     : std::initializer_list<x86_reg>{},
          Result.NF ? std::initializer_list<x86_reg>{}
                    : std::initializer_list<x86_reg>{X86_REG_EFLAGS}))
    return Result;

  Result.Valid = true;
  return Result;
}

ApxDoubleShift decodeApxDoubleShift(const cs_insn *Insn, const cs_x86 &X86) {
  ApxDoubleShift Result;
  if (!Insn || (Insn->id != X86_INS_SHLD && Insn->id != X86_INS_SHRD))
    return Result;
  Result.Present = apxvalidation::isPresent(Insn, X86);
  if (!Result.Present)
    return Result;

  size_t Marker = 0;
  while (Marker < Insn->size && Insn->bytes[Marker] != 0x62)
    ++Marker;
  if (Marker + 5 > Insn->size)
    return Result;
  const uint8_t RawOpcode = Insn->bytes[Marker + 4];
  const unsigned ImmediateBytes =
      RawOpcode == 0x24 || RawOpcode == 0x2c ? 1 : 0;
  apxvalidation::Header H;
  if (!apxvalidation::decodeHeader(Insn, X86, H, ImmediateBytes) ||
      (H.P0 & 7) != 4 || (H.P1 & 3) > 1 || (H.P2 & 0xe3) != 0 ||
      (!H.Memory && (H.P1 & 4) == 0))
    return Result;
  const bool Immediate = H.Opcode == 0x24 || H.Opcode == 0x2c;
  const bool CountCL = H.Opcode == 0xa5 || H.Opcode == 0xad;
  if ((!Immediate && !CountCL) ||
      (Insn->id == X86_INS_SHLD && H.Opcode != 0x24 && H.Opcode != 0xa5) ||
      (Insn->id == X86_INS_SHRD && H.Opcode != 0x2c && H.Opcode != 0xad))
    return Result;

  const unsigned Width = (H.P1 & 0x80) != 0 ? 8 : (H.P1 & 3) == 1 ? 2 : 4;
  Result.NDD = (H.P2 & 0x10) != 0;
  Result.NF = (H.P2 & 4) != 0;
  if (!Result.NDD && apxvalidation::vvvvv(H) != 0)
    return Result;
  const unsigned ExpectedOperands = Result.NDD ? 4 : 3;
  Result.BaseIndex = Result.NDD ? 1 : 0;
  Result.SourceIndex = Result.NDD ? 2 : 1;
  Result.CountIndex = ExpectedOperands - 1;
  if (X86.op_count != ExpectedOperands)
    return Result;
  if (Result.NDD &&
      !apxvalidation::registerOperand(X86.operands[0], apxvalidation::vvvvv(H),
                                      Width, CS_AC_WRITE))
    return Result;
  if (!apxvalidation::validateRM(
          Insn, X86, H, X86.operands[Result.BaseIndex], Width,
          Result.NDD ? CS_AC_READ : CS_AC_READ | CS_AC_WRITE, ImmediateBytes) ||
      !apxvalidation::registerOperand(X86.operands[Result.SourceIndex],
                                      apxvalidation::modrmReg(H), Width,
                                      CS_AC_READ))
    return Result;

  const cs_x86_op &Count = X86.operands[Result.CountIndex];
  if (CountCL) {
    if (Count.type != X86_OP_REG || Count.reg != X86_REG_CL ||
        Count.size != 1 || Count.access != CS_AC_READ)
      return Result;
  } else if (!apxvalidation::immediateOperand(Insn, Count, 1, 1,
                                              Insn->bytes[Insn->size - 1])) {
    return Result;
  }
  const uint64_t ExpectedFlags =
      Result.NF ? 0
                : X86_EFLAGS_MODIFY_OF | X86_EFLAGS_MODIFY_SF |
                      X86_EFLAGS_MODIFY_ZF | X86_EFLAGS_UNDEFINED_AF |
                      X86_EFLAGS_MODIFY_PF | X86_EFLAGS_MODIFY_CF;
  if (!apxvalidation::implicitDetail(
          Insn, X86, ExpectedFlags,
          CountCL ? std::initializer_list<x86_reg>{X86_REG_CL}
                  : std::initializer_list<x86_reg>{},
          Result.NF ? std::initializer_list<x86_reg>{}
                    : std::initializer_list<x86_reg>{X86_REG_EFLAGS}))
    return Result;
  Result.Valid = true;
  return Result;
}

void emitZeroCountResultGuard(X86Lifter::LiftState &S, NdVar Count,
                              NdVar Original, NdVar Candidate, NdVar Result) {
  NdVar IsZero = S.makeTemp(1);
  S.emit(NdOp::INT_EQUAL, IsZero, {Count, NdVar::scalar(0, Count.Size)});
  S.emit(NdOp::SELECT, Result, {IsZero, Original, Candidate});
}

} // namespace

bool liftCoreShift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  const ApxSingleShift ApxShift = decodeApxSingleShift(Insn, X86);
  if (ApxShift.Present && !ApxShift.Valid)
    return false;
  const ApxDoubleShift ApxDouble = decodeApxDoubleShift(Insn, X86);
  if (ApxDouble.Present && !ApxDouble.Valid)
    return false;
  switch (InsnId) {

  // --- SHL / SHR / SAR ---
  case X86_INS_SHL:
  case X86_INS_SAL:
  case X86_INS_SHR:
  case X86_INS_SAR: {
    if (X86.op_count < 2)
      break;
    const unsigned SourceIndex = ApxShift.Valid ? ApxShift.SourceIndex : 0;
    const unsigned CountIndex = ApxShift.Valid ? ApxShift.CountIndex : 1;
    const bool SuppressFlags = ApxShift.Valid && ApxShift.NF;
    NdVar Cnt = L.operandRead(S, X86.operands[CountIndex]);
    if (Cnt.isConst())
      Cnt.Provenance = ConstantAddressProvenance::Scalar;
    NdVar DstR = L.operandRead(S, X86.operands[SourceIndex]);
    NdVar DstW = L.operandWrite(X86.operands[0]);

    if (Cnt.isReg() && Cnt.Size == 1)
      Cnt.Size = 4;

    uint16_t Sz = DstR.Size;
    uint16_t Bits = Sz * 8;

    // x86 masks the shift count: 0x1F for 8/16/32-bit, 0x3F for 64-bit
    uint64_t ShiftMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar MaskedCnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, MaskedCnt, {Cnt, NdVar::scalar(ShiftMask, Sz)});

    // Snapshot the source before the shift writes the (aliased) destination, so
    // SHR's OF (= MSB of the original operand) reads the pre-shift value.
    NdVar PreSrc = S.makeTemp(Sz);
    S.emit(NdOp::COPY, PreSrc, {DstR});

    // Snapshot the flags so a zero count can restore them (x86 leaves every
    // flag unchanged when the masked shift count is 0).
    NdVar OldCF, OldZF, OldSF, OldPF;
    if (!SuppressFlags) {
      OldCF = S.makeTemp(1);
      S.emit(NdOp::COPY, OldCF, {NdVar::reg(x86reg::CF, 1)});
      OldZF = S.makeTemp(1);
      S.emit(NdOp::COPY, OldZF, {NdVar::reg(x86reg::ZF, 1)});
      OldSF = S.makeTemp(1);
      S.emit(NdOp::COPY, OldSF, {NdVar::reg(x86reg::SF, 1)});
      OldPF = S.makeTemp(1);
      S.emit(NdOp::COPY, OldPF, {NdVar::reg(x86reg::PF, 1)});
    }

    // CF = last bit Shifted out (valid when Cnt >= 1).
    // SHL: bit (Bits - Cnt); SHR/SAR: bit (Cnt - 1)
    if (!SuppressFlags) {
      NdVar CfIdx = S.makeTemp(Sz);
      if (InsnId == X86_INS_SHL || InsnId == X86_INS_SAL)
        S.emit(NdOp::INT_SUB, CfIdx, {NdVar::scalar(Bits, Sz), MaskedCnt});
      else
        S.emit(NdOp::INT_SUB, CfIdx, {MaskedCnt, NdVar::scalar(1, Sz)});
      NdVar CfTmp = S.makeTemp(Sz);
      S.emit(NdOp::INT_RIGHT, CfTmp, {DstR, CfIdx});
      NdVar CfBit = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, CfBit, {CfTmp, NdVar::scalar(0, 4)});
      S.emit(NdOp::INT_AND, CfBit, {CfBit, NdVar::scalar(1, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {CfBit});
    }

    NdOp Opc = NdOp::INT_LEFT;
    if (InsnId == X86_INS_SHR)
      Opc = NdOp::INT_RIGHT;
    if (InsnId == X86_INS_SAR)
      Opc = NdOp::INT_ASHR;

    bool MemDst =
        (X86.operands[SourceIndex].type == X86_OP_MEM) && SourceIndex == 0;
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    S.emit(Opc, Result, {DstR, MaskedCnt});
    if (!SuppressFlags) {
      L.emitZSPF(S, Result);
      // OF (1-bit shifts only): SHL = MSB(result) ^ CF, SHR = MSB(source),
      // SAR = 0.  emitShiftRotateOF leaves OF unchanged for any other count.
      NdVar OfBit = S.makeTemp(1);
      if (InsnId == X86_INS_SHR) {
        S.emit(NdOp::COPY, OfBit, {L.extractBit(S, PreSrc, Bits - 1)});
      } else if (InsnId == X86_INS_SAR) {
        S.emit(NdOp::COPY, OfBit, {NdVar::scalar(0, 1)});
      } else {
        S.emit(NdOp::BOOL_XOR, OfBit,
               {L.extractBit(S, Result, Bits - 1), NdVar::reg(x86reg::CF, 1)});
      }
      L.emitShiftRotateOF(S, MaskedCnt, OfBit);
      L.emitZeroCountFlagGuard(S, MaskedCnt,
                               {{x86reg::CF, OldCF},
                                {x86reg::ZF, OldZF},
                                {x86reg::SF, OldSF},
                                {x86reg::PF, OldPF}});
    }
    if (MemDst)
      S.storeToMem(X86.operands[SourceIndex], Result);
    break;
  }

  // --- ROL / ROR ---
  case X86_INS_ROL:
  case X86_INS_ROR: {
    if (X86.op_count < 2)
      break;
    const unsigned SourceIndex = ApxShift.Valid ? ApxShift.SourceIndex : 0;
    const unsigned CountIndex = ApxShift.Valid ? ApxShift.CountIndex : 1;
    const bool SuppressFlags = ApxShift.Valid && ApxShift.NF;
    NdVar Cnt = L.operandRead(S, X86.operands[CountIndex]);
    if (Cnt.isConst())
      Cnt.Provenance = ConstantAddressProvenance::Scalar;
    NdVar DstR = L.operandRead(S, X86.operands[SourceIndex]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    bool MemDst =
        (X86.operands[SourceIndex].type == X86_OP_MEM) && SourceIndex == 0;
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    uint16_t Sz = DstR.Size;
    uint16_t Bits = Sz * 8;

    // x86 masks rotate count: 0x1F for 8/16/32-bit, 0x3F for 64-bit
    uint64_t RotMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar MaskedCnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, MaskedCnt, {Cnt, NdVar::scalar(RotMask, Sz)});
    // BYTE/WORD rotates take a SECOND reduction mod the operand size (Intel
    // SDM: tempCOUNT = (COUNT AND 1Fh) MOD size).  Since size is a power of two
    // this is `& (Bits-1)`.  Without it, e.g. `rolb $9` feeds x<<9 into the
    // saturating INT_LEFT (over-shift -> 0), dropping the high half.  32/64-bit
    // need no step (the 5/6-bit mask already yields a count < size).
    if (Bits < 32)
      S.emit(NdOp::INT_AND, MaskedCnt,
             {MaskedCnt, NdVar::scalar(Bits - 1, Sz)});

    // Rotates affect only CF and OF; snapshot CF so a zero count preserves it.
    NdVar OldCF;
    if (!SuppressFlags) {
      OldCF = S.makeTemp(1);
      S.emit(NdOp::COPY, OldCF, {NdVar::reg(x86reg::CF, 1)});
    }

    if (InsnId == X86_INS_ROL) {
      NdVar Shl = S.makeTemp(Sz);
      NdVar Comp = S.makeTemp(Sz);
      NdVar Shr = S.makeTemp(Sz);
      S.emit(NdOp::INT_LEFT, Shl, {DstR, MaskedCnt});
      S.emit(NdOp::INT_SUB, Comp, {NdVar::scalar(Bits, Sz), MaskedCnt});
      S.emit(NdOp::INT_AND, Comp, {Comp, NdVar::scalar(Bits - 1, Sz)});
      S.emit(NdOp::INT_RIGHT, Shr, {DstR, Comp});
      S.emit(NdOp::INT_OR, Result, {Shl, Shr});
      if (!SuppressFlags) {
        NdVar CfTmp = S.makeTemp(Sz);
        S.emit(NdOp::INT_AND, CfTmp, {Result, NdVar::scalar(1, Sz)});
        S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
               {CfTmp, NdVar::scalar(0, Sz)});
      }
    } else {
      NdVar Shr = S.makeTemp(Sz);
      NdVar Comp = S.makeTemp(Sz);
      NdVar Shl = S.makeTemp(Sz);
      S.emit(NdOp::INT_RIGHT, Shr, {DstR, MaskedCnt});
      S.emit(NdOp::INT_SUB, Comp, {NdVar::scalar(Bits, Sz), MaskedCnt});
      S.emit(NdOp::INT_AND, Comp, {Comp, NdVar::scalar(Bits - 1, Sz)});
      S.emit(NdOp::INT_LEFT, Shl, {DstR, Comp});
      S.emit(NdOp::INT_OR, Result, {Shr, Shl});
      if (!SuppressFlags) {
        NdVar CfTmp = S.makeTemp(Sz);
        S.emit(NdOp::INT_RIGHT, CfTmp, {Result, NdVar::scalar(Bits - 1, Sz)});
        S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
               {CfTmp, NdVar::scalar(0, Sz)});
      }
    }
    if (!SuppressFlags) {
      // OF (1-bit rotates only): ROL = MSB(result) ^ LSB(result),
      // ROR = MSB(result) ^ next-MSB(result).
      NdVar OfBit = S.makeTemp(1);
      if (InsnId == X86_INS_ROL)
        S.emit(NdOp::BOOL_XOR, OfBit,
               {L.extractBit(S, Result, Bits - 1), L.extractBit(S, Result, 0)});
      else
        S.emit(NdOp::BOOL_XOR, OfBit,
               {L.extractBit(S, Result, Bits - 1),
                L.extractBit(S, Result, Bits - 2)});
      L.emitShiftRotateOF(S, MaskedCnt, OfBit);
      L.emitZeroCountFlagGuard(S, MaskedCnt, {{x86reg::CF, OldCF}});
    }
    if (MemDst)
      S.storeToMem(X86.operands[SourceIndex], Result);
    break;
  }

  // ========================================================================
  // Double-precision shifts (SHLD / SHRD)
  // ========================================================================
  case X86_INS_SHLD: {
    if (X86.op_count < 3)
      break;
    const unsigned BaseIndex = ApxDouble.Valid ? ApxDouble.BaseIndex : 0;
    const unsigned SourceIndex = ApxDouble.Valid ? ApxDouble.SourceIndex : 1;
    const unsigned CountIndex = ApxDouble.Valid ? ApxDouble.CountIndex : 2;
    const bool SuppressFlags = ApxDouble.Valid && ApxDouble.NF;
    NdVar DstR = L.operandRead(S, X86.operands[BaseIndex]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[SourceIndex]);
    NdVar CntRaw = L.operandRead(S, X86.operands[CountIndex]);
    if (CntRaw.isConst())
      CntRaw.Provenance = ConstantAddressProvenance::Scalar;
    uint16_t Sz = DstR.Size;
    uint16_t Bits = Sz * 8;
    // A MEMORY destination must be written with an explicit STORE:
    // L.operandWrite() of a mem operand yields a discarded ram(0) placeholder,
    // so the prior code dropped the write-back for `shld [mem],reg,cnt` (value
    // computed, flags set, memory left unchanged).  Compute into a temp and
    // store it at the end.
    bool MemDst = BaseIndex == 0 && X86.operands[BaseIndex].type == X86_OP_MEM;
    NdVar Result = MemDst ? S.makeTemp(Sz) : DstW;
    uint64_t ShldMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::scalar(ShldMask, Sz)});
    // Snapshot flags so a zero (post-mask) count restores them: x86 leaves all
    // flags unchanged when SHLD/SHRD shift by 0 (same rule as the single
    // shifts).
    NdVar OldCF, OldZF, OldSF, OldPF;
    if (!SuppressFlags) {
      OldCF = S.makeTemp(1);
      S.emit(NdOp::COPY, OldCF, {NdVar::reg(x86reg::CF, 1)});
      OldZF = S.makeTemp(1);
      S.emit(NdOp::COPY, OldZF, {NdVar::reg(x86reg::ZF, 1)});
      OldSF = S.makeTemp(1);
      S.emit(NdOp::COPY, OldSF, {NdVar::reg(x86reg::SF, 1)});
      OldPF = S.makeTemp(1);
      S.emit(NdOp::COPY, OldPF, {NdVar::reg(x86reg::PF, 1)});
      NdVar CfIdx = S.makeTemp(Sz);
      S.emit(NdOp::INT_SUB, CfIdx, {NdVar::scalar(Bits, Sz), Cnt});
      NdVar CfTmp = S.makeTemp(Sz);
      S.emit(NdOp::INT_RIGHT, CfTmp, {DstR, CfIdx});
      NdVar CfBit = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, CfBit, {CfTmp, NdVar::scalar(0, 4)});
      S.emit(NdOp::INT_AND, CfBit, {CfBit, NdVar::scalar(1, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {CfBit});
    }
    NdVar Hi = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, Hi, {DstR, Cnt});
    NdVar Rem = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, Rem, {NdVar::scalar(Bits, Sz), Cnt});
    NdVar Lo = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Lo, {Src, Rem});
    NdVar Candidate = S.makeTemp(Sz);
    S.emit(NdOp::INT_OR, Candidate, {Hi, Lo});
    emitZeroCountResultGuard(S, Cnt, DstR, Candidate, Result);
    if (!SuppressFlags) {
      L.emitZSPF(S, Result);
      // OF (1-bit only): MSB(result) ^ CF (the SHL rule).
      NdVar OfBit = S.makeTemp(1);
      S.emit(NdOp::BOOL_XOR, OfBit,
             {L.extractBit(S, Result, Bits - 1), NdVar::reg(x86reg::CF, 1)});
      L.emitShiftRotateOF(S, Cnt, OfBit);
      L.emitZeroCountFlagGuard(S, Cnt,
                               {{x86reg::CF, OldCF},
                                {x86reg::ZF, OldZF},
                                {x86reg::SF, OldSF},
                                {x86reg::PF, OldPF}});
    }
    if (MemDst)
      S.storeToMem(X86.operands[BaseIndex], Result);
    break;
  }
  case X86_INS_SHRD: {
    if (X86.op_count < 3)
      break;
    const unsigned BaseIndex = ApxDouble.Valid ? ApxDouble.BaseIndex : 0;
    const unsigned SourceIndex = ApxDouble.Valid ? ApxDouble.SourceIndex : 1;
    const unsigned CountIndex = ApxDouble.Valid ? ApxDouble.CountIndex : 2;
    const bool SuppressFlags = ApxDouble.Valid && ApxDouble.NF;
    NdVar DstR = L.operandRead(S, X86.operands[BaseIndex]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[SourceIndex]);
    NdVar CntRaw = L.operandRead(S, X86.operands[CountIndex]);
    if (CntRaw.isConst())
      CntRaw.Provenance = ConstantAddressProvenance::Scalar;
    uint16_t Sz = DstR.Size;
    uint16_t Bits = Sz * 8;
    // Memory destination: store the result explicitly (see SHLD note above).
    bool MemDst = BaseIndex == 0 && X86.operands[BaseIndex].type == X86_OP_MEM;
    NdVar Result = MemDst ? S.makeTemp(Sz) : DstW;
    uint64_t ShrdMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::scalar(ShrdMask, Sz)});
    // Snapshot the original destination MSB (for OF) and the flags (for a zero
    // count) before the result write aliases the destination register.
    NdVar PreMsb, OldCF, OldZF, OldSF, OldPF;
    if (!SuppressFlags) {
      PreMsb = S.makeTemp(1);
      S.emit(NdOp::COPY, PreMsb, {L.extractBit(S, DstR, Bits - 1)});
      OldCF = S.makeTemp(1);
      S.emit(NdOp::COPY, OldCF, {NdVar::reg(x86reg::CF, 1)});
      OldZF = S.makeTemp(1);
      S.emit(NdOp::COPY, OldZF, {NdVar::reg(x86reg::ZF, 1)});
      OldSF = S.makeTemp(1);
      S.emit(NdOp::COPY, OldSF, {NdVar::reg(x86reg::SF, 1)});
      OldPF = S.makeTemp(1);
      S.emit(NdOp::COPY, OldPF, {NdVar::reg(x86reg::PF, 1)});
      NdVar CfIdx = S.makeTemp(Sz);
      S.emit(NdOp::INT_SUB, CfIdx, {Cnt, NdVar::scalar(1, Sz)});
      NdVar CfTmp = S.makeTemp(Sz);
      S.emit(NdOp::INT_RIGHT, CfTmp, {DstR, CfIdx});
      NdVar CfBit = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, CfBit, {CfTmp, NdVar::scalar(0, 4)});
      S.emit(NdOp::INT_AND, CfBit, {CfBit, NdVar::scalar(1, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {CfBit});
    }
    NdVar Lo = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Lo, {DstR, Cnt});
    NdVar Rem = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, Rem, {NdVar::scalar(Bits, Sz), Cnt});
    NdVar Hi = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, Hi, {Src, Rem});
    NdVar Candidate = S.makeTemp(Sz);
    S.emit(NdOp::INT_OR, Candidate, {Lo, Hi});
    emitZeroCountResultGuard(S, Cnt, DstR, Candidate, Result);
    if (!SuppressFlags) {
      L.emitZSPF(S, Result);
      // OF (1-bit only): sign change = MSB(original dst) ^ MSB(result).
      NdVar OfBit = S.makeTemp(1);
      S.emit(NdOp::BOOL_XOR, OfBit,
             {PreMsb, L.extractBit(S, Result, Bits - 1)});
      L.emitShiftRotateOF(S, Cnt, OfBit);
      L.emitZeroCountFlagGuard(S, Cnt,
                               {{x86reg::CF, OldCF},
                                {x86reg::ZF, OldZF},
                                {x86reg::SF, OldSF},
                                {x86reg::PF, OldPF}});
    }
    if (MemDst)
      S.storeToMem(X86.operands[BaseIndex], Result);
    break;
  }

  case X86_INS_RCR: {
    if (X86.op_count < 2)
      break;
    const unsigned SourceIndex = ApxShift.Valid ? ApxShift.SourceIndex : 0;
    const unsigned CountIndex = ApxShift.Valid ? ApxShift.CountIndex : 1;
    NdVar SrcR = L.operandRead(S, X86.operands[SourceIndex]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar CntRaw = L.operandRead(S, X86.operands[CountIndex]);
    if (CntRaw.isConst())
      CntRaw.Provenance = ConstantAddressProvenance::Scalar;
    uint16_t Sz = SrcR.Size;
    uint16_t Bits = Sz * 8;
    // Memory destination: store the result explicitly (see SHLD note above).
    bool MemDst =
        SourceIndex == 0 && X86.operands[SourceIndex].type == X86_OP_MEM;
    NdVar Result = MemDst ? S.makeTemp(Sz) : DstW;
    uint64_t RcrMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::scalar(RcrMask, Sz)});
    // RCR affects only CF and OF; snapshot CF so a zero count preserves it.
    NdVar OldCF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldCF, {NdVar::reg(x86reg::CF, 1)});
    // Rotate-through-carry cycles through Bits+1 positions (operand bits + CF),
    // so BYTE/WORD counts reduce mod 9/17 (Intel SDM), not just the 5-bit mask.
    // 32/64-bit need no step (the masked count is already < Bits+1).
    if (Bits < 32) {
      NdVar Modded = S.makeTemp(Sz);
      S.emit(NdOp::INT_REM, Modded,
             {Cnt, NdVar::scalar((uint64_t)Bits + 1, Sz)});
      Cnt = Modded;
    }
    NdVar CfExt = snapshotCarryAtWidth(S, Sz);
    NdVar CfIdx = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, CfIdx, {Cnt, NdVar::scalar(1, Sz)});
    NdVar CfShifted = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, CfShifted, {SrcR, CfIdx});
    NdVar NewCf = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, NewCf, {CfShifted, NdVar::scalar(0, 4)});
    S.emit(NdOp::INT_AND, NewCf, {NewCf, NdVar::scalar(1, 1)});
    NdVar Lower = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, Lower, {SrcR, Cnt});
    NdVar CfPos = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, CfPos, {NdVar::scalar(Bits, Sz), Cnt});
    NdVar CfIn = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, CfIn, {CfExt, CfPos});
    // WrapAmt = Bits+1-Cnt.  Clamp to avoid UB shift when Cnt==1.
    NdVar WrapAmtRaw = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, WrapAmtRaw, {NdVar::scalar(Bits + 1, Sz), Cnt});
    NdVar WrapOk = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, WrapOk, {WrapAmtRaw, NdVar::scalar(Bits, Sz)});
    NdVar WrapSafe = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, WrapSafe, {SrcR, WrapAmtRaw});
    NdVar Wrapped = S.makeTemp(Sz);
    S.emit(NdOp::SELECT, Wrapped, {WrapOk, WrapSafe, NdVar::scalar(0, Sz)});
    NdVar Tmp1 = S.makeTemp(Sz);
    S.emit(NdOp::INT_OR, Tmp1, {Lower, CfIn});
    NdVar Candidate = S.makeTemp(Sz);
    S.emit(NdOp::INT_OR, Candidate, {Tmp1, Wrapped});
    emitZeroCountResultGuard(S, Cnt, SrcR, Candidate, Result);
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NewCf});
    // OF (1-bit only, right rotate): XOR of the two most-significant result
    // bits.
    NdVar RcrOf = S.makeTemp(1);
    S.emit(
        NdOp::BOOL_XOR, RcrOf,
        {L.extractBit(S, Result, Bits - 1), L.extractBit(S, Result, Bits - 2)});
    L.emitShiftRotateOF(S, Cnt, RcrOf);
    L.emitZeroCountFlagGuard(S, Cnt, {{x86reg::CF, OldCF}});
    if (MemDst)
      S.storeToMem(X86.operands[SourceIndex], Result);
    break;
  }

  // ========================================================================
  // RCL — rotate left through carry
  // ========================================================================
  case X86_INS_RCL: {
    if (X86.op_count < 2)
      break;
    const unsigned SourceIndex = ApxShift.Valid ? ApxShift.SourceIndex : 0;
    const unsigned CountIndex = ApxShift.Valid ? ApxShift.CountIndex : 1;
    NdVar SrcR = L.operandRead(S, X86.operands[SourceIndex]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar CntRaw = L.operandRead(S, X86.operands[CountIndex]);
    if (CntRaw.isConst())
      CntRaw.Provenance = ConstantAddressProvenance::Scalar;
    uint16_t Sz = SrcR.Size;
    uint16_t Bits = Sz * 8;
    // Memory destination: store the result explicitly (see SHLD note above).
    bool MemDst =
        SourceIndex == 0 && X86.operands[SourceIndex].type == X86_OP_MEM;
    NdVar Result = MemDst ? S.makeTemp(Sz) : DstW;
    uint64_t RclMask = (Bits == 64) ? 0x3F : 0x1F;
    NdVar Cnt = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, Cnt, {CntRaw, NdVar::scalar(RclMask, Sz)});
    // RCL affects only CF and OF; snapshot CF so a zero count preserves it.
    NdVar OldCF = S.makeTemp(1);
    S.emit(NdOp::COPY, OldCF, {NdVar::reg(x86reg::CF, 1)});
    // Rotate-through-carry cycles through Bits+1 positions (operand bits + CF),
    // so BYTE/WORD counts reduce mod 9/17 (Intel SDM), not just the 5-bit mask.
    // 32/64-bit need no step (the masked count is already < Bits+1).
    if (Bits < 32) {
      NdVar Modded = S.makeTemp(Sz);
      S.emit(NdOp::INT_REM, Modded,
             {Cnt, NdVar::scalar((uint64_t)Bits + 1, Sz)});
      Cnt = Modded;
    }
    NdVar CfExt = snapshotCarryAtWidth(S, Sz);
    NdVar CfBitPos = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, CfBitPos, {NdVar::scalar(Bits, Sz), Cnt});
    NdVar CfShifted = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, CfShifted, {SrcR, CfBitPos});
    NdVar NewCf = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, NewCf, {CfShifted, NdVar::scalar(0, 4)});
    S.emit(NdOp::INT_AND, NewCf, {NewCf, NdVar::scalar(1, 1)});
    NdVar Upper = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, Upper, {SrcR, Cnt});
    NdVar CfPos = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, CfPos, {Cnt, NdVar::scalar(1, Sz)});
    NdVar CfIn = S.makeTemp(Sz);
    S.emit(NdOp::INT_LEFT, CfIn, {CfExt, CfPos});
    // WrapAmt = Bits+1-Cnt.  When Cnt==1, WrapAmt==Bits which is UB for
    // a shift-right of Bits-wide value.  Guard with a saturating clamp.
    NdVar WrapAmtRaw = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, WrapAmtRaw, {NdVar::scalar(Bits + 1, Sz), Cnt});
    NdVar WrapOk = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, WrapOk, {WrapAmtRaw, NdVar::scalar(Bits, Sz)});
    NdVar WrapSafe = S.makeTemp(Sz);
    S.emit(NdOp::INT_RIGHT, WrapSafe, {SrcR, WrapAmtRaw});
    NdVar Wrapped = S.makeTemp(Sz);
    S.emit(NdOp::SELECT, Wrapped, {WrapOk, WrapSafe, NdVar::scalar(0, Sz)});
    NdVar Tmp1 = S.makeTemp(Sz);
    S.emit(NdOp::INT_OR, Tmp1, {Upper, CfIn});
    NdVar Candidate = S.makeTemp(Sz);
    S.emit(NdOp::INT_OR, Candidate, {Tmp1, Wrapped});
    emitZeroCountResultGuard(S, Cnt, SrcR, Candidate, Result);
    S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NewCf});
    // OF (1-bit only, left rotate): new CF XOR most-significant result bit.
    NdVar RclOf = S.makeTemp(1);
    S.emit(NdOp::BOOL_XOR, RclOf, {L.extractBit(S, Result, Bits - 1), NewCf});
    L.emitShiftRotateOF(S, Cnt, RclOf);
    L.emitZeroCountFlagGuard(S, Cnt, {{x86reg::CF, OldCF}});
    if (MemDst)
      S.storeToMem(X86.operands[SourceIndex], Result);
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
