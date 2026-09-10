#include "McpConnectionManager.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QTimer>

namespace {
constexpr qsizetype MaxMessage = 8 * 1024 * 1024;
constexpr auto Protocol = "2025-11-25";
} // namespace

McpConnectionManager::McpConnectionManager(QObject *parent) : QObject(parent) {
  setStatus(QT_TR_NOOP("Disconnected"));
  connect(&process_, &QProcess::started, this,
          &McpConnectionManager::initialize);
  connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
    input_ += process_.readAllStandardOutput();
    if (input_.size() > MaxMessage) {
      failSource(QT_TR_NOOP("MCP message exceeds 8 MiB"));
      return;
    }
    while (true) {
      const auto end = input_.indexOf('\n');
      if (end < 0)
        break;
      const auto line = input_.left(end);
      input_.remove(0, end + 1);
      QJsonParseError error;
      const auto document = QJsonDocument::fromJson(line, &error);
      if (error.error != QJsonParseError::NoError || !document.isObject()) {
        failSource(QT_TR_NOOP("Invalid MCP JSON-RPC message"));
        return;
      }
      consume(document.object());
    }
  });
  connect(&process_, &QProcess::readyReadStandardError, this, [this] {
    diagnostics_ += process_.readAllStandardError();
    if (diagnostics_.size() > 65536)
      diagnostics_ = diagnostics_.right(65536);
  });
  connect(&process_, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError) {
            if (!stopping_)
              failSource(QT_TR_NOOP("MCP process error: %1"),
                         {process_.errorString()});
          });
  connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this, [this](int code, QProcess::ExitStatus) {
            if (!stopping_)
              failSource(
                  QT_TR_NOOP("MCP process exited (%1): %2"),
                  {QString::number(code), QString::fromUtf8(diagnostics_)});
          });
  auto timer = new QTimer(this);
  timer->setInterval(1000);
  connect(timer, &QTimer::timeout, this, [this] {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto ids = pending_.keys();
    for (const auto &id : ids) {
      if (now - pending_.value(id).started < 120000)
        continue;
      const auto method = pending_.take(id).method;
      finishCall(id, "timed_out");
      if (ready_)
        send("notifications/cancelled",
             {{"requestId", id}, {"reason", "Client timeout"}}, true);
      closeReplies(id);
      setStatus(QT_TR_NOOP("MCP request timed out: %1"), {method});
      if (method == "initialize") {
        fail(status());
        return;
      }
      emit changed();
    }
  });
  timer->start();
}

McpConnectionManager::~McpConnectionManager() {
  disconnectServer();
  if (process_.state() != QProcess::NotRunning) {
    process_.kill();
    process_.waitForFinished(1000);
  }
}

void McpConnectionManager::connectStdio(const QString &program,
                                        const QStringList &arguments) {
  if (process_.state() != QProcess::NotRunning) {
    setStatus(
        QT_TR_NOOP("Disconnect the running MCP process before reconnecting"));
    emit changed();
    return;
  }
  disconnectServer();
  const QFileInfo executable(program);
  if (!executable.isAbsolute() || !executable.isFile() ||
      !executable.isExecutable()) {
    failSource(QT_TR_NOOP("Choose an absolute MCP executable path"));
    return;
  }
  stopping_ = false;
  setStatus(QT_TR_NOOP("Connecting"));
  process_.setProgram(program);
  process_.setArguments(arguments);
  process_.setProcessChannelMode(QProcess::SeparateChannels);
  process_.start();
  emit changed();
}

void McpConnectionManager::connectHttp(const QString &url,
                                       const QString &bearerToken,
                                       const QString &caCertificateFile) {
  if (process_.state() != QProcess::NotRunning) {
    setStatus(
        QT_TR_NOOP("Disconnect the running MCP process before reconnecting"));
    emit changed();
    return;
  }
  disconnectServer();
  const QUrl endpoint(url);
  const bool loopback = endpoint.host() == "localhost" ||
                        endpoint.host() == "127.0.0.1" ||
                        endpoint.host() == "::1";
  if (!endpoint.isValid() || endpoint.host().isEmpty() ||
      !endpoint.userInfo().isEmpty() || endpoint.hasFragment() ||
      (endpoint.scheme() != "https" &&
       !(endpoint.scheme() == "http" && loopback))) {
    failSource(QT_TR_NOOP("MCP HTTP requires HTTPS, or HTTP on localhost"));
    return;
  }
  if (bearerToken.contains('\r') || bearerToken.contains('\n')) {
    failSource(QT_TR_NOOP("Invalid authentication token"));
    return;
  }
  tls_ = QSslConfiguration::defaultConfiguration();
  if (!caCertificateFile.isEmpty()) {
    const auto certificates = QSslCertificate::fromPath(caCertificateFile);
    if (certificates.isEmpty()) {
      failSource(QT_TR_NOOP("Cannot load CA certificate"));
      return;
    }
    auto authorities = tls_->caCertificates();
    authorities.append(certificates);
    tls_->setCaCertificates(authorities);
  }
  endpoint_ = endpoint;
  token_ = bearerToken.toUtf8();
  stopping_ = false;
  setStatus(QT_TR_NOOP("Connecting"));
  initialize();
  emit changed();
}

