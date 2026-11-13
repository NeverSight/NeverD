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
             QString code = {}, QString analysisState = {},
             QJsonObject background = {}, QString backgroundSequence = {}) {
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
    if (!background.isEmpty()) {
      response["background"] = background;
      response["background_sequence"] = backgroundSequence;
    }
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

/// An array of \p item alone.  Braces around one array are read as an array
/// holding it by some compilers and as its copy by others, and a delta of one
/// change is such an array.
QJsonArray only(const QJsonValue &item) {
  QJsonArray array;
  array.append(item);
  return array;
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
  void sourcePageKeptAcrossAReadContextIsNamed() {
    // A replica's dispatcher starts its read context anew at every change of
    // its owner's (AnalysisService::reset): the page it kept is named still,
    // where the same project goes on.
    Harness h;
    h.service.resetReadContext("project-one", "1");
    QJsonObject first;
    h.service.subscribe(text(), &h.owner,
                        [&](const QJsonObject &response) { first = response; });
    QTRY_COMPARE(h.sent.size(), 1);
    QCOMPARE(h.sent[0].payload["if_none_match"].toString(), QString());
    h.reply(0,
            {{"text", "kept text"}, {"etag", "tag-7-0-3"}, {"revision", "1"}});
    QTRY_COMPARE(first["payload"].toObject()["text"].toString(), "kept text");
    h.service.resetReadContext("project-one", "2");
    QJsonObject second;
    h.service.subscribe(text(), &h.owner, [&](const QJsonObject &response) {
      second = response;
    });
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].payload["if_none_match"].toString(), "tag-7-0-3");
    h.reply(1, {{"unchanged", true}, {"etag", "tag-7-0-3"}, {"revision", "2"}},
            "2");
    QTRY_COMPARE(second["status"].toString(), "ok");
    QCOMPARE(second["payload"].toObject()["text"].toString(), "kept text");
    QCOMPARE(second["revision"].toString(), "2");
    // Another project's read names none of this one's pages.
    h.service.resetReadContext("project-two", "1");
    QJsonObject third;
    h.service.subscribe(text(), &h.owner,
                        [&](const QJsonObject &response) { third = response; });
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].payload["if_none_match"].toString(), QString());
  }

  // The reply to a command, at \p revision, with what the worker answered of
  // the reads it carried.
  static void replyFollowed(Harness &h, int index, const QString &revision,
                            const QJsonArray &answers) {
    const auto request = h.sent.at(index);
    h.service.receive({{"protocol_major", 1},
                       {"type", "response"},
                       {"request_id", request.id},
                       {"operation", request.operation},
                       {"project_id", "project-one"},
                       {"revision", revision},
                       {"status", "ok"},
                       {"payload", QJsonObject{{"saved", true}}},
                       {"then", answers}});
  }

  void aListingsOwnPageReadsWhatChangedOfIt() {
    Harness h;
    establish(h);
    h.service.setPageDeltas(true);
    const Spec own{"listing",
                   {{"address", "0x1"}, {"before", 0}, {"after", 2}},
                   Spec::Latest,
                   {},
                   Spec::Visible};
    QJsonObject first, second;
    h.service.subscribe(own, &h.owner,
                        [&](const QJsonObject &response) { first = response; });
    QTRY_COMPARE(h.sent.size(), 1);
    QCOMPARE(h.sent.at(0).payload.value("if_none_match"), QJsonValue(""));
    h.reply(0, {{"anchor", 0},
                {"etag", "listing-1"},
                {"lines", QJsonArray{QJsonObject{{"text", "a"}},
                                     QJsonObject{{"text", "b"}}}}});
    QTRY_VERIFY(!first.isEmpty());
    // The same lines read again: the page kept is named, and what changed of
    // it is applied to it.
    h.service.subscribe(
        own, &h.owner, [&](const QJsonObject &response) { second = response; });
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent.at(1).payload.value("if_none_match"),
             QJsonValue("listing-1"));
    QVERIFY(h.sent.at(1).payload.value("page_delta").toBool());
    h.reply(1, {{"base", "listing-1"},
                {"etag", "listing-2"},
                {"delta",
                 only(QJsonArray{
                     "items", "lines",
                     QJsonArray{QJsonArray{"same", 1},
                                QJsonArray{"put", QJsonArray{QJsonObject{
                                                      {"text", "c"}}}}}})}});
    QTRY_VERIFY(!second.isEmpty());
    const auto page = second.value("payload").toObject();
    QCOMPARE(page.value("etag"), QJsonValue("listing-2"));
    QCOMPARE(page.value("lines"),
             QJsonValue(QJsonArray{QJsonObject{{"text", "a"}},
                                   QJsonObject{{"text", "c"}}}));
    // A page scrolling adds names none.
    h.service.subscribe(
        {"listing", {{"address", "0x9"}, {"before", 0}, {"after", 2}}},
        &h.owner, [](const QJsonObject &) {});
    QTRY_COMPARE(h.sent.size(), 3);
    QVERIFY(!h.sent.at(2).payload.contains("if_none_match"));
  }

  void aCachedReadTakesNoTurnBehindRunningReads() {
    Harness h;
    establish(h);
    QJsonObject first, second, third;
    h.service.subscribe(text(), &h.owner,
                        [&](const QJsonObject &response) { first = response; });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "page"}});
    QTRY_VERIFY(!first.isEmpty());
    // Another read runs: the page read again answers from the cache at once.
    h.service.subscribe({"overview", {}}, &h.owner, [](const QJsonObject &) {});
    QTRY_COMPARE(h.sent.size(), 2);
    h.service.subscribe(text(), &h.owner, [&](const QJsonObject &response) {
      second = response;
    });
    QTRY_VERIFY(!second.isEmpty());
    QCOMPARE(h.sent.size(), 2);
    QCOMPARE(second.value("payload"), first.value("payload"));
    // Behind an accepted command it waits, and reads what the command left.
    h.service.enqueueCommand("rename", {{"address", "0x1"}, {"name", "n"}},
                             &h.owner, {});
    h.service.subscribe(text(), &h.owner,
                        [&](const QJsonObject &response) { third = response; });
    QTest::qWait(20);
    QVERIFY(third.isEmpty());
    h.reply(1);
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent.at(2).operation, QStringLiteral("rename"));
    h.reply(2, {}, "2");
    QTRY_COMPARE(h.sent.size(), 4);
    QCOMPARE(h.sent.at(3).operation, QStringLiteral("decompile"));
    QVERIFY(third.isEmpty());
    h.reply(3, {{"text", "renamed"}}, "2");
    QTRY_VERIFY(!third.isEmpty());
    QCOMPARE(third.value("payload").toObject().value("text"),
             QJsonValue("renamed"));
  }

  void anEditCarriesTheReadsOfThePagesItsViewsShow() {
    // An edit carries the read of a page read at the revision it leaves, as
    // the view showing it makes it after the edit; the answer is that read's,
    // with nothing sent, and the page it gives is the one kept.
    Harness h;
    establish(h);
    h.service.setPageDeltas(true);
    QJsonObject first;
    h.service.subscribe(text(), &h.owner,
                        [&](const QJsonObject &response) { first = response; });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "line one\nline two"},
                {"etag", "tag-7-0-2"},
                {"revision", "1"}});
    QTRY_COMPARE(first["status"].toString(), "ok");
    bool edited = false;
    QJsonObject edit;
    h.service.enqueueCommand("rename", {{"address", "0x1"}, {"name", "n"}},
                             &h.owner, [&](const QJsonObject &response) {
                               edit = response;
                               edited = true;
                             });
    QTRY_COMPARE(h.sent.size(), 2);
    const auto carried = h.sent[1].payload["then"].toArray();
    QCOMPARE(carried.size(), 1);
    auto named = text().payload;
    named["if_none_match"] = "tag-7-0-2";
    named["page_delta"] = true;
    QCOMPARE(carried[0].toObject()["operation"].toString(), "decompile");
    QCOMPARE(carried[0].toObject()["payload"].toObject(), named);
    replyFollowed(
        h, 1, "2",
        QJsonArray{QJsonObject{
            {"status", "ok"},
            {"payload",
             QJsonObject{
                 {"delta", only(QJsonArray{"lines", "text", 2,
                                           only(QJsonArray{1, "line 2"})})},
                 {"base", "tag-7-0-2"},
                 {"etag", "tag-8-0-2"},
                 {"project_id", "project-one"},
                 {"revision", "2"}}}}});
    QTRY_VERIFY(edited);
    QVERIFY(!edit.contains("then"));
    QJsonObject second;
    h.service.subscribe(text(), &h.owner, [&](const QJsonObject &response) {
      second = response;
    });
    QTRY_COMPARE(second["status"].toString(), "ok");
    QCOMPARE(h.sent.size(), 2);
    QCOMPARE(second["payload"].toObject()["text"].toString(),
             "line one\nline 2");
    QCOMPARE(second["revision"].toString(), "2");
    // A later read names the page the edit's reply gave.
    h.service.listingChanged();
    h.service.subscribe(text(), &h.owner, [](const QJsonObject &) {});
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].payload["if_none_match"].toString(), "tag-8-0-2");
  }

  void aCarriedPageIsSaidAsItsEditsReplyIsTakenIn() {
    // The page an edit's reply carries is taken, and said, as the reply is:
    // ahead of the change the reply makes and of the turn that gives the edit
    // its own answer, so that a dispatcher reading through this one has it for
    // the read its view makes at the change.
    Harness h;
    establish(h);
    h.service.setPageDeltas(true);
    QJsonObject first;
    h.service.subscribe(text(), &h.owner,
                        [&](const QJsonObject &response) { first = response; });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "line one\nline two"},
                {"etag", "tag-7-0-2"},
                {"revision", "1"}});
    QTRY_COMPARE(first["status"].toString(), "ok");
    bool edited = false;
    h.service.enqueueCommand("rename", {{"address", "0x1"}, {"name", "n"}},
                             &h.owner,
                             [&](const QJsonObject &) { edited = true; });
    QTRY_COMPARE(h.sent.size(), 2);
    struct Said {
      QString operation, was, revisionThen;
      QJsonObject payload, response;
      qint64 bytes;
      bool edited;
    };
    QList<Said> said;
    QStringList order;
    connect(&h.service, &QueryService::contextChanged, &h.owner,
            [&] { order.append("changed"); });
    connect(&h.service, &QueryService::followed, &h.owner,
            [&](const QString &operation, const QJsonObject &payload,
                const QString &was, const QJsonObject &response, qint64 bytes) {
              order.append("followed");
              said.append({operation, was, h.service.revision(), payload,
                           response, bytes, edited});
            });
    const QJsonObject delta{
        {"status", "ok"},
        {"payload",
         QJsonObject{{"delta", only(QJsonArray{"lines", "text", 2,
                                               only(QJsonArray{1, "line 2"})})},
                     {"base", "tag-7-0-2"},
                     {"etag", "tag-8-0-2"},
                     {"project_id", "project-one"},
                     {"revision", "2"}}}};
    replyFollowed(h, 1, "2", QJsonArray{delta});
    // No turn of the event loop has passed.
    QCOMPARE(said.size(), 1);
    // The page ahead of the change: who hears of the change has it.
    QCOMPARE(order, QStringList({"followed", "changed"}));
    QVERIFY(!said[0].edited);
    QVERIFY(!edited);
    QCOMPARE(said[0].operation, "decompile");
    QCOMPARE(said[0].payload, text().payload);
    QCOMPARE(said[0].was, "tag-7-0-2");
    QCOMPARE(said[0].revisionThen, "2");
    QCOMPARE(said[0].response["status"].toString(), "ok");
    QCOMPARE(said[0].response["revision"].toString(), "2");
    QCOMPARE(said[0].response["project_id"].toString(), "project-one");
    QCOMPARE(said[0].response["payload"].toObject()["text"].toString(),
             "line one\nline 2");
    QCOMPARE(said[0].response["payload"].toObject()["etag"].toString(),
             "tag-8-0-2");
    QVERIFY(said[0].bytes > 0);
    QTRY_VERIFY(edited);
    // Said once: the answer the edit is given later says it no more.
    QTest::qWait(20);
    QCOMPARE(said.size(), 1);
    // A carried read the worker refused, or one whose page this one does not
    // keep, is not said.
    h.service.subscribe(text(), &h.owner, [](const QJsonObject &) {});
    QTest::qWait(5);
    h.service.enqueueCommand("rename", {{"address", "0x1"}, {"name", "m"}},
                             &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 3);
    replyFollowed(
        h, 2, "3",
        QJsonArray{QJsonObject{
            {"status", "error"},
            {"error", QJsonObject{{"code", "not_kept"}, {"message", "x"}}}}});
    QCOMPARE(said.size(), 1);
    QTRY_VERIFY(!h.service.hasPending());
    QCOMPARE(said.size(), 1);
  }

  void anAnswerGivenIsKeptAndAnswersTheReadsThatWait() {
    // A dispatcher that reads through another (a replica's, through its
    // owner's) is given a page that one read for it: the read of it that
    // waits is answered with nothing sent, later reads find it cached, and it
    // is the page a read at a later state names.
    Harness h;
    h.service.resetReadContext("project-one", "2");
    h.service.setPageDeltas(true);
    QJsonObject waiting;
    h.service.subscribe(text(), &h.owner, [&](const QJsonObject &response) {
      waiting = response;
    });
    const QJsonObject page{{"protocol_major", 1},
                           {"type", "response"},
                           {"status", "ok"},
                           {"operation", "decompile"},
                           {"project_id", "project-one"},
                           {"revision", "2"},
                           {"payload", QJsonObject{{"text", "given text"},
                                                   {"etag", "tag-9-0-1"},
                                                   {"revision", "2"}}}};
    // Not this state's, not an answer, or of a read whose answers are not
    // kept: refused, and nothing changes.
    auto other = page;
    other["revision"] = "3";
    QVERIFY(!h.service.give(text(), other));
    other = page;
    other["project_id"] = "project-two";
    QVERIFY(!h.service.give(text(), other));
    other = page;
    other["status"] = "error";
    QVERIFY(!h.service.give(text(), other));
    QVERIFY(!h.service.give({"history", {}}, page));
    QVERIFY(waiting.isEmpty());
    // Given before the read is sent: it answers that read, in a later turn.
    const auto answered = h.service.give(text(), page, 321);
    QVERIFY(answered.has_value());
    QCOMPARE(*answered, 1);
    QVERIFY(waiting.isEmpty());
    QTRY_COMPARE(waiting["status"].toString(), "ok");
    QCOMPARE(waiting["payload"].toObject()["text"].toString(), "given text");
    QCOMPARE(waiting["revision"].toString(), "2");
    QCOMPARE(waiting["operation"].toString(), "decompile");
    QTRY_VERIFY(!h.service.hasPending());
    QCOMPARE(h.sent.size(), 0);
    // A read made after it is answered from it.
    QJsonObject later;
    h.service.subscribe(text(), &h.owner,
                        [&](const QJsonObject &response) { later = response; });
    QTRY_COMPARE(later["status"].toString(), "ok");
    QCOMPARE(later["payload"].toObject()["text"].toString(), "given text");
    QCOMPARE(h.sent.size(), 0);
    // Another read is not answered by it, and none waits for it now.
    const auto none = h.service.give(text("0xffff000000000002"), page, 321);
    QVERIFY(none.has_value());
    QCOMPARE(*none, 0);
    // At a later state the page given is the one named.
    h.service.resetReadContext("project-one", "3");
    h.service.subscribe(text(), &h.owner, [](const QJsonObject &) {});
    QTRY_COMPARE(h.sent.size(), 1);
    QCOMPARE(h.sent[0].payload["if_none_match"].toString(), "tag-9-0-1");
    QCOMPARE(h.sent[0].payload["page_delta"].toBool(), true);
    // A read on its way when its answer is given runs to its end, and takes
    // the worker's own answer: what is given answers none that waits.
    const QJsonObject newer{{"protocol_major", 1},
                            {"type", "response"},
                            {"status", "ok"},
                            {"operation", "decompile"},
                            {"project_id", "project-one"},
                            {"revision", "3"},
                            {"payload", QJsonObject{{"text", "newer text"},
                                                    {"etag", "tag-9-1-1"},
                                                    {"revision", "3"}}}};
    QJsonObject running;
    h.service.subscribe(text(), &h.owner, [&](const QJsonObject &response) {
      running = response;
    });
    const auto beside = h.service.give(text(), newer, 400);
    QVERIFY(beside.has_value());
    QCOMPARE(*beside, 0);
    QTest::qWait(5);
    QVERIFY(running.isEmpty());
    h.reply(0,
            {{"text", "newer text"}, {"etag", "tag-9-1-1"}, {"revision", "3"}},
            "3");
    QTRY_COMPARE(running["payload"].toObject()["text"].toString(),
                 "newer text");
    QTRY_VERIFY(!h.service.hasPending());
    QCOMPARE(h.sent.size(), 1);
  }

  void aReadThatTakesAGivenAnswerIsSaidOnce() {
    // The dispatcher that was given an answer hears of the first read that
    // takes it, whether that read waited for it or came after it.
    const QJsonObject page{{"protocol_major", 1},
                           {"type", "response"},
                           {"status", "ok"},
                           {"operation", "decompile"},
                           {"project_id", "project-one"},
                           {"revision", "2"},
                           {"payload", QJsonObject{{"text", "given text"},
                                                   {"etag", "tag-9-0-1"},
                                                   {"revision", "2"}}}};
    for (const bool waits : {true, false}) {
      Harness h;
      h.service.resetReadContext("project-one", "2");
      int said = 0;
      connect(&h.service, &QueryService::givenRead, &h.owner, [&] { ++said; });
      QJsonObject first;
      const auto read = [&](QJsonObject &into) {
        h.service.subscribe(
            text(), &h.owner,
            [&into](const QJsonObject &response) { into = response; });
      };
      if (waits)
        read(first);
      QVERIFY(h.service.give(text(), page, 300).has_value());
      QCOMPARE(said, waits ? 1 : 0);
      if (!waits)
        read(first);
      QCOMPARE(said, 1);
      QTRY_COMPARE(first["status"].toString(), "ok");
      // A second read of it is one more read of a cached answer.
      QJsonObject second;
      read(second);
      QTRY_COMPARE(second["status"].toString(), "ok");
      QCOMPARE(said, 1);
      QCOMPARE(h.sent.size(), 0);
      // An answer the sender gave is no given one.
      QJsonObject other;
      h.service.subscribe(
          text("0xffff000000000002"), &h.owner,
          [&](const QJsonObject &response) { other = response; });
      QTRY_COMPARE(h.sent.size(), 1);
      h.reply(0,
              {{"text", "sent text"}, {"etag", "tag-5-0-1"}, {"revision", "2"}},
              "2");
      QTRY_COMPARE(other["status"].toString(), "ok");
      QJsonObject again;
      h.service.subscribe(
          text("0xffff000000000002"), &h.owner,
          [&](const QJsonObject &response) { again = response; });
      QTRY_COMPARE(again["status"].toString(), "ok");
      QCOMPARE(said, 1);
    }
  }

  void anAnswerThereAlreadyIsGivenAtOnceToTheOneThatAsksForIt() {
    // A reader that has just asked and is ready takes the answers its reads
    // have already without the turn each would take; a read another owner
    // waits for too keeps its turn, and one still on its way is not waited
    // for.
    Harness h;
    h.service.resetReadContext("project-one", "2");
    const QJsonObject page{{"protocol_major", 1},
                           {"type", "response"},
                           {"status", "ok"},
                           {"operation", "decompile"},
                           {"project_id", "project-one"},
                           {"revision", "2"},
                           {"payload", QJsonObject{{"text", "there already"},
                                                   {"etag", "tag-9-0-1"},
                                                   {"revision", "2"}}}};
    QVERIFY(h.service.give(text(), page, 300).has_value());
    QObject reader, other;
    QStringList got;
    h.service.subscribe(text(), &reader, [&](const QJsonObject &response) {
      got.append(response["payload"].toObject()["text"].toString());
    });
    QVERIFY(got.isEmpty());
    // Nothing of another owner's.
    QCOMPARE(h.service.deliverNow(&other), 0);
    QVERIFY(got.isEmpty());
    QCOMPARE(h.service.deliverNow(&reader), 1);
    QCOMPARE(got, QStringList({"there already"}));
    // Its turn, when it comes, gives it no second time.
    QTest::qWait(10);
    QCOMPARE(got.size(), 1);
    QCOMPARE(h.service.deliverNow(&reader), 0);
    // One on its way is not there yet.
    QString sent;
    h.service.subscribe(
        text("0xffff000000000002"), &reader, [&](const QJsonObject &response) {
          sent = response["payload"].toObject()["text"].toString();
        });
    QCOMPARE(h.service.deliverNow(&reader), 0);
    QTRY_COMPARE(h.sent.size(), 1);
    QCOMPARE(h.service.deliverNow(&reader), 0);
    h.reply(0,
            {{"text", "sent text"}, {"etag", "tag-5-0-1"}, {"revision", "2"}},
            "2");
    // The reply is in: its answer is there, and taken at once.
    QVERIFY(sent.isEmpty());
    QCOMPARE(h.service.deliverNow(&reader), 1);
    QCOMPARE(sent, "sent text");
    // A read two owners wait for is given to both in its turn.
    int mine = 0, theirs = 0;
    const auto shared = text("0xffff000000000004");
    h.service.subscribe(shared, &reader, [&](const QJsonObject &) { ++mine; });
    h.service.subscribe(shared, &other, [&](const QJsonObject &) { ++theirs; });
    QTRY_COMPARE(h.sent.size(), 2);
    h.reply(1, {{"text", "for both"}, {"etag", "tag-6-0-1"}, {"revision", "2"}},
            "2");
    QCOMPARE(h.service.deliverNow(&reader), 0);
    QCOMPARE(h.service.deliverNow(&other), 0);
    QCOMPARE(mine + theirs, 0);
    QTRY_COMPARE(mine, 1);
    QCOMPARE(theirs, 1);
  }

  void aReadAskedAtAnEditsChangeIsSentBehindItsReply() {
    // The worker is free once an edit's reply is in: a read of visible
    // content asked for while the change is said goes to it then, though the
    // edit is given its own answer a turn later, and still before that read's.
    const auto shown = [](const QString &address) {
      auto spec = text(address);
      spec.priority = Spec::Visible;
      return spec;
    };
    Harness h;
    establish(h);
    QStringList order;
    bool edited = false;
    h.service.enqueueCommand("rename", {{"address", "0x1"}, {"name", "n"}},
                             &h.owner, [&](const QJsonObject &) {
                               edited = true;
                               order.append("edit");
                             });
    QTRY_COMPARE(h.sent.size(), 1);
    QJsonObject read;
    qsizetype sentAtChange = -1;
    bool ask = true, answeringAtChange = false;
    const auto changed =
        connect(&h.service, &QueryService::contextChanged, &h.owner, [&] {
          if (!std::exchange(ask, false))
            return;
          // An ordinary read asked first keeps its turn; the visible one
          // behind it goes.
          h.service.subscribe(text("0xffff000000000009"), &h.owner,
                              [](const QJsonObject &) {});
          h.service.subscribe(shown("0xffff000000000001"), &h.owner,
                              [&](const QJsonObject &response) {
                                read = response;
                                order.append("read");
                              });
          const auto before = h.sent.size();
          h.service.sendAhead();
          sentAtChange = h.sent.size() - before;
          // Once: the read on its way is the one the worker has.
          h.service.sendAhead();
          QCOMPARE(h.sent.size() - before, sentAtChange);
          answeringAtChange = h.service.answeringCommand();
        });
    QVERIFY(!h.service.answeringCommand());
    h.reply(0, {{"saved", true}}, "2");
    // No turn of the event loop has passed: the edit's reply is in, its
    // change said, and its own answer still to be given.
    QVERIFY(answeringAtChange);
    QVERIFY(h.service.answeringCommand());
    QCOMPARE(sentAtChange, 1);
    QCOMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].operation, "decompile");
    QCOMPARE(h.sent[1].payload["address"].toString(), "0xffff000000000001");
    QCOMPARE(h.sent[1].expectedRevision, "2");
    QVERIFY(!edited);
    QTRY_VERIFY(edited);
    // Given: a read on its way is no edit being answered.
    QVERIFY(!h.service.answeringCommand());
    QVERIFY(read.isEmpty());
    h.reply(1, {{"text", "after"}, {"etag", "tag-1-0-1"}, {"revision", "2"}},
            "2");
    QTRY_COMPARE(read["payload"].toObject()["text"].toString(), "after");
    QCOMPARE(order, QStringList({"edit", "read"}));
    // The ordinary read goes in its turn.
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].payload["address"].toString(), "0xffff000000000009");
    h.reply(2, {{"text", "plain"}, {"etag", "tag-2-0-1"}, {"revision", "2"}},
            "2");
    QTRY_VERIFY(!h.service.hasPending());
    disconnect(changed);
    // Where only ordinary reads wait at a change, none is sent ahead: what
    // the command's answer has its owner ask for a reader goes before them.
    h.sent.clear();
    h.service.enqueueCommand("rename", {{"address", "0x1"}, {"name", "o"}},
                             &h.owner, [&](const QJsonObject &) {
                               h.service.subscribe(shown("0xffff000000000007"),
                                                   &h.owner,
                                                   [](const QJsonObject &) {});
                             });
    QTRY_COMPARE(h.sent.size(), 1);
    ask = true;
    qsizetype plainAhead = -1;
    const auto plain =
        connect(&h.service, &QueryService::contextChanged, &h.owner, [&] {
          if (!std::exchange(ask, false))
            return;
          h.service.subscribe(text("0xffff000000000008"), &h.owner,
                              [](const QJsonObject &) {});
          const auto before = h.sent.size();
          h.service.sendAhead();
          plainAhead = h.sent.size() - before;
        });
    h.reply(0, {{"saved", true}}, "3");
    QCOMPARE(plainAhead, 0);
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].payload["address"].toString(), "0xffff000000000007");
    h.reply(1, {{"text", "first"}, {"etag", "tag-3-0-1"}, {"revision", "3"}},
            "3");
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].payload["address"].toString(), "0xffff000000000008");
    h.reply(2, {{"text", "plain"}, {"etag", "tag-4-0-1"}, {"revision", "3"}},
            "3");
    QTRY_VERIFY(!h.service.hasPending());
    disconnect(plain);
    // At no other time: a read asked for outside a change takes its turn.
    h.sent.clear();
    h.service.subscribe(shown("0xffff00000000000a"), &h.owner,
                        [](const QJsonObject &) {});
    h.service.sendAhead();
    QCOMPARE(h.sent.size(), 0);
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "later"}, {"etag", "tag-5-0-1"}, {"revision", "3"}},
            "3");
    QTRY_VERIFY(!h.service.hasPending());
    // A command that waits is not passed: the read asked for at the first
    // one's change goes after it.
    h.sent.clear();
    h.service.enqueueCommand("rename", {{"address", "0x1"}, {"name", "a"}},
                             &h.owner, {});
    h.service.enqueueCommand("rename", {{"address", "0x1"}, {"name", "b"}},
                             &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 1);
    ask = true;
    qsizetype passed = -1;
    connect(&h.service, &QueryService::contextChanged, &h.owner, [&] {
      if (!std::exchange(ask, false))
        return;
      h.service.subscribe(shown("0xffff000000000003"), &h.owner,
                          [](const QJsonObject &) {});
      const auto before = h.sent.size();
      h.service.sendAhead();
      passed = h.sent.size() - before;
    });
    h.reply(0, {{"saved", true}}, "4");
    QCOMPARE(passed, 0);
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].operation, "rename");
    QCOMPARE(h.sent[1].payload["name"].toString(), "b");
    h.reply(1, {{"saved", true}}, "5");
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].operation, "decompile");
  }

  void aCarriedAnswerIsGivenWhileAnotherReadIsOnItsWay() {
    // The reads the views make after an edit wait behind it together: the one
    // the edit's reply answered is given at once, beside the one the worker
    // is sent, rather than after that one's answer.
    Harness h;
    establish(h);
    h.service.setPageDeltas(true);
    const Spec listing{"listing",
                       {{"address", "0x1"}, {"before", 0}, {"after", 2}},
                       Spec::Latest,
                       {},
                       Spec::Visible};
    QJsonObject first;
    h.service.subscribe(text(), &h.owner,
                        [&](const QJsonObject &response) { first = response; });
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "line one\nline two"},
                {"etag", "tag-7-0-2"},
                {"revision", "1"}});
    QTRY_COMPARE(first["status"].toString(), "ok");
    QJsonObject listed, shown;
    h.service.enqueueCommand("rename", {{"address", "0x1"}, {"name", "n"}},
                             &h.owner, [](const QJsonObject &) {});
    // As a listing and a source view do at the new revision: the listing
    // first, both behind the edit until it is answered.
    connect(&h.service, &QueryService::contextChanged, &h.owner, [&] {
      h.service.subscribe(listing, &h.owner, [&](const QJsonObject &response) {
        listed = response;
      });
      h.service.subscribe(text(), &h.owner, [&](const QJsonObject &response) {
        shown = response;
      });
    });
    QTRY_COMPARE(h.sent.size(), 2);
    replyFollowed(
        h, 1, "2",
        QJsonArray{QJsonObject{
            {"status", "ok"},
            {"payload",
             QJsonObject{
                 {"delta", only(QJsonArray{"lines", "text", 2,
                                           only(QJsonArray{1, "line 2"})})},
                 {"base", "tag-7-0-2"},
                 {"etag", "tag-8-0-2"},
                 {"project_id", "project-one"},
                 {"revision", "2"}}}}});
    // The listing is sent, and the view has its page with no answer to it.
    QTRY_COMPARE(shown["status"].toString(), "ok");
    QCOMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].operation, QStringLiteral("listing"));
    QVERIFY(listed.isEmpty());
    QCOMPARE(shown["payload"].toObject()["text"].toString(),
             "line one\nline 2");
    h.reply(2, {{"lines", QJsonArray{}}, {"etag", "list-1"}}, "2");
    QTRY_COMPARE(listed["status"].toString(), "ok");
    QCOMPARE(h.sent.size(), 3);
  }

  void theNextReadIsSentBeforeAnAnswerIsTakenInAgain() {
    // A reply's subscribers are answered and the read that waits is sent in
    // the same turn: what they queue for the next turn finds it on its way.
    Harness h;
    establish(h);
    bool sentByNextTurn = false, turned = false;
    h.service.subscribe(text("0x1000"), &h.owner, [&](const QJsonObject &) {
      QTimer::singleShot(0, &h.owner, [&] {
        sentByNextTurn = h.sent.size() == 2;
        turned = true;
      });
    });
    h.service.subscribe(text("0x2000"), &h.owner, [](const QJsonObject &) {});
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "first"}});
    QTRY_VERIFY(turned);
    QVERIFY(sentByNextTurn);
    h.reply(1, {{"text", "second"}});
    QTRY_VERIFY(!h.service.hasPending());
  }

  void aReplyWeighsWhatItCameIn() {
    // The transport says how many bytes a reply came in, and the cache weighs
    // it by them in place of writing it out again; a subscriber being answered
    // is told (deliveredBytes), for a dispatcher that passes the answer on.
    QueryService::Limits limits;
    limits.cacheBytes = 2400;
    Harness h(limits);
    establish(h);
    qint64 told = -1;
    const auto read = [&](const QString &address, const QString &value,
                          qint64 bytes, bool sent) {
      const auto before = h.sent.size();
      int calls = 0;
      told = -1;
      h.service.subscribe(text(address), &h.owner, [&](const auto &) {
        ++calls;
        told = h.service.deliveredBytes();
      });
      if (sent) {
        QTRY_COMPARE(h.sent.size(), before + 1);
        const auto request = h.sent.at(int(before));
        h.service.receive({{"protocol_major", 1},
                           {"type", "response"},
                           {"request_id", request.id},
                           {"operation", request.operation},
                           {"project_id", "project-one"},
                           {"revision", "1"},
                           {"status", "ok"},
                           {"payload", QJsonObject{{"text", value}}}},
                          bytes);
      }
      QTRY_COMPARE(calls, 1);
      QTRY_VERIFY(!h.service.hasPending());
      QCOMPARE(h.sent.size(), before + (sent ? 1 : 0));
      QCOMPARE(h.service.deliveredBytes(), qint64(0));
    };
    // Written out, this reply is past what the cache holds; said to weigh
    // little, it is kept, and its next read is answered by it and told so.
    read("0x1000", QString(2048, 'a'), 300, true);
    QCOMPARE(told, qint64(300));
    read("0x1000", QString(2048, 'a'), 0, false);
    QCOMPARE(told, qint64(300));
    // And a small one said to weigh much is not kept.
    read("0x2000", QStringLiteral("b"), 5000, true);
    QCOMPARE(told, qint64(5000));
    read("0x2000", QStringLiteral("b"), 5000, true);
    QCOMPARE(told, qint64(5000));
  }

  void aCarriedReadTheWorkerRefusedIsMadeAgain() {
    // A read the worker answered with an error is the view's to make: it names
    // the page it kept, which no answer replaced.
    Harness h;
    establish(h);
    h.service.subscribe(text(), &h.owner, [](const QJsonObject &) {});
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "kept"}, {"etag", "tag-7-0-1"}, {"revision", "1"}});
    QTRY_VERIFY(!h.service.hasPending());
    h.service.enqueueCommand("annotation_set", {{"address", "0x1"}}, &h.owner,
                             {});
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].payload["then"].toArray().size(), 1);
    replyFollowed(h, 1, "2",
                  QJsonArray{QJsonObject{
                      {"status", "error"},
                      {"error", QJsonObject{{"code", "not_kept"},
                                            {"message", "No session"}}}}});
    QTRY_VERIFY(!h.service.hasPending());
    h.service.subscribe(text(), &h.owner, [](const QJsonObject &) {});
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].payload["if_none_match"].toString(), "tag-7-0-1");
  }

  void sourcePageKeptAcrossARevisionIsNamedAndTakenBack() {
    Harness h;
    establish(h);
    QJsonObject first;
    h.service.subscribe(text(), &h.owner,
                        [&](const QJsonObject &response) { first = response; });
    QTRY_COMPARE(h.sent.size(), 1);
    // A first read names no page, and asks for the page's name.
    QVERIFY(h.sent[0].payload.contains("if_none_match"));
    QCOMPARE(h.sent[0].payload["if_none_match"].toString(), QString());
    h.reply(0,
            {{"text", "kept text"}, {"etag", "tag-7-0-3"}, {"revision", "1"}});
    QTRY_COMPARE(first["payload"].toObject()["text"].toString(), "kept text");
    // An edit moves the revision; the next read names the kept page, and the
    // worker's word that it stands gives it back under the new revision.
    h.service.enqueueCommand("rename", {{"address", "0x1"}, {"name", "x"}},
                             &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 2);
    h.reply(1, {}, "2");
    QTRY_VERIFY(!h.service.hasPending());
    QJsonObject second;
    h.service.subscribe(text(), &h.owner, [&](const QJsonObject &response) {
      second = response;
    });
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].payload["if_none_match"].toString(), "tag-7-0-3");
    h.reply(2, {{"unchanged", true}, {"etag", "tag-7-0-3"}, {"revision", "2"}},
            "2");
    QTRY_COMPARE(second["status"].toString(), "ok");
    QCOMPARE(second["payload"].toObject()["text"].toString(), "kept text");
    QCOMPARE(second["payload"].toObject()["revision"].toString(), "2");
    QCOMPARE(second["revision"].toString(), "2");
    // A word for a page this client does not keep forgets the kept page, and
    // the page is read whole.
    h.service.enqueueCommand("rename", {{"address", "0x1"}, {"name", "y"}},
                             &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 4);
    h.reply(3, {}, "3");
    QTRY_VERIFY(!h.service.hasPending());
    QJsonObject third;
    h.service.subscribe(text(), &h.owner,
                        [&](const QJsonObject &response) { third = response; });
    QTRY_COMPARE(h.sent.size(), 5);
    h.reply(4, {{"unchanged", true}, {"etag", "another"}, {"revision", "3"}},
            "3");
    QTRY_COMPARE(h.sent.size(), 6);
    QCOMPARE(h.sent[5].operation, "decompile");
    QCOMPARE(h.sent[5].payload["if_none_match"].toString(), QString());
    QVERIFY(third.isEmpty());
    h.reply(5,
            {{"text", "read whole"}, {"etag", "tag-9-0-3"}, {"revision", "3"}},
            "3");
    QTRY_COMPARE(third["payload"].toObject()["text"].toString(), "read whole");
    QCOMPARE(third["status"].toString(), "ok");
    // The page read whole is the one kept; a worker that names a page again
    // when none was named is refused rather than asked once more.
    h.service.enqueueCommand("rename", {{"address", "0x1"}, {"name", "z"}},
                             &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 7);
    h.reply(6, {}, "4");
    QTRY_VERIFY(!h.service.hasPending());
    QJsonObject fourth;
    h.service.subscribe(text(), &h.owner, [&](const QJsonObject &response) {
      fourth = response;
    });
    QTRY_COMPARE(h.sent.size(), 8);
    QCOMPARE(h.sent[7].payload["if_none_match"].toString(), "tag-9-0-3");
    h.reply(7, {{"unchanged", true}, {"etag", "another"}, {"revision", "4"}},
            "4");
    QTRY_COMPARE(h.sent.size(), 9);
    QCOMPARE(h.sent[8].payload["if_none_match"].toString(), QString());
    h.reply(8, {{"unchanged", true}, {"etag", "another"}, {"revision", "4"}},
            "4");
    QTRY_COMPARE(errorCode(fourth), "invalid_response");
    QCOMPARE(h.sent.size(), 9);
  }

  void sourcePageKeptTakesWhatChanged() {
    Harness h;
    establish(h);
    h.service.setPageDeltas(true);
    QJsonObject first;
    h.service.subscribe(text(), &h.owner,
                        [&](const QJsonObject &response) { first = response; });
    QTRY_COMPARE(h.sent.size(), 1);
    // A first read keeps nothing to apply changes to.
    QVERIFY(!h.sent[0].payload.contains("page_delta"));
    const QJsonArray names{
        QJsonObject{{"begin_byte", 0}, {"end_byte", 1}, {"identity", "0x1"}},
        QJsonObject{{"begin_byte", 2}, {"end_byte", 3}},
        QJsonObject{{"begin_byte", 4}, {"end_byte", 5}, {"identity", "0x1"}}};
    h.reply(0, {{"text", "a\nb\nc"},
                {"code_names", names},
                {"etag", "tag-7-0-3"},
                {"revision", "1"}});
    QTRY_COMPARE(first["payload"].toObject()["text"].toString(), "a\nb\nc");
    h.service.enqueueCommand("annotation_set", {{"address", "0x1"}}, &h.owner,
                             {});
    QTRY_COMPARE(h.sent.size(), 2);
    h.reply(1, {}, "2");
    QTRY_VERIFY(!h.service.hasPending());
    // The next read names the kept page and takes what changed of it: a line,
    // the identity every local of the text holds, and the names after it moved.
    QJsonObject second;
    h.service.subscribe(text(), &h.owner, [&](const QJsonObject &response) {
      second = response;
    });
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].payload["if_none_match"].toString(), "tag-7-0-3");
    QVERIFY(h.sent[2].payload["page_delta"].toBool());
    const QJsonArray changes{
        QJsonArray{"lines", "text", 3, only(QJsonArray{0, "a // x"})},
        QJsonArray{"each", "code_names", QJsonObject{{"identity", "0x2"}}},
        QJsonArray{"items", "code_names",
                   QJsonArray{QJsonArray{"same", 1},
                              QJsonArray{"shift", 2,
                                         QJsonObject{{"begin_byte", 5},
                                                     {"end_byte", 5}}}}}};
    h.reply(2,
            {{"delta", changes},
             {"base", "tag-7-0-3"},
             {"etag", "tag-8-0-3"},
             {"revision", "2"}},
            "2");
    QTRY_COMPARE(second["status"].toString(), "ok");
    const auto page = second["payload"].toObject();
    QCOMPARE(page["text"].toString(), "a // x\nb\nc");
    QCOMPARE(page["code_names"].toArray()[0].toObject()["end_byte"].toInt(), 1);
    QCOMPARE(page["code_names"].toArray()[2].toObject()["begin_byte"].toInt(),
             9);
    QCOMPARE(page["code_names"].toArray()[0].toObject()["identity"].toString(),
             "0x2");
    QCOMPARE(page["code_names"].toArray()[2].toObject()["identity"].toString(),
             "0x2");
    QVERIFY(!page["code_names"].toArray()[1].toObject().contains("identity"));
    QCOMPARE(page["etag"].toString(), "tag-8-0-3");
    QCOMPARE(page["revision"].toString(), "2");
    // The page as changed is the one kept: the next read names it.
    h.service.enqueueCommand("annotation_set", {{"address", "0x2"}}, &h.owner,
                             {});
    QTRY_COMPARE(h.sent.size(), 4);
    h.reply(3, {}, "3");
    QTRY_VERIFY(!h.service.hasPending());
    QJsonObject third;
    h.service.subscribe(text(), &h.owner,
                        [&](const QJsonObject &response) { third = response; });
    QTRY_COMPARE(h.sent.size(), 5);
    QCOMPARE(h.sent[4].payload["if_none_match"].toString(), "tag-8-0-3");
    // Changes that do not fit the kept page are not taken: the page is read
    // whole, with no changes asked for.
    h.reply(4,
            {{"delta", only(QJsonArray{"lines", "text", 9, QJsonArray{}})},
             {"base", "tag-8-0-3"},
             {"etag", "tag-9-0-3"},
             {"revision", "3"}},
            "3");
    QTRY_COMPARE(h.sent.size(), 6);
    QCOMPARE(h.sent[5].payload["if_none_match"].toString(), QString());
    QVERIFY(!h.sent[5].payload.contains("page_delta"));
    QVERIFY(third.isEmpty());
    h.reply(
        5, {{"text", "a // y\nb\nc"}, {"etag", "tag-9-0-3"}, {"revision", "3"}},
        "3");
    QTRY_COMPARE(third["payload"].toObject()["text"].toString(),
                 "a // y\nb\nc");
    QCOMPARE(third["payload"].toObject()["etag"].toString(), "tag-9-0-3");
  }

  void sectionEditsInvalidateCachedCodeAndAnalysis() {
    Harness h;
    establish(h);
    h.service.subscribe(text(), &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "original"}}, "1", "project-one", "ok", {},
            "complete");
    QTRY_VERIFY(!h.service.hasPending());
    QVERIFY(h.service.analysisComplete());
    const auto epoch = h.service.sessionEpoch();
    for (const QString operation : {QString("analysis_section_add"),
                                    QString("analysis_section_remove")}) {
      const int index = h.sent.size();
      QVERIFY(h.service.enqueueCommand(operation, {}, &h.owner, {}));
      QTRY_COMPARE(h.sent.size(), index + 1);
      h.reply(index, {}, QString::number(index + 2), "project-one", "ok", {},
              "not_analyzed");
      QTRY_VERIFY(!h.service.hasPending());
      QVERIFY(!h.service.analysisComplete());
      QCOMPARE(h.service.sessionEpoch(), epoch);
      h.service.subscribe(text(), &h.owner, {});
      QTRY_COMPARE(h.sent.size(), index + 2);
      h.reply(index + 1, {{"text", "changed"}}, QString::number(index + 2),
              "project-one", "ok", {}, "complete");
      QTRY_VERIFY(!h.service.hasPending());
      QVERIFY(h.service.analysisComplete());
    }
    // Writes cannot enter through a read-only subscription.
    QJsonObject refused;
    h.service.subscribe(
        {"analysis_section_add", {}}, &h.owner,
        [&](const QJsonObject &response) { refused = response; });
    QTRY_VERIFY(!refused.isEmpty());
    QCOMPARE(refused.value("status").toString(), QString("error"));
  }

  void backgroundSnapshotFollowsSubscribersAndIsNotReplayedByCache() {
    Harness h;
    QStringList order;
    QSignalSpy snapshots(&h.service, &QueryService::backgroundSnapshotReady);
    connect(&h.service, &QueryService::backgroundSnapshotReady, &h.owner,
            [&](const QJsonObject &) { order.append("snapshot"); });
    h.service.enqueueCommand("open", {}, &h.owner, [&](const QJsonObject &) {
      order.append("open");
    });
    QTRY_COMPARE(h.sent.size(), 1);
    const QJsonObject background{{"generation", "new-listing"}};
    h.reply(0, {}, "1", "project-one", "ok", {}, {}, background, "4");
    QVERIFY(order.isEmpty());
    QTRY_COMPARE(order, (QStringList{"open", "snapshot"}));
    QCOMPARE(snapshots[0][0].toJsonObject().value("background"), background);
    const Spec spec{"functions", {{"limit", 3}}};
    int deliveries = 0;
    const auto complete = [&](const QJsonObject &) { ++deliveries; };
    h.service.subscribe(spec, &h.owner, complete);
    QTRY_COMPARE(h.sent.size(), 2);
    h.reply(1, {}, "1", "project-one", "ok", {}, {}, background, "5");
    QTRY_COMPARE(deliveries, 1);
    QCOMPARE(snapshots.size(), 2);
    h.service.subscribe(spec, &h.owner, complete);
    QTRY_COMPARE(deliveries, 2);
    QCOMPARE(h.sent.size(), 2);
    QCOMPARE(snapshots.size(), 2);
  }

  void backgroundSnapshotDoesNotEscapeRetirementOrDestruction_data() {
    QTest::addColumn<bool>("destroy");
    QTest::newRow("reset-in-subscriber") << false;
    QTest::newRow("destroy-in-subscriber") << true;
  }

  void backgroundSnapshotDoesNotEscapeRetirementOrDestruction() {
    QFETCH(bool, destroy);
    auto h = std::make_unique<Harness>();
    establish(*h);
    int publications = 0;
    connect(&h->service, &QueryService::backgroundSnapshotReady, this,
            [&](const QJsonObject &) { ++publications; });
    QObject owner;
    bool delivered = false;
    h->service.subscribe({"listing", {}}, &owner, [&](const QJsonObject &) {
      delivered = true;
      if (destroy)
        h.reset();
      else
        h->service.resetSession();
    });
    QTRY_COMPARE(h->sent.size(), 1);
    h->reply(0, {}, "1", "project-one", "ok", {}, {},
             {{"generation", "new-listing"}}, "4");
    QTRY_VERIFY(delivered);
    QCOMPARE(publications, 0);
  }

  void backgroundSnapshotRejectsUntrustedReplies_data() {
    QTest::addColumn<QString>("kind");
    for (const auto &kind :
         {"wire", "operation", "project", "admission", "cancelled", "progress"})
      QTest::newRow(kind) << QString::fromLatin1(kind);
  }

  void backgroundSnapshotConsumerCanDestroyTheService() {
    auto h = std::make_unique<Harness>();
    establish(*h);
    connect(&h->service, &QueryService::backgroundSnapshotReady, this,
            [&](const QJsonObject &) { h.reset(); });
    h->service.subscribe({"listing", {}}, &h->owner, {});
    QTRY_COMPARE(h->sent.size(), 1);
    h->reply(0, {}, "1", "project-one", "ok", {}, {},
             {{"generation", "new-listing"}}, "4");
    QTRY_VERIFY(!h);
  }

  void backgroundSnapshotRejectsUntrustedReplies() {
    QFETCH(QString, kind);
    Harness h;
    establish(h);
    QSignalSpy snapshots(&h.service, &QueryService::backgroundSnapshotReady);
    h.service.subscribe({"listing", {}}, &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 1);
    QJsonObject response{{"type", "response"},
                         {"request_id", h.sent[0].id},
                         {"operation", "listing"},
                         {"project_id", "project-one"},
                         {"revision", "1"},
                         {"status", "ok"},
                         {"background", QJsonObject{{"generation", "bogus"}}},
                         {"background_sequence", "4"}};
    if (kind == "wire")
      response["request_id"] = "unowned";
    else if (kind == "operation")
      response["operation"] = "functions";
    else if (kind == "project")
      response["project_id"] = "another-project";
    else if (kind == "admission") {
      response["status"] = "error";
      response["error"] = QJsonObject{{"code", "queue_full"}};
    } else
      response["status"] = kind;
    h.service.receive(response);
    QCoreApplication::processEvents();
    QCOMPARE(snapshots.size(), 0);
  }

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

  void aCommandThatLeavesIsSentAsItIsEnqueued() {
    // A command takes the turn after the one that enqueues it, with whatever
    // else that turn enqueued.  One that leaves is with the worker when the
    // call returns, ahead of those who hear that a command waits; behind a
    // request the worker has, or a job that waits, it keeps its place.
    Harness h;
    h.service.resetReadContext("project-one", "1");
    QStringList heard;
    QObject::connect(&h.service, &QueryService::pendingChanged, &h.owner,
                     [&] { heard.append(QString::number(h.sent.size())); });
    QVERIFY(h.service.enqueueCommand("save", {}, &h.owner,
                                     [](const QJsonObject &) {}));
    QCOMPARE(h.sent.size(), 0);
    QCOMPARE(heard, QStringList{QStringLiteral("0")});
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {}, "1");
    QTRY_VERIFY(!h.service.hasPending());
    heard.clear();
    QVERIFY(h.service.enqueueCommand(
        "rename", {{"address", "0x1000"}, {"name", "leaves"}}, &h.owner,
        [](const QJsonObject &) {}, {}, true));
    // Sent before anyone heard of it.
    QCOMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].operation, QStringLiteral("rename"));
    QCOMPARE(heard.value(0), QStringLiteral("2"));
    // One that leaves behind it waits for the worker, in its turn.
    QVERIFY(h.service.enqueueCommand(
        "rename", {{"address", "0x2000"}, {"name", "waits"}}, &h.owner,
        [](const QJsonObject &) {}, {}, true));
    QCOMPARE(h.sent.size(), 2);
    h.reply(1, {}, "2");
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].payload["name"].toString(), QStringLiteral("waits"));
    QCOMPARE(h.sent[2].expectedRevision, QStringLiteral("2"));
    h.reply(2, {}, "3");
    QTRY_VERIFY(!h.service.hasPending());
    // Behind a read that waits it does not pass the read.
    h.service.subscribe(text(), &h.owner, [](const QJsonObject &) {});
    QVERIFY(h.service.enqueueCommand(
        "rename", {{"address", "0x3000"}, {"name", "behind"}}, &h.owner,
        [](const QJsonObject &) {}, {}, true));
    QCOMPARE(h.sent.size(), 4);
    QCOMPARE(h.sent[3].operation, text().operation);
    h.reply(3, {{"text", "read"}}, "3");
    QTRY_COMPARE(h.sent.size(), 5);
    QCOMPARE(h.sent[4].payload["name"].toString(), QStringLiteral("behind"));
    h.reply(4, {}, "4");
    QTRY_VERIFY(!h.service.hasPending());
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
    QCOMPARE(h.service.pendingReadCount(), 1);
    h.service.unsubscribe(first);
    QCOMPARE(h.service.pendingReadCount(), 1);
    QCOMPARE(h.sent.size(), 1);
    h.reply(0);
    QTRY_COMPARE(secondCalls, 1);
    QCOMPARE(h.service.pendingReadCount(), 0);
    QCOMPARE(firstCalls, 0);
    QCOMPARE(h.sent.size(), 1);
  }

  void lastSubscriberCancellationWaitsForTheOriginalTerminalResponse() {
    Harness h;
    establish(h);
    const auto id = h.service.subscribe(text(), &h.owner, {});
    h.service.enqueueCommand("save", {}, &h.owner, {});
    QTRY_COMPARE(h.sent.size(), 1);
    QCOMPARE(h.service.pendingReadCount(), 1);
    h.service.unsubscribe(id);
    h.service.unsubscribe(id);
    // The cancelled wire request still blocks its dispatcher, but carries no
    // useful analysis load when selecting a read replica for new work.
    QVERIFY(h.service.hasPending());
    QCOMPARE(h.service.pendingReadCount(), 0);
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
    QCOMPARE(h.service.pendingReadCount(), 2);
    h.service.unsubscribe(queued);
    QCOMPARE(h.service.pendingReadCount(), 1);
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

  void aBackgroundListingChangeRetiresCachedAndFlyingReads() {
    // Function discovery changes what reads list without a new revision.
    Harness h;
    establish(h);
    QList<QString> texts;
    const auto collect = [&](const QJsonObject &response) {
      texts.append(response["payload"].toObject()["text"].toString());
    };
    h.service.subscribe(text(), &h.owner, collect);
    QTRY_COMPARE(h.sent.size(), 1);
    h.reply(0, {{"text", "cached before"}});
    QTRY_COMPARE(texts.size(), 1);
    h.service.subscribe(text("0x2000"), &h.owner, collect);
    QTRY_COMPARE(h.sent.size(), 2);
    h.service.listingChanged();
    // Neither the cached read nor the one in flight answers a read made
    // after the change.
    h.service.subscribe(text(), &h.owner, collect);
    h.service.subscribe(text("0x2000"), &h.owner, collect);
    h.reply(1, {{"text", "flying before"}});
    QTRY_COMPARE(h.sent.size(), 3);
    h.reply(2, {{"text", "cached after"}});
    QTRY_COMPARE(h.sent.size(), 4);
    h.reply(3, {{"text", "flying after"}});
    QTRY_COMPARE(texts.size(), 4);
    QCOMPARE(texts, (QList<QString>{"cached before", "flying before",
                                    "cached after", "flying after"}));
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

  void visibleNavigationPrecedesInventoryReads() {
    Harness h;
    establish(h);
    h.service.subscribe({"functions", {}}, &h.owner, {});
    h.service.subscribe({"imports", {}}, &h.owner, {});
    h.service.subscribe({"overview", {}, Spec::Latest, {}, Spec::Visible},
                        &h.owner, [&](const auto &) {
                          h.service.subscribe({"listing",
                                               {{"address", "0x1000"}},
                                               Spec::Latest,
                                               {},
                                               Spec::Visible},
                                              &h.owner, {});
                        });
    QTRY_COMPARE(h.sent.size(), 1);
    QCOMPARE(h.sent[0].operation, "overview");
    h.reply(0);
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].operation, "listing");
    h.reply(1);
    QTRY_COMPARE(h.sent.size(), 3);
    QCOMPARE(h.sent[2].operation, "functions");
    h.reply(2);
    QTRY_COMPARE(h.sent.size(), 4);
    QCOMPARE(h.sent[3].operation, "imports");
    h.reply(3);
    QTRY_VERIFY(!h.service.hasPending());
  }

  void visibleSubscriberPromotesTheSharedQueuedRead() {
    Harness h;
    establish(h);
    int replies = 0;
    h.service.subscribe({"functions", {}}, &h.owner, {});
    h.service.subscribe({"overview", {}}, &h.owner,
                        [&](const auto &) { ++replies; });
    h.service.subscribe({"overview", {}, Spec::Latest, {}, Spec::Visible},
                        &h.owner, [&](const auto &) { ++replies; });
    QTRY_COMPARE(h.sent.size(), 1);
    QCOMPARE(h.sent[0].operation, "overview");
    h.reply(0);
    QTRY_COMPARE(replies, 2);
    QTRY_COMPARE(h.sent.size(), 2);
    QCOMPARE(h.sent[1].operation, "functions");
    h.reply(1);
    QTRY_VERIFY(!h.service.hasPending());
    QCOMPARE(h.sent.size(), 2);
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
    auto visible = text();
    visible.priority = Spec::Visible;
    h.service.subscribe(visible, &h.owner, [&](const auto &r) {
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
