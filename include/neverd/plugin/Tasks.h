//===- Tasks.h - Shared plugin task lifecycle -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Serial task admission and exit-based completion, independent of transport.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_TASKS_H
#define NEVERD_PLUGIN_TASKS_H

#include "neverd/plugin/Config.h"
#include "neverd/plugin/PluginHost.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::plugin {
enum class TaskPhase {
  Queued,
  Starting,
  Running,
  Cancelling,
  Succeeded,
  Failed,
  Cancelled,
  Stale,
  TimedOut
};

const char *taskPhaseName(TaskPhase Phase);
bool terminal(TaskPhase Phase);

struct TaskSnapshot {
  std::string Id;
  TaskPhase Phase = TaskPhase::Queued;
  std::string Diagnostic;
  bool HasResult = false;
};

/// The transport reports hello/result/exit; only this controller owns task
/// state. Payloads, semantic queries and artifact validation belong to their
/// owning adapters, which must finish validation before successful exit.
class Tasks {
  struct Impl;
  std::shared_ptr<Impl> State;

public:
  using Notify = std::function<void(const TaskSnapshot &)>;
  using Stop = std::function<void(const std::string &)>;
  using Commit = std::function<std::string(const TaskSnapshot &)>;
  Tasks(PluginHost &Host, Stop StopProcess, Notify Changed,
        TaskLimits Limits = {}, Commit SaveReceipt = {});
  ~Tasks();
  Tasks(const Tasks &) = delete;
  Tasks &operator=(const Tasks &) = delete;

  bool enqueue(std::string Id, Registration Action, Diagnostic &Error);
  bool begin(const std::string &Id);
  bool hello(const std::string &Id);
  bool result(const std::string &Id);
  void stop(const std::string &Id, TaskPhase Outcome, std::string Reason);
  void finish(const std::string &Id, bool NormalExit, std::string Error = {});
  std::optional<TaskSnapshot> snapshot(const std::string &Id) const;
  std::optional<Owner> owner(const std::string &Id) const;
  std::vector<TaskSnapshot> snapshots() const;
  bool erase(const std::string &Id);
};
} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_TASKS_H
