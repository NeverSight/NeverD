//===- KernelModelPhysicalMemory.cpp - Existing RAM physical ownership ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Register exact data ranges when existing model allocations are created.
/// MDLs describe those allocations and publish the same physical page IDs.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error physicalModelError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}
} // namespace

llvm::Error KernelModel::preflightPhysicalAllocation(uint64_t Address,
                                                     uint64_t Size) const {
  for (const auto &[Key, View] : SectionViews)
    for (const auto &Page : View.Pages)
      if (Address < Page.Backing + profile::PageSize &&
          Page.Backing < Address + Size)
        return physicalModelError(
            "allocation backing still has a live physical section view");
  return canReleaseRange(Address, Size);
}

llvm::Expected<KernelPhysicalMemory::CacheType>
KernelModel::memoryCacheType(uint32_t Value) {
  switch (Value) {
  case windows::MmCached:
    return KernelPhysicalMemory::CacheType::Cached;
  case windows::MmNonCached:
    return KernelPhysicalMemory::CacheType::NonCached;
  case windows::MmWriteCombined:
    return KernelPhysicalMemory::CacheType::WriteCombined;
  default:
    return physicalModelError("unsupported memory cache type");
  }
}

llvm::Expected<uint64_t> KernelModel::allocateContiguousMemory(uint64_t Size,
                                                               uint64_t High) {
  if (!Size)
    return physicalModelError(
        "contiguous allocation requires a nonzero byte count");
  if (Size > KernelMemoryBytes)
    return uint64_t(0);
  const uint64_t Bytes =
      (Size + profile::PageSize - 1) & ~(profile::PageSize - 1);
  const uint64_t Pages = Bytes / profile::PageSize;
  // The matched native entry converts its inclusive byte limit to a PFN and
  // clamps it to the supported physical width before entering the allocator.
  const uint64_t HighPFN =
      std::min<uint64_t>(High / profile::PageSize, 0xfffffffff);
  const uint64_t Limit = HighPFN * profile::PageSize + profile::PageSize - 1;
  auto Plan = Physical.planAllocatedPages(0, Limit, 0, Pages, Pages);
  if (!Plan)
    return Plan.takeError();
  if (Plan->size() != Pages)
    return uint64_t(0);
  const uint64_t Start =
      (NextAllocation + profile::PageSize - 1) & ~(profile::PageSize - 1);
  if (Start > AllocationEnd || Bytes > AllocationEnd - Start)
    return uint64_t(0);
  if (auto E = preflightPhysicalAllocation(Start, Bytes))
    return E;
  constexpr auto Cache = KernelPhysicalMemory::CacheType::Cached;
  if (auto E = Physical.canRegisterAllocatedPages(Start, Start, *Plan, Cache)) {
    if (E.isA<PhysicalMemoryLimitError>()) {
      llvm::consumeError(std::move(E));
      return uint64_t(0);
    }
    return E;
  }
  auto Address = allocate(Bytes, profile::PageSize);
  if (!Address)
    return Address.takeError();
  if (auto E =
          Physical.registerAllocatedPages(*Address, *Address, *Plan, Cache))
    return E;
  // Native MmAllocateContiguousMemory selects cached executable read/write
  // protection; SpecifyCache(MmCached) converges on the same internal value.
  if (auto E = Memory.protect(*Address, Bytes, Read | Write | Execute))
    return E;
  Allocations.emplace(*Address,
                      PoolAllocation{Bytes, 0, true, std::nullopt, 0, true});
  return *Address;
}

llvm::Expected<uint64_t> KernelModel::freeContiguousMemory(uint64_t Address) {
  auto It = Allocations.find(Address);
  if (It == Allocations.end() || !It->second.Contiguous)
    return physicalModelError(
        "MmFreeContiguousMemory requires its exact live allocation base");
  if (auto E = Physical.canRetire(Address))
    return E;
  if (auto E = prepareReleaseRange(Address, It->second.Size))
    return E;
  if (auto E = Memory.protect(Address, It->second.Size, 0))
    return E;
  if (auto E = Physical.retire(Address))
    return E;
  FreedRanges.emplace(Address, It->second.Size);
  Allocations.erase(It);
  return uint64_t(0);
}

