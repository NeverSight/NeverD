#include "Engine.h"
#include "Protocol.h"
#include "WebEngine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
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
  bool send(const Json &message) {
    std::string encoded;
    try {
      encoded = frame(message);
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
  bool cancelled = false;
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
  // Engine background work state, published with heartbeats.
  Json background = nullptr;
  std::string webRevision = "0", webProjectId;
  const std::string &revisionFor(const Json &request) const {
    return isWeb(request) ? webRevision : revision;
  }
  const std::string &projectFor(const Json &request) const {
    return isWeb(request) ? webProjectId : projectId;
  }
};

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
                "analysis_snapshot",
                "analysis_restore",
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
                "string_encodings",
                "string_options",
                "string_references",
                "function_level_analysis",
                "background_index"}},
              {"cancellation",
               "queued_requests_stop; "
               "synchronous_engine_requires_completion_or_process_restart"},
              {"storage", "existing_json_sidecars"}};
  // Function edits, data items and operand formats need an engine that keeps
  // them.
  if (Engine::keepsFunctionEdits())
    for (const char *operation : {"function_create", "function_delete"})
      result["capabilities"].push_back(operation);
  if (Engine::keepsDataItems())
    result["capabilities"].push_back("item_define");
  if (Engine::keepsOperandFormats())
    result["capabilities"].push_back("operand_format");
  if (Engine::identifiesFiles())
    result["capabilities"].push_back("identify");
  result["web"] = WebEngine::capabilities();
  return result;
}

/// How long requests must pause before background work starts.
constexpr auto IdleGracePeriod = std::chrono::milliseconds(150);

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
          lock.unlock();
          try {
            engine.idleStep();
          } catch (const std::exception &) {
            // A failed step leaves its error in the published state.
          }
          lock.lock();
          state.background = engine.backgroundState();
        }
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
      try {
        if (value.contains("expected_revision") &&
            value["expected_revision"] != revision())
          throw Error("stale_revision",
                      "Project revision changed; refresh before retrying");
        if (value.contains("project_id") &&
            !value["project_id"].get<std::string>().empty() &&
            value["project_id"] != project())
          throw Error("stale_project", "Request refers to a different project");
        engine.setLoadProgressSink([&](const char *Phase, std::uint64_t Done,
                                       std::uint64_t Total,
                                       const char *Detail) {
          transport.send(response(value, engine.revision(), engine.projectId(),
                                  "progress",
                                  {{"phase", Phase ? Phase : ""},
                                   {"done", Done},
                                   {"total", Total},
                                   {"detail", Detail ? Detail : ""}}));
        });
        const auto input = value.value("payload", Json::object());
        auto payload =
            webRequest ? web.execute(op, input) : engine.execute(op, input);
        result =
            response(value, revision(), project(), "ok", std::move(payload));
      } catch (const Error &error) {
        result = failure(value, revision(), project(), error);
      } catch (const std::exception &error) {
        result = failure(value, revision(), project(),
                         Error("engine_error", webRequest ? "Web adapter failed"
                                                          : error.what()));
      }
      engine.setLoadProgressSink({});
      result["analysis_state"] = webRequest          ? web.analysisState()
                                 : engine.analyzed() ? "complete"
                                                     : "not_analyzed";
      {
        std::lock_guard lock(state.mutex);
        if (request->cancelled) {
          // An acknowledged cancellation cannot stop the synchronous C ABI.
          // Preserve committed mutations and the actual completion status.
          result["cancellation_requested"] = true;
          result["calculation_stopped"] = true;
          result["completed_before_cancellation"] = true;
        }
        state.revision = engine.revision();
        state.projectId = engine.projectId();
        state.background = engine.backgroundState();
        state.webRevision = web.revision();
        state.webProjectId = web.projectId();
        state.pending.erase(value["request_id"].get<std::string>());
        state.active.reset();
      }
      if (!transport.send(result))
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
        if (state.changed.wait_for(lock, std::chrono::seconds(1),
                                   [&] { return state.finished; }))
          break;
        Json message = {
            {"protocol_major", 1},
            {"type", "heartbeat"},
            {"revision", state.revision},
            {"project_id", state.projectId},
            {"web",
             {{"revision", state.webRevision},
              {"project_id", state.webProjectId}}},
            {"active_request_id",
             state.active ? state.active->value["request_id"] : Json(nullptr)},
            {"cancellation_requested", state.active && state.active->cancelled},
            {"background", state.background}};
        lock.unlock();
        transport.send(message);
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
            transport.send(result);
          } else if (op == "cancel") {
            const auto target = stringField(payload, "request_id", {}, 128);
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
            transport.send(
                response(value, state.revision, state.projectId, "ok",
                         {{"accepted", accepted},
                          {"stopped", stopped},
                          {"state", !accepted ? "not_found"
                                    : stopped ? "stopped"
                                              : "pending"},
                          {"requires_restart", accepted && !stopped}}));
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
                std::make_shared<Request>(Request{std::move(value), bytes}));
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
                  value[key].get_ref<const std::string &>().size() <= 128)
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
