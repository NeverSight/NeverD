//===- ServiceEventTests.cpp - Service and notification contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise exact versions, project isolation and queued owner retirement.
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Executor.h"
#include "neverd/plugin/HostServices.h"

#include <iostream>
#include <stdexcept>

using namespace neverd::plugin;
namespace {
void check(bool Value, const char *Why) {
  if (!Value)
    throw std::runtime_error(Why);
}
Owner owner(PluginHost &Host, Owner Parent, OwnerKind Kind, const char *Name,
            bool Active = true) {
  Diagnostic Error;
  const auto Scope = Host.createOwner(Parent, Kind, Name, Error);
  check(Scope.has_value(), "create owner");
  if (Active)
    check(Host.activate(*Scope, Error), "activate owner");
  return *Scope;
}
struct Fixture {
  Executor Queue;
  PluginHost Host{[this](auto Work) { Queue.post(std::move(Work)); }};
  Json Context{{"loaded", true},
               {"project_id", "project"},
               {"epoch", "1"},
               {"revision", "1"}};
  Services Registry{Host, [this](Owner) { return Context; }};
  Events Bus{Host, [this](auto Work) { Queue.post(std::move(Work)); }};
  Owner Project = owner(Host, Host.root(), OwnerKind::Project, "first");
  Owner Provider =
      owner(Host, Project, OwnerKind::Component, "provider", false);
  Owner Caller = owner(Host, Project, OwnerKind::Component, "caller");
  ~Fixture() {
    Host.close(Host.root());
    Queue.drain();
  }
  Registration service(Services::Handler Run = {}, uint32_t Version = 1) {
    Diagnostic Error;
    ServiceDefinition D;
    D.Id = "sample.increment";
    D.Version = Version;
    D.InputSchema = D.OutputSchema = {{"type", "integer"}};
    if (!Run)
      Run = [](const auto &R, auto Done) {
        Done(
            {{"status", "ok"}, {"payload", R.Payload.template get<int>() + 1}});
      };
    auto T = Registry.add(Provider, Project, D, std::move(Run), Error);
    check(T.has_value(), "register service");
    return *T;
  }
  void activate() {
    Diagnostic Error;
    check(Host.activate(Provider, Error), "activate provider");
  }
};
void services(Fixture &F) {
  Diagnostic Error;
  auto First = F.service();
  auto Second = F.service({}, 2);
  check(F.Registry.discover(F.Caller, F.Context).empty(),
        "staged service was discoverable");
  check(!F.Registry.resolve(F.Caller, "sample.increment", 1, Error),
        "staged service resolved");
  F.activate();
  check(F.Registry.resolve(F.Caller, "sample.increment", 1, Error) == First &&
            F.Registry.resolve(F.Caller, "sample.increment", 2, Error) ==
                Second,
        "service version identity");
  check(!F.Registry.resolve(F.Caller, "sample.increment", 3, Error),
        "service version silently fell back");
  Json Result;
  check(F.Registry
            .invoke(
                F.Caller, First, {"", "1", "", F.Context, 41},
                [&](Json R) { Result = std::move(R); }, Error)
            .has_value(),
        "invoke service");
  check(Result["payload"] == 42 && Result["context"] == F.Context,
        "service output/context");
  check(!F.Registry.invoke(F.Caller, First, {"", "2", "", F.Context, "bad"}, {},
                           Error) &&
            Error.Code == "invalid_service_input",
        "invalid service input was accepted");
  auto Invalid =
      F.service([](const auto &,
                   auto Done) { Done({{"status", "ok"}, {"payload", "bad"}}); },
                4);
  F.Registry.invoke(
      F.Caller, Invalid, {"", "3", "", F.Context, 1},
      [&](Json R) { Result = std::move(R); }, Error);
  check(Result["error"]["code"] == "invalid_service_output",
        "invalid service result was accepted");
}
void contexts(Fixture &F) {
  Diagnostic Error;
  const auto S = F.service();
  F.activate();
  const auto Old = F.Context;
  F.Context["revision"] = "2";
  check(!F.Registry.invoke(F.Caller, S, {"", "1", "", Old, 1}, {}, Error) &&
            Error.Code == "stale_revision",
        "stale input silently used a new revision");
  auto Invalid = F.Context;
  Invalid["loaded"] = "yes";
  check(!F.Registry.invoke(F.Caller, S, {"", "1", "", Invalid, 1}, {}, Error),
        "malformed context accepted");
  const auto Other = owner(F.Host, F.Host.root(), OwnerKind::Project, "other");
  check(F.Registry.discover(Other, F.Context).empty(),
        "cross-project service discovery");
  check(!F.Registry.invoke(Other, S, {"", "1", "", F.Context, 1}, {}, Error) &&
            Error.Code == "project_mismatch",
        "cross-project service invocation");
  Executor Q;
  PluginHost OtherHost([&](auto Work) { Q.post(std::move(Work)); });
  check(!F.Host.contains(F.Project, OtherHost.root()),
        "foreign owner passed ancestry check");
}
void lifetime(Fixture &F) {
  Diagnostic Error;
  Services::Reply Pending;
  auto S =
      F.service([&](const auto &, auto Done) { Pending = std::move(Done); });
  F.activate();
  bool Notified = false, Closed = false;
  F.Registry.invoke(
      F.Caller, S, {"", "1", "", F.Context, 1}, [&](Json) { Notified = true; },
      Error);
  F.Host.close(F.Provider, [&](const auto &) { Closed = true; });
  F.Queue.drain();
  check(!Closed, "provider closed before its pending reply");
  check(!F.Registry.invoke(F.Caller, S, {"", "2", "", F.Context, 1}, {}, Error),
        "retired provider admitted call");
  F.Host.close(F.Caller);
  F.Queue.drain();
  Pending({{"status", "ok"}, {"payload", 2}});
  Pending({{"status", "ok"}, {"payload", 2}});
  F.Queue.drain();
  check(Closed && !Notified,
        "retired caller callback ran or provider did not drain");
  Pending = {};
}
void reentrant(Fixture &F) {
  Diagnostic Error;
  bool Closed = false;
  auto S = F.service([&](const auto &, auto Done) {
    Done({{"status", "ok"}, {"payload", 2}});
    F.Host.close(F.Provider, [&](const auto &) { Closed = true; });
    F.Queue.drain();
    check(!Closed,
          "provider disposed while its synchronous handler was running");
  });
  F.activate();
  F.Registry.invoke(
      F.Caller, S, {"", "1", "", F.Context, 1}, [](Json) {}, Error);
  F.Queue.drain();
  check(Closed, "synchronous handler lease did not drain");
}
void events(Fixture &F) {
  Diagnostic Error;
  auto Source =
      F.Bus.add(F.Provider, F.Project,
                {"sample.updated", "Update", 1, {{"type", "integer"}}}, Error);
  check(Source.has_value(), "add event source");
  int Calls = 0;
  auto Subscription = F.Bus.subscribe(
      F.Caller, *Source,
      [&](const Event &E) {
        check(E.Context["revision"] == "1" && E.Payload == 7,
              "event envelope changed after publication");
        ++Calls;
      },
      Error);
  check(Subscription.has_value(), "subscribe to staged source");
  check(!F.Bus.publish(*Source, F.Context, 7, Error),
        "staged source published");
  F.activate();
  check(!F.Bus.publish(*Source, F.Context, "bad", Error),
        "invalid event payload accepted");
  check(F.Bus.publish(*Source, F.Context, 7, Error) && Calls == 0,
        "event delivered inline");
  F.Context["revision"] = "2";
  F.Queue.drain();
  check(Calls == 1, "event not delivered");
  F.Bus.publish(*Source, F.Context, 8, Error);
  F.Host.unregister(*Subscription);
  F.Queue.drain();
  check(Calls == 1, "queued event ignored unsubscribe");
  const auto Other = owner(F.Host, F.Host.root(), OwnerKind::Project, "other");
  check(!F.Bus.subscribe(
            Other, *Source, [](const auto &) {}, Error),
        "cross-project event subscription");
}
void replacement(Fixture &F) {
  Diagnostic Error;
  auto Source = *F.Bus.add(F.Provider, F.Project,
                           {"sample.updated", "Update", 1, {}}, Error);
  F.activate();
  int Calls = 0;
  F.Bus.subscribe(F.Caller, Source, [&](const auto &) { ++Calls; }, Error);
  F.Bus.publish(Source, F.Context, 1, Error);
  F.Host.unregister(Source);
  F.Queue.drain();
  check(Calls == 0, "queued event revived retired source");
  const auto Next = *F.Bus.add(F.Provider, F.Project,
                               {"sample.updated", "Update", 1, {}}, Error);
  check(Next != Source && !F.Bus.publish(Source, F.Context, 2, Error),
        "old event source rebound");
  F.Bus.publish(Next, F.Context, 3, Error);
  F.Queue.drain();
  check(Calls == 0, "old subscriber rebound");
  bool Disposed = false;
  F.Host.registerResource(
      F.Caller, {"fixture", "guard", "v1"}, {},
      [&](auto Done) {
        Disposed = true;
        Done({});
      },
      Error);
  F.Bus.subscribe(
      F.Caller, Next,
      [&](const auto &) {
        F.Host.close(F.Caller);
        check(!Disposed, "subscriber cleanup ran inside its callback");
        ++Calls;
      },
      Error);
  F.Bus.publish(Next, F.Context, 4, Error);
  F.Queue.drain();
  check(Calls == 1 && Disposed, "subscriber callback lease did not drain");
}
void bridge(Fixture &F) {
  Diagnostic Error;
  int Queries = 0;
  HostServices Hub(
      F.Host, [&](auto Work) { F.Queue.post(std::move(Work)); },
      [&](const ServiceRequest &R, auto Done) {
        check(R.Operation == "metadata" && R.Context == F.Context,
              "query adapter lost captured input");
        ++Queries;
        Done({{"status", "ok"}, {"payload", 42}});
      });
  check(Hub.bind(F.Project, F.Context, true, Error), "bind project services");
  const auto Task = owner(F.Host, F.Caller, OwnerKind::Task, "task");
  Manifest M;
  M.Services = {{"project.context", 1},
                {"analysis.metadata", 1},
                {"analysis.metadata", 99}};
  M.Events = {{"project.revision_changed", 1}};
  Json Notification, Reply;
  auto Session = Hub.attach(Task, M, F.Context,
                            [&](Json R) { Notification = std::move(R); });
  auto Done = [&](Json R) { Reply = std::move(R); };
  Session->request("1", "services.list", Json::object(), Done);
  check(Reply["payload"]["missing"].size() == 1,
        "missing service version not reported");
  std::string Service;
  for (const auto &D : Reply["payload"]["services"])
    if (D["id"] == "analysis.metadata")
      Service = D["registration"];
  Session->request("2", "services.call",
                   {{"registration", Service}, {"payload", Json::object()}},
                   Done);
  check(Reply["payload"] == 42 && Queries == 1, "generic service not routed");
  Session->request("invalid", "services.call",
                   {{"registration", Service}, {"payload", 42}}, Done);
  check(Reply["error"]["code"] == "invalid_service_input" && Queries == 1,
        "analysis adapter silently converted a non-object payload");
  Session->request("3", "events.list", Json::object(), Done);
  std::string Event;
  for (const auto &D : Reply["payload"]["events"])
    if (D["id"] == "project.revision_changed")
      Event = D["registration"];
  Session->request("4", "events.subscribe", {{"registration", Event}}, Done);
  check(Reply["status"] == "ok", "bridge subscribe failed");
  auto New = F.Context;
  New["revision"] = "2";
  Hub.setContext(F.Project, New);
  F.Queue.drain();
  check(Notification["event"]["context"]["revision"] == "2",
        "context event not delivered");
  Session->request("5", "services.call",
                   {{"registration", Service}, {"payload", Json::object()}},
                   Done);
  check(Reply["error"]["code"] == "stale_revision" && Queries == 1,
        "event silently retargeted analysis input");
  Session->request("6", "events.unsubscribe",
                   {{"subscription", Notification["subscription"]}}, Done);
  Notification = Json();
  Hub.setContext(F.Project, F.Context);
  F.Queue.drain();
  check(Notification.is_null(), "bridge unsubscribe did not remove callback");
  Session->request("7", "services.call",
                   {{"registration", "unknown"}, {"payload", Json::object()}},
                   Done);
  check(Reply["error"]["code"] == "service_undeclared",
        "forged service binding accepted");
}
} // namespace
int main(int Argc, char **Argv) {
  try {
    if (Argc != 2)
      throw std::runtime_error("case required");
    Fixture F;
    const std::string Case = Argv[1];
    if (Case == "services")
      services(F);
    else if (Case == "contexts")
      contexts(F);
    else if (Case == "lifetime")
      lifetime(F);
    else if (Case == "reentrant")
      reentrant(F);
    else if (Case == "events")
      events(F);
    else if (Case == "replacement")
      replacement(F);
    else if (Case == "bridge")
      bridge(F);
    else
      throw std::runtime_error("unknown case");
    std::cout << Case << " services/events passed\n";
    return 0;
  } catch (const std::exception &Error) {
    std::cerr << Error.what() << '\n';
    return 1;
  }
}
