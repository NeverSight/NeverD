//===- X86LiftSIMDAVX.cpp - x86/x64 AVX/AVX-512/FMA instruction lifter --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatches FMA, AVX-512 opmask and AVX-512 packed integer/float
/// instructions to the per-family handlers in X86LiftSIMDAVX*.cpp, and lifts
/// the VSIB gather/scatter forms here because they reach the private
/// liftVectorGather (defined below).
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <vector>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

bool isVectorRegister(const RegInfo &RI) {
  if (RI.Offset < x86reg::XMM0 || RI.Size < 16 || RI.Size > 64)
    return false;
  const uint64_t Delta = RI.Offset - x86reg::XMM0;
  return Delta % x86reg::VectorRegStride == 0 &&
         Delta / x86reg::VectorRegStride < x86reg::VectorRegCount;
}

x86_reg effectiveEvexSegment(uint8_t Prefix, bool Is64Bit) {
  if (Is64Bit && Prefix != 0x64 && Prefix != 0x65)
    return X86_REG_INVALID;
  switch (Prefix) {
  case 0x26:
    return X86_REG_ES;
  case 0x2e:
    return X86_REG_CS;
  case 0x36:
    return X86_REG_SS;
  case 0x3e:
    return X86_REG_DS;
  case 0x64:
    return X86_REG_FS;
  case 0x65:
    return X86_REG_GS;
  default:
    return X86_REG_INVALID;
  }
}

bool matchesEvexAddressRegister(x86_reg Register, unsigned Index,
                                uint16_t AddressSize) {
  if (Index >= 32)
    return false;
  const RegInfo Info = mapCapstoneReg(Register);
  const uint64_t ExpectedOffset =
      Index < 16 ? static_cast<uint64_t>(Index) * 8
                 : x86reg::extendedGeneralReg(Index - 16);
  return Info.Offset == ExpectedOffset && Info.Size == AddressSize;
}

uint8_t evexVSIBOpcode(unsigned InsnId) {
  switch (InsnId) {
  case X86_INS_VPGATHERDD:
  case X86_INS_VPGATHERDQ:
    return 0x90;
  case X86_INS_VPGATHERQD:
  case X86_INS_VPGATHERQQ:
    return 0x91;
  case X86_INS_VGATHERDPS:
  case X86_INS_VGATHERDPD:
    return 0x92;
  case X86_INS_VGATHERQPS:
  case X86_INS_VGATHERQPD:
    return 0x93;
  case X86_INS_VPSCATTERDD:
  case X86_INS_VPSCATTERDQ:
    return 0xa0;
  case X86_INS_VPSCATTERQD:
  case X86_INS_VPSCATTERQQ:
    return 0xa1;
  case X86_INS_VSCATTERDPS:
  case X86_INS_VSCATTERDPD:
    return 0xa2;
  case X86_INS_VSCATTERQPS:
  case X86_INS_VSCATTERQPD:
    return 0xa3;
  default:
    return 0;
  }
}

