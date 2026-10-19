//===- DriverRegistryEvents.h - Explicit external registry operations -----===//
// NeverD Decompiler
///
/// \file
/// Explicit external registry operations.
///

#ifndef NEVERD_EMULATION_DRIVERREGISTRYEVENTS_H
#define NEVERD_EMULATION_DRIVERREGISTRYEVENTS_H
#include "neverd/emulation/DriverRegistry.h"

namespace neverd::emulation {
struct DriverOptions;
// Kernel-origin operations in an explicitly live guest process. Handles are
// scenario identities, never host handles. Existing registry and filter
// continuations own all operation semantics and pre/post notifications.
struct DriverRegistryEvent {
  enum class Kind { Open, SetValue, QueryValue, Close };
  Kind Operation = Kind::Open;
  size_t BeforeRequest = 0;
  uint32_t Handle = 0;
  std::string Path;
  uint32_t DesiredAccess = 0;
  std::string ValueName;
  std::optional<uint32_t> ValueType;
  std::vector<uint8_t> Data;
  uint32_t OutputSize = 0;
  uint32_t ActorProcessID = 4;
  // Open-only parent scenario handle. Zero selects an absolute path.
  uint32_t RootHandle = 0;
};
inline constexpr size_t MaxDriverRegistryEvents = 256;
struct DriverRegistryEventResult {
  DriverRegistryEvent Event;
  size_t SourceIndex = 0;
  std::optional<uint32_t> Status, RequiredSize;
  // Successful KeyValuePartialInformation bytes, including its 12-byte header.
  std::vector<uint8_t> Output;
  bool Completed = false;
};
llvm::Error validateDriverRegistryEvents(const DriverOptions &);
} // namespace neverd::emulation
#endif
