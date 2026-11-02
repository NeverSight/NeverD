//===- X86LegacyMemoryAudit.h - Legacy memory encoding audit --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared raw/detail agreement for legacy ModRM memory operands.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LEGACYMEMORYAUDIT_H
#define NEVERD_LIB_LIFT_X86_X86LEGACYMEMORYAUDIT_H

#include "X86LiftDetail.h"

namespace neverd::legacyaudit {

// The caller checks the opcode, prefixes, address width and ModRM byte, then
// passes the first SIB/displacement position for a memory (mod != 3) operand.
inline bool memoryOperand(const cs_insn *Insn, const cs_x86_op &D,
                          unsigned Width, unsigned AddressSize, bool Long,
                          uint8_t Rex, size_t Pos, size_t TrailingBytes = 0,
                          x86_reg Segment = X86_REG_INVALID) {
  const auto &X = Insn->detail->x86;
  const unsigned Mod = X.modrm >> 6, RM = X.modrm & 7;
  if (D.type != X86_OP_MEM || D.size != Width || D.mem.segment != Segment)
    return false;
  unsigned DispSize = Mod == 1 ? 1 : Mod == 2 ? (AddressSize == 2 ? 2 : 4) : 0;
  uint8_t Sib = 0;
  if (AddressSize == 2) {
    constexpr x86_reg Bases[] = {X86_REG_BX, X86_REG_BX, X86_REG_BP,
                                 X86_REG_BP, X86_REG_SI, X86_REG_DI,
                                 X86_REG_BP, X86_REG_BX};
    constexpr x86_reg Indices[] = {
        X86_REG_SI,      X86_REG_DI,      X86_REG_SI,      X86_REG_DI,
        X86_REG_INVALID, X86_REG_INVALID, X86_REG_INVALID, X86_REG_INVALID};
    const bool Absolute = Mod == 0 && RM == 6;
    if (Absolute)
      DispSize = 2;
    if (D.mem.base != (Absolute ? X86_REG_INVALID : Bases[RM]) ||
        D.mem.index != Indices[RM] || D.mem.scale != 1)
      return false;
  } else {
    int Base = RM + ((Rex & 1) ? 8 : 0), Index = -1;
    unsigned Scale = 1;
    bool Relative = false;
    if (RM == 4) {
      if (Pos == Insn->size)
        return false;
      Sib = Insn->bytes[Pos++];
      Scale = 1U << (Sib >> 6);
      Index = ((Sib >> 3) & 7) + ((Rex & 2) ? 8 : 0);
      if (Index == 4)
        Index = -1;
      Base = (Sib & 7) + ((Rex & 1) ? 8 : 0);
      if (Mod == 0 && (Sib & 7) == 5) {
        Base = -1;
        DispSize = 4;
      }
    } else if (Mod == 0 && RM == 5) {
      Base = -1;
      Relative = Long;
      DispSize = 4;
    }
    const auto AddressRegister = [&](x86_reg R, int Number) {
      const auto Info = mapCapstoneReg(R);
      return Info.Offset == uint64_t(Number) * 8 && Info.Size == AddressSize;
    };
    const bool BaseMatches =
        Relative ? D.mem.base == (AddressSize == 8 ? X86_REG_RIP : X86_REG_EIP)
        : Base < 0 ? D.mem.base == X86_REG_INVALID
                   : AddressRegister(D.mem.base, Base);
    if (!BaseMatches ||
        (Index < 0 ? !isNoSibIndex(D.mem.index, AddressSize)
                   : !AddressRegister(D.mem.index, Index)) ||
        D.mem.scale != int(Scale))
      return false;
  }
  if (X.sib != Sib || Pos + DispSize + TrailingBytes != Insn->size ||
      X.encoding.disp_size != DispSize ||
      X.encoding.disp_offset != (DispSize ? Pos : 0))
    return false;
  uint64_t RawDisplacement = 0;
  for (unsigned I = 0; I != DispSize; ++I)
    RawDisplacement |= uint64_t(Insn->bytes[Pos + I]) << (I * 8);
  const int64_t Displacement =
      DispSize && (RawDisplacement >> (DispSize * 8 - 1))
          ? int64_t(RawDisplacement) - (INT64_C(1) << (DispSize * 8))
          : int64_t(RawDisplacement);
  return X.disp == Displacement && D.mem.disp == Displacement;
}

} // namespace neverd::legacyaudit

#endif
