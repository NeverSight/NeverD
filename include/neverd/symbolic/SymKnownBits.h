//===- SymKnownBits.h - Bounded facts about the symbolic DAG --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMKNOWNBITS_H
#define NEVERD_SYMBOLIC_SYMKNOWNBITS_H

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
  static constexpr unsigned MaxQueryWork = 256;
  static constexpr unsigned MaxDepth = 32;
  static constexpr unsigned MaxWidth = 128;
  static constexpr unsigned MaxCachedFacts = 65536;

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
  std::optional<llvm::KnownBits> query(SymRef R, unsigned &WorkBudget);

private:
  const SymContext &C;
  llvm::DenseMap<uint32_t, llvm::KnownBits> Cache;

  std::optional<llvm::KnownBits> infer(SymRef R, unsigned &Remaining,
                                       unsigned Depth);
  std::optional<llvm::KnownBits> compute(SymRef R, unsigned &Remaining,
                                         unsigned Depth);
  std::optional<llvm::KnownBits>
  compare(SymRef R, llvm::ArrayRef<llvm::KnownBits> Children,
          unsigned &Remaining, unsigned Depth);
  std::optional<bool> containsNonwrappingSum(SymRef Sum, SymRef Term,
                                             unsigned &Remaining,
                                             unsigned Depth);
  std::optional<bool> sameLowBits(SymRef A, SymRef B, unsigned Width,
                                  unsigned &Remaining, unsigned Depth);
};

} // namespace neverd::symbolic

#endif
