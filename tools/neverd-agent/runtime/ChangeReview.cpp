//===- ChangeReview.cpp - Reviewable ND project commands ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ChangeReview.h"

#include "project/EvidenceStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>

namespace neverd::agent {
namespace {
QString path(const Project &project, const QString &id) {
  return QDir(ProjectStore::stateDirectory(project))
      .filePath("changes/" + id + ".json");
}
QString display(const QJsonValue &value) {
  if (value.isNull())
    return "(由 ND 分析推断)";
  if (value.isString())
    return value.toString();
  return QString::fromUtf8(
      QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented));
}
const AnalysisObject *target(const Project &project,
                             const QJsonObject &proposal) {
  for (const auto &object : project.Objects)
    if (object.Id == proposal["objectId"] &&
        object.Sha256 == proposal["sha256"])
      return &object;
  return nullptr;
}
bool read(const Project &project, const QString &id, QJsonObject &proposal,
          QString &error) {
  if (QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id) {
    error = "Invalid change identity.";
    return false;
  }
  if (!ProjectStore::readJson(path(project, id), proposal, error))
    return false;
  if (proposal["version"].toInt() != 1 || proposal["id"] != id ||
      proposal["projectId"] != project.Id || !target(project, proposal) ||
      !QStringList{"rename", "comment", "function_type", "types"}.contains(
          proposal["kind"].toString()) ||
      !proposal["address"].isString() || !proposal.contains("before") ||
      !proposal["after"].isString() || !proposal["rationale"].isString() ||
      !proposal["evidenceIds"].isArray() ||
      !QStringList{"pending", "applied", "undone", "dismissed", "conflict",
                   "failed", "uncertain", "applied_unverified", "no_change"}
           .contains(proposal["status"].toString())) {
    error = "Invalid saved change or mismatched analysis object.";
    return false;
  }
  return true;
}
class ReviewJob final : public QObject {
public:
  Project ProjectValue;
  AnalysisObject Object;
  QJsonObject Proposal;
  WorkerSession Session;
  WorkerSession::Done Done;
  QJsonObject BeforeHead;
  bool Proposing = false, Undo = false, Committed = false, Finished = false;

