//===- FlowRegionsJSON.cpp - Bound SSA region persistence and pages ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Stream inventories without a second full JSON graph; display previews keep
/// coordinates on separately bounded pages. Addresses use hexadecimal strings.
//===----------------------------------------------------------------------===//

#include "neverd/diff/FlowRegionsJSON.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace neverd::diff {
namespace {
namespace limits = neverd::limits::diff;
using Object = llvm::json::Object;
using Array = llvm::json::Array;
using Value = llvm::json::Value;
void require(bool Good, llvm::StringRef Why) {
  if (!Good)
    throw std::invalid_argument("SSA regions: " + Why.str());
}
const Object &object(const Value &V) {
  require(V.getAsObject(), "expected object");
  return *V.getAsObject();
}
const Value &field(const Object &O, llvm::StringRef Key) {
  require(O.get(Key), "missing field " + Key.str());
  return *O.get(Key);
}
uint64_t count(const Value &V, uint64_t Max = UINT32_MAX) {
  const auto N = V.getAsInteger();
  require(N && *N >= 0 && uint64_t(*N) <= Max, "invalid bounded integer");
  return *N;
}
bool boolean(const Value &V) {
  const auto B = V.getAsBoolean();
  require(B.has_value(), "expected boolean");
  return *B;
}
std::string text(const Value &V, size_t Max = limits::kMaxRegionIdBytes) {
  const auto S = V.getAsString();
  require(S && S->size() <= Max, "invalid or oversized string");
  return S->str();
}
const Array &array(const Value &V, uint64_t Max) {
  const auto *A = V.getAsArray();
  require(A && A->size() <= Max, "invalid or oversized array");
  return *A;
}
std::string hex(uint64_t V) { return "0x" + llvm::utohexstr(V, true); }
uint64_t address(const Value &V) {
  const auto T = text(V, 18);
  llvm::StringRef S(T);
  uint64_t N;
  require(S.consume_front("0x") && !S.empty() && !S.getAsInteger(16, N),
          "expected hexadecimal address");
  return N;
}
double unit(const Value &V) {
  const auto N = V.getAsNumber();
  require(N && std::isfinite(*N) && *N >= 0 && *N <= 1, "invalid coverage");
  return *N;
}
Array gaps(uint32_t Mask) {
  Array Out;
  for (auto Name : flowGapNames(Mask))
    Out.push_back(Name);
  return Out;
}
Array pairs(const std::vector<std::pair<uint32_t, uint32_t>> &Pairs) {
  Array Out;
  for (const auto &[A, B] : Pairs)
    Out.push_back(Array{A, B});
  return Out;
}
std::vector<std::pair<uint32_t, uint32_t>>
pairs(const Value &V, uint64_t Max, const ComparisonControl &Control) {
  std::vector<std::pair<uint32_t, uint32_t>> Out;
  for (const auto &Item : array(V, Max)) {
    Control.check();
    const auto &P = array(Item, 2);
    require(P.size() == 2, "correspondence needs two coordinates");
    Out.emplace_back(count(P[0], NoIndex - 1), count(P[1], NoIndex - 1));
  }
  return Out;
}
} // namespace

