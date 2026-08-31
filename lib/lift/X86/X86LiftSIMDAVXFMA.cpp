//===- X86LiftSIMDAVXFMA.cpp - x86/x64 FMA instruction lifter -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fused multiply-add: the FMA3 132/213/231 operand
/// orderings, the FMA4 legacy forms, the negated-product and
/// subtract-addend variants, and the alternating
/// add-subtract forms.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include <algorithm>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

// Build a single-rounding x86 FMA result: the fused product Fa*Fb plus Fc with
// ONE rounding (NeverD FLOAT_FMA -> llvm.fma), matching the AArch64/ARM
// lowering.  Modeling FMA as a separate FLOAT_MULT + FLOAT_ADD rounds twice and
// diverges from hardware in the low mantissa bit.  NegProd negates the product
// (FNMADD/FNMSUB); SubAdd subtracts the addend (FMSUB/FNMSUB).  Scalar SS/SD
// forms compute only the low ElemSz element and copy the upper XMM lanes from
// ScalarUpper; packed PS/PD forms fuse every ElemSz-wide lane. Fa/Fb/Fc are the
// full-width operands.
static NdVar buildX86FmaResult(LiftStateBase &S, uint16_t ResultSize, NdVar Fa,
                               NdVar Fb, NdVar Fc, bool NegProd, bool SubAdd,
                               bool Scalar, unsigned ElemSz,
                               NdVar ScalarUpper) {
  auto laneFma = [&](NdVar A, NdVar B, NdVar C) {
    NdVar AA = A;
    if (NegProd) {
      AA = S.makeTemp(ElemSz);
      S.emit(NdOp::FLOAT_NEG, AA, {A});
    }
    NdVar CC = C;
    if (SubAdd) {
      CC = S.makeTemp(ElemSz);
      S.emit(NdOp::FLOAT_NEG, CC, {C});
    }
    NdVar R = S.makeTemp(ElemSz);
    S.emit(NdOp::FLOAT_FMA, R, {AA, B, CC});
    return R;
  };
  auto lane = [&](NdVar V, unsigned Off) {
    if (V.Size == ElemSz && Off == 0)
      return V;
    NdVar L = S.makeTemp(ElemSz);
    S.emit(NdOp::SUBBYTES, L, {V, NdVar::cst(Off, 4)});
    return L;
  };

  if (Scalar || ResultSize <= ElemSz) {
    NdVar R = laneFma(lane(Fa, 0), lane(Fb, 0), lane(Fc, 0));
    if (ResultSize > ElemSz) {
      NdVar Hi = S.makeTemp(ResultSize - ElemSz);
      S.emit(NdOp::SUBBYTES, Hi, {ScalarUpper, NdVar::cst(ElemSz, 4)});
      NdVar Result = S.makeTemp(ResultSize);
      S.emit(NdOp::CONCAT, Result, {Hi, R});
      return Result;
    }
    return R;
  }

  unsigned NLanes = ResultSize / ElemSz;
  NdVar Acc = laneFma(lane(Fa, 0), lane(Fb, 0), lane(Fc, 0));
  unsigned AccSz = ElemSz;
  for (unsigned I = 1; I < NLanes; ++I) {
    NdVar Ln = laneFma(lane(Fa, I * ElemSz), lane(Fb, I * ElemSz),
                       lane(Fc, I * ElemSz));
    NdVar Next = S.makeTemp(AccSz + ElemSz);
    S.emit(NdOp::CONCAT, Next, {Ln, Acc});
    Acc = Next;
    AccSz += ElemSz;
  }
  return Acc;
}

static NdVar buildX86AlternatingFmaResult(LiftStateBase &S,
                                          uint16_t ResultSize, NdVar Fa,
                                          NdVar Fb, NdVar Fc,
                                          bool SubtractEven,
                                          unsigned ElementSize) {
  auto Lane = [&](NdVar Value, unsigned Offset) {
    NdVar Result = S.makeTemp(ElementSize);
    S.emit(NdOp::SUBBYTES, Result, {Value, NdVar::cst(Offset, 4)});
    return Result;
  };

  NdVar Packed;
  for (unsigned Index = 0; Index < ResultSize / ElementSize; ++Index) {
    NdVar Addend = Lane(Fc, Index * ElementSize);
    if (((Index & 1) == 0) == SubtractEven) {
      NdVar Negated = S.makeTemp(ElementSize);
      S.emit(NdOp::FLOAT_NEG, Negated, {Addend});
      Addend = Negated;
    }
    NdVar ResultLane = S.makeTemp(ElementSize);
    S.emit(NdOp::FLOAT_FMA, ResultLane,
           {Lane(Fa, Index * ElementSize),
            Lane(Fb, Index * ElementSize), Addend});
    if (Index == 0) {
      Packed = ResultLane;
    } else {
      NdVar Next = S.makeTemp(Packed.Size + ElementSize);
      S.emit(NdOp::CONCAT, Next, {ResultLane, Packed});
      Packed = Next;
    }
  }
  return Packed;
}

