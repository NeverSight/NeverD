//===- ComparisonTests.cpp - Real comparison GUI contracts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Real worker inputs, bounded pages, mapping edits and responsive Qt widgets.
/// Authored machine code is inspected but never executed.
//===----------------------------------------------------------------------===//

#include "../ResourceLimits.h"
#include "ComparisonChangesDialog.h"
#include "ComparisonCounterpartDialog.h"
#include "ComparisonDialog.h"
#include "ComparisonFlowDialog.h"
#include "ComparisonGraph.h"
#include "ComparisonImpactDialog.h"
#include "ComparisonModel.h"
#include "ComparisonRegionsDialog.h"
#include "ComparisonReviewDialog.h"
#include "ComparisonSourceView.h"
#include "ComparisonValidationDialog.h"
#include "ProjectDatabase.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <algorithm>

using namespace neverd::gui;
namespace {
constexpr int Timeout = 15000;
bool write(const QString &Path, const QByteArray &Bytes) {
  QFile F(Path);
  return F.open(QIODevice::WriteOnly) && F.write(Bytes) == Bytes.size();
}
void layoutDirections() {
  QTest::addColumn<bool>("RightToLeft");
  QTest::newRow("left-to-right") << false;
  QTest::newRow("right-to-left") << true;
}
} // namespace