void McpConnectionManager::disconnectServer() {
  stopping_ = true;
  ++generation_;
  const auto activeReplies = replies_.keys();
  replies_.clear();
  for (auto *reply : activeReplies) {
    reply->abort();
    reply->deleteLater();
  }
  if (!endpoint_.isEmpty() && !session_.isEmpty()) {
    auto *reply = network_.deleteResource(request());
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
  }
  if (process_.state() != QProcess::NotRunning) {
    process_.closeWriteChannel();
    process_.terminate();
    const auto generation = generation_;
    QTimer::singleShot(1000, this, [this, generation] {
      if (generation == generation_ && process_.state() != QProcess::NotRunning)
        process_.kill();
    });
  }
  endpoint_.clear();
  token_.clear();
  session_.clear();
  input_.clear();
  diagnostics_.clear();
  for (const auto &id : pending_.keys())
    finishCall(id, "disconnected");
  pending_.clear();
  tools_.clear();
  resources_.clear();
  ready_ = false;
  setStatus(QT_TR_NOOP("Disconnected"));
  emit changed();
}

void McpConnectionManager::fail(const QString &message) {
  disconnectServer();
  statusSource_.clear();
  statusArguments_.clear();
  status_ = message;
  emit changed();
}

QString McpConnectionManager::status() const {
  if (statusSource_.isEmpty())
    return status_;
  auto result = tr(statusSource_.constData());
  for (const auto &argument : statusArguments_)
    result = result.arg(argument);
  return result;
}

void McpConnectionManager::setStatus(const char *source,
                                     const QStringList &arguments) {
  statusSource_ = source;
  statusArguments_ = arguments;
  status_.clear();
}

void McpConnectionManager::failSource(const char *source,
                                      const QStringList &arguments) {
  disconnectServer();
  setStatus(source, arguments);
  emit changed();
}

void McpConnectionManager::initialize() {
  send("initialize", {{"protocolVersion", Protocol},
                      {"capabilities", QJsonObject{}},
                      {"clientInfo", QJsonObject{{"name", "neverd-gui"},
                                                 {"version", "0.1.0"}}}});
}

