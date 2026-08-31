//===- X86LiftSIMDAVXFloat.cpp - x86/x64 AVX/AVX-512 float and lane-move lifter
//-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The remaining VEX/EVEX V* instructions: range, scale,
/// exponent/mantissa extraction, reduce, round-to-scale,
/// fixup and class tests, the 14/28-bit reciprocal and
/// reciprocal-square-root approximations, EVEX broadcast,
/// insert/extract and compress/expand, VANDN, unpack, and
/// the high/low quadword moves.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include <algorithm>
#include <cstdint>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

enum class LaneMoveKind : uint8_t {
  Broadcast,
  Insert,
  Extract,
};

struct LaneMoveSpec {
  LaneMoveKind Kind = LaneMoveKind::Broadcast;
  uint8_t Opcode = 0;
  uint16_t ElementSize = 0;
  uint16_t LaneSize = 0;
  bool W = false;
  bool RegisterTuple = false;
};

struct ApproxSpec {
  X86ApproxFloatKind Kind = X86ApproxFloatKind::Rcp14F32;
  uint8_t Opcode = 0;
  bool W = false;
  bool Scalar = false;
  bool Reference28 = false;
};

struct FPControlSpec {
  uint8_t Opcode = 0;
  uint8_t ElementSize = 0;
  bool W = false;
  bool Scalar = false;
  bool HasMergeSource = false;
};

bool beginsWithPotentialEvexPrefix(const cs_insn *Insn) {
  if (!Insn)
    return false;
  size_t Offset = 0;
  while (Offset < Insn->size) {
    switch (Insn->bytes[Offset]) {
    case 0xf0:
    case 0xf2:
    case 0xf3:
    case 0x2e:
    case 0x36:
    case 0x3e:
    case 0x26:
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67:
      ++Offset;
      continue;
    default:
      return Insn->bytes[Offset] == 0x62;
    }
  }
  return false;
}

bool parseCanonicalEvexLligEncodingInfo(const cs_insn *Insn, const cs_x86 &X86,
                                        Arch TargetArch,
                                        CanonicalEvexEncodingInfo &Encoding) {
  if (!Insn || X86.encoding.modrm_offset < 2 ||
      X86.encoding.modrm_offset >= Insn->size)
    return false;

  // Capstone canonicalizes the architecturally ignored L'L bits to zero in
  // opcode[] for scalar LLIG forms. Preserve exact validation of every other
  // decoded bit while recovering L'L from the immutable instruction bytes.
  const size_t P2Offset = X86.encoding.modrm_offset - 2;
  const uint8_t RawP2 = Insn->bytes[P2Offset];
  if (((X86.opcode[3] ^ RawP2) & static_cast<uint8_t>(~0x60U)) != 0)
    return false;

  cs_x86 CanonicalDetail = X86;
  CanonicalDetail.opcode[3] = RawP2;
  return parseCanonicalEvexEncodingInfo(Insn, CanonicalDetail, TargetArch,
                                        Encoding);
}

bool getFPControlSpec(unsigned InsnId, FPControlSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VRANGEPS:
    Spec = {0x50, 4, false, false, true};
    return true;
  case X86_INS_VRANGEPD:
    Spec = {0x50, 8, true, false, true};
    return true;
  case X86_INS_VRANGESS:
    Spec = {0x51, 4, false, true, true};
    return true;
  case X86_INS_VRANGESD:
    Spec = {0x51, 8, true, true, true};
    return true;
  case X86_INS_VGETMANTPS:
    Spec = {0x26, 4, false, false, false};
    return true;
  case X86_INS_VGETMANTPD:
    Spec = {0x26, 8, true, false, false};
    return true;
  case X86_INS_VGETMANTSS:
    Spec = {0x27, 4, false, true, true};
    return true;
  case X86_INS_VGETMANTSD:
    Spec = {0x27, 8, true, true, true};
    return true;
  case X86_INS_VREDUCEPS:
    Spec = {0x56, 4, false, false, false};
    return true;
  case X86_INS_VREDUCEPD:
    Spec = {0x56, 8, true, false, false};
    return true;
  case X86_INS_VREDUCESS:
    Spec = {0x57, 4, false, true, true};
    return true;
  case X86_INS_VREDUCESD:
    Spec = {0x57, 8, true, true, true};
    return true;
  case X86_INS_VRNDSCALEPS:
    Spec = {0x08, 4, false, false, false};
    return true;
  case X86_INS_VRNDSCALEPD:
    Spec = {0x09, 8, true, false, false};
    return true;
  case X86_INS_VRNDSCALESS:
    Spec = {0x0a, 4, false, true, true};
    return true;
  case X86_INS_VRNDSCALESD:
    Spec = {0x0b, 8, true, true, true};
    return true;
  case X86_INS_VFIXUPIMMPS:
    Spec = {0x54, 4, false, false, true};
    return true;
  case X86_INS_VFIXUPIMMPD:
    Spec = {0x54, 8, true, false, true};
    return true;
  case X86_INS_VFIXUPIMMSS:
    Spec = {0x55, 4, false, true, true};
    return true;
  case X86_INS_VFIXUPIMMSD:
    Spec = {0x55, 8, true, true, true};
    return true;
  default:
    return false;
  }
}

bool getApproxSpec(unsigned InsnId, ApproxSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VRCP14PS:
    Spec = {X86ApproxFloatKind::Rcp14F32, 0x4c, false, false};
    return true;
  case X86_INS_VRCP14PD:
    Spec = {X86ApproxFloatKind::Rcp14F64, 0x4c, true, false};
    return true;
  case X86_INS_VRCP14SS:
    Spec = {X86ApproxFloatKind::Rcp14F32, 0x4d, false, true};
    return true;
  case X86_INS_VRCP14SD:
    Spec = {X86ApproxFloatKind::Rcp14F64, 0x4d, true, true};
    return true;
  case X86_INS_VRSQRT14PS:
    Spec = {X86ApproxFloatKind::Rsqrt14F32, 0x4e, false, false};
    return true;
  case X86_INS_VRSQRT14PD:
    Spec = {X86ApproxFloatKind::Rsqrt14F64, 0x4e, true, false};
    return true;
  case X86_INS_VRSQRT14SS:
    Spec = {X86ApproxFloatKind::Rsqrt14F32, 0x4f, false, true};
    return true;
  case X86_INS_VRSQRT14SD:
    Spec = {X86ApproxFloatKind::Rsqrt14F64, 0x4f, true, true};
    return true;
  case X86_INS_VRCP28PS:
    Spec = {X86ApproxFloatKind::Rcp28F32, 0xca, false, false, true};
    return true;
  case X86_INS_VRCP28PD:
    Spec = {X86ApproxFloatKind::Rcp28F64, 0xca, true, false, true};
    return true;
  case X86_INS_VRCP28SS:
    Spec = {X86ApproxFloatKind::Rcp28F32, 0xcb, false, true, true};
    return true;
  case X86_INS_VRCP28SD:
    Spec = {X86ApproxFloatKind::Rcp28F64, 0xcb, true, true, true};
    return true;
  case X86_INS_VRSQRT28PS:
    Spec = {X86ApproxFloatKind::Rsqrt28F32, 0xcc, false, false, true};
    return true;
  case X86_INS_VRSQRT28PD:
    Spec = {X86ApproxFloatKind::Rsqrt28F64, 0xcc, true, false, true};
    return true;
  case X86_INS_VRSQRT28SS:
    Spec = {X86ApproxFloatKind::Rsqrt28F32, 0xcd, false, true, true};
    return true;
  case X86_INS_VRSQRT28SD:
    Spec = {X86ApproxFloatKind::Rsqrt28F64, 0xcd, true, true, true};
    return true;
  case X86_INS_VEXP2PS:
    Spec = {X86ApproxFloatKind::Exp2F32, 0xc8, false, false, true};
    return true;
  case X86_INS_VEXP2PD:
    Spec = {X86ApproxFloatKind::Exp2F64, 0xc8, true, false, true};
    return true;
  default:
    return false;
  }
}

bool getLaneMoveSpec(unsigned InsnId, LaneMoveSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VBROADCASTF32X2:
    Spec = {LaneMoveKind::Broadcast, 0x19, 4, 8, false, true};
    return true;
  case X86_INS_VBROADCASTF32X4:
    Spec = {LaneMoveKind::Broadcast, 0x1a, 4, 16, false, false};
    return true;
  case X86_INS_VBROADCASTF32X8:
    Spec = {LaneMoveKind::Broadcast, 0x1b, 4, 32, false, false};
    return true;
  case X86_INS_VBROADCASTF64X2:
    Spec = {LaneMoveKind::Broadcast, 0x1a, 8, 16, true, false};
    return true;
  case X86_INS_VBROADCASTF64X4:
    Spec = {LaneMoveKind::Broadcast, 0x1b, 8, 32, true, false};
    return true;
  case X86_INS_VBROADCASTI32X2:
    Spec = {LaneMoveKind::Broadcast, 0x59, 4, 8, false, true};
    return true;
  case X86_INS_VBROADCASTI32X4:
    Spec = {LaneMoveKind::Broadcast, 0x5a, 4, 16, false, false};
    return true;
  case X86_INS_VBROADCASTI32X8:
    Spec = {LaneMoveKind::Broadcast, 0x5b, 4, 32, false, false};
    return true;
  case X86_INS_VBROADCASTI64X2:
    Spec = {LaneMoveKind::Broadcast, 0x5a, 8, 16, true, false};
    return true;
  case X86_INS_VBROADCASTI64X4:
    Spec = {LaneMoveKind::Broadcast, 0x5b, 8, 32, true, false};
    return true;

  case X86_INS_VINSERTF32X4:
    Spec = {LaneMoveKind::Insert, 0x18, 4, 16, false, true};
    return true;
  case X86_INS_VINSERTF32X8:
    Spec = {LaneMoveKind::Insert, 0x1a, 4, 32, false, true};
    return true;
  case X86_INS_VINSERTF64X2:
    Spec = {LaneMoveKind::Insert, 0x18, 8, 16, true, true};
    return true;
  case X86_INS_VINSERTF64X4:
    Spec = {LaneMoveKind::Insert, 0x1a, 8, 32, true, true};
    return true;
  case X86_INS_VINSERTI32X4:
    Spec = {LaneMoveKind::Insert, 0x38, 4, 16, false, true};
    return true;
  case X86_INS_VINSERTI32X8:
    Spec = {LaneMoveKind::Insert, 0x3a, 4, 32, false, true};
    return true;
  case X86_INS_VINSERTI64X2:
    Spec = {LaneMoveKind::Insert, 0x38, 8, 16, true, true};
    return true;
  case X86_INS_VINSERTI64X4:
    Spec = {LaneMoveKind::Insert, 0x3a, 8, 32, true, true};
    return true;

  case X86_INS_VEXTRACTF32X4:
    Spec = {LaneMoveKind::Extract, 0x19, 4, 16, false, true};
    return true;
  case X86_INS_VEXTRACTF32X8:
    Spec = {LaneMoveKind::Extract, 0x1b, 4, 32, false, true};
    return true;
  case X86_INS_VEXTRACTF64X2:
    Spec = {LaneMoveKind::Extract, 0x19, 8, 16, true, true};
    return true;
  case X86_INS_VEXTRACTF64X4:
    Spec = {LaneMoveKind::Extract, 0x1b, 8, 32, true, true};
    return true;
  case X86_INS_VEXTRACTI32X4:
    Spec = {LaneMoveKind::Extract, 0x39, 4, 16, false, true};
    return true;
  case X86_INS_VEXTRACTI32X8:
    Spec = {LaneMoveKind::Extract, 0x3b, 4, 32, false, true};
    return true;
  case X86_INS_VEXTRACTI64X2:
    Spec = {LaneMoveKind::Extract, 0x39, 8, 16, true, true};
    return true;
  case X86_INS_VEXTRACTI64X4:
    Spec = {LaneMoveKind::Extract, 0x3b, 8, 32, true, true};
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

bool isValidLaneMemoryOperand(const cs_x86_op &Operand, uint16_t AddressSize,
                              uint16_t TupleSize) {
  if (Operand.type != X86_OP_MEM || Operand.size != TupleSize ||
      (AddressSize != 4 && AddressSize != 8))
    return false;
  auto IsAddressRegister = [&](x86_reg Reg) {
    if (Reg == X86_REG_INVALID)
      return true;
    if (Reg == X86_REG_RIP)
      return AddressSize == 8;
    if (Reg == X86_REG_EIP)
      return AddressSize == 4;
    const RegInfo Info = mapCapstoneReg(Reg);
    return x86reg::isGeneralRegOffset(Info.Offset) && Info.Size == AddressSize;
  };
  if (!IsAddressRegister(static_cast<x86_reg>(Operand.mem.base)) ||
      !IsAddressRegister(static_cast<x86_reg>(Operand.mem.index)) ||
      (Operand.mem.index != X86_REG_INVALID &&
       (Operand.mem.base == X86_REG_RIP || Operand.mem.base == X86_REG_EIP)))
    return false;
  return Operand.mem.scale == 1 || Operand.mem.scale == 2 ||
         Operand.mem.scale == 4 || Operand.mem.scale == 8;
}

unsigned vectorRegisterIndex(const cs_x86_op &Operand) {
  if (Operand.size == 16)
    return static_cast<unsigned>(Operand.reg - X86_REG_XMM0);
  if (Operand.size == 32)
    return static_cast<unsigned>(Operand.reg - X86_REG_YMM0);
  return static_cast<unsigned>(Operand.reg - X86_REG_ZMM0);
}

struct EvexPackedFloatBinaryInfo {
  CanonicalEvexEncodingInfo Encoding;
  const cs_x86_op *MaskOperand = nullptr;
  unsigned LeftIndex = 0;
  unsigned RightIndex = 0;
  uint16_t VectorSize = 0;
  uint16_t ElementSize = 0;
  uint16_t MaskSize = 0;
  uint16_t MemoryTupleSize = 0;
  unsigned LaneCount = 0;
  bool MemoryForm = false;
  bool Broadcast = false;
  bool ZeroMask = false;
};

bool validateEvexPackedFloatBinary(
    X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
    const cs_x86 &X86, uint8_t Opcode, bool F64,
    EvexPackedFloatBinaryInfo &Info) {
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                      Info.Encoding) ||
      (Info.Encoding.P0 & 0x07) != 0x01 ||
      ((Info.Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((F64 ? 0x80 : 0) | (F64 ? 0x01 : 0) | 0x04) ||
      Info.Encoding.Opcode != Opcode || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0 || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool HasMask = X86.op_count == 4;
  if (X86.op_count != 3 && !HasMask)
    return false;
  Info.MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  Info.LeftIndex = HasMask ? 2 : 1;
  Info.RightIndex = Info.LeftIndex + 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &Left = X86.operands[Info.LeftIndex];
  const cs_x86_op &Right = X86.operands[Info.RightIndex];
  Info.VectorSize = static_cast<uint16_t>(Destination.size);
  Info.ElementSize = F64 ? 8 : 4;
  if (Info.VectorSize != 16 && Info.VectorSize != 32 && Info.VectorSize != 64)
    return false;
  Info.LaneCount = Info.VectorSize / Info.ElementSize;
  Info.MaskSize =
      static_cast<uint16_t>(std::max(1u, (Info.LaneCount + 7u) / 8u));
  Info.MemoryForm = Right.type == X86_OP_MEM;
  Info.Broadcast = Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0;
  Info.ZeroMask = (Info.Encoding.P2 & 0x80) != 0;
  Info.MemoryTupleSize =
      Info.Broadcast ? Info.ElementSize : Info.VectorSize;

  const uint8_t EncodedLength = Info.Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Info.VectorSize == 16 ? 0 : (Info.VectorSize == 32 ? 0x20 : 0x40);
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      (((Info.Encoding.ModRM & 0xc0) != 0xc0) != Info.MemoryForm) ||
      (!Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0) ||
      !isVectorRegisterOfSize(Destination, Info.VectorSize) ||
      !isVectorRegisterOfSize(Left, Info.VectorSize) ||
      decodeEvexVectorRegIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          vectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
          vectorRegisterIndex(Left))
    return false;
  if (L.targetArch() == Arch::X86 &&
      (vectorRegisterIndex(Destination) >= 8 ||
       vectorRegisterIndex(Left) >= 8))
    return false;

  const uint8_t EncodedMask = Info.Encoding.P2 & 7;
  if (Info.MaskOperand) {
    const RegInfo MaskInfo = mapCapstoneReg(
        static_cast<x86_reg>(Info.MaskOperand->reg));
    if (!isX86OpmaskOperand(*Info.MaskOperand) ||
        Info.MaskOperand->reg == X86_REG_K0 ||
        Info.MaskOperand->size != Info.MaskSize ||
        EncodedMask !=
            static_cast<uint8_t>(Info.MaskOperand->reg - X86_REG_K0) ||
        Info.ZeroMask !=
            static_cast<bool>(Info.MaskOperand->avx_zero_opmask) ||
        MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < Info.MaskSize)
      return false;
  } else if (EncodedMask != 0 || Info.ZeroMask) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      !Info.Broadcast                  ? X86_AVX_BCAST_INVALID
      : Info.LaneCount == 2            ? X86_AVX_BCAST_2
      : Info.LaneCount == 4            ? X86_AVX_BCAST_4
      : Info.LaneCount == 8            ? X86_AVX_BCAST_8
      : Info.LaneCount == 16           ? X86_AVX_BCAST_16
                                       : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    const bool IsMask = Info.MaskOperand && &Operand == Info.MaskOperand;
    const bool IsRight = &Operand == &Right;
    if (Operand.avx_zero_opmask != (IsMask && Info.ZeroMask) ||
        Operand.avx_bcast !=
            (IsRight ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }

  if (Info.MemoryForm)
    return isValidLaneMemoryOperand(Right, S.AddressSize,
                                    Info.MemoryTupleSize) &&
           validateCanonicalEvexMemoryTail(
               Insn, X86, Info.Encoding, Right, Info.MemoryTupleSize);
  if (!isVectorRegisterOfSize(Right, Info.VectorSize) ||
      decodeEvexVectorRMIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          vectorRegisterIndex(Right) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Info.Encoding))
    return false;
  return L.targetArch() != Arch::X86 || vectorRegisterIndex(Right) < 8;
}

NdVar evexPackedActiveMask(X86Lifter::LiftState &S,
                           const EvexPackedFloatBinaryInfo &Info) {
  if (!Info.MaskOperand)
    return NdVar::cst((UINT64_C(1) << Info.LaneCount) - UINT64_C(1),
                      Info.MaskSize);
  const RegInfo MaskInfo =
      mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
  if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < Info.MaskSize)
    return {};
  return NdVar::reg(MaskInfo.Offset, Info.MaskSize);
}