static NdVar buildExactX86FmaResult(
    LiftStateBase &S, uint16_t ResultSize, NdVar A, NdVar B, NdVar C,
    NdVar ActiveMask, bool NegateProduct, bool SubtractAddend, bool Scalar,
    uint16_t ElementSize, NdVar ScalarUpper, X86FPRounding Rounding,
    bool SuppressExceptions, bool AlternatingAddend = false,
    bool SubtractEven = false) {
  if (ResultSize == 0 || A.Size != ResultSize || B.Size != ResultSize ||
      C.Size != ResultSize || ActiveMask.Size == 0)
    return {};
  const uint16_t Control = makeX86FPArithControl(
      X86FPArithKind::FusedMultiplyAdd, ElementSize == 8, Scalar,
      SuppressExceptions, Rounding, NegateProduct, SubtractAddend,
      AlternatingAddend, SubtractEven);
  NdVar Raw = S.makeTemp(ResultSize);
  S.emitIntrinsic(Intrinsic::X86FPArith, Raw,
                  {NdVar::cst(Control, 2), A, B, C, ActiveMask});
  if (!Scalar)
    return Raw;
  if (ResultSize <= ElementSize || ScalarUpper.Size != ResultSize)
    return {};
  NdVar Low = S.makeTemp(ElementSize);
  S.emit(NdOp::SUBBYTES, Low, {Raw, NdVar::cst(0, 4)});
  NdVar High = S.makeTemp(ResultSize - ElementSize);
  S.emit(NdOp::SUBBYTES, High,
         {ScalarUpper, NdVar::cst(ElementSize, 4)});
  NdVar Result = S.makeTemp(ResultSize);
  S.emit(NdOp::CONCAT, Result, {High, Low});
  return Result;
}

static bool isVectorRegisterOfSize(const cs_x86_op &Operand, uint16_t Size) {
  if (Operand.type != X86_OP_REG || Operand.size != Size)
    return false;
  if (Size == 16)
    return Operand.reg >= X86_REG_XMM0 && Operand.reg <= X86_REG_XMM31;
  if (Size == 32)
    return Operand.reg >= X86_REG_YMM0 && Operand.reg <= X86_REG_YMM31;
  if (Size == 64)
    return Operand.reg >= X86_REG_ZMM0 && Operand.reg <= X86_REG_ZMM31;
  return false;
}

static unsigned vectorRegisterIndex(const cs_x86_op &Operand) {
  if (Operand.size == 16)
    return static_cast<unsigned>(Operand.reg - X86_REG_XMM0);
  if (Operand.size == 32)
    return static_cast<unsigned>(Operand.reg - X86_REG_YMM0);
  return static_cast<unsigned>(Operand.reg - X86_REG_ZMM0);
}

static x86_avx_bcast broadcastForLaneCount(unsigned LaneCount) {
  switch (LaneCount) {
  case 2:
    return X86_AVX_BCAST_2;
  case 4:
    return X86_AVX_BCAST_4;
  case 8:
    return X86_AVX_BCAST_8;
  case 16:
    return X86_AVX_BCAST_16;
  default:
    return X86_AVX_BCAST_INVALID;
  }
}

static bool validateCanonicalEvexFma3(
    const cs_insn *Insn, const cs_x86 &X86, Arch TargetArch,
    const cs_x86_op &Destination, const cs_x86_op &FirstSource,
    const cs_x86_op &SecondSource, bool HasWriteMask, bool Scalar,
    uint16_t ElementSize, uint8_t ExpectedOpcode, bool &MemoryForm,
    bool &Broadcast, X86FPRounding &Rounding, bool &SuppressExceptions) {
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, TargetArch, Encoding) ||
      (Encoding.P0 & 0x07) != 0x02 ||
      ((Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((ElementSize == 8 ? 0x80 : 0) | 0x05) ||
      Encoding.Opcode != ExpectedOpcode || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0)
    return false;

  const uint16_t VectorSize = static_cast<uint16_t>(Destination.size);
  if (!isVectorRegisterOfSize(Destination, VectorSize) ||
      !isVectorRegisterOfSize(FirstSource, VectorSize) ||
      (Scalar ? VectorSize != 16
              : (VectorSize != 16 && VectorSize != 32 && VectorSize != 64)))
    return false;

  MemoryForm = (Encoding.ModRM & 0xc0) != 0xc0;
  const bool EncodedB = (Encoding.P2 & 0x10) != 0;
  const bool EmbeddedRounding = !MemoryForm && EncodedB;
  Broadcast = MemoryForm && EncodedB;
  if (MemoryForm != (SecondSource.type == X86_OP_MEM) ||
      (Scalar && Broadcast))
    return false;
  if (!MemoryForm && !isVectorRegisterOfSize(SecondSource, VectorSize))
    return false;

  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      VectorSize == 16 ? 0 : (VectorSize == 32 ? 0x20 : 0x40);
  if ((Scalar && !EmbeddedRounding && EncodedLength != 0) ||
      (!Scalar && !EmbeddedRounding && EncodedLength != ExpectedLength) ||
      (!Scalar && EmbeddedRounding && VectorSize != 64))
    return false;

  SuppressExceptions = EmbeddedRounding;
  Rounding = EmbeddedRounding
                 ? static_cast<X86FPRounding>(EncodedLength >> 5)
                 : X86FPRounding::MXCSR;
  const x86_avx_rm ExpectedRounding =
      EmbeddedRounding
          ? static_cast<x86_avx_rm>(X86_AVX_RM_RN + (EncodedLength >> 5))
          : X86_AVX_RM_INVALID;
  if (X86.avx_sae != EmbeddedRounding || X86.avx_rm != ExpectedRounding)
    return false;

  const unsigned LaneCount = Scalar ? 1 : VectorSize / ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  const uint8_t EncodedMask = Encoding.P2 & 7;
  if (HasWriteMask) {
    const cs_x86_op &Mask = X86.operands[1];
    const RegInfo MaskInfo = mapCapstoneReg(static_cast<x86_reg>(Mask.reg));
    if (!isX86OpmaskOperand(Mask) || Mask.reg == X86_REG_K0 ||
        Mask.size != MaskSize || MaskInfo.Offset == UINT64_C(0xffff) ||
        MaskInfo.Size < MaskSize ||
        EncodedMask != Mask.reg - X86_REG_K0 ||
        Mask.avx_zero_opmask != ((Encoding.P2 & 0x80) != 0))
      return false;
  } else if (EncodedMask != 0 || (Encoding.P2 & 0x80) != 0) {
    return false;
  }

  if (decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
          vectorRegisterIndex(FirstSource))
    return false;

  const uint16_t MemoryTupleSize =
      Scalar || Broadcast ? ElementSize : VectorSize;
  if (MemoryForm) {
    if (SecondSource.size != MemoryTupleSize ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, SecondSource,
                                         MemoryTupleSize))
      return false;
  } else if (decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 vectorRegisterIndex(SecondSource) ||
             !validateCanonicalEvexRegisterTail(Insn, X86, Encoding)) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      Broadcast ? broadcastForLaneCount(VectorSize / ElementSize)
                : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    const bool IsMask = HasWriteMask && Index == 1;
    const bool IsSecondSource = &Operand == &SecondSource;
    if (Operand.avx_zero_opmask !=
            (IsMask && (Encoding.P2 & 0x80) != 0) ||
        Operand.avx_bcast !=
            (IsSecondSource ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }
  return true;
}

