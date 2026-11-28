//===- LuaRecoveryDialogTests.cpp - Paired Lua CLI and workbench checks ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercises the actual worker, navigation, cancellation and disk exports
/// through the dialog.
//===----------------------------------------------------------------------===//

#include "ScriptRecoveryDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
using namespace neverd::gui;
namespace {
QByteArray read(const QString &Name) {
  QFile F(Name);
  return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray();
}
bool write(const QString &Name, QByteArray Data) {
  QFile F(Name);
  return F.open(QIODevice::WriteOnly) && F.write(Data) == Data.size();
}
QJsonObject cli(const QString &File, const QString &Profile) {
  QProcess P;
  P.start(TEST_LUA_CLI, {"lua", "--json", "--profile=" + Profile, File});
  if (!P.waitForFinished())
    return {};
  return QJsonDocument::fromJson(P.readAllStandardOutput()).object();
}
} // namespace
class LuaRecoveryDialogTests final : public QObject {
  Q_OBJECT
private slots:
  void cliAndDialogShareReportAndSource() {
    QTemporaryDir Temp;
    QVERIFY(Temp.isValid());
    auto Manifest = QJsonDocument::fromJson(read(TEST_LUA_MANIFEST))
                        .object()["fixtures"]
                        .toArray();
    QVERIFY(!Manifest.isEmpty());
    ScriptRecoveryDialog Dialog(TEST_LUA_WORKER);
    auto *Path = Dialog.findChild<QLineEdit *>("luaPath");
    auto *Profiles = Dialog.findChild<QComboBox *>("luaProfile");
    auto *Prototypes = Dialog.findChild<QComboBox *>("luaPrototypes");
    auto *Run = Dialog.findChild<QPushButton *>("luaRecover");
    auto *Stop = Dialog.findChild<QPushButton *>("luaCancel");
    auto *Copy = Dialog.findChild<QPushButton *>("luaCopy");
    auto *Source = Dialog.findChild<QPlainTextEdit *>("luaSource");
    auto *Mappings = Dialog.findChild<QListWidget *>("luaMappings");
    Dialog.show();
    int Cells = 0;
    for (auto Item : Manifest) {
      auto F = Item.toObject();
      if (!F["id"].toString().endsWith("/capture/debug"))
        continue;
      ++Cells;
      auto Bytes = QByteArray::fromBase64(F["bytes"].toString().toLatin1());
      auto File = Temp.filePath(QString::number(Cells) + ".bin");
      QVERIFY(write(File, Bytes));
      Path->setText(File);
      auto Profile = F["profile"].toString();
      Profiles->setCurrentIndex(Profiles->findData(Profile));
      QTRY_VERIFY(Run->isEnabled());
      Run->click();
      QVERIFY(!Run->isEnabled());
      QVERIFY(Stop->isEnabled());
      QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
      auto ReportPath = File + ".json";
      QVERIFY(Dialog.exportFile(ReportPath, true));
      auto Report = QJsonDocument::fromJson(read(ReportPath)).object();
      QCOMPARE(Report, cli(File, Profile));
      QCOMPARE(Report["status"].toString(), "reconstructed_source");
      QVERIFY(Prototypes->count() > 1);
      Prototypes->setCurrentIndex(1);
      auto Fn = Report["functions"].toArray().at(1).toObject();
      QCOMPARE(Source->toPlainText(), Fn["source"].toString());
      QVERIFY(Mappings->count() > 0);
      for (int I = 0; I < Mappings->count(); ++I) {
        int Line = Mappings->item(I)->data(Qt::UserRole).toInt();
        if (Line) {
          Mappings->setCurrentRow(I);
          QCOMPARE(Source->textCursor().blockNumber(), Line - 1);
          break;
        }
      }
      Copy->click();
      QCOMPARE(QApplication::clipboard()->text(), Fn["source"].toString());
      auto Destination = File + ".lua";
      QVERIFY(Dialog.exportFile(Destination, false));
      QCOMPARE(read(Destination), Fn["source"].toString().toUtf8());
      auto CliExport = File + ".cli.lua";
      QStringList Arguments{"lua",
                            File,
                            "--profile=" + Profile,
                            "--prototype=" + QString::number(Fn["id"].toInt()),
                            "-o",
                            CliExport};
      QCOMPARE(QProcess::execute(TEST_LUA_CLI, Arguments), 0);
      QCOMPARE(read(CliExport), read(Destination));
      QVERIFY(QProcess::execute(TEST_LUA_CLI, Arguments) != 0);
      QVERIFY(!Dialog.exportFile(File, false));
      QCOMPARE(read(File), Bytes);
      Run->click();
      Stop->click();
      QVERIFY(!Stop->isEnabled());
      QTRY_VERIFY_WITH_TIMEOUT(Run->isEnabled(), 15000);
    }
    QCOMPARE(Cells, 8);
    auto File = Temp.filePath("source.luau");
    QByteArray Original = "-- text\r\nreturn 1\r\n";
    QVERIFY(write(File, Original));
    Path->setText(File);
    Run->click();
    QTRY_VERIFY(Run->isEnabled());
    QVERIFY(Dialog.exportFile(File + ".export", false));
    QCOMPARE(read(File + ".export"), Original);
    Path->setText(Temp.filePath("missing"));
    Run->click();
    QTRY_VERIFY(Run->isEnabled());
    QCOMPARE(Prototypes->count(), 0);
    QVERIFY(!Copy->isEnabled());
  }
  void closeAndReopenWorker() {
    QTemporaryDir Temp;
    QString File = Temp.filePath("test.lua");
    QVERIFY(write(File, "return 7\n"));
    for (int I = 0; I < 2; ++I) {
      QPointer<ScriptRecoveryDialog> Dialog =
          new ScriptRecoveryDialog(TEST_LUA_WORKER);
      Dialog->show();
      Dialog->findChild<QLineEdit *>("luaPath")->setText(File);
      auto *Run = Dialog->findChild<QPushButton *>("luaRecover");
      QTRY_VERIFY(Run->isEnabled());
      Run->click();
      Dialog->close();
      QTRY_VERIFY(Dialog.isNull());
    }
    ScriptRecoveryDialog Failed(Temp.filePath("missing-worker"));
    Failed.findChild<QLineEdit *>("luaPath")->setText(File);
    auto *Status = Failed.findChild<QLabel *>("luaStatus");
    QTRY_VERIFY(Status->text() != tr("Starting analysis worker…") &&
                !Status->text().isEmpty());
    QVERIFY(!Failed.findChild<QPushButton *>("luaRecover")->isEnabled());
  }
};
QTEST_MAIN(LuaRecoveryDialogTests)
#include "LuaRecoveryDialogTests.moc"
