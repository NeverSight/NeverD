//===- DriverRegistryHives.h - Explicit virtual hive persistence --------===//
///
/// \file
/// Explicit virtual hive persistence.
///

#ifndef NEVERD_EMULATION_DRIVERREGISTRYHIVES_H
#define NEVERD_EMULATION_DRIVERREGISTRYHIVES_H

#include "neverd/emulation/DriverRegistry.h"

namespace neverd::emulation {
struct DriverOptions;
/// A closed hive mount backed by a session-owned persistent snapshot.
/// Supplying a mount explicitly declares this virtual storage provider.
/// No host registry, files, lazy writer, or native hive format is accessed.
struct DriverRegistryHive {
  std::string Path;
};
struct DriverRegistryHiveState {
  std::string Path;
  uint64_t FlushCount = 0;
  std::vector<DriverRegistryKey> Committed;
};
llvm::Error validateDriverRegistryHives(const DriverOptions &);
llvm::Error validateDriverRegistryHiveMounts(
    const std::optional<std::vector<DriverRegistryKey>> &,
    const std::optional<std::vector<DriverRegistryHive>> &);
} // namespace neverd::emulation
#endif
