//===- ProcessActions.h - External providers as NPH actions -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reuse Tasks/Process/Protocol for structured actions and command receipts.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGIN_PROCESSACTIONS_H
#define NEVERD_PLUGIN_PROCESSACTIONS_H

#include "neverd/plugin/Actions.h"
#include "neverd/plugin/Executor.h"
#include "neverd/plugin/HostServices.h"
#include "neverd/plugin/Protocol.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace neverd::plugin {
using ProcessServiceRequest = ServiceRequest;

/// Presentation of the shared task controller, including legacy v1 results.
struct ProcessActionSnapshot {
  TaskSnapshot Task;
  Registration Binding;
  std::filesystem::path Directory;
  Json Result = Json::object();
  Json Receipt = Json::object();
  std::string Stderr;
  int64_t ElapsedMs = 0;
};

struct ProcessActionEvents {
  std::function<void(const ProcessActionSnapshot &)> Changed;
  std::function<void(const std::string &, const std::string &, bool)> Log;
};

class ProcessActions {
  struct Impl;
  std::shared_ptr<Impl> State;

public:
  /// Adapters validate permissions/context and eventually return an actual
  /// service reply, including after a provider is cancelled or exits.
  using Service = std::function<void(const ProcessServiceRequest &,
                                     std::function<void(Json)>)>;
  ProcessActions(PluginHost &Host, Actions &Actions, Executor &Queue,
                 std::filesystem::path Output, Service Query,
                 Service Command = {}, ProcessActionEvents Events = {},
                 HostServices *Capabilities = nullptr);
  ~ProcessActions();
  ProcessActions(const ProcessActions &) = delete;
  ProcessActions &operator=(const ProcessActions &) = delete;
  std::optional<Registration> add(Owner Scope, const Manifest &Plugin,
                                  ActionDefinition Definition,
                                  ProtocolExtensions Extensions,
                                  Diagnostic &Error);
  /// Host invalidation and timeouts use the same task state machine as cancel.
  void stop(const std::string &Id, TaskPhase Outcome, std::string Reason);
  /// Called by the consumer event loop; Tasks owns serial admission. This only
  /// starts admitted providers, handles deadlines, and collects finished jobs.
  void poll();
  /// Asynchronous close. Done is queued only after provider exits AND accepted
  /// command replies. Keep the host, dispatcher and service adapter alive until
  /// Done; destroying this facade alone does not drop outstanding receipts.
  void shutdown(PluginHost::Work Done = {});
  bool idle() const;
};
} // namespace neverd::plugin

#endif // NEVERD_PLUGIN_PROCESSACTIONS_H
