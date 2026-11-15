//===- SideChat.h - Independent project side conversation -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A separate transcript and analysis context that can run beside the main
/// chat.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_APP_SIDECHAT_H
#define NEVERD_TOOLS_AGENT_APP_SIDECHAT_H
#include "project/ConversationInput.h"
#include "runtime/ResearchRuntime.h"

#include <QTimer>

namespace neverd::agent {
class SideChat final : public QObject {
  Q_OBJECT
public:
  SideChat(Project project, QString id, QObject *parent = nullptr);
  bool load(QString &error);
  bool inherit(const QJsonObject &conversation, const QJsonObject &context,
               QString &error);
  bool flush(QString &error);
  void updateProject(const Project &project) { ProjectValue = project; }
  bool send(const ModelProfile &profile, QString key, QString mcp,
            QJsonArray sharedMemory, QString &error,
            const QString &mode = "queue");
  bool startNext(const ModelProfile &profile, QString key, QString mcp,
                 QJsonArray sharedMemory, QString &error);
  QString projectId() const { return ProjectValue.Id; }
  QVariantList pendingInputs() const {
    return Conversation["pendingInputs"].toArray().toVariantList();
  }
  bool queuePaused() const { return Conversation["queuePaused"].toBool(); }
  QVariantList draftContext() const {
    return Conversation["context"].toArray().toVariantList();
  }
  bool setContext(const QJsonArray &context, QString &error);
  QString id() const { return Id; }
  QString title() const { return Title; }
  QString draft() const { return Conversation["draft"].toString(); }
  QVariantList messages() const;
  QVariantList inheritedMessages() const;
  QString forkedAt() const { return Conversation["forkedAt"].toString(); }
  bool running() const { return Runtime.running(); }
  QString runId() const { return Runtime.runId(); }
  QVariantList execution() const { return Runtime.execution(); }
  QVariantMap contextUsage() const;
  QString WorkerPath;
  RequestObjectImport ImportObject;
  bool UseMemory = true, GenerateMemory = false;
  void retireEngines() { Runtime.retireEngines(); }
  bool reviewPending() const { return Runtime.reviewPending(); }
  void setDraft(const QString &text);
  void cancel();
  void resumeQueue();
  void discardQueuedInput(const QString &id);
  QString answer(const QString &messageId) const;
signals:
  void messagesChanged();
  void draftChanged();
  void titleChanged();
  void runChanged();
  void operationError(const QString &error);
  void finished();
  void nextInputRequested();
  void artifactsChanged();
  void memorySource(const QJsonObject &source);

private:
  void updateAnswer(const QString &text, const QString &status, bool append);
  Project ProjectValue;
  QString Id, Title, MessageId;
  QJsonObject Conversation;
  ResearchRuntime Runtime;
  QTimer SaveTimer, StreamTimer;
  bool Dirty = false;
};
} // namespace neverd::agent
#endif // NEVERD_TOOLS_AGENT_APP_SIDECHAT_H
