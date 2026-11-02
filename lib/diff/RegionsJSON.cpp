//===- RegionsJSON.cpp - Region evidence serialization --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One strict settings and evidence contract for SDK, worker, CLI and saved
/// comparisons. Region coordinates bind only to the exact input revisions.
//===----------------------------------------------------------------------===//

#include "neverd/diff/RegionsJSON.h"

#include "neverd/diff/Comparison.h"

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
void require(bool Good, llvm::StringRef Message) {
  if (!Good)
    throw std::invalid_argument("regions: " + Message.str());
}
const Object &object(const Value &V) {
  require(V.getAsObject(), "expected an object");
  return *V.getAsObject();
}
const Value &field(const Object &O, llvm::StringRef Key) {
  const auto *V = O.get(Key);
  require(V, "missing field " + Key.str());
  return *V;
}
uint64_t count(const Value &V, uint64_t Max = UINT32_MAX) {
  const auto N = V.getAsInteger();
  require(N && *N >= 0 && uint64_t(*N) <= Max, "invalid bounded integer");
  return uint64_t(*N);
}
uint64_t count(const Object &O, llvm::StringRef Key, uint64_t Default,
               uint64_t Max) {
  return O.get(Key) ? count(*O.get(Key), Max) : Default;
}
bool boolean(const Value &V) {
  const auto B = V.getAsBoolean();
  require(B.has_value(), "expected boolean");
  return *B;
}
std::string text(const Value &V) {
  const auto S = V.getAsString();
  require(S.has_value(), "expected string");
  return S->str();
}
double unit(const Object &O, llvm::StringRef Key) {
  const auto N = field(O, Key).getAsNumber();
  require(N && std::isfinite(*N) && *N >= 0 && *N <= 1, "invalid unit score");
  return *N;
}
const Array &array(const Value &V, uint64_t Max) {
  const auto *A = V.getAsArray();
  require(A && A->size() <= Max, "invalid or oversized array");
  return *A;
}
Array indices(const std::vector<uint32_t> &Values) {
  Array Out;
  for (auto V : Values)
    Out.push_back(V);
  return Out;
}
std::vector<uint32_t> indices(const Value &V, uint64_t Max,
                              const ComparisonControl &Control) {
  std::vector<uint32_t> Out;
  for (const auto &I : array(V, Max)) {
    Control.check();
    Out.push_back(count(I));
  }
  return Out;
}
Array span(const RegionSpan &S) {
  return Array{S.Function, S.Block, S.FirstInstruction, S.InstructionCount};
}
RegionSpan span(const Value &V) {
  const auto &A = array(V, 4);
  require(A.size() == 4, "span needs four snapshot coordinates");
  return {uint32_t(count(A[0])), uint32_t(count(A[1])), uint32_t(count(A[2])),
          uint32_t(count(A[3]))};
}
Array explanations(const RegionHypothesis &H) {
  Array A;
  for (auto K : H.Explanations)
    A.push_back(regionKindName(K));
  return A;
}
Object hypothesis(const RegionHypothesis &H) {
  return Object{{"id", H.Id},
                {"anchors", indices(H.Anchors)},
                {"primary_functions", indices(H.PrimaryFunctions)},
                {"secondary_functions", indices(H.SecondaryFunctions)},
                {"explanations", explanations(H)},
                {"primary_instructions", H.PrimaryInstructions},
                {"secondary_instructions", H.SecondaryInstructions},
                {"primary_coverage", H.PrimaryCoverage},
                {"secondary_coverage", H.SecondaryCoverage},
                {"confidence", H.Confidence},
                {"ambiguous", H.Ambiguous},
                {"analysis_complete", H.AnalysisComplete}};
}
Object anchor(const RegionAnchor &A) {
  return Object{{"primary", span(A.Primary)},
                {"secondary", span(A.Secondary)},
                {"unique_seed", A.UniqueSeed}};
}
std::string hex(uint64_t V) { return "0x" + llvm::utohexstr(V, true); }
Object location(const Program &P, const RegionSpan &A) {
  const auto &F = P.Functions[A.Function];
  const auto &B = P.blocks(F)[A.Block];
  const auto &First = P.Instructions[B.FirstInstruction + A.FirstInstruction];
  const auto &Last = P.Instructions[B.FirstInstruction + A.FirstInstruction +
                                    A.InstructionCount - 1];
  return Object{{"function", hex(F.Address)},
                {"name", llvm::json::fixUTF8(F.Name)},
                {"block", hex(B.Address)},
                {"begin", hex(First.Address)},
                {"end", hex(Last.Address + Last.Size)},
                {"instructions", A.InstructionCount},
                {"revision", P.Identity.Revision}};
}
} // namespace

