//===- KernelModel.h - Bounded Windows environment ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded Windows environment model.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELMODEL_H
#define NEVERD_EMULATION_KERNELMODEL_H
#include "DeviceLifecycle.h"
#include "KernelDMA.h"
#include "KernelDispatcher.h"
#include "KernelFileSystem.h"
#include "KernelFramework.h"
#include "KernelGuardBitmap.h"
#include "KernelGuestCall.h"
#include "KernelGuestPageTables.h"
#include "KernelInterrupts.h"
#include "KernelMMIO.h"
#include "KernelModelMemory.h"
#include "KernelModuleImages.h"
#include "KernelPoFx.h"
#include "KernelRegistry.h"
#include "KernelRemoveLocks.h"
#include "KernelScheduler.h"
#include "KernelSystemVA.h"
#include "KernelUsbIdle.h"
#include "KernelWaits.h"

#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/GuestMemory.h"
#include "neverd/emulation/WindowsDriverState.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <variant>
namespace neverd::modules {
class ModuleImage;
}
namespace neverd::emulation {
class RAMInitializationWatch;
struct DriverImage;
struct DriverProcessImagePlan;
struct BackendFault;
class KernelExportRegistry;
struct KernelContextCapture;
class KernelReferenceImage;
class ModulePublication;
struct ModuleInstance;
enum class KernelAPIKind;
class KernelModel {
  friend class KernelRegistry;

public:
  KernelModel(GuestMemory &Backing, DriverResult &Result,
              KernelExportRegistry *Exports = nullptr)
      : ObservedMemory(Backing, [this] { recordUnpackEnvironmentRead(); }),
        Memory(ObservedMemory), Result(Result), Exports(Exports),
        Registry(*this, Memory),
        Dispatcher(Memory, Scheduler,
                   [this](uint64_t Address, uint32_t Size, bool IsWrite) {
                     return validateDispatcherStorage(Address, Size, IsWrite);
                   }),
        Resources([this](uint64_t PDO) { return canReleaseResources(PDO); },
                  [this](uint64_t PDO) {
                    // DMA adapter acquisition in AddDevice precedes START.
                    return llvm::joinErrors(MMIO.canRemove(PDO),
                                            Interrupts.canRelease(PDO));
                  },
                  [this](uint64_t PDO) { return DMA.canPowerDownPDO(PDO); }),
        MMIO(Memory, Resources), Interrupts(Resources, Result),
        Physical(Memory), DMA(Physical, Resources, Result) {}
  KernelModel(const KernelModel &) = delete;
  KernelModel &operator=(const KernelModel &) = delete;
  KernelModel(KernelModel &&) = delete;
  KernelModel &operator=(KernelModel &&) = delete;
  llvm::Error initialize(const DriverImage &Image,
                         const DriverOptions &Options);
  /// Opt into the retained adapter's model-owned loader record/ring.
  llvm::Error initializeLoadedModule(const DriverImage &, const std::string &);
  void addReferenceModule(uint64_t Base, uint64_t Size);
  /// Pure missing-image probe. Existing image extents and OS object lifetime
  /// remain owned even when their backing page is currently absent.
  bool isModuleResourceCandidate(uint64_t Address, uint32_t Size) const;
  /// Stage loader records/names/ring links without guest or model mutation.
  /// The common module RAM transaction owns publication of these patches.
  llvm::Expected<std::unique_ptr<ModulePublication>>
      prepareModulePublication(llvm::ArrayRef<ModuleInstance>);
  llvm::Error setDebuggerReference(const KernelReferenceImage &Reference);
  llvm::Error initializeDebuggerState();
  llvm::Error initializeObjectTypes();
  bool isObjectTypeDataRead(uint64_t Address, uint32_t Size) const;
  llvm::Error initializeProcessData();
  bool isProcessDataRead(uint64_t Address, uint32_t Size) const;
  llvm::Error initializeProcessorData();
  bool isProcessorDataRead(uint64_t Address, uint32_t Size) const;
  llvm::Error initializeNlsData();
  bool isNlsDataRead(uint64_t Address, uint32_t Size) const;
  llvm::Error initializeHalDispatch();
  llvm::Error initializeHalAcpiCache();
  bool isHalDispatchRead(uint64_t, uint32_t) const;
  llvm::Error validateHalDispatchAccess(uint64_t, uint32_t, bool) const;
  llvm::Expected<uint64_t> callHalDispatch(const KernelExportRegistry::Export &,
                                           llvm::ArrayRef<uint64_t>);
  llvm::Error validateNlsDataAccess(uint64_t Address, uint32_t Size,
                                    bool IsWrite) const;
  llvm::Expected<uint64_t> processorNumber(uint64_t Output, bool FromIndex,
                                           uint32_t Index = 0);
  llvm::Expected<uint64_t> groupAffinity(uint64_t Input, uint64_t Previous,
                                         bool Revert);
  llvm::Error synchronizeProcessList();
  llvm::Error synchronizeGuestPageTables();
  llvm::Expected<uint64_t> guestCR3();
  uint64_t guestCR4() const;
  /// End a normally returned DriverEntry, regardless of its NTSTATUS. The
  /// borrowed RegistryPath record and buffer expire before any later callback.
  llvm::Error finishEntry();
  /// A stopped DriverEntry can be rebuilt only when borrowed loader objects
  /// retain their initial state and every retained kernel effect is known.
  llvm::Error captureUnpackBaseline();
  void recordUnpackEnvironmentRead();
  llvm::Expected<std::vector<std::string>> unpackDependencies() const;
  llvm::Expected<WindowsDriverState>
  captureUnpackRuntime(bool IncludeBacking) const;
  std::map<uint64_t, uint64_t> unpackAllocations() const;
  uint64_t driverObject() const { return DriverObject; }
  bool isServiceImagePC(uint64_t PC) const;
  bool isServiceThreadEntry(uint64_t PC) const;
  llvm::Error validateServiceAPI(uint64_t PC, llvm::StringRef Name) const;
  const std::string &callDetail() const { return APICallDetail; }
  uint64_t registryPath() const { return RegistryPath; }
  using ContextCapture = std::function<llvm::Error(uint64_t)>;
  void setContextCapture(ContextCapture Capture) {
    CaptureContext = std::move(Capture);
  }
  /// Fixed kernel argument counts; unknown names have no execution contract.
  static std::optional<unsigned> argumentCount(const std::string &Name);
  static std::optional<unsigned>
  argumentCount(const KernelExportRegistry::Export &Export);
  llvm::Expected<uint64_t>
  call(const KernelExportRegistry::Export &Export,
       llvm::ArrayRef<uint64_t> Arguments,
       llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument,
       const KernelContextCapture *Context = nullptr);
  std::optional<KernelGuestCall> takeGuestCall();
  std::optional<KernelGuestCall> takeIndependentGuestCall();
  std::optional<KernelGuestCall> takePoFxThreadCall(uint64_t Thread);
  llvm::Error beginGuestCall(GuestCallToken Token);
  llvm::Expected<bool> enterFrameworkCallback(uint64_t ScheduledID,
                                              GuestCallToken Token,
                                              uint64_t IRP);
  void setFrameworkCallbackContext(bool PowerManaged) {
    FrameworkPowerManagedCallback = PowerManaged;
  }
  llvm::Expected<std::optional<uint64_t>> finishGuestCall(GuestCallToken Token,
                                                          uint64_t Result);
  llvm::Expected<uint64_t>
  call(const std::string &Name, llvm::ArrayRef<uint64_t> Arguments,
       llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument =
           nullptr,
       const KernelContextCapture *Context = nullptr);
  /// Deliver completed file notifications at an eligible caller boundary.
  /// File retirement and API results remain owned by the file continuation.
  llvm::Error
  deliverFileRequestCompletions(const KernelContextCapture *Context = nullptr);
  llvm::Error snapshot();
  struct Invocation {
    uint64_t PC = 0;
    uint64_t Argument0 = 0;
    uint64_t Argument1 = 0;
    uint64_t Argument2 = 0;
    uint64_t Argument3 = 0;
    std::vector<uint64_t> StackArguments;
    std::optional<uint32_t> FrameworkDispatchStatus;
    bool FrameworkCallerContext = false;
    uint64_t IRP = 0;
    uint64_t SynchronizationObject = 0;
  };
  // PnP device enrollment: configuration identities never expose addresses.
  llvm::Error preparePnpDevices();
  llvm::Expected<Invocation> beginAddDevice(llvm::StringRef ID);
  llvm::Error finishAddDevice(llvm::StringRef ID, uint32_t Status);
  llvm::Error finalizeAddDevice(llvm::StringRef ID);
  llvm::Error beginFrameworkRemoval(uint64_t IRP);
  /// A pending dispatch retains its packet until a guest callback completes it.
  llvm::Expected<Invocation>
  beginRequest(const DriverRequest &Request,
               std::optional<size_t> SourceIndex = std::nullopt);
  llvm::Expected<Invocation> continueFrameworkCallerContext(uint64_t IRP);
  llvm::Error recordDispatchReturn(uint64_t IRP, uint32_t DispatchStatus);
  /// Acquire the callback's effective device or queue synchronization lock.
  /// A false result suspends entry; the real guest frame has not run yet.
  llvm::Expected<bool> beginFrameworkCallback(uint64_t Object);
  llvm::Error finishFrameworkCallback(uint64_t Object);
  llvm::Error flushFrameworkCallbackDestructions();
  /// Finalization is idempotent only for an already finalized owned IRP.
  llvm::Error finalizeRequest(uint64_t IRP);
  /// IRP=0 inspects all requests. The scheduler can exclude framework packets
  /// parked by device power so a later START can produce their completion.
  enum class PendingRequestScope { All, ExcludePowerParked };
  bool
  requestPending(uint64_t IRP = 0,
                 PendingRequestScope Scope = PendingRequestScope::All) const;
  llvm::Expected<std::optional<KernelScheduler::Invocation>>
  nextScheduled(bool AdvanceTime,
                std::optional<uint64_t> Deadline = std::nullopt);
  llvm::Error finishScheduled(uint64_t ID);
  bool hasForegroundBlockingCallbacks() const {
    return Scheduler.hasForegroundBlockingCallbacks();
  }
  bool isSuspendedSystemThread(uint64_t ID) const {
    return Scheduler.isSuspendedSystemThread(ID);
  }
  bool isSystemThreadEntry(uint64_t PC) const;
  std::optional<uint32_t> takeThreadTermination();
  bool hasDeferredThreadTermination(GuestCallToken Token) const {
    if (Token.Owner != GuestCallOwner::ThreadNotify)
      return false;
    const auto It = ThreadNotifications.find(Token.ID);
    return It != ThreadNotifications.end() &&
           It->second.Thread == CurrentThreadKey && !It->second.Output;
  }
  /// Finish a scheduled framework callback, including any destruction callbacks
  /// required before releasing its scheduler ownership.
  llvm::Expected<std::optional<KernelGuestCall>>
  continueScheduled(uint64_t ID, uint64_t ReturnValue);
  llvm::Error suspendScheduled(uint64_t ID);
  llvm::Error resumeScheduled(uint64_t ID);
  /// Selector state only. Resource ownership remains in the model's canonical
  /// per-execution and per-thread tables while this continuation is parked.
  struct ExecutionContext {
    uint64_t Execution, Thread;
    GuestCallToken Call;
    uint32_t Process;
    uint8_t IRQL;
    bool UserMemory, PowerManaged;
  };
  ExecutionContext captureExecutionContext() const;
  llvm::Error restoreExecutionContext(const ExecutionContext &Context);
  llvm::Error preemptScheduled(uint64_t ID);
  llvm::Expected<uint64_t> issueReadyOrder() {
    return Scheduler.issueReadyOrder();
  }
  std::optional<uint64_t> nextPassiveReadyOrder() const {
    return Scheduler.nextPassiveReadyOrder();
  }
  std::optional<KernelScheduler::ReadyThread> nextPassiveThread() const {
    return Scheduler.nextPassiveThread();
  }
  KernelScheduler::ReadyThread
  readyThread(uint64_t Key, uint64_t Order,
              const ExecutionContext *Paused = nullptr) const {
    return Scheduler.readyThread(resolvedThreadPriorityKey(Key, Paused), Order);
  }
  int32_t threadPriority(uint64_t Key) const {
    return Scheduler.threadPriority(resolvedThreadPriorityKey(Key));
  }
  uint64_t now100ns() const { return Scheduler.now100ns(); }
  /// Process exactly one chronological boundary, including during execution.
  llvm::Error advanceExecutionTo100ns(uint64_t Time);
  llvm::Error restoreWaitIRQL(uint8_t IRQL);
  struct Wait {
    enum class Kind {
      Dispatcher,
      Thread,
      Multiple,
      Delay,
      RemoveLock,
      FrameworkQueueStop,
      FrameworkQueueEmpty,
      FrameworkFileSend,
      FrameworkIdle,
      PoFxActive,
      PoFxIdle,
      InterruptSynchronization,
      FrameworkInterruptLock,
      FrameworkWaitLock,
      FrameworkCallback,
      FrameworkCallbackLock,
      FileClose,
      FileHandleClose,
      FileRequest,
      FileOperation,
      ExecutiveCallbackUnregister,
      ProcessNotifyRemove,
      ThreadNotifyRemove,
      RegistryCallbackRemove
    };
    Kind Type = Kind::Dispatcher;
    uint64_t Object = 0;
    uint64_t Execution = 0;
    uint64_t Thread = 0;
    uint8_t IRQL = 0;
    std::optional<uint64_t> Deadline;
    std::vector<uint64_t> Objects;
    bool All = false;
    uint64_t WaitBlockArray = 0;
    uint32_t WaitBlockSize = 0;
    uint64_t Registration = 0;
    bool operator==(const Wait &) const = default;
  };
  std::optional<Wait> takeWait();
  llvm::Expected<std::optional<uint32_t>> pollWait(const Wait &Pending);
  llvm::Expected<std::optional<uint64_t>> resumeFileHandleClose(uint64_t ID);
  std::optional<uint64_t> takeFileRequestContinuation();
  llvm::Expected<std::optional<uint64_t>> resumeFileRequest(uint64_t ID);
  struct FileAPIContinuation {
    bool Request = false;
    uint64_t ID = 0;
  };
  std::optional<FileAPIContinuation> takeFileAPIContinuation();
  llvm::Expected<std::optional<uint64_t>>
  resumeFileAPI(FileAPIContinuation Continuation);
  std::optional<uint64_t> nextEventTime() const;
  bool hasQueuedDPC() const { return Scheduler.hasQueuedDPC(); }
  bool hasQueuedPriorityCallback() const {
    const auto Interrupt = nextInterruptEventTime();
    const auto Transfer = DMA.nextEventTime();
    return (Interrupt && *Interrupt <= Scheduler.now100ns()) ||
           (Transfer && *Transfer <= Scheduler.now100ns()) ||
           Scheduler.hasQueuedInterrupt() || hasQueuedDPC() ||
           Scheduler.hasQueuedDMACallback() ||
           Scheduler.hasQueuedCancellation() || Scheduler.hasQueuedCompletion();
  }
  llvm::Error activateStack(uint64_t Base, uint64_t Size);
  llvm::Error retireStack(uint64_t Base, uint64_t Size);
  void enterForeground() { CurrentIRQL = 0; }
  GuestCallToken scheduledGuestCall(uint64_t ID) const {
    const auto Found = ScheduledModelContinuations.find(ID);
    return Found == ScheduledModelContinuations.end() ? GuestCallToken{}
                                                      : Found->second;
  }
  void enterExecution(uint64_t Identity, uint64_t ThreadKey = 0,
                      GuestCallToken Call = {}) {
    CurrentExecution = Identity;
    CurrentGuestCall = Call;
    CurrentThreadKey = ThreadKey ? ThreadKey : Identity;
    ExecutionThreadKeys[Identity] = CurrentThreadKey;
    if (EnteringPassiveInterrupt) {
      PassiveInterruptThreads.emplace(*EnteringPassiveInterrupt,
                                      CurrentThreadKey);
      EnteringPassiveInterrupt.reset();
    }
    if (Scheduler.active() &&
        Scheduler.active()->Kind ==
            KernelScheduler::CallbackKind::SystemThread &&
        CurrentThreadKey == Scheduler.active()->ID)
      ApcStates.try_emplace(CurrentThreadKey, ApcState{1, 0});
    if (CancelLock.Held && CancelLock.Callback && !CancelLock.Owner) {
      CancelLock.Owner = Identity;
      CancelLock.CallbackExecution = Identity;
    }
  }
  /// A synchronous nested callback retains its parent's process and memory
  /// authority while keeping its own execution and resource ownership.
  llvm::Error
  inheritExecutionContext(uint64_t Child, uint64_t Parent,
                          std::optional<uint32_t> ProcessID = std::nullopt);
  llvm::Error activateInheritedProcess();
  llvm::Error setUserRequestContext(
      bool Active,
      uint32_t ProcessID = DriverRequest::DefaultRequestorProcessID);
  llvm::Error revokeRequestUserBuffers(uint64_t IRP);
  llvm::Error exitRequestorProcess(uint64_t IRP);
  llvm::Expected<std::optional<KernelGuestCall>>
  beginProcessEvent(const DriverProcessEvent &Event, size_t SourceIndex);
  llvm::Error completeProcessEvent();
  llvm::Expected<uint64_t> beginRegistryEvent(const DriverRegistryEvent &,
                                              size_t);
  llvm::Error finishRegistryEvent(uint64_t Status);
  bool canCatchUserAccess(uint64_t Address, uint64_t Size) const;
  llvm::Error validateExecutionReturn(uint64_t Identity, uint8_t EntryIRQL,
                                      bool Nested = false) const;
  bool hasPendingInterruptEvents() const {
    return Interrupts.hasPendingEvents();
  }
  bool hasPendingHardwareWork() const {
    return Interrupts.hasPendingEvents() || DMA.hasPendingEvents() ||
           DMA.hasPendingCallbacks() || PoFx.hasPendingCallbacks() ||
           PoFx.nextDeadline().has_value() || hasPendingPowerPolicyEvents() ||
           (Framework && Framework->hasPendingPowerPolicy());
  }
  uint8_t currentIRQL() const { return CurrentIRQL; }
  /// Return a borrowed, opaque identity for the current kernel thread.
  llvm::Expected<uint64_t> currentThreadObject();
  bool hasModelSpecificRegisters() const;
  llvm::Expected<uint64_t> readModelSpecificRegister(uint32_t Index) const;
  llvm::Expected<Invocation> beginUnload();
  llvm::Error finishUnload();
  /// Reject CPU accesses whose environment semantics this profile does not own.
  llvm::Error validateGuestAccess(uint64_t Address, uint32_t Size,
                                  bool IsWrite) const;
  /// CPU writes resolve COW through a protected-page fault and replay the
  /// original instruction; API write admission can prepare its page directly.
  llvm::Error validateGuestCPUWrite(uint64_t Address, uint32_t Size) const;
  llvm::Expected<bool>
  repairProcessImageWriteFault(const BackendFault &Fault) const;

private:
  llvm::Expected<uint64_t> dispatchKernelCall(
      const std::string &Name, llvm::ArrayRef<uint64_t> Arguments,
      llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument,
      const KernelContextCapture *Context);
  static std::optional<unsigned> halArgumentCount(llvm::StringRef Name);
  llvm::Expected<uint64_t> callHAL(llvm::StringRef Name,
                                   llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> queryPerformanceCounter(uint64_t Frequency);
  llvm::Expected<uint64_t> setCancelRoutine(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> mapMDL(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> unmapMDL(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> callPowerDriver(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> completeDriverRequest(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t>
  currentDriverRequestStack(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> initializeUnicodeString(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> ansiToUnicodeString(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> unicodeToAnsiString(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> upcaseUnicodeString(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> freeCountedString(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> stringFromGUID(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> guidFromString(llvm::ArrayRef<uint64_t> A);
  enum class PoolSource { GuestAllocation, KernelResult };
  llvm::Expected<uint64_t>
  allocatePool(llvm::ArrayRef<uint64_t> A, bool Modern,
               PoolSource Source = PoolSource::GuestAllocation);
  llvm::Expected<uint64_t> initializeCountedString(llvm::ArrayRef<uint64_t> A,
                                                   unsigned UnitBytes);
  llvm::Expected<uint64_t> convertCountedString(llvm::ArrayRef<uint64_t> A,
                                                bool ToUnicode);
  llvm::Expected<uint64_t> freeCountedString(uint64_t Address);
  std::set<uint64_t> RtlStringBuffers;
  llvm::Expected<uint64_t> freePool(llvm::ArrayRef<uint64_t> A, bool Tagged);
  llvm::Expected<uint64_t> allocateContiguousMemory(uint64_t Size,
                                                    uint64_t High);
  llvm::Expected<uint64_t> freeContiguousMemory(uint64_t Address);
  llvm::Error preflightPhysicalAllocation(uint64_t Address,
                                          uint64_t Size) const;
  llvm::Expected<uint64_t>
  deleteSymbolicLinkFromGuest(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t>
  createSymbolicLinkFromGuest(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> debugMessage(
      llvm::ArrayRef<uint64_t> A, unsigned FormatIndex,
      llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument);
  llvm::Expected<uint64_t> prefixedDebugMessage(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> checkedMemoryCopy(llvm::ArrayRef<uint64_t> A,
                                             bool ClearOnError);
  llvm::Expected<uint64_t> memoryCall(KernelAPIKind Kind,
                                      llvm::ArrayRef<uint64_t> A);

  KernelModelMemory ObservedMemory;
  GuestMemory &Memory;
  llvm::Expected<std::shared_ptr<RAMInitializationWatch>>
  watchInitialization(uint64_t Address, uint64_t Size);
  static bool initialized(llvm::ArrayRef<bool> Flags,
                          const std::shared_ptr<RAMInitializationWatch> &Watch,
                          uint64_t Offset, uint64_t Size = 1);
  static bool
  anyInitialized(llvm::ArrayRef<bool> Flags,
                 const std::shared_ptr<RAMInitializationWatch> &Watch,
                 uint64_t Offset, uint64_t Size);
  DriverResult &Result;
  KernelExportRegistry *Exports;
  std::vector<uint64_t> ObjectTypeDataSlots;
  std::optional<uint64_t> InitialSystemProcessDataSlot;
  std::optional<uint64_t> ProcessorCountDataSlot;
  std::optional<uint64_t> NlsOemDataSlot;
  std::optional<bool> NlsOemMultibyte;
  std::optional<DriverPciConfiguration> PciConfiguration;
  std::optional<uint8_t> IRPIOPriorityHint;
  llvm::Expected<uint64_t> pciBusData(llvm::ArrayRef<uint64_t>, bool Writing);
  std::set<uint64_t> SystemAffinityThreads;
  std::unique_ptr<KernelFramework> Framework;
  KernelPoFx PoFx;
  std::map<uint64_t, uint64_t> PoFxDeviceObjects;
  struct FrameworkPoFxPowerWait {
    uint64_t CallbackToken;
    KernelPoFx::CallbackKind Kind;
  };
  std::map<uint64_t, FrameworkPoFxPowerWait> FrameworkPoFxPowerWaits;
  struct BlockingPoFxOperation {
    uint64_t Handle = 0, Thread = 0;
    uint32_t Component = 0;
    bool Active = false;
    std::deque<KernelGuestCall> Calls;
    std::optional<uint64_t> TerminalCallback;
    bool Completed = false;
    uint64_t CompletionGeneration = 0;
  };
  std::map<uint64_t, BlockingPoFxOperation> BlockingPoFx;
  llvm::Error waitForPoFxOperation(uint64_t Thread);
  enum class PoFxHandleAccess { DriverOwned, AnyOwner };
  enum class PoFxCondition { Active, Idle };
  llvm::Error preparePoFxOperation(uint64_t Handle, PoFxHandleAccess Access);
  llvm::Expected<uint64_t> finishPoFxOperation(llvm::Error Error);
  llvm::Expected<uint64_t> registerPoFxDevice(uint64_t Object, uint64_t Record,
                                              uint64_t Output);
  llvm::Expected<uint64_t> unregisterPoFxDevice(uint64_t Handle);
  llvm::Expected<uint64_t> startPoFxPowerManagement(uint64_t Handle);
  llvm::Expected<uint64_t> changePoFxComponent(uint64_t Handle, uint32_t Index,
                                               uint32_t Flags,
                                               PoFxCondition Condition);
  llvm::Expected<uint64_t> completePoFxIdleCondition(uint64_t Handle,
                                                     uint32_t Index);
  llvm::Expected<uint64_t> completePoFxIdleState(uint64_t Handle,
                                                 uint32_t Index);
  llvm::Expected<uint64_t> completePoFxPowerNotRequired(uint64_t Handle);
  llvm::Expected<uint64_t> reportPoFxDevicePoweredOn(uint64_t Handle);
  llvm::Expected<uint64_t>
  setPoFxComponentLatency(uint64_t Handle, uint32_t Index, uint64_t Latency);
  llvm::Expected<uint64_t> setPoFxComponentResidency(uint64_t Handle,
                                                     uint32_t Index,
                                                     uint64_t Residency);
  llvm::Expected<uint64_t> setPoFxComponentWake(uint64_t Handle, uint32_t Index,
                                                bool Wake);
  llvm::Expected<uint64_t> setPoFxDeviceIdleTimeout(uint64_t Handle,
                                                    uint64_t Timeout);
  llvm::Expected<KernelPoFx::Registration>
  readPoFxRegistration(uint64_t PDO, uint64_t Address);
  llvm::Error queuePoFxCallbacks();
  llvm::Error processPoFxCallbacks();
  llvm::Expected<std::optional<uint64_t>> finishPoFxCall(uint64_t Token);
  llvm::Error processInternalPoFxCallback(const KernelPoFx::Callback &Call);
  llvm::Expected<KernelPoFx::Component> readPoFxComponent(uint64_t Address);
  llvm::Error validatePoFxRegistrationDevice(uint64_t PDO) const;
  void configureFrameworkPoFxHost();
  llvm::Error setFrameworkPoFxIdle(uint64_t Device, bool Idle,
                                   uint64_t Timeout);
  llvm::Error retireFrameworkPoFx(uint64_t Device);
  llvm::Error completeFrameworkPowerNotRequired(
      uint64_t Device, KernelFramework::PowerPolicyHost::RequestMode Mode);

  std::optional<KernelGuestCall> takeWdmGuestCall();
  llvm::Expected<std::optional<uint64_t>> finishWdmGuestCall(uint64_t Token,
                                                             uint64_t Result);
  void configureFrameworkDeviceHost();
  void configureFrameworkPowerPolicyHost();
  llvm::Error requestFrameworkDevicePower(
      uint64_t Device, DevicePowerState Target,
      KernelFramework::PowerPolicyHost::RequestMode Mode);
  llvm::Error armFrameworkWake(uint64_t Device, bool SystemSleep);
  llvm::Error finishFrameworkWake(uint64_t Device, bool Triggered);
  llvm::Expected<uint64_t> preflightFrameworkWake(uint64_t PDO,
                                                  uint32_t Status) const;
  llvm::Expected<uint64_t> retainProviderWake(uint64_t PDO, uint64_t IRP);
  llvm::Expected<uint64_t> preflightProviderWake(uint64_t PDO, uint64_t IRP,
                                                 uint32_t Status) const;
  llvm::Error completeProviderWake(uint64_t PDO, uint64_t IRP, uint32_t Status,
                                   std::optional<uint64_t> SourcePDO);
  static std::optional<unsigned> providerArgumentCount(llvm::StringRef Name);
  llvm::Expected<uint64_t>
  callProviderExport(const KernelExportRegistry::Export &Export,
                     llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error completeFrameworkWakes(uint64_t SourceDevice,
                                     llvm::ArrayRef<uint64_t> Devices);
  llvm::Error cancelFrameworkWakes(llvm::ArrayRef<uint64_t> Devices);
  llvm::Error
  canArmPowerPolicyEvents(llvm::ArrayRef<DriverPowerPolicyEvent> Events) const;
  llvm::Error
  armPowerPolicyEvents(llvm::ArrayRef<DriverPowerPolicyEvent> Events,
                       size_t SourceIndex);
  llvm::Error processPowerPolicyEvents();
  std::optional<uint64_t> nextPowerPolicyEventTime() const;
  bool hasPendingPowerPolicyEvents() const;
  struct FrameworkPowerEvent {
    uint64_t Epoch;
  };
  struct WdmWakeEvent {
    uint64_t StartEpoch;
    uint64_t IRP;
  };
  struct PoFxPowerEvent {
    uint64_t Epoch;
    uint64_t Handle;
  };
  struct UsbIdlePermissionEvent {
    uint64_t Epoch;
    std::vector<UsbIdleKey> Members;
  };
  using PowerEventOwner = std::variant<FrameworkPowerEvent, WdmWakeEvent,
                                       PoFxPowerEvent, UsbIdlePermissionEvent>;
  struct PowerPolicyEvent {
    uint64_t PDO;
    size_t ResultIndex;
    PowerEventOwner Owner;
  };
  llvm::Expected<PowerEventOwner>
  capturePowerEventOwner(const DriverPowerPolicyEvent &Event) const;
  std::vector<PowerPolicyEvent> PowerPolicyEvents;
  struct ProviderWake {
    uint64_t IRP = 0;
    uint64_t StartEpoch = 0;
    std::optional<uint64_t> FrameworkEpoch;
    uint64_t CancelRoutine = 0;
  };
  std::map<uint64_t, ProviderWake> ProviderWakeIRPs;
  KernelUsbIdle UsbIdle;
  struct FrameworkUsbIdleRequest {
    uint64_t Device = 0;
    uint64_t PolicyEpoch = 0;
    uint64_t Info = 0;
    UsbIdleKey Key;
  };
  std::map<uint64_t, FrameworkUsbIdleRequest> FrameworkUsbIdleRequests;
  llvm::Expected<KernelFramework::PowerPolicyHost::UsbIdleSettings>
  resolveFrameworkUsbIdle(uint64_t Device, uint32_t RequestedState) const;
  llvm::Expected<UsbIdleKey> submitFrameworkUsbIdle(uint64_t Device,
                                                    uint64_t PolicyEpoch);
  llvm::Expected<bool> hasFrameworkUsbIdle(UsbIdleKey Key) const;
  llvm::Error
  cancelFrameworkUsbIdle(UsbIdleKey Key,
                         KernelFramework::PowerPolicyHost::RequestMode Mode);
  llvm::Error requestFrameworkUsbIdlePower(
      uint64_t Device, UsbIdleKey Key, uint64_t Token,
      KernelFramework::PowerPolicyHost::RequestMode Mode);
  llvm::Error abortFrameworkUsbIdlePower(UsbIdleKey Key, uint64_t Token,
                                         uint32_t Status);
  llvm::Error finishFrameworkUsbIdleCallback(UsbIdleKey Key, uint64_t Token);
  llvm::Error tryFinalizeFrameworkUsbIdle(uint64_t IRP);
  llvm::Expected<std::optional<KernelScheduler::Invocation>>
  dispatchScheduledFrameworkUsbIdle(const KernelScheduler::Invocation &Call);

  const DriverUsbIdleConfig *usbIdleConfig(uint64_t PDO) const;
  llvm::Expected<UsbIdleSubmission>
  planUsbIdleSubmission(uint64_t PDO, uint64_t IRP, uint64_t Info,
                        uint64_t InputSize, uint64_t OutputSize,
                        uint64_t Output) const;
  llvm::Expected<uint64_t> receiveUsbIdle(uint64_t PDO, uint64_t IRP);
  llvm::Expected<std::vector<UsbIdleKey>>
  captureUsbIdlePermission(uint64_t PDO) const;
  llvm::Error validateUsbIdlePermission(llvm::ArrayRef<UsbIdleKey> Keys) const;
  llvm::Expected<std::vector<KernelScheduler::UsbIdleCallback>>
  previewUsbIdleCallbacks(llvm::ArrayRef<UsbIdleKey> Keys) const;
  llvm::Error queueUsbIdlePermission(llvm::ArrayRef<UsbIdleKey> Keys);
  llvm::Error beginUsbIdleCallback(uint64_t Token);
  llvm::Expected<std::optional<uint64_t>> finishUsbIdleCallback(uint64_t Token);
  llvm::Expected<UsbIdleCompletionPlan>
  preflightUsbIdleCompletion(UsbIdleKey Key, UsbIdleCompletionCause Cause,
                             std::optional<bool> CancelOverride = {}) const;
  llvm::Error completeUsbIdle(UsbIdleKey Key, UsbIdleCompletionCause Cause);
  llvm::Expected<uint64_t>
  cancelUsbIdle(const KernelExportRegistry::Export &Export,
                llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error observeUsbIdlePowerCompletion(uint64_t IRP, uint32_t Status);
  llvm::Error validateUsbRemoteWake(uint64_t PDO) const;
  void configureFrameworkInterruptHost();
  void configureFrameworkLockHost();
  llvm::Expected<std::optional<uint32_t>>
  pollFrameworkWaitLock(const Wait &Pending);
  struct FrameworkCallbackLock {
    uint64_t Storage = 0;
  };
  bool FrameworkPowerManagedCallback = false;
  std::map<uint64_t, FrameworkCallbackLock> FrameworkCallbackLocks;
  struct FrameworkCallbackEntry {
    uint64_t Object = 0, Thread = 0;
    uint8_t PreviousIRQL = 0;
    bool Acquired = false;
    bool Passive = true;
    bool Automatic = true;
    uint16_t CriticalDepth = 0;
  };
  std::map<std::pair<uint64_t, uint64_t>, FrameworkCallbackEntry>
      FrameworkCallbackEntries;
  llvm::Expected<uint64_t> frameworkCallbackLock(uint64_t Object);
  llvm::Expected<bool> acquireFrameworkCallback(uint64_t Object,
                                                bool Automatic);
  llvm::Error releaseFrameworkCallback(uint64_t Object, bool Automatic);
  llvm::Expected<std::optional<uint32_t>>
  pollFrameworkCallback(const Wait &Pending);
  llvm::Error completeFrameworkTransitionIfReady();
  llvm::Expected<uint64_t> forwardFrameworkTransitionRequest(uint64_t IRP);
  llvm::Error detachFrameworkPnpDevice(uint64_t Device, uint64_t PDO);
  /// Framework-owned WDM devices and their canonical symbolic-link keys.
  std::map<uint64_t, std::vector<std::string>> FrameworkDevices;
  KernelRegistry Registry;
  KernelFileSystem FileSystem;
  KernelScheduler Scheduler;
  bool SharedClockMapped = false;
  std::optional<uint64_t> PublishedSharedTick;
  llvm::Error initializeSharedClock();
  llvm::Error synchronizeSharedClock();
  llvm::Expected<uint64_t> stallProcessor(uint32_t Microseconds);
  llvm::Error validateSharedClockAccess(uint64_t Address, uint32_t Size,
                                        bool IsWrite) const;
  KernelDispatcher Dispatcher;
  KernelRemoveLocks RemoveLocks;
  KernelResources Resources;
  KernelMMIO MMIO;
  KernelInterrupts Interrupts;
  KernelPhysicalMemory Physical;
  KernelDMA DMA;
  std::optional<KernelGuestCall> PendingDMACall;
  std::set<uint64_t> InlineDMACalls;
  bool hasPendingIndependentGuestCall() const {
    return PendingServiceCall || PendingAvlCall || PendingThreadNotify ||
           PendingRegistryCallback || PendingProcessNotify ||
           PendingExecutiveCallback || PendingWdmCall || PendingInterruptCall ||
           PendingDMACall || (Framework && Framework->hasPendingGuestCall());
  }
  bool hasPendingModelGuestCall() const {
    auto Operation = BlockingPoFx.find(CurrentThreadKey);
    const bool HasPoFxCall =
        Operation != BlockingPoFx.end() && !Operation->second.Calls.empty();
    return HasPoFxCall || hasPendingIndependentGuestCall();
  }
  static std::optional<unsigned> dmaArgumentCount(llvm::StringRef Name);
  llvm::Expected<uint64_t>
  callDMAExport(const KernelExportRegistry::Export &Export,
                llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> getDMAAdapter(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t>
  allocateCommonBuffer(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error freeCommonBuffer(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t>
  getScatterGatherList(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error putScatterGatherList(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t>
  allocateAdapterChannel(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> mapTransfer(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error flushAdapterBuffers(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error freeMapRegisters(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error flushIoBuffers(uint64_t MDL);
  struct DmaMdlView {
    uint64_t Owner, Offset, DescriptorSize;
  };
  llvm::Expected<DmaMdlView> dmaMdlView(uint64_t MDL, uint64_t CurrentVA,
                                        uint32_t Length, bool ToDevice) const;
  llvm::Expected<std::vector<uint64_t>>
  dmaPromotionIDs(llvm::ArrayRef<KernelDMA::Promotion> Ready) const;
  llvm::Error releaseDMAMapping(const KernelDMA::ReleasePlan &Plan);
  llvm::Error beginDMACall(uint64_t Object);
  llvm::Expected<std::optional<uint64_t>> finishDMACall(uint64_t Object,
                                                        uint64_t Result);

  uint64_t CurrentExecution = 0;
  GuestCallToken CurrentGuestCall;
  uint64_t CurrentThreadKey = 0;
  std::map<uint64_t, uint64_t> ExecutionThreadKeys;
  // DriverSession activates the actual extent before entering an execution.
  // Nested stacks have independent bounds even when they share a thread key.
  std::map<uint64_t, uint64_t> ExecutionStacks;
  llvm::Expected<std::pair<uint64_t, uint64_t>> currentStackBounds() const;
  llvm::Expected<uint64_t> queryInitialStack() const;
  llvm::Expected<uint64_t> queryStackLimits(uint64_t Low, uint64_t High);
  struct ExecutionProcessContext {
    uint32_t ProcessID, CreatingProcessID;
    uint8_t PreviousMode;
    /// Public KeStackAttachProcess currently admits only a bounded operation
    /// set. Internal object-manager callbacks have their own continuation.
    bool RestrictedAttachment;
  };
  std::optional<ExecutionProcessContext> executionProcessContext() const;
  struct InheritedExecutionContext {
    uint64_t ThreadKey;
    uint32_t UserProcessID;
    bool UserMemoryAuthority;
    std::optional<ExecutionProcessContext> Process;
  };
  std::map<uint64_t, InheritedExecutionContext> InheritedExecutionContexts;
  struct ApcState {
    uint16_t CriticalDepth = 0;
    uint16_t GuardedDepth = 0;
  };
  struct ProcessNotifier {
    uint64_t Routine = 0;
    size_t Busy = 0;
    bool Removing = false;
    uint64_t RemovalThread = 0;
    bool Extended = false;
  };
  std::array<std::optional<uint64_t>, 64> ProcessNotifySlots{};
  std::map<uint64_t, ProcessNotifier> ProcessNotifiers;
  uint64_t NextProcessNotifier = 1, NextProcessNotification = 1;
  struct ProcessNotification {
    uint64_t ID = 0, Thread = 0, Registration = 0;
    size_t Cursor = 0, ReportIndex = 0;
    ApcState Baseline;
    bool Active = false;
    uint64_t CreateInfo = 0, ImageFileObject = 0;
    std::vector<std::pair<uint64_t, uint64_t>> Storage;
  };
  std::optional<ProcessNotification> ProcessNotificationState;
  std::optional<KernelGuestCall> PendingProcessNotify;
  std::set<uint32_t> LifecycleLiveProcesses{4}, LifecycleSeenProcesses{4};
  std::optional<DriverProcessInventory> ProcessInventory;
  std::set<uint64_t> PublishedProcessImageNames;
  std::set<uint64_t> PublishedProcessSectionBases;
  std::set<uint64_t> PublishedProcessPebs;
  // Only exiting processes are recorded. Live processes without an exit
  // observation retain the native initial STATUS_PENDING value.
  std::map<uint32_t, std::optional<uint32_t>> ProcessExitStatuses;
  std::set<uint64_t> PublishedProcessExitStatuses;
  llvm::Expected<uint64_t> processExitStatus(uint64_t Object);
  llvm::Error observeProcessExitStatus(uint32_t PID,
                                       std::optional<uint32_t> Status);
  llvm::Expected<uint64_t> processImageFileName(uint64_t Object);
  bool isLiveProcess(uint32_t PID) const {
    return LifecycleLiveProcesses.contains(PID) &&
           !ExitedUserProcesses.contains(PID);
  }
  llvm::Expected<uint64_t> materializeProcessObject(uint32_t ProcessID);
  llvm::Error admitProcess(uint32_t ProcessID);
  llvm::Expected<uint64_t> queryProcessInformation(llvm::ArrayRef<uint64_t>,
                                                   bool ZwCall);
  std::map<uint32_t, uint32_t> LifecycleParents;
  llvm::Error initializeProcessEvents(const DriverOptions &);
  llvm::Expected<uint64_t> setProcessNotify(llvm::ArrayRef<uint64_t> A,
                                            bool Extended = false);
  llvm::Expected<std::optional<uint32_t>> pollProcessNotifyRemove(const Wait &);
  llvm::Error prepareProcessNotify();
  llvm::Error prepareProcessCreateInfo(const DriverProcessEvent &);
  llvm::Error validateProcessNotifyAccess(uint64_t Address, uint32_t Size,
                                          bool Write) const;
  llvm::Error beginProcessNotify(uint64_t ID);
  llvm::Expected<std::optional<uint64_t>> finishProcessNotify(uint64_t ID);
  llvm::Error retireUserProcess(uint32_t ProcessID);
  llvm::Error validateProcessNotifyUnload() const;
  llvm::Error canReleaseProcessNotifyStorage(uint64_t Base,
                                             uint64_t Size) const;
  struct RegistryEventState {
    size_t Index = 0;
    uint64_t Thread = 0, Control = 0, Output = 0;
    std::vector<std::pair<uint64_t, uint64_t>> Storage;
  };
  std::optional<RegistryEventState> ActiveRegistryEvent;
  std::map<uint32_t, uint64_t> RegistryEventHandles;
  struct RegistryCallbackRegistration {
    uint64_t Cookie = 0, Routine = 0, Context = 0;
    size_t Busy = 0;
    bool Removing = false;
    uint64_t RemovalThread = 0;
  };
  std::optional<RegistryCallbackRegistration> RegistryCallback;
  uint64_t NextRegistryCookie = 1, NextRegistryNotification = 1;
  struct RegistryNotification {
    uint64_t Cookie = 0, Thread = 0, Storage = 0, Object = 0;
    std::string API;
    std::vector<uint64_t> Arguments;
    uint32_t PreClass = 0, PostClass = 0, PreSize = 0, ContextOffset = 0;
    uint32_t Status = 0;
    bool Post = false, Active = false;
    ApcState Baseline;
    ExecutionContext Caller{};
  };
  std::map<uint64_t, RegistryNotification> RegistryNotifications;
  std::map<uint64_t, uint64_t> RegistryKeyObjects;
  std::optional<KernelGuestCall> PendingRegistryCallback;
  llvm::Expected<uint64_t> registerRegistryCallback(llvm::ArrayRef<uint64_t>);
  llvm::Expected<uint64_t> unregisterRegistryCallback(uint64_t Cookie);
  llvm::Expected<std::optional<uint32_t>>
  pollRegistryCallbackRemove(const Wait &);
  llvm::Expected<uint64_t> registryOperation(llvm::StringRef,
                                             llvm::ArrayRef<uint64_t>);
  llvm::Expected<uint64_t> registryKeyObject(uint64_t Identity);
  llvm::Error beginRegistryCallback(uint64_t ID);
  llvm::Expected<std::optional<uint64_t>>
  finishRegistryCallback(uint64_t ID, uint64_t Value);
  llvm::Error validateRegistryCallbackAccess(uint64_t Address, uint32_t Size,
                                             bool Write) const;
  llvm::Error canReleaseRegistryCallbackStorage(uint64_t Address,
                                                uint64_t Size) const;
  llvm::Error validateRegistryCallbackUnload() const;
  std::map<uint64_t, ApcState> ApcStates;
  enum class ApcRegionKind { Critical, Guarded };
  llvm::Expected<ApcState *> apcStateForCall();
  llvm::Expected<uint64_t> apcsDisabled();
  llvm::Expected<uint64_t> allApcsDisabled();
  llvm::Expected<uint64_t> enterApcRegion(ApcRegionKind Kind);
  llvm::Expected<uint64_t> leaveApcRegion(ApcRegionKind Kind);
  bool UserRequestContext = false;
  uint32_t CurrentUserProcessID = 0;
  uint64_t NextUserAddress = profile::UserArenaBase;
  struct UserAllocation {
    uint64_t Size;
    uint32_t ProcessID;
    DriverUserPageAccess Access;
  };
  std::map<uint64_t, UserAllocation> UserAllocations;
  std::set<uint64_t> RevokedUserAllocations;
  std::set<uint32_t> ExitedUserProcesses;
  struct FileNode;
  struct ImageSection {
    std::shared_ptr<const DriverProcessImagePlan> Plan;
    uint64_t Backing = 0;
    // The section owns the source identity independently of FILE_OBJECTs.
    std::shared_ptr<FileNode> File;
  };
  struct ProcessMemoryView {
    uint64_t Base = 0, Size = 0;
    mutable uint64_t Backing = 0;
    unsigned Permissions = 0;
    mutable MemoryView Storage;
    struct ImagePage {
      std::shared_ptr<const DriverProcessImagePlan> Source;
      std::shared_ptr<ImageSection> Section;
      uint8_t Protection = 0;
      uint64_t PrivateBacking = 0;
      MemoryView PrivateStorage;
      bool Private = false;
    };
    std::shared_ptr<ImagePage> Image;
  };
  std::map<uint32_t, std::vector<ProcessMemoryView>> ProcessMemoryViews;
  struct ImageMapping {
    std::shared_ptr<ImageSection> Section;
    uint64_t Size = 0;
    bool Dynamic = false;
  };
  std::map<std::pair<uint32_t, uint64_t>, ImageMapping> ImageMappings;
  llvm::Expected<std::shared_ptr<ImageSection>>
  imageSection(const DriverProcessImageMapping &Input,
               std::shared_ptr<FileNode> File = {});
  llvm::Expected<std::vector<ProcessMemoryView>>
  imagePages(const std::shared_ptr<ImageSection> &Section, uint64_t Size);
  llvm::Expected<uint64_t> createImageSection(llvm::ArrayRef<uint64_t>);
  llvm::Expected<uint64_t> mapImageSection(llvm::ArrayRef<uint64_t>);
  llvm::Expected<uint64_t> querySection(llvm::ArrayRef<uint64_t>);
  llvm::Error retireImageMapping(uint32_t PID, uint64_t Base);
  llvm::Error initializeProcessMemory();
  llvm::Expected<uint64_t> queryVirtualMemory(llvm::ArrayRef<uint64_t> A,
                                              bool ZwCall);
  const ProcessMemoryView *processMemoryView(uint32_t PID, uint64_t Address,
                                             uint64_t Size) const;
  llvm::Error validateProcessMemoryAccess(uint64_t Address, uint32_t Size,
                                          bool IsWrite) const;
  unsigned processMemoryPermissions(const ProcessMemoryView &View) const;
  llvm::Error prepareProcessImageWrite(uint64_t Address, uint64_t Size) const;
  struct UserMdlView {
    uint64_t MDL = 0;
    uint64_t Address = 0;
    uint64_t PageBase = 0;
    uint64_t MappedSize = 0;
    uint64_t BackingAddress = 0;
    uint32_t ProcessID = 0;
    uint32_t Length = 0;
    unsigned Permissions = 0;
  };
  using UserMdlViewKey = std::pair<uint32_t, uint64_t>;
  std::map<UserMdlViewKey, UserMdlView> UserMdlViews;
  std::map<uint32_t, uint64_t> ProcessObjects;
  std::map<uint64_t, uint32_t> ProcessObjectIDs;
  struct ProcessAttachment {
    uint64_t Execution;
    uint64_t ApcState;
    uint32_t PreviousProcessID;
    bool PreviousUserContext;
    bool Private = false;
    ExecutionProcessContext PreviousContext;
    uint32_t TargetProcessID;
  };
  std::vector<ProcessAttachment> ProcessAttachments;
  bool hasProcessAttachment(uint64_t Execution) const;
  struct ExecutiveSpinLock {
    uint64_t Execution;
    uint8_t OldIRQL;
    bool RaisedIRQL;
  };
  std::map<uint64_t, ExecutiveSpinLock> ExecutiveSpinLocks;
  enum class SpinLockMode { Raise, AtDpc, TryAtDpc };
  llvm::Error validateSpinLockAddress(uint64_t Address) const;
  llvm::Error validateSpinLockStorage(uint64_t Address) const;
  llvm::Expected<uint64_t> initializeSpinLock(uint64_t Address);
  llvm::Expected<uint64_t> acquireSpinLock(uint64_t Address, SpinLockMode Mode);
  llvm::Expected<uint64_t> releaseSpinLock(uint64_t Address,
                                           std::optional<uint8_t> RestoreIRQL);
  // In-stack queued spin locks store the caller's KLOCK_QUEUE_HANDLE pointer in
  // the lock and the saved IRQL in the handle, keyed here by the lock address.
  struct QueuedSpinLock {
    uint64_t Execution;
    uint64_t Handle;
    uint8_t OldIRQL;
    bool RaisedIRQL;
  };
  std::map<uint64_t, QueuedSpinLock> QueuedSpinLocks;
  // A lock address or an execution holds a lock if either spin-lock kind tracks
  // it, so every held-lock rule treats executive and queued locks alike.
  bool spinLockHeldAt(uint64_t Address) const {
    return ExecutiveSpinLocks.count(Address) || QueuedSpinLocks.count(Address);
  }
  bool executionHoldsSpinLock(uint64_t Execution) const {
    for (const auto &[Address, Lock] : ExecutiveSpinLocks)
      if (Lock.Execution == Execution)
        return true;
    for (const auto &[Address, Lock] : QueuedSpinLocks)
      if (Lock.Execution == Execution)
        return true;
    return false;
  }
  llvm::Error validateQueueHandle(uint64_t Handle) const;
  llvm::Expected<uint64_t> acquireQueuedSpinLock(uint64_t SpinLock,
                                                 uint64_t Handle, bool Raise);
  llvm::Expected<uint64_t> releaseQueuedSpinLock(uint64_t Handle, bool Restore);
  // Run-down protection keeps its whole state in the caller's EX_RUNDOWN_REF
  // Count word, so these operations only validate and update guest memory.
  llvm::Expected<uint64_t> readRundownCount(uint64_t RunRef);
  llvm::Expected<uint64_t> initializeRundownProtection(uint64_t RunRef);
  // \p Units is 1 for the plain routines and the ULONG count of the Ex forms.
  llvm::Expected<uint64_t> acquireRundownProtection(uint64_t RunRef,
                                                    uint32_t Units);
  llvm::Expected<uint64_t> releaseRundownProtection(uint64_t RunRef,
                                                    uint32_t Units);
  llvm::Expected<uint64_t> waitForRundownProtectionRelease(uint64_t RunRef);
  llvm::Expected<uint64_t> rundownCompleted(uint64_t RunRef);
  llvm::Error validateInterlockedLock(uint64_t Lock) const;
  llvm::Error validateInterlockedStorage(uint64_t Lock, uint64_t Address,
                                         uint32_t Size, bool IsWrite) const;
  llvm::Expected<uint64_t> readInterlockedLink(uint64_t Lock, uint64_t Address,
                                               bool IsWrite) const;
  llvm::Expected<uint64_t> interlockedInsertList(llvm::ArrayRef<uint64_t> A,
                                                 bool Tail);
  llvm::Expected<uint64_t>
  interlockedRemoveHeadList(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> interlockedPushEntryList(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> interlockedPopEntryList(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> interlockedAdd(llvm::ArrayRef<uint64_t> A,
                                          unsigned Size);
  llvm::Error validateSListStorage(uint64_t Address, uint32_t Size,
                                   bool IsWrite) const;
  llvm::Expected<std::pair<uint64_t, uint64_t>>
  readSListHeader(uint64_t Header, bool IsWrite) const;
  llvm::Expected<uint64_t> initializeSList(uint64_t Header);
  llvm::Expected<uint64_t> pushEntrySList(uint64_t Header, uint64_t Entry);
  llvm::Expected<uint64_t> popEntrySList(uint64_t Header);
  llvm::Expected<uint64_t> flushSList(uint64_t Header);
  llvm::Expected<uint64_t> querySList(uint64_t Header, bool FirstEntry);

  struct Lookaside {
    bool Paged;
    uint32_t Type, EntrySize, Tag;
    uint64_t Allocate, Free;
  };
  std::map<uint64_t, Lookaside> Lookasides;
  uint64_t LookasideHeads[2] = {};
  llvm::Expected<uint64_t> initializeLookaside(llvm::ArrayRef<uint64_t> A,
                                               bool Paged);
  llvm::Expected<uint64_t> deleteLookaside(uint64_t Address, bool Paged);
  llvm::Error validateLookasideAccess(uint64_t Address, uint32_t Size,
                                      bool IsWrite) const;
  llvm::Error canReleaseLookasideRange(uint64_t Address, uint64_t Size,
                                       uint64_t Retiring) const;
  llvm::Expected<std::vector<uint64_t>>
  lookasideEntries(uint64_t Address) const;
  llvm::Error writableLookasideStorage(uint64_t Address, uint32_t Size) const;
  llvm::Error validateKernelObjectBorrow(uint64_t Address, uint64_t Size) const;
  llvm::Error validateUnborrowedKernelStorage(uint64_t Address,
                                              uint64_t Size) const;
  enum class FastMutexMode { Normal, Unsafe, UnsafeGuarded };
  struct FastMutexHold {
    uint64_t Thread, Object;
    uint8_t OldIRQL;
    FastMutexMode Mode;
  };
  std::map<uint64_t, FastMutexHold> FastMutexHolds;
  llvm::Expected<uint64_t> initializeFastMutex(uint64_t Address);
  llvm::Expected<uint64_t> acquireFastMutex(uint64_t Address,
                                            FastMutexMode Mode, bool Try);
  llvm::Expected<uint64_t> releaseFastMutex(uint64_t Address, bool Unsafe);
  llvm::Error validateFastMutexStorage(uint64_t Address, bool Initialize) const;
  llvm::Error validateFastMutexAccess(uint64_t Address, uint32_t Size,
                                      bool IsWrite) const;
  llvm::Error validateFastMutexContext(uint8_t IRQL, uint16_t Critical,
                                       uint16_t Guarded,
                                       uint64_t Retiring = 0) const;
  bool threadHoldsFastMutex(uint64_t Thread) const;
  enum class ExecutiveResourceMode {
    Exclusive,
    Shared,
    SharedWaitForExclusive
  };
  struct ExecutiveResourceOwner {
    uint64_t Thread = 0, Value = 0;
    uint32_t Count = 0;
  };
  struct ExecutiveResource {
    uint64_t Flink = 0, Blink = 0, Table = 0;
    bool Exclusive = false, BoostDisabled = false;
    // Slot zero is the inline owner. A table has an unused header slot zero
    // and the remaining owners at the same indices as this vector.
    std::vector<ExecutiveResourceOwner> Owners{1};
  };
  std::map<uint64_t, ExecutiveResource> ExecutiveResources;
  uint64_t ExecutiveResourceHead = 0;
  llvm::Error validateExecutiveResourceAccess(uint64_t Address,
                                              uint64_t Size) const;
  llvm::Error canReleaseExecutiveResourceRange(uint64_t Address,
                                               uint64_t Size) const;
  llvm::Error validateExecutiveResourceMemory(uint64_t Address, uint32_t Size,
                                              bool Write) const;
  llvm::Expected<ExecutiveResource *>
  executiveResource(uint64_t Address, bool Write,
                    bool AllowTransferred = false);
  std::vector<uint8_t> executiveResourceBytes(const ExecutiveResource &State,
                                              bool Table) const;
  llvm::Error validateExecutiveResourceRegistry() const;
  llvm::Error publishExecutiveResource(uint64_t Address,
                                       const ExecutiveResource &State);
  llvm::Error validateExecutiveResourceContext(uint8_t IRQL, uint16_t Critical,
                                               uint16_t Guarded,
                                               uint64_t Retiring = 0,
                                               uint64_t Owner = 0,
                                               bool IgnoreMutex = false) const;
  llvm::Error validateExecutiveResourceRestoreIRQL(uint8_t IRQL) const;
  bool threadHoldsExecutiveResource(uint64_t Thread) const;
  llvm::Expected<uint64_t> initializeExecutiveResource(uint64_t Address);
  llvm::Expected<uint64_t> resetExecutiveResource(uint64_t Address,
                                                  bool Delete);
  llvm::Expected<uint64_t> acquireExecutiveResource(uint64_t Address,
                                                    ExecutiveResourceMode Mode,
                                                    bool Wait,
                                                    bool EnterCritical = false);
  llvm::Expected<uint64_t> releaseExecutiveResource(uint64_t Address,
                                                    uint64_t Owner,
                                                    bool LeaveCritical);
  llvm::Expected<uint64_t> queryExecutiveResource(uint64_t Address,
                                                  bool Exclusive, bool Waiters);
  llvm::Expected<uint64_t> convertExecutiveResource(uint64_t Address);
  llvm::Expected<uint64_t> disableExecutiveResourceBoost(uint64_t Address);
  llvm::Expected<uint64_t>
  transferExecutiveResource(uint64_t Address, uint64_t Owner, uint32_t Flags);
  struct RaisedIRQL {
    uint64_t Execution;
    uint8_t OldIRQL;
    uint8_t NewIRQL;
  };
  std::vector<RaisedIRQL> RaisedIRQLs;
  llvm::Expected<uint64_t> raiseIRQL(uint8_t RequestedIRQL);
  llvm::Expected<uint64_t> lowerIRQL(uint8_t RequestedIRQL);
  llvm::Expected<uint64_t> processObject(uint32_t ProcessID);
  llvm::Expected<uint64_t> processSectionBaseAddress(uint64_t Object);
  llvm::Expected<uint64_t> processPeb(uint64_t Object);
  llvm::Expected<uint64_t> queryDeviceProperty(llvm::ArrayRef<uint64_t>);
  llvm::Expected<uint64_t> requestorProcess(uint64_t IRP);
  llvm::Expected<uint64_t> currentProcess();
  llvm::Error stackAttachProcess(uint64_t Process, uint64_t ApcState,
                                 bool Private = false);
  llvm::Error unstackDetachProcess(uint64_t ApcState, bool Private = false);
  llvm::Error probeAndLockProcessPages(uint64_t MDL, uint64_t Process,
                                       uint8_t Mode, uint32_t Operation);
  llvm::Expected<uint64_t> allocateUserBuffer(uint32_t Size,
                                              llvm::ArrayRef<uint8_t> Initial,
                                              DriverUserPageAccess Access,
                                              uint32_t ProcessID);
  llvm::Expected<uint64_t> probeUserBuffer(uint64_t Address, uint64_t Size,
                                           uint32_t Alignment, bool ForWrite);
  std::optional<KernelGuestCall> PendingInterruptCall;
  std::map<uint64_t, uint64_t> FrameworkInterruptContinuations;
  std::optional<uint64_t> EnteringPassiveInterrupt;
  std::map<uint64_t, uint64_t> PassiveInterruptThreads;
  std::map<std::pair<uint64_t, uint64_t>, uint8_t> FrameworkInterruptLocks;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> FrameworkPassiveLockThreads;
  std::map<uint64_t, bool> FrameworkLockStorage;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> FrameworkWaitLockThreads;
  struct InterruptParameters {
    uint64_t Record = 0;
    uint64_t Output = 0;
    uint64_t Routine = 0;
    uint64_t Context = 0;
    uint64_t SpinLock = 0;
    uint64_t PDO = 0;
    uint64_t Vector = 0;
    uint64_t IRQL = 0;
    uint64_t Synchronize = 0;
    uint64_t Mode = 0;
    uint64_t Share = 0;
    uint64_t Affinity = 0;
    uint64_t Floating = 0;
    uint64_t Group = 0;
    uint32_t Version = 0;
    bool LineBased = false;
    bool MessageBased = false;
    bool Passive = false;
    uint64_t Fallback = 0;
  };
  llvm::Expected<uint64_t> connectInterrupt(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> connectInterruptEx(uint64_t Record);
  llvm::Expected<uint64_t> registerInterrupt(InterruptParameters Parameters);
  llvm::Expected<uint64_t> disconnectInterrupt(uint64_t Object,
                                               uint32_t Version = 0);
  llvm::Expected<uint64_t> disconnectInterruptEx(uint64_t Record);
  llvm::Expected<uint64_t> acquireInterruptSpinLock(uint64_t Object);
  llvm::Expected<uint64_t> releaseInterruptSpinLock(uint64_t Object,
                                                    uint8_t OldIRQL);
  llvm::Expected<uint64_t>
  synchronizeInterrupt(uint64_t Object, uint64_t Routine, uint64_t Context);
  llvm::Expected<std::optional<uint64_t>> finishInterruptCall(uint64_t Token,
                                                              uint64_t Value);
  llvm::Expected<uint64_t> preflightScheduledBoundary(uint64_t Time);
  llvm::Error processScheduledBoundary(uint64_t Time, bool Executing);
  bool canServicePassiveEvents() const {
    return !InstructionClock ||
           (CurrentIRQL == scheduler::PassiveLevel && !CancelLock.Held);
  }
  llvm::Error processInterruptEvents();
  std::optional<uint64_t> nextInterruptEventTime() const {
    return Interrupts.nextEventTime();
  }
  llvm::Error canReleaseResources(uint64_t PDO) const;
  std::optional<Wait> PendingWait;
  uint64_t NextWaitRegistration = kernel_wait::FirstRegistration;
  std::map<uint64_t, Wait> WaitRegistrations;
  std::map<uint64_t, size_t> WaitReferences;
  std::map<std::pair<uint64_t, uint32_t>, size_t> WaitBlockReferences;
  enum class ObjectKind {
    Process,
    Thread,
    Driver,
    File,
    Event,
    Semaphore,
    Job,
    Token,
    Device,
    Directory,
    SymbolicLink,
    Key
  };
  struct ObjectReferences {
    ObjectKind Kind;
    size_t Pointers = 0;
    size_t Handles = 0;
    std::string Name;
    KernelRegistry *RegistryOwner = nullptr;
  };
  struct ObjectHandle {
    uint64_t Object;
    uint32_t GrantedAccess;
    uint32_t Attributes;
    uint32_t ProcessID;
    bool ScenarioOwned = false;
  };
  // External references have one owner. Execution and registered waits keep
  // their domain-owned holds until completion; they are not caller references.
  std::map<uint64_t, ObjectReferences> ReferencedObjects;
  std::map<ObjectKind, uint64_t> ObjectTypes;
  std::map<uint64_t, ObjectHandle> ObjectHandles;
  uint64_t NextObjectHandle = profile::SystemThreadHandleBase;
  struct ThreadObject {
    uint64_t CallbackID = 0;
    uint32_t ProcessID = 0;
    uint32_t ExitStatus = 0;
    uint32_t ExitPriority = 0;
    bool System = false;
    bool Exited = false;
    bool Terminating = false;
  };
  std::map<uint64_t, ThreadObject> ThreadObjects;
  std::map<uint64_t, uint64_t> CurrentThreadObjects;
  /// Client IDs of live thread objects, assigned on first observation. Issued
  /// IDs are never reused, and requestor process IDs may not collide with them.
  std::map<uint64_t, uint32_t> ThreadClientIDs;
  std::set<uint32_t> IssuedThreadClientIDs;
  std::set<uint32_t> RequestorProcessIDs;
  uint64_t NextThreadClientID = profile::ThreadIDBase;
  llvm::Expected<uint64_t> threadClientID(uint64_t Object);
  struct ThreadIdentity {
    uint64_t ID = 0;
    std::optional<uint64_t> StartAddress;
    std::optional<uint32_t> CreatingProcessID;
    // Unknown borrowed-user state differs from a known non-GUI thread.
    std::optional<uint64_t> Win32Thread;
  };
  // One identity/reference owner for scheduled and borrowed current threads.
  // Exited borrowed threads remain here until their last pointer is released.
  std::map<uint64_t, ThreadIdentity> ThreadIdentities;
  struct NativeInitialThreadView {
    uint64_t Auxiliary, Size;
    bool Readable = true;
  };
  std::map<uint64_t, NativeInitialThreadView> NativeInitialThreadViews;
  llvm::Error initializeNativeThreadView(uint64_t Object,
                                         const ThreadIdentity &Identity);
  bool isNativeThreadViewRead(uint64_t Object, uint64_t Address,
                              uint32_t Size) const;
  llvm::Error publishThreadIdentity(uint64_t Object, ThreadIdentity Identity);
  bool isThreadIdentityRead(uint64_t Object, uint64_t Address,
                            uint32_t Size) const;
  llvm::Expected<uint64_t> threadProcessID(uint64_t Object) const;
  llvm::Expected<uint64_t> threadProcessObject(uint64_t Object);
  uint64_t
  resolvedThreadPriorityKey(uint64_t Key,
                            const ExecutionContext *Paused = nullptr) const;
  std::optional<uint64_t> ForegroundThreadStart;
  // Foreground requests reuse a CPU stack, not the creating process identity.
  // A nested exception keeps both parts of its parent's logical thread key.
  using ThreadHandle = ObjectHandle;
  struct ThreadNotifier {
    uint64_t Routine = 0;
    size_t Busy = 0;
    bool Removing = false;
    uint64_t RemovalThread = 0;
  };
  struct ThreadCreationOutput {
    uint64_t Output = 0, ClientID = 0, Value = 0;
    ThreadHandle Handle;
    bool Reserved = false;
  };
  struct ThreadNotification {
    uint64_t Object = 0, Thread = 0, Registration = 0;
    size_t Cursor = 0, Report = 0;
    ApcState Baseline;
    uint8_t IRQL = 0;
    bool Active = false;
    std::optional<ThreadCreationOutput> Output;
  };
  std::array<std::optional<uint64_t>, 64> ThreadNotifySlots{};
  std::map<uint64_t, ThreadNotifier> ThreadNotifiers;
  std::map<uint64_t, ThreadNotification> ThreadNotifications;
  uint64_t NextThreadNotifier = 1, NextThreadNotification = 1;
  std::optional<KernelGuestCall> PendingThreadNotify;
  std::map<uint64_t, uint32_t> PendingThreadTerminations;
  llvm::Expected<uint64_t> setThreadNotify(uint64_t Routine, bool Remove);
  llvm::Error canStartThreadNotification() const;
  llvm::Error startThreadNotification(
      uint64_t Object, bool Create,
      std::optional<ThreadCreationOutput> Output = std::nullopt);
  llvm::Error prepareThreadNotify(uint64_t ID);
  llvm::Error completeThreadNotification(uint64_t ID);
  llvm::Error beginThreadNotify(uint64_t ID);
  llvm::Expected<std::optional<uint64_t>> finishThreadNotify(uint64_t ID);
  llvm::Expected<std::optional<uint32_t>> pollThreadNotifyRemove(const Wait &);
  llvm::Error validateThreadNotifyUnload() const;
  llvm::Error canReleaseThreadNotifyStorage(uint64_t Base, uint64_t Size) const;
  llvm::Expected<uint64_t>
  createSystemThread(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> terminateSystemThread(uint32_t Status);
  llvm::Expected<uint64_t>
  referenceObjectByHandle(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t>
  referenceObjectByPointer(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> referenceObject(uint64_t Object);
  llvm::Expected<uint64_t> dereferenceObject(uint64_t Object);
  llvm::Expected<uint64_t> objectType(uint64_t Object);
  llvm::Expected<uint64_t> objectType(ObjectKind Kind);
  llvm::Error initializeDataExports(const DriverOptions &Options);
  llvm::Error initializeObjectNamespace();
  struct ObjectLookup {
    uint64_t Status;
    uint64_t Object = 0;
    std::string RemainingName;
  };
  llvm::Expected<ObjectLookup>
  lookupNamedObject(llvm::StringRef Name, uint64_t Root, uint32_t Attributes,
                    bool OpenLink, bool ParseDevice = false,
                    unsigned LinkDepth = 0);
  std::string objectName(uint64_t Object) const;
  uint64_t findSymbolicLink(llvm::StringRef Key) const;
  const ObjectHandle *findObjectHandle(uint64_t Handle) const;
  static std::optional<uint32_t> objectAccessMask(ObjectKind Kind);
  static llvm::Expected<uint32_t> mapObjectAccess(ObjectKind Kind,
                                                  uint32_t Access);
  llvm::Expected<uint64_t> referenceObjectByName(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> openNamedObject(llvm::ArrayRef<uint64_t> A,
                                           ObjectKind Kind);
  llvm::Expected<uint64_t> queryObjectName(uint64_t Object, uint64_t Buffer,
                                           uint32_t Length,
                                           uint64_t ReturnLength,
                                           bool RequiredLength);
  llvm::Expected<uint64_t> queryObject(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> querySymbolicLink(llvm::ArrayRef<uint64_t> A);
  llvm::Error validateDataExportAccess(uint64_t Address, uint32_t Size,
                                       bool IsWrite) const;
  bool DebuggerStateDeclared = false;
  llvm::Expected<uint64_t>
  openObjectByPointer(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> lookupProcess(uint64_t ID, uint64_t Output);
  llvm::Expected<uint64_t> lookupThread(uint64_t ID, uint64_t Output);
  llvm::Expected<uint64_t> threadProcess(uint64_t Object);
  llvm::Expected<uint64_t> threadTerminating(uint64_t Object) const;
  llvm::Error validateObjectOutput(uint64_t Address, uint32_t Size);
  bool matchesObjectType(ObjectKind Kind, uint64_t Type) const;
  void retainObjectHandle(uint64_t Object, uint32_t Access, uint32_t Attributes,
                          uint32_t ProcessID);
  llvm::Expected<uint64_t> referenceThreadByHandle(llvm::ArrayRef<uint64_t>);
  llvm::Expected<uint64_t>
  queryThreadInformation(llvm::ArrayRef<uint64_t> Arguments);
  uint32_t handleProcessID() const;
  bool canAccessHandle(uint32_t Attributes, uint32_t ProcessID) const;
  llvm::Error objectBuffer(uint64_t Address, uint32_t Size, bool IsWrite) const;
  bool canAccessThreadHandle(const ThreadHandle &Handle) const;
  bool hasThreadHandles(uint64_t Object) const;
  llvm::Expected<uint64_t> dereferenceThread(uint64_t Object);
  llvm::Expected<uint64_t> threadID(uint64_t Object) const;
  llvm::Expected<uint64_t> isThreadTerminating(uint64_t Object) const;
  llvm::Expected<uint64_t> currentThreadID();
  llvm::Expected<uint64_t> currentThreadWin32Thread();
  llvm::Expected<uint64_t> threadWin32Thread(uint64_t Object) const;
  llvm::Expected<uint64_t> setThreadWin32Thread(uint64_t Object, uint64_t Value,
                                                uint64_t Expected);
  llvm::Expected<uint64_t> closeHandle(uint64_t Handle);
  void retireObjectIfUnreferenced(uint64_t Object);
  void retireBorrowedThread(uint64_t Key);
  llvm::Expected<uint64_t> threadPriorityKey(uint64_t Object,
                                             bool Changing) const;
  llvm::Expected<uint64_t> queryThreadPriority(uint64_t Object) const;
  llvm::Expected<uint64_t> setThreadPriority(uint64_t Object, int32_t Priority);
  // Canonical logical-thread ownership survives parked and nested executions.
  llvm::Expected<uint64_t> currentProcessorNumber(uint64_t Output);
  llvm::Expected<uint64_t> setSystemAffinity(uint64_t Mask);
  llvm::Expected<uint64_t> revertSystemAffinity();
  std::map<uint64_t, size_t> RemoveLockWaitReferences;
  llvm::Expected<uint64_t>
  initializeRemoveLock(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t>
  acquireRemoveLock(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> releaseRemoveLock(llvm::ArrayRef<uint64_t> Arguments,
                                             bool Wait);
  llvm::Error validateRemoveLockOwner(uint64_t Lock) const;
  llvm::Error canReleaseRemoveLockStorage(uint64_t Base, uint64_t Size) const;
  llvm::Error canReleaseRange(uint64_t Base, uint64_t Size,
                              uint64_t IgnoredDMAPin = 0,
                              uint64_t RetiringLookaside = 0) const;
  llvm::Expected<uint64_t> beginWait(llvm::ArrayRef<uint64_t> Arguments,
                                     bool Delay);
  llvm::Expected<uint64_t>
  beginMultipleWait(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> beginObjectWait(Wait Pending, uint64_t Timeout);
  llvm::Expected<std::optional<uint32_t>>
  acquireWaitObjects(const Wait &Pending);
  llvm::Error releaseWaitReferences(const Wait &Pending);
  llvm::Error
  canRevokeVirtualRange(uint64_t Base, uint64_t Size,
                        std::optional<UsbIdleKey> RetiringUsbIdle = {},
                        uint64_t RetiringLookaside = 0) const;
  llvm::Error prepareRevokeVirtualRange(uint64_t Base, uint64_t Size);
  llvm::Error prepareReleaseRange(uint64_t Base, uint64_t Size,
                                  uint64_t IgnoredDMAPin = 0);
  llvm::Error
  prepareReleaseRanges(llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Ranges,
                       uint64_t IgnoredDMAPin = 0);
  llvm::Error validateGuestAccessImpl(uint64_t Address, uint32_t Size,
                                      bool IsWrite, bool IncludeDispatcher,
                                      bool PhysicalAccess = false,
                                      bool PrepareImageWrites = true) const;
  llvm::Error validateDispatcherStorage(uint64_t Address, uint32_t Size,
                                        bool IsWrite) const;
  uint8_t CurrentIRQL = 0;
  bool InstructionClock = false;
  struct CancelSpinLockState {
    bool Held = false;
    bool Callback = false;
    uint64_t Owner = 0;
    uint64_t CallbackExecution = 0;
    uint64_t IRP = 0;
    uint8_t OldIRQL = 0;
    uint8_t CallbackIRQL = 0;
  } CancelLock;
  std::map<uint64_t, uint64_t> WorkItems;
  std::map<uint64_t, uint64_t> WorkReferences;
  std::map<uint64_t, GuestCallToken> ScheduledModelContinuations;
  llvm::Error processRequestCancellations();
  llvm::Expected<uint64_t> acquireCancelSpinLock(uint64_t OldIRQL);
  llvm::Error releaseCancelSpinLock(uint64_t OldIRQL);
  llvm::Expected<uint64_t> cancelIRP(uint64_t IRP);
  llvm::Expected<uint64_t> allocateWorkItem(uint64_t Device);
  llvm::Error queueWorkItem(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error freeWorkItem(uint64_t Address);
  llvm::Error updateDeviceReferences(uint64_t Device);
  llvm::Expected<uint64_t> resolveRoutine(uint64_t Address);
  llvm::Expected<uint64_t> querySystemInformation(llvm::ArrayRef<uint64_t> Args,
                                                  bool Trusted);
  llvm::Expected<uint64_t> pcToFileHeader(uint64_t PC, uint64_t Output);
  // Only loader-owned mapped spans, excluding image holes. Physical backing is
  // registered lazily when a permitted image range is first locked.
  std::map<uint64_t, uint64_t> ImageRAM;
  ExecutionContract Contract = ExecutionContract::Legacy;
  struct BugCheckRecord {
    uint64_t Callback, Component;
    uint32_t Reason;
    bool Live;
  };
  // Retain opaque record identities after removal: native list/checksum bytes
  // have no concrete model value, even when the State byte becomes empty.
  std::map<uint64_t, BugCheckRecord> BugCheckRecords;
  llvm::Error validateBugCheckRecord(uint64_t Record) const;

  llvm::Error validateBugCheckPageBorrow(uint64_t Address, uint64_t Size) const;
  bool hasLiveBugCheckCallbacks() const;
  llvm::Error validateBugCheckAccess(uint64_t, uint32_t, bool) const;
  llvm::Error validateStrictBugCheckAccess(uint64_t, uint32_t, bool) const;
  llvm::Error validateNativeBugCheckAccess(uint64_t, uint32_t, bool) const;
  llvm::Expected<uint64_t> registerBugCheckCallback(uint64_t Record,
                                                    uint64_t Callback,
                                                    uint32_t Reason,
                                                    uint64_t Component);
  llvm::Expected<uint64_t> deregisterBugCheckCallback(uint64_t Record);
  std::optional<uint64_t> imageOwnerForRange(uint64_t Address,
                                             uint64_t Size) const;
  bool unpackImageMDLCall(KernelAPIKind Kind,
                          llvm::ArrayRef<uint64_t> Arguments) const;
  uint64_t DriverObject = 0;
  std::string APICallDetail;
  struct NamedDriverObject {
    std::string Name;
    uint64_t Object = 0;
    bool Guest = false;
    std::array<bool, profile::MajorFunctionCount * 8> DispatchWritten{};
    std::shared_ptr<RAMInitializationWatch> DispatchInitialization;
  };
  std::map<std::string, NamedDriverObject> NamedDrivers;
  bool DriverNamespaceKnown = false;
  std::vector<std::pair<uint64_t, uint64_t>> NamedDriverNameRanges;
  llvm::Error initializeDriverNamespace(const DriverOptions &Options);
  llvm::Expected<uint64_t> referenceDriverObjectByName(llvm::ArrayRef<uint64_t>,
                                                       llvm::StringRef);
  bool isNamedDriverObject(uint64_t Object) const;
  llvm::Error validateNamedDriverAccess(uint64_t Address, uint32_t Size,
                                        bool IsWrite) const;
  llvm::Error validateNamedDriverUnload() const;
  bool isGuestDriverObject(uint64_t Object) const;
  llvm::Expected<uint64_t> guestDriverDispatch(uint64_t Owner,
                                               unsigned Major) const;
  struct ServiceImageState {
    DriverServiceImage Resource;
    std::shared_ptr<DriverImage> Image;
    enum class State {
      Unloaded,
      Loading,
      Loaded,
      Unloading
    } Phase = State::Unloaded;
    bool Mapped = false, Active = false;
    uint64_t Object = 0, Extension = 0, Module = 0, Path = 0, Thread = 0;
    std::vector<std::pair<uint64_t, uint64_t>> Storage;
  };
  bool ServiceImagesKnown = false;
  std::string PrimaryServiceName;
  std::map<uint64_t, ServiceImageState> ServiceImages;
  std::optional<KernelGuestCall> PendingServiceCall;
  llvm::Error initializeServiceImages(const DriverImage &,
                                      const DriverOptions &);
  llvm::Expected<uint64_t> callDriverService(llvm::StringRef, uint64_t);
  llvm::Error beginServiceCall(uint64_t);
  llvm::Expected<std::optional<uint64_t>> finishServiceCall(uint64_t, uint64_t);
  llvm::Error mapServiceImage(ServiceImageState &);
  llvm::Error createServiceObject(ServiceImageState &);
  llvm::Error retireServiceImage(ServiceImageState &);
  llvm::Error validateServiceUnload() const;
  llvm::Error validateServiceCallContext(llvm::StringRef,
                                         uint64_t PC = 0) const;
  uint64_t DriverModule = 0;
  ContextCapture CaptureContext;
  const KernelReferenceImage *DebuggerReference = nullptr;
  std::vector<uint8_t> DebuggerData;
  uint32_t ProcessObjectSize = profile::ProcessTokenSize;
  uint32_t ThreadObjectSize = profile::ProcessTokenSize;
  std::unique_ptr<KernelGuestPageTables> GuestPageTables;
  std::unique_ptr<KernelGuardBitmap> GuardBitmap;
  bool WindowsNonpagedMDLMappings = false;
  // Deterministic guest boot state, independent of host secure-memory state.
  uint64_t SecureMemoryCookie = 0x4e44536563757265ULL;
  llvm::Error validateReferenceDataAccess(uint64_t Address, uint32_t Size,
                                          bool IsWrite) const;
  std::set<uint64_t> PersistentDumps;
  uint64_t CapturingPersistentDump = 0;
  llvm::Expected<uint64_t> capturePersistentState(llvm::ArrayRef<uint64_t> A);
  llvm::Error validatePersistentDumpAccess(uint64_t Address, uint32_t Size,
                                           bool IsWrite) const;
  struct LoadedModule {
    uint64_t Base, Size;
    std::string Path;
    bool Guest;
    uint16_t DLLCharacteristics = 0;
    uint64_t Record = 0;
    std::string InstanceID;
    std::shared_ptr<const modules::ModuleImage> Source;
    std::string Name;
  };
  class ModulePublicationState;
  std::vector<LoadedModule> LoadedModules;
  llvm::Expected<uint64_t>
  queryNativeSystemInformation(llvm::ArrayRef<uint64_t> A, bool ZwCall);
  llvm::Expected<uint64_t> queryFirmwareInformation(llvm::ArrayRef<uint64_t> A,
                                                    bool ZwCall);
  std::optional<DriverSmbios> Smbios;
  std::optional<DriverAcpiTables> AcpiTables;
  struct AvlNode {
    uint64_t Parent = 0, Left = 0, Right = 0;
    uint32_t Size = 0, Height = 1;
  };
  struct AvlTable {
    uint64_t Root = 0, Compare = 0, Allocate = 0, Free = 0, Context = 0,
             Busy = 0;
    uint32_t Deletes = 0;
    std::map<uint64_t, AvlNode> Nodes;
    std::vector<uint8_t> Expected;
  };
  struct AvlOperation {
    enum class Kind { Lookup, LookupFull, Insert, InsertFull, Delete } Type;
    enum class Stage { Compare, Allocate, Free } Phase = Stage::Compare;
    uint64_t Table = 0, Input = 0, Flag = 0, ParentOut = 0, SearchOut = 0,
             Node = 0, Parent = 0, Thread = 0;
    uint32_t Size = 0, Search = 0;
    uint8_t IRQL = 0;
    bool Found = false, Active = false;
  };
  std::map<uint64_t, AvlTable> AvlTables;
  std::map<uint64_t, AvlOperation> AvlOperations;
  std::optional<KernelGuestCall> PendingAvlCall;
  uint64_t NextAvlOperation = 1;
  llvm::Expected<uint64_t> callAvl(llvm::StringRef, llvm::ArrayRef<uint64_t>);
  llvm::Error beginAvlCall(uint64_t);
  llvm::Expected<std::optional<uint64_t>> finishAvlCall(uint64_t, uint64_t);
  llvm::Expected<std::optional<uint64_t>> advanceAvl(uint64_t);
  llvm::Error writeAvlTable(uint64_t);
  llvm::Error validateAvlTable(uint64_t) const;
  llvm::Error validateAvlAccess(uint64_t, uint32_t, bool) const;
  llvm::Error canReleaseAvlStorage(uint64_t, uint64_t) const;
  void forgetEmptyAvlStorage(uint64_t, uint64_t);
  llvm::Error validateAvlUnload() const;
  std::optional<uint64_t> HalDispatchSlot, HalCacheFunction;
  std::vector<std::pair<uint64_t, uint64_t>> HalAcpiCache;
  std::optional<DriverOptions::WindowsVersion> Version;
  std::optional<bool> SecurePciEnabled;
  llvm::Expected<uint64_t> querySecurePciEnabled() const;
  std::optional<bool> DmaGuardPolicyEnabled;
  std::optional<uint32_t> CodeIntegrityOptions;
  bool ForegroundReadyCallbacks = false;
  llvm::Expected<uint64_t> queryCodeIntegrity(llvm::ArrayRef<uint64_t> A,
                                              bool ZwCall);
  llvm::Expected<uint64_t> queryDmaGuardPolicy(llvm::ArrayRef<uint64_t> A,
                                               bool ZwCall);
  std::optional<DriverPciMcfg> PciMcfg;
  uint32_t PciMcfgCount = 0;
  bool PciMcfgPublished = false;
  llvm::Error configurePciMcfg(const std::optional<DriverPciMcfg> &Facts,
                               bool RequireSymbols = true);
  llvm::Error initializePciMcfg();
  bool permitsPciMcfgFieldRead(uint64_t Address, uint32_t Size) const;
  llvm::Error validatePciMcfgAccess(uint64_t Address, uint32_t Size,
                                    bool IsWrite) const;
  bool WindowsPoolTags = false;
  bool WindowsIRPInitialization = false;
  llvm::Expected<uint64_t> getVersion(uint64_t Address);
  struct BugCheckRegistration {
    uint64_t Routine, Component, ComponentSize;
    bool ComponentStorageReleased = false;
  };
  std::map<uint64_t, BugCheckRegistration> BugCheckCallbacks;
  struct ExecutiveCallbackObject {
    std::string Name;
    size_t PointerReferences = 0, Registrations = 0, Notifications = 0;
    bool Multiple = true, Permanent = false, System = false;
  };
  struct ExecutiveCallbackRegistration {
    uint64_t Object = 0, Routine = 0, Context = 0;
    uint64_t UnregisterThread = 0;
    size_t Busy = 0;
    bool Removing = false;
  };
  struct ExecutiveNotification {
    uint64_t Object = 0, Argument1 = 0, Argument2 = 0;
    uint64_t Cursor = 0, Registration = 0, Thread = 0;
    uint8_t IRQL = 0;
    bool Active = false;
  };
  std::map<std::string, uint64_t> ExecutiveCallbackNames;
  std::map<uint64_t, ExecutiveCallbackObject> ExecutiveCallbacks;
  std::map<uint64_t, ExecutiveCallbackRegistration> ExecutiveRegistrations;
  std::map<uint64_t, ExecutiveNotification> ExecutiveNotifications;
  std::optional<KernelGuestCall> PendingExecutiveCallback;
  uint64_t NextExecutiveNotification = 1;
  llvm::Expected<uint64_t> createExecutiveCallback(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t>
  registerExecutiveCallback(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> notifyExecutiveCallback(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> unregisterExecutiveCallback(uint64_t Registration);
  llvm::Expected<uint64_t> dereferenceExecutiveCallback(uint64_t Object);
  llvm::Expected<uint64_t> makeCallbackTemporary(uint64_t Object);
  void retireExecutiveCallback(uint64_t Object);
  void eraseExecutiveRegistration(uint64_t Registration);
  llvm::Expected<bool> prepareExecutiveCallback(uint64_t Notification);
  llvm::Error beginExecutiveCallback(uint64_t Notification);
  llvm::Expected<std::optional<uint64_t>>
  finishExecutiveCallback(uint64_t Notification);
  llvm::Expected<std::optional<uint32_t>>
  pollExecutiveUnregister(const Wait &Pending);
  llvm::Error validateExecutiveCallbackAccess(uint64_t Address,
                                              uint32_t Size) const;
  llvm::Error canReleaseExecutiveCallbackStorage(uint64_t Base,
                                                 uint64_t Size) const;
  llvm::Error validateExecutiveCallbackUnload() const;
  llvm::Expected<uint64_t> registerBugCheck(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> deregisterBugCheck(uint64_t Record);

  llvm::Error canReleaseBugCheckStorage(uint64_t Base, uint64_t Size) const;
  void noteBugCheckComponentRelease(uint64_t Base, uint64_t Size);
  void snapshotBugCheckCallbacks();
  std::vector<std::pair<uint64_t, uint64_t>> ModuleNameRanges;
  llvm::Error validateLoadedModuleAccess(uint64_t Address, uint32_t Size,
                                         bool IsWrite) const;
  uint64_t RegistryPath = 0;
  uint64_t DriverExtension = 0;
  uint64_t DriverLoaderRecord = 0;
  bool EntryFinished = false;
  std::optional<std::map<uint64_t, std::vector<uint8_t>>> UnpackBaseline;
  bool UnpackOpaqueEffects = false;
  llvm::Error validateQueryBuffer(uint64_t Address, uint32_t Size,
                                  bool Writable, llvm::StringRef API);
  std::optional<DriverKernelVaShadowQuery> KernelVaShadowQuery;
  llvm::Expected<uint64_t> queryKernelVaShadow(llvm::ArrayRef<uint64_t> Args);
  std::optional<DriverVersionQuery> VersionQuery;
  llvm::Expected<uint64_t> queryVersion(uint64_t Address);
  llvm::Expected<uint64_t>
  captureCallerContext(uint64_t Address, const KernelContextCapture *Context);
  std::optional<DriverPhysicalMemoryInventory> PhysicalMemoryInventory;
  llvm::Expected<uint64_t> queryPhysicalMemoryRanges();
  // Admission conservatively records reads that can expose model PFNs,
  // including reads performed by modeled copy/compare services.
  mutable bool UnpackMDLIdentityRead = false;
  uint64_t KernelMemoryBytes = profile::KernelArenaSize;
  uint64_t NextAllocation = 0;
  uint64_t AllocationEnd = 0;
  uint64_t NextPagedAllocation = 0;
  uint64_t PagedAllocationEnd = 0;
  struct PoolAllocation {
    uint64_t Size;
    uint32_t Tag;
    bool NonPaged;
    // Kernel-returned buffers need pool lifetime checks but may have no
    // supported external allocation recipe for native reconstruction.
    std::optional<WindowsDriverState::PoolAPI> API;
    uint64_t TypeOrFlags;
    bool Contiguous = false;
  };
  std::map<uint64_t, PoolAllocation> Allocations;
  std::map<uint64_t, uint64_t> ArenaAllocations;
  enum class DeviceOwnerKind {
    Guest,
    PnpProvider,
    FileSystemProvider,
    FiniteFileSystem
  };
  struct DeviceRecord {
    uint64_t Address = 0;
    uint64_t Extension = 0;
    uint32_t Type = 0;
    std::string Name;
    uint64_t OwnerDriver = 0;
    DeviceOwnerKind OwnerKind = DeviceOwnerKind::Guest;
    // Association survives detach; it is not the current attachment edge.
    uint64_t PnpDevice = 0;
    uint64_t Lower = 0;
    uint64_t Upper = 0;
    uint64_t Size = 0;
    // Request and callback holds are distinct from open handles and from the
    // DEVICE_OBJECT.ReferenceCount field. Attachment links also prevent retire.
    uint64_t InternalReferences = 0;
    // Each successful legacy registration retains a queue entry. Unregister
    // and IoDeleteDevice cancel all entries for this object.
    uint32_t ShutdownNotifications = 0;
    bool DeletePending = false;
    bool UnlinkedFromDriver = false;
    std::optional<DevicePowerState> ReportedDevicePower;
  };
  std::map<uint64_t, DeviceRecord> Devices;
  // PnP provider identities persist after the concrete PDO is retired.
  struct PnpDeviceRecord {
    uint64_t PDO = 0;
    // Devnode parent identity persists independently of attachment and
    // presence.
    uint64_t ParentPDO = 0;
    size_t ResultIndex = 0;
    DriverBusKind Bus = DriverBusKind::ResourceFree;
    std::optional<uint32_t> AddDeviceStatus;
    bool AddDeviceActive = false;
    bool FrameworkAdd = false;
    uint64_t FrameworkInit = 0;
    std::set<uint64_t> ExistingGuestDevices;
    std::set<uint64_t> GuestDevices;
    std::optional<DevicePowerState> InitialReportedDevicePower;
    std::vector<DriverPowerOperation> RequestedDevicePower;
    uint32_t RequestedPowerIndex = 0;
    // Identity of the last successfully committed START transaction.
    uint64_t StartEpoch = 0;
    std::optional<DriverWakeCapabilities> WakeCapabilities;
  };
  uint64_t PnpProviderDriver = 0;
  uint64_t FileSystemProviderDriver = 0;
  uint64_t deviceOwnerDriver(DeviceOwnerKind Kind) const;
  llvm::Error initializeFileSystem(const DriverOptions &Options);
  llvm::Error validateFileSystemRoute(
      llvm::ArrayRef<uint64_t> Route, uint64_t FileObject = 0,
      DriverRequestKind Kind = DriverRequestKind::DeviceControl) const;
  llvm::Expected<uint64_t> dispatchFileSystem(uint64_t Device, uint64_t IRP);
  bool PnpDevicesPrepared = false;
  std::vector<DriverPnpDevice> ConfiguredPnpDevices;
  std::map<std::string, PnpDeviceRecord> PnpDevices;
  DeviceLifecycle Lifecycle;
  bool isPnpProviderDevice(uint64_t Address) const;
  PnpDeviceRecord *pnpDeviceForPDO(uint64_t PDO);
  const PnpDeviceRecord *pnpDeviceForPDO(uint64_t PDO) const;
  llvm::Expected<uint64_t> pnpDeviceForRoute(uint64_t Device) const;
  llvm::Expected<uint64_t> deviceStartEpoch(uint64_t PDO,
                                            bool AllowStarting) const;
  llvm::Error validatePnpTopologyTransition(uint64_t PDO,
                                            DevicePnpRequest Minor) const;
  llvm::Error finishPnpRemoval(uint64_t PDO);
  llvm::Error retirePnpProvider(uint64_t PDO);
  llvm::Error snapshotPnpDevices();
  std::map<uint64_t, uint64_t> FreedRanges;
  std::array<bool, 28 * 8> DispatchBytesWritten{};
  std::shared_ptr<RAMInitializationWatch> DispatchInitialization;
  struct SymbolicLinkObject {
    std::string Target;
    std::vector<uint8_t> TargetBytes;
    bool Permanent = true;
    bool Named = true;
  };
  std::map<uint64_t, SymbolicLinkObject> SymbolicLinks;
  enum class FileState { Failed, Opening, Open, Cleaned, Closing };
  struct DeferredFileClose {
    DriverRequest Input;
    size_t ResultIndex;
    std::optional<size_t> SourceIndex;
  };
  struct OpenFile {
    enum class Origin { Scenario, DriverCreate };
    Origin Source = Origin::Scenario;
    uint64_t Address = 0;
    uint64_t Device = 0;
    /// Stable lifecycle owner even after the opened FDO is detached.
    uint64_t PnpDevice = 0;
    bool Asynchronous = false;
    FileState State = FileState::Opening;
    uint64_t ScenarioHandle = 0;
    uint32_t ScenarioID = 0;
    bool ScenarioCleanupRequested = false;
    uint32_t HandleCloseHolds = 0;
    uint32_t InternalIOHolds = 0;
    bool FileEventInitialized = false;
    uint64_t CreateOperation = 0;
    std::optional<uint64_t> ProviderOpen;
    uint64_t NameStorage = 0;
    uint64_t NameStorageSize = 0;
    bool OpenCancelled = false;
    uint64_t RelatedFile = 0;
    std::optional<DeferredFileClose> DeferredCleanup;
    std::optional<DeferredFileClose> DeferredClose;
  };
  bool owesFileClose(const OpenFile &File) const;
  void associateFileRequest(DriverRequestResult &Observation,
                            const OpenFile &File) const;
  llvm::Error validateFileSystemOpenBindings() const;
  // Scenario identifiers name an open instance, not its object-manager body.
  // A new CREATE may reuse an identifier while an older body remains held.
  std::map<uint32_t, uint64_t> Files;
  std::map<uint64_t, OpenFile> FileObjects;
  llvm::Expected<Invocation> beginFileRequest(const DriverRequest &Input,
                                              std::optional<size_t> SourceIndex,
                                              size_t Index,
                                              uint64_t RetainedFile = 0,
                                              uint64_t DispatchDevice = 0,
                                              uint64_t FileOperationID = 0);
  llvm::Error retireFileIfUnreferenced(
      uint64_t Object, std::optional<uint32_t> CallerProcess = std::nullopt);
  llvm::Error tryFinalizeFileClose(uint64_t IRP);
  llvm::Error waitForFileClose(uint64_t IRP);
  llvm::Expected<std::optional<uint64_t>>
  finishFileCloseDispatch(uint64_t Token, uint64_t ResultValue);
  std::set<uint64_t> InternalFileCloses;
  struct FileHandleClose {
    uint64_t File = 0;
    uint64_t IRP = 0;
    uint32_t CallerProcess = 0;
  };
  std::map<uint64_t, FileHandleClose> FileHandleCloses;
  uint64_t NextFileHandleClose = 1;
  llvm::Error validateFileHandleClose(uint64_t File, bool Scenario) const;
  llvm::Expected<std::optional<DriverRequestKind>>
  planFileHandleClose(uint64_t Handle) const;
  void releaseFileHandle(uint64_t Handle);
  llvm::Expected<Invocation> beginFileHandleRequest(uint64_t File,
                                                    DriverRequestKind Kind,
                                                    uint32_t ProcessID);
  llvm::Error closeFileHandle(uint64_t Handle);
  llvm::Error dispatchFileHandleClose(uint64_t ID, const Invocation &Call,
                                      uint32_t ProcessID);
  llvm::Error waitForFileHandleClose(uint64_t ID);
  llvm::Expected<std::optional<uint64_t>>
  finishFileHandleDispatch(uint64_t Token, uint64_t ReturnValue);
  struct FileRequest {
    uint64_t File = 0, IRP = 0, ReturnedLength = 0, Storage = 0;
    uint64_t StatusOutput = 0;
    std::array<uint8_t, 16> StatusBytes{};
    uint64_t CallerExecution = 0, CallerThread = 0;
    uint32_t CallerProcess = 0;
    uint32_t Status = 0, Information = 0;
    bool Released = false;
    bool CompletionPublished = false;
    bool CriticalRegion = false;
    bool Native = false, PacketReleased = false, CopyStatusBlock = false;
  };
  std::map<uint64_t, FileRequest> FileRequests;
  uint64_t NextFileRequest = 1;
  std::optional<uint64_t> PendingFileRequestContinuation;
  enum class FileDeviceQuery { Base, Related };
  llvm::Expected<uint64_t> queryFileDevice(uint64_t File,
                                           FileDeviceQuery Query) const;
  llvm::Expected<uint64_t>
  beginFileAPIRequest(llvm::ArrayRef<uint64_t> A, DriverRequestKind Kind,
                      bool Native, const KernelContextCapture *Context);
  llvm::Expected<uint64_t>
  setFileInformation(llvm::ArrayRef<uint64_t> A,
                     const KernelContextCapture *Context);
  bool nativeFileRequest(uint64_t ID) const;
  llvm::Error releaseNativeFileRequestPacket(uint64_t IRP);
  llvm::Error validateFileAPIInterrupts(const KernelContextCapture *Context);
  llvm::Error publishFileRequestCompletion(uint64_t IRP);
  llvm::Expected<bool> fileRequestReady(uint64_t ID) const;
  llvm::Error waitForFileRequest(uint64_t ID);
  llvm::Expected<std::optional<uint64_t>>
  finishFileRequestDispatch(uint64_t Token, uint64_t ReturnValue);
  struct FileOperation {
    bool CancelOpen = false;
    bool DevicePointer = false, PointerPublished = false,
         CriticalRegion = false;
    uint64_t File = 0, IRP = 0, Root = 0;
    uint64_t HandleOutput = 0, StatusOutput = 0, ReservedHandle = 0;
    uint64_t FileOutput = 0, DeviceOutput = 0;
    uint64_t Storage = 0, StorageSize = 0, AccessState = 0;
    uint64_t CallerExecution = 0, CallerThread = 0;
    uint32_t CallerProcess = 0, Access = 0, Attributes = 0;
    uint32_t Status = 0;
    uint64_t Information = 0;
    size_t ResultIndex = 0;
    bool Released = false, RootReleased = false, CompletionPublished = false;
  };
  std::map<uint64_t, FileOperation> FileOperations;
  uint64_t NextFileOperation = 1;
  std::optional<uint64_t> PendingFileOperation;
  enum class FileOpenKind { Create, Open, DevicePointer };
  llvm::Expected<uint64_t> createFile(llvm::ArrayRef<uint64_t> A,
                                      FileOpenKind Kind,
                                      const KernelContextCapture *Context);
  llvm::Expected<uint64_t>
  getDeviceObjectPointer(llvm::ArrayRef<uint64_t> A,
                         const KernelContextCapture *Context);
  llvm::Expected<uint64_t> cancelFileOpen(llvm::ArrayRef<uint64_t> A,
                                          const KernelContextCapture *Context);
  llvm::Expected<bool> fileOperationReady(uint64_t ID) const;
  llvm::Error waitForFileOperation(uint64_t ID);
  llvm::Expected<std::optional<uint64_t>> resumeFileOperation(uint64_t ID);
  llvm::Expected<std::optional<uint64_t>>
  finishFileOperationDispatch(uint64_t Token, uint64_t ReturnValue);
  llvm::Error startFileOperationDispatch(uint64_t ID, const Invocation &Call);
  llvm::Error prepareCancelFileOpenPacket(uint64_t IRP, uint64_t ID);
  llvm::Error publishFileOperationCompletion(uint64_t IRP);
  void retainReservedObjectHandle(uint64_t Handle, uint64_t Object,
                                  uint32_t Access, uint32_t Attributes,
                                  uint32_t ProcessID);
  enum class NativeFileState { Closed, Opening, Open, Cleaned };
  struct NativeOpenFile {
    uint64_t Address = 0, Device = 0, PnpDevice = 0;
    bool Asynchronous = false;
    NativeFileState State = NativeFileState::Opening;
  };
  std::map<uint32_t, NativeOpenFile> NativeOpenFiles;
  // Native file opens share FILE_OBJECT identity/lifecycle with scenario I/O.
  std::optional<std::vector<DriverFiniteFileSystemVolume>> FiniteFileSystem;
  std::optional<bool> FilterManagerFileNames;
  struct FilterNameRecord {
    uint64_t Size = 0, References = 0, NameAddress = 0;
    uint32_t Format = 0;
    bool Cached = false, Parsed = false;
    std::u16string Name;
    uint32_t VolumeUnits = 0;
  };
  std::map<uint64_t, FilterNameRecord> FilterNames;
  std::map<std::pair<uint64_t, uint32_t>, uint64_t> FilterNameCache;
  static std::optional<unsigned>
  subsystemArgumentCount(const KernelExportRegistry::Export &);
  llvm::Expected<uint64_t> callSubsystem(const KernelExportRegistry::Export &,
                                         llvm::ArrayRef<uint64_t>);
  llvm::Expected<uint64_t> callFilterName(llvm::StringRef,
                                          llvm::ArrayRef<uint64_t>);
  llvm::Error parseFilterName(uint64_t);
  llvm::Error freeFilterName(uint64_t);
  llvm::Error validateFilterNameAccess(uint64_t, uint32_t, bool) const;
  llvm::Error releaseFilterNameCache();
  llvm::Error retireFilterNamesForFile(uint64_t);
  std::optional<DriverNetworkInterfaces> NetworkInterfaces;
  std::set<uint64_t> MibTables;
  std::optional<DriverNdisConfiguration> NdisConfiguration;
  struct NdisConfigurationHandle {
    std::string RegistryPath;
    std::optional<uint64_t> Key;
    std::vector<uint64_t> Allocations;
  };
  struct NdisConfigurationStorage {
    uint64_t Size = 0;
    std::vector<std::pair<uint64_t, uint64_t>> Readable;
  };
  std::map<uint64_t, NdisConfigurationHandle> NdisConfigurationHandles;
  std::map<uint64_t, NdisConfigurationStorage> NdisConfigurationAllocations;
  std::map<std::string, uint64_t> NdisPredefinedParameters;
  bool ReleasingNdisConfiguration = false;
  llvm::Error initializeNdisConfiguration();
  llvm::Expected<uint64_t> callNdisConfiguration(llvm::StringRef,
                                                 llvm::ArrayRef<uint64_t>);
  llvm::Error validateNdisConfigurationAccess(uint64_t, uint32_t, bool) const;
  llvm::Expected<uint64_t> callNetio(llvm::StringRef, llvm::ArrayRef<uint64_t>);
  uint64_t FiniteFileSystemDriver = 0;
  std::map<size_t, uint64_t> VolumeDevices;
  struct FileNode {
    std::string Path;
    bool Directory = false;
    std::vector<uint8_t> Data;
    bool DeletePending = false;
    std::string Key;
    std::weak_ptr<ImageSection> Image;
  };
  std::map<size_t, std::map<std::string, std::shared_ptr<FileNode>>> FileNodes;
  std::set<std::string> FiniteFileSystemLinks;
  struct NativeFile {
    uint32_t ID = 0;
    size_t Volume = 0;
    std::shared_ptr<FileNode> Node;
    bool RawVolume = false;
    uint32_t Access = 0;
    uint32_t Share = 0;

    std::string Name;
    uint64_t NameStorage = 0;
    bool DeleteOnClose = false;
  };
  struct FileHandle {
    uint64_t Object = 0;
    uint32_t Attributes = 0;
    uint32_t ProcessID = 4;
    uint32_t Access = 0;
  };
  std::map<uint64_t, NativeFile> NativeFiles;
  std::map<uint64_t, FileHandle> FileHandles;
  struct SectionHandle {
    uint32_t Access = 0, Attributes = 0, ProcessID = 4;
    // Empty selects the existing PhysicalMemory provider.
    std::string Name;
    std::shared_ptr<ImageSection> Image;
  };
  struct NamedSection {
    std::string Name;
    uint64_t Backing = 0, Size = 0;
    uint32_t Protection = 4;
  };
  std::optional<DriverSectionNamespace> SectionNamespace;
  std::map<std::string, NamedSection> NamedSections;
  llvm::Error initializeSections(const DriverOptions &Options);
  llvm::Error snapshotSections();
  std::map<uint64_t, SectionHandle> SectionHandles;
  uint64_t NextSectionHandle = 0x52000000;
  struct SectionView {
    uint64_t Base = 0, Size = 0, Offset = 0;
    uint32_t ProcessID = 4;
    unsigned Permissions = 0;
    bool Inherit = false;
    KernelPhysicalMemory::CacheType Cache =
        KernelPhysicalMemory::CacheType::Cached;
    std::vector<KernelPhysicalMemory::PhysicalViewPage> Pages;
    std::map<uint64_t, uint64_t> Pins;
  };
  using SectionViewKey = std::pair<uint32_t, uint64_t>;
  std::map<SectionViewKey, SectionView> SectionViews;
  llvm::Expected<uint64_t> openSection(llvm::ArrayRef<uint64_t>);
  llvm::Expected<uint64_t> mapSection(llvm::ArrayRef<uint64_t>);
  llvm::Expected<uint64_t> unmapSection(llvm::ArrayRef<uint64_t>);
  llvm::Expected<uint64_t> querySectionObject(llvm::ArrayRef<uint64_t>);
  llvm::Expected<bool> validateSectionViewAccess(uint64_t, uint32_t, bool,
                                                 bool) const;
  llvm::Error retireSectionView(SectionViewKey, bool AlreadyUnmapped = false);
  llvm::Error retireProcessSections(uint32_t, bool AlreadyUnmapped = false);
  llvm::Error inheritSectionViews(uint32_t Parent, uint32_t Child);
  llvm::Error pinSectionView(SectionView &);
  llvm::Error releaseSectionPins(SectionView &);
  uint64_t NextFileHandle = 0x50000000;
  uint32_t NextNativeFileID = UINT32_MAX;
  llvm::Error prepareFiniteFileSystem();
  llvm::Error snapshotFiniteFileSystem();
  std::pair<size_t, uint64_t> finiteFileSystemUsage() const;
  llvm::Error unlinkFileIfClosed(size_t Volume,
                                 const std::shared_ptr<FileNode> &Node);
  llvm::Error setFileDeletePending(size_t Volume,
                                   const std::shared_ptr<FileNode> &Node,
                                   bool Pending);
  struct FilePath {
    size_t Volume = 0;
    uint64_t Device = 0;
    std::string Relative;
    std::string Key;
    bool Raw = false;
    bool CaseInsensitive = false;
    uint32_t Status = 0;
    uint32_t Attributes = 0;
  };
  llvm::Expected<FilePath> resolveFilePath(uint64_t Attributes);
  std::shared_ptr<FileNode> findFileNode(FilePath &Path) const;
  bool fileDirectoryEmpty(size_t Volume, const FileNode &Node) const;
  llvm::Expected<uint64_t> deleteFile(uint64_t Attributes);
  llvm::Expected<uint64_t> fileInformation(llvm::ArrayRef<uint64_t> A,
                                           bool Set);
  llvm::Error retireNativeFile(uint64_t Object);
  bool canAccessFileHandle(const FileHandle &Handle) const;
  llvm::Expected<uint64_t> fileCall(llvm::StringRef Name,
                                    llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> openFile(llvm::ArrayRef<uint64_t> A,
                                    uint64_t DeviceHint = 0);
  llvm::Expected<uint64_t> referenceFile(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> openFilePointer(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> dereferenceFile(uint64_t Object);
  llvm::Expected<uint64_t> finiteFileSystemDevice(uint64_t Object);
  llvm::Expected<uint64_t> closeFile(uint64_t Handle);
  llvm::Error releaseFiniteFileSystem();
  bool Unloading = false;
  bool Unloaded = false;
  struct RequestedPower {
    uint64_t RequestDevice = 0;
    uint64_t Callback = 0;
    uint64_t Context = 0;
    // A separate callback input snapshot; the completed IRP stays retired.
    uint64_t StatusBlock = 0;
    uint32_t ResponseIndex = 0;
    std::optional<uint64_t> StartEpoch;
    bool CallbackStarted = false;
    bool CallbackReturned = false;
    DriverRequestOrigin Origin = DriverRequestOrigin::PoRequestPowerIrp;
    uint64_t FrameworkParent = 0;
    bool PrepareSystemSleep = false;
  };
  enum class PowerRequestDelivery { Inline, Queued };
  struct PowerRequestPlan {
    uint64_t PDO = 0, Top = 0, PC = 0;
    uint8_t StackCount = 0;
    DeviceLifecycleSnapshot Before;
    std::vector<uint64_t> Route;
    bool WaitWake = false, FrameworkPower = false;
    std::optional<uint64_t> StartEpoch;
  };
  struct UserRegion {
    DriverUserBufferKind Kind;
    std::string ID;
    uint64_t Address;
    uint32_t Size;
  };
  struct ControlIRPBuilder {
    uint64_t Device, Input, Output, Event, IOSB, ThreadObject, ThreadKey;
    uint32_t Code, InputSize, OutputSize, Method;
    bool Internal;
    uint64_t SystemBuffer = 0, Mdl = 0;
    llvm::SmallVector<bool, 0> SystemInitialized;
    std::shared_ptr<RAMInitializationWatch> SystemInitialization;
  };
  struct DriverIRPAllocation {
    std::optional<ControlIRPBuilder> Builder;
    uint64_t Size = 0;
    uint8_t StackCount = 0;
    bool Submitted = false;
    bool StorageReleased = false;
    bool CompletionHeld = false;
    uint32_t DispatchSlot = 0;
    uint64_t CompletionDevice = 0;
    std::optional<uint64_t> InitialDispatchToken;
    std::optional<uint32_t> ProviderDispatchReturn;
    std::optional<uint64_t> FreeCompletionToken;
    std::array<bool, 16> IOStatusWritten{};
    std::shared_ptr<RAMInitializationWatch> IOStatusInitialization;
  };
  std::map<uint64_t, DriverIRPAllocation> DriverIRPs;
  llvm::Expected<uint64_t>
  allocateDriverIRP(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error initializeIRPHeader(uint64_t IRP, uint8_t StackCount,
                                  uint8_t CurrentLocation,
                                  std::array<bool, 16> &IOStatusInitialized);
  llvm::Error freeDriverIRP(uint64_t IRP);
  llvm::Error releaseDriverIRPStorage(uint64_t IRP);
  llvm::Error freeSubmittedDriverIRP(uint64_t IRP);
  llvm::Error adoptDriverIRP(uint64_t Device, uint64_t IRP);
  llvm::Expected<uint64_t>
      buildDeviceIoControlRequest(llvm::ArrayRef<uint64_t>);
  llvm::Error adoptBuiltControlIRP(uint64_t Device, uint64_t IRP);
  llvm::Error validateBuiltControlCompletion(uint64_t IRP) const;
  llvm::Error notifyBuiltControlCompletion(uint64_t IRP);
  llvm::Error canReleaseBuiltControlStorage(uint64_t, uint64_t) const;
  llvm::Error validateBuiltControlBufferAccess(uint64_t, uint32_t, bool) const;
  llvm::Error validateDriverIRPCompletion(uint64_t IRP, uint32_t Status,
                                          uint64_t Information) const;
  llvm::Error captureDriverIRPCompletion(uint64_t IRP);
  llvm::Error recordDriverIRPDispatchReturn(uint64_t IRP, uint32_t Status);
  llvm::Error tryFinalizeDriverIRP(uint64_t IRP);
  struct ActiveRequest {
    DriverRequestKind Kind;
    size_t ResultIndex;
    uint64_t IRP = 0;
    uint64_t Device = 0, FileAddress = 0;
    uint64_t PnpDevice = 0;
    std::optional<DeviceLifecycleTicket> PnpTicket;
    std::optional<DriverPnpOperation> PnpOperation;
    uint64_t RawResources = 0;
    uint64_t TranslatedResources = 0;
    uint64_t ResourceListSize = 0;
    std::optional<DeviceLifecycleTicket> PowerTicket;
    std::optional<DriverPowerOperation> PowerOperation;
    std::optional<DriverBusCompletion> FileBusCompletion;
    bool FileBusReceived = false;
    std::optional<RequestedPower> ChildPower;
    bool LifecycleIo = false;
    uint64_t Stack = 0;
    uint8_t StackCount = 1;
    /// Immutable route holds outlive packet completion until dispatch returns.
    std::vector<uint64_t> DeviceRoute;
    /// Historical pending bits captured when each slot was unwound.
    std::vector<std::optional<bool>> UnwoundPending;
    bool Forwarded = false;
    uint64_t SystemBuffer = 0;
    /// Internal query producers borrow caller storage through completion.
    bool BorrowedSystemBuffer = false;
    uint64_t FileRequestID = 0;
    uint64_t FileOperation = 0;
    bool FileSystemReceived = false;
    uint64_t UserBuffer = 0;
    uint64_t UserInput = 0;
    std::vector<UserRegion> UserRegions;
    uint64_t BufferSize = 0;
    uint64_t SecurityContext = 0;
    uint32_t OutputSize = 0;
    bool Completed = false;
    bool DispatchReturned = false;
    bool FrameworkRemoveStarted = false;
    bool FrameworkTransitionAwaiting = false;
    bool FrameworkTransitionBeforeBus = false;
    bool FrameworkTransitionHandled = false;
    bool FrameworkPolicyIssued = false;
    bool PendingMarked = false;
    bool CancelRequested = false;
    std::optional<uint64_t> CancelDeadline = std::nullopt;
    uint32_t FileId = 0;
    bool AsynchronousFile = false;
    uint32_t ProcessID = 0;
    uint32_t InputSize = 0;
    /// Effective process of an object-manager file lifecycle callback. The
    /// invoking thread's creating process and PreviousMode stay unchanged.
    std::optional<uint32_t> CallbackProcessID;
    uint32_t TransferSize = 0;
    uint64_t ByteOffset = 0;
    bool Direct = false;
    bool Neither = false;
    uint64_t Mdl = 0;
    uint64_t SystemMdl = 0;
    std::array<bool, 16> IOStatusWritten{};
    std::shared_ptr<RAMInitializationWatch> IOStatusInitialization;
  };
  std::map<uint64_t, ActiveRequest> Requests;
  bool canDeliverCancellation(const ActiveRequest &Request) const;
  llvm::Expected<std::optional<KernelScheduler::Callback>>
  planWDMCancellation(uint64_t IRP, const ActiveRequest &Request) const;
  llvm::Error
  validatePnpRemovalFinalization(const ActiveRequest &Request) const;
  std::set<uint64_t> FinalizedRequests;
  enum class IRPCallKind {
    Dispatch,
    Completion,
    PowerDispatch,
    PowerCompletion,
    Cancel,
    FileClose,
    FileHandleClose,
    FileRequest,
    FileOperation
  };
  struct IRPCall {
    IRPCallKind Kind;
    uint64_t IRP = 0;
    uint32_t Slot = 0;
    bool AwaitingCallback = false;
    uint64_t ReturnValue = 0;
    std::optional<uint64_t> ProviderReceiptIRP;
    std::optional<uint32_t> FrameworkDispatchStatus;
    uint64_t FileHandleOperation = 0;
    uint64_t FileRequestID = 0;
    uint64_t FileOperation = 0;
  };
  struct ProviderReceipt {
    uint64_t Device = 0;
    uint64_t Deadline = 0;
    std::vector<UsbIdleCompletionPlan> Idle;
    size_t NextIdle = 0;
    std::optional<uint64_t> ScheduledDispatchToken;
  };
  std::map<uint64_t, ProviderReceipt> ProviderReceipts;
  llvm::Expected<std::vector<UsbIdleCompletionPlan>>
  planUsbIdleReceipt(uint64_t PDO, uint64_t IRP) const;
  llvm::Expected<std::optional<uint64_t>> continueProviderReceipt(uint64_t IRP);
  llvm::Expected<uint64_t> completeProviderReceipt(uint64_t PDO, uint64_t IRP,
                                                   uint64_t Deadline);
  llvm::Expected<std::optional<uint64_t>>
  finishWdmGuestCallBody(uint64_t Token, uint64_t ResultValue);
  uint64_t NextIRPCall = 1;
  std::map<uint64_t, IRPCall> IRPCalls;
  std::optional<KernelGuestCall> PendingWdmCall;
  llvm::Expected<uint32_t> requestStackCursor(uint64_t IRP) const;
  llvm::Expected<uint64_t> currentRequestStack(uint64_t IRP) const;
  /// Only the framework host may forward a framework-owned file IRP. Guest
  /// WDM dispatch still requires independent ownership of the packet.
  enum class ForwardingOwner {
    WDM,
    FrameworkFile,
    FrameworkFileSynchronous,
    FrameworkFileAsynchronous,
    FrameworkFileAutomatic
  };
  llvm::Expected<uint64_t>
  callDriver(uint64_t Device, uint64_t IRP,
             ForwardingOwner Owner = ForwardingOwner::WDM,
             std::optional<int64_t> SendTimeout = std::nullopt);
  struct IRPCompletionStep {
    uint32_t Slot;
    bool Pending;
  };
  struct IRPCompletionPlan {
    uint32_t StartSlot = 0;
    std::vector<IRPCompletionStep> Steps;
    uint64_t PC = 0, Device = 0, Context = 0;
    bool PowerCompletion = false;
    std::vector<uint64_t> Arguments;
  };
  llvm::Expected<IRPCompletionPlan>
  planIRPCompletion(uint64_t IRP,
                    std::optional<uint32_t> StatusOverride = std::nullopt,
                    std::optional<bool> CancelOverride = std::nullopt) const;
  struct ProviderCompletion {
    enum class Kind {
      WDM,
      FrameworkCallback,
      FrameworkSynchronous,
      FrameworkAutomatic
    };
    uint64_t Device = 0, Deadline = 0, Sequence = 0;
    uint32_t Status = 0;
    Kind Owner = Kind::WDM;
  };
  uint64_t NextProviderSequence = 1;
  std::map<uint64_t, ProviderCompletion> ProviderCompletions;
  llvm::Expected<uint64_t>
  callProviderDriver(uint64_t Device, uint64_t IRP,
                     ForwardingOwner Owner = ForwardingOwner::WDM,
                     std::optional<int64_t> SendTimeout = std::nullopt);
  llvm::Error processProviderCompletions();
  llvm::Expected<std::optional<uint64_t>> advanceIRPCompletion(uint64_t Token);
  llvm::Expected<bool> dispatchPending(const ActiveRequest &Request,
                                       uint32_t Slot) const;
  llvm::Error validateCompletionPending(const ActiveRequest &Request,
                                        bool Pending) const;
  llvm::Error retireCompletedRequest(uint64_t IRP, uint8_t PriorityBoost);
  ActiveRequest *requestForIRP(uint64_t IRP);
  const ActiveRequest *requestForIRP(uint64_t IRP) const;
  void configureFrameworkRequestHost();
  llvm::Error markRequestPending(uint64_t IRP);
  llvm::Error prepareRequestBuffers(ActiveRequest &Record,
                                    const DriverRequest &Input);
  llvm::Error prepareUserRequestBuffers(ActiveRequest &Request,
                                        const DriverRequest &Input);
  llvm::Error snapshotUserBuffers();
  llvm::Expected<Invocation> beginPnpRequest(const DriverRequest &Input,
                                             size_t ResultIndex);
  llvm::Expected<Invocation> beginPowerRequest(const DriverRequest &Input,
                                               size_t ResultIndex);
  /// Prepare without calling the guest. A generated child uses the next result
  /// index and publishes its observation only after successful preparation.
  llvm::Expected<Invocation>
  preparePowerRequest(const DriverRequest &Input, size_t ResultIndex,
                      std::optional<RequestedPower> Child = std::nullopt);
  llvm::Expected<PowerRequestPlan>
  planPowerRequest(const DriverRequest &Input, size_t ResultIndex,
                   const std::optional<RequestedPower> &Child,
                   PowerRequestDelivery Delivery) const;
  llvm::Expected<Invocation>
  commitPowerRequest(const DriverRequest &Input, size_t ResultIndex,
                     std::optional<RequestedPower> Child,
                     PowerRequestPlan Plan);
  llvm::Expected<uint32_t> dispatchPreparedPowerRequest(const Invocation &Call);
  llvm::Expected<std::optional<KernelScheduler::Invocation>>
  dispatchScheduledPowerProvider(const KernelScheduler::Invocation &Call);
  llvm::Expected<bool> beginFrameworkPowerPolicy(uint64_t IRP);
  llvm::Expected<bool> canAllocatePowerRequest(const PowerRequestPlan &Plan,
                                               bool Callback) const;
  llvm::Expected<uint64_t> requestPowerIrp(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> setPowerState(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error startNextPowerIrp(uint64_t IRP);
  llvm::Error tryFinalizePowerRequest(uint64_t IRP);
  llvm::Expected<std::optional<uint64_t>> finishPowerCompletion(uint64_t Token);
  llvm::Error validatePowerRequestCompletion(const ActiveRequest &Request,
                                             uint32_t Status) const;
  llvm::Error finishRequestLifecycle(ActiveRequest &Request, uint32_t Status);
  llvm::Error initializePnpResources(ActiveRequest &Request);
  llvm::Error validatePnpRequestCompletion(const ActiveRequest &Request,
                                           uint32_t Status,
                                           bool ProviderProbe = false) const;
  llvm::Error publishProviderHardware(ActiveRequest &Request, uint32_t Status);
  llvm::Error initializeRequestPacket(ActiveRequest &Record,
                                      const DriverRequest &Input);
  struct LockedMdl {
    // Descriptor storage is independent of the described pages and their lock.
    enum class StorageKind { PrivateArena, NonPagedPool, ExecutionStack };
    StorageKind Storage = StorageKind::PrivateArena;
    uint64_t StorageOwner = 0;
    enum class Ownership {
      Request,
      RequestSystemBuffer,
      Driver,
      NonPagedPool,
      UserLocked,
      KernelLocked,
      AllocatedPages,
      ReleasedPages,
      Partial
    };
    Ownership Owner = Ownership::Request;
    uint64_t OwnerIRP = 0;
    uint64_t Address = 0;
    uint64_t Size = 0;
    uint64_t Buffer = 0;
    uint64_t OriginalAddress = 0;
    uint64_t BackingAddress = 0;
    uint64_t LockOwnerMDL = 0;
    uint64_t SystemMappingOwnerMDL = 0;
    bool OwnsSystemMapping = false;
    uint64_t MappingReservation = 0;
    uint64_t AllocationSize = 0;
    uint64_t UserAddress = 0;
    uint32_t ByteCount = 0;
    struct AdvanceRange {
      uint64_t OriginalAddress, BackingAddress, MappingBase;
      uint32_t ByteCount, DroppedPages;
    };
    // Advancing the data window does not retire the original pin or alias.
    std::optional<AdvanceRange> Advance;
    uint64_t systemMappingBase() const;
    uint64_t Pool = 0;
    uint64_t Pin = 0;
    bool Writable = false;
    bool DmaWritable = false;
    bool Mapped = false;
    bool isProbeLocked() const {
      return Owner == Ownership::UserLocked || Owner == Ownership::KernelLocked;
    }
  };
  std::map<uint64_t, LockedMdl> MDLs;
  struct ReservedMapping {
    uint64_t Size;
    uint32_t Tag;
    uint64_t MDL = 0;
  };
  std::map<uint64_t, ReservedMapping> ReservedMappings;
  uint64_t findSystemMappingAddress(uint64_t Size) const;
  llvm::Expected<uint64_t> allocateMappingAddress(uint64_t Size, uint32_t Tag);
  llvm::Expected<uint64_t> freeMappingAddress(uint64_t Address, uint32_t Tag);
  llvm::Expected<uint64_t> mapReservedMapping(llvm::ArrayRef<uint64_t> A);
  llvm::Expected<uint64_t> unmapReservedMapping(llvm::ArrayRef<uint64_t> A);
  llvm::Error initializeMDLPhysicalPages(const LockedMdl &State);
  llvm::Expected<uint64_t> allocateMDL(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> createMDL(uint64_t Record, uint64_t Address,
                                     uint64_t Size);
  llvm::Error initializeMDLRecord(uint64_t Record, uint64_t Address,
                                  uint32_t Size, uint64_t Extent,
                                  uint16_t Flags);
  llvm::Error canReleaseMDLStorage(uint64_t Base, uint64_t Size) const;
  void retireMDLStorage(uint64_t Base, uint64_t Size);
  llvm::Expected<uint64_t>
  allocatePagesForMDL(llvm::ArrayRef<uint64_t> Arguments, bool Extended);
  llvm::Error freePagesFromMDL(uint64_t MDL);
  llvm::Error freeAllocatedMDL(uint64_t MDL);
  static llvm::Expected<KernelPhysicalMemory::CacheType>
  memoryCacheType(uint32_t Value);
  llvm::Error buildNonPagedMDL(uint64_t MDL);
  llvm::Error buildPartialMDL(uint64_t Source, uint64_t Target,
                              uint64_t Address, uint32_t Length);
  llvm::Error canReleaseMDLDependencies(uint64_t MDL,
                                        llvm::ArrayRef<uint64_t> Retiring = {},
                                        bool ReleasePages = true) const;
  llvm::Error probeAndLockPages(uint64_t MDL, uint32_t Mode,
                                uint32_t Operation);
  llvm::Error unlockPages(uint64_t MDL);
  llvm::Expected<uint64_t> advanceMDL(uint64_t MDL, uint32_t Bytes);
  llvm::Error restoreAdvancedMDL(LockedMdl &State);
  llvm::Expected<uint64_t> protectMDLSystemAddress(uint64_t MDL,
                                                   uint32_t Protection);
  llvm::Error freeMDL(uint64_t MDL);
  llvm::Expected<uint64_t> createMDLRecord(uint64_t Address, uint32_t Size,
                                           uint16_t Flags);
  llvm::Expected<uint64_t> frameworkRequestMDL(uint64_t IRP, bool Output);
  llvm::Expected<KernelFramework::LockedUserBuffer>
  frameworkProbeAndLockUserBuffer(uint64_t IRP, uint64_t Buffer,
                                  uint64_t Length, bool ForWrite);
  llvm::Error releaseFrameworkUserBuffer(uint64_t MDL);
  llvm::Expected<uint64_t> createRequestMDL(uint64_t IRP, uint32_t Size,
                                            llvm::ArrayRef<uint8_t> Initial,
                                            bool Writable, uint64_t UserAddress,
                                            bool DmaWritable);
  llvm::Expected<uint64_t>
  mapLockedPages(uint64_t MDL, uint32_t Priority, bool ReuseExisting,
                 KernelPhysicalMemory::CacheType Cache =
                     KernelPhysicalMemory::CacheType::Cached);
  llvm::Expected<uint64_t> mapUserMDL(uint64_t MDL, uint64_t RequestedAddress,
                                      uint32_t Priority,
                                      KernelPhysicalMemory::CacheType Cache);
  llvm::Error unmapUserMDL(uint64_t Address, uint64_t MDL);
  llvm::Error canReleaseMdlUserViews(uint64_t MDL) const;
  llvm::Error canReleaseUserViewsForBacking(uint64_t Address,
                                            uint64_t Size) const;
  llvm::Error validateUserMdlViewAccess(uint64_t Address, uint64_t Size,
                                        bool IsWrite) const;
  struct UserMemoryRange {
    uint64_t Owner;
    uint64_t Offset;
    uint64_t Backing;
  };
  llvm::Expected<UserMemoryRange> resolveUserMemoryRange(uint64_t Address,
                                                         uint64_t Length,
                                                         bool ForWrite) const;
  llvm::Error unmapLockedPages(uint64_t Address, uint64_t MDL,
                               bool Reserved = false);
  llvm::Error validateMDLAccess(uint64_t Address, uint32_t Size,
                                bool IsWrite) const;
  llvm::Expected<std::vector<uint64_t>> requestMDLChain(uint64_t IRP) const;
  llvm::Expected<std::vector<std::pair<uint64_t, uint64_t>>>
  requestReleaseRanges(uint64_t IRP) const;
  llvm::Error appendRequestMDLReleaseResources(
      uint64_t IRP, std::vector<std::pair<uint64_t, uint64_t>> &Ranges,
      std::vector<uint64_t> &Pins,
      std::vector<std::pair<uint64_t, uint64_t>> &RevokingAliases) const;
  llvm::Error expireRequestMDL(uint64_t IRP);
  llvm::Expected<std::vector<uint8_t>> readMDLBytes(uint64_t MDL,
                                                    uint32_t Count);
  llvm::Error completeRequest(uint64_t IRP, uint8_t PriorityBoost);
  llvm::Error validateRequestCompletion(uint64_t IRP, uint32_t Status,
                                        uint64_t Information) const;
  llvm::Error validateIOAccess(uint64_t Address, uint32_t Size, bool IsWrite,
                               bool DispatcherStorage = false) const;
  llvm::Expected<uint64_t> allocate(uint64_t Size, uint64_t Alignment = 16);
  llvm::Expected<uint64_t> isResidentKernelAddress(uint64_t Address) const;
  llvm::Expected<uint64_t> queryPhysicalAddress(uint64_t Address);
  llvm::Expected<uint64_t> queryPrimaryVirtualAddress(uint64_t PhysicalAddress);
  llvm::Expected<uint64_t> allocateInArena(uint64_t Size, uint64_t Alignment,
                                           uint64_t &Cursor, uint64_t End);
  llvm::Expected<uint64_t> planArenaAllocation(uint64_t Size,
                                               uint64_t Alignment,
                                               uint64_t Cursor,
                                               uint64_t End) const;
  llvm::Expected<uint64_t> allocatePhysicalBuffer(uint64_t AllocationSize,
                                                  uint64_t Alignment,
                                                  uint64_t DataOffset,
                                                  uint64_t DataSize,
                                                  bool PagedArena = false);
  llvm::Expected<uint64_t> makeUnicodeString(const std::string &Text);
  llvm::Expected<std::string>
  readObjectName(uint64_t Address,
                 size_t MaxCharacters = profile::MaxDeviceNameSize);
  llvm::Expected<std::string> readNativeObjectName(uint64_t Address,
                                                   bool Unicode = false);
  llvm::Expected<uint64_t> createDevice(llvm::ArrayRef<uint64_t> Arguments);
  struct DeviceCreation {
    uint32_t Status;
    uint64_t Address;
  };
  llvm::Expected<DeviceCreation>
  createDeviceObject(llvm::StringRef Name, uint32_t ExtensionSize,
                     uint32_t Type, uint32_t Characteristics, bool Exclusive);
  llvm::Expected<DeviceCreation>
  createDeviceObjectForOwner(llvm::StringRef Name, uint32_t ExtensionSize,
                             uint32_t Type, uint32_t Characteristics,
                             bool Exclusive, uint64_t Owner,
                             DeviceOwnerKind OwnerKind);
  llvm::Expected<uint32_t> createSymbolicLink(llvm::StringRef Name,
                                              llvm::StringRef Target);
  llvm::Expected<uint32_t> deleteSymbolicLink(llvm::StringRef Name);
  llvm::Expected<std::string> linkKey(llvm::StringRef Name) const;
  llvm::Expected<uint64_t>
  resolveDeviceName(llvm::StringRef Name, bool CaseInsensitive = false) const;
  llvm::Expected<std::string> resolveObjectPath(llvm::StringRef Name,
                                                bool CaseInsensitive,
                                                bool UnicodeTail = false) const;
  llvm::Error deleteDevice(uint64_t Address);
  llvm::Expected<uint64_t> shutdownNotification(uint64_t Device, bool Register);
  llvm::Error clearShutdownNotifications(uint64_t Device);
  llvm::Error retireDeviceIfUnreferenced(uint64_t Address);
  llvm::Expected<uint64_t> topAttachedDevice(uint64_t Base) const;
  enum class DeviceStackQuery {
    Top,
    TopReference,
    LowerReference,
    BaseReference
  };
  llvm::Expected<uint64_t> queryDeviceStack(uint64_t Device,
                                            DeviceStackQuery Query);
  llvm::Expected<std::vector<uint64_t>> deviceStack(uint64_t Top) const;
  enum class ObjectRead { Execution, Snapshot };
  llvm::Error validateFileSystemDevice(uint64_t Address, ObjectRead Mode) const;
  llvm::Expected<uint64_t> readObjectField(uint64_t Address, unsigned Width,
                                           ObjectRead Mode) const;
  llvm::Error
  validateDeviceTopology(ObjectRead Mode = ObjectRead::Execution) const;
  llvm::Expected<std::vector<uint64_t>>
  driverDeviceInventory(ObjectRead Mode = ObjectRead::Execution) const;
  llvm::Expected<uint64_t> attachDevice(uint64_t Source, uint64_t Target);
  llvm::Error detachDevice(uint64_t Lower);
  llvm::Error retainDevice(uint64_t Device);
  llvm::Error releaseDevice(uint64_t Device);
  llvm::Error validateDeviceStackMutation(llvm::ArrayRef<uint64_t> Stack) const;
};
} // namespace neverd::emulation
#endif
