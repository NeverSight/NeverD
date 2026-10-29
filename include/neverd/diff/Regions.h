//===- Regions.h - Correspondence below function boundaries -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded native instruction-run evidence and competing transformation
/// hypotheses. Regions may overlap and span several functions on either side;
/// they never replace the one-to-one inventory or certify equivalent behavior.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_REGIONS_H
#define NEVERD_DIFF_REGIONS_H

#include "neverd/diff/Control.h"
#include "neverd/diff/Limits.h"
#include "neverd/diff/Program.h"

#include "llvm/ADT/ArrayRef.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd::diff {
enum class ComparisonState : uint8_t;
struct FunctionMatch;

struct RegionSettings {
  bool Enabled = false;
  /// Include direct calls conditioned on the current function correspondence.
  /// This selects native-call-runs-v1, not semantic call equivalence.
  bool Calls = false;
  unsigned MinimumInstructions = limits::diff::kDefaultRegionMinInstructions;
  unsigned MinimumRepeatedInstructions =
      limits::diff::kDefaultRegionMinRepeatedInstructions;
  unsigned MaxOccurrences = limits::diff::kDefaultRegionMaxOccurrences;
  uint64_t MaxSeeds = limits::diff::kDefaultRegionMaxSeeds;
  uint64_t MaxWork = limits::diff::kDefaultRegionMaxWork;
  unsigned MaxAnchors = limits::diff::kDefaultRegionMaxAnchors;
  unsigned MaxHypotheses = limits::diff::kDefaultRegionMaxHypotheses;
  bool operator==(const RegionSettings &) const = default;
  std::string describe() const;
  std::optional<std::string> validate() const;
};

/// Coordinates in one exact, revision-bound snapshot. Block is relative to
/// Function; FirstInstruction is relative to Block, never a virtual address.
struct RegionSpan {
  uint32_t Function = NoIndex;
  uint32_t Block = NoIndex;
  uint32_t FirstInstruction = 0;
  uint32_t InstructionCount = 0;
  bool operator==(const RegionSpan &) const = default;
};

struct RegionAnchor {
  RegionSpan Primary, Secondary;
  /// Both sides' seed had exactly one occurrence in the admitted index.
  bool UniqueSeed = false;
  bool operator==(const RegionAnchor &) const = default;
};

/// Inferred explanations of observed instruction sharing; not source facts.
enum class RegionKind : uint8_t {
  Shared,
  Split,
  Merge,
  Inline,
  Outline,
  Clone,
  TailMerge,
  ModuleMove,
  Redistribution,
};
llvm::StringRef regionKindName(RegionKind Kind);
std::optional<RegionKind> regionKindNamed(llvm::StringRef Name);

/// Shared inference policy; each evidence profile supplies its own
/// observations.
struct RegionRelationshipFacts {
  unsigned PrimaryFunctions = 0, SecondaryFunctions = 0;
  bool Copies = false, CallsWithinPrimary = false, CallsWithinSecondary = false;
  bool MergedTail = false, ModuleMovement = false;
};
std::vector<RegionKind>
regionExplanations(const RegionRelationshipFacts &Facts);

struct RegionHypothesis {
  std::string Id;
  std::vector<uint32_t> Anchors;
  std::vector<uint32_t> PrimaryFunctions, SecondaryFunctions;
  std::vector<RegionKind> Explanations;
  uint64_t PrimaryInstructions = 0, SecondaryInstructions = 0;
  /// Coverage of eligible instructions in the participating functions.
  /// The call profile additionally admits direct calls with mapped targets;
  /// branches and returns never contribute to run evidence.
  double PrimaryCoverage = 0, SecondaryCoverage = 0;
  /// Heuristic evidence strength, not a calibrated probability.
  double Confidence = 0;
  bool Ambiguous = false;
  bool AnalysisComplete = true;
};

struct RegionAnalysis {
  bool Calls = false;
  /// Absent until requested. Complete means the bounded search finished,
  /// not that its hypotheses have been proved or all code was understood.
  std::optional<ComparisonState> State;
  std::string StoppedPhase;
  uint64_t Work = 0;
  uint64_t PrimarySeeds = 0, SecondarySeeds = 0;
  uint64_t PrimaryEligibleInstructions = 0, SecondaryEligibleInstructions = 0;
  uint64_t FrequentSeedGroups = 0, SuppressedGroups = 0;
  uint64_t WeakCandidates = 0;
  std::vector<RegionAnchor> Anchors;
  std::vector<RegionHypothesis> Hypotheses;
};

/// Exact normalized runs propose components in a bipartite function graph.
/// Repeated seeds remain ambiguous or are explicitly suppressed by the
/// occurrence bound. No instruction, call or memory effect is erased from the
/// original comparison; uncovered rows still need inspection.
RegionAnalysis findRegions(const Program &Primary, const Program &Secondary,
                           const RegionSettings &Settings,
                           const ComparisonControl &Control,
                           llvm::ArrayRef<FunctionMatch> Matches = {});

/// Validate persisted coordinates and normalized-run evidence against the
/// exact bound snapshots, including membership and arithmetic invariants.
std::optional<std::string>
validateRegions(const RegionAnalysis &Regions, const RegionSettings &Settings,
                const Program &Primary, const Program &Secondary,
                const ComparisonControl &Control = {},
                llvm::ArrayRef<FunctionMatch> Matches = {});

llvm::StringRef regionProfile(bool Calls);
} // namespace neverd::diff

#endif // NEVERD_DIFF_REGIONS_H
