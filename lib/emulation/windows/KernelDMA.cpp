//===- KernelDMA.cpp - DMA adapter, mapping and callback lifetimes --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Coherent translated bus-master mappings retain exact backing views. Resource
/// release promotes preallocated FIFO waiters without executing guest code.
///
//===----------------------------------------------------------------------===//

#include "KernelDMA.h"

#include <algorithm>
#include <limits>

namespace neverd::emulation {
namespace {
llvm::Error dmaError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "DMA: " + Message);
}
bool overlaps(uint64_t A, uint64_t N, uint64_t B, uint64_t M) {
  return N && M && A < B + M && B < A + N;
}
} // namespace

const KernelDMA::Adapter *KernelDMA::adapter(uint64_t Object) const {
  const auto I = Adapters.find(Object);
  return I == Adapters.end() || !I->second.Alive ? nullptr : &I->second;
}

llvm::Error KernelDMA::canCreateAdapter(const Adapter &Candidate) const {
  const auto *Device = Resources.find(Candidate.PDO);
  if (!Device || !Device->Dma || !Device->Present)
    return dmaError(
        "adapter requires a present PDO with explicit DMA capability");
  const auto &Config = *Device->Dma;
  if (!Candidate.Object || !Candidate.Table ||
      Candidate.Object > UINT64_MAX - dma::AdapterSize ||
      Candidate.Table > UINT64_MAX - dma::OperationsSize ||
      Adapters.count(Candidate.Object) || Adapters.size() >= dma::MaxAdapters)
    return dmaError("invalid, reused or exhausted adapter identity");
  if (!Candidate.Alive || !Candidate.MaximumLength ||
      Candidate.MaximumLength > Config.MaximumLength ||
      Candidate.MapRegisters != Config.MapRegisters ||
      Candidate.Alignment != Config.Alignment ||
      (Candidate.ScatterGather && !Config.ScatterGather) ||
      (Candidate.AddressMask != UINT32_MAX &&
       Candidate.AddressMask != UINT64_MAX) ||
      Config.LogicalBase > Candidate.AddressMask ||
      Config.LogicalLength - 1 > Candidate.AddressMask - Config.LogicalBase)
    return dmaError(
        "adapter request does not match its explicit logical domain");
  return llvm::Error::success();
}

llvm::Error KernelDMA::createAdapter(Adapter Candidate) {
  if (auto E = canCreateAdapter(Candidate))
    return E;
  const auto &Config = *Resources.find(Candidate.PDO)->Dma;
  Domains.try_emplace(Candidate.PDO, Domain{Config.LogicalBase, 0, {}});
  Adapters.emplace(Candidate.Object, Candidate);
  return llvm::Error::success();
}

llvm::Error KernelDMA::canPutAdapter(uint64_t Object) const {
  if (!adapter(Object))
    return dmaError("adapter is unknown or already released");
  for (const auto &[ID, Map] : Mappings) {
    (void)ID;
    if (Map.Adapter == Object)
      return dmaError("adapter still owns a common buffer or accepted DMA map");
  }
  return llvm::Error::success();
}

llvm::Error KernelDMA::putAdapter(uint64_t Object) {
  if (auto E = canPutAdapter(Object))
    return E;
  // A returning callback retains its internal metadata, never a live guest
  // handle or a reference to the already released SG list/IRP.
  Adapters.at(Object).Alive = false;
  return llvm::Error::success();
}

