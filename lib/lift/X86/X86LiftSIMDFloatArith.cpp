//===- X86LiftSIMDFloatArith.cpp - x86/x64 SIMD floating-point arithmetic lifter
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE/AVX packed and scalar floating-point arithmetic:
/// add/subtract/multiply/divide, horizontal add/subtract, the
/// alternating add-subtract forms, minimum/maximum, and the
/// square-root and reciprocal approximations.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

struct EvexFloatArithSpec {
  uint16_t ElementSize = 0;
  bool W = false;
  uint8_t MandatoryPrefix = 0;
  uint8_t Opcode = 0;
  bool Scalar = false;
  X86FPArithKind Kind = X86FPArithKind::Add;
  bool UnaryPacked = false;
  bool SAEOnly = false;
};

bool getEvexFloatArithSpec(unsigned InsnId, EvexFloatArithSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VADDPS:
    Spec = {4, false, 0, 0x58, false, X86FPArithKind::Add};
    return true;
  case X86_INS_VADDPD:
    Spec = {8, true, 1, 0x58, false, X86FPArithKind::Add};
    return true;
  case X86_INS_VADDSS:
    Spec = {4, false, 2, 0x58, true, X86FPArithKind::Add};
    return true;
  case X86_INS_VADDSD:
    Spec = {8, true, 3, 0x58, true, X86FPArithKind::Add};
    return true;
  case X86_INS_VSUBPS:
    Spec = {4, false, 0, 0x5c, false, X86FPArithKind::Subtract};
    return true;
  case X86_INS_VSUBPD:
    Spec = {8, true, 1, 0x5c, false, X86FPArithKind::Subtract};
    return true;
  case X86_INS_VSUBSS:
    Spec = {4, false, 2, 0x5c, true, X86FPArithKind::Subtract};
    return true;
  case X86_INS_VSUBSD:
    Spec = {8, true, 3, 0x5c, true, X86FPArithKind::Subtract};
    return true;
  case X86_INS_VMULPS:
    Spec = {4, false, 0, 0x59, false, X86FPArithKind::Multiply};
    return true;
  case X86_INS_VMULPD:
    Spec = {8, true, 1, 0x59, false, X86FPArithKind::Multiply};
    return true;
  case X86_INS_VMULSS:
    Spec = {4, false, 2, 0x59, true, X86FPArithKind::Multiply};
    return true;
  case X86_INS_VMULSD:
    Spec = {8, true, 3, 0x59, true, X86FPArithKind::Multiply};
    return true;
  case X86_INS_VDIVPS:
    Spec = {4, false, 0, 0x5e, false, X86FPArithKind::Divide};
    return true;
  case X86_INS_VDIVPD:
    Spec = {8, true, 1, 0x5e, false, X86FPArithKind::Divide};
    return true;
  case X86_INS_VDIVSS:
    Spec = {4, false, 2, 0x5e, true, X86FPArithKind::Divide};
    return true;
  case X86_INS_VDIVSD:
    Spec = {8, true, 3, 0x5e, true, X86FPArithKind::Divide};
    return true;
  case X86_INS_VMINPS:
    Spec = {4, false, 0, 0x5d, false, X86FPArithKind::Minimum, false, true};
    return true;
  case X86_INS_VMINPD:
    Spec = {8, true, 1, 0x5d, false, X86FPArithKind::Minimum, false, true};
    return true;
  case X86_INS_VMINSS:
    Spec = {4, false, 2, 0x5d, true, X86FPArithKind::Minimum, false, true};
    return true;
  case X86_INS_VMINSD:
    Spec = {8, true, 3, 0x5d, true, X86FPArithKind::Minimum, false, true};
    return true;
  case X86_INS_VMAXPS:
    Spec = {4, false, 0, 0x5f, false, X86FPArithKind::Maximum, false, true};
    return true;
  case X86_INS_VMAXPD:
    Spec = {8, true, 1, 0x5f, false, X86FPArithKind::Maximum, false, true};
    return true;
  case X86_INS_VMAXSS:
    Spec = {4, false, 2, 0x5f, true, X86FPArithKind::Maximum, false, true};
    return true;
  case X86_INS_VMAXSD:
    Spec = {8, true, 3, 0x5f, true, X86FPArithKind::Maximum, false, true};
    return true;
  case X86_INS_VSQRTPS:
    Spec = {4, false, 0, 0x51, false, X86FPArithKind::SquareRoot, true};
    return true;
  case X86_INS_VSQRTPD:
    Spec = {8, true, 1, 0x51, false, X86FPArithKind::SquareRoot, true};
    return true;
  case X86_INS_VSQRTSS:
    Spec = {4, false, 2, 0x51, true, X86FPArithKind::SquareRoot};
    return true;
  case X86_INS_VSQRTSD:
    Spec = {8, true, 3, 0x51, true, X86FPArithKind::SquareRoot};
    return true;
  default:
    return false;
  }
}

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

