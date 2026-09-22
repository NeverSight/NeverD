//===- DriverImage.h - Validated PE mapping plan --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private image-loading contract for Windows driver emulation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERIMAGE_H
#define NEVERD_EMULATION_DRIVERIMAGE_H

#include "neverd/emulation/DriverProfile.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace neverd::emulation {
struct DriverImageRegion {
  uint64_t Address = 0;
  unsigned Permissions = 0;
  std::vector<uint8_t> Bytes; // Page aligned and zero filled to virtual size.
};
struct DriverImport {
  uint64_t Slot = 0;
  std::string Module;
  std::string Name;
};
struct DriverImage {
  uint64_t Base = 0;
  uint64_t PreferredBase = 0;
  uint64_t SecurityCookieAddress = 0;
  uint64_t Entry = 0;
  uint64_t Size = 0;
  std::vector<DriverImageRegion> Regions;
  std::vector<DriverImport> Imports;
};
/// Concrete guest cookie used for reproducible analysis, never for protection
/// of the host. The PE wrapper still executes its own cookie checks.
inline constexpr uint64_t DriverSecurityCookie = profile::SecurityCookie;
/// Strict x64 PE native-subsystem executable profile. LoadAddress zero selects
/// the preferred base; explicit bases require complete supported relocations.
/// Reject malformed/truncated metadata and unsupported load-time features.
llvm::Expected<DriverImage> loadDriverImage(const std::filesystem::path &Path,
                                            uint64_t MemoryLimit,
                                            uint64_t LoadAddress = 0);
} // namespace neverd::emulation
#endif
