//===- LowIRConcolic.cpp - Verified LowIR branch flips --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/concolic/LowIRConcolic.h"

#include "LowIRConcolicDetail.h"

#include "neverd/symbolic/SymConcrete.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace neverd::concolic {

using namespace symbolic;

const char *concolicTraceReasonName(ConcolicTraceReason Reason) {
  switch (Reason) {
  case ConcolicTraceReason::None:
    return "none";
  case ConcolicTraceReason::InvalidInitialSeed:
    return "invalid_initial_seed";
  case ConcolicTraceReason::NoTrace:
    return "no_trace";
  case ConcolicTraceReason::AmbiguousTrace:
    return "ambiguous_trace";
  case ConcolicTraceReason::IncompleteConcreteTrace:
    return "incomplete_concrete_trace";
  case ConcolicTraceReason::IncompleteLift:
    return "incomplete_lift";
  case ConcolicTraceReason::UnsupportedEffects:
    return "unsupported_effects";
  case ConcolicTraceReason::IncompleteOutcome:
    return "incomplete_outcome";
  case ConcolicTraceReason::InvalidDecisionHistory:
    return "invalid_decision_history";
  }
  return "?";
}

const char *concolicProjectionReasonName(ConcolicProjectionReason Reason) {
  switch (Reason) {
  case ConcolicProjectionReason::None:
    return "none";
  case ConcolicProjectionReason::InvalidQuery:
    return "invalid_query";
  case ConcolicProjectionReason::FreshVariable:
    return "fresh_variable";
  case ConcolicProjectionReason::MissingInputOrigin:
    return "missing_input_origin";
  case ConcolicProjectionReason::UnsupportedInputKind:
    return "unsupported_input_kind";
  case ConcolicProjectionReason::NonzeroInputEpoch:
    return "nonzero_input_epoch";
  case ConcolicProjectionReason::InvalidInputWidth:
    return "invalid_input_width";
  case ConcolicProjectionReason::MissingModelValue:
    return "missing_model_value";
  case ConcolicProjectionReason::OverlappingInputOrigins:
    return "overlapping_input_origins";
  case ConcolicProjectionReason::MissingBaselineByte:
    return "missing_baseline_byte";
  case ConcolicProjectionReason::CandidateDoesNotSatisfyQuery:
    return "candidate_does_not_satisfy_query";
  }
  return "?";
}

const char *concolicReplayReasonName(ConcolicReplayReason Reason) {
  switch (Reason) {
  case ConcolicReplayReason::None:
    return "none";
  case ConcolicReplayReason::NoReplayTrace:
    return "no_replay_trace";
  case ConcolicReplayReason::EarlierDecisionMissing:
    return "earlier_decision_missing";
  case ConcolicReplayReason::EarlierOccurrenceMismatch:
    return "earlier_occurrence_mismatch";
  case ConcolicReplayReason::EarlierPolarityMismatch:
    return "earlier_polarity_mismatch";
  case ConcolicReplayReason::EarlierConstraintPrefixMismatch:
    return "earlier_constraint_prefix_mismatch";
  case ConcolicReplayReason::EarlierDecisionNotConcrete:
    return "earlier_decision_not_concrete";
  case ConcolicReplayReason::TargetDecisionMissing:
    return "target_decision_missing";
  case ConcolicReplayReason::TargetOccurrenceMismatch:
    return "target_occurrence_mismatch";
  case ConcolicReplayReason::TargetDecisionNotConcrete:
    return "target_decision_not_concrete";
  case ConcolicReplayReason::TargetConstraintPrefixMismatch:
    return "target_constraint_prefix_mismatch";
  case ConcolicReplayReason::TargetPolarityNotFlipped:
    return "target_polarity_not_flipped";
  }
  return "?";
}

const char *concolicFlipStatusName(ConcolicFlipStatus Status) {
  switch (Status) {
  case ConcolicFlipStatus::Verified:
    return "verified";
  case ConcolicFlipStatus::Unsat:
    return "unsat";
  case ConcolicFlipStatus::SolverUnknown:
    return "solver_unknown";
  case ConcolicFlipStatus::InvalidQuery:
    return "invalid_query";
  case ConcolicFlipStatus::ProjectionRejected:
    return "projection_rejected";
  case ConcolicFlipStatus::ReplayRejected:
    return "replay_rejected";
  case ConcolicFlipStatus::VerifiedDuplicate:
    return "verified_duplicate";
  case ConcolicFlipStatus::AttemptBudgetExceeded:
    return "attempt_budget_exceeded";
  case ConcolicFlipStatus::CandidateBudgetExceeded:
    return "candidate_budget_exceeded";
  }
  return "?";
}

