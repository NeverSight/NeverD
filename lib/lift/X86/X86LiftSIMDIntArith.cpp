//===- X86LiftSIMDIntArith.cpp - x86/x64 SIMD integer add/sub/min/max lifter
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SSE/AVX packed integer arithmetic: wrapping and saturating
/// add/subtract, rounded average, and signed/unsigned
/// minimum/maximum.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

struct EvexWrappingArithSpec {
  uint8_t Opcode = 0;
  uint16_t ElementSize = 0;
  bool W = false;
};

bool getEvexWrappingArithSpec(unsigned InsnId, EvexWrappingArithSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPADDB:
    Spec = {0xfc, 1, false};
    return true;
  case X86_INS_VPADDW:
    Spec = {0xfd, 2, false};
    return true;
  case X86_INS_VPADDD:
    Spec = {0xfe, 4, false};
    return true;
  case X86_INS_VPADDQ:
    Spec = {0xd4, 8, true};
    return true;
  case X86_INS_VPSUBB:
    Spec = {0xf8, 1, false};
    return true;
  case X86_INS_VPSUBW:
    Spec = {0xf9, 2, false};
    return true;
  case X86_INS_VPSUBD:
    Spec = {0xfa, 4, false};
    return true;
  case X86_INS_VPSUBQ:
    Spec = {0xfb, 8, true};
    return true;
  default:
    return false;
  }
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

bool validateEvexWrappingArithMemory(
    X86Lifter &L, const cs_insn *Insn, const cs_x86 &X86,
    const EvexWrappingArithSpec &Spec, bool HasWriteMask,
    const cs_x86_op &Destination, const cs_x86_op *Mask, const cs_x86_op &Left,
    const cs_x86_op &Memory, CanonicalEvexEncodingInfo &Encoding,
    bool &Broadcast) {
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x01 ||
      ((Encoding.P1 | 0x04) & 0x87) != (Spec.W ? 0x85 : 0x05) ||
      Encoding.Opcode != Spec.Opcode || (Encoding.ModRM & 0xc0) == 0xc0 ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  if (EncodedLength == 0x60)
    return false;
  const uint16_t VectorSize = EncodedLength == 0      ? 16
                              : EncodedLength == 0x20 ? 32
                                                      : 64;
  Broadcast = (Encoding.P2 & 0x10) != 0;
  if ((Broadcast && Spec.ElementSize < 4) ||
      !isVectorRegisterOfSize(Destination, VectorSize) ||
      !isVectorRegisterOfSize(Left, VectorSize) || Memory.type != X86_OP_MEM ||
      Memory.size != (Broadcast ? Spec.ElementSize : VectorSize) ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
          vectorRegisterIndex(Left))
    return false;

  const unsigned LaneCount = VectorSize / Spec.ElementSize;
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

  const x86_avx_bcast ExpectedBroadcast =
      Broadcast ? broadcastForLaneCount(LaneCount) : X86_AVX_BCAST_INVALID;
  if (Memory.avx_bcast != ExpectedBroadcast)
    return false;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    if (&Operand != &Memory && Operand.avx_bcast != X86_AVX_BCAST_INVALID)
      return false;
    if (Operand.avx_zero_opmask && (!Mask || &Operand != Mask))
      return false;
  }
  return validateCanonicalEvexMemoryTail(
      Insn, X86, Encoding, Memory, Broadcast ? Spec.ElementSize : VectorSize);
}

} // namespace