llvm::Expected<std::optional<KernelDMA::Mapping>>
KernelDMA::planMapping(uint64_t Object, uint64_t Owner, uint64_t Offset,
                       uint32_t Length, bool Common,
                       DriverDmaDirection Direction) const {
  const auto *Adapter = adapter(Object);
  if (!Adapter)
    return dmaError("mapping requires its live DMA adapter");
  if (!Length || Length > DriverDmaMaximumLengthLimit ||
      (Direction != DriverDmaDirection::ReadMemory &&
       Direction != DriverDmaDirection::WriteMemory))
    return dmaError("invalid DMA byte extent or direction");
  if (!Common && (!Adapter->ScatterGather || !Owner))
    return dmaError(
        "packet DMA requires a scatter/gather adapter and RAM owner");
  if (!Common && Length > Adapter->MaximumLength)
    return std::optional<Mapping>{};
  uint64_t Backing = 0;
  if (Owner) {
    const auto *Region = Physical.find(Owner);
    if (!Region || Offset > Region->Size || Length > Region->Size - Offset)
      return dmaError("mapping exceeds its live physical allocation view");
    if (auto E = Physical.canPin(Owner, Offset, Length))
      return E;
    Backing = Region->Backing + Offset;
  } else if (Offset) {
    return dmaError("unallocated common-buffer plan must be page aligned");
  }
  const uint64_t PageOffset = Backing & (DriverDmaPageSize - 1);
  const uint64_t PageCount =
      (PageOffset + Length + DriverDmaPageSize - 1) / DriverDmaPageSize;
  if (PageCount > Adapter->MapRegisters)
    return std::optional<Mapping>{};
  if (Backing & (Adapter->Alignment - 1))
    return dmaError("DMA buffer does not satisfy the adapter alignment");
  const auto &Domain = Domains.at(Adapter->PDO);
  const auto &Config = *Resources.find(Adapter->PDO)->Dma;
  const uint64_t End = Config.LogicalBase + Config.LogicalLength;
  const uint64_t Bytes = PageCount * DriverDmaPageSize;
  if (Domain.NextLogical > End || Bytes > End - Domain.NextLogical)
    return std::optional<Mapping>{};
  if (Mappings.size() + RetiredMappings.size() >= dma::MaxMappings)
    return dmaError("mapping identity budget exhausted");
  const bool Available =
      Domain.Waiting.empty() &&
      PageCount <= Config.MapRegisters - Domain.UsedRegisters;
  if (Common && !Available)
    return std::optional<Mapping>{};
  if (!Common && Callbacks.size() >= dma::MaxCallbacks)
    return dmaError("DMA callback metadata budget exhausted");
  Mapping Plan;
  Plan.Adapter = Object;
  Plan.PDO = Adapter->PDO;
  Plan.Owner = Owner;
  Plan.Backing = Backing;
  Plan.Offset = Offset;
  Plan.Logical = Domain.NextLogical + PageOffset;
  Plan.NextLogical = Domain.NextLogical + Bytes;
  Plan.Length = Length;
  Plan.Registers = uint32_t(PageCount);
  Plan.Common = Common;
  Plan.Live = Available;
  Plan.Direction = Direction;
  return std::optional<Mapping>{Plan};
}

llvm::Error KernelDMA::canPublishMapping(const Mapping &Plan) const {
  auto Current = planMapping(Plan.Adapter, Plan.Owner, Plan.Offset, Plan.Length,
                             Plan.Common, Plan.Direction);
  if (!Current)
    return Current.takeError();
  if (!*Current)
    return dmaError("mapping resources changed after preparation");
  const auto &Expected = **Current;
  if (!Plan.Object || !Plan.Owner || Plan.Pin || Mappings.count(Plan.Object) ||
      RetiredMappings.count(Plan.Object) || Expected.PDO != Plan.PDO ||
      Expected.Backing != Plan.Backing || Expected.Logical != Plan.Logical ||
      Expected.NextLogical != Plan.NextLogical ||
      Expected.Registers != Plan.Registers || Expected.Live != Plan.Live)
    return dmaError("mapping plan lost its allocation or logical identity");
  if (Plan.StorageSize > UINT64_MAX - Plan.Object)
    return dmaError("mapping storage range overflows");
  if (Plan.Common) {
    if (Plan.Object != Plan.Backing || Plan.Offset ||
        Plan.StorageSize < Plan.Length || Plan.MDL || Plan.SchedulerID)
      return dmaError("common buffer requires its exact page allocation");
  } else if (!Plan.MDL || !Plan.MDLSize || !Plan.Device || !Plan.DeviceSize ||
             !Plan.Routine || !Plan.SchedulerID ||
             Callbacks.count(Plan.Object) ||
             Plan.MDLSize > UINT64_MAX - Plan.MDL ||
             Plan.DeviceSize > UINT64_MAX - Plan.Device ||
             Plan.StorageSize !=
                 dma::ScatterGatherHeaderSize +
                     Plan.Registers * dma::ScatterGatherElementSize) {
    return dmaError("packet DMA lost descriptor, device or callback identity");
  }
  return llvm::Error::success();
}

llvm::Error KernelDMA::publishMapping(Mapping Plan) {
  if (auto E = canPublishMapping(Plan))
    return E;
  auto Pin = Physical.pin(Plan.Owner, Plan.Offset, Plan.Length);
  if (!Pin)
    return Pin.takeError();
  Plan.Pin = *Pin;
  auto &Domain = Domains.at(Plan.PDO);
  Domain.NextLogical = Plan.NextLogical;
  if (Plan.Live)
    Domain.UsedRegisters += Plan.Registers;
  else
    Domain.Waiting.push_back(Plan.Object);
  if (!Plan.Common)
    Callbacks.emplace(Plan.Object, Callback{Plan, false, false});
  Mappings.emplace(Plan.Object, Plan);
  return llvm::Error::success();
}

