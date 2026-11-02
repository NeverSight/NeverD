//===- X86LiftAtomic.cpp - x86/x64 atomic instruction lifter ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Atomic / lock-prefix instruction handlers for x86/x64: CMPXCHG,
/// CMPXCHG8B, CMPXCHG16B, and XADD.
///
//===----------------------------------------------------------------------===//

#include "X86LiftAPXValidation.h"
#include "X86NativeMemory.h"

#include "neverd/ir/intrinsics/X86/X86AtomicMemory.h"
#include "neverd/lift/X86/X86Lifter.h"

#include "llvm/Support/Debug.h"

#include <optional>

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

namespace {

enum class ApxAtomicKind { Rao, CmpccXadd };

struct ApxAtomicEncoding {
  ApxAtomicKind Kind = ApxAtomicKind::Rao;
  uint8_t Width = 0;
  uint8_t Condition = 0;
  uint8_t SegmentPrefix = 0;
  Intrinsic Id = Intrinsic::None;
};

bool legacyRmwIsLocked(const cs_x86 &X86) {
  return X86.prefix[0] == X86_PREFIX_LOCK;
}

int apxAtomicGprIndex(x86_reg Reg, unsigned Width) {
  static const x86_reg Low32[] = {X86_REG_EAX, X86_REG_ECX, X86_REG_EDX,
                                  X86_REG_EBX, X86_REG_ESP, X86_REG_EBP,
                                  X86_REG_ESI, X86_REG_EDI};
  static const x86_reg Low64[] = {X86_REG_RAX, X86_REG_RCX, X86_REG_RDX,
                                  X86_REG_RBX, X86_REG_RSP, X86_REG_RBP,
                                  X86_REG_RSI, X86_REG_RDI};
  if (Width != 4 && Width != 8)
    return -1;
  const x86_reg *Low = Width == 4 ? Low32 : Low64;
  for (unsigned I = 0; I != 8; ++I)
    if (Reg == Low[I])
      return static_cast<int>(I);
  if (Width == 4) {
    if (Reg >= X86_REG_R8D && Reg <= X86_REG_R15D)
      return 8 + static_cast<int>(Reg - X86_REG_R8D);
    if (Reg >= X86_REG_R16D && Reg <= X86_REG_R31D)
      return 16 + static_cast<int>(Reg - X86_REG_R16D);
  } else {
    if (Reg >= X86_REG_R8 && Reg <= X86_REG_R15)
      return 8 + static_cast<int>(Reg - X86_REG_R8);
    if (Reg >= X86_REG_R16 && Reg <= X86_REG_R31)
      return 16 + static_cast<int>(Reg - X86_REG_R16);
  }
  return -1;
}

x86_reg apxAtomicSegment(uint8_t Prefix) {
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

bool apxAtomicRegisterOperand(const cs_x86_op &Operand, unsigned Number,
                              unsigned Width, uint8_t Access) {
  return Operand.type == X86_OP_REG && Operand.size == Width &&
         Operand.access == Access &&
         apxAtomicGprIndex(static_cast<x86_reg>(Operand.reg), Width) ==
             static_cast<int>(Number);
}

bool validateAtomicMemory(const cs_insn *Insn, const cs_x86 &X86,
                          size_t ModRMOffset, uint8_t SegmentPrefix,
                          unsigned AddressWidth, unsigned BaseExtension,
                          unsigned IndexExtension, uint8_t ModRM,
                          unsigned Width, bool Mode64) {
  const cs_x86_op &Operand = X86.operands[0];
  if (Operand.type != X86_OP_MEM || Operand.size != Width ||
      Operand.access != (CS_AC_READ | CS_AC_WRITE) ||
      Operand.mem.segment != apxAtomicSegment(SegmentPrefix))
    return false;

  const uint8_t Mod = ModRM >> 6;
  const uint8_t RM = ModRM & 7;
  if (Mod == 3)
    return false;
  size_t Cursor = ModRMOffset + 1;
  uint8_t DisplacementSize = 0;
  x86_reg ExpectedSpecialBase = X86_REG_INVALID;
  x86_reg ExpectedSpecialIndex = X86_REG_INVALID;
  int ExpectedBase = -1;
  int ExpectedIndex = -1;
  int ExpectedScale = 1;
  bool HasSIB = false;
  uint8_t SIB = 0;

  if (AddressWidth == 2) {
    constexpr x86_reg Bases[] = {X86_REG_BX, X86_REG_BX, X86_REG_BP,
                                 X86_REG_BP, X86_REG_SI, X86_REG_DI,
                                 X86_REG_BP, X86_REG_BX};
    constexpr x86_reg Indices[] = {
        X86_REG_SI,      X86_REG_DI,      X86_REG_SI,      X86_REG_DI,
        X86_REG_INVALID, X86_REG_INVALID, X86_REG_INVALID, X86_REG_INVALID};
    if (Mod == 0 && RM == 6)
      DisplacementSize = 2;
    else {
      ExpectedSpecialBase = Bases[RM];
      ExpectedSpecialIndex = Indices[RM];
    }
  } else if (RM == 4) {
    if (Cursor >= Insn->size)
      return false;
    HasSIB = true;
    SIB = Insn->bytes[Cursor++];
    ExpectedScale = 1u << (SIB >> 6);
    const unsigned IndexLow = (SIB >> 3) & 7;
    const unsigned BaseLow = SIB & 7;
    if (IndexLow != 4 || IndexExtension != 0)
      ExpectedIndex = static_cast<int>(IndexLow + IndexExtension);
    if (Mod == 0 && BaseLow == 5)
      DisplacementSize = 4;
    else
      ExpectedBase = static_cast<int>(BaseLow + BaseExtension);
  } else if (Mod == 0 && RM == 5) {
    if (Mode64)
      ExpectedSpecialBase = AddressWidth == 4 ? X86_REG_EIP : X86_REG_RIP;
    DisplacementSize = 4;
  } else {
    ExpectedBase = static_cast<int>(RM + BaseExtension);
  }
  if (Mod == 1)
    DisplacementSize = 1;
  else if (Mod == 2)
    DisplacementSize = AddressWidth == 2 ? 2 : 4;

  int64_t Displacement = 0;
  const size_t DisplacementOffset = Cursor;
  if (DisplacementSize == 1) {
    if (Cursor >= Insn->size)
      return false;
    Displacement = static_cast<int8_t>(Insn->bytes[Cursor++]);
  } else if (DisplacementSize == 2) {
    if (Insn->size - Cursor < 2)
      return false;
    const uint16_t Raw =
        Insn->bytes[Cursor] | (uint16_t(Insn->bytes[Cursor + 1]) << 8);
    Displacement = Mod == 0 ? int64_t(Raw) : int64_t(static_cast<int16_t>(Raw));
    Cursor += 2;
  } else if (DisplacementSize == 4) {
    if (Insn->size - Cursor < 4)
      return false;
    uint32_t Raw = 0;
    for (unsigned I = 0; I != 4; ++I)
      Raw |= static_cast<uint32_t>(Insn->bytes[Cursor + I]) << (I * 8);
    Displacement = static_cast<int32_t>(Raw);
    Cursor += 4;
  }
  if (Cursor != Insn->size || X86.encoding.disp_size != DisplacementSize ||
      X86.encoding.disp_offset !=
          (DisplacementSize != 0 ? DisplacementOffset : 0) ||
      X86.disp != Displacement || X86.sib != (HasSIB ? SIB : 0) ||
      Operand.mem.disp != Displacement || Operand.mem.scale != ExpectedScale)
    return false;

  if (HasSIB) {
    if (X86.sib_base != Operand.mem.base ||
        X86.sib_index != Operand.mem.index || X86.sib_scale != ExpectedScale)
      return false;
  } else if (X86.sib_base != X86_REG_INVALID ||
             X86.sib_index != X86_REG_INVALID || X86.sib_scale != 0) {
    return false;
  }

  if (ExpectedSpecialBase != X86_REG_INVALID) {
    if (Operand.mem.base != ExpectedSpecialBase)
      return false;
  } else if (ExpectedBase < 0) {
    if (Operand.mem.base != X86_REG_INVALID)
      return false;
  } else if (apxAtomicGprIndex(static_cast<x86_reg>(Operand.mem.base),
                               AddressWidth) != ExpectedBase) {
    return false;
  }
  if (ExpectedSpecialIndex != X86_REG_INVALID)
    return Operand.mem.index == ExpectedSpecialIndex;
  if (ExpectedIndex < 0)
    return Operand.mem.index == X86_REG_INVALID;
  return apxAtomicGprIndex(static_cast<x86_reg>(Operand.mem.index),
                           AddressWidth) == ExpectedIndex;
}

std::optional<ApxAtomicEncoding> decodeApxAtomicEncoding(const cs_insn *Insn,
                                                         const cs_x86 &X86) {
  if (!Insn || !Insn->detail || Insn->size < 6 || Insn->size > 15)
    return std::nullopt;

  const bool IsRao = Insn->id == X86_INS_AADD || Insn->id == X86_INS_AAND ||
                     Insn->id == X86_INS_AOR || Insn->id == X86_INS_AXOR;
  const bool IsCmpccXadd =
      Insn->id >= X86_INS_CMPOXADD && Insn->id <= X86_INS_CMPNLEXADD;
  if (!IsRao && !IsCmpccXadd)
    return std::nullopt;

  // The scan rejects an explicit LOCK prefix as well as legacy mandatory and
  // effective REX prefixes.  RAO/CMPccXADD already carry implicit atomicity.
  apxvalidation::EvexPrefixes Prefixes;
  if (!apxvalidation::scanEvexPrefixes(Insn, Prefixes))
    return std::nullopt;
  const size_t EvexOffset = Prefixes.EvexOffset;
  const uint8_t SegmentPrefix = Prefixes.SegmentPrefix;
  const bool Address32 = Prefixes.Address32;
  if (EvexOffset + 6 > Insn->size)
    return std::nullopt;

  const uint8_t P0 = Insn->bytes[EvexOffset + 1];
  const uint8_t P1 = Insn->bytes[EvexOffset + 2];
  const uint8_t P2 = Insn->bytes[EvexOffset + 3];
  const uint8_t Opcode = Insn->bytes[EvexOffset + 4];
  const uint8_t ModRM = Insn->bytes[EvexOffset + 5];
  if (X86.encoding.modrm_offset != EvexOffset + 5 || X86.modrm != ModRM ||
      X86.encoding.imm_offset != 0 || X86.encoding.imm_size != 0 ||
      X86.addr_size != (Address32 ? 4 : 8) || X86.prefix[0] != 0 ||
      X86.prefix[1] != SegmentPrefix || X86.prefix[2] != 0 ||
      X86.prefix[3] != (Address32 ? 0x67 : 0))
    return std::nullopt;
  for (unsigned I = 0; I != 4; ++I)
    if (X86.opcode[I] != Insn->bytes[EvexOffset + I])
      return std::nullopt;
  if ((ModRM & 0xc0) == 0xc0)
    return std::nullopt;

  const unsigned Width = (P1 & 0x80) != 0 ? 8 : 4;
  ApxAtomicEncoding Result;
  Result.Width = static_cast<uint8_t>(Width);
  if (IsRao) {
    // P1.U is the inverted high SIB-index bit for an EVEX memory operand; it
    // is not fixed to one.  Only the remaining APX MAP4.SCALABLE bits and pp
    // select are fixed here.
    if ((P0 & 7) != 4 || (P1 & 0x78) != 0x78 || P2 != 0x08 || Opcode != 0xfc ||
        X86.op_count != 2 || X86.eflags != 0 ||
        Insn->detail->regs_read_count != 0 ||
        Insn->detail->regs_write_count != 0)
      return std::nullopt;
    const uint8_t PP = P1 & 3;
    const Intrinsic ExpectedIntrinsic = PP == 0   ? Intrinsic::ApxRaoAdd
                                        : PP == 1 ? Intrinsic::ApxRaoAnd
                                        : PP == 3 ? Intrinsic::ApxRaoOr
                                                  : Intrinsic::ApxRaoXor;
    const unsigned ExpectedInstruction = PP == 0   ? X86_INS_AADD
                                         : PP == 1 ? X86_INS_AAND
                                         : PP == 3 ? X86_INS_AOR
                                                   : X86_INS_AXOR;
    const unsigned Source =
        ((~P0 & 0x80) >> 4) | (~P0 & 0x10) | ((ModRM >> 3) & 7);
    if (Insn->id != ExpectedInstruction ||
        !apxAtomicRegisterOperand(X86.operands[1], Source, Width, CS_AC_READ))
      return std::nullopt;
    Result.Kind = ApxAtomicKind::Rao;
    Result.Id = ExpectedIntrinsic;
  } else {
    constexpr uint64_t ExpectedFlags =
        X86_EFLAGS_MODIFY_OF | X86_EFLAGS_MODIFY_SF | X86_EFLAGS_MODIFY_ZF |
        X86_EFLAGS_MODIFY_AF | X86_EFLAGS_MODIFY_PF | X86_EFLAGS_MODIFY_CF;
    // As above, P1.U belongs to the memory-index encoding.  CMPccXADD fixes
    // only pp=01 in the low P1 bits.
    if ((P0 & 7) != 2 || (P1 & 3) != 1 || (P2 & 0xf7) != 0 || Opcode < 0xe0 ||
        Opcode > 0xef || X86.op_count != 3 || X86.eflags != ExpectedFlags ||
        Insn->detail->regs_read_count != 0 ||
        Insn->detail->regs_write_count != 1 ||
        Insn->detail->regs_write[0] != X86_REG_EFLAGS)
      return std::nullopt;
    const unsigned Condition = Opcode - 0xe0;
    const unsigned Compare =
        ((~P0 & 0x80) >> 4) | (~P0 & 0x10) | ((ModRM >> 3) & 7);
    const unsigned Add = ((~P2 & 0x08) << 1) | ((~P1 & 0x78) >> 3);
    if (Insn->id != X86_INS_CMPOXADD + Condition ||
        !apxAtomicRegisterOperand(X86.operands[1], Compare, Width,
                                  CS_AC_READ | CS_AC_WRITE) ||
        !apxAtomicRegisterOperand(X86.operands[2], Add, Width, CS_AC_READ))
      return std::nullopt;
    Result.Kind = ApxAtomicKind::CmpccXadd;
    Result.Condition = static_cast<uint8_t>(Condition);
    Result.Id = Intrinsic::ApxCmpccXadd;
  }

  if (!validateAtomicMemory(
          Insn, X86, EvexOffset + 5, SegmentPrefix, Address32 ? 4 : 8,
          ((~P0 & 0x20) >> 2) | ((P0 & 0x08) << 1),
          ((~P0 & 0x40) >> 3) | ((~P1 & 0x04) << 2), ModRM, Width, true))
    return std::nullopt;
  if (!x86NativeSibAddressIsValid(Arch::X64, X86, X86.operands[0]))
    return std::nullopt;
  return Result;
}

std::optional<ApxAtomicEncoding>
decodeLegacyAtomicEncoding(const cs_insn *Insn, const cs_x86 &X86, Arch A) {
  if (!Insn || !Insn->detail || Insn->size < 4 || Insn->size > 15 ||
      (A != Arch::X86 && A != Arch::X64))
    return std::nullopt;
  const bool Mode64 = A == Arch::X64;
  size_t Offset = 0;
  uint8_t Segment = 0, Mandatory = 0, Rex = 0;
  bool AddressOverride = false, Lock = false;
  unsigned Segments = 0, Addresses = 0, MandatoryCount = 0;
  while (Offset < Insn->size) {
    const auto P = Insn->bytes[Offset];
    if (Mode64 && P >= 0x40 && P <= 0x4f) {
      Rex = P;
      ++Offset;
      continue;
    }
    if (apxvalidation::isSegmentPrefix(P)) {
      Segment = P;
      ++Segments;
    } else if (P == 0x67) {
      AddressOverride = true;
      ++Addresses;
    } else if (P == 0x66 || P == 0xf2 || P == 0xf3) {
      Mandatory = P;
      ++MandatoryCount;
    } else if (P == 0xf0)
      Lock = true;
    else
      break;
    Rex = 0;
    ++Offset;
  }
  if (Offset >= Insn->size || Lock || Segments > 1 || Addresses > 1)
    return std::nullopt;
  const bool Compare = Insn->bytes[Offset] == 0xc4;
  size_t ModRMOffset;
  unsigned Width, Source, Add = 0, BaseExtension, IndexExtension;
  ApxAtomicEncoding Result;
  if (Compare) {
    if (!Mode64 || MandatoryCount || Rex || Insn->size - Offset < 5)
      return std::nullopt;
    const auto P0 = Insn->bytes[Offset + 1], P1 = Insn->bytes[Offset + 2];
    const auto Opcode = Insn->bytes[Offset + 3];
    if ((P0 & 0x1f) != 2 || (P1 & 7) != 1 || Opcode < 0xe0 || Opcode > 0xef)
      return std::nullopt;
    ModRMOffset = Offset + 4;
    Width = P1 & 0x80 ? 8 : 4;
    Source = (~P0 & 0x80) >> 4;
    Add = (~P1 & 0x78) >> 3;
    BaseExtension = (~P0 & 0x20) >> 2;
    IndexExtension = (~P0 & 0x40) >> 3;
    Result.Kind = ApxAtomicKind::CmpccXadd;
    Result.Id = Intrinsic::X86LegacyCmpccXadd;
    Result.Condition = Opcode - 0xe0;
    constexpr uint64_t Flags = X86_EFLAGS_MODIFY_OF | X86_EFLAGS_MODIFY_SF |
                               X86_EFLAGS_MODIFY_ZF | X86_EFLAGS_MODIFY_AF |
                               X86_EFLAGS_MODIFY_PF | X86_EFLAGS_MODIFY_CF;
    if (Insn->id != X86_INS_CMPOXADD + Result.Condition || X86.op_count != 3 ||
        X86.eflags != Flags || Insn->detail->regs_write_count != 1 ||
        Insn->detail->regs_write[0] != X86_REG_EFLAGS ||
        !apxAtomicRegisterOperand(X86.operands[2], Add, Width, CS_AC_READ))
      return std::nullopt;
  } else {
    if (MandatoryCount > 1 || Insn->size - Offset < 4 ||
        Insn->bytes[Offset] != 0x0f || Insn->bytes[Offset + 1] != 0x38 ||
        Insn->bytes[Offset + 2] != 0xfc)
      return std::nullopt;
    ModRMOffset = Offset + 3;
    Width = Rex & 8 ? 8 : 4;
    Source = Rex & 4 ? 8 : 0;
    BaseExtension = Rex & 1 ? 8 : 0;
    IndexExtension = Rex & 2 ? 8 : 0;
    const unsigned Expected = Mandatory == 0x66   ? X86_INS_AAND
                              : Mandatory == 0xf2 ? X86_INS_AOR
                              : Mandatory == 0xf3 ? X86_INS_AXOR
                                                  : X86_INS_AADD;
    Result.Id = Mandatory == 0x66   ? Intrinsic::X86LegacyRaoAnd
                : Mandatory == 0xf2 ? Intrinsic::X86LegacyRaoOr
                : Mandatory == 0xf3 ? Intrinsic::X86LegacyRaoXor
                                    : Intrinsic::X86LegacyRaoAdd;
    if (Insn->id != Expected || X86.op_count != 2 || X86.eflags ||
        Insn->detail->regs_write_count)
      return std::nullopt;
  }
  Result.Width = Width;
  const auto ModRM = Insn->bytes[ModRMOffset];
  Source |= (ModRM >> 3) & 7;
  const unsigned AddressWidth =
      Mode64 ? (AddressOverride ? 4 : 8) : (AddressOverride ? 2 : 4);
  if (Insn->detail->regs_read_count || X86.rex != Rex ||
      X86.addr_size != AddressWidth ||
      X86.prefix[0] != (Mandatory == 0x66 ? 0 : Mandatory) ||
      X86.prefix[1] != Segment ||
      X86.prefix[2] != (Mandatory == 0x66 ? 0x66 : 0) ||
      X86.prefix[3] != (AddressOverride ? 0x67 : 0) || X86.modrm != ModRM ||
      X86.encoding.modrm_offset != ModRMOffset || X86.encoding.imm_offset ||
      X86.encoding.imm_size ||
      !apxAtomicRegisterOperand(X86.operands[1], Source, Width,
                                Compare ? CS_AC_READ | CS_AC_WRITE
                                        : CS_AC_READ))
    return std::nullopt;
  for (unsigned I = 0; I != 4; ++I)
    if (X86.opcode[I] != (I < 3 ? Insn->bytes[Offset + I] : 0))
      return std::nullopt;
  if (!validateAtomicMemory(Insn, X86, ModRMOffset, Segment, AddressWidth,
                            BaseExtension, IndexExtension, ModRM, Width,
                            Mode64))
    return std::nullopt;
  Result.SegmentPrefix = x86NativeI386Segment(A, Segment, X86.operands[0]);
  if (!Result.SegmentPrefix &&
      !x86NativeSibAddressIsValid(A, X86, X86.operands[0]))
    return std::nullopt;
  return Result;
}

std::optional<ApxAtomicEncoding>
decodeAtomicEncoding(const cs_insn *Insn, const cs_x86 &X86, Arch A) {
  if (A == Arch::X64)
    if (auto Encoding = decodeApxAtomicEncoding(Insn, X86))
      return Encoding;
  return decodeLegacyAtomicEncoding(Insn, X86, A);
}

} // namespace

bool X86Lifter::liftAtomic(LiftState &S, const cs_insn *Insn,
                           const cs_x86 &X86) {
  unsigned InsnId = Insn->id;
  switch (InsnId) {

  case X86_INS_AADD:
  case X86_INS_AAND:
  case X86_INS_AOR:
  case X86_INS_AXOR: {
    if (TargetArch != Arch::X64 && TargetArch != Arch::X86)
      return false;
    const std::optional<ApxAtomicEncoding> Encoding =
        decodeAtomicEncoding(Insn, X86, TargetArch);
    if (!Encoding || Encoding->Kind != ApxAtomicKind::Rao)
      return false;
    const cs_x86_op &Memory = X86.operands[0];
    NdVar Address = S.computeEA(Memory, true, Encoding->SegmentPrefix != 0);
    NdVar Source = operandRead(S, X86.operands[1]);
    S.emitIntrinsic(Encoding->Id, {}, {Address, Source},
                    NdMemoryOrdering::Relaxed,
                    LiftState::memoryAddressSpace(Memory));
    if (Encoding->SegmentPrefix)
      S.Ops.back().addInput(NdVar::scalar(Encoding->SegmentPrefix, 1));
    else if (x86NativeAddressIsStack(TargetArch, X86, Memory))
      S.Ops.back().addInput(NdVar::scalar(X86AtomicStackReference, 1));
    break;
  }

  case X86_INS_CMPOXADD:
  case X86_INS_CMPNOXADD:
  case X86_INS_CMPBXADD:
  case X86_INS_CMPNBXADD:
  case X86_INS_CMPZXADD:
  case X86_INS_CMPNZXADD:
  case X86_INS_CMPBEXADD:
  case X86_INS_CMPNBEXADD:
  case X86_INS_CMPSXADD:
  case X86_INS_CMPNSXADD:
  case X86_INS_CMPPXADD:
  case X86_INS_CMPNPXADD:
  case X86_INS_CMPLXADD:
  case X86_INS_CMPNLXADD:
  case X86_INS_CMPLEXADD:
  case X86_INS_CMPNLEXADD: {
    if (TargetArch != Arch::X64)
      return false;
    const std::optional<ApxAtomicEncoding> Encoding =
        decodeAtomicEncoding(Insn, X86, TargetArch);
    if (!Encoding || Encoding->Kind != ApxAtomicKind::CmpccXadd)
      return false;
    const cs_x86_op &Memory = X86.operands[0];
    const cs_x86_op &CompareOperand = X86.operands[1];
    NdVar Address = S.computeEA(Memory);
    NdVar Add = operandRead(S, X86.operands[2]);
    NdVar Compare = operandRead(S, CompareOperand);
    NdVar Old = S.makeTemp(Encoding->Width);
    // The intrinsic owns the load and both the true/false write-back.  Its
    // output is committed only on success, so all register/flag writes below
    // remain fault-atomic even on a false condition or read-only mapping.
    S.emitIntrinsic(
        Encoding->Id, Old,
        {Address, Add, Compare, NdVar::scalar(Encoding->Condition, 1)},
        NdMemoryOrdering::SequentiallyConsistent,
        LiftState::memoryAddressSpace(Memory));
    if (x86NativeAddressIsStack(TargetArch, X86, Memory))
      S.Ops.back().addInput(NdVar::scalar(X86AtomicStackReference, 1));
    NdVar Difference = S.makeTemp(Encoding->Width);
    S.emit(NdOp::INT_SUB, Difference, {Old, Compare});
    emitFlagsArith(S, Difference, Old, Compare, true);
    NdVar CompareDestination = operandWrite(CompareOperand);
    S.emit(NdOp::COPY, CompareDestination, {Old});

    // The shared post-lift fixup already zero-extends ordinary 32-bit GPR
    // writes.  Preserve the same architectural rule for ESP/EBP, which that
    // compatibility fixup intentionally excludes.
    if (Encoding->Width == 4) {
      const RegInfo RI =
          mapCapstoneReg(static_cast<x86_reg>(CompareOperand.reg));
      if (RI.Offset == x86reg::RSP || RI.Offset == x86reg::RBP)
        S.emit(NdOp::INT_ZEXT, NdVar::reg(RI.Offset, 8),
               {NdVar::reg(RI.Offset, 4)});
    }
    break;
  }

  // CMPXCHG Dst, Src: if (RAX == Dst) Dst = Src else RAX = Dst; set ZF
  case X86_INS_CMPXCHG: {
    if (X86.op_count < 2)
      break;
    const cs_x86_op &Destination = X86.operands[0];
    NdVar Src = operandRead(S, X86.operands[1]);
    const uint16_t Sz = Destination.size;
    NdVar Rax = NdVar::reg(x86reg::RAX, Sz);

    if (Destination.type == X86_OP_MEM) {
      const NdVar Address = S.computeEA(Destination);
      const NdMemoryAddressSpace AddressSpace =
          LiftState::memoryAddressSpace(Destination);
      const bool Locked = legacyRmwIsLocked(X86);
      const NdVar Old = S.makeTemp(Sz);
      if (Locked)
        S.emit(NdOp::ATOMIC_CMPXCHG, Old, {Address, Rax, Src},
               NdMemoryOrdering::SequentiallyConsistent, AddressSpace);
      else
        S.emit(NdOp::LOAD, Old, {Address}, NdMemoryOrdering::None,
               AddressSpace);

      NdVar Equal = S.makeTemp(1);
      S.emit(NdOp::INT_EQUAL, Equal, {Rax, Old});
      if (!Locked) {
        // CMPXCHG performs a write cycle even when the comparison fails.  A
        // STORE of Old on that path preserves the permission-fault boundary
        // without falsely turning an unprefixed instruction into an LLVM
        // atomic operation.
        NdVar Stored = S.makeTemp(Sz);
        S.emit(NdOp::SELECT, Stored, {Equal, Src, Old});
        S.emit(NdOp::STORE, {}, {Address, Stored}, NdMemoryOrdering::None,
               AddressSpace);
      }

      // Every architecturally visible update follows the RMW/write cycle.  A
      // translation or permission fault therefore stops before flags or RAX
      // can leak a partially executed instruction state.
      NdVar Difference = S.makeTemp(Sz);
      S.emit(NdOp::INT_SUB, Difference, {Rax, Old});
      emitFlagsArith(S, Difference, Rax, Old, true);
      if (TargetArch == Arch::X64 && Sz == 4) {
        // On success EAX is not written and RAX's high half survives; only a
        // failed comparison performs the zero-extending 32-bit write.
        NdVar OldExtended = S.makeTemp(8);
        S.emit(NdOp::INT_ZEXT, OldExtended, {Old});
        S.emit(NdOp::SELECT, NdVar::reg(x86reg::RAX, 8),
               {Equal, NdVar::reg(x86reg::RAX, 8), OldExtended});
      } else {
        S.emit(NdOp::SELECT, Rax, {Equal, Rax, Old});
      }
      break;
    }

    NdVar DstR = operandRead(S, Destination);
    NdVar CmpTmp = S.makeTemp(Sz);
    S.emit(NdOp::INT_SUB, CmpTmp, {Rax, DstR});
    emitFlagsArith(S, CmpTmp, Rax, DstR, true);
    NdVar Eq = S.makeTemp(1);
    S.emit(NdOp::COPY, Eq, {NdVar::reg(x86reg::ZF, 1)});
    NdVar EqExt = S.makeTemp(Sz);
    S.emit(NdOp::INT_ZEXT, EqExt, {Eq});
    NdVar Mask = S.makeTemp(Sz);
    S.emit(NdOp::INT_NEG2, Mask, {EqExt});
    NdVar Inv = S.makeTemp(Sz);
    S.emit(NdOp::INT_NOT, Inv, {Mask});
    // dest = ZF ? Src : dest.
    NdVar T1 = S.makeTemp(Sz);
    NdVar T2 = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, T1, {Src, Mask});
    S.emit(NdOp::INT_AND, T2, {DstR, Inv});
    NdVar DstVal = operandWrite(Destination);
    S.emit(NdOp::INT_OR, DstVal, {T1, T2});
    NdVar R1 = S.makeTemp(Sz);
    NdVar R2 = S.makeTemp(Sz);
    S.emit(NdOp::INT_AND, R1, {Rax, Mask});
    S.emit(NdOp::INT_AND, R2, {DstR, Inv});
    S.emit(NdOp::INT_OR, Rax, {R1, R2});
    break;
  }

  // CMPXCHG8B [m64]
  case X86_INS_CMPXCHG8B: {
    if (X86.op_count < 1 || X86.operands[0].type != X86_OP_MEM ||
        X86.operands[0].size != 8)
      break;
    const cs_x86_op &Memory = X86.operands[0];
    const NdVar Address = S.computeEA(Memory);
    NdVar EdxZ = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, EdxZ, {NdVar::reg(x86reg::RDX, 4)});
    NdVar EdxSh = S.makeTemp(8);
    S.emit(NdOp::INT_LEFT, EdxSh, {EdxZ, NdVar::cst(32, 8)});
    NdVar EaxZ = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, EaxZ, {NdVar::reg(x86reg::RAX, 4)});
    NdVar DxAx = S.makeTemp(8);
    S.emit(NdOp::INT_OR, DxAx, {EdxSh, EaxZ});
    NdVar EcxZ = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, EcxZ, {NdVar::reg(x86reg::RCX, 4)});
    NdVar EcxSh = S.makeTemp(8);
    S.emit(NdOp::INT_LEFT, EcxSh, {EcxZ, NdVar::cst(32, 8)});
    NdVar EbxZ = S.makeTemp(8);
    S.emit(NdOp::INT_ZEXT, EbxZ, {NdVar::reg(x86reg::RBX, 4)});
    NdVar CxBx = S.makeTemp(8);
    S.emit(NdOp::INT_OR, CxBx, {EcxSh, EbxZ});
    const NdMemoryAddressSpace AddressSpace =
        LiftState::memoryAddressSpace(Memory);
    const bool Locked = legacyRmwIsLocked(X86);
    NdVar Old = S.makeTemp(8);
    if (Locked)
      S.emit(NdOp::ATOMIC_CMPXCHG, Old, {Address, DxAx, CxBx},
             NdMemoryOrdering::SequentiallyConsistent, AddressSpace);
    else
      S.emit(NdOp::LOAD, Old, {Address}, NdMemoryOrdering::None, AddressSpace);
    NdVar Eq = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, Eq, {Old, DxAx});
    if (!Locked) {
      NdVar Stored = S.makeTemp(8);
      S.emit(NdOp::SELECT, Stored, {Eq, CxBx, Old});
      S.emit(NdOp::STORE, {}, {Address, Stored}, NdMemoryOrdering::None,
             AddressSpace);
    }
    S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {Eq});
    NdVar MemLo = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, MemLo, {Old, NdVar::cst(0, 4)});
    NdVar MemHi = S.makeTemp(8);
    S.emit(NdOp::INT_RIGHT, MemHi, {Old, NdVar::cst(32, 8)});
    NdVar MemHiLo = S.makeTemp(4);
    S.emit(NdOp::SUBBYTES, MemHiLo, {MemHi, NdVar::cst(0, 4)});
    if (TargetArch == Arch::X64) {
      // A successful CMPXCHG8B does not write EAX/EDX, so their high halves
      // survive.  A failed comparison does write the 32-bit registers and
      // therefore zero-extends them in 64-bit mode.
      NdVar MemLoZ = S.makeTemp(8);
      NdVar MemHiZ = S.makeTemp(8);
      S.emit(NdOp::INT_ZEXT, MemLoZ, {MemLo});
      S.emit(NdOp::INT_ZEXT, MemHiZ, {MemHiLo});
      S.emit(NdOp::SELECT, NdVar::reg(x86reg::RAX, 8),
             {Eq, NdVar::reg(x86reg::RAX, 8), MemLoZ});
      S.emit(NdOp::SELECT, NdVar::reg(x86reg::RDX, 8),
             {Eq, NdVar::reg(x86reg::RDX, 8), MemHiZ});
    } else {
      S.emit(NdOp::SELECT, NdVar::reg(x86reg::RAX, 4),
             {Eq, NdVar::reg(x86reg::RAX, 4), MemLo});
      S.emit(NdOp::SELECT, NdVar::reg(x86reg::RDX, 4),
             {Eq, NdVar::reg(x86reg::RDX, 4), MemHiLo});
    }
    break;
  }

  // CMPXCHG16B [m128] (x86-64 only)
  case X86_INS_CMPXCHG16B: {
    if (X86.op_count < 1 || X86.operands[0].type != X86_OP_MEM ||
        X86.operands[0].size != 16)
      break;
    if (TargetArch != Arch::X64)
      break;
    const cs_x86_op &Memory = X86.operands[0];
    const NdVar Address = S.computeEA(Memory);
    const NdMemoryAddressSpace AddressSpace =
        LiftState::memoryAddressSpace(Memory);
    S.emitIntrinsic(Intrinsic::RequireAligned, {},
                    {Address, NdVar::scalar(16, 8)}, NdMemoryOrdering::None,
                    AddressSpace);
    NdVar Rdx = NdVar::reg(x86reg::RDX, 8);
    NdVar Rax = NdVar::reg(x86reg::RAX, 8);
    NdVar Rcx = NdVar::reg(x86reg::RCX, 8);
    NdVar Rbx = NdVar::reg(x86reg::RBX, 8);
    NdVar DxAx = S.makeTemp(16);
    S.emit(NdOp::CONCAT, DxAx, {Rdx, Rax});
    NdVar CxBx = S.makeTemp(16);
    S.emit(NdOp::CONCAT, CxBx, {Rcx, Rbx});
    const bool Locked = legacyRmwIsLocked(X86);
    NdVar Old = S.makeTemp(16);
    if (Locked)
      S.emit(NdOp::ATOMIC_CMPXCHG, Old, {Address, DxAx, CxBx},
             NdMemoryOrdering::SequentiallyConsistent, AddressSpace);
    else
      S.emit(NdOp::LOAD, Old, {Address}, NdMemoryOrdering::None, AddressSpace);
    NdVar MemLo64 = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, MemLo64, {Old, NdVar::cst(0, 8)});
    NdVar MemHi64 = S.makeTemp(8);
    S.emit(NdOp::SUBBYTES, MemHi64, {Old, NdVar::cst(8, 8)});
    NdVar EqLo = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, EqLo, {MemLo64, Rax});
    NdVar EqHi = S.makeTemp(1);
    S.emit(NdOp::INT_EQUAL, EqHi, {MemHi64, Rdx});
    NdVar Eq = S.makeTemp(1);
    S.emit(NdOp::BOOL_AND, Eq, {EqLo, EqHi});
    if (!Locked) {
      NdVar StoredLo = S.makeTemp(8);
      S.emit(NdOp::SELECT, StoredLo, {Eq, Rbx, MemLo64});
      NdVar StoredHi = S.makeTemp(8);
      S.emit(NdOp::SELECT, StoredHi, {Eq, Rcx, MemHi64});
      NdVar Stored = S.makeTemp(16);
      S.emit(NdOp::CONCAT, Stored, {StoredHi, StoredLo});
      S.emit(NdOp::STORE, {}, {Address, Stored}, NdMemoryOrdering::None,
             AddressSpace);
    }
    S.emit(NdOp::COPY, NdVar::reg(x86reg::ZF, 1), {Eq});
    S.emit(NdOp::SELECT, Rax, {Eq, Rax, MemLo64});
    S.emit(NdOp::SELECT, Rdx, {Eq, Rdx, MemHi64});
    break;
  }

  // XADD Dst, Src: Tmp = Dst + Src; Src = Dst (old); Dst = Tmp
  case X86_INS_XADD: {
    if (X86.op_count < 2)
      break;
    const cs_x86_op &Destination = X86.operands[0];
    NdVar SrcW = operandWrite(X86.operands[1]); // src operand is always a reg
    NdVar SrcR = operandRead(S, X86.operands[1]);
    if (Destination.type == X86_OP_MEM) {
      const NdVar Address = S.computeEA(Destination);
      const NdMemoryAddressSpace AddressSpace =
          LiftState::memoryAddressSpace(Destination);
      const bool Locked = legacyRmwIsLocked(X86);
      NdVar Old = S.makeTemp(Destination.size);
      if (Locked)
        S.emit(NdOp::ATOMIC_ADD, Old, {Address, SrcR},
               NdMemoryOrdering::SequentiallyConsistent, AddressSpace);
      else
        S.emit(NdOp::LOAD, Old, {Address}, NdMemoryOrdering::None,
               AddressSpace);
      NdVar Sum = S.makeTemp(Old.Size);
      S.emit(NdOp::INT_ADD, Sum, {Old, SrcR});
      if (!Locked)
        S.emit(NdOp::STORE, {}, {Address, Sum}, NdMemoryOrdering::None,
               AddressSpace);
      emitFlagsArith(S, Sum, Old, SrcR, false);
      S.emit(NdOp::COPY, SrcW, {Old});
      break;
    }

    NdVar DstR = operandRead(S, Destination);
    NdVar Sum = S.makeTemp(DstR.Size);
    S.emit(NdOp::INT_ADD, Sum, {DstR, SrcR});
    // Compute the flags from the original operands BEFORE the register writes:
    // CF/OF/AF read DstR/SrcR, which would otherwise resolve to the post-write
    // values once SrcW=DstR and DstW=Sum land (write-before-snapshot, cf.
    // #309).
    emitFlagsArith(S, Sum, DstR, SrcR, false);
    NdVar DstW = operandWrite(Destination);
    S.emit(NdOp::COPY, SrcW, {DstR});
    S.emit(NdOp::COPY, DstW, {Sum});
    break;
  }

  default:
    return false;
  }
  return true;
}

} // namespace neverd
