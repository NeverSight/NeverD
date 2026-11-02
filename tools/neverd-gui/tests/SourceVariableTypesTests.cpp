// Real workbench, worker and engine: Y on an ordinary local edits that value.
#include "CodeView.h"
#include "DisassemblyView.h"
#include "Docking.h"
#include "ListingView.h"
#include "MainWindow.h"
#include "Session.h"
#include "SetTypeDialog.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDataStream>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace neverd::gui;

namespace {
QString fixture(const QTemporaryDir &Directory,
                const QByteArray &Override = {}) {
  const QByteArray Code =
      Override.isEmpty()
          ? QByteArray::fromHex(
                "53e80f0000004889c38b43040343085bc3909090904889f8c3")
          : Override;
  QByteArray Bytes;
  QDataStream Out(&Bytes, QIODevice::WriteOnly);
  Out.setByteOrder(QDataStream::LittleEndian);
  const QByteArray Ident =
      QByteArray::fromHex("7f454c46020101000000000000000000");
  Out.writeRawData(Ident.constData(), Ident.size());
  Out << quint16(2) << quint16(62) << quint32(1) << quint64(0x400078)
      << quint64(64) << quint64(0) << quint32(0) << quint16(64) << quint16(56)
      << quint16(1) << quint16(64) << quint16(0) << quint16(0);
  Out << quint32(1) << quint32(5) << quint64(0) << quint64(0x400000)
      << quint64(0x400000) << quint64(120 + Code.size())
      << quint64(120 + Code.size()) << quint64(4096);
  Bytes += Code;
  const QString Path = Directory.filePath(QStringLiteral("source-types.elf"));
  QFile File(Path);
  return File.open(QIODevice::WriteOnly) && File.write(Bytes) == Bytes.size()
             ? Path
             : QString();
}
// Navigation and presentation cases need a stable user-owned entry before
// the first view asks discovery for it. Function creation has separate tests.
QString createdFunctionFixture(const QTemporaryDir &Directory,
                               const QByteArray &Code) {
  const QString Path = fixture(Directory, Code);
  if (Path.isEmpty() || !QDir().mkpath(Path + ".neverd"))
    return {};
  QFile Functions(Path + ".neverd/functions.json");
  const QByteArray State = R"json([{"addr":"0x400078","state":"created"}])json";
  return Functions.open(QIODevice::WriteOnly) &&
                 Functions.write(State) == State.size()
             ? Path
             : QString();
}
} // namespace

