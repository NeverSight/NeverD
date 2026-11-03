//===- Main.cpp - Qt-independent process plugin CLI -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The same manifest, parameter resolver, task controller and provider runtime
/// as the GUI, driven by a headless control queue.
///
//===----------------------------------------------------------------------===//

#include "CatalogCommand.h"
#include "ToolsCommand.h"
#include "WorkerClient.h"

#include "neverd/plugin/Artifacts.h"
#include "neverd/plugin/Config.h"
#include "neverd/plugin/Manifest.h"
#include "neverd/plugin/PluginCatalog.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace neverd::plugin;
namespace {
std::atomic<bool> Cancelled = false;
struct Cleanup {
  std::function<void()> Run;
  ~Cleanup() { Run(); }
};
static_assert(std::atomic<bool>::is_always_lock_free);
void interrupt(int) { Cancelled.store(true, std::memory_order_relaxed); }
#ifdef _WIN32
BOOL WINAPI console(DWORD Event) {
  if (Event != CTRL_C_EVENT && Event != CTRL_BREAK_EVENT)
    return FALSE;
  Cancelled.store(true, std::memory_order_relaxed);
  return TRUE;
}
#endif
std::string utf8(const std::filesystem::path &Path) {
  const auto Text = Path.u8string();
  return std::string(Text.begin(), Text.end());
}
Json readConfig(const std::string &Path) {
  const auto FilePath = std::filesystem::u8path(Path);
  if (std::filesystem::file_size(FilePath) > limits::kMaxManifestBytes)
    throw std::runtime_error("Configuration file exceeds 64 KiB");
  std::ifstream File(FilePath, std::ios::binary);
  const std::string Bytes{std::istreambuf_iterator<char>(File), {}};
  auto Result = parseJson(Bytes);
  if (!Result.is_object())
    throw std::runtime_error("Configuration must be a JSON object");
  return Result;
}
int run(int Argc, char **Argv) {
  if (Argc < 3 || std::string(Argv[1]) == "--help") {
    std::cout
        << "neverd-plugin-host list <plugin.json>\n"
           "neverd-plugin-host run <plugin.json> <action> [options]\n"
           "neverd-plugin-host plugins <catalog.json> "
           "<list|explain|import|enable|disable|remove> [target]\n"
           "neverd-plugin-host tools <plugin.json> <tools.json> --output "
           "<new-directory> [options]\n"
           "  tools: JSON-lines resolve/call/inspect/cancel/release on stdin\n"
           "  --allow-project-writes   tools only: enable declared rename "
           "commands\n"
           "  --parameters <json-file>  Explicit action parameter overrides\n"
           "  --user-config <json-file> User action parameter preferences\n"
           "  --profile <json-file>     Project action parameter overrides\n"
           "  --output <new-directory>  Task result directory\n"
           "  --worker <exe> --input <binary>  Existing analysis worker "
           "bridge\n"
           "  --address <0x...> --function <0x...>  Captured selection\n"
           "Results and receipts use JSON; progress and diagnostics use "
           "stderr.\n";
    return Argc >= 2 && std::string(Argv[1]) == "--help" ? 0 : 2;
  }
  const std::string Command = Argv[1];
  if (Command == "plugins")
    return runPluginCatalog(Argc, Argv);
  if (Command == "tools")
    return runTools(Argc, Argv, [] { return Cancelled.load(); });
  if (Command != "list" && Command != "run")
    throw std::runtime_error("Expected list or run");
  std::string Error;
  const auto M = loadManifest(std::filesystem::u8path(Argv[2]), Error);
  if (!M)
    throw std::runtime_error(Error);
  if (Command == "list") {
    if (Argc != 3)
      throw std::runtime_error("list takes one manifest path");
    std::cout << Json{{"plugin", M->Id},       {"version", M->Version},
                      {"name", M->Name},       {"actions", M->Actions},
                      {"queries", M->Queries}, {"protocol", 1}}
                     .dump(2)
              << '\n';
    return 0;
  }
  if (Argc < 4)
    throw std::runtime_error("An action ID is required");
  const std::string ActionId = Argv[3];
  Json Action;
  for (const auto &Item : M->Actions)
    if (Item["id"] == ActionId)
      Action = Item;
  if (Action.is_null())
    throw std::runtime_error("Unknown action: " + ActionId);
  Json User = Json::object(), Profile = Json::object(),
       Parameters = Json::object();
  std::string Address, Function;
  std::filesystem::path Output, Worker, Input;
  for (int I = 4; I < Argc; ++I) {
    const std::string Option = Argv[I];
    if (++I >= Argc)
      throw std::runtime_error("Missing value for " + Option);
    if (Option == "--parameters")
      Parameters = readConfig(Argv[I]);
    else if (Option == "--user-config")
      User = readConfig(Argv[I]);
    else if (Option == "--profile")
      Profile = readConfig(Argv[I]);
    else if (Option == "--output")
      Output = std::filesystem::u8path(Argv[I]);
    else if (Option == "--worker")
      Worker = std::filesystem::absolute(std::filesystem::u8path(Argv[I]));
    else if (Option == "--input")
      Input = std::filesystem::absolute(std::filesystem::u8path(Argv[I]));
    else if (Option == "--address")
      Address = Argv[I];
    else if (Option == "--function")
      Function = Argv[I];
    else
      throw std::runtime_error("Unknown option: " + Option);
  }
  const auto Effective =
      resolveParameters(Action.value("parameters", Json::array()), User,
                        Profile, Parameters, Error);
  if (!Effective)
    throw std::runtime_error(Error);
  if (Worker.empty() != Input.empty())
    throw std::runtime_error("--worker and --input are required together");
  Executor Queue;
  const auto Post = [&](auto Work) { Queue.post(std::move(Work)); };
  PluginHost Host(Post);
  std::unique_ptr<cli::WorkerClient> Project;
  Json Context{
      {"loaded", false}, {"epoch", "1"}, {"project_id", ""}, {"revision", "0"}};
  if (!Worker.empty()) {
    Project = std::make_unique<cli::WorkerClient>(Queue);
    if (!Project->open(
            Worker, Input, Context, [] { return Cancelled.load(); }, Error))
      throw std::runtime_error(Error);
  }
  if (!Address.empty())
    Context["address"] = Address;
  if (!Function.empty())
    Context["function_address"] = Function;
  std::random_device Random;
  std::ostringstream IdStream;
  IdStream << std::hex << Random() << Random() << Random() << Random();
  if (Output.empty())
    Output = std::filesystem::current_path() / "neverd-plugin-results" /
             IdStream.str();
  Output = std::filesystem::absolute(Output).lexically_normal();
  if (Output.filename().empty())
    Output = Output.parent_path();
  if (std::filesystem::exists(Output))
    throw std::runtime_error("Output directory already exists");
  std::filesystem::create_directories(Output.parent_path());
  const auto Id = utf8(Output.filename());
  Actions ActionService(Host);
  ProcessActionSnapshot Outcome;
  ProcessActionEvents Events;
  Events.Changed = [&](const auto &Snapshot) { Outcome = Snapshot; };
  Events.Log = [](const auto &, const auto &Text, bool) {
    std::cerr << Text << '\n';
  };
  Services::Handler Query = [&](const ProcessServiceRequest &Request,
                                auto Done) {
    if (Project)
      Project->query(Request.Operation, Request.Payload, Request.Context,
                     std::move(Done));
    else
      Done({{"status", "unsupported"},
            {"error",
             {{"code", "service_unavailable"},
              {"message", "No project is open"}}}});
  };
  Diagnostic Why;
  const auto ProjectOwner = Host.createOwner(Host.root(), OwnerKind::Project,
                                             Context["project_id"], Why);
  if (!ProjectOwner || !Host.activate(*ProjectOwner, Why))
    throw std::runtime_error(Why.Message);
  HostServices Capabilities(Host, Post, Query);
  if (!Capabilities.bind(*ProjectOwner, Context, bool(Project), Why))
    throw std::runtime_error(Why.Message);
  ProcessActions Processes(Host, ActionService, Queue, Output.parent_path(),
                           std::move(Query), {}, std::move(Events),
                           &Capabilities);
  PluginCatalog Catalog(Host, Processes, *ProjectOwner);
  if (!Catalog.install(*M, Why))
    throw std::runtime_error(Why.Message);
  const auto Binding = Catalog.action(M->Id, ActionId);
  auto Call = ActionService.invoke(
      *Binding, {Id, Context, *Effective, {}},
      [](const auto &Snapshot) {
        if (!Snapshot.Progress.empty())
          std::cerr << Snapshot.Progress.dump() << '\n';
      },
      Why);
  if (!Call)
    throw std::runtime_error(Why.Message);
  Cleanup Drain{[&] {
    Processes.shutdown();
    Project.reset();
    Host.close(Host.root());
    Queue.drain();
    Processes.poll();
  }};
  while (!terminal(Call->snapshot().Phase)) {
    Queue.runOne(std::chrono::milliseconds(20));
    if (Cancelled.load())
      Call->cancel();
    Processes.poll();
  }
  const auto &Result = Call->snapshot();
  const auto State = Outcome.Task.Id.empty()
                         ? actionPhaseName(Result.Phase)
                         : taskPhaseName(Outcome.Task.Phase);
  Json Record{
      {"schema", "neverd.gui-job.v1"},
      {"task_id", Id},
      {"plugin", M->Id},
      {"plugin_version", M->Version},
      {"registration", Binding->id()},
      {"action", ActionId},
      {"context", Context},
      {"parameters", *Effective},
      {"state", State},
      {"diagnostic", Result.Error.Message.empty() ? Outcome.Task.Diagnostic
                                                  : Result.Error.Message},
      {"elapsed_ms", Outcome.ElapsedMs},
      {"stderr", Outcome.Stderr},
      {"result", Result.Phase == ActionPhase::Succeeded ? Outcome.Result
                                                        : Json::object()}};
  if (!Outcome.Receipt.empty())
    Record = Outcome.Receipt;
  std::cout << Record.dump(2, ' ', false, Json::error_handler_t::replace)
            << '\n';
  std::cerr << "Receipt: " << utf8(Output / "receipt.json") << '\n';
  return Result.Phase == ActionPhase::Succeeded   ? 0
         : Result.Phase == ActionPhase::Cancelled ? 130
                                                  : 1;
}
} // namespace

