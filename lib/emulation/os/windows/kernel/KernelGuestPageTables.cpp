//===- KernelGuestPageTables.cpp - Guest paging over shared frame identity ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest paging over shared frame identity.
///

#include "KernelGuestPageTables.h"

#include "../../../arch/x86_64/X64Machine.h"
#include "KernelPhysicalMemory.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/Support/Endian.h"

#include <array>

namespace neverd::emulation {
namespace {
llvm::Error pagingError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Text);
}
constexpr uint64_t Accessed = 1ULL << 5;
constexpr uint64_t Dirty = 1ULL << 6;
uint64_t recursiveAddress(llvm::ArrayRef<uint16_t> Path) {
  uint64_t Address = 0xffff000000000000ULL;
  for (unsigned I = 0; I != 4; ++I) {
    const uint64_t Index = I < 4 - Path.size()
                               ? KernelGuestPageTables::SelfIndex
                               : Path[I - (4 - Path.size())];
    Address |= Index << (39 - 9 * I);
  }
  return Address;
}
} // namespace

llvm::Error KernelGuestPageTables::initialize(uint64_t Capacity) {
  if (Initialized || !Memory.addressSpace())
    return pagingError(
        "guest page tables require one initialized address space");
  Capacity &= ~(x64::PageSize - 1);
  if (auto E = Memory.map(TableBase, TableBytes, Read))
    return E;
  if (auto E = Physical.enableGuestFrames(Capacity))
    return E;
  Initialized = true;
  SpaceIdentity = Memory.addressSpace()->identity();
  return llvm::Error::success();
}

