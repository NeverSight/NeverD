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
    llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument,
    const KernelContextCapture *Context) {
  auto Value = dispatchKernelCall(Name, A, ReadArgument, Context);
  if (!Value)
    return Value;
  if (auto E = deliverFileRequestCompletions(Context))
    return E;
  return Value;
}

llvm::Expected<uint64_t> KernelModel::dispatchKernelCall(
    const std::string &Name, llvm::ArrayRef<uint64_t> A,
    llvm::function_ref<llvm::Expected<uint64_t>(unsigned)> ReadArgument,
    const KernelContextCapture *Context) {
  APICallDetail.clear();
  if (auto E = validateServiceCallContext(Name))
    return E;
  const auto *API = lookupKernelAPI(Name);
  if (!API || A.size() != API->Arity)
    return modelError("unknown API or incorrect argument count: " + Name);
  const auto Kind = API->Kind;
  // Only these contracts have effects entirely accounted for by captured
  // bytes, resource lifetimes and the export registry. Every other call retains
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
#define NEVERD_KERNEL_RUNTIME_STRING_API(Name, Arity, IRQL, Operation)         \
  case KernelAPIKind::Name:
#include "KernelRuntimeStringAPIs.def"
#undef NEVERD_KERNEL_RUNTIME_STRING_API
#define NEVERD_KERNEL_RUNTIME_TIME_API(Name, Arity, IRQL, Operation)           \
  case KernelAPIKind::Name:
#include "KernelRuntimeTimeAPIs.def"
#undef NEVERD_KERNEL_RUNTIME_TIME_API
#define NEVERD_KERNEL_INTERLOCKED_API(Name, Arity, IRQL, Operation)            \
  case KernelAPIKind::Name:
#include "KernelInterlockedAPIs.def"
#undef NEVERD_KERNEL_INTERLOCKED_API
#define NEVERD_KERNEL_CSTRING_API(Name, Arity, IRQL, Operation)                \
  case KernelAPIKind::Name:
#include "KernelCStringAPIs.def"
#undef NEVERD_KERNEL_CSTRING_API
#define NEVERD_KERNEL_RUNDOWN_API(Name, Arity, IRQL, Operation)                \
  case KernelAPIKind::Name:
#include "KernelRundownAPIs.def"
#undef NEVERD_KERNEL_RUNDOWN_API
    break;
  case KernelAPIKind::IoAllocateMdl:
  case KernelAPIKind::IoFreeMdl:
  case KernelAPIKind::MmProbeAndLockPages:
  case KernelAPIKind::MmUnlockPages:
  case KernelAPIKind::MmGetSystemAddressForMdlSafe:
  case KernelAPIKind::MmMapLockedPagesSpecifyCache:
  case KernelAPIKind::MmMapLockedPages:
  case KernelAPIKind::MmUnmapLockedPages:
  case KernelAPIKind::MmProtectMdlSystemAddress:
    // Complete image pages already belong to the recovered file. Their
    // temporary metadata, locks and aliases must all retire before capture;
    // reads of model physical identities retain a separate dependency.
    if (!unpackImageMDLCall(Kind, A))
      UnpackOpaqueEffects = true;
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
      Context && Context->RestrictedAttachment &&
      Kind != KernelAPIKind::KeStackAttachProcess &&
      Kind != KernelAPIKind::KeUnstackDetachProcess &&
      Kind != KernelAPIKind::IoGetCurrentProcess &&
      Kind != KernelAPIKind::PsGetCurrentProcess &&
      Kind != KernelAPIKind::PsGetCurrentThread &&
      Kind != KernelAPIKind::KeGetCurrentThread &&
      Kind != KernelAPIKind::PsGetCurrentThreadId &&
      Kind != KernelAPIKind::PsGetThreadId &&
      Kind != KernelAPIKind::IoGetRequestorProcess &&
      Kind != KernelAPIKind::PsGetCurrentProcessId &&
      Kind != KernelAPIKind::PsGetProcessId &&
      Kind != KernelAPIKind::PsGetThreadProcess &&
      Kind != KernelAPIKind::IoThreadToProcess &&
      Kind != KernelAPIKind::IoGetRequestorProcessId &&
      Kind != KernelAPIKind::ExGetPreviousMode &&
      Kind != KernelAPIKind::PsGetCurrentThreadPreviousMode &&
      Kind != KernelAPIKind::RtlPcToFileHeader &&
      Kind != KernelAPIKind::KeGetCurrentIrql &&
      Kind != KernelAPIKind::KeGetCurrentProcessorNumberEx &&
      Kind != KernelAPIKind::ProbeForRead &&
      Kind != KernelAPIKind::ProbeForWrite &&
      Kind != KernelAPIKind::MmProbeAndLockPages &&
      Kind != KernelAPIKind::MmProbeAndLockProcessPages &&
      Kind != KernelAPIKind::MmMapLockedPagesSpecifyCache &&
      Kind != KernelAPIKind::MmMapLockedPages &&
      Kind != KernelAPIKind::MmGetSystemAddressForMdlSafe &&
      Kind != KernelAPIKind::MmGetPhysicalAddress &&
      Kind != KernelAPIKind::MmGetVirtualForPhysical &&
      Kind != KernelAPIKind::IoGetInitialStack &&
      Kind != KernelAPIKind::IoGetStackLimits &&
      Kind != KernelAPIKind::MmUnmapLockedPages &&
      Kind != KernelAPIKind::RtlCopyMemory &&
      Kind != KernelAPIKind::RtlMoveMemory &&
      Kind != KernelAPIKind::PsIsThreadTerminating &&
      Kind != KernelAPIKind::wcsrchr && Kind != KernelAPIKind::RtlUpperChar &&
      Kind != KernelAPIKind::RtlAnsiCharToUnicodeChar &&
      Kind != KernelAPIKind::PsGetThreadProcessId &&
      Kind != KernelAPIKind::RtlInt64ToUnicodeString &&
      Kind != KernelAPIKind::RtlIntegerToUnicodeString &&
      !(Version &&
        (Kind == KernelAPIKind::PsGetProcessSectionBaseAddress ||
         Kind == KernelAPIKind::PsGetProcessExitStatus ||
         Kind == KernelAPIKind::PsGetProcessPeb ||
         Kind == KernelAPIKind::PsGetProcessImageFileName ||
         Kind == KernelAPIKind::PsGetThreadProcessId ||
         Kind == KernelAPIKind::PsIsThreadTerminating ||
         Kind == KernelAPIKind::MmIsAddressValid ||
         Kind == KernelAPIKind::memcpy || Kind == KernelAPIKind::memmove ||
         Kind == KernelAPIKind::memset || Kind == KernelAPIKind::memcmp ||
         Kind == KernelAPIKind::RtlUpperChar ||
         Kind == KernelAPIKind::RtlInt64ToUnicodeString ||
         Kind == KernelAPIKind::RtlIntegerToUnicodeString ||
         Kind == KernelAPIKind::ZwQueryVirtualMemory ||
         Kind == KernelAPIKind::ZwMapViewOfSection ||
         Kind == KernelAPIKind::ZwUnmapViewOfSection ||
         Kind == KernelAPIKind::ZwClose ||
         Kind == KernelAPIKind::IoAllocateMdl ||
         Kind == KernelAPIKind::IoFreeMdl ||
         Kind == KernelAPIKind::MmUnlockPages ||
         Kind == KernelAPIKind::ObfDereferenceObject)))
    return modelError(Name +
                      ": process attachment permits only bounded user-memory "
                      "operations before detach");
  if ((Kind == KernelAPIKind::KeInitializeDpc ||
       Kind == KernelAPIKind::KeInitializeEvent ||
       Kind == KernelAPIKind::KeInitializeSemaphore ||
       Kind == KernelAPIKind::KeInitializeMutex ||
       Kind == KernelAPIKind::KeInitializeTimer ||
       Kind == KernelAPIKind::KeInitializeTimerEx) &&
      WaitReferences.count(A[0]))
    return modelError("cannot reinitialize an object with outstanding waits");
  if ((Kind == KernelAPIKind::ZwReadFile ||
       Kind == KernelAPIKind::ZwWriteFile ||
       Kind == KernelAPIKind::ZwQueryInformationFile ||
       Kind == KernelAPIKind::ZwSetInformationFile ||
       Kind == KernelAPIKind::ObReferenceObjectByHandle) &&
      SectionHandles.contains(A[0])) {
    const auto &H = SectionHandles.at(A[0]);
    return canAccessHandle(H.Attributes, H.ProcessID) ? StatusObjectTypeMismatch
                                                      : StatusInvalidHandle;
  }
  if (FiniteFileSystem &&
      Kind == KernelAPIKind::IoGetBaseFileSystemDeviceObject)
    return finiteFileSystemDevice(A[0]);
  if (FiniteFileSystem) {
    switch (Kind) {
#define NEVERD_KERNEL_FINITE_FILE_API(Symbol, Arity) case KernelAPIKind::Symbol:
#include "KernelFiniteFileAPIs.def"
#undef NEVERD_KERNEL_FINITE_FILE_API
      return fileCall(Name, A);
    default:
      break;
    }
  }
  switch (Kind) {
#define NEVERD_KERNEL_SECTION_API(Name, Arity, IRQL, Operation)                \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelSectionAPIs.def"
#undef NEVERD_KERNEL_SECTION_API
  case KernelAPIKind::IoCreateFileSpecifyDeviceObjectHint:
  case KernelAPIKind::ZwDeleteFile:
    return modelError(
        "this file API requires an explicit finite_filesystem provider");
#define NEVERD_KERNEL_AVL_API(Symbol, Arity)                                   \
  case KernelAPIKind::Symbol:                                                  \
    return callAvl(#Symbol, A);
#include "KernelAvlAPIs.def"
#undef NEVERD_KERNEL_AVL_API
#define NEVERD_KERNEL_SERVICE_API(Symbol)                                      \
  case KernelAPIKind::Symbol:                                                  \
    return callDriverService(#Symbol, A[0]);
#include "KernelServiceAPIs.def"
#undef NEVERD_KERNEL_SERVICE_API
  case KernelAPIKind::KeBugCheck:
  case KernelAPIKind::KeBugCheckEx: {
    DriverBugCheck Request;
    Request.Code = uint32_t(A[0]);
    if (Kind == KernelAPIKind::KeBugCheckEx)
      Request.Parameters = std::array<uint64_t, 4>{A[1], A[2], A[3], A[4]};
    return llvm::make_error<KernelGuestBugCheck>(Request);
  }
  case KernelAPIKind::KeQueryActiveProcessorCountEx:
    return uint64_t(uint16_t(A[0]) == 0xffff ||
                            uint16_t(A[0]) < processor::GroupCount
                        ? processor::ActiveCount
                        : 0);
  case KernelAPIKind::KeGetProcessorNumberFromIndex:
    return processorNumber(A[1], true, uint32_t(A[0]));
  case KernelAPIKind::PsSetCreateThreadNotifyRoutine:
    return setThreadNotify(A[0], false);
  case KernelAPIKind::PsRemoveCreateThreadNotifyRoutine:
    return setThreadNotify(A[0], true);
  case KernelAPIKind::ZwQueryVirtualMemory:
  case KernelAPIKind::NtQueryVirtualMemory:
    return queryVirtualMemory(A, Kind == KernelAPIKind::ZwQueryVirtualMemory);
  case KernelAPIKind::VslGetSecurePciEnabled:
    return querySecurePciEnabled();
#define NEVERD_KERNEL_CLOCK_API(Name, Arity, IRQL, Operation)                  \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelClockAPIs.def"
#undef NEVERD_KERNEL_CLOCK_API
#define NEVERD_KERNEL_HAL_API(Name, Arity, IRQL, Operation)                    \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelHALAPIs.def"
#undef NEVERD_KERNEL_HAL_API
#define NEVERD_KERNEL_CALLBACK_API(Name, Arity, IRQL, Operation)               \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelCallbackAPIs.def"
#undef NEVERD_KERNEL_CALLBACK_API
#define NEVERD_KERNEL_CRT_API(Symbol, Arity, Operation, Scoped)                \
  case KernelAPIKind::Symbol:                                                  \
    if (Scoped && (!Version || Version->Major != 10 || Version->Minor != 0 ||  \
                   Version->Build != 19041))                                   \
      return modelError(                                                       \
          #Symbol " requires the Windows 10.0.19041 kernel CRT profile");      \
    return runtime::crtStringOperation(                                        \
        *this, Memory, runtime::CrtStringOperation::Operation, A);
#include "KernelCrtAPIs.def"
#undef NEVERD_KERNEL_CRT_API
  case KernelAPIKind::KeCapturePersistentThreadState:
    return capturePersistentState(A);
  case KernelAPIKind::ZwQueryInformationThread:
    if (auto H = SectionHandles.find(A[0]); H != SectionHandles.end())
      return canAccessHandle(H->second.Attributes, H->second.ProcessID)
                 ? StatusObjectTypeMismatch
                 : StatusInvalidHandle;
    return queryThreadInformation(A);
  case KernelAPIKind::PsGetProcessImageFileName:
    return processImageFileName(A[0]);
  case KernelAPIKind::PsGetProcessSectionBaseAddress:
    return processSectionBaseAddress(A[0]);
  case KernelAPIKind::PsGetProcessPeb:
    return processPeb(A[0]);
  case KernelAPIKind::IoGetDeviceProperty:
    return queryDeviceProperty(A);
  case KernelAPIKind::PsGetProcessExitStatus:
    return processExitStatus(A[0]);

  case KernelAPIKind::IoBuildDeviceIoControlRequest:
    return buildDeviceIoControlRequest(A);
  case KernelAPIKind::MmAllocateContiguousMemory:
    return allocateContiguousMemory(A[0], A[1]);
  case KernelAPIKind::MmFreeContiguousMemory:
    return freeContiguousMemory(A[0]);
  case KernelAPIKind::MmAllocateMappingAddress:
    return allocateMappingAddress(A[0], uint32_t(A[1]));
  case KernelAPIKind::MmFreeMappingAddress:
    return freeMappingAddress(A[0], uint32_t(A[1]));
  case KernelAPIKind::MmMapLockedPagesWithReservedMapping:
    return mapReservedMapping(A);
  case KernelAPIKind::MmUnmapReservedMapping:
    return unmapReservedMapping(A);
  case KernelAPIKind::RtlUpperString:
    return runtime::upperAnsiString(*this, Memory, A);
  case KernelAPIKind::RtlUpperChar:
    return runtime::upperAnsiByte(uint8_t(A[0]));
  case KernelAPIKind::RtlAppendStringToString:
    return runtime::appendAnsiString(*this, Memory, A);
  case KernelAPIKind::RtlAnsiCharToUnicodeChar:
    return runtime::ansiCharToUnicode(*this, Memory, A[0]);
  case KernelAPIKind::RtlCharToInteger:
    return runtime::charToInteger(*this, Memory, A);
  case KernelAPIKind::RtlxAnsiStringToUnicodeSize:
    return runtime::ansiUnicodeSize(*this, Memory, A[0]);
  case KernelAPIKind::IoRegisterShutdownNotification:
    return shutdownNotification(A[0], true);
  case KernelAPIKind::IoUnregisterShutdownNotification:
    return shutdownNotification(A[0], false);

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
  case KernelAPIKind::ExGetPreviousMode:
  case KernelAPIKind::PsGetCurrentThreadPreviousMode: {
    auto Context = executionProcessContext();
    return uint64_t(Context ? Context->PreviousMode : KernelMode);
  }
  case KernelAPIKind::NtQuerySystemInformation:
  case KernelAPIKind::ZwQuerySystemInformation:
    return querySystemInformation(
        A, Kind == KernelAPIKind::ZwQuerySystemInformation);
  case KernelAPIKind::RtlGetVersion:
    if (!Version && !VersionQuery)
      return modelError("version_query is not declared; supply windows_version "
                        "or version_query");
    return Version ? getVersion(A[0]) : queryVersion(A[0]);
  case KernelAPIKind::RtlPcToFileHeader:
    return pcToFileHeader(A[0], A[1]);
  case KernelAPIKind::RtlCaptureContext:
    return captureCallerContext(A[0], Context);
  case KernelAPIKind::PsGetCurrentProcessId:
    if (auto Context = executionProcessContext())
      return uint64_t(Context->CreatingProcessID);
    return modelError("PsGetCurrentProcessId requires a modeled foreground "
                      "or system thread");
  case KernelAPIKind::PsCreateSystemThread:
    return createSystemThread(A);
  case KernelAPIKind::PsTerminateSystemThread:
    return terminateSystemThread(uint32_t(A[0]));
#define NEVERD_KERNEL_OBJECT_API(Name, Arity, IRQL, Operation)                 \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelNamespaceAPIs.def"
#include "KernelObjectAPIs.def"
#undef NEVERD_KERNEL_OBJECT_API
  case KernelAPIKind::IoGetRequestorProcessId: {
    const auto *Request = requestForIRP(A[0]);
    if (!Request || Request->Completed)
      return modelError("IoGetRequestorProcessId requires a live IRP");
    return uint64_t(Request->ProcessID);
  }
  case KernelAPIKind::IoGetRequestorProcess:
    return requestorProcess(A[0]);
  case KernelAPIKind::IoGetCurrentProcess:
  case KernelAPIKind::PsGetCurrentProcess:
    return currentProcess();
  case KernelAPIKind::PsGetCurrentThread:
  case KernelAPIKind::KeGetCurrentThread:
    // The same borrowed identity the inline GS:[0x188] read observes.
    return currentThreadObject();
  case KernelAPIKind::PsGetCurrentThreadId: {
    auto Thread = currentThreadObject();
    if (!Thread)
      return Thread.takeError();
    return threadClientID(*Thread);
  }
  case KernelAPIKind::PsGetThreadId:
    return threadClientID(A[0]);
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
#define NEVERD_KERNEL_RUNTIME_STRING_API(Name, Arity, IRQL, Operation)         \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelRuntimeStringAPIs.def"
#undef NEVERD_KERNEL_RUNTIME_STRING_API
#define NEVERD_KERNEL_RUNTIME_TIME_API(Name, Arity, IRQL, Operation)           \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelRuntimeTimeAPIs.def"
#undef NEVERD_KERNEL_RUNTIME_TIME_API
#define NEVERD_KERNEL_SPINLOCK_API(Name, Arity, IRQL, Operation)               \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelSpinLockAPIs.def"
#undef NEVERD_KERNEL_SPINLOCK_API
#define NEVERD_KERNEL_LOOKASIDE_API(Name, Arity, IRQL, Operation)              \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelLookasideAPIs.def"
#undef NEVERD_KERNEL_LOOKASIDE_API
#define NEVERD_KERNEL_FAST_MUTEX_API(Name, Arity, IRQL, Operation)             \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelFastMutexAPIs.def"
#undef NEVERD_KERNEL_FAST_MUTEX_API
#define NEVERD_KERNEL_RESOURCE_API(Name, Arity, IRQL, Operation)               \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelExecutiveResourceAPIs.def"
#undef NEVERD_KERNEL_RESOURCE_API
#define NEVERD_KERNEL_INTERLOCKED_API(Name, Arity, IRQL, Operation)            \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelInterlockedAPIs.def"
#undef NEVERD_KERNEL_INTERLOCKED_API
#define NEVERD_KERNEL_CSTRING_API(Name, Arity, IRQL, Operation)                \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelCStringAPIs.def"
#undef NEVERD_KERNEL_CSTRING_API
#define NEVERD_KERNEL_RUNDOWN_API(Name, Arity, IRQL, Operation)                \
  case KernelAPIKind::Name:                                                    \
    return Operation;
#include "KernelRundownAPIs.def"
#undef NEVERD_KERNEL_RUNDOWN_API
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
    if (Kind == KernelAPIKind::KeReleaseMutex &&
        !Dispatcher.ownsMutex(CurrentThreadKey, A[0])) {
      const auto State = ApcStates.find(CurrentThreadKey);
      if (auto E = validateExecutiveResourceContext(
              CurrentIRQL,
              State == ApcStates.end() ? 0 : State->second.CriticalDepth,
              State == ApcStates.end() ? 0 : State->second.GuardedDepth, 0, 0,
              true))
        return E;
    }
    return Dispatcher.call(Name, A, CurrentIRQL, CurrentThreadKey);
#define NEVERD_KERNEL_REGISTRY_API(Name, Arity) case KernelAPIKind::Name:
#include "KernelRegistryAPIs.def"
#undef NEVERD_KERNEL_REGISTRY_API
    if (Kind == KernelAPIKind::ZwClose)
      return (RegistryCallback || !RegistryNotifications.empty()) &&
                     Registry.ownsHandle(A[0])
                 ? registryOperation(Name, A)
                 : closeHandle(A[0]);
    return RegistryCallback ? registryOperation(Name, A)
                            : Registry.call(Name, A);
  case KernelAPIKind::MmGetSystemRoutineAddress:
    return resolveRoutine(A[0]);
  case KernelAPIKind::MmGetPhysicalMemoryRanges:
    return queryPhysicalMemoryRanges();
  case KernelAPIKind::MmIsAddressValid:
    return isResidentKernelAddress(A[0]);
  case KernelAPIKind::MmGetPhysicalAddress:
    return queryPhysicalAddress(A[0]);
  case KernelAPIKind::MmGetVirtualForPhysical:
    return queryPrimaryVirtualAddress(A[0]);
  case KernelAPIKind::KeRegisterBugCheckReasonCallback:
    return registerBugCheckCallback(A[0], A[1], uint32_t(A[2]), A[3]);
  case KernelAPIKind::KeDeregisterBugCheckReasonCallback:
    return deregisterBugCheckCallback(A[0]);
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
  case KernelAPIKind::MmCreateMdl:
    return createMDL(A[0], A[1], A[2]);
  case KernelAPIKind::MmAdvanceMdl:
    return advanceMDL(A[0], uint32_t(A[1]));
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
  case KernelAPIKind::MmProbeAndLockProcessPages: {
    if (auto E =
            probeAndLockProcessPages(A[0], A[1], uint8_t(A[2]), uint32_t(A[3])))
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
  case KernelAPIKind::MmMapLockedPages:
    return mapMDL({A[0], A[1], MmCached, 0, 1, HighPagePriority});
  case KernelAPIKind::MmGetSystemAddressForMdlSafe: {
    return mapLockedPages(A[0], static_cast<uint32_t>(A[1]), true);
  }
  case KernelAPIKind::MmProtectMdlSystemAddress:
    return protectMDLSystemAddress(A[0], uint32_t(A[1]));
  case KernelAPIKind::MmUnmapLockedPages:
    return unmapMDL(A);
  case KernelAPIKind::IoAttachDeviceToDeviceStack:
    return attachDevice(A[0], A[1]);
  case KernelAPIKind::IoGetAttachedDevice:
    return queryDeviceStack(A[0], DeviceStackQuery::Top);
  case KernelAPIKind::IoGetRelatedDeviceObject:
    return queryFileDevice(A[0], FileDeviceQuery::Related);
  case KernelAPIKind::IoGetBaseFileSystemDeviceObject:
    return queryFileDevice(A[0], FileDeviceQuery::Base);
  case KernelAPIKind::IoGetDeviceObjectPointer:
    return getDeviceObjectPointer(A, Context);
  case KernelAPIKind::IoQueryFileInformation:
    return beginFileAPIRequest(A, DriverRequestKind::QueryInformation, false,
                               Context);
  case KernelAPIKind::ZwQueryInformationFile:
    return beginFileAPIRequest(A, DriverRequestKind::QueryInformation, true,
                               Context);
  case KernelAPIKind::ZwReadFile:
    return beginFileAPIRequest(A, DriverRequestKind::Read, true, Context);
  case KernelAPIKind::ZwWriteFile:
    return beginFileAPIRequest(A, DriverRequestKind::Write, true, Context);
  case KernelAPIKind::ZwSetInformationFile:
    return setFileInformation(A, Context);
  case KernelAPIKind::ZwFlushBuffersFile:
    return beginFileAPIRequest(A, DriverRequestKind::FlushBuffers, true,
                               Context);
  case KernelAPIKind::ZwDeviceIoControlFile:
    return beginFileAPIRequest(A, DriverRequestKind::DeviceControl, true,
                               Context);
  case KernelAPIKind::ZwFsControlFile:
    return beginFileAPIRequest(A, DriverRequestKind::FileSystemControl, true,
                               Context);
  case KernelAPIKind::ZwCreateFile:
  case KernelAPIKind::IoCreateFile:
    return createFile(A, FileOpenKind::Create, Context);
  case KernelAPIKind::ZwOpenFile:
    return createFile(A, FileOpenKind::Open, Context);
  case KernelAPIKind::IoCancelFileOpen:
    return cancelFileOpen(A, Context);
  case KernelAPIKind::IoGetAttachedDeviceReference:
    return queryDeviceStack(A[0], DeviceStackQuery::TopReference);
  case KernelAPIKind::IoGetLowerDeviceObject:
    return queryDeviceStack(A[0], DeviceStackQuery::LowerReference);
  case KernelAPIKind::IoGetDeviceAttachmentBaseRef:
    return queryDeviceStack(A[0], DeviceStackQuery::BaseReference);
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
  case KernelAPIKind::IoCompleteRequest: {
    const auto *Request = requestForIRP(A[0]);
    if (Request && Request->FileRequestID)
      if (auto E = validateFileAPIInterrupts(Context))
        return E;
    return completeDriverRequest(A);
  }
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
  case KernelAPIKind::vDbgPrintExWithPrefix:
    return prefixedDebugMessage(A);
  case KernelAPIKind::KeGetCurrentIrql:
    return CurrentIRQL;
  case KernelAPIKind::memcpy_s:
    return checkedMemoryCopy(A, true);
  case KernelAPIKind::memmove_s:
    return checkedMemoryCopy(A, false);
  case KernelAPIKind::RtlImageNtHeader:
    return runtime::imageNtHeader(*this, Memory, A[0]);
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
