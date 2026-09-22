//===- KernelModelInterruptEvents.cpp - External interrupt deadlines -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Admit all due model callbacks before advancing the shared scheduler clock,
/// then deliver explicit device pulses after actual provider publications.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"

namespace neverd::emulation {
namespace {
llvm::Error interruptEventError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::preflightScheduledBoundary(uint64_t Time) {
  uint64_t ProviderCount = 0;
  std::vector<KernelScheduler::Callback> ProviderCallbacks;
  for (const auto &[IRP, Provider] : ProviderCompletions) {
    if (Provider.Deadline > Time)
      continue;
    const auto *Request = requestForIRP(IRP);
    if (!Request || Request->Completed ||
        Request->PnpDevice != Provider.Device)
      return interruptEventError(
          "PnP provider: deadline lost its live request or PDO identity");
    auto Plan = planIRPCompletion(IRP, Provider.Status);
    if (!Plan)
      return Plan.takeError();
    if (Plan->PC)
      ProviderCallbacks.push_back({IRP, Provider.Device,
                                   profile::WorkerThreadIdentity, Plan->PC,
                                   Plan->Arguments});
    ++ProviderCount;
  }
  if (ProviderCount) {
    if (hasPendingModelGuestCall())
      return interruptEventError(
          "PnP provider: deadline cannot replace a pending guest callback");
    if (ProviderCount > UINT64_MAX - NextIRPCall)
      return interruptEventError(
          "PnP provider: completion identity exhausted");
    if (auto E = Scheduler.canEnqueueWDMCompletions(ProviderCallbacks))
      return E;
  }

  uint64_t Cancellations = 0;
  for (const auto &[IRP, Request] : Requests) {
    if (Request.Completed || !Request.CancelDeadline ||
        *Request.CancelDeadline > Time)
      continue;
    if (!Framework || !FrameworkDevices.count(Request.Device))
      return interruptEventError(
          "scheduled cancellation requires a KMDF control request");
    auto GeneratesCallback =
        Framework->preflightRequestCancellation(IRP, Cancellations);
    if (!GeneratesCallback)
      return GeneratesCallback.takeError();
    Cancellations += *GeneratesCallback;
  }
  auto InterruptCount = Interrupts.dueCount(Time);
  if (!InterruptCount)
    return InterruptCount.takeError();
  const uint64_t Providers = ProviderCallbacks.size();
  if (Cancellations > UINT64_MAX - Providers ||
      *InterruptCount > UINT64_MAX - Providers - Cancellations)
    return interruptEventError("scheduled boundary callback count overflow");
  // The caller combines this exact count with timer-DPC coalescing in one
  // scheduler capacity/identity preflight before changing time or
  // observations.
  return Providers + Cancellations + *InterruptCount;
}

llvm::Error KernelModel::processInterruptEvents() {
  for (;;) {
    auto Delivery = Interrupts.queueNextDue(Scheduler.now100ns());
    if (!Delivery)
      return Delivery.takeError();
    if (!*Delivery)
      return llvm::Error::success();
    auto &Event = **Delivery;
    if (Event.Call.Token.Owner != GuestCallOwner::Interrupt ||
        !Event.Call.Token.ID)
      return interruptEventError(
          "interrupt event lost its typed continuation");
    KernelScheduler::InterruptCallback Callback;
    Callback.Object = Event.Call.Token.ID;
    Callback.Owner = Event.PDO;
    // This is only an opaque scheduler context identity, not a claim that an
    // ISR executes on a Windows worker thread. Guest thread APIs are
    // unmodeled.
    Callback.Thread = profile::WorkerThreadIdentity;
    Callback.PC = Event.Call.PC;
    Callback.Arguments = std::move(Event.Call.Arguments);
    Callback.IRQL = Event.IRQL;
    Callback.Priority = Event.IRQL;
    auto ID = Scheduler.enqueueInterrupt(std::move(Callback));
    if (!ID)
      return ID.takeError();
    ScheduledModelContinuations.emplace(*ID, Event.Call.Token);
  }
}

} // namespace neverd::emulation