bool validateCanonicalEvexVSIB(
    X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
    const cs_x86 &X86, unsigned InsnId, const cs_x86_op &MemoryOperand,
    const cs_x86_op &MaskOperand, const cs_x86_op &ValueOperand,
    uint16_t IndexElementBytes, uint16_t ValueElementBytes) {
  CanonicalEvexEncodingInfo Encoding;
  const uint8_t Opcode = evexVSIBOpcode(InsnId);
  if (Opcode == 0 ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x02 ||
      (Encoding.P1 | 0x04) !=
          static_cast<uint8_t>((ValueElementBytes == 8 ? 0x80 : 0) | 0x7d) ||
      Encoding.Opcode != Opcode || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0 || (Encoding.ModRM & 0xc0) == 0xc0 ||
      (Encoding.ModRM & 7) != 4 || (Encoding.P2 & 0x90) != 0)
    return false;

  if (MemoryOperand.type != X86_OP_MEM || MaskOperand.type != X86_OP_REG ||
      ValueOperand.type != X86_OP_REG ||
      MemoryOperand.mem.segment !=
          effectiveEvexSegment(Encoding.SegmentPrefix, Encoding.Is64Bit) ||
      MemoryOperand.mem.index == X86_REG_INVALID ||
      MaskOperand.reg <= X86_REG_K0 || MaskOperand.reg > X86_REG_K7 ||
      (Encoding.P2 & 7) != MaskOperand.reg - X86_REG_K0)
    return false;

  const RegInfo IndexInfo =
      mapCapstoneReg(static_cast<x86_reg>(MemoryOperand.mem.index));
  const RegInfo ValueInfo =
      mapCapstoneReg(static_cast<x86_reg>(ValueOperand.reg));
  if (!isVectorRegister(IndexInfo) || !isVectorRegister(ValueInfo) ||
      IndexInfo.Size != std::max<uint16_t>(
                            16, (MemoryOperand.size / ValueElementBytes) *
                                    IndexElementBytes) ||
      ValueInfo.Size != std::max<uint16_t>(16, MemoryOperand.size) ||
      ValueOperand.size != ValueInfo.Size)
    return false;

  const uint8_t ExpectedLength =
      IndexInfo.Size == 16 ? 0 : (IndexInfo.Size == 32 ? 0x20 : 0x40);
  if ((Encoding.P2 & 0x60) != ExpectedLength ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          (ValueInfo.Offset - x86reg::XMM0) / x86reg::VectorRegStride)
    return false;

  size_t Cursor = Encoding.Offset + 6;
  if (!Insn || Cursor >= Insn->size)
    return false;
  const uint8_t SIB = Insn->bytes[Cursor++];
  const unsigned Mod = Encoding.ModRM >> 6;
  const unsigned BaseLow = SIB & 7;
  const unsigned IndexLow = (SIB >> 3) & 7;
  const unsigned BaseExtension =
      ((Encoding.P0 & 0x20) == 0 ? 8 : 0) |
      ((Encoding.P0 & 0x08) != 0 ? 16 : 0);
  const unsigned VectorIndex =
      IndexLow | ((Encoding.P0 & 0x40) == 0 ? 8 : 0) |
      ((Encoding.P2 & 0x08) == 0 ? 16 : 0);
  const int ExpectedScale = 1 << (SIB >> 6);
  uint8_t DisplacementSize = 0;
  int ExpectedBase = -1;
  if (Mod == 0 && BaseLow == 5)
    DisplacementSize = 4;
  else
    ExpectedBase = static_cast<int>(BaseLow + BaseExtension);
  if (Mod == 1)
    DisplacementSize = 1;
  else if (Mod == 2)
    DisplacementSize = 4;

  const size_t DisplacementOffset = Cursor;
  int64_t RawDisplacement = 0;
  if (DisplacementSize == 1) {
    if (Cursor >= Insn->size)
      return false;
    RawDisplacement = static_cast<int8_t>(Insn->bytes[Cursor++]);
  } else if (DisplacementSize == 4) {
    if (Insn->size - Cursor < 4)
      return false;
    uint32_t Raw = 0;
    for (unsigned Byte = 0; Byte < 4; ++Byte)
      Raw |= static_cast<uint32_t>(Insn->bytes[Cursor + Byte]) << (Byte * 8);
    RawDisplacement = static_cast<int32_t>(Raw);
    Cursor += 4;
  }
  const int64_t Displacement =
      DisplacementSize == 1 ? RawDisplacement * ValueElementBytes
                            : RawDisplacement;
  if (Cursor != Insn->size || X86.sib != SIB ||
      X86.sib_base != MemoryOperand.mem.base ||
      X86.sib_index != MemoryOperand.mem.index ||
      X86.sib_scale != ExpectedScale ||
      MemoryOperand.mem.scale != ExpectedScale ||
      X86.encoding.disp_size != DisplacementSize ||
      X86.encoding.disp_offset !=
          (DisplacementSize == 0 ? 0 : DisplacementOffset) ||
      X86.disp != Displacement || MemoryOperand.mem.disp != Displacement ||
      IndexInfo.Offset != x86reg::vectorReg(VectorIndex))
    return false;

  if (ExpectedBase < 0)
    return MemoryOperand.mem.base == X86_REG_INVALID;
  return matchesEvexAddressRegister(
      static_cast<x86_reg>(MemoryOperand.mem.base),
      static_cast<unsigned>(ExpectedBase), S.AddressSize);
}

NdVar replaceVectorLane(X86Lifter::LiftState &S, NdVar Vector,
                        uint16_t VectorBytes, uint16_t ElementBytes,
                        uint16_t Lane, NdVar Replacement) {
  const uint16_t LaneCount = VectorBytes / ElementBytes;
  std::vector<NdVar> Parts;
  Parts.reserve(LaneCount);
  for (uint16_t Current = 0; Current < LaneCount; ++Current) {
    if (Current == Lane) {
      Parts.push_back(Replacement);
      continue;
    }
    NdVar Part = S.makeTemp(ElementBytes);
    S.emit(NdOp::SUBBYTES, Part,
           {Vector,
            NdVar::scalar(static_cast<uint64_t>(Current) * ElementBytes, 4)});
    Parts.push_back(Part);
  }
  while (Parts.size() > 1) {
    std::vector<NdVar> Joined;
    Joined.reserve(Parts.size() / 2);
    for (size_t I = 0; I < Parts.size(); I += 2) {
      NdVar Pair = S.makeTemp(Parts[I].Size + Parts[I + 1].Size);
      S.emit(NdOp::CONCAT, Pair, {Parts[I + 1], Parts[I]});
      Joined.push_back(Pair);
    }
    Parts = std::move(Joined);
  }
  return Parts.front();
}

