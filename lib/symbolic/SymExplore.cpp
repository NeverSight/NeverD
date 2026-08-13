//===- SymExplore.cpp - Walking every path a function can take ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the depth-first walk described in SymExplore.h.
///
/// A path is a state, a list of what has been assumed, and where to carry on
/// from; forking one is copying it.  The walk keeps them on a stack and takes
/// the most recent, so it follows one route to its end before starting the
/// next — which is what makes the first result the deepest rather than the
/// shallowest, and keeps the number of live states proportional to the depth
/// of the graph instead of its width.
///
//===----------------------------------------------------------------------===//

#include "neverd/symbolic/SymExplore.h"

#include "neverd/symbolic/SymExec.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <utility>

namespace neverd::symbolic {

const char *pathOutcomeName(PathOutcome Outcome) {
  switch (Outcome) {
  case PathOutcome::Returned:
    return "returned";
  case PathOutcome::LeftFunction:
    return "left_function";
  case PathOutcome::UnresolvedBranch:
    return "unresolved_branch";
  case PathOutcome::LoopBudget:
    return "loop_budget";
  case PathOutcome::StepBudget:
    return "step_budget";
  case PathOutcome::Infeasible:
    return "infeasible";
  }
  llvm_unreachable("unhandled symbolic path outcome");
}

SymRef SymPath::predicate(SymContext &Ctx) const {
  if (Constraints.empty())
    return Ctx.mkTrue();
  return Ctx.mkAnd(Constraints);
}

namespace {

/// A path still being walked.  Forking one is copying it.
struct Frontier {
  Frontier(SymState State, int BlockId)
      : State(std::move(State)), BlockId(BlockId) {}

  SymState State;
  std::vector<SymRef> Constraints;
  std::vector<int> Blocks;
  /// How often this path has entered each block, which is what the loop bound
  /// is counted against.  Per path, because two paths through a loop have
  /// nothing to say about each other.
  llvm::DenseMap<int, unsigned> Visits;
  int BlockId;
  bool Infeasible = false;
  unsigned Steps = 0;
  unsigned UnmodelledOps = 0;
};

/// Add one side of a branch and decide every contradiction the expression
/// builders can prove on their own.  This deliberately stays solver-free: a
/// non-constant conjunction is unknown, never guessed to be impossible.
bool addConstraint(SymContext &Ctx, Frontier &F, SymRef Condition) {
  F.Constraints.push_back(Condition);
  SymRef Predicate =
      F.Constraints.size() == 1 ? Condition : Ctx.mkAnd(F.Constraints);
  std::optional<llvm::APInt> Decided = Ctx.asConst(Predicate);
  return !Decided || !Decided->isZero();
}

/// Everything the walk needs to know about the shape of the function, worked
/// out once.
class BlockIndex {
public:
  explicit BlockIndex(const LowFunc &Func) : Func(Func) {
    for (size_t I = 0; I < Func.Blocks.size(); ++I)
      ByAddress[Func.Blocks[I].StartAddr] = static_cast<int>(I);
  }

  const LowBlock *block(int Id) const {
    return Id >= 0 && Id < static_cast<int>(Func.Blocks.size())
               ? &Func.Blocks[Id]
               : nullptr;
  }

  /// The block starting at \p Addr, or -1 when the function has none.
  int at(uint64_t Addr) const {
    auto It = ByAddress.find(Addr);
    return It == ByAddress.end() ? -1 : It->second;
  }

  int entry() const { return Func.Blocks.empty() ? -1 : 0; }

