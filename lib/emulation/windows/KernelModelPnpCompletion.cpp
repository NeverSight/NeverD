//===- KernelModelPnpCompletion.cpp - Explicit provider completion --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Apply configured resource-free bus responses only after a real PDO dispatch.
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
  if (CurrentIRQL >= scheduler::DispatchLevel)
    return providerError("PnP forwarding requires IRQL below DISPATCH_LEVEL");
  auto *Request = requestForIRP(IRP);
  const auto *Provider = pnpDeviceForPDO(Device);
  if (!Request || Request->Completed || !isProviderDevice(Device) ||
      !Provider || !Provider->BusResourceFree || Request->PnpDevice != Device ||
      Request->Kind != DriverRequestKind::Pnp || !Request->PnpOperation)
    return providerError(
        "dispatch requires its configured resource-free PnP IRP");
  const auto &Operation = *Request->PnpOperation;
  if (auto E = validateDriverPnpOperation(Operation))
    return E;
  const auto &Response = Operation.BusCompletion;
  if (PendingWdmCall || (Framework && Framework->hasPendingGuestCall()))
    return providerError("cannot replace a pending guest callback");
  auto Stack = currentRequestStack(IRP);
  if (!Stack)
    return Stack.takeError();
  auto Minor = Memory.readInteger(*Stack + StackMinorOffset, 1);
  if (!Minor)
    return Minor.takeError();
  if (*Minor != uint8_t(Operation.Minor))
    return providerError(
        "raw PnP minor does not match the configured operation");
  auto &Observation = Result.Requests[Request->ResultIndex].Pnp;
  if (!Observation)
    return providerError("request lost its PnP observation");
  if (Observation->BusReceivedAt100ns || ProviderCompletions.count(IRP))
    return providerError("configured bus response was already dispatched");
  auto Deadline = Scheduler.computeDeadline(-int64_t(Response.Delay100ns));
  if (!Deadline)
    return Deadline.takeError();
  // This probe shares all cursor, invocation-mask and upper-device decisions
  // with real completion. It publishes neither status nor completion progress.
  auto Plan = planIRPCompletion(IRP, *Response.Status);
  if (!Plan)
    return Plan.takeError();
  if (NextIRPCall == UINT64_MAX || NextProviderSequence == UINT64_MAX)
    return providerError("completion identity exhausted");
  if (Response.Delay100ns) {
    if (auto E = markRequestPending(IRP))
      return E;
    ProviderCompletions.emplace(IRP, ProviderCompletion{Device, *Deadline,
                                                        NextProviderSequence++,
                                                        *Response.Status});
    Observation->BusReceivedAt100ns = Scheduler.now100ns();
    return StatusPending;
  }
  if (auto E = Memory.writeInteger(IRP + IRPStatusOffset, *Response.Status, 4))
    return E;
  if (auto E = Memory.writeInteger(IRP + IRPInformationOffset, 0, 8))
    return E;
  Request->IOStatusWritten.fill(true);
  Observation->BusReceivedAt100ns = Scheduler.now100ns();
  Observation->BusStatus = *Response.Status;
  Observation->BusCompletedAt100ns = Scheduler.now100ns();
  if (auto E = completeRequest(IRP, 0))
    return E;
  if (PendingWdmCall)
    IRPCalls.at(PendingWdmCall->Token.ID).ReturnValue = *Response.Status;
  return *Response.Status;
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
      Callbacks.push_back({IRP,
                           Provider.Device,
                           profile::WorkerThreadIdentity,
                           Plan->PC,
                           {Plan->Device, IRP, Plan->Context}});
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
    auto &Observation = Result.Requests[Request.ResultIndex].Pnp;
    if (!Observation)
      return providerError("completed request lost its PnP observation");
    Observation->BusStatus = Completion.Provider.Status;
    Observation->BusCompletedAt100ns = Scheduler.now100ns();
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
        Call->Arguments != std::vector<uint64_t>{Completion.Plan.Device, IRP,
                                                 Completion.Plan.Context})
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
