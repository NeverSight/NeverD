//===- AgentController.h - Agent desktop application state --------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own the CLI workspace state, project storage and project-bound actions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_APP_AGENTCONTROLLER_H
#define NEVERD_TOOLS_AGENT_APP_AGENTCONTROLLER_H

#include "SideChat.h"
#include "project/ConversationInput.h"
#include "project/EvidenceStore.h"
#include "project/GoalState.h"
#include "project/ProjectStore.h"
#include "runtime/MemoryLearner.h"
#include "runtime/ResearchRuntime.h"

#include <QJsonObject>
#include <QLockFile>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <vector>

namespace neverd::agent {
struct WorkbenchConversation;
enum class ShutdownStatus { Ready, Pending, Failed };

class AgentController final : public QObject {
  Q_OBJECT

public:
  explicit AgentController(QString directory, QObject *parent = nullptr);
  bool initialize(QString &error);
  bool initializeWorkbench(const WorkbenchConversation &conversation,
                           const QString &attachment, QString &error);
  bool flush(QString &error);
  ShutdownStatus prepareShutdown();
  bool prepareClose();
  QVariantList projects() const;
  QVariantMap activeProject() const;
  QString activeId() const { return ActiveId; }
  QString selectedObjectId() const { return SelectedObjectId; }
  QVariantMap selectedObject() const;
  QVariantList messages() const;
  QString draft() const { return Conversation.value("draft").toString(); }
  QVariantList files() const { return Files; }
  QVariantList memory() const { return Memory; }
  QVariantList sharedMemory() const { return SharedMemory; }
  QString dataDirectory() const { return DataDirectory; }
  QString guiPath() const { return GuiPath; }
  bool sharing() const { return Sharing; }
  bool useMemory() const { return UseMemory; }
  bool generateMemory() const { return GenerateMemory; }
  QString memoryStatus() const { return MemoryStatus; }
  bool memoryWorking() const { return Learner.running(); }
  QVariantList projectMemory() const { return ProjectMemory; }
  bool lightTheme() const { return LightTheme; }
  bool autoAddAnalysisObjects() const { return AutoAddAnalysisObjects; }
  QVariantList analysisObjectApprovals() const;
  bool busy() const { return PendingImports != 0; }
  QString previewTitle() const { return PreviewTitle; }
  QString previewText() const { return PreviewText; }
  QVariantMap modelSettings() const;
  QString mcpPath() const { return McpPath; }
  bool modelConfigured() const { return !Profile.Model.isEmpty(); }
  bool credentialBusy() const { return CredentialJobs != 0; }
  QString credentialStatus() const;
  void reloadCredential();
  bool running() const;
  QVariantList progress() const;
  QVariantMap contextUsage() const;
  SideChat *sideChat() const;
  QVariantList sideChats() const;
  QVariantMap goal() const;
  QVariantList pendingInputs() const {
    return Conversation["pendingInputs"].toArray().toVariantList();
  }
  bool queuePaused() const { return Conversation["queuePaused"].toBool(); }
  QVariantMap evidence() const { return Evidence.toVariantMap(); }
  bool evidenceBusy() const { return InspectionJobs.contains(ActiveId); }
  bool reviewBusy() const { return WritingProjects.contains(ActiveId); }
  QVariantList changes();
  QVariantMap researchPlan(bool side = false);
  bool applyChange(const QString &id, bool undo = false);
  void dismissChange(const QString &id);
  void refreshResearch();
  QVariantList draftContext() const {
    return Conversation["context"].toArray().toVariantList();
  }
  QString connectionStatus() const { return ConnectionStatus; }
  bool testingConnection() const { return TestClient != nullptr; }