namespace {

using ByteMap = std::map<uint64_t, uint8_t>;

struct OwnedTrace {
  std::unique_ptr<SymContext> Context;
  SymExploration Exploration;
};

uint64_t widthMask(uint16_t Bytes) {
  return Bytes == 8 ? std::numeric_limits<uint64_t>::max()
                    : (uint64_t{1} << (unsigned(Bytes) * 8)) - 1;
}

bool validRange(uint64_t Offset, uint16_t Bytes) {
  return Bytes > 0 && Bytes <= 8 &&
         Offset <= std::numeric_limits<uint64_t>::max() - (Bytes - 1);
}

void unpack(uint64_t Value, uint16_t Bytes, llvm::endianness Order,
            uint64_t Offset, ByteMap &Out) {
  for (uint16_t I = 0; I < Bytes; ++I) {
    const unsigned Shift = Order == llvm::endianness::little
                               ? unsigned(I) * 8
                               : unsigned(Bytes - 1 - I) * 8;
    Out[Offset + I] = static_cast<uint8_t>(Value >> Shift);
  }
}

std::optional<uint64_t> pack(const ByteMap &Bytes, uint64_t Offset,
                             uint16_t Count, llvm::endianness Order) {
  if (!validRange(Offset, Count))
    return std::nullopt;
  uint64_t Value = 0;
  for (uint16_t I = 0; I < Count; ++I) {
    auto It = Bytes.find(Offset + I);
    if (It == Bytes.end())
      return std::nullopt;
    const unsigned Shift = Order == llvm::endianness::little
                               ? unsigned(I) * 8
                               : unsigned(Count - 1 - I) * 8;
    Value |= uint64_t(It->second) << Shift;
  }
  return Value;
}

std::optional<std::vector<SymConcreteRegister>>
normalizeSeed(llvm::ArrayRef<SymConcreteRegister> Seed,
              llvm::endianness Order) {
  std::vector<SymConcreteRegister> Normalized(Seed.begin(), Seed.end());
  std::sort(
      Normalized.begin(), Normalized.end(),
      [](const SymConcreteRegister &Left, const SymConcreteRegister &Right) {
        if (Left.Offset != Right.Offset)
          return Left.Offset < Right.Offset;
        if (Left.Bytes != Right.Bytes)
          return Left.Bytes < Right.Bytes;
        return Left.Value < Right.Value;
      });

  ByteMap Bytes;
  for (SymConcreteRegister &Range : Normalized) {
    if (!validRange(Range.Offset, Range.Bytes))
      return std::nullopt;
    if ((Range.Value & ~widthMask(Range.Bytes)) != 0)
      return std::nullopt;
    for (uint16_t I = 0; I < Range.Bytes; ++I)
      if (Bytes.count(Range.Offset + I) != 0)
        return std::nullopt;
    unpack(Range.Value, Range.Bytes, Order, Range.Offset, Bytes);
  }
  return Normalized;
}

ExploreOptions traceOptions(const LowIRConcolicOptions &Options,
                            ConcreteShadow &Shadow,
                            llvm::ArrayRef<SymConcreteRegister> Seed) {
  ExploreOptions Result;
  Result.MaxPaths = 1;
  Result.MaxSteps = Options.MaxSteps;
  Result.MaxBlockVisits = Options.MaxBlockVisits;
  Result.MaxLoopIterations = Options.MaxLoopIterations;
  Result.ByteOrder = Options.ByteOrder;
  Result.Concolic = &Shadow;
  Result.ConcolicSeed.assign(Seed.begin(), Seed.end());
  return Result;
}

OwnedTrace runTrace(const LowFunc &Func, const LowIRConcolicOptions &Options,
                    llvm::ArrayRef<SymConcreteRegister> Seed) {
  OwnedTrace Result;
  Result.Context = std::make_unique<SymContext>();
  SymExecConcreteShadow Shadow;
  ExploreOptions Explore = traceOptions(Options, Shadow, Seed);
  Result.Exploration = explorePathsDetailed(*Result.Context, Func, Explore);
  return Result;
}

bool isTerminalOutcome(PathOutcome Outcome) {
  return Outcome == PathOutcome::Returned ||
         Outcome == PathOutcome::LeftFunction;
}

SymRef selectedConstraint(SymContext &Ctx, const SymBranchDecision &Decision) {
  return Decision.Taken ? Decision.Condition : Ctx.mkNot(Decision.Condition);
}

bool validDecisionHistory(SymContext &Ctx, const SymPath &Path) {
  size_t PreviousPrefix = 0;
  bool First = true;
  for (const SymBranchDecision &Decision : Path.BranchDecisions) {
    if (!Decision.Condition.isValid() || !Decision.Concrete ||
        Decision.ConstraintPrefix >= Path.Constraints.size() ||
        (!First && Decision.ConstraintPrefix <= PreviousPrefix))
      return false;
    if (Path.Constraints[Decision.ConstraintPrefix] !=
        selectedConstraint(Ctx, Decision))
      return false;
    if (Decision.Occurrence.Kind == SymDecisionKind::IndirectBranchTarget &&
        !Decision.Taken)
      return false;
    PreviousPrefix = Decision.ConstraintPrefix;
    First = false;
  }
  return true;
}

SymRef buildQuery(SymContext &Ctx, const SymPath &Path,
                  const SymBranchDecision &Decision) {
  llvm::SmallVector<SymRef, 8> Terms;
  for (size_t I = 0; I < Decision.ConstraintPrefix; ++I)
    Terms.push_back(Path.Constraints[I]);
  Terms.push_back(Decision.Taken ? Ctx.mkNot(Decision.Condition)
                                 : Decision.Condition);
  return Terms.size() == 1 ? Terms.front() : Ctx.mkAnd(Terms);
}

} // namespace

