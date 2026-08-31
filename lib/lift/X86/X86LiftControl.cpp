//===- X86LiftControl.cpp - x86/x64 control-flow instruction lifter ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Control-flow instruction handlers for x86/x64: PUSH/POP, CALL/RET,
/// Jcc/JMP, SETcc, CMOVcc, LEAVE, and LOOP variants.
///
//===----------------------------------------------------------------------===//

#include "neverd/lift/X86Lifter.h"

#include "llvm/Support/Debug.h"

#include <array>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

enum class ApxConditionalKind { Compare, Test };

struct ApxConditionalEncoding {
  uint8_t SCC = 0;
  uint8_t DFV = 0;
  uint8_t Width = 0;
  bool Immediate = false;
  bool RMFirst = true;
};

int apxConditionalGprIndex(x86_reg Reg, unsigned Width) {
  static const x86_reg Low8[] = {X86_REG_AL,  X86_REG_CL,  X86_REG_DL,
                                 X86_REG_BL,  X86_REG_SPL, X86_REG_BPL,
                                 X86_REG_SIL, X86_REG_DIL};
  static const x86_reg Low16[] = {X86_REG_AX, X86_REG_CX, X86_REG_DX,
                                  X86_REG_BX, X86_REG_SP, X86_REG_BP,
                                  X86_REG_SI, X86_REG_DI};
  static const x86_reg Low32[] = {X86_REG_EAX, X86_REG_ECX, X86_REG_EDX,
                                  X86_REG_EBX, X86_REG_ESP, X86_REG_EBP,
                                  X86_REG_ESI, X86_REG_EDI};
  static const x86_reg Low64[] = {X86_REG_RAX, X86_REG_RCX, X86_REG_RDX,
                                  X86_REG_RBX, X86_REG_RSP, X86_REG_RBP,
                                  X86_REG_RSI, X86_REG_RDI};
  const x86_reg *Low = Width == 1   ? Low8
                       : Width == 2 ? Low16
                       : Width == 4 ? Low32
                                    : Low64;
  for (unsigned I = 0; I != 8; ++I)
    if (Reg == Low[I])
      return static_cast<int>(I);
  if (Width == 1) {
    if (Reg >= X86_REG_R8B && Reg <= X86_REG_R15B)
      return 8 + static_cast<int>(Reg - X86_REG_R8B);
    if (Reg >= X86_REG_R16B && Reg <= X86_REG_R31B)
      return 16 + static_cast<int>(Reg - X86_REG_R16B);
  } else if (Width == 2) {
    if (Reg >= X86_REG_R8W && Reg <= X86_REG_R15W)
      return 8 + static_cast<int>(Reg - X86_REG_R8W);
    if (Reg >= X86_REG_R16W && Reg <= X86_REG_R31W)
      return 16 + static_cast<int>(Reg - X86_REG_R16W);
  } else if (Width == 4) {
    if (Reg >= X86_REG_R8D && Reg <= X86_REG_R15D)
      return 8 + static_cast<int>(Reg - X86_REG_R8D);
    if (Reg >= X86_REG_R16D && Reg <= X86_REG_R31D)
      return 16 + static_cast<int>(Reg - X86_REG_R16D);
  } else if (Width == 8) {
    if (Reg >= X86_REG_R8 && Reg <= X86_REG_R15)
      return 8 + static_cast<int>(Reg - X86_REG_R8);
    if (Reg >= X86_REG_R16 && Reg <= X86_REG_R31)
      return 16 + static_cast<int>(Reg - X86_REG_R16);
  }
  return -1;
}

