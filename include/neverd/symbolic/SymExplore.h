//===- SymExplore.h - Walking every path a function can take ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runs a whole function symbolically instead of one straight line of it, by
/// forking at every branch whose outcome the code does not already decide.
///
/// Two things make that finite.  A budget, because a loop whose trip count is
/// unknown has no last iteration and something has to say when to stop
/// unrolling it.  And, before any budget is reached, the fact that a great
/// many branches are not branches at all: a predicate that folds to a constant
/// has one reachable side, and the walk takes it without forking.
///
/// That second point is not a detail.  Opaque predicates — a condition
/// contrived to always hold, guarding code that never runs — are the standard
/// way to bloat a control-flow graph, and they disappear here as a consequence
/// of the expression builders folding, without a rule that mentions them and
/// without asking a solver anything.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMEXPLORE_H
#define NEVERD_SYMBOLIC_SYMEXPLORE_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/symbolic/SymState.h"

#include <cstddef>
#include <vector>

namespace neverd::symbolic {

/// Why a path stopped.
enum class PathOutcome : uint8_t {
  /// Reached a return.  The interesting one: the state is what the function
  /// leaves behind when it takes this path.
  Returned,
  /// Branched to an address this function does not contain — a tail call, or
  /// an edge out of what was lifted.
  LeftFunction,
  /// Reached a branch through an address that did not come out constant.
  UnresolvedBranch,
  /// Ran out of the allowance for re-entering one block, which is what bounds
  /// a loop nothing else bounds.
  LoopBudget,
  /// Ran out of the allowance for operations.
  StepBudget,
  /// Every way on from here contradicts something already assumed.
  Infeasible,
};

const char *pathOutcomeName(PathOutcome Outcome);

struct ExploreOptions {
  /// How many reachable paths to finish before giving up on the rest.
  /// Infeasible diagnostics do not consume this allowance.
  unsigned MaxPaths = 64;
  /// Operations allowed along one path.
  unsigned MaxSteps = 1u << 16;
  /// How often one path may enter the same block.  This is the loop bound:
  /// two visits is one iteration plus the exit, so the default unrolls a
  /// couple of times before conceding.
  unsigned MaxBlockVisits = 3;
  /// Target byte order used for register and memory split/join operations.
  llvm::endianness ByteOrder = llvm::endianness::little;
  /// Register byte ranges preserved by the target's calling convention.
  std::vector<SymRegisterRange> CallPreservedRegisters;
};

/// One finished path.
struct SymPath {
  PathOutcome Outcome = PathOutcome::StepBudget;
  /// The block it stopped in.
  int BlockId = -1;
  /// Everything assumed along the way, in the order it was assumed.  A branch
  /// the code already decided contributes nothing, so this lists the genuine
  /// choices and only those.
  std::vector<SymRef> Constraints;
  /// The blocks entered, in order.  What the path *is*.
  std::vector<int> Blocks;
  /// The machine state where it stopped.
  SymState State;
  /// For \c UnresolvedBranch and \c LeftFunction, where it was trying to go.
  SymRef Target;
  /// Operations conservatively replaced by unknown values along this path.
  unsigned UnmodelledOps = 0;

  /// The conjunction of the constraints, or true when there are none.
  SymRef predicate(SymContext &Ctx) const;
};

/// Paths and completeness information from one function walk.
struct SymExploration {
  std::vector<SymPath> Paths;
  /// True only when every reachable frontier ended without a path or loop
  /// budget and without an unresolved indirect branch.
  bool Complete = false;
  unsigned ReachablePaths = 0;
  size_t ExecutedSteps = 0;
  unsigned UnmodelledOps = 0;
};

SymExploration explorePathsDetailed(SymContext &Ctx, const LowFunc &Func,
                                    const ExploreOptions &Opts = {});

/// Walk \p Func from its entry block.
///
/// Paths come back in the order they finished, which for the depth-first walk
/// used here means the first is the one that kept taking the branch.
std::vector<SymPath> explorePaths(SymContext &Ctx, const LowFunc &Func,
                                  const ExploreOptions &Opts = {});

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMEXPLORE_H
