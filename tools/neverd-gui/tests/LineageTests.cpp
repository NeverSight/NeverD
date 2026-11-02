//===- LineageTests.cpp - Offline version graph interaction ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Real worker evidence, keyboard navigation, failed replacement and captures.
/// Authored machine code is analyzed, never executed.
//===----------------------------------------------------------------------===//

#include "ComparisonSession.h"
#include "Language.h"
#include "LineageDialog.h"
#include "LineageGraph.h"
#include "LineagePageView.h"
#include "Theme.h"
#include "WindowFrame.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QGraphicsItem>
#include <QGraphicsTextItem>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTranslator>

using namespace neverd::gui;
namespace {
constexpr int Timeout = 30000;
bool privateShell(const QString &Script) {
  QProcess P;
  P.start("gdbus",
          {"call", "--session", "--dest", "org.gnome.Shell", "--object-path",
           "/org/gnome/Shell", "--method", "org.gnome.Shell.Eval", Script});
  return P.waitForFinished() && P.exitCode() == 0 &&
         P.readAllStandardOutput().startsWith("(true,");
}
bool write(const QString &Path, const QByteArray &Bytes) {
  QFile F(Path);
  return F.open(QIODevice::WriteOnly) && F.write(Bytes) == Bytes.size();
}
QByteArray read(const QString &Path) {
  QFile F(Path);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray{};
}
} // namespace

