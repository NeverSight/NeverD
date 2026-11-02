//===- X86FPEncoding.cpp - Shared legacy/VEX FP encoding contract --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Retain raw prefix and decoder-detail agreement while selecting register
/// banks by execution mode and architectural operand role.
//===----------------------------------------------------------------------===//

#include "X86FPEncoding.h"

#include "X86LiftAPXValidation.h"

#include <algorithm>
#include <array>

namespace neverd {

bool matchesFPGPROperand(const cs_x86_op &Operand, unsigned Index,
                         unsigned Width) {
  return Operand.type == X86_OP_REG && Operand.size == Width &&
         apxvalidation::addressRegister(static_cast<x86_reg>(Operand.reg),
                                        Index, Width);
}

bool parseFPEncoding(const cs_insn *Insn, const cs_x86 &X86, Arch Target,
                     bool Vex, unsigned Map, unsigned PP, FPEncodingInfo &E) {
  E = {};
  if (!Insn || !Insn->size || Insn->size > 15 ||
      (Target != Arch::X86 && Target != Arch::X64) || Map < 1 || Map > 3 ||
      PP > 3)
    return false;
  E.Mode64 = Target == Arch::X64;
  size_t Offset = 0;
  uint8_t Rex = 0, Mandatory = 0;
  bool Address = false;
  while (Offset < Insn->size) {
    const uint8_t Byte = Insn->bytes[Offset];
    if (apxvalidation::isSegmentPrefix(Byte)) {
      if (E.Segment || Rex)
        return false;
      E.Segment = Byte;
    } else if (Byte == 0x67) {
      if (Address || Rex)
        return false;
      Address = true;
    } else if (!Vex && (Byte == 0x66 || Byte == 0xf2 || Byte == 0xf3)) {
      if (Mandatory || Rex)
        return false;
      Mandatory = Byte;
    } else if (!Vex && E.Mode64 && (Byte & 0xf0) == 0x40) {
      if (Rex)
        return false;
      Rex = Byte;
    } else
      break;
    ++Offset;
  }
  E.AddressSize = E.Mode64 ? (Address ? 4 : 8) : (Address ? 2 : 4);
  if (Offset >= Insn->size || X86.prefix[0] != 0 ||
      X86.prefix[1] != E.Segment ||
      X86.prefix[2] != (Mandatory == 0x66 ? 0x66 : 0) ||
      X86.prefix[3] != (Address ? 0x67 : 0) || X86.addr_size != E.AddressSize)
    return false;

  std::array<uint8_t, 4> OpcodeDetail{};
  uint8_t Rex2 = 0;
  if (Vex) {
    if (Insn->bytes[Offset] == 0xc4) {
      CanonicalVex3EncodingInfo V;
      if (!parseCanonicalVex3EncodingInfo(Insn, X86, Target, V) ||
          (V.P0 & 31) != Map || (V.P1 & 3) != PP || X86.rex2 != 0)
        return false;
      E.OpcodeOffset = V.Offset + 3;
      E.Opcode = V.Opcode;
      E.ModRM = V.ModRM;
      E.W = (V.P1 & 0x80) != 0;
      E.L = (V.P1 & 4) != 0;
      E.RawVvvv = (V.P1 >> 3) & 15;
      E.RegExtension = V.regIndex() & 8;
      E.BaseExtension = V.baseExtension();
      E.IndexExtension = V.indexExtension();
      return true;
    }
    if (Insn->bytes[Offset] != 0xc5 || Offset + 3 >= Insn->size || Map != 1)
      return false;
    const uint8_t P1 = Insn->bytes[Offset + 1];
    // Both high bits select VEX rather than LDS outside long mode.
    if ((!E.Mode64 && (P1 & 0xc0) != 0xc0) || (P1 & 3) != PP)
      return false;
    OpcodeDetail = {0xc5, P1, 0, 0};
    E.RegExtension = E.Mode64 && !(P1 & 0x80) ? 8 : 0;
    E.RawVvvv = (P1 >> 3) & 15;
    E.L = (P1 & 4) != 0;
    Rex = E.Mode64 ? 0x40 | (E.RegExtension ? 4 : 0) : 0;
    Offset += 2;
  } else {
    const uint8_t Expected = PP == 1   ? 0x66
                             : PP == 2 ? 0xf3
                             : PP == 3 ? 0xf2
                                       : 0;
    if (Mandatory != Expected)
      return false;
    uint8_t RegisterBits = Rex;
    if (Insn->bytes[Offset] == 0xd5) {
      if (!E.Mode64 || Rex || Offset + 3 >= Insn->size || Map != 1)
        return false;
      Rex2 = Insn->bytes[Offset + 1];
      if (!(Rex2 & 0x80))
        return false;
      RegisterBits = Rex2;
      E.RegExtension = (Rex2 & 0x40) ? 16 : 0;
      E.BaseExtension = (Rex2 & 0x10) ? 16 : 0;
      E.IndexExtension = (Rex2 & 0x20) ? 16 : 0;
      Offset += 2;
      // REX2 carries the map; an explicit 0F is never another escape.
      if (Insn->bytes[Offset] == 0x0f)
        return false;
      OpcodeDetail[0] = Insn->bytes[Offset];
    } else {
      if (Insn->bytes[Offset++] != 0x0f)
        return false;
      OpcodeDetail[0] = 0x0f;
      if (Map != 1) {
        const uint8_t Escape = Map == 2 ? 0x38 : 0x3a;
        if (Offset >= Insn->size || Insn->bytes[Offset++] != Escape)
          return false;
      }
      if (Offset >= Insn->size)
        return false;
      // Generated legacy Capstone detail retains 0F and the final opcode,
      // omitting the 38/3A escape. The bytes above authenticate that map.
      OpcodeDetail[1] = Insn->bytes[Offset];
    }
    E.W = (RegisterBits & 8) != 0;
    E.RegExtension |= (RegisterBits & 4) ? 8 : 0;
    E.BaseExtension |= (RegisterBits & 1) ? 8 : 0;
    E.IndexExtension |= (RegisterBits & 2) ? 8 : 0;
  }
  if (Offset + 1 >= Insn->size || X86.rex != Rex || X86.rex2 != Rex2 ||
      X86.encoding.modrm_offset != Offset + 1 ||
      X86.modrm != Insn->bytes[Offset + 1] ||
      !std::equal(OpcodeDetail.begin(), OpcodeDetail.end(), X86.opcode))
    return false;
  E.OpcodeOffset = Offset;
  E.Opcode = Insn->bytes[Offset];
  E.ModRM = X86.modrm;
  return true;
}

} // namespace neverd
