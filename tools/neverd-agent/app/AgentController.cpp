//===- AgentController.cpp - Agent desktop application state --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Persist user actions with project identities and keep file hashing off the
/// UI thread. Model actions are implemented in AgentModelController.cpp.
//===----------------------------------------------------------------------===//

#include "AgentController.h"

#include "Config.h"
#include "platform/AppPaths.h"
#include "project/MemoryStore.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QUrl>
#include <QUuid>
#include <QtConcurrentRun>
#include <utility>

namespace neverd::agent {
namespace {
QVariantMap objectView(const AnalysisObject &object) {
  return {{"id", object.Id},
          {"name", object.Name},
          {"path", object.Path},
          {"sha256", object.Sha256},
          {"exists", QFileInfo(object.Path).isFile()}};
}

QVariantMap projectView(const Project &project) {
  QVariantList objects;
  for (const auto &object : project.Objects)
    objects.append(objectView(object));
  return {{"id", project.Id},
          {"name", project.Name},
          {"directory", project.Directory},
          {"objects", objects}};
}
} // namespace

AgentController::AgentController(QString directory, QObject *parent)
    : QObject(parent), DataDirectory(std::move(directory)) {
  connect(this, &AgentController::conversationChanged, this,
          &AgentController::messagesChanged);
  StreamTimer.setSingleShot(true);
  StreamTimer.setInterval(model_limits::kStreamUiUpdateMs);
  connect(&StreamTimer, &QTimer::timeout, this,
          &AgentController::messagesChanged);
  RunSaveTimer.setInterval(model_limits::kRunAutosaveMs);
  connect(&RunSaveTimer, &QTimer::timeout, this, [this] {
    const auto dirty = DirtyRuns;
    for (const auto &id : dirty) {
      const auto *project = findProject(id);
      QString error;
      const auto value =
          id == ActiveId ? Conversation : Conversations.value(id);
      if (project && !ProjectStore::saveConversation(*project, value, error)) {
        if (auto *runtime = Runtimes.value(id))
          runtime->cancel();
        emit operationError(error);
      } else
        DirtyRuns.remove(id);
    }
    if (DirtyRuns.isEmpty())
      RunSaveTimer.stop();
  });
  DraftTimer.setSingleShot(true);
  DraftTimer.setInterval(kDraftSaveDelayMs);
  connect(&DraftTimer, &QTimer::timeout, this, [this] {
    QString error;
    if (!flush(error))
      emit operationError(error);
  });
  MemoryTimer.setSingleShot(true);
  MemoryTimer.setInterval(kMemoryIdleDelayMs);
  connect(&MemoryTimer, &QTimer::timeout, this, &AgentController::drainMemory);
  connect(this, &AgentController::credentialChanged, this, [this] {
    if (!credentialBusy())
      scheduleMemory();
  });
  connect(&Learner, &MemoryLearner::finished, this,
          [this](const QString &, int added, const QString &error) {
            MemoryStatus =
                added < 0 ? tr("Memory organization is paused. Pending records "
                               "are kept.")
                : error.isEmpty()
                    ? tr("Memory organized · new notes: %1").arg(added)
                    : tr("Memory organization did not finish: %1").arg(error);
            refreshMemory();
            emit memoryChanged();
            scheduleMemory();
          });
}

bool AgentController::loadConfiguration(QJsonObject &config, QString &error) {
  const QString path = QDir(DataDirectory).filePath("config.json");
  if (!ProjectStore::readJson(path, config, error, true))
    return false;
  if (QFileInfo::exists(path)) {
    // Version and projects are required; settings a configuration predates
    // or omits take their defaults. The theme follows the workbench, so
    // "lightTheme" is kept only for older Agent builds.
    const auto optional = [&config](const char *field,
                                    bool (QJsonValue::*is)() const) {
      return !config.contains(field) || (config.value(field).*is)();
    };
    if (config.value("version").toInt(-1) != kStorageVersion ||
        !config.value("projects").isArray() ||
        !optional("sharing", &QJsonValue::isBool) ||
        !optional("lightTheme", &QJsonValue::isBool) ||
        !optional("guiPath", &QJsonValue::isString) ||
        !optional("mcpPath", &QJsonValue::isString)) {
      error = tr("The global configuration is invalid. Check %1.").arg(path);
      return false;
    }
    Sharing = config.value("sharing").toBool();
    for (const auto *field : {"useMemory", "generateMemory"})
      if (config.contains(field) && !config[field].isBool()) {
        error = tr("The global configuration is invalid. Check %1.").arg(path);
        return false;
      }
    UseMemory = config["useMemory"].toBool(true);
    GenerateMemory = config["generateMemory"].toBool(true);
    if (config.contains("autoAddAnalysisObjects") &&
        !config["autoAddAnalysisObjects"].isBool()) {
      error = tr("Invalid setting for adding analysis objects.");
      return false;
    }
    AutoAddAnalysisObjects = config["autoAddAnalysisObjects"].toBool();
    LightTheme = config.value("lightTheme").toBool();
    GuiPath = config.value("guiPath").toString();
    McpPath = config.value("mcpPath").toString();
    if (config.contains("model")) {
      const auto profile =
          ModelProfile::fromJson(config["model"].toObject(), error);
      if (!profile)
        return false;
      Profile = *profile;
    }
  }
  return true;
}

bool AgentController::initialize(QString &error) {
  QJsonObject config;
  if (!loadConfiguration(config, error))
    return false;
  if (!config.isEmpty()) {
    for (const auto &entry : config.value("projects").toArray()) {
      if (!entry.isString() || entry.toString().isEmpty()) {
        error = tr("The global configuration contains an invalid project "
                   "path: %1")
                    .arg(QDir(DataDirectory).filePath("config.json"));
        return false;
      }
      if (!KnownDirectories.contains(entry.toString()))
        KnownDirectories.append(entry.toString());
    }
    QStringList unavailable;
    for (const auto &directory : KnownDirectories) {
      QString issue;
      auto project = ProjectStore::open(directory, issue);
      if (!project || !bindProject(*project, issue))
        unavailable.append(issue);
    }
    if (!unavailable.isEmpty()) {
      const QString message = unavailable.join('\n');
      QTimer::singleShot(0, this,
                         [this, message] { emit operationError(message); });
    }
  }
  const QString wanted = config.value("activeProjectId").toString();
  if (findProject(wanted))
    selectProject(wanted);
  else if (!Projects.isEmpty())
    selectProject(Projects.front().Id);
  if (!writeCatalog(error))
    return false;
  emit settingsChanged();
  emit workspaceChanged();
  reloadCredential();
  return true;
}

Project *AgentController::findProject(const QString &id) {
  for (auto &project : Projects)
    if (project.Id == id)
      return &project;
  return nullptr;
}

const Project *AgentController::findProject(const QString &id) const {
  for (const auto &project : Projects)
    if (project.Id == id)
      return &project;
  return nullptr;
}

const AnalysisObject *AgentController::findObject(const Project &project,
                                                  const QString &id) const {
  for (const auto &object : project.Objects)
    if (object.Id == id)
      return &object;
  return nullptr;
}

bool AgentController::bindProject(Project project, QString &error) {
  if (const auto *existing = findProject(project.Id)) {
    if (QFileInfo(existing->Directory) != QFileInfo(project.Directory)) {
      error = tr("Two directories use the same project ID: %1\n%2")
                  .arg(existing->Directory, project.Directory);
      return false;
    }
    return true;
  }
  QJsonObject conversation;
  if (!ProjectStore::readConversation(project, conversation, error))
    return false;
  auto lock = std::make_unique<QLockFile>(
      QDir(ProjectStore::stateDirectory(project)).filePath("editor.lock"));
  if (!lock->tryLock()) {
    error = tr("The project is open in another Agent window, or its "
               "directory cannot be written: %1")
                .arg(project.Directory);
    return false;
  }
  auto messages = conversation["messages"].toArray();
  bool interrupted = false;
  for (qsizetype i = 0; i < messages.size(); ++i) {
    auto message = messages[i].toObject();
    if (message["status"] == "running" || message["status"] == "pending") {
      message["status"] = "interrupted";
      messages[i] = message;
      interrupted = true;
    }
  }
  if (!conversation["pendingInputs"].toArray().isEmpty()) {
    conversation["queuePaused"] = true;
    interrupted = true;
  }
  if (interrupted) {
    conversation["messages"] = messages;
    if (!ProjectStore::saveConversation(project, conversation, error))
      return false;
  }
  GoalState goal;
  if (!GoalState::load(project, goal, error) || !loadSideChats(project, error))
    return false;
  Goals.insert(project.Id, goal);
  ProjectLocks.push_back(std::move(lock));
  Conversations.insert(project.Id, conversation);
  Projects.append(std::move(project));
  scheduleMemory();
  return true;
}

QVariantList AgentController::projects() const {
  QVariantList values;
  for (const auto &project : Projects)
    values.append(projectView(project));
  return values;
}

QVariantMap AgentController::activeProject() const {
  const auto *project = findProject(ActiveId);
  return project ? projectView(*project) : QVariantMap{};
}

QVariantMap AgentController::selectedObject() const {
  const auto *project = findProject(ActiveId);
  const auto *object =
      project ? findObject(*project, SelectedObjectId) : nullptr;
  return object ? objectView(*object) : QVariantMap{};
}

QVariantList AgentController::messages() const {
  return Conversation.value("messages").toArray().toVariantList();
}

bool AgentController::flush(QString &error) {
  DraftTimer.stop();
  for (auto *chat : std::as_const(SideChats))
    if (!chat->flush(error))
      return false;
  if (!DraftDirty)
    return true;
  const auto *project = findProject(ActiveId);
  if (project && !ProjectStore::saveConversation(*project, Conversation, error))
    return false;
  DraftDirty = false;
  return true;
}

ShutdownStatus AgentController::prepareShutdown() {
  MemoryTimer.stop();
  Learner.cancel();
  if (credentialBusy()) {
    emit notice(tr("Accessing the system credential store. Complete or cancel "
                   "the system unlock prompt, then close again."));
    return ShutdownStatus::Pending;
  }
  if (!WritingProjects.isEmpty()) {
    emit notice(tr("Saving NeverD changes. Close again after the save is "
                   "confirmed."));
    return ShutdownStatus::Pending;
  }
  if (busy()) {
    emit notice(
        tr("Finishing file registration. Try closing again in a moment."));
    return ShutdownStatus::Pending;
  }
  bool hasRuns = false;
  for (const auto &project : Projects)
    pauseProjectGoal(project.Id);
  for (auto *chat : std::as_const(SideChats)) {
    if (chat->running()) {
      hasRuns = true;
      chat->cancel();
    }
    QString error;
    if (!chat->flush(error)) {
      emit operationError(error);
      return ShutdownStatus::Failed;
    }
  }
  for (auto *runtime : std::as_const(Runtimes)) {
    if (runtime->running()) {
      hasRuns = true;
      runtime->cancel();
    }
  }
  if (hasRuns) {
    emit notice(tr("Stopping analysis and saving records. Try closing again in "
                   "a moment."));
    return ShutdownStatus::Pending;
  }
  QString error;
  if (!flush(error)) {
    emit operationError(error);
    return ShutdownStatus::Failed;
  }
  return ShutdownStatus::Ready;
}

bool AgentController::prepareClose() {
  return prepareShutdown() == ShutdownStatus::Ready;
}

bool AgentController::saveSettings(QString &error) {
  if (!WorkbenchAttachment.isEmpty())
    return true; // The workbench reads the desktop model settings as a client.
  return ProjectStore::writeJson(
      QDir(DataDirectory).filePath("config.json"),
      {{"version", kStorageVersion},
       {"projects", QJsonArray::fromStringList(KnownDirectories)},
       {"activeProjectId", ActiveId},
       {"sharing", Sharing},
       {"useMemory", UseMemory},
       {"generateMemory", GenerateMemory},
       {"lightTheme", LightTheme},
       {"autoAddAnalysisObjects", AutoAddAnalysisObjects},
       {"guiPath", GuiPath},
       {"mcpPath", McpPath},
       {"model", Profile.toJson()}},
      error);
}

bool AgentController::writeCatalog(QString &error) {
  QJsonArray entries;
  if (Sharing) {
    for (const auto &project : Projects)
      for (const auto &object : project.Objects)
        entries.append(QJsonObject{{"projectId", project.Id},
                                   {"projectDirectory", project.Directory},
                                   {"objectId", object.Id},
                                   {"sha256", object.Sha256}});
  }
  return ProjectStore::writeJson(
      QDir(DataDirectory).filePath("catalog.json"),
      {{"version", kStorageVersion}, {"objects", entries}}, error);
}

bool AgentController::createProject(const QString &name,
                                    const QString &directory) {
  QString error;
  auto project = ProjectStore::create(localPath(directory), name, error);
  if (!project) {
    emit operationError(error);
    return false;
  }
  return openProject(project->Directory);
}

bool AgentController::openProject(const QString &directory) {
  QString error;
  auto project = ProjectStore::open(localPath(directory), error);
  if (!project || !bindProject(*project, error)) {
    emit operationError(error);
    return false;
  }
  if (!KnownDirectories.contains(project->Directory))
    KnownDirectories.append(project->Directory);
  if (!selectProject(project->Id))
    return false;
  if (!writeCatalog(error))
    emit operationError(error);
  emit workspaceChanged();
  return true;
}

bool AgentController::selectProject(const QString &id) {
  if (id == ActiveId)
    return true;
  const auto *project = findProject(id);
  if (!project)
    return false;
  QString error;
  QJsonObject conversation;
  if (!flush(error) ||
      !ProjectStore::readConversation(*project, conversation, error)) {
    emit operationError(error);
    return false;
  }
  ActiveId = id;
  Evidence = {};
  emit evidenceChanged();
  emit reviewChanged();
  emit sideChatsChanged();
  emit sideChatChanged();
  emit goalChanged();
  SelectedObjectId.clear();
  Conversation =
      Conversations.contains(id) ? Conversations.value(id) : conversation;
  ExpandedDirectories.clear();
  closePreview();
  refreshFiles();
  readMemoryViews();
  if (!saveSettings(error))
    emit operationError(error);
  emit workspaceChanged();
  emit conversationChanged();
  emit objectChanged();
  emit runChanged();
  return true;
}

void AgentController::selectObject(const QString &id) {
  const auto *project = findProject(ActiveId);
  if (!project || (!id.isEmpty() && !findObject(*project, id)))
    return;
  SelectedObjectId = id;
  readMemoryViews();
  emit objectChanged();
}

void AgentController::addObject(const QString &path) {
  const auto *project = findProject(ActiveId);
  if (!project)
    return;
  const QString projectId = project->Id;
  fingerprintObject(path, [this, projectId](const FingerprintResult &result) {
    auto *target = findProject(projectId);
    QString error;
    if (!target || !ProjectStore::addObject(*target, result, error)) {
      emit operationError(target ? error
                                 : tr("The target project was closed."));
      return;
    }
    if (ActiveId == projectId)
      selectObject(target->Objects.back().Id);
    objectAdded(*target);
    emit notice(tr("Added %1. Its file fingerprint is saved; analysis has not "
                   "started.")
                    .arg(QFileInfo(result.Path).fileName()));
  });
}

void AgentController::fingerprintObject(
    const QString &path, std::function<void(FingerprintResult)> ready) {
  auto *watcher = new QFutureWatcher<FingerprintResult>(this);
  ++PendingImports;
  emit busyChanged();
  connect(watcher, &QFutureWatcher<FingerprintResult>::finished, this,
          [this, watcher, ready = std::move(ready)] {
            auto result = watcher->result();
            watcher->deleteLater();
            --PendingImports;
            emit busyChanged();
            ready(std::move(result));
          });
  watcher->setFuture(
      QtConcurrent::run([path] { return ProjectStore::fingerprint(path); }));
}

void AgentController::objectAdded(const Project &project) {
  for (const auto &id : SideChatIds.value(project.Id))
    SideChats.value(project.Id + '/' + id)->updateProject(project);
  QString error;
  if (!writeCatalog(error))
    emit operationError(error);
  emit workspaceChanged();
}

RequestObjectImport
AgentController::objectImportFor(const QString &projectId,
                                 const QString &sideChatId) {
  if (!WorkbenchAttachment.isEmpty())
    return {};
  return [this, projectId,
          sideChatId](const QString &sourceId, const QString &path,
                      const QString &reason, ObjectImportCall call,
                      ObjectImported imported) {
    requestObjectImport(projectId, sideChatId, sourceId, path, reason,
                        std::move(call), std::move(imported));
  };
}

void AgentController::requestObjectImport(
    const QString &projectId, const QString &sideChatId,
    const QString &sourceId, const QString &path, const QString &reason,
    ObjectImportCall call, ObjectImported imported) {
  const auto *project = findProject(projectId);
  const auto *source = project ? findObject(*project, sourceId) : nullptr;
  const auto fail = [&](const QString &message) {
    call->fail({"object_import", message.toStdString()});
  };
  if (!project || (!sourceId.isEmpty() && !source)) {
    fail(QStringLiteral(
        "The target project or dependency source is no longer available."));
    return;
  }
  if (path.trimmed().isEmpty() || reason.trimmed().isEmpty()) {
    fail(QStringLiteral(
        "Provide the dependency file path and the reason for adding it."));
    return;
  }
  // A network path reaches another machine as soon as it is resolved, before
  // any approval; the model may only name local files.
  if (isRemotePath(path) || isRemotePath(localPath(path.trimmed()))) {
    fail(QStringLiteral("Network paths cannot be added as analysis objects. "
                        "Provide a local file path."));
    return;
  }
  const auto base =
      source ? QFileInfo(source->Path).absolutePath() : project->Directory;
  const auto inputPath = localPath(path.trimmed());
  const QFileInfo file(QDir(base).absoluteFilePath(inputPath));
  auto canonical = file.canonicalFilePath();
  if (canonical.isEmpty() && !QDir::isAbsolutePath(inputPath) &&
      file.fileName() == inputPath) {
    // PE import names often use different casing than filenames on Unix.
    QString match;
    for (const auto &candidate : QDir(base).entryInfoList(QDir::Files))
      if (candidate.fileName().compare(inputPath, Qt::CaseInsensitive) == 0) {
        if (!match.isEmpty()) {
          fail(QStringLiteral(
              "Several dependencies match this name. Specify the full path."));
          return;
        }
        match = candidate.canonicalFilePath();
      }
    canonical = match;
  }
  if (canonical.isEmpty() || !QFileInfo(canonical).isFile()) {
    fail(QStringLiteral("Dependency file not found: %1. Provide the full path "
                        "of an existing file.")
             .arg(QDir::toNativeSeparators(file.absoluteFilePath())));
    return;
  }
  const auto state =
      QFileInfo(ProjectStore::stateDirectory(*project)).canonicalFilePath();
  const auto relative = QDir(state).relativeFilePath(canonical);
  if (!state.isEmpty() && !QDir::isAbsolutePath(relative) && relative != ".." &&
      !relative.startsWith("../")) {
    fail(QStringLiteral(
        "Agent project state files cannot be analysis objects."));
    return;
  }

  ObjectImport request{QUuid::createUuid().toString(QUuid::WithoutBraces),
                       projectId,
                       sideChatId,
                       source ? source->Name : QString{},
                       canonical,
                       reason.trimmed(),
                       call,
                       std::move(imported)};
  for (const auto &entry : project->Objects)
    if (QFileInfo(entry.Path) == QFileInfo(canonical)) {
      finishObjectImport(request, entry, "already_registered");
      return;
    }
  if (AutoAddAnalysisObjects) {
    importRequestedObject(std::move(request), true);
    return;
  }
  if (!InteractiveObjectApprovals) {
    fail(QStringLiteral("This session cannot show approval requests. Submit "
                        "the request in the desktop Agent, or register the "
                        "object manually first."));
    return;
  }
  const auto id = request.Id;
  ObjectApprovals.append(std::move(request));
  call->onCancel([guard = QPointer<AgentController>(this), id, call] {
    if (guard && guard->ObjectApprovals.removeIf(
                     [&](const auto &request) { return request.Id == id; }))
      emit guard->analysisObjectApprovalsChanged();
    call->fail({"cancelled", "Analysis object request cancelled."}, true);
  });
  emit analysisObjectApprovalsChanged();
}

QVariantList AgentController::analysisObjectApprovals() const {
  QVariantList result;
  for (const auto &request : ObjectApprovals) {
    const auto *project = findProject(request.ProjectId);
    result.append(QVariantMap{
        {"id", request.Id},
        {"projectId", request.ProjectId},
        {"sideChatId", request.SideChatId},
        {"project", project ? project->Name : request.ProjectId},
        {"projectPath",
         project ? QDir::toNativeSeparators(project->Directory) : QString{}},
        {"source", request.SourceName},
        {"path", QDir::toNativeSeparators(request.Path)},
        {"reason", request.Reason}});
  }
  return result;
}

bool AgentController::resolveAnalysisObjectApproval(const QString &id,
                                                    bool allow) {
  for (qsizetype i = 0; i < ObjectApprovals.size(); ++i) {
    if (ObjectApprovals[i].Id != id)
      continue;
    auto request = ObjectApprovals.takeAt(i);
    emit analysisObjectApprovalsChanged();
    if (allow)
      importRequestedObject(std::move(request), false);
    else
      request.Call->fail({"denied", "The user declined to add this analysis "
                                    "object. Continue with existing objects."});
    return true;
  }
  return false;
}

void AgentController::importRequestedObject(ObjectImport request,
                                            bool automatic) {
  const auto path = request.Path;
  request.Call->onCancel([call = request.Call] {
    call->fail({"cancelled", "Analysis object request cancelled."}, true);
  });
  if (plugin::terminal(request.Call->snapshot().Phase))
    return;
  fingerprintObject(path, [this, request = std::move(request),
                           automatic](const FingerprintResult &result) {
    if (plugin::terminal(request.Call->snapshot().Phase))
      return;
    if (automatic && !AutoAddAnalysisObjects) {
      request.Call->fail({"approval_required", "Automatic object addition "
                                               "was disabled. Request approval "
                                               "before trying again."});
      return;
    }
    if (QFileInfo(request.Path).canonicalFilePath() != request.Path) {
      request.Call->fail({"object_import", "The approved file path changed. "
                                           "Request the current path again."});
      return;
    }
    auto *target = findProject(request.ProjectId);
    if (!target) {
      request.Call->fail(
          {"object_import", "Target project is no longer open."});
      return;
    }
    for (const auto &entry : target->Objects)
      if (QFileInfo(entry.Path) == QFileInfo(request.Path)) {
        finishObjectImport(request, entry, "already_registered");
        return;
      }
    QString error;
    if (!ProjectStore::addObject(*target, result, error)) {
      request.Call->fail({"object_import", error.toStdString()});
      return;
    }
    const auto added = target->Objects.back();
    objectAdded(*target);
    emit notice(tr("Agent added the analysis object %1.").arg(added.Name));
    finishObjectImport(request, added, automatic ? "automatic" : "approved");
  });
}

void AgentController::finishObjectImport(const ObjectImport &request,
                                         const AnalysisObject &object,
                                         const char *decision) {
  request.Imported(object);
  NphTools::succeed(request.Call, {{"objectId", object.Id},
                                   {"name", object.Name},
                                   {"sha256", object.Sha256},
                                   {"decision", decision}});
}

void AgentController::relocateObject(const QString &projectId,
                                     const QString &objectId,
                                     const QString &path) {
  const auto *project = findProject(projectId);
  if (!project || !findObject(*project, objectId))
    return;
  if (WritingProjects.contains(projectId) || projectHasAnalysis(projectId)) {
    emit operationError(tr(
        "Finish or stop this project's analysis before relocating the file."));
    return;
  }
  auto *watcher = new QFutureWatcher<FingerprintResult>(this);
  ++PendingImports;
  emit busyChanged();
  connect(
      watcher, &QFutureWatcher<FingerprintResult>::finished, this,
      [this, watcher, projectId, objectId] {
        const auto result = watcher->result();
        watcher->deleteLater();
        --PendingImports;
        emit busyChanged();
        auto *target = findProject(projectId);
        if (!target)
          return;
        if (WritingProjects.contains(projectId) ||
            projectHasAnalysis(projectId)) {
          emit operationError(tr("The project started an analysis, so the "
                                 "file binding was not changed. Stop it and "
                                 "try again."));
          return;
        }
        QString error;
        if (!ProjectStore::relocateObject(*target, objectId, result, error)) {
          emit operationError(error);
          return;
        }
        if (auto *runtime = Runtimes.value(projectId))
          runtime->retireEngines();
        for (const auto &chatId : SideChatIds.value(projectId)) {
          auto *chat = SideChats.value(projectId + '/' + chatId);
          chat->retireEngines();
          chat->updateProject(*target);
        }
        for (const auto &key : Inspectors.keys())
          if (key.startsWith(projectId + '/'))
            delete Inspectors.take(key);
        if (!writeCatalog(error))
          emit operationError(error);
        emit workspaceChanged();
        emit objectChanged();
        emit notice(
            tr("File relocated. The object's memory and conversations are "
               "kept."));
      });
  watcher->setFuture(
      QtConcurrent::run([path] { return ProjectStore::fingerprint(path); }));
}

void AgentController::setDraft(const QString &text) {
  if (ActiveId.isEmpty() || draft() == text)
    return;
  Conversation["draft"] = text;
  Conversations[ActiveId] = Conversation;
  DraftDirty = true;
  DraftTimer.start();
}

bool AgentController::saveRequest(const QString &objectId) {
  const auto *project = findProject(ActiveId);
  if (!project || draft().trimmed().isEmpty() ||
      (!objectId.isEmpty() && !findObject(*project, objectId)))
    return false;
  auto next = Conversation;
  auto messages = next.value("messages").toArray();
  messages.append(
      QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                  {"role", "user"},
                  {"text", draft().trimmed()},
                  {"objectId", objectId},
                  {"createdAt",
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                  {"status", "saved"}});
  next["messages"] = messages;
  next["draft"] = "";
  QString error;
  if (!ProjectStore::saveConversation(*project, next, error)) {
    emit operationError(error);
    return false;
  }
  Conversation = next;
  Conversations[ActiveId] = next;
  DraftDirty = false;
  DraftTimer.stop();
  emit conversationChanged();
  emit notice(tr("Request saved."));
  return true;
}

void AgentController::collectFiles(const QString &path, int depth) {
  const auto entries = QDir(path).entryInfoList(
      QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
  for (const auto &entry : entries) {
    if (entry.fileName() == ".nd-agent")
      continue;
    const bool directory = entry.isDir() && !entry.isSymLink();
    const QString absolute = entry.absoluteFilePath();
    Files.append(
        QVariantMap{{"name", entry.fileName()},
                    {"path", absolute},
                    {"directory", directory},
                    {"depth", depth},
                    {"expanded", ExpandedDirectories.contains(absolute)}});
    if (directory && ExpandedDirectories.contains(absolute))
      collectFiles(absolute, depth + 1);
  }
}

void AgentController::refreshFiles() {
  Files.clear();
  if (const auto *project = findProject(ActiveId))
    collectFiles(project->Directory, 0);
  emit filesChanged();
}

void AgentController::toggleDirectory(const QString &path) {
  if (ExpandedDirectories.contains(path))
    ExpandedDirectories.remove(path);
  else
    ExpandedDirectories.insert(path);
  refreshFiles();
}

void AgentController::openTextLink(const QString &link) {
  const QUrl url(link);
  if (url.scheme() == "nd-evidence") {
    openEvidence(url.host());
    return;
  }
  if (url.scheme() == "https" || url.scheme() == "http") {
    emit desktopUrlRequested(url.toString(), tr("Could not open the link."));
    return;
  }
  const auto *project = findProject(ActiveId);
  if (!project)
    return;
  QString path;
  if (url.isLocalFile())
    path = url.toLocalFile();
  else if (url.isRelative())
    path = url.path();
  else if (link.size() > 2 && link[1] == ':' && link[0].isLetter())
    path = link;
  else {
    emit operationError(
        tr("This type of link cannot be opened from the conversation."));
    return;
  }
  // A model wrote the link; a network path would contact its host.
  if (isRemotePath(link) || isRemotePath(path)) {
    emit operationError(tr("File links must point to an existing text file in "
                           "the current project."));
    return;
  }
  const QString root = QFileInfo(project->Directory).canonicalFilePath();
  const QString target =
      QFileInfo(QDir(root).absoluteFilePath(path)).canonicalFilePath();
#ifdef Q_OS_WIN
  constexpr auto caseSensitivity = Qt::CaseInsensitive;
#else
  constexpr auto caseSensitivity = Qt::CaseSensitive;
#endif
  if (target.isEmpty() || !target.startsWith(root + '/', caseSensitivity)) {
    emit operationError(tr("File links must point to an existing text file in "
                           "the current project."));
    return;
  }
  previewFile(target);
}
void AgentController::previewFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    emit operationError(tr("Cannot open the file: %1").arg(file.errorString()));
    return;
  }
  if (file.size() > kMaxPreviewBytes) {
    emit operationError(tr("This version previews text files up to %1 KiB.")
                            .arg(kMaxPreviewBytes / 1024));
    return;
  }
  const QByteArray bytes = file.readAll();
  QStringDecoder decoder(QStringDecoder::Utf8);
  const QString text = decoder(bytes);
  if (bytes.contains('\0') || decoder.hasError()) {
    emit operationError(tr("This file is not UTF-8 text. Open it in a "
                           "suitable application."));
    return;
  }
  PreviewTitle = QFileInfo(path).fileName();
  PreviewPath = QFileInfo(path).absoluteFilePath();
  PreviewText = text;
  emit previewChanged();
}

