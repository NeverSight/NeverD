//===- AgentProtocol.h - Local Agent CLI protocol ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Versioned, newline-delimited JSON shared by the CLI and its frontends.

#ifndef NEVERD_TOOLS_AGENT_CLIENT_AGENTPROTOCOL_H
#define NEVERD_TOOLS_AGENT_CLIENT_AGENTPROTOCOL_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>

namespace neverd::agent::wire {
inline constexpr int kVersion = 1;
inline constexpr qsizetype kMaxFrameBytes = 16 * 1024 * 1024;

inline QByteArray encode(const QJsonObject &message) {
  auto frame = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
  return frame.size() <= kMaxFrameBytes ? frame : QByteArray{};
}

inline QJsonDocument decode(const QByteArray &frame) {
  if (frame.size() > kMaxFrameBytes || !frame.endsWith('\n'))
    return {};
  return QJsonDocument::fromJson(frame);
}

inline QJsonObject initialize(const QString &mode) {
  return {{"command", "initialize"}, {"protocol", kVersion}, {"mode", mode}};
}

inline QJsonObject invoke(qint64 id, const QString &method,
                          const QJsonArray &args) {
  return {{"command", "invoke"},
          {"id", id},
          {"method", method},
          {"arguments", args}};
}

inline QJsonObject changedTail(const QJsonArray &previous,
                               const QJsonArray &current) {
  qsizetype first = 0;
  while (first < previous.size() && first < current.size() &&
         previous.at(first) == current.at(first))
    ++first;
  if (first == previous.size() && first == current.size())
    return {};
  QJsonArray tail;
  for (qsizetype index = first; index < current.size(); ++index)
    tail.append(current.at(index));
  return {{"from", first}, {"entries", tail}};
}

inline bool replaceTail(QJsonArray &current, const QJsonObject &change) {
  const auto from = change.value("from").toInteger(-1);
  if (from < 0 || from > current.size() || !change.value("entries").isArray())
    return false;
  while (current.size() > from)
    current.removeLast();
  for (const auto &entry : change.value("entries").toArray())
    current.append(entry);
  return true;
}

template <typename... Args> QJsonArray arguments(const Args &...args) {
  return {QJsonValue::fromVariant(QVariant::fromValue(args))...};
}
} // namespace neverd::agent::wire

#endif // NEVERD_TOOLS_AGENT_CLIENT_AGENTPROTOCOL_H