Object regionSettingsJSON(const RegionSettings &S) {
  Object O{{"enabled", S.Enabled},
           {"min_instructions", S.MinimumInstructions},
           {"min_repeated_instructions", S.MinimumRepeatedInstructions},
           {"max_occurrences", S.MaxOccurrences},
           {"max_seeds", S.MaxSeeds},
           {"max_work", S.MaxWork},
           {"max_anchors", S.MaxAnchors},
           {"max_hypotheses", S.MaxHypotheses}};
  if (S.Calls)
    O["calls"] = true;
  return O;
}
RegionSettings readRegionSettings(const Value &V) {
  RegionSettings S;
  if (V.getAsBoolean()) {
    S.Enabled = *V.getAsBoolean();
    return S;
  }
  const auto &O = object(V);
  for (const auto &[K, Ignore] : O) {
    (void)Ignore;
    require(K == "enabled" || K == "calls" || K == "min_instructions" ||
                K == "min_repeated_instructions" || K == "max_occurrences" ||
                K == "max_seeds" || K == "max_work" || K == "max_anchors" ||
                K == "max_hypotheses",
            "unknown setting " + K.str());
  }
  S.Enabled = O.get("enabled") ? boolean(*O.get("enabled")) : true;
  S.Calls = O.get("calls") ? boolean(*O.get("calls")) : false;
  S.MinimumInstructions = count(O, "min_instructions", S.MinimumInstructions,
                                limits::kMaxRegionMinInstructions);
  S.MinimumRepeatedInstructions =
      count(O, "min_repeated_instructions", S.MinimumRepeatedInstructions,
            limits::kMaxRegionMinInstructions);
  S.MaxOccurrences = count(O, "max_occurrences", S.MaxOccurrences,
                           limits::kMaxRegionOccurrences);
  S.MaxSeeds = count(O, "max_seeds", S.MaxSeeds, limits::kMaxRegionSeeds);
  S.MaxWork = count(O, "max_work", S.MaxWork, limits::kMaxRegionWork);
  S.MaxAnchors =
      count(O, "max_anchors", S.MaxAnchors, limits::kMaxRegionAnchors);
  S.MaxHypotheses =
      count(O, "max_hypotheses", S.MaxHypotheses, limits::kMaxRegionHypotheses);
  if (auto E = S.validate())
    throw std::invalid_argument(*E);
  return S;
}
void addRegionEvidenceFields(Object &O, bool Calls) {
  O["profile"] = regionProfile(Calls);
  if (Calls)
    O["call_evidence"] = "current_function_correspondence";
  else
    O.erase("call_evidence");
}
Object regionAnalysisJSON(const RegionAnalysis &R, bool SummaryOnly) {
  Object O{{"state", R.State ? Value(stateName(*R.State)) : Value(nullptr)},
           {"stopped_phase", R.StoppedPhase},
           {"work", R.Work},
           {"primary_seeds", R.PrimarySeeds},
           {"secondary_seeds", R.SecondarySeeds},
           {"primary_eligible_instructions", R.PrimaryEligibleInstructions},
           {"secondary_eligible_instructions", R.SecondaryEligibleInstructions},
           {"frequent_seed_groups", R.FrequentSeedGroups},
           {"suppressed_groups", R.SuppressedGroups},
           {"weak_candidates", R.WeakCandidates},
           {"anchor_count", R.Anchors.size()},
           {"hypothesis_count", R.Hypotheses.size()},
           {"semantic_validation", "not_performed"}};
  addRegionEvidenceFields(O, R.Calls);
  if (!SummaryOnly) {
    Array Anchors, Hypotheses;
    for (const auto &A : R.Anchors)
      Anchors.push_back(anchor(A));
    for (const auto &H : R.Hypotheses)
      Hypotheses.push_back(hypothesis(H));
    O["anchors"] = std::move(Anchors);
    O["hypotheses"] = std::move(Hypotheses);
  }
  return O;
}
void writeRegionAnalysisFields(llvm::json::OStream &J, const RegionAnalysis &R,
                               bool SummaryOnly) {
  const auto Metadata = regionAnalysisJSON(R, true);
  std::vector<llvm::StringRef> Keys;
  for (const auto &[Key, Value] : Metadata) {
    (void)Value;
    Keys.push_back(Key);
  }
  std::sort(Keys.begin(), Keys.end());
  for (const auto Key : Keys)
    J.attribute(Key, *Metadata.get(Key));
  if (SummaryOnly)
    return;
  J.attributeArray("anchors", [&] {
    for (const auto &A : R.Anchors)
      J.value(anchor(A));
  });
  J.attributeArray("hypotheses", [&] {
    for (const auto &H : R.Hypotheses)
      J.value(hypothesis(H));
  });
}
RegionAnalysis readRegionAnalysis(const Value &V,
                                  const RegionSettings &Settings,
                                  const ComparisonControl &Control) {
  Control.check();
  const auto &O = object(V);
  require(text(field(O, "profile")) == regionProfile(Settings.Calls),
          "region profile disagrees with its settings");
  RegionAnalysis R;
  R.Calls = Settings.Calls;
  if (R.Calls)
    require(text(field(O, "call_evidence")) ==
                "current_function_correspondence",
            "unsupported region call evidence");
  else
    require(!O.get("call_evidence"), "call evidence requires call regions");
  const auto &State = field(O, "state");
  if (!State.getAsNull()) {
    R.State = stateNamed(text(State));
    require(R.State.has_value(), "unknown stop state");
  }
  R.StoppedPhase = text(field(O, "stopped_phase"));
  R.Work = count(field(O, "work"), Settings.MaxWork);
  R.PrimarySeeds = count(field(O, "primary_seeds"), Settings.MaxSeeds);
  R.SecondarySeeds = count(field(O, "secondary_seeds"), Settings.MaxSeeds);
  R.PrimaryEligibleInstructions = count(
      field(O, "primary_eligible_instructions"), limits::kMaxInstructions);
  R.SecondaryEligibleInstructions = count(
      field(O, "secondary_eligible_instructions"), limits::kMaxInstructions);
  R.FrequentSeedGroups =
      count(field(O, "frequent_seed_groups"), Settings.MaxSeeds);
  R.SuppressedGroups =
      count(field(O, "suppressed_groups"), Settings.MaxAnchors);
  R.WeakCandidates = count(field(O, "weak_candidates"), Settings.MaxWork);
  require(text(field(O, "semantic_validation")) == "not_performed",
          "region sharing does not establish semantic validation");
  for (const auto &Value : array(field(O, "anchors"), Settings.MaxAnchors)) {
    Control.check();
    const auto &A = object(Value);
    R.Anchors.push_back({span(field(A, "primary")), span(field(A, "secondary")),
                         boolean(field(A, "unique_seed"))});
  }
  for (const auto &Value :
       array(field(O, "hypotheses"), Settings.MaxHypotheses)) {
    Control.check();
    const auto &A = object(Value);
    RegionHypothesis H;
    H.Id = text(field(A, "id"));
    require(H.Id.size() <= limits::kMaxRegionIdBytes,
            "region identifier too long");
    H.Anchors =
        indices(field(A, "anchors"), limits::kMaxRegionGroupAnchors, Control);
    H.PrimaryFunctions = indices(field(A, "primary_functions"),
                                 limits::kMaxRegionGroupAnchors, Control);
    H.SecondaryFunctions = indices(field(A, "secondary_functions"),
                                   limits::kMaxRegionGroupAnchors, Control);
    for (const auto &Kind : array(field(A, "explanations"),
                                  unsigned(RegionKind::Redistribution) + 1)) {
      Control.check();
      auto K = regionKindNamed(text(Kind));
      require(K.has_value(), "unknown explanation");
      H.Explanations.push_back(*K);
    }
    H.PrimaryInstructions =
        count(field(A, "primary_instructions"), limits::kMaxInstructions);
    H.SecondaryInstructions =
        count(field(A, "secondary_instructions"), limits::kMaxInstructions);
    H.PrimaryCoverage = unit(A, "primary_coverage");
    H.SecondaryCoverage = unit(A, "secondary_coverage");
    H.Confidence = unit(A, "confidence");
    H.Ambiguous = boolean(field(A, "ambiguous"));
    H.AnalysisComplete = boolean(field(A, "analysis_complete"));
    R.Hypotheses.push_back(std::move(H));
  }
  require(count(field(O, "anchor_count"), Settings.MaxAnchors) ==
                  R.Anchors.size() &&
              count(field(O, "hypothesis_count"), Settings.MaxHypotheses) ==
                  R.Hypotheses.size(),
          "inventory count mismatch");
  require((Settings.Enabled || !R.State) &&
              (R.State || (R.Anchors.empty() && R.Hypotheses.empty())),
          "unrequested region evidence");
  return R;
}
Object regionHypothesisJSON(const RegionHypothesis &H, const Program &P,
                            const Program &S) {
  auto O = hypothesis(H);
  O.erase("anchors");
  const auto Functions = [](const Program &Input,
                            const std::vector<uint32_t> &Items) {
    Array Out;
    for (auto I :
         llvm::ArrayRef(Items).take_front(limits::kRegionFunctionPreview)) {
      const auto &F = Input.Functions[I];
      Out.push_back(Object{{"address", hex(F.Address)},
                           {"name", llvm::json::fixUTF8(F.Name)}});
    }
    return Out;
  };
  O["primary_functions"] = Functions(P, H.PrimaryFunctions);
  O["secondary_functions"] = Functions(S, H.SecondaryFunctions);
  O["primary_function_count"] = H.PrimaryFunctions.size();
  O["secondary_function_count"] = H.SecondaryFunctions.size();
  O["anchor_count"] = H.Anchors.size();
  O["heuristic"] = true;
  O["semantic_validation"] = "not_performed";
  return O;
}
Object regionAnchorJSON(const RegionAnchor &A, const Program &P,
                        const Program &S) {
  return Object{{"primary", location(P, A.Primary)},
                {"secondary", location(S, A.Secondary)},
                {"unique_seed", A.UniqueSeed}};
}
} // namespace neverd::diff
