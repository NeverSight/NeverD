#include "GuiSessionBroker.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QtEndian>

namespace {
constexpr quint32 MaxFrame = 8 * 1024 * 1024;
const QSet<QString> ReadOperations{"metadata", "functions", "disasm",
                                   "bytes",    "decompile", "cfg",
                                   "xrefs",    "strings"};
} // namespace

GuiSessionBroker::GuiSessionBroker(QObject *parent) : QObject(parent) {
  setStatus(QT_TR_NOOP("Session sharing disabled"));
  connect(&server_, &QLocalServer::newConnection, this,
          &GuiSessionBroker::acceptClients);
  auto *timer = new QTimer(this);
  timer->setInterval(1000);
  connect(timer, &QTimer::timeout, this, [this] {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto &id : routes_.keys()) {
      const auto route = routes_.value(id);
      if (now - route.started < 120000)
        continue;
      routes_.remove(id);
      if (route.socket && clients_.contains(route.socket)) {
        --clients_[route.socket].pending;
        error(route.socket, route.clientId, "timeout",
              "GUI session query timed out");
      }
    }
  });
  timer->start();
}

GuiSessionBroker::~GuiSessionBroker() { stop(); }

QString GuiSessionBroker::status() const {
  auto result = tr(statusSource_.constData());
  if (!statusDetail_.isEmpty())
    result = result.arg(statusDetail_);
  return result;
}

void GuiSessionBroker::setStatus(const char *source, const QString &detail) {
  statusSource_ = source;
  statusDetail_ = detail;
}

bool GuiSessionBroker::start() {
  if (enabled())
    return true;
  directory_ = std::make_unique<QTemporaryDir>(QDir::tempPath() +
                                               "/neverd-session-XXXXXX");
  if (!directory_->isValid()) {
    setStatus(QT_TR_NOOP("Cannot create private session directory"));
    emit changed();
    return false;
  }
  QFile::setPermissions(directory_->path(),
                        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
  QByteArray tokenBytes(32, '\0');
  for (int index = 0; index < tokenBytes.size(); index += 4) {
    const auto random = QRandomGenerator::system()->generate();
    qToBigEndian(random, reinterpret_cast<uchar *>(tokenBytes.data() + index));
  }
  token_ = QString::fromLatin1(tokenBytes.toHex());
  server_.setSocketOptions(QLocalServer::UserAccessOption);
#ifdef Q_OS_WIN
  const auto endpoint = "neverd-" + token_.left(24);
#else
  const auto endpoint = directory_->filePath("session.sock");
#endif
  if (!server_.listen(endpoint)) {
    setStatus(QT_TR_NOOP("Cannot open local session broker: %1"),
              server_.errorString());
    directory_.reset();
    token_.clear();
    emit changed();
    return false;
  }
  credentialFile_ = directory_->filePath("credentials.json");
  QFile file(credentialFile_);
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
    stop();
    setStatus(QT_TR_NOOP("Cannot write private session credentials"));
    emit changed();
    return false;
  }
  file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
  const auto data =
      QJsonDocument(QJsonObject{{"endpoint", server_.fullServerName()},
                                {"token", token_}})
          .toJson(QJsonDocument::Compact);
  if (file.write(data) != data.size()) {
    file.close();
    stop();
    setStatus(QT_TR_NOOP("Cannot write private session credentials"));
    emit changed();
    return false;
  }
  file.close();
  sharedProject_ = selection_.value("project_id").toString();
  setStatus(QT_TR_NOOP("Session sharing enabled"));
  emit changed();
  return true;
}

void GuiSessionBroker::stop() {
  server_.close();
  const auto sockets = clients_.keys();
  clients_.clear();
  routes_.clear();
  for (auto *socket : sockets) {
    socket->abort();
    socket->deleteLater();
  }
  credentialFile_.clear();
  token_.clear();
  sharedProject_.clear();
  directory_.reset();
  setStatus(QT_TR_NOOP("Session sharing disabled"));
  emit changed();
}

void GuiSessionBroker::setSelection(const QJsonObject &selection) {
  if (enabled() && selection.value("project_id").toString() != sharedProject_)
    stop();
  selection_ = selection;
}

void GuiSessionBroker::acceptClients() {
  while (server_.hasPendingConnections()) {
    auto *socket = server_.nextPendingConnection();
    socket->setReadBufferSize(MaxFrame + 4);
    if (clients_.size() >= 4) {
      socket->abort();
      socket->deleteLater();
      continue;
    }
    clients_.insert(socket, {});
    connect(socket, &QLocalSocket::readyRead, this,
            [this, socket] { readClient(socket); });
    connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
      clients_.remove(socket);
      for (const auto &id : routes_.keys())
        if (routes_.value(id).socket == socket)
          routes_.remove(id);
      socket->deleteLater();
    });
    QPointer<QLocalSocket> guard(socket);
    QTimer::singleShot(5000, this, [this, guard] {
      if (guard && clients_.contains(guard) &&
          !clients_.value(guard).authenticated)
        guard->abort();
    });
  }
}

