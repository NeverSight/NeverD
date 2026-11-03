//===- Actions.h - Shared callable NPH actions -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact action dispatch, structured completion and cooperative cancellation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_ACTIONS_H
#define NEVERD_PLUGIN_ACTIONS_H

#include "neverd/plugin/Manifest.h"
#include "neverd/plugin/PluginHost.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neverd::plugin {
enum class ActionEffect { Read, Artifact, ProjectWrite };
enum class ActionPhase { Running, Cancelling, Succeeded, Failed, Cancelled };
const char *actionPhaseName(ActionPhase Phase);
bool terminal(ActionPhase Phase);

struct ActionDefinition {
  std::string PluginId, Id, Version, Title, Description;
  Json Parameters = Json::array();
  Json OutputSchema;
  std::string Context = "none";
  std::vector<std::string> Services;
  ActionEffect Effect = ActionEffect::Read;
  bool Headless = true;
};
struct ActionRequest {
  std::string Operation;
  Json Context = Json::object();
  Json Parameters = Json::object();
  /// Supplied by the host from a previously completed operation, not by a
  /// provider or by replacing an old report's context with the current one.
  Json Continuation;
};
struct ActionSnapshot {
  ActionPhase Phase = ActionPhase::Running;
  bool CancelRequested = false;
  Json Progress = Json::object();
  Json Data;
  Json Artifacts = Json::array();
  Json Receipts = Json::array();
  Diagnostic Error;
};

/// All methods and callbacks run on the host's control sequence. A provider
/// uses the injected dispatcher to post completion from other threads. Cancel
/// requests never fabricate completion or erase accepted-command receipts.
class ActionCall {
  friend class Actions;
  struct Impl;
  std::shared_ptr<Impl> State;
  explicit ActionCall(std::shared_ptr<Impl> State);

public:
  const ActionRequest &request() const;
  const ActionSnapshot &snapshot() const;
  void onCancel(PluginHost::Work Cancel);
  void cancel();
  void progress(Json Progress);
  void receipt(Json Receipt);
  void succeed(Json Data, Json Artifacts = Json::array());
  void fail(Diagnostic Error, bool Cancelled = false);
};

class Actions {
  struct Impl;
  std::shared_ptr<Impl> State;

public:
  using Handler = std::function<void(std::shared_ptr<ActionCall>)>;
  using Changed = std::function<void(const ActionSnapshot &)>;
  explicit Actions(PluginHost &Host);
  ~Actions();
  Actions(const Actions &) = delete;
  Actions &operator=(const Actions &) = delete;
  std::optional<Registration> add(Owner Scope, ActionDefinition Definition,
                                  Handler Run, Diagnostic &Error);
  /// Staged definitions can be inspected by registrars; only invoke publishes
  /// execution, after NPH activation. No callback is exposed in this snapshot.
  std::optional<ActionDefinition> definition(Registration Action) const;
  std::optional<Owner> owner(Registration Action) const;
  std::shared_ptr<ActionCall> invoke(Registration Action, ActionRequest Request,
                                     Changed Notify, Diagnostic &Error,
                                     Handler BindBeforeStart = {});
};
} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_ACTIONS_H
