//===- DriverFiniteFileSystem.h - Explicit finite guest volumes
//-----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit finite guest volumes.
///

#ifndef NEVERD_EMULATION_DRIVERFINITEFILESYSTEM_H
#define NEVERD_EMULATION_DRIVERFINITEFILESYSTEM_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {
struct DriverOptions;
inline constexpr size_t MaxDriverFiniteFileSystemEntries = 256;
inline constexpr uint64_t MaxDriverFiniteFileSystemBytes = 16 * 1024 * 1024;
bool validDriverFiniteFileSystemPath(llvm::StringRef Path,
                                     bool AllowRoot = false);
struct DriverFiniteFileSystemEntry {
  // Absolute volume-relative path. Root is implicit; parents must be listed.
  std::string Path;
  bool Directory = false;
  std::vector<uint8_t> Data;
};
struct DriverFiniteFileSystemVolume {
  std::string Drive;
  std::string DeviceName;
  std::vector<DriverFiniteFileSystemEntry> Entries;
  bool ReadOnly = true;
  struct Alias {
    // Root object name such as \SystemRoot, pointing into this volume.
    std::string Name;
    std::string Path;
  };
  std::vector<Alias> Aliases;
  // Optional complete volume-owned WCHAR uppercase map. Empty retains the
  // ASCII comparison profile. Guest filename comparisons never consult the
  // host.
  std::vector<uint16_t> UpcaseTable;
};
// Omitted inventory means unknown, an explicit empty inventory means no drives.
// Paths use strict UTF-8; guest names are counted UTF-16. Non-ASCII volume
// comparisons require an explicit UpcaseTable. Device/alias names are ASCII.
// This finite namespace never forwards guest operations to the host.
// Volumes are read-only unless explicitly made writable. Raw volume contents
// and filesystem control IRPs are unknown.
llvm::Error validateDriverFiniteFileSystem(
    const std::optional<std::vector<DriverFiniteFileSystemVolume>> &Volumes);
llvm::Error validateDriverFilterManager(const DriverOptions &);
} // namespace neverd::emulation
#endif
