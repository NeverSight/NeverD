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

llvm::Expected<KernelModel::Invocation>
KernelModel::beginRequest(const DriverRequest &Input) {
  DriverRequestResult Observation;
  Observation.Kind = Input.Kind;
  Observation.Device = Input.Device;
  Observation.ControlCode = Input.ControlCode;
  Result.Requests.push_back(std::move(Observation));
  const size_t Index = Result.Requests.size() - 1;
  if (Request || Unloading || Unloaded)
    return ioError(
        "cannot begin a request during another invocation or after unload");
  const unsigned Major = majorFunction(Input.Kind);
  if (Major >= 28)
    return ioError("unknown WDM request kind");
  if (Input.Kind != DriverRequestKind::DeviceControl &&
      (Input.ControlCode || !Input.Input.empty() || Input.OutputSize))
    return ioError("only DEVICE_CONTROL accepts an IOCTL and buffers");
  if (Input.Kind == DriverRequestKind::DeviceControl && (Input.ControlCode & 3))
    return ioError(
        "only METHOD_BUFFERED IOCTLs are supported; direct and neither "
        "transfer methods require a separate memory model");
  if (Input.Input.size() > profile::KernelArenaSize ||
      Input.OutputSize > profile::KernelArenaSize)
    return ioError("request buffers exceed the 1 MiB model operation limit");
  if (auto E = snapshot())
    return E;
  uint64_t Device = 0;
  if (Input.Device.empty()) {
    if (Devices.size() != 1)
      return ioError("an unnamed request requires exactly one live device");
    Device = Devices.begin()->first;
  } else {
    if (!std::all_of(Input.Device.begin(), Input.Device.end(),
                     [](unsigned char C) { return C >= 0x20 && C <= 0x7e; }))
      return ioError("device selection requires an exact printable ASCII name");
    for (const auto &[Address, Object] : Devices)
      if (Object.Name == Input.Device)
        Device = Address;
    if (!Device)
      return ioError("request device name does not identify a live device");
  }
  Result.Requests[Index].Device = Devices.at(Device).Name;
  if (Input.Kind == DriverRequestKind::Create) {
    if (OpenState != FileState::Closed)
      return ioError("CREATE requires no existing open file object");
  } else {
    if (!FileObject || FileDevice != Device)
      return ioError("request requires a successful CREATE on the same device");
    if ((Input.Kind == DriverRequestKind::Close &&
         OpenState != FileState::Cleaned) ||
        (Input.Kind != DriverRequestKind::Close &&
         OpenState != FileState::Open))
      return ioError(
          "request order must be CREATE, DEVICE_CONTROL*, CLEANUP, CLOSE");
  }
  if (Input.Kind == DriverRequestKind::Create) {
    auto File = allocate(FileSize);
    if (!File)
      return File.takeError();
    FileObject = *File;
    FileDevice = Device;
    OpenState = FileState::Opening;
    struct Field {
      uint64_t Offset, Value;
      unsigned Size;
    };
    for (const Field &F : std::array<Field, 9>{
             {{0, FileType, 2},
              {2, FileSize, 2},
              {FileDeviceOffset, Device, 8},
              {FileReadAccessOffset, 1, 1},
              {FileWriteAccessOffset, 1, 1}, // read and write access.
              {FileSharedReadOffset, 1, 1},
              {FileSharedWriteOffset, 1, 1}, // shared read and write.
              {FileFlagsOffset, FileFlags, 4},
              {FileNameBufferOffset, 0, 8}}})
      if (auto E = Memory.writeInteger(FileObject + F.Offset, F.Value, F.Size))
        return E;
  }
  Request = ActiveRequest{Input.Kind, Index};
  auto Packet = allocate(IRPSize + StackSize);
  if (!Packet)
    return Packet.takeError();
  Request->IRP = *Packet;
  Request->Stack = *Packet + IRPSize;
  Result.Requests[Index].IRP = *Packet;
  Request->OutputSize = Input.OutputSize;
  Request->BufferSize =
      std::max<uint64_t>(Input.Input.size(), Input.OutputSize);
  if (Request->BufferSize) {
    auto Buffer = allocate(Request->BufferSize);
    if (!Buffer)
      return Buffer.takeError();
    Request->SystemBuffer = *Buffer;
    if (auto E = Memory.write(*Buffer, Input.Input))
      return E;
  }
  if (Input.OutputSize) {
    auto Buffer = allocate(Input.OutputSize);
    if (!Buffer)
      return Buffer.takeError();
    Request->UserBuffer = *Buffer;
  }
  uint32_t Flags = IRPSynchronous; // IRP_SYNCHRONOUS_API.
  if (Input.Kind == DriverRequestKind::Create)
    Flags |= IRPCreate;
  if (Input.Kind == DriverRequestKind::Close)
    Flags |= IRPClose;
  if (Request->SystemBuffer)
    Flags |= IRPBufferedAllocation; // IRP_BUFFERED_IO | IRP_DEALLOCATE_BUFFER.
  if (Input.OutputSize)
    Flags |= IRPCopyOutput; // IRP_INPUT_OPERATION: copy output at completion.
  struct Field {
    uint64_t Offset, Value;
    unsigned Size;
  };
  for (const Field &F : std::array<Field, 12>{
           {{0, IRPType, 2},
            {2, IRPSize + StackSize, 2},
            {IRPFlagsOffset, Flags, 4},
            {IRPSystemBufferOffset, Request->SystemBuffer, 8},
            {IRPStatusOffset, 0, 4},
            {IRPRequestorModeOffset, UserMode,
             1}, // UserMode requester; no ETHREAD is synthesized.
            {IRPStackCountOffset, 1, 1},
            {IRPLocationOffset, 1, 1},
            {IRPUserBufferOffset, Request->UserBuffer, 8},
            {IRPStackPointerOffset, Request->Stack, 8},
            {IRPOriginalFileOffset, FileObject, 8},
            {IRPSize, Major, 1}}})
    if (auto E = Memory.writeInteger(*Packet + F.Offset, F.Value, F.Size))
      return E;
  if (auto E =
          Memory.writeInteger(Request->Stack + StackDeviceOffset, Device, 8))
    return E;
  if (auto E =
          Memory.writeInteger(Request->Stack + StackFileOffset, FileObject, 8))
    return E;
  if (Input.Kind == DriverRequestKind::DeviceControl) {
    for (const Field &F :
         std::array<Field, 3>{{{StackParametersOffset, Input.OutputSize, 4},
                               {StackInputLengthOffset, Input.Input.size(), 4},
                               {StackIOControlOffset, Input.ControlCode, 4}}})
      if (auto E =
              Memory.writeInteger(Request->Stack + F.Offset, F.Value, F.Size))
        return E;
  } else if (Input.Kind == DriverRequestKind::Create) {
    auto Security = allocate(SecurityContextSize);
    if (!Security)
      return Security.takeError();
    Request->SecurityContext = *Security;
    // This profile opens the device itself synchronously for read/write, with
    // no relative name, EA data, security impersonation or custom create flags.
    if (auto E = Memory.writeInteger(*Security + SecurityDesiredAccessOffset,
                                     CreateDesiredAccess, 4))
      return E;
    if (auto E = Memory.writeInteger(Request->Stack + StackParametersOffset,
                                     *Security, 8))
      return E;
    if (auto E = Memory.writeInteger(Request->Stack + StackInputLengthOffset,
                                     CreateOptions, 4))
      return E; // FILE_OPEN | FILE_SYNCHRONOUS_IO_NONALERT.
    if (auto E = Memory.writeInteger(Request->Stack + StackCreateShareOffset,
                                     ShareReadWrite, 2))
      return E; // FILE_SHARE_READ | FILE_SHARE_WRITE.
  }
  const uint64_t Callback = Result.MajorFunctions[Major];
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
    if (auto E = finishRequest(InvalidDeviceRequest))
      return E;
    return Invocation{}; // Explicit modeled default dispatch; no guest PC.
  }
  return Invocation{Callback, Device, Request->IRP};
}