  /// The successor that is not \p Taken, which is where a conditional branch
  /// falls through to.  Blocks carry their successors, so the fallthrough does
  /// not have to be guessed from an address.
  int fallthroughOf(const LowBlock &B, int Taken) const {
    // A target outside the function does not appear in Succs, leaving the one
    // in-function fallthrough as the sole edge.  With several edges, however,
    // a failed target resolution identifies none of them: every successor
    // differs from -1, so the loop below would silently guess Succs[0].
    if (Taken < 0)
      return B.Succs.size() == 1 ? B.Succs.front() : -1;
    for (int S : B.Succs)
      if (S != Taken)
        return S;
    return -1;
  }

private:
  const LowFunc &Func;
  llvm::DenseMap<uint64_t, int> ByAddress;
};

/// Where a constant branch target points, as a block.
int resolve(const SymContext &Ctx, const BlockIndex &Index, SymRef Target) {
  std::optional<llvm::APInt> Addr = Ctx.asConst(Target);
  return Addr && Addr->getActiveBits() <= 64 ? Index.at(Addr->getZExtValue())
                                             : -1;
}

SymPath finish(Frontier &&F, PathOutcome Outcome, SymRef Target = SymRef()) {
  SymPath Done{Outcome,
               F.BlockId,
               std::move(F.Constraints),
               std::move(F.Blocks),
               std::move(F.State),
               Target,
               F.UnmodelledOps};
  return Done;
}

} // namespace

SymExploration explorePathsDetailed(SymContext &Ctx, const LowFunc &Func,
                                    const ExploreOptions &Opts) {
  std::vector<SymPath> Finished;
  BlockIndex Index(Func);
  if (Index.entry() < 0)
    return SymExploration{std::move(Finished), true, 0, 0, 0};

  llvm::SmallVector<Frontier, 8> Pending;
  Pending.push_back(Frontier(SymState(Ctx, Opts.ByteOrder), Index.entry()));

  unsigned ReachablePaths = 0;
  size_t ExecutedSteps = 0;
  unsigned UnmodelledOps = 0;
  bool HitIncompleteOutcome = false;
  auto Record = [&](Frontier &&F, PathOutcome Outcome,
                    SymRef Target = SymRef()) {
    ReachablePaths += Outcome != PathOutcome::Infeasible;
    HitIncompleteOutcome |= Outcome == PathOutcome::UnresolvedBranch ||
                            Outcome == PathOutcome::LoopBudget ||
                            Outcome == PathOutcome::StepBudget;
    Finished.push_back(finish(std::move(F), Outcome, Target));
  };

  while (!Pending.empty() && ReachablePaths < Opts.MaxPaths) {
    Frontier Current = std::move(Pending.back());
    Pending.pop_back();
    if (Current.Infeasible) {
      Record(std::move(Current), PathOutcome::Infeasible);
      continue;
    }

    // Follow this path through as many blocks as it takes, forking only where
    // there is a genuine choice.
    for (;;) {
      const LowBlock *Block = Index.block(Current.BlockId);
      if (!Block) {
        Record(std::move(Current), PathOutcome::LeftFunction);
        break;
      }
      if (++Current.Visits[Current.BlockId] > Opts.MaxBlockVisits) {
        Record(std::move(Current), PathOutcome::LoopBudget);
        break;
      }
      Current.Blocks.push_back(Current.BlockId);

      SymExec Exec(Ctx, Current.State);
      Exec.setCallPreservedRegisters(Opts.CallPreservedRegisters);
      StepResult Result = StepResult::Continue;
      bool Budget = false;
      for (const LowOp &Op : Block->Ops) {
        if (++Current.Steps > Opts.MaxSteps) {
          Budget = true;
          break;
        }
        ++ExecutedSteps;
        Result = Exec.step(Op);
        // An operation the engine declined to model still wrote a named
        // unknown to its destination, so the path carries on around it.
        if (Result != StepResult::Continue && Result != StepResult::Unmodelled)
          break;
      }
      Current.UnmodelledOps += Exec.unmodelledCount();
      UnmodelledOps += Exec.unmodelledCount();
      if (Budget) {
        Record(std::move(Current), PathOutcome::StepBudget);
        break;
      }

      if (Result == StepResult::Return) {
        Record(std::move(Current), PathOutcome::Returned);
        break;
      }

      if (Result == StepResult::Continue || Result == StepResult::Unmodelled) {
        // Ran off the end without a terminator, which the lifters produce for
        // a block that simply falls into the next one.
        int Next = Block->Succs.size() == 1 ? Block->Succs.front() : -1;
        if (Next < 0) {
          Record(std::move(Current), PathOutcome::LeftFunction);
          break;
        }
        Current.BlockId = Next;
        continue;
      }

      if (Result == StepResult::Branch ||
          Result == StepResult::IndirectBranch) {
        int Next = resolve(Ctx, Index, Exec.branchTarget());
        if (Next < 0) {
          Record(std::move(Current),
                 Result == StepResult::Branch ? PathOutcome::LeftFunction
                                              : PathOutcome::UnresolvedBranch,
                 Exec.branchTarget());
          break;
        }
        Current.BlockId = Next;
        continue;
      }

      // A conditional branch.  Where the predicate already came out constant
      // the code has decided, and only one side exists to walk — which is what
      // silently undoes an opaque predicate, no rule required.
      SymRef Condition = Exec.branchCondition();
      int Taken = resolve(Ctx, Index, Exec.branchTarget());
      int NotTaken = Index.fallthroughOf(*Block, Taken);

      if (std::optional<llvm::APInt> Decided = Ctx.asConst(Condition)) {
        int Next = Decided->isZero() ? NotTaken : Taken;
        if (Next < 0) {
          Record(std::move(Current), PathOutcome::LeftFunction,
                 Exec.branchTarget());
          break;
        }
        Current.BlockId = Next;
        continue;
      }

      // Both sides are live.  The fallthrough is pushed first so the taken
      // side is walked first, which puts the deeper, more interesting path at
      // the front of the results.
      if (NotTaken >= 0) {
        Frontier Fork = Current;
        if (addConstraint(Ctx, Fork, Ctx.mkNot(Condition)))
          Fork.BlockId = NotTaken;
        else
          Fork.Infeasible = true;
        Pending.push_back(std::move(Fork));
      }
      if (!addConstraint(Ctx, Current, Condition)) {
        Record(std::move(Current), PathOutcome::Infeasible);
        break;
      }
      if (Taken < 0) {
        Record(std::move(Current), PathOutcome::LeftFunction,
               Exec.branchTarget());
        break;
      }
      Current.BlockId = Taken;
    }
  }

  const bool HasReachableFrontier =
      llvm::any_of(Pending, [](const Frontier &F) { return !F.Infeasible; });
  return SymExploration{std::move(Finished),
                        !HitIncompleteOutcome && !HasReachableFrontier,
                        ReachablePaths, ExecutedSteps, UnmodelledOps};
}

std::vector<SymPath> explorePaths(SymContext &Ctx, const LowFunc &Func,
                                  const ExploreOptions &Opts) {
  return explorePathsDetailed(Ctx, Func, Opts).Paths;
}

} // namespace neverd::symbolic