class LineageTests final : public QObject {
  Q_OBJECT
  QTemporaryDir Directory;
  QJsonArray Reports;
private slots:
  void initTestCase() {
    if (QGuiApplication::platformName().startsWith("wayland") &&
        qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER")) {
      // The socket precedes desktop readiness. Park the trusted pointer away
      // from (0, 0), whose hot corner otherwise reopens the startup overview.
      QTRY_VERIFY_WITH_TIMEOUT(
          privateShell("if (Main.layoutManager._startingUp) throw "
                       "Error('desktop starting');"),
          Timeout);
      QVERIFY(privateShell("global.neverdLineagePointer = "
                           "imports.gi.Clutter.get_default_backend()"
                           ".get_default_seat().create_virtual_device(imports."
                           "gi.Clutter.InputDeviceType.POINTER_DEVICE); "
                           "global.neverdLineagePointer.notify_absolute_motion("
                           "imports.gi.GLib.get_monotonic_time(), "
                           "global.stage.width / 2, global.stage.height / 2); "
                           "Main.overview.hide();"));
      QTest::qWait(750);
      QTRY_VERIFY_WITH_TIMEOUT(
          privateShell(
              "Main.overview.hide(); "
              "if (Main.overview.visible) throw Error('overview closing');"),
          Timeout);
    }
    QCoreApplication::setOrganizationName("NeverDLineageTests");
    QCoreApplication::setApplicationName("LineageTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       Directory.path());
    Theme::instance().apply();
    ComparisonSession Source(TEST_LINEAGE_WORKER, this);
    QSignalSpy Errors(&Source, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Source.ready(), Timeout);
    QList<QJsonObject> Inputs;
    for (int I = 1; I <= 3; ++I) {
      const auto Path = Directory.filePath(QString("release-%1.raw").arg(I));
      auto Bytes = QByteArray::fromHex("b801000000c3");
      Bytes[1] = char(I);
      QVERIFY(write(Path, Bytes));
      Inputs.append({{"path", Path},
                     {"load_options", QJsonObject{{"loader", "binary"},
                                                  {"processor", "x86_64"},
                                                  {"platform", "sysv"},
                                                  {"base", "0x1000"}}}});
    }
    for (auto Pair : {qMakePair(0, 1), qMakePair(1, 2), qMakePair(0, 2)}) {
      Source.start(Inputs[Pair.first], Inputs[Pair.second]);
      QTRY_VERIFY_WITH_TIMEOUT(!Source.busy() && !Source.revision().isEmpty(),
                               Timeout);
      QCOMPARE(Errors.size(), 0);
      const auto Path = Directory.filePath(
          QString("%1-%2.nddiff").arg(Pair.first).arg(Pair.second));
      bool Saved = false;
      Source.request("comparison_save", {{"path", Path}}, this,
                     [&](const QJsonObject &R) {
                       Saved = R["status"] == QJsonValue("ok");
                     });
      QTRY_VERIFY_WITH_TIMEOUT(Saved, Timeout);
      Reports.append(Path);
    }
  }

  void referenceUncertaintySurvivesLineageTrace_data() {
    QTest::addColumn<QString>("Locale");
    QTest::newRow("English") << QString("en");
    QTest::newRow("Chinese") << QString("zh-CN");
  }

  void referenceUncertaintySurvivesLineageTrace() {
    QFETCH(QString, Locale);
    applyLanguage(Locale);
    const auto RestoreLanguage = qScopeGuard([] { applyLanguage("en"); });
    ComparisonSession Source(TEST_LINEAGE_WORKER, this);
    QSignalSpy SourceErrors(&Source, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Source.ready(), Timeout);
    QList<QJsonObject> Inputs;
    for (int I = 0; I != 2; ++I) {
      const auto Path = Directory.filePath(QString("uncertain-%1.raw").arg(I));
      QByteArray Bytes(24, '\0');
      Bytes.replace(0, 7, QByteArray::fromHex("8b050a000000c3"));
      Bytes[16] = 17;
      Bytes[23] = char(I);
      QVERIFY(write(Path, Bytes));
      Inputs.append({{"path", Path},
                     {"load_options", QJsonObject{{"loader", "binary"},
                                                  {"processor", "x86_64"},
                                                  {"platform", "sysv"},
                                                  {"base", "0x1000"}}}});
    }
    Source.start(Inputs[0], Inputs[1]);
    QTRY_VERIFY_WITH_TIMEOUT(!Source.busy() && !Source.revision().isEmpty(),
                             Timeout);
    QCOMPARE(SourceErrors.size(), 0);
    const auto Report = Directory.filePath("uncertain-lineage.nddiff");
    bool Saved = false;
    Source.request(
        "comparison_save", {{"path", Report}}, this,
        [&](const QJsonObject &R) { Saved = R["status"] == QJsonValue("ok"); });
    QTRY_VERIFY_WITH_TIMEOUT(Saved, Timeout);
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.resize(1180, 780);
    View.show();
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", QJsonArray{Report}}});
    auto *Functions = View.findChild<LineagePageView *>("lineageFunctions");
    auto *Edges = View.findChild<LineagePageView *>("lineageTraceEdges");
    auto *Trace = View.findChild<QPushButton *>("lineageTrace");
    auto *TraceTabs = View.findChild<QTabWidget *>("lineageTraceTabs");
    QVERIFY(Functions && Edges && Trace && TraceTabs);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().size() == 1,
                             Timeout);
    Trace->click();
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Edges->rows().size() == 1,
                             Timeout);
    TraceTabs->setCurrentIndex(1);
    const auto Expected = QCoreApplication::translate(
        "neverd::gui::ComparisonModel", "Uncertain");
    if (Locale == "zh-CN")
      QCOMPARE(Expected, QString::fromUtf8("不确定"));
    const auto Evidence = Edges->selected();
    QCOMPARE(Evidence["change_state"].toString(), QString("uncertain"));
    QVERIFY(Evidence["changes"].toArray().isEmpty());
    QVERIFY(Evidence["analysis_complete"].toBool());
    QVERIFY(!Evidence["reference_comparison_complete"].toBool());
    QCOMPARE(Edges->table()->model()->index(0, 1).data().toString(), Expected);
    const auto PreviousMode = Theme::instance().mode();
    const auto RestoreTheme =
        qScopeGuard([&] { Theme::instance().setMode(PreviousMode); });
    if (const auto Captures =
            qEnvironmentVariable("NEVERD_LINEAGE_SCREENSHOTS");
        !Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(60);
        QVERIFY(View.grab().save(Captures + "/uncertain-" + Locale + '-' +
                                 Theme::modeName(Mode) + ".png"));
      }
    }
    const auto Collection = Directory.filePath("uncertain.ndlineage");
    QSignalSpy Saves(&Session, &LineageSession::saved);
    Session.save(Collection);
    QTRY_COMPARE_WITH_TIMEOUT(Saves.size(), 1, Timeout);
    Session.load({{"path", Collection}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().size() == 1,
                             Timeout);
    Trace->click();
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Edges->rows().size() == 1,
                             Timeout);
    QCOMPARE(Edges->selected(), Evidence);
    QCOMPARE(Edges->table()->model()->index(0, 1).data().toString(), Expected);
    QCOMPARE(Errors.size(), 0);
  }

  void nativeRegionCallConditionsSurviveReopen_data() {
    QTest::addColumn<bool>("Calls");
    QTest::addColumn<QString>("Locale");
    for (const auto *Locale : {"en", "zh-CN"}) {
      QTest::newRow(qPrintable(QString("ordinary-%1").arg(Locale)))
          << false << QString(Locale);
      QTest::newRow(qPrintable(QString("calls-%1").arg(Locale)))
          << true << QString(Locale);
    }
  }

  void nativeRegionCallConditionsSurviveReopen() {
    QFETCH(bool, Calls);
    QFETCH(QString, Locale);
    applyLanguage(Locale);
    const auto RestoreLanguage = qScopeGuard([] { applyLanguage("en"); });
    const auto Path = Directory.filePath("lineage-regions.raw");
    auto Code = QByteArray::fromHex(
        Calls ? "89f883c00383f017e8130000006bc00783c00983f025c3"
              : "89f883c00383f0176bc00783c00983f025c3");
    if (Calls) {
      Code.append(QByteArray(32 - Code.size(), char(0x90)));
      Code.append(QByteArray::fromHex("89f883c00bc3"));
    }
    QVERIFY(write(Path, Code));
    ComparisonSession Source(TEST_LINEAGE_WORKER, this);
    QSignalSpy SourceErrors(&Source, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Source.ready(), Timeout);
    const auto Input = [&](const char *Base) {
      return QJsonObject{{"path", Path},
                         {"load_options", QJsonObject{{"loader", "binary"},
                                                      {"processor", "x86_64"},
                                                      {"base", Base}}}};
    };
    Source.start(
        Input("0x1000"), Input("0x8000"),
        {{"regions", QJsonObject{{"enabled", true}, {"calls", Calls}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Source.busy(), Timeout);
    QCOMPARE(SourceErrors.size(), 0);
    QCOMPARE(Source.summary()["regions"].toObject()["hypothesis_count"].toInt(),
             1);
    const auto Report = Directory.filePath("lineage-regions.nddiff");
    bool Saved = false;
    Source.request(
        "comparison_save", {{"path", Report}}, this,
        [&](const QJsonObject &R) { Saved = R["status"] == QJsonValue("ok"); });
    QTRY_VERIFY_WITH_TIMEOUT(Saved, Timeout);
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.show();
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Collection = Directory.filePath("call-conditions.ndlineage");
    for (const bool Reopen : {false, true}) {
      if (Reopen) {
        QSignalSpy Saves(&Session, &LineageSession::saved);
        Session.save(Collection);
        QTRY_COMPARE_WITH_TIMEOUT(Saves.size(), 1, Timeout);
        Session.load({{"path", Collection}});
      } else {
        Session.load({{"reports", QJsonArray{Report}}});
      }
      auto *Inspect = View.findChild<QPushButton *>("lineageInspectSource");
      QVERIFY(Inspect);
      QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Inspect->isEnabled(),
                               Timeout);
      Inspect->click();
      auto *Evidence = View.findChild<QDialog *>("lineageSourceEvidence");
      QVERIFY(Evidence);
      auto *Regions = Evidence->findChild<LineagePageView *>("native_regions");
      QVERIFY(Regions);
      QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Regions->rows().size() == 1,
                               Timeout);
      QCOMPARE(Regions->selected()["profile"].toString(),
               QString(Calls ? "native-call-runs-v1" : "native-runs-v1"));
      QCOMPARE(Regions->selected().contains("call_evidence"), Calls);
      auto *All = Evidence->findChild<LineagePageView *>("native_anchors");
      QVERIFY(All);
      QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && All->rows().size() == 1,
                               Timeout);
      QCOMPARE(All->selected()["profile"], Regions->selected()["profile"]);
      QCOMPARE(All->selected()["call_evidence"],
               Regions->selected()["call_evidence"]);
      QVERIFY(!All->selected().contains("hypothesis_id"));
      const auto *AllNotice =
          All->parentWidget()->findChild<QLabel *>("lineageRegionCallEvidence");
      QVERIFY(AllNotice);
      QCOMPARE(AllNotice->isHidden(), !Calls);
      Evidence->findChild<QTabWidget *>()->setCurrentWidget(
          All->parentWidget());
      QTest::qWait(40);
      const auto Captures = qEnvironmentVariable("NEVERD_LINEAGE_SCREENSHOTS");
      if (!Reopen && Calls && !Captures.isEmpty()) {
        QVERIFY(QDir().mkpath(Captures));
        QVERIFY(Evidence->grab().save(Captures + "/all-native-spans-" + Locale +
                                      ".png"));
      }
      Evidence->findChild<QTabWidget *>()->setCurrentWidget(Regions);
      Regions->table()->setFocus();
      QTest::keyClick(Regions->table(), Qt::Key_Return);
      auto *Detail = Evidence->findChild<QDialog *>("lineageRegionAnchors");
      QVERIFY(Detail);
      const auto *Notice =
          Detail->findChild<QLabel *>("lineageRegionCallEvidence");
      QCOMPARE(Notice != nullptr, Calls);
      if (Notice)
        QVERIFY(Notice->text().contains(
            Locale == "zh-CN" ? QString::fromUtf8("函数配对")
                              : QString("source report's function pairs")));
      auto *Anchors =
          Detail->findChild<LineagePageView *>("native_region_anchors");
      QVERIFY(Anchors);
      QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Anchors->rows().size() == 1,
                               Timeout);
      QCOMPARE(Anchors->selected()["profile"], Regions->selected()["profile"]);
      QCOMPARE(Anchors->selected()["call_evidence"],
               Regions->selected()["call_evidence"]);
      QVERIFY(!Anchors->selected()["native_authenticated"].toBool(true));
      if (!Reopen && Calls) {
        const auto Captures =
            qEnvironmentVariable("NEVERD_LINEAGE_SCREENSHOTS");
        if (!Captures.isEmpty()) {
          QVERIFY(QDir().mkpath(Captures));
          QVERIFY(Detail->grab().save(Captures + "/call-region-conditions-" +
                                      Locale + ".png"));
        }
      }
      delete Evidence;
    }
    QCOMPARE(Errors.size(), 0);
  }

  void retainedUnclassifiedNativeSpans() {
    const auto Report = qEnvironmentVariable("NEVERD_DIFF_CALL_REGIONS_REPORT");
    if (Report.isEmpty())
      QSKIP("Set NEVERD_DIFF_CALL_REGIONS_REPORT for the retained all-span "
            "workflow.");
    const auto Bytes = read(Report);
    const auto Witness =
        QJsonDocument::fromJson(
            read(qEnvironmentVariable("NEVERD_DIFF_REGION_WORKFLOW")))
            .object();
    QCOMPARE(
        QCryptographicHash::hash(Bytes, QCryptographicHash::Sha256).toHex(),
        Witness["report_sha256"].toString().toLatin1());
    const auto Regions =
        QJsonDocument::fromJson(Bytes).object()["regions"].toObject();
    const auto Anchors = Regions["anchors"].toArray();
    QVERIFY(Anchors.size() > 10000);
    QSet<int> Grouped;
    for (const auto &H : Regions["hypotheses"].toArray())
      for (const auto &I : H.toObject()["anchors"].toArray())
        Grouped.insert(I.toInt());
    QVERIFY(Grouped.size() < Anchors.size());
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.show();
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    const auto Collection = Directory.filePath("all-native-spans.ndlineage");
    for (const bool Reopen : {false, true}) {
      if (Reopen) {
        QSignalSpy Saves(&Session, &LineageSession::saved);
        Session.save(Collection);
        QTRY_COMPARE_WITH_TIMEOUT(Saves.size(), 1, Timeout);
        Session.load({{"path", Collection}});
      } else {
        Session.load({{"reports", QJsonArray{Report}}});
      }
      auto *Inspect = View.findChild<QPushButton *>("lineageInspectSource");
      QVERIFY(Inspect);
      QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Inspect->isEnabled(),
                               Timeout);
      Inspect->click();
      auto *Evidence = View.findChild<QDialog *>("lineageSourceEvidence");
      QVERIFY(Evidence);
      auto *All = Evidence->findChild<LineagePageView *>("native_anchors");
      QVERIFY(All);
      QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !All->rows().isEmpty(),
                               Timeout);
      Evidence->findChild<QTabWidget *>()->setCurrentWidget(
          All->parentWidget());
      bool Unclassified = false;
      for (const auto &Row : All->rows())
        Unclassified |= !Grouped.contains(Row.toObject()["index"].toInt());
      QVERIFY(Unclassified);
      auto *Next = All->findChild<QToolButton *>("lineageNextPage");
      QVERIFY(Next && Next->isEnabled());
      Next->click();
      QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && All->offset() > 0 &&
                                   !All->rows().isEmpty(),
                               Timeout);
      const auto Row = All->selected();
      const auto Index = Row["index"].toInt(-1);
      QVERIFY(Index >= 0 && Index < Anchors.size());
      for (const auto *Side : {"primary", "secondary"}) {
        const auto Original = Anchors[Index].toObject()[Side].toArray();
        QCOMPARE(Row[QString(Side) + "_function_index"].toInt(),
                 Original[0].toInt());
        QCOMPARE(Row[QString(Side) + "_block_index"].toInt(),
                 Original[1].toInt());
        QCOMPARE(Row[QString(Side) + "_instruction_index"].toInt(),
                 Original[2].toInt());
        QCOMPARE(Row[QString(Side) + "_instruction_count"].toInt(),
                 Original[3].toInt());
      }
      QCOMPARE(Row["profile"], Regions["profile"]);
      QCOMPARE(Row["call_evidence"], Regions["call_evidence"]);
      QVERIFY(!Row.contains("hypothesis_id"));
      QTest::qWait(60);
      QVERIFY(All->table()->viewport()->rect().contains(
          All->table()->visualRect(All->table()->currentIndex())));
      const auto Captures = qEnvironmentVariable("NEVERD_LINEAGE_SCREENSHOTS");
      if (!Reopen && !Captures.isEmpty()) {
        QVERIFY(QDir().mkpath(Captures));
        QVERIFY(Evidence->grab().save(Captures +
                                      "/real-unclassified-native-spans.png"));
      }
      delete Evidence;
    }
    QCOMPARE(Errors.size(), 0);
  }

  void retainedRealCorpusWorkflow() {
    const auto Corpus = qEnvironmentVariable("NEVERD_LINEAGE_REAL_COLLECTION");
    if (Corpus.isEmpty())
      QSKIP("Set NEVERD_LINEAGE_REAL_COLLECTION to a retained real-program "
            "collection.");
    const auto Document = QJsonDocument::fromJson(read(Corpus)).object();
    const auto Sources = Document["reports"].toArray();
    QVERIFY(!Sources.isEmpty());
    QHash<QString, int> Inventories;
    int ExpectedEdges = 0, LargestInventory = 0;
    QString TargetHash, TargetAddress;
    for (const auto &Text : Sources) {
      const auto Report =
          QJsonDocument::fromJson(Text.toString().toUtf8()).object();
      const auto Matches = Report["matches"].toArray();
      ExpectedEdges += Matches.size();
      for (const auto *Side : {"primary", "secondary"}) {
        const auto Hash = Report[Side].toObject()["sha256"].toString();
        const auto Count =
            Report["functions"].toObject()[Side].toArray().size();
        QVERIFY(!Hash.isEmpty());
        if (Inventories.contains(Hash))
          QCOMPARE(Inventories[Hash], Count);
        Inventories[Hash] = Count;
        if (Count > LargestInventory && !Matches.isEmpty()) {
          LargestInventory = Count;
          TargetHash = Hash;
          TargetAddress = Matches[0].toObject()[Side].toString();
        }
      }
    }
    QVERIFY(!TargetAddress.isEmpty());
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.resize(1180, 780);
    View.show();
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"path", Corpus}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Functions = View.findChild<LineagePageView *>("lineageFunctions");
    auto *Edges = View.findChild<LineagePageView *>("lineageTraceEdges");
    auto *Trace = View.findChild<QPushButton *>("lineageTrace");
    auto *Search = View.findChild<QLineEdit *>("lineageSearch");
    QVERIFY(Versions && Functions && Edges && Trace && Search);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() &&
                                 Versions->rows().size() == Inventories.size(),
                             Timeout);
    QCOMPARE(Errors.size(), 0);
    QCOMPARE(Session.summary()["edges"].toInt(), ExpectedEdges);
    QCOMPARE(Session.summary()["reports"].toInt(), Sources.size());
    int TargetRow = -1;
    for (int I = 0; I != Versions->rows().size(); ++I)
      if (Versions->rows()[I].toObject()["sha256"].toString() == TargetHash)
        TargetRow = I;
    QVERIFY(TargetRow >= 0);
    Versions->table()->selectRow(TargetRow);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Functions->rows().isEmpty(),
                             Timeout);
    auto *Next = Functions->findChild<QToolButton *>("lineageNextPage");
    QVERIFY(Next && Next->isEnabled());
    const auto First = Functions->rows()[0].toObject()["address"].toString();
    Next->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        !Session.busy() && !Functions->rows().isEmpty() &&
            Functions->rows()[0].toObject()["address"].toString() != First,
        Timeout);
    Search->setText(TargetAddress);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().size() == 1,
                             Timeout);
    QCOMPARE(Functions->selected()["address"].toString(), TargetAddress);
    Trace->click();
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Edges->rows().isEmpty(),
                             Timeout);
    View.findChild<QTabWidget *>("lineageTraceTabs")->setCurrentIndex(1);
    const auto Revision = Session.revision();
    const auto OriginalMode = Theme::instance().mode();
    const auto RestoreTheme =
        qScopeGuard([&] { Theme::instance().setMode(OriginalMode); });
    if (const auto Captures =
            qEnvironmentVariable("NEVERD_LINEAGE_SCREENSHOTS");
        !Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(60);
        QVERIFY(View.grab().save(Captures + "/real-corpus-" +
                                 Theme::modeName(Mode) + ".png"));
      }
    }
    const auto Saved = Directory.filePath("real-corpus.ndlineage");
    QSignalSpy Saves(&Session, &LineageSession::saved);
    Session.save(Saved);
    QTRY_COMPARE_WITH_TIMEOUT(Saves.size(), 1, Timeout);
    QCOMPARE(QJsonDocument::fromJson(read(Saved)).object(), Document);
    Session.load({{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() &&
                                 Versions->rows().size() == Inventories.size(),
                             Timeout);
    QCOMPARE(Session.revision(), Revision);
    QCOMPARE(Errors.size(), 0);
  }

  void localizedWorkspace_data() {
    QTest::addColumn<QString>("Locale");
    for (const auto &Language : languages())
      QTest::newRow(qPrintable(Language.code)) << Language.code;
  }

  void localizedWorkspace() {
    QFETCH(QString, Locale);
    QTranslator Catalog;
    auto Resource = Locale;
    Resource.replace('-', '_');
    QVERIFY(Catalog.load(":/i18n/neverd_" + Resource + ".qm"));
    const auto Translated = [&](const char *Context, const char *Source) {
      return Catalog.translate(Context, Source);
    };
    applyLanguage(Locale);
    const auto RestoreLanguage = qScopeGuard([] { applyLanguage("en"); });
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.resize(760, 560);
    View.show();
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Reports}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Functions = View.findChild<LineagePageView *>("lineageFunctions");
    auto *Tabs = View.findChild<QTabWidget *>("lineageTabs");
    QVERIFY(Versions && Functions && Tabs);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 3 &&
                                 Functions->rows().size() == 1,
                             Timeout);
    QCOMPARE(Errors.size(), 0);
    QCOMPARE(View.windowTitle(), Translated("neverd::gui::LineageDialog",
                                            "Version comparison graph"));
    if (Locale != "en")
      QVERIFY(View.windowTitle() != "Version comparison graph");
    QCOMPARE(Tabs->tabText(0),
             Translated("neverd::gui::LineageDialog", "Functions"));
    const auto *Add = View.findChild<QPushButton *>("lineageAdd");
    QVERIFY(Add);
    QCOMPARE(Add->text(),
             Translated("neverd::gui::LineageDialog", "&Add comparisons…"));
    const auto *Rows = Versions->findChild<QSpinBox *>("lineagePageRows");
    QVERIFY(Rows && Rows->isEnabled());
    QCOMPARE(Rows->accessibleName(),
             Translated("neverd::gui::LineagePageView", "Rows per page"));
    QCOMPARE(Rows->prefix(),
             Translated("neverd::gui::LineagePageView", "Rows: "));
    const auto *Previous =
        Versions->findChild<QToolButton *>("lineagePreviousPage");
    const auto *Next = Versions->findChild<QToolButton *>("lineageNextPage");
    QVERIFY(Previous && Next);
    QCOMPARE(Previous->arrowType(),
             Locale == "ar" ? Qt::RightArrow : Qt::LeftArrow);
    QCOMPARE(Next->arrowType(),
             Locale == "ar" ? Qt::LeftArrow : Qt::RightArrow);
    QCOMPARE(Versions->table()->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(Functions->table()->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(
        Functions->table()->model()->headerData(1, Qt::Horizontal).toString(),
        Translated("neverd::gui::LineageDialog", "Address"));
    QCOMPARE(Functions->selected()["address"].toString(), QString("0x1000"));
    QCOMPARE(View.layoutDirection(),
             Locale == "ar" ? Qt::RightToLeft : Qt::LeftToRight);
    QCOMPARE(Session.message(), Translated("neverd::gui::LineageSession",
                                           "Version graph loaded."));
    const auto SelectedRowVisible = [](LineagePageView *Page) {
      const auto *Table = Page->table();
      const auto Row = Table->visualRect(Table->currentIndex());
      const auto Viewport = Table->viewport()->rect();
      const auto *Status = Page->findChild<QLabel *>("lineagePageStatus");
      return Row.isValid() && Row.top() >= Viewport.top() &&
             Row.bottom() <= Viewport.bottom() && Status &&
             Page->rect().contains(Table->geometry()) &&
             Page->rect().contains(Status->geometry()) &&
             Table->geometry().bottom() < Status->geometry().top();
    };
    const auto RowGeometry = [&] {
      QString Text = QString("window=%1x%2 minimum=%3x%4")
                         .arg(View.width())
                         .arg(View.height())
                         .arg(View.minimumWidth())
                         .arg(View.minimumHeight());
      for (const auto *Page : View.findChildren<LineagePageView *>()) {
        const auto *Table = Page->table();
        Text += QString(" %1: table=%2 min=%3 viewport=%4 header=%5 "
                        "headerHint=%6 row=%7 visualTop=%8 visualBottom=%9 "
                        "pageHeight=%10 statusTop=%11 tableBottom=%12")
                    .arg(Page->objectName())
                    .arg(Table->height())
                    .arg(Table->minimumHeight())
                    .arg(Table->viewport()->height())
                    .arg(Table->horizontalHeader()->height())
                    .arg(Table->horizontalHeader()->sizeHint().height())
                    .arg(Table->rowHeight(Table->currentIndex().row()))
                    .arg(Table->visualRect(Table->currentIndex()).top())
                    .arg(Table->visualRect(Table->currentIndex()).bottom())
                    .arg(Page->height())
                    .arg(Page->findChild<QLabel *>("lineagePageStatus")->y())
                    .arg(Table->geometry().bottom());
      }
      for (const auto *Widget : View.findChildren<QWidget *>())
        if (const auto *Layout = Widget->layout();
            Layout && !Widget->objectName().startsWith("qt_scrollarea_"))
          Text += QString(" layout=%1/%2 width=%3 minimumWidth=%4 hint=%5")
                      .arg(Widget->metaObject()->className())
                      .arg(Widget->objectName())
                      .arg(Widget->width())
                      .arg(Layout->minimumSize().width())
                      .arg(Widget->minimumSizeHint().width());
      for (const auto *Layout : View.findChildren<QLayout *>()) {
        if (Layout->minimumSize().width() <= View.width() / 2)
          continue;
        Text += QString(" wideLayout=%1/%2 items=")
                    .arg(Layout->metaObject()->className())
                    .arg(Layout->minimumSize().width());
        for (int I = 0; I < Layout->count(); ++I)
          if (const auto *Widget = Layout->itemAt(I)->widget())
            Text += Widget->objectName() + ',';
      }
      return Text;
    };
    QTRY_VERIFY2(SelectedRowVisible(Versions) && SelectedRowVisible(Functions),
                 qPrintable(RowGeometry()));
    QTest::qWait(60);
    if (const auto Output = qEnvironmentVariable("NEVERD_LINEAGE_SCREENSHOTS");
        !Output.isEmpty()) {
      QVERIFY(QDir().mkpath(Output));
      QVERIFY(View.grab().save(Output + "/locale-" + Locale + ".png"));
    }
    QVERIFY2(View.width() == 760, qPrintable(RowGeometry()));
    const auto *Status = Versions->findChild<QLabel *>("lineagePageStatus");
    QVERIFY(Status && Status->isVisible());
    QVERIFY(Status->width() >=
            Status->fontMetrics().horizontalAdvance(Status->text()));
    for (const auto *Name : {"lineageAdd", "lineageOpen", "lineageSave",
                             "lineageOptions", "lineageCancel"}) {
      const auto *Button = View.findChild<QPushButton *>(Name);
      QVERIFY(Button && Button->isVisible());
      QVERIFY(View.rect().contains(
          QRect(Button->mapTo(&View, QPoint()), Button->size())));
    }
    const auto Revision = Session.revision();
    const auto Target = Versions->rows()[1].toObject()["id"].toString();
    auto *Graph = View.findChild<LineageGraph *>("lineageGraph");
    QVERIFY(Graph);
    Graph->versionActivated(Target);
    QTRY_VERIFY_WITH_TIMEOUT(
        !Session.busy() && Functions->selected()["version"] == Target, Timeout);
    QCOMPARE(Versions->selected()["id"].toString(), Target);
    QCOMPARE(Session.revision(), Revision);
    View.setLayoutDirection(Locale == "ar" ? Qt::LeftToRight : Qt::RightToLeft);
    QCOMPARE(Previous->arrowType(),
             Locale == "ar" ? Qt::LeftArrow : Qt::RightArrow);
    QCOMPARE(Next->arrowType(),
             Locale == "ar" ? Qt::RightArrow : Qt::LeftArrow);
    QCOMPARE(Session.revision(), Revision);
    const auto Malformed = Directory.filePath("localized-invalid.ndlineage");
    QVERIFY(write(Malformed, "{broken"));
    Session.load({{"path", Malformed}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Errors.size() == 1, Timeout);
    QCOMPARE(Session.revision(), Revision);
    QCOMPARE(Functions->selected()["version"].toString(), Target);
    QCOMPARE(Session.message(),
             Translated("neverd::gui::LineageSession",
                        "The version graph file is not valid UTF-8 JSON. "
                        "Check the file and try again.") +
                 Translated("neverd::gui::LineageSession",
                            "\nYour previous graph is still available."));
    View.setLayoutDirection(Locale == "ar" ? Qt::RightToLeft : Qt::LeftToRight);
    QTRY_VERIFY2(SelectedRowVisible(Versions) && SelectedRowVisible(Functions),
                 qPrintable(RowGeometry()));
    if (const auto Output = qEnvironmentVariable("NEVERD_LINEAGE_SCREENSHOTS");
        !Output.isEmpty()) {
      QTest::qWait(60);
      QVERIFY(View.grab().save(Output + "/locale-error-" + Locale + ".png"));
    }
    QVERIFY(Session.releaseEditingSupported());
    Session.editReleases(
        {{"upsert",
          QJsonArray{QJsonObject{{"id", "parent"},
                                 {"label", "Stable 1 中文"},
                                 {"branch", "stable"},
                                 {"version", Target},
                                 {"published_at", QJsonValue::Null},
                                 {"parents", QJsonArray{}}},
                     QJsonObject{{"id", "child"},
                                 {"label", "Stable 2 中文"},
                                 {"branch", "stable"},
                                 {"version", Target},
                                 {"published_at", "2026-10-25T08:00:00Z"},
                                 {"parents", QJsonArray{"parent"}}}}}},
        {}, "child");
    auto *Releases = View.findChild<LineagePageView *>("lineageReleases");
    auto *Parents = View.findChild<LineagePageView *>("lineageReleaseParents");
    auto *Relations = View.findChild<QTabWidget *>("lineageReleaseRelations");
    QVERIFY(Releases && Parents && Relations);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() &&
                                 Releases->selected()["id"] == "child" &&
                                 Parents->rows().size() == 1,
                             Timeout);
    Tabs->setCurrentWidget(View.findChild<QWidget *>("lineageReleaseView"));
    QCOMPARE(Tabs->tabText(Tabs->currentIndex()),
             Translated("neverd::gui::LineageDialog", "Releases"));
    const auto *EditRelease =
        View.findChild<QPushButton *>("lineageReleaseEdit");
    QVERIFY(EditRelease && EditRelease->isEnabled());
    QCOMPARE(EditRelease->text(),
             Translated("neverd::gui::LineageReleaseView", "&Edit release…"));
    if (const auto Output = qEnvironmentVariable("NEVERD_LINEAGE_SCREENSHOTS");
        !Output.isEmpty()) {
      QTest::qWait(60);
      QVERIFY(View.grab().save(Output + "/release-layout-" + Locale + ".png"));
    }
    QTRY_VERIFY2(SelectedRowVisible(Releases), qPrintable(RowGeometry()));
    for (const auto *Name :
         {"lineageReleaseAdd", "lineageReleaseEdit", "lineageReleaseRemove",
          "lineageReleaseSnapshot"}) {
      const auto *Button = View.findChild<QPushButton *>(Name);
      QVERIFY(Button && Button->isVisible());
      QVERIFY(View.rect().contains(
          QRect(Button->mapTo(&View, QPoint()), Button->size())));
    }
    const auto ActionGeometry = [&]() -> QString {
      for (const auto *Button : View.findChildren<QPushButton *>()) {
        if (!Button->isVisible())
          continue;
        if (Button->width() < Button->minimumSizeHint().width())
          return QString("narrow action=%1 width=%2 minimum=%3")
              .arg(Button->objectName())
              .arg(Button->width())
              .arg(Button->minimumSizeHint().width());
        for (const QWidget *Parent = Button->parentWidget(); Parent;
             Parent = Parent->parentWidget()) {
          const QRect Bounds(Button->mapTo(Parent, QPoint()), Button->size());
          if (!Parent->rect().contains(Bounds))
            return "clipped action=" + Button->objectName() +
                   " ancestor=" + Parent->metaObject()->className() + '/' +
                   Parent->objectName();
          if (Parent == &View)
            break;
        }
      }
      return {};
    };
    const auto CaptureRelease = [&](const QString &Name,
                                    LineagePageView *Page) {
      const auto Output = qEnvironmentVariable("NEVERD_LINEAGE_SCREENSHOTS");
      const auto OriginalMode = Theme::instance().mode();
      const auto RestoreTheme =
          qScopeGuard([&] { Theme::instance().setMode(OriginalMode); });
      for (const auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        QTest::qWait(60);
        if (!Output.isEmpty())
          QVERIFY(View.grab().save(Output + '/' + Name + '-' + Locale + '-' +
                                   Theme::modeName(Mode) + ".png"));
        QTRY_VERIFY2(SelectedRowVisible(Page) && ActionGeometry().isEmpty(),
                     qPrintable(ActionGeometry() + ' ' + RowGeometry()));
      }
    };
    CaptureRelease("release", Releases);
    Relations->setCurrentIndex(1);
    QTRY_VERIFY(SelectedRowVisible(Parents));
    QCOMPARE(Parents->selected()["id"].toString(), QString("parent"));
    CaptureRelease("release-parents", Parents);
  }

  void navigationPersistenceAndRender() {
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Reports}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Functions = View.findChild<LineagePageView *>("lineageFunctions");
    auto *Trace = View.findChild<LineagePageView *>("lineageTraceNodes");
    auto *Tabs = View.findChild<QTabWidget *>("lineageTabs");
    QVERIFY(Versions && Functions && Trace && Tabs);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 3 &&
                                 Functions->rows().size() == 1,
                             Timeout);
    QCOMPARE(Session.summary()["edges"].toInt(), 3);
    QCOMPARE(Errors.size(), 0);
    Versions->table()->selectRow(1);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    const auto ID = Versions->selected().value("id");
    Session.load(
        {{"append", true},
         {"version_updates",
          QJsonArray{QJsonObject{{"id", ID},
                                 {"label", "Release 2026.10 — stable 中文"},
                                 {"branch", "maintenance"}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().size() == 1,
                             Timeout);
    QCOMPARE(Versions->selected()["label"].toString(),
             QString::fromUtf8("Release 2026.10 — stable 中文"));
    auto *TraceButton = View.findChild<QPushButton *>("lineageTrace");
    QVERIFY(TraceButton && TraceButton->isEnabled());
    TraceButton->setFocus();
    QTest::keyClick(TraceButton, Qt::Key_Space);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Trace->rows().size() == 3,
                             Timeout);
    QCOMPARE(Tabs->currentIndex(), 1);
    View.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&View));
    Trace->table()->setFocus();
    Trace->table()->selectRow(0);
    QTest::keyClick(Trace->table(), Qt::Key_C, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(
        QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8())
            .object(),
        Trace->selected(), Timeout);
    const auto Copied =
        QJsonDocument::fromJson(QApplication::clipboard()->text().toUtf8())
            .object();
    QVERIFY(Copied.contains("witness_edge") && Copied.contains("version"));
    Tabs->setCurrentIndex(2);
    auto *Graph = View.findChild<LineageGraph *>("lineageGraph");
    const auto Target = Versions->rows()[0].toObject()["id"].toString();
    QTest::qWait(60);
    QGraphicsItem *Node = nullptr;
    for (auto *Item : Graph->scene()->items())
      if (Item->data(0) == "version" && Item->data(1).toString() == Target)
        Node = Item;
    QVERIFY(Node);
    QTest::mouseDClick(Graph->viewport(), Qt::LeftButton, Qt::NoModifier,
                       Graph->mapFromScene(Node->sceneBoundingRect().center()));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Tabs->currentIndex() == 0,
                             Timeout);
    QCOMPARE(Versions->selected()["id"].toString(), Target);
    QCOMPARE(Functions->selected()["version"].toString(), Target);
    const auto OriginalRevision = Session.revision();
    const auto Selection = Functions->selected();
    const auto Malformed = Directory.filePath("malformed.ndlineage");
    QVERIFY(write(Malformed, "{broken"));
    Session.load({{"path", Malformed}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Errors.size(), 1);
    QCOMPARE(Session.revision(), OriginalRevision);
    QCOMPARE(Functions->selected(), Selection);
    QVERIFY(Session.message().contains("previous graph"));
    auto *Search = View.findChild<QLineEdit *>("lineageSearch");
    Tabs->setCurrentIndex(0);
    Search->setText("no such function");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().isEmpty(),
                             Timeout);
    QVERIFY(!TraceButton->isEnabled());
    Search->clear();
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().size() == 1,
                             Timeout);
    const auto Captures = qEnvironmentVariable("NEVERD_LINEAGE_SCREENSHOTS");
    if (!Captures.isEmpty()) {
      QVERIFY(QDir().mkpath(Captures));
      const auto OriginalStyle = View.styleSheet();
      const auto OriginalPixels =
          QFontInfo(Functions->table()->font()).pixelSize();
      for (auto Mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(Mode);
        const auto Name = Mode == Theme::Mode::Light ? "light" : "dark";
        View.resize(1180, 780);
        for (int Tab : {0, 1, 2}) {
          Tabs->setCurrentIndex(Tab);
          QTest::qWait(60);
          if (Tab == 2) {
            auto *Graph = View.findChild<LineageGraph *>("lineageGraph");
            QVERIFY(Graph && Graph->height() >= 180 &&
                    Graph->transform().m11() > 0.4);
          }
          QVERIFY(View.grab().save(
              Captures + QString("/%1-tab%2.png").arg(Name).arg(Tab)));
        }
        View.resize(760, 560);
        Tabs->setCurrentIndex(0);
        QTest::qWait(60);
        QVERIFY(
            View.grab().save(Captures + QString("/%1-narrow.png").arg(Name)));
        View.setStyleSheet(OriginalStyle +
                           QString("\nQWidget { font-size: %1px; }")
                               .arg(qRound(OriginalPixels * 1.5)));
        View.resize(1180, 780);
        QTest::qWait(60);
        QVERIFY(View.grab().save(Captures +
                                 QString("/%1-large-font.png").arg(Name)));
        QVERIFY(QFontInfo(Functions->table()->font()).pixelSize() >
                OriginalPixels);
        QVERIFY(Functions->table()->rowHeight(0) >=
                Functions->table()->fontMetrics().height());
        View.setStyleSheet(OriginalStyle);
      }
    }
    const auto Saved = Directory.filePath("workspace.ndlineage");
    QSignalSpy SavedSignal(&Session, &LineageSession::saved);
    Session.save(Saved);
    QTRY_VERIFY_WITH_TIMEOUT(SavedSignal.size() == 1 && !Session.busy(),
                             Timeout);
    const auto Bundle = QJsonDocument::fromJson(read(Saved)).object();
    for (const auto &R : Reports)
      QVERIFY(Bundle["reports"].toArray().contains(
          QString::fromUtf8(read(R.toString()))));
    Session.load({{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Session.revision(), OriginalRevision);
    QCOMPARE(Errors.size(), 1);
  }

  void cancellationRetainsCollectionAndCanRetry() {
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    auto &Session = View.session();
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Reports}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Session.revision().isEmpty(),
                             Timeout);
    const auto Revision = Session.revision();
    const auto Large = Directory.filePath("large-cancel.nddiff");
    QVERIFY(write(Large, read(Reports[0].toString()) +
                             QByteArray(64 * 1024 * 1024, ' ')));
    bool Cancelled = false;
    const auto Connection = connect(
        &Session, &LineageSession::progress, this, [&](const QJsonObject &P) {
          if (!Cancelled && P["phase"] == QJsonValue("lineage:read")) {
            Cancelled = true;
            Session.cancel();
          }
        });
    Session.load({{"reports", QJsonArray{Large}}});
    QTRY_VERIFY_WITH_TIMEOUT(Cancelled && !Session.busy(), Timeout);
    disconnect(Connection);
    QVERIFY(!Session.cancelRequested());
    QCOMPARE(Session.revision(), Revision);
    QVERIFY(Session.message().contains("stopped"));
    Session.load({{"reports", Reports}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Session.revision(), Revision);
    QCOMPARE(Session.summary()["reports"].toInt(), 3);
  }

  void nextPageAndMetadataKeepTheSelectedSnapshot() {
    // Authored report-only identities exercise paging; these copies do not
    // claim fresh native authentication of synthetic release hashes.
    const auto Original =
        QJsonDocument::fromJson(read(Reports[0].toString())).object();
    QJsonArray Many;
    for (int I = 0; I < 202; ++I) {
      auto Document = Original;
      auto Secondary = Document["secondary"].toObject();
      Secondary["sha256"] =
          QString::number(I + 1000, 16).rightJustified(64, '0');
      Secondary["path"] = QString("fixture-release-%1.raw").arg(I);
      Document["secondary"] = Secondary;
      const auto Path = Directory.filePath(QString("paging-%1.nddiff").arg(I));
      QVERIFY(
          write(Path, QJsonDocument(Document).toJson(QJsonDocument::Compact)));
      Many.append(Path);
    }
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    auto &Session = View.session();
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Many}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 200,
                             Timeout);
    QToolButton *Next = nullptr;
    for (auto *B : Versions->findChildren<QToolButton *>())
      if (B->arrowType() == Qt::RightArrow)
        Next = B;
    QVERIFY(Next && Next->isEnabled());
    QTest::mouseClick(Next, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 3,
                             Timeout);
    QVERIFY(!Next->isEnabled());
    Versions->table()->selectRow(1);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    const auto ID = Versions->selected().value("id");
    Session.load({{"append", true},
                  {"version_updates",
                   QJsonArray{QJsonObject{{"id", ID},
                                          {"label", "Second-page release"},
                                          {"branch", "stable"}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Versions->offset(), 200);
    QCOMPARE(Versions->selected().value("id"), ID);
    QCOMPARE(Versions->selected().value("label").toString(),
             QString("Second-page release"));
  }

  void smallerPagesRecoverLargeMetadataWithoutLosingSnapshots() {
    // These report-only fixture identities isolate paging. The source reader
    // still validates every report; no fresh native authentication is claimed.
    const auto Original =
        QJsonDocument::fromJson(read(Reports[0].toString())).object();
    QJsonArray Many;
    for (int I = 0; I < 100; ++I) {
      auto Document = Original;
      for (const auto &[Side, Number] :
           {qMakePair("primary", I * 2), qMakePair("secondary", I * 2 + 1)}) {
        auto Identity = Document[Side].toObject();
        Identity["sha256"] =
            QString::number(Number + 2000, 16).rightJustified(64, '0');
        Document[Side] = Identity;
      }
      const auto Path =
          Directory.filePath(QString("large-page-%1.nddiff").arg(I));
      QVERIFY(
          write(Path, QJsonDocument(Document).toJson(QJsonDocument::Compact)));
      Many.append(Path);
    }
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    View.show();
    View.resize(760, 560);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Many}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Sources = View.findChild<LineagePageView *>("lineageSources");
    QVERIFY(Versions && Sources);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 200,
                             Timeout);
    const auto Initial = Versions->rows();
    const QString Padding(48 * 1024, 'x');
    QJsonArray Updates;
    for (int I = 0; I < Initial.size(); ++I)
      Updates.append(
          QJsonObject{{"id", Initial[I].toObject()["id"]},
                      {"label", QString("Snapshot %1: ").arg(I) + Padding},
                      {"branch", ""}});
    const auto Saved = Directory.filePath("large-page.ndlineage");
    QSignalSpy SavedEvents(&Session, &LineageSession::saved);
    Session.save(Saved);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && SavedEvents.size() == 1,
                             Timeout);
    auto Collection = QJsonDocument::fromJson(read(Saved)).object();
    Collection["versions"] = Updates;
    QVERIFY(
        write(Saved, QJsonDocument(Collection).toJson(QJsonDocument::Compact)));
    Session.load({{"path", Saved}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Errors.size(), 0);
    QCOMPARE(Session.summary()["versions"].toInt(), 200);
    QVERIFY(Versions->rows().isEmpty());
    QVERIFY(Sources->rows().isEmpty());

    // Admission succeeded, but both default pages exceed the byte allowance.
    // The user must be able to recover without replacing or truncating data.
    auto *Rows = Versions->findChild<QSpinBox *>("lineagePageRows");
    QVERIFY(Rows && Rows->isEnabled());
    QCOMPARE(Rows->value(), 200);
    Rows->setValue(100);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 100,
                             Timeout);
    for (int I = 0; I < 100; ++I) {
      QCOMPARE(Versions->rows()[I].toObject()["id"],
               Initial[I].toObject()["id"]);
      QVERIFY(
          Versions->rows()[I].toObject()["label"].toString().endsWith(Padding));
    }
    QToolButton *Next = nullptr, *Previous = nullptr;
    for (auto *Button : Versions->findChildren<QToolButton *>()) {
      if (Button->arrowType() == Qt::RightArrow)
        Next = Button;
      if (Button->arrowType() == Qt::LeftArrow)
        Previous = Button;
    }
    QVERIFY(Next && Previous && Next->isEnabled());
    QTest::mouseClick(Next, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->offset() == 100,
                             Timeout);
    QCOMPARE(Versions->rows().size(), 100);
    for (int I = 0; I < 100; ++I)
      QCOMPARE(Versions->rows()[I].toObject()["id"],
               Initial[I + 100].toObject()["id"]);
    QVERIFY(!Next->isEnabled());
    Rows->setValue(50);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 50,
                             Timeout);
    QCOMPARE(Versions->offset(), 100);
    QVERIFY(Previous->isEnabled());
    QTest::mouseClick(Previous, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->offset() == 50,
                             Timeout);
    QCOMPARE(Versions->rows()[0].toObject()["id"],
             Initial[50].toObject()["id"]);

    auto *SourceRows = Sources->findChild<QSpinBox *>("lineagePageRows");
    QVERIFY(SourceRows && SourceRows->isEnabled());
    SourceRows->setValue(50);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Sources->rows().size() == 50,
                             Timeout);
    QCOMPARE(Session.summary()["versions"].toInt(), 200);
    QCOMPARE(Errors.size(), 0);
    const auto *PageStatus = Versions->findChild<QLabel *>("lineagePageStatus");
    QVERIFY(PageStatus && PageStatus->isVisible());
    QVERIFY(PageStatus->width() >=
            PageStatus->fontMetrics().horizontalAdvance(PageStatus->text()));
    if (const auto Output = qEnvironmentVariable("NEVERD_LINEAGE_SCREENSHOTS");
        !Output.isEmpty())
      QVERIFY(View.grab().save(Output + "/large-page-recovered.png"));
  }

  void largeReportDiagramsCanZoomAfterFitting() {
    LineageGraph Graph;
    Graph.resize(400, 240);
    Graph.show();
    QJsonArray Edges;
    for (int I = 0; I < 200; ++I)
      Edges.append(
          QJsonObject{{"id", QString("report-%1").arg(I)},
                      {"primary_version", "origin"},
                      {"secondary_version", QString("snapshot-%1").arg(I)}});
    Graph.setReports(Edges);
    QTRY_VERIFY_WITH_TIMEOUT(Graph.transform().m11() < 1, Timeout);
    const auto Fitted = Graph.transform();
    Graph.zoom(false);
    QCOMPARE(Graph.transform(), Fitted);
    Graph.zoom(true);
    const auto First = Graph.transform().m11();
    QVERIFY(First > Fitted.m11());
    Graph.zoom(true);
    const auto Second = Graph.transform().m11();
    QVERIFY(Second > First);
    Graph.zoom(false);
    QVERIFY(Graph.transform().m11() < Second);
  }

  void replacementResetsNavigationButAppendAndFailureKeepIt() {
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Reports}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Functions = View.findChild<LineagePageView *>("lineageFunctions");
    auto *Trace = View.findChild<LineagePageView *>("lineageTraceNodes");
    auto *Search = View.findChild<QLineEdit *>("lineageSearch");
    auto *Tabs = View.findChild<QTabWidget *>("lineageTabs");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().size() == 1,
                             Timeout);
    const auto ID = Versions->selected().value("id");
    QTest::mouseClick(View.findChild<QPushButton *>("lineageTrace"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Trace->rows().size() == 3,
                             Timeout);
    Search->setText("an unmatched search from the old collection");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().isEmpty(),
                             Timeout);
    const auto Previous = Session.revision();
    Session.load({{"path", Directory.filePath("missing-lineage.ndlineage")}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Errors.size() == 1, Timeout);
    QCOMPARE(Session.revision(), Previous);
    QCOMPARE(Trace->rows().size(), 3);
    QVERIFY(!Search->text().isEmpty());
    QCOMPARE(Tabs->currentIndex(), 1);
    Session.load({{"append", true},
                  {"version_updates",
                   QJsonArray{QJsonObject{{"id", ID},
                                          {"label", "A retained snapshot"},
                                          {"branch", "stable"}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QCOMPARE(Versions->selected()["id"], ID);
    QCOMPARE(Trace->rows().size(), 3);
    QVERIFY(!Search->text().isEmpty());
    QCOMPARE(Tabs->currentIndex(), 1);

    // Replace with two disjoint authored snapshot identities. These report
    // copies exercise navigation, not native authentication or new analysis.
    auto Other = QJsonDocument::fromJson(read(Reports[0].toString())).object();
    for (const auto *Side : {"primary", "secondary"}) {
      auto Identity = Other[Side].toObject();
      Identity["sha256"] = QString(64, Side[0] == 'p' ? 'a' : 'b');
      Other[Side] = Identity;
    }
    const auto Path = Directory.filePath("replacement.nddiff");
    QVERIFY(write(Path, QJsonDocument(Other).toJson(QJsonDocument::Compact)));
    Session.load({{"reports", QJsonArray{Path}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 2,
                             Timeout);
    QCOMPARE(Errors.size(), 1);
    QVERIFY(Search->text().isEmpty());
    QCOMPARE(Tabs->currentIndex(), 0);
    QVERIFY(Trace->rows().isEmpty());
    QCOMPARE(Functions->rows().size(), 1);
    QVERIFY(Versions->selected()["id"] != ID);
    QTest::mouseClick(View.findChild<QPushButton *>("lineageTrace"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Trace->rows().size() == 2,
                             Timeout);
  }

  void longMetadataKeepsFunctionControlsReachable_data() {
    QTest::addColumn<QString>("Long");
    QTest::newRow("wide") << QString("Long release label ").repeated(1024);
    QTest::newRow("multiple-lines")
        << QString("Release") + QString(256, '\r') + QString(256, '\n') +
               QString(256, QChar::LineSeparator) +
               QString(256, QChar::ParagraphSeparator) + "candidate";
  }

  void longMetadataKeepsFunctionControlsReachable() {
    QFETCH(QString, Long);
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    auto &Session = View.session();
    View.show();
    View.resize(760, 560);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Reports}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Functions = View.findChild<LineagePageView *>("lineageFunctions");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().size() == 1,
                             Timeout);
    const auto Before = View.size();
    const auto ID = Versions->selected().value("id");
    Session.load({{"append", true},
                  {"version_updates",
                   QJsonArray{QJsonObject{
                       {"id", ID}, {"label", Long}, {"branch", "stable"}}}}});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Functions->rows().size() == 1,
                             Timeout);
    QTest::qWait(60);
    QCOMPARE(Versions->selected().value("label").toString(), Long);
    const auto Label = Versions->table()->currentIndex().siblingAtColumn(0);
    QCOMPARE(Label.data(Qt::ToolTipRole).toString(), Long);
    for (const auto Break :
         {QChar('\r'), QChar('\n'), QChar(QChar::LineSeparator),
          QChar(QChar::ParagraphSeparator)})
      QVERIFY(!Label.data().toString().contains(Break));
    QCOMPARE(View.size(), Before);
    QVERIFY(Functions->table()->viewport()->height() >=
            Functions->table()->fontMetrics().height() * 3);
    auto *Action = View.findChild<QPushButton *>("lineageTrace");
    QVERIFY(Action && Action->isEnabled());
    QVERIFY(
        View.rect().contains(Action->mapTo(&View, Action->rect().center())));
    QTest::mouseClick(Action, Qt::LeftButton);
    auto *TraceNodes = View.findChild<LineagePageView *>("lineageTraceNodes");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && TraceNodes->rows().size() == 3,
                             Timeout);
    QTest::qWait(60);
    QCOMPARE(View.size(), Before);
    QVERIFY(TraceNodes->table()->viewport()->height() >=
            TraceNodes->table()->fontMetrics().height() * 3);
    View.findChild<QTabWidget *>("lineageTabs")->setCurrentIndex(2);
    auto *Graph = View.findChild<LineageGraph *>("lineageGraph");
    QTest::qWait(60);
    bool Found = false;
    for (auto *Item : Graph->scene()->items())
      if (Item->data(0) == "version" && Item->data(1) == ID.toString()) {
        Found = true;
        for (auto *Child : Item->childItems())
          if (auto *Text = qgraphicsitem_cast<QGraphicsTextItem *>(Child)) {
            QVERIFY(Item->boundingRect().contains(
                Text->mapRectToParent(Text->boundingRect())));
            QCOMPARE(Text->toPlainText().count('\n'), 1);
          }
      }
    QVERIFY(Found);
  }

  void privateCompositorResizeAndReopen() {
    if (!QGuiApplication::platformName().startsWith("wayland") ||
        !qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER"))
      QSKIP("Requires the isolated GNOME compositor's trusted pointer.");
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.setWindowTitle("NeverD lineage resize test");
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(View.session().ready(), Timeout);
    QVERIFY(QTest::qWaitForWindowExposed(&View));
    QVERIFY(privateShell(
        QString("Main.overview.hide(); global.neverdLineageWindow = "
                "global.get_window_actors().map(a => a.meta_window).find(w => "
                "w.get_pid() === %1 && w.get_title() === 'NeverD lineage "
                "resize test'); "
                "global.neverdLineageWindow.move_frame(true, 200, 150); "
                "Main.activateWindow(global.neverdLineageWindow);")
            .arg(QCoreApplication::applicationPid())));
    QTest::qWait(750);
    // Finish any overview animation before sending real input.
    QTRY_VERIFY_WITH_TIMEOUT(
        privateShell(
            "Main.overview.hide(); "
            "if (Main.overview.visible) throw Error('overview closing');"),
        Timeout);
    QVERIFY(
        privateShell("global.neverdLineageWindow.move_frame(true, 200, 150); "
                     "Main.activateWindow(global.neverdLineageWindow);"));
    QTest::qWait(100);
    const QSize Before = View.size();
    // The compositor frame excludes the transparent shadow band. Its visible
    // corner receives input; the outer buffer corner deliberately does not.
    QVERIFY(privateShell(
        "(() => { const r = global.neverdLineageWindow.get_frame_rect(); "
        "global.neverdLineageCorner = [r.x + r.width - 3, r.y + r.height "
        "- 3]; })()"));
    const auto Motion = [&](QPoint Delta) {
      return privateShell(
          QString("global.neverdLineagePointer.notify_absolute_motion("
                  "imports.gi.GLib.get_monotonic_time(), "
                  "global.neverdLineageCorner[0] + %1, "
                  "global.neverdLineageCorner[1] + %2)")
              .arg(qRound(Delta.x() * View.devicePixelRatioF()))
              .arg(qRound(Delta.y() * View.devicePixelRatioF())));
    };
    const auto Button = [&](const QString &State) {
      return privateShell(QString("global.neverdLineagePointer.notify_button("
                                  "imports.gi.GLib.get_monotonic_time(), 1, "
                                  "imports.gi.Clutter.ButtonState.%1)")
                              .arg(State));
    };
    QVERIFY(Motion({}));
    QTest::qWait(50);
    QVERIFY(Button("PRESSED"));
    QTest::qWait(50);
    QVERIFY(Motion(QPoint(80, 60)));
    QTest::qWait(100);
    QVERIFY(Button("RELEASED"));
    QTRY_VERIFY_WITH_TIMEOUT(View.width() > Before.width() &&
                                 View.height() > Before.height(),
                             Timeout);
    const auto Chosen = View.size();
    QVERIFY(View.close());
    LineageDialog Reopened(TEST_LINEAGE_WORKER);
    Reopened.setAttribute(Qt::WA_DeleteOnClose, false);
    Reopened.show();
    QVERIFY(QTest::qWaitForWindowExposed(&Reopened));
    QTRY_COMPARE_WITH_TIMEOUT(Reopened.size(), Chosen, Timeout);
  }

  void addReleaseChoosesSnapshotAndKeepsUnknownDate() {
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.show();
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    QSignalSpy Saved(&Session, &LineageSession::saved);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Reports}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Releases = View.findChild<LineagePageView *>("lineageReleases");
    auto *Tabs = View.findChild<QTabWidget *>("lineageTabs");
    auto *Add = View.findChild<QPushButton *>("lineageReleaseAdd");
    QVERIFY(Versions && Releases && Tabs && Add);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 3,
                             Timeout);
    Tabs->setCurrentWidget(View.findChild<QWidget *>("lineageReleaseView"));
    QTRY_VERIFY(Add->isEnabled());
    const auto Before = Session.revision();
    bool Cancelled = false;
    QTimer::singleShot(0, this, [&] {
      auto *D = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      QVERIFY(D);
      const auto Cleanup = qScopeGuard([&] {
        if (D->isVisible())
          D->reject();
      });
      QCOMPARE(D->objectName(), QString("lineageReleaseEditor"));
      QTest::keyClick(D, Qt::Key_Escape);
      Cancelled = true;
    });
    Add->setFocus();
    QTest::keyClick(Add, Qt::Key_Space);
    QVERIFY(Cancelled);
    QCOMPARE(Session.revision(), Before);
    QCOMPARE(Session.summary()["releases"].toInteger(), qint64(0));

    QString ChosenVersion;
    bool Created = false;
    QTimer::singleShot(0, this, [&] {
      auto *D = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      QVERIFY(D);
      const auto Cleanup = qScopeGuard([&] {
        if (D->isVisible())
          D->reject();
      });
      auto *Name = D->findChild<QLineEdit *>("lineageReleaseLabel");
      auto *Branch = D->findChild<QLineEdit *>("lineageReleaseBranch");
      auto *Known = D->findChild<QCheckBox *>("lineageReleaseDateKnown");
      auto *Change =
          D->findChild<QPushButton *>("lineageReleaseChooseSnapshot");
      auto *Buttons = D->findChild<QDialogButtonBox *>();
      QVERIFY(Name && Branch && Known && Change && Buttons);
      QVERIFY(!Known->isChecked());
      QVERIFY(!Buttons->button(QDialogButtonBox::Save)->isEnabled());
      Name->setText("Stable release 新版");
      Branch->setText("stable");
      QTimer::singleShot(0, this, [&] {
        auto *Chooser =
            qobject_cast<QDialog *>(QApplication::activeModalWidget());
        QVERIFY(Chooser && Chooser != D);
        const auto Cleanup = qScopeGuard([&] {
          if (Chooser->isVisible())
            Chooser->reject();
        });
        auto *Choices =
            Chooser->findChild<LineagePageView *>("lineageReleaseChoices");
        QVERIFY(Choices);
        QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Choices->rows().size() == 3,
                                 Timeout);
        Choices->table()->setCurrentIndex(
            Choices->table()->model()->index(1, 0));
        ChosenVersion = Choices->selected()["id"].toString();
        QVERIFY(!ChosenVersion.isEmpty());
        QVERIFY(ChosenVersion != Versions->selected()["id"].toString());
        Choices->table()->setFocus();
        QTest::keyClick(Choices->table(), Qt::Key_Return);
      });
      Change->setFocus();
      QTest::keyClick(Change, Qt::Key_Space);
      QVERIFY(!ChosenVersion.isEmpty());
      QVERIFY(Buttons->button(QDialogButtonBox::Save)->isEnabled());
      Name->setFocus();
      QTest::keyClick(Name, Qt::Key_Return);
      Created = true;
    });
    QTest::keyClick(Add, Qt::Key_Space);
    QVERIFY(Created);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Releases->rows().size() == 1,
                             Timeout);
    const auto Release = Releases->selected();
    QCOMPARE(Release["label"].toString(), QString("Stable release 新版"));
    QCOMPARE(Release["branch"].toString(), QString("stable"));
    QCOMPARE(Release["version"].toString(), ChosenVersion);
    QVERIFY(Release["published_at"].isNull());
    QCOMPARE(Release["parent_count"].toInteger(), qint64(0));
    QVERIFY(!Release["id"].toString().isEmpty());
    const auto Path = Directory.filePath("created-release.ndlineage");
    Session.save(Path);
    QTRY_COMPARE_WITH_TIMEOUT(Saved.size(), 1, Timeout);
    const auto Document = QJsonDocument::fromJson(read(Path)).object();
    QCOMPARE(Document["schema_version"], QJsonValue(2));
    const auto Persisted = Document["releases"].toArray().first().toObject();
    QCOMPARE(Persisted["id"], Release["id"]);
    QCOMPARE(Persisted["version"].toString(), ChosenVersion);
    QVERIFY(Persisted["published_at"].isNull());
    QCOMPARE(Errors.size(), 0);
  }

  void releaseEditsKeepSelectionAcrossPagesAndParentNavigation() {
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.show();
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Reports}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Releases = View.findChild<LineagePageView *>("lineageReleases");
    auto *Parents = View.findChild<LineagePageView *>("lineageReleaseParents");
    auto *Tabs = View.findChild<QTabWidget *>("lineageTabs");
    auto *Relations = View.findChild<QTabWidget *>("lineageReleaseRelations");
    QVERIFY(Versions && Releases && Parents && Tabs && Relations);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 3,
                             Timeout);
    const auto Version = Versions->selected()["id"].toString();
    Releases->findChild<QSpinBox *>("lineagePageRows")->setValue(2);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QJsonArray Rows;
    for (const auto &ID :
         {QString("a"), QString("b"), QString("c"), QString("d")}) {
      const QString Date =
          ID == "a" ? QString()
                    : "2025-0" + QString::number(ID[0].unicode() - 'a' + 1) +
                          "-01T00:00:00Z";
      Rows.append(QJsonObject{
          {"id", ID},
          {"label", "Same label"},
          {"branch", ID},
          {"version", Version},
          {"published_at",
           Date.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(Date)},
          {"parents", QJsonArray{}}});
    }
    Session.editReleases({{"upsert", Rows}}, {}, "d");
    QTRY_VERIFY_WITH_TIMEOUT(
        !Session.busy() && Releases->selected()["id"] == "d", Timeout);
    QCOMPARE(Releases->offset(), qint64(2));
    Tabs->setCurrentWidget(View.findChild<QWidget *>("lineageReleaseView"));
    bool Edited = false;
    QTimer::singleShot(0, this, [&] {
      auto *D = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      QVERIFY(D);
      const auto Cleanup = qScopeGuard([&] {
        if (D->isVisible())
          D->reject();
      });
      QCOMPARE(D->objectName(), QString("lineageReleaseEditor"));
      auto *Name = D->findChild<QLineEdit *>("lineageReleaseLabel");
      auto *Date = D->findChild<QDateTimeEdit *>("lineageReleaseDate");
      auto *Buttons = D->findChild<QDialogButtonBox *>();
      QVERIFY(Name && Date && Buttons);
      Name->setText("Renamed release 中文");
      Date->setDateTime(
          QDateTime::fromString("2024-01-01T00:00:00Z", Qt::ISODate));
      QTest::mouseClick(Buttons->button(QDialogButtonBox::Save),
                        Qt::LeftButton);
      Edited = true;
    });
    QTest::mouseClick(View.findChild<QPushButton *>("lineageReleaseEdit"),
                      Qt::LeftButton);
    QVERIFY(Edited);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Releases->selected()["label"] ==
                                                    "Renamed release 中文",
                             Timeout);
    QCOMPARE(Releases->offset(), qint64(0));
    QCOMPARE(Releases->selected()["id"].toString(), QString("d"));
    Relations->setCurrentIndex(1);
    bool Chosen = false;
    QTimer::singleShot(0, this, [&] {
      auto *D = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      QVERIFY(D);
      const auto Cleanup = qScopeGuard([&] {
        if (D->isVisible())
          D->reject();
      });
      auto *Choices = D->findChild<LineagePageView *>("lineageReleaseChoices");
      auto *Buttons = D->findChild<QDialogButtonBox *>();
      QVERIFY(Choices && Buttons);
      QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Choices->rows().size() == 4,
                               Timeout);
      for (int I = 0; I < Choices->rows().size(); ++I)
        if (Choices->rows()[I].toObject()["id"] == "a") {
          Choices->table()->setCurrentIndex(
              Choices->table()->model()->index(I, 0));
          break;
        }
      QCOMPARE(Choices->selected()["id"].toString(), QString("a"));
      QCOMPARE(Choices->selected()["label"].toString(), QString("Same label"));
      QVERIFY(Buttons->button(QDialogButtonBox::Ok)->isEnabled());
      QTest::mouseClick(Buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
      Chosen = true;
    });
    QTest::mouseClick(View.findChild<QPushButton *>("lineageReleaseAddParent"),
                      Qt::LeftButton);
    QVERIFY(Chosen);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Parents->rows().size() == 1,
                             Timeout);
    QCOMPARE(Releases->selected()["id"].toString(), QString("d"));
    QCOMPARE(Parents->selected()["id"].toString(), QString("a"));
    QCOMPARE(Errors.size(), 0);
    QTest::mouseClick(
        View.findChild<QPushButton *>("lineageReleaseRemoveParent"),
        Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Parents->rows().isEmpty(),
                             Timeout);
    QCOMPARE(Releases->selected()["id"].toString(), QString("d"));
    QCOMPARE(Releases->offset(), qint64(0));
    Relations->setCurrentIndex(0);
    Releases->findChild<QSpinBox *>("lineagePageRows")->setValue(1);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    Releases->setQuery({{"kind", "releases"}, {"focus", "a"}}, "a");
    QTRY_VERIFY_WITH_TIMEOUT(
        !Session.busy() && Releases->selected()["id"] == "a", Timeout);
    QCOMPARE(Releases->offset(), qint64(3));
    QTimer::singleShot(0, this, [&] {
      auto *D = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
      QVERIFY(D);
      QTest::mouseClick(D->button(QMessageBox::Yes), Qt::LeftButton);
    });
    QTest::mouseClick(View.findChild<QPushButton *>("lineageReleaseRemove"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() &&
                                 Session.summary()["releases"].toInteger() == 3,
                             Timeout);
    QCOMPARE(Releases->offset(), qint64(0));
    QCOMPARE(Releases->selected()["id"].toString(), QString("d"));
    QTest::mouseClick(View.findChild<QPushButton *>("lineageReleaseSnapshot"),
                      Qt::LeftButton);
    auto *Functions = View.findChild<LineagePageView *>("lineageFunctions");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() &&
                                 Functions->selected()["version"] == Version,
                             Timeout);
    QCOMPARE(Tabs->currentIndex(), 0);
    QCOMPARE(Errors.size(), 0);
  }

  void releaseLabelEditsKeepAllParentsAndTheirCurrentPage() {
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.show();
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    Session.load({{"reports", Reports}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Releases = View.findChild<LineagePageView *>("lineageReleases");
    auto *Parents = View.findChild<LineagePageView *>("lineageReleaseParents");
    auto *Tabs = View.findChild<QTabWidget *>("lineageTabs");
    auto *Relations = View.findChild<QTabWidget *>("lineageReleaseRelations");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Versions->rows().size() == 3,
                             Timeout);
    const auto Version = Versions->selected()["id"].toString();
    QJsonArray Records, IDs;
    for (unsigned I = 0; I < 257; ++I) {
      const auto ID = QString("parent-%1").arg(I, 3, 10, QChar('0'));
      IDs.append(ID);
      Records.append(QJsonObject{{"id", ID},
                                 {"label", "Duplicate label"},
                                 {"branch", ID},
                                 {"version", Version},
                                 {"published_at", QJsonValue::Null},
                                 {"parents", QJsonArray{}}});
    }
    Records.append(QJsonObject{{"id", "child"},
                               {"label", "Child"},
                               {"branch", "stable"},
                               {"version", Version},
                               {"published_at", QJsonValue::Null},
                               {"parents", IDs}});
    Session.editReleases({{"upsert", Records}}, {}, "child");
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() &&
                                 Releases->selected()["id"] == "child" &&
                                 Parents->rows().size() == 200,
                             Timeout);
    QCOMPARE(Releases->offset(), qint64(200));
    Tabs->setCurrentWidget(View.findChild<QWidget *>("lineageReleaseView"));
    QTimer::singleShot(0, this, [&] {
      auto *D = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      QVERIFY(D);
      const auto Cleanup = qScopeGuard([&] {
        if (D->isVisible())
          D->reject();
      });
      auto *Name = D->findChild<QLineEdit *>("lineageReleaseLabel");
      auto *Buttons = D->findChild<QDialogButtonBox *>();
      QVERIFY(Name && Buttons);
      Name->setText("All 257 parents retained");
      QTest::mouseClick(Buttons->button(QDialogButtonBox::Save),
                        Qt::LeftButton);
    });
    QTest::mouseClick(View.findChild<QPushButton *>("lineageReleaseEdit"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Releases->selected()["label"] ==
                                                    "All 257 parents retained",
                             Timeout);
    QCOMPARE(Releases->selected()["parent_count"].toInteger(), qint64(257));
    QCOMPARE(Releases->selected()["id"].toString(), QString("child"));
    Relations->setCurrentIndex(1);
    QTest::mouseClick(Parents->findChild<QToolButton *>("lineageNextPage"),
                      Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Parents->rows().size() == 57,
                             Timeout);
    QCOMPARE(Parents->offset(), qint64(200));
    QCOMPARE(Parents->selected()["id"].toString(), QString("parent-200"));
    QTest::mouseClick(
        View.findChild<QPushButton *>("lineageReleaseRemoveParent"),
        Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(
        !Session.busy() &&
            Releases->selected()["parent_count"].toInteger() == 256 &&
            Parents->rows().size() == 56,
        Timeout);
    QCOMPARE(Parents->offset(), qint64(200));
    QCOMPARE(Parents->selected()["id"].toString(), QString("parent-201"));
    QCOMPARE(Errors.size(), 0);
  }

  void legacyWorkerKeepsExistingGraphActions() {
    const auto Worker = qEnvironmentVariable("NEVERD_LEGACY_LINEAGE_WORKER");
    if (Worker.isEmpty())
      QSKIP("Set NEVERD_LEGACY_LINEAGE_WORKER to a retained v1 worker or a "
            "current worker using the retained v1 engine");
    // The retained engine must produce its own supported report schema.
    // A current comparison may contain evidence that did not exist in v1.
    ComparisonSession Source(Worker, this);
    QSignalSpy SourceErrors(&Source, &ComparisonSession::failed);
    QTRY_VERIFY_WITH_TIMEOUT(Source.ready(), Timeout);
    QList<QJsonObject> Inputs;
    for (int I = 1; I <= 3; ++I)
      Inputs.append(
          {{"path", Directory.filePath(QString("release-%1.raw").arg(I))},
           {"load_options", QJsonObject{{"loader", "binary"},
                                        {"processor", "x86_64"},
                                        {"platform", "sysv"},
                                        {"base", "0x1000"}}}});
    QJsonArray LegacyReports;
    for (auto Pair : {qMakePair(0, 1), qMakePair(1, 2), qMakePair(0, 2)}) {
      Source.start(Inputs[Pair.first], Inputs[Pair.second]);
      QTRY_VERIFY_WITH_TIMEOUT(!Source.busy() && !Source.revision().isEmpty(),
                               Timeout);
      QCOMPARE(SourceErrors.size(), 0);
      const auto Path = Directory.filePath(
          QString("legacy-%1-%2.nddiff").arg(Pair.first).arg(Pair.second));
      bool Saved = false;
      Source.request("comparison_save", {{"path", Path}}, this,
                     [&](const QJsonObject &R) {
                       Saved = R["status"] == QJsonValue("ok");
                     });
      QTRY_VERIFY_WITH_TIMEOUT(Saved, Timeout);
      LegacyReports.append(Path);
    }
    LineageDialog View(Worker);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.show();
    auto &Session = View.session();
    QSignalSpy Errors(&Session, &LineageSession::failed);
    QSignalSpy Saved(&Session, &LineageSession::saved);
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), Timeout);
    QVERIFY(!Session.releaseEditingSupported());
    Session.load({{"reports", LegacyReports}});
    auto *Versions = View.findChild<LineagePageView *>("lineageVersions");
    auto *Functions = View.findChild<LineagePageView *>("lineageFunctions");
    auto *Tabs = View.findChild<QTabWidget *>("lineageTabs");
    QVERIFY(Versions && Functions && Tabs);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && !Functions->rows().isEmpty(),
                             Timeout);
    QVERIFY(!Versions->selected().isEmpty());
    const auto Revision = Session.revision();
    Tabs->setCurrentWidget(View.findChild<QWidget *>("lineageReleaseView"));
    for (const auto *Name :
         {"lineageReleaseAdd", "lineageReleaseEdit", "lineageReleaseRemove",
          "lineageReleaseAddParent"}) {
      auto *Action = View.findChild<QPushButton *>(Name);
      QVERIFY(Action);
      QVERIFY(!Action->isEnabled());
    }
    auto *Status = View.findChild<QLabel *>("lineageReleaseStatus");
    QVERIFY(Status);
    QVERIFY(Status->text().contains("does not support"));
    const auto Path = Directory.filePath("legacy-worker.ndlineage");
    Session.save(Path);
    QTRY_COMPARE_WITH_TIMEOUT(Saved.size(), 1, Timeout);
    QCOMPARE(QJsonDocument::fromJson(read(Path)).object()["schema_version"],
             QJsonValue(1));
    QCOMPARE(Session.revision(), Revision);
    QVERIFY(!Session.summary()["supports_all_region_anchors"].toBool());
    auto *Inspect = View.findChild<QPushButton *>("lineageInspectSource");
    QVERIFY(Inspect);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy() && Inspect->isEnabled(), Timeout);
    Inspect->click();
    auto *Evidence = View.findChild<QDialog *>("lineageSourceEvidence");
    QVERIFY(Evidence);
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), Timeout);
    QVERIFY(Evidence->findChild<LineagePageView *>("native_regions"));
    QVERIFY(!Evidence->findChild<LineagePageView *>("native_anchors"));
    QVERIFY(!Evidence->findChild<LineagePageView *>("flow_anchors"));
    QCOMPARE(Errors.size(), 0);
  }

  void invalidResourceSettingsKeepTheDialogAndInput() {
    LineageDialog View(TEST_LINEAGE_WORKER);
    View.setAttribute(Qt::WA_DeleteOnClose, false);
    View.show();
    QTRY_VERIFY_WITH_TIMEOUT(View.session().ready(), Timeout);
    bool Checked = false;
    QTimer::singleShot(0, this, [&] {
      auto *D = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!D)
        return;
      auto *Nodes = D->findChild<QLineEdit *>("lineageOption_max_nodes");
      auto *Buttons = D->findChild<QDialogButtonBox *>();
      if (!Nodes || !Buttons) {
        D->reject();
        return;
      }
      Nodes->setText("0");
      QTest::mouseClick(Buttons->button(QDialogButtonBox::Save),
                        Qt::LeftButton);
      Checked =
          D->isVisible() && Nodes->text() == "0" &&
          !D->findChild<QLabel *>("lineageOptionsError")->text().isEmpty();
      D->reject();
    });
    QTest::mouseClick(View.findChild<QPushButton *>("lineageOptions"),
                      Qt::LeftButton);
    QVERIFY(Checked);
  }
};
int main(int argc, char **argv) {
  WindowFrame::prepareNativeWindows();
  QApplication App(argc, argv);
  WindowFrame::frameDialogs(App);
  LineageTests Tests;
  return QTest::qExec(&Tests, argc, argv);
}
#include "LineageTests.moc"
