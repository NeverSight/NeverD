#include "QueryService.h"

#include <QCache>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <utility>

namespace {
using Spec = QueryService::QuerySpec;
using Id = QueryService::SubscriptionId;

bool cacheable(const QString &operation) {
  static const QSet<QString> operations{"functions", "resolve",   "disasm",
                                        "bytes",     "decompile", "xrefs",
                                        "strings",   "segments"};
  return operations.contains(operation);
}

bool readable(const QString &operation) {
  static const QSet<QString> operations{"metadata",
                                        "history",
                                        "annotations",
                                        "contributions",
                                        "contribution_execute",
                                        "cfg",
                                        "cfg_summary",
                                        "cfg_viewport"};
  return cacheable(operation) || operations.contains(operation);
}

bool command(const QString &operation) {
  static const QSet<QString> operations{"open",
                                        "analyze",
                                        "annotation_set",
                                        "rename",
                                        "save",
                                        "reload",
                                        "undo",
                                        "redo",
                                        "history_reset",
                                        "contribution_register",
                                        "contribution_unregister"};
  return operations.contains(operation);
}

// Sort object keys only. Strings (including symbols and 64-bit addresses),
// numbers, array order and absent defaults keep their exact request meaning.
QJsonValue canonical(const QJsonValue &value, int depth, bool &valid) {
  if (depth > 64) {
    valid = false;
    return {};
  }
  if (value.isObject()) {
    const auto object = value.toObject();
    auto keys = object.keys();
    std::sort(keys.begin(), keys.end());
    QJsonObject result;
    for (const auto &key : keys)
      result.insert(key, canonical(object[key], depth + 1, valid));
    return result;
  }
  if (value.isArray()) {
    QJsonArray result;
    for (const auto &item : value.toArray())
      result.append(canonical(item, depth + 1, valid));
    return result;
  }
  return value;
}

bool sameAddress(const QJsonValue &left, const QJsonValue &right) {
  const auto parse = [](const QJsonValue &value, quint64 &address) {
    if (!value.isString())
      return false;
    const auto text = value.toString();
    if (text.size() < 3 || text.size() > 18 || text[0] != '0' ||
        (text[1] != 'x' && text[1] != 'X'))
      return false;
    for (const auto character : QStringView(text).mid(2))
      if (!((character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f') ||
            (character >= 'A' && character <= 'F')))
        return false;
    bool ok = false;
    address = text.mid(2).toULongLong(&ok, 16);
    return ok;
  };
  quint64 a = 0, b = 0;
  return parse(left, a) && parse(right, b) && a == b;
}

QJsonObject cleanResponse(QJsonObject response) {
  response.remove("request_id");
  response.remove("cancellation_requested");
  response.remove("calculation_stopped");
  response.remove("completed_before_cancellation");
  return response;
}
} // namespace

struct QueryService::State {
  enum Kind { Read, Graph, Command };
  enum Phase { Queued, Reading, Summary, Viewport, Delivering };
  struct Job {
    quint64 id = 0, epoch = 0, fence = 0;
    Kind kind = Read;
    Phase phase = Queued;
    Spec spec;
    QByteArray coalescingKey;
    qsizetype bytes = 0;
    QList<Id> subscribers;
    QString wireId, wireOperation, wireProject, wireRevision;
    bool cancelRequested = false;
    int graphRetries = 0;
    QJsonObject summary, result;
    QString summaryRevision;
  };
  using JobPtr = std::shared_ptr<Job>;
  struct Subscription {
    QPointer<QObject> owner;
    QObject *ownerKey = nullptr;
    Completion complete;
    quint64 epoch = 0, job = 0;
  };

  QueryService *q;
  Sender sender;
  Limits limits;
  bool available = false, pumpScheduled = false;
  quint64 epoch = 1, fence = 0, nextJob = 0;
  Id nextSubscription = 0;
  QString project, revision = "0";
  qsizetype queuedBytes = 0;
  QHash<quint64, JobPtr> jobs;
  QHash<Id, Subscription> subscriptions;
  QHash<QObject *, QMetaObject::Connection> owners;
  QQueue<quint64> queue;
  JobPtr active;
  QCache<QByteArray, QJsonObject> cache;

  State(QueryService *service, Sender send, Limits budget)
      : q(service), sender(std::move(send)), limits(budget),
        cache(std::clamp(budget.cacheBytes, 0, 1024 * 1024 * 1024)) {
    limits.maxJobs = std::max<qsizetype>(1, limits.maxJobs);
    limits.maxSubscriptions = std::max<qsizetype>(1, limits.maxSubscriptions);
    limits.maxQueuedBytes = std::max<qsizetype>(1, limits.maxQueuedBytes);
    limits.maxRequestBytes =
        std::clamp<qsizetype>(limits.maxRequestBytes, 1, 8 * 1024 * 1024);
  }

