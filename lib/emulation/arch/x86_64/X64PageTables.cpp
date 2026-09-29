//===- X64PageTables.cpp - x64 page table projection---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../core/ExecutionDiagnostics.h"
#include "../../core/PhysicalMemory.h"
#include "X64Machine.h"

#include "llvm/Support/Endian.h"

#include <cstring>

namespace neverd::emulation {
llvm::Expected<uint64_t> buildX64PageTables(PhysicalMemory &Memory,
                                            uint64_t PreviousRoot) {
  if (!Memory.Dirty)
    return PreviousRoot;
  // Updating backing bytes alone does not invalidate cached translations.
  // Alternate roots after each mapping transaction, forcing a CR3 transition
  // on the next entry (PCID and global pages are disabled in this profile).
  const uint64_t Root = PreviousRoot == x64::FirstTableRoot
                            ? x64::SecondTableRoot
                            : x64::FirstTableRoot;
  std::memset(Memory.data(), 0, x64::TableReserve);
  uint64_t Next = x64::FirstChildTable;
  for (const auto &[VA, P] : Memory.Pages) {
    uint64_t Table = Root;
    for (unsigned Level = x64::TableLevels; Level > 1; --Level) {
      auto Index = (VA >> (x64::PageBits + (Level - 1) * x64::TableBits)) &
                   (x64::TableEntries - 1);
      auto *Slot = Memory.data() + Table + Index * x64::WordBytes;
      uint64_t Entry = llvm::support::endian::read64le(Slot);
      if (!Entry) {
        if (Next == x64::TableReserve)
          return diagnostic::error(diagnostic::PageTables);
        Entry = Next | x64::Present | x64::Writable;
        Next += x64::PageSize;
        llvm::support::endian::write64le(Slot, Entry);
      }
      Table = Entry & x64::AddressMask;
    }
    auto Index = (VA >> x64::PageBits) & (x64::TableEntries - 1);
    uint64_t Entry = P.Physical;
    if (P.Permissions)
      Entry |= x64::Present;
    if (P.Permissions & Write)
      Entry |= x64::Writable;
    if (!(P.Permissions & Execute))
      Entry |= x64::NoExecute;
    llvm::support::endian::write64le(
        Memory.data() + Table + Index * x64::WordBytes, Entry);
  }
  Memory.Dirty = false;
  return Root;
}
} // namespace neverd::emulation
