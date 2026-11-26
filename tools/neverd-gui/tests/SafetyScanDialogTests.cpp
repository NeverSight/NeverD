//===- SafetyScanDialogTests.cpp - Desktop memory-safety report ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise selection and cancellation through the dialog's real dispatcher.
//===----------------------------------------------------------------------===//

#include "SafetyScanDialog.h"

#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <algorithm>

class SafetyScanDialogTests : public QObject {
  Q_OBJECT
private slots:
  void queuedCancellationFinishesWithoutStartingTheScan() {
    int sends = 0;
    QueryService service(
        [&](const QString &, const QJsonObject &, const QString &) {
          return QString::number(++sends);
        });
    service.setAvailable(true);
    QObject owner;
    QJsonObject terminal;
    const auto id = service.enqueueCommand(
        "memory_safety_scan", {}, &owner,
        [&](const QJsonObject &reply) { terminal = reply; });
    service.cancelCommand(id);
    QTRY_COMPARE(terminal.value("status").toString(), "cancelled");
    QCOMPARE(sends, 0);
    QVERIFY(!service.hasCommands());
  }

  void cancelRetainsEvidenceAndAnalyzerCoverage() {
    QList<QJsonObject> sent;
    QueryService service(
        [&](const QString &op, const QJsonObject &payload, const QString &) {
          const auto id = QString::number(sent.size() + 1);
          sent.append(
              {{"operation", op}, {"payload", payload}, {"request_id", id}});
          return id;
        });
    service.setAvailable(true);
    neverd::gui::SafetyScanDialog dialog(service, nullptr);
    dialog.show();
    auto *analyzers = dialog.findChild<QComboBox *>();
    QCOMPARE(analyzers->currentData().toString(), "both");
    auto *run = dialog.findChild<QPushButton *>("runSafetyScan");
    auto *cancel = dialog.findChild<QPushButton *>("cancelSafetyScan");
    QVERIFY(run && cancel);
    run->click();
    QTRY_COMPARE(sent.size(), 1);
    QCOMPARE(sent.front()["operation"].toString(), "memory_safety_scan");
    QCOMPARE(sent.front()["payload"].toObject()["analyzer"].toString(), "both");
    cancel->click();
    QTRY_COMPARE(sent.size(), 2);
    QCOMPARE(sent.back()["operation"].toString(), "cancel");
    QCOMPARE(sent.back()["payload"].toObject()["request_id"],
             sent.front()["request_id"]);

    const QJsonObject finding{{"function", "read_local"},
                              {"detail", "checked local read"}};
    const QJsonObject group{
        {"verdict", "UNSAFE"},
        {"property", "uninitialized_read"},
        {"location", QJsonObject{{"call_va", "0x1020"}}},
        {"observations",
         QJsonArray{QJsonObject{{"analyzer", "audit"}, {"finding", finding}}}}};
    const QJsonObject coverage{
        {"audit", QJsonObject{{"state", "completed"}, {"scanned", 1}}},
        {"hunt", QJsonObject{{"state", "canceled"}}}};
    service.receive(
        {{"type", "response"},
         {"request_id", "1"},
         {"operation", "memory_safety_scan"},
         {"project_id", ""},
         {"revision", "0"},
         {"status", "ok"},
         {"payload", QJsonObject{{"verdict", "UNSAFE"},
                                 {"coverage_complete", false},
                                 {"analyzers", coverage},
                                 {"findings", QJsonArray{group}}}}});
    QTRY_VERIFY(run->isEnabled());
    auto *table = dialog.findChild<QTreeWidget *>();
    QCOMPARE(table->topLevelItemCount(), 1);
    QCOMPARE(table->topLevelItem(0)->text(1), "uninitialized_read");
    QCOMPARE(table->topLevelItem(0)->text(4), "audit");
    const auto details = dialog.findChild<QPlainTextEdit *>()->toPlainText();
    QCOMPARE(QJsonDocument::fromJson(details.toUtf8()).object(), group);
    const auto labels = dialog.findChildren<QLabel *>();
    QVERIFY(std::any_of(labels.begin(), labels.end(), [](auto *label) {
      return label->text().contains("audit: completed") &&
             label->text().contains("hunt: canceled");
    }));
    analyzers->setCurrentIndex(analyzers->findData("hunt"));
    run->click();
    QTRY_COMPARE(sent.size(), 3);
    QCOMPARE(sent.back()["payload"].toObject()["analyzer"].toString(), "hunt");
  }
  void realWorkbenchScanMatchesCli() {
#ifdef TEST_SAFETY_CLI
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString fixture = directory.filePath("safety_cases_elf_x64");
    QVERIFY(QFile::copy(TEST_SAFETY_FIXTURE, fixture));
    QProcess cli;
    cli.start(TEST_SAFETY_CLI, {"scan", fixture});
    QVERIFY(cli.waitForFinished());
    QCOMPARE(cli.exitStatus(), QProcess::NormalExit);
    QCOMPARE(cli.exitCode(), 2);
    const auto expected =
        QJsonDocument::fromJson(cli.readAllStandardOutput()).object();
    const auto groups = expected.value("findings").toArray();
    QVERIFY(!groups.isEmpty());

    EngineClient client;
    QJsonObject received;
    QueryService service(&client);
    connect(&client, &EngineClient::message, &service,
            [&](const QJsonObject &message) {
              if (message.value("type") == "hello")
                service.setAvailable(message.value("capabilities")
                                         .toArray()
                                         .contains("memory_safety_scan"));
              else if (message.value("operation") == "memory_safety_scan" &&
                       message.value("status") == "ok")
                received = message.value("payload").toObject();
            });
    client.start(TEST_SAFETY_WORKER);
    QTRY_VERIFY_WITH_TIMEOUT(service.available(), 15000);
    QJsonObject opened;
    service.enqueueCommand("open",
                           {{"path", fixture},
                            {"analysis", false},
                            { "warm",
                              false }},
                           this,
                           [&](const QJsonObject &reply) { opened = reply; });
    QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(), 15000);
    QCOMPARE(opened.value("status").toString(), "ok");

    neverd::gui::SafetyScanDialog dialog(service, nullptr);
    dialog.show();
    auto *run = dialog.findChild<QPushButton *>("runSafetyScan");
    QVERIFY(run);
    QTest::mouseClick(run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!received.isEmpty(), 15000);
    QCOMPARE(received, expected);
    auto *table = dialog.findChild<QTreeWidget *>();
    QTRY_COMPARE(table->topLevelItemCount(), groups.size());
    auto *details = dialog.findChild<QPlainTextEdit *>();
    for (qsizetype i = 0; i < groups.size(); ++i) {
      table->setCurrentItem(table->topLevelItem(static_cast<int>(i)));
      QCOMPARE(
          QJsonDocument::fromJson(details->toPlainText().toUtf8()).object(),
          groups[i].toObject());
    }
    const auto status = dialog.findChild<QLabel *>("safetyScanStatus")->text();
    QVERIFY(status.contains("audit: completed"));
    QVERIFY(status.contains("hunt: completed"));
#else
    QSKIP("Requires the native CLI and worker");
#endif
  }
};
QTEST_MAIN(SafetyScanDialogTests)
#include "SafetyScanDialogTests.moc"