  bool createProject(const QString &name, const QString &directory);
  bool openProject(const QString &directory);
  bool selectProject(const QString &id);
  void selectObject(const QString &id);
  void addObject(const QString &path);
  bool resolveAnalysisObjectApproval(const QString &id, bool allow);
  void relocateObject(const QString &projectId, const QString &objectId,
                      const QString &path);
  void setDraft(const QString &text);
  bool saveRequest(const QString &objectId);
  void refreshFiles();
  void toggleDirectory(const QString &path);
  void previewFile(const QString &path);
  void openTextLink(const QString &link);
  void closePreview();
  void showProjectFolder();
  bool updateSettings(bool sharing, bool lightTheme, const QString &guiPath);
  bool updateWorkspaceSettings(bool sharing, bool lightTheme,
                               const QString &guiPath, bool autoAddObjects);
  void enableInteractiveObjectApprovals() { InteractiveObjectApprovals = true; }
  void openInGui(const QString &objectId);
  bool addMemory(const QString &objectId, const QString &text);
  bool transferMemory(const QString &projectId, const QString &objectId,
                      const QString &path, bool importing);
  void refreshMemory();
  bool updateMemorySettings(bool use, bool generate);
  bool editMemory(const QString &objectId, const QString &id,
                  const QString &text);
  bool forgetMemory(const QString &objectId, const QString &id);
  void organizeMemory(bool retryFailures = true);
  bool updateModelSettings(const QVariantMap &settings, const QString &key,
                           bool clearKey, const QString &mcpPath);
  void testConnection(const QVariantMap &settings, const QString &key,
                      bool clearKey);
  void cancelConnectionTest();
  bool sendRequest(const QString &objectId);
  bool submitRequest(const QString &objectId, const QString &mode);
  void discardQueuedInput(const QString &id);
  bool resumeInputQueue();
  QVariantList execution(const QString &runId);
  bool openEvidence(const QString &id, qint64 offset = 0);
  void inspectObject(const QString &objectId, const QString &query,
                     const QString &representation);
  void attachObjectReference(const QString &objectId, const QString &query,
                             bool side = false);
  void attachSelection(const QString &text, const QString &title,
                       bool side = false);
  void attachEvidence(const QString &id, bool side = false);
  void attachPreview(bool side = false);
  void removeContext(int index, bool side = false);
  void createSideChatFromSelection(const QString &text);
  void cancelRun();
  bool createSideChat();
  bool selectSideChat(const QString &id);
  bool sendSideRequest();
  bool submitSideRequest(const QString &mode);
  void quoteSideAnswer(const QString &messageId);
  bool startGoal(const QString &objective, qint64 tokenBudget, int roundLimit);
  bool updateGoal(const QString &objective, qint64 tokenBudget, int roundLimit);
  bool resumeGoal();
  void pauseGoal();

signals:
  void workspaceChanged();
  void conversationChanged();
  void messagesChanged();
  void objectChanged();
  void filesChanged();
  void settingsChanged();
  void analysisObjectApprovalsChanged();
  void memoryChanged();
  void credentialChanged();
  void modelSettingsSaved();
  void busyChanged();
  void previewChanged();
  void operationError(const QString &message);
  void notice(const QString &message);
  void desktopUrlRequested(const QString &url, const QString &error);
  void runChanged();
  void executionRow(const QString &runId, qsizetype index,
                    const QJsonObject &record);
  void sideChatChanged();
  void sideChatsChanged();
  void goalChanged();
  void evidenceChanged();
  void evidenceOpened(const QString &id, const QString &title);
  void reviewChanged();
  void sideChatOpened();
  void connectionChanged();
  void runFinished(const QString &projectId, const QString &text,
                   const QString &error, bool cancelled);

private:
  using ObjectImportCall = std::shared_ptr<plugin::ActionCall>;
  using ObjectImported = std::function<void(const AnalysisObject &)>;
  struct ObjectImport {
    QString Id, ProjectId, SideChatId, SourceName, Path, Reason;
    ObjectImportCall Call;
    ObjectImported Imported;
  };
  RequestObjectImport objectImportFor(const QString &projectId,
                                      const QString &sideChatId = {});
  void requestObjectImport(const QString &projectId, const QString &sideChatId,
                           const QString &sourceId, const QString &path,
                           const QString &reason, ObjectImportCall call,
                           ObjectImported imported);
  void fingerprintObject(const QString &path,
                         std::function<void(FingerprintResult)> ready);
  void importRequestedObject(ObjectImport request, bool automatic);
  static void finishObjectImport(const ObjectImport &request,
                                 const AnalysisObject &object,
                                 const char *decision);
  void objectAdded(const Project &project);
  bool loadConfiguration(QJsonObject &config, QString &error);
  Project *findProject(const QString &id);
  const Project *findProject(const QString &id) const;
  const AnalysisObject *findObject(const Project &project,
                                   const QString &id) const;
  bool bindProject(Project project, QString &error);
  bool saveSettings(QString &error);
  bool readModelKey(QString &key, QString &error) const;
  bool commitModelSettings(const ModelProfile &profile, const QString &mcpPath,
                           const QString &newKey);
  void removeCredential(const QString &id);
  bool writeCatalog(QString &error);
  void readMemoryViews();
  void queueMemory(const QString &projectId, const QJsonObject &source);
  void scheduleMemory();
  void drainMemory();
  void foregroundStarted();
  void collectFiles(const QString &path, int depth);
  void updateAnswer(const QString &projectId, const QString &messageId,
                    const QString &text, const QString &status, bool append);
  QString resolveMcp() const;
  QString resolveWorker() const;
  bool projectHasAnalysis(const QString &projectId) const;
  QJsonArray sharedMemoryFor(const Project &project);
  bool loadSideChats(const Project &project, QString &error);
  bool saveSideIndex(const Project &project, QString &error);
  void connectSideChat(SideChat *chat, const QString &projectId);
  bool beginRequest(const QString &projectId, const QString &prompt,
                    const QString &objectId, bool goalRound,
                    const QJsonObject &input = {});
  bool commitConversation(const QString &projectId, const QJsonObject &value);
  bool dispatchQueuedInput(const QString &projectId);
  void setQueuePaused(const QString &projectId, bool paused);
  bool appendContext(const QJsonObject &reference, bool side);
  bool runGoalRound(const QString &projectId);
  void goalRunChanged(const QString &projectId, const QString &goalId);
  void goalRunFinished(const QString &projectId, const QString &goalId,
                       const QString &error, bool cancelled);
  void pauseProjectGoal(const QString &projectId);
  QString DataDirectory;
  QString WorkbenchAttachment;
  QList<Project> Projects;
  QStringList KnownDirectories;
  std::vector<std::unique_ptr<QLockFile>> ProjectLocks;
  QString ActiveId;
  QString SelectedObjectId;
  QString GuiPath;
  bool Sharing = false;
  bool UseMemory = true, GenerateMemory = true;
  MemoryLearner Learner;
  QTimer MemoryTimer;
  QString MemoryStatus;
  QVariantList ProjectMemory;
  bool LightTheme = false;
  bool AutoAddAnalysisObjects = false;
  bool InteractiveObjectApprovals = false;
  QList<ObjectImport> ObjectApprovals;
  int PendingImports = 0;
  bool DraftDirty = false;
  QJsonObject Conversation;
  QTimer DraftTimer;
  QSet<QString> ExpandedDirectories;
  QVariantList Files;
  QVariantList Memory;
  QVariantList SharedMemory;
  QString PreviewTitle;
  QString PreviewPath;
  QString PreviewText;
  ModelProfile Profile;
  int CredentialJobs = 0;
  QString CachedCredentialId, CachedKey, CredentialError;
  QString McpPath, ConnectionStatus;
  QPointer<ModelClient> TestClient;
  QHash<QString, ResearchRuntime *> Runtimes;
  QHash<QString, QJsonObject> Conversations;
  QTimer RunSaveTimer, StreamTimer;
  QSet<QString> DirtyRuns;
  QHash<QString, SideChat *> SideChats;
  QHash<QString, QStringList> SideChatIds;
  QHash<QString, QString> ActiveSideIds;
  QHash<QString, GoalState> Goals;
  QHash<QString, std::shared_ptr<RunBudget>> GoalBudgets;
  QJsonObject Evidence;
  QHash<QString, McpSession *> Inspectors;
  QSet<QString> InspectionJobs;
  QSet<QString> WritingProjects;
};
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_APP_AGENTCONTROLLER_H