bool hasCanonicalFPControlEncoding(const cs_insn *Insn, const cs_x86 &X86,
                                   Arch TargetArch, const FPControlSpec &Spec) {
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, TargetArch, Encoding) ||
      Encoding.Offset + 7 > Insn->size || (Encoding.P0 & 0x07) != 3 ||
      Encoding.Opcode != Spec.Opcode ||
      ((Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((Spec.W ? 0x80 : 0) | 0x05) ||
      X86.encoding.imm_size != 1 || X86.encoding.imm_offset != Insn->size - 1)
    return false;

  const unsigned EncodedMask = Encoding.P2 & 7;
  const bool HasMask = EncodedMask != 0;
  const bool ZeroMask = (Encoding.P2 & 0x80) != 0;
  const bool EncodedB = (Encoding.P2 & 0x10) != 0;
  unsigned OperandIndex = 0;
  if (X86.op_count == 0)
    return false;
  const cs_x86_op &Destination = X86.operands[OperandIndex++];
  if (!isVectorRegisterOfSize(Destination, Destination.size) ||
      (Spec.Scalar ? Destination.size != 16
                   : (Destination.size != 16 && Destination.size != 32 &&
                      Destination.size != 64)) ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(Destination))
    return false;

  if (HasMask) {
    if (OperandIndex >= X86.op_count ||
        !isX86OpmaskOperand(X86.operands[OperandIndex]))
      return false;
    const cs_x86_op &Mask = X86.operands[OperandIndex++];
    const unsigned LaneCount =
        Spec.Scalar ? 1 : Destination.size / Spec.ElementSize;
    const uint16_t MaskSize =
        static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
    if (Mask.reg == X86_REG_K0 ||
        EncodedMask != static_cast<unsigned>(Mask.reg - X86_REG_K0) ||
        Mask.size != MaskSize || Mask.avx_zero_opmask != ZeroMask)
      return false;
  } else if (ZeroMask) {
    return false;
  }

  if (Spec.HasMergeSource) {
    if (OperandIndex >= X86.op_count ||
        !isVectorRegisterOfSize(X86.operands[OperandIndex], Destination.size) ||
        decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
            vectorRegisterIndex(X86.operands[OperandIndex]))
      return false;
    ++OperandIndex;
  } else if ((Encoding.P1 & 0x78) != 0x78 || (Encoding.P2 & 0x08) == 0) {
    return false;
  }

  if (OperandIndex + 2 != X86.op_count)
    return false;
  const cs_x86_op &Source = X86.operands[OperandIndex++];
  const cs_x86_op &Immediate = X86.operands[OperandIndex];
  if (Immediate.type != X86_OP_IMM || Immediate.size != 1 ||
      static_cast<uint8_t>(Immediate.imm) != Insn->bytes[Insn->size - 1])
    return false;

  const bool MemoryForm = (Encoding.ModRM & 0xc0) != 0xc0;
  const bool Broadcast = MemoryForm && EncodedB && !Spec.Scalar;
  if (MemoryForm != (Source.type == X86_OP_MEM) ||
      (!MemoryForm && !isVectorRegisterOfSize(Source, Destination.size)))
    return false;
  if (MemoryForm) {
    const uint16_t MemorySize =
        (Spec.Scalar || Broadcast) ? Spec.ElementSize : Destination.size;
    const unsigned BroadcastLaneCount = Destination.size / Spec.ElementSize;
    const x86_avx_bcast ExpectedBroadcast =
        !Broadcast                 ? X86_AVX_BCAST_INVALID
        : BroadcastLaneCount == 2  ? X86_AVX_BCAST_2
        : BroadcastLaneCount == 4  ? X86_AVX_BCAST_4
        : BroadcastLaneCount == 8  ? X86_AVX_BCAST_8
        : BroadcastLaneCount == 16 ? X86_AVX_BCAST_16
                                   : X86_AVX_BCAST_INVALID;
    if ((Spec.Scalar && EncodedB) || Source.size != MemorySize ||
        Source.avx_bcast != ExpectedBroadcast ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, Source,
                                         MemorySize, 1))
      return false;
  } else if (Source.avx_bcast != X86_AVX_BCAST_INVALID ||
             !validateCanonicalEvexRegisterTail(Insn, X86, Encoding, 1) ||
             decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 vectorRegisterIndex(Source)) {
    return false;
  }

  const bool SuppressExceptions = !MemoryForm && EncodedB;
  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Destination.size == 16 ? 0 : (Destination.size == 32 ? 0x20 : 0x40);
  if ((SuppressExceptions && !Spec.Scalar && Destination.size != 64) ||
      (SuppressExceptions ? EncodedLength != 0
                          : EncodedLength != ExpectedLength))
    return false;
  if (X86.avx_sae != SuppressExceptions || X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const bool IsMask = HasMask && Index == 1;
    const bool IsSource = &X86.operands[Index] == &Source;
    if (X86.operands[Index].avx_zero_opmask != (IsMask && ZeroMask) ||
        (!IsSource && X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID))
      return false;
  }
  return true;
}

