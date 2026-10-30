//===- NeverDCAPIDiff.cpp - Revision-bound native comparisons -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owns comparison operation admission, lifetime, snapshots and error states.
/// Frontends consume this contract rather than repeating matching or parsing.
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPIDiff.h"

#include "BinaryComparison.h"
#include "ComparisonScalarValidation.h"
#include "ComparisonSource.h"
#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/diff/CallEffectReview.h"
#include "neverd/diff/CallImpact.h"
#include "neverd/diff/ChangeObservations.h"
#include "neverd/diff/ComparisonJSON.h"
#include "neverd/diff/DataFlowJSON.h"
#include "neverd/diff/FlowArgumentAccess.h"
#include "neverd/diff/FlowRegionsJSON.h"
#include "neverd/diff/FunctionDelta.h"
#include "neverd/diff/RegionsJSON.h"
#include "neverd/sdk/Limits.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

using namespace neverd;
using namespace neverd::sdk;
using namespace neverd::diff;
namespace {
namespace diffLimits = neverd::limits::diff;

struct Comparison {
  std::mutex DataMutex;
  std::mutex StatusMutex;
  std::atomic<bool> Started{false}, Running{false}, Cancel{false};
  std::string Phase = "idle", Error, Outcome = "idle";
  std::string RestoreOutcome = "idle";
  bool RestorePreviousRetained = false;
  ComparisonState RestoreSnapshotState = ComparisonState::Complete;
  std::string EditOutcome = "idle";
  bool EditPreviousRetained = false;
  ComparisonState EditSnapshotState = ComparisonState::Complete;
  uint64_t Done = 0, Total = 0;
  bool Published = false;
  Program Primary, Secondary;
  std::map<uint64_t, std::string> PrimaryDisplayNames, SecondaryDisplayNames;
  using OwnedSession = std::unique_ptr<void, decltype(&neverd_session_destroy)>;
  OwnedSession PrimarySource{nullptr, neverd_session_destroy};
  OwnedSession SecondarySource{nullptr, neverd_session_destroy};
  bool OwnedInputsBound = false;
  ComparisonInputOptions PrimaryOptions, SecondaryOptions;
  ComparisonResult Result;
  std::vector<OverrideEffect> Undo;
  std::vector<Override> Redo;
  struct Detail {
    uint32_t Primary = NoIndex, Secondary = NoIndex;
    uint64_t Timeout = 0;
    FunctionDelta Delta;
  };
  std::optional<Detail> CachedDetail;
  struct SourceCache {
    std::string Key;
    ComparisonSourceDifference Difference;
  };
  std::optional<SourceCache> CachedSource;
  struct SliceCache {
    std::string Key;
    FlowSlice Slice;
  };
  std::optional<SliceCache> CachedSlice;
  struct ArgumentAccessCache {
    std::string Key;
    FlowArgumentAccesses Evidence;
  };
  std::optional<ArgumentAccessCache> CachedArgumentAccesses;
  struct ImpactCache {
    std::string Key;
    CallImpact Evidence;
    std::vector<uint32_t> Entries;
    std::optional<CallEffectReview> Effects;
  };
  std::optional<ImpactCache> CachedImpact;
  struct ObservationCache {
    std::string Key;
    FunctionChangeObservations Evidence;
  };
  std::optional<ObservationCache> CachedObservations;
  std::string InventoryKey;
  std::vector<uint32_t> Inventory;
  struct AmbiguityIndex {
    std::vector<Ambiguity> Groups;
    std::vector<std::vector<uint32_t>> Primary, Secondary;
  };
  std::optional<AmbiguityIndex> CachedAmbiguities;

  void fail(llvm::StringRef Message) {
    std::lock_guard Lock(StatusMutex);
    Error = Message.str();
  }
};

/// Views whose interpretation depends on the current correspondence.
void invalidateMappingViews(Comparison &C) {
  C.CachedDetail.reset();
  C.CachedSource.reset();
  C.CachedObservations.reset();
  C.CachedAmbiguities.reset();
  C.InventoryKey.clear();
}

struct CandidateSet {
  // A single group shares normalized members; only overlaps need a union.
  const std::vector<uint32_t> *GroupFunctions = nullptr;
  std::vector<uint32_t> MergedFunctions;
  std::vector<FunctionStep> Steps;

  llvm::ArrayRef<uint32_t> functions() const {
    return GroupFunctions ? *GroupFunctions : MergedFunctions;
  }

  std::string explanation() const {
    std::string Out;
    for (auto Step : Steps) {
      if (!Out.empty())
        Out += "; ";
      Out += stepDescription(Step).str();
    }
    return Out.empty() ? "No retained ambiguity group for this function" : Out;
  }
  llvm::json::Array stepsJSON() const {
    llvm::json::Array Out;
    for (auto Step : Steps)
      Out.push_back(stepName(Step));
    return Out;
  }
};

CandidateSet candidates(Comparison &C, bool Secondary, uint32_t Function) {
  if (!C.CachedAmbiguities) {
    Comparison::AmbiguityIndex Index;
    Index.Groups = C.Result.Ambiguities;
    Index.Primary.resize(C.Primary.Functions.size());
    Index.Secondary.resize(C.Secondary.Functions.size());
    for (uint32_t I = 0; I < Index.Groups.size(); ++I) {
      auto &Group = Index.Groups[I];
      for (auto *Members : {&Group.Primary, &Group.Secondary}) {
        llvm::sort(*Members);
        Members->erase(std::unique(Members->begin(), Members->end()),
                       Members->end());
      }
      for (uint32_t A : Group.Primary)
        Index.Primary[A].push_back(I);
      for (uint32_t B : Group.Secondary)
        Index.Secondary[B].push_back(I);
    }
    C.CachedAmbiguities = std::move(Index);
  }
  const auto &Index = *C.CachedAmbiguities;
  CandidateSet Out;
  const auto &Groups =
      Secondary ? Index.Secondary[Function] : Index.Primary[Function];
  for (uint32_t I : Groups) {
    const auto &Group = Index.Groups[I];
    const auto &Others = Secondary ? Group.Primary : Group.Secondary;
    if (Groups.size() == 1)
      Out.GroupFunctions = &Others;
    else
      Out.MergedFunctions.insert(Out.MergedFunctions.end(), Others.begin(),
                                 Others.end());
    Out.Steps.push_back(Group.Step);
  }
  llvm::sort(Out.MergedFunctions);
  Out.MergedFunctions.erase(
      std::unique(Out.MergedFunctions.begin(), Out.MergedFunctions.end()),
      Out.MergedFunctions.end());
  llvm::sort(Out.Steps);
  Out.Steps.erase(std::unique(Out.Steps.begin(), Out.Steps.end()),
                  Out.Steps.end());
  return Out;
}

void require(bool Condition, llvm::StringRef Message) {
  if (!Condition)
    throw std::runtime_error(Message.str());
}

template <typename F>
auto guarded(neverd_comparison_t Handle, F &&Work)
    -> decltype(Work(*static_cast<Comparison *>(Handle))) {
  using Result = decltype(Work(*static_cast<Comparison *>(Handle)));
  auto *C = static_cast<Comparison *>(Handle);
  if (!C)
    return Result{};
  try {
    return Work(*C);
  } catch (const std::exception &E) {
    C->fail(E.what());
  } catch (...) {
    C->fail("unexpected native comparison failure");
  }
  return Result{};
}

llvm::json::Value json(const char *Text,
                       uint64_t MaxBytes = diffLimits::kMaxOptionsBytes) {
  require(Text, "JSON is required");
  size_t N = 0;
  while (N <= MaxBytes && Text[N])
    ++N;
  require(N <= MaxBytes, "comparison JSON exceeds its byte allowance");
  auto Parsed = llvm::json::parse(llvm::StringRef(Text, N));
  if (!Parsed) {
    const std::string Error = llvm::toString(Parsed.takeError());
    throw std::runtime_error("invalid comparison JSON: " + Error);
  }
  return std::move(*Parsed);
}

void keys(const llvm::json::Object &O,
          std::initializer_list<llvm::StringRef> Allowed) {
  for (const auto &[Key, V] : O) {
    (void)V;
    require(std::find(Allowed.begin(), Allowed.end(), llvm::StringRef(Key)) !=
                Allowed.end(),
            "unknown comparison option: " + Key.str());
  }
}

std::string string(const llvm::json::Object &O, llvm::StringRef Key,
                   llvm::StringRef Default = {}) {
  if (!O.get(Key))
    return Default.str();
  const auto V = O.getString(Key);
  require(V.has_value(), "comparison option must be a string: " + Key.str());
  return V->str();
}
bool flag(const llvm::json::Object &O, llvm::StringRef Key, bool Default) {
  if (!O.get(Key))
    return Default;
  const auto V = O.getBoolean(Key);
  require(V.has_value(), "comparison option must be boolean: " + Key.str());
  return *V;
}
uint64_t count(const llvm::json::Object &O, llvm::StringRef Key,
               uint64_t Default, uint64_t Max) {
  if (!O.get(Key))
    return Default;
  const auto V = O.getInteger(Key);
  require(V && *V >= 0 && static_cast<uint64_t>(*V) <= Max,
          "comparison option is outside its integer allowance: " + Key.str());
  return static_cast<uint64_t>(*V);
}
double unit(const llvm::json::Object &O, llvm::StringRef Key, double Default) {
  if (!O.get(Key))
    return Default;
  const auto V = O.getNumber(Key);
  require(V && std::isfinite(*V) && *V >= 0 && *V <= 1,
          "comparison score must be in [0,1]: " + Key.str());
  return *V;
}
uint64_t address(const llvm::json::Object &O, llvm::StringRef Key) {
  const auto Text = O.getString(Key);
  uint64_t A = 0;
  require(Text && !Text->getAsInteger(0, A),
          "address must be an integer string: " + Key.str());
  return A;
}
std::string sourceRepresentation(const llvm::json::Object &O) {
  auto Representation =
      string(O, "representation", ComparisonSourceOptions{}.Representation);
  require(Representation == "c" || Representation == "llvmc" ||
              Representation == "source" || Representation == "cpp" ||
              Representation == "rust" || Representation == "go",
          "unsupported source representation");
  return Representation;
}
Override overrideValue(const llvm::json::Object &O) {
  keys(O, {"kind", "primary", "secondary"});
  auto Kind = overrideNamed(string(O, "kind"));
  require(Kind.has_value(), "override kind must be match, unmatch or confirm");
  return {*Kind, address(O, "primary"), address(O, "secondary")};
}
void filter(const llvm::json::Object &O, llvm::StringRef Key,
            ComparisonInputOptions &Opts) {
  if (!O.get(Key))
    return;
  const auto *F = O.getObject(Key);
  require(F, "input filter must be an object");
  keys(*F, {"begin", "end", "function"});
  if (F->get("begin"))
    Opts.Begin = address(*F, "begin");
  if (F->get("end"))
    Opts.End = address(*F, "end");
  Opts.Function = string(*F, "function");
  require(!Opts.Begin || !Opts.End || *Opts.Begin < *Opts.End,
          "function range must have begin < end");
}
std::unique_lock<std::mutex> ready(Comparison &C) {
  std::unique_lock Lock(C.DataMutex, std::try_to_lock);
  require(Lock.owns_lock() && !C.Running, "comparison is busy");
  require(C.Published, "comparison has no published result");
  return Lock;
}
llvm::json::Array changes(uint32_t Mask) {
  llvm::json::Array A;
  for (auto Name : changeNames(Mask))
    A.push_back(Name);
  return A;
}
llvm::json::Object match(const FunctionMatch &M) {
  return llvm::json::Object{
      {"step", stepName(M.Step)},
      {"explanation", stepDescription(M.Step)},
      {"similarity", M.Similarity},
      {"confidence", M.Confidence},
      {"heuristic", true},
      {"from_context", M.FromContext},
      {"confirmed", M.Confirmed},
      {"measured", M.Measured},
      {"identity_only", M.IdentityOnly},
      {"analysis_complete", M.AnalysisComplete},
      {"reference_comparison_complete", M.ReferenceComparisonComplete},
      {"change_state", changeState(M)},
      {"normalized_unchanged", M.identical()},
      {"changes", changes(M.Changes)}};
}
llvm::StringRef displayName(const Comparison &C, const Program &P,
                            uint32_t Index) {
  const auto &F = P.Functions[Index];
  const auto &Names =
      &P == &C.Primary ? C.PrimaryDisplayNames : C.SecondaryDisplayNames;
  const auto It = Names.find(F.Address);
  return It == Names.end() ? llvm::StringRef(F.Name)
                           : llvm::StringRef(It->second);
}
llvm::json::Object function(const Comparison &C, const Program &P,
                            uint32_t Index) {
  const auto &F = P.Functions[Index];
  return llvm::json::Object{
      {"address", vaHex(F.Address)},
      {"name", jsonSafeText(displayName(C, P, Index))},
      {"evidence_name", jsonSafeText(F.Name)},
      {"size", F.Size},
      {"blocks", F.BlockCount},
      {"instructions", P.instructionCount(F)},
      {"traits", F.Traits},
      {"incomplete", F.has(TraitIncomplete)},
      {"identity", P.Identity.Revision + ":" + vaHex(F.Address)}};
}
llvm::json::Object instruction(const Program &P, uint32_t Index) {
  const auto &I = P.Instructions[Index];
  llvm::json::Object O{{"address", vaHex(I.Address)},
                       {"bytes", llvm::toHex(P.bytes(I))},
                       {"mnemonic", P.mnemonic(I)},
                       {"operands", jsonSafeText(I.Operands)}};
  O["target"] = I.Target ? llvm::json::Value(vaHex(*I.Target)) : nullptr;
  O["callee"] = I.Callee != NoIndex
                    ? llvm::json::Value(vaHex(P.Functions[I.Callee].Address))
                    : nullptr;
  return O;
}
llvm::json::Object graph(const Comparison &C, const Program &P,
                         uint32_t Index) {
  const auto &F = P.Functions[Index];
  llvm::json::Array Blocks, Edges;
  for (const auto &B : P.blocks(F)) {
    llvm::json::Array Instructions;
    for (uint32_t I = 0; I < B.InstructionCount; ++I)
      Instructions.push_back(instruction(P, B.FirstInstruction + I));
    Blocks.push_back(llvm::json::Object{
        {"address", vaHex(B.Address)},
        {"identity",
         P.Identity.Revision + ":" + vaHex(F.Address) + ":" + vaHex(B.Address)},
        {"instructions", std::move(Instructions)}});
  }
  for (const auto &E : P.edges(F))
    Edges.push_back(
        llvm::json::Array{E.From, E.To, static_cast<unsigned>(E.Kind)});
  return llvm::json::Object{
      {"function", function(C, P, Index)},
      {"blocks", std::move(Blocks)},
      {"edges", std::move(Edges)},
      {"entry_block", F.EntryBlock},
      {"pseudocode",
       llvm::json::Object{{"state", "unavailable"},
                          {"reason", "not requested in the decoded profile"}}}};
}
using SourceListing = std::map<uint32_t, std::pair<uint64_t, llvm::StringRef>>;
SourceListing sourceListing(const Comparison &C, const Program &P,
                            const FunctionMatch *M, uint32_t Index,
                            bool Secondary) {
  std::map<uint32_t, std::pair<uint64_t, llvm::StringRef>> Listing;
  if (M && C.CachedDetail && C.CachedDetail->Primary == M->Primary &&
      C.CachedDetail->Secondary == M->Secondary &&
      C.CachedDetail->Delta.ComputationComplete) {
    const auto &Rows = C.CachedDetail->Delta.Rows;
    for (size_t Row = 0; Row < Rows.size(); ++Row) {
      const auto Instruction =
          Secondary ? Rows[Row].Secondary : Rows[Row].Primary;
      if (Instruction != NoIndex)
        Listing.emplace(Instruction,
                        std::pair{Row, rowKindName(Rows[Row].Kind)});
    }
  } else if (!M) {
    uint64_t Row = 0;
    for (const auto &Block : P.blocks(P.Functions[Index]))
      for (uint32_t I = 0; I < Block.InstructionCount; ++I)
        Listing.emplace(Block.FirstInstruction + I,
                        std::pair{Row++, llvm::StringRef("unmatched")});
  }
  return Listing;
}
void sourceNavigation(llvm::json::Object &Row, const SourceListing &Listing) {
  if (auto *Anchors = Row.getArray("comparison_anchors"))
    for (auto &Anchor : *Anchors) {
      auto &A = *Anchor.getAsObject();
      if (const auto I = A.getInteger("instruction_index"))
        if (const auto It = Listing.find(*I); It != Listing.end()) {
          A["listing_row"] = It->second.first;
          A["native_change"] = It->second.second;
        }
    }
}
const FunctionDelta &cachedDelta(Comparison &C, const FunctionMatch &M,
                                 uint64_t Timeout = 0, bool Refresh = false,
                                 const ComparisonControl *External = nullptr) {
  if (Refresh || !C.CachedDetail || C.CachedDetail->Primary != M.Primary ||
      C.CachedDetail->Secondary != M.Secondary ||
      C.CachedDetail->Timeout != Timeout) {
    require(C.Primary.instructionCount(C.Primary.Functions[M.Primary]) +
                    C.Secondary.instructionCount(
                        C.Secondary.Functions[M.Secondary]) <=
                diffLimits::kMaxDetailInstructions,
            "paired function exceeds detail instruction allowance");
    if (!External) {
      std::lock_guard Status(C.StatusMutex);
      C.Cancel = false;
      C.Running = true;
      C.Outcome = "running";
      C.Phase = "detail:compare";
      C.Done = 0;
      C.Total =
          C.Primary.instructionCount(C.Primary.Functions[M.Primary]) +
          C.Secondary.instructionCount(C.Secondary.Functions[M.Secondary]);
      C.Error.clear();
    }
    auto Finished = llvm::scope_exit([&] {
      if (External)
        return;
      std::lock_guard Status(C.StatusMutex);
      C.Running = false;
      if (C.Outcome == "running")
        C.Outcome = "failed";
    });
    ComparisonControl Control;
    if (External)
      Control = *External;
    else {
      Control.Cancel = &C.Cancel;
      if (Timeout)
        Control.Deadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(Timeout);
    }
    std::vector<uint32_t> Forward(C.Primary.Functions.size(), NoIndex),
        Backward(C.Secondary.Functions.size(), NoIndex);
    for (const auto &Pair : C.Result.Matches) {
      Forward[Pair.Primary] = Pair.Secondary;
      Backward[Pair.Secondary] = Pair.Primary;
    }
    C.CachedDetail = Comparison::Detail{
        M.Primary, M.Secondary, Timeout,
        compareFunctions(C.Primary, M.Primary, C.Secondary, M.Secondary,
                         {Forward, Backward}, Control)};
    if (!External) {
      std::lock_guard Status(C.StatusMutex);
      C.Outcome = stateName(C.CachedDetail->Delta.State).str();
      C.Done = C.CachedDetail->Delta.ComputationComplete ? C.Total : 0;
    }
  }
  return C.CachedDetail->Delta;
}
void detailStatus(llvm::json::Object &Out, const FunctionDelta *Delta) {
  Out["state"] = Delta ? stateName(Delta->State) : "complete";
  Out["computation_complete"] = !Delta || Delta->ComputationComplete;
}
std::pair<bool, uint32_t> selectedFunction(const Comparison &C,
                                           const llvm::json::Object &O) {
  const auto Side = string(O, "side", "primary");
  require(Side == "primary" || Side == "secondary", "invalid comparison side");
  const bool Secondary = Side == "secondary";
  const auto &P = Secondary ? C.Secondary : C.Primary;
  const auto I = P.functionAt(address(O, "address"));
  require(I.has_value(), "function is not in the comparison snapshot");
  return {Secondary, *I};
}
const RegionHypothesis &region(const Comparison &C, llvm::StringRef Id) {
  require(C.Result.Regions.State.has_value(),
          "region analysis was not requested; compare with regions:true");
  for (const auto &H : C.Result.Regions.Hypotheses)
    if (H.Id == Id)
      return H;
  throw std::runtime_error("region hypothesis is absent from this comparison");
}
} // namespace