detail::ModelProjection detail::projectRegisterModel(
    const SymContext &Ctx, SymRef Query, const solver::BitVectorModel &Model,
    llvm::ArrayRef<SymConcreteRegister> Baseline, llvm::endianness Order) {
  ModelProjection Result;
  if (!Query.isValid() || Ctx.width(Query) != 1) {
    Result.Reason = ConcolicProjectionReason::InvalidQuery;
    return Result;
  }

  ByteMap CandidateBytes;
  for (const SymConcreteRegister &Range : Baseline)
    unpack(Range.Value, Range.Bytes, Order, Range.Offset, CandidateBytes);

  llvm::SmallVector<uint32_t, 8> Vars;
  Ctx.collectVars(Query, Vars);
  ByteMap ProjectedOwners;
  for (uint32_t VarId : Vars) {
    const SymVarInfo &Info = Ctx.varInfo(VarId);
    if (Info.Fresh) {
      Result.Reason = ConcolicProjectionReason::FreshVariable;
      return Result;
    }
    if (!Info.InputOrigin) {
      Result.Reason = ConcolicProjectionReason::MissingInputOrigin;
      return Result;
    }
    const SymInputOrigin &Origin = *Info.InputOrigin;
    if (Origin.Kind != SymInputKind::Register) {
      Result.Reason = ConcolicProjectionReason::UnsupportedInputKind;
      return Result;
    }
    if (Origin.Epoch != 0) {
      Result.Reason = ConcolicProjectionReason::NonzeroInputEpoch;
      return Result;
    }
    if (!validRange(Origin.Offset, Origin.Bytes) ||
        Info.Width != uint32_t(Origin.Bytes) * 8) {
      Result.Reason = ConcolicProjectionReason::InvalidInputWidth;
      return Result;
    }
    std::optional<llvm::APInt> Value = Model.value(VarId);
    if (!Value) {
      Result.Reason = ConcolicProjectionReason::MissingModelValue;
      return Result;
    }
    for (uint16_t I = 0; I < Origin.Bytes; ++I) {
      const uint64_t Byte = Origin.Offset + I;
      if (ProjectedOwners.count(Byte) != 0) {
        Result.Reason = ConcolicProjectionReason::OverlappingInputOrigins;
        return Result;
      }
      ProjectedOwners[Byte] = 0;
      if (CandidateBytes.count(Byte) == 0) {
        Result.Reason = ConcolicProjectionReason::MissingBaselineByte;
        return Result;
      }
    }
    unpack(Value->getZExtValue(), Origin.Bytes, Order, Origin.Offset,
           CandidateBytes);
  }

  Result.Seed.reserve(Baseline.size());
  for (const SymConcreteRegister &Range : Baseline) {
    std::optional<uint64_t> Value =
        pack(CandidateBytes, Range.Offset, Range.Bytes, Order);
    if (!Value) {
      Result.Reason = ConcolicProjectionReason::MissingBaselineByte;
      Result.Seed.clear();
      return Result;
    }
    Result.Seed.push_back({Range.Offset, Range.Bytes, *Value});
  }

  // Reconstruct every query variable from the candidate bytes. Do not use
  // BitVectorModel::asVarValues here: replay verifies the projected seed, not
  // the solver's unprojected assignment.
  std::vector<llvm::APInt> VarValues;
  VarValues.reserve(Ctx.numVars());
  for (uint32_t Id = 0; Id < Ctx.numVars(); ++Id)
    VarValues.emplace_back(Ctx.varInfo(Id).Width, 0);
  for (uint32_t VarId : Vars) {
    const SymInputOrigin &Origin = *Ctx.varInfo(VarId).InputOrigin;
    std::optional<uint64_t> Value =
        pack(CandidateBytes, Origin.Offset, Origin.Bytes, Order);
    if (!Value) {
      Result.Reason = ConcolicProjectionReason::MissingBaselineByte;
      Result.Seed.clear();
      return Result;
    }
    VarValues[VarId] = llvm::APInt(Ctx.varInfo(VarId).Width, *Value);
  }
  if (Ctx.eval(Query, VarValues).isZero()) {
    Result.Reason = ConcolicProjectionReason::CandidateDoesNotSatisfyQuery;
    Result.Seed.clear();
  }
  return Result;
}

