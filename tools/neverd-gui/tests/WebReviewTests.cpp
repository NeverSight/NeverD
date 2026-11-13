//===- WebReviewTests.cpp - Offline package GUI contracts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// C++ journal, model and production worker cases. No target runtime or script
/// is used to generate fixtures or analyze evidence.
///
//===----------------------------------------------------------------------===//

#include "Docking.h"
#include "MainWindow.h"
#include "PackageArchiveFixture.h"
#include "Session.h"
#include "Theme.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"
#include "web/FindingModel.h"
#include "web/PackageReviewDialog.h"
#include "web/PackageSession.h"
#include "web/ReviewJournal.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFile>
#include <QFileDialog>
#include <QFontInfo>
#include <QGroupBox>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace neverd::gui::web;

namespace {
QString id(char C) { return QString(64, QLatin1Char(C)); }
QJsonObject summary() {
  return {{"package_assessment_id", id('a')},
          {"review_root_id", id('b')},
          {"review_head_id", id('d')}};
}
QJsonArray decisions() {
  return {QJsonObject{{"decision_id", id('c')},
                      {"package_assessment_id", id('a')},
                      {"finding_id", id('e')},
                      {"parent_head_id", id('b')},
                      {"disposition", "needs_review"},
                      {"decision_index", 0},
                      {"assertion_source", "caller"}},
          QJsonObject{{"decision_id", id('d')},
                      {"package_assessment_id", id('a')},
                      {"finding_id", id('e')},
                      {"parent_head_id", id('c')},
                      {"disposition", "false_positive"},
                      {"decision_index", 1},
                      {"assertion_source", "caller"}}};
}
bool write(const QString &Path, const QByteArray &Bytes) {
  QFile File(Path);
  return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size();
}
QString artifact(const PackageSession &Session, int Index) {
  return Session.artifacts()[Index].toObject()["artifact_id"].toString();
}
constexpr int TimeoutMs = 15000;
} // namespace

class WebReviewTests : public QObject {
  Q_OBJECT
  bool WebAvailable = false;
private slots:
  void initTestCase() {
    neverd::gui::configureDocking();
    neverd::gui::Theme::instance().apply();
    PackageSession Probe(QString::fromLocal8Bit(TEST_WEB_WORKER));
    QTRY_VERIFY_WITH_TIMEOUT(!Probe.busy(), TimeoutMs);
    WebAvailable = Probe.ready();
    if (!WebAvailable)
      QCOMPARE(Probe.error(), QStringLiteral("capability_unavailable"));
  }
  void journalRoundTripAndAtomicRefusal() {
    QTemporaryDir Directory;
    const auto Path = Directory.filePath(QStringLiteral("review.ndreview"));
    const auto Journal = ReviewJournal::create(summary(), decisions());
    QString Error;
    QVERIFY(ReviewJournal::save(Path, Journal, Error));
    QVERIFY(Error.isEmpty());
    const auto Loaded = ReviewJournal::load(Path, Error);
    QVERIFY(Loaded);
    QCOMPARE(*Loaded, Journal);
    auto Invalid = Journal;
    Invalid["notes"] = "PRIVATE_NOTE";
    QVERIFY(!ReviewJournal::save(Path, Invalid, Error));
    QCOMPARE(Error, QStringLiteral("invalid_review_journal"));
    QCOMPARE(*ReviewJournal::load(Path, Error), Journal);
    QFile File(Path);
    QVERIFY(File.open(QIODevice::ReadOnly));
    QVERIFY(!File.readAll().contains("PRIVATE_NOTE"));
    QVERIFY(!ReviewJournal::save(
        Directory.filePath(QStringLiteral("missing/review")), Journal, Error));
  }

  void journalSchemaAndChainRefusals_data() {
    QTest::addColumn<int>("Mutation");
    for (int I = 0; I < 8; ++I)
      QTest::newRow(qPrintable(QString::number(I))) << I;
  }
  void journalSchemaAndChainRefusals() {
    QFETCH(int, Mutation);
    auto Journal = ReviewJournal::create(summary(), decisions());
    auto Rows = Journal["decisions"].toArray();
    auto Row = Rows[1].toObject();
    switch (Mutation) {
    case 0:
      Journal["profile"] = "future";
      break;
    case 1:
      Journal["schema_version"] = 2;
      break;
    case 2:
      Row["disposition"] = "safe";
      break;
    case 3:
      Row["parent_head_id"] = id('b');
      break;
    case 4:
      Row["decision_index"] = 0;
      break;
    case 5:
      Row["assertion_source"] = "analyzer";
      break;
    case 6:
      Row["finding_id"] = QString(64, u'G');
      break;
    case 7:
      Row["notes"] = "PRIVATE_NOTE";
      break;
    }
    Rows[1] = Row;
    Journal["decisions"] = Rows;
    QVERIFY(!ReviewJournal::valid(Journal));
  }