neverd_comparison_t neverd_comparison_create(void) {
  try {
    return new Comparison;
  } catch (...) {
    return nullptr;
  }
}
void neverd_comparison_destroy(neverd_comparison_t C) {
  delete static_cast<Comparison *>(C);
}
void neverd_comparison_cancel(neverd_comparison_t C) {
  if (C)
    static_cast<Comparison *>(C)->Cancel = true;
}
const char *neverd_comparison_last_error(neverd_comparison_t C) {
  return guarded(C, [](Comparison &C) -> const char * {
    std::lock_guard Lock(C.StatusMutex);
    return dupStr(C.Error);
  });
}
const char *neverd_comparison_status_json(neverd_comparison_t C) {
  return guarded(C, [](Comparison &C) -> const char * {
    std::lock_guard Lock(C.StatusMutex);
    const bool Restore = llvm::StringRef(C.Phase).starts_with("restore:");
    const bool Edit = llvm::StringRef(C.Phase).starts_with("edit:");
    return dupStr(jsonToString(llvm::json::Object{
        {"schema", "neverd.bindiff.status"},
        {"version", 1},
        {"running", C.Running.load()},
        {"cancel_requested", C.Cancel.load()},
        {"computation_stopped", C.Started.load() && !C.Running.load()},
        {"partial_result_retained",
         C.Published &&
             (Restore ? C.RestoreSnapshotState != ComparisonState::Complete
              : Edit  ? C.EditSnapshotState != ComparisonState::Complete
                      : C.Outcome != "complete")},
        {"restore_state", Restore ? C.RestoreOutcome : "idle"},
        {"edit_state", Edit ? C.EditOutcome : "idle"},
        {"previous_result_retained", Restore ? C.RestorePreviousRetained
                                     : Edit  ? C.EditPreviousRetained
                                             : false},
        {"result_available", C.Published},
        {"state", C.Outcome},
        {"phase", C.Phase},
        {"done", C.Done},
        {"total", C.Total},
        {"error", C.Error}}));
  });
}

static int runComparison(neverd_comparison_t Handle, neverd_session_t A,
                         neverd_session_t B, const char *Options,
                         neverd_comparison_progress_t Progress, void *Context,
                         bool Own, neverd_session_t *TakeA = nullptr,
                         neverd_session_t *TakeB = nullptr) {
  return guarded(Handle, [&](Comparison &C) {
    std::unique_lock Lock(C.DataMutex, std::try_to_lock);
    require(Lock.owns_lock() && !C.Started.load(),
            "run requires a fresh comparison handle");
    if (Own)
      require(TakeA && TakeB && TakeA != TakeB && A && B && A != B,
              "owned run requires distinct live sessions in distinct slots");
    C.Started = true;
    {
      std::lock_guard Status(C.StatusMutex);
      C.Error.clear();
    }
    if (Own) {
      C.PrimarySource.reset(A);
      C.SecondarySource.reset(B);
      *TakeA = nullptr;
      *TakeB = nullptr;
      toSession(A)->LoadProgressCb = {};
      toSession(B)->LoadProgressCb = {};
    }
    C.Running = true;
    const auto Finished = llvm::scope_exit([&] {
      std::lock_guard Status(C.StatusMutex);
      C.Running = false;
      if (!C.Published)
        C.Outcome = "failed";
    });
    require(A && B, "two live sessions are required");
    auto Value = json(Options ? Options : "{}");
    const auto *O = Value.getAsObject();
    require(O, "comparison options must be an object");
    keys(*O, {"profile", "discover", "use_names", "approximate",
              "min_similarity", "min_context_similarity", "ambiguity_margin",
              "max_candidates", "max_instructions", "max_functions",
              "max_memory_bytes", "timeout_ms", "primary", "secondary",
              "overrides", "regions", "dataflow", "flow_regions"});
    ComparisonSettings Settings;
    Settings.UseNames = flag(*O, "use_names", Settings.UseNames);
    Settings.Approximate = flag(*O, "approximate", Settings.Approximate);
    Settings.MinimumSimilarity =
        unit(*O, "min_similarity", Settings.MinimumSimilarity);
    Settings.MinimumContextSimilarity =
        unit(*O, "min_context_similarity", Settings.MinimumContextSimilarity);
    Settings.AmbiguityMargin =
        unit(*O, "ambiguity_margin", Settings.AmbiguityMargin);
    Settings.MaxCandidates = count(*O, "max_candidates", Settings.MaxCandidates,
                                   diffLimits::kMaxCandidates);
    if (const auto *Regions = O->get("regions"))
      Settings.Regions = readRegionSettings(*Regions);
    if (const auto *Flow = O->get("dataflow"))
      Settings.DataFlow = readDataFlowSettings(*Flow);
    if (const auto *Flow = O->get("flow_regions")) {
      Settings.FlowRegions = readFlowRegionSettings(*Flow);
      if (Settings.FlowRegions.Enabled && !O->get("dataflow"))
        Settings.DataFlow.Enabled = true;
    }
    if (auto E = Settings.validate())
      throw std::runtime_error(*E);
    auto &PO = C.PrimaryOptions;
    PO.Profile = string(*O, "profile", diffLimits::kDefaultNativeProfile);
    PO.DataFlow = Settings.DataFlow;
    PO.Discover = flag(*O, "discover", true);
    PO.MaxInstructions = count(*O, "max_instructions", PO.MaxInstructions,
                               diffLimits::kMaxInstructions);
    PO.MaxFunctions = count(*O, "max_functions", PO.MaxFunctions,
                            diffLimits::kDefaultMaxFunctions);
    PO.MaxMemoryBytes = count(*O, "max_memory_bytes", PO.MaxMemoryBytes,
                              diffLimits::kDefaultMaxMemoryBytes);
    C.SecondaryOptions = PO;
    filter(*O, "primary", C.PrimaryOptions);
    filter(*O, "secondary", C.SecondaryOptions);
    std::vector<Override> Overrides;
    if (O->get("overrides")) {
      const auto *Rows = O->getArray("overrides");
      require(Rows && Rows->size() <= diffLimits::kMaxSavedOverrides,
              "overrides must be a bounded array");
      for (const auto &V : *Rows) {
        require(V.getAsObject(), "override must be an object");
        Overrides.push_back(overrideValue(*V.getAsObject()));
      }
    }
    ComparisonControl Control;
    Control.Cancel = &C.Cancel;
    const auto Timeout =
        count(*O, "timeout_ms", 0, diffLimits::kMaxTimeoutMilliseconds);
    if (Timeout)
      Control.Deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(Timeout);
    std::string Side = "primary";
    Control.Progress = [&](llvm::StringRef Phase, uint64_t Done,
                           uint64_t Total) {
      const std::string Name =
          Side.empty() ? Phase.str() : Side + ":" + Phase.str();
      {
        std::lock_guard Status(C.StatusMutex);
        C.Phase = Name;
        C.Done = Done;
        C.Total = Total;
        C.Outcome = "running";
      }
      if (Progress)
        Progress(Context, Name.c_str(), Done, Total);
    };
    std::string Error;
    auto Primary =
        readComparisonInput(*toSession(A), C.PrimaryOptions, Control, Error);
    require(Primary.has_value(), Error);
    Side = "secondary";
    auto Secondary =
        readComparisonInput(*toSession(B), C.SecondaryOptions, Control, Error);
    require(Secondary.has_value(), Error);
    require(Primary->Program.Identity.Architecture ==
                    Secondary->Program.Identity.Architecture &&
                Primary->Program.Identity.ABI ==
                    Secondary->Program.Identity.ABI,
            "unsupported ISA or ABI pair for " + PO.Profile);
    require(Primary->Stopped || Secondary->Stopped ||
                (!Primary->Program.Instructions.empty() &&
                 !Secondary->Program.Instructions.empty()),
            "comparison has no decoded code on one or both sides");
    C.Primary = std::move(Primary->Program);
    C.Secondary = std::move(Secondary->Program);
    C.PrimaryDisplayNames = std::move(Primary->DisplayNames);
    C.SecondaryDisplayNames = std::move(Secondary->DisplayNames);
    C.OwnedInputsBound = Own && !Primary->Stopped && !Secondary->Stopped;
    for (const auto &O : Overrides)
      require(C.Primary.functionAt(O.Primary) &&
                  C.Secondary.functionAt(O.Secondary),
              "override does not name functions in these snapshots");
    Side.clear();
    if (Primary->Stopped || Secondary->Stopped) {
      C.Result.Settings = Settings;
      C.Result.FlowRegions.Profile =
          flowRegionProfile(Settings.DataFlow.Profile,
                            Settings.FlowRegions.Fragments)
              .str();
      C.Result.PrimaryDataFlow = C.Primary.DataFlow;
      C.Result.SecondaryDataFlow = C.Secondary.DataFlow;
      C.Result.State =
          Primary->Stopped ? *Primary->Stopped : *Secondary->Stopped;
      C.Result.StoppedPhase =
          Primary->Stopped ? Primary->StoppedPhase : Secondary->StoppedPhase;
      C.Result.PrimaryMatch.assign(C.Primary.Functions.size(), NoIndex);
      C.Result.SecondaryMatch.assign(C.Secondary.Functions.size(), NoIndex);
      C.Result.PrimaryAmbiguity.assign(C.Primary.Functions.size(), NoIndex);
      C.Result.SecondaryAmbiguity.assign(C.Secondary.Functions.size(), NoIndex);
      C.Result.Warnings = C.Primary.Warnings;
      C.Result.Warnings.insert(C.Result.Warnings.end(),
                               C.Secondary.Warnings.begin(),
                               C.Secondary.Warnings.end());
      summarize(C.Result, C.Primary, C.Secondary);
    } else {
      C.Result = compare(C.Primary, C.Secondary, Settings, Overrides, Control);
    }
    {
      std::lock_guard Status(C.StatusMutex);
      C.Outcome = stateName(C.Result.State).str();
      C.Published = true;
    }
    return 1;
  });
}

int neverd_comparison_run(neverd_comparison_t Handle, neverd_session_t A,
                          neverd_session_t B, const char *Options,
                          neverd_comparison_progress_t Progress,
                          void *Context) {
  return runComparison(Handle, A, B, Options, Progress, Context, false);
}

int neverd_comparison_run_owned(neverd_comparison_t Handle, neverd_session_t *A,
                                neverd_session_t *B, const char *Options,
                                neverd_comparison_progress_t Progress,
                                void *Context) {
  return runComparison(Handle, A ? *A : nullptr, B ? *B : nullptr, Options,
                       Progress, Context, true, A, B);
}

const char *neverd_comparison_summary_json(neverd_comparison_t Handle) {
  return guarded(Handle, [](Comparison &C) -> const char * {
    auto Lock = ready(C);
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    writeComparison(OS, C.Result, C.Primary, C.Secondary, true);
    auto Summary = llvm::json::parse(Text);
    require(bool(Summary), "failed to serialize comparison summary");
    auto &O = *Summary->getAsObject();
    O["can_undo"] = !C.Undo.empty();
    O["can_redo"] = !C.Redo.empty();
    O["owned_validation_inputs"] = C.OwnedInputsBound;
    O["owned_source_inputs"] = C.OwnedInputsBound;
    return dupStr(jsonToString(std::move(*Summary)));
  });
}

const char *neverd_comparison_export_json(neverd_comparison_t Handle) {
  return guarded(Handle, [](Comparison &C) -> const char * {
    auto Lock = ready(C);
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    writeComparison(OS, C.Result, C.Primary, C.Secondary);
    return dupStr(Text);
  });
}
int neverd_comparison_restore_json(neverd_comparison_t Handle,
                                   const char *Text) {
  return neverd_comparison_restore_json_with_progress(Handle, Text, nullptr,
                                                      nullptr);
}