bool liftEvexFPImmediateUnary(X86Lifter &L, X86Lifter::LiftState &S,
                              const cs_insn *Insn, const cs_x86 &X86) {
  FPControlSpec Spec;
  if (!Insn || !getFPControlSpec(Insn->id, Spec) ||
      (Insn->id != X86_INS_VREDUCEPS && Insn->id != X86_INS_VREDUCEPD &&
       Insn->id != X86_INS_VREDUCESS && Insn->id != X86_INS_VREDUCESD &&
       Insn->id != X86_INS_VRNDSCALEPS &&
       Insn->id != X86_INS_VRNDSCALEPD &&
       Insn->id != X86_INS_VRNDSCALESS &&
       Insn->id != X86_INS_VRNDSCALESD &&
       Insn->id != X86_INS_VGETMANTPS &&
       Insn->id != X86_INS_VGETMANTPD &&
       Insn->id != X86_INS_VGETMANTSS &&
       Insn->id != X86_INS_VGETMANTSD) ||
      !hasCanonicalFPControlEncoding(Insn, X86, L.targetArch(), Spec))
    return false;

  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding))
    return false;
  const unsigned BaseOperandCount = Spec.HasMergeSource ? 4 : 3;
  const bool HasMask = X86.op_count == BaseOperandCount + 1;
  const unsigned FirstSourceIndex = HasMask ? 2 : 1;
  const unsigned SourceIndex =
      Spec.HasMergeSource ? FirstSourceIndex + 1 : FirstSourceIndex;
  const unsigned ImmediateIndex = SourceIndex + 1;
  if (X86.op_count != BaseOperandCount && !HasMask)
    return false;

  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op *MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  const cs_x86_op *MergeOperand =
      Spec.HasMergeSource ? &X86.operands[FirstSourceIndex] : nullptr;
  const cs_x86_op &SourceOperand = X86.operands[SourceIndex];
  const cs_x86_op &ImmediateOperand = X86.operands[ImmediateIndex];
  const uint16_t VectorSize = static_cast<uint16_t>(DestinationOperand.size);
  const unsigned LaneCount = Spec.Scalar ? 1 : VectorSize / Spec.ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  const bool MemoryForm = SourceOperand.type == X86_OP_MEM;
  const bool Broadcast = MemoryForm && (Encoding.P2 & 0x10) != 0;
  const bool SuppressExceptions = !MemoryForm && (Encoding.P2 & 0x10) != 0;
  const uint16_t MemoryTupleSize =
      (Spec.Scalar || Broadcast) ? Spec.ElementSize : VectorSize;

  NdVar ActiveMask = NdVar::cst(
      LaneCount == 64 ? UINT64_MAX : ((UINT64_C(1) << LaneCount) - 1),
      MaskSize);
  if (MaskOperand) {
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < MaskSize)
      return false;
    ActiveMask = NdVar::reg(MaskInfo.Offset, MaskSize);
  }
  if (Spec.Scalar && MaskOperand) {
    NdVar LowMask = S.makeTemp(1);
    S.emit(NdOp::INT_AND, LowMask,
           {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
    ActiveMask = LowMask;
  }

  NdVar Source;
  if (MemoryForm) {
    Source = emitEvexMaskedMemoryLoad(S, SourceOperand, ActiveMask, VectorSize,
                                      Spec.ElementSize, MemoryTupleSize,
                                      Broadcast);
  } else {
    Source = L.operandRead(S, SourceOperand);
  }
  if (Source.Size != VectorSize)
    return false;

  const bool GetMantissa =
      Insn->id == X86_INS_VGETMANTPS || Insn->id == X86_INS_VGETMANTPD ||
      Insn->id == X86_INS_VGETMANTSS || Insn->id == X86_INS_VGETMANTSD;
  const auto RoundKind =
      Insn->id == X86_INS_VREDUCEPS || Insn->id == X86_INS_VREDUCEPD ||
              Insn->id == X86_INS_VREDUCESS || Insn->id == X86_INS_VREDUCESD
          ? X86FPRoundTransformKind::Reduce
          : X86FPRoundTransformKind::RoundScale;
  const Intrinsic IntrinsicId =
      GetMantissa ? Intrinsic::X86FPExtract
                  : Intrinsic::X86FPRoundTransform;
  const uint8_t Control =
      GetMantissa
          ? makeX86FPExtractControl(X86FPExtractKind::Mantissa,
                                    Spec.ElementSize == 8, Spec.Scalar,
                                    SuppressExceptions)
          : makeX86FPRoundTransformControl(
                RoundKind, Spec.ElementSize == 8, Spec.Scalar,
                SuppressExceptions);
  NdVar Raw = S.makeTemp(VectorSize);
  S.emitIntrinsic(
      IntrinsicId, Raw,
      {NdVar::cst(Control, 1), Source,
       NdVar::cst(static_cast<uint8_t>(ImmediateOperand.imm), 1), ActiveMask});

  if (!Spec.Scalar) {
    if (MaskOperand)
      return emitMaskedVectorResult(L, S, DestinationOperand, *MaskOperand,
                                    Raw, Spec.ElementSize);
    const NdVar Destination = L.operandWrite(DestinationOperand);
    S.emit(NdOp::COPY, Destination, {Raw});
    return true;
  }

  if (!MergeOperand)
    return false;
  const NdVar Merge = L.operandRead(S, *MergeOperand);
  const NdVar Destination = L.operandWrite(DestinationOperand);
  if (Merge.Size != 16 || Destination.Size != 16)
    return false;
  NdVar Low = S.makeTemp(Spec.ElementSize);
  S.emit(NdOp::SUBBYTES, Low, {Raw, NdVar::cst(0, 4)});
  if (MaskOperand) {
    NdVar Inactive = NdVar::cst(0, Spec.ElementSize);
    if (!MaskOperand->avx_zero_opmask) {
      const NdVar OldDestination = L.operandRead(S, DestinationOperand);
      Inactive = S.makeTemp(Spec.ElementSize);
      S.emit(NdOp::SUBBYTES, Inactive,
             {OldDestination, NdVar::cst(0, 4)});
    }
    NdVar Selected = S.makeTemp(Spec.ElementSize);
    S.emit(NdOp::SELECT, Selected, {ActiveMask, Low, Inactive});
    Low = Selected;
  }
  NdVar Upper = S.makeTemp(16 - Spec.ElementSize);
  S.emit(NdOp::SUBBYTES, Upper,
         {Merge, NdVar::cst(Spec.ElementSize, 4)});
  S.emit(NdOp::CONCAT, Destination, {Upper, Low});
  return true;
}

bool liftEvexFPRange(X86Lifter &L, X86Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_x86 &X86) {
  FPControlSpec Spec;
  if (!Insn || !getFPControlSpec(Insn->id, Spec) ||
      (Insn->id != X86_INS_VRANGEPS && Insn->id != X86_INS_VRANGEPD &&
       Insn->id != X86_INS_VRANGESS && Insn->id != X86_INS_VRANGESD) ||
      !hasCanonicalFPControlEncoding(Insn, X86, L.targetArch(), Spec))
    return false;

  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding))
    return false;
  const bool HasMask = X86.op_count == 5;
  if (X86.op_count != 4 && !HasMask)
    return false;
  const unsigned FirstSourceIndex = HasMask ? 2 : 1;
  const unsigned SecondSourceIndex = FirstSourceIndex + 1;
  const unsigned ImmediateIndex = SecondSourceIndex + 1;
  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op *MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  const cs_x86_op &FirstSourceOperand = X86.operands[FirstSourceIndex];
  const cs_x86_op &SecondSourceOperand = X86.operands[SecondSourceIndex];
  const cs_x86_op &ImmediateOperand = X86.operands[ImmediateIndex];
  const uint8_t Immediate = static_cast<uint8_t>(ImmediateOperand.imm);
  if ((Immediate & 0xf0U) != 0)
    return false;

  const uint16_t VectorSize =
      static_cast<uint16_t>(DestinationOperand.size);
  const unsigned LaneCount = Spec.Scalar ? 1 : VectorSize / Spec.ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  NdVar ActiveMask = NdVar::cst(
      LaneCount == 64 ? UINT64_MAX : ((UINT64_C(1) << LaneCount) - 1),
      MaskSize);
  if (MaskOperand) {
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < MaskSize)
      return false;
    ActiveMask = NdVar::reg(MaskInfo.Offset, MaskSize);
  }
  if (Spec.Scalar && MaskOperand) {
    NdVar LowMask = S.makeTemp(1);
    S.emit(NdOp::INT_AND, LowMask,
           {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
    ActiveMask = LowMask;
  }

  const bool MemoryForm = SecondSourceOperand.type == X86_OP_MEM;
  const bool Broadcast =
      MemoryForm && !Spec.Scalar && (Encoding.P2 & 0x10) != 0;
  const bool SuppressExceptions =
      !MemoryForm && (Encoding.P2 & 0x10) != 0;
  const uint16_t MemoryTupleSize =
      (Spec.Scalar || Broadcast) ? Spec.ElementSize : VectorSize;
  const NdVar FirstSource = L.operandRead(S, FirstSourceOperand);
  NdVar SecondSource;
  if (MemoryForm) {
    SecondSource = emitEvexMaskedMemoryLoad(
        S, SecondSourceOperand, ActiveMask, VectorSize, Spec.ElementSize,
        MemoryTupleSize, Broadcast);
  } else {
    SecondSource = L.operandRead(S, SecondSourceOperand);
  }
  if (FirstSource.Size != VectorSize || SecondSource.Size != VectorSize)
    return false;

  NdVar Raw = S.makeTemp(VectorSize);
  const uint8_t Control = makeX86FPRangeControl(
      Spec.ElementSize == 8, Spec.Scalar, SuppressExceptions);
  S.emitIntrinsic(Intrinsic::X86FPRange, Raw,
                  {NdVar::cst(Control, 1), FirstSource, SecondSource,
                   NdVar::cst(Immediate, 1), ActiveMask});

  if (!Spec.Scalar) {
    if (MaskOperand)
      return emitMaskedVectorResult(L, S, DestinationOperand, *MaskOperand,
                                    Raw, Spec.ElementSize);
    const NdVar Destination = L.operandWrite(DestinationOperand);
    S.emit(NdOp::COPY, Destination, {Raw});
    return true;
  }

  const NdVar Destination = L.operandWrite(DestinationOperand);
  if (Destination.Size != 16 || FirstSource.Size != 16)
    return false;
  NdVar Low = S.makeTemp(Spec.ElementSize);
  S.emit(NdOp::SUBBYTES, Low, {Raw, NdVar::cst(0, 4)});
  if (MaskOperand) {
    NdVar Inactive = NdVar::cst(0, Spec.ElementSize);
    if (!MaskOperand->avx_zero_opmask) {
      const NdVar OldDestination = L.operandRead(S, DestinationOperand);
      Inactive = S.makeTemp(Spec.ElementSize);
      S.emit(NdOp::SUBBYTES, Inactive,
             {OldDestination, NdVar::cst(0, 4)});
    }
    NdVar Selected = S.makeTemp(Spec.ElementSize);
    S.emit(NdOp::SELECT, Selected, {ActiveMask, Low, Inactive});
    Low = Selected;
  }
  NdVar Upper = S.makeTemp(16 - Spec.ElementSize);
  S.emit(NdOp::SUBBYTES, Upper,
         {FirstSource, NdVar::cst(Spec.ElementSize, 4)});
  S.emit(NdOp::CONCAT, Destination, {Upper, Low});
  return true;
}

bool liftEvexFPFixup(X86Lifter &L, X86Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_x86 &X86) {
  FPControlSpec Spec;
  if (!Insn || !getFPControlSpec(Insn->id, Spec) ||
      (Insn->id != X86_INS_VFIXUPIMMPS &&
       Insn->id != X86_INS_VFIXUPIMMPD &&
       Insn->id != X86_INS_VFIXUPIMMSS &&
       Insn->id != X86_INS_VFIXUPIMMSD) ||
      !hasCanonicalFPControlEncoding(Insn, X86, L.targetArch(), Spec))
    return false;

  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding))
    return false;
  const bool HasMask = X86.op_count == 5;
  if (X86.op_count != 4 && !HasMask)
    return false;
  const unsigned FirstSourceIndex = HasMask ? 2 : 1;
  const unsigned TableIndex = FirstSourceIndex + 1;
  const unsigned ImmediateIndex = TableIndex + 1;
  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op *MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  const cs_x86_op &FirstSourceOperand = X86.operands[FirstSourceIndex];
  const cs_x86_op &TableOperand = X86.operands[TableIndex];
  const cs_x86_op &ImmediateOperand = X86.operands[ImmediateIndex];
  const uint16_t VectorSize =
      static_cast<uint16_t>(DestinationOperand.size);
  const unsigned LaneCount = Spec.Scalar ? 1 : VectorSize / Spec.ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  NdVar ActiveMask = NdVar::cst(
      LaneCount == 64 ? UINT64_MAX : ((UINT64_C(1) << LaneCount) - 1),
      MaskSize);
  if (MaskOperand) {
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < MaskSize)
      return false;
    ActiveMask = NdVar::reg(MaskInfo.Offset, MaskSize);
  }
  if (Spec.Scalar && MaskOperand) {
    NdVar LowMask = S.makeTemp(1);
    S.emit(NdOp::INT_AND, LowMask,
           {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
    ActiveMask = LowMask;
  }

  const bool MemoryForm = TableOperand.type == X86_OP_MEM;
  const bool Broadcast =
      MemoryForm && !Spec.Scalar && (Encoding.P2 & 0x10) != 0;
  const bool SuppressExceptions =
      !MemoryForm && (Encoding.P2 & 0x10) != 0;
  const uint16_t MemoryTupleSize =
      (Spec.Scalar || Broadcast) ? Spec.ElementSize : VectorSize;
  const NdVar OldDestination = L.operandRead(S, DestinationOperand);
  const NdVar FirstSource = L.operandRead(S, FirstSourceOperand);
  NdVar Table;
  if (MemoryForm) {
    Table = emitEvexMaskedMemoryLoad(S, TableOperand, ActiveMask, VectorSize,
                                    Spec.ElementSize, MemoryTupleSize,
                                    Broadcast);
  } else {
    Table = L.operandRead(S, TableOperand);
  }
  if (OldDestination.Size != VectorSize || FirstSource.Size != VectorSize ||
      Table.Size != VectorSize)
    return false;

  NdVar Raw = S.makeTemp(VectorSize);
  const uint16_t Control = static_cast<uint16_t>(makeX86FPFixupControl(
                               Spec.ElementSize == 8, Spec.Scalar,
                               SuppressExceptions)) |
                           (static_cast<uint16_t>(
                                static_cast<uint8_t>(ImmediateOperand.imm))
                            << 8);
  S.emitIntrinsic(
      Intrinsic::X86FPFixup, Raw,
      {NdVar::cst(Control, 2), OldDestination, FirstSource, Table, ActiveMask});

  if (!Spec.Scalar) {
    if (MaskOperand)
      return emitMaskedVectorResult(L, S, DestinationOperand, *MaskOperand,
                                    Raw, Spec.ElementSize);
    const NdVar Destination = L.operandWrite(DestinationOperand);
    S.emit(NdOp::COPY, Destination, {Raw});
    return true;
  }

  const NdVar Destination = L.operandWrite(DestinationOperand);
  if (Destination.Size != 16 || FirstSource.Size != 16)
    return false;
  NdVar Low = S.makeTemp(Spec.ElementSize);
  S.emit(NdOp::SUBBYTES, Low, {Raw, NdVar::cst(0, 4)});
  if (MaskOperand) {
    NdVar Inactive = NdVar::cst(0, Spec.ElementSize);
    if (!MaskOperand->avx_zero_opmask) {
      Inactive = S.makeTemp(Spec.ElementSize);
      S.emit(NdOp::SUBBYTES, Inactive,
             {OldDestination, NdVar::cst(0, 4)});
    }
    NdVar Selected = S.makeTemp(Spec.ElementSize);
    S.emit(NdOp::SELECT, Selected, {ActiveMask, Low, Inactive});
    Low = Selected;
  }
  NdVar Upper = S.makeTemp(16 - Spec.ElementSize);
  S.emit(NdOp::SUBBYTES, Upper,
         {FirstSource, NdVar::cst(Spec.ElementSize, 4)});
  S.emit(NdOp::CONCAT, Destination, {Upper, Low});
  return true;
}

bool liftEvexFPScale(X86Lifter &L, X86Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_x86 &X86) {
  if (!Insn || (Insn->id != X86_INS_VSCALEFPS &&
                Insn->id != X86_INS_VSCALEFPD &&
                Insn->id != X86_INS_VSCALEFSS &&
                Insn->id != X86_INS_VSCALEFSD))
    return false;
  const bool Scalar =
      Insn->id == X86_INS_VSCALEFSS || Insn->id == X86_INS_VSCALEFSD;
  const bool F64 =
      Insn->id == X86_INS_VSCALEFPD || Insn->id == X86_INS_VSCALEFSD;
  const uint16_t ElementSize = F64 ? 8 : 4;

  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x02 ||
      ((Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((F64 ? 0x80 : 0) | 0x05) ||
      Encoding.Opcode != (Scalar ? 0x2d : 0x2c) ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0)
    return false;

  const bool HasMask = X86.op_count == 4;
  if (X86.op_count != 3 && !HasMask)
    return false;
  const unsigned FirstSourceIndex = HasMask ? 2 : 1;
  const unsigned SecondSourceIndex = FirstSourceIndex + 1;
  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op *MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  const cs_x86_op &FirstSourceOperand = X86.operands[FirstSourceIndex];
  const cs_x86_op &SecondSourceOperand = X86.operands[SecondSourceIndex];
  const bool MemoryForm = SecondSourceOperand.type == X86_OP_MEM;
  const bool EncodedB = (Encoding.P2 & 0x10) != 0;
  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  const bool EmbeddedRounding = !MemoryForm && EncodedB;
  const bool Broadcast = MemoryForm && EncodedB && !Scalar;
  const uint16_t VectorSize =
      Scalar             ? 16
      : EmbeddedRounding ? 64
      : EncodedLength == 0
          ? 16
      : EncodedLength == 0x20 ? 32
                              : 64;
  if (((Encoding.ModRM & 0xc0) != 0xc0) != MemoryForm ||
      (Scalar && !EmbeddedRounding && EncodedLength != 0) ||
      (Scalar && MemoryForm && EncodedB) ||
      (!Scalar && !EmbeddedRounding && EncodedLength == 0x60) ||
      (!Scalar && EmbeddedRounding && DestinationOperand.size != 64))
    return false;

  const X86FPRounding Rounding =
      EmbeddedRounding
          ? static_cast<X86FPRounding>(EncodedLength >> 5)
          : X86FPRounding::MXCSR;
  const x86_avx_rm ExpectedRounding =
      EmbeddedRounding
          ? static_cast<x86_avx_rm>(X86_AVX_RM_RN + (EncodedLength >> 5))
          : X86_AVX_RM_INVALID;
  if (X86.avx_sae != EmbeddedRounding || X86.avx_rm != ExpectedRounding ||
      !isVectorRegisterOfSize(DestinationOperand, VectorSize) ||
      !isVectorRegisterOfSize(FirstSourceOperand, VectorSize) ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(DestinationOperand) ||
      decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
          vectorRegisterIndex(FirstSourceOperand))
    return false;

  const unsigned LaneCount = Scalar ? 1 : VectorSize / ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  const uint8_t EncodedMask = Encoding.P2 & 7;
  const bool EncodedZero = (Encoding.P2 & 0x80) != 0;
  NdVar ActiveMask = NdVar::cst(
      (UINT64_C(1) << LaneCount) - UINT64_C(1), MaskSize);
  if (MaskOperand) {
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand->reg));
    if (!isX86OpmaskOperand(*MaskOperand) ||
        MaskOperand->reg == X86_REG_K0 || MaskOperand->size != MaskSize ||
        EncodedMask != static_cast<uint8_t>(MaskOperand->reg - X86_REG_K0) ||
        EncodedZero != static_cast<bool>(MaskOperand->avx_zero_opmask) ||
        MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < MaskSize)
      return false;
    ActiveMask = NdVar::reg(MaskInfo.Offset, MaskSize);
  } else if (EncodedMask != 0 || EncodedZero) {
    return false;
  }
  if (Scalar && MaskOperand) {
    NdVar LowMask = S.makeTemp(1);
    S.emit(NdOp::INT_AND, LowMask,
           {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
    ActiveMask = LowMask;
  }

  const uint16_t MemoryTupleSize =
      (Scalar || Broadcast) ? ElementSize : VectorSize;
  const x86_avx_bcast ExpectedBroadcast =
      !Broadcast                 ? X86_AVX_BCAST_INVALID
      : LaneCount == 2           ? X86_AVX_BCAST_2
      : LaneCount == 4           ? X86_AVX_BCAST_4
      : LaneCount == 8           ? X86_AVX_BCAST_8
      : LaneCount == 16          ? X86_AVX_BCAST_16
                                 : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    const bool IsMask = MaskOperand && &Operand == MaskOperand;
    const bool IsSecondSource = &Operand == &SecondSourceOperand;
    if (Operand.avx_zero_opmask != (IsMask && EncodedZero) ||
        Operand.avx_bcast !=
            (IsSecondSource ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }
  if (MemoryForm) {
    if (!isValidLaneMemoryOperand(SecondSourceOperand, S.AddressSize,
                                  MemoryTupleSize) ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding,
                                         SecondSourceOperand,
                                         MemoryTupleSize))
      return false;
  } else if (!isVectorRegisterOfSize(SecondSourceOperand, VectorSize) ||
             decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 vectorRegisterIndex(SecondSourceOperand) ||
             !validateCanonicalEvexRegisterTail(Insn, X86, Encoding)) {
    return false;
  }

  const NdVar FirstSource = L.operandRead(S, FirstSourceOperand);
  NdVar SecondSource;
  if (MemoryForm) {
    SecondSource = emitEvexMaskedMemoryLoad(
        S, SecondSourceOperand, ActiveMask, VectorSize, ElementSize,
        MemoryTupleSize, Broadcast);
  } else {
    SecondSource = L.operandRead(S, SecondSourceOperand);
  }
  if (FirstSource.Size != VectorSize || SecondSource.Size != VectorSize)
    return false;

  const uint8_t Control = makeX86FPScaleControl(
      F64, Scalar, EmbeddedRounding, Rounding);
  NdVar Raw = S.makeTemp(VectorSize);
  S.emitIntrinsic(Intrinsic::X86FPScale, Raw,
                  {NdVar::cst(Control, 1), FirstSource, SecondSource,
                   ActiveMask});
  if (!Scalar) {
    if (MaskOperand)
      return emitMaskedVectorResult(L, S, DestinationOperand, *MaskOperand,
                                    Raw, ElementSize);
    const NdVar Destination = L.operandWrite(DestinationOperand);
    S.emit(NdOp::COPY, Destination, {Raw});
    return true;
  }

  const NdVar Destination = L.operandWrite(DestinationOperand);
  if (Destination.Size != 16)
    return false;
  NdVar Low = S.makeTemp(ElementSize);
  S.emit(NdOp::SUBBYTES, Low, {Raw, NdVar::cst(0, 4)});
  if (MaskOperand) {
    NdVar Inactive = NdVar::cst(0, ElementSize);
    if (!MaskOperand->avx_zero_opmask) {
      const NdVar OldDestination = L.operandRead(S, DestinationOperand);
      Inactive = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, Inactive,
             {OldDestination, NdVar::cst(0, 4)});
    }
    NdVar Selected = S.makeTemp(ElementSize);
    S.emit(NdOp::SELECT, Selected, {ActiveMask, Low, Inactive});
    Low = Selected;
  }
  NdVar Upper = S.makeTemp(16 - ElementSize);
  S.emit(NdOp::SUBBYTES, Upper,
         {FirstSource, NdVar::cst(ElementSize, 4)});
  S.emit(NdOp::CONCAT, Destination, {Upper, Low});
  return true;
}

bool liftEvexFPLogic(X86Lifter &L, X86Lifter::LiftState &S,
                     const cs_insn *Insn, const cs_x86 &X86) {
  if (!Insn || (Insn->id != X86_INS_VANDPS &&
                Insn->id != X86_INS_VANDPD &&
                Insn->id != X86_INS_VANDNPS &&
                Insn->id != X86_INS_VANDNPD &&
                Insn->id != X86_INS_VORPS && Insn->id != X86_INS_VORPD &&
                Insn->id != X86_INS_VXORPS && Insn->id != X86_INS_VXORPD))
    return false;
  const bool F64 = Insn->id == X86_INS_VANDPD ||
                   Insn->id == X86_INS_VANDNPD ||
                   Insn->id == X86_INS_VORPD || Insn->id == X86_INS_VXORPD;
  const bool AndNot =
      Insn->id == X86_INS_VANDNPS || Insn->id == X86_INS_VANDNPD;
  const uint8_t Opcode =
      Insn->id == X86_INS_VANDPS || Insn->id == X86_INS_VANDPD     ? 0x54
      : AndNot                                                     ? 0x55
      : Insn->id == X86_INS_VORPS || Insn->id == X86_INS_VORPD    ? 0x56
                                                                   : 0x57;
  EvexPackedFloatBinaryInfo Info;
  if (!validateEvexPackedFloatBinary(L, S, Insn, X86, Opcode, F64, Info))
    return false;
  NdVar ActiveMask = evexPackedActiveMask(S, Info);
  if (ActiveMask.Size != Info.MaskSize)
    return false;

  const cs_x86_op &LeftOperand = X86.operands[Info.LeftIndex];
  const cs_x86_op &RightOperand = X86.operands[Info.RightIndex];
  const NdVar Left = L.operandRead(S, LeftOperand);
  NdVar Right;
  if (Info.MemoryForm) {
    Right = emitEvexMaskedMemoryLoad(
        S, RightOperand, ActiveMask, Info.VectorSize, Info.ElementSize,
        Info.MemoryTupleSize, Info.Broadcast);
  } else {
    Right = L.operandRead(S, RightOperand);
  }
  if (Left.Size != Info.VectorSize || Right.Size != Info.VectorSize)
    return false;

  NdVar Raw = S.makeTemp(Info.VectorSize);
  if (AndNot) {
    NdVar Inverted = S.makeTemp(Info.VectorSize);
    S.emit(NdOp::INT_NOT, Inverted, {Left});
    S.emit(NdOp::INT_AND, Raw, {Inverted, Right});
  } else {
    const NdOp Opcode =
        Insn->id == X86_INS_VANDPS || Insn->id == X86_INS_VANDPD
            ? NdOp::INT_AND
        : Insn->id == X86_INS_VORPS || Insn->id == X86_INS_VORPD
            ? NdOp::INT_OR
            : NdOp::INT_XOR;
    S.emit(Opcode, Raw, {Left, Right});
  }
  if (Info.MaskOperand)
    return emitMaskedVectorResult(L, S, X86.operands[0], *Info.MaskOperand,
                                  Raw, Info.ElementSize);
  S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {Raw});
  return true;
}

