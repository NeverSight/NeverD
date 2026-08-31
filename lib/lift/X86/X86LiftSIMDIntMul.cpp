//===- X86LiftSIMDIntMul.cpp - x86/x64 SIMD integer multiply lifter -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE/AVX packed integer multiplication: low/high halves,
/// widening even-lane products, and the multiply-add
/// accumulate forms (PMADDWD, PMADDUBSW, PMULHRSW).
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

struct EvexPairwiseMulSpec {
  uint8_t Map;
  uint8_t Opcode;
  uint16_t OutputElementSize;
};

bool beginsWithCanonicalEvexPrefix(const cs_insn *Insn) {
  if (!Insn)
    return false;
  size_t Offset = 0;
  while (Offset < Insn->size) {
    const uint8_t Byte = Insn->bytes[Offset];
    if (Byte == 0x62)
      return true;
    if (Byte != 0x26 && Byte != 0x2e && Byte != 0x36 && Byte != 0x3e &&
        Byte != 0x64 && Byte != 0x65 && Byte != 0x67)
      return false;
    ++Offset;
  }
  return false;
}

bool isVectorRegisterOfSize(const cs_x86_op &Operand, uint16_t Size) {
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

unsigned vectorRegisterIndex(const cs_x86_op &Operand) {
  if (Operand.size == 16)
    return static_cast<unsigned>(Operand.reg - X86_REG_XMM0);
  if (Operand.size == 32)
    return static_cast<unsigned>(Operand.reg - X86_REG_YMM0);
  return static_cast<unsigned>(Operand.reg - X86_REG_ZMM0);
}

bool isEvexCandidate(const cs_insn *Insn, const cs_x86 &X86) {
  return beginsWithCanonicalEvexPrefix(Insn) || X86.opcode[0] == 0x62 ||
         (X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]));
}

bool validateEvexPairwiseMul(X86Lifter &L, const cs_insn *Insn,
                             const cs_x86 &X86, const EvexPairwiseMulSpec &Spec,
                             bool HasWriteMask, const cs_x86_op &Destination,
                             const cs_x86_op *Mask, const cs_x86_op &Left,
                             const cs_x86_op &Right,
                             CanonicalEvexEncodingInfo &Encoding) {
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != Spec.Map ||
      ((Encoding.P1 | 0x04) & 0x07) != 0x05 ||
      Encoding.Opcode != Spec.Opcode || (Encoding.P2 & 0x10) != 0 ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  if (EncodedLength == 0x60)
    return false;
  const uint16_t VectorSize = EncodedLength == 0      ? 16
                              : EncodedLength == 0x20 ? 32
                                                      : 64;
  const bool MemoryForm = Right.type == X86_OP_MEM;
  if (!isVectorRegisterOfSize(Destination, VectorSize) ||
      !isVectorRegisterOfSize(Left, VectorSize) ||
      (!MemoryForm && !isVectorRegisterOfSize(Right, VectorSize)) ||
      (MemoryForm && Right.size != VectorSize) ||
      ((Encoding.ModRM & 0xc0) != 0xc0) != MemoryForm ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
          vectorRegisterIndex(Left) ||
      (!MemoryForm && decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                          vectorRegisterIndex(Right)))
    return false;

  const unsigned LaneCount = VectorSize / Spec.OutputElementSize;
  const uint16_t MaskSize = static_cast<uint16_t>((LaneCount + 7u) / 8u);
  const uint8_t EncodedMask = Encoding.P2 & 7;
  const bool EncodedZero = (Encoding.P2 & 0x80) != 0;
  if (HasWriteMask) {
    if (!Mask || !isX86OpmaskOperand(*Mask) || Mask->reg == X86_REG_K0 ||
        Mask->size != MaskSize ||
        EncodedMask != static_cast<uint8_t>(Mask->reg - X86_REG_K0) ||
        EncodedZero != static_cast<bool>(Mask->avx_zero_opmask))
      return false;
  } else if (Mask || EncodedMask != 0 || EncodedZero) {
    return false;
  }

  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    if (Operand.avx_bcast != X86_AVX_BCAST_INVALID ||
        (Operand.avx_zero_opmask && (!Mask || &Operand != Mask)))
      return false;
  }

  if (MemoryForm)
    return validateCanonicalEvexMemoryTail(Insn, X86, Encoding, Right,
                                           VectorSize);
  return validateCanonicalEvexRegisterTail(Insn, X86, Encoding);
}

