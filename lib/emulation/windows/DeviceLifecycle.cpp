//===- DeviceLifecycle.cpp - WDM PnP and power state transitions
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original implementation of documented lifecycle contracts:
/// https://learn.microsoft.com/windows-hardware/drivers/kernel/state-transitions-for-pnp-devices
/// https://learn.microsoft.com/windows-hardware/drivers/kernel/irp-mn-start-device
/// https://learn.microsoft.com/windows-hardware/drivers/kernel/understanding-when-remove-irps-are-issued
/// https://learn.microsoft.com/windows-hardware/drivers/kernel/handling-an-irp-mn-cancel-remove-device-request
/// https://learn.microsoft.com/windows-hardware/drivers/kernel/stopping-a-device-to-rebalance-resources
/// https://learn.microsoft.com/windows-hardware/drivers/kernel/irp-mn-set-power
/// https://learn.microsoft.com/windows-hardware/drivers/kernel/handling-irp-mn-query-power-for-system-power-states
/// https://learn.microsoft.com/windows-hardware/drivers/kernel/calling-iocalldriver-versus-calling-pocalldriver
/// https://learn.microsoft.com/windows-hardware/drivers/kernel/dispatchpower-routines
/// https://learn.microsoft.com/windows-hardware/drivers/kernel/using-remove-locks
///
//===----------------------------------------------------------------------===//

#include "DeviceLifecycle.h"

#include <limits>

namespace neverd::emulation {
namespace {
#define NEVERD_DEVICE_LIFECYCLE_LIMIT(Name, Value)                             \
  constexpr size_t Name = Value;
#include "neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_DEVICE_LIFECYCLE_LIMIT

llvm::Error lifecycleError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "device lifecycle: " + Message);
}

bool valid(DevicePnpRequest Request) {
  switch (Request) {
#define NEVERD_DEVICE_PNP_REQUEST(Name, Value, MayFail)                        \
  case DevicePnpRequest::Name:                                                 \
    return true;
#include "neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_DEVICE_PNP_REQUEST
  }
  return false;
}

bool mayFail(DevicePnpRequest Request) {
  switch (Request) {
#define NEVERD_DEVICE_PNP_REQUEST(Name, Value, MayFail)                        \
  case DevicePnpRequest::Name:                                                 \
    return MayFail;
#include "neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_DEVICE_PNP_REQUEST
  }
  return false;
}

bool valid(DevicePowerState State) {
  switch (State) {
#define NEVERD_DEVICE_POWER_STATE(Name, Value)                                 \
  case DevicePowerState::Name:                                                 \
    return true;
#include "neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_DEVICE_POWER_STATE
  }
  return false;
}

bool valid(SystemPowerState State) {
  switch (State) {
#define NEVERD_SYSTEM_POWER_STATE(Name, Value)                                 \
  case SystemPowerState::Name:                                                 \
    return true;
#include "neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_SYSTEM_POWER_STATE
  }
  return false;
}

bool valid(DevicePowerRequest Request) {
  switch (Request) {
#define NEVERD_DEVICE_POWER_REQUEST(Name, Value)                               \
  case DevicePowerRequest::Name:                                               \
    return true;
#include "neverd/emulation/DeviceLifecycle.def"
#undef NEVERD_DEVICE_POWER_REQUEST
  }
  return false;
}

bool absent(DevicePnpState State) {
  return State == DevicePnpState::SurpriseRemoved ||
         State == DevicePnpState::Removing || State == DevicePnpState::Removed;
}

bool succeeded(uint32_t Status) { return (Status & 0x80000000u) == 0; }

llvm::Error finalStatus(uint32_t Status) {
  if (Status == 0x103)
    return lifecycleError("STATUS_PENDING is not a final completion status");
  return llvm::Error::success();
}
} // namespace

llvm::Expected<DeviceLifecycle::Device *>
DeviceLifecycle::lookup(uint64_t Identity) {
  auto It = Devices.find(Identity);
  if (It == Devices.end())
    return lifecycleError("unknown device identity");
  return &It->second;
}

llvm::Expected<const DeviceLifecycle::Device *>
DeviceLifecycle::lookup(uint64_t Identity) const {
  auto It = Devices.find(Identity);
  if (It == Devices.end())
    return lifecycleError("unknown device identity");
  return &It->second;
}

llvm::Expected<DeviceLifecycleTicket>
DeviceLifecycle::nextTicket(uint64_t Identity) {
  if (NextSequence == std::numeric_limits<uint64_t>::max())
    return lifecycleError("operation ticket limit exhausted");
  return DeviceLifecycleTicket{Identity, NextSequence++};
}