  bool pending() const { return !jobs.isEmpty() || !subscriptions.isEmpty(); }

  void notify() {
    const QPointer<QueryService> guard(q);
    emit q->pendingChanged();
    if (guard && !pending())
      emit q->idle();
  }

  QJsonObject error(const QString &code, const QString &message) const {
    return {{"protocol_major", 1},
            {"type", "response"},
            {"status", "error"},
            {"project_id", project},
            {"revision", revision},
            {"error", QJsonObject{{"code", code}, {"message", message}}}};
  }

  QByteArray key(const Job &job, bool forCache) const {
    QJsonArray parts{QString::number(epoch), project,
                     forCache ? revision : QString::number(job.fence),
                     job.spec.operation, job.spec.payload};
    if (!forCache) {
      parts.append(int(job.kind));
      parts.append(int(job.spec.policy));
      parts.append(job.spec.expectedRevision);
    }
    return QJsonDocument(parts).toJson(QJsonDocument::Compact);
  }

  void releaseOwner(QObject *owner) {
    for (const auto &subscription : std::as_const(subscriptions))
      if (subscription.ownerKey == owner)
        return;
    if (owners.contains(owner))
      QObject::disconnect(owners.take(owner));
  }

  Subscription takeSubscription(Id id) {
    const auto subscription = subscriptions.take(id);
    releaseOwner(subscription.ownerKey);
    return subscription;
  }

  void schedulePump() {
    if (pumpScheduled)
      return;
    pumpScheduled = true;
    QTimer::singleShot(0, q, [this] {
      pumpScheduled = false;
      pump();
    });
  }

  Id reject(QObject *owner, Completion complete, const QString &operation,
            const QString &code, const QString &message) {
    // Rejections do not reserve a job, subscription or payload budget. The
    // zero-ID completion is terminal and carries no result from an old epoch.
    if (owner && complete) {
      const QPointer<QObject> target(owner);
      const auto admittedEpoch = epoch;
      auto response = error(code, message);
      response["request_id"] = "0";
      response["operation"] = operation;
      QTimer::singleShot(0, q,
                         [this, target, admittedEpoch, response,
                          complete = std::move(complete)]() mutable {
                           if (!target)
                             return;
                           if (epoch != admittedEpoch) {
                             const auto operation = response["operation"];
                             response = error("session_changed",
                                              "The query session changed.");
                             response["request_id"] = "0";
                             response["operation"] = operation;
                           }
                           complete(response);
                         });
    }
    return 0;
  }

  Id add(Spec spec, Kind kind, QObject *owner, Completion complete) {
    if (!owner || owner->thread() != q->thread())
      return reject(owner, std::move(complete), spec.operation,
                    "invalid_request",
                    "A query requires an owner on its thread.");
    if (!available)
      return reject(owner, std::move(complete), spec.operation,
                    "transport_unavailable",
                    "Analysis worker is not available.");
    if ((kind == Command ? !command(spec.operation)
                         : !readable(spec.operation)) ||
        (spec.policy == Spec::Exact && spec.expectedRevision.isEmpty()))
      return reject(owner, std::move(complete), spec.operation,
                    "invalid_request",
                    "Invalid query operation or revision policy.");
    bool valid = true;
    spec.payload = canonical(spec.payload, 0, valid).toObject();
    const auto bytes =
        QJsonDocument(spec.payload).toJson(QJsonDocument::Compact).size() +
        spec.operation.toUtf8().size() + spec.expectedRevision.toUtf8().size() +
        256;
    if (!valid || bytes > limits.maxRequestBytes)
      return reject(owner, std::move(complete), spec.operation,
                    "budget_exceeded", "The query exceeds its request budget.");
    if (subscriptions.size() >= limits.maxSubscriptions)
      return reject(owner, std::move(complete), spec.operation, "queue_full",
                    "The query subscriber limit was reached.");

    auto candidate = std::make_shared<Job>();
    candidate->epoch = epoch;
    candidate->fence = fence;
    candidate->kind = kind;
    candidate->spec = std::move(spec);
    candidate->coalescingKey = key(*candidate, false);
    JobPtr job;
    if (kind != Command) {
      for (const auto &existing : std::as_const(jobs)) {
        if (existing->epoch == epoch && !existing->cancelRequested &&
            existing->phase != Delivering &&
            existing->coalescingKey == candidate->coalescingKey) {
          job = existing;
          break;
        }
      }
    }
    if (!job) {
      if (jobs.size() >= limits.maxJobs ||
          bytes > limits.maxQueuedBytes - queuedBytes)
        return reject(owner, std::move(complete), candidate->spec.operation,
                      "queue_full", "The query queue budget was reached.");
      job = std::move(candidate);
      job->id = ++nextJob;
      job->bytes = bytes;
      if (kind == Command)
        ++fence; // Later reads may not join a pre-command flight.
      jobs.insert(job->id, job);
      queue.enqueue(job->id);
      queuedBytes += bytes;
    }
    const auto id = ++nextSubscription;
    subscriptions.insert(id,
                         {owner, owner, std::move(complete), epoch, job->id});
    job->subscribers.append(id);
    if (!owners.contains(owner))
      owners.insert(
          owner, QObject::connect(owner, &QObject::destroyed, q, [this, owner] {
            q->unsubscribeOwner(owner);
          }));
    schedulePump();
    notify();
    return id;
  }

