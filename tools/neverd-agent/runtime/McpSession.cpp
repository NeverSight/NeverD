//===- McpSession.cpp - Object-scoped ND analysis process
//-------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reuse ND's read-only engine MCP surface without embedding analysis logic.
//===----------------------------------------------------------------------===//
#include "McpSession.h"

#include "models/ModelProfile.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QUuid>
#include <QtConcurrentRun>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace neverd::agent {
McpSession::McpSession(QObject *parent) : QObject(parent) {
#ifdef Q_OS_WIN
  Process.setCreateProcessArgumentsModifier(
      [](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
      });
#endif
  connect(&Process, &QProcess::readyReadStandardOutput, this, [this] {
    Buffer += Process.readAllStandardOutput();
    if (Buffer.size() > model_limits::kMaxResponseBytes) {
      failAll("ND response exceeds the size limit.");
      Process.kill();
      return;
    }
    while (true) {
      const auto end = Buffer.indexOf('\n');
      if (end < 0)
        break;
      const auto line = Buffer.left(end);
      Buffer.remove(0, end + 1);
      if (line.trimmed().isEmpty())
        continue;
      QJsonParseError parse;
      const auto document = QJsonDocument::fromJson(line, &parse);
      if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        failAll("ND sent malformed MCP JSON.");
        Process.kill();
        return;
      }
      const auto response = document.object();
      if (!response.contains("id"))
        continue;
      const auto id = response["id"].toInteger(-1);
      auto done = Pending.take(id);
      if (auto *timer = Timers.take(id))
        timer->deleteLater();
      if (!done)
        continue;
      if (response.contains("error"))
        done({{"error", response["error"].toObject()["message"].toString(
                            "ND request failed.")}});
      else if (!response["result"].isObject())
        done({{"error", "ND returned a non-object result."}});
      else
        done(response["result"].toObject());
    }
  });
  // Drain diagnostics without allowing a child's stderr buffer to grow forever.
  connect(&Process, &QProcess::readyReadStandardError, this,
          [this] { Process.readAllStandardError(); });
  connect(&Process, &QProcess::errorOccurred, this,
          [this](auto) { failAll("ND process: " + Process.errorString()); });
  connect(&Process, &QProcess::finished, this, [this](int code, auto) {
    failAll(QString("ND process exited (%1).").arg(code));
  });
}
McpSession::~McpSession() {
  Process.disconnect(this);
  if (Process.state() != QProcess::NotRunning) {
    Process.kill();
    Process.waitForFinished(model_limits::kProcessExitWaitMs);
  }
}
void McpSession::send(const QJsonObject &message) {
  Process.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}