const KernelDMA::Mapping *KernelDMA::mapping(uint64_t Object) const {
  const auto I = Mappings.find(Object);
  return I == Mappings.end() ? nullptr : &I->second;
}

llvm::Expected<std::vector<uint8_t>>
KernelDMA::scatterGatherBytes(const Mapping &Map) const {
  if (Map.Common || !Map.Owner)
    return dmaError("scatter/gather metadata requires a packet view");
  auto Segments = Physical.describe(Map.Owner, Map.Offset, Map.Length);
  if (!Segments)
    return Segments.takeError();
  if (Segments->size() != Map.Registers)
    return dmaError("physical view changed its scatter/gather page count");
  std::vector<uint8_t> Bytes(dma::ScatterGatherHeaderSize +
                             Map.Registers * dma::ScatterGatherElementSize);
  auto Put = [&](uint64_t Offset, uint64_t Value, unsigned Size) {
    for (unsigned I = 0; I < Size; ++I)
      Bytes[Offset + I] = uint8_t(Value >> (I * 8));
  };
  Put(dma::ScatterGatherCountOffset, Map.Registers, 4);
  uint64_t Logical = Map.Logical;
  for (size_t I = 0; I < Segments->size(); ++I) {
    const auto &Segment = (*Segments)[I];
    const uint64_t Base =
        dma::ScatterGatherHeaderSize + I * dma::ScatterGatherElementSize;
    Put(Base + dma::ScatterGatherAddressOffset, Logical, 8);
    Put(Base + dma::ScatterGatherLengthOffset, Segment.Length, 4);
    Logical += Segment.Length;
  }
  return Bytes;
}

llvm::Expected<KernelDMA::ReleasePlan>
KernelDMA::planRelease(uint64_t Object) const {
  const auto *Map = mapping(Object);
  if (!Map || !Map->Live)
    return dmaError("release requires its live admitted mapping");
  if (!Map->Common) {
    const auto Call = Callbacks.find(Object);
    if (Call == Callbacks.end() || !Call->second.Begun)
      return dmaError("packet DMA release requires callback delivery");
  }
  const auto &Domain = Domains.at(Map->PDO);
  if (Domain.UsedRegisters < Map->Registers)
    return dmaError("mapping lost its shared domain register charge");
  if (Map->Common)
    if (auto E = Physical.canRetire(Map->Owner, Map->Pin))
      return E;
  const auto &Config = *Resources.find(Map->PDO)->Dma;
  uint64_t Free = Config.MapRegisters - Domain.UsedRegisters + Map->Registers;
  ReleasePlan Plan{Object, {}};
  for (uint64_t ID : Domain.Waiting) {
    const auto *Waiting = mapping(ID);
    if (!Waiting || Waiting->Live || Waiting->Common ||
        Waiting->PDO != Map->PDO || !callback(ID))
      return dmaError("resource waiter lost its reserved mapping or callback");
    if (Waiting->Registers > Free)
      break;
    if (!Physical.find(Waiting->Owner))
      return dmaError("resource waiter lost its pinned physical allocation");
    Plan.Ready.push_back(ID);
    Free -= Waiting->Registers;
  }
  return Plan;
}

llvm::Error KernelDMA::releaseMapping(const ReleasePlan &Plan) {
  auto Current = planRelease(Plan.Object);
  if (!Current)
    return Current.takeError();
  if (Current->Ready != Plan.Ready)
    return dmaError("resource release plan changed before publication");
  const auto Old = Mappings.at(Plan.Object);
  if (auto E = Physical.unpin(Old.Pin))
    return E;
  auto &Domain = Domains.at(Old.PDO);
  Domain.UsedRegisters -= Old.Registers;
  Mappings.erase(Plan.Object);
  RetiredMappings.insert(Plan.Object);
  if (auto I = Callbacks.find(Plan.Object);
      I != Callbacks.end() && I->second.Returned)
    Callbacks.erase(I);
  for (uint64_t ID : Plan.Ready) {
    auto &Map = Mappings.at(ID);
    Map.Live = true;
    Domain.UsedRegisters += Map.Registers;
    Callbacks.at(ID).Arguments.Live = true;
    Domain.Waiting.pop_front();
  }
  return llvm::Error::success();
}