llvm::Expected<uint64_t>
KernelModel::allocatePagesForMDL(llvm::ArrayRef<uint64_t> A, bool Extended) {
  using namespace windows;
  const uint64_t Low = A[0], High = A[1], Skip = A[2], Requested = A[3];
  const uint32_t Flags = Extended ? static_cast<uint32_t>(A[5]) : 0;
  constexpr uint32_t SupportedFlags =
      MmDontZeroAllocation | MmAllocateFullyRequired | MmAllocateNoWait |
      MmAllocatePreferContiguous | MmAllocateRequireContiguousChunks;
  if (Flags & ~SupportedFlags)
    return physicalModelError(
        "physical MDL allocation flags require an "
        "unmodeled NUMA, large-page or hot-remove policy");
  if (Low > High || Skip % profile::PageSize || !Requested ||
      Requested > MmMaximumMdlAllocation)
    return physicalModelError("invalid physical MDL allocation range or size");
  const bool Contiguous = Flags & MmAllocateRequireContiguousChunks;
  if (Contiguous && Skip && ((Skip & (Skip - 1)) || Requested % Skip))
    return physicalModelError("contiguous physical MDL chunks require a "
                              "power-of-two SkipBytes and a whole chunk count");
  std::optional<KernelPhysicalMemory::CacheType> Cache;
  if (Extended) {
    auto Parsed = memoryCacheType(static_cast<uint32_t>(A[4]));
    if (!Parsed)
      return Parsed.takeError();
    Cache = *Parsed;
  }
  const uint64_t RequestedPages =
      (Requested + profile::PageSize - 1) / profile::PageSize;
  const uint64_t ChunkPages =
      Contiguous ? (Skip ? Skip / profile::PageSize : RequestedPages) : 0;
  auto Pages =
      Physical.planAllocatedPages(Low, High, Skip, RequestedPages, ChunkPages);
  if (!Pages)
    return Pages.takeError();
  const uint64_t Start =
      (NextAllocation + profile::PageSize - 1) & ~(profile::PageSize - 1);
  const uint64_t Available = Start <= AllocationEnd ? AllocationEnd - Start : 0;
  // Backing and descriptor are admitted together. A partial allocation reports
  // only pages actually owned; FULLY_REQUIRED never publishes an empty MDL.
  while (!Pages->empty() && Pages->size() * profile::PageSize + MDLSize +
                                    Pages->size() * profile::PointerSize >
                                Available)
    Pages->resize(Pages->size() - (ChunkPages ? ChunkPages : 1));
  if (Pages->empty() ||
      (((Contiguous && !Skip) || (Flags & MmAllocateFullyRequired)) &&
       Pages->size() != RequestedPages))
    return 0;
  const uint64_t AllocationSize = Pages->size() * profile::PageSize;
  if (auto E = preflightPhysicalAllocation(Start, AllocationSize))
    return E;
  if (auto E =
          Physical.canRegisterAllocatedPages(Start, Start, *Pages, Cache)) {
    if (E.isA<PhysicalMemoryLimitError>()) {
      llvm::consumeError(std::move(E));
      return 0;
    }
    return E;
  }
  auto Backing = allocate(AllocationSize, profile::PageSize);
  if (!Backing)
    return Backing.takeError();
  if (auto E =
          Physical.registerAllocatedPages(*Backing, *Backing, *Pages, Cache))
    return E;
  const uint32_t ByteCount =
      static_cast<uint32_t>(std::min<uint64_t>(Requested, AllocationSize));
  auto MDL = createMDLRecord(0, ByteCount, MDLPagesLocked);
  if (!MDL)
    return MDL.takeError();
  if (!*MDL)
    return physicalModelError("admitted physical MDL lost descriptor capacity");
  LockedMdl State;
  State.Owner = LockedMdl::Ownership::AllocatedPages;
  State.Address = *MDL;
  State.Size = MDLSize + Pages->size() * profile::PointerSize;
  State.BackingAddress = *Backing;
  State.ByteCount = ByteCount;
  State.Pool = *Backing;
  State.Writable = true;
  State.DmaWritable = true;
  if (auto E = initializeMDLPhysicalPages(State))
    return E;
  if (auto E = Memory.protect(*Backing, AllocationSize, 0))
    return E;
  // Fresh private pages are zeroed by allocate. DONT_ZERO permits unspecified
  // bytes and imposes no requirement to replace an already-zero fresh page.
  MDLs.emplace(*MDL, State);
  return *MDL;
}