void AgentController::closePreview() {
  PreviewTitle.clear();
  PreviewPath.clear();
  PreviewText.clear();
  emit previewChanged();
}

void AgentController::showProjectFolder() {
  const auto *project = findProject(ActiveId);
  if (project)
    emit desktopUrlRequested(QUrl::fromLocalFile(project->Directory).toString(),
                             tr("Could not open the project folder."));
}

bool AgentController::updateSettings(bool sharing, bool lightTheme,
                                     const QString &guiPath) {
  return updateWorkspaceSettings(sharing, lightTheme, guiPath,
                                 AutoAddAnalysisObjects);
}

bool AgentController::updateWorkspaceSettings(bool sharing, bool lightTheme,
                                              const QString &guiPath,
                                              bool autoAddObjects) {
  const bool oldSharing = Sharing;
  const bool oldLight = LightTheme;
  const bool oldAutoAdd = AutoAddAnalysisObjects;
  const QString oldPath = GuiPath;
  Sharing = sharing;
  LightTheme = lightTheme;
  AutoAddAnalysisObjects = autoAddObjects;
  GuiPath = guiPath.trimmed();
  QString error;
  if (!saveSettings(error)) {
    Sharing = oldSharing;
    LightTheme = oldLight;
    AutoAddAnalysisObjects = oldAutoAdd;
    GuiPath = oldPath;
    emit operationError(error);
    return false;
  }
  if (!writeCatalog(error))
    emit operationError(error);
  readMemoryViews();
  emit settingsChanged();
  emit objectChanged();
  return true;
}

