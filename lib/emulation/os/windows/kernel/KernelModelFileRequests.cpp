//===- KernelModelFileRequests.cpp - Caller-owned file IRP continuations -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Produce borrowed queries and buffered native IRPs, resuming the API caller
/// after dispatch, completion and any final FILE_OBJECT close have returned.

#include "KernelContextCapture.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <utility>

namespace neverd::emulation {
namespace {
using namespace windows;
constexpr uint64_t QueryStatusSize = 16;
constexpr uint64_t QueryStorageSize = QueryStatusSize + dispatcher::EventSize;

uint32_t requestMajor(DriverRequestKind Kind) {
  switch (Kind) {
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Major)                      \
  case DriverRequestKind::Name:                                                \
    return Major;
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
  }
  return profile::MajorFunctionCount;
}

llvm::Error fileRequestError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "file API request: " + Text);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::queryFileDevice(uint64_t Object, FileDeviceQuery Query) const {
  const auto Found = FileObjects.find(Object);
  if (Found == FileObjects.end() || Found->second.State == FileState::Failed ||
      Found->second.State == FileState::Closing)
    return fileRequestError("device query requires a live file object");
  const auto &File = Found->second;
  auto Type = Memory.readInteger(Object, 2);
  auto Size = Memory.readInteger(Object + 2, 2);
  if (!Type || !Size)
    return llvm::joinErrors(Type.takeError(), Size.takeError());
  if (*Type != FileType || *Size != FileSize)
    return fileRequestError("FILE_OBJECT header differs from its owner");
  auto Device = Memory.readInteger(Object + FileDeviceOffset, 8);
  auto VPB = Memory.readInteger(Object + FileVPBOffset, 8);
  if (!Device || !VPB)
    return llvm::joinErrors(Device.takeError(), VPB.takeError());
  if (*Device != File.Device || !Devices.contains(*Device))
    return fileRequestError(
        "FILE_OBJECT lost its authoritative device identity");
  auto DeviceVPB = Memory.readInteger(*Device + DeviceVPBOffset, 8);
  if (!DeviceVPB)
    return DeviceVPB.takeError();
  if (*VPB || *DeviceVPB)
    return fileRequestError("mounted VPB routing is unsupported");
  if (auto E = validateDeviceTopology())
    return E;
  // The base query neither follows attachments nor consults file extensions.
  // Both query forms borrow the result without acquiring a reference.
  if (Query == FileDeviceQuery::Base)
    return *Device;
  auto Extension = Memory.readInteger(Object + FileExtensionOffset, 8);
  if (!Extension)
    return Extension.takeError();
  if (*Extension)
    return fileRequestError("file-extension routing is unsupported");
  return topAttachedDevice(*Device);
}

std::optional<uint64_t> KernelModel::takeFileRequestContinuation() {
  return std::exchange(PendingFileRequestContinuation, std::nullopt);
}

llvm::Expected<uint64_t>
KernelModel::setFileInformation(llvm::ArrayRef<uint64_t> A,
                                const KernelContextCapture *Context) {
  const auto Process = executionProcessContext();
  if (!Process || Process->RestrictedAttachment ||
      CurrentIRQL != scheduler::PassiveLevel)
    return fileRequestError("requires a known PASSIVE_LEVEL caller");
  if (auto E = validateFileAPIInterrupts(Context))
    return E;
  auto Disabled = allApcsDisabled();
  if (!Disabled)
    return Disabled.takeError();
  if (*Disabled)
    return fileRequestError("requires enabled special kernel APCs");
  if (Unloading || Unloaded || PendingWdmCall || PendingWait ||
      PendingFileRequestContinuation || PendingFileOperation)
    return fileRequestError("cannot replace an outstanding continuation");
  if (uint32_t(A[4]) != 14)
    return fileRequestError("native set-information class is not implemented");
  const auto *Handle = findObjectHandle(A[0]);
  if (!Handle)
    return StatusInvalidHandle;
  const auto Object = ReferencedObjects.find(Handle->Object);
  if (Object == ReferencedObjects.end())
    return fileRequestError("handle lost its live object");
  if (Object->second.Kind != ObjectKind::File)
    return StatusObjectTypeMismatch;
  const auto Found = FileObjects.find(Handle->Object);
  if (Found == FileObjects.end() || (Found->second.State != FileState::Open &&
                                     Found->second.State != FileState::Cleaned))
    return fileRequestError("position setting requires an opened file object");
  const auto &File = Found->second;
  auto Flags = Memory.readInteger(File.Address + FileFlagsOffset, 4);
  if (!Flags)
    return Flags.takeError();
  if (bool(*Flags & FileSynchronousIO) == File.Asynchronous)
    return fileRequestError(
        "FILE_OBJECT synchronous mode differs from its owner");
  if (File.Asynchronous)
    return fileRequestError("asynchronous position setting is not implemented");
  if (std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.FileAddress == File.Address;
      }))
    return fileRequestError(
        "same-file I/O must finalize before position setting");
  if (auto E = snapshot())
    return E;
  auto Top = queryFileDevice(File.Address, FileDeviceQuery::Related);
  if (!Top)
    return Top.takeError();
  if (uint32_t(A[3]) < 8)
    return StatusInfoLengthMismatch;
  if (auto E = validateGuestAccess(A[2], 8, false))
    return E;
  auto Position = Memory.readInteger(A[2], 8);
  auto Sector = Memory.readInteger(*Top + DeviceSectorSizeOffset, 2);
  if (!Position || !Sector)
    return llvm::joinErrors(Position.takeError(), Sector.takeError());
  if (*Position > INT64_MAX ||
      ((*Flags & 8) && *Sector && (*Position & (*Sector - 1))))
    return StatusInvalidParameter;
  // The synchronous class-14 path takes the file lock without resetting its
  // event or allocating an IRP. Capture input before aliased IOSB stores. The
  // cursor changes first; a later output fault preserves preceding effects.
  if (auto E = Memory.writeInteger(File.Address + FileCurrentByteOffset,
                                   *Position, 8))
    return E;
  if (auto E = validateObjectOutput(A[1], 4))
    return E;
  if (auto E = Memory.writeInteger(A[1], StatusSuccess, 4))
    return E;
  if (auto E = validateObjectOutput(A[1] + 8, 8))
    return E;
  if (auto E = Memory.writeInteger(A[1] + 8, 0, 8))
    return E;
  return StatusSuccess;
}