llvm::Error DeviceLifecycle::addDevice(uint64_t Identity,
                                       DevicePowerState InitialDevicePower,
                                       SystemPowerState InitialSystemPower,
                                       bool BootConfigured) {
  if (!Identity)
    return lifecycleError("device identity must be nonzero");
  if (!valid(InitialDevicePower) || !valid(InitialSystemPower))
    return lifecycleError("invalid initial power state");
  if (Devices.contains(Identity))
    return lifecycleError("device identity already exists");
  if (Devices.size() >= MaxDevices)
    return lifecycleError("device count exceeds the limit");
  Device D;
  D.DevicePower = InitialDevicePower;
  D.SystemPower = InitialSystemPower;
  D.BootConfigured = BootConfigured;
  Devices.emplace(Identity, std::move(D));
  return llvm::Error::success();
}

bool DeviceLifecycle::canStartIo(const Device &D) {
  return D.Pnp == DevicePnpState::Started &&
         D.DevicePower == DevicePowerState::D0 &&
         D.SystemPower == SystemPowerState::Working && !D.PnpPending &&
         !D.DevicePowerPending && !D.SystemPowerPending &&
         !D.DevicePowerQueryAccepted && !D.SystemPowerQueryAccepted;
}

llvm::Expected<DeviceLifecycleSnapshot>
DeviceLifecycle::snapshot(uint64_t Identity) const {
  auto Found = lookup(Identity);
  if (!Found)
    return Found.takeError();
  const Device &D = **Found;
  DeviceLifecycleSnapshot Result;
  Result.Device = Identity;
  Result.Pnp = D.Pnp;
  Result.DevicePower = D.DevicePower;
  Result.SystemPower = D.SystemPower;
  if (D.PnpPending)
    Result.PnpOperation = D.PnpPending->Ticket;
  if (D.DevicePowerPending)
    Result.DevicePowerOperation = D.DevicePowerPending->Ticket;
  if (D.SystemPowerPending)
    Result.SystemPowerOperation = D.SystemPowerPending->Ticket;
  Result.OutstandingIo = D.Io.size();
  Result.RemoveLocks = D.Locks.size();
  Result.RemoveLockReferences = D.LockReferences;
  Result.DevicePowerQueryAccepted = D.DevicePowerQueryAccepted;
  Result.SystemPowerQueryAccepted = D.SystemPowerQueryAccepted;
  Result.CanStartIo = canStartIo(D);
  return Result;
}

llvm::Expected<DeviceLifecycleTicket>
DeviceLifecycle::beginPnp(uint64_t Identity, DevicePnpRequest Request) {
  if (!valid(Request))
    return lifecycleError("unsupported PnP request");
  auto Found = lookup(Identity);
  if (!Found)
    return Found.takeError();
  Device &D = **Found;
  if (D.PnpPending)
    return lifecycleError("a PnP operation is already active");
  if (D.Pnp == DevicePnpState::Removed || D.Pnp == DevicePnpState::Removing)
    return lifecycleError("device removal is terminal");
  if (D.Pnp == DevicePnpState::SurpriseRemoved &&
      Request != DevicePnpRequest::Remove)
    return lifecycleError("only remove can follow surprise removal");

  bool Allowed = false;
  switch (Request) {
  case DevicePnpRequest::Start:
    Allowed = D.Pnp == DevicePnpState::NotStarted ||
              D.Pnp == DevicePnpState::Started ||
              D.Pnp == DevicePnpState::Stopped;
    break;
  case DevicePnpRequest::QueryStop:
    Allowed = D.Pnp == DevicePnpState::Started ||
              (D.Pnp == DevicePnpState::NotStarted && D.BootConfigured);
    break;
  case DevicePnpRequest::CancelStop:
    Allowed = D.Pnp == DevicePnpState::StopPending ||
              D.Pnp == DevicePnpState::Started ||
              (D.Pnp == DevicePnpState::NotStarted && D.BootConfigured);
    break;
  case DevicePnpRequest::Stop:
    Allowed = D.Pnp == DevicePnpState::StopPending;
    break;
  case DevicePnpRequest::QueryRemove:
    Allowed = D.Pnp == DevicePnpState::NotStarted ||
              D.Pnp == DevicePnpState::Started ||
              D.Pnp == DevicePnpState::Stopped;
    break;
  case DevicePnpRequest::CancelRemove:
    Allowed = D.Pnp == DevicePnpState::RemovePending ||
              D.Pnp == DevicePnpState::NotStarted ||
              D.Pnp == DevicePnpState::Started ||
              D.Pnp == DevicePnpState::Stopped;
    break;
  case DevicePnpRequest::SurpriseRemoval:
  case DevicePnpRequest::Remove:
    // Removal can occur at any time after AddDevice, including failed start.
    Allowed = true;
    break;
  }
  if (!Allowed)
    return lifecycleError("PnP request is invalid in the current state");
  auto Ticket = nextTicket(Identity);
  if (!Ticket)
    return Ticket.takeError();
  D.PnpPending = PnpOperation{*Ticket, Request, D.Pnp};
  switch (Request) {
  case DevicePnpRequest::QueryStop:
    D.BeforeStop = D.Pnp;
    D.Pnp = DevicePnpState::StopPending;
    break;
  case DevicePnpRequest::QueryRemove:
    D.BeforeRemove = D.Pnp;
    D.Pnp = DevicePnpState::RemovePending;
    break;
  case DevicePnpRequest::SurpriseRemoval:
    D.Pnp = DevicePnpState::SurpriseRemoved;
    break;
  case DevicePnpRequest::Remove:
    D.Pnp = DevicePnpState::Removing;
    break;
  default:
    break;
  }
  return *Ticket;
}

