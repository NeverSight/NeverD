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

/// Canonical COP1 for microMIPS64 Volume II-B. Numeric FPR operands and
/// the physical halfword displacement are preserved in either byte order.
inline std::optional<uint32_t> microR6Float(uint32_t Word) {
  const unsigned Major = Word >> 26, T = Word >> 21 & 31, S = Word >> 16 & 31,
                 D = Word >> 11 & 31;
  switch (Major) {
  case 0x26:
  case 0x27:
  case 0x2e:
  case 0x2f: {
    const unsigned Op = Major == 0x26   ? 57
                        : Major == 0x27 ? 49
                        : Major == 0x2e ? 61
                                        : 53;
    return Op << 26 | S << 21 | T << 16 | uint16_t(Word);
  }
  case 0x11:
    if (T == 8 || T == 9)
      return (T == 8 ? 0x45200000u : 0x45a00000u) | S << 16 | uint16_t(Word);
    return std::nullopt;
  case 0x15:
    break;
  default:
    return std::nullopt;
  }
  switch (Word & 0xffff) {
#define NEVERD_MICRO_FP_UNARY(Minor, Classic)                                  \
  case Minor:                                                                  \
    return Classic | (S << 11) | (T << 6);
#define NEVERD_MICRO_FP_REVERSED(Minor, Classic)                               \
  case Minor:                                                                  \
    return Classic | (T << 11) | (S << 6);
#include "neverd/lift/MIPS/MicroMIPSFloat.def"
  }
  if ((Word & 0xc03f) == 0x3b) {
    const unsigned Minor = Word >> 6 & 255;
    switch (Minor) {
    case 0x80:
    case 0x90:
    case 0x40:
    case 0xc0:
    case 0xa0:
    case 0xb0:
    case 0x60:
    case 0xe0: {
      const unsigned Fmt = Minor == 0x80   ? 0
                           : Minor == 0x90 ? 1
                           : Minor == 0x40 ? 2
                           : Minor == 0xc0 ? 3
                           : Minor == 0xa0 ? 4
                           : Minor == 0xb0 ? 5
                           : Minor == 0x60 ? 6
                                           : 7;
      return 0x44000000u | Fmt << 21 | T << 16 | S << 11;
    }
    }
  }
  const unsigned F = Word & 63;
  if (F == 5 || F == 21) {
    const unsigned Cond = Word >> 6 & 31;
    if (Cond < 16 || (Cond >= 17 && Cond <= 19) || (Cond >= 25 && Cond <= 27))
      return (F == 5 ? 0x46800000u : 0x46a00000u) | T << 16 | S << 11 | D << 6 |
             Cond;
    return std::nullopt;
  }
  switch (Word & 0x7ff) {
#define NEVERD_MICRO_FP_BINARY(Minor, Classic)                                 \
  case Minor:                                                                  \
    return Classic | (T << 16) | (S << 11) | (D << 6);
#include "neverd/lift/MIPS/MicroMIPSFloat.def"
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
