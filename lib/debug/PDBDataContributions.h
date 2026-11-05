//===- PDBDataContributions.h - Linked read-only data regions -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Read-only storage intervals from PDB section contributions. These describe
/// linked storage, not individual C objects or buffer capacities.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_PDB_DATA_CONTRIBUTIONS_H
#define NEVERD_DEBUG_PDB_DATA_CONTRIBUTIONS_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace neverd::pdb_loader_detail {

struct ModuleRange {
  va_t Start = 0;
  va_t End = 0;
  uint16_t Module = 0;
  uint16_t Section = 0;
  bool ReadOnlyData = false;
};

/// Consume validated, start-sorted contributions. An overlapping component
/// containing code, writable storage or contradictory sections supplies no
/// data interval. Adjacent read-only components in one section share storage.
inline std::vector<std::pair<va_t, va_t>>
readOnlyDataRegions(llvm::ArrayRef<ModuleRange> Contributions) {
  std::vector<std::pair<va_t, va_t>> Regions;
  uint16_t PreviousSection = 0;
  for (size_t I = 0; I < Contributions.size();) {
    const auto &First = Contributions[I++];
    va_t End = First.End;
    bool ReadOnly = First.ReadOnlyData;
    while (I < Contributions.size() && Contributions[I].Start < End) {
      const auto &Next = Contributions[I++];
      ReadOnly &= Next.ReadOnlyData && Next.Section == First.Section;
      End = std::max(End, Next.End);
    }
    if (!ReadOnly)
      continue;
    if (!Regions.empty() && Regions.back().second == First.Start &&
        PreviousSection == First.Section)
      Regions.back().second = End;
    else
      Regions.emplace_back(First.Start, End);
    PreviousSection = First.Section;
  }
  return Regions;
}

} // namespace neverd::pdb_loader_detail

#endif // NEVERD_DEBUG_PDB_DATA_CONTRIBUTIONS_H
