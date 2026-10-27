//===- Comparison.h - Function correspondence between programs --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Which functions of one program correspond to which of another, how sure
/// the comparison is, and what changed between them.
///
/// Functions pair one to one.  The comparison first pairs functions that
/// share a feature no other unpaired function shares (a name the programs
/// state, identical bytes, an identical normalized body, a flow-graph MD
/// index, a set of strings and so on, strongest first); a feature several
/// functions share is split by the next features among just those
/// functions.  Each pair then lets the comparison look at the pair's callers
/// and callees, where a feature is far more often unique.  Functions still
/// unpaired may pair by similarity when one candidate leads clearly.
/// Functions the evidence cannot tell apart stay unpaired as an ambiguity
/// rather than receiving an arbitrary counterpart.
///
/// Similarity and confidence are heuristic scores.  A similarity of 1 means
/// identical normalized instructions and structure, not identical behavior,
/// and confidence is not a calibrated probability.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_COMPARISON_H
#define NEVERD_DIFF_COMPARISON_H

#include "neverd/diff/Control.h"
#include "neverd/diff/FlowEvidence.h"
#include "neverd/diff/FlowRegions.h"
#include "neverd/diff/Limits.h"
#include "neverd/diff/Program.h"
#include "neverd/diff/Regions.h"
#include "neverd/diff/ReviewJournal.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::diff {

/// How two functions were paired.
enum class FunctionStep : uint8_t {
#define NEVERD_DIFF_FUNCTION_STEP(Id, Name, Confidence, Description) Id,
#include "neverd/diff/MatchSteps.def"
};

/// How two blocks of a matched pair were paired.
enum class BlockStep : uint8_t {
#define NEVERD_DIFF_BLOCK_STEP(Id, Name, Confidence, Description) Id,
#include "neverd/diff/MatchSteps.def"
};

llvm::StringRef stepName(FunctionStep Step);
llvm::StringRef stepDescription(FunctionStep Step);
/// The step's prior confidence.
double stepConfidence(FunctionStep Step);
std::optional<FunctionStep> functionStepNamed(llvm::StringRef Name);
llvm::StringRef stepName(BlockStep Step);
llvm::StringRef stepDescription(BlockStep Step);
std::optional<BlockStep> blockStepNamed(llvm::StringRef Name);

/// One kind of change, as a bit of a change mask.
enum ChangeKind : uint32_t {
#define NEVERD_DIFF_CHANGE(Id, Bit, Letter, Name, Description)                 \
  Change##Id = 1u << Bit,
#include "neverd/diff/ChangeKinds.def"
};

/// Every change kind.
inline constexpr uint32_t AllChanges =
#define NEVERD_DIFF_CHANGE(Id, Bit, Letter, Name, Description) Change##Id |
#include "neverd/diff/ChangeKinds.def"
    0u;

/// Changes that are only movement: a pair with no other change has
/// identical normalized instructions and structure.
inline constexpr uint32_t MovementChanges = ChangeBytes | ChangeRelocation;

/// One mark per change kind, in bit order, '.' for a kind not in \p Mask.
std::string changeLetters(uint32_t Mask);
/// The names of the kinds in \p Mask, in bit order.
std::vector<llvm::StringRef> changeNames(uint32_t Mask);
/// The kind with this name.
std::optional<ChangeKind> changeNamed(llvm::StringRef Name);
llvm::StringRef changeDescription(ChangeKind Kind);

/// The settings one comparison runs with; with the two programs they decide
/// its result.
struct ComparisonSettings {
  /// Whether names the programs state pair functions.  Off, a stripped and
  /// a symbolized build compare as two stripped builds would.
  bool UseNames = true;
  /// Whether functions that share no exact feature may pair by similarity.
  bool Approximate = true;
  double MinimumSimilarity = limits::diff::kDefaultMinimumSimilarity;
  double MinimumContextSimilarity =
      limits::diff::kDefaultMinimumContextSimilarity;
  /// Required lead over alternatives; exact score ties remain undecided at 0.
  double AmbiguityMargin = limits::diff::kDefaultAmbiguityMargin;
  unsigned MaxCandidates = limits::diff::kDefaultMaxCandidates;
  RegionSettings Regions;
  DataFlowSettings DataFlow;
  FlowRegionSettings FlowRegions;

