//===- Tasks.cpp - Shared plugin task lifecycle ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Serial admission and asynchronous task cancellation through NPH owners.
///
//===----------------------------------------------------------------------===//

#include "neverd/plugin/Tasks.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace neverd::plugin {

const char *taskPhaseName(TaskPhase Phase) {
  switch (Phase) {
  case TaskPhase::Queued:
    return "queued";
  case TaskPhase::Starting:
    return "starting";
  case TaskPhase::Running:
    return "running";
  case TaskPhase::Cancelling:
    return "cancelling";
  case TaskPhase::Succeeded:
    return "succeeded";
  case TaskPhase::Failed:
    return "failed";
  case TaskPhase::Cancelled:
    return "cancelled";
  case TaskPhase::Stale:
    return "stale";
  case TaskPhase::TimedOut:
    return "timed_out";
  }
  throw std::logic_error("Unknown plugin task phase");
}

bool terminal(TaskPhase Phase) {
  return Phase != TaskPhase::Queued && Phase != TaskPhase::Starting &&
         Phase != TaskPhase::Running && Phase != TaskPhase::Cancelling;
}

struct Tasks::Impl : std::enable_shared_from_this<Tasks::Impl> {
  struct Task {
    uint64_t Order = 0;
    TaskSnapshot Info;
    Owner Scope;
    Registration Execution;
    Invocation ActionLease;
    Invocation ExecutionLease;
    TaskPhase StopOutcome = TaskPhase::Cancelled;
  };
  PluginHost &Host;
  Stop StopProcess;
  Notify Changed;
  Commit SaveReceipt;
  TaskLimits Limits;
  std::map<std::string, Task> Records;
  std::string Running;
  uint64_t NextOrder = 0;

  Impl(PluginHost &Host, Stop StopProcess, Notify Changed, TaskLimits Limits,
       Commit SaveReceipt)
      : Host(Host), StopProcess(std::move(StopProcess)),
        Changed(std::move(Changed)), SaveReceipt(std::move(SaveReceipt)),
        Limits(Limits) {
    if (!Limits.MaxQueued || Limits.MaxHistory < Limits.MaxQueued)
      throw std::invalid_argument("Invalid plugin task limits");
  }

  void notify(const Task &T) {
    if (Changed) {
      const auto Snapshot = T.Info;
      Changed(Snapshot);
    }
  }

  void complete(Task &T, TaskPhase Phase) {
    if (SaveReceipt) {
      auto Candidate = T.Info;
      Candidate.Phase = Phase;
      auto Error = SaveReceipt(Candidate);
      if (!Error.empty()) {
        Phase = TaskPhase::Failed;
        T.Info.Diagnostic = std::move(Error);
      }
    }
    T.Info.Phase = Phase;
    if (Running == T.Info.Id)
      Running.clear();
    T.ExecutionLease.reset();
    T.ActionLease.reset();
    Host.close(T.Scope);
    notify(T);
  }

  void stop(const std::string &Id, TaskPhase Outcome, std::string Reason) {
    auto I = Records.find(Id);
    if (I == Records.end() || terminal(I->second.Info.Phase) ||
        I->second.Info.Phase == TaskPhase::Cancelling)
      return;
    auto &T = I->second;
    T.Info.Diagnostic = std::move(Reason);
    T.StopOutcome = Outcome;
    if (T.Info.Phase == TaskPhase::Queued) {
      complete(T, Outcome);
      return;
    }
    T.Info.Phase = TaskPhase::Cancelling;
    notify(T);
    if (StopProcess)
      StopProcess(Id);
  }
};

Tasks::Tasks(PluginHost &Host, Stop StopProcess, Notify Changed,
             TaskLimits Limits, Commit SaveReceipt)
    : State(std::make_shared<Impl>(Host, std::move(StopProcess),
                                   std::move(Changed), Limits,
                                   std::move(SaveReceipt))) {}
Tasks::~Tasks() {
  // Adapters must finish in-flight transport before destroying this controller.
  // Queued tasks can be retired without a process exit.
  for (const auto &[Id, T] : State->Records)
    State->stop(Id, TaskPhase::Cancelled, "Plugin task host closed");
}

