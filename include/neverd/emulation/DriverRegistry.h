//===- DriverRegistry.h - Concrete guest registry state -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit session-local registry input and observed state. This interface
/// never reads the host registry or claims to emulate Windows ACLs.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERREGISTRY_H
#define NEVERD_EMULATION_DRIVERREGISTRY_H

#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {

#define NEVERD_DRIVER_REGISTRY_LIMIT(Name, Value)                              \
  inline constexpr size_t Name = Value;
#include "neverd/emulation/DriverRegistryLimits.def"
#undef NEVERD_DRIVER_REGISTRY_LIMIT

struct DriverRegistryValue {
  std::string Name;
  uint32_t Type = 0;
  std::vector<uint8_t> Data;
};

struct DriverRegistryKey {
  std::string Path;
  std::vector<DriverRegistryValue> Values;
};

/// An omitted inventory means unknown availability. An explicit inventory is
/// a closed tree with full access, rooted under \Registry\Machine or
/// \Registry\User. Key ancestors are implicit and count toward the key limit.
/// Names use ASCII case folding; arbitrary value bytes are preserved exactly.
/// TotalBytes bounds value payloads; the separate name limits bound names.
llvm::Error validateDriverRegistry(
    const std::optional<std::vector<DriverRegistryKey>> &Registry);

} // namespace neverd::emulation

#endif // NEVERD_EMULATION_DRIVERREGISTRY_H
