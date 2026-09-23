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

namespace neverd::emulation {
namespace {
using namespace framework;

llvm::Error invalidQueue(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "KMDF queue: " + Message);
}
} // namespace

llvm::Expected<std::optional<uint64_t>>
KernelFramework::callQueue(llvm::StringRef Name, Binding &B,
                           llvm::ArrayRef<uint64_t> A) {
  if (Name != "WdfIoQueueCreate" && Name != "WdfDeviceGetDefaultQueue" &&
      Name != "WdfIoQueueGetDevice" && Name != "WdfIoQueueGetState" &&
      Name != "WdfIoQueueStop" && Name != "WdfIoQueueStart" &&
      Name != "WdfIoQueueRetrieveNextRequest")
    return std::optional<uint64_t>{};

  const auto OI = Objects.find(A[1]);
  const auto Kind =
      Name == "WdfIoQueueCreate" || Name == "WdfDeviceGetDefaultQueue"
          ? ObjectKind::Device
          : ObjectKind::Queue;
  if (OI == Objects.end() || OI->second.Binding != B.Globals ||
      OI->second.Kind != Kind)
    return invalidQueue("invalid, foreign or wrong-kind object handle");

  if (Name == "WdfIoQueueGetState" || Name == "WdfIoQueueStop" ||
      Name == "WdfIoQueueStart") {
    auto Q = Queues.find(A[1]);
    if (Q == Queues.end() || OI->second.Deleting)
      return invalidQueue("queue has no live framework identity");
    if (Name == "WdfIoQueueStop") {
      if (A[2] && Q->second.StopComplete)
        return invalidQueue("queue already has a stop-completion callback");
      Q->second.Dispatching = false;
      if (A[2]) {
        Q->second.StopComplete = A[2];
        Q->second.StopContext = A[3];
        auto Result = start({});
        if (!Result)
          return Result.takeError();
      }
      return std::optional<uint64_t>{0};
    }
    if (Name == "WdfIoQueueStart") {
      Q->second.Dispatching = true;
      if (Q->second.Dispatch == QueueDispatchManual ||
          Q->second.Pending.empty())
        return std::optional<uint64_t>{0};
      std::vector<Step> Steps(Q->second.Pending.size(),
                              {StepKind::PresentQueue, A[1]});
      auto Result = start(std::move(Steps));
      if (!Result)
        return Result.takeError();
      return std::optional<uint64_t>{*Result};
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
    uint32_t State = QueueAcceptRequests;
    if (Q->second.Dispatching)
      State |= QueueDispatchRequests;
    if (!Queued)
      State |= QueueNoRequests;
    if (!Delivered)
      State |= QueueDriverNoRequests;
    return std::optional<uint64_t>{State};
  }
  if (Name == "WdfIoQueueGetDevice") {
    auto Q = Queues.find(A[1]);
    if (Q == Queues.end() || !Devices.count(Q->second.Device))
      return invalidQueue("queue lost its owning control device");
    return std::optional<uint64_t>{Q->second.Device};
  }
  if (Name == "WdfIoQueueRetrieveNextRequest") {
    auto Q = Queues.find(A[1]);
    if (Q == Queues.end() || OI->second.Deleting)
      return invalidQueue("queue has no live framework identity");
    if (auto E = writable(A[2], sizeof(uint64_t)))
      return E;
    if (Q->second.Dispatch == QueueDispatchParallel)
      return std::optional<uint64_t>{QueueInvalidDeviceState};
    if (!Q->second.Dispatching)
      return std::optional<uint64_t>{QueuePaused};
    if (Q->second.Pending.empty()) {
      if (auto E = Memory.writeInteger(A[2], 0, sizeof(uint64_t)))
        return E;
      return std::optional<uint64_t>{QueueNoMoreEntries};
    }
    const uint64_t Handle = Q->second.Pending.front();
    auto R = Requests.find(Handle);
    if (R == Requests.end() || !R->second.Queued || R->second.Queue != A[1])
      return invalidQueue("queue lost a pending request");
    if (auto E = Memory.writeInteger(A[2], Handle, sizeof(uint64_t)))
      return E;
    Q->second.Pending.pop_front();
    R->second.Queued = false;
    R->second.QueuedCallback = 0;
    R->second.QueuedArguments.clear();
    R->second.QueuedCompletionStatus.reset();
    return std::optional<uint64_t>{0};
  }

  auto DI = Devices.find(A[1]);
  if (DI == Devices.end())
    return invalidQueue("handle has no control-device identity");
  auto &Device = DI->second;
  if (Name == "WdfDeviceGetDefaultQueue") {
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
  if (Read32(QueueConfigPowerManaged) > QueuePowerUseDefault)
    return invalidQueue("invalid power-management tri-state");
  // A control-device queue is never power managed, including WdfTrue and
  // WdfUseDefault configurations. No PnP state is fabricated here.
  if (Dispatch == QueueDispatchParallel &&
      !Read32(QueueConfigPresentedRequests))
    return std::optional<uint64_t>{InvalidParameter};
  if (Internal || Read64(QueueConfigStop) || Read64(QueueConfigResume) ||
      Read64(QueueConfigCanceled))
    return invalidQueue(
        "internal, power and cancellation callbacks are not modeled");

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
  Q.Dispatch = Dispatch;
  if (Dispatch == QueueDispatchParallel)
    Q.PresentedLimit = Read32(QueueConfigPresentedRequests);
  Q.AllowZeroLength = Config[QueueConfigAllowZeroLength] != 0;
  Q.IsDefault = IsDefault;
  Queues.emplace(*Handle, Q);
  if (IsDefault)
    Device.DefaultQueue = *Handle;
  if (A[4])
    if (auto E = Memory.writeInteger(A[4], *Handle, 8))
      return E;
  return std::optional<uint64_t>{0};
}
} // namespace neverd::emulation
