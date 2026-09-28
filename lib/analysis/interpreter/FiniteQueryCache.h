//===- FiniteQueryCache.h - Bounded finite-proof reuse ------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_INTERPRETER_FINITEQUERYCACHE_H
#define NEVERD_ANALYSIS_INTERPRETER_FINITEQUERYCACHE_H

#include "FiniteValues.h"

#include <map>

namespace neverd::analysis::detail {

/// Run-local reuse of completed finite-domain proofs. Keys describe the exact
/// ordered query DAG modulo a consistent renaming of its free variables.
/// Names and machine-input provenance are not solver semantics. No context,
/// symbolic reference, partial model, or solver resource failure is retained.
///
/// MaxWords bounds both one key construction and the total retained keys and
/// results, including structural overhead. Full caches keep existing entries;
/// an unsupported or oversized query simply misses and is not recorded.
class FiniteQueryCache {
public:
  explicit FiniteQueryCache(uint64_t MaxWords) : MaxWords(MaxWords) {}

  std::optional<FiniteValues> lookup(const symbolic::SymContext &Ctx,
                                     symbolic::SymRef Predicate,
                                     llvm::ArrayRef<symbolic::SymRef> Values,
                                     uint32_t Limit) const;

  /// Result must come from a proof of this exact query. Only Complete and
  /// TooManyValues are accepted; both are independent of solver budgets.
  void store(const symbolic::SymContext &Ctx, symbolic::SymRef Predicate,
             llvm::ArrayRef<symbolic::SymRef> Values, uint32_t Limit,
             const FiniteValues &Result);

private:
  using Key = std::vector<uint64_t>;
  uint64_t MaxWords;
  uint64_t StoredWords = 0;
  std::map<Key, FiniteValues> Entries;
};

} // namespace neverd::analysis::detail

#endif // NEVERD_ANALYSIS_INTERPRETER_FINITEQUERYCACHE_H
