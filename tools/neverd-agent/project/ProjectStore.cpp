//===- ProjectStore.cpp - Agent project persistence -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own validation and atomic UTF-8 storage of application state. Binary inputs
/// remain at their original locations; no ND database is created by this layer.
//===----------------------------------------------------------------------===//

#include "ProjectStore.h"

#include "Config.h"
#include "ConversationInput.h"
#include "EvidenceStore.h"
#include "MemoryStore.h"
#include "platform/AppPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <utility>

namespace neverd::agent {
namespace {
struct Text {
  Q_DECLARE_TR_FUNCTIONS(neverd::agent::ProjectStore)
};

QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

bool foreignAbsolutePath(const QString &path) {
#ifdef Q_OS_WIN
  return path.startsWith('/') && !path.startsWith("//");
#else
  return (path.size() >= 3 && path[0].isLetter() && path[1] == ':' &&
          (path[2] == '/' || path[2] == '\\')) ||
         path.startsWith("\\\\");
#endif
}

bool validId(const QJsonValue &value) {
  const QString text = value.toString();
  return !QUuid(text).isNull() &&
         QUuid(text).toString(QUuid::WithoutBraces) == text;
}

bool validHash(const QJsonValue &value) {
  static const QRegularExpression pattern("^[a-f0-9]{64}$");
  return value.isString() && pattern.match(value.toString()).hasMatch();
}

QJsonObject memoryDocument(const AnalysisObject &object,
                           const QJsonArray &records) {
  return {{"version", kStorageVersion},
          {"kind", "neverd-object-memory"},
          {"sha256", object.Sha256},
          {"objectName", object.Name},
          {"records", records}};
}
} // namespace

QString ProjectStore::stateDirectory(const Project &project) {
  return QDir(project.Directory).filePath(".nd-agent");
}

QString ProjectStore::objectDirectory(const Project &project,
                                      const AnalysisObject &object) {
  return QDir(stateDirectory(project)).filePath("objects/" + object.Id);
}

bool ProjectStore::readJson(const QString &path, QJsonObject &value,
                            QString &error, bool optional) {
  value = {};
  if (optional && !QFileInfo::exists(path))
    return true;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    error = Text::tr("Cannot read %1: %2").arg(path, file.errorString());
    return false;
  }
  if (file.size() > kMaxStateBytes) {
    error = Text::tr("State file exceeds %1 bytes: %2")
                .arg(kMaxStateBytes)
                .arg(path);
    return false;
  }
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject()) {
    error = Text::tr("Invalid JSON state file %1: %2")
                .arg(path, parse.errorString());
    return false;
  }
  value = document.object();
  return true;
}

bool ProjectStore::writeJson(const QString &path, const QJsonObject &value,
                             QString &error) {
  const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
  if (bytes.size() > kMaxStateBytes) {
    error = Text::tr("State exceeds %1 bytes; the existing file was not "
                     "overwritten.")
                .arg(kMaxStateBytes);
    return false;
  }
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    error = Text::tr("Cannot create directory: %1")
                .arg(QFileInfo(path).absolutePath());
    return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    error = Text::tr("Cannot save %1: %2").arg(path, file.errorString());
    return false;
  }
  return true;
}

std::optional<Project> ProjectStore::create(const QString &directory,
                                            const QString &name,
                                            QString &error) {
  if (name.trimmed().isEmpty() || directory.trimmed().isEmpty()) {
    error = Text::tr("Enter a project name and choose a project folder.");
    return std::nullopt;
  }
  const QString root = QFileInfo(localPath(directory)).absoluteFilePath();
  Project project{newId(), name.trimmed(), root, {}};
  const QString state = stateDirectory(project);
  if (QFileInfo::exists(state)) {
    error = Text::tr(
        "This folder already contains .nd-agent. Use Open project instead.");
    return std::nullopt;
  }
  if (!save(project, error))
    return std::nullopt;
  if (!saveConversation(project,
                        {{"version", kStorageVersion},
                         {"draft", ""},
                         {"messages", QJsonArray{}}},
                        error))
    return std::nullopt;
  return project;
}

