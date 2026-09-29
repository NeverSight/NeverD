//===- PhysicalMemory.h - Shared hardware RAM and virtual mappings -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_CORE_PHYSICALMEMORY_H
#define NEVERD_EMULATION_CORE_PHYSICALMEMORY_H
#include "ExecutionBackend.h"

#include "llvm/Support/Memory.h"

#include <map>

namespace neverd::emulation {
/// One backing authority for all hardware adapters. Virtual aliases share GPA
/// pages. Page tables are a stopped-CPU projection, never a second authority.
class PhysicalMemory {
public:
  struct Page {
    uint64_t Physical;
    unsigned Permissions;
  };
  static llvm::Expected<std::unique_ptr<PhysicalMemory>> create(uint64_t Limit);
  ~PhysicalMemory();
  llvm::Error map(uint64_t Address, uint64_t Size, unsigned Permissions);
  llvm::Error aliases(llvm::ArrayRef<GuestAliasRange> Remove,
                      llvm::ArrayRef<GuestAliasMapping> Add);
  llvm::Error protect(uint64_t Address, uint64_t Size, unsigned Permissions);
  std::optional<BackendFaultKind> check(uint64_t Address, uint64_t Size,
                                        unsigned Permissions) const;
  llvm::Error read(uint64_t Address, llvm::MutableArrayRef<uint8_t> Bytes,
                   unsigned Permissions = Read) const;
  llvm::Error write(uint64_t Address, llvm::ArrayRef<uint8_t> Bytes,
                    unsigned Permissions = Write);
  uint8_t *data() const { return static_cast<uint8_t *>(Backing.base()); }
  uint64_t size() const { return Backing.allocatedSize(); }

private:
  friend llvm::Expected<uint64_t> buildX64PageTables(PhysicalMemory &Memory,
                                                     uint64_t PreviousRoot);
  PhysicalMemory(llvm::sys::MemoryBlock Backing, uint64_t Limit);
  llvm::sys::MemoryBlock Backing;
  uint64_t Limit, Used = 0, NextPhysical;
  bool Dirty = true;
  std::map<uint64_t, Page> Pages;
  std::map<uint64_t, uint64_t> Aliases;
};
} // namespace neverd::emulation
#endif