bool liftEvexFPUnpack(X86Lifter &L, X86Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_x86 &X86) {
  if (!Insn || (Insn->id != X86_INS_VUNPCKLPS &&
                Insn->id != X86_INS_VUNPCKLPD &&
                Insn->id != X86_INS_VUNPCKHPS &&
                Insn->id != X86_INS_VUNPCKHPD))
    return false;
  const bool F64 =
      Insn->id == X86_INS_VUNPCKLPD || Insn->id == X86_INS_VUNPCKHPD;
  const bool HighHalf =
      Insn->id == X86_INS_VUNPCKHPS || Insn->id == X86_INS_VUNPCKHPD;
  EvexPackedFloatBinaryInfo Info;
  if (!validateEvexPackedFloatBinary(L, S, Insn, X86,
                                     HighHalf ? 0x15 : 0x14, F64, Info))
    return false;
  NdVar ActiveMask = evexPackedActiveMask(S, Info);
  if (ActiveMask.Size != Info.MaskSize)
    return false;

  const cs_x86_op &LeftOperand = X86.operands[Info.LeftIndex];
  const cs_x86_op &RightOperand = X86.operands[Info.RightIndex];
  const NdVar Left = L.operandRead(S, LeftOperand);
  NdVar Right;
  if (!Info.MemoryForm) {
    Right = L.operandRead(S, RightOperand);
  } else if (Info.Broadcast) {
    uint64_t RightResultLanes = 0;
    for (unsigned Lane = 1; Lane < Info.LaneCount; Lane += 2)
      RightResultLanes |= UINT64_C(1) << Lane;
    NdVar LoadMask = S.makeTemp(Info.MaskSize);
    S.emit(NdOp::INT_AND, LoadMask,
           {ActiveMask, NdVar::cst(RightResultLanes, Info.MaskSize)});
    Right = emitEvexMaskedMemoryLoad(
        S, RightOperand, LoadMask, Info.VectorSize, Info.ElementSize,
        Info.ElementSize, true);
  } else {
    const NdVar SourceMask = emitPackedUnpackMemoryMask(
        S, Info.MaskOperand, Info.VectorSize, Info.ElementSize, HighHalf);
    const Intrinsic LoadId = maskedVectorLoadIntrinsic(Info.ElementSize);
    if (SourceMask.Size != Info.VectorSize || LoadId == Intrinsic::None)
      return false;
    Right = S.makeTemp(Info.VectorSize);
    S.emitIntrinsic(
        LoadId, Right, {S.computeEA(RightOperand), SourceMask},
        NdMemoryOrdering::None,
        X86Lifter::LiftState::memoryAddressSpace(RightOperand));
  }
  if (Left.Size != Info.VectorSize || Right.Size != Info.VectorSize)
    return false;

  NdVar Raw = Info.MaskOperand ? S.makeTemp(Info.VectorSize)
                               : L.operandWrite(X86.operands[0]);
  if (!emitPackedUnpack(S, Raw, Left, Right, Info.ElementSize, HighHalf))
    return false;
  if (Info.MaskOperand)
    return emitMaskedVectorResult(L, S, X86.operands[0], *Info.MaskOperand,
                                  Raw, Info.ElementSize);
  return true;
}

