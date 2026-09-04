//===- X86LiftSIMDMask.cpp - x86 EVEX writemask helpers ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include <algorithm>
#include <vector>

namespace neverd {

bool isX86OpmaskOperand(const cs_x86_op &Operand) {
  return Operand.type == X86_OP_REG && Operand.reg >= X86_REG_K0 &&
         Operand.reg <= X86_REG_K7;
}

bool hasUnsupportedEvexValueModifier(const cs_x86 &X86) {
  if (X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
    return true;
  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID)
      return true;
  return false;
}

unsigned decodeEvexVectorRegIndex(uint8_t P0, uint8_t ModRM) {
  return ((ModRM >> 3) & 0x07) | ((P0 & 0x80) == 0 ? 0x08 : 0) |
         ((P0 & 0x10) == 0 ? 0x10 : 0);
}

unsigned decodeEvexVectorRMIndex(uint8_t P0, uint8_t ModRM) {
  return (ModRM & 0x07) | ((P0 & 0x20) == 0 ? 0x08 : 0) |
         ((P0 & 0x40) == 0 ? 0x10 : 0);
}

unsigned decodeEvexVectorVvvvIndex(uint8_t P1, uint8_t P2) {
  return ((~static_cast<unsigned>(P1) >> 3) & 0x0f) |
         ((P2 & 0x08) == 0 ? 0x10 : 0);
}

NdVar expandCompactLaneMask(X86Lifter::LiftState &S, NdVar CompactMask,
                            uint16_t VectorSize, uint16_t ElementSize) {
  if (CompactMask.Size == 0 || ElementSize == 0 || ElementSize > 8 ||
      VectorSize == 0 || VectorSize % ElementSize != 0)
    return {};
  const unsigned LaneCount = VectorSize / ElementSize;
  if (LaneCount > static_cast<unsigned>(CompactMask.Size) * 8)
    return {};

  const uint64_t SignBit = UINT64_C(1) << (ElementSize * 8 - 1);
  NdVar Result;
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    NdVar Shifted = CompactMask;
    if (Lane != 0) {
      Shifted = S.makeTemp(CompactMask.Size);
      S.emit(NdOp::INT_RIGHT, Shifted,
             {CompactMask, NdVar::cst(Lane, CompactMask.Size)});
    }
    NdVar BitWide = S.makeTemp(CompactMask.Size);
    S.emit(NdOp::INT_AND, BitWide, {Shifted, NdVar::cst(1, CompactMask.Size)});
    NdVar Bit = BitWide;
    if (CompactMask.Size != 1) {
      Bit = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, Bit, {BitWide, NdVar::cst(0, 4)});
    }
    NdVar LaneMask = S.makeTemp(ElementSize);
    S.emit(NdOp::SELECT, LaneMask,
           {Bit, NdVar::cst(SignBit, ElementSize), NdVar::cst(0, ElementSize)});
    if (Lane == 0) {
      Result = LaneMask;
      continue;
    }
    NdVar Joined = S.makeTemp(Result.Size + ElementSize);
    S.emit(NdOp::CONCAT, Joined, {LaneMask, Result});
    Result = Joined;
  }
  return Result;
}

Intrinsic maskedVectorLoadIntrinsic(uint16_t ElementSize) {
  switch (ElementSize) {
  case 1:
    return Intrinsic::MaskedLoadB;
  case 2:
    return Intrinsic::MaskedLoadW;
  case 4:
    return Intrinsic::MaskedLoadD;
  case 8:
    return Intrinsic::MaskedLoadQ;
  default:
    return Intrinsic::None;
  }
}

