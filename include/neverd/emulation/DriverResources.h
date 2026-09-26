//===- DriverResources.h - Explicit guest register-bank resources --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed synthetic bus assignments and deterministic register initial values.
/// Addresses never refer to host physical memory. Each device initializes its
/// registers once; mapping, unmapping and restarting do not reset their values.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERRESOURCES_H
#define NEVERD_EMULATION_DRIVERRESOURCES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neverd::emulation {

#define NEVERD_DRIVER_RESOURCE_LIMIT(Name, Value)                              \
  inline constexpr size_t Name = Value;
#include "neverd/emulation/DriverResources.def"
#undef NEVERD_DRIVER_RESOURCE_LIMIT

enum class DriverRegisterAccess : uint8_t {
#define NEVERD_DRIVER_REGISTER_ACCESS(Name, Spelling) Name,
#include "neverd/emulation/DriverResources.def"
#undef NEVERD_DRIVER_REGISTER_ACCESS
};

struct DriverRegister {
  uint32_t Offset = 0;
  /// Exact naturally aligned access width in bytes; zero is invalid.
  uint8_t Width = 0;
  DriverRegisterAccess Access = DriverRegisterAccess::ReadOnly;
  uint32_t Value = 0;
};

struct DriverMemoryResource {
  /// Raw and translated START lists contain corresponding Memory descriptors
  /// in this inventory's order. The synthetic bus uses one full descriptor,
  /// Internal interface, bus zero, version/revision one, DeviceExclusive
  /// sharing and READ_WRITE resource flags; per-register access remains
  /// independent.
  std::string ID;
  uint64_t RawStart = 0;
  uint64_t TranslatedStart = 0;
  uint32_t Length = 0;
  /// Unlisted bytes have no register semantics and cannot be accessed.
  std::vector<DriverRegister> Registers;
};

} // namespace neverd::emulation

#endif // NEVERD_EMULATION_DRIVERRESOURCES_H
