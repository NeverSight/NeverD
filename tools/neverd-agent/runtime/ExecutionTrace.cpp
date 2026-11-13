//===- ExecutionTrace.cpp - Public execution record ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "ExecutionTrace.h"

#include "models/ModelProfile.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QUuid>

namespace neverd::agent {
std::optional<qsizetype> ExecutionTrace::observe(const QJsonObject &event) {
  const auto kind = event["kind"].toString();
  if (kind == "tool_request") {
    const auto arguments = event["arguments"].toObject();
    const auto tool = event["tool"].toString();
    const auto nested = arguments["arguments"];
    const auto target =
        tool == "nd_query"
            ? (nested.isString()
                   ? QJsonDocument::fromJson(nested.toString().toUtf8())
                         .object()
                   : nested.toObject())
            : arguments;
    const QString key =
        event["agentId"].toString() + '/' + event["callId"].toString();
    const auto node = event["node"].toObject();
    const auto index = Rows.size();
    Calls.insert(key, index);
    Rows.append(QJsonObject{
        {"id", key},
        {"tool", tool},
        {"name", tool == "nd_query" ? arguments["tool"] : event["tool"]},
        {"objectId", node["objectId"]},
        {"label", node["label"]},
        {"depth", node["depth"]},
        {"address", target["address"]},
        {"state", "running"},
        {"arguments", arguments},
        {"startedAt", event["time"]},
        {"startMs", event["elapsedMs"]},
        {"elapsedMs", 0}});
    return index;
  }
  if (kind == "tool_result" || kind == "tool_skipped") {
    const QString key =
        event["agentId"].toString() + '/' + event["callId"].toString();
    const auto result = event["result"].toObject();
    if (!Calls.contains(key)) {
      Calls.insert(key, Rows.size());
      Rows.append(QJsonObject{{"id", key},
                              {"name", event["tool"]},
                              {"label", event["node"].toObject()["label"]},
                              {"startMs", event["elapsedMs"]}});
    }
    const auto index = Calls[key];
    auto row = Rows[index].toObject();
    row["state"] = kind == "tool_skipped"         ? "skipped"
                   : result["cancelled"].toBool() ? "cancelled"
                   : result.contains("error") || result["isError"].toBool()
                       ? "failed"
                       : "succeeded";
    row["result"] = result;
    row["evidenceId"] = result["evidence_id"];
    row["elapsedMs"] = qMax<qint64>(0, event["elapsedMs"].toInteger() -
                                           row["startMs"].toInteger());
    Rows[index] = row;
    return index;
  }
  if (kind == "user_input_applied") {
    Rows.append(
        QJsonObject{{"id", event["inputId"]},
                    {"name", "已接收补充指令"},
                    {"label", event["node"].toObject()["label"]},
                    {"state", "succeeded"},
                    {"arguments", QJsonObject{{"text", event["text"]}}}});
    return Rows.size() - 1;
  }
  return std::nullopt;
}
QVariantList ExecutionTrace::read(const Project &project, const QString &runId,
                                  QString &error) {
  if (QUuid(runId).isNull() ||
      QUuid(runId).toString(QUuid::WithoutBraces) != runId) {
    error = "Invalid run identity.";
    return {};
  }
  QFile file(QDir(ProjectStore::stateDirectory(project))
                 .filePath("runs/" + runId + "/events.jsonl"));
  if (!file.open(QIODevice::ReadOnly)) {
    error = file.errorString();
    return {};
  }
  ExecutionTrace trace;
  while (!file.atEnd()) {
    const auto line = file.readLine(model_limits::kMaxResponseBytes + 1);
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(line, &parse);
    if (line.size() > model_limits::kMaxResponseBytes ||
        parse.error != QJsonParseError::NoError || !document.isObject()) {
      error = "Invalid or incomplete execution record.";
      return {};
    }
    trace.observe(document.object());
  }
  return trace.rows();
}
} // namespace neverd::agent
