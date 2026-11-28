//===- JVMRecoveryDialogTests.cpp - JVM worker and CLI parity -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise physical member selection, source navigation and create-only
/// exports.
//===----------------------------------------------------------------------===//

#include "app/JVMRecoveryDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSplitter>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>

namespace {
QByteArray readFile(const QString &Path) {
  QFile File(Path);
  return File.open(QIODevice::ReadOnly) ? File.readAll() : QByteArray{};
}
QJsonObject cli(const QString &File, QStringList Options,
                int ExpectedExit = 0) {
  QProcess P;
  P.start(TEST_JVM_CLI,
          QStringList{"java", File, "--json", "--release=17"} + Options);
  if (!P.waitForFinished(20000) || P.exitCode() != ExpectedExit)
    return {};
  return QJsonDocument::fromJson(P.readAllStandardOutput()).object();
}
} // namespace
class JVMRecoveryDialogTests final : public QObject {
  Q_OBJECT
private slots:
  void physicalSelectionAndViews() {
    QTemporaryDir Temp;
    QVERIFY(Temp.isValid());
    auto Fixtures = QJsonDocument::fromJson(readFile(TEST_JVM_MANIFEST))
                        .object()["fixtures"]
                        .toArray();
    QByteArray Jar;
    for (auto V : Fixtures)
      if (V.toObject()["id"] == "multi")
        Jar =
            QByteArray::fromBase64(V.toObject()["bytes"].toString().toLatin1());
    QVERIFY(!Jar.isEmpty());
    const QString JarName = QStringLiteral("جمع-源码.jar");
    QString Path = Temp.filePath(JarName);
    QFile File(Path);
    QVERIFY(File.open(QIODevice::WriteOnly));
    QCOMPARE(File.write(Jar), Jar.size());
    File.close();
    neverd::gui::JVMRecoveryDialog Dialog(TEST_JVM_WORKER);
    Dialog.show();
    auto *Input = Dialog.findChild<QLineEdit *>("jvmPath");
    auto *Release = Dialog.findChild<QComboBox *>("jvmRelease");
    auto *Classes = Dialog.findChild<QComboBox *>("jvmClasses");
    auto *Methods = Dialog.findChild<QComboBox *>("jvmMethods");
    auto *Results = Dialog.findChild<QComboBox *>("jvmResults");
    auto *Run = Dialog.findChild<QPushButton *>("jvmAnalyze");
    auto *Inspect = Dialog.findChild<QPushButton *>("jvmInspect");
    auto *Source = Dialog.findChild<QPlainTextEdit *>("jvmSource");
    auto *Bytecode = Dialog.findChild<QPlainTextEdit *>("jvmBytecode");
    auto *IR = Dialog.findChild<QPlainTextEdit *>("jvmIR");
    auto *Evidence = Dialog.findChild<QPlainTextEdit *>("jvmEvidence");
    auto *Mappings = Dialog.findChild<QListWidget *>("jvmMappings");
    auto *Tabs = Dialog.findChild<QTabWidget *>();
    auto *Copy = Dialog.findChild<QPushButton *>("jvmCopy");
    QVERIFY(Input && Release && Classes && Methods && Results && Run &&
            Inspect && Source && Bytecode && IR && Evidence && Mappings &&
            Tabs && Copy);
    Input->setText(Path);
    Release->setCurrentIndex(Release->findData(17));
    QTRY_VERIFY_WITH_TIMEOUT(Inspect->isEnabled(), 15000);
    Inspect->click();
    QTRY_VERIFY_WITH_TIMEOUT(Inspect->isEnabled(), 15000);
    QCOMPARE(Tabs->currentWidget(), Evidence);
    QVERIFY(Evidence->toPlainText().contains(JarName));
    QCOMPARE(Evidence->document()->defaultFont().styleStrategy(),
             Evidence->font().styleStrategy());
    auto Inventory = cli(Path, {"--inventory"});
    QVERIFY(!Inventory.isEmpty());
    int Variant = -1, Attached = -1;
    for (auto V : Inventory["components"].toArray()) {
      auto C = V.toObject();
      if (C["kind"] == "class" && C["selected"].toBool()) {
        QCOMPARE(C["release"].toInt(), 11);
        Variant = C["id"].toInt();
      }
      if (C["kind"] == "source" && C["selected"].toBool())
        Attached = C["id"].toInt();
    }
    QVERIFY(Variant >= 0 && Attached >= 0);
    QVERIFY(Classes->findData(Variant) > 0);
    Classes->setCurrentIndex(Classes->findData(Variant));
    const QString Key = "arithmetic(II)I";
    QVERIFY(Methods->findData(Key) > 0);
    Methods->setCurrentIndex(Methods->findData(Key));
    QSignalSpy BytecodeChanges(Bytecode, &QPlainTextEdit::textChanged);
    Run->click();
    QVERIFY(!Run->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
    auto Expected = cli(
        Path, {"--component=" + QString::number(Variant), "--method=" + Key});
    QVERIFY(!Expected.isEmpty());
    QString Export = Temp.filePath("report.json");
    QVERIFY(Dialog.exportView(Export, true));
    QCOMPARE(QJsonDocument::fromJson(readFile(Export)).object(), Expected);
    auto F = Expected["functions"].toArray().first().toObject();
    QCOMPARE(F["status"].toString(), "reconstructed_source");
    QCOMPARE(Tabs->currentWidget(), Source);
    QCOMPARE(Source->toPlainText(), F["source"].toString());
    QVERIFY(Source->document()->defaultFont().styleStrategy() &
            QFont::PreferNoShaping);
    QVERIFY(Mappings->font().styleStrategy() & QFont::PreferNoShaping);
    QCOMPARE(BytecodeChanges.count(), 0);
    bool VisibleDuringLoad = true;
    const auto Loaded =
        connect(Bytecode, &QPlainTextEdit::textChanged, this,
                [&] { VisibleDuringLoad = Bytecode->isVisible(); });
    Tabs->setCurrentWidget(Bytecode);
    disconnect(Loaded);
    QCOMPARE(BytecodeChanges.count(), 1);
    QVERIFY(!VisibleDuringLoad);
    QCOMPARE(QJsonDocument::fromJson(Bytecode->toPlainText().toUtf8()).array(),
             F["instructions"].toArray());
    Bytecode->moveCursor(QTextCursor::End);
    QCOMPARE(Bytecode->textCursor().block(), Bytecode->document()->lastBlock());
    const int Scroll = Bytecode->verticalScrollBar()->value();
    QVERIFY(Scroll > 0);
    Tabs->setCurrentWidget(IR);
    QCOMPARE(QJsonDocument::fromJson(IR->toPlainText().toUtf8()).object(),
             F["ir"].toObject());
    BytecodeChanges.clear();
    Tabs->setCurrentWidget(Bytecode);
    QCOMPARE(BytecodeChanges.count(), 0);
    QCOMPARE(Bytecode->textCursor().block(), Bytecode->document()->lastBlock());
    QCOMPARE(Bytecode->verticalScrollBar()->value(), Scroll);
    Copy->click();
    QCOMPARE(QApplication::clipboard()->text(), Bytecode->toPlainText());
    Tabs->setCurrentWidget(Evidence);
    QVERIFY(Evidence->document()->defaultFont().styleStrategy() &
            QFont::PreferNoShaping);
    Tabs->setCurrentWidget(Source);
    const auto Instructions = F["instructions"].toArray();
    QCOMPARE(Mappings->count(), Instructions.size());
    bool Found = false;
    for (int I = 0; I < Mappings->count(); ++I) {
      const auto Instruction = Instructions.at(I).toObject();
      QCOMPARE(Mappings->item(I)->text(),
               QStringLiteral("%1   0x%2   %3")
                   .arg(Instruction["pc"].toInt())
                   .arg(Instruction["offset"].toInteger(), 0, 16)
                   .arg(Instruction["opcode"].toString()));
      const auto Spans = Instruction["source_spans"].toArray();
      if (Spans.isEmpty())
        continue;
      Mappings->setCurrentRow(I);
      QCOMPARE(Source->textCursor().blockNumber() + 1,
               Spans.first().toObject()["line"].toInt());
      Found = true;
    }
    QVERIFY(Found);
    Source->setFocus();
    QTRY_VERIFY(Source->hasFocus());
    Source->moveCursor(QTextCursor::Start);
    for (auto V : F["source_map"].toArray()) {
      const auto Map = V.toObject();
      const int Line = Map["line"].toInt();
      Source->setTextCursor(
          QTextCursor(Source->document()->findBlockByNumber(Line - 1)));
      const auto PCs = Map["pcs"].toArray();
      for (int I = 0; I < Mappings->count(); ++I)
        QCOMPARE(Mappings->item(I)->isSelected(),
                 PCs.contains(Instructions.at(I).toObject()["pc"]));
    }
    Methods->setCurrentIndex(0);
    Run->click();
    QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
    const int Row = Results->findText(Key, Qt::MatchEndsWith);
    QVERIFY(Row >= 0 && Row != Results->currentIndex());
    Tabs->setCurrentWidget(Bytecode);
    Tabs->setCurrentWidget(IR);
    BytecodeChanges.clear();
    Results->setCurrentIndex(Row);
    QCOMPARE(Tabs->currentWidget(), IR);
    QCOMPARE(QJsonDocument::fromJson(IR->toPlainText().toUtf8()).object(),
             F["ir"].toObject());
    QCOMPARE(BytecodeChanges.count(), 0);
    Tabs->setCurrentWidget(Bytecode);
    QCOMPARE(QJsonDocument::fromJson(Bytecode->toPlainText().toUtf8()).array(),
             F["instructions"].toArray());
    Classes->setCurrentIndex(Classes->findData(Attached));
    Run->click();
    QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
    Expected = cli(Path, {"--component=" + QString::number(Attached)});
    F = Expected["functions"].toArray().first().toObject();
    QCOMPARE(F["status"].toString(), "extracted_source");
    auto View = Temp.filePath("attached.java");
    QVERIFY(Dialog.exportView(View, false));
    QCOMPARE(readFile(View), F["source"].toString().toUtf8());
    QVERIFY(!Dialog.exportView(View, false));
    QCOMPARE(readFile(View), F["source"].toString().toUtf8());
    Tabs->setCurrentWidget(Bytecode);
    QVERIFY(Bytecode->document()->isEmpty());
    QVERIFY(!Copy->isEnabled());
    Tabs->setCurrentWidget(IR);
    QVERIFY(IR->document()->isEmpty());
    Inspect->click();
    QTRY_VERIFY_WITH_TIMEOUT(Inspect->isEnabled(), 15000);
    QCOMPARE(Tabs->currentWidget(), Evidence);
    QCOMPARE(Evidence->document()->defaultFont().styleStrategy(),
             Evidence->font().styleStrategy());
    Copy->click();
    QVERIFY(QApplication::clipboard()->text().contains(JarName));
  }
  void sourceShapingFollowsSelectedText() {
    QTemporaryDir Temp;
    QVERIFY(Temp.isValid());
    neverd::gui::JVMRecoveryDialog Dialog(TEST_JVM_WORKER);
    Dialog.show();
    auto *Input = Dialog.findChild<QLineEdit *>("jvmPath");
    auto *Run = Dialog.findChild<QPushButton *>("jvmAnalyze");
    auto *Source = Dialog.findChild<QPlainTextEdit *>("jvmSource");
    auto *Copy = Dialog.findChild<QPushButton *>("jvmCopy");
    QVERIFY(Input && Run && Source && Copy);
    const auto ASCII = QStringLiteral("class Sample { int count; }\n");
    const auto Unicode =
        QStringLiteral("class Sample { String اسم = \"漢字\"; }\n");
    QFile File(Temp.filePath("Sample.java"));
    Input->setText(File.fileName());
    for (const auto &Text : {ASCII, Unicode, ASCII}) {
      QVERIFY(File.open(QIODevice::WriteOnly | QIODevice::Truncate));
      const auto Bytes = Text.toUtf8();
      QCOMPARE(File.write(Bytes), Bytes.size());
      File.close();
      QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
      Run->click();
      QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
      QCOMPARE(Source->toPlainText(), Text);
      if (Text == Unicode)
        QCOMPARE(Source->document()->defaultFont().styleStrategy(),
                 Source->font().styleStrategy());
      else
        QVERIFY(Source->document()->defaultFont().styleStrategy() &
                QFont::PreferNoShaping);
      Copy->click();
      QCOMPARE(QApplication::clipboard()->text(), Text);
    }
  }
  void tabPaneTracksSizeChanges() {
    neverd::gui::JVMRecoveryDialog Dialog(TEST_JVM_WORKER);
    Dialog.show();
    auto *Tabs = Dialog.findChild<QTabWidget *>();
    auto *Split = Dialog.findChild<QSplitter *>();
    auto *Source = Dialog.findChild<QPlainTextEdit *>("jvmSource");
    auto *IR = Dialog.findChild<QPlainTextEdit *>("jvmIR");
    QVERIFY(Tabs && Split && Source && IR);
    Tabs->setCurrentWidget(Source);
    const QSize Minimum = Dialog.minimumSizeHint();
    const int Width = Minimum.width() + Tabs->width();
    Source->setMinimumWidth(Width);
    QTRY_VERIFY(Dialog.minimumSizeHint().width() >= Width);
    Tabs->setCurrentWidget(IR);
    QVERIFY(Dialog.minimumSizeHint().width() >= Width);
    Tabs->setCurrentWidget(Source);
    Source->setMinimumWidth(0);
    QTRY_COMPARE(Dialog.minimumSizeHint(), Minimum);

    Split->setSizes({Split->width() / 3, Split->width()});
    const auto Sizes = Split->sizes();
    const auto PageSize = Source->size();
    Tabs->setCurrentWidget(IR);
    QCOMPARE(Split->sizes(), Sizes);
    QCOMPARE(IR->size(), PageSize);
  }
  void incompleteMethodShowsMappedBytecodeAndDiagnostics() {
    QTemporaryDir Temp;
    QVERIFY(Temp.isValid());
    auto ClassesFile =
        QFileInfo(TEST_JVM_MANIFEST).absolutePath() + "/classes.json";
    auto Fixtures = QJsonDocument::fromJson(readFile(ClassesFile))
                        .object()["fixtures"]
                        .toArray();
    QByteArray Data;
    for (auto V : Fixtures)
      if (V.toObject()["id"] == "lambda-generics")
        Data =
            QByteArray::fromBase64(V.toObject()["bytes"].toString().toLatin1());
    QVERIFY(!Data.isEmpty());
    auto Path = Temp.filePath("Features.class");
    QFile File(Path);
    QVERIFY(File.open(QIODevice::WriteOnly));
    QCOMPARE(File.write(Data), Data.size());
    File.close();
    neverd::gui::JVMRecoveryDialog Dialog(TEST_JVM_WORKER);
    Dialog.show();
    auto *Input = Dialog.findChild<QLineEdit *>("jvmPath");
    auto *Release = Dialog.findChild<QComboBox *>("jvmRelease");
    auto *Classes = Dialog.findChild<QComboBox *>("jvmClasses");
    auto *Methods = Dialog.findChild<QComboBox *>("jvmMethods");
    auto *Inspect = Dialog.findChild<QPushButton *>("jvmInspect");
    auto *Run = Dialog.findChild<QPushButton *>("jvmAnalyze");
    auto *Source = Dialog.findChild<QPlainTextEdit *>("jvmSource");
    auto *IR = Dialog.findChild<QPlainTextEdit *>("jvmIR");
    auto *Evidence = Dialog.findChild<QPlainTextEdit *>("jvmEvidence");
    auto *Status = Dialog.findChild<QLabel *>("jvmStatus");
    auto *Bytecode = Dialog.findChild<QPlainTextEdit *>("jvmBytecode");
    auto *Tabs = Dialog.findChild<QTabWidget *>();
    QVERIFY(Input && Release && Classes && Methods && Inspect && Run &&
            Source && IR && Evidence && Status && Bytecode && Tabs);
    Input->setText(Path);
    Release->setCurrentIndex(Release->findData(17));
    QTRY_VERIFY_WITH_TIMEOUT(Inspect->isEnabled(), 15000);
    Inspect->click();
    QTRY_VERIFY_WITH_TIMEOUT(Inspect->isEnabled(), 15000);
    QVERIFY(Classes->findData(0) > 0);
    Classes->setCurrentIndex(Classes->findData(0));
    QString Key = "cleanup(Ljava/util/List;)V";
    QVERIFY(Methods->findData(Key) > 0);
    Methods->setCurrentIndex(Methods->findData(Key));
    Run->click();
    QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
    auto Expected = cli(Path, {"--component=0", "--method=" + Key}, 1);
    QVERIFY(!Expected.isEmpty());
    auto F = Expected["functions"].toArray().first().toObject();
    QCOMPARE(F["status"].toString(), "disassembly_only");
    QVERIFY(Source->toPlainText().isEmpty());
    QCOMPARE(Tabs->currentWidget(), IR);
    QCOMPARE(QJsonDocument::fromJson(IR->toPlainText().toUtf8()).object(),
             F["ir"].toObject());
    Tabs->setCurrentWidget(Bytecode);
    QCOMPARE(QJsonDocument::fromJson(Bytecode->toPlainText().toUtf8()).array(),
             F["instructions"].toArray());
    auto D = F["diagnostics"].toArray();
    QVERIFY(!D.isEmpty());
    QVERIFY(Status->text().contains(D.first().toObject()["code"].toString()));
    Tabs->setCurrentWidget(Evidence);
    QCOMPARE(QJsonDocument::fromJson(Evidence->toPlainText().toUtf8())
                 .object()["diagnostics"]
                 .toArray(),
             D);
  }
};
QTEST_MAIN(JVMRecoveryDialogTests)
#include "JVMRecoveryDialogTests.moc"
