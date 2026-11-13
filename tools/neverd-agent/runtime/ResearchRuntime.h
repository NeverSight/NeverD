//===- ResearchRuntime.h - Project and object orchestration -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A persistent project coordinator and independent object/subtask contexts.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_RUNTIME_RESEARCHRUNTIME_H
#define NEVERD_TOOLS_AGENT_RUNTIME_RESEARCHRUNTIME_H
#include "AgentLoop.h"
#include "ExecutionTrace.h"
#include "McpSession.h"

#include <QElapsedTimer>
#include <QFile>
#include <QPointer>
#include <QSet>
#include <QVariantList>
#include <functional>

namespace neverd::agent {
using RequestObjectImport = std::function<void(
    const QString &sourceId, const QString &path, const QString &reason,
    std::shared_ptr<plugin::ActionCall> call,
    std::function<void(const AnalysisObject &)> imported)>;

struct ResearchRunOptions {
  QString SessionId;
  std::shared_ptr<RunBudget> Budget;
  QJsonObject Goal;
  QString WorkerPath;
  QString GuiAttachment;
  QString UserPrompt;
  RequestObjectImport ImportObject;
  bool UseMemory = true;
  bool GenerateMemory = false;
};
class ResearchRuntime final : public QObject {
  Q_OBJECT
public:
  explicit ResearchRuntime(QObject *parent = nullptr);
  bool start(const Project &project, const ModelProfile &profile, QString key,
             QString mcp, const QString &prompt, const QString &objectId,
             QJsonArray sharedMemory, QString &error,
             ResearchRunOptions options = {});
  void cancel();
  void retireEngines();
  QString selectedObjectId() const { return SelectedId; }
  bool reviewPending() const { return PendingReviews != 0; }
  bool steer(const QString &inputId, const QString &text, QString &error,
             const QString &userText = {});
  bool running() const { return Running; }
  QVariantList progress() const;
  QVariantMap contextUsage() const {
    return Root ? Root->contextUsage() : QVariantMap{};
  }
  QString runId() const { return RunId; }
  QJsonObject goalReport() const { return GoalReport; }
  QVariantList execution() const { return Trace.rows(); }
  QJsonObject forkContext(const QString &visibleAnswer) const {
    return Root ? Root->forkContext(visibleAnswer) : QJsonObject{};
  }
signals:
  void changed();
  void executionRow(const QString &runId, qsizetype index,
                    const QJsonObject &record);
  void textDelta(const QString &text);
  void steeringApplied(const QString &inputId);
  void artifactsChanged();
  void memorySource(const QJsonObject &source);
  void finished(const QString &text, const QString &error, bool cancelled);

private:
  using Call = std::shared_ptr<plugin::ActionCall>;
  AgentLoop *node(const AnalysisObject *object, int depth, QString label,
                  const QString &prompt, Call parent = {});
  void configure(AgentLoop *loop, const AnalysisObject *object, int depth,
                 QString &error);
  void drainDelegations();
  void withEngine(const AnalysisObject &object, Call call,
                  std::function<void(McpSession *)> ready);
  bool audit(QJsonObject event, QString &error);
  static void reply(Call call, QJsonObject result);
  Project ProjectValue;
  ModelProfile Profile;
  QString Key, McpPath, WorkerPath, RunId, SelectedId;
  QString GuiAttachment;
  RequestObjectImport ImportObject;
  QString SessionId, ContextDirectory, UserPrompt;
  QJsonObject Goal, GoalReport;
  QJsonArray SharedMemory;
  QJsonArray MemoryEvidence;
  bool UseMemory = true, GenerateMemory = false;
  std::shared_ptr<RunBudget> Budget;
  QList<AgentLoop *> Nodes;
  QHash<AgentLoop *, QVariantMap> Labels;
  QHash<QString, McpSession *> Engines;
  QHash<QString, QList<std::function<void(McpSession *)>>> EngineWaiters;
  QHash<QString, QList<Call>> EngineCalls;
  QSet<QString> BusyObjects;
  QSet<QString> DependencyObjects;
  struct Delegation {
    AnalysisObject Object;
    QString Task;
    int Depth;
    Call Parent;
  };
  QList<Delegation> Delegations;
  int StartingNodes = 0;
  QPointer<AgentLoop> Root;
  QFile AuditFile;
  ExecutionTrace Trace;
  QElapsedTimer Clock;
  bool Running = false;
  bool Cancelling = false;
  int PendingReviews = 0;
};
} // namespace neverd::agent
#endif // NEVERD_TOOLS_AGENT_RUNTIME_RESEARCHRUNTIME_H
