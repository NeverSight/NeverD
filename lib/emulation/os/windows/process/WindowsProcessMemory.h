//===- WindowsProcessMemory.h - Windows reservation ownership ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_PROCESS_MEMORY_H
#define NEVERD_EMULATION_WINDOWS_PROCESS_MEMORY_H

#include "neverd/emulation/AddressSpace.h"

#include <map>

namespace neverd::emulation {
struct ProcessOptions;
namespace windows_process {
struct Image;
struct MemoryResult {
  uint64_t Value = 0;
  uint32_t Error = 0;
  bool Unsupported = false;
};
struct MemoryInformation {
  uint64_t Base, AllocationBase, Size;
  uint32_t AllocationProtection, State, Protection, Type;
};

/// Own reservation identity and placement, never a second page table.
/// Commitment and current permissions come from AddressSpace. All operations
/// require exclusive OS ownership at a stopped execution boundary.
class VirtualMemory final {
public:
  VirtualMemory(AddressSpace &Space, const ProcessOptions &Options);
  VirtualMemory(AddressSpace &Space, const Image &Image,
                const ProcessOptions &Options);
  /// Reserve image identity before any image mappings or CPU are exposed.
  llvm::Expected<uint64_t> reserveImage(uint64_t Preferred, uint64_t Size,
                                        bool Relocatable);
  llvm::Expected<MemoryResult> allocate(uint64_t Address, uint64_t Size,
                                        uint32_t Type, uint32_t Protection);
  llvm::Expected<MemoryResult> free(uint64_t Address, uint64_t Size,
                                    uint32_t Type);
  /// Value is the old-protection output, including PAGE_NOACCESS for an
  /// uncommitted-range failure. Zero means the output must stay unchanged.
  llvm::Expected<MemoryResult> protect(uint64_t Address, uint64_t Size,
                                       uint32_t Protection);
  llvm::Expected<std::optional<MemoryInformation>> query(uint64_t Address);

private:
  enum class Owner { Virtual, Image, Runtime, Heap, Stack };
  struct Reservation {
    uint64_t Size;
    uint32_t Protection, Type;
    Owner Kind;
  };
  using Reservations = std::map<uint64_t, Reservation>;
  Reservations::iterator containing(uint64_t Address, uint64_t Size);
  std::optional<uint64_t> findGap(uint64_t Size, bool TopDown) const;
  llvm::Expected<bool> commit(uint64_t Address, uint64_t Size,
                              unsigned Permissions);
  llvm::Error decommit(uint64_t Address, uint64_t Size);

  AddressSpace &Space;
  Reservations Ranges;
};
} // namespace windows_process
} // namespace neverd::emulation
#endif
