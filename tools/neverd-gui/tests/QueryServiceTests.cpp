#include "QueryService.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <memory>

namespace {
using Spec = QueryService::QuerySpec;

struct WireRequest {
  QString id, operation;
  QJsonObject payload;
  QString expectedRevision;
};

struct Harness {
  QObject owner;
  QList<WireRequest> sent;
  quint64 nextWire = 0;
  QueryService service;

  explicit Harness(QueryService::Limits limits = {})
      : service(
            [this](const QString &operation, const QJsonObject &payload,
                   const QString &revision) {
              const auto id = "wire-" + QString::number(++nextWire);
              sent.append({id, operation, payload, revision});
              return id;
            },
            limits) {
    service.setAvailable(true);
  }

  void reply(int index, QJsonObject payload = {}, QString revision = "1",
             QString project = "project-one", QString status = "ok",
             QString code = {}, QString analysisState = {}) {
    const auto request = sent.at(index);
    QJsonObject response{
        {"protocol_major", 1},      {"type", "response"},
        {"request_id", request.id}, {"operation", request.operation},
        {"project_id", project},    {"revision", revision},
        {"status", status},         {"payload", payload}};
    if (!code.isEmpty())
      response["error"] = QJsonObject{{"code", code}, {"message", code}};
    if (!analysisState.isEmpty())
      response["analysis_state"] = analysisState;
    service.receive(response);
  }
};

Spec text(QString address = "0xffff000000000001",
          QString representation = "low") {
  return {"decompile",
          {{"address", address},
           {"representation", representation},
           {"offset", 0},
           {"limit", 512}}};
}

QJsonObject viewport(const QString &address) {
  return {{"address", address}, {"x", 10},         {"y", 20},
          {"width", 640},       {"height", 480},   {"scale", 1},
          {"node_offset", 0},   {"edge_offset", 0}};
}

QJsonObject graph(const QString &address, const QString &layout) {
  return {
      {"address", address},
      {"layout_revision", layout},
      {"nodes", QJsonArray{QJsonObject{{"id", address}, {"address", address}}}},
      {"edges", QJsonArray{}},
      {"complete", true}};
}

QString errorCode(const QJsonObject &response) {
  return response["error"].toObject()["code"].toString();
}
} // namespace

class QueryServiceTests final : public QObject {
  Q_OBJECT

  void establish(Harness &h) {
    h.service.enqueueCommand("open", {{"path", "fixture-one"}}, &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0);
    QTRY_VERIFY(!h.service.hasPending());
    QCOMPARE(h.service.projectId(), "project-one");
    QCOMPARE(h.service.revision(), "1");
    h.sent.clear();
  }

private slots:
  void analysisCompletionSurvivesLastSubscriberCancellationInFlight_data() {
    QTest::addColumn<bool>("graphAfterSummary");
    QTest::newRow("read-cancelled-before-natural-completion") << false;
    QTest::newRow("graph-cancelled-after-summary-discovery") << true;
  }

  void analysisCompletionSurvivesLastSubscriberCancellationInFlight() {
    QFETCH(bool, graphAfterSummary);
    Harness h;
    establish(h);
    QPointer<QueryService> service = &h.service;
    QPointer<QObject> owner = &h.owner;
    QSignalSpy discovered(&h.service, &QueryService::analysisCompleted);
    int cancelledReplies = 0;
    int commandReplies = 0;
    const QString address = "0x1000";
    const auto complete = [&](const QJsonObject &) { ++cancelledReplies; };
    const auto id =
        graphAfterSummary
            ? h.service.graphViewport(viewport(address), &h.owner, complete)
            : h.service.subscribe(text(address), &h.owner, complete);
    QVERIFY(id);
    QTRY_COMPARE(h.sent.size(), 1);
    if (graphAfterSummary) {
      QCOMPARE(h.sent[0].operation, QString("cfg_summary"));
      h.reply(0, graph(address, "layout-1"), "2", "project-one", "ok", {},
              "complete");
      QTRY_COMPARE(h.sent.size(), 2);
      QCOMPARE(h.sent[1].operation, QString("cfg_viewport"));
      QVERIFY(h.service.analysisComplete());
      QCoreApplication::processEvents();
      QCOMPARE(discovered.size(), 0);
    } else {
      QCOMPARE(h.sent[0].operation, QString("decompile"));
      QVERIFY(!h.service.analysisComplete());
    }
    const int executing = graphAfterSummary ? 1 : 0;
    QVERIFY(h.service.enqueueCommand(
        "save", {}, &h.owner, [&](const QJsonObject &response) {
          QCOMPARE(response["status"].toString(), QString("ok"));
          ++commandReplies;
        }));

    h.service.unsubscribe(id);
    QCOMPARE(h.sent.size(), executing + 2);
    const int cancellation = executing + 1;
    QCOMPARE(h.sent[cancellation].operation, QString("cancel"));
    QCOMPARE(h.sent[cancellation].payload["request_id"].toString(),
             h.sent[executing].id);
    QVERIFY(service && owner);
    QVERIFY(h.service.hasPending());
    QVERIFY(h.service.hasCommands());
    QCOMPARE(cancelledReplies, 0);
    QCOMPARE(commandReplies, 0);

    // Even an ACK carrying complete is administrative, belongs to a separate
    // wire ID, and cannot discover analysis or release the active-job barrier.
    h.reply(cancellation, {{"accepted", true}, {"stopped", false}}, "0", "",
            "ok", {}, "complete");
    QCoreApplication::processEvents();
    QCOMPARE(h.service.analysisComplete(), graphAfterSummary);
    QCOMPARE(discovered.size(), 0);
    QCOMPARE(h.sent.size(), executing + 2);
    QCOMPARE(cancelledReplies, 0);
    QCOMPARE(commandReplies, 0);

    // Cancellation detached the reader; the executor may still finish the
    // original request naturally and establish (or retain) the global fact.
    const auto result = graphAfterSummary
                            ? graph(address, "layout-1")
                            : QJsonObject{{"text", "finished after Cancel"}};
    h.reply(executing, result, "2", "project-one", "ok", {}, "complete");
    QVERIFY(h.service.analysisComplete());
    QCOMPARE(discovered.size(), 0);
    QCOMPARE(cancelledReplies, 0);
    QTRY_COMPARE(discovered.size(), 1);
    QTRY_COMPARE(h.sent.size(), executing + 3);
    const int queuedCommand = executing + 2;
    QCOMPARE(h.sent[queuedCommand].operation, QString("save"));
    QCOMPARE(h.sent[queuedCommand].expectedRevision, QString("2"));

    h.reply(queuedCommand, {}, "2", "project-one", "ok", {}, "complete");
    QTRY_COMPARE(commandReplies, 1);
    QTRY_VERIFY(!h.service.hasPending());
    QCoreApplication::processEvents();
    QVERIFY(service && owner);
    QVERIFY(h.service.analysisComplete());
    QCOMPARE(discovered.size(), 1);
    QCOMPARE(cancelledReplies, 0);
    QVERIFY(!h.service.hasCommands());
  }