int neverd_comparison_restore_json_with_progress(
    neverd_comparison_t Handle, const char *Text,
    neverd_comparison_progress_t Progress, void *Context) {
  return guarded(Handle, [&](Comparison &C) {
    auto Lock = ready(C);
    require(Text, "saved comparison JSON is required");
    {
      std::lock_guard Status(C.StatusMutex);
      C.Cancel = false;
      C.Running = true;
      C.Outcome = C.RestoreOutcome = "running";
      C.Phase = "restore:scan";
      C.Done = C.Total = 0;
      C.Error.clear();
      C.RestorePreviousRetained = true;
      C.RestoreSnapshotState = C.Result.State;
    }
    auto Finished = llvm::scope_exit([&] {
      std::lock_guard Status(C.StatusMutex);
      C.Running = false;
      if (C.RestoreOutcome == "running")
        C.Outcome = C.RestoreOutcome = "failed";
    });
    ComparisonControl Control;
    Control.Cancel = &C.Cancel;
    Control.Progress = [&](llvm::StringRef Phase, uint64_t Done,
                           uint64_t Total) {
      const auto Name = "restore:" + Phase.str();
      {
        std::lock_guard Status(C.StatusMutex);
        C.Phase = Name;
        C.Done = Done;
        C.Total = Total;
      }
      if (Progress)
        Progress(Context, Name.c_str(), Done, Total);
    };
    try {
      // Admit the C string without parsing a second DOM or reading past its
      // NUL.
      Control.report("scan");
      size_t Bytes = 0;
      while (Bytes <= diffLimits::kMaxSavedComparisonBytes && Text[Bytes]) {
        ++Bytes;
        if (Bytes % diffLimits::kSavedComparisonChunkBytes == 0)
          Control.report("scan", Bytes);
      }
      require(Bytes <= diffLimits::kMaxSavedComparisonBytes,
              "comparison JSON exceeds its byte allowance");
      std::string Error;
      auto Saved = readComparison(llvm::StringRef(Text, Bytes), Error, Control);
      require(Saved.has_value(), Error);
      ComparisonResult Next;
      if (const auto E = bindComparison(*Saved, C.Primary, C.Secondary, Next,
                                        nullptr, Control))
        throw std::runtime_error(*E);
      auto Outcome = stateName(Next.State).str();
      std::lock_guard Status(C.StatusMutex);
      Control.check();
      C.Result = std::move(Next);
      invalidateMappingViews(C);
      C.CachedSlice.reset();
      C.CachedArgumentAccesses.reset();
      C.CachedImpact.reset();
      C.Undo.clear();
      C.Redo.clear();
      C.Outcome = std::move(Outcome);
      C.RestoreOutcome = "complete";
      C.RestorePreviousRetained = false;
      C.RestoreSnapshotState = C.Result.State;
      C.Done = C.Total = 1;
      return 1;
    } catch (const ComparisonInterrupted &Stopped) {
      std::lock_guard Status(C.StatusMutex);
      C.Outcome = C.RestoreOutcome = stateName(Stopped.State).str();
      C.Error = "saved comparison restore " + C.RestoreOutcome;
      return 0;
    }
  });
}

const char *neverd_comparison_functions_json(neverd_comparison_t Handle,
                                             const char *Query) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    auto V = json(Query ? Query : "{}");
    const auto *O = V.getAsObject();
    require(O, "inventory query must be an object");
    keys(*O, {"side", "kind", "search", "offset", "limit", "sort", "descending",
              "ambiguous_with"});
    const auto Side = string(*O, "side", "primary");
    require(Side == "primary" || Side == "secondary", "invalid inventory side");
    const bool Secondary = Side == "secondary";
    const auto &P = Secondary ? C.Secondary : C.Primary;
    const auto &Other = Secondary ? C.Primary : C.Secondary;
    const auto &Amb =
        Secondary ? C.Result.SecondaryAmbiguity : C.Result.PrimaryAmbiguity;
    std::optional<uint32_t> Source;
    CandidateSet SourceCandidates;
    if (O->get("ambiguous_with")) {
      Source = Other.functionAt(address(*O, "ambiguous_with"));
      require(Source.has_value(),
              "ambiguity source is not in the opposite comparison snapshot");
      SourceCandidates = candidates(C, !Secondary, *Source);
    }
    const auto Kind = string(*O, "kind", "all");
    require(Kind == "all" || Kind == "matched" || Kind == "changed" ||
                Kind == "uncertain" || Kind == "ambiguous" ||
                Kind == "unmatched",
            "invalid inventory kind");
    const auto Search = llvm::StringRef(string(*O, "search")).lower();
    const auto Sort = string(*O, "sort", "address");
    require(Sort == "address" || Sort == "name" || Sort == "size" ||
                Sort == "similarity" || Sort == "confidence" ||
                Sort == "changes",
            "invalid inventory sort key");
    const bool Descending = flag(*O, "descending", false);
    const std::string Key = jsonToString(llvm::json::Object{
        {"side", Side},
        {"kind", Kind},
        {"search", Search},
        {"sort", Sort},
        {"descending", Descending},
        {"ambiguous_with",
         Source ? llvm::json::Value(vaHex(Other.Functions[*Source].Address))
                : llvm::json::Value(nullptr)}});
    const auto Pair = [&](uint32_t I) {
      return Secondary ? C.Result.matchOfSecondary(I)
                       : C.Result.matchOfPrimary(I);
    };
    if (Key != C.InventoryKey) {
      std::vector<uint32_t> Order;
      const auto Include = [&](uint32_t I) {
        const auto *M = Pair(I);
        const bool Ambiguous = I < Amb.size() && Amb[I] != NoIndex;
        if ((Kind == "matched" && !M) ||
            (Kind == "changed" &&
             (!M || !M->Measured || !(M->Changes & ~MovementChanges))) ||
            (Kind == "uncertain" && (!M || changeState(*M) != "uncertain")) ||
            (Kind == "ambiguous" && !Ambiguous) ||
            (Kind == "unmatched" && (M || Ambiguous)))
          return;
        if (!Search.empty() &&
            displayName(C, P, I).lower().find(Search) == std::string::npos &&
            llvm::StringRef(P.Functions[I].Name).lower().find(Search) ==
                std::string::npos &&
            llvm::StringRef(vaHex(P.Functions[I].Address))
                    .lower()
                    .find(Search) == std::string::npos)
          return;
        Order.push_back(I);
      };
      if (Source) {
        for (const auto I : SourceCandidates.functions())
          Include(I);
      } else {
        for (uint32_t I = 0; I < P.Functions.size(); ++I)
          Include(I);
      }
      if (Sort != "address" || Descending || Source) {
        const auto Number = [&](uint32_t I) -> double {
          const auto *M = Pair(I);
          if (Sort == "size")
            return P.Functions[I].Size;
          if (!M)
            return -1;
          if (Sort == "similarity")
            return M->Similarity;
          if (Sort == "confidence")
            return M->Confidence;
          return M->Changes;
        };
        std::sort(Order.begin(), Order.end(), [&](uint32_t A, uint32_t B) {
          if (Descending)
            std::swap(A, B);
          if (Sort == "name" && displayName(C, P, A) != displayName(C, P, B))
            return displayName(C, P, A) < displayName(C, P, B);
          if (Sort != "name" && Sort != "address" && Number(A) != Number(B))
            return Number(A) < Number(B);
          return P.Functions[A].Address < P.Functions[B].Address;
        });
      }
      C.Inventory = std::move(Order);
      C.InventoryKey = Key;
    }
    const uint64_t Count = C.Inventory.size();
    const auto Offset = count(*O, "offset", 0, Count);
    const auto Limit = count(*O, "limit", diffLimits::kDefaultPageSize,
                             diffLimits::kMaxPageSize);
    require(Limit > 0, "page limit must be positive");
    llvm::json::Array Rows;
    for (uint64_t Position = Offset; Position < std::min(Count, Offset + Limit);
         ++Position) {
      const auto I = C.Inventory[Position];
      const auto *M = Pair(I);
      const bool Ambiguous = I < Amb.size() && Amb[I] != NoIndex;
      auto Row = function(C, P, I);
      Row["state"] = M ? "matched" : Ambiguous ? "ambiguous" : "unmatched";
      if (M) {
        Row["counterpart"] =
            function(C, Other, Secondary ? M->Primary : M->Secondary);
        Row["evidence"] = match(*M);
      }
      if (Ambiguous) {
        const auto Alternatives = candidates(C, Secondary, I);
        llvm::json::Array Candidates;
        const auto Others = Alternatives.functions();
        for (size_t N = 0;
             N <
             std::min<size_t>(Others.size(), diffLimits::kDefaultMaxCandidates);
             ++N)
          Candidates.push_back(function(C, Other, Others[N]));
        Row["candidate_total"] = Others.size();
        Row["candidate_preview_complete"] = Others.size() == Candidates.size();
        Row["candidates"] = std::move(Candidates);
        Row["explanation"] = Alternatives.explanation();
        Row["candidate_steps"] = Alternatives.stepsJSON();
      }
      Rows.push_back(std::move(Row));
    }
    const auto Returned = Rows.size();
    llvm::json::Object Out{
        {"schema", "neverd.bindiff.functions"},
        {"version", 1},
        {"side", Side},
        {"total", Count},
        {"offset", Offset},
        {"next_offset", Offset + Returned < Count
                            ? llvm::json::Value(Offset + Returned)
                            : nullptr},
        {"items", std::move(Rows)}};
    if (Source)
      Out["ambiguity"] = llvm::json::Object{
          {"source_side", Secondary ? "primary" : "secondary"},
          {"source", function(C, Other, *Source)},
          {"candidate_total", SourceCandidates.functions().size()},
          {"active", !SourceCandidates.functions().empty()},
          {"explanation", SourceCandidates.explanation()},
          {"steps", SourceCandidates.stepsJSON()}};
    return dupStr(jsonToString(std::move(Out)));
  });
}

const char *neverd_comparison_function_json(neverd_comparison_t Handle,
                                            int Secondary,
                                            neverd_va_t Address) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    require(Secondary == 0 || Secondary == 1, "side must be 0 or 1");
    const auto &P = Secondary ? C.Secondary : C.Primary;
    auto Index = P.functionAt(Address);
    require(Index.has_value(), "function is not in the comparison snapshot");
    const auto *M = Secondary ? C.Result.matchOfSecondary(*Index)
                              : C.Result.matchOfPrimary(*Index);
    llvm::json::Object Out{{"schema", "neverd.bindiff.function"},
                           {"version", 1}};
    if (!M) {
      require(P.instructionCount(P.Functions[*Index]) <=
                  diffLimits::kMaxDetailInstructions,
              "function exceeds detail instruction allowance");
      Out[Secondary ? "secondary" : "primary"] = graph(C, P, *Index);
      Out[Secondary ? "primary" : "secondary"] = nullptr;
      detailStatus(Out, nullptr);
      return dupStr(jsonToString(std::move(Out)));
    }
    const auto &Delta = cachedDelta(C, *M);
    detailStatus(Out, &Delta);
    Out["primary"] = graph(C, C.Primary, M->Primary);
    Out["secondary"] = graph(C, C.Secondary, M->Secondary);
    Out["evidence"] = match(*M);
    llvm::json::Array Rows, Blocks;
    for (const auto &R : Delta.Rows) {
      llvm::json::Object Row{{"kind", rowKindName(R.Kind)},
                             {"primary_block", R.PrimaryBlock},
                             {"secondary_block", R.SecondaryBlock}};
      Row["primary"] = R.Primary == NoIndex ? llvm::json::Value(nullptr)
                                            : instruction(C.Primary, R.Primary);
      Row["secondary"] = R.Secondary == NoIndex
                             ? llvm::json::Value(nullptr)
                             : instruction(C.Secondary, R.Secondary);
      Rows.push_back(std::move(Row));
    }
    for (const auto &B : Delta.Blocks)
      Blocks.push_back(llvm::json::Object{{"primary", B.Primary},
                                          {"secondary", B.Secondary},
                                          {"step", stepName(B.Step)},
                                          {"unchanged", B.Unchanged},
                                          {"uncertain", B.Uncertain}});
    Out["rows"] = std::move(Rows);
    Out["block_pairs"] = std::move(Blocks);
    return dupStr(jsonToString(std::move(Out)));
  });
}

const char *neverd_comparison_source_page_json(neverd_comparison_t Handle,
                                               const char *Query) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    require(
        C.OwnedInputsBound,
        "source inspection requires completed native capture from run_owned");
    auto V = json(Query);
    const auto *O = V.getAsObject();
    require(O, "source query must be an object");
    keys(*O, {"side", "address", "representation", "offset", "limit",
              "max_native_instructions", "timeout_ms", "refresh"});
    const auto [Secondary, Index] = selectedFunction(C, *O);
    const auto &P = Secondary ? C.Secondary : C.Primary;
    const auto *M = Secondary ? C.Result.matchOfSecondary(Index)
                              : C.Result.matchOfPrimary(Index);
    ComparisonSourceOptions Options;
    Options.Representation = sourceRepresentation(*O);
    Options.Offset = count(*O, "offset", 0, INT64_MAX);
    Options.Limit = count(*O, "limit", Options.Limit,
                          sdk::kNeverDCAPIDecompileNeverdIrViewJsonLimit);
    require(Options.Limit > 0, "source page limit must be positive");
    Options.MaxNativeInstructions =
        count(*O, "max_native_instructions", Options.MaxNativeInstructions,
              UINT32_MAX);
    Options.Refresh = flag(*O, "refresh", false);
    const auto Timeout =
        count(*O, "timeout_ms", 0, diffLimits::kMaxTimeoutMilliseconds);
    {
      std::lock_guard Status(C.StatusMutex);
      C.Cancel = false;
      C.Running = true;
      C.Outcome = "running";
      C.Phase = "source:analysis";
      C.Done = 0;
      C.Total = 2;
      C.Error.clear();
    }
    auto Finished = llvm::scope_exit([&] {
      std::lock_guard Status(C.StatusMutex);
      C.Running = false;
      if (C.Outcome == "running")
        C.Outcome = "failed";
    });
    ComparisonControl Control;
    Control.Cancel = &C.Cancel;
    if (Timeout)
      Control.Deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(Timeout);
    Control.Progress = [&](llvm::StringRef Phase, uint64_t Done,
                           uint64_t Total) {
      std::lock_guard Status(C.StatusMutex);
      C.Phase = "source:" + Phase.str();
      C.Done = Done;
      C.Total = Total;
    };
    auto &S =
        *toSession(Secondary ? C.SecondarySource.get() : C.PrimarySource.get());
    auto Result = readComparisonSource(S, P, Index, Options, Control);
    // Navigation may reuse a completed native alignment, never infer a
    // source-to-source pairing from line numbers or source text similarity.
    const auto Listing = sourceListing(C, P, M, Index, Secondary);
    if (auto *Rows = Result.Document.getArray("rows"))
      for (auto &Value : *Rows)
        sourceNavigation(*Value.getAsObject(), Listing);
    if (auto Stop = Control.stop()) {
      Result.State = *Stop;
      Result.Availability = "unavailable";
      Result.Reason = "source inspection stopped";
      Result.Document.clear();
    }
    {
      std::lock_guard Status(C.StatusMutex);
      C.Outcome = Result.Availability == "failed"
                      ? "failed"
                      : stateName(Result.State).str();
    }
    llvm::json::Object Out{
        {"schema", "neverd.bindiff.source.page"},
        {"version", 1},
        {"state", stateName(Result.State)},
        {"availability", Result.Availability},
        {"reason", jsonSafeText(Result.Reason)},
        {"side", Secondary ? "secondary" : "primary"},
        {"function", function(C, P, Index)},
        {"input_revision", P.Identity.Revision},
        {"input_sha256", P.Identity.SHA256},
        {"representation", Options.Representation},
        {"recipe", std::string(VersionString) + ":sdk-source-page-v1"},
        {"input_source_binding", "owned_sessions"},
        {"native_analysis_complete", !P.Functions[Index].has(TraitIncomplete)},
        {"provenance_complete", false},
        {"semantic_validation", "not_performed"},
        {"source_alignment", "not_performed"},
        {"listing_navigation_available", !Listing.empty()},
        {"max_native_instructions", Options.MaxNativeInstructions},
        {"timeout_ms", Timeout},
        {"hard_deadline", false},
        {"cancellation_granularity", "between_function_analysis_and_rendering"},
        {"document", std::move(Result.Document)}};
    Out["counterpart"] =
        M ? llvm::json::Value(function(C, Secondary ? C.Primary : C.Secondary,
                                       Secondary ? M->Primary : M->Secondary))
          : llvm::json::Value(nullptr);
    return dupStr(jsonToString(std::move(Out)));
  });
}