qint64 McpSession::request(const QString &method, const QJsonObject &params,
                           Done done) {
  const auto id = ++NextId;
  Pending.insert(id, std::move(done));
  auto *timer = new QTimer(this);
  timer->setSingleShot(true);
  Timers.insert(id, timer);
  connect(timer, &QTimer::timeout, this, [this, id] {
    auto done = Pending.take(id);
    if (auto *timer = Timers.take(id))
      timer->deleteLater();
    send({{"jsonrpc", "2.0"},
          {"method", "notifications/cancelled"},
          {"params", QJsonObject{{"requestId", id}}}});
    if (done)
      done({{"error", "ND request timed out; engine computation may still be "
                      "finishing."}});
  });
  timer->start(TimeoutMs);
  send(
      {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
  return id;
}
void McpSession::start(const QString &executable, const AnalysisObject &object,
                       int timeoutSeconds, Done ready,
                       const QString &attachment) {
  TimeoutMs = timeoutSeconds * 1000;
  Identity = QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto *watcher = new QFutureWatcher<FingerprintResult>(this);
  connect(
      watcher, &QFutureWatcher<FingerprintResult>::finished, this,
      [this, watcher, executable, object, attachment,
       ready = std::move(ready)] {
        const auto fingerprint = watcher->result();
        watcher->deleteLater();
        if (!fingerprint.Error.isEmpty() ||
            fingerprint.Sha256 != object.Sha256) {
          ready({{"error",
                  fingerprint.Error.isEmpty()
                      ? "Analysis object changed since it was registered. "
                        "Add the new version as a separate object."
                      : fingerprint.Error}});
          return;
        }
        auto startDone = std::make_shared<Done>(ready);
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection =
            connect(&Process, &QProcess::errorOccurred, this,
                    [startDone, connection](auto) {
                      QObject::disconnect(*connection);
                      if (*startDone) {
                        auto done = std::move(*startDone);
                        done({{"error", "Unable to start ND MCP. Check its "
                                        "executable path and dependencies."}});
                      }
                    });
        connect(
            &Process, &QProcess::started, this,
            [this, startDone, connection, attached = !attachment.isEmpty()] {
              QObject::disconnect(*connection);
              request(
                  "initialize",
                  {{"protocolVersion", "2025-06-18"},
                   {"capabilities", QJsonObject{}},
                   {"clientInfo",
                    QJsonObject{{"name", "neverd-agent"}, {"version", "0.1"}}}},
                  [this, startDone, attached](QJsonObject result) {
                    if (!*startDone)
                      return;
                    if (result.contains("error")) {
                      auto done = std::move(*startDone);
                      done(result);
                      return;
                    }
                    send({{"jsonrpc", "2.0"},
                          {"method", "notifications/initialized"}});
                    request("tools/list", {},
                            [this, startDone, attached](QJsonObject result) {
                              if (!*startDone)
                                return;
                              auto done = std::move(*startDone);
                              if (result.contains("error")) {
                                done(result);
                                return;
                              }
                              for (const auto &tool :
                                   result["tools"].toArray()) {
                                const auto name =
                                    tool.toObject().value("name").toString();
                                if (!attached || (name != "current_selection" &&
                                                  name != "navigate_gui" &&
                                                  name != "highlight_gui"))
                                  Tools.append(tool);
                              }
                              Ready = true;
                              done({{"ready", true}});
                            });
                  });
            },
            Qt::SingleShotConnection);
        Process.setWorkingDirectory(QFileInfo(executable).absolutePath());
        Process.start(executable,
                      attachment.isEmpty()
                          ? QStringList{"--file", object.Path}
                          : QStringList{"--attach", attachment},
                      QIODevice::ReadWrite);
      });
  watcher->setFuture(QtConcurrent::run(
      [path = object.Path] { return ProjectStore::fingerprint(path); }));
}
qint64 McpSession::call(const QString &tool, const QJsonObject &arguments,
                        Done done) {
  bool allowed = false;
  for (const auto &value : Tools) {
    const auto definition = value.toObject();
    if (definition["name"] == tool &&
        definition["annotations"].toObject()["readOnlyHint"].toBool())
      allowed = true;
  }
  if (!Ready || !allowed) {
    done({{"error", "ND tool is unavailable or is not read-only."}});
    return 0;
  }
  return request("tools/call", {{"name", tool}, {"arguments", arguments}},
                 std::move(done));
}
void McpSession::cancel(qint64 id) {
  auto done = Pending.take(id);
  if (auto *timer = Timers.take(id))
    timer->deleteLater();
  if (!done)
    return;
  send({{"jsonrpc", "2.0"},
        {"method", "notifications/cancelled"},
        {"params", QJsonObject{{"requestId", id}}}});
  done({{"error",
         "ND query cancelled; engine computation may still be finishing."},
        {"cancelled", true}});
}
void McpSession::failAll(const QString &error) {
  Ready = false;
  auto pending = std::move(Pending);
  Pending.clear();
  for (auto *timer : std::as_const(Timers))
    timer->deleteLater();
  Timers.clear();
  for (auto &done : pending)
    done({{"error", error}});
}
qint64 McpSession::readResource(const QString &uri, Done done) {
  if (!Ready || !uri.startsWith("neverd://")) {
    done({{"error", "Expected a resource URI returned by this ND session."}});
    return 0;
  }
  return request("resources/read", {{"uri", uri}}, std::move(done));
}
} // namespace neverd::agent
