#pragma once

#include "EngineClient.h"
#include "QueryService.h"

#include <QElapsedTimer>
#include <array>
#include <memory>
#include <optional>

namespace neverd::gui {

/// Disposable read-only executor for expensive views. Project writes and
/// navigation keep their own worker; a cancelled synchronous engine call
/// can be stopped here without losing the owner's staged changes.
class AnalysisService final : public QObject {
  Q_OBJECT
public:
  AnalysisService(QueryService &owner, QString workerPath,
                  QObject *parent = nullptr);
  ~AnalysisService() override;
  QueryService &queries() { return queries_; }
  bool ready() const { return ready_; }
  /// Whether a replica runs, or is on its way.
  bool replicaStarted() const { return ready_ || startingReplica_; }
  void setSnapshotBlocked(bool blocked);
  /// The user is making an edit a replica will render: it starts now, if it has
  /// not, and reads \p address, so that the read after the edit finds what this
  /// one left.  Nothing waits for this read, and nobody hears its answer.
  void prepareRead(quint64 address);
  /// Asks the replica to render functions ahead in idle time.
  void setRendersAhead(bool renders) { rendersAhead_ = renders; }
  /// The owner can answer a read from what earlier sessions kept, so that a
  /// replica starts only for what they did not.
  void setOwnerReadsKept(bool reads) { ownerReadsKept_ = reads; }
  /// That job's state as the replica's last heartbeat reports it; empty where
  /// it reports none.
  const QString &renderAheadState() const { return renderAheadState_; }
  static bool handles(const QString &operation);

signals:
  void diagnostic(const QString &message);
  /// The replica's decompiles read the image's call summaries now, which it
  /// solved in idle time: earlier replies no longer answer.
  void sourceRefined();

private:
  QString send(const QString &operation, const QJsonObject &payload);
  void reset();
  void requestSnapshot();
  void startReplica();
  void sendPreparedRead();
  /// Asks the owner for the running read from what is kept, and starts the
  /// replica for what is not.
  void readKept();
  void forgetKeptRequests();
  void stopWorker();
  /// Leaves what the worker is answering to run to its end, unanswered.
  void abandonRunningRead();
  void receive(const QJsonObject &message);
  void dispatch();
  void finish(QJsonObject response);
  void fail(const QString &code, const QString &message);

  QueryService &owner_;
  EngineClient client_;
  std::optional<quint64> preparedRead_;
  QueryService queries_;
  QString workerPath_, active_, operation_, wire_, restore_, workerRevision_;
  /// The read the worker runs for nobody, and what restoring the worker took.
  QString abandoned_;
  QElapsedTimer restoring_;
  qint64 restoreMilliseconds_ = 0;
  QJsonObject payload_, snapshot_;
  QueryService::SubscriptionId snapshotRequest_ = 0;
  QueryService::SubscriptionId keptStateRequest_ = 0, keptRead_ = 0;
  /// What the owner says the store holds for the current project state.
  enum class KeptState { Unknown, Asked, Whole, Part };
  KeptState keptState_ = KeptState::Unknown;
  bool ownerReadsKept_ = false;
  quint64 serial_ = 0, next_ = 0;
  bool ready_ = false;
  bool snapshotBlocked_ = false, startingReplica_ = false;
  bool updatesNames_ = false, refreshingNames_ = false;
  bool solvesSummaries_ = false, summariesReady_ = false;
  bool rendersAhead_ = false;
  QString renderAheadState_;
  /// The project whose replica last exited abnormally, whose replicas render
  /// nothing ahead from then on.
  std::optional<QString> renderCrashProject_;
};

/// Bounded independent replicas. A function's pages and graph transaction stay
/// on one dispatcher; another function can use the other replica concurrently.
class AnalysisPool final : public QObject {
  Q_OBJECT
public:
  AnalysisPool(QueryService &owner, const QString &workerPath,
               QObject *parent = nullptr);
  QueryService &queries(const QJsonObject &payload, QObject *owner);
  /// The first replica's render-ahead job, as its last heartbeat reports it;
  /// empty where it reports none.
  /// Whether the replica reads start in renders functions ahead.  A second one
  /// serves only overlapping reads, and rendering every function again there
  /// would double the memory and the work.
  void setOwnerReadsKept(bool reads) {
    for (auto &lane : lanes_)
      lane.service->setOwnerReadsKept(reads);
  }
  bool replicaStarted() const {
    for (const auto &lane : lanes_)
      if (lane.service->replicaStarted())
        return true;
    return false;
  }
  bool replicaReady() const {
    for (const auto &lane : lanes_)
      if (lane.service->ready())
        return true;
    return false;
  }
  void setRendersAhead(bool renders) {
    lanes_.front().service->setRendersAhead(renders);
  }
  QString renderAheadState() const {
    return lanes_.front().service->renderAheadState();
  }
  /// AnalysisService::prepareRead, in the replica that takes reads first.
  void prepareRead(quint64 address) {
    lanes_.front().service->prepareRead(address);
  }
  /// Prepare one native entry in the same replica later source reads use.
  /// This is speculative read-only work; context changes cancel it normally.
  void prefetchSource(quint64 address);
  /// Avoid overlapping a large discovery snapshot with a fresh image replica.
  /// Reads in an existing replica remain independent of the owner.
  void setSnapshotBlocked(bool blocked);
  // External clients can split a graph summary and viewport across requests.
  // Keep that stateful protocol on one replica, as before the pool existed.
  QueryService &externalQueries() { return lanes_.front().service->queries(); }
  QSet<QObject *> snapshotOwners() const;
  void unsubscribeOwner(QObject *owner);
  void cancelReads(const QSet<QObject *> &keep = {});

signals:
  void diagnostic(const QString &message);
  /// A replica's decompiles changed once, as AnalysisService::sourceRefined.
  void sourceRefined();

private:
  struct Lane {
    std::unique_ptr<AnalysisService> service;
    std::optional<quint64> function;
  };
  struct Binding {
    quint64 function;
    size_t lane;
    QMetaObject::Connection destroyed;
  };
  std::array<Lane, 2> lanes_;
  QHash<QObject *, Binding> bindings_;
  /// The project those bindings are of.
  QString boundProject_;
  QObject preparationOwner_;
  quint64 preparationSerial_ = 0;
  bool preparing_ = false;
};
} // namespace neverd::gui
