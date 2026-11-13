//===- EvidenceStore.cpp - Durable object-bound evidence ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "EvidenceStore.h"

#include "Config.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include <QUuid>

namespace neverd::agent {
namespace {
bool identity(const QString &id) {
  return !QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces) == id;
}
QString directory(const Project &project, const QString &id) {
  return QDir(ProjectStore::stateDirectory(project)).filePath("evidence/" + id);
}
} // namespace
QString EvidenceStore::resultText(const QJsonObject &result) {
  QStringList pieces;
  for (const auto &entry : result["content"].toArray()) {
    const auto item = entry.toObject();
    if (item["type"] == "text")
      pieces.append(item["text"].toString());
  }
  for (const auto &entry : result["contents"].toArray())
    pieces.append(entry.toObject()["text"].toString());
  return pieces.join('\n');
}
QJsonObject EvidenceStore::resultMetadata(const QJsonObject &result) {
  if (result["structuredContent"].isObject())
    return result["structuredContent"].toObject();
  const auto text = resultText(result);
  const auto header = text.section('\n', 0, 0).toUtf8();
  return QJsonDocument::fromJson(header).object();
}
QJsonObject EvidenceStore::descriptor(const Project &project,
                                      const AnalysisObject &object,
                                      const QString &runId, const QString &tool,
                                      const QJsonObject &arguments,
                                      const QJsonObject &result) {
  const auto metadata = resultMetadata(result);
  return {
      {"version", 1},
      {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
      {"projectId", project.Id},
      {"objectId", object.Id},
      {"objectName", object.Name},
      {"sha256", object.Sha256},
      {"runId", runId},
      {"tool", tool},
      {"arguments", arguments},
      {"address", arguments["address"].toString()},
      {"revision", metadata["revision"].toString()},
      {"createdAt",
       QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {"result", result},
      {"complete", true},
      {"issue", ""},
      {"successful", !result.contains("error") && !result["isError"].toBool()}};
}
bool EvidenceStore::save(const Project &project, QJsonObject evidence,
                         const QString &text, QString &error) {
  const auto root = directory(project, evidence["id"].toString());
  if (!QDir().mkpath(root)) {
    error = "Cannot create the evidence directory.";
    return false;
  }
  QSaveFile content(QDir(root).filePath("content.txt"));
  const auto bytes = text.toUtf8();
  if (!content.open(QIODevice::WriteOnly) ||
      content.write(bytes) != bytes.size() || !content.commit()) {
    error = content.errorString();
    return false;
  }
  evidence["bytes"] = bytes.size();
  return ProjectStore::writeJson(QDir(root).filePath("evidence.json"), evidence,
                                 error);
}
bool EvidenceStore::read(const Project &project, const QString &id,
                         QJsonObject &evidence, QString &error, qint64 offset) {
  if (!identity(id) || offset < 0) {
    error = "Invalid evidence identity or page.";
    return false;
  }
  const auto root = directory(project, id);
  if (!ProjectStore::readJson(QDir(root).filePath("evidence.json"), evidence,
                              error))
    return false;
  if (evidence["version"].toInt() != 1 || evidence["id"] != id ||
      evidence["projectId"] != project.Id || !evidence["complete"].isBool() ||
      !evidence["arguments"].isObject() || !evidence["tool"].isString() ||
      !evidence["revision"].isString() || !evidence["issue"].isString() ||
      !validateReference(reference(evidence), error)) {
    if (error.isEmpty())
      error = "Invalid saved evidence.";
    return false;
  }
  bool objectMatches = false;
  for (const auto &object : project.Objects)
    if (object.Id == evidence["objectId"] &&
        object.Sha256 == evidence["sha256"])
      objectMatches = true;
  if (!objectMatches) {
    error = "Evidence belongs to a different analysis object/version.";
    return false;
  }
  QFile file(QDir(root).filePath("content.txt"));
  if (!file.open(QIODevice::ReadOnly) || offset > file.size() ||
      !file.seek(offset)) {
    error = "Cannot read the requested evidence page.";
    return false;
  }
  auto bytes = file.read(kMaxPreviewBytes);
  if (!file.atEnd()) {
    // Extend only to the end of a UTF-8 code point; page offsets are bytes.
    for (int i = 0; i < 3 && file.peek(1).size() &&
                    (quint8(file.peek(1)[0]) & 0xc0) == 0x80;
         ++i)
      bytes += file.read(1);
  }
  QStringDecoder decoder(QStringDecoder::Utf8);
  const QString text = decoder(bytes);
  if (decoder.hasError()) {
    error = "Invalid UTF-8 evidence content.";
    return false;
  }
  evidence["text"] = text;
  evidence["offset"] = offset;
  evidence["nextOffset"] = file.atEnd() ? qint64(-1) : file.pos();
  return true;
}
QJsonObject EvidenceStore::reference(const QJsonObject &evidence) {
  return {{"kind", "evidence"},
          {"title", evidence["objectName"].toString() + " · " +
                        evidence["tool"].toString()},
          {"objectId", evidence["objectId"]},
          {"sha256", evidence["sha256"]},
          {"evidenceId", evidence["id"]},
          {"address", evidence["address"]},
          {"revision", evidence["revision"]},
          {"text", evidence["text"].toString()}};
}
bool EvidenceStore::validateReference(const QJsonObject &value,
                                      QString &error) {
  const auto kind = value["kind"].toString();
  if (!QStringList{"object", "address", "selection", "file", "evidence"}
           .contains(kind) ||
      !value["title"].isString() || !value["text"].isString()) {
    error = "Invalid attached context reference.";
    return false;
  }
  if (kind != "file" && kind != "selection") {
    static const QRegularExpression hash("^[a-f0-9]{64}$");
    if (!identity(value["objectId"].toString()) ||
        !hash.match(value["sha256"].toString()).hasMatch()) {
      error = "Context reference has no valid object identity.";
      return false;
    }
  }
  if (kind == "evidence" && !identity(value["evidenceId"].toString())) {
    error = "Invalid evidence reference.";
    return false;
  }
  return true;
}
bool EvidenceStore::validateBindings(const Project &project,
                                     const QJsonArray &references,
                                     QString &error) {
  for (const auto &entry : references) {
    const auto value = entry.toObject();
    if (!validateReference(value, error))
      return false;
    if (!value.contains("objectId"))
      continue;
    bool matched = false;
    for (const auto &object : project.Objects)
      if (object.Id == value["objectId"] && object.Sha256 == value["sha256"])
        matched = true;
    if (!matched) {
      error = "A captured context reference belongs to a changed or removed "
              "analysis object.";
      return false;
    }
  }
  return true;
}
} // namespace neverd::agent
