//===- KernelFramework.h - Guest KMDF bindings and objects ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Session-owned KMDF identities, typed contexts and guest callback lifetimes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_WINDOWS_KERNELFRAMEWORK_H
#define NEVERD_EMULATION_WINDOWS_KERNELFRAMEWORK_H

#include "../GuestMemory.h"
#include "KernelExportRegistry.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace neverd::emulation {
namespace framework {
#define NEVERD_FRAMEWORK_VALUE(Name, Value) constexpr uint64_t Name = Value;
#include "KernelFrameworkQueueValues.def"
#include "KernelFrameworkRequestValues.def"
#include "KernelFrameworkValues.def"
#undef NEVERD_FRAMEWORK_VALUE
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Major)                      \
  constexpr uint32_t RequestMajor##Name = Major;
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
} // namespace framework

class KernelFramework {
public:
  using Allocate = std::function<llvm::Expected<uint64_t>(uint64_t)>;
  using Validate = std::function<llvm::Error(uint64_t, uint32_t, bool)>;
  using Release = std::function<llvm::Error(uint64_t, uint64_t)>;
  struct GuestCall {
    uint64_t Token = 0;
    uint64_t PC = 0;
    std::vector<uint64_t> Arguments;
  };
  struct DeviceCreation {
    uint32_t Status = 0;
    uint64_t Address = 0;
  };
  /// Typed bridge to the authoritative WDM device namespace and storage.
  /// Framework operations never fabricate a second DEVICE_OBJECT or API trace.
  struct DeviceHost {
    std::function<llvm::Expected<DeviceCreation>(llvm::StringRef, uint32_t)>
        Create;
    std::function<llvm::Error(uint64_t)> Delete;
    std::function<llvm::Error(uint64_t)> FinishInitializing;
    std::function<llvm::Expected<uint32_t>(uint64_t, llvm::StringRef)> Link;
  };
  void setDeviceHost(DeviceHost Host) { DevicesHost = std::move(Host); }
  struct RequestView {
    uint64_t IRP = 0, ByteOffset = 0;
    uint32_t Major = 0, ControlCode = 0, InputLength = 0, OutputLength = 0;
    bool Neither = false;
    uint64_t UserInput = 0, UserOutput = 0;
  };
  struct LockedUserBuffer {
    uint32_t Status = 0;
    uint64_t MDL = 0, Buffer = 0;
  };
  struct RequestHost {
    std::function<llvm::Expected<RequestView>(uint64_t)> View;
    std::function<llvm::Expected<uint64_t>(uint64_t, bool)> Buffer;
    std::function<llvm::Error(uint64_t)> MarkPending;
    std::function<llvm::Expected<bool>(uint64_t)> IsCanceled;
    std::function<llvm::Error(uint64_t)> RecordCancel;
    std::function<llvm::Expected<uint64_t>(uint64_t)> Information;
    std::function<llvm::Error(uint64_t, uint64_t)> SetInformation;
    /// Return the request-owned descriptor, or zero on allocation failure.
    std::function<llvm::Expected<uint64_t>(uint64_t, bool)> Mdl;
    std::function<llvm::Expected<LockedUserBuffer>(uint64_t, uint64_t, uint64_t,
                                                   bool)>
        ProbeAndLock;
    std::function<llvm::Error(uint64_t)> ReleaseUserBuffer;
    std::function<llvm::Error(uint64_t, uint32_t, uint64_t)> ValidateCompletion;
    std::function<llvm::Error(uint64_t, uint32_t, uint64_t)> Complete;
  };
  void setRequestHost(RequestHost Host) { RequestsHost = std::move(Host); }
  struct RequestDispatch {
    uint64_t PC = 0;
    std::vector<uint64_t> Arguments;
    uint32_t Status = 0;
    bool CallerContext = false;
  };
  /// Nullopt selects ordinary WDM dispatch; an engaged result owns the exact
  /// framework dispatch status independently from a void callback's RAX.
  llvm::Expected<std::optional<RequestDispatch>>
  routeRequest(uint64_t WdmDevice, uint64_t IRP, bool AfterCaller = false);
  llvm::Expected<RequestDispatch> continueCallerContext(uint64_t IRP);
  /// The WDM host first records cancellation, then asks for the one guest
  /// notification owned by this request. Ordinary WDM requests return nullopt.
  llvm::Expected<std::optional<GuestCall>> requestCancellation(uint64_t IRP);
  /// Preview the callback count before the host records an imminent cancel
  /// fact. This checks model state and token capacity without reading the host
  /// cancel flag, publishing a call, or changing references or request state.
  /// EarlierCallbacks counts other cancellation callbacks already previewed
  /// for the same boundary, so their future continuation tokens are reserved.
  llvm::Expected<bool>
  preflightRequestCancellation(uint64_t IRP,
                               uint64_t EarlierCallbacks = 0) const;
  /// Latch delivery when the scheduled or nested cancel callback is entered.
  llvm::Error beginCancelCallback(uint64_t Token);
  bool isCancelCallback(uint64_t Token) const {
    return CancelCallbacks.contains(Token);
  }
  /// Framework-owned packets must complete through their WDF request lifetime.
  bool ownsRequestIRP(uint64_t IRP) const;
  KernelFramework(GuestMemory &Memory, KernelExportRegistry &Exports,
                  Allocate AllocateStorage, Validate ValidateAccess,
                  Release ReleaseStorage)
      : Memory(Memory), Exports(Exports), AllocateStorage(AllocateStorage),
        ValidateAccess(ValidateAccess), ReleaseStorage(ReleaseStorage) {}

