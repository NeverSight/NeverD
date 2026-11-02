//===- AArch64FloatConversion.h - ARM64 narrowing --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binary64 to binary32 conversion for modeled native runtime operations.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ARCH_AARCH64_FLOAT_CONVERSION_H
#define NEVERD_EMULATION_ARCH_AARCH64_FLOAT_CONVERSION_H

#include <cstdint>
#include <optional>

namespace neverd::emulation {
struct AArch64FloatConversion {
  uint32_t Bits;
  uint32_t Raised;
};

/// FCVT S,D under the checked machine's baseline FPCR contract. Raised holds
/// cumulative FPSR bits to OR into the caller's status; FPCR is unchanged.
/// Optional control extensions and exception traps remain unsupported.
std::optional<AArch64FloatConversion> narrowAArch64Double(uint64_t Bits,
                                                          uint64_t FPCR);
} // namespace neverd::emulation
#endif // NEVERD_EMULATION_ARCH_AARCH64_FLOAT_CONVERSION_H
