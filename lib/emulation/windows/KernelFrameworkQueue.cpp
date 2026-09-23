//===- KernelFrameworkQueue.cpp - KMDF control queue state ----------------=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original model of KMDF queue creation and device association. Public ABI:
/// Microsoft's wdfio.h 1.33; validation and ownership follow fxioqueueapi.cpp,
/// fxioqueue.cpp and fxpkgio.cpp at b6191d9543441329154da32f7ab9bdd97228dd3c.
/// Configuration never invokes callbacks or completes an underlying WDM IRP.
///
//===----------------------------------------------------------------------===//

#include "KernelFramework.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace framework;

llvm::Error invalidQueue(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF queue: " + Message);
}
} // namespace

bool KernelFramework::queuePnpHeld(const Queue &Q) const {
  return Q.PowerManaged && Devices.at(Q.Device).PowerQueuesHeld;
}

llvm::Expected<std::optional<uint64_t>>
KernelFramework::callQueue(llvm::StringRef Name, Binding &B,
                           llvm::ArrayRef<uint64_t> A) {
  if (Name != api::WdfIoQueueCreate && Name != api::WdfDeviceGetDefaultQueue &&
      Name != api::WdfIoQueueGetDevice && Name != api::WdfIoQueueGetState &&
      Name != api::WdfIoQueueStop && Name != api::WdfIoQueueStart &&
      Name != api::WdfIoQueueStopSynchronously &&
      Name != api::WdfIoQueueDrain && Name != api::WdfIoQueueStopAndPurge &&
      Name != api::WdfIoQueueStopAndPurgeSynchronously &&
      Name != api::WdfIoQueueDrainSynchronously &&
      Name != api::WdfIoQueuePurge &&
      Name != api::WdfIoQueuePurgeSynchronously &&
      Name != api::WdfIoQueueReadyNotify &&
      Name != api::WdfIoQueueRetrieveNextRequest &&
      Name != api::WdfIoQueueRetrieveRequestByFileObject &&
      Name != api::WdfIoQueueFindRequest &&
      Name != api::WdfIoQueueRetrieveFoundRequest)
    return std::optional<uint64_t>{};

  const auto OI = Objects.find(A[1]);
  const auto Kind =
      Name == api::WdfIoQueueCreate || Name == api::WdfDeviceGetDefaultQueue
          ? ObjectKind::Device
          : ObjectKind::Queue;
  if (OI == Objects.end() || OI->second.Binding != B.Globals ||
      OI->second.Kind != Kind)
    return invalidQueue("invalid, foreign or wrong-kind object handle");

  const auto FileBelongsToQueue = [&](uint64_t File, const Queue &Q) {
    auto Object = Objects.find(File);
    auto State = FileObjects.find(File);
    return Object != Objects.end() && Object->second.Binding == B.Globals &&
           Object->second.Kind == ObjectKind::File &&
           !Object->second.Deleting && State != FileObjects.end() &&
           State->second.Device == Q.Device;
  };
  const auto FindPendingFile =
      [&](Queue &Q, std::deque<uint64_t>::iterator Begin,
          uint64_t File) -> llvm::Expected<std::deque<uint64_t>::iterator> {
    for (auto It = Begin; It != Q.Pending.end(); ++It) {
      auto Request = Requests.find(*It);
      auto Object = Objects.find(*It);
      if (Request == Requests.end() || Object == Objects.end() ||
          !Request->second.Queued || Request->second.Queue != A[1] ||
          Object->second.Binding != B.Globals ||
          Object->second.Kind != ObjectKind::Request)
        return invalidQueue("queue lost a pending request");
      if (Request->second.File == File)
        return It;
    }
    return Q.Pending.end();
  };
  const auto RetrievePending =
      [&](Queue &Q, std::deque<uint64_t>::iterator Position,
          uint64_t Output) -> llvm::Expected<std::optional<uint64_t>> {
    const uint64_t Handle = *Position;
    auto Request = Requests.find(Handle);
    auto Object = Objects.find(Handle);
    if (Request == Requests.end() || Object == Objects.end() ||
        !Request->second.Queued || Request->second.Queue != A[1] ||
        Object->second.Binding != B.Globals ||
        Object->second.Kind != ObjectKind::Request)
      return invalidQueue("queue lost a pending request");
    if (auto E = Memory.writeInteger(Output, Handle, sizeof(uint64_t)))
      return E;
    Q.Pending.erase(Position);
    if (Q.Pending.empty())
      Q.ReadyPending = false;
    Request->second.Queued = false;
    Request->second.DeliveredOnce = true;
    Request->second.QueuedCallback = 0;
    Request->second.QueuedArguments.clear();
    Request->second.QueuedCompletionStatus.reset();
    return std::optional<uint64_t>{0};
  };

  if (Name == api::WdfIoQueueReadyNotify) {
    auto Q = Queues.find(A[1]);
    if (Q == Queues.end() || OI->second.Deleting)
      return invalidQueue("queue has no live framework identity");
    if (Q->second.Dispatch != QueueDispatchManual ||
        (A[2] && Q->second.ReadyNotify) ||
        (!A[2] && (!Q->second.ReadyNotify || Q->second.Dispatching)))
      return std::optional<uint64_t>{ControlInvalidDeviceRequest};
    Q->second.ReadyNotify = A[2];
    Q->second.ReadyContext = A[2] ? A[3] : 0;
    Q->second.ReadyPending = A[2] && Q->second.Dispatching &&
                             !queuePnpHeld(Q->second) &&
                             !Q->second.Pending.empty();
    if (auto E = flushReadyNotifications())
      return E;
    return std::optional<uint64_t>{0};
  }

  if (Name == api::WdfIoQueueGetState || Name == api::WdfIoQueueStop ||
      Name == api::WdfIoQueueStopSynchronously ||
      Name == api::WdfIoQueueStart || Name == api::WdfIoQueueStopAndPurge ||
      Name == api::WdfIoQueueStopAndPurgeSynchronously ||
      Name == api::WdfIoQueueDrain ||
      Name == api::WdfIoQueueDrainSynchronously ||
      Name == api::WdfIoQueuePurge ||
      Name == api::WdfIoQueuePurgeSynchronously) {
    auto Q = Queues.find(A[1]);
    if (Q == Queues.end() || OI->second.Deleting)
      return invalidQueue("queue has no live framework identity");
    if (Name == api::WdfIoQueueStop ||
        Name == api::WdfIoQueueStopSynchronously) {
      if (Q->second.DrainComplete)
        return invalidQueue("queue has a pending drain-completion callback");
      if (Name == api::WdfIoQueueStop && A[2] && Q->second.StopComplete)
        return invalidQueue("queue already has a stop-completion callback");
      Q->second.Accepting = true;
      Q->second.Dispatching = false;
      if (Name == api::WdfIoQueueStop && A[2]) {
        Q->second.StopComplete = A[2];
        Q->second.StopContext = A[3];
        auto Result = start({});
        if (!Result)
          return Result.takeError();
      }
      return std::optional<uint64_t>{0};
    }
    if (Name == api::WdfIoQueueStart) {
      if (Q->second.DrainComplete)
        return invalidQueue("queue has a pending drain-completion callback");
      Q->second.Accepting = true;
      Q->second.Dispatching = true;
      if (Q->second.Dispatch == QueueDispatchManual) {
        Q->second.ReadyPending = Q->second.ReadyNotify &&
                                 !queuePnpHeld(Q->second) &&
                                 !Q->second.Pending.empty();
        if (auto E = flushReadyNotifications())
          return E;
        return std::optional<uint64_t>{0};
      }
      if (Q->second.Pending.empty())
        return std::optional<uint64_t>{0};
      std::vector<Step> Steps(Q->second.Pending.size(),
                              {StepKind::PresentQueue, A[1]});
      auto Result = start(std::move(Steps));
      if (!Result)
        return Result.takeError();
      return std::optional<uint64_t>{*Result};
    }
    if (Name == api::WdfIoQueueDrain ||
        Name == api::WdfIoQueueDrainSynchronously) {
      if (Q->second.DrainComplete || Q->second.StopComplete)
        return invalidQueue("queue already has a state-completion callback");
      Q->second.Accepting = false;
      if (Name == api::WdfIoQueueDrain && A[2]) {
        Q->second.DrainComplete = A[2];
        Q->second.DrainContext = A[3];
        auto Result = start({});
        if (!Result)
          return Result.takeError();
      }
      return std::optional<uint64_t>{0};
    }
    if (Name == api::WdfIoQueuePurge ||
        Name == api::WdfIoQueuePurgeSynchronously ||
        Name == api::WdfIoQueueStopAndPurge ||
        Name == api::WdfIoQueueStopAndPurgeSynchronously) {
      const bool StopAndPurge =
          Name == api::WdfIoQueueStopAndPurge ||
          Name == api::WdfIoQueueStopAndPurgeSynchronously;
      const bool Asynchronous =
          Name == api::WdfIoQueuePurge || Name == api::WdfIoQueueStopAndPurge;
      if (Q->second.DrainComplete || Q->second.StopComplete)
        return invalidQueue("queue already has a state-completion callback");
      if (PendingCall)
        return invalidQueue("purge cannot replace a pending guest callback");
      if (!RequestsHost.RecordCancel || !RequestsHost.ValidateCompletion ||
          !RequestsHost.SetInformation || !RequestsHost.Information ||
          !RequestsHost.Complete)
        return invalidQueue("purge cancellation host is unavailable");
      std::vector<Step> Steps;
      std::vector<uint64_t> Cancellable;
      for (uint64_t Handle : Q->second.Pending) {
        auto R = Requests.find(Handle);
        if (R == Requests.end() || !R->second.Queued ||
            R->second.Queue != A[1] || R->second.Completed ||
            R->second.Completing)
          return invalidQueue("purge lost a queued request");
        if (!(Q->second.CanceledOnQueue &&
              (R->second.DeliveredOnce || R->second.Enqueued)))
          if (auto E = RequestsHost.ValidateCompletion(R->second.IRP,
                                                       RequestCancelled, 0))
            return E;
      }
      for (const auto &[Handle, R] : Requests)
        if (R.Queue == A[1] && !R.Queued && !R.Completed && !R.Completing &&
            R.Cancellation == CancelState::Marked) {
          auto O = Objects.find(Handle);
          if (O == Objects.end() || !O->second.InternalReferences ||
              !R.CancelRoutine)
            return invalidQueue("purge lost a cancelable driver request");
          Cancellable.push_back(Handle);
        }
      for (uint64_t Handle : Q->second.Pending) {
        auto &R = Requests.at(Handle);
        if (auto E = RequestsHost.RecordCancel(R.IRP))
          return E;
        if (Q->second.CanceledOnQueue && (R.DeliveredOnce || R.Enqueued)) {
          R.Queued = false;
          R.CanceledOnQueue = true;
          R.QueuedCallback = 0;
          R.QueuedArguments.clear();
          R.QueuedCompletionStatus.reset();
          Steps.push_back({StepKind::CanceledOnQueue, Handle});
          continue;
        }
        if (auto E = RequestsHost.SetInformation(R.IRP, 0))
          return E;
        R.Completing = true;
        R.CompletionStatus = RequestCancelled;
        std::vector<Step> Delete;
        if (auto E = planDelete(Handle, Delete))
          return E;
        auto Destruction =
            std::find_if(Delete.begin(), Delete.end(), [&](const Step &S) {
              return S.Kind == StepKind::TryDestroy && S.Object == Handle;
            });
        Delete.insert(Destruction, {StepKind::CompleteRequest, Handle});
        Steps.insert(Steps.end(), Delete.begin(), Delete.end());
        R.Queued = false;
        R.QueuedCallback = 0;
        R.QueuedArguments.clear();
        R.QueuedCompletionStatus.reset();
      }
      Q->second.Pending.clear();
      Q->second.ReadyPending = false;
      for (uint64_t Handle : Cancellable)
        Steps.push_back({StepKind::PurgeCancelRequest, Handle});
      Q->second.Accepting = StopAndPurge;
      if (StopAndPurge)
        Q->second.Dispatching = false;
      if (Asynchronous && A[2]) {
        if (StopAndPurge) {
          Q->second.StopComplete = A[2];
          Q->second.StopContext = A[3];
        } else {
          Q->second.DrainComplete = A[2];
          Q->second.DrainContext = A[3];
        }
      }
      if (!Steps.empty() || (Asynchronous && A[2])) {
        auto Result = start(std::move(Steps));
        if (!Result)
          return Result.takeError();
      }
      return std::optional<uint64_t>{0};
    }
    if (A[2])
      if (auto E = writable(A[2], 4))
        return E;
    if (A[3])
      if (auto E = writable(A[3], 4))
        return E;
    const uint32_t Queued = Q->second.Pending.size();
    const uint32_t Delivered =
        std::count_if(Requests.begin(), Requests.end(), [&](const auto &Entry) {
          return Entry.second.Queue == A[1] && !Entry.second.Queued &&
                 !Entry.second.Completed;
        });
    if (A[2])
      if (auto E = Memory.writeInteger(A[2], Queued, 4))
        return E;
    if (A[3])
      if (auto E = Memory.writeInteger(A[3], Delivered, 4))
        return E;
    uint32_t State = Q->second.Accepting ? QueueAcceptRequests : 0;
    if (Q->second.Dispatching)
      State |= QueueDispatchRequests;
    if (queuePnpHeld(Q->second))
      State |= QueuePnpHeld;
    if (!Queued)
      State |= QueueNoRequests;
    if (!Delivered)
      State |= QueueDriverNoRequests;
    return std::optional<uint64_t>{State};
  }
  if (Name == api::WdfIoQueueGetDevice) {
    auto Q = Queues.find(A[1]);
    if (Q == Queues.end() || !Devices.count(Q->second.Device))
      return invalidQueue("queue lost its owning control device");
    return std::optional<uint64_t>{Q->second.Device};
  }
  if (Name == api::WdfIoQueueFindRequest ||
      Name == api::WdfIoQueueRetrieveFoundRequest) {
    auto Q = Queues.find(A[1]);
    if (Q == Queues.end() || OI->second.Deleting)
      return invalidQueue("queue has no live framework identity");
    const bool Find = Name == api::WdfIoQueueFindRequest;
    const uint64_t Output = A[Find ? 5 : 3];
    if (auto E = writable(Output, sizeof(uint64_t)))
      return E;
    if (Q->second.Dispatch != QueueDispatchManual)
      return std::optional<uint64_t>{QueueInvalidDeviceState};
    if (!Q->second.Dispatching || queuePnpHeld(Q->second))
      return std::optional<uint64_t>{QueuePaused};
    if (Find && A[3] && !FileBelongsToQueue(A[3], Q->second))
      return invalidQueue("invalid or foreign file-object filter");
    const uint64_t Previous = A[2];
    if (!Find && !Previous)
      return invalidQueue("retrieval requires a live request handle");
    auto PreviousObject = Objects.find(Previous);
    if (Previous && (PreviousObject == Objects.end() ||
                     PreviousObject->second.Binding != B.Globals ||
                     PreviousObject->second.Kind != ObjectKind::Request ||
                     !Requests.count(Previous) ||
                     (Find && !PreviousObject->second.References)))
      return invalidQueue("invalid or unreferenced search request");
    auto Position = Q->second.Pending.begin();
    if (Previous) {
      Position = std::find(Position, Q->second.Pending.end(), Previous);
      if (Position == Q->second.Pending.end()) {
        if (auto E = Memory.writeInteger(Output, 0, sizeof(uint64_t)))
          return E;
        return std::optional<uint64_t>{QueueNotFound};
      }
      if (Find)
        ++Position;
    }
    if (Find && A[3]) {
      auto Match = FindPendingFile(Q->second, Position, A[3]);
      if (!Match)
        return Match.takeError();
      Position = *Match;
    }
    if (Position == Q->second.Pending.end()) {
      if (auto E = Memory.writeInteger(Output, 0, sizeof(uint64_t)))
        return E;
      return std::optional<uint64_t>{QueueNoMoreEntries};
    }
    if (Find) {
      const uint64_t Handle = *Position;
      auto R = Requests.find(Handle);
      auto O = Objects.find(Handle);
      if (R == Requests.end() || O == Objects.end() || !R->second.Queued ||
          R->second.Queue != A[1] || O->second.Binding != B.Globals ||
          O->second.Kind != ObjectKind::Request)
        return invalidQueue("queue lost a pending request");
      if (O->second.References == UINT64_MAX)
        return invalidQueue("framework reference count overflow");
      if (A[4]) {
        if (!RequestsHost.View)
          return invalidQueue("request inspection host is unavailable");
        auto View = RequestsHost.View(R->second.IRP);
        if (!View)
          return View.takeError();
        if (auto E = writeRequestParameters(A[4], *View))
          return E;
      }
      if (auto E = Memory.writeInteger(Output, Handle, sizeof(uint64_t)))
        return E;
      ++O->second.References;
      return std::optional<uint64_t>{0};
    }
    return RetrievePending(Q->second, Position, Output);
  }
  if (Name == api::WdfIoQueueRetrieveNextRequest ||
      Name == api::WdfIoQueueRetrieveRequestByFileObject) {
    auto Q = Queues.find(A[1]);
    if (Q == Queues.end() || OI->second.Deleting)
      return invalidQueue("queue has no live framework identity");
    const bool ByFile = Name == api::WdfIoQueueRetrieveRequestByFileObject;
    const uint64_t Output = A[ByFile ? 3 : 2];
    if (auto E = writable(Output, sizeof(uint64_t)))
      return E;
    if (Q->second.Dispatch == QueueDispatchParallel)
      return std::optional<uint64_t>{QueueInvalidDeviceState};
    if (!Q->second.Dispatching || queuePnpHeld(Q->second))
      return std::optional<uint64_t>{QueuePaused};
    if (ByFile && !FileBelongsToQueue(A[2], Q->second))
      return invalidQueue(
          "retrieval requires a live file object on the queue device");
    auto Position = Q->second.Pending.begin();
    if (ByFile) {
      auto Match = FindPendingFile(Q->second, Position, A[2]);
      if (!Match)
        return Match.takeError();
      Position = *Match;
    }
    if (Position == Q->second.Pending.end()) {
      if (!ByFile) {
        if (auto E = Memory.writeInteger(Output, 0, sizeof(uint64_t)))
          return E;
      }
      return std::optional<uint64_t>{QueueNoMoreEntries};
    }
    return RetrievePending(Q->second, Position, Output);
  }

  auto DI = Devices.find(A[1]);
  if (DI == Devices.end())
    return invalidQueue("handle has no control-device identity");
  auto &Device = DI->second;
  if (Name == api::WdfDeviceGetDefaultQueue) {
    if (Device.DefaultQueue && !Queues.count(Device.DefaultQueue))
      return invalidQueue("device lost its default-queue identity");
    return std::optional<uint64_t>{Device.DefaultQueue};
  }
  if (OI->second.Deleting)
    return invalidQueue("cannot create a queue while its device is deleting");

  auto Validation = attributes(A[3], AttributesUse::Object);
  if (!Validation)
    return Validation.takeError();
  if (const auto *Status = std::get_if<uint32_t>(&*Validation))
    return std::optional<uint64_t>{*Status};
  auto Attrs = std::get<Attributes>(*Validation);

  auto Size = read(A[2], 4);
  if (!Size)
    return Size.takeError();
  if (*Size == QueueConfigV17Size || *Size == QueueConfigV19Size)
    return invalidQueue("legacy queue configuration is not modeled");
  if (*Size != QueueConfigSize)
    return std::optional<uint64_t>{InfoLengthMismatch};
  if (auto E = ValidateAccess(A[2], QueueConfigSize, false))
    return E;
  std::vector<uint8_t> Config(QueueConfigSize);
  if (auto E = Memory.read(A[2], Config))
    return E;
  const auto Read32 = [&](uint64_t Offset) {
    return llvm::support::endian::read32le(Config.data() + Offset);
  };
  const auto Read64 = [&](uint64_t Offset) {
    return llvm::support::endian::read64le(Config.data() + Offset);
  };
  const auto Dispatch = Read32(QueueConfigDispatch);
  const bool IsDefault = Config[QueueConfigIsDefault] != 0;
  if (!IsDefault && !A[4])
    return std::optional<uint64_t>{QueueInvalidOutputParameter};
  if (IsDefault && Device.Initialized)
    return std::optional<uint64_t>{QueueInvalidDeviceState};

  if (Attrs.Parent) {
    uint64_t Ancestor = Attrs.Parent;
    for (size_t Remaining = Objects.size(); Ancestor; --Remaining) {
      auto Parent = Objects.find(Ancestor);
      if (!Remaining || Parent == Objects.end() ||
          Parent->second.Binding != B.Globals || Parent->second.Deleting)
        return invalidQueue("invalid or deleting explicit queue parent");
      if (Parent->second.Kind == ObjectKind::Device)
        break;
      Ancestor = Parent->second.Parent;
    }
    if (Ancestor != A[1])
      return std::optional<uint64_t>{QueueInvalidDeviceRequest};
    if (Attrs.Parent != A[1])
      return invalidQueue(
          "queue parenting below a device child is not modeled");
  }

  const uint64_t ConfigDriver = Read64(QueueConfigDriver);
  if (ConfigDriver && ConfigDriver != B.DriverHandle)
    return invalidQueue("queue driver is not this binding's live driver");
  if (Dispatch < QueueDispatchSequential || Dispatch > QueueDispatchManual)
    return std::optional<uint64_t>{InvalidParameter};
  const uint64_t Default = Read64(QueueConfigDefault);
  const uint64_t Read = Read64(QueueConfigRead);
  const uint64_t Write = Read64(QueueConfigWrite);
  const uint64_t DeviceControl = Read64(QueueConfigDeviceControl);
  const uint64_t Internal = Read64(QueueConfigInternalDeviceControl);
  const bool HasCallback =
      Default || Read || Write || DeviceControl || Internal;
  if (Dispatch != QueueDispatchManual && !HasCallback)
    return std::optional<uint64_t>{QueueNoCallback};
  if ((Dispatch == QueueDispatchManual && HasCallback) ||
      (Dispatch != QueueDispatchParallel &&
       Read32(QueueConfigPresentedRequests)))
    return std::optional<uint64_t>{InvalidParameter};
  const uint32_t PowerManaged = Read32(QueueConfigPowerManaged);
  if (PowerManaged > QueuePowerUseDefault)
    return invalidQueue("invalid power-management tri-state");
  if (Dispatch == QueueDispatchParallel &&
      !Read32(QueueConfigPresentedRequests))
    return std::optional<uint64_t>{InvalidParameter};
  if (Internal)
    return invalidQueue("internal-device-control callbacks are not modeled");
  const uint64_t IoStop = Read64(QueueConfigStop);
  const uint64_t IoResume = Read64(QueueConfigResume);
  const bool EffectivePowerManagement =
      Device.PDO && (PowerManaged == QueuePowerEnabled ||
                     PowerManaged == QueuePowerUseDefault);
  if ((IoStop || IoResume) && !EffectivePowerManagement)
    return invalidQueue(
        "I/O stop and resume require a power-managed PnP queue");

  // Effective inherited policies are not recorded by this object profile.
  // Require an explicit policy rather than claiming an inherited PASSIVE IRQL.
  if (!A[3])
    return invalidQueue(
        "queue requires explicit passive execution and no synchronization");
  auto Execution = read(A[3] + AttributesExecution, 4);
  auto Synchronization = read(A[3] + AttributesSynchronization, 4);
  if (!Execution || !Synchronization)
    return llvm::joinErrors(Execution.takeError(), Synchronization.takeError());
  if (*Execution != ExecutionPassive || *Synchronization != SynchronizationNone)
    return invalidQueue(
        "queue requires explicit passive execution and no synchronization");
  if (IsDefault && Device.DefaultQueue)
    return std::optional<uint64_t>{QueueUnsuccessful};
  if (A[4])
    if (auto E = writable(A[4], 8))
      return E;

  Attrs.Parent = A[1];
  auto Handle = createObject(B.Globals, Attrs, false);
  if (!Handle)
    return Handle.takeError();
  Objects.at(*Handle).Kind = ObjectKind::Queue;
  Queue Q;
  Q.Device = A[1];
  Q.Default = Default;
  Q.Read = Read;
  Q.Write = Write;
  Q.DeviceControl = DeviceControl;
  Q.CanceledOnQueue = Read64(QueueConfigCanceled);
  Q.Dispatch = Dispatch;
  if (Dispatch == QueueDispatchParallel)
    Q.PresentedLimit = Read32(QueueConfigPresentedRequests);
  Q.AllowZeroLength = Config[QueueConfigAllowZeroLength] != 0;
  Q.IsDefault = IsDefault;
  Q.PowerManaged = EffectivePowerManagement;
  Q.IoStop = IoStop;
  Q.IoResume = IoResume;
  Queues.emplace(*Handle, Q);
  if (IsDefault)
    Device.DefaultQueue = *Handle;
  if (A[4])
    if (auto E = Memory.writeInteger(A[4], *Handle, 8))
      return E;
  return std::optional<uint64_t>{0};
}

llvm::Expected<bool>
KernelFramework::queueWaitReady(uint64_t Handle, bool IncludePending) const {
  auto Q = Queues.find(Handle);
  if (Q == Queues.end())
    return invalidQueue("synchronous wait lost its queue");
  if (IncludePending && !Q->second.Pending.empty())
    return false;
  if (std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.Queue == Handle && !Entry.second.Queued &&
               !Entry.second.Completed;
      }))
    return false;
  if (std::any_of(CancelCallbacks.begin(), CancelCallbacks.end(),
                  [&](const auto &Entry) {
                    auto O = Objects.find(Entry.second);
                    return O != Objects.end() && O->second.Parent == Handle;
                  }))
    return false;
  if (std::any_of(CanceledQueueCallbacks.begin(), CanceledQueueCallbacks.end(),
                  [&](const auto &Entry) { return Entry.second == Handle; }))
    return false;
  if (std::any_of(ReadyQueueCallbacks.begin(), ReadyQueueCallbacks.end(),
                  [&](const auto &Entry) { return Entry.second == Handle; }))
    return false;
  return true;
}
} // namespace neverd::emulation