static bool isVexVectorRegisterOfSize(const cs_x86_op &Operand,
                                      uint16_t Size) {
  return isVectorRegisterOfSize(Operand, Size) &&
         vectorRegisterIndex(Operand) < 16;
}

static bool validateCanonicalVexFma4(
    const cs_insn *Insn, const cs_x86 &X86, Arch TargetArch, bool Scalar,
    uint16_t ElementSize, uint8_t ExpectedOpcode) {
  CanonicalVex3EncodingInfo Encoding;
  if (!parseCanonicalVex3EncodingInfo(Insn, X86, TargetArch, Encoding) ||
      (Encoding.P0 & 0x1f) != 0x03 || (Encoding.P1 & 0x03) != 0x01 ||
      Encoding.Opcode != ExpectedOpcode || X86.op_count != 4 ||
      X86.encoding.imm_size != 1 ||
      X86.encoding.imm_offset + 1 != Insn->size ||
      X86.xop_cc != X86_XOP_CC_INVALID ||
      X86.sse_cc != X86_SSE_CC_INVALID ||
      X86.avx_cc != X86_AVX_CC_INVALID || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID || X86.eflags != 0)
    return false;

  const uint16_t VectorSize = X86.operands[0].size;
  if (!isVexVectorRegisterOfSize(X86.operands[0], VectorSize) ||
      !isVexVectorRegisterOfSize(X86.operands[1], VectorSize) ||
      (Scalar ? VectorSize != 16
              : (VectorSize != 16 && VectorSize != 32)) ||
      ((Encoding.P1 & 0x04) != 0) != (VectorSize == 32))
    return false;

  const unsigned RegisterLimit = TargetArch == Arch::X64 ? 16 : 8;
  const unsigned DestinationIndex =
      ((Encoding.P0 & 0x80) == 0 ? 8 : 0) | ((Encoding.ModRM >> 3) & 7);
  const unsigned FirstSourceIndex =
      (static_cast<uint8_t>(~Encoding.P1) >> 3) & 0x0f;
  const unsigned RmRegisterIndex =
      ((Encoding.P0 & 0x20) == 0 ? 8 : 0) | (Encoding.ModRM & 7);
  const uint8_t Is4 = Insn->bytes[X86.encoding.imm_offset];
  const unsigned Is4RegisterIndex = Is4 >> 4;
  if (DestinationIndex >= RegisterLimit ||
      FirstSourceIndex >= RegisterLimit || Is4RegisterIndex >= RegisterLimit ||
      DestinationIndex != vectorRegisterIndex(X86.operands[0]) ||
      FirstSourceIndex != vectorRegisterIndex(X86.operands[1]))
    return false;

  const bool W = (Encoding.P1 & 0x80) != 0;
  const unsigned RmSourceOperandIndex = W ? 3 : 2;
  const unsigned Is4SourceOperandIndex = W ? 2 : 3;
  const bool MemoryForm = (Encoding.ModRM & 0xc0) != 0xc0;
  const cs_x86_op &RmSource = X86.operands[RmSourceOperandIndex];
  const cs_x86_op &Is4Source = X86.operands[Is4SourceOperandIndex];
  if (!isVexVectorRegisterOfSize(Is4Source, VectorSize) ||
      vectorRegisterIndex(Is4Source) != Is4RegisterIndex)
    return false;

  if (MemoryForm) {
    const uint16_t MemorySize = Scalar ? ElementSize : VectorSize;
    if (RmSource.type != X86_OP_MEM || RmSource.size != MemorySize ||
        !validateCanonicalVex3MemoryTail(Insn, X86, Encoding, RmSource, 1))
      return false;
  } else if (!isVexVectorRegisterOfSize(RmSource, VectorSize) ||
             RmRegisterIndex >= RegisterLimit ||
             vectorRegisterIndex(RmSource) != RmRegisterIndex ||
             !validateCanonicalVex3RegisterTail(Insn, X86, Encoding, 1)) {
    return false;
  }

  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID ||
        X86.operands[Index].avx_zero_opmask)
      return false;
  return true;
}