llvm::Error KernelModel::freePagesFromMDL(uint64_t MDL) {
  using namespace windows;
  const auto It = MDLs.find(MDL);
  if (It == MDLs.end() ||
      It->second.Owner != LockedMdl::Ownership::AllocatedPages)
    return physicalModelError("MmFreePagesFromMdl requires a live independent "
                              "physical-page MDL");
  auto &State = It->second;
  if (State.MappingReservation)
    return physicalModelError(
        "physical page release requires MmUnmapReservedMapping first");
  if (auto E = canReleaseMDLDependencies(MDL))
    return E;
  if (auto E = canReleaseMdlUserViews(MDL))
    return E;
  if (auto E = canRevokeVirtualRange(MDL, State.Size))
    return E;
  auto Writable = Memory.canAccess(MDL, State.Size, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return physicalModelError(
        "physical MDL release requires writable metadata");
  const auto *Region = Physical.find(State.Pool);
  if (!Region || !Region->OwnsPages)
    return physicalModelError("independent physical MDL lost its page owner");
  std::vector<std::pair<uint64_t, uint64_t>> Ranges{
      {Region->Backing, Region->Size}};
  if (State.Mapped)
    Ranges.emplace_back(State.Buffer, State.AllocationSize);
  // No page, alias, dispatcher or descriptor changes until every dependent
  // user view, partial MDL and external physical/DMA pin has passed preflight.
  if (auto E = prepareReleaseRanges(Ranges))
    return E;
  if (State.Mapped)
    if (auto E = unmapLockedPages(State.Buffer, MDL))
      return E;
  FreedRanges.emplace(Region->Backing, Region->Size);
  if (auto E = Physical.retire(State.Pool))
    return E;
  if (auto E = Memory.writeInteger(MDL + MDLFlagsOffset, 0, 2))
    return E;
  if (auto E = Memory.writeInteger(MDL + MDLByteCountOffset, 0, 4))
    return E;
  State.Owner = LockedMdl::Ownership::ReleasedPages;
  State.ByteCount = 0;
  State.Buffer = 0;
  State.BackingAddress = 0;
  State.Pool = 0;
  State.AllocationSize = 0;
  State.Writable = false;
  State.DmaWritable = false;
  return llvm::Error::success();
}

llvm::Error KernelModel::freeAllocatedMDL(uint64_t MDL) {
  const auto It = MDLs.find(MDL);
  if (It == MDLs.end() ||
      It->second.Owner != LockedMdl::Ownership::ReleasedPages)
    return physicalModelError("physical MDL descriptor free requires "
                              "MmFreePagesFromMdl first");
  if (auto E = prepareReleaseRange(MDL, It->second.Size))
    return E;
  FreedRanges.emplace(MDL, It->second.Size);
  MDLs.erase(It);
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelModel::allocatePhysicalBuffer(uint64_t AllocationSize, uint64_t Alignment,
                                    uint64_t DataOffset, uint64_t DataSize,
                                    bool PagedArena) {
  uint64_t &Cursor = PagedArena ? NextPagedAllocation : NextAllocation;
  const uint64_t ArenaEnd = PagedArena ? PagedAllocationEnd : AllocationEnd;
  if (!AllocationSize || AllocationSize > KernelMemoryBytes || !Alignment ||
      (Alignment & (Alignment - 1)) || !DataSize ||
      DataOffset >= AllocationSize || DataSize > AllocationSize - DataOffset ||
      !ArenaEnd || Cursor > UINT64_MAX - (Alignment - 1))
    return physicalModelError("invalid physical data allocation range");
  const uint64_t Start = (Cursor + Alignment - 1) & ~(Alignment - 1);
  if (Start > ArenaEnd || AllocationSize > ArenaEnd - Start)
    return physicalModelError("Windows initialization model arena exhausted");
  // Validate every new page and the exact exposed data range before allocating
  // or zeroing anything. Page padding remains outside this owner's authority.
  if (auto E = Physical.canRegisterRegion(Start, Start + DataOffset, DataSize))
    return std::move(E);
  auto Address = allocateInArena(AllocationSize, Alignment, Cursor, ArenaEnd);
  if (!Address)
    return Address.takeError();
  if (auto E =
          Physical.registerRegion(*Address, *Address + DataOffset, DataSize))
    return std::move(E);
  return *Address;
}

llvm::Error KernelModel::initializeMDLPhysicalPages(const LockedMdl &State) {
  const uint64_t Backing = State.BackingAddress;
  auto Owner = Physical.ownerForRange(Backing, State.ByteCount);
  if (!Owner)
    return Owner.takeError();
  const auto *Region = Physical.find(*Owner);
  auto Segments =
      Physical.describe(*Owner, Backing - Region->Backing, State.ByteCount);
  if (!Segments)
    return Segments.takeError();
  const uint64_t Count = ((Backing & (profile::PageSize - 1)) +
                          State.ByteCount + profile::PageSize - 1) /
                         profile::PageSize;
  if (Segments->size() != Count ||
      State.Size < windows::MDLSize + Count * profile::PointerSize)
    return physicalModelError(
        "MDL physical pages do not match descriptor extent");
  auto Writable = Memory.canAccess(State.Address, State.Size, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return physicalModelError("MDL physical pages require writable descriptor "
                              "storage");
  for (size_t I = 0; I != Segments->size(); ++I)
    if (auto E = Memory.writeInteger(
            State.Address + windows::MDLSize + I * profile::PointerSize,
            (*Segments)[I].Physical / profile::PageSize, profile::PointerSize))
      return E;
  return llvm::Error::success();
}
llvm::Expected<uint64_t> KernelModel::queryPhysicalAddress(uint64_t Address) {
  if (Version) {
    auto Readable = Memory.canAccess(Address, 1, Read);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable)
      return physicalModelError(
          "physical address query requires resident readable RAM");
    for (const auto &[Base, Size] : FreedRanges)
      if (Address >= Base && Address - Base < Size)
        return physicalModelError("physical address query requires live RAM");
    return Physical.frameAddress(Address);
  }
  if (auto E = validateGuestAccess(Address, 1, false))
    return E;
  auto Readable = Memory.canAccess(Address, 1, Read);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return physicalModelError(
        "physical address query requires a live readable RAM mapping");
  auto Translation = Physical.resolveAddress(Address);
  if (!Translation)
    return Translation.takeError();
  // Residency belongs to allocation and pin owners, independently of the
  // selected alias's permissions. The query acquires no lock or new PFN.
  bool Resident = Physical.hasPinnedPages(Address, 1);
  for (const uint64_t Owner : Translation->Owners) {
    Resident |= Physical.find(Owner)->OwnsPages;
    if (auto Pool = Allocations.find(Owner); Pool != Allocations.end())
      Resident |= Pool->second.NonPaged;
  }
  if (!Resident)
    return physicalModelError("physical address query requires nonpaged pool, "
                              "independent physical pages or a live page lock");
  return Translation->Physical;
}

llvm::Expected<uint64_t>
KernelModel::queryPrimaryVirtualAddress(uint64_t PhysicalAddress) {
  // A live nonpaged PoolAllocation records its allocator-owned primary VA.
  // Generic physical Region::Backing also includes private/request storage,
  // so it is not sufficient evidence for the original PFN primary PTE.
  std::optional<uint64_t> Primary;
  for (const auto &[Base, Allocation] : Allocations) {
    if (!Allocation.NonPaged)
      continue;
    auto Segments = Physical.describe(Base, 0, Allocation.Size);
    if (!Segments)
      return Segments.takeError();
    for (const auto &Segment : *Segments) {
      if (PhysicalAddress < Segment.Physical ||
          PhysicalAddress - Segment.Physical >= Segment.Length)
        continue;
      const uint64_t Address =
          Segment.Backing + (PhysicalAddress - Segment.Physical);
      if (Primary && *Primary != Address)
        return physicalModelError("physical address has ambiguous primary "
                                  "nonpaged pool mappings");
      Primary = Address;
    }
  }
  if (!Primary && Version) {
    auto Pages = Physical.physicalViewPages(
        PhysicalAddress & ~(profile::PageSize - 1), profile::PageSize);
    if (!Pages)
      return Pages.takeError();
    Primary = Pages->front().Backing + PhysicalAddress % profile::PageSize;
  }
  if (!Primary)
    return physicalModelError("physical address has no live primary "
                              "nonpaged pool mapping");
  // Retained physical identity cannot substitute for an unmapped, protected or
  // rebound primary VA. Reuse the forward query's mapping/lifetime admission.
  auto Current = queryPhysicalAddress(*Primary);
  if (!Current)
    return Current.takeError();
  if (*Current != PhysicalAddress)
    return physicalModelError("primary nonpaged pool mapping was replaced");
  return *Primary;
}

} // namespace neverd::emulation