const char *neverd_comparison_source_difference_json(neverd_comparison_t Handle,
                                                     const char *Query) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    require(
        C.OwnedInputsBound,
        "source alignment requires completed native capture from run_owned");
    auto V = json(Query);
    const auto *O = V.getAsObject();
    require(O, "source difference query must be an object");
    keys(*O, {"side", "address", "representation", "offset", "limit",
              "max_native_instructions", "max_work", "max_source_bytes",
              "include_prelude", "timeout_ms", "refresh"});
    const auto [Secondary, Index] = selectedFunction(C, *O);
    const auto *M = Secondary ? C.Result.matchOfSecondary(Index)
                              : C.Result.matchOfPrimary(Index);
    require(M, "source alignment requires an accepted function counterpart");
    ComparisonSourceDifferenceOptions Options;
    Options.Source.Representation = sourceRepresentation(*O);
    Options.Source.MaxNativeInstructions =
        count(*O, "max_native_instructions",
              Options.Source.MaxNativeInstructions, UINT32_MAX);
    Options.Source.Refresh = flag(*O, "refresh", false);
    Options.Alignment.MaxWork =
        count(*O, "max_work", Options.Alignment.MaxWork, INT64_MAX);
    Options.MaxSourceBytes =
        count(*O, "max_source_bytes", Options.MaxSourceBytes, INT64_MAX);
    Options.IncludePrelude = flag(*O, "include_prelude", false);
    const auto Offset = count(*O, "offset", 0, INT64_MAX);
    const auto Limit = count(*O, "limit", diffLimits::kDefaultPageSize,
                             sdk::kNeverDCAPIDecompileNeverdIrViewJsonLimit);
    require(Limit > 0, "source difference page limit must be positive");
    const auto Timeout =
        count(*O, "timeout_ms", 0, diffLimits::kMaxTimeoutMilliseconds);
    const std::string Key =
        std::to_string(M->Primary) + ":" + std::to_string(M->Secondary) + ":" +
        Options.Source.Representation + ":" +
        std::to_string(Options.Source.MaxNativeInstructions) + ":" +
        std::to_string(Options.Alignment.MaxWork) + ":" +
        std::to_string(Options.MaxSourceBytes) + ":" +
        std::to_string(Options.IncludePrelude) + ":" + std::to_string(Timeout);
    {
      std::lock_guard Status(C.StatusMutex);
      C.Cancel = false;
      C.Running = true;
      C.Outcome = "running";
      C.Phase = "source_difference:analysis";
      C.Done = C.Total = 0;
      C.Error.clear();
    }
    auto Finished = llvm::scope_exit([&] {
      std::lock_guard Status(C.StatusMutex);
      C.Running = false;
      if (C.Outcome == "running")
        C.Outcome = "failed";
    });
    ComparisonControl Control;
    Control.Cancel = &C.Cancel;
    if (Timeout)
      Control.Deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(Timeout);
    Control.Progress = [&](llvm::StringRef Phase, uint64_t Done,
                           uint64_t Total) {
      std::lock_guard Status(C.StatusMutex);
      C.Phase = "source_difference:" + Phase.str();
      C.Done = Done;
      C.Total = Total;
    };
    const bool Cached =
        !Options.Source.Refresh && C.CachedSource && C.CachedSource->Key == Key;
    if (!Cached) {
      // Release the previous pair before collecting its replacement. The
      // evidence allowance applies to both inputs of this one retained pair.
      C.CachedSource.reset();
      auto Difference =
          compareSources(*toSession(C.PrimarySource.get()), C.Primary,
                         M->Primary, *toSession(C.SecondarySource.get()),
                         C.Secondary, M->Secondary, Options, Control);
      C.CachedSource = Comparison::SourceCache{Key, std::move(Difference)};
    }
    auto &Difference = C.CachedSource->Difference;
    llvm::json::Array Rows;
    const auto End =
        std::min<uint64_t>(Difference.Alignment.Rows.size(), Offset + Limit);
    const SourceListing Listings[] = {
        sourceListing(C, C.Primary, M, M->Primary, false),
        sourceListing(C, C.Secondary, M, M->Secondary, true)};
    const auto SourceLine = [&](unsigned Side,
                                uint64_t Line) -> llvm::json::Value {
      if (Line == NoSourceLine)
        return nullptr;
      const auto &Source = Difference.Documents[Side].Lines.at(Line);
      auto Row = Source.Evidence;
      Row["text"] = Source.Text;
      sourceNavigation(Row, Listings[Side]);
      return Row;
    };
    for (uint64_t I = Offset; I < End && !Control.stop(); ++I) {
      const auto &Pair = Difference.Alignment.Rows[I];
      Rows.push_back(
          llvm::json::Object{{"kind", sourceLineKindName(Pair.Kind)},
                             {"primary", SourceLine(0, Pair.Primary)},
                             {"secondary", SourceLine(1, Pair.Secondary)}});
    }
    if (auto Stop = Control.stop()) {
      Difference = {};
      Difference.Alignment.State = *Stop;
      Difference.Reason = "source difference paging stopped";
      Rows.clear();
    }
    const auto Metadata = [&](const Program &P, uint32_t FunctionIndex,
                              const ComparisonSourceDocument &Document) {
      return llvm::json::Object{
          {"function", function(C, P, FunctionIndex)},
          {"input_revision", P.Identity.Revision},
          {"input_sha256", P.Identity.SHA256},
          {"dialect", Document.Dialect},
          {"first_line", Document.FirstLine},
          {"total_lines", Document.TotalLines},
          {"native_analysis_complete",
           !P.Functions[FunctionIndex].has(TraitIncomplete)}};
    };
    llvm::json::Object Out{
        {"schema", "neverd.bindiff.source.difference"},
        {"version", 1},
        {"state", stateName(Difference.Alignment.State)},
        {"availability", Difference.Availability},
        {"reason", jsonSafeText(Difference.Reason)},
        {"computation_complete", Difference.Alignment.ComputationComplete},
        {"cached", Cached},
        {"representation", Options.Source.Representation},
        {"scope", Options.IncludePrelude ? "whole_emission" : "function_body"},
        {"recipe", std::string(VersionString) + ":sdk-source-difference-v2"},
        {"input_source_binding", "owned_sessions"},
        {"source_alignment", Difference.Alignment.ComputationComplete
                                 ? "exact_text_lines"
                                 : "not_completed"},
        {"provenance_complete", false},
        {"semantic_validation", "not_performed"},
        {"max_native_instructions", Options.Source.MaxNativeInstructions},
        {"max_work", Options.Alignment.MaxWork},
        {"max_source_bytes", Options.MaxSourceBytes},
        {"work", Difference.Alignment.Work},
        {"source_bytes", Difference.SourceBytes},
        {"timeout_ms", Timeout},
        {"hard_deadline", false},
        {"cancellation_granularity",
         "between_function_analysis_and_rendering_and_during_line_alignment"},
        {"primary", Metadata(C.Primary, M->Primary, Difference.Documents[0])},
        {"secondary",
         Metadata(C.Secondary, M->Secondary, Difference.Documents[1])},
        {"offset", Offset},
        {"limit", Limit},
        {"total_rows", Difference.Alignment.Rows.size()},
        {"rows", std::move(Rows)}};
    Out["next_offset"] = End < Difference.Alignment.Rows.size()
                             ? llvm::json::Value(End)
                             : llvm::json::Value(nullptr);
    {
      std::lock_guard Status(C.StatusMutex);
      C.Outcome = Difference.Availability == "failed"
                      ? "failed"
                      : stateName(Difference.Alignment.State).str();
    }
    return dupStr(jsonToString(std::move(Out)));
  });
}

const char *neverd_comparison_function_page_json(neverd_comparison_t Handle,
                                                 const char *Query) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    auto V = json(Query);
    const auto *O = V.getAsObject();
    require(O, "function query must be an object");
    keys(*O, {"side", "address", "offset", "limit", "timeout_ms", "refresh"});
    const auto [Secondary, Index] = selectedFunction(C, *O);
    const auto &P = Secondary ? C.Secondary : C.Primary;
    const auto *M = Secondary ? C.Result.matchOfSecondary(Index)
                              : C.Result.matchOfPrimary(Index);
    const auto Limit = count(*O, "limit", diffLimits::kDefaultPageSize,
                             diffLimits::kMaxPageSize);
    require(Limit > 0, "page limit must be positive");
    const auto Timeout =
        count(*O, "timeout_ms", 0, diffLimits::kMaxTimeoutMilliseconds);
    const auto Refresh = flag(*O, "refresh", false);
    const FunctionDelta *D =
        M ? &cachedDelta(C, *M, Timeout, Refresh) : nullptr;
    const uint64_t Total =
        D ? D->Rows.size() : P.instructionCount(P.Functions[Index]);
    const auto Offset = count(*O, "offset", 0, UINT32_MAX);
    require(Offset <= Total || (D && !D->ComputationComplete),
            "page offset exceeds retained instruction rows");
    const auto End = std::min(Offset + Limit, Total);
    llvm::json::Object Out{
        {"schema", "neverd.bindiff.function.page"},
        {"version", 1},
        {"total", Total},
        {"offset", Offset},
        {"next_offset", End < Total ? llvm::json::Value(End) : nullptr}};
    detailStatus(Out, D);
    Out["primary"] = M ? llvm::json::Value(function(C, C.Primary, M->Primary))
                     : !Secondary ? llvm::json::Value(function(C, P, Index))
                                  : nullptr;
    Out["secondary"] =
        M           ? llvm::json::Value(function(C, C.Secondary, M->Secondary))
        : Secondary ? llvm::json::Value(function(C, P, Index))
                    : nullptr;
    if (M)
      Out["evidence"] = match(*M);
    Out["pseudocode"] = llvm::json::Object{
        {"state", C.OwnedInputsBound ? "not_requested" : "unavailable"},
        {"reason",
         C.OwnedInputsBound
             ? "request source pages from the retained comparison inputs"
             : "source inspection requires comparison-owned inputs"}};
    llvm::json::Array Rows, Pairs;
    std::set<uint32_t> Blocks;
    if (D) {
      for (uint64_t I = Offset; I < End; ++I) {
        const auto &R = D->Rows[I];
        Rows.push_back(llvm::json::Object{
            {"kind", rowKindName(R.Kind)},
            {"primary_block", R.PrimaryBlock},
            {"secondary_block", R.SecondaryBlock},
            {"primary", R.Primary == NoIndex
                            ? llvm::json::Value(nullptr)
                            : instruction(C.Primary, R.Primary)},
            {"secondary", R.Secondary == NoIndex
                              ? llvm::json::Value(nullptr)
                              : instruction(C.Secondary, R.Secondary)}});
        if (R.PrimaryBlock != NoIndex)
          Blocks.insert(R.PrimaryBlock);
      }
      for (auto B : Blocks) {
        const auto Pair = D->PrimaryBlockPair[B];
        if (Pair != NoIndex) {
          const auto &BP = D->Blocks[Pair];
          Pairs.push_back(llvm::json::Object{{"primary", BP.Primary},
                                             {"secondary", BP.Secondary},
                                             {"step", stepName(BP.Step)},
                                             {"unchanged", BP.Unchanged},
                                             {"uncertain", BP.Uncertain}});
        }
      }
    } else {
      uint64_t I = 0;
      uint32_t BI = 0;
      for (const auto &B : P.blocks(P.Functions[Index])) {
        for (uint32_t N = 0; N < B.InstructionCount; ++N, ++I) {
          if (I < Offset || I >= End)
            continue;
          llvm::json::Object Row{{"kind", Secondary ? "inserted" : "deleted"},
                                 {"primary", nullptr},
                                 {"secondary", nullptr}};
          Row[Secondary ? "secondary" : "primary"] =
              instruction(P, B.FirstInstruction + N);
          Row[Secondary ? "secondary_block" : "primary_block"] = BI;
          Rows.push_back(std::move(Row));
        }
        ++BI;
      }
    }
    Out["rows"] = std::move(Rows);
    Out["block_pairs"] = std::move(Pairs);
    return dupStr(jsonToString(std::move(Out)));
  });
}

const char *neverd_comparison_graph_json(neverd_comparison_t Handle,
                                         const char *Query) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    auto V = json(Query);
    const auto *O = V.getAsObject();
    require(O, "graph query must be an object");
    keys(*O, {"side", "address", "node_limit", "edge_limit", "timeout_ms",
              "refresh"});
    const auto [Secondary, Index] = selectedFunction(C, *O);
    const auto &P = Secondary ? C.Secondary : C.Primary;
    const auto &F = P.Functions[Index];
    require(F.BlockCount <= count(*O, "node_limit",
                                  diffLimits::kDefaultGraphNodeLimit,
                                  diffLimits::kMaxInstructions) &&
                F.EdgeCount <= count(*O, "edge_limit",
                                     diffLimits::kDefaultGraphEdgeLimit,
                                     diffLimits::kMaxInstructions),
            "function graph exceeds the viewer's node or edge allowance");
    const auto *M = Secondary ? C.Result.matchOfSecondary(Index)
                              : C.Result.matchOfPrimary(Index);
    const auto Timeout =
        count(*O, "timeout_ms", 0, diffLimits::kMaxTimeoutMilliseconds);
    const auto Refresh = flag(*O, "refresh", false);
    const FunctionDelta *D =
        M ? &cachedDelta(C, *M, Timeout, Refresh) : nullptr;
    llvm::json::Array Nodes, Edges;
    uint32_t BI = 0;
    for (const auto &B : P.blocks(F)) {
      llvm::json::Array Preview;
      for (uint32_t I = 0; I < std::min(B.InstructionCount,
                                        diffLimits::kGraphPreviewInstructions);
           ++I) {
        const auto &Insn = P.Instructions[B.FirstInstruction + I];
        Preview.push_back((P.mnemonic(Insn) + " " + Insn.Operands).str());
      }
      std::string Change =
          D && !D->ComputationComplete ? "not_compared" : "unmatched";
      llvm::json::Value Counterpart = nullptr;
      if (D && D->ComputationComplete) {
        const auto Pair =
            Secondary ? D->SecondaryBlockPair[BI] : D->PrimaryBlockPair[BI];
        if (Pair != NoIndex) {
          const auto &BP = D->Blocks[Pair];
          Change = BP.Unchanged   ? "unchanged"
                   : BP.Uncertain ? "uncertain"
                                  : "changed";
          const auto &Other = Secondary ? C.Primary : C.Secondary;
          const auto &OtherF =
              Other.Functions[Secondary ? M->Primary : M->Secondary];
          Counterpart =
              vaHex(Other.blocks(OtherF)[Secondary ? BP.Primary : BP.Secondary]
                        .Address);
        }
      }
      Preview.insert(Preview.begin(), llvm::json::Value(Change));
      const auto End =
          B.InstructionCount
              ? P.Instructions[B.FirstInstruction + B.InstructionCount - 1]
                        .Address +
                    P.Instructions[B.FirstInstruction + B.InstructionCount - 1]
                        .Size
              : B.Address;
      Nodes.push_back(llvm::json::Object{
          {"id", BI++},
          {"start", vaHex(B.Address)},
          {"end", vaHex(End)},
          {"insn_count", B.InstructionCount},
          {"disasm", std::move(Preview)},
          {"comparison",
           llvm::json::Object{{"state", Change},
                              {"counterpart", std::move(Counterpart)}}}});
    }
    for (const auto &E : P.edges(F))
      Edges.push_back(llvm::json::Object{
          {"from", E.From},
          {"to", E.To},
          {"type", E.Kind == EdgeKind::Taken      ? "taken"
                   : E.Kind == EdgeKind::NotTaken ? "not_taken"
                                                  : "flow"}});
    llvm::json::Object Out{{"nodes", std::move(Nodes)},
                           {"edges", std::move(Edges)},
                           {"analysis_complete", !F.has(TraitIncomplete)}};
    detailStatus(Out, D);
    return dupStr(jsonToString(std::move(Out)));
  });
}

int neverd_comparison_review_record_json(neverd_comparison_t Handle,
                                         const char *Record) {
  return guarded(Handle, [&](Comparison &C) {
    auto Lock = ready(C);
    auto V = json(Record);
    const auto *O = V.getAsObject();
    require(O, "review record must be an object");
    keys(*O, {"primary", "secondary", "case", "author", "note", "reference",
              "assessment", "expected_head"});
    require(O->getString("expected_head").has_value(),
            "review record requires expected_head");
    ReviewDraft D;
    D.Primary = address(*O, "primary");
    D.Secondary = address(*O, "secondary");
    D.Case = string(*O, "case");
    D.Author = string(*O, "author");
    D.Note = string(*O, "note");
    D.Reference = string(*O, "reference");
    const auto Assessment = reviewAssessmentNamed(string(*O, "assessment"));
    require(Assessment.has_value(), "unknown analyst assessment");
    D.Assessment = *Assessment;
    if (const auto Error = appendReview(C.Result, C.Primary, C.Secondary, D,
                                        string(*O, "expected_head")))
      throw std::runtime_error(*Error);
    return 1;
  });
}

