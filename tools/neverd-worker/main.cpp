#include "ComparisonService.h"
#include "Engine.h"
#include "LineageService.h"
#include "LoadProgress.h"
#include "Protocol.h"
#include "ResourceLimits.h"
#include "UnwindQuery.h"
#include "WebEngine.h"

#ifdef NEVERD_WORKER_HAS_MIMALLOC
#include <mimalloc.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

using namespace neverd::worker;
namespace {
class Transport {
public:
  Transport() {
    // Reserve a private protocol descriptor before creating any Session or
    // loading plugins. C stdio, C++ stdout and Python print now go to stderr.
    std::fflush(stdout);
#ifdef _WIN32
    descriptor_ = _dup(_fileno(stdout));
    if (descriptor_ < 0 || _dup2(_fileno(stderr), _fileno(stdout)) != 0)
      throw std::runtime_error("Cannot isolate protocol output");
    _setmode(descriptor_, _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#else
    descriptor_ = ::dup(STDOUT_FILENO);
    if (descriptor_ < 0 || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
      throw std::runtime_error("Cannot isolate protocol output");
    (void)::fcntl(descriptor_, F_SETFD, FD_CLOEXEC);
    std::signal(SIGPIPE, SIG_IGN);
#endif
  }
  ~Transport() {
#ifdef _WIN32
    _close(descriptor_);
#else
    ::close(descriptor_);
#endif
  }
  bool send(Json message, const EncodedJson *payload = nullptr) {
    std::string encoded;
    try {
      encoded = payload ? frame(std::move(message), *payload) : frame(message);
    } catch (const Error &) {
      Json failure = message;
      failure["status"] = "budget_exceeded";
      failure["payload"] = Json::object();
      failure["error"] = {
          {"code", "budget_exceeded"},
          {"message", "Response exceeded the 8 MiB frame budget"}};
      encoded = frame(failure);
    }
    std::lock_guard guard(mutex_);
    if (failed_)
      return false;
    std::size_t offset = 0;
    while (offset < encoded.size()) {
#ifdef _WIN32
      const auto count = _write(descriptor_, encoded.data() + offset,
                                static_cast<unsigned>(encoded.size() - offset));
#else
      const auto count = ::write(descriptor_, encoded.data() + offset,
                                 encoded.size() - offset);
#endif
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0) {
        failed_ = true;
        return false;
      }
      offset += static_cast<std::size_t>(count);
    }
    return true;
  }
  static bool readExact(char *buffer, std::size_t size, bool header) {
    std::size_t offset = 0;
    while (offset < size) {
#ifdef _WIN32
      const auto count = _read(_fileno(stdin), buffer + offset,
                               static_cast<unsigned>(size - offset));
#else
      const auto count = ::read(STDIN_FILENO, buffer + offset, size - offset);
#endif
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0) {
        if (!offset && header && count == 0)
          return false;
        throw Error("truncated_frame", "Input closed during a protocol frame");
      }
      offset += static_cast<std::size_t>(count);
    }
    return true;
  }

private:
  int descriptor_ = -1;
  std::mutex mutex_;
  bool failed_ = false;
};

struct Request {
  Json value;
  std::size_t bytes;
  std::atomic<bool> cancelled{false};
  Request(Json Value, std::size_t Bytes)
      : value(std::move(Value)), bytes(Bytes) {}
};
bool isWeb(const Json &request) {
  if (!request.is_object())
    return false;
  const auto operation = request.find("operation");
  return operation != request.end() && operation->is_string() &&
         operation->get_ref<const std::string &>().starts_with("web_");
}
struct State {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::shared_ptr<Request>> queue;
  std::shared_ptr<Request> active;
  std::set<std::string> pending;
  std::size_t queuedBytes = 0;
  bool stopping = false;
  bool finished = false;
  std::string revision = "0", projectId;
  // Engine background work state, shared by replies and heartbeats.
  Json background = nullptr;
  uint64_t BackgroundSequence = 0;
  std::string webRevision = "0", webProjectId;
  /// Caller holds mutex; every executor publication has a monotonic sequence.
  void publishBackground(Json Background) {
    background = std::move(Background);
    ++BackgroundSequence;
  }
  const std::string &revisionFor(const Json &request) const {
    return isWeb(request) ? webRevision : revision;
  }
  const std::string &projectFor(const Json &request) const {
    return isWeb(request) ? webProjectId : projectId;
  }
};

/// Caller holds state.mutex, so both periodic and readiness notifications
/// describe one published executor state with the same protocol envelope.
Json heartbeatMessage(const State &state) {
  return {
      {"protocol_major", 1},
      {"type", "heartbeat"},
      {"revision", state.revision},
      {"project_id", state.projectId},
      {"web",
       {{"revision", state.webRevision}, {"project_id", state.webProjectId}}},
      {"active_request_id",
       state.active ? state.active->value["request_id"] : Json(nullptr)},
      {"cancellation_requested", state.active && state.active->cancelled},
      {"background_sequence", std::to_string(state.BackgroundSequence)},
      {"background", state.background}};
}

bool discoveryComplete(const Json &background) {
  return background.is_object() &&
         background.value("discovery_complete", false);
}

Json response(const Json &request, const std::string &revision,
              const std::string &project, const std::string &status,
              Json payload = Json::object()) {
  Json result{{"protocol_major", 1},
              {"type", "response"},
              {"request_id", request.value("request_id", std::string())},
              {"operation", request.value("operation", std::string())},
              {"revision", revision},
              {"project_id", project},
              {"status", status},
              {"payload", std::move(payload)}};
  if (isWeb(request))
    result["domain"] = "web";
  return result;
}

Json failure(const Json &request, const std::string &revision,
             const std::string &project, const Error &error) {
  auto result =
      response(request, revision, project,
               error.code == "budget_exceeded" ? "budget_exceeded" : "error",
               error.detail);
  result["error"] = {{"code", error.code}, {"message", error.what()}};
  return result;
}

Json hello() {
  Json result{{"protocol_major", 1},
              {"protocol_minor", 1},
              {"type", "hello"},
              {"engine_version", Engine::version()},
              {"project_schema", 1},
              {"revision", "0"},
              {"project_id", ""},
              {"max_frame_bytes", MaxFrameBytes},
              {"capabilities",
               {"metadata",
                "analysis_sections",
                "analysis_section_add",
                "analysis_section_remove",
                "analysis_snapshot",
                "analysis_restore",
                "analysis_update_names",
                "functions",
                "disasm",
                "bytes",
                "decompile",
                "signatures_load",
                "cfg",
                "cfg_summary",
                "cfg_viewport",
                "xrefs",
                "strings",
                "analyze",
                "resolve",
                "annotations",
                "annotation_set",
                "rename",
                "code_edit",
                "code_edit_prepare",
                "source_type",
                "source_variables",
                "call_sites",
                "save",
                "reload",
                "segments",
                "read_only",
                "heartbeat",
                "cancel",
                "history",
                "undo",
                "redo",
                "history_reset",
                "project_sync",
                "contributions",
                "contribution_register",
                "contribution_unregister",
                "contribution_execute",
                "listing",
                "overview",
                "names",
                "regions",
                "imports",
                "exports",
                "search",
                "register_flow",
                "string_encodings",
                "string_options",
                "string_references",
                "function_level_analysis",
                "background_index"}},
              {"cancellation",
               "queued_requests_stop; "
               "synchronous_engine_requires_completion_or_process_restart"},
              {"storage", "existing_json_sidecars"}};
  if (Engine::comparesBinaries()) {
    for (const char *operation :
         {"comparison_start",         "comparison_summary",
          "comparison_functions",     "comparison_function",
          "comparison_function_page", "comparison_regions",
          "comparison_region_page",   "comparison_dataflow",
          "comparison_flow_regions",  "comparison_slice",
          "comparison_impact",        "comparison_cfg_summary",
          "comparison_cfg_viewport",  "comparison_cfg_focus",
          "comparison_save",          "comparison_export",
          "comparison_restore",       "comparison_override",
          "comparison_undo",          "comparison_redo",
          "comparison_validate",      "comparison_close",
          "comparison_observations",  "comparison_scalar_validation",
          "comparison_reviews",       "comparison_review_record"})
      result["capabilities"].push_back(operation);
    if (ComparisonService::sourceSupported())
      result["capabilities"].push_back("comparison_source_page");
    if (ComparisonService::sourceDifferenceSupported())
      result["capabilities"].push_back("comparison_source_difference");
    result["comparison_cancellation"] =
        "cooperative; partial snapshots retained";
    result["comparison_cancellable_operations"] = Json::array();
    for (const auto Operation : ComparisonService::CancellableOperations)
      if (ComparisonService::cancellable(Operation))
        result["comparison_cancellable_operations"].push_back(
            std::string(Operation));
  }
  if (LineageService::supported()) {
    for (const char *Operation :
         {"lineage_load", "lineage_summary", "lineage_status", "lineage_query",
          "lineage_save", "lineage_save_report", "lineage_close"})
      result["capabilities"].push_back(Operation);
    if (LineageService::releaseEditingSupported())
      result["capabilities"].push_back("lineage_edit_releases");
    result["lineage_cancellation"] =
        "cooperative; previous collection retained on stop";
    result["lineage_cancellable_operations"] = Json::array();
    for (const auto Operation : LineageService::CancellableOperations)
      if (LineageService::cancellable(Operation))
        result["lineage_cancellable_operations"].push_back(
            std::string(Operation));
  }
  // Function edits, data items and operand formats need an engine that keeps
  // them.
  if (Engine::keepsFunctionEdits())
    for (const char *operation : {"function_create", "function_delete"})
      result["capabilities"].push_back(operation);
  if (Engine::statesFunctionAttributes())
    for (const char *operation : {"function_attributes", "function_edit"})
      result["capabilities"].push_back(operation);
  if (Engine::typesFunctions())
    result["capabilities"].push_back("function_type");
  if (Engine::keepsDataItems())
    result["capabilities"].push_back("item_define");
  if (Engine::keepsBytePatches())
    for (const char *operation :
         {"patch_context", "patch_preview", "patch_apply", "patch_revert",
          "patches", "patch_export"})
      result["capabilities"].push_back(operation);
  if (Engine::keepsSourcePatches())
    for (const char *operation : {"source_patch", "source_patch_context"})
      result["capabilities"].push_back(operation);
  if (Engine::provesSourcePatches())
    for (const char *operation :
         {"patch_source_target", "patch_source_preview"})
      result["capabilities"].push_back(operation);
  if (Engine::keepsOperandFormats())
    result["capabilities"].push_back("operand_format");
  if (Engine::identifiesFiles())
    result["capabilities"].push_back("identify");
  if (Engine::loadsPdbFiles()) {
    result["capabilities"].push_back("pdb_inspect");
    result["capabilities"].push_back("pdb_load");
  }
  if (Engine::findsPdbFiles())
    result["capabilities"].push_back("pdb_reference");
  if (Engine::listsLocalTypes())
    for (const char *operation :
         {"local_types", "local_type", "local_types_source"})
      result["capabilities"].push_back(operation);
  if (Engine::declaresLocalTypes())
    for (const char *operation : {"local_types_declare", "local_types_forget"})
      result["capabilities"].push_back(operation);
  if (Engine::choosesTypeLibraries())
    for (const char *operation : {"type_libraries", "type_libraries_set"})
      result["capabilities"].push_back(operation);
  // Structure offsets are operand formats that name local types.
  if (Engine::fitsLocalTypes()) {
    result["capabilities"].push_back("local_types_fit");
    if (Engine::keepsOperandFormats())
      result["capabilities"].push_back("operand_struct");
  }
  if (Engine::findsDwarf())
    result["capabilities"].push_back("dwarf_reference");
  if (Engine::findsSymbolTables())
    result["capabilities"].push_back("symbol_table_reference");
  if (Engine::findsMapFiles())
    result["capabilities"].push_back("map_reference");
  if (Engine::loadsMapFiles()) {
    result["capabilities"].push_back("map_inspect");
    result["capabilities"].push_back("map_load");
  }
  if (Engine::loadsDwarfFiles())
    result["capabilities"].push_back("dwarf_load");
  if (Engine::findsCallPaths())
    for (const char *operation : {"call_paths", "call_tree"})
      result["capabilities"].push_back(operation);
  if (Engine::reportsUnwindFrames())
    result["capabilities"].push_back(unwind_query::kOperation);
  if (Engine::reportsX87Capabilities())
    result["capabilities"].push_back("x87_capabilities");
  result["web"] = WebEngine::capabilities();
  return result;
}

/// How long requests must pause before background work starts.
constexpr auto IdleGracePeriod = std::chrono::milliseconds(
    ::neverd::limits::worker::kMainIdleGracePeriodMilliseconds);

#ifdef NEVERD_WORKER_HAS_MIMALLOC
void collectPagesAfterIdle(State &state, std::unique_lock<std::mutex> &lock) {
  const long delay = mi_option_get(mi_option_purge_delay);
  if (delay < 0)
    return;
  using Clock = std::chrono::steady_clock;
  const auto now = Clock::now();
  const auto maximumDelay =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          Clock::duration::max());
  const auto duration = std::chrono::duration_cast<Clock::duration>(
      std::min(std::chrono::milliseconds(delay), maximumDelay));
  const auto deadline =
      now.time_since_epoch() > Clock::duration::max() - duration
          ? Clock::time_point::max()
          : now + duration;
  if (!state.changed.wait_until(lock, deadline, [&] {
        return state.stopping || !state.queue.empty();
      })) {
    // Free pages belong to this process; every live SDK allocation stays.
    lock.unlock();
    mi_collect(true);
    lock.lock();
  }
}
#endif

