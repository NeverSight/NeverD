//===- ConversationInput.h - Captured user input ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_PROJECT_CONVERSATIONINPUT_H
#define NEVERD_TOOLS_AGENT_PROJECT_CONVERSATIONINPUT_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <optional>

namespace neverd::agent {
struct ConversationInput {
  QString Id, Text, ObjectId, CreatedAt;
  QJsonArray Context;
  static ConversationInput capture(QString text, QString objectId,
                                   QJsonArray context = {});
  static std::optional<ConversationInput> fromJson(const QJsonObject &value,
                                                   QString &error);
  QJsonObject toJson() const;
  QJsonObject message(const QString &status = "saved") const;
  QString prompt() const;
};
} // namespace neverd::agent
#endif
