//===- X86LiftSIMDAVXInt.cpp - x86/x64 AVX-512 packed integer lifter ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AVX-512 packed integer (EVEX VP*) instructions: bitwise
/// logic and ternary logic, compress/expand, scatter,
/// population count, leading-zero and conflict detection,
/// mask-producing tests, rotates and variable shifts,
/// multiplies, blends, truncating moves, mask/vector
/// conversions, funnel shifts and dot products.
///
//===----------------------------------------------------------------------===//

#include "X86LiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86Lifter.h"

#include <algorithm>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

bool beginsWithPotentialEvexPrefix(const cs_insn *Insn) {
  if (!Insn)
    return false;
  for (size_t Offset = 0; Offset < Insn->size; ++Offset) {
    const uint8_t Byte = Insn->bytes[Offset];
    if (Byte == 0x62)
      return true;
    if (Byte != 0x26 && Byte != 0x2e && Byte != 0x36 && Byte != 0x3e &&
        Byte != 0x64 && Byte != 0x65 && Byte != 0x67)
      return false;
  }
  return false;
}

bool isX86VectorRegisterOperand(const cs_x86_op &Operand) {
  if (Operand.type != X86_OP_REG)
    return false;
  return (Operand.reg >= X86_REG_XMM0 && Operand.reg <= X86_REG_XMM31) ||
         (Operand.reg >= X86_REG_YMM0 && Operand.reg <= X86_REG_YMM31) ||
         (Operand.reg >= X86_REG_ZMM0 && Operand.reg <= X86_REG_ZMM31);
}

bool isX86VectorRegisterOfSize(const cs_x86_op &Operand, uint16_t Size) {
  if (!isX86VectorRegisterOperand(Operand) || Operand.size != Size)
    return false;
  if (Size == 16)
    return Operand.reg >= X86_REG_XMM0 && Operand.reg <= X86_REG_XMM31;
  if (Size == 32)
    return Operand.reg >= X86_REG_YMM0 && Operand.reg <= X86_REG_YMM31;
  if (Size == 64)
    return Operand.reg >= X86_REG_ZMM0 && Operand.reg <= X86_REG_ZMM31;
  return false;
}

unsigned x86VectorRegisterIndex(const cs_x86_op &Operand) {
  if (Operand.size == 16)
    return static_cast<unsigned>(Operand.reg - X86_REG_XMM0);
  if (Operand.size == 32)
    return static_cast<unsigned>(Operand.reg - X86_REG_YMM0);
  return static_cast<unsigned>(Operand.reg - X86_REG_ZMM0);
}

x86_avx_bcast evexBroadcastForLaneCount(unsigned LaneCount) {
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

struct EvexIntegerLogicInfo {
  CanonicalEvexEncodingInfo Encoding;
  const cs_x86_op *MaskOperand = nullptr;
  unsigned LeftIndex = 0;
  unsigned RightIndex = 0;
  uint16_t VectorSize = 0;
  uint16_t ElementSize = 0;
  uint16_t MaskSize = 0;
  unsigned LaneCount = 0;
  bool MemoryForm = false;
  bool Broadcast = false;
  bool ZeroMask = false;
  bool AndNot = false;
  NdOp Operation = NdOp::INT_AND;
};

bool getEvexIntegerLogicEncoding(unsigned InsnId, uint8_t &Opcode,
                                 bool &W, bool &AndNot,
                                 NdOp &Operation) {
  AndNot = false;
  switch (InsnId) {
  case X86_INS_VPANDD:
    Opcode = 0xdb;
    W = false;
    Operation = NdOp::INT_AND;
    return true;
  case X86_INS_VPANDQ:
    Opcode = 0xdb;
    W = true;
    Operation = NdOp::INT_AND;
    return true;
  case X86_INS_VPANDND:
    Opcode = 0xdf;
    W = false;
    AndNot = true;
    Operation = NdOp::INT_AND;
    return true;
  case X86_INS_VPANDNQ:
    Opcode = 0xdf;
    W = true;
    AndNot = true;
    Operation = NdOp::INT_AND;
    return true;
  case X86_INS_VPORD:
    Opcode = 0xeb;
    W = false;
    Operation = NdOp::INT_OR;
    return true;
  case X86_INS_VPORQ:
    Opcode = 0xeb;
    W = true;
    Operation = NdOp::INT_OR;
    return true;
  case X86_INS_VPXORD:
    Opcode = 0xef;
    W = false;
    Operation = NdOp::INT_XOR;
    return true;
  case X86_INS_VPXORQ:
    Opcode = 0xef;
    W = true;
    Operation = NdOp::INT_XOR;
    return true;
  default:
    return false;
  }
}

bool validateEvexIntegerLogic(X86Lifter &L, const cs_insn *Insn,
                              const cs_x86 &X86,
                              EvexIntegerLogicInfo &Info) {
  uint8_t Opcode = 0;
  bool W = false;
  if (!Insn || !getEvexIntegerLogicEncoding(
                   Insn->id, Opcode, W, Info.AndNot, Info.Operation) ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                      Info.Encoding) ||
      (Info.Encoding.P0 & 0x07) != 0x01 ||
      ((Info.Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((W ? 0x80 : 0) | 0x05) ||
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
  Info.ElementSize = W ? 8 : 4;
  if (Info.VectorSize != 16 && Info.VectorSize != 32 && Info.VectorSize != 64)
    return false;
  Info.LaneCount = Info.VectorSize / Info.ElementSize;
  Info.MaskSize =
      static_cast<uint16_t>(std::max(1u, (Info.LaneCount + 7u) / 8u));
  Info.MemoryForm = Right.type == X86_OP_MEM;
  Info.Broadcast = Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0;
  Info.ZeroMask = (Info.Encoding.P2 & 0x80) != 0;

  const uint8_t EncodedLength = Info.Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Info.VectorSize == 16 ? 0 : (Info.VectorSize == 32 ? 0x20 : 0x40);
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      (((Info.Encoding.ModRM & 0xc0) != 0xc0) != Info.MemoryForm) ||
      (!Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0) ||
      !isX86VectorRegisterOfSize(Destination, Info.VectorSize) ||
      !isX86VectorRegisterOfSize(Left, Info.VectorSize) ||
      decodeEvexVectorRegIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
          x86VectorRegisterIndex(Left))
    return false;
  if (L.targetArch() == Arch::X86 &&
      (x86VectorRegisterIndex(Destination) >= 8 ||
       x86VectorRegisterIndex(Left) >= 8))
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
      Info.Broadcast ? evexBroadcastForLaneCount(Info.LaneCount)
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

  if (Info.MemoryForm) {
    const uint16_t TupleSize =
        Info.Broadcast ? Info.ElementSize : Info.VectorSize;
    return Right.size == TupleSize && validateCanonicalEvexMemoryTail(
                                          Insn, X86, Info.Encoding, Right,
                                          TupleSize);
  }
  if (!isX86VectorRegisterOfSize(Right, Info.VectorSize) ||
      decodeEvexVectorRMIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Right) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Info.Encoding))
    return false;
  return L.targetArch() != Arch::X86 || x86VectorRegisterIndex(Right) < 8;
}

enum class EvexUnaryIntegerOperation {
  PopulationCount,
  LeadingZeroCount,
  Conflict,
  Absolute,
};

struct EvexUnaryIntegerSpec {
  EvexUnaryIntegerOperation Operation;
  uint8_t Opcode;
  uint16_t ElementSize;
  bool W;
  bool BroadcastAllowed;
};

struct EvexUnaryIntegerInfo {
  CanonicalEvexEncodingInfo Encoding;
  EvexUnaryIntegerSpec Spec{};
  const cs_x86_op *MaskOperand = nullptr;
  unsigned SourceIndex = 0;
  uint16_t VectorSize = 0;
  uint16_t MaskSize = 0;
  unsigned LaneCount = 0;
  bool MemoryForm = false;
  bool Broadcast = false;
  bool ZeroMask = false;
};

bool getEvexUnaryIntegerSpec(unsigned InsnId, EvexUnaryIntegerSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPOPCNTB:
    Spec = {EvexUnaryIntegerOperation::PopulationCount, 0x54, 1, false,
            false};
    return true;
  case X86_INS_VPOPCNTW:
    Spec = {EvexUnaryIntegerOperation::PopulationCount, 0x54, 2, true,
            false};
    return true;
  case X86_INS_VPOPCNTD:
    Spec = {EvexUnaryIntegerOperation::PopulationCount, 0x55, 4, false,
            true};
    return true;
  case X86_INS_VPOPCNTQ:
    Spec = {EvexUnaryIntegerOperation::PopulationCount, 0x55, 8, true, true};
    return true;
  case X86_INS_VPLZCNTD:
    Spec = {EvexUnaryIntegerOperation::LeadingZeroCount, 0x44, 4, false,
            true};
    return true;
  case X86_INS_VPLZCNTQ:
    Spec = {EvexUnaryIntegerOperation::LeadingZeroCount, 0x44, 8, true, true};
    return true;
  case X86_INS_VPCONFLICTD:
    Spec = {EvexUnaryIntegerOperation::Conflict, 0xc4, 4, false, true};
    return true;
  case X86_INS_VPCONFLICTQ:
    Spec = {EvexUnaryIntegerOperation::Conflict, 0xc4, 8, true, true};
    return true;
  case X86_INS_VPABSQ:
    Spec = {EvexUnaryIntegerOperation::Absolute, 0x1f, 8, true, true};
    return true;
  default:
    return false;
  }
}

bool validateEvexUnaryInteger(X86Lifter &L, const cs_insn *Insn,
                              const cs_x86 &X86,
                              EvexUnaryIntegerInfo &Info) {
  if (!Insn || !getEvexUnaryIntegerSpec(Insn->id, Info.Spec) ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                      Info.Encoding) ||
      (Info.Encoding.P0 & 0x07) != 0x02 ||
      Info.Encoding.P1 !=
          static_cast<uint8_t>((Info.Spec.W ? 0x80 : 0) | 0x7d) ||
      Info.Encoding.Opcode != Info.Spec.Opcode ||
      (Info.Encoding.P2 & 0x08) == 0 || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0 || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool HasMask = X86.op_count == 3;
  if (X86.op_count != 2 && !HasMask)
    return false;
  Info.MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  Info.SourceIndex = HasMask ? 2 : 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &Source = X86.operands[Info.SourceIndex];
  Info.VectorSize = static_cast<uint16_t>(Destination.size);
  if (Info.VectorSize != 16 && Info.VectorSize != 32 &&
      Info.VectorSize != 64)
    return false;
  Info.LaneCount = Info.VectorSize / Info.Spec.ElementSize;
  Info.MaskSize =
      static_cast<uint16_t>(std::max(1u, (Info.LaneCount + 7u) / 8u));
  Info.MemoryForm = Source.type == X86_OP_MEM;
  Info.Broadcast = Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0;
  Info.ZeroMask = (Info.Encoding.P2 & 0x80) != 0;

  const uint8_t EncodedLength = Info.Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Info.VectorSize == 16 ? 0 : (Info.VectorSize == 32 ? 0x20 : 0x40);
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      (((Info.Encoding.ModRM & 0xc0) != 0xc0) != Info.MemoryForm) ||
      (!Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0) ||
      (Info.Broadcast && !Info.Spec.BroadcastAllowed) ||
      !isX86VectorRegisterOfSize(Destination, Info.VectorSize) ||
      decodeEvexVectorRegIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Destination))
    return false;
  if (L.targetArch() == Arch::X86 &&
      x86VectorRegisterIndex(Destination) >= 8)
    return false;

  const uint8_t EncodedMask = Info.Encoding.P2 & 7;
  if (Info.MaskOperand) {
    if (!isX86OpmaskOperand(*Info.MaskOperand) ||
        Info.MaskOperand->reg == X86_REG_K0 ||
        Info.MaskOperand->size != Info.MaskSize ||
        EncodedMask !=
            static_cast<uint8_t>(Info.MaskOperand->reg - X86_REG_K0) ||
        Info.ZeroMask !=
            static_cast<bool>(Info.MaskOperand->avx_zero_opmask))
      return false;
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < Info.MaskSize)
      return false;
  } else if (EncodedMask != 0 || Info.ZeroMask) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      Info.Broadcast ? evexBroadcastForLaneCount(Info.LaneCount)
                     : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    const bool IsMask = Info.MaskOperand && &Operand == Info.MaskOperand;
    const bool IsSource = &Operand == &Source;
    if (Operand.avx_zero_opmask != (IsMask && Info.ZeroMask) ||
        Operand.avx_bcast !=
            (IsSource ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }

  if (Info.MemoryForm) {
    const uint16_t TupleSize =
        Info.Broadcast ? Info.Spec.ElementSize : Info.VectorSize;
    return Source.size == TupleSize &&
           validateCanonicalEvexMemoryTail(Insn, X86, Info.Encoding, Source,
                                           TupleSize);
  }
  if (!isX86VectorRegisterOfSize(Source, Info.VectorSize) ||
      decodeEvexVectorRMIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Source) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Info.Encoding))
    return false;
  return L.targetArch() != Arch::X86 || x86VectorRegisterIndex(Source) < 8;
}

NdVar evexUnaryActiveMask(const EvexUnaryIntegerInfo &Info) {
  if (Info.MaskOperand) {
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    return NdVar::reg(MaskInfo.Offset, Info.MaskSize);
  }
  const uint64_t AllActive =
      Info.LaneCount == 64
          ? UINT64_MAX
          : ((UINT64_C(1) << Info.LaneCount) - UINT64_C(1));
  return NdVar::cst(AllActive, Info.MaskSize);
}

NdVar evexConflictMemoryMask(X86Lifter::LiftState &S, NdVar ActiveMask,
                             unsigned LaneCount) {
  NdVar LoadMask = ActiveMask;
  for (unsigned Shift = 1; Shift < LaneCount; Shift <<= 1) {
    NdVar Shifted = S.makeTemp(LoadMask.Size);
    S.emit(NdOp::INT_RIGHT, Shifted,
           {LoadMask, NdVar::cst(Shift, LoadMask.Size)});
    NdVar Spread = S.makeTemp(LoadMask.Size);
    S.emit(NdOp::INT_OR, Spread, {LoadMask, Shifted});
    LoadMask = Spread;
  }
  return LoadMask;
}

bool liftEvexUnaryInteger(X86Lifter &L, X86Lifter::LiftState &S,
                          const cs_insn *Insn, const cs_x86 &X86) {
  EvexUnaryIntegerInfo Info;
  if (!validateEvexUnaryInteger(L, Insn, X86, Info))
    return false;

  const NdVar ActiveMask = evexUnaryActiveMask(Info);
  const cs_x86_op &SourceOperand = X86.operands[Info.SourceIndex];
  NdVar Source;
  if (Info.MemoryForm) {
    NdVar LoadMask = ActiveMask;
    if (Info.Spec.Operation == EvexUnaryIntegerOperation::Conflict)
      LoadMask = evexConflictMemoryMask(S, ActiveMask, Info.LaneCount);
    Source = emitEvexMaskedMemoryLoad(
        S, SourceOperand, LoadMask, Info.VectorSize, Info.Spec.ElementSize,
        Info.Broadcast ? Info.Spec.ElementSize : Info.VectorSize,
        Info.Broadcast);
  } else {
    Source = L.operandRead(S, SourceOperand);
  }
  if (Source.Size != Info.VectorSize)
    return false;

  NdVar Raw = S.makeTemp(0);
  for (unsigned Lane = 0; Lane < Info.LaneCount; ++Lane) {
    NdVar InputLane = S.makeTemp(Info.Spec.ElementSize);
    S.emit(NdOp::SUBBYTES, InputLane,
           {Source, NdVar::cst(Lane * Info.Spec.ElementSize, 4)});
    NdVar ResultLane;
    switch (Info.Spec.Operation) {
    case EvexUnaryIntegerOperation::PopulationCount:
      ResultLane = S.makeTemp(Info.Spec.ElementSize);
      S.emit(NdOp::POPCOUNT, ResultLane, {InputLane});
      break;
    case EvexUnaryIntegerOperation::LeadingZeroCount:
      ResultLane = S.makeTemp(Info.Spec.ElementSize);
      S.emit(NdOp::LZCOUNT, ResultLane, {InputLane});
      break;
    case EvexUnaryIntegerOperation::Absolute: {
      NdVar IsNegative = S.makeTemp(1);
      S.emit(NdOp::INT_SLESS, IsNegative,
             {InputLane, NdVar::cst(0, Info.Spec.ElementSize)});
      NdVar Negated = S.makeTemp(Info.Spec.ElementSize);
      S.emit(NdOp::INT_SUB, Negated,
             {NdVar::cst(0, Info.Spec.ElementSize), InputLane});
      ResultLane = S.makeTemp(Info.Spec.ElementSize);
      S.emit(NdOp::SELECT, ResultLane,
             {IsNegative, Negated, InputLane});
      break;
    }
    case EvexUnaryIntegerOperation::Conflict: {
      ResultLane = NdVar::cst(0, Info.Spec.ElementSize);
      for (unsigned Prior = 0; Prior < Lane; ++Prior) {
        NdVar Previous = S.makeTemp(Info.Spec.ElementSize);
        S.emit(NdOp::SUBBYTES, Previous,
               {Source, NdVar::cst(Prior * Info.Spec.ElementSize, 4)});
        NdVar Equal = S.makeTemp(1);
        S.emit(NdOp::INT_EQUAL, Equal, {InputLane, Previous});
        NdVar Bit = S.makeTemp(Info.Spec.ElementSize);
        S.emit(NdOp::INT_ZEXT, Bit, {Equal});
        if (Prior != 0) {
          NdVar Shifted = S.makeTemp(Info.Spec.ElementSize);
          S.emit(NdOp::INT_LEFT, Shifted,
                 {Bit, NdVar::cst(Prior, Info.Spec.ElementSize)});
          Bit = Shifted;
        }
        NdVar Next = S.makeTemp(Info.Spec.ElementSize);
        S.emit(NdOp::INT_OR, Next, {ResultLane, Bit});
        ResultLane = Next;
      }
      break;
    }
    }

    if (Lane == 0) {
      Raw = ResultLane;
    } else {
      NdVar Next = S.makeTemp(Raw.Size + Info.Spec.ElementSize);
      S.emit(NdOp::CONCAT, Next, {ResultLane, Raw});
      Raw = Next;
    }
  }

  if (Info.MaskOperand)
    return emitMaskedVectorResult(L, S, X86.operands[0], *Info.MaskOperand,
                                  Raw, Info.Spec.ElementSize);
  S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {Raw});
  return true;
}

struct EvexRotateSpec {
  uint8_t Map;
  uint8_t Opcode;
  uint8_t ModRMExtension;
  uint16_t ElementSize;
  bool W;
  bool Immediate;
  bool Left;
};

struct EvexRotateInfo {
  CanonicalEvexEncodingInfo Encoding;
  EvexRotateSpec Spec{};
  const cs_x86_op *MaskOperand = nullptr;
  unsigned ValueIndex = 0;
  unsigned CountIndex = 0;
  uint16_t VectorSize = 0;
  uint16_t MaskSize = 0;
  unsigned LaneCount = 0;
  bool MemoryForm = false;
  bool Broadcast = false;
  bool ZeroMask = false;
};

bool getEvexRotateSpec(unsigned InsnId, EvexRotateSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPROLD:
    Spec = {1, 0x72, 1, 4, false, true, true};
    return true;
  case X86_INS_VPROLQ:
    Spec = {1, 0x72, 1, 8, true, true, true};
    return true;
  case X86_INS_VPRORD:
    Spec = {1, 0x72, 0, 4, false, true, false};
    return true;
  case X86_INS_VPRORQ:
    Spec = {1, 0x72, 0, 8, true, true, false};
    return true;
  case X86_INS_VPROLVD:
    Spec = {2, 0x15, 0, 4, false, false, true};
    return true;
  case X86_INS_VPROLVQ:
    Spec = {2, 0x15, 0, 8, true, false, true};
    return true;
  case X86_INS_VPRORVD:
    Spec = {2, 0x14, 0, 4, false, false, false};
    return true;
  case X86_INS_VPRORVQ:
    Spec = {2, 0x14, 0, 8, true, false, false};
    return true;
  default:
    return false;
  }
}