x86_reg apxConditionalSegment(uint8_t Prefix) {
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

std::optional<ApxConditionalEncoding>
decodeApxConditionalEncoding(const cs_insn *Insn, const cs_x86 &X86,
                             ApxConditionalKind Kind) {
  if (!Insn || Insn->size < 6 || Insn->size > 15 || X86.op_count != 2)
    return std::nullopt;

  size_t EvexOffset = 0;
  bool Address32 = false;
  uint8_t SegmentPrefix = 0;
  while (EvexOffset < Insn->size && Insn->bytes[EvexOffset] != 0x62) {
    const uint8_t Prefix = Insn->bytes[EvexOffset];
    if (Prefix == 0x67) {
      if (Address32)
        return std::nullopt;
      Address32 = true;
    } else if (apxConditionalSegment(Prefix) != X86_REG_INVALID) {
      if (SegmentPrefix != 0)
        return std::nullopt;
      SegmentPrefix = Prefix;
    } else {
      return std::nullopt;
    }
    ++EvexOffset;
  }
  if (EvexOffset + 6 > Insn->size || Insn->bytes[EvexOffset] != 0x62)
    return std::nullopt;

  const uint8_t P0 = Insn->bytes[EvexOffset + 1];
  const uint8_t P1 = Insn->bytes[EvexOffset + 2];
  const uint8_t P2 = Insn->bytes[EvexOffset + 3];
  const uint8_t Opcode = Insn->bytes[EvexOffset + 4];
  const size_t ModRMOffset = EvexOffset + 5;
  const uint8_t ModRM = Insn->bytes[ModRMOffset];
  if ((P0 & 0x07) != 0x04 || (P2 & 0xf0) != 0 ||
      X86.encoding.modrm_offset != ModRMOffset || X86.modrm != ModRM ||
      X86.addr_size != (Address32 ? 4 : 8) || X86.prefix[1] != SegmentPrefix ||
      X86.prefix[3] != (Address32 ? 0x67 : 0))
    return std::nullopt;

  bool Immediate = false;
  bool RMFirst = true;
  bool ByteForm = false;
  if (Kind == ApxConditionalKind::Test) {
    if (Opcode != 0x84 && Opcode != 0x85 && Opcode != 0xf6 && Opcode != 0xf7)
      return std::nullopt;
    Immediate = Opcode == 0xf6 || Opcode == 0xf7;
    ByteForm = Opcode == 0x84 || Opcode == 0xf6;
  } else {
    if (Opcode != 0x38 && Opcode != 0x39 && Opcode != 0x3a && Opcode != 0x3b &&
        Opcode != 0x80 && Opcode != 0x81 && Opcode != 0x83)
      return std::nullopt;
    Immediate = Opcode >= 0x80;
    RMFirst = Immediate || Opcode == 0x38 || Opcode == 0x39;
    ByteForm = Opcode == 0x38 || Opcode == 0x3a || Opcode == 0x80;
  }

  uint8_t Width = 0;
  if (ByteForm) {
    if ((P1 & 0x83) != 0)
      return std::nullopt;
    Width = 1;
  } else {
    const uint8_t PP = P1 & 0x03;
    if (PP > 1 || (PP == 1 && (P1 & 0x80) != 0))
      return std::nullopt;
    Width = PP == 1 ? 2 : (P1 & 0x80) != 0 ? 8 : 4;
  }

  const bool Memory = (ModRM & 0xc0) != 0xc0;
  const unsigned RMIndex = RMFirst ? 0 : 1;
  const unsigned OtherIndex = RMFirst ? 1 : 0;
  const unsigned ModRMGroup = (ModRM >> 3) & 7;
  if ((!Memory && (P1 & 0x04) == 0) ||
      Immediate != (X86.operands[OtherIndex].type == X86_OP_IMM) ||
      (Immediate && ((Kind == ApxConditionalKind::Compare && ModRMGroup != 7) ||
                     (Kind == ApxConditionalKind::Test && ModRMGroup > 1))) ||
      (!Immediate && X86.operands[OtherIndex].type != X86_OP_REG) ||
      (Memory && X86.operands[RMIndex].type != X86_OP_MEM) ||
      (!Memory && X86.operands[RMIndex].type != X86_OP_REG) ||
      X86.operands[RMIndex].size != Width)
    return std::nullopt;

  if (!Immediate) {
    const unsigned EncodedReg =
        ((~P0 & 0x80) >> 4) | (~P0 & 0x10) | ((ModRM >> 3) & 7);
    if (apxConditionalGprIndex(
            static_cast<x86_reg>(X86.operands[OtherIndex].reg), Width) !=
        static_cast<int>(EncodedReg))
      return std::nullopt;
  }

  if (!Memory) {
    const unsigned EncodedRM =
        ((~P0 & 0x20) >> 2) | ((P0 & 0x08) << 1) | (ModRM & 7);
    if (apxConditionalGprIndex(static_cast<x86_reg>(X86.operands[RMIndex].reg),
                               Width) != static_cast<int>(EncodedRM) ||
        X86.encoding.disp_size != 0 || X86.encoding.disp_offset != 0)
      return std::nullopt;
  }

  size_t Cursor = ModRMOffset + 1;
  uint8_t ExpectedDisplacementSize = 0;
  if (Memory) {
    const uint8_t Mod = ModRM >> 6;
    const uint8_t RM = ModRM & 7;
    const unsigned AddressWidth = Address32 ? 4 : 8;
    const unsigned BaseExtension = ((~P0 & 0x20) >> 2) | ((P0 & 0x08) << 1);
    const unsigned IndexExtension = ((~P0 & 0x40) >> 3) | ((~P1 & 0x04) << 2);
    const auto &Mem = X86.operands[RMIndex].mem;
    x86_reg ExpectedBase = X86_REG_INVALID;
    x86_reg ExpectedIndex = X86_REG_INVALID;
    unsigned ExpectedScale = 1;
    if (RM == 4) {
      if (Cursor >= Insn->size || X86.sib != Insn->bytes[Cursor])
        return std::nullopt;
      const uint8_t SIB = Insn->bytes[Cursor++];
      ExpectedScale = 1u << (SIB >> 6);
      const unsigned Index = (SIB >> 3) & 7;
      const unsigned Base = SIB & 7;
      if (Index != 4 || IndexExtension != 0) {
        ExpectedIndex = static_cast<x86_reg>(Mem.index);
        if (apxConditionalGprIndex(ExpectedIndex, AddressWidth) !=
            static_cast<int>(Index + IndexExtension))
          return std::nullopt;
      }
      if (Mod == 0 && (SIB & 7) == 5)
        ExpectedDisplacementSize = 4;
      else {
        ExpectedBase = static_cast<x86_reg>(Mem.base);
        if (apxConditionalGprIndex(ExpectedBase, AddressWidth) !=
            static_cast<int>(Base + BaseExtension))
          return std::nullopt;
      }
    } else if (Mod == 0 && RM == 5) {
      ExpectedBase = Address32 ? X86_REG_EIP : X86_REG_RIP;
      ExpectedDisplacementSize = 4;
    } else {
      ExpectedBase = static_cast<x86_reg>(Mem.base);
      if (apxConditionalGprIndex(ExpectedBase, AddressWidth) !=
          static_cast<int>(RM + BaseExtension))
        return std::nullopt;
    }
    if (Mod == 1)
      ExpectedDisplacementSize = 1;
    else if (Mod == 2)
      ExpectedDisplacementSize = 4;

    int64_t ExpectedDisplacement = 0;
    const size_t DisplacementOffset = Cursor;
    if (ExpectedDisplacementSize == 1) {
      if (Cursor >= Insn->size)
        return std::nullopt;
      ExpectedDisplacement = static_cast<int8_t>(Insn->bytes[Cursor]);
    } else if (ExpectedDisplacementSize == 4) {
      if (Cursor + 4 > Insn->size)
        return std::nullopt;
      uint32_t Raw = 0;
      for (unsigned I = 0; I != 4; ++I)
        Raw |= static_cast<uint32_t>(Insn->bytes[Cursor + I]) << (I * 8);
      ExpectedDisplacement = static_cast<int32_t>(Raw);
    }
    if (Mem.segment != apxConditionalSegment(SegmentPrefix) ||
        Mem.base != ExpectedBase || Mem.index != ExpectedIndex ||
        Mem.scale != static_cast<int>(ExpectedScale) ||
        Mem.disp != ExpectedDisplacement || X86.disp != ExpectedDisplacement ||
        X86.encoding.disp_size != ExpectedDisplacementSize ||
        X86.encoding.disp_offset !=
            (ExpectedDisplacementSize ? DisplacementOffset : 0) ||
        X86.sib_base != (RM == 4 ? ExpectedBase : X86_REG_INVALID) ||
        X86.sib_index != (RM == 4 ? ExpectedIndex : X86_REG_INVALID) ||
        X86.sib_scale != (RM == 4 ? static_cast<int>(ExpectedScale) : 0))
      return std::nullopt;
  }
  if (X86.encoding.disp_size != ExpectedDisplacementSize ||
      X86.encoding.disp_offset != (ExpectedDisplacementSize != 0 ? Cursor : 0))
    return std::nullopt;
  Cursor += ExpectedDisplacementSize;

  if (Immediate) {
    const uint8_t ImmediateSize =
        Kind == ApxConditionalKind::Compare && Opcode == 0x83 ? 1
        : Width == 1                                          ? 1
        : Width == 2                                          ? 2
                                                              : 4;
    if (X86.operands[OtherIndex].size != ImmediateSize ||
        X86.encoding.imm_offset != Cursor ||
        X86.encoding.imm_size != ImmediateSize ||
        Cursor + ImmediateSize != Insn->size)
      return std::nullopt;

    uint32_t RawImmediate = 0;
    for (unsigned I = 0; I < ImmediateSize; ++I)
      RawImmediate |= static_cast<uint32_t>(Insn->bytes[Cursor + I]) << (I * 8);
    const int64_t SignedImmediate =
        ImmediateSize == 1   ? static_cast<int8_t>(RawImmediate)
        : ImmediateSize == 2 ? static_cast<int16_t>(RawImmediate)
                             : static_cast<int32_t>(RawImmediate);
    if (X86.operands[OtherIndex].imm != SignedImmediate)
      return std::nullopt;
  } else if (X86.operands[OtherIndex].size != Width ||
             X86.encoding.imm_size != 0 || Cursor != Insn->size) {
    return std::nullopt;
  }

  const uint8_t SCC = P2 & 15;
  const unsigned BaseId =
      Kind == ApxConditionalKind::Compare ? X86_INS_CCMPO : X86_INS_CTESTO;
  if (Insn->id != BaseId + SCC)
    return std::nullopt;
  return ApxConditionalEncoding{SCC, static_cast<uint8_t>((P1 >> 3) & 15),
                                Width, Immediate, RMFirst};
}

NdVar emitCMovCondition(X86Lifter::LiftState &S, unsigned ConditionCode) {
  NdVar Condition = S.makeTemp(1);
  switch (ConditionCode) {
  case 0:
    S.emit(NdOp::COPY, Condition, {NdVar::reg(x86reg::OF, 1)});
    break;
  case 1:
    S.emit(NdOp::BOOL_NOT, Condition, {NdVar::reg(x86reg::OF, 1)});
    break;
  case 2:
    S.emit(NdOp::COPY, Condition, {NdVar::reg(x86reg::CF, 1)});
    break;
  case 3:
    S.emit(NdOp::BOOL_NOT, Condition, {NdVar::reg(x86reg::CF, 1)});
    break;
  case 4:
    S.emit(NdOp::COPY, Condition, {NdVar::reg(x86reg::ZF, 1)});
    break;
  case 5:
    S.emit(NdOp::BOOL_NOT, Condition, {NdVar::reg(x86reg::ZF, 1)});
    break;
  case 6:
    S.emit(NdOp::BOOL_OR, Condition,
           {NdVar::reg(x86reg::CF, 1), NdVar::reg(x86reg::ZF, 1)});
    break;
  case 7: {
    NdVar NotCarry = S.makeTemp(1);
    NdVar NotZero = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NotCarry, {NdVar::reg(x86reg::CF, 1)});
    S.emit(NdOp::BOOL_NOT, NotZero, {NdVar::reg(x86reg::ZF, 1)});
    S.emit(NdOp::BOOL_AND, Condition, {NotCarry, NotZero});
    break;
  }
  case 8:
    S.emit(NdOp::COPY, Condition, {NdVar::reg(x86reg::SF, 1)});
    break;
  case 9:
    S.emit(NdOp::BOOL_NOT, Condition, {NdVar::reg(x86reg::SF, 1)});
    break;
  case 10:
    S.emit(NdOp::COPY, Condition, {NdVar::reg(x86reg::PF, 1)});
    break;
  case 11:
    S.emit(NdOp::BOOL_NOT, Condition, {NdVar::reg(x86reg::PF, 1)});
    break;
  case 12:
    S.emit(NdOp::INT_NOTEQUAL, Condition,
           {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
    break;
  case 13:
    S.emit(NdOp::INT_EQUAL, Condition,
           {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
    break;
  case 14: {
    NdVar SignNotOverflow = S.makeTemp(1);
    S.emit(NdOp::INT_NOTEQUAL, SignNotOverflow,
           {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
    S.emit(NdOp::BOOL_OR, Condition,
           {NdVar::reg(x86reg::ZF, 1), SignNotOverflow});
    break;
  }
  case 15: {
    NdVar NotZero = S.makeTemp(1);
    NdVar SignEqualsOverflow = S.makeTemp(1);
    S.emit(NdOp::BOOL_NOT, NotZero, {NdVar::reg(x86reg::ZF, 1)});
    S.emit(NdOp::INT_EQUAL, SignEqualsOverflow,
           {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
    S.emit(NdOp::BOOL_AND, Condition, {NotZero, SignEqualsOverflow});
    break;
  }
  default:
    return {};
  }
  return Condition;
}

NdVar emitApxSourceCondition(X86Lifter::LiftState &S, unsigned SCC) {
  if (SCC == 10) {
    NdVar Condition = S.makeTemp(1);
    S.emit(NdOp::COPY, Condition, {NdVar::scalar(1, 1)});
    return Condition;
  }
  if (SCC == 11) {
    NdVar Condition = S.makeTemp(1);
    S.emit(NdOp::COPY, Condition, {NdVar::scalar(0, 1)});
    return Condition;
  }
  return emitCMovCondition(S, SCC);
}

void emitApxConditionalFlags(X86Lifter::LiftState &S, NdVar Condition,
                             unsigned DFV) {
  constexpr std::array<uint64_t, 6> FlagOffsets = {
      x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF,
  };
  std::array<NdVar, FlagOffsets.size()> TrueFlags;
  for (size_t I = 0; I < FlagOffsets.size(); ++I) {
    TrueFlags[I] = S.makeTemp(1);
    S.emit(NdOp::COPY, TrueFlags[I], {NdVar::reg(FlagOffsets[I], 1)});
  }

  const std::array<uint8_t, FlagOffsets.size()> FalseFlags = {
      static_cast<uint8_t>((DFV >> 0) & 1),
      static_cast<uint8_t>((DFV >> 0) & 1),
      0,
      static_cast<uint8_t>((DFV >> 1) & 1),
      static_cast<uint8_t>((DFV >> 2) & 1),
      static_cast<uint8_t>((DFV >> 3) & 1),
  };
  for (size_t I = 0; I < FlagOffsets.size(); ++I) {
    NdVar Selected = S.makeTemp(1);
    S.emit(NdOp::SELECT, Selected,
           {Condition, TrueFlags[I], NdVar::scalar(FalseFlags[I], 1)});
    S.emit(NdOp::COPY, NdVar::reg(FlagOffsets[I], 1), {Selected});
  }
}

std::optional<unsigned> ordinaryCMovConditionCode(unsigned InsnId) {
  switch (InsnId) {
  case X86_INS_CMOVO:
    return 0;
  case X86_INS_CMOVNO:
    return 1;
  case X86_INS_CMOVB:
    return 2;
  case X86_INS_CMOVAE:
    return 3;
  case X86_INS_CMOVE:
    return 4;
  case X86_INS_CMOVNE:
    return 5;
  case X86_INS_CMOVBE:
    return 6;
  case X86_INS_CMOVA:
    return 7;
  case X86_INS_CMOVS:
    return 8;
  case X86_INS_CMOVNS:
    return 9;
  case X86_INS_CMOVP:
    return 10;
  case X86_INS_CMOVNP:
    return 11;
  case X86_INS_CMOVL:
    return 12;
  case X86_INS_CMOVGE:
    return 13;
  case X86_INS_CMOVLE:
    return 14;
  case X86_INS_CMOVG:
    return 15;
  default:
    return std::nullopt;
  }
}

} // namespace

bool X86Lifter::liftControl(LiftState &S, const cs_insn *Insn,
                            const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  // --- APX conditional CMP ---
  case X86_INS_CCMPO:
  case X86_INS_CCMPNO:
  case X86_INS_CCMPB:
  case X86_INS_CCMPNB:
  case X86_INS_CCMPZ:
  case X86_INS_CCMPNZ:
  case X86_INS_CCMPBE:
  case X86_INS_CCMPNBE:
  case X86_INS_CCMPS:
  case X86_INS_CCMPNS:
  case X86_INS_CCMPT:
  case X86_INS_CCMPF:
  case X86_INS_CCMPL:
  case X86_INS_CCMPNL:
  case X86_INS_CCMPLE:
  case X86_INS_CCMPNLE: {
    if (TargetArch != Arch::X64)
      return false;
    const std::optional<ApxConditionalEncoding> Encoding =
        decodeApxConditionalEncoding(Insn, X86, ApxConditionalKind::Compare);
    if (!Encoding)
      return false;

    // A false predicate does not suppress a source load. Read both operands
    // before the first flag write so a fault leaves architectural flags intact.
    NdVar Left = operandRead(S, X86.operands[0]);
    NdVar Right = operandRead(S, X86.operands[1]);
    if (Encoding->Immediate) {
      Right.Size = Encoding->Width;
      if (Right.isConst() &&
          Right.Provenance == ConstantAddressProvenance::Unknown)
        Right.Provenance = ConstantAddressProvenance::Scalar;
    }
    if (Left.Size != Encoding->Width || Right.Size != Encoding->Width)
      return false;

    // The predicate consumes the incoming status flags, not the CMP result.
    const NdVar Condition = emitApxSourceCondition(S, Encoding->SCC);
    if (Condition.Size != 1)
      return false;

    NdVar Result = S.makeTemp(Encoding->Width);
    S.emit(NdOp::INT_SUB, Result, {Left, Right});
    emitFlagsArith(S, Result, Left, Right, /*IsSub=*/true);
    emitApxConditionalFlags(S, Condition, Encoding->DFV);
    break;
  }

  // --- APX conditional TEST ---
  case X86_INS_CTESTO:
  case X86_INS_CTESTNO:
  case X86_INS_CTESTB:
  case X86_INS_CTESTNB:
  case X86_INS_CTESTZ:
  case X86_INS_CTESTNZ:
  case X86_INS_CTESTBE:
  case X86_INS_CTESTNBE:
  case X86_INS_CTESTS:
  case X86_INS_CTESTNS:
  case X86_INS_CTESTT:
  case X86_INS_CTESTF:
  case X86_INS_CTESTL:
  case X86_INS_CTESTNL:
  case X86_INS_CTESTLE:
  case X86_INS_CTESTNLE: {
    if (TargetArch != Arch::X64)
      return false;
    const std::optional<ApxConditionalEncoding> Encoding =
        decodeApxConditionalEncoding(Insn, X86, ApxConditionalKind::Test);
    if (!Encoding)
      return false;

    // CTEST never suppresses a memory fault. Read both sources before the
    // first architectural flag write so a failing LOAD commits no status flag.
    const NdVar Left = operandRead(S, X86.operands[0]);
    NdVar Right = operandRead(S, X86.operands[1]);
    if (X86.operands[1].type == X86_OP_IMM) {
      Right.Size = Encoding->Width;
      if (Right.isConst() &&
          Right.Provenance == ConstantAddressProvenance::Unknown)
        Right.Provenance = ConstantAddressProvenance::Scalar;
    }
    if (Left.Size != Encoding->Width || Right.Size != Encoding->Width)
      return false;

    // The predicate consumes the incoming status flags, not the TEST result.
    const NdVar Condition = emitApxSourceCondition(S, Encoding->SCC);
    if (Condition.Size != 1)
      return false;

    NdVar Result = S.makeTemp(Encoding->Width);
    S.emit(NdOp::INT_AND, Result, {Left, Right});
    // Intel leaves TEST's true-path AF undefined. emitFlagsLogic preserves it,
    // which remains NeverD's deterministic project policy.
    emitFlagsLogic(S, Result);
    emitApxConditionalFlags(S, Condition, Encoding->DFV);
    break;
  }

  // --- APX paired stack operations ---
  case X86_INS_PUSH2:
  case X86_INS_PUSH2P:
  case X86_INS_POP2:
  case X86_INS_POP2P: {
    const bool IsPush = InsnId == X86_INS_PUSH2 || InsnId == X86_INS_PUSH2P;
    if (TargetArch != Arch::X64 || X86.op_count != 2 ||
        X86.operands[0].type != X86_OP_REG ||
        X86.operands[1].type != X86_OP_REG ||
        X86.operands[0].reg == X86_REG_RSP ||
        X86.operands[1].reg == X86_REG_RSP)
      return false;

    const RegInfo VInfo =
        mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
    const RegInfo BInfo =
        mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
    if (VInfo.Size != 8 || BInfo.Size != 8 ||
        (!IsPush && VInfo.Offset == BInfo.Offset))
      return false;

    const NdVar Rsp = NdVar::reg(x86reg::RSP, 8);
    const NdVar VReg = NdVar::reg(VInfo.Offset, 8);
    const NdVar BReg = NdVar::reg(BInfo.Offset, 8);
    S.emitIntrinsic(Intrinsic::RequireAligned, {}, {Rsp, NdVar::scalar(16, 8)});

    if (IsPush) {
      NdVar FullMask = S.makeTemp(16);
      S.emit(NdOp::CONCAT, FullMask,
             {NdVar::scalar(UINT64_MAX, 8), NdVar::scalar(UINT64_MAX, 8)});
      NdVar NextRsp = S.makeTemp(8);
      S.emit(NdOp::INT_SUB, NextRsp, {Rsp, NdVar::scalar(16, 8)});
      // CONCAT stores its second operand in the low qword.  APX therefore
      // places B at [RSP-16] and V at [RSP-8], matching the architectural
      // two-write order while the masked primitive stages both writes before
      // committing either one.
      NdVar Pair = S.makeTemp(16);
      S.emit(NdOp::CONCAT, Pair, {VReg, BReg});
      S.emitIntrinsic(Intrinsic::MaskedStoreQ, {}, {NextRsp, FullMask, Pair});
      S.emit(NdOp::COPY, Rsp, {NextRsp});
      break;
    }

    // Intel defines POP2 as two sequential POP operations rather than giving
    // it PUSH2's all-or-nothing guarantee.  Commit v and the first stack
    // increment before issuing the second potentially faulting load.
    NdVar VValue = S.makeTemp(8);
    S.emit(NdOp::LOAD, VValue, {Rsp});
    NdVar AfterV = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, AfterV, {Rsp, NdVar::scalar(8, 8)});
    S.emit(NdOp::COPY, VReg, {VValue});
    S.emit(NdOp::COPY, Rsp, {AfterV});

    NdVar BValue = S.makeTemp(8);
    S.emit(NdOp::LOAD, BValue, {Rsp});
    NdVar AfterB = S.makeTemp(8);
    S.emit(NdOp::INT_ADD, AfterB, {Rsp, NdVar::scalar(8, 8)});
    S.emit(NdOp::COPY, BReg, {BValue});
    S.emit(NdOp::COPY, Rsp, {AfterB});
    break;
  }

  // --- PUSH ---
  case X86_INS_PUSH:
  case X86_INS_PUSHP: {
    if (X86.op_count < 1)
      break;
    NdVar Src = operandRead(S, X86.operands[0]);
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    uint16_t PushSz = Src.Size > 0 ? Src.Size : PtrSize;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtrSize);
    S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::scalar(PushSz, PtrSize)});
    S.emit(NdOp::STORE, {}, {Rsp, Src});
    break;
  }

  // --- POP ---
  case X86_INS_POP:
  case X86_INS_POPP: {
    if (X86.op_count < 1)
      break;
    NdVar DstW = operandWrite(X86.operands[0]);
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    uint16_t PopSz = DstW.Size > 0 ? DstW.Size : PtrSize;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtrSize);
    NdVar Val = S.makeTemp(PopSz);
    S.emit(NdOp::LOAD, Val, {Rsp});
    NdVar NextRsp = S.makeTemp(PtrSize);
    S.emit(NdOp::INT_ADD, NextRsp, {Rsp, NdVar::scalar(PopSz, PtrSize)});
    S.emit(NdOp::COPY, Rsp, {NextRsp});
    const int PopCopySeq = S.Seq;
    S.emit(NdOp::COPY, DstW, {Val});
    // Record the exact ordinary POP producer for the CFG proof of an adjacent
    // `call $+5; pop reg` get-PC thunk.  Do not replace the loaded value with a
    // constant here: the POP can also be an independently reachable entry, in
    // which case it must retain the caller-provided stack value.  The CFG
    // builder grants GOTPC semantics only after proving the call is the sole
    // predecessor and that this LOAD/COPY consumes its exact stack push.
    if (GetPcArmedThisInsn) {
      LastGetPcOccurrence = I386GetPcOccurrence{
          GetPcCallAddr, S.Addr, PopCopySeq, static_cast<uint32_t>(GetPcValue),
          NdOp::COPY,    DstW,   Val};
    }
    break;
  }

  // --- CALL ---
  case X86_INS_CALL: {
    if (X86.op_count < 1)
      break;
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    if (X86.operands[0].type == X86_OP_IMM) {
      uint64_t Target = static_cast<uint64_t>(X86.operands[0].imm);
      // get-PC thunk: `call $+5` (target == the next instruction) only pushes
      // the return address so a following `pop reg` loads the current PC — the
      // i386 PIC idiom for addressing the constant pool / globals (32-bit x86
      // has no EIP-relative addressing).  Model it as a plain push of the
      // next-instruction address rather than a real call, so the popped value
      // is a known constant VA the emitter resolves to rodata.
      if (Target == S.Addr + S.InsnSize) {
        NdVar Rsp = NdVar::reg(x86reg::RSP, PtrSize);
        S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::scalar(PtrSize, PtrSize)});
        S.emit(NdOp::STORE, {}, {Rsp, NdVar::cst(Target, PtrSize)});
        GetPcPending = true;
        GetPcValue = Target;
        GetPcCallAddr = S.Addr;
      } else {
        NdVar TargetValue = NdVar::cst(Target, PtrSize);
        if (S.RelocatedImmediate) {
          TargetValue = *S.RelocatedImmediate;
          TargetValue.Size = PtrSize;
        }
        S.emit(NdOp::CALL, NdVar::reg(x86reg::RAX, PtrSize), {TargetValue});
      }
    } else if (X86.operands[0].type == X86_OP_MEM &&
               X86.operands[0].mem.base == X86_REG_RIP &&
               X86.operands[0].mem.index == X86_REG_INVALID &&
               LiftState::memoryAddressSpace(X86.operands[0]) ==
                   NdMemoryAddressSpace::Default) {
      // RIP-relative indirect call: call [rip + disp].
      // Compute the absolute address of the IAT/GOT slot. Using a
      // constant input lets MedABIPass resolve the import name.
      uint64_t SlotAddr =
          S.Addr + S.InsnSize + static_cast<uint64_t>(X86.operands[0].mem.disp);
      S.emit(NdOp::INDIR_CALL, NdVar::reg(x86reg::RAX, PtrSize),
             {NdVar::cst(SlotAddr, PtrSize)});
    } else {
      NdVar Target = operandRead(S, X86.operands[0]);
      S.emit(NdOp::INDIR_CALL, NdVar::reg(x86reg::RAX, PtrSize), {Target});
    }
    break;
  }

  // --- RET ---
  case X86_INS_RET: {
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    // `ret imm` is the i386 SysV callee-cleanup form (the callee pops imm extra
    // bytes off the caller stack — used for the hidden struct-return (sret)
    // pointer).  Record the function's largest pop so a caller adds it to its
    // post-call stack pointer; an ordinary `ret` leaves it 0.
    if (X86.op_count >= 1 && X86.operands[0].type == X86_OP_IMM) {
      int Pop = static_cast<int>(X86.operands[0].imm);
      if (Pop > FuncRetPopBytes)
        FuncRetPopBytes = Pop;
    }
    S.emit(NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, PtrSize)});
    break;
  }

  // --- Jcc (conditional jumps) ---
  case X86_INS_JE:
  case X86_INS_JNE:
  case X86_INS_JA:
  case X86_INS_JAE:
  case X86_INS_JB:
  case X86_INS_JBE:
  case X86_INS_JG:
  case X86_INS_JGE:
  case X86_INS_JL:
  case X86_INS_JLE:
  case X86_INS_JS:
  case X86_INS_JNS:
  case X86_INS_JO:
  case X86_INS_JNO:
  case X86_INS_JP:
  case X86_INS_JNP:
  case X86_INS_JCXZ:
  case X86_INS_JECXZ:
  case X86_INS_JRCXZ: {
    if (X86.op_count < 1)
      break;
    va_t Target = static_cast<uint64_t>(X86.operands[0].imm);
    NdVar Cond = S.makeTemp(1);

    switch (InsnId) {
    case X86_INS_JE:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_JNE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_JB:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::CF, 1)});
      break;
    case X86_INS_JAE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::CF, 1)});
      break;
    case X86_INS_JA: {
      NdVar NC = S.makeTemp(1);
      NdVar NZ = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(x86reg::CF, 1)});
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(x86reg::ZF, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NC, NZ});
      break;
    }
    case X86_INS_JBE: {
      S.emit(NdOp::BOOL_OR, Cond,
             {NdVar::reg(x86reg::CF, 1), NdVar::reg(x86reg::ZF, 1)});
      break;
    }
    case X86_INS_JG: {
      NdVar NZ = S.makeTemp(1);
      NdVar SfEqOf = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(x86reg::ZF, 1)});
      S.emit(NdOp::INT_EQUAL, SfEqOf,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NZ, SfEqOf});
      break;
    }
    case X86_INS_JGE:
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_JL:
      S.emit(NdOp::INT_NOTEQUAL, Cond,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_JLE: {
      NdVar SfNeOf = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, SfNeOf,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(x86reg::ZF, 1), SfNeOf});
      break;
    }
    case X86_INS_JS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::SF, 1)});
      break;
    case X86_INS_JNS:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::SF, 1)});
      break;
    case X86_INS_JO:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_JNO:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_JP:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::PF, 1)});
      break;
    case X86_INS_JNP:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::PF, 1)});
      break;
    case X86_INS_JCXZ:
    case X86_INS_JECXZ:
    case X86_INS_JRCXZ: {
      uint16_t CxSize = (InsnId == X86_INS_JCXZ)    ? 2
                        : (InsnId == X86_INS_JECXZ) ? 4
                                                    : 8;
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(x86reg::RCX, CxSize), NdVar::scalar(0, CxSize)});
      break;
    }
    default:
      break;
    }
    S.emit(NdOp::COND_BR, {}, {NdVar::cst(Target, 8), Cond});
    break;
  }

  // --- JMP ---
  case X86_INS_JMP:
  case X86_INS_JMPABS: {
    if (X86.op_count < 1)
      break;
    if (X86.operands[0].type == X86_OP_IMM) {
      S.emit(NdOp::BRANCH, {},
             {NdVar::cst(static_cast<uint64_t>(X86.operands[0].imm), 8)});
    } else if (X86.operands[0].type == X86_OP_MEM &&
               X86.operands[0].mem.base == X86_REG_RIP &&
               X86.operands[0].mem.index == X86_REG_INVALID &&
               LiftState::memoryAddressSpace(X86.operands[0]) ==
                   NdMemoryAddressSpace::Default) {
      uint64_t SlotAddr =
          S.Addr + S.InsnSize + static_cast<uint64_t>(X86.operands[0].mem.disp);
      S.emit(NdOp::INDIR_BR, {}, {NdVar::cst(SlotAddr, 8)});
    } else {
      NdVar Target = operandRead(S, X86.operands[0]);
      S.emit(NdOp::INDIR_BR, {}, {Target});
    }
    break;
  }

  // --- SETCC ---
  case X86_INS_SETE:
  case X86_INS_SETNE:
  case X86_INS_SETA:
  case X86_INS_SETAE:
  case X86_INS_SETB:
  case X86_INS_SETBE:
  case X86_INS_SETG:
  case X86_INS_SETGE:
  case X86_INS_SETL:
  case X86_INS_SETLE:
  case X86_INS_SETS:
  case X86_INS_SETNS:
  case X86_INS_SETO:
  case X86_INS_SETNO:
  case X86_INS_SETP:
  case X86_INS_SETNP:
  case X86_INS_SETZUO:
  case X86_INS_SETZUNO:
  case X86_INS_SETZUB:
  case X86_INS_SETZUAE:
  case X86_INS_SETZUE:
  case X86_INS_SETZUNE:
  case X86_INS_SETZUBE:
  case X86_INS_SETZUA:
  case X86_INS_SETZUS:
  case X86_INS_SETZUNS:
  case X86_INS_SETZUP:
  case X86_INS_SETZUNP:
  case X86_INS_SETZUL:
  case X86_INS_SETZUGE:
  case X86_INS_SETZULE:
  case X86_INS_SETZUG: {
    const bool ZeroUpper = InsnId >= X86_INS_SETZUO && InsnId <= X86_INS_SETZUG;
    unsigned ConditionInsnId = InsnId;
    switch (InsnId) {
    case X86_INS_SETZUO:
      ConditionInsnId = X86_INS_SETO;
      break;
    case X86_INS_SETZUNO:
      ConditionInsnId = X86_INS_SETNO;
      break;
    case X86_INS_SETZUB:
      ConditionInsnId = X86_INS_SETB;
      break;
    case X86_INS_SETZUAE:
      ConditionInsnId = X86_INS_SETAE;
      break;
    case X86_INS_SETZUE:
      ConditionInsnId = X86_INS_SETE;
      break;
    case X86_INS_SETZUNE:
      ConditionInsnId = X86_INS_SETNE;
      break;
    case X86_INS_SETZUBE:
      ConditionInsnId = X86_INS_SETBE;
      break;
    case X86_INS_SETZUA:
      ConditionInsnId = X86_INS_SETA;
      break;
    case X86_INS_SETZUS:
      ConditionInsnId = X86_INS_SETS;
      break;
    case X86_INS_SETZUNS:
      ConditionInsnId = X86_INS_SETNS;
      break;
    case X86_INS_SETZUP:
      ConditionInsnId = X86_INS_SETP;
      break;
    case X86_INS_SETZUNP:
      ConditionInsnId = X86_INS_SETNP;
      break;
    case X86_INS_SETZUL:
      ConditionInsnId = X86_INS_SETL;
      break;
    case X86_INS_SETZUGE:
      ConditionInsnId = X86_INS_SETGE;
      break;
    case X86_INS_SETZULE:
      ConditionInsnId = X86_INS_SETLE;
      break;
    case X86_INS_SETZUG:
      ConditionInsnId = X86_INS_SETG;
      break;
    default:
      break;
    }

    if (X86.op_count != 1 || X86.operands[0].size != 1 ||
        (X86.operands[0].type != X86_OP_REG &&
         X86.operands[0].type != X86_OP_MEM) ||
        (ZeroUpper && TargetArch != Arch::X64))
      return false;
    NdVar DstW = operandWrite(X86.operands[0]);
    NdVar Cond = S.makeTemp(1);

    switch (ConditionInsnId) {
    case X86_INS_SETE:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_SETNE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_SETB:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::CF, 1)});
      break;
    case X86_INS_SETAE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::CF, 1)});
      break;
    case X86_INS_SETS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::SF, 1)});
      break;
    case X86_INS_SETNS:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::SF, 1)});
      break;
    case X86_INS_SETO:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_SETNO:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_SETP:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::PF, 1)});
      break;
    case X86_INS_SETNP:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::PF, 1)});
      break;
    case X86_INS_SETA: {
      NdVar NC = S.makeTemp(1);
      NdVar NZ = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(x86reg::CF, 1)});
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(x86reg::ZF, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NC, NZ});
      break;
    }
    case X86_INS_SETBE:
      S.emit(NdOp::BOOL_OR, Cond,
             {NdVar::reg(x86reg::CF, 1), NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_SETG: {
      NdVar NZ = S.makeTemp(1);
      NdVar SfEqOf = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(x86reg::ZF, 1)});
      S.emit(NdOp::INT_EQUAL, SfEqOf,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NZ, SfEqOf});
      break;
    }
    case X86_INS_SETGE:
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_SETL:
      S.emit(NdOp::INT_NOTEQUAL, Cond,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_SETLE: {
      NdVar SfNeOf = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, SfNeOf,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(x86reg::ZF, 1), SfNeOf});
      break;
    }
    default:
      S.emit(NdOp::COPY, Cond, {NdVar::scalar(0, 1)});
      break;
    }

    if (X86.operands[0].type == X86_OP_MEM) {
      S.storeToMem(X86.operands[0], Cond);
    } else if (ZeroUpper) {
      // APX SETZUcc names the byte register in the encoding, but ND=1 makes
      // the architectural destination the complete 64-bit GPR.  APX only
      // exposes low-byte registers here, so a non-container offset is a
      // malformed shape and must fail before emitting a partial write.
      if (DstW.Size != 1 || DstW.Offset % 8 != 0)
        return false;
      NdVar WideCondition = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, WideCondition, {Cond});
      S.emit(NdOp::COPY, NdVar::reg(DstW.Offset, 8), {WideCondition});
    } else {
      // SETcc writes exactly ONE byte to the destination register's low byte
      // (AL/BL/.../R15B) or high byte (AH/BH/CH/DH); the enclosing 32/64-bit
      // register's OTHER bytes are PRESERVED.  Writing the byte through a
      // 32-bit INT_ZEXT (the old approach) zeroes bits 63:8 of the 64-bit
      // register (a 32-bit write zero-extends) — wrong for any caller that
      // keeps live data in the upper bytes.  A bare 1-byte register write is
      // correct on paper but lands the flag-derived value in a byte
      // sub-register that the downstream sub-register reconstruction mishandles
      // when it trails a wider write (`xor eax,eax; setcc al` — the compiler's
      // standard idiom — would drop the byte).  Instead read-modify-write the
      // FULL register: zero- extend the condition to register width (Pass 2
      // still recovers the comparison feeding this INT_ZEXT, as for the old
      // wide-ZEXT form), mask the target byte out of the old value, OR the
      // condition in, and store the whole register back.  Wide read + wide
      // write only — no trailing partial write — so upper bytes are preserved
      // without tripping the reconstruction.
      uint16_t WideSz = (TargetArch == Arch::X64) ? 8 : 4;
      uint64_t ByteOff = DstW.Offset % 8; // 0 for *L / 1 for AH/BH/CH/DH
      uint64_t EnclOff = DstW.Offset - ByteOff;
      NdVar Wide = NdVar::reg(EnclOff, WideSz);

      NdVar CondW = S.makeTemp(WideSz);
      S.emit(NdOp::INT_ZEXT, CondW, {Cond});
      if (ByteOff > 0) {
        NdVar Shifted = S.makeTemp(WideSz);
        S.emit(NdOp::INT_LEFT, Shifted,
               {CondW, NdVar::scalar(ByteOff * 8, WideSz)});
        CondW = Shifted;
      }
      uint64_t WideMask = (WideSz >= 8) ? ~0ULL : ((1ULL << (WideSz * 8)) - 1);
      uint64_t ClearMask = (~(0xFFULL << (ByteOff * 8))) & WideMask;
      NdVar Cleared = S.makeTemp(WideSz);
      S.emit(NdOp::INT_AND, Cleared, {Wide, NdVar::scalar(ClearMask, WideSz)});
      NdVar Result = S.makeTemp(WideSz);
      S.emit(NdOp::INT_OR, Result, {Cleared, CondW});
      S.emit(NdOp::COPY, Wide, {Result});
    }
    break;
  }

  // --- LEAVE ---
  case X86_INS_LEAVE: {
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtrSize);
    NdVar Rbp = NdVar::reg(x86reg::RBP, PtrSize);
    S.emit(NdOp::COPY, Rsp, {Rbp});
    NdVar Val = S.makeTemp(PtrSize);
    S.emit(NdOp::LOAD, Val, {Rsp});
    S.emit(NdOp::COPY, Rbp, {Val});
    S.emit(NdOp::INT_ADD, Rsp, {Rsp, NdVar::scalar(PtrSize, PtrSize)});
    break;
  }

  // --- CMOV ---
  case X86_INS_CFCMOVO:
  case X86_INS_CFCMOVNO:
  case X86_INS_CFCMOVB:
  case X86_INS_CFCMOVAE:
  case X86_INS_CFCMOVE:
  case X86_INS_CFCMOVNE:
  case X86_INS_CFCMOVBE:
  case X86_INS_CFCMOVA:
  case X86_INS_CFCMOVS:
  case X86_INS_CFCMOVNS:
  case X86_INS_CFCMOVP:
  case X86_INS_CFCMOVNP:
  case X86_INS_CFCMOVL:
  case X86_INS_CFCMOVGE:
  case X86_INS_CFCMOVLE:
  case X86_INS_CFCMOVG: {
    if (TargetArch != Arch::X64 || (X86.op_count != 2 && X86.op_count != 3))
      return false;

    const unsigned ConditionCode = InsnId - X86_INS_CFCMOVO;
    if (X86.operands[0].type == X86_OP_MEM) {
      if (X86.op_count != 2 || X86.operands[1].type != X86_OP_REG)
        return false;
      const RegInfo Source =
          mapCapstoneReg(static_cast<x86_reg>(X86.operands[1].reg));
      const uint16_t Size = Source.Size;
      if ((Size != 2 && Size != 4 && Size != 8) || X86.operands[0].size != Size)
        return false;

      NdVar Condition = emitCMovCondition(S, ConditionCode);
      if (Condition.Size != 1)
        return false;
      const uint64_t SignBit = UINT64_C(1) << (Size * 8 - 1);
      NdVar Mask = S.makeTemp(16);
      S.emit(NdOp::SELECT, Mask,
             {Condition, NdVar::scalar(SignBit, 16), NdVar::scalar(0, 16)});
      NdVar StoreData = S.makeTemp(16);
      S.emit(NdOp::INT_ZEXT, StoreData, {operandRead(S, X86.operands[1])});
      const Intrinsic StoreId = Size == 2   ? Intrinsic::MaskedStoreW
                                : Size == 4 ? Intrinsic::MaskedStoreD
                                            : Intrinsic::MaskedStoreQ;
      S.emitIntrinsic(StoreId, {},
                      {S.computeEA(X86.operands[0]), Mask, StoreData},
                      NdMemoryOrdering::None,
                      LiftState::memoryAddressSpace(X86.operands[0]));
      break;
    }

    if (X86.operands[0].type != X86_OP_REG ||
        (X86.op_count == 3 && X86.operands[1].type != X86_OP_REG))
      return false;

    const RegInfo Destination =
        mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
    const uint16_t Size = Destination.Size;
    const cs_x86_op &ConditionalSource = X86.operands[X86.op_count - 1];
    if ((Size != 2 && Size != 4 && Size != 8) ||
        (ConditionalSource.type != X86_OP_REG &&
         ConditionalSource.type != X86_OP_MEM) ||
        ConditionalSource.size != Size ||
        (X86.op_count == 3 && X86.operands[1].size != Size))
      return false;

    NdVar Condition = emitCMovCondition(S, ConditionCode);
    if (Condition.Size != 1)
      return false;

    NdVar ConditionalValue;
    if (ConditionalSource.type == X86_OP_MEM) {
      const uint64_t SignBit = UINT64_C(1) << (Size * 8 - 1);
      NdVar Mask = S.makeTemp(16);
      S.emit(NdOp::SELECT, Mask,
             {Condition, NdVar::scalar(SignBit, 16), NdVar::scalar(0, 16)});

      Intrinsic LoadId = Size == 2   ? Intrinsic::MaskedLoadW
                         : Size == 4 ? Intrinsic::MaskedLoadD
                                     : Intrinsic::MaskedLoadQ;
      NdVar LoadedVector = S.makeTemp(16);
      S.emitIntrinsic(LoadId, LoadedVector,
                      {S.computeEA(ConditionalSource), Mask},
                      NdMemoryOrdering::None,
                      LiftState::memoryAddressSpace(ConditionalSource));
      ConditionalValue = S.makeTemp(Size);
      S.emit(NdOp::SUBBYTES, ConditionalValue,
             {LoadedVector, NdVar::scalar(0, 4)});
    } else {
      ConditionalValue = operandRead(S, ConditionalSource);
    }

    const NdVar FalseValue = X86.op_count == 3 ? operandRead(S, X86.operands[1])
                                               : NdVar::scalar(0, Size);
    NdVar Selected = S.makeTemp(Size);
    S.emit(NdOp::SELECT, Selected, {Condition, ConditionalValue, FalseValue});

    const NdVar FullDestination = NdVar::reg(Destination.Offset, 8);
    if (Size == 8)
      S.emit(NdOp::COPY, FullDestination, {Selected});
    else
      S.emit(NdOp::INT_ZEXT, FullDestination, {Selected});
    break;
  }

  case X86_INS_CMOVE:
  case X86_INS_CMOVNE:
  case X86_INS_CMOVA:
  case X86_INS_CMOVAE:
  case X86_INS_CMOVB:
  case X86_INS_CMOVBE:
  case X86_INS_CMOVG:
  case X86_INS_CMOVGE:
  case X86_INS_CMOVL:
  case X86_INS_CMOVLE:
  case X86_INS_CMOVS:
  case X86_INS_CMOVNS:
  case X86_INS_CMOVO:
  case X86_INS_CMOVNO:
  case X86_INS_CMOVP:
  case X86_INS_CMOVNP: {
    if (X86.op_count == 3) {
      if (TargetArch != Arch::X64 || X86.operands[0].type != X86_OP_REG ||
          X86.operands[1].type != X86_OP_REG ||
          (X86.operands[2].type != X86_OP_REG &&
           X86.operands[2].type != X86_OP_MEM))
        return false;
      const RegInfo Destination =
          mapCapstoneReg(static_cast<x86_reg>(X86.operands[0].reg));
      const uint16_t Size = Destination.Size;
      if ((Size != 2 && Size != 4 && Size != 8) ||
          X86.operands[1].size != Size || X86.operands[2].size != Size)
        return false;
      const std::optional<unsigned> ConditionCode =
          ordinaryCMovConditionCode(InsnId);
      if (!ConditionCode)
        return false;

      // Unlike CFCMOV, ordinary CMOV reads r/m before testing the condition.
      // operandRead deliberately emits an unconditional LOAD for a memory
      // source so the false path retains the instruction's fault behavior.
      const NdVar ConditionalSource = operandRead(S, X86.operands[2]);
      const NdVar Fallback = operandRead(S, X86.operands[1]);
      const NdVar Condition = emitCMovCondition(S, *ConditionCode);
      NdVar Selected = S.makeTemp(Size);
      S.emit(NdOp::SELECT, Selected, {Condition, ConditionalSource, Fallback});

      const NdVar FullDestination = NdVar::reg(Destination.Offset, 8);
      if (Size == 8)
        S.emit(NdOp::COPY, FullDestination, {Selected});
      else
        S.emit(NdOp::INT_ZEXT, FullDestination, {Selected});
      break;
    }
    if (X86.op_count < 2)
      break;
    NdVar Src = operandRead(S, X86.operands[1]);
    NdVar DstR = operandRead(S, X86.operands[0]);
    NdVar Dst = operandWrite(X86.operands[0]);
    uint16_t Sz = Dst.Size > 0 ? Dst.Size : DstR.Size;

    NdVar Cond = S.makeTemp(1);
    switch (InsnId) {
    case X86_INS_CMOVE:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_CMOVNE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_CMOVB:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::CF, 1)});
      break;
    case X86_INS_CMOVAE:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::CF, 1)});
      break;
    case X86_INS_CMOVS:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::SF, 1)});
      break;
    case X86_INS_CMOVNS:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::SF, 1)});
      break;
    case X86_INS_CMOVO:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_CMOVNO:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_CMOVP:
      S.emit(NdOp::COPY, Cond, {NdVar::reg(x86reg::PF, 1)});
      break;
    case X86_INS_CMOVNP:
      S.emit(NdOp::BOOL_NOT, Cond, {NdVar::reg(x86reg::PF, 1)});
      break;
    case X86_INS_CMOVA: {
      NdVar NC = S.makeTemp(1);
      NdVar NZ = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NC, {NdVar::reg(x86reg::CF, 1)});
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(x86reg::ZF, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NC, NZ});
      break;
    }
    case X86_INS_CMOVBE:
      S.emit(NdOp::BOOL_OR, Cond,
             {NdVar::reg(x86reg::CF, 1), NdVar::reg(x86reg::ZF, 1)});
      break;
    case X86_INS_CMOVG: {
      NdVar NZ = S.makeTemp(1);
      NdVar SfEqOf = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZ, {NdVar::reg(x86reg::ZF, 1)});
      S.emit(NdOp::INT_EQUAL, SfEqOf,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      S.emit(NdOp::BOOL_AND, Cond, {NZ, SfEqOf});
      break;
    }
    case X86_INS_CMOVGE:
      S.emit(NdOp::INT_EQUAL, Cond,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_CMOVL:
      S.emit(NdOp::INT_NOTEQUAL, Cond,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      break;
    case X86_INS_CMOVLE: {
      NdVar SfNeOf = S.makeTemp(1);
      S.emit(NdOp::INT_NOTEQUAL, SfNeOf,
             {NdVar::reg(x86reg::SF, 1), NdVar::reg(x86reg::OF, 1)});
      S.emit(NdOp::BOOL_OR, Cond, {NdVar::reg(x86reg::ZF, 1), SfNeOf});
      break;
    }
    default:
      S.emit(NdOp::COPY, Cond, {NdVar::scalar(1, 1)});
      break;
    }
    NdVar CondExt = S.makeTemp(Sz);
    S.emit(NdOp::INT_ZEXT, CondExt, {Cond});
    NdVar Mask = S.makeTemp(Sz);
    S.emit(NdOp::INT_NEG2, Mask, {CondExt});
    NdVar InvMask = S.makeTemp(Sz);
    S.emit(NdOp::INT_NOT, InvMask, {Mask});
    NdVar T1 = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, T1, {Src, Mask});
    NdVar T2 = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, T2, {DstR, InvMask});
    S.emit(NdOp::INT_OR, Dst, {T1, T2});
    break;
  }

  // --- LOOP / LOOPE / LOOPNE ---
  case X86_INS_LOOP:
  case X86_INS_LOOPE:
  case X86_INS_LOOPNE: {
    if (X86.op_count < 1)
      break;
    uint16_t CxSize = (TargetArch == Arch::X64) ? 8 : 4;
    NdVar Rcx = NdVar::reg(x86reg::RCX, CxSize);
    S.emit(NdOp::INT_SUB, Rcx, {Rcx, NdVar::scalar(1, CxSize)});
    NdVar NZ = S.makeTemp(1);
    S.emit(NdOp::INT_NOTEQUAL, NZ, {Rcx, NdVar::scalar(0, CxSize)});
    NdVar Cond = NZ;
    if (InsnId == X86_INS_LOOPE) {
      NdVar Merged = S.makeTemp(1);
      S.emit(NdOp::BOOL_AND, Merged, {NZ, NdVar::reg(x86reg::ZF, 1)});
      Cond = Merged;
    } else if (InsnId == X86_INS_LOOPNE) {
      NdVar NZF = S.makeTemp(1);
      S.emit(NdOp::BOOL_NOT, NZF, {NdVar::reg(x86reg::ZF, 1)});
      NdVar Merged = S.makeTemp(1);
      S.emit(NdOp::BOOL_AND, Merged, {NZ, NZF});
      Cond = Merged;
    }
    S.emit(NdOp::COND_BR, {},
           {NdVar::cst(static_cast<uint64_t>(X86.operands[0].imm), 8), Cond});
    break;
  }

  // --- PUSHA / PUSHAD ---
  case X86_INS_PUSHAW:
  case X86_INS_PUSHAL: {
    uint16_t PtSz = (InsnId == X86_INS_PUSHAL) ? 4 : 2;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtSz);
    for (uint64_t Reg : {x86reg::RAX, x86reg::RCX, x86reg::RDX, x86reg::RBX,
                         x86reg::RSP, x86reg::RBP, x86reg::RSI, x86reg::RDI}) {
      S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::scalar(PtSz, PtSz)});
      S.emit(NdOp::STORE, {}, {Rsp, NdVar::reg(Reg, PtSz)});
    }
    break;
  }

  // --- POPA / POPAD ---
  case X86_INS_POPAW:
  case X86_INS_POPAL: {
    uint16_t PtSz = (InsnId == X86_INS_POPAL) ? 4 : 2;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtSz);
    for (uint64_t Reg : {x86reg::RDI, x86reg::RSI, x86reg::RBP, x86reg::RSP,
                         x86reg::RBX, x86reg::RDX, x86reg::RCX, x86reg::RAX}) {
      NdVar Val = S.makeTemp(PtSz);
      S.emit(NdOp::LOAD, Val, {Rsp});
      if (Reg != x86reg::RSP)
        S.emit(NdOp::COPY, NdVar::reg(Reg, PtSz), {Val});
      S.emit(NdOp::INT_ADD, Rsp, {Rsp, NdVar::scalar(PtSz, PtSz)});
    }
    break;
  }

  // --- LCALL (far call) ---
  case X86_INS_LCALL: {
    if (X86.op_count < 1)
      break;
    NdVar Target = operandRead(S, X86.operands[0]);
    S.emit(NdOp::INDIR_CALL, NdVar::reg(x86reg::RAX, 4), {Target});
    break;
  }

  // --- LJMP (far jump) ---
  case X86_INS_LJMP: {
    if (X86.op_count < 1)
      break;
    if (X86.operands[0].type == X86_OP_IMM) {
      S.emit(NdOp::BRANCH, {},
             {NdVar::cst(static_cast<uint64_t>(X86.operands[0].imm), 8)});
    } else {
      NdVar Target = operandRead(S, X86.operands[0]);
      S.emit(NdOp::INDIR_BR, {}, {Target});
    }
    break;
  }

  // --- ENTER imm16, imm8 ---
  // imm8 is the lexical nesting level (taken mod 32).  Intel SDM:
  //   Push(RBP); FrameTemp = RSP
  //   IF level > 0:
  //     FOR i = 1 TO level-1:  RBP -= PtrSize; Push([RBP])   // copy display
  //     Push(FrameTemp)
  //   RBP = FrameTemp;  RSP -= imm16
  // The nesting level was previously ignored, which dropped the Push(FrameTemp)
  // (and, for level >= 2, the display copies) for nested frames.
  case X86_INS_ENTER: {
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtrSize);
    NdVar Rbp = NdVar::reg(x86reg::RBP, PtrSize);
    uint64_t AllocSz =
        (X86.op_count >= 1) ? static_cast<uint64_t>(X86.operands[0].imm) : 0;
    uint64_t Level = (X86.op_count >= 2)
                         ? (static_cast<uint64_t>(X86.operands[1].imm) % 32)
                         : 0;
    // Push(RBP)
    S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::scalar(PtrSize, PtrSize)});
    S.emit(NdOp::STORE, {}, {Rsp, Rbp});
    // FrameTemp = RSP
    NdVar FrameTemp = S.makeTemp(PtrSize);
    S.emit(NdOp::COPY, FrameTemp, {Rsp});
    if (Level > 0) {
      // Display copy: FOR i = 1 TO level-1: RBP -= PtrSize; Push([RBP]).
      for (uint64_t I = 1; I < Level; ++I) {
        S.emit(NdOp::INT_SUB, Rbp, {Rbp, NdVar::scalar(PtrSize, PtrSize)});
        NdVar Disp = S.makeTemp(PtrSize);
        S.emit(NdOp::LOAD, Disp, {Rbp});
        S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::scalar(PtrSize, PtrSize)});
        S.emit(NdOp::STORE, {}, {Rsp, Disp});
      }
      // Push(FrameTemp)
      S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::scalar(PtrSize, PtrSize)});
      S.emit(NdOp::STORE, {}, {Rsp, FrameTemp});
    }
    // RBP = FrameTemp
    S.emit(NdOp::COPY, Rbp, {FrameTemp});
    // RSP -= imm16
    if (AllocSz)
      S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::scalar(AllocSz, PtrSize)});
    break;
  }

  // --- RETF / IRET ---
  case X86_INS_RETF:
  case X86_INS_RETFQ:
  case X86_INS_IRET:
  case X86_INS_IRETD:
  case X86_INS_IRETQ: {
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    S.emit(NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, PtrSize)});
    break;
  }

  // --- PUSHF / POPF ---
  case X86_INS_PUSHF:
  case X86_INS_PUSHFD:
  case X86_INS_PUSHFQ:
  case X86_INS_POPF:
  case X86_INS_POPFD:
  case X86_INS_POPFQ: {
    uint16_t PtrSize = (TargetArch == Arch::X64) ? 8 : 4;
    NdVar Rsp = NdVar::reg(x86reg::RSP, PtrSize);
    bool Push = (InsnId == X86_INS_PUSHF || InsnId == X86_INS_PUSHFD ||
                 InsnId == X86_INS_PUSHFQ);
    // Modelled EFLAGS bits at their architectural positions.  System flags
    // (TF/IF/IOPL/...) are not modelled; the previous code pushed/popped an
    // uninitialised temp, so flags were never actually (de)serialised.
    const std::pair<uint64_t, unsigned> FlagBits[] = {
        {x86reg::CF, 0}, {x86reg::PF, 2},  {x86reg::AF, 4}, {x86reg::ZF, 6},
        {x86reg::SF, 7}, {x86reg::DF, 10}, {x86reg::OF, 11}};
    if (Push) {
      // Assemble EFLAGS from the modelled flags (reserved bit 1 reads as 1).
      NdVar Eflags = S.makeTemp(PtrSize);
      S.emit(NdOp::COPY, Eflags, {NdVar::scalar(0x2, PtrSize)});
      for (auto [Fl, Bit] : FlagBits) {
        NdVar Z = S.makeTemp(PtrSize);
        S.emit(NdOp::INT_ZEXT, Z, {NdVar::reg(Fl, 1)});
        NdVar Sh = S.makeTemp(PtrSize);
        S.emit(NdOp::INT_LEFT, Sh, {Z, NdVar::scalar(Bit, PtrSize)});
        NdVar Next = S.makeTemp(PtrSize);
        S.emit(NdOp::INT_OR, Next, {Eflags, Sh});
        Eflags = Next;
      }
      S.emit(NdOp::INT_SUB, Rsp, {Rsp, NdVar::scalar(PtrSize, PtrSize)});
      S.emit(NdOp::STORE, {}, {Rsp, Eflags});
    } else {
      // Load EFLAGS and scatter the modelled bits back into the flag registers.
      NdVar Val = S.makeTemp(PtrSize);
      S.emit(NdOp::LOAD, Val, {Rsp});
      S.emit(NdOp::INT_ADD, Rsp, {Rsp, NdVar::scalar(PtrSize, PtrSize)});
      for (auto [Fl, Bit] : FlagBits) {
        NdVar Sh = S.makeTemp(PtrSize);
        S.emit(NdOp::INT_RIGHT, Sh, {Val, NdVar::scalar(Bit, PtrSize)});
        NdVar Bitv = S.makeTemp(PtrSize);
        S.emit(NdOp::INT_AND, Bitv, {Sh, NdVar::scalar(1, PtrSize)});
        S.emit(NdOp::INT_NOTEQUAL, NdVar::reg(Fl, 1),
               {Bitv, NdVar::scalar(0, PtrSize)});
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