void AgentController::openInGui(const QString &objectId) {
  const auto *project = findProject(ActiveId);
  const auto *object = project ? findObject(*project, objectId) : nullptr;
  if (!object)
    return;
  const QString program = findDesktopExecutable(GuiPath);
  if (program.isEmpty()) {
    emit operationError(tr("The NeverD workbench was not found. Choose its "
                           "executable in application settings."));
    return;
  }
  if (!QFileInfo(object->Path).isFile()) {
    emit operationError(
        tr("The original file was moved or no longer exists: %1")
            .arg(object->Path));
    return;
  }
  QProcess process;
  process.setProgram(program);
  process.setArguments({"--", object->Path});
  process.setWorkingDirectory(QFileInfo(program).absolutePath());
  if (!process.startDetached())
    emit operationError(tr("Could not start the NeverD workbench: %1")
                            .arg(process.errorString()));
  else
    emit notice(tr("Started the NeverD workbench for %1.").arg(object->Name));
}

void AgentController::readMemoryViews() {
  Memory.clear();
  SharedMemory.clear();
  ProjectMemory.clear();
  const auto *project = findProject(ActiveId);
  const auto *object =
      project ? findObject(*project, SelectedObjectId) : nullptr;
  if (!project)
    return;
  QString error;
  QJsonArray records;
  if (!MemoryStore::read(*project, {}, records, error, true)) {
    emit operationError(error);
    return;
  }
  ProjectMemory = records.toVariantList();
  if (!object)
    return;
  if (!ProjectStore::readMemory(*project, *object, records, error)) {
    emit operationError(error);
    return;
  }
  Memory = records.toVariantList();
  if (!Sharing)
    return;
  // Read source projects, never trust a cached index as the memory contents.
  QSet<QString> seen;
  for (const auto &entry : records)
    seen.insert(entry.toObject().value("id").toString());
  for (const auto &other : Projects) {
    if (other.Id == project->Id)
      continue;
    for (const auto &candidate : other.Objects) {
      if (candidate.Sha256 != object->Sha256)
        continue;
      QJsonArray shared;
      if (!MemoryStore::read(other, candidate.Id, shared, error)) {
        emit operationError(error);
        continue;
      }
      for (const auto &entry : shared) {
        const auto record = entry.toObject();
        const QString id = record.value("id").toString();
        if (seen.contains(id))
          continue;
        auto view = record.toVariantMap();
        view["sourceProject"] = other.Name;
        SharedMemory.append(view);
        seen.insert(id);
      }
    }
  }
}

void AgentController::refreshMemory() {
  readMemoryViews();
  emit objectChanged();
}

bool AgentController::addMemory(const QString &objectId, const QString &text) {
  const auto *project = findProject(ActiveId);
  const auto *object = project ? findObject(*project, objectId) : nullptr;
  if (!project || (!objectId.isEmpty() && !object))
    return false;
  QString error;
  if (!MemoryStore::append(*project, objectId, text, error)) {
    emit operationError(error);
    return false;
  }
  refreshMemory();
  emit notice(tr("Memory saved to the current project."));
  return true;
}

bool AgentController::transferMemory(const QString &projectId,
                                     const QString &objectId,
                                     const QString &path, bool importing) {
  const auto *project = findProject(projectId);
  const auto *object = project ? findObject(*project, objectId) : nullptr;
  if (!object)
    return false;
  QString error;
  const bool success =
      importing ? ProjectStore::importMemory(*project, *object, path, error)
                : ProjectStore::exportMemory(*project, *object, path, error);
  if (!success) {
    emit operationError(error);
    return false;
  }
  refreshMemory();
  emit notice(importing ? tr("Memory imported. Duplicate records were merged.")
                        : tr("Memory exported."));
  return true;
}
} // namespace neverd::agent
