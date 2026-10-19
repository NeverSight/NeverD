//===- KernelModelIO.cpp - Synchronous WDM requests -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Synchronous software WDM requests.
///
//===----------------------------------------------------------------------===//

#include "../driver/DriverScenario.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <array>
#include <iterator>

namespace neverd::emulation {
namespace {
// x64 WDM layout, including the POINTER_ALIGNMENT members in the stack union.
// The IRP tail pointer is the field used by IoGetCurrentIrpStackLocation's
// public macro; it is not a guest thread or private kernel structure.
// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ns-wdm-_irp
// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ns-wdm-_io_stack_location
// https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_object
using namespace windows;

llvm::Error ioError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

unsigned majorFunction(DriverRequestKind Kind) {
  switch (Kind) {
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Major)                      \
  case DriverRequestKind::Name:                                                \
    return Major;
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
  }
  return profile::MajorFunctionCount;
}

bool ntSuccess(uint32_t Status) {
  return !(Status & profile::NTStatusFailureMask);
}
bool ntError(uint32_t Status) {
  return (Status >> profile::NTStatusSeverityShift) ==
         profile::NTStatusErrorSeverity;
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::materializeProcessObject(uint32_t ProcessID) {
  if (!ProcessID)
    return ioError("process object requires a modeled process identity");
  if (auto It = ProcessObjects.find(ProcessID); It != ProcessObjects.end())
    return It->second;
  auto Object = allocate(ProcessObjectSize);
  if (!Object)
    return Object.takeError();
  ProcessObjects.emplace(ProcessID, *Object);
  ProcessObjectIDs.emplace(*Object, ProcessID);
  ReferencedObjects.emplace(*Object, ObjectReferences{ObjectKind::Process});
  return *Object;
}

llvm::Expected<uint64_t> KernelModel::processObject(uint32_t ProcessID) {
  auto Object = materializeProcessObject(ProcessID);
  if (!Object)
    return Object.takeError();
  if (auto E = synchronizeProcessList())
    return std::move(E);
  return *Object;
}

llvm::Error KernelModel::admitProcess(uint32_t ProcessID) {
  if (ProcessID && !ExitedUserProcesses.contains(ProcessID)) {
    LifecycleLiveProcesses.insert(ProcessID);
    LifecycleSeenProcesses.insert(ProcessID);
    return synchronizeProcessList();
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::requestorProcess(uint64_t IRP) {
  const auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return ioError("IoGetRequestorProcess requires a live IRP");
  // Bus-originated packets have no modeled requesting user thread.
  if (!Request->ProcessID)
    return 0;
  return processObject(Request->ProcessID);
}

llvm::Expected<uint64_t> KernelModel::currentProcess() {
  if (auto Context = executionProcessContext())
    return processObject(Context->ProcessID);
  return ioError("current process requires a modeled foreground or system "
                 "thread");
}

bool KernelModel::hasProcessAttachment(uint64_t Execution) const {
  return std::any_of(ProcessAttachments.begin(), ProcessAttachments.end(),
                     [&](const auto &Attachment) {
                       return Attachment.Execution == Execution;
                     });
}

llvm::Error KernelModel::stackAttachProcess(uint64_t Process, uint64_t ApcState,
                                            bool Private) {
  if (!Version &&
      (!Scheduler.active() ||
       (Scheduler.active()->Kind != KernelScheduler::CallbackKind::WorkItem &&
        Scheduler.active()->Kind !=
            KernelScheduler::CallbackKind::FrameworkInterruptWorkItem &&
        Scheduler.active()->Kind !=
            KernelScheduler::CallbackKind::FrameworkDeferred &&
        Scheduler.active()->Kind !=
            KernelScheduler::CallbackKind::SystemThread) ||
       CurrentExecution == profile::StackBase))
    return ioError("process attachment requires a system work-item thread");
  const auto Context = executionProcessContext();
  if (Version && !Context)
    return ioError("process attachment requires a modeled thread context");
  auto Target = ProcessObjectIDs.find(Process);
  if (Target == ProcessObjectIDs.end())
    return ioError("process attachment requires a known process object");
  const uint32_t ProcessID = Target->second;
  if (!isLiveProcess(ProcessID))
    return ioError("cannot attach to an unknown or exited process");
  if (ProcessID != SystemProcessID && !ProcessMemoryViews.contains(ProcessID) &&
      std::none_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.ProcessID == ProcessID && !Entry.second.Completed;
      }))
    return ioError(Version
                       ? "process attachment requires declared process memory "
                         "or a live requesting IRP"
                       : "process attachment requires a live requesting IRP");
  if (!Private) {
    if (!ApcState || (ApcState & 7) || ApcState < profile::UserProbeLimit)
      return ioError("process attachment requires aligned kernel APC storage");
    const uint64_t CurrentStackSize = CurrentExecution == profile::StackBase
                                          ? profile::StackSize
                                          : profile::CallbackStackSize;
    const bool CurrentStack =
        ApcState >= CurrentExecution &&
        ApcState - CurrentExecution <= CurrentStackSize &&
        KAPCStateSize <= CurrentStackSize - (ApcState - CurrentExecution);
    const bool NonPagedPool = std::any_of(
        Allocations.begin(), Allocations.end(), [&](const auto &Entry) {
          return Entry.second.NonPaged && ApcState >= Entry.first &&
                 ApcState - Entry.first <= Entry.second.Size &&
                 KAPCStateSize <= Entry.second.Size - (ApcState - Entry.first);
        });
    const bool ResidentDriver =
        Version &&
        std::any_of(ImageRAM.begin(), ImageRAM.end(), [&](const auto &R) {
          return ApcState >= R.first && ApcState - R.first <= R.second &&
                 KAPCStateSize <= R.second - (ApcState - R.first);
        });
    const auto Stack = ExecutionStacks.find(CurrentExecution);
    const bool ActiveStack =
        Version && Stack != ExecutionStacks.end() && ApcState >= Stack->first &&
        ApcState - Stack->first <= Stack->second &&
        KAPCStateSize <= Stack->second - (ApcState - Stack->first);
    if (!CurrentStack && !NonPagedPool && !ResidentDriver && !ActiveStack)
      return ioError("APC storage must be on the current thread stack or in "
                     "nonpaged pool");
    for (const auto &Attachment : ProcessAttachments)
      if (!Attachment.Private &&
          ApcState < Attachment.ApcState + KAPCStateSize &&
          Attachment.ApcState < ApcState + KAPCStateSize)
        return ioError("APC storage is already owned by an attachment");
    if (auto E = validateGuestAccess(ApcState, KAPCStateSize, true))
      return E;
    auto Writable = Memory.canAccess(ApcState, KAPCStateSize, Read | Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return ioError("process attachment requires writable APC storage");
  } else if (ApcState) {
    return ioError("private process attachment has no guest APC storage");
  }
  const bool PreviousUserContext = UserRequestContext;
  const uint32_t PreviousProcessID =
      PreviousUserContext ? CurrentUserProcessID : 4;
  auto PreviousProcess = processObject(Context->ProcessID);
  if (!PreviousProcess)
    return PreviousProcess.takeError();
  if (auto E = setUserRequestContext(ProcessID != SystemProcessID, ProcessID))
    return E;
  if (!Private) {
    std::array<uint8_t, KAPCStateSize> Saved{};
    if (auto E = Memory.write(ApcState, Saved))
      return E;
    if (auto E = Memory.writeInteger(ApcState + KAPCStateProcessOffset,
                                     *PreviousProcess, 8))
      return E;
  }
  ProcessAttachments.push_back(
      {CurrentExecution, ApcState, PreviousProcessID, PreviousUserContext,
       Private, Context.value_or(ExecutionProcessContext{}), ProcessID});
  return llvm::Error::success();
}

llvm::Error KernelModel::unstackDetachProcess(uint64_t ApcState, bool Private) {
  auto It = std::find_if(ProcessAttachments.rbegin(), ProcessAttachments.rend(),
                         [&](const auto &Attachment) {
                           return Attachment.Execution == CurrentExecution;
                         });
  if (It == ProcessAttachments.rend() || It->ApcState != ApcState ||
      It->Private != Private)
    return ioError("process detach requires the current thread's most recent "
                   "APC state");
  const auto Attachment = *It;
  if (auto E = setUserRequestContext(Attachment.PreviousUserContext,
                                     Attachment.PreviousProcessID))
    return E;
  ProcessAttachments.erase(std::next(It).base());
  return llvm::Error::success();
}

llvm::Error KernelModel::probeAndLockProcessPages(uint64_t MDL,
                                                  uint64_t Process,
                                                  uint8_t Mode,
                                                  uint32_t Operation) {
  auto Current = currentProcess();
  if (!Current)
    return Current.takeError();
  // This wrapper narrows LOCK_OPERATION and treats every non-read value as
  // write access before calling the ordinary probe; it is not a direct alias.
  const uint32_t Access = Operation != windows::IoReadAccess
                              ? windows::IoWriteAccess
                              : windows::IoReadAccess;
  if (Process == *Current)
    return probeAndLockPages(MDL, Mode, Access);
  // Keep the shared attachment owner's worker/live-process admission. This
  // private APC state has no fabricated guest stack or pool projection.
  if (auto E = stackAttachProcess(Process, 0, true))
    return E;
  auto Probed = probeAndLockPages(MDL, Mode, Access);
  // The target's termination handler detaches on exceptions as well. Preserve
  // the guest exception when restoration succeeds; never swallow a detach
  // error.
  return llvm::joinErrors(std::move(Probed), unstackDetachProcess(0, true));
}

KernelModel::ActiveRequest *KernelModel::requestForIRP(uint64_t IRP) {
  auto I = Requests.find(IRP);
  return I == Requests.end() ? nullptr : &I->second;
}

const KernelModel::ActiveRequest *
KernelModel::requestForIRP(uint64_t IRP) const {
  auto I = Requests.find(IRP);
  return I == Requests.end() ? nullptr : &I->second;
}

bool KernelModel::requestPending(uint64_t IRP,
                                 PendingRequestScope Scope) const {
  auto Pending = [&](const ActiveRequest &Request) {
    // An accepted child already owns its packet and power ticket while its
    // elevated issuer waits for the passive dispatch worker to run.
    if (!Request.DispatchReturned)
      return bool(Request.ChildPower);
    if (Request.Completed &&
        (!Request.ChildPower || Request.ChildPower->CallbackReturned))
      return false;
    if (Scope == PendingRequestScope::All)
      return true;
    if (UsbIdle.isParked(Request.IRP))
      return false;
    if (!Request.Completed && Request.PowerOperation &&
        Request.PowerOperation->Minor == DevicePowerRequest::WaitWake) {
      const auto Wake = ProviderWakeIRPs.find(Request.PnpDevice);
      if (Wake != ProviderWakeIRPs.end() && Wake->second.IRP == Request.IRP)
        return false;
    }
    return !Framework || !Framework->isPowerParkedIRP(Request.IRP);
  };
  if (IRP) {
    const auto *Request = requestForIRP(IRP);
    return Request && Pending(*Request);
  }
  return std::any_of(Requests.begin(), Requests.end(),
                     [&](const auto &Entry) { return Pending(Entry.second); });
}

llvm::Error KernelModel::prepareRequestBuffers(ActiveRequest &Record,
                                               const DriverRequest &Input) {
  auto *Request = &Record;
  const bool IsRead = Input.Kind == DriverRequestKind::Read;
  const bool IsWrite = Input.Kind == DriverRequestKind::Write;
  const bool IsIOCTL = Input.Kind == DriverRequestKind::DeviceControl;
  if (Input.CancelAfter100ns && (*Input.CancelAfter100ns > INT64_MAX ||
                                 (!IsRead && !IsWrite && !IsIOCTL)))
    return ioError(
        "cancellation requires a transfer request and bounded delay");
  Request->OutputSize = Input.OutputSize;
  Request->InputSize = Input.Input.size();
  Request->ByteOffset = Input.ByteOffset;
  Request->TransferSize = IsWrite ? Input.Input.size() : Input.OutputSize;
  if (Request->Neither)
    return prepareUserRequestBuffers(Record, Input);
  Request->BufferSize = Request->Direct ? (IsIOCTL ? Input.Input.size() : 0)
                                        : std::max<uint64_t>(Input.Input.size(),
                                                             Input.OutputSize);
  if (Request->BufferSize) {
    auto Buffer = allocatePhysicalBuffer(Request->BufferSize, PoolAlignment, 0,
                                         Request->BufferSize);
    if (!Buffer)
      return Buffer.takeError();
    Request->SystemBuffer = *Buffer;
    if (auto E = Memory.write(*Buffer, Input.Input))
      return E;
  }
  // Raw user addresses are intentionally inaccessible to the guest kernel in
  // this profile. The MDL mapping is the sole guest-visible access to direct
  // data; no second authoritative copy is maintained for the user address.
  const uint64_t UserSize =
      Request->Direct ? Request->TransferSize : Request->OutputSize;
  if (UserSize) {
    auto Buffer = allocatePhysicalBuffer(UserSize, PoolAlignment, 0, UserSize);
    if (!Buffer)
      return Buffer.takeError();
    Request->UserBuffer = *Buffer;
  }
  if (Request->Direct && Request->TransferSize) {
    llvm::ArrayRef<uint8_t> Initial = IsWrite ? Input.Input : Input.DirectInput;
    // IN_DIRECT only requires readable caller pages; it does not require a
    // read-only system mapping. Scenario buffers are readable and writable.
    // MdlMappingNoWrite independently restricts an explicitly created mapping.
    // DMA writes require the request's write-lock contract independently of
    // those CPU mapping permissions.
    const bool DmaWritable =
        IsRead || (IsIOCTL && (Input.ControlCode & IoControlMethodMask) ==
                                  MethodOutDirect);
    auto MDL = createRequestMDL(Request->IRP, Request->TransferSize, Initial,
                                true, Request->UserBuffer, DmaWritable);
    if (!MDL)
      return MDL.takeError();
    Request->Mdl = *MDL;
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::initializeRequestPacket(ActiveRequest &Record,
                                                 const DriverRequest &Input) {
  auto *Request = &Record;
  const uint64_t Packet = Request->IRP;
  const uint64_t File = Request->FileAddress;
  const bool IsPnp = Input.Kind == DriverRequestKind::Pnp;
  const bool IsPower = Input.Kind == DriverRequestKind::Power;
  const bool IsInternal =
      Input.Kind == DriverRequestKind::InternalDeviceControl;
  const bool FileFree = IsPnp || IsPower || IsInternal;
  const bool FileLifecycle = Input.Kind == DriverRequestKind::Close ||
                             Input.Kind == DriverRequestKind::Cleanup;
  const bool UnlockAll = Input.Kind == DriverRequestKind::UnlockAll;
  if (IsInternal && (File || Request->ProcessID))
    return ioError(
        "internal control packets require kernel file-free ownership");
  uint32_t Flags = FileFree || Request->AsynchronousFile ? 0 : IRPSynchronous;
  if (Input.Kind == DriverRequestKind::Create)
    Flags |= IRPCreate;
  if (FileLifecycle)
    Flags |= IRPClose | IRPSynchronous;
  if (UnlockAll)
    Flags |= 0x1000 | IRPSynchronous;
  if (Input.Kind == DriverRequestKind::Read)
    Flags |= IRPReadOperation;
  if (Input.Kind == DriverRequestKind::Write)
    Flags |= IRPWriteOperation;
  if (Request->SystemBuffer)
    Flags |= IRPBufferedAllocation;
  if (Input.OutputSize && !Request->Direct && !Request->Neither)
    Flags |= IRPCopyOutput;
  struct Field {
    uint64_t Offset, Value;
    unsigned Size;
  };
  if (auto E =
          initializeIRPHeader(Packet, Request->StackCount, Request->StackCount,
                              Request->IOStatusWritten))
    return E;
  for (const Field &F : std::array<Field, 8>{
           {{IRPMdlOffset, Request->Mdl, 8},
            {IRPFlagsOffset, Flags, 4},
            {IRPSystemBufferOffset, Request->SystemBuffer, 8},
            {IRPStatusOffset, FileFree ? StatusNotSupported : 0, 4},
            {IRPRequestorModeOffset,
             FileFree || FileLifecycle || UnlockAll ? KernelMode : UserMode, 1},
            {IRPUserBufferOffset, Request->UserBuffer, 8},
            {IRPOriginalFileOffset, File, 8},
            {Request->Stack - Packet, majorFunction(Input.Kind), 1}}})
    if (auto E = Memory.writeInteger(Packet + F.Offset, F.Value, F.Size))
      return E;
  if (auto E = Memory.writeInteger(Request->Stack + StackDeviceOffset,
                                   Request->DeviceRoute.front(), 8))
    return E;
  if (auto E = Memory.writeInteger(Request->Stack + StackFileOffset, File, 8))
    return E;
  if (UnlockAll)
    if (auto E = Memory.writeInteger(Request->Stack + StackMinorOffset, 3, 1))
      return E;
  if (IsPnp) {
    if (auto E = Memory.writeInteger(Request->Stack + StackMinorOffset,
                                     uint8_t(Input.Pnp->Minor), 1))
      return E;
    if (auto E = initializePnpResources(Record))
      return E;
    if (auto E = Memory.writeInteger(Packet + IRPInformationOffset, 0, 8))
      return E;
    Request->IOStatusWritten.fill(true);
  } else if (IsPower) {
    const auto &Power = *Input.Power;
    for (const Field &F : std::array<Field, 5>{
             {{StackMinorOffset, uint8_t(Power.Minor), 1},
              {StackPowerSystemContextOffset,
               Power.Minor == DevicePowerRequest::WaitWake
                   ? Power.State
                   : Power.SystemContext,
               4},
              {StackPowerTypeOffset, uint32_t(Power.Type), 4},
              {StackPowerStateOffset,
               Power.Minor == DevicePowerRequest::WaitWake ? 0 : Power.State,
               4},
              {StackPowerActionOffset, uint32_t(Power.Action), 4}}})
      if (auto E =
              Memory.writeInteger(Request->Stack + F.Offset, F.Value, F.Size))
        return E;
    if (auto E = Memory.writeInteger(Packet + IRPInformationOffset, 0, 8))
      return E;
    Request->IOStatusWritten.fill(true);
  } else if (Input.Kind == DriverRequestKind::DeviceControl || IsInternal) {
    for (const Field &F :
         std::array<Field, 3>{{{StackParametersOffset, Input.OutputSize, 4},
                               {StackInputLengthOffset, Input.Input.size(), 4},
                               {StackIOControlOffset, Input.ControlCode, 4}}})
      if (auto E =
              Memory.writeInteger(Request->Stack + F.Offset, F.Value, F.Size))
        return E;
    if (Request->Neither)
      if (auto E = Memory.writeInteger(Request->Stack + StackType3InputOffset,
                                       Request->UserInput, 8))
        return E;
  } else if (Input.Kind == DriverRequestKind::Read ||
             Input.Kind == DriverRequestKind::Write) {
    for (const Field &F : std::array<Field, 3>{
             {{StackReadWriteLengthOffset, Request->TransferSize, 4},
              {StackReadWriteKeyOffset, 0, 4},
              {StackReadWriteByteOffset, Input.ByteOffset, 8}}})
      if (auto E =
              Memory.writeInteger(Request->Stack + F.Offset, F.Value, F.Size))
        return E;
  } else if (Input.Kind == DriverRequestKind::Create) {
    auto Security = allocate(SecurityContextSize);
    if (!Security)
      return Security.takeError();
    Request->SecurityContext = *Security;
    if (auto E = Memory.writeInteger(*Security + SecurityDesiredAccessOffset,
                                     CreateDesiredAccess, 4))
      return E;
    if (auto E = Memory.writeInteger(Request->Stack + StackParametersOffset,
                                     *Security, 8))
      return E;
    if (auto E = Memory.writeInteger(Request->Stack + StackInputLengthOffset,
                                     CreateOptions, 4))
      return E;
    if (auto E = Memory.writeInteger(Request->Stack + StackCreateShareOffset,
                                     ShareReadWrite, 2))
      return E;
  }
  auto Watch = watchInitialization(Record.IRP + IRPStatusOffset,
                                   Record.IOStatusWritten.size());
  if (!Watch)
    return Watch.takeError();
  Record.IOStatusInitialization = std::move(*Watch);
  return llvm::Error::success();
}

llvm::Expected<KernelModel::Invocation>
KernelModel::beginRequest(const DriverRequest &Input,
                          std::optional<size_t> SourceIndex) {
  DriverRequestResult Observation;
  Observation.Kind = Input.Kind;
  Observation.Device = Input.Device;
  Observation.DeviceID = Input.DeviceID;
  Observation.ControlCode = Input.ControlCode;
  Observation.File = Input.File;
  Observation.RequestorProcessID = Input.RequestorProcessID;
  Observation.ByteOffset = Input.ByteOffset;
  Result.Requests.push_back(std::move(Observation));
  const size_t Index = Result.Requests.size() - 1;
  return beginFileRequest(Input, SourceIndex, Index);
}

llvm::Expected<KernelModel::Invocation> KernelModel::beginFileRequest(
    const DriverRequest &Input, std::optional<size_t> SourceIndex, size_t Index,
    uint64_t RetainedFile, uint64_t DispatchDevice, uint64_t FileOperationID) {
  const bool CancelOpen =
      Result.Requests[Index].Origin == DriverRequestOrigin::FileCancelOpen;
  auto FileIt = FileObjects.find(
      RetainedFile ? RetainedFile
                   : (Files.contains(Input.File) ? Files.at(Input.File) : 0));
  const bool UnloadTeardown =
      Unloading && RetainedFile && FileIt != FileObjects.end() &&
      FileIt->second.Source == OpenFile::Origin::DriverCreate &&
      (Input.Kind == DriverRequestKind::Cleanup ||
       Input.Kind == DriverRequestKind::UnlockAll ||
       Input.Kind == DriverRequestKind::Close);
  if ((Unloading && !UnloadTeardown) || Unloaded ||
      (!RetainedFile &&
       std::any_of(Requests.begin(), Requests.end(), [](const auto &Entry) {
         return !Entry.second.DispatchReturned;
       })))
    return ioError(
        "cannot begin a request during another invocation or after unload");
  if ((!Input.InterruptEvents.empty() || !Input.DmaEvents.empty()) &&
      Input.Kind != DriverRequestKind::Read &&
      Input.Kind != DriverRequestKind::Write &&
      Input.Kind != DriverRequestKind::DeviceControl)
    return ioError("interrupt events require a transfer request");
  if (auto E =
          Interrupts.canArm(Input.InterruptEvents, SourceIndex.value_or(Index),
                            Scheduler.now100ns()))
    return E;
  if (auto E = canArmPowerPolicyEvents(Input.PowerPolicyEvents))
    return E;
  if (auto E = DMA.canArm(Input.DmaEvents, SourceIndex.value_or(Index),
                          Scheduler.now100ns()))
    return E;
  if (auto E = validateDriverUserMemory(Input))
    return E;
  if (Input.Kind == DriverRequestKind::Pnp) {
    if (auto E = snapshot())
      return E;
    return beginPnpRequest(Input, Index);
  }
  if (Input.Kind == DriverRequestKind::Power) {
    if (auto E = snapshot())
      return E;
    return beginPowerRequest(Input, Index);
  }
  if (Input.Pnp || Input.Power ||
      (!Input.Device.empty() && !Input.DeviceID.empty()))
    return ioError("file request has incompatible PnP fields or selectors");
  const unsigned Major = majorFunction(Input.Kind);
  if (Major >= 28)
    return ioError("unknown WDM request kind");
  const bool IsRead = Input.Kind == DriverRequestKind::Read;
  const bool IsWrite = Input.Kind == DriverRequestKind::Write;
  const bool IsIOCTL = Input.Kind == DriverRequestKind::DeviceControl;
  if (Input.AsynchronousFile && Input.Kind != DriverRequestKind::Create)
    return ioError("asynchronous_file requires a create request");
  if ((!IsIOCTL && Input.ControlCode) ||
      (!IsIOCTL && !IsWrite && !Input.Input.empty()) ||
      (!IsIOCTL && !IsRead && Input.OutputSize) ||
      (!IsRead && !IsWrite && Input.ByteOffset))
    return ioError("request fields do not match its WDM major function");
  if (Input.UserInputAccess || Input.UserOutputAccess) {
    const bool NeitherIOCTL =
        IsIOCTL && (Input.ControlCode & IoControlMethodMask) == MethodNeither;
    if ((!NeitherIOCTL && !IsRead && !IsWrite) ||
        (IsRead && Input.UserInputAccess) ||
        (IsWrite && Input.UserOutputAccess) ||
        (Input.UserInputAccess && Input.Input.empty()) ||
        (Input.UserOutputAccess && !Input.OutputSize))
      return ioError("user page access requires a nonempty neither-I/O "
                     "buffer in the matching direction");
  }
  if ((!IsIOCTL ||
       (Input.ControlCode & IoControlMethodMask) == MethodBuffered ||
       (Input.ControlCode & IoControlMethodMask) == MethodNeither) &&
      !Input.DirectInput.empty())
    return ioError("direct_input is valid only for direct IOCTLs");
  if (Input.DirectInput.size() > Input.OutputSize)
    return ioError("direct_input exceeds the second IOCTL buffer length");
  if (Input.Input.size() > KernelMemoryBytes ||
      Input.OutputSize > KernelMemoryBytes)
    return ioError("request buffers exceed the configured kernel arena");
  const uint64_t TransferSize = IsWrite ? Input.Input.size() : Input.OutputSize;
  if ((IsRead || IsWrite) && (Input.ByteOffset > INT64_MAX ||
                              TransferSize > INT64_MAX - Input.ByteOffset))
    return ioError("READ/WRITE byte range exceeds a nonnegative file offset");
  if (auto E = snapshot())
    return E;
  uint64_t Device = 0;
  if (RetainedFile) {
    if (FileIt == FileObjects.end() ||
        (Input.Kind != DriverRequestKind::Close &&
         Input.Kind != DriverRequestKind::Cleanup &&
         Input.Kind != DriverRequestKind::UnlockAll))
      return ioError("retained lifecycle request lost its file object");
    Device = FileIt->second.Device;
  } else if (!Input.DeviceID.empty()) {
    auto Found = PnpDevices.find(Input.DeviceID);
    if (Found == PnpDevices.end() || !Found->second.AddDeviceStatus ||
        (*Found->second.AddDeviceStatus & profile::NTStatusFailureMask) ||
        !Devices.count(Found->second.PDO))
      return ioError(
          "file request requires a successfully added live device_id");
    Device = Found->second.PDO;
    if (Input.Kind != DriverRequestKind::Create &&
        FileIt != FileObjects.end()) {
      if (FileIt->second.PnpDevice != Device)
        return ioError("device_id does not match the file's device");
      Device = FileIt->second.Device;
    }
  } else if (Input.Device.empty()) {
    // An existing file identifies its device even when several devices exist.
    if (Input.Kind != DriverRequestKind::Create && FileIt != FileObjects.end())
      Device = FileIt->second.Device;
    else {
      for (const auto &[Address, Candidate] : Devices) {
        if (Candidate.OwnerKind != DeviceOwnerKind::Guest ||
            Candidate.DeletePending || Candidate.UnlinkedFromDriver)
          continue;
        if (Device)
          return ioError(
              "an unnamed request requires exactly one live guest device");
        Device = Address;
      }
      if (!Device)
        return ioError(
            "an unnamed request requires exactly one live guest device");
    }
  } else {
    auto Resolved = resolveDeviceName(Input.Device);
    if (!Resolved)
      return Resolved.takeError();
    Device = *Resolved;
    if (!Device)
      return ioError("request device name does not identify a live device");
  }
  if (Devices.at(Device).OwnerKind == DeviceOwnerKind::FiniteFileSystem)
    return ioError(
        "filesystem scenario requests require a modeled provider IRP contract");
  Result.Requests[Index].Device = Devices.at(Device).Name;
  uint64_t PnpOwner = 0;
  if (Input.Kind != DriverRequestKind::Create && FileIt != FileObjects.end()) {
    PnpOwner = FileIt->second.PnpDevice;
  } else {
    auto Owner = pnpDeviceForRoute(Device);
    if (!Owner)
      return Owner.takeError();
    PnpOwner = *Owner;
  }
  const bool LifecycleIo = PnpOwner &&
                           Input.Kind != DriverRequestKind::Cleanup &&
                           Input.Kind != DriverRequestKind::UnlockAll &&
                           Input.Kind != DriverRequestKind::Close;
  if (PnpOwner) {
    const auto *Owner = pnpDeviceForPDO(PnpOwner);
    if (!Owner || !Owner->AddDeviceStatus ||
        (*Owner->AddDeviceStatus & profile::NTStatusFailureMask) ||
        !Devices.count(PnpOwner))
      return ioError("file request requires a successfully added live device");
    Result.Requests[Index].DeviceID = Result.PnpDevices[Owner->ResultIndex].ID;
    if (auto E = Lifecycle.validateIoSubmission(PnpOwner))
      return E;
  }
  auto Top = DispatchDevice ? llvm::Expected<uint64_t>(DispatchDevice)
                            : topAttachedDevice(Device);
  if (!Top)
    return Top.takeError();
  auto Route = deviceStack(*Top);
  if (!Route)
    return Route.takeError();
  if (auto E = validateFileSystemRoute(*Route, RetainedFile, Input.Kind))
    return E;
  auto StackCount = Memory.readInteger(*Top + DeviceStackCountOffset, 1);
  if (!StackCount)
    return StackCount.takeError();
  if (!*StackCount || *StackCount > MaxIRPStackCount ||
      *StackCount < Route->size())
    return ioError("device stack requires a positive bounded IRP stack count");
  if (PnpOwner && *Top == PnpOwner)
    return ioError("resource-free PDO has no attached guest file dispatch");
  bool Direct = false, Neither = false;
  if (IsIOCTL) {
    const auto Method = Input.ControlCode & IoControlMethodMask;
    Direct = Method == MethodInDirect || Method == MethodOutDirect;
    Neither = Method == MethodNeither;
  } else if (IsRead || IsWrite) {
    auto Flags = Memory.readInteger(*Top + DeviceFlagsOffset, 4);
    if (!Flags)
      return Flags.takeError();
    const uint64_t TransferFlags = *Flags & (DeviceBufferedIO | DeviceDirectIO);
    if (TransferFlags == (DeviceBufferedIO | DeviceDirectIO))
      return ioError("READ/WRITE device sets both DO_BUFFERED_IO and "
                     "DO_DIRECT_IO");
    Neither = !TransferFlags;
    Direct = TransferFlags == DeviceDirectIO;
  }
  if (!Neither && (!Input.UserBuffers.empty() || !Input.UserPointers.empty()))
    return ioError("declared user memory requires neither-I/O transfer");
  if (!Neither && (Input.UserInputAccess || Input.UserOutputAccess))
    return ioError("user page access requires neither READ/WRITE I/O");
  if (Input.UserUnmapAfterDispatch &&
      (!Neither ||
       (Input.Input.empty() && !Input.OutputSize && Input.UserBuffers.empty())))
    return ioError("user unmapping requires a nonempty neither-I/O transfer");
  if (Input.RequestorProcessID < 4 ||
      (Input.RequestorProcessID == 4 && !RetainedFile))
    return ioError("requestor process identity must be above the system PID");
  // Process and thread IDs share one namespace; issued IDs keep their owner.
  if (IssuedThreadClientIDs.contains(Input.RequestorProcessID))
    return ioError("requestor process identity collides with an issued thread "
                   "client ID");
  if (ExitedUserProcesses.contains(Input.RequestorProcessID) &&
      Input.Kind != DriverRequestKind::Cleanup &&
      Input.Kind != DriverRequestKind::UnlockAll &&
      Input.Kind != DriverRequestKind::Close)
    return ioError("requesting process has exited");
  if (Input.RequestorExitAfterDispatch &&
      (!Neither ||
       (Input.Input.empty() && !Input.OutputSize && Input.UserBuffers.empty())))
    return ioError("requestor exit requires a nonempty neither-I/O transfer");
  if (Input.Kind == DriverRequestKind::Create) {
    if (NextObjectHandle > profile::SystemThreadHandleLimit)
      return ioError("scenario file handle capacity exhausted");
    if (Devices.at(Device).DeletePending || Devices.at(*Top).DeletePending)
      return ioError("CREATE cannot target a delete-pending device");
    if (FileIt != FileObjects.end())
      return ioError("CREATE requires an unused file identity");
    auto Flags = Memory.readInteger(Device + DeviceFlagsOffset, 4);
    if (!Flags)
      return Flags.takeError();
    if ((*Flags & DeviceExclusive) &&
        std::any_of(FileObjects.begin(), FileObjects.end(),
                    [&](const auto &F) { return F.second.Device == Device; }))
      return ioError("exclusive device already has a live file object");
    auto File = allocate(FileSize);
    if (!File)
      return File.takeError();
    OpenFile State;
    State.Address = *File;
    State.Device = Device;
    State.PnpDevice = PnpOwner;
    State.Asynchronous = Input.AsynchronousFile.value_or(false);
    State.State = FileState::Opening;
    State.ScenarioID = Input.File;
    State.ScenarioHandle = NextObjectHandle;
    FileIt = FileObjects.emplace(*File, std::move(State)).first;
    Files.emplace(Input.File, *File);
    ReferencedObjects.emplace(*File, ObjectReferences{ObjectKind::File});
    retainObjectHandle(*File, CreateDesiredAccess, 0, Input.RequestorProcessID);
    ObjectHandles.at(FileIt->second.ScenarioHandle).ScenarioOwned = true;
    struct Field {
      uint64_t Offset, Value;
      unsigned Size;
    };
    for (const Field &F : std::array<Field, 9>{
             {{0, FileType, 2},
              {2, FileSize, 2},
              {FileDeviceOffset, Device, 8},
              {FileReadAccessOffset, 1, 1},
              {FileWriteAccessOffset, 1, 1},
              {FileSharedReadOffset, 1, 1},
              {FileSharedWriteOffset, 1, 1},
              {FileFlagsOffset,
               State.Asynchronous ? FileFlags & ~FileSynchronousIO : FileFlags,
               4},
              {FileNameBufferOffset, 0, 8}}})
      if (auto E = Memory.writeInteger(*File + F.Offset, F.Value, F.Size))
        return E;
    if (auto E = updateDeviceReferences(Device))
      return E;
  } else {
    if (FileIt == FileObjects.end() || FileIt->second.Device != Device)
      return ioError(
          "request requires a successful CREATE on the same device and file");
    const auto State = FileIt->second.State;
    const bool DeferredScenarioClose =
        Input.Kind == DriverRequestKind::Close && !RetainedFile &&
        FileIt->second.ScenarioCleanupRequested &&
        ReferencedObjects.at(FileIt->first).Handles;
    const bool RetainedCleanup = RetainedFile && State == FileState::Cleaned &&
                                 (Input.Kind == DriverRequestKind::Cleanup ||
                                  Input.Kind == DriverRequestKind::UnlockAll);
    const bool CancelCleanup = CancelOpen && RetainedFile &&
                               Input.Kind == DriverRequestKind::Cleanup &&
                               State == FileState::Opening;
    if ((Input.Kind == DriverRequestKind::Close &&
         State != FileState::Cleaned && !DeferredScenarioClose) ||
        (Input.Kind != DriverRequestKind::Close && State != FileState::Open &&
         !RetainedCleanup && !CancelCleanup))
      return ioError(
          "file request order must be CREATE, READ/WRITE/DEVICE_CONTROL*, "
          "CLEANUP, CLOSE");
  }
  const bool Transfer = IsRead || IsWrite || IsIOCTL;
  const bool FileHasActiveRequest =
      std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.FileAddress == FileIt->second.Address;
      });
  const bool OnlyOpeningRequest =
      CancelOpen &&
      std::none_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.FileAddress == FileIt->second.Address &&
               Entry.second.Kind != DriverRequestKind::Create;
      });
  if (FileHasActiveRequest && !OnlyOpeningRequest) {
    if (!FileIt->second.Asynchronous)
      return ioError(
          "a synchronous file requires its prior request to finalize");
    if (!Transfer ||
        std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
          const auto &Prior = Entry.second;
          return Prior.FileAddress == FileIt->second.Address &&
                 Prior.Kind != DriverRequestKind::Read &&
                 Prior.Kind != DriverRequestKind::Write &&
                 Prior.Kind != DriverRequestKind::DeviceControl;
        }))
      return ioError("asynchronous file lifecycle requires prior transfers "
                     "to finalize");
  }
  if (Input.Kind == DriverRequestKind::Cleanup && !RetainedFile) {
    auto &File = FileIt->second;
    if (!File.ScenarioHandle || File.ScenarioCleanupRequested)
      return ioError("scenario file handle has already been closed");
    const auto Handle = File.ScenarioHandle;
    if (ObjectHandles.at(Handle).ProcessID != Input.RequestorProcessID)
      return ioError("scenario CLEANUP requires the handle-owning process");
    auto Kind = planFileHandleClose(Handle);
    if (!Kind)
      return Kind.takeError();
    Invocation Prepared;
    if (*Kind) {
      auto Call =
          **Kind == DriverRequestKind::Cleanup
              ? beginFileRequest(Input, SourceIndex, Index, File.Address)
              : beginFileHandleRequest(File.Address, **Kind,
                                       Input.RequestorProcessID);
      if (!Call)
        return Call.takeError();
      Prepared = std::move(*Call);
    }
    releaseFileHandle(Handle);
    File.ScenarioHandle = 0;
    File.ScenarioCleanupRequested = true;
    if (*Kind != DriverRequestKind::Cleanup)
      File.DeferredCleanup = DeferredFileClose{Input, Index, SourceIndex};
    return Prepared;
  }
  if (Input.Kind == DriverRequestKind::Close) {
    auto &File = FileIt->second;
    const auto &References = ReferencedObjects.at(File.Address);
    if (References.Pointers || References.Handles || File.HandleCloseHolds ||
        File.InternalIOHolds) {
      if (RetainedFile)
        return ioError("retained CLOSE still has external references");
      File.DeferredClose = DeferredFileClose{Input, Index, SourceIndex};
      Files.erase(Input.File);
      // No CLOSE was dispatched. Its existing result row is filled only when
      // the last pointer releases this particular body, even after ID reuse.
      return Invocation{};
    }
    File.State = FileState::Closing;
  }
  auto Packet = allocate(IRPSize + *StackCount * StackSize);
  if (!Packet)
    return Packet.takeError();
  ActiveRequest Record{Input.Kind, Index};
  Record.IRP = *Packet;
  Record.StackCount = *StackCount;
  Record.Stack = *Packet + IRPSize + (*StackCount - 1) * StackSize;
  Record.DeviceRoute = std::move(*Route);
  Record.UnwoundPending.resize(*StackCount);
  Record.Device = Device;
  Record.PnpDevice = PnpOwner;
  Record.LifecycleIo = LifecycleIo;
  Record.FileBusCompletion = Input.FileBusCompletion;
  Record.Direct = Direct;
  Record.Neither = Neither;
  if (LifecycleIo)
    if (auto E = Lifecycle.trackIo(PnpOwner, *Packet))
      return E;
  Record.FileAddress = FileIt->second.Address;
  Record.FileOperation = FileOperationID;
  associateFileRequest(Result.Requests[Index], FileIt->second);
  Record.FileId = Input.File;
  Record.AsynchronousFile = FileIt->second.Asynchronous;
  Record.ProcessID = Input.RequestorProcessID;
  if (RetainedFile)
    Record.CallbackProcessID = Input.RequestorProcessID;
  RequestorProcessIDs.insert(Input.RequestorProcessID);
  if (auto E = admitProcess(Record.ProcessID))
    return std::move(E);
  if (Input.CancelAfter100ns) {
    llvm::Expected<uint64_t> Deadline = Scheduler.now100ns();
    if (*Input.CancelAfter100ns)
      Deadline = Scheduler.computeDeadline(-int64_t(*Input.CancelAfter100ns));
    if (!Deadline)
      return Deadline.takeError();
    Record.CancelDeadline = *Deadline;
  }
  auto *Request = &Requests.emplace(*Packet, std::move(Record)).first->second;
  for (uint64_t Owner : Request->DeviceRoute)
    if (auto E = retainDevice(Owner))
      return E;
  if (auto E = prepareRequestBuffers(*Request, Input))
    return E;
  if (auto E = initializeRequestPacket(*Request, Input))
    return E;
  if (FileOperationID)
    if (auto E = prepareCancelFileOpenPacket(*Packet, FileOperationID))
      return E;
  Result.Requests[Index].IRP = *Packet;
  if (auto E =
          Interrupts.arm(Input.InterruptEvents, SourceIndex.value_or(Index),
                         Scheduler.now100ns()))
    return E;
  if (auto E = DMA.arm(Input.DmaEvents, SourceIndex.value_or(Index),
                       Scheduler.now100ns()))
    return E;
  if (auto E = armPowerPolicyEvents(Input.PowerPolicyEvents,
                                    SourceIndex.value_or(Index)))
    return E;
  if (FileSystem.ownsDevice(*Top)) {
    auto Status = dispatchFileSystem(*Top, *Packet);
    if (!Status)
      return Status.takeError();
    if (auto E = recordDispatchReturn(*Packet, uint32_t(*Status)))
      return E;
    if (auto E = finalizeRequest(*Packet))
      return E;
    Invocation Call;
    Call.IRP = *Packet;
    return Call;
  }
  if (Framework) {
    auto Route = Framework->routeRequest(*Top, Request->IRP);
    if (!Route)
      return Route.takeError();
    if (*Route) {
      const auto &Dispatch = **Route;
      if (!Dispatch.PC) {
        if (auto E = recordDispatchReturn(*Packet, Dispatch.Status))
          return E;
        if (Dispatch.Status != windows::StatusPending)
          if (auto E = finalizeRequest(*Packet))
            return E;
        if (auto E = processRequestCancellations())
          return E;
        if (auto E = Framework->flushReadyNotifications())
          return E;
        Invocation Call;
        Call.IRP = *Packet;
        return Call;
      }
      Invocation Call;
      Call.PC = Dispatch.PC;
      Call.FrameworkDispatchStatus = Dispatch.Status;
      Call.FrameworkCallerContext = Dispatch.CallerContext;
      Call.SynchronizationObject = Dispatch.SynchronizationObject;
      Call.IRP = *Packet;
      // Caller-context preprocessing may enqueue a request without presenting
      // it to an I/O callback. Apply an immediate cancellation after that
      // routing decision so the queue can deliver its cancellation callback.
      if (!Dispatch.CallerContext) {
        if (auto E = processRequestCancellations())
          return E;
        if (auto E = Framework->flushReadyNotifications())
          return E;
      }
      uint64_t *Registers[] = {&Call.Argument0, &Call.Argument1,
                               &Call.Argument2, &Call.Argument3};
      for (size_t I = 0; I < Dispatch.Arguments.size(); ++I)
        if (I < 4)
          *Registers[I] = Dispatch.Arguments[I];
        else
          Call.StackArguments.push_back(Dispatch.Arguments[I]);
      return Call;
    }
  }
  auto GuestCallback = guestDriverDispatch(Devices.at(*Top).OwnerDriver, Major);
  if (!GuestCallback)
    return GuestCallback.takeError();
  const uint64_t Callback = *GuestCallback;
  if (!Callback) {
    if (anyInitialized(DispatchBytesWritten, DispatchInitialization,
                       Major * profile::PointerSize, profile::PointerSize))
      return ioError("driver explicitly registered a null dispatch callback");
    if (auto E = Memory.writeInteger(Request->IRP + IRPStatusOffset,
                                     InvalidDeviceRequest, 4))
      return E;
    Request->IOStatusWritten.fill(true);
    if (auto E = completeRequest(Request->IRP, 0))
      return E;
    if (auto E = recordDispatchReturn(*Packet, InvalidDeviceRequest))
      return E;
    if (auto E = finalizeRequest(*Packet))
      return E;
    Invocation Call;
    Call.IRP = *Packet;
    return Call;
  }
  Invocation Call{Callback, *Top, Request->IRP};
  Call.IRP = *Packet;
  return Call;
}

