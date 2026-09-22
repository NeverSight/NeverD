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
#include "KernelDispatcher.h"
#include "KernelFramework.h"
#include "KernelRegistry.h"
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
                   }) {}
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
  std::optional<KernelFramework::GuestCall> takeGuestCall();
  llvm::Expected<std::optional<uint64_t>> finishGuestCall(uint64_t Token,
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
  /// A pending dispatch retains its packet until a guest callback completes it.
  llvm::Expected<Invocation> beginRequest(const DriverRequest &Request);
  llvm::Error recordDispatchReturn(uint64_t IRP, uint32_t DispatchStatus);
  /// Finalization is idempotent only for an already finalized owned IRP.
  llvm::Error finalizeRequest(uint64_t IRP);
  /// IRP=0 inspects all requests for the scheduler's drain boundary.
  bool requestPending(uint64_t IRP = 0) const;
  llvm::Expected<std::optional<KernelScheduler::Invocation>>
  nextScheduled(bool AdvanceTime,
                std::optional<uint64_t> Deadline = std::nullopt);
  llvm::Error finishScheduled(uint64_t ID);
  /// Finish a scheduled framework callback, including any destruction callbacks
  /// required before releasing its scheduler ownership.
  llvm::Expected<std::optional<KernelFramework::GuestCall>>
  continueScheduled(uint64_t ID, uint64_t ReturnValue);
  llvm::Error suspendScheduled(uint64_t ID);
  llvm::Error resumeScheduled(uint64_t ID);
  struct Wait {
    uint64_t Object = 0;
    std::optional<uint64_t> Deadline;
  };
  std::optional<Wait> takeWait();
  llvm::Expected<std::optional<uint32_t>> pollWait(const Wait &Pending);
  std::optional<uint64_t> nextEventTime() const;
  bool hasQueuedDPC() const { return Scheduler.hasQueuedDPC(); }
  bool hasQueuedPriorityCallback() const {
    return hasQueuedDPC() || Scheduler.hasQueuedFrameworkCancel();
  }
  llvm::Error activateStack(uint64_t Base, uint64_t Size);
  llvm::Error retireStack(uint64_t Base, uint64_t Size);
  void enterForeground() { CurrentIRQL = 0; }
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
  void configureFrameworkDeviceHost();
  /// Framework-owned WDM devices and their canonical symbolic-link keys.
  std::map<uint64_t, std::vector<std::string>> FrameworkDevices;
  KernelRegistry Registry;
  KernelScheduler Scheduler;
  KernelDispatcher Dispatcher;
  std::optional<Wait> PendingWait;
  std::map<uint64_t, size_t> WaitReferences;
  llvm::Expected<uint64_t> beginWait(llvm::ArrayRef<uint64_t> Arguments,
                                     bool Delay);
  llvm::Error prepareReleaseRange(uint64_t Base, uint64_t Size);
  llvm::Error
  prepareReleaseRanges(llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Ranges);
  llvm::Error validateGuestAccessImpl(uint64_t Address, uint32_t Size,
                                      bool IsWrite,
                                      bool IncludeDispatcher) const;
  llvm::Error validateDispatcherStorage(uint64_t Address, uint32_t Size,
                                        bool IsWrite) const;
  uint8_t CurrentIRQL = 0;
  std::map<uint64_t, uint64_t> WorkItems;
  std::map<uint64_t, uint64_t> WorkReferences;
  std::map<uint64_t, uint64_t> ScheduledCancelContinuations;
  llvm::Error processRequestCancellations();
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
  std::map<uint64_t, DriverDevice> Devices;
  std::set<uint64_t> DeletePendingDevices;
  std::map<uint64_t, uint64_t> DeviceSizes;
  std::map<uint64_t, uint64_t> FreedRanges;
  mutable std::array<bool, 28 * 8> DispatchBytesWritten{};
  std::map<std::string, std::string> SymbolicLinks;
  enum class FileState { Closed, Opening, Open, Cleaned };
  struct OpenFile {
    uint64_t Address = 0;
    uint64_t Device = 0;
    FileState State = FileState::Opening;
  };
  std::map<uint32_t, OpenFile> Files;
  bool Unloading = false;
  bool Unloaded = false;
  struct ActiveRequest {
    DriverRequestKind Kind;
    size_t ResultIndex;
    uint64_t IRP = 0;
    uint64_t Device = 0, FileAddress = 0;
    uint64_t Stack = 0;
    uint64_t SystemBuffer = 0;
    uint64_t UserBuffer = 0;
    uint64_t BufferSize = 0;
    uint64_t SecurityContext = 0;
    uint32_t OutputSize = 0;
    bool Completed = false;
    bool DispatchReturned = false;
    bool PendingMarked = false;
    bool CancelRequested = false;
    std::optional<uint64_t> CancelDeadline = std::nullopt;
    uint32_t FileId = 0;
    uint32_t InputSize = 0;
    uint32_t TransferSize = 0;
    uint64_t ByteOffset = 0;
    bool Direct = false;
    uint64_t Mdl = 0;
    uint64_t SystemMdl = 0;
    mutable std::array<bool, 16> IOStatusWritten{};
  };
  std::map<uint64_t, ActiveRequest> Requests;
  std::set<uint64_t> FinalizedRequests;
  ActiveRequest *requestForIRP(uint64_t IRP);
  const ActiveRequest *requestForIRP(uint64_t IRP) const;
  void configureFrameworkRequestHost();
  llvm::Error markRequestPending(uint64_t IRP);
  llvm::Error prepareRequestBuffers(ActiveRequest &Record,
                                    const DriverRequest &Input,
                                    uint64_t Device);
  llvm::Error initializeRequestPacket(ActiveRequest &Record,
                                      const DriverRequest &Input);
  struct LockedMdl {
    enum class Ownership { Request, RequestSystemBuffer, Driver, NonPagedPool };
    Ownership Owner = Ownership::Request;
    uint64_t OwnerIRP = 0;
    uint64_t Address = 0;
    uint64_t Size = 0;
    uint64_t Buffer = 0;
    uint64_t AllocationSize = 0;
    uint64_t UserAddress = 0;
    uint32_t ByteCount = 0;
    uint64_t Pool = 0;
    bool Writable = false;
    bool Mapped = false;
    bool MappingWritable = false;
  };
  std::map<uint64_t, LockedMdl> MDLs;
  llvm::Expected<uint64_t> allocateMDL(llvm::ArrayRef<uint64_t> Arguments);
  llvm::Error buildNonPagedMDL(uint64_t MDL);
  llvm::Error freeMDL(uint64_t MDL);
  llvm::Expected<uint64_t> createMDLRecord(uint64_t Address, uint32_t Size,
                                           uint16_t Flags);
  llvm::Expected<uint64_t> frameworkRequestMDL(uint64_t IRP, bool Output);
  llvm::Expected<uint64_t> createRequestMDL(uint64_t IRP, uint32_t Size,
                                            llvm::ArrayRef<uint8_t> Initial,
                                            bool Writable,
                                            uint64_t UserAddress);
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
  llvm::Expected<uint32_t> createSymbolicLink(llvm::StringRef Name,
                                              llvm::StringRef Target);
  llvm::Expected<uint32_t> deleteSymbolicLink(llvm::StringRef Name);
  llvm::Expected<std::string> linkKey(llvm::StringRef Name) const;
  llvm::Expected<uint64_t> resolveDeviceName(llvm::StringRef Name) const;
  llvm::Error deleteDevice(uint64_t Address);
  llvm::Error retireDeviceIfUnreferenced(uint64_t Address);
};
} // namespace neverd::emulation
#endif