x86_avx_bcast broadcastForLaneCount(unsigned LaneCount) {
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

bool validateEvexFloatArith(X86Lifter &L, const cs_insn *Insn,
                            const cs_x86 &X86, const EvexFloatArithSpec &Spec,
                            bool HasWriteMask, const cs_x86_op &Destination,
                            const cs_x86_op *Mask, const cs_x86_op &Left,
                            const cs_x86_op &Right,
                            CanonicalEvexEncodingInfo &Encoding,
                            bool &Broadcast, X86FPRounding &Rounding,
                            bool &SuppressExceptions) {
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x01 ||
      ((Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((Spec.W ? 0x80 : 0) | 0x04 |
                               Spec.MandatoryPrefix) ||
      Encoding.Opcode != Spec.Opcode || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0)
    return false;

  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  const bool MemoryForm = Right.type == X86_OP_MEM;
  const bool EncodedB = (Encoding.P2 & 0x10) != 0;
  const bool EmbeddedControl = !MemoryForm && EncodedB;
  const bool EmbeddedRounding = EmbeddedControl && !Spec.SAEOnly;
  const uint16_t VectorSize = Spec.Scalar             ? 16
                              : EmbeddedControl       ? 64
                              : EncodedLength == 0    ? 16
                              : EncodedLength == 0x20 ? 32
                                                      : 64;
  if ((Spec.Scalar && !EmbeddedControl && EncodedLength != 0) ||
      (!Spec.Scalar && !EmbeddedControl && EncodedLength == 0x60) ||
      (!Spec.Scalar && EmbeddedControl && Destination.size != 64))
    return false;

  SuppressExceptions = EmbeddedControl;
  Rounding = EmbeddedRounding ? static_cast<X86FPRounding>(EncodedLength >> 5)
                              : X86FPRounding::MXCSR;
  const x86_avx_rm ExpectedRounding =
      EmbeddedRounding
          ? static_cast<x86_avx_rm>(X86_AVX_RM_RN + (EncodedLength >> 5))
          : X86_AVX_RM_INVALID;
  if (X86.avx_sae != EmbeddedControl || X86.avx_rm != ExpectedRounding)
    return false;

  if (((Encoding.ModRM & 0xc0) != 0xc0) != MemoryForm ||
      !isVectorRegisterOfSize(Destination, VectorSize) ||
      !isVectorRegisterOfSize(Left, VectorSize) ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
          vectorRegisterIndex(Left))
    return false;

  const unsigned LaneCount = Spec.Scalar ? 1 : VectorSize / Spec.ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
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
    if (&Operand != &Right && Operand.avx_bcast != X86_AVX_BCAST_INVALID)
      return false;
    if (Operand.avx_zero_opmask && (!Mask || &Operand != Mask))
      return false;
  }

  Broadcast = MemoryForm && EncodedB;
  if (!MemoryForm) {
    return isVectorRegisterOfSize(Right, VectorSize) &&
           Right.avx_bcast == X86_AVX_BCAST_INVALID &&
           decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) ==
               vectorRegisterIndex(Right) &&
           validateCanonicalEvexRegisterTail(Insn, X86, Encoding);
  }

  if (Spec.Scalar && EncodedB)
    return false;
  const uint16_t TupleSize =
      Spec.Scalar || Broadcast ? Spec.ElementSize : VectorSize;
  const x86_avx_bcast ExpectedBroadcast =
      Broadcast ? broadcastForLaneCount(LaneCount) : X86_AVX_BCAST_INVALID;
  return Right.size == TupleSize && Right.avx_bcast == ExpectedBroadcast &&
         validateCanonicalEvexMemoryTail(Insn, X86, Encoding, Right, TupleSize);
}