llvm::Error KernelModel::completeRequest(uint64_t IRP, uint8_t PriorityBoost) {
  if (!Request || Request->IRP != IRP || Request->Completed)
    return ioError("completion requires the active IRP and cannot occur twice");
  if (PriorityBoost)
    return ioError("synchronous completion supports IO_NO_INCREMENT only");
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
  auto &Observation = Result.Requests[Request->ResultIndex];
  Observation.IOStatus = static_cast<uint32_t>(*Status);
  Observation.Information = *Information;
  auto Pending = Memory.readInteger(IRP + IRPPendingOffset, 1);
  if (!Pending)
    return Pending.takeError();
  auto Control = Memory.readInteger(Request->Stack + StackControlOffset, 1);
  if (!Control)
    return Control.takeError();
  if (*Status == StatusPending || *Pending || *Control)
    return ioError("pending or asynchronous IRP completion is unsupported");
  if (Request->Kind == DriverRequestKind::DeviceControl &&
      *Information > Request->OutputSize)
    return ioError("IoStatus.Information exceeds the requested output buffer");
  if ((Request->Kind == DriverRequestKind::Cleanup ||
       Request->Kind == DriverRequestKind::Close) &&
      *Information)
    return ioError("CLEANUP and CLOSE require zero IoStatus.Information");
  if (Request->Kind == DriverRequestKind::Create &&
      *Information > MaxCreateInformation)
    return ioError(
        "CREATE IoStatus.Information is not a defined create result");
  if (Request->Kind == DriverRequestKind::DeviceControl && !ntError(*Status) &&
      *Information) {
    Observation.Output.resize(*Information);
    if (auto E = Memory.read(Request->SystemBuffer, Observation.Output))
      return E;
    if (auto E = Memory.write(Request->UserBuffer, Observation.Output))
      return E;
  }
  // Completion consumes the IRP. Preserve observations now; subsequent guest
  // accesses to the packet and its system buffer are invalid even before the
  // dispatch routine returns to the session.
  Observation.Completed = true;
  Request->Completed = true;
  FreedRanges.emplace(IRP, IRPSize + StackSize);
  if (Request->SystemBuffer)
    FreedRanges.emplace(Request->SystemBuffer, Request->BufferSize);
  if (Request->UserBuffer)
    FreedRanges.emplace(Request->UserBuffer, Request->OutputSize);
  if (Request->SecurityContext)
    FreedRanges.emplace(Request->SecurityContext, SecurityContextSize);
  return llvm::Error::success();
}

