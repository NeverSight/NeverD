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

#include "llvm/Support/ErrorHandling.h"

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
unsigned userPermissions(DriverUserPageAccess Access) {
  switch (Access) {
  case DriverUserPageAccess::ReadWrite:
    return Read | Write;
  case DriverUserPageAccess::ReadOnly:
    return Read;
  case DriverUserPageAccess::NoAccess:
    return 0;
  }
  llvm_unreachable("invalid driver user page access");
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::allocateUserBuffer(uint32_t Size, llvm::ArrayRef<uint8_t> Initial,
                                DriverUserPageAccess Access,
                                uint32_t ProcessID) {
  if (!Size)
    return 0;
  if (Initial.size() > Size || Size > profile::KernelArenaSize)
    return ioError("invalid synthetic user buffer extent");
  const uint64_t Pages =
      (uint64_t(Size) + profile::PageSize - 1) & ~(profile::PageSize - 1);
  const uint64_t End = profile::UserArenaBase + profile::UserArenaSize;
  if (NextUserAddress > End || Pages > End - NextUserAddress)
    return ioError("synthetic user address space exhausted");
  const uint64_t Address = NextUserAddress;
  if (auto E = Memory.map(Address, Pages, Read | Write))
    return std::move(E);
  if (auto E = Physical.registerRegion(Address, Address, Size))
    return std::move(E);
  if (auto E = Memory.write(Address, Initial))
    return std::move(E);
  if (auto E = Memory.protect(Address, Pages, userPermissions(Access)))
    return std::move(E);
  UserAllocations.emplace(Address, UserAllocation{Size, ProcessID, Access});
  NextUserAddress += Pages;
  return Address;
}

llvm::Error KernelModel::setUserRequestContext(bool Active,
                                               uint32_t ProcessID) {
  if (Active) {
    for (const auto &[Address, Allocation] : UserAllocations) {
      const uint64_t Pages =
          (Allocation.Size + profile::PageSize - 1) & ~(profile::PageSize - 1);
      if (auto E = Memory.validateBacking(Address, Pages))
        return E;
    }
    for (const auto &[Address, Allocation] : UserAllocations) {
      const uint64_t Pages =
          (Allocation.Size + profile::PageSize - 1) & ~(profile::PageSize - 1);
      const unsigned Permissions =
          Allocation.ProcessID == ProcessID &&
                  !ExitedUserProcesses.contains(ProcessID) &&
                  !RevokedUserAllocations.contains(Address)
              ? userPermissions(Allocation.Access)
              : 0;
      if (auto E = Memory.protect(Address, Pages, Permissions))
        return E;
    }
  }
  UserRequestContext = Active;
  CurrentUserProcessID = Active ? ProcessID : 0;
  return llvm::Error::success();
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

bool KernelModel::requestPending(uint64_t IRP) const {
  auto Pending = [](const ActiveRequest &Request) {
    return Request.DispatchReturned &&
           (!Request.Completed ||
            (Request.ChildPower && !Request.ChildPower->CallbackReturned));
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
  if (Request->Neither) {
    if (IsIOCTL) {
      auto InputBuffer = allocateUserBuffer(
          Input.Input.size(), Input.Input,
          Input.UserInputAccess.value_or(DriverUserPageAccess::ReadWrite),
          Request->ProcessID);
      if (!InputBuffer)
        return InputBuffer.takeError();
      Request->UserInput = *InputBuffer;
    }
    auto UserBuffer = allocateUserBuffer(
        IsWrite ? Input.Input.size() : Input.OutputSize,
        IsWrite ? llvm::ArrayRef<uint8_t>(Input.Input)
                : llvm::ArrayRef<uint8_t>(),
        (IsWrite ? Input.UserInputAccess : Input.UserOutputAccess)
            .value_or(DriverUserPageAccess::ReadWrite),
        Request->ProcessID);
    if (!UserBuffer)
      return UserBuffer.takeError();
    Request->UserBuffer = *UserBuffer;
    return llvm::Error::success();
  }
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

llvm::Error KernelModel::revokeRequestUserBuffers(uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  if (!Request || !Request->Neither || !Request->DispatchReturned)
    return ioError("user unmapping requires a dispatched neither-I/O IRP");
  std::vector<std::pair<uint64_t, uint64_t>> Ranges;
  for (uint64_t Address : {Request->UserInput, Request->UserBuffer}) {
    if (!Address)
      continue;
    auto It = UserAllocations.find(Address);
    if (It == UserAllocations.end() || RevokedUserAllocations.contains(Address))
      return ioError("user unmapping requires a live original buffer");
    const uint64_t Pages =
        (It->second.Size + profile::PageSize - 1) & ~(profile::PageSize - 1);
    if (auto E = Memory.validateBacking(Address, Pages))
      return E;
    Ranges.emplace_back(Address, Pages);
  }
  for (const auto &[Address, Pages] : Ranges) {
    if (auto E = Memory.protect(Address, Pages, 0))
      return E;
    RevokedUserAllocations.insert(Address);
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::exitRequestorProcess(uint64_t IRP) {
  const auto *Request = requestForIRP(IRP);
  if (!Request || !Request->Neither || !Request->DispatchReturned ||
      !Request->ProcessID || ExitedUserProcesses.contains(Request->ProcessID))
    return ioError("requestor exit requires a live dispatched neither-I/O "
                   "request and process");
  std::vector<std::pair<uint64_t, uint64_t>> Ranges;
  for (const auto &[Address, Allocation] : UserAllocations) {
    if (Allocation.ProcessID != Request->ProcessID ||
        RevokedUserAllocations.contains(Address))
      continue;
    const uint64_t Pages =
        (Allocation.Size + profile::PageSize - 1) & ~(profile::PageSize - 1);
    if (auto E = Memory.validateBacking(Address, Pages))
      return E;
    Ranges.emplace_back(Address, Pages);
  }
  for (const auto &[Address, Pages] : Ranges) {
    if (auto E = Memory.protect(Address, Pages, 0))
      return E;
    RevokedUserAllocations.insert(Address);
  }
  ExitedUserProcesses.insert(Request->ProcessID);
  return llvm::Error::success();
}

llvm::Error KernelModel::initializeRequestPacket(ActiveRequest &Record,
                                                 const DriverRequest &Input) {
  auto *Request = &Record;
  const uint64_t Packet = Request->IRP;
  const uint64_t File = Request->FileAddress;
  const bool IsPnp = Input.Kind == DriverRequestKind::Pnp;
  const bool IsPower = Input.Kind == DriverRequestKind::Power;
  const bool FileFree = IsPnp || IsPower;
  uint32_t Flags = FileFree ? 0 : IRPSynchronous;
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
  if (Input.OutputSize && !Request->Direct && !Request->Neither)
    Flags |= IRPCopyOutput;
  struct Field {
    uint64_t Offset, Value;
    unsigned Size;
  };
  for (const Field &F : std::array<Field, 13>{
           {{0, IRPType, 2},
            {2, IRPSize + Request->StackCount * StackSize, 2},
            {IRPMdlOffset, Request->Mdl, 8},
            {IRPFlagsOffset, Flags, 4},
            {IRPSystemBufferOffset, Request->SystemBuffer, 8},
            {IRPStatusOffset, FileFree ? StatusNotSupported : 0, 4},
            {IRPRequestorModeOffset, FileFree ? KernelMode : UserMode, 1},
            {IRPStackCountOffset, Request->StackCount, 1},
            {IRPLocationOffset, Request->StackCount, 1},
            {IRPUserBufferOffset, Request->UserBuffer, 8},
            {IRPStackPointerOffset, Request->Stack, 8},
            {IRPOriginalFileOffset, File, 8},
            {Request->Stack - Packet, majorFunction(Input.Kind), 1}}})
    if (auto E = Memory.writeInteger(Packet + F.Offset, F.Value, F.Size))
      return E;
  if (auto E = Memory.writeInteger(Request->Stack + StackDeviceOffset,
                                   Request->DeviceRoute.front(), 8))
    return E;
  if (auto E = Memory.writeInteger(Request->Stack + StackFileOffset, File, 8))
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
              {StackPowerSystemContextOffset, Power.SystemContext, 4},
              {StackPowerTypeOffset, uint32_t(Power.Type), 4},
              {StackPowerStateOffset, Power.State, 4},
              {StackPowerActionOffset, uint32_t(Power.Action), 4}}})
      if (auto E =
              Memory.writeInteger(Request->Stack + F.Offset, F.Value, F.Size))
        return E;
    if (auto E = Memory.writeInteger(Packet + IRPInformationOffset, 0, 8))
      return E;
    Request->IOStatusWritten.fill(true);
  } else if (Input.Kind == DriverRequestKind::DeviceControl) {
    for (const Field &F :
         std::array<Field, 3>{{{StackParametersOffset, Input.OutputSize, 4},
                               {StackInputLengthOffset, Input.Input.size(), 4},
                               {StackIOControlOffset, Input.ControlCode, 4}}})
      if (auto E =
              Memory.writeInteger(Request->Stack + F.Offset, F.Value, F.Size))
        return E;
    if (Request->Neither)
      if (auto E = Memory.writeInteger(
              Request->Stack + StackType3InputOffset, Request->UserInput, 8))
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
  if (Unloading || Unloaded ||
      std::any_of(Requests.begin(), Requests.end(), [](const auto &Entry) {
        return !Entry.second.DispatchReturned;
      }))
    return ioError(
        "cannot begin a request during another invocation or after unload");
  if ((!Input.InterruptEvents.empty() || !Input.DmaEvents.empty()) &&
      Input.Kind != DriverRequestKind::Read &&
      Input.Kind != DriverRequestKind::Write &&
      Input.Kind != DriverRequestKind::DeviceControl)
    return ioError("interrupt events require a transfer request");
  if (auto E = Interrupts.canArm(Input.InterruptEvents, SourceIndex.value_or(Index),
                                 Scheduler.now100ns()))
    return E;
  if (auto E = DMA.canArm(Input.DmaEvents, SourceIndex.value_or(Index),
                          Scheduler.now100ns()))
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
  if (!Input.DeviceID.empty()) {
    auto Found = PnpDevices.find(Input.DeviceID);
    if (Found == PnpDevices.end() || !Found->second.AddDeviceStatus ||
        (*Found->second.AddDeviceStatus & profile::NTStatusFailureMask) ||
        !Devices.count(Found->second.PDO))
      return ioError(
          "file request requires a successfully added live device_id");
    Device = Found->second.PDO;
    const auto Existing = Files.find(Input.File);
    if (Input.Kind != DriverRequestKind::Create && Existing != Files.end()) {
      if (Existing->second.PnpDevice != Device)
        return ioError("device_id does not match the file's device");
      Device = Existing->second.Device;
    }
  } else if (Input.Device.empty()) {
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
  uint64_t PnpOwner = 0;
  const auto ExistingFile = Files.find(Input.File);
  if (Input.Kind != DriverRequestKind::Create && ExistingFile != Files.end()) {
    PnpOwner = ExistingFile->second.PnpDevice;
  } else {
    auto Owner = pnpDeviceForRoute(Device);
    if (!Owner)
      return Owner.takeError();
    PnpOwner = *Owner;
  }
  const bool LifecycleIo = PnpOwner &&
                           Input.Kind != DriverRequestKind::Cleanup &&
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
  auto Top = topAttachedDevice(Device);
  if (!Top)
    return Top.takeError();
  auto Route = deviceStack(*Top);
  if (!Route)
    return Route.takeError();
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
  if (!Neither && (Input.UserInputAccess || Input.UserOutputAccess))
    return ioError("user page access requires neither READ/WRITE I/O");
  if (Input.DeferCallbackDrain && FrameworkDevices.count(*Top))
    return ioError("defer_callback_drain requires a WDM request");
  if (Input.UserUnmapAfterDispatch &&
      (!Neither || (Input.Input.empty() && !Input.OutputSize)))
    return ioError("user unmapping requires a nonempty neither-I/O transfer");
  if (Input.RequestorProcessID <= 4)
    return ioError("requestor process identity must be above the system PID");
  if (ExitedUserProcesses.contains(Input.RequestorProcessID) &&
      Input.Kind != DriverRequestKind::Cleanup &&
      Input.Kind != DriverRequestKind::Close)
    return ioError("requesting process has exited");
  if (Input.RequestorExitAfterDispatch &&
      (!Neither || (Input.Input.empty() && !Input.OutputSize)))
    return ioError("requestor exit requires a nonempty neither-I/O transfer");
  auto FileIt = Files.find(Input.File);
  if (Input.Kind == DriverRequestKind::Create) {
    if (Devices.at(Device).DeletePending || Devices.at(*Top).DeletePending)
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
    State.PnpDevice = PnpOwner;
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
    return ioError("a synchronous file requires its prior request to finalize");
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
  Record.Direct = Direct;
  Record.Neither = Neither;
  if (LifecycleIo)
    if (auto E = Lifecycle.trackIo(PnpOwner, *Packet))
      return E;
  Record.FileAddress = FileIt->second.Address;
  Record.FileId = Input.File;
  Record.ProcessID = Input.RequestorProcessID;
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
  Result.Requests[Index].IRP = *Packet;
  if (auto E = Interrupts.arm(Input.InterruptEvents, SourceIndex.value_or(Index),
                              Scheduler.now100ns()))
    return E;
  if (auto E = DMA.arm(Input.DmaEvents, SourceIndex.value_or(Index),
                       Scheduler.now100ns()))
    return E;
  const uint64_t Callback = Result.MajorFunctions[Major];
  if (Framework) {
    auto Route = Framework->routeRequest(*Top, Request->IRP);
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
      if (auto E = processRequestCancellations())
        return E;
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
  Invocation Call{Callback, *Top, Request->IRP};
  Call.IRP = *Packet;
  return Call;
}

llvm::Error KernelModel::retireCompletedRequest(uint64_t IRP,
                                                uint8_t PriorityBoost) {
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
  auto Pending = dispatchPending(*Request, Request->StackCount - 1);
  if (!Pending)
    return Pending.takeError();
  Request->PendingMarked = *Pending;
  if (Request->DispatchReturned &&
      Observation.DispatchStatus == StatusPending && !Request->PendingMarked)
    return ioError(
        "pending dispatch completion requires propagation to the top stack");
  // Completion retires several allocations together. Preflight every range
  // before unregistering any dispatcher state or delivering output bytes.
  std::vector<std::pair<uint64_t, uint64_t>> Retiring{
      {IRP, IRPSize + Request->StackCount * StackSize}};
  if (Request->RawResources) {
    Retiring.emplace_back(Request->RawResources, Request->ResourceListSize);
    Retiring.emplace_back(Request->TranslatedResources,
                          Request->ResourceListSize);
  }
  if (Request->SystemBuffer)
    Retiring.emplace_back(Request->SystemBuffer, Request->BufferSize);
  if (Request->UserBuffer && !Request->Neither)
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
  if (Request->SystemMdl) {
    auto It = MDLs.find(Request->SystemMdl);
    if (It == MDLs.end() || It->second.OwnerIRP != IRP ||
        It->second.Owner != LockedMdl::Ownership::RequestSystemBuffer)
      return ioError("active request lost ownership of its system-buffer MDL");
    Retiring.emplace_back(It->second.Address, It->second.Size);
  }
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
  if (auto E = prepareReleaseRanges(Retiring))
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
      !ntError(*Status) && *Information) {
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
      if (auto E = Memory.write(Request->UserBuffer, Observation.Output))
        return E;
    }
  }
  if (auto E = expireRequestMDL(IRP))
    return E;
  if (auto E = finishRequestLifecycle(*Request, uint32_t(*Status)))
    return E;
  if (Request->SystemBuffer)
    if (auto E = Physical.retire(Request->SystemBuffer))
      return E;
  if (Request->UserBuffer && !Request->Neither)
    if (auto E = Physical.retire(Request->UserBuffer))
      return E;
  // Completion consumes the IRP. Preserve observations now; subsequent guest
  // accesses to the packet and its system buffer are invalid even before the
  // dispatch routine returns to the session.
  Observation.Completed = true;
  Request->Completed = true;
  Request->CancelDeadline.reset();
  FreedRanges.emplace(IRP, IRPSize + Request->StackCount * StackSize);
  if (Request->RawResources) {
    FreedRanges.emplace(Request->RawResources, Request->ResourceListSize);
    FreedRanges.emplace(Request->TranslatedResources,
                        Request->ResourceListSize);
  }
  if (Request->SystemBuffer)
    FreedRanges.emplace(Request->SystemBuffer, Request->BufferSize);
  if (Request->UserBuffer && !Request->Neither)
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
    if (!Request->Forwarded && Observation.IOStatus != DispatchStatus)
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
  if (std::any_of(IRPCalls.begin(), IRPCalls.end(),
                  [&](const auto &Entry) { return Entry.second.IRP == IRP; }))
    return ioError(
        "request finalization requires its guest continuations to return");
  if (Request->PowerTicket) {
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
  auto FileIt = Files.find(Request->FileId);
  if (FileIt == Files.end() || FileIt->second.Address != Request->FileAddress ||
      FileIt->second.Device != Request->Device ||
      FileIt->second.PnpDevice != Request->PnpDevice)
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
      !WorkItems.empty() || !IRPCalls.empty() || PendingWdmCall ||
      Scheduler.hasPending())
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
                   (Offset >= IRPCancelRoutineOffset &&
                    Offset < IRPCancelRoutineOffset + profile::PointerSize) ||
                   Offset == IRPPendingOffset || Offset == IRPLocationOffset ||
                   (Offset >= IRPStackPointerOffset &&
                    Offset < IRPStackPointerOffset + profile::PointerSize) ||
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
    if (!Request->Neither)
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