  bool operator==(const ComparisonSettings &) const = default;
  /// The settings as one stable string, part of a comparison's identity.
  std::string describe() const;
  /// An error when a value is out of range.
  std::optional<std::string> validate() const;
};

/// An analyst's decision about two functions.
enum class OverrideKind : uint8_t {
  /// Pair them, unpairing whatever either was paired with.
  Match,
  /// Unpair them, and never pair them again.
  Unmatch,
  /// Mark their pairing as reviewed.
  Confirm,
};
llvm::StringRef overrideName(OverrideKind Kind);
std::optional<OverrideKind> overrideNamed(llvm::StringRef Name);

/// An override names functions by entry address so it survives reanalysis
/// that keeps them.
struct Override {
  OverrideKind Kind = OverrideKind::Match;
  uint64_t Primary = 0;
  uint64_t Secondary = 0;

  bool operator==(const Override &) const = default;
};

/// Address pairs rejected by the final ordered decisions, in ascending order.
/// Match clears a previous rejection of the same pair; Confirm does not.
/// Historical entries need not name the current inventory. Callers that use
/// these pairs as evidence must resolve them in their own exact snapshots.
std::vector<std::pair<uint64_t, uint64_t>>
rejectedPairs(llvm::ArrayRef<Override> Overrides,
              const ComparisonControl &Control = {});

/// Two corresponding functions, by index into each program's functions.
struct FunctionMatch {
  uint32_t Primary = NoIndex;
  uint32_t Secondary = NoIndex;
  FunctionStep Step = FunctionStep::Similarity;
  /// Whether the pair came from this step's unique feature among the
  /// callers or callees of an earlier pair rather than among all functions.
  bool FromContext = false;
  /// Whether an analyst confirmed the pairing.
  bool Confirmed = false;
  /// Whether the structural delta below was computed; a stopped comparison
  /// may keep pairs without one.
  bool Measured = false;
  /// Neither side has a body; correspondence supplies identity evidence only.
  bool IdentityOnly = false;
  /// Both bodies were completely decoded/modelled by the selected profile.
  bool AnalysisComplete = false;
  /// Heuristic scores in [0, 1].
  double Similarity = 0;
  double Confidence = 0;
  /// ChangeKind mask.
  uint32_t Changes = 0;
  uint32_t MatchedBlocks = 0;
  uint32_t MatchedEdges = 0;
  /// Instructions with identical normalized forms.
  uint32_t EqualInstructions = 0;

  bool identical() const {
    return Measured && !IdentityOnly && AnalysisComplete &&
           (Changes & ~MovementChanges) == 0;
  }
};

/// Functions one feature could not tell apart: no one of them pairs with
/// any of the others.
struct Ambiguity {
  FunctionStep Step = FunctionStep::Similarity;
  std::vector<uint32_t> Primary;
  std::vector<uint32_t> Secondary;
};

/// What one side's functions came to.
struct SideCoverage {
  uint32_t Functions = 0;
  /// Functions with code to compare.
  uint32_t WithBody = 0;
  uint32_t Imports = 0;
  uint32_t Thunks = 0;
  uint32_t Library = 0;
  uint32_t Incomplete = 0;
  uint32_t Matched = 0;
  uint32_t Ambiguous = 0;
  uint32_t Unmatched = 0;
  uint64_t Instructions = 0;
  uint64_t MatchedInstructions = 0;
  bool DiscoveryComplete = true;
};

