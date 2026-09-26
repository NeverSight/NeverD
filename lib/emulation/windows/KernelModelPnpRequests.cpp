//===- KernelModelPnpRequests.cpp - Scenario PnP packets -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Dispatch explicit PnP operations through retained device routes and commit
/// lifecycle transitions only when their original IRPs finish unwinding.
///
//===----------------------------------------------------------------------===//

#include "../DriverScenario.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error pnpError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "WDM PnP: " + Message);
}
} // namespace

llvm::Expected<KernelModel::Invocation>
KernelModel::beginPnpRequest(const DriverRequest &Input, size_t Index) {
  if (!Input.Pnp || Input.Power || Input.DeviceID.empty() ||
      !Input.Device.empty() || Input.File || Input.ControlCode ||
      !Input.Input.empty() || !Input.DirectInput.empty() || Input.OutputSize ||
      Input.ByteOffset || Input.CancelAfter100ns)
    return pnpError(
        "PnP requires a device_id and operation without file fields");
  const auto &Operation = *Input.Pnp;
  const auto Minor = Operation.Minor;
  if (auto E = validateDriverPnpOperation(Operation))
    return E;
  if (CurrentIRQL != scheduler::PassiveLevel)
    return pnpError("PnP dispatch requires PASSIVE_LEVEL");
  auto Found = PnpDevices.find(Input.DeviceID);
  if (Found == PnpDevices.end() || !Found->second.AddDeviceStatus ||
      (*Found->second.AddDeviceStatus & profile::NTStatusFailureMask))
    return pnpError("request requires a successfully added device_id");
  const uint64_t PDO = Found->second.PDO;
  if (!Devices.count(PDO))
    return pnpError("request targets a removed provider device");
  auto State = Lifecycle.snapshot(PDO);
  if (!State)
    return State.takeError();
  auto Top = topAttachedDevice(PDO);
  if (!Top)
    return Top.takeError();
  auto Route = deviceStack(*Top);
  if (!Route)
    return Route.takeError();
  if (Minor == DevicePnpRequest::Remove) {
    if (State->Pnp == DevicePnpState::Started)
      return pnpError(
          "normal removal of a started device requires query-remove");
    for (const auto &[ID, File] : Files) {
      (void)ID;
      if (File.PnpDevice == PDO)
        return pnpError("remove requires all device files to close");
    }
    if (std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
          return Entry.second.PnpDevice == PDO;
        }))
      return pnpError("remove requires earlier device requests to finalize");
  }
  auto Count = Memory.readInteger(*Top + DeviceStackCountOffset, 1);
  if (!Count)
    return Count.takeError();
  if (!*Count || *Count > MaxIRPStackCount || *Count < Route->size())
    return pnpError("device route requires a positive bounded stack count");
  unsigned Major = profile::MajorFunctionCount;
  switch (Input.Kind) {
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Value)                      \
  case DriverRequestKind::Name:                                                \
    Major = Value;                                                             \
    break;
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
  }
  const uint64_t Callback = Result.MajorFunctions[Major];
  const bool FrameworkPnp = Framework && FrameworkDevices.count(*Top);
  if (*Top != PDO && !Callback && !FrameworkPnp)
    return pnpError("attached driver did not register a PnP dispatch callback");
  if (Minor == DevicePnpRequest::Start)
    if (auto E = Resources.canStart(PDO))
      return E;
  auto Packet = allocate(IRPSize + *Count * StackSize);
  if (!Packet)
    return Packet.takeError();
  auto Ticket = Lifecycle.beginPnp(PDO, Minor);
  if (!Ticket)
    return Ticket.takeError();
  if (Minor == DevicePnpRequest::Start) {
    if (auto E = Resources.beginStart(PDO))
      return E;
  } else if (Minor == DevicePnpRequest::SurpriseRemoval) {
    Resources.surpriseRemoval(PDO);
  }
  ActiveRequest Record{Input.Kind, Index};
  Record.IRP = *Packet;
  Record.Device = PDO;
  Record.PnpDevice = PDO;
  Record.PnpTicket = *Ticket;
  Record.PnpOperation = Operation;
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
  auto &Observation = Result.Requests[Index];
  Observation.IRP = *Packet;
  Observation.Pnp = DriverPnpRequestResult{};
  Observation.Pnp->Minor = Minor;
  Observation.Pnp->StateBefore = State->Pnp;
  auto ActiveState = Lifecycle.snapshot(PDO);
  if (!ActiveState)
    return ActiveState.takeError();
  Observation.Pnp->StateAfter = ActiveState->Pnp;
  if (*Top == PDO) {
    auto Status = callProviderDriver(PDO, *Packet);
    if (!Status)
      return Status.takeError();
    if (auto E = recordDispatchReturn(*Packet, uint32_t(*Status)))
      return E;
    Invocation Call;
    Call.IRP = *Packet;
    return Call;
  }
  if (FrameworkPnp) {
    if (Request.DeviceRoute.size() != 2 || Request.DeviceRoute.back() != PDO ||
        Request.StackCount < 2)
      return pnpError("framework PnP forwarding requires its FDO/PDO pair");
    auto Preprocess = Framework->beginPnpPreprocess(PDO, *Packet, Minor);
    if (!Preprocess)
      return Preprocess.takeError();
    if (*Preprocess) {
      Request.FrameworkPnpBeforeBus = true;
      Request.FrameworkPnpAwaiting = true;
      if (auto E = markRequestPending(*Packet))
        return E;
      if (auto E = recordDispatchReturn(*Packet, StatusPending))
        return E;
      Invocation Call;
      Call.IRP = *Packet;
      return Call;
    }
    auto Status = forwardFrameworkPnpRequest(*Packet);
    if (!Status)
      return Status.takeError();
    if (auto E = recordDispatchReturn(*Packet, uint32_t(*Status)))
      return E;
    Invocation Call;
    Call.IRP = *Packet;
    return Call;
  }
  Invocation Call{Callback, *Top, *Packet};
  Call.IRP = *Packet;
  return Call;
}

