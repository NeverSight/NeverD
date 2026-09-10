#include "PaneController.h"
#include "PaneRegistry.h"
#include "QueryService.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QSet>
#include <QSignalSpy>
#include <QTest>

namespace {
const QString A = "0xffff800012340000";
const QString B = "0xffff800012340100";
const QString C = "0xffff800012340200";
const QString D = "0xffff800012340300";
const QString Hash(64, 'a');

struct Request {
  QString id, operation;
  QJsonObject payload;
};
struct Harness {
  QObject owner;
  QList<Request> sent;
  QSet<QString> answered;
  QueryService queries;
  PaneRegistry panes;

  Harness()
      : queries([this](const QString &operation, const QJsonObject &payload,
                       const QString &) {
          const auto id = QString::number(sent.size() + 1);
          sent.append({id, operation, payload});
          return id;
        }),
        panes(&queries) {
    QObject::connect(&queries, &QueryService::analysisCompleted, &panes,
                     [this] { panes.repairAnalysisLocations(); });
    queries.setAvailable(true);
  }
  void reply(int index, QJsonObject payload, QString status = "ok",
             QString revision = "1", QString analysisState = {}) {
    const auto request = sent.at(index);
    answered.insert(request.id);
    queries.receive({{"type", "response"},
                     {"request_id", request.id},
                     {"operation", request.operation},
                     {"status", status},
                     {"analysis_state", analysisState},
                     {"revision", revision},
                     {"project_id", "fixture-project"},
                     {"payload", payload},
                     {"error", QJsonObject{{"code", "test_error"},
                                           {"message", "test error"}}}});
  }
  QJsonObject result(const Request &request) const {
    const auto address = request.payload["address"].toString();
    if (request.operation == "resolve") {
      const auto query = request.payload["query"].toString();
      return {{"address", query},
              {"function_address", query},
              {"name", "function:" + query},
              {"comment", "comment:" + query}};
    }
    if (request.operation == "disasm")
      return {{"items",
               QJsonArray{QJsonObject{{"address", address},
                                      {"mnemonic", "instruction:" + address}}}},
              {"next_address", QJsonValue::Null}};
    if (request.operation == "decompile") {
      const auto representation = request.payload["representation"].toString();
      const int offset = request.payload["offset"].toInt();
      const auto identity = address + ':' + representation;
      return {{"text", identity + QString(":page%1\n").arg(offset)},
              {"offset", offset},
              {"next_offset", offset ? QJsonValue::Null : QJsonValue(2)},
              {"mapping_status", "instruction_anchors"},
              {"rows", QJsonArray{QJsonObject{
                           {"line", offset},
                           {"object_id", identity + QString::number(offset)},
                           {"addresses", QJsonArray{address}}}}}};
    }
    if (request.operation == "bytes")
      return {{"address", address}, {"data", "41424344"}};
    if (request.operation == "xrefs")
      return {{"items", QJsonArray{QJsonObject{
                            {"from", A}, {"to", address}, {"kind", "call"}}}}};
    if (request.operation == "cfg_summary")
      return {
          {"address", address},
          {"layout_revision", "layout:" + address},
          {"bounds",
           QJsonObject{{"x", 0}, {"y", 0}, {"width", 200}, {"height", 100}}}};
    if (request.operation == "cfg_viewport")
      return {{"address", address},
              {"layout_revision", "layout:" + address},
              {"nodes", QJsonArray{QJsonObject{{"id", "block:" + address},
                                               {"address", address}}}},
              {"edges", QJsonArray{}}};
    return {{"items", QJsonArray{}}};
  }
  int pending(const QString &operation, const QString &query = {}) const {
    for (int i = 0; i < sent.size(); ++i)
      if (!answered.contains(sent[i].id) && sent[i].operation == operation &&
          (query.isEmpty() || sent[i].payload["query"].toString() == query))
        return i;
    return -1;
  }
  int count(const QString &operation, const QString &address = {}) const {
    int result = 0;
    for (const auto &request : sent)
      if (request.operation == operation &&
          (address.isEmpty() ||
           request.payload["address"].toString() == address))
        ++result;
    return result;
  }
  bool commit(PaneController *pane, const QString &address) {
    const auto ticket = panes.beginNavigation(pane, PaneNavigation::Function);
    return panes.commitNavigation(
        ticket, {address, address, "function:" + address, {}, true});
  }
};
} // namespace

class IndependentPaneTests final : public QObject {
  Q_OBJECT
  bool hasPaneWork(Harness &h) const {
    if (h.queries.hasPending())
      return true;
    for (const auto &item : h.panes.items()) {
      auto *pane = qobject_cast<PaneController *>(
          item.toMap()["controller"].value<QObject *>());
      if (pane && pane->busy())
        return true;
    }
    return false;
  }
  void settle(Harness &h) {
    QElapsedTimer deadline;
    deadline.start();
    int idleTurns = 0;
    while (deadline.elapsed() < 5000 && idleTurns < 3) {
      QCoreApplication::processEvents();
      for (int i = 0; i < h.sent.size(); ++i) {
        const auto request = h.sent[i];
        if (!h.answered.contains(request.id))
          h.reply(i, h.result(request));
      }
      idleTurns = hasPaneWork(h) ? 0 : idleTurns + 1;
      // Trailing reads are pane work before they reach the dispatcher.
      if (hasPaneWork(h))
        QTest::qWait(1);
    }
    QCoreApplication::processEvents();
    QVERIFY2(!hasPaneWork(h),
             "The controlled pane/transport did not become idle");
  }
  int awaitCommentRequest(Harness &h, const QString &address) {
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 5000) {
      QCoreApplication::processEvents();
      // Complete unrelated reads, while retaining the selected address's
      // actual public resolve request for manual delayed delivery.
      for (int i = 0; i < h.sent.size(); ++i) {
        const auto request = h.sent[i];
        if (h.answered.contains(request.id))
          continue;
        if (request.operation == "resolve" &&
            request.payload["query"].toString() == address)
          return i;
        h.reply(i, h.result(request));
      }
      QTest::qWait(1);
    }
    return -1;
  }
  void establish(Harness &h) {
    h.queries.enqueueCommand("open", {{"path", "fixture"}}, &h.owner, {});
    settle(h);
    h.panes.setLoaded(true);
    h.panes.setBinaryIdentity(Hash);
    QCOMPARE(h.queries.projectId(), "fixture-project");
  }