bool validateEvexRotate(X86Lifter &L, const cs_insn *Insn,
                        const cs_x86 &X86, EvexRotateInfo &Info) {
  if (!Insn || !getEvexRotateSpec(Insn->id, Info.Spec) ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                      Info.Encoding) ||
      (Info.Encoding.P0 & 0x07) != Info.Spec.Map ||
      (Info.Encoding.P1 & 0x87) !=
          static_cast<uint8_t>((Info.Spec.W ? 0x80 : 0) | 0x05) ||
      Info.Encoding.Opcode != Info.Spec.Opcode || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool HasMask =
      X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
  const unsigned ExpectedOperands =
      Info.Spec.Immediate ? (HasMask ? 4 : 3) : (HasMask ? 4 : 3);
  if (X86.op_count != ExpectedOperands)
    return false;
  Info.MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  Info.ValueIndex = HasMask ? 2 : 1;
  Info.CountIndex = Info.ValueIndex + 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &Value = X86.operands[Info.ValueIndex];
  const cs_x86_op &TailOperand = X86.operands[Info.CountIndex];
  const cs_x86_op &MemoryOrRegister =
      Info.Spec.Immediate ? Value : TailOperand;
  Info.VectorSize = static_cast<uint16_t>(Destination.size);
  if (Info.VectorSize != 16 && Info.VectorSize != 32 &&
      Info.VectorSize != 64)
    return false;
  Info.LaneCount = Info.VectorSize / Info.Spec.ElementSize;
  Info.MaskSize =
      static_cast<uint16_t>(std::max(1u, (Info.LaneCount + 7u) / 8u));
  Info.MemoryForm = MemoryOrRegister.type == X86_OP_MEM;
  Info.Broadcast = Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0;
  Info.ZeroMask = (Info.Encoding.P2 & 0x80) != 0;

  const uint8_t EncodedLength = Info.Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Info.VectorSize == 16 ? 0 : (Info.VectorSize == 32 ? 0x20 : 0x40);
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      (((Info.Encoding.ModRM & 0xc0) != 0xc0) != Info.MemoryForm) ||
      (!Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0) ||
      !isX86VectorRegisterOfSize(Destination, Info.VectorSize))
    return false;

  const unsigned DestinationIndex = x86VectorRegisterIndex(Destination);
  if (Info.Spec.Immediate) {
    if ((Info.Encoding.P0 & 0x90) != 0x90 ||
        ((Info.Encoding.ModRM >> 3) & 7) != Info.Spec.ModRMExtension ||
        decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
            DestinationIndex ||
        TailOperand.type != X86_OP_IMM || TailOperand.size != 1 ||
        X86.encoding.imm_size != 1 ||
        X86.encoding.imm_offset != Insn->size - 1 ||
        Insn->bytes[Insn->size - 1] !=
            static_cast<uint8_t>(TailOperand.imm))
      return false;
  } else {
    if (!isX86VectorRegisterOfSize(Value, Info.VectorSize) ||
        decodeEvexVectorRegIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
            DestinationIndex ||
        decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
            x86VectorRegisterIndex(Value) ||
        X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0)
      return false;
  }
  if (L.targetArch() == Arch::X86 &&
      (DestinationIndex >= 8 ||
       (!Info.Spec.Immediate && x86VectorRegisterIndex(Value) >= 8)))
    return false;

  const uint8_t EncodedMask = Info.Encoding.P2 & 7;
  if (Info.MaskOperand) {
    if (Info.MaskOperand->reg == X86_REG_K0 ||
        Info.MaskOperand->size != Info.MaskSize ||
        EncodedMask !=
            static_cast<uint8_t>(Info.MaskOperand->reg - X86_REG_K0) ||
        Info.ZeroMask !=
            static_cast<bool>(Info.MaskOperand->avx_zero_opmask))
      return false;
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < Info.MaskSize)
      return false;
  } else if (EncodedMask != 0 || Info.ZeroMask) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      Info.Broadcast ? evexBroadcastForLaneCount(Info.LaneCount)
                     : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    const bool IsMask = Info.MaskOperand && &Operand == Info.MaskOperand;
    const bool IsMemoryOrRegister = &Operand == &MemoryOrRegister;
    if (Operand.avx_zero_opmask != (IsMask && Info.ZeroMask) ||
        Operand.avx_bcast !=
            (IsMemoryOrRegister ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }

  const size_t TrailingBytes = Info.Spec.Immediate ? 1 : 0;
  if (Info.MemoryForm) {
    const uint16_t TupleSize =
        Info.Broadcast ? Info.Spec.ElementSize : Info.VectorSize;
    return MemoryOrRegister.size == TupleSize &&
           validateCanonicalEvexMemoryTail(Insn, X86, Info.Encoding,
                                           MemoryOrRegister, TupleSize,
                                           TrailingBytes);
  }
  if (!isX86VectorRegisterOfSize(MemoryOrRegister, Info.VectorSize) ||
      decodeEvexVectorRMIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(MemoryOrRegister) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Info.Encoding,
                                         TrailingBytes))
    return false;
  return L.targetArch() != Arch::X86 ||
         x86VectorRegisterIndex(MemoryOrRegister) < 8;
}

bool liftEvexRotate(X86Lifter &L, X86Lifter::LiftState &S,
                    const cs_insn *Insn, const cs_x86 &X86) {
  EvexRotateInfo Info;
  if (!validateEvexRotate(L, Insn, X86, Info))
    return false;

  NdVar ActiveMask;
  if (Info.MaskOperand) {
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    ActiveMask = NdVar::reg(MaskInfo.Offset, Info.MaskSize);
  } else {
    const uint64_t AllActive =
        (UINT64_C(1) << Info.LaneCount) - UINT64_C(1);
    ActiveMask = NdVar::cst(AllActive, Info.MaskSize);
  }

  const cs_x86_op &ValueOperand = X86.operands[Info.ValueIndex];
  const cs_x86_op &CountOperand = X86.operands[Info.CountIndex];
  NdVar Value;
  NdVar Counts;
  if (Info.Spec.Immediate) {
    Value = Info.MemoryForm
                ? emitEvexMaskedMemoryLoad(
                      S, ValueOperand, ActiveMask, Info.VectorSize,
                      Info.Spec.ElementSize,
                      Info.Broadcast ? Info.Spec.ElementSize : Info.VectorSize,
                      Info.Broadcast)
                : L.operandRead(S, ValueOperand);
  } else {
    Value = L.operandRead(S, ValueOperand);
    Counts = Info.MemoryForm
                 ? emitEvexMaskedMemoryLoad(
                       S, CountOperand, ActiveMask, Info.VectorSize,
                       Info.Spec.ElementSize,
                       Info.Broadcast ? Info.Spec.ElementSize
                                      : Info.VectorSize,
                       Info.Broadcast)
                 : L.operandRead(S, CountOperand);
  }
  if (Value.Size != Info.VectorSize ||
      (!Info.Spec.Immediate && Counts.Size != Info.VectorSize))
    return false;

  const unsigned LaneBits = Info.Spec.ElementSize * 8;
  NdVar Raw = S.makeTemp(0);
  for (unsigned Lane = 0; Lane < Info.LaneCount; ++Lane) {
    const uint64_t Offset = static_cast<uint64_t>(Lane) * Info.Spec.ElementSize;
    NdVar InputLane = S.makeTemp(Info.Spec.ElementSize);
    S.emit(NdOp::SUBBYTES, InputLane, {Value, NdVar::cst(Offset, 4)});
    NdVar MaskedCount;
    if (Info.Spec.Immediate) {
      MaskedCount = NdVar::cst(
          static_cast<uint64_t>(CountOperand.imm) & (LaneBits - 1),
          Info.Spec.ElementSize);
    } else {
      NdVar CountLane = S.makeTemp(Info.Spec.ElementSize);
      S.emit(NdOp::SUBBYTES, CountLane, {Counts, NdVar::cst(Offset, 4)});
      MaskedCount = S.makeTemp(Info.Spec.ElementSize);
      S.emit(NdOp::INT_AND, MaskedCount,
             {CountLane, NdVar::cst(LaneBits - 1, Info.Spec.ElementSize)});
    }
    NdVar Complement = S.makeTemp(Info.Spec.ElementSize);
    S.emit(NdOp::INT_SUB, Complement,
           {NdVar::cst(0, Info.Spec.ElementSize), MaskedCount});
    NdVar MaskedComplement = S.makeTemp(Info.Spec.ElementSize);
    S.emit(NdOp::INT_AND, MaskedComplement,
           {Complement, NdVar::cst(LaneBits - 1, Info.Spec.ElementSize)});
    NdVar First = S.makeTemp(Info.Spec.ElementSize);
    NdVar Second = S.makeTemp(Info.Spec.ElementSize);
    S.emit(Info.Spec.Left ? NdOp::INT_LEFT : NdOp::INT_RIGHT, First,
           {InputLane, MaskedCount});
    S.emit(Info.Spec.Left ? NdOp::INT_RIGHT : NdOp::INT_LEFT, Second,
           {InputLane, MaskedComplement});
    NdVar ResultLane = S.makeTemp(Info.Spec.ElementSize);
    S.emit(NdOp::INT_OR, ResultLane, {First, Second});
    if (Lane == 0) {
      Raw = ResultLane;
    } else {
      NdVar Next = S.makeTemp(Raw.Size + Info.Spec.ElementSize);
      S.emit(NdOp::CONCAT, Next, {ResultLane, Raw});
      Raw = Next;
    }
  }

  if (Info.MaskOperand)
    return emitMaskedVectorResult(L, S, X86.operands[0], *Info.MaskOperand,
                                  Raw, Info.Spec.ElementSize);
  S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {Raw});
  return true;
}

struct EvexVariableShiftSpec {
  uint8_t Opcode;
  uint16_t ElementSize;
  bool W;
  bool BroadcastAllowed;
  NdOp Operation;
};

struct EvexVariableShiftInfo {
  CanonicalEvexEncodingInfo Encoding;
  EvexVariableShiftSpec Spec{};
  const cs_x86_op *MaskOperand = nullptr;
  unsigned ValueIndex = 0;
  unsigned CountIndex = 0;
  uint16_t VectorSize = 0;
  uint16_t MaskSize = 0;
  unsigned LaneCount = 0;
  bool MemoryForm = false;
  bool Broadcast = false;
  bool ZeroMask = false;
};

bool getEvexVariableShiftSpec(unsigned InsnId,
                              EvexVariableShiftSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPSLLVW:
    Spec = {0x12, 2, true, false, NdOp::INT_LEFT};
    return true;
  case X86_INS_VPSLLVD:
    Spec = {0x47, 4, false, true, NdOp::INT_LEFT};
    return true;
  case X86_INS_VPSLLVQ:
    Spec = {0x47, 8, true, true, NdOp::INT_LEFT};
    return true;
  case X86_INS_VPSRLVW:
    Spec = {0x10, 2, true, false, NdOp::INT_RIGHT};
    return true;
  case X86_INS_VPSRLVD:
    Spec = {0x45, 4, false, true, NdOp::INT_RIGHT};
    return true;
  case X86_INS_VPSRLVQ:
    Spec = {0x45, 8, true, true, NdOp::INT_RIGHT};
    return true;
  case X86_INS_VPSRAVW:
    Spec = {0x11, 2, true, false, NdOp::INT_ASHR};
    return true;
  case X86_INS_VPSRAVD:
    Spec = {0x46, 4, false, true, NdOp::INT_ASHR};
    return true;
  case X86_INS_VPSRAVQ:
    Spec = {0x46, 8, true, true, NdOp::INT_ASHR};
    return true;
  default:
    return false;
  }
}

bool validateEvexVariableShift(X86Lifter &L, const cs_insn *Insn,
                               const cs_x86 &X86,
                               EvexVariableShiftInfo &Info) {
  if (!Insn || !getEvexVariableShiftSpec(Insn->id, Info.Spec) ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                      Info.Encoding) ||
      (Info.Encoding.P0 & 0x07) != 0x02 ||
      (Info.Encoding.P1 & 0x87) !=
          static_cast<uint8_t>((Info.Spec.W ? 0x80 : 0) | 0x05) ||
      Info.Encoding.Opcode != Info.Spec.Opcode ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool HasMask =
      X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
  if (X86.op_count != (HasMask ? 4 : 3))
    return false;
  Info.MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  Info.ValueIndex = HasMask ? 2 : 1;
  Info.CountIndex = Info.ValueIndex + 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &Value = X86.operands[Info.ValueIndex];
  const cs_x86_op &Count = X86.operands[Info.CountIndex];
  Info.VectorSize = static_cast<uint16_t>(Destination.size);
  if (Info.VectorSize != 16 && Info.VectorSize != 32 &&
      Info.VectorSize != 64)
    return false;
  Info.LaneCount = Info.VectorSize / Info.Spec.ElementSize;
  Info.MaskSize =
      static_cast<uint16_t>(std::max(1u, (Info.LaneCount + 7u) / 8u));
  Info.MemoryForm = Count.type == X86_OP_MEM;
  Info.Broadcast = Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0;
  Info.ZeroMask = (Info.Encoding.P2 & 0x80) != 0;

  const uint8_t EncodedLength = Info.Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Info.VectorSize == 16 ? 0 : (Info.VectorSize == 32 ? 0x20 : 0x40);
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      (((Info.Encoding.ModRM & 0xc0) != 0xc0) != Info.MemoryForm) ||
      (!Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0) ||
      (Info.Broadcast && !Info.Spec.BroadcastAllowed) ||
      !isX86VectorRegisterOfSize(Destination, Info.VectorSize) ||
      !isX86VectorRegisterOfSize(Value, Info.VectorSize) ||
      decodeEvexVectorRegIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
          x86VectorRegisterIndex(Value))
    return false;
  if (L.targetArch() == Arch::X86 &&
      (x86VectorRegisterIndex(Destination) >= 8 ||
       x86VectorRegisterIndex(Value) >= 8))
    return false;

  const uint8_t EncodedMask = Info.Encoding.P2 & 7;
  if (Info.MaskOperand) {
    if (Info.MaskOperand->reg == X86_REG_K0 ||
        Info.MaskOperand->size != Info.MaskSize ||
        EncodedMask !=
            static_cast<uint8_t>(Info.MaskOperand->reg - X86_REG_K0) ||
        Info.ZeroMask !=
            static_cast<bool>(Info.MaskOperand->avx_zero_opmask))
      return false;
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < Info.MaskSize)
      return false;
  } else if (EncodedMask != 0 || Info.ZeroMask) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      Info.Broadcast ? evexBroadcastForLaneCount(Info.LaneCount)
                     : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    const bool IsMask = Info.MaskOperand && &Operand == Info.MaskOperand;
    const bool IsCount = &Operand == &Count;
    if (Operand.avx_zero_opmask != (IsMask && Info.ZeroMask) ||
        Operand.avx_bcast !=
            (IsCount ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }

  if (Info.MemoryForm) {
    const uint16_t TupleSize =
        Info.Broadcast ? Info.Spec.ElementSize : Info.VectorSize;
    return Count.size == TupleSize &&
           validateCanonicalEvexMemoryTail(Insn, X86, Info.Encoding, Count,
                                           TupleSize);
  }
  if (!isX86VectorRegisterOfSize(Count, Info.VectorSize) ||
      decodeEvexVectorRMIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Count) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Info.Encoding))
    return false;
  return L.targetArch() != Arch::X86 || x86VectorRegisterIndex(Count) < 8;
}

bool liftEvexVariableShift(X86Lifter &L, X86Lifter::LiftState &S,
                           const cs_insn *Insn, const cs_x86 &X86) {
  EvexVariableShiftInfo Info;
  if (!validateEvexVariableShift(L, Insn, X86, Info))
    return false;

  NdVar ActiveMask;
  if (Info.MaskOperand) {
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    ActiveMask = NdVar::reg(MaskInfo.Offset, Info.MaskSize);
  } else {
    const uint64_t AllActive =
        (UINT64_C(1) << Info.LaneCount) - UINT64_C(1);
    ActiveMask = NdVar::cst(AllActive, Info.MaskSize);
  }
  const cs_x86_op &CountOperand = X86.operands[Info.CountIndex];
  const NdVar Value = L.operandRead(S, X86.operands[Info.ValueIndex]);
  const NdVar Counts = Info.MemoryForm
                           ? emitEvexMaskedMemoryLoad(
                                 S, CountOperand, ActiveMask, Info.VectorSize,
                                 Info.Spec.ElementSize,
                                 Info.Broadcast ? Info.Spec.ElementSize
                                                : Info.VectorSize,
                                 Info.Broadcast)
                           : L.operandRead(S, CountOperand);
  if (Value.Size != Info.VectorSize || Counts.Size != Info.VectorSize)
    return false;

  const unsigned LaneBits = Info.Spec.ElementSize * 8;
  const bool Arithmetic = Info.Spec.Operation == NdOp::INT_ASHR;
  NdVar Raw = S.makeTemp(0);
  for (unsigned Lane = 0; Lane < Info.LaneCount; ++Lane) {
    const uint64_t Offset = static_cast<uint64_t>(Lane) * Info.Spec.ElementSize;
    NdVar InputLane = S.makeTemp(Info.Spec.ElementSize);
    NdVar CountLane = S.makeTemp(Info.Spec.ElementSize);
    S.emit(NdOp::SUBBYTES, InputLane, {Value, NdVar::cst(Offset, 4)});
    S.emit(NdOp::SUBBYTES, CountLane, {Counts, NdVar::cst(Offset, 4)});
    NdVar InRange = S.makeTemp(1);
    S.emit(NdOp::INT_LESS, InRange,
           {CountLane, NdVar::cst(LaneBits, Info.Spec.ElementSize)});
    NdVar EffectiveCount = CountLane;
    if (Arithmetic) {
      EffectiveCount = S.makeTemp(Info.Spec.ElementSize);
      S.emit(NdOp::SELECT, EffectiveCount,
             {InRange, CountLane,
              NdVar::cst(LaneBits - 1, Info.Spec.ElementSize)});
    }
    NdVar Shifted = S.makeTemp(Info.Spec.ElementSize);
    S.emit(Info.Spec.Operation, Shifted, {InputLane, EffectiveCount});
    NdVar ResultLane = Shifted;
    if (!Arithmetic) {
      ResultLane = S.makeTemp(Info.Spec.ElementSize);
      S.emit(NdOp::SELECT, ResultLane,
             {InRange, Shifted, NdVar::cst(0, Info.Spec.ElementSize)});
    }
    if (Lane == 0) {
      Raw = ResultLane;
    } else {
      NdVar Next = S.makeTemp(Raw.Size + Info.Spec.ElementSize);
      S.emit(NdOp::CONCAT, Next, {ResultLane, Raw});
      Raw = Next;
    }
  }

  if (Info.MaskOperand)
    return emitMaskedVectorResult(L, S, X86.operands[0], *Info.MaskOperand,
                                  Raw, Info.Spec.ElementSize);
  S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {Raw});
  return true;
}

struct EvexUniformShiftSpec {
  uint8_t ImmediateOpcode;
  uint8_t ImmediateExtension;
  uint8_t VariableOpcode;
  uint16_t ElementSize;
  bool W;
  bool BroadcastAllowed;
  NdOp Operation;
};

struct EvexUniformShiftInfo {
  CanonicalEvexEncodingInfo Encoding;
  EvexUniformShiftSpec Spec{};
  const cs_x86_op *MaskOperand = nullptr;
  unsigned ValueIndex = 0;
  unsigned CountIndex = 0;
  uint16_t VectorSize = 0;
  uint16_t MaskSize = 0;
  unsigned LaneCount = 0;
  bool Immediate = false;
  bool MemoryForm = false;
  bool Broadcast = false;
  bool ZeroMask = false;
};

bool getEvexUniformShiftSpec(unsigned InsnId, EvexUniformShiftSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPSLLW:
    Spec = {0x71, 6, 0xf1, 2, false, false, NdOp::INT_LEFT};
    return true;
  case X86_INS_VPSLLD:
    Spec = {0x72, 6, 0xf2, 4, false, true, NdOp::INT_LEFT};
    return true;
  case X86_INS_VPSLLQ:
    Spec = {0x73, 6, 0xf3, 8, true, true, NdOp::INT_LEFT};
    return true;
  case X86_INS_VPSRLW:
    Spec = {0x71, 2, 0xd1, 2, false, false, NdOp::INT_RIGHT};
    return true;
  case X86_INS_VPSRLD:
    Spec = {0x72, 2, 0xd2, 4, false, true, NdOp::INT_RIGHT};
    return true;
  case X86_INS_VPSRLQ:
    Spec = {0x73, 2, 0xd3, 8, true, true, NdOp::INT_RIGHT};
    return true;
  case X86_INS_VPSRAW:
    Spec = {0x71, 4, 0xe1, 2, false, false, NdOp::INT_ASHR};
    return true;
  case X86_INS_VPSRAD:
    Spec = {0x72, 4, 0xe2, 4, false, true, NdOp::INT_ASHR};
    return true;
  case X86_INS_VPSRAQ:
    Spec = {0x72, 4, 0xe2, 8, true, true, NdOp::INT_ASHR};
    return true;
  default:
    return false;
  }
}

