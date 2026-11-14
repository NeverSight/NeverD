//===- AgentModelController.cpp - Desktop AI actions ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Capture run configuration and route asynchronous updates by project
/// identity.
//===----------------------------------------------------------------------===//
#include "AgentController.h"
#include "models/Credentials.h"
#include "platform/AppPaths.h"
#include "project/MemoryStore.h"

#include <QDateTime>
#include <QDir>
#include <QFutureWatcher>
#include <QPair>
#include <QUuid>
#include <QtConcurrentRun>
#include <algorithm>

namespace neverd::agent {
QVariantMap AgentController::modelSettings() const {
  auto value = Profile.toJson();
  value.remove("credentialId");
  value["hasKey"] = !Profile.CredentialId.isEmpty();
  return value.toVariantMap();
}
bool AgentController::running() const {
  const auto *runtime = Runtimes.value(ActiveId);
  return runtime && runtime->running();
}
QVariantList AgentController::progress() const {
  const auto *runtime = Runtimes.value(ActiveId);
  return runtime ? runtime->progress() : QVariantList{};
}
QVariantMap AgentController::contextUsage() const {
  if (const auto *runtime = Runtimes.value(ActiveId))
    return runtime->contextUsage();
  const auto *project = findProject(ActiveId);
  if (!project)
    return {};
  QJsonObject saved;
  QString error;
  ContextUsage usage;
  if (!ProjectStore::readJson(QDir(ProjectStore::stateDirectory(*project))
                                  .filePath("router/context.json"),
                              saved, error, true) ||
      saved["profile"] != Profile.identity() ||
      !usage.restore(saved["usage"].toObject()))
    return {};
  return usage.toJson(Profile).toVariantMap();
}
bool AgentController::updateModelSettings(const QVariantMap &settings,
                                          const QString &key, bool clearKey,
                                          const QString &mcpPath) {
  if (credentialBusy()) {
    emit operationError(tr("Accessing the system credential store. Wait a "
                           "moment, then save again."));
    return false;
  }
  QString error;
  auto config = QJsonObject::fromVariantMap(settings);
  config["credentialId"] = clearKey ? "" : Profile.CredentialId;
  auto candidate = ModelProfile::fromJson(config, error);
  if (!candidate || !candidate->validate(error) ||
      !Credentials::validateKey(key, error) ||
      !keyMayTravel(*candidate, key, clearKey, error)) {
    emit operationError(error);
    return false;
  }
  if (key.isEmpty())
    return commitModelSettings(*candidate, mcpPath, {});
  auto *watcher = new QFutureWatcher<QPair<QString, QString>>(this);
  ++CredentialJobs;
  emit credentialChanged();
  connect(watcher, &QFutureWatcher<QPair<QString, QString>>::finished, this,
          [this, watcher, candidate = *candidate, mcpPath, key]() mutable {
            const auto result = watcher->result();
            watcher->deleteLater();
            --CredentialJobs;
            if (!result.second.isEmpty()) {
              emit credentialChanged();
              emit operationError(result.second);
              return;
            }
            candidate.CredentialId = result.first;
            commitModelSettings(candidate, mcpPath, key);
            emit credentialChanged();
          });
  watcher->setFuture(QtConcurrent::run([directory = DataDirectory, key] {
    QPair<QString, QString> result;
    Credentials(directory).store(key, result.first, result.second);
    return result;
  }));
  return true;
}

// A saved key belongs to the server it was saved for; a key in the form is
// typed for the form's server. Either travels only over HTTPS, or over plain
// HTTP to this computer.
bool AgentController::keyMayTravel(const ModelProfile &candidate,
                                   const QString &key, bool clearKey,
                                   QString &error) const {
  const bool savedKey =
      key.isEmpty() && !clearKey && !Profile.CredentialId.isEmpty();
  if (savedKey &&
      !ModelProfile::sameServer(Profile.BaseUrl, candidate.BaseUrl)) {
    error = tr("The API URL now names a different server. Enter the API key "
               "for that server, or remove the saved key.");
    return false;
  }
  if ((savedKey || !key.isEmpty()) && !candidate.carriesKeysSafely()) {
    error = tr("An API key is sent only over HTTPS, or over HTTP to this "
               "computer. Use an HTTPS address for a remote server.");
    return false;
  }
  return true;
}

bool AgentController::commitModelSettings(const ModelProfile &profile,
                                          const QString &mcpPath,
                                          const QString &newKey) {
  const auto old = Profile;
  Learner.cancel();
  const auto oldMcp = McpPath;
  Profile = profile;
  McpPath =
      mcpPath.trimmed().isEmpty() ? QString{} : localPath(mcpPath.trimmed());
  QString error;
  if (!saveSettings(error)) {
    Profile = old;
    McpPath = oldMcp;
    if (!newKey.isEmpty())
      removeCredential(profile.CredentialId);
    emit operationError(error);
    return false;
  }
  if (!newKey.isEmpty() || Profile.CredentialId.isEmpty()) {
    CachedCredentialId = Profile.CredentialId;
    CachedKey = newKey;
    CredentialError.clear();
  }
  if (!old.CredentialId.isEmpty() && old.CredentialId != Profile.CredentialId)
    removeCredential(old.CredentialId);
  emit settingsChanged();
  emit credentialChanged();
  emit modelSettingsSaved();
  return true;
}

QString AgentController::credentialStatus() const {
  return credentialBusy() ? tr("Accessing the system credential store.")
                          : CredentialError;
}

bool AgentController::readModelKey(QString &key, QString &error) const {
  key.clear();
  if (Profile.CredentialId.isEmpty())
    return true;
  if (CachedCredentialId != Profile.CredentialId ||
      !CredentialError.isEmpty()) {
    error = credentialBusy()
                ? tr("The API key is still being read. Try again in a moment.")
                : CredentialError;
    if (error.isEmpty())
      error = tr("The API key is not ready. Reload it from the system "
                 "keychain in model settings.");
    return false;
  }
  key = CachedKey;
  return true;
}

void AgentController::reloadCredential() {
  if (credentialBusy())
    return;
  CachedCredentialId.clear();
  CachedKey.clear();
  CredentialError.clear();
  if (Profile.CredentialId.isEmpty()) {
    emit credentialChanged();
    return;
  }
  const auto id = Profile.CredentialId;
  auto *watcher = new QFutureWatcher<QPair<QString, QString>>(this);
  ++CredentialJobs;
  emit credentialChanged();
  connect(watcher, &QFutureWatcher<QPair<QString, QString>>::finished, this,
          [this, watcher, id] {
            const auto result = watcher->result();
            watcher->deleteLater();
            --CredentialJobs;
            CachedCredentialId = id;
            CachedKey = result.first;
            CredentialError = result.second;
            emit credentialChanged();
          });
  watcher->setFuture(QtConcurrent::run([directory = DataDirectory, id] {
    QPair<QString, QString> result;
    Credentials(directory).read(id, result.first, result.second);
    return result;
  }));
}

void AgentController::removeCredential(const QString &id) {
  auto *watcher = new QFutureWatcher<bool>(this);
  ++CredentialJobs;
  emit credentialChanged();
  connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher] {
    const bool removed = watcher->result();
    watcher->deleteLater();
    --CredentialJobs;
    if (!removed)
      emit operationError(tr("Settings were updated, but the old credential "
                             "could not be removed. Check the permissions of "
                             "the system credential store."));
    emit credentialChanged();
  });
  watcher->setFuture(QtConcurrent::run([directory = DataDirectory, id] {
    return Credentials(directory).remove(id);
  }));
}
void AgentController::testConnection(const QVariantMap &settings,
                                     const QString &key, bool clearKey) {
  if (TestClient)
    return;
  QString error, secret = key;
  auto config = QJsonObject::fromVariantMap(settings);
  config["credentialId"] = Profile.CredentialId;
  auto profile = ModelProfile::fromJson(config, error);
  if (!profile || !profile->validate(error) ||
      !Credentials::validateKey(secret, error) ||
      !keyMayTravel(*profile, secret, clearKey, error) ||
      (secret.isEmpty() && !clearKey && !readModelKey(secret, error))) {
    ConnectionStatus = error;
    emit connectionChanged();
    return;
  }
  profile->OutputTokens = std::min(profile->OutputTokens,
                                   model_limits::kConnectionTestOutputTokens);
  TestClient = new ModelClient(this);
  connect(
      TestClient, &ModelClient::finished, this, [this](const ModelTurn &turn) {
        ConnectionStatus = turn.Error.isEmpty()
                               ? tr("Connected · %1").arg(turn.Text.left(120))
                               : turn.Error;
        TestClient->deleteLater();
        TestClient.clear();
        emit connectionChanged();
      });
  ConnectionStatus = tr("Contacting the model.");
  emit connectionChanged();
  TestClient->start(*profile, secret, "Reply briefly.",
                    {ProviderCodec::message("user", "Reply with OK.")}, {});
}
void AgentController::cancelConnectionTest() {
  if (TestClient)
    TestClient->cancel();
}
QString AgentController::resolveMcp() const {
  return findMcpExecutable(GuiPath, McpPath);
}
QJsonArray AgentController::sharedMemoryFor(const Project &project) {
  QJsonArray shared;
  QString error;
  if (Sharing)
    for (const auto &other : Projects)
      if (other.Id != project.Id)
        for (const auto &object : other.Objects) {
          bool match = false;
          for (const auto &own : project.Objects)
            if (own.Sha256 == object.Sha256)
              match = true;
          if (!match)
            continue;
          QJsonArray records;
          if (!MemoryStore::read(other, object.Id, records, error)) {
            emit notice(error);
            continue;
          }
          for (const auto &record : records) {
            auto entry = record.toObject();
            entry["sha256"] = object.Sha256;
            entry["sourceProject"] = other.Name;
            shared.append(entry);
          }
        }
  return shared;
}
bool AgentController::sendRequest(const QString &objectId) {
  return submitRequest(objectId, "queue");
}
bool AgentController::beginRequest(const QString &projectId,
                                   const QString &prompt,
                                   const QString &objectId, bool goalRound,
                                   const QJsonObject &input) {
  const auto *project = findProject(projectId);
  const auto *existing = Runtimes.value(projectId);
  if (!project || WritingProjects.contains(projectId) ||
      (existing && (existing->running() || existing->reviewPending())))
    return false;
  if (!objectId.isEmpty() && !findObject(*project, objectId))
    return false;
  QString error, key;
  if (!input.isEmpty() && !EvidenceStore::validateBindings(
                              *project, input["context"].toArray(), error)) {
    emit operationError(error);
    return false;
  }
  if (!Profile.validate(error) || !readModelKey(key, error)) {
    emit operationError(error);
    return false;
  }
  const auto goalId = goalRound ? Goals.value(projectId).Id : QString{};
  const auto messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto conversation =
      projectId == ActiveId ? Conversation : Conversations.value(projectId);
  auto messages = conversation["messages"].toArray();
  if (!input.isEmpty()) {
    const auto captured = ConversationInput::fromJson(input, error);
    if (!captured) {
      emit operationError(error);
      return false;
    }
    messages.append(captured->message());
    auto pending = conversation["pendingInputs"].toArray();
    pending.removeFirst();
    conversation["pendingInputs"] = pending;
  }
  if (goalRound && Goals.value(projectId).Rounds == 0)
    messages.append(QJsonObject{
        {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"role", "user"},
        {"text", Goals.value(projectId).Objective},
        {"status", "saved"},
        {"objectId", ""},
        {"goalId", goalId},
        {"createdAt",
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
  messages.append(QJsonObject{
      {"id", messageId},
      {"role", "assistant"},
      {"text", ""},
      {"status", "running"},
      {"objectId", objectId},
      {"goalId", goalId},
      {"goalRound", goalRound ? Goals.value(projectId).Rounds + 1 : 0},
      {"createdAt",
       QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
  conversation["messages"] = messages;
  if (!commitConversation(projectId, conversation))
    return false;
  auto *runtime = Runtimes.value(projectId);
  if (!runtime) {
    runtime = new ResearchRuntime(this);
    Runtimes.insert(projectId, runtime);
  }
  runtime->disconnect(this);
  connect(runtime, &ResearchRuntime::memorySource, this,
          [this, projectId](const QJsonObject &source) {
            queueMemory(projectId, source);
          });
  connect(runtime, &ResearchRuntime::artifactsChanged, this,
          &AgentController::reviewChanged);
  connect(runtime, &ResearchRuntime::executionRow, this,
          [this, projectId](const QString &runId, qsizetype index,
                            const QJsonObject &record) {
            if (projectId == ActiveId)
              emit executionRow(runId, index, record);
          });
  connect(runtime, &ResearchRuntime::changed, this, [this, projectId, goalId] {
    if (!goalId.isEmpty())
      goalRunChanged(projectId, goalId);
    if (projectId == ActiveId)
      emit runChanged();
  });
  connect(runtime, &ResearchRuntime::textDelta, this,
          [this, projectId, messageId](const QString &text) {
            updateAnswer(projectId, messageId, text, "running", true);
          });
  connect(runtime, &ResearchRuntime::steeringApplied, this,
          [this, projectId](const QString &inputId) {
            auto value = projectId == ActiveId ? Conversation
                                               : Conversations.value(projectId);
            auto messages = value["messages"].toArray();
            for (qsizetype i = 0; i < messages.size(); ++i) {
              auto message = messages[i].toObject();
              if (message["id"] == inputId) {
                message["status"] = "saved";
                messages[i] = message;
              }
            }
            value["messages"] = messages;
            commitConversation(projectId, value);
          });
  connect(runtime, &ResearchRuntime::finished, this,
          [this, projectId, messageId,
           goalId](const QString &text, const QString &error, bool cancelled) {
            auto current = projectId == ActiveId
                               ? Conversation
                               : Conversations.value(projectId);
            auto pendingMessages = current["messages"].toArray();
            for (qsizetype i = 0; i < pendingMessages.size(); ++i) {
              auto message = pendingMessages[i].toObject();
              if (message["status"] == "pending") {
                message["status"] = "interrupted";
                pendingMessages[i] = message;
              }
            }
            current["messages"] = pendingMessages;
            commitConversation(projectId, current);
            QString display = text;
            if (!error.isEmpty()) {
              const auto conversation = projectId == ActiveId
                                            ? Conversation
                                            : Conversations.value(projectId);
              if (display.isEmpty())
                for (const auto &entry : conversation["messages"].toArray())
                  if (entry.toObject()["id"] == messageId)
                    display = entry.toObject()["text"].toString();
              display += "\n\n" + error;
            }
            updateAnswer(projectId, messageId, display,
                         cancelled         ? "cancelled"
                         : error.isEmpty() ? "succeeded"
                                           : "failed",
                         false);
            QString storageError;
            if (const auto *target = findProject(projectId))
              if (!ProjectStore::saveConversation(
                      *target, Conversations.value(projectId), storageError))
                emit operationError(storageError);
            DirtyRuns.remove(projectId);
            if (cancelled || !error.isEmpty())
              setQueuePaused(projectId, true);
            if (projectId == ActiveId) {
              emit runChanged();
              refreshFiles();
            }
            if (!goalId.isEmpty())
              goalRunFinished(projectId, goalId, error, cancelled);
            emit runFinished(projectId, text, error, cancelled);
            scheduleMemory();
            if (!cancelled && error.isEmpty())
              QTimer::singleShot(0, this, [this, projectId] {
                dispatchQueuedInput(projectId);
              });
          });
  const auto shared = sharedMemoryFor(*project);
  ResearchRunOptions options;
  options.UserPrompt = goalRound ? Goals.value(projectId).Objective
                                 : input["text"].toString(prompt);
  options.UseMemory = UseMemory;
  options.GenerateMemory = GenerateMemory;
  options.GuiAttachment = WorkbenchAttachment;
  options.WorkerPath = resolveWorker();
  options.ImportObject = objectImportFor(projectId);
  if (goalRound) {
    const auto &goal = Goals[projectId];
    options.Budget = GoalBudgets.value(projectId);
    options.Goal = {{"id", goal.Id},
                    {"objective", goal.Objective},
                    {"summary", goal.Summary},
                    {"nextStep", goal.NextStep},
                    {"round", goal.Rounds + 1}};
  }
  foregroundStarted();
  if (!runtime->start(*project, Profile, key, resolveMcp(), prompt, objectId,
                      shared, error, options)) {
    updateAnswer(projectId, messageId, error, "failed", false);
    emit operationError(error);
    return false;
  }
  conversation =
      projectId == ActiveId ? Conversation : Conversations.value(projectId);
  messages = conversation["messages"].toArray();
  for (qsizetype i = 0; i < messages.size(); ++i) {
    auto message = messages[i].toObject();
    if (message["id"] == messageId) {
      message["runId"] = runtime->runId();
      messages[i] = message;
    }
  }
  conversation["messages"] = messages;
  if (!commitConversation(projectId, conversation)) {
    runtime->cancel();
    return false;
  }
  emit messagesChanged();
  emit runChanged();
  return true;
}
void AgentController::updateAnswer(const QString &projectId,
                                   const QString &messageId,
                                   const QString &text, const QString &status,
                                   bool append) {
  auto conversation =
      projectId == ActiveId ? Conversation : Conversations.value(projectId);
  auto messages = conversation["messages"].toArray();
  for (qsizetype i = 0; i < messages.size(); ++i) {
    auto message = messages[i].toObject();
    if (message["id"] != messageId)
      continue;
    message["text"] = append ? message["text"].toString() + text : text;
    message["status"] = status;
    messages[i] = message;
    break;
  }
  conversation["messages"] = messages;
  Conversations[projectId] = conversation;
  if (projectId == ActiveId) {
    Conversation = conversation;
    if (!StreamTimer.isActive())
      StreamTimer.start();
  }
  DirtyRuns.insert(projectId);
  if (!RunSaveTimer.isActive())
    RunSaveTimer.start();
}
void AgentController::cancelRun() {
  setQueuePaused(ActiveId, true);
  pauseProjectGoal(ActiveId);
  if (auto *runtime = Runtimes.value(ActiveId))
    runtime->cancel();
}
} // namespace neverd::agent
