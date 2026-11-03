//===- ToolRegistry.h - Model-independent plugin tools ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Frozen consumer steps and exactly bound calls over the NPH action service.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_TOOLREGISTRY_H
#define NEVERD_TOOLS_TOOLREGISTRY_H

#include "neverd/plugin/Actions.h"

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd::tools {
using plugin::Json;

struct Environment {
  plugin::Owner Project;
  Json Context = Json::object();
  std::set<std::string> Services;
  bool Headless = true;
};
struct ToolDefinition {
  std::string Id;
  plugin::Registration Action;
  /// A declared follow-up requires a successful operation from this exact
  /// action generation, with the same original context and component owner.
  std::optional<plugin::Registration> ContinuationOf;
};
struct ToolInfo {
  plugin::Registration Token;
  ToolDefinition Tool;
  plugin::ActionDefinition Action;
  Json EffectiveParameters = Json::object();
  plugin::Diagnostic Unavailable;
};

class ResolvedTools {
  friend class ToolRegistry;
  struct Impl;
  std::shared_ptr<Impl> State;
  explicit ResolvedTools(std::shared_ptr<Impl> State);

public:
  ResolvedTools() = default;
  uint64_t id() const;
  std::vector<ToolInfo> tools() const;
  /// Copies refer to the same frozen step; release invalidates all copies for
  /// new calls, but does not cancel existing operations or discard receipts.
  void release();
};

struct Operation {
  std::string Id, Session, Step, CallId;
  uint64_t SetId = 0;
  ToolInfo Tool;
  Json Context, Parameters;
  Json Configuration = Json::object();
  std::string ParentOperation;
  plugin::ActionSnapshot Result;
};

class ToolRegistry {
  struct Impl;
  std::shared_ptr<Impl> State;

public:
  using Current = std::function<Environment()>;
  using Policy =
      std::function<plugin::Diagnostic(const ToolInfo &, const Environment &)>;
  using Changed = std::function<void(const Operation &)>;
  ToolRegistry(plugin::PluginHost &Host, plugin::Actions &Actions,
               Current GetCurrent, Policy Allow = {}, Changed Notify = {});
  ~ToolRegistry();
  ToolRegistry(const ToolRegistry &) = delete;
  ToolRegistry &operator=(const ToolRegistry &) = delete;

  std::optional<plugin::Registration> add(ToolDefinition Tool,
                                          plugin::Diagnostic &Error);
  /// Includes unavailable tools with reasons. Schemas/defaults come from NPH
  /// actions. User/Profile are objects keyed by tool ID within plugin ID.
  std::optional<ResolvedTools> resolve(std::string Session, std::string Step,
                                       const Json &User, const Json &Profile,
                                       plugin::Diagnostic &Error);
  std::optional<std::string> call(const ResolvedTools &Set,
                                  plugin::Registration Tool, std::string CallId,
                                  const Json &Parameters,
                                  std::string ParentOperation,
                                  plugin::Diagnostic &Error);
  /// Missing operations are unknown, including after restart. No implicit
  /// retry.
  std::optional<Operation> operation(const std::string &Id) const;
  bool cancel(const std::string &Id);
  /// Detach notifications without losing in-flight operations or receipts.
  void setObserver(Changed Notify);
  std::string runtimeId() const;
};

Json describeTool(const ToolInfo &Tool);
Json describeOperation(const Operation &Operation);
} // namespace neverd::tools

#endif // NEVERD_TOOLS_TOOLREGISTRY_H
