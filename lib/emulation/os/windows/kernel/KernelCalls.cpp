//===- KernelCalls.cpp - Typed kernel API dispatch ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "KernelAPIIRQL.h"
#include "KernelAPIKind.h"
#include "KernelException.h"
#include "KernelModel.h"
#include "KernelModelRuntime.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
using namespace windows;
struct KernelAPIContract {
  llvm::StringLiteral Name;
  KernelAPIKind Kind;
  unsigned Arity;
};

const KernelAPIContract *lookupKernelAPI(llvm::StringRef Name) {
  static constexpr KernelAPIContract APIs[] = {
#define NEVERD_KERNEL_API(Symbol, Arity, Availability)                         \
  {#Symbol, KernelAPIKind::Symbol, Arity},
#include "KernelAPIs.def"
#undef NEVERD_KERNEL_API
  };
  for (const auto &API : APIs)
    if (Name == API.Name)
      return &API;
  return nullptr;
}
llvm::Error modelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

std::optional<unsigned> KernelModel::argumentCount(const std::string &Name) {
  if (const auto *API = lookupKernelAPI(Name))
    return API->Arity;
  return std::nullopt;
}

llvm::Expected<uint64_t> KernelModel::call(
    const std::string &Name, llvm::ArrayRef<uint64_t> A,
    llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument) {
  const auto *API = lookupKernelAPI(Name);
  if (!API || A.size() != API->Arity)
    return modelError("unknown API or incorrect argument count: " + Name);
  const auto Kind = API->Kind;
  // Only these contracts have effects entirely accounted for by captured
  // bytes, pool ownership and the export registry. Every other call retains
  // an explicit dependency until its kernel lifetime has a recovery contract.
  switch (Kind) {
  case KernelAPIKind::MmGetSystemRoutineAddress:
  case KernelAPIKind::RtlInitUnicodeString:
  case KernelAPIKind::RtlCopyMemory:
  case KernelAPIKind::RtlMoveMemory:
  case KernelAPIKind::RtlZeroMemory:
  case KernelAPIKind::RtlFillMemory:
  case KernelAPIKind::RtlCompareMemory:
  case KernelAPIKind::ExAllocatePool:
  case KernelAPIKind::ExAllocatePoolWithTag:
  case KernelAPIKind::ExAllocatePool2:
  case KernelAPIKind::NtQuerySystemInformation:
  case KernelAPIKind::ZwQuerySystemInformation:
  case KernelAPIKind::ExFreePool:
  case KernelAPIKind::ExFreePoolWithTag:
    break;
  default:
    UnpackOpaqueEffects = true;
    break;
  }
  if (!DriverObject)
    return modelError("kernel model has not been initialized");
  auto MaximumIRQL = maximumKernelIRQL(Name);
  if (!MaximumIRQL)
    return MaximumIRQL.takeError();
  if (CurrentIRQL > *MaximumIRQL) {
    if (*MaximumIRQL == scheduler::PassiveLevel)
      return modelError(Name + " requires IRQL PASSIVE_LEVEL");
    return modelError(Name +
                      " requires IRQL <= " + std::to_string(*MaximumIRQL));
  }
  if (auto Context = executionProcessContext();
      Context && Context->Attached &&
      Kind != KernelAPIKind::KeStackAttachProcess &&
      Kind != KernelAPIKind::KeUnstackDetachProcess &&
      Kind != KernelAPIKind::IoGetCurrentProcess &&
      Kind != KernelAPIKind::IoGetRequestorProcess &&
      Kind != KernelAPIKind::PsGetCurrentProcessId &&
      Kind != KernelAPIKind::PsGetProcessId &&
      Kind != KernelAPIKind::IoGetRequestorProcessId &&
      Kind != KernelAPIKind::ExGetPreviousMode &&
      Kind != KernelAPIKind::KeGetCurrentIrql &&
      Kind != KernelAPIKind::ProbeForRead &&
      Kind != KernelAPIKind::ProbeForWrite &&
      Kind != KernelAPIKind::MmProbeAndLockPages &&
      Kind != KernelAPIKind::MmMapLockedPagesSpecifyCache &&
      Kind != KernelAPIKind::MmGetSystemAddressForMdlSafe &&
      Kind != KernelAPIKind::MmUnmapLockedPages &&
      Kind != KernelAPIKind::RtlCopyMemory &&
      Kind != KernelAPIKind::RtlMoveMemory)
    return modelError("process attachment permits only bounded user-memory "
                      "operations before detach");
  if ((Kind == KernelAPIKind::KeInitializeDpc ||
       Kind == KernelAPIKind::KeInitializeEvent ||
       Kind == KernelAPIKind::KeInitializeSemaphore ||
       Kind == KernelAPIKind::KeInitializeMutex ||
       Kind == KernelAPIKind::KeInitializeTimer ||
       Kind == KernelAPIKind::KeInitializeTimerEx) &&
      WaitReferences.count(A[0]))
    return modelError("cannot reinitialize an object with outstanding waits");
  switch (Kind) {
  case KernelAPIKind::ExRaiseStatus:
    return llvm::make_error<KernelGuestException>(uint32_t(A[0]));
  case KernelAPIKind::ExRaiseAccessViolation:
    return llvm::make_error<KernelGuestException>(
        exceptions::StatusAccessViolation);
  case KernelAPIKind::ExRaiseDatatypeMisalignment:
    return llvm::make_error<KernelGuestException>(
        exceptions::StatusDatatypeMisalignment);
  case KernelAPIKind::ProbeForRead:
  case KernelAPIKind::ProbeForWrite:
    return probeUserBuffer(A[0], A[1], uint32_t(A[2]),
                           Kind == KernelAPIKind::ProbeForWrite);
  case KernelAPIKind::ExGetPreviousMode: {
    auto Context = executionProcessContext();
    return uint64_t(Context ? Context->PreviousMode : KernelMode);
  }
  case KernelAPIKind::NtQuerySystemInformation:
  case KernelAPIKind::ZwQuerySystemInformation:
    return querySystemInformation(
        A, Kind == KernelAPIKind::ZwQuerySystemInformation);
  case KernelAPIKind::PsGetCurrentProcessId:
    if (auto Context = executionProcessContext())
      return uint64_t(Context->CreatingProcessID);
    return modelError("PsGetCurrentProcessId requires a modeled foreground "
                      "or system thread");
  case KernelAPIKind::PsCreateSystemThread:
    return createSystemThread(A);
  case KernelAPIKind::PsTerminateSystemThread:
    return terminateSystemThread(uint32_t(A[0]));
  case KernelAPIKind::ObReferenceObjectByHandle:
    return referenceThreadByHandle(A);
  case KernelAPIKind::ObfDereferenceObject:
    return dereferenceThread(A[0]);
  case KernelAPIKind::IoGetRequestorProcessId: {
    const auto *Request = requestForIRP(A[0]);
    if (!Request || Request->Completed)
      return modelError("IoGetRequestorProcessId requires a live IRP");
    return uint64_t(Request->ProcessID);
  }
  case KernelAPIKind::IoGetRequestorProcess:
    return requestorProcess(A[0]);
  case KernelAPIKind::IoGetCurrentProcess:
    return currentProcess();
  case KernelAPIKind::PsGetProcessId: {
    auto Process = ProcessObjectIDs.find(A[0]);
    if (Process == ProcessObjectIDs.end())
      return modelError("PsGetProcessId requires a known process object");
    return uint64_t(Process->second);
  }
  case KernelAPIKind::KeStackAttachProcess:
    if (auto E = stackAttachProcess(A[0], A[1]))
      return E;
    return 0;
  case KernelAPIKind::KeUnstackDetachProcess:
    if (auto E = unstackDetachProcess(A[0]))
      return E;
    return 0;
#define NEVERD_KERNEL_EXECUTION_API(Name, Arity, IRQL, Operation)              \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelExecutionAPIs.def"
#undef NEVERD_KERNEL_EXECUTION_API
#define NEVERD_KERNEL_THREAD_API(Name, Arity, IRQL, Operation)                 \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelThreadAPIs.def"
#undef NEVERD_KERNEL_THREAD_API
#define NEVERD_KERNEL_SPINLOCK_API(Name, Arity, IRQL, Operation)               \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelSpinLockAPIs.def"
#undef NEVERD_KERNEL_SPINLOCK_API
#define NEVERD_KERNEL_INTERRUPT_API(Name, Arity, IRQL, Operation)              \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelInterruptAPIs.def"
#undef NEVERD_KERNEL_INTERRUPT_API
#define NEVERD_KERNEL_POFX_API(Name, Arity, IRQL, Operation)                   \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelPoFxAPIs.def"
#undef NEVERD_KERNEL_POFX_API
  case KernelAPIKind::MmMapIoSpace:
  case KernelAPIKind::MmMapIoSpaceEx:
    return MMIO.map(A[0], A[1], uint32_t(A[2]),
                    Kind == KernelAPIKind::MmMapIoSpaceEx);
  case KernelAPIKind::MmUnmapIoSpace:
    if (auto E = MMIO.unmap(A[0], A[1]))
      return E;
    return 0;
  case KernelAPIKind::IoInitializeRemoveLockEx:
    return initializeRemoveLock(A);
  case KernelAPIKind::IoAcquireRemoveLockEx:
    return acquireRemoveLock(A);
  case KernelAPIKind::IoReleaseRemoveLockEx:
    return releaseRemoveLock(A, false);
  case KernelAPIKind::IoReleaseRemoveLockAndWaitEx:
    return releaseRemoveLock(A, true);
#define NEVERD_KERNEL_DISPATCHER_API(Name, Arity) case KernelAPIKind::Name:
#include "KernelDispatcherAPIs.def"
#undef NEVERD_KERNEL_DISPATCHER_API
    return Dispatcher.call(Name, A, CurrentIRQL, CurrentThreadKey);
#define NEVERD_KERNEL_REGISTRY_API(Name, Arity) case KernelAPIKind::Name:
#include "KernelRegistryAPIs.def"
#undef NEVERD_KERNEL_REGISTRY_API
    if (Kind == KernelAPIKind::ZwClose)
      return closeHandle(A[0]);
    return Registry.call(*this, Name, A);
  case KernelAPIKind::MmGetSystemRoutineAddress:
    return resolveRoutine(A[0]);
  case KernelAPIKind::KeWaitForSingleObject:
  case KernelAPIKind::KeDelayExecutionThread:
    return beginWait(A, Kind == KernelAPIKind::KeDelayExecutionThread);
  case KernelAPIKind::KeWaitForMultipleObjects:
    return beginMultipleWait(A);
  case KernelAPIKind::IoAllocateIrp:
    return allocateDriverIRP(A);
  case KernelAPIKind::IoFreeIrp: {
    if (auto E = freeDriverIRP(A[0]))
      return E;
    return 0;
  }
  case KernelAPIKind::IoAllocateWorkItem:
    return allocateWorkItem(A[0]);
  case KernelAPIKind::IoQueueWorkItem: {
    if (auto E = queueWorkItem(A))
      return E;
    return 0;
  }
  case KernelAPIKind::IoFreeWorkItem: {
    if (auto E = freeWorkItem(A[0]))
      return E;
    return 0;
  }
  case KernelAPIKind::IoMarkIrpPending: {
    if (auto E = markRequestPending(A[0]))
      return E;
    return 0;
  }
  case KernelAPIKind::IoSetCancelRoutine:
    return setCancelRoutine(A);
  case KernelAPIKind::IoAcquireCancelSpinLock:
    return acquireCancelSpinLock(A[0]);
  case KernelAPIKind::IoReleaseCancelSpinLock: {
    if (auto E = releaseCancelSpinLock(A[0]))
      return E;
    return 0;
  }
  case KernelAPIKind::IoCancelIrp:
    return cancelIRP(A[0]);
  case KernelAPIKind::IoGetDmaAdapter:
    return getDMAAdapter(A);
  case KernelAPIKind::IoAllocateMdl:
    return allocateMDL(A);
  case KernelAPIKind::IoBuildPartialMdl: {
    if (auto E = buildPartialMDL(A[0], A[1], A[2], uint32_t(A[3])))
      return E;
    return 0;
  }
  case KernelAPIKind::IoFreeMdl: {
    if (auto E = freeMDL(A[0]))
      return E;
    return 0;
  }
  case KernelAPIKind::MmBuildMdlForNonPagedPool: {
    if (auto E = buildNonPagedMDL(A[0]))
      return E;
    return 0;
  }
  case KernelAPIKind::MmProbeAndLockPages: {
    if (auto E = probeAndLockPages(A[0], uint8_t(A[1]), uint32_t(A[2])))
      return E;
    return 0;
  }
  case KernelAPIKind::MmUnlockPages: {
    if (auto E = unlockPages(A[0]))
      return E;
    return 0;
  }
  case KernelAPIKind::KeFlushIoBuffers: {
    if (auto E = flushIoBuffers(A[0]))
      return E;
    return 0;
  }
  case KernelAPIKind::MmAllocatePagesForMdl:
  case KernelAPIKind::MmAllocatePagesForMdlEx:
    return allocatePagesForMDL(A,
                               Kind == KernelAPIKind::MmAllocatePagesForMdlEx);
  case KernelAPIKind::MmFreePagesFromMdl: {
    if (auto E = freePagesFromMDL(A[0]))
      return E;
    return 0;
  }
  case KernelAPIKind::MmMapLockedPagesSpecifyCache:
    return mapMDL(A);
  case KernelAPIKind::MmGetSystemAddressForMdlSafe: {
    return mapLockedPages(A[0], static_cast<uint32_t>(A[1]), true);
  }
  case KernelAPIKind::MmProtectMdlSystemAddress:
    return protectMDLSystemAddress(A[0], uint32_t(A[1]));
  case KernelAPIKind::MmUnmapLockedPages:
    return unmapMDL(A);
  case KernelAPIKind::IoAttachDeviceToDeviceStack:
    return attachDevice(A[0], A[1]);
  case KernelAPIKind::IoDetachDevice: {
    if (auto E = detachDevice(A[0]))
      return E;
    return 0;
  }
  case KernelAPIKind::IofCallDriver:
  case KernelAPIKind::IoCallDriver:
    return callDriver(A[0], A[1]);
  case KernelAPIKind::PoCallDriver:
    return callPowerDriver(A);
  case KernelAPIKind::PoRequestPowerIrp:
    return requestPowerIrp(A);
  case KernelAPIKind::PoSetPowerState:
    return setPowerState(A);
  case KernelAPIKind::PoStartNextPowerIrp: {
    if (auto E = startNextPowerIrp(A[0]))
      return E;
    return 0;
  }
  case KernelAPIKind::IofCompleteRequest:
    return completeDriverRequest(A);
  case KernelAPIKind::IoCompleteRequest:
    return completeDriverRequest(A);
  case KernelAPIKind::IoGetCurrentIrpStackLocation:
    return currentDriverRequestStack(A);
  case KernelAPIKind::RtlInitUnicodeString:
    return initializeUnicodeString(A);
  case KernelAPIKind::RtlCopyUnicodeString:
    return runtime::unicodeOperation(*this, Memory,
                                     runtime::UnicodeOperation::Copy, A);
  case KernelAPIKind::RtlCompareUnicodeString:
    return runtime::unicodeOperation(*this, Memory,
                                     runtime::UnicodeOperation::Compare, A);
  case KernelAPIKind::RtlEqualUnicodeString:
    return runtime::unicodeOperation(*this, Memory,
                                     runtime::UnicodeOperation::Equal, A);
  case KernelAPIKind::ExAllocatePool:
  case KernelAPIKind::ExAllocatePoolWithTag:
    return allocatePool(A, false);
  case KernelAPIKind::ExAllocatePool2:
    return allocatePool(A, true);
  case KernelAPIKind::ExFreePoolWithTag:
    return freePool(A, true);
  case KernelAPIKind::ExFreePool:
    return freePool(A, false);
  case KernelAPIKind::IoCreateDevice:
    return createDevice(A);
  case KernelAPIKind::IoDeleteDevice: {
    if (auto E = deleteDevice(A[0]))
      return E;
    return 0;
  }
  case KernelAPIKind::IoCreateSymbolicLink:
    return createSymbolicLinkFromGuest(A);
  case KernelAPIKind::IoDeleteSymbolicLink:
    return deleteSymbolicLinkFromGuest(A);
  case KernelAPIKind::DbgPrint:
    return debugMessage(A, 0, ReadArgument);
  case KernelAPIKind::DbgPrintEx:
    return debugMessage(A, 2, ReadArgument);
  case KernelAPIKind::KeGetCurrentIrql:
    return CurrentIRQL;
  case KernelAPIKind::memcpy:
  case KernelAPIKind::memmove:
  case KernelAPIKind::memset:
  case KernelAPIKind::memcmp:
  case KernelAPIKind::RtlCopyMemory:
  case KernelAPIKind::RtlMoveMemory:
  case KernelAPIKind::RtlFillMemory:
  case KernelAPIKind::RtlZeroMemory:
  case KernelAPIKind::RtlCompareMemory:
    return memoryCall(Kind, A);
  }
  llvm_unreachable("kernel API must have a handler");
}
} // namespace neverd::emulation
