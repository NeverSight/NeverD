//===- PatchTests.cpp - Native patch keyboard regressions ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Route real QWindow key events through the production window and worker.
//===----------------------------------------------------------------------===//

#include "ActionRegistry.h"
#include "AddressSpace.h"
#include "ChooserView.h"
#include "CodeView.h"
#include "DisassemblyView.h"
#include "Docking.h"
#include "GraphView.h"
#include "HexView.h"
#include "JumpDialog.h"
#include "ListingView.h"
#include "MainWindow.h"
#include "OutputWindow.h"
#include "PatchEditor.h"
#include "PatchMenu.h"
#include "RewriteFunctionDialog.h"
#include "Session.h"
#include "Theme.h"
#include "WindowFrame.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QWindow>
#include <QtEndian>
#include <algorithm>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>

using namespace neverd::gui;

namespace {
constexpr Address FixtureBase = 0xffff800012340000ULL;
constexpr Address NativeBase = 0x400078;
constexpr int OpenTimeoutMs = 7000;

class PatchPaintGeometry final : public QObject {
public:
  QList<QRect> Frames;

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    if (event->type() == QEvent::Paint)
      if (auto *editor = qobject_cast<PatchEditor *>(object))
        Frames.append(editor->geometry());
    return QObject::eventFilter(object, event);
  }
};

class PatchWindowPaintSizes final : public QObject {
public:
  struct Window {
    QPointer<QWidget> Widget;
    QString Title;
    QElapsedTimer FirstPaint;
    QList<QSize> Sizes;
  };
  QList<Window> Windows;

  bool settled(QWidget *widget) const {
    for (const auto &window : Windows)
      if (window.Widget == widget)
        return window.FirstPaint.hasExpired(100);
    return false;
  }

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    if (event->type() != QEvent::Paint)
      return QObject::eventFilter(object, event);
    auto *widget = qobject_cast<QWidget *>(object);
    auto *chooser = qobject_cast<ChooserView *>(object);
    if (!qobject_cast<QDialog *>(object) &&
        (!chooser || chooser->model().kind() != ChooserKind::PatchedBytes))
      return QObject::eventFilter(object, event);
    for (auto &window : Windows)
      if (window.Widget == widget) {
        window.Sizes.append(widget->size());
        return false;
      }
    Window window;
    window.Widget = widget;
    window.Title = widget->windowTitle();
    window.FirstPaint.start();
    window.Sizes.append(widget->size());
    Windows.append(window);
    return false;
  }
};

bool privateWayland() {
  return QGuiApplication::platformName().startsWith(QLatin1String("wayland")) &&
         qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER");
}

// The isolated compositor runner supplies this capability. Never send Shell
// input or activation requests to the user's ordinary desktop session.
bool shell(const QString &script) {
  QProcess process;
  process.start(QStringLiteral("gdbus"),
                {QStringLiteral("call"), QStringLiteral("--session"),
                 QStringLiteral("--dest"), QStringLiteral("org.gnome.Shell"),
                 QStringLiteral("--object-path"),
                 QStringLiteral("/org/gnome/Shell"), QStringLiteral("--method"),
                 QStringLiteral("org.gnome.Shell.Eval"), script});
  return process.waitForFinished(OpenTimeoutMs) && process.exitCode() == 0 &&
         process.readAllStandardOutput().startsWith("(true,");
}

bool activate(QWidget &window) {
  if (privateWayland()) {
    if (!QTest::qWaitForWindowExposed(&window))
      return false;
    const auto title = QJsonDocument(QJsonArray{window.windowTitle()})
                           .toJson(QJsonDocument::Compact);
    const auto script =
        QStringLiteral(
            "Main.overview.hide(); const w = global.get_window_actors()"
            ".map(a => a.meta_window).find(w => w.get_pid() === %1 && "
            "w.get_title() === %2); if (w) Main.activateWindow(w);")
            .arg(QCoreApplication::applicationPid())
            .arg(QString::fromUtf8(title.mid(1, title.size() - 2)));
    return QTest::qWaitFor(
        [&] {
          if (QGuiApplication::focusWindow() == window.windowHandle())
            return true;
          shell(script);
          return false;
        },
        OpenTimeoutMs);
  }
  window.activateWindow();
  return QTest::qWaitForWindowActive(&window) && QTest::qWaitFor([&] {
           return QGuiApplication::focusWindow() == window.windowHandle();
         });
}

bool menuHasKey(QMenu *menu, int key) {
  const auto actions = menu->actions();
  return std::any_of(actions.cbegin(), actions.cend(), [key](QAction *action) {
    return action->isEnabled() && action->property("patchKey").toInt() == key;
  });
}

QByteArray nativeFixture(bool symbolized = false) {
  // Owned ELF64: conditional branch, MOV/RET, and a call back to the entry.
  const auto code = QByteArray::fromHex("7505b807000000c3e8f3ffffffc3");
  QByteArray bytes(120, '\0');
  bytes.replace(0, 7, QByteArray::fromHex("7f454c46020101"));
  const auto word = [&](qsizetype at, quint16 value) {
    qToLittleEndian(value, bytes.data() + at);
  };
  const auto dword = [&](qsizetype at, quint32 value) {
    qToLittleEndian(value, bytes.data() + at);
  };
  const auto quad = [&](qsizetype at, quint64 value) {
    qToLittleEndian(value, bytes.data() + at);
  };
  word(16, 2);
  word(18, 62);
  dword(20, 1);
  quad(24, NativeBase);
  quad(32, 64);
  word(52, 64);
  word(54, 56);
  word(56, 1);
  word(58, 64);
  dword(64, 1);
  dword(68, 5);
  quad(80, NativeBase - 120);
  quad(88, NativeBase - 120);
  quad(96, bytes.size() + code.size());
  quad(104, bytes.size() + code.size());
  quad(112, 4096);
  bytes += code;
  if (!symbolized)
    return bytes;
  // A stated function survives cache invalidation with discovery disabled.
  // Keep instruction ownership stable for the post-edit rendering checks.
  bytes.resize((bytes.size() + 7) & ~qsizetype(7));
  const auto symbols = bytes.size();
  bytes.append(QByteArray(48, '\0'));
  dword(symbols + 24, 1);
  bytes[symbols + 28] = 0x12; // STB_GLOBAL | STT_FUNC
  word(symbols + 30, 1);
  quad(symbols + 32, NativeBase);
  quad(symbols + 40, code.size());
  const auto strings = bytes.size();
  bytes.append(QByteArray("\0start\0", 7));
  bytes.resize((bytes.size() + 7) & ~qsizetype(7));
  const auto sections = bytes.size();
  bytes.append(QByteArray(4 * 64, '\0'));
  quad(40, sections);
  word(60, 4);
  dword(sections + 64 + 4, 1); // SHT_PROGBITS
  quad(sections + 64 + 8, 6);  // SHF_ALLOC | SHF_EXECINSTR
  quad(sections + 64 + 16, NativeBase);
  quad(sections + 64 + 24, 120);
  quad(sections + 64 + 32, code.size());
  quad(sections + 64 + 48, 1);
  dword(sections + 128 + 4, 2); // SHT_SYMTAB
  quad(sections + 128 + 24, symbols);
  quad(sections + 128 + 32, 48);
  dword(sections + 128 + 40, 3);
  dword(sections + 128 + 44, 1);
  quad(sections + 128 + 48, 8);
  quad(sections + 128 + 56, 24);
  dword(sections + 192 + 4, 3); // SHT_STRTAB
  quad(sections + 192 + 24, strings);
  quad(sections + 192 + 32, 7);
  quad(sections + 192 + 48, 1);
  return bytes;
}

