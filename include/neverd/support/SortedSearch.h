//===- SortedSearch.h - Bounded ordered integer lookup ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Allocation-free lower bounds over sorted, immutable unsigned keys.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_SORTEDSEARCH_H
#define NEVERD_SUPPORT_SORTEDSEARCH_H

#include "neverd/support/Limits.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace neverd::sorted {
namespace detail {
size_t longLowerBound64(const uint64_t *Data, size_t Size, uint64_t Key);
bool longContains64(const uint64_t *Data, size_t Size, uint64_t Key);
} // namespace detail

/// Index of the first key >= Key, or Size. Data contains Size sorted keys;
/// duplicate keys and the complete uint64_t range are supported. An empty
/// input may be null. No padding is read and no state is retained.
inline size_t lowerBound64(const uint64_t *Data, size_t Size, uint64_t Key) {
  if (Size < limits::support::kSortedSearchMinimumSIMDEntries)
    return static_cast<size_t>(std::lower_bound(Data, Data + Size, Key) - Data);
  return detail::longLowerBound64(Data, Size, Key);
}

inline bool contains64(const uint64_t *Data, size_t Size, uint64_t Key) {
  if (Size < limits::support::kSortedSearchMinimumSIMDEntries)
    return std::binary_search(Data, Data + Size, Key);
  if constexpr (limits::support::kSortedSearchUseDirectMembership) {
    return detail::longContains64(Data, Size, Key);
  } else {
    const size_t Index = detail::longLowerBound64(Data, Size, Key);
    return Index != Size && Data[Index] == Key;
  }
}
} // namespace neverd::sorted

#endif // NEVERD_SUPPORT_SORTEDSEARCH_H
