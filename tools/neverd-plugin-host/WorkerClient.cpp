//===- WorkerClient.cpp - Headless plugin project bridge
//-------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Frame worker replies and preserve its revision and project checks.
///
//===----------------------------------------------------------------------===//

#include "WorkerClient.h"

#include "../common/transport/ResourceLimits.h"

#include "neverd/plugin/Config.h"

#include <chrono>
#include <iostream>

namespace neverd::plugin::cli {
namespace {
Json failure(const std::string &Message) {
  return {{"status", "error"},
          {"error", {{"code", "worker_unavailable"}, {"message", Message}}}};
}
std::string utf8(const std::filesystem::path &Path) {
  const auto Text = Path.u8string();
  return std::string(Text.begin(), Text.end());
}
} // namespace

WorkerClient::WorkerClient(Executor &Queue)
    : Queue(Queue), Child([&Queue](auto Work) { Queue.post(std::move(Work)); },
                          [this](auto Event) { receive(std::move(Event)); }) {}
WorkerClient::~WorkerClient() {
  Child.stop();
  Child.join();
  Queue.drain();
}

void WorkerClient::receive(ProcessEvent Event) {
  if (Event.Type == ProcessEvent::Diagnostic) {
    std::cerr << Event.Bytes;
    return;
  }
  if (Event.Type == ProcessEvent::Error || Event.Type == ProcessEvent::Exit) {
    Error = Event.Type == ProcessEvent::Error ? Event.Bytes
                                              : "Analysis worker exited";
  } else {
    Input += Event.Bytes;
    try {
      while (Input.size() >= 4 && Error.empty()) {
        uint32_t Size = 0;
        for (int I = 0; I < 4; ++I)
          Size = (Size << 8) | static_cast<unsigned char>(Input[I]);
        if (!Size || Size > ::neverd::limits::transport::kJsonMaxJsonBytes) {
          Error = "Invalid analysis worker frame size";
          break;
        }
        if (Input.size() < size_t(Size) + 4)
          break;
        auto Reply = Json::parse(Input.begin() + 4, Input.begin() + Size + 4,
                                 nullptr, false);
        Input.erase(0, Size + 4);
        if (!Reply.is_object()) {
          Error = "Malformed analysis worker reply";
          break;
        }
        if (Reply.value("type", "") == "hello") {
          if (Reply.value("protocol_major", 0) != 1)
            Error = "Unsupported analysis worker protocol";
          else
            Greeted = true;
          continue;
        }
        if (Reply.value("type", "") == "fatal") {
          Error = Reply.dump();
          break;
        }
        if (Reply.value("status", "") == "progress")
          continue;
        const auto Id = Reply.value("request_id", "");
        auto I = Pending.find(Id);
        if (I != Pending.end()) {
          auto Done = std::move(I->second);
          Pending.erase(I);
          Done(std::move(Reply));
        }
      }
    } catch (const Json::exception &Failure) {
      Error =
          std::string("Invalid analysis worker response: ") + Failure.what();
    }
  }
  if (!Error.empty()) {
    auto Failed = std::move(Pending);
    Pending.clear();
    for (auto &[Id, Done] : Failed)
      Done(failure(Error));
    Child.stop();
  }
}

void WorkerClient::query(const std::string &Operation, const Json &Payload,
                         const Json &Context, std::function<void(Json)> Done) {
  if (!Error.empty()) {
    Done(failure(Error));
    return;
  }
  const auto Id = std::to_string(++NextId);
  Json Request{{"protocol_major", 1},
               {"request_id", Id},
               {"operation", Operation},
               {"payload", Payload}};
  if (Context.contains("project_id"))
    Request["project_id"] = Context["project_id"];
  if (Context.contains("revision"))
    Request["expected_revision"] = Context["revision"];
  const auto Bytes = Request.dump();
  std::string Frame;
  for (int Shift : {24, 16, 8, 0})
    Frame += char((uint32_t(Bytes.size()) >> Shift) & 255);
  if (!Child.write(Frame + Bytes)) {
    Done(failure("Cannot send analysis worker query"));
    return;
  }
  Pending.emplace(Id, std::move(Done));
}

bool WorkerClient::open(const std::filesystem::path &Worker,
                        const std::filesystem::path &File, Json &Context,
                        std::function<bool()> Cancelled,
                        std::string &Diagnostic, bool ReadOnly) {
  if (!Child.start(Worker, {}, Worker.parent_path(), Diagnostic))
    return false;
  const auto Deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(limits::kDefaultActionTimeoutMs);
  const auto Wait = [&](auto Predicate) {
    while (!Predicate() && Error.empty() && !Cancelled() &&
           std::chrono::steady_clock::now() < Deadline)
      Queue.runOne(std::chrono::milliseconds(20));
    return Predicate() && Error.empty() && !Cancelled();
  };
  if (!Wait([&] { return Greeted; })) {
    Diagnostic = Error.empty()
                     ? "Analysis worker startup cancelled or timed out"
                     : Error;
    return false;
  }
  bool Received = false;
  Json Response;
  query("open",
        {{"path", utf8(std::filesystem::absolute(File))},
         {"read_only", ReadOnly},
         {"warm", false},
         {"background_analysis", false}},
        Json::object(), [&](Json Reply) {
          Response = std::move(Reply);
          Received = true;
        });
  if (!Wait([&] { return Received; })) {
    // Remove stack-bound completion before the caller returns.
    Pending.clear();
    Diagnostic = Error.empty()
                     ? "Opening the project was cancelled or timed out"
                     : Error;
    return false;
  }
  if (Response.value("status", "") != "ok") {
    Diagnostic = Response.dump();
    return false;
  }
  Context = {{"loaded", true},
             {"epoch", "1"},
             {"project_id", Response.at("project_id")},
             {"revision", Response.at("revision")},
             {"input_path", utf8(std::filesystem::absolute(File))}};
  const auto Metadata = Response.value("payload", Json::object());
  for (const auto *Key : {"architecture", "format"})
    if (Metadata.contains(Key))
      Context[Key] = Metadata[Key];
  return true;
}
} // namespace neverd::plugin::cli
