#pragma once

#include "EngineClient.h"
#include "QueryService.h"

#include <QElapsedTimer>
#include <QSet>
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
  /// The reads asked of the owner from what it keeps (askOwner).
  quint64 ownerReads() const { return ownerReads_; }
  /// The reads answered with a page the owner read with an edit (take), for
  /// which it was asked nothing.
  quint64 pagesTaken() const { return pagesTaken_; }
  void setSnapshotBlocked(bool blocked);
  /// The user is making an edit a replica will render: it starts now, if it has
  /// not, and reads \p address, so that the read after the edit finds what this
  /// one left.  Given the \p declaration the user would state, it renders the
  /// function under that one, which the view after the user states it reads
  /// (prepare_function with "declaration").  Nothing waits for this read, and
  /// nobody hears its answer.  One runs at a time: a later one waits for it,
  /// in place of any that waits already.
  void prepareRead(quint64 address, const QString &declaration = {});
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
  /// The owner's answer to the running read from what it keeps; what it does
  /// not keep goes to the replica, or starts it.
  void askOwner();
  /// A page the owner read with an edit (QueryService::followed), which a view
  /// of this dispatcher's showed as the owner gave it: the read that view
  /// makes at the change is answered with it, the owner asked nothing.
  void take(const QString &operation, const QJsonObject &payload,
            const QString &was, const QJsonObject &response, qint64 bytes);
  /// Such a page: the read it answers, the answer as this dispatcher's reads
  /// have it, and the owner's own.
  struct Carried {
    QString operation;
    QJsonObject read, answer, response;
    qint64 bytes = 0;
  };
  /// Hands \p page to this dispatcher's reads; false where they are not yet
  /// those of the state it was read at.
  bool give(const Carried &page);
  /// A read took a page so given.
  void tookGiven();
  /// The owner gave the page \p response holds: it answers for it from here.
  void ownerGave(const QJsonObject &response);
  /// Asks the owner what the store holds, once for a project state; a replica
  /// starts beside the owner where that is part of the renders.
  void askKeptState();
  /// \p response as this dispatcher's reads have it: the owner's project and
  /// revision, and no analysis state, which a replica's never is the owner's.
  QJsonObject ofTheOwner(QJsonObject response) const;
  void forgetKeptRequests();
  void stopWorker();
  /// Leaves what the worker is answering to run to its end, unanswered.
  void abandonRunningRead();
  void receive(const QJsonObject &message, qint64 bytes = 0);
  void dispatch();
  /// \p bytes is what the answer weighs, where its sender said so.
  void finish(QJsonObject response, qint64 bytes = 0);
  void fail(const QString &code, const QString &message);

  QueryService &owner_;
  EngineClient client_;
  std::optional<quint64> preparedRead_;
  QString preparedDeclaration_;
  bool preparesFunctions_ = false;
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
  /// What it said last for this project, which stands while it is asked again
  /// after a change: renders a change of names or comments leaves are kept
  /// under the names it leaves.
  KeptState keptBefore_ = KeptState::Unknown;
  bool ownerReadsKept_ = false;
  /// The names (etags) of the source pages the owner gave.  One a view keeps
  /// names a document the owner kept, which a ready replica would read whole,
  /// so the owner answers for it while it keeps it.
  QSet<QString> ownerPages_;
  /// The pages the owner said ahead of the change they came with.
  QList<Carried> carried_;
  /// A read took such a page while its edit was still to be given its own
  /// answer, and no replica was started for it yet (setSnapshotBlocked).
  bool tookSince_ = false;
  quint64 serial_ = 0, next_ = 0, ownerReads_ = 0, pagesTaken_ = 0;
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
  /// Gives \p owner the answers its reads have already (QueryService).
  qsizetype deliverNow(QObject *owner) {
    qsizetype given = 0;
    for (auto &lane : lanes_)
      given += lane.service->queries().deliverNow(owner);
    return given;
  }
  /// The pages the lanes took as the owner read them with an edit.
  quint64 pagesTaken() const {
    quint64 taken = 0;
    for (const auto &lane : lanes_)
      taken += lane.service->pagesTaken();
    return taken;
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
  quint64 ownerReads() const {
    quint64 reads = 0;
    for (const auto &lane : lanes_)
      reads += lane.service->ownerReads();
    return reads;
  }
  void setRendersAhead(bool renders) {
    lanes_.front().service->setRendersAhead(renders);
  }
  QString renderAheadState() const {
    return lanes_.front().service->renderAheadState();
  }
  /// AnalysisService::prepareRead, in the replica that takes reads first.
  void prepareRead(quint64 address, const QString &declaration = {}) {
    lanes_.front().service->prepareRead(address, declaration);
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