bool validateEvexUniformShift(X86Lifter &L, const cs_insn *Insn,
                              const cs_x86 &X86,
                              EvexUniformShiftInfo &Info) {
  if (!Insn || !getEvexUniformShiftSpec(Insn->id, Info.Spec) ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                      Info.Encoding) ||
      (Info.Encoding.P0 & 0x07) != 0x01 ||
      (Info.Encoding.P1 & 0x87) !=
          static_cast<uint8_t>((Info.Spec.W ? 0x80 : 0) | 0x05) ||
      X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool HasMask =
      X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
  if (X86.op_count != (HasMask ? 4 : 3))
    return false;
  Info.MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  Info.ValueIndex = HasMask ? 2 : 1;
  Info.CountIndex = Info.ValueIndex + 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &Value = X86.operands[Info.ValueIndex];
  const cs_x86_op &Count = X86.operands[Info.CountIndex];
  Info.Immediate = Count.type == X86_OP_IMM;
  const cs_x86_op &MemoryOrRegister = Info.Immediate ? Value : Count;
  const uint8_t ExpectedOpcode =
      Info.Immediate ? Info.Spec.ImmediateOpcode : Info.Spec.VariableOpcode;
  if (Info.Encoding.Opcode != ExpectedOpcode)
    return false;

  Info.VectorSize = static_cast<uint16_t>(Destination.size);
  if (Info.VectorSize != 16 && Info.VectorSize != 32 &&
      Info.VectorSize != 64)
    return false;
  Info.LaneCount = Info.VectorSize / Info.Spec.ElementSize;
  Info.MaskSize =
      static_cast<uint16_t>(std::max(1u, (Info.LaneCount + 7u) / 8u));
  Info.MemoryForm = MemoryOrRegister.type == X86_OP_MEM;
  Info.Broadcast = Info.Immediate && Info.MemoryForm &&
                   (Info.Encoding.P2 & 0x10) != 0;
  Info.ZeroMask = (Info.Encoding.P2 & 0x80) != 0;

  const uint8_t EncodedLength = Info.Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Info.VectorSize == 16 ? 0 : (Info.VectorSize == 32 ? 0x20 : 0x40);
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      (((Info.Encoding.ModRM & 0xc0) != 0xc0) != Info.MemoryForm) ||
      (!Info.Immediate && (Info.Encoding.P2 & 0x10) != 0) ||
      (!Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0) ||
      (Info.Broadcast && !Info.Spec.BroadcastAllowed) ||
      !isX86VectorRegisterOfSize(Destination, Info.VectorSize))
    return false;

  const unsigned DestinationIndex = x86VectorRegisterIndex(Destination);
  if (Info.Immediate) {
    if ((Info.Encoding.P0 & 0x90) != 0x90 ||
        ((Info.Encoding.ModRM >> 3) & 7) != Info.Spec.ImmediateExtension ||
        decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
            DestinationIndex ||
        Count.size != 1 || X86.encoding.imm_size != 1 ||
        X86.encoding.imm_offset != Insn->size - 1 ||
        Insn->bytes[Insn->size - 1] != static_cast<uint8_t>(Count.imm))
      return false;
  } else {
    if (!isX86VectorRegisterOfSize(Value, Info.VectorSize) ||
        decodeEvexVectorRegIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
            DestinationIndex ||
        decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
            x86VectorRegisterIndex(Value) ||
        X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0)
      return false;
  }
  if (L.targetArch() == Arch::X86 &&
      (DestinationIndex >= 8 ||
       (!Info.Immediate && x86VectorRegisterIndex(Value) >= 8)))
    return false;

  const uint8_t EncodedMask = Info.Encoding.P2 & 7;
  if (Info.MaskOperand) {
    if (Info.MaskOperand->reg == X86_REG_K0 ||
        Info.MaskOperand->size != Info.MaskSize ||
        EncodedMask !=
            static_cast<uint8_t>(Info.MaskOperand->reg - X86_REG_K0) ||
        Info.ZeroMask !=
            static_cast<bool>(Info.MaskOperand->avx_zero_opmask))
      return false;
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < Info.MaskSize)
      return false;
  } else if (EncodedMask != 0 || Info.ZeroMask) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      Info.Broadcast ? evexBroadcastForLaneCount(Info.LaneCount)
                     : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    const bool IsMask = Info.MaskOperand && &Operand == Info.MaskOperand;
    const bool IsMemoryOrRegister = &Operand == &MemoryOrRegister;
    if (Operand.avx_zero_opmask != (IsMask && Info.ZeroMask) ||
        Operand.avx_bcast !=
            (IsMemoryOrRegister ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }

  const size_t TrailingBytes = Info.Immediate ? 1 : 0;
  if (Info.MemoryForm) {
    const uint16_t TupleSize =
        Info.Immediate
            ? (Info.Broadcast ? Info.Spec.ElementSize : Info.VectorSize)
            : 16;
    return MemoryOrRegister.size == TupleSize &&
           validateCanonicalEvexMemoryTail(Insn, X86, Info.Encoding,
                                           MemoryOrRegister, TupleSize,
                                           TrailingBytes);
  }
  const uint16_t RegisterSize = Info.Immediate ? Info.VectorSize : 16;
  if (!isX86VectorRegisterOfSize(MemoryOrRegister, RegisterSize) ||
      decodeEvexVectorRMIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(MemoryOrRegister) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Info.Encoding,
                                         TrailingBytes))
    return false;
  return L.targetArch() != Arch::X86 ||
         x86VectorRegisterIndex(MemoryOrRegister) < 8;
}

bool liftEvexUniformShift(X86Lifter &L, X86Lifter::LiftState &S,
                          const cs_insn *Insn, const cs_x86 &X86) {
  EvexUniformShiftInfo Info;
  if (!validateEvexUniformShift(L, Insn, X86, Info))
    return false;

  NdVar ActiveMask;
  if (Info.MaskOperand) {
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    ActiveMask = NdVar::reg(MaskInfo.Offset, Info.MaskSize);
  } else {
    const uint64_t AllActive =
        (UINT64_C(1) << Info.LaneCount) - UINT64_C(1);
    ActiveMask = NdVar::cst(AllActive, Info.MaskSize);
  }

  const cs_x86_op &ValueOperand = X86.operands[Info.ValueIndex];
  const cs_x86_op &CountOperand = X86.operands[Info.CountIndex];
  NdVar Value;
  NdVar RawCount;
  if (Info.Immediate) {
    Value = Info.MemoryForm
                ? emitEvexMaskedMemoryLoad(
                      S, ValueOperand, ActiveMask, Info.VectorSize,
                      Info.Spec.ElementSize,
                      Info.Broadcast ? Info.Spec.ElementSize : Info.VectorSize,
                      Info.Broadcast)
                : L.operandRead(S, ValueOperand);
    RawCount = NdVar::cst(static_cast<uint8_t>(CountOperand.imm), 8);
  } else {
    Value = L.operandRead(S, ValueOperand);
    // The shared xmm/m128 count is consumed before destination writemasking;
    // unlike the immediate form's vector source, it is always read in full.
    const NdVar CountVector = L.operandRead(S, CountOperand);
    if (CountVector.Size != 16)
      return false;
    RawCount = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, RawCount,
           {CountVector, NdVar::cst(0, 4)});
  }
  if (Value.Size != Info.VectorSize || RawCount.Size != 8)
    return false;

  const unsigned LaneBits = Info.Spec.ElementSize * 8;
  const bool Arithmetic = Info.Spec.Operation == NdOp::INT_ASHR;
  NdVar InRange = S.makeTemp(1);
  S.emit(NdOp::INT_LESS, InRange, {RawCount, NdVar::cst(LaneBits, 8)});
  NdVar LaneCountValue = S.makeTemp(Info.Spec.ElementSize);
  S.emit(NdOp::SUBBYTES, LaneCountValue,
         {RawCount, NdVar::cst(0, 4)});
  if (Arithmetic) {
    NdVar Clamped = S.makeTemp(Info.Spec.ElementSize);
    S.emit(NdOp::SELECT, Clamped,
           {InRange, LaneCountValue,
            NdVar::cst(LaneBits - 1, Info.Spec.ElementSize)});
    LaneCountValue = Clamped;
  }

  NdVar Raw = S.makeTemp(0);
  for (unsigned Lane = 0; Lane < Info.LaneCount; ++Lane) {
    NdVar InputLane = S.makeTemp(Info.Spec.ElementSize);
    S.emit(NdOp::SUBBYTES, InputLane,
           {Value, NdVar::cst(Lane * Info.Spec.ElementSize, 4)});
    NdVar Shifted = S.makeTemp(Info.Spec.ElementSize);
    S.emit(Info.Spec.Operation, Shifted, {InputLane, LaneCountValue});
    NdVar ResultLane = Shifted;
    if (!Arithmetic) {
      ResultLane = S.makeTemp(Info.Spec.ElementSize);
      S.emit(NdOp::SELECT, ResultLane,
             {InRange, Shifted, NdVar::cst(0, Info.Spec.ElementSize)});
    }
    if (Lane == 0) {
      Raw = ResultLane;
    } else {
      NdVar Next = S.makeTemp(Raw.Size + Info.Spec.ElementSize);
      S.emit(NdOp::CONCAT, Next, {ResultLane, Raw});
      Raw = Next;
    }
  }

  if (Info.MaskOperand)
    return emitMaskedVectorResult(L, S, X86.operands[0], *Info.MaskOperand,
                                  Raw, Info.Spec.ElementSize);
  S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {Raw});
  return true;
}

enum class EvexPackedMultiplyOperation {
  Low,
  SignedHigh,
  UnsignedHigh,
  SignedWidening,
  UnsignedWidening,
};

struct EvexPackedMultiplySpec {
  EvexPackedMultiplyOperation Operation;
  uint8_t Map;
  uint8_t Opcode;
  uint16_t OutputElementSize;
  uint16_t InputElementSize;
  bool W;
  bool BroadcastAllowed;
};

struct EvexPackedMultiplyInfo {
  CanonicalEvexEncodingInfo Encoding;
  EvexPackedMultiplySpec Spec{};
  const cs_x86_op *MaskOperand = nullptr;
  unsigned LeftIndex = 0;
  unsigned RightIndex = 0;
  uint16_t VectorSize = 0;
  uint16_t MaskSize = 0;
  unsigned LaneCount = 0;
  bool MemoryForm = false;
  bool Broadcast = false;
  bool ZeroMask = false;
};

bool getEvexPackedMultiplySpec(unsigned InsnId,
                               EvexPackedMultiplySpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPMULLW:
    Spec = {EvexPackedMultiplyOperation::Low, 1, 0xd5, 2, 2, false, false};
    return true;
  case X86_INS_VPMULLD:
    Spec = {EvexPackedMultiplyOperation::Low, 2, 0x40, 4, 4, false, true};
    return true;
  case X86_INS_VPMULLQ:
    Spec = {EvexPackedMultiplyOperation::Low, 2, 0x40, 8, 8, true, true};
    return true;
  case X86_INS_VPMULHW:
    Spec = {EvexPackedMultiplyOperation::SignedHigh, 1, 0xe5, 2, 2, false,
            false};
    return true;
  case X86_INS_VPMULHUW:
    Spec = {EvexPackedMultiplyOperation::UnsignedHigh, 1, 0xe4, 2, 2,
            false, false};
    return true;
  case X86_INS_VPMULDQ:
    Spec = {EvexPackedMultiplyOperation::SignedWidening, 2, 0x28, 8, 4,
            true, true};
    return true;
  case X86_INS_VPMULUDQ:
    Spec = {EvexPackedMultiplyOperation::UnsignedWidening, 1, 0xf4, 8, 4,
            true, true};
    return true;
  default:
    return false;
  }
}

bool validateEvexPackedMultiply(X86Lifter &L, const cs_insn *Insn,
                                const cs_x86 &X86,
                                EvexPackedMultiplyInfo &Info) {
  if (!Insn || !getEvexPackedMultiplySpec(Insn->id, Info.Spec) ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                      Info.Encoding) ||
      (Info.Encoding.P0 & 0x07) != Info.Spec.Map ||
      (Info.Encoding.P1 & 0x87) !=
          static_cast<uint8_t>((Info.Spec.W ? 0x80 : 0) | 0x05) ||
      Info.Encoding.Opcode != Info.Spec.Opcode ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool HasMask =
      X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
  if (X86.op_count != (HasMask ? 4 : 3))
    return false;
  Info.MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  Info.LeftIndex = HasMask ? 2 : 1;
  Info.RightIndex = Info.LeftIndex + 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &Left = X86.operands[Info.LeftIndex];
  const cs_x86_op &Right = X86.operands[Info.RightIndex];
  Info.VectorSize = static_cast<uint16_t>(Destination.size);
  if (Info.VectorSize != 16 && Info.VectorSize != 32 &&
      Info.VectorSize != 64)
    return false;
  Info.LaneCount = Info.VectorSize / Info.Spec.OutputElementSize;
  Info.MaskSize =
      static_cast<uint16_t>(std::max(1u, (Info.LaneCount + 7u) / 8u));
  Info.MemoryForm = Right.type == X86_OP_MEM;
  Info.Broadcast = Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0;
  Info.ZeroMask = (Info.Encoding.P2 & 0x80) != 0;

  const uint8_t EncodedLength = Info.Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Info.VectorSize == 16 ? 0 : (Info.VectorSize == 32 ? 0x20 : 0x40);
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      (((Info.Encoding.ModRM & 0xc0) != 0xc0) != Info.MemoryForm) ||
      (!Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0) ||
      (Info.Broadcast && !Info.Spec.BroadcastAllowed) ||
      !isX86VectorRegisterOfSize(Destination, Info.VectorSize) ||
      !isX86VectorRegisterOfSize(Left, Info.VectorSize) ||
      decodeEvexVectorRegIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
          x86VectorRegisterIndex(Left))
    return false;
  if (L.targetArch() == Arch::X86 &&
      (x86VectorRegisterIndex(Destination) >= 8 ||
       x86VectorRegisterIndex(Left) >= 8))
    return false;

  const uint8_t EncodedMask = Info.Encoding.P2 & 7;
  if (Info.MaskOperand) {
    if (Info.MaskOperand->reg == X86_REG_K0 ||
        Info.MaskOperand->size != Info.MaskSize ||
        EncodedMask !=
            static_cast<uint8_t>(Info.MaskOperand->reg - X86_REG_K0) ||
        Info.ZeroMask !=
            static_cast<bool>(Info.MaskOperand->avx_zero_opmask))
      return false;
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < Info.MaskSize)
      return false;
  } else if (EncodedMask != 0 || Info.ZeroMask) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      Info.Broadcast ? evexBroadcastForLaneCount(Info.LaneCount)
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

  if (Info.MemoryForm) {
    const uint16_t TupleSize =
        Info.Broadcast ? Info.Spec.OutputElementSize : Info.VectorSize;
    return Right.size == TupleSize &&
           validateCanonicalEvexMemoryTail(Insn, X86, Info.Encoding, Right,
                                           TupleSize);
  }
  if (!isX86VectorRegisterOfSize(Right, Info.VectorSize) ||
      decodeEvexVectorRMIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Right) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Info.Encoding))
    return false;
  return L.targetArch() != Arch::X86 || x86VectorRegisterIndex(Right) < 8;
}

bool liftEvexPackedMultiply(X86Lifter &L, X86Lifter::LiftState &S,
                            const cs_insn *Insn, const cs_x86 &X86) {
  EvexPackedMultiplyInfo Info;
  if (!validateEvexPackedMultiply(L, Insn, X86, Info))
    return false;

  NdVar ActiveMask;
  if (Info.MaskOperand) {
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    ActiveMask = NdVar::reg(MaskInfo.Offset, Info.MaskSize);
  } else {
    const uint64_t AllActive =
        (UINT64_C(1) << Info.LaneCount) - UINT64_C(1);
    ActiveMask = NdVar::cst(AllActive, Info.MaskSize);
  }
  const NdVar Left = L.operandRead(S, X86.operands[Info.LeftIndex]);
  const cs_x86_op &RightOperand = X86.operands[Info.RightIndex];
  const NdVar Right = Info.MemoryForm
                          ? emitEvexMaskedMemoryLoad(
                                S, RightOperand, ActiveMask, Info.VectorSize,
                                Info.Spec.OutputElementSize,
                                Info.Broadcast ? Info.Spec.OutputElementSize
                                               : Info.VectorSize,
                                Info.Broadcast)
                          : L.operandRead(S, RightOperand);
  if (Left.Size != Info.VectorSize || Right.Size != Info.VectorSize)
    return false;

  NdVar Raw = S.makeTemp(0);
  for (unsigned Lane = 0; Lane < Info.LaneCount; ++Lane) {
    const uint64_t Offset =
        static_cast<uint64_t>(Lane) * Info.Spec.OutputElementSize;
    NdVar LeftLane = S.makeTemp(Info.Spec.InputElementSize);
    NdVar RightLane = S.makeTemp(Info.Spec.InputElementSize);
    S.emit(NdOp::SUBBYTES, LeftLane, {Left, NdVar::cst(Offset, 4)});
    S.emit(NdOp::SUBBYTES, RightLane, {Right, NdVar::cst(Offset, 4)});
    NdVar ResultLane;
    switch (Info.Spec.Operation) {
    case EvexPackedMultiplyOperation::Low:
      ResultLane = S.makeTemp(Info.Spec.OutputElementSize);
      S.emit(NdOp::INT_MULT, ResultLane, {LeftLane, RightLane});
      break;
    case EvexPackedMultiplyOperation::SignedHigh:
    case EvexPackedMultiplyOperation::UnsignedHigh: {
      const bool Signed =
          Info.Spec.Operation == EvexPackedMultiplyOperation::SignedHigh;
      const uint16_t WideSize = Info.Spec.OutputElementSize * 2;
      NdVar WideLeft = S.makeTemp(WideSize);
      NdVar WideRight = S.makeTemp(WideSize);
      S.emit(Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WideLeft, {LeftLane});
      S.emit(Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WideRight,
             {RightLane});
      NdVar Product = S.makeTemp(WideSize);
      S.emit(NdOp::INT_MULT, Product, {WideLeft, WideRight});
      ResultLane = S.makeTemp(Info.Spec.OutputElementSize);
      S.emit(NdOp::SUBBYTES, ResultLane,
             {Product, NdVar::cst(Info.Spec.OutputElementSize, 4)});
      break;
    }
    case EvexPackedMultiplyOperation::SignedWidening:
    case EvexPackedMultiplyOperation::UnsignedWidening: {
      const bool Signed = Info.Spec.Operation ==
                          EvexPackedMultiplyOperation::SignedWidening;
      NdVar WideLeft = S.makeTemp(Info.Spec.OutputElementSize);
      NdVar WideRight = S.makeTemp(Info.Spec.OutputElementSize);
      S.emit(Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WideLeft, {LeftLane});
      S.emit(Signed ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WideRight,
             {RightLane});
      ResultLane = S.makeTemp(Info.Spec.OutputElementSize);
      S.emit(NdOp::INT_MULT, ResultLane, {WideLeft, WideRight});
      break;
    }
    }
    if (Lane == 0) {
      Raw = ResultLane;
    } else {
      NdVar Next = S.makeTemp(Raw.Size + Info.Spec.OutputElementSize);
      S.emit(NdOp::CONCAT, Next, {ResultLane, Raw});
      Raw = Next;
    }
  }

  if (Info.MaskOperand)
    return emitMaskedVectorResult(L, S, X86.operands[0], *Info.MaskOperand,
                                  Raw, Info.Spec.OutputElementSize);
  S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {Raw});
  return true;
}

bool hasCanonicalTernaryLogicEncoding(
    const cs_insn *Insn, const cs_x86 &X86, Arch TargetArch,
    const cs_x86_op &SecondSource, const cs_x86_op &Immediate,
    uint16_t VectorSize, uint16_t ElementSize, bool W,
    CanonicalEvexEncodingInfo &Encoding) {
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, TargetArch, Encoding))
    return false;

  const bool MemoryForm = (Encoding.ModRM & 0xc0) != 0xc0;
  const bool Broadcast = MemoryForm && (Encoding.P2 & 0x10) != 0;
  const uint8_t ExpectedLength =
      VectorSize == 16 ? 0 : (VectorSize == 32 ? 0x20 : 0x40);
  if ((Encoding.P0 & 0x07) != 0x03 ||
      ((Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((W ? 0x80 : 0) | 0x05) ||
      Encoding.Opcode != 0x25 ||
      (Encoding.P2 & 0x60) != ExpectedLength ||
      (Encoding.P2 & 0x60) == 0x60 ||
      (!MemoryForm && (Encoding.P2 & 0x10) != 0) || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID || Immediate.type != X86_OP_IMM ||
      Immediate.size != 1 || X86.encoding.imm_size != 1 ||
      X86.encoding.imm_offset != Insn->size - 1 ||
      Insn->bytes[Insn->size - 1] != static_cast<uint8_t>(Immediate.imm))
    return false;

  if (!MemoryForm)
    return validateCanonicalEvexRegisterTail(Insn, X86, Encoding, 1);
  const uint16_t TupleSize = Broadcast ? ElementSize : VectorSize;
  return SecondSource.type == X86_OP_MEM && SecondSource.size == TupleSize &&
         validateCanonicalEvexMemoryTail(Insn, X86, Encoding, SecondSource,
                                         TupleSize, 1);
}

bool hasCanonicalMaskConversionEncoding(
    const cs_insn *Insn, const cs_x86 &X86, Arch TargetArch,
    const cs_x86_op &VectorOperand, const cs_x86_op &MaskOperand,
    uint16_t VectorSize, uint16_t ElementSize, bool MaskToVector) {
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, TargetArch, Encoding) ||
      (Encoding.P0 & 0x07) != 0x02 || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0 || hasUnsupportedEvexValueModifier(X86))
    return false;

  const uint8_t ExpectedWidthByte =
      ElementSize == 2 || ElementSize == 8 ? 0xfe : 0x7e;
  if ((Encoding.P1 | 0x04) != ExpectedWidthByte)
    return false;

  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  if ((Encoding.P2 & 0x9f) != 0x08 || EncodedLength == 0x60)
    return false;
  const uint16_t EncodedVectorSize =
      EncodedLength == 0 ? 16 : (EncodedLength == 0x20 ? 32 : 64);
  if (EncodedVectorSize != VectorSize)
    return false;

  const bool SmallElement = ElementSize == 1 || ElementSize == 2;
  const uint8_t ExpectedOpcode = static_cast<uint8_t>(
      (SmallElement ? 0x28 : 0x38) + (MaskToVector ? 0 : 1));
  if (Encoding.Opcode != ExpectedOpcode ||
      (Encoding.ModRM & 0xc0) != 0xc0 ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Encoding))
    return false;

  if (MaskToVector)
    return (Encoding.P0 & 0x60) == 0x60 &&
           decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) ==
               x86VectorRegisterIndex(VectorOperand) &&
           (Encoding.ModRM & 7) == MaskOperand.reg - X86_REG_K0;
  return (Encoding.P0 & 0x90) == 0x90 &&
         ((Encoding.ModRM >> 3) & 7) ==
             MaskOperand.reg - X86_REG_K0 &&
         decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) ==
             x86VectorRegisterIndex(VectorOperand);
}

