//===- KernelModelDMATransfers.cpp - MDL views and DMA guest callbacks
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Successful packet admission pins a physical view and reserves a real guest
/// callback, including when another mapping currently owns the map registers.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

namespace neverd::emulation {
namespace {
llvm::Error transferError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "DMA transfer: " + Text);
}
KernelScheduler::CallbackKind schedulerKind(KernelDMA::CallbackKind Kind) {
  return Kind == KernelDMA::CallbackKind::AdapterControl
             ? KernelScheduler::CallbackKind::DMAAdapterControl
             : KernelScheduler::CallbackKind::DMAListControl;
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::getScatterGatherList(llvm::ArrayRef<uint64_t> A) {
  if (CurrentIRQL != scheduler::DispatchLevel || hasPendingModelGuestCall())
    return transferError(
        "GetScatterGatherList requires DISPATCH_LEVEL and no pending callback");
  const auto *Adapter = DMA.adapter(A[0]);
  auto Device = Devices.find(A[1]);
  if (!Adapter || Device == Devices.end() || Device->second.DeletePending ||
      Device->second.OwnerKind != DeviceOwnerKind::Guest ||
      Device->second.PnpDevice != Adapter->PDO)
    return transferError(
        "GetScatterGatherList requires its adapter's live guest device");
  if (!A[5])
    return transferError("GetScatterGatherList requires a callback");
  const uint32_t Length = uint32_t(A[4]);
  const bool ToDevice = bool(uint8_t(A[7]));
  auto View = dmaMdlView(A[2], A[3], Length, ToDevice);
  if (!View)
    return View.takeError();
  auto Planned = DMA.planMapping(A[0], View->Owner, View->Offset, Length, false,
                                 ToDevice ? DriverDmaDirection::ReadMemory
                                          : DriverDmaDirection::WriteMemory);
  if (!Planned)
    return Planned.takeError();
  if (!*Planned)
    return windows::StatusInsufficientResources;
  auto Plan = **Planned;
  Plan.Object = (NextAllocation + 15) & ~uint64_t(15);
  Plan.StorageSize = dma::ScatterGatherHeaderSize +
                     Plan.Registers * dma::ScatterGatherElementSize;
  if (Plan.Object > AllocationEnd ||
      Plan.StorageSize > AllocationEnd - Plan.Object)
    return windows::StatusInsufficientResources;
  Plan.MDL = A[2];
  Plan.MDLSize = View->DescriptorSize;
  Plan.Device = Device->first;
  Plan.DeviceSize = Device->second.Size;
  Plan.Routine = A[5];
  Plan.Context = A[6];
  KernelScheduler::Callback Call;
  Call.Object = Plan.Object;
  Call.Owner = Plan.Device;
  Call.Thread = profile::WorkerThreadIdentity;
  Call.PC = Plan.Routine;
  // This profile has no StartIo ownership. An active request is not the
  // device's CurrentIrp, and arbitrary driver Context is never an IRP token.
  Call.Arguments = {Plan.Device, 0, Plan.Object, Plan.Context};
  if (auto E = Scheduler.canReserveDMAListControl(Call))
    return E;
  if (Plan.Live)
    if (auto E = Scheduler.canDispatchInlineDMAListControl())
      return E;
  auto Encoded = DMA.scatterGatherBytes(Plan);
  if (!Encoded)
    return Encoded.takeError();
  auto Storage = allocate(Plan.StorageSize);
  if (!Storage)
    return Storage.takeError();
  if (*Storage != Plan.Object)
    return transferError("SG-list allocation changed after preflight");
  if (auto E = Memory.write(Plan.Object, *Encoded))
    return E;
  auto ID = Scheduler.reserveDMAListControl(Call);
  if (!ID)
    return ID.takeError();
  Plan.SchedulerID = *ID;
  if (auto E = DMA.publishMapping(Plan))
    return E;
  KernelGuestCall Guest{{GuestCallOwner::DMA, Plan.Object},
                        Plan.Routine,
                        std::move(Call.Arguments)};
  if (Plan.Live) {
    InlineDMACalls.insert(Plan.Object);
    PendingDMACall = std::move(Guest);
  } else {
    ScheduledModelContinuations.emplace(*ID, Guest.Token);
  }
  return windows::StatusSuccess;
}

llvm::Expected<std::vector<uint64_t>> KernelModel::dmaPromotionIDs(
    llvm::ArrayRef<KernelDMA::Promotion> Promotions) const {
  std::vector<uint64_t> IDs;
  for (const auto &Promotion : Promotions) {
    const auto Call = DMA.callbackInfo(Promotion.Object);
    if (!Call || Call->SchedulerID != Promotion.SchedulerID ||
        Call->Kind != Promotion.Kind)
      return transferError("promoted mapping lost its callback reservation");
    IDs.push_back(Promotion.SchedulerID);
  }
  if (auto E = Scheduler.canReadyDMACallbacks(IDs))
    return E;
  return IDs;
}

llvm::Error KernelModel::releaseDMAMapping(const KernelDMA::ReleasePlan &Plan) {
  auto Ready = dmaPromotionIDs(Plan.Ready);
  if (!Ready)
    return Ready.takeError();
  if (auto E = DMA.releaseMapping(Plan))
    return E;
  return Scheduler.readyDMACallbacks(*Ready);
}

llvm::Error KernelModel::putScatterGatherList(llvm::ArrayRef<uint64_t> A) {
  const auto *Found = DMA.mapping(A[1]);
  const auto Direction = uint8_t(A[2]) ? DriverDmaDirection::ReadMemory
                                       : DriverDmaDirection::WriteMemory;
  if (!Found || Found->Common || Found->Channel || Found->Adapter != A[0] ||
      Found->Direction != Direction)
    return transferError(
        "PutScatterGatherList requires its exact adapter/list/direction");
  const auto Map = *Found;
  auto Release = DMA.planRelease(Map.Object);
  if (!Release)
    return Release.takeError();
  auto Ready = dmaPromotionIDs(Release->Ready);
  if (!Ready)
    return Ready.takeError();
  if (auto E = prepareReleaseRange(Map.Object, Map.StorageSize))
    return E;
  if (auto E = releaseDMAMapping(*Release))
    return E;
  FreedRanges.emplace(Map.Object, Map.StorageSize);
  return llvm::Error::success();
}

llvm::Error KernelModel::beginDMACall(uint64_t Object) {
  const auto Call = DMA.callbackInfo(Object);
  if (!Call || CurrentIRQL != scheduler::DispatchLevel)
    return transferError(
        "DMA callback requires its live context at DISPATCH_LEVEL");
  if (auto E = DMA.canBeginCallback(Object))
    return E;
  if (InlineDMACalls.count(Object)) {
    if (auto E = Scheduler.beginInlineDMACallback(Call->SchedulerID,
                                                  schedulerKind(Call->Kind)))
      return E;
  } else if (!Scheduler.active() ||
             Scheduler.active()->ID != Call->SchedulerID ||
             Scheduler.active()->Kind != schedulerKind(Call->Kind)) {
    return transferError("DMA callback lost its active scheduler identity");
  }
  return DMA.beginCallback(Object);
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishDMACall(uint64_t Object, uint64_t Result) {
  const auto Found = DMA.callbackInfo(Object);
  if (!Found || CurrentIRQL != scheduler::DispatchLevel)
    return transferError(
        "DMA callback return lost its identity or DISPATCH_LEVEL");
  const auto Call = *Found;
  const bool Inline = InlineDMACalls.count(Object);
  std::optional<KernelDMA::ChannelReturnPlan> ChannelReturn;
  std::vector<uint64_t> Ready;
  if (Call.Kind == KernelDMA::CallbackKind::AdapterControl) {
    auto Plan = DMA.planChannelReturn(Object, uint32_t(Result));
    if (!Plan)
      return Plan.takeError();
    auto IDs = dmaPromotionIDs(Plan->Ready);
    if (!IDs)
      return IDs.takeError();
    Ready = std::move(*IDs);
    ChannelReturn = std::move(*Plan);
  } else if (auto E = DMA.canFinishCallback(Object)) {
    return E;
  }
  if (Inline) {
    if (auto E = Scheduler.canFinishInlineDMACallback(Call.SchedulerID,
                                                      schedulerKind(Call.Kind)))
      return E;
  } else {
    if (!Scheduler.active() ||
        Scheduler.active()->Kind != schedulerKind(Call.Kind))
      return transferError("DMA return lost its active scheduler kind");
    if (auto E = Scheduler.canFinish(Call.SchedulerID))
      return E;
  }
  if (ChannelReturn) {
    if (auto E = DMA.finishChannelReturn(*ChannelReturn))
      return E;
    if (auto E = Scheduler.readyDMACallbacks(Ready))
      return E;
    if (ChannelReturn->Action == dma::DeallocateObject)
      FreedRanges.emplace(Object, dma::ChannelTokenSize);
  } else if (auto E = DMA.finishCallback(Object)) {
    return E;
  }
  if (Inline) {
    if (auto E = Scheduler.finishInlineDMACallback(Call.SchedulerID,
                                                   schedulerKind(Call.Kind)))
      return E;
    InlineDMACalls.erase(Object);
    if (auto E = retireDeviceIfUnreferenced(Call.Device))
      return E;
  }
  // Both allocation APIs resume with acceptance status. The void list callback
  // ignores RAX; AdapterControl's low 32 bits only select resource disposition.
  return std::optional<uint64_t>{windows::StatusSuccess};
}
} // namespace neverd::emulation