llvm::Expected<KernelModel::Invocation>
KernelModel::continueFrameworkCallerContext(uint64_t IRP) {
  if (!Framework)
    return ioError("framework caller-context continuation is unavailable");
  auto Routed = Framework->continueCallerContext(IRP);
  if (!Routed)
    return Routed.takeError();
  if (auto E = processRequestCancellations())
    return E;
  if (auto E = Framework->flushReadyNotifications())
    return E;
  Invocation Call;
  Call.PC = Routed->PC;
  Call.IRP = IRP;
  Call.FrameworkDispatchStatus = Routed->Status;
  Call.SynchronizationObject = Routed->SynchronizationObject;
  uint64_t *Registers[] = {&Call.Argument0, &Call.Argument1, &Call.Argument2,
                           &Call.Argument3};
  for (size_t I = 0; I < Routed->Arguments.size(); ++I)
    if (I < 4)
      *Registers[I] = Routed->Arguments[I];
    else
      Call.StackArguments.push_back(Routed->Arguments[I]);
  return Call;
}

llvm::Expected<std::vector<std::pair<uint64_t, uint64_t>>>
KernelModel::requestReleaseRanges(uint64_t IRP) const {
  const auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return ioError("completion release requires a live IRP");
  if (auto A = DriverIRPs.find(IRP);
      A != DriverIRPs.end() && !A->second.Builder)
    return ioError("caller-owned IRPs require explicit IoFreeIrp disposal");
  std::vector<std::pair<uint64_t, uint64_t>> Retiring;
  // CREATE and IoCancelFileOpen borrow a producer-owned packet. Their wrappers
  // dispose it after lower dispatch and every completion continuation return.
  const bool NativeRequest = nativeFileRequest(Request->FileRequestID);
  if (!Request->FileOperation && !NativeRequest)
    Retiring.emplace_back(IRP, IRPSize + Request->StackCount * StackSize);
  const auto NativeUsb = FrameworkUsbIdleRequests.find(IRP);
  if (NativeUsb != FrameworkUsbIdleRequests.end()) {
    const auto &Native = NativeUsb->second;
    if (Request->Kind != DriverRequestKind::InternalDeviceControl ||
        Native.Key.IRP != IRP || Native.Key.PDO != Request->PnpDevice ||
        Native.Device != Request->Device || !Native.Info ||
        Native.Info != Request->UserInput ||
        Request->InputSize != usb_idle::CallbackInfoSize ||
        Request->FileAddress || Request->FileId || Request->ProcessID ||
        Request->OutputSize || Request->SystemBuffer || Request->Mdl)
      return ioError("framework USB release lost its exact packet ownership");
    Retiring.emplace_back(Native.Info, usb_idle::CallbackInfoSize);
  }
  if (Request->RawResources) {
    Retiring.emplace_back(Request->RawResources, Request->ResourceListSize);
    Retiring.emplace_back(Request->TranslatedResources,
                          Request->ResourceListSize);
  }
  if (Request->SystemBuffer && !Request->BorrowedSystemBuffer && !NativeRequest)
    Retiring.emplace_back(Request->SystemBuffer, Request->BufferSize);
  if (Request->UserBuffer && !Request->Neither && !NativeRequest &&
      !(DriverIRPs.contains(IRP) && DriverIRPs.at(IRP).Builder))
    Retiring.emplace_back(Request->UserBuffer, Request->Direct
                                                   ? Request->TransferSize
                                                   : Request->OutputSize);
  if (Request->SecurityContext && !Request->FileOperation)
    Retiring.emplace_back(Request->SecurityContext, SecurityContextSize);
  std::vector<uint64_t> RetiringPins;
  std::vector<std::pair<uint64_t, uint64_t>> RevokingAliases;
  if (auto E = appendRequestMDLReleaseResources(IRP, Retiring, RetiringPins,
                                                RevokingAliases))
    return E;
  // This check is shared with terminal completion planning. No stack cursor,
  // continuation, dispatcher registration or output observation changes until
  // the entire retirement set has passed its ownership checks.
  for (const auto &[Address, Size] : Retiring)
    if (auto E = canReleaseUserViewsForBacking(Address, Size))
      return E;
  const size_t ReleasedCount = Retiring.size();
  Retiring.insert(Retiring.end(), RevokingAliases.begin(),
                  RevokingAliases.end());
  for (const auto &[Address, Size] : Retiring) {
    const std::optional<UsbIdleKey> RetiringUsb =
        NativeUsb != FrameworkUsbIdleRequests.end() &&
                Address == NativeUsb->second.Info &&
                Size == usb_idle::CallbackInfoSize
            ? std::optional<UsbIdleKey>{NativeUsb->second.Key}
            : std::nullopt;
    if (auto E = canRevokeVirtualRange(Address, Size, RetiringUsb))
      return E;
  }
  if (auto E = Physical.canReleaseRanges(
          llvm::ArrayRef(Retiring).take_front(ReleasedCount), RetiringPins))
    return E;
  return Retiring;
}