void executeLoop(State &state, Transport &transport) {
  try {
    Engine engine;
    WebEngine web;
    while (true) {
      std::shared_ptr<Request> request;
      {
        std::unique_lock lock(state.mutex);
        // Background engine work runs only while no request is waiting, and
        // only once requests have paused, so a client's burst of reads after
        // opening a file does not queue behind it.  Each step is bounded so
        // interactive requests keep their latency.
        if (!state.stopping && state.queue.empty() && engine.hasIdleWork())
          state.changed.wait_for(lock, IdleGracePeriod, [&] {
            return state.stopping || !state.queue.empty();
          });
        while (!state.stopping && state.queue.empty() && engine.hasIdleWork()) {
          const bool wasDiscovered = discoveryComplete(state.background);
          lock.unlock();
          bool advanced = true;
          try {
            advanced = engine.idleStep();
          } catch (const std::exception &) {
            // A failed step leaves its error in the published state.
          }
          lock.lock();
          state.publishBackground(engine.backgroundState());
          if (!wasDiscovered && discoveryComplete(state.background)) {
            // Large-image clients wait for discovery publication before
            // starting a replica. Notify them now, then admit their request
            // before starting a potentially expensive derived-list rebuild.
            // The same idle grace wakes immediately when a request arrives.
            auto message = heartbeatMessage(state);
            lock.unlock();
            const bool sent = transport.send(std::move(message));
            lock.lock();
            if (!sent)
              throw Error("transport_closed", "Cannot publish discovery state");
            state.changed.wait_for(lock, IdleGracePeriod, [&] {
              return state.stopping || !state.queue.empty();
            });
          } else if (!advanced)
            state.changed.wait_for(
                lock,
                std::chrono::milliseconds(
                    ::neverd::limits::worker::kMainExecuteLoopMilliseconds),
                [&] { return state.stopping || !state.queue.empty(); });
        }
#ifdef NEVERD_WORKER_HAS_MIMALLOC
        if (!state.stopping && state.queue.empty())
          collectPagesAfterIdle(state, lock);
#endif
        state.changed.wait(
            lock, [&] { return state.stopping || !state.queue.empty(); });
        if (state.stopping && state.queue.empty())
          break;
        request = state.queue.front();
        state.queue.pop_front();
        state.queuedBytes -= request->bytes;
        state.active = request;
      }
      const auto &value = request->value;
      const auto op = value["operation"].get<std::string>();
      const bool webRequest = isWeb(value);
      const auto revision = [&] {
        return webRequest ? web.revision() : engine.revision();
      };
      const auto project = [&] {
        return webRequest ? web.projectId() : engine.projectId();
      };
      Json result;
      Engine::EncodedPayload encodedPayload;
      LoadProgress loadProgress;
      try {
        if (value.contains("expected_revision") &&
            value["expected_revision"] != revision())
          throw Error("stale_revision",
                      "Project revision changed; refresh before retrying");
        if (value.contains("project_id") &&
            !value["project_id"].get<std::string>().empty() &&
            value["project_id"] != project())
          throw Error("stale_project", "Request refers to a different project");
        engine.setCancellationProbe(
            [request] { return request->cancelled.load(); });
        engine.setLoadProgressSink([&](const char *Phase, std::uint64_t Done,
                                       std::uint64_t Total,
                                       const char *Detail) {
          // The loader still checks cancellation at every checkpoint.
          if (!loadProgress.publish(Phase ? Phase : "", Done, Total,
                                    Detail ? Detail : ""))
            return;
          transport.send(response(value, engine.revision(), engine.projectId(),
                                  "progress",
                                  {{"phase", Phase ? Phase : ""},
                                   {"done", Done},
                                   {"total", Total},
                                   {"detail", Detail ? Detail : ""}}));
        });
        const auto input = value.value("payload", Json::object());
        auto reply = webRequest ? Engine::Reply{web.execute(op, input), {}}
                                : engine.executeResponse(op, input);
        encodedPayload = std::move(reply.encoded);
        result = response(value, revision(), project(), "ok",
                          std::move(reply.value));
      } catch (const Error &error) {
        encodedPayload.reset();
        result = failure(value, revision(), project(), error);
      } catch (const std::exception &error) {
        encodedPayload.reset();
        result = failure(value, revision(), project(),
                         Error("engine_error", webRequest ? "Web adapter failed"
                                                          : error.what()));
      }
      engine.setLoadProgressSink({});
      engine.setCancellationProbe({});
      result["analysis_state"] = webRequest          ? web.analysisState()
                                 : engine.analyzed() ? "complete"
                                                     : "not_analyzed";
      {
        std::lock_guard lock(state.mutex);
        if (request->cancelled) {
          // The comparison cooperates; legacy synchronous operations finish.
          // Only the terminal reply reports that computation has stopped.
          result["cancellation_requested"] = true;
          result["calculation_stopped"] = true;
          const bool ComparisonOperation = ComparisonService::cancellable(op);
          const bool LineageOperation = LineageService::cancellable(op);
          const bool ComparisonPartial =
              ComparisonOperation && result.contains("payload") &&
              !result["payload"].value("restore_completed", false) &&
              !result["payload"].value("edit_completed", false) &&
              result["payload"].value("state", std::string()) == "cancelled";
          const bool ComparisonStopped =
              ComparisonPartial ||
              (result.contains("error") &&
               result["error"].value("code", std::string()) == "cancelled");
          result["completed_before_cancellation"] = !ComparisonStopped;
          if (ComparisonOperation)
            result["partial_result_retained"] = ComparisonPartial;
          if (LineageOperation)
            result["partial_result_retained"] = false;
        }
        state.revision = engine.revision();
        state.projectId = engine.projectId();
        state.publishBackground(engine.backgroundState());
        if (!webRequest) {
          result["background"] = state.background;
          result["background_sequence"] =
              std::to_string(state.BackgroundSequence);
        }
        state.webRevision = web.revision();
        state.webProjectId = web.projectId();
        state.pending.erase(value["request_id"].get<std::string>());
        state.active.reset();
      }
      if (!transport.send(std::move(result), encodedPayload.get()))
        break;
    }
  } catch (const std::exception &error) {
    transport.send(
        {{"protocol_major", 1},
         {"type", "fatal"},
         {"error",
          {{"code", "engine_unavailable"}, {"message", error.what()}}}});
  }
  {
    std::lock_guard lock(state.mutex);
    state.finished = true;
  }
  state.changed.notify_all();
}

