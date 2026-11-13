//===- MemoryStore.h - Scoped durable Agent memories ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Project/object memory, portable provenance, consolidation and task recall.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_PROJECT_MEMORYSTORE_H
#define NEVERD_TOOLS_AGENT_PROJECT_MEMORYSTORE_H

#include "ProjectStore.h"

namespace neverd::agent {
class MemoryStore {
public:
  // Empty objectId addresses the project's memory, never another project.
  static bool read(const Project &project, const QString &objectId,
                   QJsonArray &records, QString &error,
                   bool includeInactive = false);
  static bool append(const Project &project, const QString &objectId,
                     const QString &text, QString &error);
  static bool edit(const Project &project, const QString &objectId,
                   const QString &id, const QString &text, QString &error);
  static bool forget(const Project &project, const QString &objectId,
                     const QString &id, QString &error);
  static bool exportFile(const Project &project, const QString &objectId,
                         const QString &path, QString &error);
  static bool importFile(const Project &project, const QString &objectId,
                         const QString &path, QString &error);

  // Validate the whole model result before changing any accepted memory.
  // An exact source quote supports attribution, not semantic verification.
  static bool consolidate(const Project &project, const QJsonObject &source,
                          const QJsonArray &candidates, int &added,
                          QString &error);
  static bool sourceApplied(const Project &project, const QJsonObject &source,
                            bool &applied, QString &error);
  static bool collect(const Project &project, const QString &objectId,
                      const QJsonArray &shared, QJsonArray &records,
                      QString &error);
  static QJsonArray search(const QJsonArray &records, const QString &query,
                           qsizetype characterBudget);
  static QString recall(const QJsonArray &records, const QString &query,
                        qsizetype characterBudget);
  static QString redact(QString text, const QString &credential = {});
  static QString sourceDirectory(const Project &project);
};
} // namespace neverd::agent
#endif // NEVERD_TOOLS_AGENT_PROJECT_MEMORYSTORE_H
