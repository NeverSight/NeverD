//===- GoalState.cpp - Persisted project objective -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate saved state and require explicit resume after process interruption.
//===----------------------------------------------------------------------===//
#include "GoalState.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <climits>
#include <cmath>

namespace neverd::agent {
QJsonObject GoalState::toJson() const {
  return {{"version", 1},
          {"id", Id},
          {"objective", Objective},
          {"status", Status},
          {"summary", Summary},
          {"nextStep", NextStep},
          {"evidence", Evidence},
          {"error", Error},
          {"createdAt", CreatedAt},
          {"updatedAt", UpdatedAt},
          {"tokenBudget", TokenBudget},
          {"usedTokens", UsedTokens},
          {"pendingTokens", PendingTokens},
          {"rounds", Rounds},
          {"roundLimit", RoundLimit},
          {"estimated", Estimated}};
}
bool GoalState::save(const Project &project, QString &error) {
  UpdatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  return ProjectStore::writeJson(
      QDir(ProjectStore::stateDirectory(project)).filePath("router/goal.json"),
      toJson(), error);
}
bool GoalState::load(const Project &project, GoalState &goal, QString &error) {
  const auto path =
      QDir(ProjectStore::stateDirectory(project)).filePath("router/goal.json");
  if (!QFileInfo::exists(path)) {
    goal = {};
    return true;
  }
  QJsonObject value;
  if (!ProjectStore::readJson(path, value, error))
    return false;
  const auto invalid = [&] {
    error = "Invalid saved goal: " + path;
    return false;
  };
  if (value["version"].toInt() != 1)
    return invalid();
  for (const auto *key : {"id", "objective", "status", "summary", "nextStep",
                          "evidence", "error", "createdAt", "updatedAt"})
    if (!value[key].isString())
      return invalid();
  GoalState candidate;
  candidate.Id = value["id"].toString();
  if (QUuid(candidate.Id).isNull() ||
      QUuid(candidate.Id).toString(QUuid::WithoutBraces) != candidate.Id)
    return invalid();
  candidate.Objective = value["objective"].toString();
  candidate.Status = value["status"].toString();
  if (candidate.Objective.trimmed().isEmpty() ||
      !QStringList{"active", "paused", "completed", "blocked",
                   "budget_exhausted", "round_limit", "interrupted", "failed"}
           .contains(candidate.Status))
    return invalid();
  const auto number = [&](const char *key, qint64 &out) {
    const auto raw = value[key];
    const double n = raw.toDouble(-1);
    if (!raw.isDouble() || !std::isfinite(n) || n < 0 || std::floor(n) != n ||
        n > 9007199254740991.0)
      return false;
    out = qint64(n);
    return true;
  };
  qint64 rounds = 0, limit = 0;
  if (!number("tokenBudget", candidate.TokenBudget) ||
      !number("usedTokens", candidate.UsedTokens) ||
      !number("pendingTokens", candidate.PendingTokens) ||
      !number("rounds", rounds) || !number("roundLimit", limit) ||
      rounds > INT_MAX || limit > INT_MAX || candidate.TokenBudget == 0 ||
      limit == 0 || !value["estimated"].isBool())
    return invalid();
  candidate.Rounds = int(rounds);
  candidate.RoundLimit = int(limit);
  candidate.Estimated = value["estimated"].toBool();
  candidate.Summary = value["summary"].toString();
  candidate.NextStep = value["nextStep"].toString();
  candidate.Evidence = value["evidence"].toString();
  candidate.Error = value["error"].toString();
  candidate.CreatedAt = value["createdAt"].toString();
  candidate.UpdatedAt = value["updatedAt"].toString();
  if (!QDateTime::fromString(candidate.CreatedAt, Qt::ISODateWithMs)
           .isValid() ||
      !QDateTime::fromString(candidate.UpdatedAt, Qt::ISODateWithMs).isValid())
    return invalid();
  if (candidate.Status == "active" || candidate.PendingTokens > 0) {
    candidate.UsedTokens += candidate.PendingTokens;
    candidate.Estimated |= candidate.PendingTokens > 0;
    candidate.PendingTokens = 0;
    candidate.Status = "interrupted";
    candidate.Error =
        "Previous goal execution was interrupted; resume explicitly.";
    if (!candidate.save(project, error))
      return false;
  }
  goal = std::move(candidate);
  return true;
}
} // namespace neverd::agent
