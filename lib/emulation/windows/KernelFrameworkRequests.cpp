//===- KernelFrameworkRequests.cpp - KMDF queue dispatch and completion
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Framework request handles borrow one authoritative WDM IRP. Queue callbacks
/// return void; the framework owns pending dispatch status and completion owns
/// packet/buffer retirement independently from callback return.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace framework;
llvm::Error requestError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF request: " + Message);
}
} // namespace

llvm::Expected<std::optional<KernelFramework::GuestCall>>
KernelFramework::requestCancellation(uint64_t IRP) {
  auto R =
      std::find_if(Requests.begin(), Requests.end(),
                   [&](const auto &Entry) { return Entry.second.IRP == IRP; });
  if (R == Requests.end() || R->second.Completed || R->second.Completing ||
      R->second.Cancellation != CancelState::Marked)
    return std::optional<GuestCall>{};
  if (PendingCall)
    return requestError("cancellation cannot replace a pending guest callback");
  if (!RequestsHost.IsCanceled)
    return requestError("cancellation host is unavailable");
  auto Canceled = RequestsHost.IsCanceled(IRP);
  if (!Canceled)
    return Canceled.takeError();
  if (!*Canceled)
    return requestError("cancellation requires the underlying IRP cancel flag");
  auto &O = Objects.at(R->first);
  if (!O.InternalReferences || !R->second.CancelRoutine)
    return requestError("cancelable request lost its callback reference");
  const uint64_t Token = NextContinuation++;
  // FxRequest::InsertTailIrpQueue holds FXREQUEST_QUEUE_TAG. Cancellation
  // transfers that hold to ProcessCancelledRequests, which releases it only
  // after InvokeCancel returns. Driver references are a separate authority.
  // https://github.com/microsoft/Windows-Driver-Frameworks/blob/b6191d9543441329154da32f7ab9bdd97228dd3c/src/framework/shared/irphandlers/io/fxioqueue.cpp#L4892-L4935
  Continuations.emplace(
      Token,
      Continuation{{{StepKind::Callback, R->first, R->second.CancelRoutine},
                    {StepKind::CancelReturned, R->first}}});
  CancelCallbacks.emplace(Token, R->first);
  R->second.Cancellation = CancelState::Queued;
  R->second.CancelRoutine = 0;
  auto Result = advance(Token);
  if (!Result)
    return Result.takeError();
  return takeGuestCall();
}

llvm::Error KernelFramework::beginCancelCallback(uint64_t Token) {
  auto C = CancelCallbacks.find(Token);
  if (C == CancelCallbacks.end())
    return requestError("unknown cancellation callback token");
  auto R = Requests.find(C->second);
  if (R == Requests.end() || R->second.Completed || R->second.Completing ||
      R->second.Cancellation != CancelState::Queued)
    return requestError("cancellation callback was already delivered");
  // Queueing a callback does not authorize completion. The public framework
  // sets FXREQUEST_FLAG_CANCELLED immediately before invoking the driver.
  R->second.Cancellation = CancelState::Delivered;
  return llvm::Error::success();
}

