//===- ComparisonJSON.cpp - Saved binary comparisons ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Writing a comparison as a versioned JSON document, reading one back with
/// strict checks, and binding it to the programs it was made from.
///
//===----------------------------------------------------------------------===//

#include "neverd/diff/ComparisonJSON.h"

#include "Matching.h"

#include "neverd/diff/DataFlowJSON.h"
#include "neverd/diff/FlowRegionsJSON.h"
#include "neverd/diff/Limits.h"
#include "neverd/diff/RegionsJSON.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cmath>

namespace neverd::diff {

namespace {

std::string hex(uint64_t Value) { return "0x" + llvm::utohexstr(Value, true); }

std::string text(llvm::StringRef S) {
  return llvm::json::isUTF8(S) ? S.str() : llvm::json::fixUTF8(S);
}

void writeIdentity(llvm::json::OStream &J, const Program &P) {
  const ProgramIdentity &I = P.Identity;
  J.attribute("path", text(I.Path));
  J.attribute("sha256", I.SHA256);
  if (!I.ProjectInputName.empty())
    J.attributeObject("project", [&] {
      J.attribute("format", "nddb-v1");
      J.attribute("input_name", text(I.ProjectInputName));
      J.attribute("state_sha256", I.ProjectStateSHA256);
    });
  J.attribute("format", text(I.Format));
  J.attribute("architecture", text(I.Architecture));
  J.attribute("abi", text(I.ABI));
  J.attribute("normalization", text(I.Normalization));
  J.attribute("revision", text(I.Revision));
  J.attribute("bits", int64_t(I.Bits));
  if (I.Endian.empty())
    J.attribute("endian", nullptr);
  else
    J.attribute("endian", I.Endian);
  J.attribute("image_base", hex(I.ImageBase));
  J.attribute("entry_point", hex(I.EntryPoint));
  J.attribute("analysis", text(I.Analysis));
  J.attribute("discovery_complete", I.DiscoveryComplete);
}

void writeCoverage(llvm::json::OStream &J, const SideCoverage &C) {
  J.attribute("functions", int64_t(C.Functions));
  J.attribute("with_body", int64_t(C.WithBody));
  J.attribute("imports", int64_t(C.Imports));
  J.attribute("thunks", int64_t(C.Thunks));
  J.attribute("library", int64_t(C.Library));
  J.attribute("incomplete", int64_t(C.Incomplete));
  J.attribute("matched", int64_t(C.Matched));
  J.attribute("ambiguous", int64_t(C.Ambiguous));
  J.attribute("unmatched", int64_t(C.Unmatched));
  J.attribute("instructions", int64_t(C.Instructions));
  J.attribute("matched_instructions", int64_t(C.MatchedInstructions));
  J.attribute("discovery_complete", C.DiscoveryComplete);
}

void writeFunctions(llvm::json::OStream &J, const Program &P) {
  for (const Function &F : P.Functions)
    J.array([&] {
      J.value(hex(F.Address));
      J.value(int64_t(F.Size));
      J.value(text(F.Name));
      J.value(int64_t(F.Traits));
      J.value(int64_t(F.BlockCount));
      J.value(int64_t(P.instructionCount(F)));
    });
}

/// Reads typed fields of one JSON object, recording the first problem.
class Reader {
public:
  explicit Reader(std::string &Error, const ComparisonControl &Control)
      : Error(Error), Control(Control) {}

  bool failed() const {
    Control.check();
    return !Error.empty();
  }
  void record() {
    Control.check();
    if (++Records % limits::diff::kSavedComparisonProgressRecords == 0)
      Control.report("records", Records);
  }
  bool fail(const llvm::Twine &Message) {
    if (Error.empty())
      Error = Message.str();
    return false;
  }

