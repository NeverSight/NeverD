//===- DeviceLifecycle.h - Concrete WDM lifecycle transactions ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Device identity and transactional PnP/power state.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DEVICELIFECYCLE_H
#define NEVERD_EMULATION_DEVICELIFECYCLE_H

#include "neverd/emulation/DriverPnp.h"

#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>

namespace neverd::emulation {

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
  bool DevicePowerQueryAccepted = false;
  bool SystemPowerQueryAccepted = false;
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
  /// Validate a final completion without publishing a lifecycle transition.
  llvm::Error validatePnpCompletion(DeviceLifecycleTicket Ticket,
                                    uint32_t Status) const;
  llvm::Error finishPnp(DeviceLifecycleTicket Ticket, uint32_t Status);
  llvm::Expected<DeviceLifecycleTicket>
  beginDevicePower(uint64_t Device, DevicePowerRequest Request,
                   DevicePowerState Target);
  llvm::Expected<DeviceLifecycleTicket>
  beginSystemPower(uint64_t Device, DevicePowerRequest Request,
                   SystemPowerState Target);
  llvm::Error validateDevicePowerCompletion(DeviceLifecycleTicket Ticket,
                                            uint32_t Status) const;
  llvm::Error validateSystemPowerCompletion(DeviceLifecycleTicket Ticket,
                                            uint32_t Status) const;
  llvm::Error finishDevicePower(DeviceLifecycleTicket Ticket, uint32_t Status);
  llvm::Error finishSystemPower(DeviceLifecycleTicket Ticket, uint32_t Status);

  /// This profile accepts new file IRPs until REMOVE begins. Other PnP/power
  /// states do not decide whether a guest dispatch queues or fails a request.
  llvm::Error validateIoSubmission(uint64_t Device) const;
  /// Register an actual IRP's lifetime independently of hardware use. Existing
  /// requests may complete while stopped, sleeping, or being removed.
  llvm::Error trackIo(uint64_t Device, uint64_t Irp);
  llvm::Error validateIoCompletion(uint64_t Device, uint64_t Irp) const;
  llvm::Error finishIo(uint64_t Device, uint64_t Irp);

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
  };
  std::map<uint64_t, Device> Devices;
  std::map<uint64_t, uint64_t> IoOwners;
  uint64_t NextSequence = 1;

  llvm::Expected<Device *> lookup(uint64_t Identity);
  llvm::Expected<const Device *> lookup(uint64_t Identity) const;
  llvm::Expected<DeviceLifecycleTicket> nextTicket(uint64_t Identity);
};

} // namespace neverd::emulation

#endif // NEVERD_EMULATION_DEVICELIFECYCLE_H
