//===- KernelModelFileCreate.cpp - Driver file opens ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve the caller across CREATE, cancellation and final-reference CLOSE.

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <utility>

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error createError(const llvm::Twine &Text) {
  return llvm::createStringError("file create: " + Text);
}
constexpr uint32_t OpenOptions =
    0x61; // DIRECTORY, SYNCHRONOUS_NONALERT, NON_DIRECTORY.
} // namespace

std::optional<KernelModel::FileAPIContinuation>
KernelModel::takeFileAPIContinuation() {
  if (PendingFileOperation)
    return FileAPIContinuation{
        false, *std::exchange(PendingFileOperation, std::nullopt)};
  if (auto Query = takeFileRequestContinuation())
    return FileAPIContinuation{true, *Query};
  return std::nullopt;
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::resumeFileAPI(FileAPIContinuation Continuation) {
  return Continuation.Request ? resumeFileRequest(Continuation.ID)
                              : resumeFileOperation(Continuation.ID);
}

llvm::Expected<uint64_t>
KernelModel::getDeviceObjectPointer(llvm::ArrayRef<uint64_t> A,
                                    const KernelContextCapture *Context) {
  auto Entered = enterApcRegion(ApcRegionKind::Critical);
  if (!Entered)
    return Entered.takeError();
  auto Opened = createFile(A, FileOpenKind::DevicePointer, Context);
  if (Opened && PendingFileOperation) {
    // The originating thread keeps its critical region across CREATE and
    // private-handle CLEANUP, including callbacks that complete elsewhere.
    FileOperations.at(*PendingFileOperation).CriticalRegion = true;
    return Opened;
  }
  auto Left = leaveApcRegion(ApcRegionKind::Critical);
  if (!Left)
    return llvm::joinErrors(Opened.takeError(), Left.takeError());
  return Opened;
}

llvm::Expected<uint64_t>
KernelModel::createFile(llvm::ArrayRef<uint64_t> A, FileOpenKind Kind,
                        const KernelContextCapture *Context) {
  const bool OpenOnly = Kind != FileOpenKind::Create;
  const bool DevicePointer = Kind == FileOpenKind::DevicePointer;
  const auto Process = executionProcessContext();
  if (!Process || Process->RestrictedAttachment ||
      CurrentIRQL != scheduler::PassiveLevel)
    return createError("requires a known PASSIVE_LEVEL caller");
  if (auto E = validateFileAPIInterrupts(Context))
    return E;
  auto Disabled = allApcsDisabled();
  if (!Disabled)
    return Disabled.takeError();
  if (*Disabled)
    return createError("requires enabled special kernel APCs");
  if (Unloading || Unloaded || PendingWdmCall || PendingWait ||
      PendingFileOperation || PendingFileRequestContinuation ||
      NextFileOperation == UINT64_MAX || NextIRPCall == UINT64_MAX)
    return createError(
        "cannot replace an outstanding operation or open during unload");
  const uint32_t Options = DevicePointer ? 0x40 : uint32_t(A[OpenOnly ? 5 : 8]);
  const uint32_t Share = DevicePointer ? 3 : uint32_t(A[OpenOnly ? 4 : 6]);
  if ((!OpenOnly && uint32_t(A[7]) != 1) || (Options & ~OpenOptions))
    return createError("only existing-node FILE_OPEN options are implemented");
  if (((Options & 0x41) == 0x41 || Share & ~7u) && A.size() == 14 &&
      !(uint32_t(A[13]) & 0x200))
    return createError("unchecked invalid create parameters are unsupported");
  if ((Options & 0x41) == 0x41 || Share & ~7u)
    return StatusInvalidParameter;
  if (!OpenOnly && (A[9] || uint32_t(A[10])))
    return createError("extended attributes are not implemented");
  if (A.size() == 14 &&
      (uint32_t(A[11]) || A[12] || (uint32_t(A[13]) & ~0x200u)))
    return createError(
        "special create types, contexts and options are not implemented");
  uint64_t Allocation = 0;
  if (!OpenOnly && A[4]) {
    if (auto E = validateGuestAccess(A[4], 8, false))
      return E;
    auto Value = Memory.readInteger(A[4], 8);
    if (!Value)
      return Value.takeError();
    if (*Value > INT64_MAX)
      return StatusInvalidParameter;
    Allocation = *Value;
  }
  using namespace llvm::support::endian;
  std::array<uint8_t, ObjectAttributesSize> Attributes{};
  if (DevicePointer) {
    // This profile uses case-insensitive device opens. Keep the wrapper's
    // private OBJECT_ATTRIBUTES out of caller memory; the ordinary CREATE
    // owner consumes the same parsed fields.
    write32le(Attributes.data(), ObjectAttributesSize);
    write64le(Attributes.data() + ObjectNameOffset, A[0]);
    write32le(Attributes.data() + ObjectFlagsOffset,
              ObjectKernelHandle | ObjectCaseInsensitive);
  } else {
    if (auto E = validateGuestAccess(A[2], ObjectAttributesSize, false))
      return E;
    if (auto E = Memory.read(A[2], Attributes))
      return E;
  }
  if (read32le(Attributes.data()) != ObjectAttributesSize)
    return StatusInvalidParameter;
  const auto Flags = read32le(Attributes.data() + ObjectFlagsOffset);
  if ((Flags & ~(ObjectCaseInsensitive | ObjectKernelHandle)) ||
      read64le(Attributes.data() + ObjectSecurityOffset) ||
      read64le(Attributes.data() + ObjectQualityOffset))
    return createError("security descriptors, QoS and these object attributes "
                       "are unsupported");
  if (Process->ProcessID != SystemProcessID && !(Flags & ObjectKernelHandle))
    return createError("non-system callers require OBJ_KERNEL_HANDLE");
  const auto NameAddress = read64le(Attributes.data() + ObjectNameOffset);
  if (!NameAddress)
    return StatusObjectNameInvalid;
  auto Name = readObjectName(NameAddress, profile::MaxDeviceNameSize +
                                              MaxFileSystemPathBytes);
  if (!Name)
    return Name.takeError();
  auto Access = mapObjectAccess(ObjectKind::File, uint32_t(A[1]));
  if (!Access)
    return Access.takeError();
  if ((Options & 0x20) && !(*Access & 0x100000))
    return StatusInvalidParameter;
  const uint64_t RootHandle = read64le(Attributes.data() + ObjectRootOffset);
  uint64_t RootFile = 0, Device = 0;
  std::string FileName;
  if (RootHandle) {
    const auto *Handle = findObjectHandle(RootHandle);
    if (!Handle)
      return StatusInvalidHandle;
    const auto Root = FileObjects.find(Handle->Object);
    if (Root != FileObjects.end()) {
      if (!Root->second.ProviderOpen || Root->second.State != FileState::Open)
        return createError("relative open requires an open provider directory");
      if (Root->second.InternalIOHolds == UINT32_MAX)
        return createError("relative directory hold capacity exhausted");
      const auto *Node = FileSystem.nodeForOpen(*Root->second.ProviderOpen);
      if (!Node || Node->Kind != DriverFileNodeKind::Directory)
        return StatusNotADirectory;
      if (Name->empty() || Name->front() == '\\')
        return createError("empty or absolute file-root names are unsupported");
      RootFile = Root->first;
      Device = Root->second.Device;
      FileName = *Name;
    }
  }
  if (!RootFile) {
    auto Found = lookupNamedObject(*Name, RootHandle, Flags, false, true);
    if (!Found)
      return Found.takeError();
    if (Found->Status)
      return Found->Status;
    Device = Found->Object;
    FileName = std::move(Found->RemainingName);
    if (!Devices.contains(Device))
      return StatusObjectTypeMismatch;
  }
  if (!FileSystem.ownsDevice(Device))
    return createError("the named device has no declared filesystem provider");
  if (FileName.empty())
    return createError(
        "volume opens without a file-name suffix are unsupported");
  if (FileName.size() > MaxFileSystemPathBytes)
    return createError("file name exceeds the volume path limit");
  if (!DevicePointer) {
    if (auto E = validateObjectOutput(A[0], 8))
      return E;
    if (auto E = validateObjectOutput(A[3], 16))
      return E;
  }
  if (NextObjectHandle > profile::SystemThreadHandleLimit)
    return StatusInsufficientResources;
  if (auto E = snapshot())
    return E;
  auto Top = topAttachedDevice(Device);
  if (!Top)
    return Top.takeError();
  auto Route = deviceStack(*Top);
  if (!Route)
    return Route.takeError();
  for (uint64_t Member : *Route)
    if (FrameworkDevices.contains(Member))
      return createError(
          "framework filesystem CREATE dispatch is not implemented");
  auto Count = Memory.readInteger(*Top + DeviceStackCountOffset, 1);
  if (!Count)
    return Count.takeError();
  if (!*Count || *Count > MaxIRPStackCount || *Count < Route->size())
    return createError("invalid retained device stack size");
  auto Thread = currentThreadObject();
  if (!Thread)
    return Thread.takeError();
  const uint64_t ID = NextFileOperation++;
  const uint64_t Handle = NextObjectHandle;
  NextObjectHandle += profile::SystemThreadHandleStride;
  // Reserve identity before callbacks. A pending operation cannot lose its
  // handle slot to another caller; failed operations never reuse the slot.
  auto Body = allocate(FileSize);
  if (!Body)
    return Body.takeError();
  const uint64_t NameBytes = FileName.size() * 2;
  auto NameBuffer = allocate(NameBytes);
  if (!NameBuffer)
    return NameBuffer.takeError();
  auto Packet = allocate(IRPSize + *Count * StackSize);
  if (!Packet)
    return Packet.takeError();
  auto Security = allocate(SecurityContextSize);
  if (!Security)
    return Security.takeError();
  auto Storage = allocate(16);
  if (!Storage)
    return Storage.takeError();
  auto AccessState = allocate(
      16); // Opaque identity; private ACCESS_STATE fields are not modeled.
  if (!AccessState)
    return AccessState.takeError();
  OpenFile File;
  File.Source = OpenFile::Origin::DriverCreate;
  File.Address = *Body;
  File.Device = Device;
  File.Asynchronous = !(Options & 0x20);
  File.CreateOperation = ID;
  File.InternalIOHolds = 1;
  File.NameStorage = *NameBuffer;
  File.NameStorageSize = NameBytes;
  File.RelatedFile = RootFile;
  File.FileEventInitialized = true;
  FileObjects.emplace(*Body, File);
  ReferencedObjects.emplace(*Body, ObjectReferences{ObjectKind::File});
  if (RootFile)
    ++FileObjects.at(RootFile).InternalIOHolds;
  FileOperation Operation;
  Operation.DevicePointer = DevicePointer;
  Operation.File = *Body;
  Operation.IRP = *Packet;
  Operation.Root = RootFile;
  if (DevicePointer) {
    Operation.FileOutput = A[2];
    Operation.DeviceOutput = A[3];
  } else {
    Operation.HandleOutput = A[0];
    Operation.StatusOutput = A[3];
  }
  Operation.ReservedHandle = Handle;
  Operation.Storage = *Storage;
  Operation.StorageSize = 16;
  Operation.AccessState = *AccessState;
  Operation.CallerExecution = CurrentExecution;
  Operation.CallerThread = CurrentThreadKey;
  Operation.CallerProcess = Process->ProcessID;
  Operation.Access = *Access;
  Operation.Attributes = Flags & ObjectKernelHandle;
  Operation.ResultIndex = Result.Requests.size();
  FileOperations.emplace(ID, Operation);
  DriverRequestResult Observation;
  Observation.Kind = DriverRequestKind::Create;
  Observation.Origin = DriverRequestOrigin::FileCreate;
  Observation.Device = Devices.at(Device).Name;
  Observation.RequestorProcessID = Process->ProcessID;
  Observation.IRP = *Packet;
  associateFileRequest(Observation, File);
  Result.Requests.push_back(std::move(Observation));
  ActiveRequest Record{DriverRequestKind::Create, Operation.ResultIndex};
  Record.IRP = *Packet;
  Record.Device = Device;
  Record.FileAddress = *Body;
  Record.FileOperation = ID;
  Record.StackCount = *Count;
  Record.Stack = *Packet + IRPSize + (*Count - 1) * StackSize;
  Record.DeviceRoute = std::move(*Route);
  Record.UnwoundPending.resize(*Count);
  Record.SecurityContext = *Security;
  Record.AsynchronousFile = File.Asynchronous;
  Record.ProcessID = Process->ProcessID;
  Record.CallbackProcessID = Process->ProcessID;
  auto &Request = Requests.emplace(*Packet, std::move(Record)).first->second;
  for (uint64_t Member : Request.DeviceRoute)
    if (auto E = retainDevice(Member))
      return E;
  if (auto E =
          initializeIRPHeader(*Packet, *Count, *Count, Request.IOStatusWritten))
    return E;
  auto Initialized =
      Dispatcher.call("KeInitializeEvent", {*Body + FileEventOffset, 0, 0},
                      CurrentIRQL, CurrentThreadKey);
  if (!Initialized)
    return Initialized.takeError();
  std::vector<uint8_t> Bytes(NameBytes);
  for (size_t I = 0; I < FileName.size(); ++I)
    Bytes[I * 2] = uint8_t(FileName[I]);
  if (auto E = Memory.write(*NameBuffer, Bytes))
    return E;
  struct Field {
    uint64_t Address, Value;
    unsigned Width;
  };
  for (const auto &F : std::array<Field, 26>{
           {{*Body, FileType, 2},
            {*Body + 2, FileSize, 2},
            {*Body + FileDeviceOffset, Device, 8},
            {*Body + FileRelatedOffset, RootFile, 8},
            {*Body + FileFlagsOffset,
             (Options & 0x20 ? FileSynchronousIO : 0) |
                 (Flags & ObjectCaseInsensitive ? 0 : 0x800),
             4},
            {*Body + FileNameOffset, NameBytes, 2},
            {*Body + FileNameOffset + 2, NameBytes, 2},
            {*Body + FileNameBufferOffset, *NameBuffer, 8},
            {*Packet + IRPFlagsOffset, 0x884, 4},
            {*Packet + IRPThreadOffset, *Thread, 8},
            {*Packet + IRPOriginalFileOffset, *Body, 8},
            {*Packet + IRPUserIosbOffset, *Storage, 8},
            {*Packet + IRPRequestorModeOffset, KernelMode, 1},
            {*Packet + 0x58, Allocation, 8},
            {Request.Stack, 0, 1},
            {Request.Stack + StackFlagsOffset,
             Flags & ObjectCaseInsensitive ? 0u : 0x80u, 1},
            {Request.Stack + StackParametersOffset, *Security, 8},
            {Request.Stack + StackInputLengthOffset, 0x1000000 | Options, 4},
            {Request.Stack + 0x18, OpenOnly ? 0u : uint16_t(A[5]), 2},
            {Request.Stack + StackCreateShareOffset, Share, 2},
            {Request.Stack + 0x20, 0, 4},
            {Request.Stack + StackDeviceOffset, *Top, 8},
            {Request.Stack + StackFileOffset, *Body, 8},
            {*Security + 8, *AccessState, 8},
            {*Security + SecurityDesiredAccessOffset, *Access, 4},
            {*Security + 0x14, Options, 4}}})
    if (auto E = Memory.writeInteger(F.Address, F.Value, F.Width))
      return E;
  auto Watch = watchInitialization(*Packet + IRPStatusOffset,
                                   Request.IOStatusWritten.size());
  if (!Watch)
    return Watch.takeError();
  Request.IOStatusInitialization = std::move(*Watch);
  if (auto E = updateDeviceReferences(Device))
    return E;
  Invocation Call;
  Call.IRP = *Packet;
  Call.Argument0 = *Top;
  Call.Argument1 = *Packet;
  if (FileSystem.ownsDevice(*Top)) {
    auto Status = dispatchFileSystem(*Top, *Packet);
    if (!Status)
      return Status.takeError();
    if (auto E = recordDispatchReturn(*Packet, uint32_t(*Status)))
      return E;
  } else {
    auto Callback = Memory.readInteger(
        Devices.at(*Top).OwnerDriver + DriverDispatchOffset, 8);
    if (!Callback)
      return Callback.takeError();
    if (!*Callback)
      return createError(
          "filter CREATE requires a registered dispatch callback");
    Call.PC = *Callback;
  }
  if (auto E = startFileOperationDispatch(ID, Call))
    return E;
  PendingFileOperation = ID;
  return 0;
}

llvm::Error KernelModel::startFileOperationDispatch(uint64_t ID,
                                                    const Invocation &Call) {
  auto &Operation = FileOperations.at(ID);
  Operation.IRP = Call.IRP;
  if (!Call.PC)
    return llvm::Error::success();
  if (Call.FrameworkCallerContext)
    return createError(
        "framework file-operation caller contexts are unsupported");
  const uint64_t Token = NextIRPCall++;
  IRPCall Continuation{IRPCallKind::FileOperation, Call.IRP, 0, true};
  Continuation.FileOperation = ID;
  Continuation.FrameworkDispatchStatus = Call.FrameworkDispatchStatus;
  IRPCalls.emplace(Token, std::move(Continuation));
  PendingWdmCall = KernelGuestCall{{GuestCallOwner::WDM, Token},
                                   Call.PC,
                                   {Call.Argument0, Call.Argument1},
                                   Call.SynchronizationObject,
                                   Operation.CallerProcess};
  return llvm::Error::success();
}

llvm::Expected<bool> KernelModel::fileOperationReady(uint64_t ID) const {
  const auto Found = FileOperations.find(ID);
  if (Found == FileOperations.end() || Found->second.Released)
    return createError("wait lost its active operation");
  const uint64_t IRP = Found->second.IRP;
  if (FinalizedRequests.contains(IRP))
    return true;
  const auto *Request = requestForIRP(IRP);
  if (!Request)
    return createError("operation lost its packet");
  return Request->Completed && Request->DispatchReturned &&
         std::none_of(IRPCalls.begin(), IRPCalls.end(), [&](const auto &Entry) {
           return Entry.second.IRP == IRP;
         });
}

llvm::Error KernelModel::waitForFileOperation(uint64_t ID) {
  if (PendingWait)
    return createError("cannot replace an existing wait");
  const auto &Operation = FileOperations.at(ID);
  Wait Pending;
  Pending.Type = Wait::Kind::FileOperation;
  Pending.Object = ID;
  Pending.Execution = Operation.CallerExecution;
  Pending.Thread = Operation.CallerThread;
  Pending.IRQL = scheduler::PassiveLevel;
  PendingWait = std::move(Pending);
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishFileOperationDispatch(uint64_t Token, uint64_t ReturnValue) {
  const auto Call = IRPCalls.at(Token);
  if (auto E = recordDispatchReturn(Call.IRP, uint32_t(ReturnValue)))
    return E;
  IRPCalls.erase(Token);
  auto Ready = fileOperationReady(Call.FileOperation);
  if (!Ready)
    return Ready.takeError();
  if (!*Ready)
    if (auto E = waitForFileOperation(Call.FileOperation))
      return E;
  return std::optional<uint64_t>{0};
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::resumeFileOperation(uint64_t ID) {
  const auto Found = FileOperations.find(ID);
  if (Found == FileOperations.end())
    return createError("continuation lost its operation");
  auto &Operation = Found->second;
  const auto Process = executionProcessContext();
  if (CurrentExecution != Operation.CallerExecution ||
      CurrentThreadKey != Operation.CallerThread || !Process ||
      Process->ProcessID != Operation.CallerProcess ||
      CurrentIRQL != scheduler::PassiveLevel || PendingWdmCall || PendingWait)
    return createError("continuation requires the original caller context");
  if (!Operation.Released) {
    auto Ready = fileOperationReady(ID);
    if (!Ready)
      return Ready.takeError();
    if (!*Ready) {
      if (auto E = waitForFileOperation(ID))
        return E;
      return std::optional<uint64_t>{};
    }
    const auto &Observation = Result.Requests[Operation.ResultIndex];
    Operation.Status = *Observation.IOStatus;
    Operation.Information = Observation.Information;
    if (!FinalizedRequests.contains(Operation.IRP))
      if (auto E = publishFileOperationCompletion(Operation.IRP))
        return E;
    if (!Operation.CancelOpen) {
      if (auto E = Memory.writeInteger(Operation.Storage, Operation.Status, 4))
        return E;
      if (auto E = Memory.writeInteger(Operation.Storage + 8,
                                       Operation.Information, 8))
        return E;
      auto Signaled = Dispatcher.call("KeSetEvent",
                                      {Operation.File + FileEventOffset, 0, 0},
                                      CurrentIRQL, CurrentThreadKey);
      if (!Signaled)
        return Signaled.takeError();
    }
    if (!FinalizedRequests.contains(Operation.IRP))
      if (auto E = finalizeRequest(Operation.IRP))
        return E;
    auto &File = FileObjects.at(Operation.File);
    if (Operation.CancelOpen) {
      auto Reset =
          Dispatcher.call("KeResetEvent", {File.Address + FileEventOffset},
                          CurrentIRQL, CurrentThreadKey);
      if (!Reset)
        return Reset.takeError();
      File.OpenCancelled = true;
      auto Flags = Memory.readInteger(File.Address + FileFlagsOffset, 4);
      if (!Flags)
        return Flags.takeError();
      if (auto E = Memory.writeInteger(File.Address + FileFlagsOffset,
                                       *Flags | FileOpenCancelled, 4))
        return E;
    } else if (!(Operation.Status & profile::NTStatusFailureMask)) {
      if (!Operation.DevicePointer) {
        if (auto E = validateObjectOutput(Operation.HandleOutput, 8))
          return E;
        if (auto E = validateObjectOutput(Operation.StatusOutput, 16))
          return E;
      }
      retainReservedObjectHandle(Operation.ReservedHandle, File.Address,
                                 Operation.Access, Operation.Attributes,
                                 Operation.CallerProcess);
      auto Flags = Memory.readInteger(File.Address + FileFlagsOffset, 4);
      if (!Flags)
        return Flags.takeError();
      if (auto E = Memory.writeInteger(File.Address + FileFlagsOffset,
                                       *Flags | FileHandleCreated, 4))
        return E;
    }
    --File.InternalIOHolds;
    Operation.Released = true;
    if (auto E =
            retireFileIfUnreferenced(Operation.File, Operation.CallerProcess))
      return E;
    if (PendingWdmCall || PendingWait)
      return std::optional<uint64_t>{};
  }
  if (!Operation.RootReleased) {
    Operation.RootReleased = true;
    if (Operation.Root) {
      --FileObjects.at(Operation.Root).InternalIOHolds;
      if (auto E =
              retireFileIfUnreferenced(Operation.Root, Operation.CallerProcess))
        return E;
      if (PendingWdmCall || PendingWait)
        return std::optional<uint64_t>{};
    }
  }
  if (Operation.DevicePointer && !Operation.PointerPublished &&
      !(Operation.Status & profile::NTStatusFailureMask)) {
    auto Type = objectType(ObjectKind::File);
    if (!Type)
      return Type.takeError();
    // CREATE has retired its IOSB. Reuse the private storage for the wrapper's
    // local referenced object, so overlapping public outputs cannot change it.
    auto Referenced = referenceObjectByHandle(
        {Operation.ReservedHandle, 0, *Type, KernelMode, Operation.Storage, 0});
    if (!Referenced)
      return Referenced.takeError();
    Operation.Status = uint32_t(*Referenced);
    if (!(Operation.Status & profile::NTStatusFailureMask)) {
      auto File = Memory.readInteger(Operation.Storage, 8);
      if (!File)
        return File.takeError();
      if (auto E = validateObjectOutput(Operation.FileOutput, 8))
        return E;
      if (auto E = Memory.writeInteger(Operation.FileOutput, *File, 8))
        return E;
      auto Device = queryFileDevice(*File, FileDeviceQuery::Related);
      if (!Device)
        return Device.takeError();
      if (auto E = validateObjectOutput(Operation.DeviceOutput, 8))
        return E;
      if (auto E = Memory.writeInteger(Operation.DeviceOutput, *Device, 8))
        return E;
    }
    Operation.PointerPublished = true;
    auto Closed = closeHandle(Operation.ReservedHandle);
    if (!Closed)
      return Closed.takeError();
    // The wrapper ignores ZwClose's NTSTATUS, but must wait for its actual
    // CLEANUP before restoring APC state or returning the reference status.
    if (PendingWdmCall || PendingWait)
      return std::optional<uint64_t>{};
  }
  if (Operation.DevicePointer && Operation.PointerPublished) {
    const auto File = FileObjects.find(Operation.File);
    if (std::any_of(FileHandleCloses.begin(), FileHandleCloses.end(),
                    [&](const auto &Entry) {
                      return Entry.second.File == Operation.File;
                    }) ||
        (File != FileObjects.end() && File->second.State == FileState::Closing))
      return createError(
          "device open requires completed private-handle close continuation");
  }
  if (Operation.CriticalRegion) {
    auto Left = leaveApcRegion(ApcRegionKind::Critical);
    if (!Left)
      return Left.takeError();
    Operation.CriticalRegion = false;
  }
  const auto Status = Operation.CancelOpen ? 0 : Operation.Status;
  if (!Operation.CancelOpen && !Operation.DevicePointer) {
    if (!(Status & profile::NTStatusFailureMask)) {
      // The public wrapper publishes handle, Information, then Status. Preserve
      // that sequence when these caller outputs overlap.
      if (auto E = validateObjectOutput(Operation.HandleOutput, 8))
        return E;
      if (auto E = validateObjectOutput(Operation.StatusOutput, 16))
        return E;
      if (auto E = Memory.writeInteger(Operation.HandleOutput,
                                       Operation.ReservedHandle, 8))
        return E;
      if (auto E = Memory.writeInteger(Operation.StatusOutput + 8,
                                       Operation.Information, 8))
        return E;
      if (auto E = Memory.writeInteger(Operation.StatusOutput, Status, 4))
        return E;
    } else if ((Status >> profile::NTStatusSeverityShift) == 2) {
      if (auto E = validateObjectOutput(Operation.StatusOutput, 16))
        return E;
      if (auto E = Memory.writeInteger(Operation.StatusOutput, Status, 4))
        return E;
      if (auto E = Memory.writeInteger(Operation.StatusOutput + 8,
                                       Operation.Information, 8))
        return E;
    }
  }
  for (const auto &[Storage, Size] :
       {std::pair{Operation.Storage, Operation.StorageSize},
        std::pair{Operation.AccessState, uint64_t{16}}})
    if (Storage) {
      if (auto E = prepareReleaseRange(Storage, Size))
        return E;
      FreedRanges.emplace(Storage, Size);
    }
  FileOperations.erase(Found);
  return std::optional<uint64_t>{Status};
}

llvm::Expected<uint64_t>
KernelModel::cancelFileOpen(llvm::ArrayRef<uint64_t> A,
                            const KernelContextCapture *Context) {
  const auto Process = executionProcessContext();
  const auto Found = FileObjects.find(A[1]);
  if (!Process || Process->RestrictedAttachment ||
      CurrentIRQL != scheduler::PassiveLevel || Found == FileObjects.end() ||
      Found->second.Source != OpenFile::Origin::DriverCreate ||
      !Found->second.ProviderOpen ||
      (Found->second.State != FileState::Opening &&
       !Found->second.OpenCancelled))
    return createError("cancel-open requires an unpublished provider CREATE");
  if (auto E = validateFileAPIInterrupts(Context))
    return E;
  auto Disabled = allApcsDisabled();
  if (!Disabled)
    return Disabled.takeError();
  if (*Disabled)
    return createError("cancel-open requires enabled special kernel APCs");
  if (PendingWdmCall || PendingWait || PendingFileOperation ||
      PendingFileRequestContinuation || NextFileOperation == UINT64_MAX ||
      NextIRPCall == UINT64_MAX)
    return createError("cancel-open cannot replace a pending operation");
  auto &File = Found->second;
  if (File.InternalIOHolds == UINT32_MAX)
    return createError("cancel-open file hold capacity exhausted");
  const auto Parent = FileOperations.find(File.CreateOperation);
  const auto *Request = Parent != FileOperations.end()
                            ? requestForIRP(Parent->second.IRP)
                            : nullptr;
  if (!Request ||
      std::find(Request->DeviceRoute.begin(), Request->DeviceRoute.end(),
                A[0]) == Request->DeviceRoute.end())
    return createError(
        "cancel-open device is outside the retained CREATE route");
  if (ReferencedObjects.at(File.Address).Handles)
    return createError("cancel-open cannot cancel a published handle");
  const uint64_t ID = NextFileOperation++;
  FileOperation Operation;
  Operation.CancelOpen = true;
  Operation.File = File.Address;
  Operation.CallerExecution = CurrentExecution;
  Operation.CallerThread = CurrentThreadKey;
  Operation.CallerProcess = Process->ProcessID;
  Operation.ResultIndex = Result.Requests.size();
  FileOperations.emplace(ID, Operation);
  DriverRequestResult Observation;
  Observation.Kind = DriverRequestKind::Cleanup;
  Observation.Origin = DriverRequestOrigin::FileCancelOpen;
  Observation.RequestorProcessID = Process->ProcessID;
  associateFileRequest(Observation, File);
  Result.Requests.push_back(std::move(Observation));
  DriverRequest Input;
  Input.Kind = DriverRequestKind::Cleanup;
  Input.RequestorProcessID = Process->ProcessID;
  ++File.InternalIOHolds;
  auto Call = beginFileRequest(Input, std::nullopt, Operation.ResultIndex,
                               File.Address, A[0], ID);
  if (!Call)
    return Call.takeError();
  if (auto E = startFileOperationDispatch(ID, *Call))
    return E;
  PendingFileOperation = ID;
  return 0;
}

llvm::Error KernelModel::prepareCancelFileOpenPacket(uint64_t IRP,
                                                     uint64_t ID) {
  auto &Operation = FileOperations.at(ID);
  auto &Request = Requests.at(IRP);
  if (!Operation.CancelOpen || Request.Kind != DriverRequestKind::Cleanup ||
      Operation.File != Request.FileAddress)
    return createError("cancel-open lost its cleanup packet");
  auto Event = allocate(dispatcher::EventSize);
  if (!Event)
    return Event.takeError();
  Operation.IRP = IRP;
  Operation.Storage = *Event;
  Operation.StorageSize = dispatcher::EventSize;
  auto Initialized = Dispatcher.call("KeInitializeEvent", {*Event, 1, 0},
                                     CurrentIRQL, CurrentThreadKey);
  if (!Initialized)
    return Initialized.takeError();
  auto Reset =
      Dispatcher.call("KeResetEvent", {Operation.File + FileEventOffset},
                      CurrentIRQL, CurrentThreadKey);
  if (!Reset)
    return Reset.takeError();
  auto Thread = currentThreadObject();
  if (!Thread)
    return Thread.takeError();
  for (auto [Offset, Value] :
       {std::pair{IRPUserIosbOffset, IRP + IRPStatusOffset},
        std::pair{IRPUserEventOffset, *Event},
        std::pair{IRPThreadOffset, *Thread}})
    if (auto E = Memory.writeInteger(IRP + Offset, Value, 8))
      return E;
  return llvm::Error::success();
}

llvm::Error KernelModel::publishFileOperationCompletion(uint64_t IRP) {
  const auto &Request = Requests.at(IRP);
  auto &Operation = FileOperations.at(Request.FileOperation);
  if (Operation.CompletionPublished)
    return llvm::Error::success();
  uint64_t Event = Operation.Storage;
  if (!Operation.CancelOpen) {
    // The target's synchronous CREATE fast return keeps the packet and does
    // not deliver an APC notification. Pending CREATE signals the file event
    // and writes FinalStatus when the original caller can receive completion.
    if (!Request.PendingMarked) {
      Operation.CompletionPublished = true;
      return llvm::Error::success();
    }
    const auto Process = executionProcessContext();
    if (CurrentThreadKey != Operation.CallerThread ||
        CurrentIRQL != scheduler::PassiveLevel || !Process ||
        Process->RestrictedAttachment ||
        Process->ProcessID != Operation.CallerProcess)
      return llvm::Error::success();
    auto Disabled = allApcsDisabled();
    if (!Disabled)
      return Disabled.takeError();
    if (*Disabled)
      return llvm::Error::success();
    if (auto E = Memory.writeInteger(
            Operation.File + FileFinalStatusOffset,
            *Result.Requests[Request.ResultIndex].IOStatus, 4))
      return E;
    Event = Operation.File + FileEventOffset;
  }
  // IRP_CLOSE_OPERATION signals its supplied event directly, including when
  // completion runs on another thread. Its IOSB aliases the live IRP status.
  auto Signaled = Dispatcher.call("KeSetEvent", {Event, 0, 0}, CurrentIRQL,
                                  CurrentThreadKey);
  if (!Signaled)
    return Signaled.takeError();
  Operation.CompletionPublished = true;
  return llvm::Error::success();
}
} // namespace neverd::emulation
