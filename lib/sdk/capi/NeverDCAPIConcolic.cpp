//===- NeverDCAPIConcolic.cpp - LowIR concolic JSON C API ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/concolic/LowIRConcolic.h"
#include "neverd/sdk/NeverDCAPISymbolic.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;

namespace {

#define FIELD_END(Type, Field)                                                 \
  (offsetof(Type, Field) + sizeof(static_cast<Type *>(nullptr)->Field))

bool reaches(size_t Size, size_t End) { return Size >= End; }

bool isKnownOptionsPrefix(size_t Size) {
  if (Size >= sizeof(neverd_lowir_concolic_options_v1))
    return true;
  constexpr size_t Boundaries[] = {
      FIELD_END(neverd_lowir_concolic_options_v1, struct_size),
      FIELD_END(neverd_lowir_concolic_options_v1, register_seeds),
      FIELD_END(neverd_lowir_concolic_options_v1, register_seed_count),
      FIELD_END(neverd_lowir_concolic_options_v1, max_steps),
      FIELD_END(neverd_lowir_concolic_options_v1, max_block_visits),
      FIELD_END(neverd_lowir_concolic_options_v1, max_loop_iterations),
      FIELD_END(neverd_lowir_concolic_options_v1, max_flip_attempts),
      FIELD_END(neverd_lowir_concolic_options_v1, max_candidates),
      FIELD_END(neverd_lowir_concolic_options_v1, reserved),
      FIELD_END(neverd_lowir_concolic_options_v1, solver_max_conflicts),
      FIELD_END(neverd_lowir_concolic_options_v1, solver_max_propagations),
      FIELD_END(neverd_lowir_concolic_options_v1, solver_max_watch_visits),
      FIELD_END(neverd_lowir_concolic_options_v1, solver_max_gates),
  };
  return std::find(std::begin(Boundaries), std::end(Boundaries), Size) !=
         std::end(Boundaries);
}

struct ParsedOptions {
  concolic::LowIRConcolicOptions Value;
  std::string Error;
};

ParsedOptions readOptions(const neverd_lowir_concolic_options_v1 *Input) {
  ParsedOptions Result;
  if (!Input)
    return Result;

  const size_t Size = Input->struct_size;
  if (!isKnownOptionsPrefix(Size)) {
    Result.Error = "options struct_size splits a v1 field";
    return Result;
  }

  const bool HasSeedPointer = reaches(
      Size, FIELD_END(neverd_lowir_concolic_options_v1, register_seeds));
  const bool HasSeedCount = reaches(
      Size, FIELD_END(neverd_lowir_concolic_options_v1, register_seed_count));
  if (HasSeedPointer != HasSeedCount) {
    Result.Error = "register seed pointer/count prefix is incomplete";
    return Result;
  }

  if (HasSeedCount) {
    const size_t Count = Input->register_seed_count;
    if (Count > NEVERD_LOWIR_CONCOLIC_MAX_REGISTER_SEEDS_V1) {
      Result.Error = "register seed count exceeds the v1 ceiling";
      return Result;
    }
    if (Count > std::numeric_limits<size_t>::max() /
                    sizeof(neverd_lowir_concolic_register_seed_v1)) {
      Result.Error = "register seed array size overflows";
      return Result;
    }
    if (Count != 0 && !Input->register_seeds) {
      Result.Error = "nonzero register seed count requires a pointer";
      return Result;
    }

    Result.Value.InitialSeed.reserve(Count);
    struct Range {
      uint64_t Begin;
      uint64_t End;
    };
    std::vector<Range> Ranges;
    Ranges.reserve(Count);
    for (size_t I = 0; I < Count; ++I) {
      const neverd_lowir_concolic_register_seed_v1 &Seed =
          Input->register_seeds[I];
      if (Seed.reserved != 0) {
        Result.Error = "register seed reserved field must be zero";
        return Result;
      }
      if (Seed.bytes == 0 || Seed.bytes > 8) {
        Result.Error = "register seed width must be between 1 and 8 bytes";
        return Result;
      }
      if (Seed.offset >
          std::numeric_limits<uint64_t>::max() - uint64_t(Seed.bytes)) {
        Result.Error = "register seed range overflows";
        return Result;
      }
      if (Seed.bytes < 8 &&
          Seed.value >= (uint64_t{1} << (unsigned(Seed.bytes) * 8))) {
        Result.Error = "register seed value does not fit its width";
        return Result;
      }
      Ranges.push_back({Seed.offset, Seed.offset + Seed.bytes});
      Result.Value.InitialSeed.push_back(
          {Seed.offset, static_cast<uint16_t>(Seed.bytes), Seed.value});
    }
    std::sort(Ranges.begin(), Ranges.end(),
              [](const Range &Left, const Range &Right) {
                if (Left.Begin != Right.Begin)
                  return Left.Begin < Right.Begin;
                return Left.End < Right.End;
              });
    for (size_t I = 1; I < Ranges.size(); ++I) {
      if (Ranges[I].Begin < Ranges[I - 1].End) {
        Result.Error = "register seed ranges overlap";
        return Result;
      }
    }
  }

#define READ_NONZERO(Field, Destination)                                       \
  do {                                                                         \
    if (reaches(Size, FIELD_END(neverd_lowir_concolic_options_v1, Field)) &&   \
        Input->Field != 0)                                                     \
      Destination = Input->Field;                                              \
  } while (false)

  READ_NONZERO(max_steps, Result.Value.MaxSteps);
  READ_NONZERO(max_block_visits, Result.Value.MaxBlockVisits);
  READ_NONZERO(max_loop_iterations, Result.Value.MaxLoopIterations);
  READ_NONZERO(max_flip_attempts, Result.Value.MaxFlipAttempts);
  READ_NONZERO(max_candidates, Result.Value.MaxCandidates);
  READ_NONZERO(solver_max_conflicts, Result.Value.Solver.Sat.MaxConflicts);
  READ_NONZERO(solver_max_propagations,
               Result.Value.Solver.Sat.MaxPropagations);
  READ_NONZERO(solver_max_watch_visits, Result.Value.Solver.Sat.MaxWatchVisits);
#undef READ_NONZERO

  if (reaches(Size, FIELD_END(neverd_lowir_concolic_options_v1, reserved)) &&
      Input->reserved != 0) {
    Result.Error = "options reserved field must be zero";
    return Result;
  }
  if (reaches(Size,
              FIELD_END(neverd_lowir_concolic_options_v1, solver_max_gates)) &&
      Input->solver_max_gates != 0) {
    if constexpr (sizeof(size_t) < sizeof(uint64_t)) {
      if (Input->solver_max_gates > std::numeric_limits<size_t>::max()) {
        Result.Error = "solver gate budget does not fit size_t";
        return Result;
      }
    }
    Result.Value.Solver.Blast.MaxGates =
        static_cast<size_t>(Input->solver_max_gates);
  }
  return Result;
}

llvm::json::Object baseReport(bool Ok) {
  return llvm::json::Object{{"schema_version", 1},
                            {"adapter", "lowir-concolic-v1"},
                            {"mode", "concolic"},
                            {"ok", Ok},
                            {"exhaustive", false}};
}

const char *errorReport(llvm::StringRef Code, llvm::StringRef Message) {
  llvm::json::Object Root = baseReport(false);
  Root["error_code"] = jsonSafeText(Code);
  Root["error"] = jsonSafeText(Message);
  return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
}

const char *sessionError(Session *S, llvm::StringRef Code,
                         llvm::StringRef Message) {
  const std::string OwnedMessage = jsonSafeText(Message);
  S->setError(OwnedMessage);
  return errorReport(Code, OwnedMessage);
}

std::string fixedHex(uint64_t Value, unsigned Digits) {
  return "0x" + llvm::utohexstr(Value, /*LowerCase=*/true, Digits);
}

std::string sha256(llvm::ArrayRef<uint8_t> Bytes) {
  llvm::SHA256 Hash;
  Hash.update(Bytes);
  const auto Digest = Hash.final();
  static constexpr char Digits[] = "0123456789abcdef";
  std::string Result;
  Result.reserve(Digest.size() * 2);
  for (uint8_t Byte : Digest) {
    Result.push_back(Digits[Byte >> 4]);
    Result.push_back(Digits[Byte & 0xf]);
  }
  return Result;
}

llvm::json::Object imageReport(const BinaryImage &Image) {
  llvm::json::Object Result{
      {"format", jsonSafeText(Image.getFormatName())},
      {"arch", jsonSafeText(getArchName(Image.Arch))},
      {"bits", static_cast<int64_t>(getBitnessValue(Image.Bits))},
      {"endianness", "little"},
      {"base", fixedHex(Image.Base, 16)},
      {"entry", fixedHex(Image.Entry, 16)},
  };
  if (Image.Raw.empty()) {
    Result["identity_status"] = "unavailable";
    Result["identity_reason"] = "loaded_snapshot_bytes_unavailable";
    Result["sha256"] = nullptr;
  } else {
    Result["identity_status"] = "exact_loaded_snapshot";
    Result["identity_reason"] = nullptr;
    Result["sha256"] = sha256(Image.Raw);
  }
  return Result;
}

llvm::json::Object seedReport(const symbolic::SymConcreteRegister &Seed) {
  return llvm::json::Object{
      {"offset", fixedHex(Seed.Offset, 16)},
      {"bytes", static_cast<int64_t>(Seed.Bytes)},
      {"value", fixedHex(Seed.Value, unsigned(Seed.Bytes) * 2)},
  };
}

llvm::json::Array
seedReport(llvm::ArrayRef<symbolic::SymConcreteRegister> Seed) {
  llvm::json::Array Result;
  Result.reserve(Seed.size());
  for (const symbolic::SymConcreteRegister &Range : Seed)
    Result.push_back(seedReport(Range));
  return Result;
}

const char *decisionKindName(symbolic::SymDecisionKind Kind) {
  switch (Kind) {
  case symbolic::SymDecisionKind::ConditionalBranch:
    return "conditional_branch";
  case symbolic::SymDecisionKind::IndirectBranchTarget:
    return "indirect_branch_target";
  }
  return "unknown";
}

llvm::json::Object
occurrenceReport(const symbolic::SymDecisionOccurrence &Occurrence) {
  return llvm::json::Object{
      {"va", fixedHex(Occurrence.VA, 16)},
      {"seq", static_cast<int64_t>(Occurrence.Seq)},
      {"block_id", static_cast<int64_t>(Occurrence.BlockId)},
      {"op_index", static_cast<uint64_t>(Occurrence.OpIndex)},
      {"invocation", static_cast<uint64_t>(Occurrence.Invocation)},
      {"kind", decisionKindName(Occurrence.Kind)},
  };
}

llvm::json::Array
decisionReports(llvm::ArrayRef<concolic::LowIRConcolicDecision> Decisions) {
  llvm::json::Array Result;
  Result.reserve(Decisions.size());
  for (const concolic::LowIRConcolicDecision &Decision : Decisions) {
    Result.push_back(llvm::json::Object{
        {"decision_id", static_cast<uint64_t>(Decision.Index)},
        {"occurrence", occurrenceReport(Decision.Occurrence)},
        {"taken", Decision.Taken},
        {"constraint_prefix", static_cast<uint64_t>(Decision.ConstraintPrefix)},
        {"concrete", Decision.Concrete},
    });
  }
  return Result;
}

const char *projectionStatus(const concolic::LowIRConcolicFlip &Flip) {
  if (!Flip.SolverResult || *Flip.SolverResult != solver::SatResult::Sat)
    return "not_run";
  return Flip.ProjectionReason == concolic::ConcolicProjectionReason::None
             ? "accepted"
             : "rejected";
}

const char *replayStatus(const concolic::LowIRConcolicFlip &Flip) {
  if (!Flip.SolverResult || *Flip.SolverResult != solver::SatResult::Sat ||
      Flip.ProjectionReason != concolic::ConcolicProjectionReason::None)
    return "not_run";
  if (Flip.ReplayReason != concolic::ConcolicReplayReason::None)
    return "rejected";
  switch (Flip.Status) {
  case concolic::ConcolicFlipStatus::Verified:
  case concolic::ConcolicFlipStatus::VerifiedDuplicate:
  case concolic::ConcolicFlipStatus::CandidateBudgetExceeded:
    return "verified";
  default:
    return "not_run";
  }
}

llvm::json::Array
flipReports(llvm::ArrayRef<concolic::LowIRConcolicFlip> Flips) {
  llvm::json::Array Result;
  Result.reserve(Flips.size());
  for (const concolic::LowIRConcolicFlip &Flip : Flips) {
    llvm::json::Object Item{
        {"decision_id", static_cast<uint64_t>(Flip.DecisionIndex)},
        {"occurrence", occurrenceReport(Flip.Occurrence)},
        {"original_taken", Flip.OriginalTaken},
        {"constraint_prefix", static_cast<uint64_t>(Flip.ConstraintPrefix)},
        {"status", concolic::concolicFlipStatusName(Flip.Status)},
        {"solver_status", Flip.SolverResult
                              ? solver::satResultName(*Flip.SolverResult)
                              : "not_run"},
        {"encoding_error", solver::blastErrorName(Flip.EncodingError)},
        {"projection_status", projectionStatus(Flip)},
        {"projection_reason",
         concolic::concolicProjectionReasonName(Flip.ProjectionReason)},
        {"replay_status", replayStatus(Flip)},
        {"replay_reason",
         concolic::concolicReplayReasonName(Flip.ReplayReason)},
    };
    if (Flip.CandidateIndex)
      Item["candidate_id"] = static_cast<uint64_t>(*Flip.CandidateIndex);
    else
      Item["candidate_id"] = nullptr;
    Result.push_back(std::move(Item));
  }
  return Result;
}

llvm::json::Array
candidateReports(llvm::ArrayRef<concolic::LowIRConcolicCandidate> Candidates) {
  llvm::json::Array Result;
  Result.reserve(Candidates.size());
  for (size_t I = 0; I < Candidates.size(); ++I) {
    Result.push_back(llvm::json::Object{
        {"candidate_id", static_cast<uint64_t>(I)},
        {"seed", seedReport(Candidates[I].Seed)},
    });
  }
  return Result;
}

llvm::json::Array blockReport(llvm::ArrayRef<int> Blocks) {
  llvm::json::Array Result;
  Result.reserve(Blocks.size());
  for (int Block : Blocks)
    Result.push_back(static_cast<int64_t>(Block));
  return Result;
}

llvm::json::Object successReport(const Session &S,
                                 const concolic::LowIRConcolicOptions &Options,
                                 const concolic::LowIRConcolicReport &Report) {
  llvm::json::Object Root = baseReport(true);
  Root["image"] = imageReport(S.Img);
  Root["function"] = llvm::json::Object{
      {"entry", fixedHex(Report.FunctionEntry, 16)},
      {"name", jsonSafeText(Report.FunctionName)},
      {"lift_complete", Report.LiftComplete},
  };
  Root["limits"] = llvm::json::Object{
      {"max_steps", static_cast<uint64_t>(Options.MaxSteps)},
      {"max_block_visits", static_cast<uint64_t>(Options.MaxBlockVisits)},
      {"max_loop_iterations", static_cast<uint64_t>(Options.MaxLoopIterations)},
      {"max_flip_attempts", static_cast<uint64_t>(Options.MaxFlipAttempts)},
      {"max_candidates", static_cast<uint64_t>(Options.MaxCandidates)},
      {"solver_max_conflicts", Options.Solver.Sat.MaxConflicts},
      {"solver_max_propagations", Options.Solver.Sat.MaxPropagations},
      {"solver_max_watch_visits", Options.Solver.Sat.MaxWatchVisits},
      {"solver_max_width",
       static_cast<uint64_t>(Options.Solver.Blast.MaxWidth)},
      {"solver_max_gates",
       static_cast<uint64_t>(Options.Solver.Blast.MaxGates)},
  };
  Root["initial_seed"] = seedReport(Report.InitialSeed);
  if (Report.TraceOutcome)
    Root["trace_outcome"] = symbolic::pathOutcomeName(*Report.TraceOutcome);
  else
    Root["trace_outcome"] = nullptr;
  Root["trace_complete"] = Report.TraceComplete;
  Root["trace_exact"] = Report.TraceExact;
  Root["trace_reason"] = concolic::concolicTraceReasonName(Report.TraceReason);
  Root["executed_steps"] = static_cast<uint64_t>(Report.ExecutedSteps);
  Root["unmodelled_ops"] = static_cast<uint64_t>(Report.UnmodelledOps);
  Root["opaque_ops"] = static_cast<uint64_t>(Report.OpaqueOps);
  Root["call_havocs"] = static_cast<uint64_t>(Report.CallHavocs);
  Root["memory_havocs"] = static_cast<uint64_t>(Report.MemoryHavocs);
  Root["flip_attempts"] = static_cast<uint64_t>(Report.FlipAttempts);
  Root["flip_budget_hit"] = Report.FlipBudgetHit;
  Root["candidate_budget_hit"] = Report.CandidateBudgetHit;
  Root["blocks"] = blockReport(Report.Blocks);
  Root["decisions"] = decisionReports(Report.Decisions);
  Root["flips"] = flipReports(Report.Flips);
  Root["candidates"] = candidateReports(Report.Candidates);
  return Root;
}

void setBoundaryErrorNoThrow(Session *S, llvm::StringRef Message) noexcept {
  if (!S)
    return;
  try {
    S->clearError();
    S->setError(Message.str());
  } catch (...) {
  }
}

const char *internalErrorReportNoThrow(Session *S,
                                       llvm::StringRef Message) noexcept {
  try {
    std::string Detail = "unexpected LowIR concolic failure";
    if (!Message.empty()) {
      Detail += ": ";
      Detail += jsonSafeText(Message);
    }
    setBoundaryErrorNoThrow(S, Detail);
    const char *Owned = errorReport("internal_error", Detail);
    if (!Owned)
      setBoundaryErrorNoThrow(
          S,
          "LowIR concolic allocation failed while reporting an internal error");
    return Owned;
  } catch (const std::bad_alloc &) {
    setBoundaryErrorNoThrow(
        S,
        "LowIR concolic allocation failed while reporting an internal error");
  } catch (...) {
    setBoundaryErrorNoThrow(S,
                            "LowIR concolic internal error reporting failed");
  }
  return nullptr;
}

const char *
lowIRConcolicJSONImpl(neverd_session_t Sess, neverd_va_t FuncEntry,
                      const neverd_lowir_concolic_options_v1 *Options) {
  Session *S = toSession(Sess);
  if (!S)
    return errorReport("invalid_session", "invalid session");
  S->clearError();
  if (S->LowIRConcolicBeforeRunForTesting)
    S->LowIRConcolicBeforeRunForTesting();
  ParsedOptions Parsed = readOptions(Options);
  if (!Parsed.Error.empty())
    return sessionError(S, "invalid_options", Parsed.Error);
  if (!S->Loaded)
    return sessionError(S, "no_binary_loaded", "no binary loaded");
  if (S->Img.Arch != Arch::X64 && S->Img.Arch != Arch::AArch64)
    return sessionError(S, "unsupported_target",
                        "LowIR concolic v1 requires x86_64 or AArch64 LowIR");
  if (!S->ensurePipeline())
    return sessionError(S, "pipeline_failed", S->LastError);
  const LowFunc *Function = S->findLowFunc(FuncEntry);
  if (!Function)
    return sessionError(S, "function_not_found",
                        "function not found in native LowIR");

  concolic::LowIRConcolicReport Report =
      concolic::runLowIRConcolic(*Function, Parsed.Value);
  if (Report.Version != 1)
    return sessionError(S, "internal_error",
                        "unexpected LowIR concolic report version");
  S->clearError();
  return dupStr(
      jsonToString(llvm::json::Value(successReport(*S, Parsed.Value, Report))));
}

} // namespace

extern "C" {

const char *
neverd_lowir_concolic_json_v1(neverd_session_t Sess, neverd_va_t FuncEntry,
                              const neverd_lowir_concolic_options_v1 *Options) {
  Session *S = toSession(Sess);
  try {
    const char *Owned = lowIRConcolicJSONImpl(Sess, FuncEntry, Options);
    if (!Owned)
      setBoundaryErrorNoThrow(S, "LowIR concolic report allocation failed");
    return Owned;
  } catch (const std::bad_alloc &) {
    setBoundaryErrorNoThrow(S, "LowIR concolic allocation failed");
    return nullptr;
  } catch (const std::exception &Exception) {
    return internalErrorReportNoThrow(S, Exception.what());
  } catch (...) {
    return internalErrorReportNoThrow(S, "non-standard native exception");
  }
}

} // extern "C"

#undef FIELD_END
