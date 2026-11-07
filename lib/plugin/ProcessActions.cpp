//===- ProcessActions.cpp - External providers as NPH actions
//--------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep accepted service replies alive independently of the provider process.
//===----------------------------------------------------------------------===//

#include "neverd/plugin/ProcessActions.h"

#include "neverd/plugin/Artifacts.h"
#include "neverd/plugin/Config.h"
#include "neverd/plugin/Process.h"
#include "neverd/plugin/Protocol.h"
#include "neverd/plugin/Schema.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <utility>

namespace neverd::plugin {
namespace {
std::string utf8(const std::filesystem::path &Path) {
  const auto S = Path.u8string();
  return {S.begin(), S.end()};
}
} // namespace
struct ProcessActions::Impl : std::enable_shared_from_this<Impl> {
  struct Job {
    Manifest Plugin;
    Json Action;
    ActionDefinition Definition;
    Json CommitContext;
    ProtocolExtensions Extensions;
    Registration Binding;
    std::shared_ptr<ActionCall> Call;
    std::filesystem::path Directory;
    std::unique_ptr<Process> Child;
    std::unique_ptr<Protocol> Wire;
    std::shared_ptr<HostServiceSession> Capabilities;
    size_t PendingCommands = 0;
    bool Delivered = false;
    std::string Stderr;
    Json Receipt = Json::object();
    std::chrono::steady_clock::time_point Started;
    std::chrono::steady_clock::time_point Deadline;
  };
  PluginHost &Host;
  Actions &ActionService;
  Executor &Queue;
  std::filesystem::path Output;
  Service Query, Command;
  ProcessActionEvents Events;
  HostServices *Capabilities;
  Tasks Controller;
  std::map<std::string, std::shared_ptr<Job>> Jobs;
  bool Closing = false;
  bool Polling = false;
  std::vector<PluginHost::Work> CloseWaiters;
  Impl(PluginHost &Host, Actions &Actions, Executor &Queue,
       std::filesystem::path Output, Service Query, Service Command,
       ProcessActionEvents Events, HostServices *Capabilities)
      : Host(Host), ActionService(Actions), Queue(Queue),
        Output(std::move(Output)), Query(std::move(Query)),
        Command(std::move(Command)), Events(std::move(Events)),
        Capabilities(Capabilities),
        Controller(
            Host,
            [this](const auto &Id) {
              if (auto I = Jobs.find(Id); I != Jobs.end() && I->second->Child)
                I->second->Child->stop();
            },
            [this](const TaskSnapshot &Task) {
              if (!terminal(Task.Phase))
                publish(Task);
            }) {}
  void publish(const TaskSnapshot &Task) {
    const auto I = Jobs.find(Task.Id);
    if (I == Jobs.end())
      return;
    const auto &J = I->second;
    if (Capabilities)
      if (const auto Scope = Controller.owner(Task.Id))
        Capabilities->publish(*Scope, "task.changed",
                              {{"task_id", Task.Id},
                               {"state", taskPhaseName(Task.Phase)},
                               {"plugin", J->Plugin.Id},
                               {"action", J->Definition.Id},
                               {"progress", J->Call->snapshot().Progress},
                               {"input_context", J->Call->request().Context}});
    if (!Events.Changed)
      return;
    ProcessActionSnapshot Value;
    Value.Task = Task;
    Value.Binding = J->Binding;
    Value.Directory = J->Directory;
    Value.Stderr = J->Stderr;
    Value.Receipt = J->Receipt;
    Value.ElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - J->Started)
                          .count();
    if (Task.Phase == TaskPhase::Succeeded && J->Wire)
      Value.Result = J->Wire->result();
    Events.Changed(Value);
  }
  void start(const std::string &Id, const std::shared_ptr<Job> &J) {
    const auto Weak = weak_from_this();
    const auto WeakJob = std::weak_ptr<Job>(J);
    ProtocolCallbacks Callbacks;
    Callbacks.Send = [WeakJob](auto Bytes) {
      const auto Job = WeakJob.lock();
      return Job && Job->Child->write(std::move(Bytes));
    };
    Callbacks.Hello = [WeakJob] {
      if (auto Job = WeakJob.lock())
        Job->Deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(Job->Action.value(
                            "timeout_ms", limits::kDefaultActionTimeoutMs));
    };
    Callbacks.Progress = [Weak, WeakJob, Id](const auto &Progress) {
      if (auto Job = WeakJob.lock()) {
        Job->Call->progress(Progress);
        if (auto S = Weak.lock())
          S->publish(*S->Controller.snapshot(Id));
      }
    };
    Callbacks.Log = [Weak, Id](const auto &Text) {
      if (const auto S = Weak.lock(); S && S->Events.Log)
        S->Events.Log(Id, Text, false);
    };
    Callbacks.Failed = [Weak, Id](const auto &Error) {
      if (auto S = Weak.lock())
        S->Controller.stop(Id, TaskPhase::Failed, Error);
    };
    Callbacks.Query = [Weak, WeakJob](const auto &Request, const auto &Op,
                                      const auto &Payload) {
      if (auto S = Weak.lock())
        if (auto J = WeakJob.lock()) {
          auto Done = [WeakJob, Request](Json Reply) {
            if (auto Job = WeakJob.lock())
              Job->Wire->answer(Request, Reply);
          };
          if (J->Capabilities)
            J->Capabilities->query({J->Call->request().Operation, Request, Op,
                                    J->Call->request().Context, Payload},
                                   std::move(Done));
          else if (S->Query)
            S->Query({J->Call->request().Operation, Request, Op,
                      J->Call->request().Context, Payload},
                     std::move(Done));
          else
            Done({{"status", "unsupported"},
                  {"error", {{"code", "service_unavailable"}}}});
        }
    };
    Callbacks.Host = [WeakJob](const auto &Request, const auto &Op,
                               const auto &Payload) {
      if (auto J = WeakJob.lock())
        J->Capabilities->request(Request, Op, Payload,
                                 [WeakJob, Request](Json Reply) {
                                   if (auto J = WeakJob.lock())
                                     J->Wire->answer(Request, Reply);
                                 });
    };
    Callbacks.Command = [Weak, WeakJob](const auto &Request, const auto &Op,
                                        const auto &Payload) {
      const auto S = Weak.lock();
      const auto J = WeakJob.lock();
      if (!S || !J)
        return;
      ++J->PendingCommands;
      // This callback owns the job until its accepted command receives a reply.
      // Killing the provider does not cancel the existing project command.
      const auto SubmittedContext = J->CommitContext;
      const auto Completed = std::make_shared<bool>(false);
      auto Done = [S, J, Request, Op, SubmittedContext, Completed](Json Reply) {
        if (std::exchange(*Completed, true))
          return;
        if (Reply.value("status", "") == "ok" && Reply.contains("revision") &&
            Reply.value("project_id", Json()) ==
                SubmittedContext.value("project_id", Json()))
          J->CommitContext["revision"] = Reply["revision"];
        J->Call->receipt(
            {{"id", Request}, {"operation", Op}, {"reply", Reply}});
        --J->PendingCommands;
        J->Wire->answer(Request, Reply);
        S->Queue.post([S] { S->poll(); });
      };
      if (S->Command)
        S->Command({J->Call->request().Operation, Request, Op, SubmittedContext,
                    Payload},
                   std::move(Done));
      else
        Done({{"status", "unsupported"},
              {"error", {{"code", "service_unavailable"}}}});
    };
    const auto &Request = J->Call->request();
    Json Start{{"type", "start"},
               {"task_id", Id},
               {"action", J->Definition.Id},
               {"context", Request.Context},
               {"parameters", Request.Parameters},
               {"output_dir", utf8(J->Directory)}};
    if (J->Extensions.StructuredData) {
      Start["continuation"] = Request.Continuation;
      Start["extensions"] = {"structured_data", "commands"};
    }
    if (J->Capabilities) {
      if (!Start.contains("extensions"))
        Start["extensions"] = Json::array();
      Start["extensions"].push_back("host_services");
      Start["extensions"].push_back("host_events");
    }
    J->Wire = std::make_unique<Protocol>(Controller, std::move(Start),
                                         J->Plugin.Queries,
                                         std::move(Callbacks), J->Extensions);
    J->Child = std::make_unique<Process>(
        [this](auto Work) { Queue.post(std::move(Work)); },
        [Weak, WeakJob, Id](ProcessEvent Event) {
          const auto S = Weak.lock();
          const auto J = WeakJob.lock();
          if (!S || !J)
            return;
          if (Event.Type == ProcessEvent::Output)
            J->Wire->receive(Event.Bytes);
          else if (Event.Type == ProcessEvent::Diagnostic) {
            if (J->Wire->diagnostic(Event.Bytes)) {
              J->Stderr += Event.Bytes;
              if (S->Events.Log)
                S->Events.Log(Id, Event.Bytes, true);
            }
          } else if (Event.Type == ProcessEvent::Error)
            S->Controller.stop(Id, TaskPhase::Failed, Event.Bytes);
          else if (Event.Type == ProcessEvent::Exit) {
            auto Error = J->Wire->exitError();
            const auto &Result = J->Wire->result();
            if (Error.empty())
              verifyArtifacts(J->Directory, Result, Error);
            if (Error.empty() && J->Extensions.StructuredData)
              validateValue(J->Definition.OutputSchema,
                            Result.value("data", Json()), Error);
            if (Error.empty() && Event.ExitCode != 0)
              Error =
                  "Provider exited with code " + std::to_string(Event.ExitCode);
            if (!Error.empty() && !J->Stderr.empty())
              Error +=
                  "\n" +
                  J->Stderr.substr(
                      J->Stderr.size() > limits::kMaxDiagnosticCharacters
                          ? J->Stderr.size() - limits::kMaxDiagnosticCharacters
                          : 0);
            S->Controller.finish(Id, Event.ExitCode == 0, Error);
            S->Queue.post([S] { S->poll(); });
          }
        });
    J->Deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(limits::kHandshakeTimeoutMs);
    std::string Error;
    if (!J->Child->start(J->Plugin.Program, J->Plugin.Arguments,
                         J->Plugin.Path.parent_path(), Error))
      Controller.finish(Id, false, Error);
  }
  void poll() {
    // Terminal observers may destroy the facade while this stack is active.
    const auto KeepAlive = shared_from_this();
    if (std::exchange(Polling, true))
      return;
    struct Reset {
      bool &Value;
      ~Reset() { Value = false; }
    } ResetPolling{Polling};
    bool DrainRequired = false;
    for (auto I = Jobs.begin(); I != Jobs.end();) {
      const auto &[Id, J] = *I;
      auto Snapshot = Controller.snapshot(Id);
      if (!Snapshot) {
        I = Jobs.erase(I);
        continue;
      }
      if (Snapshot->Phase == TaskPhase::Queued && Controller.begin(Id)) {
        // Starting observers may retire the action before its process exists.
        // Complete that cancellation without launching a provider that missed
        // the stop callback.
        if (Controller.snapshot(Id)->Phase == TaskPhase::Starting)
          start(Id, J);
        else
          Controller.finish(Id, false);
      }
      Snapshot = Controller.snapshot(Id);
      if ((Snapshot->Phase == TaskPhase::Starting ||
           Snapshot->Phase == TaskPhase::Running) &&
          std::chrono::steady_clock::now() >= J->Deadline)
        Controller.stop(Id, TaskPhase::TimedOut, "Plugin deadline exceeded");
      Snapshot = Controller.snapshot(Id);
      if (terminal(Snapshot->Phase) && !J->PendingCommands && !J->Delivered) {
        J->Delivered = true;
        if (J->Child)
          J->Child->join();
        const auto Result = J->Wire ? J->Wire->result() : Json::object();
        Json Receipt{
            {"schema", J->Extensions.StructuredData ? "neverd.action-receipt.v1"
                                                    : "neverd.gui-job.v1"},
            {"operation", Id},
            {"task_id", Id},
            {"plugin", J->Plugin.Id},
            {"plugin_version", J->Plugin.Version},
            {"registration", J->Binding.id()},
            {"action", J->Definition.Id},
            {"context", J->Call->request().Context},
            {"parameters", J->Call->request().Parameters},
            {"state", taskPhaseName(Snapshot->Phase)},
            {"diagnostic", Snapshot->Diagnostic},
            {"result",
             Snapshot->Phase == TaskPhase::Succeeded ? Result : Json::object()},
            {"stderr", J->Stderr},
            {"elapsed_ms",
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - J->Started)
                 .count()},
            {"receipts", J->Call->snapshot().Receipts}};
        std::string Error;
        if (!writeReceipt(J->Directory, Receipt, Error)) {
          auto Failed = *Snapshot;
          Failed.Phase = TaskPhase::Failed;
          Failed.Diagnostic = Error;
          publish(Failed);
          J->Call->fail({"receipt_failed", Error});
        } else if (Snapshot->Phase == TaskPhase::Succeeded) {
          J->Receipt = Receipt;
          publish(*Snapshot);
          J->Call->succeed(Result.value("data", Json()),
                           Result.value("artifacts", Json::array()));
        } else {
          J->Receipt = Receipt;
          publish(*Snapshot);
          J->Call->fail({taskPhaseName(Snapshot->Phase), Snapshot->Diagnostic},
                        Snapshot->Phase == TaskPhase::Cancelled);
        }
      }
      if (J->Delivered && Controller.erase(Id))
        I = Jobs.erase(I);
      else {
        DrainRequired |= J->Delivered;
        ++I;
      }
    }
    if (Closing && Jobs.empty()) {
      auto Waiters = std::move(CloseWaiters);
      CloseWaiters.clear();
      for (auto &Done : Waiters)
        Queue.post(std::move(Done));
    }
    if (DrainRequired)
      Queue.post([Self = shared_from_this()] { Self->poll(); });
  }
};
ProcessActions::ProcessActions(PluginHost &Host, Actions &Actions,
                               Executor &Queue, std::filesystem::path Output,
                               Service Query, Service Command,
                               ProcessActionEvents Events,
                               HostServices *Capabilities)
    : State(std::make_shared<Impl>(Host, Actions, Queue, std::move(Output),
                                   std::move(Query), std::move(Command),
                                   std::move(Events), Capabilities)) {}
