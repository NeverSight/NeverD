//===- KernelModelControlRequests.cpp - Synchronous IOCTL packet owner ---===//
///
/// \file
/// Synchronous IOCTL packet owner.
///

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>
#include <array>
#include <tuple>

namespace neverd::emulation {
namespace {
using namespace windows;
llvm::Error controlError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "IOCTL builder: " + Text);
}
bool overlap(uint64_t A, uint64_t N, uint64_t B, uint64_t M) {
  return N && M && A < B + M && B < A + N;
}
} // namespace
llvm::Expected<uint64_t>
KernelModel::buildDeviceIoControlRequest(llvm::ArrayRef<uint64_t> A) {
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return controlError("requires windows_version 10.0.19041");
  if (!IRPIOPriorityHint)
    return controlError(
        "requires an explicit irp_io_priority_hint observation");
  auto D = Devices.find(A[1]);
  if (D == Devices.end() || D->second.DeletePending ||
      D->second.OwnerKind != DeviceOwnerKind::Guest ||
      FrameworkDevices.contains(A[1]))
    return controlError("requires a live guest WDM target");
  const uint32_t Code = uint32_t(A[0]), InputSize = uint32_t(A[3]),
                 OutputSize = uint32_t(A[5]), Method = Code & 3;
  if ((InputSize && !A[2]) || (OutputSize && !A[4]) ||
      InputSize > KernelMemoryBytes || OutputSize > KernelMemoryBytes)
    return controlError(
        "requires bounded buffers and matching nonnull pointers");
  const bool Direct = Method == 1 || Method == 2;
  for (const auto &[Address, Size, Writing] :
       {std::tuple{A[2], InputSize, false},
        std::tuple{A[4], OutputSize, Method != 1},
        std::tuple{A[8], uint32_t(16), true}}) {
    if (Size && Address < profile::UserProbeLimit)
      return controlError("buffers and IOSB require kernel storage");
    if (auto E = objectBuffer(Address, Size, Writing))
      return E;
    if (Size) {
      auto Pin = Memory.pinBacking(Address, Size);
      if (!Pin)
        return Pin.takeError();
    }
  }
  if (A[7] && !Dispatcher.isEvent(A[7]))
    return controlError("event requires a live initialized event object");
  if (overlap(A[8], 16, A[2], InputSize) ||
      overlap(A[8], 16, A[4], OutputSize) ||
      overlap(A[7], A[7] ? 24 : 0, A[8], 16) ||
      overlap(A[7], A[7] ? 24 : 0, A[2], InputSize) ||
      overlap(A[7], A[7] ? 24 : 0, A[4], OutputSize))
    return controlError(
        "overlapping IOSB/event and transfer storage is unsupported");
  // Reuse the existing kernel-pool page-lock contract for direct output.
  if (Direct && OutputSize) {
    auto Pool = Allocations.upper_bound(A[4]);
    if (Pool == Allocations.begin())
      return controlError(
          "direct output requires a live kernel pool allocation");
    --Pool;
    const auto Offset = A[4] - Pool->first;
    if (Offset >= Pool->second.Size || OutputSize > Pool->second.Size - Offset)
      return controlError(
          "direct output requires one complete kernel pool allocation");
  }
  auto Count = Memory.readInteger(A[1] + DeviceStackCountOffset, 1);
  auto Flags = Memory.readInteger(A[1] + DeviceFlagsOffset, 4);
  if (!Count || !Flags)
    return llvm::joinErrors(Count.takeError(), Flags.takeError());
  if (!*Count || *Count > MaxIRPStackCount || (*Flags & 0x08000000))
    return controlError(
        "target stack count or IRP extension allocation is unsupported");
  const uint32_t BufferSize = Method == 0      ? std::max(InputSize, OutputSize)
                              : Direct && A[2] ? InputSize
                                               : 0;
  if (Direct && A[2] && !InputSize)
    return controlError(
        "nonnull zero-length direct input allocation is unsupported");
  const uint64_t IrpBytes = IRPSize + *Count * StackSize;
  const uint64_t MdlBytes =
      Direct && OutputSize
          ? MDLSize + ((A[4] % 4096 + OutputSize + 4095) / 4096) * 8
          : 0;
  const uint64_t Required =
      IrpBytes + BufferSize + MdlBytes + 3 * PoolAlignment;
  auto Thread = currentThreadObject();
  if (!Thread)
    return Thread.takeError();
  if (Required > AllocationEnd - NextAllocation)
    return uint64_t(0);
  auto IRP = allocateDriverIRP({*Count, 0});
  if (!IRP || !*IRP)
    return IRP;
  auto &Storage = DriverIRPs.at(*IRP);
  Storage.Builder = ControlIRPBuilder{A[1],
                                      A[2],
                                      A[4],
                                      A[7],
                                      A[8],
                                      *Thread,
                                      CurrentThreadKey,
                                      Code,
                                      InputSize,
                                      OutputSize,
                                      Method,
                                      uint8_t(A[6]) != 0};
  auto Fail = [&](llvm::Error E) -> llvm::Expected<uint64_t> {
    if (Storage.Builder->Mdl) {
      auto M = MDLs.find(Storage.Builder->Mdl);
      if (M != MDLs.end() && M->second.isProbeLocked())
        llvm::consumeError(unlockPages(M->first));
      llvm::consumeError(freeMDL(Storage.Builder->Mdl));
    }
    if (Storage.Builder->SystemBuffer) {
      llvm::consumeError(Physical.retire(Storage.Builder->SystemBuffer));
      FreedRanges.emplace(Storage.Builder->SystemBuffer, BufferSize);
    }
    FreedRanges.emplace(*IRP, Storage.Size);
    DriverIRPs.erase(*IRP);
    return std::move(E);
  };
  uint32_t IrpFlags = (uint32_t(*IRPIOPriorityHint) + 1) << 17;
  if (BufferSize) {
    auto B = allocatePhysicalBuffer(BufferSize, PoolAlignment, 0, BufferSize);
    if (!B)
      return Fail(B.takeError());
    Storage.Builder->SystemBuffer = *B;
    Storage.Builder->SystemInitialized.resize(BufferSize, false);
    auto Watch = watchInitialization(*B, BufferSize);
    if (!Watch)
      return Fail(Watch.takeError());
    Storage.Builder->SystemInitialization = std::move(*Watch);
    if (InputSize) {
      std::vector<uint8_t> Input(InputSize);
      if (auto E = Memory.read(A[2], Input))
        return Fail(std::move(E));
      if (auto E = Memory.write(*B, Input))
        return Fail(std::move(E));
      std::fill_n(Storage.Builder->SystemInitialized.begin(), InputSize, true);
    }
    IrpFlags |= 0x30;
    if (Method == 0 && A[4])
      IrpFlags |= 0x40;
  }
  if (Direct && OutputSize) {
    auto M = allocateMDL({A[4], OutputSize, 0, 0, 0});
    if (!M)
      return Fail(M.takeError());
    if (!*M)
      return Fail(controlError("descriptor capacity changed after preflight"));
    Storage.Builder->Mdl = *M;
    if (auto E = probeAndLockPages(*M, KernelMode,
                                   Method == 1 ? IoReadAccess : IoWriteAccess))
      return Fail(std::move(E));
  }
  const uint64_t Stack = *IRP + IRPSize + (*Count - 1) * StackSize;
  const uint64_t UserBuffer =
      Method == 3 || (Method == 0 && BufferSize) ? A[4] : 0;
  struct Field {
    uint64_t At, Value;
    unsigned Width;
  };
  for (const auto &F : std::vector<Field>{
           {*IRP + IRPMdlOffset, Storage.Builder->Mdl, 8},
           {*IRP + IRPFlagsOffset, IrpFlags, 4},
           {*IRP + IRPSystemBufferOffset, Storage.Builder->SystemBuffer, 8},
           {*IRP + IRPStatusOffset, 0, 8},
           {*IRP + IRPInformationOffset, 0, 8},
           {*IRP + IRPUserIOSBOffset, A[8], 8},
           {*IRP + IRPUserEventOffset, A[7], 8},
           {*IRP + IRPThreadOffset, *Thread, 8},
           {*IRP + IRPUserBufferOffset, UserBuffer, 8},
           {Stack, uint8_t(A[6]) ? 15u : 14u, 1},
           {Stack + StackParametersOffset, OutputSize, 4},
           {Stack + StackInputLengthOffset, InputSize, 4},
           {Stack + StackIOControlOffset, Code, 4},
           {Stack + StackType3InputOffset, Method == 3 ? A[2] : 0, 8}})
    if (auto E = Memory.writeInteger(F.At, F.Value, F.Width))
      return Fail(std::move(E));
  Storage.IOStatusWritten.fill(true);
  return *IRP;
}

