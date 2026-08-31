//===- X86LiftSIMDAVXNarrow.cpp - EVEX packed narrowing -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Register-destination EVEX VPMOV narrowing instructions. Memory targets are
/// deliberately left fail-closed until their masked fault/store behavior has
/// a dedicated lowering.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include <algorithm>
#include <cstdint>

namespace neverd {

namespace {

enum class NarrowMode : uint8_t {
  Truncate,
  SignedSaturate,
  UnsignedSaturate,
};

struct NarrowSpec {
  uint8_t Opcode = 0;
  uint16_t SourceElementSize = 0;
  uint16_t DestinationElementSize = 0;
  NarrowMode Mode = NarrowMode::Truncate;
};

bool getNarrowSpec(unsigned InsnId, NarrowSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPMOVWB:
    Spec = {0x30, 2, 1, NarrowMode::Truncate};
    return true;
  case X86_INS_VPMOVDB:
    Spec = {0x31, 4, 1, NarrowMode::Truncate};
    return true;
  case X86_INS_VPMOVQB:
    Spec = {0x32, 8, 1, NarrowMode::Truncate};
    return true;
  case X86_INS_VPMOVDW:
    Spec = {0x33, 4, 2, NarrowMode::Truncate};
    return true;
  case X86_INS_VPMOVQW:
    Spec = {0x34, 8, 2, NarrowMode::Truncate};
    return true;
  case X86_INS_VPMOVQD:
    Spec = {0x35, 8, 4, NarrowMode::Truncate};
    return true;
  case X86_INS_VPMOVSWB:
    Spec = {0x20, 2, 1, NarrowMode::SignedSaturate};
    return true;
  case X86_INS_VPMOVSDB:
    Spec = {0x21, 4, 1, NarrowMode::SignedSaturate};
    return true;
  case X86_INS_VPMOVSQB:
    Spec = {0x22, 8, 1, NarrowMode::SignedSaturate};
    return true;
  case X86_INS_VPMOVSDW:
    Spec = {0x23, 4, 2, NarrowMode::SignedSaturate};
    return true;
  case X86_INS_VPMOVSQW:
    Spec = {0x24, 8, 2, NarrowMode::SignedSaturate};
    return true;
  case X86_INS_VPMOVSQD:
    Spec = {0x25, 8, 4, NarrowMode::SignedSaturate};
    return true;
  case X86_INS_VPMOVUSWB:
    Spec = {0x10, 2, 1, NarrowMode::UnsignedSaturate};
    return true;
  case X86_INS_VPMOVUSDB:
    Spec = {0x11, 4, 1, NarrowMode::UnsignedSaturate};
    return true;
  case X86_INS_VPMOVUSQB:
    Spec = {0x12, 8, 1, NarrowMode::UnsignedSaturate};
    return true;
  case X86_INS_VPMOVUSDW:
    Spec = {0x13, 4, 2, NarrowMode::UnsignedSaturate};
    return true;
  case X86_INS_VPMOVUSQW:
    Spec = {0x14, 8, 2, NarrowMode::UnsignedSaturate};
    return true;
  case X86_INS_VPMOVUSQD:
    Spec = {0x15, 8, 4, NarrowMode::UnsignedSaturate};
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

bool hasCanonicalNarrowEncoding(const cs_insn *Insn, const cs_x86 &X86,
                                Arch TargetArch, const NarrowSpec &Spec,
                                const cs_x86_op &DestinationOperand,
                                const cs_x86_op &SourceOperand, bool HasMask,
                                const cs_x86_op *MaskOperand, bool ZeroMask) {
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, TargetArch, Encoding) ||
      (Encoding.P0 & 0x07) != 0x02 || (Encoding.P1 | 0x04) != 0x7e ||
      Encoding.Opcode != Spec.Opcode || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0 || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  if ((Encoding.P2 & 0x18) != 0x08 || EncodedLength == 0x60)
    return false;
  const uint16_t EncodedSourceSize =
      EncodedLength == 0 ? 16 : (EncodedLength == 0x20 ? 32 : 64);
  if (EncodedSourceSize != SourceOperand.size ||
      !isVectorRegisterOfSize(SourceOperand, EncodedSourceSize) ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(SourceOperand))
    return false;

  const unsigned LaneCount = EncodedSourceSize / Spec.SourceElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  const uint8_t EncodedMask = Encoding.P2 & 0x07;
  if (HasMask) {
    if (!MaskOperand || !isX86OpmaskOperand(*MaskOperand) ||
        MaskOperand->reg == X86_REG_K0 || MaskOperand->size != MaskSize ||
        EncodedMask != static_cast<uint8_t>(MaskOperand->reg - X86_REG_K0) ||
        ZeroMask != static_cast<bool>(MaskOperand->avx_zero_opmask))
      return false;
  } else if (MaskOperand || EncodedMask != 0 || ZeroMask) {
    return false;
  }

  const uint16_t ActiveDestinationSize =
      static_cast<uint16_t>(LaneCount * Spec.DestinationElementSize);
  const bool MemoryDestination = DestinationOperand.type == X86_OP_MEM;
  if (MemoryDestination) {
    if ((Encoding.ModRM & 0xc0) == 0xc0 || ZeroMask ||
        DestinationOperand.size != ActiveDestinationSize ||
        !validateCanonicalEvexMemoryTail(
            Insn, X86, Encoding, DestinationOperand, ActiveDestinationSize))
      return false;
  } else {
    const uint16_t DestinationSize =
        std::max<uint16_t>(16, ActiveDestinationSize);
    if (!isVectorRegisterOfSize(DestinationOperand, DestinationSize) ||
        decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
            vectorRegisterIndex(DestinationOperand) ||
        !validateCanonicalEvexRegisterTail(Insn, X86, Encoding))
      return false;
  }

  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    if (Operand.avx_bcast != X86_AVX_BCAST_INVALID)
      return false;
    if (Operand.avx_zero_opmask && (!MaskOperand || &Operand != MaskOperand))
      return false;
  }
  return true;
}

Intrinsic maskedStoreForElementSize(uint16_t ElementSize) {
  switch (ElementSize) {
  case 1:
    return Intrinsic::MaskedStoreB;
  case 2:
    return Intrinsic::MaskedStoreW;
  case 4:
    return Intrinsic::MaskedStoreD;
  default:
    return Intrinsic::None;
  }
}

uint64_t maskForBytes(uint16_t Size) {
  return Size == 8 ? UINT64_MAX : (UINT64_C(1) << (Size * 8)) - 1;
}

NdVar extractMaskBit(X86Lifter::LiftState &S, NdVar Mask, unsigned Lane) {
  NdVar Shifted = Mask;
  if (Lane != 0) {
    Shifted = S.makeTemp(Mask.Size);
    S.emit(NdOp::INT_RIGHT, Shifted, {Mask, NdVar::cst(Lane, Mask.Size)});
  }
  NdVar WideBit = S.makeTemp(Mask.Size);
  S.emit(NdOp::INT_AND, WideBit, {Shifted, NdVar::cst(1, Mask.Size)});
  if (Mask.Size == 1)
    return WideBit;
  NdVar Bit = S.makeTemp(1);
  S.emit(NdOp::SUBBYTES, Bit, {WideBit, NdVar::cst(0, 4)});
  return Bit;
}

NdVar narrowLane(X86Lifter::LiftState &S, NdVar Source,
                 const NarrowSpec &Spec) {
  NdVar Narrow = S.makeTemp(Spec.DestinationElementSize);
  S.emit(NdOp::SUBBYTES, Narrow, {Source, NdVar::cst(0, 4)});
  if (Spec.Mode == NarrowMode::Truncate)
    return Narrow;

  // A round-trip detects whether the source already fits the destination.
  // On overflow, its original sign distinguishes the high and low clamps.
  // This also gives unsigned saturation its architectural signed-source
  // behavior: a negative source selects zero, while a positive overflow
  // selects the unsigned maximum.
  NdVar BackWide = S.makeTemp(Spec.SourceElementSize);
  S.emit(Spec.Mode == NarrowMode::SignedSaturate ? NdOp::INT_SEXT
                                                 : NdOp::INT_ZEXT,
         BackWide, {Narrow});
  NdVar Fits = S.makeTemp(1);
  S.emit(NdOp::INT_EQUAL, Fits, {Source, BackWide});

  const uint64_t High =
      Spec.Mode == NarrowMode::SignedSaturate
          ? (UINT64_C(1) << (Spec.DestinationElementSize * 8 - 1)) - 1
          : maskForBytes(Spec.DestinationElementSize);
  const uint64_t Low = Spec.Mode == NarrowMode::SignedSaturate
                           ? UINT64_C(1)
                                 << (Spec.DestinationElementSize * 8 - 1)
                           : 0;

  NdVar SelectHigh = S.makeTemp(1);
  if (Spec.Mode == NarrowMode::SignedSaturate) {
    S.emit(NdOp::INT_SLESS, SelectHigh,
           {NdVar::cst(0, Spec.SourceElementSize), Source});
  } else {
    S.emit(NdOp::INT_SLESS, SelectHigh,
           {NdVar::cst(High, Spec.SourceElementSize), Source});
  }

  NdVar Overflow = S.makeTemp(Spec.DestinationElementSize);
  S.emit(NdOp::SELECT, Overflow,
         {SelectHigh, NdVar::cst(High, Spec.DestinationElementSize),
          NdVar::cst(Low, Spec.DestinationElementSize)});
  NdVar Result = S.makeTemp(Spec.DestinationElementSize);
  S.emit(NdOp::SELECT, Result, {Fits, Narrow, Overflow});
  return Result;
}

} // namespace

