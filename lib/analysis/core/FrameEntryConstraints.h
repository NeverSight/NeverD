//===- FrameEntryConstraints.h - Explicit nonwrapping entry domain --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETER_FRAMEENTRYCONSTRAINTS_H
#define NEVERD_ANALYSIS_INTERPRETER_FRAMEENTRYCONSTRAINTS_H

#include "neverd/symbolic/SymExpr.h"

#include <cassert>
#include <cstdint>
#include <utility>

namespace neverd::analysis::detail {

/// Inclusive root bounds shared by symbolic predicates and source guards.
/// The last byte may be UINT64_MAX; the exclusive end is never materialized.
inline std::pair<uint64_t, uint64_t> nonwrappingFrameRootBounds(int64_t Begin,
                                                                int64_t End) {
  assert(Begin < End);
  return {Begin < 0 ? uint64_t{0} - static_cast<uint64_t>(Begin) : 0,
          End > 0 ? UINT64_MAX - static_cast<uint64_t>(End - 1) : UINT64_MAX};
}

/// Caller validates a 64-bit Root and Begin < End. Bound the first and last
/// bytes without signed negation or constructing a wrapping exclusive end.
/// This does not establish memory accessibility, provenance or disjointness.
inline symbolic::SymRef nonwrappingFramePredicate(symbolic::SymContext &Ctx,
                                                  symbolic::SymRef Root,
                                                  int64_t Begin, int64_t End) {
  assert(Root && Ctx.width(Root) == 64 && Begin < End);
  const auto [Minimum, Maximum] = nonwrappingFrameRootBounds(Begin, End);
  auto Predicate = Ctx.mkTrue();
  if (Begin < 0)
    Predicate = Ctx.mkAnd(Predicate, Ctx.mkUle(Ctx.mkConst(64, Minimum), Root));
  if (End > 0)
    Predicate = Ctx.mkAnd(Predicate, Ctx.mkUle(Root, Ctx.mkConst(64, Maximum)));
  return Predicate;
}

/// Nonwrapping frame bounds must already hold. Compare first and last bytes
/// so adjacency remains valid, including a frame whose last byte is UINT64_MAX.
inline symbolic::SymRef excludedFrameRangePredicate(symbolic::SymContext &Ctx,
                                                    symbolic::SymRef Root,
                                                    int64_t Begin, int64_t End,
                                                    uint64_t ExcludedBegin,
                                                    uint64_t ExcludedEnd) {
  assert(Root && Ctx.width(Root) == 64 && Begin < End &&
         ExcludedBegin < ExcludedEnd);
  const auto First =
      Ctx.mkAdd(Root, Ctx.mkConst(64, static_cast<uint64_t>(Begin)));
  const auto Last =
      Ctx.mkAdd(Root, Ctx.mkConst(64, static_cast<uint64_t>(End) - 1));
  return Ctx.mkOr(Ctx.mkUlt(Last, Ctx.mkConst(64, ExcludedBegin)),
                  Ctx.mkUle(Ctx.mkConst(64, ExcludedEnd), First));
}

/// Both complete relative ranges must already be nonwrapping. Distinct root
/// identities do not imply different values or disjoint memory. Compare their
/// first/last bytes so adjacency and a final byte at UINT64_MAX remain valid.
inline symbolic::SymRef disjointFrameRangesPredicate(symbolic::SymContext &Ctx,
                                                     symbolic::SymRef Root,
                                                     int64_t Begin, int64_t End,
                                                     symbolic::SymRef OtherRoot,
                                                     int64_t OtherBegin,
                                                     int64_t OtherEnd) {
  assert(Root && OtherRoot && Ctx.width(Root) == 64 &&
         Ctx.width(OtherRoot) == 64 && Begin < End && OtherBegin < OtherEnd);
  const auto First =
      Ctx.mkAdd(Root, Ctx.mkConst(64, static_cast<uint64_t>(Begin)));
  const auto Last =
      Ctx.mkAdd(Root, Ctx.mkConst(64, static_cast<uint64_t>(End) - 1));
  const auto OtherFirst =
      Ctx.mkAdd(OtherRoot, Ctx.mkConst(64, static_cast<uint64_t>(OtherBegin)));
  const auto OtherLast = Ctx.mkAdd(
      OtherRoot, Ctx.mkConst(64, static_cast<uint64_t>(OtherEnd) - 1));
  return Ctx.mkOr(Ctx.mkUlt(Last, OtherFirst), Ctx.mkUlt(OtherLast, First));
}

/// A nullable result may be zero. Every nonzero result names a complete,
/// nonwrapping object disjoint from the entry frame. The caller still owns the
/// relation between a callee's result and this object; no identity follows
/// from the root's symbolic name alone.
inline symbolic::SymRef nullableObjectFramePredicate(
    symbolic::SymContext &Ctx, symbolic::SymRef FrameRoot, int64_t FrameBegin,
    int64_t FrameEnd, symbolic::SymRef ObjectRoot, int64_t ObjectBytes) {
  assert(ObjectBytes > 0);
  const auto Nonzero = Ctx.mkAnd(
      nonwrappingFramePredicate(Ctx, ObjectRoot, 0, ObjectBytes),
      disjointFrameRangesPredicate(Ctx, FrameRoot, FrameBegin, FrameEnd,
                                   ObjectRoot, 0, ObjectBytes));
  return Ctx.mkOr(Ctx.mkEq(ObjectRoot, Ctx.mkConst(64, 0)), Nonzero);
}

/// A caller-declared nonnull result object misses this absolute range.
/// The null outcome remains possible and gains no object extent.
inline symbolic::SymRef nullableObjectExclusionPredicate(
    symbolic::SymContext &Ctx, symbolic::SymRef ObjectRoot, int64_t ObjectBytes,
    uint64_t ExcludedBegin, uint64_t ExcludedEnd) {
  assert(ObjectRoot && Ctx.width(ObjectRoot) == 64 && ObjectBytes > 0 &&
         ExcludedBegin < ExcludedEnd);
  return Ctx.mkOr(Ctx.mkEq(ObjectRoot, Ctx.mkConst(64, 0)),
                  excludedFrameRangePredicate(Ctx, ObjectRoot, 0, ObjectBytes,
                                              ExcludedBegin, ExcludedEnd));
}

} // namespace neverd::analysis::detail

#endif