  explicit ReviewJob(QObject *owner) : QObject(owner), Session(this) {}
  void start(const QString &program, int seconds) {
    Session.start(
        program, Object, Proposing, seconds, [this](const auto &result) {
          if (!accept(result))
            return;
          if (Undo) {
            history([this](QJsonObject head) {
              if (head != Proposal["receipt"].toObject()) {
                finish("applied", "ND history changed. This action will not "
                                  "undo a later or unrelated command.");
                return;
              }
              if (!persistIntent())
                return;
              Session.request("undo", {}, true, [this](const auto &reply) {
                if (!accept(reply))
                  return;
                Committed = reply["payload"].toObject()["saved"].toBool();
                saveAndFinish();
              });
            });
          } else {
            before([this](QJsonValue value) {
              if (Proposing) {
                Proposal["before"] = value;
                if (!Proposal.contains("beforeDisplay"))
                  Proposal["beforeDisplay"] = display(value);
                finish("pending");
              } else if (value != Proposal["before"]) {
                Proposal["current"] = value;
                finish("conflict",
                       "The current ND value differs from the reviewed "
                       "before-value. Generate a fresh proposal.");
              } else {
                history([this](QJsonObject head) {
                  BeforeHead = head;
                  write();
                });
              }
            });
          }
        });
  }
  bool accept(const QJsonObject &result) {
    if (result["status"] == "ok")
      return true;
    const bool saved =
        result["payload"].toObject()["saved"].toBool() || Committed;
    QString state = saved                          ? "applied_unverified"
                    : result["uncertain"].toBool() ? "uncertain"
                                                   : "failed";
    if (Undo && (saved || result["uncertain"].toBool()))
      state = "uncertain";
    else if (Undo)
      state = "applied";
    finish(state, result["error"].toObject()["message"].toString(
                      "ND command failed."));
    return false;
  }
  void before(std::function<void(QJsonValue)> done) {
    const auto kind = Proposal["kind"].toString();
    if (kind == "types") {
      Session.request("local_types_source", {{"language", "c"}}, false,
                      [this, done](const auto &result) {
                        if (!accept(result))
                          return;
                        const auto source =
                            result["payload"].toObject()["source"].toString();
                        const QJsonObject baseline{
                            {"librarySha256",
                             QString::fromLatin1(QCryptographicHash::hash(
                                                     source.toUtf8(),
                                                     QCryptographicHash::Sha256)
                                                     .toHex())}};
                        if (!Proposing) {
                          done(baseline);
                          return;
                        }
                        Proposal["beforeDisplay"] = "";
                        typeDisplay(0, [done, baseline] { done(baseline); });
                      });
      return;
    }
    Session.request(
        "resolve", {{"query", Proposal["address"]}}, false,
        [this, kind, done](const auto &result) {
          if (!accept(result))
            return;
          const auto payload = result["payload"].toObject();
          const auto address = payload["address"].toString();
          if (address.isEmpty()) {
            finish("failed", "ND did not resolve the target address.");
            return;
          }
          Proposal["address"] = address;
          if (kind == "rename")
            done(payload["name"].toString());
          else if (kind == "comment")
            done(payload["comment"].toString());
          else
            Session.request(
                "function_attributes", {{"address", address}}, false,
                [this, done](const auto &attributes) {
                  if (accept(attributes))
                    done(attributes["payload"].toObject().contains("type")
                             ? attributes["payload"].toObject()["type"]
                             : QJsonValue(QJsonValue::Null));
                });
        });
  }
  void typeDisplay(qsizetype index, std::function<void()> done) {
    const auto names = Proposal["typeNames"].toArray();
    if (index == names.size()) {
      done();
      return;
    }
    const auto name = names[index].toString();
    Session.request(
        "local_types", {{"filter", name}}, false,
        [this, name, index, done](const auto &result) {
          if (!accept(result))
            return;
          bool exists = false;
          for (const auto &entry :
               result["payload"].toObject()["types"].toArray())
            if (entry.toObject()["name"] == name)
              exists = true;
          if (!exists) {
            Proposal["beforeDisplay"] =
                Proposal["beforeDisplay"].toString() + name + ": 尚未定义\n";
            typeDisplay(index + 1, done);
            return;
          }
          Session.request(
              "local_types_source",
              {{"language", "c"},
               {"types", QJsonArray{name}},
               {"dependencies", false},
               {"prototypes", false}},
              false, [this, index, done](const auto &rendered) {
                if (!accept(rendered))
                  return;
                Proposal["beforeDisplay"] =
                    Proposal["beforeDisplay"].toString() +
                    rendered["payload"].toObject()["source"].toString() + '\n';
                typeDisplay(index + 1, done);
              });
        });
  }
  void history(std::function<void(QJsonObject)> done) {
    Session.request(
        "history", {{"offset", 0}, {"limit", 1}}, false,
        [this, done](const auto &result) {
          if (!accept(result))
            return;
          const auto header = result["payload"].toObject();
          if (!header["available"].toBool(true) ||
              header["source_sha256"] != Object.Sha256 ||
              !header["cursor"].isDouble() || !header["total"].isDouble()) {
            finish("failed",
                   "ND history is unavailable or belongs to another binary.");
            return;
          }
          QJsonObject receipt{{"source_sha256", header["source_sha256"]},
                              {"cursor", header["cursor"]},
                              {"total", header["total"]}};
          const auto cursor = header["cursor"].toInteger(-1);
          if (cursor < 0 || cursor > header["total"].toInteger(-1)) {
            finish("failed", "Invalid ND history cursor.");
            return;
          }
          if (cursor == 0) {
            receipt["head"] = QJsonValue::Null;
            done(receipt);
            return;
          }
          Session.request(
              "history", {{"offset", cursor - 1}, {"limit", 1}}, false,
              [this, receipt, done](const auto &last) mutable {
                if (!accept(last))
                  return;
                const auto rows = last["payload"].toObject()["items"].toArray();
                if (rows.size() != 1 ||
                    !rows[0].toObject()["applied"].toBool()) {
                  finish("failed",
                         "ND did not return the latest applied history entry.");
                  return;
                }
                receipt["head"] = rows[0];
                done(receipt);
              });
        });
  }
  void write() {
    const auto kind = Proposal["kind"].toString();
    QString operation;
    QJsonObject payload{{"address", Proposal["address"]}};
    if (kind == "rename") {
      operation = "rename";
      payload["name"] = Proposal["after"];
    } else if (kind == "comment") {
      operation = "annotation_set";
      payload["text"] = Proposal["after"];
    } else if (kind == "function_type") {
      operation = "function_edit";
      payload["attributes"] =
          QJsonObject{{"type", Proposal["after"].toString().trimmed().isEmpty()
                                   ? QJsonValue(QJsonValue::Null)
                                   : Proposal["after"]}};
    } else {
      operation = "local_types_declare";
      payload = {{"source", Proposal["after"]}, {"language", "c"}};
    }
    if (!Session.supports(operation)) {
      finish("failed", "The configured ND worker lacks " + operation + ".");
      return;
    }
    if (!persistIntent())
      return;
    Session.request(operation, payload, true, [this](const auto &result) {
      if (!accept(result))
        return;
      const auto payload = result["payload"].toObject();
      Committed = payload["saved"].toBool();
      if (!payload["problems"].toArray().isEmpty()) {
        finish("failed", "ND rejected the declaration:\n" + display(payload));
        return;
      }
      saveAndFinish();
    });
  }
  bool persistIntent() {
    Proposal["writeAttempted"] = true;
    QString error;
    // Persist the intent before submitting a write. A crash cannot make the
    // next application run treat this as a fresh, never-submitted proposal.
    auto intent = Proposal;
    intent["status"] = "uncertain";
    if (!ProjectStore::writeJson(path(ProjectValue, Proposal["id"].toString()),
                                 intent, error)) {
      finish("failed", error);
      return false;
    }
    return true;
  }
  void saveAndFinish() {
    // Comments and their undo are staged; all commands need a durable receipt.
    Session.request("save", {}, true, [this](const auto &saved) {
      if (!accept(saved))
        return;
      if (!saved["payload"].toObject()["saved"].toBool()) {
        finish("uncertain", "ND did not acknowledge a durable save.");
        return;
      }
      Committed = true;
      if (Undo) {
        finish("undone");
        return;
      }
      history([this](QJsonObject head) {
        if (head == BeforeHead) {
          finish("no_change");
          return;
        }
        Proposal["receipt"] = head;
        finish("applied");
      });
    });
  }
  void finish(const QString &status, const QString &issue = {}) {
    if (Finished)
      return;
    Finished = true;
    Proposal["status"] = status;
    Proposal["error"] = issue;
    Proposal["updatedAt"] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    Session.stop([this, issue] {
      QString storageError;
      QJsonObject result;
      if (!Proposal.contains("before")) {
        result["error"] = issue;
      } else if (!ProjectStore::writeJson(
                     path(ProjectValue, Proposal["id"].toString()), Proposal,
                     storageError)) {
        result = {
            {"error", "Cannot persist the change receipt: " + storageError},
            {"status",
             Committed ? QJsonValue("applied_unverified") : Proposal["status"]},
            {"proposal", Proposal}};
      } else {
        result = {{"proposal", Proposal}, {"status", Proposal["status"]}};
        if (!issue.isEmpty())
          result["error"] = issue;
      }
      auto done = std::move(Done);
      done(result);
      deleteLater();
    });
  }
};
} // namespace
void ChangeReview::propose(const Project &project,
                           const QJsonObject &parameters, const QString &worker,
                           int timeoutSeconds, QObject *owner,
                           WorkerSession::Done done) {
  const AnalysisObject *object = nullptr;
  for (const auto &candidate : project.Objects)
    if (candidate.Id == parameters["object_id"])
      object = &candidate;
  const auto kind = parameters["kind"].toString();
  const auto typeNames = parameters["type_names"].toArray();
  if (kind == "types" &&
      (typeNames.isEmpty() ||
       std::any_of(typeNames.begin(), typeNames.end(), [](const auto &name) {
         return !name.isString() || name.toString().trimmed().isEmpty();
       }))) {
    done({{"error", "Type declarations require type_names listing the affected "
                    "type names for before/after review."}});
    return;
  }
  if (!object ||
      !QStringList{"rename", "comment", "function_type", "types"}.contains(
          kind) ||
      !parameters["after"].isString() || !parameters["rationale"].isString() ||
      !parameters["evidence_ids"].isArray() ||
      (kind != "types" &&
       parameters["address"].toString().trimmed().isEmpty()) ||
      ((kind == "rename" || kind == "types") &&
       parameters["after"].toString().trimmed().isEmpty())) {
    done({{"error", "A change needs a registered object, supported kind, "
                    "target/value, rationale and evidence_ids array."}});
    return;
  }
  QString error;
  for (const auto &id : parameters["evidence_ids"].toArray()) {
    QJsonObject evidence;
    if (!EvidenceStore::read(project, id.toString(), evidence, error) ||
        evidence["objectId"] != object->Id) {
      done({{"error", error.isEmpty()
                          ? "Change evidence belongs to a different object."
                          : error}});
      return;
    }
  }
  auto *job = new ReviewJob(owner);
  job->ProjectValue = project;
  job->Object = *object;
  job->Done = std::move(done);
  job->Proposing = true;
  job->Proposal = {{"version", 1},
                   {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                   {"projectId", project.Id},
                   {"objectId", object->Id},
                   {"objectName", object->Name},
                   {"sha256", object->Sha256},
                   {"typeNames", typeNames},
                   {"kind", kind},
                   {"address", parameters["address"].toString()},
                   {"after", parameters["after"]},
                   {"rationale", parameters["rationale"]},
                   {"evidenceIds", parameters["evidence_ids"]},
                   {"createdAt", QDateTime::currentDateTimeUtc().toString(
                                     Qt::ISODateWithMs)}};
  job->start(worker, timeoutSeconds);
}
void ChangeReview::apply(const Project &project, const QString &id, bool undo,
                         const QString &worker, int timeoutSeconds,
                         QObject *owner, WorkerSession::Done done) {
  QJsonObject proposal;
  QString error;
  if (!read(project, id, proposal, error)) {
    done({{"error", error}});
    return;
  }
  if (undo
          ? proposal["status"] != "applied" || !proposal["receipt"].isObject()
          : proposal["status"] != "pending" && proposal["status"] != "failed") {
    done({{"error", "This change is not in an applicable review state."}});
    return;
  }
  auto *job = new ReviewJob(owner);
  job->ProjectValue = project;
  job->Object = *target(project, proposal);
  job->Proposal = proposal;
  job->Undo = undo;
  job->Done = std::move(done);
  job->start(worker, timeoutSeconds);
}
QVariantList ChangeReview::list(const Project &project, QString &error) {
  QVariantList result;
  QDir root(QDir(ProjectStore::stateDirectory(project)).filePath("changes"));
  const auto files = root.entryInfoList({"*.json"}, QDir::Files, QDir::Time);
  for (const auto &file : files) {
    QJsonObject proposal;
    if (!read(project, file.completeBaseName(), proposal, error))
      return {};
    result.append(proposal.toVariantMap());
  }
  return result;
}
bool ChangeReview::dismiss(const Project &project, const QString &id,
                           QString &error) {
  QJsonObject proposal;
  if (!read(project, id, proposal, error))
    return false;
  if (proposal["status"] != "pending" && proposal["status"] != "failed" &&
      proposal["status"] != "conflict") {
    error = "Only unapplied changes can be dismissed.";
    return false;
  }
  proposal["status"] = "dismissed";
  return ProjectStore::writeJson(path(project, id), proposal, error);
}
} // namespace neverd::agent