const char *neverd_comparison_reviews_json(neverd_comparison_t Handle,
                                           const char *Query) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    auto V = json(Query ? Query : "{}");
    const auto *O = V.getAsObject();
    require(O, "review query must be an object");
    keys(*O, {"case", "side", "address", "latest", "offset", "limit"});
    const auto Case = string(*O, "case");
    require(Case.size() <= diffLimits::kMaxReviewCaseBytes,
            "review case filter is too long");
    const bool LatestOnly = flag(*O, "latest", false);
    const auto Offset = count(*O, "offset", 0, UINT32_MAX);
    const auto Limit = count(*O, "limit", diffLimits::kDefaultPageSize,
                             diffLimits::kMaxPageSize);
    require(Limit, "page limit must be positive");
    require(!O->get("side") || O->get("address"),
            "review side filter requires an address");
    std::optional<std::pair<bool, uint32_t>> Selected;
    llvm::json::Value Selection = nullptr;
    if (O->get("address")) {
      Selected = selectedFunction(C, *O);
      const auto [Secondary, Index] = *Selected;
      const auto *M = Secondary ? C.Result.matchOfSecondary(Index)
                                : C.Result.matchOfPrimary(Index);
      const auto PF = Secondary ? (M ? M->Primary : NoIndex) : Index;
      const auto SF = Secondary ? Index : (M ? M->Secondary : NoIndex);
      Selection = llvm::json::Object{
          {"primary", PF == NoIndex ? llvm::json::Value(nullptr)
                                    : function(C, C.Primary, PF)},
          {"secondary", SF == NoIndex ? llvm::json::Value(nullptr)
                                      : function(C, C.Secondary, SF)},
          {"pair_active", M != nullptr}};
    }
    const auto &J = C.Result.Reviews;
    using Key = std::tuple<std::string, uint64_t, uint64_t>;
    const auto KeyOf = [](const ReviewRecord &R) {
      return Key{R.Case, R.Primary, R.Secondary};
    };
    std::map<Key, uint32_t> Latest;
    for (const auto &R : J.Records)
      Latest[KeyOf(R)] = R.Sequence;
    llvm::json::Array Rows;
    uint64_t Total = 0;
    for (auto It = J.Records.rbegin(); It != J.Records.rend(); ++It) {
      const auto &R = *It;
      const bool Current = Latest.at(KeyOf(R)) == R.Sequence;
      if ((!Case.empty() && Case != R.Case) || (LatestOnly && !Current))
        continue;
      if (Selected) {
        const auto [Secondary, Index] = *Selected;
        const auto Entry =
            (Secondary ? C.Secondary : C.Primary).Functions[Index].Address;
        if ((Secondary ? R.Secondary : R.Primary) != Entry)
          continue;
      }
      const auto Position = Total++;
      if (Position < Offset || Position >= Offset + Limit)
        continue;
      auto Row = reviewRecordJSON(R);
      Row["latest"] = Current;
      Row["pair_active"] =
          reviewPairActive(R, C.Result, C.Primary, C.Secondary);
      Rows.push_back(std::move(Row));
    }
    require(Offset <= Total, "review page offset exceeds history");
    const auto Returned = Rows.size();
    return dupStr(jsonToString(llvm::json::Object{
        {"schema", "neverd.bindiff.reviews"},
        {"version", 1},
        {"profile", ReviewJournalProfile},
        {"head", J.head()},
        {"primary_sha256", C.Primary.Identity.SHA256},
        {"secondary_sha256", C.Secondary.Identity.SHA256},
        {"primary_revision", C.Primary.Identity.Revision},
        {"secondary_revision", C.Secondary.Identity.Revision},
        {"selection", std::move(Selection)},
        {"record_count", J.Records.size()},
        {"case", Case},
        {"latest_only", LatestOnly},
        {"total", Total},
        {"offset", Offset},
        {"next_offset", Offset + Returned < Total
                            ? llvm::json::Value(Offset + Returned)
                            : nullptr},
        {"assessment_source", "analyst"},
        {"author_authentication", "not_performed"},
        {"semantic_validation", "not_performed"},
        {"rows", std::move(Rows)}}));
  });
}

static const char *scalarValidation(neverd_comparison_t Handle,
                                    neverd_session_t A, neverd_session_t B,
                                    const char *Query, bool Owned) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    if (Owned) {
      require(C.OwnedInputsBound, "owned validation requires a completed "
                                  "native capture from run_owned");
      A = C.PrimarySource.get();
      B = C.SecondarySource.get();
    }
    require(A && B, "scalar validation requires two live input sessions");
    auto V = json(Query);
    const auto *O = V.getAsObject();
    require(O, "scalar validation query must be an object");
    keys(*O, {"side", "address", "max_native_instructions", "max_work",
              "timeout_ms"});
    const auto [Secondary, Index] = selectedFunction(C, *O);
    const auto *M = Secondary ? C.Result.matchOfSecondary(Index)
                              : C.Result.matchOfPrimary(Index);
    require(M, "scalar validation requires an accepted counterpart");
    ScalarValidationOptions Options;
    Options.MaxNativeInstructions =
        count(*O, "max_native_instructions", Options.MaxNativeInstructions,
              diffLimits::kMaxValidationNativeInstructions);
    Options.MaxWork =
        count(*O, "max_work", Options.MaxWork, diffLimits::kMaxValidationWork);
    if (auto Error = Options.validate())
      throw std::runtime_error(*Error);
    const auto Timeout =
        count(*O, "timeout_ms", 0, diffLimits::kMaxTimeoutMilliseconds);
    {
      std::lock_guard Status(C.StatusMutex);
      C.Cancel = false;
      C.Running = true;
      C.Outcome = "running";
      C.Phase = Owned ? "validation:owned_inputs" : "validation:inputs";
      C.Done = 0;
      C.Total = 2;
      C.Error.clear();
    }
    auto Finished = llvm::scope_exit([&] {
      std::lock_guard Status(C.StatusMutex);
      C.Running = false;
      if (C.Outcome == "running")
        C.Outcome = "failed";
    });
    ComparisonControl Control;
    Control.Cancel = &C.Cancel;
    if (Timeout)
      Control.Deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(Timeout);
    Control.Progress = [&](llvm::StringRef Phase, uint64_t Done,
                           uint64_t Total) {
      std::lock_guard Status(C.StatusMutex);
      C.Phase = "validation:" + Phase.str();
      C.Done = Done;
      C.Total = Total;
    };
    ComparisonScalarValidation Checked;
    bool Bound = false;
    std::string Error;
    auto PO = C.PrimaryOptions, SO = C.SecondaryOptions;
    PO.DataFlow.Enabled = SO.DataFlow.Enabled = false;
    const auto Rebind =
        [&](auto Handle, const auto &Settings,
            const Program &Expected) -> std::optional<ComparisonState> {
      // Release each full temporary snapshot before reading the other input
      // or lifting. Both identities are still freshly checked on every query.
      auto Input =
          readComparisonInput(*toSession(Handle), Settings, Control, Error);
      require(Input.has_value(), Error);
      if (Input->Stopped)
        return Input->Stopped;
      require(Expected.Identity.Revision == Input->Program.Identity.Revision,
              "stale comparison: input or analysis revision changed");
      return {};
    };
    auto Stopped = Control.stop();
    if (!Owned && !Stopped) {
      Stopped = Rebind(A, PO, C.Primary);
      if (!Stopped)
        Stopped = Rebind(B, SO, C.Secondary);
    }
    if (Stopped) {
      Checked.Result.State = *Stopped;
      Checked.Result.Proof.Status =
          Checked.Result.State == ComparisonState::ResourceLimited
              ? analysis::LLVMScalarEquivalenceStatus::BudgetExceeded
              : analysis::LLVMScalarEquivalenceStatus::Cancelled;
      Checked.Result.Proof.Diagnostic =
          Owned ? "scalar validation stopped" : "input rebinding stopped";
    } else {
      Bound = true;
      Checked = validateComparisonScalars(*toSession(A), C.Primary, M->Primary,
                                          *toSession(B), C.Secondary,
                                          M->Secondary, Options, Control);
    }
    const auto &R = Checked.Result;
    const auto Lift = [](const ScalarLiftSummary &S) {
      return llvm::json::Object{{"decoded_instructions", S.Decoded},
                                {"lifted_instructions", S.Lifted},
                                {"verified_native_body", S.Verified},
                                {"llvm_sha256", S.SHA256.empty()
                                                    ? llvm::json::Value(nullptr)
                                                    : S.SHA256}};
    };
    llvm::json::Array Arguments, Bits;
    for (const auto &Arg : R.Arguments)
      Arguments.push_back(
          llvm::json::Object{{"ordinal", Arguments.size()},
                             {"register_offset", Arg.RegisterOffset},
                             {"bits", Arg.Bits}});
    for (const auto &Bit : R.Proof.ControlBits)
      Bits.push_back(
          llvm::json::Object{{"argument", Bit.Argument}, {"bit", Bit.Bit}});
    const analysis::LLVMScalarEquivalenceLimits ProofLimits;
    llvm::json::Object Out{
        {"schema", "neverd.bindiff.scalar_validation"},
        {"version", 1},
        {"profile", ScalarValidationProfile},
        {"state", stateName(R.State)},
        {"status", scalarValidationStatusName(R.Proof.Status)},
        {"diagnostic", R.Proof.Diagnostic},
        {"primary", function(C, C.Primary, M->Primary)},
        {"secondary", function(C, C.Secondary, M->Secondary)},
        {"primary_revision", C.Primary.Identity.Revision},
        {"secondary_revision", C.Secondary.Identity.Revision},
        {"primary_sha256", C.Primary.Identity.SHA256},
        {"secondary_sha256", C.Secondary.Identity.SHA256},
        {"abi", C.Primary.Identity.ABI},
        {"input_source_binding",
         Owned ? "owned_sessions" : "revalidated_sessions"},
        {"inputs_bound", Bound},
        {"inputs_revalidated", Bound && !Owned},
        {"primary_lift", Lift(Checked.Primary)},
        {"secondary_lift", Lift(Checked.Secondary)},
        {"lift_recipe",
         std::string(VersionString) + ":machine-abi:no-opt:mem2reg"},
        {"proof_scope", "lifted_integer_returns_and_defined_termination"},
        {"input_domain", "defined_bitvectors"},
        {"input_binding", "identical_ordered_gpr_locations_and_widths"},
        {"exception_metadata", "absent_or_canonical_source_unwind_only"},
        {"arguments", std::move(Arguments)},
        {"result_bits",
         R.ResultBits ? llvm::json::Value(R.ResultBits) : nullptr},
        {"work", R.Proof.Work},
        {"work_limit_exceeded", R.Proof.WorkLimitExceeded},
        {"attempts", R.Proof.Attempts},
        {"completed_partitions", R.Proof.CompletedPartitions},
        {"control_bits", std::move(Bits)},
        {"limits",
         llvm::json::Object{
             {"native_instructions_per_function",
              Options.MaxNativeInstructions},
             {"proof_work", Options.MaxWork},
             {"model_input_items", ProofLimits.Model.MaxInputItems},
             {"unwind_projection_input_items", ProofLimits.Model.MaxInputItems},
             {"model_blocks", ProofLimits.Model.MaxBlocks},
             {"model_operations", ProofLimits.Model.MaxOperations},
             {"model_work_per_function", ProofLimits.Model.MaxWork},
             {"control_bits", ProofLimits.MaxControlBits},
             {"partitions", ProofLimits.MaxPartitions},
             {"block_visits", ProofLimits.MaxBlockVisits},
             {"symbolic_nodes", ProofLimits.MaxSymbolicNodes},
             {"timeout_ms", Timeout}}},
        {"native_state_equivalence", "not_proved"},
        {"lifting_correctness", "assumed"},
        {"memory_and_call_effects", "unsupported"},
        {"target_execution", "not_performed"},
        {"security_assessment", "not_performed"},
        {"unproved_means_inequivalent", false},
        {"persistent_certificate", false}};
    {
      std::lock_guard Status(C.StatusMutex);
      C.Outcome = stateName(R.State).str();
      C.Done = R.Proof.Work;
    }
    return dupStr(jsonToString(std::move(Out)));
  });
}

const char *neverd_comparison_scalar_validation_json(neverd_comparison_t Handle,
                                                     neverd_session_t A,
                                                     neverd_session_t B,
                                                     const char *Query) {
  return scalarValidation(Handle, A, B, Query, false);
}

const char *
neverd_comparison_owned_scalar_validation_json(neverd_comparison_t Handle,
                                               const char *Query) {
  return scalarValidation(Handle, nullptr, nullptr, Query, true);
}

