//===- LineageSession.cpp - Retained offline workspace navigation ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Revision-bound dispatch with cancellation acknowledgement separated from
/// terminal completion. Failed replacement leaves loaded evidence visible.
//===----------------------------------------------------------------------===//

#include "LineageSession.h"

#include "../ResourceLimits.h"

#include <QJsonArray>
#include <QTimer>

namespace neverd::gui {
LineageSession::LineageSession(const QString &Worker, QObject *Parent)
    : QObject(Parent), Client(this) {
  connect(&Client, &EngineClient::message, this, &LineageSession::receive);
  connect(&Client, &EngineClient::failure, this, [this](const QString &Error) {
    Ready = false;
    ReleaseEditing = false;
    Requests.clear();
    Cancellable.clear();
    Message = tr("Version graph worker: %1").arg(Error);
    emit changed();
    emit failed(Message);
  });
  Client.start(Worker);
}
void LineageSession::receive(const QJsonObject &Value) {
  if (Value["type"] == QJsonValue("hello")) {
    for (const auto &Op : Value["lineage_cancellable_operations"].toArray())
      CancellableOperations.append(Op.toString());
    Ready = Value["protocol_major"] == QJsonValue(1) &&
            Value["capabilities"].toArray().contains("lineage_load") &&
            CancellableOperations.contains("lineage_load");
    ReleaseEditing =
        Ready &&
        Value["capabilities"].toArray().contains("lineage_edit_releases") &&
        CancellableOperations.contains("lineage_edit_releases");
    if (!Ready)
      Message = tr("This worker does not support version comparison graphs.");
    emit changed();
    return;
  }
  if (Value["type"] != QJsonValue("response"))
    return;
  const auto ID = Value["request_id"].toString();
  if (Value["status"] == QJsonValue("progress")) {
    if (Cancellable.contains(ID))
      emit progress(Value["payload"].toObject());
    return;
  }
  const auto It = Requests.find(ID);
  if (It == Requests.end())
    return;
  const auto Pending = std::move(It.value());
  Requests.erase(It);
  Cancellable.removeAll(ID);
  if (Cancellable.isEmpty())
    CancelRequested = false;
  if (Pending.Owner && Pending.Complete)
    Pending.Complete(Value);
  emit changed();
}
void LineageSession::request(const QString &Operation, QJsonObject Payload,
                             QObject *Owner, Reply Complete) {
  if (!Ready ||
      (Operation != "cancel" &&
       (Requests.size() >= ::neverd::limits::gui::kComparisonPendingRequests ||
        (CancelRequested && cancellable())))) {
    QTimer::singleShot(0, Owner, [Complete = std::move(Complete)] {
      Complete(
          {{"status", "error"},
           {"error",
            QJsonObject{
                {"code", "lineage_busy"},
                {"message",
                 tr("Version graph worker is unavailable or stopping.")}}}});
    });
    return;
  }
  if (Operation != "cancel" && !revision().isEmpty())
    Payload["expected_lineage_revision"] = revision();
  if (Operation == "lineage_query") {
    auto Q = Payload["query"].toObject();
    for (const auto *Key : {"max_work", "timeout_ms"})
      if (!Q.contains(Key) && QueryOptions.contains(Key))
        Q[Key] = QueryOptions[Key];
    Payload["query"] = Q;
  }
  const auto ID = Client.request(Operation, Payload);
  Requests.insert(ID, Pending{Owner, std::move(Complete)});
  if (CancellableOperations.contains(Operation)) {
    if (Cancellable.isEmpty())
      CancelRequested = false;
    Cancellable.append(ID);
  }
  emit changed();
}
void LineageSession::load(const QJsonObject &Payload) {
  if (!Ready || busy())
    return;
  Message.clear();
  request("lineage_load", Payload, this,
          [this, Appended = Payload.value("append").toBool()](
              const QJsonObject &Value) {
            if (Value["status"] != QJsonValue("ok")) {
              const auto Error = Value["error"].toObject();
              if (Error["code"] == QJsonValue("cancelled"))
                Message = tr("Loading stopped. You can retry.");
              else if (Error["code"] == QJsonValue("invalid_json") &&
                       Value["payload"].toObject()["source"] ==
                           QJsonValue("lineage_file"))
                Message = tr("The version graph file is not valid UTF-8 JSON. "
                             "Check the file and try again.");
              else
                Message = Error["message"].toString();
              if (!Summary.isEmpty())
                Message += tr("\nYour previous graph is still available.");
              emit failed(Message);
              return;
            }
            Summary = Value["payload"].toObject();
            Message = tr("Version graph loaded.");
            emit collectionChanged(Appended);
          });
}
void LineageSession::save(const QString &Path, const QString &Report) {
  if (!Ready || busy() || Summary.isEmpty())
    return;
  QJsonObject Payload{{"path", Path}};
  if (!Report.isEmpty())
    Payload["report"] = Report;
  request(Report.isEmpty() ? "lineage_save" : "lineage_save_report", Payload,
          this, [this](const QJsonObject &Value) {
            if (Value["status"] != QJsonValue("ok")) {
              Message = Value["error"].toObject()["message"].toString();
              emit failed(Message);
              return;
            }
            const auto Path = Value["payload"].toObject()["path"].toString();
            Message = tr("Saved %1").arg(Path);
            emit saved(Path);
          });
}
void LineageSession::editReleases(QJsonObject Edit, const QJsonObject &Options,
                                  const QString &Focus) {
  if (!Ready || !ReleaseEditing || busy() || Summary.isEmpty())
    return;
  if (!Edit.contains("revision"))
    Edit["revision"] = revision();
  Message.clear();
  request("lineage_edit_releases", {{"edit", Edit}, {"options", Options}}, this,
          [this, Focus](const QJsonObject &Value) {
            if (Value["status"] != QJsonValue("ok")) {
              Message = Value["error"].toObject()["message"].toString();
              Message += tr("\nYour previous graph is still available.");
              emit failed(Message);
              return;
            }
            Summary = Value["payload"].toObject();
            Message = tr("Release metadata saved in the current graph.");
            emit releasesEdited(Focus);
            emit collectionChanged(true);
          });
}
void LineageSession::cancel() {
  if (CancelRequested || Cancellable.isEmpty())
    return;
  CancelRequested = true;
  const auto Active = Cancellable;
  for (const auto &ID : Active)
    request("cancel", {{"request_id", ID}}, this,
            [this](const QJsonObject &Value) {
              if (Value["status"] != QJsonValue("ok")) {
                Message = tr("The worker could not acknowledge cancellation.");
                emit failed(Message);
              }
            });
  emit changed();
}
} // namespace neverd::gui
