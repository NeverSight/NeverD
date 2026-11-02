//===- X87CapabilitiesTests.cpp - Capability presentation contracts
//--------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared-report joins, dispatcher failures, read-only inspection and real
/// worker integration. Optional captures use isolated test preferences.
//===----------------------------------------------------------------------===//

#include "EngineClient.h"
#include "QueryService.h"
#include "Theme.h"
#include "X87CapabilitiesDialog.h"
#include "X87CapabilitiesModel.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>

using namespace neverd::gui;

namespace {
QJsonObject dimensions(const QString &Consumer = "concrete") {
  return {{"source_mode", "x86-64"},       {"abi", "system-v"},
          {"state_path", "owned-context"}, {"consumer", Consumer},
          {"backend", "portable"},         {"destination", "arm64"}};
}

QJsonObject report() {
  QJsonArray Coverage;
  for (const auto *Consumer : {"concrete", "translation"}) {
    auto Cell = dimensions(Consumer);
    Cell.remove("abi");
    Cell.insert("coverage_class", "finite");
    const bool Concrete = Cell.value("consumer") == QLatin1String("concrete");
    Cell.insert("implemented", Concrete);
    Cell.insert("numerical", Concrete ? "exact" : "unsupported");
    Cell.insert("architectural_state", "conservative-unknown");
    Cell.insert("profile", "fixture-explicit-profile");
    Cell.insert("reason", Concrete ? "finite-only" : "translation-unqualified");
    Cell.insert("requirements",
                "Known incoming payload; valid control word; "
                "masked exceptions. <b>This is plain text.</b>");
    Cell.insert("test_sources", "owned/fixture.cpp");
    Coverage.append(Cell);
  }
  auto Source = dimensions();
  for (const auto *Key : {"consumer", "backend", "destination"})
    Source.remove(Key);
  Source.insert("available", true);
  Source.insert("default_path", false);
  Source.insert("long_double", "binary80; ABI-specific padding");
  Source.insert("call_contract", "External state requires authentication");
  return {{"schema_version", 1},
          {"scope", "static-inventory; no host probe, execution or proof"},
          {"matrix_contract", "Unlisted combinations are unsupported"},
          {"precision", "Exact-only by default"},
          {"evidence", "Source references do not qualify passing runs"},
          {"forms", QJsonArray{QJsonObject{{"id", "FADD_ST"},
                                           {"mnemonic", "FADD"},
                                           {"operands", "ST0, STi"},
                                           {"coverage_class", "finite"}},
                               QJsonObject{{"id", "FSUB_ST"},
                                           {"mnemonic", "FSUB"},
                                           {"operands", "ST0, STi"},
                                           {"coverage_class", "finite"}}}},
          {"coverage", Coverage},
          {"source_paths", QJsonArray{Source}}};
}

struct Harness {
  QList<QJsonObject> Sent;
  QueryService Queries;
  Harness()
      : Queries([this](const QString &Operation, const QJsonObject &Payload,
                       const QString &Revision) {
          const auto ID = QString::number(Sent.size() + 1);
          Sent.append({{"request_id", ID},
                       {"operation", Operation},
                       {"payload", Payload},
                       {"revision", Revision}});
          return ID;
        }) {
    Queries.setAvailable(true);
  }
  void reply(int Index, QJsonObject Payload = report(), QString Error = {}) {
    const auto Request = Sent.at(Index);
    Queries.receive(
        {{"type", "response"},
         {"protocol_major", 1},
         {"request_id", Request.value("request_id")},
         {"operation", Request.value("operation")},
         {"project_id", ""},
         {"revision", "0"},
         {"status", Error.isEmpty() ? "ok" : "error"},
         {"payload", Payload},
         {"error", QJsonObject{{"code", Error}, {"message", Error}}}});
  }
};
} // namespace

