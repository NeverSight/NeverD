//===- AgentWorkspace.h - CLI project workspace ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own a project workspace and expose its state and actions to a CLI client.

#ifndef NEVERD_TOOLS_AGENT_BRIDGE_AGENTWORKSPACE_H
#define NEVERD_TOOLS_AGENT_BRIDGE_AGENTWORKSPACE_H

#include "app/AgentController.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>
#include <QSet>
#include <QTimer>
#include <functional>
#include <optional>

namespace neverd::agent {
bool initializeWorkspace(AgentController &controller, QLockFile &lock,
                         const QString &project, QString &error);

class AgentWorkspace final : public QObject {
public:
  AgentWorkspace(const QString &directory,
                 std::function<void(const QJsonObject &)> publish);
  bool initialize(const QString &project, bool interactiveObjectApprovals,
                  QString &error);
  std::optional<QJsonValue> dispatch(const QString &method,
                                     const QJsonArray &args,
                                     const QString &sideId);
  ShutdownStatus prepareShutdown();

private:
  void markChanged(const QByteArray &signal);
  void postEvent(const QJsonObject &event);
  void publishPending();
  void publishSideChat();
  std::optional<QJsonValue>
  invoke(const QString &method, const QJsonArray &args, const QString &sideId);

  QLockFile Lock;
  AgentController Controller;
  std::function<void(const QJsonObject &)> Publish;
  QTimer Flush;
  QSet<QByteArray> Dirty;
  QJsonObject Published, PublishedSide;
  QList<QJsonObject> Events;
  SideChat *Side = nullptr;
  QList<QMetaObject::Connection> SideConnections;
  bool SideDirty = false;
};
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_BRIDGE_AGENTWORKSPACE_H
