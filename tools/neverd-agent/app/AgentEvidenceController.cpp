//===- AgentEvidenceController.cpp - Object-bound evidence inspection -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AgentController.h"
#include "runtime/EvidenceCapture.h"

#include <QUuid>

namespace neverd::agent {
bool AgentController::openEvidence(const QString &id, qint64 offset) {
  const auto *project = findProject(ActiveId);
  if (!project)
    return false;
  QJsonObject value;
  QString error;
  if (!EvidenceStore::read(*project, id, value, error, offset)) {
    emit operationError(error);
    return false;
  }
  Evidence = value;
  emit evidenceChanged();
  emit evidenceOpened(id, value["objectName"].toString() + " · " +
                              value["tool"].toString());
  return true;
}
void AgentController::inspectObject(const QString &objectId,
                                    const QString &query,
                                    const QString &representation) {
  const auto *project = findProject(ActiveId);
  const auto *object = project ? findObject(*project, objectId) : nullptr;
  if (!object || WritingProjects.contains(ActiveId) ||
      InspectionJobs.contains(ActiveId))
    return;
  const QHash<QString, QString> tools{{"c", "read_decompilation"},
                                      {"disasm", "read_disassembly"},
                                      {"xrefs", "list_xrefs"},
                                      {"cfg", "read_cfg"}};
  if (!tools.contains(representation) || query.trimmed().isEmpty()) {
    emit operationError(
        tr("Choose an analysis object and enter a function name or address."));
    return;
  }
  const auto executable = resolveMcp();
  if (executable.isEmpty()) {
    emit operationError(
        tr("Set the NeverD MCP server executable in model settings first."));
    return;
  }
  const Project owner = *project;
  const AnalysisObject target = *object;
  const QString key = owner.Id + '/' + objectId;
  InspectionJobs.insert(owner.Id);
  emit evidenceChanged();
  auto *engine = Inspectors.value(key);
  if (!engine || !engine->ready()) {
    if (engine)
      delete engine;
    engine = new McpSession(this);
    Inspectors[key] = engine;
  }
  const auto finish = [this, owner](const QJsonObject &result) {
    InspectionJobs.remove(owner.Id);
    if (owner.Id != ActiveId)
      return;
    emit evidenceChanged();
    if (result.contains("error"))
      emit operationError(result["error"].toString());
    else if (result["evidence_id"].isString())
      openEvidence(result["evidence_id"].toString());
  };
  const auto run = [this, owner, target, engine, query, representation,
                    tool = tools[representation],
                    finish](const QJsonObject &ready) {
    if (ready.contains("error")) {
      finish(ready);
      return;
    }
    QJsonObject arguments{{"address", query.trimmed()}};
    if (representation == "c") {
      arguments["representation"] = "c";
      arguments["exact"] = true;
    }
    engine->call(
        tool, arguments,
        [owner, target, engine, tool, arguments, finish](QJsonObject result) {
          EvidenceCapture::captureAndSave(
              engine, owner, target,
              QUuid::createUuid().toString(QUuid::WithoutBraces), tool,
              arguments, result, finish);
        });
  };
  if (engine->ready())
    run({});
  else
    engine->start(executable, target, Profile.TimeoutSeconds, run);
}
} // namespace neverd::agent
