//===- ResolverPendingWalks.h - Active recursive proof dependencies ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded dependency tracking for provisional reaching-value results.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_JUMPTABLE_RESOLVERPENDINGWALKS_H
#define NEVERD_IR_LOW_JUMPTABLE_RESOLVERPENDINGWALKS_H

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

namespace neverd::detail {

/// Tracks dependencies on active recursive value/memory queries. A memoized
/// result is provisional until every skipped ancestor has finished. The deepest
/// actual ancestor's unique activation certifies that all shallower ones are
/// still live; reusing its stack depth cannot revive an old snapshot.
///
/// Exact bits avoid treating unrelated intermediate frames as dependencies.
/// Storage is fixed: caller-specified depths above 255 use a conservative
/// interval there, without changing the caller's depth limit. Finishing removes
/// only the current activation and publishes the remaining dependencies to the
/// parent. Value and memory queries must share one owner.
struct ResolverPendingWalks {
  struct Pending {
    uint64_t LastIdentity = 0;
    uint32_t First = 0;
    uint32_t Last = 0;
    // Track actual skipped activations in the common bounded depth range.
    // Greater caller-specified depths retain the conservative interval above.
    std::array<uint64_t, 4> Bits{};
    bool empty() const { return LastIdentity == 0; }
  };
  struct Frame {
    uint64_t Identity;
    Pending Dependencies;
  };

  std::vector<Frame> Frames;
  uint64_t NextIdentity = 0;
  bool live(const Pending &R) const {
    return R.empty() || (R.Last < Frames.size() &&
                         Frames[R.Last].Identity == R.LastIdentity);
  }
  void observe(const Pending &R) {
    if (R.empty() || Frames.empty())
      return;
    assert(live(R));
    auto &P = Frames.back().Dependencies;
    if (P.empty()) {
      P = R;
      return;
    }
    for (size_t I = 0; I < P.Bits.size(); ++I)
      P.Bits[I] |= R.Bits[I];
    P.First = std::min(P.First, R.First);
    if (P.Last < R.Last) {
      P.Last = R.Last;
      P.LastIdentity = R.LastIdentity;
    }
  }
  struct Scope {
    ResolverPendingWalks &Owner;
    Pending Self;
    bool Finished = false;
    explicit Scope(ResolverPendingWalks &Owner) : Owner(Owner) {
      assert(Owner.Frames.size() < std::numeric_limits<uint32_t>::max());
      const uint32_t Depth = Owner.Frames.size();
      Self = {++Owner.NextIdentity, Depth, Depth};
      if (Depth < Self.Bits.size() * 64)
        Self.Bits[Depth / 64] = uint64_t{1} << (Depth % 64);
      Owner.Frames.push_back({Self.LastIdentity, {}});
    }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    ~Scope() {
      if (!Finished) {
        assert(Owner.Frames.size() == Self.Last + 1);
        Owner.Frames.pop_back();
      }
    }
    Pending finish() {
      assert(Owner.Frames.size() == Self.Last + 1);
      auto P = Owner.Frames.back().Dependencies;
      Owner.Frames.pop_back();
      Finished = true;
      // The completed state closes its own transparent backedges. Retain
      // dependencies on enclosing walks; their missing arms are still live.
      if (!P.empty() && P.Last == Self.Last) {
        if (P.Last < P.Bits.size() * 64)
          P.Bits[P.Last / 64] &= ~(uint64_t{1} << (P.Last % 64));
        if (P.First == Self.Last) {
          P = {};
        } else if (P.Last > P.Bits.size() * 64) {
          --P.Last;
          P.LastIdentity = Owner.Frames[P.Last].Identity;
        } else {
          P.LastIdentity = 0;
          for (size_t I = P.Bits.size(); I != 0; --I) {
            if (!P.Bits[I - 1])
              continue;
            P.Last = (I - 1) * 64 + 63 - std::countl_zero(P.Bits[I - 1]);
            P.LastIdentity = Owner.Frames[P.Last].Identity;
            break;
          }
          if (P.empty())
            P = {};
        }
      }
      Owner.observe(P);
      return P;
    }
  };
};

} // namespace neverd::detail

#endif
