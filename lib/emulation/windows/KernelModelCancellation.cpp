//===- KernelModelCancellation.cpp - Scheduled request cancellation -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Own cancellation facts on WDM requests and route framework callbacks through
/// the same bounded scheduler as other guest work. Request completion removes a
/// future deadline; it never turns a completed request into a cancellation.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error cancellationError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

llvm::Expected<std::optional<KernelScheduler::Callback>>
KernelModel::planWDMCancellation(uint64_t IRP,
                                 const ActiveRequest &Request) const {
  if (Request.Completed)
    return cancellationError("WDM cancellation requires a live IRP");
  auto Routine = Memory.readInteger(IRP + windows::IRPCancelRoutineOffset, 8);
  if (!Routine)
    return Routine.takeError();
  if (!*Routine)
    return std::optional<KernelScheduler::Callback>{};
  auto Stack = currentRequestStack(IRP);
  if (!Stack)
    return Stack.takeError();
  auto Device = Memory.readInteger(*Stack + windows::StackDeviceOffset, 8);
  if (!Device)
    return Device.takeError();
  if (!Devices.count(*Device) ||
      std::find(Request.DeviceRoute.begin(), Request.DeviceRoute.end(),
                *Device) == Request.DeviceRoute.end())
    return cancellationError(
        "WDM cancellation lost the current stack's device identity");
  KernelScheduler::Callback Callback;
  Callback.Object = IRP;
  Callback.Owner = *Device;
  Callback.Thread = profile::WorkerThreadIdentity;
  Callback.PC = *Routine;
  Callback.Arguments = {*Device, IRP};
  return std::optional<KernelScheduler::Callback>{std::move(Callback)};
}

llvm::Expected<uint64_t> KernelModel::acquireCancelSpinLock(uint64_t OldIRQL) {
  if (CancelLock.Held)
    return cancellationError("cancel spin lock is already held");
  if (CurrentIRQL > scheduler::DispatchLevel)
    return cancellationError(
        "IoAcquireCancelSpinLock requires IRQL <= DISPATCH_LEVEL");
  if (auto E = validateGuestAccess(OldIRQL, 1, true))
    return E;
  if (auto E = Memory.writeInteger(OldIRQL, CurrentIRQL, 1))
    return E;
  CancelLock.Held = true;
  CancelLock.Owner = CurrentExecution;
  CancelLock.OldIRQL = CurrentIRQL;
  CurrentIRQL = scheduler::DispatchLevel;
  return 0;
}

llvm::Error KernelModel::releaseCancelSpinLock(uint64_t OldIRQL) {
  if (!CancelLock.Held || CancelLock.Owner != CurrentExecution ||
      CurrentIRQL != scheduler::DispatchLevel)
    return cancellationError(
        "IoReleaseCancelSpinLock requires the current cancel lock owner");
  if (uint8_t(OldIRQL) != CancelLock.OldIRQL)
    return cancellationError(
        "IoReleaseCancelSpinLock must restore the saved IRQL");
  CancelLock.Held = false;
  CancelLock.Owner = 0;
  CurrentIRQL = CancelLock.OldIRQL;
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::cancelIRP(uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed ||
      (Framework && Framework->ownsRequestIRP(IRP)))
    return cancellationError("IoCancelIrp requires a live WDM-owned IRP");
  if (CancelLock.Held || CancelLock.Callback || hasPendingModelGuestCall())
    return cancellationError(
        "IoCancelIrp cannot nest inside a cancel lock or guest callback");
  if (NextIRPCall == UINT64_MAX)
    return cancellationError("WDM cancel continuation identity exhausted");
  auto Plan = planWDMCancellation(IRP, *Request);
  if (!Plan)
    return Plan.takeError();
  const uint8_t OldIRQL = CurrentIRQL;
  if (auto E = Memory.writeInteger(IRP + windows::IRPCancelOffset, 1, 1))
    return E;
  Request->CancelRequested = true;
  Request->CancelDeadline.reset();
  auto &Observation = Result.Requests[Request->ResultIndex];
  if (!Observation.CancelRequestedAt100ns)
    Observation.CancelRequestedAt100ns = Scheduler.now100ns();
  if (!*Plan)
    return 0;
  if (auto E =
          Memory.writeInteger(IRP + windows::IRPCancelIROffset, OldIRQL, 1))
    return E;
  if (auto E = Memory.writeInteger(IRP + windows::IRPCancelRoutineOffset, 0, 8))
    return E;
  const uint64_t Token = NextIRPCall++;
  IRPCalls.emplace(Token, IRPCall{IRPCallKind::Cancel, IRP, 0, true});
  PendingWdmCall = KernelGuestCall{
      {GuestCallOwner::WDM, Token}, (**Plan).PC, std::move((**Plan).Arguments)};
  CancelLock.Held = true;
  CancelLock.Callback = true;
  CancelLock.Owner = 0;
  CancelLock.CallbackExecution = 0;
  CancelLock.IRP = IRP;
  CancelLock.OldIRQL = OldIRQL;
  CurrentIRQL = scheduler::DispatchLevel;
  return 0;
}