  void configure(uint64_t Driver, uint64_t RegistryPath,
                 std::string ServiceName);
  static std::optional<unsigned>
  argumentCount(const KernelExportRegistry::Export &Export);
  llvm::Expected<uint64_t> call(const KernelExportRegistry::Export &Export,
                                llvm::ArrayRef<uint64_t> Arguments,
                                uint8_t IRQL);
  std::optional<GuestCall> takeGuestCall();
  bool hasPendingGuestCall() const { return PendingCall.has_value(); }
  /// Synchronous queue APIs wait for driver-owned requests; drain/purge also
  /// wait for requests still held by the framework queue.
  llvm::Expected<bool> queueWaitReady(uint64_t Queue,
                                      bool IncludePending) const;
  /// Resume one suspended framework operation after its actual guest callback.
  llvm::Expected<std::optional<uint64_t>> finishGuestCall(uint64_t Token,
                                                          uint64_t Result);
  llvm::Error validateGuestAccess(uint64_t Address, uint32_t Size,
                                  bool IsWrite) const;
  bool hasLiveBinding() const;

private:
  GuestMemory &Memory;
  KernelExportRegistry &Exports;
  Allocate AllocateStorage;
  Validate ValidateAccess;
  Release ReleaseStorage;
  uint64_t Driver = 0, RegistryPath = 0;
  std::string ServiceName;
  struct Region {
    uint64_t Size;
    bool Writable;
    bool Opaque;
    bool Freed = false;
  };
  std::map<uint64_t, Region> Regions;
  struct Binding {
    uint64_t Info = 0, Globals = 0, Table = 0, Module = 0;
    uint64_t DriverHandle = 0, RegistryCopy = 0;
    uint64_t UnloadCallback = 0;
    std::vector<uint8_t> RegistryBytes;
    bool Unloaded = false, Unbinding = false, Unbound = false;
  };
  std::map<uint64_t, Binding> Bindings;
  DeviceHost DevicesHost;
  struct DeviceInit {
    uint64_t Binding = 0;
    std::string Name;
    uint32_t IoType = framework::ControlIoBuffered;
    uint64_t CallerContext = 0;
  };
  std::map<uint64_t, DeviceInit> DeviceInits;
  struct Device {
    uint64_t Wdm = 0;
    uint64_t DefaultQueue = 0;
    uint64_t CallerContext = 0;
    bool Initialized = false;
    bool HasLink = false;
  };
  std::map<uint64_t, Device> Devices;
  struct Queue {
    uint64_t Device = 0;
    uint64_t Default = 0, Read = 0, Write = 0, DeviceControl = 0;
    uint32_t Dispatch = framework::QueueDispatchSequential;
    uint32_t PresentedLimit = UINT32_MAX;
    bool AllowZeroLength = false;
    bool IsDefault = false;
    bool Accepting = true;
    bool Dispatching = true;
    uint64_t StopComplete = 0;
    uint64_t StopContext = 0;
    uint64_t DrainComplete = 0;
    uint64_t DrainContext = 0;
    std::deque<uint64_t> Pending;
  };
  std::map<uint64_t, Queue> Queues;
  RequestHost RequestsHost;
  enum class CancelState { Unmarked, Marked, Queued, Delivered };
  struct Request {
    uint64_t IRP = 0, Queue = 0;
    uint64_t Device = 0;
    bool InCallerContext = false;
    bool Enqueued = false;
    bool Queued = false;
    bool Completed = false;
    bool Completing = false;
    uint32_t CompletionStatus = 0;
    uint64_t QueuedCallback = 0;
    std::vector<uint64_t> QueuedArguments;
    std::optional<uint32_t> QueuedCompletionStatus;
    CancelState Cancellation = CancelState::Unmarked;
    uint64_t CancelRoutine = 0;
  };
  std::map<uint64_t, Request> Requests;
  std::map<uint64_t, uint64_t> CallerRequests;
  struct UserMemory {
    uint64_t Request = 0, MDL = 0, Buffer = 0, Length = 0;
    bool Active = true;
  };
  std::map<uint64_t, UserMemory> UserMemories;
  struct Context {
    uint64_t Address = 0, Size = 0;
    uint64_t Cleanup = 0, Destroy = 0;
  };
  struct Attributes {
    uint64_t Parent = 0, Cleanup = 0, Destroy = 0;
    uint64_t Type = 0, ContextSize = 0;
  };
  enum class AttributesUse { Driver, Object, AdditionalContext, Device };
  using AttributeResult = std::variant<Attributes, uint32_t>;
  enum class ObjectKind { Driver, Generic, Device, Queue, Request, Memory };
  struct Object {
    uint64_t Binding = 0, Parent = 0;
    ObjectKind Kind = ObjectKind::Generic;
    bool Deleting = false, Cleaned = false;
    bool DestroyEligible = false;
    uint64_t References = 0;
    uint64_t InternalReferences = 0;
    std::map<uint64_t, Context> Contexts;
    std::vector<uint64_t> ContextOrder;
    std::vector<uint64_t> Children;
  };
  std::map<uint64_t, Object> Objects;
  enum class StepKind {
    Callback,
    PresentQueue,
    Cleaned,
    CompleteRequest,
    PurgeCancelRequest,
    CancelReturned,
    TryDestroy,
    Destroy,
    BeginDriverDelete,
    FinishBindingUnbind,
    DriverUnloaded
  };
  struct Step {
    StepKind Kind;
    uint64_t Object;
    uint64_t PC = 0;
  };
  struct Continuation {
    std::vector<Step> Steps;
    size_t Index = 0;
  };
  uint64_t NextContinuation = 1;
  std::map<uint64_t, Continuation> Continuations;
  std::map<uint64_t, uint64_t> CancelCallbacks;
  std::optional<GuestCall> PendingCall;