bool liftSIMDAVXNarrow(X86Lifter &L, X86Lifter::LiftState &S,
                       const cs_insn *Insn, const cs_x86 &X86) {
  NarrowSpec Spec;
  if (!Insn || !getNarrowSpec(Insn->id, Spec))
    return false;

  // Complete validation precedes every operandRead and every emit. A rejected
  // modifier or shape therefore leaves the instruction transaction empty for
  // the strict dispatcher to report fail-closed.
  if (X86.op_count != 2 && X86.op_count != 3)
    return false;

  const bool HasMask = X86.op_count == 3;
  const unsigned SourceIndex = HasMask ? 2 : 1;
  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op *MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  const cs_x86_op &SourceOperand = X86.operands[SourceIndex];
  if (!isVectorRegisterOfSize(SourceOperand, SourceOperand.size) ||
      (SourceOperand.size != 16 && SourceOperand.size != 32 &&
       SourceOperand.size != 64) ||
      DestinationOperand.avx_zero_opmask || SourceOperand.avx_zero_opmask ||
      SourceOperand.size % Spec.SourceElementSize != 0)
    return false;

  const uint16_t SourceSize = static_cast<uint16_t>(SourceOperand.size);
  const unsigned LaneCount = SourceSize / Spec.SourceElementSize;
  const uint16_t ActiveDestinationSize =
      static_cast<uint16_t>(LaneCount * Spec.DestinationElementSize);
  const bool MemoryDestination = DestinationOperand.type == X86_OP_MEM;
  const uint16_t DestinationSize =
      MemoryDestination ? ActiveDestinationSize
                        : std::max<uint16_t>(16, ActiveDestinationSize);
  if ((!MemoryDestination && DestinationSize != 16 && DestinationSize != 32) ||
      (MemoryDestination &&
       maskedStoreForElementSize(Spec.DestinationElementSize) ==
           Intrinsic::None))
    return false;

  bool ZeroMask = false;
  uint16_t MaskSize = 0;
  RegInfo MaskInfo{};
  if (HasMask) {
    ZeroMask = MaskOperand->avx_zero_opmask;
    MaskSize = static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
    MaskInfo = mapCapstoneReg(static_cast<x86_reg>(MaskOperand->reg));
    if (!isX86OpmaskOperand(*MaskOperand) || MaskOperand->reg == X86_REG_K0 ||
        MaskOperand->size != MaskSize || MaskInfo.Offset == UINT64_C(0xffff) ||
        MaskInfo.Size < MaskSize)
      return false;
  }

  if (!hasCanonicalNarrowEncoding(Insn, X86, L.targetArch(), Spec,
                                  DestinationOperand, SourceOperand, HasMask,
                                  MaskOperand, ZeroMask))
    return false;

  const RegInfo SourceInfo =
      mapCapstoneReg(static_cast<x86_reg>(SourceOperand.reg));
  RegInfo DestinationInfo{};
  if (!MemoryDestination)
    DestinationInfo =
        mapCapstoneReg(static_cast<x86_reg>(DestinationOperand.reg));
  if (SourceInfo.Offset == UINT64_C(0xffff) || SourceInfo.Size != SourceSize ||
      (!MemoryDestination && (DestinationInfo.Offset == UINT64_C(0xffff) ||
                              DestinationInfo.Size != DestinationSize)))
    return false;

  const NdVar Source = L.operandRead(S, SourceOperand);
  const NdVar Destination =
      MemoryDestination ? NdVar{} : L.operandWrite(DestinationOperand);
  const NdVar Mask = HasMask ? NdVar::reg(MaskInfo.Offset, MaskSize) : NdVar{};
  const NdVar OldDestination = HasMask && !ZeroMask && !MemoryDestination
                                   ? L.operandRead(S, DestinationOperand)
                                   : NdVar{};

  NdVar Result = S.makeTemp(0);
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    const uint64_t SourceOffset =
        static_cast<uint64_t>(Lane) * Spec.SourceElementSize;
    NdVar SourceLane = S.makeTemp(Spec.SourceElementSize);
    S.emit(NdOp::SUBBYTES, SourceLane, {Source, NdVar::cst(SourceOffset, 4)});
    NdVar ResultLane = narrowLane(S, SourceLane, Spec);

    if (HasMask && !MemoryDestination) {
      const NdVar MaskBit = extractMaskBit(S, Mask, Lane);
      NdVar Inactive = NdVar::cst(0, Spec.DestinationElementSize);
      if (!ZeroMask) {
        Inactive = S.makeTemp(Spec.DestinationElementSize);
        const uint64_t DestinationOffset =
            static_cast<uint64_t>(Lane) * Spec.DestinationElementSize;
        S.emit(NdOp::SUBBYTES, Inactive,
               {OldDestination, NdVar::cst(DestinationOffset, 4)});
      }
      NdVar Selected = S.makeTemp(Spec.DestinationElementSize);
      S.emit(NdOp::SELECT, Selected, {MaskBit, ResultLane, Inactive});
      ResultLane = Selected;
    }

    if (Lane == 0) {
      Result = ResultLane;
    } else {
      NdVar Next = S.makeTemp(Result.Size + Spec.DestinationElementSize);
      S.emit(NdOp::CONCAT, Next, {ResultLane, Result});
      Result = Next;
    }
  }

  if (MemoryDestination) {
    const uint16_t CompactMaskSize =
        static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
    NdVar CompactMask =
        HasMask ? Mask
                : NdVar::cst(LaneCount == 64
                                 ? UINT64_MAX
                                 : (UINT64_C(1) << LaneCount) - UINT64_C(1),
                             CompactMaskSize);
    if (HasMask && LaneCount < 64) {
      NdVar RelevantMask = S.makeTemp(CompactMaskSize);
      S.emit(NdOp::INT_AND, RelevantMask,
             {CompactMask, NdVar::cst((UINT64_C(1) << LaneCount) - UINT64_C(1),
                                      CompactMaskSize)});
      CompactMask = RelevantMask;
    }
    const uint16_t StoreSize = Spec.DestinationElementSize == 1
                                   ? std::max<uint16_t>(8, Result.Size)
                                   : std::max<uint16_t>(16, Result.Size);
    if (Result.Size < StoreSize) {
      NdVar Padded = S.makeTemp(StoreSize);
      S.emit(NdOp::INT_ZEXT, Padded, {Result});
      Result = Padded;
    }
    const NdVar ExpandedMask = expandCompactLaneMask(
        S, CompactMask, StoreSize, Spec.DestinationElementSize);
    if (ExpandedMask.Size != Result.Size)
      return false;
    const NdVar Address = S.computeEA(DestinationOperand);
    S.emitIntrinsic(
        maskedStoreForElementSize(Spec.DestinationElementSize), {},
        {Address, ExpandedMask, Result}, NdMemoryOrdering::None,
        X86Lifter::LiftState::memoryAddressSpace(DestinationOperand));
    return true;
  }

  if (Result.Size < Destination.Size) {
    NdVar Extended = S.makeTemp(Destination.Size);
    S.emit(NdOp::INT_ZEXT, Extended, {Result});
    Result = Extended;
  }
  S.emit(NdOp::COPY, Destination, {Result});
  return true;
}

} // namespace neverd
