//===- PhysicalMemory.cpp - Shared RAM and virtual mappings--------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "PhysicalMemory.h"

#include "ExecutionDiagnostics.h"
#include "MemoryLayout.h"

#include <algorithm>
#include <cstring>

namespace neverd::emulation {
namespace {
bool valid(uint64_t Address, uint64_t Size, unsigned Permissions) {
  return Size && !(Address % memory::PageSize) && !(Size % memory::PageSize) &&
         Size - 1 <= UINT64_MAX - Address &&
         !(Permissions & ~(Read | Write | Execute));
}
} // namespace
PhysicalMemory::PhysicalMemory(llvm::sys::MemoryBlock Backing, uint64_t Limit)
    : Backing(Backing), Limit(Limit), NextPhysical(memory::ProjectionReserve) {}
PhysicalMemory::~PhysicalMemory() {
  (void)llvm::sys::Memory::releaseMappedMemory(Backing);
}

llvm::Expected<std::unique_ptr<PhysicalMemory>>
PhysicalMemory::create(uint64_t Limit) {
  if (!Limit || Limit > memory::MaxRAM)
    return diagnostic::error(diagnostic::MemoryLimit);
  std::error_code EC;
  auto Block = llvm::sys::Memory::allocateMappedMemory(
      Limit + memory::ProjectionReserve, nullptr,
      llvm::sys::Memory::MF_READ | llvm::sys::Memory::MF_WRITE, EC);
  if (EC)
    return llvm::errorCodeToError(EC);
  return std::unique_ptr<PhysicalMemory>(new PhysicalMemory(Block, Limit));
}

llvm::Error PhysicalMemory::map(uint64_t Address, uint64_t Size,
                                unsigned Permissions) {
  if (!valid(Address, Size, Permissions))
    return diagnostic::error(diagnostic::InvalidMapping);
  if (Size > Limit - Used || Size > size() - NextPhysical)
    return llvm::make_error<GuestMemoryLimitError>();
  for (uint64_t Offset = 0; Offset < Size; Offset += memory::PageSize)
    if (Pages.count(Address + Offset))
      return diagnostic::error(diagnostic::InvalidMapping);
  std::memset(data() + NextPhysical, 0, Size);
  for (uint64_t Offset = 0; Offset < Size; Offset += memory::PageSize)
    Pages.emplace(Address + Offset, Page{NextPhysical + Offset, Permissions});
  NextPhysical += Size;
  Used += Size;
  Dirty = true;
  return llvm::Error::success();
}

llvm::Error PhysicalMemory::aliases(llvm::ArrayRef<GuestAliasRange> Remove,
                                    llvm::ArrayRef<GuestAliasMapping> Add) {
  auto Next = Pages;
  auto NextAliases = Aliases;
  uint64_t NextUsed = Used;
  for (const auto &R : Remove) {
    auto I = NextAliases.find(R.Address);
    if (I == NextAliases.end() || I->second != R.Size)
      return diagnostic::error(diagnostic::InvalidMapping);
    for (uint64_t Offset = 0; Offset < R.Size; Offset += memory::PageSize)
      Next.erase(R.Address + Offset);
    NextAliases.erase(I);
    NextUsed -= R.Size;
  }
  // Validate against the surviving original mappings, never newly added
  // aliases.
  const auto Sources = Next;
  for (const auto &R : Add) {
    if (!valid(R.Address, R.Size, R.Permissions) ||
        !valid(R.Source, R.Size, R.Permissions))
      return diagnostic::error(diagnostic::InvalidMapping);
    if (R.Size > Limit - NextUsed)
      return llvm::make_error<GuestMemoryLimitError>();
    for (uint64_t Offset = 0; Offset < R.Size; Offset += memory::PageSize) {
      auto I = Sources.find(R.Source + Offset);
      if (I == Sources.end() || Next.count(R.Address + Offset))
        return diagnostic::error(diagnostic::InvalidMapping);
      Next.emplace(R.Address + Offset, Page{I->second.Physical, R.Permissions});
    }
    NextAliases.emplace(R.Address, R.Size);
    NextUsed += R.Size;
  }
  Pages.swap(Next);
  Aliases.swap(NextAliases);
  Used = NextUsed;
  Dirty = true;
  return llvm::Error::success();
}

llvm::Error PhysicalMemory::protect(uint64_t Address, uint64_t Size,
                                    unsigned Permissions) {
  if (!valid(Address, Size, Permissions) || check(Address, Size, 0))
    return diagnostic::error(diagnostic::InvalidMapping);
  for (uint64_t Offset = 0; Offset < Size; Offset += memory::PageSize)
    Pages.at(Address + Offset).Permissions = Permissions;
  Dirty = true;
  return llvm::Error::success();
}

std::optional<BackendFaultKind>
PhysicalMemory::check(uint64_t Address, uint64_t Size,
                      unsigned Permissions) const {
  if (!Size)
    return std::nullopt;
  if (Size - 1 > UINT64_MAX - Address)
    return BackendFaultKind::InvalidMemoryRange;
  uint64_t Last = (Address + Size - 1) & ~(memory::PageSize - 1);
  for (uint64_t VA = Address & ~(memory::PageSize - 1);;
       VA += memory::PageSize) {
    auto I = Pages.find(VA);
    if (I == Pages.end())
      return BackendFaultKind::UnmappedMemory;
    if ((I->second.Permissions & Permissions) != Permissions)
      return BackendFaultKind::Protection;
    if (VA == Last)
      return std::nullopt;
  }
}

llvm::Error PhysicalMemory::read(uint64_t Address,
                                 llvm::MutableArrayRef<uint8_t> Bytes,
                                 unsigned Permissions) const {
  if (check(Address, Bytes.size(), Permissions))
    return diagnostic::error(diagnostic::MemoryAccess);
  while (!Bytes.empty()) {
    uint64_t Offset = Address % memory::PageSize;
    size_t Count = std::min<uint64_t>(Bytes.size(), memory::PageSize - Offset);
    std::copy_n(data() + Pages.at(Address - Offset).Physical + Offset, Count,
                Bytes.data());
    Bytes = Bytes.drop_front(Count);
    Address += Count;
  }
  return llvm::Error::success();
}

llvm::Error PhysicalMemory::write(uint64_t Address,
                                  llvm::ArrayRef<uint8_t> Bytes,
                                  unsigned Permissions) {
  if (check(Address, Bytes.size(), Permissions))
    return diagnostic::error(diagnostic::MemoryAccess);
  while (!Bytes.empty()) {
    uint64_t Offset = Address % memory::PageSize;
    size_t Count = std::min<uint64_t>(Bytes.size(), memory::PageSize - Offset);
    std::copy_n(Bytes.data(), Count,
                data() + Pages.at(Address - Offset).Physical + Offset);
    Bytes = Bytes.drop_front(Count);
    Address += Count;
  }
  return llvm::Error::success();
}

} // namespace neverd::emulation