bool liftEvexFPExponent(X86Lifter &L, X86Lifter::LiftState &S,
                        const cs_insn *Insn, const cs_x86 &X86) {
  if (!Insn || (Insn->id != X86_INS_VGETEXPPS &&
                Insn->id != X86_INS_VGETEXPPD &&
                Insn->id != X86_INS_VGETEXPSS &&
                Insn->id != X86_INS_VGETEXPSD))
    return false;
  const bool Scalar =
      Insn->id == X86_INS_VGETEXPSS || Insn->id == X86_INS_VGETEXPSD;
  const bool F64 =
      Insn->id == X86_INS_VGETEXPPD || Insn->id == X86_INS_VGETEXPSD;
  const uint16_t ElementSize = F64 ? 8 : 4;

  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x02 ||
      ((Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((F64 ? 0x80 : 0) | 0x05) ||
      Encoding.Opcode != (Scalar ? 0x43 : 0x42) ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0)
    return false;

  const unsigned BaseOperandCount = Scalar ? 3 : 2;
  const bool HasMask = X86.op_count == BaseOperandCount + 1;
  if (X86.op_count != BaseOperandCount && !HasMask)
    return false;
  const unsigned FirstSourceIndex = HasMask ? 2 : 1;
  const unsigned SourceIndex = Scalar ? FirstSourceIndex + 1
                                      : FirstSourceIndex;
  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op *MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  const cs_x86_op *MergeOperand =
      Scalar ? &X86.operands[FirstSourceIndex] : nullptr;
  const cs_x86_op &SourceOperand = X86.operands[SourceIndex];
  const uint16_t VectorSize = static_cast<uint16_t>(DestinationOperand.size);
  const bool MemoryForm = SourceOperand.type == X86_OP_MEM;
  const bool EncodedB = (Encoding.P2 & 0x10) != 0;
  const bool Broadcast = MemoryForm && EncodedB && !Scalar;
  const bool SuppressExceptions = !MemoryForm && EncodedB;
  const uint16_t MemoryTupleSize =
      (Scalar || Broadcast) ? ElementSize : VectorSize;
  if (!isVectorRegisterOfSize(DestinationOperand, VectorSize) ||
      (Scalar ? VectorSize != 16
              : (VectorSize != 16 && VectorSize != 32 && VectorSize != 64)) ||
      (MergeOperand && !isVectorRegisterOfSize(*MergeOperand, 16)) ||
      (MemoryForm &&
       !isValidLaneMemoryOperand(SourceOperand, S.AddressSize,
                                 MemoryTupleSize)) ||
      (!MemoryForm && !isVectorRegisterOfSize(SourceOperand, VectorSize)) ||
      (Scalar && MemoryForm && EncodedB) ||
      (SuppressExceptions && !Scalar && VectorSize != 64) ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(DestinationOperand) ||
      (((Encoding.ModRM & 0xc0) != 0xc0) != MemoryForm))
    return false;

  if (Scalar) {
    if (!MergeOperand ||
        decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
            vectorRegisterIndex(*MergeOperand))
      return false;
  } else if ((Encoding.P1 & 0x78) != 0x78 ||
             (Encoding.P2 & 0x08) == 0) {
    return false;
  }
  if (!MemoryForm &&
      decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(SourceOperand))
    return false;

  const unsigned LaneCount = Scalar ? 1 : VectorSize / ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  const uint8_t EncodedMask = Encoding.P2 & 7;
  const bool EncodedZero = (Encoding.P2 & 0x80) != 0;
  NdVar ActiveMask = NdVar::cst(
      LaneCount == 64 ? UINT64_MAX : ((UINT64_C(1) << LaneCount) - 1),
      MaskSize);
  if (MaskOperand) {
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand->reg));
    if (!isX86OpmaskOperand(*MaskOperand) ||
        MaskOperand->reg == X86_REG_K0 || MaskOperand->size != MaskSize ||
        EncodedMask != static_cast<uint8_t>(MaskOperand->reg - X86_REG_K0) ||
        EncodedZero != static_cast<bool>(MaskOperand->avx_zero_opmask) ||
        MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < MaskSize)
      return false;
    ActiveMask = NdVar::reg(MaskInfo.Offset, MaskSize);
  } else if (EncodedMask != 0 || EncodedZero) {
    return false;
  }
  if (Scalar && MaskOperand) {
    NdVar LowMask = S.makeTemp(1);
    S.emit(NdOp::INT_AND, LowMask,
           {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
    ActiveMask = LowMask;
  }

  const unsigned BroadcastLaneCount = VectorSize / ElementSize;
  const x86_avx_bcast ExpectedBroadcast =
      !Broadcast                  ? X86_AVX_BCAST_INVALID
      : BroadcastLaneCount == 2  ? X86_AVX_BCAST_2
      : BroadcastLaneCount == 4  ? X86_AVX_BCAST_4
      : BroadcastLaneCount == 8  ? X86_AVX_BCAST_8
      : BroadcastLaneCount == 16 ? X86_AVX_BCAST_16
                                 : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const bool IsMask = MaskOperand && &X86.operands[Index] == MaskOperand;
    const bool IsSource = &X86.operands[Index] == &SourceOperand;
    if (X86.operands[Index].avx_zero_opmask != (IsMask && EncodedZero) ||
        X86.operands[Index].avx_bcast !=
            (IsSource ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }
  if (MemoryForm) {
    if (SourceOperand.size != MemoryTupleSize ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, SourceOperand,
                                         MemoryTupleSize))
      return false;
  } else if (SourceOperand.avx_bcast != X86_AVX_BCAST_INVALID ||
             !validateCanonicalEvexRegisterTail(Insn, X86, Encoding)) {
    return false;
  }

  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      VectorSize == 16 ? 0 : (VectorSize == 32 ? 0x20 : 0x40);
  if ((SuppressExceptions ? EncodedLength != 0
                          : EncodedLength != ExpectedLength) ||
      X86.avx_sae != SuppressExceptions ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  NdVar Source;
  if (MemoryForm) {
    Source = emitEvexMaskedMemoryLoad(S, SourceOperand, ActiveMask, VectorSize,
                                      ElementSize, MemoryTupleSize, Broadcast);
  } else {
    Source = L.operandRead(S, SourceOperand);
  }
  if (Source.Size != VectorSize)
    return false;
  const uint8_t Control = makeX86FPExtractControl(
      X86FPExtractKind::Exponent, F64, Scalar, SuppressExceptions);
  NdVar Raw = S.makeTemp(VectorSize);
  S.emitIntrinsic(Intrinsic::X86FPExtract, Raw,
                  {NdVar::cst(Control, 1), Source, NdVar::cst(0, 1),
                   ActiveMask});

  if (!Scalar) {
    if (MaskOperand)
      return emitMaskedVectorResult(L, S, DestinationOperand, *MaskOperand,
                                    Raw, ElementSize);
    const NdVar Destination = L.operandWrite(DestinationOperand);
    S.emit(NdOp::COPY, Destination, {Raw});
    return true;
  }

  if (!MergeOperand)
    return false;
  const NdVar Merge = L.operandRead(S, *MergeOperand);
  const NdVar Destination = L.operandWrite(DestinationOperand);
  if (Merge.Size != 16 || Destination.Size != 16)
    return false;
  NdVar Low = S.makeTemp(ElementSize);
  S.emit(NdOp::SUBBYTES, Low, {Raw, NdVar::cst(0, 4)});
  if (MaskOperand) {
    NdVar Inactive = NdVar::cst(0, ElementSize);
    if (!MaskOperand->avx_zero_opmask) {
      const NdVar OldDestination = L.operandRead(S, DestinationOperand);
      Inactive = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, Inactive,
             {OldDestination, NdVar::cst(0, 4)});
    }
    NdVar Selected = S.makeTemp(ElementSize);
    S.emit(NdOp::SELECT, Selected, {ActiveMask, Low, Inactive});
    Low = Selected;
  }
  NdVar Upper = S.makeTemp(16 - ElementSize);
  S.emit(NdOp::SUBBYTES, Upper, {Merge, NdVar::cst(ElementSize, 4)});
  S.emit(NdOp::CONCAT, Destination, {Upper, Low});
  return true;
}

bool isLegalMainVectorSize(const LaneMoveSpec &Spec, uint16_t Size,
                           unsigned InsnId) {
  if (Spec.Kind == LaneMoveKind::Broadcast) {
    if (InsnId == X86_INS_VBROADCASTI32X2)
      return Size == 16 || Size == 32 || Size == 64;
    if (Spec.LaneSize == 8 || Spec.LaneSize == 16)
      return Size == 32 || Size == 64;
    return Size == 64;
  }
  if (Spec.LaneSize == 16)
    return Size == 32 || Size == 64;
  return Size == 64;
}

bool hasCanonicalLaneMoveEncoding(
    const cs_insn *Insn, const cs_x86 &X86, Arch TargetArch,
    const LaneMoveSpec &Spec, uint16_t MainVectorSize,
    const cs_x86_op &DestinationOperand,
    const cs_x86_op &MainSourceOperand, const cs_x86_op *TupleOperand,
    const cs_x86_op *ImmediateOperand, bool HasMask, x86_reg MaskRegister,
    bool ZeroMask) {
  const bool MemoryForm = DestinationOperand.type == X86_OP_MEM ||
                          MainSourceOperand.type == X86_OP_MEM ||
                          (TupleOperand && TupleOperand->type == X86_OP_MEM);
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, TargetArch, Encoding) ||
      (Encoding.P0 & 0x07) !=
          (Spec.Kind == LaneMoveKind::Broadcast ? 0x02 : 0x03) ||
      Encoding.Opcode != Spec.Opcode)
    return false;

  if (((Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((Spec.W ? 0x80 : 0) | 0x05) ||
      (Encoding.P2 & 0x10) != 0)
    return false;

  const uint8_t ExpectedLength =
      MainVectorSize == 16 ? 0 : (MainVectorSize == 32 ? 0x20 : 0x40);
  if ((Encoding.P2 & 0x60) != ExpectedLength)
    return false;

  const uint8_t EncodedMask = Encoding.P2 & 0x07;
  const uint8_t ExpectedMask =
      HasMask ? static_cast<uint8_t>(MaskRegister - X86_REG_K0) : 0;
  if (EncodedMask != ExpectedMask ||
      ZeroMask != ((Encoding.P2 & 0x80) != 0) ||
      (ZeroMask && !HasMask))
    return false;

  if (Spec.Kind == LaneMoveKind::Broadcast) {
    if ((Encoding.P1 & 0x78) != 0x78 || (Encoding.P2 & 0x08) == 0 ||
        decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
            vectorRegisterIndex(DestinationOperand))
      return false;
    if (MainSourceOperand.type == X86_OP_REG) {
      if ((Encoding.ModRM & 0xc0) != 0xc0 ||
          decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
              vectorRegisterIndex(MainSourceOperand))
        return false;
    } else if ((Encoding.ModRM & 0xc0) == 0xc0) {
      return false;
    }
  } else if (Spec.Kind == LaneMoveKind::Insert) {
    if (!TupleOperand ||
        decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
            vectorRegisterIndex(DestinationOperand) ||
        decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
            vectorRegisterIndex(MainSourceOperand))
      return false;
    if (TupleOperand->type == X86_OP_REG) {
      if ((Encoding.ModRM & 0xc0) != 0xc0 ||
          decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
              vectorRegisterIndex(*TupleOperand))
        return false;
    } else if ((Encoding.ModRM & 0xc0) == 0xc0) {
      return false;
    }
  } else {
    if ((Encoding.P1 & 0x78) != 0x78 || (Encoding.P2 & 0x08) == 0 ||
        decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
            vectorRegisterIndex(MainSourceOperand))
      return false;
    if (DestinationOperand.type == X86_OP_REG) {
      if ((Encoding.ModRM & 0xc0) != 0xc0 ||
          decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
              vectorRegisterIndex(DestinationOperand))
        return false;
    } else if ((Encoding.ModRM & 0xc0) == 0xc0) {
      return false;
    }
  }

  const size_t TrailingBytes = Spec.Kind == LaneMoveKind::Broadcast ? 0 : 1;
  if (MemoryForm) {
    const cs_x86_op &MemoryOperand =
        Spec.Kind == LaneMoveKind::Broadcast
            ? MainSourceOperand
            : (Spec.Kind == LaneMoveKind::Insert ? *TupleOperand
                                                 : DestinationOperand);
    if (!validateCanonicalEvexMemoryTail(Insn, X86, Encoding, MemoryOperand,
                                         Spec.LaneSize, TrailingBytes))
      return false;
  } else if (!validateCanonicalEvexRegisterTail(Insn, X86, Encoding,
                                                TrailingBytes)) {
    return false;
  }

  if (Spec.Kind == LaneMoveKind::Broadcast)
    return X86.encoding.imm_offset == 0 && X86.encoding.imm_size == 0;
  return ImmediateOperand && ImmediateOperand->type == X86_OP_IMM &&
         ImmediateOperand->size == 1 && X86.encoding.imm_size == 1 &&
         X86.encoding.imm_offset == Insn->size - 1 &&
         Insn->bytes[Insn->size - 1] ==
             static_cast<uint8_t>(ImmediateOperand->imm);
}

NdVar emitRepeatedTuple(X86Lifter::LiftState &S, NdVar Source,
                        uint16_t TupleSize, uint16_t DestinationSize) {
  NdVar Tuple = S.makeTemp(TupleSize);
  S.emit(NdOp::SUBBYTES, Tuple, {Source, NdVar::cst(0, 4)});
  NdVar Result = Tuple;
  while (Result.Size < DestinationSize) {
    NdVar Next = S.makeTemp(Result.Size * 2);
    S.emit(NdOp::CONCAT, Next, {Result, Result});
    Result = Next;
  }
  return Result;
}

NdVar addTupleByteOffset(X86Lifter::LiftState &S, NdVar Address,
                         uint16_t Offset) {
  if (Offset == 0)
    return Address;
  NdVar Adjusted = S.makeTemp(Address.Size);
  S.emit(NdOp::INT_ADD, Adjusted,
         {Address, NdVar::scalar(Offset, Address.Size)});
  return Adjusted;
}

NdVar emitTupleLoad(X86Lifter::LiftState &S, const cs_x86_op &Memory,
                    uint16_t TupleSize) {
  const NdVar Address = S.computeEA(Memory);
  const NdMemoryAddressSpace AddressSpace =
      X86Lifter::LiftState::memoryAddressSpace(Memory);
  NdVar Result = S.makeTemp(0);
  for (uint16_t Offset = 0; Offset < TupleSize; Offset += 8) {
    NdVar Part = S.makeTemp(8);
    S.emit(NdOp::LOAD, Part, {addTupleByteOffset(S, Address, Offset)},
           NdMemoryOrdering::None, AddressSpace);
    if (Offset == 0) {
      Result = Part;
    } else {
      NdVar Next = S.makeTemp(Result.Size + 8);
      S.emit(NdOp::CONCAT, Next, {Part, Result});
      Result = Next;
    }
  }
  return Result;
}

NdVar lowCompactMaskWindow(X86Lifter::LiftState &S, NdVar Mask,
                           unsigned FirstLane, unsigned LaneCount) {
  if (Mask.Size == 0 || Mask.Size > 8 || LaneCount == 0 || LaneCount > 8 ||
      FirstLane + LaneCount > static_cast<unsigned>(Mask.Size) * 8)
    return {};
  NdVar Shifted = Mask;
  if (FirstLane != 0) {
    Shifted = S.makeTemp(Mask.Size);
    S.emit(NdOp::INT_RIGHT, Shifted,
           {Mask, NdVar::cst(FirstLane, Mask.Size)});
  }
  NdVar Relevant = S.makeTemp(Mask.Size);
  S.emit(NdOp::INT_AND, Relevant,
         {Shifted, NdVar::cst((UINT64_C(1) << LaneCount) - 1, Mask.Size)});
  const uint16_t ResultSize = static_cast<uint16_t>((LaneCount + 7) / 8);
  if (Relevant.Size == ResultSize)
    return Relevant;
  NdVar Result = S.makeTemp(ResultSize);
  S.emit(NdOp::SUBBYTES, Result, {Relevant, NdVar::cst(0, 4)});
  return Result;
}

NdVar repeatedTupleLoadMask(X86Lifter::LiftState &S, NdVar DestinationMask,
                            unsigned DestinationLaneCount,
                            unsigned TupleLaneCount) {
  if (DestinationMask.Size == 0 || DestinationMask.Size > 8 ||
      DestinationLaneCount == 0 || TupleLaneCount == 0 ||
      DestinationLaneCount > static_cast<unsigned>(DestinationMask.Size) * 8 ||
      TupleLaneCount > 8 || DestinationLaneCount % TupleLaneCount != 0)
    return {};

  NdVar Result = NdVar::cst(0, DestinationMask.Size);
  for (unsigned TupleLane = 0; TupleLane < TupleLaneCount; ++TupleLane) {
    NdVar Any = NdVar::cst(0, DestinationMask.Size);
    for (unsigned Lane = TupleLane; Lane < DestinationLaneCount;
         Lane += TupleLaneCount) {
      NdVar Shifted = DestinationMask;
      if (Lane != 0) {
        Shifted = S.makeTemp(DestinationMask.Size);
        S.emit(NdOp::INT_RIGHT, Shifted,
               {DestinationMask, NdVar::cst(Lane, DestinationMask.Size)});
      }
      NdVar Bit = S.makeTemp(DestinationMask.Size);
      S.emit(NdOp::INT_AND, Bit,
             {Shifted, NdVar::cst(1, DestinationMask.Size)});
      NdVar Next = S.makeTemp(DestinationMask.Size);
      S.emit(NdOp::INT_OR, Next, {Any, Bit});
      Any = Next;
    }
    if (TupleLane != 0) {
      NdVar Positioned = S.makeTemp(DestinationMask.Size);
      S.emit(NdOp::INT_LEFT, Positioned,
             {Any, NdVar::cst(TupleLane, DestinationMask.Size)});
      Any = Positioned;
    }
    NdVar Next = S.makeTemp(DestinationMask.Size);
    S.emit(NdOp::INT_OR, Next, {Result, Any});
    Result = Next;
  }

  const uint16_t ResultSize =
      static_cast<uint16_t>((TupleLaneCount + 7) / 8);
  if (Result.Size == ResultSize)
    return Result;
  NdVar Narrowed = S.makeTemp(ResultSize);
  S.emit(NdOp::SUBBYTES, Narrowed, {Result, NdVar::cst(0, 4)});
  return Narrowed;
}

NdVar emitMaskedTupleLoad(X86Lifter::LiftState &S, const cs_x86_op &Memory,
                          NdVar CompactMask, uint16_t TupleSize,
                          uint16_t ElementSize) {
  if (TupleSize == 0 || TupleSize > 32 || TupleSize % ElementSize != 0 ||
      CompactMask.Size == 0)
    return {};
  const uint16_t LoadSize = std::max<uint16_t>(16, TupleSize);
  const NdVar ExpandedMask =
      expandCompactLaneMask(S, CompactMask, LoadSize, ElementSize);
  const Intrinsic LoadId = maskedVectorLoadIntrinsic(ElementSize);
  if (ExpandedMask.Size != LoadSize || LoadId == Intrinsic::None)
    return {};
  NdVar Loaded = S.makeTemp(LoadSize);
  S.emitIntrinsic(LoadId, Loaded, {S.computeEA(Memory), ExpandedMask},
                  NdMemoryOrdering::None,
                  X86Lifter::LiftState::memoryAddressSpace(Memory));
  if (LoadSize == TupleSize)
    return Loaded;
  NdVar Tuple = S.makeTemp(TupleSize);
  S.emit(NdOp::SUBBYTES, Tuple, {Loaded, NdVar::cst(0, 4)});
  return Tuple;
}

Intrinsic maskedTupleStoreIntrinsic(uint16_t ElementSize) {
  return ElementSize == 4   ? Intrinsic::MaskedStoreD
         : ElementSize == 8 ? Intrinsic::MaskedStoreQ
                            : Intrinsic::None;
}

bool emitMaskedTupleStore(X86Lifter::LiftState &S, const cs_x86_op &Memory,
                          NdVar Value, NdVar CompactMask,
                          uint16_t ElementSize) {
  if (Value.Size == 0 || Value.Size > 32 || Value.Size % ElementSize != 0 ||
      CompactMask.Size == 0)
    return false;
  const uint16_t StoreSize = std::max<uint16_t>(16, Value.Size);
  if (Value.Size != StoreSize) {
    NdVar Padded = S.makeTemp(StoreSize);
    S.emit(NdOp::INT_ZEXT, Padded, {Value});
    Value = Padded;
  }
  const NdVar ExpandedMask =
      expandCompactLaneMask(S, CompactMask, StoreSize, ElementSize);
  const Intrinsic StoreId = maskedTupleStoreIntrinsic(ElementSize);
  if (ExpandedMask.Size != StoreSize || StoreId == Intrinsic::None)
    return false;
  S.emitIntrinsic(StoreId, {},
                  {S.computeEA(Memory), ExpandedMask, Value},
                  NdMemoryOrdering::None,
                  X86Lifter::LiftState::memoryAddressSpace(Memory));
  return true;
}

void emitTupleStore(X86Lifter::LiftState &S, const cs_x86_op &Memory,
                    NdVar Value) {
  const NdVar Address = S.computeEA(Memory);
  const NdMemoryAddressSpace AddressSpace =
      X86Lifter::LiftState::memoryAddressSpace(Memory);
  for (uint16_t Offset = 0; Offset < Value.Size; Offset += 8) {
    NdVar Part = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, Part, {Value, NdVar::cst(Offset, 4)});
    S.emit(NdOp::STORE, {}, {addTupleByteOffset(S, Address, Offset), Part},
           NdMemoryOrdering::None, AddressSpace);
  }
}

NdVar emitInsertedTuple(X86Lifter::LiftState &S, NdVar Base, NdVar Tuple,
                        uint16_t TupleSize, uint8_t Immediate) {
  const unsigned TupleCount = Base.Size / TupleSize;
  const unsigned Selected = Immediate & (TupleCount - 1);
  NdVar Result = S.makeTemp(0);
  for (unsigned Index = 0; Index < TupleCount; ++Index) {
    NdVar Part = Tuple;
    if (Index != Selected) {
      Part = S.makeTemp(TupleSize);
      S.emit(NdOp::SUBBYTES, Part,
             {Base, NdVar::cst(static_cast<uint64_t>(Index) * TupleSize, 4)});
    }
    if (Index == 0) {
      Result = Part;
    } else {
      NdVar Next = S.makeTemp(Result.Size + TupleSize);
      S.emit(NdOp::CONCAT, Next, {Part, Result});
      Result = Next;
    }
  }
  return Result;
}

