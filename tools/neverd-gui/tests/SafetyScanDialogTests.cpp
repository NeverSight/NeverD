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
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTest>
#include <QTreeWidget>
#include <algorithm>

class SafetyScanDialogTests : public QObject {
  Q_OBJECT
private slots:
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
    auto *box = dialog.findChild<QDialogButtonBox *>();
    auto *analyzers = dialog.findChild<QComboBox *>();
    QCOMPARE(analyzers->currentData().toString(), "both");
    QPushButton *run = nullptr, *cancel = nullptr;
    for (auto *button : box->buttons()) {
      if (button->text() == "&Scan")
        run = qobject_cast<QPushButton *>(button);
      if (button->text() == "Cancel scan")
        cancel = qobject_cast<QPushButton *>(button);
    }
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
};
QTEST_MAIN(SafetyScanDialogTests)
#include "SafetyScanDialogTests.moc"