  const llvm::json::Object *object(const llvm::json::Object &O,
                                   llvm::StringRef Key) {
    const llvm::json::Object *V = O.getObject(Key);
    if (!V)
      fail("\"" + Key + "\" must be an object");
    return V;
  }
  const llvm::json::Array *array(const llvm::json::Object &O,
                                 llvm::StringRef Key) {
    const llvm::json::Array *V = O.getArray(Key);
    if (!V)
      fail("\"" + Key + "\" must be an array");
    return V;
  }
  std::string string(const llvm::json::Object &O, llvm::StringRef Key) {
    if (const auto V = O.getString(Key))
      return V->str();
    fail("\"" + Key + "\" must be a string");
    return {};
  }
  bool boolean(const llvm::json::Object &O, llvm::StringRef Key) {
    if (const auto V = O.getBoolean(Key))
      return *V;
    fail("\"" + Key + "\" must be a boolean");
    return false;
  }
  uint64_t count(const llvm::json::Value &V, const llvm::Twine &What,
                 uint64_t Max = UINT32_MAX) {
    const auto N = V.getAsInteger();
    if (!N || *N < 0 || uint64_t(*N) > Max) {
      fail(What + " must be a whole number from 0 to " + llvm::Twine(Max));
      return 0;
    }
    return uint64_t(*N);
  }
  uint64_t count(const llvm::json::Object &O, llvm::StringRef Key,
                 uint64_t Max = UINT32_MAX) {
    if (const llvm::json::Value *V = O.get(Key))
      return count(*V, "\"" + Key + "\"", Max);
    fail("\"" + Key + "\" is missing");
    return 0;
  }
  double unit(const llvm::json::Object &O, llvm::StringRef Key) {
    const auto V = O.getNumber(Key);
    if (!V || !std::isfinite(*V) || *V < 0 || *V > 1) {
      fail("\"" + Key + "\" must be a number from 0 to 1");
      return 0;
    }
    return *V;
  }
  uint64_t address(const llvm::json::Value &V, const llvm::Twine &What) {
    const auto S = V.getAsString();
    uint64_t A = 0;
    if (!S || !S->starts_with("0x") || S->size() > 18 ||
        S->drop_front(2).getAsInteger(16, A)) {
      fail(What + " must be a hexadecimal address such as \"0x401000\"");
      return 0;
    }
    return A;
  }
  uint64_t address(const llvm::json::Object &O, llvm::StringRef Key) {
    if (const llvm::json::Value *V = O.get(Key))
      return address(*V, "\"" + Key + "\"");
    fail("\"" + Key + "\" is missing");
    return 0;
  }

private:
  std::string &Error;
  const ComparisonControl &Control;
  uint64_t Records = 0;
};

void readIdentity(Reader &In, const llvm::json::Object &O, ProgramIdentity &I) {
  I.Path = In.string(O, "path");
  I.SHA256 = In.string(O, "sha256");
  if (O.get("project")) {
    if (const auto *Project = In.object(O, "project")) {
      if (In.string(*Project, "format") != "nddb-v1")
        In.fail("unsupported project provenance format");
      I.ProjectInputName = In.string(*Project, "input_name");
      I.ProjectStateSHA256 = In.string(*Project, "state_sha256");
      if (I.ProjectInputName.empty() || I.ProjectStateSHA256.size() != 64 ||
          !llvm::all_of(I.ProjectStateSHA256,
                        [](char C) { return llvm::isHexDigit(C); }))
        In.fail("invalid project provenance");
    }
  }
  I.Format = In.string(O, "format");
  I.Architecture = In.string(O, "architecture");
  I.ABI = In.string(O, "abi");
  I.Normalization = In.string(O, "normalization");
  I.Revision = In.string(O, "revision");
  I.Bits = static_cast<unsigned>(In.count(O, "bits", 128));
  if (const llvm::json::Value *E = O.get("endian")) {
    if (const auto S = E->getAsString())
      I.Endian = S->str();
    else if (!E->getAsNull())
      In.fail("\"endian\" must be a string or null");
  }
  I.ImageBase = In.address(O, "image_base");
  I.EntryPoint = In.address(O, "entry_point");
  I.Analysis = In.string(O, "analysis");
  I.DiscoveryComplete = In.boolean(O, "discovery_complete");
  if (!I.SHA256.empty() &&
      (I.SHA256.size() != 64 ||
       !llvm::all_of(I.SHA256, [](char C) { return llvm::isHexDigit(C); })))
    In.fail("\"sha256\" must be 64 hexadecimal digits");
}

void readCoverage(Reader &In, const llvm::json::Object &O, SideCoverage &C) {
  C.Functions = uint32_t(In.count(O, "functions"));
  C.WithBody = uint32_t(In.count(O, "with_body"));
  C.Imports = uint32_t(In.count(O, "imports"));
  C.Thunks = uint32_t(In.count(O, "thunks"));
  C.Library = uint32_t(In.count(O, "library"));
  C.Incomplete = uint32_t(In.count(O, "incomplete"));
  C.Matched = uint32_t(In.count(O, "matched"));
  C.Ambiguous = uint32_t(In.count(O, "ambiguous"));
  C.Unmatched = uint32_t(In.count(O, "unmatched"));
  C.Instructions = In.count(O, "instructions", INT64_MAX);
  C.MatchedInstructions = In.count(O, "matched_instructions", INT64_MAX);
  C.DiscoveryComplete = In.boolean(O, "discovery_complete");
}

void readFunctions(Reader &In, const llvm::json::Array &A,
                   std::vector<SavedFunction> &Out) {
  for (const llvm::json::Value &V : A) {
    In.record();
    const llvm::json::Array *Row = V.getAsArray();
    if (!Row || Row->size() != 6) {
      In.fail("a function must be [address, size, name, traits, blocks, "
              "instructions]");
      return;
    }
    SavedFunction F;
    F.Address = In.address((*Row)[0], "a function's address");
    F.Size = In.count((*Row)[1], "a function's size", INT64_MAX);
    if (const auto Name = (*Row)[2].getAsString())
      F.Name = Name->str();
    else
      In.fail("a function's name must be a string");
    F.Traits = uint16_t(In.count((*Row)[3], "a function's traits", 0xffff));
    F.Blocks = uint32_t(In.count((*Row)[4], "a function's block count"));
    F.Instructions =
        In.count((*Row)[5], "a function's instruction count", INT64_MAX);
    if (!Out.empty() && Out.back().Address >= F.Address)
      In.fail("functions must be listed in ascending address order");
    if (In.failed())
      return;
    Out.push_back(std::move(F));
  }
}

std::optional<std::string> decisionConflict(const SavedComparison &Saved,
                                            const ComparisonControl &Control) {
  Control.report("decisions");
  const auto Rejected = rejectedPairs(Saved.Overrides, Control);
  for (const auto &M : Saved.Matches) {
    Control.check();
    if (M.Match.Step == FunctionStep::InlinedTailCall)
      return "inlined_tail_call can only describe ambiguous candidates: " +
             hex(M.Primary) + " <-> " + hex(M.Secondary);
    if (std::binary_search(Rejected.begin(), Rejected.end(),
                           std::pair{M.Primary, M.Secondary}))
      return "a saved match contradicts an effective unmatch decision: " +
             hex(M.Primary) + " <-> " + hex(M.Secondary);
  }
  return std::nullopt;
}

} // namespace

void writeComparison(llvm::raw_ostream &OS, const ComparisonResult &R,
                     const Program &P, const Program &S, bool SummaryOnly) {
  llvm::json::OStream J(OS);
  J.object([&] {
    J.attribute("schema",
                SummaryOnly ? "neverd.bindiff.summary" : ComparisonSchema);
    J.attribute("schema_version", int64_t(ComparisonSchemaVersion));
    J.attribute("matcher_version", int64_t(MatcherVersion));
    J.attribute("state", stateName(R.State));
    J.attribute("stopped_phase", R.StoppedPhase);
    J.attributeObject("settings", [&] {
      J.attribute("use_names", R.Settings.UseNames);
      J.attribute("approximate", R.Settings.Approximate);
      J.attribute("min_similarity", R.Settings.MinimumSimilarity);
      J.attribute("min_context_similarity",
                  R.Settings.MinimumContextSimilarity);
      J.attribute("ambiguity_margin", R.Settings.AmbiguityMargin);
      J.attribute("max_candidates", int64_t(R.Settings.MaxCandidates));
      J.attribute("regions", regionSettingsJSON(R.Settings.Regions));
      J.attribute("dataflow", dataFlowSettingsJSON(R.Settings.DataFlow));
      J.attribute("flow_regions",
                  flowRegionSettingsJSON(R.Settings.FlowRegions));
    });
    J.attributeObject("regions", [&] {
      writeRegionAnalysisFields(J, R.Regions, SummaryOnly);
    });
    J.attributeObject("flow_regions", [&] {
      writeFlowRegionAnalysisFields(J, R.FlowRegions, SummaryOnly);
    });
    J.attributeObject("dataflow", [&] {
      for (const auto &[Side, Evidence] :
           {std::pair{"primary", R.PrimaryDataFlow.get()},
            {"secondary", R.SecondaryDataFlow.get()}}) {
        J.attributeBegin(Side);
        if (SummaryOnly)
          J.value(dataFlowEvidenceSummary(Evidence));
        else
          writeDataFlowEvidence(J, Evidence);
        J.attributeEnd();
      }
    });
    J.attributeObject("primary", [&] { writeIdentity(J, P); });
    J.attributeObject("secondary", [&] { writeIdentity(J, S); });
    J.attribute("similarity", R.Similarity);
    J.attribute("confidence", R.Confidence);
    J.attributeObject("coverage", [&] {
      J.attributeObject("primary",
                        [&] { writeCoverage(J, R.PrimaryCoverage); });
      J.attributeObject("secondary",
                        [&] { writeCoverage(J, R.SecondaryCoverage); });
    });
    J.attributeObject("counts", [&] {
      uint64_t Changed = 0, Unchanged = 0, Unmeasured = 0, Incomplete = 0,
               IdentityOnly = 0, IncompletePairs = 0, Uncertain = 0,
               ReferenceGaps = 0;
      for (const auto &M : R.Matches) {
        IncompletePairs += M.Measured && !M.IdentityOnly && !M.AnalysisComplete;
        ReferenceGaps +=
            M.Measured && !M.IdentityOnly && !M.ReferenceComparisonComplete;
        if (!M.Measured)
          ++Unmeasured;
        else if (M.IdentityOnly)
          ++IdentityOnly;
        else if (M.Changes & ~MovementChanges)
          ++Changed;
        else if (M.identical())
          ++Unchanged;
        else if (!M.AnalysisComplete)
          ++Incomplete;
        else
          ++Uncertain;
      }
      J.attribute("matches", uint64_t(R.Matches.size()));
      J.attribute("changed", Changed);
      J.attribute("normalized_unchanged", Unchanged);
      J.attribute("unmeasured", Unmeasured);
      J.attribute("identity_only", IdentityOnly);
      J.attribute("incomplete_pairs", IncompletePairs);
      J.attribute("incomplete_without_observed_change", Incomplete);
      J.attribute("reference_gap_pairs", ReferenceGaps);
      J.attribute("uncertain_without_observed_change", Uncertain);
      J.attribute("ambiguities", uint64_t(R.Ambiguities.size()));
      J.attribute("overrides", uint64_t(R.Overrides.size()));
      J.attribute("review_records", uint64_t(R.Reviews.Records.size()));
    });
    if (SummaryOnly)
      J.attribute("reviews",
                  llvm::json::Object{{"profile", ReviewJournalProfile},
                                     {"head", R.Reviews.head()},
                                     {"total", R.Reviews.Records.size()}});
    else
      J.attribute("reviews", reviewJournalJSON(R.Reviews));
    if (!SummaryOnly) {
      J.attributeArray("matches", [&] {
        for (const FunctionMatch &M : R.Matches)
          J.object([&] {
            J.attribute("primary", hex(P.Functions[M.Primary].Address));
            J.attribute("secondary", hex(S.Functions[M.Secondary].Address));
            J.attribute("step", stepName(M.Step));
            J.attribute("context", M.FromContext);
            J.attribute("confirmed", M.Confirmed);
            J.attribute("measured", M.Measured);
            J.attribute("identity_only", M.IdentityOnly);
            J.attribute("analysis_complete", M.AnalysisComplete);
            J.attribute("reference_comparison_complete",
                        M.ReferenceComparisonComplete);
            J.attribute("similarity", M.Similarity);
            J.attribute("confidence", M.Confidence);
            J.attributeArray("changes", [&] {
              for (llvm::StringRef Name : changeNames(M.Changes))
                J.value(Name);
            });
            J.attribute("blocks", int64_t(M.MatchedBlocks));
            J.attribute("edges", int64_t(M.MatchedEdges));
            J.attribute("equal_instructions", int64_t(M.EqualInstructions));
          });
      });
      J.attributeArray("ambiguities", [&] {
        for (const Ambiguity &A : R.Ambiguities)
          J.object([&] {
            J.attribute("step", stepName(A.Step));
            J.attributeArray("primary", [&] {
              for (uint32_t F : A.Primary)
                J.value(hex(P.Functions[F].Address));
            });
            J.attributeArray("secondary", [&] {
              for (uint32_t F : A.Secondary)
                J.value(hex(S.Functions[F].Address));
            });
          });
      });
      J.attributeArray("overrides", [&] {
        for (const Override &O : R.Overrides)
          J.object([&] {
            J.attribute("kind", overrideName(O.Kind));
            J.attribute("primary", hex(O.Primary));
            J.attribute("secondary", hex(O.Secondary));
          });
      });
    }
    J.attributeArray("warnings", [&] {
      for (const std::string &W : R.Warnings)
        J.value(text(W));
    });
    J.attributeArray("timings", [&] {
      for (const PhaseTime &T : R.Timings)
        J.object([&] {
          J.attribute("phase", T.Phase);
          J.attribute("milliseconds", T.Milliseconds);
        });
    });
    if (!SummaryOnly)
      J.attributeObject("functions", [&] {
        J.attributeArray("primary", [&] { writeFunctions(J, P); });
        J.attributeArray("secondary", [&] { writeFunctions(J, S); });
      });
  });
}

std::optional<SavedComparison>
readComparison(llvm::StringRef Text, std::string &Error,
               const ComparisonControl &Control) {
  Error.clear();
  if (Text.size() > limits::diff::kMaxSavedComparisonBytes) {
    Error = "the saved comparison is larger than " +
            std::to_string(limits::diff::kMaxSavedComparisonBytes) + " bytes";
    return std::nullopt;
  }
  Control.report("parse", 0, Text.size());
  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Text);
  if (!Parsed) {
    Error = "not JSON: " + llvm::toString(Parsed.takeError());
    Control.report("parse", Text.size(), Text.size());
    return std::nullopt;
  }
  // Consume a parser failure (or check its success) before a stop can unwind
  // the Expected. LLVM's checked-error builds reject an unchecked destructor.
  Control.report("parse", Text.size(), Text.size());
  const llvm::json::Object *Root = Parsed->getAsObject();
  if (!Root) {
    Error = "a saved comparison must be a JSON object";
    return std::nullopt;
  }
  Control.report("records");
  Reader In(Error, Control);
  SavedComparison C;
  if (In.string(*Root, "schema") != ComparisonSchema) {
    if (Error.empty() || Error.find("schema") != std::string::npos) {
      Error.clear();
      In.fail(llvm::Twine("not a saved comparison: \"schema\" must be \"") +
              ComparisonSchema + "\"");
    }
    return std::nullopt;
  }
  const uint64_t Version = In.count(*Root, "schema_version");
  if (In.failed())
    return std::nullopt;
  if (Version < OldestComparisonSchemaVersion ||
      Version > ComparisonSchemaVersion) {
    In.fail("saved comparison schema version " + llvm::Twine(Version) +
            " is not supported; this build reads versions " +
            llvm::Twine(OldestComparisonSchemaVersion) + " through " +
            llvm::Twine(ComparisonSchemaVersion));
    return std::nullopt;
  }
  C.MatcherVersion = unsigned(In.count(*Root, "matcher_version"));
  const std::string State = In.string(*Root, "state");
  if (State == "complete")
    C.State = ComparisonState::Complete;
  else if (State == "cancelled")
    C.State = ComparisonState::Cancelled;
  else if (State == "timed_out")
    C.State = ComparisonState::TimedOut;
  else if (State == "resource_limited")
    C.State = ComparisonState::ResourceLimited;
  else
    In.fail("\"state\" must be \"complete\", \"cancelled\" or \"timed_out\"");
  C.StoppedPhase = In.string(*Root, "stopped_phase");
  if (const llvm::json::Object *O = In.object(*Root, "settings")) {
    C.Settings.UseNames = In.boolean(*O, "use_names");
    C.Settings.Approximate = In.boolean(*O, "approximate");
    C.Settings.MinimumSimilarity = In.unit(*O, "min_similarity");
    C.Settings.MinimumContextSimilarity = In.unit(*O, "min_context_similarity");
    C.Settings.AmbiguityMargin = In.unit(*O, "ambiguity_margin");
    C.Settings.MaxCandidates =
        unsigned(In.count(*O, "max_candidates", limits::diff::kMaxCandidates));
    if (const auto *Regions = O->get("regions")) {
      try {
        C.Settings.Regions = readRegionSettings(*Regions);
      } catch (const ComparisonInterrupted &) {
        throw;
      } catch (const std::exception &E) {
        In.fail(E.what());
      }
    }
    if (const auto *Flow = O->get("dataflow")) {
      try {
        C.Settings.DataFlow = readDataFlowSettings(*Flow);
      } catch (const ComparisonInterrupted &) {
        throw;
      } catch (const std::exception &E) {
        In.fail(E.what());
      }
    }
    if (const auto *FlowRegions = O->get("flow_regions")) {
      try {
        C.Settings.FlowRegions = readFlowRegionSettings(*FlowRegions);
      } catch (const ComparisonInterrupted &) {
        throw;
      } catch (const std::exception &E) {
        In.fail(E.what());
      }
    }
    if (!In.failed())
      if (const auto Invalid = C.Settings.validate())
        In.fail(*Invalid);
  }
  if (const llvm::json::Object *O = In.object(*Root, "primary"))
    readIdentity(In, *O, C.Primary);
  if (const llvm::json::Object *O = In.object(*Root, "secondary"))
    readIdentity(In, *O, C.Secondary);
  C.Similarity = In.unit(*Root, "similarity");
  C.Confidence = In.unit(*Root, "confidence");
  if (const llvm::json::Object *O = In.object(*Root, "coverage")) {
    if (const llvm::json::Object *P = In.object(*O, "primary"))
      readCoverage(In, *P, C.PrimaryCoverage);
    if (const llvm::json::Object *S = In.object(*O, "secondary"))
      readCoverage(In, *S, C.SecondaryCoverage);
  }
  if (const llvm::json::Object *O = In.object(*Root, "functions")) {
    if (const llvm::json::Array *A = In.array(*O, "primary"))
      readFunctions(In, *A, C.PrimaryFunctions);
    if (const llvm::json::Array *A = In.array(*O, "secondary"))
      readFunctions(In, *A, C.SecondaryFunctions);
  }
  if (!In.failed()) {
    if (const auto *Regions = Root->get("flow_regions")) {
      try {
        C.FlowRegions =
            readFlowRegionAnalysis(*Regions, C.Settings.FlowRegions, Control);
      } catch (const ComparisonInterrupted &) {
        throw;
      } catch (const std::exception &E) {
        In.fail(E.what());
      }
    }
    if (C.FlowRegions.Profile !=
        flowRegionProfile(C.Settings.DataFlow.Profile,
                          C.Settings.FlowRegions.Fragments))
      In.fail(
          "SSA region profile disagrees with requested dependency evidence");
    if (C.Settings.FlowRegions.Enabled &&
        C.State == ComparisonState::Complete &&
        C.FlowRegions.State != ComparisonState::Complete)
      In.fail("completed comparison has no completed SSA region analysis");
  }
  if (In.failed())
    return std::nullopt;

