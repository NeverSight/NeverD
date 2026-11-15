//===- AgentInputController.cpp - Queued and steering conversation input --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AgentController.h"

namespace neverd::agent {
bool AgentController::commitConversation(const QString &projectId,
                                         const QJsonObject &value) {
  QString error;
  if (!findProject(projectId))
    return false;
  if (!ProjectStore::saveConversation(*findProject(projectId), value, error)) {
    emit operationError(error);
    return false;
  }
  Conversations[projectId] = value;
  if (projectId == ActiveId) {
    Conversation = value;
    DraftDirty = false;
    emit conversationChanged();
    emit messagesChanged();
  }
  return true;
}
bool AgentController::submitRequest(const QString &objectId,
                                    const QString &mode) {
  const auto *project = findProject(ActiveId);
  if (!project || draft().trimmed().isEmpty() ||
      (!objectId.isEmpty() && !findObject(*project, objectId)))
    return false;
  if (mode != "queue" && mode != "steer") {
    emit operationError(tr("Unknown message send mode."));
    return false;
  }
  const auto input = ConversationInput::capture(
      draft(), objectId, Conversation["context"].toArray());
  auto value = Conversation;
  value["draft"] = "";
  value["context"] = QJsonArray{};
  if (mode == "steer" && running()) {
    if (Runtimes[ActiveId]->selectedObjectId() != objectId) {
      emit operationError(tr("The running analysis keeps its object scope. "
                             "Queue messages for another object for the next "
                             "round."));
      return false;
    }
    auto messages = value["messages"].toArray();
    auto message = input.message("pending");
    message["inputKind"] = "steer";
    messages.append(message);
    value["messages"] = messages;
    if (!commitConversation(ActiveId, value))
      return false;
    QString error;
    if (!Runtimes[ActiveId]->steer(input.Id, input.prompt(), error,
                                   input.Text)) {
      updateAnswer(ActiveId, input.Id, input.Text, "failed", false);
      emit operationError(error);
      return false;
    }
    return true;
  }
  auto pending = value["pendingInputs"].toArray();
  pending.append(input.toJson());
  value["pendingInputs"] = pending;
  value["queuePaused"] = false;
  if (!commitConversation(ActiveId, value))
    return false;
  // Explicit queued work takes precedence over an automatically scheduled goal
  // round, without cancelling the round already in progress.
  pauseProjectGoal(ActiveId);
  return running() || dispatchQueuedInput(ActiveId);
}
bool AgentController::dispatchQueuedInput(const QString &projectId) {
  const auto *runtime = Runtimes.value(projectId);
  const auto value =
      projectId == ActiveId ? Conversation : Conversations.value(projectId);
  if (WritingProjects.contains(projectId) ||
      (runtime && (runtime->running() || runtime->reviewPending())) ||
      value["queuePaused"].toBool())
    return false;
  const auto pending = value["pendingInputs"].toArray();
  if (pending.isEmpty())
    return false;
  QString error;
  const auto input =
      ConversationInput::fromJson(pending.first().toObject(), error);
  if (!input) {
    emit operationError(error);
    return false;
  }
  if (!beginRequest(projectId, input->prompt(), input->ObjectId, false,
                    input->toJson())) {
    setQueuePaused(projectId, true);
    return false;
  }
  return true;
}
void AgentController::setQueuePaused(const QString &projectId, bool paused) {
  auto value =
      projectId == ActiveId ? Conversation : Conversations.value(projectId);
  value["queuePaused"] = paused;
  commitConversation(projectId, value);
}
void AgentController::discardQueuedInput(const QString &id) {
  auto value = Conversation;
  auto pending = value["pendingInputs"].toArray();
  for (qsizetype i = 0; i < pending.size(); ++i)
    if (pending[i].toObject()["id"] == id) {
      pending.removeAt(i);
      value["pendingInputs"] = pending;
      commitConversation(ActiveId, value);
      return;
    }
}
bool AgentController::resumeInputQueue() {
  if (!findProject(ActiveId) || running())
    return false;
  setQueuePaused(ActiveId, false);
  pauseProjectGoal(ActiveId);
  return dispatchQueuedInput(ActiveId);
}
QVariantList AgentController::execution(const QString &runId) {
  const auto *project = findProject(ActiveId);
  if (!project)
    return {};
  if (const auto *runtime = Runtimes.value(ActiveId);
      runtime && runtime->runId() == runId)
    return runtime->execution();
  for (const auto &id : SideChatIds.value(ActiveId)) {
    const auto *chat = SideChats.value(ActiveId + '/' + id);
    if (chat->runId() == runId)
      return chat->execution();
  }
  QString error;
  const auto result = ExecutionTrace::read(*project, runId, error);
  if (!error.isEmpty())
    emit operationError(error);
  return result;
}
} // namespace neverd::agent
