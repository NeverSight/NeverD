//===- DriverProcessMemory.h - Declared process-owned committed RAM -------===//
// NeverD Decompiler
///
/// \file
/// Declared process-owned committed RAM.
///

#ifndef NEVERD_EMULATION_DRIVERPROCESSMEMORY_H
#define NEVERD_EMULATION_DRIVERPROCESSMEMORY_H
#include "neverd/emulation/DriverUserMemory.h"

#include <cstddef>
#include <cstdint>
#include <vector>
namespace neverd::emulation {
struct DriverProcessMemoryRegion {
  uint64_t Base = 0;
  uint32_t Size = 0;
  // Committed zero-initialized RAM, populated with these initial bytes.
  std::vector<uint8_t> Bytes;
  DriverUserPageAccess Access = DriverUserPageAccess::ReadOnly;
};
inline constexpr size_t MaxDriverProcessMemoryRegions = 64;
inline constexpr uint64_t MaxDriverProcessMemoryBytes = 4 * 1024 * 1024;
} // namespace neverd::emulation
#endif