bool liftLaneMove(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                  const cs_x86 &X86) {
  LaneMoveSpec Spec;
  if (!Insn || !getLaneMoveSpec(Insn->id, Spec))
    return false;

  const unsigned BaseOperandCount = Spec.Kind == LaneMoveKind::Broadcast ? 2
                                    : Spec.Kind == LaneMoveKind::Insert  ? 4
                                                                         : 3;
  if (hasUnsupportedEvexValueModifier(X86) ||
      (X86.op_count != BaseOperandCount &&
       X86.op_count != BaseOperandCount + 1))
    return false;
  const bool HasMask = X86.op_count == BaseOperandCount + 1;
  if (HasMask && !isX86OpmaskOperand(X86.operands[1]))
    return false;

  const unsigned MainSourceIndex = HasMask ? 2 : 1;
  const unsigned TupleIndex = HasMask ? 3 : 2;
  const unsigned ImmediateIndex =
      Spec.Kind == LaneMoveKind::Insert ? (HasMask ? 4 : 3) : (HasMask ? 3 : 2);
  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op &MainSourceOperand = X86.operands[MainSourceIndex];
  const cs_x86_op *TupleOperand =
      Spec.Kind == LaneMoveKind::Insert ? &X86.operands[TupleIndex] : nullptr;
  const cs_x86_op *ImmediateOperand = Spec.Kind == LaneMoveKind::Broadcast
                                          ? nullptr
                                          : &X86.operands[ImmediateIndex];

  uint16_t MainVectorSize = 0;
  bool MemoryForm = false;
  if (Spec.Kind == LaneMoveKind::Broadcast) {
    MainVectorSize = static_cast<uint16_t>(DestinationOperand.size);
    MemoryForm = MainSourceOperand.type == X86_OP_MEM;
    if (!isVectorRegisterOfSize(DestinationOperand, MainVectorSize) ||
        (!MemoryForm && (!Spec.RegisterTuple ||
                         !isVectorRegisterOfSize(MainSourceOperand, 16))) ||
        (MemoryForm && !isValidLaneMemoryOperand(MainSourceOperand,
                                                 S.AddressSize, Spec.LaneSize)))
      return false;
  } else if (Spec.Kind == LaneMoveKind::Insert) {
    MainVectorSize = static_cast<uint16_t>(DestinationOperand.size);
    MemoryForm = TupleOperand && TupleOperand->type == X86_OP_MEM;
    if (!isVectorRegisterOfSize(DestinationOperand, MainVectorSize) ||
        !isVectorRegisterOfSize(MainSourceOperand, MainVectorSize) ||
        !TupleOperand ||
        (!MemoryForm &&
         !isVectorRegisterOfSize(*TupleOperand, Spec.LaneSize)) ||
        (MemoryForm && !isValidLaneMemoryOperand(*TupleOperand, S.AddressSize,
                                                 Spec.LaneSize)))
      return false;
  } else {
    MainVectorSize = static_cast<uint16_t>(MainSourceOperand.size);
    MemoryForm = DestinationOperand.type == X86_OP_MEM;
    if ((!MemoryForm &&
         !isVectorRegisterOfSize(DestinationOperand, Spec.LaneSize)) ||
        (MemoryForm && !isValidLaneMemoryOperand(
                           DestinationOperand, S.AddressSize, Spec.LaneSize)) ||
        !isVectorRegisterOfSize(MainSourceOperand, MainVectorSize))
      return false;
  }

  if (!isLegalMainVectorSize(Spec, MainVectorSize, Insn->id) ||
      Spec.LaneSize > MainVectorSize ||
      (Spec.Kind != LaneMoveKind::Broadcast &&
       (!ImmediateOperand || ImmediateOperand->type != X86_OP_IMM ||
        ImmediateOperand->size != 1)))
    return false;

  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    if (X86.operands[Index].avx_zero_opmask && (!HasMask || Index != 1))
      return false;
  }

  bool ZeroMask = false;
  x86_reg MaskRegister = X86_REG_INVALID;
  NdVar ActiveMask;
  if (HasMask) {
    const cs_x86_op &MaskOperand = X86.operands[1];
    MaskRegister = static_cast<x86_reg>(MaskOperand.reg);
    ZeroMask = MaskOperand.avx_zero_opmask;
    const unsigned MainLaneCount = MainVectorSize / Spec.ElementSize;
    const uint16_t MaskSize = static_cast<uint16_t>((MainLaneCount + 7) / 8);
    const RegInfo MaskInfo = mapCapstoneReg(MaskRegister);
    if (MaskRegister == X86_REG_K0 || MaskOperand.size < MaskSize ||
        MaskOperand.size > 8 || MaskInfo.Offset == UINT64_C(0xffff) ||
        MaskInfo.Size < MaskSize)
      return false;
    ActiveMask = NdVar::reg(MaskInfo.Offset, MaskOperand.size);
  }

  // EVEX.z is reserved when the architectural destination is memory.
  if (MemoryForm && Spec.Kind == LaneMoveKind::Extract && ZeroMask)
    return false;

  if (!hasCanonicalLaneMoveEncoding(
          Insn, X86, L.targetArch(), Spec, MainVectorSize, DestinationOperand,
          MainSourceOperand, TupleOperand, ImmediateOperand, HasMask,
          MaskRegister, ZeroMask))
    return false;

  if (DestinationOperand.type == X86_OP_REG) {
    const RegInfo DestinationInfo =
        mapCapstoneReg(static_cast<x86_reg>(DestinationOperand.reg));
    if (DestinationInfo.Offset == UINT64_C(0xffff) ||
        DestinationInfo.Size != DestinationOperand.size)
      return false;
  }
  if (MainSourceOperand.type == X86_OP_REG) {
    const RegInfo MainSourceInfo =
        mapCapstoneReg(static_cast<x86_reg>(MainSourceOperand.reg));
    if (MainSourceInfo.Offset == UINT64_C(0xffff) ||
        MainSourceInfo.Size != MainSourceOperand.size)
      return false;
  }
  if (TupleOperand && TupleOperand->type == X86_OP_REG) {
    const RegInfo TupleInfo =
        mapCapstoneReg(static_cast<x86_reg>(TupleOperand->reg));
    if (TupleInfo.Offset == UINT64_C(0xffff) ||
        TupleInfo.Size != TupleOperand->size)
      return false;
  }

  NdVar MainSource;
  if (MainSourceOperand.type == X86_OP_MEM) {
    if (HasMask) {
      const unsigned DestinationLaneCount = MainVectorSize / Spec.ElementSize;
      const unsigned TupleLaneCount = Spec.LaneSize / Spec.ElementSize;
      const NdVar TupleMask = repeatedTupleLoadMask(
          S, ActiveMask, DestinationLaneCount, TupleLaneCount);
      MainSource = emitMaskedTupleLoad(S, MainSourceOperand, TupleMask,
                                       Spec.LaneSize, Spec.ElementSize);
    } else {
      MainSource = emitTupleLoad(S, MainSourceOperand, Spec.LaneSize);
    }
  } else {
    MainSource = L.operandRead(S, MainSourceOperand);
  }
  if (MainSource.Size == 0)
    return false;
  NdVar Raw;
  if (Spec.Kind == LaneMoveKind::Broadcast) {
    Raw = emitRepeatedTuple(S, MainSource, Spec.LaneSize, MainVectorSize);
  } else if (Spec.Kind == LaneMoveKind::Insert) {
    NdVar Tuple;
    if (TupleOperand->type == X86_OP_MEM) {
      if (HasMask) {
        const unsigned TupleLaneCount = Spec.LaneSize / Spec.ElementSize;
        const unsigned TupleCount = MainVectorSize / Spec.LaneSize;
        const unsigned Selected =
            static_cast<uint8_t>(ImmediateOperand->imm) & (TupleCount - 1);
        const NdVar TupleMask = lowCompactMaskWindow(
            S, ActiveMask, Selected * TupleLaneCount, TupleLaneCount);
        Tuple = emitMaskedTupleLoad(S, *TupleOperand, TupleMask,
                                    Spec.LaneSize, Spec.ElementSize);
      } else {
        Tuple = emitTupleLoad(S, *TupleOperand, Spec.LaneSize);
      }
    } else {
      Tuple = L.operandRead(S, *TupleOperand);
    }
    if (Tuple.Size != Spec.LaneSize)
      return false;
    Raw = emitInsertedTuple(S, MainSource, Tuple, Spec.LaneSize,
                            static_cast<uint8_t>(ImmediateOperand->imm));
  } else {
    const unsigned TupleCount = MainVectorSize / Spec.LaneSize;
    const unsigned Selected =
        static_cast<uint8_t>(ImmediateOperand->imm) & (TupleCount - 1);
    Raw = S.makeTemp(Spec.LaneSize);
    S.emit(NdOp::SUBBYTES, Raw,
           {MainSource,
            NdVar::cst(static_cast<uint64_t>(Selected) * Spec.LaneSize, 4)});
  }

  if (Spec.Kind == LaneMoveKind::Extract && MemoryForm) {
    if (HasMask) {
      const unsigned TupleLaneCount = Spec.LaneSize / Spec.ElementSize;
      const NdVar StoreMask =
          lowCompactMaskWindow(S, ActiveMask, 0, TupleLaneCount);
      return emitMaskedTupleStore(S, DestinationOperand, Raw, StoreMask,
                                  Spec.ElementSize);
    }
    emitTupleStore(S, DestinationOperand, Raw);
    return true;
  }
  if (HasMask)
    return emitMaskedVectorResult(L, S, DestinationOperand, X86.operands[1],
                                  Raw, Spec.ElementSize);
  const NdVar Destination = L.operandWrite(DestinationOperand);
  S.emit(NdOp::COPY, Destination, {Raw});
  return true;
}

