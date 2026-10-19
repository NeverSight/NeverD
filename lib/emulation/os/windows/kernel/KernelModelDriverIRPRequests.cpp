//===- KernelModelDriverIRPRequests.cpp - Caller IRP dispatch -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bind a caller's packet to its actual lower route at first submission. The
/// caller's completion routine owns disposal; guest storage and outstanding
/// dispatch/cancel continuations therefore have separate retirement points.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/ADT/DenseMap.h"

#include <algorithm>
#include <array>
#include <tuple>

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error driverIRPError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "caller IRP: " + Message);
}
} // namespace

llvm::Error KernelModel::adoptDriverIRP(uint64_t Device, uint64_t IRP) {
  auto Allocation = DriverIRPs.find(IRP);
  if (Allocation == DriverIRPs.end() || Allocation->second.Submitted ||
      Allocation->second.StorageReleased)
    return driverIRPError("first submission requires a live unused allocation");
  if (Allocation->second.Builder)
    return adoptBuiltControlIRP(Device, IRP);
  if (PendingWdmCall || (Framework && Framework->hasPendingGuestCall()))
    return driverIRPError("submission cannot replace a pending callback");
  auto Target = Devices.find(Device);
  if (Target == Devices.end() || Target->second.DeletePending ||
      FrameworkDevices.contains(Device))
    return driverIRPError(
        "internal submission requires a live guest WDM target");
  auto Route = deviceStack(Device);
  if (!Route)
    return Route.takeError();
  if (auto E = validateFileSystemRoute(*Route))
    return E;
  const auto &Storage = Allocation->second;
  struct Field {
    uint64_t Offset, Expected;
    unsigned Width;
  };
  for (const auto &Field :
       std::array<Field, 8>{{{IRPTypeOffset, IRPType, 2},
                             {ObjectSizeOffset, Storage.Size, 2},
                             {IRPStackCountOffset, Storage.StackCount, 1},
                             {IRPRequestorModeOffset, KernelMode, 1},
                             {IRPMdlOffset, 0, 8},
                             {IRPSystemBufferOffset, 0, 8},
                             {IRPThreadOffset, 0, 8},
                             {IRPOriginalFileOffset, 0, 8}}}) {
    auto Value = Memory.readInteger(IRP + Field.Offset, Field.Width);
    if (!Value)
      return Value.takeError();
    if (*Value != Field.Expected)
      return driverIRPError("internal packet requires its allocated header, "
                            "kernel mode and no MDL, system buffer or file");
  }
  auto Location = Memory.readInteger(IRP + IRPLocationOffset, 1);
  auto Pointer = Memory.readInteger(IRP + IRPStackPointerOffset, 8);
  auto Required = Memory.readInteger(Device + DeviceStackCountOffset, 1);
  if (!Location || !Pointer || !Required)
    return llvm::joinErrors(
        Location.takeError(),
        llvm::joinErrors(Pointer.takeError(), Required.takeError()));
  if (*Location < 2 || *Location > uint64_t(Storage.StackCount) + 1 ||
      *Pointer != IRP + IRPSize + (*Location - 1) * StackSize || !*Required ||
      *Required < Route->size() || *Location - 1 < *Required ||
      uint64_t(Storage.StackCount) + 1 - *Location > 1)
    return driverIRPError(
        "internal packet has an invalid cursor or insufficient "
        "lower stack capacity");
  const uint32_t Slot = uint32_t(*Location - 2);
  const uint64_t Stack = IRP + IRPSize + Slot * StackSize;
  auto Major = Memory.readInteger(Stack, 1);
  auto Code = Memory.readInteger(Stack + StackIOControlOffset, 4);
  auto InputSize = Memory.readInteger(Stack + StackInputLengthOffset, 4);
  auto OutputSize = Memory.readInteger(Stack + StackParametersOffset, 4);
  auto Input = Memory.readInteger(Stack + StackType3InputOffset, 8);
  auto Output = Memory.readInteger(IRP + IRPUserBufferOffset, 8);
  auto Control = Memory.readInteger(Stack + StackControlOffset, 1);
  auto Completion = Memory.readInteger(Stack + StackCompletionOffset, 8);
  if (!Major || !Code || !InputSize || !OutputSize || !Input || !Output ||
      !Control || !Completion)
    return llvm::joinErrors(
        llvm::joinErrors(Major.takeError(), Code.takeError()),
        llvm::joinErrors(
            llvm::joinErrors(InputSize.takeError(), OutputSize.takeError()),
            llvm::joinErrors(
                llvm::joinErrors(Input.takeError(), Output.takeError()),
                llvm::joinErrors(Control.takeError(),
                                 Completion.takeError()))));
  unsigned InternalMajor = profile::MajorFunctionCount;
#define NEVERD_DRIVER_REQUEST_KIND(Name, Spelling, Value)                      \
  if (DriverRequestKind::Name == DriverRequestKind::InternalDeviceControl)     \
    InternalMajor = Value;
#include "neverd/emulation/DriverRequestKinds.def"
#undef NEVERD_DRIVER_REQUEST_KIND
  if (*Major != InternalMajor || (*Code & IoControlMethodMask) != MethodNeither)
    return driverIRPError("caller packets support kernel METHOD_NEITHER "
                          "internal device control");
  const bool Usb = *Code == usb_idle::SubmitIdleNotification;
  uint64_t PDO = 0;
  if (Usb) {
    auto Provider = pnpDeviceForRoute(Device);
    if (!Provider)
      return Provider.takeError();
    PDO = *Provider;
    auto Plan = planUsbIdleSubmission(PDO, IRP, *Input, *InputSize, *OutputSize,
                                      *Output);
    if (!Plan)
      return Plan.takeError();
  } else if (isPnpProviderDevice(Device)) {
    return driverIRPError("internal provider submission requires USB idle");
  }
  constexpr uint64_t CompletionFlags =
      StackInvokeOnSuccess | StackInvokeOnError | StackInvokeOnCancel;
  if (!*Completion || (*Control & ~CompletionFlags) ||
      (*Control & CompletionFlags) != CompletionFlags)
    return driverIRPError("caller packets require a completion routine for "
                          "success, error and cancellation");
  auto PC =
      isPnpProviderDevice(Device)
          ? llvm::Expected<uint64_t>(0)
          : guestDriverDispatch(Target->second.OwnerDriver, InternalMajor);
  if (!PC)
    return PC.takeError();
  if (!*PC && !isPnpProviderDevice(Device))
    return driverIRPError(
        "internal target requires a registered dispatch routine");
  uint64_t CompletionDevice = 0;
  if (Slot + 1 < Storage.StackCount) {
    auto Upper = Memory.readInteger(Stack + StackSize + StackDeviceOffset, 8);
    if (!Upper)
      return Upper.takeError();
    auto Caller = Devices.find(*Upper);
    if (Caller == Devices.end() || Caller->second.DeletePending ||
        Caller->second.OwnerKind != DeviceOwnerKind::Guest ||
        Caller->second.OwnerDriver != DriverObject)
      return driverIRPError(
          "allocator stack requires the caller's live device");
    CompletionDevice = *Upper;
  }
  for (const auto &[Address, Size, Write] :
       {std::tuple{*Input, *InputSize, false},
        std::tuple{*Output, *OutputSize, true}}) {
    if (Size > KernelMemoryBytes || (Size && Address < profile::UserProbeLimit))
      return driverIRPError("internal buffers require bounded kernel storage");
    if (Size)
      if (auto E = validateGuestAccess(Address, uint32_t(Size), Write))
        return E;
  }
  if (NextIRPCall == UINT64_MAX)
    return driverIRPError("dispatch continuation identity exhausted");
  auto Writable = Memory.canAccess(IRP, Storage.Size, Read | Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return driverIRPError("caller packet must remain writable kernel memory");
  llvm::SmallDenseMap<uint64_t, uint64_t> References;
  for (uint64_t Member : *Route)
    ++References[Member];
  if (CompletionDevice)
    ++References[CompletionDevice];
  for (const auto &[Member, Count] : References)
    if (Devices.at(Member).DeletePending ||
        Count > UINT64_MAX - Devices.at(Member).InternalReferences)
      return driverIRPError("internal route requires live retainable devices");

  ActiveRequest Request{DriverRequestKind::InternalDeviceControl,
                        Result.Requests.size()};
  Request.IRP = IRP;
  Request.Device = Device;
  Request.PnpDevice = PDO;
  Request.StackCount = Storage.StackCount;
  Request.Stack = Stack;
  Request.DeviceRoute = std::move(*Route);
  Request.UnwoundPending.resize(Storage.StackCount);
  Request.InputSize = uint32_t(*InputSize);
  Request.OutputSize = uint32_t(*OutputSize);
  Request.TransferSize = Request.OutputSize;
  Request.UserInput = *Input;
  Request.UserBuffer = *Output;
  Request.Neither = true;
  Request.IOStatusWritten = Storage.IOStatusWritten;
  Request.IOStatusInitialization = Storage.IOStatusInitialization;
  DriverRequestResult Observation;
  Observation.Kind = Request.Kind;
  Observation.Origin = DriverRequestOrigin::DriverAllocatedIRP;
  Observation.IRP = IRP;
  Observation.ControlCode = uint32_t(*Code);
  if (PDO)
    Observation.DeviceID =
        Result.PnpDevices[pnpDeviceForPDO(PDO)->ResultIndex].ID;
  for (uint64_t Member : Request.DeviceRoute)
    if (auto E = retainDevice(Member))
      return E;
  if (CompletionDevice)
    if (auto E = retainDevice(CompletionDevice))
      return E;
  Result.Requests.push_back(std::move(Observation));
  Requests.emplace(IRP, std::move(Request));
  Allocation->second.Submitted = true;
  Allocation->second.DispatchSlot = Slot;
  Allocation->second.CompletionDevice = CompletionDevice;
  return llvm::Error::success();
}

llvm::Error
KernelModel::validateDriverIRPCompletion(uint64_t IRP, uint32_t Status,
                                         uint64_t Information) const {
  const auto Allocation = DriverIRPs.find(IRP);
  const auto *Request = requestForIRP(IRP);
  if (Allocation == DriverIRPs.end() || Allocation->second.StorageReleased ||
      !Request || Request->Completed)
    return driverIRPError("completion requires a live submitted caller packet");
  if (Status == StatusPending)
    return driverIRPError("caller completion cannot retain STATUS_PENDING");
  if (Request->OutputSize && Information > Request->OutputSize)
    return driverIRPError("internal completion exceeds its output buffer");
  auto Mdl = Memory.readInteger(IRP + IRPMdlOffset, 8);
  if (!Mdl)
    return Mdl.takeError();
  if (*Mdl)
    return driverIRPError("submitted caller MDL transfers are not modeled");
  return DMA.canReleaseRange(IRP, Allocation->second.Size);
}

llvm::Error KernelModel::captureDriverIRPCompletion(uint64_t IRP) {
  auto *Request = requestForIRP(IRP);
  if (!Request)
    return driverIRPError("caller completion lost its request");
  if (Request->Completed)
    return llvm::Error::success();
  for (unsigned I = 0; I < Request->IOStatusWritten.size(); ++I)
    if ((I < sizeof(uint32_t) || I >= sizeof(uint64_t)) &&
        !initialized(Request->IOStatusWritten, Request->IOStatusInitialization,
                     I))
      return driverIRPError("caller completion requires initialized IoStatus");
  auto Status = Memory.readInteger(IRP + IRPStatusOffset, 4);
  auto Information = Memory.readInteger(IRP + IRPInformationOffset, 8);
  if (!Status || !Information)
    return llvm::joinErrors(Status.takeError(), Information.takeError());
  if (auto E =
          validateDriverIRPCompletion(IRP, uint32_t(*Status), *Information))
    return E;
  auto Pending = dispatchPending(*Request, DriverIRPs.at(IRP).DispatchSlot);
  if (!Pending)
    return Pending.takeError();
  if (auto E = validateCompletionPending(*Request, *Pending))
    return E;
  std::vector<uint8_t> Output;
  if (Request->OutputSize && *Information &&
      (uint32_t(*Status) >> profile::NTStatusSeverityShift) !=
          profile::NTStatusErrorSeverity) {
    if (auto E = validateGuestAccess(Request->UserBuffer,
                                     uint32_t(*Information), false))
      return E;
    Output.resize(*Information);
    if (auto E = Memory.read(Request->UserBuffer, Output))
      return E;
  }
  auto &Observation = Result.Requests[Request->ResultIndex];
  Observation.Output = std::move(Output);
  Observation.IOStatus = uint32_t(*Status);
  Observation.Information = *Information;
  Observation.Completed = true;
  if (Observation.UsbIdle)
    Observation.UsbIdle->CompletedAt100ns = Scheduler.now100ns();
  Request->Completed = true;
  Request->PendingMarked = *Pending;
  Request->CancelDeadline.reset();
  return llvm::Error::success();
}

llvm::Error KernelModel::freeSubmittedDriverIRP(uint64_t IRP) {
  auto &Allocation = DriverIRPs.at(IRP);
  auto *Request = requestForIRP(IRP);
  if (!Request || CancelLock.Held || ProviderCompletions.contains(IRP))
    return driverIRPError("free requires caller completion ownership");
  std::optional<uint64_t> Completion;
  for (const auto &[Token, Call] : IRPCalls) {
    if (Call.IRP != IRP || Call.Kind != IRPCallKind::Completion ||
        !Call.AwaitingCallback)
      continue;
    if (Call.Slot != Allocation.DispatchSlot + 1)
      return driverIRPError(
          "free requires completion at the allocating caller");
    if (Completion)
      return driverIRPError("free cannot cross nested packet completions");
    Completion = Token;
  }
  if (Completion && (CurrentGuestCall.Owner != GuestCallOwner::WDM ||
                     CurrentGuestCall.ID != *Completion))
    return driverIRPError(
        "IoFreeIrp requires the currently executing completion");
  if (!Completion && !Allocation.CompletionHeld)
    return driverIRPError(
        "IoFreeIrp requires an active or retained completion");
  if (auto E = canReleaseRange(IRP, Allocation.Size))
    return E;
  if (auto E = captureDriverIRPCompletion(IRP))
    return E;
  if (auto E = releaseDriverIRPStorage(IRP))
    return E;
  Allocation.FreeCompletionToken = Completion;
  return tryFinalizeDriverIRP(IRP);
}

llvm::Error KernelModel::recordDriverIRPDispatchReturn(uint64_t IRP,
                                                       uint32_t Status) {
  auto *Request = requestForIRP(IRP);
  if (!Request || Request->DispatchReturned)
    return driverIRPError("initial dispatch return lost its caller packet");
  if (Status != StatusPending && !Request->Completed &&
      !(DriverIRPs.at(IRP).Builder && DriverIRPs.at(IRP).CompletionHeld))
    return driverIRPError("internal dispatch returned without completion");
  if (Request->Completed && (Status == StatusPending) != Request->PendingMarked)
    return driverIRPError(
        "internal dispatch status differs from its pending bit");
  Result.Requests[Request->ResultIndex].DispatchStatus = Status;
  Request->DispatchReturned = true;
  return llvm::Error::success();
}

llvm::Error KernelModel::tryFinalizeDriverIRP(uint64_t IRP) {
  auto Allocation = DriverIRPs.find(IRP);
  const auto *Request = requestForIRP(IRP);
  if (Allocation != DriverIRPs.end() && Request && Request->Completed &&
      !Request->DispatchReturned && Allocation->second.ProviderDispatchReturn) {
    if (auto E = recordDriverIRPDispatchReturn(
            IRP, *Allocation->second.ProviderDispatchReturn))
      return E;
    Allocation->second.ProviderDispatchReturn.reset();
  }
  if (Allocation == DriverIRPs.end() || !Allocation->second.StorageReleased ||
      !Request || !Request->Completed || !Request->DispatchReturned)
    return llvm::Error::success();
  if (std::any_of(IRPCalls.begin(), IRPCalls.end(),
                  [&](const auto &Entry) { return Entry.second.IRP == IRP; }))
    return llvm::Error::success();
  return finalizeRequest(IRP);
}
} // namespace neverd::emulation
