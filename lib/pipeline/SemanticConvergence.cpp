//===- SemanticConvergence.cpp - Observable semantic fixed point ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SemanticConvergence.h"

#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace neverd {

namespace {

void addSaturating(uint64_t &Total, uint64_t Delta) {
  constexpr uint64_t Max = std::numeric_limits<uint64_t>::max();
  Total = Delta > Max - Total ? Max : Total + Delta;
}

void addProofStats(solver::ProofStats &Total, const solver::ProofStats &Delta) {
  addSaturating(Total.Queries, Delta.Queries);
  addSaturating(Total.Conflicts, Delta.Conflicts);
  addSaturating(Total.Propagations, Delta.Propagations);
  addSaturating(Total.WatchVisits, Delta.WatchVisits);
}

unsigned stopSeverity(OptimizationStopReason Stop) {
  switch (Stop) {
  case OptimizationStopReason::Stable:
    return 0;
  case OptimizationStopReason::CycleDetected:
    return 1;
  case OptimizationStopReason::BudgetExhausted:
    return 2;
  case OptimizationStopReason::VerificationFailed:
    return 3;
  case OptimizationStopReason::InputInvalid:
    return 4;
  }
  llvm_unreachable("unhandled optimization stop reason");
}

void addRound(FunctionOptimizationResult &Result,
              const ConvergenceRound &Round) {
  Result.Changed |= Round.Changed;
  addSaturating(Result.SemanticRewrites, Round.Semantic.Rewrites);
  addSaturating(Result.SearchWork, Round.Semantic.SearchWork);
  addProofStats(Result.ProofWork, Round.Semantic.ProofWork);
  if (Result.Rounds != std::numeric_limits<unsigned>::max())
    ++Result.Rounds;
}

} // namespace

const char *optimizationStopReasonName(OptimizationStopReason Stop) {
  switch (Stop) {
  case OptimizationStopReason::Stable:
    return "stable";
  case OptimizationStopReason::CycleDetected:
    return "cycle-detected";
  case OptimizationStopReason::BudgetExhausted:
    return "budget-exhausted";
  case OptimizationStopReason::VerificationFailed:
    return "verification-failed";
  case OptimizationStopReason::InputInvalid:
    return "input-invalid";
  }
  llvm_unreachable("unhandled optimization stop reason");
}

FunctionOptimizationResult driveSemanticConvergence(unsigned MaxRounds,
                                                    RunRoundFn RunRound,
                                                    SnapshotFn Snapshot) {
  FunctionOptimizationResult Result;

  bool HaveCheckpoint = false;
  uint64_t CheckpointHash = 0;
  std::string CheckpointSnapshot;
  uint64_t CheckpointSpan = 2;
  uint64_t Distance = 0;

  for (;;) {
    ConvergenceRound Round = RunRound();
    addRound(Result, Round);

    // ProofIncomplete/Unknown is a fail-closed rejection of one candidate, not
    // a failed module verification.  This driver owns convergence stops only;
    // transactional verification may produce VerificationFailed separately.
    if (!Round.Changed) {
      Result.Stop = OptimizationStopReason::Stable;
      return Result;
    }

    // Reaching stability on the last permitted call is success.  A productive
    // last call has more work to expose, so the finite caller-owned budget is
    // the most severe stop observable on this round and outranks a cycle.
    if (MaxRounds != 0 && Result.Rounds >= MaxRounds) {
      Result.Stop = OptimizationStopReason::BudgetExhausted;
      return Result;
    }

    if (!HaveCheckpoint) {
      CheckpointHash = Round.StructuralHash;
      CheckpointSnapshot = Snapshot();
      HaveCheckpoint = true;
      continue;
    }

    if (Distance != std::numeric_limits<uint64_t>::max())
      ++Distance;

    std::optional<std::string> CurrentSnapshot;
    if (Round.StructuralHash == CheckpointHash) {
      CurrentSnapshot = Snapshot();
      if (*CurrentSnapshot == CheckpointSnapshot) {
        Result.Stop = OptimizationStopReason::CycleDetected;
        return Result;
      }
    }

    // Exponentially spaced checkpoints are Brent's O(1)-storage tradeoff: a
    // finite cycle is eventually observed over an interval at least as long as
    // its period.  The first checkpoint covers the next two observations, so
    // output states A -> B -> A are detected at zero-based index two.
    if (Distance >= CheckpointSpan) {
      if (!CurrentSnapshot)
        CurrentSnapshot = Snapshot();
      CheckpointHash = Round.StructuralHash;
      CheckpointSnapshot = std::move(*CurrentSnapshot);
      Distance = 0;
      if (CheckpointSpan <= std::numeric_limits<uint64_t>::max() / 2)
        CheckpointSpan *= 2;
      else
        CheckpointSpan = std::numeric_limits<uint64_t>::max();
    }
  }
}

void mergeFunctionOptimizationResult(OptimizationResult &Module,
                                     const FunctionOptimizationResult &Func) {
  Module.Changed |= Func.Changed;
  addSaturating(Module.SemanticRewrites, Func.SemanticRewrites);
  addSaturating(Module.SearchWork, Func.SearchWork);
  addProofStats(Module.ProofWork, Func.ProofWork);
  Module.Rounds = std::max(Module.Rounds, Func.Rounds);
  if (stopSeverity(Func.Stop) > stopSeverity(Module.Stop))
    Module.Stop = Func.Stop;
  addSaturating(Module.FunctionsVisited, 1);
}

} // namespace neverd
