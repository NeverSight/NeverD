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

#include "neverd/emulation/DriverInterrupts.h"
#include "neverd/emulation/DriverResources.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

/// Return nullptr for a modeled final status, or a stable diagnostic. Scenario
/// preflight and final guest completion share this decision; neither may accept
/// a completion that requires an unmodeled PnP manager operation.
const char *devicePnpFinalStatusError(DevicePnpRequest Request,
                                      uint32_t Status);

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

enum class DriverPowerType : uint32_t {
#define NEVERD_DRIVER_POWER_TYPE(Name, Value, Spelling) Name = Value,
#include "neverd/emulation/DriverPower.def"
#undef NEVERD_DRIVER_POWER_TYPE
};

enum class DriverPowerAction : uint32_t {
#define NEVERD_DRIVER_POWER_ACTION(Name, Value, Spelling) Name = Value,
#include "neverd/emulation/DriverPower.def"
#undef NEVERD_DRIVER_POWER_ACTION
};

enum class DriverRequestOrigin {
#define NEVERD_DRIVER_REQUEST_ORIGIN(Name, Spelling) Name,
#include "neverd/emulation/DriverPower.def"
#undef NEVERD_DRIVER_REQUEST_ORIGIN
};

struct DriverBusCompletion {
  /// Omission is invalid; the provider never invents a completion status.
  std::optional<uint32_t> Status;
  uint64_t Delay100ns = 0;
};

struct DriverPowerOperation {
  DevicePowerRequest Minor = DevicePowerRequest::Set;
  DriverPowerType Type = DriverPowerType::Device;
  /// Interpreted only according to Type; zero is not a supported target.
  uint32_t State = 0;
  /// Explicit scenario packet facts, never inferred from another request.
  uint32_t SystemContext = 0;
  DriverPowerAction Action = DriverPowerAction::None;
  DriverBusCompletion BusCompletion;
};

struct DriverPnpDevice {
  std::string ID;
  DriverBusKind Bus = DriverBusKind::ResourceFree;
  std::optional<DevicePowerState> InitialDevicePower;
  std::optional<SystemPowerState> InitialSystemPower;
  /// Independent notification state for each newly associated device object.
  /// Absence is unknown, not an assumed D0 or a copy of lifecycle state.
  std::optional<DevicePowerState> InitialReportedDevicePower = std::nullopt;
  /// Per-PDO FIFO consumed only by matching real PoRequestPowerIrp calls.
  std::vector<DriverPowerOperation> RequestedDevicePower{};
  /// Fixed ordered raw/translated assignments for the register_bank provider.
  /// The resource_free provider requires an empty inventory.
  std::vector<DriverMemoryResource> Resources{};
  /// Fixed interrupt assignments, following memory entries in each CM list.
  std::vector<DriverInterruptResource> Interrupts{};
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
  DevicePowerState DevicePower = DevicePowerState::D0;
  SystemPowerState SystemPower = SystemPowerState::Working;
};

struct DriverPnpRequestResult {
  DevicePnpRequest Minor = DevicePnpRequest::Start;
  DevicePnpState StateBefore = DevicePnpState::NotStarted;
  DevicePnpState StateAfter = DevicePnpState::NotStarted;
  std::optional<uint32_t> BusStatus;
  std::optional<uint64_t> BusReceivedAt100ns;
  std::optional<uint64_t> BusCompletedAt100ns;
};

struct DriverPowerRequestResult {
  DevicePowerRequest Minor = DevicePowerRequest::Set;
  DriverPowerType Type = DriverPowerType::Device;
  uint32_t State = 0;
  uint32_t SystemContext = 0;
  DriverPowerAction Action = DriverPowerAction::None;
  DevicePowerState DeviceStateBefore = DevicePowerState::D0;
  DevicePowerState DeviceStateAfter = DevicePowerState::D0;
  SystemPowerState SystemStateBefore = SystemPowerState::Working;
  SystemPowerState SystemStateAfter = SystemPowerState::Working;
  std::optional<uint32_t> BusStatus;
  std::optional<uint64_t> BusReceivedAt100ns;
  std::optional<uint64_t> BusCompletedAt100ns;
  /// Original API device argument, independent of canonical PDO identity.
  std::optional<uint64_t> RequestedDeviceObject;
};
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_DRIVERPNP_H
