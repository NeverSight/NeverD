//===- WorkspaceClient.h - QML frontend to the Agent CLI --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Cache workspace state and expose the existing QML controller surface.

#ifndef NEVERD_TOOLS_AGENT_CLIENT_WORKSPACECLIENT_H
#define NEVERD_TOOLS_AGENT_CLIENT_WORKSPACECLIENT_H

#include "AgentProcess.h"
#include "AgentProtocol.h"

#include <QSet>
#include <QTimer>
#include <type_traits>

namespace neverd::agent {
class WorkspaceClient;

class SideChatClient final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString id READ id CONSTANT)
  Q_PROPERTY(QString title READ title NOTIFY titleChanged)
  Q_PROPERTY(QString draft READ draft WRITE setDraft NOTIFY draftChanged)
  Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
  Q_PROPERTY(QVariantList inheritedMessages READ inheritedMessages CONSTANT)
  Q_PROPERTY(QString forkedAt READ forkedAt CONSTANT)
  Q_PROPERTY(QVariantList pendingInputs READ pendingInputs NOTIFY draftChanged)
  Q_PROPERTY(bool queuePaused READ queuePaused NOTIFY draftChanged)
  Q_PROPERTY(QVariantList draftContext READ draftContext NOTIFY draftChanged)
  Q_PROPERTY(bool running READ running NOTIFY runChanged)
  Q_PROPERTY(QVariantMap contextUsage READ contextUsage NOTIFY runChanged)

public:
  SideChatClient(WorkspaceClient &workspace, const QString &id);
  QString id() const { return Id; }
  QString forkedAt() const { return State.value("forkedAt").toString(); }
  QVariantList inheritedMessages() const {
    return State.value("inheritedMessages").toArray().toVariantList();
  }
