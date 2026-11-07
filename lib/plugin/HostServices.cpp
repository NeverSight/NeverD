//===- HostServices.cpp - Common host services and event bridge -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt existing queries, context notifications and declared process clients.
//===----------------------------------------------------------------------===//

#include "neverd/plugin/HostServices.h"

#include "neverd/plugin/Config.h"

#include <algorithm>
#include <map>
#include <utility>

namespace neverd::plugin {
namespace {
Json ok(Json Payload) {
  return {{"status", "ok"}, {"payload", std::move(Payload)}};
}
bool declares(const std::vector<CapabilityRequirement> &List,
              const std::string &Id, uint32_t Version) {
  return std::any_of(List.begin(), List.end(), [&](const auto &R) {
    return R.Id == Id && R.Version == Version;
  });
}
Json emptyObjectSchema() {
  return {{"type", "object"},
          {"properties", Json::object()},
          {"additionalProperties", false}};
}
} // namespace
struct HostServices::Impl : std::enable_shared_from_this<Impl> {
  struct Project {
    Owner Scope, Provider;
    Json Context;
    std::map<std::string, Registration> Sources;
  };
  struct Contexts {
    std::map<uint64_t, Project> Projects;
  };
  PluginHost &Host;
  std::shared_ptr<Contexts> Current = std::make_shared<Contexts>();
  Services Registry;
  Events Bus;
  Services::Handler Query;
  Impl(PluginHost &Host, PluginHost::Post Post, Services::Handler Query)
      : Host(Host),
        Registry(Host,
                 [Weak = std::weak_ptr<Contexts>(Current)](Owner Domain) {
                   if (auto C = Weak.lock()) {
                     const auto I = C->Projects.find(Domain.id());
                     if (I != C->Projects.end() && I->second.Scope == Domain)
                       return I->second.Context;
                   }
                   return Json();
                 }),
        Bus(Host, std::move(Post)), Query(std::move(Query)) {}
};
struct HostServiceSession::Impl {
  std::shared_ptr<HostServices::Impl> Host;
  Owner Caller;
  Json Context;
  std::string Task;
  std::vector<CapabilityRequirement> RequiredServices, RequiredEvents;
  std::map<std::string, ServiceDescription> Services;
  std::map<std::string, EventDescription> Events;
  std::map<std::string, Registration> Subscriptions;
  std::function<void(Json)> Notification;
  bool serviceDeclared(const ServiceDefinition &D) const {
    return declares(RequiredServices, D.Id, D.Version);
  }
};
HostServices::HostServices(PluginHost &Host, PluginHost::Post Post,
                           Services::Handler Query)
    : State(std::make_shared<Impl>(Host, std::move(Post), std::move(Query))) {}
HostServices::~HostServices() {
  const auto Projects = State->Current->Projects;
  for (const auto &[Id, P] : Projects)
    State->Host.close(P.Provider);
}
Services &HostServices::services() { return State->Registry; }
Events &HostServices::events() { return State->Bus; }
bool HostServices::bind(Owner Project, Json Context, bool AnalysisAvailable,
                        Diagnostic &Error) {
  const auto Snapshot = State->Host.snapshot(Project);
  if (!Snapshot || Snapshot->Kind != OwnerKind::Project ||
      !Context.is_object() || State->Current->Projects.contains(Project.id()) ||
      (AnalysisAvailable && !State->Query)) {
    Error = {"invalid_service_project",
             "Services require one valid binding per project owner"};
    return false;
  }
  auto Provider = State->Host.createOwner(Project, OwnerKind::Component,
                                          "nph-services", Error);
  if (!Provider)
    return false;
  State->Current->Projects.emplace(
      Project.id(), Impl::Project{Project, *Provider, std::move(Context), {}});
  auto Cleanup = State->Host.registerResource(
      *Provider, {"context", "project", "v1"}, {},
      [Weak = std::weak_ptr<Impl::Contexts>(State->Current),
       Project](auto Done) {
        if (auto C = Weak.lock())
          C->Projects.erase(Project.id());
        Done({});
      },
      Error);
  const auto Fail = [&] {
    State->Host.failActivation(*Provider, Error.Message);
    return false;
  };
  if (!Cleanup)
    return Fail();
  ServiceDefinition Definition;
  Definition.Id = "project.context";
  Definition.Description = "Read the current host context explicitly; does not "
                           "change captured analysis input";
  Definition.Consistency = ServiceConsistency::None;
  Definition.InputSchema = emptyObjectSchema();
  if (!State->Registry.add(
          *Provider, Project, std::move(Definition),
          [Weak = std::weak_ptr<Impl::Contexts>(State->Current),
           Project](const auto &, auto Done) {
            auto C = Weak.lock();
            Done(ok(C->Projects.at(Project.id()).Context));
          },
          Error))
    return Fail();
  if (AnalysisAvailable)
    for (const auto &Query : queryServices()) {
      ServiceDefinition D;
      D.Id = "analysis." + Query;
      D.Description =
          "Existing " + Query + " query; SDK/worker owns payload validation";
      if (!State->Registry.add(
              *Provider, Project, std::move(D),
              [Run = State->Query, Query](const ServiceRequest &Request,
                                          auto Done) {
                if (!Request.Payload.is_object()) {
                  Done(serviceError(
                      {"invalid_service_input",
                       "Analysis query payload must be an object"}));
                  return;
                }
                auto Adapted = Request;
                Adapted.Operation = Query;
                Run(Adapted, std::move(Done));
              },
              Error))
        return Fail();
    }
  for (const auto *Id :
       {"project.context_changed", "project.revision_changed",
        "selection.changed", "task.changed", "plugin.installation_changed"}) {
    EventDefinition D;
    D.Id = Id;
    D.Description = Id;
    const auto Source = State->Bus.add(*Provider, Project, std::move(D), Error);
    if (!Source)
      return Fail();
    State->Current->Projects.at(Project.id()).Sources.emplace(Id, *Source);
  }
  if (!State->Host.activate(*Provider, Error))
    return Fail();
  return true;
}
void HostServices::publish(Owner Project, const std::string &Id, Json Payload) {
  const auto I =
      std::find_if(State->Current->Projects.begin(),
                   State->Current->Projects.end(), [&](const auto &P) {
                     return State->Host.contains(P.second.Scope, Project);
                   });
  if (I == State->Current->Projects.end())
    return;
  const auto Source = I->second.Sources.find(Id);
  if (Source == I->second.Sources.end())
    return;
  Diagnostic Error;
  State->Bus.publish(Source->second, I->second.Context, std::move(Payload),
                     Error);
}
void HostServices::setContext(Owner Project, Json Context) {
  auto &P = State->Current->Projects.at(Project.id());
  if (P.Context == Context)
    return;
  auto Previous = std::exchange(P.Context, std::move(Context));
  publish(Project, "project.context_changed",
          {{"previous", Previous}, {"current", P.Context}});
  if (Previous.value("revision", Json()) != P.Context.value("revision", Json()))
    publish(Project, "project.revision_changed",
            {{"previous", Previous.value("revision", Json())},
             {"current", P.Context.value("revision", Json())}});
  if (Previous.value("address", Json()) != P.Context.value("address", Json()) ||
      Previous.value("function_address", Json()) !=
          P.Context.value("function_address", Json()))
    publish(Project, "selection.changed",
            {{"previous", Previous}, {"current", P.Context}});
}
std::shared_ptr<HostServiceSession>
HostServices::attach(Owner Caller, const Manifest &Plugin, Json Context,
                     std::function<void(Json)> Notification) {
  auto Session = std::make_shared<HostServiceSession::Impl>();
  Session->Host = State;
  Session->Caller = Caller;
  Session->Context = std::move(Context);
  Session->Task = State->Host.snapshot(Caller)->Name;
  Session->RequiredServices = Plugin.Services;
  Session->RequiredEvents = Plugin.Events;
  Session->Notification = std::move(Notification);
  for (const auto &Query : Plugin.Queries)
    if (!declares(Session->RequiredServices, "analysis." + Query, 1))
      Session->RequiredServices.push_back({"analysis." + Query, 1});
  for (const auto &D : State->Registry.discover(Caller, Session->Context))
    Session->Services.emplace(std::to_string(D.Binding.id()), D);
  for (const auto &D : State->Bus.discover(Caller))
    Session->Events.emplace(std::to_string(D.Binding.id()), D);
  return std::shared_ptr<HostServiceSession>(
      new HostServiceSession(std::move(Session)));
}
HostServiceSession::HostServiceSession(std::shared_ptr<Impl> State)
    : State(std::move(State)) {}
void HostServiceSession::query(const ServiceRequest &Request,
                               Services::Reply Done) {
  for (const auto &[Id, D] : State->Services)
    if (D.Definition.Id == "analysis." + Request.Operation &&
        D.Definition.Version == 1 && State->serviceDeclared(D.Definition)) {
      Diagnostic Error;
      if (!State->Host->Registry.invoke(State->Caller, D.Binding, Request, Done,
                                        Error))
        Done(serviceError(Error));
      return;
    }
  Done(serviceError({"service_unavailable",
                     "No exact analysis service binding was captured"}));
}
void HostServiceSession::request(const std::string &Id,
                                 const std::string &Operation,
                                 const Json &Payload, Services::Reply Done) {
  const auto Fail = [&](const char *Code, const char *Message) {
    Done(serviceError({Code, Message}));
  };
  if (Operation == "services.list" || Operation == "events.list") {
    if (!Payload.empty()) {
      Fail("invalid_request", "Discovery payload must be empty");
      return;
    }
    Json Items = Json::array(), Missing = Json::array();
    if (Operation == "services.list") {
      const auto CurrentServices =
          State->Host->Registry.discover(State->Caller, State->Context);
      for (const auto &[Id, D] : State->Services) {
        auto Description = describeService(D);
        Description["declared"] = State->serviceDeclared(D.Definition);
        const auto Current = std::find_if(
            CurrentServices.begin(), CurrentServices.end(),
            [&](const auto &Value) { return Value.Binding == D.Binding; });
        if (Current != CurrentServices.end()) {
          Description["available"] = Current->Unavailable.Code.empty();
          Description["unavailable"] = {
              {"code", Current->Unavailable.Code},
              {"message", Current->Unavailable.Message}};
        } else {
          Description["available"] = false;
          Description["unavailable"] = {
              {"code", "service_unavailable"},
              {"message", "Captured service has retired"}};
        }
        Items.push_back(std::move(Description));
      }
      for (const auto &R : State->RequiredServices)
        if (std::none_of(State->Services.begin(), State->Services.end(),
                         [&](const auto &D) {
                           return D.second.Definition.Id == R.Id &&
                                  D.second.Definition.Version == R.Version;
                         }))
          Missing.push_back({{"id", R.Id},
                             {"version", R.Version},
                             {"code", "service_unavailable"}});
    } else {
      for (const auto &[Id, D] : State->Events) {
        auto Description = describeEventSource(D);
        Description["available"] =
            State->Host->Host.describe(D.Binding).has_value();
        Description["declared"] = declares(
            State->RequiredEvents, D.Definition.Id, D.Definition.Version);
        Items.push_back(std::move(Description));
      }
      for (const auto &R : State->RequiredEvents)
        if (std::none_of(State->Events.begin(), State->Events.end(),
                         [&](const auto &D) {
                           return D.second.Definition.Id == R.Id &&
                                  D.second.Definition.Version == R.Version;
                         }))
          Missing.push_back({{"id", R.Id},
                             {"version", R.Version},
                             {"code", "event_unavailable"}});
    }
    Done(ok({{Operation == "services.list" ? "services" : "events",
              std::move(Items)},
             {"missing", std::move(Missing)}}));
  } else if (Operation == "services.call") {
    if (Payload.size() != 2 || !Payload.contains("registration") ||
        !Payload["registration"].is_string() || !Payload.contains("payload")) {
      Fail("invalid_request", "Service call requires registration and payload");
      return;
    }
    const auto I =
        State->Services.find(Payload["registration"].get<std::string>());
    if (I == State->Services.end() ||
        !State->serviceDeclared(I->second.Definition)) {
      Fail("service_undeclared",
           "Service binding was not captured or declared");
      return;
    }
    if (I->second.Definition.Effect != ServiceEffect::Read) {
      Fail("service_unavailable",
           "Project writes require the declared command adapter");
      return;
    }
    Diagnostic Error;
    if (!State->Host->Registry.invoke(
            State->Caller, I->second.Binding,
            {State->Task, Id, {}, State->Context, Payload["payload"]}, Done,
            Error))
      Done(serviceError(Error));
  } else if (Operation == "events.subscribe") {
    if (Payload.size() != 1 || !Payload.contains("registration") ||
        !Payload["registration"].is_string()) {
      Fail("invalid_request",
           "Subscription requires an exact event registration");
      return;
    }
    const auto I =
        State->Events.find(Payload["registration"].get<std::string>());
    if (I == State->Events.end() ||
        !declares(State->RequiredEvents, I->second.Definition.Id,
                  I->second.Definition.Version)) {
      Fail("event_undeclared", "Event binding was not captured or declared");
      return;
    }
    if (State->Subscriptions.size() >= size_t(limits::kMaxEventSubscriptions)) {
      Fail("subscription_limit", "Process task subscription limit reached");
      return;
    }
    Diagnostic Error;
    auto Identity = std::make_shared<std::string>();
    auto Token = State->Host->Bus.subscribe(
        State->Caller, I->second.Binding,
        [Weak = std::weak_ptr<Impl>(State), Identity](const Event &E) {
          if (auto S = Weak.lock())
            S->Notification(
                {{"subscription", *Identity}, {"event", describeEvent(E)}});
        },
        Error);
    if (!Token) {
      Done(serviceError(Error));
      return;
    }
    *Identity = std::to_string(Token->id());
    State->Subscriptions.emplace(*Identity, *Token);
    Done(ok({{"subscription", *Identity}}));
  } else if (Operation == "events.unsubscribe") {
    if (Payload.size() != 1 || !Payload.contains("subscription") ||
        !Payload["subscription"].is_string()) {
      Fail("invalid_request", "Unsubscribe requires a subscription ID");
      return;
    }
    const auto Key = Payload["subscription"].get<std::string>();
    const auto I = State->Subscriptions.find(Key);
    if (I != State->Subscriptions.end()) {
      State->Host->Host.unregister(I->second);
      State->Subscriptions.erase(I);
    }
    Done(ok({{"subscription", Key}, {"active", false}}));
  } else
    Fail("unsupported_operation", "Unknown host capability operation");
}
} // namespace neverd::plugin
