//===- MicroMIPSLegacyFloat.h - Floating encodings -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_MIPS_MICROMIPSLEGACYFLOAT_H
#define NEVERD_LIFT_MIPS_MICROMIPSLEGACYFLOAT_H

#include <cstdint>
#include <optional>

namespace neverd::mips {
/// BC1F/T has a halfword-scaled displacement and one physical delay slot.
inline std::optional<uint32_t> microLegacyFloatBranch(uint32_t Word) {
  if ((Word & 0xffc30000u) != 0x43800000u)
    return std::nullopt;
  return 0x45000000u | (Word & 0x001c0000u) | ((Word & 0x00200000u) >> 5) |
         uint16_t(Word);
}

/// Decode only physical pre-R6 floating encodings. Semantic availability,
/// FR pairing, condition restrictions and FCSR effects belong to COP1.
inline std::optional<uint32_t> microLegacyFloat(uint32_t Word) {
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
  case 0x10:
    return microLegacyFloatBranch(Word);
  case 0x15:
    break;
  default:
    return std::nullopt;
  }
  switch (Word & 0xffff) {
#define NEVERD_MICRO_FP_UNARY(Minor, Classic)                                  \
  case Minor:                                                                  \
    return Classic | (S << 11) | (T << 6);
#include "neverd/lift/MIPS/MicroMIPSFloat.def"
#define NEVERD_MICRO_LEGACY_FP_UNARY(Minor, Classic)                           \
  case Minor:                                                                  \
    return Classic | (S << 11) | (T << 6);
#include "neverd/lift/MIPS/MicroMIPSLegacyFloat.def"
  }
  switch (Word & 0x7ff) {
#define NEVERD_MICRO_FP_BINARY(Minor, Classic, Legacy)                         \
  case Minor:                                                                  \
    if (Legacy)                                                                \
      return Classic | (T << 16) | (S << 11) | (D << 6);                       \
    break;
#include "neverd/lift/MIPS/MicroMIPSFloat.def"
#define NEVERD_MICRO_LEGACY_FP_BINARY(Minor, Classic)                          \
  case Minor:                                                                  \
    return Classic | (T << 16) | (S << 11) | (D << 6);
#include "neverd/lift/MIPS/MicroMIPSLegacyFloat.def"
  }
  switch (Word & 63) {
#define NEVERD_MICRO_LEGACY_FP_TERNARY(Minor, Classic)                         \
  case Minor:                                                                  \
    return Classic | ((Word >> 6 & 31) << 21) | (T << 16) | (S << 11) |        \
           (D << 6);
#include "neverd/lift/MIPS/MicroMIPSLegacyFloat.def"
  case 0x3c: { // C.cond.fmt: bit 12 is reserved, fmt occupies bits 11:10.
    const unsigned Fmt = Word >> 10 & 3;
    if ((Word & 0x1000) || Fmt == 3)
      return std::nullopt;
    return 0x44000030u | ((Fmt == 2 ? 22u : 16u + Fmt) << 21) | (T << 16) |
           (S << 11) | ((Word >> 13 & 7) << 8) | (Word >> 6 & 15);
  }
  case 0x20: { // MOVF/T.fmt uses T as the destination, not an input.
    const unsigned Fmt = Word >> 9 & 3;
    if ((Word & 0x1980) || Fmt == 3)
      return std::nullopt;
    return 0x44000011u | ((Fmt == 2 ? 22u : 16u + Fmt) << 21) |
           (((Word >> 13 & 7) * 4 + (Word >> 6 & 1)) << 16) | (S << 11) |
           (T << 6);
  }
  case 0x38: { // MOVN/Z.fmt tests a GPR; its fmt field is bits 9:8.
    const unsigned Fmt = Word >> 8 & 3;
    if ((Word & 0x480) || Fmt == 3)
      return std::nullopt;
    return 0x44000000u | ((Fmt == 2 ? 22u : 16u + Fmt) << 21) | (T << 16) |
           (S << 11) | (D << 6) | ((Word & 64) ? 18u : 19u);
  }
  case 0x08: { // Indexed memory: FP register D, address GPRs S and T.
    if (Word & 0x600)
      return std::nullopt;
    const unsigned Minor = Word >> 6 & 7;
    if (Minor < 1 || Minor > 6)
      return std::nullopt;
    const bool Store = !(Minor & 1);
    const unsigned Fn = (Minor <= 2 ? 0 : Minor <= 4 ? 1 : 5) + (Store ? 8 : 0);
    return 0x4c000000u | (S << 21) | (T << 16) | (D << (Store ? 11 : 6)) | Fn;
  }
  case 0x3b: {
    const unsigned Minor = Word >> 6 & 1023;
    // Integer MOVF/T uses the same condition bank as its FP counterpart.
    if ((Minor & 0x7f) == 5 || (Minor & 0x7f) == 0x25)
      return (S << 21) | (((Word >> 13 & 7) * 4 + bool(Minor & 0x20)) << 16) |
             (T << 11) | 1;
    if (Word & 0xc000)
      return std::nullopt;
    unsigned Transfer;
    switch (Minor) {
    case 0x80:
      Transfer = 0;
      break; // MFC1
    case 0x90:
      Transfer = 1;
      break; // DMFC1
    case 0x40:
      Transfer = 2;
      break; // CFC1
    case 0xc0:
      Transfer = 3;
      break; // MFHC1
    case 0xa0:
      Transfer = 4;
      break; // MTC1
    case 0xb0:
      Transfer = 5;
      break; // DMTC1
    case 0x60:
      Transfer = 6;
      break; // CTC1
    case 0xe0:
      Transfer = 7;
      break; // MTHC1
    default:
      return std::nullopt;
    }
    return 0x44000000u | Transfer << 21 | T << 16 | S << 11;
  }
  default:
    return std::nullopt;
  }
}
} // namespace neverd::mips
#endif // NEVERD_LIFT_MIPS_MICROMIPSLEGACYFLOAT_H