bool emitMaskedVectorResult(X86Lifter &L, X86Lifter::LiftState &S,
                            const cs_x86_op &DestinationOperand,
                            const cs_x86_op &MaskOperand, NdVar RawResult,
                            uint16_t ElementSize) {
  NdVar Destination = L.operandWrite(DestinationOperand);
  if (Destination.Size == 0 || Destination.Size != RawResult.Size ||
      ElementSize == 0 || Destination.Size % ElementSize != 0)
    return false;

  const unsigned LaneCount = Destination.Size / ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  const RegInfo MaskInfo =
      mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
  if (!isX86OpmaskOperand(MaskOperand) || MaskInfo.Size < MaskSize)
    return false;

  const NdVar Mask = NdVar::reg(MaskInfo.Offset, MaskSize);
  const NdVar OldDestination = L.operandRead(S, DestinationOperand);
  std::vector<NdVar> SelectedLanes;
  SelectedLanes.reserve(LaneCount);
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    const uint64_t ByteOffset = static_cast<uint64_t>(Lane) * ElementSize;
    NdVar NewLane = S.makeTemp(ElementSize);
    S.emit(NdOp::SUBBYTES, NewLane, {RawResult, NdVar::cst(ByteOffset, 4)});

    NdVar ShiftedMask = Mask;
    if (Lane != 0) {
      ShiftedMask = S.makeTemp(MaskSize);
      S.emit(NdOp::INT_RIGHT, ShiftedMask, {Mask, NdVar::cst(Lane, MaskSize)});
    }
    NdVar MaskBitWide = S.makeTemp(MaskSize);
    S.emit(NdOp::INT_AND, MaskBitWide, {ShiftedMask, NdVar::cst(1, MaskSize)});
    NdVar MaskBit = MaskBitWide;
    if (MaskSize != 1) {
      MaskBit = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, MaskBit, {MaskBitWide, NdVar::cst(0, 4)});
    }

    NdVar Inactive = NdVar::cst(0, ElementSize);
    if (!MaskOperand.avx_zero_opmask) {
      Inactive = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, Inactive,
             {OldDestination, NdVar::cst(ByteOffset, 4)});
    }
    NdVar Selected = S.makeTemp(ElementSize);
    S.emit(NdOp::SELECT, Selected, {MaskBit, NewLane, Inactive});
    SelectedLanes.push_back(Selected);
  }

  NdVar Result = SelectedLanes.front();
  for (unsigned Lane = 1; Lane < LaneCount; ++Lane) {
    NdVar Next = S.makeTemp(Result.Size + ElementSize);
    S.emit(NdOp::CONCAT, Next, {SelectedLanes[Lane], Result});
    Result = Next;
  }
  S.emit(NdOp::COPY, Destination, {Result});
  return true;
}

