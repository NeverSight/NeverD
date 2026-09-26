//===- KernelPhysicalMemory.cpp - RAM pages and allocation ownership ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stable simulated physical pages and exact live allocation/pin authority.
/// Data stays in GuestMemory; device access never changes CPU permissions.
///
//===----------------------------------------------------------------------===//

#include "KernelPhysicalMemory.h"

#include <algorithm>

namespace neverd::emulation {
char PhysicalMemoryLimitError::ID;
void PhysicalMemoryLimitError::log(llvm::raw_ostream &OS) const {
  OS << Message;
}
std::error_code PhysicalMemoryLimitError::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}
namespace {
llvm::Error physicalError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
uint64_t pageBase(uint64_t Address) {
  return Address & ~(physical::PageSize - 1);
}

bool validCacheType(KernelPhysicalMemory::CacheType Cache) {
  switch (Cache) {
  case KernelPhysicalMemory::CacheType::Cached:
  case KernelPhysicalMemory::CacheType::NonCached:
  case KernelPhysicalMemory::CacheType::WriteCombined:
    return true;
  }
  return false;
}
} // namespace

const KernelPhysicalMemory::Region *
KernelPhysicalMemory::find(uint64_t Owner) const {
  const auto I = Regions.find(Owner);
  return I == Regions.end() ? nullptr : &I->second;
}

llvm::Expected<std::vector<uint64_t>>
KernelPhysicalMemory::planRegion(uint64_t Owner, uint64_t Backing,
                                 uint64_t Size, CacheType Cache) const {
  if (!validCacheType(Cache))
    return physicalError("physical RAM has an unsupported cache attribute");
  if (!Owner || UsedOwners.count(Owner))
    return physicalError("physical RAM owner is zero, duplicate or retired");
  if (!Size || Size > UINT64_MAX - Backing)
    return physicalError("physical RAM region requires a nonempty, "
                         "nonoverflowing range");
  if (UsedOwners.size() >= physical::RegionLimit)
    return llvm::make_error<PhysicalMemoryLimitError>(
        "physical RAM owner registration limit exceeded");
  auto Next = OwnersByAddress.lower_bound(Backing);
  if (Next != OwnersByAddress.end() && Size > Next->first - Backing)
    return physicalError("physical RAM region overlaps a live owner");
  if (Next != OwnersByAddress.begin()) {
    const auto Previous = std::prev(Next);
    if (Backing - Previous->first < Regions.at(Previous->second).Size)
      return physicalError("physical RAM region overlaps a live owner");
  }
  if (auto E = Memory.validateBacking(Backing, Size))
    return std::move(E);
  std::vector<uint64_t> NewPages;
  const uint64_t Last = pageBase(Backing + Size - 1);
  const uint64_t Limit = physical::PhysicalSize / physical::PageSize;
  for (uint64_t Page = pageBase(Backing);; Page += physical::PageSize) {
    const auto Existing = Pages.find(Page);
    if (Existing != Pages.end() && Existing->second.Cache != Cache)
      return physicalError("physical RAM cache attribute conflicts with its "
                           "existing page");
    if (!Pages.count(Page)) {
      if (Pages.size() + NewPages.size() >= Limit)
        return llvm::make_error<PhysicalMemoryLimitError>(
            "physical RAM page identity limit exceeded");
      NewPages.push_back(Page);
    }
    if (Page == Last)
      break;
  }
  return NewPages;
}

