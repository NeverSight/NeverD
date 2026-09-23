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
#include "../GuestMemory.h"
#include "DeviceLifecycle.h"
#include "KernelDMA.h"
#include "KernelDispatcher.h"
#include "KernelFramework.h"
#include "KernelGuestCall.h"
#include "KernelInterrupts.h"
#include "KernelMMIO.h"
#include "KernelRegistry.h"
#include "KernelRemoveLocks.h"
#include "KernelScheduler.h"

#include "neverd/emulation/DriverSession.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <map>
#include <optional>
#include <set>
namespace neverd::emulation {
struct DriverImage;
class KernelExportRegistry;
class KernelModel {
public:
  KernelModel(GuestMemory &Memory, DriverResult &Result,
              KernelExportRegistry *Exports = nullptr)
      : Memory(Memory), Result(Result), Exports(Exports), Registry(Memory),
        Dispatcher(Memory, Scheduler,
                   [this](uint64_t Address, uint32_t Size, bool IsWrite) {
                     return validateDispatcherStorage(Address, Size, IsWrite);
                   }),
        Resources([this](uint64_t PDO) { return canReleaseResources(PDO); },
                  [this](uint64_t PDO) {
                    // DMA adapter acquisition in AddDevice precedes START.
                    return llvm::joinErrors(MMIO.canRemove(PDO),
                                            Interrupts.canRelease(PDO));
                  }),
        MMIO(Memory, Resources), Interrupts(Resources, Result),
        Physical(Memory), DMA(Physical, Resources, Result) {}
  KernelModel(const KernelModel &) = delete;
  KernelModel &operator=(const KernelModel &) = delete;
  KernelModel(KernelModel &&) = delete;
  KernelModel &operator=(KernelModel &&) = delete;
  llvm::Error initialize(const DriverImage &Image,
                         const DriverOptions &Options);
  /// End a normally returned DriverEntry, regardless of its NTSTATUS. The
  /// borrowed RegistryPath record and buffer expire before any later callback.
  llvm::Error finishEntry();
  uint64_t driverObject() const { return DriverObject; }
  uint64_t registryPath() const { return RegistryPath; }
  /// Fixed kernel argument counts; unknown names have no execution contract.
  static std::optional<unsigned> argumentCount(const std::string &Name);
  static std::optional<unsigned>
  argumentCount(const KernelExportRegistry::Export &Export);
  llvm::Expected<uint64_t>
  call(const KernelExportRegistry::Export &Export,
       llvm::ArrayRef<uint64_t> Arguments,
       llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument);
  std::optional<KernelGuestCall> takeGuestCall();
  llvm::Error beginGuestCall(GuestCallToken Token);
  llvm::Expected<std::optional<uint64_t>> finishGuestCall(GuestCallToken Token,
                                                          uint64_t Result);
  llvm::Expected<uint64_t>
  call(const std::string &Name, llvm::ArrayRef<uint64_t> Arguments,
       llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument =
           nullptr);
  llvm::Error snapshot();
  struct Invocation {
    uint64_t PC = 0;
    uint64_t Argument0 = 0;
    uint64_t Argument1 = 0;
    uint64_t Argument2 = 0;
    uint64_t Argument3 = 0;
    std::vector<uint64_t> StackArguments;
    std::optional<uint32_t> FrameworkDispatchStatus;
    uint64_t IRP = 0;
  };
  // PnP device enrollment: configuration identities never expose addresses.
  llvm::Error preparePnpDevices();
  llvm::Expected<Invocation> beginAddDevice(llvm::StringRef ID);
  llvm::Error finishAddDevice(llvm::StringRef ID, uint32_t Status);
  /// A pending dispatch retains its packet until a guest callback completes it.
  llvm::Expected<Invocation>
  beginRequest(const DriverRequest &Request,
               std::optional<size_t> SourceIndex = std::nullopt);
  llvm::Error recordDispatchReturn(uint64_t IRP, uint32_t DispatchStatus);
  /// Finalization is idempotent only for an already finalized owned IRP.
  llvm::Error finalizeRequest(uint64_t IRP);
  /// IRP=0 inspects all requests for the scheduler's drain boundary.
  bool requestPending(uint64_t IRP = 0) const;
  llvm::Expected<std::optional<KernelScheduler::Invocation>>
  nextScheduled(bool AdvanceTime,
                std::optional<uint64_t> Deadline = std::nullopt);
  llvm::Error finishScheduled(uint64_t ID);
  std::optional<uint32_t> takeThreadTermination();
  /// Finish a scheduled framework callback, including any destruction callbacks
  /// required before releasing its scheduler ownership.
  llvm::Expected<std::optional<KernelGuestCall>>
  continueScheduled(uint64_t ID, uint64_t ReturnValue);
  llvm::Error suspendScheduled(uint64_t ID);
  llvm::Error resumeScheduled(uint64_t ID);
  struct Wait {
    enum class Kind { Dispatcher, Thread, Delay, RemoveLock };
    Kind Type = Kind::Dispatcher;
    uint64_t Object = 0;
    uint64_t Execution = 0;
    uint8_t IRQL = 0;
    std::optional<uint64_t> Deadline;
  };
  std::optional<Wait> takeWait();
  llvm::Expected<std::optional<uint32_t>> pollWait(const Wait &Pending);
  std::optional<uint64_t> nextEventTime() const;
  bool hasQueuedDPC() const { return Scheduler.hasQueuedDPC(); }
  bool hasQueuedPriorityCallback() const {
    const auto Interrupt = nextInterruptEventTime();
    const auto Transfer = DMA.nextEventTime();
    return (Interrupt && *Interrupt <= Scheduler.now100ns()) ||
           (Transfer && *Transfer <= Scheduler.now100ns()) ||
           Scheduler.hasQueuedInterrupt() || hasQueuedDPC() ||
           Scheduler.hasQueuedDMACallback() ||
           Scheduler.hasQueuedCancellation() ||
           Scheduler.hasQueuedWDMCompletion();
  }
  llvm::Error activateStack(uint64_t Base, uint64_t Size);
  llvm::Error retireStack(uint64_t Base, uint64_t Size);
  void enterForeground() { CurrentIRQL = 0; }
  void enterExecution(uint64_t Identity) {
    CurrentExecution = Identity;
    if (CancelLock.Held && CancelLock.Callback && !CancelLock.Owner) {
      CancelLock.Owner = Identity;
      CancelLock.CallbackExecution = Identity;
    }
  }
  llvm::Error setUserRequestContext(
      bool Active,
      uint32_t ProcessID = DriverRequest::DefaultRequestorProcessID);
  llvm::Error revokeRequestUserBuffers(uint64_t IRP);
  llvm::Error exitRequestorProcess(uint64_t IRP);
  bool canCatchUserAccess(uint64_t Address, uint64_t Size) const;
  llvm::Error validateExecutionReturn(uint64_t Identity, uint8_t EntryIRQL) const;
  bool hasPendingInterruptEvents() const { return Interrupts.hasPendingEvents(); }
  bool hasPendingHardwareWork() const {
    return Interrupts.hasPendingEvents() || DMA.hasPendingEvents() ||
           DMA.hasPendingCallbacks();
  }
  uint8_t currentIRQL() const { return CurrentIRQL; }
  llvm::Expected<Invocation> beginUnload();
  llvm::Error finishUnload();
  /// Reject CPU accesses whose environment semantics this profile does not own.
  llvm::Error validateGuestAccess(uint64_t Address, uint32_t Size,
                                  bool IsWrite) const;

private:
  GuestMemory &Memory;
  DriverResult &Result;
  KernelExportRegistry *Exports;
  std::unique_ptr<KernelFramework> Framework;
  std::optional<KernelGuestCall> takeWdmGuestCall();
  llvm::Expected<std::optional<uint64_t>> finishWdmGuestCall(uint64_t Token,
                                                             uint64_t Result);
  void configureFrameworkDeviceHost();
  /// Framework-owned WDM devices and their canonical symbolic-link keys.
  std::map<uint64_t, std::vector<std::string>> FrameworkDevices;
  KernelRegistry Registry;
  KernelScheduler Scheduler;
  KernelDispatcher Dispatcher;
  KernelRemoveLocks RemoveLocks;
  KernelResources Resources;
  KernelMMIO MMIO;
  KernelInterrupts Interrupts;
  KernelPhysicalMemory Physical;
  KernelDMA DMA;
  std::optional<KernelGuestCall> PendingDMACall;
  std::set<uint64_t> InlineDMACalls;
  bool hasPendingModelGuestCall() const {
    return PendingWdmCall || PendingInterruptCall || PendingDMACall ||
           (Framework && Framework->hasPendingGuestCall());
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
  bool UserRequestContext = false;
  uint32_t CurrentUserProcessID = 0;
  uint64_t NextUserAddress = profile::UserArenaBase;
  uint64_t NextUserAlias = profile::UserAliasBase;
  struct UserAllocation {
    uint64_t Size;
    uint32_t ProcessID;
    DriverUserPageAccess Access;
  };
  std::map<uint64_t, UserAllocation> UserAllocations;
  std::set<uint64_t> RevokedUserAllocations;
  std::set<uint32_t> ExitedUserProcesses;
  std::map<uint32_t, uint64_t> ProcessObjects;
  std::map<uint64_t, uint32_t> ProcessObjectIDs;
  struct ProcessAttachment {
    uint64_t Execution;
    uint64_t ApcState;
    uint32_t PreviousProcessID;
    bool PreviousUserContext;
  };
  std::vector<ProcessAttachment> ProcessAttachments;
  struct ExecutiveSpinLock {
    uint64_t Execution;
    uint8_t OldIRQL;
    bool RaisedIRQL;
  };
  std::map<uint64_t, ExecutiveSpinLock> ExecutiveSpinLocks;
  llvm::Expected<uint64_t> callSpinLockAPI(llvm::StringRef Name,
                                           llvm::ArrayRef<uint64_t> Arguments);
  struct RaisedIRQL {
    uint64_t Execution;
    uint8_t OldIRQL;
    uint8_t NewIRQL;
  };
  std::vector<RaisedIRQL> RaisedIRQLs;
  llvm::Expected<uint64_t> callIRQLAPI(llvm::StringRef Name,
                                       llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> processObject(uint32_t ProcessID);
  llvm::Expected<uint64_t> requestorProcess(uint64_t IRP);
  llvm::Expected<uint64_t> currentProcess();
  llvm::Error stackAttachProcess(uint64_t Process, uint64_t ApcState);
  llvm::Error unstackDetachProcess(uint64_t ApcState);
  llvm::Expected<uint64_t> allocateUserBuffer(uint32_t Size,
                                              llvm::ArrayRef<uint8_t> Initial,
                                              DriverUserPageAccess Access,
                                              uint32_t ProcessID);
  llvm::Expected<uint64_t> probeUserBuffer(uint64_t Address, uint64_t Size,
                                            uint32_t Alignment, bool ForWrite);
  std::optional<KernelGuestCall> PendingInterruptCall;
  llvm::Expected<uint64_t> callInterruptAPI(llvm::StringRef Name,
                                            llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<std::optional<uint64_t>>
  finishInterruptCall(uint64_t Token, uint64_t Value);
  llvm::Expected<uint64_t> preflightScheduledBoundary(uint64_t Time);
  llvm::Error processInterruptEvents();
  std::optional<uint64_t> nextInterruptEventTime() const {
    return Interrupts.nextEventTime();
  }
  llvm::Error canReleaseResources(uint64_t PDO) const;
  std::optional<Wait> PendingWait;
  std::map<uint64_t, size_t> WaitReferences;
  struct SystemThread {
    uint64_t Handle = 0;
    uint64_t CallbackID = 0;
    uint32_t ExitStatus = 0;
    size_t PointerReferences = 0;
    bool HandleOpen = true;
    bool Exited = false;
    bool Terminating = false;
  };
  std::map<uint64_t, SystemThread> SystemThreads;
  std::map<uint64_t, uint64_t> ThreadHandles;
  uint64_t NextThreadHandle = 0x60000000;
  std::optional<uint32_t> PendingThreadTermination;
  llvm::Expected<uint64_t>
  createSystemThread(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> terminateSystemThread(uint32_t Status);
  llvm::Expected<uint64_t>
  referenceThreadByHandle(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Expected<uint64_t> dereferenceThread(uint64_t Object);
  llvm::Expected<uint64_t> closeHandle(uint64_t Handle);
  void retireThreadIfUnreferenced(uint64_t Object);
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
                              uint64_t IgnoredDMAPin = 0) const;
  llvm::Expected<uint64_t> beginWait(llvm::ArrayRef<uint64_t> Arguments,
                                     bool Delay);
  llvm::Error canRevokeVirtualRange(uint64_t Base, uint64_t Size) const;
  llvm::Error prepareRevokeVirtualRange(uint64_t Base, uint64_t Size);
  llvm::Error prepareReleaseRange(uint64_t Base, uint64_t Size,
                                  uint64_t IgnoredDMAPin = 0);
  llvm::Error
  prepareReleaseRanges(llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Ranges,
                       uint64_t IgnoredDMAPin = 0);
  llvm::Error validateGuestAccessImpl(uint64_t Address, uint32_t Size,
                                      bool IsWrite,
                                      bool IncludeDispatcher) const;
  llvm::Error validateDispatcherStorage(uint64_t Address, uint32_t Size,
                                        bool IsWrite) const;
  uint8_t CurrentIRQL = 0;
  struct CancelSpinLockState {
    bool Held = false;
    bool Callback = false;
    uint64_t Owner = 0;
    uint64_t CallbackExecution = 0;
    uint64_t IRP = 0;
    uint8_t OldIRQL = 0;
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
  uint64_t DriverObject = 0;
  uint64_t RegistryPath = 0;
  uint64_t DriverExtension = 0;
  bool EntryFinished = false;
  uint64_t NextAllocation = 0;
  uint64_t AllocationEnd = 0;
  struct PoolAllocation {
    uint64_t Size;
    uint32_t Tag;
    bool NonPaged;
  };
  std::map<uint64_t, PoolAllocation> Allocations;
  std::map<uint64_t, uint64_t> ArenaAllocations;
  enum class DeviceOwnerKind { Guest, Provider };
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
    bool DeletePending = false;
    std::optional<DevicePowerState> ReportedDevicePower;
  };
  std::map<uint64_t, DeviceRecord> Devices;
  // PnP provider identities persist after the concrete PDO is retired.
  struct PnpDeviceRecord {
    uint64_t PDO = 0;
    size_t ResultIndex = 0;
    DriverBusKind Bus = DriverBusKind::ResourceFree;
    std::optional<uint32_t> AddDeviceStatus;
    bool AddDeviceActive = false;
    std::set<uint64_t> ExistingGuestDevices;
    std::set<uint64_t> GuestDevices;
    std::optional<DevicePowerState> InitialReportedDevicePower;
    std::vector<DriverPowerOperation> RequestedDevicePower;
    uint32_t RequestedPowerIndex = 0;
  };
  uint64_t PnpProviderDriver = 0;
  bool PnpDevicesPrepared = false;
  std::vector<DriverPnpDevice> ConfiguredPnpDevices;
  std::map<std::string, PnpDeviceRecord> PnpDevices;
  DeviceLifecycle Lifecycle;
  bool isProviderDevice(uint64_t Address) const;
  PnpDeviceRecord *pnpDeviceForPDO(uint64_t PDO);
  const PnpDeviceRecord *pnpDeviceForPDO(uint64_t PDO) const;
  llvm::Expected<uint64_t> pnpDeviceForRoute(uint64_t Device) const;
  llvm::Error finishPnpRemoval(uint64_t PDO);
  llvm::Error retirePnpProvider(uint64_t PDO);
  llvm::Error snapshotPnpDevices();
  std::map<uint64_t, uint64_t> FreedRanges;
  mutable std::array<bool, 28 * 8> DispatchBytesWritten{};
  std::map<std::string, std::string> SymbolicLinks;
  enum class FileState { Closed, Opening, Open, Cleaned };
  struct OpenFile {
    uint64_t Address = 0;
    uint64_t Device = 0;
    /// Stable lifecycle owner even after the opened FDO is detached.
    uint64_t PnpDevice = 0;
    bool Asynchronous = false;
    FileState State = FileState::Opening;
  };
  std::map<uint32_t, OpenFile> Files;
  bool Unloading = false;
  bool Unloaded = false;
  struct RequestedPower {
    uint64_t RequestDevice = 0;
    uint64_t Callback = 0;
    uint64_t Context = 0;
    // A separate callback input snapshot; the completed IRP stays retired.
    uint64_t StatusBlock = 0;
    uint32_t ResponseIndex = 0;
    bool CallbackStarted = false;
    bool CallbackReturned = false;
  };
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
    uint64_t UserBuffer = 0;
    uint64_t UserInput = 0;
    uint64_t BufferSize = 0;
    uint64_t SecurityContext = 0;
    uint32_t OutputSize = 0;
    bool Completed = false;
    bool DispatchReturned = false;
    bool PendingMarked = false;
    bool CancelRequested = false;
    std::optional<uint64_t> CancelDeadline = std::nullopt;
    uint32_t FileId = 0;
    bool AsynchronousFile = false;
    uint32_t ProcessID = 0;
    uint32_t InputSize = 0;
    uint32_t TransferSize = 0;
    uint64_t ByteOffset = 0;
    bool Direct = false;
    bool Neither = false;
    uint64_t Mdl = 0;
    uint64_t SystemMdl = 0;
    mutable std::array<bool, 16> IOStatusWritten{};
  };
  std::map<uint64_t, ActiveRequest> Requests;
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
    Cancel
  };
  struct IRPCall {
    IRPCallKind Kind;
    uint64_t IRP = 0;
    uint32_t Slot = 0;
    bool AwaitingCallback = false;
    uint64_t ReturnValue = 0;
  };
  uint64_t NextIRPCall = 1;
  std::map<uint64_t, IRPCall> IRPCalls;
  std::optional<KernelGuestCall> PendingWdmCall;
  llvm::Expected<uint32_t> requestStackCursor(uint64_t IRP) const;
  llvm::Expected<uint64_t> currentRequestStack(uint64_t IRP) const;
  llvm::Expected<uint64_t> callDriver(uint64_t Device, uint64_t IRP);
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
  llvm::Expected<IRPCompletionPlan> planIRPCompletion(
      uint64_t IRP,
      std::optional<uint32_t> StatusOverride = std::nullopt) const;
  struct ProviderCompletion {
    uint64_t Device = 0, Deadline = 0, Sequence = 0;
    uint32_t Status = 0;
  };
  uint64_t NextProviderSequence = 1;
  std::map<uint64_t, ProviderCompletion> ProviderCompletions;
  llvm::Expected<uint64_t> callProviderDriver(uint64_t Device, uint64_t IRP);
  llvm::Error processProviderCompletions();
  llvm::Expected<std::optional<uint64_t>> advanceIRPCompletion(uint64_t Token);
  llvm::Expected<bool> dispatchPending(const ActiveRequest &Request,
                                       uint32_t Slot) const;
  llvm::Error retireCompletedRequest(uint64_t IRP, uint8_t PriorityBoost);
  ActiveRequest *requestForIRP(uint64_t IRP);
  const ActiveRequest *requestForIRP(uint64_t IRP) const;
  void configureFrameworkRequestHost();
  llvm::Error markRequestPending(uint64_t IRP);
  llvm::Error prepareRequestBuffers(ActiveRequest &Record,
                                    const DriverRequest &Input);
  llvm::Expected<Invocation> beginPnpRequest(const DriverRequest &Input,
                                             size_t ResultIndex);
  llvm::Expected<Invocation> beginPowerRequest(const DriverRequest &Input,
                                               size_t ResultIndex);
  /// Prepare without calling the guest. A generated child uses the next result
  /// index and publishes its observation only after successful preparation.
  llvm::Expected<Invocation>
  preparePowerRequest(const DriverRequest &Input, size_t ResultIndex,
                      std::optional<RequestedPower> Child = std::nullopt);
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
    enum class Ownership {
      Request,
      RequestSystemBuffer,
      Driver,
      NonPagedPool,
      UserLocked
    };
    Ownership Owner = Ownership::Request;
    uint64_t OwnerIRP = 0;
    uint64_t Address = 0;
    uint64_t Size = 0;
    uint64_t Buffer = 0;
    uint64_t AllocationSize = 0;
    uint64_t UserAddress = 0;
    uint32_t ByteCount = 0;
    uint64_t Pool = 0;
    uint64_t Pin = 0;
    bool Writable = false;
    bool DmaWritable = false;
    bool Mapped = false;
    bool MappingWritable = false;
  };
  std::map<uint64_t, LockedMdl> MDLs;
  llvm::Error initializeMDLPhysicalPages(const LockedMdl &State);
  llvm::Expected<uint64_t> allocateMDL(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error buildNonPagedMDL(uint64_t MDL);
  llvm::Error probeAndLockPages(uint64_t MDL, uint32_t Mode,
                                 uint32_t Operation);
  llvm::Error unlockPages(uint64_t MDL);
  llvm::Error freeMDL(uint64_t MDL);
  llvm::Expected<uint64_t> createMDLRecord(uint64_t Address, uint32_t Size,
                                           uint16_t Flags);
  llvm::Expected<uint64_t> frameworkRequestMDL(uint64_t IRP, bool Output);
  llvm::Expected<uint64_t> createRequestMDL(uint64_t IRP, uint32_t Size,
                                            llvm::ArrayRef<uint8_t> Initial,
                                            bool Writable, uint64_t UserAddress,
                                            bool DmaWritable);
  llvm::Expected<uint64_t> mapLockedPages(uint64_t MDL, uint32_t Priority,
                                          bool ReuseExisting);
  llvm::Error unmapLockedPages(uint64_t Address, uint64_t MDL);
  llvm::Error validateMDLAccess(uint64_t Address, uint32_t Size,
                                bool IsWrite) const;
  llvm::Error expireRequestMDL(uint64_t IRP);
  llvm::Expected<std::vector<uint8_t>> readMDLBytes(uint64_t MDL,
                                                    uint32_t Count);
  llvm::Error completeRequest(uint64_t IRP, uint8_t PriorityBoost);
  llvm::Error validateRequestCompletion(uint64_t IRP, uint32_t Status,
                                        uint64_t Information) const;
  llvm::Error validateIOAccess(uint64_t Address, uint32_t Size,
                               bool IsWrite) const;
  llvm::Expected<uint64_t> allocate(uint64_t Size, uint64_t Alignment = 16);
  llvm::Expected<uint64_t> allocatePhysicalBuffer(uint64_t AllocationSize,
                                                  uint64_t Alignment,
                                                  uint64_t DataOffset,
                                                  uint64_t DataSize);
  llvm::Expected<uint64_t> makeUnicodeString(const std::string &Text);
  llvm::Expected<std::string> readObjectName(uint64_t Address);
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
  llvm::Expected<uint64_t> resolveDeviceName(llvm::StringRef Name) const;
  llvm::Error deleteDevice(uint64_t Address);
  llvm::Error retireDeviceIfUnreferenced(uint64_t Address);
  llvm::Expected<uint64_t> topAttachedDevice(uint64_t Base) const;
  llvm::Expected<std::vector<uint64_t>> deviceStack(uint64_t Top) const;
  llvm::Error validateDeviceTopology() const;
  llvm::Expected<std::vector<uint64_t>> driverDeviceInventory() const;
  llvm::Expected<uint64_t> attachDevice(uint64_t Source, uint64_t Target);
  llvm::Error detachDevice(uint64_t Lower);
  llvm::Error retainDevice(uint64_t Device);
  llvm::Error releaseDevice(uint64_t Device);
  llvm::Error validateDeviceStackMutation(llvm::ArrayRef<uint64_t> Stack) const;
};
} // namespace neverd::emulation
#endif