  // Every address must name a listed function, and a function pairs once.
  const auto Listed = [](const std::vector<SavedFunction> &Fs, uint64_t A) {
    return std::binary_search(
        Fs.begin(), Fs.end(), A, [](const auto &L, const auto &R) {
          if constexpr (std::is_same_v<std::decay_t<decltype(L)>,
                                       SavedFunction>)
            return L.Address < R;
          else
            return L < R.Address;
        });
  };
  llvm::DenseSet<uint64_t> PairedP, PairedS;
  if (const llvm::json::Array *A = In.array(*Root, "matches"))
    for (const llvm::json::Value &V : *A) {
      In.record();
      const llvm::json::Object *O = V.getAsObject();
      if (!O) {
        In.fail("a match must be an object");
        break;
      }
      SavedMatch M;
      M.Primary = In.address(*O, "primary");
      M.Secondary = In.address(*O, "secondary");
      const std::string Step = In.string(*O, "step");
      if (const auto S = functionStepNamed(Step))
        M.Match.Step = *S;
      else if (!In.failed())
        In.fail("unknown matching step \"" + Step + "\"");
      M.Match.FromContext = In.boolean(*O, "context");
      M.Match.Confirmed = In.boolean(*O, "confirmed");
      M.Match.Measured = In.boolean(*O, "measured");
      M.Match.IdentityOnly = In.boolean(*O, "identity_only");
      M.Match.AnalysisComplete = In.boolean(*O, "analysis_complete");
      // Older schemas did not distinguish unknown references from changes.
      M.Match.ReferenceComparisonComplete =
          Version >= ReferenceEvidenceSchemaVersion &&
          In.boolean(*O, "reference_comparison_complete");
      M.Match.Similarity = In.unit(*O, "similarity");
      M.Match.Confidence = In.unit(*O, "confidence");
      if (const llvm::json::Array *Changes = In.array(*O, "changes"))
        for (const llvm::json::Value &Name : *Changes) {
          const auto S = Name.getAsString();
          const auto Kind = S ? changeNamed(*S) : std::nullopt;
          if (!Kind) {
            In.fail("unknown change kind in a match");
            break;
          }
          M.Match.Changes |= *Kind;
        }
      M.Match.MatchedBlocks = uint32_t(In.count(*O, "blocks"));
      M.Match.MatchedEdges = uint32_t(In.count(*O, "edges"));
      M.Match.EqualInstructions = uint32_t(In.count(*O, "equal_instructions"));
      if (In.failed())
        break;
      if (!Listed(C.PrimaryFunctions, M.Primary) ||
          !Listed(C.SecondaryFunctions, M.Secondary)) {
        In.fail("a match names a function the comparison does not list: " +
                hex(M.Primary) + " <-> " + hex(M.Secondary));
        break;
      }
      if (!PairedP.insert(M.Primary).second ||
          !PairedS.insert(M.Secondary).second) {
        In.fail("a function is paired twice: " + hex(M.Primary) + " <-> " +
                hex(M.Secondary));
        break;
      }
      C.Matches.push_back(std::move(M));
    }
  if (const llvm::json::Array *A = In.array(*Root, "ambiguities"))
    for (const llvm::json::Value &V : *A) {
      In.record();
      const llvm::json::Object *O = V.getAsObject();
      if (!O) {
        In.fail("an ambiguity must be an object");
        break;
      }
      SavedAmbiguity Amb;
      const std::string Step = In.string(*O, "step");
      if (const auto S = functionStepNamed(Step))
        Amb.Step = *S;
      else if (!In.failed())
        In.fail("unknown matching step \"" + Step + "\"");
      for (bool Primary : {true, false}) {
        const llvm::json::Array *Members =
            In.array(*O, Primary ? "primary" : "secondary");
        if (!Members)
          break;
        for (const llvm::json::Value &M : *Members) {
          const uint64_t Address = In.address(M, "an ambiguous function");
          if (In.failed())
            break;
          if (!Listed(Primary ? C.PrimaryFunctions : C.SecondaryFunctions,
                      Address)) {
            In.fail("an ambiguity names a function the comparison does not "
                    "list: " +
                    hex(Address));
            break;
          }
          if ((Primary ? PairedP : PairedS).contains(Address)) {
            In.fail("an ambiguity names a paired function: " + hex(Address));
            break;
          }
          (Primary ? Amb.Primary : Amb.Secondary).push_back(Address);
        }
      }
      if (In.failed())
        break;
      if (Amb.Primary.empty() || Amb.Secondary.empty()) {
        In.fail("an ambiguity must name functions on both sides");
        break;
      }
      C.Ambiguities.push_back(std::move(Amb));
    }
  if (const llvm::json::Array *A = In.array(*Root, "overrides")) {
    if (A->size() > limits::diff::kMaxSavedOverrides)
      In.fail("more than " + llvm::Twine(limits::diff::kMaxSavedOverrides) +
              " overrides");
    for (const llvm::json::Value &V : *A) {
      In.record();
      if (In.failed())
        break;
      const llvm::json::Object *O = V.getAsObject();
      if (!O) {
        In.fail("an override must be an object");
        break;
      }
      Override Ov;
      const std::string Kind = In.string(*O, "kind");
      if (const auto K = overrideNamed(Kind))
        Ov.Kind = *K;
      else if (!In.failed())
        In.fail("unknown override \"" + Kind + "\"");
      Ov.Primary = In.address(*O, "primary");
      Ov.Secondary = In.address(*O, "secondary");
      C.Overrides.push_back(Ov);
    }
  }
  if (const llvm::json::Array *A = In.array(*Root, "warnings"))
    for (const llvm::json::Value &V : *A) {
      In.record();
      if (const auto S = V.getAsString())
        C.Warnings.push_back(S->str());
      else {
        In.fail("a warning must be a string");
        break;
      }
    }
  if (const llvm::json::Array *A = In.array(*Root, "timings"))
    for (const auto &V : *A) {
      In.record();
      const auto *O = V.getAsObject();
      if (!O) {
        In.fail("a timing must be an object");
        break;
      }
      const auto MS = O->getNumber("milliseconds");
      if (!MS || !std::isfinite(*MS) || *MS < 0) {
        In.fail("a timing must have finite nonnegative milliseconds");
        break;
      }
      C.Timings.push_back({In.string(*O, "phase"), *MS});
    }
  if (!In.failed())
    if (const auto *Regions = Root->get("regions")) {
      try {
        C.Regions = readRegionAnalysis(*Regions, C.Settings.Regions, Control);
      } catch (const ComparisonInterrupted &) {
        throw;
      } catch (const std::exception &E) {
        In.fail(E.what());
      }
    }
  if (!In.failed()) {
    const auto *Flow = Root->getObject("dataflow");
    if (Root->get("dataflow") && !Flow)
      In.fail("dataflow must be an object");
    if (!Flow && C.Settings.DataFlow.Enabled)
      In.fail("enabled dataflow has no saved collection");
    if (Flow) {
      try {
        if (!Flow->get("primary") || !Flow->get("secondary"))
          In.fail("dataflow requires both saved collections");
        else {
          C.PrimaryDataFlow = readDataFlowEvidence(
              *Flow->get("primary"), C.Settings.DataFlow, Control);
          C.SecondaryDataFlow = readDataFlowEvidence(
              *Flow->get("secondary"), C.Settings.DataFlow, Control);
        }
      } catch (const ComparisonInterrupted &) {
        throw;
      } catch (const std::exception &E) {
        In.fail(E.what());
      }
    }
  }
  if (!In.failed()) {
    const auto *Reviews = Root->get("reviews");
    if (Version == OldestComparisonSchemaVersion) {
      if (Reviews)
        In.fail("legacy comparison schema cannot contain review history");
    } else if (!Reviews) {
      In.fail("saved comparison requires review history");
    } else {
      std::string ReviewError;
      if (auto Journal = readReviewJournal(*Reviews, C.Primary, C.Secondary,
                                           ReviewError, Control))
        C.Reviews = std::move(*Journal);
      else
        In.fail(ReviewError);
    }
  }
  if (In.failed())
    return std::nullopt;
  if (const auto Conflict = decisionConflict(C, Control)) {
    In.fail(*Conflict);
    return std::nullopt;
  }
  Control.check();
  return C;
}

