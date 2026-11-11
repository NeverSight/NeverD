#include "QueryService.h"

#include "PageDelta.h"
#include "ResourceLimits.h"
#include "app/UnwindInfoWords.h"

#include <QCache>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace {
using Spec = QueryService::QuerySpec;
using Id = QueryService::SubscriptionId;

// QT_LOGGING_RULES="neverd.queries.debug=true" traces every worker request.
Q_LOGGING_CATEGORY(lcQueries, "neverd.queries", QtWarningMsg)

bool cacheable(const QString &operation) {
  // Listing pages and the overview also change with the background index
  // generation, which does not advance the revision; their views cache them.
  static const QSet<QString> operations{
      "functions", "resolve", "disasm",  "bytes",   "decompile", "strings",
      "segments",  "names",   "regions", "imports", "exports"};
  return operations.contains(operation);
}

bool readable(const QString &operation) {
  static const QSet<QString> operations{"patch_context",
                                        "patch_preview",
                                        "patches",
                                        "patch_source_target",
                                        "patch_source_preview",
                                        "source_patch_context",
                                        "metadata",
                                        "analysis_sections",
                                        "analysis_snapshot",
                                        "code_edit_prepare",
                                        "history",
                                        "annotations",
                                        "contributions",
                                        "contribution_execute",
                                        "cfg",
                                        "cfg_summary",
                                        "cfg_viewport",
                                        "listing",
                                        "overview",
                                        "xrefs",
                                        "kept_renders",
                                        "string_references",
                                        "string_encodings",
                                        "x87_capabilities",
                                        "string_options",
                                        "identify",
                                        "pdb_inspect",
                                        "pdb_reference",
                                        "dwarf_reference",
                                        "map_reference",
                                        "map_inspect",
                                        "symbol_table_reference",
                                        "function_attributes",
                                        "function_type",
                                        "source_type",
                                        "source_variables",
                                        "call_sites",
                                        "stack_frame",
                                        "register_flow",
                                        "local_types",
                                        "local_type",
                                        "local_types_source",
                                        "local_types_fit",
                                        "type_libraries",
                                        "call_paths",
                                        "call_tree",
                                        "search"};
  return operation == QLatin1String(neverd::gui::unwind_info::kOperation) ||
         cacheable(operation) || operations.contains(operation);
}

bool command(const QString &operation) {
  static const QSet<QString> operations{"open",
                                        "analyze",
                                        "signatures_load",
                                        "pdb_load",
                                        "dwarf_load",
                                        "map_load",
                                        "annotation_set",
                                        "rename",
                                        "analysis_section_add",
                                        "analysis_section_remove",
                                        "code_edit",
                                        "function_create",
                                        "function_delete",
                                        "function_edit",
                                        "item_define",
                                        "patch_apply",
                                        "patch_revert",
                                        "patch_export",
                                        "source_patch",
                                        "operand_format",
                                        "save",
                                        "reload",
                                        "undo",
                                        "redo",
                                        "history_reset",
                                        "project_sync",
                                        "string_options",
                                        "contribution_register",
                                        "contribution_unregister",
                                        "local_types_declare",
                                        "local_types_forget",
                                        "type_libraries_set"};
  return operations.contains(operation);
}