const char *neverd_comparison_observations_json(neverd_comparison_t Handle,
                                                const char *Query) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    auto V = json(Query);
    const auto *O = V.getAsObject();
    require(O, "change-observation query must be an object");
    keys(*O, {"side", "address", "context", "max_rows", "max_observations",
              "max_work", "timeout_ms", "refresh", "offset", "limit"});
    const auto [Secondary, Index] = selectedFunction(C, *O);
    const auto *M = Secondary ? C.Result.matchOfSecondary(Index)
                              : C.Result.matchOfPrimary(Index);
    require(M, "change observations require an accepted counterpart");
    ChangeObservationOptions Options;
    Options.MaxRows =
        count(*O, "max_rows", Options.MaxRows, diffLimits::kMaxObservationRows);
    Options.MaxObservations =
        count(*O, "max_observations", Options.MaxObservations,
              diffLimits::kMaxObservations);
    Options.MaxWork =
        count(*O, "max_work", Options.MaxWork, diffLimits::kMaxObservationWork);
    Options.Graph = C.Result.Settings.DataFlow.Graph;
    if (const auto Error = Options.validate())
      throw std::runtime_error(*Error);
    const auto Timeout =
        count(*O, "timeout_ms", 0, diffLimits::kMaxTimeoutMilliseconds);
    const bool Refresh = flag(*O, "refresh", false);
    const auto Offset = count(*O, "offset", 0, UINT32_MAX);
    const auto Limit = count(*O, "limit", diffLimits::kDefaultPageSize,
                             diffLimits::kMaxPageSize);
    require(Limit, "page limit must be positive");
    const auto Context = string(*O, "context", "all");
    uint32_t ContextMask = 0;
    for (auto Bit : {ContextPredicate, ContextReturn, ContextCallArgument,
                     ContextMemoryWrite})
      if (observationContextNames(Bit).front() == Context)
        ContextMask = Bit;
    require(Context == "all" || ContextMask,
            "unknown change-observation context");
    const auto *AE = C.Result.PrimaryDataFlow
                         ? C.Result.PrimaryDataFlow->find(
                               C.Primary.Functions[M->Primary].Address)
                         : nullptr;
    const auto *BE = C.Result.SecondaryDataFlow
                         ? C.Result.SecondaryDataFlow->find(
                               C.Secondary.Functions[M->Secondary].Address)
                         : nullptr;
    const auto *AG = AE && AE->Graph ? &*AE->Graph : nullptr;
    const auto *BG = BE && BE->Graph ? &*BE->Graph : nullptr;
    llvm::json::Object Recipe{{"primary", M->Primary},
                              {"secondary", M->Secondary},
                              {"max_rows", Options.MaxRows},
                              {"max_observations", Options.MaxObservations},
                              {"max_work", Options.MaxWork},
                              {"timeout_ms", Timeout}};
    const auto Key = jsonToString(llvm::json::Object(Recipe));
    if (Refresh || !C.CachedObservations || C.CachedObservations->Key != Key) {
      {
        std::lock_guard Status(C.StatusMutex);
        C.Cancel = false;
        C.Running = true;
        C.Outcome = "running";
        C.Phase = "changes:detail";
        C.Done = 0;
        C.Total =
            C.Primary.instructionCount(C.Primary.Functions[M->Primary]) +
            C.Secondary.instructionCount(C.Secondary.Functions[M->Secondary]);
        C.Error.clear();
      }
      auto Finished = llvm::scope_exit([&] {
        std::lock_guard Status(C.StatusMutex);
        C.Running = false;
        if (C.Outcome == "running")
          C.Outcome = "failed";
      });
      ComparisonControl Control;
      Control.Cancel = &C.Cancel;
      if (Timeout)
        Control.Deadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(Timeout);
      const auto &D = cachedDelta(C, *M, Timeout, Refresh, &Control);
      {
        std::lock_guard Status(C.StatusMutex);
        C.Phase = "changes:observations";
        C.Total = D.Rows.size();
      }
      C.CachedObservations = Comparison::ObservationCache{
          Key,
          observeFunctionChanges(C.Primary, M->Primary, C.Secondary,
                                 M->Secondary, D, AG, BG, Options, Control)};
      std::lock_guard Status(C.StatusMutex);
      C.Outcome = stateName(C.CachedObservations->Evidence.State).str();
      C.Done = C.CachedObservations->Evidence.VisitedRows;
    }
    const auto &R = C.CachedObservations->Evidence;
    const auto Names = [](const std::vector<llvm::StringRef> &Values) {
      llvm::json::Array Out;
      for (auto Name : Values)
        Out.push_back(Name);
      return Out;
    };
    llvm::json::Array Rows;
    uint64_t Total = 0;
    for (uint32_t I = 0; I < R.Observations.size(); ++I) {
      const auto &Fact = R.Observations[I];
      if (ContextMask &&
          !((Fact.PrimaryContext | Fact.SecondaryContext) & ContextMask))
        continue;
      const auto Position = Total++;
      if (Position < Offset || Position >= Offset + Limit)
        continue;
      llvm::json::Object Row{
          {"id", I},
          {"native_row", Fact.Row},
          {"native_kind", rowKindName(Fact.NativeKind)},
          {"priority", Fact.Priority},
          {"association", Fact.PrimaryNode == NoIndex ? "native_only"
                          : Fact.PrimaryNode == Fact.PrimaryAnchorNode
                              ? "native_origin"
                              : "direct_synthetic_operand"},
          {"primary_anchor_node", Fact.PrimaryAnchorNode == NoIndex
                                      ? llvm::json::Value(nullptr)
                                      : Fact.PrimaryAnchorNode},
          {"secondary_anchor_node", Fact.SecondaryAnchorNode == NoIndex
                                        ? llvm::json::Value(nullptr)
                                        : Fact.SecondaryAnchorNode},
          {"kinds", Names(observationKindNames(Fact.Kinds))},
          {"primary_context",
           Names(observationContextNames(Fact.PrimaryContext))},
          {"secondary_context",
           Names(observationContextNames(Fact.SecondaryContext))}};
      Row["primary"] = Fact.PrimaryInstruction == NoIndex
                           ? llvm::json::Value(nullptr)
                           : instruction(C.Primary, Fact.PrimaryInstruction);
      Row["secondary"] =
          Fact.SecondaryInstruction == NoIndex
              ? llvm::json::Value(nullptr)
              : instruction(C.Secondary, Fact.SecondaryInstruction);
      Row["primary_node"] = Fact.PrimaryNode == NoIndex
                                ? llvm::json::Value(nullptr)
                                : dataFlowNodeJSON(*AG, Fact.PrimaryNode);
      Row["secondary_node"] = Fact.SecondaryNode == NoIndex
                                  ? llvm::json::Value(nullptr)
                                  : dataFlowNodeJSON(*BG, Fact.SecondaryNode);
      Rows.push_back(std::move(Row));
    }
    require(Offset <= Total || R.State != ComparisonState::Complete,
            "observation page offset exceeds retained rows");
    const auto SSAStatus = [](const DataFlowEntry *Entry,
                              const DataFlowEvidence *Evidence, bool Indexed) {
      const bool UnsupportedProfile =
          Entry && Entry->Graph && Entry->Graph->Profile != DataFlowProfile;
      return llvm::json::Object{
          {"availability", UnsupportedProfile ? "unsupported_profile"
                           : Entry    ? availabilityName(Entry->Availability)
                           : Evidence ? "outside_collection"
                                      : "not_requested"},
          {"reason",
           UnsupportedProfile
               ? "native change associations require the med-ssa-v2 profile"
           : Entry ? Entry->Reason
                   : ""},
          {"indexed", Indexed},
          {"recipe", Evidence ? Evidence->Recipe : ""}};
    };
    const auto Returned = Rows.size();
    return dupStr(jsonToString(llvm::json::Object{
        {"schema", "neverd.bindiff.observations"},
        {"version", 1},
        {"profile", ChangeObservationProfile},
        {"anchor_scope", "aligned_native_rows"},
        {"dependency_scope", "local_and_unique_direct_synthetic_operands"},
        {"state", stateName(R.State)},
        {"primary", function(C, C.Primary, M->Primary)},
        {"secondary", function(C, C.Secondary, M->Secondary)},
        {"primary_sha256", C.Primary.Identity.SHA256},
        {"secondary_sha256", C.Secondary.Identity.SHA256},
        {"pairing", match(*M)},
        {"query", std::move(Recipe)},
        {"native_analysis_complete", R.NativeAnalysisComplete},
        {"reference_comparison_complete", R.ReferenceComparisonComplete},
        {"primary_ssa",
         SSAStatus(AE, C.Result.PrimaryDataFlow.get(), R.PrimarySSA)},
        {"secondary_ssa",
         SSAStatus(BE, C.Result.SecondaryDataFlow.get(), R.SecondarySSA)},
        {"primary_gaps", Names(flowGapNames(R.PrimaryGaps))},
        {"secondary_gaps", Names(flowGapNames(R.SecondaryGaps))},
        {"total_native_rows", R.TotalRows},
        {"visited_native_rows", R.VisitedRows},
        {"ambiguous_origins", R.AmbiguousOrigins},
        {"work", R.Work},
        {"observation_count", R.Observations.size()},
        {"context", Context},
        {"total", Total},
        {"offset", Offset},
        {"next_offset", Offset + Returned < Total
                            ? llvm::json::Value(Offset + Returned)
                            : nullptr},
        {"heuristic_ranking", true},
        {"semantic_validation", "not_performed"},
        {"security_inference", "not_performed"},
        {"fix_presence", "not_assessed"},
        {"rows", std::move(Rows)}}));
  });
}

const char *neverd_comparison_dataflow_json(neverd_comparison_t Handle,
                                            const char *Query) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    auto Value = json(Query ? Query : "{}");
    const auto *O = Value.getAsObject();
    require(O, "dataflow query must be an object");
    keys(*O, {"side", "kind", "address", "offset", "limit", "call",
              "max_records", "max_work", "timeout_ms", "refresh"});
    const auto Side = string(*O, "side", "primary");
    require(Side == "primary" || Side == "secondary", "invalid dataflow side");
    const auto *E = Side == "primary" ? C.Result.PrimaryDataFlow.get()
                                      : C.Result.SecondaryDataFlow.get();
    const auto &P = Side == "primary" ? C.Primary : C.Secondary;
    require(E, "dataflow was not requested; compare with dataflow:true");
    const auto Offset = count(*O, "offset", 0, UINT32_MAX);
    const auto Limit = count(*O, "limit", diffLimits::kDefaultPageSize,
                             diffLimits::kMaxPageSize);
    const auto Kind = string(*O, "kind", "functions");
    require(Kind == "functions" || Kind == "nodes" || Kind == "blocks" ||
                Kind == "accesses",
            "invalid dataflow page kind");
    require(Limit, "page limit must be positive");
    require(Kind == "accesses" || (!O->get("call") && !O->get("max_records") &&
                                   !O->get("max_work") &&
                                   !O->get("timeout_ms") && !O->get("refresh")),
            "argument access selectors require kind:accesses");
    llvm::json::Object Out;
    if (Kind == "functions") {
      require(!O->get("address"), "function inventory has no address selector");
      Out = dataFlowEntriesJSON(*E, Offset, Limit);
    } else {
      const auto Address = address(*O, "address");
      const auto *Row = E->find(Address);
      require(Row, "function was outside the retained dataflow scope");
      if (Kind == "accesses") {
        require(Row->Graph.has_value(), Row->Reason);
        const auto &G = *Row->Graph;
        require(G.Profile == DataFlowProfile,
                "argument accesses require med-ssa-v2");
        require(O->get("call"), "argument accesses require a call node");
        const auto Call = count(*O, "call", 0, UINT32_MAX);
        require(Call < G.Nodes.size() && (G.Nodes[Call].Opcode == "CALL" ||
                                          G.Nodes[Call].Opcode == "INDIR_CALL"),
                "argument access selector is not a call node");
        FlowArgumentAccessOptions Options;
        Options.MaxRecords = count(*O, "max_records", Options.MaxRecords,
                                   diffLimits::kMaxFlowAccessRecords);
        Options.MaxWork =
            count(*O, "max_work", Options.MaxWork, diffLimits::kMaxFlowWork);
        if (auto Error = Options.validate())
          throw std::runtime_error(*Error);
        const auto Timeout =
            count(*O, "timeout_ms", 0, diffLimits::kMaxTimeoutMilliseconds);
        const bool Refresh = flag(*O, "refresh", false);
        const auto Key =
            jsonToString(llvm::json::Object{{"side", Side},
                                            {"address", vaHex(Address)},
                                            {"max_records", Options.MaxRecords},
                                            {"max_work", Options.MaxWork},
                                            {"timeout_ms", Timeout}});
        if (Refresh || !C.CachedArgumentAccesses ||
            C.CachedArgumentAccesses->Key != Key) {
          {
            std::lock_guard Status(C.StatusMutex);
            C.Cancel = false;
            C.Running = true;
            C.Outcome = "running";
            C.Phase = "dataflow:argument_accesses";
            C.Done = 0;
            C.Total = G.Nodes.size();
            C.Error.clear();
          }
          auto Finished = llvm::scope_exit([&] {
            std::lock_guard Status(C.StatusMutex);
            C.Running = false;
            if (C.Outcome == "running")
              C.Outcome = "failed";
          });
          ComparisonControl Control;
          Control.Cancel = &C.Cancel;
          if (Timeout)
            Control.Deadline = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(Timeout);
          auto Evidence = observeFlowArgumentAccesses(
              G, Options, C.Result.Settings.DataFlow.Graph, Control);
          C.CachedArgumentAccesses =
              Comparison::ArgumentAccessCache{Key, std::move(Evidence)};
          std::lock_guard Status(C.StatusMutex);
          C.Outcome = stateName(C.CachedArgumentAccesses->Evidence.State).str();
          if (C.CachedArgumentAccesses->Evidence.Complete)
            C.Done = C.Total;
        }
        const auto &R = C.CachedArgumentAccesses->Evidence;
        const auto Begin = std::lower_bound(
            R.Records.begin(), R.Records.end(), Call,
            [](const auto &Record, auto Node) { return Record.Call < Node; });
        const auto End = std::upper_bound(
            Begin, R.Records.end(), Call,
            [](auto Node, const auto &Record) { return Node < Record.Call; });
        const uint64_t Total = End - Begin;
        llvm::json::Array Rows, Gaps;
        for (auto Gap : flowGapNames(R.Gaps))
          Gaps.push_back(Gap);
        for (uint64_t I = std::min(Offset, Total);
             I < std::min(Offset + Limit, Total); ++I) {
          const auto &A = Begin[I];
          auto Access = dataFlowNodeJSON(G, A.Access);
          Access["call"] = A.Call;
          Access["argument"] = A.Argument;
          Access["access"] = A.Write ? "write" : "read";
          Access["access_bytes"] = A.Bytes;
          Access["return_operand_path"] = A.ReturnOperandPath;
          Rows.push_back(std::move(Access));
        }
        const auto Returned = Rows.size();
        Out = llvm::json::Object{
            {"function", vaHex(Address)},
            {"observation_profile", FlowArgumentAccessProfile},
            {"state", stateName(R.State)},
            {"inspection_complete", R.Complete},
            {"work", R.Work},
            {"call", dataFlowNodeJSON(G, Call)},
            {"scope", "shared_ssa_or_parameter_address"},
            {"execution_order", "not_assessed"},
            {"callee_memory_effects", "not_proved"},
            {"alias_completeness", "not_proved"},
            {"gaps", std::move(Gaps)},
            {"offset", Offset},
            {"total", Total},
            {"next_offset", Offset + Returned < Total
                                ? llvm::json::Value(Offset + Returned)
                                : nullptr},
            {"rows", std::move(Rows)}};
      } else if (Row->Graph)
        Out = dataFlowPageJSON(*Row->Graph, Kind == "blocks", Offset, Limit);
      else {
        require(Limit, "page limit must be positive");
        Out = llvm::json::Object{{"function", vaHex(Address)},
                                 {"offset", Offset},
                                 {"total", 0},
                                 {"rows", llvm::json::Array{}}};
      }
      Out["availability"] = availabilityName(Row->Availability);
      Out["reason"] = Row->Reason;
      Out["function_info"] = function(C, P, *P.functionAt(Address));
    }
    Out["schema"] = "neverd.bindiff.dataflow.page";
    Out["schema_version"] =
        dataFlowSchemaVersion(C.Result.Settings.DataFlow.Profile);
    if (isLLVMDataFlowProfile(C.Result.Settings.DataFlow.Profile))
      Out["profile"] = C.Result.Settings.DataFlow.Profile;
    Out["side"] = Side;
    Out["revision"] = P.Identity.Revision;
    Out["semantic_validation"] = "not_performed";
    return dupStr(jsonToString(std::move(Out)));
  });
}

const char *neverd_comparison_slice_json(neverd_comparison_t Handle,
                                         const char *Query) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    auto Value = json(Query);
    const auto *O = Value.getAsObject();
    require(O, "slice query must be an object");
    keys(*O, {"side", "address", "seeds", "direction", "control", "timeout_ms",
              "offset", "limit", "refresh"});
    const auto Side = string(*O, "side", "primary");
    require(Side == "primary" || Side == "secondary", "invalid slice side");
    const auto *E = Side == "primary" ? C.Result.PrimaryDataFlow.get()
                                      : C.Result.SecondaryDataFlow.get();
    require(E, "dataflow was not requested; compare with dataflow:true");
    const auto Address = address(*O, "address");
    const auto *Row = E->find(Address);
    require(Row && Row->Graph,
            Row ? Row->Reason
                : "function was outside the retained dataflow scope");
    const auto *SeedValues = O->getArray("seeds");
    require(SeedValues && !SeedValues->empty() &&
                SeedValues->size() <= diffLimits::kMaxPageSize,
            "slice requires a bounded, nonempty node seed array");
    std::vector<uint32_t> Seeds;
    for (const auto &V : *SeedValues) {
      auto N = V.getAsInteger();
      require(N && *N >= 0 && uint64_t(*N) < Row->Graph->Nodes.size(),
              "invalid slice seed");
      Seeds.push_back(*N);
    }
    std::sort(Seeds.begin(), Seeds.end());
    Seeds.erase(std::unique(Seeds.begin(), Seeds.end()), Seeds.end());
    const auto Direction = string(*O, "direction", "backward");
    require(Direction == "backward" || Direction == "forward",
            "invalid slice direction");
    const bool IncludeControl = flag(*O, "control", true);
    const auto Timeout =
        count(*O, "timeout_ms", 0, diffLimits::kMaxTimeoutMilliseconds);
    const auto Offset = count(*O, "offset", 0, UINT32_MAX);
    const auto Limit = count(*O, "limit", diffLimits::kDefaultPageSize,
                             diffLimits::kMaxPageSize);
    require(Limit, "page limit must be positive");
    const bool Refresh = flag(*O, "refresh", false);
    llvm::json::Array SeedArray;
    for (auto N : Seeds)
      SeedArray.push_back(N);
    const auto Key =
        jsonToString(llvm::json::Object{{"side", Side},
                                        {"address", vaHex(Address)},
                                        {"direction", Direction},
                                        {"control", IncludeControl},
                                        {"timeout_ms", Timeout},
                                        {"seeds", std::move(SeedArray)}});
    if (Refresh || !C.CachedSlice || C.CachedSlice->Key != Key) {
      {
        std::lock_guard Status(C.StatusMutex);
        C.Cancel = false;
        C.Running = true;
        C.Outcome = "running";
        C.Phase = "dataflow:slice";
        C.Done = 0;
        C.Total = Row->Graph->Nodes.size();
        C.Error.clear();
      }
      auto Finished = llvm::scope_exit([&] {
        std::lock_guard Status(C.StatusMutex);
        C.Running = false;
        if (C.Outcome == "running")
          C.Outcome = "failed";
      });
      ComparisonControl Control;
      Control.Cancel = &C.Cancel;
      if (Timeout)
        Control.Deadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(Timeout);
      auto Slice = sliceDataFlow(
          *Row->Graph, Seeds,
          Direction == "backward" ? SliceDirection::Backward
                                  : SliceDirection::Forward,
          IncludeControl, C.Result.Settings.DataFlow.Graph, Control);
      C.CachedSlice = Comparison::SliceCache{Key, std::move(Slice)};
      std::lock_guard Status(C.StatusMutex);
      C.Outcome = stateName(C.CachedSlice->Slice.State).str();
      C.Done = C.CachedSlice->Slice.Nodes.size();
    }
    auto Out =
        dataFlowSliceJSON(*Row->Graph, C.CachedSlice->Slice, Offset, Limit);
    Out["schema"] = "neverd.bindiff.dataflow.slice";
    Out["side"] = Side;
    Out["direction"] = Direction;
    return dupStr(jsonToString(std::move(Out)));
  });
}