bool Tasks::enqueue(std::string Id, Registration Action, Diagnostic &Error) {
  if (Id.empty() || State->Records.contains(Id)) {
    Error = {"invalid_task", "Task ID is empty or already exists"};
    return false;
  }
  size_t Pending = 0;
  for (const auto &[Key, T] : State->Records)
    Pending += !terminal(T.Info.Phase);
  if (Pending >= State->Limits.MaxQueued ||
      State->Records.size() >= State->Limits.MaxHistory) {
    Error = {"queue_full", "Plugin task queue or retained history is full"};
    return false;
  }
  auto Parent = State->Host.ownerOf(Action);
  if (!Parent) {
    Error = {"registration_retired", "Task action is unavailable"};
    return false;
  }
  auto Lease = State->Host.invoke(Action, Error);
  if (!Lease)
    return false;
  auto Scope = State->Host.createOwner(*Parent, OwnerKind::Task, Id, Error);
  if (!Scope)
    return false;
  std::weak_ptr<Impl> Weak = State;
  auto Execution = State->Host.registerResource(
      *Scope, {"task", Id, "v1"},
      [Weak, Id] {
        if (auto S = Weak.lock())
          S->stop(Id, TaskPhase::Cancelled, "Task owner retired");
      },
      {}, Error);
  if (!Execution || !State->Host.activate(*Scope, Error)) {
    State->Host.failActivation(*Scope, Error.Message);
    return false;
  }
  Impl::Task T;
  T.Order = ++State->NextOrder;
  T.Info.Id = Id;
  T.Scope = *Scope;
  T.Execution = *Execution;
  T.ActionLease = std::move(Lease);
  auto [I, Inserted] = State->Records.emplace(std::move(Id), std::move(T));
  State->notify(I->second);
  Error = {};
  return true;
}

bool Tasks::begin(const std::string &Id) {
  auto I = State->Records.find(Id);
  if (I == State->Records.end() || I->second.Info.Phase != TaskPhase::Queued ||
      !State->Running.empty())
    return false;
  if (std::any_of(State->Records.begin(), State->Records.end(),
                  [&](const auto &Entry) {
                    return Entry.second.Info.Phase == TaskPhase::Queued &&
                           Entry.second.Order < I->second.Order;
                  }))
    return false;
  Diagnostic Error;
  auto Lease = State->Host.invoke(I->second.Execution, Error);
  if (!Lease) {
    State->stop(Id, TaskPhase::Stale, Error.Message);
    return false;
  }
  auto &T = I->second;
  T.ExecutionLease = std::move(Lease);
  T.Info.Phase = TaskPhase::Starting;
  State->Running = Id;
  State->notify(T);
  return true;
}

bool Tasks::hello(const std::string &Id) {
  auto I = State->Records.find(Id);
  if (I == State->Records.end() || I->second.Info.Phase != TaskPhase::Starting)
    return false;
  I->second.Info.Phase = TaskPhase::Running;
  State->notify(I->second);
  return true;
}

bool Tasks::result(const std::string &Id) {
  auto I = State->Records.find(Id);
  if (I == State->Records.end() || I->second.Info.Phase != TaskPhase::Running ||
      I->second.Info.HasResult)
    return false;
  I->second.Info.HasResult = true;
  return true;
}

void Tasks::stop(const std::string &Id, TaskPhase Outcome, std::string Reason) {
  if (!terminal(Outcome) || Outcome == TaskPhase::Succeeded)
    throw std::invalid_argument("Invalid plugin cancellation outcome");
  State->stop(Id, Outcome, std::move(Reason));
}

void Tasks::finish(const std::string &Id, bool NormalExit, std::string Error) {
  auto I = State->Records.find(Id);
  if (I == State->Records.end() || terminal(I->second.Info.Phase) ||
      I->second.Info.Phase == TaskPhase::Queued)
    return;
  auto &T = I->second;
  if (T.Info.Phase == TaskPhase::Cancelling) {
    State->complete(T, T.StopOutcome);
    return;
  }
  if (!Error.empty())
    T.Info.Diagnostic = std::move(Error);
  if (!NormalExit || !T.Info.HasResult || !T.Info.Diagnostic.empty()) {
    if (T.Info.Diagnostic.empty())
      T.Info.Diagnostic =
          "Provider exited without a complete successful result";
    State->complete(T, TaskPhase::Failed);
  } else {
    State->complete(T, TaskPhase::Succeeded);
  }
}

std::optional<TaskSnapshot> Tasks::snapshot(const std::string &Id) const {
  auto I = State->Records.find(Id);
  return I == State->Records.end()
             ? std::nullopt
             : std::optional<TaskSnapshot>(I->second.Info);
}
std::optional<Owner> Tasks::owner(const std::string &Id) const {
  const auto I = State->Records.find(Id);
  return I == State->Records.end() ? std::nullopt
                                   : std::optional(I->second.Scope);
}
std::vector<TaskSnapshot> Tasks::snapshots() const {
  std::vector<TaskSnapshot> Result;
  for (const auto &[Id, T] : State->Records)
    Result.push_back(T.Info);
  return Result;
}
bool Tasks::erase(const std::string &Id) {
  auto I = State->Records.find(Id);
  if (I == State->Records.end() || !terminal(I->second.Info.Phase))
    return false;
  if (State->Host.snapshot(I->second.Scope) &&
      !State->Host.forget(I->second.Scope))
    return false;
  State->Records.erase(I);
  return true;
}
} // namespace neverd::plugin