llvm::Expected<uint64_t> KernelModel::forwardFrameworkPnpRequest(uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  if (!Request || !Request->PnpOperation || Request->Completed ||
      Request->Forwarded || Request->FrameworkPnpAwaiting ||
      Request->DeviceRoute.size() != 2 || Request->StackCount < 2 ||
      Request->DeviceRoute.back() != Request->PnpDevice ||
      !FrameworkDevices.count(Request->DeviceRoute.front()))
    return pnpError("framework forwarding lost its undispatched FDO/PDO IRP");
  const uint64_t Lower = Request->Stack - StackSize;
  std::array<uint8_t, StackSize> StackBytes{};
  if (auto E = Memory.read(Request->Stack, StackBytes))
    return E;
  StackBytes[StackControlOffset] = 0;
  if (auto E = Memory.write(Lower, StackBytes))
    return E;
  if (auto E =
          Memory.writeInteger(Lower + StackDeviceOffset, Request->PnpDevice, 8))
    return E;
  if (auto E = Memory.writeInteger(IRP + IRPLocationOffset,
                                   Request->StackCount - 1, 1))
    return E;
  if (auto E = Memory.writeInteger(IRP + IRPStackPointerOffset, Lower, 8))
    return E;
  Request->Forwarded = true;
  return callProviderDriver(Request->PnpDevice, IRP);
}

llvm::Error KernelModel::finishRequestLifecycle(ActiveRequest &Request,
                                                uint32_t Status) {
  if (Request.PowerTicket) {
    const auto &Power = *Request.PowerOperation;
    auto E = Power.Type == DriverPowerType::Device
                 ? Lifecycle.finishDevicePower(*Request.PowerTicket, Status)
                 : Lifecycle.finishSystemPower(*Request.PowerTicket, Status);
    if (E)
      return E;
    auto State = Lifecycle.snapshot(Request.PnpDevice);
    if (!State)
      return State.takeError();
    auto &Observation = *Result.Requests[Request.ResultIndex].Power;
    Observation.DeviceStateAfter = State->DevicePower;
    Observation.SystemStateAfter = State->SystemPower;
  } else if (Request.PnpTicket) {
    if (auto E = Resources.finishPnp(Request.PnpDevice,
                                     Request.PnpOperation->Minor, Status))
      return E;
    if (auto E = Lifecycle.finishPnp(*Request.PnpTicket, Status))
      return E;
    auto State = Lifecycle.snapshot(Request.PnpDevice);
    if (!State)
      return State.takeError();
    Result.Requests[Request.ResultIndex].Pnp->StateAfter = State->Pnp;
  } else if (Request.LifecycleIo) {
    if (auto E = Lifecycle.finishIo(Request.PnpDevice, Request.IRP))
      return E;
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
