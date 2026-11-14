//===- WorkerSession.h - Revision-bound ND command client -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_RUNTIME_WORKERSESSION_H
#define NEVERD_TOOLS_AGENT_RUNTIME_WORKERSESSION_H
#include "project/ProjectStore.h"

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <functional>

namespace neverd::agent {
class WorkerSession final : public QObject {
public:
  using Done = std::function<void(QJsonObject)>;
  explicit WorkerSession(QObject *parent = nullptr);
  ~WorkerSession() override;
  void start(const QString &program, const AnalysisObject &object,
             bool readOnly, int timeoutSeconds, Done ready);
  void request(const QString &operation, const QJsonObject &payload,
               bool mutation, Done done);
  void stop(std::function<void()> done);
  bool supports(const QString &operation) const {
    return Capabilities.contains(operation);
  }
  QString revision() const { return Revision; }

private:
  void receive();
  void fail(const QString &error);
  void opened(const QJsonObject &result);
  QProcess Process;
  QTimer Deadline, StopDeadline;
  QByteArray Buffer;
  QString Revision, ProjectId, RequestId;
  QStringList Capabilities;
  AnalysisObject Object;
  Done Current, Ready;
  std::function<void()> Stopped;
  qint64 NextId = 0;
  int TimeoutMs = 0;
  bool ReadOnly = true, Mutation = false, Failed = false, Hello = false;
};
} // namespace neverd::agent
#endif