  void removeJob(const JobPtr &job) {
    if (!jobs.remove(job->id))
      return;
    queue.removeAll(job->id);
    queuedBytes -= job->bytes;
    if (active == job)
      active.reset();
  }

  void detach(Id id) {
    const auto iterator = subscriptions.constFind(id);
    if (iterator == subscriptions.cend())
      return;
    const auto job = jobs.value(iterator->job);
    takeSubscription(id);
    if (!job)
      return;
    job->subscribers.removeAll(id);
    if (!job->subscribers.isEmpty() || job->kind == Command)
      return;
    if (job->phase == Queued || job->phase == Delivering) {
      removeJob(job);
      schedulePump();
    } else if (!job->cancelRequested) {
      job->cancelRequested = true;
      // The administrative ACK has a different ID and is deliberately not
      // owned. Only the original request's terminal response releases active.
      sender("cancel", {{"request_id", job->wireId}}, {});
    }
  }

  void dispatch(const JobPtr &job, const QString &operation,
                const QJsonObject &payload) {
    job->wireOperation = operation;
    job->wireProject = project;
    job->wireRevision = revision;
    const auto expected =
        job->spec.policy == Spec::Exact ? job->spec.expectedRevision : revision;
    job->wireId = sender ? sender(operation, payload, expected) : QString{};
    if (job->wireId.isEmpty())
      finish(job, error("transport_unavailable",
                        "Analysis worker is not available."));
  }

  void pump() {
    if (active || queue.isEmpty() || !available)
      return;
    const auto job = jobs.value(queue.dequeue());
    if (!job) {
      schedulePump();
      return;
    }
    active = job;
    if (job->epoch != epoch) {
      finish(job, error("session_changed", "The query session changed."));
      return;
    }
    if (job->spec.policy == Spec::Exact &&
        job->spec.expectedRevision != revision) {
      finish(job, error("stale_revision",
                        "Project revision changed; refresh before retrying."));
      return;
    }
    if (job->kind == Read && cacheable(job->spec.operation)) {
      if (const auto *cached = cache.object(key(*job, true))) {
        finish(job, *cached);
        return;
      }
    }
    if (job->kind == Command)
      cache.clear(); // A save/registry command need not advance image revision.
    job->phase = job->kind == Graph ? Summary : Reading;
    dispatch(job,
             job->kind == Graph ? QString("cfg_summary") : job->spec.operation,
             job->kind == Graph
                 ? QJsonObject{{"address", job->spec.payload["address"]}}
                 : job->spec.payload);
  }

  void standalone(Id id, QJsonObject response) {
    QTimer::singleShot(0, q,
                       [this, id, response = std::move(response)]() mutable {
                         if (!subscriptions.contains(id))
                           return;
                         auto subscription = takeSubscription(id);
                         response["request_id"] = QString::number(id);
                         const QPointer<QueryService> guard(q);
                         if (subscription.owner && subscription.complete)
                           subscription.complete(response);
                         if (guard)
                           notify();
                       });
  }

  void finish(const JobPtr &job, QJsonObject response) {
    if (!jobs.contains(job->id))
      return;
    job->wireId.clear();
    job->phase = Delivering;
    job->result = cleanResponse(std::move(response));
    job->result["operation"] = job->spec.operation;
    const auto id = job->id;
    QTimer::singleShot(0, q, [this, id] { deliver(id); });
  }

