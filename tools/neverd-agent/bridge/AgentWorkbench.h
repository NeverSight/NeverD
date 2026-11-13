//===- AgentWorkbench.h - Attached workbench conversation ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Serve a workbench-bound conversation through the Agent CLI.

#ifndef NEVERD_TOOLS_AGENT_BRIDGE_AGENTWORKBENCH_H
#define NEVERD_TOOLS_AGENT_BRIDGE_AGENTWORKBENCH_H

#include "app/AgentController.h"
#include "app/AgentWorkbenchController.h"

#include <QJsonArray>
#include <functional>
#include <optional>

namespace neverd::agent {
class AgentWorkbench final : public QObject {
public:
  AgentWorkbench(const QString &home,
                 std::function<void(const QJsonObject &)> publish);
  void bind(const QJsonObject &context);
  std::optional<QJsonValue> dispatch(const QString &method,
                                     const QJsonArray &args);
  ShutdownStatus prepareShutdown();

private:
  void fail(const QString &text, bool fatal = false);
  void announceReady();
  void finishPreparation();
  void prepare(const QString &attachment);
  bool loadPreview(QString &error);
  void publishMessages();
  void publishExecution(const QString &runId, qsizetype first,
                        const QJsonArray &rows);
  void readExecution(const QString &runId);
  void navigateEvidence(const QString &id);
  void publishState();

  QString Home;
  AgentController Controller;
  std::function<void(const QJsonObject &)> Publish;
  QJsonObject Binding, PublishedState;
  QJsonArray PreviewMessages, Published;
  std::optional<WorkbenchConversation> Location;
  std::optional<Project> PreviewProject;
  bool Prepared = false;
  bool AnalysisReady = false;
  bool Ready = false;
};
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_BRIDGE_AGENTWORKBENCH_H