/// Lift one EVEX VSIB gather/scatter using one fault-suppressing memory
/// intrinsic per lane.  Every structural check precedes the first emitted op:
/// a malformed or unsupported shape therefore leaves no plausible partial
/// semantics behind for another handler to consume.
bool liftEVEXVectorGatherScatter(X86Lifter &L, X86Lifter::LiftState &S,
                                 const cs_insn *Insn, const cs_x86 &X86,
                                 unsigned InsnId, bool Scatter) {
  uint16_t IndexElementBytes = 0;
  uint16_t ValueElementBytes = 0;
  switch (InsnId) {
  case X86_INS_VPGATHERDD:
  case X86_INS_VGATHERDPS:
  case X86_INS_VPSCATTERDD:
  case X86_INS_VSCATTERDPS:
    IndexElementBytes = 4;
    ValueElementBytes = 4;
    break;
  case X86_INS_VPGATHERDQ:
  case X86_INS_VGATHERDPD:
  case X86_INS_VPSCATTERDQ:
  case X86_INS_VSCATTERDPD:
    IndexElementBytes = 4;
    ValueElementBytes = 8;
    break;
  case X86_INS_VPGATHERQD:
  case X86_INS_VGATHERQPS:
  case X86_INS_VPSCATTERQD:
  case X86_INS_VSCATTERQPS:
    IndexElementBytes = 8;
    ValueElementBytes = 4;
    break;
  case X86_INS_VPGATHERQQ:
  case X86_INS_VGATHERQPD:
  case X86_INS_VPSCATTERQQ:
  case X86_INS_VSCATTERQPD:
    IndexElementBytes = 8;
    ValueElementBytes = 8;
    break;
  default:
    return false;
  }

  if (X86.opcode[0] != 0x62 || X86.op_count != 3 ||
      X86.addr_size != S.AddressSize ||
      (S.AddressSize != 4 && S.AddressSize != 8) || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID || X86.encoding.modrm_offset == 0 ||
      (X86.modrm & 0xc0) == 0xc0 || (X86.modrm & 7) != 4)
    return false;

  const cs_x86_op &MemoryOperand = X86.operands[Scatter ? 0 : 2];
  const cs_x86_op &MaskOperand = X86.operands[1];
  const cs_x86_op &ValueOperand = X86.operands[Scatter ? 2 : 0];
  if (!validateCanonicalEvexVSIB(L, S, Insn, X86, InsnId, MemoryOperand,
                                MaskOperand, ValueOperand, IndexElementBytes,
                                ValueElementBytes))
    return false;
  if (MemoryOperand.type != X86_OP_MEM || MaskOperand.type != X86_OP_REG ||
      ValueOperand.type != X86_OP_REG ||
      MemoryOperand.mem.index == X86_REG_INVALID ||
      (MemoryOperand.mem.scale != 1 && MemoryOperand.mem.scale != 2 &&
       MemoryOperand.mem.scale != 4 && MemoryOperand.mem.scale != 8) ||
      X86.sib_index != MemoryOperand.mem.index ||
      X86.sib_base != MemoryOperand.mem.base ||
      X86.sib_scale != MemoryOperand.mem.scale ||
      MaskOperand.reg <= X86_REG_K0 || MaskOperand.reg > X86_REG_K7)
    return false;

  for (uint8_t I = 0; I < X86.op_count; ++I)
    if (X86.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
        X86.operands[I].avx_zero_opmask)
      return false;

  const RegInfo IndexRI =
      mapCapstoneReg(static_cast<x86_reg>(MemoryOperand.mem.index));
  const RegInfo ValueRI =
      mapCapstoneReg(static_cast<x86_reg>(ValueOperand.reg));
  const RegInfo MaskRI = mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
  if (!isVectorRegister(IndexRI) || !isVectorRegister(ValueRI) ||
      MaskRI.Size != 8 || MemoryOperand.size == 0 ||
      MemoryOperand.size % ValueElementBytes != 0)
    return false;

  const uint16_t DataBytes = MemoryOperand.size;
  const uint16_t LaneCount = DataBytes / ValueElementBytes;
  const uint16_t MaximumLaneCount =
      IndexElementBytes == 4 && ValueElementBytes == 4 ? 16 : 8;
  const uint16_t ExpectedIndexBytes =
      std::max<uint16_t>(16, LaneCount * IndexElementBytes);
  const uint16_t ExpectedValueRegisterBytes = std::max<uint16_t>(16, DataBytes);
  if (LaneCount < 2 || LaneCount > MaximumLaneCount ||
      (LaneCount & (LaneCount - 1)) != 0 ||
      IndexRI.Size != ExpectedIndexBytes ||
      ValueRI.Size != ExpectedValueRegisterBytes ||
      ValueOperand.size != ExpectedValueRegisterBytes ||
      MaskOperand.size != (LaneCount + 7) / 8)
    return false;

  // EVEX gathers cannot alias the destination and VSIB index.  Such an
  // encoding raises #UD rather than executing with an implementation-chosen
  // read/write order.
  if (!Scatter && ValueRI.Offset == IndexRI.Offset)
    return false;

  if (MemoryOperand.mem.base != X86_REG_INVALID) {
    const RegInfo BaseRI =
        mapCapstoneReg(static_cast<x86_reg>(MemoryOperand.mem.base));
    if (!x86reg::isGeneralRegOffset(BaseRI.Offset) ||
        BaseRI.Size != S.AddressSize)
      return false;
  }

  // Form base+displacement in the architectural address-size domain.  The
  // signed VSIB lane and scale are added later, also at this width, so addr32
  // wraps before its final zero extension.  FS/GS is carried separately as a
  // LowIR address space and is never folded into this offset.
  const uint16_t AddressBytes = S.AddressSize;
  NdVar BaseDisplacement = S.makeTemp(AddressBytes);
  bool First = true;
  auto Accumulate = [&](NdVar Value) {
    if (First) {
      S.emit(NdOp::COPY, BaseDisplacement, {Value});
      First = false;
    } else {
      S.emit(NdOp::INT_ADD, BaseDisplacement, {BaseDisplacement, Value});
    }
  };
  if (MemoryOperand.mem.base != X86_REG_INVALID) {
    const RegInfo BaseRI =
        mapCapstoneReg(static_cast<x86_reg>(MemoryOperand.mem.base));
    Accumulate(NdVar::reg(BaseRI.Offset, AddressBytes));
  }
  if (S.RelocatedDisplacement) {
    NdVar Displacement = *S.RelocatedDisplacement;
    const uint64_t Original = Displacement.Offset;
    if (AddressBytes < 8)
      Displacement.Offset &= UINT64_C(0xffffffff);
    Displacement.Size = AddressBytes;
    if (X86Lifter::LiftState::memoryAddressSpace(MemoryOperand) !=
        NdMemoryAddressSpace::Default) {
      Displacement.Provenance = ConstantAddressProvenance::Scalar;
      Displacement.AddressOwnerVA = InvalidVA;
    } else if (Displacement.Offset != Original &&
               Displacement.Provenance != ConstantAddressProvenance::Unknown &&
               Displacement.Provenance != ConstantAddressProvenance::Scalar) {
      Displacement.Provenance = ConstantAddressProvenance::Address;
      Displacement.AddressOwnerVA = InvalidVA;
    }
    Accumulate(Displacement);
  } else if (MemoryOperand.mem.disp != 0) {
    Accumulate(NdVar::scalar(static_cast<uint64_t>(MemoryOperand.mem.disp),
                             AddressBytes));
  }
  if (First)
    Accumulate(NdVar::scalar(0, AddressBytes));

  const NdMemoryAddressSpace AddressSpace =
      X86Lifter::LiftState::memoryAddressSpace(MemoryOperand);
  const NdVar IndexVector = NdVar::reg(IndexRI.Offset, IndexRI.Size);
  const NdVar FullMask = NdVar::reg(MaskRI.Offset, 8);
  NdVar WorkingMask = FullMask;
  const NdVar FullValue = NdVar::reg(ValueRI.Offset, 64);
  NdVar WorkingDestination = FullValue;

  for (uint16_t Lane = 0; Lane < LaneCount; ++Lane) {
    NdVar MaskShift = S.makeTemp(8);
    S.emit(NdOp::INT_RIGHT, MaskShift, {WorkingMask, NdVar::scalar(Lane, 8)});
    NdVar MaskBit = S.makeTemp(8);
    S.emit(NdOp::INT_AND, MaskBit, {MaskShift, NdVar::scalar(1, 8)});

    NdVar IndexElement = S.makeTemp(IndexElementBytes);
    S.emit(NdOp::SUBBYTES, IndexElement,
           {IndexVector,
            NdVar::scalar(static_cast<uint64_t>(Lane) * IndexElementBytes, 4)});
    NdVar AddressIndex = IndexElement;
    if (IndexElementBytes < AddressBytes) {
      AddressIndex = S.makeTemp(AddressBytes);
      S.emit(NdOp::INT_SEXT, AddressIndex, {IndexElement});
    } else if (IndexElementBytes > AddressBytes) {
      AddressIndex = S.makeTemp(AddressBytes);
      S.emit(NdOp::SUBBYTES, AddressIndex, {IndexElement, NdVar::scalar(0, 4)});
    }
    NdVar ScaledIndex = AddressIndex;
    if (MemoryOperand.mem.scale != 1) {
      ScaledIndex = S.makeTemp(AddressBytes);
      S.emit(
          NdOp::INT_MULT, ScaledIndex,
          {AddressIndex, NdVar::scalar(MemoryOperand.mem.scale, AddressBytes)});
    }
    NdVar Offset = S.makeTemp(AddressBytes);
    S.emit(NdOp::INT_ADD, Offset, {BaseDisplacement, ScaledIndex});
    NdVar Address = Offset;
    if (AddressBytes < 8) {
      Address = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, Address, {Offset});
    }

    const uint64_t SignMask = ValueElementBytes == 8
                                  ? UINT64_C(0x8000000000000000)
                                  : UINT64_C(0x80000000);
    NdVar LaneMask = S.makeTemp(16);
    S.emit(NdOp::SELECT, LaneMask,
           {MaskBit, NdVar::scalar(SignMask, 16), NdVar::scalar(0, 16)});

    if (Scatter) {
      NdVar SourceLane = S.makeTemp(ValueElementBytes);
      S.emit(
          NdOp::SUBBYTES, SourceLane,
          {FullValue,
           NdVar::scalar(static_cast<uint64_t>(Lane) * ValueElementBytes, 4)});
      NdVar StoreVector = S.makeTemp(16);
      S.emit(NdOp::INT_ZEXT, StoreVector, {SourceLane});
      S.emitIntrinsic(ValueElementBytes == 8 ? Intrinsic::MaskedStoreQ
                                             : Intrinsic::MaskedStoreD,
                      {}, {Address, LaneMask, StoreVector},
                      NdMemoryOrdering::None, AddressSpace);
    } else {
      NdVar LoadedVector = S.makeTemp(16);
      S.emitIntrinsic(ValueElementBytes == 8 ? Intrinsic::MaskedLoadQ
                                             : Intrinsic::MaskedLoadD,
                      LoadedVector, {Address, LaneMask}, NdMemoryOrdering::None,
                      AddressSpace);
      NdVar Loaded = S.makeTemp(ValueElementBytes);
      S.emit(NdOp::SUBBYTES, Loaded, {LoadedVector, NdVar::scalar(0, 4)});
      NdVar OldLane = S.makeTemp(ValueElementBytes);
      S.emit(
          NdOp::SUBBYTES, OldLane,
          {WorkingDestination,
           NdVar::scalar(static_cast<uint64_t>(Lane) * ValueElementBytes, 4)});
      NdVar ResultLane = S.makeTemp(ValueElementBytes);
      S.emit(NdOp::SELECT, ResultLane, {MaskBit, Loaded, OldLane});
      WorkingDestination = replaceVectorLane(
          S, WorkingDestination, 64, ValueElementBytes, Lane, ResultLane);
    }

    NdVar ClearedMask = S.makeTemp(8);
    S.emit(NdOp::INT_AND, ClearedMask,
           {WorkingMask, NdVar::scalar(~(UINT64_C(1) << Lane), 8)});

    // A memory fault stops before these copies.  Thus each successful active
    // lane is restartably committed and cleared, while the faulting lane and
    // every later lane keep their incoming state.  Low-to-high is a permitted
    // deterministic implementation order and also preserves Intel's required
    // ordering for overlapping scatter destinations.
    if (!Scatter) {
      S.emit(NdOp::COPY, FullValue, {WorkingDestination});
      WorkingDestination = FullValue;
    }
    S.emit(NdOp::COPY, FullMask, {ClearedMask});
    WorkingMask = FullMask;
  }

  if (!Scatter && DataBytes < 64) {
    NdVar LowResult = S.makeTemp(DataBytes);
    S.emit(NdOp::SUBBYTES, LowResult,
           {WorkingDestination, NdVar::scalar(0, 4)});
    S.emit(NdOp::INT_ZEXT, FullValue, {LowResult});
  }
  // Both gather and scatter set the entire K register to zero on normal
  // completion.  Keeping this final full-width write after every possible
  // fault preserves high K bits on an interrupted instruction.
  S.emit(NdOp::COPY, FullMask, {NdVar::scalar(0, 8)});
  return true;
}

} // namespace