const KernelDMA::Mapping *KernelDMA::callback(uint64_t Object) const {
  const auto I = Callbacks.find(Object);
  return I == Callbacks.end() || I->second.Returned ? nullptr
                                                    : &I->second.Arguments;
}

bool KernelDMA::hasPendingCallbacks() const {
  return std::any_of(Callbacks.begin(), Callbacks.end(),
                     [](const auto &Entry) { return !Entry.second.Returned; });
}

llvm::Error KernelDMA::canBeginCallback(uint64_t Object) const {
  const auto I = Callbacks.find(Object);
  if (I == Callbacks.end() || I->second.Begun || I->second.Returned ||
      !I->second.Arguments.Live || !mapping(Object))
    return dmaError("callback is unknown, waiting or already entered");
  return llvm::Error::success();
}

llvm::Error KernelDMA::beginCallback(uint64_t Object) {
  if (auto E = canBeginCallback(Object))
    return E;
  Callbacks.at(Object).Begun = true;
  return llvm::Error::success();
}

llvm::Error KernelDMA::canFinishCallback(uint64_t Object) const {
  const auto I = Callbacks.find(Object);
  if (I == Callbacks.end() || !I->second.Begun || I->second.Returned)
    return dmaError("callback return lost its independent owner");
  return llvm::Error::success();
}

llvm::Error KernelDMA::finishCallback(uint64_t Object) {
  if (auto E = canFinishCallback(Object))
    return E;
  const auto I = Callbacks.find(Object);
  I->second.Returned = true;
  if (!mapping(Object))
    Callbacks.erase(I);
  return llvm::Error::success();
}

llvm::Error KernelDMA::canReleasePDO(uint64_t PDO) const {
  for (const auto &[ID, Adapter] : Adapters) {
    (void)ID;
    if (Adapter.PDO == PDO && Adapter.Alive)
      return dmaError("device still owns a DMA adapter");
  }
  for (const auto &[ID, Callback] : Callbacks) {
    (void)ID;
    if (Callback.Arguments.PDO == PDO && !Callback.Returned)
      return dmaError("device still owns an accepted DMA callback");
  }
  for (const auto &[ID, Event] : Events) {
    (void)ID;
    if (Event.PDO == PDO)
      return dmaError("device still owns an explicit DMA transaction");
  }
  return llvm::Error::success();
}

llvm::Error KernelDMA::canReleaseRange(uint64_t Base, uint64_t Size) const {
  if (Size > UINT64_MAX - Base)
    return dmaError("release range overflows");
  for (const auto &[ID, Map] : Mappings) {
    (void)ID;
    if (!Map.Common && overlaps(Base, Size, Map.MDL, Map.MDLSize))
      return dmaError("MDL still owns an accepted DMA mapping");
  }
  for (const auto &[ID, Callback] : Callbacks) {
    (void)ID;
    const auto &Map = Callback.Arguments;
    if (!Callback.Returned && overlaps(Base, Size, Map.Device, Map.DeviceSize))
      return dmaError("device storage still owns a DMA callback");
  }
  return llvm::Error::success();
}

llvm::Error KernelDMA::validateGuestAccess(uint64_t Address, uint32_t Size,
                                           bool IsWrite) const {
  if (Size > UINT64_MAX - Address)
    return dmaError("guest access range overflows");
  for (const auto &[ID, Adapter] : Adapters) {
    (void)ID;
    if (overlaps(Address, Size, Adapter.Object, dma::AdapterSize) ||
        overlaps(Address, Size, Adapter.Table, dma::OperationsSize)) {
      if (!Adapter.Alive)
        return dmaError(
            "guest access to a retired DMA adapter or operation table");
      if (IsWrite)
        return dmaError(
            "DMA adapter and operation records are read-only metadata");
    }
  }
  for (const auto &[ID, Map] : Mappings) {
    (void)ID;
    if (Map.Common) {
      if (overlaps(Address, Size, Map.Object, Map.StorageSize) &&
          (Address < Map.Backing || Address + Size > Map.Backing + Map.Length))
        return dmaError(
            "common-buffer access exceeds its requested byte count");
    } else {
      if (Map.Live && overlaps(Address, Size, Map.Backing, Map.Length))
        return dmaError("CPU access requires PutScatterGatherList to release "
                        "packet DMA ownership");
      if (IsWrite && overlaps(Address, Size, Map.Object, Map.StorageSize))
        return dmaError("scatter/gather metadata is model-owned and read-only");
    }
  }
  return llvm::Error::success();
}

} // namespace neverd::emulation