QString McpConnectionManager::send(const QString &method,
                                   const QJsonObject &params,
                                   bool notification) {
  if (pending_.size() >= 32 && !notification) {
    setStatus(QT_TR_NOOP("MCP request queue is full"));
    emit changed();
    return {};
  }
  const auto id = QString::number(++serial_);
  QJsonObject message{
      {"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
  if (!notification) {
    message.insert("id", id);
    pending_.insert(id, {method, QDateTime::currentMSecsSinceEpoch()});
  }
  if (!endpoint_.isEmpty())
    post(message);
  else if (process_.state() == QProcess::Running) {
    auto body = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (body.size() > 1024 * 1024 ||
        process_.bytesToWrite() > 2 * 1024 * 1024) {
      failSource(QT_TR_NOOP("MCP outgoing message budget exceeded"));
      return {};
    }
    process_.write(body);
  } else {
    pending_.remove(id);
    setStatus(QT_TR_NOOP("MCP server is disconnected"));
    emit changed();
    return {};
  }
  return id;
}

QNetworkRequest McpConnectionManager::request() const {
  QNetworkRequest request(endpoint_);
  request.setRawHeader("Content-Type", "application/json");
  request.setRawHeader("Accept", "application/json, text/event-stream");
  request.setRawHeader("MCP-Protocol-Version", Protocol);
  if (!session_.isEmpty())
    request.setRawHeader("MCP-Session-Id", session_);
  if (!token_.isEmpty())
    request.setRawHeader("Authorization", "Bearer " + token_);
  if (tls_)
    request.setSslConfiguration(*tls_);
  request.setTransferTimeout(120000);
  // Never forward credentials to an endpoint selected by an HTTP redirect.
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  return request;
}

void McpConnectionManager::post(const QJsonObject &message) {
  const auto body = QJsonDocument(message).toJson(QJsonDocument::Compact);
  if (body.size() > 1024 * 1024) {
    failSource(QT_TR_NOOP("MCP outgoing message budget exceeded"));
    return;
  }
  auto *reply = network_.post(request(), body);
  HttpStream stream;
  stream.requestId = message.value("id").toString();
  watchReply(reply, stream);
}

void McpConnectionManager::watchReply(QNetworkReply *reply, HttpStream stream) {
  reply->setReadBufferSize(MaxMessage + 1);
  replies_.insert(reply, QSharedPointer<HttpStream>::create(stream));
  connect(reply, &QNetworkReply::readyRead, this,
          [this, reply] { readHttp(reply); });
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    if (!replies_.contains(reply)) {
      reply->deleteLater();
      return;
    }
    readHttp(reply);
    if (!replies_.contains(reply)) {
      reply->deleteLater();
      return;
    }
    auto stream = *replies_.take(reply);
    const auto httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto error = reply->error();
    const auto message = reply->errorString();
    const auto contentType =
        reply->header(QNetworkRequest::ContentTypeHeader).toString();
    reply->deleteLater();
    if (stopping_)
      return;
    if (httpStatus == 404 && !session_.isEmpty()) {
      for (const auto &id : pending_.keys())
        finishCall(id, "disconnected");
      pending_.clear();
      session_.clear();
      ready_ = false;
      tools_.clear();
      resources_.clear();
      setStatus(QT_TR_NOOP("MCP session expired; reconnecting"));
      initialize();
      emit changed();
      return;
    }
    if (error != QNetworkReply::NoError || httpStatus < 200 ||
        httpStatus >= 300) {
      pending_.remove(stream.requestId);
      finishCall(stream.requestId, "failed", message);
      setStatus(QT_TR_NOOP("MCP HTTP error %1: %2"),
                {QString::number(httpStatus), message});
      if (!ready_)
        fail(status());
      else
        emit changed();
      return;
    }
    if (contentType.startsWith("text/event-stream")) {
      if (!stream.completed && !stream.requestId.isEmpty() &&
          pending_.contains(stream.requestId)) {
        if (stream.eventId.isEmpty() || ++stream.reconnects > 3) {
          pending_.remove(stream.requestId);
          finishCall(stream.requestId, "failed");
          setStatus(QT_TR_NOOP("MCP event stream ended before its response"));
          emit changed();
          return;
        }
        stream.buffer.clear();
        stream.eventData.clear();
        stream.pendingEventId.clear();
        stream.hasPendingEventId = false;
        const auto generation = generation_;
        QTimer::singleShot(stream.retryMs, this, [this, stream, generation] {
          if (generation != generation_ || !pending_.contains(stream.requestId))
            return;
          auto next = request();
          next.setRawHeader("Accept", "text/event-stream");
          next.setRawHeader("Last-Event-ID", stream.eventId);
          watchReply(network_.get(next), stream);
        });
      }
    } else if (httpStatus != 202) {
      QJsonParseError parseError;
      const auto document = QJsonDocument::fromJson(stream.buffer, &parseError);
      if (!contentType.startsWith("application/json") ||
          parseError.error != QJsonParseError::NoError ||
          !document.isObject()) {
        failSource(QT_TR_NOOP("Invalid MCP HTTP response"));
        return;
      }
      consume(document.object());
    }
  });
}

