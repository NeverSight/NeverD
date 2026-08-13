//===- BinaryImageDynamic.h - Dynamic linking metadata ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What an image says about how it is to be loaded and bound: its dependencies
/// and initializer arrays, its build identity, and the PE load-configuration
/// provenance the guard tables hang off.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_BINARYIMAGEDYNAMIC_H
#define NEVERD_LOADER_BINARYIMAGEDYNAMIC_H

#include "neverd/Common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd {

// ===--------------------------------------------------------------------===//
// DynamicInfo — dynamic linking metadata
// ===--------------------------------------------------------------------===//

struct DynamicInfo {
  std::string SOName;
  std::vector<std::string> NeededLibs;
  std::vector<std::string> RPaths;
  va_t InitAddr = 0;
  va_t FiniAddr = 0;
  std::vector<va_t> PreinitArray;
  std::vector<va_t> InitArray;
  std::vector<va_t> FiniArray;

  /// PDB path (PE/COFF) or build-id string (ELF).
  std::string PDBPath;

  /// Mach-O: 16-byte UUID from LC_UUID, hex-encoded.
  std::string UUID;

  /// Mach-O: LC_BUILD_VERSION min OS version string (e.g. "14.0.0").
  std::string MinOSVersion;

  /// PE/COFF: security cookie RVA from Load Configuration directory.
  va_t SecurityCookieRVA = 0;

  /// PE/COFF: CF Guard check function pointer RVA.
  va_t GuardCFCheckFunctionRVA = 0;

  /// PE/COFF load-configuration provenance and guard tables.  Native table
  /// pointers are normalized from VAs to RVAs at load time.
  va_t LoadConfigRVA = 0;
  uint32_t LoadConfigSize = 0;
  uint32_t GuardFlags = 0;
  va_t GuardCFFunctionTableRVA = 0;
  uint64_t GuardCFFunctionCount = 0;
  va_t GuardEHContinuationTableRVA = 0;
  uint64_t GuardEHContinuationCount = 0;
};

} // namespace neverd

#endif // NEVERD_LOADER_BINARYIMAGEDYNAMIC_H
