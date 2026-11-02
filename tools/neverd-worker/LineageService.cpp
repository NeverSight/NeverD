//===- LineageService.cpp - File-backed version comparison workspace ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps operation outcome separate from loaded evidence, forwards cancellation
/// and commits saved files once after the final cooperative checkpoint.
//===----------------------------------------------------------------------===//

#include "LineageService.h"

#include "EngineSymbols.h"

#include "neverd/diff/Limits.h"
#include "neverd/sdk/NeverDCAPILineage.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace neverd::worker {
namespace {
namespace limits = ::neverd::limits::diff;
struct API {
#define ENTRY(Name)                                                            \
  decltype(&neverd_lineage_##Name) Name =                                      \
      engineSymbol<decltype(Name)>("neverd_lineage_" #Name)
  ENTRY(create);
  ENTRY(destroy);
  ENTRY(cancel);
  ENTRY(status_json);
  ENTRY(last_error);
  ENTRY(load_json);
  ENTRY(edit_releases_json);
  ENTRY(summary_json);
  ENTRY(query_json);
  ENTRY(export_json);
  ENTRY(report_json);
#undef ENTRY
  explicit operator bool() const {
    return create && destroy && cancel && status_json && last_error &&
           load_json && summary_json && query_json && export_json &&
           report_json;
  }
};
const API &api() {
  static const API A;
  return A;
}
std::string take(const char *Text) {
  const std::unique_ptr<const char, decltype(&neverd_free_string)> Owned(
      Text, neverd_free_string);
  return Text ? Text : "";
}
void fields(const Json &O, std::initializer_list<std::string_view> Allowed) {
  if (!O.is_object())
    throw Error("invalid_request", "Lineage payload must be an object");
  for (const auto &[K, V] : O.items()) {
    (void)V;
    if (std::find(Allowed.begin(), Allowed.end(), K) == Allowed.end())
      throw Error("invalid_request", "Unknown lineage field: " + K);
  }
}
void check(const LineageService::Cancellation &Cancelled) {
  if (Cancelled && Cancelled())
    throw Error("cancelled", "Lineage operation cancelled");
}
std::filesystem::path path(const std::string &Text) {
  if (Text.empty() || Text.find('\0') != std::string::npos)
    throw Error("invalid_request", "A lineage file path is required");
  return std::filesystem::u8path(Text.begin(), Text.end());
}

struct Forward {
  neverd_lineage_t Handle;
  const LineageService::Progress &Notify;
  const LineageService::Cancellation &Cancelled;
  std::mutex Mutex;
  std::exception_ptr Failure;
  void probe() noexcept {
    try {
      if (Cancelled && Cancelled())
        api().cancel(Handle);
    } catch (...) {
      fail();
    }
  }
  void fail() noexcept {
    {
      std::lock_guard Lock(Mutex);
      if (!Failure)
        Failure = std::current_exception();
    }
    api().cancel(Handle);
  }
  void rethrow() {
    std::lock_guard Lock(Mutex);
    if (Failure)
      std::rethrow_exception(Failure);
  }
  static void progress(void *Context, const char *Phase, neverd_va_t Done,
                       neverd_va_t Total) noexcept {
    auto &F = *static_cast<Forward *>(Context);
    try {
      F.probe();
      if (F.Notify)
        F.Notify(Phase, Done, Total, "");
      F.probe();
    } catch (...) {
      F.fail();
    }
  }
};
class Monitor {
  std::atomic<bool> Done{false};
  std::thread Thread;

public:
  explicit Monitor(Forward &F)
      : Thread([&, this] {
          while (!Done.load(std::memory_order_relaxed)) {
            F.probe();
            std::this_thread::sleep_for(std::chrono::milliseconds(
                limits::kCancellationPollMilliseconds));
          }
        }) {}
  void stop() {
    Done = true;
    if (Thread.joinable())
      Thread.join();
  }
  ~Monitor() { stop(); }
};

std::string read(const std::string &Name, uint64_t &Aggregate,
                 const LineageService::Progress &Notify,
                 const LineageService::Cancellation &Cancelled) {
  check(Cancelled);
  const auto Path = path(Name);
  std::error_code SizeError;
  const auto Size = std::filesystem::file_size(Path, SizeError);
  if (!SizeError && Size > limits::kMaxSavedComparisonBytes - Aggregate)
    throw Error("resource_limited",
                "Lineage inputs exceed the aggregate file allowance");
  std::ifstream File(Path, std::ios::binary);
  if (!File)
    throw Error("load_failed", "Cannot read lineage source: " + Name);
  std::string Text;
  std::array<char, limits::kSavedComparisonChunkBytes> Buffer;
  while (File) {
    check(Cancelled);
    File.read(Buffer.data(), Buffer.size());
    const auto Count = uint64_t(File.gcount());
    if (Count > limits::kMaxSavedComparisonBytes - Aggregate)
      throw Error("resource_limited",
                  "Lineage inputs exceed the aggregate file allowance");
    Aggregate += Count;
    Text.append(Buffer.data(), Count);
    if (Notify)
      Notify("lineage:read", Text.size(), 0, Name.c_str());
  }
  if (!File.eof())
    throw Error("load_failed", "Cannot finish reading lineage source: " + Name);
  check(Cancelled);
  return Text;
}
} // namespace

struct LineageService::Impl {
  Writer Write;
  neverd_lineage_t Handle = nullptr;
  Json Summary = Json::object();
  std::string Outcome = "idle";
  explicit Impl(Writer W) : Write(std::move(W)) {}
  ~Impl() {
    if (Handle)
      api().destroy(Handle);
  }
  std::string revision() const {
    return Summary.value("revision", std::string());
  }
  Json status() const {
    return {{"operation_state", Outcome},
            {"loaded_revision", revision()},
            {"result_available", !Summary.empty()}};
  }
  std::string text(const char *Value) const {
    if (Value)
      return take(Value);
    const auto Job =
        parseJson(take(api().status_json(Handle)), MaxBackendBytes);
    auto Code = Job.value("operation_state", std::string());
    if (Code != "cancelled" && Code != "timed_out" &&
        Code != "resource_limited")
      Code = "lineage_failed";
    throw Error(Code, take(api().last_error(Handle)), status());
  }
  Json summary() const {
    auto Out = Summary;
    Out["job"] = status();
    return Out;
  }
};
LineageService::LineageService(Writer W)
    : State(std::make_unique<Impl>(std::move(W))) {}
LineageService::~LineageService() = default;
bool LineageService::supported() { return bool(api()); }
bool LineageService::releaseEditingSupported() {
  return supported() && api().edit_releases_json;
}
bool LineageService::cancellable(std::string_view Operation) {
  if (Operation == "lineage_edit_releases")
    return releaseEditingSupported();
  return supported() && std::find(std::begin(CancellableOperations),
                                  std::end(CancellableOperations),
                                  Operation) != std::end(CancellableOperations);
}

Json LineageService::execute(const std::string &Operation, const Json &Payload,
                             const Progress &Notify,
                             const Cancellation &Cancelled) {
  if (!supported())
    throw Error("unsupported", "This engine has no offline version graph API");
  auto &S = *State;
  Json P = Payload;
  if (!P.is_object())
    throw Error("invalid_request", "Lineage payload must be an object");
  if (P.contains("expected_lineage_revision")) {
    if (P.at("expected_lineage_revision") != S.revision())
      throw Error("stale_lineage",
                  "Version graph changed; refresh before retrying", S.status());
    P.erase("expected_lineage_revision");
  }
  if (Operation == "lineage_status") {
    fields(P, {});
    return S.status();
  }
  if (Operation == "lineage_summary") {
    fields(P, {});
    return S.summary();
  }
  if (Operation == "lineage_close") {
    fields(P, {});
    if (S.Handle)
      api().destroy(S.Handle);
    S.Handle = nullptr;
    S.Summary = Json::object();
    S.Outcome = "idle";
    return S.status();
  }
  if (!S.Handle) {
    S.Handle = api().create();
    if (!S.Handle)
      throw Error("lineage_failed", "Cannot allocate version graph handle");
  }
  S.Outcome = "running";
  try {
    check(Cancelled);
    Forward F{S.Handle, Notify, Cancelled, {}, {}};
    Monitor Stop(F);
    if (Operation == "lineage_edit_releases") {
      if (!releaseEditingSupported())
        throw Error("unsupported", "This engine cannot edit release metadata");
      if (S.Summary.empty())
        throw Error("invalid_request", "No version graph is loaded");
      fields(P, {"edit", "options"});
      auto Edit = P.value("edit", Json::object());
      if (!Edit.is_object())
        throw Error("invalid_request", "Release edit must be an object");
      if (!Edit.contains("revision"))
        Edit["revision"] = S.revision();
      const auto Text =
          transport::serializeJson(Edit, limits::kMaxOptionsBytes);
      const auto Options = transport::serializeJson(
          P.value("options", Json::object()), limits::kMaxOptionsBytes);
      const auto OK = api().edit_releases_json(
          S.Handle, Text.c_str(), Options.c_str(), Forward::progress, &F);
      Stop.stop();
      F.rethrow();
      if (!OK)
        (void)S.text(nullptr);
      S.Summary =
          parseJson(S.text(api().summary_json(S.Handle)), MaxBackendBytes);
      S.Outcome = "complete";
      auto Out = S.summary();
      Out["edit_completed"] = true;
      return Out;
    }
    if (Operation == "lineage_load") {
      fields(P, {"reports", "path", "append", "versions", "version_updates",
                 "options"});
      const bool Append = P.value("append", false);
      if (Append && (S.Summary.empty() || P.contains("path")))
        throw Error("invalid_request",
                    "Append requires an existing graph and no reopen path");
      Json Document{{"schema", "neverd.bindiff.lineage"},
                    {"schema_version", 1},
                    {"reports", Json::array()}};
      uint64_t Aggregate = 0;
      if (Append) {
        const auto Text =
            S.text(api().export_json(S.Handle, Forward::progress, &F));
        Aggregate = Text.size();
        Document = parseJson(Text, limits::kMaxSavedComparisonBytes);
      } else if (P.contains("path")) {
        const auto Name = stringField(P, "path");
        const auto Text = read(Name, Aggregate, Notify, Cancelled);
        try {
          Document = parseJson(Text, limits::kMaxSavedComparisonBytes);
        } catch (const Error &E) {
          if (E.code != "invalid_json")
            throw;
          auto Detail = E.detail;
          Detail["source"] = "lineage_file";
          Detail["path"] = Name;
          throw Error(E.code,
                      "Version graph file is not valid UTF-8 JSON: " + Name,
                      std::move(Detail));
        }
      }
      check(Cancelled);
      if (!Document.is_object() || !Document.contains("reports") ||
          !Document["reports"].is_array())
        throw Error("invalid_request",
                    "Lineage file must contain a reports array");
      if (P.contains("reports")) {
        if (!P["reports"].is_array())
          throw Error("invalid_request", "Report paths must be an array");
        for (const auto &Name : P["reports"]) {
          if (!Name.is_string())
            throw Error("invalid_request", "Report path must be a string");
          Document["reports"].push_back(
              read(Name.get<std::string>(), Aggregate, Notify, Cancelled));
        }
      }
      if (P.contains("versions"))
        Document["versions"] = P["versions"];
      if (P.contains("version_updates")) {
        if (P.contains("versions") || !P["version_updates"].is_array())
          throw Error("invalid_request",
                      "Version updates require a separate metadata array");
        std::set<std::string> IDs;
        for (const auto &V : P["version_updates"])
          if (V.is_object() && V.contains("id") && V["id"].is_string())
            IDs.insert(V["id"].get<std::string>());
        auto Versions = Document.value("versions", Json::array());
        if (!Versions.is_array())
          throw Error("invalid_request", "Version metadata must be an array");
        // Keep duplicate updates in the candidate so the shared reader rejects
        // them. This adapter only merges requested edits with retained labels.
        Versions.erase(
            std::remove_if(Versions.begin(), Versions.end(),
                           [&](const auto &V) {
                             check(Cancelled);
                             return V.is_object() && V.contains("id") &&
                                    V["id"].is_string() &&
                                    IDs.contains(
                                        V["id"].template get<std::string>());
                           }),
            Versions.end());
        for (const auto &V : P["version_updates"])
          Versions.push_back(V);
        Document["versions"] = std::move(Versions);
      }
      const auto Text =
          transport::serializeJson(Document, limits::kMaxSavedComparisonBytes);
      const auto Options = transport::serializeJson(
          P.value("options", Json::object()), limits::kMaxOptionsBytes);
      const auto OK = api().load_json(S.Handle, Text.c_str(), Options.c_str(),
                                      Forward::progress, &F);
      Stop.stop();
      F.rethrow();
      if (!OK)
        (void)S.text(nullptr);
      // The native commit has happened. Stop forwarding late requests before
      // the bounded readback; no callback/check may relabel it as cancelled.
      S.Summary =
          parseJson(S.text(api().summary_json(S.Handle)), MaxBackendBytes);
      S.Outcome = "complete";
      auto Out = S.summary();
      Out["load_completed"] = true;
      return Out;
    }
    if (S.Summary.empty())
      throw Error("invalid_request", "No version graph is loaded");
    if (Operation == "lineage_query") {
      fields(P, {"query"});
      auto Q = P.value("query", Json::object());
      if (!Q.is_object())
        throw Error("invalid_request", "Lineage query must be an object");
      if (!Q.contains("revision"))
        Q["revision"] = S.revision();
      const auto Text = transport::serializeJson(Q, limits::kMaxOptionsBytes);
      const auto Page = S.text(
          api().query_json(S.Handle, Text.c_str(), Forward::progress, &F));
      Stop.stop();
      F.rethrow();
      S.Outcome = "complete";
      return parseJson(Page, limits::kMaxLineagePageBytes);
    }
    if (Operation == "lineage_save" || Operation == "lineage_save_report") {
      fields(P,
             Operation == "lineage_save"
                 ? std::initializer_list<std::string_view>{"path"}
                 : std::initializer_list<std::string_view>{"path", "report"});
      const auto Name = stringField(P, "path");
      const auto Destination = path(Name);
      const auto Text =
          Operation == "lineage_save"
              ? S.text(api().export_json(S.Handle, Forward::progress, &F))
              : S.text(api().report_json(S.Handle,
                                         stringField(P, "report").c_str(),
                                         Forward::progress, &F));
      Stop.stop();
      F.rethrow();
      Json Result{{"path", Name},
                  {"bytes", Text.size()},
                  {"revision", S.revision()},
                  {"save_completed", true}};
      S.Write(Destination, Text, [&] {
        if (Notify)
          Notify("lineage:save_publish", Text.size(), Text.size(),
                 Name.c_str());
        check(Cancelled);
      });
      S.Outcome = "complete";
      return Result;
    }
    throw Error("unsupported", "Unknown lineage operation");
  } catch (Error &E) {
    S.Outcome = E.code == "cancelled" || E.code == "timed_out" ||
                        E.code == "resource_limited"
                    ? E.code
                    : "failed";
    E.detail["lineage"] = S.status();
    throw;
  } catch (...) {
    S.Outcome = "failed";
    throw;
  }
}
} // namespace neverd::worker