bool X86Lifter::liftSIMDAVX(LiftState &S, const cs_insn *Insn,
                            const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // ========================================================================
  // Gather — native per-lane conditional load (see liftVectorGather).  The FP
  // forms are bit-identical to the integer VPGATHER forms (the value is only
  // FP-typed), so they share the same lowering.
  // ========================================================================
  case X86_INS_VGATHERDPD:
  case X86_INS_VGATHERDPS:
  case X86_INS_VGATHERQPD:
  case X86_INS_VGATHERQPS: {
    if (X86.opcode[0] == 0x62)
      return liftEVEXVectorGatherScatter(*this, S, Insn, X86, InsnId, false);
    return liftVectorGather(S, X86, InsnId);
  }
  // AVX-512 sparse prefetch is an architecturally non-faulting hint.  It does
  // not update its mask, so a no-op is exact at LowIR's architectural level.
  case X86_INS_VGATHERPF0DPD:
  case X86_INS_VGATHERPF0DPS:
  case X86_INS_VGATHERPF0QPD:
  case X86_INS_VGATHERPF0QPS:
  case X86_INS_VGATHERPF1DPD:
  case X86_INS_VGATHERPF1DPS:
  case X86_INS_VGATHERPF1QPD:
  case X86_INS_VGATHERPF1QPS: {
    return true;
  }
  case X86_INS_VSCATTERDPD:
  case X86_INS_VSCATTERDPS:
  case X86_INS_VSCATTERQPD:
  case X86_INS_VSCATTERQPS: {
    return liftEVEXVectorGatherScatter(*this, S, Insn, X86, InsnId, true);
  }
  case X86_INS_VSCATTERPF0DPD:
  case X86_INS_VSCATTERPF0DPS:
  case X86_INS_VSCATTERPF0QPD:
  case X86_INS_VSCATTERPF0QPS:
  case X86_INS_VSCATTERPF1DPD:
  case X86_INS_VSCATTERPF1DPS:
  case X86_INS_VSCATTERPF1QPD:
  case X86_INS_VSCATTERPF1QPS: {
    return true;
  }

  // VPGATHER{DD,DQ,QD,QQ} — gather load from memory via index vector.
  case X86_INS_VPGATHERDD:
  case X86_INS_VPGATHERDQ:
  case X86_INS_VPGATHERQD:
  case X86_INS_VPGATHERQQ: {
    if (X86.opcode[0] == 0x62)
      return liftEVEXVectorGatherScatter(*this, S, Insn, X86, InsnId, false);
    return liftVectorGather(S, X86, InsnId);
  }

  case X86_INS_VPSCATTERDD:
  case X86_INS_VPSCATTERDQ:
  case X86_INS_VPSCATTERQD:
  case X86_INS_VPSCATTERQQ: {
    return liftEVEXVectorGatherScatter(*this, S, Insn, X86, InsnId, true);
  }

  default:
    return liftSIMDAVXFMA(*this, S, Insn, X86) ||
           liftSIMDAVXSSE(*this, S, Insn, X86) ||
           liftSIMDAVXMask(*this, S, Insn, X86) ||
           liftSIMDAVXNarrow(*this, S, Insn, X86) ||
           liftSIMDAVXInt(*this, S, Insn, X86) ||
           liftSIMDAVXConvert(*this, S, Insn, X86) ||
           liftSIMDAVXFloat(*this, S, Insn, X86);
  }
  return true;
}

