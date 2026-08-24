//===- SBFFunctionBody.h - Semantic SBF function CFG views -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Builds a compact typed intraprocedural CFG index and enumerates the
/// semantic body of a function without assigning shared tails to an arbitrary
/// owner. The index stores the graph, never its potentially quadratic
/// transitive closure; each query materializes only the result it requests.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_ANALYSIS_SBFFUNCTIONBODY_H
#define NEVERD_SBF_ANALYSIS_SBFFUNCTIONBODY_H

#include "neverd/sbf/SBFIR.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace neverd::sbf {

class FunctionBodyIndex {
public:
  /// Logical resident sizes. These counters make graph-index scale regressions
  /// observable without depending on host timing or allocator details.
  struct Statistics {
    size_t IndexedBlockCount = 0;
    size_t IndexedEdgeCount = 0;
    size_t IndexedFunctionCount = 0;
    size_t ResidentIndexEntryCount = 0;
    /// Number of batched block-to-function provenance requests.
    size_t BlockFunctionBatchQueryCount = 0;
    /// Requests that could not use unique ownership and therefore traversed
    /// the reverse CFG. One batch performs at most one such traversal.
    size_t ReverseReachabilityQueryCount = 0;
    /// Reverse visited-set workspaces actually initialized. A batch containing
    /// only uniquely owned blocks leaves this at zero, regardless of program
    /// size.
    size_t ReverseWorkspaceInitializationCount = 0;
  };

  /// Exact-or-unknown result of a whole-program function-size query. Bytes is
  /// aligned with HighIR::Functions. A cleared Exact bit always has byte value
  /// zero; no caller can mistake a budget-truncated partial traversal for a
  /// semantic function size.
  struct ByteSizeBatch {
    std::vector<uint64_t> Bytes;
    llvm::BitVector Exact;
    size_t BlockVisitCount = 0;
    /// Number of exact reachability traversals actually entered.
    size_t ReachabilityQueryCount = 0;
    /// Number of visited-set workspaces initialized for this batch. It is at
    /// most one and remains zero when the budget rejects all shared queries.
    size_t WorkspaceInitializationCount = 0;
    bool BudgetExhausted = false;
  };

  struct BlockGroupQuery {
    llvm::ArrayRef<size_t> Blocks;
  };

  /// Exact caller sets for a batch of block groups. FunctionOffsets is a CSR
  /// index aligned with the input groups. An exhausted result deliberately
  /// owns no relation, so consumers cannot publish a partial call graph.
  struct BlockGroupFunctionBatch {
    std::vector<size_t> FunctionOffsets;
    std::vector<size_t> FunctionIDs;
    size_t ChargedBlockWork = 0;
    size_t BlockVisitCount = 0;
    size_t FunctionTraversalCount = 0;
    size_t ReverseGroupTraversalCount = 0;
    size_t WorkspaceInitializationCount = 0;
    size_t OutputRelationCount = 0;
    bool VisitBudgetExhausted = false;
    bool OutputBudgetExhausted = false;

    [[nodiscard]] bool complete() const {
      return !VisitBudgetExhausted && !OutputBudgetExhausted;
    }

    [[nodiscard]] llvm::ArrayRef<size_t>
    functionsForGroup(size_t GroupID) const {
      if (FunctionOffsets.empty() || GroupID >= FunctionOffsets.size() - 1)
        return {};
      return llvm::ArrayRef(FunctionIDs)
          .slice(FunctionOffsets[GroupID],
                 FunctionOffsets[GroupID + 1] - FunctionOffsets[GroupID]);
    }
  };

  explicit FunctionBodyIndex(const SBFProgram &Program);

  /// Sum the encoded sizes of the semantic body without materializing its
  /// block list.
  [[nodiscard]] uint64_t byteSize(const Function &Function) const;

  /// Compute every semantic function size under one global block-visit
  /// budget. Uniquely owned blocks are charged once, functions with identical
  /// shared-tail frontiers reuse one traversal, and genuinely distinct
  /// reachability sets consume the supplied query budget. On exhaustion the
  /// affected size is zero (unknown), never a partial sum.
  [[nodiscard]] ByteSizeBatch byteSizes(size_t BlockVisitBudget) const;

  /// Resolve the semantic functions containing any source block in each
  /// group. Ambiguous provenance adaptively chooses the smaller of grouped
  /// reverse reachability and candidate-function forward reachability;
  /// uniquely owned blocks need no graph workspace. Both host work and the
  /// caller-group relation are exact-or-fail budgets.
  [[nodiscard]] BlockGroupFunctionBatch
  functionsForBlockGroups(llvm::ArrayRef<BlockGroupQuery> Groups,
                          size_t BlockVisitBudget,
                          size_t OutputRelationBudget) const;

  /// Return every block reachable from Function.EntrySlot through typed
  /// intraprocedural edges. Other function entries are hard boundaries;
  /// shared non-entry tails deliberately appear in every reaching body. The
  /// returned vector is sorted and owns its query-local storage.
  [[nodiscard]] std::vector<size_t> blocks(const Function &Function) const;

  /// Return the functions whose semantic bodies contain BlockID. The returned
  /// vector is sorted and owns its query-local storage; the index deliberately
  /// does not memoize whole-program reachability closures.
  [[nodiscard]] std::vector<size_t> functionsForBlock(size_t BlockID) const;

  /// Return the sorted union of functions whose semantic bodies contain any
  /// requested block. Repeated blocks are coalesced, uniquely owned blocks use
  /// the HighIR owner index directly, and all ambiguous blocks share one
  /// multi-source reverse traversal. This is the call-graph query: many call
  /// sites for one target must not each walk the whole CFG.
  [[nodiscard]] std::vector<size_t>
  functionsForAnyBlock(llvm::ArrayRef<size_t> BlockIDs) const;

  [[nodiscard]] const Statistics &statistics() const { return Stats; }

private:
  using FlatEdge = std::pair<size_t, size_t>;

  struct FlatAdjacency {
    std::vector<size_t> Offsets;
    std::vector<size_t> Targets;

    [[nodiscard]] llvm::ArrayRef<size_t> operator[](size_t Node) const;
  };

  [[nodiscard]] static FlatAdjacency
  buildFlatAdjacency(size_t Count, llvm::ArrayRef<FlatEdge> Edges,
                     bool Reverse = false);
  [[nodiscard]] llvm::BitVector
  reachableSet(size_t Root, const FlatAdjacency &Adjacency) const;
  [[nodiscard]] size_t functionID(const Function &Function) const;

  llvm::DenseMap<size_t, size_t> FunctionIDsByEntrySlot;
  std::vector<size_t> FunctionEntryBlocks;
  FlatAdjacency Successors;
  FlatAdjacency Predecessors;
  std::vector<size_t> SeedOffsets;
  std::vector<size_t> SeedFunctions;
  /// Functions whose uniquely owned region enters shared/ambiguous CFG. This
  /// is the exact candidate set for an adaptive forward provenance query when
  /// the validated owner authority is available; otherwise it contains every
  /// valid function root.
  std::vector<size_t> AmbiguousReachableFunctions;
  std::vector<uint64_t> BlockByteSizes;
  /// Non-owning unique/ambiguous ownership authority. The SBFProgram passed to
  /// the constructor must outlive this query index and its HighIR must not be
  /// mutated while queries are in flight.
  llvm::ArrayRef<size_t> FunctionOwners;
  mutable Statistics Stats;
};

} // namespace neverd::sbf

#endif // NEVERD_SBF_ANALYSIS_SBFFUNCTIONBODY_H
