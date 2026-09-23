//===- KernelModelDMAChannels.cpp - WDM channel and transfer operations ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decode adapter-scoped channel operations against the same locked MDL and
/// physical RAM authority used by scatter/gather lists.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error channelError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "DMA channel: " + Text);
}
} // namespace

llvm::Expected<KernelModel::DmaMdlView>
KernelModel::dmaMdlView(uint64_t MDL, uint64_t CurrentVA, uint32_t Length,
                        bool ToDevice) const {
  const auto I = MDLs.find(MDL);
  if (I == MDLs.end() || I->second.Owner == LockedMdl::Ownership::Driver)
    return channelError("DMA requires a locked or nonpaged MDL");
  const auto &View = I->second;
  const uint64_t Virtual =
      View.Owner == LockedMdl::Ownership::Request ||
              View.Owner == LockedMdl::Ownership::UserLocked
          ? View.UserAddress
          : View.Buffer;
  if (!Length || CurrentVA < Virtual || CurrentVA - Virtual >= View.ByteCount ||
      Length > View.ByteCount - (CurrentVA - Virtual))
    return channelError("CurrentVa and Length exceed the MDL");
  if (!ToDevice && !View.DmaWritable)
    return channelError("device writes require a write-locked MDL");
  const uint64_t Backing =
      (View.Owner == LockedMdl::Ownership::UserLocked ? View.UserAddress
                                                      : View.Buffer) +
      (CurrentVA - Virtual);
  auto Owner = Physical.ownerForRange(Backing, Length);
  if (!Owner)
    return Owner.takeError();
  return DmaMdlView{*Owner, Backing - Physical.find(*Owner)->Backing,
                    View.Size};
}

llvm::Error KernelModel::flushIoBuffers(uint64_t MDL) {
  const auto I = MDLs.find(MDL);
  if (I == MDLs.end() || I->second.Owner == LockedMdl::Ownership::Driver)
    return channelError("KeFlushIoBuffers requires a locked or nonpaged MDL");
  const auto &View = I->second;
  const uint64_t Backing = View.Owner == LockedMdl::Ownership::UserLocked
                               ? View.UserAddress
                               : View.Buffer;
  auto Owner = Physical.ownerForRange(Backing, View.ByteCount);
  if (!Owner)
    return Owner.takeError();
  // This coherent platform has no separate CPU cache contents, for either
  // ReadOperation or DmaOperation. A cache flush does not end DMA ownership or
  // replace the operation-specific FlushAdapterBuffers contract.
  return Memory.validateBacking(Backing, View.ByteCount);
}

llvm::Expected<uint64_t>
KernelModel::allocateAdapterChannel(llvm::ArrayRef<uint64_t> A) {
  if (CurrentIRQL != scheduler::DispatchLevel || hasPendingModelGuestCall())
    return channelError("AllocateAdapterChannel requires DISPATCH_LEVEL and no "
                        "pending callback");
  const auto *Adapter = DMA.adapter(A[0]);
  const auto Device = Devices.find(A[1]);
  if (!Adapter || Device == Devices.end() || Device->second.DeletePending ||
      Device->second.OwnerKind != DeviceOwnerKind::Guest ||
      Device->second.PnpDevice != Adapter->PDO)
    return channelError(
        "AllocateAdapterChannel requires its adapter's live guest device");
  auto Current = Memory.readInteger(A[1] + windows::DeviceCurrentIRP,
                                    profile::PointerSize);
  if (!Current)
    return Current.takeError();
  uint64_t IRPSize = 0;
  if (*Current) {
    const auto *Request = requestForIRP(*Current);
    if (!Request || Request->Completed ||
        std::find(Request->DeviceRoute.begin(), Request->DeviceRoute.end(),
                  A[1]) == Request->DeviceRoute.end())
      return channelError(
          "CurrentIrp must name a live IRP routed to the device");
    IRPSize = windows::IRPSize + Request->StackCount * windows::StackSize;
  }
  KernelDMA::ChannelRequest Request;
  Request.Object = (NextAllocation + 15) & ~uint64_t(15);
  Request.Adapter = A[0];
  Request.Device = A[1];
  Request.DeviceSize = Device->second.Size;
  Request.Routine = A[3];
  Request.Context = A[4];
  Request.CurrentIRP = *Current;
  Request.IRPSize = IRPSize;
  Request.Registers = uint32_t(A[2]);
  auto Planned = DMA.planChannel(Request);
  if (!Planned)
    return Planned.takeError();
  if (!*Planned || Request.Object > AllocationEnd ||
      dma::ChannelTokenSize > AllocationEnd - Request.Object)
    return windows::StatusInsufficientResources;
  auto Plan = **Planned;
  constexpr auto Kind = KernelScheduler::CallbackKind::DMAAdapterControl;
  KernelScheduler::Callback Call;
  Call.Object = Request.Object;
  Call.Owner = Request.Device;
  Call.Thread = profile::WorkerThreadIdentity;
  Call.PC = Request.Routine;
  Call.Arguments = {Request.Device, Request.CurrentIRP, Request.Object,
                    Request.Context};
  if (auto E = Scheduler.canReserveDMACallback(Call, Kind))
    return E;
  if (Plan.Live)
    if (auto E = Scheduler.canDispatchInlineDMACallback())
      return E;
  auto Storage = allocate(dma::ChannelTokenSize);
  if (!Storage)
    return Storage.takeError();
  if (*Storage != Request.Object)
    return channelError("register-token allocation changed after preflight");
  auto ID = Scheduler.reserveDMACallback(Call, Kind);
  if (!ID)
    return ID.takeError();
  Plan.Request.SchedulerID = *ID;
  if (auto E = DMA.publishChannel(Plan))
    return E;
  KernelGuestCall Guest{{GuestCallOwner::DMA, Request.Object},
                        Request.Routine,
                        std::move(Call.Arguments)};
  if (Plan.Live) {
    InlineDMACalls.insert(Request.Object);
    PendingDMACall = std::move(Guest);
  } else {
    ScheduledModelContinuations.emplace(*ID, Guest.Token);
  }
  return windows::StatusSuccess;
}

