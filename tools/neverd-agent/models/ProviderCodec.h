//===- ProviderCodec.h - Model wire protocols -------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Provider-native history and complete tool calls, shared by GUI and runners.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_MODELS_PROVIDERCODEC_H
#define NEVERD_TOOLS_AGENT_MODELS_PROVIDERCODEC_H

#include "ModelProfile.h"

#include <QJsonArray>
#include <QList>
#include <QMap>
#include <functional>

namespace neverd::agent {
struct ModelToolCall {
  QString Id, Name;
  QJsonObject Arguments;
};
struct ModelTurn {
  QString Text, Error;
  QJsonArray Output;
  QList<ModelToolCall> Calls;
  QJsonObject Usage;
  bool Cancelled = false;
};

class ProviderCodec {
public:
  explicit ProviderCodec(ModelProtocol protocol);
  static QJsonObject request(const ModelProfile &profile, const QString &system,
                             const QJsonArray &history,
                             const QJsonArray &tools);
  static QJsonObject message(const QString &role, const QString &text);
  static void toolResults(ModelProtocol protocol, QJsonArray &history,
                          const QList<ModelToolCall> &calls,
                          const QList<QJsonObject> &results);
  bool feed(const QByteArray &bytes, const std::function<void(QString)> &delta);
  ModelTurn finish(bool streaming, const QByteArray &body);
  QString error() const { return Error; }

private:
  bool event(const QByteArray &data, const std::function<void(QString)> &delta);
  ModelTurn decode(const QJsonObject &object);
  ModelProtocol Protocol;
  QByteArray Buffer, EventData;
  QString Error, StopReason;
  QJsonObject Final, Usage;
  QString Text, Reasoning;
  QMap<int, QJsonObject> Blocks;
  QMap<int, QString> Arguments;
  bool Completed = false;
};
} // namespace neverd::agent
#endif // NEVERD_TOOLS_AGENT_MODELS_PROVIDERCODEC_H