llvm::Error KernelModel::retireCompletedRequest(uint64_t IRP,
                                                uint8_t PriorityBoost) {
  auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return ioError("completion requires the active IRP and cannot occur twice");
  if (PriorityBoost)
    return ioError("modeled completion supports IO_NO_INCREMENT only");
  for (unsigned I = 0; I < Request->IOStatusWritten.size(); ++I)
    if ((I < 4 || I >= 8) && !initialized(Request->IOStatusWritten,
                                          Request->IOStatusInitialization, I))
      return ioError(
          "completion requires initialized IoStatus.Status and Information");
  auto Status = Memory.readInteger(IRP + IRPStatusOffset, 4);
  if (!Status)
    return Status.takeError();
  auto Information = Memory.readInteger(IRP + IRPInformationOffset, 8);
  if (!Information)
    return Information.takeError();
  if (auto E = validateRequestCompletion(IRP, static_cast<uint32_t>(*Status),
                                         *Information))
    return E;
  auto &Observation = Result.Requests[Request->ResultIndex];
  const bool HasIOCTLOutput =
      (Request->Kind == DriverRequestKind::DeviceControl ||
       (Request->Kind == DriverRequestKind::InternalDeviceControl &&
        DriverIRPs.contains(IRP) && DriverIRPs.at(IRP).Builder)) &&
      Request->OutputSize;
  const bool NativeRequest = nativeFileRequest(Request->FileRequestID);
  auto Pending = dispatchPending(*Request, Request->StackCount - 1);
  if (!Pending)
    return Pending.takeError();
  if (auto E = validateCompletionPending(*Request, *Pending))
    return E;
  auto Retiring = requestReleaseRanges(IRP);
  if (!Retiring)
    return Retiring.takeError();
  if (Request->PowerTicket) {
    if (auto E = validatePowerRequestCompletion(*Request, uint32_t(*Status)))
      return E;
  } else if (Request->PnpTicket) {
    if (auto E = validatePnpRequestCompletion(*Request, uint32_t(*Status)))
      return E;
  } else if (Request->LifecycleIo) {
    if (auto E = Lifecycle.validateIoCompletion(Request->PnpDevice, IRP))
      return E;
  }
  // The plan accounts for the exact pins released by this IRP. Revoke virtual
  // registrations now; expireRequestMDL drops those pins before retiring RAM.
  for (const auto &[Address, Size] : *Retiring)
    if (auto E = prepareRevokeVirtualRange(Address, Size))
      return E;
  Observation.IOStatus = static_cast<uint32_t>(*Status);
  Observation.Information = *Information;
  Request->PendingMarked = *Pending;
  if (Request->FileRequestID)
    if (auto E = publishFileRequestCompletion(IRP))
      return E;
  if (Request->FileOperation)
    if (auto E = publishFileOperationCompletion(IRP))
      return E;
  if (Request->ChildPower && Request->ChildPower->StatusBlock) {
    const auto Block = Request->ChildPower->StatusBlock;
    if (auto E = Memory.writeInteger(Block, uint32_t(*Status), 4))
      return E;
    if (auto E = Memory.writeInteger(Block + PowerStatusBlockInformationOffset,
                                     *Information, 8))
      return E;
  }
  if ((HasIOCTLOutput || Request->Kind == DriverRequestKind::Read) &&
      !Request->FileRequestID && !ntError(*Status) && *Information) {
    if (Request->Neither) {
      if (!RevokedUserAllocations.contains(Request->UserBuffer)) {
        Observation.Output.resize(*Information);
        if (auto E =
                Memory.readBacking(Request->UserBuffer, Observation.Output))
          return E;
      }
    } else if (Request->Direct) {
      auto Bytes = readMDLBytes(Request->Mdl, *Information);
      if (!Bytes)
        return Bytes.takeError();
      Observation.Output = std::move(*Bytes);
    } else {
      Observation.Output.resize(*Information);
      if (auto E = Memory.read(Request->SystemBuffer, Observation.Output))
        return E;
      if (!Request->BorrowedSystemBuffer)
        if (auto E = Memory.write(Request->UserBuffer, Observation.Output))
          return E;
    }
  }
  if (auto E = expireRequestMDL(IRP))
    return E;
  if (auto E = finishRequestLifecycle(*Request, uint32_t(*Status)))
    return E;
  if (Request->SystemBuffer && !Request->BorrowedSystemBuffer && !NativeRequest)
    if (auto E = Physical.retire(Request->SystemBuffer))
      return E;
  if (Request->UserBuffer && !Request->Neither && !NativeRequest &&
      !(DriverIRPs.contains(IRP) && DriverIRPs.at(IRP).Builder))
    if (auto E = Physical.retire(Request->UserBuffer))
      return E;
  // Ordinary request completion consumes the IRP. File operation producers
  // retain their borrowed packet until the calling wrapper resumes.
  Observation.Completed = true;
  if (Observation.UsbIdle)
    Observation.UsbIdle->CompletedAt100ns = Scheduler.now100ns();
  Request->Completed = true;
  Request->CancelDeadline.reset();
  if (DriverIRPs.contains(IRP) && DriverIRPs.at(IRP).Builder)
    if (auto E = notifyBuiltControlCompletion(IRP))
      return E;
  if (!Request->FileOperation && !NativeRequest)
    FreedRanges.emplace(IRP, IRPSize + Request->StackCount * StackSize);
  if (const auto Native = FrameworkUsbIdleRequests.find(IRP);
      Native != FrameworkUsbIdleRequests.end())
    FreedRanges.emplace(Native->second.Info, usb_idle::CallbackInfoSize);
  if (Request->RawResources) {
    FreedRanges.emplace(Request->RawResources, Request->ResourceListSize);
    FreedRanges.emplace(Request->TranslatedResources,
                        Request->ResourceListSize);
  }
  if (Request->SystemBuffer && !Request->BorrowedSystemBuffer && !NativeRequest)
    FreedRanges.emplace(Request->SystemBuffer, Request->BufferSize);
  if (Request->UserBuffer && !Request->Neither && !NativeRequest &&
      !(DriverIRPs.contains(IRP) && DriverIRPs.at(IRP).Builder))
    FreedRanges.emplace(Request->UserBuffer, Request->Direct
                                                 ? Request->TransferSize
                                                 : Request->OutputSize);
  if (Request->SecurityContext && !Request->FileOperation)
    FreedRanges.emplace(Request->SecurityContext, SecurityContextSize);
  if (NativeRequest)
    return releaseNativeFileRequestPacket(IRP);
  return llvm::Error::success();
}

