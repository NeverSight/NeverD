//===- PythonRecoveryDialogTests.cpp - Python CLI and workbench parity ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Drive the dialog, real worker and CLI through selection and immutable
/// exports.
//===----------------------------------------------------------------------===//

#include "ScriptRecoveryDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
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
#include <QTemporaryDir>
#include <QTest>

using namespace neverd::gui;
namespace {
QByteArray contents(const QString &Name) {
  QFile F(Name);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
}
bool write(const QString &Path, const QByteArray &Bytes) {
  QFile F(Path);
  return F.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
         F.write(Bytes) == Bytes.size();
}
QByteArray fixture(const QString &Name) {
  auto Rows = QJsonDocument::fromJson(contents(TEST_PYTHON_MANIFEST))
                  .object()["fixtures"]
                  .toArray();
  for (auto V : Rows)
    if (V.toObject()["name"] == Name)
      return QByteArray::fromBase64(V.toObject()["data"].toString().toLatin1());
  return {};
}
QJsonObject report(const QString &File, int Component = -1) {
  QProcess P;
  QStringList Args{"python", "--json", File};
  if (Component >= 0)
    Args << "--component=" + QString::number(Component);
  P.start(TEST_PYTHON_CLI, Args);
  if (!P.waitForFinished(30000))
    return {};
  return QJsonDocument::fromJson(P.readAllStandardOutput()).object();
}
} // namespace
class PythonRecoveryDialogTests final : public QObject {
  Q_OBJECT
private slots:
  void functionsAndComponentsShareCliEvidence() {
    QTemporaryDir Dir;
    QVERIFY(Dir.isValid());
    auto Artifact = Dir.filePath("owned.pkg");
    auto Bytes = fixture("owned.pkg");
    QVERIFY(!Bytes.isEmpty());
    QVERIFY(write(Artifact, Bytes));
    ScriptRecoveryDialog Dialog(TEST_PYTHON_WORKER, nullptr,
                                ScriptLanguage::Python);
    Dialog.show();
    auto *Path = Dialog.findChild<QLineEdit *>("pythonPath");
    auto *Run = Dialog.findChild<QPushButton *>("pythonRecover");
    auto *Cancel = Dialog.findChild<QPushButton *>("pythonCancel");
    auto *Components = Dialog.findChild<QComboBox *>("pythonComponents");
    auto *Functions = Dialog.findChild<QComboBox *>("pythonPrototypes");
    auto *Mappings = Dialog.findChild<QListWidget *>("pythonMappings");
    auto *Source = Dialog.findChild<QPlainTextEdit *>("pythonSource");
    auto *Copy = Dialog.findChild<QPushButton *>("pythonCopy");
    QVERIFY(Path && Run && Cancel && Components && Functions && Mappings &&
            Source && Copy);
    Path->setText(Artifact);
    QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
    Run->click();
    QVERIFY(Cancel->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
    auto Reference = report(Artifact);
    QCOMPARE(Reference["status"].toString(), QString("reconstructed_source"));
    auto Manifest = Dir.filePath("report.json");
    QVERIFY(Dialog.exportFile(Manifest, true));
    QCOMPARE(QJsonDocument::fromJson(contents(Manifest)).object(), Reference);
    int Child = -1, Native = -1;
    for (auto V : Reference["components"].toArray()) {
      auto R = V.toObject();
      if (R["name"] == "owned.child")
        Child = R["id"].toInt();
      if (R["backend"] == "native")
        Native = R["id"].toInt();
    }
    QVERIFY(Child >= 0 && Native >= 0);
    Components->setCurrentIndex(Components->findData(Child));
    QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
    auto Selected = report(Artifact, Child);
    QVERIFY(Dialog.exportFile(Dir.filePath("selected.json"), true));
    QCOMPARE(QJsonDocument::fromJson(contents(Dir.filePath("selected.json")))
                 .object(),
             Selected);
    QVERIFY(Functions->count() > 1);
    Functions->setCurrentIndex(1);
    auto Fn = Selected["functions"].toArray().at(1).toObject();
    auto Text = Fn["source"].toString();
    QCOMPARE(Source->toPlainText(), Text);
    Copy->click();
    QCOMPARE(QApplication::clipboard()->text(), Text);
    bool Navigated = false;
    for (int I = 0; I < Mappings->count(); ++I) {
      auto Line = Mappings->item(I)->data(Qt::UserRole).toInt();
      if (Line <= 1)
        continue;
      Mappings->setCurrentRow(I);
      QCOMPARE(Source->textCursor().blockNumber(), Line - 1);
      Navigated = true;
      break;
    }
    QVERIFY(Navigated);
    auto Out = Dir.filePath("function.py");
    QVERIFY(Dialog.exportFile(Out, false));
    auto CliOut = Dir.filePath("cli.py");
    QCOMPARE(
        QProcess::execute(
            TEST_PYTHON_CLI,
            {"python", Artifact, "--component=" + QString::number(Child),
             "--function=" + QString::number(Fn["id"].toInt()), "-o", CliOut}),
        0);
    QCOMPARE(contents(Out), contents(CliOut));
    QVERIFY(!Dialog.exportFile(Artifact, false));
    Components->setCurrentIndex(Components->findData(Native));
    QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
    QVERIFY(!Copy->isEnabled());
    QSignalSpy Opened(&Dialog, &ScriptRecoveryDialog::nativeReady);
    auto Image = Dir.filePath("member.o");
    QVERIFY(Dialog.openNative(Image));
    QTRY_COMPARE_WITH_TIMEOUT(Opened.count(), 1, 15000);
    QCOMPARE(Opened.first().first().toString(), Image);
    QCOMPARE(contents(Image), fixture("owned.so"));
    QCOMPARE(contents(Artifact), Bytes);
    QProcess NativeCLI;
    auto Exported = Dir.filePath("cli-native.o");
    NativeCLI.start(TEST_PYTHON_CLI,
                    {"python", Artifact, "--native=" + QString::number(Native),
                     "-o", Exported});
    QVERIFY(NativeCLI.waitForFinished(30000));
    QCOMPARE(NativeCLI.exitCode(), 0);
    auto Discovered =
        QJsonDocument::fromJson(NativeCLI.readAllStandardOutput()).array();
    QStringList Names;
    for (const auto &V : Discovered)
      Names.append(V.toObject()["name"].toString());
    QVERIFY(Names.contains("PyInit_owned"));
    QCOMPARE(contents(Exported), fixture("owned.so"));

    Run->click();
    Cancel->click();
    QVERIFY(!Cancel->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
  }
  void decodedFunctionHasMappingsAndNoSourceExport() {
    QTemporaryDir Dir;
    auto File = Dir.filePath("modern.pyc");
    auto Manifest =
        QFileInfo(TEST_PYTHON_MANIFEST).dir().filePath("modern.json");
    auto Bytes =
        QByteArray::fromBase64(QJsonDocument::fromJson(contents(Manifest))
                                   .object()["data"]
                                   .toString()
                                   .toLatin1());
    QVERIFY(!Bytes.isEmpty());
    QVERIFY(write(File, Bytes));
    ScriptRecoveryDialog Dialog(TEST_PYTHON_WORKER, nullptr,
                                ScriptLanguage::Python);
    auto *Run = Dialog.findChild<QPushButton *>("pythonRecover");
    Dialog.findChild<QLineEdit *>("pythonPath")->setText(File);
    QTRY_VERIFY(Run->isEnabled());
    Run->click();
    QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
    auto Reference = report(File);
    QCOMPARE(Reference["status"].toString(), QString("disassembly_only"));
    auto Out = Dir.filePath("modern.json");
    QVERIFY(Dialog.exportFile(Out, true));
    QCOMPARE(QJsonDocument::fromJson(contents(Out)).object(), Reference);
    auto *Functions = Dialog.findChild<QComboBox *>("pythonPrototypes");
    QVERIFY(Functions->count() > 1);
    Functions->setCurrentIndex(1);
    QVERIFY(Dialog.findChild<QListWidget *>("pythonMappings")->count() > 0);
    QVERIFY(!Dialog.findChild<QPushButton *>("pythonCopy")->isEnabled());
    QVERIFY(!Dialog.exportFile(Dir.filePath("unavailable.py"), false));
  }
  void originalSourceAndMissingFile() {
    QTemporaryDir Dir;
    ScriptRecoveryDialog Dialog(TEST_PYTHON_WORKER, nullptr,
                                ScriptLanguage::Python);
    auto *Path = Dialog.findChild<QLineEdit *>("pythonPath");
    auto *Run = Dialog.findChild<QPushButton *>("pythonRecover");
    auto *Status = Dialog.findChild<QLabel *>("pythonStatus");
    auto Source = Dir.filePath("source.py");
    QByteArray Bytes = "# coding: latin-1\r\nx = '\xe9'\r\n";
    QVERIFY(write(Source, Bytes));
    Path->setText(Source);
    QTRY_VERIFY(Run->isEnabled());
    Run->click();
    QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
    QVERIFY(Dialog.exportFile(Dir.filePath("out.py"), false));
    QCOMPARE(contents(Dir.filePath("out.py")), Bytes);
    Path->setText(Dir.filePath("missing.pyc"));
    Run->click();
    QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
    QVERIFY(Status->text().contains("io_error"));
    QVERIFY(!Dialog.exportFile(Dir.filePath("missing.py"), false));
  }
};
QTEST_MAIN(PythonRecoveryDialogTests)
#include "PythonRecoveryDialogTests.moc"