int mainUtf8(int Argc, char **Argv) {
  std::signal(SIGINT, interrupt);
  std::signal(SIGTERM, interrupt);
#ifdef _WIN32
  SetConsoleCtrlHandler(console, TRUE);
#endif
  try {
    return run(Argc, Argv);
  } catch (const std::exception &Error) {
    std::cerr << Error.what() << '\n';
    // Messages can carry argument or OS path bytes that are not UTF-8.
    std::cout << Json{{"status", "error"},
                      {"code", "plugin_host_error"},
                      {"message", Error.what()}}
                     .dump(-1, ' ', false, Json::error_handler_t::replace)
              << '\n';
    return Cancelled.load() ? 130 : 2;
  }
}

#ifdef _WIN32
int wmain(int Argc, wchar_t **Argv) {
  std::vector<std::string> Words;
  for (int I = 0; I < Argc; ++I) {
    const auto Size = WideCharToMultiByte(CP_UTF8, 0, Argv[I], -1, nullptr, 0,
                                          nullptr, nullptr);
    std::string Word(Size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, Argv[I], -1, Word.data(), Size, nullptr,
                        nullptr);
    Word.pop_back();
    Words.push_back(std::move(Word));
  }
  std::vector<char *> Arguments;
  for (auto &Word : Words)
    Arguments.push_back(Word.data());
  return mainUtf8(Argc, Arguments.data());
}
#else
int main(int Argc, char **Argv) { return mainUtf8(Argc, Argv); }
#endif