Object flowRegionSettingsJSON(const FlowRegionSettings &S) {
  Object Out{{"enabled", S.Enabled},
             {"min_nodes", S.MinimumNodes},
             {"min_origins", S.MinimumOrigins},
             {"max_occurrences", S.MaxOccurrences},
             {"max_seeds", S.MaxSeeds},
             {"max_work", S.MaxWork},
             {"max_anchors", S.MaxAnchors},
             {"max_node_pairs", S.MaxNodePairs}};
  if (S.Fragments)
    Out["fragments"] = true;
  return Out;
}
FlowRegionSettings readFlowRegionSettings(const Value &V) {
  FlowRegionSettings S;
  if (V.getAsBoolean()) {
    S.Enabled = *V.getAsBoolean();
    return S;
  }
  const auto &O = object(V);
  for (const auto &[Key, Ignored] : O) {
    (void)Ignored;
    require(Key == "enabled" || Key == "min_nodes" || Key == "min_origins" ||
                Key == "max_occurrences" || Key == "max_seeds" ||
                Key == "max_work" || Key == "max_anchors" ||
                Key == "max_node_pairs" || Key == "fragments",
            "unknown setting " + Key.str());
  }
  S.Enabled = O.get("enabled") ? boolean(*O.get("enabled")) : true;
  if (const auto *Fragments = O.get("fragments"))
    S.Fragments = boolean(*Fragments);
  const auto Set = [&](llvm::StringRef Key, auto &To, uint64_t Max) {
    if (const auto *V = O.get(Key))
      To = count(*V, Max);
  };
  Set("min_nodes", S.MinimumNodes, limits::kMaxFlowRegionMinNodes);
  Set("min_origins", S.MinimumOrigins, limits::kMaxFlowRegionMinNodes);
  Set("max_occurrences", S.MaxOccurrences, limits::kMaxFlowRegionOccurrences);
  Set("max_seeds", S.MaxSeeds, limits::kMaxFlowRegionSeeds);
  Set("max_work", S.MaxWork, limits::kMaxFlowRegionWork);
  Set("max_anchors", S.MaxAnchors, limits::kMaxFlowRegionAnchors);
  Set("max_node_pairs", S.MaxNodePairs, limits::kMaxFlowRegionPairs);
  if (auto E = S.validate())
    throw std::invalid_argument(*E);
  return S;
}
Object flowRegionSummaryJSON(const FlowRegionAnalysis &R) {
  return Object{
      {"profile", R.Profile},
      {"state", R.State ? Value(stateName(*R.State)) : Value(nullptr)},
      {"stopped_phase", R.StoppedPhase},
      {"work", R.Work},
      {"primary_seeds", R.PrimarySeeds},
      {"secondary_seeds", R.SecondarySeeds},
      {"frequent_seed_groups", R.FrequentSeedGroups},
      {"weak_candidates", R.WeakCandidates},
      {"ambiguous_candidates", R.AmbiguousCandidates},
      {"different_candidates", R.DifferentCandidates},
      {"limited_graphs", R.LimitedGraphs},
      {"suppressed_groups", R.SuppressedGroups},
      {"primary_scope_complete", R.PrimaryScopeComplete},
      {"secondary_scope_complete", R.SecondaryScopeComplete},
      {"anchor_count", R.Anchors.size()},
      {"hypothesis_count", R.Hypotheses.size()},
      {"semantic_validation", "not_performed"}};
}
Object flowRegionAnchorJSON(const FlowRegionAnchor &A, bool Coordinates) {
  Object Out{{"id", A.Id},
             {"primary_function", hex(A.PrimaryFunction)},
             {"secondary_function", hex(A.SecondaryFunction)},
             {"primary_root", A.PrimaryRoot},
             {"secondary_root", A.SecondaryRoot},
             {"node_count", A.Nodes.size()},
             {"phi_block_count", A.PhiBlocks.size()},
             {"primary_origins", A.PrimaryOrigins},
             {"secondary_origins", A.SecondaryOrigins},
             {"unique_seed", A.UniqueSeed},
             {"gap_mask", A.Gaps},
             {"gaps", gaps(A.Gaps)},
             {"open_data_frontier", A.OpenDataFrontier},
             {"open_control_frontier", A.OpenControlFrontier}};
  if (A.Depth) {
    Out["depth"] = A.Depth;
    Out["frontier_count"] = A.Frontier.size();
    if (Coordinates)
      Out["frontier"] = pairs(A.Frontier);
  }
  if (Coordinates) {
    Out["nodes"] = pairs(A.Nodes);
    Out["phi_blocks"] = pairs(A.PhiBlocks);
  }
  return Out;
}
Object flowRegionHypothesisJSON(const FlowRegionHypothesis &H, bool Full) {
  Array PF, SF, Kinds;
  for (const auto &[From, To] :
       {std::pair{&H.PrimaryFunctions, &PF}, {&H.SecondaryFunctions, &SF}})
    for (size_t I = 0;
         I < From->size() && (Full || I < limits::kRegionFunctionPreview); ++I)
      To->push_back(hex((*From)[I]));
  for (auto K : H.Explanations)
    Kinds.push_back(regionKindName(K));
  Object Out{{"id", H.Id},
             {"primary_functions", std::move(PF)},
             {"secondary_functions", std::move(SF)},
             {"primary_function_count", H.PrimaryFunctions.size()},
             {"secondary_function_count", H.SecondaryFunctions.size()},
             {"anchor_count", H.Anchors.size()},
             {"explanations", std::move(Kinds)},
             {"primary_nodes", H.PrimaryNodes},
             {"secondary_nodes", H.SecondaryNodes},
             {"primary_coverage", H.PrimaryCoverage},
             {"secondary_coverage", H.SecondaryCoverage},
             {"gap_mask", H.Gaps},
             {"gaps", gaps(H.Gaps)},
             {"open_data_frontier", H.OpenDataFrontier},
             {"open_control_frontier", H.OpenControlFrontier},
             {"ambiguous", H.Ambiguous}};
  if (Full) {
    Array Anchors;
    for (auto I : H.Anchors)
      Anchors.push_back(I);
    Out["anchors"] = std::move(Anchors);
  }
  return Out;
}
void writeFlowRegionAnalysisFields(llvm::json::OStream &Out,
                                   const FlowRegionAnalysis &R,
                                   bool SummaryOnly) {
  const auto Summary = flowRegionSummaryJSON(R);
  std::vector<llvm::StringRef> Keys;
  for (const auto &[Key, Ignored] : Summary) {
    (void)Ignored;
    Keys.push_back(Key);
  }
  std::sort(Keys.begin(), Keys.end());
  for (auto Key : Keys)
    Out.attribute(Key, *Summary.get(Key));
  if (SummaryOnly)
    return;
  Out.attributeArray("anchors", [&] {
    for (const auto &A : R.Anchors)
      Out.value(flowRegionAnchorJSON(A, true));
  });
  Out.attributeArray("hypotheses", [&] {
    for (const auto &H : R.Hypotheses)
      Out.value(flowRegionHypothesisJSON(H, true));
  });
}
FlowRegionAnalysis readFlowRegionAnalysis(const Value &V,
                                          const FlowRegionSettings &Settings,
                                          const ComparisonControl &Control) {
  Control.check();
  const auto &O = object(V);
  const auto Profile = text(field(O, "profile"));
  const bool Fragments = isFlowFragmentProfile(Profile);
  require(Profile == FlowRegionProfile || isLLVMFlowRegionProfile(Profile) ||
              Profile == FlowFragmentProfile,
          "unsupported profile");
  require(Fragments == Settings.Fragments,
          "fragment profile disagrees with settings");
  require(text(field(O, "semantic_validation")) == "not_performed",
          "SSA correspondence does not establish semantic validation");
  FlowRegionAnalysis R;
  R.Profile = Profile;
  if (!field(O, "state").getAsNull()) {
    R.State = stateNamed(text(field(O, "state")));
    require(R.State.has_value(), "unknown analysis state");
  }
  R.StoppedPhase = text(field(O, "stopped_phase"));
  require(R.State && *R.State != ComparisonState::Complete
              ? R.StoppedPhase == "flow-regions:index-primary" ||
                    R.StoppedPhase == "flow-regions:index-secondary" ||
                    R.StoppedPhase == "flow-regions:anchors" ||
                    R.StoppedPhase == "flow-regions:groups"
              : R.StoppedPhase.empty(),
          "stop phase disagrees with analysis state");
  R.Work = count(field(O, "work"), Settings.MaxWork);
  R.PrimarySeeds = count(field(O, "primary_seeds"), Settings.MaxSeeds);
  R.SecondarySeeds = count(field(O, "secondary_seeds"), Settings.MaxSeeds);
  R.FrequentSeedGroups =
      count(field(O, "frequent_seed_groups"), Settings.MaxSeeds);
  R.WeakCandidates = count(field(O, "weak_candidates"), Settings.MaxWork);
  R.AmbiguousCandidates =
      count(field(O, "ambiguous_candidates"), Settings.MaxWork);
  R.DifferentCandidates =
      count(field(O, "different_candidates"), Settings.MaxWork);
  R.LimitedGraphs =
      count(field(O, "limited_graphs"), 2 * limits::kMaxFlowFunctions);
  R.SuppressedGroups =
      count(field(O, "suppressed_groups"), Settings.MaxAnchors);
  R.PrimaryScopeComplete = boolean(field(O, "primary_scope_complete"));
  R.SecondaryScopeComplete = boolean(field(O, "secondary_scope_complete"));
  uint64_t StoredPairs = 0;
  for (const auto &Value : array(field(O, "anchors"), Settings.MaxAnchors)) {
    Control.check();
    const auto &A = object(Value);
    FlowRegionAnchor Row;
    Row.Id = text(field(A, "id"));
    Row.PrimaryFunction = address(field(A, "primary_function"));
    Row.SecondaryFunction = address(field(A, "secondary_function"));
    Row.PrimaryRoot = count(field(A, "primary_root"), NoIndex - 1);
    Row.SecondaryRoot = count(field(A, "secondary_root"), NoIndex - 1);
    Row.Nodes =
        pairs(field(A, "nodes"), Settings.MaxNodePairs - StoredPairs, Control);
    StoredPairs += Row.Nodes.size();
    Row.PhiBlocks = pairs(field(A, "phi_blocks"),
                          Settings.MaxNodePairs - StoredPairs, Control);
    StoredPairs += Row.PhiBlocks.size();
    if (Fragments) {
      Row.Depth = count(field(A, "depth"), limits::kMaxFlowRounds);
      require(Row.Depth && Row.PhiBlocks.empty(),
              "fragment depth or PHI inventory is invalid");
      Row.Frontier = pairs(field(A, "frontier"),
                           Settings.MaxNodePairs - StoredPairs, Control);
      StoredPairs += Row.Frontier.size();
      require(Row.Frontier.size() == count(field(A, "frontier_count")),
              "fragment frontier count disagrees with coordinates");
    } else {
      require(!A.get("depth") && !A.get("frontier") && !A.get("frontier_count"),
              "legacy neighborhood contains fragment fields");
    }
    const uint64_t OriginAllowance = isLLVMFlowRegionProfile(R.Profile)
                                         ? limits::kMaxFlowNativeInstructions
                                         : Row.Nodes.size();
    Row.PrimaryOrigins = count(field(A, "primary_origins"), OriginAllowance);
    Row.SecondaryOrigins =
        count(field(A, "secondary_origins"), OriginAllowance);
    Row.UniqueSeed = boolean(field(A, "unique_seed"));
    Row.Gaps = count(field(A, "gap_mask"), AllFlowGaps);
    Row.OpenDataFrontier = boolean(field(A, "open_data_frontier"));
    Row.OpenControlFrontier = boolean(field(A, "open_control_frontier"));
    require(!Row.Nodes.empty() &&
                Row.Nodes.size() == count(field(A, "node_count")) &&
                Row.PhiBlocks.size() == count(field(A, "phi_block_count")),
            "anchor counts disagree with coordinates");
    R.Anchors.push_back(std::move(Row));
  }
  uint64_t GroupedAnchors = 0;
  for (const auto &Value : array(field(O, "hypotheses"), Settings.MaxAnchors)) {
    Control.check();
    const auto &A = object(Value);
    FlowRegionHypothesis H;
    H.Id = text(field(A, "id"));
    for (const auto &I :
         array(field(A, "anchors"),
               std::min<uint64_t>(limits::kMaxFlowRegionGroupAnchors,
                                  R.Anchors.size() - GroupedAnchors))) {
      Control.check();
      const auto N = count(I);
      require(N < R.Anchors.size(), "hypothesis names an absent anchor");
      H.Anchors.push_back(N);
    }
    GroupedAnchors += H.Anchors.size();
    for (const auto &[Key, To] :
         {std::pair{"primary_functions", &H.PrimaryFunctions},
          {"secondary_functions", &H.SecondaryFunctions}})
      for (const auto &F : array(field(A, Key), H.Anchors.size()))
        To->push_back(address(F));
    for (const auto &K : array(field(A, "explanations"),
                               unsigned(RegionKind::Redistribution) + 1)) {
      Control.check();
      const auto Kind = regionKindNamed(text(K));
      require(Kind.has_value(), "unknown relationship explanation");
      H.Explanations.push_back(*Kind);
    }
    H.PrimaryNodes = count(field(A, "primary_nodes"), Settings.MaxNodePairs);
    H.SecondaryNodes =
        count(field(A, "secondary_nodes"), Settings.MaxNodePairs);
    H.PrimaryCoverage = unit(field(A, "primary_coverage"));
    H.SecondaryCoverage = unit(field(A, "secondary_coverage"));
    H.Gaps = count(field(A, "gap_mask"), AllFlowGaps);
    H.OpenDataFrontier = boolean(field(A, "open_data_frontier"));
    H.OpenControlFrontier = boolean(field(A, "open_control_frontier"));
    H.Ambiguous = boolean(field(A, "ambiguous"));
    require(!H.Anchors.empty() &&
                H.Anchors.size() == count(field(A, "anchor_count")) &&
                H.PrimaryFunctions.size() ==
                    count(field(A, "primary_function_count")) &&
                H.SecondaryFunctions.size() ==
                    count(field(A, "secondary_function_count")),
            "hypothesis counts disagree with members");
    R.Hypotheses.push_back(std::move(H));
  }
  require(R.Anchors.size() == count(field(O, "anchor_count")) &&
              R.Hypotheses.size() == count(field(O, "hypothesis_count")),
          "analysis counts disagree with inventories");
  FlowRegionAnalysis Empty;
  Empty.Profile = R.Profile;
  require(R.State || R == Empty, "unstarted analysis contains evidence");
  require(Settings.Enabled || !R.State, "unrequested SSA region evidence");
  return R;
}
} // namespace neverd::diff
