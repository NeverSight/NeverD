//===- DeviceLifecycle.cpp - WDM PnP and power transitions ---------------===//
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
  Result.DevicePowerQueryAccepted = D.DevicePowerQueryAccepted;
  Result.SystemPowerQueryAccepted = D.SystemPowerQueryAccepted;
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
  if (const char *Message =
          devicePnpFinalStatusError(D.PnpPending->Request, Status))
    return lifecycleError(Message);
  if (!succeeded(Status))
    return llvm::Error::success();
  if (D.PnpPending->Request == DevicePnpRequest::Remove) {
    if (!D.Io.empty() || D.DevicePowerPending || D.SystemPowerPending)
      return lifecycleError("remove completion has outstanding operations");
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

llvm::Error
DeviceLifecycle::validateDevicePowerCompletion(DeviceLifecycleTicket Ticket,
                                               uint32_t Status) const {
  auto Found = lookup(Ticket.Device);
  if (!Found)
    return Found.takeError();
  const Device &D = **Found;
  if (!D.DevicePowerPending || D.DevicePowerPending->Ticket != Ticket)
    return lifecycleError("stale or mismatched device power ticket");
  if (auto E = finalStatus(Status))
    return E;
  const auto &Operation = *D.DevicePowerPending;
  if (succeeded(Status)) {
    if (absent(D.Pnp))
      return lifecycleError("power completion cannot revive a removed device");
  } else if (Operation.Request == DevicePowerRequest::Set) {
    const bool RemovingPowerUp = D.Pnp == DevicePnpState::RemovePending &&
                                 static_cast<uint32_t>(Operation.Target) <
                                     static_cast<uint32_t>(D.DevicePower);
    if (!absent(D.Pnp) && !RemovingPowerUp)
      return lifecycleError(
          "device set-power must not fail on a present device");
  }
  return llvm::Error::success();
}

llvm::Error DeviceLifecycle::finishDevicePower(DeviceLifecycleTicket Ticket,
                                               uint32_t Status) {
  if (auto E = validateDevicePowerCompletion(Ticket, Status))
    return E;
  Device &D = Devices.at(Ticket.Device);
  const auto Operation = *D.DevicePowerPending;
  if (succeeded(Status)) {
    if (Operation.Request == DevicePowerRequest::Set) {
      D.DevicePower = Operation.Target;
      D.DevicePowerQueryAccepted = false;
    } else {
      D.DevicePowerQueryAccepted = true;
    }
  }
  D.DevicePowerPending.reset();
  return llvm::Error::success();
}

llvm::Error
DeviceLifecycle::validateSystemPowerCompletion(DeviceLifecycleTicket Ticket,
                                               uint32_t Status) const {
  auto Found = lookup(Ticket.Device);
  if (!Found)
    return Found.takeError();
  const Device &D = **Found;
  if (!D.SystemPowerPending || D.SystemPowerPending->Ticket != Ticket)
    return lifecycleError("stale or mismatched system power ticket");
  if (auto E = finalStatus(Status))
    return E;
  const auto &Operation = *D.SystemPowerPending;
  // Vista+ permits completing S0 immediately after issuing the D0 request.
  // It does not consume that device transaction: its completion remains live.
  // https://learn.microsoft.com/windows-hardware/drivers/kernel/handling-a-system-set-power-irp-in-a-device-power-policy-owner
  if (D.DevicePowerPending &&
      !(Operation.Request == DevicePowerRequest::Set &&
        Operation.Target == SystemPowerState::Working &&
        D.DevicePowerPending->Request == DevicePowerRequest::Set &&
        D.DevicePowerPending->Target == DevicePowerState::D0))
    return lifecycleError(
        "system power completion has a device power operation");
  if (succeeded(Status)) {
    if (absent(D.Pnp))
      return lifecycleError("power completion cannot revive a removed device");
  } else if (Operation.Request == DevicePowerRequest::Set && !absent(D.Pnp)) {
    return lifecycleError("system set-power must not fail on a present device");
  }
  return llvm::Error::success();
}

llvm::Error DeviceLifecycle::finishSystemPower(DeviceLifecycleTicket Ticket,
                                               uint32_t Status) {
  if (auto E = validateSystemPowerCompletion(Ticket, Status))
    return E;
  Device &D = Devices.at(Ticket.Device);
  const auto Operation = *D.SystemPowerPending;
  if (succeeded(Status)) {
    if (Operation.Request == DevicePowerRequest::Set) {
      D.SystemPower = Operation.Target;
      D.SystemPowerQueryAccepted = false;
    } else {
      D.SystemPowerQueryAccepted = true;
    }
  }
  D.SystemPowerPending.reset();
  return llvm::Error::success();
}

llvm::Error DeviceLifecycle::validateIoSubmission(uint64_t Identity) const {
  auto Found = lookup(Identity);
  if (!Found)
    return Found.takeError();
  const auto State = (**Found).Pnp;
  if (State == DevicePnpState::Removing || State == DevicePnpState::Removed)
    return lifecycleError("new file I/O is unavailable after REMOVE begins");
  return llvm::Error::success();
}

llvm::Error DeviceLifecycle::trackIo(uint64_t Identity, uint64_t Irp) {
  if (auto E = validateIoSubmission(Identity))
    return E;
  auto Found = lookup(Identity);
  if (!Found)
    return Found.takeError();
  Device &D = **Found;
  if (!Irp)
    return lifecycleError("IRP identity must be nonzero");
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

} // namespace neverd::emulation
