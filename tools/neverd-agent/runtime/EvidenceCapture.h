//===- EvidenceCapture.h - Materialize ND snapshot pages --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_RUNTIME_EVIDENCECAPTURE_H
#define NEVERD_TOOLS_AGENT_RUNTIME_EVIDENCECAPTURE_H
#include "McpSession.h"

#include <QPointer>

namespace neverd::agent {
class EvidenceCapture final : public QObject {
  Q_OBJECT
public:
  explicit EvidenceCapture(McpSession *engine);
  static EvidenceCapture *captureAndSave(McpSession *engine, Project project,
                                         AnalysisObject object, QString runId,
                                         QString tool, QJsonObject arguments,
                                         QJsonObject result,
                                         McpSession::Done done);
  void start(const QJsonObject &result);
  void cancel();
signals:
  void finished(const QString &text, bool complete, const QString &issue);

private:
  void next();
  void finish(bool complete, const QString &issue = {});
  QPointer<McpSession> Engine;
  QString URI, Text, Revision;
  qint64 Offset = 0, Total = 0, Request = 0;
  bool Cancelled = false, Finished = false;
};
} // namespace neverd::agent
#endif
