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
  auto MDL = MDLs.find(A[2]);
  if (MDL == MDLs.end() || MDL->second.Owner == LockedMdl::Ownership::Driver)
    return transferError(
        "GetScatterGatherList requires a locked or nonpaged MDL");
  const auto &View = MDL->second;
  const uint64_t Virtual = View.Owner == LockedMdl::Ownership::Request
                               ? View.UserAddress
                               : View.Buffer;
  const uint32_t Length = uint32_t(A[4]);
  if (!Length || A[3] < Virtual || A[3] - Virtual >= View.ByteCount ||
      Length > View.ByteCount - (A[3] - Virtual) || !A[5])
    return transferError(
        "CurrentVa and Length exceed the MDL, or callback is absent");
  const bool ToDevice = bool(uint8_t(A[7]));
  if (!ToDevice && !View.DmaWritable)
    return transferError("device writes require a write-locked MDL");
  const uint64_t Backing = View.Buffer + (A[3] - Virtual);
  auto Owner = Physical.ownerForRange(Backing, Length);
  if (!Owner)
    return Owner.takeError();
  const auto *Region = Physical.find(*Owner);
  auto Planned =
      DMA.planMapping(A[0], *Owner, Backing - Region->Backing, Length, false,
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
  Plan.MDL = MDL->first;
  Plan.MDLSize = View.Size;
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

llvm::Error KernelModel::releaseDMAMapping(const KernelDMA::ReleasePlan &Plan) {
  std::vector<uint64_t> Ready;
  for (uint64_t Object : Plan.Ready) {
    const auto *Map = DMA.mapping(Object);
    if (!Map || !Map->SchedulerID)
      return transferError("promoted mapping lost its callback reservation");
    Ready.push_back(Map->SchedulerID);
  }
  if (auto E = Scheduler.canReadyDMAListControls(Ready))
    return E;
  if (auto E = DMA.releaseMapping(Plan))
    return E;
  return Scheduler.readyDMAListControls(Ready);
}

llvm::Error KernelModel::putScatterGatherList(llvm::ArrayRef<uint64_t> A) {
  const auto *Found = DMA.mapping(A[1]);
  const auto Direction = uint8_t(A[2]) ? DriverDmaDirection::ReadMemory
                                       : DriverDmaDirection::WriteMemory;
  if (!Found || Found->Common || Found->Adapter != A[0] ||
      Found->Direction != Direction)
    return transferError(
        "PutScatterGatherList requires its exact adapter/list/direction");
  const auto Map = *Found;
  auto Release = DMA.planRelease(Map.Object);
  if (!Release)
    return Release.takeError();
  std::vector<uint64_t> Ready;
  for (uint64_t Object : Release->Ready)
    Ready.push_back(DMA.mapping(Object)->SchedulerID);
  if (auto E = Scheduler.canReadyDMAListControls(Ready))
    return E;
  if (auto E = prepareReleaseRange(Map.Object, Map.StorageSize))
    return E;
  if (auto E = releaseDMAMapping(*Release))
    return E;
  FreedRanges.emplace(Map.Object, Map.StorageSize);
  return llvm::Error::success();
}

llvm::Error KernelModel::beginDMACall(uint64_t Object) {
  const auto *Call = DMA.callback(Object);
  if (!Call || CurrentIRQL != scheduler::DispatchLevel)
    return transferError(
        "list callback requires its live context at DISPATCH_LEVEL");
  if (auto E = DMA.canBeginCallback(Object))
    return E;
  if (InlineDMACalls.count(Object)) {
    if (auto E = Scheduler.beginInlineDMAListControl(Call->SchedulerID))
      return E;
  } else if (!Scheduler.active() ||
             Scheduler.active()->ID != Call->SchedulerID ||
             Scheduler.active()->Kind !=
                 KernelScheduler::CallbackKind::DMAListControl) {
    return transferError("list callback lost its active scheduler identity");
  }
  return DMA.beginCallback(Object);
}

llvm::Expected<std::optional<uint64_t>>
KernelModel::finishDMACall(uint64_t Object) {
  const auto *Found = DMA.callback(Object);
  if (!Found || CurrentIRQL != scheduler::DispatchLevel)
    return transferError(
        "list callback return lost its identity or DISPATCH_LEVEL");
  const auto Call = *Found;
  const bool Inline = InlineDMACalls.count(Object);
  if (auto E = DMA.canFinishCallback(Object))
    return E;
  if (Inline)
    if (auto E = Scheduler.canFinishInlineDMAListControl(Call.SchedulerID))
      return E;
  if (auto E = DMA.finishCallback(Object))
    return E;
  if (Inline) {
    if (auto E = Scheduler.finishInlineDMAListControl(Call.SchedulerID))
      return E;
    InlineDMACalls.erase(Object);
    if (auto E = retireDeviceIfUnreferenced(Call.Device))
      return E;
  }
  // AdapterListControl is void: its return register cannot become Get's status.
  return std::optional<uint64_t>{windows::StatusSuccess};
}
} // namespace neverd::emulation
