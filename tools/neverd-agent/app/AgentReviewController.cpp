//===- AgentReviewController.cpp - Explicit reviewed ND writes ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AgentController.h"
#include "platform/AppPaths.h"
#include "project/ResearchPlan.h"
#include "runtime/ChangeReview.h"

#include <QDir>
#include <QFileInfo>

namespace neverd::agent {
QString AgentController::resolveWorker() const {
  return findWorkerExecutable(GuiPath, McpPath);
}
bool AgentController::projectHasAnalysis(const QString &projectId) const {
  if (InspectionJobs.contains(projectId))
    return true;
  if (const auto *runtime = Runtimes.value(projectId);
      runtime && (runtime->running() || runtime->reviewPending()))
    return true;
  for (const auto &id : SideChatIds.value(projectId)) {
    const auto *chat = SideChats.value(projectId + '/' + id);
    if (chat->running() || chat->reviewPending())
      return true;
  }
  return false;
}
QVariantList AgentController::changes() {
  const auto *project = findProject(ActiveId);
  if (!project)
    return {};
  QString error;
  auto result = ChangeReview::list(*project, error);
  if (!error.isEmpty())
    emit operationError(error);
  return result;
}
QVariantMap AgentController::researchPlan(bool side) {
  const auto *project = findProject(ActiveId);
  if (!project || (side && !sideChat()))
    return {};
  QString error;
  QJsonObject plan;
  if (!ResearchPlan::read(*project, side ? sideChat()->id() : QString{}, plan,
                          error)) {
    emit operationError(error);
    return {};
  }
  return plan.toVariantMap();
}
void AgentController::refreshResearch() { emit reviewChanged(); }
bool AgentController::applyChange(const QString &id, bool undo) {
  const auto *project = findProject(ActiveId);
  if (!project)
    return false;
  QJsonObject proposal;
  for (const auto &entry : changes())
    if (entry.toMap()["id"] == id)
      proposal = QJsonObject::fromVariantMap(entry.toMap());
  const auto *object = findObject(*project, proposal["objectId"].toString());
  if (!object)
    return false;
  const auto sourcePath = QFileInfo(object->Path).canonicalFilePath();
  QSet<QString> affected;
  for (const auto &candidate : Projects)
    for (const auto &target : candidate.Objects)
      if (QFileInfo(target.Path).canonicalFilePath() == sourcePath)
        affected.insert(candidate.Id);
  for (const auto &projectId : affected)
    if (WritingProjects.contains(projectId) || projectHasAnalysis(projectId)) {
      emit operationError(tr("该对象仍有分析、侧边聊天或证据查询在运行。完成或"
                             "停止后再应用变更。"));
      return false;
    }
  const auto worker = resolveWorker();
  if (worker.isEmpty()) {
    emit operationError(tr("未找到配套的 neverd-worker；请在设置中指定完整的 "
                           "ND MCP 或 GUI 安装路径。"));
    return false;
  }
  const Project owner = *project;
  WritingProjects.unite(affected);
  for (const auto &projectId : affected) {
    pauseProjectGoal(projectId);
    if (auto *runtime = Runtimes.value(projectId))
      runtime->retireEngines();
    for (const auto &chatId : SideChatIds.value(projectId))
      SideChats[projectId + '/' + chatId]->retireEngines();
    for (const auto &key : Inspectors.keys())
      if (key.startsWith(projectId + '/'))
        delete Inspectors.take(key);
  }
  emit reviewChanged();
  ChangeReview::apply(
      owner, id, undo, worker, Profile.TimeoutSeconds, this,
      [this, affected](const auto &result) {
        WritingProjects.subtract(affected);
        emit reviewChanged();
        if (result.contains("error"))
          emit operationError(result["error"].toString());
        else
          emit notice(result["status"] == "undone" ? tr("变更已撤销并保存。")
                      : result["status"] == "no_change"
                          ? tr("ND 当前值与提案一致，没有新增变更。")
                          : tr("变更已应用并保存。"));
        for (const auto &projectId : affected)
          dispatchQueuedInput(projectId);
      });
  return true;
}
void AgentController::dismissChange(const QString &id) {
  const auto *project = findProject(ActiveId);
  if (!project || reviewBusy())
    return;
  QString error;
  if (!ChangeReview::dismiss(*project, id, error))
    emit operationError(error);
  emit reviewChanged();
}
} // namespace neverd::agent
