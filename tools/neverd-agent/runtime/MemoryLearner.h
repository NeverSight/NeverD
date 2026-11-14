//===- MemoryLearner.h - Idle memory extraction -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_RUNTIME_MEMORYLEARNER_H
#define NEVERD_TOOLS_AGENT_RUNTIME_MEMORYLEARNER_H

#include "models/ModelClient.h"
#include "project/ProjectStore.h"

namespace neverd::agent {
class MemoryLearner final : public QObject {
  Q_OBJECT
public:
  explicit MemoryLearner(QObject *parent = nullptr);
  // Sources are persisted before scheduling, allowing offline restart/recovery.
  static bool enqueue(const Project &project, QJsonObject source,
                      const QString &credential, QString &error);
  static QJsonObject pending(const Project &project, QString &error);
  static bool retryFailed(const Project &project, QString &error);
  bool start(const Project &project, const QJsonObject &source,
             ModelProfile profile, QString key, QString &error);
  void cancel();
  bool running() const { return Running; }
signals:
  void finished(const QString &projectId, int added, const QString &error);

private:
  void complete(const ModelTurn &turn);
  ModelClient Client;
  Project ProjectValue;
  QJsonObject Source;
  QString Path, Credential;
  bool Running = false, Interrupted = false;
};
} // namespace neverd::agent
#endif // NEVERD_TOOLS_AGENT_RUNTIME_MEMORYLEARNER_H