bool hasCanonicalMaskBroadcastEncoding(
    const cs_insn *Insn, const cs_x86 &X86, Arch TargetArch,
    const cs_x86_op &Destination, const cs_x86_op &Source,
    uint16_t VectorSize, bool WordSource) {
  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, TargetArch, Encoding) ||
      (Encoding.P0 & 0x07) != 0x02 || (Encoding.P0 & 0x60) != 0x60 ||
      (Encoding.P1 | 0x04) != (WordSource ? 0x7e : 0xfe) ||
      Encoding.Opcode != (WordSource ? 0x3a : 0x2a) ||
      (Encoding.ModRM & 0xc0) != 0xc0 || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0 || hasUnsupportedEvexValueModifier(X86) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Encoding))
    return false;

  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  if ((Encoding.P2 & 0x9f) != 0x08 || EncodedLength == 0x60)
    return false;
  const uint16_t EncodedVectorSize =
      EncodedLength == 0 ? 16 : (EncodedLength == 0x20 ? 32 : 64);
  return EncodedVectorSize == VectorSize &&
         decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) ==
             x86VectorRegisterIndex(Destination) &&
         (Encoding.ModRM & 7) == Source.reg - X86_REG_K0;
}

struct EvexDoubleShiftSpec {
  uint8_t Map;
  uint8_t Opcode;
  uint16_t ElementSize;
  bool W;
  bool Left;
  bool Variable;
  bool AllowBroadcast;
};

struct EvexDoubleShiftInfo {
  CanonicalEvexEncodingInfo Encoding;
  EvexDoubleShiftSpec Spec{};
  const cs_x86_op *MaskOperand = nullptr;
  unsigned FirstSourceIndex = 0;
  unsigned SecondSourceIndex = 0;
  unsigned ImmediateIndex = 0;
  uint16_t VectorSize = 0;
  uint16_t MaskSize = 0;
  unsigned LaneCount = 0;
  bool MemoryForm = false;
  bool Broadcast = false;
  bool ZeroMask = false;
};

bool getEvexDoubleShiftSpec(unsigned InsnId, EvexDoubleShiftSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPSHLDW:
    Spec = {3, 0x70, 2, true, true, false, false};
    return true;
  case X86_INS_VPSHLDD:
    Spec = {3, 0x71, 4, false, true, false, true};
    return true;
  case X86_INS_VPSHLDQ:
    Spec = {3, 0x71, 8, true, true, false, true};
    return true;
  case X86_INS_VPSHRDW:
    Spec = {3, 0x72, 2, true, false, false, false};
    return true;
  case X86_INS_VPSHRDD:
    Spec = {3, 0x73, 4, false, false, false, true};
    return true;
  case X86_INS_VPSHRDQ:
    Spec = {3, 0x73, 8, true, false, false, true};
    return true;
  case X86_INS_VPSHLDVW:
    Spec = {2, 0x70, 2, true, true, true, false};
    return true;
  case X86_INS_VPSHLDVD:
    Spec = {2, 0x71, 4, false, true, true, true};
    return true;
  case X86_INS_VPSHLDVQ:
    Spec = {2, 0x71, 8, true, true, true, true};
    return true;
  case X86_INS_VPSHRDVW:
    Spec = {2, 0x72, 2, true, false, true, false};
    return true;
  case X86_INS_VPSHRDVD:
    Spec = {2, 0x73, 4, false, false, true, true};
    return true;
  case X86_INS_VPSHRDVQ:
    Spec = {2, 0x73, 8, true, false, true, true};
    return true;
  default:
    return false;
  }
}

bool validateEvexDoubleShift(X86Lifter &L, const cs_insn *Insn,
                             const cs_x86 &X86,
                             EvexDoubleShiftInfo &Info) {
  if (!Insn || !getEvexDoubleShiftSpec(Insn->id, Info.Spec) ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                      Info.Encoding) ||
      (Info.Encoding.P0 & 0x0f) != Info.Spec.Map ||
      (Info.Encoding.P1 & 0x87) != (Info.Spec.W ? 0x85 : 0x05) ||
      Info.Encoding.Opcode != Info.Spec.Opcode || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool HasMask =
      X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
  const unsigned ExpectedOperands = Info.Spec.Variable
                                        ? (HasMask ? 4 : 3)
                                        : (HasMask ? 5 : 4);
  if (X86.op_count != ExpectedOperands)
    return false;
  Info.MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  Info.FirstSourceIndex = HasMask ? 2 : 1;
  Info.SecondSourceIndex = Info.FirstSourceIndex + 1;
  Info.ImmediateIndex = Info.Spec.Variable ? 0 : Info.SecondSourceIndex + 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &FirstSource = X86.operands[Info.FirstSourceIndex];
  const cs_x86_op &SecondSource = X86.operands[Info.SecondSourceIndex];
  Info.VectorSize = static_cast<uint16_t>(Destination.size);
  if (Info.VectorSize != 16 && Info.VectorSize != 32 &&
      Info.VectorSize != 64)
    return false;
  Info.LaneCount = Info.VectorSize / Info.Spec.ElementSize;
  Info.MaskSize =
      static_cast<uint16_t>(std::max(1u, (Info.LaneCount + 7u) / 8u));
  Info.MemoryForm = SecondSource.type == X86_OP_MEM;
  Info.Broadcast = Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0;
  Info.ZeroMask = (Info.Encoding.P2 & 0x80) != 0;

  if (Info.Spec.Variable) {
    if (X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0)
      return false;
  } else {
    const cs_x86_op &Immediate = X86.operands[Info.ImmediateIndex];
    if (Immediate.type != X86_OP_IMM || Immediate.size != 1 ||
        X86.encoding.imm_size != 1 ||
        X86.encoding.imm_offset + 1 != Insn->size ||
        static_cast<uint8_t>(Immediate.imm) !=
            Insn->bytes[X86.encoding.imm_offset])
      return false;
  }

  const uint8_t EncodedLength = Info.Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Info.VectorSize == 16 ? 0 : (Info.VectorSize == 32 ? 0x20 : 0x40);
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      (((Info.Encoding.ModRM & 0xc0) != 0xc0) != Info.MemoryForm) ||
      (!Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0) ||
      (Info.Broadcast && !Info.Spec.AllowBroadcast) ||
      !isX86VectorRegisterOfSize(Destination, Info.VectorSize) ||
      !isX86VectorRegisterOfSize(FirstSource, Info.VectorSize) ||
      decodeEvexVectorRegIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
          x86VectorRegisterIndex(FirstSource))
    return false;
  if (L.targetArch() == Arch::X86 &&
      (x86VectorRegisterIndex(Destination) >= 8 ||
       x86VectorRegisterIndex(FirstSource) >= 8))
    return false;

  const uint8_t EncodedMask = Info.Encoding.P2 & 7;
  if (Info.MaskOperand) {
    if (Info.MaskOperand->reg == X86_REG_K0 ||
        Info.MaskOperand->size != Info.MaskSize ||
        EncodedMask !=
            static_cast<uint8_t>(Info.MaskOperand->reg - X86_REG_K0) ||
        Info.ZeroMask !=
            static_cast<bool>(Info.MaskOperand->avx_zero_opmask))
      return false;
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < Info.MaskSize)
      return false;
  } else if (EncodedMask != 0 || Info.ZeroMask) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      Info.Broadcast ? evexBroadcastForLaneCount(Info.LaneCount)
                     : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    const bool IsMask = Info.MaskOperand && &Operand == Info.MaskOperand;
    const bool IsSecondSource = &Operand == &SecondSource;
    if (Operand.avx_zero_opmask != (IsMask && Info.ZeroMask) ||
        Operand.avx_bcast !=
            (IsSecondSource ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }

  const size_t TrailingBytes = Info.Spec.Variable ? 0 : 1;
  if (Info.MemoryForm) {
    const uint16_t TupleSize =
        Info.Broadcast ? Info.Spec.ElementSize : Info.VectorSize;
    return SecondSource.size == TupleSize &&
           validateCanonicalEvexMemoryTail(Insn, X86, Info.Encoding,
                                           SecondSource, TupleSize,
                                           TrailingBytes);
  }
  if (!isX86VectorRegisterOfSize(SecondSource, Info.VectorSize) ||
      decodeEvexVectorRMIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(SecondSource) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Info.Encoding,
                                         TrailingBytes))
    return false;
  return L.targetArch() != Arch::X86 ||
         x86VectorRegisterIndex(SecondSource) < 8;
}

enum class EvexVbmiKind : uint8_t {
  Permute,
  MultiShift,
};

struct EvexVbmiSpec {
  uint8_t Opcode;
  uint16_t ElementSize;
  bool W;
  bool AllowBroadcast;
  EvexVbmiKind Kind;
};

struct EvexVbmiInfo {
  CanonicalEvexEncodingInfo Encoding;
  EvexVbmiSpec Spec{};
  const cs_x86_op *MaskOperand = nullptr;
  unsigned FirstSourceIndex = 0;
  unsigned SecondSourceIndex = 0;
  uint16_t VectorSize = 0;
  uint16_t MaskSize = 0;
  unsigned LaneCount = 0;
  bool MemoryForm = false;
  bool Broadcast = false;
  bool ZeroMask = false;
};

bool getEvexVbmiSpec(unsigned InsnId, EvexVbmiSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPERMB:
    Spec = {0x8d, 1, false, false, EvexVbmiKind::Permute};
    return true;
  case X86_INS_VPERMW:
    Spec = {0x8d, 2, true, false, EvexVbmiKind::Permute};
    return true;
  case X86_INS_VPMULTISHIFTQB:
    Spec = {0x83, 1, true, true, EvexVbmiKind::MultiShift};
    return true;
  default:
    return false;
  }
}

bool validateEvexVbmi(X86Lifter &L, const cs_insn *Insn, const cs_x86 &X86,
                      EvexVbmiInfo &Info) {
  if (!Insn || !getEvexVbmiSpec(Insn->id, Info.Spec) ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                      Info.Encoding) ||
      (Info.Encoding.P0 & 0x0f) != 0x02 ||
      (Info.Encoding.P1 & 0x87) != (Info.Spec.W ? 0x85 : 0x05) ||
      Info.Encoding.Opcode != Info.Spec.Opcode ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool HasMask =
      X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
  if (X86.op_count != (HasMask ? 4 : 3))
    return false;
  Info.MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  Info.FirstSourceIndex = HasMask ? 2 : 1;
  Info.SecondSourceIndex = Info.FirstSourceIndex + 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &FirstSource = X86.operands[Info.FirstSourceIndex];
  const cs_x86_op &SecondSource = X86.operands[Info.SecondSourceIndex];
  Info.VectorSize = static_cast<uint16_t>(Destination.size);
  if (Info.VectorSize != 16 && Info.VectorSize != 32 &&
      Info.VectorSize != 64)
    return false;
  Info.LaneCount = Info.VectorSize / Info.Spec.ElementSize;
  Info.MaskSize =
      static_cast<uint16_t>(std::max(1u, (Info.LaneCount + 7u) / 8u));
  Info.MemoryForm = SecondSource.type == X86_OP_MEM;
  Info.Broadcast = Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0;
  Info.ZeroMask = (Info.Encoding.P2 & 0x80) != 0;

  const uint8_t EncodedLength = Info.Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Info.VectorSize == 16 ? 0 : (Info.VectorSize == 32 ? 0x20 : 0x40);
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      (((Info.Encoding.ModRM & 0xc0) != 0xc0) != Info.MemoryForm) ||
      (!Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0) ||
      (Info.Broadcast && !Info.Spec.AllowBroadcast) ||
      !isX86VectorRegisterOfSize(Destination, Info.VectorSize) ||
      !isX86VectorRegisterOfSize(FirstSource, Info.VectorSize) ||
      decodeEvexVectorRegIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
          x86VectorRegisterIndex(FirstSource))
    return false;
  if (L.targetArch() == Arch::X86 &&
      (x86VectorRegisterIndex(Destination) >= 8 ||
       x86VectorRegisterIndex(FirstSource) >= 8))
    return false;

  const uint8_t EncodedMask = Info.Encoding.P2 & 7;
  if (Info.MaskOperand) {
    if (Info.MaskOperand->reg == X86_REG_K0 ||
        Info.MaskOperand->size != Info.MaskSize ||
        EncodedMask !=
            static_cast<uint8_t>(Info.MaskOperand->reg - X86_REG_K0) ||
        Info.ZeroMask !=
            static_cast<bool>(Info.MaskOperand->avx_zero_opmask))
      return false;
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < Info.MaskSize)
      return false;
  } else if (EncodedMask != 0 || Info.ZeroMask) {
    return false;
  }

  const unsigned BroadcastLanes = Info.VectorSize / 8;
  const x86_avx_bcast ExpectedBroadcast =
      Info.Broadcast ? evexBroadcastForLaneCount(BroadcastLanes)
                     : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    const bool IsMask = Info.MaskOperand && &Operand == Info.MaskOperand;
    const bool IsSecondSource = &Operand == &SecondSource;
    if (Operand.avx_zero_opmask != (IsMask && Info.ZeroMask) ||
        Operand.avx_bcast !=
            (IsSecondSource ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }

  if (Info.MemoryForm) {
    const uint16_t TupleSize = Info.Broadcast ? 8 : Info.VectorSize;
    return SecondSource.size == TupleSize &&
           validateCanonicalEvexMemoryTail(Insn, X86, Info.Encoding,
                                           SecondSource, TupleSize);
  }
  if (!isX86VectorRegisterOfSize(SecondSource, Info.VectorSize) ||
      decodeEvexVectorRMIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(SecondSource) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Info.Encoding))
    return false;
  return L.targetArch() != Arch::X86 ||
         x86VectorRegisterIndex(SecondSource) < 8;
}

struct EvexBitShuffleInfo {
  CanonicalEvexEncodingInfo Encoding;
  const cs_x86_op *WriteMaskOperand = nullptr;
  unsigned DataIndex = 0;
  unsigned ControlIndex = 0;
  uint16_t VectorSize = 0;
  uint16_t MaskSize = 0;
  unsigned LaneCount = 0;
  bool MemoryForm = false;
};

bool validateEvexBitShuffle(X86Lifter &L, const cs_insn *Insn,
                            const cs_x86 &X86,
                            EvexBitShuffleInfo &Info) {
  if (!Insn || Insn->id != X86_INS_VPSHUFBITQMB ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                      Info.Encoding) ||
      (Info.Encoding.P0 & 0x0f) != 0x02 ||
      (Info.Encoding.P0 & 0x90) != 0x90 ||
      (Info.Encoding.P1 & 0x87) != 0x05 ||
      Info.Encoding.Opcode != 0x8f || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0 || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID || (Info.Encoding.P2 & 0x90) != 0)
    return false;

  const bool HasWriteMask =
      X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
  if (X86.op_count != (HasWriteMask ? 4 : 3))
    return false;
  Info.WriteMaskOperand = HasWriteMask ? &X86.operands[1] : nullptr;
  Info.DataIndex = HasWriteMask ? 2 : 1;
  Info.ControlIndex = Info.DataIndex + 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &Data = X86.operands[Info.DataIndex];
  const cs_x86_op &Controls = X86.operands[Info.ControlIndex];
  if (!isX86OpmaskOperand(Destination) ||
      !isX86VectorRegisterOperand(Data))
    return false;
  Info.VectorSize = static_cast<uint16_t>(Data.size);
  if (Info.VectorSize != 16 && Info.VectorSize != 32 &&
      Info.VectorSize != 64)
    return false;
  Info.LaneCount = Info.VectorSize;
  Info.MaskSize =
      static_cast<uint16_t>(std::max(1u, (Info.LaneCount + 7u) / 8u));
  Info.MemoryForm = Controls.type == X86_OP_MEM;
  if (Destination.size != Info.MaskSize ||
      (((Info.Encoding.ModRM & 0xc0) != 0xc0) != Info.MemoryForm) ||
      (!Info.MemoryForm &&
       !isX86VectorRegisterOfSize(Controls, Info.VectorSize)))
    return false;

  const uint8_t EncodedLength = Info.Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Info.VectorSize == 16 ? 0 : (Info.VectorSize == 32 ? 0x20 : 0x40);
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      ((Info.Encoding.ModRM >> 3) & 7) !=
          Destination.reg - X86_REG_K0 ||
      decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
          x86VectorRegisterIndex(Data))
    return false;
  if (L.targetArch() == Arch::X86 && x86VectorRegisterIndex(Data) >= 8)
    return false;

  const uint8_t EncodedMask = Info.Encoding.P2 & 7;
  if (Info.WriteMaskOperand) {
    if (Info.WriteMaskOperand->reg == X86_REG_K0 ||
        Info.WriteMaskOperand->size != Info.MaskSize ||
        EncodedMask != static_cast<uint8_t>(Info.WriteMaskOperand->reg -
                                           X86_REG_K0))
      return false;
    const RegInfo MaskInfo = mapCapstoneReg(
        static_cast<x86_reg>(Info.WriteMaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < Info.MaskSize)
      return false;
  } else if (EncodedMask != 0) {
    return false;
  }
  for (unsigned Index = 0; Index < X86.op_count; ++Index)
    if (X86.operands[Index].avx_zero_opmask ||
        X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID)
      return false;

  if (Info.MemoryForm)
    return Controls.size == Info.VectorSize &&
           validateCanonicalEvexMemoryTail(Insn, X86, Info.Encoding, Controls,
                                           Info.VectorSize);
  if (decodeEvexVectorRMIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Controls) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Info.Encoding))
    return false;
  return L.targetArch() != Arch::X86 ||
         x86VectorRegisterIndex(Controls) < 8;
}

struct EvexBlendSpec {
  uint8_t Opcode;
  uint16_t ElementSize;
  bool W;
  bool AllowBroadcast;
};

struct EvexBlendInfo {
  CanonicalEvexEncodingInfo Encoding;
  EvexBlendSpec Spec{};
  const cs_x86_op *SelectionMaskOperand = nullptr;
  unsigned FirstSourceIndex = 0;
  unsigned SecondSourceIndex = 0;
  uint16_t VectorSize = 0;
  uint16_t MaskSize = 0;
  unsigned LaneCount = 0;
  bool MemoryForm = false;
  bool Broadcast = false;
  bool ZeroMask = false;
};

bool getEvexBlendSpec(unsigned InsnId, EvexBlendSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPBLENDMB:
    Spec = {0x66, 1, false, false};
    return true;
  case X86_INS_VPBLENDMW:
    Spec = {0x66, 2, true, false};
    return true;
  case X86_INS_VPBLENDMD:
    Spec = {0x64, 4, false, true};
    return true;
  case X86_INS_VPBLENDMQ:
    Spec = {0x64, 8, true, true};
    return true;
  default:
    return false;
  }
}