  void deliver(quint64 id) {
    const auto job = jobs.value(id);
    if (!job || job->phase != Delivering)
      return;
    if (job->kind != Command && job->result["status"] == "ok" &&
        (job->epoch != epoch ||
         job->result["project_id"].toString() != project ||
         job->result["revision"].toString() != revision)) {
      if (job->epoch == epoch && job->spec.policy == Spec::Latest) {
        job->phase = Queued;
        job->result = {};
        active.reset();
        queue.prepend(job->id);
        schedulePump();
        return;
      }
      job->result =
          error(job->epoch == epoch ? "stale_revision" : "session_changed",
                "The query context changed before delivery.");
    }
    // Keep active through delivery. A callback can enqueue more work, cancel
    // peers, reset the session or destroy this service; never retain iterators.
    const auto ids = job->subscribers;
    const QPointer<QueryService> guard(q);
    for (const auto subscriptionId : ids) {
      const auto iterator = subscriptions.constFind(subscriptionId);
      if (iterator == subscriptions.cend() || iterator->job != id)
        continue;
      auto subscription = takeSubscription(subscriptionId);
      if (!subscription.owner || subscription.epoch != epoch)
        continue;
      auto response = job->result;
      response["request_id"] = QString::number(subscriptionId);
      if (subscription.complete)
        subscription.complete(response);
      if (!guard)
        return;
    }
    removeJob(job);
    schedulePump();
    notify();
  }

  void retire(const JobPtr &keep, const QString &code) {
    ++epoch;
    ++fence;
    cache.clear();
    const auto currentJobs = jobs.values();
    for (const auto &job : currentJobs) {
      if (job == keep) {
        job->epoch = epoch;
        for (const auto id : job->subscribers)
          if (subscriptions.contains(id))
            subscriptions[id].epoch = epoch;
        continue;
      }
      for (const auto id : job->subscribers) {
        if (!subscriptions.contains(id))
          continue;
        subscriptions[id].job = 0;
        subscriptions[id].epoch = epoch;
        auto response = error(code, "The query session was retired.");
        response["operation"] = job->spec.operation;
        standalone(id, response);
      }
      removeJob(job);
    }
  }