bool liftSIMDIntArith(X86Lifter &L, X86Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  EvexWrappingArithSpec WrappingSpec;
  const bool IsWrappingArith = getEvexWrappingArithSpec(InsnId, WrappingSpec);
  if (hasUnsupportedEvexValueModifier(X86) && !IsWrappingArith)
    return false;
  switch (InsnId) {

  // SIMD saturating add/sub — per-lane with saturation.
  // Uses LLVM saturating intrinsics (@llvm.{s,u}{add,sub}.sat) to avoid
  // a constant-folding bug in the fork's optimizer that mis-computes manual
  // sext→add→icmp→select→trunc clamp chains for signed saturation.
  case X86_INS_PADDSB:
  case X86_INS_PADDSW:
  case X86_INS_PADDUSB:
  case X86_INS_PADDUSW:
  case X86_INS_PSUBSB:
  case X86_INS_PSUBSW:
  case X86_INS_PSUBUSB:
  case X86_INS_PSUBUSW: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar DstR = L.operandRead(S, X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    unsigned LaneSz = 1;
    bool IsSigned = false;
    bool IsSub = false;
    switch (InsnId) {
    case X86_INS_PADDSW:
    case X86_INS_PSUBSW:
    case X86_INS_PADDUSW:
    case X86_INS_PSUBUSW:
      LaneSz = 2;
      break;
    default:
      break;
    }
    switch (InsnId) {
    case X86_INS_PADDSB:
    case X86_INS_PADDSW:
    case X86_INS_PSUBSB:
    case X86_INS_PSUBSW:
      IsSigned = true;
      break;
    default:
      break;
    }
    switch (InsnId) {
    case X86_INS_PSUBSB:
    case X86_INS_PSUBSW:
    case X86_INS_PSUBUSB:
    case X86_INS_PSUBUSW:
      IsSub = true;
      break;
    default:
      break;
    }
    Intrinsic IC;
    if (IsSigned)
      IC = IsSub ? Intrinsic::X86_SsubSat : Intrinsic::X86_SaddSat;
    else
      IC = IsSub ? Intrinsic::X86_UsubSat : Intrinsic::X86_UaddSat;
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
        S.emitIntrinsic(IC, Lr, {La, Lb});
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

  // PAVG{B,W} — packed average bytes/words.
  case X86_INS_PAVGB:
  case X86_INS_PAVGW:
  case X86_INS_VPAVGB:
  case X86_INS_VPAVGW: {
    // Packed unsigned rounding average per lane: dst[i] = (a[i]+b[i]+1) >> 1.
    // The old code emitted a single full-width INT_ADD — no divide, no rounding
    // and not lane-isolated (carries crossed byte/word boundaries).
    if (X86.op_count < 2)
      break;
    const bool HasWriteMask =
        X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
    if (HasWriteMask &&
        (X86.op_count != 4 || X86.operands[0].type != X86_OP_REG ||
         X86.operands[2].type != X86_OP_REG ||
         X86.operands[3].type != X86_OP_REG))
      return false;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    const unsigned LeftIndex = HasWriteMask ? 2 : 1;
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[LeftIndex])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz =
        (InsnId == X86_INS_PAVGB || InsnId == X86_INS_VPAVGB) ? 1 : 2;
    unsigned WideSz = LaneSz * 2;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar Al = S.makeTemp(LaneSz), Bl = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Al, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Bl, {B, NdVar::cst(Off, 4)});
        NdVar Aw = S.makeTemp(WideSz), Bw = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ZEXT, Aw, {Al});
        S.emit(NdOp::INT_ZEXT, Bw, {Bl});
        NdVar Sum = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Sum, {Aw, Bw});
        NdVar Sum1 = S.makeTemp(WideSz);
        S.emit(NdOp::INT_ADD, Sum1, {Sum, NdVar::cst(1, WideSz)});
        NdVar Sh = S.makeTemp(WideSz);
        S.emit(NdOp::INT_RIGHT, Sh, {Sum1, NdVar::cst(1, WideSz)});
        NdVar Res = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Res, {Sh, NdVar::cst(0, 4)});
        if (I == 0) {
          Acc = Res;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Res, Acc});
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
      if (!emitMaskedVectorResult(L, S, X86.operands[0], X86.operands[1], Full,
                                  LaneSz))
        return false;
    } else {
      S.emit(NdOp::COPY, Dst, {Full});
    }
    break;
  }

  // Packed min/max — per-lane comparison + select.
  case X86_INS_PMINSB:
  case X86_INS_PMINSW:
  case X86_INS_PMINSD:
  case X86_INS_PMINUB:
  case X86_INS_PMINUW:
  case X86_INS_PMINUD:
  case X86_INS_PMAXSB:
  case X86_INS_PMAXSW:
  case X86_INS_PMAXSD:
  case X86_INS_PMAXUB:
  case X86_INS_PMAXUW:
  case X86_INS_PMAXUD:
  case X86_INS_VPMINSB:
  case X86_INS_VPMINSW:
  case X86_INS_VPMINSD:
  case X86_INS_VPMINSQ:
  case X86_INS_VPMINUB:
  case X86_INS_VPMINUW:
  case X86_INS_VPMINUD:
  case X86_INS_VPMINUQ:
  case X86_INS_VPMAXSB:
  case X86_INS_VPMAXSW:
  case X86_INS_VPMAXSD:
  case X86_INS_VPMAXSQ:
  case X86_INS_VPMAXUB:
  case X86_INS_VPMAXUW:
  case X86_INS_VPMAXUD:
  case X86_INS_VPMAXUQ: {
    if (X86.op_count < 2)
      break;
    const bool HasWriteMask =
        X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
    if (HasWriteMask &&
        (X86.op_count != 4 || X86.operands[0].type != X86_OP_REG ||
         X86.operands[2].type != X86_OP_REG ||
         X86.operands[3].type != X86_OP_REG))
      return false;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    bool IsVex = (X86.op_count >= 3);
    const unsigned LeftIndex = HasWriteMask ? 2 : 1;
    NdVar A = IsVex ? L.operandRead(S, X86.operands[LeftIndex])
                    : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 1;
    switch (InsnId) {
    case X86_INS_PMINSW:
    case X86_INS_PMAXSW:
    case X86_INS_PMINUW:
    case X86_INS_PMAXUW:
    case X86_INS_VPMINSW:
    case X86_INS_VPMAXSW:
    case X86_INS_VPMINUW:
    case X86_INS_VPMAXUW:
      LaneSz = 2;
      break;
    case X86_INS_PMINSD:
    case X86_INS_PMAXSD:
    case X86_INS_PMINUD:
    case X86_INS_PMAXUD:
    case X86_INS_VPMINSD:
    case X86_INS_VPMAXSD:
    case X86_INS_VPMINUD:
    case X86_INS_VPMAXUD:
      LaneSz = 4;
      break;
    case X86_INS_VPMINSQ:
    case X86_INS_VPMAXSQ:
    case X86_INS_VPMINUQ:
    case X86_INS_VPMAXUQ:
      LaneSz = 8;
      break;
    default:
      break;
    }
    bool IsSigned = false;
    switch (InsnId) {
    case X86_INS_PMINSB:
    case X86_INS_PMINSW:
    case X86_INS_PMINSD:
    case X86_INS_PMAXSB:
    case X86_INS_PMAXSW:
    case X86_INS_PMAXSD:
    case X86_INS_VPMINSB:
    case X86_INS_VPMINSW:
    case X86_INS_VPMINSD:
    case X86_INS_VPMINSQ:
    case X86_INS_VPMAXSB:
    case X86_INS_VPMAXSW:
    case X86_INS_VPMAXSD:
    case X86_INS_VPMAXSQ:
      IsSigned = true;
      break;
    default:
      break;
    }
    bool IsMax = false;
    switch (InsnId) {
    case X86_INS_PMAXSB:
    case X86_INS_PMAXSW:
    case X86_INS_PMAXSD:
    case X86_INS_PMAXUB:
    case X86_INS_PMAXUW:
    case X86_INS_PMAXUD:
    case X86_INS_VPMAXSB:
    case X86_INS_VPMAXSW:
    case X86_INS_VPMAXSD:
    case X86_INS_VPMAXSQ:
    case X86_INS_VPMAXUB:
    case X86_INS_VPMAXUW:
    case X86_INS_VPMAXUD:
    case X86_INS_VPMAXUQ:
      IsMax = true;
      break;
    default:
      break;
    }
    NdOp CmpOp = IsSigned ? NdOp::INT_SLESS : NdOp::INT_LESS;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Cond = S.makeTemp(1);
        S.emit(CmpOp, Cond, {La, Lb});
        NdVar Lr = S.makeTemp(LaneSz);
        if (IsMax)
          S.emit(NdOp::SELECT, Lr, {Cond, Lb, La});
        else
          S.emit(NdOp::SELECT, Lr, {Cond, La, Lb});
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
    if (HasWriteMask) {
      if (!emitMaskedVectorResult(L, S, X86.operands[0], X86.operands[1], Full,
                                  LaneSz))
        return false;
    } else {
      S.emit(NdOp::COPY, Dst, {Full});
    }
    break;
  }

  // AVX packed integer add/sub — per-lane decomposition.
  case X86_INS_VPADDB:
  case X86_INS_VPADDW:
  case X86_INS_VPADDD:
  case X86_INS_VPADDQ:
  case X86_INS_VPSUBB:
  case X86_INS_VPSUBW:
  case X86_INS_VPSUBD:
  case X86_INS_VPSUBQ: {
    if (X86.op_count < 2)
      break;
    const bool HasWriteMask =
        X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
    const unsigned LeftIndex = HasWriteMask ? 2 : 1;
    const unsigned RightIndex = HasWriteMask ? 3 : 2;
    if ((!HasWriteMask && X86.op_count != 3) ||
        (HasWriteMask && X86.op_count != 4) ||
        X86.operands[0].type != X86_OP_REG ||
        X86.operands[LeftIndex].type != X86_OP_REG ||
        (X86.operands[RightIndex].type != X86_OP_REG &&
         X86.operands[RightIndex].type != X86_OP_MEM))
      return false;
    const cs_x86_op &DestinationOperand = X86.operands[0];
    const cs_x86_op *MaskOperand = HasWriteMask ? &X86.operands[1] : nullptr;
    const cs_x86_op &LeftOperand = X86.operands[LeftIndex];
    const cs_x86_op &RightOperand = X86.operands[RightIndex];
    const bool MemoryForm = RightOperand.type == X86_OP_MEM;
    const bool RawEvex = beginsWithCanonicalEvexPrefix(Insn);
    CanonicalEvexEncodingInfo Encoding;
    bool Broadcast = false;
    if (RawEvex) {
      if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                          Encoding) ||
          ((Encoding.ModRM & 0xc0) != 0xc0) != MemoryForm)
        return false;
      if (MemoryForm &&
          !validateEvexWrappingArithMemory(
              L, Insn, X86, WrappingSpec, HasWriteMask, DestinationOperand,
              MaskOperand, LeftOperand, RightOperand, Encoding, Broadcast))
        return false;
      if (!MemoryForm && hasUnsupportedEvexValueModifier(X86))
        return false;
    } else if (hasUnsupportedEvexValueModifier(X86)) {
      return false;
    }

    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[LeftIndex])
                                  : L.operandRead(S, X86.operands[0]);
    unsigned LaneSz = 0;
    switch (InsnId) {
    case X86_INS_VPADDB:
    case X86_INS_VPSUBB:
      LaneSz = 1;
      break;
    case X86_INS_VPADDW:
    case X86_INS_VPSUBW:
      LaneSz = 2;
      break;
    case X86_INS_VPADDD:
    case X86_INS_VPSUBD:
      LaneSz = 4;
      break;
    case X86_INS_VPADDQ:
    case X86_INS_VPSUBQ:
      LaneSz = 8;
      break;
    default:
      break;
    }
    NdVar ActiveMask = NdVar::cst(UINT64_MAX, 8);
    if (HasWriteMask) {
      const RegInfo MaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
      const uint16_t RequiredMaskSize =
          static_cast<uint16_t>((Dst.Size / LaneSz + 7u) / 8u);
      if (MaskInfo.Offset == UINT64_C(0xffff) ||
          MaskInfo.Size < RequiredMaskSize)
        return false;
      ActiveMask = NdVar::reg(MaskInfo.Offset, RequiredMaskSize);
    }
    NdVar B = RawEvex && MemoryForm
                  ? emitEvexMaskedMemoryLoad(
                        S, RightOperand, ActiveMask, Dst.Size, LaneSz,
                        Broadcast ? LaneSz : Dst.Size, Broadcast)
                  : L.operandRead(S, RightOperand);
    if (Dst.Size == 0 || A.Size != Dst.Size || B.Size != Dst.Size)
      return false;
    // NOTE: even qword (LaneSz==8) lanes must be added/subtracted
    // independently — a full-width INT_ADD/INT_SUB would propagate the carry
    // from lane 0 into lane 1 (wrong for VPADDQ/VPSUBQ).
    bool IsSub = (InsnId == X86_INS_VPSUBB || InsnId == X86_INS_VPSUBW ||
                  InsnId == X86_INS_VPSUBD || InsnId == X86_INS_VPSUBQ);
    NdOp LaneOpc = IsSub ? NdOp::INT_SUB : NdOp::INT_ADD;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(LaneOpc, Lr, {La, Lb});
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
    if (HasWriteMask) {
      if (!emitMaskedVectorResult(L, S, X86.operands[0], X86.operands[1], Full,
                                  LaneSz))
        return false;
    } else {
      S.emit(NdOp::COPY, Dst, {Full});
    }
    break;
  }
  // AVX saturating add/sub — per-lane with saturation.
  case X86_INS_VPADDSB:
  case X86_INS_VPADDSW:
  case X86_INS_VPADDUSB:
  case X86_INS_VPADDUSW:
  case X86_INS_VPSUBSB:
  case X86_INS_VPSUBSW:
  case X86_INS_VPSUBUSB:
  case X86_INS_VPSUBUSW: {
    if (X86.op_count < 2)
      break;
    const bool HasWriteMask =
        X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
    if (HasWriteMask &&
        (X86.op_count != 4 || X86.operands[0].type != X86_OP_REG ||
         X86.operands[2].type != X86_OP_REG ||
         X86.operands[3].type != X86_OP_REG))
      return false;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    const unsigned LeftIndex = HasWriteMask ? 2 : 1;
    NdVar A = (X86.op_count >= 3) ? L.operandRead(S, X86.operands[LeftIndex])
                                  : L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[X86.op_count - 1]);
    unsigned LaneSz = 1;
    bool IsSigned = false, IsSub = false;
    switch (InsnId) {
    case X86_INS_VPADDSW:
    case X86_INS_VPSUBSW:
    case X86_INS_VPADDUSW:
    case X86_INS_VPSUBUSW:
      LaneSz = 2;
      break;
    default:
      break;
    }
    switch (InsnId) {
    case X86_INS_VPADDSB:
    case X86_INS_VPADDSW:
    case X86_INS_VPSUBSB:
    case X86_INS_VPSUBSW:
      IsSigned = true;
      break;
    default:
      break;
    }
    switch (InsnId) {
    case X86_INS_VPSUBSB:
    case X86_INS_VPSUBSW:
    case X86_INS_VPSUBUSB:
    case X86_INS_VPSUBUSW:
      IsSub = true;
      break;
    default:
      break;
    }
    NdOp ArithOp = IsSub ? NdOp::INT_SUB : NdOp::INT_ADD;
    unsigned WiderSz = LaneSz * 2;
    unsigned HalfSz = Dst.Size / 2;
    unsigned LanesPerHalf = HalfSz / LaneSz;
    auto BuildHalf = [&](unsigned BaseOff) -> NdVar {
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < LanesPerHalf; ++I) {
        unsigned Off = BaseOff + I * LaneSz;
        NdVar La = S.makeTemp(LaneSz), Lb = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(Off, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(Off, 4)});
        NdVar Ax = S.makeTemp(WiderSz), Bx = S.makeTemp(WiderSz);
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Ax, {La});
        S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, Bx, {Lb});
        NdVar Wide = S.makeTemp(WiderSz);
        S.emit(ArithOp, Wide, {Ax, Bx});
        int64_t MaxV, MinV;
        if (IsSigned) {
          MaxV = (1LL << (LaneSz * 8 - 1)) - 1;
          MinV = -(1LL << (LaneSz * 8 - 1));
        } else {
          MaxV = (1LL << (LaneSz * 8)) - 1;
          MinV = 0;
        }
        NdVar HiClamp = S.makeTemp(1);
        if (IsSigned)
          S.emit(NdOp::INT_SLESS, HiClamp, {NdVar::cst(MaxV, WiderSz), Wide});
        else
          S.emit(NdOp::INT_LESS, HiClamp, {NdVar::cst(MaxV, WiderSz), Wide});
        NdVar Clamped1 = S.makeTemp(WiderSz);
        S.emit(NdOp::SELECT, Clamped1,
               {HiClamp, NdVar::cst(MaxV, WiderSz), Wide});
        NdVar LoClamp = S.makeTemp(1);
        if (IsSigned)
          S.emit(NdOp::INT_SLESS, LoClamp,
                 {Clamped1, NdVar::cst(MinV, WiderSz)});
        else
          S.emit(NdOp::INT_SLESS, LoClamp, {Wide, NdVar::cst(0, WiderSz)});
        NdVar Clamped2 = S.makeTemp(WiderSz);
        S.emit(NdOp::SELECT, Clamped2,
               {LoClamp, NdVar::cst(MinV, WiderSz), Clamped1});
        NdVar Lr = S.makeTemp(LaneSz);
        S.emit(NdOp::SUBBYTES, Lr, {Clamped2, NdVar::cst(0, 4)});
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
    if (HasWriteMask) {
      if (!emitMaskedVectorResult(L, S, X86.operands[0], X86.operands[1], Full,
                                  LaneSz))
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
