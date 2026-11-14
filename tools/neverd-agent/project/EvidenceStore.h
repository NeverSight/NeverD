//===- EvidenceStore.h - Durable object-bound evidence ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_PROJECT_EVIDENCESTORE_H
#define NEVERD_TOOLS_AGENT_PROJECT_EVIDENCESTORE_H
#include "ProjectStore.h"

namespace neverd::agent {
class EvidenceStore {
public:
  static QJsonObject resultMetadata(const QJsonObject &result);
  static QString resultText(const QJsonObject &result);
  static QJsonObject descriptor(const Project &project,
                                const AnalysisObject &object,
                                const QString &runId, const QString &tool,
                                const QJsonObject &arguments,
                                const QJsonObject &result);
  static bool save(const Project &project, QJsonObject evidence,
                   const QString &text, QString &error);
  static bool read(const Project &project, const QString &id,
                   QJsonObject &evidence, QString &error, qint64 offset = 0);
  static QJsonObject reference(const QJsonObject &evidence);
  static bool validateReference(const QJsonObject &value, QString &error);
  static bool validateBindings(const Project &project,
                               const QJsonArray &references, QString &error);
};
} // namespace neverd::agent
#endif