llvm::Expected<uint64_t>
KernelModel::beginFileAPIRequest(llvm::ArrayRef<uint64_t> A,
                                 DriverRequestKind Kind, bool Native,
                                 const KernelContextCapture *Context) {
  const bool Query = Kind == DriverRequestKind::QueryInformation;
  const bool Flush = Kind == DriverRequestKind::FlushBuffers;
  const bool FileSystemControl = Kind == DriverRequestKind::FileSystemControl;
  const bool Control =
      Kind == DriverRequestKind::DeviceControl || FileSystemControl;
  const bool Transfer =
      Kind == DriverRequestKind::Read || Kind == DriverRequestKind::Write;
  const bool Write = Kind == DriverRequestKind::Write;
  const auto Process = executionProcessContext();
  if (!Process || Process->RestrictedAttachment ||
      CurrentIRQL != scheduler::PassiveLevel)
    return fileRequestError("requires a known PASSIVE_LEVEL caller");
  if (auto E = validateFileAPIInterrupts(Context))
    return E;
  auto Disabled = allApcsDisabled();
  if (!Disabled)
    return Disabled.takeError();
  if (*Disabled)
    return fileRequestError("requires enabled special kernel APCs");
  if (Unloading || Unloaded || PendingWdmCall || PendingWait ||
      PendingFileRequestContinuation || PendingFileOperation ||
      NextFileRequest == UINT64_MAX || NextIRPCall == UINT64_MAX)
    return fileRequestError("cannot replace an outstanding continuation");
  if ((Transfer || Control) && (A[1] || A[2] || A[3]))
    return fileRequestError(
        "native requests with events or APCs are unsupported");
  const uint64_t OffsetPointer = Transfer ? A[7] : 0;
  const uint64_t KeyPointer = Transfer ? A[8] : 0;
  const uint64_t StatusOutput = Native ? A[Transfer || Control ? 4 : 1] : 0;
  const uint32_t ControlCode = Control ? uint32_t(A[5]) : 0;
  const uint32_t InputLength = Control ? uint32_t(A[7]) : 0;
  const uint32_t OutputLength = Control ? uint32_t(A[9]) : 0;
  const uint64_t InputPointer = Control ? A[6] : 0;
  uint32_t GrantedAccess = 0;
  std::array<uint64_t, 5> Normalized;
  if (Native) {
    const auto *Handle = findObjectHandle(A[0]);
    if (!Handle)
      return StatusInvalidHandle;
    const auto Object = ReferencedObjects.find(Handle->Object);
    if (Object == ReferencedObjects.end())
      return fileRequestError("handle lost its live object");
    if (Object->second.Kind != ObjectKind::File)
      return StatusObjectTypeMismatch;
    // Zw selects KernelMode for object referencing. The handle's process scope
    // still applies, but its granted mask is not a kernel-mode access check.
    GrantedAccess = Handle->GrantedAccess;
    Normalized =
        Flush     ? std::array<uint64_t, 5>{Handle->Object, 0, 0, 0, 0}
        : Control ? std::array<uint64_t, 5>{Handle->Object, 0,
                                            std::max(InputLength, OutputLength),
                                            A[8], 0}
        : Transfer ? std::array<uint64_t, 5>{Handle->Object, 0, uint32_t(A[6]),
                                             A[5], 0}
                   : std::array<uint64_t, 5>{Handle->Object, uint32_t(A[4]),
                                             uint32_t(A[3]), A[2], 0};
    A = Normalized;
  }
  const auto Found = FileObjects.find(A[0]);
  if (Found == FileObjects.end() || (Found->second.State != FileState::Open &&
                                     Found->second.State != FileState::Cleaned))
    return fileRequestError("requires an opened file object");
  auto &File = Found->second;
  const uint32_t Class = uint32_t(A[1]), Length = uint32_t(A[2]);
  auto Flags = Memory.readInteger(A[0] + FileFlagsOffset, 4);
  if (!Flags)
    return Flags.takeError();
  if (bool(*Flags & FileSynchronousIO) == File.Asynchronous)
    return fileRequestError(
        "FILE_OBJECT synchronous mode differs from its owner");
  if (Flush) {
    // Unlike the read/write producers, flush checks this granted mask even
    // for a KernelMode handle reference. Named pipes require FILE_WRITE_DATA.
    const uint32_t Required = (*Flags & 0x80) ? 2u : 6u;
    if (!(GrantedAccess & Required))
      return StatusAccessDenied;
    if (File.Asynchronous || File.State != FileState::Open)
      return fileRequestError("native flush requires an open synchronous file");
  }
  if (Control) {
    if (File.Asynchronous || File.State != FileState::Open)
      return fileRequestError(
          "native control requires an open synchronous file");
    if (ControlCode & 3)
      return fileRequestError("native control requires METHOD_BUFFERED");
    // These codes perform additional I/O-manager actions before ordinary
    // dispatch (offload admission, file extension state or shared accounting).
    if (ControlCode == 0x94264 || ControlCode == 0x98268 ||
        ControlCode == 0x9042c || ControlCode == 0x90020)
      return fileRequestError(
          "control code requires unmodeled manager actions");
    if (InputLength && !InputPointer)
      return fileRequestError(
          "native control requires initialized input bytes");
    auto Completion =
        Memory.readInteger(File.Address + FileCompletionContextOffset, 8);
    if (!Completion)
      return Completion.takeError();
    if (*Completion)
      return fileRequestError(
          "native control completion ports are unsupported");
  }
  uint64_t ByteOffset = 0;
  uint32_t Key = 0;
  if (Transfer) {
    if (File.Asynchronous || File.State != FileState::Open)
      return fileRequestError(
          "native transfers require an open synchronous file");
    auto Cached =
        Memory.readInteger(File.Address + FilePrivateCacheMapOffset, 8);
    auto Completion =
        Memory.readInteger(File.Address + FileCompletionContextOffset, 8);
    if (!Cached || !Completion)
      return llvm::joinErrors(Cached.takeError(), Completion.takeError());
    if (*Cached || *Completion)
      return fileRequestError(
          "cached Fast I/O and completion ports are unsupported");
    if (OffsetPointer)
      if (auto E = validateGuestAccess(OffsetPointer, 8, false))
        return E;
    auto Offset = Memory.readInteger(
        OffsetPointer ? OffsetPointer : File.Address + FileCurrentByteOffset,
        8);
    if (!Offset)
      return Offset.takeError();
    ByteOffset = *Offset;
    if (ByteOffset == uint64_t(-2)) {
      auto Position =
          Memory.readInteger(File.Address + FileCurrentByteOffset, 8);
      if (!Position)
        return Position.takeError();
      ByteOffset = *Position;
    }
    // KernelMode skips a granted-mask access check. Append-only handles still
    // select FILE_WRITE_TO_END_OF_FILE in the original write producer.
    if (Write && (GrantedAccess & 6) == 4)
      ByteOffset = UINT64_MAX;
    if (ByteOffset > INT64_MAX && !(Write && ByteOffset == UINT64_MAX))
      return StatusInvalidParameter;
    if (KeyPointer) {
      if (auto E = validateGuestAccess(KeyPointer, 4, false))
        return E;
      auto Value = Memory.readInteger(KeyPointer, 4);
      if (!Value)
        return Value.takeError();
      Key = uint32_t(*Value);
    }
  }
  // Access and remote-device observations do not take the file lock. The
  // synchronous position fast path does, even though it never creates an IRP.
  if ((Transfer || !Native || (Class != 8 && Class != 51)) &&
      std::any_of(Requests.begin(), Requests.end(), [&](const auto &Entry) {
        return Entry.second.FileAddress == A[0];
      }))
    return fileRequestError(
        "same-file I/O must finalize before a file API request");
  if (Query && Native &&
      (Class == 8 || Class == 51 || (Class == 14 && !File.Asynchronous))) {
    const unsigned Width = Class == 51 ? 1 : Class == 14 ? 8 : 4;
    if (Length < Width)
      return StatusInfoLengthMismatch;
    if (auto E = snapshot())
      return E;
    if (auto E = validateObjectOutput(A[3], Width))
      return E;
    if (auto E = validateObjectOutput(StatusOutput, QueryStatusSize))
      return E;
    uint64_t Value = GrantedAccess;
    if (Class != 8) {
      auto Top = queryFileDevice(File.Address, FileDeviceQuery::Related);
      if (!Top)
        return Top.takeError();
      auto Field = Memory.readInteger(
          Class == 14 ? File.Address + FileCurrentByteOffset
                      : File.Device + DeviceCharacteristicsOffset,
          Class == 14 ? 8 : 4);
      if (!Field)
        return Field.takeError();
      Value = Class == 14 ? *Field : bool(*Field & 0x10);
    }
    if (auto E = Memory.writeInteger(A[3], Value, Width))
      return E;
    if (auto E = Memory.writeInteger(StatusOutput, StatusSuccess, 4))
      return E;
    if (auto E = Memory.writeInteger(StatusOutput + 8, Width, 8))
      return E;
    return StatusSuccess;
  }
  if (Query && Native) {
    switch (Class) {
    case 4:
    case 5:
    case 6:
    case 7:
    case 9:
    case 14:
    case 16:
    case 17:
    case 21:
    case 22:
    case 28:
    case 34:
    case 35:
    case 59:
    case 71:
    case 75:
      break;
    default:
      return fileRequestError("native information class is not implemented");
    }
    if (!Length || ((Class == 16 || Class == 17) && Length < 4))
      return fileRequestError(
          "native buffered query requires a valid class buffer");
  }
  if (File.InternalIOHolds == UINT32_MAX)
    return fileRequestError("file I/O hold capacity exhausted");
  if (Length > profile::KernelArenaSize)
    return fileRequestError("buffer exceeds the 1 MiB model operation limit");
  if (Control) {
    if (InputLength)
      if (auto E = validateGuestAccess(InputPointer, InputLength, false))
        return E;
    if (OutputLength)
      if (auto E = validateObjectOutput(A[3], OutputLength))
        return E;
  } else if (Length)
    if (auto E = Write ? validateGuestAccess(A[3], Length, false)
                       : validateObjectOutput(A[3], Length))
      return E;
  if (auto E = validateObjectOutput(Native ? StatusOutput : A[4],
                                    Native ? QueryStatusSize : 4))
    return E;
  if (auto E = snapshot())
    return E;
  auto Top = queryFileDevice(A[0], FileDeviceQuery::Related);
  if (!Top)
    return Top.takeError();
  auto Route = deviceStack(*Top);
  if (!Route)
    return Route.takeError();
  if (auto E = validateFileSystemRoute(*Route, File.Address, Kind))
    return E;
  if (Transfer) {
    auto DeviceFlags = Memory.readInteger(*Top + DeviceFlagsOffset, 4);
    if (!DeviceFlags)
      return DeviceFlags.takeError();
    if (!(*DeviceFlags & DeviceBufferedIO))
      return fileRequestError("native transfers require buffered device I/O");
  }
  if (Control) {
    auto DeviceFlags = Memory.readInteger(*Top + DeviceFlagsOffset, 4);
    if (!DeviceFlags)
      return DeviceFlags.takeError();
    if (*DeviceFlags & 0x80000)
      return fileRequestError("native control buffered bypass is unsupported");
    if (!FileSystemControl) {
      auto FastIO = Memory.readInteger(
          Devices.at(*Top).OwnerDriver + DriverFastIOOffset, 8);
      if (!FastIO)
        return FastIO.takeError();
      if (*FastIO)
        return fileRequestError("native Fast I/O control is unsupported");
    }
  }
  auto Count = Memory.readInteger(*Top + DeviceStackCountOffset, 1);
  if (!Count)
    return Count.takeError();
  if (!*Count || *Count > MaxIRPStackCount || *Count < Route->size())
    return fileRequestError(
        "device requires a positive bounded IRP stack count");
  if (File.PnpDevice) {
    if (*Top == File.PnpDevice)
      return fileRequestError("provider PDO has no guest file dispatch");
    if (auto E = Lifecycle.validateIoSubmission(File.PnpDevice))
      return E;
  }
  const bool Provider = FileSystem.ownsDevice(*Top);
  const bool LocalNative = Query && Native && (Class == 16 || Class == 17);
  if (Query && Native && !Provider && !File.Asynchronous &&
      (Class == 4 || Class == 5)) {
    auto FastIO = Memory.readInteger(
        Devices.at(*Top).OwnerDriver + DriverFastIOOffset, 8);
    if (!FastIO)
      return FastIO.takeError();
    if (*FastIO)
      return fileRequestError("native Fast I/O queries are not implemented");
  }
  const unsigned Major = requestMajor(Kind);
  const uint64_t Callback = Provider ? 0 : Result.MajorFunctions[Major];
  if (!Provider && !LocalNative && !Callback &&
      anyInitialized(DispatchBytesWritten, DispatchInitialization,
                     Major * profile::PointerSize, profile::PointerSize))
    return fileRequestError(
        "driver explicitly registered a null file dispatch callback");
  auto Thread = currentThreadObject();
  if (!Thread)
    return Thread.takeError();
  // The synchronous file lock resets its event before IRP allocation. A
  // resource failure leaves that reset visible without changing ReturnedLength.
  if (!File.Asynchronous || Native) {
    const uint64_t FileEvent = File.Address + FileEventOffset;
    File.FileEventInitialized = true;
    auto Initialized =
        Dispatcher.isWaitable(FileEvent)
            ? Dispatcher.call("KeResetEvent", {FileEvent}, CurrentIRQL,
                              CurrentThreadKey)
            : Dispatcher.call("KeInitializeEvent", {FileEvent, 0, 0},
                              CurrentIRQL, CurrentThreadKey);
    if (!Initialized)
      return Initialized.takeError();
  }
  const uint64_t PacketSize = IRPSize + *Count * StackSize;
  const auto Align = [](uint64_t Value) {
    return (Value + PoolAlignment - 1) & ~(PoolAlignment - 1);
  };
  const uint64_t Start = Align(NextAllocation);
  if (Start > AllocationEnd || PacketSize > AllocationEnd - Start ||
      Align(Start + PacketSize) > AllocationEnd ||
      QueryStorageSize > AllocationEnd - Align(Start + PacketSize) ||
      (Native &&
       (Align(Align(Start + PacketSize) + QueryStorageSize) > AllocationEnd ||
        Length > AllocationEnd -
                     Align(Align(Start + PacketSize) + QueryStorageSize))))
    return StatusInsufficientResources;
  auto Packet = allocate(PacketSize, PoolAlignment);
  if (!Packet)
    return Packet.takeError();
  auto Storage = allocate(QueryStorageSize, PoolAlignment);
  if (!Storage)
    return Storage.takeError();
  uint64_t Buffer = A[3];
  if (Native && Length) {
    auto Owned = allocatePhysicalBuffer(Length, PoolAlignment, 0, Length);
    if (!Owned)
      return Owned.takeError();
    Buffer = *Owned;
    std::vector<uint8_t> Bytes(Length);
    if (Control && InputLength) {
      if (auto E = Memory.read(
              InputPointer,
              llvm::MutableArrayRef<uint8_t>(Bytes).take_front(InputLength)))
        return E;
    } else if (Write)
      if (auto E = Memory.read(A[3], Bytes))
        return E;
    if (auto E = Memory.write(Buffer, Bytes))
      return E;
  } else if (Native) {
    Buffer = 0;
  }
  const uint64_t ID = NextFileRequest++;
  FileRequest Operation;
  Operation.File = A[0];
  Operation.IRP = *Packet;
  Operation.ReturnedLength = A[4];
  Operation.Native = Native;
  Operation.StatusOutput = StatusOutput;
  Operation.Storage = *Storage;
  Operation.CallerExecution = CurrentExecution;
  Operation.CallerThread = CurrentThreadKey;
  Operation.CallerProcess = Process->ProcessID;
  FileRequests.emplace(ID, Operation);
  ++File.InternalIOHolds;
  DriverRequestResult Observation;
  Observation.Kind = Kind;
  Observation.Origin =
      Control
          ? (FileSystemControl ? DriverRequestOrigin::FileHandleSystemControl
                               : DriverRequestOrigin::FileHandleDeviceControl)
      : Flush    ? DriverRequestOrigin::FileHandleFlush
      : Transfer ? (Write ? DriverRequestOrigin::FileHandleWrite
                          : DriverRequestOrigin::FileHandleRead)
      : Native   ? DriverRequestOrigin::FileHandleQuery
                 : DriverRequestOrigin::FileInformationQuery;
  Observation.Device = Devices.at(File.Device).Name;
  Observation.File = File.ScenarioID;
  Observation.RequestorProcessID = Process->ProcessID;
  if (Query)
    Observation.FileInformationClass = Class;
  Observation.ByteOffset = ByteOffset;
  Observation.ControlCode = ControlCode;
  Observation.IRP = *Packet;
  associateFileRequest(Observation, File);
  if (const auto *Owner = pnpDeviceForPDO(File.PnpDevice))
    Observation.DeviceID = Result.PnpDevices[Owner->ResultIndex].ID;
  const size_t Index = Result.Requests.size();
  Result.Requests.push_back(std::move(Observation));
  ActiveRequest Record{Kind, Index};
  Record.IRP = *Packet;
  Record.Device = File.Device;
  Record.FileAddress = File.Address;
  Record.FileId = File.ScenarioID;
  Record.PnpDevice = File.PnpDevice;
  Record.LifecycleIo = File.PnpDevice != 0;
  Record.StackCount = *Count;
  Record.Stack = *Packet + IRPSize + (*Count - 1) * StackSize;
  Record.DeviceRoute = std::move(*Route);
  Record.UnwoundPending.resize(*Count);
  Record.SystemBuffer = Buffer;
  Record.BorrowedSystemBuffer = !Native;
  Record.UserBuffer = Native && !Write && Length ? A[3] : 0;
  Record.FileRequestID = ID;
  Record.BufferSize = Length;
  Record.ByteOffset = ByteOffset;
  Record.OutputSize = Write ? 0 : Length;
  Record.InputSize = Write ? Length : 0;
  Record.TransferSize = Length;
  if (Control) {
    Record.OutputSize = OutputLength;
    Record.InputSize = InputLength;
    Record.TransferSize = OutputLength;
    Record.UserInput = InputPointer;
  }
  Record.AsynchronousFile = File.Asynchronous;
  Record.ProcessID = Process->ProcessID;
  Record.CallbackProcessID = Process->ProcessID;
  auto &Request = Requests.emplace(*Packet, std::move(Record)).first->second;
  if (Request.LifecycleIo)
    if (auto E = Lifecycle.trackIo(Request.PnpDevice, *Packet))
      return E;
  for (uint64_t Owner : Request.DeviceRoute)
    if (auto E = retainDevice(Owner))
      return E;
  if (auto E =
          initializeIRPHeader(*Packet, *Count, *Count, Request.IOStatusWritten))
    return E;
  const uint64_t Event = File.Asynchronous ? *Storage + QueryStatusSize : 0;
  if (Event) {
    auto Initialized = Dispatcher.call("KeInitializeEvent", {Event, 1, 0},
                                       CurrentIRQL, CurrentThreadKey);
    if (!Initialized)
      return Initialized.takeError();
  }
  struct Field {
    uint64_t Address, Value;
    unsigned Width;
  };
  // The bounded profile uses normal I/O priority. The service tail encodes
  // that hint after producer allocation and before invoking the driver.
  const uint32_t PacketFlags =
      Control ? 0x60000u | (FileSystemControl ? 0x800u : 0u) |
                    (Length ? (OutputLength ? 0x70u : 0x30u) : 0u)
      : Flush ? 0x60000u
      : Transfer
          ? 0x60000u | (Write ? 0xa00u : 0x900u) |
                (Write ? (Length ? 0x30u : 0x10u) : (Length ? 0x70u : 0x50u)) |
                ((*Flags & 8) ? 1u : 0u)
          : (File.Asynchronous ? 0x14u : 0x10u) | (Native ? 0x860u : 0u);
  for (const Field &F : std::array<Field, 16>{
           {{*Packet + IRPFlagsOffset, PacketFlags, 4},
            {*Packet + IRPAllocationFlagsOffset, File.Asynchronous ? 0u : 2u,
             1},
            {*Packet + IRPSystemBufferOffset, Buffer, 8},
            {*Packet + IRPUserBufferOffset, Request.UserBuffer, 8},
            {*Packet + IRPRequestorModeOffset, KernelMode, 1},
            {*Packet + IRPOriginalFileOffset, File.Address, 8},
            {*Packet + IRPThreadOffset, *Thread, 8},
            {*Packet + IRPUserIosbOffset,
             Native && !File.Asynchronous ? StatusOutput : *Storage, 8},
            {*Packet + IRPUserEventOffset, Event, 8},
            {Request.Stack, Major, 1},
            {Request.Stack + StackFlagsOffset,
             Control    ? (GrantedAccess & 1) | (2 * (GrantedAccess & 2))
             : Transfer ? (Write && (*Flags & 0x10) ? 4u : 0u)
                        : (Class == 75 ? 1u : 0u),
             1},
            {Request.Stack + StackQueryLengthOffset,
             Control ? OutputLength : Length, 4},
            {Request.Stack + StackQueryClassOffset,
             Control    ? InputLength
             : Transfer ? Key
                        : (Class == 75 ? 71 : Class),
             4},
            {Request.Stack + StackReadWriteByteOffset,
             Control ? ControlCode : ByteOffset, Control ? 4u : 8u},
            {Request.Stack + StackDeviceOffset, *Top, 8},
            {Request.Stack + StackFileOffset, File.Address, 8}}})
    if (auto E = Memory.writeInteger(F.Address, F.Value, F.Width))
      return E;
  auto Watch = watchInitialization(*Packet + IRPStatusOffset,
                                   Request.IOStatusWritten.size());
  if (!Watch)
    return Watch.takeError();
  Request.IOStatusInitialization = std::move(*Watch);
  if (Framework) {
    auto Routed = Framework->routeRequest(*Top, *Packet);
    if (!Routed)
      return Routed.takeError();
    if (*Routed)
      return fileRequestError("framework file API dispatch is unsupported");
  }
  if (!File.Asynchronous) {
    auto Entered = enterApcRegion(ApcRegionKind::Critical);
    if (!Entered)
      return Entered.takeError();
    FileRequests.at(ID).CriticalRegion = true;
  }
  if (LocalNative) {
    auto Value =
        Class == 17
            ? Memory.readInteger(*Top + DeviceAlignmentOffset, 4)
            : llvm::Expected<uint64_t>(
                  ((*Flags & 0x10) ? 2u : 0u) | ((*Flags & 0x20) ? 4u : 0u) |
                  ((*Flags & 8) ? 8u : 0u) |
                  ((*Flags & 2) ? ((*Flags & 4) ? 0x10u : 0x20u) : 0u) |
                  ((*Flags & 0x10000) ? 0x1000u : 0u));
    if (!Value)
      return Value.takeError();
    if (auto E = Memory.writeInteger(Buffer, *Value, 4))
      return E;
    if (auto E = Memory.writeInteger(*Packet + IRPStatusOffset, 0, 4))
      return E;
    if (auto E = Memory.writeInteger(*Packet + IRPInformationOffset, 4, 8))
      return E;
    Request.IOStatusWritten.fill(true);
    if (auto E = completeRequest(*Packet, 0))
      return E;
    if (auto E = recordDispatchReturn(*Packet, 0))
      return E;
  } else if (Provider) {
    auto Status = dispatchFileSystem(*Top, *Packet);
    if (!Status)
      return Status.takeError();
    if (auto E = recordDispatchReturn(*Packet, uint32_t(*Status)))
      return E;
  } else if (Callback) {
    const uint64_t Token = NextIRPCall++;
    IRPCall Continuation{IRPCallKind::FileRequest, *Packet, 0, true};
    Continuation.FileRequestID = ID;
    IRPCalls.emplace(Token, std::move(Continuation));
    PendingWdmCall = KernelGuestCall{{GuestCallOwner::WDM, Token},
                                     Callback,
                                     {*Top, *Packet},
                                     0,
                                     Process->ProcessID};
  } else {
    if (auto E = Memory.writeInteger(*Packet + IRPStatusOffset,
                                     InvalidDeviceRequest, 4))
      return E;
    Request.IOStatusWritten.fill(true);
    if (auto E = completeRequest(*Packet, 0))
      return E;
    if (auto E = recordDispatchReturn(*Packet, InvalidDeviceRequest))
      return E;
  }
  PendingFileRequestContinuation = ID;
  return 0; // The session must retain the caller until resume publishes it.
}