llvm::Error KernelModel::adoptBuiltControlIRP(uint64_t Device, uint64_t IRP) {
  auto &Allocation = DriverIRPs.at(IRP);
  const auto &B = *Allocation.Builder;
  if (Allocation.Submitted || Allocation.StorageReleased || Device != B.Device)
    return controlError("first submission requires the original live target");
  if (PendingWdmCall || (Framework && Framework->hasPendingGuestCall()))
    return controlError("submission cannot replace a pending callback");
  auto Target = Devices.find(Device);
  if (Target == Devices.end() || Target->second.DeletePending ||
      FrameworkDevices.contains(Device))
    return controlError("submission requires the original live WDM target");
  const uint64_t Stack =
      IRP + IRPSize + (Allocation.StackCount - 1) * StackSize;
  auto Routine = Memory.readInteger(Stack + StackCompletionOffset, 8);
  auto Control = Memory.readInteger(Stack + StackControlOffset, 1);
  if (!Routine || !Control)
    return llvm::joinErrors(Routine.takeError(), Control.takeError());
  constexpr uint64_t CompletionFlags =
      StackInvokeOnSuccess | StackInvokeOnError | StackInvokeOnCancel;
  if ((*Control & ~CompletionFlags) || (*Control && !*Routine))
    return controlError("invalid completion invocation flags");
  if (!B.Event && (!*Routine || *Control != CompletionFlags))
    return controlError("an eventless request requires a completion routine "
                        "for success, error and cancellation");
  auto PC =
      Memory.readInteger(Target->second.OwnerDriver + DriverDispatchOffset +
                             (B.Internal ? 15 : 14) * 8,
                         8);
  if (!PC)
    return PC.takeError();
  if (!*PC)
    return controlError("submission requires a registered dispatch routine");
  auto Route = deviceStack(Device);
  if (!Route)
    return Route.takeError();
  if (Route->size() > Allocation.StackCount)
    return controlError("device route grew after construction");
  for (auto Member : *Route)
    if (Devices.at(Member).DeletePending ||
        Devices.at(Member).InternalReferences == UINT64_MAX)
      return controlError("submission requires live retainable devices");
  for (const auto &[Address, Expected, Width] :
       {std::tuple{IRP + IRPTypeOffset, uint64_t(IRPType), 2u},
        std::tuple{IRP + ObjectSizeOffset, Allocation.Size, 2u},
        std::tuple{IRP + IRPStackCountOffset, uint64_t(Allocation.StackCount),
                   1u},
        std::tuple{IRP + IRPRequestorModeOffset, uint64_t(KernelMode), 1u},
        std::tuple{IRP + IRPLocationOffset, uint64_t(Allocation.StackCount) + 1,
                   1u},
        std::tuple{IRP + IRPStackPointerOffset, Stack + StackSize, 8u},
        std::tuple{IRP + IRPOriginalFileOffset, uint64_t(0), 8u},
        std::tuple{Stack, uint64_t(B.Internal ? 15 : 14), 1u},
        std::tuple{Stack + StackIOControlOffset, uint64_t(B.Code), 4u},
        std::tuple{Stack + StackInputLengthOffset, uint64_t(B.InputSize), 4u},
        std::tuple{Stack + StackParametersOffset, uint64_t(B.OutputSize), 4u},
        std::tuple{Stack + StackType3InputOffset, B.Method == 3 ? B.Input : 0,
                   8u},
        std::tuple{IRP + IRPUserBufferOffset,
                   B.Method == 3 || (B.Method == 0 && B.SystemBuffer) ? B.Output
                                                                      : 0,
                   8u}}) {
    auto V = Memory.readInteger(Address, Width);
    if (!V)
      return V.takeError();
    if (*V != Expected)
      return controlError(
          "constructed packet header or transfer fields changed");
  }
  ActiveRequest Request{B.Internal ? DriverRequestKind::InternalDeviceControl
                                   : DriverRequestKind::DeviceControl,
                        Result.Requests.size()};
  Request.IRP = IRP;
  Request.Device = Device;
  Request.StackCount = Allocation.StackCount;
  Request.Stack = IRP + IRPSize + (Allocation.StackCount - 1) * StackSize;
  Request.DeviceRoute = *Route;
  Request.UnwoundPending.resize(Allocation.StackCount);
  Request.SystemBuffer = B.SystemBuffer;
  Request.UserInput = B.Input;
  Request.UserBuffer = B.Output;
  Request.BufferSize =
      B.Method == 0 ? std::max(B.InputSize, B.OutputSize) : B.InputSize;
  Request.InputSize = B.InputSize;
  Request.OutputSize = B.OutputSize;
  Request.TransferSize = B.OutputSize;
  Request.Direct = B.Method == 1 || B.Method == 2;
  Request.Neither = B.Method == 3;
  Request.Mdl = B.Mdl;
  Request.IOStatusWritten = Allocation.IOStatusWritten;
  Request.IOStatusInitialization = Allocation.IOStatusInitialization;
  for (const auto &[Offset, Expected, Width] :
       {std::tuple{IRPSystemBufferOffset, B.SystemBuffer, 8u},
        std::tuple{IRPMdlOffset, B.Mdl, 8u},
        std::tuple{IRPUserIOSBOffset, B.IOSB, 8u},
        std::tuple{IRPUserEventOffset, B.Event, 8u},
        std::tuple{IRPThreadOffset, B.ThreadObject, 8u}}) {
    auto V = Memory.readInteger(IRP + Offset, Width);
    if (!V)
      return V.takeError();
    if (*V != Expected)
      return controlError("constructed packet fields were redirected");
  }
  DriverRequestResult Observation;
  Observation.Kind = Request.Kind;
  Observation.Origin = DriverRequestOrigin::IoControlBuilder;
  Observation.IRP = IRP;
  Observation.ControlCode = B.Code;
  for (auto Member : *Route)
    if (auto E = retainDevice(Member))
      return E;
  Result.Requests.push_back(std::move(Observation));
  Requests.emplace(IRP, std::move(Request));
  Allocation.Submitted = true;
  Allocation.DispatchSlot = Allocation.StackCount - 1;
  return llvm::Error::success();
}
llvm::Error KernelModel::validateBuiltControlCompletion(uint64_t IRP) const {
  const auto &B = *DriverIRPs.at(IRP).Builder;
  if (auto E = objectBuffer(B.IOSB, 16, true))
    return E;
  if (B.Event && !Dispatcher.isEvent(B.Event))
    return controlError("completion lost its event");
  auto *R = requestForIRP(IRP);
  auto Status = Memory.readInteger(IRP + IRPStatusOffset, 4);
  auto Information = Memory.readInteger(IRP + IRPInformationOffset, 8);
  if (!Status || !Information)
    return llvm::joinErrors(Status.takeError(), Information.takeError());
  if (B.Method == 0 && R->OutputSize && *Information &&
      (uint32_t(*Status) >> profile::NTStatusSeverityShift) !=
          profile::NTStatusErrorSeverity)
    if (auto E = validateBuiltControlBufferAccess(
            B.SystemBuffer, uint32_t(*Information), false))
      return E;
  if (R->OutputSize && !R->Direct)
    if (auto E = objectBuffer(B.Output, R->OutputSize, true))
      return E;
  return llvm::Error::success();
}
llvm::Error KernelModel::notifyBuiltControlCompletion(uint64_t IRP) {
  auto &A = DriverIRPs.at(IRP);
  const auto &B = *A.Builder;
  std::array<uint8_t, 16> IOSB;
  if (auto E = Memory.read(IRP + IRPStatusOffset, IOSB))
    return E;
  if (auto E = Memory.write(B.IOSB, IOSB))
    return E;
  if (B.Event) {
    auto Signal = Dispatcher.call("KeSetEvent", {B.Event, 0, 0}, CurrentIRQL,
                                  CurrentThreadKey);
    if (!Signal)
      return Signal.takeError();
  }
  A.StorageReleased = true;
  return llvm::Error::success();
}
llvm::Error KernelModel::canReleaseBuiltControlStorage(uint64_t Base,
                                                       uint64_t Size) const {
  for (const auto &[IRP, A] : DriverIRPs) {
    if (!A.Builder || A.StorageReleased)
      continue;
    const auto &B = *A.Builder;
    for (const auto &[Address, Bytes] :
         {std::pair{B.IOSB, uint64_t(16)},
          std::pair{B.Event, B.Event ? uint64_t(24) : 0},
          std::pair{B.Input, B.Method == 3 ? uint64_t(B.InputSize) : 0},
          std::pair{B.Output, uint64_t(B.OutputSize)}})
      if (overlap(Base, Size, Address, Bytes))
        return controlError("cannot release live request caller storage");
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::validateBuiltControlBufferAccess(uint64_t Address,
                                                          uint32_t Size,
                                                          bool IsWrite) const {
  for (const auto &[IRP, A] : DriverIRPs) {
    if (!A.Builder || A.StorageReleased)
      continue;
    if (overlap(Address, Size, IRP + 0x20, 16))
      return controlError("thread queue links are opaque");
    if (!A.Builder->SystemBuffer)
      continue;
    const auto &B = *A.Builder;
    const uint64_t End = B.SystemBuffer + B.SystemInitialized.size();
    if (Address >= End || B.SystemBuffer >= Address + Size)
      continue;
    const auto First = std::max(Address, B.SystemBuffer) - B.SystemBuffer;
    const auto Last = std::min(Address + Size, End) - B.SystemBuffer;
    if (!IsWrite && !initialized(B.SystemInitialized, B.SystemInitialization,
                                 First, Last - First))
      return controlError("read of uninitialized system buffer bytes");
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