// Sort object keys only. Strings (including symbols and 64-bit addresses),
// numbers, array order and absent defaults keep their exact request meaning.
QJsonValue canonical(const QJsonValue &value, int depth, bool &valid) {
  if (depth > ::neverd::limits::gui::kQueryServiceCanonicalDepthLimit) {
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

/// The cfg_summary request of a graph query: its address plus the optional
/// node metrics that size content-fitted nodes.
QJsonObject summaryPayload(const QJsonObject &viewport) {
  QJsonObject payload{{"address", viewport["address"]}};
  if (viewport.contains("metrics"))
    payload["metrics"] = viewport["metrics"];
  return payload;
}

/// The commands after which the views that show pages read them again.
bool changesPages(const QString &operation) {
  static const QSet<QString> edits{QStringLiteral("annotation_set"),
                                   QStringLiteral("rename"),
                                   QStringLiteral("code_edit"),
                                   QStringLiteral("function_create"),
                                   QStringLiteral("function_delete"),
                                   QStringLiteral("function_edit"),
                                   QStringLiteral("item_define"),
                                   QStringLiteral("operand_format"),
                                   QStringLiteral("local_types_declare"),
                                   QStringLiteral("local_types_forget"),
                                   QStringLiteral("undo"),
                                   QStringLiteral("redo")};
  return edits.contains(operation);
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
    quint64 id = 0, epoch = 0, fence = 0, listing = 0;
    Kind kind = Read;
    Phase phase = Queued;
    Spec spec;
    QByteArray coalescingKey;
    qsizetype bytes = 0;
    QList<Id> subscribers;
    QString wireId, wireOperation, wireProject, wireRevision;
    bool cancelRequested = false, discoveredAnalysis = false;
    int graphRetries = 0;
    QJsonObject summary, result;
    QJsonObject BackgroundSnapshot;
    QString summaryRevision;
    QElapsedTimer sent;
    /// The page of this read the client keeps, which the worker may answer it
    /// is still (if_none_match).
    QString keptEtag;
    QJsonObject keptResponse;
    /// A command's reads that follow it (the worker's "then"): the pages its
    /// views showed, as they kept them, and what the worker answered.
    struct Follow {
      Spec spec;
      QByteArray keptKey;
      QString keptEtag;
      QJsonObject keptResponse;
      qint64 keptBytes = 0;
    };
    /// What the kept page a read names weighs, and what its answer does.
    qint64 keptBytes = 0, resultBytes = 0;
    QList<Follow> follows;
    QJsonArray followed;
  };
  /// A page as the worker last sent it, under what names it there, and the
  /// revision a read last found it at.
  struct KeptPage {
    QString etag;
    QJsonObject response;
    QString revision;
    quint64 read = 0;
    /// What it weighs: the bytes it came in, and those of what changed of it
    /// since.
    qint64 bytes = 0;
  };
  /// A reply as the cache keeps it, with what it weighs.
  struct Cached {
    QJsonObject response;
    qint64 bytes = 0;
  };
  quint64 keptReads = 0;
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
  bool available = false, pumpScheduled = false, analysisComplete = false;
  /// The worker sends what changed of a kept page (page_delta).
  bool pageDeltas = false;
  quint64 epoch = 1, fence = 0, nextJob = 0;
  /// Advances when background analysis changes what reads list without a
  /// new revision; a read made before answers no read made after.
  quint64 listing = 0;
  Id nextSubscription = 0;
  QString project, revision = "0";
  bool localNamesOnly = false;
  qsizetype queuedBytes = 0;
  QHash<quint64, JobPtr> jobs;
  QHash<Id, Subscription> subscriptions;
  QHash<QObject *, QMetaObject::Connection> owners;
  QQueue<quint64> queue;
  JobPtr active;
  /// What the answer a subscriber is being given weighs (deliveredBytes).
  qint64 delivering = 0;
  QCache<QByteArray, Cached> cache;
  QCache<QByteArray, KeptPage> keptPages{
      ::neverd::limits::gui::kQueryServiceKeptPages};

  State(QueryService *service, Sender send, Limits budget)
      : q(service), sender(std::move(send)), limits(budget),
        cache(std::clamp(
            budget.cacheBytes, 0,
            ::neverd::limits::gui::kQueryServiceStateClampArgument3)) {
    limits.maxJobs = std::max<qsizetype>(1, limits.maxJobs);
    limits.maxSubscriptions = std::max<qsizetype>(1, limits.maxSubscriptions);
    limits.maxQueuedBytes = std::max<qsizetype>(1, limits.maxQueuedBytes);
    limits.maxRequestBytes = std::clamp<qsizetype>(
        limits.maxRequestBytes, 1,
        ::neverd::limits::gui::
            kQueryServiceStateLimitsMaxRequestBytesClampArgument3);
  }

  bool pending() const { return !jobs.isEmpty() || !subscriptions.isEmpty(); }

  /// What \p cleaned weighs: its size on the wire where the transport said it
  /// (\p bytes), else what writing it out gives.
  static qint64 weight(const QJsonObject &cleaned, qint64 bytes) {
    return bytes > 0 ? bytes
                     : qint64(QJsonDocument(cleaned)
                                  .toJson(QJsonDocument::Compact)
                                  .size());
  }

  /// Keeps \p cleaned, of \p bytes, as the answer to reads under \p name,
  /// where the cache has room for one of its weight.
  void keepAnswer(const QByteArray &name, const QJsonObject &cleaned,
                  qint64 bytes) {
    const qint64 cost = bytes * 4 + name.size() + 256;
    if (cost <= cache.maxCost())
      cache.insert(name, new Cached{cleaned, bytes}, int(cost));
  }

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

  /// A source page's place apart from the revision it was read at, and apart
  /// from the session: a replica's dispatcher starts one at every change of
  /// its owner's, and a page stands only where the worker that named it says
  /// so, whose names no other worker gives.  Nor does what the listing reads
  /// move it: an edit moves the listing's generation, and the worker answers
  /// for the page by its name whatever moved.
  QByteArray keptPageKey(const Job &job) const {
    return QJsonDocument(
               QJsonArray{project, job.spec.operation, job.spec.payload})
        .toJson(QJsonDocument::Compact);
  }

  QByteArray key(const Job &job, bool forCache) const {
    QJsonArray parts{QString::number(epoch),
                     project,
                     forCache ? revision : QString::number(job.fence),
                     QString::number(job.listing),
                     job.spec.operation,
                     job.spec.payload};
    if (!forCache) {
      parts.append(int(job.kind));
      parts.append(int(job.spec.policy));
      parts.append(job.spec.expectedRevision);
    }
    return QJsonDocument(parts).toJson(QJsonDocument::Compact);
  }

  /// What a read of \p spec made now is cached under (key).
  QByteArray cacheKey(const Spec &spec) const {
    Job job;
    job.listing = listing;
    job.spec = spec;
    return key(job, true);
  }

  /// The reads of the pages read at this revision, the last read first, which
  /// \p job's command carries in \p payload: the views that show them read
  /// them again after it.
  void follow(const JobPtr &job, QJsonObject &payload) {
    std::vector<std::pair<quint64, QByteArray>> shown;
    for (const auto &name : keptPages.keys())
      if (const auto *kept = keptPages.object(name);
          kept && kept->revision == revision)
        shown.emplace_back(kept->read, name);
    std::sort(shown.begin(), shown.end(), std::greater<>());
    QJsonArray reads;
    for (const auto &[read, name] : shown) {
      if (reads.size() >= ::neverd::limits::gui::kQueryServiceThenReads)
        break;
      const auto parts = QJsonDocument::fromJson(name).array();
      const auto *kept = keptPages.object(name);
      if (!kept || parts.size() != 3 || parts[0].toString() != project ||
          parts[1].toString() != QLatin1String("decompile"))
        continue;
      Job::Follow follow;
      follow.spec.operation = QStringLiteral("decompile");
      follow.spec.payload = parts[2].toObject();
      follow.keptKey = name;
      follow.keptEtag = kept->etag;
      follow.keptResponse = kept->response;
      follow.keptBytes = kept->bytes;
      auto wire = follow.spec.payload;
      wire["if_none_match"] = kept->etag;
      if (pageDeltas)
        wire["page_delta"] = true;
      reads.append(
          QJsonObject{{"operation", follow.spec.operation}, {"payload", wire}});
      job->follows.append(std::move(follow));
    }
    if (!reads.isEmpty())
      payload["then"] = reads;
  }

  /// The page \p answer gives of \p follow's kept one, kept and cached as a
  /// read of it made now; none where the answer does not take that page.
  void takeFollowed(const Job::Follow &follow, const QJsonObject &answer) {
    if (answer.value("status") != QLatin1String("ok"))
      return;
    const auto payload = answer.value("payload").toObject();
    const bool delta = payload.contains("delta");
    QJsonObject response;
    // The page weighs what it did and what changed of it does, where the
    // first is known: the answer came inside its edit's reply.
    qint64 bytes = 0;
    if (payload.value("unchanged").toBool() || delta) {
      if (follow.keptEtag.isEmpty() ||
          payload.value(delta ? "base" : "etag").toString() != follow.keptEtag)
        return;
      if (follow.keptBytes > 0)
        bytes = follow.keptBytes +
                (delta ? qint64(QJsonDocument(payload.value("delta").toArray())
                                    .toJson(QJsonDocument::Compact)
                                    .size())
                       : 0);
      const auto applied =
          delta
              ? page_delta::apply(
                    follow.keptResponse.value("payload").toObject(),
                    payload.value("delta").toArray())
              : std::optional(follow.keptResponse.value("payload").toObject());
      if (!applied)
        return;
      auto page = *applied;
      for (const char *field : {"project_id", "revision", "etag"})
        if (payload.contains(QLatin1String(field)))
          page[QLatin1String(field)] = payload.value(QLatin1String(field));
      response = follow.keptResponse;
      response["payload"] = page;
    } else {
      if (!payload.contains("etag"))
        return;
      response = QJsonObject{
          {"protocol_major", 1}, {"type", "response"}, {"payload", payload}};
    }
    response["status"] = QStringLiteral("ok");
    response["operation"] = follow.spec.operation;
    response["project_id"] = project;
    response["revision"] = revision;
    const auto cleaned = cleanResponse(response);
    bytes = weight(cleaned, bytes);
    keptPages.insert(follow.keptKey,
                     new KeptPage{payload.value("etag").toString(), cleaned,
                                  revision, ++keptReads, bytes});
    keepAnswer(cacheKey(follow.spec), cleaned, bytes);
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

  /// Whether \p job reads a page the client keeps and names to the worker: a
  /// source page, or a listing's own page (a jump or a refresh, read as
  /// visible), whose next read after an edit is what changed of it.  The pages
  /// scrolling adds to a listing are not kept.
  static bool keepsPage(const Job &job) {
    return job.kind == Read &&
           (job.spec.operation == QLatin1String("decompile") ||
            (job.spec.operation == QLatin1String("listing") &&
             job.spec.priority == Spec::Visible));
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
    candidate->listing = listing;
    candidate->kind = kind;
    candidate->spec = std::move(spec);
    candidate->coalescingKey = key(*candidate, false);
    JobPtr job;
    bool created = false;
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
      created = true;
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
    if (candidate && candidate->spec.priority == Spec::Visible)
      job->spec.priority = Spec::Visible;
    if (!owners.contains(owner))
      owners.insert(
          owner, QObject::connect(owner, &QObject::destroyed, q, [this, owner] {
            q->unsubscribeOwner(owner);
          }));
    // A read the cache answers takes no turn behind the reads running or
    // queued: only an accepted command ahead of it could change its answer.
    if (created && kind == Read && job->spec.policy == Spec::Latest &&
        cacheable(job->spec.operation) && !commandAhead(job->id))
      if (const auto *cached = cache.object(key(*job, true))) {
        queue.removeAll(job->id);
        job->resultBytes = cached->bytes;
        finish(job, cached->response);
        notify();
        return id;
      }
    schedulePump();
    notify();
    return id;
  }

  /// Whether an accepted command runs, or waits ahead of the job \p id.
  bool commandAhead(quint64 id) const {
    if (active && active->kind == Command)
      return true;
    for (const auto queued : queue) {
      if (queued == id)
        return false;
      const auto job = jobs.value(queued);
      if (job && job->kind == Command)
        return true;
    }
    return false;
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
    if (job->phase == Queued) {
      removeJob(job);
      schedulePump();
    } else if (job->phase == Delivering) {
      // Preserve the delivery barrier even if all subscribers have detached.
      // Analysis discovery is published only after this job has finished.
      return;
    } else if (!job->cancelRequested) {
      job->cancelRequested = true;
      // The administrative ACK has a different ID and is deliberately not
      // owned. Only the original request's terminal response releases active.
      sender("cancel", {{"request_id", job->wireId}}, {});
    }
  }

  void dispatch(const JobPtr &job, const QString &operation,
                const QJsonObject &payload) {
    job->BackgroundSnapshot = {};
    job->wireOperation = operation;
    job->wireProject = project;
    job->wireRevision = revision;
    const auto expected =
        job->spec.policy == Spec::Exact ? job->spec.expectedRevision : revision;
    job->wireId = sender ? sender(operation, payload, expected) : QString{};
    job->sent.start();
    qCDebug(lcQueries).noquote()
        << "send" << operation << job->wireId << "queued" << queue.size();
    if (job->wireId.isEmpty())
      finish(job, error("transport_unavailable",
                        "Analysis worker is not available."));
  }

  /// Answers the reads that wait which the cache answers, up to the first
  /// command that waits: a read no accepted command is ahead of takes no turn
  /// behind the one being read.
  void answerWaiting() {
    if (!available || (active && active->kind == Command))
      return;
    const auto waiting = queue;
    for (const auto id : waiting) {
      const auto job = jobs.value(id);
      if (!job)
        continue;
      if (job->kind == Command)
        break;
      if (job->kind != Read || job->epoch != epoch ||
          job->spec.policy != Spec::Latest || !cacheable(job->spec.operation))
        continue;
      if (const auto *cached = cache.object(key(*job, true))) {
        queue.removeAll(id);
        job->resultBytes = cached->bytes;
        finish(job, cached->response);
      }
    }
  }

  void pump() {
    answerWaiting();
    // A job answered without the worker takes no turn: the next is sent at
    // once, and their answers are given in the order they were made.
    while (!active && !queue.isEmpty() && available) {
      // A navigation reply can enqueue its viewport behind inventory queries.
      // Prefer that visible content, but never cross an accepted command.
      qsizetype next = 0;
      for (qsizetype index = 0; index < queue.size(); ++index) {
        const auto queued = jobs.value(queue[index]);
        if (!queued)
          continue;
        if (queued->kind == Command)
          break;
        if (queued->spec.priority == Spec::Visible) {
          next = index;
          break;
        }
      }
      const auto job = jobs.value(queue.takeAt(next));
      if (!job)
        continue;
      if (job->epoch != epoch) {
        finish(job, error("session_changed", "The query session changed."));
        continue;
      }
      if (job->spec.policy == Spec::Exact &&
          job->spec.expectedRevision != revision) {
        finish(job,
               error("stale_revision",
                     "Project revision changed; refresh before retrying."));
        continue;
      }
      if (job->kind == Read && cacheable(job->spec.operation)) {
        if (const auto *cached = cache.object(key(*job, true))) {
          job->resultBytes = cached->bytes;
          finish(job, cached->response);
          continue;
        }
      }
      active = job;
      if (job->kind == Command)
        cache.clear(); // A save/registry command need not advance image
                       // revision.
      job->phase = job->kind == Graph ? Summary : Reading;
      auto payload = job->kind == Graph ? summaryPayload(job->spec.payload)
                                        : job->spec.payload;
      // An edit carries the reads its views make after it.
      if (job->kind == Command && changesPages(job->spec.operation))
        follow(job, payload);
      if (keepsPage(*job)) {
        // A page kept from an earlier revision is named; a worker that still
        // holds its document answers that it stands rather than sending it.
        const auto *kept = keptPages.object(keptPageKey(*job));
        job->keptEtag = kept ? kept->etag : QString();
        job->keptResponse = kept ? kept->response : QJsonObject();
        job->keptBytes = kept ? kept->bytes : 0;
        payload["if_none_match"] = job->keptEtag;
        // One that keeps the document sends what changed of it instead.
        if (pageDeltas && !job->keptEtag.isEmpty())
          payload["page_delta"] = true;
      }
      dispatch(job,
               job->kind == Graph ? QString("cfg_summary")
                                  : job->spec.operation,
               payload);
    }
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
        job->resultBytes = 0;
        // Another job may be the one being read: an answer the cache gave is
        // delivered beside it.
        if (active == job)
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
      if (subscription.complete) {
        delivering = job->resultBytes;
        subscription.complete(response);
        if (!guard)
          return;
        delivering = 0;
      }
    }
    removeJob(job);
    if (!job->BackgroundSnapshot.isEmpty() && job->epoch == epoch &&
        job->BackgroundSnapshot.value("project_id").toString() == project &&
        job->BackgroundSnapshot.value("revision").toString() == revision) {
      emit q->backgroundSnapshotReady(job->BackgroundSnapshot);
      if (!guard)
        return;
    }
    // The pages its views showed, as the edit's reply read them, answer the
    // reads those views make next, at the listing the reply's state left.
    if (job->kind == Command && job->epoch == epoch &&
        job->result["status"] == "ok")
      for (qsizetype at = 0;
           at < job->follows.size() && at < job->followed.size(); ++at)
        takeFollowed(job->follows[at], job->followed[at].toObject());
    if (job->discoveredAnalysis && job->epoch == epoch) {
      const auto discoveredEpoch = epoch;
      QTimer::singleShot(0, q, [this, discoveredEpoch] {
        if (epoch == discoveredEpoch && analysisComplete)
          emit q->analysisCompleted();
      });
    }
    // The next job is sent now, ahead of the turns the answers just made take
    // to be given: the worker reads it while their views take them in.
    pump();
    notify();
  }

  void retire(const JobPtr &keep, const QString &code) {
    ++epoch;
    ++fence;
    analysisComplete = false;
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

  void receive(QJsonObject response, qint64 bytes) {
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
    if (status == "progress") {
      emit q->loadProgress(response.value("payload").toObject());
      return;
    }
    if (response.contains("then")) {
      if (job->kind == Command && status == "ok")
        job->followed = response.value("then").toArray();
      response.remove("then");
    }
    qCDebug(lcQueries).noquote() << "done" << job->wireOperation << job->wireId
                                 << status << job->sent.elapsed() << "ms";
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
        localNamesOnly = !opening && status == "ok" &&
                         job->spec.operation == "code_edit" &&
                         job->spec.payload.value("kind") == "name" &&
                         response.value("payload")
                             .toObject()
                             .value("local_names_only")
                             .toBool();
        const QPointer<QueryService> guard(q);
        emit q->contextChanged();
        if (!guard || active != job || !jobs.contains(job->id))
          return;
      }
    } else {
      response["project_id"] = project;
      response["revision"] = revision;
    }

    // This is an executor fact, including a view error after successful
    // analysis. Administrative and uncorrelated replies cannot establish it.
    if (job->kind == Command && status == "ok" &&
        (job->spec.operation == "analysis_section_add" ||
         job->spec.operation == "analysis_section_remove" ||
         (job->spec.operation == "reload" &&
          response.value("analysis_state") == "not_analyzed")))
      analysisComplete = false;
    if (!admissionFailure && status != "cancelled" && !project.isEmpty() &&
        response.value("analysis_state") == "complete" && !analysisComplete) {
      analysisComplete = true;
      job->discoveredAnalysis = true;
    }

    if (!admissionFailure && status != "cancelled" && !project.isEmpty() &&
        response.value("background").isObject() &&
        response.value("background_sequence").isString()) {
      job->BackgroundSnapshot = {
          {"project_id", project},
          {"revision", revision},
          {"background", response.value("background")},
          {"background_sequence", response.value("background_sequence")}};
    }

    if (job->kind == Graph && !job->cancelRequested) {
      if (status == "error" && code == "stale_layout" &&
          job->graphRetries++ == 0) {
        job->phase = Summary;
        job->summary = {};
        dispatch(job, "cfg_summary", summaryPayload(job->spec.payload));
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
    // What the answer weighs as it is given: the reply's bytes, or those of
    // the kept page it was applied to with them; none where they are unknown.
    qint64 answerBytes = bytes;
    if (keepsPage(*job) && status == "ok") {
      const auto payload = response.value("payload").toObject();
      const bool delta = payload.contains("delta");
      if (payload.value("unchanged").toBool() || delta) {
        // The kept page, as of this reply, with what changed applied.
        auto kept = job->keptResponse;
        std::optional<QJsonObject> applied;
        if (!job->keptEtag.isEmpty() &&
            payload.value(delta ? "base" : "etag").toString() == job->keptEtag)
          applied = delta ? page_delta::apply(kept.value("payload").toObject(),
                                              payload.value("delta").toArray())
                          : kept.value("payload").toObject();
        if (!applied) {
          // A reply the kept page does not take forgets that page, and the
          // page is read whole, once.
          keptPages.remove(keptPageKey(*job));
          qCWarning(lcQueries).noquote()
              << "kept page" << job->keptEtag << "does not take"
              << (delta ? "the changes" : "the word") << "of" << job->wireId;
          if (!job->keptEtag.isEmpty() && !job->cancelRequested) {
            job->keptEtag.clear();
            job->keptResponse = {};
            auto whole = job->spec.payload;
            whole["if_none_match"] = QString();
            dispatch(job, job->spec.operation, whole);
            return;
          }
          finish(job, error("invalid_response",
                            "Worker named a page this client does not keep."));
          return;
        }
        auto keptPayload = std::move(*applied);
        for (const char *field : {"project_id", "revision", "etag"})
          if (payload.contains(QLatin1String(field)))
            keptPayload[QLatin1String(field)] =
                payload.value(QLatin1String(field));
        kept["payload"] = keptPayload;
        for (const char *field :
             {"request_id", "project_id", "revision", "analysis_state",
              "background", "background_sequence"}) {
          const QLatin1String name(field);
          if (response.contains(name))
            kept[name] = response.value(name);
          else
            kept.remove(name);
        }
        response = kept;
        // The page it was, and what changed of it, where both are known.
        answerBytes = job->keptBytes > 0 && (!delta || bytes > 0)
                          ? job->keptBytes + (delta ? bytes : 0)
                          : 0;
        if (delta) {
          const auto cleaned = cleanResponse(response);
          answerBytes = weight(cleaned, answerBytes);
          keptPages.insert(keptPageKey(*job),
                           new KeptPage{payload.value("etag").toString(),
                                        cleaned, revision, ++keptReads,
                                        answerBytes});
        } else if (auto *page = keptPages.object(keptPageKey(*job))) {
          page->revision = revision;
          page->read = ++keptReads;
        }
      } else if (payload.contains("etag")) {
        const auto cleaned = cleanResponse(response);
        answerBytes = weight(cleaned, answerBytes);
        keptPages.insert(keptPageKey(*job),
                         new KeptPage{payload.value("etag").toString(), cleaned,
                                      revision, ++keptReads, answerBytes});
      }
    }
    if (job->kind == Read && status == "ok" && cacheable(job->spec.operation)) {
      const auto cleaned = cleanResponse(response);
      answerBytes = weight(cleaned, answerBytes);
      keepAnswer(key(*job, true), cleaned, answerBytes);
    }
    job->resultBytes = answerBytes;
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
QueryService::SubscriptionId
QueryService::enqueueCommand(QString operation, QJsonObject payload,
                             QObject *owner, Completion complete,
                             QString expectedRevision) {
  QuerySpec spec{std::move(operation), std::move(payload)};
  if (!expectedRevision.isEmpty()) {
    spec.policy = QuerySpec::Exact;
    spec.expectedRevision = std::move(expectedRevision);
  }
  return state_->add(std::move(spec), State::Command, owner,
                     std::move(complete));
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
void QueryService::cancelReads(const QSet<QObject *> &keep) {
  const auto ids = state_->subscriptions.keys();
  for (const auto id : ids) {
    const auto &subscription = state_->subscriptions.value(id);
    const auto job = state_->jobs.value(subscription.job);
    if (job && job->kind != State::Command &&
        !keep.contains(subscription.ownerKey))
      state_->detach(id);
  }
  state_->notify();
}
void QueryService::resetSession() {
  state_->available = false;
  state_->project.clear();
  state_->revision = "0";
  state_->localNamesOnly = false;
  state_->retire({}, "worker_stopped");
  const QPointer<QueryService> guard(this);
  emit contextChanged();
  if (guard)
    state_->notify();
}
void QueryService::setPageDeltas(bool deltas) { state_->pageDeltas = deltas; }

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
void QueryService::resetReadContext(const QString &project,
                                    const QString &revision) {
  resetSession();
  state_->project = project;
  state_->revision = revision;
  setAvailable(!project.isEmpty());
}
bool QueryService::available() const { return state_->available; }
QString QueryService::projectId() const { return state_->project; }
QString QueryService::revision() const { return state_->revision; }
bool QueryService::localNamesOnly() const { return state_->localNamesOnly; }
quint64 QueryService::sessionEpoch() const { return state_->epoch; }
bool QueryService::analysisComplete() const { return state_->analysisComplete; }
bool QueryService::hasPending() const { return state_->pending(); }
qsizetype QueryService::pendingReadCount() const {
  qsizetype count = 0;
  for (const auto &job : std::as_const(state_->jobs))
    if (job->kind != State::Command && !job->subscribers.isEmpty())
      ++count;
  return count;
}
bool QueryService::hasCommands() const {
  for (const auto &job : std::as_const(state_->jobs))
    if (job->kind == State::Command)
      return true;
  return false;
}
void QueryService::listingChanged() {
  ++state_->listing;
  state_->cache.clear();
}
void QueryService::setCacheBudgetMiB(int mebibytes) {
  state_->cache.setMaxCost(
      std::clamp(
          mebibytes, 16,
          ::neverd::limits::gui::kQueryServiceSetCacheBudgetMiBClampArgument3) *
      1024 * 1024);
}
qint64 QueryService::deliveredBytes() const { return state_->delivering; }
void QueryService::receive(const QJsonObject &response, qint64 bytes) {
  state_->receive(response, bytes);
}
