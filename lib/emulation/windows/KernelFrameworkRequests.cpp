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

llvm::Error
KernelFramework::preflightCancellationToken(uint64_t EarlierCallbacks) const {
  if (!NextContinuation || EarlierCallbacks >= UINT64_MAX - NextContinuation)
    return requestError("cancellation continuation token capacity exhausted");
  const uint64_t Token = NextContinuation + EarlierCallbacks;
  if (Continuations.contains(Token) || CancelCallbacks.contains(Token))
    return requestError("cancellation continuation token is already owned");
  return llvm::Error::success();
}

llvm::Expected<bool>
KernelFramework::preflightRequestCancellation(uint64_t IRP,
                                              uint64_t EarlierCallbacks) const {
  const auto R =
      std::find_if(Requests.begin(), Requests.end(),
                   [&](const auto &Entry) { return Entry.second.IRP == IRP; });
  if (R != Requests.end() && R->second.Queued) {
    auto Q = Queues.find(R->second.Queue);
    if (R->second.Completed || R->second.Completing ||
        R->second.Cancellation != CancelState::Unmarked || Q == Queues.end() ||
        (Q->second.Dispatch != QueueDispatchManual &&
         Q->second.Dispatch != QueueDispatchParallel &&
         Q->second.Dispatch != QueueDispatchSequential) ||
        std::find(Q->second.Pending.begin(), Q->second.Pending.end(),
                  R->first) == Q->second.Pending.end())
      return requestError("queued request lost framework queue ownership");
    if (PendingCall)
      return requestError(
          "cancellation cannot replace a pending guest callback");
    const bool Notify = Q->second.CanceledOnQueue &&
                        (R->second.DeliveredOnce || R->second.Enqueued);
    if (!RequestsHost.IsCanceled ||
        (!Notify &&
         (!RequestsHost.ValidateCompletion || !RequestsHost.SetInformation ||
          !RequestsHost.Information || !RequestsHost.Complete)))
      return requestError("queued request cancellation host is unavailable");
    if (auto E = preflightCancellationToken(EarlierCallbacks))
      return E;
    // Even callback-free cancellation consumes a framework continuation.
    // Reserve one token and one scheduler slot conservatively at this boundary.
    return true;
  }
  if (R == Requests.end() || R->second.Completed || R->second.Completing ||
      R->second.Cancellation != CancelState::Marked)
    return false;
  if (PendingCall)
    return requestError("cancellation cannot replace a pending guest callback");
  if (!RequestsHost.IsCanceled)
    return requestError("cancellation host is unavailable");
  const auto O = Objects.find(R->first);
  if (O == Objects.end() || !O->second.InternalReferences ||
      !R->second.CancelRoutine)
    return requestError("cancelable request lost its callback reference");
  if (auto E = preflightCancellationToken(EarlierCallbacks))
    return E;
  return true;
}