QString currentBytes(Session &session, Address address, qsizetype size) {
  QEventLoop loop;
  QString bytes;
  bool finished = false;
  session.read("bytes", {{"address", hexAddress(address)}, {"size", size}},
               &loop, [&](const QJsonObject &reply) {
                 bytes = reply.value("data").toString();
                 finished = true;
                 loop.quit();
               });
  QTimer::singleShot(OpenTimeoutMs, &loop, &QEventLoop::quit);
  if (!finished)
    loop.exec();
  return bytes;
}
} // namespace

class PatchTests : public QObject {
  Q_OBJECT
  QTemporaryDir settings_;

private slots:
  void initTestCase() {
    QVERIFY(settings_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("NeverDTests"));
    QCoreApplication::setApplicationName(QStringLiteral("PatchTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settings_.path());
    auto &theme = Theme::instance();
    theme.setMode(qEnvironmentVariable("NEVERD_PATCH_THEME") == "light"
                      ? Theme::Mode::Light
                      : Theme::Mode::Dark);
    theme.apply();
    configureDocking();
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    WindowFrame::frameDialogs(*qApp);
    if (privateWayland())
      QVERIFY(shell(QStringLiteral(
          "Main.overview.hide(); global.neverdPatchPointer = "
          "imports.gi.Clutter.get_default_backend().get_default_seat()"
          ".create_virtual_device(imports.gi.Clutter.InputDeviceType.POINTER_"
          "DEVICE); "
          "global.neverdPatchPointer.notify_absolute_motion("
          "imports.gi.GLib.get_monotonic_time(), global.stage.width / 2, "
          "global.stage.height / 2);")));
  }

  void allSecondKeysOwnNativeShortcutDispatch_data() {
    QTest::addColumn<int>("id");
    QTest::addColumn<int>("key");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("ready");
    for (bool ready : {true, false}) {
#define NEVERD_PATCH_ITEM(Id, Key, Kind)                                       \
  QTest::newRow(                                                               \
      qPrintable(QStringLiteral(#Key) + (ready ? "-ready" : "-pending")))      \
      << int(ActionId::Id) << int(Qt::Key_##Key) << QStringLiteral(Kind)       \
      << ready;
#include "PatchMenu.def"
    }
  }

  void allSecondKeysOwnNativeShortcutDispatch() {
    QFETCH(int, id);
    QFETCH(int, key);
    QFETCH(QString, kind);
    QFETCH(bool, ready);
    QWidget window;
    window.setFocusPolicy(Qt::StrongFocus);
    ActionRegistry actions;
    actions.attachViewShortcuts(&window);
    actions.attachWindowShortcuts(&window);
    QMenu menu(&window);
    PatchMenu binding(&menu);
    if (ready)
      binding.fill(actions, {kind});
    else
      menu.addAction(QStringLiteral("Checking"))->setEnabled(false);
    auto *open = actions.action(ActionId::EditPatchMenu);
    connect(open, &QAction::triggered, &window,
            [&] { menu.popup(window.mapToGlobal(QPoint(10, 10))); });
    QSignalSpy openings(open, &QAction::triggered);
    QSignalSpy chosen(actions.action(ActionId(id)), &QAction::triggered);
    window.show();
    QVERIFY(activate(window));
    window.setFocus();
    QTest::keyClick(window.windowHandle(), Qt::Key_J);
    QTRY_VERIFY(menu.isVisible());
    QTest::keyClick(menu.windowHandle(), Qt::Key(key));
    if (!ready) {
      QCOMPARE(chosen.size(), 0);
      // Typing ahead must not replace the command already chosen while its
      // capabilities are loading, or leak a view shortcut behind the menu.
      QTest::keyClick(menu.windowHandle(), Qt::Key_X);
      binding.fill(actions, {kind});
    }
    QCOMPARE(chosen.size(), 1);
    QCOMPARE(openings.size(), 1);
    QVERIFY(!menu.isVisible());
  }

  void cancelledAndRepeatedKeysNeverPatch() {
    QWidget window;
    ActionRegistry actions;
    QMenu menu(&window);
    PatchMenu binding(&menu);
    menu.addAction(QStringLiteral("Checking"))->setEnabled(false);
    QSignalSpy nop(actions.action(ActionId::EditPatchNop), &QAction::triggered);
    window.show();
    QVERIFY(activate(window));
    menu.popup(window.mapToGlobal(QPoint(10, 10)));
    QTRY_VERIFY(menu.isVisible());
    QKeyEvent repeated(QEvent::KeyPress, Qt::Key_J, Qt::NoModifier, "j", true);
    QApplication::sendEvent(&menu, &repeated);
    binding.fill(actions, {"nop"});
    QCOMPARE(nop.size(), 0);
    QVERIFY(menu.isVisible());
    QApplication::sendEvent(&menu, &repeated);
    QCOMPARE(nop.size(), 0);
    QTest::keyClick(menu.windowHandle(), Qt::Key_Escape);
    QVERIFY(!menu.isVisible());
    QMenu pending(&window);
    PatchMenu pendingBinding(&pending);
    pending.addAction(QStringLiteral("Checking"))->setEnabled(false);
    pending.popup(window.mapToGlobal(QPoint(10, 10)));
    QTest::keyClick(pending.windowHandle(), Qt::Key_J);
    QTest::keyClick(pending.windowHandle(), Qt::Key_Escape);
    pendingBinding.fill(actions, {"nop"});
    QCOMPARE(nop.size(), 0);
    // Shift/Caps-Lock must not make the fixed, displayed capital letters inert.
    menu.popup(window.mapToGlobal(QPoint(10, 10)));
    QTest::keyClick(menu.windowHandle(), Qt::Key_J, Qt::ShiftModifier);
    QCOMPARE(nop.size(), 1);
  }

  void secondKeyPatchesThroughTheNativePopup_data() {
    QTest::addColumn<int>("key");
    QTest::addColumn<bool>("waitForMenu");
    for (bool ready : {true, false})
      for (auto key : {Qt::Key_J, Qt::Key_A, Qt::Key_B})
        QTest::newRow(
            qPrintable(QString(QChar(key)) + (ready ? "-ready" : "-fast")))
            << int(key) << ready;
  }

  void secondKeyPatchesThroughTheNativePopup() {
    QFETCH(int, key);
    QFETCH(bool, waitForMenu);
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("keys.bin"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("fixture"), 7);
    file.close();
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    McpConnectionManager mcp;
    GuiSessionBroker broker;
    MainWindow window(session, mcp, broker);
    window.setLayoutPath(directory.filePath(QStringLiteral("layout.json")));
    window.resize(1400, 900);
    window.initializeLayout();
    window.show();
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    auto *disassembly = window.disassembly();
    disassembly->navigate(FixtureBase);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(FixtureBase));
    QVERIFY(activate(window));
    disassembly->focusContent();
    QTRY_VERIFY(disassembly->listing()->hasFocus());
    auto *open = window.actions().action(ActionId::EditPatchMenu);
    QTRY_VERIFY(open->isEnabled());
    QSignalSpy openings(open, &QAction::triggered);
    QTest::keyClick(window.windowHandle(), Qt::Key_J);
    QTRY_COMPARE(openings.size(), 1);
    auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    QVERIFY(popup);
    QCOMPARE(popup->objectName(), QStringLiteral("patchMenu"));
    if (waitForMenu)
      QTRY_VERIFY(menuHasKey(popup, key));
    QTimer confirmation;
    bool askedForConfirmation = false;
    connect(&confirmation, &QTimer::timeout, &window, [&] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
        if (box->objectName() == QStringLiteral("bytePatchConfirmation")) {
          askedForConfirmation = true;
          box->button(QMessageBox::Apply)->click();
        }
    });
    confirmation.start(5);
    QTimer guard;
    guard.setSingleShot(true);
    connect(&guard, &QTimer::timeout, &window, [] {
      if (auto *dialog =
              qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        dialog->reject();
    });
    guard.start(OpenTimeoutMs);
    // QWidget keyClick bypasses the native shortcut and popup routing that
    // broke real J-J taps. Keep both taps on their actual QWindow surfaces.
    QTest::keyClick(popup->windowHandle(), Qt::Key(key));
    if (key != Qt::Key_J) {
      QTRY_VERIFY_WITH_TIMEOUT(window.findChild<PatchEditor *>(),
                               OpenTimeoutMs);
      auto *editor = window.findChild<PatchEditor *>();
      auto *input =
          editor->findChild<QLineEdit *>(QStringLiteral("patchInput"));
      // The offscreen plugin cannot restore native focus after a popup.
      // Activating the owner still requires the editor to have requested its
      // own input focus; do not assign focus to the line edit in the test.
      QVERIFY(activate(*input->window()));
      if (key == Qt::Key_A) {
        QTRY_VERIFY(input->hasFocus());
        input->selectAll();
        QTest::keyClick(input->window()->windowHandle(), Qt::Key_J);
        QVERIFY(input->text().contains(QLatin1Char('j'), Qt::CaseInsensitive));
        QVERIFY(!QApplication::activePopupWidget());
      }
      input->setText(key == Qt::Key_A ? "ret" : "2a");
      auto *apply =
          editor->findChild<QPushButton *>(QStringLiteral("patchApply"));
      QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), OpenTimeoutMs);
      QTest::keyClick(input->window()->windowHandle(), Qt::Key_Return);
    }
    QTRY_VERIFY2_WITH_TIMEOUT(session.hasBytePatch(FixtureBase),
                              qPrintable(session.lastError()), OpenTimeoutMs);
    QCOMPARE(openings.size(), 1);
    QVERIFY(!QApplication::activePopupWidget());
    QVERIFY(!askedForConfirmation);
    QVERIFY(activate(window));
    disassembly->focusContent();
    QTest::keyClick(window.windowHandle(), Qt::Key_J);
    QPointer<QMenu> obsolete =
        qobject_cast<QMenu *>(QApplication::activePopupWidget());
    QVERIFY(obsolete);
    QTRY_VERIFY(menuHasKey(obsolete, Qt::Key_J));
    QSignalSpy nop(window.actions().action(ActionId::EditPatchNop),
                   &QAction::triggered);
    bool reverted = false;
    session.editBytePatch("patch_revert",
                          {{"address", hexAddress(FixtureBase)}, {"size", 7}},
                          session.epoch(), [&] { reverted = true; });
    QTRY_VERIFY(reverted);
    QTRY_VERIFY(!obsolete || !obsolete->isVisible());
    QCOMPARE(nop.size(), 0);
    // Colors are replaced atomically when the asynchronous range read arrives.
    QTRY_VERIFY(!session.hasBytePatches());
  }

  void patchEditorDismissalDoesNotApply_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("hex");
    QTest::addColumn<bool>("nibble");
    QTest::addColumn<int>("dismissal");
    for (int dismissal = 0; dismissal < 3; ++dismissal) {
      const auto suffix = dismissal == 0   ? "-close-click"
                          : dismissal == 1 ? "-close-enter"
                                           : "-cancel-enter";
      QTest::newRow(qPrintable(QStringLiteral("assembly") + suffix))
          << QString("assemble") << false << false << dismissal;
      QTest::newRow(qPrintable(QStringLiteral("bytes") + suffix))
          << QString("bytes") << false << false << dismissal;
      QTest::newRow(qPrintable(QStringLiteral("hex-bytes") + suffix))
          << QString("bytes") << true << false << dismissal;
      QTest::newRow(qPrintable(QStringLiteral("hex-f2") + suffix))
          << QString("bytes") << true << true << dismissal;
    }
  }

  void patchEditorDismissalDoesNotApply() {
    QFETCH(QString, kind);
    QFETCH(bool, hex);
    QFETCH(bool, nibble);
    QFETCH(int, dismissal);
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("close.bin"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("fixture"), 7);
    file.close();
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    McpConnectionManager mcp;
    GuiSessionBroker broker;
    MainWindow window(session, mcp, broker);
    window.setLayoutPath(directory.filePath(QStringLiteral("layout.json")));
    window.resize(1400, 900);
    window.initializeLayout();
    window.show();
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    auto *disassembly = window.disassembly();
    disassembly->navigate(FixtureBase);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(FixtureBase));
    auto *hexView = window.findChild<HexView *>();
    QWidget *view =
        hex ? static_cast<QWidget *>(hexView) : disassembly->listing();
    if (hex) {
      window.actions().action(ActionId::ViewHex)->trigger();
      hexView->setCurrent(FixtureBase);
      QTRY_VERIFY(hexView->byteAt(FixtureBase).has_value());
    }
    QVERIFY(activate(*view->window()));
    view->setFocus();
    const auto original = currentBytes(session, FixtureBase, 7);
    if (nibble)
      QTest::keyClick(view->window()->windowHandle(), Qt::Key_F2);
    else
      window.actions()
          .action(kind == "assemble" ? ActionId::EditPatchAssemble
                                     : ActionId::EditPatchBytes)
          ->trigger();
    QTRY_VERIFY(window.findChild<PatchEditor *>());
    QPointer<PatchEditor> editor = window.findChild<PatchEditor *>();
    auto *input = editor->findChild<QLineEdit *>("patchInput");
    auto *apply = editor->findChild<QPushButton *>("patchApply");
    auto *close = editor->findChild<QToolButton *>("patchClose");
    auto *cancel = editor->findChild<QPushButton *>("patchCancel");
    QVERIFY(input && apply && close && cancel);
    QCOMPARE(editor->cursor().shape(), Qt::ArrowCursor);
    QCOMPARE(apply->cursor().shape(), Qt::ArrowCursor);
    QCOMPARE(close->cursor().shape(), Qt::ArrowCursor);
    QCOMPARE(cancel->cursor().shape(), Qt::ArrowCursor);
    QCOMPARE(input->cursor().shape(), Qt::IBeamCursor);
    input->setText(kind == "assemble" ? "ret" : "2a");
    QTRY_VERIFY(apply->isEnabled());
    QVERIFY(close->isVisible() && close->isEnabled());
    if (const auto capture = qEnvironmentVariable("NEVERD_PATCH_CAPTURE_DIR");
        !capture.isEmpty() && dismissal == 0) {
      QDir().mkpath(capture);
      QVERIFY(editor->grab().save(capture + "/editor-" +
                                  QTest::currentDataTag() + ".png"));
      QVERIFY(window.grab().save(capture + "/window-" +
                                 QTest::currentDataTag() + ".png"));
    }
    QSignalSpy applied(editor, &PatchEditor::applied);
    QVERIFY(activate(*view->window()));
    if (dismissal == 0)
      QTest::mouseClick(close, Qt::LeftButton);
    else {
      auto *button =
          dismissal == 1 ? static_cast<QAbstractButton *>(close) : cancel;
      button->setFocus();
      QTRY_VERIFY(button->hasFocus());
      QTest::keyClick(view->window()->windowHandle(), Qt::Key_Return);
    }
    QTRY_VERIFY(!editor);
    QTRY_VERIFY(view->hasFocus());
    QCOMPARE(applied.size(), 0);
    QVERIFY(!session.hasBytePatches());
    QCOMPARE(currentBytes(session, FixtureBase, 7), original);
  }

  void patchEditorKeepsFirstPaintGeometry_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("initial");
    QTest::newRow("assembly") << QString("assemble") << QString("ret");
    QTest::newRow("bytes") << QString("bytes") << QString("2a");
    QTest::newRow("assembly-refusal")
        << QString("assemble") << QString("invalid");
  }