bool liftApproxFloat(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                     const cs_x86 &X86) {
  ApproxSpec Spec;
  if (!Insn || !getApproxSpec(Insn->id, Spec) || Insn->size > 15)
    return false;

  CanonicalEvexEncodingInfo Encoding;
  const bool Parsed =
      Spec.Scalar
          ? parseCanonicalEvexLligEncodingInfo(Insn, X86, L.targetArch(),
                                               Encoding)
          : parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding);
  if (!Parsed || X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      Encoding.Opcode != Spec.Opcode)
    return false;

  const bool MemoryForm = (Encoding.ModRM & 0xc0) != 0xc0;
  if ((Encoding.P0 & 0x07) != 0x02 || X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const unsigned BaseOperandCount = Spec.Scalar ? 3 : 2;
  if (X86.op_count != BaseOperandCount && X86.op_count != BaseOperandCount + 1)
    return false;
  const bool HasMask = X86.op_count == BaseOperandCount + 1;
  if (HasMask && !isX86OpmaskOperand(X86.operands[1]))
    return false;

  const unsigned FirstSourceIndex = HasMask ? 2 : 1;
  const unsigned ApproxSourceIndex =
      Spec.Scalar ? FirstSourceIndex + 1 : FirstSourceIndex;
  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op &ApproxSourceOperand = X86.operands[ApproxSourceIndex];
  const cs_x86_op *PassThroughOperand =
      Spec.Scalar ? &X86.operands[FirstSourceIndex] : nullptr;
  const uint16_t VectorSize = static_cast<uint16_t>(DestinationOperand.size);
  const uint16_t ElementSize = Spec.W ? 8 : 4;
  const bool Broadcast = MemoryForm && (Encoding.P2 & 0x10) != 0;
  const uint16_t MemoryTupleSize =
      Spec.Scalar || Broadcast ? ElementSize : VectorSize;
  if (!isVectorRegisterOfSize(DestinationOperand, VectorSize) ||
      (PassThroughOperand &&
       !isVectorRegisterOfSize(*PassThroughOperand, VectorSize)) ||
      (Spec.Scalar
           ? VectorSize != 16
           : (Spec.Reference28 ? VectorSize != 64
                               : (VectorSize != 16 && VectorSize != 32 &&
                                  VectorSize != 64))))
    return false;
  if (MemoryForm) {
    if (!isValidLaneMemoryOperand(ApproxSourceOperand, S.AddressSize,
                                  MemoryTupleSize) ||
        (Spec.Scalar && Broadcast))
      return false;
  } else if (!isVectorRegisterOfSize(ApproxSourceOperand, VectorSize)) {
    return false;
  }

  const unsigned BroadcastLaneCount = VectorSize / ElementSize;
  const x86_avx_bcast ExpectedBroadcast =
      BroadcastLaneCount == 2    ? X86_AVX_BCAST_2
      : BroadcastLaneCount == 4  ? X86_AVX_BCAST_4
      : BroadcastLaneCount == 8  ? X86_AVX_BCAST_8
      : BroadcastLaneCount == 16 ? X86_AVX_BCAST_16
                                 : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const x86_avx_bcast OperandBroadcast = X86.operands[Index].avx_bcast;
    if (Index == ApproxSourceIndex && Broadcast) {
      if (OperandBroadcast != ExpectedBroadcast)
        return false;
    } else if (OperandBroadcast != X86_AVX_BCAST_INVALID) {
      return false;
    }
  }

  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].avx_zero_opmask && (!HasMask || Index != 1))
      return false;

  const bool SuppressExceptions =
      !MemoryForm && (Encoding.P2 & 0x10) != 0;
  if (((Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((Spec.W ? 0x80 : 0) | 0x05) ||
      SuppressExceptions != X86.avx_sae ||
      (SuppressExceptions && !Spec.Reference28) ||
      decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
          vectorRegisterIndex(DestinationOperand) ||
      (!MemoryForm &&
       decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
           vectorRegisterIndex(ApproxSourceOperand)))
    return false;

  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  if (Spec.Reference28) {
    if (MemoryForm) {
      if ((!Spec.Scalar && EncodedLength != 0x40) ||
          (Spec.Scalar && EncodedLength == 0x60))
        return false;
    } else if (!SuppressExceptions) {
      if ((!Spec.Scalar && EncodedLength != 0x40) ||
          (Spec.Scalar && EncodedLength == 0x60))
        return false;
    }
  } else {
    const uint8_t ExpectedLength =
        VectorSize == 16 ? 0 : (VectorSize == 32 ? 0x20 : 0x40);
    if ((!Spec.Scalar && EncodedLength != ExpectedLength) ||
        (Spec.Scalar && EncodedLength == 0x60))
      return false;
  }
  if (Spec.Scalar) {
    if (!PassThroughOperand ||
        decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
            vectorRegisterIndex(*PassThroughOperand))
      return false;
  } else if ((Encoding.P1 & 0x78) != 0x78 ||
             (Encoding.P2 & 0x08) == 0) {
    return false;
  }

  if (MemoryForm) {
    if (!validateCanonicalEvexMemoryTail(Insn, X86, Encoding,
                                         ApproxSourceOperand,
                                         MemoryTupleSize))
      return false;
  } else if (!validateCanonicalEvexRegisterTail(Insn, X86, Encoding)) {
    return false;
  }

  const uint8_t EncodedMask = Encoding.P2 & 0x07;
  bool ZeroInactive = false;
  NdVar ActiveMask = NdVar::cst(UINT64_MAX, 8);
  if (HasMask) {
    const cs_x86_op &MaskOperand = X86.operands[1];
    if (MaskOperand.reg == X86_REG_K0)
      return false;
    const uint8_t ExpectedMask =
        static_cast<uint8_t>(MaskOperand.reg - X86_REG_K0);
    ZeroInactive = MaskOperand.avx_zero_opmask;
    const unsigned LaneCount = Spec.Scalar ? 1 : VectorSize / (Spec.W ? 8 : 4);
    const uint16_t MaskSize =
        static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
    if (EncodedMask != ExpectedMask ||
        ZeroInactive != ((Encoding.P2 & 0x80) != 0) ||
        MaskOperand.size < MaskSize || MaskOperand.size > 8 ||
        MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < MaskSize)
      return false;
    ActiveMask = NdVar::reg(MaskInfo.Offset, MaskSize);
  } else if (EncodedMask != 0 || (Encoding.P2 & 0x80) != 0) {
    return false;
  }

  NdVar Source;
  if (MemoryForm) {
    NdVar LoadMask = ActiveMask;
    if (Spec.Scalar) {
      LoadMask = S.makeTemp(ActiveMask.Size);
      S.emit(NdOp::INT_AND, LoadMask,
             {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
    }
    Source =
        emitEvexMaskedMemoryLoad(S, ApproxSourceOperand, LoadMask, VectorSize,
                                 ElementSize, MemoryTupleSize, Broadcast);
    if (Source.Size != VectorSize)
      return false;
  } else {
    Source = L.operandRead(S, ApproxSourceOperand);
  }
  const NdVar OldDestination = L.operandRead(S, DestinationOperand);
  const NdVar PassThrough = PassThroughOperand
                                ? L.operandRead(S, *PassThroughOperand)
                                : OldDestination;
  if (Source.Size != VectorSize || OldDestination.Size != VectorSize ||
      PassThrough.Size != VectorSize)
    return false;
  const NdVar Destination = L.operandWrite(DestinationOperand);
  const uint8_t Control = static_cast<uint8_t>(
      static_cast<uint8_t>(Spec.Kind) | (ZeroInactive ? 0x10 : 0) |
      (Spec.Scalar ? 0x20 : 0) | (SuppressExceptions ? 0x40 : 0));
  S.emitIntrinsic(Intrinsic::X86ApproxFloat, Destination,
                  {NdVar::cst(Control, 1), Source, OldDestination, PassThrough,
                   ActiveMask});
  return true;
}

bool liftFPClass(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                 const cs_x86 &X86) {
  if (!Insn || Insn->size > 15)
    return false;
  const bool Scalar =
      Insn->id == X86_INS_VFPCLASSSS || Insn->id == X86_INS_VFPCLASSSD;
  const bool F64 =
      Insn->id == X86_INS_VFPCLASSPD || Insn->id == X86_INS_VFPCLASSSD;
  const uint8_t Opcode = Scalar ? 0x67 : 0x66;
  const uint16_t ElementSize = F64 ? 8 : 4;

  CanonicalEvexEncodingInfo Encoding;
  const bool Parsed =
      Scalar
          ? parseCanonicalEvexLligEncodingInfo(Insn, X86, L.targetArch(),
                                               Encoding)
          : parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding);
  if (!Parsed || X86.encoding.imm_size != 1 ||
      X86.encoding.imm_offset != Insn->size - 1 || Encoding.Opcode != Opcode)
    return false;

  const bool MemoryForm = (Encoding.ModRM & 0xc0) != 0xc0;
  const bool EncodedB = (Encoding.P2 & 0x10) != 0;
  const bool Broadcast = MemoryForm && EncodedB;
  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  if ((Encoding.P0 & 0x07) != 0x03 || (Encoding.P0 & 0x90) != 0x90 ||
      (Encoding.P1 | 0x04) != static_cast<uint8_t>(F64 ? 0xfd : 0x7d) ||
      (Encoding.P2 & 0x88) != 0x08 || EncodedLength == 0x60 ||
      (EncodedB && (!MemoryForm || Scalar)) || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const uint16_t VectorSize =
      Scalar ? 16
             : (EncodedLength == 0 ? 16 : (EncodedLength == 0x20 ? 32 : 64));
  const unsigned LaneCount = Scalar ? 1 : VectorSize / ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
  constexpr unsigned BaseOperandCount = 3;
  if (X86.op_count != BaseOperandCount && X86.op_count != BaseOperandCount + 1)
    return false;
  const bool HasWriteMask = X86.op_count == BaseOperandCount + 1;
  if (HasWriteMask && !isX86OpmaskOperand(X86.operands[1]))
    return false;
  const unsigned SourceIndex = HasWriteMask ? 2 : 1;
  const unsigned ImmediateIndex = HasWriteMask ? 3 : 2;
  const cs_x86_op &DestinationOperand = X86.operands[0];
  const cs_x86_op &SourceOperand = X86.operands[SourceIndex];
  const cs_x86_op &ImmediateOperand = X86.operands[ImmediateIndex];
  if (!isX86OpmaskOperand(DestinationOperand) ||
      DestinationOperand.size != MaskSize ||
      ((Encoding.ModRM >> 3) & 7) !=
          DestinationOperand.reg - X86_REG_K0 ||
      ImmediateOperand.type != X86_OP_IMM || ImmediateOperand.size != 1 ||
      Insn->bytes[Insn->size - 1] !=
          static_cast<uint8_t>(ImmediateOperand.imm) ||
      MemoryForm != (SourceOperand.type == X86_OP_MEM))
    return false;

  const uint16_t MemoryTupleSize =
      Scalar || Broadcast ? ElementSize : VectorSize;
  if (MemoryForm) {
    if (SourceOperand.size != MemoryTupleSize)
      return false;
  } else if (!isVectorRegisterOfSize(SourceOperand, VectorSize) ||
             decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 vectorRegisterIndex(SourceOperand)) {
    return false;
  }

  if (MemoryForm) {
    if (!validateCanonicalEvexMemoryTail(Insn, X86, Encoding, SourceOperand,
                                         MemoryTupleSize, 1))
      return false;
  } else if (!validateCanonicalEvexRegisterTail(Insn, X86, Encoding, 1)) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast = LaneCount == 2   ? X86_AVX_BCAST_2
                                          : LaneCount == 4 ? X86_AVX_BCAST_4
                                          : LaneCount == 8 ? X86_AVX_BCAST_8
                                          : LaneCount == 16
                                              ? X86_AVX_BCAST_16
                                              : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    if (Index == SourceIndex && Broadcast) {
      if (X86.operands[Index].avx_bcast != ExpectedBroadcast)
        return false;
    } else if (X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID) {
      return false;
    }
    if (X86.operands[Index].avx_zero_opmask)
      return false;
  }

  NdVar ActiveMask = NdVar::cst(UINT64_MAX, 8);
  if (HasWriteMask) {
    const cs_x86_op &MaskOperand = X86.operands[1];
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
    if (MaskOperand.reg == X86_REG_K0 || MaskOperand.size != MaskSize ||
        (Encoding.P2 & 7) != MaskOperand.reg - X86_REG_K0 ||
        MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < MaskSize)
      return false;
    ActiveMask = NdVar::reg(MaskInfo.Offset, MaskSize);
  } else if ((Encoding.P2 & 7) != 0) {
    return false;
  }

  NdVar Source;
  if (MemoryForm) {
    NdVar LoadMask = ActiveMask;
    if (Scalar) {
      LoadMask = S.makeTemp(ActiveMask.Size);
      S.emit(NdOp::INT_AND, LoadMask,
             {ActiveMask, NdVar::cst(1, ActiveMask.Size)});
    }
    Source = emitEvexMaskedMemoryLoad(S, SourceOperand, LoadMask, VectorSize,
                                      ElementSize, MemoryTupleSize, Broadcast);
  } else {
    Source = L.operandRead(S, SourceOperand);
  }
  if (Source.Size != VectorSize)
    return false;

  const RegInfo DestinationInfo =
      mapCapstoneReg(static_cast<x86_reg>(DestinationOperand.reg));
  if (DestinationInfo.Offset == UINT64_C(0xffff) || DestinationInfo.Size != 8)
    return false;
  const NdVar Classified = S.makeTemp(MaskSize);
  const uint8_t Control =
      static_cast<uint8_t>((F64 ? 1 : 0) | (Scalar ? 2 : 0));
  S.emitIntrinsic(Intrinsic::X86FPClass, Classified,
                  {NdVar::cst(Control, 1), Source, ActiveMask,
                   NdVar::cst(static_cast<uint8_t>(ImmediateOperand.imm), 1)});
  S.emit(NdOp::INT_ZEXT,
         NdVar::reg(DestinationInfo.Offset, DestinationInfo.Size),
         {Classified});
  return true;
}

} // namespace

bool liftSIMDAVXFloat(X86Lifter &L, X86Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  case X86_INS_VREDUCEPS:
  case X86_INS_VREDUCEPD:
  case X86_INS_VREDUCESS:
  case X86_INS_VREDUCESD:
  case X86_INS_VRNDSCALEPS:
  case X86_INS_VRNDSCALEPD:
  case X86_INS_VRNDSCALESS:
  case X86_INS_VRNDSCALESD:
  case X86_INS_VGETMANTPS:
  case X86_INS_VGETMANTPD:
  case X86_INS_VGETMANTSS:
  case X86_INS_VGETMANTSD:
    return liftEvexFPImmediateUnary(L, S, Insn, X86);

  case X86_INS_VGETEXPPS:
  case X86_INS_VGETEXPPD:
  case X86_INS_VGETEXPSS:
  case X86_INS_VGETEXPSD:
    return liftEvexFPExponent(L, S, Insn, X86);

  case X86_INS_VRANGEPS:
  case X86_INS_VRANGEPD:
  case X86_INS_VRANGESS:
  case X86_INS_VRANGESD:
    return liftEvexFPRange(L, S, Insn, X86);

  case X86_INS_VFIXUPIMMPS:
  case X86_INS_VFIXUPIMMPD:
  case X86_INS_VFIXUPIMMSS:
  case X86_INS_VFIXUPIMMSD:
    return liftEvexFPFixup(L, S, Insn, X86);

  // VSCALEF{PS,PD,SS,SD} — float * 2^int_src (scale by power of 2).
  case X86_INS_VSCALEFPS:
  case X86_INS_VSCALEFPD:
  case X86_INS_VSCALEFSS:
  case X86_INS_VSCALEFSD:
    return liftEvexFPScale(L, S, Insn, X86);

  // VFPCLASS{PS,PD,SS,SD} — float classification test → Mask register.
  case X86_INS_VFPCLASSPS:
  case X86_INS_VFPCLASSPD:
  case X86_INS_VFPCLASSSS:
  case X86_INS_VFPCLASSSD: {
    return liftFPClass(L, S, Insn, X86);
  }

  // VBROADCAST (EVEX 512-bit variants).
  case X86_INS_VBROADCASTF32X2:
  case X86_INS_VBROADCASTF32X4:
  case X86_INS_VBROADCASTF32X8:
  case X86_INS_VBROADCASTF64X2:
  case X86_INS_VBROADCASTF64X4:
  case X86_INS_VBROADCASTI32X2:
  case X86_INS_VBROADCASTI32X4:
  case X86_INS_VBROADCASTI32X8:
  case X86_INS_VBROADCASTI64X2:
  case X86_INS_VBROADCASTI64X4:
    return liftLaneMove(L, S, Insn, X86);

  // VINSERT{F,I}{32X4,32X8,64X2,64X4} — insert 128/256 Lane into ZMM.
  case X86_INS_VINSERTF32X4:
  case X86_INS_VINSERTF32X8:
  case X86_INS_VINSERTF64X2:
  case X86_INS_VINSERTF64X4:
  case X86_INS_VINSERTI32X4:
  case X86_INS_VINSERTI32X8:
  case X86_INS_VINSERTI64X2:
  case X86_INS_VINSERTI64X4:
    return liftLaneMove(L, S, Insn, X86);

  // VEXTRACT{F,I}{32X4,32X8,64X2,64X4} — extract 128/256 Lane from ZMM.
  case X86_INS_VEXTRACTF32X4:
  case X86_INS_VEXTRACTF32X8:
  case X86_INS_VEXTRACTF64X2:
  case X86_INS_VEXTRACTF64X4:
  case X86_INS_VEXTRACTI32X4:
  case X86_INS_VEXTRACTI32X8:
  case X86_INS_VEXTRACTI64X2:
  case X86_INS_VEXTRACTI64X4:
    return liftLaneMove(L, S, Insn, X86);

  // VCOMPRESS{PS,PD} / VEXPAND{PS,PD} — float compress/expand.
  case X86_INS_VCOMPRESSPS:
  case X86_INS_VCOMPRESSPD:
  case X86_INS_VEXPANDPS:
  case X86_INS_VEXPANDPD:
    return liftEvexCompressExpandRegister(
        L, S, Insn, X86,
        InsnId == X86_INS_VCOMPRESSPD || InsnId == X86_INS_VEXPANDPD ? 8 : 4,
        InsnId == X86_INS_VCOMPRESSPS || InsnId == X86_INS_VCOMPRESSPD,
        InsnId == X86_INS_VCOMPRESSPS || InsnId == X86_INS_VCOMPRESSPD ? 0x8a
                                                                       : 0x88,
        InsnId == X86_INS_VCOMPRESSPD || InsnId == X86_INS_VEXPANDPD);

  // VDBPSADBW — double block packed sums of absolute differences.
  case X86_INS_VDBPSADBW: {
    CanonicalEvexEncodingInfo Encoding;
    if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
        Encoding.Offset + 7 > Insn->size ||
        X86.encoding.imm_offset != Insn->size - 1 ||
        X86.encoding.imm_size != 1 || Encoding.Opcode != 0x42 ||
        (Encoding.P0 & 0x07) != 3 ||
        ((Encoding.P1 | 0x04) & 0x87) != 0x05 ||
        (Encoding.P2 & 0x10) != 0 || (Encoding.P2 & 0x60) == 0x60)
      return false;
    const bool HasMask = X86.op_count == 5;
    if ((!HasMask && X86.op_count != 4) ||
        (HasMask && !isX86OpmaskOperand(X86.operands[1])))
      return false;
    const unsigned AIndex = HasMask ? 2 : 1;
    const unsigned BIndex = HasMask ? 3 : 2;
    const unsigned ImmIndex = HasMask ? 4 : 3;
    const cs_x86_op &Destination = X86.operands[0];
    const cs_x86_op &AOperand = X86.operands[AIndex];
    const cs_x86_op &BOperand = X86.operands[BIndex];
    if (!isVectorRegisterOfSize(Destination, Destination.size) ||
        !isVectorRegisterOfSize(AOperand, Destination.size) ||
        (BOperand.type != X86_OP_REG && BOperand.type != X86_OP_MEM) ||
        X86.operands[ImmIndex].type != X86_OP_IMM ||
        (Destination.size != 16 && Destination.size != 32 &&
         Destination.size != 64) ||
        AOperand.size != Destination.size ||
        BOperand.size != Destination.size || X86.operands[ImmIndex].size != 1 ||
        static_cast<uint8_t>(X86.operands[ImmIndex].imm) !=
            Insn->bytes[Insn->size - 1])
      return false;
    auto VectorIndex = [](const cs_x86_op &Operand) -> unsigned {
      if (Operand.size == 16)
        return Operand.reg - X86_REG_XMM0;
      if (Operand.size == 32)
        return Operand.reg - X86_REG_YMM0;
      return Operand.reg - X86_REG_ZMM0;
    };
    const unsigned EncodedLength = (Encoding.P2 >> 5) & 3;
    const uint16_t EncodedSize = EncodedLength == 0   ? 16
                                 : EncodedLength == 1 ? 32
                                                      : 64;
    const unsigned DestinationIndex = VectorIndex(Destination);
    const unsigned ARegisterIndex = VectorIndex(AOperand);
    const unsigned BRegisterIndex =
        BOperand.type == X86_OP_REG ? VectorIndex(BOperand) : 0;
    if (EncodedSize != Destination.size ||
        decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
            DestinationIndex ||
        decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) != ARegisterIndex ||
        (BOperand.type == X86_OP_REG &&
         (!isVectorRegisterOfSize(BOperand, Destination.size) ||
          decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
              BRegisterIndex)) ||
        (L.targetArch() == Arch::X86 &&
         (DestinationIndex >= 8 || ARegisterIndex >= 8 ||
          (BOperand.type == X86_OP_REG && BRegisterIndex >= 8))))
      return false;
    const bool MemoryForm = (Encoding.ModRM & 0xc0) != 0xc0;
    if (MemoryForm != (BOperand.type == X86_OP_MEM) ||
        (MemoryForm
             ? !validateCanonicalEvexMemoryTail(
                   Insn, X86, Encoding, BOperand, Destination.size, 1)
             : !validateCanonicalEvexRegisterTail(Insn, X86, Encoding, 1)))
      return false;
    for (unsigned I = 0; I < X86.op_count; ++I)
      if (X86.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
          (X86.operands[I].avx_zero_opmask && (!HasMask || I != 1)))
        return false;
    if (X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
      return false;
    const unsigned EncodedMask = Encoding.P2 & 7;
    const unsigned WordCount = Destination.size / 2;
    const uint16_t MaskSize =
        static_cast<uint16_t>(std::max(1u, (WordCount + 7u) / 8u));
    if (HasMask) {
      const cs_x86_op &MaskOperand = X86.operands[1];
      const RegInfo MaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
      if (EncodedMask == 0 || EncodedMask != MaskOperand.reg - X86_REG_K0 ||
          MaskOperand.size != MaskSize || MaskInfo.Offset == UINT64_C(0xffff) ||
          MaskInfo.Size < MaskSize ||
          (((Encoding.P2 & 0x80) != 0) != MaskOperand.avx_zero_opmask))
        return false;
    } else if (EncodedMask != 0 || (Encoding.P2 & 0x80) != 0) {
      return false;
    }

    NdVar A = L.operandRead(S, AOperand);
    // E4NF.nb uses the writemask only for destination merging/zeroing.  Its
    // Full Mem tuple is never fault-suppressed, even when every mask bit is 0.
    NdVar B = L.operandRead(S, BOperand);
    if (A.Size != Destination.size || B.Size != Destination.size)
      return false;
    NdVar Raw = S.makeTemp(Destination.size);
    S.emitIntrinsic(
        Intrinsic::Vdbpsadbw, Raw,
        {A, B,
         NdVar::cst(static_cast<uint8_t>(X86.operands[ImmIndex].imm), 1)});
    if (HasMask)
      return emitMaskedVectorResult(L, S, Destination, X86.operands[1], Raw, 2);
    S.emit(NdOp::COPY, L.operandWrite(Destination), {Raw});
    return true;
  }

  // VRSQRT14{PS,PD,SS,SD} / VRCP14{PS,PD,SS,SD} — reciprocal sqrt/reciprocal
  // (14-bit approx).
  case X86_INS_VRSQRT14PS:
  case X86_INS_VRSQRT14PD:
  case X86_INS_VRSQRT14SS:
  case X86_INS_VRSQRT14SD:
  case X86_INS_VRCP14PS:
  case X86_INS_VRCP14PD:
  case X86_INS_VRCP14SS:
  case X86_INS_VRCP14SD:
  case X86_INS_VRSQRT28PS:
  case X86_INS_VRSQRT28PD:
  case X86_INS_VRSQRT28SS:
  case X86_INS_VRSQRT28SD:
  case X86_INS_VRCP28PS:
  case X86_INS_VRCP28PD:
  case X86_INS_VRCP28SS:
  case X86_INS_VRCP28SD:
  case X86_INS_VEXP2PS:
  case X86_INS_VEXP2PD:
    return liftApproxFloat(L, S, Insn, X86);

  // V4FMA{DD,PS}SS / V4FNMA{DD,PS}SS — quad FMA.
  case X86_INS_V4FMADDPS:
  case X86_INS_V4FMADDSS:
  case X86_INS_V4FNMADDPS:
  case X86_INS_V4FNMADDSS: {
    CanonicalEvexEncodingInfo Encoding;
    if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
        X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
        (Encoding.P0 & 0x07) != 2 ||
        ((Encoding.P1 | 0x04) & 0x87) != 0x07 ||
        (Encoding.P2 & 0x10) != 0 || (Encoding.ModRM & 0xc0) == 0xc0)
      return false;
    const bool Scalar =
        InsnId == X86_INS_V4FMADDSS || InsnId == X86_INS_V4FNMADDSS;
    const bool Negative =
        InsnId == X86_INS_V4FNMADDPS || InsnId == X86_INS_V4FNMADDSS;
    const uint8_t ExpectedOpcode =
        static_cast<uint8_t>((Negative ? 0xaa : 0x9a) + (Scalar ? 1 : 0));
    // Packed forms require 512-bit L'L=10.  Scalar forms are LLIG; the
    // decoder currently only materializes L'L=00, while this check remains
    // correct if the fork later accepts the other architecturally ignored
    // encodings.
    if (Encoding.Opcode != ExpectedOpcode ||
        (!Scalar && (Encoding.P2 & 0x60) != 0x40) || X86.avx_sae ||
        X86.avx_rm != X86_AVX_RM_INVALID)
      return false;
    const bool HasMask = X86.op_count == 4;
    if ((!HasMask && X86.op_count != 3) ||
        (HasMask && !isX86OpmaskOperand(X86.operands[1])))
      return false;
    const unsigned SourceIndex = HasMask ? 2 : 1;
    const unsigned MemoryIndex = HasMask ? 3 : 2;
    const cs_x86_op &Destination = X86.operands[0];
    const cs_x86_op &EncodedSource = X86.operands[SourceIndex];
    const cs_x86_op &Memory = X86.operands[MemoryIndex];
    const uint16_t VectorSize = Scalar ? 16 : 64;
    if (!isVectorRegisterOfSize(Destination, VectorSize) ||
        !isVectorRegisterOfSize(EncodedSource, VectorSize) ||
        Memory.type != X86_OP_MEM || Memory.size != 16 ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, Memory, 16, 0))
      return false;
    const x86_reg DestinationBase = Scalar ? X86_REG_XMM0 : X86_REG_ZMM0;
    if (Destination.reg < DestinationBase ||
        Destination.reg > DestinationBase + 31 ||
        decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
            static_cast<unsigned>(Destination.reg - DestinationBase))
      return false;
    for (unsigned I = 0; I < X86.op_count; ++I)
      if (X86.operands[I].avx_bcast != X86_AVX_BCAST_INVALID ||
          (X86.operands[I].avx_zero_opmask && (!HasMask || I != 1)))
        return false;
    const unsigned EncodedMask = Encoding.P2 & 7;
    const uint16_t MaskSize = Scalar ? 1 : 2;
    if (HasMask) {
      const cs_x86_op &MaskOperand = X86.operands[1];
      const RegInfo MaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
      if (EncodedMask == 0 || EncodedMask != MaskOperand.reg - X86_REG_K0 ||
          MaskOperand.size != MaskSize || MaskInfo.Offset == UINT64_C(0xffff) ||
          MaskInfo.Size < MaskSize ||
          (((Encoding.P2 & 0x80) != 0) != MaskOperand.avx_zero_opmask))
        return false;
    } else if (EncodedMask != 0 || (Encoding.P2 & 0x80) != 0) {
      return false;
    }

    const unsigned EncodedBase =
        decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2);
    const unsigned GroupBase = EncodedBase & ~3u;
    auto VectorReg = [&](unsigned Index) {
      return static_cast<x86_reg>((Scalar ? X86_REG_XMM0 : X86_REG_ZMM0) +
                                  Index);
    };
    if (EncodedSource.reg != VectorReg(EncodedBase) || GroupBase + 3 >= 32 ||
        (L.targetArch() == Arch::X86 &&
         (static_cast<unsigned>(Destination.reg - DestinationBase) >= 8 ||
          GroupBase + 3 >= 8)))
      return false;
    for (unsigned I = 0; I < 4; ++I) {
      const RegInfo Info = mapCapstoneReg(VectorReg(GroupBase + I));
      if (Info.Size < VectorSize)
        return false;
    }
    NdVar Mask = NdVar::cst(Scalar ? 1 : 0xffff, Scalar ? 1 : 2);
    if (HasMask) {
      const RegInfo Info =
          mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
      if (Info.Size < MaskSize)
        return false;
      Mask = NdVar::reg(Info.Offset, MaskSize);
    }
    const uint8_t Control = static_cast<uint8_t>(
        (Scalar ? 1 : 0) | (Negative ? 2 : 0) |
        (HasMask && X86.operands[1].avx_zero_opmask ? 4 : 0));
    const NdVar OldDestination = L.operandRead(S, Destination);
    const NdVar ArchitecturalOut = L.operandWrite(Destination);
    const NdVar Out = Scalar ? S.makeTemp(16) : ArchitecturalOut;
    const NdVar Address = S.computeEA(Memory);
    S.emitIntrinsic(Intrinsic::X86FourFMA, Out,
                    {Address, OldDestination, NdVar::cst(GroupBase, 1), Mask,
                     NdVar::cst(Control, 1)},
                    NdMemoryOrdering::None,
                    X86Lifter::LiftState::memoryAddressSpace(Memory));
    if (Scalar) {
      NdVar Full = S.makeTemp(64);
      S.emit(NdOp::CONCAT, Full, {NdVar::cst(0, 48), Out});
      const RegInfo DestinationInfo =
          mapCapstoneReg(static_cast<x86_reg>(Destination.reg));
      S.emit(NdOp::COPY, NdVar::reg(DestinationInfo.Offset, 64), {Full});
    }
    return true;
  }

  // VANDNPS / VANDNPD (AVX/AVX-512 bitwise AND-NOT float).
  case X86_INS_VANDPS:
  case X86_INS_VANDPD:
  case X86_INS_VANDNPS:
  case X86_INS_VANDNPD:
  case X86_INS_VORPS:
  case X86_INS_VORPD:
  case X86_INS_VXORPS:
  case X86_INS_VXORPD: {
    if (beginsWithPotentialEvexPrefix(Insn))
      return liftEvexFPLogic(L, S, Insn, X86);
    if (InsnId != X86_INS_VANDNPS && InsnId != X86_INS_VANDNPD)
      return false;
    if (X86.op_count != 3 || X86.operands[0].type != X86_OP_REG ||
        X86.operands[1].type != X86_OP_REG ||
        (X86.operands[2].type != X86_OP_REG &&
         X86.operands[2].type != X86_OP_MEM) ||
        (X86.operands[0].size != 16 && X86.operands[0].size != 32) ||
        X86.operands[1].size != X86.operands[0].size ||
        X86.operands[2].size != X86.operands[0].size)
      return false;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar A = L.operandRead(S, X86.operands[1]);
    NdVar B = L.operandRead(S, X86.operands[2]);
    if (Dst.Size != A.Size || Dst.Size != B.Size)
      return false;
    NdVar NotA = S.makeTemp(Dst.Size);
    S.emit(NdOp::INT_NOT, NotA, {A});
    S.emit(NdOp::INT_AND, Dst, {NotA, B});
    break;
  }

  // VUNPCKLPS/PD / VUNPCKHPS/PD — interleave independently in each 128-bit
  // lane. EVEX owns writemask, broadcast and fault-suppressed memory semantics;
  // the VEX forms use the same exact LowIR interleave primitive.
  case X86_INS_VUNPCKLPS:
  case X86_INS_VUNPCKLPD:
  case X86_INS_VUNPCKHPS:
  case X86_INS_VUNPCKHPD: {
    if (beginsWithPotentialEvexPrefix(Insn))
      return liftEvexFPUnpack(L, S, Insn, X86);
    if (X86.op_count != 3 || X86.operands[0].type != X86_OP_REG ||
        X86.operands[1].type != X86_OP_REG ||
        (X86.operands[2].type != X86_OP_REG &&
         X86.operands[2].type != X86_OP_MEM) ||
        (X86.operands[0].size != 16 && X86.operands[0].size != 32) ||
        X86.operands[1].size != X86.operands[0].size ||
        X86.operands[2].size != X86.operands[0].size)
      return false;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src1 = L.operandRead(S, X86.operands[1]);
    NdVar Src2 = L.operandRead(S, X86.operands[2]);
    const bool F64 =
        InsnId == X86_INS_VUNPCKLPD || InsnId == X86_INS_VUNPCKHPD;
    const bool HighHalf =
        InsnId == X86_INS_VUNPCKHPS || InsnId == X86_INS_VUNPCKHPD;
    if (!emitPackedUnpack(S, Dst, Src1, Src2, F64 ? 8 : 4, HighHalf))
      return false;
    break;
  }

  // VMOVHPS/VMOVHPD/VMOVLPS/VMOVLPD — partial 64-bit moves.  The old handler
  // did a flat `COPY Dst, last-operand` for everything, which (a) dropped the
  // non-destructive merge source on the 3-operand load form, (b) silently
  // dropped the memory write on the 2-operand store form (L.operandWrite() of a
  // MEM operand is a discarded ram(0) placeholder), and (c) stored/loaded the
  // wrong half for the HIGH variants.
  //   store (2 ops): m64 = xmm[selected half]
  //   load  (3 ops): dst = merge(src1, m64) keeping src1's other half
  case X86_INS_VMOVHPS:
  case X86_INS_VMOVHPD:
  case X86_INS_VMOVLPS:
  case X86_INS_VMOVLPD: {
    if (X86.op_count < 2)
      break;
    bool IsHigh = (InsnId == X86_INS_VMOVHPS || InsnId == X86_INS_VMOVHPD);
    if (X86.operands[0].type == X86_OP_MEM) {
      NdVar Src = L.operandRead(S, X86.operands[1]);
      NdVar Half = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Half, {Src, NdVar::cst(IsHigh ? 8 : 0, 4)});
      S.storeToMem(X86.operands[0], Half);
    } else {
      // Load form is dst,src1,m64 — a narrower operand list would read a stale
      // operands[2] slot.
      if (X86.op_count < 3)
        break;
      NdVar Dst = L.operandWrite(X86.operands[0]);
      NdVar Src1 = L.operandRead(S, X86.operands[1]);
      NdVar Mem = L.operandRead(S, X86.operands[2]);
      NdVar MemLo = Mem;
      if (Mem.Size != 8) {
        MemLo = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, MemLo, {Mem, NdVar::cst(0, 4)});
      }
      if (IsHigh) {
        // high = m64, low = src1.low → {src1.lo, m64}.
        NdVar Lo = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, Lo, {Src1, NdVar::cst(0, 4)});
        S.emit(NdOp::CONCAT, Dst, {MemLo, Lo});
      } else {
        // low = m64, high = src1.high → {m64, src1.hi}.
        NdVar Hi = S.makeTemp(8);
        S.emit(NdOp::SUBBYTES, Hi, {Src1, NdVar::cst(8, 4)});
        S.emit(NdOp::CONCAT, Dst, {Hi, MemLo});
      }
    }
    break;
  }

  // VMOVLHPS xmm1,xmm2,xmm3 → { xmm2[63:0],   xmm3[63:0]  }
  // VMOVHLPS xmm1,xmm2,xmm3 → { xmm3[127:64], xmm2[127:64]}
  case X86_INS_VMOVLHPS:
  case X86_INS_VMOVHLPS: {
    if (X86.op_count < 3)
      break;
    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Src1 = L.operandRead(S, X86.operands[1]);
    NdVar Src2 = L.operandRead(S, X86.operands[2]);
    if (InsnId == X86_INS_VMOVLHPS) {
      NdVar S1Lo = S.makeTemp(8), S2Lo = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, S1Lo, {Src1, NdVar::cst(0, 4)});
      S.emit(NdOp::SUBBYTES, S2Lo, {Src2, NdVar::cst(0, 4)});
      S.emit(NdOp::CONCAT, Dst, {S2Lo, S1Lo}); // {lo=src1.lo, hi=src2.lo}
    } else {
      NdVar S1Hi = S.makeTemp(8), S2Hi = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, S1Hi, {Src1, NdVar::cst(8, 4)});
      S.emit(NdOp::SUBBYTES, S2Hi, {Src2, NdVar::cst(8, 4)});
      S.emit(NdOp::CONCAT, Dst, {S1Hi, S2Hi}); // {lo=src2.hi, hi=src1.hi}
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
