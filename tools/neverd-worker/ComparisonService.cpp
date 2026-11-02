//===- ComparisonService.cpp - Independent binary comparison jobs ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owns two read-only inputs and a C comparison handle. The transport supplies
/// a cancellation probe; a joined monitor forwards it without touching
/// sessions.
//===----------------------------------------------------------------------===//

#include "ComparisonService.h"

#include "EngineSymbols.h"
#include "GraphSnapshot.h"

#include "neverd/diff/Limits.h"
#include "neverd/sdk/NeverDCAPIDiff.h"
#include "neverd/sdk/NeverDCAPISession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

namespace neverd::worker {
namespace {
namespace limits = ::neverd::limits::diff;
namespace fs = std::filesystem;

struct API {
  decltype(&neverd_session_load_cancellable) LoadCancellable =
      engineSymbol<decltype(LoadCancellable)>(
          "neverd_session_load_cancellable");
#define ENTRY(Name)                                                            \
  decltype(&neverd_comparison_##Name) Name =                                   \
      engineSymbol<decltype(Name)>("neverd_comparison_" #Name)
  ENTRY(create);
  ENTRY(destroy);
  ENTRY(run_owned);
  ENTRY(cancel);
  ENTRY(last_error);
  ENTRY(status_json);
  ENTRY(summary_json);
  ENTRY(export_json);
  ENTRY(restore_json);
  ENTRY(restore_json_with_progress);
  ENTRY(functions_json);
  ENTRY(function_json);
  ENTRY(function_page_json);
  ENTRY(source_page_json);
  ENTRY(source_difference_json);
  ENTRY(graph_json);
  ENTRY(regions_json);
  ENTRY(region_page_json);
  ENTRY(flow_regions_json);
  ENTRY(dataflow_json);
  ENTRY(slice_json);
  ENTRY(impact_json);
  ENTRY(observations_json);
  ENTRY(owned_scalar_validation_json);
  ENTRY(reviews_json);
  ENTRY(review_record_json);
  ENTRY(override_json);
  ENTRY(override_json_with_progress);
  ENTRY(undo);
  ENTRY(undo_with_progress);
  ENTRY(redo);
  ENTRY(redo_with_progress);
  ENTRY(validate_inputs);
#undef ENTRY
  explicit operator bool() const {
    return create && destroy && run_owned && cancel && last_error &&
           status_json && summary_json && export_json && restore_json &&
           functions_json && function_json && function_page_json &&
           graph_json && override_json && undo && redo && validate_inputs &&
           regions_json && region_page_json && flow_regions_json &&
           dataflow_json && slice_json && impact_json && observations_json &&
           reviews_json && review_record_json && owned_scalar_validation_json;
  }
};
const API &api() {
  static const API Functions;
  return Functions;
}

// Keep cancellation forwarding joined on every return and exception path,
// including standard libraries that do not yet provide std::jthread.
class CancellationMonitor {
  std::atomic<bool> Done = false;
  std::thread Thread;

public:
  CancellationMonitor(neverd_comparison_t Handle,
                      const ComparisonService::Cancellation &Cancelled,
                      bool StopAfterCancel = false) {
    if (!Handle)
      return;
    Thread = std::thread([this, Handle, &Cancelled, StopAfterCancel] {
      while (!Done.load(std::memory_order_relaxed)) {
        if (Cancelled && Cancelled()) {
          api().cancel(Handle);
          if (StopAfterCancel)
            return;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(limits::kCancellationPollMilliseconds));
      }
    });
  }
  ~CancellationMonitor() { stop(); }

  void stop() {
    Done.store(true, std::memory_order_relaxed);
    if (Thread.joinable())
      Thread.join();
  }
};

std::string take(const char *Text) {
  const std::unique_ptr<const char, decltype(&neverd_free_string)> Owned(
      Text, neverd_free_string);
  return Text ? Text : "";
}
fs::path path(const std::string &Text) {
  return fs::u8path(Text.begin(), Text.end());
}
std::string pathField(const Json &Value) {
  const auto Text =
      stringField(Value, "path", {},
                  ::neverd::limits::worker::
                      kEngineExecuteImplIdentifyPathPathStringFieldMax);
  if (Text.empty())
    throw Error("invalid_request",
                "A comparison input or output path is required");
  return Text;
}
void fields(const Json &Value,
            std::initializer_list<std::string_view> Allowed) {
  if (!Value.is_object())
    throw Error("invalid_request", "Comparison payload must be an object");
  for (const auto &[Key, V] : Value.items()) {
    (void)V;
    if (std::find(Allowed.begin(), Allowed.end(), Key) == Allowed.end())
      throw Error("invalid_request", "Unknown comparison field: " + Key);
  }
}
using Session = std::unique_ptr<void, decltype(&neverd_session_destroy)>;
Session load(const Json &Input, const char *Side,
             const ComparisonService::Progress &Notify,
             const ComparisonService::Cancellation &Cancelled) {
  fields(Input, {"path", "load_options"});
  if (Cancelled && Cancelled())
    throw Error("cancelled", "Comparison stopped before loading an input");
  Session S(neverd_session_create(), neverd_session_destroy);
  if (!S)
    throw Error("comparison_load_failed", "Cannot create comparison session");
  const auto Options = Input.value("load_options", Json::object());
  if (!Options.is_object())
    throw Error("invalid_request", "load_options must be an object");
  if (!Options.empty() &&
      neverd_session_set_load_options(S.get(), Options.dump().c_str()) < 0)
    throw Error("comparison_load_failed", take(neverd_last_error(S.get())));
  const auto Name = pathField(Input);
  const std::string Phase = std::string(Side) + ":load";
  if (Notify)
    Notify(Phase.c_str(), 0, 1, Name.c_str());
  struct LoadContext {
    const ComparisonService::Progress &Notify;
    const ComparisonService::Cancellation &Cancelled;
    const std::string &Phase;
    std::exception_ptr Failure;
    std::mutex FailureMutex;
    void rememberFailure() {
      std::lock_guard Lock(FailureMutex);
      if (!Failure)
        Failure = std::current_exception();
    }
    bool failed() {
      std::lock_guard Lock(FailureMutex);
      return bool(Failure);
    }
    static void progress(void *User, const char *Step, unsigned long long Done,
                         unsigned long long Total,
                         const char *Detail) noexcept {
      auto &Context = *static_cast<LoadContext *>(User);
      try {
        if (Context.Notify)
          Context.Notify((Context.Phase + ":" + Step).c_str(), Done, Total,
                         Detail);
      } catch (...) {
        Context.rememberFailure();
      }
    }
    static int stop(void *User) noexcept {
      auto &Context = *static_cast<LoadContext *>(User);
      try {
        return Context.failed() || (Context.Cancelled && Context.Cancelled());
      } catch (...) {
        Context.rememberFailure();
        return 1;
      }
    }
  } Context{Notify, Cancelled, Phase, {}, {}};
  neverd_session_set_load_progress(S.get(), LoadContext::progress, &Context);
  const int Loaded = api().LoadCancellable
                         ? api().LoadCancellable(S.get(), Name.c_str(),
                                                 LoadContext::stop, &Context)
                         : neverd_session_load(S.get(), Name.c_str());
  neverd_session_set_load_progress(S.get(), nullptr, nullptr);
  if (Context.Failure)
    std::rethrow_exception(Context.Failure);
  if (Loaded == -2 || (Cancelled && Cancelled()))
    throw Error("cancelled", std::string(Side) + ": input loading stopped");
  if (Loaded != 1)
    throw Error("comparison_load_failed",
                std::string(Side) + ": " + take(neverd_last_error(S.get())));
  if (Notify)
    Notify(Phase.c_str(), 1, 1, Name.c_str());
  return S;
}
struct Stamp {
  struct File {
    fs::path Path;
    fs::file_time_type Time;
    std::uintmax_t Size = 0;
    explicit File(fs::path P) : Path(std::move(P)) {
      Time = fs::last_write_time(Path);
      Size = fs::file_size(Path);
    }
    bool changed() const {
      std::error_code E;
      return fs::file_size(Path, E) != Size || E ||
             fs::last_write_time(Path, E) != Time || E;
    }
  } Input;
  fs::path Locator;
  fs::path WalPath, JournalPath;
  std::optional<File> Wal, Journal;
  explicit Stamp(const Json &Value)
      : Input(fs::canonical(path(pathField(Value)))),
        Locator(path(pathField(Value))) {
    // SQLite may commit solely to the WAL while the main NDDB inode is
    // unchanged. Include creation/removal of transaction companions as well.
    WalPath = Input.Path;
    WalPath += "-wal";
    JournalPath = Input.Path;
    JournalPath += "-journal";
    if (fs::exists(WalPath))
      Wal.emplace(WalPath);
    if (fs::exists(JournalPath))
      Journal.emplace(JournalPath);
  }
  bool changed() const {
    std::error_code LocatorError;
    if (fs::canonical(Locator, LocatorError) != Input.Path || LocatorError)
      return true;
    const auto Changed = [](const fs::path &P, const std::optional<File> &F) {
      std::error_code E;
      const bool Exists = fs::exists(P, E);
      return E || Exists != F.has_value() || (F && F->changed());
    };
    return Input.changed() || Changed(WalPath, Wal) ||
           Changed(JournalPath, Journal);
  }
};
void checkCancelled(const ComparisonService::Cancellation &Cancelled) {
  if (Cancelled && Cancelled())
    throw Error("cancelled", "Saved comparison restore cancelled");
}
struct ComparisonProgress {
  neverd_comparison_t Handle;
  const ComparisonService::Progress &Notify;
  const ComparisonService::Cancellation &Cancelled;
  static void report(void *Context, const char *Phase, unsigned long long Done,
                     unsigned long long Total) {
    const auto &S = *static_cast<const ComparisonProgress *>(Context);
    if (S.Notify)
      S.Notify(Phase, Done, Total, "");
    // The SDK resets cancellation when admitting a new operation. Forward
    // an already requested stop again at its first callback, without a race
    // against the polling monitor or a fast empty report.
    if (S.Cancelled && S.Cancelled())
      api().cancel(S.Handle);
  }
};
std::string readSaved(const Json &Payload,
                      const ComparisonService::Progress &Notify,
                      const ComparisonService::Cancellation &Cancelled) {
  checkCancelled(Cancelled);
  const auto Path = path(pathField(Payload));
  std::error_code E;
  const auto Size = fs::file_size(Path, E);
  if (E || Size > limits::kMaxSavedComparisonBytes)
    throw Error("comparison_read_failed",
                "Cannot read bounded comparison file");
  std::string Text;
  std::ifstream Input(Path, std::ios::binary);
  if (!Input)
    throw Error("comparison_read_failed", "Cannot read comparison file");
  std::array<char, limits::kSavedComparisonChunkBytes> Buffer;
  const auto Progress = [&] {
    if (Notify)
      Notify("restore:read", Text.size(), Size, "");
    checkCancelled(Cancelled);
  };
  Progress();
  while (Text.size() < Size) {
    const auto Count = std::min<uint64_t>(Buffer.size(), Size - Text.size());
    if (!Input.read(Buffer.data(), static_cast<std::streamsize>(Count)))
      throw Error("comparison_read_failed", "Cannot read comparison file");
    checkCancelled(Cancelled);
    Text.append(Buffer.data(), Count);
    Progress();
  }
  if (Input.peek() != std::char_traits<char>::eof())
    throw Error("comparison_read_failed",
                "Comparison file changed while reading");
  return Text;
}
void restoreSaved(neverd_comparison_t Handle, const std::string &Saved,
                  const ComparisonService::Progress &Notify,
                  const ComparisonService::Cancellation &Cancelled) {
  checkCancelled(Cancelled);
  ComparisonProgress Forward{Handle, Notify, Cancelled};
  const int OK =
      api().restore_json_with_progress
          ? api().restore_json_with_progress(
                Handle, Saved.c_str(), ComparisonProgress::report, &Forward)
          : api().restore_json(Handle, Saved.c_str());
  if (!OK) {
    const auto Status = parseJson(take(api().status_json(Handle)));
    throw Error(Status.value("restore_state", std::string()) == "cancelled"
                    ? "cancelled"
                    : "stale_comparison",
                take(api().last_error(Handle)));
  }
  // Success means the handle committed. A late request cannot turn that
  // commit into an error without advancing the standalone worker revision.
}

} // namespace

struct ComparisonService::Impl {
  Writer Write;
  neverd_comparison_t Handle = nullptr;
  std::unique_ptr<Stamp> PrimaryStamp, SecondaryStamp;
  Json Primary, Secondary;
  std::uint64_t Revision = 0;
  std::unique_ptr<GraphSnapshot> Graphs[2];
  std::string GraphMetricsKeys[2];
  bool GraphComplete[2] = {false, false};
  void clearGraphs() {
    Graphs[0].reset();
    Graphs[1].reset();
  }
  explicit Impl(Writer W) : Write(std::move(W)) {}
  ~Impl() {
    if (Handle)
      api().destroy(Handle);
  }
  bool stale() const {
    return PrimaryStamp &&
           (PrimaryStamp->changed() || SecondaryStamp->changed());
  }
  std::string text(const char *Value) const {
    if (!Value)
      throw Error("comparison_failed", take(api().last_error(Handle)));
    return take(Value);
  }
  Json result(const char *Value) const {
    auto Out = parseJson(text(Value), MaxBackendBytes);
    Out["comparison_revision"] = std::to_string(Revision);
    return Out;
  }
  Json summary() const {
    auto Out = result(api().summary_json(Handle));
    Out["stale"] = stale();
    Out["job"] = parseJson(text(api().status_json(Handle)));
    return Out;
  }
  void check(int OK, const char *Code = "comparison_failed") const {
    if (!OK)
      throw Error(Code, take(api().last_error(Handle)));
  }
};

ComparisonService::ComparisonService(Writer Write)
    : State(std::make_unique<Impl>(std::move(Write))) {}
ComparisonService::~ComparisonService() = default;
bool ComparisonService::sourceSupported() {
  return supported() && api().source_page_json;
}
bool ComparisonService::sourceDifferenceSupported() {
  return supported() && api().source_difference_json;
}
bool ComparisonService::supported() { return bool(api()); }

bool ComparisonService::cancellable(std::string_view Operation) {
  if (!supported() ||
      (Operation == "comparison_restore" &&
       !api().restore_json_with_progress) ||
      (Operation == "comparison_override" &&
       !api().override_json_with_progress) ||
      (Operation == "comparison_undo" && !api().undo_with_progress) ||
      (Operation == "comparison_redo" && !api().redo_with_progress) ||
      (Operation == "comparison_source_page" && !sourceSupported()) ||
      (Operation == "comparison_source_difference" &&
       !sourceDifferenceSupported()))
    return false;
  for (const auto Candidate : CancellableOperations)
    if (Candidate == Operation)
      return true;
  return false;
}

Json ComparisonService::execute(const std::string &Operation, const Json &P,
                                const Progress &Notify,
                                const Cancellation &Cancelled) {
  if (!supported())
    throw Error("unsupported", "This engine has no native comparison C API");
  auto &S = *State;
  Json Payload = P;
  if (Payload.contains("expected_comparison_revision")) {
    if (Payload.at("expected_comparison_revision") !=
        std::to_string(S.Revision))
      throw Error("stale_comparison",
                  "Comparison mapping changed; refresh before retrying");
    Payload.erase("expected_comparison_revision");
  }
  if (Operation == "comparison_start") {
    fields(Payload, {"primary", "secondary", "options", "restore_path"});
    if (!Payload.contains("primary") || !Payload.contains("secondary"))
      throw Error("invalid_request", "Two comparison inputs are required");
    const auto &A = Payload.at("primary"), &B = Payload.at("secondary");
    auto AS = std::make_unique<Stamp>(A), BS = std::make_unique<Stamp>(B);
    auto Primary = load(A, "primary", Notify, Cancelled);
    auto Secondary = load(B, "secondary", Notify, Cancelled);
    auto Settings = Payload.value("options", Json::object());
    std::string Saved;
    if (Payload.contains("restore_path")) {
      Saved =
          readSaved({{"path", Payload.at("restore_path")}}, Notify, Cancelled);
      if (Notify)
        Notify("restore:settings", 0, 0, "");
      checkCancelled(Cancelled);
      // The report was admitted by the saved-file reader. Its settings are
      // local input, not an IPC/backend reply subject to the smaller reply
      // allowance.
      const auto Document = parseJson(Saved, limits::kMaxSavedComparisonBytes);
      checkCancelled(Cancelled);
      if (!Document.contains("settings") ||
          !Document.at("settings").is_object() || !Settings.is_object())
        throw Error("invalid_request",
                    "Saved comparison and request need settings objects");
      for (const auto &[Key, Value] : Document.at("settings").items())
        Settings[Key] = Value;
    }
    const auto Options = Settings.dump();
    std::unique_ptr<void, decltype(api().destroy)> Next(api().create(),
                                                        api().destroy);
    if (!Next)
      throw Error("comparison_failed", "Cannot create comparison handle");
    ComparisonProgress Forward{Next.get(), Notify, Cancelled};
    if (Cancelled && Cancelled())
      api().cancel(Next.get());
    CancellationMonitor Monitor(Next.get(), Cancelled);
    auto AHandle = Primary.get(), BHandle = Secondary.get();
    const int OK =
        api().run_owned(Next.get(), &AHandle, &BHandle, Options.c_str(),
                        ComparisonProgress::report, &Forward);
    if (!AHandle)
      Primary.release();
    if (!BHandle)
      Secondary.release();
    if (!OK)
      throw Error("comparison_failed", take(api().last_error(Next.get())));
    if (AS->changed() || BS->changed())
      throw Error("stale_input",
                  "A comparison input changed while it was read");
    if (Payload.contains("restore_path")) {
      restoreSaved(Next.get(), Saved, Notify, Cancelled);
      // The replacement is still disposable until the session swap.
      checkCancelled(Cancelled);
    }
    if (S.Handle)
      api().destroy(S.Handle);
    S.Handle = Next.release();
    S.Primary = A;
    S.Secondary = B;
    S.PrimaryStamp = std::move(AS);
    S.SecondaryStamp = std::move(BS);
    ++S.Revision;
    S.clearGraphs();
    auto Out = S.summary();
    if (Payload.contains("restore_path"))
      Out["restore_completed"] = true;
    return Out;
  }
  if (!S.Handle)
    throw Error("comparison_not_loaded", "Start a comparison first");
  if (Operation == "comparison_summary") {
    fields(Payload, {});
    return S.summary();
  }
  if (Operation == "comparison_close") {
    fields(Payload, {});
    api().destroy(S.Handle);
    S.Handle = nullptr;
    ++S.Revision;
    S.PrimaryStamp.reset();
    S.SecondaryStamp.reset();
    return {{"closed", true}};
  }
  if (Operation == "comparison_save") {
    fields(Payload, {"path"});
    S.Write(path(pathField(Payload)),
            S.text(api().export_json(S.Handle)) + "\n");
    return {{"saved", true}, {"path", pathField(Payload)}};
  }
  if (Operation == "comparison_export") {
    fields(Payload, {});
    // Export keeps the exact shared schema; large exports use comparison_save.
    return parseJson(S.text(api().export_json(S.Handle)), MaxBackendBytes);
  }
  if (S.stale())
    throw Error("stale_input",
                "A comparison input changed; reload the comparison");
  CancellationMonitor Monitor(cancellable(Operation) ? S.Handle : nullptr,
                              Cancelled);
  if (Operation == "comparison_functions")
    return S.result(api().functions_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_source_page") {
    if (!api().source_page_json)
      throw Error("unsupported_operation",
                  "This engine has no comparison source view");
    return S.result(api().source_page_json(S.Handle, Payload.dump().c_str()));
  }
  if (Operation == "comparison_source_difference") {
    if (!api().source_difference_json)
      throw std::runtime_error("SDK does not support source line alignment");
    return S.result(
        api().source_difference_json(S.Handle, Payload.dump().c_str()));
  }
  if (Operation == "comparison_function_page")
    return S.result(api().function_page_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_regions")
    return S.result(api().regions_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_flow_regions")
    return S.result(api().flow_regions_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_region_page")
    return S.result(api().region_page_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_reviews")
    return S.result(api().reviews_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_observations")
    return S.result(api().observations_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_scalar_validation")
    return S.result(
        api().owned_scalar_validation_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_dataflow")
    return S.result(api().dataflow_json(S.Handle, Payload.dump().c_str()));
  if (Operation == "comparison_slice" || Operation == "comparison_impact") {
    return S.result((Operation == "comparison_slice" ? api().slice_json
                                                     : api().impact_json)(
        S.Handle, Payload.dump().c_str()));
  }
  if (Operation == "comparison_cfg_summary" ||
      Operation == "comparison_cfg_viewport" ||
      Operation == "comparison_cfg_focus") {
    const auto Side = stringField(Payload, "side", "primary");
    if (Side != "primary" && Side != "secondary")
      throw Error("invalid_request",
                  "Comparison side must be primary or secondary");
    const auto Address =
        hexAddress(parseAddress(stringField(Payload, "address")));
    const int Index = Side == "secondary";
    const auto Metrics = GraphMetrics::fromRequest(Payload);
    const auto MetricsKey = Metrics.key();
    if (!S.Graphs[Index] || S.Graphs[Index]->address() != Address ||
        Payload.value("refresh", false) ||
        (Operation == "comparison_cfg_summary" &&
         S.GraphMetricsKeys[Index] != MetricsKey)) {
      Json Query{
          {"side", Side},
          {"address", Address},
          {"node_limit", ::neverd::limits::worker::kGraphSnapshotMaxNodes},
          {"edge_limit", ::neverd::limits::worker::kGraphSnapshotMaxEdges}};
      for (const auto *Key : {"timeout_ms", "refresh"})
        if (Payload.contains(Key))
          Query[Key] = Payload.at(Key);
      auto Graph =
          parseJson(S.text(api().graph_json(S.Handle, Query.dump().c_str())),
                    MaxBackendBytes);
      if (!Graph.value("computation_complete", true))
        throw Error(Graph.value("state", std::string("comparison_stopped")),
                    "Function comparison stopped; refresh to retry");
      S.GraphComplete[Index] = Graph.value("analysis_complete", false);
      S.GraphMetricsKeys[Index] = MetricsKey;
      S.Graphs[Index] = std::make_unique<GraphSnapshot>(
          std::move(Graph), Address,
          std::to_string(S.Revision) + ":" + Side + ":" + Address + ":" +
              MetricsKey,
          Metrics);
    }
    if (Operation == "comparison_cfg_focus") {
      auto Box = S.Graphs[Index]->locate(
          hexAddress(parseAddress(stringField(Payload, "block"))));
      if (Box.is_null())
        throw Error("invalid_request", "Block is absent from comparison graph");
      return Box;
    }
    auto Result = Operation == "comparison_cfg_summary"
                      ? S.Graphs[Index]->summary()
                      : S.Graphs[Index]->viewport(Payload);
    Result["comparison_revision"] = std::to_string(S.Revision);
    Result["analysis_complete"] = S.GraphComplete[Index];
    return Result;
  }
  if (Operation == "comparison_function") {
    fields(Payload, {"side", "address"});
    const auto Side = stringField(Payload, "side", "primary");
    if (Side != "primary" && Side != "secondary")
      throw Error("invalid_request",
                  "Comparison side must be primary or secondary");
    return S.result(
        api().function_json(S.Handle, Side == "secondary",
                            parseAddress(stringField(Payload, "address"))));
  }
  if (Operation == "comparison_restore") {
    fields(Payload, {"path"});
    const auto Saved = readSaved(Payload, Notify, Cancelled);
    restoreSaved(S.Handle, Saved, Notify, Cancelled);
  } else if (Operation == "comparison_review_record") {
    S.check(api().review_record_json(S.Handle, Payload.dump().c_str()));
  } else if (Operation == "comparison_override" ||
             Operation == "comparison_undo" || Operation == "comparison_redo") {
    ComparisonProgress Forward{S.Handle, Notify, Cancelled};
    int OK;
    if (Operation == "comparison_override") {
      const auto Text = Payload.dump();
      OK = api().override_json_with_progress
               ? api().override_json_with_progress(S.Handle, Text.c_str(),
                                                   ComparisonProgress::report,
                                                   &Forward)
               : api().override_json(S.Handle, Text.c_str());
    } else {
      fields(Payload, {});
      const bool Undo = Operation == "comparison_undo";
      const auto Controlled =
          Undo ? api().undo_with_progress : api().redo_with_progress;
      OK = Controlled
               ? Controlled(S.Handle, ComparisonProgress::report, &Forward)
               : (Undo ? api().undo : api().redo)(S.Handle);
    }
    if (!OK) {
      const auto Status = parseJson(take(api().status_json(S.Handle)));
      S.check(OK, Status.value("edit_state", std::string()) == "cancelled"
                      ? "cancelled"
                      : "comparison_failed");
    }
    // A successful edit is committed; a later stop cannot hide its revision.
  } else if (Operation == "comparison_validate") {
    fields(Payload, {});
    auto Primary = load(S.Primary, "primary", Notify, Cancelled);
    auto Secondary = load(S.Secondary, "secondary", Notify, Cancelled);
    S.check(api().validate_inputs(S.Handle, Primary.get(), Secondary.get()),
            "stale_input");
    return S.summary();
  } else {
    throw Error("unsupported", "Unknown comparison operation");
  }
  ++S.Revision;
  S.clearGraphs();
  auto Out = S.summary();
  if (Operation == "comparison_restore")
    Out["restore_completed"] = true;
  else if (Operation == "comparison_override" ||
           Operation == "comparison_undo" || Operation == "comparison_redo")
    Out["edit_completed"] = true;
  return Out;
}
} // namespace neverd::worker
