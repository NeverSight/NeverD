//===- GoalState.h - Persisted project objective ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Goal progress is independent of a single model turn and its context usage.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_PROJECT_GOALSTATE_H
#define NEVERD_TOOLS_AGENT_PROJECT_GOALSTATE_H
#include "ProjectStore.h"

namespace neverd::agent {
struct GoalState {
  QString Id, Objective, Status = "none", Summary, NextStep, Evidence, Error;
  QString CreatedAt, UpdatedAt;
  qint64 TokenBudget = 0, UsedTokens = 0, PendingTokens = 0;
  int Rounds = 0, RoundLimit = 0;
  bool Estimated = false;
  QJsonObject toJson() const;
  bool save(const Project &project, QString &error);
  static bool load(const Project &project, GoalState &goal, QString &error);
};
} // namespace neverd::agent
#endif // NEVERD_TOOLS_AGENT_PROJECT_GOALSTATE_H
