//===- ToolsTests.cpp - Frozen tool and action contracts ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe dispatch, generation, context and receipt behavior independently.
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Executor.h"
#include "neverd/plugin/Schema.h"
#include "neverd/tools/ToolRegistry.h"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace neverd::plugin;
using namespace neverd::tools;
namespace {
void check(bool Value, const char *Message) {
  if (!Value)
    throw std::runtime_error(Message);
}
Json numberSchema() { return {{"type", "integer"}}; }
struct Fixture {
  Executor Queue;
  PluginHost Host{[&](auto Work) { Queue.post(std::move(Work)); }};
  Actions ActionsService{Host};
  Diagnostic Error;
  Owner Project, Component;
  Environment Env;
  bool Allowed = true;
  std::function<void()> PolicyCallback;
  ToolRegistry Tools{Host, ActionsService, [&] { return Env; },
                     [&](const auto &, const auto &) {
                       const auto Callback = PolicyCallback;
                       if (Callback)
                         Callback();
                       return Allowed
                                  ? Diagnostic{}
                                  : Diagnostic{"policy_denied", "Not allowed"};
                     }};
  Fixture() {
    Project = scope(Host.root(), "project", OwnerKind::Project);
    Component = scope(Project, "plugin");
    Env.Project = Project;
    Env.Context = {{"loaded", true},
                   {"project_id", "p"},
                   {"epoch", "1"},
                   {"revision", "0"},
                   {"profile", "default"}};
  }
  ~Fixture() {
    Host.close(Host.root());
    Queue.drain();
  }
  Owner scope(Owner Parent, const std::string &Name,
              OwnerKind Kind = OwnerKind::Component, bool Active = true) {
    const auto S = Host.createOwner(Parent, Kind, Name, Error);
    check(S.has_value(), "owner creation");
    if (Active)
      check(Host.activate(*S, Error), "owner activation");
    return *S;
  }
  ActionDefinition definition(const std::string &Id) {
    ActionDefinition A;
    A.PluginId = "sample";
    A.Id = Id;
    A.Version = "1";
    A.Title = "A useful action";
    A.OutputSchema = numberSchema();
    A.Parameters = Json::array({{{"id", "count"},
                                 {"title", "Count"},
                                 {"type", "integer"},
                                 {"default", 3},
                                 {"minimum", 1},
                                 {"maximum", 10}}});
    return A;
  }
  Registration action(const std::string &Id, Actions::Handler Run,
                      Owner Scope = {}) {
    auto R = ActionsService.add(Scope.id() ? Scope : Component, definition(Id),
                                std::move(Run), Error);
    check(R.has_value(), "action registration");
    return *R;
  }
  Registration tool(const std::string &Id, Registration Action,
                    std::optional<Registration> Parent = {}) {
    auto R = Tools.add({Id, Action, Parent}, Error);
    check(R.has_value(), "tool registration");
    return *R;
  }
  ResolvedTools resolve(const std::string &Step = "step") {
    auto Set =
        Tools.resolve("consumer", Step, Json::object(), Json::object(), Error);
    check(Set.has_value(), "tool resolution");
    return *Set;
  }
  std::string call(const ResolvedTools &Set, Registration Tool,
                   const std::string &Id = "call", Json Params = Json::object(),
                   const std::string &Parent = {}) {
    auto Result = Tools.call(Set, Tool, Id, Params, Parent, Error);
    check(Result.has_value(), Error.Message.c_str());
    return *Result;
  }
};
void dispatch() {
  Fixture F;
  int Runs = 0;
  auto A = F.action("count", [&](auto C) {
    ++Runs;
    C->succeed(C->request().Parameters["count"]);
  });
  auto T = F.tool("count", A);
  const auto Set = F.resolve();
  check(describeTool(Set.tools()[0])["input_schema"]["required"].empty(),
        "schema required a parameter that the resolver defaults");
  const auto Op = F.call(Set, T);
  check(Runs == 1 && F.Tools.operation(Op)->Result.Data == 3,
        "actual default dispatch");
  check(F.Tools.operation(Op)->Result.Phase == ActionPhase::Succeeded,
        "synchronous result");
  auto Custom = F.Tools.resolve(
      "consumer", "configured", {{"sample", {{"count", {{"count", 4}}}}}},
      {{"sample", {{"count", {{"count", 5}}}}}}, F.Error);
  check(Custom.has_value(), "resolve configured step");
  check(describeTool(Custom->tools()[0])["input_schema"]["properties"]["count"]
                                        ["default"] == 5,
        "effective defaults not published to consumer");
  check(F.Tools.operation(F.call(*Custom, T, "explicit", {{"count", 6}}))
                ->Result.Data == 6,
        "explicit parameter precedence");
  check(!F.Tools.call(Set, T, "bad", {{"count", nullptr}}, "", F.Error),
        "null input admitted");
  check(!F.Tools.call(Set, T, "extra", {{"other", 1}}, "", F.Error),
        "unknown input admitted");
}
void asyncCompletion() {
  Fixture F;
  std::shared_ptr<ActionCall> Pending;
  auto A = F.action("async", [&](auto C) {
    Pending = C;
    C->progress({{"done", 1}});
  });
  auto T = F.tool("async", A);
  auto Set = F.resolve();
  auto Id = F.call(Set, T);
  check(F.Tools.operation(Id)->Result.Progress["done"] == 1, "lost progress");
  Set.release();
  check(!F.Tools.call(Set, T, "new", Json::object(), "", F.Error),
        "released set invoked");
  check(F.Tools.operation(Id)->Result.Phase == ActionPhase::Running,
        "release cancelled existing operation");
  Pending->succeed(9);
  F.Queue.drain();
  check(F.Tools.operation(Id)->Result.Data == 9,
        "completion lost after release");
  Pending->succeed(1);
  check(F.Tools.operation(Id)->Result.Data == 9,
        "duplicate completion rewrote output");
}
void replacement() {
  Fixture F;
  int Old = 0, New = 0;
  auto A = F.action("same", [&](auto C) {
    ++Old;
    C->succeed(1);
  });
  auto T = F.tool("same", A);
  const auto Set = F.resolve();
  F.Host.unregister(T);
  F.Host.unregister(A);
  auto B = F.action("same", [&](auto C) {
    ++New;
    C->succeed(2);
  });
  auto U = F.tool("same", B);
  F.Host.unregister(T);
  check(!F.Tools.call(Set, T, "old", Json::object(), "", F.Error),
        "old set retargeted replacement");
  auto Current = F.resolve("new");
  F.call(Current, U);
  check(Old == 0 && New == 1, "replacement dispatch identity");
}
void activation() {
  Fixture F;
  const auto C = F.scope(F.Project, "staging", OwnerKind::Component, false);
  auto A = F.action("one", [](auto C) { C->succeed(1); }, C);
  F.tool("one", A);
  auto B = F.action("two", [](auto C) { C->succeed(2); }, C);
  F.tool("two", B);
  check(F.resolve().tools().empty(), "staged tool published");
  check(!F.Tools.add({"two", B, {}}, F.Error), "duplicate third tool accepted");
  F.Host.failActivation(C, "third tool failed");
  F.Queue.drain();
  check(F.resolve().tools().empty(), "failed component leaked partial tools");
  auto Good = F.action("independent", [](auto C) { C->succeed(7); });
  F.tool("independent", Good);
  check(F.resolve().tools().size() == 1, "independent component affected");
}
void context() {
  Fixture F;
  auto Def = F.definition("needs");
  Def.Services = {"disasm"};
  auto A = F.ActionsService.add(
      F.Component, Def, [](auto C) { C->succeed(1); }, F.Error);
  auto T = F.tool("needs", *A);
  auto Set = F.resolve();
  check(Set.tools()[0].Unavailable.Code == "service_unavailable",
        "missing capability not explained");
  check(!F.Tools.call(Set, T, "missing", Json::object(), "", F.Error),
        "missing capability executed");
  F.Env.Services.insert("disasm");
  Set = F.resolve();
  F.Allowed = false;
  check(!F.Tools.call(Set, T, "policy", Json::object(), "", F.Error) &&
            F.Error.Code == "policy_denied",
        "current policy not rechecked");
  F.Allowed = true;
  F.Env.Context["revision"] = "1";
  check(!F.Tools.call(Set, T, "stale", Json::object(), "", F.Error) &&
            F.Error.Code == "context_changed",
        "stale context accepted");
  const auto Other = F.scope(F.Host.root(), "other", OwnerKind::Project);
  F.Env.Project = Other;
  check(F.resolve().tools().empty(), "cross-project tool discovery");
}
void continuation() {
  Fixture F;
  auto A = F.action("report", [](auto C) { C->succeed(42); });
  auto B = F.action("apply", [](auto C) {
    C->succeed(C->request().Continuation.at("data"));
  });
  auto T = F.tool("report", A);
  auto U = F.tool("apply", B, A);
  const auto Parent = F.call(F.resolve(), T);
  const auto Apply =
      F.call(F.resolve("followup"), U, "apply", Json::object(), Parent);
  check(F.Tools.operation(Apply)->Result.Data == 42,
        "explicit follow-up lost parent data");
  F.Env.Context["revision"] = "1";
  // A fresh step at r1 cannot bless a report originally made at r0.
  const auto Fresh = F.resolve("after-edit");
  check(!F.Tools.call(Fresh, U, "stale-report", Json::object(), Parent,
                      F.Error) &&
            F.Error.Code == "context_changed",
        "new context laundered old report");
  check(!F.Tools.call(Fresh, U, "missing", Json::object(), "unknown", F.Error),
        "unknown report accepted");
}
void deduplication() {
  Fixture F;
  int Runs = 0;
  std::shared_ptr<ActionCall> Pending;
  auto A = F.action("once", [&](auto C) {
    ++Runs;
    Pending = C;
  });
  auto T = F.tool("once", A);
  auto Set = F.resolve();
  const auto Id = F.call(Set, T, "key");
  check(F.call(Set, T, "key", {{"count", 3}}) == Id && Runs == 1,
        "running call executed twice");
  check(!F.Tools.call(Set, T, "key", {{"count", 4}}, "", F.Error) &&
            F.Error.Code == "call_conflict",
        "mismatched repeated parameters accepted");
  Pending->succeed(3);
  F.Env.Context["revision"] = "1";
  check(F.call(Set, T, "key") == Id && Runs == 1,
        "receipt query retried after edit");
  F.Env.Context["revision"] = "0";
  const auto Again = F.call(Set, T, "new-key");
  check(Again != Id && Runs == 2, "new explicit request over-deduplicated");
  Pending->succeed(4);
  check(!F.Tools.operation("op-unknown"), "missing outcome invented");
}
void retirement() {
  Fixture F;
  bool Cancelled = false, Closed = false;
  std::shared_ptr<ActionCall> Pending;
  auto A = F.action("long", [&](auto C) {
    Pending = C;
    C->onCancel([&] { Cancelled = true; });
  });
  auto T = F.tool("long", A);
  auto Set = F.resolve();
  auto Id = F.call(Set, T);
  F.Host.close(F.Component, [&](const auto &) { Closed = true; });
  F.Queue.drain();
  check(Cancelled && !Closed, "retirement did not await actual completion");
  check(!F.Tools.call(Set, T, "later", Json::object(), "", F.Error),
        "retired tool executed");
  Pending->fail({"cancelled", "provider exited"}, true);
  F.Queue.drain();
  check(Closed && F.Tools.operation(Id)->Result.Phase == ActionPhase::Cancelled,
        "retirement did not drain");
}
void acceptedReceipt() {
  Fixture F;
  std::shared_ptr<ActionCall> Pending;
  auto A = F.action("write", [&](auto C) {
    Pending = C;
    C->onCancel([] {});
  });
  auto T = F.tool("write", A);
  auto Set = F.resolve();
  auto Id = F.call(Set, T);
  F.Tools.cancel(Id);
  F.Tools.setObserver({});
  Set.release();
  check(F.Tools.operation(Id)->Result.Phase == ActionPhase::Cancelling,
        "cancel invented terminal receipt");
  Pending->receipt({{"status", "ok"}, {"revision", "1"}, {"saved", true}});
  Pending->fail({"cancelled", "provider exited"}, true);
  auto Result = F.Tools.operation(Id)->Result;
  check(Result.CancelRequested && Result.Receipts.at(0).at("saved") == true,
        "accepted write receipt erased by cancellation/disconnect");
}
void taskTool() {
  Fixture F;
  const auto Task = F.scope(F.Component, "temporary", OwnerKind::Task);
  auto A = F.action("temporary", [](auto C) { C->succeed(8); }, Task);
  auto T = F.tool("temporary", A);
  auto Set = F.resolve();
  F.Host.close(Task);
  F.Queue.drain();
  check(!F.Tools.call(Set, T, "late", Json::object(), "", F.Error),
        "temporary task tool survived closure");
  check(F.Host.forget(Task), "task not closed");
  check(F.resolve().tools().empty(), "forgotten task kept provider closures");
}
void schemas() {
  Fixture F;
  auto Def = F.definition("invalid");
  Def.OutputSchema = {{"type", "object"}, {"$ref", "unimplemented"}};
  check(!F.ActionsService.add(
            F.Component, Def, [](auto) {}, F.Error),
        "unsupported schema silently accepted");
  auto A =
      F.action("wrong-output", [](auto C) { C->succeed("not an integer"); });
  auto T = F.tool("wrong-output", A);
  auto Id = F.call(F.resolve(), T);
  check(F.Tools.operation(Id)->Result.Error.Code == "invalid_output",
        "output schema unchecked");
  const Json Schema{
      {"type", "object"},
      {"properties",
       {{"list", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}},
      {"required", {"list"}},
      {"additionalProperties", false}};
  std::string Why;
  check(validateSchema(Schema, Why) &&
            validateValue(Schema, {{"list", {1, 2}}}, Why),
        "structured schema unsupported");
  check(!validateValue(Schema, {{"list", {1, "bad"}}}, Why),
        "nested output unchecked");
  Json Text = Json::array({{{"id", "content"},
                            {"title", "Report text"},
                            {"type", "string"},
                            {"default", ""}}});
  const std::string Long(5000, 'a');
  check(!validateParameters(Text, {{"content", Long}}, Why),
        "legacy text limit changed");
  check(parameterSchema(Text)["properties"]["content"]["maxLength"] == 4096,
        "default string limit hidden from tool consumers");
  Text[0]["maxLength"] = 6000;
  auto TextSchema = parameterSchema(Text);
  check(validateParameterFields(Text, Why) && validateSchema(TextSchema, Why) &&
            validateParameters(Text, {{"content", Long}}, Why) &&
            validateValue(TextSchema, {{"content", Long}}, Why),
        "declared long-text parameter rejected");
  check(!validateParameters(Text, {{"content", std::string(6001, 'a')}}, Why) &&
            !validateValue(TextSchema, {{"content", std::string(6001, 'a')}},
                           Why),
        "declared text bound not enforced");
  Text[0]["maxLength"] = 1;
  check(validateParameters(Text, {{"content", "\xF0\x9F\x98\x80"}}, Why) &&
            validateValue(parameterSchema(Text),
                          {{"content", "\xF0\x9F\x98\x80"}}, Why),
        "explicit JSON Schema string length must count Unicode code points");
  Text[0]["maxLength"] = -1;
  check(!validateParameterFields(Text, Why), "negative text bound admitted");
  Json Structured = Json::array({{{"id", "arguments"},
                                  {"title", "Service arguments"},
                                  {"type", "object"},
                                  {"default", Json::object()}}});
  const Json Payload{
      {"arguments",
       {{"address", "0x1000"}, {"limit", 20}, {"nested", {1, 2}}}}};
  check(validateParameterFields(Structured, Why) &&
            validateParameters(Structured, Payload, Why) &&
            validateSchema(parameterSchema(Structured), Why) &&
            validateValue(parameterSchema(Structured), Payload, Why),
        "structured service payload rejected");
  check(!validateParameters(Structured, {{"arguments", "{\"limit\":20}"}}, Why),
        "object parameter accepted a double-encoded string");
  const Json OpenObject{{"type", "object"},
                        {"properties", Json::object()},
                        {"additionalProperties", true}};
  check(validateSchema(OpenObject, Why) &&
            validateValue(OpenObject, Payload, Why),
        "explicit open result schema rejected");
}
void foreignTokens() {
  Fixture F, G;
  auto A = F.action("same", [](auto C) { C->succeed(1); });
  auto B = G.action("same", [](auto C) { C->succeed(2); });
  check(A.id() == B.id() && A != B, "host identity not part of token");
  check(!F.ActionsService.definition(B),
        "foreign token resolved by numeric ID");
  auto T = F.tool("same", A), U = G.tool("same", B);
  check(!F.Tools.call(F.resolve(), U, "foreign", Json::object(), "", F.Error),
        "foreign tool dispatched");
  (void)T;
}
void reentrantCancel() {
  Fixture F;
  bool Cancelled = false;
  std::shared_ptr<ActionCall> Pending;
  auto A = F.action("progress", [&](auto C) {
    Pending = C;
    C->onCancel([&] { Cancelled = true; });
    C->progress({{"done", 1}});
    check(C->snapshot().CancelRequested,
          "handler did not observe reentrant cancellation");
  });
  auto T = F.tool("progress", A);
  F.Tools.setObserver([&](const auto &Op) {
    if (Op.Result.Phase == ActionPhase::Running)
      F.Tools.cancel(Op.Id);
  });
  F.call(F.resolve(), T);
  check(Cancelled, "cancel during first synchronous progress was lost");
  Pending->fail({"cancelled", "cancelled"}, true);
  Cancelled = false;
  F.Tools.setObserver([&](const auto &Op) {
    if (Op.Result.Phase == ActionPhase::Running)
      F.Host.unregister(T);
  });
  F.call(F.resolve("retire"), T);
  check(Cancelled, "tool retirement during initial progress was lost");
  Pending->fail({"cancelled", "cancelled"}, true);
}
void reentrantAdmission() {
  Fixture F;
  int Runs = 0;
  auto A = F.action("policy", [&](auto C) {
    ++Runs;
    C->succeed(1);
  });
  auto T = F.tool("policy", A);
  auto Set = F.resolve();
  std::string Inner;
  F.PolicyCallback = [&] {
    F.PolicyCallback = {};
    Inner = F.call(Set, T, "same-key");
  };
  const auto Outer = F.call(Set, T, "same-key");
  check(Outer == Inner && Runs == 1,
        "reentrant policy duplicated the same call");
  auto Released = F.resolve("release");
  F.PolicyCallback = [&] { Released.release(); };
  check(!F.Tools.call(Released, T, "released", Json::object(), "", F.Error) &&
            F.Error.Code == "set_released",
        "policy released set still executed");
  F.PolicyCallback = {};
  bool Closed = false;
  F.Tools.setObserver([&](const auto &Op) {
    if (terminal(Op.Result.Phase)) {
      F.Host.close(F.Component, [&](const auto &) { Closed = true; });
      F.Queue.drain();
      check(!Closed, "module closed while synchronous handler was on stack");
    }
  });
  F.call(F.resolve("stack"), T);
  F.Queue.drain();
  check(Closed, "synchronous handler lease was not released");
}
} // namespace
int main(int Argc, char **Argv) {
  try {
    const std::string Name = Argc > 1 ? Argv[1] : "";
    if (Name == "dispatch")
      dispatch();
    else if (Name == "async")
      asyncCompletion();
    else if (Name == "replacement")
      replacement();
    else if (Name == "activation")
      activation();
    else if (Name == "context")
      context();
    else if (Name == "continuation")
      continuation();
    else if (Name == "deduplication")
      deduplication();
    else if (Name == "retirement")
      retirement();
    else if (Name == "receipt")
      acceptedReceipt();
    else if (Name == "task")
      taskTool();
    else if (Name == "schemas")
      schemas();
    else if (Name == "foreign")
      foreignTokens();
    else if (Name == "reentrant")
      reentrantCancel();
    else if (Name == "admission")
      reentrantAdmission();
    else
      throw std::runtime_error("unknown test");
    std::cout << Name << " passed\n";
    return 0;
  } catch (const std::exception &Error) {
    std::cerr << Error.what() << '\n';
    return 1;
  }
}
