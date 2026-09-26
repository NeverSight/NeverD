//===- KernelModelPowerRequests.cpp - Explicit WDM power packets ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Create resource-free power IRPs without inventing policy-owner requests or
/// per-object notification history. The guest and provider complete real IRPs.
///
//===----------------------------------------------------------------------===//

#include "../DriverScenario.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error powerError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "WDM power: " + Message);
}
} // namespace

llvm::Expected<KernelModel::Invocation>
KernelModel::preparePowerRequest(const DriverRequest &Input, size_t Index,
                                 std::optional<RequestedPower> Child) {
  if (Input.Kind != DriverRequestKind::Power || !Input.Power || Input.Pnp ||
      Input.DeviceID.empty() || !Input.Device.empty() || Input.File ||
      Input.ControlCode || !Input.Input.empty() || !Input.DirectInput.empty() ||
      Input.OutputSize || Input.ByteOffset || Input.CancelAfter100ns)
    return powerError("power requires a device_id and operation without file "
                      "or PnP fields");
  if ((Child && Index != Result.Requests.size()) ||
      (!Child && Index >= Result.Requests.size()))
    return powerError("power request lost its independent result identity");
  if (Unloading || Unloaded || CurrentIRQL != scheduler::PassiveLevel)
    return powerError("pageable power dispatch requires PASSIVE_LEVEL before "
                      "unload");
  const auto &Operation = *Input.Power;
  if (auto E = validateDriverPowerOperation(Operation, bool(Child)))
    return E;
  const auto Found = PnpDevices.find(Input.DeviceID);
  if (Found == PnpDevices.end() || !Found->second.AddDeviceStatus ||
      (*Found->second.AddDeviceStatus & profile::NTStatusFailureMask) ||
      !isProviderDevice(Found->second.PDO))
    return powerError("request requires a successfully added live device_id");
  const uint64_t PDO = Found->second.PDO;
  auto State = Lifecycle.snapshot(PDO);
  if (!State)
    return State.takeError();
  auto Top = topAttachedDevice(PDO);
  if (!Top)
    return Top.takeError();
  auto Route = deviceStack(*Top);
  if (!Route)
    return Route.takeError();
  if (Child && std::find(Route->begin(), Route->end(), Child->RequestDevice) ==
                   Route->end())
    return powerError("PoRequestPowerIrp requires its original device in the "
                      "live PDO route");
  for (uint64_t Device : *Route) {
    if (Devices.at(Device).DeletePending)
      return powerError("power route requires live devices");
    auto Flags = Memory.readInteger(Device + DeviceFlagsOffset, 4);
    if (!Flags)
      return Flags.takeError();
    if (!(*Flags & DevicePowerPageable) || (*Flags & DevicePowerInrush))
      return powerError("power route requires DO_POWER_PAGABLE without "
                        "DO_POWER_INRUSH in this profile");
  }
  // This is consistency with a known transaction, not an inferred parent or
  // lifetime link. The FIFO still owns the child's independent packet facts.
  if (Child && Operation.State == uint32_t(DevicePowerState::D3)) {
    bool ActiveSystem = false;
    for (const auto &[IRP, Request] : Requests) {
      (void)IRP;
      if (Request.PnpDevice == PDO && !Request.Completed &&
          Request.PowerOperation &&
          Request.PowerOperation->Type == DriverPowerType::System) {
        ActiveSystem = true;
        if (Request.PowerOperation->Action != Operation.Action)
          return powerError("device child action differs from the active "
                            "system power transaction");
      }
    }
    if (!ActiveSystem && Operation.Minor == DevicePowerRequest::Query &&
        Operation.Action != DriverPowerAction::None)
      return powerError("standalone device query requires PowerActionNone");
  }
  auto Count = Memory.readInteger(*Top + DeviceStackCountOffset, 1);
  if (!Count)
    return Count.takeError();
  if (!*Count || *Count > MaxIRPStackCount || *Count < Route->size())
    return powerError("power route requires a positive bounded stack count");
  enum class RequestMajor {
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Major) Name = Major,
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
  };
  const uint64_t PC =
      Result.MajorFunctions[static_cast<unsigned>(RequestMajor::Power)];
  const bool FrameworkPower = Framework && FrameworkDevices.count(*Top);
  if (FrameworkPower &&
      ((Child && Child->Origin != DriverRequestOrigin::FrameworkPowerPolicy) ||
       Route->size() != 2 || Route->back() != PDO))
    return powerError("framework power requires an explicit FDO/PDO request");
  if (*Top != PDO && !PC && !FrameworkPower)
    return powerError("attached driver did not register DispatchPower");
  auto Packet = allocate(IRPSize + *Count * StackSize);
  if (!Packet)
    return Packet.takeError();
  if (Child) {
    Child->CallbackReturned = !Child->Callback;
    if (Child->Callback) {
      auto Block = allocate(PowerStatusBlockSize);
      if (!Block)
        return Block.takeError();
      Child->StatusBlock = *Block;
    }
  }
  auto Ticket =
      Operation.Type == DriverPowerType::Device
          ? Lifecycle.beginDevicePower(PDO, Operation.Minor,
                                       DevicePowerState(Operation.State))
          : Lifecycle.beginSystemPower(PDO, Operation.Minor,
                                       SystemPowerState(Operation.State));
  if (!Ticket)
    return Ticket.takeError();
  ActiveRequest Record{Input.Kind, Index};
  Record.IRP = *Packet;
  Record.Device = PDO;
  Record.PnpDevice = PDO;
  Record.PowerTicket = *Ticket;
  Record.PowerOperation = Operation;
  Record.ChildPower = Child;
  Record.StackCount = *Count;
  Record.Stack = *Packet + IRPSize + (*Count - 1) * StackSize;
  Record.DeviceRoute = std::move(*Route);
  Record.UnwoundPending.resize(*Count);
  auto &Request = Requests.emplace(*Packet, std::move(Record)).first->second;
  for (uint64_t Device : Request.DeviceRoute)
    if (auto E = retainDevice(Device))
      return E;
  if (auto E = initializeRequestPacket(Request, Input))
    return E;
  if (Child) {
    DriverRequestResult Observation;
    Observation.Kind = DriverRequestKind::Power;
    Observation.DeviceID = Input.DeviceID;
    Observation.Origin = Child->Origin;
    Observation.ResponseIndex = Child->ResponseIndex;
    Result.Requests.push_back(std::move(Observation));
  }
  auto &Observation = Result.Requests[Index];
  Observation.IRP = *Packet;
  DriverPowerRequestResult Power;
  Power.Minor = Operation.Minor;
  Power.Type = Operation.Type;
  Power.State = Operation.State;
  Power.SystemContext = Operation.SystemContext;
  Power.Action = Operation.Action;
  Power.DeviceStateBefore = Power.DeviceStateAfter = State->DevicePower;
  Power.SystemStateBefore = Power.SystemStateAfter = State->SystemPower;
  if (Child)
    Power.RequestedDeviceObject = Child->RequestDevice;
  Observation.Power = Power;
  Invocation Call{*Top == PDO || FrameworkPower ? 0 : PC, *Top, *Packet};
  Call.IRP = *Packet;
  return Call;
}