llvm::Error KernelModel::processRequestCancellations() {
  for (auto &[IRP, Request] : Requests) {
    if (Request.Completed || !Request.CancelDeadline ||
        *Request.CancelDeadline > Scheduler.now100ns())
      continue;
    const bool KMDF = Framework && FrameworkDevices.count(Request.Device);
    std::optional<KernelScheduler::Callback> WDMCancellation;
    if (!KMDF) {
      if (!Request.DispatchReturned || Request.CancelRequested)
        return cancellationError(
            "scheduled WDM cancellation requires a dispatched IRP");
      auto Plan = planWDMCancellation(IRP, Request);
      if (!Plan)
        return Plan.takeError();
      WDMCancellation = std::move(*Plan);
      if (auto E = Memory.writeInteger(IRP + windows::IRPCancelIROffset,
                                       scheduler::PassiveLevel, 1))
        return E;
      if (WDMCancellation)
        if (auto E = Memory.writeInteger(IRP + windows::IRPCancelRoutineOffset,
                                         0, 8))
          return E;
    }
    if (auto E = Memory.writeInteger(IRP + windows::IRPCancelOffset, 1, 1))
      return E;
    Request.CancelRequested = true;
    Request.CancelDeadline.reset();
    Result.Requests[Request.ResultIndex].CancelRequestedAt100ns =
        Scheduler.now100ns();
    if (!KMDF) {
      if (WDMCancellation) {
        auto ID = Scheduler.enqueueWDMCancellation(std::move(*WDMCancellation));
        if (!ID)
          return ID.takeError();
      }
      continue;
    }
    auto Call = Framework->requestCancellation(IRP);
    if (!Call)
      return Call.takeError();
    if (!*Call)
      continue;
    KernelScheduler::Callback Callback;
    Callback.Object = IRP;
    Callback.Owner = Request.Device;
    Callback.Thread = profile::WorkerThreadIdentity;
    Callback.PC = (**Call).PC;
    Callback.Arguments = (**Call).Arguments;
    auto ID = Scheduler.enqueueFrameworkCancel(std::move(Callback));
    if (!ID)
      return ID.takeError();
    ScheduledModelContinuations.emplace(
        *ID, GuestCallToken{GuestCallOwner::Framework, (**Call).Token});
  }
  return llvm::Error::success();
}

llvm::Expected<std::optional<KernelGuestCall>>
KernelModel::continueScheduled(uint64_t ID, uint64_t ReturnValue) {
  if (!Scheduler.active() || Scheduler.active()->ID != ID)
    return cancellationError(
        "callback continuation does not match active task");
  const auto Kind = Scheduler.active()->Kind;
  if (Kind != KernelScheduler::CallbackKind::FrameworkCancel &&
      Kind != KernelScheduler::CallbackKind::FrameworkCompletion &&
      Kind != KernelScheduler::CallbackKind::WDMCompletion &&
      Kind != KernelScheduler::CallbackKind::Interrupt &&
      !KernelScheduler::isDMACallbackKind(Kind))
    return std::optional<KernelGuestCall>{};
  auto Token = ScheduledModelContinuations.find(ID);
  if (Token == ScheduledModelContinuations.end())
    return cancellationError("scheduled callback lost its model continuation");
  const auto ExpectedOwner =
      (Kind == KernelScheduler::CallbackKind::FrameworkCancel ||
       Kind == KernelScheduler::CallbackKind::FrameworkCompletion)
          ? GuestCallOwner::Framework
      : Kind == KernelScheduler::CallbackKind::Interrupt
          ? GuestCallOwner::Interrupt
      : KernelScheduler::isDMACallbackKind(Kind) ? GuestCallOwner::DMA
                                                 : GuestCallOwner::WDM;
  if (Token->second.Owner != ExpectedOwner)
    return cancellationError(
        "scheduled callback has a foreign continuation owner");
  auto Result = finishGuestCall(Token->second, ReturnValue);
  if (!Result)
    return Result.takeError();
  if (*Result) {
    ScheduledModelContinuations.erase(Token);
    return std::optional<KernelGuestCall>{};
  }
  auto Call = takeGuestCall();
  if (!Call || Call->Token.Owner != Token->second.Owner ||
      Call->Token.ID != Token->second.ID)
    return cancellationError("scheduled epilogue lost its guest callback");
  return Call;
}
} // namespace neverd::emulation
