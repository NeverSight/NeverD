//===- KernelSystemVA.h - Owned Windows 19041 virtual reservations ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Owned Windows 19041 virtual reservations.
///

#ifndef NEVERD_EMULATION_KERNELSYSTEMVA_H
#define NEVERD_EMULATION_KERNELSYSTEMVA_H
#include "KernelGuestPageTables.h"

#include "neverd/emulation/DriverProfile.h"

namespace neverd::emulation::systemva {
// _MI_SYSTEM_VA_TYPE and the 13-entry assignment array in the matched 19041
// public PDB. The model reserves only the address classes it actually owns.
enum class Kind : unsigned {
  Unused,
  SessionSpace,
  ProcessSpace,
  BootLoaded,
  PfnDatabase,
  NonPagedPool,
  PagedPool,
  SpecialPoolPaged,
  SystemCache,
  SystemPtes,
  Hal,
  SessionGlobalSpace,
  DriverImages
};
inline constexpr unsigned Count = 13;
inline constexpr uint64_t AssignmentBytes = Count * 16;
inline constexpr uint64_t PagedPoolBase = 0xffffc10000000000ULL;
inline constexpr uint64_t PagedPoolSize = profile::KernelArenaSize;
inline constexpr uint64_t SystemMappingBase =
    KernelGuestPageTables::TableBase + KernelGuestPageTables::TableBytes;
inline constexpr uint64_t SystemMappingSize = profile::UserAliasSize;
inline constexpr uint64_t SystemPteBase = KernelGuestPageTables::TableBase;
inline constexpr uint64_t SystemPteSize =
    KernelGuestPageTables::TableBytes + SystemMappingSize;
} // namespace neverd::emulation::systemva
#endif