llvm::Error KernelModel::finishRequest(uint32_t DispatchStatus) {
  if (!Request)
    return ioError("no active request to finish");
  auto &Observation = Result.Requests[Request->ResultIndex];
  Observation.DispatchStatus = DispatchStatus;
  if (DispatchStatus == StatusPending)
    return ioError("STATUS_PENDING requires unsupported asynchronous dispatch");
  if (!Request->Completed)
    return ioError("dispatch returned without completing its IRP");
  if (Observation.IOStatus != DispatchStatus)
    return ioError(
        "synchronous dispatch status does not match completed IoStatus.Status");
  if (auto E = Memory.writeInteger(FileObject + FileFinalStatusOffset,
                                   DispatchStatus, 4))
    return E;
  if (Request->Kind == DriverRequestKind::Create) {
    if (ntSuccess(DispatchStatus)) {
      OpenState = FileState::Open;
      if (auto E = Memory.writeInteger(FileDevice + DeviceReferenceCount, 1, 4))
        return E;
    } else {
      FreedRanges.emplace(FileObject, FileSize);
      FileObject = FileDevice = 0;
      OpenState = FileState::Closed;
    }
  } else if (Request->Kind == DriverRequestKind::Cleanup) {
    OpenState = FileState::Cleaned;
    if (auto E = Memory.writeInteger(FileDevice + DeviceReferenceCount, 0, 4))
      return E;
    if (auto E = Memory.writeInteger(FileObject + FileFlagsOffset,
                                     FileFlags | FileCleanupComplete, 4))
      return E;
  } else if (Request->Kind == DriverRequestKind::Close) {
    FreedRanges.emplace(FileObject, FileSize);
    FileObject = FileDevice = 0;
    OpenState = FileState::Closed;
  }
  Request.reset();
  return snapshot();
}

llvm::Expected<KernelModel::Invocation> KernelModel::beginUnload() {
  if (Request || FileObject || OpenState != FileState::Closed)
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
  if (!Devices.empty() || !SymbolicLinks.empty() || !Allocations.empty() ||
      FileObject || Request)
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
  if (auto E = Check(FileObject, FileSize, [&](uint64_t Offset) {
        if (IsWrite)
          return Offset >= FileContextOffset && Offset < FileSectionOffset;
        return Offset < FileSectionOffset ||
               (Offset >= FileFinalStatusOffset && Offset < FileWaitersOffset);
      }))
    return E;
  if (!Request)
    return llvm::Error::success();
  if (auto E = Check(Request->IRP, IRPSize, [&](uint64_t Offset) {
        if (IsWrite)
          return (Offset >= IRPStatusOffset &&
                  Offset < IRPRequestorModeOffset) ||
                 Offset == IRPPendingOffset ||
                 (Offset >= IRPDriverContextOffset && Offset < IRPThreadOffset);
        if (Offset >= IRPStatusOffset && Offset < IRPRequestorModeOffset)
          return Request->IOStatusWritten[Offset - IRPStatusOffset];
        return (Offset >= profile::PointerSize &&
                Offset < IRPSystemBufferOffset + profile::PointerSize) ||
               (Offset >= IRPStatusOffset && Offset <= IRPCancelIROffset) ||
               (Offset >= IRPCancelRoutineOffset && Offset < IRPThreadOffset) ||
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
               Request->Kind == DriverRequestKind::DeviceControl;
      }))
    return E;
  if (auto E = Check(
          Request->SecurityContext, SecurityContextSize, [&](uint64_t Offset) {
            return !IsWrite && Offset >= SecurityDesiredAccessOffset &&
                   Offset < SecurityDesiredAccessOffset + 4;
          }))
    return E;
  // Buffered I/O exposes a user pointer in the packet, but only SystemBuffer
  // is a legal driver buffer. Copies to UserBuffer belong to completion.
  if (auto E = Check(Request->UserBuffer, Request->OutputSize,
                     [](uint64_t) { return false; }))
    return E;
  if (IsWrite)
    for (uint64_t Byte = std::max(Address, Request->IRP + IRPStatusOffset);
         Byte < std::min(End, Request->IRP + IRPRequestorModeOffset); ++Byte)
      Request->IOStatusWritten[Byte - Request->IRP - IRPStatusOffset] = true;
  return llvm::Error::success();
}

} // namespace neverd::emulation