llvm::Expected<KernelModel::Invocation>
KernelModel::beginPowerRequest(const DriverRequest &Input, size_t Index) {
  auto Call = preparePowerRequest(Input, Index);
  if (!Call)
    return Call.takeError();
  if (!Call->PC) {
    auto Status = dispatchPreparedPowerRequest(*Call);
    if (!Status)
      return Status.takeError();
    if (auto E = recordDispatchReturn(Call->IRP, *Status))
      return E;
  }
  return *Call;
}

llvm::Expected<uint32_t>
KernelModel::dispatchPreparedPowerRequest(const Invocation &Call) {
  auto *Request = requestForIRP(Call.IRP);
  if (!Request || !Request->PowerOperation || Call.PC)
    return powerError(
        "framework/provider dispatch lost its prepared power IRP");
  const auto Operation = *Request->PowerOperation;
  const bool FrameworkPower =
      Framework && FrameworkDevices.count(Call.Argument0);
  if (FrameworkPower && Operation.Minor == DevicePowerRequest::Set &&
      Operation.Type == DriverPowerType::Device &&
      Operation.State != uint32_t(DevicePowerState::D0)) {
    const auto Power = *Result.Requests[Request->ResultIndex].Power;
    auto Deferred = Framework->beginDevicePowerTransition(
        Request->PnpDevice, Call.IRP, Power.DeviceStateBefore,
        DevicePowerState(Power.State));
    if (!Deferred)
      return Deferred.takeError();
    if (*Deferred) {
      Request->FrameworkTransitionAwaiting = true;
      Request->FrameworkTransitionBeforeBus = true;
      if (auto E = markRequestPending(Call.IRP))
        return E;
      return StatusPending;
    }
  }
  auto Status = FrameworkPower ? forwardFrameworkTransitionRequest(Call.IRP)
                               : callProviderDriver(Call.Argument0, Call.IRP);
  if (!Status)
    return Status.takeError();
  return Request->FrameworkTransitionAwaiting ? StatusPending
                                              : uint32_t(*Status);
}