void GuiSessionBroker::readClient(QLocalSocket *socket) {
  if (!clients_.contains(socket))
    return;
  auto &client = clients_[socket];
  client.buffer += socket->readAll();
  if (client.buffer.size() > MaxFrame + 4) {
    socket->abort();
    return;
  }
  while (client.buffer.size() >= 4) {
    const auto size = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar *>(client.buffer.constData()));
    if (!size || size > MaxFrame) {
      socket->abort();
      return;
    }
    if (client.buffer.size() < qsizetype(size) + 4)
      return;
    const auto body = client.buffer.mid(4, size);
    client.buffer.remove(0, size + 4);
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      socket->abort();
      return;
    }
    handle(socket, document.object());
    if (!clients_.contains(socket))
      return;
  }
}

void GuiSessionBroker::handle(QLocalSocket *socket,
                              const QJsonObject &request) {
  const auto id = request.value("request_id").toString();
  const auto operation = request.value("operation").toString();
  const auto payload = request.value("payload").toObject();
  if (request.value("protocol_major").toInt() != 1 || id.isEmpty() ||
      id.size() > 128 || !request.value("payload").isObject()) {
    socket->abort();
    return;
  }
  if (!clients_[socket].authenticated) {
    // Constant amount of work for same-size tokens; token itself is never
    // logged.
    const auto supplied = payload.value("token").toString().toLatin1();
    const auto expected = token_.toLatin1();
    uchar difference = supplied.size() == expected.size() ? 0 : 1;
    for (qsizetype index = 0; index < expected.size(); ++index)
      difference |= uchar(expected[index]) ^
                    uchar(index < supplied.size() ? supplied[index] : 0);
    if (operation != "authenticate" || difference) {
      socket->abort();
      return;
    }
    clients_[socket].authenticated = true;
    write(socket, {{"type", "hello"},
                   {"protocol_major", 1},
                   {"protocol_minor", 0},
                   {"engine_version", "gui-session"},
                   {"capabilities",
                    QJsonArray{"read_only", "selection", "navigation"}}});
    return;
  }
  const auto revision = request.value("expected_revision").toString();
  if (revision.size() > 128) {
    error(socket, id, "invalid_request", "Revision is too long");
    return;
  }
  if (operation == "selection" || operation == "navigate" ||
      operation == "highlight") {
    const auto currentRevision = selection_.value("revision").toString();
    if (!revision.isEmpty() && revision != currentRevision) {
      error(socket, id, "stale_revision", "GUI revision changed");
      return;
    }
    if (operation != "selection") {
      static const QRegularExpression addressPattern("^0x[0-9a-fA-F]{1,16}$");
      const auto address = payload.value("address").toString();
      if (!addressPattern.match(address).hasMatch()) {
        error(socket, id, "invalid_address",
              "Expected a hexadecimal virtual address");
        return;
      }
      emit navigationRequested(address, operation == "highlight");
    }
    write(socket, {{"type", "response"},
                   {"request_id", id},
                   {"status", "ok"},
                   {"revision", currentRevision},
                   {"payload", selection_}});
    return;
  }
  if (!ReadOperations.contains(operation)) {
    error(socket, id, "unsupported", "Only session read queries are exposed");
    return;
  }
  if (clients_[socket].pending >= 16 || routes_.size() >= 32) {
    error(socket, id, "busy", "GUI session request queue is full");
    return;
  }
  for (const auto &route : routes_)
    if (route.socket == socket && route.clientId == id) {
      error(socket, id, "invalid_request", "Duplicate request id");
      return;
    }
  const auto brokerId = "mcp-" + QString::number(++serial_);
  routes_.insert(brokerId, {socket, id, QDateTime::currentMSecsSinceEpoch()});
  ++clients_[socket].pending;
  emit queryRequested(brokerId, operation, payload, revision);
}

void GuiSessionBroker::reply(const QString &brokerRequestId,
                             const QJsonObject &workerResponse) {
  if (!routes_.contains(brokerRequestId))
    return;
  const auto route = routes_.take(brokerRequestId);
  if (!route.socket || !clients_.contains(route.socket))
    return;
  --clients_[route.socket].pending;
  auto response = workerResponse;
  response.insert("type", "response");
  response.insert("request_id", route.clientId);
  write(route.socket, response);
}

void GuiSessionBroker::write(QLocalSocket *socket, const QJsonObject &message) {
  const auto body = QJsonDocument(message).toJson(QJsonDocument::Compact);
  if (body.size() > MaxFrame || socket->bytesToWrite() > MaxFrame) {
    socket->abort();
    return;
  }
  QByteArray frame(4, '\0');
  qToBigEndian(quint32(body.size()), reinterpret_cast<uchar *>(frame.data()));
  frame += body;
  socket->write(frame);
}

void GuiSessionBroker::error(QLocalSocket *socket, const QString &id,
                             const QString &code, const QString &message) {
  write(socket, {{"type", "response"},
                 {"request_id", id},
                 {"status", "error"},
                 {"revision", selection_.value("revision")},
                 {"error", QJsonObject{{"code", code}, {"message", message}}}});
}