llvm::Error KernelModel::recordDispatchReturn(uint64_t IRP,
                                              uint32_t DispatchStatus) {
  auto *Request = requestForIRP(IRP);
  if (!Request)
    return ioError("no active request to record dispatch return");
  auto &Observation = Result.Requests[Request->ResultIndex];
  if (Request->DispatchReturned)
    return ioError("dispatch return was already recorded for this IRP");
  Observation.DispatchStatus = DispatchStatus;
  if (!Request->Completed) {
    auto Control = Memory.readInteger(Request->Stack + StackControlOffset, 1);
    if (!Control)
      return Control.takeError();
    Request->PendingMarked = (*Control & StackPendingReturned) != 0;
  }
  // In a forwarded asynchronous request, the completion routine propagates
  // pending later. Validate that final propagation when unwinding reaches the
  // top, rather than requiring a callback that has not run yet to have marked
  // it.
  if (!(Request->Forwarded && DispatchStatus == StatusPending &&
        !Request->Completed) &&
      (DispatchStatus == StatusPending) != Request->PendingMarked)
    return ioError("STATUS_PENDING and IoMarkIrpPending must agree");
  if (DispatchStatus != StatusPending) {
    if (!Request->Completed)
      return ioError("dispatch returned without completing its IRP");
    // File API producers return a nonpending dispatch status independently
    // from its IOSB. Ordinary scenario producers retain their stricter check.
    if (!Request->Forwarded && !Request->FileRequestID &&
        Observation.IOStatus != DispatchStatus)
      return ioError("synchronous dispatch status does not match completed "
                     "IoStatus.Status");
  }
  Request->DispatchReturned = true;
  if (Framework)
    return Framework->finishRequestDispatch(IRP);
  return llvm::Error::success();
}

