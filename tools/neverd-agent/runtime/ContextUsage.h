//===- ContextUsage.h - Current context accounting -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Provider usage calibrates the estimate for the next request, independently
/// of cumulative billing. Cached input remains part of the context window.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_RUNTIME_CONTEXTUSAGE_H
#define NEVERD_TOOLS_AGENT_RUNTIME_CONTEXTUSAGE_H

#include "models/ModelProfile.h"

#include <QJsonArray>
#include <QVariantMap>

namespace neverd::agent {
struct ContextUsage {
  qint64 Input = 0, Output = 0, Cached = 0, Tokens = 0, Baseline = 0;
  qint64 Before = 0, After = 0;
  qint64 RetainedTurns = 0, RetainedExchanges = 0, PreservedRequests = 0,
         SummaryRequests = 0;
  int Compactions = 0;
  bool Measured = false, Estimated = true, Compacting = false;

  static qint64 estimate(const QJsonObject &request);
  void prepare(const QJsonObject &request);
  void observe(ModelProtocol protocol, const QJsonObject &usage,
               const QJsonObject &continuation);
  void reset(const QJsonObject &request);
  QJsonObject toJson(const ModelProfile &profile) const;
  bool restore(const QJsonObject &value);
};

/// Public messages, calls and results only. Opaque reasoning/signatures are
/// not useful summary evidence; provider history remains intact elsewhere.
QString compactionTranscript(ModelProtocol protocol, const QJsonArray &history);
QString compactionInstructions();
} // namespace neverd::agent
#endif // NEVERD_TOOLS_AGENT_RUNTIME_CONTEXTUSAGE_H
