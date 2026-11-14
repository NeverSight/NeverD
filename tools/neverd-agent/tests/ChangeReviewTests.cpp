//===- ChangeReviewTests.cpp - Real ND apply/save/undo contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "runtime/ChangeReview.h"

#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

using namespace neverd::agent;
namespace {
QJsonObject awaitReply(std::function<void(WorkerSession::Done)> start) {
  QEventLoop loop;
  QTimer deadline;
  deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  QJsonObject result;
  bool finished = false;
  start([&](QJsonObject reply) {
    result = reply;
    finished = true;
    loop.quit();
  });
  deadline.start(25000);
  if (!finished)
    loop.exec();
  return finished ? result : QJsonObject{{"error", "Test deadline exceeded."}};
}
} // namespace
class ChangeReviewTests final : public QObject {
  Q_OBJECT
  QString Worker;
  std::unique_ptr<QTemporaryDir> Directory;
  Project Owner;
  QJsonObject propose(const QString &kind, const QString &after) {
    return awaitReply([&](auto done) {
      ChangeReview::propose(
          Owner,
          {{"object_id", Owner.Objects[0].Id},
           {"kind", kind},
           {"address", "ValidatePacket"},
           {"after", after},
           {"rationale", "Checked fixture declaration"},
           {"type_names",
            kind == "types" ? QJsonArray{"AgentPacketHeader"} : QJsonArray{}},
           {"evidence_ids", QJsonArray{}}},
          Worker, 15, this, done);
    });
  }
  QJsonObject apply(const QJsonObject &proposal, bool undo = false) {
    return awaitReply([&](auto done) {
      ChangeReview::apply(Owner, proposal["id"].toString(), undo, Worker, 15,
                          this, done);
    });
  }
private slots:
  void init() {
    Worker = qEnvironmentVariable("NEVERD_AGENT_ND_WORKER");
    const auto fixture = qEnvironmentVariable("NEVERD_AGENT_FIXTURE");
    if (Worker.isEmpty() || fixture.isEmpty())
      QSKIP("Set NEVERD_AGENT_ND_WORKER and NEVERD_AGENT_FIXTURE to run real "
            "ND command tests.");
    Directory = std::make_unique<QTemporaryDir>();
    QString error;
    auto project =
        ProjectStore::create(Directory->filePath("project"), "Review", error);
    QVERIFY2(project.has_value(), qPrintable(error));
    Owner = *project;
    const auto path = Directory->filePath("sample.dll");
    QVERIFY(QFile::copy(fixture, path));
    QVERIFY2(
        ProjectStore::addObject(Owner, ProjectStore::fingerprint(path), error),
        qPrintable(error));
  }
  void applyAndUndo_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("after");
    QTest::newRow("comment")
        << QString("comment")
        << QString("Returns -5 when the payload checksum differs.");
    QTest::newRow("rename")
        << QString("rename") << QString("ValidatePacketReviewed");
    QTest::newRow("function_type")
        << QString("function_type")
        << QString("int ValidatePacket(const unsigned char *data, unsigned int "
                   "length);");
    QTest::newRow("types")
        << QString("types")
        << QString("struct AgentPacketHeader { unsigned short magic; unsigned "
                   "char kind; unsigned char flags; unsigned short size; "
                   "unsigned short checksum; };");
  }
  void applyAndUndo() {
    QFETCH(QString, kind);
    QFETCH(QString, after);
    const auto proposed = propose(kind, after);
    QVERIFY2(!proposed.contains("error"),
             QJsonDocument(proposed).toJson().constData());
    QCOMPARE(proposed["status"].toString(), "pending");
    const auto proposal = proposed["proposal"].toObject();
    const auto applied = apply(proposal);
    QVERIFY2(!applied.contains("error"),
             QJsonDocument(applied).toJson().constData());
    QCOMPARE(applied["status"].toString(), "applied");
    QVERIFY(applied["proposal"].toObject()["receipt"].isObject());
    const auto undone = apply(proposal, true);
    QVERIFY2(!undone.contains("error"),
             QJsonDocument(undone).toJson().constData());
    QCOMPARE(undone["status"].toString(), "undone");
    const auto restored = propose(kind, after);
    QVERIFY2(!restored.contains("error"),
             QJsonDocument(restored).toJson().constData());
    QCOMPARE(restored["proposal"].toObject()["before"], proposal["before"]);
    QCOMPARE(apply(proposal, true)["error"].isString(), true);
  }
  void changedBeforeValueAndInterveningHistoryAreRejected() {
    const auto first = propose("comment", "first")["proposal"].toObject();
    const auto stale = propose("comment", "stale")["proposal"].toObject();
    QVERIFY(!first.isEmpty());
    QVERIFY(!stale.isEmpty());
    auto result = apply(first);
    QCOMPARE(result["status"].toString(), "applied");
    result = apply(stale);
    QCOMPARE(result["status"].toString(), "conflict");
    const auto later = propose("rename", "LaterRename")["proposal"].toObject();
    result = apply(later);
    QCOMPARE(result["status"].toString(), "applied");
    result = apply(first, true);
    QCOMPARE(result["status"].toString(), "applied");
    QVERIFY(result["error"].toString().contains("history changed"));
    result = apply(later, true);
    QCOMPARE(result["status"].toString(), "undone");
  }
  void changedBinaryIsRejectedBeforeCommand() {
    const auto proposed = propose("comment", "reviewed");
    QVERIFY(!proposed.contains("error"));
    QFile file(Owner.Objects[0].Path);
    QVERIFY(file.open(QIODevice::Append));
    file.write("changed");
    file.close();
    const auto result = apply(proposed["proposal"].toObject());
    QCOMPARE(result["status"].toString(), "failed");
    QVERIFY(result["error"].toString().contains("SHA-256"));
  }
};
QTEST_GUILESS_MAIN(ChangeReviewTests)
#include "ChangeReviewTests.moc"
