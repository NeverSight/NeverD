//===- X86LegacySegmentEncoding.h - Legacy selector grammar ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate selector transfers and their 16/32-bit effective addresses.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_TRANSLATE_X86_X86LEGACYSEGMENTENCODING_H
#define NEVERD_LIB_TRANSLATE_X86_X86LEGACYSEGMENTENCODING_H

#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace neverd::translate::detail {

struct X86LegacySegmentInstruction {
  X86SegmentRegister Segment = X86SegmentRegister::ES;
  X86SegmentRegister MemorySegment = X86SegmentRegister::DS;
  std::optional<unsigned> Register, Base, Index;
  uint32_t Displacement = 0;
  unsigned Bytes = 4, AddressBytes = 4, Scale = 1;
  bool Read = false, Push = false, Pop = false, Far = false;
};

inline std::optional<X86LegacySegmentInstruction>
decodeLegacySegmentInstruction(llvm::ArrayRef<uint8_t> Bytes, bool Code32) {
  if (Bytes.empty() || Bytes.size() > 15)
    return std::nullopt;
  X86LegacySegmentInstruction F;
  bool OperandOverride = false, AddressOverride = false;
  std::optional<X86SegmentRegister> Override;
  size_t P = 0;
  while (P < Bytes.size()) {
    const uint8_t B = Bytes[P];
    if (B == 0x66 && !OperandOverride)
      OperandOverride = true;
    else if (B == 0x67 && !AddressOverride)
      AddressOverride = true;
    else if ((B == 0x26 || B == 0x2e || B == 0x36 || B == 0x3e || B == 0x64 ||
              B == 0x65) &&
             !Override)
      Override = static_cast<X86SegmentRegister>(B >= 0x64 ? B - 0x60
                                                           : (B - 0x26) / 8);
    else
      break;
    ++P;
  }
  F.Bytes = Code32 != OperandOverride ? 4 : 2;
  F.AddressBytes = Code32 != AddressOverride ? 4 : 2;
  if (P == Bytes.size())
    return std::nullopt;
  const uint8_t Opcode = Bytes[P++];
  uint8_t Extended = 0;
  if (Opcode == 0x0f) {
    if (P == Bytes.size())
      return std::nullopt;
    Extended = Bytes[P++];
  }
  if (Opcode == 0x06 || Opcode == 0x07 || Opcode == 0x0e || Opcode == 0x16 ||
      Opcode == 0x17 || Opcode == 0x1e || Opcode == 0x1f ||
      (Opcode == 0x0f && (Extended == 0xa0 || Extended == 0xa1 ||
                          Extended == 0xa8 || Extended == 0xa9))) {
    if (P != Bytes.size())
      return std::nullopt;
    F.Pop = (Opcode == 0x0f ? Extended : Opcode) & 1;
    F.Read = F.Push = !F.Pop;
    F.Segment = static_cast<X86SegmentRegister>(
        Opcode == 0x0f ? (Extended & 8 ? 5 : 4) : (Opcode - 6) / 8);
    F.MemorySegment = X86SegmentRegister::SS;
    F.Base = 4;
    return F;
  }
  F.Far = Opcode == 0xc4 || Opcode == 0xc5 ||
          (Opcode == 0x0f &&
           (Extended == 0xb2 || Extended == 0xb4 || Extended == 0xb5));
  if ((!F.Far && Opcode != 0x8c && Opcode != 0x8e) || P == Bytes.size())
    return std::nullopt;
  F.Read = Opcode == 0x8c;
  const uint8_t ModRM = Bytes[P++];
  const unsigned Mod = ModRM >> 6, Reg = (ModRM >> 3) & 7, RM = ModRM & 7;
  if (F.Far) {
    if (Mod == 3)
      return std::nullopt;
    F.Segment = Opcode == 0xc4     ? X86SegmentRegister::ES
                : Opcode == 0xc5   ? X86SegmentRegister::DS
                : Extended == 0xb2 ? X86SegmentRegister::SS
                : Extended == 0xb4 ? X86SegmentRegister::FS
                                   : X86SegmentRegister::GS;
    F.Register = Reg;
  } else {
    if (!x86SegmentRegisterName(Reg) || (!F.Read && Reg == 1))
      return std::nullopt;
    F.Segment = static_cast<X86SegmentRegister>(Reg);
    if (Mod == 3) {
      if (P != Bytes.size())
        return std::nullopt;
      F.Register = RM;
      return F;
    }
  }
  unsigned DispBytes = Mod == 1 ? 1 : Mod == 2 ? F.AddressBytes : 0;
  if (F.AddressBytes == 2) {
    if (Mod == 0 && RM == 6) {
      DispBytes = 2;
    } else {
      constexpr unsigned Bases[]{3, 3, 5, 5, 6, 7, 5, 3};
      F.Base = Bases[RM];
      if (RM < 4)
        F.Index = RM & 1 ? 7 : 6;
    }
  } else if (RM == 4) {
    if (P == Bytes.size())
      return std::nullopt;
    const uint8_t SIB = Bytes[P++];
    const unsigned Index = (SIB >> 3) & 7, Base = SIB & 7;
    F.Scale = 1u << (SIB >> 6);
    if (Index != 4)
      F.Index = Index;
    if (Mod == 0 && Base == 5)
      DispBytes = 4;
    else
      F.Base = Base;
  } else if (Mod == 0 && RM == 5) {
    DispBytes = 4;
  } else {
    F.Base = RM;
  }
  if (P + DispBytes != Bytes.size())
    return std::nullopt;
  for (unsigned I = 0; I != DispBytes; ++I)
    F.Displacement |= uint32_t(Bytes[P + I]) << (I * 8);
  if (DispBytes == 1)
    F.Displacement = int8_t(F.Displacement);
  F.MemorySegment =
      Override.value_or(F.Base == 4 || F.Base == 5 ? X86SegmentRegister::SS
                                                   : X86SegmentRegister::DS);
  return F;
}

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYSEGMENTENCODING_H
