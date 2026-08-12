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

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <utility>

namespace neverd::symbolic {

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
};

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
  return Addr ? Index.at(Addr->getZExtValue()) : -1;
}

SymPath finish(Frontier &&F, PathOutcome Outcome, SymRef Target = SymRef()) {
  SymPath Done{Outcome,
               F.BlockId,
               std::move(F.Constraints),
               std::move(F.Blocks),
               std::move(F.State),
               Target};
  return Done;
}

} // namespace

std::vector<SymPath> explorePaths(SymContext &Ctx, const LowFunc &Func,
                                  const ExploreOptions &Opts) {
  std::vector<SymPath> Finished;
  BlockIndex Index(Func);
  if (Index.entry() < 0)
    return Finished;

  llvm::SmallVector<Frontier, 8> Pending;
  Pending.push_back(Frontier(SymState(Ctx), Index.entry()));

  unsigned Steps = 0;
  while (!Pending.empty() && Finished.size() < Opts.MaxPaths) {
    Frontier Current = std::move(Pending.back());
    Pending.pop_back();

    // Follow this path through as many blocks as it takes, forking only where
    // there is a genuine choice.
    for (;;) {
      const LowBlock *Block = Index.block(Current.BlockId);
      if (!Block) {
        Finished.push_back(finish(std::move(Current), PathOutcome::LeftFunction));
        break;
      }
      if (++Current.Visits[Current.BlockId] > Opts.MaxBlockVisits) {
        Finished.push_back(finish(std::move(Current), PathOutcome::LoopBudget));
        break;
      }
      Current.Blocks.push_back(Current.BlockId);

      SymExec Exec(Ctx, Current.State);
      StepResult Result = StepResult::Continue;
      bool Budget = false;
      for (const LowOp &Op : Block->Ops) {
        if (++Steps > Opts.MaxSteps) {
          Budget = true;
          break;
        }
        Result = Exec.step(Op);
        // An operation the engine declined to model still wrote a named
        // unknown to its destination, so the path carries on around it.
        if (Result != StepResult::Continue && Result != StepResult::Unmodelled)
          break;
      }
      if (Budget) {
        Finished.push_back(finish(std::move(Current), PathOutcome::StepBudget));
        break;
      }

      if (Result == StepResult::Return) {
        Finished.push_back(finish(std::move(Current), PathOutcome::Returned));
        break;
      }

      if (Result == StepResult::Continue || Result == StepResult::Unmodelled) {
        // Ran off the end without a terminator, which the lifters produce for
        // a block that simply falls into the next one.
        int Next = Block->Succs.size() == 1 ? Block->Succs.front() : -1;
        if (Next < 0) {
          Finished.push_back(
              finish(std::move(Current), PathOutcome::LeftFunction));
          break;
        }
        Current.BlockId = Next;
        continue;
      }

      if (Result == StepResult::Branch ||
          Result == StepResult::IndirectBranch) {
        int Next = resolve(Ctx, Index, Exec.branchTarget());
        if (Next < 0) {
          Finished.push_back(finish(std::move(Current),
                                    Result == StepResult::Branch
                                        ? PathOutcome::LeftFunction
                                        : PathOutcome::UnresolvedBranch,
                                    Exec.branchTarget()));
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
          Finished.push_back(finish(std::move(Current),
                                    PathOutcome::LeftFunction,
                                    Exec.branchTarget()));
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
        Fork.Constraints.push_back(Ctx.mkNot(Condition));
        Fork.BlockId = NotTaken;
        Pending.push_back(std::move(Fork));
      }
      if (Taken < 0) {
        Current.Constraints.push_back(Condition);
        Finished.push_back(finish(std::move(Current),
                                  PathOutcome::LeftFunction,
                                  Exec.branchTarget()));
        break;
      }
      Current.Constraints.push_back(Condition);
      Current.BlockId = Taken;
    }
  }

  return Finished;
}

} // namespace neverd::symbolic