ProcessActions::~ProcessActions() {
  shutdown();
  for (const auto &[Id, J] : State->Jobs)
    if (J->Child)
      J->Child->join();
  State->Queue.drain();
  State->poll();
  // Accepted-command callbacks retain Impl until their receipts are drained.
  // The facade's destruction is not an asynchronous close completion signal.
}
std::optional<Registration> ProcessActions::add(Owner Scope,
                                                const Manifest &Plugin,
                                                ActionDefinition Definition,
                                                ProtocolExtensions Extensions,
                                                Diagnostic &Error) {
  const auto I =
      std::find_if(Plugin.Actions.begin(), Plugin.Actions.end(),
                   [&](const auto &A) { return A.at("id") == Definition.Id; });
  if (I == Plugin.Actions.end() ||
      (!Extensions.Commands.empty() &&
       Definition.Effect != ActionEffect::ProjectWrite)) {
    Error = {"invalid_action",
             "Missing manifest action or commands on a non-write action"};
    return std::nullopt;
  }
  Definition.PluginId = Plugin.Id;
  Definition.Version = Plugin.Version;
  Definition.Title = I->at("title");
  if (Definition.Description.empty())
    Definition.Description = Definition.Title;
  Definition.Context = I->at("context");
  Definition.Parameters = I->value("parameters", Json::array());
  if (Definition.Services.empty())
    Definition.Services = Plugin.Queries;
  if (!Extensions.StructuredData)
    Definition.OutputSchema = {{"type", "null"}};
  const auto Weak = std::weak_ptr<Impl>(State);
  const auto Token = std::make_shared<Registration>();
  auto Binding = State->ActionService.add(
      Scope, Definition,
      [Weak, Plugin, Action = *I, Definition,
       Extensions = std::move(Extensions), Token](auto Call) {
        auto S = Weak.lock();
        if (!S || S->Closing) {
          Call->fail({"runtime_closed", "Process runtime is closing"});
          return;
        }
        const auto Id = Call->request().Operation;
        if (Id.empty() || Id == "." || Id == ".." ||
            Id.find_first_of("/\\:") != std::string::npos ||
            Id.find('\0') != std::string::npos) {
          Call->fail(
              {"invalid_operation", "Operation ID must be one directory name"});
          return;
        }
        auto J = std::make_shared<Impl::Job>();
        J->Plugin = Plugin;
        J->Action = Action;
        J->Definition = Definition;
        J->CommitContext = Call->request().Context;
        J->Extensions = Extensions;
        J->Binding = *Token;
        J->Call = Call;
        J->Started = std::chrono::steady_clock::now();
        J->Directory = S->Output / std::filesystem::u8path(Id);
        std::error_code EC;
        // The operation ID is assigned by the host, never taken from provider
        // input.
        if (!std::filesystem::create_directory(J->Directory, EC)) {
          Call->fail(
              {"output_error",
               "Cannot create a new operation directory: " + EC.message()});
          return;
        }
        S->Jobs.emplace(Id, J);
        Diagnostic Error;
        if (!S->Controller.enqueue(Id, *Token, Error)) {
          S->Jobs.erase(Id);
          Call->fail(Error);
          return;
        }
        if (S->Capabilities) {
          const auto WeakJob = std::weak_ptr<Impl::Job>(J);
          J->Capabilities = S->Capabilities->attach(
              *S->Controller.owner(Id), Plugin, Call->request().Context,
              [WeakJob](Json Notification) {
                if (auto J = WeakJob.lock(); J && J->Wire)
                  J->Wire->event(Notification);
              });
          J->Extensions.HostCapabilities = true;
        }
        Call->onCancel([Weak, Id] {
          if (auto S = Weak.lock())
            S->Controller.stop(Id, TaskPhase::Cancelled, "Action cancelled");
        });
      },
      Error);
  if (Binding)
    *Token = *Binding;
  return Binding;
}
void ProcessActions::poll() { State->poll(); }
void ProcessActions::stop(const std::string &Id, TaskPhase Outcome,
                          std::string Reason) {
  State->Controller.stop(Id, Outcome, std::move(Reason));
}
void ProcessActions::shutdown(PluginHost::Work Done) {
  const auto State = this->State;
  if (Done)
    State->CloseWaiters.push_back(std::move(Done));
  if (!State->Closing) {
    State->Closing = true;
    const auto Jobs = State->Jobs;
    for (const auto &[Id, J] : Jobs)
      J->Call->cancel();
  }
  State->poll();
}
bool ProcessActions::idle() const { return State->Jobs.empty(); }
} // namespace neverd::plugin
