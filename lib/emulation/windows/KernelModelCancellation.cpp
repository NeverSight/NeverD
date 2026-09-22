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

namespace neverd::emulation {
namespace {
llvm::Error cancellationError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

llvm::Error KernelModel::processRequestCancellations() {
  for (auto &[IRP, Request] : Requests) {
    if (Request.Completed || !Request.CancelDeadline ||
        *Request.CancelDeadline > Scheduler.now100ns())
      continue;
    if (!Framework || !FrameworkDevices.count(Request.Device))
      return cancellationError(
          "scheduled cancellation requires a KMDF control request");
    if (auto E = Memory.writeInteger(IRP + windows::IRPCancelOffset, 1, 1))
      return E;
    Request.CancelRequested = true;
    Request.CancelDeadline.reset();
    Result.Requests[Request.ResultIndex].CancelRequestedAt100ns =
        Scheduler.now100ns();
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
      Kind != KernelScheduler::CallbackKind::WDMCompletion)
    return std::optional<KernelGuestCall>{};
  auto Token = ScheduledModelContinuations.find(ID);
  if (Token == ScheduledModelContinuations.end())
    return cancellationError("scheduled callback lost its model continuation");
  const auto ExpectedOwner = Kind == KernelScheduler::CallbackKind::FrameworkCancel
                                 ? GuestCallOwner::Framework : GuestCallOwner::WDM;
  if (Token->second.Owner != ExpectedOwner)
    return cancellationError("scheduled callback has a foreign continuation owner");
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