llvm::Expected<std::optional<KernelFramework::RequestDispatch>>
KernelFramework::routeRequest(uint64_t WdmDevice, uint64_t IRP) {
  auto D = std::find_if(Devices.begin(), Devices.end(), [&](const auto &Entry) {
    return Entry.second.Wdm == WdmDevice;
  });
  if (D == Devices.end())
    return std::optional<RequestDispatch>{};
  if (!D->second.Initialized || Objects.at(D->first).Deleting)
    return requestError("I/O requires a fully initialized live control device");
  if (!RequestsHost.View || !RequestsHost.Complete || !RequestsHost.MarkPending)
    return requestError("underlying WDM request host is unavailable");
  auto View = RequestsHost.View(IRP);
  if (!View)
    return View.takeError();
  auto CompleteImmediately =
      [&](uint32_t Status,
          uint32_t Dispatch) -> llvm::Expected<std::optional<RequestDispatch>> {
    if (auto E = RequestsHost.Complete(IRP, Status, 0))
      return E;
    return std::optional<RequestDispatch>{RequestDispatch{0, {}, Dispatch}};
  };
  // The no-file-callback nonfilter package completes these synchronously with
  // Information=0. File identity/order remains owned by the WDM request model.
  if (View->Major == RequestMajorCreate || View->Major == RequestMajorCleanup ||
      View->Major == RequestMajorClose)
    return CompleteImmediately(0, 0);
  auto Q = Queues.find(D->second.DefaultQueue);
  if (Q == Queues.end())
    return CompleteImmediately(ControlInvalidDeviceRequest,
                               ControlInvalidDeviceRequest);
  if (Objects.at(Q->first).Deleting)
    return requestError("default queue is deleting");
  if (std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.Queue == Q->first && !Entry.second.Completed;
      }))
    return requestError("sequential queue still owns a delivered request");
  auto &Queue = Q->second;
  uint64_t Callback = 0;
  uint64_t Length = 0;
  bool Specific = true;
  if (View->Major == RequestMajorRead) {
    Callback = Queue.Read;
    Length = View->OutputLength;
  } else if (View->Major == RequestMajorWrite) {
    Callback = Queue.Write;
    Length = View->InputLength;
  } else if (View->Major == RequestMajorDeviceControl) {
    Callback = Queue.DeviceControl;
  } else {
    return requestError("unsupported framework request major function");
  }
  if (!Callback) {
    Specific = false;
    Callback = Queue.Default;
  }
  // FxIoQueue::QueueRequest marks accepted IRPs pending before dispatching.
  // That status persists even when delivery completes the IRP immediately.
  if (auto E = RequestsHost.MarkPending(IRP))
    return E;
  if (!Callback)
    return CompleteImmediately(ControlInvalidDeviceRequest,
                               windows::StatusPending);
  if (!Queue.AllowZeroLength && !Length &&
      (View->Major == RequestMajorRead || View->Major == RequestMajorWrite))
    return CompleteImmediately(0, windows::StatusPending);
  Attributes Attrs;
  Attrs.Parent = Q->first;
  const uint64_t Globals = Objects.at(D->first).Binding;
  auto Handle = createObject(Globals, Attrs, false);
  if (!Handle)
    return Handle.takeError();
  Objects.at(*Handle).Kind = ObjectKind::Request;
  Requests.emplace(*Handle, Request{IRP, Q->first, false});
  RequestDispatch Dispatch;
  Dispatch.PC = Callback;
  Dispatch.Status = windows::StatusPending;
  Dispatch.Arguments = {Q->first, *Handle};
  if (Specific) {
    if (View->Major == RequestMajorDeviceControl) {
      Dispatch.Arguments.push_back(View->OutputLength);
      Dispatch.Arguments.push_back(View->InputLength);
      Dispatch.Arguments.push_back(View->ControlCode);
    } else {
      Dispatch.Arguments.push_back(Length);
    }
  }
  return std::optional<RequestDispatch>{std::move(Dispatch)};
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::callRequest(llvm::StringRef Name, Binding &B,
                             llvm::ArrayRef<uint64_t> A) {
  using Result = std::optional<uint64_t>;
  if (Name != "WdfRequestComplete" &&
      Name != "WdfRequestCompleteWithInformation" &&
      Name != "WdfRequestGetParameters" &&
      Name != "WdfRequestRetrieveInputBuffer" &&
      Name != "WdfRequestRetrieveOutputBuffer" &&
      Name != "WdfRequestMarkCancelableEx" &&
      Name != "WdfRequestUnmarkCancelable" && Name != "WdfRequestIsCanceled")
    return Result{};
  auto O = Objects.find(A[1]);
  auto R = Requests.find(A[1]);
  if (O == Objects.end() || O->second.Kind != ObjectKind::Request ||
      O->second.Binding != B.Globals || R == Requests.end() ||
      R->second.Completed)
    return requestError("invalid, foreign or completed framework request");
  if (R->second.Completing)
    return requestError("request completion in progress");
  if (Name == "WdfRequestMarkCancelableEx") {
    if (!A[2])
      return requestError("marking cancelable requires a cancel callback");
    if (R->second.Cancellation != CancelState::Unmarked)
      return Result{ControlInvalidDeviceRequest};
    if (!RequestsHost.IsCanceled)
      return requestError("cancellation host is unavailable");
    auto Canceled = RequestsHost.IsCanceled(R->second.IRP);
    if (!Canceled)
      return Canceled.takeError();
    // Unlike the legacy void MarkCancelable API, Ex never delivers a cancel
    // callback for an IRP that was already canceled when registration began.
    if (*Canceled)
      return Result{RequestCancelled};
    if (O->second.InternalReferences == UINT64_MAX)
      return requestError("internal reference count overflow");
    ++O->second.InternalReferences;
    R->second.CancelRoutine = A[2];
    R->second.Cancellation = CancelState::Marked;
    return Result{0};
  }
  if (Name == "WdfRequestUnmarkCancelable") {
    if (R->second.Cancellation == CancelState::Unmarked)
      return Result{ControlInvalidDeviceRequest};
    if (R->second.Cancellation != CancelState::Marked)
      return Result{RequestCancelled};
    if (!O->second.InternalReferences)
      return requestError("cancelable request lost its callback reference");
    --O->second.InternalReferences;
    R->second.CancelRoutine = 0;
    R->second.Cancellation = CancelState::Unmarked;
    return Result{0};
  }
  if (Name == "WdfRequestIsCanceled") {
    // The public verifier requires an owned, noncancelable request here.
    // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdfrequest/nf-wdfrequest-wdfrequestiscanceled
    if (R->second.Cancellation != CancelState::Unmarked)
      return requestError("IsCanceled requires an unmarked request");
    if (!RequestsHost.IsCanceled)
      return requestError("cancellation host is unavailable");
    auto Canceled = RequestsHost.IsCanceled(R->second.IRP);
    if (!Canceled)
      return Canceled.takeError();
    return Result{*Canceled ? 1 : 0};
  }
  if (Name == "WdfRequestComplete" ||
      Name == "WdfRequestCompleteWithInformation") {
    if (R->second.Cancellation == CancelState::Marked ||
        R->second.Cancellation == CancelState::Queued)
      return requestError(
          "completion requires successful UnmarkCancelable or delivered "
          "EvtRequestCancel");
    if (!RequestsHost.Complete || !RequestsHost.Information ||
        !RequestsHost.ValidateCompletion)
      return requestError("completion host is unavailable");
    uint64_t Information = 0;
    if (Name == "WdfRequestCompleteWithInformation") {
      Information = A[3];
    } else {
      auto Value = RequestsHost.Information(R->second.IRP);
      if (!Value)
        return Value.takeError();
      Information = *Value;
    }
    if (auto E = RequestsHost.ValidateCompletion(R->second.IRP, uint32_t(A[2]),
                                                 Information))
      return E;
    // FxRequest::CompleteInternal performs EarlyDispose before giving up the
    // IRP. A cleanup callback may still use its buffers or release resources
    // embedded in them; explicit references only extend the object lifetime.
    R->second.Completing = true;
    R->second.CompletionStatus = uint32_t(A[2]);
    R->second.CompletionInformation = Information;
    std::vector<Step> Steps;
    if (auto E = planDelete(A[1], Steps))
      return E;
    auto Destruction =
        std::find_if(Steps.begin(), Steps.end(), [&](const Step &S) {
          return S.Kind == StepKind::TryDestroy && S.Object == A[1];
        });
    Steps.insert(Destruction, {StepKind::CompleteRequest, A[1]});
    auto Value = start(std::move(Steps));
    if (!Value)
      return Value.takeError();
    return Result{*Value};
  }
  if (!RequestsHost.View)
    return requestError("request view host is unavailable");
  auto View = RequestsHost.View(R->second.IRP);
  if (!View)
    return View.takeError();
  if (Name == "WdfRequestGetParameters") {
    auto Size = read(A[2], 2);
    if (!Size)
      return Size.takeError();
    if (*Size != RequestParametersSize)
      return requestError("unsupported WDF_REQUEST_PARAMETERS size");
    if (auto E = writable(A[2], RequestParametersSize))
      return E;
    if (auto E =
            Memory.write(A[2], std::vector<uint8_t>(RequestParametersSize)))
      return E;
    if (auto E = Memory.writeInteger(A[2], RequestParametersSize, 2))
      return E;
    if (auto E =
            Memory.writeInteger(A[2] + RequestParametersType, View->Major, 4))
      return E;
    if (View->Major == RequestMajorDeviceControl) {
      if (auto E = Memory.writeInteger(A[2] + RequestParametersLength,
                                       View->OutputLength, 8))
        return E;
      if (auto E = Memory.writeInteger(A[2] + RequestParametersInputLength,
                                       View->InputLength, 8))
        return E;
      if (auto E = Memory.writeInteger(A[2] + RequestParametersControlCode,
                                       View->ControlCode, 4))
        return E;
    } else {
      const auto Length = View->Major == RequestMajorRead ? View->OutputLength
                                                          : View->InputLength;
      if (auto E =
              Memory.writeInteger(A[2] + RequestParametersLength, Length, 8))
        return E;
      if (auto E = Memory.writeInteger(A[2] + RequestParametersOffset,
                                       View->ByteOffset, 8))
        return E;
    }
    return Result{0};
  }
  if (auto E = writable(A[3], 8))
    return E;
  if (A[4])
    if (auto E = writable(A[4], 8))
      return E;
  if (auto E = Memory.writeInteger(A[3], 0, 8))
    return E;
  if (A[4])
    if (auto E = Memory.writeInteger(A[4], 0, 8))
      return E;
  const bool Output = Name == "WdfRequestRetrieveOutputBuffer";
  if ((!Output && View->Major == RequestMajorRead) ||
      (Output && View->Major == RequestMajorWrite))
    return Result{ControlInvalidDeviceRequest};
  const auto Length = Output ? View->OutputLength : View->InputLength;
  if (!Length || Length < A[2])
    return Result{RequestBufferTooSmall};
  if (!RequestsHost.Buffer)
    return requestError("request buffer host is unavailable");
  auto Buffer = RequestsHost.Buffer(R->second.IRP, Output);
  if (!Buffer)
    return Buffer.takeError();
  if (!*Buffer)
    return Result{windows::StatusInsufficientResources};
  if (auto E = Memory.writeInteger(A[3], *Buffer, 8))
    return E;
  if (A[4])
    if (auto E = Memory.writeInteger(A[4], Length, 8))
      return E;
  return Result{0};
}
} // namespace neverd::emulation
