//===- ExecutionTrace.h - Public execution record ----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_RUNTIME_EXECUTIONTRACE_H
#define NEVERD_TOOLS_AGENT_RUNTIME_EXECUTIONTRACE_H

#include "project/ProjectStore.h"

#include <QHash>
#include <QVariantList>
#include <optional>

namespace neverd::agent {
class ExecutionTrace {
public:
  std::optional<qsizetype> observe(const QJsonObject &event);
  QJsonObject record(qsizetype index) const {
    return Rows.at(index).toObject();
  }
  QVariantList rows() const { return Rows.toVariantList(); }
  static QVariantList read(const Project &project, const QString &runId,
                           QString &error);

private:
  QJsonArray Rows;
  QHash<QString, qsizetype> Calls;
};
} // namespace neverd::agent
#endif