  void journalDuplicateAndByteBudgets() {
    QTemporaryDir Directory;
    const auto Path = Directory.filePath(QStringLiteral("review.ndreview"));
    const auto Journal = ReviewJournal::create(summary(), decisions());
    auto Bytes = QJsonDocument(Journal).toJson(QJsonDocument::Compact);
    Bytes.replace("\"schema_version\":1",
                  "\"schema_version\":1,\"schema_version\":1");
    QVERIFY(write(Path, Bytes));
    QString Error;
    QVERIFY(!ReviewJournal::load(Path, Error));
    QCOMPARE(Error, QStringLiteral("invalid_review_journal"));
    QVERIFY(write(Path, QByteArray(MaxJournalBytes + 1, 'x')));
    QVERIFY(!ReviewJournal::load(Path, Error));
    QCOMPARE(Error, QStringLiteral("review_journal_byte_budget_exceeded"));
    auto TooMany = Journal;
    QJsonArray Rows;
    for (int I = 0; I < 257; ++I)
      Rows.append(decisions()[0]);
    TooMany["decisions"] = Rows;
    QVERIFY(!ReviewJournal::valid(TooMany));
  }

  void modelKeepsEvidenceSeparateFromAssertions() {
    FindingModel Model;
    QJsonObject Finding{{"finding_id", id('e')},
                        {"kind", "coverage"},
                        {"evidence_status", "not_selected"},
                        {"binding", "none"},
                        {"confidence", "coverage_only"},
                        {"disposition", "accepted_risk"},
                        {"disposition_source", "caller_assertion"}};
    Model.setRows({Finding});
    QCOMPARE(Model.rowCount(), 1);
    QCOMPARE(Model.columnCount(), 7);
    QCOMPARE(Model.data(Model.index(0, 3), Qt::DisplayRole).toString(),
             QStringLiteral("coverage_only"));
    QCOMPARE(Model.data(Model.index(0, 4), Qt::DisplayRole).toString(),
             QStringLiteral("accepted_risk"));
    QVERIFY(!Model.flags(Model.index(0, 4)).testFlag(Qt::ItemIsEditable));
    QVERIFY(Model.finding(-1).isEmpty());
    Model.setRows({});
    QCOMPARE(Model.rowCount(), 0);
  }