class ComparisonTests final : public QObject {
  Q_OBJECT
  QTemporaryDir Directory;
private slots:
  void initTestCase() {
    QCoreApplication::setOrganizationName("NeverDComparisonTests");
    QCoreApplication::setApplicationName("ComparisonTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       Directory.path());
    Theme::instance().apply();
  }
  void aarch64RawInputsReachSourceAndOriginalInstructions() {
    const auto A = Directory.filePath("aarch64-before.bin");
    const auto B = Directory.filePath("aarch64-after.bin");
    QVERIFY(write(A, QByteArray::fromHex("e00080d2000c0091c0035fd6")));
    QVERIFY(write(B, QByteArray::fromHex("200180d2000c0091c0035fd6")));
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    View.findChild<QLineEdit *>("comparisonPrimary")->setText(A);
    View.findChild<QLineEdit *>("comparisonSecondary")->setText(B);
    for (const auto *Name : {"primaryLoader", "secondaryLoader"}) {
      auto *Loader = View.findChild<QComboBox *>(Name);
      QVERIFY(Loader);
      const int Index = Loader->findData("aarch64");
      QVERIFY(Index >= 0);
      Loader->setCurrentIndex(Index);
    }
    View.findChild<QLineEdit *>("primaryBase")->setText("0x1000");
    View.findChild<QLineEdit *>("secondaryBase")->setText("0x8000");
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonStart"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    QCOMPARE(Failures.size(), 0);
    QCOMPARE(Session.summary()["primary"].toObject()["architecture"].toString(),
             QString("aarch64"));
    auto *Inventory = View.findChild<QTableView *>("comparisonFunctions");
    QTRY_VERIFY_WITH_TIMEOUT(Inventory->model()->rowCount() > 0, Timeout);
    Inventory->setCurrentIndex(Inventory->model()->index(0, 0));
    auto *Instructions = View.findChild<QTableView *>("comparisonInstructions");
    QTRY_VERIFY_WITH_TIMEOUT(Instructions->model()->rowCount() == 3, Timeout);
    auto *Source = View.findChild<ComparisonSourceView *>("comparisonSource");
    auto *Tabs = View.findChild<QTabWidget *>();
    QVERIFY(Source && Tabs);
    Tabs->setCurrentWidget(Source);
    auto *Primary = View.findChild<QPlainTextEdit *>("comparisonPrimarySource");
    auto *Secondary =
        View.findChild<QPlainTextEdit *>("comparisonSecondarySource");
    QTRY_VERIFY_WITH_TIMEOUT(Primary->toPlainText().contains("return"),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Secondary->toPlainText().contains("return"),
                             Timeout);
    QVERIFY(Primary->toPlainText() != Secondary->toPlainText());
    QVERIFY(!Primary->extraSelections().isEmpty());
    QCOMPARE(Primary->blockCount(), Secondary->blockCount());
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        View.resize(1280, 800);
        QTest::qWait(40);
        QVERIFY(
            View.grab().save(Captures + (Mode == Theme::Mode::Dark
                                             ? "/aarch64-dark-source.png"
                                             : "/aarch64-light-source.png")));
      }
    }
    auto *Navigate =
        View.findChild<QPushButton *>("comparisonPrimarySourceInstruction");
    QVERIFY(Navigate);
    for (auto Block = Primary->document()->begin(); Block.isValid();
         Block = Block.next()) {
      Primary->setTextCursor(QTextCursor(Block));
      if (Navigate->isEnabled())
        break;
    }
    QVERIFY(Navigate->isEnabled());
    QTest::mouseClick(Navigate, Qt::LeftButton);
    QVERIFY(Tabs->currentWidget() != Source);
    QCOMPARE(Failures.size(), 0);
  }
  void unresolvedReferencesStayDistinctFromObservedChanges() {
    const auto A = Directory.filePath("reference-before.bin");
    const auto B = Directory.filePath("reference-after.bin");
    QByteArray Bytes(24, '\0');
    Bytes.replace(0, 7, QByteArray::fromHex("8b050a000000c3"));
    Bytes[16] = 17;
    QVERIFY(write(A, Bytes));
    Bytes[23] = 29;
    QVERIFY(write(B, Bytes));
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    View.findChild<QLineEdit *>("comparisonPrimary")->setText(A);
    View.findChild<QLineEdit *>("comparisonSecondary")->setText(B);
    for (const auto *Name : {"primaryLoader", "secondaryLoader"})
      View.findChild<QComboBox *>(Name)->setCurrentIndex(1);
    for (const auto *Name : {"primaryBase", "secondaryBase"})
      View.findChild<QLineEdit *>(Name)->setText("0x1000");
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonStart"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    QCOMPARE(Failures.size(), 0);
    auto *Inventory = View.findChild<QTableView *>("comparisonFunctions");
    auto *Model = qobject_cast<ComparisonModel *>(Inventory->model());
    auto *Filter = View.findChild<QComboBox *>("comparisonFilter");
    const auto Uncertain = Filter->findData("uncertain");
    QVERIFY(Uncertain >= 0);
    Filter->setCurrentIndex(Uncertain);
    QTRY_COMPARE_WITH_TIMEOUT(Model->rowCount(), 1, Timeout);
    QCOMPARE(Model->data(Model->index(0, 5), Qt::DisplayRole).toString(),
             QString("Uncertain"));
    Inventory->setCurrentIndex(Model->index(0, 0));
    auto *Evidence = View.findChild<QPlainTextEdit *>("comparisonEvidence");
    QTRY_VERIFY_WITH_TIMEOUT(
        Evidence->toPlainText().contains("Reference identities: unresolved"),
        Timeout);
    QVERIFY(Evidence->toPlainText().contains("decoded bodies complete"));
    auto *Instructions = View.findChild<QTableView *>("comparisonInstructions");
    QTRY_VERIFY_WITH_TIMEOUT(Instructions->model()->rowCount() > 0, Timeout);
    QCOMPARE(Instructions->model()
                 ->data(Instructions->model()->index(0, 3))
                 .toString(),
             QString("Uncertain"));
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      const auto PreviousMode = Theme::instance().mode();
      for (auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        View.resize(1240, 820);
        QTest::qWait(50);
        QVERIFY(View.grab().save(Captures + (Mode == Theme::Mode::Dark
                                                 ? "/uncertain-dark.png"
                                                 : "/uncertain-light.png")));
      }
      Theme::instance().setMode(PreviousMode);
    }
    Filter->setCurrentIndex(Filter->findData("changed"));
    QTRY_COMPARE_WITH_TIMEOUT(Model->rowCount(), 0, Timeout);
    const auto Saved = Directory.filePath("uncertain.nddiff");
    Session.command("comparison_save", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    Session.command("comparison_restore", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    Filter->setCurrentIndex(Uncertain);
    QTRY_COMPARE_WITH_TIMEOUT(Model->rowCount(), 1, Timeout);
    QCOMPARE(Model->data(Model->index(0, 5), Qt::DisplayRole).toString(),
             QString("Uncertain"));
    QCOMPARE(Failures.size(), 0);
  }
  void retainedRealCorpusReview() {
    const auto ReportPath = qEnvironmentVariable("NEVERD_DIFF_REAL_REPORT");
    const auto ObservationsPath =
        qEnvironmentVariable("NEVERD_DIFF_REAL_OBSERVATIONS");
    if (ReportPath.isEmpty() && ObservationsPath.isEmpty())
      QSKIP("Set NEVERD_DIFF_REAL_REPORT and NEVERD_DIFF_REAL_OBSERVATIONS "
            "to a retained comparison and its complete CLI change query.");
    QFile ReportFile(ReportPath), ObservationsFile(ObservationsPath);
    QVERIFY(ReportFile.open(QIODevice::ReadOnly));
    QVERIFY(ObservationsFile.open(QIODevice::ReadOnly));
    const auto Report = QJsonDocument::fromJson(ReportFile.readAll()).object();
    const auto Expected =
        QJsonDocument::fromJson(ObservationsFile.readAll()).object();
    QCOMPARE(Report["state"].toString(), QString("complete"));
    QCOMPARE(Expected["schema"].toString(),
             QString("neverd.bindiff.observations"));
    QCOMPARE(Expected["offset"].toInt(-1), 0);
    QVERIFY(Expected["next_offset"].isNull());
    const auto ExpectedRows = Expected["rows"].toArray();
    QVERIFY(!ExpectedRows.isEmpty());
    const auto Address = Expected["primary"].toObject()["address"].toString();
    QVERIFY(!Address.isEmpty());
    for (const auto *Side : {"primary", "secondary"})
      QCOMPARE(Expected[QString(Side) + "_sha256"],
               Report[Side].toObject()["sha256"]);

    ComparisonDialog View(TEST_COMPARISON_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.resize(1280, 800);
    View.show();
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.start({{"path", Report["primary"].toObject()["path"]}},
                  {{"path", Report["secondary"].toObject()["path"]}}, {},
                  ReportPath);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    QCOMPARE(Failures.size(), 0);
    QCOMPARE(Session.summary()["coverage"], Report["coverage"]);
    const auto Revision = Session.revision();
    View.findChild<QLineEdit *>("comparisonSearch")->setText(Address);
    auto *Functions = View.findChild<QTableView *>("comparisonFunctions");
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 1, Timeout);
    Functions->setCurrentIndex(Functions->model()->index(0, 0));
    auto *Open = View.findChild<QPushButton *>("comparisonChangesReview");
    QTRY_VERIFY_WITH_TIMEOUT(Open->isEnabled(), Timeout);
    Open->click();
    auto *Changes = View.findChild<ComparisonChangesDialog *>();
    QVERIFY(Changes);
    auto *Table = Changes->findChild<QTableView *>("changesTable");
    auto *Rows = qobject_cast<ComparisonModel *>(Table->model());
    QSignalSpy QueryFailures(Rows, &ComparisonModel::failed);
    QJsonObject Page;
    connect(Rows, &ComparisonModel::pageReady, this,
            [&](const QJsonObject &Value) { Page = Value; });
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), ExpectedRows.size(), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Page.isEmpty(), Timeout);
    for (const auto *Field :
         {"primary_sha256", "secondary_sha256", "pairing",
          "native_analysis_complete", "reference_comparison_complete",
          "primary_gaps", "secondary_gaps", "semantic_validation",
          "security_inference", "fix_presence"})
      QCOMPARE(Page[Field], Expected[Field]);
    for (int I = 0; I != ExpectedRows.size(); ++I) {
      QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(I).isEmpty(), Timeout);
      QCOMPARE(Rows->row(I), ExpectedRows[I].toObject());
    }
    QCOMPARE(QueryFailures.size(), 0);
    const auto PreviousMode = Theme::instance().mode();
    const auto RestoreTheme =
        qScopeGuard([&] { Theme::instance().setMode(PreviousMode); });
    if (const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
        !Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(60);
        QVERIFY(Changes->grab().save(Captures + "/real-corpus-changes-" +
                                     Theme::modeName(Mode) + ".png"));
      }
    }
    Changes->close();
    const auto Saved = Directory.filePath("real-corpus.nddiff");
    Session.command("comparison_save", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QFile SavedFile(Saved);
    QVERIFY(SavedFile.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(SavedFile.readAll()).object(), Report);
    Session.command("comparison_restore", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    // Restore preserves the document but advances the live query generation.
    QVERIFY(Session.revision() != Revision);
    QCOMPARE(Session.summary()["coverage"], Report["coverage"]);
    QCOMPARE(Failures.size(), 0);
  }
  void optimizedSSASelectionShowsPluralNativeOrigins() {
    const auto A = Directory.filePath("optimized-ssa-before.bin");
    const auto B = Directory.filePath("optimized-ssa-after.bin");
    // Owned AArch64 compiler_regions_fixture.c at O0/O2; analysis only.
    QVERIFY(
        write(A, QByteArray::fromHex(
                     "ff4300d1e00f00b9e10b00b9e80f40b9e90b40b90801090be90280520"
                     "801094ae80700b9e80740b9e9008052087d091b08250011e80700b9e9"
                     "0740b9e80740b9081105530815492ae80700b9e80740b9e90b40b9aa0"
                     "18052297d0a1b294500110801094a000d0011ff430091c0035fd6")));
    QVERIFY(write(B, QByteArray::fromHex(
                         "e80280522900000baa0180522801084a2b0280522a2c0a1b09711"
                         "d532801084b082500114815c84a000d0011c0035fd6")));
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    View.findChild<QLineEdit *>("comparisonPrimary")->setText(A);
    View.findChild<QLineEdit *>("comparisonSecondary")->setText(B);
    for (const auto *Name : {"primaryLoader", "secondaryLoader"}) {
      auto *Loader = View.findChild<QComboBox *>(Name);
      Loader->setCurrentIndex(Loader->findData("aarch64"));
    }
    View.findChild<QLineEdit *>("primaryBase")->setText("0x1000");
    View.findChild<QLineEdit *>("secondaryBase")->setText("0x8000");
    auto *Profile = View.findChild<QComboBox *>("comparisonFlowProfile");
    QVERIFY(Profile && !Profile->isEnabled());
    View.findChild<QCheckBox *>("comparisonAnalyzeFlowRegions")
        ->setChecked(true);
    QVERIFY(Profile->isEnabled());
    Profile->setCurrentIndex(Profile->findData("llvm-ssa-v1"));
    QTest::mouseClick(View.findChild<QToolButton *>("comparisonSettings"),
                      Qt::LeftButton);
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonStart"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    QCOMPARE(Failures.size(), 0);
    QCOMPARE(Session.summary()["settings"]
                 .toObject()["dataflow"]
                 .toObject()["profile"]
                 .toString(),
             QString("llvm-ssa-v1"));
    QVERIFY(
        Session.summary()["flow_regions"].toObject()["anchor_count"].toInt() >
        0);
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonFlowReview"),
                      Qt::LeftButton);
    auto *Dialog =
        View.findChild<ComparisonFlowDialog *>("comparisonFlowDialog");
    QVERIFY(Dialog);
    auto *Table = Dialog->findChild<QTableView *>("flowNodes");
    auto *Rows = qobject_cast<ComparisonModel *>(Table->model());
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() > 0 && !Rows->row(0).isEmpty(),
                             Timeout);
    int Plural = -1;
    for (int N = 0; N < Rows->rowCount(); ++N) {
      QStringList Addresses;
      for (const auto &O : Rows->row(N)["origins"].toArray())
        Addresses << O.toObject()["address"].toString();
      Addresses.removeDuplicates();
      if (Addresses.size() > 1) {
        Plural = N;
        const auto Display =
            Rows->data(Rows->index(N, 1), Qt::DisplayRole).toString();
        for (const auto &Address : Addresses)
          QVERIFY(Display.contains(Address));
        break;
      }
    }
    QVERIFY(Plural >= 0);
    QVERIFY(!Rows->data(Rows->index(Plural, 4), Qt::DisplayRole)
                 .toString()
                 .isEmpty());
    QVERIFY(Rows->data(Rows->index(Plural, 1), Qt::ToolTipRole)
                .toString()
                .contains("Original addresses:"));
    Table->selectRow(Plural);
    QTest::mouseClick(Dialog->findChild<QPushButton *>("flowBackward"),
                      Qt::LeftButton);
    auto *Slice = Dialog->findChild<QTableView *>("flowSlice");
    QTRY_VERIFY_WITH_TIMEOUT(Slice->model()->rowCount() > 0, Timeout);
    QVERIFY(Dialog->findChild<QLabel *>("flowSummary")
                ->text()
                .contains("Optimized SSA"));
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        View.resize(1280, 800);
        Dialog->resize(1280, 800);
        QTest::qWait(40);
        const auto Suffix = Mode == Theme::Mode::Dark ? "dark" : "light";
        QVERIFY(View.grab().save(Captures + "/optimized-settings-" + Suffix +
                                 ".png"));
        QVERIFY(Dialog->grab().save(Captures + "/optimized-origins-" + Suffix +
                                    ".png"));
      }
    }
    QCOMPARE(Failures.size(), 0);
  }
  void parameterViewsExplainNativeInputsAndRestoreSelection() {
    const auto A = Directory.filePath("parameter-before.bin");
    const auto B = Directory.filePath("parameter-after.bin");
    // Owned control_regions_fixture.c, x64 ELF O0/O2; never executed.
    QVERIFY(write(
        A,
        QByteArray::fromHex(
            "554889e5897dfc8975f88b45fc3b45f873118b45fc83f0176bc00783c009"
            "8945fceb0f8b45fc83c0116b4df80d31c88945fc8b45fc3345f883c0035dc3")));
    QVERIFY(write(
        B, QByteArray::fromHex(
               "39f7731183f7178d04fd0000000029f883c009eb0b83c7118d04768d0486"
               "31f831f083c003c3")));
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    View.findChild<QLineEdit *>("comparisonPrimary")->setText(A);
    View.findChild<QLineEdit *>("comparisonSecondary")->setText(B);
    for (const auto *Name : {"primaryLoader", "secondaryLoader"})
      View.findChild<QComboBox *>(Name)->setCurrentIndex(1);
    View.findChild<QLineEdit *>("primaryBase")->setText("0x1000");
    View.findChild<QLineEdit *>("secondaryBase")->setText("0x8000");
    View.findChild<QCheckBox *>("comparisonAnalyzeFlowRegions")
        ->setChecked(true);
    auto *Profile = View.findChild<QComboBox *>("comparisonFlowProfile");
    const int ParameterProfile = Profile->findData("llvm-parameter-ssa-v1");
    QVERIFY(ParameterProfile >= 0);
    Profile->setCurrentIndex(ParameterProfile);
    QTest::mouseClick(View.findChild<QToolButton *>("comparisonSettings"),
                      Qt::LeftButton);
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonStart"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    QCOMPARE(Failures.size(), 0);
    QVERIFY(
        Session.summary()["flow_regions"].toObject()["anchor_count"].toInt() >
        0);
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonFlowReview"),
                      Qt::LeftButton);
    auto *Dialog =
        View.findChild<ComparisonFlowDialog *>("comparisonFlowDialog");
    QVERIFY(Dialog);
    auto *Side = Dialog->findChild<QComboBox *>("flowSide");
    Side->setCurrentIndex(Side->findData("secondary"));
    auto *Table = Dialog->findChild<QTableView *>("flowNodes");
    auto *Rows = qobject_cast<ComparisonModel *>(Table->model());
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() && !Rows->row(0).isEmpty(),
                             Timeout);
    int Projected = -1;
    for (int N = 0; N < Rows->rowCount(); ++N)
      if (Rows->data(Rows->index(N, 4), Qt::DisplayRole)
              .toString()
              .contains("low 32 of 64 bits")) {
        Projected = N;
        break;
      }
    QVERIFY(Projected >= 0);
    QVERIFY(Rows->data(Rows->index(Projected, 4), Qt::ToolTipRole)
                .toString()
                .contains("original width and native site"));
    QVERIFY(Dialog->findChild<QLabel *>("flowSummary")
                ->text()
                .contains("parameter views"));
    Table->selectRow(Projected);
    QTest::mouseClick(Dialog->findChild<QPushButton *>("flowBackward"),
                      Qt::LeftButton);
    auto *Slice = Dialog->findChild<QTableView *>("flowSlice");
    QTRY_VERIFY_WITH_TIMEOUT(Slice->model()->rowCount(), Timeout);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        Dialog->resize(1280, 800);
        QTest::qWait(40);
        QVERIFY(Dialog->grab().save(Captures +
                                    (Mode == Theme::Mode::Dark
                                         ? "/parameter-views-dark.png"
                                         : "/parameter-views-light.png")));
      }
    }
    const auto Saved = Directory.filePath("parameters.nddiff");
    Session.command("comparison_save", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && QFile::exists(Saved), Timeout);
    Dialog->hide();
    Profile->setCurrentIndex(Profile->findData("med-ssa-v2"));
    bool Selected = false;
    QTimer Select;
    connect(&Select, &QTimer::timeout, this, [&] {
      if (auto *File =
              qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
        File->selectFile(Saved);
        if (auto *Name = File->findChild<QLineEdit *>("fileNameEdit"))
          Name->setText(Saved);
        Selected = true;
        static_cast<QDialog *>(File)->accept();
      }
    });
    Select.start(10);
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonReopen"),
                      Qt::LeftButton);
    Select.stop();
    QVERIFY(Selected);
    QTRY_COMPARE_WITH_TIMEOUT(Profile->currentData().toString(),
                              QString("llvm-parameter-ssa-v1"), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Failures.size(), 0);
  }
  void loopProgressExplainsProofAndRestoresSelection_data() {
    QTest::addColumn<int>("Version");
    QTest::addColumn<int>("Family");
    QTest::newRow("self-loop-v1") << 1 << 2;
    QTest::newRow("multi-block-v2") << 2 << 3;
    QTest::newRow("nested-v3") << 3 << 4;
    QTest::newRow("selected-updates-v4") << 4 << 5;
    QTest::newRow("common-update-v4") << 4 << 6;
    QTest::newRow("nowrap-proof-v5") << 5 << 6;
    QTest::newRow("modular-proof-v5") << 5 << 7;
    QTest::newRow("parameter-bound-proof-v6") << 6 << 8;
    QTest::newRow("invariant-bound-proof-v7") << 7 << 9;
  }
  void loopProgressExplainsProofAndRestoresSelection() {
    QFETCH(int, Version);
    QFETCH(int, Family);
    const auto RequestedProfile = QString("llvm-progress-ssa-v%1").arg(Version);
    const auto A = Directory.filePath("progress-before.bin");
    const auto B = Directory.filePath("progress-after.bin");
    // Owned control_regions_fixture.c, family 2, x64 ELF O0/O2; never executed.
    QVERIFY(write(A, QByteArray::fromHex("554889e5897dfc8975f88b45f883e0078945f"
                                         "48b45f489c183c1ff894df483f80074228b"
                                         "45fc83f0176bc00783c0098945fc8b45fcc1e"
                                         "8058b4dfcc1e11b09c88945fcebce8b45fc"
                                         "3345f883c0035dc3")));
    QVERIFY(write(B, QByteArray::fromHex("89f889f183e1070f84ae00000083f0178d14c"
                                         "50000000029c283c20989d0c1c01b83f901"
                                         "0f849100000083f0178d14c50000000029c28"
                                         "3c20989d0c1c01b83f902747883f0178d14"
                                         "c50000000029c283c20989d0c1c01b83f9037"
                                         "45f83f0178d14c50000000029c283c20989"
                                         "d0c1c01b83f904744683f0178d14c50000000"
                                         "029c283c20989d0c1c01b83f905742d83f0"
                                         "178d14c50000000029c283c20989d0c1c01b8"
                                         "3f906741483f0178d0cc50000000029c183"
                                         "c109c1c11b89c831f083c003c3")));
    if (Version == 2) {
      // Owned family3 O0 has a branched loop body; both sides retain it.
      const auto Bytes = QByteArray::fromHex(
          "554889e5897dfc8975f88b45f883e0078945f48b45f489c183c1ff894df483f80074"
          "6e8b45fc83e00183f80074528b45fc83f0176bc00783c0098945fc8b45fcc1e8058b"
          "4dfcc1e11b09c88945fc8b45fc05674523013345f88945fc8b45fc8b4dfcc1e90b31"
          "c86bc00d8945fc8b45fcc1e0078b4dfcc1e91909c88945fceb0f8b45fc83c0116b4d"
          "f80d31c88945fceb828b45fc3345f883c0035dc3");
      QVERIFY(write(A, Bytes));
      QVERIFY(write(B, Bytes));
    }
    if (Version == 3) {
      // Owned family4 O0 retains both nested counters; never execute it.
      const auto Bytes = QByteArray::fromHex(
          "554889e5897dfc8975f88b45f883e0078945f48b45f489c183c1ff894df483f80074"
          "5b"
          "8b45fc83e00783c0018945f08b45f089c183c1ff894df083f800742e8b45fc83f017"
          "6b"
          "c00783c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fc8b45fc0345f43345f8"
          "89"
          "45fcebc28b45fcc1e0070345f43345fc8945fceb958b45fc3345f883c0035dc3");
      QVERIFY(write(A, Bytes));
      QVERIFY(write(B, Bytes));
    }
    if (Family == 5) {
      // Owned family5 x64 bytes, compiled at O0; never executed.
      const auto Bytes =
          QByteArray::fromHex("554889e5897dfc8975f88b45f883e00783c0018945f48b45"
                              "fc83e00183f80074328b45"
                              "fc83f0176bc00783c0098945fc8b45fcc1e8058b4dfcc1e1"
                              "1b09c88945fc8b45f483c0"
                              "ff8945f483f8007402ebc5eb308b45fc83c0116b4df80d31"
                              "c88945fc8b45fcc1e00703"
                              "45f83345fc8945fc8b45f483c0ff8945f483f8007402eb95"
                              "eb008b45fc3345f883c003"
                              "5dc3");
      QVERIFY(write(A, Bytes));
      QVERIFY(write(B, Bytes));
    }
    if (Family == 6) {
      // Owned family6 x64 bytes, compiled at O2; never executed.
      const auto Bytes =
          QByteArray::fromHex("89f183e1078d04768d1486eb1e0f1f0083f7178d04fd0000"
                              "000029f883c009c1c01b83"
                              "f9027c1dffc989c740f6c70175df83c71131d789f8c1e007"
                              "01f031f883f9017fe331f0"
                              "83c003c3");
      QVERIFY(write(A, Bytes));
      QVERIFY(write(B, Bytes));
    }
    if (Family == 7) {
      // Owned modular_progress_fixture.c, x64 O0; never executed.
      const auto Bytes = QByteArray::fromHex(
          "554889e5897dfc8975f88b45f88945f48b45fc83e00183f80074228b45fc83f0176b"
          "c00783c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fceb1e8b45fc83c0116b"
          "4df40d31c88945fc8b45fcc1e0070345f83345fc8945fc8b45f483c0038945f4837d"
          "f40173a68b45fc3345f883c0035dc3");
      QVERIFY(write(A, Bytes));
      QVERIFY(write(B, Bytes));
    }
    if (Family == 8) {
      // Owned parameter_bound_progress_fixture.c, x64 O0; never executed.
      const auto Bytes =
          QByteArray::fromHex("554889e5897dfc8975f88b45fc8945f48b45fc83e00183f8"
                              "0074228b45fc83f0176bc007"
                              "83c0098945fc8b45fcc1e8058b4dfcc1e11b09c88945fceb"
                              "1e8b45fc83c0116b4df40d"
                              "31c88945fc8b45fcc1e0070345f83345fc8945fc8b45f483"
                              "c0038945f48b45f48b4df8"
                              "83c10039c8759f8b45fc3345f883c0035dc3");
      QVERIFY(write(A, Bytes));
      QVERIFY(write(B, Bytes));
    }
    if (Family == 9) {
      // Owned invariant_bound_progress_fixture.c, x64 O0; never executed.
      const auto Bytes = QByteArray::fromHex(
          "554889e5897dfc8975f88b45f88b4df8c1e90331c88b4dfc81e1ff00000001c88945"
          "f48b45fc8945f08b45fc83e00183f80074228b45fc83f0176bc00783c0098945fc8b"
          "45fcc1e8058b4dfcc1e11b09c88945fceb1e8b45fc83c0116b4df00d31c88945fc8b"
          "45fcc1e0070345f83345fc8945fc8b45f083c0038945f08b45f08b4df483c10039c8"
          "759f8b45fc3345f883c0035dc3");
      QVERIFY(write(A, Bytes));
      QVERIFY(write(B, Bytes));
    }
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    View.findChild<QLineEdit *>("comparisonPrimary")->setText(A);
    View.findChild<QLineEdit *>("comparisonSecondary")->setText(B);
    for (const auto *Name : {"primaryLoader", "secondaryLoader"})
      View.findChild<QComboBox *>(Name)->setCurrentIndex(1);
    View.findChild<QLineEdit *>("primaryBase")->setText("0x1000");
    View.findChild<QLineEdit *>("secondaryBase")->setText("0x8000");
    View.findChild<QCheckBox *>("comparisonFlowFragments")->setChecked(true);
    auto *Profile = View.findChild<QComboBox *>("comparisonFlowProfile");
    const int ProgressProfile = Profile->findData(RequestedProfile);
    QVERIFY(ProgressProfile >= 0);
    Profile->setCurrentIndex(ProgressProfile);
    QTest::mouseClick(View.findChild<QToolButton *>("comparisonSettings"),
                      Qt::LeftButton);
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonStart"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    QCOMPARE(Failures.size(), 0);
    QVERIFY(
        Session.summary()["flow_regions"].toObject()["anchor_count"].toInt() >
        0);
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonFlowReview"),
                      Qt::LeftButton);
    auto *Dialog =
        View.findChild<ComparisonFlowDialog *>("comparisonFlowDialog");
    QVERIFY(Dialog);
    auto *Side = Dialog->findChild<QComboBox *>("flowSide");
    Side->setCurrentIndex(Side->findData("primary"));
    auto *Table = Dialog->findChild<QTableView *>("flowNodes");
    auto *Rows = qobject_cast<ComparisonModel *>(Table->model());
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() && !Rows->row(0).isEmpty(),
                             Timeout);
    int Proved = -1;
    for (int N = 0; N < Rows->rowCount(); ++N)
      if (Rows->row(N)["progress_loop"].isObject()) {
        Proved = N;
        break;
      }
    QVERIFY(Proved >= 0);
    QVERIFY(Rows->data(Rows->index(Proved, 4), Qt::ToolTipRole)
                .toString()
                .contains("progress_loop"));
    auto *Status = Dialog->findChild<QLabel *>("flowFunctionStatus");
    QVERIFY(Status->text().contains("Counter loops"));
    QVERIFY(Status->toolTip().contains("Source contract:"));
    QVERIFY(Status->toolTip().contains("willreturn"));
    QVERIFY(Dialog->findChild<QLabel *>("flowSummary")
                ->text()
                .contains("loop analysis"));
    Table->selectRow(Proved);
    auto *LoopStatus = Dialog->findChild<QLabel *>("flowLoopStatus");
    QVERIFY(LoopStatus && LoopStatus->isVisible());
    QVERIFY(LoopStatus->text().contains("header block"));
    if (Version == 2) {
      const auto Witness = Rows->row(Proved)["progress_loop"].toObject();
      QVERIFY(Witness["header"] != Witness["block"]);
    }
    if (Version == 3) {
      QSet<int> Latches;
      for (int N = 0; N < Rows->rowCount(); ++N) {
        const auto Witness = Rows->row(N)["progress_loop"].toObject();
        if (Witness.isEmpty())
          continue;
        Latches.insert(Witness["block"].toInt());
        Table->selectRow(N);
        QVERIFY(LoopStatus->text().contains(
            QString("header block %1").arg(Witness["header"].toInt())));
        QVERIFY(LoopStatus->text().contains(
            QString("latch block %1").arg(Witness["block"].toInt())));
      }
      QCOMPARE(Latches.size(), 2);
      Table->selectRow(Proved);
    }
    if (Version >= 4) {
      const int GuardCount = Family >= 7 ? 1 : 2;
      QVERIFY(Status->text().contains("Counter loops: 1"));
      QVERIFY(Status->text().contains("Backedges: 1"));
      QVERIFY(Status->text().contains(QString("Guards: %1").arg(GuardCount)));
      QSet<int> Guards, Latches;
      for (int N = 0; N < Rows->rowCount(); ++N) {
        const auto Witness = Rows->row(N)["progress_loop"].toObject();
        if (Witness.isEmpty())
          continue;
        Guards.insert(Witness["block"].toInt());
        Latches.insert(Witness["latch"].toInt());
        Table->selectRow(N);
        QVERIFY(LoopStatus->text().contains(
            QString("latch block %1").arg(Witness["latch"].toInt())));
        QVERIFY(
            LoopStatus->text().contains(QString("guard #%1 in block %2")
                                            .arg(Witness["compare"].toInt())
                                            .arg(Witness["block"].toInt())));
        if (Version >= 5) {
          QCOMPARE(Witness["proof"].toString(),
                   QString(Family == 9   ? "invariant_modular"
                           : Family == 8 ? "parameter_modular"
                           : Family == 7 ? "modular"
                                         : "nowrap"));
          QVERIFY(LoopStatus->text().contains(
              Family >= 7 ? "every counter cycle reaches a shared exit"
                          : "monotone counter with a no-overflow contract"));
          if (Family == 9) {
            QVERIFY(LoopStatus->text().contains("input-derived expression"));
            QVERIFY(
                LoopStatus->text().contains("Undefined behavior may occur"));
          }
          if (Family == 8) {
            QVERIFY(
                LoopStatus->text().contains("relative to an input parameter"));
            QVERIFY(
                LoopStatus->text().contains("Undefined behavior may occur"));
          }
        }
      }
      QCOMPARE(Guards.size(), GuardCount);
      QCOMPARE(Latches.size(), 1);
      Table->selectRow(Proved);
    }
    QTest::mouseClick(Dialog->findChild<QPushButton *>("flowBackward"),
                      Qt::LeftButton);
    auto *Slice = Dialog->findChild<QTableView *>("flowSlice");
    QTRY_VERIFY_WITH_TIMEOUT(Slice->model()->rowCount(), Timeout);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        Dialog->resize(1280, 800);
        QTest::qWait(40);
        QVERIFY(Dialog->grab().save(Captures +
                                    (Family == 9    ? "/invariant-bound-proof"
                                     : Family == 8  ? "/parameter-bound-proof"
                                     : Family == 7  ? "/modular-proof"
                                     : Version == 5 ? "/nowrap-proof"
                                     : Family == 6  ? "/common-update"
                                     : Family == 5  ? "/selected-updates"
                                     : Version == 3 ? "/nested"
                                     : Version == 2 ? "/multiblock"
                                                    : "/singleblock") +
                                    (Mode == Theme::Mode::Dark
                                         ? "-loop-progress-dark.png"
                                         : "-loop-progress-light.png")));
      }
    }
    Side->setCurrentIndex(Side->findData("secondary"));
    QTRY_VERIFY_WITH_TIMEOUT(
        Status->text().contains(Version >= 2 ? "Counter loops"
                                             : "acyclic control flow"),
        Timeout);
    const auto Saved = Directory.filePath("progress.nddiff");
    Session.command("comparison_save", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && QFile::exists(Saved), Timeout);
    Dialog->hide();
    Profile->setCurrentIndex(Profile->findData("med-ssa-v2"));
    bool Selected = false;
    QTimer Select;
    connect(&Select, &QTimer::timeout, this, [&] {
      if (auto *File =
              qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
        File->selectFile(Saved);
        if (auto *Name = File->findChild<QLineEdit *>("fileNameEdit"))
          Name->setText(Saved);
        Selected = true;
        static_cast<QDialog *>(File)->accept();
      }
    });
    Select.start(10);
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonReopen"),
                      Qt::LeftButton);
    Select.stop();
    QVERIFY(Selected);
    QTRY_COMPARE_WITH_TIMEOUT(Profile->currentData().toString(),
                              QString(RequestedProfile), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Failures.size(), 0);
  }
  void sharedFragmentsKeepExternalInputsSeparateAndRestoreSettings() {
    const auto A = Directory.filePath("fragments-before.bin");
    const auto B = Directory.filePath("fragments-after.bin");
    QVERIFY(write(A, QByteArray::fromHex("89f883c00383f0176bc00783c009c3")));
    QVERIFY(write(B, QByteArray::fromHex("89f883c00583f0176bc00783c009c3")));
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    View.findChild<QLineEdit *>("comparisonPrimary")->setText(A);
    View.findChild<QLineEdit *>("comparisonSecondary")->setText(B);
    for (const auto *Name : {"primaryLoader", "secondaryLoader"})
      View.findChild<QComboBox *>(Name)->setCurrentIndex(1);
    View.findChild<QLineEdit *>("primaryBase")->setText("0x1000");
    View.findChild<QLineEdit *>("secondaryBase")->setText("0x8000");
    auto *Fragments = View.findChild<QCheckBox *>("comparisonFlowFragments");
    auto *Regions = View.findChild<QCheckBox *>("comparisonAnalyzeFlowRegions");
    auto *Flow = View.findChild<QCheckBox *>("comparisonAnalyzeDataFlow");
    auto *Profile = View.findChild<QComboBox *>("comparisonFlowProfile");
    QVERIFY(Fragments && Regions && Flow && Profile);
    Fragments->setChecked(true);
    QVERIFY(Regions->isChecked() && Flow->isChecked() && Profile->isEnabled());
    Profile->setCurrentIndex(Profile->findData("llvm-ssa-v1"));
    QTest::mouseClick(View.findChild<QToolButton *>("comparisonSettings"),
                      Qt::LeftButton);
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonStart"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    QCOMPARE(Failures.size(), 0);
    QCOMPARE(Session.summary()["flow_regions"].toObject()["profile"].toString(),
             QString("llvm-fragments-v1"));
    auto *Menu =
        View.findChild<QPushButton *>("comparisonRegionReview")->menu();
    QVERIFY(Menu && Menu->actions().size() == 2);
    Menu->actions()[1]->trigger();
    auto *Dialog =
        View.findChild<ComparisonRegionsDialog *>("comparisonFlowRegions");
    QVERIFY(Dialog);
    auto *Anchors = Dialog->findChild<QTableView *>("regionAnchors");
    auto *Nodes = Dialog->findChild<QTableView *>("regionInstructions");
    auto *Rows = qobject_cast<ComparisonModel *>(Nodes->model());
    auto *Coordinates = Dialog->findChild<QComboBox *>("flowRegionCoordinates");
    auto *Scope = Dialog->findChild<QLabel *>("flowFragmentScope");
    QTRY_VERIFY_WITH_TIMEOUT(Anchors->model()->rowCount() > 0, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() >= 4 && !Rows->row(0).isEmpty(),
                             Timeout);
    QVERIFY(Coordinates->isVisible() && Scope->isVisible());
    QVERIFY(Dialog->findChild<QLabel *>("regionSummary")
                ->text()
                .contains("llvm-fragments-v1"));
    const int Matched = Rows->rowCount();
    QVERIFY(
        Scope->text().contains(QString("Matched operations: %1").arg(Matched)));
    QVERIFY(Scope->text().contains("not compared or counted"));
    Coordinates->setCurrentIndex(Coordinates->findData("frontier"));
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() == 1 && !Rows->row(0).isEmpty(),
                             Timeout);
    const auto Boundary = Rows->row(0);
    for (const auto &[Side, Value] :
         {std::pair{"primary", "0x3"}, {"secondary", "0x5"}}) {
      const auto Node = Boundary[Side].toObject();
      QCOMPARE(Node["opcode"].toString(), QString("add"));
      const auto Originals = Node["originals"].toArray();
      QVERIFY(!Originals.empty());
      QVERIFY(!Originals[0].toObject()["bytes"].toString().isEmpty());
      bool Found = false;
      for (const auto &Input : Node["inputs"].toArray()) {
        const auto I = Input.toArray();
        Found |= I[0].toInt() == 1 && I[3].toString() == Value;
      }
      QVERIFY(Found);
    }
    QVERIFY(Rows->data(Rows->index(0, 0), Qt::ToolTipRole)
                .toString()
                .contains("have not been compared"));
    QSignalSpy Navigation(Dialog, &ComparisonRegionsDialog::inspectFunction);
    for (auto *Button : Dialog->findChildren<QPushButton *>())
      if (Button->text() == "Inspect secondary function")
        QTest::mouseClick(Button, Qt::LeftButton);
    QCOMPARE(Navigation.size(), 1);
    QCOMPARE(Navigation[0][0].toString(), QString("secondary"));
    QCOMPARE(Navigation[0][1].toString(), QString("0x8000"));
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        View.resize(1280, 800);
        Dialog->resize(1280, 800);
        QTest::qWait(40);
        const auto Suffix = Mode == Theme::Mode::Dark ? "dark" : "light";
        QVERIFY(View.grab().save(Captures + "/fragments-settings-" + Suffix +
                                 ".png"));
        QVERIFY(Dialog->grab().save(Captures + "/fragments-frontier-" + Suffix +
                                    ".png"));
      }
    }
    Coordinates->setCurrentIndex(Coordinates->findData("nodes"));
    QTRY_VERIFY_WITH_TIMEOUT(
        Rows->rowCount() == Matched && !Rows->row(0).isEmpty(), Timeout);
    for (int I = 0; I < Matched; ++I)
      for (const auto *Side : {"primary", "secondary"})
        QVERIFY(Rows->row(I)[Side].toObject()["id"] !=
                Boundary[Side].toObject()["id"]);
    const auto Saved = Directory.filePath("fragments.nddiff");
    Session.command("comparison_save", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && QFile::exists(Saved), Timeout);
    Dialog->hide();
    Flow->setChecked(false);
    QVERIFY(!Fragments->isChecked() && !Regions->isChecked());
    Profile->setCurrentIndex(Profile->findData("med-ssa-v2"));
    // Exercise the ordinary reopen button so the restored recipe repopulates
    // the settings widgets, not just the underlying worker session.
    bool Selected = false;
    QTimer Select;
    connect(&Select, &QTimer::timeout, this, [&] {
      if (auto *File =
              qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
        File->selectFile(Saved);
        if (auto *Name = File->findChild<QLineEdit *>("fileNameEdit"))
          Name->setText(Saved);
        Selected = true;
        static_cast<QDialog *>(File)->accept();
      }
    });
    Select.start(10);
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonReopen"),
                      Qt::LeftButton);
    Select.stop();
    QVERIFY(Selected);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Fragments->isChecked(),
                             Timeout);
    QVERIFY(Regions->isChecked() && Flow->isChecked());
    QCOMPARE(Profile->currentData().toString(), QString("llvm-ssa-v1"));
    QCOMPARE(Failures.size(), 0);
  }

  void savedProjectsCompareThroughTheSharedReader() {
    const auto A = Directory.filePath("saved-before.bin");
    const auto B = Directory.filePath("saved-after.bin");
    const auto DA = ProjectDatabase::pathFor(A);
    const auto DB = ProjectDatabase::pathFor(B);
    QVERIFY(write(A, QByteArray::fromHex("b811000000c3")));
    QVERIFY(write(B, QByteArray::fromHex("b809000000c3")));
    const auto Load = QJsonDocument(QJsonObject{{"loader", "binary"},
                                                {"processor", "x86_64"},
                                                {"platform", "sysv"},
                                                {"base", "0x1000"}});
    for (const auto &Path : {A, B}) {
      QCOMPARE(
          ProjectDatabase::writeProjectFile(Path, ".neverd-load.json", Load),
          QString());
      QCOMPARE(
          ProjectDatabase::writeProjectFile(
              Path, ".neverd-renames.json",
              QJsonDocument(QJsonArray{QJsonObject{
                  {"addr", "0x1000"},
                  {"renamed", Path == A ? "saved_check<int>"
                                        : "saved_secondary_check<int>"}}})),
          QString());
      QCOMPARE(ProjectDatabase::save(ProjectDatabase::pathFor(Path), Path, {}),
               QString());
      QVERIFY(QFile::remove(Path));
      QVERIFY(QDir(ProjectDatabase::projectFolder(Path)).removeRecursively());
    }
    const auto Read = [](const QString &Path) {
      QFile F(Path);
      return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
    };
    const auto Before = Read(DA);
    const auto After = Read(DB);
    // A user's normal unpack/recovery copy must remain separate from DIFF.
    const auto Recovery = ProjectDatabase::workingDirectory(DA);
    QVERIFY(QDir().mkpath(Recovery));
    const auto Sentinel = QDir(Recovery).filePath("pending-edit.txt");
    QVERIFY(write(Sentinel, "local recovery remains intact"));
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    View.findChild<QLineEdit *>("comparisonPrimary")->setText(DA);
    View.findChild<QLineEdit *>("comparisonSecondary")->setText(DB);
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonStart"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    QCOMPARE(Failures.size(), 0);
    const auto Primary = Session.summary()["primary"].toObject();
    QCOMPARE(Primary["path"].toString(), DA);
    QCOMPARE(Primary["project"].toObject()["input_name"].toString(),
             QString("saved-before.bin"));
    auto *Inventory = View.findChild<QTableView *>("comparisonFunctions");
    auto *Model = qobject_cast<ComparisonModel *>(Inventory->model());
    QTRY_VERIFY_WITH_TIMEOUT(Model->rowCount() > 0, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(
        Model->row(0)["name"].toString() == "saved_check<int>", Timeout);
    QCOMPARE(Model->row(0)["evidence_name"].toString(), QString("start"));
    QTextDocument NameTipDocument;
    NameTipDocument.setHtml(
        Model->data(Model->index(0, 1), Qt::ToolTipRole).toString());
    const auto NameTip = NameTipDocument.toPlainText();
    QVERIFY(NameTip.contains("saved_check<int>"));
    QVERIFY(NameTip.contains("saved_secondary_check<int>"));
    QVERIFY(NameTip.contains("start"));
    Inventory->setCurrentIndex(Model->index(0, 0));
    auto *Instructions = View.findChild<QTableView *>("comparisonInstructions");
    QTRY_VERIFY_WITH_TIMEOUT(Instructions->model()->rowCount() > 0, Timeout);
    auto *Evidence = View.findChild<QPlainTextEdit *>("comparisonEvidence");
    QVERIFY(Evidence);
    QVERIFY(Evidence->isReadOnly());
    QTRY_VERIFY_WITH_TIMEOUT(
        Evidence->toPlainText().contains("saved_check<int>") &&
            Evidence->toPlainText().contains("saved_secondary_check<int>") &&
            Evidence->toPlainText().contains("start"),
        Timeout);
    QVERIFY(
        Evidence->toPlainText().contains(Model->row(0)["identity"].toString()));
    QVERIFY(Evidence->toPlainText().contains(
        Model->row(0)["counterpart"].toObject()["identity"].toString()));
    auto *EvidenceTabs = View.findChild<QTabWidget *>();
    QVERIFY(EvidenceTabs);
    EvidenceTabs->setCurrentWidget(Evidence->parentWidget());
    View.resize(1000, 650);
    QVERIFY(Evidence->isVisible());
    QCOMPARE(Evidence->horizontalScrollBar()->maximum(), 0);
    Evidence->moveCursor(QTextCursor::End);
    QTRY_VERIFY(Evidence->viewport()->rect().contains(Evidence->cursorRect()));
    if (Evidence->verticalScrollBar()->maximum() > 0)
      QVERIFY(Evidence->verticalScrollBar()->value() > 0);
    if (const auto Captures =
            qEnvironmentVariable("NEVERD_COMPARISON_SCREENSHOTS");
        !Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      QVERIFY(
          View.grab().save(Captures + "/renamed-function-evidence-end.png"));
      Evidence->moveCursor(QTextCursor::Start);
      QVERIFY(View.grab().save(Captures + "/renamed-function-evidence.png"));
    }
    View.resize(1280, 800);
    QCOMPARE(Read(DA), Before);
    QCOMPARE(Read(DB), After);
    QCOMPARE(Read(Sentinel), QByteArray("local recovery remains intact"));
    QVERIFY(!QFileInfo::exists(A));
    QVERIFY(!QFileInfo::exists(B));
    auto *Source = View.findChild<ComparisonSourceView *>("comparisonSource");
    QVERIFY(Source);
    auto *Tabs = View.findChild<QTabWidget *>();
    QVERIFY(Tabs);
    Tabs->setCurrentWidget(Source);
    QCOMPARE(Tabs->currentWidget(), Source);
    QVERIFY(Source->isVisible());
    auto *PrimarySource =
        View.findChild<QPlainTextEdit *>("comparisonPrimarySource");
    auto *SecondarySource =
        View.findChild<QPlainTextEdit *>("comparisonSecondarySource");
    QTRY_VERIFY2_WITH_TIMEOUT(
        PrimarySource->toPlainText().contains("return"),
        qPrintable(
            View.findChild<QLabel *>("comparisonPrimarySourceStatus")->text()),
        Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(SecondarySource->toPlainText().contains("return"),
                             Timeout);
    QVERIFY(PrimarySource->toPlainText().contains("return"));
    QVERIFY(PrimarySource->toPlainText() != SecondarySource->toPlainText());
    auto *Alignment = View.findChild<QCheckBox *>("comparisonSourceAlignment");
    QVERIFY(Alignment && Alignment->isChecked());
    QCOMPARE(PrimarySource->blockCount(), SecondarySource->blockCount());
    QVERIFY(!PrimarySource->extraSelections().isEmpty());
    QVERIFY(!SecondarySource->extraSelections().isEmpty());
    QVERIFY(View.findChild<QLabel *>("comparisonPrimarySourceTitle")
                ->text()
                .contains("saved_check<int>"));
    auto *Jump =
        View.findChild<QPushButton *>("comparisonPrimarySourceInstruction");
    for (auto Block = PrimarySource->document()->begin();
         Block.isValid() && !Jump->isEnabled(); Block = Block.next())
      PrimarySource->setTextCursor(QTextCursor(Block));
    QVERIFY(Jump->isEnabled());
    QTest::mouseClick(Jump, Qt::LeftButton);
    QCOMPARE(Tabs->currentWidget(), Instructions);
    QVERIFY(Instructions->currentIndex().isValid());
    QCOMPARE(Instructions->currentIndex().column(), 0);
    Tabs->setCurrentWidget(Source);
    QTest::mouseClick(Alignment, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(PrimarySource->toPlainText().contains("return"),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(SecondarySource->toPlainText().contains("return"),
                             Timeout);
    QVERIFY(PrimarySource->extraSelections().isEmpty());
    const auto SecondaryPage = SecondarySource->toPlainText();
    auto *PrimaryLine =
        View.findChild<QSpinBox *>("comparisonPrimarySourceLine");
    QVERIFY(PrimaryLine->value() > 1);
    PrimaryLine->setFocus();
    PrimaryLine->setValue(1);
    bool UnexpectedDialog = false;
    QTimer::singleShot(100, &View, [&] {
      if (auto *Modal =
              qobject_cast<QDialog *>(QApplication::activeModalWidget());
          Modal && Modal != &View) {
        UnexpectedDialog = true;
        qWarning() << "Source line Enter opened"
                   << Modal->metaObject()->className() << Modal->windowTitle();
        Modal->reject();
      }
    });
    QTest::keyClick(PrimaryLine, Qt::Key_Return);
    QTest::qWait(120);
    QVERIFY(!UnexpectedDialog);
    QTRY_VERIFY_WITH_TIMEOUT(PrimarySource->toPlainText().startsWith("#define"),
                             Timeout);
    QCOMPARE(SecondarySource->toPlainText(), SecondaryPage);
    auto *Representations =
        View.findChild<QComboBox *>("comparisonSourceRepresentation");
    Representations->setCurrentIndex(Representations->findData("llvmc"));
    QTRY_VERIFY_WITH_TIMEOUT(PrimarySource->toPlainText().contains("return"),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(SecondarySource->toPlainText().contains("return"),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Representations->setCurrentIndex(Representations->findData("rust"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    QVERIFY(PrimarySource->toPlainText().isEmpty());
    QVERIFY(SecondarySource->toPlainText().isEmpty());
    QVERIFY(View.findChild<QLabel *>("comparisonPrimarySourceStatus")
                ->text()
                .contains("Source unavailable"));
    QVERIFY(!Jump->isEnabled());
    Tabs->setCurrentWidget(Instructions);
    Tabs->setCurrentWidget(Source);
    QVERIFY(!Session.cancellable());
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonSourceReload"),
                      Qt::LeftButton);
    QVERIFY(Session.cancellable());
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    // Multiple queued view changes must publish only the last selection.
    Representations->setCurrentIndex(Representations->findData("c"));
    Representations->setCurrentIndex(Representations->findData("llvmc"));
    QTRY_VERIFY_WITH_TIMEOUT(PrimarySource->toPlainText().contains("return"),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(SecondarySource->toPlainText().contains("return"),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    QTest::mouseClick(Alignment, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!PrimarySource->extraSelections().isEmpty(),
                             Timeout);
    QCOMPARE(PrimarySource->blockCount(), SecondarySource->blockCount());
    const int PrimaryMinimum = PrimarySource->minimumHeight();
    const int SecondaryMinimum = SecondarySource->minimumHeight();
    PrimarySource->setMinimumHeight(0);
    SecondarySource->setMinimumHeight(0);
    PrimarySource->setMaximumHeight(PrimarySource->fontMetrics().height() * 2);
    SecondarySource->setMaximumHeight(SecondarySource->fontMetrics().height() *
                                      2);
    QTRY_VERIFY_WITH_TIMEOUT(PrimarySource->verticalScrollBar()->maximum() > 0,
                             Timeout);
    PrimarySource->verticalScrollBar()->setValue(1);
    QCOMPARE(SecondarySource->verticalScrollBar()->value(), 1);
    PrimarySource->setMaximumHeight(QWIDGETSIZE_MAX);
    SecondarySource->setMaximumHeight(QWIDGETSIZE_MAX);
    PrimarySource->setMinimumHeight(PrimaryMinimum);
    SecondarySource->setMinimumHeight(SecondaryMinimum);
    PrimarySource->verticalScrollBar()->setValue(0);
    const auto SourceCaptures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!SourceCaptures.isEmpty()) {
      QVERIFY(QDir().mkpath(SourceCaptures));
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(40);
        const auto Name =
            Mode == Theme::Mode::Dark ? "dark-source" : "light-source";
        QVERIFY(View.grab().save(
            QDir(SourceCaptures).filePath(QString(Name) + ".png")));
      }
    }
    if (!SourceCaptures.isEmpty()) {
      const QString LongName = "saved_check_" +
                               QString("template_argument_").repeated(32) +
                               "suffix";
      Source->setFunctions({{"name", LongName}, {"address", "0x1000"}},
                           {{"name", LongName}, {"address", "0x1000"}});
      QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
      View.resize(View.minimumSize());
      QTRY_VERIFY_WITH_TIMEOUT(PrimarySource->viewport()->height() >=
                                   PrimarySource->fontMetrics().height() * 6,
                               Timeout);
      auto *Title = View.findChild<QLabel *>("comparisonPrimarySourceTitle");
      QTRY_VERIFY_WITH_TIMEOUT(Title->text().contains(QChar(0x2026)), Timeout);
      QVERIFY(Title->toolTip().contains(LongName));
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(40);
        QVERIFY(View.grab().save(
            QDir(SourceCaptures)
                .filePath(Mode == Theme::Mode::Dark
                              ? "dark-source-minimum-long.png"
                              : "light-source-minimum-long.png")));
      }
      View.resize(1280, 800);
      Source->setFunctions({{"name", "saved_check"}, {"address", "0x1000"}},
                           {{"name", "saved_check"}, {"address", "0x1000"}});
      QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    }
    Tabs->setCurrentWidget(Instructions);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      for (const auto &ThemeName : {QString("light"), QString("dark")}) {
        Theme::instance().setMode(ThemeName == "dark" ? Theme::Mode::Dark
                                                      : Theme::Mode::Light);
        QTest::qWait(40);
        QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
        QVERIFY(View.grab().save(Captures + "/" + ThemeName +
                                 "-saved-projects.png"));
      }
    }
    QVERIFY(QFile::remove(Sentinel));
  }

  void counterpartPickerPagesSearchesAndPreservesMappingIntent() {
    constexpr int Count = 417;
    const auto A = Directory.filePath("candidate-before.bin");
    const auto B = Directory.filePath("candidate-after.bin");
    auto Bytes =
        QByteArray::fromHex("b80700000083c003c3cccccccccccccc").repeated(Count);
    QVERIFY(write(A, Bytes));
    Bytes.back() = char(0xdd);
    QVERIFY(write(B, Bytes));
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [](const QString &Path, const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"platform", "sysv"},
                                                      {"base", Base}}}};
    };
    Session.start(Input(A, "0x1000"), Input(B, "0x0"), {{"use_names", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    Session.command(
        "comparison_override",
        {{"kind", "match"}, {"primary", "0x1000"}, {"secondary", "0x0"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    const auto OriginalCounts = Session.summary()["counts"];
    const auto OriginalRevision = Session.revision();
    auto *Inventory = View.findChild<QTableView *>("comparisonFunctions");
    auto *Functions = qobject_cast<ComparisonModel *>(Inventory->model());
    Inventory->sortByColumn(0, Qt::AscendingOrder);
    QTRY_VERIFY_WITH_TIMEOUT(Functions->rowCount() >= Count, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Functions->row(1)["address"].toString(),
                              QString("0x1010"), Timeout);
    Inventory->selectRow(1);
    auto *Open = View.findChild<QPushButton *>("comparisonPairWith");
    QTRY_VERIFY_WITH_TIMEOUT(Open->isEnabled(), Timeout);
    QTest::mouseClick(Open, Qt::LeftButton);
    QTRY_VERIFY(View.findChild<ComparisonCounterpartDialog *>());
    auto *Picker = View.findChild<ComparisonCounterpartDialog *>();
    QCOMPARE(Picker->windowModality(), Qt::NonModal);
    QVERIFY(!QApplication::activeModalWidget());
    View.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&View));
    View.setFocus();
    QVERIFY(Picker->isVisible());
    Picker->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(Picker));
    auto *Table = Picker->findChild<QTableView *>("counterpartFunctions");
    auto *Model = qobject_cast<ComparisonModel *>(Table->model());
    auto *Search = Picker->findChild<QLineEdit *>("counterpartSearch");
    auto *Sort = Picker->findChild<QComboBox *>("counterpartSort");
    auto *Pair = Picker->findChild<QPushButton *>("counterpartPair");
    auto *Status = Picker->findChild<QLabel *>("counterpartStatus");
    QTRY_VERIFY_WITH_TIMEOUT(Model->rowCount() >= Count - 1, Timeout);
    QVERIFY(Status->text().contains("hypotheses"));
    QVERIFY(!Pair->isEnabled());
    const int Last = Model->rowCount() - 1;
    Table->scrollTo(Model->index(Last, 1));
    QTRY_VERIFY_WITH_TIMEOUT(!Model->row(Last).isEmpty(), Timeout);
    const auto LastAddress = Model->row(Last)["address"].toString();
    Table->selectRow(Last);
    QTRY_VERIFY(Pair->isEnabled());
    QVERIFY(Picker->findChild<QLabel *>("counterpartSelection")
                ->text()
                .contains(LastAddress));
    QVERIFY(Model->cachedPages() >= 2);
    Sort->setCurrentIndex(1);
    QVERIFY(!Pair->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(Model->row(0)["address"].toString(), LastAddress,
                              Timeout);
    Search->setText("0x20");
    QTest::keyClick(Search, Qt::Key_Return);
    QCoreApplication::processEvents();
    Search->setText("no matching function");
    QVERIFY(!Pair->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(Status->text().contains("No ambiguity candidates"),
                             Timeout);
    QCOMPARE(Model->rowCount(), 0);
    QCOMPARE(Session.revision(), OriginalRevision);
    Search->setText(LastAddress);
    QTRY_COMPARE_WITH_TIMEOUT(Model->rowCount(), 1, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Model->row(0)["address"].toString(), LastAddress,
                              Timeout);
    Table->selectRow(0);
    QTRY_VERIFY(Pair->isEnabled());
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QDir().mkpath(Captures);
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        const auto Prefix = Mode == Theme::Mode::Dark ? "dark" : "light";
        Picker->resize(940, 620);
        QTest::qWait(100);
        QVERIFY(
            Picker->grab().save(Captures + '/' + Prefix + "-counterpart.png"));
        Picker->resize(Picker->minimumSize());
        QTest::qWait(100);
        QVERIFY(Picker->grab().save(Captures + '/' + Prefix +
                                    "-counterpart-minimum.png"));
      }
    }
    QTest::mouseClick(Pair, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(
        !Session.busy() && Session.revision() != OriginalRevision, Timeout);
    QJsonObject Paired;
    Session.request("comparison_functions",
                    {{"side", "primary"}, {"search", "0x1010"}}, &View,
                    [&](const QJsonObject &Message) {
                      Paired = Message["payload"].toObject();
                    });
    QTRY_VERIFY_WITH_TIMEOUT(Paired.contains("items"), Timeout);
    QCOMPARE(Paired["items"]
                 .toArray()
                 .first()
                 .toObject()["counterpart"]
                 .toObject()["address"]
                 .toString(),
             LastAddress);
    Session.command("comparison_undo");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Session.summary()["counts"], OriginalCounts);
    // All-function search admits address zero and explains displaced pairs.
    ComparisonCounterpartDialog Manual(Session, "primary", "0x1010", {}, &View);
    Manual.show();
    auto *All = Manual.findChild<QComboBox *>("counterpartScope");
    auto *ManualSearch = Manual.findChild<QLineEdit *>("counterpartSearch");
    auto *ManualTable = Manual.findChild<QTableView *>("counterpartFunctions");
    auto *ManualModel = qobject_cast<ComparisonModel *>(ManualTable->model());
    auto *ManualPair = Manual.findChild<QPushButton *>("counterpartPair");
    All->setCurrentIndex(1);
    ManualSearch->setText("0x0");
    QTRY_COMPARE_WITH_TIMEOUT(ManualModel->rowCount(), 1, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(ManualModel->row(0)["address"].toString(),
                              QString("0x0"), Timeout);
    ManualTable->selectRow(0);
    QTRY_VERIFY(ManualPair->isEnabled());
    QVERIFY(Manual.findChild<QLabel *>("counterpartSelection")
                ->text()
                .contains("0x1000"));
    const auto BeforeManual = Session.revision();
    QTest::keyClick(ManualSearch, Qt::Key_Return);
    QCOMPARE(Session.revision(), BeforeManual);
    QTRY_COMPARE_WITH_TIMEOUT(ManualModel->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!ManualModel->row(0).isEmpty(), Timeout);
    ManualTable->selectRow(0);
    QTest::mouseClick(ManualPair, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(
        !Session.busy() && Session.revision() != BeforeManual, Timeout);
    Session.command("comparison_undo");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Session.summary()["counts"], OriginalCounts);
    // Browsing the reverse direction and cancelling never records an edit.
    ComparisonCounterpartDialog Existing(Session, "primary", "0x1000", "0x0",
                                         &View);
    Existing.show();
    QTRY_VERIFY_WITH_TIMEOUT(Existing.findChild<QLabel *>("counterpartStatus")
                                 ->text()
                                 .contains("no retained ambiguity group"),
                             Timeout);
    Existing.findChild<QComboBox *>("counterpartScope")->setCurrentIndex(1);
    Existing.findChild<QLineEdit *>("counterpartSearch")->setText("0x0");
    auto *ExistingTable =
        Existing.findChild<QTableView *>("counterpartFunctions");
    auto *ExistingModel =
        qobject_cast<ComparisonModel *>(ExistingTable->model());
    QTRY_COMPARE_WITH_TIMEOUT(ExistingModel->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!ExistingModel->row(0).isEmpty(), Timeout);
    ExistingTable->selectRow(0);
    QVERIFY(!Existing.findChild<QPushButton *>("counterpartPair")->isEnabled());
    QVERIFY(Existing.findChild<QLabel *>("counterpartSelection")
                ->text()
                .contains("already paired"));
    QTest::keyClick(&Existing, Qt::Key_Escape);
    ComparisonCounterpartDialog Cancelled(Session, "secondary", "0x10", {},
                                          &View);
    Cancelled.show();
    auto *ReverseTable =
        Cancelled.findChild<QTableView *>("counterpartFunctions");
    auto *ReverseModel = qobject_cast<ComparisonModel *>(ReverseTable->model());
    QTRY_VERIFY_WITH_TIMEOUT(ReverseModel->rowCount() >= Count - 1, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(ReverseModel->row(0)["address"].toString(),
                              QString("0x1010"), Timeout);
    const auto BeforeCancel = Session.revision();
    QTest::keyClick(&Cancelled, Qt::Key_Escape);
    QVERIFY(!Cancelled.isVisible());
    QCOMPARE(Session.revision(), BeforeCancel);
    ComparisonCounterpartDialog Stale(Session, "primary", "0x1010", {}, &View);
    Stale.show();
    auto *StaleTable = Stale.findChild<QTableView *>("counterpartFunctions");
    auto *StaleModel = qobject_cast<ComparisonModel *>(StaleTable->model());
    auto *StalePair = Stale.findChild<QPushButton *>("counterpartPair");
    QTRY_VERIFY_WITH_TIMEOUT(!StaleModel->row(0).isEmpty(), Timeout);
    StaleTable->selectRow(0);
    QTRY_VERIFY(StalePair->isEnabled());
    Session.command(
        "comparison_override",
        {{"kind", "match"}, {"primary", "0x1010"}, {"secondary", "0x20"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(!StalePair->isEnabled());
    QCOMPARE(StaleModel->rowCount(), 0);
    QVERIFY(Stale.findChild<QLabel *>("counterpartStatus")
                ->text()
                .contains("comparison changed"));
    QCOMPARE(Failures.count(), 0);

    // A six-cycle of measured alternatives must reach the picker as six
    // edges, without losing an overlap or inventing cross-pairs.
    const auto CycleInput = [&](const QString &Name, bool Secondary) {
      const QList<QByteArray> Operations =
          Secondary ? QList<QByteArray>{"01c831c8", "29c809c8", "21c80fafc1"}
                    : QList<QByteArray>{"01c829c8", "31c821c8", "09c80fafc1"};
      QByteArray Data;
      for (const auto &Ops : Operations) {
        const auto Code = QByteArray::fromHex("b807000000" + Ops + "c3");
        Data += Code + QByteArray(16 - Code.size(), char(0xcc));
      }
      const auto Path = Directory.filePath(Name);
      if (!write(Path, Data))
        return QJsonObject{};
      return Input(Path, Secondary ? "0x8000" : "0x1000");
    };
    ComparisonDialog OverlapView(TEST_COMPARISON_WORKER);
    auto &OverlapSession = OverlapView.session();
    QSignalSpy OverlapFailures(&OverlapSession, &ComparisonSession::failed);
    OverlapView.show();
    QTRY_VERIFY_WITH_TIMEOUT(OverlapSession.ready(), Timeout);
    const auto CyclePrimary = CycleInput("cycle-before.bin", false);
    const auto CycleSecondary = CycleInput("cycle-after.bin", true);
    QVERIFY(!CyclePrimary.isEmpty() && !CycleSecondary.isEmpty());
    OverlapSession.start(CyclePrimary, CycleSecondary,
                         {{"use_names", false},
                          {"min_similarity", 0.8},
                          {"ambiguity_margin", 0}});
    QTRY_VERIFY_WITH_TIMEOUT(!OverlapSession.busy() &&
                                 !OverlapSession.revision().isEmpty(),
                             Timeout);
    ComparisonCounterpartDialog Choice(OverlapSession, "primary", "0x1010", {},
                                       &OverlapView);
    Choice.show();
    auto *Choices = Choice.findChild<QTableView *>("counterpartFunctions");
    auto *ChoiceModel = qobject_cast<ComparisonModel *>(Choices->model());
    QTRY_COMPARE_WITH_TIMEOUT(ChoiceModel->rowCount(), 2, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(ChoiceModel->row(0)["address"].toString(),
                              QString("0x8000"), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(ChoiceModel->row(1)["address"].toString(),
                              QString("0x8020"), Timeout);
    Choices->selectRow(1);
    auto *Choose = Choice.findChild<QPushButton *>("counterpartPair");
    QTRY_VERIFY(Choose->isEnabled());
    const auto BeforeChoice = OverlapSession.revision();
    QTest::mouseClick(Choose, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!OverlapSession.busy() &&
                                 OverlapSession.revision() != BeforeChoice,
                             Timeout);
    OverlapSession.command("comparison_undo");
    QTRY_VERIFY_WITH_TIMEOUT(!OverlapSession.busy(), Timeout);
    ComparisonCounterpartDialog ReverseChoice(OverlapSession, "secondary",
                                              "0x8020", {}, &OverlapView);
    ReverseChoice.show();
    auto *ReverseChoices =
        ReverseChoice.findChild<QTableView *>("counterpartFunctions");
    auto *ReverseChoiceModel =
        qobject_cast<ComparisonModel *>(ReverseChoices->model());
    QTRY_COMPARE_WITH_TIMEOUT(ReverseChoiceModel->rowCount(), 2, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(ReverseChoiceModel->row(0)["address"].toString(),
                              QString("0x1010"), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(ReverseChoiceModel->row(1)["address"].toString(),
                              QString("0x1020"), Timeout);
    QCOMPARE(OverlapFailures.count(), 0);
  }
  void scalarValidationShowsAssumptionsAndClearsStaleResults() {
    const auto A = Directory.filePath("scalar-before.bin");
    const auto B = Directory.filePath("scalar-after.bin");
    QVERIFY(write(A, QByteArray::fromHex("89f883c003c3")));
    QVERIFY(write(B, QByteArray::fromHex("89f883c00183c002c3")));
    ComparisonSession Session(TEST_COMPARISON_WORKER);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [](const QString &Path) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"platform", "sysv"},
                                                      {"base", 4096}}}};
    };
    Session.start(Input(A), Input(B), {{"discover", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    Session.command(
        "comparison_override",
        {{"kind", "match"}, {"primary", "0x1000"}, {"secondary", "0x1000"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    ComparisonValidationDialog View(Session, nullptr);
    View.show();
    View.review("primary", "0x1000");
    auto *Report = View.findChild<QPlainTextEdit *>("validationReport");
    auto *Evidence = View.findChild<QPlainTextEdit *>("validationEvidence");
    auto *Status = View.findChild<QLabel *>("validationStatus");
    auto *Run = View.findChild<QPushButton *>("validationRun");
    QTRY_VERIFY_WITH_TIMEOUT(!Report->toPlainText().isEmpty(), Timeout);
    auto Result =
        QJsonDocument::fromJson(Report->toPlainText().toUtf8()).object();
    QCOMPARE(Result["status"].toString(), QString("proved"));
    QCOMPARE(Result["input_source_binding"].toString(),
             QString("owned_sessions"));
    QVERIFY(Result["inputs_bound"].toBool());
    QVERIFY(!Result["inputs_revalidated"].toBool());
    QVERIFY(Status->text().contains("lifted scalar models"));
    QVERIFY(Evidence->toPlainText().contains("defined integer bitvectors"));
    QVERIFY(Evidence->toPlainText().contains("register offset 56"));
    QVERIFY(Evidence->toPlainText().contains("does not assess vulnerability"));
    QVERIFY(Evidence->toPlainText().contains("retained comparison sources"));
    QVERIFY(Run->isEnabled());
    auto *Work = View.findChild<QSpinBox *>("validationWork");
    auto *Instructions = View.findChild<QSpinBox *>("validationInstructions");
    QVERIFY(Work && Instructions);
    const auto Limits = Result["limits"].toObject();
    QCOMPARE(Work->value(), Limits["proof_work"].toInt());
    QCOMPARE(Instructions->value(),
             Limits["native_instructions_per_function"].toInt());
    for (auto *Limit : {Work, Instructions}) {
      const auto Previous = Limit->value();
      Limit->setValue(1);
      QVERIFY(Report->toPlainText().isEmpty());
      QVERIFY(Evidence->toPlainText().isEmpty());
      QVERIFY(Status->text().contains("Limits changed"));
      QTest::mouseClick(Run, Qt::LeftButton);
      QVERIFY(!Work->isEnabled());
      QVERIFY(!Instructions->isEnabled());
      QTRY_VERIFY_WITH_TIMEOUT(!Report->toPlainText().isEmpty(), Timeout);
      Result = QJsonDocument::fromJson(Report->toPlainText().toUtf8()).object();
      QCOMPARE(Result["status"].toString(), QString("budget_exceeded"));
      QCOMPARE(
          Result["limits"]
              .toObject()[Limit == Work ? "proof_work"
                                        : "native_instructions_per_function"]
              .toInt(),
          1);
      QVERIFY(Status->text().contains("Adjust the limits"));
      Limit->setValue(Previous);
      QVERIFY(Report->toPlainText().isEmpty());
    }
    QTest::mouseClick(Run, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Report->toPlainText().isEmpty(), Timeout);
    Result = QJsonDocument::fromJson(Report->toPlainText().toUtf8()).object();
    QCOMPARE(Result["status"].toString(), QString("proved"));
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QDir().mkpath(Captures);
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        const auto Prefix = Mode == Theme::Mode::Dark ? "dark" : "light";
        View.resize(920, 680);
        QTest::qWait(100);
        QVERIFY(View.grab().save(Captures + '/' + Prefix + "-validation.png"));
        View.resize(View.minimumSize());
        QTest::qWait(100);
        QVERIFY(View.grab().save(Captures + '/' + Prefix +
                                 "-validation-minimum.png"));
      }
    }
    Session.command(
        "comparison_override",
        {{"kind", "confirm"}, {"primary", "0x1000"}, {"secondary", "0x1000"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_VERIFY(Report->toPlainText().isEmpty());
    QVERIFY(Evidence->toPlainText().isEmpty());
    QVERIFY(!Run->isEnabled());
    View.review("primary", "0x1000");
    QTRY_VERIFY_WITH_TIMEOUT(!Report->toPlainText().isEmpty(), Timeout);
    QCOMPARE(QJsonDocument::fromJson(Report->toPlainText().toUtf8())
                 .object()["status"]
                 .toString(),
             QString("proved"));
    const auto Unsupported = Directory.filePath("scalar-memory.bin");
    QVERIFY(write(Unsupported, QByteArray::fromHex("8b07c3")));
    Session.start(Input(Unsupported), Input(Unsupported),
                  {{"discover", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_VERIFY(Report->toPlainText().isEmpty());
    View.review("primary", "0x1000");
    QTRY_VERIFY_WITH_TIMEOUT(!Report->toPlainText().isEmpty(), Timeout);
    QCOMPARE(QJsonDocument::fromJson(Report->toPlainText().toUtf8())
                 .object()["status"]
                 .toString(),
             QString("unsupported"));
    QVERIFY(Status->text().contains("outside the supported scalar model"));
  }

  void largeScalarValidationRemainsInteractive() {
    const auto Input = qEnvironmentVariable("NEVERD_DIFF_SCALAR_PROBE_INPUT");
    const auto Address =
        qEnvironmentVariable("NEVERD_DIFF_SCALAR_PROBE_ADDRESS");
    if (Input.isEmpty() && Address.isEmpty())
      QSKIP("Opt-in local large-input scalar GUI probe.");
    QVERIFY2(!Input.isEmpty() && !Address.isEmpty(),
             "Set both probe input and scalar function address.");
    const auto WaitUntil = [](auto Ready) {
      QElapsedTimer Wait;
      Wait.start();
      while (!Ready() && Wait.elapsed() < Timeout)
        QTest::qWait(1);
      return Ready();
    };
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    View.show();
    QVERIFY(WaitUntil([&] { return Session.ready(); }));
    View.findChild<QLineEdit *>("comparisonPrimary")->setText(Input);
    View.findChild<QLineEdit *>("comparisonSecondary")->setText(Input);
    QElapsedTimer Clock;
    Clock.start();
    qint64 LastTick = 0, MaxGap = 0;
    int Ticks = 0;
    QTimer Heartbeat;
    Heartbeat.setInterval(1);
    connect(&Heartbeat, &QTimer::timeout, this, [&] {
      const auto Now = Clock.elapsed();
      MaxGap = std::max(MaxGap, Now - LastTick);
      LastTick = Now;
      ++Ticks;
    });
    Heartbeat.start();
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonStart"),
                      Qt::LeftButton);
    QVERIFY(WaitUntil([&] {
      return !Session.busy() && !Session.cancellable() &&
             !Session.revision().isEmpty();
    }));
    QVERIFY(Failures.empty());
    const auto Preparation = Clock.elapsed();
    ComparisonValidationDialog Validation(Session, &View);
    Validation.show();
    auto *Report = Validation.findChild<QPlainTextEdit *>("validationReport");
    auto *Run = Validation.findChild<QPushButton *>("validationRun");
    QJsonArray Times;
    QJsonObject Result;
    for (int Attempt = 0; Attempt < 3; ++Attempt) {
      const auto Begin = Clock.elapsed();
      if (!Attempt)
        Validation.review("primary", Address);
      else {
        QVERIFY(Run->isEnabled());
        QTest::mouseClick(Run, Qt::LeftButton);
      }
      QVERIFY(WaitUntil([&] { return !Report->toPlainText().isEmpty(); }));
      QTest::qWait(1); // Include the result's next paint/event-loop turn.
      Times.append(Clock.elapsed() - Begin);
      Result = QJsonDocument::fromJson(Report->toPlainText().toUtf8()).object();
      QCOMPARE(Result["status"].toString(), QString("proved"));
      QCOMPARE(Result["input_source_binding"].toString(),
               QString("owned_sessions"));
    }
    Heartbeat.stop();
    MaxGap = std::max(MaxGap, Clock.elapsed() - LastTick);
    QVERIFY(Ticks > 0);
    QVERIFY(Failures.empty());
    const auto Measurement =
        QJsonDocument(
            QJsonObject{
                {"track",
                 "local offscreen GUI probe; not competitive qualification"},
                {"input", Input},
                {"address", Address},
                {"preparation_ms", Preparation},
                {"fresh_query_round_trip_ms", Times},
                {"heartbeat_max_gap_ms", MaxGap},
                {"heartbeat_ticks", Ticks},
                {"primary_sha256", Result["primary_sha256"]},
                {"primary_lift", Result["primary_lift"]},
                {"proof_work", Result["work"]}})
            .toJson(QJsonDocument::Indented);
    qInfo().noquote() << Measurement;
    const auto Output = qEnvironmentVariable("NEVERD_DIFF_SCALAR_PROBE_REPORT");
    if (!Output.isEmpty())
      QVERIFY(write(Output, Measurement));
  }

  void inputsPagesEditingAndRender() {
    const auto A = Directory.filePath("original.bin"),
               B = Directory.filePath("patched.bin");
    // cmp edi, limit; je second; mov eax, 2; ret; second: mov eax, 3; ret
    auto Bytes = QByteArray::fromHex("83ff117406b802000000c3b803000000c3");
    QVERIFY(write(A, Bytes));
    Bytes[2] = 9;
    QVERIFY(write(B, Bytes));
    QPointer<ComparisonDialog> Dialog =
        new ComparisonDialog(TEST_COMPARISON_WORKER);
    Dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    QSignalSpy Failure(&Dialog->session(), &ComparisonSession::failed);
    Dialog->show();
    QTRY_VERIFY_WITH_TIMEOUT(Dialog->session().ready(), Timeout);
    auto *Primary = Dialog->findChild<QLineEdit *>("comparisonPrimary");
    auto *Secondary = Dialog->findChild<QLineEdit *>("comparisonSecondary");
    Primary->setText(A);
    Secondary->setText(B);
    for (const char *Name : {"primaryLoader", "secondaryLoader"})
      Dialog->findChild<QComboBox *>(Name)->setCurrentIndex(1);
    for (const char *Name : {"primaryBase", "secondaryBase"})
      Dialog->findChild<QLineEdit *>(Name)->setText("0x1000");
    auto *Start = Dialog->findChild<QPushButton *>("comparisonStart");
    QVERIFY(Start->isEnabled());
    int Ticks = 0;
    QTimer Heartbeat;
    Heartbeat.setInterval(1);
    connect(&Heartbeat, &QTimer::timeout, this, [&] { ++Ticks; });
    Heartbeat.start();
    QTest::mouseClick(Start, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Dialog->session().busy(), Timeout);
    QVERIFY2(Failure.empty(),
             qPrintable(Failure.empty() ? QString()
                                        : Failure.first()[0].toString()));
    QVERIFY(Ticks > 0);
    QCOMPARE(
        Dialog->session().summary()["counts"].toObject()["changed"].toInt(), 1);
    auto *Functions = Dialog->findChild<QTableView *>("comparisonFunctions");
    auto *Instructions =
        Dialog->findChild<QTableView *>("comparisonInstructions");
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 1, Timeout);
    Functions->setCurrentIndex(Functions->model()->index(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT(Instructions->model()->rowCount() >= 6, Timeout);
    auto *Rows = qobject_cast<ComparisonModel *>(Instructions->model());
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QVERIFY(Rows->cachedPages() <= 16);
    QCOMPARE(Rows->row(0)["kind"].toString(), QString("constant"));
    const auto Revision = Dialog->session().revision();
    Dialog->session().command(
        "comparison_override",
        {{"kind", "confirm"}, {"primary", "0x1000"}, {"secondary", "0x1000"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Dialog->session().busy(), Timeout);
    QVERIFY(Revision != Dialog->session().revision());
    auto *Undo = Dialog->findChild<QPushButton *>("comparisonUndo");
    QTRY_VERIFY(Undo->isEnabled());
    QTest::mouseClick(Undo, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Dialog->session().busy(), Timeout);
    QVERIFY(Dialog->findChild<QPushButton *>("comparisonRedo")->isEnabled());
    const auto Saved = Directory.filePath("review.nddiff");
    Dialog->session().command("comparison_save", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Dialog->session().busy(), Timeout);
    QVERIFY(QFile::exists(Saved));
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 1, Timeout);
    Functions->setCurrentIndex(Functions->model()->index(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT(Instructions->model()->rowCount() >= 6, Timeout);

    auto *GraphTabs = Dialog->findChild<QTabWidget *>();
    // The offscreen platform has a small virtual screen; choose a known
    // review window before checking fit rather than assuming its screen size.
    Dialog->resize(1280, 800);
    GraphTabs->setCurrentIndex(1);
    for (auto *Graph : Dialog->findChildren<ComparisonGraph *>())
      QTRY_VERIFY_WITH_TIMEOUT(Graph->zoom() >= 0.4, Timeout);
    GraphTabs->setCurrentIndex(0);
    // Real renders use the production palette, fonts, icons and native widgets.
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QDir().mkpath(Captures);
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        const auto Prefix = Mode == Theme::Mode::Dark ? "dark" : "light";
        Dialog->resize(1280, 800);
        QTest::qWait(100);
        QVERIFY(
            Dialog->grab().save(Captures + '/' + Prefix + "-disassembly.png"));
        auto *Tabs = Dialog->findChild<QTabWidget *>();
        Tabs->setCurrentIndex(1);
        QTest::qWait(200);
        QVERIFY(Dialog->grab().save(Captures + '/' + Prefix + "-cfg.png"));
        Tabs->setCurrentIndex(0);
        Dialog->resize(760, 540);
        QTest::qWait(100);
        QVERIFY(Dialog->grab().save(Captures + '/' + Prefix + "-minimum.png"));
        auto *Settings = Dialog->findChild<QToolButton *>("comparisonSettings");
        Settings->setChecked(true);
        QTest::qWait(100);
        QVERIFY(Dialog->grab().save(Captures + '/' + Prefix + "-settings.png"));
        Settings->setChecked(false);
      }
    }
    const auto Before = Instructions->size();
    Dialog->resize(1400, 900);
    QTest::qWait(50);
    QVERIFY(Instructions->width() > Before.width() ||
            Instructions->height() > Before.height());
    Primary->setFocus();
    QTest::keyClick(Primary, Qt::Key_Tab);
    QVERIFY(QApplication::focusWidget() != Primary);
    Dialog->close();
    delete Dialog;
    QCOMPARE(Failure.count(), 0);
  }
  void cancellingInputLoadingKeepsThePreviousComparison() {
    const auto Large = Directory.filePath("loading-large.bin");
    QFile Sparse(Large);
    QVERIFY(Sparse.open(QIODevice::WriteOnly));
    QVERIFY(Sparse.resize(64 * 1024 * 1024));
    Sparse.close();
    const auto Small = Directory.filePath("loading-small.bin");
    QVERIFY(write(Small, QByteArray::fromHex("b807000000c3")));
    ComparisonDialog Dialog(TEST_COMPARISON_WORKER);
    Dialog.show();
    QTRY_VERIFY_WITH_TIMEOUT(Dialog.session().ready(), Timeout);
    QSignalSpy Failures(&Dialog.session(), &ComparisonSession::failed);
    auto *Primary = Dialog.findChild<QLineEdit *>("comparisonPrimary");
    auto *Secondary = Dialog.findChild<QLineEdit *>("comparisonSecondary");
    for (const char *Name : {"primaryLoader", "secondaryLoader"})
      Dialog.findChild<QComboBox *>(Name)->setCurrentIndex(1);
    auto *Start = Dialog.findChild<QPushButton *>("comparisonStart");
    auto *Cancel = Dialog.findChild<QPushButton *>("comparisonCancel");
    auto *Status = Dialog.findChild<QLabel *>("comparisonStatus");
    for (const bool Retain : {false, true}) {
      const auto Before = Dialog.session().summary();
      QCOMPARE(!Before.isEmpty(), Retain);
      Primary->setText(Large);
      Secondary->setText(Small);
      bool Clicked = false, StillRunning = false, Enabled = false;
      QString StageText;
      const auto Connection = connect(
          &Dialog.session(), &ComparisonSession::progress, &Dialog,
          [&](const auto &P) {
            if (Clicked || P["phase"] != QJsonValue("primary:load:hash") ||
                P["done"].toDouble() <= 0)
              return;
            Clicked = true;
            StageText = Status->text();
            Enabled = Cancel->isEnabled();
            QTest::mouseClick(Cancel, Qt::LeftButton);
            StillRunning = Dialog.session().busy();
          });
      QTest::mouseClick(Start, Qt::LeftButton);
      QTRY_VERIFY_WITH_TIMEOUT(Clicked && !Dialog.session().busy(), Timeout);
      disconnect(Connection);
      QVERIFY(Enabled);
      QVERIFY(StillRunning);
      QVERIFY(StageText.contains("Checking file identity"));
      QVERIFY(!StageText.contains("primary:load"));
      QCOMPARE(Dialog.session().summary(), Before);
      QVERIFY(Status->text().contains("Loading stopped"));
      QVERIFY(Status->text().contains(Retain ? "previous comparison"
                                             : "try again"));
      QVERIFY(Start->isEnabled());
      QVERIFY(!Cancel->isEnabled());
      QCOMPARE(Failures.count(), 0);
      const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
      if (Retain && !Captures.isEmpty()) {
        QDir().mkpath(Captures);
        QVERIFY(Dialog.grab().save(Captures + "/loading-stopped-retained.png"));
      }
      // The same UI can start again, clearing the stopped state on completion.
      Primary->setText(Small);
      QTest::mouseClick(Start, Qt::LeftButton);
      QTRY_VERIFY_WITH_TIMEOUT(!Dialog.session().busy(), Timeout);
      QCOMPARE(Dialog.session().summary()["state"].toString(),
               QString("complete"));
      QVERIFY(Dialog.session().operationMessage().isEmpty());
      QCOMPARE(Failures.count(), 0);
    }
    // A failed replacement has the same retention guarantee and its reason
    // remains visible after the request bookkeeping emits another update.
    const auto BeforeFailure = Dialog.session().summary();
    Primary->setText(Directory.filePath("missing-input.bin"));
    QTest::mouseClick(Start, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Dialog.session().busy(), Timeout);
    QCOMPARE(Failures.count(), 1);
    QCOMPARE(Dialog.session().summary(), BeforeFailure);
    QVERIFY(Status->text().contains("Could not replace"));
    QVERIFY(Status->text().contains("previous comparison"));
    Dialog.close();
  }

  void cancellingSavedRestoreKeepsSelectionEditsAndAllowsRetry() {
    const auto Input = Directory.filePath("restore-input.bin");
    QVERIFY(write(Input, QByteArray::fromHex("b80700000083c003c3")));
    const auto Saved = Directory.filePath("restore-review.nddiff");
    ComparisonDialog Dialog(TEST_COMPARISON_WORKER);
    auto &Session = Dialog.session();
    Dialog.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    for (const char *Name : {"comparisonPrimary", "comparisonSecondary"})
      Dialog.findChild<QLineEdit *>(Name)->setText(Input);
    for (const char *Name : {"primaryLoader", "secondaryLoader"})
      Dialog.findChild<QComboBox *>(Name)->setCurrentIndex(1);
    for (const char *Name : {"primaryBase", "secondaryBase"})
      Dialog.findChild<QLineEdit *>(Name)->setText("0x1000");
    auto *Start = Dialog.findChild<QPushButton *>("comparisonStart");
    auto *Cancel = Dialog.findChild<QPushButton *>("comparisonCancel");
    auto *Status = Dialog.findChild<QLabel *>("comparisonStatus");
    QTest::mouseClick(Start, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Failures.count(), 0);
    Session.command("comparison_save", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QFile Report(Saved);
    QVERIFY(Report.open(QIODevice::ReadOnly));
    const auto Original = Report.readAll();
    Report.close();
    QVERIFY(write(Saved, Original + QByteArray(64 * 1024 * 1024, ' ')));
    Session.command(
        "comparison_override",
        {{"kind", "confirm"}, {"primary", "0x1000"}, {"secondary", "0x1000"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    auto *Functions = Dialog.findChild<QTableView *>("comparisonFunctions");
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 1, Timeout);
    Functions->setCurrentIndex(Functions->model()->index(0, 0));
    auto *Instructions =
        Dialog.findChild<QTableView *>("comparisonInstructions");
    QTRY_VERIFY_WITH_TIMEOUT(Instructions->model()->rowCount() >= 3, Timeout);
    const auto Before = Session.summary();
    const auto Selection = Functions->currentIndex();
    QVERIFY(Before["can_undo"].toBool());
    int Ticks = 0;
    QTimer Heartbeat;
    connect(&Heartbeat, &QTimer::timeout, this, [&] { ++Ticks; });
    Heartbeat.start(1);
    const QJsonObject Binary{
        {"path", Input},
        {"load_options", QJsonObject{{"loader", "binary"},
                                     {"processor", "x86_64"},
                                     {"platform", "sysv"},
                                     {"base", "0x1000"}}}};
    for (const bool Replace : {false, true}) {
      bool Clicked = false, StillRunning = false;
      QString StageText;
      const auto Connection = connect(
          &Session, &ComparisonSession::progress, &Dialog, [&](const auto &P) {
            if (Clicked || P["phase"] != QJsonValue("restore:read"))
              return;
            StageText = Status->text();
            if (!Cancel->isEnabled())
              return;
            Clicked = true;
            QTest::mouseClick(Cancel, Qt::LeftButton);
            StillRunning = Session.busy();
          });
      if (Replace)
        Session.start(Binary, Binary, {}, Saved);
      else
        Session.command("comparison_restore", {{"path", Saved}});
      QTRY_VERIFY_WITH_TIMEOUT(Clicked && !Session.busy(), Timeout);
      disconnect(Connection);
      QVERIFY(StillRunning);
      QVERIFY(StageText.contains("Reading saved comparison"));
      QCOMPARE(Session.summary(), Before);
      QCOMPARE(Functions->currentIndex(), Selection);
      QVERIFY(Instructions->model()->rowCount() >= 3);
      QVERIFY(Status->text().contains("Restore stopped"));
      QVERIFY(Status->text().contains("previous comparison"));
      QVERIFY(Dialog.findChild<QPushButton *>("comparisonUndo")->isEnabled());
      QVERIFY(Start->isEnabled());
      QVERIFY(!Cancel->isEnabled());
      QCOMPARE(Failures.count(), 0);
    }
    QVERIFY(Ticks > 0);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QDir().mkpath(Captures);
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(100);
        QVERIFY(Dialog.grab().save(Captures +
                                   (Mode == Theme::Mode::Dark
                                        ? "/restore-stopped-dark.png"
                                        : "/restore-stopped-light.png")));
      }
    }
    // The prior edit is still undoable; a successful retry resets its history.
    Session.command("comparison_undo");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Session.summary()["can_redo"].toBool());
    QVERIFY(write(Saved, Original));
    Session.command("comparison_restore", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Failures.count(), 0);
    QVERIFY(Session.summary()["restore_completed"].toBool());
    QVERIFY(Session.operationMessage().isEmpty());
    QVERIFY(!Session.summary()["can_redo"].toBool());
    Dialog.close();
  }

  void cancellingMappingEditKeepsSelectionAndRedo() {
    const auto Input = Directory.filePath("edit-alignment.bin");
    const auto OtherInput = Directory.filePath("edit-alignment-other.bin");
    const auto Add = QByteArray::fromHex("83c001").repeated(3000);
    const auto Xor = QByteArray::fromHex("83f003").repeated(3000);
    QVERIFY(write(Input,
                  QByteArray::fromHex("b807000000") + Add + Xor + char(0xc3)));
    QVERIFY(write(OtherInput,
                  QByteArray::fromHex("b808000000") + Xor + Add + char(0xc3)));
    ComparisonDialog Dialog(TEST_COMPARISON_WORKER);
    auto &Session = Dialog.session();
    Dialog.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    const QJsonObject Binary{
        {"path", Input},
        {"load_options", QJsonObject{{"loader", "binary"},
                                     {"processor", "x86_64"},
                                     {"platform", "sysv"},
                                     {"base", "0x1000"}}}};
    auto Secondary = Binary;
    Secondary["path"] = OtherInput;
    Session.start(Binary, Secondary, {{"discover", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Failures.count(), 0);
    Session.command(
        "comparison_override",
        {{"kind", "unmatch"}, {"primary", "0x1000"}, {"secondary", "0x1000"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    const QJsonObject Decision{
        {"kind", "match"}, {"primary", "0x1000"}, {"secondary", "0x1000"}};
    Session.command("comparison_override", Decision);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    Session.command("comparison_undo");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Session.summary()["can_redo"].toBool());
    auto *Functions = Dialog.findChild<QTableView *>("comparisonFunctions");
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 1, Timeout);
    Functions->setCurrentIndex(Functions->model()->index(0, 0));
    auto *Instructions =
        Dialog.findChild<QTableView *>("comparisonInstructions");
    QTRY_VERIFY_WITH_TIMEOUT(Instructions->model()->rowCount() > 0, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    const auto Before = Session.summary();
    const auto Selection = Functions->currentIndex();
    auto *Cancel = Dialog.findChild<QPushButton *>("comparisonCancel");
    auto *Status = Dialog.findChild<QLabel *>("comparisonStatus");
    int Ticks = 0;
    QTimer Heartbeat;
    connect(&Heartbeat, &QTimer::timeout, this, [&] { ++Ticks; });
    Heartbeat.start(1);
    bool Clicked = false, StillRunning = false;
    const auto Connection = connect(
        &Session, &ComparisonSession::progress, &Dialog, [&](const auto &P) {
          if (Clicked || P["phase"] != QJsonValue("edit:measure"))
            return;
          QVERIFY(Status->text().contains("Updating affected functions"));
          QVERIFY(Cancel->isEnabled());
          Clicked = true;
          QTest::mouseClick(Cancel, Qt::LeftButton);
          StillRunning = Session.busy();
        });
    Session.command("comparison_override", Decision);
    QTRY_VERIFY_WITH_TIMEOUT(Clicked && !Session.busy(), Timeout);
    disconnect(Connection);
    QVERIFY(StillRunning);
    QVERIFY(Ticks > 0);
    QCOMPARE(Failures.count(), 0);
    QCOMPARE(Session.summary(), Before);
    QCOMPARE(Functions->currentIndex(), Selection);
    QVERIFY(Instructions->model()->rowCount() > 0);
    QVERIFY(Status->text().contains("Edit stopped"));
    QVERIFY(Status->text().contains("edit history"));
    QVERIFY(Dialog.findChild<QPushButton *>("comparisonRedo")->isEnabled());
    QVERIFY(!Cancel->isEnabled());
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QDir().mkpath(Captures);
      QVERIFY(Dialog.grab().save(Captures + "/edit-stopped.png"));
    }
    Session.command("comparison_redo");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Session.summary()["can_undo"].toBool());
    QVERIFY(Session.operationMessage().isEmpty());
    Session.command("comparison_override", Decision);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Failures.count(), 0);
    QVERIFY(Session.summary()["edit_completed"].toBool());
    QVERIFY(Session.revision() != Before["comparison_revision"].toString());
    Dialog.close();
  }

  void cancelButtonWaitsForActualStop() {
    const auto Path = Directory.filePath("large.bin");
    QVERIFY(write(Path, QByteArray(8 * 1024 * 1024, char(0x90)) + char(0xc3)));
    auto *Dialog = new ComparisonDialog(TEST_COMPARISON_WORKER);
    Dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    Dialog->show();
    QTRY_VERIFY_WITH_TIMEOUT(Dialog->session().ready(), Timeout);
    Dialog->findChild<QLineEdit *>("comparisonPrimary")->setText(Path);
    Dialog->findChild<QLineEdit *>("comparisonSecondary")->setText(Path);
    for (const char *Name : {"primaryLoader", "secondaryLoader"})
      Dialog->findChild<QComboBox *>(Name)->setCurrentIndex(1);
    auto *Cancel = Dialog->findChild<QPushButton *>("comparisonCancel");
    bool Clicked = false, Enabled = false, StillRunning = false;
    connect(&Dialog->session(), &ComparisonSession::progress, Dialog,
            [&](const auto &P) {
              if (Clicked || P["phase"] != QJsonValue("primary:discovery"))
                return;
              Clicked = true;
              Enabled = Cancel->isEnabled();
              QTest::mouseClick(Cancel, Qt::LeftButton);
              StillRunning = Dialog->session().busy();
            });
    QTest::mouseClick(Dialog->findChild<QPushButton *>("comparisonStart"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(Clicked && !Dialog->session().busy(), Timeout);
    QVERIFY(Enabled);
    QVERIFY(StillRunning);
    QCOMPARE(Dialog->session().summary()["state"].toString(),
             QString("cancelled"));
    const auto Job = Dialog->session().summary()["job"].toObject();
    QVERIFY(Job["computation_stopped"].toBool());
    QVERIFY(Job["partial_result_retained"].toBool());
    QVERIFY(Dialog->findChild<QLabel *>("comparisonStatus")
                ->text()
                .contains("cancelled"));
    Dialog->close();
    delete Dialog;
  }

  void callRegionSettingsAndEvidenceSurviveEditsAndReopen() {
    const auto Path = Directory.filePath("call-regions.bin");
    auto Code =
        QByteArray::fromHex("89f883c00383f017e8130000006bc00783c00983f025c3");
    Code.append(QByteArray(32 - Code.size(), char(0x90)));
    Code.append(QByteArray::fromHex("89f883c00bc3"));
    QVERIFY(write(Path, Code));
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    View.findChild<QLineEdit *>("comparisonPrimary")->setText(Path);
    View.findChild<QLineEdit *>("comparisonSecondary")->setText(Path);
    for (const auto *Name : {"primaryLoader", "secondaryLoader"})
      View.findChild<QComboBox *>(Name)->setCurrentIndex(1);
    View.findChild<QLineEdit *>("primaryBase")->setText("0x1000");
    View.findChild<QLineEdit *>("secondaryBase")->setText("0x8000");
    auto *Calls = View.findChild<QCheckBox *>("comparisonRegionCalls");
    auto *Regions = View.findChild<QCheckBox *>("comparisonAnalyzeRegions");
    QVERIFY(Calls && Regions);
    View.findChild<QToolButton *>("comparisonSettings")->setChecked(true);
    Calls->setFocus();
    QTest::keyClick(Calls, Qt::Key_Space);
    QVERIFY(Regions->isChecked());
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonStart"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    QCOMPARE(Session.summary()["regions"].toObject()["profile"].toString(),
             QString("native-call-runs-v1"));
    QCOMPARE(Session.summary()["regions"].toObject()["anchor_count"].toInt(),
             1);
    View.findChild<QPushButton *>("comparisonRegionReview")
        ->menu()
        ->actions()[0]
        ->trigger();
    auto *Dialog =
        View.findChild<ComparisonRegionsDialog *>("comparisonRegions");
    QVERIFY(Dialog);
    auto *Instructions = Dialog->findChild<QTableView *>("regionInstructions");
    QTRY_COMPARE_WITH_TIMEOUT(Instructions->model()->rowCount(), 7, Timeout);
    QVERIFY(Dialog->findChild<QLabel *>("regionSummary")
                ->text()
                .contains("function pairs"));
    const auto PreviousMode = Theme::instance().mode();
    const auto RestoreTheme =
        qScopeGuard([&] { Theme::instance().setMode(PreviousMode); });
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty())
      for (const auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(40);
        QVERIFY(
            Dialog->grab().save(Captures + (Mode == Theme::Mode::Dark
                                                ? "/dark-call-regions.png"
                                                : "/light-call-regions.png")));
        QVERIFY(View.grab().save(Captures +
                                 (Mode == Theme::Mode::Dark
                                      ? "/dark-call-region-settings.png"
                                      : "/light-call-region-settings.png")));
      }
    auto *Context = Dialog->findChild<QPushButton *>("regionCallContext");
    QVERIFY(Context && Context->isEnabled());
    Context->menu()->actions()[1]->trigger();
    auto *Impact =
        View.findChild<ComparisonImpactDialog *>("comparisonImpactDialog");
    QVERIFY(Impact);
    QCOMPARE(Impact->findChild<QLineEdit *>("impactSeed")->text(),
             QString("0x8000"));
    QCOMPARE(
        Impact->findChild<QComboBox *>("impactSide")->currentData().toString(),
        QString("secondary"));
    QTRY_VERIFY_WITH_TIMEOUT(Impact->findChild<QTableView *>("impactFunctions")
                                     ->model()
                                     ->rowCount() > 0,
                             Timeout);
    Impact->hide();
    const auto Input = [&](const char *Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"base", Base}}}};
    };
    // The full FFmpeg workflow needs larger allowances than the defaults.
    // Retain both custom capacity and quality settings through the ordinary
    // desktop reopen and Compare actions, without exposing test-only controls.
    Session.start(Input("0x1000"), Input("0x8000"),
                  {{"regions", QJsonObject{{"calls", true},
                                           {"max_seeds", 4194304},
                                           {"max_work", 268435456},
                                           {"max_anchors", 1000000},
                                           {"min_instructions", 5},
                                           {"min_repeated_instructions", 9}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY2(Failures.isEmpty(),
             qPrintable(Failures.isEmpty()
                            ? QString{}
                            : Failures.last().first().toString()));
    QTRY_COMPARE_WITH_TIMEOUT(Instructions->model()->rowCount(), 7, Timeout);
    auto SavedRegions =
        Session.summary()["settings"].toObject()["regions"].toObject();
    QCOMPARE(SavedRegions["max_seeds"].toInt(), 4194304);
    QCOMPARE(SavedRegions["min_instructions"].toInt(), 5);
    const auto Saved = Directory.filePath("call-regions.nddiff");
    Session.command("comparison_save", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && QFile::exists(Saved), Timeout);
    Session.command(
        "comparison_override",
        {{"kind", "unmatch"}, {"primary", "0x1020"}, {"secondary", "0x8020"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Instructions->model()->rowCount(), 0, Timeout);
    QVERIFY(!Context->isEnabled());
    Session.command("comparison_undo");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Instructions->model()->rowCount(), 7, Timeout);
    Dialog->hide();
    Regions->setChecked(false);
    QVERIFY(!Calls->isChecked());
    bool Selected = false;
    QTimer Select;
    connect(&Select, &QTimer::timeout, this, [&] {
      if (auto *File =
              qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
        File->selectFile(Saved);
        if (auto *Name = File->findChild<QLineEdit *>("fileNameEdit"))
          Name->setText(Saved);
        Selected = true;
        static_cast<QDialog *>(File)->accept();
      }
    });
    Select.start(10);
    QTest::mouseClick(View.findChild<QPushButton *>("comparisonReopen"),
                      Qt::LeftButton);
    Select.stop();
    QVERIFY(Selected);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Calls->isChecked(), Timeout);
    QVERIFY(Regions->isChecked());
    auto *Compare = View.findChild<QPushButton *>("comparisonStart");
    QTest::mouseClick(Compare, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Session.summary()["settings"].toObject()["regions"].toObject(),
             SavedRegions);
    Calls->setChecked(false);
    QTest::mouseClick(Compare, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    SavedRegions.remove("calls");
    QCOMPARE(Session.summary()["settings"].toObject()["regions"].toObject(),
             SavedRegions);
    QCOMPARE(Failures.size(), 0);
  }

  void regionSelectionResolvesAfterAnUnloadedPageArrives() {
    constexpr int Count = ::neverd::limits::gui::kComparisonPageSize * 2 + 17;
    QByteArray Code;
    for (int I = 0; I != Count; ++I) {
      // Five distinct immediate-bearing operations per function, followed by
      // return/padding. Every four-operation seed identifies just this body.
      for (const auto *Opcode : {"b8", "05", "35", "69c0", "05"}) {
        Code.append(QByteArray::fromHex(Opcode));
        for (unsigned Byte = 0; Byte != 4; ++Byte)
          Code.append(char((unsigned(I + 17) >> (Byte * 8)) & 0xff));
      }
      Code.append(char(0xc3));
      Code.append(QByteArray(5, char(0xcc)));
    }
    const auto Path = Directory.filePath("paged-regions.bin");
    QVERIFY(write(Path, Code));
    ComparisonSession Session(TEST_COMPARISON_WORKER);
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [&](const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"base", Base}}}};
    };
    Session.start(Input("0x1000"), Input("0x8000"), {{"regions", true}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Failures.size(), 0);
    ComparisonRegionsDialog Dialog(Session, nullptr);
    Dialog.setAttribute(Qt::WA_DeleteOnClose, false);
    Dialog.show();
    auto *Hypotheses = Dialog.findChild<QTableView *>("regionHypotheses");
    auto *Spans = Dialog.findChild<QTableView *>("regionAnchors");
    auto *Instructions = Dialog.findChild<QTableView *>("regionInstructions");
    auto *Groups = qobject_cast<ComparisonModel *>(Hypotheses->model());
    auto *Anchors = qobject_cast<ComparisonModel *>(Spans->model());
    auto *Rows = qobject_cast<ComparisonModel *>(Instructions->model());
    QTRY_COMPARE_WITH_TIMEOUT(Groups->rowCount(), Count, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    const auto FirstAddress =
        Rows->row(0)["primary"].toObject()["address"].toString();
    const int Last = Count - 1;
    QVERIFY(Groups->row(Last).isEmpty());
    Hypotheses->setCurrentIndex(Groups->index(Last, 0));
    QTRY_VERIFY_WITH_TIMEOUT(!Groups->row(Last).isEmpty(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 5, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(
        !Rows->row(0).isEmpty() &&
            Rows->row(0)["primary"].toObject()["address"].toString() !=
                FirstAddress,
        Timeout);
    const auto SelectedAddress =
        Rows->row(0)["primary"].toObject()["address"].toString();
    QCOMPARE(Anchors->row(0)["primary"].toObject()["function"].toString(),
             SelectedAddress);
    QVERIFY(Groups->cachedPages() >= 2);

    auto *Kind = Dialog.findChild<QComboBox *>("regionKind");
    Kind->setCurrentIndex(Kind->findData("raw_anchors"));
    QTRY_COMPARE_WITH_TIMEOUT(Anchors->rowCount(), Count, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    const auto FirstRawAddress =
        Rows->row(0)["primary"].toObject()["address"].toString();
    QVERIFY(Anchors->row(Last).isEmpty());
    Spans->setCurrentIndex(Anchors->index(Last, 0));
    QTRY_VERIFY_WITH_TIMEOUT(!Anchors->row(Last).isEmpty(), Timeout);
    const auto LastAddress =
        Anchors->row(Last)["primary"].toObject()["function"].toString();
    QVERIFY(LastAddress != FirstRawAddress);
    QTRY_COMPARE_WITH_TIMEOUT(
        Rows->row(0)["primary"].toObject()["address"].toString(), LastAddress,
        Timeout);
    QCOMPARE(Rows->rowCount(), 5);
    QVERIFY(Anchors->cachedPages() >= 2);
    auto *Context = Dialog.findChild<QPushButton *>("regionCallContext");
    QVERIFY(Context->isEnabled());
    QSignalSpy Inspect(&Dialog, &ComparisonRegionsDialog::inspectCallContext);
    Context->menu()->actions()[0]->trigger();
    QCOMPARE(Inspect.size(), 1);
    QCOMPARE(Inspect[0][1].toString(), LastAddress);
    QCOMPARE(Failures.size(), 0);
  }

  void retainedLargeCallRegionWorkflow() {
    const auto ReportPath =
        qEnvironmentVariable("NEVERD_DIFF_CALL_REGIONS_REPORT");
    const auto WorkflowPath =
        qEnvironmentVariable("NEVERD_DIFF_REGION_WORKFLOW");
    if (ReportPath.isEmpty() && WorkflowPath.isEmpty())
      QSKIP(
          "Set NEVERD_DIFF_CALL_REGIONS_REPORT and NEVERD_DIFF_REGION_WORKFLOW "
          "to a retained large call-region report and its native witnesses.");
    QFile ReportFile(ReportPath), WorkflowFile(WorkflowPath);
    QVERIFY(ReportFile.open(QIODevice::ReadOnly));
    QVERIFY(WorkflowFile.open(QIODevice::ReadOnly));
    const auto ReportBytes = ReportFile.readAll();
    const auto Report = QJsonDocument::fromJson(ReportBytes).object();
    const auto Workflow =
        QJsonDocument::fromJson(WorkflowFile.readAll()).object();
    QCOMPARE(Workflow["status"].toString(), QString("passed"));
    QCOMPARE(Workflow["report_sha256"].toString(),
             QString::fromLatin1(QCryptographicHash::hash(
                                     ReportBytes, QCryptographicHash::Sha256)
                                     .toHex()));
    QCOMPARE(Report["regions"].toObject()["profile"].toString(),
             QString("native-call-runs-v1"));
    const int AnchorCount =
        Report["regions"].toObject()["anchors"].toArray().size();
    QVERIFY(AnchorCount > 10000);
    QJsonObject Witness;
    int AnchorIndex = -1;
    for (const auto &Value : Workflow["members"].toArray()) {
      const auto Member = Value.toObject();
      const auto Site = Member["public_tail_transfer"].toObject();
      if (Site.isEmpty())
        continue;
      for (const auto &TargetValue : Member["members"].toArray()) {
        const auto Target = TargetValue.toObject();
        if (Target["address"] == Site["callee"].toObject()["address"]) {
          const auto Indices = Target["direct_anchor_indices"].toArray();
          if (!Indices.isEmpty()) {
            Witness = Site;
            AnchorIndex = Indices.first().toInt(-1);
          }
        }
      }
    }
    QVERIFY(!Witness.isEmpty());
    QVERIFY(AnchorIndex >= ::neverd::limits::gui::kComparisonPageSize);
    const auto Seed = Witness["callee"].toObject()["address"].toString();
    const auto Public = Witness["caller"].toObject()["address"].toString();
    // Restoring the complete large fixture includes decoding and native
    // binding. Keep this allowance local to the opt-in corpus test.
    constexpr int LargeFixtureTimeout = 120000;
    ComparisonDialog View(TEST_COMPARISON_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.show();
    auto &Session = View.session();
    QSignalSpy Failures(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.start({{"path", Report["primary"].toObject()["path"]}},
                  {{"path", Report["secondary"].toObject()["path"]}}, {},
                  ReportPath);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             LargeFixtureTimeout);
    QCOMPARE(Failures.size(), 0);
    View.findChild<QPushButton *>("comparisonRegionReview")
        ->menu()
        ->actions()[0]
        ->trigger();
    auto *Regions =
        View.findChild<ComparisonRegionsDialog *>("comparisonRegions");
    QVERIFY(Regions);
    auto *Kind = Regions->findChild<QComboBox *>("regionKind");
    Kind->setCurrentIndex(Kind->findData("raw_anchors"));
    QVERIFY(Kind->isVisible());
    QVERIFY(!Regions->findChild<QTableView *>("regionHypotheses")->isVisible());
    auto *Table = Regions->findChild<QTableView *>("regionAnchors");
    auto *Anchors = qobject_cast<ComparisonModel *>(Table->model());
    auto *Instructions = Regions->findChild<QTableView *>("regionInstructions");
    auto *Rows = qobject_cast<ComparisonModel *>(Instructions->model());
    QTRY_COMPARE_WITH_TIMEOUT(Anchors->rowCount(), AnchorCount, Timeout);
    QVERIFY(Anchors->row(AnchorIndex).isEmpty());
    Table->setCurrentIndex(Anchors->index(AnchorIndex, 0));
    Table->scrollTo(Table->currentIndex());
    QTRY_COMPARE_WITH_TIMEOUT(Anchors->row(AnchorIndex)["secondary"]
                                  .toObject()["function"]
                                  .toString(),
                              Seed, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QVERIFY(Anchors->cachedPages() >= 2);
    QVERIFY(Anchors->cachedPages() <=
            ::neverd::limits::gui::kComparisonCachedPages);
    QTRY_VERIFY_WITH_TIMEOUT(Table->viewport()->rect().contains(
                                 Table->visualRect(Table->currentIndex())),
                             Timeout);
    auto *Context = Regions->findChild<QPushButton *>("regionCallContext");
    QTRY_VERIFY_WITH_TIMEOUT(Context->isEnabled(), Timeout);
    Context->menu()->actions()[1]->trigger();
    auto *Impact =
        View.findChild<ComparisonImpactDialog *>("comparisonImpactDialog");
    QVERIFY(Impact);
    QCOMPARE(Impact->findChild<QLineEdit *>("impactSeed")->text(), Seed);
    QCOMPARE(
        Impact->findChild<QComboBox *>("impactSide")->currentData().toString(),
        QString("secondary"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Impact->findChild<QSpinBox *>("impactDepth")->setValue(1);
    Impact->findChild<QPushButton *>("impactTrace")->click();
    auto *Functions = Impact->findChild<QTableView *>("impactFunctions");
    auto *FunctionRows = qobject_cast<ComparisonModel *>(Functions->model());
    QTRY_VERIFY_WITH_TIMEOUT(FunctionRows->rowCount() > 1, Timeout);
    int PublicRow = -1;
    for (int I = 0; I != FunctionRows->rowCount(); ++I) {
      QTRY_VERIFY_WITH_TIMEOUT(!FunctionRows->row(I).isEmpty(), Timeout);
      if (FunctionRows->row(I)["address"].toString() == Public) {
        PublicRow = I;
        QCOMPARE(FunctionRows->row(I)["depth"].toInt(), 1);
      }
    }
    QVERIFY(PublicRow >= 0);
    Functions->setCurrentIndex(FunctionRows->index(PublicRow, 0));
    auto *Sites = Impact->findChild<QTableView *>("impactSites");
    auto *SiteRows = qobject_cast<ComparisonModel *>(Sites->model());
    QTRY_COMPARE_WITH_TIMEOUT(SiteRows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!SiteRows->row(0).isEmpty(), Timeout);
    for (const auto *Field : {"caller", "callee", "instruction"})
      QCOMPARE(SiteRows->row(0)[Field], Witness[Field]);
    const auto PreviousMode = Theme::instance().mode();
    const auto RestoreTheme =
        qScopeGuard([&] { Theme::instance().setMode(PreviousMode); });
    if (const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
        !Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      for (const auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(60);
        QVERIFY(Regions->grab().save(Captures + "/large-call-regions-" +
                                     Theme::modeName(Mode) + ".png"));
        QVERIFY(Impact->grab().save(Captures + "/large-call-context-" +
                                    Theme::modeName(Mode) + ".png"));
      }
    }
    QCOMPARE(Failures.size(), 0);
  }

  void regionReviewUsesBoundedPagesAndOriginalBytes() {
    const auto Path = Directory.filePath("region.bin");
    QVERIFY(write(Path, QByteArray::fromHex("89f883c00383f0176bc00783c009c3")));
    ComparisonSession Session(TEST_COMPARISON_WORKER);
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [&](const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"base", Base}}}};
    };
    Session.start(Input("0x1000"), Input("0x8000"), {{"regions", true}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Failure.empty());
    QCOMPARE(
        Session.summary()["regions"].toObject()["hypothesis_count"].toInt(), 1);
    auto *Dialog = new ComparisonRegionsDialog(Session, nullptr);
    Dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    Dialog->show();
    auto *Functions = Dialog->findChild<QTableView *>("regionHypotheses");
    auto *Spans = Dialog->findChild<QTableView *>("regionAnchors");
    auto *Instructions = Dialog->findChild<QTableView *>("regionInstructions");
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 1, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Spans->model()->rowCount(), 1, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Instructions->model()->rowCount(), 5, Timeout);
    auto *Rows = qobject_cast<ComparisonModel *>(Instructions->model());
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["primary"].toObject()["bytes"].toString(),
             QString("89F8"));
    QCOMPARE(Rows->row(0)["secondary"].toObject()["address"].toString(),
             QString("0x8000"));
    QVERIFY(Rows->cachedPages() <= 16);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty())
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(100);
        QVERIFY(Dialog->grab().save(Captures + (Mode == Theme::Mode::Dark
                                                    ? "/dark-regions.png"
                                                    : "/light-regions.png")));
      }
    auto *Kind = Dialog->findChild<QComboBox *>("regionKind");
    Kind->setCurrentIndex(Kind->findData("raw_anchors"));
    QTRY_COMPARE_WITH_TIMEOUT(Spans->model()->rowCount(), 1, Timeout);
    QVERIFY(Kind->isVisible());
    QVERIFY(!Functions->isVisible());
    Kind->setCurrentIndex(
        1); // This one-to-one fixture has no split hypothesis.
    QVERIFY(Functions->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 0, Timeout);
    QCOMPARE(Spans->model()->rowCount(), 0);
    QCOMPARE(Instructions->model()->rowCount(), 0);
    QVERIFY(write(
        Path,
        QByteArray::fromHex(
            "89f883c00383f0176bc00783c009eb0089f283c21383f25d6bd20b83c21bc3")));
    Session.start(
        Input("0x1000"), Input("0x8000"),
        {{"discover", false}, {"regions", QJsonObject{{"max_anchors", 1}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Session.summary()["state"].toString(),
             QString("resource_limited"));
    QTRY_COMPARE_WITH_TIMEOUT(Kind->currentData().toString(),
                              QString("raw_anchors"), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Spans->model()->rowCount(), 1, Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Instructions->model()->rowCount(), 5, Timeout);
    QVERIFY(!Functions->isEnabled());
    QVERIFY(!Functions->isVisible());
    Session.start(Input("0x1000"), Input("0x8000"), {{"regions", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Spans->model()->rowCount(), 0, Timeout);
    QCOMPARE(Instructions->model()->rowCount(), 0);
    QCOMPARE(Functions->model()->rowCount(), 0);
    QVERIFY(!Kind->isEnabled());
    Dialog->close();
    delete Dialog;
    QCOMPARE(Failure.count(), 0);
  }

  void ssaRegionReviewPagesNodePairsAndRetiresDisabledEvidence() {
    const auto Path = Directory.filePath("ssa-regions.bin");
    QVERIFY(write(Path, QByteArray::fromHex("89f883c00383f0176bc00783c009c3")));
    ComparisonSession Session(TEST_COMPARISON_WORKER);
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [&](const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"base", Base},
                                                      {"platform", "sysv"}}}};
    };
    Session.start(Input("0x1000"), Input("0x8000"), {{"flow_regions", true}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Failure.empty());
    auto *Dialog = new ComparisonRegionsDialog(Session, nullptr, true);
    Dialog->setAttribute(Qt::WA_DeleteOnClose, false);
    Dialog->show();
    auto *Hypotheses = Dialog->findChild<QTableView *>("regionHypotheses");
    auto *Anchors = Dialog->findChild<QTableView *>("regionAnchors");
    auto *Nodes = Dialog->findChild<QTableView *>("regionInstructions");
    auto *Rows = qobject_cast<ComparisonModel *>(Nodes->model());
    QTRY_VERIFY_WITH_TIMEOUT(Hypotheses->model()->rowCount() > 0, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Anchors->model()->rowCount() > 0, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() >= 4, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    int Original = 0;
    for (int I = 0; I < Rows->rowCount(); ++I) {
      const auto A = Rows->row(I)["primary"].toObject()["original"].toObject();
      if (!A.isEmpty()) {
        QVERIFY(!A["bytes"].toString().isEmpty());
        ++Original;
      }
    }
    QVERIFY(Original >= 2);
    QVERIFY(Rows->cachedPages() <= 16);
    QVERIFY(Dialog->findChild<QLabel *>("regionSummary")
                ->text()
                .contains("Dependency scope"));
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty())
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(100);
        QVERIFY(Dialog->grab().save(
            Captures + (Mode == Theme::Mode::Dark ? "/dark-ssa-regions.png"
                                                  : "/light-ssa-regions.png")));
      }
    auto *Kind = Dialog->findChild<QComboBox *>("regionKind");
    Session.start(Input("0x1000"), Input("0x8000"),
                  {{"flow_regions", QJsonObject{{"max_anchors", 1}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Kind->currentData().toString(),
                              QString("raw_anchors"), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Anchors->model()->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() >= 4, Timeout);
    Session.start(Input("0x1000"), Input("0x8000"), {{"flow_regions", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 0, Timeout);
    QCOMPARE(Anchors->model()->rowCount(), 0);
    QVERIFY(!Kind->isEnabled());
    Dialog->close();
    delete Dialog;
    QCOMPARE(Failure.count(), 0);
  }

  void stoppedFunctionDifferenceCanBeRetriedWithoutOldGraphClaims() {
    const auto Path = Directory.filePath("large-detail.bin");
    auto Code = QByteArray(60000, char(0x90));
    Code.append(char(0xc3));
    QVERIFY(write(Path, Code));
    auto *Dialog = new ComparisonDialog(TEST_COMPARISON_WORKER);
    Dialog->show();
    auto &Session = Dialog->session();
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [&](const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"base", Base},
                                                      {"platform", "sysv"}}}};
    };
    Session.start(Input("0x1000"), Input("0x8000"), {{"discover", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    auto *Table = Dialog->findChild<QTableView *>("comparisonInstructions");
    auto *Rows = qobject_cast<ComparisonModel *>(Table->model());
    auto *Retry = Dialog->findChild<QPushButton *>("comparisonRetryDetail");
    Rows->setQuery(
        {{"address", "0x1000"}, {"timeout_ms", 1}, {"refresh", true}});
    QTRY_VERIFY_WITH_TIMEOUT(Retry->isEnabled(), Timeout);
    QCOMPARE(Rows->rowCount(), 0);
    QVERIFY(Dialog->findChild<QLabel *>("comparisonStatus")
                ->text()
                .contains("Function difference stopped"));
    QVERIFY(Dialog->findChild<QLabel *>("comparisonFunctionTitle")
                ->text()
                .contains("timed out"));
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty())
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(100);
        QVERIFY(Dialog->grab().save(Captures +
                                    (Mode == Theme::Mode::Dark
                                         ? "/dark-detail-stopped.png"
                                         : "/light-detail-stopped.png")));
      }
    QTest::mouseClick(Retry, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), int(Code.size()), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Retry->isEnabled() && !Session.cancellable(),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["kind"].toString(), QString("identical"));
    QVERIFY(!Dialog->findChild<QLabel *>("comparisonFunctionTitle")
                 ->text()
                 .contains("timed out"));
    QCOMPARE(Failure.count(), 0);
    delete Dialog;
  }

  void changeReviewPreservesSyntheticAnchorsAndNavigatesEvidence_data() {
    layoutDirections();
  }

  void changeReviewPreservesSyntheticAnchorsAndNavigatesEvidence() {
    QFETCH(bool, RightToLeft);
    const auto OriginalDirection = QApplication::layoutDirection();
    const auto RestoreDirection = qScopeGuard(
        [&] { QApplication::setLayoutDirection(OriginalDirection); });
    QApplication::setLayoutDirection(RightToLeft ? Qt::RightToLeft
                                                 : Qt::LeftToRight);
    const auto A = Directory.filePath("change-review-a.bin"),
               B = Directory.filePath("change-review-b.bin");
    auto Bytes = QByteArray::fromHex("83ff117406b802000000c3b803000000c3");
    QVERIFY(write(A, Bytes));
    Bytes[2] = 9;
    QVERIFY(write(B, Bytes));
    auto *Parent = new ComparisonDialog(TEST_COMPARISON_WORKER);
    Parent->setAttribute(Qt::WA_DeleteOnClose, false);
    Parent->show();
    auto &Session = Parent->session();
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [](const QString &Path, const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"platform", "sysv"},
                                                      {"base", Base}}}};
    };
    Session.start(Input(A, "0x1000"), Input(B, "0x8000"), {{"dataflow", true}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    auto *Functions = Parent->findChild<QTableView *>("comparisonFunctions");
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 1, Timeout);
    Functions->setCurrentIndex(Functions->model()->index(0, 0));
    auto *Open = Parent->findChild<QPushButton *>("comparisonChangesReview");
    QTRY_VERIFY_WITH_TIMEOUT(Open->isEnabled(), Timeout);
    QTest::mouseClick(Open, Qt::LeftButton);
    auto *Dialog = Parent->findChild<ComparisonChangesDialog *>();
    QVERIFY(Dialog);
    auto *Table = Dialog->findChild<QTableView *>("changesTable");
    auto *Rows = qobject_cast<ComparisonModel *>(Table->model());
    QSignalSpy ReviewFailure(Rows, &ComparisonModel::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() || !ReviewFailure.empty(),
                             Timeout);
    QVERIFY2(ReviewFailure.empty(),
             qPrintable(Dialog->findChild<QLabel *>("changesSummary")->text()));
    QCOMPARE(Rows->rowCount(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["association"].toString(),
             QString("direct_synthetic_operand"));
    auto *Evidence = Dialog->findChild<QPlainTextEdit *>("changesEvidence");
    QTRY_VERIFY_WITH_TIMEOUT(
        Evidence->toPlainText().contains("Synthetic operation"), Timeout);
    QVERIFY(Evidence->toPlainText().contains("constant 0x11"));
    auto *Context = Dialog->findChild<QComboBox *>("changesContext");
    QTRY_VERIFY_WITH_TIMEOUT(Context->isEnabled(), Timeout);
    Context->setCurrentIndex(1);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    auto *Backward = Dialog->findChild<QPushButton *>("changesBackward");
    QTRY_VERIFY_WITH_TIMEOUT(Backward->isEnabled(), Timeout);
    QTest::mouseClick(Backward, Qt::LeftButton);
    auto *SliceTable = Dialog->findChild<QTableView *>("changesSlice");
    QTRY_VERIFY_WITH_TIMEOUT(SliceTable->model()->rowCount() > 0, Timeout);
    auto *Side = Dialog->findChild<QComboBox *>("changesSide");
    QTRY_VERIFY_WITH_TIMEOUT(Side->isEnabled(), Timeout);
    Side->setCurrentIndex(1);
    QSignalSpy Inspect(Dialog, &ComparisonChangesDialog::inspectFunction);
    QTest::mouseClick(Dialog->findChild<QPushButton *>("changesInspect"),
                      Qt::LeftButton);
    QCOMPARE(Inspect.count(), 1);
    QCOMPARE(Inspect[0][1].toString(), QString("0x8000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Dialog->raise();
    auto *Details = Dialog->findChild<QTabWidget *>("changesDetails");
    Details->setCurrentIndex(0);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QDir().mkpath(Captures);
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        Dialog->resize(1180, 820);
        QTest::qWait(100);
        const auto Prefix =
            Captures +
            (Mode == Theme::Mode::Dark ? "/dark-changes" : "/light-changes");
        QVERIFY(Dialog->grab().save(Prefix + ".png"));
        Dialog->resize(Dialog->minimumSize());
        QTest::qWait(50);
        QVERIFY(Dialog->grab().save(Prefix + "-minimum.png"));
      }
    }
    auto *Calls = Dialog->findChild<QPushButton *>("changesCalls");
    QTRY_VERIFY_WITH_TIMEOUT(Calls->isEnabled(), Timeout);
    QTest::mouseClick(Calls, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(Parent->findChild<ComparisonImpactDialog *>(),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Session.start(Input(A, "0x1000"), Input(B, "0x8000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 0, Timeout);
    QVERIFY(!Backward->isEnabled());
    Dialog->review("primary", "0x1000");
    // A context filter with no SSA evidence must not imply unchanged code.
    QTRY_VERIFY_WITH_TIMEOUT(Context->isEnabled(), Timeout);
    Context->setCurrentIndex(0);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["association"].toString(), QString("native_only"));
    QVERIFY(Dialog->findChild<QLabel *>("changesSummary")
                ->text()
                .contains("not requested"));
    QVERIFY(!Backward->isEnabled());
    QCOMPARE(Failure.count(), 0);
    delete Parent;
  }

  void analystHistoryKeepsDraftBindingAndSurvivesMappingChanges_data() {
    layoutDirections();
  }

  void analystHistoryKeepsDraftBindingAndSurvivesMappingChanges() {
    QFETCH(bool, RightToLeft);
    const auto OriginalDirection = QApplication::layoutDirection();
    const auto RestoreDirection = qScopeGuard(
        [&] { QApplication::setLayoutDirection(OriginalDirection); });
    QApplication::setLayoutDirection(RightToLeft ? Qt::RightToLeft
                                                 : Qt::LeftToRight);
    const auto A = Directory.filePath("review-a.bin"),
               B = Directory.filePath("review-b.bin");
    QVERIFY(write(A, QByteArray::fromHex("b807000000c3")));
    QVERIFY(write(B, QByteArray::fromHex("b809000000c3")));
    auto *Parent = new ComparisonDialog(TEST_COMPARISON_WORKER);
    Parent->setAttribute(Qt::WA_DeleteOnClose, false);
    Parent->show();
    auto &Session = Parent->session();
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [](const QString &Path, const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"platform", "sysv"},
                                                      {"base", Base}}}};
    };
    Session.start(Input(A, "0x1000"), Input(B, "0x8000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    auto *Functions = Parent->findChild<QTableView *>("comparisonFunctions");
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 1, Timeout);
    Functions->setCurrentIndex(Functions->model()->index(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT(
        Parent->findChild<QPushButton *>("comparisonChangesReview")
            ->isEnabled(),
        Timeout);
    QTest::mouseClick(
        Parent->findChild<QPushButton *>("comparisonReviewHistory"),
        Qt::LeftButton);
    auto *Dialog = Parent->findChild<ComparisonReviewDialog *>();
    QVERIFY(Dialog);
    auto *Rows = qobject_cast<ComparisonModel *>(
        Dialog->findChild<QTableView *>("reviewHistory")->model());
    auto *Case = Dialog->findChild<QLineEdit *>("reviewCase");
    auto *Author = Dialog->findChild<QLineEdit *>("reviewAuthor");
    auto *Note = Dialog->findChild<QPlainTextEdit *>("reviewNote");
    auto *Assessment = Dialog->findChild<QComboBox *>("reviewAssessment");
    auto *Append = Dialog->findChild<QPushButton *>("reviewAppend");
    Case->setText("owned-gui-case");
    Author->setText("GUI fixture analyst");
    Note->setPlainText(
        "Observed local change; assessment needs further investigation.");
    Assessment->setCurrentIndex(3);
    QTRY_VERIFY_WITH_TIMEOUT(Append->isEnabled(), Timeout);
    QTest::mouseClick(Append, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["source"].toString(), QString("analyst"));
    QVERIFY(Note->toPlainText().isEmpty());
    Note->setPlainText(
        "Manual patch-present fixture claim, without engine validation.");
    Assessment->setCurrentIndex(4);
    QTRY_VERIFY_WITH_TIMEOUT(Append->isEnabled(), Timeout);
    QTest::mouseClick(Append, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 2, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["assessment"].toString(), QString("patch_present"));
    QCOMPARE(Rows->row(0)["semantic_validation"].toString(),
             QString("not_performed"));
    auto *Latest = Dialog->findChild<QCheckBox *>("reviewLatest");
    Latest->setChecked(true);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    const auto Saved = Directory.filePath("analyst-review.nddiff");
    Session.command("comparison_save", {{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QFile File(Saved);
    QVERIFY(File.open(QIODevice::ReadOnly));
    const auto Document = QJsonDocument::fromJson(File.readAll()).object();
    QCOMPARE(Document["reviews"].toObject()["records"].toArray().size(), 2);
    Note->setPlainText("Draft retained across pairing and input changes.");
    Session.command(
        "comparison_override",
        {{"kind", "unmatch"}, {"primary", "0x1000"}, {"secondary", "0x8000"}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QVERIFY(!Rows->row(0)["pair_active"].toBool());
    QVERIFY(!Append->isEnabled());
    QVERIFY(Note->toPlainText().contains("Draft retained"));
    Session.command("comparison_undo");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Append->isEnabled(), Timeout);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QDir().mkpath(Captures);
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        Dialog->resize(1100, 800);
        QTest::qWait(100);
        const auto Prefix =
            Captures +
            (Mode == Theme::Mode::Dark ? "/dark-reviews" : "/light-reviews");
        QVERIFY(Dialog->grab().save(Prefix + ".png"));
        Dialog->resize(Dialog->minimumSize());
        QTest::qWait(50);
        QVERIFY(Dialog->grab().save(Prefix + "-minimum.png"));
        auto *Tabs = Dialog->findChild<QTabWidget *>("reviewTabs");
        Tabs->setCurrentIndex(1);
        QTest::qWait(50);
        QVERIFY(Dialog->grab().save(Prefix + "-record.png"));
        Tabs->setCurrentIndex(0);
      }
    }
    QVERIFY(write(B, QByteArray::fromHex("b80b000000c3")));
    Session.start(Input(A, "0x1000"), Input(B, "0x8000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 0, Timeout);
    QVERIFY(Note->toPlainText().contains("Draft retained"));
    QVERIFY(!Append->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(
        Dialog->findChild<QPushButton *>("reviewNewDraft")->isEnabled(),
        Timeout);
    QTest::mouseClick(Dialog->findChild<QPushButton *>("reviewNewDraft"),
                      Qt::LeftButton);
    QVERIFY(Note->toPlainText().isEmpty());
    QCOMPARE(Failure.count(), 0);
    delete Parent;
  }

  void callImpactPathsKeepWitnessesAndUnknownScope_data() {
    layoutDirections();
  }

  void callImpactPathsKeepWitnessesAndUnknownScope() {
    QFETCH(bool, RightToLeft);
    const auto OriginalDirection = QApplication::layoutDirection();
    const auto RestoreDirection = qScopeGuard(
        [&] { QApplication::setLayoutDirection(OriginalDirection); });
    QApplication::setLayoutDirection(RightToLeft ? Qt::RightToLeft
                                                 : Qt::LeftToRight);
    const auto Path = Directory.filePath("call-paths.bin");
    auto Bytes = QByteArray::fromHex(
        "e80b000000ffd0c3ccccccccccccccccb80700000083c003c3");
    QVERIFY(write(Path, Bytes));
    ComparisonSession Session(TEST_COMPARISON_WORKER);
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [&](const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"base", Base},
                                                      {"platform", "sysv"}}}};
    };
    Session.start(Input("0x1000"), Input("0x8000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Failure.empty());
    auto *Dialog = new ComparisonImpactDialog(Session, nullptr);
    Dialog->show();
    Dialog->trace("primary", "0x1010");
    auto *Functions = Dialog->findChild<QTableView *>("impactFunctions");
    auto *Sites = Dialog->findChild<QTableView *>("impactSites");
    auto *Rows = qobject_cast<ComparisonModel *>(Functions->model());
    auto *SiteRows = qobject_cast<ComparisonModel *>(Sites->model());
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 2, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(1).isEmpty(), Timeout);
    Functions->setCurrentIndex(Functions->model()->index(1, 0));
    QTRY_COMPARE_WITH_TIMEOUT(SiteRows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!SiteRows->row(0).isEmpty(), Timeout);
    QCOMPARE(SiteRows->row(0)["instruction"].toObject()["bytes"].toString(),
             QString("E80B000000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    QSignalSpy Inspect(Dialog, &ComparisonImpactDialog::inspectFunction);
    QTest::mouseClick(Dialog->findChild<QPushButton *>("impactInspect"),
                      Qt::LeftButton);
    QCOMPARE(Inspect.count(), 1);
    QCOMPARE(Inspect[0][1].toString(), QString("0x1000"));
    auto *FunctionKind = Dialog->findChild<QComboBox *>("impactFunctionKind");
    FunctionKind->setCurrentIndex(1);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["address"].toString(), QString("0x1000"));
    FunctionKind->setCurrentIndex(0);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 2, Timeout);
    auto *Kind = Dialog->findChild<QComboBox *>("impactSiteKind");
    Kind->setCurrentIndex(2);
    QTRY_COMPARE_WITH_TIMEOUT(SiteRows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!SiteRows->row(0).isEmpty(), Timeout);
    QCOMPARE(SiteRows->row(0)["instruction"].toObject()["address"].toString(),
             QString("0x1005"));
    QVERIFY(Dialog->findChild<QLabel *>("impactSiteStatus")
                ->text()
                .contains("Global"));
    Kind->setCurrentIndex(Kind->findData("effects"));
    QTRY_COMPARE_WITH_TIMEOUT(SiteRows->rowCount(), 2, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!SiteRows->row(1).isEmpty(), Timeout);
    QCOMPARE(SiteRows->row(0)["effect"].toObject()["status"].toString(),
             QString("internal_target"));
    QCOMPARE(SiteRows->row(1)["effect"].toObject()["status"].toString(),
             QString("unresolved_target"));
    Sites->setCurrentIndex(Sites->model()->index(0, 0));
    QVERIFY(Dialog->findChild<QPlainTextEdit *>("impactSiteDetails")
                ->toPlainText()
                .contains("Internal callee"));
    Kind->setCurrentIndex(Kind->findData("boundaries"));
    QTRY_COMPARE_WITH_TIMEOUT(SiteRows->rowCount(), 1, Timeout);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty())
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(100);
        const auto Prefix =
            Captures +
            (Mode == Theme::Mode::Dark ? "/dark-impact" : "/light-impact");
        QVERIFY(Dialog->grab().save(Prefix + ".png"));
        const auto Size = Dialog->size();
        Dialog->resize(Dialog->minimumSize());
        QTest::qWait(50);
        QVERIFY(Dialog->grab().save(Prefix + "-minimum.png"));
        Dialog->resize(Size);
      }
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Dialog->findChild<QSpinBox *>("impactDepth")->setValue(0);
    QCOMPARE(Rows->rowCount(), 0);
    QTest::mouseClick(Dialog->findChild<QPushButton *>("impactTrace"),
                      Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    QVERIFY(Dialog->findChild<QLabel *>("impactSummary")
                ->text()
                .contains("Analysis limit reached"));
    Kind->setCurrentIndex(3);
    QTRY_COMPARE_WITH_TIMEOUT(SiteRows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Dialog->trace("secondary", "0x8010");
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 1, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QCOMPARE(Rows->row(0)["address"].toString(), QString("0x8010"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    int Replies = 0;
    const QJsonObject Query{{"seeds", QJsonArray{"0x1010"}}};
    for (int I = 0; I < 2; ++I)
      Session.request("comparison_impact", Query, Dialog,
                      [&](const auto &) { ++Replies; });
    QVERIFY(Session.cancellable());
    Session.cancel();
    QTRY_COMPARE_WITH_TIMEOUT(Replies, 2, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Session.start(Input("0x1000"), Input("0x8000"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Rows->rowCount(), 0, Timeout);
    QCOMPARE(SiteRows->rowCount(), 0);
    QVERIFY(!Dialog->findChild<QPushButton *>("impactInspect")->isEnabled());
    delete Dialog;
    QCOMPARE(Failure.count(), 0);
  }

  void importContractsShowAssumptionsRolesAndMissingSSA() {
#ifndef TEST_COMPARISON_EFFECT_FIXTURE
    QSKIP("Owned ELF import fixture requires a Linux x64 C compiler");
#else
    ComparisonSession Session(TEST_COMPARISON_WORKER);
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const QJsonObject Input{{"path", TEST_COMPARISON_EFFECT_FIXTURE}};
    Session.start(Input, Input, {{"dataflow", true}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Failure.empty());
    auto *Dialog = new ComparisonImpactDialog(Session, nullptr);
    QJsonObject InventoryReply;
    Session.request(
        "comparison_functions", {{"search", "effect_entry"}}, Dialog,
        [&](const QJsonObject &Message) { InventoryReply = Message; });
    QTRY_VERIFY_WITH_TIMEOUT(!InventoryReply.isEmpty(), Timeout);
    QCOMPARE(InventoryReply["status"].toString(), QString("ok"));
    const auto Items = InventoryReply["payload"].toObject()["items"].toArray();
    QCOMPARE(Items.size(), 1);
    const auto Entry = Items.first().toObject()["address"].toString();
    QVERIFY(!Entry.isEmpty());
    Dialog->show();
    Dialog->findChild<QComboBox *>("impactDirection")->setCurrentIndex(1);
    Dialog->trace("primary", Entry);
    auto *Functions = Dialog->findChild<QTableView *>("impactFunctions");
    QTRY_VERIFY_WITH_TIMEOUT(Functions->model()->rowCount() > 0, Timeout);
    auto *Kind = Dialog->findChild<QComboBox *>("impactSiteKind");
    Kind->setCurrentIndex(Kind->findData("effects"));
    auto *Sites = Dialog->findChild<QTableView *>("impactSites");
    auto *Rows = qobject_cast<ComparisonModel *>(Sites->model());
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() > 0 && !Rows->row(0).isEmpty(),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->row(0).contains("effect"), Timeout);
    int Copy = -1;
    for (int I = 0; I < Rows->rowCount(); ++I)
      if (Rows->row(I)["effect"].toObject()["family"].toString() == "copy")
        Copy = I;
    QVERIFY2(
        Copy >= 0,
        qPrintable(Dialog->findChild<QLabel *>("impactSiteStatus")->text()));
    Sites->selectRow(Copy);
    auto *Details = Dialog->findChild<QPlainTextEdit *>("impactSiteDetails");
    QVERIFY(Details->toPlainText().contains("Qualified copy contract"));
    QVERIFY(Details->toPlainText().contains("Length: argument 2"));
    QVERIFY(Details->toPlainText().contains("Assumption:"));
    QVERIFY(Details->toPlainText().contains("memory alias"));
    QVERIFY(Dialog->findChild<QLabel *>("impactSiteStatus")
                ->text()
                .contains("2 qualified"));
    auto *Tabs = Dialog->findChild<QTabWidget *>("impactSiteTabs");
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty())
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        Tabs->setCurrentIndex(0);
        QTest::qWait(100);
        const auto Prefix =
            Captures +
            (Mode == Theme::Mode::Dark ? "/dark-effects" : "/light-effects");
        QVERIFY(Dialog->grab().save(Prefix + ".png"));
        const auto Size = Dialog->size();
        Dialog->resize(Dialog->minimumSize());
        QTest::qWait(50);
        QVERIFY(Dialog->grab().save(Prefix + "-minimum.png"));
        Tabs->setCurrentIndex(1);
        QTest::qWait(50);
        QVERIFY(Dialog->grab().save(Prefix + "-contract.png"));
        Dialog->resize(Size);
      }
    Dialog->findChild<QComboBox *>("impactSide")->setCurrentIndex(1);
    QCOMPARE(Rows->rowCount(), 0);
    QVERIFY(Details->toPlainText().isEmpty());
    Dialog->trace("secondary", Entry);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() > 0 && !Rows->row(0).isEmpty(),
                             Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    Session.start(Input, Input);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Rows->rowCount(), 0);
    QVERIFY(Details->toPlainText().isEmpty());
    Dialog->trace("primary", Entry);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() > 0 && !Rows->row(0).isEmpty(),
                             Timeout);
    QCOMPARE(Rows->row(0)["effect"].toObject()["status"].toString(),
             QString("ssa_unavailable"));
    QVERIFY(Dialog->findChild<QLabel *>("impactSiteStatus")
                ->text()
                .contains("0 qualified"));
    QCOMPARE(Failure.count(), 0);
    delete Dialog;
#endif
  }

  void dependencyReviewSlicesNativeOriginsAndClearsOldEvidence_data() {
    layoutDirections();
  }

  void dependencyReviewSlicesNativeOriginsAndClearsOldEvidence() {
    QFETCH(bool, RightToLeft);
    const auto OriginalDirection = QApplication::layoutDirection();
    const auto RestoreDirection = qScopeGuard(
        [&] { QApplication::setLayoutDirection(OriginalDirection); });
    QApplication::setLayoutDirection(RightToLeft ? Qt::RightToLeft
                                                 : Qt::LeftToRight);
    const auto Path = Directory.filePath("dependencies.bin");
    // Arithmetic result becomes a recovered argument of a local helper.
    QVERIFY(write(Path, QByteArray::fromHex(
                            "89f883c00383f0176bc00783c0095089c7e80600000059c3"
                            "cccccccc8d4705c3")));
    ComparisonSession Session(TEST_COMPARISON_WORKER);
    QSignalSpy Failure(&Session, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Input = [&](const QString &Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"base", Base},
                                                      {"platform", "sysv"}}}};
    };
    Session.start(Input("0x1000"), Input("0x8000"), {{"dataflow", true}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Failure.empty());
    auto *Dialog = new ComparisonFlowDialog(Session, nullptr);
    Dialog->show();
    auto *Functions = Dialog->findChild<QTableView *>("flowFunctions");
    auto *Nodes = Dialog->findChild<QTableView *>("flowNodes");
    auto *Slice = Dialog->findChild<QTableView *>("flowSlice");
    auto *Rows = qobject_cast<ComparisonModel *>(Nodes->model());
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 2, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() > 0, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    int Mul = -1;
    int Call = -1;
    for (int I = 0; I < Rows->rowCount(); ++I) {
      if (Rows->row(I)["opcode"] == QJsonValue("INT_MULT"))
        Mul = I;
      if (Rows->row(I)["opcode"] == QJsonValue("CALL"))
        Call = I;
    }
    QVERIFY(Mul >= 0);
    QVERIFY(Call >= 0);
    QCOMPARE(Rows->row(Call)["call_argument_count"].toInt(), 1);
    QVERIFY(Rows->data(Rows->index(Call, 4), Qt::DisplayRole)
                .toString()
                .contains("arg 1:"));
    QCOMPARE(Rows->row(Mul)["address"].toString(), QString("0x1008"));
    Nodes->setCurrentIndex(Rows->index(Call, 0));
    auto *Trace = Dialog->findChild<QPushButton *>("flowBackward");
    QTRY_VERIFY(Trace->isEnabled());
    QTest::mouseClick(Trace, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(Slice->model()->rowCount() > 0, Timeout);
    auto *SliceRows = qobject_cast<ComparisonModel *>(Slice->model());
    QTRY_VERIFY_WITH_TIMEOUT(!SliceRows->row(0).isEmpty(), Timeout);
    bool ContainsSetup = false;
    for (int I = 0; I < SliceRows->rowCount(); ++I) {
      ContainsSetup |= SliceRows->row(I)["opcode"] == QJsonValue("INT_MULT");
      if (SliceRows->row(I)["opcode"] == QJsonValue("CALL"))
        QVERIFY(SliceRows->data(SliceRows->index(I, 4), Qt::DisplayRole)
                    .toString()
                    .contains("arg 1:"));
    }
    QVERIFY(ContainsSetup);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    QVERIFY(Dialog->findChild<QLabel *>("flowSliceStatus")
                ->text()
                .contains("complete"));
    int Replies = 0;
    const QJsonObject Query{{"address", "0x1000"}, {"seeds", QJsonArray{0}}};
    Session.request("comparison_slice", Query, Dialog,
                    [&](const auto &) { ++Replies; });
    Session.request("comparison_slice", Query, Dialog,
                    [&](const auto &) { ++Replies; });
    QVERIFY(Session.cancellable());
    Session.cancel();
    QTRY_COMPARE_WITH_TIMEOUT(Replies, 2, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.cancellable(), Timeout);
    const auto Captures = qEnvironmentVariable("NEVERD_DIFF_SCREENSHOTS");
    if (!Captures.isEmpty())
      for (const auto Mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(100);
        QVERIFY(Dialog->grab().save(Captures + (Mode == Theme::Mode::Dark
                                                    ? "/dark-flow.png"
                                                    : "/light-flow.png")));
      }
    Dialog->findChild<QComboBox *>("flowSide")->setCurrentIndex(1);
    QTRY_VERIFY_WITH_TIMEOUT(Rows->rowCount() > 0, Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(!Rows->row(0).isEmpty(), Timeout);
    QTRY_VERIFY_WITH_TIMEOUT(Dialog->findChild<QLabel *>("flowFunctionStatus")
                                 ->text()
                                 .contains("0x8000"),
                             Timeout);
    QCOMPARE(Slice->model()->rowCount(), 0);
    Session.start(Input("0x1000"), Input("0x8000"), {{"dataflow", false}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QTRY_COMPARE_WITH_TIMEOUT(Functions->model()->rowCount(), 0, Timeout);
    QCOMPARE(Nodes->model()->rowCount(), 0);
    QCOMPARE(Slice->model()->rowCount(), 0);
    QVERIFY(!Trace->isEnabled());
    Dialog->close();
    delete Dialog;
    QCOMPARE(Failure.count(), 0);
  }
};

QTEST_MAIN(ComparisonTests)
#include "ComparisonTests.moc"
