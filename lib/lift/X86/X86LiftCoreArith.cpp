//===- X86LiftCoreArith.cpp - x86/x64 integer arithmetic lifter -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Integer arithmetic and the EFLAGS it defines: ADD, SUB,
/// CMP, TEST, AND/OR/XOR, INC/DEC, NEG, NOT, the
/// carry-propagating ADC/SBB, IMUL/MUL, and the
/// sign-extension pairs (CDQ/CQO/CDQE).
///
//===----------------------------------------------------------------------===//

#include "X86LiftAPXValidation.h"
#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

struct ApxCarryArithmetic {
  bool Present = false;
  bool Valid = false;
  bool NDD = false;
  unsigned FirstSource = 0;
  unsigned SecondSource = 1;
};

struct ApxUnaryArithmetic {
  bool Present = false;
  bool Valid = false;
  bool NDD = false;
  bool NF = false;
  unsigned SourceIndex = 0;
};

struct ApxImulArithmetic {
  bool Present = false;
  bool Valid = false;
  bool NDD = false;
  bool NF = false;
  bool Immediate = false;
  bool OneOperand = false;
  bool ZeroUpper = false;
};

ApxCarryArithmetic decodeApxCarryArithmetic(const cs_insn *Insn,
                                            const cs_x86 &X86) {
  ApxCarryArithmetic Result;
  if (!Insn || (Insn->id != X86_INS_ADC && Insn->id != X86_INS_SBB))
    return Result;
  Result.Present = apxvalidation::isPresent(Insn, X86);
  if (!Result.Present)
    return Result;

  size_t Marker = 0;
  while (Marker < Insn->size && Insn->bytes[Marker] != 0x62)
    ++Marker;
  if (Marker + 5 > Insn->size)
    return Result;
  const uint8_t RawP1 = Insn->bytes[Marker + 2];
  const uint8_t RawOpcode = Insn->bytes[Marker + 4];
  unsigned ImmediateBytes = 0;
  if (RawOpcode == 0x80 || RawOpcode == 0x83)
    ImmediateBytes = 1;
  else if (RawOpcode == 0x81)
    ImmediateBytes = (RawP1 & 0x80) != 0 || (RawP1 & 3) == 0 ? 4 : 2;

  apxvalidation::Header H;
  if (!apxvalidation::decodeHeader(Insn, X86, H, ImmediateBytes) ||
      (H.P0 & 7) != 4 || (H.P2 & 0xe7) != 0 || (!H.Memory && (H.P1 & 4) == 0))
    return Result;

  const bool Immediate =
      H.Opcode == 0x80 || H.Opcode == 0x81 || H.Opcode == 0x83;
  const bool Binary = (H.Opcode >= 0x10 && H.Opcode <= 0x13) ||
                      (H.Opcode >= 0x18 && H.Opcode <= 0x1b);
  if (!Immediate && !Binary)
    return Result;
  const bool IsAdc = Binary ? H.Opcode < 0x18 : ((H.ModRM >> 3) & 7) == 2;
  const bool IsSbb = Binary ? H.Opcode >= 0x18 : ((H.ModRM >> 3) & 7) == 3;
  if ((Insn->id == X86_INS_ADC && !IsAdc) ||
      (Insn->id == X86_INS_SBB && !IsSbb) || (!IsAdc && !IsSbb))
    return Result;

  unsigned Width = 0;
  if ((Binary && (H.Opcode & 1) == 0) || H.Opcode == 0x80) {
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
  if (!Result.NDD && apxvalidation::vvvvv(H) != 0)
    return Result;
  const unsigned ExpectedOperands = Result.NDD ? 3 : 2;
  if (X86.op_count != ExpectedOperands)
    return Result;

  if (Result.NDD) {
    if (!apxvalidation::registerOperand(
            X86.operands[0], apxvalidation::vvvvv(H), Width, CS_AC_WRITE))
      return Result;
    Result.FirstSource = 1;
    Result.SecondSource = 2;
  }

  if (Immediate) {
    const unsigned RmIndex = Result.NDD ? 1 : 0;
    if (!apxvalidation::validateRM(
            Insn, X86, H, X86.operands[RmIndex], Width,
            Result.NDD ? CS_AC_READ : CS_AC_READ | CS_AC_WRITE, ImmediateBytes))
      return Result;
    uint32_t Raw = 0;
    for (unsigned I = 0; I != ImmediateBytes; ++I)
      Raw |= static_cast<uint32_t>(Insn->bytes[Insn->size - ImmediateBytes + I])
             << (I * 8);
    int64_t ExpectedImmediate = 0;
    if (H.Opcode == 0x83)
      ExpectedImmediate = static_cast<int8_t>(Raw);
    else if (Width == 8)
      ExpectedImmediate = static_cast<int32_t>(Raw);
    else if (Width == 2)
      ExpectedImmediate = static_cast<uint16_t>(Raw);
    else if (Width == 1)
      ExpectedImmediate = static_cast<uint8_t>(Raw);
    else
      ExpectedImmediate = Raw;
    if (!apxvalidation::immediateOperand(
            Insn, X86.operands[ExpectedOperands - 1], ImmediateBytes, Width,
            ExpectedImmediate))
      return Result;
  } else {
    const bool Reverse = (H.Opcode & 2) != 0;
    if (Result.NDD) {
      if (Reverse) {
        if (!apxvalidation::registerOperand(X86.operands[1],
                                            apxvalidation::modrmReg(H), Width,
                                            CS_AC_READ) ||
            !apxvalidation::validateRM(Insn, X86, H, X86.operands[2], Width,
                                       CS_AC_READ))
          return Result;
      } else if (!apxvalidation::validateRM(Insn, X86, H, X86.operands[1],
                                            Width, CS_AC_READ) ||
                 !apxvalidation::registerOperand(X86.operands[2],
                                                 apxvalidation::modrmReg(H),
                                                 Width, CS_AC_READ)) {
        return Result;
      }
    } else if (Reverse) {
      if (!apxvalidation::registerOperand(X86.operands[0],
                                          apxvalidation::modrmReg(H), Width,
                                          CS_AC_READ | CS_AC_WRITE) ||
          !apxvalidation::validateRM(Insn, X86, H, X86.operands[1], Width,
                                     CS_AC_READ))
        return Result;
    } else if (!apxvalidation::validateRM(Insn, X86, H, X86.operands[0], Width,
                                          CS_AC_READ | CS_AC_WRITE) ||
               !apxvalidation::registerOperand(X86.operands[1],
                                               apxvalidation::modrmReg(H),
                                               Width, CS_AC_READ)) {
      return Result;
    }
  }

  const uint64_t ExpectedFlags =
      X86_EFLAGS_MODIFY_OF | X86_EFLAGS_MODIFY_SF | X86_EFLAGS_MODIFY_ZF |
      (Insn->id == X86_INS_ADC ? X86_EFLAGS_MODIFY_AF
                               : X86_EFLAGS_UNDEFINED_AF) |
      X86_EFLAGS_MODIFY_PF | X86_EFLAGS_MODIFY_CF | X86_EFLAGS_TEST_CF;
  if (!apxvalidation::implicitDetail(Insn, X86, ExpectedFlags, {X86_REG_EFLAGS},
                                     {X86_REG_EFLAGS}))
    return Result;
  Result.Valid = true;
  return Result;
}

ApxUnaryArithmetic decodeApxUnaryArithmetic(const cs_insn *Insn,
                                            const cs_x86 &X86) {
  ApxUnaryArithmetic Result;
  switch (Insn ? Insn->id : X86_INS_INVALID) {
  case X86_INS_INC:
  case X86_INS_DEC:
  case X86_INS_NEG:
  case X86_INS_NOT:
  case X86_INS_MUL:
    break;
  default:
    return Result;
  }
  Result.Present = apxvalidation::isPresent(Insn, X86);
  if (!Result.Present)
    return Result;

  apxvalidation::Header H;
  if (!apxvalidation::decodeHeader(Insn, X86, H) || (H.P0 & 7) != 4 ||
      (H.P2 & 0xe3) != 0 || (!H.Memory && (H.P1 & 4) == 0) || (H.P1 & 3) > 1)
    return Result;
  const unsigned Group = (H.ModRM >> 3) & 7;
  const bool ByteOpcode = H.Opcode == 0xfe || H.Opcode == 0xf6;
  const bool ScalableOpcode = H.Opcode == 0xff || H.Opcode == 0xf7;
  if (!ByteOpcode && !ScalableOpcode)
    return Result;
  if ((H.Opcode == 0xfe || H.Opcode == 0xff) &&
      ((Insn->id == X86_INS_INC && Group != 0) ||
       (Insn->id == X86_INS_DEC && Group != 1) ||
       (Insn->id != X86_INS_INC && Insn->id != X86_INS_DEC)))
    return Result;
  if ((H.Opcode == 0xf6 || H.Opcode == 0xf7) &&
      ((Insn->id == X86_INS_NOT && Group != 2) ||
       (Insn->id == X86_INS_NEG && Group != 3) ||
       (Insn->id == X86_INS_MUL && Group != 4) ||
       (Insn->id != X86_INS_NOT && Insn->id != X86_INS_NEG &&
        Insn->id != X86_INS_MUL)))
    return Result;

  unsigned Width = 0;
  if (ByteOpcode) {
    if ((H.P1 & 3) != 0)
      return Result;
    Width = 1;
  } else if (H.P1 & 0x80) {
    Width = 8;
  } else if ((H.P1 & 3) == 1) {
    Width = 2;
  } else {
    Width = 4;
  }

  Result.NDD = (H.P2 & 0x10) != 0;
  Result.NF = (H.P2 & 4) != 0;
  if ((!Result.NDD && apxvalidation::vvvvv(H) != 0) ||
      (Insn->id == X86_INS_MUL && Result.NDD) ||
      (Insn->id == X86_INS_NOT && Result.NF))
    return Result;
  const unsigned ExpectedOperands = Result.NDD ? 2 : 1;
  if (X86.op_count != ExpectedOperands)
    return Result;
  if (Result.NDD) {
    if (!apxvalidation::registerOperand(
            X86.operands[0], apxvalidation::vvvvv(H), Width, CS_AC_WRITE))
      return Result;
    Result.SourceIndex = 1;
  }
  const uint8_t SourceAccess =
      Result.NDD || Insn->id == X86_INS_MUL
          ? CS_AC_READ
          : static_cast<uint8_t>(CS_AC_READ | CS_AC_WRITE);
  if (!apxvalidation::validateRM(Insn, X86, H, X86.operands[Result.SourceIndex],
                                 Width, SourceAccess))
    return Result;

  uint64_t ExpectedFlags = 0;
  if (!Result.NF) {
    if (Insn->id == X86_INS_INC || Insn->id == X86_INS_DEC)
      ExpectedFlags = X86_EFLAGS_MODIFY_OF | X86_EFLAGS_MODIFY_SF |
                      X86_EFLAGS_MODIFY_ZF | X86_EFLAGS_MODIFY_AF |
                      X86_EFLAGS_MODIFY_PF;
    else if (Insn->id == X86_INS_NEG)
      ExpectedFlags = X86_EFLAGS_MODIFY_OF | X86_EFLAGS_MODIFY_SF |
                      X86_EFLAGS_MODIFY_ZF | X86_EFLAGS_MODIFY_AF |
                      X86_EFLAGS_MODIFY_PF | X86_EFLAGS_MODIFY_CF;
    else if (Insn->id == X86_INS_MUL)
      ExpectedFlags = X86_EFLAGS_MODIFY_OF | X86_EFLAGS_UNDEFINED_SF |
                      X86_EFLAGS_UNDEFINED_ZF | X86_EFLAGS_UNDEFINED_AF |
                      X86_EFLAGS_UNDEFINED_PF | X86_EFLAGS_MODIFY_CF;
  }
  if (Insn->id == X86_INS_MUL) {
    const x86_reg Low = Width == 1   ? X86_REG_AL
                        : Width == 2 ? X86_REG_AX
                        : Width == 4 ? X86_REG_EAX
                                     : X86_REG_RAX;
    const x86_reg High = Width == 1   ? X86_REG_AH
                         : Width == 2 ? X86_REG_DX
                         : Width == 4 ? X86_REG_EDX
                                      : X86_REG_RDX;
    const bool DetailValid =
        Result.NF
            ? apxvalidation::implicitDetail(Insn, X86, 0, {Low}, {Low, High})
            : apxvalidation::implicitDetail(Insn, X86, ExpectedFlags, {Low},
                                            {Low, High, X86_REG_EFLAGS});
    if (!DetailValid)
      return Result;
  } else if (!apxvalidation::implicitDetail(
                 Insn, X86, ExpectedFlags, {},
                 ExpectedFlags == 0
                     ? std::initializer_list<x86_reg>{}
                     : std::initializer_list<x86_reg>{X86_REG_EFLAGS})) {
    return Result;
  }
  Result.Valid = true;
  return Result;
}

ApxImulArithmetic decodeApxImulArithmetic(const cs_insn *Insn,
                                          const cs_x86 &X86) {
  ApxImulArithmetic Result;
  if (!Insn || Insn->id != X86_INS_IMUL)
    return Result;
  Result.Present = apxvalidation::isPresent(Insn, X86);
  if (!Result.Present)
    return Result;

  size_t Marker = 0;
  while (Marker < Insn->size && Insn->bytes[Marker] != 0x62)
    ++Marker;
  if (Marker + 5 > Insn->size)
    return Result;
  const uint8_t RawP1 = Insn->bytes[Marker + 2];
  const uint8_t RawOpcode = Insn->bytes[Marker + 4];
  unsigned ImmediateBytes = 0;
  if (RawOpcode == 0x6b)
    ImmediateBytes = 1;
  else if (RawOpcode == 0x69)
    ImmediateBytes = (RawP1 & 0x80) != 0 || (RawP1 & 3) == 0 ? 4 : 2;

  apxvalidation::Header H;
  if (!apxvalidation::decodeHeader(Insn, X86, H, ImmediateBytes) ||
      (H.P0 & 7) != 4 || (H.P1 & 3) > 1 || (H.P2 & 0xe3) != 0 ||
      (!H.Memory && (H.P1 & 4) == 0))
    return Result;

  Result.NF = (H.P2 & 4) != 0;
  const uint64_t ExpectedFlags =
      Result.NF ? 0
                : X86_EFLAGS_MODIFY_OF | X86_EFLAGS_UNDEFINED_SF |
                      X86_EFLAGS_UNDEFINED_ZF | X86_EFLAGS_UNDEFINED_AF |
                      X86_EFLAGS_UNDEFINED_PF | X86_EFLAGS_MODIFY_CF;

  if (H.Opcode == 0xf6 || H.Opcode == 0xf7) {
    const bool ByteOpcode = H.Opcode == 0xf6;
    if (((H.ModRM >> 3) & 7) != 5 || (H.P2 & 0x10) != 0 ||
        apxvalidation::vvvvv(H) != 0 || (ByteOpcode && (H.P1 & 3) != 0))
      return Result;
    const unsigned Width = ByteOpcode           ? 1
                           : (H.P1 & 0x80) != 0 ? 8
                           : (H.P1 & 3) == 1    ? 2
                                                : 4;
    if (X86.op_count != 1 ||
        !apxvalidation::validateRM(Insn, X86, H, X86.operands[0], Width,
                                   CS_AC_READ))
      return Result;
    const x86_reg Low = Width == 1   ? X86_REG_AL
                        : Width == 2 ? X86_REG_AX
                        : Width == 4 ? X86_REG_EAX
                                     : X86_REG_RAX;
    const x86_reg High = Width == 1   ? X86_REG_AH
                         : Width == 2 ? X86_REG_DX
                         : Width == 4 ? X86_REG_EDX
                                      : X86_REG_RDX;
    const bool DetailValid =
        Result.NF
            ? apxvalidation::implicitDetail(Insn, X86, 0, {Low}, {Low, High})
            : apxvalidation::implicitDetail(Insn, X86, ExpectedFlags, {Low},
                                            {Low, High, X86_REG_EFLAGS});
    if (!DetailValid)
      return Result;
    Result.OneOperand = true;
    Result.Valid = true;
    return Result;
  }

  const unsigned Width = (H.P1 & 0x80) != 0 ? 8 : (H.P1 & 3) == 1 ? 2 : 4;
  if (H.Opcode == 0x69 || H.Opcode == 0x6b) {
    if (apxvalidation::vvvvv(H) != 0 || X86.op_count != 3 ||
        !apxvalidation::registerOperand(
            X86.operands[0], apxvalidation::modrmReg(H), Width, CS_AC_WRITE) ||
        !apxvalidation::validateRM(Insn, X86, H, X86.operands[1], Width,
                                   CS_AC_READ, ImmediateBytes))
      return Result;

    uint32_t RawImmediate = 0;
    for (unsigned I = 0; I != ImmediateBytes; ++I)
      RawImmediate |=
          static_cast<uint32_t>(Insn->bytes[Insn->size - ImmediateBytes + I])
          << (I * 8);
    const int64_t ExpectedImmediate =
        ImmediateBytes == 1   ? static_cast<int8_t>(RawImmediate)
        : ImmediateBytes == 2 ? static_cast<int16_t>(RawImmediate)
                              : static_cast<int32_t>(RawImmediate);
    if (!apxvalidation::immediateOperand(Insn, X86.operands[2], ImmediateBytes,
                                         Width, ExpectedImmediate) ||
        !apxvalidation::implicitDetail(
            Insn, X86, ExpectedFlags, {},
            Result.NF ? std::initializer_list<x86_reg>{}
                      : std::initializer_list<x86_reg>{X86_REG_EFLAGS}))
      return Result;
    Result.Immediate = true;
    Result.ZeroUpper = (H.P2 & 0x10) != 0;
    Result.Valid = true;
    return Result;
  }

  if (H.Opcode != 0xaf)
    return Result;
  Result.NDD = (H.P2 & 0x10) != 0;
  if (!Result.NDD && apxvalidation::vvvvv(H) != 0)
    return Result;
  const unsigned ExpectedOperands = Result.NDD ? 3 : 2;
  if (X86.op_count != ExpectedOperands)
    return Result;
  if (Result.NDD) {
    if (!apxvalidation::registerOperand(
            X86.operands[0], apxvalidation::vvvvv(H), Width, CS_AC_WRITE) ||
        !apxvalidation::registerOperand(
            X86.operands[1], apxvalidation::modrmReg(H), Width, CS_AC_READ) ||
        !apxvalidation::validateRM(Insn, X86, H, X86.operands[2], Width,
                                   CS_AC_READ))
      return Result;
  } else if (!apxvalidation::registerOperand(X86.operands[0],
                                             apxvalidation::modrmReg(H), Width,
                                             CS_AC_READ | CS_AC_WRITE) ||
             !apxvalidation::validateRM(Insn, X86, H, X86.operands[1], Width,
                                        CS_AC_READ)) {
    return Result;
  }
  if (!apxvalidation::implicitDetail(
          Insn, X86, ExpectedFlags, {},
          Result.NF ? std::initializer_list<x86_reg>{}
                    : std::initializer_list<x86_reg>{X86_REG_EFLAGS}))
    return Result;
  Result.Valid = true;
  return Result;
}

bool decodeApxPromotedModifiers(const cs_insn *Insn, bool &Ndd, bool &Nf) {
  if (!Insn || Insn->size < 6)
    return false;
  size_t Offset = 0;
  while (Offset < Insn->size && Insn->bytes[Offset] != 0x62) {
    switch (Insn->bytes[Offset]) {
    case 0x26:
    case 0x2e:
    case 0x36:
    case 0x3e:
    case 0x64:
    case 0x65:
    case 0x67:
      ++Offset;
      break;
    default:
      return false;
    }
  }
  if (Offset + 6 != Insn->size || Insn->bytes[Offset] != 0x62 ||
      (Insn->bytes[Offset + 1] & 0x07) != 0x04)
    return false;
  Ndd = (Insn->bytes[Offset + 3] & 0x10) != 0;
  Nf = (Insn->bytes[Offset + 3] & 0x04) != 0;
  return true;
}

} // namespace