bool validateEvexBlend(X86Lifter &L, const cs_insn *Insn, const cs_x86 &X86,
                       EvexBlendInfo &Info) {
  if (!Insn || !getEvexBlendSpec(Insn->id, Info.Spec) ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                      Info.Encoding) ||
      (Info.Encoding.P0 & 0x0f) != 0x02 ||
      (Info.Encoding.P1 & 0x87) != (Info.Spec.W ? 0x85 : 0x05) ||
      Info.Encoding.Opcode != Info.Spec.Opcode ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool HasMask =
      X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
  if (X86.op_count != (HasMask ? 4 : 3))
    return false;
  Info.SelectionMaskOperand = HasMask ? &X86.operands[1] : nullptr;
  Info.FirstSourceIndex = HasMask ? 2 : 1;
  Info.SecondSourceIndex = Info.FirstSourceIndex + 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &FirstSource = X86.operands[Info.FirstSourceIndex];
  const cs_x86_op &SecondSource = X86.operands[Info.SecondSourceIndex];
  Info.VectorSize = static_cast<uint16_t>(Destination.size);
  if (Info.VectorSize != 16 && Info.VectorSize != 32 &&
      Info.VectorSize != 64)
    return false;
  Info.LaneCount = Info.VectorSize / Info.Spec.ElementSize;
  Info.MaskSize =
      static_cast<uint16_t>(std::max(1u, (Info.LaneCount + 7u) / 8u));
  Info.MemoryForm = SecondSource.type == X86_OP_MEM;
  Info.Broadcast = Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0;
  Info.ZeroMask = (Info.Encoding.P2 & 0x80) != 0;

  const uint8_t EncodedLength = Info.Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Info.VectorSize == 16 ? 0 : (Info.VectorSize == 32 ? 0x20 : 0x40);
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      (((Info.Encoding.ModRM & 0xc0) != 0xc0) != Info.MemoryForm) ||
      (!Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0) ||
      (Info.Broadcast && !Info.Spec.AllowBroadcast) ||
      !isX86VectorRegisterOfSize(Destination, Info.VectorSize) ||
      !isX86VectorRegisterOfSize(FirstSource, Info.VectorSize) ||
      decodeEvexVectorRegIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
          x86VectorRegisterIndex(FirstSource))
    return false;
  if (L.targetArch() == Arch::X86 &&
      (x86VectorRegisterIndex(Destination) >= 8 ||
       x86VectorRegisterIndex(FirstSource) >= 8))
    return false;

  const uint8_t EncodedMask = Info.Encoding.P2 & 7;
  if (Info.SelectionMaskOperand) {
    if (Info.SelectionMaskOperand->reg == X86_REG_K0 ||
        Info.SelectionMaskOperand->size != Info.MaskSize ||
        EncodedMask != static_cast<uint8_t>(
                           Info.SelectionMaskOperand->reg - X86_REG_K0) ||
        Info.ZeroMask != static_cast<bool>(
                             Info.SelectionMaskOperand->avx_zero_opmask))
      return false;
    const RegInfo MaskInfo = mapCapstoneReg(
        static_cast<x86_reg>(Info.SelectionMaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < Info.MaskSize)
      return false;
  } else if (EncodedMask != 0 || Info.ZeroMask) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      Info.Broadcast ? evexBroadcastForLaneCount(Info.LaneCount)
                     : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    const bool IsMask = Info.SelectionMaskOperand &&
                        &Operand == Info.SelectionMaskOperand;
    const bool IsSecondSource = &Operand == &SecondSource;
    if (Operand.avx_zero_opmask != (IsMask && Info.ZeroMask) ||
        Operand.avx_bcast !=
            (IsSecondSource ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }

  if (Info.MemoryForm) {
    const uint16_t TupleSize =
        Info.Broadcast ? Info.Spec.ElementSize : Info.VectorSize;
    return SecondSource.size == TupleSize &&
           validateCanonicalEvexMemoryTail(Insn, X86, Info.Encoding,
                                           SecondSource, TupleSize);
  }
  if (!isX86VectorRegisterOfSize(SecondSource, Info.VectorSize) ||
      decodeEvexVectorRMIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(SecondSource) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Info.Encoding))
    return false;
  return L.targetArch() != Arch::X86 ||
         x86VectorRegisterIndex(SecondSource) < 8;
}

struct EvexIfmaSpec {
  uint8_t Opcode;
  bool HighHalf;
};

struct EvexIfmaInfo {
  CanonicalEvexEncodingInfo Encoding;
  EvexIfmaSpec Spec{};
  const cs_x86_op *MaskOperand = nullptr;
  unsigned FirstSourceIndex = 0;
  unsigned SecondSourceIndex = 0;
  uint16_t VectorSize = 0;
  uint16_t MaskSize = 1;
  unsigned LaneCount = 0;
  bool MemoryForm = false;
  bool Broadcast = false;
  bool ZeroMask = false;
};

bool getEvexIfmaSpec(unsigned InsnId, EvexIfmaSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPMADD52LUQ:
    Spec = {0xb4, false};
    return true;
  case X86_INS_VPMADD52HUQ:
    Spec = {0xb5, true};
    return true;
  default:
    return false;
  }
}

bool validateEvexIfma(X86Lifter &L, const cs_insn *Insn, const cs_x86 &X86,
                      EvexIfmaInfo &Info) {
  if (!Insn || !getEvexIfmaSpec(Insn->id, Info.Spec) ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                      Info.Encoding) ||
      (Info.Encoding.P0 & 0x0f) != 0x02 ||
      (Info.Encoding.P1 & 0x87) != 0x85 ||
      Info.Encoding.Opcode != Info.Spec.Opcode ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool HasMask =
      X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
  if (X86.op_count != (HasMask ? 4 : 3))
    return false;
  Info.MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  Info.FirstSourceIndex = HasMask ? 2 : 1;
  Info.SecondSourceIndex = Info.FirstSourceIndex + 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &FirstSource = X86.operands[Info.FirstSourceIndex];
  const cs_x86_op &SecondSource = X86.operands[Info.SecondSourceIndex];
  Info.VectorSize = static_cast<uint16_t>(Destination.size);
  if (Info.VectorSize != 16 && Info.VectorSize != 32 &&
      Info.VectorSize != 64)
    return false;
  Info.LaneCount = Info.VectorSize / 8;
  Info.MemoryForm = SecondSource.type == X86_OP_MEM;
  Info.Broadcast = Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0;
  Info.ZeroMask = (Info.Encoding.P2 & 0x80) != 0;

  const uint8_t EncodedLength = Info.Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Info.VectorSize == 16 ? 0 : (Info.VectorSize == 32 ? 0x20 : 0x40);
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      (((Info.Encoding.ModRM & 0xc0) != 0xc0) != Info.MemoryForm) ||
      (!Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0) ||
      !isX86VectorRegisterOfSize(Destination, Info.VectorSize) ||
      !isX86VectorRegisterOfSize(FirstSource, Info.VectorSize) ||
      decodeEvexVectorRegIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
          x86VectorRegisterIndex(FirstSource))
    return false;
  if (L.targetArch() == Arch::X86 &&
      (x86VectorRegisterIndex(Destination) >= 8 ||
       x86VectorRegisterIndex(FirstSource) >= 8))
    return false;

  const uint8_t EncodedMask = Info.Encoding.P2 & 7;
  if (Info.MaskOperand) {
    if (Info.MaskOperand->reg == X86_REG_K0 ||
        Info.MaskOperand->size != Info.MaskSize ||
        EncodedMask !=
            static_cast<uint8_t>(Info.MaskOperand->reg - X86_REG_K0) ||
        Info.ZeroMask !=
            static_cast<bool>(Info.MaskOperand->avx_zero_opmask))
      return false;
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < Info.MaskSize)
      return false;
  } else if (EncodedMask != 0 || Info.ZeroMask) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      Info.Broadcast ? evexBroadcastForLaneCount(Info.LaneCount)
                     : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    const bool IsMask = Info.MaskOperand && &Operand == Info.MaskOperand;
    const bool IsSecondSource = &Operand == &SecondSource;
    if (Operand.avx_zero_opmask != (IsMask && Info.ZeroMask) ||
        Operand.avx_bcast !=
            (IsSecondSource ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }

  if (Info.MemoryForm) {
    const uint16_t TupleSize = Info.Broadcast ? 8 : Info.VectorSize;
    return SecondSource.size == TupleSize &&
           validateCanonicalEvexMemoryTail(Insn, X86, Info.Encoding,
                                           SecondSource, TupleSize);
  }
  if (!isX86VectorRegisterOfSize(SecondSource, Info.VectorSize) ||
      decodeEvexVectorRMIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(SecondSource) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Info.Encoding))
    return false;
  return L.targetArch() != Arch::X86 ||
         x86VectorRegisterIndex(SecondSource) < 8;
}

struct EvexVnniSpec {
  uint8_t Opcode;
  bool WordElements;
  bool Saturating;
};

struct EvexVnniInfo {
  CanonicalEvexEncodingInfo Encoding;
  EvexVnniSpec Spec{};
  const cs_x86_op *MaskOperand = nullptr;
  unsigned FirstSourceIndex = 0;
  unsigned SecondSourceIndex = 0;
  uint16_t VectorSize = 0;
  uint16_t MaskSize = 0;
  unsigned LaneCount = 0;
  bool MemoryForm = false;
  bool Broadcast = false;
  bool ZeroMask = false;
};

bool getEvexVnniSpec(unsigned InsnId, EvexVnniSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPDPBUSD:
    Spec = {0x50, false, false};
    return true;
  case X86_INS_VPDPBUSDS:
    Spec = {0x51, false, true};
    return true;
  case X86_INS_VPDPWSSD:
    Spec = {0x52, true, false};
    return true;
  case X86_INS_VPDPWSSDS:
    Spec = {0x53, true, true};
    return true;
  default:
    return false;
  }
}

bool validateEvexVnni(X86Lifter &L, const cs_insn *Insn, const cs_x86 &X86,
                      EvexVnniInfo &Info) {
  if (!Insn || !getEvexVnniSpec(Insn->id, Info.Spec) ||
      !parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(),
                                      Info.Encoding) ||
      (Info.Encoding.P0 & 0x07) != 0x02 ||
      ((Info.Encoding.P1 | 0x04) & 0x87) != 0x05 ||
      Info.Encoding.Opcode != Info.Spec.Opcode ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      X86.avx_sae || X86.avx_rm != X86_AVX_RM_INVALID)
    return false;

  const bool HasMask =
      X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
  if (X86.op_count != (HasMask ? 4 : 3))
    return false;
  Info.MaskOperand = HasMask ? &X86.operands[1] : nullptr;
  Info.FirstSourceIndex = HasMask ? 2 : 1;
  Info.SecondSourceIndex = Info.FirstSourceIndex + 1;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &FirstSource = X86.operands[Info.FirstSourceIndex];
  const cs_x86_op &SecondSource = X86.operands[Info.SecondSourceIndex];
  Info.VectorSize = static_cast<uint16_t>(Destination.size);
  if (Info.VectorSize != 16 && Info.VectorSize != 32 &&
      Info.VectorSize != 64)
    return false;
  Info.LaneCount = Info.VectorSize / 4;
  Info.MaskSize =
      static_cast<uint16_t>(std::max(1u, (Info.LaneCount + 7u) / 8u));
  Info.MemoryForm = SecondSource.type == X86_OP_MEM;
  Info.Broadcast = Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0;
  Info.ZeroMask = (Info.Encoding.P2 & 0x80) != 0;

  const uint8_t EncodedLength = Info.Encoding.P2 & 0x60;
  const uint8_t ExpectedLength =
      Info.VectorSize == 16 ? 0 : (Info.VectorSize == 32 ? 0x20 : 0x40);
  if (EncodedLength == 0x60 || EncodedLength != ExpectedLength ||
      (((Info.Encoding.ModRM & 0xc0) != 0xc0) != Info.MemoryForm) ||
      (!Info.MemoryForm && (Info.Encoding.P2 & 0x10) != 0) ||
      !isX86VectorRegisterOfSize(Destination, Info.VectorSize) ||
      !isX86VectorRegisterOfSize(FirstSource, Info.VectorSize) ||
      decodeEvexVectorRegIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(Destination) ||
      decodeEvexVectorVvvvIndex(Info.Encoding.P1, Info.Encoding.P2) !=
          x86VectorRegisterIndex(FirstSource))
    return false;
  if (L.targetArch() == Arch::X86 &&
      (x86VectorRegisterIndex(Destination) >= 8 ||
       x86VectorRegisterIndex(FirstSource) >= 8))
    return false;

  const uint8_t EncodedMask = Info.Encoding.P2 & 7;
  if (Info.MaskOperand) {
    if (Info.MaskOperand->reg == X86_REG_K0 ||
        Info.MaskOperand->size != Info.MaskSize ||
        EncodedMask !=
            static_cast<uint8_t>(Info.MaskOperand->reg - X86_REG_K0) ||
        Info.ZeroMask !=
            static_cast<bool>(Info.MaskOperand->avx_zero_opmask))
      return false;
    const RegInfo MaskInfo =
        mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
    if (MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < Info.MaskSize)
      return false;
  } else if (EncodedMask != 0 || Info.ZeroMask) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      Info.Broadcast ? evexBroadcastForLaneCount(Info.LaneCount)
                     : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    const bool IsMask = Info.MaskOperand && &Operand == Info.MaskOperand;
    const bool IsSecondSource = &Operand == &SecondSource;
    if (Operand.avx_zero_opmask != (IsMask && Info.ZeroMask) ||
        Operand.avx_bcast !=
            (IsSecondSource ? ExpectedBroadcast : X86_AVX_BCAST_INVALID))
      return false;
  }

  if (Info.MemoryForm) {
    const uint16_t TupleSize = Info.Broadcast ? 4 : Info.VectorSize;
    return SecondSource.size == TupleSize &&
           validateCanonicalEvexMemoryTail(Insn, X86, Info.Encoding,
                                           SecondSource, TupleSize);
  }
  if (!isX86VectorRegisterOfSize(SecondSource, Info.VectorSize) ||
      decodeEvexVectorRMIndex(Info.Encoding.P0, Info.Encoding.ModRM) !=
          x86VectorRegisterIndex(SecondSource) ||
      !validateCanonicalEvexRegisterTail(Insn, X86, Info.Encoding))
    return false;
  return L.targetArch() != Arch::X86 ||
         x86VectorRegisterIndex(SecondSource) < 8;
}

struct EvexMaskTestSpec {
  uint8_t Opcode;
  uint8_t MandatoryPrefix;
  uint16_t ElementSize;
  bool W;
  bool Negated;
};

bool getEvexMaskTestSpec(unsigned InsnId, EvexMaskTestSpec &Spec) {
  switch (InsnId) {
  case X86_INS_VPTESTMB:
    Spec = {0x26, 1, 1, false, false};
    return true;
  case X86_INS_VPTESTMW:
    Spec = {0x26, 1, 2, true, false};
    return true;
  case X86_INS_VPTESTMD:
    Spec = {0x27, 1, 4, false, false};
    return true;
  case X86_INS_VPTESTMQ:
    Spec = {0x27, 1, 8, true, false};
    return true;
  case X86_INS_VPTESTNMB:
    Spec = {0x26, 2, 1, false, true};
    return true;
  case X86_INS_VPTESTNMW:
    Spec = {0x26, 2, 2, true, true};
    return true;
  case X86_INS_VPTESTNMD:
    Spec = {0x27, 2, 4, false, true};
    return true;
  case X86_INS_VPTESTNMQ:
    Spec = {0x27, 2, 8, true, true};
    return true;
  default:
    return false;
  }
}

bool liftEvexMaskTest(X86Lifter &L, X86Lifter::LiftState &S,
                      const cs_insn *Insn, const cs_x86 &X86,
                      unsigned InsnId) {
  EvexMaskTestSpec Spec{};
  if (!getEvexMaskTestSpec(InsnId, Spec))
    return false;

  const bool HasWriteMask =
      X86.op_count >= 4 && isX86OpmaskOperand(X86.operands[1]);
  const unsigned FirstSourceIndex = HasWriteMask ? 2 : 1;
  const unsigned SecondSourceIndex = HasWriteMask ? 3 : 2;
  if (X86.op_count != (HasWriteMask ? 4u : 3u))
    return false;

  CanonicalEvexEncodingInfo Encoding;
  if (!parseCanonicalEvexEncodingInfo(Insn, X86, L.targetArch(), Encoding) ||
      (Encoding.P0 & 0x07) != 0x02 || (Encoding.P0 & 0x90) != 0x90 ||
      ((Encoding.P1 | 0x04) & 0x87) !=
          static_cast<uint8_t>((Spec.W ? 0x80 : 0) | 0x04 |
                               Spec.MandatoryPrefix) ||
      Encoding.Opcode != Spec.Opcode || X86.encoding.imm_offset != 0 ||
      X86.encoding.imm_size != 0 || X86.avx_sae ||
      X86.avx_rm != X86_AVX_RM_INVALID || (Encoding.P2 & 0x80) != 0)
    return false;

  const uint8_t EncodedLength = Encoding.P2 & 0x60;
  if (EncodedLength == 0x60)
    return false;
  const uint16_t VectorSize = EncodedLength == 0      ? 16
                              : EncodedLength == 0x20 ? 32
                                                      : 64;
  const cs_x86_op &Destination = X86.operands[0];
  const cs_x86_op &First = X86.operands[FirstSourceIndex];
  const cs_x86_op &Second = X86.operands[SecondSourceIndex];
  const bool MemoryForm = Second.type == X86_OP_MEM;
  const bool Broadcast = (Encoding.P2 & 0x10) != 0;
  const unsigned LaneCount = VectorSize / Spec.ElementSize;
  const uint16_t MaskSize =
      static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));

  if (!isX86OpmaskOperand(Destination) || Destination.size != MaskSize ||
      !isX86VectorRegisterOfSize(First, VectorSize) ||
      ((Encoding.ModRM >> 3) & 7) !=
          static_cast<unsigned>(Destination.reg - X86_REG_K0) ||
      decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
          x86VectorRegisterIndex(First) ||
      (((Encoding.ModRM & 0xc0) != 0xc0) != MemoryForm))
    return false;

  const uint8_t EncodedMask = Encoding.P2 & 7;
  if (HasWriteMask) {
    const cs_x86_op &Mask = X86.operands[1];
    const RegInfo MaskInfo = mapCapstoneReg(static_cast<x86_reg>(Mask.reg));
    if (Mask.reg == X86_REG_K0 || Mask.size != MaskSize ||
        MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < MaskSize ||
        EncodedMask != static_cast<uint8_t>(Mask.reg - X86_REG_K0))
      return false;
  } else if (EncodedMask != 0) {
    return false;
  }

  const x86_avx_bcast ExpectedBroadcast =
      Broadcast ? evexBroadcastForLaneCount(LaneCount)
                : X86_AVX_BCAST_INVALID;
  for (unsigned Index = 0; Index < X86.op_count; ++Index) {
    const cs_x86_op &Operand = X86.operands[Index];
    if (Operand.avx_zero_opmask ||
        (&Operand != &Second &&
         Operand.avx_bcast != X86_AVX_BCAST_INVALID))
      return false;
  }

  if (MemoryForm) {
    const uint16_t TupleSize = Broadcast ? Spec.ElementSize : VectorSize;
    if ((Broadcast && Spec.ElementSize < 4) || Second.size != TupleSize ||
        Second.avx_bcast != ExpectedBroadcast ||
        !validateCanonicalEvexMemoryTail(Insn, X86, Encoding, Second,
                                         TupleSize))
      return false;
  } else if (Broadcast || !isX86VectorRegisterOfSize(Second, VectorSize) ||
             Second.avx_bcast != X86_AVX_BCAST_INVALID ||
             decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
                 x86VectorRegisterIndex(Second) ||
             !validateCanonicalEvexRegisterTail(Insn, X86, Encoding)) {
    return false;
  }

  const RegInfo DestinationInfo =
      mapCapstoneReg(static_cast<x86_reg>(Destination.reg));
  if (DestinationInfo.Offset == UINT64_C(0xffff) || DestinationInfo.Size < 8)
    return false;

  NdVar ActiveMask = NdVar::cst(
      LaneCount == 64 ? UINT64_MAX : ((UINT64_C(1) << LaneCount) - 1),
      MaskSize);
  if (HasWriteMask)
    ActiveMask = L.operandRead(S, X86.operands[1]);
  NdVar FirstValue = L.operandRead(S, First);
  NdVar SecondValue =
      MemoryForm
          ? emitEvexMaskedMemoryLoad(S, Second, ActiveMask, VectorSize,
                                     Spec.ElementSize,
                                     Broadcast ? Spec.ElementSize : VectorSize,
                                     Broadcast)
          : L.operandRead(S, Second);
  if (ActiveMask.Size != MaskSize || FirstValue.Size != VectorSize ||
      SecondValue.Size != VectorSize)
    return false;

  NdVar Packed = NdVar::cst(0, MaskSize);
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    NdVar FirstLane = S.makeTemp(Spec.ElementSize);
    NdVar SecondLane = S.makeTemp(Spec.ElementSize);
    S.emit(NdOp::SUBBYTES, FirstLane,
           {FirstValue, NdVar::cst(Lane * Spec.ElementSize, 4)});
    S.emit(NdOp::SUBBYTES, SecondLane,
           {SecondValue, NdVar::cst(Lane * Spec.ElementSize, 4)});
    NdVar Intersection = S.makeTemp(Spec.ElementSize);
    S.emit(NdOp::INT_AND, Intersection, {FirstLane, SecondLane});
    NdVar Matches = S.makeTemp(1);
    S.emit(Spec.Negated ? NdOp::INT_EQUAL : NdOp::INT_NOTEQUAL, Matches,
           {Intersection, NdVar::cst(0, Spec.ElementSize)});
    NdVar Bit = S.makeTemp(MaskSize);
    S.emit(NdOp::INT_ZEXT, Bit, {Matches});
    if (Lane != 0) {
      NdVar Shifted = S.makeTemp(MaskSize);
      S.emit(NdOp::INT_LEFT, Shifted,
             {Bit, NdVar::cst(Lane, MaskSize)});
      Bit = Shifted;
    }
    NdVar Next = S.makeTemp(MaskSize);
    S.emit(NdOp::INT_OR, Next, {Packed, Bit});
    Packed = Next;
  }
  if (HasWriteMask) {
    NdVar Masked = S.makeTemp(MaskSize);
    S.emit(NdOp::INT_AND, Masked, {Packed, ActiveMask});
    Packed = Masked;
  }
  S.emit(NdOp::COPY, L.operandWrite(Destination), {Packed});
  return true;
}

} // namespace

