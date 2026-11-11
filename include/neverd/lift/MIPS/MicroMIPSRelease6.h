//===- MicroMIPSRelease6.h - Compact control and atomic encodings --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_MIPS_MICROMIPSRELEASE6_H
#define NEVERD_LIFT_MIPS_MICROMIPSRELEASE6_H

#include "llvm/Support/MathExtras.h"

#include <cstdint>
#include <optional>

namespace neverd::mips {
/// Return the equivalent Release 6 branch predicate and displacement fields.
/// Physical length, halfword displacement scale and tagged links remain micro.
inline std::optional<uint32_t> microR6Compact(uint32_t Word, unsigned Size) {
  if (Size == 2) {
    const unsigned Major = Word >> 10;
    if (Major == 0x33)
      return (50u << 26) |
             (uint32_t(llvm::SignExtend64<10>(Word)) & 0x03ffffff);
    if (Major == 0x23 || Major == 0x2b) {
      const unsigned Field = (Word >> 7) & 7;
      return ((Major == 0x23 ? 54u : 62u) << 26) |
             ((Field < 2 ? Field + 16 : Field) << 21) |
             (uint32_t(llvm::SignExtend64<7>(Word)) & 0x1fffff);
    }
    return std::nullopt;
  }
  const unsigned Major = Word >> 26;
  const unsigned RS = (Word >> 16) & 31, RT = (Word >> 21) & 31;
  switch (Major) {
  case 0x25:
    return (50u << 26) | (Word & 0x03ffffff); // BC
  case 0x2d:
    return (58u << 26) | (Word & 0x03ffffff); // BALC
  case 0x20:
  case 0x28: // BEQZC/JIC and BNEZC/JIALC
    return ((Major == 0x20 ? 54u : 62u) << 26) | (Word & 0x03ffffff);
  case 0x1d:
  case 0x1f: // BOVC / BNVC, equality and conditional link
    return ((Major == 0x1d ? 8u : 24u) << 26) | (RS << 21) | (RT << 16) |
           uint16_t(Word);
  case 0x30:
  case 0x38:
  case 0x35:
  case 0x3d: {
    if (!RT)
      return std::nullopt;
    const unsigned Op = Major == 0x30   ? 6
                        : Major == 0x38 ? 7
                        : Major == 0x35 ? 23
                                        : 22;
    return (Op << 26) | (RS << 21) | (RT << 16) | uint16_t(Word);
  }
  default:
    return std::nullopt;
  }
}

/// Canonical register roles for LSA and ALIGN; some physical decoders
/// exchange the source and destination fields in their operand detail.
inline std::optional<uint32_t> microR6ScaledALU(uint32_t Word) {
  if ((Word & 0xfc0001ef) != 0x0f)
    return std::nullopt;
  const unsigned RS = (Word >> 16) & 31, RT = (Word >> 21) & 31;
  const unsigned RD = (Word >> 11) & 31, SA = (Word >> 9) & 3;
  const auto Fields = (RS << 21) | (RT << 16) | (RD << 11) | (SA << 6);
  return Fields | ((Word & 0x10) ? (31u << 26) | (8u << 6) | 32u : 5u);
}

inline std::optional<uint32_t> microR6HardwareRegister(uint32_t Word) {
  if ((Word & 0xfc00c7ff) != 0x1c0)
    return std::nullopt;
  return 0x7c00003b | (((Word >> 21) & 31) << 16) |
         (((Word >> 16) & 31) << 11) | (((Word >> 11) & 7) << 6);
}

inline std::optional<uint32_t> microR6Cache(uint32_t Word) {
  const uint32_t Shape = Word & 0xfc00fe00;
  if (Shape != 0x20006000 && Shape != 0x60002000)
    return std::nullopt;
  const unsigned RS = (Word >> 16) & 31, Hint = (Word >> 21) & 31;
  return (31u << 26) | (RS << 21) | (Hint << 16) | ((Word & 0x1ff) << 7) |
         (Shape == 0x20006000 ? 0x25u : 0x35u);
}

inline std::optional<uint32_t> microR6Reservation(uint32_t Word) {
  if ((Word >> 26) != 0x18)
    return std::nullopt;
  const unsigned Sub = (Word >> 12) & 15;
  if (!(Sub & 1) || (Word & 0xe00))
    return std::nullopt;
  const bool Pair = !(Sub & 2);
  if (Pair && (Word & 15))
    return std::nullopt;
  const unsigned RS = (Word >> 16) & 31, RT = (Word >> 21) & 31;
  const unsigned Fn = (Sub & 8 ? 0x26u : 0x36u) + bool(Sub & 4);
  const unsigned Argument =
      Pair ? (((Word >> 4) & 31) << 11) | 64 : (Word & 0x1ff) << 7;
  return (31u << 26) | (RS << 21) | (RT << 16) | Argument | Fn;
}
} // namespace neverd::mips
#endif // NEVERD_LIFT_MIPS_MICROMIPSRELEASE6_H