  void patchEditorKeepsFirstPaintGeometry() {
    QFETCH(QString, kind);
    QFETCH(QString, initial);
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("first-paint.bin"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("fixture"), 7);
    file.close();
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QWidget surface;
    surface.setWindowTitle(QStringLiteral("Patch first paint"));
    surface.resize(1000, 700);
    surface.show();
    QVERIFY(activate(surface));
    PatchPaintGeometry geometry;
    qApp->installEventFilter(&geometry);
    PatchEditor editor(session, FixtureBase, kind, initial, &surface);
    auto *preview = editor.findChild<QPlainTextEdit *>("patchPreview");
    auto *input = editor.findChild<QLineEdit *>("patchInput");
    auto *apply = editor.findChild<QPushButton *>("patchApply");
    const auto preparing = preview->toPlainText();
    editor.openAt(QRect(220, 400, 1, 20));
    QTRY_VERIFY(!geometry.Frames.isEmpty());
    const auto firstPaint = geometry.Frames.front();
    QTRY_VERIFY(preview->toPlainText() != preparing);
    QCOMPARE(editor.geometry(), firstPaint);
    if (kind == "assemble") {
      input->setText("mov eax, 1");
      QTRY_VERIFY(preview->toPlainText().contains("b8010000009090"));
      QVERIFY(!apply->isEnabled());
      QTRY_VERIFY(preview->verticalScrollBar()->maximum() > 0);
      QCOMPARE(editor.geometry(), firstPaint);
      editor.findChildren<QCheckBox *>().at(1)->setChecked(true);
      QTRY_VERIFY(apply->isEnabled());
      QCOMPARE(editor.geometry(), firstPaint);
    }
    input->setText("invalid");
    QTRY_VERIFY(preview->toPlainText().contains("invalid patch bytes"));
    QVERIFY(!apply->isEnabled());
    QCOMPARE(editor.geometry(), firstPaint);
    input->setText(kind == "assemble" ? "ret" : "2a");
    QTRY_VERIFY(apply->isEnabled());
    QCOMPARE(editor.geometry(), firstPaint);
    for (const auto &frame : geometry.Frames)
      QCOMPARE(frame, firstPaint);
    QVERIFY(!session.hasBytePatches());
  }

