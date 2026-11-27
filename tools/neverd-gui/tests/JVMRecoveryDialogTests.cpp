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
#include <QSignalSpy>
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
    QString Path = Temp.filePath("multi.jar");
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
    auto *Run = Dialog.findChild<QPushButton *>("jvmAnalyze");
    auto *Inspect = Dialog.findChild<QPushButton *>("jvmInspect");
    auto *Source = Dialog.findChild<QPlainTextEdit *>("jvmSource");
    auto *Bytecode = Dialog.findChild<QPlainTextEdit *>("jvmBytecode");
    auto *IR = Dialog.findChild<QPlainTextEdit *>("jvmIR");
    auto *Mappings = Dialog.findChild<QListWidget *>("jvmMappings");
    auto *Tabs = Dialog.findChild<QTabWidget *>();
    auto *Copy = Dialog.findChild<QPushButton *>("jvmCopy");
    QVERIFY(Input && Release && Classes && Methods && Run && Inspect &&
            Source && Bytecode && IR && Mappings && Tabs && Copy);
    Input->setText(Path);
    Release->setCurrentIndex(Release->findData(17));
    QTRY_VERIFY_WITH_TIMEOUT(Inspect->isEnabled(), 15000);
    Inspect->click();
    QTRY_VERIFY_WITH_TIMEOUT(Inspect->isEnabled(), 15000);
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
    QCOMPARE(BytecodeChanges.count(), 0);
    Tabs->setCurrentWidget(Bytecode);
    QCOMPARE(QJsonDocument::fromJson(Bytecode->toPlainText().toUtf8()).array(),
             F["instructions"].toArray());
    Tabs->setCurrentWidget(IR);
    QCOMPARE(QJsonDocument::fromJson(IR->toPlainText().toUtf8()).object(),
             F["ir"].toObject());
    BytecodeChanges.clear();
    Tabs->setCurrentWidget(Bytecode);
    QCOMPARE(BytecodeChanges.count(), 0);
    Copy->click();
    QCOMPARE(QApplication::clipboard()->text(), Bytecode->toPlainText());
    Tabs->setCurrentWidget(Source);
    bool Found = false;
    for (int I = 0; I < Mappings->count(); ++I) {
      auto Lines = Mappings->item(I)->data(Qt::UserRole).toList();
      if (Lines.empty())
        continue;
      Mappings->setCurrentRow(I);
      QCOMPARE(Source->textCursor().blockNumber() + 1, Lines.front().toInt());
      Found = true;
      break;
    }
    QVERIFY(Found);
    Source->setFocus();
    QTRY_VERIFY(Source->hasFocus());
    auto Map = F["source_map"].toArray().first().toObject();
    int Line = Map["line"].toInt();
    Source->moveCursor(QTextCursor::Start);
    Source->setTextCursor(
        QTextCursor(Source->document()->findBlockByNumber(Line - 1)));
    QCOMPARE(Mappings->selectedItems().size(), Map["pcs"].toArray().size());
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