/// How long one phase took.
struct PhaseTime {
  std::string Phase;
  double Milliseconds = 0;
};

/// The outcome of comparing two programs.
struct ComparisonResult {
  ComparisonState State = ComparisonState::Complete;
  /// The phase a stopped comparison stopped in.
  std::string StoppedPhase;
  ComparisonSettings Settings;
  /// In ascending primary address order.
  std::vector<FunctionMatch> Matches;
  std::vector<Ambiguity> Ambiguities;
  /// Each function's match or ambiguity, by function index; NoIndex for
  /// none.
  std::vector<uint32_t> PrimaryMatch;
  std::vector<uint32_t> SecondaryMatch;
  std::vector<uint32_t> PrimaryAmbiguity;
  std::vector<uint32_t> SecondaryAmbiguity;
  SideCoverage PrimaryCoverage;
  SideCoverage SecondaryCoverage;
  /// Program-level scores over functions that are not library code,
  /// imports or thunks.
  double Similarity = 0;
  double Confidence = 0;
  std::vector<std::string> Warnings;
  /// The overrides the result reflects, in the order applied.
  std::vector<Override> Overrides;
  ReviewJournal Reviews;
  std::vector<PhaseTime> Timings;
  RegionAnalysis Regions;
  FlowRegionAnalysis FlowRegions;
  std::shared_ptr<const DataFlowEvidence> PrimaryDataFlow, SecondaryDataFlow;

  /// The counterpart of a primary function, or NoIndex.
  uint32_t secondaryOf(uint32_t Primary) const;
  uint32_t primaryOf(uint32_t Secondary) const;
  const FunctionMatch *matchOfPrimary(uint32_t Primary) const;
  const FunctionMatch *matchOfSecondary(uint32_t Secondary) const;
};

/// Compares \p Primary with \p Secondary.  \p Overrides apply in order: a
/// Match seeds its pair before any feature is consulted and an Unmatch
/// keeps its pair from ever forming.  An override naming an address that
/// begins no function is reported as a warning and skipped.
ComparisonResult compare(const Program &Primary, const Program &Secondary,
                         const ComparisonSettings &Settings,
                         llvm::ArrayRef<Override> Overrides = {},
                         const ComparisonControl &Control = {});

/// What one applied override changed, so that it can be reverted.
struct OverrideEffect {
  Override Applied;
  /// Matches it dissolved, as they were.
  std::vector<FunctionMatch> Removed;
  /// The match it formed or changed, as it now is.
  std::optional<FunctionMatch> Added;
  /// Matches it measured again because a function they call changed
  /// counterpart, as they were.
  std::vector<FunctionMatch> Remeasured;
  /// Ambiguities it changed or removed, in increasing original index order,
  /// as they were before the edit.
  std::vector<std::pair<uint32_t, Ambiguity>> Ambiguities;
};

/// Applies one more override to a finished comparison without comparing
/// again: the pair forms or dissolves, the functions it frees become
/// unmatched and leave any ambiguity, and the deltas it affects are
/// measured again. Groups without candidates on both sides are removed;
/// their remaining functions become unmatched. Returns an error, leaving
/// \p Result unchanged, when the override names no function or contradicts
/// the result (unpairing or
/// confirming an absent pair).
std::optional<std::string> applyOverride(ComparisonResult &Result,
                                         const Program &Primary,
                                         const Program &Secondary,
                                         const Override &O,
                                         OverrideEffect *Effect = nullptr);

/// Undoes the last override applied to \p Result, which \p Effect
/// describes.
void revertOverride(ComparisonResult &Result, const Program &Primary,
                    const Program &Secondary, const OverrideEffect &Effect);

/// Recomputes the coverage and program-level scores of \p Result from its
/// matches.
void summarize(ComparisonResult &Result, const Program &Primary,
               const Program &Secondary);

} // namespace neverd::diff

#endif // NEVERD_DIFF_COMPARISON_H
