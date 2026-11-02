//===- InstanceServer.cpp - Other programs' way into a workbench -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "InstanceServer.h"

#include "DeepLink.h"
#include "SettingsKeys.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <memory>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace neverd::gui {
namespace {

QJsonObject succeeded() { return {{QStringLiteral("status"), "ok"}}; }

QJsonObject failed(const QString &message) {
  return {{QStringLiteral("status"), "error"},
          {QStringLiteral("message"), message}};
}

std::optional<QJsonObject> parseReply(const QByteArray &text) {
  const auto document = QJsonDocument::fromJson(text);
  if (!document.isObject())
    return std::nullopt;
  const auto reply = document.object();
  const auto status = reply.value(QStringLiteral("status")).toString();
  if (status == QLatin1String("ok") ||
      (status == QLatin1String("error") &&
       !reply.value(QStringLiteral("message")).toString().isEmpty()))
    return reply;
  return std::nullopt;
}

/// A server's name: its process, and its serial among the process's.
QString instanceName(qint64 pid, int serial) {
  return QStringLiteral("%1-%2").arg(pid).arg(serial);
}

int nextSerial() {
  static int serial = 0;
  return ++serial;
}

#ifdef Q_OS_WIN
QString pipeName(const QString &name) {
  return QStringLiteral("\\\\.\\pipe\\neverd-instance-") + name;
}
#endif

bool processAlive(qint64 pid) {
  if (pid <= 0)
    return false;
#ifdef Q_OS_WIN
  HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
  if (!process)
    return GetLastError() == ERROR_ACCESS_DENIED;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
  CloseHandle(process);
  return alive;
#else
  return kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

#ifndef Q_OS_WIN
/// Whether the peer of the connected socket \p descriptor is this user.
bool sameUser(qintptr descriptor) {
#ifdef Q_OS_LINUX
  ucred credentials{};
  socklen_t size = sizeof credentials;
  return getsockopt(int(descriptor), SOL_SOCKET, SO_PEERCRED, &credentials,
                    &size) == 0 &&
         credentials.uid == getuid();
#else
  uid_t user = 0;
  gid_t group = 0;
  return getpeereid(int(descriptor), &user, &group) == 0 && user == getuid();
#endif
}
#endif

} // namespace

struct InstanceServer::RequestBuffer {
  QByteArray text;
  qsizetype checked = 0;
  int depth = 0;
  bool quoted = false, escaped = false, container = false;

  void resetProbe() {
    checked = 0;
    depth = 0;
    quoted = escaped = container = false;
  }

  // This is only a completion hint. Qt still validates the complete JSON.
  bool complete() {
    for (; checked < text.size(); ++checked) {
      const char c = text[checked];
      if (quoted) {
        if (escaped)
          escaped = false;
        else if (c == '\\')
          escaped = true;
        else if (c == '"')
          quoted = false;
      } else if (c == '"') {
        quoted = true;
      } else if (c == '{' || c == '[') {
        ++depth;
        container = true;
      } else if (c == '}' || c == ']') {
        --depth;
        container = true;
      }
    }
    return container && depth <= 0 && !quoted;
  }
};

InstanceServer::InstanceServer(QObject *parent) : QObject(parent) {
  connect(&server_, &QLocalServer::newConnection, this,
          &InstanceServer::accept);
}

InstanceServer::~InstanceServer() { stop(); }

QString InstanceServer::directory() {
#ifdef Q_OS_WIN
  // Only the files naming the pipes live here.
  const QString base =
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#else
  // $XDG_RUNTIME_DIR, or the directory Qt makes in its place, both the
  // user's own.
  const QString base =
      QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
#endif
  if (base.isEmpty())
    return {};
  const QString path = base + QStringLiteral("/neverd-instances");
  if (!QDir().mkpath(path) ||
      !QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner |
                                       QFile::ExeOwner))
    return {};
  return path;
}

bool InstanceServer::start() {
  stop();
  error_.clear();
  if (QSettings().value(settings::InstancesDisabled, false).toBool()) {
    error_ = QStringLiteral("turned off by the instances/disabled setting");
    return false;
  }
  const QString directory = InstanceServer::directory();
  if (directory.isEmpty()) {
    error_ = QStringLiteral("no private directory for the server");
    return false;
  }
  const QString name =
      instanceName(QCoreApplication::applicationPid(), nextSerial());
  server_.setSocketOptions(QLocalServer::UserAccessOption);
#ifdef Q_OS_WIN
  const QString endpoint = pipeName(name);
#else
  const QString endpoint = directory + QLatin1Char('/') + name;
  QLocalServer::removeServer(endpoint);
#endif
  if (!server_.listen(endpoint)) {
    error_ = server_.errorString();
    return false;
  }
#ifdef Q_OS_WIN
  marker_ = directory + QLatin1Char('/') + name;
  QFile marker(marker_);
  if (!marker.open(QIODevice::WriteOnly)) {
    error_ = marker.errorString();
    stop();
    return false;
  }
#endif
  return true;
}

void InstanceServer::stop() {
  server_.close();
  if (!marker_.isEmpty())
    QFile::remove(std::exchange(marker_, {}));
}