llvm::Error DeviceLifecycle::validatePnpCompletion(DeviceLifecycleTicket Ticket,
                                                   uint32_t Status) const {
  auto Found = lookup(Ticket.Device);
  if (!Found)
    return Found.takeError();
  const Device &D = **Found;
  if (!D.PnpPending || D.PnpPending->Ticket != Ticket)
    return lifecycleError("stale or mismatched PnP ticket");
  if (auto E = finalStatus(Status))
    return E;
  if (!succeeded(Status)) {
    if (!mayFail(D.PnpPending->Request))
      return lifecycleError("this PnP request must not fail");
    return llvm::Error::success();
  }
  if (devicePnpRequiresSuccess(D.PnpPending->Request) && Status != 0)
    return lifecycleError("this PnP request requires STATUS_SUCCESS");
  if (D.PnpPending->Request == DevicePnpRequest::Remove) {
    if (!D.Io.empty() || D.LockReferences || D.DevicePowerPending ||
        D.SystemPowerPending)
      return lifecycleError("remove completion has outstanding operations");
    for (const auto &[Identity, Lock] : D.Locks)
      if (!Lock.Draining)
        return lifecycleError("remove completion has an undrained remove lock");
  }
  return llvm::Error::success();
}

llvm::Error DeviceLifecycle::finishPnp(DeviceLifecycleTicket Ticket,
                                       uint32_t Status) {
  if (auto E = validatePnpCompletion(Ticket, Status))
    return E;
  Device &D = Devices.at(Ticket.Device);
  const PnpOperation Operation = *D.PnpPending;
  if (!succeeded(Status)) {
    D.Pnp = Operation.Previous;
    D.PnpPending.reset();
    return llvm::Error::success();
  }
  switch (Operation.Request) {
  case DevicePnpRequest::Start:
    D.Pnp = DevicePnpState::Started;
    break;
  case DevicePnpRequest::CancelStop:
    if (Operation.Previous == DevicePnpState::StopPending)
      D.Pnp = D.BeforeStop;
    break;
  case DevicePnpRequest::Stop:
    D.Pnp = DevicePnpState::Stopped;
    D.BootConfigured = false;
    break;
  case DevicePnpRequest::CancelRemove:
    if (Operation.Previous == DevicePnpState::RemovePending)
      D.Pnp = D.BeforeRemove;
    break;
  case DevicePnpRequest::Remove:
    D.Pnp = DevicePnpState::Removed;
    break;
  case DevicePnpRequest::QueryStop:
  case DevicePnpRequest::QueryRemove:
  case DevicePnpRequest::SurpriseRemoval:
    break;
  }
  D.PnpPending.reset();
  return llvm::Error::success();
}

