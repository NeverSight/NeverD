//===- AnalysisSectionTests.cpp - Analysis image workbench checks --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Analysis image workbench qualification.
///
//===----------------------------------------------------------------------===//

// Real-engine workbench: the add-section dialog, normal views and project save.
#include "CodeView.h"
#include "DisassemblyView.h"
#include "GraphView.h"
#include "MainWindow.h"
#include "ProjectDatabase.h"
#include "Session.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

using namespace neverd::gui;

namespace {
QByteArray image() {
  QByteArray data(126, '\0');
  data.replace(0, 7, QByteArray::fromHex("7f454c46020101"));
  const auto put = [&]<typename T>(int offset, T value) {
    qToLittleEndian(value, data.data() + offset);
  };
  put(16, quint16(2));
  put(18, quint16(62));
  put(20, quint32(1));
  put(24, quint64(0x400078));
  put(32, quint64(64));
  put(52, quint16(64));
  put(54, quint16(56));
  put(56, quint16(1));
  put(58, quint16(64));
  put(64, quint32(1));
  put(68, quint32(5));
  put(80, quint64(0x400000));
  put(88, quint64(0x400000));
  put(96, quint64(data.size()));
  put(104, quint64(data.size()));
  put(112, quint64(4096));
  data.replace(120, 6, QByteArray::fromHex("b807000000c3"));
  return data;
}
bool write(const QString &path, const QByteArray &data) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
} // namespace

class AnalysisSectionTests final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("NeverDTests"));
    QCoreApplication::setApplicationName(QStringLiteral("AnalysisSections"));
    QSettings().clear();
  }

  void dialogAndNativeViews() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto binary = directory.filePath(QStringLiteral("original.elf"));
    const auto bytes = directory.filePath(QStringLiteral("recovered.bin"));
    QVERIFY(write(binary, image()));
    QVERIFY(write(bytes, QByteArray::fromHex("b82a000000c3")));
    Session session(QString::fromLocal8Bit(TEST_REAL_WORKER));
    McpConnectionManager mcp;
    GuiSessionBroker broker;
    MainWindow window(session, mcp, broker);
    window.setAsksForDebugInfo(false);
    window.setLayoutPath(directory.filePath(QStringLiteral("layout.json")));
    window.resize(1400, 900);
    window.initializeLayout();
    window.show();
    session.open(binary);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), 10000);
    auto *add = window.actions().action(ActionId::EditAddAnalysisSection);
    QTRY_VERIFY(add->isEnabled());
    add->trigger();
    auto *dialog =
        window.findChild<QDialog *>(QStringLiteral("addAnalysisSection"));
    QVERIFY(dialog);
    auto *buttons = dialog->findChild<QDialogButtonBox *>();
    const auto field = [&](const char *name) {
      return dialog->findChild<QLineEdit *>(QString::fromLatin1(name));
    };
    field("sectionFile")->setText(bytes);
    field("sectionName")->setText(QStringLiteral(".from_dialog"));
    field("sectionFunction")->setText(QStringLiteral("dialog_returns_42"));
    // Reject overlap and keep the same dialog editable for correction.
    field("sectionAddress")->setText(QStringLiteral("0x400078"));
    buttons->button(QDialogButtonBox::Ok)->click();
    auto *error = dialog->findChild<QLabel *>(QStringLiteral("sectionError"));
    QTRY_VERIFY(!error->text().isEmpty());
    QVERIFY(dialog->isVisible());
    QTRY_VERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    field("sectionAddress")->setText(QStringLiteral("0x500000"));
    QPointer<QDialog> guard(dialog);
    buttons->button(QDialogButtonBox::Ok)->click();
    QTRY_VERIFY(!guard);
    QTRY_COMPARE(window.disassembly()->currentFunction(),
                 std::optional<Address>(0x500000));
    QVERIFY(session.dirty());
    window.actions().action(ActionId::ViewPseudocode)->trigger();
    CodeView *code = nullptr;
    QTRY_VERIFY(([&] {
      for (auto *view : window.findChildren<CodeView *>())
        if (view->representation() == QLatin1String("source"))
          code = view;
      return code != nullptr;
    })());
    QTRY_VERIFY(
        code->text()->allText().contains(QStringLiteral("dialog_returns_42")));
    QVERIFY2(code->text()->allText().contains(QStringLiteral("return 0x2A;")),
             qPrintable(code->text()->allText()));
    window.actions().action(ActionId::ViewToggleGraph)->trigger();
    QTRY_VERIFY(!window.disassembly()->graph()->nodes().isEmpty());
    QSignalSpy saved(&session, &Session::databaseSaved);
    session.packDatabase(session.databasePath());
    QTRY_VERIFY_WITH_TIMEOUT(!saved.isEmpty(), 10000);
    QVERIFY(saved.last().at(0).toBool());
    QString errorText;
    const auto database =
        ProjectDatabase::read(session.databasePath(), &errorText);
    QVERIFY2(database, qPrintable(errorText));
    QVERIFY(database->sidecars
                .value(QStringLiteral(".neverd-analysis-sections.json"))
                .contains(".from_dialog"));
    QFile source(binary);
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), image());
    // A reopened project uses the same native function identity and code.
    QSignalSpy opened(&session, &Session::opened);
    session.open(session.databasePath());
    QTRY_VERIFY_WITH_TIMEOUT(!opened.isEmpty(), 10000);
    bool seen = false;
    session.read(QStringLiteral("analysis_sections"), {}, &window,
                 [&](const QJsonObject &payload) {
                   seen = payload.value("sections").toArray().size() == 1;
                 });
    QTRY_VERIFY(seen);
    bool removed = false;
    session.removeAnalysisSection(QStringLiteral(".from_dialog"),
                                  [&](const QJsonObject &) { removed = true; });
    QTRY_VERIFY(removed);
    QVERIFY(session.dirty());
    // Finish with a clean project so window teardown never needs a prompt.
    session.save();
    QTRY_VERIFY(!session.dirty());
    QTRY_VERIFY(!session.queries().hasPending());
  }
};

QTEST_MAIN(AnalysisSectionTests)
#include "AnalysisSectionTests.moc"