private slots:
  void membershipChangesKeepCurrentDetailsUnlessCancelled_data() {
    QTest::addColumn<QString>("action");
    QTest::addColumn<bool>("held");
    QTest::addColumn<bool>("cancelDuringChange");
    for (const auto &action : {"pin", "leave", "remove-group"})
      for (const auto held : {false, true})
        for (const auto cancelled : {false, true}) {
          const auto name = QString("%1-%2-%3")
                                .arg(action, held ? "held" : "queued",
                                     cancelled ? "cancel" : "complete");
          QTest::newRow(qPrintable(name))
              << QString(action) << held << cancelled;
        }
  }
  void membershipChangesKeepCurrentDetailsUnlessCancelled() {
    QFETCH(QString, action);
    QFETCH(bool, held);
    QFETCH(bool, cancelDuringChange);
    Harness h;
    establish(h);
    auto *pane = h.panes.defaultMachine();
    h.panes.setPaneOpen(h.panes.defaultRepresentation()->id(), false);
    const auto group = h.panes.createGroup("Detail membership");
    QVERIFY(!group.isEmpty());
    h.panes.setPaneGroup(pane->id(), group);
    QVERIFY(h.commit(pane, C));
    settle(h);
    QVERIFY(h.commit(pane, A));
    settle(h);
    QVERIFY(pane->commentReady());
    QVERIFY(pane->canGoBack());
    const auto historyBefore = pane->canGoBack();
    auto *xrefs = qobject_cast<PageModel *>(pane->xrefsModel());
    QVERIFY(xrefs);

    // Move inside the known function, then change membership before the
    // current address's details finish. The address and history must survive.
    pane->selectInstruction(B);
    QCOMPARE(pane->selectedAddress(), B);
    QCOMPARE(pane->selectedFunctionAddress(), A);
    QVERIFY(!pane->commentReady());
    QCOMPARE(xrefs->count(), 0);
    int heldRequest = -1;
    if (held) {
      QTRY_VERIFY(h.pending("xrefs") >= 0);
      heldRequest = h.pending("xrefs");
      QCOMPARE(h.sent[heldRequest].payload["address"].toString(), B);
    } else {
      QVERIFY(!h.queries.hasPending());
      QVERIFY(pane->busy());
    }
    const auto xrefsBefore = h.count("xrefs");
    const auto resolvesBefore = h.count("resolve");
    bool cancelled = false;
    QObject observer;
    connect(
        pane, &PaneController::changed, &observer,
        [&] {
          const bool membershipChanged =
              action == "pin" ? pane->pinned() : pane->groupId().isEmpty();
          if (cancelDuringChange && membershipChanged && !cancelled) {
            cancelled = true;
            h.panes.cancelReads();
          }
        },
        Qt::DirectConnection);

    if (action == "pin")
      h.panes.setPanePinned(pane->id(), true);
    else if (action == "leave")
      h.panes.setPaneGroup(pane->id(), {});
    else
      h.panes.removeGroup(group);
    QCOMPARE(cancelled, cancelDuringChange);
    if (action == "pin") {
      QVERIFY(pane->pinned());
      QCOMPARE(pane->groupId(), group);
    } else {
      QVERIFY(pane->groupId().isEmpty());
      if (action == "remove-group")
        for (const auto &entry : h.panes.groups())
          QVERIFY(entry.toMap()["id"].toString() != group);
    }
    QCOMPARE(pane->selectedAddress(), B);
    QCOMPARE(pane->selectedFunctionAddress(), A);
    QCOMPARE(pane->canGoBack(), historyBefore);
    QVERIFY(!pane->commentReady());
    if (heldRequest >= 0)
      h.reply(heldRequest, h.result(h.sent[heldRequest]));
    settle(h);

    QCOMPARE(pane->selectedAddress(), B);
    QCOMPARE(pane->selectedFunctionAddress(), A);
    QCOMPARE(pane->canGoBack(), historyBefore);
    QVERIFY(!pane->busy());
    if (cancelDuringChange) {
      QVERIFY(!pane->commentReady());
      QVERIFY(pane->selectedComment().isEmpty());
      QCOMPARE(xrefs->count(), 0);
      QCOMPARE(h.count("xrefs"), xrefsBefore);
      QCOMPARE(h.count("resolve"), resolvesBefore);
    } else {
      QVERIFY(pane->commentReady());
      QCOMPARE(pane->selectedComment(), "comment:" + B);
      QCOMPARE(xrefs->count(), 1);
      QCOMPARE(xrefs->get(0)["to"].toString(), B);
    }
  }

  void missingCommentNavigationWaitsForItsAddressDetail() {
    Harness h;
    establish(h);
    auto *pane = h.panes.defaultMachine();
    h.panes.setPaneOpen(h.panes.defaultRepresentation()->id(), false);
    // A symbol query and its resolved address have distinct legitimate cache
    // keys; this tests one address-detail fallback without bypassing caching.
    pane->navigate("entry_without_comment");
    QTRY_VERIFY(h.pending("resolve", "entry_without_comment") >= 0);
    h.reply(h.pending("resolve", "entry_without_comment"),
            {{"address", A}, {"function_address", A}, {"name", "entry"}});
    QTRY_COMPARE(pane->selectedAddress(), A);
    QVERIFY(!pane->commentReady());
    QVERIFY(pane->selectedComment().isEmpty());
    const auto detail = awaitCommentRequest(h, A);
    QVERIFY(detail >= 0);
    QCOMPARE(h.count("resolve"), 2);
    QTest::qWait(80);
    QVERIFY(!pane->commentReady());
    QVERIFY(pane->selectedComment().isEmpty());
    auto payload = h.result(h.sent[detail]);
    payload["comment"] = "saved entry note";
    h.reply(detail, payload);
    QTRY_VERIFY(pane->commentReady());
    QCOMPARE(pane->selectedComment(), QString("saved entry note"));
    settle(h);
    QCOMPARE(h.count("resolve"), 2);
  }

  void explicitlyEmptyCommentIsReadyFromNavigationAndDetail() {
    Harness h;
    establish(h);
    auto *pane = h.panes.defaultMachine();
    h.panes.setPaneOpen(h.panes.defaultRepresentation()->id(), false);
    pane->navigate(A);
    QTRY_VERIFY(h.pending("resolve", A) >= 0);
    h.reply(h.pending("resolve", A), {{"address", A},
                                      {"function_address", A},
                                      {"name", "entry"},
                                      {"comment", ""}});
    QTRY_COMPARE(pane->selectedAddress(), A);
    QTRY_VERIFY(pane->commentReady());
    QVERIFY(pane->selectedComment().isEmpty());
    settle(h);
    // An explicitly known empty comment requires no redundant supplement.
    QCOMPARE(h.count("resolve"), 1);

    pane->selectInstruction(B);
    QVERIFY(!pane->commentReady());
    QVERIFY(pane->selectedComment().isEmpty());
    const auto detail = awaitCommentRequest(h, B);
    QVERIFY(detail >= 0);
    auto payload = h.result(h.sent[detail]);
    payload["comment"] = "";
    QSignalSpy changes(pane, &PaneController::changed);
    h.reply(detail, payload);
    QTRY_VERIFY(pane->commentReady());
    QVERIFY(pane->selectedComment().isEmpty());
    // Readiness must notify the UI even though the string stays empty.
    QVERIFY(!changes.isEmpty());
    settle(h);
    QCOMPARE(h.count("resolve"), 2);
  }

  void staleCommentReplyCannotAuthorizeTheNewSelection() {
    Harness h;
    establish(h);
    auto *pane = h.panes.defaultMachine();
    h.panes.setPaneOpen(h.panes.defaultRepresentation()->id(), false);
    pane->navigate(A);
    settle(h);
    QVERIFY(pane->commentReady());
    QCOMPARE(pane->selectedComment(), "comment:" + A);

    pane->selectInstruction(B);
    QCOMPARE(pane->selectedAddress(), B);
    QVERIFY(!pane->commentReady());
    QVERIFY(pane->selectedComment().isEmpty());
    const auto oldDetail = awaitCommentRequest(h, B);
    QVERIFY(oldDetail >= 0);
    pane->selectInstruction(C);
    QCOMPARE(pane->selectedAddress(), C);
    QVERIFY(!pane->commentReady());
    QVERIFY(pane->selectedComment().isEmpty());
    auto oldPayload = h.result(h.sent[oldDetail]);
    oldPayload["comment"] = "old address note";
    h.reply(oldDetail, oldPayload);
    QCoreApplication::processEvents();
    QCOMPARE(pane->selectedAddress(), C);
    QVERIFY(!pane->commentReady());
    QVERIFY(pane->selectedComment().isEmpty());

    const auto latestDetail = awaitCommentRequest(h, C);
    QVERIFY(latestDetail >= 0);
    QVERIFY(!pane->commentReady());
    auto latestPayload = h.result(h.sent[latestDetail]);
    latestPayload["comment"] = "current address note";
    h.reply(latestDetail, latestPayload);
    QTRY_VERIFY(pane->commentReady());
    QCOMPARE(pane->selectedAddress(), C);
    QCOMPARE(pane->selectedComment(), QString("current address note"));
    settle(h);
  }

  void rapidInstructionDetailsCoalesceWithoutDelayingSelection() {
    Harness h;
    establish(h);
    auto *pane = h.panes.defaultMachine();
    h.panes.setPaneOpen(h.panes.defaultRepresentation()->id(), false);
    QVERIFY(h.commit(pane, A));
    settle(h);
    const auto before = h.sent.size();
    const auto beforeXrefs = h.count("xrefs"),
               beforeResolve = h.count("resolve");
    QSignalSpy selections(pane, &PaneController::selectionChanged);
    QString latest;
    for (quint64 index = 1; index <= 64; ++index) {
      latest = "0x" + QString::number(A.toULongLong(nullptr, 16) + index, 16);
      pane->selectInstruction(latest);
      QCOMPARE(pane->selectedAddress(), latest);
      QVERIFY(pane->selectedComment().isEmpty());
    }
    QCOMPARE(selections.size(), 64);
    QCOMPARE(h.sent.size(), before);
    // The old implementation already has queued subscriptions at this point.
    QVERIFY(!h.queries.hasPending());
    QVERIFY(pane->busy());
    settle(h);
    QCOMPARE(h.sent.size() - before, 2);
    QCOMPARE(h.count("xrefs") - beforeXrefs, 1);
    QCOMPARE(h.count("resolve") - beforeResolve, 1);
    QCOMPARE(pane->selectedComment(), "comment:" + latest);
    auto *xrefs = qobject_cast<PageModel *>(pane->xrefsModel());
    QVERIFY(xrefs);
    QCOMPARE(xrefs->count(), 1);
    QCOMPARE(xrefs->get(0)["to"].toString(), latest);
    QVERIFY(pane->error().isEmpty());
  }

  void heldDetailKeepsOnlyTheLatestOfTwentyEightTimedSelections() {
    Harness h;
    establish(h);
    auto *pane = h.panes.defaultMachine();
    h.panes.setPaneOpen(h.panes.defaultRepresentation()->id(), false);
    QVERIFY(h.commit(pane, A));
    settle(h);
    const auto beforeXrefs = h.count("xrefs"),
               beforeResolve = h.count("resolve");
    const auto beforeCancel = h.count("cancel");
    auto *xrefs = qobject_cast<PageModel *>(pane->xrefsModel());
    QVERIFY(xrefs);
    QStringList publishedTargets;
    connect(xrefs, &QAbstractItemModel::modelReset, this, [&] {
      if (xrefs->count())
        publishedTargets.append(xrefs->get(0)["to"].toString());
    });
    pane->selectInstruction(B);
    QTRY_VERIFY(h.pending("xrefs") >= 0);
    const auto held = h.pending("xrefs");
    const auto whileHeld = h.sent.size();
    QString latest;
    for (quint64 index = 1; index <= 28; ++index) {
      latest = "0x" + QString::number(B.toULongLong(nullptr, 16) + index, 16);
      pane->selectInstruction(latest);
      QCOMPARE(pane->selectedAddress(), latest);
      // Hold the first terminal reply independently of GUI/event-loop time.
      QTest::qWait(50);
      QCOMPARE(h.sent.size(), whileHeld);
      QCOMPARE(h.count("cancel"), beforeCancel);
    }
    h.reply(held, h.result(h.sent[held]));
    settle(h);
    QCOMPARE(h.count("xrefs") - beforeXrefs, 2);
    QCOMPARE(h.count("resolve") - beforeResolve, 1);
    QCOMPARE(publishedTargets, QStringList{latest});
    QCOMPARE(pane->selectedComment(), "comment:" + latest);
    QVERIFY(pane->error().isEmpty());
  }

  void queuedDetailsDoNotSurvivePaneRetirement_data() {
    QTest::addColumn<QString>("action");
    for (const auto &action : {"cancel", "hide", "close", "remove", "epoch"})
      QTest::newRow(action) << QString(action);
  }
  void queuedDetailsDoNotSurvivePaneRetirement() {
    QFETCH(QString, action);
    Harness h;
    establish(h);
    h.panes.setPaneOpen(h.panes.defaultMachine()->id(), false);
    h.panes.setPaneOpen(h.panes.defaultRepresentation()->id(), false);
    const auto id = h.panes.createPane("machine");
    QPointer<PaneController> pane = h.panes.findPane(id);
    QVERIFY(pane);
    h.panes.setPaneVisible(id, true);
    QVERIFY(h.commit(pane, A));
    settle(h);
    pane->selectInstruction(B);
    QVERIFY(!h.queries.hasPending());
    if (action == "cancel")
      h.panes.cancelReads();
    else if (action == "hide")
      h.panes.setPaneVisible(id, false);
    else if (action == "close")
      h.panes.setPaneOpen(id, false);
    else if (action == "remove") {
      QVERIFY(h.panes.beginRemovePane(id));
      h.panes.finishRemovePane(id);
      QVERIFY(!pane);
    } else {
      h.queries.resetSession();
      h.queries.setAvailable(true);
      h.panes.setLoaded(true);
    }
    const auto afterRetirement = h.sent.size();
    QTest::qWait(100);
    QCOMPARE(h.sent.size(), afterRetirement);
    QVERIFY(!h.queries.hasPending());
    if (pane)
      QVERIFY(!pane->busy());
  }

  void cancelRetiresHeldDetailsWithoutRevivingTheTrailingSelection() {
    Harness h;
    establish(h);
    auto *pane = h.panes.defaultMachine();
    h.panes.setPaneOpen(h.panes.defaultRepresentation()->id(), false);
    QVERIFY(h.commit(pane, A));
    settle(h);
    pane->selectInstruction(B);
    QTRY_VERIFY(h.pending("xrefs") >= 0);
    const auto held = h.pending("xrefs");
    pane->selectInstruction(C);
    h.panes.cancelReads();
    const auto afterCancel = h.sent.size();
    h.reply(held, h.result(h.sent[held]), "ok", "1", "complete");
    QTest::qWait(100);
    settle(h);
    QCOMPARE(h.sent.size(), afterCancel);
    QCOMPARE(pane->selectedAddress(), C);
    QVERIFY(pane->selectedComment().isEmpty());
    QCOMPARE(qobject_cast<PageModel *>(pane->xrefsModel())->count(), 0);
    QVERIFY(!pane->busy());
    QVERIFY(pane->error().isEmpty());
  }

  void reentrantPendingChangeCancelDoesNotRetainDetails_data() {
    QTest::addColumn<bool>("inFlight");
    QTest::newRow("detail-admission") << false;
    QTest::newRow("detail-in-flight") << true;
  }
  void reentrantPendingChangeCancelDoesNotRetainDetails() {
    QFETCH(bool, inFlight);
    Harness h;
    establish(h);
    auto *pane = h.panes.defaultMachine();
    h.panes.setPaneOpen(h.panes.defaultRepresentation()->id(), false);
    QVERIFY(h.commit(pane, A));
    settle(h);
    const auto beforeXrefs = h.count("xrefs");
    const auto beforeResolve = h.count("resolve");
    int held = -1;
    if (inFlight) {
      pane->selectInstruction(B);
      QTRY_VERIFY(h.pending("xrefs") >= 0);
      held = h.pending("xrefs");
      pane->selectInstruction(C);
    }
    bool armed = true;
    connect(&h.queries, &QueryService::pendingChanged, this, [&] {
      if (armed && h.queries.hasPending()) {
        armed = false;
        h.panes.cancelReads();
      }
    });
    int externalReplies = 0;
    if (inFlight) {
      // Another owner's admission emits pendingChanged while the first detail
      // is in flight. Cancel must retire the pane but preserve that owner.
      h.queries.subscribe({"metadata", {}}, &h.owner,
                          [&](const auto &response) {
                            QCOMPARE(response["status"].toString(), "ok");
                            ++externalReplies;
                          });
    } else {
      // This Cancel is synchronous inside the detail's own admission, before
      // the returned subscription ID can be assigned by PaneController.
      pane->selectInstruction(B);
    }
    QTRY_VERIFY(!armed);
    if (held >= 0)
      h.reply(held, h.result(h.sent[held]), "ok", "1", "complete");
    QTest::qWait(100);
    settle(h);
    QVERIFY(!pane->busy());
    QVERIFY(!h.queries.hasPending());
    QCOMPARE(h.count("xrefs") - beforeXrefs, inFlight ? 1 : 0);
    QCOMPARE(h.count("resolve"), beforeResolve);
    QCOMPARE(externalReplies, inFlight ? 1 : 0);
    QCOMPARE(pane->selectedAddress(), inFlight ? C : B);
    QVERIFY(pane->selectedComment().isEmpty());
    auto *xrefs = qobject_cast<PageModel *>(pane->xrefsModel());
    QVERIFY(xrefs);
    QCOMPARE(xrefs->count(), 0);
    // An explicit later selection may resume; no retired timer can do so.
    pane->selectInstruction(D);
    settle(h);
    QCOMPARE(pane->selectedComment(), "comment:" + D);
    QCOMPARE(xrefs->get(0)["to"].toString(), D);
    QVERIFY(pane->error().isEmpty());
  }

  void discoveredIdentityUpdatesTheGroupWithoutNavigating() {
    Harness h;
    establish(h);
    auto *machine = h.panes.defaultMachine();
    auto *representation = h.panes.defaultRepresentation();
    const auto ticket =
        h.panes.beginNavigation(machine, PaneNavigation::Function);
    QVERIFY(h.panes.commitNavigation(ticket, {A, {}, {}, {}}));
    settle(h);
    QVERIFY(machine->selectedFunctionAddress().isEmpty());
    QVERIFY(representation->representationText().isEmpty());
    const auto navigation = h.panes.navigationRevision();
    const auto beforeHistory = machine->canGoBack();
    h.queries.subscribe({"metadata", {}}, &h.owner, {});
    QTRY_VERIFY(h.pending("metadata") >= 0);
    h.reply(h.pending("metadata"), {}, "ok", "1", "complete");
    QTRY_VERIFY(h.queries.analysisComplete());
    QCoreApplication::processEvents();
    settle(h);
    QCOMPARE(machine->selectedAddress(), A);
    QCOMPARE(representation->selectedAddress(), A);
    QCOMPARE(machine->selectedFunctionAddress(), A);
    QCOMPARE(representation->selectedFunctionAddress(), A);
    QVERIFY(representation->representationText().contains(A + ":c"));
    QCOMPARE(h.panes.navigationRevision(), navigation);
    QCOMPARE(machine->canGoBack(), beforeHistory);
    QVERIFY(!representation->canGoBack());
    h.panes.setPanePinned(representation->id(), true);
    h.panes.setPanePinned(representation->id(), false);
    settle(h);
    QCOMPARE(representation->selectedFunctionAddress(), A);
    QCOMPARE(representation->selectedAddress(), A);
  }

  void externalSharedCompletionDoesNotResumeCancelledPanes() {
    Harness h;
    establish(h);
    auto *machine = h.panes.defaultMachine();
    auto *representation = h.panes.defaultRepresentation();
    const auto ticket =
        h.panes.beginNavigation(machine, PaneNavigation::Function);
    QVERIFY(h.panes.commitNavigation(ticket, {A, {}, {}, {}}));
    settle(h);
    machine->selectInstruction(D);
    QTRY_VERIFY(h.pending("xrefs") >= 0);
    const int pending = h.pending("xrefs");
    int externalReplies = 0;
    h.queries.subscribe(
        {"xrefs", {{"address", D}, {"direction", "to"}, {"limit", 256}}},
        &h.owner, [&](const auto &response) {
          QCOMPARE(response["status"].toString(), "ok");
          ++externalReplies;
        });
    h.panes.cancelReads();
    const int requestsAfterCancel = h.sent.size();
    h.reply(pending, {{"items", QJsonArray{}}}, "ok", "1", "complete");
    QTRY_COMPARE(externalReplies, 1);
    QCoreApplication::processEvents();
    settle(h);
    QCOMPARE(h.sent.size(), requestsAfterCancel);
    QVERIFY(machine->selectedFunctionAddress().isEmpty());
    QVERIFY(representation->selectedFunctionAddress().isEmpty());
    QVERIFY(representation->representationText().isEmpty());

    representation->reloadRepresentation();
    settle(h);
    QCOMPARE(representation->selectedAddress(), D);
    QCOMPARE(representation->selectedFunctionAddress(), D);
    QVERIFY(representation->representationText().contains(D + ":c"));
    QVERIFY(machine->selectedFunctionAddress().isEmpty());
  }

  void analysisRepairCannotReplaceANewerNavigation() {
    Harness h;
    establish(h);
    auto *machine = h.panes.defaultMachine();
    const auto ticket =
        h.panes.beginNavigation(machine, PaneNavigation::Function);
    QVERIFY(h.panes.commitNavigation(ticket, {A, {}, {}, {}}));
    settle(h);
    h.queries.subscribe({"metadata", {}}, &h.owner, {});
    QTRY_VERIFY(h.pending("metadata") >= 0);
    // A new revision also invalidates the pre-analysis resolve cache.
    h.reply(h.pending("metadata"), {}, "ok", "2", "complete");
    QTRY_VERIFY(h.pending("resolve", A) >= 0);
    const int repair = h.pending("resolve", A);
    machine->navigate(B);
    h.reply(repair, h.result(h.sent[repair]), "ok", "2", "complete");
    settle(h);
    QCOMPARE(machine->selectedAddress(), B);
    QCOMPARE(h.panes.defaultRepresentation()->selectedAddress(), B);
    QCOMPARE(machine->selectedFunctionAddress(), B);
  }

  void fourPanesKeepDistinctViewsPagesAndHistories() {
    Harness h;
    establish(h);
    auto *m1 = h.panes.defaultMachine();
    auto *r1 = h.panes.defaultRepresentation();
    QVERIFY(h.commit(m1, A));
    settle(h);
    const auto m2id = h.panes.createPane("machine");
    const auto r2id = h.panes.createPane("representation");
    auto *m2 = h.panes.findPane(m2id), *r2 = h.panes.findPane(r2id);
    QVERIFY(m2 && r2);
    QVERIFY(m2->groupId().isEmpty());
    QCOMPARE(m2->selectedAddress(), A);
    QVERIFY(!m2->canGoBack());
    const auto second = h.panes.createGroup("Second <group> 中文");
    h.panes.setPaneGroup(m2id, second);
    h.panes.setPaneGroup(r2id, second);
    r1->setRepresentation("low");
    r2->setRepresentation("med");
    h.panes.setPaneVisible(m2id, true);
    h.panes.setPaneVisible(r2id, true);
    QVERIFY(h.commit(m2, B));
    settle(h);
    QCOMPARE(m1->selectedAddress(), A);
    QCOMPARE(r1->selectedAddress(), A);
    QCOMPARE(m2->selectedAddress(), B);
    QCOMPARE(r2->selectedAddress(), B);
    QVERIFY(r1->representationText().contains(A + ":low"));
    QVERIFY(r2->representationText().contains(B + ":med"));
    QVERIFY(r1->textMappings().first().toMap()["object_id"] !=
            r2->textMappings().first().toMap()["object_id"]);
    QVERIFY(m1->instructionsModel() != m2->instructionsModel());
    QCOMPARE(qobject_cast<PageModel *>(m2->instructionsModel())
                 ->get(0)["address"]
                 .toString(),
             B);
    r2->loadMoreText();
    settle(h);
    QVERIFY(r2->representationText().contains("page2"));
    QVERIFY(!r1->representationText().contains("page2"));
    m2->requestView("hex");
    settle(h);
    QVERIFY(m2->hexText().startsWith(B.mid(2)));
    QVERIFY(m1->hexText().isEmpty());
    m1->requestView("cfg");
    settle(h);
    QCOMPARE(m1->graphNodes().first().toMap()["id"].toString(), "block:" + A);
    QCOMPARE(m2->centralView(), "hex");
    QVERIFY(h.commit(m2, C));
    settle(h);
    QVERIFY(m2->canGoBack());
    QVERIFY(!m1->canGoBack());
    m2->goBack();
    settle(h);
    QCOMPARE(m2->selectedAddress(), B);
    QCOMPARE(r2->selectedAddress(), B);
    QCOMPARE(m1->selectedAddress(), A);
    QVERIFY(m2->canGoForward());
    h.panes.setActivePane(r2id);
    QCOMPARE(h.panes.activePane(), r2);
    QCOMPARE(r2->selection()["project_id"].toString(), h.queries.projectId());
    QCOMPARE(r2->selection()["address"].toString(), B);
  }

  void pinnedRepresentationKeepsItsFunctionAndPagination() {
    Harness h;
    establish(h);
    QVERIFY(h.commit(h.panes.defaultMachine(), A));
    settle(h);
    auto *representation = h.panes.defaultRepresentation();
    h.panes.setPanePinned(representation->id(), true);
    QVERIFY(h.commit(h.panes.defaultMachine(), B));
    settle(h);
    representation->loadMoreText();
    settle(h);
    QCOMPARE(representation->selectedFunctionAddress(), A);
    QVERIFY(representation->representationText().contains(A + ":c:page2"));
    h.panes.setPanePinned(representation->id(), false);
    settle(h);
    QCOMPARE(representation->selectedFunctionAddress(), B);
    QVERIFY(representation->representationText().contains(B + ":c"));
  }
  void instructionSelectionWithoutAFunctionNeverInventsFunctionNavigation() {
    Harness h;
    establish(h);
    auto *machine = h.panes.defaultMachine();
    const auto ticket =
        h.panes.beginNavigation(machine, PaneNavigation::Function);
    QVERIFY(h.panes.commitNavigation(ticket, {C, {}, "address only", {}}));
    settle(h);
    machine->selectInstruction(A);
    settle(h);
    QCOMPARE(machine->selectedAddress(), A);
    QVERIFY(machine->selectedFunctionAddress().isEmpty());
    QVERIFY(!machine->canGoBack());
    QVERIFY(h.panes.defaultRepresentation()->representationText().isEmpty());
    QCOMPARE(h.count("decompile", A), 0);
  }

  void laterGroupIntentRetiresEarlierResolveBeforeAnyApply() {
    Harness h;
    establish(h);
    auto *first = h.panes.defaultMachine(),
         *second = h.panes.defaultRepresentation();
    QVERIFY(h.commit(first, C));
    settle(h);
    first->navigate(A);
    QTRY_VERIFY(h.pending("resolve", A) >= 0);
    const auto old = h.pending("resolve", A);
    second->navigate(B);
    QVERIFY(!first->busy());
    h.reply(old, h.result(h.sent[old]));
    QTRY_VERIFY(h.pending("resolve", B) >= 0);
    QCOMPARE(first->selectedAddress(), C);
    QCOMPARE(second->selectedAddress(), C);
    const auto latest = h.pending("resolve", B);
    h.reply(latest, h.result(h.sent[latest]));
    settle(h);
    QCOMPARE(first->selectedAddress(), B);
    QCOMPARE(second->selectedAddress(), B);
    QCOMPARE(h.count("disasm", A), 0);
    QCOMPARE(h.count("decompile", A), 0);
    first->goBack();
    settle(h);
    QCOMPARE(first->selectedAddress(), C);
  }

  void failedNewerIntentNeverRevalidatesOlderSuccess() {
    Harness h;
    establish(h);
    auto *first = h.panes.defaultMachine(),
         *second = h.panes.defaultRepresentation();
    QVERIFY(h.commit(first, C));
    settle(h);
    const auto old = h.panes.beginNavigation(first, PaneNavigation::Function);
    second->navigate(B);
    QTRY_VERIFY(h.pending("resolve", B) >= 0);
    h.reply(h.pending("resolve", B), {}, "error");
    settle(h);
    QVERIFY(!h.panes.commitNavigation(old, {A, A, "A", {}}));
    QCOMPARE(first->selectedAddress(), C);
    QCOMPARE(second->selectedAddress(), C);
    QVERIFY(!first->busy());
    QVERIFY(!second->busy());
    QVERIFY(!first->canGoBack());
    QVERIFY(!second->canGoBack());
    // A cancelled latest intent likewise leaves the monotonic watermark.
    const auto oldAgain =
        h.panes.beginNavigation(first, PaneNavigation::Function);
    const auto cancelled =
        h.panes.beginNavigation(second, PaneNavigation::Function);
    h.panes.finishNavigation(cancelled);
    QVERIFY(!h.panes.isCurrent(oldAgain));
    QCOMPARE(first->selectedAddress(), C);
  }

  void sameAddressInstructionAndQueuedCacheHitRespectIntentOrder() {
    Harness h;
    establish(h);
    auto *first = h.panes.defaultMachine(),
         *second = h.panes.defaultRepresentation();
    QVERIFY(h.commit(first, C));
    settle(h);
    const auto old = h.panes.beginNavigation(first, PaneNavigation::Function);
    second->selectInstruction(C);
    QVERIFY(!h.panes.commitNavigation(old, {A, A, "A", {}}));
    QCOMPARE(first->selectedAddress(), C);
    settle(h);
    // Prime the real service cache, then retire its deferred subscriber before
    // delivery. Cached resolve must obey the same guards as a worker reply.
    h.queries.subscribe({"resolve", {{"query", A}}}, &h.owner, {});
    settle(h);
    first->navigate(A);
    second->navigate(B);
    settle(h);
    QCOMPARE(first->selectedAddress(), B);
    QCOMPARE(second->selectedAddress(), B);
  }

  void membershipChangesDoNotCancelAnotherOrigin_data() {
    QTest::addColumn<QString>("action");
    for (const auto &action : {"pin", "leave", "close", "hide"})
      QTest::newRow(action) << QString(action);
  }
  void membershipChangesDoNotCancelAnotherOrigin() {
    QFETCH(QString, action);
    Harness h;
    establish(h);
    auto *first = h.panes.defaultMachine(),
         *second = h.panes.defaultRepresentation();
    QVERIFY(h.commit(first, C));
    settle(h);
    const auto ticket =
        h.panes.beginNavigation(second, PaneNavigation::Function);
    if (action == "pin")
      h.panes.setPanePinned(first->id(), true);
    if (action == "leave")
      h.panes.setPaneGroup(first->id(), {});
    if (action == "close")
      h.panes.setPaneOpen(first->id(), false);
    if (action == "hide")
      h.panes.setPaneVisible(first->id(), false);
    QVERIFY(h.panes.isCurrent(ticket));
    QVERIFY(h.panes.commitNavigation(ticket, {B, B, "B", {}}));
    settle(h);
    QCOMPARE(second->selectedAddress(), B);
    QCOMPARE(first->selectedAddress(), action == "hide" ? B : C);
  }

  void retiringLatestOriginDoesNotReviveOlderIntent_data() {
    membershipChangesDoNotCancelAnotherOrigin_data();
  }
  void retiringLatestOriginDoesNotReviveOlderIntent() {
    QFETCH(QString, action);
    Harness h;
    establish(h);
    auto *first = h.panes.defaultMachine(),
         *second = h.panes.defaultRepresentation();
    QVERIFY(h.commit(first, C));
    settle(h);
    const auto old = h.panes.beginNavigation(first, PaneNavigation::Function);
    const auto latest =
        h.panes.beginNavigation(second, PaneNavigation::Function);
    if (action == "pin")
      h.panes.setPanePinned(second->id(), true);
    if (action == "leave")
      h.panes.setPaneGroup(second->id(), {});
    if (action == "close")
      h.panes.setPaneOpen(second->id(), false);
    if (action == "hide")
      h.panes.setPaneVisible(second->id(), false);
    QVERIFY(!h.panes.isCurrent(latest));
    QVERIFY(!h.panes.isCurrent(old));
    QCOMPARE(first->selectedAddress(), C);
    QCOMPARE(second->selectedAddress(), C);
    QVERIFY(!h.panes.commitNavigation(old, {D, D, "old", {}}));
    QVERIFY(!h.panes.commitNavigation(latest, {B, B, "latest", {}}));
    // Membership may refresh details at C after retiring both navigation
    // intents. Draining those reads must never revive either stale target.
    settle(h);
    QVERIFY(!h.panes.isCurrent(latest));
    QVERIFY(!h.panes.isCurrent(old));
    QVERIFY(!second->busy());
    QCOMPARE(first->selectedAddress(), C);
    QCOMPARE(second->selectedAddress(), C);
  }

  void unpinAndJoiningAdoptCanonicalWithoutStealingPendingIntent() {
    Harness h;
    establish(h);
    auto *first = h.panes.defaultMachine(),
         *second = h.panes.defaultRepresentation();
    QVERIFY(h.commit(first, C));
    settle(h);
    h.panes.setPanePinned(first->id(), true);
    const auto local = h.panes.beginNavigation(first, PaneNavigation::Function);
    const auto grouped =
        h.panes.beginNavigation(second, PaneNavigation::Function);
    h.panes.setPanePinned(first->id(), false);
    QVERIFY(!h.panes.isCurrent(local));
    QVERIFY(h.panes.isCurrent(grouped));
    QCOMPARE(first->selectedAddress(), C);
    QVERIFY(h.panes.commitNavigation(grouped, {B, B, "B", {}}));
    settle(h);
    QCOMPARE(first->selectedAddress(), B);

    const auto extraId = h.panes.createPane("machine");
    auto *extra = h.panes.findPane(extraId);
    const auto other = h.panes.createGroup("Other");
    h.panes.setPaneGroup(extraId, other);
    const auto oldGroup =
        h.panes.beginNavigation(first, PaneNavigation::Function);
    const auto newGroup =
        h.panes.beginNavigation(extra, PaneNavigation::Function);
    h.panes.setPaneGroup(first->id(), other);
    QVERIFY(!h.panes.isCurrent(oldGroup));
    QVERIFY(h.panes.isCurrent(newGroup));
    QVERIFY(h.panes.commitNavigation(newGroup, {D, D, "D", {}}));
    settle(h);
    QCOMPARE(first->selectedAddress(), D);
    QCOMPARE(second->selectedAddress(), B);
  }

  void hiddenPanesReleasePagesAndOnlyReceiveLightweightLocations() {
    Harness h;
    establish(h);
    auto *machine = h.panes.defaultMachine(),
         *representation = h.panes.defaultRepresentation();
    QVERIFY(h.commit(machine, A));
    settle(h);
    h.panes.setPaneVisible(representation->id(), false);
    QVERIFY(representation->representationText().isEmpty());
    QVERIFY(h.commit(machine, B));
    settle(h);
    QCOMPARE(representation->selectedAddress(), B);
    QCOMPARE(h.count("decompile", B), 0);
    h.panes.setPaneVisible(representation->id(), true);
    settle(h);
    QVERIFY(representation->representationText().contains(B));
    QCOMPARE(h.count("decompile", B), 1);
    representation->navigate(D);
    QTRY_VERIFY(h.pending("resolve", D) >= 0);
    h.panes.setPaneVisible(representation->id(), false);
    settle(h);
    QCOMPARE(machine->selectedAddress(), B);
    QVERIFY(!representation->busy());
  }

  void hidingOneSubscriberDoesNotCancelTheOtherSharedReader() {
    Harness h;
    establish(h);
    QVERIFY(h.commit(h.panes.defaultMachine(), A));
    settle(h);
    const auto secondId = h.panes.createPane("representation");
    auto *first = h.panes.defaultRepresentation(),
         *second = h.panes.findPane(secondId);
    first->setRepresentation("llvm");
    second->setRepresentation("llvm");
    h.panes.setPaneVisible(secondId, true);
    QTRY_VERIFY(h.pending("decompile") >= 0);
    const auto before = h.count("cancel");
    h.panes.setPaneVisible(first->id(), false);
    QCOMPARE(h.count("cancel"), before);
    settle(h);
    QVERIFY(first->representationText().isEmpty());
    QVERIFY(second->representationText().contains(A + ":llvm"));
  }

  void closeRemoveAndGroupIncarnationsRejectOldTickets() {
    Harness h;
    establish(h);
    QVERIFY(h.commit(h.panes.defaultMachine(), C));
    settle(h);
    const auto id = h.panes.createPane("machine");
    QPointer<PaneController> extra = h.panes.findPane(id);
    const auto group = h.panes.createGroup("Reusable name");
    h.panes.setPaneGroup(id, group);
    auto ticket = h.panes.beginNavigation(extra, PaneNavigation::Function);
    h.panes.renameGroup(group, "Renamed");
    h.panes.setActivePane(id);
    QVERIFY(h.panes.isCurrent(ticket));
    h.panes.setPaneOpen(id, false);
    h.panes.setPaneOpen(id, true);
    QVERIFY(!h.panes.isCurrent(ticket));
    ticket = h.panes.beginNavigation(extra, PaneNavigation::Function);
    h.panes.removeGroup(group);
    const auto replacementGroup = h.panes.createGroup("Reusable name");
    QVERIFY(replacementGroup != group);
    h.panes.setPaneGroup(id, replacementGroup);
    QVERIFY(!h.panes.isCurrent(ticket));
    const auto metadata = h.panes.serializeMetadata();
    ticket = h.panes.beginNavigation(extra, PaneNavigation::Function);
    QVERIFY(h.panes.beginRemovePane(id));
    QVERIFY(!h.panes.isCurrent(ticket));
    h.panes.finishRemovePane(id);
    QVERIFY(!extra);
    QVERIFY(h.panes.restoreMetadata(metadata));
    QVERIFY(h.panes.findPane(id));
    QVERIFY(!h.panes.isCurrent(ticket));
    QVERIFY(!h.panes.beginRemovePane("machine"));
  }

  void sessionReplacementRetiresAllLocationsAndLateCallbacks() {
    Harness h;
    establish(h);
    auto *machine = h.panes.defaultMachine();
    QVERIFY(h.commit(machine, A));
    settle(h);
    machine->navigate(B);
    QTRY_VERIFY(h.pending("resolve", B) >= 0);
    const int late = h.pending("resolve", B);
    const auto ticket = h.panes.beginNavigation(h.panes.defaultRepresentation(),
                                                PaneNavigation::Function);
    h.queries.resetSession();
    QVERIFY(!machine->loaded());
    QVERIFY(machine->selectedAddress().isEmpty());
    QVERIFY(!h.panes.isCurrent(ticket));
    h.reply(late, h.result(h.sent[late]));
    QCoreApplication::processEvents();
    QVERIFY(machine->selectedAddress().isEmpty());
    QVERIFY(!machine->busy());
  }

  void duplicateCommitAndSelectionNotificationsDoNotCreateHistory() {
    Harness h;
    establish(h);
    auto *machine = h.panes.defaultMachine();
    auto *representation = h.panes.defaultRepresentation();
    QVERIFY(h.commit(machine, A));
    settle(h);
    const auto ticket =
        h.panes.beginNavigation(machine, PaneNavigation::Function);
    const auto revision = h.panes.navigationRevision();
    QVERIFY(h.panes.commitNavigation(ticket, {B, B, "B", {}}));
    QVERIFY(!h.panes.commitNavigation(ticket, {B, B, "B", {}}));
    QCOMPARE(h.panes.navigationRevision(), revision);
    settle(h);
    machine->goBack();
    settle(h);
    QCOMPARE(machine->selectedAddress(), A);
    QVERIFY(!machine->canGoBack());
    QVERIFY(representation->canGoBack());
  }

  void reentrantNavigationStopsTheOlderGroupFanout() {
    Harness h;
    establish(h);
    auto *machine = h.panes.defaultMachine(),
         *representation = h.panes.defaultRepresentation();
    QVERIFY(h.commit(machine, A));
    settle(h);
    bool reentered = false;
    connect(machine, &PaneController::selectionChanged, this, [&] {
      if (machine->selectedAddress() == B && !reentered) {
        reentered = true;
        QVERIFY(h.commit(machine, C));
      }
    });
    QVERIFY(h.commit(machine, B));
    settle(h);
    QVERIFY(reentered);
    QCOMPARE(machine->selectedAddress(), C);
    QCOMPARE(representation->selectedAddress(), C);
  }

  void cancelDuringModelResetDoesNotRestartReadsOrContinueGroupFanout() {
    Harness h;
    establish(h);
    auto *machine = h.panes.defaultMachine();
    auto *representation = h.panes.defaultRepresentation();
    QVERIFY(h.commit(machine, A));
    settle(h);
    auto *model = qobject_cast<PageModel *>(machine->instructionsModel());
    QVERIFY(model);
    bool cancelled = false;
    QObject observer;
    connect(
        model, &QAbstractItemModel::modelReset, &observer,
        [&] {
          if (cancelled || machine->selectedAddress() != B)
            return;
          cancelled = true;
          h.panes.cancelReads();
        },
        Qt::DirectConnection);

    QVERIFY(h.commit(machine, B));
    QVERIFY(cancelled);
    // Both controllers remain alive. Cancel must stop this stack's read launch
    // and prevent the older commit from applying B to the next recipient.
    QCOMPARE(representation->selectedAddress(), A);
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());
    settle(h);
    QCOMPARE(h.count("disasm", B), 0);
    QCOMPARE(h.count("decompile", B), 0);
    QCOMPARE(h.count("xrefs", B), 0);
  }

  void cancelDuringPendingChangedDoesNotRestartReadsOrContinueGroupFanout() {
    Harness h;
    establish(h);
    auto *machine = h.panes.defaultMachine();
    auto *representation = h.panes.defaultRepresentation();
    QVERIFY(h.commit(machine, A));
    settle(h);
    machine->requestView(
        "hex"); // Leave a queued read for clearResults to detach.
    QVERIFY(machine->busy());
    bool cancelled = false;
    QObject observer;
    connect(
        &h.queries, &QueryService::pendingChanged, &observer,
        [&] {
          if (cancelled || machine->selectedAddress() != B)
            return;
          cancelled = true;
          h.panes.cancelReads();
        },
        Qt::DirectConnection);

    QVERIFY(h.commit(representation, B));
    QVERIFY(cancelled);
    QCOMPARE(representation->selectedAddress(), A);
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());
    settle(h);
    QCOMPARE(h.count("bytes", B), 0);
    QCOMPARE(h.count("decompile", B), 0);
    QCOMPARE(h.count("xrefs", B), 0);
  }

  void cancelDuringNavigationReplacementStopsTheOuterIntent_data() {
    QTest::addColumn<bool>("previousGroupOrigin");
    QTest::newRow("own-subscription") << false;
    QTest::newRow("previous-group-origin") << true;
  }
  void cancelDuringNavigationReplacementStopsTheOuterIntent() {
    QFETCH(bool, previousGroupOrigin);
    Harness h;
    establish(h);
    QPointer<PaneController> machine = h.panes.defaultMachine();
    QPointer<PaneController> representation = h.panes.defaultRepresentation();
    QPointer<PaneRegistry> registry = &h.panes;
    QVERIFY(h.commit(machine, A));
    settle(h);
    const auto resolvesBefore = h.count("resolve");
    auto *previous =
        previousGroupOrigin ? representation.data() : machine.data();
    previous->navigate(B);
    QVERIFY(previous->busy());
    QVERIFY(h.queries.hasPending());
    QCOMPARE(h.count("resolve"), resolvesBefore);
    bool cancelled = false;
    QObject observer;
    QPointer<QObject> observerGuard = &observer;
    connect(
        &h.queries, &QueryService::pendingChanged, &observer,
        [&] {
          if (cancelled)
            return;
          // B is still queued: removing its last subscriber empties the
          // service synchronously, before C may admit a replacement resolve.
          QVERIFY(!h.queries.hasPending());
          cancelled = true;
          h.panes.cancelReads();
        },
        Qt::DirectConnection);

    machine->navigate(C);
    QVERIFY(cancelled);
    QVERIFY(registry && machine && representation && observerGuard);
    QCOMPARE(machine->selectedAddress(), A);
    QCOMPARE(representation->selectedAddress(), A);
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());
    settle(h);
    QVERIFY(registry && machine && representation && observerGuard);
    QCOMPARE(h.count("resolve"), resolvesBefore);
    QCOMPARE(h.count("disasm", C), 0);
    QCOMPARE(h.count("decompile", C), 0);
    QCOMPARE(h.count("xrefs", C), 0);
    QCOMPARE(machine->selectedAddress(), A);
    QCOMPARE(representation->selectedAddress(), A);
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());

    machine->navigate(D);
    settle(h);
    QCOMPARE(machine->selectedAddress(), D);
    QCOMPARE(representation->selectedAddress(), D);
    QVERIFY(representation->representationText().contains(D));
    QCOMPARE(h.count("disasm", D), 1);
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());
  }

  void cancelDuringRequestAdmissionDoesNotPublishAnObsoleteRead_data() {
    QTest::addColumn<bool>("newerNavigation");
    QTest::newRow("cancel-admission") << false;
    QTest::newRow("cancel-then-synchronous-navigation") << true;
  }
  void cancelDuringRequestAdmissionDoesNotPublishAnObsoleteRead() {
    QFETCH(bool, newerNavigation);
    Harness h;
    establish(h);
    QPointer<PaneController> machine = h.panes.defaultMachine();
    QPointer<PaneController> representation = h.panes.defaultRepresentation();
    QPointer<PaneRegistry> registry = &h.panes;
    QVERIFY(h.commit(machine, A));
    settle(h);
    QVERIFY(!h.queries.hasPending());
    bool cancelled = false;
    QObject observer;
    QPointer<QObject> observerGuard = &observer;
    connect(
        &h.queries, &QueryService::pendingChanged, &observer,
        [&] {
          if (cancelled || machine->selectedAddress() != B)
            return;
          // With all previous reads settled, this is the first disassembly
          // admission. Its subscription exists before request gets its ID.
          QVERIFY(h.queries.hasPending());
          QVERIFY(!machine->busy());
          QCOMPARE(h.count("disasm", B), 0);
          cancelled = true;
          h.panes.cancelReads();
          if (newerNavigation)
            QVERIFY(h.commit(machine, C));
        },
        Qt::DirectConnection);

    QVERIFY(h.commit(machine, B));
    QVERIFY(cancelled);
    QVERIFY(registry && machine && representation && observerGuard);
    QCOMPARE(machine->selectedAddress(), newerNavigation ? C : B);
    QCOMPARE(representation->selectedAddress(), newerNavigation ? C : A);
    if (newerNavigation) {
      QVERIFY(machine->busy());
      QVERIFY(representation->busy());
    } else {
      QVERIFY(!machine->busy());
      QVERIFY(!representation->busy());
    }
    settle(h);
    QVERIFY(registry && machine && representation && observerGuard);
    QCOMPARE(h.count("disasm", B), 0);
    QCOMPARE(h.count("decompile", B), 0);
    QCOMPARE(h.count("xrefs", B), 0);
    QCOMPARE(machine->selectedAddress(), newerNavigation ? C : B);
    QCOMPARE(representation->selectedAddress(), newerNavigation ? C : A);
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());
    if (newerNavigation) {
      // The obsolete returned ID must not replace or detach C's new slot.
      QCOMPARE(h.count("disasm", C), 1);
      QVERIFY(representation->representationText().contains(C));
      auto *model = qobject_cast<PageModel *>(machine->instructionsModel());
      QVERIFY(model);
      QCOMPARE(model->get(0)["address"].toString(), C);
    }

    machine->navigate(D);
    settle(h);
    QCOMPARE(machine->selectedAddress(), D);
    QCOMPARE(representation->selectedAddress(), D);
    QVERIFY(representation->representationText().contains(D));
    QCOMPARE(h.count("disasm", D), 1);
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());
  }

  void cancelDuringMetadataRestoreDoesNotReplaySavedPositions_data() {
    QTest::addColumn<bool>("duringModelReset");
    QTest::newRow("internal-cancel-pendingChanged") << false;
    QTest::newRow("clear-results-modelReset") << true;
  }
  void cancelDuringMetadataRestoreDoesNotReplaySavedPositions() {
    QFETCH(bool, duringModelReset);
    Harness source;
    establish(source);
    QVERIFY(source.commit(source.panes.defaultMachine(), B));
    settle(source);
    const auto metadata = source.panes.serializeMetadata();

    Harness h;
    // Delay the identity response so any erroneously retained restore is
    // observable when setBinaryIdentity arrives after the nested Cancel.
    h.queries.enqueueCommand("open", {{"path", "fixture"}}, &h.owner, {});
    settle(h);
    h.panes.setLoaded(true);
    QPointer<PaneController> machine = h.panes.defaultMachine();
    QPointer<PaneController> representation = h.panes.defaultRepresentation();
    QPointer<PaneRegistry> registry = &h.panes;
    QVERIFY(h.commit(machine, A));
    settle(h);
    auto *model = qobject_cast<PageModel *>(machine->instructionsModel());
    QVERIFY(model);
    QVERIFY(model->count() > 0);
    const auto resolvesBefore = h.count("resolve");
    if (!duringModelReset) {
      machine->navigate(C);
      QVERIFY(machine->busy());
      QVERIFY(h.queries.hasPending());
    }
    bool cancelled = false;
    QObject observer;
    QPointer<QObject> observerGuard = &observer;
    if (duringModelReset) {
      connect(
          model, &QAbstractItemModel::modelReset, &observer,
          [&] {
            if (cancelled)
              return;
            QVERIFY(machine->selectedAddress().isEmpty());
            cancelled = true;
            h.panes.cancelReads();
          },
          Qt::DirectConnection);
    } else {
      connect(
          &h.queries, &QueryService::pendingChanged, &observer,
          [&] {
            if (cancelled)
              return;
            QVERIFY(!h.queries.hasPending());
            QCOMPARE(machine->selectedAddress(), A);
            cancelled = true;
            h.panes.cancelReads();
          },
          Qt::DirectConnection);
    }

    h.panes.restoreMetadata(metadata);
    QVERIFY(cancelled);
    QVERIFY(registry && machine && representation && observerGuard);
    const auto machineAfterCancel = machine->selectedAddress();
    const auto representationAfterCancel = representation->selectedAddress();
    QVERIFY(machineAfterCancel != B);
    QVERIFY(representationAfterCancel != B);
    h.panes.setBinaryIdentity(Hash);
    QCOMPARE(machine->selectedAddress(), machineAfterCancel);
    QCOMPARE(representation->selectedAddress(), representationAfterCancel);
    h.panes.setPaneVisible(machine->id(), true);
    h.panes.setPaneVisible(representation->id(), true);
    settle(h);
    QVERIFY(registry && machine && representation && observerGuard);
    QCOMPARE(machine->selectedAddress(), machineAfterCancel);
    QCOMPARE(representation->selectedAddress(), representationAfterCancel);
    QCOMPARE(h.count("disasm", B), 0);
    QCOMPARE(h.count("decompile", B), 0);
    QCOMPARE(h.count("xrefs", B), 0);
    QCOMPARE(h.count("resolve"), resolvesBefore);
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());

    machine->navigate(D);
    settle(h);
    QCOMPARE(machine->selectedAddress(), D);
    QCOMPARE(representation->selectedAddress(), D);
    QVERIFY(representation->representationText().contains(D));
    QCOMPARE(h.count("disasm", D), 1);
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());
  }

  void revisionChangeInvalidatesAnchorsWithoutCancellingCurrentAnalysis() {
    Harness h;
    establish(h);
    QVERIFY(h.commit(h.panes.defaultMachine(), A));
    settle(h);
    auto *representation = h.panes.defaultRepresentation();
    QVERIFY(!representation->textMappings().isEmpty());
    representation->setRepresentation("low");
    QTRY_VERIFY(h.pending("decompile") >= 0);
    const auto index = h.pending("decompile");
    h.reply(index, h.result(h.sent[index]), "ok", "2");
    QTRY_VERIFY(!representation->textMappings().isEmpty());
    QVERIFY(representation->representationText().contains(A + ":low"));
    h.queries.enqueueCommand("annotation_set",
                             {{"address", A}, {"text", "new"}}, &h.owner, {});
    QTRY_VERIFY(h.pending("annotation_set") >= 0);
    h.reply(h.pending("annotation_set"), {}, "ok", "3");
    QTRY_VERIFY(representation->textMappings().isEmpty());
    QVERIFY(!representation->representationText().isEmpty());
  }

  void catalogRestoresPositionsOnlyForTheSameBinaryAndPreservesNames() {
    Harness source;
    establish(source);
    QVERIFY(source.commit(source.panes.defaultMachine(), B));
    settle(source);
    source.panes.setPanePinned("representation", true);
    const auto group = source.panes.createGroup("组 <>& مجموعة");
    const auto extraId = source.panes.createPane("representation");
    source.panes.setPaneGroup(extraId, group);
    const auto metadata = source.panes.serializeMetadata();

    Harness restored;
    establish(restored);
    QVERIFY(restored.panes.restoreMetadata(metadata));
    QCOMPARE(restored.panes.defaultMachine()->selectedAddress(), B);
    QVERIFY(restored.panes.defaultRepresentation()->pinned());
    QCOMPARE(restored.panes.findPane(extraId)->groupId(), group);
    restored.panes.setPaneVisible("machine", true);
    settle(restored);
    QCOMPARE(restored.panes.defaultMachine()->selectedFunctionName(),
             "function:" + B);
    restored.panes.setPanePinned("machine", true);
    restored.panes.setPanePinned("machine", false);
    QCOMPARE(restored.panes.defaultMachine()->selectedFunctionName(),
             "function:" + B);
    settle(restored);
    QCOMPARE(restored.panes.defaultMachine()->selectedFunctionName(),
             "function:" + B);
    QVERIFY(restored.panes.groups().last().toMap()["name"].toString().contains(
        "<>&"));

    Harness other;
    establish(other);
    other.panes.setBinaryIdentity(QString(64, 'b'));
    QVERIFY(other.panes.restoreMetadata(metadata));
    QVERIFY(other.panes.defaultMachine()->selectedAddress().isEmpty());
    QVERIFY(!other.panes.defaultRepresentation()->pinned());
    QVERIFY(other.panes.findPane(extraId));
  }

  void delayedRestoreDoesNotOverrideNewNavigationOrPin() {
    Harness source;
    establish(source);
    QVERIFY(source.commit(source.panes.defaultMachine(), A));
    settle(source);
    const auto metadata = source.panes.serializeMetadata();
    Harness restored;
    restored.panes.setLoaded(true);
    QVERIFY(restored.panes.restoreMetadata(metadata));
    restored.panes.setPanePinned("representation", true);
    restored.panes.setBinaryIdentity(Hash);
    QVERIFY(restored.panes.defaultRepresentation()->pinned());
    QVERIFY(
        restored.panes.defaultRepresentation()->selectedAddress().isEmpty());

    Harness navigated;
    navigated.panes.setLoaded(true);
    QVERIFY(navigated.panes.restoreMetadata(metadata));
    QVERIFY(navigated.commit(navigated.panes.defaultMachine(), C));
    navigated.panes.setBinaryIdentity(Hash);
    QCOMPARE(navigated.panes.defaultMachine()->selectedAddress(), C);
  }

  void invalidOrOversizedCatalogDoesNotEvictExistingPanes() {
    Harness h;
    establish(h);
    const auto before = h.panes.serializeMetadata();
    auto invalid = before;
    auto panes = invalid["panes"].toList();
    panes.append(panes.first());
    invalid["panes"] = panes;
    QVERIFY(!h.panes.restoreMetadata(invalid));
    QCOMPARE(h.panes.serializeMetadata(), before);
    for (int i = 0; i < 14; ++i)
      QVERIFY(!h.panes.createPane("machine").isEmpty());
    const auto full = h.panes.serializeMetadata();
    QVERIFY(h.panes.createPane("representation").isEmpty());
    QCOMPARE(h.panes.serializeMetadata(), full);
  }
  void reentrantRemovalDuringAdoptionAndRestoreDoesNotUseTheDeletedPane() {
    Harness h;
    establish(h);
    QVERIFY(h.commit(h.panes.defaultMachine(), A));
    settle(h);
    const auto id = h.panes.createPane("machine");
    QPointer<PaneController> extra = h.panes.findPane(id);
    QVERIFY(h.commit(h.panes.defaultMachine(), B));
    settle(h);
    h.panes.setActivePane(id);
    connect(extra, &PaneController::selectionChanged, this, [&] {
      QVERIFY(h.panes.beginRemovePane(id));
      h.panes.finishRemovePane(id);
    });
    h.panes.setPaneGroup(id, "default");
    QVERIFY(!extra);
    QCOMPARE(h.panes.activePaneId(), "machine");

    const auto restoredId = h.panes.createPane("representation");
    h.panes.setActivePane(restoredId);
    const auto metadata = h.panes.serializeMetadata();
    Harness restored;
    restored.panes.setLoaded(true);
    QVERIFY(restored.panes.restoreMetadata(metadata));
    QPointer<PaneController> restoredPane = restored.panes.findPane(restoredId);
    connect(restoredPane, &PaneController::selectionChanged, this, [&] {
      QVERIFY(restored.panes.beginRemovePane(restoredId));
      restored.panes.finishRemovePane(restoredId);
    });
    restored.panes.setBinaryIdentity(Hash);
    QVERIFY(!restoredPane);
    QVERIFY(!restored.panes.findPane(restoredId));
  }
  void cancelDuringGroupRemovalStillDetachesEveryMember() {
    Harness h;
    establish(h);
    QPointer<PaneController> machine = h.panes.defaultMachine();
    QPointer<PaneController> representation = h.panes.defaultRepresentation();
    QPointer<PaneRegistry> registry = &h.panes;
    QVERIFY(h.commit(machine, A));
    settle(h);
    const auto group = h.panes.createGroup("Temporary group");
    QVERIFY(!group.isEmpty());
    h.panes.setPaneGroup(machine->id(), group);
    h.panes.setPaneGroup(representation->id(), group);
    settle(h);
    QCOMPARE(machine->groupId(), group);
    QCOMPARE(representation->groupId(), group);
    QCOMPARE(machine->selectedAddress(), A);
    QCOMPARE(representation->selectedAddress(), A);
    const auto resolvesBefore = h.count("resolve");
    const auto disassembliesBefore = h.count("disasm");
    const auto decompilationsBefore = h.count("decompile");
    const auto xrefsBefore = h.count("xrefs");
    machine->navigate(B);
    QVERIFY(machine->busy());
    QVERIFY(h.queries.hasPending());
    QCOMPARE(h.count("resolve"), resolvesBefore);
    bool cancelled = false;
    QObject observer;
    QPointer<QObject> observerGuard = &observer;
    connect(
        &h.queries, &QueryService::pendingChanged, &observer,
        [&] {
          if (cancelled)
            return;
          // removeGroup retires the queued Navigation subscription while
          // detaching the first member. Cancel must not stop that structure.
          QVERIFY(!h.queries.hasPending());
          cancelled = true;
          h.panes.cancelReads();
        },
        Qt::DirectConnection);

    h.panes.removeGroup(group);
    QVERIFY(cancelled);
    QVERIFY(registry && machine && representation && observerGuard);
    QVERIFY(machine->groupId().isEmpty());
    QVERIFY(representation->groupId().isEmpty());
    for (const auto &value : h.panes.groups())
      QVERIFY(value.toMap()["id"].toString() != group);
    QCOMPARE(machine->selectedAddress(), A);
    QCOMPARE(representation->selectedAddress(), A);
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());
    settle(h);
    QVERIFY(registry && machine && representation && observerGuard);
    QVERIFY(machine->groupId().isEmpty());
    QVERIFY(representation->groupId().isEmpty());
    QCOMPARE(machine->selectedAddress(), A);
    QCOMPARE(representation->selectedAddress(), A);
    QCOMPARE(h.count("resolve"), resolvesBefore);
    QCOMPARE(h.count("disasm"), disassembliesBefore);
    QCOMPARE(h.count("decompile"), decompilationsBefore);
    QCOMPARE(h.count("xrefs"), xrefsBefore);
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());
  }

  void cancelDuringTextReloadRetirementDoesNotRestartTheRead() {
    Harness h;
    establish(h);
    QPointer<PaneController> machine = h.panes.defaultMachine();
    QPointer<PaneController> representation = h.panes.defaultRepresentation();
    QPointer<PaneRegistry> registry = &h.panes;
    QVERIFY(h.commit(machine, A));
    settle(h);
    QVERIFY(representation->representationText().contains(A + ":c"));
    const auto decompilationsBefore = h.count("decompile");
    // This representation has no cached response. Leave its Text request
    // queued so the next reload synchronously removes a real subscription.
    representation->setRepresentation("llvm");
    QVERIFY(representation->busy());
    QVERIFY(h.queries.hasPending());
    QCOMPARE(h.count("decompile"), decompilationsBefore);
    bool cancelled = false;
    QObject observer;
    QPointer<QObject> observerGuard = &observer;
    connect(
        &h.queries, &QueryService::pendingChanged, &observer,
        [&] {
          if (cancelled)
            return;
          QVERIFY(!h.queries.hasPending());
          cancelled = true;
          h.panes.cancelReads();
        },
        Qt::DirectConnection);

    representation->reloadRepresentation();
    QVERIFY(cancelled);
    QVERIFY(registry && machine && representation && observerGuard);
    QCOMPARE(representation->representation(), QString("llvm"));
    QCOMPARE(representation->selectedAddress(), A);
    QVERIFY(representation->representationText().isEmpty());
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());
    settle(h);
    QVERIFY(registry && machine && representation && observerGuard);
    QCOMPARE(h.count("decompile"), decompilationsBefore);
    QVERIFY(representation->representationText().isEmpty());
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());

    representation->reloadRepresentation();
    QVERIFY(representation->busy());
    settle(h);
    QVERIFY(registry && machine && representation && observerGuard);
    QCOMPARE(h.count("decompile"), decompilationsBefore + 1);
    QVERIFY(representation->representationText().contains(A + ":llvm"));
    QCOMPARE(machine->selectedAddress(), A);
    QCOMPARE(representation->selectedAddress(), A);
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());
  }
  void cancelDuringRestoreKeepsTheWholeCatalogStructurallyValid() {
    Harness source;
    establish(source);
    QVERIFY(source.commit(source.panes.defaultMachine(), B));
    settle(source);
    source.panes.setActivePane("representation");
    const auto metadata = source.panes.serializeMetadata();

    Harness h;
    establish(h);
    QPointer<PaneController> machine = h.panes.defaultMachine();
    QPointer<PaneController> representation = h.panes.defaultRepresentation();
    QVERIFY(h.commit(machine, A));
    settle(h);
    const auto removedGroup = h.panes.createGroup("Old representation group");
    QVERIFY(!removedGroup.isEmpty());
    h.panes.setPaneGroup(representation->id(), removedGroup);
    settle(h);
    QCOMPARE(representation->groupId(), removedGroup);
    auto *model = qobject_cast<PageModel *>(machine->instructionsModel());
    QVERIFY(model);
    QVERIFY(model->count() > 0);
    const auto resolvesBefore = h.count("resolve");
    const auto disassembliesBefore = h.count("disasm");
    const auto decompilationsBefore = h.count("decompile");
    const auto xrefsBefore = h.count("xrefs");
    bool cancelled = false;
    QObject observer;
    connect(
        model, &QAbstractItemModel::modelReset, &observer,
        [&] {
          if (cancelled)
            return;
          cancelled = true;
          h.panes.cancelReads();
        },
        Qt::DirectConnection);

    QVERIFY(!h.panes.restoreMetadata(metadata));
    QVERIFY(cancelled);
    QVERIFY(machine && representation);
    QSet<QString> groupIds;
    for (const auto &value : h.panes.groups())
      groupIds.insert(value.toMap()["id"].toString());
    QVERIFY(!groupIds.contains(removedGroup));
    for (const auto &value : h.panes.items()) {
      auto *pane = h.panes.findPane(value.toMap()["id"].toString());
      QVERIFY(pane);
      QVERIFY(pane->groupId().isEmpty() || groupIds.contains(pane->groupId()));
    }
    QCOMPARE(machine->groupId(), QString("default"));
    QCOMPARE(representation->groupId(), QString("default"));
    QCOMPARE(h.panes.activePaneId(), QString("representation"));
    const auto machineAfterCancel = machine->selectedAddress();
    const auto representationAfterCancel = representation->selectedAddress();
    QVERIFY(machineAfterCancel != B);
    QVERIFY(representationAfterCancel != B);
    h.panes.setBinaryIdentity(Hash);
    h.panes.setPaneVisible(machine->id(), true);
    h.panes.setPaneVisible(representation->id(), true);
    settle(h);
    QCOMPARE(machine->selectedAddress(), machineAfterCancel);
    QCOMPARE(representation->selectedAddress(), representationAfterCancel);
    QCOMPARE(h.count("resolve"), resolvesBefore);
    QCOMPARE(h.count("disasm"), disassembliesBefore);
    QCOMPARE(h.count("decompile"), decompilationsBefore);
    QCOMPARE(h.count("xrefs"), xrefsBefore);
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());

    representation->navigate(D);
    settle(h);
    QCOMPARE(machine->selectedAddress(), D);
    QCOMPARE(representation->selectedAddress(), D);
    QCOMPARE(h.count("disasm", D), 1);
    QVERIFY(representation->representationText().contains(D));
    QVERIFY(!machine->busy());
    QVERIFY(!representation->busy());
  }
};

QTEST_MAIN(IndependentPaneTests)
#include "IndependentPaneTests.moc"