  void receive(QJsonObject response) {
    const auto job = active;
    if (!job || job->wireId.isEmpty() || response["type"] != "response" ||
        response["request_id"].toString() != job->wireId || job->epoch != epoch)
      return;
    if (response.contains("operation") &&
        response["operation"] != job->wireOperation) {
      finish(job,
             error("invalid_response",
                   "Worker response operation does not match the request."));
      return;
    }
    const auto status = response["status"].toString();
    if (status != "ok" && status != "error" && status != "cancelled" &&
        status != "budget_exceeded") {
      finish(job, error("invalid_response",
                        "Worker response has no terminal status."));
      return;
    }
    const auto code = response["error"].toObject()["code"].toString();
    const bool admissionFailure =
        code == "queue_full" || code == "transport_unavailable" ||
        code == "invalid_frame" || code == "invalid_json";
    const bool opening =
        job->kind == Command && job->spec.operation == "open" && status == "ok";
    if (!admissionFailure && status != "cancelled") {
      if (!response["revision"].isString() ||
          response["revision"].toString().isEmpty() ||
          !response["project_id"].isString() ||
          (!opening && response["project_id"].toString() != job->wireProject)) {
        finish(job,
               error("stale_project",
                     "Worker response belongs to a different query context."));
        return;
      }
      const auto nextRevision = response["revision"].toString();
      const auto nextProject = response["project_id"].toString();
      const bool changed =
          opening || revision != nextRevision || project != nextProject;
      revision = nextRevision;
      project = nextProject;
      if (opening)
        retire(job, "session_changed");
      else if (changed)
        cache.clear();
      if (changed) {
        const QPointer<QueryService> guard(q);
        emit q->contextChanged();
        if (!guard || active != job || !jobs.contains(job->id))
          return;
      }
    } else {
      response["project_id"] = project;
      response["revision"] = revision;
    }

    if (job->kind == Graph && !job->cancelRequested) {
      if (status == "error" && code == "stale_layout" &&
          job->graphRetries++ == 0) {
        job->phase = Summary;
        job->summary = {};
        dispatch(job, "cfg_summary",
                 {{"address", job->spec.payload["address"]}});
        return;
      }
      if (status == "ok") {
        const auto payload = response["payload"].toObject();
        if (!sameAddress(payload["address"], job->spec.payload["address"]) ||
            !payload["layout_revision"].isString() ||
            payload["layout_revision"].toString().isEmpty()) {
          finish(job, error("invalid_graph",
                            "Worker returned an unrelated graph snapshot."));
          return;
        }
        if (job->phase == Summary) {
          // Cursors index one layout's ordered lists. A fresh snapshot must
          // not silently reuse a continuation from the previous layout.
          // These fields are optional: a non-const [] read would insert null
          // and turn an omitted worker default into an invalid cursor.
          const bool continuation =
              job->spec.payload.value("node_offset").toDouble() != 0 ||
              job->spec.payload.value("edge_offset").toDouble() != 0;
          if (continuation && job->spec.payload.value("layout_revision") !=
                                  payload["layout_revision"]) {
            finish(job,
                   error("stale_layout",
                         "Graph pagination requires its original layout."));
            return;
          }
          job->summary = payload;
          job->summaryRevision = revision;
          auto viewport = job->spec.payload;
          viewport["layout_revision"] = payload["layout_revision"];
          job->phase = Viewport;
          dispatch(job, "cfg_viewport", viewport);
          return;
        }
        if (payload["layout_revision"] != job->summary["layout_revision"] ||
            job->summaryRevision != revision) {
          finish(job, error("stale_layout",
                            "Graph viewport no longer matches its summary."));
          return;
        }
        response["payload"] =
            QJsonObject{{"summary", job->summary}, {"viewport", payload}};
      }
    }
    if (job->kind == Read && status == "ok" && cacheable(job->spec.operation)) {
      const auto cleaned = cleanResponse(response);
      const auto cacheKey = key(*job, true);
      const qint64 cost =
          qint64(QJsonDocument(cleaned).toJson(QJsonDocument::Compact).size()) *
              4 +
          cacheKey.size() + 256;
      if (cost <= cache.maxCost())
        cache.insert(cacheKey, new QJsonObject(cleaned), int(cost));
    }
    finish(job, std::move(response));
  }
};

QueryService::QueryService(Sender sender, QObject *parent)
    : QueryService(std::move(sender), Limits{}, parent) {}
QueryService::QueryService(Sender sender, Limits limits, QObject *parent)
    : QObject(parent),
      state_(std::make_unique<State>(this, std::move(sender), limits)) {}
QueryService::~QueryService() = default;

QueryService::SubscriptionId
QueryService::subscribe(QuerySpec spec, QObject *owner, Completion complete) {
  return state_->add(std::move(spec), State::Read, owner, std::move(complete));
}
QueryService::SubscriptionId QueryService::graphViewport(QJsonObject payload,
                                                         QObject *owner,
                                                         Completion complete) {
  return state_->add({"cfg_viewport", std::move(payload)}, State::Graph, owner,
                     std::move(complete));
}
QueryService::SubscriptionId QueryService::enqueueCommand(QString operation,
                                                          QJsonObject payload,
                                                          QObject *owner,
                                                          Completion complete) {
  return state_->add({std::move(operation), std::move(payload)}, State::Command,
                     owner, std::move(complete));
}
void QueryService::unsubscribe(SubscriptionId id) {
  state_->detach(id);
  state_->notify();
}
void QueryService::unsubscribeOwner(QObject *owner) {
  const auto ids = state_->subscriptions.keys();
  for (const auto id : ids)
    if (state_->subscriptions.value(id).ownerKey == owner)
      state_->detach(id);
  state_->notify();
}
void QueryService::cancelReads() {
  const auto ids = state_->subscriptions.keys();
  for (const auto id : ids) {
    const auto job = state_->jobs.value(state_->subscriptions.value(id).job);
    if (job && job->kind != State::Command)
      state_->detach(id);
  }
  state_->notify();
}
void QueryService::resetSession() {
  state_->available = false;
  state_->project.clear();
  state_->revision = "0";
  state_->retire({}, "worker_stopped");
  const QPointer<QueryService> guard(this);
  emit contextChanged();
  if (guard)
    state_->notify();
}
void QueryService::setAvailable(bool available) {
  if (state_->available == available)
    return;
  if (!available) {
    resetSession();
    return;
  }
  state_->available = true;
  state_->schedulePump();
  state_->notify();
}
bool QueryService::available() const { return state_->available; }
QString QueryService::projectId() const { return state_->project; }
QString QueryService::revision() const { return state_->revision; }
quint64 QueryService::sessionEpoch() const { return state_->epoch; }
bool QueryService::hasPending() const { return state_->pending(); }
bool QueryService::hasCommands() const {
  for (const auto &job : std::as_const(state_->jobs))
    if (job->kind == State::Command)
      return true;
  return false;
}
void QueryService::setCacheBudgetMiB(int mebibytes) {
  state_->cache.setMaxCost(std::clamp(mebibytes, 16, 1024) * 1024 * 1024);
}
void QueryService::receive(const QJsonObject &response) {
  state_->receive(response);
}
