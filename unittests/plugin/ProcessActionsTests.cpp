//===- ProcessActionsTests.cpp - Pending command ownership ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Real external providers with independently controlled service completion.
//===----------------------------------------------------------------------===//

#include "neverd/plugin/ProcessActions.h"
#include "neverd/tools/ToolRegistry.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace neverd::plugin;
using namespace neverd::tools;
namespace {
void check(bool Condition, const char *Why) {
  if (!Condition)
    throw std::runtime_error(Why);
}
} // namespace
int main(int Argc, char **Argv) {
  try {
    if (Argc != 4)
      throw std::runtime_error("mode python fixture required");
    const std::string Mode = Argv[1];
    Executor Queue;
    PluginHost Host([&](auto Work) { Queue.post(std::move(Work)); });
    Actions ActionService(Host);
    Diagnostic Error;
    auto Project =
        *Host.createOwner(Host.root(), OwnerKind::Project, "project", Error);
    Host.activate(Project, Error);
    auto Component =
        *Host.createOwner(Project, OwnerKind::Component, "sample", Error);
    Host.activate(Component, Error);
    Environment Env;
    Env.Project = Project;
    Env.Context = {
        {"loaded", true}, {"project_id", "project"}, {"revision", "0"}};
    ToolRegistry Tools(Host, ActionService, [&] { return Env; });
    std::random_device Random;
    const auto Output = std::filesystem::temp_directory_path() /
                        ("neverd-tools-command-" + std::to_string(Random()));
    std::filesystem::create_directory(Output);
    std::function<void(Json)> Pending;
    std::vector<Json> Submitted;
    ProcessActionEvents Events;
    Events.Changed = [&](const ProcessActionSnapshot &Snapshot) {
      if (Mode == "starting_close" &&
          Snapshot.Task.Phase == TaskPhase::Starting)
        Host.close(Component);
    };
    auto Runtime = std::make_unique<ProcessActions>(
        Host, ActionService, Queue, Output, ProcessActions::Service{},
        [&](const ProcessServiceRequest &Request, auto Done) {
          Submitted.push_back(Request.Context);
          Pending = std::move(Done);
        },
        std::move(Events));
    Manifest M;
    M.Id = "command-fixture";
    M.Version = "1";
    M.Path = std::filesystem::absolute(std::filesystem::u8path(Argv[3]));
    M.Program = std::filesystem::u8path(Argv[2]);
    M.Arguments = {Argv[3]};
    const auto Action = Mode == "close" ? "once" : "twice";
    M.Actions = Json::array(
        {{{"id", Action}, {"title", "Commands"}, {"context", "project"}}});
    ActionDefinition Def;
    Def.Id = Action;
    Def.OutputSchema = {{"type", "integer"}};
    Def.Effect = ActionEffect::ProjectWrite;
    auto A = Runtime->add(Component, M, Def, {true, {"rename"}}, Error);
    check(A.has_value(), "register external action");
    auto T = Tools.add({"command", *A, {}}, Error);
    check(T.has_value(), "register external tool");
    auto Set = Tools.resolve("consumer", "step", Json::object(), Json::object(),
                             Error);
    auto Op = Tools.call(*Set, *T, "call", Json::object(), "", Error);
    check(Op.has_value(), "invoke external tool");
    const auto Deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    const auto Wait = [&](auto Predicate) {
      while (!Predicate() && std::chrono::steady_clock::now() < Deadline) {
        Queue.runOne(std::chrono::milliseconds(10));
        if (Runtime)
          Runtime->poll();
      }
      check(Predicate(), "external fixture timed out");
    };
    bool Closed = false;
    if (Mode == "starting_close") {
      Runtime->poll();
      check(Tools.operation(*Op)->Result.Phase == ActionPhase::Cancelled &&
                Submitted.empty(),
            "retirement during starting launched a provider");
      Runtime->shutdown([&] { Closed = true; });
      Wait([&] { return Closed; });
    } else if (Mode == "queued_close") {
      Tools.setObserver([&](const auto &Operation) {
        if (terminal(Operation.Result.Phase))
          Runtime.reset();
      });
      Runtime->shutdown([&] { Closed = true; });
      Queue.drain();
      check(!Runtime && Closed && Submitted.empty(),
            "queued terminal observer did not safely destroy the facade");
      check(Tools.operation(*Op)->Result.Phase == ActionPhase::Cancelled,
            "queued cancellation did not complete");
    } else {
      Wait([&] { return bool(Pending); });
      check(Submitted.at(0).at("revision") == "0", "original commit cursor");
      if (Mode == "close") {
        Runtime->shutdown([&] { Closed = true; });
        Set->release();
        Tools.setObserver({});
        Runtime.reset();
        Queue.drain();
        check(!Closed, "close completed before accepted command receipt");
        auto Done = std::move(Pending);
        Done({{"status", "ok"},
              {"project_id", "project"},
              {"revision", "1"},
              {"payload", {{"saved", true}}}});
        // A transport retry cannot deliver the receipt twice.
        Done({{"status", "ok"}, {"project_id", "project"}, {"revision", "1"}});
        Wait([&] { return Closed; });
        const auto Result = Tools.operation(*Op)->Result;
        check(Result.Phase == ActionPhase::Cancelled &&
                  Result.Receipts.size() == 1 &&
                  Result.Receipts[0]["reply"]["payload"]["saved"] == true,
              "close lost an accepted write receipt");
      } else {
        auto Done = std::move(Pending);
        Pending = {};
        Done({{"status", "ok"},
              {"project_id", "project"},
              {"revision", "1"},
              {"payload", {{"saved", true}}}});
        Wait([&] { return bool(Pending); });
        check(Submitted.at(1).at("revision") == "1",
              "second command did not advance its own cursor");
        check(Tools.operation(*Op)->Context["revision"] == "0",
              "command advanced analysis input");
        Done = std::move(Pending);
        if (Mode == "conflict")
          Done({{"status", "conflict"},
                {"project_id", "project"},
                {"revision", "2"},
                {"error",
                 {{"code", "revision_mismatch"},
                  {"message", "External edit"}}}});
        else
          Done({{"status", "ok"},
                {"project_id", "project"},
                {"revision", "2"},
                {"payload", {{"saved", true}}}});
        Wait([&] { return terminal(Tools.operation(*Op)->Result.Phase); });
        const auto Result = Tools.operation(*Op)->Result;
        check(Result.Receipts.size() == 2, "command receipts missing");
        check(Result.Phase == (Mode == "conflict" ? ActionPhase::Failed
                                                  : ActionPhase::Succeeded),
              "wrong sequential command outcome");
        Runtime->shutdown([&] { Closed = true; });
        Wait([&] { return Closed; });
      }
    }
    Host.close(Host.root());
    Queue.drain();
    // Only this test's freshly created directory can be removed.
    const auto Root = std::filesystem::weakly_canonical(
        std::filesystem::temp_directory_path());
    if (std::filesystem::weakly_canonical(Output).parent_path() == Root)
      std::filesystem::remove_all(Output);
    std::cout << Mode << " command receipt passed\n";
    return 0;
  } catch (const std::exception &Error) {
    std::cerr << Error.what() << '\n';
    return 1;
  }
}
