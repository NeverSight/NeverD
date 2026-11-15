//===- AgentGoalController.cpp - Persistent objective execution ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Run explicit goals in bounded rounds; completion requires a progress report.
//===----------------------------------------------------------------------===//
#include "AgentController.h"
#include "Config.h"

#include <QDateTime>
#include <QDir>
#include <QUuid>

namespace neverd::agent {
QVariantMap AgentController::goal() const {
  auto result = Goals.value(ActiveId).toJson().toVariantMap();
  result["defaultTokenBudget"] = kDefaultGoalTokens;
  result["defaultRoundLimit"] = kDefaultGoalRounds;
  return result;
}
bool AgentController::startGoal(const QString &objective, qint64 tokenBudget,
                                int roundLimit) {
  const auto *project = findProject(ActiveId);
  if (!project || running())
    return false;
  if (objective.trimmed().isEmpty() || tokenBudget <= 0 || roundLimit <= 0) {
    emit operationError(tr("Enter a goal and set a positive token budget and "
                           "round limit."));
    return false;
  }
  const auto old = Goals.value(ActiveId);
  if (old.Status == "active")
    return false;
  QString error;
  if (!old.Id.isEmpty() &&
      !ProjectStore::writeJson(QDir(ProjectStore::stateDirectory(*project))
                                   .filePath("goals/" + old.Id + ".json"),
                               old.toJson(), error)) {
    emit operationError(error);
    return false;
  }
  GoalState next;
  next.Id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  next.Objective = objective.trimmed();
  next.TokenBudget = tokenBudget;
  next.RoundLimit = roundLimit;
  next.Status = "paused";
  next.CreatedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  if (!next.save(*project, error)) {
    emit operationError(error);
    return false;
  }
  Goals[ActiveId] = next;
  emit goalChanged();
  return resumeGoal();
}
bool AgentController::updateGoal(const QString &objective, qint64 tokenBudget,
                                 int roundLimit) {
  const auto *project = findProject(ActiveId);
  auto next = Goals.value(ActiveId);
  if (!project || next.Id.isEmpty() || running() || next.Status == "active")
    return false;
  if (objective.trimmed().isEmpty() || tokenBudget <= 0 || roundLimit <= 0) {
    emit operationError(
        tr("The goal, token budget or round limit is invalid."));
    return false;
  }
  next.Objective = objective.trimmed();
  next.TokenBudget = tokenBudget;
  next.RoundLimit = roundLimit;
  // Keep all spent usage and completed rounds when editing/resuming a goal.
  next.Status = "paused";
  QString error;
  if (!next.save(*project, error)) {
    emit operationError(error);
    return false;
  }
  Goals[ActiveId] = next;
  emit goalChanged();
  return true;
}
bool AgentController::resumeGoal() {
  auto &goal = Goals[ActiveId];
  const auto *project = findProject(ActiveId);
  if (!project || goal.Id.isEmpty() || running() || goal.Status == "active" ||
      goal.Status == "completed")
    return false;
  if (goal.UsedTokens >= goal.TokenBudget || goal.Rounds >= goal.RoundLimit) {
    emit operationError(tr("The goal has reached its budget or round limit. "
                           "Adjust the goal settings first."));
    return false;
  }
  auto budget = std::make_shared<RunBudget>();
  budget->TokenLimit = goal.TokenBudget;
  budget->Goal = true;
  budget->Tokens = goal.UsedTokens;
  budget->Estimated = goal.Estimated;
  GoalBudgets[ActiveId] = budget;
  goal.Status = "active";
  goal.Error.clear();
  QString error;
  if (!goal.save(*project, error)) {
    goal.Status = "paused";
    emit operationError(error);
    return false;
  }
  emit goalChanged();
  return runGoalRound(ActiveId);
}
bool AgentController::runGoalRound(const QString &projectId) {
  auto &goal = Goals[projectId];
  const auto *project = findProject(projectId);
  if (!project || goal.Status != "active")
    return false;
  const auto *runtime = Runtimes.value(projectId);
  if (runtime && runtime->running())
    return false;
  if (goal.UsedTokens >= goal.TokenBudget || goal.Rounds >= goal.RoundLimit) {
    goal.Status = goal.UsedTokens >= goal.TokenBudget ? "budget_exhausted"
                                                      : "round_limit";
    QString error;
    if (!goal.save(*project, error))
      emit operationError(error);
    if (projectId == ActiveId)
      emit goalChanged();
    return false;
  }
  const auto prompt =
      goal.Rounds == 0 ? goal.Objective
                       : QStringLiteral("Continue working toward the goal: "
                                        "%1\nResult of the previous round: "
                                        "%2\nNext step: %3\nContinue from the "
                                        "existing evidence and avoid repeating "
                                        "work.")
                             .arg(goal.Objective, goal.Summary, goal.NextStep);
  if (!beginRequest(projectId, prompt, {}, true)) {
    goal.Status = "failed";
    goal.Error =
        "Unable to start goal round. Check model settings and the run error.";
    QString error;
    if (!goal.save(*project, error))
      emit operationError(error);
    if (projectId == ActiveId)
      emit goalChanged();
    return false;
  }
  ++goal.Rounds;
  QString error;
  if (!goal.save(*project, error)) {
    goal.Status = "paused";
    if (auto *active = Runtimes.value(projectId))
      active->cancel();
    emit operationError(error);
    return false;
  }
  if (projectId == ActiveId)
    emit goalChanged();
  return true;
}
void AgentController::goalRunChanged(const QString &projectId,
                                     const QString &goalId) {
  auto &goal = Goals[projectId];
  const auto budget = GoalBudgets.value(projectId);
  if (goal.Id != goalId || !budget)
    return;
  if (goal.UsedTokens == budget->Tokens &&
      goal.PendingTokens == budget->Reserved &&
      goal.Estimated == budget->Estimated)
    return;
  goal.UsedTokens = budget->Tokens;
  goal.PendingTokens = budget->Reserved;
  goal.Estimated = budget->Estimated;
  QString error;
  if (const auto *project = findProject(projectId))
    if (!goal.save(*project, error)) {
      goal.Status = "paused";
      if (auto *runtime = Runtimes.value(projectId))
        runtime->cancel();
      emit operationError(error);
    }
  if (projectId == ActiveId)
    emit goalChanged();
}
void AgentController::goalRunFinished(const QString &projectId,
                                      const QString &goalId,
                                      const QString &error, bool cancelled) {
  auto &goal = Goals[projectId];
  if (goal.Id != goalId)
    return;
  goalRunChanged(projectId, goalId);
  const auto budget = GoalBudgets.value(projectId);
  const auto report = Runtimes.value(projectId)->goalReport();
  if (!report.isEmpty()) {
    goal.Summary = report["summary"].toString();
    goal.NextStep = report["nextStep"].toString();
    goal.Evidence = report["evidence"].toString();
  }
  if (goal.Status == "active") {
    if (cancelled)
      goal.Status = "paused";
    else if (budget->LimitReached)
      goal.Status = "budget_exhausted";
    else if (!error.isEmpty())
      goal.Status = "failed";
    else if (report.isEmpty()) {
      goal.Status = "blocked";
      goal.Error = "The model finished without reporting goal progress. Review "
                   "before resuming.";
    } else {
      if (report["status"] == "completed")
        goal.Status = "completed";
      else if (report["status"] == "blocked")
        goal.Status = "blocked";
      else if (goal.Rounds >= goal.RoundLimit)
        goal.Status = "round_limit";
    }
  }
  if (!error.isEmpty())
    goal.Error = error;
  QString storageError;
  if (!goal.save(*findProject(projectId), storageError)) {
    goal.Status = "paused";
    emit operationError(storageError);
  }
  if (projectId == ActiveId)
    emit goalChanged();
  if (goal.Status == "active")
    QTimer::singleShot(0, this, [this, projectId, goalId] {
      if (Goals.value(projectId).Id == goalId &&
          Goals.value(projectId).Status == "active")
        runGoalRound(projectId);
    });
}
void AgentController::pauseProjectGoal(const QString &projectId) {
  auto &goal = Goals[projectId];
  if (goal.Status != "active")
    return;
  goal.Status = "paused";
  QString error;
  if (const auto *project = findProject(projectId))
    if (!goal.save(*project, error))
      emit operationError(error);
  if (projectId == ActiveId)
    emit goalChanged();
}
void AgentController::pauseGoal() {
  pauseProjectGoal(ActiveId);
  if (auto *runtime = Runtimes.value(ActiveId))
    runtime->cancel();
}
} // namespace neverd::agent
