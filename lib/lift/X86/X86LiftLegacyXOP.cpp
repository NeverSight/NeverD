//===- X86LiftLegacyXOP.cpp - AMD XOP and AVX-512 leftover lifter ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AMD XOP: VPCOM* compares, VPCMOV/VPPERM/VPERMIL2*,
/// horizontal VPHADD*/VPHSUB*, VPMACS* multiply-accumulate,
/// VPROT*/VPSHA*/VPSHL* and VFRCZ*.  Also the AVX-512
/// VPANDN, align/shuffle, VPCMPxSTRx, VTESTPD/PS and
/// VP4DPWSSD handlers that share this dispatcher.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include <algorithm>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

bool isZmm64(const cs_x86_op &Operand) {
  return Operand.type == X86_OP_REG && Operand.size == 64 &&
         Operand.reg >= X86_REG_ZMM0 && Operand.reg <= X86_REG_ZMM31;
}

unsigned zmmIndex(const cs_x86_op &Operand) {
  return static_cast<unsigned>(Operand.reg - X86_REG_ZMM0);
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

} // namespace

bool liftLegacyXOP(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // AVX-512 VPANDN — ~Dst & Src (bulk 128/256/512b).
  // ========================================================================
  case X86_INS_VPANDN: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    NdVar Inv = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, Inv, {A});
    S.emit(NdOp::INT_AND, Dst, {Inv, B});
    break;
  }

  // ========================================================================
  // AVX-512 VALIGND/Q — concatenate src1 above src2, shift right by an
  // immediate element count, and keep the low vector width.  The count wraps
  // to the number of elements in one destination vector.
  // ========================================================================
  case X86_INS_VALIGND:
  case X86_INS_VALIGNQ: {
    const uint16_t ElementSize = InsnId == X86_INS_VALIGND ? 4 : 8;
    CanonicalEvexEncodingInfo Encoding;
    if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
        Encoding.Offset + 7 > Insn->size || (Encoding.P0 & 0x07) != 0x03 ||
        (Encoding.P1 & 0x87) !=
            static_cast<uint8_t>((ElementSize == 8 ? 0x80 : 0) | 0x05) ||
        Encoding.Opcode != 0x03 || (Encoding.P2 & 0x60) == 0x60 ||
        (Encoding.P2 & 0x10) != 0 || X86.encoding.imm_size != 1 ||
        X86.encoding.imm_offset != Insn->size - 1 || X86.avx_sae ||
        X86.avx_rm != X86_AVX_RM_INVALID)
      return false;

    const bool HasMask = X86.op_count == 5;
    if ((!HasMask && X86.op_count != 4) ||
        (HasMask && !isX86OpmaskOperand(X86.operands[1])))
      return false;
    const unsigned Source1Index = HasMask ? 2 : 1;
    const unsigned Source2Index = HasMask ? 3 : 2;
    const unsigned ImmediateIndex = HasMask ? 4 : 3;
    const cs_x86_op &DestinationOperand = X86.operands[0];
    const cs_x86_op &Source1Operand = X86.operands[Source1Index];
    const cs_x86_op &Source2Operand = X86.operands[Source2Index];
    const cs_x86_op &ImmediateOperand = X86.operands[ImmediateIndex];

    const uint8_t EncodedLength = Encoding.P2 & 0x60;
    const uint16_t VectorSize =
        EncodedLength == 0 ? 16 : (EncodedLength == 0x20 ? 32 : 64);
    if (!isVectorRegisterOfSize(DestinationOperand, VectorSize) ||
        !isVectorRegisterOfSize(Source1Operand, VectorSize) ||
        (Source2Operand.type != X86_OP_REG &&
         Source2Operand.type != X86_OP_MEM) ||
        ImmediateOperand.type != X86_OP_IMM || ImmediateOperand.size != 1 ||
        static_cast<uint8_t>(ImmediateOperand.imm) !=
            Insn->bytes[Insn->size - 1] ||
        decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
            vectorRegisterIndex(DestinationOperand) ||
        decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
            vectorRegisterIndex(Source1Operand))
      return false;

    const bool MemoryForm = (Encoding.ModRM & 0xc0) != 0xc0;
    if (MemoryForm != (Source2Operand.type == X86_OP_MEM) ||
        (MemoryForm &&
         (HasMask || Source2Operand.size != VectorSize ||
          !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, Source2Operand,
                                           VectorSize, 1))) ||
        (!MemoryForm &&
         (!isVectorRegisterOfSize(Source2Operand, VectorSize) ||
          decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
              vectorRegisterIndex(Source2Operand) ||
          !validateCanonicalEvexRegisterTail(Insn, X86, Encoding, 1))))
      return false;

    if (L.targetArch() == Arch::X86 &&
        (vectorRegisterIndex(DestinationOperand) >= 8 ||
         vectorRegisterIndex(Source1Operand) >= 8 ||
         (!MemoryForm && vectorRegisterIndex(Source2Operand) >= 8)))
      return false;

    const unsigned LaneCount = VectorSize / ElementSize;
    const uint16_t MaskSize =
        static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
    const uint8_t EncodedMask = Encoding.P2 & 0x07;
    if (HasMask) {
      const cs_x86_op &MaskOperand = X86.operands[1];
      const RegInfo MaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
      if (EncodedMask == 0 || MaskOperand.reg == X86_REG_K0 ||
          EncodedMask != MaskOperand.reg - X86_REG_K0 ||
          MaskOperand.size != MaskSize || MaskInfo.Offset == UINT64_C(0xffff) ||
          MaskInfo.Size < MaskSize ||
          (((Encoding.P2 & 0x80) != 0) != MaskOperand.avx_zero_opmask))
        return false;
    } else if (EncodedMask != 0 || (Encoding.P2 & 0x80) != 0) {
      return false;
    }
    for (unsigned Index = 0; Index < X86.op_count; ++Index)
      if (X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID ||
          (X86.operands[Index].avx_zero_opmask && (!HasMask || Index != 1)))
        return false;

    NdVar Source1 = L.operandRead(S, Source1Operand);
    NdVar Source2 = L.operandRead(S, Source2Operand);

    const unsigned Shift =
        static_cast<uint8_t>(ImmediateOperand.imm) & (LaneCount - 1);
    NdVar Lanes[16];
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      const unsigned SourceLane = Shift + Lane;
      if (SourceLane < LaneCount) {
        Lanes[Lane] = S.makeTemp(ElementSize);
        S.emit(NdOp::SUBBYTES, Lanes[Lane],
               {Source2, NdVar::cst(SourceLane * ElementSize, 4)});
      } else {
        Lanes[Lane] = S.makeTemp(ElementSize);
        S.emit(
            NdOp::SUBBYTES, Lanes[Lane],
            {Source1, NdVar::cst((SourceLane - LaneCount) * ElementSize, 4)});
      }
    }

    for (unsigned Count = LaneCount, Width = ElementSize; Count > 1;
         Count >>= 1, Width <<= 1) {
      for (unsigned Pair = 0; Pair < Count / 2; ++Pair) {
        NdVar Joined = S.makeTemp(Width * 2);
        S.emit(NdOp::CONCAT, Joined, {Lanes[Pair * 2 + 1], Lanes[Pair * 2]});
        Lanes[Pair] = Joined;
      }
    }
    if (HasMask)
      return emitMaskedVectorResult(L, S, DestinationOperand, X86.operands[1],
                                    Lanes[0], ElementSize);
    S.emit(NdOp::COPY, L.operandWrite(DestinationOperand), {Lanes[0]});
    return true;
  }

  // ========================================================================
  // AVX-512 VSHUFF32X4/VSHUFF64X2/VSHUFI32X4/VSHUFI64X2 — select 128-bit
  // quarters from two vector sources.
  // ========================================================================
  case X86_INS_VSHUFF32X4:
  case X86_INS_VSHUFF64X2:
  case X86_INS_VSHUFI32X4:
  case X86_INS_VSHUFI64X2: {
    const bool IsQword =
        InsnId == X86_INS_VSHUFF64X2 || InsnId == X86_INS_VSHUFI64X2;
    const uint8_t Opcode =
        InsnId == X86_INS_VSHUFF32X4 || InsnId == X86_INS_VSHUFF64X2 ? 0x23
                                                                     : 0x43;
    CanonicalEvexEncodingInfo Encoding;
    if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
        Encoding.Offset + 7 > Insn->size || (Encoding.P0 & 0x07) != 0x03 ||
        (Encoding.P1 & 0x87) !=
            static_cast<uint8_t>((IsQword ? 0x80 : 0) | 0x05) ||
        Encoding.Opcode != Opcode ||
        ((Encoding.P2 & 0x60) != 0x20 && (Encoding.P2 & 0x60) != 0x40) ||
        (Encoding.P2 & 0x10) != 0 || X86.encoding.imm_size != 1 ||
        X86.encoding.imm_offset != Insn->size - 1 || X86.avx_sae ||
        X86.avx_rm != X86_AVX_RM_INVALID)
      return false;

    const bool HasMask = X86.op_count == 5;
    if ((!HasMask && X86.op_count != 4) ||
        (HasMask && !isX86OpmaskOperand(X86.operands[1])))
      return false;
    const unsigned Source1Index = HasMask ? 2 : 1;
    const unsigned Source2Index = HasMask ? 3 : 2;
    const unsigned ImmediateIndex = HasMask ? 4 : 3;
    const uint16_t VectorSize = (Encoding.P2 & 0x60) == 0x20 ? 32 : 64;
    const uint16_t ElementSize = IsQword ? 8 : 4;
    const cs_x86_op &DestinationOperand = X86.operands[0];
    const cs_x86_op &Source1Operand = X86.operands[Source1Index];
    const cs_x86_op &Source2Operand = X86.operands[Source2Index];
    const cs_x86_op &ImmediateOperand = X86.operands[ImmediateIndex];
    if (!isVectorRegisterOfSize(DestinationOperand, VectorSize) ||
        !isVectorRegisterOfSize(Source1Operand, VectorSize) ||
        (Source2Operand.type != X86_OP_REG &&
         Source2Operand.type != X86_OP_MEM) ||
        ImmediateOperand.type != X86_OP_IMM || ImmediateOperand.size != 1 ||
        static_cast<uint8_t>(ImmediateOperand.imm) !=
            Insn->bytes[Insn->size - 1] ||
        decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
            vectorRegisterIndex(DestinationOperand) ||
        decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
            vectorRegisterIndex(Source1Operand))
      return false;

    const bool MemoryForm = (Encoding.ModRM & 0xc0) != 0xc0;
    if (MemoryForm != (Source2Operand.type == X86_OP_MEM) ||
        (MemoryForm &&
         (Source2Operand.size != VectorSize ||
          !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, Source2Operand,
                                           VectorSize, 1))) ||
        (!MemoryForm &&
         (!isVectorRegisterOfSize(Source2Operand, VectorSize) ||
          decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
              vectorRegisterIndex(Source2Operand) ||
          !validateCanonicalEvexRegisterTail(Insn, X86, Encoding, 1))))
      return false;

    if (L.targetArch() == Arch::X86 &&
        (vectorRegisterIndex(DestinationOperand) >= 8 ||
         vectorRegisterIndex(Source1Operand) >= 8 ||
         (!MemoryForm && vectorRegisterIndex(Source2Operand) >= 8)))
      return false;

    const unsigned LaneCount = VectorSize / ElementSize;
    const uint16_t MaskSize =
        static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
    const uint8_t EncodedMask = Encoding.P2 & 0x07;
    if (HasMask) {
      const cs_x86_op &MaskOperand = X86.operands[1];
      const RegInfo MaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
      if (EncodedMask == 0 || MaskOperand.reg == X86_REG_K0 ||
          EncodedMask != MaskOperand.reg - X86_REG_K0 ||
          MaskOperand.size != 2 || MaskInfo.Offset == UINT64_C(0xffff) ||
          MaskInfo.Size < MaskSize ||
          (((Encoding.P2 & 0x80) != 0) != MaskOperand.avx_zero_opmask))
        return false;
    } else if (EncodedMask != 0 || (Encoding.P2 & 0x80) != 0) {
      return false;
    }
    for (unsigned Index = 0; Index < X86.op_count; ++Index)
      if (X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID ||
          (X86.operands[Index].avx_zero_opmask && (!HasMask || Index != 1)))
        return false;

    NdVar Source1 = L.operandRead(S, Source1Operand);
    NdVar Source2 = L.operandRead(S, Source2Operand);
    const unsigned QuarterCount = VectorSize / 16;
    const unsigned SelectorBits = VectorSize == 32 ? 1 : 2;
    const uint8_t Immediate = static_cast<uint8_t>(ImmediateOperand.imm);
    NdVar Quarters[4];
    for (unsigned Quarter = 0; Quarter < QuarterCount; ++Quarter) {
      const unsigned Selector =
          (Immediate >> (Quarter * SelectorBits)) & (QuarterCount - 1);
      Quarters[Quarter] = S.makeTemp(16);
      S.emit(NdOp::SUBBYTES, Quarters[Quarter],
             {Quarter < QuarterCount / 2 ? Source1 : Source2,
              NdVar::cst(Selector * 16, 4)});
    }
    for (unsigned Count = QuarterCount, Width = 16; Count > 1;
         Count >>= 1, Width <<= 1) {
      for (unsigned Pair = 0; Pair < Count / 2; ++Pair) {
        NdVar Joined = S.makeTemp(Width * 2);
        S.emit(NdOp::CONCAT, Joined,
               {Quarters[Pair * 2 + 1], Quarters[Pair * 2]});
        Quarters[Pair] = Joined;
      }
    }
    if (HasMask)
      return emitMaskedVectorResult(L, S, DestinationOperand, X86.operands[1],
                                    Quarters[0], ElementSize);
    S.emit(NdOp::COPY, L.operandWrite(DestinationOperand), {Quarters[0]});
    return true;
  }

  // ========================================================================
  // AVX VPCMPESTRI/M, VPCMPISTRI/M → intrinsic (sets RCX + EFLAGS).
  // ========================================================================
  case X86_INS_VPCMPESTRI:
    S.emitIntrinsic(Intrinsic::Pcmpestri);
    break;
  case X86_INS_VPCMPESTRM:
    S.emitIntrinsic(Intrinsic::Pcmpestrm);
    break;
  case X86_INS_VPCMPISTRI:
    S.emitIntrinsic(Intrinsic::Pcmpistri);
    break;
  case X86_INS_VPCMPISTRM:
    S.emitIntrinsic(Intrinsic::Pcmpistrm);
    break;

  // ========================================================================
  // AVX-512 VTESTPD/PS — set ZF/CF from AND/ANDN of packed floats.
  // ========================================================================
  case X86_INS_VTESTPD:
  case X86_INS_VTESTPS: {
    if (X86.op_count < 2)
      break;
    NdVar A = L.operandRead(S, X86.operands[0]);
    NdVar B = L.operandRead(S, X86.operands[1]);
    NdVar AndR = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, AndR, {A, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::ZF, 1),
           {AndR, NdVar::cst(0, AndR.Size)});
    NdVar InvA = S.makeTemp(A.Size);
    S.emit(NdOp::INT_NOT, InvA, {A});
    NdVar AndnR = S.makeTemp(A.Size);
    S.emit(NdOp::INT_AND, AndnR, {InvA, B});
    S.emit(NdOp::INT_EQUAL, NdVar::reg(x86reg::CF, 1),
           {AndnR, NdVar::cst(0, AndnR.Size)});
    break;
  }

  // ========================================================================
  // AVX-512 VP4DPWSSD / VP4DPWSSDS — exact four-register dot product.  The
  // memory operand is m128 (Tuple1_4X), while EVEX.vvvv names the first
  // member of a four-ZMM block.  Keep the entire effect in one intrinsic so
  // a fault cannot expose a partial memory read or destination update.
  // ========================================================================
  case X86_INS_VP4DPWSSD:
  case X86_INS_VP4DPWSSDS: {
    CanonicalEvexEncodingInfo Encoding;
    if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
        X86.op_count < 3 || X86.op_count > 4 || X86.encoding.imm_offset != 0 ||
        X86.encoding.imm_size != 0 || (Encoding.P0 & 0x07) != 2 ||
        ((Encoding.P1 | 0x04) & 0x87) != 0x07 || (Encoding.P2 & 0x60) != 0x40 ||
        (Encoding.P2 & 0x10) != 0 ||
        Encoding.Opcode !=
            static_cast<uint8_t>(InsnId == X86_INS_VP4DPWSSD ? 0x52 : 0x53) ||
        (Encoding.ModRM & 0xc0) == 0xc0 || X86.avx_sae ||
        X86.avx_rm != X86_AVX_RM_INVALID)
      break;

    const bool HasMask = X86.op_count == 4;
    const unsigned SourceIndex = HasMask ? 2 : 1;
    const unsigned MemoryIndex = HasMask ? 3 : 2;
    const cs_x86_op &Destination = X86.operands[0];
    const cs_x86_op &Source = X86.operands[SourceIndex];
    const cs_x86_op &Memory = X86.operands[MemoryIndex];
    if (!isZmm64(Destination) || !isZmm64(Source) ||
        Memory.type != X86_OP_MEM || Memory.size != 16 ||
        decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
            zmmIndex(Destination) ||
        decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
            zmmIndex(Source) ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, Memory, 16))
      break;

    for (unsigned I = 0; I < X86.op_count; ++I)
      if (X86.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
          (X86.operands[I].avx_zero_opmask && (!HasMask || I != 1)))
        return false;

    const unsigned SourceBase = zmmIndex(Source) & ~3u;
    if (SourceBase + 3 >= 32 ||
        (L.targetArch() == Arch::X86 &&
         (zmmIndex(Destination) >= 8 || SourceBase + 3 >= 8)))
      break;
    for (unsigned I = 0; I < 4; ++I) {
      const RegInfo Info =
          mapCapstoneReg(static_cast<x86_reg>(X86_REG_ZMM0 + SourceBase + I));
      if (Info.Offset == UINT64_C(0xffff) || Info.Size < 64)
        return false;
    }

    const uint8_t EncodedMask = Encoding.P2 & 7;
    const bool ZeroMask = (Encoding.P2 & 0x80) != 0;
    NdVar Mask = NdVar::cst(UINT64_C(0xffff), 2);
    if (HasMask) {
      const cs_x86_op &MaskOperand = X86.operands[1];
      const RegInfo MaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
      if (!isX86OpmaskOperand(MaskOperand) || MaskOperand.reg == X86_REG_K0 ||
          MaskOperand.size != 2 ||
          EncodedMask != static_cast<unsigned>(MaskOperand.reg - X86_REG_K0) ||
          MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < 2 ||
          static_cast<bool>(MaskOperand.avx_zero_opmask) != ZeroMask)
        break;
      Mask = NdVar::reg(MaskInfo.Offset, 2);
    } else if (EncodedMask != 0 || ZeroMask) {
      break;
    }

    const Intrinsic Id = InsnId == X86_INS_VP4DPWSSD ? Intrinsic::X86VP4DPWSSD
                                                     : Intrinsic::X86VP4DPWSSDS;
    const NdVar OldDestination = L.operandRead(S, Destination);
    if (!S.emitMemoryIntrinsic(Id, Memory,
                               {OldDestination, NdVar::cst(SourceBase, 1), Mask,
                                NdVar::cst(ZeroMask ? 1 : 0, 1)},
                               L.operandWrite(Destination)))
      break;
    break;
  }

  // ========================================================================
  // AMD XOP: VPCOM* (integer compare) — Result is all-1s or all-0s Mask.
  // ========================================================================
  case X86_INS_VPCOM:
  case X86_INS_VPCOMB:
  case X86_INS_VPCOMD:
  case X86_INS_VPCOMQ:
  case X86_INS_VPCOMUB:
  case X86_INS_VPCOMUD:
  case X86_INS_VPCOMUQ:
  case X86_INS_VPCOMUW:
  case X86_INS_VPCOMW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_EQUAL, Dst, {A, B});
    break;
  }

  // ========================================================================
  // AMD XOP: VPCMOV — conditional move; VPERMIL2PD/PS — permute.
  // ========================================================================
  case X86_INS_VPCMOV:
  case X86_INS_VPERMIL2PD:
  case X86_INS_VPERMIL2PS:
  case X86_INS_VPPERM: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  // ========================================================================
  // AMD XOP: VPHADD* / VPHSUB* — horizontal add/sub variants.
  // ========================================================================
  case X86_INS_VPHADDBD:
  case X86_INS_VPHADDBQ:
  case X86_INS_VPHADDBW:
  case X86_INS_VPHADDDQ:
  case X86_INS_VPHADDUBD:
  case X86_INS_VPHADDUBQ:
  case X86_INS_VPHADDUBW:
  case X86_INS_VPHADDUDQ:
  case X86_INS_VPHADDUWD:
  case X86_INS_VPHADDUWQ:
  case X86_INS_VPHADDWD:
  case X86_INS_VPHADDWQ:
  case X86_INS_VPHSUBBW:
  case X86_INS_VPHSUBDQ:
  case X86_INS_VPHSUBWD: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::INT_ADD, Dst, {Dst, Src});
    break;
  }

  // ========================================================================
  // AMD XOP: VPMACS* / VPMADCS* — multiply-accumulate.
  // ========================================================================
  case X86_INS_VPMACSDD:
  case X86_INS_VPMACSDQH:
  case X86_INS_VPMACSDQL:
  case X86_INS_VPMACSSDD:
  case X86_INS_VPMACSSDQH:
  case X86_INS_VPMACSSDQL:
  case X86_INS_VPMACSSWD:
  case X86_INS_VPMACSSWW:
  case X86_INS_VPMACSWD:
  case X86_INS_VPMACSWW:
  case X86_INS_VPMADCSSWD:
  case X86_INS_VPMADCSWD: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    NdVar Prod = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_MULT, Prod, {A, B});
    S.emit(NdOp::INT_ADD, Dst, {Dst, Prod});
    break;
  }

  // ========================================================================
  // AMD XOP: VPROT* — packed rotate; VPSHA/VPSHL* — packed shift.
  // ========================================================================
  case X86_INS_VPROTB:
  case X86_INS_VPROTD:
  case X86_INS_VPROTQ:
  case X86_INS_VPROTW:
  case X86_INS_VPSHAB:
  case X86_INS_VPSHAD:
  case X86_INS_VPSHAQ:
  case X86_INS_VPSHAW:
  case X86_INS_VPSHLB:
  case X86_INS_VPSHLD:
  case X86_INS_VPSHLQ:
  case X86_INS_VPSHLW: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    S.emit(NdOp::INT_LEFT, Dst, {A, B});
    break;
  }

  // ========================================================================
  // AMD XOP: VFRCZ* — approximate reciprocal.
  // ========================================================================
  case X86_INS_VFRCZPD:
  case X86_INS_VFRCZPS:
  case X86_INS_VFRCZSD:
  case X86_INS_VFRCZSS: {
    if (X86.op_count < 2)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src = L.operandRead(S, X86.operands[1]);
    S.emit(NdOp::COPY, Dst, {Src});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
