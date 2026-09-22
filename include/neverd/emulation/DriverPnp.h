//===- DriverPnp.h - Explicit driver PnP scenario identities --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Public PnP input and observation records. A configured bus provider is a
/// model-owned guest device, never a host device or an inferred hardware bus.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERPNP_H
#define NEVERD_EMULATION_DRIVERPNP_H

#include <cstdint>
#include <optional>
#include <string>

namespace neverd::emulation {
enum class DevicePnpState : uint8_t {
#define NEVERD_DEVICE_PNP_STATE(Name, Value) Name = Value,
#include "neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_DEVICE_PNP_STATE
};
enum class DevicePnpRequest : uint8_t {
#define NEVERD_DEVICE_PNP_REQUEST(Name, Value, MayFail) Name = Value,
#include "neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_DEVICE_PNP_REQUEST
};

constexpr bool devicePnpRequiresSuccess(DevicePnpRequest Request) {
  switch (Request) {
#define NEVERD_DEVICE_PNP_EXACT_SUCCESS(Name) case DevicePnpRequest::Name:
#include "neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_DEVICE_PNP_EXACT_SUCCESS
    return true;
  default:
    return false;
  }
}

enum class DevicePowerState : uint32_t {
#define NEVERD_DEVICE_POWER_STATE(Name, Value) Name = Value,
#include "neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_DEVICE_POWER_STATE
};
enum class SystemPowerState : uint32_t {
#define NEVERD_SYSTEM_POWER_STATE(Name, Value) Name = Value,
#include "neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_SYSTEM_POWER_STATE
};
enum class DevicePowerRequest : uint8_t {
#define NEVERD_DEVICE_POWER_REQUEST(Name, Value) Name = Value,
#include "neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_DEVICE_POWER_REQUEST
};

enum class DriverBusKind {
#define NEVERD_DRIVER_BUS_KIND(Name, Spelling) Name,
#include "neverd/emulation/DriverPnpNames.def"
#undef NEVERD_DRIVER_BUS_KIND
};

struct DriverPnpDevice {
  std::string ID;
  DriverBusKind Bus = DriverBusKind::ResourceFree;
  std::optional<DevicePowerState> InitialDevicePower;
  std::optional<SystemPowerState> InitialSystemPower;
};

struct DriverBusCompletion {
  /// Omission is invalid; the provider never invents a completion status.
  std::optional<uint32_t> Status;
  uint64_t Delay100ns = 0;
};

struct DriverPnpOperation {
  DevicePnpRequest Minor = DevicePnpRequest::Start;
  DriverBusCompletion BusCompletion;
};

struct DriverPnpDeviceResult {
  std::string ID;
  uint64_t PDO = 0;
  std::optional<uint32_t> AddDeviceStatus;
  bool Attached = false;
  DevicePnpState PnpState = DevicePnpState::NotStarted;
  bool ProviderPresent = false;
};

struct DriverPnpRequestResult {
  DevicePnpRequest Minor = DevicePnpRequest::Start;
  DevicePnpState StateBefore = DevicePnpState::NotStarted;
  DevicePnpState StateAfter = DevicePnpState::NotStarted;
  std::optional<uint32_t> BusStatus;
  std::optional<uint64_t> BusReceivedAt100ns;
  std::optional<uint64_t> BusCompletedAt100ns;
};
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_DRIVERPNP_H