llvm::Error KernelModel::finalizeRequest(uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  if (!Request)
    return FinalizedRequests.count(IRP)
               ? llvm::Error::success()
               : ioError("no owned request to finalize");
  if (!Request->DispatchReturned || !Request->Completed)
    return ioError(
        "request finalization requires completion and dispatch return");
  if (Request->FileBusCompletion && !Request->FileBusReceived)
    return ioError("configured lower file response was not consumed");
  if (std::any_of(IRPCalls.begin(), IRPCalls.end(),
                  [&](const auto &Entry) { return Entry.second.IRP == IRP; }))
    return ioError(
        "request finalization requires its guest continuations to return");
  if (Request->FileOperation) {
    const uint64_t Size = IRPSize + Request->StackCount * StackSize;
    if (auto E = prepareReleaseRange(IRP, Size))
      return E;
    if (Request->SecurityContext)
      if (auto E = prepareReleaseRange(Request->SecurityContext,
                                       SecurityContextSize))
        return E;
    FreedRanges.emplace(IRP, Size);
    if (Request->SecurityContext)
      FreedRanges.emplace(Request->SecurityContext, SecurityContextSize);
  }
  if (auto Allocation = DriverIRPs.find(IRP); Allocation != DriverIRPs.end()) {
    if (!Allocation->second.StorageReleased)
      return ioError("caller request finalization requires IoFreeIrp");
    auto Route = std::move(Request->DeviceRoute);
    const uint64_t CompletionDevice = Allocation->second.CompletionDevice;
    Requests.erase(IRP);
    DriverIRPs.erase(Allocation);
    FinalizedRequests.insert(IRP);
    for (uint64_t Owner : Route)
      if (auto E = releaseDevice(Owner))
        return E;
    if (CompletionDevice)
      if (auto E = releaseDevice(CompletionDevice))
        return E;
    return snapshot();
  }
  if (auto Native = FrameworkUsbIdleRequests.find(IRP);
      Native != FrameworkUsbIdleRequests.end()) {
    if (Request->Kind != DriverRequestKind::InternalDeviceControl ||
        Native->second.Key.IRP != IRP ||
        Native->second.Key.PDO != Request->PnpDevice ||
        Native->second.Device != Request->Device ||
        Native->second.Info != Request->UserInput ||
        !FreedRanges.contains(Native->second.Info))
      return ioError("framework USB finalization lost its completed packet");
    auto Route = std::move(Request->DeviceRoute);
    Requests.erase(IRP);
    FrameworkUsbIdleRequests.erase(Native);
    FinalizedRequests.insert(IRP);
    for (uint64_t Owner : Route)
      if (auto E = releaseDevice(Owner))
        return E;
    return snapshot();
  }
  if (Request->PowerOperation) {
    if (Request->ChildPower && !Request->ChildPower->CallbackReturned)
      return ioError("power request finalization requires its callback return");
    auto Route = std::move(Request->DeviceRoute);
    Requests.erase(IRP);
    FinalizedRequests.insert(IRP);
    for (uint64_t Owner : Route)
      if (auto E = releaseDevice(Owner))
        return E;
    return snapshot();
  }
  if (Request->PnpTicket) {
    const uint64_t PDO = Request->PnpDevice;
    const bool Removed =
        Request->PnpOperation->Minor == DevicePnpRequest::Remove;
    if (Removed)
      if (auto E = validatePnpRemovalFinalization(*Request))
        return E;
    auto Route = std::move(Request->DeviceRoute);
    Requests.erase(IRP);
    FinalizedRequests.insert(IRP);
    for (uint64_t Owner : Route)
      if (auto E = releaseDevice(Owner))
        return E;
    if (Removed) {
      for (uint64_t Owner : Route)
        if (Owner != PDO && Devices.count(Owner))
          return ioError(
              "remove finalization has a retained guest device or attachment");
      if (auto E = finishPnpRemoval(PDO))
        return E;
    }
    return snapshot();
  }
  auto &Observation = Result.Requests[Request->ResultIndex];
  auto FileIt = FileObjects.find(Request->FileAddress);
  if (FileIt == FileObjects.end() ||
      FileIt->second.Address != Request->FileAddress ||
      FileIt->second.Device != Request->Device ||
      FileIt->second.PnpDevice != Request->PnpDevice)
    return ioError("active request lost its file identity");
  auto &File = FileIt->second;
  const uint64_t Device = File.Device;
  const uint32_t CompletionStatus = *Observation.IOStatus;
  if (!Request->FileRequestID && !Request->FileOperation)
    if (auto E = Memory.writeInteger(File.Address + FileFinalStatusOffset,
                                     CompletionStatus, 4))
      return E;
  if (Request->Kind == DriverRequestKind::Create) {
    if (ntSuccess(CompletionStatus)) {
      File.State = FileState::Open;
      auto Flags = Memory.readInteger(File.Address + FileFlagsOffset, 4);
      if (!Flags)
        return Flags.takeError();
      if (auto E = Memory.writeInteger(
              File.Address + FileFlagsOffset,
              *Flags |
                  (File.Source == OpenFile::Origin::Scenario ? FileHandleCreated
                                                             : 0),
              4))
        return E;
    } else {
      if (File.Source == OpenFile::Origin::Scenario) {
        const auto Identity = Files.find(Request->FileId);
        if (Identity != Files.end() && Identity->second == File.Address)
          Files.erase(Identity);
        ObjectHandles.erase(File.ScenarioHandle);
        --ReferencedObjects.at(File.Address).Handles;
        File.ScenarioHandle = 0;
      }
      if (File.OpenCancelled) {
        File.State = FileState::Cleaned;
      } else {
        if (File.ProviderOpen)
          return ioError("failed provider CREATE requires IoCancelFileOpen");
        File.State = FileState::Failed;
        File.Device = 0;
        File.PnpDevice = 0;
        if (auto E = Memory.writeInteger(File.Address + FileDeviceOffset, 0, 8))
          return E;
      }
      if (auto E = retireFileIfUnreferenced(File.Address))
        return E;
    }
  } else if (Request->Kind == DriverRequestKind::Cleanup) {
    File.State = FileState::Cleaned;
    auto Flags = Memory.readInteger(File.Address + FileFlagsOffset, 4);
    if (!Flags)
      return Flags.takeError();
    if (auto E = Memory.writeInteger(File.Address + FileFlagsOffset,
                                     *Flags | FileCleanupComplete, 4))
      return E;
  } else if (Request->Kind == DriverRequestKind::Close) {
    if (auto E = prepareReleaseRange(File.Address, FileSize))
      return E;
    if (File.NameStorage)
      if (auto E = prepareReleaseRange(File.NameStorage, File.NameStorageSize))
        return E;
    if (File.ProviderOpen) {
      if (auto E = FileSystem.closeOpen(*File.ProviderOpen))
        return E;
      File.ProviderOpen.reset();
    }
    const auto Identity = Files.find(Request->FileId);
    if (Identity != Files.end() && Identity->second == File.Address)
      Files.erase(Identity);
    FreedRanges.emplace(File.Address, FileSize);
    if (File.NameStorage)
      FreedRanges.emplace(File.NameStorage, File.NameStorageSize);
    ReferencedObjects.erase(File.Address);
    FileObjects.erase(FileIt);
  }
  // A transfer offset is an input to dispatch. Completion does not seek the
  // file; a storage operation must own any change to CurrentByteOffset.
  if (auto E = updateDeviceReferences(Device))
    return E;
  auto Route = std::move(Request->DeviceRoute);
  Requests.erase(IRP);
  FinalizedRequests.insert(IRP);
  for (uint64_t Owner : Route)
    if (auto E = releaseDevice(Owner))
      return E;
  return snapshot();
}

