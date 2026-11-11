//===- MicroMIPS64Release6.h - Doubleword R6 encodings ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_MIPS_MICROMIPS64RELEASE6_H
#define NEVERD_LIFT_MIPS_MICROMIPS64RELEASE6_H

#include "neverd/lift/MIPS/MicroMIPS64.h"

namespace neverd::mips {
/// Doubleword microMIPS64 Release 6 fields in the classic R6 encoding.
inline std::optional<uint32_t> micro64R6Word(uint32_t Word) {
  const unsigned Major = Word >> 26, RT = (Word >> 21) & 31;
  const unsigned RS = (Word >> 16) & 31, RD = (Word >> 11) & 31;
  const auto GPRs = (RS << 21) | (RT << 16) | (RD << 11);
  if (Major == 0x3c && RS) // DAUI
    return (29u << 26) | (RS << 21) | (RT << 16) | uint16_t(Word);
  if (Major == 0x10 && (RT == 16 || RT == 17)) // DATI/DAHI
    return (1u << 26) | (RS << 21) | ((RT == 16 ? 30u : 6u) << 16) |
           uint16_t(Word);
  if (Major == 0x18 && ((Word >> 12) & 15) != 14)
    return std::nullopt; // LWU survives; the old LDL/LDR fields were
                         // reassigned.
  if (Major == 0x16) {
    const auto Sub = Word & 0x7ff;
    if ((Sub & 0x63f) == 0x18) {
      const unsigned K = (Sub >> 6) & 7;
      return GPRs | ((2u + (K & 1)) << 6) | (28u + K / 2);
    }
    if ((Sub & 0xff) == 0x1c) // DALIGN bp at bits 10:8
      return (31u << 26) | GPRs | ((8 + (Sub >> 8)) << 6) | 36u;
    if ((Word & 0xffff) == 0x0b3c) // DBITSWAP
      return (31u << 26) | (RT << 16) | (RS << 11) | 36u;
    if ((Word & 0xffff) == 0x4b3c || (Word & 0xffff) == 0x5b3c)
      return (RS << 21) | (RT << 11) | 64u | (Word & 0x1000 ? 18u : 19u);
    if ((Word & 0xc7ff) == 0x00fc || (Word & 0xc7ff) == 0x02fc)
      return (16u << 26) | ((Sub == 0xfc ? 1u : 5u) << 21) | (RT << 16) |
             (RS << 11) | ((Word >> 11) & 7);
    // The DLSA instruction-format diagram in MD00594 rev. 6.05
    // overlaps DINSM; table 11.14 assigns a different minor. Neither
    // candidate can certify a DLSA here.
    if ((Sub & 0x1ff) == 0x104 || (Sub & 0x1ff) == 0x100)
      return std::nullopt;
    // DSRA32 is likewise 0x084 in its diagram (a DINSM position)
    // and 0x088 in the opcode map. Only the r2 encoding is qualified.
    if (Sub == 0x084 || Sub == 0x088)
      return std::nullopt;
  }
  return micro64Word(Word);
}
} // namespace neverd::mips
#endif // NEVERD_LIFT_MIPS_MICROMIPS64RELEASE6_H
