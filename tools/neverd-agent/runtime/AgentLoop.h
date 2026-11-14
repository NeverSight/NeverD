//===- AgentLoop.h - Stateful model and tool loop ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent agent context with shared token accounting and NPH dispatch.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_RUNTIME_AGENTLOOP_H
#define NEVERD_TOOLS_AGENT_RUNTIME_AGENTLOOP_H
#include "ContextUsage.h"
#include "NphTools.h"
#include "models/ModelClient.h"

#include <QSet>
#include <memory>

namespace neverd::agent {
struct RunBudget {
  int Steps = 0, Active = 0;
  qint64 TokenLimit = 0, Tokens = 0, Reserved = 0;
  bool Estimated = false, LimitReached = false;
  /// A persistent goal's allowance rather than one question's.
  bool Goal = false;
};
class AgentLoop final : public QObject {
  Q_OBJECT
public:
  AgentLoop(QString projectId, QString id, ModelProfile profile, QString key,
            std::shared_ptr<RunBudget> budget, QObject *parent = nullptr);
  void start(QString system, QString prompt, QString sessionPath);
  void cancel();
  bool steer(const QString &inputId, const QString &text);
  NphTools &tools() { return Tools; }
  QString id() const { return Id; }
  QString requestText() const { return Prompt; }
  QString status() const { return Status; }
  QString activity() const { return Activity; }
  int steps() const { return Steps; }
  bool running() const { return Running; }
  QJsonObject forkContext(const QString &visibleAnswer) const;
  QVariantMap contextUsage() const {
    return Usage.toJson(Profile).toVariantMap();
  }
  // A failed audit write stops before the next model request/tool effect.
  std::function<bool(const QJsonObject &, QString &)> Audit;
signals:
  void changed();
  void textDelta(const QString &text);
  void steeringApplied(const QString &inputId);
  void finished(const QString &text, const QString &error, bool cancelled);

private:
  void next();
  QJsonArray history(const QString &checkpoint, const QJsonArray &groups,
                     const QJsonArray &current) const;
  bool reserveRequest(ModelProfile &requestProfile, const QJsonObject &request);
  void settleRequest(const ModelTurn &turn);
  void beginCompaction();
  void compactNext();
  void acceptCompaction(const ModelTurn &turn);
  bool persist(const QString &answer, const QJsonArray &items, QString &error);
  QJsonObject contextSnapshot(const QString &answer,
                              const QJsonArray &items) const;
  void invokeNext();
  bool applySteering();
  void complete(QString text, QString error = {});
  bool audit(QJsonObject event);
  QString Id, Key, System, Prompt, SessionPath, Status, Activity, Answer;
  ModelProfile Profile;
  std::shared_ptr<RunBudget> Budget;
  ModelClient Client;
  NphTools Tools;
  QJsonArray PastGroups, History, Current, VisibleTools;
  QList<ModelToolCall> Calls;
  QList<QJsonObject> Results;
  QSet<qsizetype> FinishedCalls;
  qsizetype NextCall = 0;
  int InFlightCalls = 0;
  bool SerialCallActive = false;
  QList<QPair<QString, QString>> PendingSteers;
  ContextUsage Usage;
  QString Checkpoint, CompactSource, CompactSummary;
  QJsonArray RetainedGroups, RetainedCurrent;
  QList<qsizetype> ExchangeStarts, RetainedExchangeStarts;
  qsizetype CompactOffset = 0, CompactEnd = 0;
  qint64 CompactBeforeEstimate = 0;
  qint64 RequestReservation = 0, RequestEstimate = 0;
  int Steps = 0, CompactRequests = 0;
  bool Running = false, Cancelling = false, Completing = false;
  bool RequestSent = false;
};
} // namespace neverd::agent
#endif // NEVERD_TOOLS_AGENT_RUNTIME_AGENTLOOP_H