llvm::Error KernelPhysicalMemory::canRegisterRegion(uint64_t Owner,
                                                    uint64_t Backing,
                                                    uint64_t Size,
                                                    CacheType Cache) const {
  auto Plan = planRegion(Owner, Backing, Size, Cache);
  if (!Plan)
    return Plan.takeError();
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::registerRegion(uint64_t Owner,
                                                 uint64_t Backing,
                                                 uint64_t Size,
                                                 CacheType Cache) {
  auto Plan = planRegion(Owner, Backing, Size, Cache);
  if (!Plan)
    return Plan.takeError();
  std::set<uint64_t> Used;
  for (const auto &[BackingPage, Page] : Pages)
    Used.insert(Page.Physical);
  uint64_t Candidate = physical::PhysicalBase;
  for (uint64_t Page : *Plan) {
    while (Used.contains(Candidate))
      Candidate += physical::PageSize;
    Pages.emplace(Page, PageRecord{Candidate, Cache});
    Used.insert(Candidate);
  }
  Regions.emplace(Owner, Region{Backing, Size});
  OwnersByAddress.emplace(Backing, Owner);
  UsedOwners.insert(Owner);
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint64_t>>
KernelPhysicalMemory::planAllocatedPages(uint64_t Low, uint64_t High,
                                         uint64_t Skip, uint64_t PageCount,
                                         uint64_t ChunkPages) const {
  if (Low > High || Skip % physical::PageSize || !PageCount ||
      (ChunkPages && (PageCount % ChunkPages ||
                      (Skip && ((Skip & (Skip - 1)) ||
                                ChunkPages != Skip / physical::PageSize)))))
    return physicalError("invalid physical page allocation bounds");
  std::set<uint64_t> Used;
  for (const auto &[Backing, Page] : Pages)
    Used.insert(Page.Physical);
  auto Eligible = [&](uint64_t Page) {
    if (Page < Low || Used.contains(Page))
      return false;
    uint64_t Offset = Page - Low;
    if (Skip)
      Offset %= Skip;
    const uint64_t Width = High - Low;
    return Offset <= Width && physical::PageSize - 1 <= Width - Offset;
  };
  std::vector<uint64_t> Selected;
  const uint64_t End = physical::PhysicalBase + physical::PhysicalSize;
  if (!ChunkPages) {
    for (uint64_t Page = physical::PhysicalBase;
         Page < End && Selected.size() < PageCount; Page += physical::PageSize)
      if (Eligible(Page))
        Selected.push_back(Page);
    return Selected;
  }
  if (ChunkPages > physical::PhysicalSize / physical::PageSize)
    return Selected;
  const uint64_t ChunkSize = ChunkPages * physical::PageSize;
  for (uint64_t Page = physical::PhysicalBase;
       ChunkSize <= End - Page && Selected.size() < PageCount;) {
    bool Available = !Skip || Page % ChunkSize == 0;
    for (uint64_t Offset = 0; Available && Offset < ChunkSize;
         Offset += physical::PageSize)
      Available = Eligible(Page + Offset);
    if (!Available) {
      Page += physical::PageSize;
      continue;
    }
    for (uint64_t Offset = 0; Offset < ChunkSize; Offset += physical::PageSize)
      Selected.push_back(Page + Offset);
    Page += ChunkSize;
  }
  return Selected;
}

llvm::Error KernelPhysicalMemory::registerAllocatedPages(
    uint64_t Owner, uint64_t Backing, llvm::ArrayRef<uint64_t> PhysicalPages,
    std::optional<CacheType> Cache) {
  if (PhysicalPages.empty() ||
      PhysicalPages.size() > physical::PhysicalSize / physical::PageSize ||
      Backing % physical::PageSize || (Cache && !validCacheType(*Cache)))
    return physicalError("invalid independent physical page allocation");
  const uint64_t Size = PhysicalPages.size() * physical::PageSize;
  auto Plan =
      planRegion(Owner, Backing, Size, Cache.value_or(CacheType::Cached));
  if (!Plan)
    return Plan.takeError();
  if (Plan->size() != PhysicalPages.size())
    return physicalError(
        "allocated physical pages require private new backing");
  std::set<uint64_t> Used;
  for (const auto &[Address, Page] : Pages)
    Used.insert(Page.Physical);
  for (uint64_t Page : PhysicalPages) {
    if (Page < physical::PhysicalBase ||
        Page - physical::PhysicalBase >= physical::PhysicalSize ||
        Page % physical::PageSize || !Used.insert(Page).second)
      return physicalError("physical page allocation contains an unavailable "
                           "or duplicate page");
  }
  for (size_t I = 0; I != PhysicalPages.size(); ++I)
    Pages.emplace(Backing + I * physical::PageSize,
                  PageRecord{PhysicalPages[I], Cache});
  Regions.emplace(Owner, Region{Backing, Size, true});
  OwnersByAddress.emplace(Backing, Owner);
  UsedOwners.insert(Owner);
  return llvm::Error::success();
}

llvm::Expected<KernelPhysicalMemory::CacheType>
KernelPhysicalMemory::cacheTypeForMapping(uint64_t Backing, uint64_t Size,
                                          CacheType Requested) const {
  if (!validCacheType(Requested))
    return physicalError("physical RAM mapping has an unsupported cache "
                         "attribute");
  auto Owner = ownerForRange(Backing, Size);
  if (!Owner)
    return Owner.takeError();
  std::optional<CacheType> Existing;
  const uint64_t Last = pageBase(Backing + Size - 1);
  for (uint64_t Page = pageBase(Backing);; Page += physical::PageSize) {
    const auto Cache = Pages.at(Page).Cache;
    if (Cache) {
      if (Existing && Existing != Cache)
        return physicalError("physical RAM owner has inconsistent page cache "
                             "attributes");
      Existing = Cache;
    }
    if (Page == Last)
      break;
  }
  return Existing.value_or(Requested);
}

llvm::Error KernelPhysicalMemory::commitMappingCache(uint64_t Backing,
                                                     uint64_t Size,
                                                     CacheType Cache) {
  auto Effective = cacheTypeForMapping(Backing, Size, Cache);
  if (!Effective)
    return Effective.takeError();
  if (*Effective != Cache)
    return physicalError(
        "mapping cache commit differs from its page attribute");
  const uint64_t Last = pageBase(Backing + Size - 1);
  for (uint64_t Page = pageBase(Backing);; Page += physical::PageSize) {
    Pages.at(Page).Cache = Cache;
    if (Page == Last)
      break;
  }
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::canRetire(uint64_t Owner,
                                            uint64_t IgnoredPin) const {
  if (!find(Owner))
    return physicalError("physical RAM retirement requires a live owner");
  if (IgnoredPin) {
    const auto I = Pins.find(IgnoredPin);
    if (I == Pins.end() || I->second.Owner != Owner)
      return physicalError(
          "ignored physical RAM pin must belong to this owner");
  }
  for (const auto &[ID, Pin] : Pins)
    if (Pin.Owner == Owner && ID != IgnoredPin)
      return physicalError("physical RAM owner still has a pinned view");
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::retire(uint64_t Owner) {
  if (auto E = canRetire(Owner))
    return E;
  const auto Region = Regions.at(Owner);
  if (Region.OwnsPages)
    for (uint64_t Offset = 0; Offset < Region.Size;
         Offset += physical::PageSize)
      Pages.erase(Region.Backing + Offset);
  OwnersByAddress.erase(Region.Backing);
  Regions.erase(Owner);
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::canReleaseRange(uint64_t Backing,
                                                  uint64_t Size,
                                                  uint64_t IgnoredPin) const {
  if (!Size || Size > UINT64_MAX - Backing)
    return physicalError("physical RAM release requires a nonempty, "
                         "nonoverflowing range");
  auto Intersects = [&](const Region &Region) {
    return Backing <= Region.Backing ? Region.Backing - Backing < Size
                                     : Backing - Region.Backing < Region.Size;
  };
  if (IgnoredPin) {
    const auto I = Pins.find(IgnoredPin);
    if (I == Pins.end() || !Intersects(Regions.at(I->second.Owner)))
      return physicalError("ignored physical RAM pin must belong to the "
                           "released range");
  }
  return canReleaseRanges({{Backing, Size}},
                          IgnoredPin ? llvm::ArrayRef<uint64_t>(IgnoredPin)
                                     : llvm::ArrayRef<uint64_t>());
}

llvm::Error KernelPhysicalMemory::canReleaseRanges(
    llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Ranges,
    llvm::ArrayRef<uint64_t> RetiringPins) const {
  std::set<uint64_t> Retiring;
  for (uint64_t Pin : RetiringPins) {
    if (auto E = canUnpin(Pin))
      return E;
    if (!Retiring.insert(Pin).second)
      return physicalError("physical RAM release repeats a retiring pin");
  }
  for (const auto &[Backing, Size] : Ranges) {
    if (!Size || Size > UINT64_MAX - Backing)
      return physicalError("physical RAM release requires a nonempty, "
                           "nonoverflowing range");
    for (const auto &[ID, Pin] : Pins) {
      if (Retiring.count(ID))
        continue;
      const auto &Region = Regions.at(Pin.Owner);
      const bool Intersects = Backing <= Region.Backing
                                  ? Region.Backing - Backing < Size
                                  : Backing - Region.Backing < Region.Size;
      if (Intersects)
        return physicalError("physical RAM release intersects a pinned owner");
    }
  }
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelPhysicalMemory::viewBacking(uint64_t Owner, uint64_t Offset,
                                  uint64_t Length) const {
  const auto *Region = find(Owner);
  if (!Region)
    return physicalError("physical RAM view requires a live owner");
  if (!Length || Offset >= Region->Size || Length > Region->Size - Offset)
    return physicalError("physical RAM view exceeds its exact owner range");
  const uint64_t Address = Region->Backing + Offset;
  if (auto E = Memory.validateBacking(Address, Length))
    return std::move(E);
  return Address;
}

llvm::Expected<std::vector<KernelPhysicalMemory::Segment>>
KernelPhysicalMemory::describe(uint64_t Owner, uint64_t Offset,
                               uint64_t Length) const {
  auto Backing = viewBacking(Owner, Offset, Length);
  if (!Backing)
    return Backing.takeError();
  std::vector<Segment> Result;
  uint64_t Address = *Backing;
  while (Length) {
    const uint64_t Page = pageBase(Address);
    const uint64_t PageOffset = Address - Page;
    const uint64_t Count = std::min(Length, physical::PageSize - PageOffset);
    Result.push_back({Pages.at(Page).Physical + PageOffset, Address, Count});
    Address += Count;
    Length -= Count;
  }
  return Result;
}

llvm::Error KernelPhysicalMemory::canPin(uint64_t Owner, uint64_t Offset,
                                         uint64_t Length) const {
  auto Backing = viewBacking(Owner, Offset, Length);
  if (!Backing)
    return Backing.takeError();
  if (Pins.size() >= physical::PinLimit || NextPin == UINT64_MAX)
    return llvm::make_error<PhysicalMemoryLimitError>(
        "physical RAM pin identity limit exceeded");
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelPhysicalMemory::pin(uint64_t Owner, uint64_t Offset, uint64_t Length) {
  if (auto E = canPin(Owner, Offset, Length))
    return std::move(E);
  const uint64_t ID = NextPin++;
  Pins.emplace(ID, PinRecord{Owner, Offset, Length});
  return ID;
}

bool KernelPhysicalMemory::hasPinnedPages(uint64_t Backing,
                                          uint64_t Size) const {
  if (!Size || Size > UINT64_MAX - Backing)
    return false;
  const uint64_t Last = pageBase(Backing + Size - 1);
  for (uint64_t Page = pageBase(Backing);; Page += physical::PageSize) {
    const bool Pinned =
        std::any_of(Pins.begin(), Pins.end(), [&](const auto &Entry) {
          const auto &Pin = Entry.second;
          const uint64_t First = Regions.at(Pin.Owner).Backing + Pin.Offset;
          return pageBase(First) <= Page &&
                 Page <= pageBase(First + Pin.Length - 1);
        });
    if (!Pinned)
      return false;
    if (Page == Last)
      return true;
  }
}

llvm::Error KernelPhysicalMemory::canUnpin(uint64_t Pin) const {
  if (!Pins.contains(Pin))
    return physicalError("physical RAM unpin requires a live pin");
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::unpin(uint64_t Pin) {
  if (auto E = canUnpin(Pin))
    return E;
  Pins.erase(Pin);
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::canExtendPin(uint64_t Pin,
                                               uint64_t NewLength) const {
  const auto I = Pins.find(Pin);
  if (I == Pins.end())
    return physicalError("physical RAM extension requires a live pin");
  if (NewLength < I->second.Length)
    return physicalError("physical RAM pin extension cannot shrink its view");
  auto Backing = viewBacking(I->second.Owner, I->second.Offset, NewLength);
  if (!Backing)
    return Backing.takeError();
  return llvm::Error::success();
}

llvm::Error KernelPhysicalMemory::extendPin(uint64_t Pin, uint64_t NewLength) {
  if (auto E = canExtendPin(Pin, NewLength))
    return E;
  Pins.at(Pin).Length = NewLength;
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelPhysicalMemory::pinBacking(uint64_t Pin, uint64_t Offset,
                                 uint64_t Length) const {
  const auto I = Pins.find(Pin);
  if (I == Pins.end())
    return physicalError("physical RAM access requires a live pin");
  const auto &View = I->second;
  if (Offset > View.Length || Length > View.Length - Offset)
    return physicalError("physical RAM access exceeds its pinned view");
  const auto *Region = find(View.Owner);
  if (!Region)
    return physicalError("physical RAM pin lost its allocation owner");
  const uint64_t Address = Region->Backing + View.Offset + Offset;
  if (auto E = Memory.validateBacking(Address, Length))
    return std::move(E);
  return Address;
}

llvm::Error KernelPhysicalMemory::read(uint64_t Pin, uint64_t Offset,
                                       llvm::MutableArrayRef<uint8_t> Bytes) {
  auto Backing = pinBacking(Pin, Offset, Bytes.size());
  if (!Backing)
    return Backing.takeError();
  return Memory.readBacking(*Backing, Bytes);
}

llvm::Error KernelPhysicalMemory::write(uint64_t Pin, uint64_t Offset,
                                        llvm::ArrayRef<uint8_t> Bytes) {
  auto Backing = pinBacking(Pin, Offset, Bytes.size());
  if (!Backing)
    return Backing.takeError();
  return Memory.writeBacking(*Backing, Bytes);
}

llvm::Expected<uint64_t>
KernelPhysicalMemory::ownerForRange(uint64_t Backing, uint64_t Size) const {
  if (!Size || Size > UINT64_MAX - Backing)
    return physicalError("physical RAM lookup requires a nonempty, "
                         "nonoverflowing range");
  auto I = OwnersByAddress.upper_bound(Backing);
  if (I == OwnersByAddress.begin())
    return physicalError("physical RAM range has no live allocation owner");
  --I;
  const auto &Region = Regions.at(I->second);
  const uint64_t Offset = Backing - Region.Backing;
  if (Offset >= Region.Size || Size > Region.Size - Offset)
    return physicalError(
        "physical RAM range has no single live allocation owner");
  return I->second;
}

llvm::Expected<uint64_t>
KernelPhysicalMemory::physicalAddress(uint64_t Backing) const {
  auto Owner = ownerForRange(Backing, 1);
  if (!Owner)
    return Owner.takeError();
  const uint64_t Page = pageBase(Backing);
  return Pages.at(Page).Physical + Backing - Page;
}
} // namespace neverd::emulation