llvm::StringRef bindFailureName(BindFailure Failure) {
  switch (Failure) {
  case BindFailure::StaleInput:
    return "stale_input";
  case BindFailure::StaleAnalysis:
    return "stale_analysis";
  case BindFailure::MissingFunction:
    return "missing_function";
  case BindFailure::InvalidEvidence:
    return "invalid_evidence";
  }
  return {};
}

std::optional<std::string>
bindComparison(const SavedComparison &Saved, const Program &Primary,
               const Program &Secondary, ComparisonResult &R,
               BindFailure *Failure, const ComparisonControl &Control) {
  Control.report("identity");
  const auto Fail = [&](BindFailure Why, std::string Message) {
    if (Failure)
      *Failure = Why;
    return std::optional<std::string>(std::move(Message));
  };
  for (const auto &[Name, Was, Now] :
       {std::tuple<const char *, const ProgramIdentity *,
                   const ProgramIdentity *>{"primary", &Saved.Primary,
                                            &Primary.Identity},
        {"secondary", &Saved.Secondary, &Secondary.Identity}}) {
    if (Was->SHA256.empty() || Now->SHA256.empty())
      return Fail(BindFailure::StaleInput,
                  std::string("the ") + Name +
                      " input's file hash is unknown, so it cannot be "
                      "checked against the comparison");
    if (Was->SHA256 != Now->SHA256)
      return Fail(BindFailure::StaleInput,
                  std::string("the ") + Name + " input changed: SHA-256 " +
                      Now->SHA256 + ", the comparison was made from " +
                      Was->SHA256);
    if (programAnalysisIdentity(*Was) != programAnalysisIdentity(*Now)) {
      if (Was->Analysis == Now->Analysis)
        return Fail(
            BindFailure::StaleAnalysis,
            std::string("the ") + Name +
                " input's analyzed snapshot no longer matches the saved "
                "comparison; run a new comparison to use the current "
                "analysis");
      return Fail(BindFailure::StaleAnalysis,
                  std::string("the ") + Name +
                      " input was analyzed with other settings: \"" +
                      Now->Analysis + "\", the comparison used \"" +
                      Was->Analysis + "\"");
    }
  }
  if (const auto Conflict = decisionConflict(Saved, Control))
    return Fail(BindFailure::InvalidEvidence, *Conflict);
  const auto InventoryAgrees = [&](const std::vector<SavedFunction> &Was,
                                   const Program &Now) {
    if (Was.size() != Now.Functions.size())
      return false;
    for (size_t I = 0; I < Was.size(); ++I) {
      Control.check();
      const auto &A = Was[I];
      const auto &B = Now.Functions[I];
      if (A.Address != B.Address || A.Size != B.Size ||
          A.Name != text(B.Name) || A.Traits != B.Traits ||
          A.Blocks != B.BlockCount || A.Instructions != Now.instructionCount(B))
        return false;
    }
    return true;
  };
  if (!InventoryAgrees(Saved.PrimaryFunctions, Primary) ||
      !InventoryAgrees(Saved.SecondaryFunctions, Secondary))
    return Fail(BindFailure::InvalidEvidence,
                "saved function inventory disagrees with decoded evidence");
  Control.report("dataflow");
  for (const auto &[Was, Now] :
       {std::pair{Saved.PrimaryDataFlow.get(), &Primary},
        {Saved.SecondaryDataFlow.get(), &Secondary}})
    if (auto Error = bindDataFlowEvidence(
            Was, Now->DataFlow.get(), Saved.Settings.DataFlow, *Now, Control))
      return Fail(BindFailure::InvalidEvidence, *Error);
  Control.report("reviews");
  ComparisonResult Out;
  if (const auto Error = validateReviewJournal(Saved.Reviews, Primary.Identity,
                                               Secondary.Identity, Control))
    return Fail(BindFailure::InvalidEvidence, *Error);
  for (const auto &Record : Saved.Reviews.Records)
    if (!Primary.functionAt(Record.Primary) ||
        !Secondary.functionAt(Record.Secondary))
      return Fail(BindFailure::MissingFunction,
                  "review history names a missing function");
  Out.State = Saved.State;
  Out.StoppedPhase = Saved.StoppedPhase;
  Out.Settings = Saved.Settings;
  Out.PrimaryDataFlow = Saved.PrimaryDataFlow;
  Out.SecondaryDataFlow = Saved.SecondaryDataFlow;
  Out.Overrides = Saved.Overrides;
  Out.Reviews = Saved.Reviews;
  Out.Warnings = Saved.Warnings;
  Out.Timings = Saved.Timings;
  if (Saved.Settings.FlowRegions.Enabled &&
      Saved.State == ComparisonState::Complete &&
      Saved.FlowRegions.State != ComparisonState::Complete)
    return Fail(BindFailure::InvalidEvidence,
                "completed comparison has no completed SSA region analysis");
  Control.report("flow_regions");
  if (auto Error = validateFlowRegions(
          Saved.FlowRegions, {Primary, Saved.PrimaryDataFlow.get()},
          {Secondary, Saved.SecondaryDataFlow.get()}, Saved.Settings.DataFlow,
          Saved.Settings.FlowRegions, Control))
    return Fail(BindFailure::InvalidEvidence, *Error);
  Out.FlowRegions = Saved.FlowRegions;
  for (const SavedMatch &M : Saved.Matches) {
    Control.check();
    const auto A = Primary.functionAt(M.Primary);
    const auto B = Secondary.functionAt(M.Secondary);
    if (!A || !B)
      return Fail(BindFailure::MissingFunction,
                  "the analysis no longer finds a paired function: " +
                      hex(A ? M.Secondary : M.Primary));
    FunctionMatch Match = M.Match;
    Match.Primary = *A;
    Match.Secondary = *B;
    Out.Matches.push_back(Match);
  }
  for (const SavedAmbiguity &Saved : Saved.Ambiguities) {
    Control.check();
    Ambiguity Amb;
    Amb.Step = Saved.Step;
    for (uint64_t Address : Saved.Primary) {
      Control.check();
      const auto F = Primary.functionAt(Address);
      if (!F)
        return Fail(BindFailure::MissingFunction,
                    "the analysis no longer finds an ambiguous function: " +
                        hex(Address));
      Amb.Primary.push_back(*F);
    }
    for (uint64_t Address : Saved.Secondary) {
      Control.check();
      const auto F = Secondary.functionAt(Address);
      if (!F)
        return Fail(BindFailure::MissingFunction,
                    "the analysis no longer finds an ambiguous function: " +
                        hex(Address));
      Amb.Secondary.push_back(*F);
    }
    Out.Ambiguities.push_back(std::move(Amb));
  }
  if (Saved.MatcherVersion != MatcherVersion)
    Out.Warnings.push_back("the comparison was made by matcher version " +
                           std::to_string(Saved.MatcherVersion) +
                           "; comparing again may pair functions differently");
  llvm::sort(Out.Matches, [&](const FunctionMatch &L, const FunctionMatch &M) {
    Control.check();
    return L.Primary < M.Primary;
  });
  Out.PrimaryMatch.assign(Primary.Functions.size(), NoIndex);
  Out.SecondaryMatch.assign(Secondary.Functions.size(), NoIndex);
  for (uint32_t I = 0; I < Out.Matches.size(); ++I) {
    Control.check();
    if (Out.PrimaryMatch[Out.Matches[I].Primary] != NoIndex ||
        Out.SecondaryMatch[Out.Matches[I].Secondary] != NoIndex)
      return Fail(BindFailure::InvalidEvidence, "a function is paired twice");
    Out.PrimaryMatch[Out.Matches[I].Primary] = I;
    Out.SecondaryMatch[Out.Matches[I].Secondary] = I;
  }
  Control.report("regions");
  if (auto Error = validateRegions(Saved.Regions, Saved.Settings.Regions,
                                   Primary, Secondary, Control, Out.Matches))
    return Fail(BindFailure::StaleAnalysis, *Error);
  Out.Regions = Saved.Regions;
  // A saved report is evidence to check, not authority for decoded facts.
  // Reuse the final correspondence and the same measurement as a fresh run.
  std::vector<uint32_t> PMap(Primary.Functions.size(), NoIndex),
      SMap(Secondary.Functions.size(), NoIndex);
  for (const auto &M : Out.Matches) {
    Control.check();
    PMap[M.Primary] = M.Secondary;
    SMap[M.Secondary] = M.Primary;
  }
  Control.report("measure", 0, Out.Matches.size());
  const auto PK = mnemonicKeys(Primary, Control),
             SK = mnemonicKeys(Secondary, Control);
  auto WorkerControl = Control;
  WorkerControl.Progress = {};
  std::vector<uint8_t> Invalid(Out.Matches.size());
  parallelForEach(Out.Matches.size(), [&](auto Claim, size_t Count) {
    for (size_t I = Claim(); I < Count; I = Claim()) {
      const auto &M = Out.Matches[I];
      WorkerControl.check();
      FunctionMatch Actual;
      if (M.Measured) {
        const auto Delta =
            compareFunctionsWithKeys(Primary, M.Primary, Secondary, M.Secondary,
                                     {PMap, SMap}, PK, SK, WorkerControl);
        if (Delta.State == ComparisonState::Cancelled ||
            Delta.State == ComparisonState::TimedOut)
          throw ComparisonInterrupted(Delta.State);
        WorkerControl.check();
        measure(Actual, Delta);
      }
      Invalid[I] =
          Actual.Measured != M.Measured ||
          Actual.IdentityOnly != M.IdentityOnly ||
          Actual.AnalysisComplete != M.AnalysisComplete ||
          Actual.ReferenceComparisonComplete != M.ReferenceComparisonComplete ||
          Actual.Similarity != M.Similarity || Actual.Changes != M.Changes ||
          Actual.MatchedBlocks != M.MatchedBlocks ||
          Actual.MatchedEdges != M.MatchedEdges ||
          Actual.EqualInstructions != M.EqualInstructions ||
          (!M.Measured && Out.State == ComparisonState::Complete);
    }
  });
  Control.report("measure", Out.Matches.size(), Out.Matches.size());
  for (size_t I = 0; I < Invalid.size(); ++I)
    if (Invalid[I])
      return Fail(
          BindFailure::InvalidEvidence,
          "saved measurement disagrees with decoded evidence: " +
              hex(Primary.Functions[Out.Matches[I].Primary].Address) + " <-> " +
              hex(Secondary.Functions[Out.Matches[I].Secondary].Address));
  Out.PrimaryAmbiguity.assign(Primary.Functions.size(), NoIndex);
  Out.SecondaryAmbiguity.assign(Secondary.Functions.size(), NoIndex);
  for (uint32_t I = 0; I < Out.Ambiguities.size(); ++I) {
    Control.check();
    for (uint32_t F : Out.Ambiguities[I].Primary)
      Out.PrimaryAmbiguity[F] = I;
    for (uint32_t F : Out.Ambiguities[I].Secondary)
      Out.SecondaryAmbiguity[F] = I;
  }
  summarize(Out, Primary, Secondary);
  Control.report("publish", 0, 1);
  R = std::move(Out);
  return std::nullopt;
}

} // namespace neverd::diff