bool liftSIMDAVXFMA(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86) {
  const bool IsEvex = Insn && X86.opcode[0] == 0x62;

  enum class FmaOrder { Order132, Order213, Order231 };
  const auto EmitFma3 = [&](FmaOrder Order, bool NegProd, bool SubAdd,
                            bool Scalar, unsigned ElemSz) {
    const bool HasWriteMask =
        X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
    const unsigned ExpectedOperands = HasWriteMask ? 4 : 3;
    const unsigned Source1Index = HasWriteMask ? 2 : 1;
    const unsigned Source2Index = HasWriteMask ? 3 : 2;
    if (X86.op_count != ExpectedOperands ||
        X86.operands[0].type != X86_OP_REG ||
        X86.operands[Source1Index].type != X86_OP_REG ||
        (X86.operands[Source2Index].type != X86_OP_REG &&
         X86.operands[Source2Index].type != X86_OP_MEM))
      return false;

    bool MemoryForm = false;
    bool Broadcast = false;
    X86FPRounding Rounding = X86FPRounding::MXCSR;
    bool SuppressExceptions = false;
    if (IsEvex) {
      uint8_t ExpectedOpcode = Order == FmaOrder::Order132 ? 0x98
                               : Order == FmaOrder::Order213 ? 0xa8
                                                             : 0xb8;
      ExpectedOpcode = static_cast<uint8_t>(
          ExpectedOpcode + (NegProd ? 4 : 0) + (SubAdd ? 2 : 0) +
          (Scalar ? 1 : 0));
      if (!validateCanonicalEvexFma3(
              Insn, X86, L.targetArch(), X86.operands[0],
              X86.operands[Source1Index], X86.operands[Source2Index],
              HasWriteMask, Scalar, ElemSz, ExpectedOpcode, MemoryForm,
              Broadcast, Rounding, SuppressExceptions))
        return false;
    }

    NdVar Dst = L.operandWrite(X86.operands[0]);
    RegInfo MaskInfo{0, 0};
    const unsigned LaneCount = Scalar ? 1 : Dst.Size / ElemSz;
    const uint16_t RequiredMaskSize =
        static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
    const uint64_t RelevantMask =
        LaneCount == 64 ? UINT64_MAX
                        : (UINT64_C(1) << LaneCount) - UINT64_C(1);
    NdVar ActiveMask = NdVar::cst(RelevantMask, RequiredMaskSize);
    if (HasWriteMask) {
      MaskInfo = mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
      if (MaskInfo.Offset == UINT64_C(0xffff) ||
          MaskInfo.Size < RequiredMaskSize)
        return false;
      ActiveMask = NdVar::reg(MaskInfo.Offset, RequiredMaskSize);
    }

    NdVar OldDst = L.operandRead(S, X86.operands[0]);
    NdVar Source1 = L.operandRead(S, X86.operands[Source1Index]);
    NdVar Source2;
    if (MemoryForm) {
      NdVar LoadMask = ActiveMask;
      if (Scalar) {
        if (HasWriteMask) {
          LoadMask = S.makeTemp(ActiveMask.Size);
          S.emit(NdOp::INT_AND, LoadMask,
                 {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
        } else {
          LoadMask = NdVar::cst(1, 8);
        }
      }
      const uint16_t MemoryTupleSize =
          Scalar || Broadcast ? ElemSz : Dst.Size;
      Source2 = emitEvexMaskedMemoryLoad(
          S, X86.operands[Source2Index], LoadMask, Dst.Size, ElemSz,
          MemoryTupleSize, Broadcast);
    } else {
      Source2 = L.operandRead(S, X86.operands[Source2Index]);
    }
    if (Dst.Size < ElemSz || Dst.Size % ElemSz != 0 ||
        OldDst.Size != Dst.Size || Source1.Size != Dst.Size ||
        Source2.Size != Dst.Size)
      return false;

    NdVar Fa;
    NdVar Fb;
    NdVar Fc;
    switch (Order) {
    case FmaOrder::Order132:
      Fa = OldDst;
      Fb = Source2;
      Fc = Source1;
      break;
    case FmaOrder::Order213:
      Fa = Source1;
      Fb = OldDst;
      Fc = Source2;
      break;
    case FmaOrder::Order231:
      Fa = Source1;
      Fb = Source2;
      Fc = OldDst;
      break;
    }

    NdVar Raw = IsEvex
                    ? buildExactX86FmaResult(
                          S, Dst.Size, Fa, Fb, Fc, ActiveMask, NegProd, SubAdd,
                          Scalar, ElemSz, Source1, Rounding,
                          SuppressExceptions)
                    : buildX86FmaResult(S, Dst.Size, Fa, Fb, Fc, NegProd,
                                        SubAdd, Scalar, ElemSz, Source1);
    if (Raw.Size != Dst.Size)
      return false;
    if (!HasWriteMask) {
      S.emit(NdOp::COPY, Dst, {Raw});
      return true;
    }
    if (!Scalar)
      return emitMaskedVectorResult(L, S, X86.operands[0], X86.operands[1], Raw,
                                    ElemSz);

    NdVar MaskBit = S.makeTemp(1);
    S.emit(NdOp::INT_AND, MaskBit,
           {NdVar::reg(MaskInfo.Offset, 1), NdVar::cst(1, 1)});
    NdVar NewLow = S.makeTemp(ElemSz);
    S.emit(NdOp::SUBBYTES, NewLow, {Raw, NdVar::cst(0, 4)});
    NdVar Inactive = NdVar::cst(0, ElemSz);
    if (!X86.operands[1].avx_zero_opmask) {
      Inactive = S.makeTemp(ElemSz);
      S.emit(NdOp::SUBBYTES, Inactive, {OldDst, NdVar::cst(0, 4)});
    }
    NdVar Low = S.makeTemp(ElemSz);
    S.emit(NdOp::SELECT, Low, {MaskBit, NewLow, Inactive});
    if (Dst.Size == ElemSz) {
      S.emit(NdOp::COPY, Dst, {Low});
      return true;
    }
    NdVar High = S.makeTemp(Dst.Size - ElemSz);
    S.emit(NdOp::SUBBYTES, High, {Raw, NdVar::cst(ElemSz, 4)});
    NdVar Result = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Result, {High, Low});
    S.emit(NdOp::COPY, Dst, {Result});
    return true;
  };

  const auto EmitAlternatingFma3 = [&](FmaOrder Order, bool SubtractEven,
                                       unsigned ElementSize) {
    const bool HasWriteMask =
        X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
    const unsigned Source1Index = HasWriteMask ? 2 : 1;
    const unsigned Source2Index = HasWriteMask ? 3 : 2;
    if (X86.op_count != (HasWriteMask ? 4u : 3u) ||
        X86.operands[0].type != X86_OP_REG ||
        X86.operands[Source1Index].type != X86_OP_REG ||
        (X86.operands[Source2Index].type != X86_OP_REG &&
         X86.operands[Source2Index].type != X86_OP_MEM))
      return false;

    bool MemoryForm = false;
    bool Broadcast = false;
    X86FPRounding Rounding = X86FPRounding::MXCSR;
    bool SuppressExceptions = false;
    if (IsEvex) {
      uint8_t ExpectedOpcode = Order == FmaOrder::Order132 ? 0x90
                               : Order == FmaOrder::Order213 ? 0xa0
                                                             : 0xb0;
      ExpectedOpcode = static_cast<uint8_t>(
          ExpectedOpcode + (SubtractEven ? 0x06 : 0x07));
      if (!validateCanonicalEvexFma3(
              Insn, X86, L.targetArch(), X86.operands[0],
              X86.operands[Source1Index], X86.operands[Source2Index],
              HasWriteMask, false, ElementSize, ExpectedOpcode, MemoryForm,
              Broadcast, Rounding, SuppressExceptions))
        return false;
    }

    const NdVar Destination = L.operandWrite(X86.operands[0]);
    const unsigned LaneCount = Destination.Size / ElementSize;
    const uint16_t RequiredMaskSize =
        static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
    const uint64_t RelevantMask =
        LaneCount == 64 ? UINT64_MAX
                        : (UINT64_C(1) << LaneCount) - UINT64_C(1);
    NdVar ActiveMask = NdVar::cst(RelevantMask, RequiredMaskSize);
    if (HasWriteMask) {
      const RegInfo MaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
      if (MaskInfo.Offset == UINT64_C(0xffff) ||
          MaskInfo.Size < RequiredMaskSize)
        return false;
      ActiveMask = NdVar::reg(MaskInfo.Offset, RequiredMaskSize);
    }

    const NdVar OldDestination = L.operandRead(S, X86.operands[0]);
    const NdVar FirstSource = L.operandRead(S, X86.operands[Source1Index]);
    NdVar SecondSource;
    if (MemoryForm) {
      const uint16_t MemoryTupleSize = Broadcast ? ElementSize
                                                 : Destination.Size;
      SecondSource = emitEvexMaskedMemoryLoad(
          S, X86.operands[Source2Index], ActiveMask, Destination.Size,
          ElementSize, MemoryTupleSize, Broadcast);
    } else {
      SecondSource = L.operandRead(S, X86.operands[Source2Index]);
    }
    if (Destination.Size == 0 || Destination.Size % ElementSize != 0 ||
        OldDestination.Size != Destination.Size ||
        FirstSource.Size != Destination.Size ||
        SecondSource.Size != Destination.Size)
      return false;

    NdVar A;
    NdVar B;
    NdVar C;
    switch (Order) {
    case FmaOrder::Order132:
      A = OldDestination;
      B = SecondSource;
      C = FirstSource;
      break;
    case FmaOrder::Order213:
      A = FirstSource;
      B = OldDestination;
      C = SecondSource;
      break;
    case FmaOrder::Order231:
      A = FirstSource;
      B = SecondSource;
      C = OldDestination;
      break;
    }
    const NdVar Raw =
        IsEvex
            ? buildExactX86FmaResult(
                  S, Destination.Size, A, B, C, ActiveMask, false, false,
                  false, ElementSize, FirstSource, Rounding,
                  SuppressExceptions, true, SubtractEven)
            : buildX86AlternatingFmaResult(
                  S, Destination.Size, A, B, C, SubtractEven, ElementSize);
    if (Raw.Size != Destination.Size)
      return false;
    if (HasWriteMask)
      return emitMaskedVectorResult(L, S, X86.operands[0], X86.operands[1],
                                    Raw, ElementSize);
    S.emit(NdOp::COPY, Destination, {Raw});
    return true;
  };

  const auto EmitFma4 = [&](bool NegateProduct, bool SubtractAddend,
                            bool Scalar, unsigned ElementSize,
                            uint8_t ExpectedOpcode) {
    if (!validateCanonicalVexFma4(Insn, X86, L.targetArch(), Scalar,
                                  ElementSize, ExpectedOpcode))
      return false;

    const NdVar Destination = L.operandWrite(X86.operands[0]);
    const NdVar FirstSource = L.operandRead(S, X86.operands[1]);
    const NdVar SecondSource = L.operandRead(S, X86.operands[2]);
    const NdVar ThirdSource = L.operandRead(S, X86.operands[3]);
    const auto IsValidSourceSize = [&](NdVar Source) {
      return Source.Size == Destination.Size ||
             (Scalar && Source.Size == ElementSize);
    };
    if (Destination.Size < ElementSize ||
        Destination.Size % ElementSize != 0 ||
        FirstSource.Size != Destination.Size ||
        !IsValidSourceSize(SecondSource) ||
        !IsValidSourceSize(ThirdSource))
      return false;

    const NdVar Raw = buildX86FmaResult(
        S, Destination.Size, FirstSource, SecondSource, ThirdSource,
        NegateProduct, SubtractAddend, Scalar, ElementSize, FirstSource);
    S.emit(NdOp::COPY, Destination, {Raw});
    return true;
  };

  const auto EmitAlternatingFma4 = [&](bool SubtractEven,
                                       unsigned ElementSize,
                                       uint8_t ExpectedOpcode) {
    if (!validateCanonicalVexFma4(Insn, X86, L.targetArch(), false,
                                  ElementSize, ExpectedOpcode))
      return false;

    const NdVar Destination = L.operandWrite(X86.operands[0]);
    const NdVar FirstSource = L.operandRead(S, X86.operands[1]);
    const NdVar SecondSource = L.operandRead(S, X86.operands[2]);
    const NdVar ThirdSource = L.operandRead(S, X86.operands[3]);
    if (Destination.Size == 0 || Destination.Size % ElementSize != 0 ||
        FirstSource.Size != Destination.Size ||
        SecondSource.Size != Destination.Size ||
        ThirdSource.Size != Destination.Size)
      return false;

    const NdVar Raw = buildX86AlternatingFmaResult(
        S, Destination.Size, FirstSource, SecondSource, ThirdSource,
        SubtractEven, ElementSize);
    S.emit(NdOp::COPY, Destination, {Raw});
    return true;
  };

  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // FMA (Fused Multiply-Add) — Dst = a * b ± c with correct operand mapping.
  // 132: Dst = Dst * src3 + src2     (op0 = op0 * op2 + op1)
  // 213: Dst = src2 * Dst + src3     (op0 = op1 * op0 + op2)
  // 231: Dst = src2 * src3 + Dst     (op0 = op1 * op2 + op0)
  // FNMADD: negate first multiplicand; FMSUB/FNMSUB: subtract addend.
  // ========================================================================
  case X86_INS_VFMADD132PD:
  case X86_INS_VFMADD132PS:
  case X86_INS_VFMADD132SD:
  case X86_INS_VFMADD132SS: {
    bool Sc = InsnId == X86_INS_VFMADD132SS || InsnId == X86_INS_VFMADD132SD;
    unsigned E =
        (InsnId == X86_INS_VFMADD132SD || InsnId == X86_INS_VFMADD132PD) ? 8
                                                                         : 4;
    return EmitFma3(FmaOrder::Order132, false, false, Sc, E);
  }
  case X86_INS_VFMADD213PD:
  case X86_INS_VFMADD213PS:
  case X86_INS_VFMADD213SD:
  case X86_INS_VFMADD213SS: {
    bool Sc = InsnId == X86_INS_VFMADD213SS || InsnId == X86_INS_VFMADD213SD;
    unsigned E =
        (InsnId == X86_INS_VFMADD213SD || InsnId == X86_INS_VFMADD213PD) ? 8
                                                                         : 4;
    return EmitFma3(FmaOrder::Order213, false, false, Sc, E);
  }
  case X86_INS_VFMADD231PD:
  case X86_INS_VFMADD231PS:
  case X86_INS_VFMADD231SD:
  case X86_INS_VFMADD231SS: {
    bool Sc = InsnId == X86_INS_VFMADD231SS || InsnId == X86_INS_VFMADD231SD;
    unsigned E =
        (InsnId == X86_INS_VFMADD231SD || InsnId == X86_INS_VFMADD231PD) ? 8
                                                                         : 4;
    return EmitFma3(FmaOrder::Order231, false, false, Sc, E);
  }
  case X86_INS_VFMADDPD:
  case X86_INS_VFMADDPS:
  case X86_INS_VFMADDSD:
  case X86_INS_VFMADDSS: {
    const bool Scalar =
        InsnId == X86_INS_VFMADDSS || InsnId == X86_INS_VFMADDSD;
    const bool Double =
        InsnId == X86_INS_VFMADDPD || InsnId == X86_INS_VFMADDSD;
    return EmitFma4(false, false, Scalar, Double ? 8 : 4,
                    static_cast<uint8_t>(0x68 + (Double ? 1 : 0) +
                                         (Scalar ? 2 : 0)));
  }
  case X86_INS_VFMSUB132PD:
  case X86_INS_VFMSUB132PS:
  case X86_INS_VFMSUB132SD:
  case X86_INS_VFMSUB132SS: {
    bool Sc = InsnId == X86_INS_VFMSUB132SS || InsnId == X86_INS_VFMSUB132SD;
    unsigned E =
        (InsnId == X86_INS_VFMSUB132SD || InsnId == X86_INS_VFMSUB132PD) ? 8
                                                                         : 4;
    return EmitFma3(FmaOrder::Order132, false, true, Sc, E);
  }
  case X86_INS_VFMSUB213PD:
  case X86_INS_VFMSUB213PS:
  case X86_INS_VFMSUB213SD:
  case X86_INS_VFMSUB213SS: {
    bool Sc = InsnId == X86_INS_VFMSUB213SS || InsnId == X86_INS_VFMSUB213SD;
    unsigned E =
        (InsnId == X86_INS_VFMSUB213SD || InsnId == X86_INS_VFMSUB213PD) ? 8
                                                                         : 4;
    return EmitFma3(FmaOrder::Order213, false, true, Sc, E);
  }
  case X86_INS_VFMSUB231PD:
  case X86_INS_VFMSUB231PS:
  case X86_INS_VFMSUB231SD:
  case X86_INS_VFMSUB231SS: {
    bool Sc = InsnId == X86_INS_VFMSUB231SS || InsnId == X86_INS_VFMSUB231SD;
    unsigned E =
        (InsnId == X86_INS_VFMSUB231SD || InsnId == X86_INS_VFMSUB231PD) ? 8
                                                                         : 4;
    return EmitFma3(FmaOrder::Order231, false, true, Sc, E);
  }
  case X86_INS_VFMSUBPD:
  case X86_INS_VFMSUBPS:
  case X86_INS_VFMSUBSD:
  case X86_INS_VFMSUBSS: {
    const bool Scalar =
        InsnId == X86_INS_VFMSUBSS || InsnId == X86_INS_VFMSUBSD;
    const bool Double =
        InsnId == X86_INS_VFMSUBPD || InsnId == X86_INS_VFMSUBSD;
    return EmitFma4(false, true, Scalar, Double ? 8 : 4,
                    static_cast<uint8_t>(0x6c + (Double ? 1 : 0) +
                                         (Scalar ? 2 : 0)));
  }
  case X86_INS_VFNMADD132PD:
  case X86_INS_VFNMADD132PS:
  case X86_INS_VFNMADD132SD:
  case X86_INS_VFNMADD132SS: {
    bool Sc = InsnId == X86_INS_VFNMADD132SS || InsnId == X86_INS_VFNMADD132SD;
    unsigned E =
        (InsnId == X86_INS_VFNMADD132SD || InsnId == X86_INS_VFNMADD132PD) ? 8
                                                                           : 4;
    return EmitFma3(FmaOrder::Order132, true, false, Sc, E);
  }
  case X86_INS_VFNMADD213PD:
  case X86_INS_VFNMADD213PS:
  case X86_INS_VFNMADD213SD:
  case X86_INS_VFNMADD213SS: {
    bool Sc = InsnId == X86_INS_VFNMADD213SS || InsnId == X86_INS_VFNMADD213SD;
    unsigned E =
        (InsnId == X86_INS_VFNMADD213SD || InsnId == X86_INS_VFNMADD213PD) ? 8
                                                                           : 4;
    return EmitFma3(FmaOrder::Order213, true, false, Sc, E);
  }
  case X86_INS_VFNMADD231PD:
  case X86_INS_VFNMADD231PS:
  case X86_INS_VFNMADD231SD:
  case X86_INS_VFNMADD231SS: {
    bool Sc = InsnId == X86_INS_VFNMADD231SS || InsnId == X86_INS_VFNMADD231SD;
    unsigned E =
        (InsnId == X86_INS_VFNMADD231SD || InsnId == X86_INS_VFNMADD231PD) ? 8
                                                                           : 4;
    return EmitFma3(FmaOrder::Order231, true, false, Sc, E);
  }
  case X86_INS_VFNMADDPD:
  case X86_INS_VFNMADDPS:
  case X86_INS_VFNMADDSD:
  case X86_INS_VFNMADDSS: {
    const bool Scalar =
        InsnId == X86_INS_VFNMADDSS || InsnId == X86_INS_VFNMADDSD;
    const bool Double =
        InsnId == X86_INS_VFNMADDPD || InsnId == X86_INS_VFNMADDSD;
    return EmitFma4(true, false, Scalar, Double ? 8 : 4,
                    static_cast<uint8_t>(0x78 + (Double ? 1 : 0) +
                                         (Scalar ? 2 : 0)));
  }
  case X86_INS_VFNMSUB132PD:
  case X86_INS_VFNMSUB132PS:
  case X86_INS_VFNMSUB132SD:
  case X86_INS_VFNMSUB132SS: {
    bool Sc = InsnId == X86_INS_VFNMSUB132SS || InsnId == X86_INS_VFNMSUB132SD;
    unsigned E =
        (InsnId == X86_INS_VFNMSUB132SD || InsnId == X86_INS_VFNMSUB132PD) ? 8
                                                                           : 4;
    return EmitFma3(FmaOrder::Order132, true, true, Sc, E);
  }
  case X86_INS_VFNMSUB213PD:
  case X86_INS_VFNMSUB213PS:
  case X86_INS_VFNMSUB213SD:
  case X86_INS_VFNMSUB213SS: {
    bool Sc = InsnId == X86_INS_VFNMSUB213SS || InsnId == X86_INS_VFNMSUB213SD;
    unsigned E =
        (InsnId == X86_INS_VFNMSUB213SD || InsnId == X86_INS_VFNMSUB213PD) ? 8
                                                                           : 4;
    return EmitFma3(FmaOrder::Order213, true, true, Sc, E);
  }
  case X86_INS_VFNMSUB231PD:
  case X86_INS_VFNMSUB231PS:
  case X86_INS_VFNMSUB231SD:
  case X86_INS_VFNMSUB231SS: {
    bool Sc = InsnId == X86_INS_VFNMSUB231SS || InsnId == X86_INS_VFNMSUB231SD;
    unsigned E =
        (InsnId == X86_INS_VFNMSUB231SD || InsnId == X86_INS_VFNMSUB231PD) ? 8
                                                                           : 4;
    return EmitFma3(FmaOrder::Order231, true, true, Sc, E);
  }
  case X86_INS_VFNMSUBPD:
  case X86_INS_VFNMSUBPS:
  case X86_INS_VFNMSUBSD:
  case X86_INS_VFNMSUBSS: {
    const bool Scalar =
        InsnId == X86_INS_VFNMSUBSS || InsnId == X86_INS_VFNMSUBSD;
    const bool Double =
        InsnId == X86_INS_VFNMSUBPD || InsnId == X86_INS_VFNMSUBSD;
    return EmitFma4(true, true, Scalar, Double ? 8 : 4,
                    static_cast<uint8_t>(0x7c + (Double ? 1 : 0) +
                                         (Scalar ? 2 : 0)));
  }
  // FMADDSUB/FMSUBADD: fused alternating add/subtract per lane.
  case X86_INS_VFMADDSUB132PD:
  case X86_INS_VFMADDSUB132PS:
    return EmitAlternatingFma3(
        FmaOrder::Order132, true,
        InsnId == X86_INS_VFMADDSUB132PD ? 8 : 4);
  case X86_INS_VFMADDSUB213PD:
  case X86_INS_VFMADDSUB213PS:
    return EmitAlternatingFma3(
        FmaOrder::Order213, true,
        InsnId == X86_INS_VFMADDSUB213PD ? 8 : 4);
  case X86_INS_VFMADDSUB231PD:
  case X86_INS_VFMADDSUB231PS:
    return EmitAlternatingFma3(
        FmaOrder::Order231, true,
        InsnId == X86_INS_VFMADDSUB231PD ? 8 : 4);
  case X86_INS_VFMSUBADD132PD:
  case X86_INS_VFMSUBADD132PS:
    return EmitAlternatingFma3(
        FmaOrder::Order132, false,
        InsnId == X86_INS_VFMSUBADD132PD ? 8 : 4);
  case X86_INS_VFMSUBADD213PD:
  case X86_INS_VFMSUBADD213PS:
    return EmitAlternatingFma3(
        FmaOrder::Order213, false,
        InsnId == X86_INS_VFMSUBADD213PD ? 8 : 4);
  case X86_INS_VFMSUBADD231PD:
  case X86_INS_VFMSUBADD231PS:
    return EmitAlternatingFma3(
        FmaOrder::Order231, false,
        InsnId == X86_INS_VFMSUBADD231PD ? 8 : 4);

  case X86_INS_VFMADDSUBPD:
    return EmitAlternatingFma4(true, 8, 0x5d);
  case X86_INS_VFMADDSUBPS:
    return EmitAlternatingFma4(true, 4, 0x5c);
  case X86_INS_VFMSUBADDPD:
    return EmitAlternatingFma4(false, 8, 0x5f);
  case X86_INS_VFMSUBADDPS:
    return EmitAlternatingFma4(false, 4, 0x5e);

  default:
    return false;
  }
  return true;
}

} // namespace neverd