  llvm::Error preflightCancellationToken(uint64_t EarlierCallbacks) const;
  llvm::Expected<RequestDispatch> queueDispatch(uint64_t QueueHandle,
                                                uint64_t RequestHandle,
                                                const RequestView &View) const;
  llvm::Expected<bool> presentQueued(uint64_t QueueHandle, uint64_t Token);

  llvm::Expected<uint64_t> read(uint64_t Address, unsigned Width = 8);
  llvm::Expected<std::vector<uint8_t>> readRegistryPath(uint64_t Address);
  llvm::Error writable(uint64_t Address, uint32_t Size);
  llvm::Expected<uint64_t> allocate(uint64_t Size, bool Writable, bool Opaque);
  llvm::Error retire(uint64_t Address);
  llvm::Expected<uint64_t> bind(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> unbind(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error finishUnbind(Binding &B);
  llvm::Expected<AttributeResult> attributes(uint64_t Address,
                                             AttributesUse Use);
  llvm::Expected<uint64_t> createObject(uint64_t Globals, const Attributes &A,
                                        bool IsDriver);
  llvm::Expected<uint64_t> addContext(Object &O, const Attributes &A);
  llvm::Expected<uint64_t> createDriver(Binding &B,
                                        llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<std::optional<uint64_t>>
  callControl(llvm::StringRef Name, Binding &B,
              llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<std::optional<uint64_t>>
  callQueue(llvm::StringRef Name, Binding &B,
            llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<std::optional<uint64_t>>
  callRequest(llvm::StringRef Name, Binding &B,
              llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<std::optional<uint64_t>>
  callRequestAccessors(llvm::StringRef Name, Binding &B,
                       llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<std::string> readControlString(uint64_t Address);
  llvm::Error planDelete(uint64_t Handle, std::vector<Step> &Steps);
  llvm::Expected<std::optional<uint64_t>> advance(uint64_t Token);
  llvm::Expected<uint64_t> start(std::vector<Step> Steps);
};
} // namespace neverd::emulation
#endif