llvm::Expected<std::optional<KernelFramework::GuestCall>>
KernelFramework::requestCancellation(uint64_t IRP) {
  auto GeneratesCallback = preflightRequestCancellation(IRP);
  if (!GeneratesCallback)
    return GeneratesCallback.takeError();
  if (!*GeneratesCallback)
    return std::optional<GuestCall>{};
  auto Canceled = RequestsHost.IsCanceled(IRP);
  if (!Canceled)
    return Canceled.takeError();
  if (!*Canceled)
    return requestError("cancellation requires the underlying IRP cancel flag");
  auto R =
      std::find_if(Requests.begin(), Requests.end(),
                   [&](const auto &Entry) { return Entry.second.IRP == IRP; });
  if (R->second.Queued) {
    auto Q = Queues.find(R->second.Queue);
    if (Q == Queues.end())
      return requestError("queued cancellation lost its framework queue");
    auto Pending =
        std::find(Q->second.Pending.begin(), Q->second.Pending.end(), R->first);
    if (Pending == Q->second.Pending.end())
      return requestError("queued cancellation lost its pending request");
    if (Q->second.CanceledOnQueue &&
        (R->second.DeliveredOnce || R->second.Enqueued)) {
      Q->second.Pending.erase(Pending);
      if (Q->second.Pending.empty())
        Q->second.ReadyPending = false;
      R->second.Queued = false;
      R->second.CanceledOnQueue = true;
      R->second.QueuedCallback = 0;
      R->second.QueuedArguments.clear();
      R->second.QueuedCompletionStatus.reset();
      auto Canceled = start({{StepKind::CanceledOnQueue, R->first}});
      if (!Canceled)
        return Canceled.takeError();
      return takeGuestCall();
    }
    if (auto E = RequestsHost.ValidateCompletion(IRP, RequestCancelled, 0))
      return E;
    if (auto E = RequestsHost.SetInformation(IRP, 0))
      return E;
    R->second.Completing = true;
    R->second.CompletionStatus = RequestCancelled;
    std::vector<Step> Steps;
    if (auto E = planDelete(R->first, Steps)) {
      R->second.Completing = false;
      return E;
    }
    auto Destruction =
        std::find_if(Steps.begin(), Steps.end(), [&](const Step &S) {
          return S.Kind == StepKind::TryDestroy && S.Object == R->first;
        });
    Steps.insert(Destruction, {StepKind::CompleteRequest, R->first});
    Q->second.Pending.erase(Pending);
    if (Q->second.Pending.empty())
      Q->second.ReadyPending = false;
    R->second.Queued = false;
    auto Completed = start(std::move(Steps));
    if (!Completed)
      return Completed.takeError();
    return takeGuestCall();
  }
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

llvm::Expected<KernelFramework::RequestDispatch>
KernelFramework::queueDispatch(uint64_t QueueHandle, uint64_t RequestHandle,
                               const RequestView &View) const {
  auto Q = Queues.find(QueueHandle);
  if (Q == Queues.end())
    return requestError("request queue has no framework identity");
  const auto &Queue = Q->second;
  uint64_t Callback = 0;
  uint64_t Length = 0;
  bool Specific = true;
  if (View.Major == RequestMajorRead) {
    Callback = Queue.Read;
    Length = View.OutputLength;
  } else if (View.Major == RequestMajorWrite) {
    Callback = Queue.Write;
    Length = View.InputLength;
  } else if (View.Major == RequestMajorDeviceControl) {
    Callback = Queue.DeviceControl;
  } else {
    return requestError("unsupported framework request major function");
  }
  if (!Callback) {
    Specific = false;
    Callback = Queue.Default;
  }
  if (!Callback)
    return RequestDispatch{0, {}, ControlInvalidDeviceRequest};
  if (!Queue.AllowZeroLength && !Length &&
      (View.Major == RequestMajorRead || View.Major == RequestMajorWrite))
    return RequestDispatch{0, {}, 0};
  RequestDispatch Dispatch;
  Dispatch.PC = Callback;
  Dispatch.Status = windows::StatusPending;
  Dispatch.Arguments = {QueueHandle, RequestHandle};
  if (Specific) {
    if (View.Major == RequestMajorDeviceControl) {
      Dispatch.Arguments.push_back(View.OutputLength);
      Dispatch.Arguments.push_back(View.InputLength);
      Dispatch.Arguments.push_back(View.ControlCode);
    } else {
      Dispatch.Arguments.push_back(Length);
    }
  }
  return Dispatch;
}

llvm::Expected<bool> KernelFramework::presentQueued(uint64_t QueueHandle,
                                                    uint64_t Token) {
  auto Q = Queues.find(QueueHandle);
  if (Q == Queues.end() || Q->second.Dispatch == QueueDispatchManual ||
      !Q->second.Dispatching || queuePnpHeld(Q->second) ||
      Q->second.Pending.empty())
    return false;
  if (PendingCall || !Continuations.contains(Token))
    return requestError("queued delivery lost its callback continuation");
  const uint32_t Limit = Q->second.Dispatch == QueueDispatchSequential
                             ? 1
                             : Q->second.PresentedLimit;
  const auto Presented =
      std::count_if(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.Queue == QueueHandle && !Entry.second.Queued &&
               !Entry.second.Completed;
      });
  // A sequential queue may also have driver-owned requests explicitly
  // retrieved from its pending list. They do not create another automatic
  // presentation slot until they complete or leave the queue.
  if (Presented >= Limit)
    return false;
  const uint64_t Handle = Q->second.Pending.front();
  auto R = Requests.find(Handle);
  if (R == Requests.end() || !R->second.Queued ||
      R->second.Queue != QueueHandle ||
      (!R->second.QueuedCallback && !R->second.QueuedCompletionStatus) ||
      (R->second.QueuedCallback && R->second.QueuedArguments.size() < 2))
    return requestError("automatic queue lost its pending delivery");
  if (R->second.QueuedCompletionStatus) {
    const uint32_t Status = *R->second.QueuedCompletionStatus;
    if (!RequestsHost.ValidateCompletion || !RequestsHost.SetInformation ||
        !RequestsHost.Information || !RequestsHost.Complete)
      return requestError("automatic queue completion host is unavailable");
    if (auto E = RequestsHost.ValidateCompletion(R->second.IRP, Status, 0))
      return E;
    if (auto E = RequestsHost.SetInformation(R->second.IRP, 0))
      return E;
    R->second.Completing = true;
    R->second.CompletionStatus = Status;
    std::vector<Step> Steps;
    if (auto E = planDelete(Handle, Steps)) {
      R->second.Completing = false;
      return E;
    }
    auto Destruction =
        std::find_if(Steps.begin(), Steps.end(), [&](const Step &S) {
          return S.Kind == StepKind::TryDestroy && S.Object == Handle;
        });
    Steps.insert(Destruction, {StepKind::CompleteRequest, Handle});
    Q->second.Pending.pop_front();
    R->second.Queued = false;
    R->second.QueuedCompletionStatus.reset();
    auto &Continuation = Continuations.at(Token);
    Continuation.Steps.insert(Continuation.Steps.begin() + Continuation.Index,
                              Steps.begin(), Steps.end());
    return false;
  }
  Q->second.Pending.pop_front();
  R->second.Queued = false;
  R->second.DeliveredOnce = true;
  PendingCall = GuestCall{Token, R->second.QueuedCallback,
                          std::move(R->second.QueuedArguments)};
  R->second.QueuedCallback = 0;
  return true;
}