bool liftSIMDAVXInt(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                    const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  EvexMaskTestSpec MaskTestSpec{};
  const bool IsEvexMaskTest = getEvexMaskTestSpec(InsnId, MaskTestSpec);
  uint8_t IntegerLogicOpcode = 0;
  bool IntegerLogicW = false;
  bool IntegerLogicAndNot = false;
  NdOp IntegerLogicOperation = NdOp::INT_AND;
  const bool IsEvexIntegerLogic = getEvexIntegerLogicEncoding(
      InsnId, IntegerLogicOpcode, IntegerLogicW, IntegerLogicAndNot,
      IntegerLogicOperation);
  EvexUnaryIntegerSpec UnaryIntegerSpec{};
  const bool IsEvexUnaryInteger =
      getEvexUnaryIntegerSpec(InsnId, UnaryIntegerSpec);
  EvexRotateSpec RotateSpec{};
  const bool IsEvexRotate = getEvexRotateSpec(InsnId, RotateSpec);
  EvexVariableShiftSpec VariableShiftSpec{};
  const bool IsEvexVariableShift =
      getEvexVariableShiftSpec(InsnId, VariableShiftSpec);
  EvexUniformShiftSpec UniformShiftSpec{};
  const bool IsEvexUniformShift =
      getEvexUniformShiftSpec(InsnId, UniformShiftSpec);
  EvexPackedMultiplySpec PackedMultiplySpec{};
  const bool IsEvexPackedMultiply =
      getEvexPackedMultiplySpec(InsnId, PackedMultiplySpec);
  EvexVnniSpec VnniSpec{};
  const bool IsEvexVnni = getEvexVnniSpec(InsnId, VnniSpec);
  EvexIfmaSpec IfmaSpec{};
  const bool IsEvexIfma = getEvexIfmaSpec(InsnId, IfmaSpec);
  EvexDoubleShiftSpec DoubleShiftSpec{};
  const bool IsEvexDoubleShift =
      getEvexDoubleShiftSpec(InsnId, DoubleShiftSpec);
  EvexVbmiSpec VbmiSpec{};
  const bool IsEvexVbmi = getEvexVbmiSpec(InsnId, VbmiSpec);
  const bool IsEvexBitShuffle = InsnId == X86_INS_VPSHUFBITQMB;
  EvexBlendSpec BlendSpec{};
  const bool IsEvexBlend = getEvexBlendSpec(InsnId, BlendSpec);
  if (InsnId != X86_INS_VPTERNLOGD && InsnId != X86_INS_VPTERNLOGQ &&
      !IsEvexMaskTest && !IsEvexIntegerLogic && !IsEvexUnaryInteger &&
      !IsEvexRotate && !IsEvexVariableShift && !IsEvexUniformShift &&
      !IsEvexPackedMultiply && !IsEvexVnni && !IsEvexIfma &&
      !IsEvexDoubleShift && !IsEvexVbmi && !IsEvexBitShuffle &&
      !IsEvexBlend &&
      hasUnsupportedEvexValueModifier(X86))
    return false;
  switch (InsnId) {

  // ========================================================================
  // P1: AVX-512 packed VP* integer instructions.
  // ========================================================================

  // VPAND{D,Q} / VPOR{D,Q} / VPXOR{D,Q} — AVX-512 bitwise (EVEX).
  case X86_INS_VPANDD:
  case X86_INS_VPANDQ:
  case X86_INS_VPORD:
  case X86_INS_VPORQ:
  case X86_INS_VPXORD:
  case X86_INS_VPXORQ:
  case X86_INS_VPANDND:
  case X86_INS_VPANDNQ: {
    EvexIntegerLogicInfo Info;
    if (!validateEvexIntegerLogic(L, Insn, X86, Info))
      return false;

    NdVar ActiveMask = NdVar::cst(
        (UINT64_C(1) << Info.LaneCount) - UINT64_C(1), Info.MaskSize);
    if (Info.MaskOperand) {
      const RegInfo MaskInfo = mapCapstoneReg(
          static_cast<x86_reg>(Info.MaskOperand->reg));
      if (MaskInfo.Offset == UINT64_C(0xffff) ||
          MaskInfo.Size < Info.MaskSize)
        return false;
      ActiveMask = NdVar::reg(MaskInfo.Offset, Info.MaskSize);
    }
    const cs_x86_op &RightOperand = X86.operands[Info.RightIndex];
    const NdVar Left = L.operandRead(S, X86.operands[Info.LeftIndex]);
    NdVar Right = Info.MemoryForm
                      ? emitEvexMaskedMemoryLoad(
                            S, RightOperand, ActiveMask, Info.VectorSize,
                            Info.ElementSize,
                            Info.Broadcast ? Info.ElementSize : Info.VectorSize,
                            Info.Broadcast)
                      : L.operandRead(S, RightOperand);
    if (Left.Size != Info.VectorSize || Right.Size != Info.VectorSize)
      return false;

    NdVar Raw = S.makeTemp(Info.VectorSize);
    if (Info.AndNot) {
      NdVar Inverted = S.makeTemp(Info.VectorSize);
      S.emit(NdOp::INT_NOT, Inverted, {Left});
      S.emit(NdOp::INT_AND, Raw, {Inverted, Right});
    } else {
      S.emit(Info.Operation, Raw, {Left, Right});
    }
    if (Info.MaskOperand)
      return emitMaskedVectorResult(L, S, X86.operands[0], *Info.MaskOperand,
                                    Raw, Info.ElementSize);
    S.emit(NdOp::COPY, L.operandWrite(X86.operands[0]), {Raw});
    return true;
  }

  // VPTERNLOG{D,Q} — ternary logic (imm8 truth table).
  case X86_INS_VPTERNLOGD:
  case X86_INS_VPTERNLOGQ: {
    const bool HasWriteMask =
        X86.op_count >= 2 && isX86OpmaskOperand(X86.operands[1]);
    const unsigned FirstSourceIndex = HasWriteMask ? 2 : 1;
    const unsigned SecondSourceIndex = HasWriteMask ? 3 : 2;
    const unsigned ImmediateIndex = HasWriteMask ? 4 : 3;
    const unsigned ExpectedOperands = HasWriteMask ? 5 : 4;
    if (X86.op_count != ExpectedOperands)
      return false;

    const cs_x86_op &DestinationOperand = X86.operands[0];
    const cs_x86_op &FirstSourceOperand = X86.operands[FirstSourceIndex];
    const cs_x86_op &SecondSourceOperand = X86.operands[SecondSourceIndex];
    const cs_x86_op &ImmediateOperand = X86.operands[ImmediateIndex];
    const uint16_t VectorSize = DestinationOperand.size;
    const uint16_t ElementSize = InsnId == X86_INS_VPTERNLOGQ ? 8 : 4;
    if (!isX86VectorRegisterOfSize(DestinationOperand, VectorSize) ||
        !isX86VectorRegisterOfSize(FirstSourceOperand, VectorSize) ||
        (SecondSourceOperand.type != X86_OP_REG &&
         SecondSourceOperand.type != X86_OP_MEM) ||
        ImmediateOperand.type != X86_OP_IMM || ImmediateOperand.size != 1)
      return false;

    CanonicalEvexEncodingInfo Encoding;
    if (!hasCanonicalTernaryLogicEncoding(
            Insn, X86, L.targetArch(), SecondSourceOperand, ImmediateOperand,
            VectorSize, ElementSize, ElementSize == 8, Encoding))
      return false;
    const bool MemoryForm = (Encoding.ModRM & 0xc0) != 0xc0;
    const bool Broadcast = MemoryForm && (Encoding.P2 & 0x10) != 0;
    if (MemoryForm != (SecondSourceOperand.type == X86_OP_MEM) ||
        (!MemoryForm &&
         !isX86VectorRegisterOfSize(SecondSourceOperand, VectorSize)) ||
        decodeEvexVectorRegIndex(Encoding.P0, Encoding.ModRM) !=
            x86VectorRegisterIndex(DestinationOperand) ||
        decodeEvexVectorVvvvIndex(Encoding.P1, Encoding.P2) !=
            x86VectorRegisterIndex(FirstSourceOperand) ||
        (!MemoryForm &&
         decodeEvexVectorRMIndex(Encoding.P0, Encoding.ModRM) !=
             x86VectorRegisterIndex(SecondSourceOperand)))
      return false;

    const x86_avx_bcast ExpectedBroadcast =
        evexBroadcastForLaneCount(VectorSize / ElementSize);
    for (unsigned Index = 0; Index < X86.op_count; ++Index) {
      if (Index == SecondSourceIndex && Broadcast) {
        if (X86.operands[Index].avx_bcast != ExpectedBroadcast)
          return false;
      } else if (X86.operands[Index].avx_bcast != X86_AVX_BCAST_INVALID) {
        return false;
      }
      if (X86.operands[Index].avx_zero_opmask && (!HasWriteMask || Index != 1))
        return false;
    }

    const unsigned LaneCount = VectorSize / ElementSize;
    const uint16_t MaskSize =
        static_cast<uint16_t>(std::max(1u, (LaneCount + 7u) / 8u));
    NdVar ActiveMask = NdVar::cst(UINT64_MAX, 8);
    if (HasWriteMask) {
      const cs_x86_op &MaskOperand = X86.operands[1];
      const RegInfo MaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(MaskOperand.reg));
      if (MaskOperand.reg == X86_REG_K0 ||
          (Encoding.P2 & 7) != MaskOperand.reg - X86_REG_K0 ||
          ((Encoding.P2 & 0x80) != 0) != MaskOperand.avx_zero_opmask ||
          MaskOperand.size < MaskSize || MaskOperand.size > 8 ||
          MaskInfo.Offset == UINT64_C(0xffff) || MaskInfo.Size < MaskSize)
        return false;
      ActiveMask = NdVar::reg(MaskInfo.Offset, MaskSize);
    } else if ((Encoding.P2 & 0x87) != 0) {
      return false;
    }

    NdVar Dst = L.operandWrite(DestinationOperand);
    NdVar OldDst = L.operandRead(S, DestinationOperand);
    NdVar First = L.operandRead(S, FirstSourceOperand);
    NdVar Second =
        MemoryForm
            ? emitEvexMaskedMemoryLoad(
                  S, SecondSourceOperand, ActiveMask, VectorSize, ElementSize,
                  Broadcast ? ElementSize : VectorSize, Broadcast)
            : L.operandRead(S, SecondSourceOperand);
    if (Dst.Size == 0 || OldDst.Size != Dst.Size || First.Size != Dst.Size ||
        Second.Size != Dst.Size)
      return false;

    NdVar Negated[3] = {S.makeTemp(Dst.Size), S.makeTemp(Dst.Size),
                        S.makeTemp(Dst.Size)};
    const NdVar Inputs[3] = {OldDst, First, Second};
    for (unsigned Input = 0; Input < 3; ++Input)
      S.emit(NdOp::INT_NOT, Negated[Input], {Inputs[Input]});

    const uint8_t TruthTable = static_cast<uint8_t>(ImmediateOperand.imm);
    NdVar Raw = NdVar::cst(0, Dst.Size);
    bool HasTerm = false;
    for (unsigned Index = 0; Index < 8; ++Index) {
      if ((TruthTable & (1u << Index)) == 0)
        continue;
      const NdVar &A = (Index & 4) != 0 ? Inputs[0] : Negated[0];
      const NdVar &B = (Index & 2) != 0 ? Inputs[1] : Negated[1];
      const NdVar &C = (Index & 1) != 0 ? Inputs[2] : Negated[2];
      NdVar Pair = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_AND, Pair, {A, B});
      NdVar Term = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_AND, Term, {Pair, C});
      if (!HasTerm) {
        Raw = Term;
        HasTerm = true;
      } else {
        NdVar Combined = S.makeTemp(Dst.Size);
        S.emit(NdOp::INT_OR, Combined, {Raw, Term});
        Raw = Combined;
      }
    }

    if (HasWriteMask) {
      if (!emitMaskedVectorResult(L, S, DestinationOperand, X86.operands[1],
                                  Raw, ElementSize))
        return false;
    } else {
      S.emit(NdOp::COPY, Dst, {Raw});
    }
    break;
  }

  // VPCOMPRESS{B,W,D,Q} — compress active elements under Mask.
  case X86_INS_VPCOMPRESSB:
  case X86_INS_VPCOMPRESSW:
  case X86_INS_VPCOMPRESSD:
  case X86_INS_VPCOMPRESSQ: {
    uint16_t ElementSize = 1;
    if (InsnId == X86_INS_VPCOMPRESSW)
      ElementSize = 2;
    else if (InsnId == X86_INS_VPCOMPRESSD)
      ElementSize = 4;
    else if (InsnId == X86_INS_VPCOMPRESSQ)
      ElementSize = 8;
    const bool ByteOrWord = ElementSize <= 2;
    return liftEvexCompressExpandRegister(L, S, Insn, X86, ElementSize, true,
                                          ByteOrWord ? 0x63 : 0x8b,
                                          ElementSize == 2 || ElementSize == 8);
  }

  // VPEXPAND{B,W,D,Q} — expand active elements under Mask.
  case X86_INS_VPEXPANDB:
  case X86_INS_VPEXPANDW:
  case X86_INS_VPEXPANDD:
  case X86_INS_VPEXPANDQ: {
    uint16_t ElementSize = 1;
    if (InsnId == X86_INS_VPEXPANDW)
      ElementSize = 2;
    else if (InsnId == X86_INS_VPEXPANDD)
      ElementSize = 4;
    else if (InsnId == X86_INS_VPEXPANDQ)
      ElementSize = 8;
    const bool ByteOrWord = ElementSize <= 2;
    return liftEvexCompressExpandRegister(L, S, Insn, X86, ElementSize, false,
                                          ByteOrWord ? 0x62 : 0x89,
                                          ElementSize == 2 || ElementSize == 8);
  }

  // VPSCATTER{DD,DQ,QD,QQ} — scatter store to memory via index vector.
  case X86_INS_VPSCATTERDD:
  case X86_INS_VPSCATTERDQ:
  case X86_INS_VPSCATTERQD:
  case X86_INS_VPSCATTERQQ:
    return false;

  // Unary AVX-512 integer operations with ordinary writemasking.
  case X86_INS_VPOPCNTB:
  case X86_INS_VPOPCNTW:
  case X86_INS_VPOPCNTD:
  case X86_INS_VPOPCNTQ:
  case X86_INS_VPLZCNTD:
  case X86_INS_VPLZCNTQ:
  case X86_INS_VPCONFLICTD:
  case X86_INS_VPCONFLICTQ:
    return liftEvexUnaryInteger(L, S, Insn, X86);

  // VPTESTM{B,W,D,Q} / VPTESTNM{B,W,D,Q} — test Mask Bits.
  case X86_INS_VPTESTMB:
  case X86_INS_VPTESTMW:
  case X86_INS_VPTESTMD:
  case X86_INS_VPTESTMQ:
  case X86_INS_VPTESTNMB:
  case X86_INS_VPTESTNMW:
  case X86_INS_VPTESTNMD:
  case X86_INS_VPTESTNMQ:
    return liftEvexMaskTest(L, S, Insn, X86, InsnId);

  // VPROL{D,Q} / VPROR{D,Q} — packed rotate by immediate.
  case X86_INS_VPROLD:
  case X86_INS_VPROLQ:
  case X86_INS_VPRORD:
  case X86_INS_VPRORQ:
    return liftEvexRotate(L, S, Insn, X86);

  // VPROLV{D,Q} / VPRORV{D,Q} — packed variable rotate.
  case X86_INS_VPROLVD:
  case X86_INS_VPROLVQ:
  case X86_INS_VPRORVD:
  case X86_INS_VPRORVQ:
    return liftEvexRotate(L, S, Insn, X86);

  // EVEX packed variable shifts.
  case X86_INS_VPSRAVW:
  case X86_INS_VPSRAVD:
  case X86_INS_VPSRAVQ:
  case X86_INS_VPSLLVW:
  case X86_INS_VPSLLVD:
  case X86_INS_VPSLLVQ:
  case X86_INS_VPSRLVW:
  case X86_INS_VPSRLVD:
  case X86_INS_VPSRLVQ:
    return liftEvexVariableShift(L, S, Insn, X86);

  // EVEX packed uniform shifts with either an imm8 or a shared XMM count.
  case X86_INS_VPSLLW:
  case X86_INS_VPSLLD:
  case X86_INS_VPSLLQ:
  case X86_INS_VPSRLW:
  case X86_INS_VPSRLD:
  case X86_INS_VPSRLQ:
  case X86_INS_VPSRAW:
  case X86_INS_VPSRAD:
  case X86_INS_VPSRAQ:
    return liftEvexUniformShift(L, S, Insn, X86);

  // VPMULL{D,W} / VPMULH{W,UW} / VPMUL{DQ,UDQ} — packed multiply per-lane.
  case X86_INS_VPMULLD:
  case X86_INS_VPMULLQ:
  case X86_INS_VPMULLW:
  case X86_INS_VPMULHW:
  case X86_INS_VPMULHUW:
  case X86_INS_VPMULDQ:
  case X86_INS_VPMULUDQ: {
    if (beginsWithPotentialEvexPrefix(Insn) || X86.opcode[0] == 0x62)
      return liftEvexPackedMultiply(L, S, Insn, X86);
    if (InsnId == X86_INS_VPMULLQ)
      return false;
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
    if (Dst.Size == 0 || A.Size != Dst.Size || B.Size != Dst.Size)
      return false;
    unsigned LaneSz = 4;
    if (InsnId == X86_INS_VPMULLW || InsnId == X86_INS_VPMULHW ||
        InsnId == X86_INS_VPMULHUW)
      LaneSz = 2;
    else if (InsnId == X86_INS_VPMULDQ || InsnId == X86_INS_VPMULUDQ)
      LaneSz = 8;
    if (Dst.Size > LaneSz) {
      unsigned NLanes = Dst.Size / LaneSz;
      bool IsHigh = (InsnId == X86_INS_VPMULHW || InsnId == X86_INS_VPMULHUW);
      bool IsWidening =
          (InsnId == X86_INS_VPMULDQ || InsnId == X86_INS_VPMULUDQ);
      bool IsSigned = (InsnId == X86_INS_VPMULHW || InsnId == X86_INS_VPMULDQ);
      NdVar Acc = S.makeTemp(0);
      for (unsigned I = 0; I < NLanes; ++I) {
        // The widening forms consume every other narrow element, so both forms
        // step one destination lane per iteration.
        unsigned SrcLaneSz = IsWidening ? LaneSz / 2 : LaneSz;
        unsigned SrcOff = I * LaneSz;
        NdVar La = S.makeTemp(SrcLaneSz);
        NdVar Lb = S.makeTemp(SrcLaneSz);
        S.emit(NdOp::SUBBYTES, La, {A, NdVar::cst(SrcOff, 4)});
        S.emit(NdOp::SUBBYTES, Lb, {B, NdVar::cst(SrcOff, 4)});
        NdVar Lr;
        if (IsHigh) {
          unsigned WideSz = LaneSz * 2;
          NdVar WA = S.makeTemp(WideSz);
          NdVar WB = S.makeTemp(WideSz);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WA, {La});
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WB, {Lb});
          NdVar WR = S.makeTemp(WideSz);
          S.emit(NdOp::INT_MULT, WR, {WA, WB});
          Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::SUBBYTES, Lr, {WR, NdVar::cst(LaneSz, 4)});
        } else if (IsWidening) {
          NdVar WA = S.makeTemp(LaneSz);
          NdVar WB = S.makeTemp(LaneSz);
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WA, {La});
          S.emit(IsSigned ? NdOp::INT_SEXT : NdOp::INT_ZEXT, WB, {Lb});
          Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_MULT, Lr, {WA, WB});
        } else {
          Lr = S.makeTemp(LaneSz);
          S.emit(NdOp::INT_MULT, Lr, {La, Lb});
        }
        if (I == 0) {
          Acc = Lr;
        } else {
          NdVar Next = S.makeTemp(Acc.Size + LaneSz);
          S.emit(NdOp::CONCAT, Next, {Lr, Acc});
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
    } else {
      NdVar Raw = HasWriteMask ? S.makeTemp(Dst.Size) : Dst;
      S.emit(NdOp::INT_MULT, Raw, {A, B});
      if (HasWriteMask && !emitMaskedVectorResult(L, S, X86.operands[0],
                                                  X86.operands[1], Raw, LaneSz))
        return false;
    }
    break;
  }

  // VPABSQ — packed absolute value (qword).
  case X86_INS_VPABSQ:
    return liftEvexUnaryInteger(L, S, Insn, X86);

  // VPBLENDM{B,W,D,Q} — Masked blend.
  case X86_INS_VPBLENDMB:
  case X86_INS_VPBLENDMW:
  case X86_INS_VPBLENDMD:
  case X86_INS_VPBLENDMQ: {
    EvexBlendInfo Info;
    if (!validateEvexBlend(L, Insn, X86, Info))
      return false;
    const uint16_t ElementSize = Info.Spec.ElementSize;
    const cs_x86_op &DestinationOperand = X86.operands[0];
    const cs_x86_op &FirstSourceOperand =
        X86.operands[Info.FirstSourceIndex];
    const cs_x86_op &SecondSourceOperand =
        X86.operands[Info.SecondSourceIndex];

    NdVar Dst = L.operandWrite(DestinationOperand);
    NdVar First = L.operandRead(S, FirstSourceOperand);
    NdVar ActiveMask;
    if (Info.SelectionMaskOperand) {
      const RegInfo MaskInfo = mapCapstoneReg(
          static_cast<x86_reg>(Info.SelectionMaskOperand->reg));
      ActiveMask = NdVar::reg(MaskInfo.Offset, Info.MaskSize);
    } else {
      const uint64_t AllLanes = Info.LaneCount == 64
                                    ? UINT64_MAX
                                    : (UINT64_C(1) << Info.LaneCount) - 1;
      ActiveMask = NdVar::cst(AllLanes, Info.MaskSize);
    }
    NdVar Second =
        Info.MemoryForm
            ? emitEvexMaskedMemoryLoad(
                  S, SecondSourceOperand, ActiveMask, Info.VectorSize,
                  ElementSize,
                  Info.Broadcast ? ElementSize : Info.VectorSize,
                  Info.Broadcast)
            : L.operandRead(S, SecondSourceOperand);
    if (Dst.Size != Info.VectorSize || First.Size != Dst.Size ||
        Second.Size != Dst.Size)
      return false;
    if (!Info.SelectionMaskOperand) {
      S.emit(NdOp::COPY, Dst, {Second});
      break;
    }

    NdVar Mask = L.operandRead(S, *Info.SelectionMaskOperand);
    if (Mask.Size != Info.MaskSize)
      return false;
    NdVar Result = S.makeTemp(0);
    for (unsigned Lane = 0; Lane < Info.LaneCount; ++Lane) {
      const uint64_t Offset = static_cast<uint64_t>(Lane) * ElementSize;
      NdVar FirstLane = S.makeTemp(ElementSize);
      NdVar SecondLane = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, FirstLane, {First, NdVar::cst(Offset, 4)});
      S.emit(NdOp::SUBBYTES, SecondLane, {Second, NdVar::cst(Offset, 4)});
      NdVar ShiftedMask = Mask;
      if (Lane != 0) {
        ShiftedMask = S.makeTemp(Info.MaskSize);
        S.emit(NdOp::INT_RIGHT, ShiftedMask,
               {Mask, NdVar::cst(Lane, Info.MaskSize)});
      }
      NdVar MaskBitWide = S.makeTemp(Info.MaskSize);
      S.emit(NdOp::INT_AND, MaskBitWide,
             {ShiftedMask, NdVar::cst(1, Info.MaskSize)});
      NdVar MaskBit = MaskBitWide;
      if (Info.MaskSize != 1) {
        MaskBit = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, MaskBit, {MaskBitWide, NdVar::cst(0, 4)});
      }
      NdVar Inactive = FirstLane;
      if (Info.ZeroMask)
        Inactive = NdVar::cst(0, ElementSize);
      NdVar Selected = S.makeTemp(ElementSize);
      S.emit(NdOp::SELECT, Selected, {MaskBit, SecondLane, Inactive});
      if (Lane == 0) {
        Result = Selected;
      } else {
        NdVar Next = S.makeTemp(Result.Size + ElementSize);
        S.emit(NdOp::CONCAT, Next, {Selected, Result});
        Result = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Result});
    break;
  }

  // VPMOV{DB,DW,QB,QW,QD,WB} — packed truncate (down-convert).
  case X86_INS_VPMOVDB:
  case X86_INS_VPMOVDW:
  case X86_INS_VPMOVQB:
  case X86_INS_VPMOVQW:
  case X86_INS_VPMOVQD:
  case X86_INS_VPMOVWB:
  case X86_INS_VPMOVSDB:
  case X86_INS_VPMOVSDW:
  case X86_INS_VPMOVSQB:
  case X86_INS_VPMOVSQW:
  case X86_INS_VPMOVSQD:
  case X86_INS_VPMOVSWB:
  case X86_INS_VPMOVUSDB:
  case X86_INS_VPMOVUSDW:
  case X86_INS_VPMOVUSQB:
  case X86_INS_VPMOVUSQW:
  case X86_INS_VPMOVUSQD:
  case X86_INS_VPMOVUSWB:
    return false;

  // VPMOVM2{B,W,D,Q} — Mask to vector.
  case X86_INS_VPMOVM2B:
  case X86_INS_VPMOVM2W:
  case X86_INS_VPMOVM2D:
  case X86_INS_VPMOVM2Q: {
    uint16_t ElementSize = 1;
    if (InsnId == X86_INS_VPMOVM2W)
      ElementSize = 2;
    else if (InsnId == X86_INS_VPMOVM2D)
      ElementSize = 4;
    else if (InsnId == X86_INS_VPMOVM2Q)
      ElementSize = 8;

    if (X86.op_count != 2)
      return false;
    const cs_x86_op &DestinationOperand = X86.operands[0];
    const cs_x86_op &MaskOperand = X86.operands[1];
    if (!isX86VectorRegisterOperand(DestinationOperand) ||
        !isX86OpmaskOperand(MaskOperand) ||
        DestinationOperand.avx_zero_opmask || MaskOperand.avx_zero_opmask ||
        (DestinationOperand.size != 16 && DestinationOperand.size != 32 &&
         DestinationOperand.size != 64) ||
        DestinationOperand.size % ElementSize != 0)
      return false;

    const uint16_t VectorSize = static_cast<uint16_t>(DestinationOperand.size);
    const unsigned LaneCount = VectorSize / ElementSize;
    const uint16_t MaskSize = static_cast<uint16_t>((LaneCount + 7u) / 8u);
    if (MaskOperand.size != MaskSize ||
        !hasCanonicalMaskConversionEncoding(
            Insn, X86, L.targetArch(), DestinationOperand, MaskOperand,
            VectorSize, ElementSize, true))
      return false;

    const NdVar Dst = L.operandWrite(DestinationOperand);
    const NdVar Mask = L.operandRead(S, MaskOperand);
    NdVar Result = S.makeTemp(0);
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      NdVar ShiftedMask = Mask;
      if (Lane != 0) {
        ShiftedMask = S.makeTemp(MaskSize);
        S.emit(NdOp::INT_RIGHT, ShiftedMask,
               {Mask, NdVar::cst(Lane, MaskSize)});
      }
      NdVar MaskBitWide = S.makeTemp(MaskSize);
      S.emit(NdOp::INT_AND, MaskBitWide,
             {ShiftedMask, NdVar::cst(1, MaskSize)});
      NdVar MaskBit = MaskBitWide;
      if (MaskSize != 1) {
        MaskBit = S.makeTemp(1);
        S.emit(NdOp::SUBBYTES, MaskBit, {MaskBitWide, NdVar::cst(0, 4)});
      }

      NdVar ResultLane = S.makeTemp(ElementSize);
      S.emit(NdOp::SELECT, ResultLane,
             {MaskBit, NdVar::cst(UINT64_MAX, ElementSize),
              NdVar::cst(0, ElementSize)});
      if (Lane == 0) {
        Result = ResultLane;
      } else {
        NdVar Next = S.makeTemp(Result.Size + ElementSize);
        S.emit(NdOp::CONCAT, Next, {ResultLane, Result});
        Result = Next;
      }
    }
    S.emit(NdOp::COPY, Dst, {Result});
    break;
  }

  // VPMOV{B,W,D,Q}2M — vector to Mask.
  case X86_INS_VPMOVB2M:
  case X86_INS_VPMOVW2M:
  case X86_INS_VPMOVD2M:
  case X86_INS_VPMOVQ2M: {
    uint16_t ElementSize = 1;
    if (InsnId == X86_INS_VPMOVW2M)
      ElementSize = 2;
    else if (InsnId == X86_INS_VPMOVD2M)
      ElementSize = 4;
    else if (InsnId == X86_INS_VPMOVQ2M)
      ElementSize = 8;

    if (X86.op_count != 2)
      return false;
    const cs_x86_op &MaskOperand = X86.operands[0];
    const cs_x86_op &SourceOperand = X86.operands[1];
    if (!isX86OpmaskOperand(MaskOperand) ||
        !isX86VectorRegisterOperand(SourceOperand) ||
        MaskOperand.avx_zero_opmask || SourceOperand.avx_zero_opmask ||
        (SourceOperand.size != 16 && SourceOperand.size != 32 &&
         SourceOperand.size != 64) ||
        SourceOperand.size % ElementSize != 0)
      return false;

    const uint16_t VectorSize = static_cast<uint16_t>(SourceOperand.size);
    const unsigned LaneCount = VectorSize / ElementSize;
    const uint16_t MaskSize = static_cast<uint16_t>((LaneCount + 7u) / 8u);
    if (MaskOperand.size != MaskSize ||
        !hasCanonicalMaskConversionEncoding(
            Insn, X86, L.targetArch(), SourceOperand, MaskOperand, VectorSize,
            ElementSize, false))
      return false;

    const NdVar Dst = L.operandWrite(MaskOperand);
    const NdVar Src = L.operandRead(S, SourceOperand);
    NdVar Result = NdVar::cst(0, MaskSize);
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      NdVar InputLane = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, InputLane,
             {Src, NdVar::cst(Lane * ElementSize, 4)});
      NdVar SignAtBitZero = S.makeTemp(ElementSize);
      S.emit(NdOp::INT_RIGHT, SignAtBitZero,
             {InputLane, NdVar::cst(ElementSize * 8 - 1, ElementSize)});
      NdVar SignByte = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, SignByte, {SignAtBitZero, NdVar::cst(0, 4)});
      NdVar MaskedSign = S.makeTemp(1);
      S.emit(NdOp::INT_AND, MaskedSign, {SignByte, NdVar::cst(1, 1)});

      NdVar WideSign = MaskedSign;
      if (MaskSize != 1) {
        WideSign = S.makeTemp(MaskSize);
        S.emit(NdOp::INT_ZEXT, WideSign, {MaskedSign});
      }
      NdVar PositionedSign = WideSign;
      if (Lane != 0) {
        PositionedSign = S.makeTemp(MaskSize);
        S.emit(NdOp::INT_LEFT, PositionedSign,
               {WideSign, NdVar::cst(Lane, MaskSize)});
      }
      NdVar Combined = S.makeTemp(MaskSize);
      S.emit(NdOp::INT_OR, Combined, {Result, PositionedSign});
      Result = Combined;
    }
    S.emit(NdOp::COPY, Dst, {Result});
    break;
  }

  // VPBROADCASTM{B2Q,W2D} — broadcast Mask to vector.
  case X86_INS_VPBROADCASTMB2Q:
  case X86_INS_VPBROADCASTMW2D: {
    const bool WordSource = InsnId == X86_INS_VPBROADCASTMW2D;
    const uint16_t SourceSize = WordSource ? 2 : 1;
    const uint16_t ElementSize = WordSource ? 4 : 8;
    if (X86.op_count != 2 || !isX86VectorRegisterOperand(X86.operands[0]) ||
        !isX86OpmaskOperand(X86.operands[1]) ||
        X86.operands[0].avx_zero_opmask || X86.operands[1].avx_zero_opmask)
      return false;

    const cs_x86_op &DestinationOperand = X86.operands[0];
    const uint16_t VectorSize = static_cast<uint16_t>(DestinationOperand.size);
    if ((VectorSize != 16 && VectorSize != 32 && VectorSize != 64) ||
        VectorSize % ElementSize != 0 ||
        !hasCanonicalMaskBroadcastEncoding(
            Insn, X86, L.targetArch(), DestinationOperand, X86.operands[1],
            VectorSize, WordSource))
      return false;

    const RegInfo SourceInfo =
        mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
    if (SourceInfo.Size < SourceSize)
      return false;
    const NdVar FullSource = NdVar::reg(SourceInfo.Offset, SourceInfo.Size);
    NdVar Source = S.makeTemp(SourceSize);
    S.emit(NdOp::SUBBYTES, Source,
           {FullSource, NdVar::cst(0, sizeof(uint32_t))});
    NdVar Lane = S.makeTemp(ElementSize);
    S.emit(NdOp::INT_ZEXT, Lane, {Source});

    NdVar Result = Lane;
    for (unsigned LaneIndex = 1; LaneIndex < VectorSize / ElementSize;
         ++LaneIndex) {
      NdVar Next = S.makeTemp(Result.Size + ElementSize);
      S.emit(NdOp::CONCAT, Next, {Lane, Result});
      Result = Next;
    }
    S.emit(NdOp::COPY, L.operandWrite(DestinationOperand), {Result});
    break;
  }

  // VPSHLD{D,Q,W} / VPSHRD{D,Q,W} — double shift by immediate.
  case X86_INS_VPSHLDD:
  case X86_INS_VPSHLDQ:
  case X86_INS_VPSHLDW:
  case X86_INS_VPSHRDD:
  case X86_INS_VPSHRDQ:
  case X86_INS_VPSHRDW: {
    EvexDoubleShiftInfo Info;
    if (!validateEvexDoubleShift(L, Insn, X86, Info) || Info.Spec.Variable)
      return false;
    const uint16_t ElementSize = Info.Spec.ElementSize;
    const bool IsLeft = Info.Spec.Left;
    const cs_x86_op &DestinationOperand = X86.operands[0];
    const cs_x86_op &FirstSourceOperand =
        X86.operands[Info.FirstSourceIndex];
    const cs_x86_op &SecondSourceOperand =
        X86.operands[Info.SecondSourceIndex];

    NdVar Dst = L.operandWrite(DestinationOperand);
    NdVar First = L.operandRead(S, FirstSourceOperand);
    NdVar ActiveMask = NdVar::cst(
        (UINT64_C(1) << Info.LaneCount) - UINT64_C(1), Info.MaskSize);
    if (Info.MaskOperand) {
      const RegInfo MaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
      ActiveMask = NdVar::reg(MaskInfo.Offset, Info.MaskSize);
    }
    NdVar Second =
        Info.MemoryForm
            ? emitEvexMaskedMemoryLoad(
                  S, SecondSourceOperand, ActiveMask, Info.VectorSize,
                  ElementSize,
                  Info.Broadcast ? ElementSize : Info.VectorSize,
                  Info.Broadcast)
            : L.operandRead(S, SecondSourceOperand);
    if (Dst.Size != Info.VectorSize || First.Size != Dst.Size ||
        Second.Size != Dst.Size)
      return false;
    const unsigned LaneBits = ElementSize * 8;
    const uint64_t Count =
        static_cast<uint64_t>(X86.operands[Info.ImmediateIndex].imm) &
        (LaneBits - 1);
    const unsigned LaneCount = Dst.Size / ElementSize;
    NdVar Raw = S.makeTemp(0);
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Offset = static_cast<uint64_t>(Lane) * ElementSize;
      NdVar FirstLane = S.makeTemp(ElementSize);
      NdVar SecondLane = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, FirstLane, {First, NdVar::cst(Offset, 4)});
      S.emit(NdOp::SUBBYTES, SecondLane, {Second, NdVar::cst(Offset, 4)});
      NdVar ResultLane = FirstLane;
      if (Count != 0) {
        NdVar PrimaryPart = S.makeTemp(ElementSize);
        NdVar SecondaryPart = S.makeTemp(ElementSize);
        S.emit(IsLeft ? NdOp::INT_LEFT : NdOp::INT_RIGHT, PrimaryPart,
               {FirstLane, NdVar::cst(Count, ElementSize)});
        S.emit(IsLeft ? NdOp::INT_RIGHT : NdOp::INT_LEFT, SecondaryPart,
               {SecondLane, NdVar::cst(LaneBits - Count, ElementSize)});
        ResultLane = S.makeTemp(ElementSize);
        S.emit(NdOp::INT_OR, ResultLane, {PrimaryPart, SecondaryPart});
      }
      if (Lane == 0) {
        Raw = ResultLane;
      } else {
        NdVar Next = S.makeTemp(Raw.Size + ElementSize);
        S.emit(NdOp::CONCAT, Next, {ResultLane, Raw});
        Raw = Next;
      }
    }
    if (Info.MaskOperand) {
      if (!emitMaskedVectorResult(L, S, DestinationOperand,
                                  *Info.MaskOperand, Raw, ElementSize))
        return false;
    } else {
      S.emit(NdOp::COPY, Dst, {Raw});
    }
    break;
  }

  // VPSHLDV{D,Q,W} / VPSHRDV{D,Q,W} — variable double shift.
  case X86_INS_VPSHLDVD:
  case X86_INS_VPSHLDVQ:
  case X86_INS_VPSHLDVW:
  case X86_INS_VPSHRDVD:
  case X86_INS_VPSHRDVQ:
  case X86_INS_VPSHRDVW: {
    EvexDoubleShiftInfo Info;
    if (!validateEvexDoubleShift(L, Insn, X86, Info) || !Info.Spec.Variable)
      return false;
    const uint16_t ElementSize = Info.Spec.ElementSize;
    const bool IsLeft = Info.Spec.Left;
    const cs_x86_op &DestinationOperand = X86.operands[0];
    const cs_x86_op &SourceOperand = X86.operands[Info.FirstSourceIndex];
    const cs_x86_op &CountOperand = X86.operands[Info.SecondSourceIndex];

    NdVar Dst = L.operandWrite(DestinationOperand);
    NdVar OldDst = L.operandRead(S, DestinationOperand);
    NdVar Source = L.operandRead(S, SourceOperand);
    NdVar ActiveMask = NdVar::cst(
        (UINT64_C(1) << Info.LaneCount) - UINT64_C(1), Info.MaskSize);
    if (Info.MaskOperand) {
      const RegInfo MaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
      ActiveMask = NdVar::reg(MaskInfo.Offset, Info.MaskSize);
    }
    NdVar Counts =
        Info.MemoryForm
            ? emitEvexMaskedMemoryLoad(
                  S, CountOperand, ActiveMask, Info.VectorSize, ElementSize,
                  Info.Broadcast ? ElementSize : Info.VectorSize,
                  Info.Broadcast)
            : L.operandRead(S, CountOperand);
    if (Dst.Size != Info.VectorSize || OldDst.Size != Dst.Size ||
        Source.Size != Dst.Size || Counts.Size != Dst.Size)
      return false;
    const unsigned LaneBits = ElementSize * 8;
    const unsigned LaneCount = Dst.Size / ElementSize;
    NdVar Raw = S.makeTemp(0);
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Offset = static_cast<uint64_t>(Lane) * ElementSize;
      NdVar OldLane = S.makeTemp(ElementSize);
      NdVar SourceLane = S.makeTemp(ElementSize);
      NdVar CountLane = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, OldLane, {OldDst, NdVar::cst(Offset, 4)});
      S.emit(NdOp::SUBBYTES, SourceLane, {Source, NdVar::cst(Offset, 4)});
      S.emit(NdOp::SUBBYTES, CountLane, {Counts, NdVar::cst(Offset, 4)});
      NdVar MaskedCount = S.makeTemp(ElementSize);
      S.emit(NdOp::INT_AND, MaskedCount,
             {CountLane, NdVar::cst(LaneBits - 1, ElementSize)});
      NdVar NegativeCount = S.makeTemp(ElementSize);
      S.emit(NdOp::INT_SUB, NegativeCount,
             {NdVar::cst(0, ElementSize), MaskedCount});
      NdVar Complement = S.makeTemp(ElementSize);
      S.emit(NdOp::INT_AND, Complement,
             {NegativeCount, NdVar::cst(LaneBits - 1, ElementSize)});
      NdVar PrimaryPart = S.makeTemp(ElementSize);
      NdVar SecondaryPart = S.makeTemp(ElementSize);
      S.emit(IsLeft ? NdOp::INT_LEFT : NdOp::INT_RIGHT, PrimaryPart,
             {OldLane, MaskedCount});
      S.emit(IsLeft ? NdOp::INT_RIGHT : NdOp::INT_LEFT, SecondaryPart,
             {SourceLane, Complement});
      NdVar Combined = S.makeTemp(ElementSize);
      S.emit(NdOp::INT_OR, Combined, {PrimaryPart, SecondaryPart});
      NdVar CountIsZero = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, CountIsZero,
             {MaskedCount, NdVar::cst(0, ElementSize)});
      NdVar ResultLane = S.makeTemp(ElementSize);
      S.emit(NdOp::SELECT, ResultLane, {CountIsZero, OldLane, Combined});
      if (Lane == 0) {
        Raw = ResultLane;
      } else {
        NdVar Next = S.makeTemp(Raw.Size + ElementSize);
        S.emit(NdOp::CONCAT, Next, {ResultLane, Raw});
        Raw = Next;
      }
    }
    if (Info.MaskOperand) {
      if (!emitMaskedVectorResult(L, S, DestinationOperand,
                                  *Info.MaskOperand, Raw, ElementSize))
        return false;
    } else {
      S.emit(NdOp::COPY, Dst, {Raw});
    }
    break;
  }

  // VPDPBUSD{,S} / VPDPWSSD{,S} — VNNI dot product.
  case X86_INS_VPDPBUSD:
  case X86_INS_VPDPBUSDS:
  case X86_INS_VPDPWSSD:
  case X86_INS_VPDPWSSDS: {
    EvexVnniInfo Info;
    if (!validateEvexVnni(L, Insn, X86, Info))
      return false;
    const cs_x86_op &DestinationOperand = X86.operands[0];
    const cs_x86_op &FirstSourceOperand =
        X86.operands[Info.FirstSourceIndex];
    const cs_x86_op &SecondSourceOperand =
        X86.operands[Info.SecondSourceIndex];
    NdVar ActiveMask;
    if (Info.MaskOperand) {
      const RegInfo MaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
      ActiveMask = NdVar::reg(MaskInfo.Offset, Info.MaskSize);
    } else {
      ActiveMask = NdVar::cst(
          (UINT64_C(1) << Info.LaneCount) - UINT64_C(1), Info.MaskSize);
    }
    const NdVar OldDestination = L.operandRead(S, DestinationOperand);
    const NdVar FirstSource = L.operandRead(S, FirstSourceOperand);
    const NdVar SecondSource =
        Info.MemoryForm
            ? emitEvexMaskedMemoryLoad(
                  S, SecondSourceOperand, ActiveMask, Info.VectorSize, 4,
                  Info.Broadcast ? 4 : Info.VectorSize, Info.Broadcast)
            : L.operandRead(S, SecondSourceOperand);
    if (OldDestination.Size != Info.VectorSize ||
        FirstSource.Size != Info.VectorSize ||
        SecondSource.Size != Info.VectorSize)
      return false;

    const uint16_t InputElementSize = Info.Spec.WordElements ? 2 : 1;
    const unsigned InputsPerResult = Info.Spec.WordElements ? 2 : 4;
    NdVar Raw = S.makeTemp(0);
    for (unsigned Lane = 0; Lane < Info.LaneCount; ++Lane) {
      NdVar OldDword = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, OldDword,
             {OldDestination, NdVar::cst(Lane * 4, sizeof(uint32_t))});
      NdVar Sum = S.makeTemp(8);
      S.emit(NdOp::INT_SEXT, Sum, {OldDword});
      for (unsigned Element = 0; Element < InputsPerResult; ++Element) {
        const unsigned InputIndex = Lane * InputsPerResult + Element;
        const uint64_t Offset =
            static_cast<uint64_t>(InputIndex) * InputElementSize;
        NdVar FirstLane = S.makeTemp(InputElementSize);
        NdVar SecondLane = S.makeTemp(InputElementSize);
        S.emit(NdOp::SUBBYTES, FirstLane,
               {FirstSource, NdVar::cst(Offset, sizeof(uint32_t))});
        S.emit(NdOp::SUBBYTES, SecondLane,
               {SecondSource, NdVar::cst(Offset, sizeof(uint32_t))});
        NdVar WideFirst = S.makeTemp(8);
        NdVar WideSecond = S.makeTemp(8);
        S.emit(Info.Spec.WordElements ? NdOp::INT_SEXT : NdOp::INT_ZEXT,
               WideFirst,
               {FirstLane});
        S.emit(NdOp::INT_SEXT, WideSecond, {SecondLane});
        NdVar Product = S.makeTemp(8);
        S.emit(NdOp::INT_MULT, Product, {WideFirst, WideSecond});
        NdVar NextSum = S.makeTemp(8);
        S.emit(NdOp::INT_ADD, NextSum, {Sum, Product});
        Sum = NextSum;
      }

      if (Info.Spec.Saturating) {
        constexpr uint64_t SignedDwordMin = UINT64_C(0xffffffff80000000);
        constexpr uint64_t SignedDwordMax = UINT64_C(0x000000007fffffff);
        NdVar AboveMaximum = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, AboveMaximum,
               {NdVar::cst(SignedDwordMax, 8), Sum});
        NdVar HighClamped = S.makeTemp(8);
        S.emit(NdOp::SELECT, HighClamped,
               {AboveMaximum, NdVar::cst(SignedDwordMax, 8), Sum});
        NdVar BelowMinimum = S.makeTemp(1);
        S.emit(NdOp::INT_SLESS, BelowMinimum,
               {HighClamped, NdVar::cst(SignedDwordMin, 8)});
        NdVar Clamped = S.makeTemp(8);
        S.emit(NdOp::SELECT, Clamped,
               {BelowMinimum, NdVar::cst(SignedDwordMin, 8), HighClamped});
        Sum = Clamped;
      }

      NdVar ResultLane = S.makeTemp(4);
      S.emit(NdOp::SUBBYTES, ResultLane,
             {Sum, NdVar::cst(0, sizeof(uint32_t))});
      if (Lane == 0) {
        Raw = ResultLane;
      } else {
        NdVar Next = S.makeTemp(Raw.Size + 4);
        S.emit(NdOp::CONCAT, Next, {ResultLane, Raw});
        Raw = Next;
      }
    }

    if (Info.MaskOperand)
      return emitMaskedVectorResult(L, S, DestinationOperand,
                                    *Info.MaskOperand, Raw, 4);
    S.emit(NdOp::COPY, L.operandWrite(DestinationOperand), {Raw});
    return true;
  }

  // VPMADD52{H,L}UQ — packed multiply-add 52-bit unsigned.
  case X86_INS_VPMADD52HUQ:
  case X86_INS_VPMADD52LUQ: {
    EvexIfmaInfo Info;
    if (!validateEvexIfma(L, Insn, X86, Info))
      return false;
    const cs_x86_op &DestinationOperand = X86.operands[0];
    const cs_x86_op &FirstSourceOperand =
        X86.operands[Info.FirstSourceIndex];
    const cs_x86_op &SecondSourceOperand =
        X86.operands[Info.SecondSourceIndex];

    NdVar Dst = L.operandWrite(DestinationOperand);
    NdVar OldDst = L.operandRead(S, DestinationOperand);
    NdVar First = L.operandRead(S, FirstSourceOperand);
    NdVar ActiveMask = NdVar::cst(
        (UINT64_C(1) << Info.LaneCount) - UINT64_C(1), Info.MaskSize);
    if (Info.MaskOperand) {
      const RegInfo MaskInfo =
          mapCapstoneReg(static_cast<x86_reg>(Info.MaskOperand->reg));
      ActiveMask = NdVar::reg(MaskInfo.Offset, Info.MaskSize);
    }
    NdVar Second =
        Info.MemoryForm
            ? emitEvexMaskedMemoryLoad(
                  S, SecondSourceOperand, ActiveMask, Info.VectorSize, 8,
                  Info.Broadcast ? 8 : Info.VectorSize, Info.Broadcast)
            : L.operandRead(S, SecondSourceOperand);
    if (Dst.Size != Info.VectorSize || OldDst.Size != Dst.Size ||
        First.Size != Dst.Size || Second.Size != Dst.Size)
      return false;

    constexpr uint64_t OperandMask = UINT64_C(0x000fffffffffffff);
    constexpr uint64_t HalfMask = (UINT64_C(1) << 26) - 1;
    const bool HighHalf = Info.Spec.HighHalf;
    const unsigned LaneCount = Dst.Size / 8;
    NdVar Raw = S.makeTemp(0);
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      const uint64_t Offset = static_cast<uint64_t>(Lane) * 8;
      NdVar Accumulator = S.makeTemp(8);
      NdVar A = S.makeTemp(8);
      NdVar B = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, Accumulator, {OldDst, NdVar::cst(Offset, 4)});
      S.emit(NdOp::SUBBYTES, A, {First, NdVar::cst(Offset, 4)});
      S.emit(NdOp::SUBBYTES, B, {Second, NdVar::cst(Offset, 4)});
      NdVar MaskedA = S.makeTemp(8);
      NdVar MaskedB = S.makeTemp(8);
      S.emit(NdOp::INT_AND, MaskedA, {A, NdVar::cst(OperandMask, 8)});
      S.emit(NdOp::INT_AND, MaskedB, {B, NdVar::cst(OperandMask, 8)});

      NdVar Contribution;
      if (!HighHalf) {
        NdVar Product = S.makeTemp(8);
        S.emit(NdOp::INT_MULT, Product, {MaskedA, MaskedB});
        Contribution = S.makeTemp(8);
        S.emit(NdOp::INT_AND, Contribution,
               {Product, NdVar::cst(OperandMask, 8)});
      } else {
        NdVar A0 = S.makeTemp(8);
        NdVar A1 = S.makeTemp(8);
        NdVar B0 = S.makeTemp(8);
        NdVar B1 = S.makeTemp(8);
        S.emit(NdOp::INT_AND, A0, {MaskedA, NdVar::cst(HalfMask, 8)});
        S.emit(NdOp::INT_RIGHT, A1, {MaskedA, NdVar::cst(26, 8)});
        S.emit(NdOp::INT_AND, B0, {MaskedB, NdVar::cst(HalfMask, 8)});
        S.emit(NdOp::INT_RIGHT, B1, {MaskedB, NdVar::cst(26, 8)});
        NdVar Product00 = S.makeTemp(8);
        NdVar Product01 = S.makeTemp(8);
        NdVar Product10 = S.makeTemp(8);
        NdVar Product11 = S.makeTemp(8);
        S.emit(NdOp::INT_MULT, Product00, {A0, B0});
        S.emit(NdOp::INT_MULT, Product01, {A0, B1});
        S.emit(NdOp::INT_MULT, Product10, {A1, B0});
        S.emit(NdOp::INT_MULT, Product11, {A1, B1});
        NdVar Carry = S.makeTemp(8);
        S.emit(NdOp::INT_RIGHT, Carry, {Product00, NdVar::cst(26, 8)});
        NdVar Cross = S.makeTemp(8);
        S.emit(NdOp::INT_ADD, Cross, {Product01, Product10});
        NdVar CrossWithCarry = S.makeTemp(8);
        S.emit(NdOp::INT_ADD, CrossWithCarry, {Cross, Carry});
        NdVar CrossHigh = S.makeTemp(8);
        S.emit(NdOp::INT_RIGHT, CrossHigh, {CrossWithCarry, NdVar::cst(26, 8)});
        NdVar High = S.makeTemp(8);
        S.emit(NdOp::INT_ADD, High, {Product11, CrossHigh});
        Contribution = S.makeTemp(8);
        S.emit(NdOp::INT_AND, Contribution, {High, NdVar::cst(OperandMask, 8)});
      }
      NdVar ResultLane = S.makeTemp(8);
      S.emit(NdOp::INT_ADD, ResultLane, {Accumulator, Contribution});
      if (Lane == 0) {
        Raw = ResultLane;
      } else {
        NdVar Next = S.makeTemp(Raw.Size + 8);
        S.emit(NdOp::CONCAT, Next, {ResultLane, Raw});
        Raw = Next;
      }
    }
    if (Info.MaskOperand) {
      if (!emitMaskedVectorResult(L, S, DestinationOperand,
                                  *Info.MaskOperand, Raw, 8))
        return false;
    } else {
      S.emit(NdOp::COPY, Dst, {Raw});
    }
    break;
  }

  // VPSHUFBITQMB — shuffle Bits into Mask register.
  case X86_INS_VPSHUFBITQMB: {
    EvexBitShuffleInfo Info;
    if (!validateEvexBitShuffle(L, Insn, X86, Info))
      return false;
    const cs_x86_op &DataOperand = X86.operands[Info.DataIndex];
    const cs_x86_op &ControlOperand = X86.operands[Info.ControlIndex];

    NdVar Dst = L.operandWrite(X86.operands[0]);
    NdVar Data = L.operandRead(S, DataOperand);
    NdVar ActiveMask;
    if (Info.WriteMaskOperand) {
      const RegInfo MaskInfo = mapCapstoneReg(
          static_cast<x86_reg>(Info.WriteMaskOperand->reg));
      ActiveMask = NdVar::reg(MaskInfo.Offset, Info.MaskSize);
    } else {
      const uint64_t AllLanes = Info.LaneCount == 64
                                    ? UINT64_MAX
                                    : (UINT64_C(1) << Info.LaneCount) - 1;
      ActiveMask = NdVar::cst(AllLanes, Info.MaskSize);
    }
    NdVar Controls = Info.MemoryForm
                         ? emitEvexMaskedMemoryLoad(
                               S, ControlOperand, ActiveMask, Info.VectorSize,
                               1, Info.VectorSize, false)
                         : L.operandRead(S, ControlOperand);
    if (Dst.Size != Info.MaskSize || Data.Size != Info.VectorSize ||
        Controls.Size != Info.VectorSize)
      return false;
    NdVar Packed = NdVar::cst(0, Dst.Size);
    for (unsigned Byte = 0; Byte < Data.Size; ++Byte) {
      NdVar ControlByte = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, ControlByte, {Controls, NdVar::cst(Byte, 4)});
      NdVar ControlWide = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, ControlWide, {ControlByte});
      NdVar Count = S.makeTemp(8);
      S.emit(NdOp::INT_AND, Count, {ControlWide, NdVar::cst(63, 8)});
      NdVar DataQword = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, DataQword, {Data, NdVar::cst((Byte / 8) * 8, 4)});
      NdVar Shifted = S.makeTemp(8);
      S.emit(NdOp::INT_RIGHT, Shifted, {DataQword, Count});
      NdVar SelectedBitWide = S.makeTemp(8);
      S.emit(NdOp::INT_AND, SelectedBitWide, {Shifted, NdVar::cst(1, 8)});
      NdVar SelectedBitByte = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, SelectedBitByte,
             {SelectedBitWide, NdVar::cst(0, 4)});
      NdVar SelectedBit = SelectedBitByte;
      if (Dst.Size != 1) {
        SelectedBit = S.makeTemp(Dst.Size);
        S.emit(NdOp::INT_ZEXT, SelectedBit, {SelectedBitByte});
      }
      if (Byte != 0) {
        NdVar Positioned = S.makeTemp(Dst.Size);
        S.emit(NdOp::INT_LEFT, Positioned,
               {SelectedBit, NdVar::cst(Byte, Dst.Size)});
        SelectedBit = Positioned;
      }
      NdVar Next = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_OR, Next, {Packed, SelectedBit});
      Packed = Next;
    }
    if (Info.WriteMaskOperand) {
      NdVar WriteMask = L.operandRead(S, *Info.WriteMaskOperand);
      if (WriteMask.Size != Dst.Size)
        return false;
      NdVar Masked = S.makeTemp(Dst.Size);
      S.emit(NdOp::INT_AND, Masked, {Packed, WriteMask});
      Packed = Masked;
    }
    S.emit(NdOp::COPY, Dst, {Packed});
    break;
  }

  // VPERMB / VPERMW — packed byte/word permute.
  case X86_INS_VPERMB:
  case X86_INS_VPERMW: {
    EvexVbmiInfo Info;
    if (!validateEvexVbmi(L, Insn, X86, Info) ||
        Info.Spec.Kind != EvexVbmiKind::Permute)
      return false;
    const uint16_t ElementSize = Info.Spec.ElementSize;
    const cs_x86_op &DestinationOperand = X86.operands[0];
    const cs_x86_op &IndexSourceOperand =
        X86.operands[Info.FirstSourceIndex];
    const cs_x86_op &DataSourceOperand =
        X86.operands[Info.SecondSourceIndex];

    NdVar Dst = L.operandWrite(DestinationOperand);
    NdVar Indices = L.operandRead(S, IndexSourceOperand);
    NdVar Data = L.operandRead(S, DataSourceOperand);
    if (Dst.Size != Info.VectorSize || Indices.Size != Dst.Size ||
        Data.Size != Dst.Size)
      return false;
    const unsigned LaneCount = Dst.Size / ElementSize;
    NdVar Raw = S.makeTemp(0);
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      NdVar IndexLane = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, IndexLane,
             {Indices, NdVar::cst(Lane * ElementSize, 4)});
      NdVar MaskedIndex = S.makeTemp(ElementSize);
      S.emit(NdOp::INT_AND, MaskedIndex,
             {IndexLane, NdVar::cst(LaneCount - 1, ElementSize)});
      NdVar ByteOffset = MaskedIndex;
      if (ElementSize != 1) {
        ByteOffset = S.makeTemp(ElementSize);
        S.emit(NdOp::INT_MULT, ByteOffset,
               {MaskedIndex, NdVar::cst(ElementSize, ElementSize)});
      }
      NdVar ResultLane = S.makeTemp(ElementSize);
      S.emit(NdOp::SUBBYTES, ResultLane, {Data, ByteOffset});
      if (Lane == 0) {
        Raw = ResultLane;
      } else {
        NdVar Next = S.makeTemp(Raw.Size + ElementSize);
        S.emit(NdOp::CONCAT, Next, {ResultLane, Raw});
        Raw = Next;
      }
    }
    if (Info.MaskOperand) {
      if (!emitMaskedVectorResult(L, S, DestinationOperand,
                                  *Info.MaskOperand, Raw, ElementSize))
        return false;
    } else {
      S.emit(NdOp::COPY, Dst, {Raw});
    }
    break;
  }

  // VPERM{I2,T2}{B,D,PS,PD,Q,W} already handled above in permute section.

  // VPMULTISHIFTQB — multi-shift bytes from qwords.
  case X86_INS_VPMULTISHIFTQB: {
    EvexVbmiInfo Info;
    if (!validateEvexVbmi(L, Insn, X86, Info) ||
        Info.Spec.Kind != EvexVbmiKind::MultiShift)
      return false;
    const cs_x86_op &DestinationOperand = X86.operands[0];
    const cs_x86_op &ControlOperand = X86.operands[Info.FirstSourceIndex];
    const cs_x86_op &DataOperand = X86.operands[Info.SecondSourceIndex];

    NdVar Dst = L.operandWrite(DestinationOperand);
    NdVar Controls = L.operandRead(S, ControlOperand);
    NdVar Data;
    if (Info.MemoryForm && Info.Broadcast) {
      const unsigned QwordCount = Info.VectorSize / 8;
      const NdVar AllQwords =
          NdVar::cst((UINT64_C(1) << QwordCount) - UINT64_C(1), 1);
      Data = emitEvexMaskedMemoryLoad(S, DataOperand, AllQwords,
                                      Info.VectorSize, 8, 8, true);
    } else {
      Data = L.operandRead(S, DataOperand);
    }
    if (Dst.Size != Info.VectorSize || Controls.Size != Dst.Size ||
        Data.Size != Dst.Size)
      return false;
    NdVar Raw = S.makeTemp(0);
    for (unsigned Byte = 0; Byte < Dst.Size; ++Byte) {
      NdVar ControlByte = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, ControlByte, {Controls, NdVar::cst(Byte, 4)});
      NdVar ControlWide = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, ControlWide, {ControlByte});
      NdVar Count = S.makeTemp(8);
      S.emit(NdOp::INT_AND, Count, {ControlWide, NdVar::cst(63, 8)});
      NdVar NegativeCount = S.makeTemp(8);
      S.emit(NdOp::INT_SUB, NegativeCount, {NdVar::cst(0, 8), Count});
      NdVar Complement = S.makeTemp(8);
      S.emit(NdOp::INT_AND, Complement, {NegativeCount, NdVar::cst(63, 8)});
      NdVar DataQword = S.makeTemp(8);
      S.emit(NdOp::SUBBYTES, DataQword, {Data, NdVar::cst((Byte / 8) * 8, 4)});
      NdVar LowPart = S.makeTemp(8);
      NdVar HighPart = S.makeTemp(8);
      S.emit(NdOp::INT_RIGHT, LowPart, {DataQword, Count});
      S.emit(NdOp::INT_LEFT, HighPart, {DataQword, Complement});
      NdVar Rotated = S.makeTemp(8);
      S.emit(NdOp::INT_OR, Rotated, {LowPart, HighPart});
      NdVar ResultByte = S.makeTemp(1);
      S.emit(NdOp::SUBBYTES, ResultByte, {Rotated, NdVar::cst(0, 4)});
      if (Byte == 0) {
        Raw = ResultByte;
      } else {
        NdVar Next = S.makeTemp(Raw.Size + 1);
        S.emit(NdOp::CONCAT, Next, {ResultByte, Raw});
        Raw = Next;
      }
    }
    if (Info.MaskOperand) {
      if (!emitMaskedVectorResult(L, S, DestinationOperand,
                                  *Info.MaskOperand, Raw, 1))
        return false;
    } else {
      S.emit(NdOp::COPY, Dst, {Raw});
    }
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
