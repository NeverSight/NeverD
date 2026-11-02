//===- FlowRegions.h - Overlapping verified SSA neighborhoods ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Finite SSA correspondences below function boundaries. Candidate roots are
/// indexed, exactly checked and grouped into alternative transformation claims.
/// Full native deltas remain separate; neither matching nor coverage proves
/// whole-function behavior, patch presence or vulnerability elimination.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_FLOWREGIONS_H
#define NEVERD_DIFF_FLOWREGIONS_H

#include "neverd/diff/Control.h"
#include "neverd/diff/FlowEvidence.h"
#include "neverd/diff/FlowMatch.h"
#include "neverd/diff/Regions.h"

namespace neverd::diff {
inline constexpr llvm::StringLiteral FlowRegionProfile = "med-neighborhoods-v1";
inline constexpr llvm::StringLiteral LLVMFlowRegionProfile =
    "llvm-neighborhoods-v1";
inline constexpr llvm::StringLiteral FlowFragmentProfile = "med-fragments-v1";
inline constexpr llvm::StringLiteral LLVMFlowFragmentProfile =
    "llvm-fragments-v1";
inline constexpr llvm::StringLiteral LLVMParameterFlowRegionProfile =
    "llvm-parameter-neighborhoods-v1";
inline constexpr llvm::StringLiteral LLVMParameterFlowFragmentProfile =
    "llvm-parameter-fragments-v1";
inline constexpr llvm::StringLiteral LLVMProgressFlowRegionProfile =
    "llvm-progress-neighborhoods-v1";
inline constexpr llvm::StringLiteral LLVMProgressFlowFragmentProfile =
    "llvm-progress-fragments-v1";
inline constexpr llvm::StringLiteral LLVMMultiBlockProgressFlowRegionProfile =
    "llvm-progress-neighborhoods-v2";
inline constexpr llvm::StringLiteral LLVMMultiBlockProgressFlowFragmentProfile =
    "llvm-progress-fragments-v2";
inline constexpr llvm::StringLiteral LLVMNestedProgressFlowRegionProfile =
    "llvm-progress-neighborhoods-v3";
inline constexpr llvm::StringLiteral LLVMNestedProgressFlowFragmentProfile =
    "llvm-progress-fragments-v3";
inline constexpr llvm::StringLiteral LLVMBranchProgressFlowRegionProfile =
    "llvm-progress-neighborhoods-v4";
inline constexpr llvm::StringLiteral LLVMBranchProgressFlowFragmentProfile =
    "llvm-progress-fragments-v4";
inline constexpr llvm::StringLiteral LLVMModularProgressFlowRegionProfile =
    "llvm-progress-neighborhoods-v5";
inline constexpr llvm::StringLiteral LLVMModularProgressFlowFragmentProfile =
    "llvm-progress-fragments-v5";
inline constexpr llvm::StringLiteral
    LLVMParameterBoundProgressFlowRegionProfile =
        "llvm-progress-neighborhoods-v6";
inline constexpr llvm::StringLiteral
    LLVMParameterBoundProgressFlowFragmentProfile =
        "llvm-progress-fragments-v6";
inline constexpr llvm::StringLiteral
    LLVMInvariantBoundProgressFlowRegionProfile =
        "llvm-progress-neighborhoods-v7";
inline constexpr llvm::StringLiteral
    LLVMInvariantBoundProgressFlowFragmentProfile =
        "llvm-progress-fragments-v7";
inline bool isFlowFragmentProfile(llvm::StringRef Profile) {
  return Profile == FlowFragmentProfile || Profile == LLVMFlowFragmentProfile ||
         Profile == LLVMParameterFlowFragmentProfile ||
         Profile == LLVMProgressFlowFragmentProfile ||
         Profile == LLVMMultiBlockProgressFlowFragmentProfile ||
         Profile == LLVMNestedProgressFlowFragmentProfile ||
         Profile == LLVMBranchProgressFlowFragmentProfile ||
         Profile == LLVMModularProgressFlowFragmentProfile ||
         Profile == LLVMParameterBoundProgressFlowFragmentProfile ||
         Profile == LLVMInvariantBoundProgressFlowFragmentProfile;
}
inline bool isLLVMFlowRegionProfile(llvm::StringRef Profile) {
  return Profile == LLVMFlowRegionProfile ||
         Profile == LLVMFlowFragmentProfile ||
         Profile == LLVMParameterFlowRegionProfile ||
         Profile == LLVMProgressFlowRegionProfile ||
         Profile == LLVMMultiBlockProgressFlowRegionProfile ||
         Profile == LLVMNestedProgressFlowRegionProfile ||
         Profile == LLVMBranchProgressFlowRegionProfile ||
         Profile == LLVMModularProgressFlowRegionProfile ||
         Profile == LLVMParameterBoundProgressFlowRegionProfile ||
         Profile == LLVMInvariantBoundProgressFlowRegionProfile ||
         Profile == LLVMParameterFlowFragmentProfile ||
         Profile == LLVMProgressFlowFragmentProfile ||
         Profile == LLVMMultiBlockProgressFlowFragmentProfile ||
         Profile == LLVMNestedProgressFlowFragmentProfile ||
         Profile == LLVMBranchProgressFlowFragmentProfile ||
         Profile == LLVMModularProgressFlowFragmentProfile ||
         Profile == LLVMParameterBoundProgressFlowFragmentProfile ||
         Profile == LLVMInvariantBoundProgressFlowFragmentProfile;
}
llvm::StringRef flowRegionProfile(llvm::StringRef GraphProfile,
                                  bool Fragments = false);
struct FlowRegionSettings {
  bool Enabled = false;
  unsigned MinimumNodes = limits::diff::kDefaultFlowRegionMinNodes;
  unsigned MinimumOrigins = limits::diff::kDefaultFlowRegionMinOrigins;
  unsigned MaxOccurrences = limits::diff::kDefaultFlowRegionOccurrences;
  uint64_t MaxSeeds = limits::diff::kDefaultFlowRegionSeeds;
  uint64_t MaxWork = limits::diff::kDefaultFlowRegionWork;
  unsigned MaxAnchors = limits::diff::kDefaultFlowRegionAnchors;
  uint64_t MaxNodePairs = limits::diff::kDefaultFlowRegionPairs;
  bool Fragments = false;
  bool operator==(const FlowRegionSettings &) const = default;
  std::optional<std::string> validate() const;
  std::string describe() const;
};
struct FlowRegionAnchor {
  std::string Id;
  uint64_t PrimaryFunction = 0, SecondaryFunction = 0;
  uint32_t PrimaryRoot = NoIndex, SecondaryRoot = NoIndex;
  std::vector<std::pair<uint32_t, uint32_t>> Nodes, PhiBlocks;
  uint32_t Gaps = 0;
  bool OpenDataFrontier = false, OpenControlFrontier = true, UniqueSeed = false;
  uint32_t PrimaryOrigins = 0, SecondaryOrigins = 0;
  /// Zero is the original fixed-neighborhood recipe. Fragment depth counts
  /// included operation levels; its external bindings are not matched nodes.
  unsigned Depth = 0;
  std::vector<std::pair<uint32_t, uint32_t>> Frontier;
  bool operator==(const FlowRegionAnchor &) const = default;
};
struct FlowRegionHypothesis {
  std::string Id;
  std::vector<uint32_t> Anchors;
  std::vector<uint64_t> PrimaryFunctions, SecondaryFunctions;
  std::vector<RegionKind> Explanations;
  uint64_t PrimaryNodes = 0, SecondaryNodes = 0;
  double PrimaryCoverage = 0, SecondaryCoverage = 0;
  uint32_t Gaps = 0;
  bool OpenDataFrontier = false, OpenControlFrontier = true, Ambiguous = false;
  bool operator==(const FlowRegionHypothesis &) const = default;
};
struct FlowRegionAnalysis {
  std::string Profile = FlowRegionProfile.str();
  std::optional<ComparisonState> State;
  std::string StoppedPhase;
  uint64_t Work = 0, PrimarySeeds = 0, SecondarySeeds = 0;
  uint64_t FrequentSeedGroups = 0, WeakCandidates = 0, AmbiguousCandidates = 0;
  uint64_t DifferentCandidates = 0, LimitedGraphs = 0, SuppressedGroups = 0;
  bool PrimaryScopeComplete = false, SecondaryScopeComplete = false;
  std::vector<FlowRegionAnchor> Anchors;
  std::vector<FlowRegionHypothesis> Hypotheses;
  bool operator==(const FlowRegionAnalysis &) const = default;
};
/// A saved partial collection can be checked against its bound native input
/// without copying that input's decoded instruction and byte inventories.
struct FlowRegionInput {
  const Program &Native;
  const DataFlowEvidence *Evidence;
  FlowRegionInput(const Program &P) : Native(P), Evidence(P.DataFlow.get()) {}
  FlowRegionInput(const Program &P, const DataFlowEvidence *E)
      : Native(P), Evidence(E) {}
};
FlowRegionAnalysis findFlowRegions(FlowRegionInput Primary,
                                   FlowRegionInput Secondary,
                                   const DataFlowSettings &EvidenceSettings,
                                   const FlowRegionSettings &Settings,
                                   const ComparisonControl &Control = {});
/// Recompute from bound observations. Interrupted records may contain only a
/// completed prefix of anchors/groups; completed and resource-limited evidence
/// must reproduce exactly. Historical work counters need only fit their bound.
/// This never trusts saved coverage or node mappings.
std::optional<std::string> validateFlowRegions(
    const FlowRegionAnalysis &Saved, FlowRegionInput Primary,
    FlowRegionInput Secondary, const DataFlowSettings &EvidenceSettings,
    const FlowRegionSettings &Settings, const ComparisonControl &Control = {});
} // namespace neverd::diff

#endif // NEVERD_DIFF_FLOWREGIONS_H