void McpConnectionManager::readHttp(QNetworkReply *reply) {
  auto it = replies_.find(reply);
  if (it == replies_.end())
    return;
  const auto streamOwner = it.value();
  auto &stream = *streamOwner;
  if (pending_.value(stream.requestId).method == "initialize") {
    const auto session = reply->rawHeader("MCP-Session-Id");
    if (!session.isEmpty()) {
      for (const auto character : session) {
        if (character < 0x21 || character > 0x7e) {
          failSource(QT_TR_NOOP("Invalid MCP session identifier"));
          return;
        }
      }
      if (session.size() > 4096) {
        failSource(QT_TR_NOOP("MCP session identifier is too large"));
        return;
      }
      session_ = session;
    }
  }
  stream.buffer += reply->readAll();
  if (stream.buffer.size() + stream.eventData.size() > MaxMessage) {
    failSource(QT_TR_NOOP("MCP message exceeds 8 MiB"));
    return;
  }
  if (!reply->header(QNetworkRequest::ContentTypeHeader)
           .toString()
           .startsWith("text/event-stream"))
    return;
  while (true) {
    const auto newline = stream.buffer.indexOf('\n');
    if (newline < 0)
      return;
    auto line = stream.buffer.left(newline);
    stream.buffer.remove(0, newline + 1);
    if (line.endsWith('\r'))
      line.chop(1);
    if (line.isEmpty()) {
      if (stream.hasPendingEventId) {
        stream.eventId = stream.pendingEventId;
        stream.hasPendingEventId = false;
      }
      if (!stream.eventData.isEmpty()) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(stream.eventData, &error);
        stream.eventData.clear();
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
          failSource(QT_TR_NOOP("Invalid MCP event data"));
          return;
        }
        const auto object = document.object();
        if (object.value("id").toString() == stream.requestId &&
            (object.contains("result") || object.contains("error")))
          stream.completed = true;
        consume(object);
        if (!replies_.contains(reply))
          return;
      }
    } else if (line.startsWith("data:")) {
      auto data = line.mid(5);
      if (data.startsWith(' '))
        data.remove(0, 1);
      if (!stream.eventData.isEmpty())
        stream.eventData += '\n';
      stream.eventData += data;
    } else if (line.startsWith("id:")) {
      auto id = line.mid(3);
      if (id.startsWith(' '))
        id.remove(0, 1);
      if (id.size() > 4096 || id.contains('\r')) {
        failSource(QT_TR_NOOP("Invalid MCP event data"));
        return;
      }
      if (!id.contains('\0')) {
        stream.pendingEventId = id;
        stream.hasPendingEventId = true;
      }
    } else if (line.startsWith("retry:")) {
      bool valid = false;
      const auto delay = line.mid(6).trimmed().toInt(&valid);
      if (valid && delay >= 0)
        stream.retryMs = qBound(100, delay, 60000);
    }
  }
}

void McpConnectionManager::consume(const QJsonObject &message) {
  if (message.value("jsonrpc") != "2.0") {
    failSource(QT_TR_NOOP("Invalid MCP protocol version"));
    return;
  }
  if (message.contains("method")) {
    if (message.contains("id")) {
      QJsonObject response{{"jsonrpc", "2.0"}, {"id", message.value("id")}};
      if (message.value("method") == "ping")
        response.insert("result", QJsonObject{});
      else
        response.insert(
            "error",
            QJsonObject{{"code", -32601},
                        {"message", "Client capability not supported"}});
      if (!endpoint_.isEmpty())
        post(response);
      else
        process_.write(QJsonDocument(response).toJson(QJsonDocument::Compact) +
                       '\n');
    }
    return;
  }
  const auto id = message.value("id").toString();
  if (!pending_.contains(id))
    return;
  const auto method = pending_.take(id).method;
  if (message.contains("error")) {
    lastResult_ =
        QString::fromUtf8(QJsonDocument(message.value("error").toObject())
                              .toJson(QJsonDocument::Indented));
    finishCall(id, "failed", lastResult_);
    setStatus(QT_TR_NOOP("MCP request failed: %1"), {method});
    if (method == "initialize") {
      fail(status() + '\n' + lastResult_);
      return;
    }
  } else if (!message.value("result").isObject()) {
    failSource(QT_TR_NOOP("MCP response has no result object"));
    return;
  } else {
    const auto result = message.value("result").toObject();
    if (method == "initialize") {
      if (result.value("protocolVersion").toString() != Protocol) {
        failSource(QT_TR_NOOP("Unsupported MCP protocol version"));
        return;
      }
      ready_ = true;
      send("notifications/initialized", {}, true);
      const auto capabilities = result.value("capabilities").toObject();
      if (capabilities.contains("tools"))
        listTools();
      if (capabilities.contains("resources"))
        listResources();
      setStatus(QT_TR_NOOP("Connected"));
    } else if (method == "tools/list") {
      tools_ = result.value("tools").toArray().toVariantList();
      if (result.contains("nextCursor"))
        setStatus(QT_TR_NOOP("Connected; tool list is partial"));
    } else if (method == "resources/list") {
      resources_ = result.value("resources").toArray().toVariantList();
      if (result.contains("nextCursor"))
        setStatus(QT_TR_NOOP("Connected; resource list is partial"));
    } else {
      lastResult_ = QString::fromUtf8(
          QJsonDocument(result).toJson(QJsonDocument::Indented));
      finishCall(id, result.value("isError").toBool() ? "failed" : "completed",
                 lastResult_);
      setStatus(result.value("isError").toBool()
                    ? QT_TR_NOOP("MCP tool reported an error")
                    : QT_TR_NOOP("Connected"));
    }
  }
  emit changed();
}