  void productionWorkerPagesAndRestartRestore() {
    if (!WebAvailable)
      QSKIP("Offline backend omitted");
    QTemporaryDir Directory;
    QVERIFY(write(Directory.filePath(QStringLiteral("a.package.json")),
                  R"({"name":"PRIVATE_NAME","version":"1.0.0"})"));
    for (int I = 0; I < 140; ++I)
      QVERIFY(write(Directory.filePath(QStringLiteral("z.%1.txt").arg(I)),
                    "PRIVATE_BODY"));
    PackageSession Session(QString::fromLocal8Bit(TEST_WEB_WORKER));
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), TimeoutMs);
    QVERIFY(Session.canRestore());
    Session.previewCapture(Directory.path());
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    QVERIFY(!Session.preview().isEmpty());
    Session.commitPreview();
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    QCOMPARE(Session.artifacts().size(), 142);
    Session.assess(artifact(Session, 1), QStringLiteral("package-json"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    QVERIFY2(!Session.findings().isEmpty(), qPrintable(Session.error()));
    const auto Finding =
        Session.findings()[0].toObject()["finding_id"].toString();
    Session.review(Finding, QStringLiteral("needs_review"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    Session.review(Finding, QStringLiteral("false_positive"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    const auto Saved = Session.journal();
    QCOMPARE(Saved["decisions"].toArray().size(), 2);
    QVERIFY(!QJsonDocument(Saved).toJson().contains("PRIVATE"));
    Session.cancel();
    PackageSession Fresh(QString::fromLocal8Bit(TEST_WEB_WORKER));
    QTRY_VERIFY_WITH_TIMEOUT(Fresh.ready(), TimeoutMs);
    Fresh.previewCapture(Directory.path());
    QTRY_VERIFY_WITH_TIMEOUT(!Fresh.busy(), TimeoutMs);
    Fresh.commitPreview();
    QTRY_VERIFY_WITH_TIMEOUT(!Fresh.busy(), TimeoutMs);
    Fresh.assess(artifact(Fresh, 1), QStringLiteral("package-json"));
    QTRY_VERIFY_WITH_TIMEOUT(!Fresh.busy(), TimeoutMs);
    QCOMPARE(Fresh.assessment()["package_assessment_id"],
             Saved["package_assessment_id"]);
    const auto Root = Fresh.assessment()["review_head_id"];
    auto Tampered = Saved;
    auto Rows = Tampered["decisions"].toArray();
    auto Last = Rows[1].toObject();
    Last["decision_id"] = id('f');
    Rows[1] = Last;
    Tampered["decisions"] = Rows;
    Tampered["review_head_id"] = id('f');
    QVERIFY(ReviewJournal::valid(Tampered));
    Fresh.restore(Tampered);
    QTRY_VERIFY_WITH_TIMEOUT(!Fresh.busy(), TimeoutMs);
    QCOMPARE(Fresh.error(), QStringLiteral("package_review_chain_mismatch"));
    QCOMPARE(Fresh.assessment()["review_head_id"], Root);
    QCOMPARE(Fresh.assessment()["decision_count"].toInteger(), 0);
    Fresh.restore(Saved);
    QTRY_VERIFY_WITH_TIMEOUT(!Fresh.busy(), TimeoutMs);
    QVERIFY2(Fresh.error().isEmpty(), qPrintable(Fresh.error()));
    QCOMPARE(Fresh.journal(), Saved);
    Fresh.restore(Saved);
    QTRY_VERIFY_WITH_TIMEOUT(!Fresh.busy(), TimeoutMs);
    QCOMPARE(Fresh.journal(), Saved);
  }

  void productionArchiveAndDeclaredRegistryEvidence() {
    if (!WebAvailable)
      QSKIP("Offline backend omitted");
    QTemporaryDir Directory;
    const auto Tar = neverd::web::test::finishTar(neverd::web::test::tarMember(
        "package/package.json",
        R"({"name":"PRIVATE_NAME","version":"1.0.0"})"));
    const auto Bytes = QByteArray::fromStdString(Tar);
    QVERIFY(write(Directory.filePath(QStringLiteral("a.package.tar")), Bytes));
    const auto Digest =
        QCryptographicHash::hash(Bytes, QCryptographicHash::Sha512).toBase64();
    QVERIFY(write(Directory.filePath(QStringLiteral("b.registry.json")),
                  "{\"name\":\"PRIVATE_NAME\",\"version\":\"1.0.0\",\"dist\":{"
                  "\"integrity\":\"sha512-" +
                      Digest + "\"}}"));
    QVERIFY(write(
        Directory.filePath(QStringLiteral("c.descriptor.json")),
        R"({"schema_version":1,"source":"PRIVATE_SOURCE","retrieved_at":"2026-10-12T01:00:00Z"})"));
    PackageSession Session(QString::fromLocal8Bit(TEST_WEB_WORKER));
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), TimeoutMs);
    Session.previewCapture(Directory.path());
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    Session.commitPreview();
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    Session.extractArchive(artifact(Session, 1), QStringLiteral("tar"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    QVERIFY2(!Session.members().isEmpty(), qPrintable(Session.error()));
    Session.assess(
        Session.members()[0].toObject()["member_id"].toString(),
        QStringLiteral("package-json"),
        {artifact(Session, 1), artifact(Session, 2), artifact(Session, 3)});
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    QVERIFY2(!Session.findings().isEmpty(), qPrintable(Session.error()));
    QVERIFY(Session.assessment()["publisher_authenticated"] ==
            QJsonValue(false));
    QVERIFY(!QJsonDocument(Session.findings()).toJson().contains("PRIVATE"));
    bool FoundDigest = false;
    for (const auto &V : Session.findings()) {
      const auto Row = V.toObject();
      if (Row["kind"] == QJsonValue("integrity")) {
        QCOMPARE(Row["evidence_status"].toString(), QStringLiteral("match"));
        QCOMPARE(Row["binding"].toString(),
                 QStringLiteral("original_contains_declared_root_manifest"));
        FoundDigest = true;
      }
    }
    QVERIFY(FoundDigest);
  }

  void changedEvidenceCannotInheritReview() {
    if (!WebAvailable)
      QSKIP("Offline backend omitted");
    QTemporaryDir Directory;
    const auto Path = Directory.filePath(QStringLiteral("a.package.json"));
    QVERIFY(write(Path, R"({"name":"PRIVATE_NAME","version":"1.0.0"})"));
    PackageSession Session(QString::fromLocal8Bit(TEST_WEB_WORKER));
    QTRY_VERIFY_WITH_TIMEOUT(Session.ready(), TimeoutMs);
    Session.previewCapture(Directory.path());
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    Session.commitPreview();
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    Session.assess(artifact(Session, 1), QStringLiteral("package-json"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    const auto Old = Session.journal();
    QVERIFY(write(Path, R"({"name":"PRIVATE_NAME","version":"2.0.0"})"));
    Session.previewCapture(Directory.path());
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    Session.commitPreview();
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    Session.assess(artifact(Session, 1), QStringLiteral("package-json"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    QVERIFY(Old["package_assessment_id"] !=
            Session.assessment()["package_assessment_id"]);
    Session.restore(Old);
    QCOMPARE(Session.error(), QStringLiteral("review_journal_owner_mismatch"));
    QCOMPARE(Session.assessment()["decision_count"].toInteger(), 0);
  }

  void widgetLayoutAndStopStayAccessible() {
    auto *Dialog =
        new PackageReviewDialog(QString::fromLocal8Bit(TEST_WEB_WORKER));
    Dialog->resize(640, 720);
    Dialog->setLayoutDirection(Qt::RightToLeft);
    auto Font = Dialog->font();
    Font.setPointSize(16);
    Dialog->setFont(Font);
    Dialog->show();
    auto *Table =
        Dialog->findChild<QTableView *>(QStringLiteral("packageFindings"));
    auto *Details = Dialog->findChild<QPlainTextEdit *>(
        QStringLiteral("packageFindingDetails"));
    QVERIFY(Table);
    QVERIFY(Details && Details->isReadOnly());
    QVERIFY(
        Dialog->findChild<QComboBox *>(QStringLiteral("packageDisposition")));
    QTRY_VERIFY_WITH_TIMEOUT(!Dialog->findChild<PackageSession *>()->busy(),
                             TimeoutMs);
    QCOMPARE(Dialog->findChild<PackageSession *>()->ready(), WebAvailable);
    for (auto *Button : Dialog->findChildren<QPushButton *>())
      QVERIFY(Button->focusPolicy() != Qt::NoFocus);
    Dialog->findChild<PackageSession *>()->cancel();
    QVERIFY(!Dialog->findChild<PackageSession *>()->ready());
    QVERIFY(Dialog->close());
  }

  void theWindowKeepsTheWorkbenchsManners() {
    // "Preview file…" was the window's default by Qt's choice of its first
    // push button, so Return in the finding filter opened a file chooser.
    // The window also set a font of at least 12 points where every other
    // takes the desktop's size, and framed registry fields it did not show.
    auto *Dialog =
        new PackageReviewDialog(QString::fromLocal8Bit(TEST_WEB_WORKER));
    Dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(Dialog));
    QTRY_VERIFY_WITH_TIMEOUT(!Dialog->findChild<PackageSession *>()->busy(),
                             TimeoutMs);
    bool Opened = false;
    QTimer Watch;
    connect(&Watch, &QTimer::timeout, Dialog, [&Opened] {
      if (auto *Chooser =
              qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
        Opened = true;
        Chooser->reject();
      }
    });
    Watch.start(10);
    auto *Filter =
        Dialog->findChild<QLineEdit *>(QStringLiteral("packageFindingFilter"));
    QVERIFY(Filter);
    Filter->setFocus();
    QTest::keyClick(Filter, Qt::Key_Return);
    QTest::qWait(100);
    QVERIFY(!Opened);
    QVERIFY(Dialog->isVisible());
    // No button is the default.  The step every review starts with wears
    // the accent all the same: no window is left in one color.
    QStringList Accented;
    for (const auto *Button : Dialog->findChildren<QPushButton *>()) {
      QVERIFY2(!Button->isDefault() && !Button->autoDefault(),
               qPrintable(Button->text()));
      if (Button->property("mainAction").toBool())
        Accented.append(Button->text());
    }
    QCOMPARE(Accented, QStringList{QStringLiteral("Preview file…")});
    QCOMPARE(QFontInfo(Dialog->font()).pointSizeF(),
             QFontInfo(QApplication::font()).pointSizeF());
    QVERIFY(Dialog->findChild<QGroupBox *>(QStringLiteral("packageRegistry"))
                ->isFlat());
    Dialog->findChild<PackageSession *>()->cancel();
    QVERIFY(Dialog->close());
  }

  void capturedClaudePackageWhenSupplied() {
    if (!WebAvailable)
      QSKIP("Offline backend omitted");
    const auto Base =
        qEnvironmentVariable("NEVERD_CLAUDE_CODE_21296_CROSS_DIR");
    if (Base.isEmpty())
      QSKIP("Captured Claude platform packages not supplied");
    auto *Dialog =
        new PackageReviewDialog(QString::fromLocal8Bit(TEST_WEB_WORKER));
    auto *Session = Dialog->findChild<PackageSession *>();
    QTRY_VERIFY_WITH_TIMEOUT(Session->ready(), TimeoutMs);
    Session->previewCapture(Base + QStringLiteral("/darwin-arm64/input"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session->busy(), TimeoutMs);
    Session->commitPreview();
    QTRY_VERIFY_WITH_TIMEOUT(!Session->busy(), TimeoutMs);
    QCOMPARE(Session->artifacts().size(), 4);
    QCOMPARE(Session->artifacts()[1].toObject()["blob_sha256"].toString(),
             QStringLiteral("50fbf594d6526c9aae7d66b8b45a08d89ae6b863e2bc9777bf"
                            "b277d6521a7458"));
    auto *Artifact =
        Dialog->findChild<QComboBox *>(QStringLiteral("packageArtifact"));
    Artifact->setCurrentIndex(Artifact->findData(artifact(*Session, 1)));
    auto *Container =
        Dialog->findChild<QComboBox *>(QStringLiteral("packageContainer"));
    Container->setCurrentIndex(Container->findData(QStringLiteral("tgz")));
    auto *Extract =
        Dialog->findChild<QPushButton *>(QStringLiteral("packageExtract"));
    QVERIFY(Extract->isEnabled());
    Extract->click();
    QTRY_VERIFY_WITH_TIMEOUT(!Session->busy(), TimeoutMs);
    QCOMPARE(Session->members().size(), 4);
    auto *Member =
        Dialog->findChild<QComboBox *>(QStringLiteral("packageMember"));
    Member->setCurrentIndex(Member->findData(
        Session->members()[1].toObject()["member_id"].toString()));
    Dialog->findChild<QGroupBox *>(QStringLiteral("packageRegistry"))
        ->setChecked(true);
    for (const auto &[Name, Index] :
         {std::pair{"packageRegistryOriginal", 1},
          std::pair{"packageRegistryMetadata", 2},
          std::pair{"packageRegistryDescriptor", 3}}) {
      auto *Combo = Dialog->findChild<QComboBox *>(QString::fromLatin1(Name));
      Combo->setCurrentIndex(Combo->findData(artifact(*Session, Index)));
    }
    auto *Assess =
        Dialog->findChild<QPushButton *>(QStringLiteral("packageAssess"));
    QVERIFY(Assess->isEnabled());
    Assess->click();
    QTRY_VERIFY_WITH_TIMEOUT(!Session->busy(), TimeoutMs);
    QVERIFY2(!Session->findings().isEmpty(), qPrintable(Session->error()));
    QVERIFY(!Session->assessment()["publisher_authenticated"].toBool());
    QCOMPARE(Session->assessment()["decision_count"].toInteger(), 0);
    QJsonArray Integrity;
    for (const auto &V : Session->findings())
      if (V.toObject()["kind"] == QJsonValue("integrity"))
        Integrity.append(V);
    QCOMPARE(Integrity.size(), 1);
    QCOMPARE(Integrity[0].toObject()["evidence_status"].toString(),
             QStringLiteral("match"));
    QCOMPARE(Integrity[0].toObject()["digest_algorithm"].toString(),
             QStringLiteral("sha512"));
    QCOMPARE(Integrity[0].toObject()["binding"].toString(),
             QStringLiteral("original_contains_declared_root_manifest"));
    Dialog->show();
    QCoreApplication::processEvents();
    const auto ReceiptPath = qEnvironmentVariable("NEVERD_GUI_WEB_RECEIPT");
    if (!ReceiptPath.isEmpty()) {
      QVERIFY(write(ReceiptPath, QJsonDocument(Session->assessment())
                                     .toJson(QJsonDocument::Indented)));
      QVERIFY(Dialog->grab().save(ReceiptPath + QStringLiteral(".png")));
    }
    QVERIFY(Dialog->close());
  }
  void unsavedAssertionsSurviveEscapeAndWorkerStop() {
    if (!WebAvailable)
      QSKIP("Offline backend omitted");
    QTemporaryDir Directory;
    QVERIFY(write(Directory.filePath(QStringLiteral("a.package.json")),
                  R"({"name":"PRIVATE_NAME","version":"1.0.0"})"));
    neverd::gui::Session Native(QString::fromLocal8Bit(TEST_WEB_WORKER));
    McpConnectionManager MCP;
    GuiSessionBroker Broker;
    neverd::gui::MainWindow Parent(Native, MCP, Broker);
    Parent.setLayoutPath(Directory.filePath(QStringLiteral("layout.json")));
    Parent.show();
    Parent.actions()
        .action(neverd::gui::ActionId::FilePackageReview)
        ->trigger();
    QPointer<PackageReviewDialog> Dialog =
        Parent.findChild<PackageReviewDialog *>();
    QVERIFY(Dialog);
    auto *Session = Dialog->findChild<PackageSession *>();
    QTRY_VERIFY_WITH_TIMEOUT(Session->ready(), TimeoutMs);
    Session->previewCapture(Directory.path());
    QTRY_VERIFY_WITH_TIMEOUT(!Session->busy(), TimeoutMs);
    Session->commitPreview();
    QTRY_VERIFY_WITH_TIMEOUT(!Session->busy(), TimeoutMs);
    Session->assess(artifact(*Session, 1), QStringLiteral("package-json"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session->busy(), TimeoutMs);
    QVERIFY(!Session->findings().isEmpty());
    Session->review(Session->findings()[0].toObject()["finding_id"].toString(),
                    QStringLiteral("needs_review"));
    QTRY_VERIFY_WITH_TIMEOUT(!Session->busy(), TimeoutMs);
    const auto Before = Session->journal();
    QCOMPARE(Before["decisions"].toArray().size(), 1);
    Dialog->show();
    QSignalSpy Finished(Dialog, &QDialog::finished);
    const auto Answer = [](QMessageBox::StandardButton Choice) {
      QTimer::singleShot(0, [Choice] {
        for (auto *Widget : QApplication::topLevelWidgets())
          if (auto *Box = qobject_cast<QMessageBox *>(Widget))
            Box->button(Choice)->click();
      });
    };
    Answer(QMessageBox::Cancel);
    QTest::keyClick(Dialog, Qt::Key_Escape);
    QVERIFY(Dialog && Dialog->isVisible());
    QCOMPARE(Finished.count(), 0);
    QCOMPARE(Session->journal(), Before);
    Answer(QMessageBox::Cancel);
    QVERIFY(!Dialog->close());
    QCOMPARE(Session->journal(), Before);
    Answer(QMessageBox::Cancel);
    QVERIFY(!Parent.close());
    QVERIFY(Parent.isVisible());
    QCOMPARE(Session->journal(), Before);
    Session->cancel();
    QVERIFY(!Session->ready());
    QVERIFY(Dialog->findChild<QPushButton *>(QStringLiteral("packageSave"))
                ->isEnabled());
    QCOMPARE(Session->journal(), Before);
    Answer(QMessageBox::Discard);
    Dialog->reject();
    QCOMPARE(Finished.count(), 1);
    QTRY_VERIFY(Dialog.isNull());
    QVERIFY(Parent.close());
  }
  void omittedBackendHasNoImplicitFallback() {
    if (WebAvailable)
      QSKIP("This case qualifies the backend-omission build");
    PackageSession Session(QString::fromLocal8Bit(TEST_WEB_WORKER));
    QTRY_VERIFY_WITH_TIMEOUT(!Session.busy(), TimeoutMs);
    QVERIFY(!Session.ready());
    QVERIFY(!Session.canRestore());
    QCOMPARE(Session.error(), QStringLiteral("capability_unavailable"));
  }
};

QTEST_MAIN(WebReviewTests)
#include "WebReviewTests.moc"
