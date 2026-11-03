//===- PluginHostTests.cpp - Plugin lifetime contracts --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent lifecycle observations across activation, retirement and reuse.
///
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Executor.h"
#include "neverd/plugin/Manifest.h"
#include "neverd/plugin/PluginHost.h"
#include "neverd/plugin/Tasks.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using namespace neverd::plugin;

namespace {
void check(bool Value, const char *Message) {
  if (!Value)
    throw std::runtime_error(Message);
}

struct Fixture {
  Executor Queue;
  PluginHost Host{[this](auto Work) { Queue.post(std::move(Work)); }};
  Diagnostic Error;
  ~Fixture() {
    Host.close(Host.root());
    Queue.drain();
  }
  Owner scope(Owner Parent, const std::string &Name, bool Active = true) {
    auto Result = Host.createOwner(Parent, OwnerKind::Component, Name, Error);
    check(Result.has_value(), "create scope");
    if (Active)
      check(Host.activate(*Result, Error), "activate scope");
    return *Result;
  }
  Registration resource(Owner Scope, const std::string &Name,
                        PluginHost::Work Retire = {},
                        PluginHost::Cleanup Dispose = {}) {
    auto R =
        Host.registerResource(Scope, {"action", Name, "v1"}, std::move(Retire),
                              std::move(Dispose), Error);
    check(R.has_value(), "register resource");
    return *R;
  }
};

void activation() {
  Fixture F;
  auto C = F.scope(F.Host.root(), "component", false);
  int Disposed = 0;
  auto First = F.resource(C, "first", {}, [&](auto Done) {
    ++Disposed;
    Done({});
  });
  F.resource(C, "second");
  check(!F.Host.find(C, "action", "first"), "staged definition leaked");
  check(!F.Host.invoke(First, F.Error), "staged invocation admitted");
  check(!F.Host.registerResource(C, {"action", "first", "v1"}, {}, {}, F.Error),
        "duplicate registration admitted");
  F.Host.failActivation(C, "third registration failed");
  F.Queue.drain();
  check(Disposed == 1, "partial activation was not cleaned");
  check(F.Host.snapshot(C)->Phase == OwnerPhase::Closed,
        "activation not closed");
  check(!F.Host.snapshot(C)->ActivationFailure.empty(),
        "activation cause lost");
  check(!F.Host.activate(C, F.Error), "failed generation reactivated");
  auto Next = F.scope(F.Host.root(), "component", false);
  F.resource(Next, "first");
  check(F.Host.activate(Next, F.Error), "new generation did not activate");
  check(F.Host.find(Next, "action", "first").has_value(),
        "published item missing");
}

void retirement() {
  Fixture F;
  auto C = F.scope(F.Host.root(), "component");
  auto Child = F.scope(C, "task");
  bool Retired = false, Disposed = false, Closed = false;
  Registration R;
  R = F.resource(
      Child, "work",
      [&] {
        Retired = true;
        check(!F.Host.invoke(R, F.Error),
              "retiring callback reopened admission");
        check(!F.Host.createOwner(C, OwnerKind::Task, "late", F.Error),
              "created child under retiring ancestor");
      },
      [&](auto Done) {
        Disposed = true;
        Done({});
      });
  auto Lease = F.Host.invoke(R, F.Error);
  check(bool(Lease), "invocation not admitted");
  F.Host.close(C, [&](const auto &Errors) {
    check(Errors.empty(), "unexpected retirement failure");
    Closed = true;
  });
  F.Queue.drain();
  check(Retired && !Disposed && !Closed, "closed before invocation finished");
  check(F.Host.snapshot(C)->Phase == OwnerPhase::Retiring,
        "missing retiring state");
  Lease.reset();
  F.Queue.drain();
  check(Disposed && Closed, "retirement did not drain");
}

void replacement() {
  Fixture F;
  auto C = F.scope(F.Host.root(), "component");
  auto Old = F.resource(C, "report");
  auto Lease = F.Host.invoke(Old, F.Error);
  check(F.Host.unregister(Old), "unregister failed");
  auto New = F.resource(C, "report");
  check(Old.id() != New.id(), "identity reused");
  F.Host.unregister(Old);
  check(!F.Host.invoke(Old, F.Error), "old binding called replacement");
  auto Current = F.Host.invoke(New, F.Error);
  check(bool(Current), "old token removed replacement");
  Lease.reset();
  Current.reset();
  F.Queue.drain();
}

void cleanup() {
  Fixture F;
  auto C = F.scope(F.Host.root(), "component");
  PluginHost::CleanupDone Pending;
  bool Other = false;
  int Completions = 0;
  F.resource(C, "slow", {}, [&](auto Done) { Pending = std::move(Done); });
  F.resource(C, "throw", [] { throw std::runtime_error("stop failed"); });
  F.resource(C, "other", {}, [&](auto Done) {
    Other = true;
    Done("cleanup failed");
  });
  auto Closed = [&](const auto &Errors) {
    check(Errors.size() == 2, "cleanup diagnostics missing");
    ++Completions;
  };
  F.Host.close(C, Closed);
  F.Host.close(C, Closed);
  F.Queue.drain();
  check(Other && Completions == 0, "cleanup skipped or completion premature");
  Pending({});
  Pending({});
  Pending = {};
  F.Queue.drain();
  check(Completions == 2, "close waiters not completed once each");
}

void projects() {
  Fixture F;
  auto A = F.scope(F.Host.root(), "project A");
  auto B = F.scope(F.Host.root(), "project B");
  auto CA = F.scope(A, "same plugin");
  auto CB = F.scope(B, "same plugin");
  auto RA = F.resource(CA, "report");
  auto RB = F.resource(CB, "report");
  F.Host.close(A);
  F.Queue.drain();
  check(!F.Host.invoke(RA, F.Error), "closed project still accepts work");
  auto L = F.Host.invoke(RB, F.Error);
  check(bool(L), "closing A affected B");
  Fixture Other;
  check(!Other.Host.invoke(RB, Other.Error), "foreign registration accepted");
  check(!Other.Host.createOwner(B, OwnerKind::Task, "foreign", Other.Error),
        "foreign owner accepted");
  L.reset();
  F.Queue.drain();
}

void dispatch() {
  Fixture F;
  auto C = F.scope(F.Host.root(), "component");
  bool Disposed = false;
  auto Control = std::this_thread::get_id();
  auto R = F.resource(C, "worker", {}, [&](auto Done) {
    check(std::this_thread::get_id() == Control, "cleanup on foreign thread");
    Disposed = true;
    Done({});
  });
  auto Lease = F.Host.invoke(R, F.Error);
  F.Host.close(C);
  std::thread Worker([Lease = std::move(Lease)]() mutable { Lease.reset(); });
  Worker.join();
  check(!Disposed, "release bypassed control sequence");
  F.Queue.drain();
  check(Disposed, "release was not delivered");
}

void disposalBarrier() {
  Fixture F;
  auto C = F.scope(F.Host.root(), "component");
  bool Destroyed = false;
  F.resource(C, "instance", {}, [&](auto Done) {
    Destroyed = true;
    Done({});
  });
  auto Action = F.resource(C, "action");
  auto Call = F.Host.invoke(Action, F.Error);
  auto Child = F.scope(C, "task");
  PluginHost::CleanupDone ChildDone;
  auto Work = F.resource(Child, "execution", {},
                         [&](auto Done) { ChildDone = std::move(Done); });
  auto Task = F.Host.invoke(Work, F.Error);
  F.Host.close(C);
  F.Queue.drain();
  check(!Destroyed && !ChildDone, "instance destroyed with live child");
  Task.reset();
  F.Queue.drain();
  check(!Destroyed && bool(ChildDone), "child cleanup bypassed barrier");
  ChildDone({});
  ChildDone = {};
  F.Queue.drain();
  check(!Destroyed, "instance destroyed with live sibling action");
  Call.reset();
  F.Queue.drain();
  check(Destroyed && F.Host.snapshot(C)->Phase == OwnerPhase::Closed,
        "instance did not close after scope drained");
}

void lateCleanup() {
  Fixture F;
  auto C = F.scope(F.Host.root(), "component");
  PluginHost::CleanupDone Done;
  F.resource(C, "instance", {},
             [&](auto Callback) { Done = std::move(Callback); });
  F.Host.close(C,
               [&](const auto &) { check(F.Host.forget(C), "forget failed"); });
  Done({});
  F.Queue.drain();
  auto Next = F.scope(F.Host.root(), "component");
  auto Action = F.resource(Next, "instance");
  Done("late duplicate");
  Done = {};
  F.Queue.drain();
  auto Lease = F.Host.invoke(Action, F.Error);
  check(bool(Lease), "late cleanup touched replacement");
  check(F.Host.snapshot(Next)->CleanupErrors.empty(), "late diagnostic leaked");
  Lease.reset();
  F.Queue.drain();
}

void taskCompletion() {
  Fixture F;
  auto C = F.scope(F.Host.root(), "provider");
  auto Action = F.resource(C, "report");
  int Terminals = 0, Receipts = 0;
  Tasks T(
      F.Host, {},
      [&](const auto &Info) {
        if (terminal(Info.Phase)) {
          check(Receipts > Terminals, "published before receipt");
          ++Terminals;
        }
      },
      {},
      [&](const auto &) {
        ++Receipts;
        return std::string();
      });
  check(T.enqueue("ok", Action, F.Error), "enqueue failed");
  check(T.begin("ok") && T.hello("ok") && T.result("ok"), "protocol failed");
  check(!terminal(T.snapshot("ok")->Phase), "result accepted as process exit");
  T.finish("ok", true);
  T.finish("ok", false, "duplicate exit");
  check(T.snapshot("ok")->Phase == TaskPhase::Succeeded && Terminals == 1,
        "duplicate completion changed outcome");
  check(T.enqueue("crash", Action, F.Error), "second enqueue failed");
  T.begin("crash");
  T.hello("crash");
  T.result("crash");
  T.finish("crash", false);
  check(T.snapshot("crash")->Phase == TaskPhase::Failed, "crash accepted");
  F.Queue.drain();
  check(T.erase("ok") && T.erase("crash"), "completed history not reclaimed");
}

void taskRetirement() {
  Fixture F;
  auto C = F.scope(F.Host.root(), "provider");
  auto Action = F.resource(C, "report");
  int Stopped = 0;
  Tasks T(F.Host, [&](const auto &) { ++Stopped; }, {}, {2, 3});
  check(T.enqueue("running", Action, F.Error), "enqueue running");
  check(T.enqueue("queued", Action, F.Error), "enqueue queued");
  check(!T.enqueue("full", Action, F.Error), "queue limit ignored");
  T.begin("running");
  T.hello("running");
  bool Closed = false;
  F.Host.close(C, [&](const auto &) { Closed = true; });
  F.Queue.drain();
  check(Stopped == 1 && !Closed, "retirement did not await process exit");
  check(T.snapshot("queued")->Phase == TaskPhase::Cancelled,
        "queued task survived owner retirement");
  check(T.snapshot("running")->Phase == TaskPhase::Cancelling,
        "running task completed before exit");
  check(!T.enqueue("late", Action, F.Error), "retired action admitted work");
  T.finish("running", true);
  F.Queue.drain();
  check(Closed && T.snapshot("running")->Phase == TaskPhase::Cancelled,
        "exit overrode cancellation");
}

void receiptFailure() {
  Fixture F;
  auto C = F.scope(F.Host.root(), "provider");
  auto Action = F.resource(C, "report");
  bool SuccessObserved = false;
  Tasks T(
      F.Host, {},
      [&](const auto &Info) {
        SuccessObserved |= Info.Phase == TaskPhase::Succeeded;
      },
      {}, [](const auto &) { return std::string("disk full"); });
  T.enqueue("write", Action, F.Error);
  T.begin("write");
  T.hello("write");
  T.result("write");
  T.finish("write", true);
  check(!SuccessObserved && T.snapshot("write")->Phase == TaskPhase::Failed &&
            T.snapshot("write")->Diagnostic == "disk full",
        "receipt failure hidden");
  F.Queue.drain();
}

void configuration() {
  const Json Fields = Json::array({{{"id", "count"},
                                    {"title", "Count"},
                                    {"type", "integer"},
                                    {"minimum", 1},
                                    {"maximum", 100},
                                    {"default", 3}},
                                   {{"id", "title"},
                                    {"title", "Title"},
                                    {"type", "string"},
                                    {"default", "base"}}});
  const Json Empty = Json::object();
  const Json User{{"count", 4}}, Project{{"count", 5}, {"title", "project"}},
      Explicit{{"count", 6}};
  std::string Error;
  const auto Values = resolveParameters(Fields, User, Project, Explicit, Error);
  check(Values && Values->at("count") == 6 && Values->at("title") == "project",
        "configuration precedence diverged");
  check(resolveParameters(Fields, Empty, Empty, Empty, Error)->at("count") == 3,
        "default not applied");
  check(!resolveParameters(Fields, User, Project, {{"count", nullptr}}, Error),
        "null treated as inheritance");
  check(
      !resolveParameters(Fields, {{"count", "bad"}}, Project, Explicit, Error),
      "invalid saved layer hidden by later override");
  check(!resolveParameters(Fields, User, Project, {{"unknown", 1}}, Error),
        "unknown explicit field accepted");
  check(!resolveParameters(Fields, Json::array(), Empty, Empty, Error),
        "array layer accepted");
  check(User.at("count") == 4 && Project.at("count") == 5,
        "configuration resolver modified a source layer");
}
} // namespace

int main(int Argc, char **Argv) {
  try {
    check(Argc == 2, "expected case name");
    const std::string Name = Argv[1];
    if (Name == "activation")
      activation();
    else if (Name == "retirement")
      retirement();
    else if (Name == "replacement")
      replacement();
    else if (Name == "cleanup")
      cleanup();
    else if (Name == "projects")
      projects();
    else if (Name == "dispatch")
      dispatch();
    else if (Name == "disposal_barrier")
      disposalBarrier();
    else if (Name == "late_cleanup")
      lateCleanup();
    else if (Name == "task_completion")
      taskCompletion();
    else if (Name == "task_retirement")
      taskRetirement();
    else if (Name == "receipt_failure")
      receiptFailure();
    else if (Name == "configuration")
      configuration();
    else
      throw std::runtime_error("unknown test case");
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 1;
  }
}
