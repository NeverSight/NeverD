//===- SortedSearch.cpp - SIMD tails for ordered integer lookup -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Binary narrowing followed by independent comparisons of a bounded tail.
/// This applies bbtree's dense-key comparison idea to existing sorted arrays;
/// it introduces neither tree nodes nor padding reads or software prefetches.
///
//===----------------------------------------------------------------------===//

#include "neverd/support/SortedSearch.h"

#if NEVERD_USE_HIGHWAY
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "SortedSearch.cpp"
#include "hwy/foreach_target.h"
#include "hwy/highway.h"

HWY_BEFORE_NAMESPACE();
namespace neverd::sorted::detail {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

template <bool Membership>
auto search64(const uint64_t *Data, size_t Size, uint64_t Key) {
#if HWY_TARGET == HWY_SCALAR
  if constexpr (Membership)
    return std::binary_search(Data, Data + Size, Key);
  else
    return static_cast<size_t>(std::lower_bound(Data, Data + Size, Key) - Data);
#else
  constexpr size_t Tail = limits::support::kSortedSearchSIMDTailEntries;
  static_assert(Tail > 1 &&
                Tail <= limits::support::kSortedSearchMinimumSIMDEntries);
  const hn::CappedTag<uint64_t, Tail> D;
#if HWY_ARCH_X86
  if constexpr (hn::MaxLanes(D) <
                limits::support::kSortedSearchMinimumX86SIMDLanes) {
    if constexpr (Membership)
      return std::binary_search(Data, Data + Size, Key);
    else
      return static_cast<size_t>(std::lower_bound(Data, Data + Size, Key) -
                                 Data);
  }
#endif
  const size_t Last = Size - Tail;
  size_t Base = 0;
  // A lower bound may equal the interval's end. Membership needs that element
  // inside the loaded tail, so leave room for the inclusive upper candidate.
  while (Size > (Membership ? Tail - 1 : Tail)) {
    const size_t Half = Size / 2;
#if defined(__clang__)
    // Retain the pivot on the right with a query-independent interval length.
    // Keep Clang's select-to-branch conversion from serializing random probes.
    Base += __builtin_unpredictable(Data[Base + Half] < Key) ? Half : 0;
    Size -= Half;
#else
    // Preserve the standard bisection shape for compilers whose scalar search
    // benefits from predictable branches on ordered queries.
    if (Data[Base + Half] < Key) {
      Base += Half + 1;
      Size -= Half + 1;
    } else {
      Size = Half;
    }
#endif
  }
  // A complete tail avoids remainder loops. Extending the interval right adds
  // only keys >= Key. At EOF, shifting it left adds keys < Key; the absolute
  // rank stays the same. Every load remains inside the original input.
  Base = std::min(Base, Last);
  const size_t Width = hn::Lanes(D);
  const auto Target = hn::Set(D, Key);
  size_t I = 0;
  if constexpr (Membership) {
    auto Matches = hn::MaskFalse(D);
    for (; Tail - I >= Width; I += Width)
      Matches = hn::Or(Matches, hn::Eq(hn::LoadU(D, Data + Base + I), Target));
    bool Found = !hn::AllFalse(D, Matches);
    for (; I != Tail; ++I)
      Found |= Data[Base + I] == Key;
    return Found;
  } else {
    size_t Below = 0;
    for (; Tail - I >= Width; I += Width)
      Below += hn::CountTrue(D, hn::Lt(hn::LoadU(D, Data + Base + I), Target));
    // Scalable vectors need not divide Tail (for example, 384-bit SVE).
    for (; I != Tail; ++I)
      Below += Data[Base + I] < Key;
    return Base + Below;
  }
#endif
}

size_t lowerBound64Impl(const uint64_t *Data, size_t Size, uint64_t Key) {
  return search64<false>(Data, Size, Key);
}

bool contains64Impl(const uint64_t *Data, size_t Size, uint64_t Key) {
  return search64<true>(Data, Size, Key);
}
} // namespace HWY_NAMESPACE
} // namespace neverd::sorted::detail
HWY_AFTER_NAMESPACE();
#endif

#if !NEVERD_USE_HIGHWAY || HWY_ONCE
namespace neverd::sorted::detail {
#if NEVERD_USE_HIGHWAY
HWY_EXPORT(lowerBound64Impl);
HWY_EXPORT(contains64Impl);
#endif

size_t longLowerBound64(const uint64_t *Data, size_t Size, uint64_t Key) {
#if NEVERD_USE_HIGHWAY
  return HWY_DYNAMIC_DISPATCH(lowerBound64Impl)(Data, Size, Key);
#else
  return static_cast<size_t>(std::lower_bound(Data, Data + Size, Key) - Data);
#endif
}

bool longContains64(const uint64_t *Data, size_t Size, uint64_t Key) {
#if NEVERD_USE_HIGHWAY
  return HWY_DYNAMIC_DISPATCH(contains64Impl)(Data, Size, Key);
#else
  return std::binary_search(Data, Data + Size, Key);
#endif
}
} // namespace neverd::sorted::detail
#endif
