//===- WorkerSession.cpp - Revision-bound ND command client ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WorkerSession.h"

#include "common/transport/Json.h"
#include "models/ModelProfile.h"

#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QtConcurrentRun>
#include <QtEndian>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace neverd::agent {
WorkerSession::WorkerSession(QObject *parent) : QObject(parent) {
#ifdef Q_OS_WIN
  Process.setCreateProcessArgumentsModifier(
      [](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
      });
#endif
  Deadline.setSingleShot(true);
  StopDeadline.setSingleShot(true);
  connect(&Deadline, &QTimer::timeout, this, [this] {
    fail("ND command timed out. A submitted write may already have committed; "
         "it was not retried.");
    Process.kill();
  });
  connect(&StopDeadline, &QTimer::timeout, this, [this] { Process.kill(); });
  connect(&Process, &QProcess::readyReadStandardOutput, this,
          &WorkerSession::receive);
  connect(&Process, &QProcess::readyReadStandardError, this,
          [this] { Process.readAllStandardError(); });
  connect(&Process, &QProcess::errorOccurred, this,
          [this](auto) { fail("ND worker: " + Process.errorString()); });
  connect(&Process, &QProcess::finished, this, [this](int, auto) {
    Deadline.stop();
    StopDeadline.stop();
    if (Current || Ready)
      fail("ND worker exited before acknowledging the command.");
    if (Stopped) {
      auto done = std::move(Stopped);
      done();
    }
  });
}
WorkerSession::~WorkerSession() {
  Process.disconnect(this);
  if (Process.state() != QProcess::NotRunning) {
    Process.kill();
    Process.waitForFinished(model_limits::kProcessExitWaitMs);
  }
}
void WorkerSession::start(const QString &program, const AnalysisObject &object,
                          bool readOnly, int timeoutSeconds, Done ready) {
  Object = object;
  ReadOnly = readOnly;
  TimeoutMs = timeoutSeconds * 1000;
  Ready = std::move(ready);
  auto *watcher = new QFutureWatcher<FingerprintResult>(this);
  connect(
      watcher, &QFutureWatcher<FingerprintResult>::finished, this,
      [this, watcher, program] {
        const auto fingerprint = watcher->result();
        watcher->deleteLater();
        if (!fingerprint.Error.isEmpty() ||
            fingerprint.Sha256 != Object.Sha256) {
          fail(fingerprint.Error.isEmpty()
                   ? "Analysis object no longer matches its registered SHA-256."
                   : fingerprint.Error);
          return;
        }
        Process.setWorkingDirectory(QFileInfo(program).absolutePath());
        Process.start(program, {});
        Deadline.start(TimeoutMs);
      });
  watcher->setFuture(QtConcurrent::run(
      [path = object.Path] { return ProjectStore::fingerprint(path); }));
}
void WorkerSession::receive() {
  Buffer += Process.readAllStandardOutput();
  while (Buffer.size() >= 4) {
    const auto size = qFromBigEndian<quint32>(Buffer.constData());
    if (size == 0 || size > transport::MaxJsonBytes) {
      fail("Invalid ND worker frame length.");
      Process.kill();
      return;
    }
    if (Buffer.size() < qint64(size) + 4)
      return;
    const auto bytes = Buffer.mid(4, size);
    Buffer.remove(0, size + 4);
    QJsonObject message;
    try {
      const auto admitted = transport::parseJson(
          std::string_view(bytes.constData(), bytes.size()));
      if (!admitted.is_object())
        throw std::runtime_error("Worker frame is not an object.");
      message = QJsonDocument::fromJson(bytes).object();
    } catch (const std::exception &error) {
      fail(QString::fromUtf8(error.what()));
      Process.kill();
      return;
    }
    if (message["type"] == "hello" && !Hello) {
      Hello = true;
      Deadline.stop();
      if (message["protocol_major"].toInt() != 1 ||
          !message["capabilities"].isArray()) {
        fail("Unsupported ND worker protocol.");
        return;
      }
      for (const auto &capability : message["capabilities"].toArray())
        Capabilities.append(capability.toString());
      request("open",
              {{"path", Object.Path},
               {"read_only", ReadOnly},
               {"analysis", false},
               {"warm", false}},
              false, [this](const auto &result) { opened(result); });
    } else if (message["type"] == "response" &&
               message["request_id"] == RequestId && Current) {
      if (message["status"] == "progress")
        continue;
      Deadline.stop();
      Revision = message["revision"].toString();
      ProjectId = message["project_id"].toString();
      auto done = std::move(Current);
      Mutation = false;
      done(message);
    } else if (message["type"] == "fatal") {
      fail(message["error"].toObject()["message"].toString(
          "ND worker protocol failure."));
    }
  }
}
void WorkerSession::opened(const QJsonObject &result) {
  if (!Ready)
    return;
  auto done = std::move(Ready);
  if (result["status"] != "ok") {
    done(result);
    return;
  }
  if (result["payload"].toObject()["sha256"] != Object.Sha256) {
    done({{"status", "error"},
          {"error", QJsonObject{{"message",
                                 "ND opened a different binary identity."}}}});
    return;
  }
  done(result);
}
void WorkerSession::request(const QString &operation,
                            const QJsonObject &payload, bool mutation,
                            Done done) {
  if (Failed || Current || Process.state() != QProcess::Running) {
    done({{"status", "error"},
          {"error",
           QJsonObject{
               {"message", "ND command session is unavailable or busy."}}}});
    return;
  }
  RequestId = QString::number(++NextId);
  QJsonObject request{{"protocol_major", 1},
                      {"request_id", RequestId},
                      {"operation", operation},
                      {"payload", payload}};
  if (operation != "open" && !Revision.isEmpty())
    request["expected_revision"] = Revision;
  if (operation != "open")
    request["project_id"] = ProjectId;
  const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
  if (bytes.size() > transport::MaxJsonBytes) {
    done({{"status", "error"},
          {"error",
           QJsonObject{
               {"message", "ND command exceeds the worker frame budget."}}}});
    return;
  }
  Current = std::move(done);
  Mutation = mutation;
  QByteArray frame(4, '\0');
  qToBigEndian<quint32>(quint32(bytes.size()), frame.data());
  frame += bytes;
  Process.write(frame);
  Deadline.start(TimeoutMs);
}
void WorkerSession::fail(const QString &error) {
  Failed = true;
  Deadline.stop();
  QJsonObject result{{"status", "error"},
                     {"uncertain", Mutation},
                     {"error", QJsonObject{{"message", error}}}};
  auto current = std::move(Current);
  auto ready = std::move(Ready);
  Mutation = false;
  if (current)
    current(result);
  if (ready)
    ready(result);
}
void WorkerSession::stop(std::function<void()> done) {
  Stopped = std::move(done);
  if (Process.state() == QProcess::NotRunning) {
    auto stopped = std::move(Stopped);
    if (stopped)
      stopped();
    return;
  }
  Process.closeWriteChannel();
  StopDeadline.start(model_limits::kProcessExitWaitMs);
}
} // namespace neverd::agent
