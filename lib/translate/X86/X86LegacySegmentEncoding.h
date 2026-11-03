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

#include "X86LegacyEncoding.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace neverd::translate::detail {

struct X86LegacySegmentInstruction : X86LegacyAddress {
  X86SegmentRegister Segment = X86SegmentRegister::ES;
  unsigned Bytes = 4;
  bool Read = false, Push = false, Pop = false, Far = false;
};

inline std::optional<X86LegacySegmentInstruction>
decodeLegacySegmentInstruction(llvm::ArrayRef<uint8_t> Bytes, bool Code32) {
  X86LegacyEncoding Reader(Bytes, Code32);
  X86LegacySegmentInstruction F;
  F.Bytes = Reader.OperandBytes;
  F.AddressBytes = Reader.AddressBytes;
  const uint8_t Opcode = Reader.byte();
  const uint8_t Extended = Opcode == 0x0f ? Reader.byte() : 0;
  if (Opcode == 0x06 || Opcode == 0x07 || Opcode == 0x0e || Opcode == 0x16 ||
      Opcode == 0x17 || Opcode == 0x1e || Opcode == 0x1f ||
      (Opcode == 0x0f && (Extended == 0xa0 || Extended == 0xa1 ||
                          Extended == 0xa8 || Extended == 0xa9))) {
    if (!Reader.done())
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
  if (!F.Far && Opcode != 0x8c && Opcode != 0x8e)
    return std::nullopt;
  F.Read = Opcode == 0x8c;
  const unsigned Reg = Reader.modRM(F);
  if (F.Far) {
    if (F.Register)
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
    if (F.Register) {
      if (!Reader.done())
        return std::nullopt;
      return F;
    }
  }
  if (!Reader.done())
    return std::nullopt;
  return F;
}

} // namespace neverd::translate::detail

#endif // NEVERD_LIB_TRANSLATE_X86_X86LEGACYSEGMENTENCODING_H
