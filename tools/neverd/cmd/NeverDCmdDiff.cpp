//===- NeverDCmdDiff.cpp - Structural binary comparison -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Human and JSON consumers of the shared comparison API. Exit codes: 0 no
/// observed code changes, 1 failure, 2 differences, 3 incomplete coverage/work.
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/diff/Limits.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/WithColor.h"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace neverd::cli {
namespace {
// The signal handler and joined loader/monitor threads share only this
// lock-free atomic. No allocation, I/O or SDK operation runs in the handler.
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> Interrupted{false};
void interrupt(int) { Interrupted.store(true, std::memory_order_relaxed); }
std::string take(const char *Text) {
  const std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}
llvm::json::Value parse(llvm::StringRef Text) {
  auto V = llvm::json::parse(Text);
  if (!V)
    throw std::runtime_error(llvm::toString(V.takeError()));
  return std::move(*V);
}
std::string read(llvm::StringRef Path, uint64_t Limit) {
  uint64_t Bytes = 0;
  if (llvm::sys::fs::file_size(Path, Bytes) || Bytes > Limit)
    throw std::runtime_error("cannot read bounded comparison file: " +
                             Path.str());
  auto File = llvm::MemoryBuffer::getFile(Path);
  if (!File)
    throw std::runtime_error("cannot read comparison file: " + Path.str());
  return (*File)->getBuffer().str();
}
std::string render(llvm::json::Value V) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << V;
  return Text;
}
uint64_t primaryFunction(neverd_comparison_t C, llvm::StringRef Name) {
  std::optional<uint64_t> Found;
  uint64_t Offset = 0;
  for (;;) {
    if (Interrupted)
      throw std::runtime_error("function lookup cancelled");
    const auto Query =
        render(llvm::json::Object{{"side", "primary"},
                                  {"search", Name},
                                  {"offset", Offset},
                                  {"limit", limits::diff::kDefaultPageSize}});
    const auto *Text = neverd_comparison_functions_json(C, Query.c_str());
    if (!Text)
      throw std::runtime_error(take(neverd_comparison_last_error(C)));
    const auto Page = parse(take(Text));
    const auto &Rows = *Page.getAsObject();
    for (const auto &Item : *Rows.getArray("items")) {
      const auto &Row = *Item.getAsObject();
      if (Row.getString("name") != Name)
        continue;
      if (Found)
        throw std::runtime_error(
            "primary function name is ambiguous; use an entry address");
      uint64_t Address = 0;
      if (Row.getString("address").value_or("").getAsInteger(0, Address))
        throw std::runtime_error("invalid comparison function address");
      Found = Address;
    }
    const auto Next = Rows.getInteger("next_offset");
    if (!Next)
      break;
    Offset = *Next;
  }
  if (!Found)
    throw std::runtime_error("primary function not found");
  return *Found;
}
void save(llvm::StringRef Path, llvm::StringRef Text,
          const std::function<void()> &BeforePublish) {
  llvm::SmallString<256> Temporary;
  int FD = -1;
  if (auto E =
          llvm::sys::fs::createUniqueFile(Path + ".tmp-%%%%%%", FD, Temporary))
    throw std::runtime_error("cannot create comparison output: " + E.message());
  const auto Cleanup =
      llvm::scope_exit([&] { (void)llvm::sys::fs::remove(Temporary); });
  llvm::raw_fd_ostream OS(FD, true);
  OS << Text << '\n';
  OS.close();
  if (OS.has_error())
    throw std::runtime_error("cannot write comparison output");
  BeforePublish();
  if (auto E = llvm::sys::fs::rename(Temporary, Path))
    throw std::runtime_error("cannot publish comparison output: " +
                             E.message());
}
struct ProgressState {
  neverd_comparison_t Handle = nullptr;
  std::string Phase;
  std::chrono::steady_clock::time_point Last;
};
void progress(void *Context, const char *Phase, neverd_va_t Done,
              neverd_va_t Total) {
  auto &S = *static_cast<ProgressState *>(Context);
  if (S.Handle && Interrupted.load(std::memory_order_relaxed))
    neverd_comparison_cancel(S.Handle);
  const auto Now = std::chrono::steady_clock::now();
  if (S.Phase != Phase || Done == Total ||
      Now - S.Last >= std::chrono::milliseconds(
                          limits::diff::kProgressIntervalMilliseconds)) {
    llvm::errs() << "diff: " << Phase << " " << Done << "/" << Total << '\n';
    S.Phase = Phase;
    S.Last = Now;
  }
}
struct ComparisonCancelled : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void checkRestoreCancelled() {
  if (Interrupted.load(std::memory_order_relaxed))
    throw ComparisonCancelled("saved comparison restore cancelled");
}
void checkEditCancelled() {
  if (Interrupted.load(std::memory_order_relaxed))
    throw ComparisonCancelled("comparison edit cancelled");
}
std::string readSaved(llvm::StringRef Path, ProgressState &Progress) {
  checkRestoreCancelled();
  uint64_t Size = 0;
  if (llvm::sys::fs::file_size(Path, Size) ||
      Size > limits::diff::kMaxSavedComparisonBytes)
    throw std::runtime_error("cannot read bounded comparison file: " +
                             Path.str());
  std::ifstream Input(
      std::filesystem::path(std::u8string(Path.begin(), Path.end())),
      std::ios::binary);
  if (!Input)
    throw std::runtime_error("cannot read comparison file: " + Path.str());
  std::string Text;
  std::array<char, limits::diff::kSavedComparisonChunkBytes> Buffer;
  progress(&Progress, "restore:read", 0, Size);
  while (Text.size() < Size) {
    checkRestoreCancelled();
    const auto Count = std::min<uint64_t>(Buffer.size(), Size - Text.size());
    if (!Input.read(Buffer.data(), static_cast<std::streamsize>(Count)))
      throw std::runtime_error("cannot read comparison file: " + Path.str());
    checkRestoreCancelled();
    Text.append(Buffer.data(), Count);
    progress(&Progress, "restore:read", Text.size(), Size);
  }
  checkRestoreCancelled();
  if (Input.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("comparison file changed while reading");
  return Text;
}
struct InputProgress {
  const char *Side;
  ProgressState Output;
  std::atomic<bool> Failed{false};
  static void report(void *User, const char *Stage, neverd_va_t Done,
                     neverd_va_t Total, const char *) noexcept {
    auto &State = *static_cast<InputProgress *>(User);
    try {
      const auto Phase = std::string(State.Side) + ":load:" + Stage;
      progress(&State.Output, Phase.c_str(), Done, Total);
    } catch (...) {
      State.Failed.store(true, std::memory_order_relaxed);
    }
  }
  static int cancelled(void *User) noexcept {
    const auto &State = *static_cast<InputProgress *>(User);
    return Interrupted.load(std::memory_order_relaxed) ||
           State.Failed.load(std::memory_order_relaxed);
  }
};
} // namespace

