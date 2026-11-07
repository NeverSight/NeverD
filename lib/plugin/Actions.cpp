//===- Actions.cpp - Shared callable NPH actions --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Call leases keep provider resources alive until real completion.
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Actions.h"

#include "neverd/plugin/Schema.h"

#include <map>
#include <stdexcept>
#include <thread>
#include <utility>

namespace neverd::plugin {
const char *actionPhaseName(ActionPhase Phase) {
  switch (Phase) {
  case ActionPhase::Running:
    return "running";
  case ActionPhase::Cancelling:
    return "cancelling";
  case ActionPhase::Succeeded:
    return "succeeded";
  case ActionPhase::Failed:
    return "failed";
  case ActionPhase::Cancelled:
    return "cancelled";
  }
  return "failed";
}
bool terminal(ActionPhase Phase) {
  return Phase == ActionPhase::Succeeded || Phase == ActionPhase::Failed ||
         Phase == ActionPhase::Cancelled;
}
struct ActionCall::Impl {
  ActionRequest Request;
  ActionSnapshot Snapshot;
  Json OutputSchema;
  Invocation Lease;
  Actions::Changed Notify;
  PluginHost::Work Cancel;
  std::thread::id Thread = std::this_thread::get_id();
  void checkThread() const {
    if (Thread != std::this_thread::get_id())
      throw std::logic_error(
          "Action callbacks require the NPH control sequence");
  }
  void notify() {
    const auto Observer = Notify;
    const auto Value = Snapshot;
    if (Observer)
      Observer(Value);
  }
  void finish() {
    Cancel = {};
    // Invocation release is queued, keeping provider code alive through Notify.
    Lease.reset();
    auto Done = std::move(Notify);
    if (Done)
      Done(Snapshot);
  }
};
ActionCall::ActionCall(std::shared_ptr<Impl> State) : State(std::move(State)) {}
const ActionRequest &ActionCall::request() const {
  State->checkThread();
  return State->Request;
}
const ActionSnapshot &ActionCall::snapshot() const {
  State->checkThread();
  return State->Snapshot;
}
void ActionCall::onCancel(PluginHost::Work Cancel) {
  State->checkThread();
  if (terminal(State->Snapshot.Phase))
    return;
  State->Cancel = std::move(Cancel);
  if (State->Snapshot.CancelRequested && State->Cancel) {
    auto Run = std::move(State->Cancel);
    Run();
  }
}
void ActionCall::cancel() {
  State->checkThread();
  if (terminal(State->Snapshot.Phase) || State->Snapshot.CancelRequested)
    return;
  State->Snapshot.CancelRequested = true;
  State->Snapshot.Phase = ActionPhase::Cancelling;
  auto Cancel = std::move(State->Cancel);
  State->notify();
  if (Cancel)
    Cancel();
}
void ActionCall::progress(Json Progress) {
  State->checkThread();
  if (terminal(State->Snapshot.Phase))
    return;
  State->Snapshot.Progress = std::move(Progress);
  State->notify();
}
void ActionCall::receipt(Json Receipt) {
  State->checkThread();
  if (terminal(State->Snapshot.Phase))
    throw std::logic_error(
        "Action completed before its accepted command receipt");
  State->Snapshot.Receipts.push_back(std::move(Receipt));
  State->notify();
}
void ActionCall::succeed(Json Data, Json Artifacts) {
  State->checkThread();
  if (terminal(State->Snapshot.Phase))
    return;
  std::string Error;
  if (!validateValue(State->OutputSchema, Data, Error) ||
      !Artifacts.is_array()) {
    fail({"invalid_output",
          Error.empty() ? "Artifacts must be an array" : Error});
    return;
  }
  State->Snapshot.Phase = ActionPhase::Succeeded;
  State->Snapshot.Data = std::move(Data);
  State->Snapshot.Artifacts = std::move(Artifacts);
  State->finish();
}
void ActionCall::fail(Diagnostic Error, bool Cancelled) {
  State->checkThread();
  if (terminal(State->Snapshot.Phase))
    return;
  State->Snapshot.Phase =
      Cancelled ? ActionPhase::Cancelled : ActionPhase::Failed;
  State->Snapshot.Error = std::move(Error);
  State->finish();
}

struct Actions::Impl {
  struct Entry {
    Owner Scope;
    Registration Token;
    ActionDefinition Definition;
    Handler Run;
    bool Retired = false;
    std::vector<std::shared_ptr<ActionCall>> Calls;
  };
  PluginHost &Host;
  std::map<uint64_t, std::shared_ptr<Entry>> Entries;
  explicit Impl(PluginHost &Host) : Host(Host) {}
  std::shared_ptr<Entry> find(Registration Token) const {
    const auto I = Entries.find(Token.id());
    return I != Entries.end() && I->second->Token == Token &&
                   !I->second->Retired
               ? I->second
               : nullptr;
  }
};
Actions::Actions(PluginHost &Host) : State(std::make_shared<Impl>(Host)) {}
Actions::~Actions() {
  // Unregister before releasing the dispatcher. In-flight calls remain leased;
  // the transport owner must still drain them before destroying its resources.
  auto Entries = State->Entries;
  for (const auto &[Id, Entry] : Entries)
    State->Host.unregister(Entry->Token);
}
std::optional<Registration> Actions::add(Owner Scope,
                                         ActionDefinition Definition,
                                         Handler Run, Diagnostic &Error) {
  std::string Why;
  if (Definition.PluginId.empty() || Definition.Id.empty() || !Run ||
      (Definition.Context != "none" && Definition.Context != "project" &&
       Definition.Context != "address" && Definition.Context != "function") ||
      !validateParameterFields(Definition.Parameters, Why) ||
      !validateSchema(Definition.OutputSchema, Why)) {
    Error = {"invalid_action",
             Why.empty() ? "Invalid action declaration" : Why};
    return std::nullopt;
  }
  auto Entry = std::make_shared<Impl::Entry>();
  Entry->Scope = Scope;
  Entry->Definition = std::move(Definition);
  Entry->Run = std::move(Run);
  auto Token = State->Host.registerResource(
      Scope, {"action", Entry->Definition.Id, "neverd.action.v1"},
      [Entry] {
        Entry->Retired = true;
        const auto Calls = Entry->Calls;
        for (const auto &Call : Calls)
          Call->cancel();
      },
      [Entry, Weak = std::weak_ptr<Impl>(State)](auto Done) {
        Entry->Run = {};
        Entry->Calls.clear();
        if (auto State = Weak.lock())
          State->Entries.erase(Entry->Token.id());
        Done({});
      },
      Error);
  if (!Token)
    return std::nullopt;
  Entry->Token = *Token;
  State->Entries.emplace(Token->id(), std::move(Entry));
  return Token;
}
std::optional<ActionDefinition> Actions::definition(Registration Action) const {
  const auto Entry = State->find(Action);
  return Entry ? std::optional(Entry->Definition) : std::nullopt;
}
std::optional<Owner> Actions::owner(Registration Action) const {
  const auto Entry = State->find(Action);
  return Entry ? std::optional(Entry->Scope) : std::nullopt;
}
std::shared_ptr<ActionCall> Actions::invoke(Registration Action,
                                            ActionRequest Request,
                                            Changed Notify, Diagnostic &Error,
                                            Handler BindBeforeStart) {
  const auto Entry = State->find(Action);
  if (!Entry) {
    Error = {"action_unavailable", "Action registration is unavailable"};
    return {};
  }
  std::string Why;
  if (!validateParameters(Entry->Definition.Parameters, Request.Parameters,
                          Why)) {
    Error = {"invalid_parameters", Why};
    return {};
  }
  const auto &Context = Request.Context;
  const auto &Requirement = Entry->Definition.Context;
  if (!Context.is_object() ||
      (Requirement != "none" &&
       (Context.value("loaded", Json()) != true ||
        (Requirement == "address" &&
         (!Context.contains("address") || !Context["address"].is_string() ||
          Context["address"].get_ref<const std::string &>().empty())) ||
        (Requirement == "function" &&
         (!Context.contains("function_address") ||
          !Context["function_address"].is_string() ||
          Context["function_address"]
              .get_ref<const std::string &>()
              .empty()))))) {
    Error = {"context_unavailable",
             "The action's required context is unavailable"};
    return {};
  }
  auto Lease = State->Host.invoke(Action, Error);
  if (!Lease)
    return {};
  auto Data = std::make_shared<ActionCall::Impl>();
  Data->Request = std::move(Request);
  Data->OutputSchema = Entry->Definition.OutputSchema;
  // The outer lease spans the handler's stack even when synchronous completion
  // reenters the dispatcher. The call lease spans asynchronous work afterward.
  Data->Lease = State->Host.invoke(Action, Error);
  Data->Notify = std::move(Notify);
  auto Call = std::shared_ptr<ActionCall>(new ActionCall(std::move(Data)));
  std::erase_if(Entry->Calls, [](const auto &Call) {
    return terminal(Call->snapshot().Phase);
  });
  Entry->Calls.push_back(Call);
  // Copy across reentrant unregister/completion; no registry iterator survives.
  const auto Run = Entry->Run;
  try {
    if (BindBeforeStart)
      BindBeforeStart(Call);
    if (!terminal(Call->snapshot().Phase))
      Run(Call);
  } catch (const std::exception &Exception) {
    Call->fail({"handler_error", Exception.what()});
  } catch (...) {
    Call->fail({"handler_error", "Action handler threw an unknown exception"});
  }
  Error = {};
  return Call;
}
} // namespace neverd::plugin