  void patchEditorDragsAndRetainsPosition() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("drag.bin"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("fixture"), 7);
    file.close();
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QWidget surface;
    surface.setWindowTitle(QStringLiteral("Patch dragging"));
    surface.setCursor(Qt::IBeamCursor);
    surface.resize(1000, 700);
    surface.show();
    QVERIFY(activate(surface));
    PatchEditor editor(session, FixtureBase, "assemble", "ret", &surface);
    const QRect anchor(220, 530, 1, 20);
    editor.openAt(anchor);
    auto *input = editor.findChild<QLineEdit *>("patchInput");
    auto *apply = editor.findChild<QPushButton *>("patchApply");
    auto *close = editor.findChild<QToolButton *>("patchClose");
    auto *header = editor.findChild<QWidget *>("patchHeader");
    QTRY_VERIFY(apply->isEnabled());
    QVERIFY(surface.rect().contains(editor.geometry()));
    QCOMPARE(editor.x(), anchor.x());
    QVERIFY(editor.geometry().bottom() < anchor.top());
    QCOMPARE(header->cursor().shape(), Qt::ArrowCursor);
    QCOMPARE(editor.findChild<QPlainTextEdit *>("patchPreview")
                 ->viewport()
                 ->cursor()
                 .shape(),
             Qt::IBeamCursor);
    QSignalSpy applied(&editor, &PatchEditor::applied);
    const auto drag = [&](const QPoint &delta) {
      const auto start = header->mapTo(&surface, header->rect().center());
      QTest::mousePress(surface.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                        start);
      QTest::mouseMove(surface.windowHandle(), start + delta);
      QTest::mouseRelease(surface.windowHandle(), Qt::LeftButton,
                          Qt::NoModifier, start + delta);
    };
    const QPoint parked(80, 70);
    drag(parked - editor.pos());
    QTRY_COMPARE(editor.pos(), parked);
    QVERIFY(input->hasFocus());
    editor.findChildren<QCheckBox *>().at(1)->setChecked(true);
    input->setText("mov eax, 1");
    QTRY_VERIFY(apply->isEnabled());
    QCOMPARE(editor.pos(), parked);
    surface.resize(700, 380);
    QTRY_VERIFY(surface.rect().contains(editor.geometry()));
    drag(QPoint(-1000, -1000));
    QTRY_COMPARE(editor.pos(), QPoint(0, 0));
    drag(QPoint(1000, 1000));
    QTRY_COMPARE(editor.geometry().bottomRight(), surface.rect().bottomRight());
    QVERIFY(close->isVisible());
    QCOMPARE(close->cursor().shape(), Qt::ArrowCursor);
    QCOMPARE(applied.size(), 0);
    QVERIFY(!session.hasBytePatches());
    if (const auto capture = qEnvironmentVariable("NEVERD_PATCH_CAPTURE_DIR");
        !capture.isEmpty()) {
      QDir().mkpath(capture);
      QVERIFY(surface.grab().save(capture + "/editor-dragged.png"));
    }
  }

  void patchPopupFollowsItsOrigin_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("floating");
    for (const QString &kind : {"listing", "graph", "hex", "source", "low"})
      QTest::newRow(qPrintable(kind)) << kind << false;
    for (const QString &kind : {"source", "graph", "hex"})
      QTest::newRow(qPrintable(kind + "-floating")) << kind << true;
  }

  void patchPopupFollowsItsOrigin() {
    QFETCH(QString, kind);
    QFETCH(bool, floating);
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("origin.bin"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("fixture"), 7);
    file.close();
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    McpConnectionManager mcp;
    GuiSessionBroker broker;
    MainWindow window(session, mcp, broker);
    window.setLayoutPath(directory.filePath(QStringLiteral("layout.json")));
    window.resize(1400, 900);
    window.initializeLayout();
    window.show();
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    auto *assembly = window.disassembly();
    assembly->navigate(FixtureBase);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(FixtureBase));
    QVERIFY(activate(window));
    assembly->focusContent();
    QWidget *pane = assembly;
    QWidget *view = assembly->listing();
    QWidget *surface = assembly->listing()->viewport();
    std::function<QRect()> caret = [&] {
      return assembly->listing()->cursorViewportRect();
    };
    const bool source = kind == "source" || kind == "low";
    if (source) {
      window.actions()
          .action(kind == "source" ? ActionId::ViewPseudocode
                                   : ActionId::ViewLowIR)
          ->trigger();
      CodeView *code = nullptr;
      QTRY_VERIFY((code = window.findChild<CodeView *>()));
      QTRY_VERIFY(code->text()->complete());
      code->text()->setCursorLine(1);
      pane = code;
      view = code->text();
      surface = code->text()->viewport();
      caret = [code] { return code->text()->cursorViewportRect(); };
    } else if (kind == "graph") {
      assembly->setGraphMode(true);
      QTRY_VERIFY(assembly->graph()->loaded());
      view = surface = assembly->graph();
      caret = [&] { return assembly->graph()->cursorViewportRect(); };
    } else if (kind == "hex") {
      window.actions().action(ActionId::ViewHex)->trigger();
      auto *hex = window.findChild<HexView *>();
      hex->setCurrent(FixtureBase + 9);
      QTRY_VERIFY(hex->byteAt(FixtureBase + 9).has_value());
      pane = view = hex;
      surface = hex->viewport();
      caret = [hex] { return hex->cursorViewportRect(); };
    }
    if (floating) {
      KDDockWidgets::QtWidgets::DockWidget *owner = nullptr;
      for (auto *dock :
           window.findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
        if (dock->widget() == pane)
          owner = dock;
      QVERIFY(owner);
      owner->setFloating(true);
      pane->window()->resize(1100, 700);
    }
    QVERIFY(activate(*view->window()));
    view->setFocus();
    QTRY_VERIFY(view->hasFocus());
    QTRY_VERIFY(window.actions().action(ActionId::EditPatchMenu)->isEnabled());
    // Deliberately put the pointer elsewhere; keyboard menus follow the caret.
    QTest::mouseMove(view->window()->windowHandle(), QPoint(30, 30));
    const auto anchor = caret();
    auto at = surface->mapToGlobal(
        QPoint(std::clamp(anchor.left(), 0, surface->width() - 1),
               std::clamp(anchor.bottom() + 1, 0, surface->height() - 1)));
    const auto screen = surface->screen()->availableGeometry();
    at.setX(std::clamp(at.x(), screen.left(), screen.right()));
    at.setY(std::clamp(at.y(), screen.top(), screen.bottom()));
    QTest::keyClick(view->window()->windowHandle(), Qt::Key_J);
    QPointer<QMenu> menu =
        qobject_cast<QMenu *>(QApplication::activePopupWidget());
    QVERIFY(menu);
    QTRY_VERIFY(menuHasKey(menu, Qt::Key_P));
    QCOMPARE(menu->parentWidget(), view->window());
    QCOMPARE(menu->cursor().shape(), Qt::ArrowCursor);
    QTRY_VERIFY2(menu->geometry().adjusted(-4, -4, 4, 4).contains(at),
                 qPrintable(QString("caret %1,%2, popup %3,%4 %5x%6")
                                .arg(at.x())
                                .arg(at.y())
                                .arg(menu->x())
                                .arg(menu->y())
                                .arg(menu->width())
                                .arg(menu->height())));
    if (const auto capture = qEnvironmentVariable("NEVERD_PATCH_CAPTURE_DIR");
        !capture.isEmpty()) {
      QDir().mkpath(capture);
      QVERIFY(view->window()->grab().save(capture + "/origin-" +
                                          QTest::currentDataTag() + ".png"));
      QVERIFY(menu->grab().save(capture + "/menu-" + QTest::currentDataTag() +
                                ".png"));
    }
    if (source) {
      QTest::keyClick(menu->windowHandle(), Qt::Key_Escape);
    } else {
      QTRY_VERIFY(menuHasKey(menu, Qt::Key_B));
      QTest::keyClick(menu->windowHandle(), Qt::Key_B);
      // Change focus before the worker answers the editor's context request.
      window.setFocus(Qt::OtherFocusReason);
      QTRY_VERIFY(surface->findChild<PatchEditor *>());
      auto *editor = surface->findChild<PatchEditor *>();
      QCOMPARE(editor->parentWidget(), surface);
      QVERIFY(surface->rect().contains(editor->geometry()));
      auto *input = editor->findChild<QLineEdit *>("patchInput");
      QVERIFY(activate(*view->window()));
      QTRY_VERIFY(input->hasFocus());
      QTest::keyClick(view->window()->windowHandle(), Qt::Key_Escape);
    }
    QTRY_VERIFY(!menu || !menu->isVisible());
    QVERIFY(!session.hasBytePatches());
  }

  void patchHistoryKeepsUnchangedFunctionRows_data() {
    QTest::addColumn<int>("kind");
    QTest::addColumn<bool>("sorted");
    QTest::newRow("functions") << int(ChooserKind::Functions) << false;
    QTest::newRow("functions-sorted-filtered")
        << int(ChooserKind::Functions) << true;
    QTest::newRow("names") << int(ChooserKind::Names) << false;
  }

  void patchHistoryKeepsUnchangedFunctionRows() {
    QFETCH(int, kind);
    QFETCH(bool, sorted);
    QTemporaryDir directory;
    const auto path =
        directory.filePath(QStringLiteral("function-refresh.bin"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("fixture"), 7);
    file.close();
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    AddressSpace space;
    ChooserView view(session, space, ChooserKind(kind));
    view.resize(720, 480);
    view.show();
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    auto &model = view.model();
    model.reload();
    QTRY_VERIFY(model.total() > 0 && !model.loading());
    if (sorted) {
      model.setFilter("function_");
      model.sort(0, Qt::DescendingOrder);
      QTRY_VERIFY(model.total() > 0 && !model.loading());
    }
    auto *table = view.table();
    const int row = std::min(300, model.total() - 1);
    table->setCurrentIndex(model.index(row, 0));
    table->scrollTo(model.index(row, 0), QAbstractItemView::PositionAtCenter);
    QTRY_VERIFY(!model.rowObject(row).isEmpty());
    QTRY_VERIFY(!model.loading() && !session.queries().hasPending());
    const auto address = view.currentAddress();
    QVERIFY(address);
    const int top = table->verticalScrollBar()->value();
    const int total = model.total();
    QPersistentModelIndex selected(table->currentIndex());
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy layouts(&model, &QAbstractItemModel::layoutChanged);
    QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy counts(&model, &ChooserModel::totalChanged);
    for (int step = 0; step < 4; ++step) {
      const auto revision = session.revision();
      if (step == 0)
        session.editBytePatch("patch_apply",
                              {{"address", hexAddress(FixtureBase)},
                               {"kind", "bytes"},
                               {"bytes", "90"}},
                              session.epoch());
      else if (step == 1) {
        QTRY_VERIFY(session.canUndo());
        session.undo();
      } else if (step == 2) {
        QTRY_VERIFY(session.canRedo());
        session.redo();
      } else
        session.editBytePatch(
            "patch_revert", {{"address", hexAddress(FixtureBase)}, {"size", 1}},
            session.epoch());
      QTRY_VERIFY(session.revision() != revision);
      QTRY_COMPARE(session.hasBytePatches(), step == 0 || step == 2);
      QTRY_VERIFY(!model.loading() && !session.queries().hasPending());
      QCOMPARE(model.total(), total);
      QCOMPARE(resets.count(), 0);
      QCOMPARE(layouts.count(), 0);
      QCOMPARE(changes.count(), 0);
      QCOMPARE(counts.count(), 0);
      QVERIFY(selected.isValid());
      QCOMPARE(table->currentIndex(), QModelIndex(selected));
      QCOMPARE(view.currentAddress(), address);
      QCOMPARE(table->verticalScrollBar()->value(), top);
    }
    // Real table changes must still arrive, including a name-sorted row moving
    // beyond the pages that were cached before the edit.
    const auto revision = session.revision();
    session.rename(*address, "function_renamed", session.epoch());
    QTRY_VERIFY(session.revision() != revision);
    QTRY_VERIFY(!model.loading() && !session.queries().hasPending());
    QCOMPARE(view.currentAddress(), address);
    QVERIFY(view.selectedText().contains("function_renamed"));
    QCOMPARE(resets.count(), 0);
    // A different project must discard the old inventory and edited names.
    const auto other =
        directory.filePath(QStringLiteral("another-project.bin"));
    QFile next(other);
    QVERIFY(next.open(QIODevice::WriteOnly));
    QCOMPARE(next.write("fixture"), 7);
    next.close();
    const auto epoch = session.epoch();
    session.open(other);
    QTRY_VERIFY(session.loaded() && session.epoch() != epoch);
    QTRY_VERIFY(model.total() > 0 && !model.loading());
    QVERIFY(resets.count() > 0);
    QVERIFY(!selected.isValid());
  }

  void patchColorsStayVisibleAcrossUnrelatedEdits() {
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("patch-colors.bin"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("fixture"), 7);
    file.close();
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    session.editBytePatch("patch_apply",
                          {{"address", hexAddress(FixtureBase)},
                           {"kind", "bytes"},
                           {"bytes", "90"}},
                          session.epoch());
    QTRY_VERIFY(session.hasBytePatch(FixtureBase));
    QTRY_VERIFY(!session.queries().hasPending());
    QSignalSpy colors(&session, &Session::bytePatchesChanged);
    bool disappeared = false;
    connect(&session, &Session::bytePatchesChanged, &session,
            [&] { disappeared |= !session.hasBytePatch(FixtureBase); });
    const auto revision = session.revision();
    session.rename(FixtureBase, "renamed_with_patch", session.epoch());
    QTRY_VERIFY(session.revision() != revision);
    QTRY_VERIFY(!session.queries().hasPending());
    QVERIFY(!disappeared);
    QCOMPARE(colors.count(), 0);
    QVERIFY(session.hasBytePatch(FixtureBase));
    // Opening another input must still retire the old project's colors.
    const auto other = directory.filePath(QStringLiteral("unpatched.bin"));
    QFile next(other);
    QVERIFY(next.open(QIODevice::WriteOnly));
    QCOMPARE(next.write("fixture"), 7);
    next.close();
    const auto epoch = session.epoch();
    session.open(other);
    QTRY_VERIFY(session.loaded() && session.epoch() != epoch);
    QTRY_VERIFY(!session.queries().hasPending());
    QVERIFY(!session.hasBytePatches());
    QVERIFY(disappeared);
  }

  void nativeInstructionKeysApplyUndoAndRedo_data() {
    QTest::addColumn<int>("key");
    QTest::addColumn<int>("offset");
    QTest::addColumn<QString>("expected");
    QTest::addColumn<bool>("hex");
    QTest::addColumn<bool>("floating");
    QTest::newRow("J-listing")
        << int(Qt::Key_J) << 0 << QString("9090") << false << false;
    QTest::newRow("J-hex") << int(Qt::Key_J) << 0 << QString("9090") << true
                           << false;
    QTest::newRow("J-floating")
        << int(Qt::Key_J) << 0 << QString("9090") << false << true;
    QTest::newRow("G") << int(Qt::Key_G) << 0 << QString("eb05") << false
                       << false;
    QTest::newRow("N") << int(Qt::Key_N) << 0 << QString("9090") << false
                       << false;
    QTest::newRow("I") << int(Qt::Key_I) << 0 << QString("7405") << false
                       << false;
    QTest::newRow("S") << int(Qt::Key_S) << 8 << QString("9090909090") << false
                       << false;
    QTest::newRow("A") << int(Qt::Key_A) << 0 << QString("c390") << false
                       << false;
    QTest::newRow("B") << int(Qt::Key_B) << 0 << QString("2a") << false
                       << false;
    QTest::newRow("B-hex-operand")
        << int(Qt::Key_B) << 3 << QString("2a") << true << false;
    QTest::newRow("C") << int(Qt::Key_C) << 2 << QString("b82a000000") << false
                       << false;
    QTest::newRow("K") << int(Qt::Key_K) << 8 << QString("b82a000000") << false
                       << false;
    QTest::newRow("F") << int(Qt::Key_F) << 0 << QString("90909090909090")
                       << true << false;
  }

  void nativeInstructionKeysApplyUndoAndRedo() {
    const auto worker = qEnvironmentVariable("NEVERD_PATCH_NATIVE_WORKER");
    if (worker.isEmpty())
      QSKIP("NEVERD_PATCH_NATIVE_WORKER is not configured");
    QFETCH(int, key);
    QFETCH(int, offset);
    QFETCH(QString, expected);
    QFETCH(bool, hex);
    QFETCH(bool, floating);
    PatchWindowPaintSizes sizes;
    qApp->installEventFilter(&sizes);
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("keys.elf"));
    const auto original = nativeFixture(key == Qt::Key_A);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(original), original.size());
    file.close();
    Session session(worker);
    McpConnectionManager mcp;
    GuiSessionBroker broker;
    MainWindow window(session, mcp, broker);
    window.setLayoutPath(directory.filePath(QStringLiteral("layout.json")));
    window.resize(1400, 900);
    window.initializeLayout();
    window.show();
    LoadOptions options;
    options.analysis = false;
    session.open(path, options);
    QTRY_VERIFY2_WITH_TIMEOUT(session.loaded(), qPrintable(session.lastError()),
                              OpenTimeoutMs);
    QVERIFY2(session.keepsBytePatches(),
             "The worker SDK lacks byte patch APIs; check the library beside "
             "neverd-worker.");
    auto *disassembly = window.disassembly();
    const auto address = NativeBase + Address(offset);
    const auto initialAddress = hex ? NativeBase : address;
    disassembly->navigate(initialAddress);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(initialAddress));
    if (key == Qt::Key_C) {
      // This symbol-free fixture disables discovery. Give JW an explicit
      // MOV/RET function rather than relying on automatic entry recognition.
      session.createFunction(address, session.epoch());
      QTRY_VERIFY(session.canUndo());
      QTRY_COMPARE(disassembly->currentFunction(),
                   std::optional<Address>(address));
    }
    auto *hexView = window.findChild<HexView *>();
    QWidget *view =
        hex ? static_cast<QWidget *>(hexView) : disassembly->listing();
    for (auto *parent = view; parent; parent = parent->parentWidget())
      if (auto *dock =
              qobject_cast<KDDockWidgets::QtWidgets::DockWidget *>(parent)) {
        if (floating)
          dock->setFloating(true);
        dock->raise();
        break;
      }
    QVERIFY(activate(*view->window()));
    if (hex)
      hexView->setCurrent(initialAddress);
    view->setFocus();
    QTRY_VERIFY(view->hasFocus());
    if (hex && offset) {
      for (int i = 0; i < offset; ++i)
        QTest::keyClick(view->window()->windowHandle(), Qt::Key_Right);
      QTRY_VERIFY(!disassembly->listing()->jumpPending());
      QCOMPARE(hexView->currentAddress(), std::optional<Address>(address));
    }
    if (key == Qt::Key_F)
      for (int i = 0; i < 6; ++i)
        QTest::keyClick(view->window()->windowHandle(), Qt::Key_Right,
                        Qt::ShiftModifier);
    if (key == Qt::Key_F) {
      QTRY_VERIFY(!disassembly->listing()->jumpPending());
      QCOMPARE(hexView->selection(),
               (std::optional<std::pair<Address, Address>>(
                   {NativeBase, NativeBase + 6})));
    }
    QTRY_VERIFY(window.actions().action(ActionId::EditPatchMenu)->isEnabled());
    QTest::keyClick(view->window()->windowHandle(), Qt::Key_J);
    auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    QVERIFY(popup);
    QTRY_VERIFY(menuHasKey(popup, key));
    if (const auto capture = qEnvironmentVariable("NEVERD_PATCH_CAPTURE_DIR");
        !capture.isEmpty()) {
      QDir().mkpath(capture);
      QVERIFY(popup->grab().save(capture + "/patch-keys-" +
                                 QTest::currentDataTag() + ".png"));
    }
    int confirmations = 0;
    QTimer answer;
    connect(&answer, &QTimer::timeout, &window, [&] {
      if (auto *input =
              qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
        if (!sizes.settled(input))
          return;
        input->setTextValue(QStringLiteral("042"));
        input->accept();
      } else if (auto *box = qobject_cast<QMessageBox *>(
                     QApplication::activeModalWidget())) {
        if (box->objectName() == QStringLiteral("bytePatchConfirmation")) {
          if (!sizes.settled(box))
            return;
          ++confirmations;
          if (const auto capture =
                  qEnvironmentVariable("NEVERD_PATCH_CAPTURE_DIR");
              !capture.isEmpty()) {
            QDir().mkpath(capture);
            QVERIFY(box->grab().save(capture + "/confirmation-" +
                                     QTest::currentDataTag() + ".png"));
          }
          box->button(QMessageBox::Apply)->click();
        }
      }
    });
    answer.start(5);
    QTimer guard;
    guard.setSingleShot(true);
    connect(&guard, &QTimer::timeout, &window, [] {
      if (auto *dialog =
              qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        dialog->reject();
    });
    guard.start(OpenTimeoutMs);
    QTest::keyClick(popup->windowHandle(), Qt::Key(key));
    if (key == Qt::Key_A || key == Qt::Key_B) {
      QTRY_VERIFY_WITH_TIMEOUT(window.findChild<PatchEditor *>(),
                               OpenTimeoutMs);
      QPointer<PatchEditor> editor = window.findChild<PatchEditor *>();
      auto *input =
          editor->findChild<QLineEdit *>(QStringLiteral("patchInput"));
      // The offscreen plugin cannot restore native focus after a popup.
      // Activating the owner still requires the editor to have requested its
      // own input focus; do not assign focus to the line edit in the test.
      QVERIFY(activate(*input->window()));
      if (key == Qt::Key_A) {
        QTRY_VERIFY(input->hasFocus());
        input->selectAll();
        QTest::keyClick(input->window()->windowHandle(), Qt::Key_J);
        QVERIFY(input->text().contains(QLatin1Char('j'), Qt::CaseInsensitive));
        QVERIFY(!QApplication::activePopupWidget());
      }
      input->setText(key == Qt::Key_A ? "ret" : "2a");
      auto *apply =
          editor->findChild<QPushButton *>(QStringLiteral("patchApply"));
      QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), OpenTimeoutMs);
      QTest::keyClick(input->window()->windowHandle(), Qt::Key_Return);
      QTRY_VERIFY(!editor);
      if (hex)
        QTRY_VERIFY(view->hasFocus());
    }
    QTRY_VERIFY2_WITH_TIMEOUT(session.hasBytePatches(),
                              qPrintable(session.lastError()), OpenTimeoutMs);
    answer.stop();
    const auto patchAddress = key == Qt::Key_F ? NativeBase : address;
    const auto patchSize = expected.size() / 2;
    QCOMPARE(currentBytes(session, patchAddress, patchSize), expected);
    auto *output = window.findChild<OutputWindow *>();
    QVERIFY(output);
    auto *log = output->findChild<QPlainTextEdit *>();
    QVERIFY(log);
    QTRY_VERIFY(log->toPlainText().contains("Applied patch at " +
                                            hexAddress(patchAddress)));
    QVERIFY(
        log->toPlainText().contains(expected.left(64), Qt::CaseInsensitive));
    if (key == Qt::Key_A) {
      QVERIFY(log->toPlainText().contains("Instructions:"));
      QTRY_VERIFY(disassembly->listing()->searchReady());
      QTRY_VERIFY(disassembly->listing()->currentLineText().contains("ret"));
    }
    if (key == Qt::Key_A || (key == Qt::Key_B && hex)) {
      // Do not move the cursor or pointer after applying. The current row
      // must already show its patch, including an edited operand in Hex.
      auto *viewport =
          hex ? hexView->viewport() : disassembly->listing()->viewport();
      const auto expectedColor = Theme::instance().color(
          hex ? ColorRole::ListingPatchedSelection : ColorRole::ListingPatched);
      const auto tinted = [&] {
        const auto rect = hex ? hexView->cursorViewportRect()
                              : disassembly->listing()->cursorViewportRect();
        const QRect sample =
            hex ? rect
                : QRect(viewport->width() - 24, rect.top(), 16, rect.height());
        const auto pixels = viewport->grab(sample).toImage();
        for (int y = 0; y < pixels.height(); ++y)
          for (int x = 0; x < pixels.width(); ++x)
            if (pixels.pixelColor(x, y) == expectedColor)
              return true;
        return false;
      };
      if (hex)
        QTRY_COMPARE(hexView->currentAddress(),
                     std::optional<Address>(address));
      QTRY_VERIFY(tinted());
      if (const auto capture = qEnvironmentVariable("NEVERD_PATCH_CAPTURE_DIR");
          !capture.isEmpty())
        QVERIFY(window.grab().save(capture + "/applied-selected-" +
                                   QTest::currentDataTag() + ".png"));
    }
    QCOMPARE(confirmations,
             (key == Qt::Key_C || key == Qt::Key_K || key == Qt::Key_F) ? 1
                                                                        : 0);
    QVERIFY(!QApplication::activePopupWidget());
    QVERIFY(activate(*view->window()));
    view->setFocus();
    QTRY_VERIFY(window.actions().action(ActionId::EditUndo)->isEnabled());
    QTest::keyClick(view->window()->windowHandle(), Qt::Key_Z,
                    Qt::ControlModifier);
    QTRY_VERIFY(!session.hasBytePatches());
    QTRY_VERIFY(log->toPlainText().contains("Undid edit at "));
    QCOMPARE(currentBytes(session, patchAddress, patchSize),
             QString::fromLatin1(
                 original.mid(120 + (key == Qt::Key_F ? 0 : offset), patchSize)
                     .toHex()));
    QTRY_VERIFY(window.actions().action(ActionId::EditRedo)->isEnabled());
    QTest::keyClick(view->window()->windowHandle(), Qt::Key_Z,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(session.hasBytePatches());
    QTRY_VERIFY(log->toPlainText().contains("Redid edit at "));
    QCOMPARE(currentBytes(session, patchAddress, patchSize), expected);
    if (key == Qt::Key_C) {
      // Exercise the remaining J windows while an exportable patch exists.
      QVERIFY(activate(*view->window()));
      view->setFocus();
      QTest::keyClick(view->window()->windowHandle(), Qt::Key_J);
      popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      QVERIFY(popup);
      QTRY_VERIFY(menuHasKey(popup, Qt::Key_P));
      QTest::keyClick(popup->windowHandle(), Qt::Key_P);
      ChooserView *patches = nullptr;
      for (auto *chooser : window.findChildren<ChooserView *>())
        if (chooser->model().kind() == ChooserKind::PatchedBytes)
          patches = chooser;
      QVERIFY(patches);
      QTRY_VERIFY(patches->model().total() > 0 && !patches->model().loading());
      QTRY_VERIFY(sizes.settled(patches));
      bool exportedDialog = false;
      QTimer cancelExport;
      connect(&cancelExport, &QTimer::timeout, &window, [&] {
        if (auto *dialog =
                qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            dialog && sizes.settled(dialog)) {
          exportedDialog = true;
          dialog->reject();
        }
      });
      cancelExport.start(5);
      guard.start(OpenTimeoutMs);
      window.actions().action(ActionId::ViewDisassembly)->trigger();
      QVERIFY(activate(*view->window()));
      view->setFocus();
      QTest::keyClick(view->window()->windowHandle(), Qt::Key_J);
      popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      QVERIFY(popup);
      QTRY_VERIFY(menuHasKey(popup, Qt::Key_E));
      QTest::keyClick(popup->windowHandle(), Qt::Key_E);
      QVERIFY(exportedDialog);
      cancelExport.stop();
      guard.stop();
      QVERIFY(!QFile::exists(path + ".patched"));
    }
    // Revert must also be offered when only an operand byte changed, leaving
    // the opcode at the cursor identical to the original file.
    QVERIFY(activate(*view->window()));
    view->setFocus();
    QTest::keyClick(view->window()->windowHandle(), Qt::Key_J);
    popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    QVERIFY(popup);
    QTRY_VERIFY(menuHasKey(popup, Qt::Key_R));
    QTest::keyClick(popup->windowHandle(), Qt::Key_R);
    QTRY_VERIFY(!session.hasBytePatches());
    QTRY_VERIFY(log->toPlainText().contains("Restored original bytes at "));
    if (key == Qt::Key_A) {
      const int appliedEntries = log->toPlainText().count("Applied patch at ");
      bool unchanged = false;
      session.editBytePatch(
          "patch_apply",
          {{"address", hexAddress(address)},
           {"kind", "bytes"},
           {"bytes", QString::fromLatin1(
                         original.mid(120 + offset, patchSize).toHex())}},
          session.epoch(), [&] { unchanged = true; });
      QTRY_VERIFY(unchanged);
      QVERIFY(log->toPlainText().contains("made no changes"));
      bool refused = false;
      session.editBytePatch(
          "patch_apply",
          {{"address", hexAddress(address)},
           {"kind", "bytes"},
           {"bytes", "90"},
           {"expected", "00"}},
          session.epoch(), {},
          [&](const QString &, const QString &) { refused = true; });
      QTRY_VERIFY(refused);
      QVERIFY(!session.hasBytePatches());
      QCOMPARE(log->toPlainText().count("Applied patch at "), appliedEntries);
    }
    QCOMPARE(currentBytes(session, patchAddress, patchSize),
             QString::fromLatin1(
                 original.mid(120 + (key == Qt::Key_F ? 0 : offset), patchSize)
                     .toHex()));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
    if (key == Qt::Key_C) {
      const auto capture = qEnvironmentVariable("NEVERD_PATCH_CAPTURE_DIR");
      if (!capture.isEmpty()) {
        JumpDialog jump(session, &window);
        jump.show();
        QVERIFY(activate(jump));
        QTRY_VERIFY(sizes.settled(&jump));
        QVERIFY(jump.grab().save(capture + "/jump-reference.png"));
        auto *close = jump.findChild<QAbstractButton *>("closeButton");
        QVERIFY(close && close->isVisible());
        QTest::mouseClick(close, Qt::LeftButton);
        QTRY_VERIFY(!jump.isVisible());
      }
      QVERIFY(activate(*view->window()));
      view->setFocus();
      QTRY_VERIFY(window.actions()
                      .action(ActionId::EditPatchRewriteFunction)
                      ->isEnabled());
      QTest::keyClick(view->window()->windowHandle(), Qt::Key_J);
      popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      QVERIFY(popup);
      QTRY_VERIFY(menuHasKey(popup, Qt::Key_W));
      QTest::keyClick(popup->windowHandle(), Qt::Key_W);
      QPointer<RewriteFunctionDialog> rewrite;
      QTRY_VERIFY((rewrite = window.findChild<RewriteFunctionDialog *>()));
      QVERIFY(activate(*rewrite));
      auto *buttons = rewrite->findChild<QDialogButtonBox *>();
      QVERIFY(buttons);
      QPushButton *compile = nullptr;
      for (auto *button : buttons->findChildren<QPushButton *>())
        if (button->isDefault())
          compile = button;
      QVERIFY(compile);
      QTRY_VERIFY_WITH_TIMEOUT(compile->isEnabled(), OpenTimeoutMs);
      QTRY_VERIFY(sizes.settled(rewrite));
      if (!capture.isEmpty())
        QVERIFY(rewrite->grab().save(capture + "/rewrite-function.png"));
      auto *close = rewrite->findChild<QAbstractButton *>("closeButton");
      QVERIFY(close && close->isVisible());
      QTest::mouseClick(close, Qt::LeftButton);
      QTRY_VERIFY(!rewrite || !rewrite->isVisible());
      QVERIFY(!session.hasBytePatches());
    }
    for (const auto &painted : sizes.Windows)
      for (const auto &size : painted.Sizes)
        QVERIFY2(size == painted.Sizes.front(), qPrintable(painted.Title));
  }
};

QTEST_MAIN(PatchTests)
#include "PatchTests.moc"