ConcolicReplayReason
detail::compareReplayDecisions(llvm::ArrayRef<LowIRConcolicDecision> Replay,
                               llvm::ArrayRef<LowIRConcolicDecision> Original,
                               size_t TargetIndex) {
  for (size_t I = 0; I < TargetIndex; ++I) {
    if (I >= Replay.size())
      return ConcolicReplayReason::EarlierDecisionMissing;
    const LowIRConcolicDecision &Actual = Replay[I];
    const LowIRConcolicDecision &Expected = Original[I];
    if (!Actual.Concrete)
      return ConcolicReplayReason::EarlierDecisionNotConcrete;
    if (Actual.Occurrence != Expected.Occurrence)
      return ConcolicReplayReason::EarlierOccurrenceMismatch;
    if (Actual.Taken != Expected.Taken)
      return ConcolicReplayReason::EarlierPolarityMismatch;
    if (Actual.ConstraintPrefix != Expected.ConstraintPrefix)
      return ConcolicReplayReason::EarlierConstraintPrefixMismatch;
  }

  if (TargetIndex >= Replay.size())
    return ConcolicReplayReason::TargetDecisionMissing;
  const LowIRConcolicDecision &Actual = Replay[TargetIndex];
  const LowIRConcolicDecision &Expected = Original[TargetIndex];
  if (!Actual.Concrete)
    return ConcolicReplayReason::TargetDecisionNotConcrete;
  if (Actual.Occurrence != Expected.Occurrence)
    return ConcolicReplayReason::TargetOccurrenceMismatch;
  if (Actual.ConstraintPrefix != Expected.ConstraintPrefix)
    return ConcolicReplayReason::TargetConstraintPrefixMismatch;
  if (Actual.Taken == Expected.Taken)
    return ConcolicReplayReason::TargetPolarityNotFlipped;
  return ConcolicReplayReason::None;
}

namespace {

ConcolicReplayReason
verifyReplay(const LowFunc &Func, const LowIRConcolicOptions &Options,
             llvm::ArrayRef<LowIRConcolicDecision> Original, size_t TargetIndex,
             llvm::ArrayRef<SymConcreteRegister> Seed) {
  OwnedTrace Replay = runTrace(Func, Options, Seed);
  if (Replay.Exploration.Paths.empty())
    return ConcolicReplayReason::NoReplayTrace;

  ConcolicReplayReason Best = ConcolicReplayReason::NoReplayTrace;
  for (const SymPath &Path : Replay.Exploration.Paths) {
    std::vector<LowIRConcolicDecision> Decisions;
    Decisions.reserve(Path.BranchDecisions.size());
    for (size_t I = 0; I < Path.BranchDecisions.size(); ++I) {
      const SymBranchDecision &Decision = Path.BranchDecisions[I];
      Decisions.push_back({I, Decision.Occurrence, Decision.Taken,
                           Decision.ConstraintPrefix, Decision.Concrete});
    }
    const ConcolicReplayReason Reason =
        detail::compareReplayDecisions(Decisions, Original, TargetIndex);
    if (Reason == ConcolicReplayReason::None)
      return Reason;
    if (Best == ConcolicReplayReason::NoReplayTrace)
      Best = Reason;
  }
  return Best;
}

bool equalSeed(llvm::ArrayRef<SymConcreteRegister> Left,
               llvm::ArrayRef<SymConcreteRegister> Right) {
  if (Left.size() != Right.size())
    return false;
  for (size_t I = 0; I < Left.size(); ++I)
    if (Left[I].Offset != Right[I].Offset || Left[I].Bytes != Right[I].Bytes ||
        Left[I].Value != Right[I].Value)
      return false;
  return true;
}

} // namespace