llvm::Expected<bool> KernelModel::beginFrameworkPowerPolicy(uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  if (!Framework || !Request || !Request->PowerOperation ||
      Request->PowerOperation->Type != DriverPowerType::System ||
      Request->FrameworkPolicyIssued || Request->DeviceRoute.empty() ||
      !FrameworkDevices.count(Request->DeviceRoute.front()))
    return false;
  auto Owner = Framework->ownsPowerPolicy(Request->DeviceRoute.front());
  if (!Owner)
    return Owner.takeError();
  if (!*Owner)
    return false;
  const auto System = *Request->PowerOperation;
  const auto Target = System.State == uint32_t(SystemPowerState::Working)
                          ? DevicePowerState::D0
                          : DevicePowerState::D3;
  auto *Provider = pnpDeviceForPDO(Request->PnpDevice);
  if (!Provider ||
      Provider->RequestedPowerIndex >= Provider->RequestedDevicePower.size())
    return powerError("framework power policy requires an explicit "
                      "requested_device_power response");
  const auto Index = Provider->RequestedPowerIndex;
  const auto Operation = Provider->RequestedDevicePower[Index];
  if (Operation.Type != DriverPowerType::Device ||
      Operation.Minor != System.Minor || Operation.State != uint32_t(Target))
    return powerError("framework power policy does not match the next "
                      "requested_device_power response");
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Power;
  Input.DeviceID = Result.PnpDevices[Provider->ResultIndex].ID;
  Input.Power = Operation;
  RequestedPower Child;
  Child.RequestDevice = Request->DeviceRoute.front();
  Child.ResponseIndex = Index;
  Child.Origin = DriverRequestOrigin::FrameworkPowerPolicy;
  Child.FrameworkParent = Target == DevicePowerState::D0 ? 0 : IRP;
  auto Call = preparePowerRequest(Input, Result.Requests.size(), Child);
  if (!Call)
    return Call.takeError();
  ++Provider->RequestedPowerIndex;
  Request->FrameworkPolicyIssued = true;
  // S0 may complete after the independent D0 transaction is issued. S3 must
  // retain its original packet until the matching device transaction finishes.
  if (Target == DevicePowerState::D0) {
    if (auto E = completeRequest(IRP, 0))
      return E;
  } else {
    Request->FrameworkTransitionAwaiting = true;
    if (auto E = markRequestPending(IRP))
      return E;
  }
  auto Status = dispatchPreparedPowerRequest(*Call);
  if (!Status)
    return Status.takeError();
  if (auto E = recordDispatchReturn(Call->IRP, *Status))
    return E;
  if (auto E = tryFinalizePowerRequest(Call->IRP))
    return E;
  return true;
}

llvm::Error
KernelModel::validatePowerRequestCompletion(const ActiveRequest &Request,
                                            uint32_t Status) const {
  if (!Request.PowerTicket || !Request.PowerOperation)
    return powerError("completion lost its power transaction");
  return Request.PowerOperation->Type == DriverPowerType::Device
             ? Lifecycle.validateDevicePowerCompletion(*Request.PowerTicket,
                                                       Status)
             : Lifecycle.validateSystemPowerCompletion(*Request.PowerTicket,
                                                       Status);
}

llvm::Expected<uint64_t>
KernelModel::setPowerState(llvm::ArrayRef<uint64_t> A) {
  const auto Device = Devices.find(A[0]);
  if (Device == Devices.end() || Device->second.DeletePending ||
      Device->second.OwnerKind != DeviceOwnerKind::Guest ||
      !Device->second.PnpDevice)
    return powerError("PoSetPowerState requires the driver's live PnP device");
  if (uint32_t(A[1]) != uint32_t(DriverPowerType::Device))
    return powerError("PoSetPowerState requires DevicePowerState type");
  const auto State = DevicePowerState(uint32_t(A[2]));
  if (State != DevicePowerState::D0 && State != DevicePowerState::D3)
    return powerError("PoSetPowerState supports D0 and D3 in this profile");
  if (State != DevicePowerState::D0 && CurrentIRQL > APCLevel)
    return powerError("PoSetPowerState below D0 requires IRQL <= APC_LEVEL");
  auto &Reported = Device->second.ReportedDevicePower;
  if (!Reported)
    return powerError("PoSetPowerState requires explicit "
                      "initial_reported_device_power");
  const uint32_t Previous = uint32_t(*Reported);
  Reported = State;
  if (auto E = snapshot())
    return E;
  return Previous;
}

llvm::Error KernelModel::startNextPowerIrp(uint64_t IRP) {
  const auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed ||
      Request->Kind != DriverRequestKind::Power)
    return powerError("PoStartNextPowerIrp requires an owned live power IRP");
  // The Vista+ DDI has no power serialization operation. Do not add a token,
  // consume a response, advance virtual time, or require a per-stack handshake.
  return llvm::Error::success();
}
} // namespace neverd::emulation
