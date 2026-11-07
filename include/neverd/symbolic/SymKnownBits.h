//===- SymKnownBits.h - Bounded facts about the symbolic DAG --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMKNOWNBITS_H
#define NEVERD_SYMBOLIC_SYMKNOWNBITS_H

#include "neverd/symbolic/Limits.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/KnownBits.h"

namespace neverd::symbolic {

/// Read-only bit facts for one append-only SymContext. The context must outlive
/// this object and must not be moved or replaced. Facts describe the total
/// symbolic operations, not LLVM poison, instruction flags or path premises.
/// Callers retain responsibility for those obligations.
class SymKnownBits {
public:
  static constexpr unsigned MaxQueryWork =
      ::neverd::limits::symbolic::kSymKnownBitsMaxQueryWork;
  static constexpr unsigned MaxDepth =
      ::neverd::limits::symbolic::kSymKnownBitsMaxDepth;
  static constexpr unsigned MaxWidth =
      ::neverd::limits::symbolic::kSymKnownBitsMaxWidth;
  static constexpr unsigned MaxCachedFacts =
      ::neverd::limits::symbolic::kSymKnownBitsMaxCachedFacts;

  explicit SymKnownBits(const SymContext &Context) : C(Context) {}
  SymKnownBits(const SymKnownBits &) = delete;
  SymKnownBits &operator=(const SymKnownBits &) = delete;
  SymKnownBits(SymKnownBits &&) = delete;
  SymKnownBits &operator=(SymKnownBits &&) = delete;

  /// Charge visited nodes, bitvector words and operand searches to WorkBudget,
  /// retaining a separate per-query ceiling. Invalid refs, unsupported widths
  /// and insufficient work return nullopt. Unhandled operations and the depth
  /// boundary supply unknown bits; enclosing operations may still prove facts.
  /// Cached facts are bounded and belong only to this context. Cache hits are
  /// charged too; a failed query may retain sound facts about visited children.
  /// Width and branch-common facts survive an exhausted child. Only
  /// complete facts persist across queries; partial facts can improve
  /// with more work on the next query.
  std::optional<llvm::KnownBits> query(SymRef R, unsigned &WorkBudget);

private:
  const SymContext &C;
  struct Query;
  llvm::DenseMap<uint32_t, llvm::KnownBits> Cache;

  std::optional<llvm::KnownBits> infer(SymRef R, Query &Q, unsigned Depth);
  std::optional<llvm::KnownBits> compute(SymRef R, Query &Q, unsigned Depth);
  std::optional<llvm::KnownBits>
  compare(SymRef R, llvm::ArrayRef<llvm::KnownBits> Children, Query &Q,
          unsigned Depth);
  std::optional<bool> nonwrappingAtLeast(SymRef Sum, SymRef Term, Query &Q,
                                         unsigned Depth);
  std::optional<bool> sameLowBits(SymRef A, SymRef B, unsigned Width, Query &Q,
                                  unsigned Depth);
  std::optional<bool> sameLowProduct(SymRef A, SymRef B, unsigned Width,
                                     Query &Q, unsigned Depth);
};

} // namespace neverd::symbolic

#endif