llvm::Expected<DeviceLifecycleTicket>
DeviceLifecycle::beginDevicePower(uint64_t Identity, DevicePowerRequest Request,
                                  DevicePowerState Target) {
  if (!valid(Request) || !valid(Target))
    return lifecycleError("unsupported device power request or state");
  auto Found = lookup(Identity);
  if (!Found)
    return Found.takeError();
  Device &D = **Found;
  if (absent(D.Pnp))
    return lifecycleError("device is being removed");
  if (D.DevicePowerPending)
    return lifecycleError("a device power operation is already active");
  if (Request == DevicePowerRequest::Set && D.SystemPowerPending &&
      D.SystemPowerPending->Request == DevicePowerRequest::Query)
    return lifecycleError("device set-power cannot satisfy a system query");
  auto Ticket = nextTicket(Identity);
  if (!Ticket)
    return Ticket.takeError();
  D.DevicePowerPending =
      PowerOperation<DevicePowerState>{*Ticket, Request, Target};
  return *Ticket;
}

llvm::Expected<DeviceLifecycleTicket>
DeviceLifecycle::beginSystemPower(uint64_t Identity, DevicePowerRequest Request,
                                  SystemPowerState Target) {
  if (!valid(Request) || !valid(Target))
    return lifecycleError("unsupported system power request or state");
  if (Request == DevicePowerRequest::Query &&
      Target == SystemPowerState::Working)
    return lifecycleError("system power-up does not use a query");
  auto Found = lookup(Identity);
  if (!Found)
    return Found.takeError();
  Device &D = **Found;
  if (absent(D.Pnp))
    return lifecycleError("device is being removed");
  if (D.SystemPowerPending)
    return lifecycleError("a system power operation is already active");
  auto Ticket = nextTicket(Identity);
  if (!Ticket)
    return Ticket.takeError();
  D.SystemPowerPending =
      PowerOperation<SystemPowerState>{*Ticket, Request, Target};
  return *Ticket;
}

llvm::Error DeviceLifecycle::finishDevicePower(DeviceLifecycleTicket Ticket,
                                               uint32_t Status) {
  auto Found = lookup(Ticket.Device);
  if (!Found)
    return Found.takeError();
  Device &D = **Found;
  if (!D.DevicePowerPending || D.DevicePowerPending->Ticket != Ticket)
    return lifecycleError("stale or mismatched device power ticket");
  if (auto E = finalStatus(Status))
    return E;
  const auto Operation = *D.DevicePowerPending;
  if (succeeded(Status)) {
    if (absent(D.Pnp))
      return lifecycleError("power completion cannot revive a removed device");
    if (Operation.Request == DevicePowerRequest::Set) {
      D.DevicePower = Operation.Target;
      D.DevicePowerQueryAccepted = false;
    } else {
      D.DevicePowerQueryAccepted = true;
    }
  } else if (Operation.Request == DevicePowerRequest::Set) {
    const bool RemovingPowerUp = D.Pnp == DevicePnpState::RemovePending &&
                                 static_cast<uint32_t>(Operation.Target) <
                                     static_cast<uint32_t>(D.DevicePower);
    if (!absent(D.Pnp) && !RemovingPowerUp)
      return lifecycleError(
          "device set-power must not fail on a present device");
  }
  D.DevicePowerPending.reset();
  return llvm::Error::success();
}

llvm::Error DeviceLifecycle::finishSystemPower(DeviceLifecycleTicket Ticket,
                                               uint32_t Status) {
  auto Found = lookup(Ticket.Device);
  if (!Found)
    return Found.takeError();
  Device &D = **Found;
  if (!D.SystemPowerPending || D.SystemPowerPending->Ticket != Ticket)
    return lifecycleError("stale or mismatched system power ticket");
  if (auto E = finalStatus(Status))
    return E;
  if (D.DevicePowerPending)
    return lifecycleError(
        "system power completion has a device power operation");
  const auto Operation = *D.SystemPowerPending;
  if (succeeded(Status)) {
    if (absent(D.Pnp))
      return lifecycleError("power completion cannot revive a removed device");
    if (Operation.Request == DevicePowerRequest::Set) {
      D.SystemPower = Operation.Target;
      D.SystemPowerQueryAccepted = false;
    } else {
      D.SystemPowerQueryAccepted = true;
    }
  } else if (Operation.Request == DevicePowerRequest::Set && !absent(D.Pnp)) {
    return lifecycleError("system set-power must not fail on a present device");
  }
  D.SystemPowerPending.reset();
  return llvm::Error::success();
}

llvm::Error DeviceLifecycle::beginIo(uint64_t Identity, uint64_t Irp) {
  auto Found = lookup(Identity);
  if (!Found)
    return Found.takeError();
  Device &D = **Found;
  if (!Irp)
    return lifecycleError("IRP identity must be nonzero");
  if (!canStartIo(D))
    return lifecycleError("device is not ready to start I/O");
  if (IoOwners.contains(Irp))
    return lifecycleError("IRP identity already has an outstanding operation");
  if (IoOwners.size() >= MaxOutstandingIo)
    return lifecycleError("outstanding IRP count exceeds the limit");
  IoOwners.emplace(Irp, Identity);
  D.Io.insert(Irp);
  return llvm::Error::success();
}