bool validateEvexSqrt(X86Lifter &L, const cs_insn *Insn, const cs_x86 &X86,
                      const EvexFloatArithSpec &Spec, bool HasWriteMask,
                      const cs_x86_op &Destination, const cs_x86_op *Mask,
                      const cs_x86_op *MergeSource, const cs_x86_op &Source,
                      bool &MemoryForm, bool &Broadcast,
                      X86FPRounding &Rounding, bool &SuppressExceptions) {
  CanonicalEvexEncodingInfo Encoding;
  if (Spec.Kind != X86FPArithKind::SquareRoot ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x01 ||
      ((Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((Spec.W ? 0x80 : 0) | 0x04 |
                               Spec.MandatoryPrefix) ||
      Encoding.Opcode != Spec.Opcode || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0)
    return false;

  MemoryForm = Source.type == X86_OP_MEM;
  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  const bool EncodedB = (Encoding.P2 & 0x10) != 0;
  const bool EmbeddedRounding = !MemoryForm && EncodedB;
  Broadcast = MemoryForm && EncodedB;
  const uint16_t VectorSize = Spec.Scalar             ? 16
                              : EmbeddedRounding      ? 64
                              : EncodedLength == 0    ? 16
                              : EncodedLength == 0x20 ? 32
                                                      : 64;
  if ((Spec.Scalar && !EmbeddedRounding && EncodedLength != 0) ||
      (!Spec.Scalar && !EmbeddedRounding && EncodedLength == 0x60) ||
      (!Spec.Scalar && EmbeddedRounding && VectorSize != 64) ||
      (Spec.Scalar && Broadcast))
    return false;

  SuppressExceptions = EmbeddedRounding;
  Rounding = EmbeddedRounding ? static_cast<X86FPRounding>(EncodedLength >> 5)
                              : X86FPRounding::MXCSR;
  const x86_avx_rm ExpectedRounding =
      EmbeddedRounding
          ? static_cast<x86_avx_rm>(X86_AVX_RM_RN + (EncodedLength >> 5))
          : X86_AVX_RM_INVALID;
  if (X86.avx_sae != EmbeddedRounding || X86.avx_rm != ExpectedRounding ||
      ((Encoding.ModRM & 0xc0) != 0xc0) != MemoryForm ||
      !isVectorRegisterOfSize(Destination, VectorSize) ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(Destination))
    return false;

  if (Spec.UnaryPacked) {
    if (MergeSource || (Encoding.P1 & 0x78) != 0x78 ||
        (Encoding.P2 & 0x08) == 0)
      return false;
  } else if (!MergeSource ||
             !isVectorRegisterOfSize(*MergeSource, VectorSize) ||
             decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
                 vectorRegisterIndex(*MergeSource)) {
    return false;
  }

  const unsigned LaneCount = Spec.Scalar ? 1 : VectorSize / Spec.ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
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

  const x86_avx_bcast ExpectedBroadcast =
      Broadcast ? broadcastForLaneCount(LaneCount) : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    const bool IsMask = HasWriteMask && &Operand == Mask;
    if (Operand.avx_zero_opmask != (IsMask && EncodedZero) ||
        Operand.avx_bcast !=
            (&Operand == &Source ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }

  if (!MemoryForm)
    return isVectorRegisterOfSize(Source, VectorSize) &&
           decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) ==
               vectorRegisterIndex(Source) &&
           validateCanonicalEvexRegisterTail(Insn, X86, Encoding);

  const uint16_t TupleSize =
      Spec.Scalar || Broadcast ? Spec.ElementSize : VectorSize;
  return Source.size == TupleSize &&
         validateCanonicalEvexMemoryTail(Insn, X86, Encoding, Source,
                                         TupleSize);
}

} // namespace

bool liftSIMDFloatArith(X86Lifter &L, X86Lifter::LiftState &S,
                        const cs_insn *Insn, const cs_x86 &X86) {
  const unsigned InsnId = Insn->id;
  const bool IsEvex = beginsWithCanonicalEvexPrefix(Insn);
  EvexFloatArithSpec EvexFloatSpec;
  const bool IsEvexFloatArith =
      IsEvex && getEvexFloatArithSpec(InsnId, EvexFloatSpec);
  if (hasUnsupportedEvexValueModifier(X86) && !IsEvexFloatArith)
    return false;
  if (IsEvex && !IsEvexFloatArith)
    for (unsigned Index = 0; Index < X86.op_count; ++Index)
      if (X86.operands[Index].type == X86_OP_MEM)
        return false;

  switch (InsnId) {

  // HADDPS/HADDPD/HSUBPS/HSUBPD — horizontal pair-wise add/sub.
  // HADDPS: Dst[0]=A[0]+A[1], Dst[1]=A[2]+A[3], Dst[2]=B[0]+B[1],
  // Dst[3]=B[2]+B[3] HADDPD: Dst[0]=A[0]+A[1], Dst[1]=B[0]+B[1]
  case X86_INS_VHADDPS:
  case X86_INS_VHSUBPS:
  case X86_INS_HADDPS:
  case X86_INS_HSUBPS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    bool IsSub = (InsnId == X86_INS_VHSUBPS || InsnId == X86_INS_HSUBPS);
    NdOp Opc = IsSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD;
    // Horizontal pair-add/sub is per-128-bit lane: each result lane's low 64
    // bits are src1's two pair reductions, its high 64 bits src2's (for the
    // SAME lane).  The old code only read the low 128 bits, so the 256-bit ymm
    // (VEX) form computed garbage for the high lane.  Build lane-by-lane.
    unsigned NumLanes = Dst.Size >= 16 ? Dst.Size / 16 : 1;
    auto laneF = [&](NdVar Src, unsigned Off) -> NdVar {
      NdVar E = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, E, {Src, NdVar::cst(Off, 4)});
      return E;
    };
    NdVar Full = S.makeTemp(0);
    for (unsigned L = 0; L < NumLanes; ++L) {
      unsigned Base = L * 16;
      NdVar R0 = S.makeTemp(4), R1 = S.makeTemp(4), R2 = S.makeTemp(4),
            R3 = S.makeTemp(4);
      S.emit(Opc, R0, {laneF(A, Base + 0), laneF(A, Base + 4)});
      S.emit(Opc, R1, {laneF(A, Base + 8), laneF(A, Base + 12)});
      S.emit(Opc, R2, {laneF(B, Base + 0), laneF(B, Base + 4)});
      S.emit(Opc, R3, {laneF(B, Base + 8), laneF(B, Base + 12)});
      NdVar Lo = S.makeTemp(8), Hi = S.makeTemp(8), Lane = S.makeTemp(16);
      S.emit(NdOp::CONCAT, Lo, {R1, R0});
      S.emit(NdOp::CONCAT, Hi, {R3, R2});
      S.emit(NdOp::CONCAT, Lane, {Hi, Lo});
      if (L == 0) {
        Full = Lane;
      } else {
        NdVar Next = S.makeTemp((L + 1) * 16);
        S.emit(NdOp::CONCAT, Next, {Lane, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }
  case X86_INS_VHADDPD:
  case X86_INS_VHSUBPD:
  case X86_INS_HADDPD:
  case X86_INS_HSUBPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    bool IsSub = (InsnId == X86_INS_VHSUBPD || InsnId == X86_INS_HSUBPD);
    NdOp Opc = IsSub ? NdOp::FLOAT_SUB : NdOp::FLOAT_ADD;
    // Per-128-bit lane (each lane holds two doubles): result lane low = src1's
    // pair sum, high = src2's pair sum.  The old code handled only the low 128
    // bits; rebuild lane-by-lane so the 256-bit ymm form is correct.
    unsigned NumLanes = Dst.Size >= 16 ? Dst.Size / 16 : 1;
    auto laneD = [&](NdVar Src, unsigned Off) -> NdVar {
      NdVar E = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, E, {Src, NdVar::cst(Off, 4)});
      return E;
    };
    NdVar Full = S.makeTemp(0);
    for (unsigned L = 0; L < NumLanes; ++L) {
      unsigned Base = L * 16;
      NdVar R0 = S.makeTemp(8), R1 = S.makeTemp(8), Lane = S.makeTemp(16);
      S.emit(Opc, R0, {laneD(A, Base + 0), laneD(A, Base + 8)});
      S.emit(Opc, R1, {laneD(B, Base + 0), laneD(B, Base + 8)});
      S.emit(NdOp::CONCAT, Lane, {R1, R0});
      if (L == 0) {
        Full = Lane;
      } else {
        NdVar Next = S.makeTemp((L + 1) * 16);
        S.emit(NdOp::CONCAT, Next, {Lane, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // ADDSUBPS/ADDSUBPD — alternating sub/add per lane.
  // PS: Dst[0]=A[0]-B[0], Dst[1]=A[1]+B[1], Dst[2]=A[2]-B[2], Dst[3]=A[3]+B[3]
  // PD: Dst[0]=A[0]-B[0], Dst[1]=A[1]+B[1]
  case X86_INS_VADDSUBPS:
  case X86_INS_ADDSUBPS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    // Even single lanes subtract, odd lanes add — a uniform per-element pattern
    // across the whole register.  The old code only did the low 4 floats, so
    // the 256-bit ymm (VEX) form left the high 128 bits uncomputed.
    unsigned NElems = Dst.Size / 4;
    NdVar Full = S.makeTemp(0);
    for (unsigned I = 0; I < NElems; ++I) {
      NdVar Ae = S.makeTemp(4), Be = S.makeTemp(4), R = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, Ae, {A, NdVar::cst(I * 4, 4)});
      S.emit(NdOp::SUBBYTES, Be, {B, NdVar::cst(I * 4, 4)});
      S.emit((I & 1) ? NdOp::FLOAT_ADD : NdOp::FLOAT_SUB, R, {Ae, Be});
      if (I == 0) {
        Full = R;
      } else {
        NdVar Next = S.makeTemp((I + 1) * 4);
        S.emit(NdOp::CONCAT, Next, {R, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }
  case X86_INS_VADDSUBPD:
  case X86_INS_ADDSUBPD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    // Even double lanes subtract, odd lanes add; uniform per-element across the
    // whole register (the old code only did the low 2 doubles → ymm truncated).
    unsigned NElems = Dst.Size / 8;
    NdVar Full = S.makeTemp(0);
    for (unsigned I = 0; I < NElems; ++I) {
      NdVar Ae = S.makeTemp(8), Be = S.makeTemp(8), R = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Ae, {A, NdVar::cst(I * 8, 4)});
      S.emit(NdOp::SUBBYTES, Be, {B, NdVar::cst(I * 8, 4)});
      S.emit((I & 1) ? NdOp::FLOAT_ADD : NdOp::FLOAT_SUB, R, {Ae, Be});
      if (I == 0) {
        Full = R;
      } else {
        NdVar Next = S.makeTemp((I + 1) * 8);
        S.emit(NdOp::CONCAT, Next, {R, Full});
        Full = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Full});
    break;
  }

  // SQRTSS/SQRTSD/SQRTPS/SQRTPD — float square root.
  case X86_INS_SQRTSS:
  case X86_INS_SQRTSD:
  case X86_INS_SQRTPS:
  case X86_INS_SQRTPD:
  case X86_INS_RSQRTSS:
  case X86_INS_RSQRTPS:
  case X86_INS_RCPSS:
  case X86_INS_RCPPS:
  case X86_INS_VSQRTSS:
  case X86_INS_VSQRTSD:
  case X86_INS_VSQRTPS:
  case X86_INS_VSQRTPD:
  case X86_INS_VRSQRTSS:
  case X86_INS_VRSQRTPS:
  case X86_INS_VRCPSS:
  case X86_INS_VRCPPS: {
    if (X86.op_count < 2)
      break;

    const bool IsStrictEvexSqrt =
        IsEvexFloatArith && EvexFloatSpec.Kind == X86FPArithKind::SquareRoot;
    if (IsStrictEvexSqrt) {
      const bool HasWriteMask =
          X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
      const unsigned SourceIndex = EvexFloatSpec.Scalar
                                       ? (HasWriteMask ? 3 : 2)
                                       : (HasWriteMask ? 2 : 1);
      const unsigned ExpectedOperands = EvexFloatSpec.Scalar
                                            ? (HasWriteMask ? 4 : 3)
                                            : (HasWriteMask ? 3 : 2);
      if (X86.op_count != ExpectedOperands ||
          X86.operands[0].type != X86_OP_REG ||
          (X86.operands[SourceIndex].type != X86_OP_REG &&
           X86.operands[SourceIndex].type != X86_OP_MEM))
        return false;

      const cs_x86_op *MaskOperand = HasWriteMask ? &X86.operands[1] : nullptr;
      const cs_x86_op *MergeSource =
          EvexFloatSpec.Scalar ? &X86.operands[HasWriteMask ? 2 : 1] : nullptr;
      bool MemoryForm = false;
      bool Broadcast = false;
      bool SuppressExceptions = false;
      X86FPRounding Rounding = X86FPRounding::MXCSR;
      if (!validateEvexSqrt(L, Insn, X86, EvexFloatSpec, HasWriteMask,
                            X86.operands[0], MaskOperand, MergeSource,
                            X86.operands[SourceIndex], MemoryForm, Broadcast,
                            Rounding, SuppressExceptions))
        return false;

      NdVar Dst = L.operandWrite(X86.operands[0]);
      const unsigned LaneCount =
          EvexFloatSpec.Scalar ? 1 : Dst.Size / EvexFloatSpec.ElementSize;
      const uint16_t RequiredMaskSize =
          static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
      const uint64_t RelevantMask = (UINT64_C(1) << LaneCount) - UINT64_C(1);
      NdVar ActiveMask = NdVar::cst(RelevantMask, RequiredMaskSize);
      RegInfo MaskInfo{0, 0};
      if (HasWriteMask) {
        MaskInfo = mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
        if (MaskInfo.Offset == UINT64_C(0xffff) ||
            MaskInfo.Size < RequiredMaskSize)
          return false;
        ActiveMask = NdVar::reg(MaskInfo.Offset, RequiredMaskSize);
      }

      NdVar Source;
      if (MemoryForm) {
        NdVar LoadMask = ActiveMask;
        if (EvexFloatSpec.Scalar && HasWriteMask) {
          LoadMask = S.makeTemp(ActiveMask.Size);
          S.emit(NdOp::INT_AND, LoadMask,
                 {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
        }
        const uint16_t MemoryTupleSize = EvexFloatSpec.Scalar || Broadcast
                                             ? EvexFloatSpec.ElementSize
                                             : Dst.Size;
        Source = emitEvexMaskedMemoryLoad(
            S, X86.operands[SourceIndex], LoadMask, Dst.Size,
            EvexFloatSpec.ElementSize, MemoryTupleSize, Broadcast);
      } else {
        Source = L.operandRead(S, X86.operands[SourceIndex]);
      }
      if (Dst.Size == 0 || Source.Size != Dst.Size ||
          Dst.Size % EvexFloatSpec.ElementSize != 0)
        return false;

      const uint16_t Control = makeX86FPArithControl(
          X86FPArithKind::SquareRoot, EvexFloatSpec.ElementSize == 8,
          EvexFloatSpec.Scalar, SuppressExceptions, Rounding);
      NdVar Raw = S.makeTemp(Dst.Size);
      S.emitIntrinsic(
          Intrinsic::X86FPArith, Raw,
          {NdVar::cst(Control, 2), Source, Source, Source, ActiveMask});

      if (!EvexFloatSpec.Scalar) {
        if (HasWriteMask)
          return emitMaskedVectorResult(L, S, X86.operands[0], X86.operands[1],
                                        Raw, EvexFloatSpec.ElementSize);
        S.emit(NdOp::COPY, Dst, {Raw});
        break;
      }

      NdVar NewLow = S.makeTemp(EvexFloatSpec.ElementSize);
      S.emit(NdOp::SUBBYTES, NewLow, {Raw, NdVar::cst(0, 4)});
      NdVar Low = NewLow;
      if (HasWriteMask) {
        NdVar MaskBit = S.makeTemp(1);
        S.emit(NdOp::INT_AND, MaskBit,
               {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
        NdVar Inactive = NdVar::cst(0, EvexFloatSpec.ElementSize);
        if (!X86.operands[1].avx_zero_opmask) {
          const NdVar OldDestination = L.operandRead(S, X86.operands[0]);
          Inactive = S.makeTemp(EvexFloatSpec.ElementSize);
          S.emit(NdOp::SUBBYTES, Inactive, {OldDestination, NdVar::cst(0, 4)});
        }
        Low = S.makeTemp(EvexFloatSpec.ElementSize);
        S.emit(NdOp::SELECT, Low, {MaskBit, NewLow, Inactive});
      }
      const NdVar UpperSource = L.operandRead(S, *MergeSource);
      if (UpperSource.Size != Dst.Size)
        return false;
      NdVar Upper = S.makeTemp(Dst.Size - EvexFloatSpec.ElementSize);
      S.emit(NdOp::SUBBYTES, Upper,
             {UpperSource, NdVar::cst(EvexFloatSpec.ElementSize, 4)});
      S.emit(NdOp::CONCAT, Dst, {Upper, Low});
      break;
    }

    bool IsPacked = false;
    unsigned LaneSz = 0;
    switch (InsnId) {
    case X86_INS_SQRTPS:
    case X86_INS_VSQRTPS:
    case X86_INS_RSQRTPS:
    case X86_INS_VRSQRTPS:
    case X86_INS_RCPPS:
    case X86_INS_VRCPPS:
      IsPacked = true;
      LaneSz = 4;
      break;
    case X86_INS_SQRTPD:
    case X86_INS_VSQRTPD:
      IsPacked = true;
      LaneSz = 8;
      break;
    default:
      break;
    }

    const bool HasWriteMask =
        X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
    if (HasWriteMask &&
        ((InsnId != X86_INS_VSQRTPS && InsnId != X86_INS_VSQRTPD) ||
         X86.op_count != 3 || X86.operands[0].type != X86_OP_REG ||
         X86.operands[2].type != X86_OP_REG))
      return false;

    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[X86.op_count - 1]);

    bool IsRcp = (InsnId == X86_INS_RCPPS || InsnId == X86_INS_RCPSS ||
                  InsnId == X86_INS_VRCPPS || InsnId == X86_INS_VRCPSS);
    bool IsRsqrt = (InsnId == X86_INS_RSQRTPS || InsnId == X86_INS_RSQRTSS ||
                    InsnId == X86_INS_VRSQRTPS || InsnId == X86_INS_VRSQRTSS);

    auto emitLaneOp = [&](NdVar In, NdVar Out) {
      if (IsRcp) {
        NdVar One = NdVar::cst(0x3F800000, In.Size); // 1.0f
        S.emit(NdOp::FLOAT_DIV, Out, {One, In});
      } else if (IsRsqrt) {
        NdVar Sq = S.makeTemp(In.Size);
        S.emit(NdOp::FLOAT_SQRT, Sq, {In});
        NdVar One = NdVar::cst(0x3F800000, In.Size); // 1.0f
        S.emit(NdOp::FLOAT_DIV, Out, {One, Sq});
      } else {
        S.emit(NdOp::FLOAT_SQRT, Out, {In});
      }
    };

    if (IsPacked && Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar Lane = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lane, {Src, NdVar::cst(I * LaneSz, 4)});
        NdVar Res = S.makeTemp(LaneSz);
        emitLaneOp(Lane, Res);
        if (I == 0) {
          Acc = Res;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Res, Acc});
          Acc = Next;
        }
      }
      if (HasWriteMask) {
        if (!emitMaskedVectorResult(L, S, X86.operands[0], X86.operands[1], Acc,
                                    LaneSz))
          return false;
      } else {
        S.emit(NdOp::COPY, Dst, {Acc});
      }
    } else if (Dst.Size > 8) {
      // Scalar form (SQRTSS/SD, RSQRTSS, RCPSS): operate on the low element
      // only and keep the upper lanes.  Extracting the scalar — instead of
      // handing the whole 16B XMM to the float emitter, which infers double
      // from the width — is what makes a *SS op single-precision (else SQRTSS
      // became sqrt.f64).
      unsigned ScalarSz =
          (InsnId == X86_INS_SQRTSD || InsnId == X86_INS_VSQRTSD) ? 8 : 4;
      NdVar In = S.makeTemp(ScalarSz);
      S.emit(NdOp::SUBBYTES, In, {Src, NdVar::cst(0, 4)});
      NdVar Res = S.makeTemp(ScalarSz);
      emitLaneOp(In, Res);
      // VEX scalar takes the upper lanes from src1 (operand 1); SSE keeps
      // Dst's.
      NdVar Upper =
          (X86.op_count >= 3) ? L.operandRead(S, X86.operands[1]) : Dst;
      NdVar Hi = S.makeTemp(Dst.Size - ScalarSz);
      S.emit(NdOp::SUBBYTES, Hi, {Upper, NdVar::cst(ScalarSz, 4)});
      S.emit(NdOp::CONCAT, Dst, {Hi, Res});
    } else {
      emitLaneOp(Src, Dst);
    }
    break;
  }

  // Packed min/max — float: per-lane FLOAT_LESS + SELECT
  case X86_INS_MINSS:
  case X86_INS_MINSD:
  case X86_INS_MINPS:
  case X86_INS_MINPD:
  case X86_INS_MAXSS:
  case X86_INS_MAXSD:
  case X86_INS_MAXPS:
  case X86_INS_MAXPD:
  case X86_INS_VMINSS:
  case X86_INS_VMINSD:
  case X86_INS_VMINPS:
  case X86_INS_VMINPD:
  case X86_INS_VMAXSS:
  case X86_INS_VMAXSD:
  case X86_INS_VMAXPS:
  case X86_INS_VMAXPD: {
    if (X86.op_count < 2)
      break;
    bool IsMin = (InsnId == X86_INS_MINSS || InsnId == X86_INS_MINSD ||
                  InsnId == X86_INS_MINPS || InsnId == X86_INS_MINPD ||
                  InsnId == X86_INS_VMINSS || InsnId == X86_INS_VMINSD ||
                  InsnId == X86_INS_VMINPS || InsnId == X86_INS_VMINPD);

    bool IsScalar = (InsnId == X86_INS_MINSS || InsnId == X86_INS_MAXSS ||
                     InsnId == X86_INS_VMINSS || InsnId == X86_INS_VMAXSS ||
                     InsnId == X86_INS_MINSD || InsnId == X86_INS_MAXSD ||
                     InsnId == X86_INS_VMINSD || InsnId == X86_INS_VMAXSD);

    const bool HasWriteMask =
        X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
    if (IsEvexFloatArith) {
      const unsigned LeftIndex = HasWriteMask ? 2 : 1;
      const unsigned RightIndex = HasWriteMask ? 3 : 2;
      if ((!HasWriteMask && X86.op_count != 3) ||
          (HasWriteMask && X86.op_count != 4) ||
          X86.operands[0].type != X86_OP_REG ||
          X86.operands[LeftIndex].type != X86_OP_REG ||
          (X86.operands[RightIndex].type != X86_OP_REG &&
           X86.operands[RightIndex].type != X86_OP_MEM))
        return false;

      CanonicalEvexEncodingInfo Encoding;
      bool Broadcast = false;
      bool SuppressExceptions = false;
      X86FPRounding Rounding = X86FPRounding::MXCSR;
      const cs_x86_op *MaskOperand = HasWriteMask ? &X86.operands[1] : nullptr;
      if (!validateEvexFloatArith(
              L, Insn, X86, EvexFloatSpec, HasWriteMask, X86.operands[0],
              MaskOperand, X86.operands[LeftIndex], X86.operands[RightIndex],
              Encoding, Broadcast, Rounding, SuppressExceptions))
        return false;

      NdVar Dst = L.operandWrite(X86.operands[0]);
      NdVar A = L.operandRead(S, X86.operands[LeftIndex]);
      const unsigned LaneCount =
          EvexFloatSpec.Scalar ? 1 : Dst.Size / EvexFloatSpec.ElementSize;
      const uint16_t RequiredMaskSize =
          static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
      const uint64_t RelevantMask = (UINT64_C(1) << LaneCount) - UINT64_C(1);
      NdVar ActiveMask = NdVar::cst(RelevantMask, RequiredMaskSize);
      RegInfo MaskInfo{0, 0};
      if (HasWriteMask) {
        MaskInfo = mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
        if (MaskInfo.Offset == UINT64_C(0xffff) ||
            MaskInfo.Size < RequiredMaskSize)
          return false;
        ActiveMask = NdVar::reg(MaskInfo.Offset, RequiredMaskSize);
      }

      NdVar B;
      if (X86.operands[RightIndex].type == X86_OP_MEM) {
        NdVar LoadMask = ActiveMask;
        if (EvexFloatSpec.Scalar && HasWriteMask) {
          LoadMask = S.makeTemp(ActiveMask.Size);
          S.emit(NdOp::INT_AND, LoadMask,
                 {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
        }
        const uint16_t MemoryTupleSize = EvexFloatSpec.Scalar || Broadcast
                                             ? EvexFloatSpec.ElementSize
                                             : Dst.Size;
        B = emitEvexMaskedMemoryLoad(S, X86.operands[RightIndex], LoadMask,
                                     Dst.Size, EvexFloatSpec.ElementSize,
                                     MemoryTupleSize, Broadcast);
      } else {
        B = L.operandRead(S, X86.operands[RightIndex]);
      }
      if (Dst.Size == 0 || A.Size != Dst.Size || B.Size != Dst.Size ||
          Dst.Size % EvexFloatSpec.ElementSize != 0)
        return false;

      const uint16_t Control = makeX86FPArithControl(
          EvexFloatSpec.Kind, EvexFloatSpec.ElementSize == 8,
          EvexFloatSpec.Scalar, SuppressExceptions, Rounding);
      NdVar Raw = S.makeTemp(Dst.Size);
      S.emitIntrinsic(Intrinsic::X86FPArith, Raw,
                      {NdVar::cst(Control, 2), A, B, A, ActiveMask});
      if (!EvexFloatSpec.Scalar) {
        if (HasWriteMask &&
            !emitMaskedVectorResult(L, S, X86.operands[0], X86.operands[1], Raw,
                                    EvexFloatSpec.ElementSize))
          return false;
        if (!HasWriteMask)
          S.emit(NdOp::COPY, Dst, {Raw});
        break;
      }

      NdVar NewLow = S.makeTemp(EvexFloatSpec.ElementSize);
      S.emit(NdOp::SUBBYTES, NewLow, {Raw, NdVar::cst(0, 4)});
      NdVar Low = NewLow;
      if (HasWriteMask) {
        NdVar MaskBit = S.makeTemp(1);
        S.emit(NdOp::INT_AND, MaskBit,
               {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
        NdVar Inactive = NdVar::cst(0, EvexFloatSpec.ElementSize);
        if (!X86.operands[1].avx_zero_opmask) {
          const NdVar OldDestination = L.operandRead(S, X86.operands[0]);
          Inactive = S.makeTemp(EvexFloatSpec.ElementSize);
          S.emit(NdOp::SUBBYTES, Inactive, {OldDestination, NdVar::cst(0, 4)});
        }
        Low = S.makeTemp(EvexFloatSpec.ElementSize);
        S.emit(NdOp::SELECT, Low, {MaskBit, NewLow, Inactive});
      }
      NdVar Upper = S.makeTemp(Dst.Size - EvexFloatSpec.ElementSize);
      S.emit(NdOp::SUBBYTES, Upper,
             {A, NdVar::cst(EvexFloatSpec.ElementSize, 4)});
      S.emit(NdOp::CONCAT, Dst, {Upper, Low});
      break;
    }

    if (HasWriteMask &&
        (IsScalar || X86.op_count != 4 || X86.operands[0].type != X86_OP_REG ||
         X86.operands[2].type != X86_OP_REG ||
         X86.operands[3].type != X86_OP_REG))
      return false;
    const unsigned LeftIndex = HasWriteMask ? 2 : 1;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[LeftIndex])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);

    bool IsDouble = (InsnId == X86_INS_MINSD || InsnId == X86_INS_MAXSD ||
                     InsnId == X86_INS_MINPD || InsnId == X86_INS_MAXPD ||
                     InsnId == X86_INS_VMINSD || InsnId == X86_INS_VMAXSD ||
                     InsnId == X86_INS_VMINPD || InsnId == X86_INS_VMAXPD);

    unsigned LaneSz = IsDouble ? 8 : 4;
    unsigned NLanes = IsScalar ? 1 : (Dst.Size / LaneSz);

    NdVar Acc = S.makeTemp(0);
    for (unsigned I = 0; I < NLanes; ++I) {
      NdVar La = S.makeTemp(LaneSz);
      NdVar Lb = S.makeTemp(LaneSz);
      S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(I * LaneSz, 4)});
      S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(I * LaneSz, 4)});
      NdVar Cmp = S.makeTemp(1);
      if (IsMin)
        S.emit(NdOp::FLOAT_LESS, Cmp, {La, Lb});
      else
        S.emit(NdOp::FLOAT_LESS, Cmp, {Lb, La});
      NdVar Sel = S.makeTemp(LaneSz);
      S.emit(NdOp::SELECT, Sel, {Cmp, La, Lb});
      if (I == 0) {
        Acc = Sel;
      } else {
        NdVar Next = S.makeTemp(Acc.Size + LaneSz);
        S.emit(NdOp::CONCAT, Next, {Sel, Acc});
        Acc = Next;
      }
    }

    if (!IsScalar && HasWriteMask) {
      if (!emitMaskedVectorResult(L, S, X86.operands[0], X86.operands[1], Acc,
                                  LaneSz))
        return false;
    } else if (IsScalar && Dst.Size > LaneSz) {
      NdVar Upper = S.makeTemp(Dst.Size - LaneSz);
      S.emit(NdOp::SUBBYTES, Upper, {A, NdVar::cst(LaneSz, 4)});
      NdVar Full = S.makeTemp(Dst.Size);
      S.emit(NdOp::CONCAT, Full, {Upper, Acc});
      S.emit(NdOp::COPY, Dst, {Full});
    } else if (Acc.Size < Dst.Size) {
      NdVar Wide = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_ZEXT, Wide, {Acc});
      S.emit(NdOp::COPY, Dst, {Wide});
    } else {
      S.emit(NdOp::COPY, Dst, {Acc});
    }
    break;
  }

  // AVX float arithmetic
  case X86_INS_VADDSS:
  case X86_INS_VADDSD:
  case X86_INS_VADDPS:
  case X86_INS_VADDPD:
  case X86_INS_VSUBSS:
  case X86_INS_VSUBSD:
  case X86_INS_VSUBPS:
  case X86_INS_VSUBPD:
  case X86_INS_VMULSS:
  case X86_INS_VMULSD:
  case X86_INS_VMULPS:
  case X86_INS_VMULPD:
  case X86_INS_VDIVSS:
  case X86_INS_VDIVSD:
  case X86_INS_VDIVPS:
  case X86_INS_VDIVPD: {
    if (X86.op_count < 2)
      break;
    NdOp Opc;
    switch (InsnId) {
    case X86_INS_VADDSS:
    case X86_INS_VADDSD:
    case X86_INS_VADDPS:
    case X86_INS_VADDPD:
      Opc = NdOp::FLOAT_ADD;
      break;
    case X86_INS_VSUBSS:
    case X86_INS_VSUBSD:
    case X86_INS_VSUBPS:
    case X86_INS_VSUBPD:
      Opc = NdOp::FLOAT_SUB;
      break;
    case X86_INS_VMULSS:
    case X86_INS_VMULSD:
    case X86_INS_VMULPS:
    case X86_INS_VMULPD:
      Opc = NdOp::FLOAT_MULT;
      break;
    default:
      Opc = NdOp::FLOAT_DIV;
    }
    bool IsPacked = (InsnId == X86_INS_VADDPS || InsnId == X86_INS_VADDPD ||
                     InsnId == X86_INS_VSUBPS || InsnId == X86_INS_VSUBPD ||
                     InsnId == X86_INS_VMULPS || InsnId == X86_INS_VMULPD ||
                     InsnId == X86_INS_VDIVPS || InsnId == X86_INS_VDIVPD);
    const bool IsSS = InsnId == X86_INS_VADDSS || InsnId == X86_INS_VSUBSS ||
                      InsnId == X86_INS_VMULSS || InsnId == X86_INS_VDIVSS;
    const bool IsSD = InsnId == X86_INS_VADDSD || InsnId == X86_INS_VSUBSD ||
                      InsnId == X86_INS_VMULSD || InsnId == X86_INS_VDIVSD;
    const bool IsScalar = IsSS || IsSD;
    const bool HasWriteMask =
        X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
    if (HasWriteMask &&
        ((!IsPacked && !IsScalar) || X86.op_count != 4 ||
         X86.operands[0].type != X86_OP_REG ||
         X86.operands[2].type != X86_OP_REG ||
         (X86.operands[3].type != X86_OP_REG &&
          (!IsEvexFloatArith || X86.operands[3].type != X86_OP_MEM))))
      return false;
    RegInfo ScalarMaskInfo{0, 0};
    if (HasWriteMask && IsScalar) {
      ScalarMaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
      if (ScalarMaskInfo.Size < 1)
        return false;
    }

    const unsigned LeftIndex = HasWriteMask ? 2 : 1;
    const unsigned RightIndex = HasWriteMask ? 3 : 2;
    if (IsEvexFloatArith && ((!HasWriteMask && X86.op_count != 3) ||
                             (HasWriteMask && X86.op_count != 4) ||
                             X86.operands[0].type != X86_OP_REG ||
                             X86.operands[LeftIndex].type != X86_OP_REG ||
                             (X86.operands[RightIndex].type != X86_OP_REG &&
                              X86.operands[RightIndex].type != X86_OP_MEM)))
      return false;

    CanonicalEvexEncodingInfo EvexEncoding;
    bool EvexBroadcast = false;
    bool EvexSuppressExceptions = false;
    X86FPRounding EvexRounding = X86FPRounding::MXCSR;
    const cs_x86_op *MaskOperand = HasWriteMask ? &X86.operands[1] : nullptr;
    if (IsEvexFloatArith &&
        !validateEvexFloatArith(
            L, Insn, X86, EvexFloatSpec, HasWriteMask, X86.operands[0],
            MaskOperand, X86.operands[LeftIndex], X86.operands[RightIndex],
            EvexEncoding, EvexBroadcast, EvexRounding, EvexSuppressExceptions))
      return false;

    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[LeftIndex])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar ActiveMask = NdVar::cst(UINT64_MAX, 8);
    uint16_t RequiredMaskSize = 0;
    if (IsEvexFloatArith) {
      const unsigned LaneCount =
          EvexFloatSpec.Scalar ? 1 : Dst.Size / EvexFloatSpec.ElementSize;
      RequiredMaskSize =
          static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
      const uint64_t RelevantMask =
          LaneCount == 64 ? UINT64_MAX
                          : (UINT64_C(1) << LaneCount) - UINT64_C(1);
      ActiveMask = NdVar::cst(RelevantMask, RequiredMaskSize);
      if (HasWriteMask) {
        const RegInfo MaskInfo =
            mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
        if (MaskInfo.Offset == UINT64_C(0xffff) ||
            MaskInfo.Size < RequiredMaskSize)
          return false;
        ActiveMask = NdVar::reg(MaskInfo.Offset, RequiredMaskSize);
      }
    }
    NdVar B;
    if (IsEvexFloatArith && X86.operands[RightIndex].type == X86_OP_MEM) {
      NdVar LoadMask = ActiveMask;
      if (EvexFloatSpec.Scalar && HasWriteMask) {
        LoadMask = S.makeTemp(ActiveMask.Size);
        S.emit(NdOp::INT_AND, LoadMask,
               {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
      }
      B = emitEvexMaskedMemoryLoad(S, X86.operands[RightIndex], LoadMask,
                                   Dst.Size, EvexFloatSpec.ElementSize,
                                   EvexFloatSpec.Scalar || EvexBroadcast
                                       ? EvexFloatSpec.ElementSize
                                       : Dst.Size,
                                   EvexBroadcast);
      if (B.Size != Dst.Size)
        return false;
    } else {
      B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    }
    if (IsEvexFloatArith) {
      if (A.Size != Dst.Size || B.Size != Dst.Size)
        return false;
      const uint16_t Control = makeX86FPArithControl(
          EvexFloatSpec.Kind, EvexFloatSpec.ElementSize == 8,
          EvexFloatSpec.Scalar, EvexSuppressExceptions, EvexRounding);
      NdVar Raw = S.makeTemp(Dst.Size);
      S.emitIntrinsic(Intrinsic::X86FPArith, Raw,
                      {NdVar::cst(Control, 2), A, B, A, ActiveMask});

      if (!EvexFloatSpec.Scalar) {
        if (HasWriteMask)
          return emitMaskedVectorResult(L, S, X86.operands[0], X86.operands[1],
                                        Raw, EvexFloatSpec.ElementSize);
        S.emit(NdOp::COPY, Dst, {Raw});
        break;
      }

      NdVar NewLow = S.makeTemp(EvexFloatSpec.ElementSize);
      S.emit(NdOp::SUBBYTES, NewLow, {Raw, NdVar::cst(0, 4)});
      NdVar Low = NewLow;
      if (HasWriteMask) {
        NdVar MaskBit = S.makeTemp(1);
        S.emit(NdOp::INT_AND, MaskBit,
               {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
        NdVar Inactive = NdVar::cst(0, EvexFloatSpec.ElementSize);
        if (!X86.operands[1].avx_zero_opmask) {
          const NdVar OldDestination = L.operandRead(S, X86.operands[0]);
          Inactive = S.makeTemp(EvexFloatSpec.ElementSize);
          S.emit(NdOp::SUBBYTES, Inactive, {OldDestination, NdVar::cst(0, 4)});
        }
        Low = S.makeTemp(EvexFloatSpec.ElementSize);
        S.emit(NdOp::SELECT, Low, {MaskBit, NewLow, Inactive});
      }
      NdVar Upper = S.makeTemp(Dst.Size - EvexFloatSpec.ElementSize);
      S.emit(NdOp::SUBBYTES, Upper,
             {A, NdVar::cst(EvexFloatSpec.ElementSize, 4)});
      S.emit(NdOp::CONCAT, Dst, {Upper, Low});
      break;
    }
    if (IsPacked && Dst.Size >= 16) {
      bool IsPD = (InsnId == X86_INS_VADDPD || InsnId == X86_INS_VSUBPD ||
                   InsnId == X86_INS_VMULPD || InsnId == X86_INS_VDIVPD);
      unsigned ElemSz = IsPD ? 8 : 4;
      unsigned NLanes = Dst.Size / ElemSz;
      std::vector<NdVar> Lanes(NLanes);
      for (unsigned I = 0; I < NLanes; ++I) {
        NdVar LA = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, LA, {A, NdVar::cst(I * ElemSz, 4)});
        NdVar LB = S.makeTemp(ElemSz);
        S.emit(NdOp::SUBBYTES, LB, {B, NdVar::cst(I * ElemSz, 4)});
        Lanes[I] = S.makeTemp(ElemSz);
        S.emit(Opc, Lanes[I], {LA, LB});
      }
      NdVar Result = Lanes.front();
      for (unsigned I = 1; I < NLanes; ++I) {
        NdVar Next = S.makeTemp(Result.Size + ElemSz);
        S.emit(NdOp::CONCAT, Next, {Lanes[I], Result});
        Result = Next;
      }
      if (HasWriteMask) {
        if (!emitMaskedVectorResult(L, S, X86.operands[0], X86.operands[1],
                                    Result, ElemSz))
          return false;
      } else {
        S.emit(NdOp::COPY, Dst, {Result});
      }
    } else {
      if ((IsSS || IsSD) && Dst.Size > 8) {
        unsigned ScalarSz = IsSS ? 4 : 8;
        NdVar SA = S.makeTemp(ScalarSz);
        S.emit(NdOp::SUBBYTES, SA, {A, NdVar::cst(0, 4)});
        NdVar SB = S.makeTemp(ScalarSz);
        S.emit(NdOp::SUBBYTES, SB, {B, NdVar::cst(0, 4)});
        NdVar Res = S.makeTemp(ScalarSz);
        S.emit(Opc, Res, {SA, SB});

        NdVar Low = Res;
        if (HasWriteMask) {
          const NdVar Mask = NdVar::reg(ScalarMaskInfo.Offset, 1);
          NdVar MaskBit = S.makeTemp(1);
          S.emit(NdOp::INT_AND, MaskBit, {Mask, NdVar::cst(1, 1)});
          NdVar Inactive = NdVar::cst(0, ScalarSz);
          if (!X86.operands[1].avx_zero_opmask) {
            NdVar OldDestination = L.operandRead(S, X86.operands[0]);
            Inactive = S.makeTemp(ScalarSz);
            S.emit(NdOp::SUBBYTES, Inactive,
                   {OldDestination, NdVar::cst(0, 4)});
          }
          Low = S.makeTemp(ScalarSz);
          S.emit(NdOp::SELECT, Low, {MaskBit, Res, Inactive});
        }

        unsigned HiSz = Dst.Size - ScalarSz;
        NdVar Hi = S.makeTemp(HiSz);
        S.emit(NdOp::SUBBYTES, Hi, {A, NdVar::cst(ScalarSz, 4)});
        S.emit(NdOp::CONCAT, Dst, {Hi, Low});
      } else {
        S.emit(Opc, Dst, {A, B});
      }
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
