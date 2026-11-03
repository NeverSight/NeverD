//===- ToolRegistry.cpp - Model-independent plugin tools ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// NPH owns registrations and execution; Tools owns frozen consumer bindings.
//===----------------------------------------------------------------------===//

#include "neverd/tools/ToolRegistry.h"

#include "neverd/plugin/Schema.h"

#include <algorithm>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

namespace neverd::tools {
using namespace plugin;
namespace {
Json diagnostic(const Diagnostic &Error) {
  return {{"code", Error.Code}, {"message", Error.Message}};
}
const char *effectName(ActionEffect Effect) {
  switch (Effect) {
  case ActionEffect::Read:
    return "read";
  case ActionEffect::Artifact:
    return "artifact";
  case ActionEffect::ProjectWrite:
    return "project_write";
  }
  return "read";
}
Json layer(const Json &Config, const ToolInfo &Tool) {
  if (!Config.is_object())
    throw std::runtime_error(
        "Configuration must be an object keyed by plugin and tool");
  const auto Plugin = Config.value(Tool.Action.PluginId, Json::object());
  if (!Plugin.is_object())
    throw std::runtime_error(
        "Plugin configuration must be an object keyed by tool");
  return Plugin.value(Tool.Tool.Id, Json::object());
}
} // namespace

struct ResolvedTools::Impl {
  std::weak_ptr<void> Registry;
  uint64_t Id = 0;
  std::string Session, Step;
  Environment Captured;
  Json Configuration;
  bool Released = false;
  std::vector<ToolInfo> Entries;
};
ResolvedTools::ResolvedTools(std::shared_ptr<Impl> State)
    : State(std::move(State)) {}
uint64_t ResolvedTools::id() const { return State ? State->Id : 0; }
std::vector<ToolInfo> ResolvedTools::tools() const {
  return State && !State->Released ? State->Entries : std::vector<ToolInfo>();
}
void ResolvedTools::release() {
  if (State)
    State->Released = true;
}

struct ToolRegistry::Impl {
  struct Entry {
    ToolInfo Info;
    Owner Scope;
  };
  struct Running {
    Operation Info;
    Invocation Lease;
    std::shared_ptr<ActionCall> Call;
    bool CancelRequested = false;
  };
  PluginHost &Host;
  Actions &ActionService;
  Current GetCurrent;
  Policy Allow;
  Changed Notify;
  std::thread::id Thread = std::this_thread::get_id();
  uint64_t NextSet = 0, NextOperation = 0;
  std::string Identity;
  std::map<uint64_t, Entry> Entries;
  std::map<std::string, std::shared_ptr<Running>> Operations;
  std::map<std::tuple<std::string, std::string, std::string>, std::string>
      Calls;
  Impl(PluginHost &Host, Actions &Actions, Current Current, Policy Allow,
       Changed Notify)
      : Host(Host), ActionService(Actions), GetCurrent(std::move(Current)),
        Allow(std::move(Allow)), Notify(std::move(Notify)) {
    std::random_device Random;
    std::ostringstream Name;
    Name << std::hex << Random() << '-' << Random() << '-' << Random() << '-'
         << Random();
    Identity = Name.str();
  }
  void checkThread() const {
    if (Thread != std::this_thread::get_id())
      throw std::logic_error(
          "Tools operations require the NPH control sequence");
  }
  bool inProject(Owner Scope, Owner Project) const {
    auto P = Host.snapshot(Project), S = Host.snapshot(Scope);
    if (!P || !S)
      return false;
    const auto Owners = Host.owners();
    while (S) {
      if (S->Id == P->Id)
        return true;
      const auto I =
          std::find_if(Owners.begin(), Owners.end(),
                       [&](const auto &O) { return O.Id == S->ParentId; });
      S = I == Owners.end() ? std::nullopt : std::optional(*I);
    }
    return false;
  }
  Diagnostic available(const ToolInfo &Tool, const Environment &Env) const {
    if (!Host.describe(Tool.Token) || !Host.describe(Tool.Tool.Action))
      return {"registration_retired",
              "Tool or action registration is not active"};
    const auto Scope = Host.ownerOf(Tool.Token);
    if (!Scope || !inProject(*Scope, Env.Project))
      return {"wrong_project", "Tool belongs to a different project"};
    const auto &A = Tool.Action;
    if (Env.Headless && !A.Headless)
      return {"headless_unavailable", "Action requires a GUI consumer"};
    if ((A.Context != "none" && !Env.Context.value("loaded", false)) ||
        (A.Context == "address" && !Env.Context.contains("address")) ||
        (A.Context == "function" && !Env.Context.contains("function_address")))
      return {"context_unavailable", "Required action context is missing"};
    for (const auto &Service : A.Services)
      if (!Env.Services.contains(Service))
        return {"service_unavailable", "Missing service: " + Service};
    return Allow ? Allow(Tool, Env) : Diagnostic{};
  }
  void notify(const Operation &Info) {
    const auto Observer = Notify;
    const auto Value = Info;
    if (Observer)
      Observer(Value);
  }
};

ToolRegistry::ToolRegistry(PluginHost &Host, Actions &Actions,
                           Current GetCurrent, Policy Allow, Changed Notify)
    : State(std::make_shared<Impl>(Host, Actions, std::move(GetCurrent),
                                   std::move(Allow), std::move(Notify))) {}
ToolRegistry::~ToolRegistry() {
  State->Notify = {};
  const auto Entries = State->Entries;
  for (const auto &[Id, Entry] : Entries)
    State->Host.unregister(Entry.Info.Token);
}
std::optional<Registration> ToolRegistry::add(ToolDefinition Tool,
                                              Diagnostic &Error) {
  State->checkThread();
  const auto Action = State->ActionService.definition(Tool.Action);
  const auto Scope = State->ActionService.owner(Tool.Action);
  if (Tool.Id.empty() || !Action || !Scope) {
    Error = {"invalid_tool", "Tool requires an ID and exact action binding"};
    return std::nullopt;
  }
  if (Tool.ContinuationOf) {
    const auto Parent = State->ActionService.owner(*Tool.ContinuationOf);
    if (!Parent || *Parent != *Scope) {
      Error = {"invalid_continuation",
               "Continuation must bind an action in the same component"};
      return std::nullopt;
    }
  }
  auto Id = std::make_shared<uint64_t>(0);
  const auto Weak = std::weak_ptr<Impl>(State);
  const auto Token = State->Host.registerResource(
      *Scope, {"tool", Tool.Id, "neverd.tool.v1"},
      [Weak, Id] {
        if (auto State = Weak.lock()) {
          std::vector<std::shared_ptr<ActionCall>> Cancel;
          for (const auto &[Key, Op] : State->Operations)
            if (Op->Info.Tool.Token.id() == *Id) {
              Op->CancelRequested = true;
              if (Op->Call)
                Cancel.push_back(Op->Call);
            }
          for (const auto &Call : Cancel)
            Call->cancel();
        }
      },
      [Weak, Id](auto Done) {
        if (auto State = Weak.lock())
          State->Entries.erase(*Id);
        Done({});
      },
      Error);
  if (!Token)
    return std::nullopt;
  *Id = Token->id();
  State->Entries.emplace(
      *Id, Impl::Entry{ToolInfo{*Token, std::move(Tool), *Action}, *Scope});
  return Token;
}
std::optional<ResolvedTools>
ToolRegistry::resolve(std::string Session, std::string Step, const Json &User,
                      const Json &Profile, Diagnostic &Error) {
  const auto State = this->State;
  State->checkThread();
  if (Session.empty() || Step.empty()) {
    Error = {"invalid_step", "Consumer session and step IDs are required"};
    return std::nullopt;
  }
  auto Set = std::make_shared<ResolvedTools::Impl>();
  Set->Registry = State;
  Set->Id = ++State->NextSet;
  Set->Session = std::move(Session);
  Set->Step = std::move(Step);
  Set->Captured = State->GetCurrent();
  Set->Configuration = {{"user", User}, {"profile", Profile}};
  const auto Entries = State->Entries;
  try {
    for (const auto &[Id, Entry] : Entries) {
      if (!State->inProject(Entry.Scope, Set->Captured.Project))
        continue;
      // Staged/retiring registrations are not published as callable tools.
      if (!State->Host.describe(Entry.Info.Token))
        continue;
      auto Info = Entry.Info;
      Info.Unavailable = State->available(Info, Set->Captured);
      std::string Why;
      const auto Parameters =
          resolveParameters(Info.Action.Parameters, layer(User, Info),
                            layer(Profile, Info), Json::object(), Why);
      if (!Parameters)
        Info.Unavailable = {"invalid_configuration", Why};
      else
        Info.EffectiveParameters = *Parameters;
      Set->Entries.push_back(std::move(Info));
    }
  } catch (const std::exception &Exception) {
    Error = {"invalid_configuration", Exception.what()};
    return std::nullopt;
  }
  Error = {};
  return ResolvedTools(std::move(Set));
}
std::optional<std::string>
ToolRegistry::call(const ResolvedTools &Set, Registration Token,
                   std::string CallId, const Json &Parameters,
                   std::string ParentId, Diagnostic &Error) {
  const auto State = this->State;
  State->checkThread();
  const auto Fail = [&](std::string Code,
                        std::string Why) -> std::optional<std::string> {
    Error = {std::move(Code), std::move(Why)};
    return std::nullopt;
  };
  if (!Set.State || Set.State->Registry.lock() != State || Set.State->Released)
    return Fail("set_released", "Resolved tool set is unavailable");
  if (CallId.empty())
    return Fail("invalid_call", "Call ID is required");
  const auto I =
      std::find_if(Set.State->Entries.begin(), Set.State->Entries.end(),
                   [&](const auto &Info) { return Info.Token == Token; });
  if (I == Set.State->Entries.end())
    return Fail("binding_missing", "Tool is not in this resolved set");
  const auto Info = *I;
  std::string Why;
  const auto Effective =
      resolveParameters(Info.Action.Parameters, Json::object(),
                        Info.EffectiveParameters, Parameters, Why);
  if (!Effective)
    return Fail("invalid_parameters", Why);
  const auto Key = std::make_tuple(Set.State->Session, Set.State->Step, CallId);
  const auto Existing = [&]() -> std::optional<std::string> {
    const auto &Previous = State->Operations.at(State->Calls.at(Key))->Info;
    if (Previous.Tool.Token != Token || Previous.Parameters != *Effective ||
        Previous.ParentOperation != ParentId ||
        Previous.Context != Set.State->Captured.Context ||
        Previous.Configuration != Set.State->Configuration)
      return Fail(
          "call_conflict",
          "Call ID already refers to a different binding, input or context");
    Error = {};
    return Previous.Id;
  };
  if (State->Calls.contains(Key))
    return Existing();
  if (!Info.Unavailable.Code.empty()) {
    Error = Info.Unavailable;
    return std::nullopt;
  }
  const auto Current = State->GetCurrent();
  if (Current.Project != Set.State->Captured.Project ||
      Current.Context != Set.State->Captured.Context)
    return Fail("context_changed",
                "Resolved input/project/profile/revision changed");
  Error = State->available(Info, Current);
  if (!Error.Code.empty())
    return std::nullopt;
  // Consumer policy can reenter the control sequence. A call admitted there
  // owns this key; never create another operation after returning from policy.
  if (State->Calls.contains(Key))
    return Existing();
  const auto AfterPolicy = State->GetCurrent();
  if (Set.State->Released)
    return Fail("set_released",
                "Resolved tool set was released during admission");
  if (AfterPolicy.Project != Current.Project ||
      AfterPolicy.Context != Current.Context)
    return Fail("context_changed", "Context changed during admission");
  Json Continuation;
  if (Info.Tool.ContinuationOf) {
    const auto Found = State->Operations.find(ParentId);
    if (Found == State->Operations.end())
      return Fail("unknown_operation",
                  "Continuation requires a known completed operation");
    const auto &Parent = Found->second->Info;
    if (Parent.Session != Set.State->Session ||
        Parent.Tool.Tool.Action != *Info.Tool.ContinuationOf ||
        Parent.Result.Phase != ActionPhase::Succeeded ||
        !State->Host.describe(Parent.Tool.Tool.Action))
      return Fail("invalid_continuation",
                  "Parent operation or action generation is unavailable");
    if (Parent.Context != Current.Context ||
        Parent.Configuration != Set.State->Configuration)
      return Fail("context_changed",
                  "Original report input/project/profile/revision changed");
    Continuation = describeOperation(Parent);
  } else if (!ParentId.empty())
    return Fail("invalid_continuation", "Tool does not declare a continuation");
  auto Lease = State->Host.invoke(Token, Error);
  if (!Lease)
    return std::nullopt;
  auto Op = std::make_shared<Impl::Running>();
  Op->Info = {State->Identity + "-op-" + std::to_string(++State->NextOperation),
              Set.State->Session,
              Set.State->Step,
              std::move(CallId),
              Set.id(),
              Info,
              Current.Context,
              *Effective,
              Set.State->Configuration,
              std::move(ParentId),
              {}};
  Op->Lease = std::move(Lease);
  // Record before entering provider code: reentrant/repeated calls see one op.
  State->Calls.emplace(Key, Op->Info.Id);
  State->Operations.emplace(Op->Info.Id, Op);
  const auto Strong = State;
  Op->Call = State->ActionService.invoke(
      Info.Tool.Action,
      {Op->Info.Id, Current.Context, *Effective, Continuation},
      [Strong, Op](const auto &Result) {
        Op->Info.Result = Result;
        if (terminal(Result.Phase))
          Op->Lease.reset();
        Strong->notify(Op->Info);
      },
      Error,
      [Op](auto Call) {
        Op->Call = std::move(Call);
        if (Op->CancelRequested)
          Op->Call->cancel();
      });
  if (!Op->Call) {
    Op->Info.Result.Phase = ActionPhase::Failed;
    Op->Info.Result.Error = Error;
    Op->Lease.reset();
    State->notify(Op->Info);
  } else if (Op->CancelRequested)
    Op->Call->cancel();
  Error = {};
  return Op->Info.Id;
}
std::optional<Operation> ToolRegistry::operation(const std::string &Id) const {
  State->checkThread();
  const auto I = State->Operations.find(Id);
  return I == State->Operations.end() ? std::nullopt
                                      : std::optional(I->second->Info);
}
bool ToolRegistry::cancel(const std::string &Id) {
  State->checkThread();
  const auto I = State->Operations.find(Id);
  if (I == State->Operations.end())
    return false;
  I->second->CancelRequested = true;
  if (I->second->Call)
    I->second->Call->cancel();
  return true;
}
void ToolRegistry::setObserver(Changed Notify) {
  State->checkThread();
  State->Notify = std::move(Notify);
}
std::string ToolRegistry::runtimeId() const {
  State->checkThread();
  return State->Identity;
}

Json describeTool(const ToolInfo &Info) {
  auto Input = parameterSchema(Info.Action.Parameters);
  for (auto I = Info.EffectiveParameters.begin();
       I != Info.EffectiveParameters.end(); ++I)
    Input["properties"][I.key()]["default"] = I.value();
  return {{"plugin", Info.Action.PluginId},
          {"version", Info.Action.Version},
          {"id", Info.Tool.Id},
          {"title", Info.Action.Title},
          {"description", Info.Action.Description},
          {"registration", Info.Token.id()},
          {"action_registration", Info.Tool.Action.id()},
          {"input_schema", Input},
          {"output_schema", Info.Action.OutputSchema},
          {"context", Info.Action.Context},
          {"services", Info.Action.Services},
          {"effect", effectName(Info.Action.Effect)},
          {"headless", Info.Action.Headless},
          {"cancellation", "cooperative"},
          {"commit", "accepted_commands_keep_receipts"},
          {"continuation_of", Info.Tool.ContinuationOf
                                  ? Json(Info.Tool.ContinuationOf->id())
                                  : Json()},
          {"available", Info.Unavailable.Code.empty()},
          {"reason", diagnostic(Info.Unavailable)}};
}
Json describeOperation(const Operation &Op) {
  return {{"operation", Op.Id},
          {"session", Op.Session},
          {"step", Op.Step},
          {"call_id", Op.CallId},
          {"set", Op.SetId},
          {"tool", describeTool(Op.Tool)},
          {"context", Op.Context},
          {"parameters", Op.Parameters},
          {"configuration", Op.Configuration},
          {"parent_operation", Op.ParentOperation},
          {"state", actionPhaseName(Op.Result.Phase)},
          {"cancel_requested", Op.Result.CancelRequested},
          {"progress", Op.Result.Progress},
          {"data", Op.Result.Data},
          {"artifacts", Op.Result.Artifacts},
          {"receipts", Op.Result.Receipts},
          {"error", diagnostic(Op.Result.Error)}};
}
} // namespace neverd::tools