bool liftEvexCompressExpandRegister(X86Lifter &L, X86Lifter::LiftState &S,
                                    const cs_insn *Insn, const cs_x86 &X86,
                                    uint16_t ElementSize, bool Compress,
                                    uint8_t Opcode, bool W) {
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x02 ||
      (Encoding.P1 | 0x04) != static_cast<uint8_t>((W ? 0x80 : 0) | 0x7d) ||
      Encoding.Opcode != Opcode || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0 || hasUnsupportedEvexValueModifier(X86) ||
      (X86.op_count != 2 && X86.op_count != 3))
    return false;

  auto IsVectorRegister = [](const cs_x86_op &Operand) {
    if (Operand.type != X86_OP_REG)
      return false;
    if (Operand.size == 16)
      return Operand.reg >= X86_REG_XMM0 && Operand.reg <= X86_REG_XMM31;
    if (Operand.size == 32)
      return Operand.reg >= X86_REG_YMM0 && Operand.reg <= X86_REG_YMM31;
    if (Operand.size == 64)
      return Operand.reg >= X86_REG_ZMM0 && Operand.reg <= X86_REG_ZMM31;
    return false;
  };
  auto VectorIndex = [](const cs_x86_op &Operand) {
    if (Operand.size == 16)
      return static_cast<unsigned>(Operand.reg - X86_REG_XMM0);
    if (Operand.size == 32)
      return static_cast<unsigned>(Operand.reg - X86_REG_YMM0);
    return static_cast<unsigned>(Operand.reg - X86_REG_ZMM0);
  };
  const bool HasMask = X86.op_count == 3;
  const unsigned SourceIndex = HasMask ? 2 : 1;
  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op &SourceOperand = X86.operands[SourceIndex];
  const bool MemoryForm = Compress ? DestinationOperand.type == X86_OP_MEM
                                   : SourceOperand.type == X86_OP_MEM;
  if (MemoryForm) {
    const cs_x86_op &VectorOperand =
        Compress ? SourceOperand : DestinationOperand;
    const cs_x86_op &MemoryOperand =
        Compress ? DestinationOperand : SourceOperand;
    if ((Encoding.ModRM & 0xc0) == 0xc0 || !IsVectorRegister(VectorOperand) ||
        MemoryOperand.type != X86_OP_MEM ||
        VectorOperand.size % ElementSize != 0 ||
        (VectorOperand.size != 16 && VectorOperand.size != 32 &&
         VectorOperand.size != 64))
      return false;
    const uint16_t VectorSize = VectorOperand.size;
    const uint8_t ExpectedLength =
        VectorSize == 16 ? 0 : (VectorSize == 32 ? 0x20 : 0x40);
    if ((Encoding.P2 & 0x60) != ExpectedLength ||
        (Encoding.P2 & 0x18) != 0x08 ||
        decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
            VectorIndex(VectorOperand) ||
        (Compress && (Encoding.P2 & 0x80) != 0) ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, MemoryOperand,
                                         ElementSize))
      return false;

    NdVar Mask = NdVar::cst(UINT64_MAX, 8);
    if (HasMask) {
      const cs_x86_op &MaskOperand = X86.operands[1];
      const RegInfo MaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
      const unsigned Lanes = VectorSize / ElementSize;
      const uint16_t Required = std::max(1u, (Lanes + 7u) / 8u);
      if (!isX86OpmaskOperand(MaskOperand) || MaskOperand.reg == X86_REG_K0 ||
          MaskInfo.Size < Required ||
          (Encoding.P2 & 7) != MaskOperand.reg - X86_REG_K0 ||
          ((Encoding.P2 & 0x80) != 0) != MaskOperand.avx_zero_opmask)
        return false;
      Mask = NdVar::reg(MaskInfo.Offset, 8);
    } else if ((Encoding.P2 & 0x87) != 0) {
      return false;
    }
    const NdVar Address = S.computeEA(MemoryOperand);
    const NdMemoryAddressSpace AddressSpace =
        X86Lifter::LiftState::memoryAddressSpace(MemoryOperand);
    if (AddressSpace != NdMemoryAddressSpace::Default)
      return false;
    if (Compress) {
      const NdVar Source = L.operandRead(S, VectorOperand);
      S.emitIntrinsic(Intrinsic::EVEXCompressStore, {},
                      {Address, Mask, Source, NdVar::cst(ElementSize, 1)},
                      NdMemoryOrdering::None, AddressSpace);
    } else {
      const NdVar OldDestination = L.operandRead(S, VectorOperand);
      const NdVar Destination = L.operandWrite(VectorOperand);
      const uint8_t Control = static_cast<uint8_t>(
          ElementSize |
          (HasMask && X86.operands[1].avx_zero_opmask ? 0x80 : 0));
      S.emitIntrinsic(Intrinsic::EVEXExpandLoad, Destination,
                      {Address, Mask, OldDestination, NdVar::cst(Control, 1)},
                      NdMemoryOrdering::None, AddressSpace);
    }
    return true;
  }
  if ((Encoding.ModRM & 0xc0) != 0xc0 ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Encoding))
    return false;
  if (!IsVectorRegister(DestinationOperand) ||
      !IsVectorRegister(SourceOperand) ||
      DestinationOperand.size != SourceOperand.size ||
      DestinationOperand.size % ElementSize != 0)
    return false;
  const uint16_t VectorSize = static_cast<uint16_t>(DestinationOperand.size);

  const uint8_t ExpectedLength =
      VectorSize == 16 ? 0 : (VectorSize == 32 ? 0x20 : 0x40);
  if ((Encoding.P2 & 0x60) != ExpectedLength || (Encoding.P2 & 0x18) != 0x08)
    return false;

  const unsigned DestinationIndex = VectorIndex(DestinationOperand);
  const unsigned SourceRegisterIndex = VectorIndex(SourceOperand);
  if (Compress) {
    if (decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
            SourceRegisterIndex ||
        decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
            DestinationIndex)
      return false;
  } else if (decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
                 DestinationIndex ||
             decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 SourceRegisterIndex) {
    return false;
  }

  bool ZeroInactive = false;
  NdVar Mask;
  if (HasMask) {
    const cs_x86_op &MaskOperand = X86.operands[1];
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
    const unsigned LaneCount = VectorSize / ElementSize;
    const uint16_t RequiredMaskSize =
        static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
    if (!isX86OpmaskOperand(MaskOperand) || MaskOperand.reg == X86_REG_K0 ||
        MaskOperand.size == 0 || MaskOperand.size > 8 ||
        MaskInfo.Size < RequiredMaskSize ||
        (Encoding.P2 & 7) != MaskOperand.reg - X86_REG_K0)
      return false;
    ZeroInactive = MaskOperand.avx_zero_opmask;
    if (((Encoding.P2 & 0x80) != 0) != ZeroInactive)
      return false;
    Mask = NdVar::reg(MaskInfo.Offset, 8);
  } else if ((Encoding.P2 & 0x87) != 0) {
    return false;
  }
  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].avx_zero_opmask && (!HasMask || Index != 1))
      return false;

  const NdVar Source = L.operandRead(S, SourceOperand);
  const NdVar Destination = L.operandWrite(DestinationOperand);
  if (!HasMask) {
    S.emit(NdOp::COPY, Destination, {Source});
    return true;
  }

  auto ExtractMaskBit = [&](unsigned Lane) {
    NdVar Shifted = Mask;
    if (Lane != 0) {
      Shifted = S.makeTemp(8);
      S.emit(NdOp::INT_RIGHT, Shifted, {Mask, NdVar::cst(Lane, 8)});
    }
    NdVar Bit = S.makeTemp(8);
    S.emit(NdOp::INT_AND, Bit, {Shifted, NdVar::cst(1, 8)});
    NdVar Narrow = S.makeTemp(1);
    S.emit(NdOp::SUBBYTES, Narrow, {Bit, NdVar::cst(0, 4)});
    return Narrow;
  };

  const NdVar OldDestination = L.operandRead(S, DestinationOperand);
  const unsigned LaneCount = VectorSize / ElementSize;
  std::vector<NdVar> ResultLanes;
  ResultLanes.reserve(LaneCount);
  if (Compress) {
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      if (ZeroInactive) {
        ResultLanes.push_back(NdVar::cst(0, ElementSize));
      } else {
        NdVar OldLane = S.makeTemp(ElementSize);
        S.emit(NdOp::SUBBYTES, OldLane,
               {OldDestination,
                NdVar::cst(static_cast<uint64_t>(Lane) * ElementSize, 4)});
        ResultLanes.push_back(OldLane);
      }
    }

    NdVar ActiveCount = NdVar::cst(0, 1);
    for (unsigned SourceLaneIndex = 0; SourceLaneIndex < LaneCount;
         ++SourceLaneIndex) {
      const NdVar MaskBit = ExtractMaskBit(SourceLaneIndex);
      NdVar SourceLane = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, SourceLane,
             {Source,
              NdVar::cst(static_cast<uint64_t>(SourceLaneIndex) * ElementSize,
                         sizeof(uint32_t))});
      for (unsigned OutputLane = 0; OutputLane < LaneCount; ++OutputLane) {
        NdVar IsOutputLane = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsOutputLane,
               {ActiveCount, NdVar::cst(OutputLane, 1)});
        NdVar ShouldWrite = S.makeTemp(1);
        S.emit(NdOp::INT_AND, ShouldWrite, {MaskBit, IsOutputLane});
        NdVar Selected = S.makeTemp(ElementSize);
        S.emit(NdOp::SELECT, Selected,
               {ShouldWrite, SourceLane, ResultLanes[OutputLane]});
        ResultLanes[OutputLane] = Selected;
      }
      NdVar NextCount = S.makeTemp(1);
      S.emit(NdOp::INT_ADD, NextCount, {ActiveCount, MaskBit});
      ActiveCount = NextCount;
    }
  } else {
    std::vector<NdVar> SourceLanes;
    SourceLanes.reserve(LaneCount);
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      NdVar SourceLane = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, SourceLane,
             {Source, NdVar::cst(static_cast<uint64_t>(Lane) * ElementSize,
                                 sizeof(uint32_t))});
      SourceLanes.push_back(SourceLane);
    }

    NdVar SourceLaneIndex = NdVar::cst(0, 1);
    for (unsigned OutputLane = 0; OutputLane < LaneCount; ++OutputLane) {
      const NdVar MaskBit = ExtractMaskBit(OutputLane);
      NdVar ActiveValue = NdVar::cst(0, ElementSize);
      for (unsigned Candidate = 0; Candidate < LaneCount; ++Candidate) {
        NdVar IsCandidate = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, IsCandidate,
               {SourceLaneIndex, NdVar::cst(Candidate, 1)});
        NdVar Selected = S.makeTemp(ElementSize);
        S.emit(NdOp::SELECT, Selected,
               {IsCandidate, SourceLanes[Candidate], ActiveValue});
        ActiveValue = Selected;
      }

      NdVar Inactive = NdVar::cst(0, ElementSize);
      if (!ZeroInactive) {
        Inactive = S.makeTemp(ElementSize);
        S.emit(
            NdOp::SUBBYTES, Inactive,
            {OldDestination,
             NdVar::cst(static_cast<uint64_t>(OutputLane) * ElementSize, 4)});
      }
      NdVar ResultLane = S.makeTemp(ElementSize);
      S.emit(NdOp::SELECT, ResultLane, {MaskBit, ActiveValue, Inactive});
      ResultLanes.push_back(ResultLane);

      NdVar NextIndex = S.makeTemp(1);
      S.emit(NdOp::INT_ADD, NextIndex, {SourceLaneIndex, MaskBit});
      SourceLaneIndex = NextIndex;
    }
  }

  NdVar Result = ResultLanes.front();
  for (unsigned Lane = 1; Lane < LaneCount; ++Lane) {
    NdVar Next = S.makeTemp(Result.Size + ElementSize);
    S.emit(NdOp::CONCAT, Next, {ResultLanes[Lane], Result});
    Result = Next;
  }
  S.emit(NdOp::COPY, Destination, {Result});
  return true;
}

} // namespace neverd