llvm::Expected<uint64_t> KernelModel::mapTransfer(llvm::ArrayRef<uint64_t> A) {
  if (auto E = validateGuestAccess(A[4], sizeof(uint32_t), false))
    return E;
  if (auto E = validateGuestAccess(A[4], sizeof(uint32_t), true))
    return E;
  auto Length = Memory.readInteger(A[4], sizeof(uint32_t));
  if (!Length)
    return Length.takeError();
  const bool ToDevice = bool(uint8_t(A[5]));
  auto View = dmaMdlView(A[1], A[3], uint32_t(*Length), ToDevice);
  if (!View)
    return View.takeError();
  KernelDMA::TransferRequest Request;
  Request.Adapter = A[0];
  Request.Object = A[2];
  Request.Owner = View->Owner;
  Request.Offset = View->Offset;
  Request.MDL = A[1];
  Request.MDLSize = View->DescriptorSize;
  Request.CurrentVA = A[3];
  Request.Length = uint32_t(*Length);
  Request.Direction = ToDevice ? DriverDmaDirection::ReadMemory
                               : DriverDmaDirection::WriteMemory;
  auto Plan = DMA.planTransfer(Request);
  if (!Plan)
    return Plan.takeError();
  if (auto E = Memory.writeInteger(A[4], Plan->Length, sizeof(uint32_t)))
    return E;
  if (auto E = DMA.commitTransfer(*Plan))
    return E;
  return Plan->Logical;
}

llvm::Error KernelModel::flushAdapterBuffers(llvm::ArrayRef<uint64_t> A) {
  const bool ToDevice = bool(uint8_t(A[5]));
  auto View = dmaMdlView(A[1], A[3], uint32_t(A[4]), ToDevice);
  if (!View)
    return View.takeError();
  auto Plan = DMA.planFlush(A[0], A[2], A[1], A[3], uint32_t(A[4]),
                            ToDevice ? DriverDmaDirection::ReadMemory
                                     : DriverDmaDirection::WriteMemory);
  if (!Plan)
    return Plan.takeError();
  return DMA.flush(*Plan);
}

llvm::Error KernelModel::freeMapRegisters(llvm::ArrayRef<uint64_t> A) {
  auto Plan = DMA.planFreeRegisters(A[0], A[1], uint32_t(A[2]));
  if (!Plan)
    return Plan.takeError();
  auto Ready = dmaPromotionIDs(Plan->Ready);
  if (!Ready)
    return Ready.takeError();
  if (auto E = DMA.freeRegisters(*Plan))
    return E;
  if (auto E = Scheduler.readyDMACallbacks(*Ready))
    return E;
  FreedRanges.emplace(A[1], dma::ChannelTokenSize);
  return llvm::Error::success();
}

} // namespace neverd::emulation