llvm::Error
KernelModel::validateFileAPIInterrupts(const KernelContextCapture *Context) {
  if (!Context)
    return llvm::Error::success(); // Direct model calls use the default
                                   // context.
  auto Captured = Context->Read();
  if (!Captured)
    return Captured.takeError();
  const auto Flags = llvm::support::endian::read32le(Captured->Bytes.data() +
                                                     seh::ContextEFlagsOffset);
  if (!(Flags & 0x200))
    return fileRequestError(
        "interrupt-disabled file API execution is unsupported");
  return llvm::Error::success();
}

llvm::Error KernelModel::publishFileRequestCompletion(uint64_t IRP) {
  auto &Request = Requests.at(IRP);
  auto &Query = FileRequests.at(Request.FileRequestID);
  if (Query.CompletionPublished)
    return llvm::Error::success();
  // IRP_DEFER_IO_COMPLETION retains immediate native read/write/query/fsctl
  // packets until the producer regains control. Flush and device control omit
  // it: eligible completion publishes and disposes before dispatch returns.
  // Queued packets still notify through the original caller boundary.
  if (Query.Native && Request.Kind != DriverRequestKind::FlushBuffers &&
      Request.Kind != DriverRequestKind::DeviceControl &&
      !Request.PendingMarked && !Request.DispatchReturned)
    return llvm::Error::success();
  // IopfCompleteRequest queues requestor APC delivery when a worker or an
  // elevated/guarded caller completes the packet. Eligible API and execution
  // boundaries deliver the notification without retiring the query operation.
  if (CurrentThreadKey != Query.CallerThread ||
      CurrentIRQL != scheduler::PassiveLevel)
    return llvm::Error::success();
  if (auto Process = executionProcessContext();
      !Process || Process->RestrictedAttachment ||
      Process->ProcessID != Query.CallerProcess)
    return llvm::Error::success();
  auto Disabled = allApcsDisabled();
  if (!Disabled)
    return Disabled.takeError();
  if (*Disabled)
    return llvm::Error::success();
  auto &Observation = Result.Requests[Request.ResultIndex];
  const uint32_t Status = *Observation.IOStatus;
  // Completion on a worker never reads the borrowed caller address. Capture
  // report bytes only after restoring its owning process, before length aliases
  // are overwritten by the final API continuation.
  if ((Status >> profile::NTStatusSeverityShift) !=
          profile::NTStatusErrorSeverity &&
      Observation.Information &&
      (Request.Kind == DriverRequestKind::Read ||
       Request.Kind == DriverRequestKind::QueryInformation ||
       ((Request.Kind == DriverRequestKind::DeviceControl ||
         Request.Kind == DriverRequestKind::FileSystemControl) &&
        Request.OutputSize))) {
    if (auto E = validateGuestAccess(Request.SystemBuffer,
                                     Observation.Information, false))
      return E;
    Observation.Output.resize(Observation.Information);
    if (auto E = Memory.read(Request.SystemBuffer, Observation.Output))
      return E;
    if (Query.Native) {
      if (auto E =
              validateObjectOutput(Request.UserBuffer, Observation.Information))
        return E;
      if (auto E = Memory.write(Request.UserBuffer, Observation.Output))
        return E;
    }
  }
  const bool NativeLocalStatus =
      Query.Native && Request.AsynchronousFile && Request.PendingMarked;
  const uint64_t StatusBlock =
      Query.Native && !NativeLocalStatus ? Query.StatusOutput : Query.Storage;
  if (Query.Native && !Request.PendingMarked) {
    if (auto E =
            Memory.writeInteger(IRP + IRPUserIosbOffset, Query.StatusOutput, 8))
      return E;
    if (auto E = Memory.writeInteger(IRP + IRPUserEventOffset, 0, 8))
      return E;
  }
  if ((Status >> profile::NTStatusSeverityShift) !=
          profile::NTStatusErrorSeverity ||
      Request.PendingMarked) {
    if (Query.Native && !NativeLocalStatus)
      if (auto E = validateObjectOutput(StatusBlock, QueryStatusSize))
        return E;
    // The completion owner publishes Information before the four-byte Status;
    // an aliased output has already been copied by this point.
    if (auto E =
            Memory.writeInteger(StatusBlock + 8, Observation.Information, 8))
      return E;
    if (auto E = Memory.writeInteger(StatusBlock, Status, 4))
      return E;
    const bool LocalEvent =
        Request.AsynchronousFile && (!Query.Native || NativeLocalStatus);
    const uint64_t Event = LocalEvent ? Query.Storage + QueryStatusSize
                                      : Query.File + FileEventOffset;
    auto Signaled = Dispatcher.call("KeSetEvent", {Event, 0, 0}, CurrentIRQL,
                                    CurrentThreadKey);
    if (!Signaled)
      return Signaled.takeError();
    if (!LocalEvent)
      if (auto E = Memory.writeInteger(Query.File + FileFinalStatusOffset,
                                       Status, 4))
        return E;
  }
  Query.CompletionPublished = true;
  if (Query.Native && Request.Completed)
    return releaseNativeFileRequestPacket(IRP);
  return llvm::Error::success();
}