  void analysisCompletionFollowsAllSubscribersAndDoesNotRepeat() {
    Harness h;
    establish(h);
    QObject external;
    QStringList order;
    connect(&h.service, &QueryService::analysisCompleted, &h.owner,
            [&] { order.append("discovery"); });
    const auto complete = [&](const QString &name) {
      return [&, name](const QJsonObject &) {
        QVERIFY(h.service.analysisComplete());
        order.append(name);
      };
    };
    h.service.subscribe(text(), &h.owner, complete("pane"));
    h.service.subscribe(text(), &external, complete("external"));
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "recovered"}}, "2", "project-one", "ok", {},
            "complete");
    QVERIFY(order.isEmpty());
    QTRY_COMPARE(order, (QStringList{"pane", "external", "discovery"}));

    // Cached reads and later edit revisions cannot rediscover the project.
    h.service.subscribe(text(), &h.owner, complete("cache"));
    QTRY_COMPARE(order.size(), 4);
    QCOMPARE(h.sent.size(), 1);
    h.service.enqueueCommand("annotation_set", {}, &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 2);
    h.reply(1, {}, "3", "project-one", "ok", {}, "complete");
    QTRY_VERIFY(!h.service.hasPending());
    QCOMPARE(order.count("discovery"), 1);
    QVERIFY(h.service.analysisComplete());
  }

  void analysisCompletionCanAccompanyAnUnsupportedView() {
    Harness h;
    establish(h);
    QSignalSpy discovered(&h.service, &QueryService::analysisCompleted);
    QList<QJsonObject> replies;
    h.service.subscribe(text(), &h.owner, [&](const auto &response) {
      replies.append(response);
    });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {}, "2", "project-one", "error", "unsupported_view", "complete");
    QTRY_COMPARE(replies.size(), 1);
    QTRY_COMPARE(discovered.size(), 1);
    QCOMPARE(errorCode(replies[0]), "unsupported_view");
    QVERIFY(h.service.analysisComplete());
  }

  void analysisCompletionRejectsUntrustedReplies_data() {
    QTest::addColumn<QString>("variant");
    for (const auto *variant : {"wire", "operation", "project", "admission",
                                "cancelled", "nonterminal", "missing"})
      QTest::newRow(variant) << QString(variant);
  }

  void analysisCompletionRejectsUntrustedReplies() {
    QFETCH(QString, variant);
    Harness h;
    establish(h);
    QSignalSpy discovered(&h.service, &QueryService::analysisCompleted);
    h.service.subscribe(text(), &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 1);
    QJsonObject response{{"type", "response"},
                         {"request_id", h.sent[0].id},
                         {"operation", h.sent[0].operation},
                         {"status", "ok"},
                         {"project_id", "project-one"},
                         {"revision", "2"},
                         {"analysis_state", "complete"}};
    if (variant == "wire")
      response["request_id"] = "unowned";
    else if (variant == "operation")
      response["operation"] = "cancel";
    else if (variant == "project")
      response["project_id"] = "other-project";
    else if (variant == "admission") {
      response["status"] = "error";
      response["error"] = QJsonObject{{"code", "queue_full"}};
    } else if (variant == "cancelled")
      response["status"] = "cancelled";
    else if (variant == "nonterminal")
      response["status"] = "progress";
    else if (variant == "missing")
      response.remove("analysis_state");
    h.service.receive(response);
    QCoreApplication::processEvents();
    QVERIFY(!h.service.analysisComplete());
    QCOMPARE(discovered.size(), 0);
  }

  void graphAnalysisCompletionWaitsForViewportDelivery() {
    Harness h;
    establish(h);
    QSignalSpy discovered(&h.service, &QueryService::analysisCompleted);
    int replies = 0;
    const QString address = "0x1000";
    h.service.graphViewport(viewport(address), &h.owner, [&](const auto &) {
      QCOMPARE(discovered.size(), 0);
      ++replies;
    });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, graph(address, "layout-1"), "2", "project-one", "ok", {},
            "complete");
    QTRY_COMPARE(h.sent.size(), 2);
    QCoreApplication::processEvents();
    QVERIFY(h.service.analysisComplete());
    QCOMPARE(discovered.size(), 0);
    QCOMPARE(replies, 0);
    h.reply(1, graph(address, "layout-1"), "2", "project-one", "ok", {},
            "complete");
    QTRY_COMPARE(replies, 1);
    QTRY_COMPARE(discovered.size(), 1);
  }

  void analysisCompletionSurvivesLastSubscriberDetachingBeforeDelivery() {
    Harness h;
    establish(h);
    QSignalSpy discovered(&h.service, &QueryService::analysisCompleted);
    int replies = 0;
    auto id =
        h.service.subscribe(text(), &h.owner, [&](const auto &) { ++replies; });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {}, "2", "project-one", "ok", {}, "complete");
    h.service.unsubscribe(id);
    QTRY_COMPARE(discovered.size(), 1);
    QTRY_VERIFY(!h.service.hasPending());
    QCOMPARE(replies, 0);
  }

  void reentrantSubscriberCancellationCannotPublishDuringDelivery() {
    Harness h;
    establish(h);
    QObject external;
    QSignalSpy discovered(&h.service, &QueryService::analysisCompleted);
    int replies = 0;
    QueryService::SubscriptionId peer = 0;
    h.service.subscribe(text(), &h.owner, [&](const auto &) {
      h.service.unsubscribe(peer);
      QCoreApplication::processEvents();
      QCOMPARE(discovered.size(), 0);
      ++replies;
    });
    peer = h.service.subscribe(text(), &external,
                               [&](const auto &) { ++replies; });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {}, "2", "project-one", "ok", {}, "complete");
    QTRY_COMPARE(discovered.size(), 1);
    QCOMPARE(replies, 1);
  }

  void analysisCompletionDoesNotEscapeSessionRetirement() {
    Harness h;
    establish(h);
    QSignalSpy discovered(&h.service, &QueryService::analysisCompleted);
    bool delivered = false;
    h.service.subscribe(text(), &h.owner, [&](const auto &) {
      delivered = true;
      h.service.resetSession();
    });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {}, "2", "project-one", "ok", {}, "complete");
    QTRY_VERIFY(delivered);
    QCoreApplication::processEvents();
    QVERIFY(!h.service.analysisComplete());
    QCOMPARE(discovered.size(), 0);

    h.service.setAvailable(true);
    h.service.enqueueCommand("open", {{"path", "fixture-two"}}, &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 2);
    h.reply(1, {}, "3", "project-two", "ok", {}, "not_analyzed");
    QTRY_VERIFY(!h.service.hasPending());
    h.service.subscribe(text(), &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 3);
    h.reply(2, {}, "4", "project-two", "ok", {}, "complete");
    QTRY_COMPARE(discovered.size(), 1);
    QVERIFY(h.service.analysisComplete());
  }

  void identicalReadsShareOneWireRequestAndCompleteAsynchronously() {
    Harness h;
    establish(h);
    QObject other;
    QList<QJsonObject> a, b;
    const auto first = h.service.subscribe(text(), &h.owner,
                                           [&](const auto &r) { a.append(r); });
    const auto second = h.service.subscribe(
        text(), &other, [&](const auto &r) { b.append(r); });
    QVERIFY(first && second && first != second);
    QVERIFY(a.isEmpty() && b.isEmpty());
    QTRY_COMPARE(h.sent.size(), 1);
    QCOMPARE(h.sent[0].expectedRevision, "1");
    h.reply(0, {{"text", "shared page"}});
    QVERIFY(a.isEmpty() && b.isEmpty());
    QTRY_COMPARE(a.size(), 1);
    QTRY_COMPARE(b.size(), 1);
    QCOMPARE(a[0]["request_id"].toString(), QString::number(first));
    QCOMPARE(b[0]["request_id"].toString(), QString::number(second));
    QCOMPARE(a[0]["payload"], b[0]["payload"]);
    QTRY_VERIFY(!h.service.hasPending());
  }

  void
  objectKeyOrderCoalescesWhileAddressesSymbolsAndPageParametersStayExact() {
    Harness h;
    establish(h);
    QJsonObject first{{"query", "FunctionA"},
                      {"extra", QJsonObject{{"b", 2}, {"a", 1}}}};
    QJsonObject reordered{{"extra", QJsonObject{{"a", 1}, {"b", 2}}},
                          {"query", "FunctionA"}};
    h.service.subscribe({"resolve", first}, &h.owner, {});
    h.service.subscribe({"resolve", reordered}, &h.owner, {});
    h.service.subscribe({"resolve", {{"query", "functiona"}}}, &h.owner, {});
    h.service.subscribe(text(), &h.owner, {});
    auto laterPage = text();
    laterPage.payload["offset"] = 512;
    h.service.subscribe(laterPage, &h.owner, {});
    h.service.subscribe(text("0xffff000000000001", "med"), &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 1);
    QCOMPARE(h.sent[0].payload, first);
    h.reply(0);
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].payload["query"].toString(), "functiona");
    h.reply(1);
    QTRY_COMPARE(h.sent.size(), 3);
    QVERIFY(h.sent[2].payload["address"].isString());
    QCOMPARE(h.sent[2].payload["address"].toString(), "0xffff000000000001");
    h.reply(2);
    QTRY_COMPARE(h.sent.size(), 4);
    QCOMPARE(h.sent[3].payload["offset"].toInt(), 512);
    h.reply(3);
    QTRY_COMPARE(h.sent.size(), 5);
    QCOMPARE(h.sent[4].payload["representation"].toString(), "med");
    h.reply(4);
    QTRY_VERIFY(!h.service.hasPending());
  }

  void unsubscribingOneOwnerDoesNotCancelTheOtherOwner() {
    Harness h;
    establish(h);
    QObject other;
    int firstCalls = 0, secondCalls = 0;
    const auto first = h.service.subscribe(text(), &h.owner,
                                           [&](const auto &) { ++firstCalls; });
    h.service.subscribe(text(), &other, [&](const auto &) { ++secondCalls; });
    QTRY_COMPARE(h.sent.size(), 1);
    h.service.unsubscribe(first);
    QCOMPARE(h.sent.size(), 1);
    h.reply(0);
    QTRY_COMPARE(secondCalls, 1);
    QCOMPARE(firstCalls, 0);
    QCOMPARE(h.sent.size(), 1);
  }

  void lastSubscriberCancellationWaitsForTheOriginalTerminalResponse() {
    Harness h;
    establish(h);
    const auto id = h.service.subscribe(text(), &h.owner, {});
    h.service.enqueueCommand("save", {}, &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 1);
    h.service.unsubscribe(id);
    h.service.unsubscribe(id);
    QCOMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].operation, "cancel");
    QCOMPARE(h.sent[1].payload["request_id"].toString(), h.sent[0].id);
    h.reply(1, {{"accepted", true}, {"stopped", false}}, "0", "");
    QCoreApplication::processEvents();
    QCOMPARE(h.sent.size(), 2);
    QCOMPARE(h.service.revision(), "1");
    QVERIFY(h.service.hasCommands());
    h.reply(0, {}, "2");
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].operation, "save");
    QCOMPARE(h.sent[2].expectedRevision, "2");
    h.reply(2, {{"saved", true}}, "2");
    QTRY_VERIFY(!h.service.hasPending());
  }

  void cancellingQueuedReadsSendsNoCancellationAndReclaimsCapacity() {
    QueryService::Limits limits;
    limits.maxJobs = 2;
    Harness h(limits);
    establish(h);
    h.service.subscribe(text(), &h.owner, {});
    const auto queued = h.service.subscribe(text("0x2000"), &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 1);
    h.service.unsubscribe(queued);
    QVERIFY(h.service.subscribe(text("0x3000"), &h.owner, {}));
    QCOMPARE(h.sent.size(), 1);
    h.reply(0);
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].operation, "decompile");
    QCOMPARE(h.sent[1].payload["address"].toString(), "0x3000");
    h.reply(1);
    QTRY_VERIFY(!h.service.hasPending());
  }

  void ownerDestructionDetachesReadsButAnAcceptedCommandStillExecutes() {
    Harness h;
    establish(h);
    auto owner = std::make_unique<QObject>();
    int calls = 0;
    h.service.subscribe(text(), owner.get(), [&](const auto &) { ++calls; });
    h.service.enqueueCommand("rename",
                             {{"address", "0x1000"}, {"name", "newName"}},
                             owner.get(), [&](const auto &) { ++calls; });
    QTRY_COMPARE(h.sent.size(), 1);
    owner.reset();
    QCOMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].operation, "cancel");
    h.reply(0);
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].operation, "rename");
    h.reply(2, {}, "2");
    QTRY_VERIFY(!h.service.hasPending());
    QCOMPARE(calls, 0);
  }

  void cacheDeliveryIsDeferredAndCannotReviveADestroyedOwner() {
    Harness h;
    establish(h);
    int calls = 0;
    h.service.subscribe(text(), &h.owner, [&](const auto &) { ++calls; });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "cached"}});
    QTRY_COMPARE(calls, 1);
    QTRY_VERIFY(!h.service.hasPending());
    auto owner = std::make_unique<QObject>();
    h.service.subscribe(text(), owner.get(), [&](const auto &) { ++calls; });
    QCOMPARE(calls, 1);
    owner.reset();
    QCoreApplication::processEvents();
    QCOMPARE(calls, 1);
    QCOMPARE(h.sent.size(), 1);
    QList<QJsonObject> cached;
    h.service.subscribe(text(), &h.owner,
                        [&](const auto &r) { cached.append(r); });
    QVERIFY(cached.isEmpty());
    QTRY_COMPARE(cached.size(), 1);
    QCOMPARE(cached[0]["payload"].toObject()["text"].toString(), "cached");
    QCOMPARE(h.sent.size(), 1);
  }

  void aNewSubscriberDoesNotJoinACancelledFlightButCanUseItsNaturalResult() {
    Harness h;
    establish(h);
    int oldCalls = 0;
    QList<QJsonObject> fresh;
    const auto old = h.service.subscribe(text(), &h.owner,
                                         [&](const auto &) { ++oldCalls; });
    QTRY_COMPARE(h.sent.size(), 1);
    h.service.unsubscribe(old);
    const auto current = h.service.subscribe(
        text(), &h.owner, [&](const auto &r) { fresh.append(r); });
    QCOMPARE(h.sent.size(), 2);
    h.service.receive({{"type", "response"},
                       {"request_id", h.sent[0].id},
                       {"operation", "decompile"},
                       {"project_id", "project-one"},
                       {"revision", "2"},
                       {"status", "ok"},
                       {"cancellation_requested", true},
                       {"calculation_stopped", true},
                       {"completed_before_cancellation", true},
                       {"payload", QJsonObject{{"text", "completed result"}}}});
    QTRY_COMPARE(fresh.size(), 1);
    QCOMPARE(oldCalls, 0);
    QCOMPARE(h.sent.size(), 2);
    QCOMPARE(fresh[0]["request_id"].toString(), QString::number(current));
    QVERIFY(!fresh[0].contains("cancellation_requested"));
    QVERIFY(!fresh[0].contains("calculation_stopped"));
    QVERIFY(!fresh[0].contains("completed_before_cancellation"));
    QCOMPARE(fresh[0]["revision"].toString(), "2");
  }

  void queueLimitsRejectAsynchronouslyAndRecoverAfterUnsubscribe_data() {
    QTest::addColumn<int>("kind");
    QTest::newRow("jobs") << 0;
    QTest::newRow("subscribers") << 1;
    QTest::newRow("request bytes") << 2;
  }
  void queueLimitsRejectAsynchronouslyAndRecoverAfterUnsubscribe() {
    QFETCH(int, kind);
    QueryService::Limits limits;
    if (kind == 0)
      limits.maxJobs = 1;
    else if (kind == 1)
      limits.maxSubscriptions = 1;
    else
      limits.maxQueuedBytes = 512;
    Harness h(limits);
    establish(h);
    QList<QJsonObject> rejected;
    const auto first = h.service.subscribe(text(), &h.owner, {});
    QVERIFY(first);
    const auto second =
        h.service.subscribe(kind == 1 ? text() : text("0x2000"), &h.owner,
                            [&](const auto &r) { rejected.append(r); });
    QCOMPARE(second, QueryService::SubscriptionId(0));
    QVERIFY(rejected.isEmpty());
    QTRY_COMPARE(rejected.size(), 1);
    QCOMPARE(errorCode(rejected[0]), "queue_full");
    h.service.unsubscribe(first);
    QTRY_VERIFY(h.sent.size() >= 1);
    if (h.sent.size() == 2)
      h.reply(0);
    QTRY_VERIFY(!h.service.hasPending());
    QVERIFY(h.service.subscribe(text("0x3000"), &h.owner, {}));
  }

  void oversizedRequestsAndCommandsInTheReadApiAreRejectedWithoutDispatch() {
    QueryService::Limits limits;
    limits.maxRequestBytes = 600;
    Harness h(limits);
    establish(h);
    QList<QJsonObject> rejected;
    h.service.subscribe({"resolve", {{"query", QString(1024, 'x')}}}, &h.owner,
                        [&](const auto &r) { rejected.append(r); });
    h.service.subscribe(
        {"rename", {{"address", "0x1000"}, {"name", "wrong API"}}}, &h.owner,
        [&](const auto &r) { rejected.append(r); });
    h.service.enqueueCommand("cancel", {}, &h.owner,
                             [&](const auto &r) { rejected.append(r); });
    QTRY_COMPARE(rejected.size(), 3);
    QCOMPARE(errorCode(rejected[0]), "budget_exceeded");
    QCOMPARE(errorCode(rejected[1]), "invalid_request");
    QCOMPARE(errorCode(rejected[2]), "invalid_request");
    QVERIFY(h.sent.isEmpty());
  }

  void commandFenceKeepsLaterReadsBehindTheEdit() {
    Harness h;
    establish(h);
    QList<QString> results;
    h.service.subscribe(text(), &h.owner, [&](const auto &r) {
      results.append(r["payload"].toObject()["text"].toString());
    });
    h.service.enqueueCommand("annotation_set",
                             {{"address", "0x1000"}, {"text", "new"}}, &h.owner,
                             {});
    h.service.subscribe(text(), &h.owner, [&](const auto &r) {
      results.append(r["payload"].toObject()["text"].toString());
    });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "before"}}, "2");
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(results, QList<QString>{"before"});
    QCOMPARE(h.sent[1].operation, "annotation_set");
    QCOMPARE(h.sent[1].expectedRevision, "2");
    h.reply(1, {}, "3");
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].operation, "decompile");
    QCOMPARE(h.sent[2].expectedRevision, "3");
    h.reply(2, {{"text", "after"}}, "3");
    QTRY_COMPARE(results.size(), 2);
    QCOMPARE(results[1], "after");
  }

  void exactRevisionIsNotSilentlyUpgradedAndDoesNotCoalesceWithLatest() {
    Harness h;
    establish(h);
    QList<QJsonObject> replies;
    h.service.subscribe({"metadata", {}, Spec::Latest, {}}, &h.owner, {});
    h.service.subscribe({"metadata", {}, Spec::Exact, "1"}, &h.owner,
                        [&](const auto &r) { replies.append(r); });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"analyzed", true}}, "2");
    QTRY_COMPARE(replies.size(), 1);
    QCOMPARE(errorCode(replies[0]), "stale_revision");
    QCOMPARE(h.sent.size(), 1);
    h.service.subscribe({"metadata", {}, Spec::Exact, "2"}, &h.owner,
                        [&](const auto &r) { replies.append(r); });
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].expectedRevision, "2");
    h.reply(1, {}, "2");
    QTRY_COMPARE(replies.size(), 2);
    QCOMPARE(replies[1]["status"].toString(), "ok");
  }

  void lazyAnalysisPublishesItsRevisionEvenWhenRenderingFails() {
    Harness h;
    establish(h);
    QSignalSpy context(&h.service, &QueryService::contextChanged);
    QList<QJsonObject> results;
    h.service.subscribe(text(), &h.owner,
                        [&](const auto &r) { results.append(r); });
    h.service.subscribe(text("0xffff000000000001", "med"), &h.owner,
                        [&](const auto &r) { results.append(r); });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {}, "2", "project-one", "error", "render_failed");
    QCOMPARE(h.service.revision(), "2");
    QCOMPARE(context.size(), 1);
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].expectedRevision, "2");
    h.reply(1, {{"text", "Med page"}}, "2");
    QTRY_COMPARE(results.size(), 2);
    QCOMPARE(errorCode(results[0]), "render_failed");
    QCOMPARE(results[1]["revision"].toString(), "2");
  }

  void administrativeAndAdmissionResponsesCannotRollBackContext() {
    Harness h;
    establish(h);
    QList<QJsonObject> replies;
    h.service.subscribe(text(), &h.owner,
                        [&](const auto &r) { replies.append(r); });
    QTRY_COMPARE(h.sent.size(), 1);
    h.service.receive({{"type", "response"},
                       {"request_id", "not-owned"},
                       {"operation", "cancel"},
                       {"status", "ok"},
                       {"project_id", "obsolete"},
                       {"revision", "0"}});
    h.service.receive(
        {{"type", "heartbeat"}, {"project_id", "obsolete"}, {"revision", "0"}});
    QCOMPARE(h.service.projectId(), "project-one");
    h.reply(0, {}, "0", "", "error", "queue_full");
    QCOMPARE(h.service.revision(), "1");
    QTRY_COMPARE(replies.size(), 1);
    QCOMPARE(replies[0]["revision"].toString(), "1");
    QCOMPARE(errorCode(replies[0]), "queue_full");
  }

  void resetTerminatesLiveSubscribersAndCannotReuseAnOldProcessCache() {
    Harness h;
    establish(h);
    QList<QJsonObject> replies;
    h.service.subscribe(text(), &h.owner,
                        [&](const auto &r) { replies.append(r); });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "old cache"}});
    QTRY_COMPARE(replies.size(), 1);
    QTRY_VERIFY(!h.service.hasPending());
    h.service.subscribe(text("0x2000"), &h.owner,
                        [&](const auto &r) { replies.append(r); });
    QTRY_COMPARE(h.sent.size(), 2);
    const auto oldEpoch = h.service.sessionEpoch();
    h.service.resetSession();
    QVERIFY(h.service.sessionEpoch() > oldEpoch);
    QVERIFY(!h.service.available());
    h.reply(1, {{"text", "late old process"}}, "999");
    QCOMPARE(h.service.revision(), "0");
    QTRY_COMPARE(replies.size(), 2);
    QCOMPARE(errorCode(replies[1]), "worker_stopped");
    h.sent.clear();
    h.service.setAvailable(true);
    establish(h);
    h.service.subscribe(text(), &h.owner,
                        [&](const auto &r) { replies.append(r); });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "new process"}});
    QTRY_COMPARE(replies.size(), 3);
    QCOMPARE(replies[2]["payload"].toObject()["text"].toString(),
             "new process");
  }

  void successfulOpenRetiresOldQueuedReadsAndAddressCommands() {
    Harness h;
    establish(h);
    QList<QJsonObject> oldResults, newResults;
    const auto oldEpoch = h.service.sessionEpoch();
    h.service.enqueueCommand(
        "open", {{"path", "fixture-two"}}, &h.owner, [&](const auto &) {
          h.service.subscribe(text("0x3000"), &h.owner,
                              [&](const auto &r) { newResults.append(r); });
        });
    h.service.subscribe(text(), &h.owner,
                        [&](const auto &r) { oldResults.append(r); });
    h.service.enqueueCommand(
        "rename", {{"address", "0x1000"}, {"name", "old target"}}, &h.owner,
        [&](const auto &r) { oldResults.append(r); });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {}, "1", "project-two");
    QVERIFY(h.service.sessionEpoch() > oldEpoch);
    QTRY_COMPARE(oldResults.size(), 2);
    QCOMPARE(errorCode(oldResults[0]), "session_changed");
    QCOMPARE(errorCode(oldResults[1]), "session_changed");
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].operation, "decompile");
    QCOMPARE(h.sent[1].payload["address"].toString(), "0x3000");
    h.reply(1, {{"text", "new project page"}}, "1", "project-two");
    QTRY_COMPARE(newResults.size(), 1);
    QCOMPARE(newResults[0]["project_id"].toString(), "project-two");
  }

  void failedOpenPreservesTheOldSessionAndQueue() {
    Harness h;
    establish(h);
    const auto oldEpoch = h.service.sessionEpoch();
    h.service.enqueueCommand("open", {{"path", "missing"}}, &h.owner, {});
    h.service.enqueueCommand("rename",
                             {{"address", "0x1000"}, {"name", "still valid"}},
                             &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {}, "1", "project-one", "error", "open_failed");
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.service.sessionEpoch(), oldEpoch);
    QCOMPARE(h.sent[1].operation, "rename");
    h.reply(1, {}, "2");
    QTRY_VERIFY(!h.service.hasPending());
  }

  void mutableMetadataHistoryAndContributionsAreNeverCached() {
    Harness h;
    establish(h);
    for (const auto *operation :
         {"metadata", "history", "contributions", "contribution_execute"}) {
      for (int repeat = 0; repeat < 2; ++repeat) {
        QList<QJsonObject> result;
        const auto count = h.sent.size();
        h.service.subscribe({operation, {}}, &h.owner,
                            [&](const auto &r) { result.append(r); });
        QTRY_COMPARE(h.sent.size(), count + 1);
        h.reply(int(count), {{"dirty", repeat == 0}});
        QTRY_COMPARE(result.size(), 1);
        QCOMPARE(result[0]["payload"].toObject()["dirty"].toBool(),
                 repeat == 0);
        QTRY_VERIFY(!h.service.hasPending());
      }
    }
  }

  void graphTransactionsDoNotInterleaveWithAnotherPaneOrMcp() {
    Harness h;
    establish(h);
    QList<QJsonObject> a, b;
    h.service.graphViewport(viewport("0x1000"), &h.owner,
                            [&](const auto &r) { a.append(r); });
    h.service.graphViewport(viewport("0x2000"), &h.owner,
                            [&](const auto &r) { b.append(r); });
    h.service.subscribe({"cfg_summary", {{"address", "0x3000"}}}, &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 1);
    QCOMPARE(h.sent[0].operation, "cfg_summary");
    h.reply(0, graph("0x1000", "layout-A"), "2");
    QCOMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].operation, "cfg_viewport");
    QCOMPARE(h.sent[1].expectedRevision, "2");
    QCOMPARE(h.sent[1].payload["layout_revision"].toString(), "layout-A");
    h.reply(1, graph("0x1000", "layout-A"), "2");
    QTRY_COMPARE(a.size(), 1);
    const auto first = a[0]["payload"].toObject();
    QCOMPARE(first["summary"].toObject()["layout_revision"],
             first["viewport"].toObject()["layout_revision"]);
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].operation, "cfg_summary");
    QCOMPARE(h.sent[2].payload["address"].toString(), "0x2000");
    h.reply(2, graph("0x2000", "layout-B"), "2");
    QCOMPARE(h.sent.size(), 4);
    QCOMPARE(h.sent[3].operation, "cfg_viewport");
    h.reply(3, graph("0x2000", "layout-B"), "2");
    QTRY_COMPARE(b.size(), 1);
    QCOMPARE(
        b[0]["payload"].toObject()["viewport"].toObject()["address"].toString(),
        "0x2000");
    QTRY_COMPARE(h.sent.size(), 5);
    QCOMPARE(h.sent[4].operation, "cfg_summary");
    QCOMPARE(h.sent[4].payload["address"].toString(), "0x3000");
    h.reply(4, graph("0x3000", "layout-C"), "2");
    QTRY_VERIFY(!h.service.hasPending());
  }

  void graphStaleLayoutRetriesOnceAndThenReleasesTheBarrier() {
    Harness h;
    establish(h);
    QList<QJsonObject> result;
    h.service.graphViewport(viewport("0x1000"), &h.owner,
                            [&](const auto &r) { result.append(r); });
    h.service.subscribe({"metadata", {}}, &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, graph("0x1000", "layout-1"));
    QCOMPARE(h.sent.size(), 2);
    h.reply(1, {}, "1", "project-one", "error", "stale_layout");
    QCOMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].operation, "cfg_summary");
    h.reply(2, graph("0x1000", "layout-2"));
    QCOMPARE(h.sent.size(), 4);
    QCOMPARE(h.sent[3].payload["layout_revision"].toString(), "layout-2");
    h.reply(3, {}, "1", "project-one", "error", "stale_layout");
    QTRY_COMPARE(result.size(), 1);
    QCOMPARE(errorCode(result[0]), "stale_layout");
    QTRY_COMPARE(h.sent.size(), 5);
    QCOMPARE(h.sent[4].operation, "metadata");
    h.reply(4);
    QTRY_VERIFY(!h.service.hasPending());
  }

  void graphPreservesOmittedDefaultCursors_data() {
    QTest::addColumn<bool>("nodePresent");
    QTest::addColumn<bool>("edgePresent");
    QTest::newRow("both omitted") << false << false;
    QTest::newRow("node omitted") << false << true;
    QTest::newRow("edge omitted") << true << false;
  }
  void graphPreservesOmittedDefaultCursors() {
    QFETCH(bool, nodePresent);
    QFETCH(bool, edgePresent);
    Harness h;
    establish(h);
    auto request = viewport("0x1000");
    if (!nodePresent)
      request.remove("node_offset");
    if (!edgePresent)
      request.remove("edge_offset");
    QList<QJsonObject> result;
    h.service.graphViewport(request, &h.owner, [&](const auto &response) {
      result.append(response);
    });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, graph("0x1000", "layout-defaults"));
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].operation, "cfg_viewport");
    auto expected = request;
    expected["layout_revision"] = "layout-defaults";
    // Worker sizeField accepts absence as the default, but rejects null.
    // Assert the actual outgoing transaction payload, not a permissive fake's
    // eventual result, so accidental insertion cannot hide behind the fixture.
    QCOMPARE(h.sent[1].payload, expected);
    QCOMPARE(h.sent[1].payload.contains("node_offset"), nodePresent);
    QCOMPARE(h.sent[1].payload.contains("edge_offset"), edgePresent);
    h.reply(1, graph("0x1000", "layout-defaults"));
    QTRY_COMPARE(result.size(), 1);
    QCOMPARE(result.first()["status"].toString(), "ok");
  }

  void graphContinuationCannotMoveToADifferentLayout_data() {
    QTest::addColumn<QString>("cursor");
    QTest::addColumn<QString>("originalLayout");
    QTest::newRow("nodes changed layout")
        << QString("node_offset") << QString("old");
    QTest::newRow("edges changed layout")
        << QString("edge_offset") << QString("old");
    QTest::newRow("nodes missing layout")
        << QString("node_offset") << QString{};
    QTest::newRow("edges missing layout")
        << QString("edge_offset") << QString{};
  }
  void graphContinuationCannotMoveToADifferentLayout() {
    QFETCH(QString, cursor);
    QFETCH(QString, originalLayout);
    Harness h;
    establish(h);
    auto request = viewport("0x1000");
    request[cursor] = 7;
    if (!originalLayout.isEmpty())
      request["layout_revision"] = originalLayout;
    QList<QJsonObject> results;
    h.service.graphViewport(request, &h.owner,
                            [&](const auto &r) { results.append(r); });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, graph("0x1000", "new"));
    // A continuation of the old sorted list must not be dispatched against
    // a new one: doing so silently omits the new layout's first objects.
    QCOMPARE(h.sent.size(), 1);
    QTRY_COMPARE(results.size(), 1);
    QCOMPARE(errorCode(results[0]), "stale_layout");
    QVERIFY(results[0]["payload"].toObject().isEmpty());
    QTRY_VERIFY(!h.service.hasPending());
  }

  void graphContinuationKeepsItsCursorsWhenTheLayoutStillMatches() {
    Harness h;
    establish(h);
    auto request = viewport("0x1000");
    request["node_offset"] = 7;
    request["edge_offset"] = 3;
    request["layout_revision"] = "stable";
    QList<QJsonObject> results;
    h.service.graphViewport(request, &h.owner,
                            [&](const auto &r) { results.append(r); });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, graph("0x1000", "stable"));
    QCOMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].payload, request);
    h.reply(1, graph("0x1000", "stable"));
    QTRY_COMPARE(results.size(), 1);
    QCOMPARE(results[0]["status"].toString(), "ok");
  }

  void graphRetryCannotMoveContinuationCursorsToANewLayout() {
    Harness h;
    establish(h);
    auto request = viewport("0x1000");
    request["node_offset"] = 7;
    request["layout_revision"] = "old";
    QList<QJsonObject> results;
    h.service.graphViewport(request, &h.owner,
                            [&](const auto &r) { results.append(r); });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, graph("0x1000", "old"));
    QCOMPARE(h.sent.size(), 2);
    h.reply(1, {}, "1", "project-one", "error", "stale_layout");
    QCOMPARE(h.sent.size(), 3);
    h.reply(2, graph("0x1000", "new"));
    QCOMPARE(h.sent.size(), 3);
    QTRY_COMPARE(results.size(), 1);
    QCOMPARE(errorCode(results[0]), "stale_layout");
    QTRY_VERIFY(!h.service.hasPending());
  }

  void graphRejectsAnUnrelatedAddressOrLayout_data() {
    QTest::addColumn<bool>("wrongAddress");
    QTest::newRow("address") << true;
    QTest::newRow("layout") << false;
  }
  void graphRejectsAnUnrelatedAddressOrLayout() {
    QFETCH(bool, wrongAddress);
    Harness h;
    establish(h);
    QList<QJsonObject> result;
    h.service.graphViewport(viewport("0x1000"), &h.owner,
                            [&](const auto &r) { result.append(r); });
    QTRY_COMPARE(h.sent.size(), 1);
    if (wrongAddress) {
      h.reply(0, graph("0x2000", "unrelated"));
    } else {
      h.reply(0, graph("0x1000", "layout-A"));
      QCOMPARE(h.sent.size(), 2);
      h.reply(1, graph("0x1000", "layout-B"));
    }
    QTRY_COMPARE(result.size(), 1);
    QCOMPARE(errorCode(result[0]),
             wrongAddress ? QString("invalid_graph") : QString("stale_layout"));
    QVERIFY(result[0]["payload"].toObject().isEmpty());
    QTRY_VERIFY(!h.service.hasPending());
  }

  void cacheBudgetEvictsOldPagesAndDoesNotRetainOversizedResults() {
    QueryService::Limits limits;
    limits.cacheBytes = 2400;
    Harness h(limits);
    establish(h);
    const auto read = [&](const QString &address, const QString &value) {
      const auto before = h.sent.size();
      int calls = 0;
      h.service.subscribe(text(address), &h.owner,
                          [&](const auto &) { ++calls; });
      QTRY_COMPARE(h.sent.size(), before + 1);
      h.reply(int(before), {{"text", value}});
      QTRY_COMPARE(calls, 1);
      QTRY_VERIFY(!h.service.hasPending());
    };
    read("0x1000", QString(160, 'a'));
    read("0x2000", QString(160, 'b'));
    read("0x1000", QString(160, 'a')); // A was evicted by B.
    read("0x3000", QString(2048, 'c'));
    read("0x3000", QString(2048, 'c')); // Oversized C was not retained.
  }

  void completionCanResetTheSessionWithoutDeliveringTheSharedResultToAPeer() {
    Harness h;
    establish(h);
    QObject other;
    QList<QJsonObject> peer;
    h.service.subscribe(text(), &h.owner,
                        [&](const auto &) { h.service.resetSession(); });
    h.service.subscribe(text(), &other, [&](const auto &r) { peer.append(r); });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "must not reach retired peer"}});
    QTRY_COMPARE(peer.size(), 1);
    QCOMPARE(errorCode(peer[0]), "worker_stopped");
    QVERIFY(peer[0]["payload"].toObject().isEmpty());
    QTRY_VERIFY(!h.service.hasPending());
  }

  void completionCanDestroyTheServiceAndSuppressRemainingCallbacks() {
    QObject owner;
    QList<WireRequest> sent;
    auto service = std::make_unique<QueryService>(
        [&](const QString &operation, const QJsonObject &payload,
            const QString &revision) {
          sent.append({"wire", operation, payload, revision});
          return QString("wire");
        });
    service->setAvailable(true);
    int calls = 0;
    service->subscribe({"metadata", {}}, &owner, [&](const auto &) {
      ++calls;
      service.reset();
    });
    service->subscribe({"metadata", {}}, &owner,
                       [&](const auto &) { ++calls; });
    QTRY_COMPARE(sent.size(), 1);
    service->receive({{"type", "response"},
                      {"request_id", "wire"},
                      {"operation", "metadata"},
                      {"status", "ok"},
                      {"project_id", ""},
                      {"revision", "0"},
                      {"payload", QJsonObject{}}});
    QTRY_VERIFY(!service);
    QCOMPARE(calls, 1);
  }
};

QTEST_GUILESS_MAIN(QueryServiceTests)
#include "QueryServiceTests.moc"
