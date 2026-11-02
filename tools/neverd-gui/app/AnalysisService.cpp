#include "AnalysisService.h"

#include <QJsonArray>
#include <QTimer>
#include <utility>

namespace neverd::gui {
AnalysisService::AnalysisService(QueryService &owner, QString workerPath,
                                 QObject *parent)
    : QObject(parent), owner_(owner),
      queries_([this](const QString &op, const QJsonObject &payload,
                      const QString &) { return send(op, payload); }),
      workerPath_(std::move(workerPath)) {
  connect(&owner_, &QueryService::contextChanged, this,
          &AnalysisService::reset);
  connect(&client_, &EngineClient::message, this, &AnalysisService::receive);
  connect(&client_, &EngineClient::diagnostic, this,
          &AnalysisService::diagnostic);
  connect(&client_, &EngineClient::failure, this, [this](const QString &text) {
    fail(QStringLiteral("analysis_failed"), text);
  });
  connect(&client_, &EngineClient::stopped, this, [this] {
    ready_ = false;
    if (!active_.isEmpty())
      fail(QStringLiteral("analysis_failed"), tr("Analysis worker stopped."));
  });
  queries_.resetReadContext(owner_.projectId(), owner_.revision());
}

AnalysisService::~AnalysisService() {
  disconnect(&client_, nullptr, this, nullptr);
  owner_.unsubscribeOwner(this);
}

bool AnalysisService::handles(const QString &operation) {
  return operation == QLatin1String("decompile") ||
         operation == QLatin1String("cfg") ||
         operation == QLatin1String("cfg_summary") ||
         operation == QLatin1String("cfg_viewport");
}

void AnalysisService::stopWorker() {
  ++serial_;
  if (snapshotRequest_)
    owner_.unsubscribe(std::exchange(snapshotRequest_, 0));
  wire_.clear();
  restore_.clear();
  ready_ = false;
  startingReplica_ = false;
  refreshingNames_ = false;
  snapshot_ = {};
  client_.stop();
}

void AnalysisService::reset() {
  const bool retain = ready_ && active_.isEmpty() && updatesNames_ &&
                      owner_.localNamesOnly() &&
                      queries_.projectId() == owner_.projectId();
  active_.clear();
  if (retain) {
    ++serial_;
    ready_ = false;
    refreshingNames_ = true;
  } else
    stopWorker();
  queries_.resetReadContext(owner_.projectId(), owner_.revision());
}

QString AnalysisService::send(const QString &operation,
                              const QJsonObject &payload) {
  const QString id = QString::number(++next_);
  if (operation == QLatin1String("cancel")) {
    if (payload.value("request_id").toString() == active_) {
      const QString cancelled = std::exchange(active_, {});
      const QString op = operation_;
      stopWorker();
      // Release this dispatcher's active slot only after the executor has
      // been retired. EngineClient filters all replies from its old epoch.
      QTimer::singleShot(0, this, [this, cancelled, op] {
        queries_.receive({{"type", "response"},
                          {"request_id", cancelled},
                          {"operation", op},
                          {"status", "cancelled"}});
      });
    }
    return id;
  }
  active_ = id;
  operation_ = operation;
  payload_ = payload;
  const auto serial = serial_;
  // Sender replies must be asynchronous, including validation failures.
  QTimer::singleShot(0, this, [this, serial, id] {
    if (serial != serial_ || id != active_)
      return;
    if (!handles(operation_)) {
      fail(QStringLiteral("unsupported"),
           tr("Only analysis reads are allowed."));
      return;
    }
    if (ready_) {
      dispatch();
      return;
    }
    requestSnapshot();
  });
  return id;
}

void AnalysisService::setSnapshotBlocked(bool blocked) {
  snapshotBlocked_ = blocked;
  if (!blocked)
    requestSnapshot();
}

void AnalysisService::requestSnapshot() {
  if (snapshotBlocked_ || startingReplica_ || ready_ || active_.isEmpty() ||
      !handles(operation_))
    return;
  startingReplica_ = true;
  const auto serial = serial_;
  const auto id = active_;
  snapshotRequest_ = owner_.subscribe(
      {QStringLiteral("analysis_snapshot"), {}}, this,
      [this, serial, id](const QJsonObject &response) {
        if (serial != serial_ || active_ != id)
          return;
        snapshotRequest_ = 0;
        if (response.value("status") != QLatin1String("ok")) {
          startingReplica_ = false;
          finish(response);
          return;
        }
        snapshot_ = response.value("payload").toObject();
        if (refreshingNames_)
          restore_ = client_.request(QStringLiteral("analysis_update_names"),
                                     snapshot_, workerRevision_);
        else
          client_.start(workerPath_);
      });
}

void AnalysisService::receive(const QJsonObject &message) {
  if (active_.isEmpty())
    return;
  if (message.value("type") == QLatin1String("hello")) {
    updatesNames_ = message.value("capabilities")
                        .toArray()
                        .contains(QStringLiteral("analysis_update_names"));
    if (!message.value("capabilities")
             .toArray()
             .contains(QStringLiteral("analysis_restore"))) {
      fail(QStringLiteral("unsupported"),
           tr("The worker cannot restore analysis state."));
      return;
    }
    restore_ = client_.request(QStringLiteral("analysis_restore"), snapshot_);
    return;
  }
  if (message.value("type") != QLatin1String("response") ||
      message.value("status") == QLatin1String("progress"))
    return;
  const auto id = message.value("request_id").toString();
  if (!restore_.isEmpty() && id == restore_) {
    restore_.clear();
    startingReplica_ = false;
    if (message.value("status") != QLatin1String("ok")) {
      if (refreshingNames_) {
        // A semantic or external change raced the display-name commit. Keep
        // the existing full-restore contract and its input/sidecar checks.
        refreshingNames_ = false;
        startingReplica_ = true;
        client_.start(workerPath_);
        return;
      }
      finish(message);
      client_.stop();
      return;
    }
    snapshot_ = {};
    refreshingNames_ = false;
    workerRevision_ = message.value("revision").toString();
    ready_ = true;
    dispatch();
  } else if (!wire_.isEmpty() && id == wire_) {
    workerRevision_ = message.value("revision").toString();
    finish(message);
  }
}

void AnalysisService::dispatch() {
  wire_ = client_.request(operation_, payload_, workerRevision_);
}

void AnalysisService::finish(QJsonObject response) {
  if (active_.isEmpty())
    return;
  response["type"] = "response";
  response["request_id"] = std::exchange(active_, {});
  response["operation"] = operation_;
  response["project_id"] = owner_.projectId();
  response["revision"] = owner_.revision();
  // A replica's local pipeline/revision never advances the project owner's
  // analysis state, revision or function discovery.
  response.remove("analysis_state");
  auto payload = response.value("payload").toObject();
  if (payload.contains("project_id"))
    payload["project_id"] = owner_.projectId();
  if (payload.contains("revision"))
    payload["revision"] = owner_.revision();
  response["payload"] = payload;
  wire_.clear();
  queries_.receive(response);
}

void AnalysisService::fail(const QString &code, const QString &message) {
  startingReplica_ = false;
  finish({{"status", "error"},
          {"error", QJsonObject{{"code", code}, {"message", message}}}});
}

AnalysisPool::AnalysisPool(QueryService &owner, const QString &workerPath,
                           QObject *parent)
    : QObject(parent) {
  for (auto &lane : lanes_) {
    lane.service = std::make_unique<AnalysisService>(owner, workerPath, this);
    // Keep the pool's total response-cache allowance at the single-replica
    // default. Processes start lazily and each keeps its own C API session.
    lane.service->queries().setCacheBudgetMiB(
        QueryService::Limits{}.cacheBytes / (1024 * 1024 * lanes_.size()));
    connect(lane.service.get(), &AnalysisService::diagnostic, this,
            &AnalysisPool::diagnostic);
  }
  connect(&owner, &QueryService::contextChanged, this, [this, &owner] {
    ++preparationSerial_;
    preparing_ = false;
    if (owner.localNamesOnly())
      return;
    for (auto &lane : lanes_)
      lane.function.reset();
    for (const auto &binding : std::as_const(bindings_))
      disconnect(binding.destroyed);
    bindings_.clear();
  });
}

void AnalysisPool::prefetchSource(quint64 address) {
  if (preparing_ || lanes_.front().service->queries().pendingReadCount() != 0)
    return;
  auto &lane = lanes_.front();
  lane.function = address;
  preparing_ = true;
  const auto serial = ++preparationSerial_;
  const QJsonObject payload{
      {"address", QStringLiteral("0x") + QString::number(address, 16)},
      {"representation", QStringLiteral("source")},
      {"source_options", QJsonObject{}},
      {"offset", 0},
      {"limit", 1}};
  lane.service->queries().subscribe({QStringLiteral("decompile"), payload},
                                    &preparationOwner_,
                                    [this, serial](const QJsonObject &) {
                                      if (serial == preparationSerial_)
                                        preparing_ = false;
                                    });
}

void AnalysisPool::setSnapshotBlocked(bool blocked) {
  for (auto &lane : lanes_)
    lane.service->setSnapshotBlocked(blocked);
}

QueryService &AnalysisPool::queries(const QJsonObject &payload,
                                    QObject *owner) {
  bool valid = false;
  const quint64 function =
      payload.value("address").toString().toULongLong(&valid, 16);
  if (!valid)
    return lanes_.front().service->queries();
  if (const auto binding = bindings_.constFind(owner);
      binding != bindings_.cend() && binding->function == function)
    return lanes_[binding->lane].service->queries();
  const auto bind = [&](size_t index) -> QueryService & {
    auto &lane = lanes_[index];
    lane.function = function;
    if (owner) {
      auto it = bindings_.find(owner);
      if (it != bindings_.end()) {
        it->function = function;
        it->lane = index;
      } else {
        const auto destroyed =
            connect(owner, &QObject::destroyed, this,
                    [this, owner] { bindings_.remove(owner); });
        bindings_.insert(owner, {function, index, destroyed});
      }
    }
    return lane.service->queries();
  };
  for (size_t i = 0; i < lanes_.size(); ++i)
    if (lanes_[i].function == function)
      return bind(i);
  // Reuse the in-flight image preparation instead of loading the same large
  // image into another process. A second independent foreground read can
  // still use the other lane once the first has queued behind preparation.
  if (preparing_ && lanes_.front().service->queries().pendingReadCount() == 1)
    return bind(0);
  // Reuse a loaded idle replica before starting another process. Only
  // independent work that overlaps needs the second worker and its memory.
  for (const bool ready : {true, false})
    for (size_t i = 0; i < lanes_.size(); ++i)
      if (lanes_[i].service->queries().pendingReadCount() == 0 &&
          lanes_[i].service->ready() == ready)
        return bind(i);
  // Both workers are busy. Their existing dispatchers retain bounded queues,
  // request deduplication and atomic summary/viewport graph transactions.
  size_t selected = 0;
  for (size_t i = 1; i < lanes_.size(); ++i)
    if (lanes_[i].service->queries().pendingReadCount() <
        lanes_[selected].service->queries().pendingReadCount())
      selected = i;
  return bind(selected);
}

QSet<QObject *> AnalysisPool::snapshotOwners() const {
  QSet<QObject *> owners;
  for (const auto &lane : lanes_)
    owners.insert(lane.service.get());
  return owners;
}

void AnalysisPool::unsubscribeOwner(QObject *owner) {
  if (const auto binding = bindings_.find(owner); binding != bindings_.end()) {
    disconnect(binding->destroyed);
    bindings_.erase(binding);
  }
  for (auto &lane : lanes_)
    lane.service->queries().unsubscribeOwner(owner);
}

void AnalysisPool::cancelReads(const QSet<QObject *> &keep) {
  // A detached prefetch never completes. Stop treating the first lane's
  // next read as preparation, or independent views would queue behind it.
  if (!keep.contains(&preparationOwner_)) {
    ++preparationSerial_;
    preparing_ = false;
  }
  for (auto &lane : lanes_)
    lane.service->queries().cancelReads(keep);
}
} // namespace neverd::gui