class X87CapabilitiesTests final : public QObject {
  Q_OBJECT
  QTemporaryDir Settings;
private slots:
  void initTestCase() {
    QVERIFY(Settings.isValid());
    QCoreApplication::setOrganizationName("NeverDTests");
    QCoreApplication::setApplicationName("X87CapabilitiesTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       Settings.path());
  }
  void numericalAndStateClaimsRemainSeparate() {
    X87CapabilitiesModel Model;
    QString Error;
    QVERIFY2(Model.setReport(report(), Error), qPrintable(Error));
    QCOMPARE(Model.report(), report());
    Model.setDimensions(dimensions());
    QCOMPARE(Model.rowCount(), 2);
    QCOMPARE(Model.data(Model.index(0, 3)).toString(), "exact");
    QCOMPARE(Model.data(Model.index(0, 4)).toString(), "conservative-unknown");
    QCOMPARE(Model.capability(0), Model.capability(1));
    QVERIFY(!Model.sourcePath().value("default_path").toBool());
    Model.setDimensions(dimensions("translation"));
    QVERIFY(!Model.capability(0).value("implemented").toBool());
    QCOMPARE(Model.capability(0).value("reason").toString(),
             "translation-unqualified");
    auto Unlisted = dimensions();
    Unlisted.insert("source_mode", "i386");
    Model.setDimensions(Unlisted);
    QVERIFY(Model.capability(0).isEmpty());
    QVERIFY(Model.sourcePath().isEmpty());
    QCOMPARE(Model.data(Model.index(0, 3)).toString(), "unsupported");
  }
  void malformedOrNewSchemasClearPreviousClaims() {
    X87CapabilitiesModel Model;
    QString Error;
    for (int Mutation = 0; Mutation != 4; ++Mutation) {
      QVERIFY(Model.setReport(report(), Error));
      auto Bad = report();
      if (Mutation == 0)
        Bad.insert("schema_version", 2);
      if (Mutation == 1)
        Bad.insert("forms", QJsonArray{});
      auto Cells = Bad.value("coverage").toArray();
      if (Mutation == 2)
        Cells.append(Cells.first());
      if (Mutation == 3) {
        auto Cell = Cells.first().toObject();
        Cell.insert("implemented", "yes");
        Cells.replace(0, Cell);
      }
      Bad.insert("coverage", Cells);
      QVERIFY(!Model.setReport(Bad, Error));
      QVERIFY(!Error.isEmpty());
      QCOMPARE(Model.rowCount(), 0);
      QVERIFY(Model.report().isEmpty());
    }
  }
  void inspectionUsesOnlyTheReadDispatcher() {
    Harness H;
    X87CapabilitiesDialog Dialog(H.Queries);
    auto *Model = Dialog.findChild<X87CapabilitiesModel *>();
    auto *Status = Dialog.findChild<QLabel *>("x87Status");
    Dialog.show();
    Dialog.setProviderAvailable(false, false);
    QVERIFY(Status->text().contains("disconnected"));
    Dialog.setProviderAvailable(true, false);
    QVERIFY(H.Sent.isEmpty());
    QVERIFY(Status->text().contains("does not provide"));
    Dialog.setProviderAvailable(true, true);
    QTRY_COMPARE(H.Sent.size(), 1);
    QCOMPARE(H.Sent.first().value("operation").toString(), "x87_capabilities");
    QVERIFY(H.Sent.first().value("payload").toObject().isEmpty());
    H.reply(0);
    QTRY_COMPARE(Model->rowCount(), 2);
    QCOMPARE(H.Queries.revision(), "0");
    QVERIFY(H.Queries.projectId().isEmpty());
    auto *Details = Dialog.findChild<QPlainTextEdit *>("x87Details");
    QVERIFY(Details->toPlainText().contains("<b>This is plain text.</b>"));
    auto *Consumer = Dialog.findChild<QComboBox *>("consumer");
    Consumer->setCurrentText("translation");
    QVERIFY(Details->toPlainText().contains("translation-unqualified"));
    auto *Search = Dialog.findChild<QLineEdit *>("x87Filter");
    Search->setFocus();
    QTest::keyClicks(Search, "nothing-matches");
    QCOMPARE(Dialog.findChild<QTableView *>("x87Forms")->model()->rowCount(),
             0);
    QVERIFY(Details->toPlainText().contains("Clear the filter"));
    Search->clear();
    QTest::keyClick(Search, Qt::Key_Tab);
    QVERIFY(Dialog.focusWidget() != Search);
    QCOMPARE(H.Sent.size(), 1);
    Dialog.setProviderAvailable(false, false);
    QCOMPARE(Model->rowCount(), 0);
    QVERIFY(Status->text().contains("disconnected"));
    QTest::keyClick(&Dialog, Qt::Key_Escape);
    QVERIFY(!Dialog.isVisible());
  }
  void errorsAndRetiredResponsesNeverBecomeSupport() {
    Harness H;
    X87CapabilitiesDialog Dialog(H.Queries);
    auto *Model = Dialog.findChild<X87CapabilitiesModel *>();
    auto *Status = Dialog.findChild<QLabel *>("x87Status");
    Dialog.setProviderAvailable(true, true);
    QTRY_COMPARE(H.Sent.size(), 1);
    H.reply(0, {}, "unsupported");
    QTRY_VERIFY(Status->text().contains("unsupported"));
    QCOMPARE(Model->rowCount(), 0);
    Dialog.findChild<QPushButton *>("x87Reload")->click();
    QTRY_COMPARE(H.Sent.size(), 2);
    Dialog.setProviderAvailable(false, false);
    H.reply(1);
    QTest::qWait(1);
    QCOMPARE(Model->rowCount(), 0);
    QVERIFY(Status->text().contains("disconnected"));
  }
  void realWorkerReportAndCaptures() {
#ifdef TEST_REAL_WORKER
    EngineClient Client;
    QueryService Queries(&Client);
    X87CapabilitiesDialog Dialog(Queries);
    QSignalSpy Sent(&Client, &EngineClient::sendRequested);
    connect(&Client, &EngineClient::message, &Dialog,
            [&](const QJsonObject &Message) {
              if (Message.value("type") == QLatin1String("hello")) {
                Queries.setAvailable(true);
                Dialog.setProviderAvailable(true,
                                            Message.value("capabilities")
                                                .toArray()
                                                .contains("x87_capabilities"));
              }
            });
    Client.start(TEST_REAL_WORKER);
    Dialog.show();
    auto *Model = Dialog.findChild<X87CapabilitiesModel *>();
    QTRY_VERIFY_WITH_TIMEOUT(Model->rowCount() > 0, 15000);
    QCOMPARE(Sent.size(), 1);
    QCOMPARE(Sent.first().first().toJsonObject().value("operation").toString(),
             "x87_capabilities");
    QCOMPARE(Queries.revision(), "0");
    const auto RawReport = Model->report();
    X87CapabilitiesModel Projection;
    QString Error;
    QVERIFY2(Projection.setReport(RawReport, Error), qPrintable(Error));
    const auto RawCoverage = RawReport.value("coverage").toArray();
    for (const auto *Consumer : {"llvm", "high-c", "llvm-c", "translation"}) {
      const auto Selected = dimensions(Consumer);
      Projection.setDimensions(Selected);
      bool SawAdmitted = false, SawRefused = false;
      for (int Row = 0; Row != Projection.rowCount(); ++Row) {
        const auto Form = Projection.form(Row);
        QJsonObject Expected;
        for (const auto &Value : RawCoverage) {
          const auto Cell = Value.toObject();
          bool Matches =
              Cell.value("coverage_class") == Form.value("coverage_class");
          for (auto It = Selected.begin(); It != Selected.end(); ++It)
            if (It.key() != "abi")
              Matches &= Cell.value(It.key()) == It.value();
          if (Matches) {
            QVERIFY(Expected.isEmpty());
            Expected = Cell;
          }
        }
        QVERIFY(!Expected.isEmpty());
        QCOMPARE(Projection.capability(Row), Expected);
        SawAdmitted |= Expected.value("implemented").toBool();
        SawRefused |= !Expected.value("implemented").toBool();
      }
      QVERIFY(SawAdmitted);
      QVERIFY(SawRefused);
    }
    const auto Selected = dimensions("translation");
    for (auto It = Selected.begin(); It != Selected.end(); ++It) {
      auto *Selector = Dialog.findChild<QComboBox *>(It.key());
      QVERIFY(Selector);
      QVERIFY(Selector->findText(It.value().toString()) >= 0);
      Selector->setCurrentText(It.value().toString());
    }
    Dialog.findChild<QLineEdit *>("x87Filter")->setText("m80");
    auto *Details = Dialog.findChild<QPlainTextEdit *>("x87Details");
    QTRY_VERIFY(Details->toPlainText().contains(
        "explicit-portable-x87-translation-v1"));
    auto *Table = Dialog.findChild<QTableView *>("x87Forms");
    QVERIFY(Table->model()->rowCount() > 0);
    QCOMPARE(Table->model()->index(0, 4).data().toString(),
             "conservative-unknown");
    const auto Capture = qEnvironmentVariable("NEVERD_X87_GUI_CAPTURES");
    if (!Capture.isEmpty()) {
      QVERIFY(QDir().mkpath(Capture));
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        Theme::instance().apply();
        for (const auto Size : {QSize(1100, 850), QSize(720, 620)}) {
          Dialog.resize(Size);
          QTest::qWait(20);
          const auto Name =
              QString("x87-%1-%2.png")
                  .arg(Mode == Theme::Mode::Light ? "light" : "dark")
                  .arg(Size.width());
          QVERIFY(Dialog.grab().save(QDir(Capture).filePath(Name)));
        }
      }
      Dialog.setLayoutDirection(Qt::RightToLeft);
      QTest::qWait(20);
      QVERIFY(Dialog.grab().save(QDir(Capture).filePath("x87-rtl.png")));
      Dialog.setProviderAvailable(true, false);
      QTest::qWait(20);
      QVERIFY(
          Dialog.grab().save(QDir(Capture).filePath("x87-unavailable.png")));
    }
    Client.stop();
#else
    QSKIP("Real worker was not enabled in this build");
#endif
  }
};

QTEST_MAIN(X87CapabilitiesTests)
#include "X87CapabilitiesTests.moc"