llvm::Error DeviceLifecycle::validateIoCompletion(uint64_t Identity,
                                                  uint64_t Irp) const {
  auto Found = lookup(Identity);
  if (!Found)
    return Found.takeError();
  if (!(**Found).Io.contains(Irp))
    return lifecycleError("IRP is not outstanding for this device");
  return llvm::Error::success();
}

llvm::Error DeviceLifecycle::finishIo(uint64_t Identity, uint64_t Irp) {
  if (auto E = validateIoCompletion(Identity, Irp))
    return E;
  Devices.at(Identity).Io.erase(Irp);
  IoOwners.erase(Irp);
  return llvm::Error::success();
}

llvm::Error DeviceLifecycle::initializeRemoveLock(uint64_t Identity,
                                                  uint64_t Lock) {
  auto Found = lookup(Identity);
  if (!Found)
    return Found.takeError();
  Device &D = **Found;
  if (!Lock)
    return lifecycleError("remove-lock identity must be nonzero");
  if (absent(D.Pnp))
    return lifecycleError("cannot initialize a remove lock during removal");
  if (LockOwners.contains(Lock))
    return lifecycleError("remove lock is already initialized");
  if (LockOwners.size() >= MaxRemoveLocks)
    return lifecycleError("remove-lock count exceeds the limit");
  LockOwners.emplace(Lock, Identity);
  D.Locks.emplace(Lock, RemoveLock{});
  return llvm::Error::success();
}

llvm::Error DeviceLifecycle::acquireRemoveLock(uint64_t Identity, uint64_t Lock,
                                               uint64_t Tag) {
  auto Found = lookup(Identity);
  if (!Found)
    return Found.takeError();
  Device &D = **Found;
  auto It = D.Locks.find(Lock);
  if (It == D.Locks.end())
    return lifecycleError("remove lock is not owned by this device");
  if (It->second.Draining || D.Pnp == DevicePnpState::Removed)
    return lifecycleError("remove lock no longer accepts acquisitions");
  if (TotalLockReferences >= MaxRemoveLockReferences)
    return lifecycleError("remove-lock reference count exceeds the limit");
  ++It->second.Tags[Tag];
  ++D.LockReferences;
  ++TotalLockReferences;
  return llvm::Error::success();
}

llvm::Error DeviceLifecycle::releaseRemoveLock(uint64_t Identity, uint64_t Lock,
                                               uint64_t Tag) {
  auto Found = lookup(Identity);
  if (!Found)
    return Found.takeError();
  Device &D = **Found;
  auto It = D.Locks.find(Lock);
  if (It == D.Locks.end())
    return lifecycleError("remove lock is not owned by this device");
  auto Acquisition = It->second.Tags.find(Tag);
  if (Acquisition == It->second.Tags.end())
    return lifecycleError("remove lock does not hold this tag");
  if (--Acquisition->second == 0)
    It->second.Tags.erase(Acquisition);
  --D.LockReferences;
  --TotalLockReferences;
  return llvm::Error::success();
}

llvm::Expected<bool>
DeviceLifecycle::releaseRemoveLockAndWait(uint64_t Identity, uint64_t Lock,
                                          uint64_t Tag) {
  auto Found = lookup(Identity);
  if (!Found)
    return Found.takeError();
  Device &D = **Found;
  auto It = D.Locks.find(Lock);
  if (It == D.Locks.end())
    return lifecycleError("remove lock is not owned by this device");
  if (D.Pnp != DevicePnpState::Removing)
    return lifecycleError("remove-lock drain requires a remove operation");
  if (It->second.Draining)
    return lifecycleError("remove lock is already draining");
  if (auto E = releaseRemoveLock(Identity, Lock, Tag))
    return E;
  It->second.Draining = true;
  return It->second.Tags.empty();
}

llvm::Expected<bool> DeviceLifecycle::removeLockDrained(uint64_t Identity,
                                                        uint64_t Lock) const {
  auto Found = lookup(Identity);
  if (!Found)
    return Found.takeError();
  const Device &D = **Found;
  auto It = D.Locks.find(Lock);
  if (It == D.Locks.end())
    return lifecycleError("remove lock is not owned by this device");
  return It->second.Draining && It->second.Tags.empty();
}

} // namespace neverd::emulation
