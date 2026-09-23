//===- KernelModelPnpCompletion.cpp - Explicit provider completion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Apply configured bus responses only after a real PDO dispatch.
/// Deadlines are virtual; only actual guest completion routines are scheduled.
///
//===----------------------------------------------------------------------===//

#include "../DriverScenario.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
#include <tuple>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error providerError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "PnP provider: " + Message);
}
} // namespace

llvm::Expected<uint64_t> KernelModel::callProviderDriver(uint64_t Device,
                                                         uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  const auto *Provider = pnpDeviceForPDO(Device);
  if (!Request || Request->Completed || !isProviderDevice(Device) ||
      !Provider || Request->PnpDevice != Device)
    return providerError("dispatch requires its configured provider IRP");
  const DriverBusCompletion *Response = nullptr;
  uint8_t ExpectedMinor = 0;
  if (Request->Kind == DriverRequestKind::Pnp && Request->PnpOperation) {
    if (CurrentIRQL >= scheduler::DispatchLevel)
      return providerError("PnP forwarding requires IRQL below DISPATCH_LEVEL");
    if (auto E = validateDriverPnpOperation(*Request->PnpOperation))
      return E;
    Response = &Request->PnpOperation->BusCompletion;
    ExpectedMinor = uint8_t(Request->PnpOperation->Minor);
  } else if (Request->Kind == DriverRequestKind::Power &&
             Request->PowerOperation) {
    if (CurrentIRQL != scheduler::PassiveLevel)
      return providerError("pageable power forwarding requires PASSIVE_LEVEL");
    if (auto E = validateDriverPowerOperation(*Request->PowerOperation))
      return E;
    Response = &Request->PowerOperation->BusCompletion;
    ExpectedMinor = uint8_t(Request->PowerOperation->Minor);
  } else {
    return providerError(
        "dispatch requires a configured PnP or power operation");
  }
  if (PendingWdmCall || (Framework && Framework->hasPendingGuestCall()))
    return providerError("cannot replace a pending guest callback");
  auto Stack = currentRequestStack(IRP);
  if (!Stack)
    return Stack.takeError();
  auto Minor = Memory.readInteger(*Stack + StackMinorOffset, 1);
  if (!Minor)
    return Minor.takeError();
  if (*Minor != ExpectedMinor)
    return providerError(
        Request->PowerOperation
            ? "raw power minor does not match the configured operation"
            : "raw PnP minor does not match the configured operation");
  if (Request->PnpOperation &&
      Request->PnpOperation->Minor == DevicePnpRequest::Start) {
    for (const auto &[Offset, Expected] :
         std::initializer_list<std::pair<uint64_t, uint64_t>>{
             {StackStartResourcesOffset, Request->RawResources},
             {StackStartTranslatedResourcesOffset,
              Request->TranslatedResources}}) {
      auto Actual = Memory.readInteger(*Stack + Offset, 8);
      if (!Actual)
        return Actual.takeError();
      if (*Actual != Expected)
        return providerError(
            "START resource pointers changed while forwarding");
    }
  }
  if (Request->PowerOperation) {
    const auto &Operation = *Request->PowerOperation;
    for (const auto &[Offset, Expected] :
         std::initializer_list<std::pair<uint64_t, uint32_t>>{
             {StackPowerSystemContextOffset, Operation.SystemContext},
             {StackPowerTypeOffset, uint32_t(Operation.Type)},
             {StackPowerStateOffset, Operation.State},
             {StackPowerActionOffset, uint32_t(Operation.Action)}}) {
      auto Actual = Memory.readInteger(*Stack + Offset, 4);
      if (!Actual)
        return Actual.takeError();
      if (*Actual != Expected)
        return providerError("raw power parameters changed while forwarding");
    }
  }
  auto &Observation = Result.Requests[Request->ResultIndex];
  const auto Received = Observation.Pnp ? Observation.Pnp->BusReceivedAt100ns
                        : Observation.Power
                            ? Observation.Power->BusReceivedAt100ns
                            : std::optional<uint64_t>{};
  if (!Observation.Pnp && !Observation.Power)
    return providerError("request lost its bus observation");
  if (Received || ProviderCompletions.count(IRP))
    return providerError("configured bus response was already dispatched");
  auto Deadline = Scheduler.computeDeadline(-int64_t(Response->Delay100ns));
  if (!Deadline)
    return Deadline.takeError();
  // This probe shares all cursor, invocation-mask and upper-device decisions
  // with real completion. It publishes neither status nor completion progress.
  auto Plan = planIRPCompletion(IRP, *Response->Status);
  if (!Plan)
    return Plan.takeError();
  if (NextIRPCall == UINT64_MAX || NextProviderSequence == UINT64_MAX)
    return providerError("completion identity exhausted");
  if (Response->Delay100ns) {
    if (auto E = markRequestPending(IRP))
      return E;
    ProviderCompletions.emplace(IRP, ProviderCompletion{Device, *Deadline,
                                                        NextProviderSequence++,
                                                        *Response->Status});
    if (Observation.Pnp)
      Observation.Pnp->BusReceivedAt100ns = Scheduler.now100ns();
    else
      Observation.Power->BusReceivedAt100ns = Scheduler.now100ns();
    return StatusPending;
  }
  // Completion can finalize a generated child; preserve the configured value
  // before any operation that can retire its owning request record.
  const uint32_t Status = *Response->Status;
  if (auto E = Memory.writeInteger(IRP + IRPStatusOffset, Status, 4))
    return E;
  if (auto E = Memory.writeInteger(IRP + IRPInformationOffset, 0, 8))
    return E;
  Request->IOStatusWritten.fill(true);
  auto Publish = [&](auto &Bus) {
    Bus.BusReceivedAt100ns = Scheduler.now100ns();
    Bus.BusStatus = Status;
    Bus.BusCompletedAt100ns = Scheduler.now100ns();
  };
  if (Observation.Pnp)
    Publish(*Observation.Pnp);
  else
    Publish(*Observation.Power);
  if (Request->PowerOperation &&
      Request->PowerOperation->Type == DriverPowerType::Device &&
      Request->PowerOperation->Minor == DevicePowerRequest::Set &&
      !(Status & profile::NTStatusFailureMask))
    Devices.at(Device).ReportedDevicePower =
        static_cast<DevicePowerState>(Request->PowerOperation->State);
  if (auto E = publishProviderHardware(*Request, Status))
    return E;
  if (auto E = completeRequest(IRP, 0))
    return E;
  if (Request->FrameworkPnpAwaiting)
    return StatusPending;
  if (PendingWdmCall)
    IRPCalls.at(PendingWdmCall->Token.ID).ReturnValue = Status;
  return Status;
}

