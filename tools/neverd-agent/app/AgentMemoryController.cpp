//===- AgentMemoryController.cpp - Memory preferences and idle scheduling -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AgentController.h"
#include "Config.h"
#include "project/MemoryStore.h"

namespace neverd::agent {
bool AgentController::updateMemorySettings(bool use, bool generate) {
  const auto oldUse = UseMemory, oldGenerate = GenerateMemory;
  UseMemory = use;
  GenerateMemory = generate;
  QString error;
  if (!saveSettings(error)) {
    UseMemory = oldUse;
    GenerateMemory = oldGenerate;
    emit operationError(error);
    return false;
  }
  if (!generate) {
    MemoryTimer.stop();
    Learner.cancel();
    MemoryStatus =
        tr("Automatic organization is off. Saved memories are kept.");
  } else
    scheduleMemory();
  emit settingsChanged();
  emit memoryChanged();
  return true;
}
bool AgentController::editMemory(const QString &objectId, const QString &id,
                                 const QString &text) {
  const auto *project = findProject(ActiveId);
  if (!project)
    return false;
  // A user correction wins over an extraction still using the old index.
  Learner.cancel();
  QString error;
  if (!MemoryStore::edit(*project, objectId, id, text, error)) {
    emit operationError(error);
    return false;
  }
  refreshMemory();
  return true;
}
bool AgentController::forgetMemory(const QString &objectId, const QString &id) {
  const auto *project = findProject(ActiveId);
  if (!project)
    return false;
  Learner.cancel();
  QString error;
  if (!MemoryStore::forget(*project, objectId, id, error)) {
    emit operationError(error);
    return false;
  }
  refreshMemory();
  return true;
}
void AgentController::queueMemory(const QString &projectId,
                                  const QJsonObject &source) {
  if (!GenerateMemory)
    return;
  const auto *project = findProject(projectId);
  if (!project)
    return;
  QString error;
  if (!MemoryLearner::enqueue(*project, source, CachedKey, error))
    MemoryStatus =
        tr("Cannot save memory for later organization: %1").arg(error);
  else
    MemoryStatus = tr("Analysis record saved. Memory will be organized when "
                      "idle.");
  emit memoryChanged();
  scheduleMemory();
}
void AgentController::foregroundStarted() {
  MemoryTimer.stop();
  Learner.cancel();
}
void AgentController::scheduleMemory() {
  if (GenerateMemory)
    MemoryTimer.start();
}
void AgentController::organizeMemory(bool retryFailures) {
  if (!GenerateMemory) {
    emit notice(tr("Turn on automatic memory organization first."));
    return;
  }
  const auto *project = findProject(ActiveId);
  QString error;
  if (retryFailures && project &&
      !MemoryLearner::retryFailed(*project, error)) {
    MemoryStatus = error;
    emit memoryChanged();
    return;
  }
  drainMemory();
}
void AgentController::drainMemory() {
  if (!GenerateMemory || Learner.running())
    return;
  for (const auto &project : Projects)
    if (projectHasAnalysis(project.Id)) {
      scheduleMemory();
      return;
    }
  QString key, error;
  if (!Profile.validate(error) || !readModelKey(key, error))
    return;
  for (const auto &project : Projects) {
    auto source = MemoryLearner::pending(project, error);
    if (!error.isEmpty()) {
      MemoryStatus = error;
      emit memoryChanged();
      return;
    }
    if (source.isEmpty())
      continue;
    if (!Learner.start(project, source, Profile, key, error)) {
      MemoryStatus = tr("Memory organization did not start: %1").arg(error);
      emit memoryChanged();
      return;
    }
    MemoryStatus =
        tr("Organizing memory for %1 in the background").arg(project.Name);
    emit memoryChanged();
    return;
  }
}
} // namespace neverd::agent
