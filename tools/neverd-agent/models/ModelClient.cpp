//===- ModelClient.cpp - Asynchronous model requests ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Network I/O stays on Qt's event loop; authentication never enters run logs.
//===----------------------------------------------------------------------===//
#include "ModelClient.h"

#include <QJsonDocument>
#include <QNetworkRequest>

namespace neverd::agent {
ModelClient::ModelClient(QObject *parent) : QObject(parent) {
  Timer.setSingleShot(true);
  // The timeout measures silence, not the whole answer: every chunk of a
  // streamed reply starts it again, so long answers and reasoning finish.
  connect(&Timer, &QTimer::timeout, this, [this] {
    Failure = tr("The model sent nothing for %1 seconds.")
                  .arg(Timer.interval() / 1000);
    if (Reply)
      Reply->abort();
  });
}
void ModelClient::start(const ModelProfile &profile, const QString &key,
                        const QString &system, const QJsonArray &history,
                        const QJsonArray &tools) {
  Q_ASSERT(!Reply);
  Key = key;
  Body.clear();
  Failure.clear();
  Received = 0;
  Cancelled = false;
  Codec = std::make_unique<ProviderCodec>(profile.Protocol);
  QNetworkRequest request(profile.endpoint());
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::ManualRedirectPolicy);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  request.setRawHeader("Accept", "text/event-stream, application/json");
  if (!key.isEmpty())
    request.setRawHeader(
        profile.Protocol == ModelProtocol::Messages ? "x-api-key"
                                                    : "Authorization",
        (profile.Protocol == ModelProtocol::Messages ? key : "Bearer " + key)
            .toUtf8());
  if (profile.Protocol == ModelProtocol::Messages)
    request.setRawHeader("anthropic-version",
                         profile.AnthropicVersion.toUtf8());
  Reply = Network.post(
      request,
      QJsonDocument(ProviderCodec::request(profile, system, history, tools))
          .toJson(QJsonDocument::Compact));
  connect(Reply, &QNetworkReply::readyRead, this, &ModelClient::read);
  connect(Reply, &QNetworkReply::finished, this, [this] {
    Timer.stop();
    read();
    ModelTurn turn;
    const int status =
        Reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (Cancelled) {
      turn.Error = tr("Analysis cancelled.");
      turn.Cancelled = true;
    } else if (!Failure.isEmpty()) {
      if (!Codec->error().isEmpty())
        turn = Codec->finish(Streaming, Body);
      else
        turn.Error = Failure;
    } else if (status < 200 || status >= 300) {
      const auto error =
          QJsonDocument::fromJson(Body).object()["error"].toObject();
      turn.Error = QString("HTTP %1: %2")
                       .arg(status)
                       .arg(error["message"].toString(Reply->errorString()));
    } else if (Reply->error() != QNetworkReply::NoError)
      turn.Error = Reply->errorString();
    else
      turn = Codec->finish(Streaming, Body);
    if (!Key.isEmpty())
      turn.Error.replace(Key, "[redacted]");
    Key.clear();
    Reply->deleteLater();
    Reply.clear();
    emit finished(turn);
  });
  Timer.start(profile.TimeoutSeconds * 1000);
}
void ModelClient::read() {
  if (!Reply)
    return;
  const auto bytes = Reply->isOpen() ? Reply->readAll() : QByteArray{};
  if (!bytes.isEmpty() && Timer.isActive())
    Timer.start();
  Received += bytes.size();
  if (Received > model_limits::kMaxResponseBytes) {
    Failure = tr("Model response exceeds the configured size limit.");
    if (Reply->isRunning())
      Reply->abort();
    return;
  }
  Streaming = Reply->header(QNetworkRequest::ContentTypeHeader)
                  .toString()
                  .startsWith("text/event-stream");
  const int status =
      Reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (Streaming && status >= 200 && status < 300) {
    if (!Codec->feed(bytes,
                     [this](const QString &text) { emit textDelta(text); })) {
      Failure = Codec->error();
      if (Reply->isRunning())
        Reply->abort();
    }
  } else
    Body += bytes;
}
void ModelClient::cancel() {
  Cancelled = true;
  if (Reply)
    Reply->abort();
}
} // namespace neverd::agent