llvm::Error KernelModel::processProviderCompletions() {
  struct DueCompletion {
    uint64_t IRP;
    ProviderCompletion Provider;
    IRPCompletionPlan Plan;
  };
  std::vector<DueCompletion> Due;
  std::vector<KernelScheduler::Callback> Callbacks;
  for (const auto &[IRP, Provider] : ProviderCompletions) {
    if (Provider.Deadline > Scheduler.now100ns())
      continue;
    const auto *Request = requestForIRP(IRP);
    if (!Request || Request->Completed || Request->PnpDevice != Provider.Device)
      return providerError("deadline lost its live request or PDO identity");
    auto Plan = planIRPCompletion(IRP, Provider.Status);
    if (!Plan)
      return Plan.takeError();
    if (Plan->PC)
      Callbacks.push_back({IRP, Provider.Device, profile::WorkerThreadIdentity,
                           Plan->PC, Plan->Arguments});
    Due.push_back({IRP, Provider, std::move(*Plan)});
  }
  if (Due.empty())
    return llvm::Error::success();
  if (PendingWdmCall || (Framework && Framework->hasPendingGuestCall()))
    return providerError("deadline cannot replace a pending guest callback");
  if (Due.size() > UINT64_MAX - NextIRPCall)
    return providerError("completion identity exhausted");
  if (auto E = Scheduler.canEnqueueWDMCompletions(Callbacks))
    return E;
  std::sort(Due.begin(), Due.end(), [](const auto &A, const auto &B) {
    return std::tie(A.Provider.Deadline, A.Provider.Sequence) <
           std::tie(B.Provider.Deadline, B.Provider.Sequence);
  });
  // No guest code can run between the batch preflight and these commits.
  // Every potential guest callback already has queue and identity capacity.
  for (const auto &Completion : Due) {
    const uint64_t IRP = Completion.IRP;
    auto &Request = *requestForIRP(IRP);
    if (auto E = Memory.writeInteger(IRP + IRPStatusOffset,
                                     Completion.Provider.Status, 4))
      return E;
    if (auto E = Memory.writeInteger(IRP + IRPInformationOffset, 0, 8))
      return E;
    Request.IOStatusWritten.fill(true);
    auto &Observation = Result.Requests[Request.ResultIndex];
    auto Publish = [&](auto &Bus) {
      Bus.BusStatus = Completion.Provider.Status;
      Bus.BusCompletedAt100ns = Scheduler.now100ns();
    };
    if (Observation.Pnp)
      Publish(*Observation.Pnp);
    else if (Observation.Power)
      Publish(*Observation.Power);
    else
      return providerError("completed request lost its bus observation");
    if (Request.PowerOperation &&
        Request.PowerOperation->Type == DriverPowerType::Device &&
        Request.PowerOperation->Minor == DevicePowerRequest::Set &&
        !(Completion.Provider.Status & profile::NTStatusFailureMask))
      Devices.at(Completion.Provider.Device).ReportedDevicePower =
          static_cast<DevicePowerState>(Request.PowerOperation->State);
    if (auto E = publishProviderHardware(Request, Completion.Provider.Status))
      return E;
    ProviderCompletions.erase(IRP);
    if (auto E = completeRequest(IRP, 0)) {
      ProviderCompletions.emplace(IRP, Completion.Provider);
      return E;
    }
    auto Call = takeWdmGuestCall();
    if (bool(Call) != bool(Completion.Plan.PC))
      return providerError("completion diverged from its preflight plan");
    if (!Call)
      continue;
    if (Call->Token.Owner != GuestCallOwner::WDM ||
        Call->PC != Completion.Plan.PC ||
        Call->Arguments != Completion.Plan.Arguments)
      return providerError("completion callback changed after preflight");
    auto ID = Scheduler.enqueueWDMCompletion(
        {IRP, Completion.Provider.Device, profile::WorkerThreadIdentity,
         Call->PC, std::move(Call->Arguments)});
    if (!ID)
      return ID.takeError();
    ScheduledModelContinuations.emplace(*ID, Call->Token);
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