bool liftCoreArith(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86) {
  auto readArithmeticOperand = [&](const cs_x86_op &Operand) {
    NdVar Value = L.operandRead(S, Operand);
    // Encoded arithmetic immediates are scalar bit patterns unless the loader
    // attached provenance to this exact immediate field.  Keeping the exact
    // occurrence override lets `cmp $symbol,%reg` remain relocatable while a
    // loop bound that merely equals a low image VA stays numeric.
    if (Operand.type == X86_OP_IMM && Value.isConst() &&
        Value.Provenance == ConstantAddressProvenance::Unknown)
      Value.Provenance = ConstantAddressProvenance::Scalar;
    return Value;
  };
  unsigned InsnId = Insn->id;

  const ApxCarryArithmetic ApxCarry = decodeApxCarryArithmetic(Insn, X86);
  if (ApxCarry.Present && !ApxCarry.Valid)
    return false;
  const ApxUnaryArithmetic ApxUnary = decodeApxUnaryArithmetic(Insn, X86);
  if (ApxUnary.Present && !ApxUnary.Valid)
    return false;
  const ApxImulArithmetic ApxImul = decodeApxImulArithmetic(Insn, X86);
  if (ApxImul.Present && !ApxImul.Valid)
    return false;

  bool ApxNdd = false;
  bool ApxNf = false;
  const bool IsApxPromoted = decodeApxPromotedModifiers(Insn, ApxNdd, ApxNf);
  if (ApxCarry.Valid) {
    const NdVar DestinationWrite = L.operandWrite(X86.operands[0]);
    NdVar A = S.makeTemp(X86.operands[0].size);
    S.emit(NdOp::COPY, A,
           {readArithmeticOperand(X86.operands[ApxCarry.FirstSource])});
    NdVar B = S.makeTemp(X86.operands[0].size);
    S.emit(NdOp::COPY, B,
           {readArithmeticOperand(X86.operands[ApxCarry.SecondSource])});
    const bool MemoryDestination = X86.operands[0].type == X86_OP_MEM;
    const NdVar Destination =
        MemoryDestination ? S.makeTemp(A.Size) : DestinationWrite;
    NdVar CfExt = snapshotCarryAtWidth(S, A.Size);
    NdVar Adjusted = S.makeTemp(A.Size);
    S.emit(NdOp::INT_ADD, Adjusted, {B, CfExt});

    if (InsnId == X86_INS_ADC) {
      NdVar CarryInner = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, CarryInner, {B, CfExt});
      S.emit(NdOp::INT_ADD, Destination, {A, Adjusted});
      NdVar CarryOuter = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, CarryOuter, {A, Adjusted});
      S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1),
             {CarryInner, CarryOuter});
      NdVar OverflowInner = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, OverflowInner, {B, CfExt});
      NdVar OverflowOuter = S.makeTemp(1);
      S.emit(NdOp::INT_SOVF, OverflowOuter, {A, Adjusted});
      S.emit(NdOp::BOOL_XOR, NdVar::reg(x86reg::OF, 1),
             {OverflowInner, OverflowOuter});
    } else {
      NdVar CarryInner = S.makeTemp(1);
      S.emit(NdOp::INT_CARRY, CarryInner, {B, CfExt});
      S.emit(NdOp::INT_SUB, Destination, {A, Adjusted});
      NdVar BorrowOuter = S.makeTemp(1);
      S.emit(NdOp::INT_LESS, BorrowOuter, {A, Adjusted});
      S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1),
             {CarryInner, BorrowOuter});
      NdVar Difference = S.makeTemp(A.Size);
      S.emit(NdOp::INT_SUB, Difference, {A, B});
      NdVar OverflowInner = S.makeTemp(1);
      S.emit(NdOp::INT_SBOR, OverflowInner, {A, B});
      NdVar OverflowOuter = S.makeTemp(1);
      S.emit(NdOp::INT_SBOR, OverflowOuter, {Difference, CfExt});
      S.emit(NdOp::BOOL_XOR, NdVar::reg(x86reg::OF, 1),
             {OverflowInner, OverflowOuter});
    }
    L.emitZSPF(S, Destination);
    L.emitAF(S, Destination, A, B);
    if (MemoryDestination)
      S.storeToMem(X86.operands[0], Destination);
    return true;
  }
  if (IsApxPromoted &&
      (InsnId == X86_INS_ADD || InsnId == X86_INS_OR || InsnId == X86_INS_AND ||
       InsnId == X86_INS_SUB || InsnId == X86_INS_XOR)) {
    const unsigned ExpectedOperands = ApxNdd ? 3 : 2;
    if (X86.op_count != ExpectedOperands)
      return false;
    for (unsigned Index = 0; Index < ExpectedOperands; ++Index)
      if (X86.operands[Index].type != X86_OP_REG ||
          X86.operands[Index].size != X86.operands[0].size)
        return false;
    if (X86.operands[0].size != 1 && X86.operands[0].size != 2 &&
        X86.operands[0].size != 4 && X86.operands[0].size != 8)
      return false;

    const unsigned FirstSourceIndex = ApxNdd ? 1 : 0;
    const unsigned SecondSourceIndex = ApxNdd ? 2 : 1;
    const NdVar FirstRead =
        readArithmeticOperand(X86.operands[FirstSourceIndex]);
    const NdVar SecondRead =
        readArithmeticOperand(X86.operands[SecondSourceIndex]);
    const NdVar Destination = L.operandWrite(X86.operands[0]);
    if (FirstRead.Size != Destination.Size ||
        SecondRead.Size != Destination.Size)
      return false;

    NdVar First = S.makeTemp(Destination.Size);
    NdVar Second = S.makeTemp(Destination.Size);
    S.emit(NdOp::COPY, First, {FirstRead});
    S.emit(NdOp::COPY, Second, {SecondRead});
    NdOp Opcode = NdOp::INT_ADD;
    if (InsnId == X86_INS_OR)
      Opcode = NdOp::INT_OR;
    else if (InsnId == X86_INS_AND)
      Opcode = NdOp::INT_AND;
    else if (InsnId == X86_INS_SUB)
      Opcode = NdOp::INT_SUB;
    else if (InsnId == X86_INS_XOR)
      Opcode = NdOp::INT_XOR;
    S.emit(Opcode, Destination, {First, Second});

    if (!ApxNf) {
      if (InsnId == X86_INS_ADD || InsnId == X86_INS_SUB)
        L.emitFlagsArith(S, Destination, First, Second, InsnId == X86_INS_SUB);
      else
        L.emitFlagsLogic(S, Destination);
    }
    return true;
  }

  switch (InsnId) {

  // --- ADD ---
  case X86_INS_ADD: {
    if (X86.op_count < 2)
      break;
    NdVar Src = readArithmeticOperand(X86.operands[1]);
    NdVar DstR = readArithmeticOperand(X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    // Snapshot operands into temps before INT_ADD writes to DstW, so that
    // sub-register aliasing in LowToMed cannot redirect the flag operands
    // to the post-add value (fixes byte-level addb OF bug).
    NdVar FlagA = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, FlagA, {DstR});
    NdVar FlagB = S.makeTemp(Src.Size);
    S.emit(NdOp::COPY, FlagB, {Src});
    NdVar Result =
        (X86.operands[0].type == X86_OP_MEM) ? S.makeTemp(DstR.Size) : DstW;
    S.emit(NdOp::INT_ADD, Result, {DstR, Src});
    L.emitFlagsArith(S, Result, FlagA, FlagB, false);
    if (X86.operands[0].type == X86_OP_MEM)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- SUB ---
  case X86_INS_SUB: {
    if (X86.op_count < 2)
      break;
    NdVar Src = readArithmeticOperand(X86.operands[1]);
    NdVar DstR = readArithmeticOperand(X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar FlagA = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, FlagA, {DstR});
    NdVar FlagB = S.makeTemp(Src.Size);
    S.emit(NdOp::COPY, FlagB, {Src});
    NdVar Result =
        (X86.operands[0].type == X86_OP_MEM) ? S.makeTemp(DstR.Size) : DstW;
    S.emit(NdOp::INT_SUB, Result, {DstR, Src});
    L.emitFlagsArith(S, Result, FlagA, FlagB, true);
    if (X86.operands[0].type == X86_OP_MEM)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- CMP ---
  case X86_INS_CMP: {
    if (X86.op_count < 2)
      break;
    NdVar A = readArithmeticOperand(X86.operands[0]);
    NdVar B = readArithmeticOperand(X86.operands[1]);
    NdVar TmpR = S.makeTemp(A.Size);
    S.emit(NdOp::INT_SUB, TmpR, {A, B});
    L.emitFlagsArith(S, TmpR, A, B, true);
    break;
  }

  // --- TEST ---
  case X86_INS_TEST: {
    if (X86.op_count < 2)
      break;
    NdVar A = readArithmeticOperand(X86.operands[0]);
    NdVar B = readArithmeticOperand(X86.operands[1]);
    NdVar TmpR = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, TmpR, {A, B});
    L.emitFlagsLogic(S, TmpR);
    break;
  }

  // --- AND / OR / XOR ---
  case X86_INS_AND:
  case X86_INS_OR:
  case X86_INS_XOR: {
    if (X86.op_count < 2)
      break;

    // Idiom: xor reg, reg → COPY reg = 0 (avoids false live-in)
    if (InsnId == X86_INS_XOR && X86.operands[0].type == X86_OP_REG &&
        X86.operands[1].type == X86_OP_REG &&
        X86.operands[0].reg == X86.operands[1].reg) {
      NdVar DstW = L.operandWrite(X86.operands[0]);
      S.emit(NdOp::COPY, DstW, {NdVar::scalar(0, DstW.Size)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {NdVar::scalar(1, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::SF, 1), {NdVar::scalar(0, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::PF, 1), {NdVar::scalar(1, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::CF, 1), {NdVar::scalar(0, 1)});
      S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::scalar(0, 1)});
      break;
    }

    NdVar Src = readArithmeticOperand(X86.operands[1]);
    NdVar DstR = readArithmeticOperand(X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);

    // Idiom: test reg, reg (AND with itself) → just set flags, don't overwrite
    // reg
    if (InsnId == X86_INS_AND && X86.operands[0].type == X86_OP_REG &&
        X86.operands[1].type == X86_OP_REG &&
        X86.operands[0].reg == X86.operands[1].reg) {
      NdVar TmpV = S.makeTemp(DstR.Size);
      S.emit(NdOp::INT_AND, TmpV, {DstR, Src});
      L.emitFlagsLogic(S, TmpV);
      break;
    }

    NdOp Opc = NdOp::INT_AND;
    if (InsnId == X86_INS_OR)
      Opc = NdOp::INT_OR;
    if (InsnId == X86_INS_XOR)
      Opc = NdOp::INT_XOR;

    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    S.emit(Opc, Result, {DstR, Src});
    L.emitFlagsLogic(S, Result);
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- INC / DEC ---
  case X86_INS_INC:
  case X86_INS_DEC: {
    if (X86.op_count < 1)
      break;
    const unsigned SourceIndex = ApxUnary.Valid ? ApxUnary.SourceIndex : 0;
    const bool SuppressFlags = ApxUnary.Valid && ApxUnary.NF;
    NdVar DstR = readArithmeticOperand(X86.operands[SourceIndex]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar One = NdVar::scalar(1, DstR.Size);
    bool IsInc = (InsnId == X86_INS_INC);
    bool MemDst =
        SourceIndex == 0 && X86.operands[SourceIndex].type == X86_OP_MEM;
    // Snapshot the source before INT_ADD/INT_SUB writes DstW: for a register
    // operand DstR and DstW alias the same reg, so a later AF read of DstR
    // would see the post-update value (sub-register aliasing, cf. ADD).
    NdVar PreVal = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, PreVal, {DstR});
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    S.emit(IsInc ? NdOp::INT_ADD : NdOp::INT_SUB, Result, {DstR, One});
    if (!SuppressFlags) {
      L.emitZSPF(S, Result);
      L.emitAF(S, Result, PreVal, One);
      // OF must use the pre-update source (PreVal); for a register operand a
      // bare DstR read here is redirected to the post-update value.
      if (IsInc)
        S.emit(NdOp::INT_SOVF, NdVar::reg(x86reg::OF, 1), {PreVal, One});
      else
        S.emit(NdOp::INT_SBOR, NdVar::reg(x86reg::OF, 1), {PreVal, One});
    }
    if (MemDst)
      S.storeToMem(X86.operands[SourceIndex], Result);
    break;
  }

  // --- NEG / NOT ---
  case X86_INS_NEG: {
    if (X86.op_count < 1)
      break;
    const unsigned SourceIndex = ApxUnary.Valid ? ApxUnary.SourceIndex : 0;
    const bool SuppressFlags = ApxUnary.Valid && ApxUnary.NF;
    NdVar DstR = readArithmeticOperand(X86.operands[SourceIndex]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    bool MemDst =
        SourceIndex == 0 && X86.operands[SourceIndex].type == X86_OP_MEM;
    // Snapshot before INT_NEG2 overwrites DstW (register NEG aliases DstR).
    NdVar PreVal = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, PreVal, {DstR});
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    S.emit(NdOp::INT_NEG2, Result, {DstR});
    if (!SuppressFlags) {
      // CF/OF use the pre-update source (PreVal): a register NEG aliases
      // DstR/DstW so a post-2COMP read of DstR would be the negated value.
      S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
             {PreVal, NdVar::scalar(0, DstR.Size)});
      L.emitZSPF(S, Result);
      L.emitAF(S, Result, NdVar::scalar(0, DstR.Size), PreVal);
      S.emit(NdOp::INT_SBOR, NdVar::reg(x86reg::OF, 1),
             {NdVar::scalar(0, DstR.Size), PreVal});
    }
    if (MemDst)
      S.storeToMem(X86.operands[SourceIndex], Result);
    break;
  }
  case X86_INS_NOT: {
    if (X86.op_count < 1)
      break;
    const unsigned SourceIndex = ApxUnary.Valid ? ApxUnary.SourceIndex : 0;
    NdVar DstR = readArithmeticOperand(X86.operands[SourceIndex]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    bool MemDst =
        SourceIndex == 0 && X86.operands[SourceIndex].type == X86_OP_MEM;
    NdVar Result = MemDst ? S.makeTemp(DstR.Size) : DstW;
    S.emit(NdOp::INT_NOT, Result, {DstR});
    if (MemDst)
      S.storeToMem(X86.operands[SourceIndex], Result);
    break;
  }

  // --- IMUL ---
  case X86_INS_IMUL: {
    if (X86.op_count == 1) {
      const bool SuppressFlags =
          ApxImul.Valid && ApxImul.OneOperand && ApxImul.NF;
      NdVar Src = readArithmeticOperand(X86.operands[0]);
      uint16_t Sz = Src.Size;
      if (Sz == 1) {
        // 8-bit: AX = AL * r/m8 (signed, Result in AX)
        NdVar Al = NdVar::reg(x86reg::RAX, 1);
        NdVar Ax = NdVar::reg(x86reg::RAX, 2);
        NdVar ExtA = S.makeTemp(2);
        NdVar ExtB = S.makeTemp(2);
        S.emit(NdOp::INT_SEXT, ExtA, {Al});
        S.emit(NdOp::INT_SEXT, ExtB, {Src});
        S.emit(NdOp::INT_MULT, Ax, {ExtA, ExtB});
        if (!SuppressFlags) {
          NdVar LowSext = S.makeTemp(2);
          S.emit(NdOp::INT_SEXT, LowSext, {NdVar::reg(x86reg::RAX, 1)});
          S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1), {LowSext, Ax});
          S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1),
                 {NdVar::reg(x86reg::CF, 1)});
        }
      } else {
        NdVar Rax = NdVar::reg(x86reg::RAX, Sz);
        NdVar Rdx = NdVar::reg(x86reg::RDX, Sz);
        NdVar ExtA = S.makeTemp(Sz * 2);
        NdVar ExtB = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_SEXT, ExtA, {Rax});
        S.emit(NdOp::INT_SEXT, ExtB, {Src});
        NdVar Full = S.makeTemp(Sz * 2);
        S.emit(NdOp::INT_MULT, Full, {ExtA, ExtB});
        S.emit(NdOp::SUBBYTES, Rax, {Full, NdVar::scalar(0, 4)});
        S.emit(NdOp::SUBBYTES, Rdx, {Full, NdVar::scalar(Sz, 4)});
        if (!SuppressFlags) {
          NdVar LowSext = S.makeTemp(Sz * 2);
          S.emit(NdOp::INT_SEXT, LowSext, {Rax});
          S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
                 {LowSext, Full});
          S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1),
                 {NdVar::reg(x86reg::CF, 1)});
        }
      }
      break;
    }
    NdVar Dst{}, MulA{}, MulB{};
    if (X86.op_count == 2) {
      MulA = readArithmeticOperand(X86.operands[0]);
      MulB = readArithmeticOperand(X86.operands[1]);
      Dst = L.operandWrite(X86.operands[0]);
    } else if (X86.op_count == 3) {
      MulA = readArithmeticOperand(X86.operands[1]);
      MulB = readArithmeticOperand(X86.operands[2]);
      Dst = L.operandWrite(X86.operands[0]);
    } else {
      break;
    }
    const bool ZeroUpper =
        ApxImul.Valid && ApxImul.Immediate && ApxImul.ZeroUpper && Dst.Size < 8;
    const NdVar Result = ZeroUpper ? S.makeTemp(Dst.Size) : Dst;
    auto emitZeroUpperWriteback = [&] {
      if (!ZeroUpper)
        return;
      NdVar Wide = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, Wide, {Result});
      S.emit(NdOp::COPY, NdVar::reg(Dst.Offset, 8), {Wide});
    };
    if (ApxImul.Valid && ApxImul.NF) {
      S.emit(NdOp::INT_MULT, Result, {MulA, MulB});
      emitZeroUpperWriteback();
      break;
    }
    // CF=OF: widen both operands to double-size, multiply, then compare the
    // sign-extended truncated Result against the Full product.  The operands
    // are sign-extended (and the full product formed) BEFORE Dst is written, so
    // the flags stay correct when Dst aliases a source (`imul r,r/m`/`imul
    // r,r,imm`). The result keeps a native-width INT_MULT — only the flag path
    // uses the double-width product, so it is dropped when flags are dead (no
    // i128 lib call for a 64-bit imul whose flags are unused).
    NdVar ExtA = S.makeTemp(Dst.Size * 2);
    NdVar ExtB = S.makeTemp(Dst.Size * 2);
    S.emit(NdOp::INT_SEXT, ExtA, {MulA});
    S.emit(NdOp::INT_SEXT, ExtB, {MulB});
    NdVar Full = S.makeTemp(Dst.Size * 2);
    S.emit(NdOp::INT_MULT, Full, {ExtA, ExtB});
    S.emit(NdOp::INT_MULT, Result, {MulA, MulB});
    NdVar ExtRes = S.makeTemp(Dst.Size * 2);
    S.emit(NdOp::INT_SEXT, ExtRes, {Result});
    S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1), {ExtRes, Full});
    S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1), {NdVar::reg(x86reg::CF, 1)});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {Result, NdVar::scalar(0, Dst.Size)});
    S.emit(NdOp::INT_SLESS, NdVar::reg(x86reg::SF, 1),
           {Result, NdVar::scalar(0, Dst.Size)});
    emitZeroUpperWriteback();
    break;
  }

  // --- CDQ/CDQE/CQO ---
  case X86_INS_CDQ:
    S.emit(NdOp::INT_ASHR, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RAX, 4), NdVar::scalar(31, 4)});
    break;
  case X86_INS_CQO:
    S.emit(NdOp::INT_ASHR, NdVar::reg(x86reg::RDX, 8),
           {NdVar::reg(x86reg::RAX, 8), NdVar::scalar(63, 8)});
    break;
  case X86_INS_CDQE:
    S.emit(NdOp::INT_SEXT, NdVar::reg(x86reg::RAX, 8),
           {NdVar::reg(x86reg::RAX, 4)});
    break;

  // --- MUL / DIV ---
  case X86_INS_MUL: {
    if (X86.op_count < 1)
      break;
    const unsigned SourceIndex = ApxUnary.Valid ? ApxUnary.SourceIndex : 0;
    const bool SuppressFlags = ApxUnary.Valid && ApxUnary.NF;
    NdVar Src = readArithmeticOperand(X86.operands[SourceIndex]);
    uint16_t Sz = Src.Size;
    if (Sz == 1) {
      // 8-bit: AX = AL * r/m8 (Result in AX, not DL:AL)
      NdVar Al = NdVar::reg(x86reg::RAX, 1);
      NdVar Ax = NdVar::reg(x86reg::RAX, 2);
      NdVar ExtA = S.makeTemp(2);
      NdVar ExtB = S.makeTemp(2);
      S.emit(NdOp::INT_ZEXT, ExtA, {Al});
      S.emit(NdOp::INT_ZEXT, ExtB, {Src});
      S.emit(NdOp::INT_MULT, Ax, {ExtA, ExtB});
      if (!SuppressFlags) {
        NdVar Ah = NdVar::reg(x86reg::RAX + 1, 1);
        S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
               {Ah, NdVar::scalar(0, 1)});
        S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1),
               {NdVar::reg(x86reg::CF, 1)});
      }
    } else {
      NdVar Rax = NdVar::reg(x86reg::RAX, Sz);
      NdVar Rdx = NdVar::reg(x86reg::RDX, Sz);
      NdVar ExtA = S.makeTemp(Sz * 2);
      NdVar ExtB = S.makeTemp(Sz * 2);
      S.emit(NdOp::INT_ZEXT, ExtA, {Rax});
      S.emit(NdOp::INT_ZEXT, ExtB, {Src});
      NdVar Full = S.makeTemp(Sz * 2);
      S.emit(NdOp::INT_MULT, Full, {ExtA, ExtB});
      S.emit(NdOp::SUBBYTES, Rax, {Full, NdVar::scalar(0, 4)});
      S.emit(NdOp::SUBBYTES, Rdx, {Full, NdVar::scalar(Sz, 4)});
      if (!SuppressFlags) {
        S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(x86reg::CF, 1),
               {Rdx, NdVar::scalar(0, Sz)});
        S.emit(NdOp::COPY, NdVar::reg(x86reg::OF, 1),
               {NdVar::reg(x86reg::CF, 1)});
      }
    }
    break;
  }

  // --- SBB (subtract with borrow): Dst = Dst - Src - CF; CF = combined borrow.
  case X86_INS_SBB: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = readArithmeticOperand(X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar Src = readArithmeticOperand(X86.operands[1]);
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    // Snapshot operands before the result overwrites DstW, so the borrow/OF
    // flags read the pre-write values (DstW aliases operand[0]).  Without this
    // a multi-limb sbb chain consumes a borrow computed from the result.
    NdVar A = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, A, {DstR});
    NdVar B = S.makeTemp(Src.Size);
    S.emit(NdOp::COPY, B, {Src});
    NdVar Result = MemDst ? S.makeTemp(A.Size) : DstW;
    NdVar CfExt = snapshotCarryAtWidth(S, A.Size);
    NdVar CarryInner = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, CarryInner, {B, CfExt});
    NdVar Adj = S.makeTemp(A.Size);
    S.emit(NdOp::INT_ADD, Adj, {B, CfExt});
    S.emit(NdOp::INT_SUB, Result, {A, Adj});
    NdVar BorrowOuter = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, BorrowOuter, {A, Adj});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1), {CarryInner, BorrowOuter});
    L.emitZSPF(S, Result);
    // OF: two-stage sborrow XOR — sborrow(Dst,Src) ^ sborrow(Dst-Src, Cf).
    // Using INT_SBOR(Dst, Adj) alone is WRONG when Src+CF wraps
    // (e.g. Src=0x7F, CF=1 → Adj=0x80 flips the sign of the subtrahend).
    {
      NdVar Temp = S.makeTemp(A.Size);
      S.emit(NdOp::INT_SUB, Temp, {A, B});
      NdVar SB1 = S.makeTemp(1);
      S.emit(NdOp::INT_SBOR, SB1, {A, B});
      NdVar SB2 = S.makeTemp(1);
      S.emit(NdOp::INT_SBOR, SB2, {Temp, CfExt});
      S.emit(NdOp::BOOL_XOR, NdVar::reg(x86reg::OF, 1), {SB1, SB2});
    }
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  // --- ADC (add with carry): mirror of SBB.
  case X86_INS_ADC: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = readArithmeticOperand(X86.operands[0]);
    NdVar DstW = L.operandWrite(X86.operands[0]);
    NdVar Src = readArithmeticOperand(X86.operands[1]);
    bool MemDst = (X86.operands[0].type == X86_OP_MEM);
    // Snapshot operands before the result overwrites DstW, so the carry/OF
    // flags read the pre-write values (DstW aliases operand[0]).  Without this
    // a multi-limb adc chain consumes a carry computed from the result.
    NdVar A = S.makeTemp(DstR.Size);
    S.emit(NdOp::COPY, A, {DstR});
    NdVar B = S.makeTemp(Src.Size);
    S.emit(NdOp::COPY, B, {Src});
    NdVar Result = MemDst ? S.makeTemp(A.Size) : DstW;
    NdVar CfExt = snapshotCarryAtWidth(S, A.Size);
    NdVar CarryInner = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, CarryInner, {B, CfExt});
    NdVar Adj = S.makeTemp(A.Size);
    S.emit(NdOp::INT_ADD, Adj, {B, CfExt});
    S.emit(NdOp::INT_ADD, Result, {A, Adj});
    L.emitZSPF(S, Result);
    NdVar CarryOuter = S.makeTemp(1);
    S.emit(NdOp::INT_CARRY, CarryOuter, {A, Adj});
    S.emit(NdOp::BOOL_OR, NdVar::reg(x86reg::CF, 1), {CarryInner, CarryOuter});
    NdVar V1 = S.makeTemp(1);
    S.emit(NdOp::INT_SOVF, V1, {B, CfExt});
    NdVar V2 = S.makeTemp(1);
    S.emit(NdOp::INT_SOVF, V2, {A, Adj});
    S.emit(NdOp::BOOL_XOR, NdVar::reg(x86reg::OF, 1), {V1, V2});
    if (MemDst)
      S.storeToMem(X86.operands[0], Result);
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
