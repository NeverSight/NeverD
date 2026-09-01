//===- LowIRConcolic.h - Verified LowIR branch flips -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runs one concrete LowIR trace, negates each exact conditional-decision
/// prefix, and publishes only entry-register seeds whose fresh replay reaches
/// the same occurrence with the opposite polarity.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_CONCOLIC_LOWIRCONCOLIC_H
#define NEVERD_CONCOLIC_LOWIRCONCOLIC_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExplore.h"

#include "llvm/Support/Endian.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::concolic {

enum class ConcolicTraceReason : uint8_t {
  None,
  InvalidInitialSeed,
  NoTrace,
  AmbiguousTrace,
  IncompleteConcreteTrace,
  IncompleteLift,
  UnsupportedEffects,
  IncompleteOutcome,
  InvalidDecisionHistory,
};

enum class ConcolicProjectionReason : uint8_t {
  None,
  InvalidQuery,
  FreshVariable,
  MissingInputOrigin,
  UnsupportedInputKind,
  NonzeroInputEpoch,
  InvalidInputWidth,
  MissingModelValue,
  OverlappingInputOrigins,
  MissingBaselineByte,
  CandidateDoesNotSatisfyQuery,
};

enum class ConcolicReplayReason : uint8_t {
  None,
  NoReplayTrace,
  EarlierDecisionMissing,
  EarlierOccurrenceMismatch,
  EarlierPolarityMismatch,
  EarlierConstraintPrefixMismatch,
  EarlierDecisionNotConcrete,
  TargetDecisionMissing,
  TargetOccurrenceMismatch,
  TargetDecisionNotConcrete,
  TargetConstraintPrefixMismatch,
  TargetPolarityNotFlipped,
};

enum class ConcolicFlipStatus : uint8_t {
  Verified,
  Unsat,
  SolverUnknown,
  InvalidQuery,
  ProjectionRejected,
  ReplayRejected,
  VerifiedDuplicate,
  AttemptBudgetExceeded,
  CandidateBudgetExceeded,
};

const char *concolicTraceReasonName(ConcolicTraceReason Reason);
const char *concolicProjectionReasonName(ConcolicProjectionReason Reason);
const char *concolicReplayReasonName(ConcolicReplayReason Reason);
const char *concolicFlipStatusName(ConcolicFlipStatus Status);

inline solver::SolverOptions defaultLowIRConcolicSolverOptions() {
  solver::SolverOptions Options;
  Options.Sat.MaxConflicts = uint64_t{1} << 18;
  Options.Sat.MaxPropagations = uint64_t{1} << 24;
  Options.Sat.MaxWatchVisits = uint64_t{1} << 26;
  return Options;
}

struct LowIRConcolicOptions {
  std::vector<symbolic::SymConcreteRegister> InitialSeed;
  llvm::endianness ByteOrder = llvm::endianness::little;

  unsigned MaxSteps = 1u << 16;
  unsigned MaxBlockVisits = 3;
  unsigned MaxLoopIterations = 3;

  /// Number of solver checks. Zero deliberately attempts no flips.
  unsigned MaxFlipAttempts = 64;
  /// Number of distinct replay-verified seeds retained in the report.
  unsigned MaxCandidates = 64;
  solver::SolverOptions Solver = defaultLowIRConcolicSolverOptions();
};

/// Context-free copy of one decision from the original trace.
struct LowIRConcolicDecision {
  size_t Index = 0;
  symbolic::SymDecisionOccurrence Occurrence;
  bool Taken = false;
  size_t ConstraintPrefix = 0;
  bool Concrete = false;
};

struct LowIRConcolicCandidate {
  /// Sorted, non-overlapping ranges with the same shape as InitialSeed.
  std::vector<symbolic::SymConcreteRegister> Seed;
};

struct LowIRConcolicFlip {
  size_t DecisionIndex = 0;
  symbolic::SymDecisionOccurrence Occurrence;
  bool OriginalTaken = false;
  size_t ConstraintPrefix = 0;

  ConcolicFlipStatus Status = ConcolicFlipStatus::InvalidQuery;
  /// Absent when the attempt budget prevented a solver call.
  std::optional<solver::SatResult> SolverResult;
  solver::BlastError EncodingError = solver::BlastError::None;
  ConcolicProjectionReason ProjectionReason = ConcolicProjectionReason::None;
  ConcolicReplayReason ReplayReason = ConcolicReplayReason::None;
  std::optional<size_t> CandidateIndex;
};

/// An owned report. It contains neither SymRef values nor pointers into Func.
struct LowIRConcolicReport {
  uint32_t Version = 1;
  va_t FunctionEntry = 0;
  std::string FunctionName;
  std::vector<symbolic::SymConcreteRegister> InitialSeed;

  std::optional<symbolic::PathOutcome> TraceOutcome;
  bool LiftComplete = false;
  bool TraceComplete = false;
  bool TraceExact = false;
  /// A concolic run follows one trace and is never exhaustive.
  bool Exhaustive = false;
  ConcolicTraceReason TraceReason = ConcolicTraceReason::None;
  size_t ExecutedSteps = 0;
  unsigned UnmodelledOps = 0;
  unsigned OpaqueOps = 0;
  unsigned CallHavocs = 0;
  unsigned MemoryHavocs = 0;
  /// Block IDs entered by the original trace, in execution order.
  std::vector<int> Blocks;

  std::vector<LowIRConcolicDecision> Decisions;
  std::vector<LowIRConcolicFlip> Flips;
  std::vector<LowIRConcolicCandidate> Candidates;

  unsigned FlipAttempts = 0;
  bool FlipBudgetHit = false;
  bool CandidateBudgetHit = false;
};

LowIRConcolicReport
runLowIRConcolic(const LowFunc &Func,
                 const LowIRConcolicOptions &Options = LowIRConcolicOptions());

} // namespace neverd::concolic

#endif // NEVERD_CONCOLIC_LOWIRCONCOLIC_H