class SourceVariableTypesTests : public QObject {
  Q_OBJECT
  QTemporaryDir Settings;
private slots:
  void initTestCase() {
    QCoreApplication::setOrganizationName("NeverDTests");
    QCoreApplication::setApplicationName("SourceVariableTypes");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       Settings.path());
    configureDocking();
  }
  void longSourceSearchSurvivesResizing_data() {
    QTest::addColumn<QString>("Representation");
    QTest::newRow("pseudocode") << "source";
    QTest::newRow("llvm-c") << "llvmc";
  }
  void longSourceSearchSurvivesResizing() {
    QFETCH(QString, Representation);
    QTemporaryDir Directory;
    const auto Path =
        createdFunctionFixture(Directory, QByteArray::fromHex("b82a000000c3"));
    QVERIFY(!Path.isEmpty());
    Session Session(QString::fromLocal8Bit(TEST_REAL_WORKER));
    Session.open(Path);
    QTRY_VERIFY_WITH_TIMEOUT(Session.loaded(), 30000);
    const QString Needle = QStringLiteral("visible_tail_marker");
    const QString Name = QString(160, QLatin1Char('n')) + Needle;
    bool Renamed = false;
    Session.rename(0x400078, Name, {}, [&] { Renamed = true; });
    QTRY_VERIFY_WITH_TIMEOUT(Renamed, 30000);
    CodeText Code(Session);
    Code.resize(560, 280);
    Code.show();
    Code.load(0x400078, Representation);
    QTRY_VERIFY_WITH_TIMEOUT(Code.complete(), 30000);
    const int Match = Code.allText().indexOf(Needle);
    QVERIFY(Match >= 0);
    auto *Across = Code.horizontalScrollBar();
    const int Extent = Across->maximum() + Code.viewport()->width();
    QVERIFY(Code.selectSearchRange(Match, Needle.size()));
    QCOMPARE(Code.selectedText(), Needle);
    QVERIFY(Across->value() > 0);
    // A resize changes the viewport, not the document's horizontal extent.
    // Searching a far-right match must not reduce the next range calculation.
    for (const int Width : {640, 480, 800, 560}) {
      Code.resize(Width, 280);
      QTRY_COMPARE(Across->maximum() + Code.viewport()->width(), Extent);
      QVERIFY(Code.selectSearchRange(Match, Needle.size()));
      QCOMPARE(Code.selectedText(), Needle);
      QVERIFY(Code.cursorViewportRect().left() >= 0);
      QVERIFY(Code.cursorViewportRect().left() <= Code.viewport()->width());
    }
  }
  void immediateAndRegisterNavigation_data() {
    QTest::addColumn<QString>("Kind");
    QTest::newRow("immediate") << "immediate";
    QTest::newRow("definition") << "definition";
    QTest::newRow("value") << "value";
  }
  void immediateAndRegisterNavigation() {
    QFETCH(QString, Kind);
    QTemporaryDir Directory;
    const auto Path = createdFunctionFixture(
        Directory, QByteArray::fromHex("b82a00000089c183c007c3"));
    QVERIFY(!Path.isEmpty());
    Session Session(QString::fromLocal8Bit(TEST_REAL_WORKER));
    McpConnectionManager Mcp;
    GuiSessionBroker Broker;
    MainWindow Window(Session, Mcp, Broker);
    Window.setAsksForDebugInfo(false);
    Window.setLayoutPath(Directory.filePath("layout.json"));
    Window.resize(1400, 900);
    Window.initializeLayout();
    Window.show();
    Session.open(Path);
    QTRY_VERIFY_WITH_TIMEOUT(Session.loaded(), 30000);
    Window.disassembly()->navigate(0x40007D);
    QTRY_COMPARE_WITH_TIMEOUT(Window.disassembly()->currentItem(),
                              std::optional<Address>(0x40007D), 30000);
    Window.disassembly()->listing()->setFocus();
    auto *Action = Window.actions().action(
        Kind == "immediate"    ? ActionId::SearchImmediate
        : Kind == "definition" ? ActionId::FindRegisterDefinition
                               : ActionId::FindRegisterValue);
    QTRY_VERIFY(Action->isEnabled());
    bool Input = false, Value = false;
    QTimer Timer;
    connect(&Timer, &QTimer::timeout, this, [&] {
      if (auto *Dialog =
              qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
        Input = true;
        Dialog->setTextValue(Kind == "immediate" ? "42" : "eax");
        Dialog->accept();
      } else if (auto *Message = Window.findChild<QMessageBox *>(
                     QStringLiteral("registerValueDialog"));
                 Message && Message->isVisible()) {
        QCOMPARE(Message->windowModality(), Qt::NonModal);
        QVERIFY(!QApplication::activeModalWidget());
        Value = Message->text().contains("0x2A") &&
                Message->text().contains("before instruction");
        Message->accept();
      }
    });
    Timer.start(10);
    Action->trigger();
    if (Kind == "value")
      QTRY_VERIFY_WITH_TIMEOUT(Value, 30000);
    else
      QTRY_COMPARE_WITH_TIMEOUT(Window.disassembly()->currentAddress(),
                                std::optional<Address>(0x400078), 30000);
    if (Kind == "immediate")
      QVERIFY(Input);
    Timer.stop();
  }
  void dataPresentationDialogs_data() {
    QTest::addColumn<QString>("Kind");
    QTest::newRow("enum") << "enum";
    QTest::newRow("array") << "array";
    QTest::newRow("alignment") << "alignment";
  }
  void dataPresentationDialogs() {
    QFETCH(QString, Kind);
    QTemporaryDir Directory;
    const auto Path = createdFunctionFixture(
        Directory,
        QByteArray::fromHex("554889e5b80b0000005dc3") + QByteArray(96, '\0'));
    QVERIFY(!Path.isEmpty());
    if (Kind == "alignment") {
      // This dialog edits a chosen data item. A byte row gives the chosen
      // address a stable boundary inside the automatically grouped padding.
      QFile Item(Path + ".neverd/items.json");
      const QByteArray Boundary =
          R"json([{"addr":"0x4000a0","kind":"byte","size":1}])json";
      QVERIFY(Item.open(QIODevice::WriteOnly));
      QCOMPARE(Item.write(Boundary), Boundary.size());
    }
    Session Session(QString::fromLocal8Bit(TEST_REAL_WORKER));
    McpConnectionManager Mcp;
    GuiSessionBroker Broker;
    MainWindow Window(Session, Mcp, Broker);
    Window.setAsksForDebugInfo(false);
    Window.setLayoutPath(Directory.filePath("layout.json"));
    Window.resize(1400, 900);
    Window.initializeLayout();
    Window.show();
    Session.open(Path);
    QTRY_VERIFY_WITH_TIMEOUT(Session.loaded(), 30000);
    bool Declared = false;
    Session.declareTypes("c", "enum GUI_FLAGS { GUI_READ = 1, GUI_WRITE = 2 };",
                         [&](const QJsonObject &) { Declared = true; });
    QTRY_VERIFY_WITH_TIMEOUT(Declared, 30000);
    const Address Address = 0x400078 + (Kind == "enum"    ? 4
                                        : Kind == "array" ? 11
                                                          : 40);
    Window.disassembly()->navigate(Address);
    QTRY_COMPARE_WITH_TIMEOUT(Window.disassembly()->currentAddress(),
                              std::optional<neverd::gui::Address>(Address),
                              30000);
    QTRY_COMPARE_WITH_TIMEOUT(Window.disassembly()->currentItem(),
                              std::optional<neverd::gui::Address>(Address),
                              30000);
    Window.disassembly()->listing()->setFocus();
    auto *Action = Window.actions().action(
        Kind == "enum"    ? ActionId::EditOperandEnum
        : Kind == "array" ? ActionId::EditMakeArray
                          : ActionId::EditMakeAlignment);
    QTRY_VERIFY(Action->isEnabled());
    bool Opened = false, Saved = false;
    QTimer Editor;
    connect(&Editor, &QTimer::timeout, this, [&] {
      auto *Dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!Dialog || Dialog->objectName() != "dataPresentationDialog")
        return;
      if (Kind == "enum") {
        Dialog->findChild<QLineEdit *>("dataPresentationType")
            ->setText("GUI_FLAGS");
        Dialog->findChild<QCheckBox *>("enumFlags")->setChecked(true);
      } else if (Kind == "array") {
        Dialog->findChild<QSpinBox *>("arrayElementSize")->setValue(4);
        Dialog->findChild<QSpinBox *>("arrayCount")->setValue(4);
      }
      Opened = true;
      Dialog->accept();
    });
    Editor.start(20);
    Action->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(Opened, 30000);
    Editor.stop();
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
          Session.read(
              "listing",
              {{"address", hexAddress(Address)}, {"before", 0}, {"after", 40}},
              &Window, [&](const QJsonObject &Reply) {
                const auto Text =
                    QString::fromUtf8(QJsonDocument(Reply).toJson());
                Saved =
                    Text.contains(Kind == "enum" ? "GUI_READ | GUI_WRITE | 0x8"
                                  : Kind == "array" ? "array element"
                                                    : "align");
              });
          return Saved;
        })(),
        30000);
    Session.closeFile();
    Window.close();
  }
  void stackFrameDialogs_data() {
    QTest::addColumn<QString>("Offset");
    QTest::addColumn<QString>("Width");
    QTest::addColumn<QString>("After");
    QTest::newRow("decimal") << "-16" << "8" << "-40";
    QTest::newRow("leading-zero-decimal") << "-016" << "08" << "-040";
    QTest::newRow("explicit-hex") << "-0x10" << "0x8" << "-0x28";
  }
  void stackFrameDialogs() {
    QFETCH(QString, Offset);
    QFETCH(QString, Width);
    QFETCH(QString, After);
    QTemporaryDir Directory;
    const auto Path = fixture(
        Directory, QByteArray::fromHex(
                       "4883ec2848897c2418e8c3ff0f00488b4424184883c428c3"));
    QVERIFY(!Path.isEmpty());
    Session Session(QString::fromLocal8Bit(TEST_REAL_WORKER));
    McpConnectionManager Mcp;
    GuiSessionBroker Broker;
    MainWindow Window(Session, Mcp, Broker);
    Window.setAsksForDebugInfo(false);
    Window.setLayoutPath(Directory.filePath("layout.json"));
    Window.resize(1400, 900);
    Window.initializeLayout();
    Window.show();
    Session.open(Path);
    QTRY_VERIFY_WITH_TIMEOUT(Session.loaded(), 30000);
    Window.disassembly()->navigate(0x40007C);
    QTRY_COMPARE_WITH_TIMEOUT(Window.disassembly()->currentFunction(),
                              std::optional<Address>(0x400078), 30000);
    Window.disassembly()->listing()->setFocus();
    auto *Action = Window.actions().action(ActionId::EditStackFrame);
    QTRY_VERIFY(Action->isEnabled());
    bool Opened = false;
    QTimer Editor;
    connect(&Editor, &QTimer::timeout, this, [&] {
      auto *Dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!Dialog || Dialog->objectName() != "stackFrameDialog")
        return;
      auto *Table = Dialog->findChild<QTableWidget *>("stackVariablesTable");
      if (!Table) {
        Dialog->reject();
        return;
      }
      for (int Row = 0; Row < Table->rowCount(); ++Row)
        if (Table->item(Row, 0)->text() == "-16") {
          Table->item(Row, 0)->setText(Offset);
          Table->item(Row, 1)->setText(Width);
          Table->item(Row, 2)->setText("gui_saved_context");
          Table->item(Row, 3)->setText("uint64_t");
          Opened = true;
        }
      Dialog->findChild<QDialogButtonBox *>()
          ->button(QDialogButtonBox::Apply)
          ->click();
    });
    Editor.start(20);
    Action->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(Opened, 30000);
    Editor.stop();
    bool Saved = false;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
          Session.read(
              "stack_frame", {{"address", "0x400078"}}, &Window,
              [&](const QJsonObject &Reply) {
                const auto Variables = Reply.value("policy")
                                           .toObject()
                                           .value("variables")
                                           .toArray();
                Saved = Variables.size() == 1 &&
                        Variables[0].toObject().value("offset").toInteger() ==
                            -16 &&
                        Variables[0].toObject().value("bytes").toInt() == 8;
              });
          return Saved;
        })(),
        30000);
    // The SP editor uses the same numeric grammar as stack slots. All three
    // spellings retain the actual sub rsp, 40 transition and the named slot.
    Window.disassembly()->navigate(0x400078);
    QTRY_COMPARE_WITH_TIMEOUT(Window.disassembly()->currentItem(),
                              std::optional<Address>(0x400078), 30000);
    Window.disassembly()->listing()->setFocus();
    Action = Window.actions().action(ActionId::EditStackPointer);
    QTRY_VERIFY(Action->isEnabled());
    bool PointerOpened = false;
    QTimer PointerEditor;
    connect(&PointerEditor, &QTimer::timeout, this, [&] {
      auto *Dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!Dialog || Dialog->objectName() != "stackFrameDialog")
        return;
      auto *Input = Dialog->findChild<QLineEdit *>("stackPointerAfter");
      if (!Input) {
        Dialog->reject();
        return;
      }
      Input->setText(After);
      PointerOpened = true;
      PointerEditor.stop();
      Dialog->findChild<QDialogButtonBox *>()
          ->button(QDialogButtonBox::Apply)
          ->click();
    });
    PointerEditor.start(20);
    Action->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(PointerOpened, 30000);
    Saved = false;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
          Session.read("stack_frame", {{"address", "0x400078"}}, &Window,
                       [&](const QJsonObject &Reply) {
                         const auto Policy = Reply.value("policy").toObject();
                         const auto Points = Policy.value("points").toArray();
                         Saved =
                             Points.size() == 1 &&
                             Points[0].toObject().value("after").toInteger() ==
                                 -40 &&
                             Policy.value("variables").toArray().size() == 1;
                       });
          return Saved;
        })(),
        30000);
    Session.closeFile();
    Window.close();
  }
  void unknownStackPointerRequiresAnExplicitValue() {
    QTemporaryDir Directory;
    const auto Path =
        fixture(Directory, QByteArray::fromHex("4883e4f0488b0424c3"));
    QVERIFY(!Path.isEmpty());
    Session Session(QString::fromLocal8Bit(TEST_REAL_WORKER));
    McpConnectionManager Mcp;
    GuiSessionBroker Broker;
    MainWindow Window(Session, Mcp, Broker);
    Window.setAsksForDebugInfo(false);
    Window.setLayoutPath(Directory.filePath("layout.json"));
    Window.resize(1400, 900);
    Window.initializeLayout();
    Window.show();
    Session.open(Path);
    QTRY_VERIFY_WITH_TIMEOUT(Session.loaded(), 30000);
    Window.disassembly()->navigate(0x400078);
    QTRY_COMPARE_WITH_TIMEOUT(Window.disassembly()->currentFunction(),
                              std::optional<Address>(0x400078), 30000);
    Window.disassembly()->listing()->setFocus();
    auto *Action = Window.actions().action(ActionId::EditStackPointer);
    QTRY_VERIFY(Action->isEnabled());
    bool Opened = false, Empty = false;
    QTimer Editor;
    connect(&Editor, &QTimer::timeout, this, [&] {
      auto *Dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!Dialog || Dialog->objectName() != "stackFrameDialog")
        return;
      auto *After = Dialog->findChild<QLineEdit *>("stackPointerAfter");
      Opened = After != nullptr;
      Empty = After && After->text().isEmpty();
      Editor.stop();
      Dialog->reject();
    });
    Editor.start(20);
    QTimer::singleShot(30000, &Window, [] {
      if (auto *Dialog =
              qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        Dialog->reject();
    });
    Action->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(Opened, 30000);
    QVERIFY2(Empty, "An unknown SP must not offer zero as a correction");
    Session.closeFile();
    Window.close();
  }
  void localType_data() {
    QTest::addColumn<QString>("Representation");
    QTest::addColumn<QString>("Name");
    QTest::newRow("pseudocode") << "source" << "v4_0";
    QTest::newRow("llvm-c") << "llvmc" << "call0";
  }
  void localType() {
    QFETCH(QString, Representation);
    QFETCH(QString, Name);
    QTemporaryDir Directory;
    const QString Path = fixture(Directory);
    QVERIFY(!Path.isEmpty());
    Session Session(QString::fromLocal8Bit(TEST_REAL_WORKER));
    McpConnectionManager Mcp;
    GuiSessionBroker Broker;
    MainWindow Window(Session, Mcp, Broker);
    Window.setAsksForDebugInfo(false);
    Window.setLayoutPath(Directory.filePath("layout.json"));
    Window.resize(1400, 900);
    Window.initializeLayout();
    Window.show();
    Session.open(Path);
    QTRY_VERIFY_WITH_TIMEOUT(Session.loaded(), 30000);
    bool Declared = false;
    Session.declareTypes("c",
                         "struct GUI_CONTEXT { unsigned int State; unsigned "
                         "int Length; unsigned int Flags; };",
                         [&](const QJsonObject &Reply) {
                           Declared =
                               Reply.value("problems").toArray().isEmpty();
                         });
    QTRY_VERIFY_WITH_TIMEOUT(Declared, 30000);
    Window.disassembly()->navigate(0x400078);
    QTRY_COMPARE_WITH_TIMEOUT(Window.disassembly()->currentFunction(),
                              std::optional<Address>(0x400078), 30000);
    Window.actions()
        .action(Representation == "source" ? ActionId::ViewPseudocode
                                           : ActionId::ViewLLVMC)
        ->trigger();
    CodeView *View = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
          for (auto *Candidate : Window.findChildren<CodeView *>())
            if (Candidate->representation() == Representation)
              View = Candidate;
          return View && !View->text()->loading() &&
                 View->text()->allText().contains(Name);
        })(),
        30000);
    auto *Code = View->text();
    QVERIFY(Code->findText(Name, true));
    Code->setFocus();
    QTRY_VERIFY(Code->renameTarget() &&
                Code->renameTarget()->contains("variable_id"));
    QTimer Editor;
    bool Edited = false, Refused = false;
    int Stage = 0;
    QString Diagnostic;
    connect(&Editor, &QTimer::timeout, this, [&] {
      auto *Dialog =
          qobject_cast<SetTypeDialog *>(QApplication::activeModalWidget());
      if (!Dialog)
        return;
      auto *Field = Dialog->findChild<QLineEdit *>("setTypeField");
      auto *Status = Dialog->findChild<QLabel *>("setTypeStatus");
      auto *Apply = Dialog->findChild<QPushButton *>("setTypeApply");
      Diagnostic = Dialog->findChild<QLabel *>("setTypeSubject")->text() +
                   " / " + Status->text() + " / " + Field->text();
      if (!Field || !Field->isEnabled())
        return;
      if (Stage == 0) {
        QTest::keyClicks(Field, "unsigned int");
        Stage = 1;
      } else if (Stage == 1 && Status->text().startsWith(QStringLiteral("✗"))) {
        Refused = !Apply->isEnabled();
        Field->selectAll();
        QTest::keyClicks(Field, "struct GUI_CONTEXT *");
        Stage = 2;
      } else if (Stage == 2 && Apply->isEnabled()) {
        Editor.stop();
        Edited = true;
        Apply->click();
      }
    });
    QTimer::singleShot(30000, &Window, [] {
      if (auto *Dialog =
              qobject_cast<SetTypeDialog *>(QApplication::activeModalWidget()))
        Dialog->reject();
    });
    Editor.start(10);
    Window.actions().action(ActionId::EditSetType)->trigger();
    QVERIFY2(Edited && Refused,
             qPrintable(QString::number(Stage) + ": " + Diagnostic));
    QTRY_VERIFY_WITH_TIMEOUT(Code->allText().contains("->Length") &&
                                 Code->allText().contains("->Flags"),
                             30000);
    Session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(!Code->allText().contains("->Length"), 30000);
    Session.redo();
    QTRY_VERIFY_WITH_TIMEOUT(Code->allText().contains("->Length"), 30000);
    QVERIFY2(Session.lastError().isEmpty(), qPrintable(Session.lastError()));
  }
  void callSiteType_data() {
    QTest::addColumn<QString>("Representation");
    QTest::newRow("pseudocode") << "source";
    QTest::newRow("llvm-c") << "llvmc";
  }
  void callSiteType() {
    QFETCH(QString, Representation);
    QTemporaryDir Directory;
    const auto Code =
        QByteArray::fromHex(
            "bf07000000be0b000000ba0d000000e82c000000bf11000000e822000000c3")
            .leftJustified(64, char(0x90)) +
        QByteArray::fromHex("89f8c3");
    const auto Path = fixture(Directory, Code);
    Session Session(QString::fromLocal8Bit(TEST_REAL_WORKER));
    McpConnectionManager Mcp;
    GuiSessionBroker Broker;
    MainWindow Window(Session, Mcp, Broker);
    Window.setAsksForDebugInfo(false);
    Window.setLayoutPath(Directory.filePath("layout.json"));
    Window.resize(1400, 900);
    Window.initializeLayout();
    Window.show();
    Session.open(Path);
    QTRY_VERIFY_WITH_TIMEOUT(Session.loaded(), 30000);
    Window.disassembly()->navigate(0x400078);
    QTRY_COMPARE_WITH_TIMEOUT(Window.disassembly()->currentFunction(),
                              std::optional<Address>(0x400078), 30000);
    Window.actions()
        .action(Representation == "source" ? ActionId::ViewPseudocode
                                           : ActionId::ViewLLVMC)
        ->trigger();
    CodeView *View = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
          for (auto *Candidate : Window.findChildren<CodeView *>())
            if (Candidate->representation() == Representation)
              View = Candidate;
          return View && !View->text()->loading() &&
                 View->text()->allText().contains("sub_4000B8(7");
        })(),
        30000);
    QVERIFY(View->text()->findText("sub_4000B8(7", true));
    View->text()->setFocus();
    QTRY_VERIFY(Window.actions().action(ActionId::EditCallType)->isEnabled());
    bool Edited = false;
    QTimer Editor;
    connect(&Editor, &QTimer::timeout, this, [&] {
      auto *Dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (auto *Picker = qobject_cast<QInputDialog *>(Dialog);
          Picker && Picker->windowTitle() == "Select call instruction") {
        Picker->accept();
        return;
      }
      if (!Dialog || Dialog->objectName() != "callTypeDialog")
        return;
      Dialog->findChild<QPlainTextEdit *>("callTypeDeclaration")
          ->setPlainText("uint64_t call(uint32_t first, ...);");
      Dialog->findChild<QPlainTextEdit *>("callTypeActuals")
          ->setPlainText("unsigned int\nunsigned int");
      Edited = true;
      Editor.stop();
      Dialog->findChild<QPushButton *>("callTypeApply")->click();
    });
    Editor.start(10);
    QTimer::singleShot(30000, &Window, [] {
      auto *Dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (Dialog)
        Dialog->reject();
    });
    Window.actions().action(ActionId::EditCallType)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(Edited, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        View->text()->allText().contains("(*)(uint32_t, ...)"), 30000);
    QVERIFY2(Session.lastError().isEmpty(), qPrintable(Session.lastError()));
    Session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(
        !View->text()->allText().contains("(*)(uint32_t, ...)"), 30000);
    Session.redo();
    QTRY_VERIFY_WITH_TIMEOUT(
        View->text()->allText().contains("(*)(uint32_t, ...)"), 30000);
    Window.activateWindow();
    View->text()->setFocus();
    QTRY_VERIFY_WITH_TIMEOUT(
        !View->text()->loading() &&
            Window.actions().action(ActionId::EditResetCallTypes)->isEnabled(),
        30000);
    Window.actions().action(ActionId::EditResetCallTypes)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !View->text()->allText().contains("(*)(uint32_t, ...)"), 30000);
  }
  void variableAssociations() {
    QTemporaryDir Directory;
    const auto Path = fixture(
        Directory,
        QByteArray::fromHex("4883ec2848897c2418e87aff0f00488b7c24184883c701e86c"
                            "ff0f004889442418e862ff0f00488b4424184883c428c3"));
    Session Session(QString::fromLocal8Bit(TEST_REAL_WORKER));
    McpConnectionManager Mcp;
    GuiSessionBroker Broker;
    MainWindow Window(Session, Mcp, Broker);
    Window.setAsksForDebugInfo(false);
    Window.setLayoutPath(Directory.filePath("layout.json"));
    Window.resize(1400, 900);
    Window.initializeLayout();
    Window.show();
    Session.open(Path);
    QTRY_VERIFY_WITH_TIMEOUT(Session.loaded(), 30000);
    Window.disassembly()->navigate(0x400078);
    QTRY_COMPARE_WITH_TIMEOUT(Window.disassembly()->currentFunction(),
                              std::optional<Address>(0x400078), 30000);
    Window.actions().action(ActionId::ViewPseudocode)->trigger();
    CodeView *View = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        ([&] {
          for (auto *Candidate : Window.findChildren<CodeView *>())
            if (Candidate->representation() == "source")
              View = Candidate;
          return View && !View->text()->loading() &&
                 View->text()->allText().contains("var_m10");
        })(),
        30000);
    QVERIFY(View->text()->findText("var_m10", true));
    View->text()->setFocus();
    QTRY_VERIFY(View->text()->renameTarget() &&
                View->text()->renameTarget()->contains("variable_id"));
    QTRY_VERIFY(
        Window.actions().action(ActionId::EditSplitVariable)->isEnabled());
    Window.actions().action(ActionId::EditSplitVariable)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!View->text()->loading() &&
                                 !View->text()->allText().contains("var_m10"),
                             30000);
    QVERIFY2(Session.lastError().isEmpty(), qPrintable(Session.lastError()));
    Window.actions().action(ActionId::EditResetVariables)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(View->text()->allText().contains("var_m10"),
                             30000);
    Session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(!View->text()->allText().contains("var_m10"),
                             30000);
  }
};
QTEST_MAIN(SourceVariableTypesTests)
#include "SourceVariableTypesTests.moc"
