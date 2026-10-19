//===- DriverProcessImages.h - Explicit SEC_IMAGE snapshot sources --------===//
// NeverD Decompiler
///
/// \file
/// Explicit SEC_IMAGE snapshot sources.
///

#ifndef NEVERD_EMULATION_DRIVERPROCESSIMAGES_H
#define NEVERD_EMULATION_DRIVERPROCESSIMAGES_H
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace neverd::emulation {
/// An independent process image-section snapshot, before user-loader fixups.
/// FileBytes are the original PE file, not bytes already arranged by RVA.
/// MappedFile is an explicit guest NT device path, never a host path lookup.
struct DriverProcessImageMapping {
  uint64_t Base = 0; // Zero selects the PE preferred base.
  std::string MappedFile;
  // Observed file-volume device object name, independent of its relative path.
  // Native partial filename queries need this boundary, not a guessed split.
  std::string DeviceName;
  std::vector<uint8_t> FileBytes;
};
inline constexpr uint64_t MaxDriverProcessImageFileBytes = 1024 * 1024;
inline constexpr size_t MaxDriverProcessImages = 16;
} // namespace neverd::emulation
#endif
