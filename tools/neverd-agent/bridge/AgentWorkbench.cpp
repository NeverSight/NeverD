//===- AgentWorkbench.cpp - Headless workbench conversation
//----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "AgentWorkbench.h"

#include "client/AgentProtocol.h"
#include "project/EvidenceStore.h"
#include "runtime/ExecutionTrace.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <algorithm>
#include <utility>

namespace neverd::agent {
namespace {
constexpr qsizetype ExecutionRowsPerMessage = 64;

QJsonObject executionSummary(const QJsonObject &record) {
  QJsonObject summary;
  for (const auto *field :
       {"label", "name", "address", "state", "elapsedMs", "evidenceId"})
    summary.insert(field, record.value(field));
  if (record.value("state") == "failed") {
    const auto error = record.value("result").toObject().value("error");
    summary.insert("error", error.isObject() ? error.toObject().value("message")
                                             : error);
  }
  return summary;
}

} // namespace

AgentWorkbench::AgentWorkbench(const QString &home,
                               std::function<void(const QJsonObject &)> publish)
    : Home(home), Controller(home, this), Publish(std::move(publish)) {
  connect(&Controller, &AgentController::credentialChanged, this,
          [this] { finishPreparation(); });
  connect(&Controller, &AgentController::operationError, this,
          [this](const QString &error) { fail(error); });
  connect(&Controller, &AgentController::messagesChanged, this,
          [this] { publishMessages(); });
  connect(&Controller, &AgentController::runChanged, this,
          [this] { publishState(); });
  connect(
      &Controller, &AgentController::executionRow, this,
      [this](const QString &runId, qsizetype index, const QJsonObject &record) {
        if (Ready)
          publishExecution(runId, index, QJsonArray{executionSummary(record)});
      });
  connect(&Controller, &AgentController::runFinished, this, [this] {
    publishMessages();
    publishState();
  });
}

void AgentWorkbench::fail(const QString &text, bool fatal) {
  Publish({{"type", "error"},
           {"text", text},
           {"running", Controller.running()},
           {"fatal", fatal}});
}

void AgentWorkbench::announceReady() {
  Ready = true;
  Publish({{"type", "ready"}, {"protocol", wire::kVersion}});
  publishMessages();
}

void AgentWorkbench::finishPreparation() {
  if (!Prepared || AnalysisReady || Controller.credentialBusy())
    return;
  if (!Controller.credentialStatus().isEmpty()) {
    fail(Controller.credentialStatus(), true);
    return;
  }
  AnalysisReady = true;
  Publish({{"type", "analysis_ready"}});
}

void AgentWorkbench::prepare(const QString &attachment) {
  if (Prepared) {
    if (AnalysisReady)
      Publish({{"type", "analysis_ready"}});
    return;
  }
  QString error;
  if (!Controller.initializeWorkbench(*Location, attachment, error)) {
    fail(error, true);
    return;
  }
  Prepared = true;
  PreviewMessages = {};
  PreviewProject.reset();
  publishMessages();
  finishPreparation();
}

bool AgentWorkbench::loadPreview(QString &error) {
  const QDir directory(Location->Directory);
  if (!QFileInfo::exists(directory.filePath(".nd-agent/project.json")))
    return true;
  PreviewProject = ProjectStore::open(Location->Directory, error);
  QJsonObject conversation;
  if (!PreviewProject ||
      !ProjectStore::readConversation(*PreviewProject, conversation, error) ||
      !recoverWorkbenchInputs(conversation, error))
    return false;
  PreviewMessages = conversation.value("messages").toArray();
  for (qsizetype i = 0; i < PreviewMessages.size(); ++i) {
    auto message = PreviewMessages.at(i).toObject();
    if (message.value("status") == "running") {
      message["status"] = "snapshot";
      PreviewMessages[i] = message;
    }
  }
  return true;
}

void AgentWorkbench::publishMessages() {
  if (!Ready)
    return;
  const auto messages = Prepared
                            ? QJsonArray::fromVariantList(Controller.messages())
                            : PreviewMessages;
  qsizetype first = 0;
  while (first < messages.size() && first < Published.size() &&
         messages[first] == Published[first])
    ++first;
  if (first == messages.size() && first == Published.size())
    return;
  QJsonArray changes;
  for (auto index = first; index < messages.size(); ++index) {
    const auto message = messages[index].toObject();
    changes.append(QJsonObject{{"role", message.value("role")},
                               {"text", message.value("text")},
                               {"status", message.value("status")},
                               {"runId", message.value("runId")}});
  }
  Publish({{"type", "messages"}, {"from", first}, {"entries", changes}});
  Published = messages;
}

void AgentWorkbench::publishExecution(const QString &runId, qsizetype first,
                                      const QJsonArray &rows) {
  Publish({{"type", "execution"},
           {"runId", runId},
           {"from", first},
           {"rows", rows}});
}

void AgentWorkbench::readExecution(const QString &runId) {
  if (runId.isEmpty() ||
      std::none_of(Published.begin(), Published.end(), [&](const auto &entry) {
        return entry.toObject().value("runId") == runId;
      }))
    return;
  QVariantList records;
  if (Prepared) {
    records = Controller.execution(runId);
  } else if (PreviewProject) {
    QString error;
    records = ExecutionTrace::read(*PreviewProject, runId, error);
    if (!error.isEmpty())
      fail(error);
  }
  if (records.isEmpty()) {
    publishExecution(runId, 0, {});
    return;
  }
  for (qsizetype first = 0; first < records.size();
       first += ExecutionRowsPerMessage) {
    const auto end = std::min(first + ExecutionRowsPerMessage, records.size());
    QJsonArray rows;
    for (auto index = first; index < end; ++index)
      rows.append(executionSummary(
          QJsonObject::fromVariantMap(records.at(index).toMap())));
    publishExecution(runId, first, rows);
  }
}

void AgentWorkbench::navigateEvidence(const QString &id) {
  QString address;
  if (Prepared) {
    if (!Controller.openEvidence(id))
      return;
    address = Controller.evidence().value("address").toString();
  } else {
    if (!PreviewProject)
      return;
    QJsonObject evidence;
    QString error;
    if (!EvidenceStore::read(*PreviewProject, id, evidence, error)) {
      fail(error);
      return;
    }
    address = evidence.value("address").toString();
  }
  if (address.isEmpty())
    fail(tr("This evidence does not have a code address."));
  else
    Publish({{"type", "navigate"}, {"address", address}});
}

void AgentWorkbench::publishState() {
  if (!Ready || !AnalysisReady)
    return;
  const auto progress = Controller.progress();
  const QJsonObject state{
      {"type", "state"},
      {"running", Controller.running()},
      {"activity", progress.isEmpty()
                       ? QString{}
                       : progress.last().toMap().value("activity").toString()}};
  if (state != PublishedState) {
    Publish(state);
    PublishedState = state;
  }
}

void AgentWorkbench::bind(const QJsonObject &context) {
  Binding = context;
  QString error;
  Location = resolveWorkbenchConversation(Home, Binding, error);
  if (!Location || !loadPreview(error)) {
    fail(error, true);
    return;
  }
  announceReady();
}

ShutdownStatus AgentWorkbench::prepareShutdown() {
  return Prepared ? Controller.prepareShutdown() : ShutdownStatus::Ready;
}

std::optional<QJsonValue> AgentWorkbench::dispatch(const QString &method,
                                                   const QJsonArray &args) {
  if (method == "stop" && args.isEmpty()) {
    Controller.cancelRun();
    return QJsonValue{};
  }
  if (args.size() == 1 && args.at(0).isString()) {
    const auto value = args.at(0).toString();
    if (method == "prepare")
      prepare(value);
    else if (method == "execution")
      readExecution(value);
    else if (method == "evidence")
      navigateEvidence(value);
    else
      return std::nullopt;
    return QJsonValue{};
  }
  if (method != "send" || args.size() != 2 || !args.at(0).isString() ||
      !args.at(1).isObject())
    return std::nullopt;
  if (!AnalysisReady || Controller.running()) {
    fail(tr("The analysis is already running or the session is not ready."));
    return QJsonValue{};
  }
  const auto text = args.at(0).toString().trimmed();
  const auto context = args.at(1).toObject();
  if (text.isEmpty() || context.value("epoch") != Binding.value("epoch") ||
      context.value("project_id") != Binding.value("project_id")) {
    fail(tr("The question or its captured analysis context is unavailable."));
    return QJsonValue{};
  }
  Controller.setDraft(text);
  const auto function = context.value("function_address").toString();
  const auto address = context.value("address").toString();
  const auto selection = context.value("selection_text").toString();
  if (!function.isEmpty() || !address.isEmpty() || !selection.isEmpty()) {
    const auto captured =
        QStringLiteral("Function: %1\nAddress: %2\nRevision: %3\n%4")
            .arg(function, address, context.value("revision").toString(),
                 selection);
    Controller.attachSelection(captured, tr("Captured ND GUI context"));
  }
  if (!Controller.sendRequest(
          Controller.selectedObject().value("id").toString()))
    Publish({{"type", "state"}, {"running", false}});
  return QJsonValue{};
}

} // namespace neverd::agent