const char *neverd_comparison_impact_json(neverd_comparison_t Handle,
                                          const char *Query) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    auto V = json(Query);
    const auto *O = V.getAsObject();
    require(O, "impact query must be an object");
    keys(*O, {"side", "seeds", "direction", "kind", "witness", "max_depth",
              "max_functions", "max_sites", "max_work", "timeout_ms", "offset",
              "limit", "refresh", "max_effect_records", "max_effect_work"});
    const auto Side = string(*O, "side", "primary");
    require(Side == "primary" || Side == "secondary", "invalid impact side");
    const auto &P = Side == "primary" ? C.Primary : C.Secondary;
    const auto Direction = string(*O, "direction", "callers");
    require(Direction == "callers" || Direction == "callees",
            "invalid impact direction");
    const auto Kind = string(*O, "kind", "functions");
    require(Kind == "functions" || Kind == "entrypoints" || Kind == "sites" ||
                Kind == "boundaries" || Kind == "frontier" || Kind == "path" ||
                Kind == "effects",
            "invalid impact page kind");
    const bool WithEffects = Kind == "effects";
    require(WithEffects ||
                (!O->get("max_effect_records") && !O->get("max_effect_work")),
            "effect allowances require the effects page");
    CallEffectReviewOptions EffectOptions;
    EffectOptions.Graph = C.Result.Settings.DataFlow.Graph;
    EffectOptions.MaxRecords =
        count(*O, "max_effect_records", EffectOptions.MaxRecords,
              diffLimits::kMaxEffectRecords);
    EffectOptions.MaxWork = count(*O, "max_effect_work", EffectOptions.MaxWork,
                                  diffLimits::kMaxEffectWork);
    if (auto Error = EffectOptions.validate())
      throw std::runtime_error(*Error);
    require((Kind == "path") == bool(O->get("witness")),
            "impact path requires a witness row; other pages cannot select it");
    ImpactOptions Options;
    Options.MaxDepth =
        count(*O, "max_depth", Options.MaxDepth, diffLimits::kMaxImpactDepth);
    Options.MaxFunctions = count(*O, "max_functions", Options.MaxFunctions,
                                 diffLimits::kMaxImpactFunctions);
    Options.MaxSites =
        count(*O, "max_sites", Options.MaxSites, diffLimits::kMaxImpactSites);
    Options.MaxWork =
        count(*O, "max_work", Options.MaxWork, diffLimits::kMaxImpactWork);
    if (auto Error = Options.validate())
      throw std::runtime_error(*Error);
    const auto *SeedValues = O->getArray("seeds");
    require(SeedValues && !SeedValues->empty() &&
                SeedValues->size() <=
                    std::min(Options.MaxFunctions, diffLimits::kMaxImpactSeeds),
            "impact requires a bounded nonempty array of function addresses");
    std::vector<uint32_t> Seeds;
    for (const auto &Value : *SeedValues) {
      const auto Text = Value.getAsString();
      uint64_t VA = 0;
      require(Text && !Text->getAsInteger(0, VA),
              "invalid impact seed address");
      const auto FI = P.functionAt(VA);
      require(FI.has_value(), "impact seed is outside the comparison snapshot");
      Seeds.push_back(*FI);
    }
    llvm::sort(Seeds);
    Seeds.erase(std::unique(Seeds.begin(), Seeds.end()), Seeds.end());
    llvm::json::Array SeedArray;
    for (auto FI : Seeds)
      SeedArray.push_back(vaHex(P.Functions[FI].Address));
    const auto Timeout =
        count(*O, "timeout_ms", 0, diffLimits::kMaxTimeoutMilliseconds);
    const auto Offset = count(*O, "offset", 0, UINT32_MAX);
    const auto Limit = count(*O, "limit", diffLimits::kDefaultPageSize,
                             diffLimits::kMaxPageSize);
    require(Limit, "page limit must be positive");
    const bool Refresh = flag(*O, "refresh", false);
    llvm::json::Object Recipe{{"side", Side},
                              {"direction", Direction},
                              {"seeds", std::move(SeedArray)},
                              {"max_depth", Options.MaxDepth},
                              {"max_functions", Options.MaxFunctions},
                              {"max_sites", Options.MaxSites},
                              {"max_work", Options.MaxWork},
                              {"timeout_ms", Timeout}};
    if (WithEffects)
      Recipe["effects"] =
          llvm::json::Object{{"profile", CallEffectReviewProfile},
                             {"max_records", EffectOptions.MaxRecords},
                             {"max_work", EffectOptions.MaxWork}};
    const auto Key = jsonToString(llvm::json::Object(Recipe));
    if (Refresh || !C.CachedImpact || C.CachedImpact->Key != Key) {
      {
        std::lock_guard Status(C.StatusMutex);
        C.Cancel = false;
        C.Running = true;
        C.Outcome = "running";
        C.Phase = "impact:index";
        C.Done = 0;
        C.Total = P.Functions.size();
        C.Error.clear();
      }
      auto Finished = llvm::scope_exit([&] {
        std::lock_guard Status(C.StatusMutex);
        C.Running = false;
        if (C.Outcome == "running")
          C.Outcome = "failed";
      });
      ComparisonControl Control;
      Control.Cancel = &C.Cancel;
      Control.Progress = [&](llvm::StringRef Phase, uint64_t Done,
                             uint64_t Total) {
        std::lock_guard Status(C.StatusMutex);
        C.Phase = Phase.str();
        C.Done = Done;
        C.Total = Total;
      };
      if (Timeout)
        Control.Deadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(Timeout);
      auto Evidence =
          analyzeCallImpact(P, Seeds,
                            Direction == "callers" ? ImpactDirection::Callers
                                                   : ImpactDirection::Callees,
                            Options, Control);
      std::vector<uint32_t> Entries;
      for (uint32_t I = 0; I < Evidence.Functions.size(); ++I) {
        const auto &F = P.Functions[Evidence.Functions[I].Function];
        if (F.has(TraitEntryPoint) || F.has(TraitExported))
          Entries.push_back(I);
      }
      std::optional<CallEffectReview> Effects;
      if (WithEffects)
        Effects = reviewCallEffects(P, Evidence, EffectOptions, Control);
      C.CachedImpact = Comparison::ImpactCache{
          Key, std::move(Evidence), std::move(Entries), std::move(Effects)};
      std::lock_guard Status(C.StatusMutex);
      const auto &Cached = *C.CachedImpact;
      C.Outcome = stateName(Cached.Effects && Cached.Effects->State !=
                                                  ComparisonState::Complete
                                ? Cached.Effects->State
                                : Cached.Evidence.State)
                      .str();
    }
    const auto &R = C.CachedImpact->Evidence;
    const auto *Effects =
        C.CachedImpact->Effects ? &*C.CachedImpact->Effects : nullptr;
    const auto Gaps = [](uint32_t Mask) {
      llvm::json::Array Out;
      for (auto Gap : impactGapNames(Mask))
        Out.push_back(Gap);
      return Out;
    };
    const auto Site = [&](uint32_t I) {
      const auto &S = R.Sites[I];
      return llvm::json::Object{
          {"site", I},
          {"caller", function(C, P, S.Caller)},
          {"callee", S.Callee == NoIndex
                         ? llvm::json::Value(nullptr)
                         : llvm::json::Value(function(C, P, S.Callee))},
          {"instruction",
           S.Instruction == NoIndex
               ? llvm::json::Value(nullptr)
               : llvm::json::Value(instruction(P, S.Instruction))},
          {"gaps", Gaps(S.Gaps)}};
    };
    std::vector<uint32_t> Path;
    if (Kind == "path") {
      auto RI = count(*O, "witness", 0, UINT32_MAX);
      require(RI < R.Functions.size(),
              "impact witness is outside retained functions");
      while (R.Functions[RI].WitnessSite != NoIndex) {
        Path.push_back(R.Functions[RI].WitnessSite);
        RI = R.Functions[RI].Predecessor;
      }
      std::reverse(Path.begin(), Path.end());
    }
    const auto &Indices = Kind == "entrypoints"  ? C.CachedImpact->Entries
                          : Kind == "boundaries" ? R.BoundarySites
                          : Kind == "frontier"   ? R.FrontierSites
                          : Kind == "path"       ? Path
                                                 : R.TraversedSites;
    const uint64_t Total = WithEffects           ? Effects->Records.size()
                           : Kind == "functions" ? R.Functions.size()
                                                 : Indices.size();
    llvm::json::Array Rows;
    for (uint64_t I = std::min(Offset, Total);
         I < std::min(Offset + Limit, Total); ++I) {
      if (WithEffects) {
        const auto &E = Effects->Records[I];
        auto Row = Site(E.Site);
        llvm::json::Array Capabilities, SSAGaps;
        for (auto Name : callEffectCapabilityNames(E.Capabilities))
          Capabilities.push_back(Name);
        for (auto Name : flowGapNames(E.SSAGaps))
          SSAGaps.push_back(Name);
        const auto Role = [](int Index) -> llvm::json::Value {
          return Index < 0 ? llvm::json::Value(nullptr)
                           : llvm::json::Value(Index);
        };
        Row["effect"] = llvm::json::Object{
            {"status", callEffectStatusName(E.Status)},
            {"family", callEffectFamilyName(E.Family)},
            {"capabilities", std::move(Capabilities)},
            {"ssa_node", E.Node == NoIndex ? llvm::json::Value(nullptr)
                                           : llvm::json::Value(E.Node)},
            {"argument_count",
             E.Node == NoIndex || E.Status == CallEffectStatus::MissingArguments
                 ? llvm::json::Value(nullptr)
                 : llvm::json::Value(E.ArgumentCount)},
            {"argument_roles",
             llvm::json::Object{{"destination", Role(E.Destination)},
                                {"source", Role(E.Source)},
                                {"length", Role(E.Length)},
                                {"capacity", Role(E.Capacity)},
                                {"format", Role(E.Format)},
                                {"handle", Role(E.Handle)},
                                {"input_buffer", Role(E.InputBuffer)}}},
            {"release_may_fail",
             E.Status == CallEffectStatus::Qualified && E.Handle >= 0
                 ? llvm::json::Value(E.ReleaseMayFail)
                 : llvm::json::Value(nullptr)},
            {"unbounded_write",
             E.Status == CallEffectStatus::Qualified && E.Destination >= 0
                 ? llvm::json::Value(E.UnboundedWrite)
                 : llvm::json::Value(nullptr)},
            {"ssa_gaps", std::move(SSAGaps)}};
        Rows.push_back(std::move(Row));
        continue;
      }
      if (Kind != "functions" && Kind != "entrypoints") {
        Rows.push_back(Site(Indices[I]));
        continue;
      }
      const auto RI = Kind == "functions" ? uint32_t(I) : Indices[I];
      const auto &F = R.Functions[RI];
      const auto &Native = P.Functions[F.Function];
      auto Row = function(C, P, F.Function);
      Row["row"] = RI;
      Row["depth"] = F.Depth;
      Row["entry_point"] = Native.has(TraitEntryPoint);
      Row["exported"] = Native.has(TraitExported);
      Row["scan_complete"] = F.ScanComplete;
      Row["gaps"] = Gaps(F.Gaps);
      Row["predecessor"] = F.Predecessor == NoIndex
                               ? llvm::json::Value(nullptr)
                               : llvm::json::Value(F.Predecessor);
      Row["witness"] = F.WitnessSite == NoIndex
                           ? llvm::json::Value(nullptr)
                           : llvm::json::Value(Site(F.WitnessSite));
      Rows.push_back(std::move(Row));
    }
    llvm::json::Object Out{
        {"schema", "neverd.bindiff.impact"},
        {"schema_version", 1},
        {"profile", CallImpactProfile},
        {"revision", P.Identity.Revision},
        {"input_sha256", P.Identity.SHA256},
        {"query", std::move(Recipe)},
        {"side", Side},
        {"direction", Direction},
        {"kind", Kind},
        {"state",
         stateName(Effects && Effects->State != ComparisonState::Complete
                       ? Effects->State
                       : R.State)},
        {"index_state", stateName(R.IndexState)},
        {"traversal_state", stateName(R.TraversalState)},
        {"index_complete", R.IndexComplete},
        {"discovery_complete", P.Identity.DiscoveryComplete},
        {"traversal_complete", R.TraversalComplete},
        {"indexed_functions", R.IndexedFunctions},
        {"indexed_instructions", R.IndexedInstructions},
        {"program_functions", P.Functions.size()},
        {"work", R.Work},
        {"function_count", R.Functions.size()},
        {"entry_count", C.CachedImpact->Entries.size()},
        {"site_count", R.TraversedSites.size()},
        {"boundary_count", R.BoundarySites.size()},
        {"frontier_count", R.FrontierSites.size()},
        {"gaps", Gaps(R.Gaps)},
        {"assessment", "possible_call_reachability"},
        {"path_feasibility", "not_checked"},
        {"parameter_return_binding", "not_modeled"},
        {"memory_aliases", "not_modeled"},
        {"external_callers", "not_indexed"},
        {"semantic_validation", "not_performed"},
        {"offset", Offset},
        {"total", Total},
        {"rows", std::move(Rows)}};
    if (Effects) {
      Out["effect_review"] = llvm::json::Object{
          {"profile", CallEffectReviewProfile},
          {"state", stateName(Effects->State)},
          {"inspection_complete", Effects->Complete},
          {"work", Effects->Work},
          {"visited_indexed_sites", Effects->VisitedSites},
          {"qualified_contracts", Effects->Qualified},
          {"scope", "indexed_transfers_in_reached_callers"},
          {"contract_authority", "SafetyCallEffects"},
          {"identity_requirement", "exact_native_import"},
          {"assumption", "import_resolves_to_catalog_contract"},
          {"data_propagation", "not_performed"},
          {"security_assessment", "not_performed"},
          {"must_release", "not_proved"}};
      Out["dataflow_recipe"] = P.DataFlow
                                   ? llvm::json::Value(P.DataFlow->Recipe)
                                   : llvm::json::Value(nullptr);
    }
    return dupStr(jsonToString(std::move(Out)));
  });
}

