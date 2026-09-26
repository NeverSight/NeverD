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
  if (auto E = Physical.canRegisterRegion(
          Start, Start, AllocationSize,
          Cache.value_or(KernelPhysicalMemory::CacheType::Cached))) {
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
                                    uint64_t DataOffset, uint64_t DataSize) {
  if (!AllocationSize || AllocationSize > profile::KernelArenaSize ||
      !Alignment || (Alignment & (Alignment - 1)) || !DataSize ||
      DataOffset >= AllocationSize || DataSize > AllocationSize - DataOffset ||
      NextAllocation > UINT64_MAX - (Alignment - 1))
    return physicalModelError("invalid physical data allocation range");
  const uint64_t Start = (NextAllocation + Alignment - 1) & ~(Alignment - 1);
  if (Start > AllocationEnd || AllocationSize > AllocationEnd - Start)
    return physicalModelError("Windows initialization model arena exhausted");
  // Validate every new page and the exact exposed data range before allocating
  // or zeroing anything. Page padding remains outside this owner's authority.
  if (auto E = Physical.canRegisterRegion(Start, Start + DataOffset, DataSize))
    return std::move(E);
  auto Address = allocate(AllocationSize, Alignment);
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
} // namespace neverd::emulation