llvm::Expected<std::optional<KernelFramework::RequestDispatch>>
KernelFramework::routeRequest(uint64_t WdmDevice, uint64_t IRP,
                              bool AfterCaller) {
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
  auto FileRoute = routeFileRequest(D->first, IRP, *View);
  if (!FileRoute)
    return FileRoute.takeError();
  if (*FileRoute)
    return *FileRoute;
  auto File = requestFileObject(D->first, View->File);
  if (!File)
    return File.takeError();
  auto CompleteImmediately =
      [&](uint32_t Status,
          uint32_t Dispatch) -> llvm::Expected<std::optional<RequestDispatch>> {
    if (auto E = RequestsHost.Complete(IRP, Status, 0))
      return E;
    return std::optional<RequestDispatch>{RequestDispatch{0, {}, Dispatch}};
  };
  auto Q = Queues.find(D->second.DefaultQueue);
  if (Q == Queues.end() && D->second.Filter)
    return requestError(
        "automatic non-file filter forwarding is outside this profile");
  if (Q == Queues.end())
    return CompleteImmediately(ControlInvalidDeviceRequest,
                               ControlInvalidDeviceRequest);
  if (Objects.at(Q->first).Deleting)
    return requestError("default queue is deleting");
  if (!Q->second.Accepting) {
    if (AfterCaller)
      return requestError(
          "caller-context queue drained during request routing");
    return CompleteImmediately(QueueInvalidDeviceState,
                               QueueInvalidDeviceState);
  }
  uint64_t ExistingHandle = 0;
  if (AfterCaller) {
    auto Caller = CallerRequests.find(IRP);
    if (Caller == CallerRequests.end())
      return requestError("caller-context continuation lost its request");
    ExistingHandle = Caller->second;
    auto R = Requests.find(ExistingHandle);
    if (R == Requests.end() || !R->second.InCallerContext ||
        !R->second.Enqueued || R->second.Device != D->first)
      return requestError("caller-context request was not enqueued");
    R->second.InCallerContext = false;
    R->second.Queue = Q->first;
    CallerRequests.erase(Caller);
  } else if (D->second.CallerContext) {
    if (auto E = RequestsHost.MarkPending(IRP))
      return E;
    Attributes Attrs;
    Attrs.Parent = Q->first;
    const uint64_t Globals = Objects.at(D->first).Binding;
    auto Handle = createObject(Globals, Attrs, false);
    if (!Handle)
      return Handle.takeError();
    Objects.at(*Handle).Kind = ObjectKind::Request;
    Requests.emplace(*Handle, Request{IRP, 0, D->first, true});
    Requests.at(*Handle).File = *File;
    CallerRequests.emplace(IRP, *Handle);
    return std::optional<RequestDispatch>{
        RequestDispatch{D->second.CallerContext,
                        {D->first, *Handle},
                        windows::StatusPending,
                        true}};
  }
  const bool Manual = Q->second.Dispatch == QueueDispatchManual;
  if (Manual && View->Major != RequestMajorRead &&
      View->Major != RequestMajorWrite &&
      View->Major != RequestMajorDeviceControl)
    return requestError("unsupported framework request major function");
  auto Planned = Manual ? llvm::Expected<RequestDispatch>(RequestDispatch{})
                        : queueDispatch(Q->first, 0, *View);
  if (!Planned)
    return Planned.takeError();
  if (AfterCaller && !Manual && !Planned->PC)
    return requestError("caller-context queue completion without a guest I/O "
                        "callback is outside this profile");
  auto &Queue = Q->second;
  bool WaitForSlot = !Queue.Dispatching || queuePnpHeld(Queue);
  if (Queue.Dispatch == QueueDispatchSequential ||
      (Queue.Dispatch == QueueDispatchParallel &&
       Queue.PresentedLimit != UINT32_MAX)) {
    const uint32_t Limit =
        Queue.Dispatch == QueueDispatchSequential ? 1 : Queue.PresentedLimit;
    const auto Presented =
        std::count_if(Requests.begin(), Requests.end(), [&](const auto &Entry) {
          return Entry.first != ExistingHandle &&
                 Entry.second.Queue == Q->first && !Entry.second.Queued &&
                 !Entry.second.Completed;
        });
    WaitForSlot |= Presented >= Limit || !Queue.Pending.empty();
  }
  // FxIoQueue::QueueRequest marks accepted IRPs pending before dispatching.
  // That status persists even when delivery completes the IRP immediately.
  if (auto E = RequestsHost.MarkPending(IRP))
    return E;
  if (!Manual && !Planned->PC && !WaitForSlot)
    return CompleteImmediately(Planned->Status, windows::StatusPending);
  uint64_t Handle = ExistingHandle;
  if (!Handle) {
    Attributes Attrs;
    Attrs.Parent = Q->first;
    const uint64_t Globals = Objects.at(D->first).Binding;
    auto Created = createObject(Globals, Attrs, false);
    if (!Created)
      return Created.takeError();
    Handle = *Created;
    Objects.at(Handle).Kind = ObjectKind::Request;
    Requests.emplace(Handle, Request{IRP, Q->first, D->first});
    Requests.at(Handle).File = *File;
  }
  if (Manual || WaitForSlot) {
    auto &Pending = Requests.at(Handle);
    Pending.Queued = true;
    if (!Manual) {
      if (Planned->PC) {
        Planned->Arguments[1] = Handle;
        Pending.QueuedCallback = Planned->PC;
        Pending.QueuedArguments = std::move(Planned->Arguments);
      } else {
        Pending.QueuedCompletionStatus = Planned->Status;
      }
    }
    const bool WasEmpty = Queue.Pending.empty();
    Queue.Pending.push_back(Handle);
    if (Manual && WasEmpty && Queue.ReadyNotify)
      Queue.ReadyPending = true;
    return std::optional<RequestDispatch>{
        RequestDispatch{0, {}, windows::StatusPending}};
  }
  RequestDispatch Dispatch = std::move(*Planned);
  Dispatch.Arguments[1] = Handle;
  Requests.at(Handle).DeliveredOnce = true;
  return std::optional<RequestDispatch>{std::move(Dispatch)};
}

