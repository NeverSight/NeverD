#pragma once

#include "EngineClient.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <functional>
#include <memory>

// One dispatcher for a borrowed worker session. Call this object only from its
// owning thread. Pane-specific navigation generations remain with the caller.
class QueryService final : public QObject {
  Q_OBJECT
public:
  struct QuerySpec {
    QString operation;
    QJsonObject payload;
    enum RevisionPolicy { Latest, Exact } policy = Latest;
    QString expectedRevision;
    // Visible content precedes ordinary reads within the same command fence.
    // This affects scheduling only, never read identity or command order.
    enum Priority { Normal, Visible } priority = Normal;
  };
  struct Limits {
    qsizetype maxJobs = 48;
    qsizetype maxSubscriptions = 256;
    qsizetype maxQueuedBytes = 16 * 1024 * 1024;
    qsizetype maxRequestBytes = 8 * 1024 * 1024;
    int cacheBytes = 256 * 1024 * 1024;
  };
  using SubscriptionId = quint64;
  using Completion = std::function<void(const QJsonObject &)>;
  // A sender must return a unique, nonempty wire ID before delivering a reply.
  // Replies must arrive asynchronously through receive(), on this thread.
  using Sender = std::function<QString(const QString &, const QJsonObject &,
                                       const QString &expectedRevision)>;

  explicit QueryService(Sender sender, QObject *parent = nullptr);
  QueryService(Sender sender, Limits limits, QObject *parent = nullptr);
  // Inline so a Core/Test-only fake-transport target does not need to link the
  // real transport. No process or EngineClient is created by the service.
  explicit QueryService(EngineClient *client, QObject *parent = nullptr)
      : QueryService(
            Sender([client = QPointer<EngineClient>(client)](
                       const QString &operation, const QJsonObject &payload,
                       const QString &revision) {
              return client ? client->request(operation, payload, revision)
                            : QString{};
            }),
            parent) {
    if (client) {
      connect(client, &EngineClient::message, this, &QueryService::receive);
      connect(client, &EngineClient::stopped, this,
              [this] { setAvailable(false); });
      connect(client, &QObject::destroyed, this,
              [this] { setAvailable(false); });
    }
  }
  ~QueryService() override;

  // Completion is always deferred and request_id is this subscription's ID,
  // never a shared wire/cache ID. A zero ID means admission was rejected; its
  // deferred error still observes owner lifetime. An owner is required.
  SubscriptionId subscribe(QuerySpec spec, QObject *owner, Completion complete);
  // Returns a single payload containing {summary, viewport}. The two worker
  // requests cannot be interleaved with another read or command.
  SubscriptionId graphViewport(QJsonObject payload, QObject *owner,
                               Completion complete);
  SubscriptionId enqueueCommand(QString operation, QJsonObject payload,
                                QObject *owner, Completion complete);
  void unsubscribe(SubscriptionId id);
  void unsubscribeOwner(QObject *owner);
  /// Detach every read except those owned by \p keep (commands always run).
  void cancelReads(const QSet<QObject *> &keep = {});

  // resetSession must coincide with retiring the old transport/session; it
  // cannot stop a synchronous engine call by itself. Live subscribers receive
  // worker_stopped. setAvailable(false) performs the same retirement.
  void resetSession();
  /// Retire a read replica and bind its replies/cache to the authoritative
  /// project's version. The caller must retire its old transport first.
  void resetReadContext(const QString &project, const QString &revision);
  void setAvailable(bool available);
  bool available() const;
  QString projectId() const;
  QString revision() const;
  /// The current context change is a confirmed local display-name commit.
  bool localNamesOnly() const;
  quint64 sessionEpoch() const;
  // Monotonic within an open project; ordinary edit revisions do not reset it.
  bool analysisComplete() const;
  bool hasPending() const;
  /// Read jobs with live subscribers, excluding a cancelled job's final ACK.
  qsizetype pendingReadCount() const;
  bool hasCommands() const;
  void setCacheBudgetMiB(int mebibytes);
  /// Background analysis changed what reads list, such as the functions it
  /// discovered, without a new revision: cached reads and reads in flight no
  /// longer answer new reads.
  void listingChanged();

public slots:
  void receive(const QJsonObject &response);

signals:
  void contextChanged();
  // Once per session, after the discovering job has delivered its replies.
  void analysisCompleted();
  void pendingChanged();
  void idle();
  void loadProgress(const QJsonObject &payload);
  /// Validated executor state, after subscribers have installed the reply.
  /// Cached reads never republish an older snapshot.
  void backgroundSnapshotReady(const QJsonObject &snapshot);

private:
  struct State;
  std::unique_ptr<State> state_;
};
