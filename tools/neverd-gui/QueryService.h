#pragma once

#include "EngineClient.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
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
  void cancelReads();

  // resetSession must coincide with retiring the old transport/session; it
  // cannot stop a synchronous engine call by itself. Live subscribers receive
  // worker_stopped. setAvailable(false) performs the same retirement.
  void resetSession();
  void setAvailable(bool available);
  bool available() const;
  QString projectId() const;
  QString revision() const;
  quint64 sessionEpoch() const;
  // Monotonic within an open project; ordinary edit revisions do not reset it.
  bool analysisComplete() const;
  bool hasPending() const;
  bool hasCommands() const;
  void setCacheBudgetMiB(int mebibytes);

public slots:
  void receive(const QJsonObject &response);

signals:
  void contextChanged();
  // Once per session, after the discovering job has delivered its replies.
  void analysisCompleted();
  void pendingChanged();
  void idle();

private:
  struct State;
  std::unique_ptr<State> state_;
};