std::optional<Project> ProjectStore::open(const QString &directory,
                                          QString &error) {
  const QString root = QFileInfo(localPath(directory)).absoluteFilePath();
  QJsonObject value;
  if (!readJson(QDir(root).filePath(".nd-agent/project.json"), value, error))
    return std::nullopt;
  if (value.value("version").toInt(-1) != kStorageVersion ||
      !validId(value.value("id")) ||
      value.value("name").toString().trimmed().isEmpty() ||
      !value.value("objects").isArray()) {
    error = Text::tr("Invalid project format or version: %1").arg(root);
    return std::nullopt;
  }
  Project project{
      value.value("id").toString(), value.value("name").toString(), root, {}};
  QSet<QString> ids;
  for (const auto &entry : value.value("objects").toArray()) {
    const auto item = entry.toObject();
    if (!validId(item.value("id")) || !validHash(item.value("sha256")) ||
        item.value("name").toString().trimmed().isEmpty() ||
        item.value("path").toString().trimmed().isEmpty() ||
        ids.contains(item.value("id").toString())) {
      error = Text::tr("The project contains invalid or duplicate analysis "
                       "objects: %1")
                  .arg(root);
      return std::nullopt;
    }
    ids.insert(item.value("id").toString());
    const QString path = item.value("path").toString();
    project.Objects.append(
        {item.value("id").toString(), item.value("name").toString(),
         QDir::isAbsolutePath(path) || foreignAbsolutePath(path)
             ? path
             : QDir(root).absoluteFilePath(path),
         item.value("sha256").toString()});
  }
  return project;
}

bool ProjectStore::save(const Project &project, QString &error) {
  QJsonArray objects;
  for (const auto &object : project.Objects) {
    const QString relative =
        QDir(project.Directory).relativeFilePath(object.Path);
    const bool outside = foreignAbsolutePath(object.Path) || relative == ".." ||
                         relative.startsWith("../") ||
                         QDir::isAbsolutePath(relative);
    objects.append(QJsonObject{{"id", object.Id},
                               {"name", object.Name},
                               {"path", outside ? object.Path : relative},
                               {"sha256", object.Sha256}});
  }
  return writeJson(QDir(stateDirectory(project)).filePath("project.json"),
                   {{"version", kStorageVersion},
                    {"id", project.Id},
                    {"name", project.Name},
                    {"objects", objects}},
                   error);
}

FingerprintResult ProjectStore::fingerprint(const QString &path) {
  const QString absolute = QFileInfo(localPath(path)).absoluteFilePath();
  QFile file(absolute);
  const QFileInfo before(absolute);
  if (!before.isFile() || !file.open(QIODevice::ReadOnly))
    return {absolute,
            {},
            Text::tr("Cannot read the analysis object: %1").arg(absolute)};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file))
    return {
        absolute,
        {},
        Text::tr("Could not compute the file fingerprint: %1").arg(absolute)};
  const QFileInfo after(absolute);
  if (before.size() != after.size() ||
      before.lastModified() != after.lastModified())
    return {absolute,
            {},
            Text::tr("The file changed while it was being read. Add it "
                     "again: %1")
                .arg(absolute)};
  return {absolute, QString::fromLatin1(hash.result().toHex()), {}};
}

bool ProjectStore::addObject(Project &project, const FingerprintResult &input,
                             QString &error) {
  if (!input.Error.isEmpty()) {
    error = input.Error;
    return false;
  }
  for (const auto &object : project.Objects) {
    if (QFileInfo(object.Path) == QFileInfo(input.Path)) {
      error = Text::tr("This file is already in the project.");
      return false;
    }
  }
  Project next = project;
  const AnalysisObject object{newId(), QFileInfo(input.Path).fileName(),
                              input.Path, input.Sha256};
  next.Objects.append(object);
  if (!writeJson(QDir(objectDirectory(next, object)).filePath("memory.json"),
                 memoryDocument(object, {}), error) ||
      !save(next, error))
    return false;
  project = next;
  return true;
}

bool ProjectStore::relocateObject(Project &project, const QString &objectId,
                                  const FingerprintResult &input,
                                  QString &error) {
  if (!input.Error.isEmpty()) {
    error = input.Error;
    return false;
  }
  Project next = project;
  for (auto &object : next.Objects) {
    if (object.Id != objectId)
      continue;
    if (object.Sha256 != input.Sha256) {
      error =
          Text::tr("The selected file's SHA-256 does not match the original "
                   "object. Add a different version as a new object.");
      return false;
    }
    for (const auto &other : next.Objects)
      if (other.Id != objectId &&
          QFileInfo(other.Path) == QFileInfo(input.Path)) {
        error = Text::tr(
            "This file is already bound to another object in this project.");
        return false;
      }
    object.Path = input.Path;
    if (!save(next, error))
      return false;
    project = std::move(next);
    return true;
  }
  error = Text::tr("The analysis object no longer exists.");
  return false;
}