const char *neverd_comparison_flow_regions_json(neverd_comparison_t Handle,
                                                const char *Query) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    auto V = json(Query);
    const auto *O = V.getAsObject();
    require(O, "SSA region query must be an object");
    keys(*O, {"kind", "region_id", "all_anchors", "anchor", "coordinates",
              "offset", "limit"});
    const auto &R = C.Result.FlowRegions;
    require(R.State.has_value(),
            "SSA regions were not analyzed; compare with flow_regions:true");
    const auto Id = string(*O, "region_id");
    const FlowRegionHypothesis *H = nullptr;
    if (!Id.empty()) {
      const auto It = llvm::find_if(
          R.Hypotheses, [&](const auto &Row) { return Row.Id == Id; });
      require(It != R.Hypotheses.end(), "unknown SSA region hypothesis");
      H = &*It;
    }
    const bool All = flag(*O, "all_anchors", false);
    const bool HasAnchor = O->get("anchor");
    require(!All || (!H && !HasAnchor),
            "all_anchors is exclusive with region_id and anchor");
    const auto Kind = string(*O, "kind", "all");
    const auto Filter = regionKindNamed(Kind);
    require((Kind == "all" || Filter) &&
                (!(H || All || HasAnchor) || Kind == "all"),
            "invalid relationship filter on SSA region query");
    const auto Coordinates = string(*O, "coordinates", "nodes");
    require((Coordinates == "nodes" || Coordinates == "phi_blocks" ||
             Coordinates == "frontier") &&
                (HasAnchor || !O->get("coordinates")),
            "coordinates requires an anchor and nodes, phi_blocks or frontier");
    const auto Limit = count(*O, "limit", diffLimits::kDefaultPageSize,
                             diffLimits::kMaxPageSize);
    require(Limit, "page limit must be positive");
    const FlowRegionAnchor *A = nullptr;
    uint32_t AnchorIndex = NoIndex;
    std::vector<uint32_t> Selected;
    if (HasAnchor) {
      AnchorIndex = count(*O, "anchor", 0, R.Anchors.size());
      require(AnchorIndex < R.Anchors.size() &&
                  (!H || std::binary_search(H->Anchors.begin(),
                                            H->Anchors.end(), AnchorIndex)),
              "anchor does not belong to this SSA hypothesis");
      A = &R.Anchors[AnchorIndex];
      require(Coordinates != "frontier" || A->Depth,
              "frontier coordinates require a shared fragment");
    } else if (!H && !All) {
      for (uint32_t I = 0; I < R.Hypotheses.size(); ++I)
        if (!Filter ||
            llvm::is_contained(R.Hypotheses[I].Explanations, *Filter))
          Selected.push_back(I);
    }
    const uint64_t Total =
        A     ? (Coordinates == "nodes"      ? A->Nodes.size()
                 : Coordinates == "frontier" ? A->Frontier.size()
                                             : A->PhiBlocks.size())
        : H   ? H->Anchors.size()
        : All ? R.Anchors.size()
              : Selected.size();
    const auto Offset = count(*O, "offset", 0, Total);
    const auto End = std::min(Offset + Limit, Total);
    llvm::json::Object Out{
        {"schema", "neverd.bindiff.flow-regions"},
        {"version", 1},
        {"total", Total},
        {"offset", Offset},
        {"next_offset", End < Total ? llvm::json::Value(End) : nullptr},
        {"analysis", flowRegionSummaryJSON(R)},
        {"primary_revision", C.Primary.Identity.Revision},
        {"secondary_revision", C.Secondary.Identity.Revision},
        {"hypothesis",
         H ? llvm::json::Value(flowRegionHypothesisJSON(*H)) : nullptr}};
    llvm::json::Array Rows;
    if (A) {
      const auto &PG =
          *C.Result.PrimaryDataFlow->find(A->PrimaryFunction)->Graph;
      const auto &SG =
          *C.Result.SecondaryDataFlow->find(A->SecondaryFunction)->Graph;
      const auto Originals = [](const Program &P, uint64_t Address) {
        std::map<uint64_t, uint32_t> Index;
        for (const auto &B : P.blocks(P.Functions[*P.functionAt(Address)]))
          for (uint32_t I = B.FirstInstruction;
               I < B.FirstInstruction + B.InstructionCount; ++I)
            Index.emplace(P.Instructions[I].Address, I);
        return Index;
      };
      const auto PO = Originals(C.Primary, A->PrimaryFunction),
                 SO = Originals(C.Secondary, A->SecondaryFunction);
      const auto Node = [&](const Program &P, const DataFlowGraph &G,
                            const auto &Original, uint32_t N) {
        auto Item = dataFlowNodeJSON(G, N);
        const auto It = Original.find(G.Nodes[N].Address);
        Item["original"] =
            G.Nodes[N].OriginSequence >= 0 && It != Original.end()
                ? llvm::json::Value(instruction(P, It->second))
                : nullptr;
        if (isLLVMDataFlowProfile(G.Profile)) {
          llvm::json::Array Originals;
          std::optional<uint64_t> Previous;
          for (const auto &Origin : G.Nodes[N].Origins) {
            if (Previous == Origin.Address)
              continue;
            Previous = Origin.Address;
            const auto At = Original.find(Origin.Address);
            require(At != Original.end(),
                    "optimized SSA origin is outside the native function");
            Originals.push_back(instruction(P, At->second));
          }
          Item["originals"] = std::move(Originals);
        }
        return Item;
      };
      const auto &Pairs = Coordinates == "nodes"      ? A->Nodes
                          : Coordinates == "frontier" ? A->Frontier
                                                      : A->PhiBlocks;
      for (uint64_t I = Offset; I < End; ++I) {
        const auto [P, S] = Pairs[I];
        Rows.push_back(
            Coordinates != "phi_blocks"
                ? llvm::json::Object{{"primary", Node(C.Primary, PG, PO, P)},
                                     {"secondary",
                                      Node(C.Secondary, SG, SO, S)}}
                : llvm::json::Object{
                      {"primary_block", P},
                      {"secondary_block", S},
                      {"primary_begin", vaHex(PG.Blocks[P].Begin)},
                      {"secondary_begin", vaHex(SG.Blocks[S].Begin)}});
      }
      Out["anchor"] = AnchorIndex;
      Out["location"] = flowRegionAnchorJSON(*A);
      Out["view"] = Coordinates;
      if (Coordinates == "frontier")
        Out["boundary_only"] = true;
      Out["rows"] = std::move(Rows);
    } else {
      for (uint64_t I = Offset; I < End; ++I) {
        if (H || All) {
          const auto N = H ? H->Anchors[I] : uint32_t(I);
          auto Item = flowRegionAnchorJSON(R.Anchors[N]);
          Item["anchor"] = N;
          Rows.push_back(std::move(Item));
        } else {
          Rows.push_back(flowRegionHypothesisJSON(R.Hypotheses[Selected[I]]));
        }
      }
      Out["view"] = H || All ? "anchors" : "hypotheses";
      Out["items"] = std::move(Rows);
    }
    return dupStr(jsonToString(std::move(Out)));
  });
}

const char *neverd_comparison_regions_json(neverd_comparison_t Handle,
                                           const char *Query) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    auto V = json(Query);
    const auto *O = V.getAsObject();
    require(O, "region query must be an object");
    keys(*O, {"kind", "region_id", "all_anchors", "offset", "limit"});
    const auto &R = C.Result.Regions;
    require(R.State.has_value(),
            "region analysis was not requested; compare with regions:true");
    const auto Limit = count(*O, "limit", diffLimits::kDefaultPageSize,
                             diffLimits::kMaxPageSize);
    require(Limit > 0, "page limit must be positive");
    const auto Id = string(*O, "region_id");
    const RegionHypothesis *H = Id.empty() ? nullptr : &region(C, Id);
    const bool AllAnchors = flag(*O, "all_anchors", false);
    require(!H || !AllAnchors,
            "region_id and all_anchors are mutually exclusive");
    const bool AnchorPage = H || AllAnchors;
    const auto Kind = string(*O, "kind", "all");
    const auto Filter = regionKindNamed(Kind);
    require((Kind == "all" || Filter.has_value()) &&
                (!AnchorPage || Kind == "all"),
            "invalid region kind or a kind filter on an anchor page");
    std::vector<uint32_t> Selected;
    if (!AnchorPage)
      for (uint32_t I = 0; I < R.Hypotheses.size(); ++I)
        if (!Filter ||
            llvm::is_contained(R.Hypotheses[I].Explanations, *Filter))
          Selected.push_back(I);
    const uint64_t Total = H            ? H->Anchors.size()
                           : AllAnchors ? R.Anchors.size()
                                        : Selected.size();
    const auto Offset = count(*O, "offset", 0, Total);
    const auto End = std::min(Offset + Limit, Total);
    llvm::json::Object Out{
        {"schema", AnchorPage ? "neverd.bindiff.region.anchors"
                              : "neverd.bindiff.regions"},
        {"version", 1},
        {"total", Total},
        {"offset", Offset},
        {"next_offset", End < Total ? llvm::json::Value(End) : nullptr},
        {"analysis", regionAnalysisJSON(R, true)}};
    llvm::json::Array Items;
    if (AnchorPage) {
      Out["hypothesis"] = H ? llvm::json::Value(regionHypothesisJSON(
                                  *H, C.Primary, C.Secondary))
                            : nullptr;
      for (uint64_t I = Offset; I < End; ++I) {
        const auto Index = H ? H->Anchors[I] : uint32_t(I);
        auto Item = regionAnchorJSON(R.Anchors[Index], C.Primary, C.Secondary);
        Item["anchor"] = Index;
        Items.push_back(std::move(Item));
      }
    } else {
      for (uint64_t I = Offset; I < End; ++I)
        Items.push_back(regionHypothesisJSON(R.Hypotheses[Selected[I]],
                                             C.Primary, C.Secondary));
    }
    Out["items"] = std::move(Items);
    return dupStr(jsonToString(std::move(Out)));
  });
}

const char *neverd_comparison_region_page_json(neverd_comparison_t Handle,
                                               const char *Query) {
  return guarded(Handle, [&](Comparison &C) -> const char * {
    auto Lock = ready(C);
    auto V = json(Query);
    const auto *O = V.getAsObject();
    require(O, "region instruction query must be an object");
    keys(*O, {"region_id", "anchor", "offset", "limit"});
    require(C.Result.Regions.State.has_value(),
            "region analysis was not requested");
    const auto Id = string(*O, "region_id");
    const auto *H = Id.empty() ? nullptr : &region(C, Id);
    require(O->get("anchor"), "region anchor is required");
    const auto Index = count(*O, "anchor", 0, C.Result.Regions.Anchors.size());
    require(Index < C.Result.Regions.Anchors.size() &&
                (!H || std::binary_search(H->Anchors.begin(), H->Anchors.end(),
                                          Index)),
            "anchor does not belong to this hypothesis");
    const auto &A = C.Result.Regions.Anchors[Index];
    const auto Total = A.Primary.InstructionCount;
    const auto Offset = count(*O, "offset", 0, Total);
    const auto Limit = count(*O, "limit", diffLimits::kDefaultPageSize,
                             diffLimits::kMaxPageSize);
    require(Limit > 0, "page limit must be positive");
    const auto End = std::min(Offset + Limit, uint64_t(Total));
    const auto Begin = [](const Program &P, const RegionSpan &S) {
      return P.blocks(P.Functions[S.Function])[S.Block].FirstInstruction +
             S.FirstInstruction;
    };
    const auto PI = Begin(C.Primary, A.Primary),
               SI = Begin(C.Secondary, A.Secondary);
    llvm::json::Object Out{
        {"schema", "neverd.bindiff.region.instructions"},
        {"version", 1},
        {"region_id", H ? llvm::json::Value(H->Id) : nullptr},
        {"anchor", Index},
        {"total", Total},
        {"offset", Offset},
        {"next_offset", End < Total ? llvm::json::Value(End) : nullptr},
        {"primary", function(C, C.Primary, A.Primary.Function)},
        {"secondary", function(C, C.Secondary, A.Secondary.Function)},
        {"hypothesis",
         H ? llvm::json::Value(regionHypothesisJSON(*H, C.Primary, C.Secondary))
           : nullptr},
        {"location", regionAnchorJSON(A, C.Primary, C.Secondary)}};
    llvm::json::Array Rows;
    for (uint64_t I = Offset; I < End; ++I) {
      const bool Same = C.Primary.bytes(C.Primary.Instructions[PI + I]) ==
                        C.Secondary.bytes(C.Secondary.Instructions[SI + I]);
      Rows.push_back(
          llvm::json::Object{{"primary", instruction(C.Primary, PI + I)},
                             {"secondary", instruction(C.Secondary, SI + I)},
                             {"kind", Same ? "identical" : "moved"},
                             {"primary_block", A.Primary.Block},
                             {"secondary_block", A.Secondary.Block}});
    }
    Out["rows"] = std::move(Rows);
    return dupStr(jsonToString(std::move(Out)));
  });
}

namespace {
enum class EditAction { Apply, Undo, Redo };

int editComparison(neverd_comparison_t Handle, EditAction Action,
                   const char *Text, neverd_comparison_progress_t Progress,
                   void *Context) {
  return guarded(Handle, [&](Comparison &C) {
    auto Lock = ready(C);
    {
      std::lock_guard Status(C.StatusMutex);
      C.Cancel = false;
      C.Running = true;
      C.Outcome = C.EditOutcome = "running";
      C.Phase = "edit:prepare";
      C.Done = C.Total = 0;
      C.Error.clear();
      C.EditPreviousRetained = true;
      C.EditSnapshotState = C.Result.State;
    }
    auto Finished = llvm::scope_exit([&] {
      std::lock_guard Status(C.StatusMutex);
      C.Running = false;
      if (C.EditOutcome == "running")
        C.Outcome = C.EditOutcome = "failed";
    });
    ComparisonControl Control;
    Control.Cancel = &C.Cancel;
    Control.Progress = [&](llvm::StringRef Phase, uint64_t Done,
                           uint64_t Total) {
      const auto Name = "edit:" + Phase.str();
      {
        std::lock_guard Status(C.StatusMutex);
        C.Phase = Name;
        C.Done = Done;
        C.Total = Total;
      }
      if (Progress)
        Progress(Context, Name.c_str(), Done, Total);
    };
    try {
      Override Decision;
      if (Action == EditAction::Apply) {
        auto V = json(Text);
        require(V.getAsObject(), "override must be an object");
        Decision = overrideValue(*V.getAsObject());
      } else if (Action == EditAction::Undo) {
        require(!C.Undo.empty(), "no comparison edit to undo");
        C.Redo.reserve(C.Redo.size() + 1);
      } else {
        require(!C.Redo.empty(), "no comparison edit to redo");
        Decision = C.Redo.back();
      }
      if (Action != EditAction::Undo)
        C.Undo.reserve(C.Undo.size() + 1);
      if (Action == EditAction::Undo) {
        revertOverride(C.Result, C.Primary, C.Secondary, C.Undo.back(),
                       Control);
        C.Redo.push_back(C.Undo.back().Applied);
        C.Undo.pop_back();
      } else {
        OverrideEffect Effect;
        if (auto Error = applyOverride(C.Result, C.Primary, C.Secondary,
                                       Decision, &Effect, Control))
          throw std::runtime_error(*Error);
        C.Undo.push_back(std::move(Effect));
        if (Action == EditAction::Redo)
          C.Redo.pop_back();
        else
          C.Redo.clear();
      }
      invalidateMappingViews(C);
      std::lock_guard Status(C.StatusMutex);
      C.Outcome = stateName(C.Result.State).str();
      C.EditOutcome = "complete";
      C.EditPreviousRetained = false;
      C.Done = C.Total = 1;
      return 1;
    } catch (const ComparisonInterrupted &Stopped) {
      std::lock_guard Status(C.StatusMutex);
      C.Outcome = C.EditOutcome = stateName(Stopped.State).str();
      C.Error = "comparison edit " + C.EditOutcome;
      return 0;
    }
  });
}
} // namespace

int neverd_comparison_override_json(neverd_comparison_t Handle,
                                    const char *Text) {
  return neverd_comparison_override_json_with_progress(Handle, Text, nullptr,
                                                       nullptr);
}
int neverd_comparison_override_json_with_progress(
    neverd_comparison_t Handle, const char *Text,
    neverd_comparison_progress_t Progress, void *Context) {
  return editComparison(Handle, EditAction::Apply, Text, Progress, Context);
}
int neverd_comparison_undo(neverd_comparison_t Handle) {
  return neverd_comparison_undo_with_progress(Handle, nullptr, nullptr);
}
int neverd_comparison_undo_with_progress(neverd_comparison_t Handle,
                                         neverd_comparison_progress_t Progress,
                                         void *Context) {
  return editComparison(Handle, EditAction::Undo, nullptr, Progress, Context);
}
int neverd_comparison_redo(neverd_comparison_t Handle) {
  return neverd_comparison_redo_with_progress(Handle, nullptr, nullptr);
}
int neverd_comparison_redo_with_progress(neverd_comparison_t Handle,
                                         neverd_comparison_progress_t Progress,
                                         void *Context) {
  return editComparison(Handle, EditAction::Redo, nullptr, Progress, Context);
}
int neverd_comparison_validate_inputs(neverd_comparison_t Handle,
                                      neverd_session_t A, neverd_session_t B) {
  return guarded(Handle, [&](Comparison &C) {
    auto Lock = ready(C);
    require(A && B, "two live sessions are required");
    std::string Error;
    auto Primary =
        readComparisonInput(*toSession(A), C.PrimaryOptions, {}, Error);
    require(Primary.has_value() && !Primary->Stopped,
            Error.empty() ? "incomplete primary input" : Error);
    auto Secondary =
        readComparisonInput(*toSession(B), C.SecondaryOptions, {}, Error);
    require(Secondary.has_value() && !Secondary->Stopped,
            Error.empty() ? "incomplete secondary input" : Error);
    require(C.Primary.Identity.Revision == Primary->Program.Identity.Revision &&
                C.Secondary.Identity.Revision ==
                    Secondary->Program.Identity.Revision,
            "stale comparison: input or analysis revision changed");
    for (const auto &[Was, Now] :
         {std::pair{C.Result.PrimaryDataFlow.get(), &Primary->Program},
          {C.Result.SecondaryDataFlow.get(), &Secondary->Program}})
      if (auto Error = bindDataFlowEvidence(Was, Now->DataFlow.get(),
                                            C.Result.Settings.DataFlow, *Now))
        throw std::runtime_error(*Error);
    return 1;
  });
}
