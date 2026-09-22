//===- DeviceLifecycle.h - Concrete WDM lifecycle transactions ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Device identity, transactional PnP/power state and removal references.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DEVICELIFECYCLE_H
#define NEVERD_EMULATION_DEVICELIFECYCLE_H

#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>

namespace neverd::emulation {

enum class DevicePnpState : uint8_t {
#define NEVERD_DEVICE_PNP_STATE(Name, Value) Name = Value,
#include "DeviceLifecycle.def"
#undef NEVERD_DEVICE_PNP_STATE
};
enum class DevicePnpRequest : uint8_t {
#define NEVERD_DEVICE_PNP_REQUEST(Name, Value, MayFail) Name = Value,
#include "DeviceLifecycle.def"
#undef NEVERD_DEVICE_PNP_REQUEST
};
enum class DevicePowerState : uint32_t {
#define NEVERD_DEVICE_POWER_STATE(Name, Value) Name = Value,
#include "DeviceLifecycle.def"
#undef NEVERD_DEVICE_POWER_STATE
};
enum class SystemPowerState : uint32_t {
#define NEVERD_SYSTEM_POWER_STATE(Name, Value) Name = Value,
#include "DeviceLifecycle.def"
#undef NEVERD_SYSTEM_POWER_STATE
};
enum class DevicePowerRequest : uint8_t {
#define NEVERD_DEVICE_POWER_REQUEST(Name, Value) Name = Value,
#include "DeviceLifecycle.def"
#undef NEVERD_DEVICE_POWER_REQUEST
};

struct DeviceLifecycleTicket {
  uint64_t Device = 0;
  uint64_t Sequence = 0;
  bool operator==(const DeviceLifecycleTicket &) const = default;
};

struct DeviceLifecycleSnapshot {
  uint64_t Device = 0;
  DevicePnpState Pnp = DevicePnpState::NotStarted;
  DevicePowerState DevicePower = DevicePowerState::D3;
  SystemPowerState SystemPower = SystemPowerState::Working;
  std::optional<DeviceLifecycleTicket> PnpOperation;
  std::optional<DeviceLifecycleTicket> DevicePowerOperation;
  std::optional<DeviceLifecycleTicket> SystemPowerOperation;
  size_t OutstandingIo = 0;
  size_t RemoveLocks = 0;
  size_t RemoveLockReferences = 0;
  bool DevicePowerQueryAccepted = false;
  bool SystemPowerQueryAccepted = false;
  bool CanStartIo = false;
};

/// Session-local Windows 2000+ PnP state and Vista+ power sequencing.
/// Device identity is the concrete device object identity selected by the
/// owning device-stack model. No host device, resource, or power policy is
/// inferred here. Initial power states must be supplied explicitly.
///
/// Transactions remain active until the caller supplies the final NTSTATUS.
/// Pending is not a completion. Invalid completions leave the transaction
/// intact and produce a diagnostic. Fallible PnP/query failures are ordinary
/// completions and restore the previous PnP state without changing power.
///
/// This model tracks IRP lifetime, not hardware use or driver queue placement.
/// Dispatch, IRP holding/cancellation, resources, child ordering, wait/wake,
/// per-driver completion order and power capabilities belong to the caller.
class DeviceLifecycle {
public:
  llvm::Error addDevice(uint64_t Device, DevicePowerState InitialDevicePower,
                        SystemPowerState InitialSystemPower,
                        bool BootConfigured = false);
  llvm::Expected<DeviceLifecycleSnapshot> snapshot(uint64_t Device) const;

  llvm::Expected<DeviceLifecycleTicket> beginPnp(uint64_t Device,
                                                 DevicePnpRequest Request);
  llvm::Error finishPnp(DeviceLifecycleTicket Ticket, uint32_t Status);
  llvm::Expected<DeviceLifecycleTicket>
  beginDevicePower(uint64_t Device, DevicePowerRequest Request,
                   DevicePowerState Target);
  llvm::Expected<DeviceLifecycleTicket>
  beginSystemPower(uint64_t Device, DevicePowerRequest Request,
                   SystemPowerState Target);
  llvm::Error finishDevicePower(DeviceLifecycleTicket Ticket, uint32_t Status);
  llvm::Error finishSystemPower(DeviceLifecycleTicket Ticket, uint32_t Status);

  /// New dispatch is allowed only while started, in D0, and not quiesced by a
  /// PnP/power transaction or successful power query. Existing requests may
  /// complete while stopped, sleeping, surprise-removed, or removing.
  llvm::Error beginIo(uint64_t Device, uint64_t Irp);
  llvm::Error finishIo(uint64_t Device, uint64_t Irp);

  /// Remove locks have identities separate from IRPs and independently
  /// counted acquisition tags, including repeat acquisitions of one tag.
  llvm::Error initializeRemoveLock(uint64_t Device, uint64_t Lock);
  llvm::Error acquireRemoveLock(uint64_t Device, uint64_t Lock, uint64_t Tag);
  llvm::Error releaseRemoveLock(uint64_t Device, uint64_t Lock, uint64_t Tag);
  /// Releases this acquisition and closes the lock to new acquisitions.
  /// False means the caller must wait for other acquisitions to drain.
  llvm::Expected<bool> releaseRemoveLockAndWait(uint64_t Device, uint64_t Lock,
                                                uint64_t Tag);
  llvm::Expected<bool> removeLockDrained(uint64_t Device, uint64_t Lock) const;

private:
  struct PnpOperation {
    DeviceLifecycleTicket Ticket;
    DevicePnpRequest Request;
    DevicePnpState Previous;
  };
  template <class State> struct PowerOperation {
    DeviceLifecycleTicket Ticket;
    DevicePowerRequest Request;
    State Target;
  };
  struct RemoveLock {
    bool Draining = false;
    std::map<uint64_t, size_t> Tags;
  };
  struct Device {
    DevicePnpState Pnp = DevicePnpState::NotStarted;
    DevicePnpState BeforeStop = DevicePnpState::NotStarted;
    DevicePnpState BeforeRemove = DevicePnpState::NotStarted;
    DevicePowerState DevicePower;
    SystemPowerState SystemPower;
    bool BootConfigured;
    bool DevicePowerQueryAccepted = false;
    bool SystemPowerQueryAccepted = false;
    std::optional<PnpOperation> PnpPending;
    std::optional<PowerOperation<DevicePowerState>> DevicePowerPending;
    std::optional<PowerOperation<SystemPowerState>> SystemPowerPending;
    std::set<uint64_t> Io;
    std::map<uint64_t, RemoveLock> Locks;
    size_t LockReferences = 0;
  };
  std::map<uint64_t, Device> Devices;
  std::map<uint64_t, uint64_t> IoOwners;
  std::map<uint64_t, uint64_t> LockOwners;
  size_t TotalLockReferences = 0;
  uint64_t NextSequence = 1;

  llvm::Expected<Device *> lookup(uint64_t Identity);
  llvm::Expected<const Device *> lookup(uint64_t Identity) const;
  llvm::Expected<DeviceLifecycleTicket> nextTicket(uint64_t Identity);
  static bool canStartIo(const Device &D);
};

} // namespace neverd::emulation

#endif // NEVERD_EMULATION_DEVICELIFECYCLE_H