void InstanceServer::accept() {
  while (QLocalSocket *socket = server_.nextPendingConnection()) {
#ifndef Q_OS_WIN
    // The directory is the user's own; this refuses whatever got in anyway.
    if (!sameUser(socket->socketDescriptor())) {
      socket->abort();
      socket->deleteLater();
      continue;
    }
#endif
    socket->setReadBufferSize(
        ::neverd::limits::gui::kInstanceServerMaxRequestBytes + 1);
    auto request = std::make_shared<RequestBuffer>();
    connect(socket, &QLocalSocket::readyRead, this,
            [this, socket, request] { read(socket, *request); });
    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    if (socket->bytesAvailable())
      read(socket, *request);
  }
}

void InstanceServer::read(QLocalSocket *socket, RequestBuffer &request) {
  QByteArray &buffer = request.text;
  buffer += socket->readAll();
  const auto rejectOversized = [&] {
    reply(socket,
          failed(
              QStringLiteral("Request exceeds %1 bytes")
                  .arg(::neverd::limits::gui::kInstanceServerMaxRequestBytes)));
    socket->disconnectFromServer();
    buffer.clear();
    request.resetProbe();
  };
  qsizetype consumed = 0;
  for (qsizetype newline; (newline = buffer.indexOf('\n', consumed)) >= 0;) {
    const auto size = newline - consumed;
    if (size > ::neverd::limits::gui::kInstanceServerMaxRequestBytes)
      return rejectOversized();
    const QByteArray line = buffer.mid(consumed, size);
    consumed = newline + 1;
    if (!line.trimmed().isEmpty())
      answer(socket, line);
  }
  if (consumed) {
    buffer.remove(0, consumed);
    request.resetProbe();
  }
  // Bound the request before parsing or dispatching it, including the
  // complete objects clients send without a newline.
  if (buffer.size() > ::neverd::limits::gui::kInstanceServerMaxRequestBytes)
    return rejectOversized();
  // IDA's hcli writes one object without a newline and waits.
  if (request.complete()) {
    answer(socket, buffer);
    buffer.clear();
    request.resetProbe();
  }
}

void InstanceServer::answer(QLocalSocket *socket, const QByteArray &text) {
  QJsonParseError parse;
  const QJsonDocument document = QJsonDocument::fromJson(text, &parse);
  if (parse.error != QJsonParseError::NoError)
    return reply(
        socket,
        failed(
            QStringLiteral("JSON parse error: %1").arg(parse.errorString())));
  if (!document.isObject())
    return reply(socket, failed(QStringLiteral("Expected a JSON object")));
  const QJsonObject request = document.object();
  const QString command = request.value(QStringLiteral("cmd")).toString();
  if (command == QLatin1String("ping"))
    return reply(socket, succeeded());
  if (command == QLatin1String("get_info") ||
      command == QLatin1String("is_analysis_complete")) {
    QJsonObject info = info_ ? info_() : QJsonObject();
    if (command == QLatin1String("is_analysis_complete"))
      return reply(
          socket, {{QStringLiteral("status"), "ok"},
                   {QStringLiteral("analysis_complete"),
                    info.value(QStringLiteral("analysis_complete")).toBool()}});
    info.insert(QStringLiteral("status"), "ok");
    info.insert(QStringLiteral("pid"), QCoreApplication::applicationPid());
    info.insert(QStringLiteral("server"), serverName());
    return reply(socket, info);
  }
  if (command == QLatin1String("open_link")) {
    const ParsedLink parsed =
        parseDeepLink(request.value(QStringLiteral("uri")).toString());
    if (!parsed.link)
      return reply(socket, failed(parsed.error));
    if (!handler_)
      return reply(socket,
                   failed(QStringLiteral("This workbench opens no links")));
    // The link resolves through the worker; its reply follows.
    handler_(*parsed.link,
             [socket = QPointer<QLocalSocket>(socket)](const QString &error) {
               if (socket)
                 reply(socket, error.isEmpty() ? succeeded() : failed(error));
             });
    return;
  }
  reply(socket, failed(QStringLiteral("Unknown command: %1").arg(command)));
}

void InstanceServer::reply(QLocalSocket *socket, const QJsonObject &reply) {
  if (socket->state() != QLocalSocket::ConnectedState)
    return;
  socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
  socket->flush();
}

QList<InstanceServer::Instance> InstanceServer::discover() {
  QList<Instance> instances;
  const QString directory = InstanceServer::directory();
  if (directory.isEmpty())
    return instances;
  static const QRegularExpression Name(QStringLiteral("^([0-9]+)-[0-9]+$"));
  // A socket is a system entry to QDir.
  const auto entries = QDir(directory).entryInfoList(
      QDir::Files | QDir::System | QDir::Hidden | QDir::NoDotAndDotDot);
  for (const QFileInfo &entry : entries) {
    const auto match = Name.match(entry.fileName());
    if (!match.hasMatch())
      continue;
    const qint64 pid = match.captured(1).toLongLong();
    if (!processAlive(pid)) {
      QFile::remove(entry.filePath());
      continue;
    }
#ifdef Q_OS_WIN
    instances.push_back({pid, pipeName(entry.fileName())});
#else
    instances.push_back({pid, entry.filePath()});
#endif
  }
  std::sort(instances.begin(), instances.end(),
            [](const Instance &left, const Instance &right) {
              return left.pid != right.pid ? left.pid < right.pid
                                           : left.server < right.server;
            });
  return instances;
}