llvm::Expected<KernelModel::Invocation> KernelModel::beginUnload() {
  for (const auto &[ID, Device] : PnpDevices) {
    (void)ID;
    if (Device.AddDeviceActive || Devices.count(Device.PDO))
      return ioError(
          "unload requires all configured provider devices to retire");
  }
  const bool BlockingCallbacks =
      ForegroundReadyCallbacks
          ? Scheduler.hasForegroundBlockingCallbacks()
          : Scheduler.queuedCallbackCount() || Scheduler.active() ||
                Scheduler.suspendedCallbackCount();
  if (BlockingCallbacks)
    return ioError("unload requires scheduled callbacks to drain");
  const bool ScenarioOrOpeningFile = std::any_of(
      FileObjects.begin(), FileObjects.end(), [](const auto &Entry) {
        const auto &File = Entry.second;
        return File.Source != OpenFile::Origin::DriverCreate ||
               (File.State != FileState::Open &&
                File.State != FileState::Cleaned);
      });
  if (!Requests.empty() || ScenarioOrOpeningFile || !DriverIRPs.empty() ||
      !FileRequests.empty() || PendingFileRequestContinuation ||
      !FileOperations.empty() || PendingFileOperation)
    return ioError(
        "unload requires all requests completed and the file closed");
  if (Unloading || Unloaded)
    return ioError("driver unload can run only once");
  if (auto E = snapshot())
    return E;
  if (!Result.DriverUnload)
    return ioError("driver did not register an unload callback");
  Unloading = true;
  return Invocation{Result.DriverUnload, DriverObject, 0};
}