bool KernelModel::nativeFileRequest(uint64_t ID) const {
  return ID && FileRequests.at(ID).Native;
}

llvm::Error KernelModel::releaseNativeFileRequestPacket(uint64_t IRP) {
  const auto &Request = Requests.at(IRP);
  auto &Query = FileRequests.at(Request.FileRequestID);
  if (!Query.Native || Query.PacketReleased || !Query.CompletionPublished)
    return llvm::Error::success();
  if (!Request.Completed)
    return fileRequestError(
        "native disposal requires completed stack unwinding");
  const uint64_t PacketSize = IRPSize + Request.StackCount * StackSize;
  std::vector<std::pair<uint64_t, uint64_t>> Ranges{{IRP, PacketSize}};
  if (Request.SystemBuffer)
    Ranges.emplace_back(Request.SystemBuffer, Request.BufferSize);
  if (auto E = prepareReleaseRanges(Ranges))
    return E;
  if (Request.SystemBuffer) {
    if (auto E = Physical.retire(Request.SystemBuffer))
      return E;
    FreedRanges.emplace(Request.SystemBuffer, Request.BufferSize);
  }
  FreedRanges.emplace(IRP, PacketSize);
  Query.PacketReleased = true;
  return llvm::Error::success();
}

llvm::Error KernelModel::deliverFileRequestCompletions(
    const KernelContextCapture *Context) {
  for (const auto &[ID, Query] : FileRequests) {
    (void)ID;
    if (Query.CompletionPublished || Query.Released ||
        Query.CallerThread != CurrentThreadKey)
      continue;
    const auto *Request = requestForIRP(Query.IRP);
    if (!Request || !Request->Completed)
      continue;
    if (Context) {
      auto Captured = Context->Read();
      if (!Captured)
        return Captured.takeError();
      if (!(llvm::support::endian::read32le(Captured->Bytes.data() +
                                            seh::ContextEFlagsOffset) &
            0x200))
        return llvm::Error::success();
    }
    if (auto E = publishFileRequestCompletion(Query.IRP))
      return E;
  }
  return llvm::Error::success();
}