int runStructuralDiff() {
  auto A = neverd_session_create(), B = neverd_session_create();
  const auto C = neverd_comparison_create();
  const auto Cleanup = llvm::scope_exit([&] {
    neverd_comparison_destroy(C);
    neverd_session_destroy(A);
    neverd_session_destroy(B);
  });
  if (!A || !B || !C) {
    llvm::WithColor::error() << "cannot allocate comparison sessions\n";
    return 1;
  }
  try {
    Interrupted.store(false, std::memory_order_relaxed);
    const auto Previous = std::signal(SIGINT, interrupt);
    if (Previous == SIG_ERR)
      throw std::runtime_error("cannot install comparison interrupt handler");
    const auto RestoreSignal =
        llvm::scope_exit([&] { std::signal(SIGINT, Previous); });
    ProgressState Progress;
    Progress.Handle = C;
    std::string Reopen;
    llvm::json::Object RestoredOptions;
    if (!DiffReopen.empty()) {
      Reopen = readSaved(DiffReopen, Progress);
      checkRestoreCancelled();
      progress(&Progress, "restore:settings", 0, 0);
      auto Document = parse(Reopen);
      checkRestoreCancelled();
      const auto *Root = Document.getAsObject();
      if (!Root || !Root->getObject("settings"))
        throw std::runtime_error("saved comparison has no settings object");
      RestoredOptions = *Root->getObject("settings");
    }
    auto Value = DiffOptions.empty()
                     ? llvm::json::Value(std::move(RestoredOptions))
                     : parse(read(DiffOptions, limits::diff::kMaxOptionsBytes));
    auto *Options = Value.getAsObject();
    if (!Options)
      throw std::runtime_error("comparison options must be an object");
    if (!DiffOptions.empty())
      for (auto &[Key, Setting] : RestoredOptions)
        if (!Options->get(Key))
          (*Options)[Key] = std::move(Setting);
    if (DiffProfile.getNumOccurrences())
      (*Options)["profile"] = DiffProfile.getValue();
    if (DiffMinSimilarity.getNumOccurrences())
      (*Options)["min_similarity"] = DiffMinSimilarity.getValue();
    if (DiffIgnoreNames)
      (*Options)["use_names"] = false;
    if (DiffTimeout.getNumOccurrences())
      (*Options)["timeout_ms"] = uint64_t(DiffTimeout);
    const bool NativeRegionsView =
        DiffList == "regions" || DiffList == "region-anchors" ||
        !DiffRegion.empty() || DiffRegionAnchor.getNumOccurrences();
    const bool FlowRegionsView =
        DiffList == "flow-regions" || DiffList == "flow-region-anchors" ||
        !DiffFlowRegion.empty() || DiffFlowRegionAnchor.getNumOccurrences();
    const bool DataFlowView = DiffList == "dataflow" ||
                              !DiffFlowFunction.empty() ||
                              DiffSliceNode.getNumOccurrences();
    const bool ImpactView = !DiffImpactFunction.empty();
    const bool ChangesView = !DiffChangesFunction.empty();
    const bool ValidationView = !DiffValidationFunction.empty();
    const bool SourceView = !DiffSourceFunction.empty();
    if (!SourceView && (DiffSourceSide.getNumOccurrences() ||
                        DiffSourceRepresentation.getNumOccurrences() ||
                        DiffSourceInstructions.getNumOccurrences() ||
                        DiffSourceDifference.getNumOccurrences() ||
                        DiffSourcePrelude.getNumOccurrences() ||
                        DiffSourceWork.getNumOccurrences() ||
                        DiffSourceBytes.getNumOccurrences()))
      throw std::runtime_error("source options require --source-function");
    if (!DiffSourceDifference && (DiffSourcePrelude.getNumOccurrences() ||
                                  DiffSourceWork.getNumOccurrences() ||
                                  DiffSourceBytes.getNumOccurrences()))
      throw std::runtime_error(
          "source alignment options require --source-difference");
    if (!ValidationView && (DiffValidationSide.getNumOccurrences() ||
                            DiffValidationWork.getNumOccurrences() ||
                            DiffValidationInstructions.getNumOccurrences()))
      throw std::runtime_error(
          "validation options require --validate-function");
    const bool ReviewView = DiffList == "reviews";
    const bool FunctionInventory =
        DiffList == "all" || DiffList == "matched" || DiffList == "changed" ||
        DiffList == "uncertain" || DiffList == "ambiguous" ||
        DiffList == "unmatched";
    if (!FunctionInventory && (DiffListSide.getNumOccurrences() ||
                               DiffListSearch.getNumOccurrences() ||
                               DiffListSort.getNumOccurrences() ||
                               DiffListDescending.getNumOccurrences() ||
                               DiffAmbiguousWith.getNumOccurrences()))
      throw std::runtime_error(
          "function inventory filters require --list "
          "all/matched/changed/uncertain/ambiguous/unmatched");
    if (!ReviewView && (DiffReviewCase.getNumOccurrences() ||
                        DiffReviewFunction.getNumOccurrences() ||
                        DiffReviewSide.getNumOccurrences() ||
                        DiffReviewLatest.getNumOccurrences()))
      throw std::runtime_error("review filters require --list reviews");
    if (DiffReviewSide.getNumOccurrences() && DiffReviewFunction.empty())
      throw std::runtime_error("--review-side requires --review-function");
    if (!DiffReviewRecord.empty() && DiffSave.empty())
      throw std::runtime_error(
          "--review-record requires --save to preserve its history");
    const bool OtherList =
        !DiffList.empty() && DiffList != "regions" &&
        DiffList != "region-anchors" && DiffList != "flow-regions" &&
        DiffList != "flow-region-anchors" && DiffList != "dataflow";
    if (unsigned(NativeRegionsView) + unsigned(FlowRegionsView) +
            unsigned(DataFlowView) + unsigned(OtherList) +
            unsigned(!DiffFunc.empty()) + unsigned(ImpactView) +
            unsigned(ChangesView) + unsigned(ValidationView) +
            unsigned(SourceView) >
        1)
      throw std::runtime_error(
          "select one inventory, region, function, "
          "dataflow, impact, change, source or validation view");
    if (!ChangesView && (DiffChangesSide.getNumOccurrences() ||
                         DiffChangesContext.getNumOccurrences() ||
                         DiffChangesRows.getNumOccurrences() ||
                         DiffChangesCount.getNumOccurrences() ||
                         DiffChangesWork.getNumOccurrences()))
      throw std::runtime_error(
          "change review options require --changes-function");
    if (!ImpactView && (DiffImpactSide.getNumOccurrences() ||
                        DiffImpactDirection.getNumOccurrences() ||
                        DiffImpactKind.getNumOccurrences() ||
                        DiffImpactWitness.getNumOccurrences() ||
                        DiffImpactDepth.getNumOccurrences() ||
                        DiffImpactFunctions.getNumOccurrences() ||
                        DiffImpactSites.getNumOccurrences() ||
                        DiffImpactWork.getNumOccurrences()))
      throw std::runtime_error("impact options require --impact-function");
    if (ImpactView && (DiffImpactKind == "path") !=
                          bool(DiffImpactWitness.getNumOccurrences()))
      throw std::runtime_error("--impact-kind path requires --impact-witness");
    if ((!ImpactView || DiffImpactKind != "effects") &&
        (DiffEffectRecords.getNumOccurrences() ||
         DiffEffectWork.getNumOccurrences()))
      throw std::runtime_error("effect options require --impact-kind effects");
    if (DiffRegions || DiffRegionCalls || NativeRegionsView) {
      if (auto *Regions = Options->getObject("regions"))
        (*Regions)["enabled"] = true;
      else
        (*Options)["regions"] = true;
    }
    if (DiffRegionCalls) {
      if (!Options->getObject("regions"))
        (*Options)["regions"] = llvm::json::Object{{"enabled", true}};
      (*Options->getObject("regions"))["calls"] = true;
    }
    if (DiffFlowRegions || DiffFlowFragments || FlowRegionsView) {
      if (auto *Regions = Options->getObject("flow_regions"))
        (*Regions)["enabled"] = true;
      else
        (*Options)["flow_regions"] = true;
    }
    if (DiffFlowFragments) {
      if (!Options->getObject("flow_regions"))
        (*Options)["flow_regions"] = llvm::json::Object{{"enabled", true}};
      (*Options->getObject("flow_regions"))["fragments"] = true;
    }
    if (DiffDataFlow || DataFlowView || DiffFlowRegions || DiffFlowFragments ||
        FlowRegionsView || DiffDataFlowProfile.getNumOccurrences()) {
      if (auto *Flow = Options->getObject("dataflow"))
        (*Flow)["enabled"] = true;
      else
        (*Options)["dataflow"] = true;
    }
    if (DiffDataFlowProfile.getNumOccurrences()) {
      if (!Options->getObject("dataflow"))
        (*Options)["dataflow"] = llvm::json::Object{{"enabled", true}};
      (*Options->getObject("dataflow"))["profile"] =
          DiffDataFlowProfile.getValue();
    }
    if (DiffSliceNode.getNumOccurrences() && DiffFlowFunction.empty())
      throw std::runtime_error("--slice-node requires --flow-function");
    if (DiffFlowRegionCoordinates.getNumOccurrences() &&
        !DiffFlowRegionAnchor.getNumOccurrences())
      throw std::runtime_error(
          "--flow-region-coordinates requires --flow-region-anchor");
    for (const auto &[S, Path] :
         {std::pair{A, &DiffFileA}, std::pair{B, &DiffFileB}}) {
      llvm::errs() << "diff: loading " << Path->getValue() << '\n';
      InputProgress Load{S == A ? "primary" : "secondary", {}};
      neverd_session_set_load_progress(S, InputProgress::report, &Load);
      const auto ClearProgress = llvm::scope_exit(
          [&] { neverd_session_set_load_progress(S, nullptr, nullptr); });
      const int Loaded = neverd_session_load_cancellable(
          S, Path->c_str(), InputProgress::cancelled, &Load);
      if (Load.Failed.load(std::memory_order_relaxed))
        throw std::runtime_error("could not report input loading progress");
      if (Loaded == -2 || Interrupted.load(std::memory_order_relaxed)) {
        llvm::errs()
            << "diff: input loading stopped; no comparison was published\n";
        return 3;
      }
      if (Loaded != 1)
        throw std::runtime_error("input load failed: " + takeLastError(S));
    }
    std::atomic<bool> StopMonitor = false;
    std::thread Monitor([&] {
      while (!StopMonitor.load(std::memory_order_relaxed)) {
        if (Interrupted) {
          neverd_comparison_cancel(C);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(
            limits::diff::kCancellationPollMilliseconds));
      }
    });
    const auto JoinMonitor = llvm::scope_exit([&] {
      StopMonitor.store(true, std::memory_order_relaxed);
      Monitor.join();
    });
    const auto OptionsText = render(std::move(Value));
    if (!neverd_comparison_run_owned(C, &A, &B, OptionsText.c_str(), progress,
                                     &Progress))
      throw std::runtime_error("comparison failed: " +
                               take(neverd_comparison_last_error(C)));
    if (!DiffReopen.empty()) {
      checkRestoreCancelled();
      if (!neverd_comparison_restore_json_with_progress(C, Reopen.c_str(),
                                                        progress, &Progress)) {
        const auto Status = parse(take(neverd_comparison_status_json(C)));
        if (Status.getAsObject()->getString("restore_state") == "cancelled")
          throw ComparisonCancelled("saved comparison restore cancelled");
        throw std::runtime_error("reopen failed: " +
                                 take(neverd_comparison_last_error(C)));
      }
      checkRestoreCancelled();
    }
    if (!DiffOverrides.empty()) {
      checkEditCancelled();
      auto Overrides =
          parse(read(DiffOverrides, limits::diff::kMaxOptionsBytes));
      const auto *Rows = Overrides.getAsArray();
      if (!Rows)
        throw std::runtime_error("override file must contain an array");
      for (const auto &Row : *Rows) {
        checkEditCancelled();
        std::string Text;
        llvm::raw_string_ostream OS(Text);
        OS << Row;
        if (!neverd_comparison_override_json_with_progress(
                C, Text.c_str(), progress, &Progress)) {
          const auto Status = parse(take(neverd_comparison_status_json(C)));
          if (Status.getAsObject()->getString("edit_state") == "cancelled")
            throw ComparisonCancelled("comparison edit cancelled");
          throw std::runtime_error("override failed: " +
                                   take(neverd_comparison_last_error(C)));
        }
        checkEditCancelled();
      }
    }
    if (!DiffReviewRecord.empty()) {
      const auto Record =
          read(DiffReviewRecord, limits::diff::kMaxOptionsBytes);
      if (!neverd_comparison_review_record_json(C, Record.c_str()))
        throw std::runtime_error("review record failed: " +
                                 take(neverd_comparison_last_error(C)));
    }
    const auto SummaryText = take(neverd_comparison_summary_json(C));
    if (SummaryText.empty())
      throw std::runtime_error(take(neverd_comparison_last_error(C)));
    auto Document = parse(SummaryText);
    const auto &Root = *Document.getAsObject();
    const auto &Counts = *Root.getObject("counts");
    bool Partial = Root.getString("state") != "complete" ||
                   Counts.getInteger("unmeasured").value_or(0) != 0 ||
                   Counts.getInteger("reference_gap_pairs").value_or(0) != 0 ||
                   Counts.getInteger("incomplete_pairs").value_or(0) != 0;
    const uint64_t Changed = Counts.getInteger("changed").value_or(0);
    const uint64_t PairCount = Counts.getInteger("matches").value_or(0);
    std::string Saved;
    if (!DiffSave.empty() ||
        (DiffJson && DiffList.empty() && DiffFunc.empty() && !SourceView)) {
      Saved = take(neverd_comparison_export_json(C));
      if (Saved.empty())
        throw std::runtime_error(take(neverd_comparison_last_error(C)));
    }
    uint64_t Unresolved = 0;
    const auto *Coverage = Root.getObject("coverage");
    for (const char *Side : {"primary", "secondary"}) {
      const auto *S = Coverage->getObject(Side);
      Partial |= !S->getBoolean("discovery_complete").value_or(false) ||
                 S->getInteger("incomplete").value_or(0) != 0;
      Unresolved += S->getInteger("unmatched").value_or(0) +
                    S->getInteger("ambiguous").value_or(0);
      if (const auto *Flows = Root.getObject("dataflow"))
        if (const auto *Flow = Flows->getObject(Side);
            Flow && Flow->getBoolean("requested").value_or(false))
          Partial |= Flow->getInteger("unsupported").value_or(0) ||
                     Flow->getInteger("failed").value_or(0) ||
                     Flow->getInteger("resource_limited").value_or(0) ||
                     Flow->getInteger("unprocessed_functions").value_or(0);
    }
    if (const auto *Regions = Root.getObject("flow_regions");
        Regions && Regions->getString("state"))
      Partial |=
          !Regions->getBoolean("primary_scope_complete").value_or(false) ||
          !Regions->getBoolean("secondary_scope_complete").value_or(false);
    std::string Output = Saved.empty() ? SummaryText : Saved;
    if (ReviewView) {
      llvm::json::Object Query{{"case", DiffReviewCase.getValue()},
                               {"latest", bool(DiffReviewLatest)},
                               {"offset", unsigned(DiffOffset)},
                               {"limit", unsigned(DiffLimit)}};
      if (!DiffReviewFunction.empty()) {
        Query["side"] = DiffReviewSide.getValue();
        Query["address"] = DiffReviewFunction.getValue();
      }
      Output = take(
          neverd_comparison_reviews_json(C, render(std::move(Query)).c_str()));
    } else if (SourceView) {
      llvm::json::Object SourceQuery{
          {"side", DiffSourceSide.getValue()},
          {"address", DiffSourceFunction.getValue()},
          {"representation", DiffSourceRepresentation.getValue()},
          {"offset", unsigned(DiffOffset)},
          {"limit", unsigned(DiffLimit)},
          {"max_native_instructions", unsigned(DiffSourceInstructions)},
          {"timeout_ms", uint64_t(DiffTimeout)}};
      if (DiffSourceDifference) {
        SourceQuery["max_work"] = uint64_t(DiffSourceWork);
        SourceQuery["max_source_bytes"] = uint64_t(DiffSourceBytes);
        SourceQuery["include_prelude"] = bool(DiffSourcePrelude);
      }
      const auto Query = render(std::move(SourceQuery));
      Output =
          take(DiffSourceDifference
                   ? neverd_comparison_source_difference_json(C, Query.c_str())
                   : neverd_comparison_source_page_json(C, Query.c_str()));
      if (!Output.empty())
        Partial |= parse(Output).getAsObject()->getString("availability") !=
                   "available";
    } else if (ValidationView) {
      llvm::json::Object Query{
          {"side", DiffValidationSide.getValue()},
          {"address", DiffValidationFunction.getValue()},
          {"max_native_instructions", unsigned(DiffValidationInstructions)},
          {"timeout_ms", uint64_t(DiffTimeout)}};
      if (DiffValidationWork.getNumOccurrences())
        Query["max_work"] = uint64_t(DiffValidationWork);
      Output = take(neverd_comparison_owned_scalar_validation_json(
          C, render(std::move(Query)).c_str()));
      if (!Output.empty())
        Partial |= parse(Output).getAsObject()->getString("status") != "proved";
    } else if (ChangesView) {
      llvm::json::Object Query{{"side", DiffChangesSide.getValue()},
                               {"address", DiffChangesFunction.getValue()},
                               {"context", DiffChangesContext.getValue()},
                               {"max_rows", unsigned(DiffChangesRows)},
                               {"max_observations", unsigned(DiffChangesCount)},
                               {"max_work", uint64_t(DiffChangesWork)},
                               {"timeout_ms", uint64_t(DiffTimeout)},
                               {"offset", unsigned(DiffOffset)},
                               {"limit", unsigned(DiffLimit)}};
      Output = take(neverd_comparison_observations_json(
          C, render(std::move(Query)).c_str()));
      if (!Output.empty()) {
        const auto Result = parse(Output);
        Partial |= Result.getAsObject()->getString("state") != "complete" ||
                   !Result.getAsObject()
                        ->getBoolean("native_analysis_complete")
                        .value_or(false);
      }
    } else if (ImpactView) {
      llvm::json::Array Seeds;
      llvm::SmallVector<llvm::StringRef> Parts;
      llvm::StringRef(DiffImpactFunction).split(Parts, ',');
      for (auto Part : Parts)
        Seeds.push_back(Part.trim());
      llvm::json::Object Query{{"side", DiffImpactSide.getValue()},
                               {"direction", DiffImpactDirection.getValue()},
                               {"kind", DiffImpactKind.getValue()},
                               {"seeds", std::move(Seeds)},
                               {"max_depth", unsigned(DiffImpactDepth)},
                               {"max_functions", unsigned(DiffImpactFunctions)},
                               {"max_sites", unsigned(DiffImpactSites)},
                               {"max_work", uint64_t(DiffImpactWork)},
                               {"timeout_ms", uint64_t(DiffTimeout)},
                               {"offset", unsigned(DiffOffset)},
                               {"limit", unsigned(DiffLimit)}};
      if (DiffImpactWitness.getNumOccurrences())
        Query["witness"] = unsigned(DiffImpactWitness);
      if (DiffImpactKind == "effects") {
        Query["max_effect_records"] = unsigned(DiffEffectRecords);
        Query["max_effect_work"] = uint64_t(DiffEffectWork);
      }
      Output = take(
          neverd_comparison_impact_json(C, render(std::move(Query)).c_str()));
      if (!Output.empty())
        Partial |=
            parse(Output).getAsObject()->getString("state") != "complete";
    } else if (FlowRegionsView) {
      llvm::json::Object Query{{"offset", unsigned(DiffOffset)},
                               {"limit", unsigned(DiffLimit)}};
      if (!DiffFlowRegion.empty())
        Query["region_id"] = DiffFlowRegion.getValue();
      if (DiffFlowRegionAnchor.getNumOccurrences()) {
        Query["anchor"] = unsigned(DiffFlowRegionAnchor);
        Query["coordinates"] = DiffFlowRegionCoordinates.getValue();
      } else if (DiffList == "flow-region-anchors") {
        Query["all_anchors"] = true;
      } else if (DiffFlowRegion.empty()) {
        Query["kind"] = DiffRegionKind.getValue();
      }
      Output = take(neverd_comparison_flow_regions_json(
          C, render(std::move(Query)).c_str()));
    } else if (DataFlowView) {
      llvm::json::Object Query{{"side", DiffFlowSide.getValue()},
                               {"offset", unsigned(DiffOffset)},
                               {"limit", unsigned(DiffLimit)}};
      if (!DiffFlowFunction.empty())
        Query["address"] = DiffFlowFunction.getValue();
      if (DiffSliceNode.getNumOccurrences()) {
        Query["seeds"] = llvm::json::Array{unsigned(DiffSliceNode)};
        Query["direction"] = DiffSliceDirection.getValue();
        Query["control"] = !DiffSliceDataOnly;
        Query["timeout_ms"] = uint64_t(DiffTimeout);
        Output = take(
            neverd_comparison_slice_json(C, render(std::move(Query)).c_str()));
        if (!Output.empty())
          Partial |=
              parse(Output).getAsObject()->getString("state") != "complete";
      } else {
        Query["kind"] =
            DiffFlowFunction.empty() ? "functions" : DiffFlowKind.getValue();
        Output = take(neverd_comparison_dataflow_json(
            C, render(std::move(Query)).c_str()));
      }
    } else if (DiffList == "regions" || DiffList == "region-anchors" ||
               !DiffRegion.empty() || DiffRegionAnchor.getNumOccurrences()) {
      llvm::json::Object Query{{"offset", unsigned(DiffOffset)},
                               {"limit", unsigned(DiffLimit)}};
      if (!DiffRegion.empty())
        Query["region_id"] = DiffRegion.getValue();
      if (DiffRegionAnchor.getNumOccurrences())
        Query["anchor"] = unsigned(DiffRegionAnchor);
      else if (DiffList == "region-anchors")
        Query["all_anchors"] = true;
      else if (DiffRegion.empty())
        Query["kind"] = DiffRegionKind.getValue();
      const auto Q = render(std::move(Query));
      Output = take((DiffRegionAnchor.getNumOccurrences()
                         ? neverd_comparison_region_page_json
                         : neverd_comparison_regions_json)(C, Q.c_str()));
    } else if (!DiffList.empty()) {
      llvm::json::Object Query{{"kind", DiffList.getValue()},
                               {"side", DiffListSide.getValue()},
                               {"search", DiffListSearch.getValue()},
                               {"sort", DiffListSort.getValue()},
                               {"descending", bool(DiffListDescending)},
                               {"offset", unsigned(DiffOffset)},
                               {"limit", unsigned(DiffLimit)}};
      if (DiffAmbiguousWith.getNumOccurrences())
        Query["ambiguous_with"] = DiffAmbiguousWith.getValue();
      const auto Q = render(std::move(Query));
      Output = take(neverd_comparison_functions_json(C, Q.c_str()));
    } else if (!DiffFunc.empty()) {
      uint64_t Address = 0;
      if (llvm::StringRef(DiffFunc).getAsInteger(0, Address))
        Address = primaryFunction(C, DiffFunc.getValue());
      Output = take(neverd_comparison_function_json(C, 0, Address));
      if (!Output.empty())
        Partial |=
            parse(Output).getAsObject()->getString("state") != "complete";
    }
    if (Output.empty())
      throw std::runtime_error(take(neverd_comparison_last_error(C)));
    const bool JSONOutput =
        DiffJson || !DiffList.empty() || !DiffFunc.empty() ||
        !DiffRegion.empty() || DiffRegionAnchor.getNumOccurrences() ||
        !DiffFlowFunction.empty() || FlowRegionsView || ImpactView ||
        ChangesView || ValidationView || SourceView;
    std::string Human;
    llvm::raw_string_ostream HumanOutput(Human);
    if (!JSONOutput) {
      HumanOutput
          << "A: " << DiffFileA.getValue() << "\nB: " << DiffFileB.getValue()
          << "\nState: " << Root.getString("state").value_or("unknown")
          << "\nMatched: " << PairCount << "  Changed: " << Changed
          << "  Uncertain: "
          << Counts.getInteger("uncertain_without_observed_change").value_or(0)
          << "  Unmatched/ambiguous: " << Unresolved
          << "\nSimilarity: " << Root.getNumber("similarity").value_or(0)
          << " (structural heuristic)\n";
      for (const auto &W : *Root.getArray("warnings"))
        HumanOutput << "Warning: " << W.getAsString().value_or("") << '\n';
      if (const auto *Regions = Root.getObject("regions");
          Regions && Regions->getString("state"))
        HumanOutput << "Region hypotheses: "
                    << Regions->getInteger("hypothesis_count").value_or(0)
                    << "  Anchors: "
                    << Regions->getInteger("anchor_count").value_or(0)
                    << "  State: " << *Regions->getString("state")
                    << "\nNative run evidence; behavior and patch presence "
                       "are not validated.\n";
      if (const auto *Regions = Root.getObject("flow_regions");
          Regions && Regions->getString("state"))
        HumanOutput << "SSA region hypotheses: "
                    << Regions->getInteger("hypothesis_count").value_or(0)
                    << "  Anchors: "
                    << Regions->getInteger("anchor_count").value_or(0)
                    << "  State: " << *Regions->getString("state")
                    << "\nFinite dependency correspondences; open effects and "
                       "control require review.\n";
      if (Partial)
        HumanOutput << "Coverage is incomplete; inspect the reported gaps.\n";
    }
    const auto BeforePublish = [&] {
      if (!DiffOverrides.empty()) {
        progress(&Progress, "edit:publish-output", 0, 1);
        checkEditCancelled();
      } else if (!DiffReopen.empty()) {
        progress(&Progress, "restore:publish-output", 0, 1);
        checkRestoreCancelled();
      }
    };
    if (!DiffSave.empty()) {
      if (!DiffOverrides.empty()) {
        progress(&Progress, "edit:write", 0, Saved.size());
        checkEditCancelled();
      } else if (!DiffReopen.empty()) {
        progress(&Progress, "restore:write", 0, Saved.size());
        checkRestoreCancelled();
      }
      save(DiffSave, Saved, BeforePublish);
    } else {
      BeforePublish();
    }
    // From the first rename/stdout publication onward, a late signal does not
    // turn a completed external commit into a claim of unpublished output.
    llvm::outs() << (JSONOutput ? Output : Human);
    if (JSONOutput)
      llvm::outs() << '\n';
    return Partial ? 3 : (Changed || Unresolved) ? 2 : 0;
  } catch (const ComparisonCancelled &E) {
    llvm::errs() << "diff: " << E.what() << "; no output was published\n";
    return 3;
  } catch (const std::exception &E) {
    llvm::WithColor::error() << E.what() << '\n';
    return 1;
  }
}
} // namespace neverd::cli
