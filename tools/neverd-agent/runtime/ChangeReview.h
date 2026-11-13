//===- ChangeReview.h - Reviewable ND project commands ------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_RUNTIME_CHANGEREVIEW_H
#define NEVERD_TOOLS_AGENT_RUNTIME_CHANGEREVIEW_H
#include "WorkerSession.h"

namespace neverd::agent {
class ChangeReview {
public:
  static void propose(const Project &project, const QJsonObject &parameters,
                      const QString &worker, int timeoutSeconds, QObject *owner,
                      WorkerSession::Done done);
  static void apply(const Project &project, const QString &id, bool undo,
                    const QString &worker, int timeoutSeconds, QObject *owner,
                    WorkerSession::Done done);
  static QVariantList list(const Project &project, QString &error);
  static bool dismiss(const Project &project, const QString &id,
                      QString &error);
};
} // namespace neverd::agent
#endif
