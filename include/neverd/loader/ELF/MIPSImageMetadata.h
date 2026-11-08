//===- MIPSImageMetadata.h - Admitted MIPS ELF profile --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MIPS ABI and ISA evidence retained by the ELF loader. The architecture
/// identity separately records register width and byte order.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_MIPSIMAGEMETADATA_H
#define NEVERD_LOADER_ELF_MIPSIMAGEMETADATA_H

#include <cstdint>
#include <optional>

namespace neverd {

enum class MIPSABI : uint8_t { O32, N64 };

struct MIPSImageMetadata {
  MIPSABI ABI = MIPSABI::O32;
  uint32_t ELFFlags = 0;
  /// The admitted instruction profile. Without .MIPS.abiflags, e_flags cannot
  /// distinguish Release 2 from Releases 3 and 5; only the R2 subset is
  /// admitted.
  uint8_t ISARevision = 2;
  bool HasABIFlags = false;
  /// GNU MIPS FP ABI tag, when supplied by .MIPS.abiflags. Integer analysis
  /// does not establish support for these floating-point calling conventions.
  std::optional<uint8_t> FPABI;
};

} // namespace neverd

#endif // NEVERD_LOADER_ELF_MIPSIMAGEMETADATA_H