llvm::Error KernelModel::finishUnload() {
  if (!Unloading)
    return ioError("no active unload invocation");
  if (hasLiveBugCheckCallbacks())
    return ioError("unload returned with live bug-check callbacks");
  if (auto E = snapshot())
    return E;
  if (Registry.hasOpenHandles())
    return ioError("unload returned with live registry handles");
  if (!BugCheckCallbacks.empty())
    return ioError("unload returned with registered bug-check callbacks");
  if (auto E = validateExecutiveCallbackUnload())
    return E;
  if (auto E = validateProcessNotifyUnload())
    return E;
  if (auto E = validateThreadNotifyUnload())
    return E;
  if (auto E = validateRegistryCallbackUnload())
    return E;
  if (!SectionHandles.empty() || !SectionViews.empty())
    return ioError("unload returned with live section handles or mapped views");
  if (!ReservedMappings.empty())
    return ioError("unload returned with live mapping reservations");
  for (const auto &[Key, V] : ImageMappings)
    if (V.Dynamic)
      return ioError("unload returned with a live dynamic image view");
  if (auto E = validateNamedDriverUnload())
    return E;
  if (!Lookasides.empty())
    return ioError("unload returned with live lookaside lists");
  if (!ExecutiveResources.empty())
    return ioError("unload returned with live executive resources");
  if (!FastMutexHolds.empty())
    return ioError("unload returned with held fast mutexes");
  for (const auto &[Object, Thread] : ThreadObjects)
    if (Thread.System && (!Thread.Exited || WaitReferences.contains(Object)))
      return ioError("unload returned with a live system thread object");
  for (const auto &[Object, References] : ReferencedObjects)
    if (References.Pointers || References.Handles)
      return ioError("unload returned with live object references or handles "
                     "(thread handles, thread pointer references, process "
                     "pointer references or symbolic-link handles)");
  if (Framework && Framework->hasLiveBinding())
    return ioError("unload returned with a live framework binding");
  if (auto E = releaseFilterNameCache())
    return E;
  if (!NdisConfigurationHandles.empty())
    return ioError("unload returned with live NDIS configuration handles");
  if (auto E = validateAvlUnload())
    return E;
  if (auto E = validateServiceUnload())
    return E;
  if (auto E = releaseFiniteFileSystem())
    return E;
  const bool OnlyDeclaredVolumes =
      Devices.size() == FileSystem.volumeCount() &&
      std::all_of(Devices.begin(), Devices.end(), [&](const auto &Entry) {
        const auto &[Address, Device] = Entry;
        return Device.OwnerKind == DeviceOwnerKind::FileSystemProvider &&
               FileSystem.ownsDevice(Address) && !Device.Lower &&
               !Device.Upper && !Device.DeletePending &&
               !Device.UnlinkedFromDriver && !Device.InternalReferences;
      });
  if (!OnlyDeclaredVolumes || !SymbolicLinks.empty() || !Allocations.empty() ||
      !FileObjects.empty() || FileSystem.openCount() || !Requests.empty() ||
      !DriverIRPs.empty() || !FileRequests.empty() ||
      PendingFileRequestContinuation || !FileOperations.empty() ||
      PendingFileOperation || !MDLs.empty() || !WorkItems.empty() ||
      !IRPCalls.empty() || PendingWdmCall || Scheduler.hasPending() ||
      !PoFxDeviceObjects.empty() || !BlockingPoFx.empty())
    return ioError("unload returned with live devices, symbolic links, pool "
                   "allocations or file/request state");
  Unloading = false;
  Unloaded = true;
  Result.UnloadCompleted = true;
  return llvm::Error::success();
}

