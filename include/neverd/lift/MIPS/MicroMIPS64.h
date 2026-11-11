//===- MicroMIPS64.h - microMIPS64r2 scalar encodings -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_MIPS_MICROMIPS64_H
#define NEVERD_LIFT_MIPS_MICROMIPS64_H

#include "llvm/Support/MathExtras.h"

#include <cstdint>
#include <optional>

namespace neverd::mips {
/// Canonical scalar word for doubleword forms absent from the micro decoder.
/// The physical encoding always consists of two target-endian halfwords.
inline std::optional<uint32_t> micro64Word(uint32_t Word) {
  const unsigned Major = Word >> 26, RT = (Word >> 21) & 31;
  const unsigned RS = (Word >> 16) & 31, RD = (Word >> 11) & 31;
  const uint32_t Bases = (RS << 21) | (RT << 16);
  switch (Major) {
  case 0x17:
    return (0x19u << 26) | Bases | uint16_t(Word); // DADDIU
  case 0x36:
    return (0x3fu << 26) | Bases | uint16_t(Word); // SD
  case 0x37:
    return (0x37u << 26) | Bases | uint16_t(Word); // LD
  case 0x18: { // POOL32C has a four-bit subopcode and a signed 12-bit
               // displacement.
    unsigned Op;
    switch ((Word >> 12) & 15) {
    case 4:
      Op = 26;
      break; // LDL
    case 5:
      Op = 27;
      break; // LDR
    case 14:
      Op = 39;
      break; // LWU
    default:
      return std::nullopt;
    }
    return (Op << 26) | Bases | uint16_t(llvm::SignExtend64<12>(Word));
  }
  case 0x16:
    break; // POOL32S
  default:
    return std::nullopt;
  }

  // DEXT*/DINS* share the classic immediate and size fields at bits 15:6.
  switch (Word & 63) {
#define NEVERD_MICRO64_BITFIELD(Micro, Classic)                                \
  case Micro:                                                                  \
    return (31u << 26) | Bases | (Word & 0xffc0) | Classic;
#include "neverd/lift/MIPS/MicroMIPS64.def"
  default:
    break;
  }
  switch (Word & 0xffff) {
  case 0x4b3c:
  case 0x5b3c: // DCLO / DCLZ (the classic format repeats rd in rt).
    return (28u << 26) | Bases | (RT << 11) | (Word & 0x1000 ? 36u : 37u);
  case 0x7b3c:
  case 0xfb3c: // DSBH / DSHD
    return (31u << 26) | (RS << 16) | (RT << 11) |
           ((Word & 0x8000 ? 5u : 2u) << 6) | 36u;
  default:
    break;
  }
  switch (Word & 0x7ff) {
  case 0x150:
    return Bases | (RD << 11) | 45; // DADDU
  case 0x1d0:
    return Bases | (RD << 11) | 47; // DSUBU
#define NEVERD_MICRO64_SHIFT(Micro, Classic, Rotate)                           \
  case Micro:                                                                  \
    return (Rotate << 21) | (RS << 16) | (RT << 11) | (RD << 6) | Classic;
#define NEVERD_MICRO64_VARIABLE_SHIFT(Micro, Classic, Rotate)                  \
  case Micro:                                                                  \
    return Bases | (RD << 11) | (Rotate << 6) | Classic;
#include "neverd/lift/MIPS/MicroMIPS64.def"
  default:
    return std::nullopt;
  }
}
} // namespace neverd::mips
#endif // NEVERD_LIFT_MIPS_MICROMIPS64_H
