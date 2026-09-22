//===- KernelModelScheduling.cpp - Guest work-item ownership --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Connect Windows work-item lifetime to scheduled guest callbacks.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error schedulingError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
} // namespace

llvm::Expected<uint64_t> KernelModel::allocateWorkItem(uint64_t Device) {
  if (CurrentIRQL > scheduler::DispatchLevel)
    return schedulingError(
        "IoAllocateWorkItem requires IRQL <= DISPATCH_LEVEL");
  if (!Devices.count(Device))
    return schedulingError("IoAllocateWorkItem requires a live device");
  const uint64_t Aligned = (NextAllocation + profile::WorkItemTokenSize - 1) &
                           ~(profile::WorkItemTokenSize - 1);
  if (Aligned > AllocationEnd ||
      profile::WorkItemTokenSize > AllocationEnd - Aligned)
    return 0;
  auto Address = allocate(profile::WorkItemTokenSize);
  if (!Address)
    return Address.takeError();
  WorkItems.emplace(*Address, Device);
  return *Address;
}

llvm::Error KernelModel::queueWorkItem(llvm::ArrayRef<uint64_t> Arguments) {
  if (CurrentIRQL > scheduler::DispatchLevel)
    return schedulingError("IoQueueWorkItem requires IRQL <= DISPATCH_LEVEL");
  auto Item = WorkItems.find(Arguments[0]);
  if (Item == WorkItems.end() || !Devices.count(Item->second))
    return schedulingError(
        "IoQueueWorkItem requires a live work item and device");
  if (static_cast<uint32_t>(Arguments[2]) != profile::DelayedWorkQueue)
    return schedulingError("IoQueueWorkItem requires DelayedWorkQueue");
  KernelScheduler::Callback Callback;
  Callback.Object = Item->first;
  Callback.Owner = Item->second;
  Callback.Thread = profile::WorkerThreadIdentity;
  Callback.PC = Arguments[1];
  Callback.Arguments = {Item->second, Arguments[3]};
  auto ID = Scheduler.enqueueWorkItem(std::move(Callback));
  if (!ID)
    return ID.takeError();
  ++WorkReferences[Item->second];
  return updateDeviceReferences(Item->second);
}

llvm::Error KernelModel::freeWorkItem(uint64_t Address) {
  if (CurrentIRQL > scheduler::DispatchLevel)
    return schedulingError("IoFreeWorkItem requires IRQL <= DISPATCH_LEVEL");
  auto Item = WorkItems.find(Address);
  if (Item == WorkItems.end())
    return schedulingError(
        "IoFreeWorkItem requires a live allocated work item");
  if (Scheduler.isWorkItemQueued(Address))
    return schedulingError("IoFreeWorkItem cannot free a queued work item");
  // Windows dequeues before invoking the callback; it may free its own item.
  // The scheduler retains the device reference until that invocation returns.
  WorkItems.erase(Item);
  FreedRanges.emplace(Address, profile::WorkItemTokenSize);
  return llvm::Error::success();
}

llvm::Error KernelModel::updateDeviceReferences(uint64_t Device) {
  const uint64_t OpenCount =
      std::count_if(Files.begin(), Files.end(), [&](const auto &Entry) {
        return Entry.second.Device == Device &&
               Entry.second.State == FileState::Open;
      });
  return Memory.writeInteger(Device + windows::DeviceReferenceCount,
                             OpenCount + WorkReferences[Device], 4);
}

llvm::Expected<std::optional<KernelScheduler::Invocation>>
KernelModel::nextScheduled(bool AdvanceTime) {
  auto Next = Scheduler.next(AdvanceTime);
  if (!Next)
    return Next.takeError();
  if (*Next)
    CurrentIRQL = (**Next).IRQL;
  return std::move(*Next);
}

llvm::Error KernelModel::finishScheduled(uint64_t ID) {
  if (!Scheduler.active() || Scheduler.active()->ID != ID)
    return schedulingError("callback completion does not match active task");
  const auto Invocation = *Scheduler.active();
  if (auto E = Scheduler.finish(ID))
    return E;
  CurrentIRQL = scheduler::PassiveLevel;
  if (Invocation.Kind == KernelScheduler::CallbackKind::WorkItem) {
    auto Reference = WorkReferences.find(Invocation.Owner);
    if (Reference == WorkReferences.end() || !Reference->second)
      return schedulingError("work item lost its device reference");
    --Reference->second;
    if (auto E = updateDeviceReferences(Invocation.Owner))
      return E;
    return retireDeviceIfUnreferenced(Invocation.Owner);
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