llvm::Error KernelGuestPageTables::synchronize() {
  if (!Initialized)
    return pagingError("guest page tables are not enabled");
  auto Space = Memory.addressSpace();
  if (!Space || Space->identity() != SpaceIdentity)
    return pagingError(
        "guest page tables cannot change address-space ownership");
  if (Generation == Space->mappingGeneration() &&
      FrameGeneration == Physical.frameGeneration())
    return llvm::Error::success();
  auto Layout = Space->mappings();
  if (!Layout)
    return Layout.takeError();
  std::vector<uint8_t> Bytes(TableBytes, 0);
  std::map<uint64_t, uint64_t> Tables;
  std::map<uint64_t, uint64_t> NextAliases;
  std::map<uint64_t, uint64_t> NextFrames;
  auto RootAddress = Physical.frameAddress(TableBase);
  if (!RootAddress)
    return RootAddress.takeError();
  Tables.emplace(*RootAddress, 0);
  NextAliases.emplace(recursiveAddress({}), TableBase);
  uint64_t Used = 1;
  auto MapPage = [&](uint64_t VA, uint64_t Entry) -> llvm::Error {
    uint64_t Table = 0;
    std::array<uint16_t, 3> Path{};
    for (unsigned Level = 4; Level > 1; --Level) {
      const uint16_t Index = (VA >> (12 + (Level - 1) * 9)) & 511;
      Path[4 - Level] = Index;
      auto *Slot = Bytes.data() + Table * x64::PageSize + Index * 8;
      uint64_t Child = llvm::support::endian::read64le(Slot);
      if (!Child) {
        if (Used == TableBytes / x64::PageSize)
          return pagingError("guest page table storage capacity exhausted");
        const uint64_t Source = TableBase + Used * x64::PageSize;
        auto Frame = Physical.frameAddress(Source);
        if (!Frame)
          return Frame.takeError();
        Tables.emplace(*Frame, Used++);
        NextAliases.emplace(
            recursiveAddress(llvm::ArrayRef(Path.data(), 5 - Level)), Source);
        Child =
            *Frame | x64::Present | x64::Writable | x64::UserPage | Accessed;
        llvm::support::endian::write64le(Slot, Child);
      }
      Table = Tables.at(Child & x64::AddressMask);
    }
    auto *Slot = Bytes.data() + Table * x64::PageSize + ((VA >> 12) & 511) * 8;
    llvm::support::endian::write64le(Slot, Entry);
    return llvm::Error::success();
  };
  for (const auto &Region : *Layout) {
    if (!x64::canonicalRange(Region.Address, Region.Size))
      return pagingError("guest paging requires canonical x64 mappings");
    if (Region.Device)
      return pagingError(
          "guest page tables cannot encode unmodeled device frames");
    for (uint64_t Offset = 0; Offset < Region.Size; Offset += x64::PageSize) {
      const uint64_t VA = Region.Address + Offset;
      if (((VA >> 39) & 511) == SelfIndex) {
        if (!Aliases.contains(VA))
          return pagingError(
              "guest mapping conflicts with the recursive page-table window");
        continue;
      }
      if (!(Region.Permissions & GuestAccessPermissions))
        continue;
      auto Frame = Physical.frameAddress(VA);
      if (!Frame)
        return Frame.takeError();
      NextFrames.emplace(VA, *Frame);
      auto Cache = Physical.frameCacheType(VA);
      if (!Cache)
        return Cache.takeError();
      if (*Cache == KernelPhysicalMemory::CacheType::WriteCombined)
        return pagingError(
            "guest paging does not model write-combined PAT entries");
      uint64_t Entry = *Frame | x64::Present | Accessed;
      if (*Cache == KernelPhysicalMemory::CacheType::NonCached)
        Entry |= (1ULL << 3) | (1ULL << 4);
      if (Region.Permissions & Write)
        Entry |= x64::Writable | Dirty;
      if (Region.Permissions & UserAccessible)
        Entry |= x64::UserPage;
      if (!(Region.Permissions & Execute))
        Entry |= x64::NoExecute;
      if (auto E = MapPage(VA, Entry))
        return E;
    }
  }
  // A read-only supervisor recursive window. All table contents are model
  // state; guest PTE writes, CR3 switching and TLB invalidation are
  // unsupported.
  llvm::support::endian::write64le(Bytes.data() + SelfIndex * 8,
                                   *RootAddress | x64::Present | Accessed |
                                       x64::NoExecute);
  std::vector<GuestAliasRange> Removing;
  std::vector<GuestAliasMapping> Adding;
  for (const auto &[VA, Source] : Aliases)
    Removing.push_back({VA, x64::PageSize});
  for (const auto &[VA, Source] : NextAliases) {
    Adding.push_back({VA, Source, x64::PageSize, Read});
    auto Frame = Physical.frameAddress(Source);
    if (!Frame)
      return Frame.takeError();
    NextFrames.emplace(VA, *Frame);
  }
  // Publish through the stopped address-space authority. A CPU that retains
  // an acknowledged resource continuation deliberately rejects its ordinary
  // memory-mutation APIs; refreshing this projection must not consume or
  // weaken that continuation guard.
  if (auto E = Space->replaceAliases(Removing, Adding))
    return E;
  if (auto E = Space->writeBacking(TableBase, Bytes))
    return E;
  Root = *RootAddress;
  Aliases = std::move(NextAliases);
  VirtualFrames = std::move(NextFrames);
  Generation = Space->mappingGeneration();
  FrameGeneration = Physical.frameGeneration();
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelGuestPageTables::root() {
  if (auto E = synchronize())
    return E;
  return Root;
}
llvm::Expected<uint64_t>
KernelGuestPageTables::physicalAddress(uint64_t Address) {
  if (auto E = synchronize())
    return E;
  const auto Found = VirtualFrames.find(Address & ~(x64::PageSize - 1));
  if (Found == VirtualFrames.end())
    return pagingError("physical address requires resident ordinary guest RAM");
  return Found->second + Address % x64::PageSize;
}
llvm::Expected<uint64_t>
KernelGuestPageTables::virtualAddress(uint64_t Address) {
  if (auto E = synchronize())
    return E;
  const uint64_t Frame = Address & ~(x64::PageSize - 1);
  // Prefer the canonical table storage over its recursive alias.
  for (const auto &[VA, Physical] : VirtualFrames)
    if (Physical == Frame)
      return VA + Address % x64::PageSize;
  return 0;
}
} // namespace neverd::emulation
