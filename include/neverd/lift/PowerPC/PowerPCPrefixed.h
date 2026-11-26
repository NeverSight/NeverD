//===- PowerPCPrefixed.h - Prefixed instruction fields ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared instruction-boundary and D34 displacement rules for lifting and
/// ELF linkage recognition.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_POWERPC_POWERPCPREFIXED_H
#define NEVERD_LIFT_POWERPC_POWERPCPREFIXED_H

#include "llvm/Support/MathExtras.h"

#include <cstdint>

namespace neverd::ppc::prefix {
#include "neverd/lift/PowerPC/PowerPCPrefixed.inc"

inline bool fitsFetchBlock(uint64_t PC) {
  return PC % FetchBlockBytes <= FetchBlockBytes - InstructionBytes;
}

inline int64_t displacement(uint32_t Prefix, uint32_t Suffix) {
  uint64_t Bits = uint64_t(Prefix & HighImmediateMask) << LowImmediateBits;
  return llvm::SignExtend64<DisplacementBits>(Bits |
                                              (Suffix & LowImmediateMask));
}
} // namespace neverd::ppc::prefix

#endif // NEVERD_LIFT_POWERPC_POWERPCPREFIXED_H
