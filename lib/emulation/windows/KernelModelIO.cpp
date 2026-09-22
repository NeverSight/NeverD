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

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include <algorithm>
#include <array>

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
  return 28;
}

bool ntSuccess(uint32_t Status) { return !(Status & 0x80000000U); }
bool ntError(uint32_t Status) { return (Status >> 30) == 3; }
} // namespace

KernelModel::ActiveRequest *KernelModel::requestForIRP(uint64_t IRP) {
  auto I = Requests.find(IRP);
  return I == Requests.end() ? nullptr : &I->second;
}

const KernelModel::ActiveRequest *
KernelModel::requestForIRP(uint64_t IRP) const {
  auto I = Requests.find(IRP);
  return I == Requests.end() ? nullptr : &I->second;
}

bool KernelModel::requestPending(uint64_t IRP) const {
  if (IRP) {
    const auto *Request = requestForIRP(IRP);
    return Request && Request->DispatchReturned && !Request->Completed;
  }
  return std::any_of(Requests.begin(), Requests.end(), [](const auto &Entry) {
    return Entry.second.DispatchReturned && !Entry.second.Completed;
  });
}

llvm::Error KernelModel::prepareRequestBuffers(ActiveRequest &Record,
                                               const DriverRequest &Input,
                                               uint64_t Device) {
  auto *Request = &Record;
  const bool IsRead = Input.Kind == DriverRequestKind::Read;
  const bool IsWrite = Input.Kind == DriverRequestKind::Write;
  const bool IsIOCTL = Input.Kind == DriverRequestKind::DeviceControl;
  Request->OutputSize = Input.OutputSize;
  Request->InputSize = Input.Input.size();
  Request->ByteOffset = Input.ByteOffset;
  Request->TransferSize = IsWrite ? Input.Input.size() : Input.OutputSize;
  if (IsIOCTL) {
    Request->Direct =
        (Input.ControlCode & IoControlMethodMask) != MethodBuffered;
  } else if (IsRead || IsWrite) {
    auto Flags = Memory.readInteger(Device + DeviceFlagsOffset, 4);
    if (!Flags)
      return Flags.takeError();
    const uint64_t TransferFlags = *Flags & (DeviceBufferedIO | DeviceDirectIO);
    if (TransferFlags != DeviceBufferedIO && TransferFlags != DeviceDirectIO)
      return ioError("READ/WRITE requires exactly one of DO_BUFFERED_IO or "
                     "DO_DIRECT_IO; neither I/O is not modeled");
    Request->Direct = TransferFlags == DeviceDirectIO;
  }
  Request->BufferSize = Request->Direct ? (IsIOCTL ? Input.Input.size() : 0)
                                        : std::max<uint64_t>(Input.Input.size(),
                                                             Input.OutputSize);
  if (Request->BufferSize) {
    auto Buffer = allocate(Request->BufferSize);
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
    auto Buffer = allocate(UserSize);
    if (!Buffer)
      return Buffer.takeError();
    Request->UserBuffer = *Buffer;
  }
  if (Request->Direct && Request->TransferSize) {
    llvm::ArrayRef<uint8_t> Initial = IsWrite ? Input.Input : Input.DirectInput;
    // IN_DIRECT only requires readable caller pages; it does not require a
    // read-only system mapping. Scenario buffers are readable and writable.
    // MdlMappingNoWrite independently restricts an explicitly created mapping.
    auto MDL = createRequestMDL(Request->IRP, Request->TransferSize, Initial,
                                true, Request->UserBuffer);
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
  const uint64_t Device = Request->Device;
  const uint64_t File = Request->FileAddress;
  uint32_t Flags = IRPSynchronous;
  if (Input.Kind == DriverRequestKind::Create)
    Flags |= IRPCreate;
  if (Input.Kind == DriverRequestKind::Close)
    Flags |= IRPClose;
  if (Input.Kind == DriverRequestKind::Read)
    Flags |= IRPReadOperation;
  if (Input.Kind == DriverRequestKind::Write)
    Flags |= IRPWriteOperation;
  if (Request->SystemBuffer)
    Flags |= IRPBufferedAllocation;
  if (Input.OutputSize && !Request->Direct)
    Flags |= IRPCopyOutput;
  struct Field {
    uint64_t Offset, Value;
    unsigned Size;
  };
  for (const Field &F :
       std::array<Field, 13>{{{0, IRPType, 2},
                              {2, IRPSize + StackSize, 2},
                              {IRPMdlOffset, Request->Mdl, 8},
                              {IRPFlagsOffset, Flags, 4},
                              {IRPSystemBufferOffset, Request->SystemBuffer, 8},
                              {IRPStatusOffset, 0, 4},
                              {IRPRequestorModeOffset, UserMode, 1},
                              {IRPStackCountOffset, 1, 1},
                              {IRPLocationOffset, 1, 1},
                              {IRPUserBufferOffset, Request->UserBuffer, 8},
                              {IRPStackPointerOffset, Request->Stack, 8},
                              {IRPOriginalFileOffset, File, 8},
                              {IRPSize, majorFunction(Input.Kind), 1}}})
    if (auto E = Memory.writeInteger(Packet + F.Offset, F.Value, F.Size))
      return E;
  if (auto E =
          Memory.writeInteger(Request->Stack + StackDeviceOffset, Device, 8))
    return E;
  if (auto E = Memory.writeInteger(Request->Stack + StackFileOffset, File, 8))
    return E;
  if (Input.Kind == DriverRequestKind::DeviceControl) {
    for (const Field &F :
         std::array<Field, 3>{{{StackParametersOffset, Input.OutputSize, 4},
                               {StackInputLengthOffset, Input.Input.size(), 4},
                               {StackIOControlOffset, Input.ControlCode, 4}}})
      if (auto E =
              Memory.writeInteger(Request->Stack + F.Offset, F.Value, F.Size))
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
  return llvm::Error::success();
}

llvm::Expected<KernelModel::Invocation>
KernelModel::beginRequest(const DriverRequest &Input) {
  DriverRequestResult Observation;
  Observation.Kind = Input.Kind;
  Observation.Device = Input.Device;
  Observation.ControlCode = Input.ControlCode;
  Observation.File = Input.File;
  Observation.ByteOffset = Input.ByteOffset;
  Result.Requests.push_back(std::move(Observation));
  const size_t Index = Result.Requests.size() - 1;
  if (Unloading || Unloaded ||
      std::any_of(Requests.begin(), Requests.end(), [](const auto &Entry) {
        return !Entry.second.DispatchReturned;
      }))
    return ioError(
        "cannot begin a request during another invocation or after unload");
  const unsigned Major = majorFunction(Input.Kind);
  if (Major >= 28)
    return ioError("unknown WDM request kind");
  const bool IsRead = Input.Kind == DriverRequestKind::Read;
  const bool IsWrite = Input.Kind == DriverRequestKind::Write;
  const bool IsIOCTL = Input.Kind == DriverRequestKind::DeviceControl;
  if ((!IsIOCTL && Input.ControlCode) ||
      (!IsIOCTL && !IsWrite && !Input.Input.empty()) ||
      (!IsIOCTL && !IsRead && Input.OutputSize) ||
      (!IsRead && !IsWrite && Input.ByteOffset))
    return ioError("request fields do not match its WDM major function");
  if (IsIOCTL && (Input.ControlCode & IoControlMethodMask) == MethodNeither)
    return ioError("METHOD_NEITHER requires an unsupported user address model");
  if ((!IsIOCTL ||
       (Input.ControlCode & IoControlMethodMask) == MethodBuffered) &&
      !Input.DirectInput.empty())
    return ioError("direct_input is valid only for direct IOCTLs");
  if (Input.DirectInput.size() > Input.OutputSize)
    return ioError("direct_input exceeds the second IOCTL buffer length");
  if (Input.Input.size() > profile::KernelArenaSize ||
      Input.OutputSize > profile::KernelArenaSize)
    return ioError("request buffers exceed the 1 MiB model operation limit");
  const uint64_t TransferSize = IsWrite ? Input.Input.size() : Input.OutputSize;
  if ((IsRead || IsWrite) && (Input.ByteOffset > INT64_MAX ||
                              TransferSize > INT64_MAX - Input.ByteOffset))
    return ioError("READ/WRITE byte range exceeds a nonnegative file offset");
  if (auto E = snapshot())
    return E;
  uint64_t Device = 0;
  if (Input.Device.empty()) {
    // An existing file identifies its device even when several devices exist.
    auto Existing = Files.find(Input.File);
    if (Input.Kind != DriverRequestKind::Create && Existing != Files.end())
      Device = Existing->second.Device;
    else if (Devices.size() == 1)
      Device = Devices.begin()->first;
    else
      return ioError("an unnamed request requires exactly one live device");
  } else {
    auto Resolved = resolveDeviceName(Input.Device);
    if (!Resolved)
      return Resolved.takeError();
    Device = *Resolved;
    if (!Device)
      return ioError("request device name does not identify a live device");
  }
  Result.Requests[Index].Device = Devices.at(Device).Name;
  auto FileIt = Files.find(Input.File);
  if (Input.Kind == DriverRequestKind::Create) {
    if (DeletePendingDevices.count(Device))
      return ioError("CREATE cannot target a delete-pending device");
    if (FileIt != Files.end())
      return ioError("CREATE requires an unused file identity");
    auto Flags = Memory.readInteger(Device + DeviceFlagsOffset, 4);
    if (!Flags)
      return Flags.takeError();
    if ((*Flags & DeviceExclusive) &&
        std::any_of(Files.begin(), Files.end(),
                    [&](const auto &F) { return F.second.Device == Device; }))
      return ioError("exclusive device already has a live file object");
    auto File = allocate(FileSize);
    if (!File)
      return File.takeError();
    OpenFile State;
    State.Address = *File;
    State.Device = Device;
    State.State = FileState::Opening;
    FileIt = Files.emplace(Input.File, State).first;
    struct Field {
      uint64_t Offset, Value;
      unsigned Size;
    };
    for (const Field &F : std::array<Field, 9>{{{0, FileType, 2},
                                                {2, FileSize, 2},
                                                {FileDeviceOffset, Device, 8},
                                                {FileReadAccessOffset, 1, 1},
                                                {FileWriteAccessOffset, 1, 1},
                                                {FileSharedReadOffset, 1, 1},
                                                {FileSharedWriteOffset, 1, 1},
                                                {FileFlagsOffset, FileFlags, 4},
                                                {FileNameBufferOffset, 0, 8}}})
      if (auto E = Memory.writeInteger(*File + F.Offset, F.Value, F.Size))
        return E;
  } else {
    if (FileIt == Files.end() || FileIt->second.Device != Device)
      return ioError(
          "request requires a successful CREATE on the same device and file");
    const auto State = FileIt->second.State;
    if ((Input.Kind == DriverRequestKind::Close &&
         State != FileState::Cleaned) ||
        (Input.Kind != DriverRequestKind::Close && State != FileState::Open))
      return ioError(
          "file request order must be CREATE, READ/WRITE/DEVICE_CONTROL*, "
          "CLEANUP, CLOSE");
  }
  if (std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.FileAddress == FileIt->second.Address;
      }))
    return ioError(
        "a synchronous file requires its prior request to finalize");
  auto Packet = allocate(IRPSize + StackSize);
  if (!Packet)
    return Packet.takeError();
  ActiveRequest Record{Input.Kind, Index};
  Record.IRP = *Packet;
  Record.Stack = *Packet + IRPSize;
  Record.Device = Device;
  Record.FileAddress = FileIt->second.Address;
  Record.FileId = Input.File;
  auto *Request = &Requests.emplace(*Packet, std::move(Record)).first->second;
  if (auto E = prepareRequestBuffers(*Request, Input, Device))
    return E;
  if (auto E = initializeRequestPacket(*Request, Input))
    return E;
  Result.Requests[Index].IRP = *Packet;
  const uint64_t Callback = Result.MajorFunctions[Major];
  if (Framework) {
    auto Route = Framework->routeRequest(Device, Request->IRP);
    if (!Route)
      return Route.takeError();
    if (*Route) {
      const auto &Dispatch = **Route;
      if (!Dispatch.PC) {
        if (auto E = recordDispatchReturn(*Packet, Dispatch.Status))
          return E;
        if (auto E = finalizeRequest(*Packet))
          return E;
        Invocation Call;
        Call.IRP = *Packet;
        return Call;
      }
      Invocation Call;
      Call.PC = Dispatch.PC;
      Call.FrameworkDispatchStatus = Dispatch.Status;
      Call.IRP = *Packet;
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
  if (!Callback) {
    const auto First =
        DispatchBytesWritten.begin() + Major * profile::PointerSize;
    if (std::any_of(First, First + profile::PointerSize,
                    [](bool Written) { return Written; }))
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
  Invocation Call{Callback, Device, Request->IRP};
  Call.IRP = *Packet;
  return Call;
}

llvm::Error KernelModel::completeRequest(uint64_t IRP, uint8_t PriorityBoost) {
  auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return ioError("completion requires the active IRP and cannot occur twice");
  if (PriorityBoost)
    return ioError("modeled completion supports IO_NO_INCREMENT only");
  for (unsigned I = 0; I < Request->IOStatusWritten.size(); ++I)
    if ((I < 4 || I >= 8) && !Request->IOStatusWritten[I])
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
  Observation.IOStatus = static_cast<uint32_t>(*Status);
  Observation.Information = *Information;
  const bool HasIOCTLOutput =
      Request->Kind == DriverRequestKind::DeviceControl && Request->OutputSize;
  auto Control = Memory.readInteger(Request->Stack + StackControlOffset, 1);
  if (!Control)
    return Control.takeError();
  Request->PendingMarked = (*Control & StackPendingReturned) != 0;
  // Completion retires several allocations together. Preflight every range
  // before unregistering any dispatcher state or delivering output bytes.
  std::vector<std::pair<uint64_t, uint64_t>> Retiring{
      {IRP, IRPSize + StackSize}};
  if (Request->SystemBuffer)
    Retiring.emplace_back(Request->SystemBuffer, Request->BufferSize);
  if (Request->UserBuffer)
    Retiring.emplace_back(Request->UserBuffer, Request->Direct
                                                   ? Request->TransferSize
                                                   : Request->OutputSize);
  if (Request->SecurityContext)
    Retiring.emplace_back(Request->SecurityContext, SecurityContextSize);
  if (Request->Mdl) {
    auto It = MDLs.find(Request->Mdl);
    if (It == MDLs.end())
      return ioError("active request lost ownership of its MDL");
    Retiring.emplace_back(It->second.Address, It->second.Size);
    Retiring.emplace_back(It->second.Buffer & ~(profile::PageSize - 1),
                          It->second.AllocationSize);
  }
  if (auto E = prepareReleaseRanges(Retiring))
    return E;
  if ((HasIOCTLOutput || Request->Kind == DriverRequestKind::Read) &&
      !ntError(*Status) && *Information) {
    if (Request->Direct) {
      auto Bytes = readMDLBytes(Request->Mdl, *Information);
      if (!Bytes)
        return Bytes.takeError();
      Observation.Output = std::move(*Bytes);
    } else {
      Observation.Output.resize(*Information);
      if (auto E = Memory.read(Request->SystemBuffer, Observation.Output))
        return E;
      if (auto E = Memory.write(Request->UserBuffer, Observation.Output))
        return E;
    }
  }
  if (auto E = expireRequestMDL(IRP))
    return E;
  // Completion consumes the IRP. Preserve observations now; subsequent guest
  // accesses to the packet and its system buffer are invalid even before the
  // dispatch routine returns to the session.
  Observation.Completed = true;
  Request->Completed = true;
  FreedRanges.emplace(IRP, IRPSize + StackSize);
  if (Request->SystemBuffer)
    FreedRanges.emplace(Request->SystemBuffer, Request->BufferSize);
  if (Request->UserBuffer)
    FreedRanges.emplace(Request->UserBuffer, Request->Direct
                                                 ? Request->TransferSize
                                                 : Request->OutputSize);
  if (Request->SecurityContext)
    FreedRanges.emplace(Request->SecurityContext, SecurityContextSize);
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
  if ((DispatchStatus == StatusPending) != Request->PendingMarked)
    return ioError("STATUS_PENDING and IoMarkIrpPending must agree");
  if (DispatchStatus != StatusPending) {
    if (!Request->Completed)
      return ioError("dispatch returned without completing its IRP");
    if (Observation.IOStatus != DispatchStatus)
      return ioError("synchronous dispatch status does not match completed "
                     "IoStatus.Status");
  }
  Request->DispatchReturned = true;
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
  auto &Observation = Result.Requests[Request->ResultIndex];
  auto FileIt = Files.find(Request->FileId);
  if (FileIt == Files.end() || FileIt->second.Address != Request->FileAddress ||
      FileIt->second.Device != Request->Device)
    return ioError("active request lost its file identity");
  auto &File = FileIt->second;
  const uint64_t Device = File.Device;
  const uint32_t CompletionStatus = *Observation.IOStatus;
  if (auto E = Memory.writeInteger(File.Address + FileFinalStatusOffset,
                                   CompletionStatus, 4))
    return E;
  if (Request->Kind == DriverRequestKind::Create) {
    if (ntSuccess(CompletionStatus)) {
      File.State = FileState::Open;
    } else {
      FreedRanges.emplace(File.Address, FileSize);
      Files.erase(FileIt);
    }
  } else if (Request->Kind == DriverRequestKind::Cleanup) {
    File.State = FileState::Cleaned;
    if (auto E = Memory.writeInteger(File.Address + FileFlagsOffset,
                                     FileFlags | FileCleanupComplete, 4))
      return E;
  } else if (Request->Kind == DriverRequestKind::Close) {
    FreedRanges.emplace(File.Address, FileSize);
    Files.erase(FileIt);
  } else if ((Request->Kind == DriverRequestKind::Read ||
              Request->Kind == DriverRequestKind::Write) &&
             !ntError(CompletionStatus)) {
    if (auto E =
            Memory.writeInteger(File.Address + FileCurrentByteOffset,
                                Request->ByteOffset + Observation.Information,
                                profile::PointerSize))
      return E;
  }
  if (auto E = updateDeviceReferences(Device))
    return E;
  Requests.erase(IRP);
  FinalizedRequests.insert(IRP);
  if (auto E = retireDeviceIfUnreferenced(Device))
    return E;
  return snapshot();
}

llvm::Expected<KernelModel::Invocation> KernelModel::beginUnload() {
  if (Scheduler.queuedCallbackCount() || Scheduler.active() ||
      Scheduler.suspendedCallbackCount())
    return ioError("unload requires scheduled callbacks to drain");
  if (!Requests.empty() || !Files.empty())
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
  if (auto E = snapshot())
    return E;
  if (Registry.hasOpenHandles())
    return ioError("unload returned with live registry handles");
  if (Framework && Framework->hasLiveBinding())
    return ioError("unload returned with a live framework binding");
  if (!Devices.empty() || !SymbolicLinks.empty() || !Allocations.empty() ||
      !Files.empty() || !Requests.empty() || !MDLs.empty() ||
      !WorkItems.empty() || Scheduler.hasPending())
    return ioError("unload returned with live devices, symbolic links, pool "
                   "allocations or file/request state");
  Unloading = false;
  Unloaded = true;
  Result.UnloadCompleted = true;
  return llvm::Error::success();
}

llvm::Error KernelModel::validateIOAccess(uint64_t Address, uint32_t Size,
                                          bool IsWrite) const {
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
  for (const auto &[Id, File] : Files) {
    (void)Id;
    if (auto E = Check(File.Address, FileSize, [&](uint64_t Offset) {
          if (IsWrite)
            return Offset >= FileContextOffset && Offset < FileSectionOffset;
          return Offset < FileSectionOffset ||
                 (Offset >= FileFinalStatusOffset &&
                  Offset < FileWaitersOffset);
        }))
      return E;
  }
  for (const auto &[IRP, Record] : Requests) {
    if (Record.Completed)
      continue;
    const auto *Request = &Record;
    if (auto E = Check(Request->IRP, IRPSize, [&](uint64_t Offset) {
          if (IsWrite)
            return (Offset >= IRPStatusOffset &&
                    Offset < IRPRequestorModeOffset) ||
                   Offset == IRPPendingOffset ||
                   (Offset >= IRPDriverContextOffset &&
                    Offset < IRPThreadOffset);
          if (Offset >= IRPStatusOffset && Offset < IRPRequestorModeOffset)
            return Request->IOStatusWritten[Offset - IRPStatusOffset];
          return (Offset >= profile::PointerSize &&
                  Offset < IRPSystemBufferOffset + profile::PointerSize) ||
                 (Offset >= IRPStatusOffset && Offset <= IRPCancelIROffset) ||
                 (Offset >= IRPCancelRoutineOffset &&
                  Offset < IRPThreadOffset) ||
                 (Offset >= IRPStackPointerOffset &&
                  Offset < IRPOriginalFileOffset + profile::PointerSize);
        }))
      return E;
    if (auto E = Check(Request->Stack, StackSize, [&](uint64_t Offset) {
          if (IsWrite)
            return Offset == StackControlOffset;
          if (Offset < StackParametersOffset || Offset >= StackDeviceOffset)
            return true;
          return Request->Kind == DriverRequestKind::Create ||
                 Request->Kind == DriverRequestKind::DeviceControl ||
                 ((Request->Kind == DriverRequestKind::Read ||
                   Request->Kind == DriverRequestKind::Write) &&
                  Offset < StackReadWriteByteOffset + profile::PointerSize);
        }))
      return E;
    if (auto E = Check(Request->SecurityContext, SecurityContextSize,
                       [&](uint64_t Offset) {
                         return !IsWrite &&
                                Offset >= SecurityDesiredAccessOffset &&
                                Offset < SecurityDesiredAccessOffset + 4;
                       }))
      return E;
    // Raw user pointers are outside this kernel-only profile. Buffered requests
    // use SystemBuffer; direct requests access the locked system mapping.
    if (auto E =
            Check(Request->UserBuffer,
                  Request->Direct ? Request->TransferSize : Request->OutputSize,
                  [](uint64_t) { return false; }))
      return E;
  }
  if (IsWrite)
    for (const auto &[IRP, Record] : Requests) {
      if (Record.Completed)
        continue;
      for (uint64_t Byte = std::max(Address, IRP + IRPStatusOffset);
           Byte < std::min(End, IRP + IRPRequestorModeOffset); ++Byte)
        Record.IOStatusWritten[Byte - IRP - IRPStatusOffset] = true;
    }
  return llvm::Error::success();
}

} // namespace neverd::emulation