QString ProjectStore::sessionDirectory(const Project &project,
                                       const QString &sessionId) {
  if (sessionId.isEmpty())
    return stateDirectory(project);
  if (!validId(sessionId))
    return {};
  return QDir(stateDirectory(project)).filePath("sessions/" + sessionId);
}
bool ProjectStore::readConversation(const Project &project, QJsonObject &value,
                                    QString &error, const QString &sessionId) {
  const auto directory = sessionDirectory(project, sessionId);
  if (directory.isEmpty()) {
    error = "Invalid conversation identity.";
    return false;
  }
  const QString path = QDir(directory).filePath("router/conversation.json");
  if (!QFileInfo::exists(path)) {
    value = {{"version", kStorageVersion}, {"draft", ""},
             {"context", QJsonArray{}},    {"pendingInputs", QJsonArray{}},
             {"queuePaused", false},       {"messages", QJsonArray{}}};
    return true;
  }
  if (!readJson(path, value, error))
    return false;
  if (value.value("version").toInt(-1) != kStorageVersion ||
      !value.value("draft").isString() || !value.value("messages").isArray() ||
      (value.contains("forkedAt") &&
       !QDateTime::fromString(value["forkedAt"].toString(), Qt::ISODateWithMs)
            .isValid())) {
    error = Text::tr("Invalid project conversation format: %1").arg(path);
    return false;
  }
  if (!value.contains("context"))
    value["context"] = QJsonArray{};
  if (!value.contains("pendingInputs"))
    value["pendingInputs"] = QJsonArray{};
  if (!value.contains("queuePaused"))
    value["queuePaused"] = false;
  if (!value["context"].isArray()) {
    error = "Invalid draft context attachments.";
    return false;
  }
  for (const auto &reference : value["context"].toArray())
    if (!EvidenceStore::validateReference(reference.toObject(), error))
      return false;
  if ((value.contains("pendingInputs") && !value["pendingInputs"].isArray()) ||
      (value.contains("queuePaused") && !value["queuePaused"].isBool())) {
    error = "Invalid pending conversation input state: " + path;
    return false;
  }
  QSet<QString> pendingIds;
  for (const auto &entry : value["pendingInputs"].toArray()) {
    const auto input = ConversationInput::fromJson(entry.toObject(), error);
    if (!input || pendingIds.contains(input->Id)) {
      if (input)
        error = "Repeated pending input identity: " + path;
      return false;
    }
    pendingIds.insert(input->Id);
  }
  for (const auto &entry : value.value("messages").toArray()) {
    const auto message = entry.toObject();
    if (!validId(message.value("id")) ||
        (message.value("role").toString() != "user" &&
         message.value("role").toString() != "assistant") ||
        !message.value("text").isString() ||
        !QStringList{"saved", "running", "succeeded", "failed", "cancelled",
                     "interrupted", "snapshot", "pending"}
             .contains(message.value("status").toString()) ||
        (message.contains("inherited") && !message["inherited"].isBool()) ||
        !message.value("objectId").isString() ||
        !QDateTime::fromString(message.value("createdAt").toString(),
                               Qt::ISODateWithMs)
             .isValid()) {
      error =
          Text::tr("The project contains an invalid conversation record: %1")
              .arg(path);
      return false;
    }
  }
  return true;
}

bool ProjectStore::saveConversation(const Project &project,
                                    const QJsonObject &value, QString &error,
                                    const QString &sessionId) {
  const auto directory = sessionDirectory(project, sessionId);
  if (directory.isEmpty()) {
    error = "Invalid conversation identity.";
    return false;
  }
  return writeJson(QDir(directory).filePath("router/conversation.json"), value,
                   error);
}

bool ProjectStore::readMemory(const Project &project,
                              const AnalysisObject &object, QJsonArray &records,
                              QString &error) {
  return MemoryStore::read(project, object.Id, records, error, true);
}

bool ProjectStore::appendMemory(const Project &project,
                                const AnalysisObject &object,
                                const QString &text, QString &error) {
  return MemoryStore::append(project, object.Id, text, error);
}

bool ProjectStore::exportMemory(const Project &project,
                                const AnalysisObject &object,
                                const QString &destination, QString &error) {
  return MemoryStore::exportFile(project, object.Id, destination, error);
}

bool ProjectStore::importMemory(const Project &project,
                                const AnalysisObject &object,
                                const QString &source, QString &error) {
  return MemoryStore::importFile(project, object.Id, source, error);
}
} // namespace neverd::agent
