//===- KernelGuestPageTables.h - Guest-owned Windows x64 page tables -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest-owned Windows x64 page tables.
///

#ifndef NEVERD_EMULATION_KERNELGUESTPAGETABLES_H
#define NEVERD_EMULATION_KERNELGUESTPAGETABLES_H

#include "neverd/emulation/GuestMemory.h"

#include <map>
#include <optional>

namespace neverd::emulation {
class KernelPhysicalMemory;
/// Read-only four-level guest tables over the current address-space authority.
/// These are Windows-visible data, never a backend's private transport tables.
class KernelGuestPageTables {
public:
  static constexpr uint64_t TableBase = 0xfffff90000000000ULL;
  static constexpr uint64_t TableBytes = 0x100000;
  static constexpr uint64_t SelfIndex = 510;
  KernelGuestPageTables(GuestMemory &Memory, KernelPhysicalMemory &Physical)
      : Memory(Memory), Physical(Physical) {}
  llvm::Error initialize(uint64_t Capacity);
  llvm::Error synchronize();
  llvm::Expected<uint64_t> root();
  llvm::Expected<uint64_t> virtualAddress(uint64_t PhysicalAddress);
  llvm::Expected<uint64_t> physicalAddress(uint64_t VirtualAddress);
  static constexpr uint64_t pteBase() {
    return 0xffff000000000000ULL | (SelfIndex << 39);
  }
  /// Storage and the complete recursive PML4 slot belong to this model even
  /// when no alias for a particular table page has been published yet.
  static constexpr bool overlapsReservedMemory(uint64_t Address,
                                               uint64_t Size) {
    if (!Size || Size - 1 > UINT64_MAX - Address)
      return true;
    const auto Overlaps = [=](uint64_t Base, uint64_t Length) {
      return Address < Base ? Base - Address < Size : Address - Base < Length;
    };
    return Overlaps(TableBase, TableBytes) || Overlaps(pteBase(), 1ULL << 39);
  }

private:
  GuestMemory &Memory;
  KernelPhysicalMemory &Physical;
  bool Initialized = false;
  uint64_t Root = 0;
  std::optional<uint64_t> Generation;
  uint64_t FrameGeneration = 0;
  std::shared_ptr<const void> SpaceIdentity;
  std::map<uint64_t, uint64_t> Aliases;
  std::map<uint64_t, uint64_t> VirtualFrames;
};
} // namespace neverd::emulation
#endif