#define AGENT_SIDE_PROPERTY(Type, Name, Signal)                                \
  Type Name() const { return State.value(#Name).toVariant().value<Type>(); }
#include "WorkspaceProtocol.def"
#undef AGENT_SIDE_PROPERTY
  Q_INVOKABLE void setDraft(const QString &text);
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void resumeQueue();
  Q_INVOKABLE void discardQueuedInput(const QString &id);

signals:
  void draftChanged();
  void messagesChanged();
  void titleChanged();
  void runChanged();

private:
  friend class WorkspaceClient;
  bool update(const QJsonObject &message);
  void notify();

  WorkspaceClient &Workspace;
  QString Id;
  QJsonObject State;
  QSet<QByteArray> Changed;
};

class WorkspaceClient final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList projects READ projects NOTIFY workspaceChanged)
  Q_PROPERTY(
      QVariantMap activeProject READ activeProject NOTIFY workspaceChanged)
  Q_PROPERTY(QString activeId READ activeId NOTIFY workspaceChanged)
  Q_PROPERTY(
      QString selectedObjectId READ selectedObjectId NOTIFY objectChanged)
  Q_PROPERTY(
      QVariantMap selectedObject READ selectedObject NOTIFY objectChanged)
  Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
  Q_PROPERTY(QString draft READ draft NOTIFY conversationChanged)
  Q_PROPERTY(QVariantList files READ files NOTIFY filesChanged)
  Q_PROPERTY(QVariantList memory READ memory NOTIFY objectChanged)
  Q_PROPERTY(QVariantList sharedMemory READ sharedMemory NOTIFY objectChanged)
  Q_PROPERTY(QString dataDirectory READ dataDirectory CONSTANT)
  Q_PROPERTY(QString guiPath READ guiPath NOTIFY settingsChanged)
  Q_PROPERTY(bool sharing READ sharing NOTIFY settingsChanged)
  Q_PROPERTY(bool useMemory READ useMemory NOTIFY settingsChanged)
  Q_PROPERTY(bool generateMemory READ generateMemory NOTIFY settingsChanged)
  Q_PROPERTY(QString memoryStatus READ memoryStatus NOTIFY memoryChanged)
  Q_PROPERTY(bool memoryWorking READ memoryWorking NOTIFY memoryChanged)
  Q_PROPERTY(QVariantList projectMemory READ projectMemory NOTIFY objectChanged)
  Q_PROPERTY(bool lightTheme READ lightTheme NOTIFY settingsChanged)
  Q_PROPERTY(bool autoAddAnalysisObjects READ autoAddAnalysisObjects NOTIFY
                 settingsChanged)
  Q_PROPERTY(QVariantList analysisObjectApprovals READ analysisObjectApprovals
                 NOTIFY analysisObjectApprovalsChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
  Q_PROPERTY(QString previewTitle READ previewTitle NOTIFY previewChanged)
  Q_PROPERTY(QString previewText READ previewText NOTIFY previewChanged)
  Q_PROPERTY(
      QVariantMap modelSettings READ modelSettings NOTIFY settingsChanged)
  Q_PROPERTY(QString mcpPath READ mcpPath NOTIFY settingsChanged)
  Q_PROPERTY(bool modelConfigured READ modelConfigured NOTIFY settingsChanged)
  Q_PROPERTY(bool credentialBusy READ credentialBusy NOTIFY credentialChanged)
  Q_PROPERTY(
      QString credentialStatus READ credentialStatus NOTIFY credentialChanged)
  Q_PROPERTY(bool running READ running NOTIFY runChanged)
  Q_PROPERTY(QVariantList progress READ progress NOTIFY runChanged)
  Q_PROPERTY(QVariantMap contextUsage READ contextUsage NOTIFY runChanged)
  Q_PROPERTY(neverd::agent::SideChatClient *sideChat READ sideChat NOTIFY
                 sideChatChanged)
  Q_PROPERTY(QVariantList sideChats READ sideChats NOTIFY sideChatsChanged)
  Q_PROPERTY(QVariantMap goal READ goal NOTIFY goalChanged)
  Q_PROPERTY(
      QVariantList pendingInputs READ pendingInputs NOTIFY conversationChanged)
  Q_PROPERTY(bool queuePaused READ queuePaused NOTIFY conversationChanged)
  Q_PROPERTY(QVariantMap evidence READ evidence NOTIFY evidenceChanged)
  Q_PROPERTY(bool evidenceBusy READ evidenceBusy NOTIFY evidenceChanged)
  Q_PROPERTY(bool reviewBusy READ reviewBusy NOTIFY reviewChanged)
  Q_PROPERTY(QVariantList changes READ changes NOTIFY reviewChanged)
  Q_PROPERTY(
      QVariantList draftContext READ draftContext NOTIFY conversationChanged)
  Q_PROPERTY(
      QString connectionStatus READ connectionStatus NOTIFY connectionChanged)
  Q_PROPERTY(
      bool testingConnection READ testingConnection NOTIFY connectionChanged)

public:
  explicit WorkspaceClient(const QString &directory, QObject *parent = nullptr);
  bool initialize(const QString &project, QString &error);
  QString dataDirectory() const { return Directory; }
  SideChatClient *sideChat() const { return Side; }
  Q_INVOKABLE bool prepareClose();
  Q_INVOKABLE QString pathFromUrl(const QString &url) const;
  Q_INVOKABLE QString shortcutText(const QString &sequence) const;
#define AGENT_PROPERTY(Type, Name, Signal)                                     \
  Type Name() const { return State.value(#Name).toVariant().value<Type>(); }
#include "WorkspaceProtocol.def"
#undef AGENT_PROPERTY
  Q_INVOKABLE void reloadCredential();
  Q_INVOKABLE QVariantMap researchPlan(bool side = false);
  Q_INVOKABLE bool applyChange(const QString &id, bool undo = false);
  Q_INVOKABLE void dismissChange(const QString &id);
  Q_INVOKABLE void refreshResearch();
  Q_INVOKABLE bool createProject(const QString &name, const QString &directory);
  Q_INVOKABLE bool openProject(const QString &directory);
  Q_INVOKABLE bool selectProject(const QString &id);
  Q_INVOKABLE void selectObject(const QString &id);
  Q_INVOKABLE void addObject(const QString &path);
  Q_INVOKABLE bool resolveAnalysisObjectApproval(const QString &id, bool allow);
  Q_INVOKABLE void relocateObject(const QString &projectId,
                                  const QString &objectId, const QString &path);
  Q_INVOKABLE void setDraft(const QString &text);
  Q_INVOKABLE bool saveRequest(const QString &objectId);
  Q_INVOKABLE void refreshFiles();
  Q_INVOKABLE void toggleDirectory(const QString &path);
  Q_INVOKABLE void previewFile(const QString &path);
  Q_INVOKABLE void openTextLink(const QString &link);
  Q_INVOKABLE void closePreview();
  Q_INVOKABLE void showProjectFolder();
  Q_INVOKABLE bool updateSettings(bool sharing, bool lightTheme,
                                  const QString &guiPath);
  Q_INVOKABLE bool updateWorkspaceSettings(bool sharing, bool lightTheme,
                                           const QString &guiPath,
                                           bool autoAddObjects);
  Q_INVOKABLE void openInGui(const QString &objectId);
  Q_INVOKABLE bool addMemory(const QString &objectId, const QString &text);
  Q_INVOKABLE bool transferMemory(const QString &projectId,
                                  const QString &objectId, const QString &path,
                                  bool importing);
  Q_INVOKABLE void refreshMemory();
  Q_INVOKABLE bool updateMemorySettings(bool use, bool generate);
  Q_INVOKABLE bool editMemory(const QString &objectId, const QString &id,
                              const QString &text);
  Q_INVOKABLE bool forgetMemory(const QString &objectId, const QString &id);
  Q_INVOKABLE void organizeMemory(bool retryFailures = true);
  Q_INVOKABLE bool updateModelSettings(const QVariantMap &settings,
                                       const QString &key, bool clearKey,
                                       const QString &mcpPath);
  Q_INVOKABLE void testConnection(const QVariantMap &settings,
                                  const QString &key, bool clearKey);
  Q_INVOKABLE void cancelConnectionTest();
  Q_INVOKABLE bool sendRequest(const QString &objectId);
  Q_INVOKABLE bool submitRequest(const QString &objectId, const QString &mode);
  Q_INVOKABLE void discardQueuedInput(const QString &id);
  Q_INVOKABLE bool resumeInputQueue();
  Q_INVOKABLE QVariantList execution(const QString &runId);
  Q_INVOKABLE bool openEvidence(const QString &id, qint64 offset = 0);
  Q_INVOKABLE void inspectObject(const QString &objectId, const QString &query,
                                 const QString &representation);
  Q_INVOKABLE void attachObjectReference(const QString &objectId,
                                         const QString &query,
                                         bool side = false);
  Q_INVOKABLE void attachSelection(const QString &text, const QString &title,
                                   bool side = false);
  Q_INVOKABLE void attachEvidence(const QString &id, bool side = false);
  Q_INVOKABLE void attachPreview(bool side = false);
  Q_INVOKABLE void removeContext(int index, bool side = false);
  Q_INVOKABLE void createSideChatFromSelection(const QString &text);
  Q_INVOKABLE void cancelRun();
  Q_INVOKABLE bool createSideChat();
  Q_INVOKABLE bool selectSideChat(const QString &id);
  Q_INVOKABLE bool sendSideRequest();
  Q_INVOKABLE bool submitSideRequest(const QString &mode);
  Q_INVOKABLE void quoteSideAnswer(const QString &messageId);
  Q_INVOKABLE bool startGoal(const QString &objective, qint64 tokenBudget,
                             int roundLimit);
  Q_INVOKABLE bool updateGoal(const QString &objective, qint64 tokenBudget,
                              int roundLimit);
  Q_INVOKABLE bool resumeGoal();
  Q_INVOKABLE void pauseGoal();

signals:
  void workspaceChanged();
  void objectChanged();
  void messagesChanged();
  void conversationChanged();
  void filesChanged();
  void settingsChanged();
  void analysisObjectApprovalsChanged();
  void memoryChanged();
  void busyChanged();
  void previewChanged();
  void credentialChanged();
  void runChanged();
  void sideChatsChanged();
  void goalChanged();
  void evidenceChanged();
  void reviewChanged();
  void connectionChanged();
  void sideChatChanged();
  void operationError(const QString &message);
  void notice(const QString &message);
  void modelSettingsSaved();
  void evidenceOpened(const QString &id, const QString &title);
  void sideChatOpened();

private:
  friend class SideChatClient;
  template <typename T> T call(const QString &name, const QJsonArray &args) {
    const auto result = request(name, args);
    if constexpr (!std::is_void_v<T>)
      return result.toVariant().value<T>();
  }
  QJsonValue request(const QString &name, const QJsonArray &args,
                     const QString &sideId = {});
  bool waitFor(const bool &completed);
  void receive(const QJsonObject &message);
  void fail(const QString &error);
  void notify();

  QString Directory, TransportError;
  AgentProcess Process;
  QTimer NotifyTimer;
  QJsonObject State;
  QSet<QByteArray> Changed;
  QList<QJsonObject> Events;
  SideChatClient *Side = nullptr;
  QJsonValue Reply;
  qint64 RequestId = 0;
  bool Ready = false;
  bool Initialized = false;
  bool ReplyReceived = false;
};
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_CLIENT_WORKSPACECLIENT_H