llvm::Expected<KernelFramework::RequestDispatch>
KernelFramework::continueCallerContext(uint64_t IRP) {
  auto Caller = CallerRequests.find(IRP);
  if (Caller == CallerRequests.end())
    return requestError("no caller-context callback owns this request");
  auto R = Requests.find(Caller->second);
  if (R == Requests.end() || R->second.Completed) {
    CallerRequests.erase(Caller);
    return RequestDispatch{0, {}, windows::StatusPending};
  }
  if (!R->second.Enqueued)
    return requestError(
        "caller-context callback returned without enqueue or completion");
  auto D = Devices.find(R->second.Device);
  if (D == Devices.end())
    return requestError("caller-context request lost its device");
  auto Routed = routeRequest(D->second.Wdm, IRP, true);
  if (!Routed)
    return Routed.takeError();
  if (!*Routed)
    return requestError("caller-context continuation lost framework routing");
  return std::move(**Routed);
}

llvm::Error KernelFramework::writeRequestParameters(uint64_t Address,
                                                    const RequestView &View) {
  auto Size = read(Address, 2);
  if (!Size)
    return Size.takeError();
  if (*Size != RequestParametersSize)
    return requestError("unsupported WDF_REQUEST_PARAMETERS size");
  if (auto E = writable(Address, RequestParametersSize))
    return E;
  if (auto E =
          Memory.write(Address, std::vector<uint8_t>(RequestParametersSize)))
    return E;
  if (auto E = Memory.writeInteger(Address, RequestParametersSize, 2))
    return E;
  if (auto E =
          Memory.writeInteger(Address + RequestParametersType, View.Major, 4))
    return E;
  if (View.Major == RequestMajorDeviceControl) {
    if (auto E = Memory.writeInteger(Address + RequestParametersLength,
                                     View.OutputLength, 8))
      return E;
    if (auto E = Memory.writeInteger(Address + RequestParametersInputLength,
                                     View.InputLength, 8))
      return E;
    if (auto E = Memory.writeInteger(Address + RequestParametersControlCode,
                                     View.ControlCode, 4))
      return E;
  } else {
    const auto Length =
        View.Major == RequestMajorRead ? View.OutputLength : View.InputLength;
    if (auto E =
            Memory.writeInteger(Address + RequestParametersLength, Length, 8))
      return E;
    if (auto E = Memory.writeInteger(Address + RequestParametersOffset,
                                     View.ByteOffset, 8))
      return E;
  }
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::callRequest(llvm::StringRef Name, Binding &B,
                             llvm::ArrayRef<uint64_t> A) {
  using Result = std::optional<uint64_t>;
  if (Name == api::WdfMemoryGetBuffer) {
    auto O = Objects.find(A[1]);
    auto M = RequestMemories.find(A[1]);
    if (O == Objects.end() || O->second.Kind != ObjectKind::Memory ||
        O->second.Binding != B.Globals || M == RequestMemories.end() ||
        !M->second.Active)
      return requestError("invalid or completed framework request memory");
    if (A[2]) {
      if (auto E = writable(A[2], sizeof(uint64_t)))
        return E;
      if (auto E =
              Memory.writeInteger(A[2], M->second.Length, sizeof(uint64_t)))
        return E;
    }
    return Result{M->second.Buffer};
  }
  if (Name != api::WdfRequestComplete &&
      Name != api::WdfRequestCompleteWithInformation &&
      Name != api::WdfRequestStopAcknowledge &&
      Name != api::WdfRequestGetParameters &&
      Name != api::WdfRequestRetrieveInputBuffer &&
      Name != api::WdfRequestRetrieveOutputBuffer &&
      Name != api::WdfRequestRetrieveInputMemory &&
      Name != api::WdfRequestRetrieveOutputMemory &&
      Name != api::WdfRequestRetrieveUnsafeUserInputBuffer &&
      Name != api::WdfRequestRetrieveUnsafeUserOutputBuffer &&
      Name != api::WdfRequestProbeAndLockUserBufferForRead &&
      Name != api::WdfRequestProbeAndLockUserBufferForWrite &&
      Name != api::WdfRequestMarkCancelable &&
      Name != api::WdfRequestMarkCancelableEx &&
      Name != api::WdfRequestUnmarkCancelable &&
      Name != api::WdfRequestIsCanceled &&
      Name != api::WdfRequestForwardToIoQueue && Name != api::WdfRequestRequeue)
    return Result{};
  auto O = Objects.find(A[1]);
  auto R = Requests.find(A[1]);
  if (O == Objects.end() || O->second.Kind != ObjectKind::Request ||
      O->second.Binding != B.Globals || R == Requests.end() ||
      R->second.Completed)
    return requestError("invalid, foreign or completed framework request");
  if (R->second.Completing)
    return requestError("request completion in progress");
  if (Name == api::WdfRequestStopAcknowledge) {
    if (A[2] > 1)
      return requestError(
          "stop acknowledgment requires a Boolean requeue flag");
    const auto Active = std::find_if(
        PnpTransitions.begin(), PnpTransitions.end(), [&](const auto &Entry) {
          return Entry.second.Current.Phase == PnpPhase::IoStop &&
                 Entry.second.Current.Request == A[1] &&
                 !Entry.second.WaitingForRequests;
        });
    if (Active == PnpTransitions.end() || R->second.StopAcknowledged ||
        R->second.InCallerContext || R->second.Queued)
      return requestError(
          "stop acknowledgment requires the active EvtIoStop request");
    auto Q = Queues.find(R->second.Queue);
    if (Q == Queues.end() || !Q->second.PowerManaged ||
        Q->first != Active->second.Current.Queue)
      return requestError("stop acknowledgment lost its power-managed queue");
    if (A[2]) {
      if (R->second.Cancellation != CancelState::Unmarked)
        return requestError("requeue requires an unmarked request");
      std::optional<RequestDispatch> Dispatch;
      if (Q->second.Dispatch != QueueDispatchManual) {
        if (!RequestsHost.View)
          return requestError("request inspection host is unavailable");
        auto View = RequestsHost.View(R->second.IRP);
        if (!View)
          return View.takeError();
        auto Planned = queueDispatch(Q->first, A[1], *View);
        if (!Planned)
          return Planned.takeError();
        Dispatch = std::move(*Planned);
      }
      R->second.Queued = true;
      if (Dispatch) {
        R->second.QueuedCallback = Dispatch->PC;
        R->second.QueuedArguments = std::move(Dispatch->Arguments);
        if (!Dispatch->PC)
          R->second.QueuedCompletionStatus = Dispatch->Status;
      }
      const bool WasEmpty = Q->second.Pending.empty();
      Q->second.Pending.push_back(A[1]);
      if (WasEmpty && Q->second.Dispatch == QueueDispatchManual &&
          Q->second.ReadyNotify)
        Q->second.ReadyPending = true;
    } else {
      if (!Q->second.IoResume)
        return requestError(
            "retained stop acknowledgment requires EvtIoResume");
      R->second.PowerSuspended = true;
    }
    R->second.StopAcknowledged = true;
    return Result{0};
  }
  if (Name == api::WdfRequestForwardToIoQueue ||
      Name == api::WdfRequestRequeue) {
    const bool Requeue = Name == api::WdfRequestRequeue;
    const uint64_t Target = Requeue ? R->second.Queue : A[2];
    auto DestinationObject = Objects.find(Target);
    auto Destination = Queues.find(Target);
    if (DestinationObject == Objects.end() ||
        DestinationObject->second.Kind != ObjectKind::Queue ||
        DestinationObject->second.Binding != B.Globals ||
        Destination == Queues.end() || DestinationObject->second.Deleting)
      return requestError("invalid, foreign or deleting destination queue");
    if (!Destination->second.Accepting)
      return Result{QueueBusy};
    if (R->second.InCallerContext || R->second.Queued ||
        R->second.CanceledOnQueue || !R->second.Queue ||
        (Requeue ? Destination->second.Dispatch != QueueDispatchManual
                 : R->second.Queue == Target) ||
        R->second.Device != Destination->second.Device ||
        R->second.Cancellation != CancelState::Unmarked)
      return Result{ControlInvalidDeviceRequest};
    std::optional<RequestDispatch> ForwardDispatch;
    if (!Requeue && Destination->second.Dispatch != QueueDispatchManual) {
      if (!RequestsHost.View)
        return requestError("request inspection host is unavailable");
      auto View = RequestsHost.View(R->second.IRP);
      if (!View)
        return View.takeError();
      auto Planned = queueDispatch(Target, A[1], *View);
      if (!Planned)
        return Planned.takeError();
      if (!Planned->PC &&
          (!RequestsHost.ValidateCompletion || !RequestsHost.SetInformation ||
           !RequestsHost.Information || !RequestsHost.Complete))
        return requestError("automatic queue completion host is unavailable");
      ForwardDispatch = std::move(*Planned);
    }
    if (!Requeue) {
      if (PendingCall)
        return requestError("forwarding cannot replace a pending callback");
      if (auto E = preflightCancellationToken(0))
        return E;
    }
    auto Source = Objects.find(R->second.Queue);
    if (Source == Objects.end() || Source->second.Kind != ObjectKind::Queue ||
        Source->second.Deleting || O->second.Parent != R->second.Queue)
      return requestError("request lost its source queue ownership");
    auto Child = std::find(Source->second.Children.begin(),
                           Source->second.Children.end(), A[1]);
    if (Child == Source->second.Children.end())
      return requestError("source queue lost its request child");
    const uint64_t SourceHandle = R->second.Queue;
    if (!RequestsHost.IsCanceled)
      return requestError("cancellation host is unavailable");
    auto AlreadyCanceled = RequestsHost.IsCanceled(R->second.IRP);
    if (!AlreadyCanceled)
      return AlreadyCanceled.takeError();
    if (!Requeue) {
      DestinationObject->second.Children.push_back(A[1]);
      Source->second.Children.erase(Child);
      O->second.Parent = Target;
      R->second.Queue = Target;
    }
    R->second.Queued = true;
    if (ForwardDispatch) {
      R->second.QueuedCallback = ForwardDispatch->PC;
      R->second.QueuedArguments = std::move(ForwardDispatch->Arguments);
      if (!ForwardDispatch->PC)
        R->second.QueuedCompletionStatus = ForwardDispatch->Status;
    }
    const bool WasEmpty = Destination->second.Pending.empty();
    if (Requeue)
      Destination->second.Pending.push_front(A[1]);
    else
      Destination->second.Pending.push_back(A[1]);
    if (WasEmpty && Destination->second.Dispatch == QueueDispatchManual &&
        Destination->second.ReadyNotify)
      Destination->second.ReadyPending = true;
    if (*AlreadyCanceled) {
      // A request canceled before forwarding is subject to framework queue
      // cancellation as soon as the new queue takes ownership.
      auto Call = requestCancellation(R->second.IRP);
      if (!Call)
        return Call.takeError();
      if (*Call) {
        if (!Requeue)
          Continuations.at((**Call).Token)
              .Steps.push_back({StepKind::PresentQueue, SourceHandle});
        PendingCall = std::move(**Call);
      } else if (!Requeue) {
        const uint64_t Token = NextContinuation++;
        Continuations.emplace(
            Token, Continuation{{{StepKind::PresentQueue, SourceHandle}}});
        auto Next = advance(Token);
        if (!Next)
          return Next.takeError();
      }
    } else if (!Requeue) {
      const uint64_t Token = NextContinuation++;
      Continuations.emplace(
          Token, Continuation{{{StepKind::PresentQueue, SourceHandle}}});
      auto Delivered = presentQueued(Target, Token);
      if (!Delivered)
        return Delivered.takeError();
      if (!*Delivered) {
        auto Next = advance(Token);
        if (!Next)
          return Next.takeError();
      }
    }
    if (auto E = flushReadyNotifications())
      return E;
    return Result{0};
  }
  if (R->second.Queued)
    return requestError("framework owns the request in a manual queue");
  if (Name == api::WdfRequestMarkCancelable ||
      Name == api::WdfRequestMarkCancelableEx) {
    const bool Legacy = Name == api::WdfRequestMarkCancelable;
    if (!A[2])
      return requestError("marking cancelable requires a cancel callback");
    if (R->second.Cancellation != CancelState::Unmarked) {
      if (Legacy)
        return requestError("legacy marking requires an unmarked request");
      return Result{ControlInvalidDeviceRequest};
    }
    if (!RequestsHost.IsCanceled)
      return requestError("cancellation host is unavailable");
    auto Canceled = RequestsHost.IsCanceled(R->second.IRP);
    if (!Canceled)
      return Canceled.takeError();
    // Unlike the legacy void MarkCancelable API, Ex never delivers a cancel
    // callback for an IRP that was already canceled when registration began.
    if (*Canceled && !Legacy)
      return Result{RequestCancelled};
    if (*Canceled && PendingCall)
      return requestError(
          "cancellation cannot replace a pending guest callback");
    if (*Canceled)
      if (auto E = preflightCancellationToken(0))
        return E;
    if (O->second.InternalReferences == UINT64_MAX)
      return requestError("internal reference count overflow");
    ++O->second.InternalReferences;
    R->second.CancelRoutine = A[2];
    R->second.Cancellation = CancelState::Marked;
    if (*Canceled) {
      // RequestCancelable(..., FALSE) takes the same cancellation reference
      // even when insertion finds an already canceled IRP. In this profile,
      // PASSIVE_LEVEL and SynchronizationNone let DispatchEvents invoke the
      // driver recursively before the legacy void API returns.
      // https://github.com/microsoft/Windows-Driver-Frameworks/blob/b6191d9543441329154da32f7ab9bdd97228dd3c/src/framework/shared/irphandlers/io/fxioqueue.cpp#L2195-L2223
      auto Call = requestCancellation(R->second.IRP);
      if (!Call)
        return Call.takeError();
      if (!*Call)
        return requestError("legacy cancellation lost its guest callback");
      if (auto E = beginCancelCallback((**Call).Token))
        return E;
      // Publishing this as the current API's child preserves caller state and
      // keeps the API suspended through callback waits, nested completion and
      // the final CancelReturned/destroy continuation. No worker is queued.
      PendingCall = std::move(**Call);
    }
    return Result{0};
  }
  if (Name == api::WdfRequestUnmarkCancelable) {
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
  if (Name == api::WdfRequestIsCanceled) {
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
  if (Name == api::WdfRequestComplete ||
      Name == api::WdfRequestCompleteWithInformation) {
    if (R->second.Cancellation == CancelState::Marked ||
        R->second.Cancellation == CancelState::Queued)
      return requestError(
          "completion requires successful UnmarkCancelable or delivered "
          "EvtRequestCancel");
    if (!RequestsHost.Complete || !RequestsHost.Information ||
        !RequestsHost.SetInformation || !RequestsHost.ValidateCompletion)
      return requestError("completion host is unavailable");
    uint64_t Information = 0;
    if (Name == api::WdfRequestCompleteWithInformation) {
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
    // CompleteWithInformation publishes to the IRP before EarlyDispose, so a
    // cleanup callback holding the raw packet observes the supplied value.
    // Keep that packet authoritative through cleanup and final completion.
    // https://github.com/microsoft/Windows-Driver-Frameworks/blob/b6191d9543441329154da32f7ab9bdd97228dd3c/src/framework/shared/inc/private/common/fxrequest.hpp#L810-L821
    if (Name == api::WdfRequestCompleteWithInformation)
      if (auto E = RequestsHost.SetInformation(R->second.IRP, Information))
        return E;
    // FxRequest::CompleteInternal performs EarlyDispose before giving up the
    // IRP. A cleanup callback may still use its buffers or release resources
    // embedded in them; explicit references only extend the object lifetime.
    R->second.Completing = true;
    R->second.CompletionStatus = uint32_t(A[2]);
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
  const bool InputMemory = Name == api::WdfRequestRetrieveInputMemory;
  const bool OutputMemory = Name == api::WdfRequestRetrieveOutputMemory;
  if (InputMemory || OutputMemory) {
    if (auto E = writable(A[2], sizeof(uint64_t)))
      return E;
    if (auto E = Memory.writeInteger(A[2], 0, sizeof(uint64_t)))
      return E;
    if (View->Neither || (InputMemory && View->Major == RequestMajorRead) ||
        (OutputMemory && View->Major == RequestMajorWrite))
      return Result{ControlInvalidDeviceRequest};
    const uint32_t Length =
        OutputMemory ? View->OutputLength : View->InputLength;
    if (!Length)
      return Result{RequestBufferTooSmall};
    auto Existing = std::find_if(
        RequestMemories.begin(), RequestMemories.end(), [&](const auto &Entry) {
          const auto &Memory = Entry.second;
          return Memory.Request == A[1] && Memory.Active &&
                 Memory.Output == OutputMemory;
        });
    if (Existing != RequestMemories.end()) {
      if (auto E = Memory.writeInteger(A[2], Existing->first, sizeof(uint64_t)))
        return E;
      return Result{0};
    }
    if (!RequestsHost.Buffer)
      return requestError("request buffer host is unavailable");
    auto Buffer = RequestsHost.Buffer(R->second.IRP, OutputMemory);
    if (!Buffer)
      return Buffer.takeError();
    if (!*Buffer)
      return Result{windows::StatusInsufficientResources};
    Attributes Attrs;
    Attrs.Parent = A[1];
    auto Created = createObject(B.Globals, Attrs, false);
    if (!Created)
      return Created.takeError();
    const uint64_t Handle = *Created;
    Objects.at(Handle).Kind = ObjectKind::Memory;
    RequestMemories.emplace(Handle, RequestMemory{A[1], std::nullopt, *Buffer,
                                                  Length, true, OutputMemory});
    if (auto E = Memory.writeInteger(A[2], Handle, sizeof(uint64_t)))
      return E;
    return Result{0};
  }
  const bool UnsafeInput = Name == api::WdfRequestRetrieveUnsafeUserInputBuffer;
  const bool UnsafeOutput =
      Name == api::WdfRequestRetrieveUnsafeUserOutputBuffer;
  if (UnsafeInput || UnsafeOutput) {
    if (auto E = writable(A[3], sizeof(uint64_t)))
      return E;
    if (A[4])
      if (auto E = writable(A[4], sizeof(uint64_t)))
        return E;
    if (auto E = Memory.writeInteger(A[3], 0, sizeof(uint64_t)))
      return E;
    if (A[4])
      if (auto E = Memory.writeInteger(A[4], 0, sizeof(uint64_t)))
        return E;
    if (!R->second.InCallerContext || !View->Neither ||
        (UnsafeInput && View->Major == RequestMajorRead) ||
        (UnsafeOutput && View->Major == RequestMajorWrite))
      return Result{ControlInvalidDeviceRequest};
    const uint32_t Length =
        UnsafeOutput ? View->OutputLength : View->InputLength;
    const uint64_t Buffer = UnsafeOutput ? View->UserOutput : View->UserInput;
    if (!Buffer || !Length || Length < A[2])
      return Result{RequestBufferTooSmall};
    if (auto E = Memory.writeInteger(A[3], Buffer, sizeof(uint64_t)))
      return E;
    if (A[4])
      if (auto E = Memory.writeInteger(A[4], Length, sizeof(uint64_t)))
        return E;
    return Result{0};
  }
  const bool ProbeRead = Name == api::WdfRequestProbeAndLockUserBufferForRead;
  const bool ProbeWrite = Name == api::WdfRequestProbeAndLockUserBufferForWrite;
  if (ProbeRead || ProbeWrite) {
    if (auto E = writable(A[4], sizeof(uint64_t)))
      return E;
    if (auto E = Memory.writeInteger(A[4], 0, sizeof(uint64_t)))
      return E;
    if (!R->second.InCallerContext)
      return Result{RequestAccessViolation};
    if (!RequestsHost.ProbeAndLock || !RequestsHost.ReleaseUserBuffer)
      return requestError("user-buffer lock host is unavailable");
    auto Locked =
        RequestsHost.ProbeAndLock(R->second.IRP, A[2], A[3], ProbeWrite);
    if (!Locked)
      return Locked.takeError();
    if (Locked->Status)
      return Result{Locked->Status};
    Attributes Attrs;
    Attrs.Parent = A[1];
    auto Handle = createObject(B.Globals, Attrs, false);
    if (!Handle)
      return llvm::joinErrors(Handle.takeError(),
                              RequestsHost.ReleaseUserBuffer(Locked->MDL));
    Objects.at(*Handle).Kind = ObjectKind::Memory;
    RequestMemories.emplace(
        *Handle, RequestMemory{A[1], Locked->MDL, Locked->Buffer, A[3]});
    if (auto E = Memory.writeInteger(A[4], *Handle, sizeof(uint64_t)))
      return E;
    return Result{0};
  }
  if (Name == api::WdfRequestGetParameters) {
    if (auto E = writeRequestParameters(A[2], *View))
      return E;
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
  const bool Output = Name == api::WdfRequestRetrieveOutputBuffer;
  if (View->Neither)
    return Result{ControlInvalidDeviceRequest};
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