void McpConnectionManager::listTools() {
  if (ready_)
    send("tools/list");
}
void McpConnectionManager::listResources() {
  if (ready_)
    send("resources/list");
}

void McpConnectionManager::callTool(const QString &name,
                                    const QString &jsonArguments) {
  if (!ready_) {
    setStatus(QT_TR_NOOP("Connect an MCP server first"));
    emit changed();
    return;
  }
  if (jsonArguments.size() > 1024 * 1024) {
    setStatus(QT_TR_NOOP("Tool arguments exceed the size limit"));
    emit changed();
    return;
  }
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(jsonArguments.toUtf8(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    setStatus(QT_TR_NOOP("Tool arguments must be a JSON object"));
    emit changed();
    return;
  }
  const auto id =
      send("tools/call", {{"name", name}, {"arguments", document.object()}});
  if (!id.isEmpty())
    recordCall(id, "tools/call", name, jsonArguments);
}

void McpConnectionManager::readResource(const QString &uri) {
  if (ready_) {
    const auto id = send("resources/read", {{"uri", uri}});
    if (!id.isEmpty())
      recordCall(id, "resources/read", uri, QString());
  }
}

void McpConnectionManager::recordCall(const QString &id, const QString &method,
                                      const QString &name,
                                      const QString &arguments) {
  history_.prepend(QVariantMap{{"id", id},
                               {"connection", QString::number(generation_)},
                               {"method", method},
                               {"name", name.left(256)},
                               {"arguments", arguments.left(4096)},
                               {"status", "pending"},
                               {"started", QDateTime::currentMSecsSinceEpoch()},
                               {"result", QString()},
                               {"truncated", arguments.size() > 4096}});
  while (history_.size() > 50) {
    // Keep every active call reachable for cancellation; there are at most 32.
    for (qsizetype index = history_.size() - 1; index >= 0; --index) {
      if (history_[index].toMap().value("status").toString() != "pending") {
        history_.removeAt(index);
        break;
      }
    }
  }
  emit changed();
}

void McpConnectionManager::finishCall(const QString &id, const QString &state,
                                      const QString &result) {
  for (auto &item : history_) {
    auto entry = item.toMap();
    if (entry.value("id").toString() != id)
      continue;
    entry.insert("status", state);
    entry.insert("result", result.left(4096));
    entry.insert("truncated",
                 entry.value("truncated").toBool() || result.size() > 4096);
    entry.insert("durationMs", QDateTime::currentMSecsSinceEpoch() -
                                   entry.value("started").toLongLong());
    item = entry;
    return;
  }
}

void McpConnectionManager::cancelCall(const QString &id) {
  if (!pending_.contains(id))
    return;
  bool explicitCall = false;
  for (const auto &item : history_)
    if (item.toMap().value("id").toString() == id) {
      explicitCall = true;
      break;
    }
  if (!explicitCall)
    return;
  pending_.remove(id);
  send("notifications/cancelled",
       {{"requestId", id}, {"reason", "Cancelled by user"}}, true);
  // The notification requests cancellation; it does not assert that a remote
  // synchronous computation has stopped. Late responses are ignored by id.
  closeReplies(id);
  finishCall(id, "cancelled");
  emit changed();
}

void McpConnectionManager::closeReplies(const QString &id) {
  for (auto *reply : replies_.keys()) {
    if (replies_.value(reply)->requestId != id)
      continue;
    replies_.remove(reply);
    reply->abort();
    reply->deleteLater();
  }
}

void McpConnectionManager::inspectCall(const QString &id) {
  for (const auto &item : history_) {
    const auto entry = item.toMap();
    if (entry.value("id").toString() != id)
      continue;
    lastResult_ =
        QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(entry))
                              .toJson(QJsonDocument::Indented));
    emit changed();
    return;
  }
}