detail::CandidatePublication detail::publishReplayVerifiedSeed(
    std::vector<LowIRConcolicCandidate> &Published,
    std::vector<SymConcreteRegister> Seed, unsigned MaxCandidates) {
  auto Existing = std::find_if(Published.begin(), Published.end(),
                               [&](const LowIRConcolicCandidate &Candidate) {
                                 return equalSeed(Candidate.Seed, Seed);
                               });
  if (Existing != Published.end())
    return {ConcolicFlipStatus::VerifiedDuplicate,
            static_cast<size_t>(Existing - Published.begin())};
  if (Published.size() >= MaxCandidates)
    return {ConcolicFlipStatus::CandidateBudgetExceeded, std::nullopt};

  const size_t Index = Published.size();
  Published.push_back({std::move(Seed)});
  return {ConcolicFlipStatus::Verified, Index};
}

ConcolicFlipStatus
detail::classifyPrefixEncodingFailure(solver::BlastError Error) {
  switch (Error) {
  case solver::BlastError::WidthTooLarge:
  case solver::BlastError::TooManyGates:
    return ConcolicFlipStatus::SolverUnknown;
  case solver::BlastError::None:
  case solver::BlastError::Malformed:
    return ConcolicFlipStatus::InvalidQuery;
  }
  return ConcolicFlipStatus::InvalidQuery;
}

