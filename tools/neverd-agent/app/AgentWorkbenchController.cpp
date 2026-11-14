//===- AgentWorkbenchController.cpp - Bind an existing GUI session --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "AgentWorkbenchController.h"

#include "AgentController.h"
#include "project/ConversationInput.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <utility>

namespace neverd::agent {
std::optional<WorkbenchConversation>
resolveWorkbenchConversation(const QString &dataDirectory,
                             const QJsonObject &binding, QString &error) {
  const auto inputPath = binding.value("input_path").toString();
  const auto database = binding.value("project_path").toString();
  if (!binding.value("loaded").toBool() || inputPath.isEmpty() ||
      binding.value("project_id").toString().isEmpty() ||
      binding.value("epoch").toString().isEmpty()) {
    error =
        AgentController::tr("The workbench analysis session is unavailable.");
    return std::nullopt;
  }
  auto input = ProjectStore::fingerprint(inputPath);
  if (!input.Error.isEmpty()) {
    error = input.Error;
    return std::nullopt;
  }
  const QFileInfo identity(database.isEmpty() ? inputPath : database);
  const auto key =
      QCryptographicHash::hash(identity.absoluteFilePath().toUtf8() + '\0' +
                                   input.Sha256.toLatin1(),
                               QCryptographicHash::Sha256)
          .toHex();
  return WorkbenchConversation{
      QDir(dataDirectory).filePath("workbench/" + QString::fromLatin1(key)),
      identity.fileName(), std::move(input)};
}

bool recoverWorkbenchInputs(QJsonObject &conversation, QString &error) {
  // Inputs left by an interrupted workbench run stay in the transcript.
  // They do not become part of the next request.
  auto messages = conversation["messages"].toArray();
  for (const auto &entry : conversation["pendingInputs"].toArray()) {
    const auto input = ConversationInput::fromJson(entry.toObject(), error);
    if (!input)
      return false;
    messages.append(input->message("interrupted"));
  }
  conversation["messages"] = messages;
  conversation["pendingInputs"] = QJsonArray{};
  conversation["context"] = QJsonArray{};
  conversation["queuePaused"] = false;
  return true;
}

bool AgentController::initializeWorkbench(
    const WorkbenchConversation &conversation, const QString &attachment,
    QString &error) {
  if (!QFileInfo(attachment).isFile()) {
    error = tr("The workbench analysis session is unavailable.");
    return false;
  }
  QJsonObject config;
  if (!loadConfiguration(config, error))
    return false;
  if (!modelConfigured()) {
    error = tr("Configure a model in the standalone NeverD Agent first.");
    return false;
  }
  if (!Profile.validate(error))
    return false;

  const auto &directory = conversation.Directory;
  if (!QDir().mkpath(directory)) {
    error = tr("Cannot create the workbench conversation directory.");
    return false;
  }
  // Serialize first-time creation; bindProject then holds the ordinary editor
  // lock for this workspace for the lifetime of the attached client.
  QLockFile creation(QDir(directory).filePath("create.lock"));
  if (!creation.tryLock()) {
    error = tr("Another Agent is opening this workbench conversation.");
    return false;
  }
  auto project =
      QFileInfo::exists(QDir(directory).filePath(".nd-agent"))
          ? ProjectStore::open(directory, error)
          : ProjectStore::create(directory, conversation.Name, error);
  if (!project || !bindProject(*project, error))
    return false;
  auto *bound = findProject(project->Id);
  if (bound->Objects.isEmpty()) {
    if (!ProjectStore::addObject(*bound, conversation.Input, error))
      return false;
  } else if (!ProjectStore::relocateObject(*bound, bound->Objects.front().Id,
                                           conversation.Input, error)) {
    return false;
  }
  WorkbenchAttachment = attachment;
  Sharing = false;
  if (!selectProject(bound->Id)) {
    error = tr("Cannot open the workbench conversation.");
    return false;
  }
  selectObject(bound->Objects.front().Id);

  auto current = Conversation;
  if (!recoverWorkbenchInputs(current, error))
    return false;
  if (!commitConversation(ActiveId, current)) {
    error = tr("Cannot save the workbench conversation.");
    return false;
  }
  reloadCredential();
  return true;
}
} // namespace neverd::agent