llvm::Error KernelModel::validateIOAccess(uint64_t Address, uint32_t Size,
                                          bool IsWrite,
                                          bool DispatcherStorage) const {
  const uint64_t End = Address + Size;
  auto Check = [&](uint64_t Object, uint64_t Length,
                   auto &&Allowed) -> llvm::Error {
    if (!Object || Address >= Object + Length || Object >= End)
      return llvm::Error::success();
    for (uint64_t Offset = std::max(Address, Object) - Object;
         Offset < std::min(End, Object + Length) - Object; ++Offset)
      if (!Allowed(Offset))
        return ioError(
            "guest access to an opaque or read-only WDM request field");
    return llvm::Error::success();
  };
  for (const auto &[Id, File] : FileObjects) {
    (void)Id;
    if (auto E = Check(File.Address, FileSize, [&](uint64_t Offset) {
          if (DispatcherStorage && File.FileEventInitialized &&
              Offset >= FileEventOffset &&
              Offset < FileEventOffset + dispatcher::EventSize)
            return true;
          if (IsWrite)
            return (Offset >= FileContextOffset &&
                    Offset < FileSectionOffset) ||
                   (File.Source == OpenFile::Origin::DriverCreate &&
                    File.State == FileState::Opening &&
                    Offset >= FileNameOffset && Offset < FileNameOffset + 16);
          return Offset < FileSectionOffset ||
                 (Offset >= FileFinalStatusOffset &&
                  Offset < FileWaitersOffset);
        }))
      return E;
  }
  for (const auto &[ID, Operation] : FileOperations) {
    (void)ID;
    if (auto E =
            Check(Operation.AccessState, 16, [](uint64_t) { return false; }))
      return E;
  }
  for (const auto &[IRP, Record] : Requests) {
    if (Record.Completed && !Record.FileOperation &&
        !nativeFileRequest(Record.FileRequestID))
      continue;
    const auto *Request = &Record;
    if (auto E = Check(Request->IRP, IRPSize, [&](uint64_t Offset) {
          if (IsWrite && Request->Completed &&
              (Request->FileOperation ||
               nativeFileRequest(Request->FileRequestID)))
            return false;
          if (IsWrite)
            return (Offset >= IRPMdlOffset &&
                    Offset < IRPMdlOffset + profile::PointerSize) ||
                   (Offset >= IRPStatusOffset &&
                    Offset < IRPRequestorModeOffset) ||
                   (Offset >= IRPCancelRoutineOffset &&
                    Offset < IRPCancelRoutineOffset + profile::PointerSize) ||
                   Offset == IRPPendingOffset || Offset == IRPLocationOffset ||
                   (Offset >= IRPStackPointerOffset &&
                    Offset < IRPStackPointerOffset + profile::PointerSize) ||
                   (Offset >= IRPDriverContextOffset &&
                    Offset < IRPThreadOffset);
          if (Offset >= IRPStatusOffset && Offset < IRPRequestorModeOffset)
            return initialized(Request->IOStatusWritten,
                               Request->IOStatusInitialization,
                               Offset - IRPStatusOffset);
          return (DriverIRPs.contains(IRP) && DriverIRPs.at(IRP).Builder &&
                  ((Offset >= IRPUserIOSBOffset &&
                    Offset < IRPUserEventOffset + 8) ||
                   (Offset >= IRPThreadOffset &&
                    Offset < IRPThreadOffset + 8))) ||
                 ((Request->FileRequestID || Request->FileOperation) &&
                  ((Offset >= IRPAllocationFlagsOffset &&
                    Offset < IRPUserEventOffset + profile::PointerSize) ||
                   (Offset >= IRPThreadOffset &&
                    Offset < IRPThreadOffset + profile::PointerSize))) ||
                 (DriverIRPs.contains(IRP) && Offset < sizeof(uint32_t)) ||
                 (Offset >= profile::PointerSize &&
                  Offset < IRPSystemBufferOffset + profile::PointerSize) ||
                 (Offset >= IRPStatusOffset && Offset <= IRPCancelIROffset) ||
                 (Offset >= IRPCancelRoutineOffset &&
                  Offset < IRPThreadOffset) ||
                 (Offset >= IRPStackPointerOffset &&
                  Offset < IRPOriginalFileOffset + profile::PointerSize);
        }))
      return E;
    // WDK CopyCurrent copies the entire prefix through FileObject, including
    // unused union bytes and padding for READ/WRITE/CLEANUP/CLOSE. These bytes
    // belong to the allocated stack record; checking only active union members
    // would reject the public inline operation before forwarding can validate
    // it.
    if (auto E = Check(Request->IRP + IRPSize, Request->StackCount * StackSize,
                       [](uint64_t) { return true; }))
      return E;
    if (auto E = Check(Request->SecurityContext, SecurityContextSize,
                       [&](uint64_t Offset) {
                         if (Request->FileOperation)
                           return !IsWrite;
                         return !IsWrite &&
                                Offset >= SecurityDesiredAccessOffset &&
                                Offset < SecurityDesiredAccessOffset + 4;
                       }))
      return E;
    for (uint64_t ResourceList :
         {Request->RawResources, Request->TranslatedResources})
      if (auto E = Check(ResourceList, Request->ResourceListSize,
                         [&](uint64_t) { return !IsWrite; }))
        return E;
    // Raw user pointers are outside this kernel-only profile. Buffered requests
    // use SystemBuffer; direct requests access the locked system mapping.
    if (!Request->Neither && !nativeFileRequest(Request->FileRequestID) &&
        !(DriverIRPs.contains(IRP) && DriverIRPs.at(IRP).Builder))
      if (auto E = Check(Request->UserBuffer,
                         Request->Direct ? Request->TransferSize
                                         : Request->OutputSize,
                         [](uint64_t) { return false; }))
        return E;
  }
  return llvm::Error::success();
}

} // namespace neverd::emulation