void cancelQueued(State &state, Transport &transport) {
  for (const auto &item : state.queue) {
    transport.send(response(item->value, state.revisionFor(item->value),
                            state.projectFor(item->value), "cancelled",
                            {{"calculation_stopped", true}}));
    state.pending.erase(item->value["request_id"].get<std::string>());
  }
  state.queue.clear();
  state.queuedBytes = 0;
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::cout << "neverd-worker protocol 1.1\n";
    return 0;
  }
  if (argc != 1) {
    std::cerr << "Usage: neverd-worker [--version]\nThe worker accepts framed "
                 "requests on stdin.\n";
    return 2;
  }
  try {
    Transport transport;
    if (!transport.send(hello()))
      return 1;
    State state;
    std::thread executor([&] { executeLoop(state, transport); });
    std::thread heartbeat([&] {
      std::unique_lock lock(state.mutex);
      while (!state.finished) {
        if (state.changed.wait_for(
                lock,
                std::chrono::seconds(
                    ::neverd::limits::worker::kMainMainHeartbeatSeconds),
                [&] { return state.finished; }))
          break;
        auto message = heartbeatMessage(state);
        lock.unlock();
        transport.send(std::move(message));
        lock.lock();
      }
    });
    int exitCode = 0;
    try {
      while (true) {
        unsigned char header[4];
        if (!Transport::readExact(reinterpret_cast<char *>(header), 4, true))
          break;
        const auto bytes = frameSize(header);
        std::string body(bytes, '\0');
        Transport::readExact(body.data(), body.size(), false);
        Json value = Json::object();
        try {
          value = parseJson(body);
          validateRequest(value);
          const auto op = value["operation"].get<std::string>();
          const auto id = value["request_id"].get<std::string>();
          const auto payload = value.value("payload", Json::object());
          std::unique_lock lock(state.mutex);
          if (state.finished)
            throw Error("engine_unavailable", "Session executor has exited");
          if (state.pending.contains(id))
            throw Error("duplicate_request", "request_id is already pending");
          if (op == "hello") {
            auto result =
                response(value, state.revision, state.projectId, "ok", hello());
            lock.unlock();
            transport.send(std::move(result));
          } else if (op == "cancel") {
            const auto target = stringField(
                payload, "request_id", {},
                ::neverd::limits::worker::
                    kMainMainHelloCancelTargetRequestIdStringFieldMax);
            if (target.empty())
              throw Error("invalid_request",
                          "cancel requires payload.request_id");
            bool accepted = false, stopped = false;
            if (state.active && state.active->value["request_id"] == target) {
              state.active->cancelled = true;
              accepted = true;
            } else {
              for (auto it = state.queue.begin(); it != state.queue.end();
                   ++it) {
                if ((*it)->value["request_id"] != target)
                  continue;
                transport.send(
                    response((*it)->value, state.revisionFor((*it)->value),
                             state.projectFor((*it)->value), "cancelled",
                             {{"calculation_stopped", true}}));
                state.queuedBytes -= (*it)->bytes;
                state.pending.erase(target);
                state.queue.erase(it);
                accepted = stopped = true;
                break;
              }
            }
            transport.send(response(
                value, state.revision, state.projectId, "ok",
                {{"accepted", accepted},
                 {"stopped", stopped},
                 {"state", !accepted ? "not_found"
                           : stopped ? "stopped"
                                     : "pending"},
                 {"requires_restart",
                  accepted && !stopped &&
                      (!state.active || (!ComparisonService::cancellable(
                                             state.active->value["operation"]
                                                 .get<std::string>()) &&
                                         !LineageService::cancellable(
                                             state.active->value["operation"]
                                                 .get<std::string>())))}}));
          } else if (op == "shutdown") {
            state.stopping = true;
            cancelQueued(state, transport);
            transport.send(
                response(value, state.revision, state.projectId, "ok",
                         {{"stopped", !state.active},
                          {"requires_restart", state.active != nullptr}}));
            state.changed.notify_all();
            break;
          } else {
            if (state.queue.size() >= MaxQueueRequests ||
                bytes > MaxQueueBytes - state.queuedBytes)
              throw Error("queue_full",
                          "Worker queue reached its request or byte limit");
            state.queue.push_back(
                std::make_shared<Request>(std::move(value), bytes));
            state.pending.insert(id);
            state.queuedBytes += bytes;
            state.changed.notify_all();
          }
        } catch (const Error &error) {
          std::string revision, project;
          {
            std::lock_guard lock(state.mutex);
            revision = state.revisionFor(value);
            project = state.projectFor(value);
          }
          // Shape-invalid requests may not have string correlation fields.
          Json safe = Json::object();
          if (value.is_object())
            for (const auto *key : {"request_id", "operation"})
              if (value.contains(key) && value[key].is_string() &&
                  value[key].get_ref<const std::string &>().size() <=
                      ::neverd::limits::worker::
                          kMainMainValueKeyGetRefConstStdStringSizeLimit)
                safe[key] = value[key];
          transport.send(failure(safe, revision, project, error));
        }
      }
    } catch (const Error &error) {
      transport.send(
          {{"protocol_major", 1},
           {"type", "fatal"},
           {"error", {{"code", error.code}, {"message", error.what()}}}});
      exitCode = 2;
    }
    {
      std::lock_guard lock(state.mutex);
      state.stopping = true;
      cancelQueued(state, transport);
    }
    state.changed.notify_all();
    executor.join();
    heartbeat.join();
    return exitCode;
  } catch (const std::exception &error) {
    std::cerr << "neverd-worker: " << error.what() << '\n';
    return 1;
  }
}