LowIRConcolicReport runLowIRConcolic(const LowFunc &Func,
                                     const LowIRConcolicOptions &Options) {
  LowIRConcolicReport Report;
  Report.FunctionEntry = Func.Entry;
  Report.FunctionName = Func.Name;
  Report.LiftComplete = Func.hasCompleteInstructionLift();

  std::optional<std::vector<SymConcreteRegister>> Initial =
      normalizeSeed(Options.InitialSeed, Options.ByteOrder);
  if (!Initial) {
    Report.TraceReason = ConcolicTraceReason::InvalidInitialSeed;
    return Report;
  }
  Report.InitialSeed = *Initial;

  OwnedTrace Trace = runTrace(Func, Options, *Initial);
  if (Trace.Exploration.Paths.empty()) {
    Report.TraceReason = ConcolicTraceReason::NoTrace;
    return Report;
  }
  if (Trace.Exploration.Paths.size() != 1) {
    Report.TraceReason = ConcolicTraceReason::AmbiguousTrace;
    return Report;
  }

  SymContext &Ctx = *Trace.Context;
  const SymPath &Path = Trace.Exploration.Paths.front();
  Report.TraceOutcome = Path.Outcome;
  Report.ExecutedSteps = Trace.Exploration.ExecutedSteps;
  Report.UnmodelledOps = Path.UnmodelledOps;
  Report.OpaqueOps = Path.OpaqueOps;
  Report.CallHavocs = Path.CallHavocs;
  Report.MemoryHavocs = Path.MemoryHavocs;
  Report.Blocks = Path.Blocks;
  Report.TraceComplete =
      Path.ConcreteComplete && isTerminalOutcome(Path.Outcome);
  Report.Decisions.reserve(Path.BranchDecisions.size());
  for (size_t I = 0; I < Path.BranchDecisions.size(); ++I) {
    const SymBranchDecision &Decision = Path.BranchDecisions[I];
    Report.Decisions.push_back({I, Decision.Occurrence, Decision.Taken,
                                Decision.ConstraintPrefix, Decision.Concrete});
  }

  if (Path.UnmodelledOps != 0 || Path.OpaqueOps != 0 || Path.CallHavocs != 0 ||
      Path.MemoryHavocs != 0) {
    Report.TraceReason = ConcolicTraceReason::UnsupportedEffects;
    return Report;
  }
  if (!Path.ConcreteComplete) {
    Report.TraceReason = ConcolicTraceReason::IncompleteConcreteTrace;
    return Report;
  }
  if (!isTerminalOutcome(Path.Outcome)) {
    Report.TraceReason = ConcolicTraceReason::IncompleteOutcome;
    return Report;
  }
  if (!Report.LiftComplete) {
    Report.TraceReason = ConcolicTraceReason::IncompleteLift;
    return Report;
  }
  if (!validDecisionHistory(Ctx, Path)) {
    Report.TraceReason = ConcolicTraceReason::InvalidDecisionHistory;
    return Report;
  }

  Report.TraceExact = true;
  Report.TraceReason = ConcolicTraceReason::None;

  solver::SolverOptions SolverOptions = Options.Solver;
  SolverOptions.BuildModel = true;
  solver::BitVectorSolver Solver(Ctx, SolverOptions);
  size_t AssertedPrefix = 0;
  bool PrefixUsable = true;

  for (size_t DecisionIndex = 0; DecisionIndex < Path.BranchDecisions.size();
       ++DecisionIndex) {
    const SymBranchDecision &Decision = Path.BranchDecisions[DecisionIndex];

    while (PrefixUsable && AssertedPrefix < Decision.ConstraintPrefix) {
      PrefixUsable = Solver.assertTrue(Path.Constraints[AssertedPrefix]);
      ++AssertedPrefix;
    }

    // Indirect target equalities are exact prefix constraints, but v1 does
    // not enumerate a different target.
    if (Decision.Occurrence.Kind != SymDecisionKind::ConditionalBranch) {
      if (PrefixUsable && AssertedPrefix == Decision.ConstraintPrefix) {
        PrefixUsable = Solver.assertTrue(Path.Constraints[AssertedPrefix]);
        ++AssertedPrefix;
      }
      continue;
    }

    LowIRConcolicFlip Flip;
    Flip.DecisionIndex = DecisionIndex;
    Flip.Occurrence = Decision.Occurrence;
    Flip.OriginalTaken = Decision.Taken;
    Flip.ConstraintPrefix = Decision.ConstraintPrefix;

    if (Report.FlipAttempts >= Options.MaxFlipAttempts) {
      Flip.Status = ConcolicFlipStatus::AttemptBudgetExceeded;
      Report.FlipBudgetHit = true;
      Report.Flips.push_back(std::move(Flip));
    } else if (!PrefixUsable) {
      Flip.EncodingError = Solver.encodeError();
      Flip.Status = detail::classifyPrefixEncodingFailure(Flip.EncodingError);
      Report.Flips.push_back(std::move(Flip));
    } else {
      ++Report.FlipAttempts;
      const SymRef Opposite =
          Decision.Taken ? Ctx.mkNot(Decision.Condition) : Decision.Condition;
      Flip.SolverResult = Solver.check({Opposite});
      Flip.EncodingError = Solver.encodeError();

      if (*Flip.SolverResult == solver::SatResult::Unsat) {
        Flip.Status = ConcolicFlipStatus::Unsat;
      } else if (*Flip.SolverResult == solver::SatResult::Unknown) {
        Flip.Status = ConcolicFlipStatus::SolverUnknown;
      } else if (*Flip.SolverResult == solver::SatResult::Invalid) {
        Flip.Status = ConcolicFlipStatus::InvalidQuery;
      } else {
        const SymRef Query = buildQuery(Ctx, Path, Decision);
        detail::ModelProjection Candidate = detail::projectRegisterModel(
            Ctx, Query, Solver.model(), *Initial, Options.ByteOrder);
        Flip.ProjectionReason = Candidate.Reason;
        if (Candidate.Reason != ConcolicProjectionReason::None) {
          Flip.Status = ConcolicFlipStatus::ProjectionRejected;
        } else {
          Flip.ReplayReason = verifyReplay(Func, Options, Report.Decisions,
                                           DecisionIndex, Candidate.Seed);
          if (Flip.ReplayReason != ConcolicReplayReason::None) {
            Flip.Status = ConcolicFlipStatus::ReplayRejected;
          } else {
            detail::CandidatePublication Publication =
                detail::publishReplayVerifiedSeed(Report.Candidates,
                                                  std::move(Candidate.Seed),
                                                  Options.MaxCandidates);
            Flip.Status = Publication.Status;
            Flip.CandidateIndex = Publication.CandidateIndex;
            if (Publication.Status ==
                ConcolicFlipStatus::CandidateBudgetExceeded)
              Report.CandidateBudgetHit = true;
          }
        }
      }
      Report.Flips.push_back(std::move(Flip));
    }

    if (PrefixUsable && AssertedPrefix == Decision.ConstraintPrefix) {
      PrefixUsable = Solver.assertTrue(Path.Constraints[AssertedPrefix]);
      ++AssertedPrefix;
    }
  }

  return Report;
}

} // namespace neverd::concolic
