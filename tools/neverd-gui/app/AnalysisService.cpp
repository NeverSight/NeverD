#include "AnalysisService.h"

#include "../ResourceLimits.h"
#include "CodeView.h"

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
    // Rendering ahead visits every function, and one the worker cannot survive
    // would end every replica of this project the same way.
    renderCrashProject_ = queries_.projectId();
    fail(QStringLiteral("analysis_failed"), text);
  });
  connect(&client_, &EngineClient::stopped, this, [this] {
    ready_ = false;
    refreshingNames_ = false;
    abandoned_.clear();
    renderAheadState_.clear();
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
  forgetKeptRequests();
  if (snapshotRequest_)
    owner_.unsubscribe(std::exchange(snapshotRequest_, 0));
  wire_.clear();
  abandoned_.clear();
  restore_.clear();
  ready_ = false;
  startingReplica_ = false;
  refreshingNames_ = false;
  summariesReady_ = false;
  snapshot_ = {};
  client_.stop();
}

void AnalysisService::reset() {
  // The replica takes a change of names or comments in place, and refuses any
  // other (receive), which restores a fresh one.
  const bool retain = (ready_ || refreshingNames_) && updatesNames_ &&
                      queries_.projectId() == owner_.projectId();
  active_.clear();
  preparedRead_.reset();
  preparedDeclaration_.clear();
  forgetKeptRequests();
  if (retain) {
    ++serial_;
    ready_ = false;
    refreshingNames_ = true;
    // What it is still answering belongs to the state before this change, and
    // the next update replaces any it is taking.
    if (snapshotRequest_)
      owner_.unsubscribe(std::exchange(snapshotRequest_, 0));
    startingReplica_ = false;
    restore_.clear();
    abandonRunningRead();
    snapshot_ = {};
  } else
    stopWorker();
  queries_.resetReadContext(owner_.projectId(), owner_.revision());
}

void AnalysisService::abandonRunningRead() {
  if (wire_.isEmpty())
    return;
  abandoned_ = std::exchange(wire_, {});
  namespace limits = ::neverd::limits::gui;
  const auto grace = std::max<qint64>(
      limits::kAnalysisAbandonedReadMinimumMilliseconds,
      limits::kAnalysisAbandonedReadRestores * restoreMilliseconds_);
  QTimer::singleShot(grace, this, [this, read = abandoned_] {
    if (abandoned_ != read)
      return;
    // Starting over costs less than this read still may.
    const QString waiting = std::exchange(active_, {});
    stopWorker();
    active_ = waiting;
    requestSnapshot();
  });
}

QString AnalysisService::send(const QString &operation,
                              const QJsonObject &payload) {
  const QString id = QString::number(++next_);
  if (operation == QLatin1String("cancel")) {
    if (payload.value("request_id").toString() == active_) {
      const QString cancelled = std::exchange(active_, {});
      const QString op = operation_;
      abandonRunningRead();
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
    if (ownerReadsKept_ && (operation_ == QLatin1String("decompile") ||
                            operation_ == QLatin1String("cfg_summary") ||
                            operation_ == QLatin1String("cfg_viewport"))) {
      readKept();
      return;
    }
    requestSnapshot();
  });
  return id;
}

void AnalysisService::readKept() {
  if (keptState_ == KeptState::Unknown) {
    keptState_ = KeptState::Asked;
    const auto serial = serial_;
    keptStateRequest_ = owner_.subscribe(
        {QStringLiteral("kept_renders"), {}}, this,
        [this, serial](const QJsonObject &response) {
          if (serial != serial_)
            return;
          keptStateRequest_ = 0;
          keptState_ =
              response.value("payload").toObject().value("complete").toBool()
                  ? KeptState::Whole
                  : KeptState::Part;
          if (!active_.isEmpty() && !ready_)
            readKept();
        });
    return;
  }
  if (keptState_ == KeptState::Asked)
    return;
  // Part of the renders leaves a job to go on with, whatever this read finds.
  if (keptState_ == KeptState::Part)
    requestSnapshot();
  auto payload = payload_;
  payload["kept_only"] = true;
  const auto serial = serial_;
  if (keptRead_)
    owner_.unsubscribe(std::exchange(keptRead_, 0));
  keptRead_ = owner_.subscribe(
      {operation_, payload}, this,
      [this, serial, read = active_](const QJsonObject &response) {
        if (serial != serial_)
          return;
        keptRead_ = 0;
        if (read != active_)
          return;
        // A graph the owner has let go of is the query's to ask for again.
        if (response.value("status") == QLatin1String("ok") ||
            response.value("error").toObject().value("code") ==
                QLatin1String("stale_layout")) {
          finish(response);
          return;
        }
        if (ready_)
          dispatch();
        else
          requestSnapshot();
      });
}

void AnalysisService::forgetKeptRequests() {
  if (keptStateRequest_)
    owner_.unsubscribe(std::exchange(keptStateRequest_, 0));
  if (keptRead_)
    owner_.unsubscribe(std::exchange(keptRead_, 0));
  keptState_ = KeptState::Unknown;
}

void AnalysisService::setSnapshotBlocked(bool blocked) {
  snapshotBlocked_ = blocked;
  if (!blocked)
    requestSnapshot();
}

void AnalysisService::prepareRead(quint64 address, const QString &declaration) {
  preparedRead_ = address;
  preparedDeclaration_ = declaration;
  if (ready_)
    sendPreparedRead();
  else
    startReplica();
}

void AnalysisService::sendPreparedRead() {
  // One read the worker runs for nobody at a time: a declaration the dialog
  // reads while the last one renders waits for it, and a later one takes its
  // place, so the view after the edit waits for one render at most.
  if (!preparedRead_ || !abandoned_.isEmpty())
    return;
  const auto address = *std::exchange(preparedRead_, std::nullopt);
  const auto declaration = std::exchange(preparedDeclaration_, {});
  const auto at = QStringLiteral("0x") + QString::number(address, 16);
  // A worker that prepares no function renders no declaration ahead either.
  if (!declaration.isEmpty() && !preparesFunctions_)
    return;
  QJsonObject prepare{{"address", at}};
  if (!declaration.isEmpty())
    prepare.insert(QStringLiteral("declaration"), declaration);
  // It runs to its end as a read let go of does: a read sent meanwhile does not
  // ask for the revision it leaves.  A view would answer from what is kept, and
  // prepare nothing.
  abandoned_ =
      preparesFunctions_
          ? client_.request(QStringLiteral("prepare_function"), prepare)
          : client_.request(QStringLiteral("decompile"),
                            {{"address", at},
                             {"representation", QStringLiteral("source")},
                             {"source_options", QJsonObject{}},
                             {"offset", 0},
                             {"limit", 1}});
}

void AnalysisService::requestSnapshot() {
  if (active_.isEmpty() || !handles(operation_))
    return;
  startReplica();
}

void AnalysisService::startReplica() {
  if (snapshotBlocked_ || startingReplica_ || ready_)
    return;
  startingReplica_ = true;
  const auto serial = serial_;
  // A replica that takes the change in place needs no write to have reached the
  // disk: the owner's state comes with the snapshot (in_memory).
  snapshotRequest_ = owner_.subscribe(
      {QStringLiteral("analysis_snapshot"),
       refreshingNames_ ? QJsonObject{{"in_memory", true}} : QJsonObject{}},
      this, [this, serial](const QJsonObject &response) {
        if (serial != serial_)
          return;
        snapshotRequest_ = 0;
        if (response.value("status") != QLatin1String("ok")) {
          startingReplica_ = false;
          finish(response);
          return;
        }
        snapshot_ = response.value("payload").toObject();
        // An update it is still taking moves the revision: the snapshot itself
        // says what this one applies to.
        if (refreshingNames_)
          restore_ = client_.request(QStringLiteral("analysis_update_names"),
                                     snapshot_);
        else {
          restoring_.start();
          client_.start(workerPath_);
        }
      });
}

void AnalysisService::receive(const QJsonObject &message) {
  // The replica solves the image's call summaries while no read waits.
  if (message.value("type") == QLatin1String("heartbeat")) {
    renderAheadState_ = message.value("background")
                            .toObject()
                            .value("render_ahead")
                            .toObject()
                            .value("state")
                            .toString();
    if (solvesSummaries_ && !summariesReady_ &&
        message.value("background")
                .toObject()
                .value("call_summaries")
                .toObject()
                .value("state")
                .toString() == QLatin1String("ready")) {
      summariesReady_ = true;
      queries_.listingChanged();
      emit sourceRefined();
    }
    return;
  }
  if (message.value("type") == QLatin1String("hello")) {
    updatesNames_ = message.value("capabilities")
                        .toArray()
                        .contains(QStringLiteral("analysis_update_names"));
    solvesSummaries_ = message.value("capabilities")
                           .toArray()
                           .contains(QStringLiteral("prepare_call_summaries"));
    preparesFunctions_ = message.value("capabilities")
                             .toArray()
                             .contains(QStringLiteral("prepare_function"));
    queries_.setPageDeltas(message.value("capabilities")
                               .toArray()
                               .contains(QStringLiteral("page_delta")));
    if (!message.value("capabilities")
             .toArray()
             .contains(QStringLiteral("analysis_restore"))) {
      fail(QStringLiteral("unsupported"),
           tr("The worker cannot restore analysis state."));
      return;
    }
    auto restore = snapshot_;
    if (solvesSummaries_)
      restore["call_summaries"] = true;
    // The job renders what a view reads under no options; one that opens its
    // functions under others would read none of it.
    if (solvesSummaries_ && rendersAhead_ &&
        owner_.projectId() != renderCrashProject_ &&
        SourceDefaults::options(QStringLiteral("source")).isEmpty() &&
        message.value("capabilities")
            .toArray()
            .contains(QStringLiteral("render_ahead")))
      restore["render_ahead"] = true;
    // Any replica reads what earlier sessions rendered; the one that renders
    // adds to it.
    if (message.value("capabilities")
            .toArray()
            .contains(QStringLiteral("render_store")))
      restore["render_store"] = restore.contains(QStringLiteral("render_ahead"))
                                    ? QStringLiteral("write")
                                    : QStringLiteral("read");
    restore_ = client_.request(QStringLiteral("analysis_restore"), restore);
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
        restoring_.start();
        client_.start(workerPath_);
        return;
      }
      finish(message);
      client_.stop();
      return;
    }
    snapshot_ = {};
    if (!refreshingNames_ && restoring_.isValid())
      restoreMilliseconds_ = restoring_.elapsed();
    refreshingNames_ = false;
    workerRevision_ = message.value("revision").toString();
    ready_ = true;
    // The replica answers from here on.
    if (keptRead_)
      owner_.unsubscribe(std::exchange(keptRead_, 0));
    dispatch();
    sendPreparedRead();
  } else if (!wire_.isEmpty() && id == wire_) {
    workerRevision_ = message.value("revision").toString();
    finish(message);
  } else if (!abandoned_.isEmpty() && id == abandoned_) {
    abandoned_.clear();
    workerRevision_ = message.value("revision").toString();
    // A prepared read waited for this one.
    if (ready_)
      sendPreparedRead();
  }
}

void AnalysisService::dispatch() {
  if (active_.isEmpty())
    return;
  // A read that still runs can move the worker's revision before this reaches
  // it.
  wire_ = client_.request(operation_, payload_,
                          abandoned_.isEmpty() ? workerRevision_ : QString());
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
    connect(lane.service.get(), &AnalysisService::sourceRefined, this,
            &AnalysisPool::sourceRefined);
  }
  connect(&owner, &QueryService::contextChanged, this, [this, &owner] {
    ++preparationSerial_;
    preparing_ = false;
    // A function stays with the replica that holds its analysis, which takes
    // a change to the same project in place where it can.
    if (std::exchange(boundProject_, owner.projectId()) == owner.projectId())
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
