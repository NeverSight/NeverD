//===- ModelClient.h - Asynchronous model requests --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One cancellable HTTP request; no retries or credential-bearing redirects.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_MODELS_MODELCLIENT_H
#define NEVERD_TOOLS_AGENT_MODELS_MODELCLIENT_H
#include "ProviderCodec.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <memory>

namespace neverd::agent {
class ModelClient final : public QObject {
  Q_OBJECT
public:
  explicit ModelClient(QObject *parent = nullptr);
  void start(const ModelProfile &profile, const QString &key,
             const QString &system, const QJsonArray &history,
             const QJsonArray &tools);
  void cancel();
  bool running() const { return !Reply.isNull(); }
signals:
  void textDelta(const QString &text);
  void finished(const neverd::agent::ModelTurn &turn);

private:
  void read();
  QNetworkAccessManager Network;
  QPointer<QNetworkReply> Reply;
  QTimer Timer;
  std::unique_ptr<ProviderCodec> Codec;
  QByteArray Body;
  QString Key, Failure;
  qint64 Received = 0;
  bool Streaming = false, Cancelled = false;
};
} // namespace neverd::agent
#endif // NEVERD_TOOLS_AGENT_MODELS_MODELCLIENT_H
