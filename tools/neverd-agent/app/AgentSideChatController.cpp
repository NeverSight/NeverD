//===- AgentSideChatController.cpp - Side conversation routing -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Side conversations remain project-bound and have their own runtime/history.
//===----------------------------------------------------------------------===//
#include "AgentController.h"

#include <QDir>
#include <QFileInfo>
#include <QUuid>

namespace neverd::agent {
SideChat *AgentController::sideChat() const {
  return SideChats.value(ActiveId + '/' + ActiveSideIds.value(ActiveId));
}
QVariantList AgentController::sideChats() const {
  QVariantList result;
  for (const auto &id : SideChatIds.value(ActiveId)) {
    const auto *chat = SideChats.value(ActiveId + '/' + id);
    result.append(QVariantMap{
        {"id", id}, {"title", chat->title()}, {"running", chat->running()}});
  }
  return result;
}
void AgentController::connectSideChat(SideChat *chat,
                                      const QString &projectId) {
  chat->ImportObject = objectImportFor(projectId, chat->id());
  connect(chat, &SideChat::memorySource, this,
          [this, projectId](const QJsonObject &source) {
            queueMemory(projectId, source);
          });
  connect(chat, &SideChat::artifactsChanged, this,
          &AgentController::reviewChanged);
  connect(chat, &SideChat::nextInputRequested, this, [this, chat, projectId] {
    if (WritingProjects.contains(projectId)) {
      chat->cancel();
      return;
    }
    chat->WorkerPath = resolveWorker();
    chat->UseMemory = UseMemory;
    chat->GenerateMemory = GenerateMemory;
    if (chat->pendingInputs().isEmpty() || chat->running() ||
        chat->queuePaused())
      return;
    QString error, key;
    const auto *project = findProject(projectId);
    foregroundStarted();
    if (!Profile.validate(error) || !readModelKey(key, error) ||
        !chat->startNext(Profile, std::move(key), resolveMcp(),
                         sharedMemoryFor(*project), error)) {
      chat->cancel();
      if (!error.isEmpty())
        emit operationError(error);
    }
  });
  connect(chat, &SideChat::operationError, this,
          &AgentController::operationError);
  connect(chat, &SideChat::titleChanged, this, [this, projectId] {
    if (projectId == ActiveId)
      emit sideChatsChanged();
  });
  connect(chat, &SideChat::runChanged, this, [this, projectId] {
    if (projectId == ActiveId)
      emit sideChatsChanged();
  });
  connect(chat, &SideChat::finished, this, [this, projectId] {
    scheduleMemory();
    if (projectId == ActiveId)
      refreshFiles();
  });
}
bool AgentController::loadSideChats(const Project &project, QString &error) {
  QJsonObject index;
  if (!ProjectStore::readJson(QDir(ProjectStore::stateDirectory(project))
                                  .filePath("side-chats.json"),
                              index, error, true))
    return false;
  if (index.isEmpty() &&
      !QFileInfo::exists(QDir(ProjectStore::stateDirectory(project))
                             .filePath("side-chats.json")))
    return true;
  if (index["version"].toInt() != 1 || !index["sessions"].isArray() ||
      !index["active"].isString()) {
    error = "Invalid side conversation index.";
    return false;
  }
  QStringList ids;
  std::vector<std::unique_ptr<SideChat>> prepared;
  for (const auto &value : index["sessions"].toArray()) {
    const auto id = value.toString();
    if (id.isEmpty() || ProjectStore::sessionDirectory(project, id).isEmpty() ||
        ids.contains(id)) {
      error = "Invalid or repeated side conversation ID.";
      return false;
    }
    auto chat = std::make_unique<SideChat>(project, id);
    if (!chat->load(error))
      return false;
    ids.append(id);
    prepared.push_back(std::move(chat));
  }
  const auto active = index["active"].toString();
  if ((!ids.isEmpty() && !ids.contains(active)) ||
      (ids.isEmpty() && !active.isEmpty())) {
    error = "Active side conversation is missing.";
    return false;
  }
  for (auto &entry : prepared) {
    auto *chat = entry.release();
    chat->setParent(this);
    SideChats.insert(project.Id + '/' + chat->id(), chat);
    connectSideChat(chat, project.Id);
  }
  SideChatIds.insert(project.Id, ids);
  ActiveSideIds.insert(project.Id, active);
  return true;
}
bool AgentController::saveSideIndex(const Project &project, QString &error) {
  return ProjectStore::writeJson(
      QDir(ProjectStore::stateDirectory(project)).filePath("side-chats.json"),
      {{"version", 1},
       {"sessions", QJsonArray::fromStringList(SideChatIds.value(project.Id))},
       {"active", ActiveSideIds.value(project.Id)}},
      error);
}
bool AgentController::createSideChat() {
  const auto *project = findProject(ActiveId);
  if (!project)
    return false;
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto chat = std::make_unique<SideChat>(*project, id);
  QString error;
  if (!chat->load(error)) {
    emit operationError(error);
    return false;
  }
  const auto sourceMessages = Conversation["messages"].toArray();
  const auto *runtime = Runtimes.value(ActiveId);
  const bool inFlight = runtime && runtime->running();
  QString visibleAnswer;
  for (const auto &value : sourceMessages)
    if (value.toObject()["role"] == "assistant" &&
        value.toObject()["status"] == "running")
      visibleAnswer = value.toObject()["text"].toString();
  auto context = inFlight ? runtime->forkContext(visibleAnswer) : QJsonObject{};
  if (context.isEmpty()) {
    if (!ProjectStore::readJson(QDir(ProjectStore::stateDirectory(*project))
                                    .filePath("router/context.json"),
                                context, error, true)) {
      emit operationError(error);
      return false;
    }
    // A just-started runtime has not loaded its context yet. Preserve its
    // current question as well as already persisted turns. Old transcript-only
    // projects can also seed a branch without losing their visible history.
    if (inFlight || context.isEmpty()) {
      qsizetype first = 0;
      if (inFlight)
        for (qsizetype i = 0; i < sourceMessages.size(); ++i)
          if (sourceMessages[i].toObject()["role"] == "user")
            first = i;
      QString history;
      for (qsizetype i = first; i < sourceMessages.size(); ++i) {
        const auto message = sourceMessages[i].toObject();
        if (!message["text"].toString().isEmpty())
          history += message["role"].toString() + ": " +
                     message["text"].toString() + '\n';
      }
      if (!history.isEmpty()) {
        if (context.isEmpty())
          context = {{"version", 2},
                     {"profile", Profile.identity()},
                     {"checkpoint", ""},
                     {"usage", ContextUsage{}.toJson(Profile)},
                     {"groups", QJsonArray{}}};
        auto groups = context["groups"].toArray();
        const QString prompt = "Main conversation history at the fork point";
        groups.append(QJsonObject{
            {"prompt", prompt},
            {"answer", history},
            {"items",
             QJsonArray{ProviderCodec::message("user", prompt),
                        ProviderCodec::message("assistant", history)}}});
        context["groups"] = groups;
      }
    }
  }
  if (!chat->inherit(Conversation, context, error)) {
    emit operationError(error);
    return false;
  }
  const auto previous = ActiveSideIds.value(ActiveId);
  SideChatIds[ActiveId].append(id);
  ActiveSideIds[ActiveId] = id;
  if (!saveSideIndex(*project, error)) {
    SideChatIds[ActiveId].removeLast();
    ActiveSideIds[ActiveId] = previous;
    emit operationError(error);
    return false;
  }
  auto *created = chat.release();
  created->setParent(this);
  SideChats.insert(ActiveId + '/' + id, created);
  connectSideChat(created, ActiveId);
  emit sideChatsChanged();
  emit sideChatChanged();
  return true;
}
bool AgentController::selectSideChat(const QString &id) {
  if (!SideChatIds.value(ActiveId).contains(id))
    return false;
  const auto previous = ActiveSideIds.value(ActiveId);
  ActiveSideIds[ActiveId] = id;
  QString error;
  if (!saveSideIndex(*findProject(ActiveId), error)) {
    ActiveSideIds[ActiveId] = previous;
    emit operationError(error);
    return false;
  }
  emit sideChatChanged();
  return true;
}
bool AgentController::sendSideRequest() { return submitSideRequest("queue"); }
bool AgentController::submitSideRequest(const QString &mode) {
  const auto *project = findProject(ActiveId);
  auto *chat = sideChat();
  if (!project || !chat)
    return false;
  if (WritingProjects.contains(ActiveId)) {
    emit operationError(
        tr("Saving NeverD changes. Send after the save finishes."));
    return false;
  }
  chat->WorkerPath = resolveWorker();
  chat->UseMemory = UseMemory;
  chat->GenerateMemory = GenerateMemory;
  QString error, key;
  if (!Profile.validate(error) || !readModelKey(key, error)) {
    emit operationError(error);
    return false;
  }
  chat->updateProject(*project);
  foregroundStarted();
  if (!chat->send(Profile, std::move(key), resolveMcp(),
                  sharedMemoryFor(*project), error, mode)) {
    if (!error.isEmpty())
      emit operationError(error);
    return false;
  }
  return true;
}
void AgentController::quoteSideAnswer(const QString &messageId) {
  const auto *chat = sideChat();
  if (!chat)
    return;
  auto text = chat->answer(messageId);
  if (text.isEmpty())
    return;
  text.replace('\n', "\n> ");
  text = tr("Quoted from side chat '%1':\n\n> %2").arg(chat->title(), text);
  setDraft(draft().isEmpty() ? text : draft() + "\n\n" + text);
  closePreview();
  emit notice(tr("Quoted in the main conversation draft."));
}
} // namespace neverd::agent
