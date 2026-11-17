//===- ResearchPlan.cpp - Conversation research steps ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ResearchPlan.h"

#include "EvidenceStore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace neverd::agent {
namespace {
bool validate(const Project &project, const QJsonObject &plan, QString &error) {
  if (!plan["steps"].isArray() || !plan["questions"].isArray()) {
    error = "Research plan requires steps and questions arrays.";
    return false;
  }
  QSet<QString> ids;
  for (const auto &value : plan["steps"].toArray()) {
    const auto step = value.toObject();
    const auto id = step["id"].toString();
    if (id.isEmpty() || ids.contains(id) ||
        step["title"].toString().trimmed().isEmpty() ||
        !QStringList{"pending", "in_progress", "completed", "blocked"}.contains(
            step["status"].toString()) ||
        !step["summary"].isString() || !step["evidence_ids"].isArray() ||
        (step["status"] == "completed" &&
         step["summary"].toString().trimmed().isEmpty())) {
      error = "Each research step needs a unique ID, title, status, summary "
              "and evidence_ids; completed steps need an outcome.";
      return false;
    }
    ids.insert(id);
    for (const auto &evidenceId : step["evidence_ids"].toArray()) {
      QJsonObject evidence;
      if (!EvidenceStore::read(project, evidenceId.toString(), evidence, error))
        return false;
    }
  }
  for (const auto &question : plan["questions"].toArray())
    if (!question.isString() || question.toString().trimmed().isEmpty()) {
      error = "Open questions must be nonempty strings.";
      return false;
    }
  return true;
}
QString path(const Project &project, const QString &session) {
  const auto directory = ProjectStore::sessionDirectory(project, session);
  return directory.isEmpty() ? QString{}
                             : QDir(directory).filePath("research-plan.json");
}
} // namespace
bool ResearchPlan::read(const Project &project, const QString &session,
                        QJsonObject &plan, QString &error) {
  const auto file = path(project, session);
  if (file.isEmpty()) {
    error = "Invalid conversation identity.";
    return false;
  }
  if (!QFileInfo::exists(file)) {
    plan = {{"steps", QJsonArray{}}, {"questions", QJsonArray{}}};
    return true;
  }
  return ProjectStore::readJson(file, plan, error) &&
         validate(project, plan, error);
}
bool ResearchPlan::save(const Project &project, const QString &session,
                        QJsonObject plan, const QString &runId,
                        QString &error) {
  const auto file = path(project, session);
  if (file.isEmpty()) {
    error = "Invalid conversation identity.";
    return false;
  }
  if (!validate(project, plan, error))
    return false;
  plan["version"] = 1;
  plan["runId"] = runId;
  plan["updatedAt"] =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  return ProjectStore::writeJson(file, plan, error);
}
} // namespace neverd::agent