NdVar evexActiveMask(const cs_x86_op *Mask, uint16_t VectorSize,
                     uint16_t OutputElementSize) {
  const unsigned LaneCount = VectorSize / OutputElementSize;
  const uint16_t MaskSize = static_cast<uint16_t>((LaneCount + 7u) / 8u);
  if (!Mask) {
    const uint64_t RelevantBits =
        LaneCount == 64 ? UINT64_MAX : ((UINT64_C(1) << LaneCount) - 1);
    return NdVar::cst(RelevantBits, MaskSize);
  }
  const RegInfo MaskInfo = mapCapstoneReg(static_cast<x86_reg>(Mask->reg));
  if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < MaskSize)
    return {};
  return NdVar::reg(MaskInfo.Offset, MaskSize);
}

} // namespace

bool liftSIMDIntMul(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // PMULLD/PMULLW — packed integer multiply (low result), per-lane.
  case X86_INS_PMULLD:
  case X86_INS_PMULLW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    unsigned LaneSz = (InsnId == X86_INS_PMULLD) ? 4 : 2;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {DstR, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {Src, NdVar::cst(Off, 4)});
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
      return Acc;
    };
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }
  // PMULDQ/PMULUDQ — per-lane widening multiply.  Each 64-bit output lane is
  // the product of the *even* (low) dword of that lane from dst and src,
  // sign-extended (PMULDQ) or zero-extended (PMULUDQ) to 64 bits.  A full
  // i128 INT_MULT (the old behaviour) is semantically wrong: it propagates
  // carries across the dword/qword lanes.
  case X86_INS_PMULDQ:
  case X86_INS_PMULUDQ: {
    if (X86.op_count < 2)
      break;
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    bool IsSigned = (InsnId == X86_INS_PMULDQ);
    unsigned NLanes = Dst.Size / 8; // 64-bit output lanes (2=XMM, 4=YMM)
    if (NLanes == 0) {
      S.emit(NdOp::INT_MULT, Dst, {DstR, Src});
      break;
    }
    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      unsigned Off = I * 8; // even dword sits at the base of each qword lane
      NdVar Ad = S.makeTemp(4), Bd = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Ad, {DstR, NdVar::cst(Off, 4)});
      S.emit(NdOp::SUBBYTES, Bd, {Src, NdVar::cst(Off, 4)});
      NdVar Aq = S.makeTemp(8), Bq = S.makeTemp(8);
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Aq, {Ad});
      S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Bq, {Bd});
      NdVar Prod = S.makeTemp(8);
      S.emit(NdOp::INT_MULT, Prod, {Aq, Bq});
      if (I == 0) {
        Acc = Prod;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + 8);
        S.emit(NdOp::CONCAT, Next, {Prod, Acc});
        Acc = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Acc});
    break;
  }

  // PMULHW/PMULHUW: per-lane word multiply, return high 16-bit result.
  case X86_INS_PMULHW:
  case X86_INS_PMULHUW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    bool IsSigned = (InsnId == X86_INS_PMULHW);
    unsigned HalfSz = Dst.Size / 2;
    unsigned WordsPerHalf = HalfSz / 2;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < WordsPerHalf; ++I) {
        unsigned Off = BaseOff + I * 2;
        NdVar Aw = S.makeTemp(2), Bw = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Aw, {DstR, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Bw, {Src, NdVar::cst(Off, 4)});
        NdVar Ax = S.makeTemp(4), Bx = S.makeTemp(4);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Ax, {Aw});
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Bx, {Bw});
        NdVar Prod = S.makeTemp(4);
        S.emit(NdOp::INT_MULT, Prod, {Ax, Bx});
        NdVar Hi = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Hi, {Prod, NdVar::cst(2, 4)});
        if (I == 0) {
          Acc = Hi;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + 2);
          S.emit(NdOp::CONCAT, Next, {Hi, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // PMADDWD: paired word multiply-add → dword results.
  case X86_INS_PMADDWD:
  case X86_INS_VPMADDWD: {
    if (X86.op_count < 2)
      break;
    const bool IsEvex =
        InsnId == X86_INS_VPMADDWD && isEvexCandidate(Insn, X86);
    const bool HasWriteMask =
        X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
    const unsigned LeftIndex = HasWriteMask ? 2 : 1;
    const unsigned RightIndex = HasWriteMask ? 3 : X86.op_count - 1;
    const cs_x86_op *MaskOperand = HasWriteMask ? &X86.operands[1] : nullptr;
    CanonicalEvexEncodingInfo Encoding;
    if (IsEvex) {
      const EvexPairwiseMulSpec Spec{0x01, 0xf5, 4};
      if ((!HasWriteMask && X86.op_count != 3) ||
          (HasWriteMask && X86.op_count != 4) ||
          !validateEvexPairwiseMul(
              L, Insn, X86, Spec, HasWriteMask, X86.operands[0], MaskOperand,
              X86.operands[LeftIndex], X86.operands[RightIndex], Encoding))
        return false;
    } else if (HasWriteMask) {
      return false;
    }

    NdVar Dst = L.operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? L.operandRead(S, X86.operands[LeftIndex])
                    : L.operandRead(S, X86.operands[0]);
    NdVar ActiveMask;
    NdVar B;
    if (IsEvex) {
      ActiveMask = evexActiveMask(MaskOperand, Dst.Size, 4);
      if (ActiveMask.Size == 0)
        return false;
      B = X86.operands[RightIndex].type == X86_OP_MEM
              ? emitEvexMaskedMemoryLoad(S, X86.operands[RightIndex],
                                         ActiveMask, Dst.Size, 4, Dst.Size,
                                         false)
              : L.operandRead(S, X86.operands[RightIndex]);
    } else {
      B = L.operandRead(S, X86.operands[RightIndex]);
    }
    if (Dst.Size == 0 || A.Size != Dst.Size || B.Size != Dst.Size)
      return false;
    unsigned HalfSz = Dst.Size / 2;
    unsigned DwordsPerHalf = HalfSz / 4;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < DwordsPerHalf; ++I) {
        unsigned WOff = BaseOff + I * 4;
        NdVar Aw0 = S.makeTemp(2), Aw1 = S.makeTemp(2);
        NdVar Bw0 = S.makeTemp(2), Bw1 = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Aw0, {A, NdVar::cst(WOff, 4)});
        S.emit(NdOp::SUBBYTES, Aw1, {A, NdVar::cst(WOff + 2, 4)});
        S.emit(NdOp::SUBBYTES, Bw0, {B, NdVar::cst(WOff, 4)});
        S.emit(NdOp::SUBBYTES, Bw1, {B, NdVar::cst(WOff + 2, 4)});
        NdVar Aw0x = S.makeTemp(4), Aw1x = S.makeTemp(4);
        NdVar Bw0x = S.makeTemp(4), Bw1x = S.makeTemp(4);
        S.emit(NdOp::INT_SEXT, Aw0x, {Aw0});
        S.emit(NdOp::INT_SEXT, Aw1x, {Aw1});
        S.emit(NdOp::INT_SEXT, Bw0x, {Bw0});
        S.emit(NdOp::INT_SEXT, Bw1x, {Bw1});
        NdVar P0 = S.makeTemp(4), P1 = S.makeTemp(4);
        S.emit(NdOp::INT_MULT, P0, {Aw0x, Bw0x});
        S.emit(NdOp::INT_MULT, P1, {Aw1x, Bw1x});
        NdVar Sum = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Sum, {P0, P1});
        if (I == 0) {
          Acc = Sum;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + 4);
          S.emit(NdOp::CONCAT, Next, {Sum, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    if (HasWriteMask) {
      if (!emitMaskedVectorResult(L, S, X86.operands[0], *MaskOperand, Full, 4))
        return false;
    } else {
      S.emit(NdOp::COPY, Dst, {Full});
    }
    break;
  }

  // PMADDUBSW: unsigned bytes * signed bytes → signed words with saturation.
  case X86_INS_PMADDUBSW:
  case X86_INS_VPMADDUBSW: {
    if (X86.op_count < 2)
      break;
    const bool IsEvex =
        InsnId == X86_INS_VPMADDUBSW && isEvexCandidate(Insn, X86);
    const bool HasWriteMask =
        X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
    const unsigned LeftIndex = HasWriteMask ? 2 : 1;
    const unsigned RightIndex = HasWriteMask ? 3 : X86.op_count - 1;
    const cs_x86_op *MaskOperand = HasWriteMask ? &X86.operands[1] : nullptr;
    CanonicalEvexEncodingInfo Encoding;
    if (IsEvex) {
      const EvexPairwiseMulSpec Spec{0x02, 0x04, 2};
      if ((!HasWriteMask && X86.op_count != 3) ||
          (HasWriteMask && X86.op_count != 4) ||
          !validateEvexPairwiseMul(
              L, Insn, X86, Spec, HasWriteMask, X86.operands[0], MaskOperand,
              X86.operands[LeftIndex], X86.operands[RightIndex], Encoding))
        return false;
    } else if (HasWriteMask) {
      return false;
    }

    NdVar Dst = L.operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? L.operandRead(S, X86.operands[LeftIndex])
                    : L.operandRead(S, X86.operands[0]);
    NdVar ActiveMask;
    NdVar B;
    if (IsEvex) {
      ActiveMask = evexActiveMask(MaskOperand, Dst.Size, 2);
      if (ActiveMask.Size == 0)
        return false;
      B = X86.operands[RightIndex].type == X86_OP_MEM
              ? emitEvexMaskedMemoryLoad(S, X86.operands[RightIndex],
                                         ActiveMask, Dst.Size, 2, Dst.Size,
                                         false)
              : L.operandRead(S, X86.operands[RightIndex]);
    } else {
      B = L.operandRead(S, X86.operands[RightIndex]);
    }
    if (Dst.Size == 0 || A.Size != Dst.Size || B.Size != Dst.Size)
      return false;
    unsigned HalfSz = Dst.Size / 2;
    unsigned WordsPerHalf = HalfSz / 2;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < WordsPerHalf; ++I) {
        unsigned ByteOff = BaseOff + I * 2;
        NdVar Ab0 = S.makeTemp(1), Ab1 = S.makeTemp(1);
        NdVar Bb0 = S.makeTemp(1), Bb1 = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, Ab0, {A, NdVar::cst(ByteOff, 4)});
        S.emit(NdOp::SUBBYTES, Ab1, {A, NdVar::cst(ByteOff + 1, 4)});
        S.emit(NdOp::SUBBYTES, Bb0, {B, NdVar::cst(ByteOff, 4)});
        S.emit(NdOp::SUBBYTES, Bb1, {B, NdVar::cst(ByteOff + 1, 4)});
        NdVar Ax0 = S.makeTemp(4), Ax1 = S.makeTemp(4);
        NdVar Bx0 = S.makeTemp(4), Bx1 = S.makeTemp(4);
        S.emit(NdOp::INT_ZEXT, Ax0, {Ab0});
        S.emit(NdOp::INT_ZEXT, Ax1, {Ab1});
        S.emit(NdOp::INT_SEXT, Bx0, {Bb0});
        S.emit(NdOp::INT_SEXT, Bx1, {Bb1});
        NdVar P0 = S.makeTemp(4), P1 = S.makeTemp(4);
        S.emit(NdOp::INT_MULT, P0, {Ax0, Bx0});
        S.emit(NdOp::INT_MULT, P1, {Ax1, Bx1});
        NdVar Sum32 = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Sum32, {P0, P1});
        NdVar Clamped = S.makeTemp(2);
        NdVar IsOvfPos = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsOvfPos, {NdVar::cst(0x7FFF, 4), Sum32});
        NdVar IsOvfNeg = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, IsOvfNeg, {Sum32, NdVar::cst(0xFFFF8000U, 4)});
        NdVar Lo = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Lo, {Sum32, NdVar::cst(0, 4)});
        NdVar R1 = S.makeTemp(2);
        S.emit(NdOp::SELECT, R1, {IsOvfPos, NdVar::cst(0x7FFF, 2), Lo});
        S.emit(NdOp::SELECT, Clamped, {IsOvfNeg, NdVar::cst(0x8000, 2), R1});
        if (I == 0) {
          Acc = Clamped;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + 2);
          S.emit(NdOp::CONCAT, Next, {Clamped, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    if (HasWriteMask) {
      if (!emitMaskedVectorResult(L, S, X86.operands[0], *MaskOperand, Full, 2))
        return false;
    } else {
      S.emit(NdOp::COPY, Dst, {Full});
    }
    break;
  }

  // PMULHRSW: multiply high with rounding and scale.
  // result[i] = (a[i]*b[i] + 0x4000) >> 15, per word lane.
  case X86_INS_PMULHRSW:
  case X86_INS_VPMULHRSW: {
    if (X86.op_count < 2)
      break;
    const bool IsEvex =
        InsnId == X86_INS_VPMULHRSW && isEvexCandidate(Insn, X86);
    const bool HasWriteMask =
        X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
    const unsigned LeftIndex = HasWriteMask ? 2 : 1;
    const unsigned RightIndex = HasWriteMask ? 3 : X86.op_count - 1;
    const cs_x86_op *MaskOperand = HasWriteMask ? &X86.operands[1] : nullptr;
    CanonicalEvexEncodingInfo Encoding;
    if (IsEvex) {
      const EvexPairwiseMulSpec Spec{0x02, 0x0b, 2};
      if ((!HasWriteMask && X86.op_count != 3) ||
          (HasWriteMask && X86.op_count != 4) ||
          !validateEvexPairwiseMul(
              L, Insn, X86, Spec, HasWriteMask, X86.operands[0], MaskOperand,
              X86.operands[LeftIndex], X86.operands[RightIndex], Encoding))
        return false;
    } else if (HasWriteMask) {
      return false;
    }

    NdVar Dst = L.operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    NdVar A = IsVex ? L.operandRead(S, X86.operands[LeftIndex])
                    : L.operandRead(S, X86.operands[0]);
    NdVar ActiveMask;
    NdVar B;
    if (IsEvex) {
      ActiveMask = evexActiveMask(MaskOperand, Dst.Size, 2);
      if (ActiveMask.Size == 0)
        return false;
      B = X86.operands[RightIndex].type == X86_OP_MEM
              ? emitEvexMaskedMemoryLoad(S, X86.operands[RightIndex],
                                         ActiveMask, Dst.Size, 2, Dst.Size,
                                         false)
              : L.operandRead(S, X86.operands[RightIndex]);
    } else {
      B = L.operandRead(S, X86.operands[RightIndex]);
    }
    if (Dst.Size == 0 || A.Size != Dst.Size || B.Size != Dst.Size)
      return false;
    unsigned HalfSz = Dst.Size / 2;
    unsigned WordsPerHalf = HalfSz / 2;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < WordsPerHalf; ++I) {
        unsigned Off = BaseOff + I * 2;
        NdVar Aw = S.makeTemp(2), Bw = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Aw, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Bw, {B, NdVar::cst(Off, 4)});
        NdVar Ax = S.makeTemp(4), Bx = S.makeTemp(4);
        S.emit(NdOp::INT_SEXT, Ax, {Aw});
        S.emit(NdOp::INT_SEXT, Bx, {Bw});
        NdVar Prod = S.makeTemp(4);
        S.emit(NdOp::INT_MULT, Prod, {Ax, Bx});
        NdVar Rounded = S.makeTemp(4);
        S.emit(NdOp::INT_ADD, Rounded, {Prod, NdVar::cst(0x4000, 4)});
        NdVar Shifted = S.makeTemp(4);
        S.emit(NdOp::INT_ASHR, Shifted, {Rounded, NdVar::cst(15, 4)});
        NdVar Result = S.makeTemp(2);
        S.emit(NdOp::SUBBYTES, Result, {Shifted, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Result;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + 2);
          S.emit(NdOp::CONCAT, Next, {Result, Acc});
          Acc = Next;
        }
      }
      return Acc;
    };
    NdVar LoHalf = BuildHalf(0);
    NdVar HiHalf = BuildHalf(HalfSz);
    NdVar Full = S.makeTemp(Dst.Size);
    S.emit(NdOp::CONCAT, Full, {HiHalf, LoHalf});
    if (HasWriteMask) {
      if (!emitMaskedVectorResult(L, S, X86.operands[0], *MaskOperand, Full, 2))
        return false;
    } else {
      S.emit(NdOp::COPY, Dst, {Full});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