llvm::Expected<bool> KernelModel::fileRequestReady(uint64_t ID) const {
  const auto Found = FileRequests.find(ID);
  if (Found == FileRequests.end() || Found->second.Released)
    return fileRequestError("wait lost its active file request");
  const auto *Request = requestForIRP(Found->second.IRP);
  if (!Request)
    return fileRequestError("operation lost its file IRP");
  return Request->Completed && Request->DispatchReturned &&
         std::none_of(IRPCalls.begin(), IRPCalls.end(), [&](const auto &Entry) {
           return Entry.second.IRP == Found->second.IRP;
         });
}

llvm::Error KernelModel::waitForFileRequest(uint64_t ID) {
  if (PendingWait)
    return fileRequestError("cannot replace an existing wait");
  const auto &Query = FileRequests.at(ID);
  Wait Pending;
  Pending.Type = Wait::Kind::FileRequest;
  Pending.Object = ID;
  Pending.Execution = Query.CallerExecution;
  Pending.Thread = Query.CallerThread;
  Pending.IRQL = scheduler::PassiveLevel;
  PendingWait = std::move(Pending);
  return llvm::Error::success();
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishFileRequestDispatch(uint64_t Token, uint64_t ReturnValue) {
  const auto Call = IRPCalls.at(Token);
  if (auto E = recordDispatchReturn(Call.IRP, uint32_t(ReturnValue)))
    return E;
  IRPCalls.erase(Token);
  auto Ready = fileRequestReady(Call.FileRequestID);
  if (!Ready)
    return Ready.takeError();
  if (!*Ready)
    if (auto E = waitForFileRequest(Call.FileRequestID))
      return E;
  return std::optional<uint64_t>{0}; // Never publish from the dispatch frame.
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::resumeFileRequest(uint64_t ID) {
  const auto Found = FileRequests.find(ID);
  if (Found == FileRequests.end())
    return fileRequestError("continuation lost its operation");
  auto &Query = Found->second;
  const auto Process = executionProcessContext();
  if (CurrentExecution != Query.CallerExecution ||
      CurrentThreadKey != Query.CallerThread || !Process ||
      Process->ProcessID != Query.CallerProcess ||
      CurrentIRQL != scheduler::PassiveLevel || PendingWdmCall || PendingWait)
    return fileRequestError(
        "continuation requires the original caller context");
  if (!Query.Released) {
    auto Ready = fileRequestReady(ID);
    if (!Ready)
      return Ready.takeError();
    if (!*Ready) {
      if (auto E = waitForFileRequest(ID))
        return E;
      return std::optional<uint64_t>{};
    }
    if (auto E = publishFileRequestCompletion(Query.IRP))
      return E;
    if (!Query.CompletionPublished)
      return fileRequestError("caller cannot deliver its file completion APC");
    const auto &Observation =
        Result.Requests[Requests.at(Query.IRP).ResultIndex];
    Query.Status = *Observation.DispatchStatus == StatusPending
                       ? *Observation.IOStatus
                       : *Observation.DispatchStatus;
    if (Query.Native) {
      Query.CopyStatusBlock = Requests.at(Query.IRP).AsynchronousFile &&
                              *Observation.DispatchStatus == StatusPending;
      if (Query.CopyStatusBlock)
        if (auto E = Memory.read(Query.Storage, Query.StatusBytes))
          return E;
    } else {
      auto Information = Memory.readInteger(Query.Storage + 8, 8);
      if (!Information)
        return Information.takeError();
      Query.Information = uint32_t(*Information);
      if (auto E = validateObjectOutput(Query.ReturnedLength, 4))
        return E;
    }
    if (auto E = prepareReleaseRange(Query.Storage, QueryStorageSize))
      return E;
    if (auto E = finalizeRequest(Query.IRP))
      return E;
    FreedRanges.emplace(Query.Storage, QueryStorageSize);
    --FileObjects.at(Query.File).InternalIOHolds;
    Query.Released = true;
    if (auto E = retireFileIfUnreferenced(Query.File, Query.CallerProcess))
      return E;
    if (PendingWdmCall || PendingWait)
      return std::optional<uint64_t>{};
  }
  // Completion or final CLOSE can have run on another execution. Validate at
  // the publication point, including aliases into the borrowed output buffer.
  if (Query.CriticalRegion) {
    auto Left = leaveApcRegion(ApcRegionKind::Critical);
    if (!Left)
      return Left.takeError();
    Query.CriticalRegion = false;
  }
  if (Query.CopyStatusBlock) {
    if (auto E = validateObjectOutput(Query.StatusOutput, QueryStatusSize))
      return E;
    if (auto E = Memory.write(Query.StatusOutput, Query.StatusBytes))
      return E;
  } else if (!Query.Native) {
    if (auto E = validateObjectOutput(Query.ReturnedLength, 4))
      return E;
    if (auto E =
            Memory.writeInteger(Query.ReturnedLength, Query.Information, 4))
      return E;
  }
  const uint64_t Status = Query.Status;
  FileRequests.erase(Found);
  return std::optional<uint64_t>{Status};
}
} // namespace neverd::emulation
