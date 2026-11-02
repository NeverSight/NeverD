//===- X86LegacyScalarPrefixes.h - Scalar prefixes ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Scan the independent 66/F3 selectors used by scalar bit counts and ADX.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LEGACYSCALARPREFIXES_H
#define NEVERD_LIB_LIFT_X86_X86LEGACYSCALARPREFIXES_H

#include "X86LiftAPXValidation.h"

#include <capstone/capstone.h>
#include <cstddef>
#include <cstdint>

namespace neverd::legacyvalidation {

struct ScalarPrefixes {
  size_t OpcodeOffset = 0;
  uint8_t Segment = 0;
  uint8_t Rex = 0;
  bool Address = false;
  bool Operand16 = false;
  bool F3 = false;
};

/// A legacy prefix cancels a preceding REX; the last adjacent REX wins.
/// The family validates its selectors, opcode map and any REX2 escape.
inline bool scalarPrefixes(const cs_insn *Insn, bool Mode64,
                           ScalarPrefixes &P) {
  P = {};
  while (P.OpcodeOffset < Insn->size) {
    const uint8_t B = Insn->bytes[P.OpcodeOffset];
    if (Mode64 && (B & 0xf0) == 0x40) {
      P.Rex = B;
      ++P.OpcodeOffset;
      continue;
    }
    if (apxvalidation::isSegmentPrefix(B)) {
      if (P.Segment)
        return false;
      P.Segment = B;
    } else if (B == 0x67) {
      if (P.Address)
        return false;
      P.Address = true;
    } else if (B == 0x66) {
      if (P.Operand16)
        return false;
      P.Operand16 = true;
    } else if (B == 0xf3) {
      if (P.F3)
        return false;
      P.F3 = true;
    } else {
      break;
    }
    P.Rex = 0;
    ++P.OpcodeOffset;
  }
  return P.OpcodeOffset < Insn->size;
}

} // namespace neverd::legacyvalidation

#endif // NEVERD_LIB_LIFT_X86_X86LEGACYSCALARPREFIXES_H