bool X86Lifter::liftVectorGather(LiftState &S, const cs_x86 &X86,
                                 unsigned InsnId) {
  // Index element size (D=4 / Q=8) and value element size, derived from the
  // mnemonic: VxGATHER<idx><val>.  The FP forms share the integer sizes.
  uint16_t IdxSz, ValSz;
  switch (InsnId) {
  case X86_INS_VPGATHERDD:
  case X86_INS_VGATHERDPS:
    IdxSz = 4;
    ValSz = 4;
    break;
  case X86_INS_VPGATHERDQ:
  case X86_INS_VGATHERDPD:
    IdxSz = 4;
    ValSz = 8;
    break;
  case X86_INS_VPGATHERQD:
  case X86_INS_VGATHERQPS:
    IdxSz = 8;
    ValSz = 4;
    break;
  case X86_INS_VPGATHERQQ:
  case X86_INS_VGATHERQPD:
    IdxSz = 8;
    ValSz = 8;
    break;
  default:
    return false;
  }

  // Operands: dst = operands[0]; the VSIB memory operand carries the base GPR,
  // the vector index register, scale, and disp; the remaining register operand
  // is the mask (read for sign bits, zeroed afterward).
  const cs_x86_op *Mem = nullptr;
  int MaskIdx = -1;
  for (int I = 0; I < X86.op_count; ++I) {
    if (X86.operands[I].type == X86_OP_MEM)
      Mem = &X86.operands[I];
    else if (I != 0 && X86.operands[I].type == X86_OP_REG)
      MaskIdx = I;
  }
  if (!Mem || MaskIdx < 0 || X86.operands[0].type != X86_OP_REG ||
      Mem->mem.index == X86_REG_INVALID)
    return false;

  NdVar Dst = operandWrite(X86.operands[0]);
  uint16_t DstBytes = Dst.Size;
  RegInfo IdxRI = mapCapstoneReg(static_cast<x86_reg>(Mem->mem.index));
  NdVar IdxVec = NdVar::reg(IdxRI.Offset, IdxRI.Size);
  NdVar MaskVec = operandRead(S, X86.operands[MaskIdx]);
  NdVar DstOld = operandRead(S, X86.operands[0]);

  uint16_t IdxLanes = IdxRI.Size / IdxSz;
  uint16_t ValLanes = DstBytes / ValSz;
  uint16_t NumElems = IdxLanes < ValLanes ? IdxLanes : ValLanes;
  if (NumElems == 0 || ValLanes > 8)
    return false;

  // Build the effective offset in the instruction's address-size domain.
  // addr32 gathers on x86-64 wrap base/index/scale/disp at 32 bits before the
  // final zero-extension used by LowIR's common 64-bit memory-address carrier.
  const uint16_t AddrSz = S.AddressSize;
  NdVar BaseAddr = S.makeTemp(AddrSz);
  bool First = true;
  auto Acc = [&](NdVar V) {
    if (First) {
      S.emit(NdOp::COPY, BaseAddr, {V});
      First = false;
    } else {
      S.emit(NdOp::INT_ADD, BaseAddr, {BaseAddr, V});
    }
  };
  if (Mem->mem.base != X86_REG_INVALID) {
    RegInfo BaseRI = mapCapstoneReg(static_cast<x86_reg>(Mem->mem.base));
    Acc(NdVar::reg(BaseRI.Offset, AddrSz));
  }
  if (S.RelocatedDisplacement) {
    NdVar Displacement = *S.RelocatedDisplacement;
    const uint64_t Original = Displacement.Offset;
    if (AddrSz < 8) {
      const unsigned Bits = AddrSz * 8;
      Displacement.Offset &= (uint64_t{1} << Bits) - 1;
    }
    Displacement.Size = AddrSz;
    if (LiftState::memoryAddressSpace(*Mem) != NdMemoryAddressSpace::Default) {
      Displacement.Provenance = ConstantAddressProvenance::Scalar;
      Displacement.AddressOwnerVA = InvalidVA;
    } else if (Displacement.Offset != Original &&
               Displacement.Provenance != ConstantAddressProvenance::Unknown &&
               Displacement.Provenance != ConstantAddressProvenance::Scalar) {
      Displacement.Provenance = ConstantAddressProvenance::Address;
      Displacement.AddressOwnerVA = InvalidVA;
    }
    Acc(Displacement);
  } else if (Mem->mem.disp != 0)
    Acc(NdVar::scalar(static_cast<uint64_t>(Mem->mem.disp), AddrSz));
  if (First)
    Acc(NdVar::scalar(0, AddrSz));
  unsigned Scale = Mem->mem.scale ? Mem->mem.scale : 1;

  NdVar MaskOut = operandWrite(X86.operands[MaskIdx]);
  NdVar WorkingDst = DstOld;
  NdVar WorkingMask = MaskVec;
  auto ReplaceLane = [&](NdVar Vector, uint16_t Lane, NdVar Replacement) {
    NdVar Parts[8];
    for (uint16_t J = 0; J < ValLanes; ++J) {
      if (J == Lane) {
        Parts[J] = Replacement;
        continue;
      }
      Parts[J] = S.makeTemp(ValSz);
      S.emit(NdOp::SUBBYTES, Parts[J],
             {Vector, NdVar::scalar(static_cast<uint64_t>(J) * ValSz, 4)});
    }
    uint16_t Count = ValLanes;
    uint16_t Sz = ValSz;
    while (Count > 1) {
      const uint16_t Half = Count / 2;
      const uint16_t NewSz = static_cast<uint16_t>(Sz * 2);
      for (uint16_t K = 0; K < Half; ++K) {
        NdVar Out = S.makeTemp(NewSz);
        S.emit(NdOp::CONCAT, Out, {Parts[2 * K + 1], Parts[2 * K]});
        Parts[K] = Out;
      }
      Count = Half;
      Sz = NewSz;
    }
    return Parts[0];
  };

  for (uint16_t I = 0; I < ValLanes; ++I) {
    if (I >= NumElems) {
      // SDM: lanes past the gather count are zero on successful completion.
      WorkingDst = ReplaceLane(WorkingDst, I, NdVar::scalar(0, ValSz));
      WorkingMask = ReplaceLane(WorkingMask, I, NdVar::scalar(0, ValSz));
      S.emit(NdOp::COPY, Dst, {WorkingDst});
      S.emit(NdOp::COPY, MaskOut, {WorkingMask});
      WorkingDst = Dst;
      WorkingMask = MaskOut;
      continue;
    }
    // addr = baseAddr + sext(index[I]) * scale.
    NdVar IdxElem = S.makeTemp(IdxSz);
    S.emit(NdOp::SUBBYTES, IdxElem,
           {IdxVec, NdVar::scalar(static_cast<uint64_t>(I) * IdxSz, 4)});
    NdVar AddressIndex = IdxElem;
    if (IdxSz < AddrSz) {
      AddressIndex = S.makeTemp(AddrSz);
      S.emit(NdOp::INT_SEXT, AddressIndex, {IdxElem});
    } else if (IdxSz > AddrSz) {
      AddressIndex = S.makeTemp(AddrSz);
      S.emit(NdOp::SUBBYTES, AddressIndex, {IdxElem, NdVar::scalar(0, 4)});
    }
    NdVar Off = AddressIndex;
    if (Scale > 1) {
      Off = S.makeTemp(AddrSz);
      S.emit(NdOp::INT_MULT, Off, {AddressIndex, NdVar::scalar(Scale, AddrSz)});
    }
    NdVar Offset = S.makeTemp(AddrSz);
    S.emit(NdOp::INT_ADD, Offset, {BaseAddr, Off});
    NdVar Addr = Offset;
    if (AddrSz < 8) {
      Addr = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, Addr, {Offset});
    }
    // The mask element's sign bit gates the load; a clear sign keeps the source
    // lane (operands[0] is also the merge source for masked-off elements).
    NdVar MaskElem = S.makeTemp(ValSz);
    S.emit(NdOp::SUBBYTES, MaskElem,
           {WorkingMask, NdVar::scalar(static_cast<uint64_t>(I) * ValSz, 4)});
    NdVar SignSet = S.makeTemp(1);
    S.emit(NdOp::INT_SLESS, SignSet, {MaskElem, NdVar::scalar(0, ValSz)});
    // Reuse the fault-suppressing masked-load intrinsic for one lane.  The low
    // lane carries this gather element's mask and every other lane is zero, so
    // a clear sign bit performs no memory access at all (unlike LOAD+SELECT).
    NdVar LaneMask = S.makeTemp(16);
    S.emit(NdOp::INT_ZEXT, LaneMask, {MaskElem});
    NdVar LoadedVector = S.makeTemp(16);
    S.emitIntrinsic(ValSz == 8 ? Intrinsic::MaskedLoadQ
                               : Intrinsic::MaskedLoadD,
                    LoadedVector, {Addr, LaneMask}, NdMemoryOrdering::None,
                    LiftState::memoryAddressSpace(*Mem));
    NdVar Loaded = S.makeTemp(ValSz);
    S.emit(NdOp::SUBBYTES, Loaded, {LoadedVector, NdVar::scalar(0, 4)});
    NdVar OldLane = S.makeTemp(ValSz);
    S.emit(NdOp::SUBBYTES, OldLane,
           {WorkingDst, NdVar::scalar(static_cast<uint64_t>(I) * ValSz, 4)});
    NdVar Res = S.makeTemp(ValSz);
    S.emit(NdOp::SELECT, Res, {SignSet, Loaded, OldLane});
    // Commit each completed lane before the next possible faulting access.
    // Intel permits implementation-defined gather order; choosing low-to-high
    // makes partial progress deterministic.  If a later lane faults, every
    // prior destination update and mask clear remains architecturally visible.
    WorkingDst = ReplaceLane(WorkingDst, I, Res);
    WorkingMask = ReplaceLane(WorkingMask, I, NdVar::scalar(0, ValSz));
    S.emit(NdOp::COPY, Dst, {WorkingDst});
    S.emit(NdOp::COPY, MaskOut, {WorkingMask});
    WorkingDst = Dst;
    WorkingMask = MaskOut;
  }
  return true;
}

} // namespace neverd
