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

#include "neverd/Common.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace neverd {

enum class MIPSABI : uint8_t { O32, N64 };

struct MIPSCodeRegion {
  uint64_t Start, End;
  InstructionMode Mode;
};

struct MIPSImageMetadata {
  MIPSABI ABI = MIPSABI::O32;
  uint32_t ELFFlags = 0;
  /// The admitted instruction profile. Without .MIPS.abiflags, e_flags cannot
  /// distinguish Release 2 from Releases 3 and 5; only the R2 subset is
  /// admitted for that header without the record. Release 6 has distinct
  /// e_flags. MSA requires explicit R5 and FP64 evidence.
  uint8_t ISARevision = 2;
  /// The ELF admits both classic and microMIPS code, not a global micro mode.
  bool HasMicroMIPS = false;
  /// MIPS16e32 code; mutually exclusive with the microMIPS ASE.
  bool HasMIPS16 = false;
  /// MIPS16e2 requires its own .MIPS.abiflags ASE evidence.
  bool HasMIPS16E2 = false;
  /// MT control operations require explicit ABI feature evidence.
  bool HasMT = false;
  /// DSP/DSPR2 execution in classic o32/n64 or microMIPS o32 code.
  bool HasDSP = false;
  bool HasDSPR2 = false;
  /// MSA in classic or microMIPS32r5/o32 code with FP64 register mode.
  bool HasMSA = false;
  std::map<uint64_t, InstructionMode> CodeModeEntries;
  std::vector<MIPSCodeRegion> CodeRegions;
  bool HasABIFlags = false;
  /// GNU MIPS FP ABI tag, when supplied by .MIPS.abiflags. Integer analysis
  /// does not establish support for these floating-point calling conventions.
  std::optional<uint8_t> FPABI;
};

} // namespace neverd

#endif // NEVERD_LOADER_ELF_MIPSIMAGEMETADATA_H
