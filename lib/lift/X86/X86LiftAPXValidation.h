//===- X86LiftAPXValidation.h - APX promoted-form validation -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared raw-byte and Capstone-detail validation for APX promoted integer
/// instructions.  A promoted instruction is lifted only when its public
/// operands, implicit register effects, and encoding metadata agree with the
/// architectural EVEX fields.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LIFTAPXVALIDATION_H
#define NEVERD_LIB_LIFT_X86_X86LIFTAPXVALIDATION_H

#include "neverd/lift/X86Regs.h"

#include <capstone/capstone.h>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace neverd::apxvalidation {

struct Header {
  size_t EvexOffset = 0;
  uint8_t SegmentPrefix = 0;
  bool Address32 = false;
  uint8_t P0 = 0;
  uint8_t P1 = 0;
  uint8_t P2 = 0;
  uint8_t Opcode = 0;
  uint8_t ModRM = 0;
  bool Memory = false;
};

inline x86_reg segmentRegister(uint8_t Prefix) {
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

inline bool isSegmentPrefix(uint8_t Prefix) {
  return segmentRegister(Prefix) != X86_REG_INVALID;
}

inline bool isPotentialPrefix(uint8_t Prefix) {
  return isSegmentPrefix(Prefix) || Prefix == 0x67 || Prefix == 0x66 ||
         Prefix == 0xf0 || Prefix == 0xf2 || Prefix == 0xf3 ||
         (Prefix >= 0x40 && Prefix <= 0x4f);
}

inline bool isPresent(const cs_insn *Insn, const cs_x86 &X86) {
  if (!Insn)
    return false;
  if (X86.opcode[0] == 0x62)
    return true;
  size_t Offset = 0;
  while (Offset < Insn->size && isPotentialPrefix(Insn->bytes[Offset]))
    ++Offset;
  return Offset < Insn->size && Insn->bytes[Offset] == 0x62;
}

inline bool decodeHeader(const cs_insn *Insn, const cs_x86 &X86, Header &Result,
                         unsigned TrailingImmediateBytes = 0) {
  Result = {};
  if (!Insn || Insn->size == 0 || Insn->size > 15 ||
      TrailingImmediateBytes > Insn->size)
    return false;

  size_t Offset = 0;
  while (Offset < Insn->size && Insn->bytes[Offset] != 0x62) {
    const uint8_t Prefix = Insn->bytes[Offset];
    if (isSegmentPrefix(Prefix)) {
      if (Result.SegmentPrefix != 0)
        return false;
      Result.SegmentPrefix = Prefix;
    } else if (Prefix == 0x67) {
      if (Result.Address32)
        return false;
      Result.Address32 = true;
    } else {
      return false;
    }
    ++Offset;
  }
  if (Offset + 6 > Insn->size || Insn->bytes[Offset] != 0x62)
    return false;

  Result.EvexOffset = Offset;
  Result.P0 = Insn->bytes[Offset + 1];
  Result.P1 = Insn->bytes[Offset + 2];
  Result.P2 = Insn->bytes[Offset + 3];
  Result.Opcode = Insn->bytes[Offset + 4];
  Result.ModRM = Insn->bytes[Offset + 5];
  Result.Memory = (Result.ModRM & 0xc0) != 0xc0;
  const unsigned ExpectedImmediateOffset =
      TrailingImmediateBytes == 0 ? 0 : Insn->size - TrailingImmediateBytes;
  if (X86.encoding.modrm_offset != Offset + 5 || X86.modrm != Result.ModRM ||
      X86.encoding.imm_offset != ExpectedImmediateOffset ||
      X86.encoding.imm_size != TrailingImmediateBytes ||
      X86.addr_size != (Result.Address32 ? 4 : 8) || X86.prefix[0] != 0 ||
      X86.prefix[1] != Result.SegmentPrefix || X86.prefix[2] != 0 ||
      X86.prefix[3] != (Result.Address32 ? 0x67 : 0))
    return false;
  for (unsigned I = 0; I != 4; ++I)
    if (X86.opcode[I] != Insn->bytes[Offset + I])
      return false;
  return true;
}

inline uint64_t gprOffset(unsigned Number) {
  return Number < 16 ? static_cast<uint64_t>(Number) * 8
                     : x86reg::extendedGeneralReg(Number - 16);
}

inline bool registerOperand(const cs_x86_op &Operand, unsigned Number,
                            unsigned Width, uint8_t Access) {
  if (Operand.type != X86_OP_REG || Operand.size != Width ||
      Operand.access != Access || Number >= 32)
    return false;
  const RegInfo Info = mapCapstoneReg(static_cast<x86_reg>(Operand.reg));
  return Info.Offset == gprOffset(Number) && Info.Size == Width;
}

inline bool addressRegister(x86_reg Register, unsigned Number, unsigned Width) {
  if (Number >= 32)
    return false;
  const RegInfo Info = mapCapstoneReg(Register);
  return Info.Offset == gprOffset(Number) && Info.Size == Width;
}

inline unsigned modrmReg(const Header &H) {
  return ((~H.P0 & 0x80) >> 4) | (~H.P0 & 0x10) | ((H.ModRM >> 3) & 7);
}

inline unsigned modrmRm(const Header &H) {
  return ((~H.P0 & 0x20) >> 2) | ((H.P0 & 0x08) << 1) | (H.ModRM & 7);
}

inline unsigned vvvvv(const Header &H) {
  return ((~H.P2 & 0x08) << 1) | ((~H.P1 & 0x78) >> 3);
}

inline bool validateMemory(const cs_insn *Insn, const cs_x86 &X86,
                           const Header &H, const cs_x86_op &Operand,
                           unsigned Width, uint8_t Access,
                           unsigned TrailingImmediateBytes = 0) {
  if (!H.Memory || Operand.type != X86_OP_MEM || Operand.size != Width ||
      Operand.access != Access ||
      Operand.mem.segment != segmentRegister(H.SegmentPrefix))
    return false;

  const uint8_t Mod = H.ModRM >> 6;
  const uint8_t RM = H.ModRM & 7;
  const unsigned AddressWidth = H.Address32 ? 4 : 8;
  const unsigned BaseExtension = ((~H.P0 & 0x20) >> 2) | ((H.P0 & 0x08) << 1);
  const unsigned IndexExtension = ((~H.P0 & 0x40) >> 3) | ((~H.P1 & 0x04) << 2);
  size_t Cursor = H.EvexOffset + 6;
  uint8_t DisplacementSize = 0;
  x86_reg ExpectedSpecialBase = X86_REG_INVALID;
  int ExpectedBase = -1;
  int ExpectedIndex = -1;
  int ExpectedScale = 1;
  bool HasSIB = false;
  uint8_t SIB = 0;

  if (RM == 4) {
    if (Cursor >= Insn->size)
      return false;
    HasSIB = true;
    SIB = Insn->bytes[Cursor++];
    ExpectedScale = 1U << (SIB >> 6);
    const unsigned IndexLow = (SIB >> 3) & 7;
    const unsigned BaseLow = SIB & 7;
    if (IndexLow != 4 || IndexExtension != 0)
      ExpectedIndex = static_cast<int>(IndexLow + IndexExtension);
    if (Mod == 0 && BaseLow == 5)
      DisplacementSize = 4;
    else
      ExpectedBase = static_cast<int>(BaseLow + BaseExtension);
  } else if (Mod == 0 && RM == 5) {
    ExpectedSpecialBase = H.Address32 ? X86_REG_EIP : X86_REG_RIP;
    DisplacementSize = 4;
  } else {
    ExpectedBase = static_cast<int>(RM + BaseExtension);
  }
  if (Mod == 1)
    DisplacementSize = 1;
  else if (Mod == 2)
    DisplacementSize = 4;

  int64_t Displacement = 0;
  const size_t DisplacementOffset = Cursor;
  if (DisplacementSize == 1) {
    if (Cursor >= Insn->size)
      return false;
    Displacement = static_cast<int8_t>(Insn->bytes[Cursor++]);
  } else if (DisplacementSize == 4) {
    if (Insn->size - Cursor < 4)
      return false;
    uint32_t Raw = 0;
    for (unsigned I = 0; I != 4; ++I)
      Raw |= static_cast<uint32_t>(Insn->bytes[Cursor + I]) << (I * 8);
    Displacement = static_cast<int32_t>(Raw);
    Cursor += 4;
  }
  if (TrailingImmediateBytes > Insn->size ||
      Cursor != Insn->size - TrailingImmediateBytes ||
      X86.encoding.disp_size != DisplacementSize ||
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
  } else if (!addressRegister(static_cast<x86_reg>(Operand.mem.base),
                              static_cast<unsigned>(ExpectedBase),
                              AddressWidth)) {
    return false;
  }
  if (ExpectedIndex < 0)
    return Operand.mem.index == X86_REG_INVALID;
  return addressRegister(static_cast<x86_reg>(Operand.mem.index),
                         static_cast<unsigned>(ExpectedIndex), AddressWidth);
}

inline bool validateRM(const cs_insn *Insn, const cs_x86 &X86, const Header &H,
                       const cs_x86_op &Operand, unsigned Width, uint8_t Access,
                       unsigned TrailingImmediateBytes = 0) {
  if (H.Memory)
    return validateMemory(Insn, X86, H, Operand, Width, Access,
                          TrailingImmediateBytes);
  if (Insn->size != H.EvexOffset + 6 + TrailingImmediateBytes ||
      X86.encoding.disp_offset != 0 || X86.encoding.disp_size != 0 ||
      X86.disp != 0 || X86.sib != 0 || X86.sib_base != X86_REG_INVALID ||
      X86.sib_index != X86_REG_INVALID || X86.sib_scale != 0)
    return false;
  return registerOperand(Operand, modrmRm(H), Width, Access);
}

inline bool implicitDetail(const cs_insn *Insn, const cs_x86 &X86,
                           uint64_t ExpectedFlags,
                           std::initializer_list<x86_reg> Reads,
                           std::initializer_list<x86_reg> Writes) {
  if (!Insn || !Insn->detail || X86.eflags != ExpectedFlags ||
      Insn->detail->regs_read_count != Reads.size() ||
      Insn->detail->regs_write_count != Writes.size())
    return false;
  unsigned Index = 0;
  for (x86_reg Register : Reads)
    if (Insn->detail->regs_read[Index++] != Register)
      return false;
  Index = 0;
  for (x86_reg Register : Writes)
    if (Insn->detail->regs_write[Index++] != Register)
      return false;
  return true;
}

inline bool immediateOperand(const cs_insn *Insn, const cs_x86_op &Operand,
                             unsigned EncodedBytes, unsigned LogicalWidth,
                             int64_t ExpectedValue) {
  return Insn && EncodedBytes != 0 && EncodedBytes <= Insn->size &&
         Operand.type == X86_OP_IMM && Operand.size == LogicalWidth &&
         Operand.access == CS_AC_READ && Operand.imm == ExpectedValue;
}

} // namespace neverd::apxvalidation

#endif // NEVERD_LIB_LIFT_X86_X86LIFTAPXVALIDATION_H
