//===- ToolsCommand.cpp - Headless tools consumer -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A deterministic consumer protocol that future Agent adapters can also use.
//===----------------------------------------------------------------------===//

#include "ToolsCommand.h"

#include "WorkerClient.h"

#include "neverd/plugin/Artifacts.h"
#include "neverd/plugin/Config.h"
#include "neverd/tools/ProcessTools.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

using namespace neverd::plugin;
using namespace neverd::tools;
namespace {
Json failure(const Diagnostic &Error) {
  return {{"status", "error"},
          {"error", {{"code", Error.Code}, {"message", Error.Message}}}};
}
void send(Json Value) {
  std::cout << Value.dump(-1, ' ', false, Json::error_handler_t::replace)
            << '\n'
            << std::flush;
}
struct Cleanup {
  std::function<void()> Run;
  ~Cleanup() { Run(); }
};
// Read only available bytes so cancellation does not depend on the caller
// closing stdin. The parser enforces the existing transport frame bound before
// allocating an unbounded line. Console reads are cancellable on Windows.
void readConsumer(std::atomic<bool> &Stop,
                  std::function<void(std::string)> Line,
                  std::function<void()> End) {
  std::string Pending;
  bool TooLong = false;
  char Bytes[4096];
  while (!Stop.load()) {
    size_t Count = 0;
#ifdef _WIN32
    const auto Input = GetStdHandle(STD_INPUT_HANDLE);
    const auto Type = GetFileType(Input);
    DWORD Available = sizeof(Bytes), Read = 0;
    if (Type == FILE_TYPE_PIPE) {
      if (!PeekNamedPipe(Input, nullptr, 0, nullptr, &Available, nullptr))
        break;
      if (!Available) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
    } else if (Type == FILE_TYPE_CHAR &&
               WaitForSingleObject(Input, 20) != WAIT_OBJECT_0)
      continue;
    if (Stop.load() ||
        !ReadFile(Input, Bytes, std::min<DWORD>(Available, sizeof(Bytes)),
                  &Read, nullptr) ||
        !Read)
      break;
    Count = Read;
#else
    pollfd Input{STDIN_FILENO, POLLIN, 0};
    const auto Ready = ::poll(&Input, 1, 20);
    if (Ready < 0)
      break;
    if (!Ready)
      continue;
    const auto Read = ::read(STDIN_FILENO, Bytes, sizeof(Bytes));
    if (Read <= 0)
      break;
    Count = size_t(Read);
#endif
    for (size_t I = 0; I < Count; ++I) {
      if (Bytes[I] == '\n') {
        Line(TooLong ? std::string() : std::move(Pending));
        Pending.clear();
        TooLong = false;
      } else if (!TooLong) {
        if (Pending.size() == size_t(limits::kMaxFrameBytes)) {
          Pending.clear();
          TooLong = true;
        } else
          Pending += Bytes[I];
      }
    }
  }
  if (!Stop.load() && (!Pending.empty() || TooLong))
    Line(TooLong ? std::string() : std::move(Pending));
  End();
}
} // namespace
int runTools(int Argc, char **Argv, std::function<bool()> Cancelled) {
  if (Argc < 4)
    throw std::runtime_error("tools requires plugin.json and tools.json");
  std::string Error;
  const auto Plugin = loadManifest(std::filesystem::u8path(Argv[2]), Error);
  if (!Plugin)
    throw std::runtime_error(Error);
  const auto CatalogPath = std::filesystem::u8path(Argv[3]);
  if (std::filesystem::file_size(CatalogPath) > limits::kMaxManifestBytes)
    throw std::runtime_error("Tools catalog exceeds 64 KiB");
  std::ifstream File(CatalogPath, std::ios::binary);
  auto Catalog = Json::parse(File, nullptr, false);
  std::filesystem::path Worker, Input, Output;
  std::string Address, Function;
  bool Writable = false;
  for (int I = 4; I < Argc; ++I) {
    const std::string Option = Argv[I];
    if (Option == "--allow-project-writes") {
      Writable = true;
      continue;
    }
    if (++I >= Argc)
      throw std::runtime_error("Missing option value: " + Option);
    if (Option == "--worker")
      Worker = std::filesystem::absolute(std::filesystem::u8path(Argv[I]));
    else if (Option == "--input")
      Input = std::filesystem::absolute(std::filesystem::u8path(Argv[I]));
    else if (Option == "--output")
      Output = std::filesystem::absolute(std::filesystem::u8path(Argv[I]));
    else if (Option == "--address")
      Address = Argv[I];
    else if (Option == "--function")
      Function = Argv[I];
    else
      throw std::runtime_error("Unknown tools option: " + Option);
  }
  if (Output.empty() || Worker.empty() != Input.empty())
    throw std::runtime_error("tools requires --output <new-directory>; "
                             "--worker and --input pair together");
  std::filesystem::create_directories(Output.parent_path());
  if (!std::filesystem::create_directory(Output))
    throw std::runtime_error("Output directory already exists");
  Executor Queue;
  PluginHost Host([&](auto Work) { Queue.post(std::move(Work)); });
  Actions ActionService(Host);
  std::unique_ptr<cli::WorkerClient> Project;
  Environment Env;
  Env.Context = {{"loaded", false},
                 {"epoch", "1"},
                 {"project_id", ""},
                 {"revision", "0"},
                 {"profile", "default"}};
  if (!Worker.empty()) {
    Project = std::make_unique<cli::WorkerClient>(Queue);
    if (!Project->open(Worker, Input, Env.Context, Cancelled, Error, !Writable))
      throw std::runtime_error(Error);
    Env.Context["profile"] = "default";
    for (const auto &Service : queryServices())
      Env.Services.insert(Service);
    if (Writable)
      Env.Services.insert("command:rename");
  }
  if (!Address.empty())
    Env.Context["address"] = Address;
  if (!Function.empty())
    Env.Context["function_address"] = Function;
  Diagnostic Why;
  auto ProjectOwner =
      Host.createOwner(Host.root(), OwnerKind::Project, "tools-project", Why);
  if (!ProjectOwner || !Host.activate(*ProjectOwner, Why))
    throw std::runtime_error(Why.Message);
  Env.Project = *ProjectOwner;
  Services::Handler Query = [&](const ProcessServiceRequest &Request,
                                auto Done) {
    if (!Project)
      Done(failure({"service_unavailable", "No project is open"}));
    else
      Project->query(Request.Operation, Request.Payload, Request.Context,
                     std::move(Done));
  };
  HostServices Capabilities(
      Host, [&](auto Work) { Queue.post(std::move(Work)); }, Query);
  if (!Capabilities.bind(*ProjectOwner, Env.Context, bool(Project), Why))
    throw std::runtime_error(Why.Message);
  ProcessActions Processes(
      Host, ActionService, Queue, Output, std::move(Query),
      [&](const ProcessServiceRequest &Request, auto Done) {
        if (!Project || !Writable || Request.Operation != "rename") {
          Done(failure({"service_unavailable",
                        "Command is not enabled by this consumer"}));
          return;
        }
        Project->query(
            Request.Operation, Request.Payload, Request.Context,
            [&, Done = std::move(Done)](Json Reply) {
              if (Reply.value("error", Json::object()).value("code", "") ==
                  "worker_unavailable") {
                Reply["status"] = "unknown";
                Reply["error"]["code"] = "outcome_unknown";
              }
              if (Reply.contains("revision") &&
                  Reply.value("project_id", "") ==
                      Env.Context["project_id"].get<std::string>())
                Env.Context["revision"] = Reply["revision"];
              Capabilities.setContext(*ProjectOwner, Env.Context);
              Done(std::move(Reply));
            });
      },
      {}, &Capabilities);
  bool Connected = true;
  ToolRegistry Tools(
      Host, ActionService, [&] { return Env; }, {},
      [&](const auto &Op) {
        if (Connected)
          send({{"event", "operation"}, {"value", describeOperation(Op)}});
      });
  PluginCatalog Installations(Host, Processes, *ProjectOwner);
  // Close/drain while every query/command adapter and operation ledger exists.
  Cleanup Drain{[&] {
    Connected = false;
    Tools.setObserver({});
    Processes.shutdown();
    Project.reset();
    while (!Processes.idle()) {
      Queue.runOne(std::chrono::milliseconds(20));
      Processes.poll();
    }
    Host.close(Host.root());
    Queue.drain();
  }};
  if (!installProcessTools(Installations, Tools, *Plugin, Catalog, Why))
    throw std::runtime_error(Why.Message);
  send({{"event", "ready"},
        {"protocol", "neverd.tools-consumer.v1"},
        {"runtime", Tools.runtimeId()},
        {"context", Env.Context}});
  std::map<uint64_t, ResolvedTools> Sets;
  bool End = false;
  const auto Handle = [&](const Json &Request) {
    Json Reply;
    try {
      if (Request.value("runtime", "") != Tools.runtimeId()) {
        Reply = failure(
            {"runtime_changed", "Consumer request belongs to another runtime"});
        Reply["id"] = Request.value("id", Json());
        if (Connected)
          send(Reply);
        return;
      }
      const auto Op = Request.at("op").get<std::string>();
      if (Op == "plugins") {
        Json Entries = Json::array();
        for (const auto &Snapshot : Installations.snapshots())
          Entries.push_back(describeInstallation(Snapshot));
        Reply = {{"status", "ok"}, {"plugins", std::move(Entries)}};
      } else if (Op == "resolve") {
        auto Set = Tools.resolve(Request.at("session"), Request.at("step"),
                                 Request.value("user", Json::object()),
                                 Request.value("profile", Json::object()), Why);
        if (!Set)
          Reply = failure(Why);
        else {
          Json Entries = Json::array();
          for (const auto &Tool : Set->tools())
            Entries.push_back(describeTool(Tool));
          Reply = {{"status", "ok"},
                   {"set", Set->id()},
                   {"tools", Entries},
                   {"context", Env.Context}};
          Sets.emplace(Set->id(), std::move(*Set));
        }
      } else if (Op == "call") {
        const auto SetId = Request.at("set").get<uint64_t>();
        if (!Sets.contains(SetId))
          Reply = failure({"set_released", "Unknown resolved set"});
        else {
          Registration Token;
          for (const auto &Info : Sets.at(SetId).tools())
            if (Info.Token.id() == Request.at("registration").get<uint64_t>())
              Token = Info.Token;
          const auto Id =
              Tools.call(Sets.at(SetId), Token, Request.at("call_id"),
                         Request.value("parameters", Json::object()),
                         Request.value("parent_operation", ""), Why);
          Reply = Id ? Json{{"status", "ok"},
                            {"value", describeOperation(*Tools.operation(*Id))}}
                     : failure(Why);
        }
      } else if (Op == "inspect") {
        const auto Info = Tools.operation(Request.at("operation"));
        Reply =
            Info ? Json{{"status", "ok"}, {"value", describeOperation(*Info)}}
                 : failure({"outcome_unknown",
                            "Operation is not retained in this runtime"});
      } else if (Op == "cancel") {
        const bool Found = Tools.cancel(Request.at("operation"));
        Reply = Found ? Json{{"status", "ok"}}
                      : failure({"outcome_unknown", "Unknown operation"});
      } else if (Op == "release") {
        const auto Id = Request.at("set").get<uint64_t>();
        if (auto I = Sets.find(Id); I != Sets.end()) {
          I->second.release();
          Sets.erase(I);
        }
        Reply = {{"status", "ok"}};
      } else
        Reply = failure(
            {"unsupported_operation",
             "Expected plugins, resolve, call, inspect, cancel or release"});
    } catch (const std::exception &Exception) {
      Reply = failure({"invalid_request", Exception.what()});
    }
    Reply["id"] = Request.is_object() ? Request.value("id", Json()) : Json();
    if (Connected)
      send(std::move(Reply));
  };
  // stdin is solely a consumer transport. All state mutations are queued onto
  // the constructing sequence. EOF detaches the observer and drains receipts.
  std::atomic<bool> StopReader = false;
  std::thread Reader([&] {
    readConsumer(
        StopReader,
        [&](std::string Line) {
          auto Request = Json::parse(Line, nullptr, false);
          Queue.post([&, Request = std::move(Request)] { Handle(Request); });
        },
        [&] {
          Queue.post([&] {
            End = true;
            Connected = false;
            Tools.setObserver({});
            Processes.shutdown();
          });
        });
  });
  Cleanup StopInput{[&] {
    StopReader.store(true);
#ifdef _WIN32
    CancelSynchronousIo(Reader.native_handle());
#endif
    Reader.join();
    Queue.drain();
  }};
  while (!End || !Processes.idle()) {
    Queue.runOne(std::chrono::milliseconds(20));
    if (Cancelled()) {
      StopReader.store(true);
#ifdef _WIN32
      CancelSynchronousIo(Reader.native_handle());
#endif
      Processes.shutdown();
    }
    Processes.poll();
  }
  return Cancelled() ? 130 : 0;
}
