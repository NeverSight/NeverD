//===- ComparisonJSON.h - Saved binary comparisons --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A comparison as a versioned JSON document: the settings and the identity
/// of both inputs it was made with, every pair with its evidence and scores,
/// the ambiguities, the analyst's overrides and the coverage.  Functions are
/// named by entry address, so a saved comparison binds to the programs it
/// was made from even after another analysis numbered their functions
/// differently, and refuses programs read from other inputs or with other
/// settings instead of rebinding to them.
///
/// The document also lists each side's functions, so a saved comparison can
/// be browsed without its inputs; only the structural deltas need them.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_COMPARISONJSON_H
#define NEVERD_DIFF_COMPARISONJSON_H

#include "neverd/diff/Comparison.h"
#include "neverd/diff/Control.h"
#include "neverd/diff/Program.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::diff {

/// The document's schema name and the version this build writes and reads.
inline constexpr llvm::StringLiteral ComparisonSchema = "neverd.bindiff";
inline constexpr unsigned ComparisonSchemaVersion = 3;
inline constexpr unsigned OldestComparisonSchemaVersion = 2;
/// The version of the matcher, the features and the delta; part of a
/// comparison's identity.  Raise it whenever a change could pair functions
/// differently or measure a pair differently.
inline constexpr unsigned MatcherVersion = 4;

/// Writes \p Result of \p Primary and \p Secondary as a saved comparison.
/// SummaryOnly emits bounded metadata and counts without inventories or pairs.
void writeComparison(llvm::raw_ostream &OS, const ComparisonResult &Result,
                     const Program &Primary, const Program &Secondary,
                     bool SummaryOnly = false);

/// One function of a saved comparison's list.
struct SavedFunction {
  uint64_t Address = 0;
  uint64_t Size = 0;
  std::string Name;
  uint16_t Traits = 0;
  uint32_t Blocks = 0;
  uint64_t Instructions = 0;
};

/// A pair of a saved comparison, by entry address.
struct SavedMatch {
  uint64_t Primary = 0;
  uint64_t Secondary = 0;
  FunctionMatch Match;
};

/// An ambiguity of a saved comparison, by entry address.
struct SavedAmbiguity {
  FunctionStep Step = FunctionStep::Similarity;
  std::vector<uint64_t> Primary;
  std::vector<uint64_t> Secondary;
};

/// A saved comparison as read, before it is bound to programs.
struct SavedComparison {
  unsigned MatcherVersion = 0;
  ComparisonState State = ComparisonState::Complete;
  std::string StoppedPhase;
  ComparisonSettings Settings;
  ProgramIdentity Primary;
  ProgramIdentity Secondary;
  std::vector<SavedFunction> PrimaryFunctions;
  std::vector<SavedFunction> SecondaryFunctions;
  std::vector<SavedMatch> Matches;
  std::vector<SavedAmbiguity> Ambiguities;
  std::vector<Override> Overrides;
  ReviewJournal Reviews;
  SideCoverage PrimaryCoverage;
  SideCoverage SecondaryCoverage;
  double Similarity = 0;
  double Confidence = 0;
  std::vector<std::string> Warnings;
  std::vector<PhaseTime> Timings;
  RegionAnalysis Regions;
  FlowRegionAnalysis FlowRegions;
  std::shared_ptr<const DataFlowEvidence> PrimaryDataFlow, SecondaryDataFlow;
};

/// Reads a saved comparison, checking every field's type and range.  The
/// error names the first problem. Cancellation/deadline throws
/// ComparisonInterrupted; the JSON DOM parser is one indivisible checkpoint.
std::optional<SavedComparison>
readComparison(llvm::StringRef Text, std::string &Error,
               const ComparisonControl &Control = {});

/// Why a saved comparison does not bind to two programs.
enum class BindFailure : uint8_t {
  /// An input's file hash differs.
  StaleInput,
  /// An input was analyzed with other settings.
  StaleAnalysis,
  /// A function the comparison names is not in the program.
  MissingFunction,
  /// Saved structural facts disagree with the current decoded evidence.
  InvalidEvidence,
};
llvm::StringRef bindFailureName(BindFailure Failure);

/// Binds \p Saved to \p Primary and \p Secondary, which must be the
/// programs it was made from: the same inputs read with the same analysis.
/// Saved pairs and analyst decisions are kept; measured structural facts are
/// recomputed and must agree before publication. On failure
/// \p Failure says why and the returned message which input or function.
/// Cancellation/deadline throws ComparisonInterrupted. Result is unchanged on
/// failure or stop; progress callbacks run only on the calling thread.
std::optional<std::string>
bindComparison(const SavedComparison &Saved, const Program &Primary,
               const Program &Secondary, ComparisonResult &Result,
               BindFailure *Failure = nullptr,
               const ComparisonControl &Control = {});

} // namespace neverd::diff

#endif // NEVERD_DIFF_COMPARISONJSON_H