std::optional<QJsonObject> InstanceServer::request(const QString &server,
                                                   const QJsonObject &request,
                                                   int timeoutMs) {
  QLocalSocket socket;
  socket.setReadBufferSize(::neverd::limits::gui::kInstanceServerMaxReplyBytes +
                           1);
  socket.connectToServer(server);
  QDeadlineTimer deadline(timeoutMs);
  if (!socket.waitForConnected(int(deadline.remainingTime())))
    return std::nullopt;
  socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
  if (socket.bytesToWrite() &&
      !socket.waitForBytesWritten(int(deadline.remainingTime())))
    return std::nullopt;
  QByteArray buffer;
  while (!buffer.contains('\n')) {
    if (deadline.hasExpired() ||
        (!socket.bytesAvailable() &&
         !socket.waitForReadyRead(int(deadline.remainingTime()))))
      return std::nullopt;
    buffer += socket.readAll();
    if (buffer.size() > ::neverd::limits::gui::kInstanceServerMaxReplyBytes)
      return std::nullopt;
  }
  return parseReply(buffer.left(buffer.indexOf('\n')));
}

QList<std::optional<QJsonObject>> InstanceServer::requestMany(
    const QStringList &servers, const QJsonObject &request, int timeoutMs,
    std::function<bool(int, const QJsonObject &)> stop) {
  QList<std::optional<QJsonObject>> replies(servers.size());
  if (servers.isEmpty() || timeoutMs <= 0)
    return replies;
  struct Probe {
    QLocalSocket socket;
    QTimer deadline;
    QByteArray buffer;
    bool done = false;
  };
  std::vector<std::unique_ptr<Probe>> probes;
  probes.reserve(servers.size());
  // The event-loop context dies before the sockets, retiring their callbacks.
  QEventLoop loop;
  QTimer deadline;
  deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  const QByteArray wire =
      QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
  // Reserve time for every wave: an entire stalled first wave must not
  // prevent queued healthy endpoints from being tried before the deadline.
  const auto waves = 1 + (servers.size() - 1) /
                             ::neverd::limits::gui::kInstanceProbeConcurrency;
  const int probeTimeoutMs = std::max<qint64>(1, qint64(timeoutMs) / waves);
  int next = 0, active = 0, finished = 0;
  bool stopped = false;
  std::function<void()> pump;
  const auto complete = [&](int index, std::optional<QJsonObject> reply) {
    if (stopped || probes[index]->done)
      return;
    probes[index]->done = true;
    replies[index] = std::move(reply);
    probes[index]->deadline.stop();
    probes[index]->socket.abort();
    --active;
    ++finished;
    if (replies[index] && stop && stop(index, *replies[index]))
      stopped = true;
    if (stopped || finished == servers.size())
      loop.quit();
    else
      QTimer::singleShot(0, &loop, [&] { pump(); });
  };
  pump = [&] {
    while (!stopped && next < servers.size() &&
           active < ::neverd::limits::gui::kInstanceProbeConcurrency) {
      const int index = next++;
      probes.push_back(std::make_unique<Probe>());
      auto &probe = *probes.back();
      ++active;
      probe.socket.setReadBufferSize(
          ::neverd::limits::gui::kInstanceServerMaxReplyBytes + 1);
      probe.deadline.setSingleShot(true);
      QObject::connect(&probe.deadline, &QTimer::timeout, &loop,
                       [&, index] { complete(index, std::nullopt); });
      QObject::connect(&probe.socket, &QLocalSocket::connected, &loop,
                       [&, index] {
                         auto &socket = probes[index]->socket;
                         if (socket.write(wire) != wire.size())
                           complete(index, std::nullopt);
                       });
      QObject::connect(
          &probe.socket, &QLocalSocket::readyRead, &loop, [&, index] {
            auto &probe = *probes[index];
            probe.buffer += probe.socket.readAll();
            if (probe.buffer.size() >
                ::neverd::limits::gui::kInstanceServerMaxReplyBytes)
              return complete(index, std::nullopt);
            const auto newline = probe.buffer.indexOf('\n');
            if (newline < 0)
              return;
            complete(index, parseReply(probe.buffer.left(newline)));
          });
      QObject::connect(&probe.socket, &QLocalSocket::errorOccurred, &loop,
                       [&, index] { complete(index, std::nullopt); });
      QObject::connect(&probe.socket, &QLocalSocket::disconnected, &loop,
                       [&, index] { complete(index, std::nullopt); });
      probe.deadline.start(probeTimeoutMs);
      probe.socket.connectToServer(servers[index]);
    }
  };
  deadline.start(timeoutMs);
  pump();
  if (!stopped && finished < servers.size())
    loop.exec(QEventLoop::ExcludeUserInputEvents);
  return replies;
}

} // namespace neverd::gui
