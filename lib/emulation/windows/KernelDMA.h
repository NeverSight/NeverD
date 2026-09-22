//===- KernelDMA.h - Adapter domains and pinned DMA ownership -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Keep adapter handles, physical views, device logical mappings and guest
/// callback lifetimes separate. All data remains in the physical RAM authority.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELDMA_H
#define NEVERD_EMULATION_KERNELDMA_H

#include "KernelPhysicalMemory.h"
#include "KernelResources.h"

#include "neverd/emulation/DriverSession.h"

#include <deque>
#include <map>
#include <set>

namespace neverd::emulation {
namespace dma {
#define NEVERD_KERNEL_DMA_VALUE(Name, Value)                                   \
  inline constexpr uint64_t Name = Value;
#include "KernelDMAValues.def"
#undef NEVERD_KERNEL_DMA_VALUE
} // namespace dma

class KernelDMA {
public:
  KernelDMA(KernelPhysicalMemory &Physical, const KernelResources &Resources,
            DriverResult &Result)
      : Physical(Physical), Resources(Resources), Result(Result) {}

  struct Adapter {
    uint64_t Object = 0;
    uint64_t Table = 0;
    uint64_t PDO = 0;
    uint64_t AddressMask = 0;
    uint32_t MaximumLength = 0;
    uint32_t MapRegisters = 0;
    uint32_t Alignment = 0;
    bool ScatterGather = false;
    bool Alive = true;
  };
  /// Validate explicit bus-master capability, without requiring START or D0.
  llvm::Error canCreateAdapter(const Adapter &Candidate) const;
  llvm::Error createAdapter(Adapter Candidate);
  const Adapter *adapter(uint64_t Object) const;
  llvm::Error canPutAdapter(uint64_t Object) const;
  llvm::Error putAdapter(uint64_t Object);

  struct Mapping {
    uint64_t Object = 0; // Common VA or unique guest SG-list allocation.
    uint64_t Adapter = 0;
    uint64_t PDO = 0;
    uint64_t Owner = 0;
    uint64_t Backing = 0;
    uint64_t Offset = 0;
    uint64_t Pin = 0;
    uint64_t Logical = 0;
    uint64_t NextLogical = 0;
    uint64_t StorageSize = 0;
    uint32_t Length = 0;
    uint32_t Registers = 0;
    bool Common = false;
    bool Live = false;
    DriverDmaDirection Direction = DriverDmaDirection::ReadMemory;
    uint64_t MDL = 0;
    uint64_t MDLSize = 0;
    uint64_t Device = 0;
    uint64_t DeviceSize = 0;
    uint64_t Routine = 0;
    uint64_t Context = 0;
    uint64_t SchedulerID = 0;
  };
  /// Plan a logical span against immutable existing backing. Object/storage and
  /// callback metadata are assigned by the bridge before publishing the map.
  llvm::Expected<std::optional<Mapping>>
  planMapping(uint64_t Adapter, uint64_t Owner, uint64_t Offset,
              uint32_t Length, bool Common, DriverDmaDirection Direction) const;
  llvm::Error canPublishMapping(const Mapping &Plan) const;
  llvm::Error publishMapping(Mapping Plan);
  const Mapping *mapping(uint64_t Object) const;
  /// Encoded public SG metadata refers to device logical addresses.
  llvm::Expected<std::vector<uint8_t>>
  scatterGatherBytes(const Mapping &Map) const;

  struct ReleasePlan {
    uint64_t Object = 0;
    /// Map IDs in per-domain FIFO order; callback capacity was reserved at Get.
    std::vector<uint64_t> Ready;
  };
  llvm::Expected<ReleasePlan> planRelease(uint64_t Object) const;
  llvm::Error releaseMapping(const ReleasePlan &Plan);
  /// Callback records survive Put and even logical adapter retirement.
  const Mapping *callback(uint64_t Object) const;
  bool hasPendingCallbacks() const;
  llvm::Error canBeginCallback(uint64_t Object) const;
  llvm::Error beginCallback(uint64_t Object);
  llvm::Error canFinishCallback(uint64_t Object) const;
  llvm::Error finishCallback(uint64_t Object);
  llvm::Error canReleasePDO(uint64_t PDO) const;
  llvm::Error canReleaseRange(uint64_t Base, uint64_t Size) const;
  llvm::Error validateGuestAccess(uint64_t Address, uint32_t Size,
                                  bool IsWrite) const;

  llvm::Error canArm(llvm::ArrayRef<DriverDmaEvent> Events, size_t SourceIndex,
                     uint64_t Now) const;
  llvm::Error arm(llvm::ArrayRef<DriverDmaEvent> Events, size_t SourceIndex,
                  uint64_t Now);
  std::optional<uint64_t> nextEventTime() const;
  bool hasPendingEvents() const { return !Events.empty(); }
  llvm::Error processEvents(uint64_t Now);

private:
  struct Domain {
    uint64_t NextLogical = 0;
    uint32_t UsedRegisters = 0;
    std::deque<uint64_t> Waiting;
  };
  struct Callback {
    Mapping Arguments;
    bool Begun = false;
    bool Returned = false;
  };
  struct Event {
    uint64_t PDO = 0;
    uint64_t Epoch = 0;
    DriverDmaEvent Input;
    uint64_t Due = 0;
    size_t Observation = 0;
  };
  KernelPhysicalMemory &Physical;
  const KernelResources &Resources;
  DriverResult &Result;
  std::map<uint64_t, Adapter> Adapters;
  std::map<uint64_t, Domain> Domains;
  std::map<uint64_t, Mapping> Mappings;
  std::map<uint64_t, Callback> Callbacks;
  std::set<uint64_t> RetiredMappings;
  std::map<size_t, Event> Events;
  uint64_t ObservedBytes = 0;
  llvm::Expected<Event> resolveEvent(const DriverDmaEvent &Event,
                                     uint64_t Now) const;
  llvm::Error executeEvent(Event &Event, uint64_t Now);
};

} // namespace neverd::emulation
#endif // NEVERD_EMULATION_KERNELDMA_H
