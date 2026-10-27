//===- WorkbenchTests.cpp - Workbench controller regressions --------------===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the production session, window and views with a fixture worker.
//===----------------------------------------------------------------------===//

#include "../Config.h"
#include "AddressSpace.h"
#include "ChooserView.h"
#include "CodeView.h"
#include "DeclareTypesDialog.h"
#include "DeepLink.h"
#include "DisassemblyView.h"
#include "Docking.h"
#include "DwarfPromptDialog.h"
#include "ExportDataDialog.h"
#include "ExtensionsView.h"
#include "FindInViewDialog.h"
#include "FunctionEditDialog.h"
#include "GraphView.h"
#include "HexView.h"
#include "InstanceServer.h"
#include "InteractionMetrics.h"
#include "JumpDialog.h"
#include "LinkDispatch.h"
#include "LinkOpener.h"
#include "ListingView.h"
#include "ListingViewTestAccess.h"
#include "LoadFileDialog.h"
#include "LocalTypesView.h"
#include "MainWindow.h"
#include "MapPromptDialog.h"
#include "NativeWindowStates.h"
#include "NavigationBand.h"
#include "OutputWindow.h"
#include "PatchEditor.h"
#include "PathfinderView.h"
#include "PdbPromptDialog.h"
#include "ProjectDatabase.h"
#include "QuickStartDialog.h"
#include "ResourceLimits.h"
#include "Session.h"
#include "SessionTestAccess.h"
#include "SetTypeDialog.h"
#include "SettingsKeys.h"
#include "SymbolTablePromptDialog.h"
#include "Theme.h"
#include "TitleBar.h"
#include "TypeLibrariesView.h"
#include "TypePickerDialog.h"
#include "WindowFrame.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QHelpEvent>
#include <QImage>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QTranslator>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QWindow>
#include <cmath>
#include <functional>
#include <kddockwidgets/LayoutSaver.h>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <kddockwidgets/qtwidgets/views/TabBar.h>
#include <kddockwidgets/qtwidgets/views/TitleBar.h>
#include <memory>

using namespace neverd::gui;

namespace neverd::gui {
/// Observe sampling and run one production measurement without exiting the
/// test process or spending ten seconds in the independent scroll phases.
struct InteractionMetricsTestAccess {
  static bool started(const InteractionMetrics &metrics) {
    return metrics.started_;
  }
  static int functionCount(const InteractionMetrics &metrics) {
    return metrics.functionCount_;
  }
  static qsizetype samples(const InteractionMetrics &metrics) {
    return metrics.samples_.size();
  }
  static void pseudocode(InteractionMetrics &metrics, Address address,
                         std::function<void()> done) {
    metrics.started_ = true;
    metrics.samples_ = {address};
    metrics.measurePseudocode("pseudocode", std::move(done));
  }
  static void openPseudocode(InteractionMetrics &metrics, Address address,
                             std::function<void()> done) {
    metrics.started_ = true;
    metrics.samples_ = {address};
    metrics.pseudocodeStep("pseudocode")(std::move(done));
  }
  static QJsonObject pseudocodeResult(const InteractionMetrics &metrics) {
    return metrics.phases_.value("pseudocode").toObject();
  }
};
} // namespace neverd::gui

namespace {
constexpr Address Base = 0xffff800012340000ULL;
constexpr int OpenTimeoutMs = 7000;
constexpr int NativeOpenTimeoutMs = 30000;
// The widest a view may insist on being: a dock's tab bar keeps the dock some
// 130 pixels wide whatever it shows.
constexpr int NarrowViewWidth = 160;

/// Long enough for the offer of a missing PDB to follow an open.
constexpr int OfferWaitMs = 500;

QuickStartDialog *visibleQuickStart() {
  for (auto *window : QApplication::topLevelWidgets())
    if (auto *dialog = qobject_cast<QuickStartDialog *>(window);
        dialog && dialog->isVisible())
      return dialog;
  return nullptr;
}

QString writeFixture(const QTemporaryDir &directory, const QString &name) {
  const auto path = directory.filePath(name);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write("fixture") != 7)
    return {};
  return path;
}

struct ListingPaintObserver : QObject {
  std::function<void()> Observe;
  bool eventFilter(QObject *, QEvent *event) override {
    if (event->type() == QEvent::Paint)
      Observe();
    return false;
  }
};

/// The production window over the fixture worker.
struct Workbench {
  Session session{QString::fromLocal8Bit(TEST_WORKER)};
  McpConnectionManager mcp;
  GuiSessionBroker broker;
  QTemporaryDir layout;
  std::unique_ptr<MainWindow> window;
  mutable QPointer<ChooserView> functionsView;

  /// Unless \p shown, the window stays as a startup without a file leaves
  /// it: hidden, its desktop not laid out yet.
  explicit Workbench(QString worker = QString::fromLocal8Bit(TEST_WORKER),
                     bool shown = true)
      : session(std::move(worker)) {
    window = std::make_unique<MainWindow>(session, mcp, broker);
    window->setLayoutPath(layout.filePath(QStringLiteral("layout.json")));
    window->resize(1400, 900);
    if (!shown)
      return;
    window->initializeLayout();
    window->show();
  }
  ChooserView *functions() const {
    if (!functionsView)
      if (auto *table =
              window->findChild<QTreeView *>(QStringLiteral("functionsList")))
        functionsView = qobject_cast<ChooserView *>(table->parentWidget());
    return functionsView;
  }
  QAction *action(ActionId id) const { return window->actions().action(id); }
  QString dockTitle(const QString &uniqueName) const {
    for (auto *dock :
         window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->uniqueName() == uniqueName)
        return dock->title();
    return {};
  }
  KDDockWidgets::QtWidgets::DockWidget *dockFor(QWidget *view) const {
    // Floating panes have their own top-level QObject tree.
    for (auto *parent = view; parent; parent = parent->parentWidget())
      if (auto *dock =
              qobject_cast<KDDockWidgets::QtWidgets::DockWidget *>(parent))
        return dock;
    return nullptr;
  }
  CodeView *codeView(const QString &representation) const {
    for (auto *view : window->findChildren<CodeView *>())
      if (view->representation() == representation)
        return view;
    return nullptr;
  }
};

QDialog *visibleDialog(const QString &name = {}) {
  for (auto *widget : QApplication::topLevelWidgets())
    if (auto *dialog = qobject_cast<QDialog *>(widget);
        dialog && dialog->isVisible() &&
        (name.isEmpty() || dialog->objectName() == name))
      return dialog;
  return nullptr;
}

bool privateWayland() {
  return QGuiApplication::platformName().startsWith(QLatin1String("wayland")) &&
         qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER");
}

// Only the disposable compositor runner grants this capability. Never ask the
// user's ordinary desktop to activate windows or create input devices.
bool privateShell(const QString &script) {
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

bool activateTestWindow(QWidget *window) {
  // Qt can report a transient family as active before native keyboard focus
  // reaches this particular window. A dialog's pending activation can also
  // supersede a single request. Keep requesting within the ordinary test
  // deadline, and require both observations before sending keyboard input.
  QString nativeActivation;
  if (privateWayland()) {
    const auto title = QJsonDocument(QJsonArray{window->windowTitle()})
                           .toJson(QJsonDocument::Compact);
    nativeActivation =
        QStringLiteral(
            "Main.overview.hide(); const w = global.get_window_actors()"
            ".map(a => a.meta_window).find(w => w.get_pid() === %1 && "
            "w.get_title() === %2); if (w) Main.activateWindow(w);")
            .arg(QCoreApplication::applicationPid())
            .arg(QString::fromUtf8(title.mid(1, title.size() - 2)));
  }
  const bool active = QTest::qWaitFor([window, &nativeActivation] {
    if (window->isActiveWindow() &&
        QGuiApplication::focusWindow() == window->windowHandle())
      return true;
    if (!nativeActivation.isEmpty())
      privateShell(nativeActivation);
    else {
      window->raise();
      window->activateWindow();
    }
    return false;
  });
  if (!active)
    qWarning() << "Native focus did not reach" << window << "visible"
               << window->isVisible() << "exposed"
               << (window->windowHandle() &&
                   window->windowHandle()->isExposed())
               << "focus" << QGuiApplication::focusWindow() << "active"
               << QApplication::activeWindow() << "popup"
               << QApplication::activePopupWidget() << "modal"
               << QApplication::activeModalWidget();
  return active;
}

/// Bound a regression to exec() so a modal reference list fails rather than
/// hanging the native interaction tests.
QPointer<QDialog> openLocalReferences(Workbench &bench, CodeView *view) {
  QTimer guard;
  guard.setSingleShot(true);
  QObject::connect(&guard, &QTimer::timeout, &guard, [] {
    if (auto *dialog =
            qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      dialog->reject();
  });
  guard.start(OpenTimeoutMs);
  bench.action(ActionId::JumpXrefsTo)->trigger();
  for (auto *dialog :
       view->findChildren<QDialog *>(QStringLiteral("localReferencesDialog")))
    if (dialog->isVisible())
      return dialog;
  return nullptr;
}

/// The representations a code view's menu offers, in order.
QStringList representationsOf(CodeView *view) {
  QStringList names;
  if (auto *selector = view->findChild<QComboBox *>())
    for (int i = 0; i < selector->count(); ++i)
      names << selector->itemData(i).toString();
  return names;
}

QByteArray readAll(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// A file dialog the user cancels.
QString noFile(QWidget *) { return {}; }

QPushButton *buttonNamed(QWidget *window, const QString &text) {
  for (auto *button : window->findChildren<QPushButton *>())
    if (button->text() == text)
      return button;
  return nullptr;
}
/// Accepts every "Load a new file" dialog with its defaults while it lives,
/// as pressing Enter does.
/// Shows \p code's context menu at its cursor and hands it to \p use, which
/// may trigger one of its entries; the menu then closes.
void withCodeMenu(CodeText *code, const std::function<void(QMenu *)> &use) {
  const QPointer<QWidget> window = code->window();
  QTimer probe;
  QObject::connect(&probe, &QTimer::timeout, &probe, [&] {
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu)
      return;
    probe.stop();
    use(menu);
    menu->close();
  });
  probe.start(5);
  emit code->contextMenuRequested(
      code->viewport()->mapToGlobal(QPoint(40, 40)));
  // A programmatically closed popup has no native click to reactivate its
  // owner on the offscreen platform. Continue in the same active workbench.
  if (window && window->isVisible() &&
      (!QApplication::activeWindow() ||
       QApplication::activeWindow() == window)) {
    window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(window));
  }
}

/// As withCodeMenu, for a right click on \p assembly's text or graph.
void withAssemblyMenu(DisassemblyView *assembly,
                      const std::function<void(QMenu *)> &use) {
  const QPointer<QWidget> window = assembly->window();
  QTimer probe;
  QObject::connect(&probe, &QTimer::timeout, &probe, [&] {
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu)
      return;
    probe.stop();
    use(menu);
    menu->close();
  });
  probe.start(5);
  QWidget *surface = assembly->graphMode()
                         ? static_cast<QWidget *>(assembly->graph())
                         : assembly->listing()->viewport();
  const QPoint point(40, 40);
  QContextMenuEvent context(QContextMenuEvent::Mouse, point,
                            surface->mapToGlobal(point));
  QApplication::sendEvent(surface, &context);
  if (window && window->isVisible() &&
      (!QApplication::activeWindow() ||
       QApplication::activeWindow() == window)) {
    window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(window));
  }
}

class LoadDialogAcceptor {
public:
  LoadDialogAcceptor() {
    QObject::connect(&timer_, &QTimer::timeout, [this] {
      if (auto *dialog = qobject_cast<LoadFileDialog *>(
              QApplication::activeModalWidget())) {
        ++accepted_;
        dialog->accept();
      }
    });
    timer_.start(10);
  }
  int accepted() const { return accepted_; }

private:
  QTimer timer_;
  int accepted_ = 0;
};

bool dropFile(QWidget *target, const QString &path) {
  QMimeData mime;
  mime.setUrls({QUrl::fromLocalFile(path)});
  QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction | Qt::MoveAction, &mime,
                        Qt::LeftButton, Qt::ShiftModifier);
  QApplication::sendEvent(target, &enter);
  if (!enter.isAccepted() || enter.dropAction() != Qt::CopyAction)
    return false;
  QDragMoveEvent move(QPoint(10, 10), Qt::CopyAction | Qt::MoveAction, &mime,
                      Qt::LeftButton, Qt::ShiftModifier);
  QApplication::sendEvent(target, &move);
  if (!move.isAccepted() || move.dropAction() != Qt::CopyAction)
    return false;
  QDropEvent drop(QPointF(10, 10), Qt::CopyAction | Qt::MoveAction, &mime,
                  Qt::LeftButton, Qt::ShiftModifier);
  QApplication::sendEvent(target, &drop);
  return drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
}

void dragText(QWidget *viewport, const QPoint &begin, const QPoint &end) {
  QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, begin);
  QMouseEvent move(QEvent::MouseMove, end, viewport->mapToGlobal(end),
                   Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(viewport, &move);
  QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, end);
}

// Windows delivers clipboard notifications asynchronously. Pump them between
// synthetic user actions, including reads that request delayed rendering.
QString clipboardText() {
  const QString text = QApplication::clipboard()->text();
  if (QGuiApplication::platformName() == QLatin1String("windows"))
    QTest::qWait(20);
  return text;
}

void clearClipboard() {
  QApplication::clipboard()->clear();
  if (QGuiApplication::platformName() == QLatin1String("windows"))
    QTest::qWait(20);
}
} // namespace

class WorkbenchTests : public QObject {
  Q_OBJECT
  QTemporaryDir settingsDirectory_;

public:
  static void initMain() { WindowFrame::prepareNativeWindows(); }

private slots:
  void floatingDocksReturnToWorkbench_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
  }

  void floatingDocksReturnToWorkbench() {
    QFETCH(bool, dark);
    auto &theme = Theme::instance();
    const auto mode = theme.mode();
    const auto restore = qScopeGuard([&] { theme.setMode(mode); });
    theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    theme.apply();
    Workbench bench;
    auto *assembly = bench.window->disassembly();
    auto *dock = bench.dockFor(assembly);
    QVERIFY(dock);
    for (int method = 0; method < 3; ++method) {
      dock->setFloating(true);
      QTRY_VERIFY(dock->isFloating());
      auto *floating = dock->QWidget::window();
      QVERIFY(floating != bench.window.get());
      floating->activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(floating));
      KDDockWidgets::QtWidgets::TitleBar *bar = nullptr;
      for (auto *candidate :
           floating->findChildren<KDDockWidgets::QtWidgets::TitleBar *>())
        if (candidate->isVisible())
          bar = candidate;
      QVERIFY(bar);
      const QString captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
      if (!captures.isEmpty()) {
        QDir().mkpath(captures);
        QVERIFY(floating->grab().save(
            captures + (dark ? "/floating-dark.png" : "/floating-light.png")));
      }
      auto *lock = floating->findChild<QAbstractButton *>(
          QStringLiteral("floatingLockButton"));
      QVERIFY(lock && lock->isChecked());
      if (method == 0) {
        // Direct Dock actions below deliberately leave the default lock on.
        QTest::mouseClick(lock, Qt::LeftButton);
        QVERIFY(!lock->isChecked());
        QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier,
                           bar->rect().center());
      } else if (method == 1) {
        QAbstractButton *button = nullptr;
        for (auto *candidate : bar->findChildren<QAbstractButton *>())
          if (candidate->isVisible() &&
              candidate->toolTip() == QStringLiteral("Dock window"))
            button = candidate;
        QVERIFY(button);
        QTest::mouseClick(button, Qt::LeftButton);
      } else {
        auto *tabs = floating->findChild<KDDockWidgets::QtWidgets::TabBar *>();
        QVERIFY(tabs);
        bool offered = false;
        QTimer select;
        connect(&select, &QTimer::timeout, this, [&] {
          auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
          if (!menu)
            return;
          select.stop();
          for (auto *action : menu->actions())
            if (action->objectName() == QStringLiteral("dockBack")) {
              offered = true;
              QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier,
                                menu->actionGeometry(action).center());
              return;
            }
          menu->close();
        });
        select.start(10);
        const QPoint point = tabs->tabRect(tabs->currentIndex()).center();
        QContextMenuEvent context(QContextMenuEvent::Mouse, point,
                                  tabs->mapToGlobal(point));
        QApplication::sendEvent(tabs, &context);
        QVERIFY(offered);
      }
      QTRY_VERIFY(!dock->isFloating());
      QCOMPARE(assembly->window(), bench.window.get());
      QVERIFY(assembly->isVisible());
    }
  }

  void closingWorkbenchClosesDetachedWindows_data() {
    QTest::addColumn<bool>("dirty");
    QTest::newRow("clean") << false;
    QTest::newRow("cancel-then-discard") << true;
  }

  void codeOpensBesideFloatingDisassembly_data() {
    QTest::addColumn<int>("command");
    QTest::addColumn<bool>("alreadyOpen");
    for (const auto [name, command] :
         {std::pair{"source", ActionId::ViewPseudocode},
          {"ir", ActionId::ViewMedIR}})
      for (bool open : {false, true})
        QTest::newRow(
            qPrintable(QString::fromLatin1(name) + (open ? "-open" : "-new")))
            << int(command) << open;
  }

  void codeOpensBesideFloatingDisassembly() {
    QFETCH(int, command);
    QFETCH(bool, alreadyOpen);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "floating-code.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    QTRY_VERIFY(ListingViewTestAccess::settled(*assembly->listing()));
    auto *action = bench.action(ActionId(command));
    if (alreadyOpen)
      action->trigger();
    auto *assemblyDock = bench.dockFor(assembly);
    QVERIFY(assemblyDock);
    assemblyDock->setFloating(true);
    auto *floating = assembly->window();
    floating->resize(1100, 700);
    QVERIFY(activateTestWindow(floating));
    assembly->focusContent();
    action->trigger();
    QTRY_VERIFY(floating->findChild<CodeView *>());
    auto *code = floating->findChild<CodeView *>();
    QTRY_VERIFY(code->isVisible());
    QCOMPARE(code->window(), assembly->window());
    QVERIFY(code->mapTo(floating, QPoint()).x() >=
            assembly->mapTo(floating, QPoint()).x() + assembly->width());
    QTRY_COMPARE(code->text()->function(), assembly->currentFunction());
    KDDockWidgets::QtWidgets::DockWidget *codeDock = nullptr;
    for (auto *dock :
         floating->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->widget() == code)
        codeDock = dock;
    QVERIFY(codeDock);
    codeDock->forceClose();
    QVERIFY(!code->isVisible());
    assembly->focusContent();
    action->trigger();
    QTRY_VERIFY(code->isVisible());
    QCOMPARE(code->window(), floating);
  }

  void focusModeGroupsViewsAndRestoresLayout_data() {
    QTest::addColumn<bool>("floating");
    QTest::addColumn<bool>("dark");
    QTest::addColumn<bool>("inFunction");
    for (bool floating : {false, true})
      for (bool dark : {false, true})
        QTest::newRow(qPrintable(QString(floating ? "floating" : "docked") +
                                 (dark ? "-dark" : "-light")))
            << floating << dark << true;
    QTest::newRow("data-address") << false << false << false;
  }

  void focusModeGroupsViewsAndRestoresLayout() {
    QFETCH(bool, floating);
    QFETCH(bool, dark);
    QFETCH(bool, inFunction);
    auto &theme = Theme::instance();
    const auto previous = theme.mode();
    const auto restoreTheme = qScopeGuard([&] { theme.setMode(previous); });
    theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    theme.apply();
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "focus-mode.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address address = Base + (inFunction ? 0x140 : 0x3000);
    assembly->navigate(address);
    QTRY_COMPARE(assembly->currentItem(), std::optional<Address>(address));
    QTRY_COMPARE(assembly->currentFunction(),
                 inFunction ? std::optional<Address>(address) : std::nullopt);
    QTRY_VERIFY(ListingViewTestAccess::settled(*assembly->listing()));
    auto *output = bench.window->findChild<OutputWindow *>();
    auto *assemblyDock = bench.dockFor(assembly);
    auto *outputDock = bench.dockFor(output);
    QVERIFY(assemblyDock && outputDock);
    if (floating)
      assemblyDock->setFloating(true);
    QVERIFY(activateTestWindow(assembly->window()));
    const bool wasMaximized = assembly->window()->isMaximized();
    assembly->focusContent();
    auto *mode = bench.action(ActionId::ViewFocusMode);
    QVERIFY(mode->isEnabled());
    QSignalSpy hex(bench.action(ActionId::EditOperandHex), &QAction::triggered);
    const bool dirty = bench.session.dirty();
    QTest::keyClick(assembly->window()->windowHandle(), Qt::Key_F);
    QTest::keyClick(assembly->window()->windowHandle(), Qt::Key_F);
    QVERIFY(!mode->isChecked());
    QTest::keyClick(assembly->window()->windowHandle(), Qt::Key_F);
    QTRY_VERIFY(mode->isChecked());
    auto *workspace = assembly->window();
    QVERIFY(workspace != bench.window.get());
    QTRY_VERIFY(workspace->isMaximized());
    auto *code = workspace->findChild<CodeView *>();
    QVERIFY(code);
    QCOMPARE(output->window(), workspace);
    QTRY_VERIFY(code->isVisible() && output->isVisible());
    QTRY_COMPARE(code->text()->function(), assembly->currentFunction());
    QTRY_VERIFY(!code->text()->loading());
    QVERIFY(code->mapTo(workspace, QPoint()).x() >=
            assembly->mapTo(workspace, QPoint()).x() + assembly->width());
    QVERIFY(output->mapTo(workspace, QPoint()).y() >=
            assembly->mapTo(workspace, QPoint()).y() + assembly->height());
    QVERIFY(output->width() >= assembly->width() + code->width());
    QCOMPARE(hex.size(), 0);
    QCOMPARE(bench.session.dirty(), dirty);
    auto *input = output->findChild<QLineEdit *>();
    QVERIFY(input);
    QVERIFY(activateTestWindow(workspace));
    input->setFocus();
    for (int i = 0; i < 3; ++i)
      QTest::keyClick(input->window()->windowHandle(), Qt::Key_F);
    QCOMPARE(input->text(), QStringLiteral("fff"));
    QVERIFY(mode->isChecked());
    input->clear();
    QTest::keyClicks(input, "help");
    QTest::keyClick(input, Qt::Key_Return);
    QVERIFY(
        output->findChild<QPlainTextEdit *>()->toPlainText().contains("help"));
    const QString captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
    if (!captures.isEmpty()) {
      QDir().mkpath(captures);
      QVERIFY(workspace->grab().save(captures + "/focus-" +
                                     QTest::currentDataTag() + ".png"));
    }
    code->text()->setFocus();
    for (int i = 0; i < 3; ++i)
      QTest::keyClick(code->window()->windowHandle(), Qt::Key_F);
    QTRY_VERIFY(!mode->isChecked());
    QCOMPARE(assembly->window() != bench.window.get(), floating);
    QTRY_COMPARE(assembly->window()->isMaximized(), wasMaximized);
    QCOMPARE(output->window(), bench.window.get());
    QVERIFY(output->isVisible() && assembly->isVisible());
    QVERIFY(!code->isVisible());
    QCOMPARE(hex.size(), 0);
    // Both the drawn close button and a desktop close request leave focus
    // mode through the same layout restoration as the F sequence.
    bool nativePointerUnavailable = false;
    for (bool button : {true, false}) {
      mode->trigger();
      workspace = assembly->window();
      QVERIFY(activateTestWindow(workspace));
      QVERIFY(mode->isChecked());
      if (button) {
        auto *close = workspace->findChild<QAbstractButton *>(
            QStringLiteral("floatingCloseButton"));
        QVERIFY(close && close->isVisible());
        QTest::mouseClick(workspace->windowHandle(), Qt::LeftButton,
                          Qt::NoModifier,
                          close->mapTo(workspace, close->rect().center()));
      } else {
        workspace->close();
      }
      QTRY_VERIFY(!mode->isChecked());
      QCOMPARE(assembly->window() != bench.window.get(), floating);
      QTRY_COMPARE(assembly->window()->isMaximized(), wasMaximized);
      QCOMPARE(output->window(), bench.window.get());
      QVERIFY(output->isVisible() && assembly->isVisible());
      QVERIFY(!code->isVisible());
      QVERIFY(output->findChild<QPlainTextEdit *>()->toPlainText().contains(
          "help"));
      if (button && !floating) {
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() == QLatin1String("cocoa") &&
            !test::canPostNativePointerEvents()) {
          nativePointerUnavailable = true;
          continue;
        }
#endif
        auto *bar =
            bench.window->findChild<TitleBar *>(Qt::FindDirectChildrenOnly);
        QVERIFY(bar);
        auto *close = bar->findChild<QAbstractButton *>("closeButton");
        QVERIFY(close && close->isVisible());
        const auto background = [&] {
          const QImage painted = bar->grab().toImage();
          return painted.pixelColor(close->mapTo(bar, QPoint(1, 1)) *
                                    painted.devicePixelRatio());
        };
        QVERIFY(activateTestWindow(bench.window.get()));
        const auto movePointer = [&](QPoint position) {
#ifdef Q_OS_MACOS
          // After closing the focus window, Cocoa needs a native enter/move
          // for the restored window. A QWindow test event does not move the
          // system pointer or establish that native tracking transition.
          if (QGuiApplication::platformName() == QLatin1String("cocoa")) {
            return test::moveNativePointer(bench.window->mapToGlobal(position));
          }
#endif
          QTest::mouseMove(bench.window->windowHandle(), position);
          return true;
        };
        QVERIFY(movePointer(bench.window->rect().center()));
        QTRY_VERIFY(!close->underMouse());
        QVERIFY(movePointer(
            close->mapTo(bench.window.get(), close->rect().center())));
        QTRY_VERIFY(close->underMouse());
        QCOMPARE(background(),
                 theme.chrome(QStringLiteral("WindowCloseHover")));
        QVERIFY(movePointer(bench.window->rect().center()));
        QTRY_VERIFY(!close->underMouse() && !close->isDown());
        QCOMPARE(background(), theme.chrome(QStringLiteral("TitleBar")));
      }
    }
    if (nativePointerUnavailable)
      QSKIP("Layout and close paths passed; native hover requires macOS input "
            "permission");
  }

  void fileReviewWindowsOfferWindowControls_data() {
    QTest::addColumn<int>("command");
    QTest::addColumn<QString>("name");
    QTest::newRow("offline-package") << int(ActionId::FilePackageReview)
                                     << QStringLiteral("offlinePackageReview");
    QTest::newRow("binary-comparison")
        << int(ActionId::FileCompare) << QStringLiteral("binaryComparison");
  }

  void focusToolbarKeepsWorkbenchCommands_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
  }

  void focusToolbarKeepsWorkbenchCommands() {
    QFETCH(bool, dark);
    auto &theme = Theme::instance();
    const auto previous = theme.mode();
    const auto restoreTheme = qScopeGuard([&] { theme.setMode(previous); });
    theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    theme.apply();
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "focus-toolbar.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_VERIFY(ListingViewTestAccess::settled(*assembly->listing()));
    auto *mode = bench.action(ActionId::ViewFocusMode);
    mode->trigger();
    auto *workspace = assembly->window();
    QVERIFY(activateTestWindow(workspace));
    QPointer<QToolBar> toolbar =
        workspace->findChild<QToolBar *>(QStringLiteral("focusToolbar"));
    QVERIFY(toolbar && toolbar->isVisible());
    QCOMPARE(toolbar->toolButtonStyle(), Qt::ToolButtonIconOnly);
    for (auto *normal : bench.window->findChildren<QToolBar *>(
             QString(), Qt::FindDirectChildrenOnly)) {
      if (normal->objectName() == QLatin1String("toolbarNavigationBand"))
        continue;
      for (auto *action : normal->actions())
        if (!action->isSeparator())
          QVERIFY2(toolbar->actions().contains(action),
                   qPrintable(action->objectName()));
    }
    auto *code = workspace->findChild<CodeView *>();
    QVERIFY(code);
    const auto focusState = [&] {
      QString state;
      QDebug(&state) << "focused" << QApplication::focusWidget() << "active"
                     << QApplication::activeWindow() << "native focus"
                     << QGuiApplication::focusWindow() << "workspace"
                     << workspace << "native workspace"
                     << workspace->windowHandle() << "code" << code->text()
                     << "visible" << code->text()->isVisible() << "enabled"
                     << code->text()->isEnabled() << "loading"
                     << code->text()->loading();
      return state;
    };
    QTRY_COMPARE(code->text()->function(),
                 std::optional<Address>(Base + 0x140));
    QTRY_VERIFY(!code->text()->loading());
    assembly->navigate(Base + 0x1e0);
    QTRY_VERIFY(ListingViewTestAccess::settled(*assembly->listing()));
    code->text()->setFocus();
    QTRY_VERIFY2(code->text()->hasFocus(), qPrintable(focusState()));
    const auto click = [&](ActionId id) {
      auto *button = toolbar->widgetForAction(bench.action(id));
      if (!button || !button->isVisible() || !button->isEnabled())
        return false;
      QTest::mouseClick(workspace->windowHandle(), Qt::LeftButton,
                        Qt::NoModifier,
                        button->mapTo(workspace, button->rect().center()));
      return true;
    };
    QVERIFY(click(ActionId::JumpBack));
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    QTRY_VERIFY2(code->text()->hasFocus(), qPrintable(focusState()));
    QVERIFY(click(ActionId::JumpForward));
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x1e0));
    QTRY_COMPARE(code->text()->function(), assembly->currentFunction());
    QTRY_VERIFY(!code->text()->loading());
    const QString captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
    if (!captures.isEmpty()) {
      QDir().mkpath(captures);
      QVERIFY(workspace->grab().save(captures + "/focus-toolbar-" +
                                     QTest::currentDataTag() + ".png"));
    }
    mode->trigger();
    QTRY_VERIFY(!toolbar);
    auto *output = bench.window->findChild<OutputWindow *>();
    QVERIFY(output && output->isVisible());
    mode->trigger();
    workspace = assembly->window();
    QCOMPARE(workspace->findChildren<QToolBar *>(QStringLiteral("focusToolbar"))
                 .size(),
             1);
    workspace->close();
    QTRY_VERIFY(!mode->isChecked());
    QVERIFY(output->isVisible());
  }

  void focusFunctionsSidebarTogglesAndNavigates_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
  }

  void focusFunctionsSidebarTogglesAndNavigates() {
    QFETCH(bool, dark);
    auto &theme = Theme::instance();
    const auto previous = theme.mode();
    const auto restoreTheme = qScopeGuard([&] { theme.setMode(previous); });
    theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    theme.apply();
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "focus-sidebar.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    QTRY_VERIFY(ListingViewTestAccess::settled(*assembly->listing()));
    auto *original = bench.functions();
    QVERIFY(original);
    const int originalNameWidth = original->table()->header()->sectionSize(0);
    original->setFilterText(QStringLiteral("function_1"));
    QTRY_COMPARE(original->model().filter(), QStringLiteral("function_1"));
    assembly->focusContent();
    auto *mode = bench.action(ActionId::ViewFocusMode);
    mode->trigger();
    auto *workspace = assembly->window();
    QVERIFY(activateTestWindow(workspace));
    QPointer<QWidget> sidebar = workspace->findChild<QWidget *>(
        QStringLiteral("focusFunctionsSidebar"));
    QPointer<QToolButton> toggle = workspace->findChild<QToolButton *>(
        QStringLiteral("focusSidebarToggle"));
    QVERIFY(sidebar && toggle && toggle->isVisible());
    QVERIFY(!sidebar->isVisible());
    auto *code = workspace->findChild<CodeView *>();
    QVERIFY(code);
    QTRY_VERIFY(!code->text()->loading());
    const QRect assemblyGeometry(assembly->mapTo(workspace, QPoint()),
                                 assembly->size());
    const QRect codeGeometry(code->mapTo(workspace, QPoint()), code->size());
    const QPoint triangle = toggle->mapTo(workspace, toggle->rect().center());
    const QString captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
    const auto capture = [&](const QString &state) {
      if (captures.isEmpty())
        return true;
      QDir().mkpath(captures);
      return workspace->grab().save(captures + "/focus-sidebar-" + state + "-" +
                                    QTest::currentDataTag() + ".png");
    };
    QVERIFY(capture(QStringLiteral("collapsed")));
    QTest::mouseMove(workspace->windowHandle(), triangle);
    QTest::qWait(350);
    QVERIFY(!sidebar->isVisible()); // Hover must not open the list.
    QTest::mouseClick(workspace->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      triangle);
    QTRY_VERIFY(sidebar->isVisible());
    QVERIFY(toggle->isChecked());
    QCOMPARE(toggle->arrowType(), Qt::LeftArrow);
    QCOMPARE(toggle->mapTo(workspace, toggle->rect().center()), triangle);
    QCOMPARE(QRect(assembly->mapTo(workspace, QPoint()), assembly->size()),
             assemblyGeometry);
    QCOMPARE(QRect(code->mapTo(workspace, QPoint()), code->size()),
             codeGeometry);
    auto *functions = sidebar->findChild<ChooserView *>();
    QVERIFY(functions && functions != original);
    QTRY_VERIFY(functions->model().total() > 0 &&
                !functions->model().loading());
    QVERIFY(capture(QStringLiteral("open")));
    auto *handle = workspace->findChild<QWidget *>(
        QStringLiteral("focusSidebarResizeHandle"));
    QVERIFY(handle && handle->isVisible());
    QCOMPARE(handle->cursor().shape(), Qt::SplitHCursor);
    const int initialWidth = sidebar->width();
    const int nameWidth = functions->table()->header()->sectionSize(0);
    const int keyboardStep =
        ::neverd::limits::gui_config::kFocusSidebarKeyboardResizeStep;
    // Keep the initial growth within the disassembly pane on small CI screens,
    // leaving room for the later keyboard expansion.
    const int grownWidth =
        std::min(initialWidth + 180,
                 assemblyGeometry.right() - sidebar->x() + 1 - keyboardStep);
    QVERIFY(grownWidth > initialWidth);
    const auto resizeBy = [&](int delta) {
      const QPoint start = handle->mapTo(workspace, handle->rect().center());
      const QPoint end(std::clamp(start.x() + delta, 0, workspace->width() - 1),
                       start.y());
      QTest::mousePress(workspace->windowHandle(), Qt::LeftButton,
                        Qt::NoModifier, start);
      QTest::mouseMove(workspace->windowHandle(), end);
      QTest::mouseRelease(workspace->windowHandle(), Qt::LeftButton,
                          Qt::NoModifier, end);
    };
    resizeBy(grownWidth - initialWidth);
    QTRY_COMPARE(sidebar->width(), grownWidth);
    QTRY_COMPARE(functions->table()->header()->sectionSize(0),
                 nameWidth + grownWidth - initialWidth);
    resizeBy(-80);
    QTRY_COMPARE(sidebar->width(), grownWidth - 80);
    resizeBy(-workspace->width());
    QTRY_COMPARE(sidebar->width(),
                 ::neverd::limits::gui_config::kFocusSidebarMinimumWidth);
    resizeBy(workspace->width() * 2);
    QVERIFY(sidebar->geometry().right() <= workspace->rect().right());
    QVERIFY(
        !WindowFrame::edgesAt(handle->mapTo(workspace, handle->rect().center()),
                              workspace->size(), {})
             .testFlag(Qt::RightEdge));
    resizeBy(grownWidth - sidebar->width());
    QTRY_COMPARE(sidebar->width(), grownWidth);
    handle->setFocus(Qt::TabFocusReason);
    QTest::keyClick(handle, Qt::Key_Right);
    QTRY_COMPARE(sidebar->width(), grownWidth + keyboardStep);
    QTest::keyClick(handle, Qt::Key_Left);
    QCOMPARE(sidebar->width(), grownWidth);
    QCOMPARE(toggle->mapTo(workspace, toggle->rect().center()), triangle);
    QCOMPARE(QRect(assembly->mapTo(workspace, QPoint()), assembly->size()),
             assemblyGeometry);
    QCOMPARE(QRect(code->mapTo(workspace, QPoint()), code->size()),
             codeGeometry);
    QVERIFY(capture(QStringLiteral("resized")));
    QTest::mouseMove(workspace->windowHandle(),
                     code->mapTo(workspace, code->rect().center()));
    QTest::qWait(350);
    QVERIFY(sidebar->isVisible()); // Moving away must not fold it either.
    auto *filter = functions->findChild<QLineEdit *>();
    QVERIFY(filter && filter->isVisible());
    filter->setFocus();
    QTest::keyClicks(filter, "function_30");
    QTRY_COMPARE(functions->model().filter(), QStringLiteral("function_30"));
    constexpr Address Target = Base + 0x1e0;
    QTRY_COMPARE(functions->model().addressAt(0),
                 std::optional<Address>(Target));
    auto *table = functions->table();
    const QPoint row =
        table->visualRect(functions->model().index(0, 0)).center();
    QTest::mouseClick(workspace->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      table->viewport()->mapTo(workspace, row));
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(Target));
    QTRY_COMPARE(code->text()->function(), std::optional<Address>(Target));
    QVERIFY(sidebar->isVisible());
    bool menuOwned = false;
    QTimer closeMenu;
    closeMenu.setSingleShot(true);
    connect(&closeMenu, &QTimer::timeout, this, [&] {
      if (auto *menu =
              qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
        menuOwned = menu->parentWidget() == workspace;
        menu->close();
      }
    });
    closeMenu.start(0);
    emit table->customContextMenuRequested(row);
    closeMenu.stop();
    QVERIFY(menuOwned);
    QVERIFY(sidebar->isVisible());
    QVERIFY(capture(QStringLiteral("filtered")));
    QTest::mouseClick(workspace->windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      triangle);
    QTRY_VERIFY(!sidebar->isVisible());
    QVERIFY(!toggle->isChecked());
    QCOMPARE(toggle->arrowType(), Qt::RightArrow);
    bench.action(ActionId::ViewFunctions)->trigger();
    QVERIFY(sidebar->isVisible());
    QCOMPARE(sidebar->width(), grownWidth);
    QCOMPARE(filter->text(), QStringLiteral("function_30"));
    filter->clear();
    filter->setFocus();
    QTest::keyClicks(filter, "fff");
    QCOMPARE(filter->text(), QStringLiteral("fff"));
    QVERIFY(mode->isChecked());
    mode->trigger();
    QVERIFY(!sidebar && !toggle);
    QCOMPARE(original->model().filter(), QStringLiteral("function_1"));
    QCOMPARE(original->table()->header()->sectionSize(0), originalNameWidth);
    QCOMPARE(original->window(), bench.window.get());
    mode->trigger();
    QPointer<QWidget> reopened = assembly->window()->findChild<QWidget *>(
        QStringLiteral("focusFunctionsSidebar"));
    QVERIFY(reopened && !reopened->isVisible());
    QVERIFY(bench.window->close());
    QTRY_VERIFY(!bench.window->isVisible());
    QVERIFY(!reopened);
  }

  void focusSequenceUsesNativeKeysAndIgnoresTyping() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "code-numbers.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    QTRY_VERIFY(ListingViewTestAccess::settled(*assembly->listing()));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 3);
    code->setPreludeFolded(false);
    QVERIFY(activateTestWindow(bench.window.get()));
    code->setFocus();
    code->setCursorLine(1);
    QVERIFY(code->findText(QStringLiteral("24"), true));
    auto *hexAction = bench.action(ActionId::EditOperandHex);
    QVERIFY(hexAction->isEnabled());
    QSignalSpy hex(hexAction, &QAction::triggered);
    auto *mode = bench.action(ActionId::ViewFocusMode);
    QTest::keyClick(code->window()->windowHandle(), Qt::Key_Q);
    QCOMPARE(hex.size(), 1); // No delay waiting to distinguish a Q sequence.
    QTRY_VERIFY(!code->loading() &&
                code->allText().contains(QStringLiteral("v0 + 0x18; /* 8 */")));
    QCOMPARE(code->window(), bench.window.get());
    code->setCursorLine(1);
    QVERIFY(code->findText(QStringLiteral("0x18"), true));
    auto *output = bench.window->findChild<OutputWindow *>();
    auto *input = output->findChild<QLineEdit *>();
    QVERIFY(input);
    const auto tap = [code] {
      QTest::keyClick(code->window()->windowHandle(), Qt::Key_F);
    };
    tap();
    QKeyEvent held(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier,
                   QStringLiteral("f"), true);
    QApplication::sendEvent(code->window()->windowHandle(), &held);
    QApplication::sendEvent(code->window()->windowHandle(), &held);
    QVERIFY(!mode->isChecked());
    // Another key, a focus change, or a long gap breaks the sequence.
    QTest::keyClick(code->window()->windowHandle(), Qt::Key_Right);
    tap();
    tap();
    QVERIFY(!mode->isChecked());
    input->setFocus();
    for (int i = 0; i < 3; ++i)
      QTest::keyClick(input->window()->windowHandle(), Qt::Key_F);
    QCOMPARE(input->text(), QStringLiteral("fff"));
    QVERIFY(!mode->isChecked());
    code->setFocus();
    tap();
    QVERIFY(!mode->isChecked());
    tap();
    QTest::qWait(1100);
    tap();
    QVERIFY(!mode->isChecked());
    input->setFocus();
    code->setFocus();
    bench.dockFor(assembly)->forceClose();
    QVERIFY(!assembly->isVisible());
    code->setFocus();
    // Deliberate taps need not fit the previous 400 ms interval. Route every
    // key through QWindow, including its forwarding to the focused widget.
    tap();
    QTest::qWait(600);
    tap();
    QTest::qWait(600);
    tap();
    QTRY_VERIFY(mode->isChecked());
    QVERIFY(assembly->isVisible());
    QCOMPARE(hex.size(), 1);
    mode->trigger();
    QVERIFY(!assembly->isVisible());
    QTRY_VERIFY(!mode->isChecked());
    QVERIFY(view->isVisible());
    QCOMPARE(code->window(), bench.window.get());
  }

  void fileReviewWindowsOfferWindowControls() {
    QFETCH(int, command);
    QFETCH(QString, name);
    Workbench bench;
    bench.action(ActionId(command))->trigger();
    QTRY_VERIFY(visibleDialog(name));
    QPointer<QDialog> dialog = visibleDialog(name);
    QVERIFY(dialog->windowFlags() & Qt::WindowMinimizeButtonHint);
    QVERIFY(dialog->windowFlags() & Qt::WindowMaximizeButtonHint);
    const QString captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
    if (!captures.isEmpty()) {
      auto &theme = Theme::instance();
      const auto mode = theme.mode();
      const auto restoreTheme = qScopeGuard([&] { theme.setMode(mode); });
      QDir().mkpath(captures);
      for (bool dark : {false, true}) {
        theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
        theme.apply();
        QApplication::processEvents();
        QVERIFY(dialog->grab().save(captures + "/" + name +
                                    (dark ? "-dark.png" : "-light.png")));
      }
    }
    if (auto *bar = dialog->findChild<TitleBar *>()) {
      auto *maximize =
          bar->findChild<QAbstractButton *>(QStringLiteral("maximizeButton"));
      auto *minimize =
          bar->findChild<QAbstractButton *>(QStringLiteral("minimizeButton"));
      QVERIFY(maximize && minimize);
      QVERIFY(maximize->isVisible() && minimize->isVisible());
      if (maximize->isVisible()) {
        QTest::mouseClick(maximize, Qt::LeftButton);
        QTRY_VERIFY(dialog->isMaximized());
        QTest::mouseClick(maximize, Qt::LeftButton);
        QTRY_VERIFY(!dialog->isMaximized());
      }
    }
    dialog->close();
    QTRY_VERIFY(!dialog || !dialog->isVisible());
  }

  void closingWorkbenchClosesDetachedWindows() {
    QFETCH(bool, dirty);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "exit-floating.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    auto *assemblyDock = bench.dockFor(assembly);
    auto *hexDock = bench.dockFor(bench.window->findChild<HexView *>());
    auto *functionsDock = bench.dockFor(bench.functions());
    QVERIFY(assemblyDock && hexDock && functionsDock);
    QList<QPointer<QWidget>> floating;
    for (auto *dock : {assemblyDock, hexDock, functionsDock}) {
      dock->setFloating(true);
      QVERIFY(dock->isFloating());
      floating.append(dock->QWidget::window());
      QVERIFY(QTest::qWaitForWindowExposed(floating.back()));
    }
    auto *sourceChild = new QDialog(assembly);
    sourceChild->setAttribute(Qt::WA_DeleteOnClose);
    sourceChild->show();
    const QPointer<QDialog> sourceGuard = sourceChild;
    QDialog mainChild(bench.window.get());
    mainChild.show();
    QDialog unrelated;
    unrelated.show();
    QVERIFY(QTest::qWaitForWindowExposed(sourceChild));
    QVERIFY(QTest::qWaitForWindowExposed(&mainChild));
    QSignalSpy shutdown(bench.window.get(), &MainWindow::shutdownAccepted);
    bool answered = false;
    QMessageBox::StandardButton response = QMessageBox::Cancel;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        if (auto *button = box->button(response)) {
          answer.stop();
          answered = true;
          button->click();
        }
      }
    });
    if (dirty) {
      bench.session.setComment(Base,
                               QStringLiteral("keep unless exit is accepted"));
      QTRY_VERIFY(bench.session.dirty());
      answer.start(10);
      bench.window->close();
      QTRY_VERIFY(answered);
      QCOMPARE(shutdown.size(), 0);
      QVERIFY(bench.window->isVisible());
      for (const auto &window : floating)
        QVERIFY(window && window->isVisible());
      QVERIFY(sourceGuard && sourceGuard->isVisible());
      QVERIFY(mainChild.isVisible());
      answered = false;
      response = QMessageBox::Discard;
      answer.start(10);
    }
    bench.window->close();
    QTRY_COMPARE(shutdown.size(), 1);
    QTRY_VERIFY(!bench.window->isVisible());
    if (dirty)
      QVERIFY(answered);
    for (const auto &window : floating)
      QTRY_VERIFY(!window || !window->isVisible());
    QTRY_VERIFY(!sourceGuard || !sourceGuard->isVisible());
    QVERIFY(!mainChild.isVisible());
    QVERIFY(unrelated.isVisible());
    QVERIFY(QFileInfo::exists(
        bench.layout.filePath(QStringLiteral("layout.json"))));
  }

  void closedFloatingDocksReopenInWorkbench_data() {
    QTest::addColumn<int>("command");
    QTest::addColumn<QString>("id");
    for (const auto &[id, action] :
         {std::pair{"disassembly-a", ActionId::ViewDisassembly},
          std::pair{"hex-1", ActionId::ViewHex},
          std::pair{"functions", ActionId::ViewFunctions},
          std::pair{"pseudocode-a", ActionId::ViewPseudocode},
          std::pair{"ir-a", ActionId::ViewMedIR},
          std::pair{"local-types", ActionId::ViewLocalTypes},
          std::pair{"type-libraries", ActionId::ViewTypeLibraries},
          std::pair{"pathfinder", ActionId::ViewPathfinder},
          std::pair{"extensions", ActionId::ViewExtensions},
          std::pair{"output", ActionId::ViewOutput},
          std::pair{"graph-overview", ActionId::ViewGraphOverview}})
      QTest::newRow(id) << int(action) << QString::fromLatin1(id);
    QTest::newRow("names")
        << int(ActionId::ViewNames)
        << QStringLiteral("chooser-%1").arg(int(ChooserKind::Names));
  }

  void closedFloatingDocksReopenInWorkbench() {
    QFETCH(int, command);
    QFETCH(QString, id);
    Theme::instance().apply();
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "reopen-dock.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    QTRY_VERIFY(ListingViewTestAccess::settled(*assembly->listing()));
    auto *action = bench.action(ActionId(command));
    action->trigger();
    KDDockWidgets::QtWidgets::DockWidget *dock = nullptr;
    for (auto *candidate :
         bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (candidate->uniqueName() == id)
        dock = candidate;
    QVERIFY(dock);
    QTRY_VERIFY(dock->isOpen());
    for (int method = 0; method < 2; ++method) {
      dock->setFloating(true);
      QTRY_VERIFY(dock->widget()->isVisible());
      QTRY_VERIFY(dock->isFloating());
      auto *floating = dock->QWidget::window();
      QVERIFY(floating != bench.window.get());
      // Raising an existing floating pane must not move it. Overview's action
      // is a visibility toggle, so it deliberately closes an open overview.
      if (ActionId(command) != ActionId::ViewGraphOverview) {
        if (auto *code = qobject_cast<CodeView *>(dock->widget())) {
          QVERIFY(activateTestWindow(floating));
          code->text()->setFocus();
          QTRY_VERIFY(code->text()->hasFocus());
        }
        action->trigger();
        QVERIFY(dock->isOpen());
        QCOMPARE(dock->QWidget::window(), floating);
      }
      if (method == 0)
        QVERIFY(floating->close());
      else
        dock->toggleAction()->trigger();
      QTRY_VERIFY(!dock->isOpen());
      QVERIFY(!dock->widget()->isVisible());
      action->trigger();
      QTRY_VERIFY(dock->isOpen() && dock->widget()->isVisible());
      QTRY_COMPARE(dock->QWidget::window(), bench.window.get());
      QVERIFY(!dock->isFloating());
    }
    if (ActionId(command) == ActionId::ViewExtensions)
      for (auto *button : dock->widget()->findChildren<QPushButton *>())
        QVERIFY2(button->width() >= button->sizeHint().width(),
                 qPrintable(button->text()));
    const QString captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
    if (ActionId(command) == ActionId::ViewExtensions && !captures.isEmpty()) {
      auto &theme = Theme::instance();
      const auto mode = theme.mode();
      const auto restore = qScopeGuard([&] { theme.setMode(mode); });
      QDir().mkpath(captures);
      for (const bool dark : {false, true}) {
        theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
        theme.apply();
        QApplication::processEvents();
        QVERIFY(bench.window->grab().save(
            captures + (dark ? "/queries-dark.png" : "/queries-light.png")));
      }
    }
  }

  void reopeningPaneDoesNotFollowFloatingAssembly() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "reopen-group.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY(bench.action(ActionId::ViewHex)->isEnabled());
    auto *assembly = bench.window->disassembly();
    auto *assemblyDock = bench.dockFor(assembly);
    auto *hex = bench.window->findChild<HexView *>();
    auto *hexDock = bench.dockFor(hex);
    QVERIFY(assemblyDock && hexDock);
    assemblyDock->setFloating(true);
    // Two tabs in one floating window report isFloating() differently from
    // a standalone floating dock. Reopening must still return to the main UI.
    assemblyDock->addDockWidgetAsTab(hexDock);
    QTRY_COMPARE(hex->window(), assembly->window());
    QVERIFY(hex->window() != bench.window.get());
    hexDock->toggleAction()->trigger();
    QTRY_VERIFY(!hexDock->isOpen());
    bench.action(ActionId::ViewHex)->trigger();
    QTRY_VERIFY(hex->isVisible());
    QCOMPARE(hex->window(), bench.window.get());
    QVERIFY(assembly->window() != bench.window.get());
    QVERIFY(assemblyDock->isOpen());
  }

  void floatingAnalysisMenusStayWithTheirView_data() {
    QTest::addColumn<QString>("viewKind");
    QTest::addColumn<bool>("dark");
    for (const auto &kind : {QStringLiteral("listing"), QStringLiteral("graph"),
                             QStringLiteral("source"), QStringLiteral("low")})
      for (const bool dark : {false, true})
        QTest::newRow(qPrintable(kind + (dark ? "-dark" : "-light")))
            << kind << dark;
  }

  void floatingAnalysisMenusStayWithTheirView() {
    QFETCH(QString, viewKind);
    QFETCH(bool, dark);
    auto &theme = Theme::instance();
    const auto mode = theme.mode();
    const auto restore = qScopeGuard([&] { theme.setMode(mode); });
    theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    theme.apply();
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "floating-menus.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    QTRY_VERIFY(assembly->currentAddress().has_value());
    QWidget *pane = assembly;
    QWidget *content = assembly->listing();
    if (viewKind == QLatin1String("graph")) {
      assembly->setGraphMode(true);
      content = assembly->graph();
    } else if (viewKind == QLatin1String("source") ||
               viewKind == QLatin1String("low")) {
      bench
          .action(viewKind == QLatin1String("source") ? ActionId::ViewPseudocode
                                                      : ActionId::ViewLowIR)
          ->trigger();
      auto *code = bench.codeView(viewKind);
      QVERIFY(code);
      QTRY_VERIFY(!code->text()->loading() && code->text()->lineCount());
      pane = code;
      content = code->text();
    }
    auto *dock = bench.dockFor(pane);
    QVERIFY(dock);
    dock->setFloating(true);
    auto *floating = pane->window();
    floating->move(220, 100);
    floating->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(floating));
    content->setFocus();
    QTRY_VERIFY(content->hasFocus());
    bool seen = false, renameSeen = false;
    QTimer inspectDialog;
    connect(&inspectDialog, &QTimer::timeout, this, [&] {
      auto *dialog =
          qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
      if (!dialog)
        return;
      inspectDialog.stop();
      const auto close = qScopeGuard([&] { dialog->reject(); });
      renameSeen = true;
      QCOMPARE(dialog->parentWidget(), floating);
      QCOMPARE(dialog->windowHandle()->transientParent(),
               floating->windowHandle());
    });
    if (viewKind == QLatin1String("listing"))
      inspectDialog.start(10);
    QTimer inspect;
    connect(&inspect, &QTimer::timeout, this, [&] {
      auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      if (!menu)
        return;
      inspect.stop();
      const auto close = qScopeGuard([&] { menu->close(); });
      seen = true;
      QCOMPARE(menu->objectName(), QStringLiteral("analysisContextMenu"));
      QCOMPARE(menu->parentWidget(), floating);
      QVERIFY(menu->windowHandle());
      QCOMPARE(menu->windowHandle()->transientParent(),
               floating->windowHandle());
      QVERIFY(menu->geometry().intersects(floating->geometry()));
      QVERIFY(
          menu->actions().contains(bench.action(ActionId::EditCopyAddress)));
      const QString captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
      if (!captures.isEmpty()) {
        QDir().mkpath(captures);
        QVERIFY(menu->grab().save(captures + "/menu-" + viewKind +
                                  (dark ? "-dark.png" : "-light.png")));
      }
      if (viewKind == QLatin1String("listing"))
        QTest::mouseClick(
            menu, Qt::LeftButton, Qt::NoModifier,
            menu->actionGeometry(bench.action(ActionId::EditRename)).center());
    });
    inspect.start(10);
    const QPoint point(45, 45);
    auto *scroll = qobject_cast<QAbstractScrollArea *>(content);
    auto *surface = scroll ? scroll->viewport() : content;
    QContextMenuEvent context(QContextMenuEvent::Mouse, point,
                              surface->mapToGlobal(point));
    QApplication::sendEvent(surface, &context);
    QVERIFY(seen);
    if (viewKind == QLatin1String("listing"))
      QTRY_VERIFY(renameSeen);
    QVERIFY(dock->isFloating());
  }

  void disassemblyMenuOffersTheLinesEdits() {
    // An instruction of a function offers the function's stack variables,
    // the SP the instruction leaves and its operand's forms; a data item its
    // array and alignment. A pseudocode window offers none of them.
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "line-edits.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    QTRY_VERIFY(assembly->currentAddress().has_value());
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    const auto offers = [&](QMenu *menu, ActionId id) {
      return menu->actions().contains(bench.action(id));
    };
    const auto checkInstruction = [&](QMenu *menu) {
      QVERIFY(offers(menu, ActionId::EditStackFrame));
      QVERIFY(offers(menu, ActionId::EditStackPointer));
      QVERIFY(!offers(menu, ActionId::EditMakeArray));
      QVERIFY(!offers(menu, ActionId::EditMakeAlignment));
      auto *forms = menu->findChild<QMenu *>(QStringLiteral("operandTypeMenu"));
      QVERIFY(forms);
      const auto operand = forms->actions();
      for (const auto id :
           {ActionId::EditOperandNumber, ActionId::EditOperandHex,
            ActionId::EditOperandDecimal, ActionId::EditOperandBinary,
            ActionId::EditOperandChar, ActionId::EditOperandOffset,
            ActionId::EditOperandEnum, ActionId::EditOperandNegate,
            ActionId::EditOperandInvert, ActionId::EditStackOperand})
        QVERIFY(operand.contains(bench.action(id)));
      // The structure offset keeps its own entry.
      QVERIFY(offers(menu, ActionId::EditOperandStruct));
      QVERIFY(!operand.contains(bench.action(ActionId::EditOperandStruct)));
    };
    const Address function = Base + 0x140;
    assembly->navigate(function);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(function));
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    QTRY_VERIFY(assembly->onInstruction());
    bool shown = false;
    withAssemblyMenu(assembly, [&](QMenu *menu) {
      shown = true;
      checkInstruction(menu);
    });
    QVERIFY(shown);
    assembly->setGraphMode(true);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(function));
    QTRY_VERIFY(assembly->onInstruction());
    shown = false;
    withAssemblyMenu(assembly, [&](QMenu *menu) {
      shown = true;
      checkInstruction(menu);
    });
    QVERIFY(shown);
    assembly->setGraphMode(false);

    // A pointer of the read-only data section.
    const Address slot = Base + 0x3000;
    assembly->navigate(slot);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(slot));
    QTRY_VERIFY(assembly->onData());
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    shown = false;
    withAssemblyMenu(assembly, [&](QMenu *menu) {
      shown = true;
      QVERIFY(offers(menu, ActionId::EditMakeArray));
      QVERIFY(offers(menu, ActionId::EditMakeAlignment));
      QVERIFY(!offers(menu, ActionId::EditStackFrame));
      QVERIFY(!offers(menu, ActionId::EditStackPointer));
      QVERIFY(!menu->findChild<QMenu *>(QStringLiteral("operandTypeMenu")));
    });
    QVERIFY(shown);

    // Its function can differ from the disassembly's, which these edit.
    assembly->navigate(function);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    QTRY_VERIFY(!view->text()->loading() && view->text()->lineCount());
    view->text()->setFocus();
    QTRY_VERIFY(view->text()->hasFocus());
    shown = false;
    withCodeMenu(view->text(), [&](QMenu *menu) {
      shown = true;
      for (const auto id :
           {ActionId::EditStackFrame, ActionId::EditStackPointer,
            ActionId::EditMakeArray, ActionId::EditMakeAlignment})
        QVERIFY(!offers(menu, id));
      QVERIFY(!menu->findChild<QMenu *>(QStringLiteral("operandTypeMenu")));
    });
    QVERIFY(shown);
  }

  void floatingWindowsKeepMouseHistory() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "floating-history.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    QTRY_VERIFY(assembly->currentAddress().has_value());
    auto *dock = bench.dockFor(assembly);
    QVERIFY(dock);
    dock->setFloating(true);
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(Base + 0x140));
    assembly->navigate(Base + 0x280);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(Base + 0x280));
    QTest::mouseClick(assembly->listing()->viewport(), Qt::BackButton);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(Base + 0x140));
    QTest::mouseClick(assembly->listing()->viewport(), Qt::ForwardButton);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(Base + 0x280));
  }

  void floatingCommandDialogsStayWithTheirView_data() {
    QTest::addColumn<int>("command");
    for (const auto &[name, action] :
         {std::pair{"text", ActionId::SearchText},
          std::pair{"bytes", ActionId::SearchBytes},
          std::pair{"immediate", ActionId::SearchImmediate},
          std::pair{"link", ActionId::JumpLink},
          std::pair{"calculator", ActionId::ViewCalculator},
          std::pair{"general", ActionId::OptionsGeneral},
          std::pair{"colors", ActionId::OptionsColors},
          std::pair{"strings", ActionId::OptionsStrings},
          std::pair{"shortcuts", ActionId::OptionsShortcuts},
          std::pair{"connections", ActionId::ViewConnections},
          std::pair{"about", ActionId::HelpAbout}})
      QTest::newRow(name) << int(action);
  }

  void floatingCommandDialogsStayWithTheirView() {
    QFETCH(int, command);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "floating-dialogs.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    QTRY_VERIFY(assembly->currentAddress().has_value());
    auto *dock = bench.dockFor(assembly);
    QVERIFY(dock);
    dock->setFloating(true);
    auto *owner = assembly->window();
    owner->activateWindow();
    QVERIFY(QTest::qWaitForWindowExposed(owner));
    QTRY_VERIFY(owner->isActiveWindow());
    QTRY_COMPARE(QGuiApplication::focusWindow(), owner->windowHandle());
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    const bool calculator = ActionId(command) == ActionId::ViewCalculator;
    int inspected = 0;
    QTimer inspect;
    connect(&inspect, &QTimer::timeout, this, [&] {
      QPointer<QDialog> dialog = visibleDialog();
      // About can also be presented as a modeless panel.
      if (!dialog && ActionId(command) == ActionId::HelpAbout)
        for (auto *widget : QApplication::topLevelWidgets())
          if (auto *about = qobject_cast<QMessageBox *>(widget);
              about && about->isVisible()) {
            dialog = about;
            break;
          }
      if (!dialog || !dialog->isVisible() || !dialog->windowHandle())
        return;
      const auto close = qScopeGuard([&] {
        if (dialog && dialog->isVisible())
          dialog->reject();
      });
      ++inspected;
      QCOMPARE(dialog->parentWidget()->window(), owner);
      QCOMPARE(dialog->windowHandle()->transientParent(),
               owner->windowHandle());
      if (calculator && inspected == 1) {
        auto *input = qobject_cast<QInputDialog *>(dialog);
        QVERIFY(input);
        input->setTextValue(QStringLiteral("0x10"));
        input->accept();
      } else {
        inspect.stop();
        if (calculator) {
          auto *result = qobject_cast<QMessageBox *>(dialog);
          QVERIFY(result);
          QVERIFY(result->text().contains(QStringLiteral("Decimal: 16")));
        }
      }
    });
    inspect.start(10);
    QTimer deadline;
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, [] {
      if (auto *dialog = visibleDialog())
        dialog->close();
    });
    deadline.start(OpenTimeoutMs);
    QVERIFY(bench.action(ActionId(command))->isEnabled());
    bench.action(ActionId(command))->trigger();
    QTRY_COMPARE(inspected, calculator ? 2 : 1);
    QVERIFY(dock->isFloating());
  }

  void selectedBytesExportWithIdasKey_data() {
    QTest::addColumn<bool>("hex");
    QTest::addColumn<bool>("floating");
    for (const bool hex : {false, true})
      for (const bool floating : {false, true})
        QTest::newRow(qPrintable(QString(hex ? "hex" : "listing") +
                                 (floating ? "-floating" : "-docked")))
            << hex << floating;
  }

  void analysisBrowsersStayInteractive_data() {
    QTest::addColumn<int>("command");
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("floating");
    QTest::addColumn<bool>("sessionBound");
    struct Case {
      ActionId Command;
      const char *Name;
      bool SessionBound = true;
    };
    for (const bool floating : {false, true})
      for (const auto &item :
           {Case{ActionId::JumpXrefsTo, "referencesDialog"},
            Case{ActionId::JumpXrefsFrom, "referencesDialog"},
            Case{ActionId::JumpAnywhere, "jumpDialog"},
            Case{ActionId::JumpEntry, "locationChooser"},
            Case{ActionId::JumpFunction, "locationChooser"},
            Case{ActionId::JumpByName, "locationChooser"},
            Case{ActionId::JumpSegment, "locationChooser"},
            Case{ActionId::EditExportData, "exportDataDialog"},
            Case{ActionId::SearchInView, "findInViewDialog"},
            Case{ActionId::OptionsShortcuts, "shortcutsDialog", false},
            Case{ActionId::ViewConnections, "connectionsDialog", false},
            Case{ActionId::ViewCalculator, "calculatorDialog"},
            Case{ActionId::HelpAbout, "aboutDialog", false},
            Case{ActionId::HelpX87Capabilities, "x87Capabilities", false}})
        QTest::newRow(qPrintable(QStringLiteral("%1-%2")
                                     .arg(int(item.Command))
                                     .arg(floating ? "floating" : "docked")))
            << int(item.Command) << QString::fromLatin1(item.Name) << floating
            << item.SessionBound;
  }

  void analysisBrowsersStayInteractive() {
    QFETCH(int, command);
    QFETCH(QString, name);
    QFETCH(bool, floating);
    QFETCH(bool, sessionBound);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "byte-export.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    auto *listing = assembly->listing();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(Base + 0x140));
    auto *dock = bench.dockFor(assembly);
    QVERIFY(dock);
    if (floating)
      dock->setFloating(true);
    const auto open = [&] {
      QVERIFY(activateTestWindow(assembly->window()));
      assembly->focusContent();
      QTRY_VERIFY(listing->hasFocus());
      if (ActionId(command) == ActionId::EditExportData) {
        QTRY_VERIFY(ListingViewTestAccess::settled(*listing));
        QTest::keyClick(listing, Qt::Key_Home);
        QTest::keyClick(listing, Qt::Key_End, Qt::ShiftModifier);
        QVERIFY(listing->byteSelection().has_value());
      }
      QVERIFY(bench.action(ActionId(command))->isEnabled());
      QTimer guard;
      guard.setSingleShot(true);
      connect(&guard, &QTimer::timeout, &guard, [] {
        if (auto *modal = QApplication::activeModalWidget())
          modal->close();
      });
      guard.start(OpenTimeoutMs);
      bench.action(ActionId(command))->trigger();
      QTRY_VERIFY(visibleDialog(name));
    };
    open();
    QPointer<QDialog> dialog = visibleDialog(name);
    QVERIFY(dialog);
    QCOMPARE(dialog->parentWidget(), assembly);
    QCOMPARE(dialog->windowModality(), Qt::NonModal);
    QVERIFY(!QApplication::activeModalWidget());
    QVERIFY(activateTestWindow(dialog));

    // Background keyboard input remains usable while browsing. The list is
    // a snapshot of its opening location, not tied to the new cursor.
    QVERIFY(activateTestWindow(assembly->window()));
    assembly->focusContent();
    QTRY_VERIFY(listing->hasFocus());
    const auto address = assembly->currentAddress();
    QTest::keyClick(listing, Qt::Key_Down);
    QTRY_VERIFY(assembly->currentAddress() != address);
    QVERIFY(dialog && dialog->isVisible());
    dock->setFloating(!floating);
    dock->raise();
    QTRY_COMPARE(dock->isFloating(), !floating);
    QTRY_VERIFY(assembly->isVisible());
    QVERIFY(dialog && dialog->isVisible());
    QTRY_COMPARE(dialog->windowHandle()->transientParent(),
                 assembly->window()->windowHandle());

    // Reopening replaces the same browser; Find reuses its existing field.
    open();
    QPointer<QDialog> replacement = visibleDialog(name);
    QVERIFY(replacement);
    if (ActionId(command) != ActionId::SearchInView) {
      QVERIFY(replacement != dialog);
      QTRY_VERIFY(!dialog);
    }
    QCOMPARE(assembly->findChildren<QDialog *>(name, Qt::FindDirectChildrenOnly)
                 .size(),
             1);
    dock->forceClose();
    QTRY_VERIFY(!replacement);
    bench.action(ActionId::ViewDisassembly)->trigger();
    QTRY_VERIFY(assembly->isVisible());
    open();
    dialog = visibleDialog(name);
    QVERIFY(dialog);
    bench.session.closeFile();
    if (sessionBound) {
      QTRY_VERIFY(!dialog);
    } else {
      QVERIFY(dialog && dialog->isVisible());
      dialog->reject();
      QTRY_VERIFY(!dialog);
    }
  }

  void auxiliaryChoosersStayInteractive_data() {
    QTest::addColumn<bool>("types");
    QTest::addColumn<bool>("floating");
    for (const bool types : {false, true})
      for (const bool floating : {false, true})
        QTest::newRow(
            qPrintable(QString(types ? "type-libraries" : "pathfinder") +
                       (floating ? "-floating" : "-docked")))
            << types << floating;
  }

  void auxiliaryChoosersStayInteractive() {
    QFETCH(bool, types);
    QFETCH(bool, floating);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "local-types.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench
        .action(types ? ActionId::ViewTypeLibraries : ActionId::ViewPathfinder)
        ->trigger();
    QWidget *source = types
                          ? static_cast<QWidget *>(
                                bench.window->findChild<TypeLibrariesView *>())
                          : bench.window->findChild<PathfinderView *>();
    QVERIFY(source);
    auto *dock = bench.dockFor(source);
    QVERIFY(dock);
    if (floating)
      dock->setFloating(true);
    QVERIFY(activateTestWindow(source->window()));
    auto *add = source->findChild<QToolButton *>(
        types ? QStringLiteral("typeLibrariesAdd")
              : QStringLiteral("pathfinderAdd"));
    QVERIFY(add);
    QTRY_VERIFY(add->isEnabled());
    QTest::mouseClick(add, Qt::LeftButton);
    QPointer<QDialog> dialog =
        visibleDialog(types ? QStringLiteral("addTypeLibraryDialog")
                            : QStringLiteral("pathfinderChooser"));
    QVERIFY(dialog);
    QCOMPARE(dialog->parentWidget(), source);
    QCOMPARE(dialog->windowModality(), Qt::NonModal);
    QVERIFY(!QApplication::activeModalWidget());
    QVERIFY(activateTestWindow(dialog));
    QVERIFY(activateTestWindow(bench.window.get()));
    bench.action(ActionId::ViewDisassembly)->trigger();
    auto *assembly = bench.window->disassembly();
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    const auto before = assembly->currentAddress();
    QTest::keyClick(assembly->listing(), Qt::Key_Down);
    QTRY_VERIFY(assembly->currentAddress() != before);
    dock->setFloating(!floating);
    dock->raise();
    QTRY_COMPARE(dock->isFloating(), !floating);
    QTRY_VERIFY(source->isVisible());
    QVERIFY(dialog && dialog->isVisible());
    QTRY_COMPARE(dialog->windowHandle()->transientParent(),
                 source->window()->windowHandle());
    if (types) {
      bench.session.rename(Base, QStringLiteral("updated_while_choosing"));
      QTRY_VERIFY(!dialog);
      QTRY_VERIFY(add->isEnabled());
      QVERIFY(activateTestWindow(source->window()));
      QTest::mouseClick(add, Qt::LeftButton);
      dialog = visibleDialog(QStringLiteral("addTypeLibraryDialog"));
      QVERIFY(dialog);
    }
    // A revision arrives before the rename's write completion. Closing while
    // that write is still pending opens the unsaved-edits confirmation.
    QTRY_VERIFY(!bench.session.dirty());
    bench.session.closeFile();
    QTRY_VERIFY(!bench.session.loaded());
    QTRY_VERIFY(!dialog);
  }

  void jumpBrowserKeepsItsOpeningAddress_data() {
    QTest::addColumn<bool>("floating");
    QTest::newRow("docked") << false;
    QTest::newRow("floating") << true;
  }

  void jumpBrowserKeepsItsOpeningAddress() {
    QFETCH(bool, floating);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "classic-jumps.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    auto *dock = bench.dockFor(assembly);
    QVERIFY(dock);
    if (floating)
      dock->setFloating(true);
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(Base + 0x140));
    QVERIFY(activateTestWindow(assembly->window()));
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    bench.action(ActionId::JumpAnywhere)->trigger();
    QPointer<QDialog> dialog = visibleDialog("jumpDialog");
    QVERIFY(dialog);
    QVERIFY(activateTestWindow(dialog));
    auto *input = dialog->findChild<QLineEdit *>();
    QVERIFY(input);
    input->setText(QStringLiteral("here+0x23"));
    QVERIFY(activateTestWindow(assembly->window()));
    assembly->navigate(Base + 0x180);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(Base + 0x180));
    QVERIFY(activateTestWindow(dialog));
    input->setFocus();
    QTRY_VERIFY(input->hasFocus());
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY(!dialog);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(Base + 0x163));
  }

  void commandPaletteReturnsToItsSource_data() {
    jumpBrowserKeepsItsOpeningAddress_data();
  }

  void commandPaletteReturnsToItsSource() {
    QFETCH(bool, floating);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "classic-jumps.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    auto *dock = bench.dockFor(assembly);
    QVERIFY(dock);
    if (floating)
      dock->setFloating(true);
    for (const bool accept : {false, true}) {
      QVERIFY(activateTestWindow(assembly->window()));
      assembly->focusContent();
      QTRY_VERIFY(assembly->listing()->hasFocus());
      QTRY_VERIFY(bench.action(ActionId::EditRepeatableComment)->isEnabled());
      bench.action(ActionId::OptionsPalette)->trigger();
      QPointer<QDialog> palette = visibleDialog("commandPalette");
      QVERIFY(palette);
      auto *commands = palette->findChild<QListWidget *>();
      QVERIFY(commands);
      const auto comments = commands->findItems(
          QStringLiteral("Enter comment..."), Qt::MatchExactly);
      QCOMPARE(comments.size(), 1);
      QCOMPARE(comments.front()->data(Qt::UserRole).toString(),
               QStringLiteral("EditRepeatableComment"));
      QCOMPARE(QApplication::activePopupWidget(), palette.data());
      QVERIFY(!QApplication::activeModalWidget());
      if (QGuiApplication::platformName() == QLatin1String("cocoa")) {
        // Cocoa popup panels retain their owner's native key window. Qt
        // routes that window's keys to the active popup's focused widget.
        QTRY_COMPARE(QGuiApplication::focusWindow(),
                     assembly->window()->windowHandle());
      } else {
        QVERIFY(activateTestWindow(palette));
        QTRY_COMPARE(QGuiApplication::focusWindow(), palette->windowHandle());
      }
      auto *input = palette->findChild<QLineEdit *>();
      QVERIFY(input);
      QTRY_VERIFY(input->hasFocus());
      QVERIFY(QGuiApplication::focusWindow());
      QTest::keyClick(QGuiApplication::focusWindow(), 'a');
      QTRY_COMPARE(input->text(), QStringLiteral("a"));
      input->setText(bench.action(ActionId::JumpAnywhere)->text().remove('&'));
      QSignalSpy operand(bench.action(ActionId::JumpOperand),
                         &QAction::triggered);
      QTest::keyClick(QGuiApplication::focusWindow(),
                      accept ? Qt::Key_Return : Qt::Key_Escape);
      QVERIFY(operand.isEmpty());
      QTRY_VERIFY(!palette);
      if (accept) {
        QTRY_VERIFY(visibleDialog("jumpDialog"));
        auto *jump = visibleDialog("jumpDialog");
        QCOMPARE(jump->parentWidget(), assembly);
        QCOMPARE(jump->windowHandle()->transientParent(),
                 assembly->window()->windowHandle());
        jump->reject();
      } else {
        QVERIFY(!visibleDialog("jumpDialog"));
      }
    }
  }

  void commandPaletteDropsFromTheCommandCenter() {
    // A click on the command center drops the palette from it, as in Visual
    // Studio Code; its key still opens it over the pane its commands act on.
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "classic-jumps.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *center = bench.window->findChild<QAbstractButton *>(
        QStringLiteral("commandCenter"));
    if (!center)
      QSKIP("This window draws no command center.");
    auto *bar = center->parentWidget();
    auto *assembly = bench.window->disassembly();
    QVERIFY(activateTestWindow(assembly->window()));
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    QTest::mouseClick(center, Qt::LeftButton);
    QPointer<QDialog> palette = visibleDialog("commandPalette");
    QVERIFY(palette);
    const QRect dropped = palette->geometry();
    QCOMPARE(dropped.top(), bar->mapToGlobal(QPoint(0, bar->height())).y());
    QVERIFY(dropped.width() >= center->width());
    // Centered under the field, unless that would leave the screen.
    const QRect area = center->screen()->availableGeometry();
    QCOMPARE(
        dropped.left(),
        std::clamp(center->mapToGlobal(QPoint(center->width() / 2, 0)).x() -
                       dropped.width() / 2,
                   area.left(),
                   std::max(area.left(), area.right() + 1 - dropped.width())));
    palette->reject();
    QTRY_VERIFY(!palette);
    QVERIFY(activateTestWindow(assembly->window()));
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    bench.action(ActionId::OptionsPalette)->trigger();
    palette = visibleDialog("commandPalette");
    QVERIFY(palette);
    auto *pane = bench.dockFor(assembly)->widget();
    QCOMPARE(palette->geometry().center(),
             pane->mapToGlobal(pane->rect().center()));
    palette->reject();
    QTRY_VERIFY(!palette);
  }

  void selectedBytesExportWithIdasKey() {
    QFETCH(bool, hex);
    QFETCH(bool, floating);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "byte-export.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    QTRY_VERIFY(assembly->currentAddress().has_value());
    assembly->navigate(Base + 0x142);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(Base + 0x142));
    auto *hexView = bench.window->findChild<HexView *>();
    QWidget *pane = hex ? static_cast<QWidget *>(hexView) : assembly;
    QWidget *keys = hex ? static_cast<QWidget *>(hexView) : assembly->listing();
    auto *dock = bench.dockFor(pane);
    QVERIFY(dock);
    dock->open();
    dock->raise();
    if (floating)
      dock->setFloating(true);
    auto *owner = pane->window();
    owner->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(owner));
    keys->setFocus();
    QTRY_VERIFY(keys->hasFocus());
    QTRY_VERIFY(ListingViewTestAccess::settled(*assembly->listing()));
    if (hex) {
      hexView->setCurrent(Base + 0x142);
      QTest::keyClick(keys, Qt::Key_Right, Qt::ShiftModifier);
      QTest::keyClick(keys, Qt::Key_Right, Qt::ShiftModifier);
    } else {
      QTest::keyClick(keys, Qt::Key_Down, Qt::ShiftModifier);
      QTest::keyClick(keys, Qt::Key_Down, Qt::ShiftModifier);
      QTest::keyClick(keys, Qt::Key_End, Qt::ShiftModifier);
    }
    const auto expected =
        std::pair<Address, Address>{Base + 0x142, Base + 0x144};
    QCOMPARE(hex ? hexView->byteSelection()
                 : assembly->listing()->byteSelection(),
             std::optional(expected));
    bool seen = false;
    QTimer inspect;
    connect(&inspect, &QTimer::timeout, this, [&] {
      QPointer<ExportDataDialog> dialog = qobject_cast<ExportDataDialog *>(
          visibleDialog(QStringLiteral("exportDataDialog")));
      if (!dialog)
        return;
      auto *save = dialog->findChild<QPushButton *>("exportSave");
      if (!save->isEnabled())
        return;
      inspect.stop();
      const auto close = qScopeGuard([&] {
        if (dialog)
          dialog->reject();
      });
      seen = true;
      QCOMPARE(dialog->parentWidget()->window(), owner);
      auto *format = dialog->findChild<QComboBox *>("exportFormat");
      auto *preview = dialog->findChild<QPlainTextEdit *>("exportPreview");
      auto *copy = dialog->findChild<QPushButton *>("exportCopy");
      const QStringList expectedText{
          "020304", "02 03 04", "\"\\002\\003\\004\"",
          "unsigned char data[] = { 0x02, 0x03, 0x04 };",
          "unsigned char data[] = { 2, 3, 4 };"};
      for (int i = 0; i < expectedText.size(); ++i) {
        format->setCurrentIndex(i);
        QCOMPARE(preview->toPlainText(), expectedText[i]);
        QTest::mouseClick(copy, Qt::LeftButton);
        QCOMPARE(clipboardText(), expectedText[i]);
      }
      format->setCurrentIndex(5);
      QVERIFY(!copy->isEnabled());
      const QString target = directory.filePath("export.bin");
      QVERIFY(dialog->saveToFile(target));
      QCOMPARE(readAll(target), QByteArray::fromHex("020304"));
      const QString captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
      if (!captures.isEmpty()) {
        QDir().mkpath(captures);
        QVERIFY(dialog->grab().save(
            captures + (hex ? "/export-hex.png" : "/export-listing.png")));
      }
      auto *buttons = dialog->findChild<QDialogButtonBox *>();
      QVERIFY(buttons);
      QVERIFY(!buttons->button(QDialogButtonBox::Close)
                   ->text()
                   .contains(QKeySequence(Qt::Key_Escape)
                                 .toString(QKeySequence::NativeText)));
      QTest::keyClick(dialog, Qt::Key_Escape);
      QVERIFY(!dialog->isVisible());
    });
    inspect.start(10);
    QTimer deadline;
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, [] {
      if (auto *dialog = visibleDialog())
        dialog->close();
    });
    deadline.start(OpenTimeoutMs);
    QTest::keyClick(keys, Qt::Key_E, Qt::ShiftModifier);
    QTRY_VERIFY(seen);
  }

  void byteExportsRejectHolesAndRetireStaleReads() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "export-ranges.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    ExportDataDialog dialog(bench.session, Base, Base + 8200,
                            bench.window.get());
    dialog.show();
    auto *save = dialog.findChild<QPushButton *>("exportSave");
    auto *start = dialog.findChild<QLineEdit *>("exportStart");
    auto *end = dialog.findChild<QLineEdit *>("exportEnd");
    auto *format = dialog.findChild<QComboBox *>("exportFormat");
    auto *status = dialog.findChild<QLabel *>("exportStatus");
    QTRY_VERIFY(save->isEnabled());
    format->setCurrentIndex(5);
    const QString path = directory.filePath("range.bin");
    QVERIFY(dialog.saveToFile(path));
    QByteArray expected;
    for (int i = 0; i <= 8200; ++i)
      expected.append(char(i & 15));
    QCOMPARE(readAll(path), expected);
    QVERIFY(status->text().contains("8201"));
    end->setText(hexAddress(
        Base + 9600)); // A missing final byte rejects the whole export.
    QVERIFY(!save->isEnabled());
    QTRY_VERIFY(status->text().contains("Cannot read"));
    QVERIFY(!dialog.saveToFile(path));
    QCOMPARE(readAll(path), expected);
    end->setText(QStringLiteral("0xffffffffffffffff"));
    QTRY_VERIFY(status->text().contains("at most"));
    start->setText(QStringLiteral("0xffffffffffffffff"));
    end->setText(QStringLiteral("0x0"));
    QTRY_VERIFY(status->text().contains("valid address range"));
    start->setText(hexAddress(Base));
    end->setText(hexAddress(Base + 15));
    QTRY_VERIFY(save->isEnabled());
    // A new input range immediately retires the previous complete bytes.
    end->setText(hexAddress(Base + 31));
    QVERIFY(!dialog.saveToFile(path));
    bench.session.closeFile();
    QTRY_VERIFY(!dialog.isVisible());
    QVERIFY(!dialog.saveToFile(path));
    QCOMPARE(readAll(path), expected);
  }

  void classicJumpChoosersSelectAndCancel_data() {
    QTest::addColumn<int>("key");
    QTest::addColumn<int>("kind");
    QTest::addColumn<QString>("filter");
    QTest::addColumn<quint64>("target");
    QTest::addColumn<bool>("dark");
    QTest::addColumn<bool>("floating");
    for (const bool dark : {true, false}) {
      for (const bool floating : {false, true}) {
        const QByteArray suffix = QByteArray(dark ? "-dark" : "-light") +
                                  (floating ? "-floating" : "-docked");
        QTest::newRow(QByteArray("entry" + suffix).constData())
            << int(Qt::Key_E) << int(ChooserKind::Exports)
            << QStringLiteral("start") << quint64(Base) << dark << floating;
        QTest::newRow(QByteArray("function" + suffix).constData())
            << int(Qt::Key_P) << int(ChooserKind::Functions)
            << QStringLiteral("function_18") << quint64(Base + 0x120) << dark
            << floating;
        QTest::newRow(QByteArray("name" + suffix).constData())
            << int(Qt::Key_L) << int(ChooserKind::Names)
            << QStringLiteral("function_0") << quint64(Base) << dark
            << floating;
        QTest::newRow(QByteArray("segment" + suffix).constData())
            << int(Qt::Key_S) << int(ChooserKind::Segments) << QString()
            << quint64(Base) << dark << floating;
      }
    }
  }

  void classicJumpChoosersSelectAndCancel() {
    QFETCH(int, key);
    QFETCH(int, kind);
    QFETCH(QString, filter);
    QFETCH(quint64, target);
    QFETCH(bool, dark);
    QFETCH(bool, floating);
    auto &theme = Theme::instance();
    const auto originalMode = theme.mode();
    const auto restore = qScopeGuard([&] { theme.setMode(originalMode); });
    theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    theme.apply();
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "classic-jumps.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address original = Base + 0x140;
    QTRY_VERIFY(assembly->currentAddress().has_value());
    assembly->navigate(original);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(original));
    if (floating) {
      auto *dock = bench.dockFor(assembly);
      QVERIFY(dock);
      dock->setFloating(true);
    }
    QWidget *owner = assembly->window();
    for (const bool accept : {false, true}) {
      owner->activateWindow();
      QVERIFY(QTest::qWaitForWindowExposed(owner));
      QTRY_VERIFY(owner->isActiveWindow());
      // A closed Wayland dialog returns native keyboard focus asynchronously.
      QTRY_COMPARE(QGuiApplication::focusWindow(), owner->windowHandle());
      assembly->focusContent();
      QTRY_VERIFY(assembly->listing()->hasFocus());
      bool sawDialog = false, typedFilter = false, timedOut = false;
      QTimer answer, deadline;
      deadline.setSingleShot(true);
      connect(&deadline, &QTimer::timeout, this, [&] {
        timedOut = true;
        qWarning() << "Chooser timeout; active window:"
                   << QApplication::activeWindow()
                   << "focus:" << QApplication::focusWidget();
        if (auto *dialog = visibleDialog())
          dialog->close();
      });
      connect(&answer, &QTimer::timeout, this, [&] {
        QPointer<QDialog> dialog = visibleDialog();
        if (!dialog || dialog->objectName() != "locationChooser")
          return;
        auto *view = dialog->findChild<ChooserView *>("locationChooserView");
        if (!view || view->model().loading() || !view->model().total())
          return;
        if (!dialog->isActiveWindow() ||
            QGuiApplication::focusWindow() != dialog->windowHandle() ||
            (!typedFilter && !view->table()->hasFocus()))
          return;
        if (accept && !filter.isEmpty() && !typedFilter) {
          typedFilter = true;
          QTest::keyClicks(view->table(), filter);
          return;
        }
        if (accept && !filter.isEmpty() && view->model().filter() != filter)
          return;
        answer.stop();
        auto closeOnFailure = qScopeGuard([dialog] {
          if (dialog)
            dialog->reject();
        });
        sawDialog = true;
        QCOMPARE(dialog->parentWidget()->window(), owner);
        QCOMPARE(int(view->model().kind()), kind);
        QVERIFY(dialog->windowTitle().contains("Jump"));
        QCOMPARE(assembly->currentAddress(), std::optional<Address>(original));
        if (accept) {
          view->table()->setCurrentIndex(view->model().index(0, 0));
          QCOMPARE(view->currentAddress(), std::optional<Address>(target));
          const QString captures =
              qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
          if (!captures.isEmpty() && kind == int(ChooserKind::Exports)) {
            QDir().mkpath(captures);
            QVERIFY(
                dialog->grab().save(captures + (dark ? "/entries-dark.png"
                                                     : "/entries-light.png")));
          }
          QTest::keyClick(view->table(), Qt::Key_Return);
        } else {
          QTest::keyClick(view->table(), Qt::Key_Escape);
        }
        closeOnFailure.dismiss();
      });
      answer.start(10);
      deadline.start(OpenTimeoutMs);
      QTest::keyClick(assembly->listing(), Qt::Key(key), Qt::ControlModifier);
      QTRY_VERIFY_WITH_TIMEOUT(sawDialog || timedOut, OpenTimeoutMs);
      deadline.stop();
      answer.stop();
      QVERIFY(!timedOut);
      QVERIFY(sawDialog);
      QTRY_COMPARE(assembly->currentAddress(),
                   std::optional<Address>(accept ? target : original));
    }
    // The chooser's jump participates in the usual Esc history.
    owner->activateWindow();
    QVERIFY(QTest::qWaitForWindowExposed(owner));
    QTRY_VERIFY(owner->isActiveWindow());
    QTRY_COMPARE(QGuiApplication::focusWindow(), owner->windowHandle());
    assembly->focusContent();
    QTest::keyClick(assembly->listing(), Qt::Key_Escape);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(original));
  }

  void shortcutsCanBeFoundByKeyAndScope() {
    Workbench bench;
    bool inspected = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      QPointer<QDialog> dialog = visibleDialog();
      if (!dialog || dialog->objectName() != "shortcutsDialog")
        return;
      answer.stop();
      const auto close = qScopeGuard([dialog] {
        if (dialog)
          dialog->reject();
      });
      inspected = true;
      auto *filter = dialog->findChild<QLineEdit *>("shortcutsFilter");
      auto *tree = dialog->findChild<QTreeWidget *>("shortcutsList");
      QVERIFY(filter && tree);
      QVERIFY(tree->topLevelItemCount() > int(ActionId::Count));
      const auto entryKey =
          QKeySequence(Qt::CTRL | Qt::Key_E).toString(QKeySequence::NativeText);
      for (const auto &query : {entryKey, QStringLiteral("Ctrl+E")}) {
        filter->setText(query);
        QSet<QString> scopes;
        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
          auto *item = tree->topLevelItem(row);
          if (!item->isHidden()) {
            QCOMPARE(item->text(1), entryKey);
            scopes.insert(item->text(2));
          }
        }
        QCOMPARE(scopes,
                 QSet<QString>({"Workbench", "Functions", "Local Types"}));
      }
      filter->setText(QStringLiteral("Exports"));
      bool foundUnassigned = false;
      for (int row = 0; row < tree->topLevelItemCount(); ++row) {
        auto *item = tree->topLevelItem(row);
        if (!item->isHidden() && item->text(0) == "Exports")
          foundUnassigned = item->text(1) == "Unassigned";
      }
      QVERIFY(foundUnassigned);
      for (const auto &query : {QStringLiteral(";"), QStringLiteral("/")}) {
        filter->setText(query);
        int matches = 0;
        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
          auto *item = tree->topLevelItem(row);
          if (item->isHidden())
            continue;
          ++matches;
          QCOMPARE(item->data(0, Qt::UserRole).toString(),
                   QStringLiteral("EditRepeatableComment"));
          QCOMPARE(item->text(0), QStringLiteral("Enter comment..."));
          QCOMPARE(item->text(1), QStringLiteral(";, /"));
          QCOMPARE(item->text(2), QStringLiteral("Analysis views"));
        }
        QCOMPARE(matches, 1);
      }
      filter->clear();
      const QString captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
      if (!captures.isEmpty()) {
        QDir().mkpath(captures);
        QVERIFY(dialog->grab().save(captures + "/shortcuts.png"));
      }
    });
    answer.start(10);
    bench.action(ActionId::OptionsShortcuts)->trigger();
    QTRY_VERIFY(inspected);
  }

  void commandButtonsExposeTheirKeys_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
  }

  void commandButtonsExposeTheirKeys() {
    QFETCH(bool, dark);
    auto &theme = Theme::instance();
    const auto originalMode = theme.mode();
    const auto restore = qScopeGuard([&] { theme.setMode(originalMode); });
    theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    theme.apply();
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "local-types.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const QString captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
    if (!captures.isEmpty())
      QDir().mkpath(captures);
    const auto inspect = [&](ActionId action, const QStringList &names,
                             const QList<QKeySequence> &keys,
                             const QString &capture) {
      bench.action(action)->trigger();
      if (action == ActionId::ViewLocalTypes) {
        auto *list = bench.window->findChild<QTreeView *>("localTypesList");
        QVERIFY(list);
        QTRY_COMPARE(list->model()->rowCount(), 3);
      }
      for (int i = 0; i < names.size(); ++i) {
        auto *button = bench.window->findChild<QToolButton *>(names[i]);
        QVERIFY2(button, qPrintable(names[i]));
        QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
        QCOMPARE(button->text(), keys[i].toString(QKeySequence::NativeText));
        QVERIFY(!button->accessibleName().isEmpty());
      }
      if (!captures.isEmpty()) {
        QTest::qWait(30);
        QVERIFY(bench.window->grab().save(captures + '/' + capture +
                                          (dark ? "-dark.png" : "-light.png")));
      }
    };
    inspect(ActionId::ViewLocalTypes,
            {"localTypesAdd", "localTypesEdit", "localTypesDelete"},
            {LocalTypesView::commandKey(TypesCommand::Add),
             LocalTypesView::commandKey(TypesCommand::Edit),
             LocalTypesView::commandKey(TypesCommand::Delete)},
            "local-types");
    auto *back = bench.window->findChild<QToolButton *>("localTypesBack");
    QVERIFY(back);
    QCOMPARE(back->toolButtonStyle(), Qt::ToolButtonIconOnly);
    QVERIFY(!back->accessibleName().isEmpty());
    inspect(ActionId::ViewTypeLibraries,
            {"typeLibrariesAdd", "typeLibrariesRemove"},
            {QKeySequence(Qt::Key_Insert), QKeySequence(QKeySequence::Delete)},
            "type-libraries");
    inspect(ActionId::ViewPathfinder,
            {"pathfinderAdd", "pathfinderExclude", "pathfinderRemove",
             "pathfinderUp", "pathfinderDown"},
            {QKeySequence(Qt::Key_Insert), QKeySequence(Qt::Key_Insert),
             QKeySequence(QKeySequence::Delete),
             QKeySequence(Qt::ALT | Qt::Key_Up),
             QKeySequence(Qt::ALT | Qt::Key_Down)},
            "pathfinder");
    bench.window->disassembly()->focusContent();
    bench.action(ActionId::SearchInView)->trigger();
    auto *find = bench.window->findChild<FindInViewDialog *>();
    QVERIFY(find);
    for (const auto &[name, key] :
         {std::pair{"findInViewPrevious", QKeySequence(Qt::SHIFT | Qt::Key_F3)},
          std::pair{"findInViewNext", QKeySequence(Qt::Key_F3)}}) {
      auto *button = find->findChild<QPushButton *>(QLatin1String(name));
      QVERIFY(button);
      QVERIFY(button->text().contains(key.toString(QKeySequence::NativeText)));
    }
    if (!captures.isEmpty())
      QVERIFY(find->grab().save(captures +
                                (dark ? "/find-dark.png" : "/find-light.png")));
    auto *close = find->findChild<QPushButton *>("findInViewClose");
    QVERIFY(close);
    QCOMPARE(close->text(), QStringLiteral("Close"));
    QTest::keyClick(close, Qt::Key_Escape);
    QVERIFY(!find->isVisible());
    DeclareTypesDialog declarations(
        QStringLiteral("c"), QStringLiteral("struct Example { int Value; };"),
        {}, bench.window.get());
    declarations.show();
    auto *declare = declarations.findChild<QPushButton *>("declare");
    QVERIFY(declare);
    QVERIFY(declare->text().contains(QKeySequence(Qt::CTRL | Qt::Key_Return)
                                         .toString(QKeySequence::NativeText)));
    if (!captures.isEmpty())
      QVERIFY(declarations.grab().save(
          captures + (dark ? "/declare-dark.png" : "/declare-light.png")));
  }

  void viewSearchLiteralMatchingHasSharedCaseAndWordRules() {
    ViewSearchQuery query{QStringLiteral("v7")};
    QCOMPARE(query.matches(u"v7 + v7 + v70 + V7"),
             QVector<int>({0, 5, 10, 16}));
    query.WholeWords = true;
    QCOMPARE(query.matches(u"v7 + v7 + v70 + V7"), QVector<int>({0, 5, 16}));
    query.Case = Qt::CaseSensitive;
    QCOMPARE(query.matches(u"v7 + v7 + v70 + V7"), QVector<int>({0, 5}));
    query.Text = QStringLiteral("if (");
    query.WholeWords = false;
    QCOMPARE(query.matches(u"if (a) { if (b) {} }"), QVector<int>({0, 9}));
    query.Text.clear();
    QVERIFY(query.matches(u"anything").isEmpty());
    query.Text = QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87");
    QCOMPARE(query.matches(u"// \u4e2d\u6587 + \u4e2d\u6587"),
             QVector<int>({3, 8}));
  }

  void viewSearchWholeWordsRecognizesSupplementaryCharacters() {
    const ViewSearchQuery query{QStringLiteral("v7"), Qt::CaseSensitive, true};
    // Supplementary letters and combining marks remain part of a word on
    // either side of a match. Emoji still delimit words.
    QCOMPARE(query.matches(u"\U00010400v7 v7\U00010400 v7"),
             QVector<int>({10}));
    QCOMPARE(query.matches(u"\U0001D165v7 v7\U0001D165 v7"),
             QVector<int>({10}));
    QCOMPARE(query.matches(u"\U0001F600v7 v7\U0001F600 v7"),
             QVector<int>({2, 5, 10}));
  }

  void findInViewSearchesPseudocode_data() {
    QTest::addColumn<QString>("representation");
    QTest::newRow("pseudocode") << QStringLiteral("source");
    QTest::newRow("llvm-c") << QStringLiteral("llvmc");
  }

  void findInViewSearchesPseudocode() {
    QFETCH(QString, representation);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "local-references.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench
        .action(representation == "llvmc" ? ActionId::ViewLLVMC
                                          : ActionId::ViewPseudocode)
        ->trigger();
    auto *view = bench.codeView(representation);
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(code->complete());
    const QString original = code->allText();
    const int first = original.indexOf("return v7 + v7") + 7;
    QVERIFY(first > 7);
    code->setCursorLine(code->lineCount() - 2);
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const auto address = assembly->currentAddress();
    const auto version = code->documentVersion();
    QSignalSpy locations(code, &CodeText::locationChanged);
    // Exercise the real shortcut and origin capture, not a direct search call.
    QTest::keyClick(code, Qt::Key_F, Qt::ControlModifier);
    auto *dialog = bench.window->findChild<FindInViewDialog *>();
    QVERIFY(dialog);
    auto *input = dialog->findChild<QLineEdit *>("findInViewText");
    auto *status = dialog->findChild<QLabel *>("findInViewStatus");
    QTRY_VERIFY(input->hasFocus());
    input->setText("v7");
    QTRY_COMPARE(code->searchSelection().first, first);
    QCOMPARE(code->selectedText(), QStringLiteral("v7"));
    const auto firstStatus = status->text();
    QVERIFY(firstStatus.contains(" of "));
    // Two matches on the same current row, then wrap around to the prelude.
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(code->searchSelection().first, first + 5);
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(code->searchSelection().first, int(original.indexOf("v7")));
    QTest::keyClick(input, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(code->searchSelection().first, first + 5);
    QTest::keyClick(input, Qt::Key_F3, Qt::ShiftModifier);
    QCOMPARE(code->searchSelection().first, first);
    QTest::keyClick(input, Qt::Key_F3);
    QCOMPARE(code->searchSelection().first, first + 5);
    // The same keys work after returning focus to the originating pane,
    // overriding the workbench's Shift+F3 chooser shortcut only during Find.
    bench.window->activateWindow();
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    QTest::keyClick(code, Qt::Key_F3, Qt::ShiftModifier);
    QCOMPARE(code->searchSelection().first, first);
    QTest::keyClick(code, Qt::Key_F3);
    QCOMPARE(code->searchSelection().first, first + 5);
    dialog->activateWindow();
    input->setFocus();
    QTRY_VERIFY(input->hasFocus());
    QTest::keyClick(input, Qt::Key_F, Qt::ControlModifier);
    QCOMPARE(input->selectedText(), input->text());
    // Live Unicode matching searches comments too, and selects exact text.
    input->setText(QString::fromUtf8("\xe4\xb8\xad\xe6\x96\x87"));
    QTRY_COMPARE(status->text(), QStringLiteral("1 of 2"));
    QCOMPARE(code->selectedText(), input->text());
    input->clear();
    QTRY_COMPARE(status->text(), QStringLiteral("Enter text to find"));
    input->setText("not_present_in_this_function");
    QTRY_COMPARE(status->text(), QStringLiteral("No matches"));
    QCOMPARE(code->documentVersion(), version);
    QCOMPARE(assembly->currentAddress(), address);
    QVERIFY(locations.isEmpty());
    QPointer<FindInViewDialog> retired = dialog;
    QTest::keyClick(input, Qt::Key_Escape);
    QTRY_VERIFY(!retired);
    QVERIFY(!bench.window->findChild<FindInViewDialog *>());
  }

  void findInViewUnfoldsHiddenMatchesAndHonorsOptions() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "code-folding.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *code = bench.codeView("source")->text();
    QTRY_VERIFY(code->complete());
    code->setCursorLine(1);
    QVERIFY(code->setCurrentBlockFolded(true));
    QCOMPARE(code->lineCount(), 3);
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    bench.action(ActionId::SearchInView)->trigger();
    auto *dialog = bench.window->findChild<FindInViewDialog *>();
    QVERIFY(dialog);
    auto *input = dialog->findChild<QLineEdit *>("findInViewText");
    auto *status = dialog->findChild<QLabel *>("findInViewStatus");
    auto *caseSensitive = dialog->findChild<QCheckBox *>("findInViewCase");
    auto *words = dialog->findChild<QCheckBox *>("findInViewWords");
    auto *wrap = dialog->findChild<QCheckBox *>("findInViewWrap");
    input->setText("--v7");
    QTRY_COMPARE(status->text(), QStringLiteral("1 of 1"));
    QCOMPARE(code->lineCount(), 19);
    QCOMPARE(code->selectedText(), QStringLiteral("--v7"));
    QCOMPARE(code->commentTarget()->value("line").toInt(), 5);
    wrap->setChecked(false);
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(status->text(), QStringLiteral("End of search range"));
    QTest::keyClick(input, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(status->text(), QStringLiteral("Start of search range"));
    wrap->setChecked(true);
    input->setText("V7");
    QTRY_VERIFY(status->text().contains(" of "));
    caseSensitive->setChecked(true);
    QCOMPARE(status->text(), QStringLiteral("No matches"));
    input->setText("int32");
    QTRY_VERIFY(status->text().contains(" of "));
    words->setChecked(true);
    QCOMPARE(status->text(), QStringLiteral("No matches"));
    // Metacharacters are literals, not regular expressions.
    words->setChecked(false);
    input->setText("if (");
    QTRY_COMPARE(status->text(), QStringLiteral("1 of 1"));
    QCOMPARE(code->selectedText(), QStringLiteral("if ("));
    dialog->reject();
  }

  void findInViewFollowsOnlyItsOwnSourceDocument() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "tab-sync-paged.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView("source");
    auto *code = view->text();
    QTRY_VERIFY(code->complete());
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    bench.action(ActionId::SearchInView)->trigger();
    auto *dialog = bench.window->findChild<FindInViewDialog *>();
    QVERIFY(dialog);
    auto *input = dialog->findChild<QLineEdit *>("findInViewText");
    auto *status = dialog->findChild<QLabel *>("findInViewStatus");
    // The hit is on the third page, beyond the first loaded source batch.
    input->setText("code line 550");
    QTRY_COMPARE(status->text(), QStringLiteral("1 of 1"));
    QCOMPARE(code->selectedText(), input->text());
    const auto assemblyAddress = bench.window->disassembly()->currentAddress();
    // An independent pane loading a different function cannot retarget Find.
    CodeView other(bench.session, "source");
    other.show();
    other.showFunction(Base);
    QTRY_VERIFY(other.text()->complete());
    const auto otherSelection = other.text()->searchSelection();
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(other.text()->searchSelection(), otherSelection);
    QCOMPARE(code->function(), std::optional<Address>(Base + 0x140));
    // Retire the old complete snapshot immediately when this pane reloads.
    view->showFunction(Base + 0x180);
    QVERIFY(code->loading());
    QCOMPARE(status->text(), QStringLiteral("Waiting for source…"));
    QVERIFY(!dialog->findChild<QPushButton *>("findInViewNext")->isEnabled());
    input->setText("code line 551");
    QTRY_VERIFY(code->complete());
    QTRY_COMPARE(status->text(), QStringLiteral("1 of 1"));
    QCOMPARE(code->selectedText(), input->text());
    QCOMPARE(code->function(), std::optional<Address>(Base + 0x180));
    QCOMPARE(bench.window->disassembly()->currentAddress(), assemblyAddress);
    dialog->reject();
  }

  void findInViewSearchesLargeSourceWithoutReanalysis() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "pseudocode-large.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY(bench.window->disassembly()->currentFunction().has_value());
    bench.action(ActionId::ViewLLVMC)->trigger();
    auto *code = bench.codeView("llvmc")->text();
    // Loading is outside the search measurement; allow the complete fixture
    // to arrive even when other native tests are using the same machine.
    QTRY_VERIFY_WITH_TIMEOUT(code->complete(), NativeOpenTimeoutMs);
    QVERIFY(code->allText().contains("code line 19999"));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const auto version = code->documentVersion();
    bench.action(ActionId::SearchInView)->trigger();
    auto *dialog = bench.window->findChild<FindInViewDialog *>();
    QVERIFY(dialog);
    auto *input = dialog->findChild<QLineEdit *>("findInViewText");
    auto *status = dialog->findChild<QLabel *>("findInViewStatus");
    QElapsedTimer latency;
    latency.start();
    input->setText("code line");
    QTRY_VERIFY(status->text().endsWith(" of 20000"));
    qInfo() << "20,000-match search including input debounce ms:"
            << latency.elapsed();
    input->setText("code line 19999");
    // Enter flushes the debounce, selecting a new query's first hit once.
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(status->text(), QStringLiteral("1 of 1"));
    QCOMPARE(code->selectedText(), input->text());
    QCOMPARE(code->documentVersion(), version);
    dialog->reject();
  }

  void findInViewOnOptionalNativeInput() {
    const auto worker = qEnvironmentVariable("NEVERD_FIND_WORKER");
    const auto input = qEnvironmentVariable("NEVERD_FIND_INPUT");
    const auto name = qEnvironmentVariable("NEVERD_FIND_TARGET");
    if (worker.isEmpty() || input.isEmpty() || name.isEmpty())
      QSKIP("Set NEVERD_FIND_WORKER, NEVERD_FIND_INPUT and NEVERD_FIND_TARGET");
    QTemporaryDir directory;
    const auto path = directory.filePath(QFileInfo(input).fileName());
    QVERIFY(QFile::copy(input, path));
    const auto pdb = qEnvironmentVariable("NEVERD_FIND_PDB");
    if (!pdb.isEmpty())
      QVERIFY(QFile::copy(pdb, directory.filePath(QFileInfo(pdb).fileName())));
    Session session(worker);
    LoadOptions options;
    options.analysis = false;
    session.open(path, options);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), NativeOpenTimeoutMs);
    std::optional<Address> function;
    session.read("resolve", {{"query", name}}, this,
                 [&](const QJsonObject &reply) {
                   function = addressValue(reply.value("address"));
                 });
    QTRY_VERIFY_WITH_TIMEOUT(function.has_value(), NativeOpenTimeoutMs);
    CodeView view(session, "source");
    view.resize(1000, 800);
    view.show();
    view.showFunction(*function);
    auto *code = view.text();
    QTRY_VERIFY_WITH_TIMEOUT(code->complete(), NativeOpenTimeoutMs);
    const QString term =
        qEnvironmentVariable("NEVERD_FIND_TEXT", "__readgsqword");
    QVERIFY(code->allText().contains(term));
    const auto version = code->documentVersion();
    QSignalSpy locations(code, &CodeText::locationChanged);
    FindInViewDialog dialog(&view);
    dialog.setAttribute(Qt::WA_DeleteOnClose, false);
    dialog.search(code, nullptr);
    auto *edit = dialog.findChild<QLineEdit *>("findInViewText");
    auto *status = dialog.findChild<QLabel *>("findInViewStatus");
    QElapsedTimer latency;
    latency.start();
    edit->setText(term);
    QTRY_VERIFY(status->text().contains(" of "));
    qInfo() << "Native source search including input debounce ms:"
            << latency.elapsed();
    QCOMPARE(code->selectedText(), term);
    QTest::keyClick(edit, Qt::Key_F3);
    QCOMPARE(code->selectedText(), term);
    QCOMPARE(code->documentVersion(), version);
    QVERIFY(locations.isEmpty());
    const auto capture = qEnvironmentVariable("NEVERD_FIND_CAPTURE");
    if (!capture.isEmpty()) {
      QVERIFY(view.grab().save(capture + "/source.png"));
      QVERIFY(dialog.grab().save(capture + "/find-dialog.png"));
    }
    dialog.reject();
  }

  void findInViewSurvivesTargetDestruction_data() {
    QTest::addColumn<bool>("source");
    QTest::addColumn<bool>("pending");
    QTest::newRow("source-idle") << true << false;
    QTest::newRow("source-pending") << true << true;
    QTest::newRow("listing-idle") << false << false;
    QTest::newRow("listing-pending") << false << true;
  }

  void findInViewSurvivesTargetDestruction() {
    QFETCH(bool, source);
    QFETCH(bool, pending);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "code-folding.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    QWidget *target = assembly->listing();
    if (source) {
      bench.action(ActionId::ViewPseudocode)->trigger();
      auto *code = bench.codeView("source")->text();
      QTRY_VERIFY(code->complete());
      target = code;
    }
    target->setFocus();
    QTRY_VERIFY(target->hasFocus());
    bench.action(ActionId::SearchInView)->trigger();
    QPointer<FindInViewDialog> dialog =
        bench.window->findChild<FindInViewDialog *>();
    QVERIFY(dialog);
    if (pending)
      dialog->findChild<QLineEdit *>("findInViewText")->setText("return");
    // A pane can disappear without closing its modeless search first.
    // Exercise direct owner teardown, including a pending debounce callback.
    bench.window.reset();
    QVERIFY(!dialog);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
  }

  void findInViewSearchesAssemblyAddressesAndRetargets() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "code-folding.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    // Fix the document generation for the navigation checks below. A separate
    // refresh phase then exercises invalidation at an exact request boundary.
    QTRY_COMPARE_WITH_TIMEOUT(
        bench.session.background().value("state").toString(),
        QStringLiteral("ready"), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *code = bench.codeView("source")->text();
    QTRY_VERIFY(code->complete());
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    bench.action(ActionId::SearchInView)->trigger();
    auto *dialog = bench.window->findChild<FindInViewDialog *>();
    QVERIFY(dialog);
    auto *input = dialog->findChild<QLineEdit *>("findInViewText");
    input->setText("--v7");
    QTRY_COMPARE(code->selectedText(), QStringLiteral("--v7"));
    const auto sourceSelection = code->searchSelection();
    const auto version = code->documentVersion();
    bench.window->activateWindow();
    auto *listing = assembly->listing();
    listing->setFocus();
    QTRY_VERIFY(listing->hasFocus());
    QTRY_VERIFY(listing->searchReady());
    QTest::keyClick(listing, Qt::Key_F, Qt::ControlModifier);
    QCOMPARE(bench.window->findChildren<FindInViewDialog *>().size(), 1);
    QVERIFY(dialog->findChild<QLabel *>("findInViewScope")
                ->text()
                .startsWith("Disassembly:"));
    const QString text = listing->searchText();
    const int firstColon = text.indexOf(':');
    QVERIFY(firstColon >= 0);
    const QString address = text.mid(firstColon + 1, 16);
    QVERIFY(address.contains(QRegularExpression("^[0-9A-Fa-f]{16}$")));
    input->setText(address);
    auto *status = dialog->findChild<QLabel *>("findInViewStatus");
    QTRY_VERIFY(status->text().contains(" of "));
    QCOMPARE(listing->selectedText(), address);
    const auto first = listing->searchSelection();
    QTest::keyClick(input, Qt::Key_Return);
    QVERIFY(listing->searchSelection() != first || text.count(address) == 1);
    QCOMPARE(code->searchSelection(), sourceSelection);
    QCOMPARE(code->documentVersion(), version);
    listing->refresh();
    QVERIFY(!listing->searchReady());
    QCOMPARE(status->text(), QStringLiteral("No searchable text available"));
    QVERIFY(!dialog->findChild<QPushButton *>("findInViewNext")->isEnabled());
    QTRY_VERIFY(listing->searchReady());
    QTRY_VERIFY(
        dialog->findChild<QPushButton *>("findInViewNext")->isEnabled());
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(listing->selectedText(), address);
    QPointer<FindInViewDialog> retired = dialog;
    bench.action(ActionId::FileClose)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY(!retired);
  }

  void sourceExportWaitsForItsOwnCompleteDocument_data() {
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<bool>("failure");
    QTest::newRow("delayed-new-function") << "source-export.bin" << false;
    QTest::newRow("preparation-failed") << "source-export-fail.bin" << true;
    QTest::newRow("all-pages") << "source-cache-budget.bin" << false;
  }

  void sourceExportWaitsForItsOwnCompleteDocument() {
    QFETCH(QString, fixture);
    QFETCH(bool, failure);
    QTemporaryDir directory;
    const auto path = writeFixture(directory, fixture);
    const auto destination = directory.filePath(QStringLiteral("export.c"));
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(bench.window->disassembly()->currentFunction(),
                              std::optional<Address>(Base), OpenTimeoutMs);
    bench.action(ActionId::ViewPseudocode)->trigger();
    CodeView *code = nullptr;
    QTRY_VERIFY((code = bench.codeView(QStringLiteral("source"))));
    QTRY_VERIFY_WITH_TIMEOUT(code->text()->complete(), NativeOpenTimeoutMs);
    QVERIFY(code->text()->allText().contains(QStringLiteral("function_0")));
    bench.action(ActionId::ViewDisassembly)->trigger();
    auto *origin = bench.window->disassembly();
    origin->focusContent();
    origin->navigate(Base + 16);
    QTRY_COMPARE(origin->currentItem(), std::optional<Address>(Base + 16));
    // A failed export must preserve the existing destination.
    if (failure) {
      QFile existing(destination);
      QVERIFY(existing.open(QIODevice::WriteOnly));
      existing.write("previous export");
    }
    const bool native =
        QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const auto restore = qScopeGuard([native] {
      QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native);
    });
    bool accepted = false;
    QTimer files;
    connect(&files, &QTimer::timeout, &files, [&] {
      if (auto *dialog =
              qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
        accepted = true;
        dialog->setOption(QFileDialog::DontConfirmOverwrite);
        dialog->selectFile(destination);
        if (auto *name =
                dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
          name->setText(destination);
        static_cast<QDialog *>(dialog)->accept();
      } else if (auto *box = qobject_cast<QMessageBox *>(
                     QApplication::activeModalWidget()))
        box->button(QMessageBox::Yes)->click();
    });
    files.start(10);
    bench.action(ActionId::FileExportPseudocode)->trigger();
    files.stop();
    QVERIFY(accepted);
    // Moving the pane while the export runs must not change its target.
    origin->navigate(Base + 32);
    if (failure) {
      QTest::qWait(500);
      QCOMPARE(readAll(destination), QByteArray("previous export"));
    } else {
      QTRY_VERIFY_WITH_TIMEOUT(readAll(destination).contains("function_1"),
                               NativeOpenTimeoutMs);
      QVERIFY(!readAll(destination).contains("function_0"));
      QVERIFY(readAll(destination).contains("return value;"));
      if (fixture == QLatin1String("source-cache-budget.bin"))
        QVERIFY(readAll(destination).size() > 4 * 1024 * 1024);
    }
  }

  void searchResultsClearOnFailureCloseAndProjectSwitch() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("search-first.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const auto search = [&](const QString &pattern) {
      QTimer input;
      connect(&input, &QTimer::timeout, &input, [&] {
        if (auto *dialog = qobject_cast<QInputDialog *>(
                QApplication::activeModalWidget())) {
          dialog->setTextValue(pattern);
          dialog->accept();
        }
      });
      input.start(5);
      bench.action(ActionId::SearchText)->trigger();
    };
    const auto results = [&]() -> ChooserView * {
      for (auto *view : bench.window->findChildren<ChooserView *>())
        if (view->model().kind() == ChooserKind::SearchResults)
          return view;
      return nullptr;
    };
    search(QStringLiteral("string 42"));
    QTRY_VERIFY_WITH_TIMEOUT(results() && results()->model().total() > 0,
                             OpenTimeoutMs);
    search(QStringLiteral("does-not-exist"));
    QTRY_COMPARE(results()->model().total(), 0);
    search(QStringLiteral("string 7"));
    QTRY_VERIFY(results()->model().total() > 0);
    // Closing a dock can detach it from the main window's QObject tree.
    const QPointer<ChooserView> view = results();
    bench.session.closeFile();
    QTRY_VERIFY(!bench.session.loaded());
    QVERIFY(view);
    QVERIFY(!view->isVisible());
    QCOMPARE(view->model().total(), 0);
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("search-second.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(view);
    QVERIFY(!view->isVisible());
    QCOMPARE(view->model().total(), 0);
  }

  void searchDuringAJumpStartsWhereTheJumpLands() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("search-jump.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const auto results = [&]() -> ChooserView * {
      for (auto *view : bench.window->findChildren<ChooserView *>())
        if (view->model().kind() == ChooserKind::SearchResults)
          return view;
      return nullptr;
    };
    QTimer input;
    connect(&input, &QTimer::timeout, &input, [&] {
      if (auto *dialog =
              qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
        dialog->setTextValue(QStringLiteral("string 42"));
        dialog->accept();
      }
    });
    input.start(5);
    bench.action(ActionId::SearchText)->trigger();
    input.stop();
    QTRY_VERIFY_WITH_TIMEOUT(results() && results()->model().total() > 0,
                             OpenTimeoutMs);
    auto *listing = bench.window->disassembly()->listing();
    QTRY_VERIFY_WITH_TIMEOUT(!listing->jumpPending(), OpenTimeoutMs);
    // The listing answers this jump before the search: the search starts
    // where the jump lands, and the landing does not drop its reply.
    // Request the asynchronous path explicitly; current prefetched rows can
    // otherwise satisfy ordinary navigation without a pending jump.
    listing->jumpTo(Base + 3);
    QVERIFY(listing->jumpPending());
    bench.action(ActionId::SearchNextText)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(results()->model().total() > 0, OpenTimeoutMs);
    // From the old cursor at the first match, Next would skip that match.
    QCOMPARE(results()->model().addressAt(0),
             std::optional<Address>(Base + 42));
  }

  void sortingLongStringsKeepsTheTableWithinTheTransportBudget() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("strings-preview.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.action(ActionId::ViewStrings)->trigger();
    ChooserView *strings = nullptr;
    for (auto *view : bench.window->findChildren<ChooserView *>())
      if (view->model().kind() == ChooserKind::Strings)
        strings = view;
    QVERIFY(strings);
    strings->model().sort(1, Qt::DescendingOrder);
    QTRY_COMPARE_WITH_TIMEOUT(strings->model().total(), 4, NativeOpenTimeoutMs);
    QTRY_VERIFY(strings->model().addressAt(0).has_value());
    const auto first = strings->model().rowObject(0);
    QVERIFY(first.value("length").toInt() > 3 * 1024 * 1024);
    QVERIFY(first.value("text_truncated").toBool());
    QVERIFY(first.value("text").toString().size() <= 400);
    QCOMPARE(strings->model().addressAt(0), std::optional<Address>(Base + 3));
  }

  void newDisassemblyWindowKeepsIndependentCursorAndHistory() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("new-window.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *origin = bench.window->disassembly();
    origin->navigate(Base + 8);
    origin->focusContent();
    QTRY_COMPARE(origin->currentItem(), std::optional<Address>(Base + 8));
    QTRY_COMPARE(origin->operandTarget(), std::optional<Address>(Base));
    bench.window->activateWindow();
    QTest::keyClick(origin->listing(), Qt::Key_Return, Qt::AltModifier);
    QTRY_COMPARE(bench.window->findChildren<DisassemblyView *>().size(), 2);
    auto *second = bench.window->disassembly();
    QVERIFY(second != origin);
    QTRY_COMPARE(second->currentItem(), std::optional<Address>(Base));
    QCOMPARE(origin->currentItem(), std::optional<Address>(Base + 8));
    bench.action(ActionId::JumpNextFunction)->trigger();
    QTRY_COMPARE(second->currentItem(), std::optional<Address>(Base + 16));
    QCOMPARE(origin->currentItem(), std::optional<Address>(Base + 8));
    bench.action(ActionId::JumpBack)->trigger();
    QTRY_COMPARE(second->currentItem(), std::optional<Address>(Base));
    QCOMPARE(origin->currentItem(), std::optional<Address>(Base + 8));
    bench.session.closeFile();
    QTRY_VERIFY(!bench.session.loaded());
    QVERIFY(!origin->currentItem());
    QVERIFY(!second->currentItem());
  }

  void recoveredDatabaseStaysDirtyAcrossReopenUntilSaved() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("recovery.bin"));
    const auto database = directory.filePath(QStringLiteral("recovery.nddb"));
    {
      Workbench bench;
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      bench.session.setComment(Base, QStringLiteral("packed"));
      QTRY_VERIFY(bench.session.dirty());
      bench.session.save();
      QTRY_VERIFY(!bench.session.dirty());
      QSignalSpy saved(&bench.session, &Session::databaseSaved);
      bench.session.packDatabase(database);
      QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, OpenTimeoutMs);
      QVERIFY(saved.first().first().toBool());
    }
    QString error;
    const auto working = ProjectDatabase::unpack(
        database, ProjectDatabase::workingDirectory(database), &error);
    QVERIFY2(!working.isEmpty(), qPrintable(error));
    const auto historyPath =
        working + QStringLiteral(".neverd/local/history.json");
    const auto beforeHistory =
        QJsonDocument::fromJson(readAll(historyPath)).object();
    QVERIFY(!beforeHistory.isEmpty());
    const auto before = beforeHistory.value("state").toObject();
    auto after = before;
    const QJsonArray notes{
        QJsonObject{{"addr", hexAddress(Base)}, {"text", "recovered newest"}}};
    after["annotations"] = notes;
    auto afterHistory = beforeHistory;
    afterHistory["state"] = after;
    afterHistory["cursor"] = 1;
    afterHistory["commands"] =
        QJsonArray{QJsonObject{{"kind", "annotation"},
                               {"address", hexAddress(Base)},
                               {"before", "packed"},
                               {"after", "recovered newest"}}};
    const QJsonObject journal{
        {"schema_version", 1},
        {"source_sha256", beforeHistory.value("source_sha256")},
        {"before", before},
        {"after", after},
        {"before_history", beforeHistory},
        {"after_history", afterHistory}};
    QFile pending(working + QStringLiteral(".neverd/local/journal.json"));
    QVERIFY(pending.open(QIODevice::WriteOnly));
    pending.write(QJsonDocument(journal).toJson());
    pending.close();
    {
      Workbench bench;
      bench.window->openFile(database);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      QCOMPARE(commentAt(bench, Base), QStringLiteral("recovered newest"));
      QVERIFY(bench.session.databaseOutOfDate());
      QVERIFY(bench.session.dirty());
      // Simulate a second interruption after the worker consumed its journal.
      QVERIFY(!QFileInfo::exists(pending.fileName()));
    }
    Workbench bench;
    bench.window->openFile(database);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(commentAt(bench, Base), QStringLiteral("recovered newest"));
    QVERIFY(bench.session.databaseOutOfDate());
    QString question;
    QTimer answer;
    connect(&answer, &QTimer::timeout, &answer, [&] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        question = box->text();
        box->button(QMessageBox::Cancel)->click();
      }
    });
    answer.start(5);
    bench.session.closeFile();
    QTRY_VERIFY(!question.isEmpty());
    QVERIFY(bench.session.loaded());
    answer.stop();
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.dirty(), OpenTimeoutMs);
    bench.session.closeFile();
    QTRY_VERIFY(!bench.session.loaded());
    bench.window->openFile(database);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!bench.session.databaseOutOfDate());
    QCOMPARE(commentAt(bench, Base), QStringLiteral("recovered newest"));
  }

  void nestedPseudocodeBlocksFoldWithoutMovingAssembly_data() {
    QTest::addColumn<QString>("representation");
    QTest::newRow("pseudocode") << QStringLiteral("source");
    QTest::newRow("explicit-c") << QStringLiteral("c");
    QTest::newRow("llvm-c") << QStringLiteral("llvmc");
  }

  void nestedPseudocodeBlocksFoldWithoutMovingAssembly() {
    QFETCH(QString, representation);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    // An explicit C view is offered beside another language only.
    bench.window->openFile(
        writeFixture(directory, representation == QLatin1String("c")
                                    ? QStringLiteral("code-folding-cpp.bin")
                                    : QStringLiteral("code-folding.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench
        .action(representation == QLatin1String("llvmc")
                    ? ActionId::ViewLLVMC
                    : ActionId::ViewPseudocode)
        ->trigger();
    auto *view = bench.codeView(representation == QLatin1String("llvmc")
                                    ? QStringLiteral("llvmc")
                                    : QStringLiteral("source"));
    QVERIFY(view);
    view->setRepresentation(representation);
    QCOMPARE(view->representation(), representation);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() == 19);
    const QString original = code->allText();
    QVERIFY(activateTestWindow(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    assembly->navigate(Base + 0x180);
    QTRY_COMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x180));

    // The while arrow uses the fixed line-number gutter, without selecting
    // code or changing assembly. Its parent retains the child's fold state.
    code->setCursorLine(4);
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const int height = int(std::ceil(metrics.lineSpacing()));
    const QPoint arrow(int(6 + 4.25 * metrics.horizontalAdvance('M')),
                       (4 - code->verticalScrollBar()->value()) * height +
                           height / 2);
    QSignalSpy locations(code, &CodeText::locationChanged);
    QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, arrow);
    QCOMPARE(code->lineCount(), 18);
    QVERIFY(locations.isEmpty());
    // The header stays visible: finding its text or clicking past the end of
    // its row neither expands the block nor offers a fold control there.
    code->setCursorLine(1);
    QVERIFY(code->findText(QStringLiteral("while (v7)"), true));
    QCOMPARE(code->lineCount(), 18);
    const QPoint pastHeader(code->viewport()->width() - 4, arrow.y());
    QTest::mouseMove(code->viewport(), arrow);
    QTRY_COMPARE(code->viewport()->cursor().shape(), Qt::PointingHandCursor);
    QTest::mouseMove(code->viewport(), pastHeader);
    QTRY_COMPARE(code->viewport()->cursor().shape(), Qt::IBeamCursor);
    QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier,
                      pastHeader);
    QCOMPARE(code->lineCount(), 18);
    code->setCursorLine(3);
    QTest::keyClick(code, Qt::Key_BracketLeft,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(code->lineCount(), 16);
    QTest::keyClick(code, Qt::Key_BracketRight,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(code->lineCount(), 18);
    QCOMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x180));

    // Folding restored the cursor to the if header; search starts below it.
    code->setCursorLine(1);
    QVERIFY(code->findText(QStringLiteral("v7 = v0"), true));
    const auto target = code->renameTarget();
    QVERIFY(target);
    const auto references = code->localReferences(*target);
    const auto hidden =
        std::find_if(references.cbegin(), references.cend(),
                     [](const auto &ref) { return ref.line == 5; });
    QVERIFY(hidden != references.cend());
    code->setCursorLine(1);
    QVERIFY(code->setCurrentBlockFolded(true));
    QCOMPARE(code->lineCount(), 3);
    QVERIFY(
        !code->currentAddress()); // The body cannot supply a header address.
    QTest::keyClick(code, Qt::Key_Home, Qt::ControlModifier);
    QTest::keyClick(code, Qt::Key_End, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(code->selectedText(), original);
    QCOMPARE(code->allText(), original);
    code->setCursorLine(1);
    QVERIFY(code->findText(QStringLiteral("--v7"), true));
    QCOMPARE(code->lineCount(), 19);
    QVERIFY(code->commentTarget());
    QCOMPARE(code->commentTarget()->value("line").toInt(), 5);
    code->setCursorLine(1);
    QVERIFY(code->setCurrentBlockFolded(true));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    QVERIFY(code->selectLocalReference(*target, *hidden));
    QCOMPARE(code->lineCount(), 19);
    QCOMPARE(code->selectedText(), QStringLiteral("v7"));
    QVERIFY(code->commentTarget());
    QCOMPARE(code->commentTarget()->value("line").toInt(), 5);
    // Selecting a mapped reference moves the source cursor and its
    // synchronized assembly, independently of unfolding the reference.
    QCOMPARE(code->currentAddress(), std::optional<Address>(Base + 0x142));
    QTRY_COMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x142));

    // A refresh preserves folds, and explicit instruction navigation expands
    // all blocks hiding its mapped original row.
    code->setCursorLine(3);
    QVERIFY(code->setCurrentBlockFolded(true));
    code->load(Base + 0x140, representation);
    QTRY_VERIFY(!code->loading() && code->lineCount() == 16);
    code->selectAddress(Base + 0x142);
    QCOMPARE(code->lineCount(), 19);
    QCOMPARE(code->currentAddress(), std::optional<Address>(Base + 0x142));
    QVERIFY(code->commentTarget());
    QCOMPARE(code->commentTarget()->value("line").toInt(), 5);
  }

  void localVariableReferencesStayInTheirCodeWindow_data() {
    QTest::addColumn<QString>("representation");
    QTest::addColumn<bool>("renamed");
    QTest::addColumn<bool>("floating");
    QTest::addColumn<QString>("finish");
    for (const bool floating : {false, true}) {
      const auto suffix = floating ? "-floating" : "-docked";
      QTest::newRow(qPrintable(QStringLiteral("pseudocode") + suffix))
          << QStringLiteral("source") << false << floating
          << QStringLiteral("ok");
      QTest::newRow(qPrintable(QStringLiteral("explicit-c") + suffix))
          << QStringLiteral("c") << false << floating
          << QStringLiteral("enter");
      QTest::newRow(qPrintable(QStringLiteral("llvm-c") + suffix))
          << QStringLiteral("llvmc") << false << floating
          << QStringLiteral("double");
      QTest::newRow(qPrintable(QStringLiteral("renamed-local") + suffix))
          << QStringLiteral("source") << true << floating
          << QStringLiteral("escape");
    }
  }

  void localVariableReferencesStayInTheirCodeWindow() {
    QFETCH(QString, representation);
    QFETCH(bool, renamed);
    QFETCH(bool, floating);
    QFETCH(QString, finish);
    Theme::instance().apply();
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, representation == QLatin1String("c")
                                    ? QStringLiteral("local-references-cpp.bin")
                                    : QStringLiteral("local-references.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench
        .action(representation == QLatin1String("llvmc")
                    ? ActionId::ViewLLVMC
                    : ActionId::ViewPseudocode)
        ->trigger();
    auto *view = bench.codeView(representation == QLatin1String("llvmc")
                                    ? QStringLiteral("llvmc")
                                    : QStringLiteral("source"));
    QVERIFY(view);
    view->setRepresentation(representation);
    QCOMPARE(view->representation(), representation);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->allText().contains("return v7 + v7"));
    if (floating) {
      auto *dock = bench.dockFor(view);
      QVERIFY(dock);
      dock->setFloating(true);
      QTRY_VERIFY(dock->isFloating());
    }
    view->window()->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(view->window()));
    for (auto *button : view->findChildren<QToolButton *>())
      if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
        button->setChecked(true);
    QVERIFY(view->locked());
    assembly->navigate(Base + 0x180);
    QTRY_COMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x180));
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x180));
    QVERIFY(code->findText(QStringLiteral("v7 = 0"), true));
    if (renamed) {
      const auto target = code->renameTarget();
      QVERIFY(target);
      auto edit = *target;
      edit["address"] = hexAddress(Base + 0x140);
      edit["representation"] = representation;
      edit["kind"] = "name";
      edit["name"] = "counter";
      bench.session.editCode(edit, bench.session.epoch());
      QTRY_VERIFY(!code->loading() &&
                  code->allText().contains("return counter + counter"));
    }
    const auto name =
        renamed ? QStringLiteral("counter") : QStringLiteral("v7");
    // Search continues below the cursor, which is on the earlier match.
    code->setCursorLine(0);
    QVERIFY(code->findText(name + QStringLiteral(" = 0"), true));
    const auto target = code->renameTarget();
    QVERIFY(target);
    QCOMPARE(target->value("kind").toString(), QStringLiteral("local"));
    const auto references = code->localReferences(*target);
    // The complete function spans three pages. Its similarly named v70,
    // Box::v7 member, strings and comments are not this local's occurrences.
    QCOMPARE(references.size(), 5);
    QCOMPARE(references[0].line, 3);
    QCOMPARE(references[1].line, 9);
    QVERIFY(references[1].text.contains(QStringLiteral("中文")));
    QCOMPARE(references[2].line, 550);
    QCOMPARE(references[2].column, 2);
    QCOMPARE(references[3].line, 700);
    QCOMPARE(references[4].line, 700);
    QVERIFY(references[3].column < references[4].column);
    code->setFolded(true);
    QVERIFY(code->libraryFolded());
    code->setCursorLine(0);
    QVERIFY(code->findText(name + QStringLiteral(" = 0"), true));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const auto dialog = openLocalReferences(bench, view);
    QVERIFY(dialog);
    QVERIFY(dialog->windowTitle().contains(name));
    QCOMPARE(dialog->parentWidget(), view);
    QCOMPARE(dialog->windowModality(), Qt::NonModal);
    QVERIFY(!QApplication::activeModalWidget());
    QVERIFY(QTest::qWaitForWindowActive(dialog));
    auto *table =
        dialog->findChild<QTableView *>(QStringLiteral("localReferencesTable"));
    QVERIFY(table);
    QTRY_VERIFY(table->hasFocus());
    QVERIFY(dialog->windowHandle());
    QCOMPARE(dialog->windowHandle()->transientParent(),
             view->window()->windowHandle());
    QSignalSpy finished(dialog, &QDialog::finished);
    const auto *model = table->model();
    QCOMPARE(model->rowCount(), 5);
    QCOMPARE(model->index(2, 0).data().toInt(), 551);
    QCOMPARE(model->index(2, 1).data().toInt(), 3);
    // Clicking the initially selected row must also reveal its exact token.
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                      table->visualRect(model->index(0, 0)).center());
    QCOMPARE(code->selectedText(), name);
    QCOMPARE(code->searchSelection(),
             std::pair(references[0].begin, references[0].end));
    QVERIFY(table->hasFocus());
    QVERIFY(dialog->isVisible());

    // A single click unfolds the distant use immediately, without closing
    // the list or moving the independent assembly cursor.
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                      table->visualRect(model->index(2, 2)).center());
    QCOMPARE(code->searchSelection(),
             std::pair(references[2].begin, references[2].end));
    QVERIFY(!code->libraryFolded());
    QVERIFY(table->hasFocus());
    QVERIFY(dialog->isVisible());
    QCOMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x180));

    const auto captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
    if (!captures.isEmpty() && representation == QLatin1String("source") &&
        !renamed) {
      auto &theme = Theme::instance();
      const auto mode = theme.mode();
      const auto restore = qScopeGuard([&] { theme.setMode(mode); });
      QDir().mkpath(captures);
      for (const bool dark : {false, true}) {
        theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
        theme.apply();
        QApplication::processEvents();
        const QString suffix = QString(floating ? "-floating" : "-docked") +
                               (dark ? "-dark.png" : "-light.png");
        QVERIFY(view->grab().save(captures + "/local-xrefs-code" + suffix));
        QVERIFY(dialog->grab().save(captures + "/local-xrefs-list" + suffix));
      }
    }

    // Two uses on the same line have distinct columns; keyboard selection
    // must preview each occurrence, even when its source line is unchanged.
    QTest::keyClick(table, Qt::Key_Down);
    QCOMPARE(code->searchSelection(),
             std::pair(references[3].begin, references[3].end));
    QTest::keyClick(table, Qt::Key_Down);
    QCOMPARE(code->searchSelection(),
             std::pair(references[4].begin, references[4].end));
    QTest::keyClick(table, Qt::Key_Up);
    QCOMPARE(code->searchSelection(),
             std::pair(references[3].begin, references[3].end));
    QTest::keyClick(table, Qt::Key_Up);
    QCOMPARE(code->searchSelection(),
             std::pair(references[2].begin, references[2].end));
    QVERIFY(table->hasFocus());
    if (finish == QLatin1String("ok")) {
      auto *buttons = dialog->findChild<QDialogButtonBox *>();
      QVERIFY(buttons);
      QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    } else if (finish == QLatin1String("enter")) {
      QTest::keyClick(table, Qt::Key_Return);
    } else if (finish == QLatin1String("double")) {
      // QTest's double-click sends only the second press. Supply the first
      // click too, as a native double-click does after keyboard navigation.
      const auto point = table->visualRect(model->index(2, 0)).center();
      QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                        point);
      QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                         point);
    } else {
      QTest::keyClick(table, Qt::Key_Escape);
    }
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(finished.first().first().toInt(), finish == QLatin1String("escape")
                                                   ? int(QDialog::Rejected)
                                                   : int(QDialog::Accepted));
    QTRY_VERIFY(!dialog);
    QCOMPARE(code->selectedText(), name);
    QCOMPARE(code->currentToken(), name);
    QCOMPARE(code->function(), std::optional<Address>(Base + 0x140));
    QCOMPARE(view->representation(), representation);
    QVERIFY(view->locked());
    QVERIFY(!code->libraryFolded());
    QVERIFY(code->preludeFolded());
    const auto row = code->commentTarget();
    QVERIFY(row);
    QCOMPARE(row->value("line").toInt(), 550);
    QCOMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x180));
  }

  void localReferencesAllowWorkbenchInteraction_data() {
    QTest::addColumn<bool>("floating");
    QTest::newRow("docked") << false;
    QTest::newRow("floating") << true;
    // Each data row constructs fresh native owners. Repeated ownership
    // transitions must preserve the browser and its keyboard focus.
    for (unsigned Repeat = 2; Repeat <= 5; ++Repeat) {
      QTest::newRow(qPrintable(QStringLiteral("docked-repeat-%1").arg(Repeat)))
          << false;
      QTest::newRow(
          qPrintable(QStringLiteral("floating-repeat-%1").arg(Repeat)))
          << true;
    }
  }

  void localReferencesAllowWorkbenchInteraction() {
    QFETCH(bool, floating);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-references.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(code->complete());
    auto *dock = bench.dockFor(view);
    auto *assemblyDock = bench.dockFor(assembly);
    QVERIFY(dock && assemblyDock);
    if (floating)
      dock->setFloating(true);
    for (auto *button : view->findChildren<QToolButton *>())
      if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
        button->setChecked(true);
    QVERIFY(view->locked());
    QVERIFY(activateTestWindow(view->window()));
    QVERIFY(code->findText(QStringLiteral("v7 = 0"), true));
    const auto target = code->renameTarget();
    QVERIFY(target);
    const auto references = code->localReferences(*target);
    QCOMPARE(references.size(), 5);
    const auto version = code->documentVersion();
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const auto dialog = openLocalReferences(bench, view);
    QVERIFY(dialog);
    auto *table =
        dialog->findChild<QTableView *>(QStringLiteral("localReferencesTable"));
    QVERIFY(table);
    QVERIFY(activateTestWindow(dialog));
    QTRY_VERIFY(table->hasFocus());
    QCOMPARE(dialog->windowModality(), Qt::NonModal);
    QVERIFY(!QApplication::activeModalWidget());

    // Real input must reach the source and another analysis pane while the
    // reference list remains open, including controller-level navigation.
    QVERIFY(activateTestWindow(view->window()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const auto line = code->cursorLine();
    QTest::keyClick(code, Qt::Key_Down);
    QCOMPARE(code->cursorLine(), line + 1);
    const auto top = code->verticalScrollBar()->value();
    QTest::keyClick(code, Qt::Key_PageDown);
    QVERIFY(code->verticalScrollBar()->value() > top);
    QVERIFY(dialog->isVisible());

    QVERIFY(activateTestWindow(bench.window.get()));
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    auto *functions = bench.functions();
    QVERIFY(functions);
    QTRY_COMPARE(functions->model().addressAt(24),
                 std::optional<Address>(Base + 0x180));
    functions->table()->setCurrentIndex(functions->model().index(24, 0));
    functions->table()->setFocus();
    QTRY_VERIFY(functions->table()->hasFocus());
    QTest::keyClick(functions->table(), Qt::Key_Return);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x180));
    QCOMPARE(code->function(), std::optional<Address>(Base + 0x140));
    QCOMPARE(code->documentVersion(), version);
    QVERIFY(activateTestWindow(dialog));
    table->setFocus();
    QTRY_VERIFY(table->hasFocus());
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                      table->visualRect(table->model()->index(1, 0)).center());
    QCOMPARE(code->searchSelection(),
             std::pair(references[1].begin, references[1].end));

    // Floating and docking replace the native owner, not the source or list.
    // The actual dock button must still receive its click behind the list.
    dock->setFloating(true);
    QTRY_VERIFY(dock->isFloating());
    QVERIFY(dialog && dialog->isVisible());
    QTRY_COMPARE(dialog->windowHandle()->transientParent(),
                 view->window()->windowHandle());
    QVERIFY(activateTestWindow(view->window()));
    QAbstractButton *dockButton = nullptr;
    for (auto *bar :
         view->window()->findChildren<KDDockWidgets::QtWidgets::TitleBar *>())
      if (bar->isVisible())
        for (auto *button : bar->findChildren<QAbstractButton *>())
          if (button->isVisible() &&
              button->toolTip() == QStringLiteral("Dock window"))
            dockButton = button;
    QVERIFY(dockButton);
    QTest::mouseClick(dockButton, Qt::LeftButton);
    QTRY_VERIFY(!dock->isFloating());
    QCOMPARE(view->window(), bench.window.get());
    QVERIFY(dialog && dialog->isVisible());
    QTRY_COMPARE(dialog->windowHandle()->transientParent(),
                 bench.window->windowHandle());
    QCOMPARE(code->documentVersion(), version);

    // Switching tabs also leaves the document intact. A reference click
    // reveals its source tab without taking keyboard focus from the list.
    assemblyDock->addDockWidgetAsTab(dock);
    assemblyDock->setAsCurrentTab();
    QTRY_VERIFY(!view->isVisible());
    QVERIFY(dialog && dialog->isVisible());
    QVERIFY(activateTestWindow(dialog));
    table->setFocus();
    QTRY_VERIFY(table->hasFocus());
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                      table->visualRect(table->model()->index(4, 0)).center());
    QTRY_VERIFY(view->isVisible());
    QCOMPARE(code->searchSelection(),
             std::pair(references[4].begin, references[4].end));
    QCOMPARE(code->documentVersion(), version);
    QVERIFY(table->hasFocus());
    QVERIFY(dialog->isVisible());
    QTest::keyClick(table, Qt::Key_Escape);
    QTRY_VERIFY(!dialog);
  }

  void changedSourceDismissesLocalReferences_data() {
    QTest::addColumn<QString>("change");
    QTest::addColumn<bool>("floating");
    for (const bool floating : {false, true})
      for (const auto *change : {"reload", "function", "representation",
                                 "close", "replace", "window"})
        QTest::newRow(qPrintable(QString::fromLatin1(change) +
                                 (floating ? "-floating" : "-docked")))
            << QString::fromLatin1(change) << floating;
  }

  void changedSourceDismissesLocalReferences() {
    QFETCH(QString, change);
    QFETCH(bool, floating);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-references.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(code->complete());
    auto *dock = bench.dockFor(view);
    QVERIFY(dock);
    if (floating)
      dock->setFloating(true);
    view->window()->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(view->window()));
    QVERIFY(code->findText(QStringLiteral("v7 = 0"), true));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const auto dialog = openLocalReferences(bench, view);
    QVERIFY(dialog);
    if (change == QLatin1String("reload"))
      view->showFunction(Base + 0x140);
    else if (change == QLatin1String("function"))
      view->showFunction(Base + 0x160);
    else if (change == QLatin1String("representation"))
      view->setRepresentation(QStringLiteral("llvmc"));
    else if (change == QLatin1String("close"))
      dock->forceClose();
    else if (change == QLatin1String("window"))
      bench.window.reset();
    else {
      view->window()->activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(view->window()));
      code->setFocus();
      QTRY_VERIFY(code->hasFocus());
      const auto replacement = openLocalReferences(bench, view);
      QVERIFY(replacement && replacement != dialog);
      QTRY_VERIFY(!dialog);
      QVERIFY(replacement->isVisible());
      QCOMPARE(
          view->findChildren<QDialog *>(QStringLiteral("localReferencesDialog"))
              .size(),
          1);
      replacement->reject();
      QTRY_VERIFY(!replacement);
    }
    QTRY_VERIFY(!dialog);
  }

  void closingFileDismissesLocalVariableReferences() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-references.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->allText().contains("v7 = 0"));
    QVERIFY(code->findText(QStringLiteral("v7 = 0"), true));
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const auto dialog = openLocalReferences(bench, view);
    QVERIFY(dialog);
    auto *table =
        dialog->findChild<QTableView *>(QStringLiteral("localReferencesTable"));
    QVERIFY(table);
    table->setCurrentIndex(table->model()->index(2, 0));
    QVERIFY(code->commentTarget());
    QCOMPARE(code->commentTarget()->value("line").toInt(), 550);
    bench.session.closeFile();
    QTRY_VERIFY(!dialog);
    QVERIFY(!bench.session.loaded());
    QVERIFY(!code->function());
    QVERIFY(code->selectedText().isEmpty());
  }

  void xrefsReturnToTheOriginatingCodeWindow_data() {
    QTest::addColumn<QString>("scenario");
    for (const auto *scenario :
         {"source", "llvmc", "low", "secondary-source", "published-address",
          "call-token", "unmapped", "assembly"})
      QTest::newRow(scenario) << QString::fromLatin1(scenario);
  }

  void xrefsReturnToTheOriginatingCodeWindow() {
    QFETCH(QString, scenario);
    QTemporaryDir directory;
    Workbench bench;
    const auto path = writeFixture(
        directory, scenario == "unmapped"            ? "xrefs-unmapped.bin"
                   : scenario == "published-address" ? "xrefs-address.bin"
                                                     : "xrefs-code.bin");
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *primary = bench.codeView(QStringLiteral("source"));
    QVERIFY(primary);
    bench.action(ActionId::ViewLowIR)->trigger();
    auto *secondary = bench.codeView(QStringLiteral("low"));
    QVERIFY(secondary);
    const bool useSecondary = scenario == "llvmc" || scenario == "low" ||
                              scenario == "secondary-source";
    auto *origin = useSecondary ? secondary : primary;
    auto *other = useSecondary ? primary : secondary;
    auto *originDock = bench.dockFor(origin);
    auto *otherDock = bench.dockFor(other);
    QVERIFY(originDock && otherDock);
    // Code documents share a tab group. Select the tab before sending input
    // to its viewport, just as a user must do in the workbench.
    originDock->setAsCurrentTab();
    QTRY_VERIFY(origin->isVisible());
    const QString representation = scenario == "llvmc" || scenario == "low"
                                       ? scenario
                                       : QStringLiteral("source");
    origin->setRepresentation(representation);
    origin->showFunction(scenario == "call-token" ? Base + 0x160
                                                  : Base + 0x140);
    other->showFunction(Base + 0x180);
    QTRY_VERIFY(!origin->text()->loading() && !other->text()->loading());
    for (auto *button : origin->findChildren<QToolButton *>())
      if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
        button->setChecked(true);
    QVERIFY(origin->locked());
    // Assembly has an independent location when the source query starts.
    assembly->navigate(Base + 0x1a0);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x1a0));
    originDock->setAsCurrentTab();
    QVERIFY(activateTestWindow(origin->window()));
    auto *code = origin->text();
    const int row = scenario == "call-token" ? 1 : 0;
    code->setCursorLine(row);
    const QString line = code->selectedText();
    const QString token = scenario == "published-address"
                              ? QStringLiteral("display_function")
                              : QStringLiteral("function_20");
    const int column = int(line.indexOf(token));
    QVERIFY(column >= 0);
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const QPoint point(qRound(6 + (5 + column + 2) * metrics.horizontalAdvance(
                                                         QLatin1Char('M'))),
                       qRound((row - code->verticalScrollBar()->value() + 0.5) *
                              std::ceil(metrics.lineSpacing())));
    QTest::mouseClick(code->viewport(), Qt::LeftButton, {}, point);
    QCOMPARE(code->currentToken(), token);
    QTRY_VERIFY(code->hasFocus());
    if (scenario == "published-address") {
      const auto target = code->renameTarget();
      QVERIFY(target);
      QCOMPARE(addressValue(target->value("address")),
               std::optional<Address>(Base + 0x140));
    }
    QSignalSpy mapped(code, &CodeText::addressSelected);
    bool chosen = false, correctTarget = false;
    QString dialogState;
    QTimer chooser;
    connect(&chooser, &QTimer::timeout, this, [&] {
      QPointer<QDialog> dialog =
          visibleDialog(QStringLiteral("referencesDialog"));
      auto *list = dialog ? dialog->findChild<ChooserView *>() : nullptr;
      if (!list || list->model().kind() != ChooserKind::CrossReferences ||
          !dialog->isActiveWindow() || !list->table()->hasFocus())
        return;
      dialogState = QStringLiteral("%1, %2 rows")
                        .arg(dialog->windowTitle())
                        .arg(list->model().total());
      for (auto *label : list->findChildren<QLabel *>())
        dialogState += QLatin1Char('\n') + label->text();
      correctTarget =
          dialog->windowTitle().contains(displayAddress(Base + 0x140, 16));
      for (int i = 0; i < list->model().total(); ++i)
        if (list->model().addressAt(i) == Base + 0x163) {
          chooser.stop();
          // Browsing elsewhere must not redirect acceptance to that pane.
          otherDock->setAsCurrentTab();
          QTRY_VERIFY(other->isVisible());
          QVERIFY(activateTestWindow(other->window()));
          other->text()->setFocus();
          QTRY_VERIFY(other->text()->hasFocus());
          QVERIFY(dialog->isVisible());
          QVERIFY(activateTestWindow(dialog));
          list->table()->setFocus();
          QTRY_VERIFY(list->table()->hasFocus());
          chosen = true;
          list->table()->setCurrentIndex(list->model().index(i, 0));
          QTest::keyClick(list->table(), Qt::Key_Return);
          return;
        }
    });
    chooser.start(5);
    QTimer timeout;
    connect(&timeout, &QTimer::timeout, this, [] {
      if (auto *dialog = visibleDialog(QStringLiteral("referencesDialog")))
        dialog->reject();
    });
    timeout.setSingleShot(true);
    timeout.start(OpenTimeoutMs);
    if (scenario == "assembly") {
      assembly->navigate(Base + 0x140);
      QTRY_COMPARE(assembly->currentFunction(),
                   std::optional<Address>(Base + 0x140));
      assembly->focusContent();
      QTRY_VERIFY(assembly->listing()->hasFocus());
      QTest::keyClick(assembly->listing(), Qt::Key_X);
    } else {
      QTest::keyClick(code, Qt::Key_X);
    }
    QTRY_VERIFY2_WITH_TIMEOUT(
        chosen,
        qPrintable(dialogState + QLatin1Char('\n') +
                   bench.window->findChild<OutputWindow *>()
                       ->findChild<QPlainTextEdit *>()
                       ->toPlainText()),
        OpenTimeoutMs);
    QVERIFY(correctTarget);
    if (scenario == "assembly") {
      QTRY_COMPARE(assembly->currentAddress(),
                   std::optional<Address>(Base + 0x163));
      QTRY_VERIFY(assembly->listing()->hasFocus());
      QCOMPARE(code->function(), std::optional<Address>(Base + 0x140));
      QVERIFY(mapped.isEmpty());
      return;
    }
    QTRY_COMPARE(code->function(), std::optional<Address>(Base + 0x160));
    QTRY_VERIFY(!code->loading() && !mapped.isEmpty());
    QCOMPARE(origin->representation(), representation);
    QVERIFY(origin->locked());
    QTRY_VERIFY(code->hasFocus());
    QCOMPARE(other->text()->function(), std::optional<Address>(Base + 0x180));
    if (scenario == "unmapped") {
      QVERIFY(!mapped.back().at(1).toBool());
      QVERIFY(!code->currentAddress());
      QVERIFY(bench.window->findChild<OutputWindow *>()
                  ->findChild<QPlainTextEdit *>()
                  ->toPlainText()
                  .contains(QStringLiteral(
                      "This instruction has no source-line address mapping.")));
    } else {
      QCOMPARE(code->currentAddress(), std::optional<Address>(Base + 0x163));
      QVERIFY(code->selectedText().contains(QStringLiteral("return ") + token));
    }
  }

  void changedSourceDismissesReferenceBrowsers_data() {
    QTest::addColumn<QString>("change");
    QTest::addColumn<bool>("floating");
    for (const bool floating : {false, true})
      for (const QString change :
           {QStringLiteral("reload"), QStringLiteral("function"),
            QStringLiteral("representation")})
        QTest::newRow(qPrintable(change + (floating ? "-floating" : "-docked")))
            << change << floating;
  }

  void changedSourceDismissesReferenceBrowsers() {
    QFETCH(QString, change);
    QFETCH(bool, floating);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "xrefs-code.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    QTRY_VERIFY(view->text()->complete());
    auto *dock = bench.dockFor(view);
    QVERIFY(dock);
    if (floating)
      dock->setFloating(true);
    QVERIFY(activateTestWindow(view->window()));
    view->text()->setFocus();
    QTRY_VERIFY(view->text()->hasFocus());
    bench.action(ActionId::JumpXrefsFrom)->trigger();
    QPointer<QDialog> dialog = visibleDialog("referencesDialog");
    QVERIFY(dialog);
    QCOMPARE(dialog->parentWidget(), view);
    if (change == "representation")
      view->setRepresentation(QStringLiteral("llvmc"));
    else
      view->showFunction(change == "reload" ? Base + 0x140 : Base + 0x160);
    QTRY_VERIFY(!dialog);
  }

  void staleCodeXrefQueriesDoNotOpenDialogs_data() {
    QTest::addColumn<QString>("change");
    for (const auto *change :
         {"function", "representation", "navigation", "session", "hidden"})
      QTest::newRow(change) << QString::fromLatin1(change);
  }

  void staleCodeXrefQueriesDoNotOpenDialogs() {
    QFETCH(QString, change);
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(writeFixture(directory, "xrefs-delayed.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    // IR has no pre-resolved source edit targets, so X resolves the name
    // asynchronously rather than using an address published with the text.
    bench.action(ActionId::ViewLowIR)->trigger();
    auto *view = bench.codeView(QStringLiteral("low"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading());
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    const QFontMetricsF metrics(Theme::instance().codeFont());
    code->setCursorLine(1);
    const int column = int(code->selectedText().indexOf("delayed_function"));
    QVERIFY(column >= 0);
    QTest::mouseClick(
        code->viewport(), Qt::LeftButton, {},
        QPoint(qRound(6 + (5 + column + 2) * metrics.horizontalAdvance('M')),
               qRound((1 - code->verticalScrollBar()->value() + 0.5) *
                      std::ceil(metrics.lineSpacing()))));
    QCOMPARE(code->currentToken(), QStringLiteral("delayed_function"));
    QTRY_VERIFY(code->hasFocus());
    QSignalSpy messages(&bench.session, &Session::message);
    bool dialogOpened = false;
    QTimer guard;
    connect(&guard, &QTimer::timeout, this, [&] {
      if (auto *dialog = visibleDialog(QStringLiteral("referencesDialog"))) {
        dialogOpened = true;
        dialog->reject();
      }
    });
    guard.start(5);
    bench.action(ActionId::JumpXrefOperand)->trigger();
    const auto started = [&] {
      for (const auto &row : messages)
        if (row.first().toString().contains("fixture delayed lookup started"))
          return true;
      return false;
    };
    QTRY_VERIFY(started());
    if (change == "function")
      view->showFunction(Base + 0x180);
    else if (change == "representation")
      view->setRepresentation(QStringLiteral("llvmc"));
    else if (change == "navigation")
      assembly->navigate(Base + 0x180);
    else if (change == "session")
      bench.session.closeFile();
    else
      view->hide();
    QTest::qWait(600);
    QVERIFY(!dialogOpened);
    QVERIFY(!QApplication::activeModalWidget());
    if (change == "session") {
      QVERIFY(!bench.session.loaded());
    } else {
      QCOMPARE(code->function(),
               std::optional<Address>(change == "function" ? Base + 0x180
                                                           : Base + 0x140));
      // Navigation reads its page from the worker; a slow runner may still
      // be on the way there.
      QTRY_COMPARE(assembly->currentFunction(),
                   std::optional<Address>(
                       change == "navigation" ? Base + 0x180 : Base + 0x140));
    }
  }

  void copyingCharacterSelections_data() {
    QTest::addColumn<QString>("view");
    QTest::addColumn<bool>("reverse");
    for (const QString view :
         {QStringLiteral("address"), QStringLiteral("assembly"),
          QStringLiteral("source"), QStringLiteral("go"),
          QStringLiteral("llvmc")}) {
      QTest::newRow(qPrintable(view + "-forward")) << view << false;
      QTest::newRow(qPrintable(view + "-reverse")) << view << true;
    }
  }

  void outputLogClearsFromItsMenu() {
    Workbench bench;
    auto *output = bench.window->findChild<OutputWindow *>();
    QVERIFY(output);
    auto *log = output->findChild<QPlainTextEdit *>();
    QVERIFY(log);
    QCOMPARE(log->contextMenuPolicy(), Qt::CustomContextMenu);
    output->append(QStringLiteral("first line"));
    const auto action = [](const std::unique_ptr<QMenu> &menu,
                           const char *name) {
      return menu->findChild<QAction *>(QLatin1String(name));
    };
    std::unique_ptr<QMenu> menu(output->createLogMenu());
    QVERIFY(action(menu, "outputCopy") && action(menu, "outputSelectAll"));
    QVERIFY(!action(menu, "outputCopy")->isEnabled());
    QVERIFY(action(menu, "outputClear")->isEnabled());
    action(menu, "outputSelectAll")->trigger();
    QVERIFY(log->textCursor().hasSelection());
    menu.reset(output->createLogMenu());
    QVERIFY(action(menu, "outputCopy")->isEnabled());
    action(menu, "outputClear")->trigger();
    QVERIFY(log->document()->isEmpty());
    // An empty log has nothing to clear.
    menu.reset(output->createLogMenu());
    QVERIFY(!action(menu, "outputClear")->isEnabled());
  }

  void outputLogFollowsTheTheme_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark-to-light") << true;
    QTest::newRow("light-to-dark") << false;
  }

  void outputLogFollowsTheTheme() {
    // Lines written before a theme change take the new theme's colors: the
    // dark theme's light gray all but vanished on the light theme's white.
    QFETCH(bool, dark);
    auto &theme = Theme::instance();
    const auto previous = theme.mode();
    const auto restoreTheme = qScopeGuard([&] { theme.setMode(previous); });
    theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    theme.apply();
    Workbench bench;
    auto *output = bench.window->findChild<OutputWindow *>();
    QVERIFY(output);
    auto *log = output->findChild<QPlainTextEdit *>();
    QVERIFY(log);
    log->clear();
    for (int level = 0; level <= 2; ++level)
      output->append(QStringLiteral("level %1").arg(level), level);
    const auto written = [log] {
      QList<QColor> colors;
      for (auto block = log->document()->begin(); block.isValid();
           block = block.next())
        colors << block.begin().fragment().charFormat().foreground().color();
      return colors;
    };
    const auto levels = [&theme] {
      return QList<QColor>{theme.color(ColorRole::CodeDefault),
                           theme.color(ColorRole::CodePreprocessor),
                           theme.color(ColorRole::ListingError)};
    };
    QCOMPARE(written(), levels());
    const auto before = written();
    theme.setMode(dark ? Theme::Mode::Light : Theme::Mode::Dark);
    QCOMPARE(written(), levels());
    QVERIFY(written() != before);
  }

  void copyingCharacterSelections() {
    QFETCH(QString, view);
    QFETCH(bool, reverse);
    QTemporaryDir directory;
    Workbench bench;
    const auto path = writeFixture(directory, view == QLatin1String("go")
                                                  ? "pseudocode-go.bin"
                                                  : "fixture.bin");
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    const QFont font = Theme::instance().codeFont();
    const QFontMetricsF metrics(font);
    const int height = int(std::ceil(metrics.lineSpacing()));
    const qreal width = metrics.horizontalAdvance(QLatin1Char('M'));
    QAbstractScrollArea *text = nullptr;
    QString row;
    int first = 0, last = 0, y = 0, left = 0;
    if (view == QLatin1String("address") || view == QLatin1String("assembly")) {
      auto *listing = bench.window->disassembly()->listing();
      QTRY_VERIFY(listing->cursorLine());
      listing->setShowPrefixes(view == QLatin1String("address"));
      listing->horizontalScrollBar()->setValue(0);
      left = int(std::ceil(width * 7)) + 6;
      // Pick a visible row by the same native mouse input a user uses.
      for (y = height / 2; y < listing->viewport()->height(); y += height) {
        QTest::mouseClick(listing->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(left, y));
        if (!listing->cursorLine())
          continue;
        row = view == QLatin1String("address")
                  ? listing->cursorLine()->prefix
                  : listing->cursorLine()->styled.text;
        if (row.size() >= 8)
          break;
      }
      QVERIFY(row.size() >= 8);
      first = view == QLatin1String("address") ? int(row.size()) - 8 : 2;
      last = view == QLatin1String("address") ? int(row.size()) : 6;
      text = listing;
    } else {
      const QString representation = view == QLatin1String("llvmc")
                                         ? QStringLiteral("llvmc")
                                         : QStringLiteral("source");
      bench.action(ActionId::ViewPseudocode)->trigger();
      auto *codeView = bench.codeView(QStringLiteral("source"));
      QVERIFY(codeView);
      codeView->setRepresentation(representation);
      auto *code = codeView->text();
      QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() > 3,
                               OpenTimeoutMs);
      if (view == QLatin1String("llvmc"))
        QVERIFY(code->preludeFolded());
      const QString token = view == QLatin1String("go")
                                ? QStringLiteral("int64")
                                : QStringLiteral("code");
      int line = 1;
      for (; line < std::min(50, code->lineCount()); ++line) {
        code->setCursorLine(line);
        row = code->selectedText();
        first = int(row.indexOf(token));
        if (first >= 0)
          break;
      }
      QVERIFY(first >= 0);
      last = first + int(token.size());
      y = (line - code->verticalScrollBar()->value()) * height + height / 2;
      left = 6 +
             int((std::max(3, int(QString::number(code->lineCount()).size())) +
                  2) *
                 width) -
             code->horizontalScrollBar()->value();
      text = code;
    }
    QTextLayout layout(row, font);
    layout.beginLayout();
    const auto line = layout.createLine();
    layout.endLayout();
    const QPoint begin(left + qRound(line.cursorToX(first)), y);
    const QPoint end(left + qRound(line.cursorToX(last)), y);
    dragText(text->viewport(), reverse ? end : begin, reverse ? begin : end);
    const QString expected = row.mid(first, last - first);
    clearClipboard();
    QTest::keyClick(text, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(clipboardText(), expected);
    clearClipboard();
    bench.action(ActionId::EditCopy)->trigger();
    QCOMPARE(clipboardText(), expected);
    // A right click must keep the selection for context-menu copying.
    QTest::mouseClick(text->viewport(), Qt::RightButton, Qt::NoModifier, end);
    bench.action(ActionId::EditCopy)->trigger();
    QCOMPARE(clipboardText(), expected);
    // Keyboard selection uses the same character boundaries as mouse dragging.
    QTest::keyClick(text, Qt::Key_Home);
    for (int i = 0; i < 3; ++i)
      QTest::keyClick(
          text, view == QLatin1String("address") ? Qt::Key_Left : Qt::Key_Right,
          Qt::ShiftModifier);
    QTest::keyClick(text, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(clipboardText(), view == QLatin1String("address")
                                  ? (row + QLatin1Char(' ')).right(3)
                                  : row.left(3));
    if (view != QLatin1String("address")) {
      QTest::keyClick(text, Qt::Key_Home);
      QTest::keyClick(text, Qt::Key_Right);
      QTest::keyClick(text, Qt::Key_Right);
      QTest::keyClick(text, Qt::Key_Down);
      const QString next =
          qobject_cast<CodeText *>(text)
              ? qobject_cast<CodeText *>(text)->selectedText()
              : qobject_cast<ListingView *>(text)->selectedText();
      QTest::keyClick(text, Qt::Key_Up);
      QTest::keyClick(text, Qt::Key_Home);
      QTest::keyClick(text, Qt::Key_Right);
      QTest::keyClick(text, Qt::Key_Right);
      QTest::keyClick(text, Qt::Key_Down, Qt::ShiftModifier);
      QTest::keyClick(text, Qt::Key_C, Qt::ControlModifier);
      QCOMPARE(clipboardText(), row.mid(2) + QLatin1Char('\n') + next.left(2));
    }
  }

  void tabFromCodeUsesSelectedInstruction_data() {
    QTest::addColumn<QString>("representation");
    QTest::addColumn<bool>("native");
    QTest::addColumn<bool>("graph");
    QTest::newRow("pseudocode") << QStringLiteral("source") << false << false;
    QTest::newRow("llvm-c") << QStringLiteral("llvmc") << false << false;
    QTest::newRow("low-ir") << QStringLiteral("low") << false << false;
    QTest::newRow("high-ir") << QStringLiteral("high") << false << false;
    QTest::newRow("pseudocode-graph")
        << QStringLiteral("source") << false << true;
    if (!qEnvironmentVariable("NEVERD_CODE_NAV_WORKER").isEmpty() &&
        !qEnvironmentVariable("NEVERD_CODE_NAV_FILE").isEmpty()) {
      QTest::newRow("native-pseudocode")
          << QStringLiteral("source") << true << false;
      QTest::newRow("native-llvm-c")
          << QStringLiteral("llvmc") << true << false;
      QTest::newRow("native-high-ir")
          << QStringLiteral("high") << true << false;
      QTest::newRow("native-pseudocode-graph")
          << QStringLiteral("source") << true << true;
    }
  }

  void tabFromCodeUsesSelectedInstruction() {
    QFETCH(QString, representation);
    QFETCH(bool, native);
    QFETCH(bool, graph);
    QTemporaryDir directory;
    Workbench bench(native ? qEnvironmentVariable("NEVERD_CODE_NAV_WORKER")
                           : QString::fromLocal8Bit(TEST_WORKER));
    const auto path =
        native
            ? qEnvironmentVariable("NEVERD_CODE_NAV_FILE")
            : writeFixture(directory, QStringLiteral("code-edits-mapped.bin"));
    if (native)
      bench.window->setAsksForDebugInfo(false);
    LoadDialogAcceptor acceptLoad;
    bench.window->openFile(path);
    QTRY_VERIFY2_WITH_TIMEOUT(bench.session.loaded(),
                              qPrintable(bench.session.lastError()),
                              native ? NativeOpenTimeoutMs : OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    auto *functions = bench.functions();
    QTRY_VERIFY(functions && functions->model().total() >= 2);
    const auto function =
        native ? *functions->model().addressAt(0) : Base + 0x140;
    const auto other = native ? *functions->model().addressAt(1) : Base + 0x180;
    assembly->navigate(function);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(function));
    if (graph) {
      assembly->setGraphMode(true);
      QTRY_VERIFY(assembly->graphMode() && assembly->graph()->loaded());
    }
    bench
        .action(representation == QLatin1String("low")    ? ActionId::ViewLowIR
                : representation == QLatin1String("high") ? ActionId::ViewHighIR
                : representation == QLatin1String("llvmc")
                    ? ActionId::ViewLLVMC
                    : ActionId::ViewPseudocode)
        ->trigger();
    auto *view = bench.codeView(representation);
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 2);
    code->setPreludeFolded(false);
    bool queryRow = false;
    if (native) {
      code->setCursorLine(0);
      queryRow = code->findText(representation == QLatin1String("llvmc")
                                    ? QStringLiteral("\"cpuid\"")
                                    : QStringLiteral("__cpuid"),
                                true);
      QVERIFY(queryRow || code->findText(QStringLiteral("return "), true));
    } else
      code->setCursorLine(2);
    const auto target = code->currentAddress();
    QVERIFY(target.has_value());
    QVERIFY(*target != function);
    if (native) {
      bool decoded = false;
      bench.session.read(
          "disasm", {{"address", hexAddress(*target)}, {"limit", 1}}, code,
          [&](const QJsonObject &p) {
            const auto rows = p.value("items").toArray();
            decoded = !rows.isEmpty() &&
                      addressValue(rows.first().toObject().value("address")) ==
                          target;
            if (queryRow)
              decoded = decoded && rows.first().toObject().value("mnemonic") ==
                                       QLatin1String("cpuid");
          });
      QTRY_VERIFY(decoded);
    } else
      QCOMPARE(target, std::optional<Address>(function + 2));
    // Clicks and arrow keys move the source cursor and take the assembly
    // cursor to the row's instruction, leaving the focus and the jump
    // history where they are.
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const bool back = assembly->canGoBack();
    const bool forward = assembly->canGoForward();
    if (!native) {
      code->setCursorLine(0);
      assembly->navigate(function, false);
      QTRY_COMPARE(assembly->currentAddress(),
                   std::optional<Address>(function));
      const int height = int(
          std::ceil(QFontMetricsF(Theme::instance().codeFont()).lineSpacing()));
      QTest::mouseClick(code->viewport(), Qt::LeftButton, {},
                        QPoint(100, 2 * height + height / 2));
      QCOMPARE(code->currentAddress(), target);
      QTRY_COMPARE(assembly->currentAddress(), target);
    }
    assembly->navigate(function, false);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(function));
    QTest::keyClick(code, Qt::Key_Right);
    QTRY_COMPARE(assembly->currentAddress(), target);
    QVERIFY(code->hasFocus());
    QCOMPARE(assembly->canGoBack(), back);
    QCOMPARE(assembly->canGoForward(), forward);
    // Assembly navigation leaves this unpinned source on its own function.
    assembly->navigate(other);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(other));
    QTest::qWait(300);
    QCOMPARE(code->function(), std::optional<Address>(function));
    QCOMPARE(code->currentAddress(), target);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    QTest::keyClick(code, Qt::Key_Tab);
    QTRY_COMPARE(assembly->currentAddress(), target);
    QTRY_VERIFY(!code->hasFocus());
    QCOMPARE(code->function(), std::optional<Address>(function));
    // A reverse Tab must select the mapped row, rather than simply show the
    // function. Changing the source cursor first disproves a focus-only fix.
    code->setCursorLine(0);
    QVERIFY(!code->currentAddress());
    QCOMPARE(assembly->currentAddress(), target);
    assembly->focusContent();
    QTRY_VERIFY(assembly->isAncestorOf(QApplication::focusWidget()));
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
    QTRY_VERIFY(code->hasFocus());
    QTRY_COMPARE(code->currentAddress(), target);
    QCOMPARE(view->representation(), representation);
    if (native) {
      const auto capture = qEnvironmentVariable("NEVERD_CODE_NAV_CAPTURE_DIR");
      if (!capture.isEmpty())
        QVERIFY(bench.window->grab().save(capture + "/tab-" + representation +
                                          ".png"));
    }

    // A declaration has no instruction anchor; the fallback is explicit and
    // belongs to this source function, not the hidden assembly's function.
    code->setCursorLine(0);
    QVERIFY(!code->currentAddress());
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    QTest::keyClick(code, Qt::Key_Tab);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(function));
  }

  void contextMenusOfferTabWithItsKey() {
    // Tab's switch to the other pane is in both context menus with its key:
    // a code window's as Jump to disassembly beside the synchronization
    // switch, the disassembly's as Jump to pseudocode.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-numbers.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address function = Base + 0x140;
    assembly->navigate(function);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 3);
    code->setPreludeFolded(false);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    code->setCursorLine(3);
    assembly->navigate(function, false);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(function));
    QString shown;
    bool jumpOffered = true;
    withCodeMenu(code, [&](QMenu *menu) {
      auto *cross =
          menu->findChild<QAction *>(QStringLiteral("codeJumpDisassembly"));
      QVERIFY(cross);
      shown = cross->text();
      // The disassembly's own wording would point back at this window.
      jumpOffered =
          menu->actions().contains(bench.action(ActionId::JumpPseudocode));
      cross->trigger();
    });
    QCOMPARE(shown,
             QStringLiteral("Jump to disassembly\t") +
                 QKeySequence(Qt::Key_Tab).toString(QKeySequence::NativeText));
    QVERIFY(!jumpOffered);
    QTRY_VERIFY(assembly->isAncestorOf(QApplication::focusWidget()));
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 6));

    QStringList texts;
    QTimer probe;
    connect(&probe, &QTimer::timeout, this, [&] {
      auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      if (!menu)
        return;
      probe.stop();
      for (const QAction *action : menu->actions())
        texts << action->text();
      menu->close();
    });
    probe.start(5);
    emit assembly->contextMenuRequested(assembly->mapToGlobal(QPoint(40, 40)));
    QVERIFY(texts.contains(bench.action(ActionId::JumpPseudocode)->text()));
    QCOMPARE(bench.action(ActionId::JumpPseudocode)->shortcut(),
             QKeySequence(Qt::Key_Tab));
  }

  void generationCreatesIndependentWindows_data() {
    QTest::addColumn<int>("command");
    QTest::addColumn<QString>("representation");
    for (const auto &[action, representation] :
         {std::pair{ActionId::ViewPseudocode, "source"},
          {ActionId::ViewLLVMC, "llvmc"},
          {ActionId::ViewLowIR, "low"},
          {ActionId::ViewMedIR, "med"},
          {ActionId::ViewHighIR, "high"},
          {ActionId::ViewLLVMIR, "llvm"}})
      QTest::newRow(representation)
          << int(action) << QString::fromLatin1(representation);
  }

  void generationCreatesIndependentWindows() {
    QFETCH(int, command);
    QFETCH(QString, representation);
    Theme::instance().apply();
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "code-numbers.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(activateTestWindow(bench.window.get()));
    auto *assembly = bench.window->disassembly();
    auto *action = bench.action(ActionId(command));
    const auto generate = [&](QWidget *target) {
      // Cocoa can deactivate the test between gestures. Establish the
      // intended input surface instead of sending a key to a null focus.
      QVERIFY(activateTestWindow(target->window()));
      target->setFocus();
      QTRY_VERIFY(target->hasFocus());
      if (action->shortcut().isEmpty())
        action->trigger();
      else
        QTest::keySequence(target, action->shortcut());
    };
    assembly->navigate(Base + 0x140);
    QTRY_VERIFY(ListingViewTestAccess::settled(*assembly->listing()));
    assembly->focusContent();
    generate(assembly->listing());
    QTRY_COMPARE(bench.window->findChildren<CodeView *>().size(), 1);
    auto *first = bench.codeView(representation);
    QVERIFY(first);
    QTRY_VERIFY(first->text()->complete());
    const auto original = first->text()->allText();
    auto *firstDock = bench.dockFor(first);

    assembly->focusContent();
    // Generate while the jump is pending: the second window must use its
    // destination, not the first window's function.
    assembly->navigate(Base + 0x160);
    generate(assembly->listing());
    QTRY_COMPARE(bench.window->findChildren<CodeView *>().size(), 2);
    auto views = bench.window->findChildren<CodeView *>();
    auto *second = views[0] == first ? views[1] : views[0];
    QTRY_VERIFY(second->text()->complete());
    QCOMPARE(second->representation(), representation);
    QCOMPARE(second->text()->function(), std::optional<Address>(Base + 0x160));
    QCOMPARE(first->text()->function(), std::optional<Address>(Base + 0x140));
    QCOMPARE(first->text()->allText(), original);
    auto *secondDock = bench.dockFor(second);
    QVERIFY(firstDock->uniqueName() != secondDock->uniqueName());
    QVERIFY(firstDock->title() != secondDock->title());
    QCOMPARE(firstDock->parentWidget(), secondDock->parentWidget());

    // Generate inside a code window refreshes only that document. Tab then
    // reuses it for another function without replacing the first document.
    QVERIFY(activateTestWindow(second->window()));
    QTRY_VERIFY(second->text()->hasFocus());
    generate(second->text());
    QTRY_VERIFY(second->text()->complete());
    QCOMPARE(bench.window->findChildren<CodeView *>().size(), 2);
    QTest::keyClick(second->text(), Qt::Key_Tab);
    QTRY_VERIFY(assembly->isAncestorOf(QApplication::focusWidget()));
    assembly->navigate(Base + 0x180);
    QTRY_VERIFY(ListingViewTestAccess::settled(*assembly->listing()));
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
    QTRY_COMPARE(second->text()->function(),
                 std::optional<Address>(Base + 0x180));
    QTRY_VERIFY(second->text()->complete());
    QCOMPARE(bench.window->findChildren<CodeView *>().size(), 2);
    QCOMPARE(first->text()->allText(), original);

    // An explicit generation from assembly may open the same function twice.
    assembly->focusContent();
    generate(assembly->listing());
    QTRY_COMPARE(bench.window->findChildren<CodeView *>().size(), 3);
    secondDock->forceClose();
    assembly->focusContent();
    generate(assembly->listing());
    QTRY_VERIFY(secondDock->isOpen() && second->isVisible());
    QTRY_VERIFY(second->text()->complete());
    QCOMPARE(bench.window->findChildren<CodeView *>().size(), 3);

    const auto capture = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
    if (!capture.isEmpty()) {
      QDir().mkpath(capture);
      auto &theme = Theme::instance();
      const auto mode = theme.mode();
      const auto restore = qScopeGuard([&] { theme.setMode(mode); });
      for (const bool dark : {false, true}) {
        theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
        theme.apply();
        QApplication::processEvents();
        QVERIFY(bench.window->grab().save(capture + "/windows-" +
                                          representation +
                                          (dark ? "-dark.png" : "-light.png")));
      }
    }
  }

  void generationFromFloatingSourceKeepsItsFunctionAndLanguage() {
    Theme::instance().apply();
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loads;
    bench.window->openFile(writeFixture(directory, "pseudocode-cpp.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_VERIFY(ListingViewTestAccess::settled(*assembly->listing()));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *source = bench.codeView("source");
    QVERIFY(source);
    QTRY_VERIFY(source->text()->complete());
    source->setRepresentation("c");
    QTRY_VERIFY(source->text()->complete());
    QCOMPARE(source->representation(), QStringLiteral("c"));
    bench.dockFor(assembly)->setFloating(true);
    QVERIFY(activateTestWindow(bench.window.get()));
    source->text()->setFocus();
    QTRY_VERIFY(source->text()->hasFocus());
    bench.action(ActionId::ViewPseudocode)->trigger();
    QCOMPARE(source->window(), bench.window.get());
    auto *dock = bench.dockFor(source);
    dock->setFloating(true);
    auto *floating = source->window();
    QVERIFY(activateTestWindow(floating));
    source->text()->setFocus();
    QTRY_VERIFY(source->text()->hasFocus());
    assembly->navigate(Base + 0x180);
    QTRY_VERIFY(ListingViewTestAccess::settled(*assembly->listing()));
    bench.action(ActionId::ViewPseudocode)->trigger();
    QTRY_VERIFY(source->text()->complete());
    QCOMPARE(source->representation(), QStringLiteral("c"));
    QCOMPARE(source->text()->function(), std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewMedIR)->trigger();
    CodeView *med = nullptr;
    for (auto *view : floating->findChildren<CodeView *>())
      if (view->representation() == QLatin1String("med"))
        med = view;
    QVERIFY(med);
    QTRY_VERIFY(med->text()->complete());
    QCOMPARE(med->text()->function(), std::optional<Address>(Base + 0x140));
    QCOMPARE(med->window(), floating);
    QCOMPARE(source->representation(), QStringLiteral("c"));
  }

  void assemblyAndCodeCursorsFollowEachOther() {
    // A move in the disassembly takes a code window showing the same
    // function to the instruction's row and leaves the focus there; a code
    // window showing another function stays. "Synchronize with disassembly"
    // in the code's context menu lets both browse apart.
    QTemporaryDir directory;
    Workbench bench;
    const auto restore = qScopeGuard(
        [] { QSettings().remove(QStringLiteral("code/synchronize")); });
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-numbers.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address function = Base + 0x140;
    assembly->navigate(function);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 3);
    code->setPreludeFolded(false);
    code->setCursorLine(1);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    assembly->focusContent();
    QTRY_VERIFY(assembly->isAncestorOf(QApplication::focusWidget()));
    assembly->navigate(function + 6, false);
    QTRY_COMPARE(code->cursorLine(), 3);
    QCOMPARE(code->currentAddress(), std::optional<Address>(function + 6));
    QVERIFY(assembly->isAncestorOf(QApplication::focusWidget()));
    assembly->navigate(function + 2, false);
    QTRY_COMPARE(code->cursorLine(), 2);
    // An instruction no row maps leaves the cursor where it is.
    assembly->navigate(function + 9, false);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 9));
    QTest::qWait(100);
    QCOMPARE(code->cursorLine(), 2);
    // Another function is not decompiled on the way.
    assembly->navigate(Base + 0x180, false);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x180));
    QTest::qWait(200);
    QCOMPARE(code->function(), std::optional<Address>(function));
    QCOMPARE(code->cursorLine(), 2);

    // Apart, the code's rows move neither the disassembly nor the other way.
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    withCodeMenu(code, [](QMenu *menu) {
      auto *synchronize =
          menu->findChild<QAction *>(QStringLiteral("codeSynchronize"));
      QVERIFY(synchronize && synchronize->isChecked());
      synchronize->trigger();
    });
    QVERIFY(
        !QSettings().value(QStringLiteral("code/synchronize"), true).toBool());
    code->setCursorLine(3);
    QTest::qWait(200);
    QCOMPARE(assembly->currentFunction(), std::optional<Address>(Base + 0x180));
    assembly->navigate(function, false);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(function));
    assembly->focusContent();
    QTRY_VERIFY2(
        assembly->isAncestorOf(QApplication::focusWidget()),
        qPrintable(
            QStringLiteral("visible=%1 active=%2 focus=%3")
                .arg(assembly->isVisible())
                .arg(QApplication::activeWindow() ? QApplication::activeWindow()
                                                        ->metaObject()
                                                        ->className()
                                                  : "none")
                .arg(
                    QApplication::focusWidget()
                        ? QApplication::focusWidget()->metaObject()->className()
                        : "none")));
    assembly->navigate(function + 2, false);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 2));
    QTest::qWait(100);
    QCOMPARE(code->cursorLine(), 3);
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    withCodeMenu(code, [](QMenu *menu) {
      auto *synchronize =
          menu->findChild<QAction *>(QStringLiteral("codeSynchronize"));
      QVERIFY(synchronize && !synchronize->isChecked());
      synchronize->trigger();
    });
    code->setCursorLine(2);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 2));
    code->setCursorLine(3);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 6));
  }

  void pseudocodeNumbersTakeTheBaseTheUserPicks() {
    // Hex-Rays' bases for a pseudocode number: H toggles between
    // hexadecimal and decimal, the context menu offers each base with what
    // the number would read in it, # gives the number back to the
    // decompiler, a base that would change its type is refused, and each
    // change is a step of the history.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-numbers.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const Address function = Base + 0x140;
    bench.window->disassembly()->navigate(function);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 3);
    code->setPreludeFolded(false);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    // A comment's digits are no number.
    code->setCursorLine(1);
    QVERIFY(code->findText(QStringLiteral("8 */"), true));
    QVERIFY(!code->numberTarget());
    code->setCursorLine(1);
    QVERIFY(code->findText(QStringLiteral("24"), true));
    const auto target = code->numberTarget();
    QVERIFY(target.has_value());
    QCOMPARE(target->line, 2);
    QCOMPARE(target->index, 0);
    QCOMPARE(target->literal.Text, std::string("24"));
    QCOMPARE(target->chosen, neverd::LiteralRadix::Number);

    QVERIFY(bench.action(ActionId::EditOperandDecimal)->isEnabled());
    QVERIFY(!bench.action(ActionId::EditOperandOffset)->isEnabled());
    bench.action(ActionId::EditOperandDecimal)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(QStringLiteral("v0 + 0x18; /* 8 */")),
        OpenTimeoutMs);
    QStringList offered;
    withCodeMenu(code, [&](QMenu *menu) {
      for (const QAction *action : menu->actions())
        if (action->objectName().startsWith(QStringLiteral("codeNumber_")))
          offered << action->text();
      auto *hex = menu->findChild<QAction *>(QStringLiteral("codeNumber_hex"));
      QVERIFY(hex && hex->isChecked());
      auto *octal =
          menu->findChild<QAction *>(QStringLiteral("codeNumber_octal"));
      QVERIFY(octal && octal->isEnabled());
      octal->trigger();
    });
    QCOMPARE(offered, (QStringList{QStringLiteral("&Number\t24"),
                                   QStringLiteral("&Hexadecimal\t0x18"),
                                   QStringLiteral("&Decimal\t24"),
                                   QStringLiteral("Octa&l\t030"),
                                   QStringLiteral("&Binary\t0b11000"),
                                   QStringLiteral("&Character\t'\\030'")}));
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(QStringLiteral("v0 + 030; /* 8 */")),
        OpenTimeoutMs);
    bench.action(ActionId::EditOperandNumber)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(QStringLiteral("v0 + 24; /* 8 */")),
        OpenTimeoutMs);
    QTRY_VERIFY(bench.session.canUndo());
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(QStringLiteral("v0 + 030; /* 8 */")),
        OpenTimeoutMs);

    // An unsigned int has no character constant; in decimal it keeps its
    // type with a suffix.
    code->setCursorLine(2);
    QVERIFY(code->findText(QStringLiteral("0xFFFFFFFF"), true));
    bench.action(ActionId::EditOperandChar)->trigger();
    QTest::qWait(200);
    QVERIFY(code->allText().contains(QStringLiteral("v1 & 0xFFFFFFFF;")));
    bench.action(ActionId::EditOperandDecimal)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(QStringLiteral("v1 & 4294967295U;")),
        OpenTimeoutMs);
    QVERIFY(code->allText().contains(QStringLiteral("v0 + 030; /* 8 */")));
  }

  void codeTextSelectsLikeAnEditor_data() {
    QTest::addColumn<bool>("popupTakesFocus");
    QTest::newRow("mouse") << false;
    QTest::newRow("popup-focus") << true;
  }

  void codeTextSelectsLikeAnEditor() {
    QFETCH(bool, popupTakesFocus);
    // Ctrl+A selects the whole function, a drag held below the text scrolls
    // and extends the selection past the first screen, and a right click
    // inside the selection keeps it for Copy, as in an editor.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-numbers-paged.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const Address function = Base + 0x140;
    bench.window->disassembly()->navigate(function);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    // Ctrl+A while pages still arrive selects the whole function once the
    // last one is in, its folded prelude included.
    bool pressed = false;
    QObject context;
    connect(code->verticalScrollBar(), &QScrollBar::rangeChanged, &context,
            [&] {
              if (pressed || !code->loading() || code->lineCount() == 0)
                return;
              pressed = true;
              QTest::keyClick(code, Qt::Key_A, Qt::ControlModifier);
            });
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() > 600,
                             OpenTimeoutMs);
    QVERIFY(pressed);
    QCOMPARE(code->selectedText(), code->allText());
    code->setPreludeFolded(false);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());

    QTest::keyClick(code, Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(code->selectedText(), code->allText());
    bench.action(ActionId::EditCopy)->trigger();
    QCOMPARE(clipboardText(), code->allText());

    code->setCursorLine(0);
    QCOMPARE(code->verticalScrollBar()->value(), 0);
    const int height = int(
        std::ceil(QFontMetricsF(Theme::instance().codeFont()).lineSpacing()));
    QTest::mousePress(code->viewport(), Qt::LeftButton, {},
                      QPoint(60, 2 * height + height / 2));
    const QPoint below(60, code->viewport()->height() + 3 * height);
    QTest::mouseMove(code->viewport(), below);
    QTRY_VERIFY(code->verticalScrollBar()->value() > 100);
    QTest::mouseRelease(code->viewport(), Qt::LeftButton, {}, below);
    const int last = code->cursorLine();
    QVERIFY(last > 100);
    const QString selected = code->selectedText();
    QCOMPARE(int(selected.count(QLatin1Char('\n'))), last - 2);

    // A right click inside the selection keeps it, and the menu's Copy,
    // beside Select all, copies it.
    QApplication::clipboard()->clear();
    const QPoint inside(60, code->viewport()->height() / 2);
    QTest::mousePress(code->viewport(), Qt::RightButton, {}, inside);
    QTest::mouseRelease(code->viewport(), Qt::RightButton, {}, inside);
    QCOMPARE(code->selectedText(), selected);
    auto *copy = bench.action(ActionId::EditCopy);
    bool clicked = false;
    QTimer clicker;
    connect(&clicker, &QTimer::timeout, this, [&] {
      auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      if (!menu)
        return;
      clicker.stop();
      clicked = menu->actions().contains(copy) &&
                menu->findChild<QAction *>(QStringLiteral("codeSelectAll"));
      if (clicked) {
        if (popupTakesFocus) {
          // Native popups can take the focus before their action fires.
          code->clearFocus();
          QVERIFY(!code->hasFocus());
          copy->trigger();
        } else {
          QTest::mouseClick(menu, Qt::LeftButton, {},
                            menu->actionGeometry(copy).center());
        }
      }
      if (menu->isVisible())
        menu->close();
    });
    clicker.start(5);
    QContextMenuEvent request(QContextMenuEvent::Mouse, inside,
                              code->viewport()->mapToGlobal(inside));
    QApplication::sendEvent(code->viewport(), &request);
    QVERIFY(clicked);
    QCOMPARE(clipboardText(), selected);
    QCOMPARE(code->selectedText(), selected);
  }

  void gotoLabelsGoToTheirBlock() {
    // A double-click on a goto's label takes the code to the label and the
    // disassembly to the block the label names, Back returning to the goto.
    // The label's row maps to that block, not to the statement around it
    // the engine's rows give it, so the cursor on it takes the disassembly
    // there too.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-labels.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const Address function = Base + 0x140;
    const Address block = function + 0xA;
    bench.window->disassembly()->navigate(function);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 6);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const QString label =
        QStringLiteral("L_") + QString::number(block, 16).toUpper();
    code->setCursorLine(0);
    QVERIFY(code->findText(QStringLiteral("goto ") + label, true));
    const int gotoRow = code->cursorLine();
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(function + 2));

    // The double-click acts on the token the cursor is on.
    code->setCursorLine(gotoRow - 1);
    QVERIFY(code->findText(label, true));
    QCOMPARE(code->cursorLine(), gotoRow);
    QCOMPARE(code->currentToken(), label);
    QSignalSpy names(code, &CodeText::nameActivated);
    const QPointF point(code->viewport()->width() / 2.0, 1);
    QMouseEvent click(QEvent::MouseButtonDblClick, point,
                      code->viewport()->mapToGlobal(point), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(code->viewport(), &click);
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(block));
    QCOMPARE(names.size(), 0);
    QVERIFY(code->cursorLine() > gotoRow);
    const int labelRow = code->cursorLine();
    QCOMPARE(code->currentToken(), label);
    QCOMPARE(code->currentAddress(), std::optional<Address>(block));
    QVERIFY(code->hasFocus());
    QCOMPARE(code->function(), std::optional<Address>(function));

    bench.action(ActionId::JumpBack)->trigger();
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(function + 2));
    QTRY_COMPARE(code->cursorLine(), gotoRow);
    bench.action(ActionId::JumpForward)->trigger();
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(block));
    QTRY_COMPARE(code->cursorLine(), labelRow);

    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    code->setCursorLine(gotoRow);
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(function + 2));
    code->setCursorLine(labelRow);
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(block));
    // A note on the label stays the line's, where the code shows it, not
    // the block instruction's comment.
    const auto note = code->commentTarget();
    QVERIFY(note);
    QVERIFY(!note->contains(QStringLiteral("mapped_address")));
  }

  void frameRecordsGoToTheirFunctionAndMembersToTheirUses_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
  }

  void frameRecordsGoToTheirFunctionAndMembersToTheirUses() {
    QFETCH(bool, dark);
    auto &theme = Theme::instance();
    const auto previous = theme.mode();
    const auto restoreTheme = qScopeGuard([&] { theme.setMode(previous); });
    theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    theme.apply();
    // The record HighC declares for a function's stack names the function:
    // a double-click on it, or on its storage, takes the disassembly to where
    // the function begins, and on a member to the instruction that first
    // reads or writes it, while the code stays; Back returns.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-frame.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const Address function = Base + 0x140;
    bench.window->disassembly()->navigate(function);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() == 8);
    QVERIFY(code->allText().contains(QStringLiteral("gap0[54]")));
    QVERIFY(activateTestWindow(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const auto capture = [&](const QString &state) {
      const QString captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
      if (captures.isEmpty())
        return true;
      QDir().mkpath(captures);
      return view->grab().save(captures + "/frame-" + state +
                               (dark ? "-dark.png" : "-light.png"));
    };
    QVERIFY(capture(QStringLiteral("folded")));
    QSignalSpy names(code, &CodeText::nameActivated);
    const auto doubleClick = [&] {
      const QPointF point(code->viewport()->width() / 2.0, 1);
      QMouseEvent click(QEvent::MouseButtonDblClick, point,
                        code->viewport()->mapToGlobal(point), Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(code->viewport(), &click);
    };
    code->setCursorLine(0);
    QVERIFY(code->findText(QStringLiteral("return stack_storage"), true));
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(function + 0xC));

    const QString record =
        QStringLiteral("frame_") + QString::number(function, 16).toUpper();
    code->setCursorLine(0);
    QVERIFY(code->findText(record, true));
    QCOMPARE(code->currentToken(), record);
    const int recordRow = code->cursorLine();
    doubleClick();
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(function));
    QCOMPARE(code->cursorLine(), recordRow);
    QVERIFY(code->hasFocus());
    bench.action(ActionId::JumpBack)->trigger();
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(function + 0xC));

    // A member's declaration goes to the statement that first writes it.
    code->setCursorLine(0);
    QVERIFY(code->findText(QStringLiteral("var_m30"), true));
    QCOMPARE(code->lineCount(), 12);
    QVERIFY(capture(QStringLiteral("expanded")));
    QCOMPARE(code->currentToken(), QStringLiteral("var_m30"));
    QVERIFY(code->cursorLine() < recordRow + 5);
    doubleClick();
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(function + 6));

    // The storage is the frame's too.
    code->setCursorLine(0);
    QVERIFY(code->findText(QStringLiteral("stack_storage"), true));
    QCOMPARE(code->currentToken(), QStringLiteral("stack_storage"));
    doubleClick();
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(function));
    QCOMPARE(names.size(), 0);
    QCOMPARE(code->function(), std::optional<Address>(function));
    // A refresh preserves an expanded layout as well as a collapsed one.
    code->load(function, QStringLiteral("source"));
    QTRY_VERIFY(!code->loading() && code->lineCount() == 12);
    code->setCursorLine(0);
    QVERIFY(code->findText(record, true));
    QVERIFY(code->setCurrentBlockFolded(true));
    QCOMPARE(code->lineCount(), 8);
    code->load(function, QStringLiteral("source"));
    QTRY_VERIFY(!code->loading() && code->lineCount() == 8);
    code->setCursorLine(0);
    QVERIFY(code->findText(QStringLiteral("gap0"), true));
    QCOMPARE(code->lineCount(), 12);
  }

  void codeEditKeepsThePlaceInALongFunction() {
    // An edit loads the function's pages again. The first page used to
    // clamp the cursor and the view, which then stayed near its end: the
    // cursor on a number of the third page jumped away from it.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-numbers-paged.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const Address function = Base + 0x140;
    bench.window->disassembly()->navigate(function);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() > 600,
                             OpenTimeoutMs);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    code->setCursorLine(0);
    QVERIFY(code->findText(QStringLiteral("24"), true));
    const int line = code->cursorLine();
    const int top = code->verticalScrollBar()->value();
    QVERIFY(line > 512);
    QVERIFY(top > 256);
    bench.action(ActionId::EditOperandDecimal)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(QStringLiteral("return v0 + 0x18;")),
        OpenTimeoutMs);
    QCOMPARE(code->cursorLine(), line);
    QCOMPARE(code->verticalScrollBar()->value(), top);
    const auto target = code->numberTarget();
    QVERIFY(target.has_value());
    QCOMPARE(target->shown, neverd::LiteralRadix::Hex);
  }

  void graphJumpDuringFunctionLoadKeepsLatestAddress() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-edits-mapped.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address first = Base + 0x140;
    const Address second = Base + 0x180;
    const Address target = first + 2;
    assembly->navigate(first);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(first));
    assembly->setGraphMode(true);
    QTRY_VERIFY(assembly->graphMode() && assembly->graph()->loaded());

    // Enter the interval after the next function was requested, before its
    // asynchronous layout arrives. A new jump must not use the old nodes as
    // though they belonged to the function being loaded.
    assembly->graph()->showFunction(second, second);
    assembly->navigate(target);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(first));
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(target));
    QTest::qWait(100);
    QCOMPARE(assembly->currentFunction(), std::optional<Address>(first));
    QCOMPARE(assembly->currentAddress(), std::optional<Address>(target));
  }

  void graphRefreshKeepsLatestRequestedAddress_data() {
    QTest::addColumn<QString>("refresh");
    QTest::newRow("navigate-during-refresh") << QStringLiteral("navigate");
    QTest::newRow("keyboard-during-refresh") << QStringLiteral("keyboard");
    QTest::newRow("mouse-during-refresh") << QStringLiteral("mouse");
    QTest::newRow("generation-during-load") << QStringLiteral("generation");
    QTest::newRow("revision-during-load") << QStringLiteral("revision");
  }

  void graphJumpDuringFunctionResolutionKeepsLatestAddress() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-edits-mapped.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address first = Base + 0x140;
    const Address second = Base + 0x180;
    const Address target = first + 2;
    assembly->navigate(first);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(first));
    assembly->setGraphMode(true);
    QTRY_VERIFY(assembly->graphMode() && assembly->graph()->loaded());

    // Another function first resolves through the listing. A new jump within
    // the visible graph supersedes that still-pending resolution as well.
    assembly->navigate(second);
    QVERIFY(assembly->listing()->jumpPending());
    assembly->navigate(target);
    QCOMPARE(assembly->currentAddress(), std::optional<Address>(target));
    QTRY_VERIFY(!assembly->listing()->jumpPending());
    QTRY_VERIFY(assembly->graph()->loaded());
    QCOMPARE(assembly->currentFunction(), std::optional<Address>(first));
    QCOMPARE(assembly->currentAddress(), std::optional<Address>(target));
    QCOMPARE(assembly->listing()->currentAddress(),
             std::optional<Address>(target));
  }

  void graphRefreshKeepsLatestRequestedAddress() {
    QFETCH(QString, refresh);
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-edits-mapped.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address first = Base + 0x140;
    const Address second = Base + 0x180;
    assembly->navigate(first);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(first));
    assembly->setGraphMode(true);
    QTRY_VERIFY(assembly->graphMode() && assembly->graph()->loaded());
    auto *graph = assembly->graph();
    QSignalSpy status(graph, &GraphView::statusChanged);
    Address target;
    if (refresh != QLatin1String("generation") &&
        refresh != QLatin1String("revision")) {
      target = first + 2;
      // A refresh retains its current nodes, so navigation can succeed before
      // the replacement layout arrives. That layout must retain the new row.
      graph->showFunction(first, first);
      if (refresh == QLatin1String("navigate")) {
        assembly->navigate(target);
      } else if (refresh == QLatin1String("keyboard")) {
        QTest::keyClick(graph, Qt::Key_Down);
        QTest::keyClick(graph, Qt::Key_Down);
      } else {
        QVERIFY(graph->nodes().size() > 2);
        graph->centerOn(graph->nodes().at(2).box.center());
        QTest::mouseClick(graph, Qt::LeftButton, {},
                          QPoint(graph->width() / 2, graph->height() / 2));
      }
      QCOMPARE(assembly->currentAddress(), std::optional<Address>(target));
    } else {
      target = second + 2;
      graph->showFunction(second, target);
      QVERIFY(!graph->currentAddress());
      // Background discovery or edits can refresh a function whose graph has
      // not arrived. Preserve the requested instruction across that refresh.
      if (refresh == QLatin1String("generation"))
        emit bench.session.generationChanged();
      else
        emit bench.session.revisionChanged();
    }
    QTRY_VERIFY(!status.isEmpty() &&
                status.back().front().toString().isEmpty());
    QVERIFY(graph->loaded());
    QCOMPARE(assembly->currentAddress(), std::optional<Address>(target));
  }

  void tabFromAssemblyWaitsForPagesAndUnfoldsTarget() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("tab-sync-paged.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address function = Base + 0x140, other = Base + 0x180;
    QVERIFY(activateTestWindow(bench.window.get()));
    assembly->navigate(function + 9);
    assembly->focusContent();
    // Tab during a listing jump also waits for the correct function to land.
    bench.action(ActionId::JumpPseudocode)->trigger();
    CodeView *view = nullptr;
    QTRY_VERIFY((view = bench.codeView(QStringLiteral("source"))));
    auto *code = view->text();
    QTRY_VERIFY(!code->loading());
    QCOMPARE(code->function(), std::optional<Address>(function));
    QCOMPARE(code->currentAddress(), std::optional<Address>(function + 9));
    QCOMPARE(code->commentTarget()->value("line").toInt(), 550);
    QVERIFY(code->preludeFolded());
    QVERIFY(code->verticalScrollBar()->value() > 256);
    QTRY_VERIFY(code->hasFocus());

    // Reuse the complete function without a decompile and expand just the
    // target operation; the declaration prelude stays folded.
    code->setFolded(true);
    QVERIFY(code->libraryFolded());
    code->setCursorLine(0);
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    QTest::keyClick(assembly->listing(), Qt::Key_Tab);
    QTRY_VERIFY(code->hasFocus());
    QVERIFY(!code->libraryFolded());
    QVERIFY(code->preludeFolded());
    QCOMPARE(code->commentTarget()->value("line").toInt(), 550);
    QCOMPARE(code->currentAddress(), std::optional<Address>(function + 9));
    // This row's primary mapping is +8: the selected secondary instruction
    // still survives the reverse Tab.
    QTest::keyClick(code, Qt::Key_Tab);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 9));

    const auto oldScroll = code->verticalScrollBar()->value();
    assembly->navigate(other + 9);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(other));
    QTest::qWait(300);
    QCOMPARE(code->function(), std::optional<Address>(function));
    QCOMPARE(code->verticalScrollBar()->value(), oldScroll);
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    QTest::keyClick(assembly->listing(), Qt::Key_Tab);
    QTRY_VERIFY(!code->loading() && code->function() == other);
    QCOMPARE(code->currentAddress(), std::optional<Address>(other + 9));
    QCOMPARE(code->commentTarget()->value("line").toInt(), 550);
    QTRY_VERIFY(code->hasFocus());

    // Missing mappings are reported without guessing a nearby source row.
    assembly->navigate(other + 5);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(other + 5));
    assembly->focusContent();
    QSignalSpy selected(code, &CodeText::addressSelected);
    QTRY_VERIFY(assembly->listing()->hasFocus());
    QTest::keyClick(assembly->listing(), Qt::Key_Tab);
    QTRY_COMPARE(selected.size(), 1);
    QCOMPARE(selected.first().at(1).toBool(), false);
    QCOMPARE(code->currentAddress(), std::optional<Address>(other + 9));

    // Two code panes can display the same representation. Reverse Tab must
    // return to the actual pane used, rather than the default source dock.
    const auto originalVersion = code->documentVersion();
    bench.action(ActionId::ViewLowIR)->trigger();
    auto *second = bench.codeView(QStringLiteral("low"));
    QVERIFY(second && second != view);
    QTRY_VERIFY(!view->isVisible());
    QCOMPARE(code->documentVersion(), originalVersion);
    QCOMPARE(code->currentAddress(), std::optional<Address>(other + 9));
    second->setRepresentation(QStringLiteral("source"));
    QTRY_VERIFY(!second->text()->loading());
    second->text()->setFocus();
    QTRY_VERIFY(second->text()->hasFocus());
    assembly->navigate(function + 9);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 9));
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    QTest::keyClick(assembly->listing(), Qt::Key_Tab);
    QTRY_VERIFY(second->text()->hasFocus() && !second->text()->loading());
    QCOMPARE(second->text()->function(), std::optional<Address>(function));
    QCOMPARE(second->text()->currentAddress(),
             std::optional<Address>(function + 9));
    QCOMPARE(code->function(), std::optional<Address>(other));
    QCOMPARE(code->currentAddress(), std::optional<Address>(other + 9));
    // Returning to the retained tab keeps the secondary instruction mapping,
    // including a reverse Tab to assembly, without reloading the document.
    auto *originalDock = bench.dockFor(view);
    QVERIFY(originalDock);
    originalDock->setAsCurrentTab();
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    QCOMPARE(code->documentVersion(), originalVersion);
    QCOMPARE(code->currentAddress(), std::optional<Address>(other + 9));
    QTest::keyClick(code, Qt::Key_Tab);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(other + 9));
  }

  void pseudocodeNamesAndCommentsEditTheirSource_data() {
    QTest::addColumn<QString>("representation");
    QTest::addColumn<bool>("native");
    QTest::addColumn<bool>("mapped");
    for (const auto &representation :
         {QStringLiteral("source"), QStringLiteral("c"),
          QStringLiteral("llvmc")})
      QTest::newRow(qPrintable(representation))
          << representation << false << false;
    QTest::newRow("mapped-source") << QStringLiteral("source") << false << true;
    if (!qEnvironmentVariable("NEVERD_CODE_NAV_WORKER").isEmpty() &&
        !qEnvironmentVariable("NEVERD_CODE_NAV_FILE").isEmpty()) {
      QTest::newRow("native-source")
          << QStringLiteral("source") << true << false;
      QTest::newRow("native-llvmc") << QStringLiteral("llvmc") << true << false;
    }
  }

  void savedSourceNoteRetainsItsOwnerOnAMappedRow() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-edits-mapped.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const auto function = Base + 0x140;
    bench.window->disassembly()->navigate(function);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading());
    // Simulate a note saved while an older engine could not map this line.
    bench.session.editCode({{"address", hexAddress(function)},
                            {"representation", "source"},
                            {"kind", "comment"},
                            {"line", 2},
                            {"anchor", "  int32_t v1 = v0 + function_22();"},
                            {"text", "old source note"}},
                           bench.session.epoch());
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() &&
                                 code->allText().contains("old source note"),
                             OpenTimeoutMs);
    code->setPreludeFolded(false);
    code->setCursorLine(2);
    QCOMPARE(code->currentAddress(), std::optional<Address>(function + 2));
    const auto target = code->commentTarget();
    QVERIFY(target.has_value());
    QVERIFY(!target->contains("mapped_address"));
    QCOMPARE(target->value("text").toString(),
             QStringLiteral("old source note"));
    auto edit = *target;
    edit["address"] = hexAddress(function);
    edit["representation"] = "source";
    edit["kind"] = "comment";
    edit["text"] = "updated source note";
    bench.session.editCode(edit, bench.session.epoch());
    QTRY_VERIFY_WITH_TIMEOUT(code->allText().contains("updated source note"),
                             OpenTimeoutMs);
    QVERIFY(!code->allText().contains("old source note"));
  }

  void pseudocodeNamesAndCommentsEditTheirSource() {
    QFETCH(QString, representation);
    QFETCH(bool, native);
    QFETCH(bool, mapped);
    QTemporaryDir directory;
    const auto input = qEnvironmentVariable("NEVERD_CODE_NAV_FILE");
    const auto path =
        native ? directory.filePath("acceptance.exe")
               : writeFixture(directory,
                              mapped ? QStringLiteral("code-edits-mapped.bin")
                              : representation == QLatin1String("c")
                                  ? QStringLiteral("code-edits-cpp.bin")
                                  : QStringLiteral("code-edits.bin"));
    if (native) {
      QVERIFY(QFile::copy(input, path));
      const auto pdb = QFileInfo(input).absolutePath() + "/" +
                       QFileInfo(input).completeBaseName() + ".pdb";
      if (QFileInfo::exists(pdb))
        QVERIFY(QFile::copy(pdb, directory.filePath("acceptance.pdb")));
    }
    Workbench bench(native ? qEnvironmentVariable("NEVERD_CODE_NAV_WORKER")
                           : QString::fromLocal8Bit(TEST_WORKER));
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    auto *functions = bench.functions();
    QVERIFY(functions);
    QTRY_VERIFY_WITH_TIMEOUT(functions->model().total() >= 2,
                             NativeOpenTimeoutMs);
    const auto function =
        native ? *functions->model().addressAt(0) : Base + 0x140;
    const auto elsewhere =
        native ? *functions->model().addressAt(1) : Base + 0x180;
    assembly->navigate(function);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(function));
    bench
        .action(representation == QLatin1String("llvmc")
                    ? ActionId::ViewLLVMC
                    : ActionId::ViewPseudocode)
        ->trigger();
    auto *view = bench.codeView(representation == QLatin1String("llvmc")
                                    ? QStringLiteral("llvmc")
                                    : QStringLiteral("source"));
    QVERIFY(view);
    if (representation == QLatin1String("c")) {
      view->setRepresentation(representation);
      QCOMPARE(view->representation(), representation);
    }
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(native ? "value" : "int32_t v1"),
        NativeOpenTimeoutMs);
    for (auto *button : view->findChildren<QToolButton *>())
      if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
        button->setChecked(true);
    QVERIFY(view->locked());
    assembly->navigate(elsewhere);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(elsewhere));

    int commentLine = -1;
    auto edit = [&](const QString &name, const QString &value,
                    bool comment = false) {
      bench.window->activateWindow();
      if (!QTest::qWaitForWindowActive(bench.window.get()))
        return false;
      code->setFocus();
      if (!QTest::qWaitFor([&] { return code->hasFocus(); }, OpenTimeoutMs))
        return false;
      code->setCursorLine(0);
      if (!code->findText(name, true))
        return false;
      if (comment) {
        const auto target = code->commentTarget();
        if (!target || !target->value("anchor").toString().contains(name))
          return false;
        commentLine = target->value("line").toInt(-1);
      }
      QTimer filler;
      bool accepted = false;
      connect(&filler, &QTimer::timeout, &filler, [&] {
        if (auto *dialog = qobject_cast<QInputDialog *>(
                QApplication::activeModalWidget())) {
          dialog->setTextValue(value);
          accepted = true;
          dialog->accept();
        }
      });
      filler.start(10);
      if (comment)
        QTest::keyClick(code->window()->windowHandle(), Qt::Key_Slash);
      else
        QTest::keyClick(code, Qt::Key_N);
      return QTest::qWaitFor([&] { return accepted; }, OpenTimeoutMs);
    };
    QVERIFY(edit(native ? QStringLiteral("value") : QStringLiteral("v0"),
                 QStringLiteral("input_value")));
    QTRY_VERIFY2_WITH_TIMEOUT(
        code->allText().contains(native ? "input_value" : "v1 = input_value +"),
        qPrintable(code->status() + '\n' + diagnostics.join('\n') + '\n' +
                   code->allText()),
        NativeOpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading(), OpenTimeoutMs);
    if (!native)
      QVERIFY(code->allText().contains("\"v0 v1 function_22\""));
    QCOMPARE(assembly->currentFunction(), std::optional<Address>(elsewhere));

    QVERIFY(
        edit(native ? QStringLiteral("sample") : QStringLiteral("function_22"),
             QStringLiteral("renamed_callee")));
    QTRY_VERIFY_WITH_TIMEOUT(code->allText().contains("renamed_callee("),
                             NativeOpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading(), OpenTimeoutMs);
    if (!native)
      QVERIFY(code->allText().contains("\"v0 v1 function_22\""));
    QVERIFY(edit(native ? QStringLiteral("return (") : QStringLiteral("v1 ="),
                 QString::fromUtf8("本行注释\nsecond */ line"), true));
    QTRY_VERIFY_WITH_TIMEOUT(
        code->allText().contains(QString::fromUtf8("本行注释")), OpenTimeoutMs);
    QVERIFY(code->allText().contains("second * / line"));
    QVERIFY(commentLine >= 0);
    QVERIFY(code->allText()
                .split('\n')
                .at(commentLine)
                .contains(QString::fromUtf8("本行注释")));
    QVERIFY(code->isVisible());
    QVERIFY(view->locked());
    if (mapped) {
      bool checked = false;
      bench.session.read("resolve", {{"query", hexAddress(function + 2)}}, code,
                         [&](const QJsonObject &p) {
                           checked = p.value("comment").toString().contains(
                               QString::fromUtf8("本行注释"));
                         });
      QTRY_VERIFY_WITH_TIMEOUT(checked, OpenTimeoutMs);
    }
    bench.action(ActionId::EditUndo)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->allText().contains(QString::fromUtf8("本行注释")),
        OpenTimeoutMs);
    bench.action(ActionId::EditRedo)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        code->allText().contains(QString::fromUtf8("本行注释")), OpenTimeoutMs);
    // Save keeps the project beside the binary; packing writes a database.
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.dirty(), OpenTimeoutMs);
    QVERIFY(!QFileInfo::exists(path + ".nddb"));
    // The file appears as the database starts to be written; the session
    // counts the write until it is done, and a restart before then asks to
    // save first.
    QSignalSpy packed(&bench.session, &Session::databaseSaved);
    bench.session.packDatabase(path + ".nddb");
    QTRY_COMPARE_WITH_TIMEOUT(packed.size(), 1, OpenTimeoutMs);
    QVERIFY(packed.first().first().toBool());
    QVERIFY(QFileInfo::exists(path + ".nddb"));
    QString databaseError;
    const auto stored = ProjectDatabase::read(path + ".nddb", &databaseError);
    QVERIFY2(stored.has_value(), qPrintable(databaseError));
    QVERIFY(stored->sidecars.contains(QStringLiteral(".neverd-code.json")));
    QVERIFY(stored->sidecars.value(QStringLiteral(".neverd-code.json"))
                .contains("input_value"));
    QTemporaryDir restored;
    const auto unpacked = ProjectDatabase::unpack(
        path + ".nddb", restored.path(), &databaseError);
    QVERIFY2(!unpacked.isEmpty(), qPrintable(databaseError));
    QCOMPARE(readAll(unpacked + ".neverd/code.json"),
             readAll(path + ".neverd/code.json"));
    bench.session.restart();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    assembly->navigate(function);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(function));
    view->showFunction(function);
    bench
        .action(representation == QLatin1String("llvmc")
                    ? ActionId::ViewLLVMC
                    : ActionId::ViewPseudocode)
        ->trigger();
    view->setRepresentation(representation);
    QTRY_VERIFY_WITH_TIMEOUT(
        code->allText().contains("input_value") &&
            code->allText().contains("renamed_callee(") &&
            code->allText().contains(QString::fromUtf8("本行注释")),
        NativeOpenTimeoutMs);
    if (native) {
      const auto capture = qEnvironmentVariable("NEVERD_CODE_NAV_CAPTURE_DIR");
      if (!capture.isEmpty()) {
        QFile source(capture + "/edits-" + representation + ".txt");
        QVERIFY(source.open(QIODevice::WriteOnly));
        QVERIFY(source.write(code->allText().toUtf8()) > 0);
        QVERIFY(bench.window->grab().save(capture + "/edits-" + representation +
                                          ".png"));
      }
    }
  }
  void localNameLatencyOnOptionalNativeInput() {
    const auto worker = qEnvironmentVariable("NEVERD_RENAME_BENCH_WORKER");
    const auto input = qEnvironmentVariable("NEVERD_RENAME_BENCH_INPUT");
    if (worker.isEmpty() || input.isEmpty())
      QSKIP("Set NEVERD_RENAME_BENCH_WORKER and NEVERD_RENAME_BENCH_INPUT");
    QTemporaryDir directory;
    const auto path = directory.filePath(QFileInfo(input).fileName());
    QVERIFY(QFile::copy(input, path));
    const auto pdb = qEnvironmentVariable("NEVERD_RENAME_BENCH_PDB");
    if (!pdb.isEmpty())
      QVERIFY(QFile::copy(pdb, directory.filePath(QFileInfo(pdb).fileName())));
    Session session(worker);
    LoadOptions options;
    options.analysis = false;
    session.open(path, options);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    std::optional<Address> function;
    session.read(
        "resolve",
        {{"query", qEnvironmentVariable("NEVERD_RENAME_BENCH_TARGET")}}, this,
        [&](const QJsonObject &reply) {
          function = addressValue(reply.value("address"));
        });
    QTRY_VERIFY_WITH_TIMEOUT(function.has_value(), OpenTimeoutMs);
    CodeText code(session);
    code.resize(900, 700);
    code.show();
    code.load(*function, "source", QJsonObject{});
    QTRY_VERIFY_WITH_TIMEOUT(!code.loading() && !code.allText().isEmpty(),
                             OpenTimeoutMs);
    code.setPreludeFolded(false);
    code.setCursorLine(0);
    QVERIFY(
        code.findText(qEnvironmentVariable("NEVERD_RENAME_BENCH_LOCAL"), true));
    const auto target = code.renameTarget();
    QVERIFY(target && target->value("kind") == "local");
    const auto uses = code.localReferences(*target);
    const auto gsRead =
        std::find_if(uses.cbegin(), uses.cend(), [](const auto &use) {
          return use.text.contains("__readgsqword") &&
                 use.text.contains("0x188");
        });
    QVERIFY(gsRead != uses.cend());
    code.setPreludeFolded(true);
    QVERIFY(code.selectLocalReference(*target, *gsRead));
    QCOMPARE(code.selectedText(), target->value("name").toString());
    const int cursor = code.cursorLine();
    const int top = code.verticalScrollBar()->value();
    auto edit = *target;
    edit["address"] = hexAddress(*function);
    edit["representation"] = "source";
    edit["kind"] = "name";
    edit["name"] = target->value("original");
    QElapsedTimer timer;
    timer.start();
    session.prepareCodeName(edit, session.epoch());
    QTRY_VERIFY_WITH_TIMEOUT(!session.queries().hasPending(), OpenTimeoutMs);
    qInfo() << "rename preparation during dialog ms:" << timer.elapsed();
    for (const auto &name : {QStringLiteral("qa_thread_current"),
                             QStringLiteral("qa_thread_value")}) {
      timer.restart();
      edit["name"] = name;
      session.editCode(edit, session.epoch());
      QTRY_VERIFY_WITH_TIMEOUT(!code.loading() && code.allText().contains(name),
                               OpenTimeoutMs);
      qInfo() << "rename to complete pseudocode refresh ms:" << timer.elapsed();
      QCOMPARE(code.cursorLine(), cursor);
      QCOMPARE(code.verticalScrollBar()->value(), top);
      QCOMPARE(code.selectedText(), name);
      QVERIFY(code.preludeFolded());
    }
  }

  void localNameRefreshKeepsEveryLineWhenTheBatchExceedsBytes() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    LoadOptions options;
    options.analysis = false;
    session.open(
        writeFixture(directory,
                     QStringLiteral("source-cache-refresh-budget.bin")),
        options);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    CodeText code(session);
    code.resize(800, 600);
    code.show();
    code.load(Base, "source");
    QTRY_VERIFY_WITH_TIMEOUT(!code.loading() && code.lineCount() == 600,
                             OpenTimeoutMs);
    const auto before = code.allText();
    const int beforeLines = code.lineCount();
    QStringList partialDocuments;
    QTimer observe;
    connect(&observe, &QTimer::timeout, this, [&] {
      if (code.loading() &&
          (code.allText() != before || code.lineCount() != beforeLines))
        partialDocuments.append(QString::number(code.lineCount()));
    });
    observe.start(1);
    code.setCursorLine(0);
    QVERIFY(code.findText("value", true));
    const auto target = code.renameTarget();
    QVERIFY(target && target->value("kind") == "local");
    const auto references = code.localReferences(*target);
    QVERIFY(!references.isEmpty());
    QVERIFY(code.selectLocalReference(*target, references.back()));
    QCOMPARE(code.selectedText(), QStringLiteral("value"));
    const int cursor = code.cursorLine();
    code.verticalScrollBar()->setValue(200);
    code.horizontalScrollBar()->setValue(70);
    const int top = code.verticalScrollBar()->value();
    const int across = code.horizontalScrollBar()->value();
    QSignalSpy rowsMoved(code.verticalScrollBar(), &QScrollBar::valueChanged);
    QSignalSpy columnsMoved(code.horizontalScrollBar(),
                            &QScrollBar::valueChanged);
    auto edit = *target;
    edit["address"] = hexAddress(Base);
    edit["representation"] = "source";
    edit["kind"] = "name";
    edit["name"] = "renamed_local";
    session.editCode(edit, session.epoch());
    QTRY_VERIFY_WITH_TIMEOUT(!code.loading() &&
                                 code.allText().contains("renamed_local"),
                             OpenTimeoutMs);
    auto expected = before;
    expected.replace("int value =", "int renamed_local =");
    expected.replace("return value;", "return renamed_local;");
    QCOMPARE(code.allText(), expected);
    QCOMPARE(code.lineCount(), 600);
    QVERIFY2(partialDocuments.isEmpty(),
             qPrintable(partialDocuments.join(',')));
    QCOMPARE(code.cursorLine(), cursor);
    QCOMPARE(code.verticalScrollBar()->value(), top);
    QCOMPARE(code.horizontalScrollBar()->value(), across);
    QCOMPARE(rowsMoved.count(), 0);
    QCOMPARE(columnsMoved.count(), 0);
    QCOMPARE(code.currentToken(), QStringLiteral("renamed_local"));
    QCOMPARE(code.selectedText(), QStringLiteral("renamed_local"));
  }

  void retiringANameRefreshCannotPublishItsBufferedPages_data() {
    QTest::addColumn<QString>("retirement");
    for (const auto *kind : {"cancel", "function", "close"})
      QTest::newRow(kind) << QString::fromLatin1(kind);
  }

  void retiringANameRefreshCannotPublishItsBufferedPages() {
    QFETCH(QString, retirement);
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    LoadOptions options;
    options.analysis = false;
    session.open(writeFixture(directory, "source-cache-refresh-budget.bin"),
                 options);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    CodeText code(session);
    code.resize(800, 600);
    code.show();
    code.load(Base, "source");
    QTRY_VERIFY_WITH_TIMEOUT(code.complete(), OpenTimeoutMs);
    const auto before = code.allText();
    QVERIFY(code.findText("value", true));
    const auto target = code.renameTarget();
    QVERIFY(target);
    auto edit = *target;
    edit["address"] = hexAddress(Base);
    edit["representation"] = "source";
    edit["kind"] = "name";
    edit["name"] = "retired_name";
    bool retired = false;
    connect(&session, &Session::transitionRequested, &session,
            [&] { session.resolveTransition(QStringLiteral("discard")); });
    connect(&session, &Session::revisionChanged, &code, [&] {
      if (!session.queries().localNamesOnly())
        return;
      QTimer::singleShot(0, &code, [&] {
        if (retirement == "cancel")
          code.cancel();
        else if (retirement == "function")
          code.load(Base + 16, "source");
        else
          session.closeFile();
        retired = true;
      });
    });
    session.editCode(edit, session.epoch());
    QTRY_VERIFY_WITH_TIMEOUT(retired, OpenTimeoutMs);
    if (retirement == "function") {
      QTRY_VERIFY_WITH_TIMEOUT(code.complete(), OpenTimeoutMs);
      QCOMPARE(code.function(), std::optional<Address>(Base + 16));
      QVERIFY(code.allText().startsWith("int function_1("));
    } else if (retirement == "cancel") {
      QCOMPARE(code.allText(), before);
      QVERIFY(!code.complete());
      QVERIFY(!code.renameTarget());
    } else {
      QTRY_VERIFY_WITH_TIMEOUT(!code.function(), OpenTimeoutMs);
      QVERIFY(code.allText().isEmpty());
    }
    QTest::qWait(100);
    QVERIFY(!code.allText().contains("retired_name"));
  }

  void localNameEditsReuseTheIdleAnalysisReplica() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("source-cache.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.analysisComplete(), OpenTimeoutMs);
    QObject reader;
    QJsonObject first;
    const QJsonObject payload{{"address", hexAddress(Base)},
                              {"representation", "source"}};
    bench.session.read("decompile", payload, &reader,
                       [&](const QJsonObject &page) { first = page; });
    QTRY_VERIFY_WITH_TIMEOUT(!first.isEmpty(), OpenTimeoutMs);
    QVERIFY(!first.value("fixture_instance").toString().isEmpty());
    QJsonObject target;
    for (const auto &value : first.value("code_names").toArray()) {
      const auto item = value.toObject();
      if (item.value("kind") == "local" && item.value("original") == "value") {
        target = item;
        break;
      }
    }
    QVERIFY(!target.isEmpty());
    QJsonObject edit{{"address", hexAddress(Base)},
                     {"representation", "source"},
                     {"kind", "name"},
                     {"original", target.value("original")},
                     {"identity", target.value("identity")},
                     {"name", "value"}};
    bench.session.prepareCodeName(edit, bench.session.epoch());
    // Preparation can run under the rename dialog; it commits no history.
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.queries().hasPending(),
                             OpenTimeoutMs);
    QVERIFY(!bench.session.canUndo());
    auto *functions = bench.functions();
    QTRY_VERIFY_WITH_TIMEOUT(!functions->model().loading(), OpenTimeoutMs);
    functions->table()->setCurrentIndex(functions->model().index(20, 0));
    const auto selectedFunction = functions->currentAddress();
    auto *listing = bench.window->disassembly()->listing();
    GraphView graph(bench.session);
    graph.showFunction(Base);
    QTRY_VERIFY_WITH_TIMEOUT(
        graph.loaded() && graph.currentAddress() == Base &&
            !bench.session.analysisQueries(payload, &graph).hasPending(),
        OpenTimeoutMs);
    // Count only changes caused by the rename. The initial listing can still
    // settle after the function model and graph have answered.
    QTRY_VERIFY_WITH_TIMEOUT(listing->currentItem().has_value() &&
                                 !listing->jumpPending() &&
                                 !bench.session.queries().hasPending(),
                             OpenTimeoutMs);
    // Initial discovery and navigation may retire the first source replica.
    // Establish the cache baseline after that setup, then require this same
    // idle replica to survive both display-name commits.
    first = {};
    bench.session.read("decompile", payload, &reader,
                       [&](const QJsonObject &page) { first = page; });
    QTRY_VERIFY_WITH_TIMEOUT(!first.isEmpty(), OpenTimeoutMs);
    QSignalSpy listResets(&functions->model(), &QAbstractItemModel::modelReset);
    QSignalSpy listingMoves(listing, &ListingView::jumpSettled);
    QSignalSpy graphChanges(&graph, &GraphView::viewChanged);
    for (const auto &name :
         {QStringLiteral("current_thread"), QStringLiteral("thread_state")}) {
      QTRY_VERIFY_WITH_TIMEOUT(
          !bench.session.queries().hasPending() &&
              !bench.session.analysisQueries(payload, &reader).hasPending(),
          OpenTimeoutMs);
      const auto revision = bench.session.revision();
      edit["name"] = name;
      bench.session.editCode(edit, bench.session.epoch());
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.revision() != revision,
                               OpenTimeoutMs);
      QJsonObject current;
      bench.session.read("decompile", payload, &reader,
                         [&](const QJsonObject &page) { current = page; });
      QTRY_VERIFY_WITH_TIMEOUT(!current.isEmpty(), OpenTimeoutMs);
      QVERIFY(current.value("text").toString().contains(name + " = 1"));
      for (const auto *field :
           {"fixture_instance", "fixture_prepares", "fixture_pages"})
        QCOMPARE(current.value(field), first.value(field));
      QCOMPARE(listResets.count(), 0);
      QCOMPARE(listingMoves.count(), 0);
      QCOMPARE(graphChanges.count(), 0);
      QCOMPARE(functions->currentAddress(), selectedFunction);
    }
    // A source comment still takes the existing full invalidation path.
    const auto revision = bench.session.revision();
    const auto row = first.value("rows").toArray().first().toObject();
    bench.session.editCode({{"address", hexAddress(Base)},
                            {"representation", "source"},
                            {"kind", "comment"},
                            {"line", 0},
                            {"anchor", row.value("code_anchor")},
                            {"text", "checked source comment"}},
                           bench.session.epoch());
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.revision() != revision,
                             OpenTimeoutMs);
    QJsonObject changed;
    bench.session.read("decompile", payload, &reader,
                       [&](const QJsonObject &page) { changed = page; });
    QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), OpenTimeoutMs);
    QVERIFY(
        changed.value("text").toString().contains("checked source comment"));
    QVERIFY(changed.value("fixture_instance") !=
            first.value("fixture_instance"));
  }

  void functionsClicksKeepTheLoadedListAndDocument_data() {
    QTest::addColumn<QString>("worker");
    QTest::addColumn<QString>("input");
    QTest::newRow("fixture")
        << QString::fromLocal8Bit(TEST_WORKER) << QString();
    const auto worker = qEnvironmentVariable("NEVERD_RENAME_BENCH_WORKER");
    const auto input = qEnvironmentVariable("NEVERD_RENAME_BENCH_INPUT");
    if (!worker.isEmpty() && !input.isEmpty())
      QTest::newRow("native") << worker << input;
  }

  void backgroundFunctionDiscoveryKeepsSelection_data() {
    QTest::addColumn<bool>("sorted");
    QTest::newRow("address-order") << false;
    QTest::newRow("sorted-filtered") << true;
  }

  void backgroundDiscoveryKeepsSelectionBeyondCachedPages_data() {
    QTest::addColumn<bool>("sorted");
    QTest::addColumn<bool>("retire");
    QTest::newRow("address-order") << false << false;
    QTest::newRow("sorted-filtered") << true << false;
    QTest::newRow("filter-retires-address-refresh") << false << true;
  }

  void backgroundDiscoveryKeepsSelectionBeyondCachedPages() {
    QFETCH(bool, sorted);
    QFETCH(bool, retire);
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    LoadOptions options;
    options.analysis = false;
    session.open(writeFixture(directory, "chooser-refresh.bin"), options);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    AddressSpace space;
    ChooserModel model(session, space, ChooserKind::Functions);
    model.reload();
    QTRY_COMPARE(model.total(), 600);
    QTRY_VERIFY(!model.loading());
    if (sorted) {
      model.setFilter("function_");
      model.sort(0, Qt::DescendingOrder);
      QTRY_COMPARE(model.total(), 600);
      QTRY_VERIFY(!model.loading());
    }
    // Only page zero is loaded. Discovery moves its final row onto page one.
    QPersistentModelIndex selected(model.index(255, 0));
    QItemSelectionModel selection(&model);
    selection.setCurrentIndex(model.index(5, 0), QItemSelectionModel::Select);
    selection.select(model.index(255, 0), QItemSelectionModel::Select);
    const auto address = model.addressAt(selected.row());
    QVERIFY(address);
    QVERIFY(model.rowObject(256).isEmpty());
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    bool retired = false;
    if (retire)
      connect(&model, &QAbstractItemModel::layoutChanged, this, [&] {
        if (!retired) {
          retired = true;
          model.setFilter("function_599");
        }
      });
    QVERIFY(!writeFixture(directory, "chooser-refresh.bin.discover").isEmpty());
    auto background = session.background();
    background["functions"] = 601;
    SessionTestAccess::receive(
        session, {{"type", "heartbeat"},
                  {"project_id", session.queries().projectId()},
                  {"background_sequence",
                   SessionTestAccess::nextBackgroundSequence(session)},
                  {"background", background}});
    if (retire) {
      QTRY_VERIFY(retired);
      QTRY_COMPARE(model.total(), 1);
      QTRY_VERIFY(!model.loading());
      QCOMPARE(model.addressAt(0), std::optional<Address>(Base + 599 * 16));
      return;
    }
    QTRY_COMPARE_WITH_TIMEOUT(model.total(), 601, OpenTimeoutMs);
    QTRY_VERIFY(!model.loading());
    QVERIFY(selected.isValid());
    QCOMPARE(selected.row(), 256);
    QCOMPARE(model.addressAt(selected.row()), address);
    QCOMPARE(selection.currentIndex().row(), 6);
    QVERIFY(selection.isSelected(model.index(6, 0)));
    QVERIFY(selection.isSelected(model.index(256, 0)));
    QCOMPARE(selection.selectedIndexes().size(), 2);
    QCOMPARE(resets.count(), 0);
  }

  void backgroundFunctionDiscoveryKeepsSelection() {
    QFETCH(bool, sorted);
    QTemporaryDir directory;
    Workbench bench;
    const auto path = writeFixture(directory, "chooser-refresh.bin");
    bench.session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *view = bench.functions();
    auto &model = view->model();
    auto *table = view->table();
    QTRY_COMPARE(model.total(), 600);
    QTRY_VERIFY(!bench.session.busy());
    if (sorted) {
      model.setFilter("function_");
      model.sort(0, Qt::DescendingOrder);
      QTRY_COMPARE(model.total(), 600);
      QTRY_VERIFY(!model.loading());
    }
    model.data(model.index(257, 0), Qt::DisplayRole);
    QTRY_VERIFY(!model.rowObject(257).isEmpty());
    const int row = sorted ? 20 : 255;
    const auto selected = model.addressAt(row);
    const auto alsoSelected = model.addressAt(257);
    QVERIFY(selected && alsoSelected);
    table->setCurrentIndex(model.index(row, 0));
    table->selectionModel()->select(model.index(257, 0),
                                    QItemSelectionModel::Select |
                                        QItemSelectionModel::Rows);
    table->scrollTo(model.index(row, 0));
    const int top = table->verticalScrollBar()->value();
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy layouts(&model, &QAbstractItemModel::layoutChanged);
    QVERIFY(!writeFixture(directory, "chooser-refresh.bin.discover").isEmpty());
    auto background = bench.session.background();
    background["functions"] = 601;
    SessionTestAccess::receive(
        bench.session,
        {{"type", "heartbeat"},
         {"project_id", bench.session.queries().projectId()},
         {"background_sequence",
          SessionTestAccess::nextBackgroundSequence(bench.session)},
         {"background", background}});
    QTRY_COMPARE_WITH_TIMEOUT(model.total(), 601, OpenTimeoutMs);
    QTRY_VERIFY(!model.loading());
    QCOMPARE(resets.count(), 0);
    QVERIFY(layouts.count() > 0);
    QCOMPARE(view->currentAddress(), selected);
    QSet<Address> addresses;
    for (const auto &index : table->selectionModel()->selectedRows())
      if (const auto address = model.addressAt(index.row()))
        addresses.insert(*address);
    QCOMPARE(addresses, (QSet<Address>{*selected, *alsoSelected}));
    QCOMPARE(table->verticalScrollBar()->value(), top);
    QCOMPARE(model.filter(), sorted ? QStringLiteral("function_") : QString());
    // A filter change supersedes an in-flight background refresh.
    emit bench.session.functionsChanged();
    model.setFilter("function_599");
    QTRY_COMPARE_WITH_TIMEOUT(model.total(), 1, OpenTimeoutMs);
    QTRY_COMPARE(model.addressAt(0), std::optional<Address>(Base + 599 * 16));
  }

  void functionsClicksKeepTheLoadedListAndDocument() {
    QFETCH(QString, worker);
    QFETCH(QString, input);
    QTemporaryDir directory;
    auto path = writeFixture(directory, QStringLiteral("source-cache.bin"));
    if (!input.isEmpty()) {
      path = directory.filePath(QFileInfo(input).fileName());
      QVERIFY(QFile::copy(input, path));
      const auto pdb = qEnvironmentVariable("NEVERD_RENAME_BENCH_PDB");
      if (!pdb.isEmpty())
        QVERIFY(
            QFile::copy(pdb, directory.filePath(QFileInfo(pdb).fileName())));
    }
    Workbench bench(worker);
    bench.session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), NativeOpenTimeoutMs);
    auto *functions = bench.functions();
    QVERIFY(functions);
    auto *table = functions->table();
    auto &model = functions->model();
    QTRY_VERIFY_WITH_TIMEOUT(!model.rowObject(20).isEmpty(),
                             NativeOpenTimeoutMs);
    // Wait for initial discovery before attributing a change to clicking.
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.busy(), NativeOpenTimeoutMs);
    QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
    QSignalSpy revisions(&bench.session, &Session::revisionChanged);
    QSignalSpy discoveries(&bench.session, &Session::functionsChanged);
    connect(&model, &QAbstractItemModel::modelReset, this, [&] {
      qInfo() << "function list reset: revision" << bench.session.revision()
              << "functions" << bench.session.background().value("functions")
              << "revision events" << revisions.count() << "inventory events"
              << discoveries.count();
    });
    for (const int row : {8, 16, 20}) {
      const auto index = model.index(row, 0);
      const auto target = model.addressAt(row);
      QVERIFY(target);
      table->scrollTo(index);
      const auto cell =
          table->visualRect(index).intersected(table->viewport()->rect());
      QVERIFY(!cell.isEmpty());
      QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                        cell.center());
      QCOMPARE(table->currentIndex(), index);
      QTest::keyClick(table, Qt::Key_Return);
      QTRY_COMPARE_WITH_TIMEOUT(bench.window->disassembly()->currentFunction(),
                                target, NativeOpenTimeoutMs);
      QTest::qWait(100);
      QCOMPARE(table->currentIndex(), index);
    }
    qInfo() << "function activation: list resets" << resets.count()
            << "revisions" << revisions.count() << "discoveries"
            << discoveries.count();
    QCOMPARE(resets.count(), 0);
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView("source");
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(code->complete(), NativeOpenTimeoutMs);
    qInfo() << "after pseudocode: list resets" << resets.count() << "revisions"
            << revisions.count() << "discoveries" << discoveries.count();
    const auto version = code->documentVersion();
    const auto index = table->currentIndex();
    table->setFocus();
    QTest::keyClick(table, Qt::Key_Return);
    QTest::qWait(100);
    QCOMPARE(code->documentVersion(), version);
    QCOMPARE(table->currentIndex(), index);
    QCOMPARE(resets.count(), 0);
  }

  void independentAnalysisViewsRunConcurrently() {
    QTemporaryDir directory;
    Workbench bench;
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-slow.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QObject slowOwner, fastOwner;
    bool slowFinished = false, fastFinished = false;
    bench.session.read(
        "decompile",
        {{"address", hexAddress(Base)}, {"representation", "source"}},
        &slowOwner, [&](const QJsonObject &) { slowFinished = true; });
    QTRY_VERIFY_WITH_TIMEOUT(
        diagnostics.join('\n').contains("fixture slow decompile started"),
        5000);
    bench.session.read(
        "decompile",
        {{"address", hexAddress(Base + 16)}, {"representation", "source"}},
        &fastOwner, [&](const QJsonObject &page) {
          fastFinished = !page.value("text").toString().isEmpty();
        });
    // A pinned slow function must not serialize another analysis window.
    QTRY_VERIFY_WITH_TIMEOUT(fastFinished, 1500);
    QVERIFY(!slowFinished);
    bench.session.cancelReads();
  }

  void cancellingOneAnalysisLeavesTheOtherRunning() {
    QTemporaryDir directory;
    Workbench bench;
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-parallel.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QObject firstOwner, secondOwner, replacementOwner;
    bool firstFinished = false, secondFinished = false,
         replacementFinished = false;
    bench.session.read(
        "decompile",
        {{"address", hexAddress(Base)}, {"representation", "source"}},
        &firstOwner, [&](const QJsonObject &) { firstFinished = true; });
    bench.session.read(
        "decompile",
        {{"address", hexAddress(Base + 16)}, {"representation", "source"}},
        &secondOwner, [&](const QJsonObject &) { secondFinished = true; });
    QTRY_VERIFY_WITH_TIMEOUT(
        diagnostics.join('\n').contains("fixture parallel decompile " +
                                        hexAddress(Base) + " started") &&
            diagnostics.join('\n').contains("fixture parallel decompile " +
                                            hexAddress(Base + 16) + " started"),
        5000);
    bench.session.cancelAnalysisReads(&firstOwner);
    bench.session.read(
        "decompile",
        {{"address", hexAddress(Base + 32)}, {"representation", "source"}},
        &replacementOwner,
        [&](const QJsonObject &) { replacementFinished = true; });
    QTRY_VERIFY_WITH_TIMEOUT(replacementFinished, 1500);
    QTRY_VERIFY_WITH_TIMEOUT(secondFinished, 5000);
    QVERIFY(!firstFinished);
  }

  void ownerDoesNotWarmPipelinesUsedOnlyByAnalysisReplicas() {
    // Reference indexing still runs in the owner. Pipeline warmup there
    // cannot accelerate a view prepared in a different analysis Session.
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    const auto preparation = [&] {
      return session.background()
          .value("warm")
          .toObject()
          .value("state")
          .toString();
    };
    session.open(writeFixture(directory, QStringLiteral("prepared.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(preparation(), QStringLiteral("off"),
                              OpenTimeoutMs);
    QJsonObject initialSnapshot;
    session.read(
        "analysis_snapshot", {}, this,
        [&](const QJsonObject &payload) { initialSnapshot = payload; });
    QTRY_VERIFY_WITH_TIMEOUT(initialSnapshot.contains("open"), OpenTimeoutMs);
    QVERIFY(initialSnapshot.value("open")
                .toObject()
                .value("step_discovery")
                .toBool());
    LoadOptions options;
    options.analysis = false;
    session.open(writeFixture(directory, QStringLiteral("unprepared.bin")),
                 options);
    QTRY_COMPARE_WITH_TIMEOUT(session.fileName(),
                              QStringLiteral("unprepared.bin"), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(preparation(), QStringLiteral("off"),
                              OpenTimeoutMs);
    QJsonObject disabledSnapshot;
    session.read(
        "analysis_snapshot", {}, this,
        [&](const QJsonObject &payload) { disabledSnapshot = payload; });
    QTRY_VERIFY_WITH_TIMEOUT(disabledSnapshot.contains("open"), OpenTimeoutMs);
    QVERIFY(!disabledSnapshot.value("open")
                 .toObject()
                 .value("step_discovery")
                 .toBool());
  }

  void prefetchedSourceAndForegroundUseTheSameAnalysisReplica() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(
        writeFixture(directory, QStringLiteral("pseudocode-parallel.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    AnalysisPool pool(session.queries(), QString::fromLocal8Bit(TEST_WORKER));
    QStringList diagnostics;
    connect(&pool, &AnalysisPool::diagnostic, this,
            [&](const QString &text) { diagnostics.append(text); });
    pool.prefetchSource(Base);
    QTRY_VERIFY_WITH_TIMEOUT(
        diagnostics.join('\n').contains("fixture parallel decompile " +
                                        hexAddress(Base) + " started"),
        OpenTimeoutMs);
    QObject owner;
    const QJsonObject payload{{"address", hexAddress(Base + 32)},
                              {"representation", "source"}};
    auto &replica = pool.queries(payload, &owner);
    QCOMPARE(&replica, &pool.externalQueries());
    bool done = false;
    replica.subscribe({"decompile", payload}, &owner,
                      [&](const QJsonObject &response) {
                        done = response.value("status") == QLatin1String("ok");
                      });
    QTRY_VERIFY_WITH_TIMEOUT(done, OpenTimeoutMs);
    const auto preparations = diagnostics.size();
    const QJsonObject cached{{"address", hexAddress(Base)},
                             {"representation", "source"},
                             {"source_options", QJsonObject{}},
                             {"offset", 0},
                             {"limit", 1}};
    done = false;
    pool.queries(cached, &owner)
        .subscribe({"decompile", cached}, &owner,
                   [&](const QJsonObject &response) {
                     done = response.value("status") == QLatin1String("ok") &&
                            !response.value("payload")
                                 .toObject()
                                 .value("text")
                                 .toString()
                                 .isEmpty();
                   });
    QTRY_VERIFY_WITH_TIMEOUT(done, OpenTimeoutMs);
    QCOMPARE(diagnostics.size(), preparations);
  }

  void analysisReplicaWaitsWithoutBlockingOwnerReads_data() {
    QTest::addColumn<bool>("cancel");
    QTest::newRow("release") << false;
    QTest::newRow("cancel-before-release") << true;
  }

  void analysisReplicaWaitsWithoutBlockingOwnerReads() {
    QFETCH(bool, cancel);
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(writeFixture(directory, QStringLiteral("snapshot-wait.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    AnalysisPool pool(session.queries(), QString::fromLocal8Bit(TEST_WORKER));
    pool.setSnapshotBlocked(true);
    QStringList diagnostics;
    connect(&pool, &AnalysisPool::diagnostic, this,
            [&](const QString &text) { diagnostics.append(text); });
    QObject owner;
    const QJsonObject payload{{"address", hexAddress(Base)},
                              {"representation", "source"}};
    auto &replica = pool.queries(payload, &owner);
    bool done = false;
    replica.subscribe({"decompile", payload}, &owner,
                      [&](const QJsonObject &response) {
                        done = response.value("status") == QLatin1String("ok");
                      });
    // Round-trip a read through the writable owner while the analysis job is
    // queued. No replica may start before discovery's snapshot is released.
    bool ownerDone = false;
    session.read("functions", {{"limit", 1}}, this,
                 [&](const QJsonObject &) { ownerDone = true; });
    QTRY_VERIFY_WITH_TIMEOUT(ownerDone, OpenTimeoutMs);
    QVERIFY(!done);
    QVERIFY(diagnostics.isEmpty());
    QCOMPARE(replica.pendingReadCount(), 1);
    if (cancel) {
      pool.unsubscribeOwner(&owner);
      QTRY_COMPARE(replica.pendingReadCount(), 0);
    }
    pool.setSnapshotBlocked(false);
    if (cancel) {
      QTest::qWait(50);
      QVERIFY(!done);
      QVERIFY(diagnostics.isEmpty());
    } else {
      QTRY_VERIFY_WITH_TIMEOUT(done, OpenTimeoutMs);
    }
  }

  void prefetchedSourceIsRetiredWhenTheProjectChanges() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(
        writeFixture(directory, QStringLiteral("pseudocode-slow.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    AnalysisPool pool(session.queries(), QString::fromLocal8Bit(TEST_WORKER));
    QStringList diagnostics;
    connect(&pool, &AnalysisPool::diagnostic, this,
            [&](const QString &text) { diagnostics.append(text); });
    pool.prefetchSource(Base);
    QTRY_VERIFY_WITH_TIMEOUT(
        diagnostics.join('\n').contains("fixture slow decompile started"),
        OpenTimeoutMs);
    session.open(writeFixture(directory, QStringLiteral("replacement.bin")));
    QTRY_COMPARE_WITH_TIMEOUT(session.fileName(),
                              QStringLiteral("replacement.bin"), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QObject owner;
    const QJsonObject payload{{"address", hexAddress(Base + 32)},
                              {"representation", "source"}};
    bool done = false;
    pool.queries(payload, &owner)
        .subscribe({"decompile", payload}, &owner,
                   [&](const QJsonObject &response) {
                     done = response.value("status") == QLatin1String("ok");
                   });
    QTRY_VERIFY_WITH_TIMEOUT(done, OpenTimeoutMs);
    QCOMPARE(session.fileName(), QStringLiteral("replacement.bin"));
  }

  void cancelledPrefetchDoesNotHoldItsLane() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(
        writeFixture(directory, QStringLiteral("pseudocode-slow.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    AnalysisPool pool(session.queries(), QString::fromLocal8Bit(TEST_WORKER));
    QStringList diagnostics;
    connect(&pool, &AnalysisPool::diagnostic, this,
            [&](const QString &text) { diagnostics.append(text); });
    pool.prefetchSource(Base);
    QTRY_VERIFY_WITH_TIMEOUT(
        diagnostics.join('\n').contains("fixture slow decompile started"),
        OpenTimeoutMs);
    // Cancel retires the prefetch without completing it. A read queued on
    // its lane afterwards is not that preparation: an independent view still
    // takes the other lane instead of waiting behind it.
    pool.cancelReads({});
    QObject first, second;
    const QJsonObject firstPayload{{"address", hexAddress(Base + 32)},
                                   {"representation", "source"}};
    auto &firstLane = pool.queries(firstPayload, &first);
    firstLane.subscribe({"decompile", firstPayload}, &first,
                        [](const QJsonObject &) {});
    QCOMPARE(firstLane.pendingReadCount(), 1);
    const QJsonObject secondPayload{{"address", hexAddress(Base + 64)},
                                    {"representation", "source"}};
    QVERIFY(&pool.queries(secondPayload, &second) != &firstLane);
  }

  void heartbeatsOfAnotherProjectLeaveTheSessionAlone() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(
        writeFixture(directory, QStringLiteral("heartbeat-project.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    const auto heartbeat = [&](const QString &project) {
      return QJsonObject{
          {"type", "heartbeat"},
          {"protocol_major", 1},
          {"project_id", project},
          {"revision", session.revision()},
          {"background_sequence",
           SessionTestAccess::nextBackgroundSequence(session)},
          {"background", QJsonObject{{"state", "replayed"},
                                     {"functions", 4242},
                                     {"generation", "replayed"},
                                     {"discovery_complete", true}}}};
    };
    // The worker built this before the reply that opened the project, and
    // sent it after: its state, discovery completion included, is not ours.
    SessionTestAccess::receive(session,
                               heartbeat(QStringLiteral("previous-project")));
    QVERIFY(session.background().value("state") != QLatin1String("replayed"));
    SessionTestAccess::receive(session,
                               heartbeat(session.queries().projectId()));
    QCOMPARE(session.background().value("state").toString(),
             QStringLiteral("replayed"));
  }

  void replySnapshotsRejectOlderHeartbeatsInTheSameProject() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(writeFixture(directory, QStringLiteral("snapshots.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.indexReady() && !session.busy(),
                             OpenTimeoutMs);
    const auto sequence =
        SessionTestAccess::nextBackgroundSequence(session).toULongLong();
    QJsonObject snapshot{
        {"type", "heartbeat"},
        {"project_id", session.queries().projectId()},
        {"revision", session.revision()},
        {"background_sequence", QString::number(sequence)},
        {"background", QJsonObject{{"generation", "reply-generation"},
                                   {"state", "ready"},
                                   {"functions", 600},
                                   {"discovery_complete", true}}}};
    emit session.queries().backgroundSnapshotReady(snapshot);
    QCOMPARE(session.generation(), QStringLiteral("reply-generation"));
    auto stale = snapshot;
    stale["background"] = QJsonObject{{"generation", "stale-generation"}};
    for (const auto &value : {QString::number(sequence - 1),
                              QString::number(sequence), QString("invalid")}) {
      stale["background_sequence"] = value;
      SessionTestAccess::receive(session, stale);
      QCOMPARE(session.generation(), QStringLiteral("reply-generation"));
    }
    stale["background_sequence"] = QString::number(sequence + 1);
    stale["revision"] = "another-revision";
    SessionTestAccess::receive(session, stale);
    QCOMPARE(session.generation(), QStringLiteral("reply-generation"));
    stale["revision"] = session.revision();
    SessionTestAccess::receive(session, stale);
    QCOMPARE(session.generation(), QStringLiteral("stale-generation"));
  }

  void analysisPagesKeepTheirReplicaWhenAnotherFunctionQueues() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(
        writeFixture(directory, QStringLiteral("pseudocode-parallel.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QObject firstOwner, secondOwner, thirdOwner;
    const QJsonObject first{{"address", hexAddress(Base)},
                            {"representation", "source"}};
    auto *firstReplica = &session.analysisQueries(first, &firstOwner);
    int completed = 0;
    session.read("decompile", first, &firstOwner,
                 [&](const QJsonObject &) { ++completed; });
    session.read(
        "decompile",
        {{"address", hexAddress(Base + 16)}, {"representation", "source"}},
        &secondOwner, [&](const QJsonObject &) { ++completed; });
    session.read(
        "decompile",
        {{"address", hexAddress(Base + 32)}, {"representation", "source"}},
        &thirdOwner, [&](const QJsonObject &) { ++completed; });
    auto next = first;
    next["offset"] = 512;
    QCOMPARE(&session.analysisQueries(next, &firstOwner), firstReplica);
    session.read("decompile", next, &firstOwner, [&](const QJsonObject &page) {
      QVERIFY(page.value("text").toString().contains("code line 699"));
      ++completed;
    });
    QTRY_COMPARE_WITH_TIMEOUT(completed, 4, 8000);
  }

  void changingProjectsRetiresBothAnalysisReplicas() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    QStringList diagnostics;
    connect(&session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    session.open(
        writeFixture(directory, QStringLiteral("pseudocode-parallel.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QObject firstOwner, secondOwner;
    int retired = 0;
    const auto oldReply = [&](const QJsonObject &response) {
      QCOMPARE(response.value("status").toString(), QStringLiteral("error"));
      QCOMPARE(response.value("error").toObject().value("code").toString(),
               QStringLiteral("worker_stopped"));
      ++retired;
    };
    for (const auto &[address, owner] :
         {std::pair{Base, &firstOwner}, std::pair{Base + 16, &secondOwner}}) {
      const QJsonObject payload{{"address", hexAddress(address)},
                                {"representation", "source"}};
      session.analysisQueries(payload, owner)
          .subscribe({"decompile", payload}, owner, oldReply);
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        diagnostics.join('\n').contains("fixture parallel decompile " +
                                        hexAddress(Base) + " started") &&
            diagnostics.join('\n').contains("fixture parallel decompile " +
                                            hexAddress(Base + 16) + " started"),
        5000);
    const auto nextPath =
        writeFixture(directory, QStringLiteral("pseudocode-import.bin"));
    session.open(nextPath);
    QTRY_COMPARE_WITH_TIMEOUT(retired, 2, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded() && session.filePath() == nextPath,
                             OpenTimeoutMs);
    int fresh = 0;
    for (const auto &[address, owner] :
         {std::pair{Base, &firstOwner}, std::pair{Base + 16, &secondOwner}})
      session.read(
          "decompile",
          {{"address", hexAddress(address)}, {"representation", "source"}},
          owner, [&](const QJsonObject &page) {
            QVERIFY(page.value("text").toString().contains("function_22"));
            ++fresh;
          });
    QTRY_COMPARE_WITH_TIMEOUT(fresh, 2, OpenTimeoutMs);
    QCOMPARE(retired, 2);
  }

  void externalGraphPagesKeepTheirSnapshotAcrossQueuedFunctions() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(
        writeFixture(directory, QStringLiteral("pseudocode-parallel.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QSignalSpy replies(&session, &Session::externalResponse);
    session.externalQuery("summary", "cfg_summary",
                          {{"address", hexAddress(Base)}}, {});
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, OpenTimeoutMs);
    const auto summary = replies.front()[1].toJsonObject();
    QCOMPARE(summary.value("status").toString(), QStringLiteral("ok"));
    const auto layout =
        summary.value("payload").toObject().value("layout_revision");
    QVERIFY(!layout.toString().isEmpty());
    for (int i = 1; i <= 3; ++i)
      session.externalQuery(QString::number(i), "decompile",
                            {{"address", hexAddress(Base + 16 * i)},
                             {"representation", "source"}},
                            {});
    session.externalQuery("viewport", "cfg_viewport",
                          {{"address", hexAddress(Base)},
                           {"layout_revision", layout},
                           {"x", 0},
                           {"y", 0},
                           {"width", 1000},
                           {"height", 1000}},
                          {});
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 5, OpenTimeoutMs);
    QCOMPARE(replies.back()[0].toString(), QStringLiteral("viewport"));
    const auto viewport = replies.back()[1].toJsonObject();
    QCOMPARE(viewport.value("status").toString(), QStringLiteral("ok"));
    QCOMPARE(viewport.value("payload").toObject().value("layout_revision"),
             layout);
  }

  void slowPseudocodeKeepsBrowsingUntilExplicitTab() {
    QTemporaryDir directory;
    Workbench bench;
    const auto path =
        writeFixture(directory, QStringLiteral("pseudocode-slow.bin"));
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base));
    QVERIFY(!bench.codeView(QStringLiteral("source")));
    QTest::qWait(250);
    QVERIFY(!diagnostics.join('\n').contains("fixture slow decompile started"));
    bench.session.setComment(Base, QStringLiteral("unsaved comment"));
    QTRY_VERIFY(bench.session.dirty());
    bench.action(ActionId::JumpPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    QTRY_VERIFY(view->isVisible() && bench.window->disassembly()->isVisible());
    QVERIFY(view->mapToGlobal(QPoint()).x() >
            bench.window->disassembly()->mapToGlobal(QPoint()).x());
    QTRY_VERIFY_WITH_TIMEOUT(
        diagnostics.join('\n').contains("fixture slow decompile started"),
        5000);
    // An uncached page and listing must finish while the 30-second engine
    // call is still running, not just leave the Qt event loop responsive.
    bool functions = false, listing = false;
    bench.session.read("functions", {{"offset", 512}, {"limit", 32}}, this,
                       [&](const QJsonObject &p) {
                         functions = !p.value("items").toArray().isEmpty();
                       });
    bench.session.read("listing",
                       {{"address", hexAddress(Base + 16)}, {"after", 16}},
                       this, [&](const QJsonObject &) { listing = true; });
    QTRY_VERIFY_WITH_TIMEOUT(functions && listing, 1500);
    QVERIFY(view->text()->loading());
    bench.window->disassembly()->navigate(Base + 16);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 16));
    QTest::qWait(300);
    QCOMPARE(view->text()->function(), std::optional<Address>(Base));
    QVERIFY(view->text()->loading());
    bench.window->disassembly()->focusContent();
    bench.action(ActionId::JumpPseudocode)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(view->text()->function(),
                              std::optional<Address>(Base + 16), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !view->text()->loading() &&
            view->text()->allText().contains("code line 699"),
        5000);
    QVERIFY(bench.session.dirty());
    QVERIFY(!QFile::exists(path + ".neverd/annotations.json"));
    // Tab changes focus without removing either side of the split.
    view->text()->setFocus();
    bench.action(ActionId::JumpPseudocode)->trigger();
    QVERIFY(view->isVisible() && bench.window->disassembly()->isVisible());
    for (auto *dock :
         bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->widget() == view)
        dock->forceClose();
    bench.window->disassembly()->navigate(Base);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base));
    QTest::qWait(300);
    QCOMPARE(diagnostics.join('\n').count("fixture slow decompile started"), 1);
  }

  void sourceTransformationCancelsWithoutBlockingBrowsingOrLosingSource() {
    QTemporaryDir directory;
    Workbench bench;
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    bench.window->openFile(writeFixture(directory, "source-options-slow.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base));
    bench.action(ActionId::JumpPseudocode)->trigger();
    auto *view = bench.codeView("source");
    QVERIFY(view);
    QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                 !view->text()->allText().isEmpty(),
                             OpenTimeoutMs);
    const auto original = view->text()->allText();
    QVERIFY(view->text()->sourceOptions().isEmpty());
    QVERIFY(!diagnostics.join('\n').contains(
        "fixture symbolic transformation started"));
    auto *button = view->findChild<QToolButton *>("sourceOptimization");
    QVERIFY(button && button->isVisible() && button->isEnabled());
    auto *menu = button->menu();
    menu->popup(button->mapToGlobal(QPoint(0, button->height())));
    QTRY_VERIFY(menu->isVisible());
    auto *symbolic = menu->findChild<QAction *>("sourceSymbolic");
    QVERIFY(symbolic);
    QVERIFY(!symbolic->isChecked());
    menu->setActiveAction(symbolic);
    QTest::keyClick(menu, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(diagnostics.join('\n').contains(
                                 "fixture symbolic transformation started"),
                             5000);
    QVERIFY(view->text()->loading());
    QCOMPARE(view->text()->allText(), original);
    bool functions = false, listing = false;
    bench.session.read("functions", {{"offset", 512}, {"limit", 32}}, this,
                       [&](const QJsonObject &p) {
                         functions = !p.value("items").toArray().isEmpty();
                       });
    bench.session.read("listing",
                       {{"address", hexAddress(Base + 16)}, {"after", 16}},
                       this, [&](const QJsonObject &) { listing = true; });
    QTRY_VERIFY_WITH_TIMEOUT(functions && listing, 1500);
    QMenu cancellation;
    view->addSourceActions(cancellation);
    auto *cancel = cancellation.findChild<QAction *>("sourceCancel");
    QVERIFY(cancel && cancel->isEnabled());
    cancel->trigger();
    QVERIFY(!view->text()->loading());
    QVERIFY(view->text()->sourceOptions().isEmpty());
    QCOMPARE(view->text()->allText(), original);
    // A fresh request after retiring the blocked replica still succeeds.
    view->text()->load(Base + 16, "source");
    QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                 !view->text()->allText().isEmpty(),
                             OpenTimeoutMs);
    QVERIFY(view->text()->sourceOptions().isEmpty());
  }

  void sourceTransformationFailureKeepsThePrecedingSource() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(writeFixture(directory, "source-options-fail.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base));
    bench.action(ActionId::JumpPseudocode)->trigger();
    auto *view = bench.codeView("source");
    QVERIFY(view);
    QTRY_VERIFY(!view->text()->loading() && !view->text()->allText().isEmpty());
    const auto original = view->text()->allText();
    view->text()->setSourceOptions({{"symbolic", true}});
    QTRY_VERIFY(!view->text()->loading());
    QVERIFY(view->text()->sourceOptions().isEmpty());
    QCOMPARE(view->text()->allText(), original);
    QVERIFY(view->text()->status().contains("Transformation failed"));
  }

  void sourceTransformationProfilesAreIndependentAndResetOnNavigation() {
    QTemporaryDir directory;
    Workbench bench;
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    bench.window->openFile(writeFixture(directory, "source-cache.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base));
    bench.action(ActionId::ViewLLVMC)->trigger();
    auto *view = bench.codeView("llvmc");
    QVERIFY(view);
    QTRY_VERIFY(!view->text()->loading() &&
                view->text()->allText().contains("return value"));
    QVERIFY(view->text()->sourceOptions().isEmpty());
    QVERIFY(view->text()->status().contains("O2"));
    {
      QMenu choices;
      view->addSourceActions(choices);
      auto *defaultLevel = choices.findChild<QAction *>("sourceLLVM_O2");
      auto *basic = choices.findChild<QAction *>("sourceLLVM_basic");
      auto *recovery = choices.findChild<QAction *>("sourceLLVMControlFlow");
      QVERIFY(defaultLevel && defaultLevel->isChecked());
      QVERIFY(basic && !basic->isChecked());
      QVERIFY(recovery && recovery->isChecked());
      basic->trigger();
      QTRY_VERIFY(!view->text()->loading());
      QCOMPARE(
          view->text()->sourceOptions().value("llvm_optimization").toString(),
          QStringLiteral("basic"));
      defaultLevel->trigger();
      QTRY_VERIFY(!view->text()->loading());
      QVERIFY(view->text()->sourceOptions().isEmpty());
      recovery->trigger();
      QTRY_VERIFY(!view->text()->loading());
      QCOMPARE(view->text()->sourceOptions().value("llvm_control_flow"),
               QJsonValue(false));
    }
    for (const QString name :
         {QStringLiteral("sourceLLVM_O3"), QStringLiteral("sourceSymbolic")}) {
      QMenu choices;
      view->addSourceActions(choices);
      auto *action = choices.findChild<QAction *>(name);
      QVERIFY(action);
      action->trigger();
      QTRY_VERIFY(!view->text()->loading());
    }
    QCOMPARE(
        view->text()->sourceOptions().value("llvm_optimization").toString(),
        QStringLiteral("O3"));
    QVERIFY(view->text()->sourceOptions().value("symbolic").toBool());
    QCOMPARE(view->text()->sourceOptions().value("llvm_control_flow"),
             QJsonValue(false));
    QVERIFY(diagnostics.join('\n').contains("\"llvm_optimization\":\"O3\""));
    QVERIFY(diagnostics.join('\n').contains("\"symbolic\":true"));
    QMenu defaults;
    view->addSourceActions(defaults);
    defaults.findChild<QAction *>("sourceDefault")->trigger();
    QTRY_VERIFY(!view->text()->loading());
    QVERIFY(view->text()->sourceOptions().isEmpty());
    {
      QMenu choices;
      view->addSourceActions(choices);
      QVERIFY(choices.findChild<QAction *>("sourceLLVM_O2")->isChecked());
      QVERIFY(!choices.findChild<QAction *>("sourceSymbolic")->isChecked());
      QVERIFY(
          choices.findChild<QAction *>("sourceLLVMControlFlow")->isChecked());
    }
    if (const auto capture =
            qEnvironmentVariable("NEVERD_CODE_NAV_CAPTURE_DIR");
        !capture.isEmpty()) {
      QDir().mkpath(capture);
      const auto previous = Theme::instance().mode();
      for (const auto mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(mode);
        QTest::qWait(30);
        const auto suffix = mode == Theme::Mode::Light ? "light" : "dark";
        QVERIFY(bench.window->grab().save(capture + "/source-options-" +
                                          suffix + ".png"));
        QMenu choices;
        view->addSourceActions(choices);
        choices.popup(view->mapToGlobal(QPoint(10, 50)));
        QTRY_VERIFY(choices.isVisible());
        QVERIFY(choices.grab().save(capture + "/source-options-menu-" + suffix +
                                    ".png"));
        auto *levels = qobject_cast<QMenu *>(
            choices.findChild<QAction *>("sourceLLVM_O2")->parent());
        QVERIFY(levels);
        levels->popup(choices.mapToGlobal(QPoint(choices.width(), 0)));
        QTRY_VERIFY(levels->isVisible());
        QVERIFY(levels->grab().save(capture + "/source-options-levels-" +
                                    suffix + ".png"));
      }
      Theme::instance().setMode(previous);
    }
    view->text()->setSourceOptions({{"symbolic", true}});
    QTRY_VERIFY(!view->text()->loading());
    view->text()->load(Base + 16, "llvmc");
    QTRY_VERIFY(!view->text()->loading() &&
                view->text()->allText().contains("function_1"));
    QVERIFY(view->text()->sourceOptions().isEmpty());
    QVERIFY(view->text()->status().contains("O2"));
  }

  void symbolicSimplificationOfEveryFunctionIsTheDefaultUntilTurnedOff() {
    QTemporaryDir directory;
    Workbench bench;
    const auto forget = qScopeGuard(
        [] { QSettings().remove(settings::SymbolicSimplification); });
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    bench.window->openFile(writeFixture(directory, "source-cache.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base));
    bench.action(ActionId::JumpPseudocode)->trigger();
    auto *view = bench.codeView("source");
    QVERIFY(view);
    QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                 !view->text()->allText().isEmpty(),
                             OpenTimeoutMs);
    QVERIFY(view->text()->sourceOptions().isEmpty());
    const QJsonObject symbolic{{"symbolic", true}};
    // Turned on, it reaches the shown function at once and stays on.
    {
      QMenu choices;
      view->addSourceActions(choices);
      auto *every = choices.findChild<QAction *>("sourceSymbolicEvery");
      QVERIFY(every && every->isCheckable() && !every->isChecked());
      every->trigger();
    }
    QVERIFY(QSettings().value(settings::SymbolicSimplification).toBool());
    QTRY_VERIFY(!view->text()->loading() &&
                view->text()->status().contains("Symbolic simplification"));
    QCOMPARE(view->text()->sourceOptions(), symbolic);
    QVERIFY(diagnostics.join('\n').contains("\"symbolic\":true"));
    {
      QMenu choices;
      view->addSourceActions(choices);
      QVERIFY(choices.findChild<QAction *>("sourceSymbolic")->isChecked());
      QVERIFY(choices.findChild<QAction *>("sourceSymbolicEvery")->isChecked());
      QVERIFY(!choices.findChild<QAction *>("sourceDefault")->isEnabled());
    }
    // Another function opens with it.
    view->text()->load(Base + 16, "source");
    QTRY_VERIFY(!view->text()->loading() && !view->text()->allText().isEmpty());
    QCOMPARE(view->text()->sourceOptions(), symbolic);
    // One function turns it off; Restore default source turns it on again.
    {
      QMenu choices;
      view->addSourceActions(choices);
      choices.findChild<QAction *>("sourceSymbolic")->trigger();
    }
    QTRY_VERIFY(!view->text()->loading());
    QVERIFY(view->text()->sourceOptions().isEmpty());
    {
      QMenu choices;
      view->addSourceActions(choices);
      auto *restore = choices.findChild<QAction *>("sourceDefault");
      QVERIFY(restore->isEnabled());
      restore->trigger();
    }
    QTRY_VERIFY(!view->text()->loading());
    QCOMPARE(view->text()->sourceOptions(), symbolic);
    // An IR has no source options to take.
    bench.action(ActionId::ViewLowIR)->trigger();
    auto *ir = bench.codeView(QStringLiteral("low"));
    QVERIFY(ir);
    QTRY_VERIFY(!ir->text()->loading() && !ir->text()->allText().isEmpty());
    QVERIFY(ir->text()->sourceOptions().isEmpty());
    QVERIFY(!ir->text()->status().contains("source view"));
    // Options > General shows the same choice and turns it off everywhere.
    bool shownOn = false;
    QTimer dialogProbe;
    connect(&dialogProbe, &QTimer::timeout, this, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      auto *box = dialog
                      ? dialog->findChild<QCheckBox *>("optionsSymbolicEvery")
                      : nullptr;
      if (!box)
        return;
      dialogProbe.stop();
      shownOn = box->isChecked();
      box->setChecked(false);
      dialog->findChild<QDialogButtonBox *>()
          ->button(QDialogButtonBox::Ok)
          ->click();
    });
    dialogProbe.start(10);
    bench.action(ActionId::OptionsGeneral)->trigger();
    QVERIFY(shownOn);
    QVERIFY(!QSettings().value(settings::SymbolicSimplification).toBool());
    QTRY_VERIFY(!view->text()->loading());
    QVERIFY(view->text()->sourceOptions().isEmpty());
    view->text()->load(Base, "source");
    QTRY_VERIFY(!view->text()->loading() && !view->text()->allText().isEmpty());
    QVERIFY(view->text()->sourceOptions().isEmpty());
  }

  void symbolicSimplificationOfEveryFunctionGivesWayToOrdinarySource() {
    QTemporaryDir directory;
    Workbench bench;
    const auto forget = qScopeGuard(
        [] { QSettings().remove(settings::SymbolicSimplification); });
    SourceDefaults::instance().setSymbolic(true);
    bench.window->openFile(writeFixture(directory, "source-options-fail.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base));
    bench.action(ActionId::JumpPseudocode)->trigger();
    auto *view = bench.codeView("source");
    QVERIFY(view);
    // A refused simplification still shows the function.
    QTRY_VERIFY_WITH_TIMEOUT(
        !view->text()->loading() &&
            view->text()->status().contains("Transformation failed"),
        OpenTimeoutMs);
    QVERIFY(view->text()->sourceOptions().isEmpty());
    QVERIFY(!view->text()->allText().isEmpty());
    QVERIFY(!view->text()->allText().contains("refused"));
  }

  void cancellingSymbolicSimplificationOfANewFunctionShowsOrdinarySource() {
    QTemporaryDir directory;
    Workbench bench;
    const auto forget = qScopeGuard(
        [] { QSettings().remove(settings::SymbolicSimplification); });
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    SourceDefaults::instance().setSymbolic(true);
    bench.window->openFile(writeFixture(directory, "source-options-slow.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base));
    bench.action(ActionId::JumpPseudocode)->trigger();
    auto *view = bench.codeView("source");
    QVERIFY(view);
    QTRY_VERIFY_WITH_TIMEOUT(diagnostics.join('\n').contains(
                                 "fixture symbolic transformation started"),
                             5000);
    QVERIFY(view->text()->loading());
    QVERIFY(view->text()->allText().isEmpty());
    QMenu cancellation;
    view->addSourceActions(cancellation);
    auto *cancel = cancellation.findChild<QAction *>("sourceCancel");
    QVERIFY(cancel && cancel->isEnabled());
    cancel->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                 !view->text()->allText().isEmpty(),
                             OpenTimeoutMs);
    QVERIFY(view->text()->sourceOptions().isEmpty());
    QVERIFY(view->text()->status().contains("Cancelled"));
  }

  void largePseudocodeKeepsProcessingEvents() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-large.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY(bench.window->disassembly()->currentFunction().has_value());
    QElapsedTimer elapsed;
    elapsed.start();
    qint64 longest = 0;
    int ticks = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, this, [&] {
      longest = std::max(longest, elapsed.restart());
      ++ticks;
    });
    heartbeat.start(5);
    bench.action(ActionId::ViewLLVMC)->trigger();
    auto *view = bench.codeView(QStringLiteral("llvmc"));
    QVERIFY(view);
    QTRY_VERIFY_WITH_TIMEOUT(
        !view->text()->loading() &&
            view->text()->allText().contains("code line 19999"),
        15000);
    QTest::qWait(10);
    QVERIFY(view->text()->preludeFolded());
    QVERIFY(view->text()->lineCount() >= 20000);
    QVERIFY(ticks > 1);
    QVERIFY2(longest < 500,
             qPrintable(QStringLiteral("UI event gap: %1 ms").arg(longest)));
    qInfo() << "Largest event gap while rendering 20,000 source lines:"
            << longest << "ms";
  }

  void codeAddressLinksRetainAddressNavigation_data() {
    QTest::addColumn<bool>("global");
    QTest::newRow("import-with-function-address") << false;
    QTest::newRow("global-object") << true;
  }

  void codeAddressLinksRetainAddressNavigation() {
    QFETCH(bool, global);
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(writeFixture(
        directory, global ? QStringLiteral("pseudocode-global.bin")
                          : QStringLiteral("pseudocode-import.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 1);
    for (auto *button : view->findChildren<QToolButton *>())
      if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
        button->setChecked(true);
    QVERIFY(view->locked());
    const auto target = global ? Base + 0x3000 : Base + 0x160;
    // The import intentionally has both address and function_address.
    if (!global) {
      QJsonObject resolved;
      bench.session.read(QStringLiteral("resolve"), {{"query", "function_22"}},
                         bench.window.get(),
                         [&](const QJsonObject &p) { resolved = p; });
      QTRY_VERIFY(!resolved.isEmpty());
      QVERIFY(resolved.value("import").toBool());
      QVERIFY(addressValue(resolved.value("function_address")).has_value());
    }
    const auto token =
        global ? QStringLiteral("global_value") : QStringLiteral("function_22");
    QSignalSpy activation(code, &CodeText::objectActivated);
    QSignalSpy names(code, &CodeText::nameActivated);
    // Clicking the use, after a real declaration, exercises object recognition.
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const int row = global ? 2 : 1;
    const QPoint point(
        qRound(6 + 17 * metrics.horizontalAdvance(QLatin1Char('M'))),
        qRound((row - code->verticalScrollBar()->value() + 0.5) *
               std::ceil(metrics.lineSpacing())));
    code->setFocus();
    QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(code->currentToken(), token);
    QTest::mouseDClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(global ? activation.size() : names.size(), 1);
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(target));
    QTRY_VERIFY(!code->hasFocus());
    QCOMPARE(code->function(), std::optional<Address>(Base + 0x140));
  }

  void lateCodeLookupsDoNotReplaceNewerNavigation_data() {
    QTest::addColumn<bool>("error");
    QTest::addColumn<bool>("sessionChange");
    QTest::newRow("late-success-navigation") << false << false;
    QTest::newRow("late-error-navigation") << true << false;
    QTest::newRow("late-success-session") << false << true;
    QTest::newRow("late-error-session") << true << true;
  }

  void lateCodeLookupsDoNotReplaceNewerNavigation() {
    QFETCH(bool, error);
    QFETCH(bool, sessionChange);
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(writeFixture(
        directory, error ? QStringLiteral("pseudocode-delayed-error.bin")
                         : QStringLiteral("pseudocode-delayed.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 1);
    for (auto *button : view->findChildren<QToolButton *>())
      if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
        button->setChecked(true);
    QVERIFY(view->locked());
    QSignalSpy messages(&bench.session, &Session::message);
    QSignalSpy names(code, &CodeText::nameActivated);
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const QPoint point(
        qRound(6 + 17 * metrics.horizontalAdvance(QLatin1Char('M'))),
        qRound((1 - code->verticalScrollBar()->value() + 0.5) *
               std::ceil(metrics.lineSpacing())));
    code->setFocus();
    QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QTest::mouseDClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(names.size(), 1);
    const auto started = [&] {
      for (const auto &row : messages)
        if (row.first().toString().contains(
                QStringLiteral("fixture delayed lookup started")))
          return true;
      return false;
    };
    QTRY_VERIFY(started());
    if (sessionChange) {
      const auto epoch = bench.session.epoch();
      bench.session.closeFile();
      QTRY_VERIFY(bench.session.epoch() != epoch);
      QTRY_VERIFY(!bench.session.loaded());
    } else {
      // The pinned view remains at its source; the address view moves.
      bench.window->disassembly()->navigate(Base + 0x180);
      QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                   std::optional<Address>(Base + 0x180));
    }
    QTest::qWait(600);
    if (!sessionChange) {
      QCOMPARE(code->function(), std::optional<Address>(Base + 0x140));
      QCOMPARE(bench.window->disassembly()->currentFunction(),
               std::optional<Address>(Base + 0x180));
    } else {
      QVERIFY(!bench.session.loaded());
    }
    for (const auto &row : messages)
      QVERIFY(
          !row.first().toString().startsWith(QStringLiteral("Cannot jump to")));
  }

  void pseudocodeFunctionLinksStayInTheirWindow_data() {
    QTest::addColumn<QString>("representation");
    QTest::addColumn<bool>("secondWindow");
    QTest::addColumn<bool>("locked");
    for (const auto &representation :
         {QStringLiteral("source"), QStringLiteral("llvmc")})
      for (const bool second : {false, true})
        for (const bool locked : {false, true}) {
          const auto name = representation + (second ? "-second" : "-primary") +
                            (locked ? "-locked" : "-following");
          QTest::newRow(qPrintable(name)) << representation << second << locked;
        }
  }

  void pseudocodeFunctionLinksStayInTheirWindow() {
    QFETCH(QString, representation);
    QFETCH(bool, secondWindow);
    QFETCH(bool, locked);
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("pseudocode-navigation.bin"));
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    const Address caller = Base + 0x140;
    const Address callee =
        Base + (representation == QLatin1String("source") ? 0x160 : 0x150);
    disassembly->navigate(caller);
    QTRY_COMPARE(disassembly->currentFunction(),
                 std::optional<Address>(caller));
    bench.action(secondWindow ? ActionId::ViewLowIR : ActionId::ViewPseudocode)
        ->trigger();
    auto *view = bench.codeView(secondWindow ? QStringLiteral("low")
                                             : QStringLiteral("source"));
    QVERIFY(view);
    view->setRepresentation(representation);
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() > 1,
                             OpenTimeoutMs);
    QCOMPARE(code->function(), std::optional<Address>(caller));
    if (locked) {
      QToolButton *lock = nullptr;
      for (auto *button : view->findChildren<QToolButton *>())
        if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
          lock = button;
      QVERIFY(lock);
      lock->setChecked(true);
      QVERIFY(view->locked());
      // A pinned code view can differ from the background assembly location.
      disassembly->navigate(Base + 0x180);
      QTRY_COMPARE(disassembly->currentFunction(),
                   std::optional<Address>(Base + 0x180));
      QCOMPARE(code->function(), std::optional<Address>(caller));
    }
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const QString link = representation == QLatin1String("source")
                             ? QStringLiteral("function_22")
                             : QStringLiteral("Bar_ctor");
    if (representation == QLatin1String("llvmc"))
      QVERIFY(code->findText(QStringLiteral("caller(void)"), true));
    QVERIFY(code->findText(link, true));
    QCOMPARE(code->currentToken(), link);
    QSignalSpy activation(code, &CodeText::nameActivated);
    // The call is row 1 in C, row 3 after LLVM C's folded prelude.
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const int row = representation == QLatin1String("source") ? 1 : 3;
    const QPoint point(
        qRound(6 + 17 * metrics.horizontalAdvance(QLatin1Char('M'))),
        qRound((row - code->verticalScrollBar()->value() + 0.5) *
               std::ceil(metrics.lineSpacing())));
    QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(code->currentToken(), link);
    QTest::mouseDClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(activation.size(), 1);
    if (representation == QLatin1String("llvmc"))
      QCOMPARE(activation.first().first().toString(),
               QStringLiteral("_ZN3BarC1Ev"));
    QTRY_COMPARE_WITH_TIMEOUT(code->function(), std::optional<Address>(callee),
                              OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading(), OpenTimeoutMs);
    QCOMPARE(view->representation(), representation);
    QVERIFY(code->isVisible());
    QTRY_VERIFY(code->hasFocus());
    QTRY_COMPARE(disassembly->currentFunction(),
                 std::optional<Address>(callee));
    QVERIFY(bench.action(ActionId::JumpBack)->isEnabled());
    bench.action(ActionId::JumpBack)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(code->function(), std::optional<Address>(caller),
                              OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading(), OpenTimeoutMs);
    QVERIFY(code->isVisible());
    QTRY_VERIFY(code->hasFocus());
    QCOMPARE(view->representation(), representation);
    QVERIFY(bench.action(ActionId::JumpForward)->isEnabled());
    bench.action(ActionId::JumpForward)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(code->function(), std::optional<Address>(callee),
                              OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading(), OpenTimeoutMs);
    QVERIFY(code->isVisible());
    QTRY_VERIFY(code->hasFocus());
  }

  void functionsActivationKeepsCodeWindow_data() {
    pseudocodeFunctionLinksStayInTheirWindow_data();
    QTest::newRow("low-secondary-following")
        << QStringLiteral("low") << true << false;
    QTest::newRow("rust-primary-following")
        << QStringLiteral("source") << false << false;
    QTest::newRow("go-secondary-following")
        << QStringLiteral("source") << true << false;
    QTest::newRow("c-rust-primary-locked")
        << QStringLiteral("c") << false << true;
    QTest::newRow("c-rust-secondary-following")
        << QStringLiteral("c") << true << false;
    if (!qEnvironmentVariable("NEVERD_CODE_NAV_WORKER").isEmpty() &&
        !qEnvironmentVariable("NEVERD_CODE_NAV_FILE").isEmpty()) {
      QTest::newRow("native-c") << QStringLiteral("source") << false << false;
      QTest::newRow("native-llvmc") << QStringLiteral("llvmc") << true << false;
    }
  }

  void functionsActivationKeepsCodeWindow() {
    QFETCH(QString, representation);
    QFETCH(bool, secondWindow);
    QFETCH(bool, locked);
    const bool native =
        QByteArray(QTest::currentDataTag()).startsWith("native-");
    QTemporaryDir directory;
    const QByteArray tag = QTest::currentDataTag();
    const QString fixture = tag.startsWith("rust-") || tag.startsWith("c-rust-")
                                ? QStringLiteral("pseudocode-rust.bin")
                            : tag.startsWith("go-")
                                ? QStringLiteral("pseudocode-go.bin")
                                : QStringLiteral("fixture.bin");
    const auto path = native ? qEnvironmentVariable("NEVERD_CODE_NAV_FILE")
                             : writeFixture(directory, fixture);
    Workbench bench(native ? qEnvironmentVariable("NEVERD_CODE_NAV_WORKER")
                           : QString::fromLocal8Bit(TEST_WORKER));
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *functions = bench.functions();
    QVERIFY(functions);
    QTRY_VERIFY_WITH_TIMEOUT(!functions->model().rowObject(2).isEmpty(),
                             OpenTimeoutMs);
    const Address caller =
        native ? *functions->model().addressAt(0) : Base + 0x140;
    auto *disassembly = bench.window->disassembly();
    disassembly->navigate(caller);
    QTRY_COMPARE(disassembly->currentFunction(),
                 std::optional<Address>(caller));
    bench.action(secondWindow ? ActionId::ViewLowIR : ActionId::ViewPseudocode)
        ->trigger();
    auto *view = bench.codeView(secondWindow ? QStringLiteral("low")
                                             : QStringLiteral("source"));
    QVERIFY(view);
    view->setRepresentation(representation);
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() > 0,
                             OpenTimeoutMs);
    if (locked) {
      for (auto *button : view->findChildren<QToolButton *>())
        if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
          button->setChecked(true);
      QVERIFY(view->locked());
      disassembly->navigate(Base + 0x180);
      QTRY_COMPARE(disassembly->currentFunction(),
                   std::optional<Address>(Base + 0x180));
    }

    auto *table = functions->table();
    Address previous = caller;
    for (const bool keyboard : {false, true}) {
      const int row = native ? (keyboard ? 2 : 1) : (keyboard ? 23 : 22);
      const auto target = functions->model().addressAt(row);
      QVERIFY(target);
      code->setFocus();
      QTRY_VERIFY(code->hasFocus());
      const auto index = functions->model().index(row, 0);
      table->scrollTo(index);
      // A narrow chooser can clip the name column beside a second code view.
      const QRect cell =
          table->visualRect(index).intersected(table->viewport()->rect());
      QVERIFY(!cell.isEmpty());
      const QPoint point = cell.center();
      QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                        point);
      QTRY_VERIFY(table->hasFocus());
      QCOMPARE(table->currentIndex().row(), row);
      if (keyboard)
        QTest::keyClick(table, Qt::Key_Return);
      else
        QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                           point);
      QVERIFY(code->isVisible());
      QTRY_COMPARE_WITH_TIMEOUT(code->function(), target, OpenTimeoutMs);
      QTRY_VERIFY_WITH_TIMEOUT(!code->loading(), OpenTimeoutMs);
      QVERIFY(code->lineCount() > 0);
      QCOMPARE(view->representation(), representation);
      QCOMPARE(view->locked(), locked);
      QTRY_VERIFY(code->hasFocus());
      QTRY_COMPARE(disassembly->currentFunction(), target);

      bench.action(ActionId::JumpBack)->trigger();
      QTRY_COMPARE_WITH_TIMEOUT(
          code->function(), std::optional<Address>(previous), OpenTimeoutMs);
      QTRY_VERIFY(!code->loading() && code->hasFocus());
      bench.action(ActionId::JumpForward)->trigger();
      QTRY_COMPARE_WITH_TIMEOUT(code->function(), target, OpenTimeoutMs);
      QTRY_VERIFY(!code->loading() && code->hasFocus());
      previous = *target;
    }
    if (native) {
      const auto capture = qEnvironmentVariable("NEVERD_CODE_NAV_CAPTURE_DIR");
      if (!capture.isEmpty()) {
        QDir().mkpath(capture);
        QVERIFY(bench.window->grab().save(QDir(capture).filePath(
            QString::fromLatin1(QTest::currentDataTag()) +
            QStringLiteral(".png"))));
      }
    }
  }

  void functionsActivationUsesDisassemblyWhenCodeIsNotCurrent_data() {
    QTest::addColumn<bool>("closed");
    QTest::newRow("disassembly-current") << false;
    QTest::newRow("code-closed") << true;
  }

  void functionsActivationUsesDisassemblyWhenCodeIsNotCurrent() {
    QFETCH(bool, closed);
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    QTRY_VERIFY(disassembly->currentFunction().has_value());
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    QTRY_VERIFY(!view->text()->loading() && view->text()->isVisible());
    const auto originalFunction = view->text()->function();
    if (closed) {
      for (auto *dock :
           bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
        if (dock->widget() == view)
          dock->forceClose();
      QTRY_VERIFY(!view->isVisible());
    } else {
      // Users can still put the two views in one tab group. Their default
      // arrangement is now a split, so create that hidden-view case here.
      KDDockWidgets::QtWidgets::DockWidget *codeDock = nullptr,
                                           *listingDock = nullptr;
      for (auto *dock :
           bench.window
               ->findChildren<KDDockWidgets::QtWidgets::DockWidget *>()) {
        if (dock->widget() == view)
          codeDock = dock;
        if (dock->widget() == disassembly)
          listingDock = dock;
      }
      QVERIFY(codeDock && listingDock);
      listingDock->addDockWidgetAsTab(codeDock);
      bench.action(ActionId::ViewDisassembly)->trigger();
      disassembly->focusContent();
      QTRY_VERIFY(disassembly->isVisible());
      const auto revision = bench.session.revision();
      bench.session.rename(Base, QStringLiteral("renamed_while_hidden"));
      QTRY_VERIFY(bench.session.revision() != revision);
      QVERIFY(view->text()->interrupted());
    }
    auto *table = bench.functions()->table();
    QTRY_VERIFY(!bench.functions()->model().rowObject(22).isEmpty());
    table->setCurrentIndex(bench.functions()->model().index(22, 0));
    table->setFocus();
    QTRY_VERIFY(table->hasFocus());
    QTest::keyClick(table, Qt::Key_Return);
    QTRY_COMPARE(disassembly->currentFunction(),
                 std::optional<Address>(Base + 0x160));
    QVERIFY(disassembly->isVisible());
    QVERIFY(!view->isVisible());
    if (!closed) {
      // Showing the existing tab catches up to edits of its own function,
      // without following assembly navigation performed while it was hidden.
      for (auto *dock :
           bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
        if (dock->widget() == view)
          dock->raise();
      QTRY_COMPARE(view->text()->function(), originalFunction);
      QTRY_VERIFY(!view->text()->loading() && !view->text()->interrupted());
      disassembly->focusContent();
      bench.action(ActionId::JumpPseudocode)->trigger();
      QTRY_COMPARE(view->text()->function(),
                   std::optional<Address>(Base + 0x160));
    }
  }

  void changingFiltersRetiresObsoletePages() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto &model = bench.functions()->model();
    QTRY_COMPARE(model.total(), 600);
    QTRY_VERIFY(!bench.session.busy());
    QSignalSpy failures(&model, &ChooserModel::failed);

    // Keep the worker busy while successive filters replace the needed page.
    QObject blocker;
    bench.session.read(QStringLiteral("resolve"),
                       {{"query", "delayed_function"}}, &blocker,
                       [](const QJsonObject &) {});
    QTest::qWait(25);
    for (int i = 0; i < 96; ++i)
      model.setFilter(QStringLiteral("function_%1").arg(i));
    model.setFilter(QStringLiteral("function_"));
    QTest::qWait(50);
    QCOMPARE(failures.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(model.total(), 600, OpenTimeoutMs);
    QTRY_COMPARE(model.rowObject(0).value("name").toString(),
                 QStringLiteral("function_0"));
  }

  void rapidlyScrollingLargeListsKeepsTheLastPageAvailable_data() {
    QTest::addColumn<QString>("worker");
    QTest::addColumn<QString>("binary");
    QTest::newRow("fixture")
        << QString::fromLocal8Bit(TEST_WORKER) << QString();
    const auto worker = qEnvironmentVariable("NEVERD_LARGE_PE_WORKER");
    const auto binary = qEnvironmentVariable("NEVERD_LARGE_PE_FILE");
    if (!worker.isEmpty() && !binary.isEmpty())
      QTest::newRow("native-pe") << worker << binary;
  }

  void rapidlyScrollingLargeListsKeepsTheLastPageAvailable() {
    QFETCH(QString, worker);
    QFETCH(QString, binary);
    QTemporaryDir directory;
    const bool fixture = binary.isEmpty();
    const auto path =
        fixture ? writeFixture(directory, QStringLiteral("many-functions.bin"))
                : binary;
    QVERIFY(!path.isEmpty());
    Workbench bench(worker);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto &model = bench.functions()->model();
    // The optional native profile includes background discovery over the
    // entire large image; ordinary fixture deadlines remain unchanged.
    const int listTimeout = fixture ? OpenTimeoutMs : NativeOpenTimeoutMs;
    if (!fixture)
      QTRY_VERIFY_WITH_TIMEOUT(
          bench.session.background().value("state").toString() == "building" ||
              bench.session.indexReady(),
          listTimeout);
    const int total =
        fixture ? 20000 : bench.session.background().value("functions").toInt();
    QVERIFY(total > 48 * 256);
    QTRY_COMPARE_WITH_TIMEOUT(model.total(), total, listTimeout);
    if (fixture)
      QTRY_VERIFY_WITH_TIMEOUT(!bench.session.busy(), OpenTimeoutMs);
    QSignalSpy failures(&model, &ChooserModel::failed);

    QObject blocker;
    if (fixture) {
      bench.session.read(QStringLiteral("resolve"),
                         {{"query", "delayed_function"}}, &blocker,
                         [](const QJsonObject &) {});
      QTest::qWait(25);
    }
    // Successive viewport positions span more pages than the shared queue.
    for (int row = 256; row < model.total(); row += 256)
      model.data(model.index(row, 0), Qt::DisplayRole);
    const int last = model.total() - 1;
    QVERIFY(model.index(last, 0).isValid());
    model.data(model.index(last, 0), Qt::DisplayRole);
    QTest::qWait(50);
    QCOMPARE(failures.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!model.rowObject(last).isEmpty(), listTimeout);
    if (fixture)
      QCOMPARE(model.rowObject(last).value("name").toString(),
               QStringLiteral("function_19999"));
    else {
      // The real worker must also survive replacing expensive filters.
      for (int i = 0; i < 96; ++i)
        model.setFilter(QStringLiteral("sub_%1").arg(i));
      model.setFilter(QString());
      QTest::qWait(50);
      QCOMPARE(failures.count(), 0);
      QTRY_COMPARE_WITH_TIMEOUT(model.total(), total, listTimeout);
      QTRY_VERIFY_WITH_TIMEOUT(!model.rowObject(0).isEmpty(), listTimeout);
      if (const auto capture = qEnvironmentVariable("NEVERD_LARGE_PE_CAPTURE");
          !capture.isEmpty())
        QVERIFY(bench.window->grab().save(capture));
    }
  }

  void pseudocodeReadsInTheFunctionsLanguage() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-rust.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    // F5 shows a Rust function as Rust, and the window says so.
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() > 1,
                             OpenTimeoutMs);
    QCOMPARE(code->language(), QStringLiteral("rust"));
    QCOMPARE(view->chosenLanguage(), QStringLiteral("Rust"));
    QTRY_COMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
                 QStringLiteral("Pseudocode-A (Rust)"));
    // A source name is one name wherever it is clicked, and resolves by the
    // symbol it reads.
    const QStringList lines = code->allText().split(QLatin1Char('\n'));
    int row = -1;
    for (int i = 0; i < lines.size() && row < 0; ++i)
      if (lines[i].contains(QStringLiteral("core::fmt::write(")))
        row = i;
    QVERIFY(row >= 0);
    const int column = int(lines[row].indexOf(QStringLiteral("fmt")));
    const auto name = code->sourceNameAt(row, column);
    QVERIFY(name);
    QCOMPARE(name->first, QStringLiteral("core::fmt::write"));
    QCOMPARE(name->second.identifier, QStringLiteral("core_fmt_write"));
    QCOMPARE(name->second.address, std::optional<Address>(Base + 0x160));
    QVERIFY(!code->sourceNameAt(row,
                                int(lines[row].indexOf(QStringLiteral("v0")))));
    QSignalSpy names(code, &CodeText::nameActivated);
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const QPoint point(
        qRound(6 + (6 + column + 0.5) *
                       metrics.horizontalAdvance(QLatin1Char('M'))),
        qRound((row - code->verticalScrollBar()->value() + 0.5) *
               std::ceil(metrics.lineSpacing())));
    code->setFocus();
    QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QTest::mouseDClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(names.size(), 1);
    QCOMPARE(names.first().first().toString(),
             QStringLiteral("_ZN4core3fmt5write17h0123456789abcdefE"));
    // A function the engine refuses reads in no language.
    bench.window->disassembly()->navigate(Base + 0x180);
    bench.window->disassembly()->focusContent();
    bench.action(ActionId::JumpPseudocode)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->status().contains(QStringLiteral("fixture refuses")),
        OpenTimeoutMs);
    QCOMPARE(code->language(), QStringLiteral("c"));
    QCOMPARE(view->chosenLanguage(), QString());
    QTRY_COMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
                 QStringLiteral("Pseudocode-A"));
    QVERIFY(!code->sourceNameAt(0, 0));
    bench.window->disassembly()->navigate(Base + 0x140);
    bench.window->disassembly()->focusContent();
    bench.action(ActionId::JumpPseudocode)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() &&
                                 code->language() == QLatin1String("rust"),
                             OpenTimeoutMs);
    // Pseudocode reads the Rust; C beside it reads every function as C.
    const QStringList pseudocodeAndC = {
        QStringLiteral("source"), QStringLiteral("c"),
        QStringLiteral("llvmc"),  QStringLiteral("low"),
        QStringLiteral("med"),    QStringLiteral("high"),
        QStringLiteral("llvm")};
    QCOMPARE(representationsOf(view), pseudocodeAndC);
    view->setRepresentation(QStringLiteral("c"));
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() &&
                                 code->language() == QLatin1String("c"),
                             OpenTimeoutMs);
    QCOMPARE(view->chosenLanguage(), QString());
    QTRY_COMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
                 QStringLiteral("C-A"));
    // A language the menu does not offer leaves the view as it is.
    view->setRepresentation(QStringLiteral("go"));
    QCOMPARE(view->representation(), QStringLiteral("c"));
    view->setRepresentation(QStringLiteral("source"));
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() &&
                                 code->language() == QLatin1String("rust"),
                             OpenTimeoutMs);
    QTRY_COMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
                 QStringLiteral("Pseudocode-A (Rust)"));
  }

  void llvmIrNamesGoWhereTheyStandFor() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("llvm-view.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    for (auto *button : view->findChildren<QToolButton *>())
      if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
        button->setChecked(true);
    view->setRepresentation(QStringLiteral("llvm"));
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() && code->representation() == QLatin1String("llvm") &&
            code->hasPrelude(),
        OpenTimeoutMs);
    // The function shows alone: its declarations fold before it, its
    // attribute groups and metadata after it.
    QVERIFY(code->preludeFolded());
    QVERIFY(code->hasEpilogue() && code->epilogueFolded());
    QCOMPARE(code->lineCount(), 14);
    const QStringList source = code->allText().split(QLatin1Char('\n'));
    QCOMPARE(source.size(), 29);
    QSignalSpy messages(&bench.session, &Session::message);
    QSignalSpy names(code, &CodeText::nameActivated);
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const qreal width = metrics.horizontalAdvance(QLatin1Char('M'));
    // Double-click occurrence \p occurrence of \p token on displayed line
    // \p line, which shows source line \p sourceLine, inside its second
    // character.
    const auto doubleClick = [&](int line, int sourceLine, const QString &token,
                                 int occurrence = 0) {
      int column = -1;
      for (int i = 0; i <= occurrence; ++i)
        column = int(source[sourceLine].indexOf(token, column + 1));
      QVERIFY2(column >= 0, qPrintable(token));
      code->setCursorLine(line);
      code->horizontalScrollBar()->setValue(0);
      const QPoint point(
          qRound(6 + int(5 * width) + (column + 1.5) * width),
          qRound((line - code->verticalScrollBar()->value() + 0.5) *
                 std::ceil(metrics.lineSpacing())));
      code->setFocus();
      QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier,
                        point);
      QTest::mouseDClick(code->viewport(), Qt::LeftButton, Qt::NoModifier,
                         point);
    };
    // Metadata the definition names opens the epilogue at its node.
    doubleClick(2, 14, QStringLiteral("!0"));
    QVERIFY(!code->epilogueFolded());
    QCOMPARE(code->cursorLine(), 28 - 12);
    // A global the binary does not name shows at its declaration.
    doubleClick(7, 19, QStringLiteral("@llvm.example"));
    QCOMPARE(names.size(), 1);
    QCOMPARE(names.last().first().toString(), QStringLiteral("llvm.example"));
    QTRY_VERIFY(!code->preludeFolded());
    QTRY_COMPARE(code->cursorLine(), 11);
    // Locals, blocks, parameters and attribute groups open where defined.
    doubleClick(18, 18, QStringLiteral("%slot"));
    QCOMPARE(code->cursorLine(), 16);
    doubleClick(20, 20, QStringLiteral("%done"));
    QCOMPARE(code->cursorLine(), 22);
    doubleClick(18, 18, QStringLiteral("%arg0"));
    QCOMPARE(code->cursorLine(), 14);
    doubleClick(14, 14, QStringLiteral("#0"));
    QCOMPARE(code->cursorLine(), 26);
    QCOMPARE(names.size(), 1);
    // A comment's address resolves as it reads.
    doubleClick(16, 16, QStringLiteral("0xFFFF800012343008"));
    QCOMPARE(names.size(), 2);
    QCOMPARE(names.last().first().toString(),
             QStringLiteral("0xFFFF800012343008"));
    // Hovering a mirror tells what it is: the comment over its declaration,
    // the declaration, and where this line's spelling goes.
    const int load = int(source[16].indexOf(QStringLiteral("@__nd_codeptr_")));
    const QString hover = code->globalHoverText(16, load + 2);
    QVERIFY2(
        hover.startsWith(QStringLiteral(
            "NeverD's mirror of the 64 image bytes at 0xFFFF800012343000")),
        qPrintable(hover));
    QVERIFY2(hover.contains(
                 QStringLiteral("\n  +8 = 0xFFFF800012343008 data_slot\n")),
             qPrintable(hover));
    QVERIFY2(hover.contains(QStringLiteral(
                 "\n@__nd_codeptr_FFFF800012343000 = external global")),
             qPrintable(hover));
    QVERIFY2(hover.endsWith(
                 QStringLiteral("Double-click to go to 0xFFFF800012343008.")),
             qPrintable(hover));
    QVERIFY(code->globalHoverText(
                    18, int(source[18].indexOf(QStringLiteral("%slot"))))
                .isEmpty());
    // An image mirror stands for its first byte where declared, and for the
    // byte the load's constant offset reaches on the load.
    doubleClick(9, 9, QStringLiteral("@__nd_codeptr_"));
    QCOMPARE(names.size(), 3);
    QCOMPARE(names.last().first().toString(), hexAddress(Base + 0x3000));
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(Base + 0x3000));
    // A mirror a line reaches at two offsets: each spelling, its own.
    doubleClick(17, 17, QStringLiteral("@__nd_codeptr_"), 1);
    QCOMPARE(names.size(), 4);
    QCOMPARE(names.last().first().toString(), hexAddress(Base + 0x3018));
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(Base + 0x3018));
    doubleClick(16, 16, QStringLiteral("@__nd_codeptr_"));
    QCOMPARE(names.size(), 5);
    QCOMPARE(names.last().first().toString(), hexAddress(Base + 0x3008));
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(Base + 0x3008));
    QCOMPARE(code->function(), std::optional<Address>(Base + 0x140));
    for (const auto &row : messages)
      QVERIFY(
          !row.first().toString().startsWith(QStringLiteral("Cannot jump to")));
  }

  void cppPseudocodeRetainsASeparateCChoice() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-cpp.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                 view->text()->language() ==
                                     QLatin1String("cpp"),
                             OpenTimeoutMs);
    QCOMPARE(view->chosenLanguage(), QStringLiteral("C++"));
    QTRY_COMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
                 QStringLiteral("Pseudocode-A (C++)"));
    QVERIFY(view->text()->allText().contains(QStringLiteral("std::string")));
    QVERIFY(representationsOf(view).contains(QStringLiteral("c")));
    view->setRepresentation(QStringLiteral("c"));
    QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                 view->text()->language() == QLatin1String("c"),
                             OpenTimeoutMs);
    QVERIFY(!view->text()->allText().contains(QStringLiteral("std::string")));
    view->setRepresentation(QStringLiteral("source"));
    QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                 view->text()->language() ==
                                     QLatin1String("cpp"),
                             OpenTimeoutMs);
  }

  void pseudocodeOffersOnlyTheProgramsLanguages() {
    QTemporaryDir directory;
    const QStringList cOnly = {
        QStringLiteral("source"), QStringLiteral("llvmc"),
        QStringLiteral("low"),    QStringLiteral("med"),
        QStringLiteral("high"),   QStringLiteral("llvm")};
    {
      // A C program's pseudocode is C: no other language is offered.
      Workbench bench;
      bench.window->openFile(
          writeFixture(directory, QStringLiteral("pseudocode-import.bin")));
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      bench.window->disassembly()->navigate(Base + 0x140);
      QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                   std::optional<Address>(Base + 0x140));
      bench.action(ActionId::ViewPseudocode)->trigger();
      auto *view = bench.codeView(QStringLiteral("source"));
      QVERIFY(view);
      QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                   view->text()->lineCount() > 1,
                               OpenTimeoutMs);
      QCOMPARE(representationsOf(view), cOnly);
      QCOMPARE(view->text()->language(), QStringLiteral("c"));
      QCOMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
               QStringLiteral("Pseudocode-A"));
    }
    // A Go program reads in Go, says so, and counts what it shows as C.
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-go.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() &&
                                 code->language() == QLatin1String("go"),
                             OpenTimeoutMs);
    QCOMPARE(representationsOf(view),
             QStringList({QStringLiteral("source"), QStringLiteral("c"),
                          QStringLiteral("llvmc"), QStringLiteral("low"),
                          QStringLiteral("med"), QStringLiteral("high"),
                          QStringLiteral("llvm")}));
    QVERIFY(code->allText().contains(QStringLiteral("func main.main()")));
    QVERIFY(code->status().contains(QStringLiteral("shown as C")));
    QTRY_COMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
                 QStringLiteral("Pseudocode-A (Go)"));
  }

  void unknownEntryBrowsesMappedCodeAndShowsDiagnostics() {
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("unknown-entry.bin"));
    Workbench bench;
    QSignalSpy messages(&bench.session, &Session::message);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(bench.session.entryAddress(), Address(0));
    QTRY_VERIFY_WITH_TIMEOUT(
        bench.window->disassembly()->currentAddress().has_value(),
        OpenTimeoutMs);
    QVERIFY(*bench.window->disassembly()->currentAddress() >= Base);
    QVERIFY(*bench.window->disassembly()->currentAddress() < Base + 0x3000);
    bool warned = false;
    for (const auto &message : messages)
      warned |= message.first().toString() ==
                QLatin1String("Fixture PE entry is unknown.");
    QVERIFY(warned);
    QCOMPARE(bench.session.entryAddress(), Address(0));
  }

  void fileDropOpensFromWorkbenchViews_data() {
    QTest::addColumn<QString>("targetName");
    for (const auto *name : {"window", "listing", "functions", "hex", "code",
                             "output", "command", "floating"})
      QTest::newRow(name) << QString::fromLatin1(name);
  }

  void fileDropOpensFromWorkbenchViews() {
    QFETCH(QString, targetName);
    QTemporaryDir directory;
    const auto path = writeFixture(
        directory, QString::fromUtf8("\u4e2d\u6587 space #% fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    QWidget *target = bench.window.get();
    if (targetName == QLatin1String("listing"))
      target = bench.window->disassembly()->listing()->viewport();
    else if (targetName == QLatin1String("functions"))
      target = bench.functions()->table()->viewport();
    else if (targetName == QLatin1String("hex"))
      target = bench.window->findChild<HexView *>()->viewport();
    else if (targetName == QLatin1String("code")) {
      // Code docks created after startup must accept file drops too.
      bench.window->openFile(writeFixture(directory, "first.bin"));
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      bench.window->disassembly()->navigate(Base + 0x140);
      QTRY_VERIFY_WITH_TIMEOUT(
          bench.window->disassembly()->currentFunction().has_value(),
          OpenTimeoutMs);
      bench.action(ActionId::ViewPseudocode)->trigger();
      auto *view = bench.codeView(QStringLiteral("source"));
      QVERIFY(view);
      target = view->text()->viewport();
    } else if (targetName == QLatin1String("output"))
      target = bench.window->findChild<OutputWindow *>()
                   ->findChild<QPlainTextEdit *>()
                   ->viewport();
    else if (targetName == QLatin1String("command"))
      target =
          bench.window->findChild<OutputWindow *>()->findChild<QLineEdit *>();
    else if (targetName == QLatin1String("floating")) {
      auto *dock = qobject_cast<KDDockWidgets::QtWidgets::DockWidget *>(
          bench.functions()->parentWidget());
      QVERIFY(dock);
      dock->setFloating(true);
      target = bench.functions()->table()->viewport();
      QVERIFY(target->window() != bench.window.get());
    }
    QVERIFY(target);
    // A new file asks how to load it first.
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.identifiesFiles(), OpenTimeoutMs);
    LoadDialogAcceptor loadDialogs;
    QVERIFY(dropFile(target, path));
    QTRY_COMPARE_WITH_TIMEOUT(bench.session.filePath(), path, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(loadDialogs.accepted(), 1);
    QVERIFY(QFileInfo::exists(path));
    if (targetName == QLatin1String("command"))
      QVERIFY(static_cast<QLineEdit *>(target)->text().isEmpty());
  }

  void fileDropRejectsInvalidInputs() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    const QList<QList<QUrl>> rejected{
        {},
        {QUrl(QStringLiteral("https://example.com/fixture.bin"))},
        {QUrl::fromLocalFile(directory.path())},
        {QUrl::fromLocalFile(directory.filePath("missing.bin"))},
        {QUrl::fromLocalFile(path), QUrl::fromLocalFile(path)},
        {QUrl(QStringLiteral("file:relative.bin"))}};
    for (const auto &urls : rejected) {
      QMimeData mime;
      mime.setUrls(urls);
      QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime,
                            Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(bench.window.get(), &enter);
      QVERIFY(!enter.isAccepted());
    }
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    QDragEnterEvent moveOnly(QPoint(10, 10), Qt::MoveAction, &mime,
                             Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(bench.window.get(), &moveOnly);
    QVERIFY(!moveOnly.isAccepted());
    QMimeData text;
    text.setText(path);
    QDragEnterEvent plainText(QPoint(10, 10), Qt::CopyAction, &text,
                              Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(bench.window.get(), &plainText);
    QVERIFY(!plainText.isAccepted());
    QVERIFY(bench.session.filePath().isEmpty());
  }

  void automaticLayoutsPreserveMainWindow_data() {
    QTest::addColumn<bool>("maximized");
    QTest::addColumn<bool>("startupDesktop");
    QTest::newRow("normal-startup") << false << true;
    QTest::newRow("maximized-startup") << true << true;
    QTest::newRow("normal-project") << false << false;
    QTest::newRow("maximized-project") << true << false;
  }

  void automaticLayoutsPreserveMainWindow() {
    QFETCH(bool, maximized);
    QFETCH(bool, startupDesktop);
    Theme::instance().apply();
    QTemporaryDir directory;
    const auto path = writeFixture(directory, "startup-layout.bin");
    const auto desktop = directory.filePath("desktop.json");
    {
      Workbench previous;
      previous.window->setLayoutPath(desktop);
      previous.window->setGeometry(QRect(80, 90, 1200, 760));
      // Model a desktop saved while maximized, including its restore frame.
      auto layout = QJsonDocument::fromJson(
                        KDDockWidgets::LayoutSaver().serializeLayout())
                        .object();
      auto windows = layout.value("mainWindows").toArray();
      auto window = windows[0].toObject();
      window.insert("windowState", int(Qt::WindowMaximized));
      window.insert("isVisible", true);
      windows[0] = window;
      layout.insert("mainWindows", windows);
      QFile file(desktop);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(QJsonDocument(layout).toJson());
      QCOMPARE(ProjectDatabase::writeProjectFile(
                   path, ProjectDatabase::WorkbenchSuffix,
                   QJsonDocument(QJsonObject{{"desktop", layout}})),
               QString());
    }
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    if (startupDesktop)
      bench.window->setLayoutPath(desktop);
    bench.window->setGeometry(QRect(140, 150, 1300, 780));
    if (maximized)
      bench.window->setWindowState(Qt::WindowMaximized);
    const auto geometry = bench.window->geometry();
    const auto state = bench.window->windowState();
    bench.window->initializeLayout();
    // The desktop must not expose an unfinished window or override the
    // geometry already selected by the application's startup settings.
    QVERIFY(!bench.window->isVisible());
    QCOMPARE(bench.window->geometry(), geometry);
    QCOMPARE(bench.window->windowState(), state);
    bench.window->showWorkbench();
    QVERIFY(QTest::qWaitForWindowExposed(bench.window.get()));
    QTRY_COMPARE(bench.window->windowHandle()->windowState(),
                 maximized ? Qt::WindowMaximized : Qt::WindowNoState);
    const auto shownGeometry = bench.window->geometry();
    int visibleMoves = 0;
    struct Observer : QObject {
      std::function<void(QEvent *)> Observe;
      bool eventFilter(QObject *, QEvent *event) override {
        Observe(event);
        return false;
      }
    } observer;
    observer.Observe = [&](QEvent *event) {
      if (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
          event->type() == QEvent::WindowStateChange)
        ++visibleMoves;
    };
    bench.window->installEventFilter(&observer);
    LoadDialogAcceptor loads;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY(ListingViewTestAccess::settled(
        *bench.window->disassembly()->listing()));
    QCOMPARE(bench.window->geometry(), shownGeometry);
    QCOMPARE(bench.window->windowState(), state);
    QCOMPARE(visibleMoves, 0);
  }

  void quickStartMinimizesAndReopens_data() {
    QTest::addColumn<bool>("greeting");
    QTest::newRow("startup") << true;
    QTest::newRow("workbench") << false;
  }

  void quickStartMinimizesAndReopens() {
    QFETCH(bool, greeting);
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), !greeting);
    bench.window->showQuickStart();
    auto *dialog = visibleQuickStart();
    QVERIFY(dialog);
    QVERIFY(!QApplication::activeModalWidget());
    QVERIFY(QTest::qWaitForWindowExposed(dialog));
    const auto size = dialog->size();
    auto *minimize = dialog->findChild<QAbstractButton *>("minimizeButton");
    QVERIFY(minimize && minimize->isVisible());
    QTest::mouseClick(minimize, Qt::LeftButton);
    QTRY_VERIFY(dialog->isMinimized());
    // File > Quick start must restore the existing window on every desktop.
    bench.window->showQuickStart();
    QTRY_VERIFY(!dialog->isMinimized());
    QCOMPARE(visibleQuickStart(), dialog);
    QCOMPARE(dialog->size(), size);
    QCOMPARE(bench.window->isVisible(), !greeting);
  }

  void quickStartReusesItsWindowAndClosesWithTheWorkbench() {
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    bench.window->showQuickStart();
    QPointer<QuickStartDialog> dialog = visibleQuickStart();
    QVERIFY(dialog);
    QVERIFY(!QApplication::activeModalWidget());
    QVERIFY(!bench.window->isVisible());
    QVERIFY(!dialog->parentWidget());
    bench.window->showQuickStart();
    QCOMPARE(visibleQuickStart(), dialog.data());
    auto *go = buttonNamed(dialog, QStringLiteral("&Go"));
    QVERIFY(go);
    go->click();
    QTRY_VERIFY(bench.window->isVisible());
    QTRY_VERIFY(!dialog);
    bench.window->showQuickStart();
    dialog = visibleQuickStart();
    QVERIFY(dialog);
    QVERIFY(!QApplication::activeModalWidget());
    QCOMPARE(dialog->parentWidget(), bench.window.get());
    bench.window.reset();
    QVERIFY(!dialog);

    Workbench startup(QString::fromLocal8Bit(TEST_WORKER), false);
    startup.window->showQuickStart();
    dialog = visibleQuickStart();
    QVERIFY(dialog);
    startup.window.reset();
    QVERIFY(!dialog);
  }

  void quickStartQuitsWhileMinimized() {
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() != QLatin1String("cocoa"))
      QSKIP("Requires Cocoa application termination");
    // Run a real application event loop in a child so AppKit termination cannot
    // stop the surrounding test runner or leave its workers behind.
    if (!qEnvironmentVariableIsSet("NEVERD_TEST_MINIMIZED_QUIT")) {
      QProcess child;
      auto environment = QProcessEnvironment::systemEnvironment();
      environment.insert(QStringLiteral("NEVERD_TEST_MINIMIZED_QUIT"),
                         QStringLiteral("1"));
      child.setProcessEnvironment(environment);
      child.setProcessChannelMode(QProcess::MergedChannels);
      child.start(QCoreApplication::applicationFilePath(),
                  {QStringLiteral("quickStartQuitsWhileMinimized")});
      QVERIFY(child.waitForFinished(NativeOpenTimeoutMs));
      const auto output = child.readAll();
      QVERIFY2(child.exitStatus() == QProcess::NormalExit &&
                   child.exitCode() == 0,
               output.constData());
      return;
    }
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    bool minimized = false, timedOut = false;
    QTimer deadline;
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, [&] {
      timedOut = true;
      QCoreApplication::exit(1);
    });
    deadline.start(OpenTimeoutMs);
    QTimer::singleShot(0, this, [&] {
      auto *dialog = visibleQuickStart();
      QVERIFY(dialog);
      auto close = qScopeGuard([&] { dialog->close(); });
      auto *button = dialog->findChild<QAbstractButton *>("minimizeButton");
      QVERIFY(button && button->isVisible());
      QTest::mouseClick(button, Qt::LeftButton);
      QTRY_VERIFY(test::nativeWindowIsMinimized(*dialog));
      minimized = true;
      close.dismiss();
      test::quitNativeApplication();
    });
    bench.window->showQuickStart();
    const auto result = qApp->exec();
    QVERIFY(!timedOut);
    QVERIFY(minimized);
    QCOMPARE(result, 0);
#else
    QSKIP("Requires Cocoa application termination");
#endif
  }

  void quickStartRestoresFromTheDock_data() {
    QTest::addColumn<bool>("greeting");
    QTest::newRow("startup") << true;
    QTest::newRow("workbench") << false;
  }

  void quickStartRestoresFromTheDock() {
#ifdef Q_OS_MACOS
    if (QGuiApplication::platformName() != QLatin1String("cocoa"))
      QSKIP("Requires native Cocoa window minimization and application reopen");
    QFETCH(bool, greeting);
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), !greeting);
    bool exercised = false;
    QTimer::singleShot(0, this, [&] {
      auto *dialog = visibleQuickStart();
      QVERIFY(dialog);
      const auto close = qScopeGuard([&] { dialog->close(); });
      QVERIFY(activateTestWindow(dialog));
      // A direct widget click bypasses Cocoa's activation before native input.
      // Settle the first activation before testing a later Dock reopen.
      QTRY_COMPARE(QGuiApplication::applicationState(), Qt::ApplicationActive);
      auto *minimize = dialog->findChild<QAbstractButton *>("minimizeButton");
      QVERIFY(minimize && minimize->isVisible());
      const auto size = dialog->size();
      QTest::mouseClick(minimize, Qt::LeftButton);
      QTRY_VERIFY(test::nativeWindowIsMinimized(*dialog));
      QVERIFY(test::reopenNativeApplication());
      QTRY_VERIFY_WITH_TIMEOUT(!test::nativeWindowIsMinimized(*dialog), 3000);
      QTRY_VERIFY(dialog->isVisible() && !dialog->isMinimized());
      QCOMPARE(dialog->size(), size);
      QVERIFY(!test::hasNativeApplicationModalWindow());
      exercised = true;
    });
    bench.window->showQuickStart();
    QTRY_VERIFY(exercised);
#else
    QSKIP("Requires native Cocoa window minimization and application reopen");
#endif
  }

  void startupQuickStartDoesNotWaitForExposureOrWorker_data() {
    QTest::addColumn<bool>("go");
    QTest::newRow("close") << false;
    QTest::newRow("go") << true;
  }

  void startupQuickStartDoesNotWaitForExposureOrWorker() {
    QFETCH(bool, go);
    QTemporaryDir directory;
    Workbench bench(directory.filePath(QStringLiteral("missing-worker.exe")));
    // The initial exposure has already happened, and no engine can become
    // ready.
    QVERIFY(QTest::qWaitForWindowExposed(bench.window.get()));
    int shown = 0;
    bool clickedGo = false;
    QTimer close;
    connect(&close, &QTimer::timeout, this, [&] {
      auto *dialog = visibleQuickStart();
      if (!dialog)
        return;
      ++shown;
      if (go)
        for (auto *button : dialog->findChildren<QPushButton *>())
          if (button->text() == QStringLiteral("&Go")) {
            clickedGo = true;
            QTest::mouseClick(button, Qt::LeftButton);
            return;
          }
      dialog->reject();
    });
    close.start(5);
    bench.window->scheduleQuickStart();
    QTRY_COMPARE_WITH_TIMEOUT(shown, 1, 1000);
    QCOMPARE(clickedGo, go);
    QVERIFY(bench.window->isVisible());
    QVERIFY(bench.session.filePath().isEmpty());
    QVERIFY(!bench.session.loaded());
    QVERIFY(!QApplication::activeModalWidget());
    bench.window->hide();
    bench.window->show();
    QTest::qWait(50);
    QCOMPARE(shown, 1);
  }

  void openingAFileCancelsStartupQuickStart_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("before-worker-ready") << QStringLiteral("open");
    QTest::newRow("drop-before-queued-dialog") << QStringLiteral("drop");
    QTest::newRow("already-loaded") << QStringLiteral("loaded");
  }

  void openingAFileCancelsStartupQuickStart() {
    QFETCH(QString, mode);
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    if (mode == QLatin1String("drop"))
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.identifiesFiles(), OpenTimeoutMs);
    if (mode == QLatin1String("loaded")) {
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    }
    int shown = 0;
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, this, [&] {
      if (auto *dialog = visibleQuickStart()) {
        ++shown;
        dialog->reject();
      }
    });
    dismiss.start(5);
    LoadDialogAcceptor loadDialogs;
    bench.window->scheduleQuickStart();
    if (mode == QLatin1String("drop"))
      QVERIFY(dropFile(bench.window.get(), path));
    else if (mode == QLatin1String("open"))
      bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    // Process the startup callback even when the session was already loaded.
    QTest::qWait(50);
    QCOMPARE(shown, 0);
    QCOMPARE(bench.session.filePath(), path);
    // Explicitly opening Quick Start from the menu still works in a project.
    QTimer::singleShot(0, bench.window.get(), [&] {
      if (auto *dialog = visibleQuickStart()) {
        ++shown;
        dialog->reject();
      }
    });
    bench.window->showQuickStart();
    QTRY_COMPARE(shown, 1);
    QVERIFY(bench.session.loaded());
  }

  void startupQuickStartGreetsAlone_data() {
    QTest::addColumn<QString>("choice");
    QTest::addColumn<QString>("button");
    QTest::newRow("go") << QStringLiteral("go") << QStringLiteral("&Go");
    QTest::newRow("escape") << QStringLiteral("escape") << QString();
    QTest::newRow("new") << QStringLiteral("new") << QStringLiteral("&New");
    QTest::newRow("previous")
        << QStringLiteral("previous") << QStringLiteral("&Previous");
    QTest::newRow("drop") << QStringLiteral("drop") << QString();
    QTest::newRow("close") << QStringLiteral("close") << QString();
  }

  void startupQuickStartGreetsAlone() {
    // As IDA's: a startup without a file shows the quick start alone, as a
    // window of its own.  The workbench lays out its desktop and shows once
    // the user chooses how to start; closing the quick start quits instead,
    // without ever showing it.
    QFETCH(QString, choice);
    QFETCH(QString, button);
    // The file-selection timer drives QFileDialog's widgets.
    const bool nativeDialogsDisabled =
        QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const auto restoreDialogs = qScopeGuard([nativeDialogsDisabled] {
      QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs,
                                     nativeDialogsDisabled);
    });
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QSettings().setValue(settings::RecentFiles, QStringList{path});
    const auto restore = qScopeGuard([] {
      QSettings().remove(settings::RecentFiles);
      QSettings().remove(settings::RecentOpened);
    });
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    LoadDialogAcceptor loadDialogs;
    // New asks for the file in a dialog over the quick start.
    QTimer files;
    connect(&files, &QTimer::timeout, this, [&] {
      if (auto *dialog =
              qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
        dialog->selectFile(path);
        // selectFile preserves the name while its editor has the focus.
        if (auto *name =
                dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
          name->setText(path);
        // As pressing Open does; QFileDialog keeps its override protected.
        static_cast<QDialog *>(dialog)->accept();
      }
    });
    files.start(10);
    bool answered = false, alone = false, dropped = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      auto *dialog = visibleQuickStart();
      if (!dialog || answered)
        return;
      answered = true;
      alone = dialog->isVisible() && !dialog->parentWidget() &&
              !bench.window->isVisible();
      if (!button.isEmpty()) {
        if (auto *start = buttonNamed(dialog, button))
          start->click();
      } else if (choice == QLatin1String("escape")) {
        QTest::keyClick(dialog, Qt::Key_Escape);
      } else if (choice == QLatin1String("drop")) {
        // The drop opens the file on the next event-loop turn.
        dropped = dropFile(dialog, path);
      }
      // Close, or end a choice that failed to answer.
      if (!dropped && dialog->isVisible())
        dialog->close();
    });
    answer.start(5);
    bool startupTimedOut = false;
    QString startupState;
    QTimer deadline;
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, [&] {
      startupTimedOut = true;
      auto *modal = QApplication::activeModalWidget();
      startupState = QStringLiteral("modal=%1 answered=%2")
                         .arg(modal ? modal->metaObject()->className() : "none")
                         .arg(answered);
      if (auto *file = qobject_cast<QFileDialog *>(modal)) {
        auto *name =
            file->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
        startupState += QStringLiteral(" directory=%1 selected=%2 field=%3")
                            .arg(file->directory().path(),
                                 file->selectedFiles().join(QLatin1Char(';')),
                                 name ? name->text() : QStringLiteral("none"));
      }
      if (modal)
        modal->close();
    });
    deadline.start(OpenTimeoutMs);
    bench.window->scheduleQuickStart();
    QTRY_VERIFY_WITH_TIMEOUT(answered, 1000);
    deadline.stop();
    QVERIFY2(!startupTimedOut, qPrintable(startupState));
    QVERIFY(alone);
    QVERIFY(!QApplication::activeModalWidget());
    if (choice == QLatin1String("close")) {
      QVERIFY(!bench.window->isVisible());
      return;
    }
    // A chosen file shows the workbench as it starts to load.
    QTRY_VERIFY_WITH_TIMEOUT(bench.window->isVisible(), OpenTimeoutMs);
    QVERIFY(bench.functions() && bench.functions()->isVisible());
    if (choice == QLatin1String("go") || choice == QLatin1String("escape")) {
      QVERIFY(bench.session.filePath().isEmpty());
      return;
    }
    QCOMPARE(dropped, choice == QLatin1String("drop"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(QFileInfo(bench.session.filePath()).canonicalFilePath(),
             QFileInfo(path).canonicalFilePath());
  }

  void pdbQuestionAtStartupGreetsAlone() {
    // As the quick start: a PE named at startup asks whether to load the PDB
    // it names in a window of its own, and the workbench shows once the
    // user answered, as the file starts to load.
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("symbols-missing.bin"));
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    bool answered = false, alone = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      auto *dialog =
          qobject_cast<PdbPromptDialog *>(QApplication::activeModalWidget());
      if (!dialog || answered)
        return;
      answered = true;
      alone = dialog->isVisible() && !dialog->parentWidget() &&
              !bench.window->isVisible();
      if (auto *no = dialog->findChild<QPushButton *>(QStringLiteral("no")))
        no->click();
    });
    answer.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(answered, OpenTimeoutMs);
    QVERIFY(alone);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.window->isVisible());
    QCOMPARE(bench.session.metadata()
                 .value("pdb")
                 .toObject()
                 .value("mode")
                 .toString(),
             QStringLiteral("none"));
  }

  void pdbQuestionOnlyForNewProjects_data() {
    QTest::addColumn<QString>("state");
    QTest::newRow("new") << QStringLiteral("new");
    QTest::newRow("project-folder") << QStringLiteral("folder");
    QTest::newRow("packed-database") << QStringLiteral("database");
    QTest::newRow("legacy-sidecar") << QStringLiteral("legacy");
  }

  void pdbQuestionOnlyForNewProjects() {
    QFETCH(QString, state);
    QTemporaryDir directory;
    const auto input =
        writeFixture(directory, QStringLiteral("symbols-missing.bin"));
    auto path = input;
    if (state == QLatin1String("folder")) {
      QCOMPARE(ProjectDatabase::writeProjectFile(
                   input, ProjectDatabase::BookmarksSuffix,
                   QJsonDocument(QJsonArray())),
               QString());
    } else if (state == QLatin1String("database")) {
      path = ProjectDatabase::pathFor(input);
      QCOMPARE(ProjectDatabase::save(path, input, {}), QString());
    } else if (state == QLatin1String("legacy")) {
      QFile sidecar(input + QStringLiteral(".neverd-annotations.json"));
      QVERIFY(sidecar.open(QIODevice::WriteOnly));
      QCOMPARE(sidecar.write("[]"), qint64(2));
    }
    Workbench bench;
    int questions = 0;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      auto *dialog =
          qobject_cast<PdbPromptDialog *>(QApplication::activeModalWidget());
      if (dialog) {
        ++questions;
        dialog->reject();
      }
    });
    answer.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTest::qWait(OfferWaitMs);
    const int expected = state == QLatin1String("new") ? 1 : 0;
    QCOMPARE(questions, expected);
    bench.session.closeFile();
    QTRY_VERIFY(!bench.session.loaded());
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTest::qWait(OfferWaitMs);
    QCOMPARE(questions, expected);
  }

  void unattendedRunsAskNoPdbQuestion() {
    // A run nobody answers, such as a capture, loads as discovery finds.
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("symbols-missing.bin"));
    Workbench bench;
    bench.window->setAsksForDebugInfo(false);
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (qobject_cast<PdbPromptDialog *>(QApplication::activeModalWidget())) {
        asked = true;
        QApplication::activeModalWidget()->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    // Nor once the file lacks its PDB, which an answered run would offer.
    QTest::qWait(OfferWaitMs);
    QVERIFY(!asked);
  }

  void dwarfQuestionAtStartupGreetsAlone_data() {
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<bool>("accept");
    QTest::addColumn<QString>("kind");
    QTest::newRow("embedded, yes") << QStringLiteral("dwarf-embedded.bin")
                                   << true << QStringLiteral("dwarf");
    QTest::newRow("embedded, no") << QStringLiteral("dwarf-embedded.bin")
                                  << false << QStringLiteral("none");
    QTest::newRow("split, no")
        << QStringLiteral("dwarf-split.bin") << false << QStringLiteral("none");
  }

  void dwarfQuestionAtStartupGreetsAlone() {
    // As IDA asks "Load DWARF debug information?": an image with DWARF asks
    // in a window of its own before it loads, names the file that holds the
    // DWARF when that is not the input, and No opens it without the DWARF.
    QFETCH(QString, fixture);
    QFETCH(bool, accept);
    QFETCH(QString, kind);
    QTemporaryDir directory;
    const auto path = writeFixture(directory, fixture);
    const bool split = fixture == QLatin1String("dwarf-split.bin");
    if (split)
      QVERIFY(!writeFixture(directory, fixture + QStringLiteral(".debug"))
                   .isEmpty());
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    bool answered = false, alone = false, named = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      auto *dialog =
          qobject_cast<DwarfPromptDialog *>(QApplication::activeModalWidget());
      if (!dialog || answered)
        return;
      answered = true;
      alone = dialog->isVisible() && !dialog->parentWidget() &&
              !bench.window->isVisible();
      named = dialog->findChild<QLabel *>(QStringLiteral("path")) != nullptr;
      if (auto *button = dialog->findChild<QPushButton *>(
              accept ? QStringLiteral("yes") : QStringLiteral("no")))
        button->click();
    });
    answer.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(answered, OpenTimeoutMs);
    QVERIFY(alone);
    QCOMPARE(named, split);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.window->isVisible());
    const auto metadata = bench.session.metadata();
    QCOMPARE(metadata.value("debug_info").toObject().value("kind").toString(),
             kind);
    QCOMPARE(metadata.value("dwarf").toObject().value("mode").toString(),
             accept ? QString() : QStringLiteral("none"));
  }

  void unattendedRunsAskNoDwarfQuestion() {
    // A capture or benchmark reads the DWARF that discovery finds.
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("dwarf-embedded.bin"));
    Workbench bench;
    bench.window->setAsksForDebugInfo(false);
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (qobject_cast<DwarfPromptDialog *>(
              QApplication::activeModalWidget())) {
        asked = true;
        QApplication::activeModalWidget()->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!asked);
    QCOMPARE(bench.session.metadata()
                 .value("debug_info")
                 .toObject()
                 .value("kind")
                 .toString(),
             QStringLiteral("dwarf"));
  }

  void declinedDwarfStaysDeclinedInTheProject() {
    // The answer is kept with the project, as IDA keeps it in its database:
    // reopening the project asks nothing and reads no DWARF.
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("dwarf-embedded.bin"));
    const auto database = ProjectDatabase::pathFor(path);
    {
      Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
      QTimer answer;
      connect(&answer, &QTimer::timeout, this, [&] {
        if (auto *dialog = qobject_cast<DwarfPromptDialog *>(
                QApplication::activeModalWidget()))
          if (auto *no = dialog->findChild<QPushButton *>(QStringLiteral("no")))
            no->click();
      });
      answer.start(5);
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      answer.stop();
      // The answer is the project's as soon as it is given, in a file
      // beside the binary rather than a database.
      const auto debug =
          QJsonDocument::fromJson(readAll(path + ".neverd/debug.json"))
              .object();
      QCOMPARE(debug.value("dwarf").toObject().value("mode").toString(),
               QStringLiteral("none"));
      QVERIFY(!bench.session.dirty());
    }
    QVERIFY(!QFileInfo::exists(database));
    Workbench bench;
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (auto *dialog = qobject_cast<DwarfPromptDialog *>(
              QApplication::activeModalWidget())) {
        asked = true;
        dialog->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!asked);
    const auto metadata = bench.session.metadata();
    QCOMPARE(metadata.value("dwarf").toObject().value("mode").toString(),
             QStringLiteral("none"));
    QVERIFY(metadata.value("debug_info").toObject().value("kind").toString() !=
            QLatin1String("dwarf"));
  }

  /// Write \p text as the MAP beside the fixture \p fixture in \p directory.
  static bool writeMapBeside(const QTemporaryDir &directory,
                             const QString &fixture, const QByteArray &text) {
    QFile map(directory.filePath(QFileInfo(fixture).completeBaseName() +
                                 QStringLiteral(".map")));
    return map.open(QIODevice::WriteOnly) && map.write(text) == text.size();
  }

  void mapQuestionAtStartupGreetsAlone_data() {
    QTest::addColumn<QByteArray>("map");
    QTest::addColumn<bool>("accept");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("mode");
    QTest::newRow("same build, yes") << QByteArray("same build") << true
                                     << QStringLiteral("map") << QString();
    QTest::newRow("same build, no")
        << QByteArray("same build") << false << QStringLiteral("none")
        << QStringLiteral("none");
    QTest::newRow("other build, yes")
        << QByteArray("other build") << true << QStringLiteral("map")
        << QStringLiteral("unchecked");
  }

  void mapQuestionAtStartupGreetsAlone() {
    // An image no PDB or DWARF describes asks before it loads the MAP beside
    // it, in a window of its own that says which linker wrote the MAP and
    // whether it is of the input's build; one of another build defaults to
    // No, and Yes still loads it, as the user chose.
    QFETCH(QByteArray, map);
    QFETCH(bool, accept);
    QFETCH(QString, kind);
    QFETCH(QString, mode);
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("map.bin"));
    QVERIFY(writeMapBeside(directory, QStringLiteral("map.bin"), map));
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    bool answered = false, alone = false, cautious = false;
    QString description;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      auto *dialog =
          qobject_cast<MapPromptDialog *>(QApplication::activeModalWidget());
      if (!dialog || answered)
        return;
      answered = true;
      alone = dialog->isVisible() && !dialog->parentWidget() &&
              !bench.window->isVisible();
      if (auto *label =
              dialog->findChild<QLabel *>(QStringLiteral("description")))
        description = label->text();
      if (auto *no = dialog->findChild<QPushButton *>(QStringLiteral("no")))
        cautious = no->isDefault();
      if (auto *button = dialog->findChild<QPushButton *>(
              accept ? QStringLiteral("yes") : QStringLiteral("no")))
        button->click();
    });
    answer.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(answered, OpenTimeoutMs);
    QVERIFY(alone);
    QCOMPARE(cautious, map == "other build");
    QVERIFY2(description.startsWith(QLatin1String("link.exe MAP, 3 names.")),
             qPrintable(description));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.window->isVisible());
    const auto metadata = bench.session.metadata();
    QCOMPARE(metadata.value("debug_info").toObject().value("kind").toString(),
             kind);
    QCOMPARE(metadata.value("map").toObject().value("mode").toString(), mode);
  }

  void unattendedRunsAskNoMapQuestion() {
    // A capture or benchmark reads the MAP that discovery finds.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("map.bin"));
    QVERIFY(writeMapBeside(directory, QStringLiteral("map.bin"), "same build"));
    Workbench bench;
    bench.window->setAsksForDebugInfo(false);
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (qobject_cast<MapPromptDialog *>(QApplication::activeModalWidget())) {
        asked = true;
        QApplication::activeModalWidget()->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!asked);
    QCOMPARE(bench.session.metadata()
                 .value("debug_info")
                 .toObject()
                 .value("kind")
                 .toString(),
             QStringLiteral("map"));
  }

  void anAcceptedPdbLeavesTheMapUnasked() {
    // A PDB outranks the MAP beside the PE: once the user takes the PDB,
    // the MAP is not asked about, and the open reads no MAP before the PDB.
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("symbols-local.bin"));
    QFile pdb(directory.filePath(QStringLiteral("fixture.pdb")));
    QVERIFY(pdb.open(QIODevice::WriteOnly));
    pdb.write("matching fixture PDB");
    pdb.close();
    QVERIFY(writeMapBeside(directory, QStringLiteral("symbols-local.bin"),
                           "same build"));
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    bool answeredPdb = false, askedMap = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      auto *modal = QApplication::activeModalWidget();
      if (auto *dialog = qobject_cast<PdbPromptDialog *>(modal)) {
        answeredPdb = true;
        if (auto *yes = dialog->findChild<QPushButton *>(QStringLiteral("yes")))
          yes->click();
      } else if (qobject_cast<MapPromptDialog *>(modal)) {
        askedMap = true;
        modal->close();
      }
    });
    answer.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(answeredPdb);
    QVERIFY(!askedMap);
    const auto debug = bench.session.metadata().value("debug_info").toObject();
    QCOMPARE(debug.value("kind").toString(), QStringLiteral("pdb"));
    QVERIFY(debug.value("files").toArray().isEmpty());
  }

  void declinedMapStaysDeclinedInTheProject() {
    // The project keeps the answer, as it keeps the DWARF one: reopening it
    // asks nothing and reads no MAP.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("map.bin"));
    QVERIFY(writeMapBeside(directory, QStringLiteral("map.bin"), "same build"));
    const auto database = ProjectDatabase::pathFor(path);
    {
      Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
      QTimer answer;
      connect(&answer, &QTimer::timeout, this, [&] {
        if (auto *dialog = qobject_cast<MapPromptDialog *>(
                QApplication::activeModalWidget()))
          if (auto *no = dialog->findChild<QPushButton *>(QStringLiteral("no")))
            no->click();
      });
      answer.start(5);
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      answer.stop();
      // The answer is the project's as soon as it is given, in a file
      // beside the binary rather than a database.
      const auto debug =
          QJsonDocument::fromJson(readAll(path + ".neverd/debug.json"))
              .object();
      QCOMPARE(debug.value("map").toObject().value("mode").toString(),
               QStringLiteral("none"));
      QVERIFY(!bench.session.dirty());
    }
    QVERIFY(!QFileInfo::exists(database));
    Workbench bench;
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (auto *dialog = qobject_cast<MapPromptDialog *>(
              QApplication::activeModalWidget())) {
        asked = true;
        dialog->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!asked);
    const auto metadata = bench.session.metadata();
    QCOMPARE(metadata.value("map").toObject().value("mode").toString(),
             QStringLiteral("none"));
    QVERIFY(metadata.value("debug_info").toObject().value("kind").toString() !=
            QLatin1String("map"));
  }

  void loadFileOffersTheDebugFilesTheImageCanHave() {
    // File, Load file offers a PDB for a PE, DWARF for an ELF or Mach-O
    // image, and a MAP for any image; a chosen MAP loads beside the image's
    // other debug information.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("map.bin"));
    QFile map(directory.filePath(QStringLiteral("chosen.map")));
    QVERIFY(map.open(QIODevice::WriteOnly));
    map.write("same build");
    map.close();
    Workbench bench;
    bench.window->setAsksForDebugInfo(false);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *loadMap = bench.action(ActionId::FileLoadMap);
    auto *loadDwarf = bench.action(ActionId::FileLoadDwarf);
    QVERIFY(loadMap && loadDwarf);
    QTRY_VERIFY(loadMap->isEnabled());
    QCOMPARE(loadDwarf->isEnabled(),
             bench.session.format() == QLatin1String("ELF") ||
                 bench.session.format() == QLatin1String("Mach-O"));
    QSignalSpy changed(&bench.session, &Session::functionsChanged);
    bench.session.loadMap({{"path", map.fileName()}, {"mode", "checked"}});
    QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, OpenTimeoutMs);
    const auto metadata = bench.session.metadata();
    QCOMPARE(metadata.value("map").toObject().value("mode").toString(),
             QStringLiteral("checked"));
    const auto files =
        metadata.value("debug_info").toObject().value("files").toArray();
    QCOMPARE(files.size(), 1);
    QCOMPARE(files.first().toObject().value("kind").toString(),
             QStringLiteral("map"));
  }

  void symbolQuestionAtStartupGreetsAlone_data() {
    QTest::addColumn<bool>("accept");
    QTest::newRow("yes") << true;
    QTest::newRow("no") << false;
  }

  void symbolQuestionAtStartupGreetsAlone() {
    // Anyone can write names into a symbol table: an image with one asks
    // before it loads, in a window of its own that shows the first names it
    // holds, and No reads the image as if stripped, its functions unnamed.
    QFETCH(bool, accept);
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("symtab.bin"));
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    bool answered = false, alone = false;
    QString shown;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      auto *dialog = qobject_cast<SymbolTablePromptDialog *>(
          QApplication::activeModalWidget());
      if (!dialog || answered)
        return;
      answered = true;
      alone = dialog->isVisible() && !dialog->parentWidget() &&
              !bench.window->isVisible();
      if (auto *names = dialog->findChild<QLabel *>(QStringLiteral("names")))
        shown = names->toolTip();
      if (auto *button = dialog->findChild<QPushButton *>(
              accept ? QStringLiteral("yes") : QStringLiteral("no")))
        button->click();
    });
    answer.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(answered, OpenTimeoutMs);
    QVERIFY(alone);
    QCOMPARE(shown, QStringLiteral("function_0\nfunction_1\nfunction_2"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.window->isVisible());
    QCOMPARE(bench.session.metadata()
                 .value("symbols")
                 .toObject()
                 .value("mode")
                 .toString(),
             accept ? QString() : QStringLiteral("none"));
    // The function after the entry, which the analysis names "start" when
    // nothing else does, has only the table's name.
    auto &model = bench.functions()->model();
    QTRY_VERIFY(!model.rowObject(1).isEmpty());
    const QString second = model.rowObject(1).value("name").toString();
    QVERIFY2(accept ? second == QLatin1String("function_1")
                    : second.startsWith(QLatin1String("sub_")),
             qPrintable(second));
  }

  void unattendedRunsAskNoSymbolQuestion() {
    // A capture or benchmark reads the image's symbol table.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("symtab.bin"));
    Workbench bench;
    bench.window->setAsksForDebugInfo(false);
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (qobject_cast<SymbolTablePromptDialog *>(
              QApplication::activeModalWidget())) {
        asked = true;
        QApplication::activeModalWidget()->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!asked);
    QVERIFY(bench.session.metadata().value("symbols").isNull());
  }

  void declinedSymbolTableStaysDeclinedInTheProject() {
    // The project keeps the answer, as it keeps the DWARF one: reopening it
    // asks nothing and still reads the image as if stripped.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("symtab.bin"));
    const auto database = ProjectDatabase::pathFor(path);
    {
      Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
      QTimer answer;
      connect(&answer, &QTimer::timeout, this, [&] {
        if (auto *dialog = qobject_cast<SymbolTablePromptDialog *>(
                QApplication::activeModalWidget()))
          if (auto *no = dialog->findChild<QPushButton *>(QStringLiteral("no")))
            no->click();
      });
      answer.start(5);
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      answer.stop();
      // The answer is the project's as soon as it is given, in a file
      // beside the binary rather than a database.
      const auto debug =
          QJsonDocument::fromJson(readAll(path + ".neverd/debug.json"))
              .object();
      QCOMPARE(debug.value("symbols").toObject().value("mode").toString(),
               QStringLiteral("none"));
      QVERIFY(!bench.session.dirty());
    }
    QVERIFY(!QFileInfo::exists(database));
    Workbench bench;
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (auto *dialog = qobject_cast<SymbolTablePromptDialog *>(
              QApplication::activeModalWidget())) {
        asked = true;
        dialog->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!asked);
    QCOMPARE(bench.session.metadata()
                 .value("symbols")
                 .toObject()
                 .value("mode")
                 .toString(),
             QStringLiteral("none"));
  }

  void quickStartShowsRecentFilesAndStartsAsChosen() {
    // As IDA's Quick start: New, Go and Previous, the recent files and
    // whether it greets the next start.  Each recent file shows its format,
    // name, folder and when it was last opened.
    QTemporaryDir directory;
    const QString elf = directory.filePath(QStringLiteral("program"));
    {
      QFile file(elf);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(QByteArray("\x7F"
                            "ELF\x02\x01\x01\0",
                            8));
    }
    const QString missing = directory.filePath(QStringLiteral("gone.bin"));
    QSettings().setValue(settings::RecentFiles, QStringList{elf, missing});
    QSettings().setValue(settings::RecentOpened,
                         QVariantMap{{elf, QDateTime::currentDateTime()}});
    const auto restore = qScopeGuard([] {
      QSettings().remove(settings::RecentFiles);
      QSettings().remove(settings::RecentOpened);
      QSettings().remove(settings::QuickStart);
      QSettings().remove(settings::QuickStartSize);
    });

    QuickStartDialog dialog(noFile);
    auto *recent =
        dialog.findChild<QListWidget *>(QStringLiteral("quickStartRecent"));
    QVERIFY(recent);
    QCOMPARE(recent->count(), 2);
    QCOMPARE(recent->item(0)->text(), QStringLiteral("program"));
    QCOMPARE(recent->item(0)->data(Qt::UserRole + 1).toString(),
             QStringLiteral("ELF"));
    QVERIFY(recent->item(0)
                ->data(Qt::UserRole + 3)
                .toString()
                .startsWith(QStringLiteral("Today, ")));
    QCOMPARE(recent->item(1)->data(Qt::UserRole + 3).toString(),
             QStringLiteral("Missing"));

    // Delete forgets the selected file.
    recent->setCurrentRow(1);
    QTest::keyClick(recent, Qt::Key_Delete);
    QCOMPARE(recent->count(), 1);
    QCOMPARE(QSettings().value(settings::RecentFiles).toStringList(),
             QStringList{elf});

    // Display at startup is the setting the next start reads.
    auto *atStartup = dialog.findChild<QCheckBox *>();
    QVERIFY(atStartup && atStartup->isChecked());
    atStartup->setChecked(false);
    QVERIFY(!QSettings().value(settings::QuickStart, true).toBool());

    // Previous loads the selected file.
    QPushButton *previous = nullptr;
    for (auto *button : dialog.findChildren<QPushButton *>())
      if (button->text() == QStringLiteral("&Previous"))
        previous = button;
    QVERIFY(previous && previous->isEnabled() && previous->isDefault());
    recent->setCurrentRow(0);
    previous->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.start(), QuickStartDialog::Start::Previous);
    QCOMPARE(dialog.file(), elf);
  }

  void quickStartKeysFollowTheTranslatedAccessKeys() {
    // A translation moves a button's access key, and the key that presses
    // it without Alt and its key cap move with it: French "&Continuer"
    // takes C, and G no longer works on one's own.
    class French final : public QTranslator {
    public:
      bool isEmpty() const override { return false; }
      QString translate(const char *context, const char *source, const char *,
                        int) const override {
        if (qstrcmp(context, "neverd::gui::QuickStartDialog") == 0 &&
            qstrcmp(source, "&Go") == 0)
          return QStringLiteral("&Continuer");
        return {};
      }
    } french;
    QCoreApplication::installTranslator(&french);
    const auto restore =
        qScopeGuard([&] { QCoreApplication::removeTranslator(&french); });
    const auto pick = [](QWidget *) { return QString(); };

    QuickStartDialog ignored(pick);
    ignored.show();
    QVERIFY(QTest::qWaitForWindowExposed(&ignored));
    QVERIFY(activateTestWindow(&ignored));
    QTest::keyClick(&ignored, Qt::Key_G);
    QVERIFY(ignored.isVisible());
    QCOMPARE(ignored.start(), QuickStartDialog::Start::None);

    QuickStartDialog dialog(pick);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY(activateTestWindow(&dialog));
    QVERIFY(buttonNamed(&dialog, QStringLiteral("&Continuer")));
    QTest::keyClick(&dialog, Qt::Key_C);
    QVERIFY(!dialog.isVisible());
    QCOMPARE(dialog.start(), QuickStartDialog::Start::Go);
  }

  void quickStartWidensForALongerTranslation() {
    // A translation longer than English, here the longest catalog's, widens
    // the side rather than cut the action's description off.
    class German final : public QTranslator {
    public:
      bool isEmpty() const override { return false; }
      QString translate(const char *context, const char *source, const char *,
                        int) const override {
        if (qstrcmp(context, "neverd::gui::QuickStartDialog") == 0 &&
            qstrcmp(source, "Load the selected recent file") == 0)
          return QStringLiteral("Ausgewählte zuletzt verwendete Datei laden");
        return {};
      }
    } german;
    QCoreApplication::installTranslator(&german);
    const auto restore =
        qScopeGuard([&] { QCoreApplication::removeTranslator(&german); });
    QuickStartDialog dialog([](QWidget *) { return QString(); });
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    // Even at its smallest the dialog keeps the side's width.
    dialog.resize(dialog.minimumSize());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    const auto actions =
        dialog.findChildren<QPushButton *>(QStringLiteral("quickStartAction"));
    QCOMPARE(actions.size(), 3);
    for (const auto *action : actions)
      QVERIFY2(
          action->width() >= action->sizeHint().width(),
          qPrintable(QStringLiteral("%1: %2 < %3, side %4, font %5, dpi %6")
                         .arg(action->text())
                         .arg(action->width())
                         .arg(action->sizeHint().width())
                         .arg(action->parentWidget()->width())
                         .arg(action->font().toString())
                         .arg(action->logicalDpiX())));
    QVERIFY(actions.front()->parentWidget()->width() > 300);
  }

  void quickStartAsksForANewFileOverItself() {
    // New asks for the file over the quick start, which stays when the user
    // cancels.  Escape works on one's own as Go does; only closing the
    // window leaves the start unchosen.
    QWidget *askedOver = nullptr;
    QString picked;
    const auto pick = [&](QWidget *parent) {
      askedOver = parent;
      return picked;
    };
    QuickStartDialog dialog(pick);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *newButton = buttonNamed(&dialog, QStringLiteral("&New"));
    QVERIFY(newButton);
    newButton->click();
    QVERIFY(askedOver == &dialog);
    QVERIFY(dialog.isVisible());
    QCOMPARE(dialog.start(), QuickStartDialog::Start::None);
    picked = QStringLiteral("/data/program");
    newButton->click();
    QVERIFY(!dialog.isVisible());
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.start(), QuickStartDialog::Start::New);
    QCOMPARE(dialog.file(), picked);

    QuickStartDialog escaped(pick);
    escaped.show();
    QVERIFY(QTest::qWaitForWindowExposed(&escaped));
    QTest::keyClick(&escaped, Qt::Key_Escape);
    QVERIFY(!escaped.isVisible());
    QCOMPARE(escaped.start(), QuickStartDialog::Start::Go);

    QuickStartDialog closed(pick);
    closed.show();
    QVERIFY(QTest::qWaitForWindowExposed(&closed));
    QVERIFY(closed.close());
    QCOMPARE(closed.result(), int(QDialog::Rejected));
    QCOMPARE(closed.start(), QuickStartDialog::Start::None);
  }

  void quickStartResizesAndRemembersItsSize() {
    QSettings().remove(settings::QuickStartSize);
    const auto restore =
        qScopeGuard([] { QSettings().remove(settings::QuickStartSize); });
    QuickStartDialog dialog(noFile);
    dialog.resize(dialog.minimumSize());
    dialog.move(20, 20);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *recent =
        dialog.findChild<QListWidget *>(QStringLiteral("quickStartRecent"));
    auto *action =
        dialog.findChild<QPushButton *>(QStringLiteral("quickStartAction"));
    QVERIFY(recent && action);
    QVERIFY(dialog.findChild<TitleBar *>());
    QVERIFY(!dialog.isSizeGripEnabled());
    const QSize initialSize = dialog.size();
    const QSize initialListSize = recent->size();
    const QSize actionSize = action->size();

    // Native edge gestures are covered by the shared frame tests. Here the
    // dialog's content and saved size must follow the resulting geometry.
    dialog.resize(initialSize + QSize(24, 24));
    QTRY_VERIFY(dialog.width() > initialSize.width());
    QTRY_VERIFY(dialog.height() > initialSize.height());
    const QSize chosenSize = dialog.size();
    QCOMPARE(recent->size() - initialListSize, chosenSize - initialSize);
    QCOMPARE(action->size(), actionSize);

    dialog.showDropTarget(true);
    auto *drop =
        dialog.findChild<QWidget *>(QStringLiteral("quickStartDropTarget"));
    QVERIFY(drop && drop->isVisible());
    QCOMPARE(drop->geometry(), dialog.rect());
    dialog.resize(initialSize);
    QCOMPARE(drop->geometry(), dialog.rect());
    dialog.showDropTarget(false);
    dialog.resize(chosenSize);
    QTest::keyClick(&dialog, Qt::Key_Escape);

    QuickStartDialog reopened(noFile);
    QCOMPARE(reopened.size(), chosenSize);
    reopened.resize(1, 1);
    QCOMPARE(reopened.size(), reopened.minimumSize());
  }

  void fileDropOpensFromQuickStart() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    QTimer drag;
    bool accepted = false;
    connect(&drag, &QTimer::timeout, this, [&] {
      auto *dialog = visibleQuickStart();
      if (!dialog)
        return;
      drag.stop();
      accepted = dropFile(dialog, path);
      if (!accepted)
        dialog->reject();
    });
    drag.start(10);
    bench.window->showQuickStart();
    QTRY_VERIFY(accepted);
    QTRY_COMPARE_WITH_TIMEOUT(bench.session.filePath(), path, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!QApplication::activeModalWidget());
  }

  void quickStartShowsWhatADropWouldDo() {
    // A file dragged over the quick start shows that a drop opens it, until
    // it leaves; a folder, which cannot open, shows nothing.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    QTimer drag;
    bool shown = false, hidden = false, folderShown = true;
    connect(&drag, &QTimer::timeout, this, [&] {
      auto *dialog = visibleQuickStart();
      if (!dialog)
        return;
      drag.stop();
      auto *target =
          dialog->findChild<QWidget *>(QStringLiteral("quickStartDropTarget"));
      const auto enter = [dialog](const QString &dragged) {
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(dragged)});
        QDragEnterEvent event(QPoint(10, 10), Qt::CopyAction, &mime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(dialog, &event);
      };
      QDragLeaveEvent leave;
      enter(path);
      shown = target && target->isVisible();
      QApplication::sendEvent(dialog, &leave);
      hidden = target && !target->isVisible();
      enter(directory.path());
      folderShown = target && target->isVisible();
      dialog->reject();
    });
    drag.start(10);
    bench.window->showQuickStart();
    QTRY_VERIFY(shown);
    QVERIFY(hidden);
    QVERIFY(!folderShown);
  }

  void reloadReadsTheInputFileAgainWithItsSavedComments() {
    // As IDA's File, Load file, Reload the input file: a fresh worker reads
    // the file again, with the comments that were saved; unsaved ones go
    // when the user discards them.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.session.setComment(Base, QStringLiteral("saved comment"));
    QTRY_VERIFY(bench.session.dirty());
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path + ".neverd/annotations.json"),
                             OpenTimeoutMs);
    QTRY_VERIFY(!bench.session.dirty());
    bench.session.setComment(Base + 0x140, QStringLiteral("unsaved comment"));
    QTRY_VERIFY(bench.session.dirty());

    const quint64 epoch = bench.session.epoch();
    LoadDialogAcceptor loadDialogs;
    QTimer discard;
    bool prompted = false;
    connect(&discard, &QTimer::timeout, this, [&] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        prompted = true;
        box->button(QMessageBox::Discard)->click();
      }
    });
    discard.start(10);
    bench.action(ActionId::FileReload)->trigger();
    QTRY_VERIFY(prompted);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded() &&
                                 bench.session.epoch() != epoch,
                             OpenTimeoutMs);
    QCOMPARE(bench.session.filePath(), path);
    QVERIFY(!bench.session.dirty());
    const auto saved = readAll(path + ".neverd/annotations.json");
    QVERIFY(saved.contains("saved comment"));
    QVERIFY(!saved.contains("unsaved comment"));
    auto *log = bench.window->findChild<OutputWindow *>()
                    ->findChild<QPlainTextEdit *>();
    QVERIFY(log);
    QTRY_VERIFY(
        log->toPlainText().contains(QStringLiteral("Reloaded the input file")));
  }

  void fileInAReadOnlyFolderKeepsItsProjectInTheDataDirectory() {
    // IDA asks for another place for its database when the input's folder
    // is read-only.  The workbench opens the file anyway: its database,
    // sidecars and lock sit in the user's data directory, beside a copy.
    QTemporaryDir data;
    const QByteArray dataHome = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QFile::setPermissions(directory.path(),
                          QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    const auto restore = qScopeGuard([&] {
      QFile::setPermissions(directory.path(), QFileDevice::ReadOwner |
                                                  QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner);
      if (dataHome.isNull())
        qunsetenv("XDG_DATA_HOME");
      else
        qputenv("XDG_DATA_HOME", dataHome);
    });
    if (QFileInfo(directory.path()).isWritable())
      QSKIP("This user can write to a read-only folder");
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(bench.session.projectPath(), path);
    const QString copy = bench.session.filePath();
    // XDG_DATA_HOME moves the data directory on Linux; macOS and Windows
    // keep their own, which Qt names.
    QVERIFY2(copy.startsWith(QStandardPaths::writableLocation(
                 QStandardPaths::AppLocalDataLocation)),
             qPrintable(copy));
    QVERIFY2(!copy.startsWith(directory.path()), qPrintable(copy));
    QCOMPARE(readAll(copy), readAll(path));
    auto *log = bench.window->findChild<OutputWindow *>()
                    ->findChild<QPlainTextEdit *>();
    QVERIFY(log);
    QTRY_VERIFY(log->toPlainText().contains(
        QStringLiteral("is in a folder you cannot write to")));

    bench.session.setComment(Base, QStringLiteral("kept elsewhere"));
    QTRY_VERIFY(bench.session.dirty());
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(copy + ".neverd/annotations.json"),
                             OpenTimeoutMs);
    QVERIFY(!QFile::exists(path + ".neverd/annotations.json"));
    QTRY_VERIFY(!bench.session.dirty());

    // Reloading reads the file the user opened, with what was saved.
    const quint64 epoch = bench.session.epoch();
    LoadDialogAcceptor loadDialogs;
    bench.action(ActionId::FileReload)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded() &&
                                 bench.session.epoch() != epoch,
                             OpenTimeoutMs);
    QCOMPARE(bench.session.projectPath(), path);
    QCOMPARE(bench.session.filePath(), copy);
    QVERIFY(
        readAll(copy + ".neverd/annotations.json").contains("kept elsewhere"));
  }

  void fileDropKeepsUnsavedChangesWhenCancelled() {
    QTemporaryDir directory;
    const auto first = writeFixture(directory, QStringLiteral("first.bin"));
    const auto second = writeFixture(directory, QStringLiteral("second.bin"));
    Workbench bench;
    bench.window->openFile(first);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.session.setComment(Base, QStringLiteral("unsaved comment"));
    QTRY_VERIFY(bench.session.dirty());
    LoadDialogAcceptor loadDialogs;
    QTimer cancel;
    bool prompted = false;
    connect(&cancel, &QTimer::timeout, this, [&] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        prompted = true;
        box->reject();
      }
    });
    cancel.start(10);
    QVERIFY(dropFile(bench.window.get(), second));
    QTRY_VERIFY(prompted);
    QCOMPARE(bench.session.filePath(), first);
    QVERIFY(bench.session.loaded());
    QVERIFY(bench.session.dirty());
  }

  void initTestCase() {
    QVERIFY(settingsDirectory_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("NeverDTests"));
    QCoreApplication::setApplicationName(QStringLiteral("WorkbenchTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory_.path());
    configureDocking();
    WindowFrame::frameDialogs(*qApp);
    if (privateWayland()) {
      QTRY_VERIFY_WITH_TIMEOUT(
          privateShell(QStringLiteral("if (Main.layoutManager._startingUp) "
                                      "throw Error('desktop starting');")),
          NativeOpenTimeoutMs);
      QVERIFY(privateShell(QStringLiteral(
          "global.neverdWorkbenchPointer = "
          "imports.gi.Clutter.get_default_backend().get_default_seat()"
          ".create_virtual_device(imports.gi.Clutter.InputDeviceType.POINTER_"
          "DEVICE); global.neverdWorkbenchPointer.notify_absolute_motion("
          "imports.gi.GLib.get_monotonic_time(), global.stage.width / 2, "
          "global.stage.height / 2); Main.overview.hide();")));
    }
  }

  void windowChromeLeavesRoomToType() {
    Workbench bench;
    // The window manager resizes the window from its edges.
    QVERIFY(!bench.window->statusBar()->isSizeGripEnabled());
    // Input dialogs leave room for a full name, comment or expression.
    QInputDialog input(bench.window.get());
    input.show();
    QVERIFY(QTest::qWaitForWindowExposed(&input));
    QVERIFY2(input.width() >= 420, qPrintable(QString::number(input.width())));
  }

  void localTypesLayOutAndRenderTheProgramsTypes() {
    QTemporaryDir directory;
    // A choice kept before exact widths became the default starts over.
    QSettings().setValue(QStringLiteral("localTypes/spelling"),
                         QStringLiteral("ida"));
    QSettings().setValue(QStringLiteral("localTypes/luaSpelling"),
                         QStringLiteral("windows"));
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-types.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.action(ActionId::ViewLocalTypes)->trigger();
    auto *types = bench.window->findChild<LocalTypesView *>();
    QVERIFY(types);
    auto *list =
        types->findChild<QTreeView *>(QStringLiteral("localTypesList"));
    auto *layout =
        types->findChild<QTreeWidget *>(QStringLiteral("localTypesLayout"));
    auto *source =
        types->findChild<QPlainTextEdit *>(QStringLiteral("localTypesSource"));
    auto *filter =
        types->findChild<QLineEdit *>(QStringLiteral("localTypesFilter"));
    QVERIFY(list && layout && source && filter);
    QTRY_COMPARE(list->model()->rowCount(), 3);
    // The filter has the focus, as IDA's quick filter does.
    QTRY_VERIFY(filter->hasFocus());

    // The list leaves the ordinal and the member count out until asked.
    auto *listHeader = list->header();
    QVERIFY(listHeader->isSectionHidden(0));
    QVERIFY(!listHeader->isSectionHidden(1));
    QVERIFY(listHeader->isSectionHidden(4));

    // A record shows its members, the padding between and the union it
    // holds, the union's bitfield at its bit: where each lies first, as a
    // memory map reads, the tree in the member's column.
    types->showType(QStringLiteral("_DEVICE"));
    QTRY_COMPARE(types->currentType(), QStringLiteral("_DEVICE"));
    QTRY_COMPARE(layout->topLevelItemCount(), 4);
    QCOMPARE(layout->headerItem()->text(0), QStringLiteral("Offset"));
    QCOMPARE(layout->headerItem()->text(2), QStringLiteral("Member"));
    QCOMPARE(layout->treePosition(), 2);
    QCOMPARE(layout->columnCount(), 4);
    QCOMPARE(layout->topLevelItem(0)->text(2), QStringLiteral("Type"));
    QCOMPARE(layout->topLevelItem(1)->text(2), QStringLiteral("(padding)"));
    QCOMPARE(layout->topLevelItem(1)->text(1), QStringLiteral("0x6"));
    QCOMPARE(layout->topLevelItem(3)->text(2), QStringLiteral("union"));
    QCOMPARE(layout->topLevelItem(3)->child(1)->text(0),
             QStringLiteral("0x18.0"));
    QCOMPARE(layout->topLevelItem(3)->child(1)->text(1), QStringLiteral(":1"));
    // What only source takes is not on the layout's options.
    auto *dependencies = types->findChild<QCheckBox *>();
    QVERIFY(dependencies);
    for (auto *box : types->findChildren<QCheckBox *>())
      if (box->text() != QLatin1String("Hex offsets"))
        QVERIFY2(box->isHidden(), qPrintable(box->text()));

    // A member's type opens on a double click; Back returns.
    auto *links = layout->topLevelItem(2);
    QCOMPARE(links->text(2), QStringLiteral("Links"));
    emit layout->itemDoubleClicked(links, 0);
    QTRY_COMPARE(types->currentType(), QStringLiteral("_LIST_ENTRY"));
    QTest::keyClick(layout, Qt::Key_Escape);
    QTRY_COMPARE(types->currentType(), QStringLiteral("_DEVICE"));

    // The source in the language of its tab, with the options asked: C/C++
    // shows C, and each language chooses among its own spellings, apart
    // from the others'.
    auto *spelling =
        types->findChild<QComboBox *>(QStringLiteral("localTypesSpelling"));
    QVERIFY(spelling);
    const auto choices = [spelling] {
      QStringList ids;
      for (int index = 0; index < spelling->count(); ++index)
        ids.append(spelling->itemData(index).toString());
      return ids;
    };
    // Exact widths come first, every language's default.
    const QStringList twoSpellings{QStringLiteral("portable"),
                                   QStringLiteral("windows")};
    types->setPage(2);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language rust spelling portable")));
    QVERIFY(!spelling->isHidden());
    QCOMPARE(choices(), twoSpellings);
    spelling->setCurrentIndex(1);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language rust spelling windows")));
    types->setPage(3);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language go spelling portable")));
    QVERIFY(!spelling->isHidden());
    QCOMPARE(choices(), twoSpellings);
    QCOMPARE(spelling->itemText(0), QStringLiteral("Exact widths (uint32)"));
    spelling->setCurrentIndex(1);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language go spelling windows")));
    // Python renders through ctypes, with its own two spellings.
    types->setPage(4);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language python spelling portable")));
    QVERIFY(!spelling->isHidden());
    QCOMPARE(choices(), twoSpellings);
    QCOMPARE(spelling->itemText(0), QStringLiteral("Exact widths (c_uint32)"));
    // JavaScript whose JSDoc TypeScript checks, as one tab.
    const auto *tabBar =
        types->findChild<QTabBar *>(QStringLiteral("localTypesTabs"));
    QVERIFY(tabBar);
    QCOMPARE(tabBar->tabText(5), QStringLiteral("JS/TS"));
    types->setPage(5);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language javascript spelling portable")));
    QCOMPARE(choices(), twoSpellings);
    QCOMPARE(spelling->itemText(0),
             QStringLiteral("JavaScript types (number, bigint)"));
    // Lua, whose spelling names the C types its comments give.
    QCOMPARE(tabBar->tabText(6), QStringLiteral("Lua"));
    types->setPage(6);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language lua spelling portable")));
    QCOMPARE(choices(), twoSpellings);
    types->setPage(1);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language c spelling portable")));
    QVERIFY(source->toPlainText().contains(QStringLiteral("struct _DEVICE")));
    QVERIFY(!spelling->isHidden());
    for (auto *box : types->findChildren<QCheckBox *>())
      QVERIFY2(!box->isHidden(), qPrintable(box->text()));
    QCOMPARE(choices(),
             (QStringList{QStringLiteral("portable"), QStringLiteral("windows"),
                          QStringLiteral("keywords")}));
    QCOMPARE(spelling->currentData().toString(), QStringLiteral("portable"));
    QCOMPARE(QSettings().value(QStringLiteral("localTypes/view")).toString(),
             QStringLiteral("c"));
    QCOMPARE(QSettings()
                 .value(QStringLiteral("localTypes/spellings/rust"))
                 .toString(),
             QStringLiteral("windows"));
    QCOMPARE(
        QSettings().value(QStringLiteral("localTypes/spellings/go")).toString(),
        QStringLiteral("windows"));
    QVERIFY(!QSettings().contains(QStringLiteral("localTypes/spelling")));
    QVERIFY(!QSettings().contains(QStringLiteral("localTypes/luaSpelling")));

    // The command line shows a type by name, and says when none has it.
    auto *output = bench.window->findChild<OutputWindow *>();
    QVERIFY(output);
    output->run(QStringLiteral("t _LIST_ENTRY"));
    QTRY_COMPARE(types->currentType(), QStringLiteral("_LIST_ENTRY"));
    output->run(QStringLiteral("types _NO_SUCH_TYPE"));
    QTRY_VERIFY(types->findChild<QLabel *>(QStringLiteral("localTypesSummary"))
                    ->text()
                    .contains(QStringLiteral("_NO_SUCH_TYPE")));

    // The filter keeps the types whose name holds its text.
    filter->setText(QStringLiteral("list"));
    QTRY_COMPARE(list->model()->rowCount(), 1);
    types->setPage(0);
  }

  void localTypesShowKindsPathsAndCards() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-types.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    // A local Esc must win even when the disassembly has a place to return to.
    QTRY_VERIFY(bench.window->disassembly()->currentAddress().has_value());
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_VERIFY(bench.action(ActionId::JumpBack)->isEnabled());
    bench.action(ActionId::ViewLocalTypes)->trigger();
    auto *types = bench.window->findChild<LocalTypesView *>();
    QVERIFY(types);
    auto *list =
        types->findChild<QTreeView *>(QStringLiteral("localTypesList"));
    auto *layout =
        types->findChild<QTreeWidget *>(QStringLiteral("localTypesLayout"));
    auto *source =
        types->findChild<QPlainTextEdit *>(QStringLiteral("localTypesSource"));
    auto *path = types->findChild<QLabel *>(QStringLiteral("localTypesPath"));
    QVERIFY(list && layout && source && path);
    QTRY_COMPARE(list->model()->rowCount(), 3);

    // Each type shows the icon of its kind, and hovering its name says what
    // it is, how big and where it comes from.
    for (int row = 0; row < list->model()->rowCount(); ++row) {
      const QModelIndex name = list->model()->index(row, 1);
      QVERIFY2(!name.data(Qt::DecorationRole).value<QIcon>().isNull(),
               qPrintable(name.data().toString()));
      QVERIFY(name.data(Qt::ToolTipRole)
                  .toString()
                  .contains(
                      QStringLiteral("<b>%1</b>").arg(name.data().toString())));
    }
    const QModelIndexList device =
        list->model()->match(list->model()->index(0, 1), Qt::DisplayRole,
                             QStringLiteral("_DEVICE"), 1, Qt::MatchExactly);
    QCOMPARE(device.size(), 1);
    const QString card = device.first().data(Qt::ToolTipRole).toString();
    QVERIFY2(card.contains(QStringLiteral("structure")) &&
                 card.contains(QStringLiteral("bytes")),
             qPrintable(card));

    // The breadcrumb names the records that hold the member selected, and
    // a crumb selects the record it names.
    types->showType(QStringLiteral("_DEVICE"));
    QTRY_COMPARE(layout->topLevelItemCount(), 4);
    QTreeWidgetItem *bit = layout->topLevelItem(3)->child(1);
    layout->setCurrentItem(bit);
    QVERIFY(path->isVisibleTo(types));
    QVERIFY2(path->text().contains(QStringLiteral("_DEVICE")) &&
                 path->text().contains(QStringLiteral("union")) &&
                 path->text().contains(bit->text(2)),
             qPrintable(path->text()));
    emit path->linkActivated(QStringLiteral("3/"));
    QCOMPARE(layout->currentItem(), layout->topLevelItem(3));
    emit path->linkActivated(QString());
    QCOMPARE(layout->currentItem(), nullptr);
    QVERIFY(!path->isVisibleTo(types));
    // A member's row says where it lies.
    QVERIFY2(bit->toolTip(2).contains(QStringLiteral("bits")),
             qPrintable(bit->toolTip(2)));
    QVERIFY(layout->topLevelItem(2)->toolTip(2).contains(
        QStringLiteral("<b>Links</b>")));

    // The member selected shows its line in the source, which marks the line
    // its cursor is on.
    layout->setCurrentItem(layout->topLevelItem(2));
    types->setPage(1);
    QTRY_VERIFY(
        source->textCursor().block().text().contains(QStringLiteral("Links;")));
    QCOMPARE(source->extraSelections().size(), 1);
    QCOMPARE(source->extraSelections().first().cursor.block(),
             source->textCursor().block());

    // Hovering a type's name in the source says what it is; Ctrl+click opens
    // it, as an editor follows a definition.
    const QTextCursor word =
        source->document()->find(QStringLiteral("_LIST_ENTRY"));
    QVERIFY(!word.isNull());
    QTextCursor inside(source->document());
    inside.setPosition(word.selectionStart() + 3);
    const QPoint point = source->cursorRect(inside).center();
    QHelpEvent help(QEvent::ToolTip, point,
                    source->viewport()->mapToGlobal(point));
    QApplication::sendEvent(source->viewport(), &help);
    QTRY_VERIFY(QToolTip::text().contains(QStringLiteral("_LIST_ENTRY")));
    QTest::mouseClick(source->viewport(), Qt::LeftButton, Qt::ControlModifier,
                      point);
    QTRY_COMPARE(types->currentType(), QStringLiteral("_LIST_ENTRY"));
    QTest::keyClick(source, Qt::Key_Escape);
    QTRY_COMPARE(types->currentType(), QStringLiteral("_DEVICE"));

    // So does Ctrl+click on a member's type in the layout.
    types->setPage(0);
    QTRY_COMPARE(layout->topLevelItemCount(), 4);
    QTreeWidgetItem *links = layout->topLevelItem(2);
    const QRect row = layout->visualItemRect(links);
    QTest::mouseClick(layout->viewport(), Qt::LeftButton, Qt::ControlModifier,
                      row.center());
    QTRY_COMPARE(types->currentType(), QStringLiteral("_LIST_ENTRY"));
  }

  void structureOffsetsRankTypesByTheCodesAccesses() {
    // T on `mov rax, [rcx+8]` lists the local types by how function_7's
    // accesses through rcx land on their members, the one that holds them
    // all first and previewed as the operand becomes; Apply shows the
    // operand, and every other access through rcx, as members of it in one
    // step of the history.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-types.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.session.showsStructOffsets());
    auto *disassembly = bench.window->findChild<DisassemblyView *>();
    QVERIFY(disassembly);
    const Address read = Base + 0x70;
    const auto lineAt = [&](Address at) {
      disassembly->navigate(at);
      return disassembly->listing()->currentLineText();
    };
    disassembly->navigate(read);
    disassembly->focusContent();
    QTRY_COMPARE(disassembly->currentItem(), std::optional<Address>(read));
    QTRY_VERIFY(disassembly->listing()->currentLineText().contains(
        QLatin1String("[rcx+8]")));
    QTRY_VERIFY(bench.action(ActionId::EditOperandStruct)->isEnabled());

    QStringList listed;
    QString expression, every, summary;
    auto *picker = new QTimer(&bench.session);
    connect(picker, &QTimer::timeout, picker, [&, picker] {
      auto *dialog =
          qobject_cast<TypePickerDialog *>(QApplication::activeModalWidget());
      if (!dialog || !dialog->isActiveWindow())
        return;
      auto *list =
          dialog->findChild<QListWidget *>(QStringLiteral("typePickerList"));
      auto *search =
          dialog->findChild<QLineEdit *>(QStringLiteral("typePickerSearch"));
      if (!list || !search || list->count() < 2 || !search->hasFocus())
        return;
      picker->stop();
      picker->deleteLater();
      for (int row = 0; row < list->count(); ++row)
        listed << list->item(row)->text();
      expression =
          dialog->findChild<QLabel *>(QStringLiteral("typePickerExpression"))
              ->text();
      summary = dialog->findChild<QLabel *>(QStringLiteral("typePickerSummary"))
                    ->text();
      auto *box =
          dialog->findChild<QCheckBox *>(QStringLiteral("typePickerEvery"));
      every = box->isVisibleTo(dialog) ? box->text() : QString();
      box->setChecked(true);
      QTest::keyClick(search, Qt::Key_Return);
    });
    picker->start(10);
    bench.action(ActionId::EditOperandStruct)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(
        listed,
        (QStringList{QStringLiteral("_DEVICE"), QStringLiteral("_LIST_ENTRY")}),
        OpenTimeoutMs);
    QCOMPARE(expression, QStringLiteral("[rcx+_DEVICE.Links.Flink]"));
    QCOMPARE(every, QStringLiteral("Also show the 2 other instructions that "
                                   "go through rcx as members"));
    QCOMPARE(summary, QStringLiteral("2 types"));
    QTRY_VERIFY_WITH_TIMEOUT(
        lineAt(read).contains(QLatin1String("[rcx+_DEVICE.Links.Flink]")),
        OpenTimeoutMs);
    QTRY_VERIFY(lineAt(read + 1).contains(QLatin1String("[rcx+_DEVICE.Type]")));
    QTRY_VERIFY(
        lineAt(read + 2).contains(QLatin1String("[rcx+_DEVICE.Links.Blink]")));
    // The number of `add rcx, 18h` stays: no access went through it.
    QTRY_VERIFY(lineAt(read + 3).contains(QLatin1String("rcx, 18h")));

    // One undo takes the three back.
    // Updated listing rows can arrive before the separate history query.
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.canUndo(), OpenTimeoutMs);
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(
        lineAt(read + 2).contains(QLatin1String("[rcx+10h]")), OpenTimeoutMs);
    QTRY_VERIFY(lineAt(read).contains(QLatin1String("[rcx+8]")));
    QVERIFY2(bench.session.lastError().isEmpty(),
             qPrintable(bench.session.lastError()));
  }

  void pseudocodeParametersTakeTypesByConvertAndY() {
    // In function_7's pseudocode, Convert to struct * on arg0 ranks the local
    // types by the reads through rcx and makes it a _DEVICE * under the name
    // the user gives, whose members the code then reads; Y on arg1 completes
    // a type's name, refuses one the program lacks and states one that
    // reads; Y on the function shows its declaration and hands it back to
    // analysis.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-types.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.session.convertsParameters());
    const Address function = Base + 0x70;
    bench.window->disassembly()->navigate(function);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() &&
                                 code->allText().contains("void* arg0"),
                             OpenTimeoutMs);
    const auto onParameter = [&](const QString &name, int position) {
      code->setCursorLine(0);
      if (!code->findText(name, true))
        return false;
      code->setFocus();
      const auto target = code->renameTarget();
      return target && target->value("parameter").toInt(-1) == position;
    };
    QTRY_VERIFY(onParameter(QStringLiteral("arg0"), 0));
    // The commands follow the view the focus moves to, at once: T is the
    // disassembly's, Convert to struct * the pseudocode's.
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    bench.window->disassembly()->focusContent();
    QVERIFY(bench.action(ActionId::EditOperandStruct)->isEnabled());
    QVERIFY(!bench.action(ActionId::EditConvertToStruct)->isEnabled());
    code->setFocus();
    QVERIFY(bench.action(ActionId::EditConvertToStruct)->isEnabled());
    QVERIFY(!bench.action(ActionId::EditOperandStruct)->isEnabled());

    QStringList listed;
    QString member;
    bool pickerRan = false;
    auto *picker = new QTimer(&bench.session);
    connect(picker, &QTimer::timeout, picker, [&, picker] {
      auto *dialog =
          qobject_cast<TypePickerDialog *>(QApplication::activeModalWidget());
      if (!dialog || !dialog->isActiveWindow())
        return;
      auto *list =
          dialog->findChild<QListWidget *>(QStringLiteral("typePickerList"));
      auto *search =
          dialog->findChild<QLineEdit *>(QStringLiteral("typePickerSearch"));
      auto *name =
          dialog->findChild<QLineEdit *>(QStringLiteral("typePickerName"));
      if (!list || !search || !name || list->count() < 2 || !search->hasFocus())
        return;
      pickerRan = true;
      picker->stop();
      picker->deleteLater();
      for (int row = 0; row < list->count(); ++row)
        listed << list->item(row)->text();
      name->setText(QStringLiteral("Device"));
      member = dialog->findChild<QLabel *>(QStringLiteral("typePickerMember"))
                   ->text();
      QTest::keyClick(search, Qt::Key_Return);
    });
    picker->start(10);
    bool pickerTimedOut = false;
    QString pickerState;
    int stage = 0;
    QTimer pickerDeadline;
    pickerDeadline.setSingleShot(true);
    connect(&pickerDeadline, &QTimer::timeout, this, [&] {
      pickerTimedOut = true;
      auto *dialog = QApplication::activeModalWidget();
      auto *focus = QApplication::focusWidget();
      pickerState =
          QStringLiteral("modal=%1 active=%2 focus=%3")
              .arg(dialog ? dialog->metaObject()->className() : "none")
              .arg(dialog && dialog->isActiveWindow())
              .arg(focus ? focus->objectName() : QStringLiteral("none"));
      if (dialog) {
        auto *list =
            dialog->findChild<QListWidget *>(QStringLiteral("typePickerList"));
        auto *summary =
            dialog->findChild<QLabel *>(QStringLiteral("typePickerSummary"));
        auto *search =
            dialog->findChild<QLineEdit *>(QStringLiteral("typePickerSearch"));
        auto *name =
            dialog->findChild<QLineEdit *>(QStringLiteral("typePickerName"));
        auto *apply =
            dialog->findChild<QPushButton *>(QStringLiteral("typePickerApply"));
        pickerState +=
            QStringLiteral(" pickerRan=%1 rows=%2 row=%3 searchFocus=%4"
                           " name=%5 apply=%6 summary=%7")
                .arg(pickerRan)
                .arg(list ? list->count() : -1)
                .arg(list ? list->currentRow() : -1)
                .arg(search && search->hasFocus())
                .arg(name ? name->text() : QStringLiteral("missing"))
                .arg(apply && apply->isEnabled())
                .arg(summary ? summary->text() : QString());
        auto *field =
            dialog->findChild<QLineEdit *>(QStringLiteral("setTypeField"));
        auto *status =
            dialog->findChild<QLabel *>(QStringLiteral("setTypeStatus"));
        pickerState += QStringLiteral(" stage=%1 field=%2 status=%3")
                           .arg(stage)
                           .arg(field ? field->text() : QString())
                           .arg(status ? status->text() : QString());
        dialog->close();
      }
    });
    pickerDeadline.start(OpenTimeoutMs);
    bench.action(ActionId::EditConvertToStruct)->trigger();
    pickerDeadline.stop();
    QVERIFY2(!pickerTimedOut, qPrintable(pickerState));
    QTRY_COMPARE_WITH_TIMEOUT(
        listed,
        (QStringList{QStringLiteral("_DEVICE"), QStringLiteral("_LIST_ENTRY")}),
        OpenTimeoutMs);
    QCOMPARE(member, QStringLiteral("_DEVICE *Device"));
    QTRY_VERIFY_WITH_TIMEOUT(code->allText().contains("_DEVICE *Device") &&
                                 code->allText().contains("Device->Type"),
                             OpenTimeoutMs);

    // Y on arg1: its type, a name completed, one refused, one stated.
    QTRY_VERIFY(onParameter(QStringLiteral("arg1"), 1));
    QTRY_VERIFY(bench.action(ActionId::EditSetType)->isEnabled());
    QString subject, completed, refused;
    auto *typer = new QTimer(&bench.session);
    connect(typer, &QTimer::timeout, typer, [&, typer] {
      auto *dialog =
          qobject_cast<SetTypeDialog *>(QApplication::activeModalWidget());
      if (!dialog)
        return;
#ifdef Q_OS_MACOS
      // On macOS the completion list takes the activation while it shows.
      // The system hands it back to the dialog when the list closes; the
      // offscreen platform does not.
      if (stage == 2 && !dialog->isActiveWindow())
        dialog->activateWindow();
#endif
      // The completion list is a window of its own while it shows.
      if (stage != 1 && !dialog->isActiveWindow())
        return;
      auto *field =
          dialog->findChild<QLineEdit *>(QStringLiteral("setTypeField"));
      auto *status =
          dialog->findChild<QLabel *>(QStringLiteral("setTypeStatus"));
      auto *apply =
          dialog->findChild<QPushButton *>(QStringLiteral("setTypeApply"));
      if (!field || !status || !apply || !field->isEnabled())
        return;
      if (stage == 0) {
        if (field->text() != QLatin1String("int64_t") || !apply->isEnabled())
          return;
        subject = dialog->findChild<QLabel *>(QStringLiteral("setTypeSubject"))
                      ->text();
        field->selectAll();
        QTest::keyClicks(field, QStringLiteral("_LIS"));
        stage = 1;
      } else if (stage == 1) {
        auto *completer = dialog->findChild<QCompleter *>();
        auto *popup = completer ? completer->popup() : nullptr;
        if (!popup || !popup->isVisible() || !popup->model()->rowCount())
          return;
        completed = popup->model()->index(0, 0).data().toString();
        popup->setCurrentIndex(popup->model()->index(0, 0));
        QTest::keyClick(popup, Qt::Key_Return);
        stage = 2;
      } else if (stage == 2) {
        if (field->text() != QLatin1String("_LIST_ENTRY"))
          return;
        QTest::keyClicks(field, QStringLiteral(" *NOPE"));
        stage = 3;
      } else if (stage == 3) {
        if (!status->text().startsWith(QStringLiteral("✗")))
          return;
        refused = status->text();
        QVERIFY(!apply->isEnabled());
        field->selectAll();
        QTest::keyClicks(field, QStringLiteral("_LIST_ENTRY *"));
        stage = 4;
      } else if (stage == 4) {
        if (!apply->isEnabled() ||
            !status->text().contains(QLatin1String("_LIST_ENTRY *arg1")))
          return;
        typer->stop();
        typer->deleteLater();
        QTest::keyClick(field, Qt::Key_Return);
      }
    });
    typer->start(10);
    pickerDeadline.start(OpenTimeoutMs);
    bench.action(ActionId::EditSetType)->trigger();
    pickerDeadline.stop();
    QVERIFY2(!pickerTimedOut, qPrintable(pickerState));
    QTRY_VERIFY_WITH_TIMEOUT(code->allText().contains("_LIST_ENTRY *arg1"),
                             OpenTimeoutMs);
    QCOMPARE(subject, QStringLiteral(
                          "arg1, parameter 2 of function_7, arrives in rdx."));
    QCOMPARE(completed, QStringLiteral("_LIST_ENTRY"));
    QVERIFY2(refused.contains(QLatin1String("'NOPE' names no type")),
             qPrintable(refused));

    // Y on the function: the declaration the user gave it, which goes back
    // to analysis.
    code->setCursorLine(0);
    QVERIFY(code->findText(QStringLiteral("function_7"), true));
    code->setFocus();
    QString declaration;
    auto *resetter = new QTimer(&bench.session);
    connect(resetter, &QTimer::timeout, resetter, [&, resetter] {
      auto *dialog =
          qobject_cast<SetTypeDialog *>(QApplication::activeModalWidget());
      if (!dialog || !dialog->isActiveWindow())
        return;
      auto *field =
          dialog->findChild<QLineEdit *>(QStringLiteral("setTypeField"));
      auto *reset =
          dialog->findChild<QPushButton *>(QStringLiteral("setTypeReset"));
      if (!field || !reset || !field->isEnabled() || !reset->isVisible())
        return;
      resetter->stop();
      resetter->deleteLater();
      declaration = field->text();
      reset->click();
    });
    resetter->start(10);
    pickerDeadline.start(OpenTimeoutMs);
    bench.action(ActionId::EditSetType)->trigger();
    pickerDeadline.stop();
    QVERIFY2(!pickerTimedOut, qPrintable(pickerState));
    QTRY_VERIFY_WITH_TIMEOUT(code->allText().contains("void* arg0"),
                             OpenTimeoutMs);
    QCOMPARE(declaration, QStringLiteral("int64_t function_7(_DEVICE *Device, "
                                         "_LIST_ENTRY *arg1)"));
    // One undo states the declaration again.
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(code->allText().contains("_LIST_ENTRY *arg1"),
                             OpenTimeoutMs);
    QVERIFY2(bench.session.lastError().isEmpty(),
             qPrintable(bench.session.lastError()));
  }

  void localTypesAreDeclaredEditedAndDeletedByIdasKeys() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-types.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.session.declaresLocalTypes());
    bench.action(ActionId::ViewLocalTypes)->trigger();
    auto *types = bench.window->findChild<LocalTypesView *>();
    QVERIFY(types);
    auto *list =
        types->findChild<QTreeView *>(QStringLiteral("localTypesList"));
    QVERIFY(list);
    QTRY_COMPARE(list->model()->rowCount(), 3);
    types->showType(QStringLiteral("_DEVICE"));
    QTRY_COMPARE(types->currentType(), QStringLiteral("_DEVICE"));

    // Ins opens the editor over the list, the main window's keys notwith-
    // standing; a problem stays in it at its place, and a declaration that
    // reads adds its type and shows it.
    QStringList titles, languages;
    QString chosen;
    bool problemShown = false, problemAtItsPlace = false;
    int answers = 0;
    QTimer editor;
    connect(&editor, &QTimer::timeout, this, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      auto *text = dialog ? dialog->findChild<QPlainTextEdit *>(
                                QStringLiteral("declareEditor"))
                          : nullptr;
      if (auto *language = dialog ? dialog->findChild<QComboBox *>(
                                        QStringLiteral("declareLanguage"))
                                  : nullptr;
          language && languages.isEmpty()) {
        chosen = language->currentData().toString();
        for (int index = 0; index < language->count(); ++index)
          languages.append(language->itemText(index) + QLatin1Char('=') +
                           language->itemData(index).toString());
      }
      auto *declare =
          dialog ? dialog->findChild<QPushButton *>(QStringLiteral("declare"))
                 : nullptr;
      auto *problems = dialog ? dialog->findChild<QListWidget *>(
                                    QStringLiteral("declareProblems"))
                              : nullptr;
      if (!text || !declare || !problems || !declare->isEnabled())
        return;
      if (titles.size() == answers)
        titles.append(dialog->windowTitle());
      if (answers == 0 && !problems->isVisible()) {
        text->setPlainText(QStringLiteral("struct BAD {\n  NOPE x;\n};"));
        QTest::mouseClick(declare, Qt::LeftButton);
        return;
      }
      if (answers == 0) {
        problemShown = problems->count() == 1;
        problemAtItsPlace = text->textCursor().blockNumber() == 1 &&
                            text->textCursor().positionInBlock() == 2;
        text->setPlainText(QStringLiteral("struct OWNER { void *Next; };"));
        // Ctrl+Return declares from the editor.
        ++answers;
        QTest::keyClick(text, Qt::Key_Return, Qt::ControlModifier);
        return;
      }
      if (answers == 1 &&
          text->toPlainText().contains(QStringLiteral("struct OWNER"))) {
        // Edit: the type shown, declared in the language shown.
        ++answers;
        QTest::mouseClick(declare, Qt::LeftButton);
      }
    });
    editor.start(10);
    QTimer::singleShot(OpenTimeoutMs, &bench.session, [] {
      if (auto *dialog =
              qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        dialog->reject();
    });
    // From a tab that renders only, as Python's, the editor declares C.
    types->setPage(4);
    list->setFocus();
    QTest::keyClick(list, Qt::Key_Insert);
    QVERIFY(problemShown);
    QVERIFY(problemAtItsPlace);
    QCOMPARE(chosen, QStringLiteral("c"));
    // C/C++ declares C, the language the window shows it in.
    QCOMPARE(languages, (QStringList{QStringLiteral("C/C++=c"),
                                     QStringLiteral("Rust=rust"),
                                     QStringLiteral("Go=go")}));
    QTRY_COMPARE(list->model()->rowCount(), 4);
    QTRY_COMPARE(types->currentType(), QStringLiteral("OWNER"));

    // The list shows one origin when asked.
    auto *origin =
        types->findChild<QComboBox *>(QStringLiteral("localTypesOrigin"));
    QVERIFY(origin);
    origin->setCurrentIndex(origin->findData(QStringLiteral("user")));
    QTRY_COMPARE(list->model()->rowCount(), 1);
    origin->setCurrentIndex(origin->findData(QStringLiteral("pdb")));
    QTRY_COMPARE(list->model()->rowCount(), 3);
    // A type of another origin opens all of them again.
    types->showType(QStringLiteral("OWNER"));
    QTRY_COMPARE(types->currentType(), QStringLiteral("OWNER"));
    QCOMPARE(origin->currentIndex(), 0);
    QTRY_COMPARE(list->model()->rowCount(), 4);

    // Ctrl+E edits it, though the main window jumps to an entry point by
    // the same key.
    list->setFocus();
    QTest::keyClick(list, Qt::Key_E, Qt::ControlModifier);
    QTRY_COMPARE(answers, 2);
    editor.stop();
    QCOMPARE(titles.value(0), QStringLiteral("Add types"));
    QCOMPARE(titles.value(1), QStringLiteral("Edit OWNER"));
    QTRY_COMPARE(types->currentType(), QStringLiteral("OWNER"));

    // Del deletes the selected type once asked.
    QTimer confirm;
    connect(&confirm, &QTimer::timeout, this, [&] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        confirm.stop();
        QTest::mouseClick(box->button(QMessageBox::Yes), Qt::LeftButton);
      }
    });
    confirm.start(10);
    list->setFocus();
    QTest::keyClick(list, Qt::Key_Delete);
    QTRY_COMPARE(list->model()->rowCount(), 3);
    auto *summary =
        types->findChild<QLabel *>(QStringLiteral("localTypesSummary"));
    QVERIFY(summary);
    QTRY_VERIFY(summary->text().contains(QStringLiteral("Deleted OWNER")));

    // Undo takes the deletion back, and redo makes it again.
    QTRY_VERIFY(bench.action(ActionId::EditUndo)->isEnabled());
    bench.action(ActionId::EditUndo)->trigger();
    QTRY_COMPARE(list->model()->rowCount(), 4);
    QTRY_VERIFY(bench.action(ActionId::EditRedo)->isEnabled());
    bench.action(ActionId::EditRedo)->trigger();
    QTRY_COMPARE(list->model()->rowCount(), 3);
  }

  void headerFilesDeclareTheirTypesByCtrlF9() {
    // Ctrl+F9, as IDA parses a C header, opens the editor with a header
    // file's declarations, which declare its types; undo takes them away.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-types.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.action(ActionId::ViewLocalTypes)->trigger();
    auto *types = bench.window->findChild<LocalTypesView *>();
    QVERIFY(types);
    auto *list =
        types->findChild<QTreeView *>(QStringLiteral("localTypesList"));
    QVERIFY(list);
    QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 3, OpenTimeoutMs);
    const QString header = directory.filePath(QStringLiteral("owner.h"));
    {
      QFile file(header);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("#pragma once\nstruct FROM_HEADER { int x; };\n");
    }
    QCOMPARE(bench.action(ActionId::FileLoadHeader)->shortcut(),
             QKeySequence(QStringLiteral("Ctrl+F9")));
    QString parsed;
    QStringList messages;
    QTimer files;
    connect(&files, &QTimer::timeout, this, [&] {
      QWidget *modal = QApplication::activeModalWidget();
      if (!modal || !modal->isVisible())
        return;
      // A message, as the file dialog's when it finds no such file, is
      // reported rather than waited on.
      if (auto *box = qobject_cast<QMessageBox *>(modal)) {
        messages.append(box->text());
        box->reject();
        return;
      }
      if (auto *dialog = qobject_cast<QFileDialog *>(modal)) {
        // The name typed, as a user types it: selectFile leaves the name
        // alone while its edit has the focus.
        if (auto *name =
                dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
          name->setText(header);
        else
          dialog->selectFile(header);
        static_cast<QDialog *>(dialog)->accept();
        return;
      }
      auto *dialog = qobject_cast<QDialog *>(modal);
      auto *text = dialog ? dialog->findChild<QPlainTextEdit *>(
                                QStringLiteral("declareEditor"))
                          : nullptr;
      auto *declare =
          dialog ? dialog->findChild<QPushButton *>(QStringLiteral("declare"))
                 : nullptr;
      if (!text || !declare || !declare->isEnabled() || !parsed.isEmpty())
        return;
      parsed = text->toPlainText();
      // As pressing Declare does, whether or not the window is shown yet.
      declare->click();
    });
    files.start(10);
    QTRY_VERIFY_WITH_TIMEOUT(
        bench.action(ActionId::FileLoadHeader)->isEnabled(), OpenTimeoutMs);
    bench.action(ActionId::FileLoadHeader)->trigger();
    files.stop();
    QVERIFY2(parsed.contains(QStringLiteral("struct FROM_HEADER")),
             qPrintable(parsed + messages.join(QLatin1Char('\n'))));
    QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 4, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.action(ActionId::EditUndo)->isEnabled(),
                             OpenTimeoutMs);
    bench.action(ActionId::EditUndo)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 3, OpenTimeoutMs);
  }

  void typeLibrariesWaitForTheirCatalogueBeforeOfferingChoices() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-types.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(
        bench.session.indexReady() && !bench.session.busy(), OpenTimeoutMs);
    bench.action(ActionId::ViewTypeLibraries)->trigger();
    auto *view = bench.window->findChild<TypeLibrariesView *>();
    QVERIFY(view);
    auto *add =
        view->findChild<QToolButton *>(QStringLiteral("typeLibrariesAdd"));
    QVERIFY(add);

    bool premature = false;
    QStringList offered;
    QTimer closer;
    closer.setSingleShot(true);
    connect(&closer, &QTimer::timeout, this, [&] {
      if (auto *dialog = visibleDialog(QStringLiteral("addTypeLibraryDialog"));
          dialog &&
          dialog->objectName() == QLatin1String("addTypeLibraryDialog")) {
        auto *choices = dialog->findChild<QTreeWidget *>(
            QStringLiteral("typeLibraryChoices"));
        premature = choices && choices->topLevelItemCount() == 0;
        if (choices)
          for (int row = 0; row < choices->topLevelItemCount(); ++row)
            offered.push_back(choices->topLevelItem(row)->text(0));
        dialog->reject();
      }
    });
    closer.start(0);
    // The catalogue reply is deferred. An immediate click must not open an
    // empty snapshot that stays empty even after that reply arrives.
    QTest::mouseClick(add, Qt::LeftButton);
    closer.stop();
    QVERIFY2(!premature,
             "Add opened before the type-library catalogue arrived.");
    QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), OpenTimeoutMs);

    closer.start(0);
    QTest::mouseClick(add, Qt::LeftButton);
    QTRY_COMPARE(offered, QStringList{QStringLiteral("uefi-x64")});
  }

  void typeLibrariesAreAddedAndLeftOutByShiftF11() {
    // Shift+F11, as IDA lists its type libraries: the program's two join
    // its types; Del leaves the kernel's out, Ins adds UEFI's, and the
    // project keeps the choice.
    QTemporaryDir directory;
    Workbench bench;
    const QString input =
        writeFixture(directory, QStringLiteral("local-types.bin"));
    bench.window->openFile(input);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(bench.action(ActionId::ViewTypeLibraries)->shortcut(),
             QKeySequence(QStringLiteral("Shift+F11")));
    bench.action(ActionId::ViewTypeLibraries)->trigger();
    auto *view = bench.window->findChild<TypeLibrariesView *>();
    QVERIFY(view);
    auto *table = view->table();
    const auto names = [table] {
      QStringList listed;
      for (int row = 0; row < table->topLevelItemCount(); ++row)
        listed << table->topLevelItem(row)->text(0);
      return listed;
    };
    QTRY_COMPARE_WITH_TIMEOUT(
        names(),
        (QStringList{QStringLiteral("windows-user-x64"),
                     QStringLiteral("windows-kernel-x64")}),
        OpenTimeoutMs);
    QCOMPARE(table->topLevelItem(0)->text(1), QStringLiteral("The program"));
    QCOMPARE(table->topLevelItem(0)->text(4), QStringLiteral("2"));
    auto *status =
        view->findChild<QLabel *>(QStringLiteral("typeLibrariesStatus"));
    QVERIFY(status);
    QCOMPARE(status->text(),
             QStringLiteral("2 type libraries join the program's types."));

    // Del leaves the selected library out.
    table->setCurrentItem(table->topLevelItem(1));
    view->removeLibrary();
    QTRY_COMPARE_WITH_TIMEOUT(names(),
                              QStringList{QStringLiteral("windows-user-x64")},
                              OpenTimeoutMs);
    QCOMPARE(status->text(),
             QStringLiteral("1 type library joins the program's types. Left "
                            "out: windows-kernel-x64."));

    // Ins offers the libraries that lay out as the program's and join none
    // of its types now, the kernel's left out among them.
    QStringList offered;
    QTimer chooser;
    connect(&chooser, &QTimer::timeout, this, [&] {
      auto *dialog = visibleDialog(QStringLiteral("addTypeLibraryDialog"));
      if (!dialog || !dialog->isVisible() || !offered.isEmpty() ||
          dialog->objectName() != QLatin1String("addTypeLibraryDialog"))
        return;
      auto *choices = dialog->findChild<QTreeWidget *>(
          QStringLiteral("typeLibraryChoices"));
      if (!choices)
        return;
      for (int row = 0; row < choices->topLevelItemCount(); ++row) {
        offered << choices->topLevelItem(row)->text(0);
        if (offered.back() == QLatin1String("uefi-x64"))
          choices->setCurrentItem(choices->topLevelItem(row));
      }
      dialog->accept();
    });
    chooser.start(10);
    QTRY_VERIFY_WITH_TIMEOUT(
        view->findChild<QToolButton *>(QStringLiteral("typeLibrariesAdd"))
            ->isEnabled(),
        OpenTimeoutMs);
    view->addLibrary();
    QTRY_VERIFY(!offered.isEmpty());
    chooser.stop();
    QTRY_COMPARE(offered, (QStringList{QStringLiteral("windows-kernel-x64"),
                                       QStringLiteral("uefi-x64")}));
    QTRY_COMPARE_WITH_TIMEOUT(names(),
                              (QStringList{QStringLiteral("windows-user-x64"),
                                           QStringLiteral("uefi-x64")}),
                              OpenTimeoutMs);
    QCOMPARE(table->topLevelItem(1)->text(1), QStringLiteral("Added"));

    // The project keeps the choice, which the next session joins.
    QFile kept(input + QStringLiteral(".neverd/libraries.json"));
    QVERIFY(kept.open(QIODevice::ReadOnly));
    const QJsonObject choice = QJsonDocument::fromJson(kept.readAll()).object();
    QCOMPARE(choice.value(QStringLiteral("added")).toArray(),
             QJsonArray{QStringLiteral("uefi-x64")});
    QCOMPARE(choice.value(QStringLiteral("removed")).toArray(),
             QJsonArray{QStringLiteral("windows-kernel-x64")});
  }

  void linksCopyAndOpenTheirPlaces() {
    // Copy link names the place the disassembly shows, and a link opens its
    // place in the view it asks for, as IDA's open_ida_link does; another
    // build, an address the program lacks or a bookmark it lacks is refused.
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("fixture.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const QString hash =
        bench.session.metadata().value(QStringLiteral("sha256")).toString();
    QCOMPARE(hash.size(), 64);
    auto *disassembly = bench.window->disassembly();
    disassembly->navigate(Base + 0x44);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x44));
    disassembly->focusContent();
    clearClipboard();
    QTRY_VERIFY(bench.action(ActionId::EditCopyLink)->isEnabled());
    bench.action(ActionId::EditCopyLink)->trigger();
    QCOMPARE(QApplication::clipboard()->text(),
             QStringLiteral("neverd:///fixture.bin/addresses?rva=0x44"
                            "&sha256=%1")
                 .arg(hash.left(16)));

    QString error;
    const auto open = [&](const QString &text) {
      error = QStringLiteral("unanswered");
      const ParsedLink parsed = parseDeepLink(text);
      if (!parsed.link) {
        error = parsed.error;
        return;
      }
      bench.window->openLink(*parsed.link,
                             [&](const QString &result) { error = result; });
    };
    // A function by an address inside it, shown from its entry.
    open(QStringLiteral(
        "neverd:///fixture.bin/functions?rva=0x48&view=pseudocode"));
    QTRY_COMPARE_WITH_TIMEOUT(error, QString(), OpenTimeoutMs);
    QTRY_VERIFY(bench.codeView(QStringLiteral("source")));
    QTRY_COMPARE_WITH_TIMEOUT(
        bench.codeView(QStringLiteral("source"))->text()->function(),
        std::optional<Address>(Base + 0x40), OpenTimeoutMs);
    open(QStringLiteral("neverd:///addresses?ea=%1&view=graph")
             .arg(hexAddress(Base + 0x52)));
    QTRY_COMPARE_WITH_TIMEOUT(error, QString(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(disassembly->graphMode() &&
                                 disassembly->graph()->loaded(),
                             OpenTimeoutMs);
    QTRY_COMPARE(disassembly->currentFunction(),
                 std::optional<Address>(Base + 0x50));

    open(QStringLiteral("neverd:///addresses?rva=0x52&view=disasm"));
    QTRY_COMPARE_WITH_TIMEOUT(error, QString(), OpenTimeoutMs);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x52));
    open(QStringLiteral("neverd:///fixture.bin/functions?rva=0x40&sha256=%1")
             .arg(hash.startsWith(QLatin1String("00000000"))
                      ? QStringLiteral("ffffffff")
                      : QStringLiteral("00000000")));
    QTRY_VERIFY(error.startsWith(
        QStringLiteral("The link names another build of the input")));
    QCOMPARE(disassembly->currentItem(), std::optional<Address>(Base + 0x52));
    open(QStringLiteral("neverd:///addresses?rva=0x7fffffff"));
    QTRY_VERIFY(error.startsWith(QStringLiteral("The link's address")));
    QCOMPARE(disassembly->currentItem(), std::optional<Address>(Base + 0x52));
    open(QStringLiteral("neverd:///bookmarks?rva=0x40"));
    QTRY_VERIFY(error.startsWith(QStringLiteral("No bookmark is at")));
    QCOMPARE(disassembly->currentItem(), std::optional<Address>(Base + 0x52));
  }

  void externalLinksPreserveWindowState_data() {
    QTest::addColumn<bool>("maximized");
    QTest::addColumn<bool>("minimized");
    QTest::newRow("normal") << false << false;
    QTest::newRow("maximized") << true << false;
    QTest::newRow("normal-minimized") << false << true;
    QTest::newRow("maximized-minimized") << true << true;
  }

  void externalLinksPreserveWindowState() {
    QFETCH(bool, maximized);
    QFETCH(bool, minimized);
    Theme::instance().apply();
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    if (maximized)
      bench.window->showMaximized();
    bench.window->openFile(writeFixture(directory, "window-link.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(QTest::qWaitForWindowExposed(bench.window.get()));
    const auto state = maximized ? Qt::WindowMaximized : Qt::WindowNoState;
    QTRY_COMPARE(bench.window->windowHandle()->windowState(), state);
    const auto geometry = bench.window->geometry();
    if (minimized) {
      bench.window->showMinimized();
      QTRY_VERIFY(bench.window->isMinimized());
    }

    QLocalSocket socket;
    socket.connectToServer(bench.window->instanceServer().serverName());
    QVERIFY(socket.waitForConnected(1000));
    socket.write("{\"cmd\":\"open_link\",\"uri\":\"neverd:///"
                 "window-link.bin/addresses?rva=0x52\"}\n");
    QByteArray reply;
    QTRY_VERIFY_WITH_TIMEOUT((reply += socket.readAll()).contains('\n'),
                             OpenTimeoutMs);
    const auto result =
        QJsonDocument::fromJson(reply.left(reply.indexOf('\n'))).object();
    QCOMPARE(result.value("status").toString(), QStringLiteral("ok"));
    QTRY_VERIFY(!bench.window->isMinimized());
    QVERIFY(QTest::qWaitForWindowExposed(bench.window.get()));
    QCOMPARE(bench.window->isMaximized(), maximized);
    QTRY_COMPARE(bench.window->windowHandle()->windowState(), state);
    QCOMPARE(bench.window->geometry(), geometry);
    QTRY_COMPARE(bench.window->disassembly()->currentItem(),
                 std::optional<Address>(Base + 0x52));
  }

  void otherProgramsFindTheWorkbenchAndOpenLinksInIt() {
    // As IDA 9.4's IPC server: get_info names the program, and open_link
    // opens a link in this window; hcli's requests, without a newline, are
    // answered too.  The server lives where only this user can reach it.
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    auto &server = bench.window->instanceServer();
    QVERIFY2(server.listening(), qPrintable(server.errorString()));
#ifndef Q_OS_WIN
    QCOMPARE(QFileInfo(server.serverName()).absolutePath(),
             InstanceServer::directory());
    QCOMPARE(QFileInfo(InstanceServer::directory()).permissions() &
                 (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                  QFile::ReadOther | QFile::WriteOther | QFile::ExeOther),
             QFileDevice::Permissions());
#endif
    const auto instances = InstanceServer::discover();
    const auto instance = std::find_if(
        instances.begin(), instances.end(),
        [&](const InstanceServer::Instance &instance) {
          return instance.server == server.serverName() &&
                 instance.pid == QCoreApplication::applicationPid();
        });
    QVERIFY(instance != instances.end());
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("fixture.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);

    QLocalSocket socket;
    socket.connectToServer(instance->server);
    QVERIFY(socket.waitForConnected(1000));
    const auto ask = [&](const QByteArray &request) {
      socket.write(request);
      QByteArray reply;
      QTest::qWaitFor(
          [&] {
            reply += socket.readAll();
            return reply.contains('\n');
          },
          OpenTimeoutMs);
      return QJsonDocument::fromJson(reply.left(reply.indexOf('\n'))).object();
    };
    const QJsonObject info = ask("{\"cmd\":\"get_info\"}\n");
    QCOMPARE(info.value(QStringLiteral("status")).toString(),
             QStringLiteral("ok"));
    QVERIFY(info.value(QStringLiteral("loaded")).toBool());
    QCOMPARE(info.value(QStringLiteral("input_name")).toString(),
             QStringLiteral("fixture.bin"));
    QCOMPARE(info.value(QStringLiteral("input")).toString(),
             bench.session.filePath());
    QVERIFY(info.value(QStringLiteral("analysis_complete")).isBool());
    QCOMPARE(
        info.value(QStringLiteral("sha256")).toString(),
        bench.session.metadata().value(QStringLiteral("sha256")).toString());
    QCOMPARE(info.value(QStringLiteral("pid")).toInteger(),
             QCoreApplication::applicationPid());
    QCOMPARE(ask("{\"cmd\":\"ping\"}").value(QStringLiteral("status")),
             QJsonValue(QStringLiteral("ok")));
    QCOMPARE(ask("{\"cmd\":\"is_analysis_complete\"}\n")
                 .value(QStringLiteral("status")),
             QJsonValue(QStringLiteral("ok")));
    const QJsonObject opened =
        ask("{\"cmd\":\"open_link\",\"uri\":\"neverd:///fixture.bin/"
            "addresses?rva=0x52\"}\n");
    QVERIFY2(opened.value(QStringLiteral("status")).toString() ==
                 QLatin1String("ok"),
             qPrintable(QJsonDocument(opened).toJson()));
    QTRY_COMPARE(bench.window->disassembly()->currentItem(),
                 std::optional<Address>(Base + 0x52));
    const QJsonObject location = ask("{\"cmd\":\"get_info\"}\n")
                                     .value(QStringLiteral("location"))
                                     .toObject();
    QCOMPARE(addressValue(location.value(QStringLiteral("address"))),
             std::optional<Address>(Base + 0x52));
    QCOMPARE(addressValue(location.value(QStringLiteral("rva"))),
             std::optional<Address>(0x52));
    QCOMPARE(ask("{\"cmd\":\"open_link\",\"uri\":\"neverd:///types\"}\n")
                 .value(QStringLiteral("message"))
                 .toString(),
             QStringLiteral("A types link names its type with name=."));
    QCOMPARE(ask("{\"cmd\":\"rename\"}\n")
                 .value(QStringLiteral("message"))
                 .toString(),
             QStringLiteral("Unknown command: rename"));
    QCOMPARE(ask("[1]\n").value(QStringLiteral("message")).toString(),
             QStringLiteral("Expected a JSON object"));
  }

  void instanceRequestsRespectTheirByteLimit_data() {
    QTest::addColumn<int>("extraBytes");
    QTest::addColumn<bool>("newline");
    QTest::addColumn<bool>("complete");
    QTest::addColumn<int>("requests");
    QTest::newRow("limit-newline") << 0 << true << true << 1;
    QTest::newRow("limit-no-newline") << 0 << false << true << 1;
    QTest::newRow("oversized-newline") << 1 << true << true << 1;
    QTest::newRow("oversized-no-newline") << 1 << false << true << 1;
    QTest::newRow("oversized-incomplete") << 1 << false << false << 1;
    QTest::newRow("limit-per-message") << 0 << true << true << 2;
  }

  void instanceRequestsRespectTheirByteLimit() {
    QFETCH(int, extraBytes);
    QFETCH(bool, newline);
    QFETCH(bool, complete);
    QFETCH(int, requests);
    InstanceServer server;
    int opened = 0;
    server.setLinkHandler([&](const DeepLink &, auto done) {
      ++opened;
      done({});
    });
    QVERIFY2(server.start(), qPrintable(server.errorString()));
    QLocalSocket socket;
    socket.connectToServer(server.serverName());
    QVERIFY(socket.waitForConnected(
        ::neverd::limits::gui::kInstanceServerRequestTimeoutMs));
    // The same valid link at the boundary must open, while a larger JSON
    // object must be refused before the handler sees it, with or without
    // a newline. Multiple bounded requests may share a connection.
    QByteArray request = "{\"cmd\":\"open_link\",\"uri\":\"neverd:///"
                         "addresses?ea=1\",\"padding\":\"";
    const QByteArray suffix = complete ? "\"}" : "\" ";
    const int limit = ::neverd::limits::gui::kInstanceServerMaxRequestBytes;
    request +=
        QByteArray(limit + extraBytes - request.size() - suffix.size(), 'x');
    request += suffix;
    if (newline)
      request += '\n';
    const QByteArray wire = request.repeated(requests);
    QCOMPARE(socket.write(wire), qint64(wire.size()));
    QByteArray replies;
    QVERIFY(QTest::qWaitFor(
        [&] {
          replies += socket.readAll();
          return replies.count('\n') == requests;
        },
        OpenTimeoutMs));
    const bool accepted = extraBytes == 0;
    QCOMPARE(opened, accepted ? requests : 0);
    for (const QByteArray &line : replies.trimmed().split('\n')) {
      const QJsonObject reply = QJsonDocument::fromJson(line).object();
      QCOMPARE(reply.value(QStringLiteral("status")).toString(),
               accepted ? QStringLiteral("ok") : QStringLiteral("error"));
      if (!accepted)
        QCOMPARE(reply.value(QStringLiteral("message")).toString(),
                 QStringLiteral("Request exceeds %1 bytes").arg(limit));
    }
    if (!accepted)
      QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
  }

  void instanceServingCanBeDisabled() {
    const auto restore =
        qScopeGuard([] { QSettings().remove(settings::InstancesDisabled); });
    QSettings().setValue(settings::InstancesDisabled, true);
    InstanceServer server;
    QVERIFY(!server.start());
    QVERIFY(!server.listening());
    QVERIFY(!server.errorString().isEmpty());
    QSettings().setValue(settings::InstancesDisabled, false);
    QVERIFY2(server.start(), qPrintable(server.errorString()));
    QVERIFY(server.listening());
  }

  void interactionSamplingWaitsForTheCompletedFunctionCatalog() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("chooser-refresh.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.analysisComplete(), OpenTimeoutMs);
    const auto background = bench.session.background();
    const auto publish = [&](QString state, int count) {
      auto next = background;
      next["state"] = state;
      next["functions"] = count;
      SessionTestAccess::receive(
          bench.session,
          {{"type", "heartbeat"},
           {"project_id", bench.session.queries().projectId()},
           {"background_sequence",
            SessionTestAccess::nextBackgroundSequence(bench.session)},
           {"background", next}});
    };
    publish("building", 600);
    QVERIFY(!bench.session.analysisComplete());
    InteractionMetrics metrics(*bench.window, bench.session,
                               directory.filePath("interactions.json"),
                               NativeOpenTimeoutMs, nullptr);
    emit bench.window->firstContentPainted();
    bool delivered = false;
    QTimer::singleShot(0, &metrics, [&] { delivered = true; });
    QTRY_VERIFY(delivered);
    QVERIFY(!InteractionMetricsTestAccess::started(metrics));
    QCOMPARE(InteractionMetricsTestAccess::samples(metrics), 0);

    // The completed catalog differs from open-time metadata. The worker's
    // existing discovery fixture adds one identity when this marker appears.
    QVERIFY(!writeFixture(directory, "chooser-refresh.bin.discover").isEmpty());
    publish("ready", 601);
    QTRY_VERIFY_WITH_TIMEOUT(InteractionMetricsTestAccess::samples(metrics) > 0,
                             OpenTimeoutMs);
    QCOMPARE(bench.session.metadata().value("function_count").toInt(), 600);
    QCOMPARE(InteractionMetricsTestAccess::functionCount(metrics), 601);
  }

  void pseudocodeMeasurementWaitsForThePendingListingJump() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("source-cache.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.analysisComplete(), OpenTimeoutMs);
    QVERIFY(!bench.codeView("source"));
    auto *listing = bench.window->disassembly()->listing();
    listing->jumpTo(Base + 16);
    QVERIFY(listing->jumpPending());
    InteractionMetrics metrics(*bench.window, bench.session,
                               directory.filePath("interactions.json"),
                               NativeOpenTimeoutMs, nullptr);
    bool done = false;
    InteractionMetricsTestAccess::openPseudocode(metrics, Base + 16,
                                                 [&] { done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, OpenTimeoutMs);
    const auto result = InteractionMetricsTestAccess::pseudocodeResult(metrics);
    QCOMPARE(result.value("samples").toInt(), 1);
    QCOMPARE(result.value("missed").toInt(), 0);
    auto *view = bench.codeView("source");
    QVERIFY(view);
    QVERIFY(view->text()->complete());
    QCOMPARE(view->text()->function(), std::optional<Address>(Base + 16));
  }

  void pseudocodeMeasurementRequiresTheRequestedCompleteDocument_data() {
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<bool>("success");
    QTest::newRow("complete-after-cancellation")
        << QStringLiteral("source-export.bin") << true;
    QTest::newRow("error-page-is-not-source")
        << QStringLiteral("source-export-fail.bin") << false;
  }

  void pseudocodeMeasurementRequiresTheRequestedCompleteDocument() {
    QFETCH(QString, fixture);
    QFETCH(bool, success);
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(writeFixture(directory, fixture));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.analysisComplete(), OpenTimeoutMs);
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView("source");
    QVERIFY(view);
    view->showFunction(Base + 16);
    QVERIFY(view->text()->loading());
    InteractionMetrics metrics(*bench.window, bench.session,
                               directory.filePath("interactions.json"),
                               NativeOpenTimeoutMs, nullptr);
    bool done = false;
    InteractionMetricsTestAccess::pseudocode(metrics, Base + 16,
                                             [&] { done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, OpenTimeoutMs);
    const auto result = InteractionMetricsTestAccess::pseudocodeResult(metrics);
    QCOMPARE(result.value("samples").toInt(), success ? 1 : 0);
    QCOMPARE(result.value("missed").toInt(), success ? 0 : 1);
    QCOMPARE(view->text()->complete(), success);
    QCOMPARE(view->text()->function(), std::optional<Address>(Base + 16));
    const auto detail = result.value("detail").toArray();
    QCOMPARE(detail.size(), 1);
    QCOMPARE(detail[0].toObject().contains("ms"), success);
    if (!success)
      QVERIFY(detail[0].toObject().value("missed").toString().contains(
          "fixture export preparation failed"));
  }

  void analysisCompletionDoesNotRequireAnUnsupportedReferenceIndex() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("source-cache-evm.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(
        bench.session.background().value("state").toString(),
        QStringLiteral("unavailable"), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.queries().analysisComplete(),
                             OpenTimeoutMs);
    QVERIFY(!bench.session.indexReady());
    QVERIFY(bench.session.analysisComplete());
    const auto replies = InstanceServer::requestMany(
        {bench.window->instanceServer().serverName()},
        {{"cmd", "is_analysis_complete"}});
    QVERIFY(replies[0]);
    QVERIFY(replies[0]->value("analysis_complete").toBool());
  }

  void recentLinksFindRenamedInputsByHash() {
    QTemporaryDir directory;
    const QString renamed = writeFixture(directory, QStringLiteral("new.bin"));
    const auto original = QSettings().value(settings::RecentFiles);
    const auto restore = qScopeGuard([&] {
      if (original.isValid())
        QSettings().setValue(settings::RecentFiles, original);
      else
        QSettings().remove(settings::RecentFiles);
    });
    QSettings().setValue(settings::RecentFiles, QStringList{renamed});
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(QByteArray("fixture"),
                                                     QCryptographicHash::Sha256)
                                .toHex());
    const auto parsed = parseDeepLink(
        QStringLiteral("neverd:///old.bin/addresses?rva=0&sha256=") +
        hash.left(16));
    QVERIFY(parsed.link);
    QCOMPARE(recentFileFor(*parsed.link), std::optional<QString>(renamed));
    DeepLink wrong = *parsed.link;
    wrong.input = QStringLiteral("new.bin");
    wrong.sha256 = QString(16, QLatin1Char('0'));
    QVERIFY(!recentFileFor(wrong));
    wrong.sha256.clear();
    QCOMPARE(recentFileFor(wrong), std::optional<QString>(renamed));
    wrong.input = QStringLiteral("old.bin");
    QVERIFY(!recentFileFor(wrong));
  }

  void recentLinksFindTheProgramStoredInADatabase() {
    QTemporaryDir directory;
    const QString binary =
        writeFixture(directory, QStringLiteral("sample.exe"));
    const QString database = directory.filePath(QStringLiteral("renamed.nddb"));
    QCOMPARE(
        ProjectDatabase::save(database, binary,
                              {{QStringLiteral("marker"), QByteArray("kept")}}),
        QString());
    const auto identity = ProjectDatabase::read(
        database, nullptr, ProjectDatabase::ReadScope::InputIdentity);
    QVERIFY(identity);
    QCOMPARE(identity->inputName, QStringLiteral("sample.exe"));
    QVERIFY(identity->state.isEmpty());
    const auto full = ProjectDatabase::read(database);
    QVERIFY(full);
    QCOMPARE(full->state.value(QStringLiteral("marker")), QByteArray("kept"));
    const auto original = QSettings().value(settings::RecentFiles);
    const auto restore = qScopeGuard([&] {
      if (original.isValid())
        QSettings().setValue(settings::RecentFiles, original);
      else
        QSettings().remove(settings::RecentFiles);
    });
    QSettings().setValue(settings::RecentFiles, QStringList{database});
    DeepLink link;
    link.input = QStringLiteral("sample.exe");
    link.rva = 0;
    link.sha256 = identity->inputSha256.left(16);
    QCOMPARE(recentFileFor(link), std::optional<QString>(database));
    link.input = QStringLiteral("older-name.exe");
    QCOMPARE(recentFileFor(link), std::optional<QString>(database));
    link.sha256.clear();
    link.input = QStringLiteral("sample.exe.i64");
    QCOMPARE(recentFileFor(link), std::optional<QString>(database));
    link.sha256 = QString(16, QLatin1Char('0'));
    QVERIFY(!recentFileFor(link));
  }

  void resourceLinksRequireTheNamedItemAndPreserveTheCursor() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-import.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    disassembly->navigate(Base + 0x44);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x44));
    QString result;
    const auto open = [&](const QString &resource, Address address) {
      result = QStringLiteral("pending");
      const auto parsed =
          parseDeepLink(QStringLiteral("ida:///%1?ea=%2&view=disasm")
                            .arg(resource, hexAddress(address)));
      QVERIFY(parsed.link);
      bench.window->openLink(*parsed.link,
                             [&](const QString &error) { result = error; });
    };
    for (const QString &resource :
         {QStringLiteral("strings"), QStringLiteral("imports"),
          QStringLiteral("exports")}) {
      open(resource, Base + 0x44);
      QTRY_VERIFY_WITH_TIMEOUT(result != QLatin1String("pending"),
                               OpenTimeoutMs);
      QVERIFY2(!result.isEmpty(), qPrintable(resource));
      QCOMPARE(disassembly->currentItem(), std::optional<Address>(Base + 0x44));
    }
    open(QStringLiteral("strings"), Base + 0x3100);
    QTRY_COMPARE_WITH_TIMEOUT(result, QString(), OpenTimeoutMs);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x3100));
    open(QStringLiteral("imports"), Base + 0x160);
    QTRY_COMPARE_WITH_TIMEOUT(result, QString(), OpenTimeoutMs);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x160));
    open(QStringLiteral("exports"), Base);
    QTRY_COMPARE_WITH_TIMEOUT(result, QString(), OpenTimeoutMs);
    QTRY_COMPARE(disassembly->currentItem(), std::optional<Address>(Base));
    open(QStringLiteral("segments"), Base + 0x44);
    QTRY_COMPARE_WITH_TIMEOUT(result, QString(), OpenTimeoutMs);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x44));
    const auto segment = parseDeepLink(QStringLiteral("ida:///segments?ea=") +
                                       hexAddress(Base + 0x44));
    QVERIFY(segment.link);
    result = QStringLiteral("pending");
    bench.window->openLink(*segment.link,
                           [&](const QString &error) { result = error; });
    QTRY_COMPARE_WITH_TIMEOUT(result, QString(), OpenTimeoutMs);
    ChooserView *segments = nullptr;
    for (auto *chooser : bench.window->findChildren<ChooserView *>())
      if (chooser->model().kind() == ChooserKind::Segments)
        segments = chooser;
    QVERIFY(segments);
    QTRY_COMPARE_WITH_TIMEOUT(segments->currentAddress(),
                              std::optional<Address>(Base), OpenTimeoutMs);
  }

  void disassemblyLinksLeaveTheGraphAtTheirOwnTarget_data() {
    QTest::addColumn<QString>("resource");
    QTest::addColumn<quint64>("expectedRva");
    QTest::newRow("address") << QStringLiteral("addresses") << quint64(0x44);
    QTest::newRow("function") << QStringLiteral("functions") << quint64(0x40);
  }

  void disassemblyLinksLeaveTheGraphAtTheirOwnTarget() {
    QFETCH(QString, resource);
    QFETCH(quint64, expectedRva);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("fixture.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address origin = Base + 0x50;
    assembly->navigate(origin);
    QTRY_COMPARE(assembly->currentItem(), std::optional<Address>(origin));
    assembly->setGraphMode(true);
    QTRY_VERIFY_WITH_TIMEOUT(
        assembly->graphMode() && assembly->graph()->loaded(), OpenTimeoutMs);
    QCOMPARE(assembly->currentFunction(), std::optional<Address>(Base + 0x50));
    QCOMPARE(assembly->currentAddress(), std::optional<Address>(origin));
    const auto link = parseDeepLink(
        QStringLiteral("ida:///%1?rva=0x44&view=disasm").arg(resource));
    QVERIFY(link.link);
    QString result = QStringLiteral("pending");
    bench.window->openLink(*link.link,
                           [&](const QString &error) { result = error; });
    QTRY_COMPARE_WITH_TIMEOUT(result, QString(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!assembly->graphMode() &&
                                 !assembly->listing()->jumpPending(),
                             OpenTimeoutMs);
    QCOMPARE(assembly->currentItem(),
             std::optional<Address>(Base + expectedRva));
    assembly->goBack();
    QTRY_COMPARE(assembly->currentItem(), std::optional<Address>(origin));
  }

  void laterLinksSupersedeAQueuedGraphView() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("fixture.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x50);
    QTRY_COMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x50));
    // A loaded session may still be waiting for its address space. Complete
    // one link before testing the graph switch queued by an accepted link.
    const auto ready =
        parseDeepLink(QStringLiteral("ida:///addresses?rva=0x50&view=disasm"));
    QVERIFY(ready.link);
    QString readyResult = QStringLiteral("pending");
    bench.window->openLink(*ready.link,
                           [&](const QString &error) { readyResult = error; });
    QTRY_COMPARE_WITH_TIMEOUT(readyResult, QString(), OpenTimeoutMs);
    QStringList results;
    for (const auto &text :
         {QStringLiteral("ida:///addresses?rva=0x50&view=graph"),
          QStringLiteral("ida:///addresses?rva=0x44&view=disasm")}) {
      const auto link = parseDeepLink(text);
      QVERIFY(link.link);
      bench.window->openLink(*link.link,
                             [&](const QString &error) { results << error; });
    }
    bool queued = false;
    QTimer::singleShot(0, this, [&] { queued = true; });
    QTRY_VERIFY_WITH_TIMEOUT(queued && results.size() == 2 &&
                                 !assembly->listing()->jumpPending(),
                             OpenTimeoutMs);
    QCOMPARE(results, (QStringList{QString(), QString()}));
    QVERIFY(!assembly->graphMode());
    QCOMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x44));
  }

  void initialNavigationOverviewDoesNotWaitForRefreshDebounce() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(writeFixture(directory, QStringLiteral("fixture.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.indexReady() && !session.busy(),
                             OpenTimeoutMs);
    AddressSpace space;
    NavigationBand band(session, space);
    QSignalSpy changed(&band, &NavigationBand::spaceChanged);
    QVERIFY(space.empty());
    QVERIFY(!session.queries().hasPending());

    // Opening must admit the address-map request in the same event turn:
    // the first listing cannot navigate until that map arrives.
    emit session.opened();
    QVERIFY(session.queries().hasPending());
    QTRY_VERIFY_WITH_TIMEOUT(!space.empty(), OpenTimeoutMs);
    QCOMPARE(changed.size(), 1);
  }

  void listingKeepsRowsWhenTheirGenerationIsPublished() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(writeFixture(directory, QStringLiteral("fixture.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.indexReady() && !session.busy(),
                             OpenTimeoutMs);
    AddressSpace space;
    ListingView listing(session, space);
    listing.jumpTo(Base + 0x40);
    QTRY_VERIFY_WITH_TIMEOUT(listing.searchReady(), OpenTimeoutMs);
    const QString text = listing.searchText();
    // Delivery can install this generation before Session publishes it.
    // Publishing that same generation must not invalidate the current rows.
    emit session.generationChanged();
    QVERIFY(listing.searchReady());
    QCOMPARE(listing.searchText(), text);
  }

  void listingFirstContentPaintWaitsForCurrentRows_data() {
    QTest::addColumn<bool>("jump");
    QTest::newRow("refresh") << false;
    QTest::newRow("pending-jump") << true;
  }

  void listingFirstContentPaintWaitsForCurrentRows() {
    QFETCH(bool, jump);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("fixture.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(
        bench.session.indexReady() && !bench.session.busy(), OpenTimeoutMs);
    AddressSpace space;
    ListingView listing(bench.session, space);
    listing.resize(640, 480);
    listing.jumpTo(Base + 0x40);
    QTRY_VERIFY_WITH_TIMEOUT(listing.searchReady(), OpenTimeoutMs);
    QSignalSpy painted(&listing, &ListingView::contentPainted);
    // Render the real viewport synchronously while a replacement is pending.
    // Its retained rows are visible, but cannot qualify a useful first frame.
    if (jump)
      listing.jumpTo(Base + 0x50);
    else
      listing.refresh();
    QVERIFY(!listing.searchReady());
    QVERIFY(!listing.searchText().isEmpty());
    const auto paint = [&] {
      QImage image(listing.viewport()->size(), QImage::Format_ARGB32);
      listing.viewport()->render(&image);
    };
    paint();
    QCOMPARE(painted.size(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(listing.searchReady(), OpenTimeoutMs);
    paint();
    QCOMPARE(painted.size(), 1);
    paint();
    QCOMPARE(painted.size(), 1);
    listing.clear();
    paint();
    QCOMPARE(painted.size(), 1);
    listing.jumpTo(Base + 0x40);
    QTRY_VERIFY_WITH_TIMEOUT(listing.searchReady(), OpenTimeoutMs);
    paint();
    QCOMPARE(painted.size(), 2);
  }

  void listingRefreshKeepsItsContextAndArrows() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("fixture.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *listing = bench.window->disassembly()->listing();
    const Address address = Base + 0x1400;
    bench.window->disassembly()->navigate(address);
    QTRY_COMPARE(listing->currentItem(), std::optional<Address>(address));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.indexReady() &&
                                 !bench.session.busy() &&
                                 ListingViewTestAccess::settled(*listing),
                             OpenTimeoutMs);
    const auto first = ListingViewTestAccess::first(*listing);
    const auto last = ListingViewTestAccess::last(*listing);
    const auto top = listing->topItem();
    const auto arrows = ListingViewTestAccess::arrows(*listing);
    QVERIFY(ListingViewTestAccess::rows(*listing) >
            ::neverd::limits::gui::kListingViewPageLines * 2);
    bool keptContext = true, keptArrows = true;
    int paints = 0;
    ListingPaintObserver observer;
    observer.Observe = [&] {
      ++paints;
      keptContext &= ListingViewTestAccess::rows(*listing) > 0 &&
                     ListingViewTestAccess::first(*listing) <= first &&
                     ListingViewTestAccess::last(*listing) >= last;
      keptArrows &= ListingViewTestAccess::arrows(*listing) == arrows;
    };
    listing->viewport()->installEventFilter(&observer);
    for (int pass = 0; pass < 3; ++pass) {
      const int previous = paints;
      listing->refresh();
      QTRY_VERIFY_WITH_TIMEOUT(ListingViewTestAccess::settled(*listing) &&
                                   paints > previous,
                               OpenTimeoutMs);
      QCOMPARE(listing->currentItem(), std::optional<Address>(address));
      QCOMPARE(listing->topItem(), top);
      QVERIFY(keptContext);
      QVERIFY(keptArrows);
    }
    listing->viewport()->removeEventFilter(&observer);
    // A real revision still replaces the rows, and navigation supersedes a
    // refresh instead of letting its late completion move the cursor back.
    bench.session.setComment(address,
                             QStringLiteral("updated without a flash"));
    QTRY_VERIFY_WITH_TIMEOUT(listing->currentLineText().contains(
                                 QStringLiteral("updated without a flash")),
                             OpenTimeoutMs);
    listing->refresh();
    bench.window->disassembly()->navigate(Base + 0x1500);
    QTRY_COMPARE_WITH_TIMEOUT(listing->currentItem(),
                              std::optional<Address>(Base + 0x1500),
                              OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(ListingViewTestAccess::settled(*listing),
                             OpenTimeoutMs);
    QCOMPARE(listing->currentItem(), std::optional<Address>(Base + 0x1500));
  }

  void laterLinksSupersedeWhileTheProgramOpens() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    QStringList results;
    connect(&bench.session, &Session::opened, this, [&] {
      for (const auto &text :
           {QStringLiteral("ida:///addresses?rva=0x50&view=graph"),
            QStringLiteral("ida:///addresses?rva=0x44&view=disasm")}) {
        const auto link = parseDeepLink(text);
        QVERIFY(link.link);
        bench.window->openLink(*link.link,
                               [&](const QString &error) { results << error; });
      }
    });
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("fixture.bin")));
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 2, OpenTimeoutMs);
    QCOMPARE(results.front(),
             QStringLiteral("Another link took this one's place."));
    QCOMPARE(results.back(), QString());
    auto *assembly = bench.window->disassembly();
    QTRY_VERIFY_WITH_TIMEOUT(!assembly->listing()->jumpPending(),
                             OpenTimeoutMs);
    QVERIFY(!assembly->graphMode());
    QCOMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x44));
  }

  void repeatedFunctionLinksReuseOnlyTheCurrentListingRevision() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("fixture.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    auto *listing = assembly->listing();
    const Address address = Base + 0x140;
    assembly->navigate(address);
    QTRY_COMPARE(listing->currentItem(), std::optional<Address>(address));
    QTRY_VERIFY_WITH_TIMEOUT(
        bench.session.indexReady() && !bench.session.busy(), OpenTimeoutMs);
    QCOMPARE(listing->loadedFunctionAt(address),
             std::optional<Address>(address));
    const auto link =
        parseDeepLink(QStringLiteral("ida:///functions?ea=") +
                      hexAddress(address) + QStringLiteral("&view=disasm"));
    QVERIFY(link.link);
    QString result = QStringLiteral("pending");
    bool reloaded = true;
    bench.window->openLink(*link.link, [&](const QString &error) {
      reloaded = listing->jumpPending();
      result = error;
    });
    QTRY_COMPARE_WITH_TIMEOUT(result, QString(), OpenTimeoutMs);
    QVERIFY(!reloaded);
    QCOMPARE(listing->currentItem(), std::optional<Address>(address));
    bool edited = false, refreshed = false, invalidated = false;
    connect(
        &bench.session, &Session::commented, this,
        [&](Address, const QString &) {
          invalidated = !listing->loadedFunctionAt(address);
          assembly->navigate(address);
          refreshed = listing->jumpPending();
          edited = true;
        },
        Qt::SingleShotConnection);
    bench.session.setComment(address, QStringLiteral("fresh listing"));
    QTRY_VERIFY_WITH_TIMEOUT(edited, OpenTimeoutMs);
    QVERIFY(refreshed);
    QVERIFY(invalidated);
    QTRY_VERIFY_WITH_TIMEOUT(!listing->jumpPending(), OpenTimeoutMs);
    QCOMPARE(listing->currentItem(), std::optional<Address>(address));
    QCOMPARE(listing->loadedFunctionAt(address),
             std::optional<Address>(address));
  }

  void functionLinksSelectRowsBeyondTheLoadedPages() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("many-functions.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *functions = bench.functions();
    QTRY_COMPARE_WITH_TIMEOUT(functions->model().total(), 20000, OpenTimeoutMs);
    functions->model().sort(0, Qt::DescendingOrder);
    functions->setFilterText(QStringLiteral("function_1"));
    const Address target = Base + 599 * 16;
    const auto parsed = parseDeepLink(QStringLiteral("ida:///functions?ea=") +
                                      hexAddress(target));
    QVERIFY(parsed.link);
    QString result = QStringLiteral("pending");
    bench.window->openLink(*parsed.link,
                           [&](const QString &error) { result = error; });
    QTRY_COMPARE_WITH_TIMEOUT(result, QString(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(functions->currentAddress(),
                              std::optional<Address>(target), OpenTimeoutMs);
    QCOMPARE(functions->model().filter(), QString());
  }

  void nativeUrlEventsReachTheWindowThatOwnsTheirProgram() {
    QTemporaryDir directory;
    Workbench receiver;
    LoadDialogAcceptor loadDialogs;
    receiver.window->openFile(
        writeFixture(directory, QStringLiteral("receiver.bin")));
    QProcess target;
    target.start(QString::fromLocal8Bit(TEST_GUI),
                 {QStringLiteral("--worker"),
                  QString::fromLocal8Bit(TEST_WORKER),
                  QStringLiteral("--smoke-test"), QStringLiteral("--capture"),
                  directory.filePath(QStringLiteral("unused.png")),
                  QStringLiteral("--capture-delay"), QStringLiteral("60000"),
                  writeFixture(directory, QStringLiteral("target.bin"))});
    const auto closeTarget = qScopeGuard([&] {
      target.terminate();
      if (!target.waitForFinished(OpenTimeoutMs)) {
        target.kill();
        target.waitForFinished(OpenTimeoutMs);
      }
    });
    QVERIFY(target.waitForStarted(OpenTimeoutMs));
    QString endpoint;
    const auto targetInfo = [&]() -> QJsonObject {
      if (endpoint.isEmpty())
        for (const auto &instance : InstanceServer::discover())
          if (instance.pid == target.processId())
            endpoint = instance.server;
      return endpoint.isEmpty()
                 ? QJsonObject()
                 : InstanceServer::request(
                       endpoint, {{QStringLiteral("cmd"), "get_info"}})
                       .value_or(QJsonObject());
    };
    QTRY_VERIFY_WITH_TIMEOUT(receiver.session.loaded() &&
                                 targetInfo().value("loaded").toBool(),
                             OpenTimeoutMs);
    receiver.window->disassembly()->navigate(Base + 0x44);
    QTRY_COMPARE(receiver.window->disassembly()->currentItem(),
                 std::optional<Address>(Base + 0x44));
    LinkOpener opener(*receiver.window, receiver.session);
    qApp->installEventFilter(&opener);
    QFileOpenEvent event(
        QUrl(QStringLiteral("ida:///target.bin/addresses?ea=") +
             hexAddress(Base + 0x52)));
    QCoreApplication::sendEvent(qApp, &event);
    QTRY_COMPARE_WITH_TIMEOUT(
        addressValue(
            targetInfo().value("location").toObject().value("address")),
        std::optional<Address>(Base + 0x52), OpenTimeoutMs);
    QCOMPARE(receiver.window->disassembly()->currentItem(),
             std::optional<Address>(Base + 0x44));
    QFileOpenEvent own(
        QUrl(QStringLiteral("neverd:///receiver.bin/addresses?ea=") +
             hexAddress(Base + 0x42)));
    QCoreApplication::sendEvent(qApp, &own);
    QTRY_COMPARE_WITH_TIMEOUT(receiver.window->disassembly()->currentItem(),
                              std::optional<Address>(Base + 0x42),
                              OpenTimeoutMs);
  }

  void instanceProbesStopAtAResponsiveMatchAndShareTheirDeadline() {
    QTemporaryDir directory;
    QLocalServer stalled, untouched;
    stalled.setMaxPendingConnections(
        ::neverd::limits::gui::kInstanceProbeConcurrency + 1);
    stalled.setSocketOptions(QLocalServer::UserAccessOption);
    untouched.setSocketOptions(QLocalServer::UserAccessOption);
#ifdef Q_OS_WIN
    const QString prefix = QFileInfo(directory.path()).fileName();
#else
    const QString prefix = directory.path() + QLatin1Char('/');
#endif
    QVERIFY(stalled.listen(prefix + QStringLiteral("stalled")));
    QVERIFY(untouched.listen(prefix + QStringLiteral("untouched")));
    InstanceServer responder;
    responder.setInfo([] {
      return QJsonObject{{"loaded", true}, {"input_name", "target.bin"}};
    });
    QVERIFY(responder.start());
    QStringList servers{responder.serverName()};
    for (int index = 1;
         index < ::neverd::limits::gui::kInstanceProbeConcurrency; ++index)
      servers << stalled.fullServerName();
    servers << untouched.fullServerName();
    bool matched = false;
    auto replies = InstanceServer::requestMany(
        servers, {{"cmd", "get_info"}},
        ::neverd::limits::gui::kInstanceServerRequestTimeoutMs,
        [&](int index, const QJsonObject &info) {
          matched = index == 0 && info.value("input_name") == "target.bin";
          return matched;
        });
    QVERIFY(matched);
    QVERIFY(replies[0]);
    QVERIFY(!replies.back());
    QVERIFY(!untouched.hasPendingConnections());
    // A stalled earlier endpoint must not consume the later endpoint's
    // opportunity to answer within the same request budget.
    replies = InstanceServer::requestMany(
        {stalled.fullServerName(), responder.serverName()},
        {{"cmd", "get_info"}});
    QVERIFY(!replies[0]);
    QVERIFY(replies[1]);
    QCOMPARE(replies[1]->value("input_name"), QJsonValue("target.bin"));
    servers.clear();
    for (int index = 0;
         index < ::neverd::limits::gui::kInstanceProbeConcurrency; ++index)
      servers << stalled.fullServerName();
    servers << responder.serverName();
    replies = InstanceServer::requestMany(servers, {{"cmd", "get_info"}});
    QVERIFY(replies.back());
    QCOMPARE(replies.back()->value("input_name"), QJsonValue("target.bin"));
    QLocalServer malformed;
    malformed.setSocketOptions(QLocalServer::UserAccessOption);
    QVERIFY(malformed.listen(prefix + QStringLiteral("malformed")));
    connect(&malformed, &QLocalServer::newConnection, this, [&] {
      while (auto *socket = malformed.nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected, socket,
                &QObject::deleteLater);
        socket->write("{\"loaded\":true,\"input_name\":\"target.bin\"}\n");
        socket->flush();
      }
    });
    const auto invalid = InstanceServer::requestMany(
        {malformed.fullServerName()}, {{"cmd", "get_info"}});
    QVERIFY(!invalid[0]);
  }

  void instanceRequestsSurviveQuotedFragmentBoundaries() {
    InstanceServer server;
    QVERIFY(server.start());
    const QString text = QStringLiteral("quoted } ") + QLatin1Char('\\') +
                         QLatin1Char('"') + QStringLiteral(" { 中文");
    const QByteArray wire =
        QJsonDocument(QJsonObject{{"cmd", "ping"}, {"padding", text}})
            .toJson(QJsonDocument::Compact);
    const QList<qsizetype> splits{wire.indexOf('}') + 1, wire.indexOf('\\') + 1,
                                  wire.indexOf(QStringLiteral("中").toUtf8()) +
                                      1};
    const auto connected = [&](QLocalSocket &socket) {
      socket.connectToServer(server.serverName());
      return socket.waitForConnected(
          ::neverd::limits::gui::kInstanceServerRequestTimeoutMs);
    };
    const auto reply = [&](QLocalSocket &socket) {
      QByteArray bytes;
      const bool received = QTest::qWaitFor(
          [&] {
            bytes += socket.readAll();
            return bytes.contains('\n');
          },
          OpenTimeoutMs);
      return received ? QJsonDocument::fromJson(bytes).object() : QJsonObject();
    };
    for (const auto split : splits) {
      QVERIFY(split > 0 && split < wire.size());
      QLocalSocket socket, barrier;
      QVERIFY(connected(socket));
      socket.write(wire.left(split));
      QVERIFY(connected(barrier));
      barrier.write("{\"cmd\":\"ping\"}");
      QCOMPARE(reply(barrier).value("status"), QJsonValue("ok"));
      QVERIFY(socket.readAll().isEmpty());
      socket.write(wire.mid(split));
      QCOMPARE(reply(socket).value("status"), QJsonValue("ok"));
    }
    QLocalSocket malformed;
    QVERIFY(connected(malformed));
    malformed.write("{\"cmd\":}");
    const auto refused = reply(malformed);
    QCOMPARE(refused.value("status"), QJsonValue("error"));
    QVERIFY(!refused.value("message").toString().isEmpty());
  }

  void pathfinderChooserLoadsBeforeFiltering_data() {
    QTest::addColumn<bool>("exclusion");
    QTest::addColumn<bool>("chooseImport");
    QTest::newRow("waypoint-function") << false << false;
    QTest::newRow("waypoint-import") << false << true;
    QTest::newRow("exclusion-function") << true << false;
    QTest::newRow("exclusion-import") << true << true;
  }

  void pathfinderChooserLoadsBeforeFiltering() {
    QFETCH(bool, exclusion);
    QFETCH(bool, chooseImport);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-import.bin")));
    // No later discovery or revision signal may load the new dialog for it.
    QTRY_VERIFY_WITH_TIMEOUT(
        bench.session.indexReady() && !bench.session.busy(), OpenTimeoutMs);
    bench.action(ActionId::ViewPathfinder)->trigger();
    auto *view = bench.window->findChild<PathfinderView *>();
    QVERIFY(view);
    auto *add = view->findChild<QToolButton *>(
        exclusion ? QStringLiteral("pathfinderExclude")
                  : QStringLiteral("pathfinderAdd"));
    QVERIFY(add && add->isEnabled());

    bool loaded = false;
    QTimer chooser, timeout;
    connect(&chooser, &QTimer::timeout, this, [&] {
      QPointer<QDialog> dialog = visibleDialog();
      if (!dialog || dialog->objectName() != QLatin1String("pathfinderChooser"))
        return;
      auto *tabs = dialog->findChild<QTabWidget *>();
      if (!tabs || tabs->count() != 2)
        return;
      auto *functions = qobject_cast<ChooserView *>(tabs->widget(0));
      auto *imports = qobject_cast<ChooserView *>(tabs->widget(1));
      if (!functions || !imports || functions->model().addressAt(0) != Base ||
          imports->model().addressAt(0) != Base + 0x160)
        return;
      chooser.stop();
      timeout.stop();
      // Both pages arrived with empty filters, including the hidden tab.
      loaded = functions->model().filter().isEmpty() &&
               imports->model().filter().isEmpty() &&
               functions->model().index(0, 0).data().toString() ==
                   QStringLiteral("function_0") &&
               imports->model().index(0, 2).data().toString() ==
                   QStringLiteral("function_22");
      tabs->setCurrentIndex(chooseImport ? 1 : 0);
      auto *selected = chooseImport ? imports : functions;
      selected->table()->setCurrentIndex(selected->model().index(0, 0));
      if (auto *buttons = dialog->findChild<QDialogButtonBox *>())
        QTest::mouseClick(buttons->button(QDialogButtonBox::Ok),
                          Qt::LeftButton);
      else
        dialog->reject();
    });
    connect(&timeout, &QTimer::timeout, this, [] {
      if (auto *dialog = visibleDialog())
        dialog->reject();
    });
    timeout.setSingleShot(true);
    timeout.start(OpenTimeoutMs);
    chooser.start(10);
    QTest::mouseClick(add, Qt::LeftButton);
    QTRY_VERIFY2(loaded,
                 "The waypoint chooser did not load its unfiltered lists.");
    const QVector<Address> expected{chooseImport ? Base + 0x160 : Base};
    QTRY_COMPARE_WITH_TIMEOUT(exclusion ? view->exclusions()
                                        : view->waypoints(),
                              expected, OpenTimeoutMs);
  }

  void pathfinderListsTheCallPathsBetweenWaypoints() {
    // IDA's Pathfinder: Shift+F9 starts a path at the function under the
    // cursor and Ctrl+Shift+F9 adds the next.  Between the two the list
    // shows the paths, shortest first, each step with the call that reaches
    // it; an exclusion takes away the paths through a function, and the
    // project keeps the path.
    QTemporaryDir directory;
    const QString input =
        writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(input);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.session.findsCallPaths());
    QCOMPARE(bench.action(ActionId::PathfinderNewPath)->shortcut(),
             QKeySequence(QStringLiteral("Shift+F9")));
    QCOMPARE(bench.action(ActionId::PathfinderAddWaypoint)->shortcut(),
             QKeySequence(QStringLiteral("Ctrl+Shift+F9")));
    auto *disassembly = bench.window->disassembly();
    // An address inside a function stands for the function.
    disassembly->navigate(Base + 4);
    QTRY_COMPARE(disassembly->currentFunction(), std::optional<Address>(Base));
    QTRY_VERIFY(bench.action(ActionId::PathfinderNewPath)->isEnabled());
    bench.action(ActionId::PathfinderNewPath)->trigger();
    auto *view = bench.window->findChild<PathfinderView *>();
    QVERIFY(view);
    QTRY_COMPARE_WITH_TIMEOUT(view->waypoints(), QVector<Address>{Base},
                              OpenTimeoutMs);
    auto *status =
        view->findChild<QLabel *>(QStringLiteral("pathfinderStatus"));
    QVERIFY(status);
    QVERIFY(
        status->text().startsWith(QStringLiteral("Add two waypoints or more")));
    disassembly->navigate(Base + 0x42);
    QTRY_COMPARE(disassembly->currentFunction(),
                 std::optional<Address>(Base + 0x40));
    bench.action(ActionId::PathfinderAddWaypoint)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(view->waypoints(),
                              (QVector<Address>{Base, Base + 0x40}),
                              OpenTimeoutMs);

    // function_0 reaches function_4 through function_3, and through
    // function_1, function_2 and function_3.
    QTreeWidget *list = view->waypointList();
    QTRY_COMPARE_WITH_TIMEOUT(status->text(),
                              QStringLiteral("Found 5 nodes on the paths."),
                              OpenTimeoutMs);
    QCOMPARE(list->topLevelItemCount(), 3);
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("function_0"));
    QCOMPARE(list->topLevelItem(2)->text(0), QStringLiteral("function_4"));
    QTreeWidgetItem *segment = list->topLevelItem(1);
    QCOMPARE(segment->text(0), QStringLiteral("↓ 5 nodes (shortest: 2)"));
    QCOMPARE(segment->childCount(), 2);
    QCOMPARE(segment->child(0)->text(0),
             QStringLiteral("Path 1: function_0 → function_3 → "
                            "function_4"));
    QCOMPARE(segment->child(1)->text(0),
             QStringLiteral("Path 2: function_0 → function_1 → "
                            "function_2 → function_3 → function_4"));
    // A step names the instruction that reaches it, where activating it goes.
    QTreeWidgetItem *step = segment->child(0)->child(0);
    QCOMPARE(step->text(0), QStringLiteral("function_3"));
    QCOMPARE(step->text(2),
             QStringLiteral("call at %1").arg(displayAddress(Base + 2)));
    emit list->itemActivated(step, 0);
    QTRY_COMPARE(disassembly->currentAddress(),
                 std::optional<Address>(Base + 2));
    // Waypoints copy as their rows.
    list->clearSelection();
    list->topLevelItem(0)->setSelected(true);
    QCOMPARE(view->selectedText(),
             QStringLiteral("function_0\t%1").arg(displayAddress(Base)));

    // Without function_3 neither waypoint reaches the other.
    view->addExclusion(Base + 0x30);
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("No path found."),
                              OpenTimeoutMs);
    QCOMPARE(view->exclusions(), QVector<Address>{Base + 0x30});
    QCOMPARE(list->topLevelItem(1)->text(0), QStringLiteral("↓ No path"));

    // The project keeps the waypoints and the exclusion with the user's
    // place, and shows them when it opens again.
    bench.session.saveDatabaseState();
    bench.action(ActionId::FileClose)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(view->waypoints().isEmpty());
    bench.window->openFile(input);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(view->waypoints(),
                              (QVector<Address>{Base, Base + 0x40}),
                              OpenTimeoutMs);
    QCOMPARE(view->exclusions(), QVector<Address>{Base + 0x30});
    QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("No path found."),
                              OpenTimeoutMs);
  }

  void viewsNarrowPastTheirRows() {
    // A status line or a row of buttons is cut at its view's edge.  Kept
    // whole, the disassembly's status line held its dock over 500 pixels
    // wide, and the separators beside that dock would not move.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-import.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    bench.action(ActionId::ViewExtensions)->trigger();
    bench.action(ActionId::ViewLocalTypes)->trigger();
    bench.action(ActionId::ViewPathfinder)->trigger();
    auto *code = bench.codeView(QStringLiteral("source"));
    auto *extensions = bench.window->findChild<ExtensionsView *>();
    auto *types = bench.window->findChild<LocalTypesView *>();
    auto *pathfinder = bench.window->findChild<PathfinderView *>();
    QVERIFY(code && extensions && types && pathfinder && bench.functions());
    // Every button shows, as the fold button does for library code.
    for (auto *button : code->findChildren<QToolButton *>())
      button->show();
    const QString wide(400, QLatin1Char('x'));
    const QList<QWidget *> views{bench.window->disassembly(),
                                 code,
                                 bench.functions(),
                                 extensions,
                                 types,
                                 pathfinder};
    for (auto *view : views) {
      for (auto *label : view->findChildren<QLabel *>())
        label->setText(wide);
      QVERIFY2(view->minimumSizeHint().width() <= NarrowViewWidth,
               view->metaObject()->className());
    }
  }

  void closingFileClearsDisassemblyAndReopensAtTheSameAddress() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    auto *listing = disassembly->listing();
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(listing->currentItem(), std::optional<Address>(Base + 0x140));
    QVERIFY(!listing->currentLineText().isEmpty());
    listing->setFocus();
    QTest::keyClick(listing, Qt::Key_Home);
    QTest::keyClick(listing, Qt::Key_Right, Qt::ShiftModifier);
    QVERIFY(!listing->selectedText().isEmpty());
    listing->horizontalScrollBar()->setValue(
        listing->horizontalScrollBar()->maximum());

    bench.action(ActionId::FileClose)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.loaded(), OpenTimeoutMs);
    // A closed image has no cached address, copyable text or scroll position.
    QVERIFY(!listing->currentAddress());
    QVERIFY(!listing->currentItem());
    QVERIFY(!listing->topItem());
    QVERIFY(!listing->currentFunction());
    QVERIFY(!listing->cursorLine());
    QVERIFY(listing->currentLineText().isEmpty());
    QVERIFY(listing->currentToken().isEmpty());
    QVERIFY(listing->selectedText().isEmpty());
    QVERIFY(!listing->jumpPending());
    QCOMPARE(listing->horizontalScrollBar()->maximum(), 0);
    QCOMPARE(listing->horizontalScrollBar()->value(), 0);
    QCOMPARE(listing->verticalScrollBar()->maximum(), 0);
    QCOMPARE(listing->verticalScrollBar()->value(), 0);
    QVERIFY(!disassembly->currentAddress());
    QVERIFY(!disassembly->canGoBack());
    QVERIFY(!disassembly->canGoForward());

    QSignalSpy locations(listing, &ListingView::locationChanged);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(listing->currentItem(), std::optional<Address>(Base + 0x140));
    // Reusing an address in the new image still publishes its location.
    QVERIFY(!locations.isEmpty());
  }

  void closingDuringAJumpCancelsTheDeferredGraphSwitch() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    disassembly->navigate(Base + 0x140);
    QVERIFY(disassembly->listing()->jumpPending());
    disassembly->setGraphMode(true);
    QVERIFY(!disassembly->graphMode());
    bench.action(ActionId::FileClose)->trigger();
    QVERIFY(!bench.session.loaded());
    QVERIFY(!disassembly->listing()->jumpPending());

    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x140));
    QVERIFY(!disassembly->graphMode());
  }

  void graphStopsWaitingWhenTheFileCloses() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->findChild<DisassemblyView *>();
    QVERIFY(disassembly);
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x140));
    // The graph says it is laying out until its layout arrives...
    bench.action(ActionId::ViewToggleGraph)->trigger();
    QVERIFY(disassembly->graph()->waiting());
    // ...and stops when the file closes first.
    bench.session.closeFile();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!disassembly->graph()->waiting());
    QVERIFY(!disassembly->graph()->loaded());
    QVERIFY(!disassembly->graph()->function());
    QVERIFY(!disassembly->graph()->currentAddress());
    QVERIFY(!disassembly->listing()->currentItem());
  }

  void hexBytePatchF2PreviewApplyCancelAndRevert_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
  }

  void hexBytePatchF2PreviewApplyCancelAndRevert() {
    QFETCH(bool, dark);
    auto &theme = Theme::instance();
    const auto previous = theme.mode();
    const auto restore = qScopeGuard([&] { theme.setMode(previous); });
    theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("patch.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    for (auto *dock :
         bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->uniqueName() == QLatin1String("hex-1"))
        dock->raise();
    auto *hex = bench.window->findChild<HexView *>();
    QVERIFY(hex);
    QTRY_VERIFY(hex->byteAt(Base).has_value());
    const auto original = *hex->byteAt(Base);
    hex->setCurrent(Base);
    hex->setFocus();
    QTest::keyClick(hex, Qt::Key_F2);
    QTRY_VERIFY(hex->findChild<PatchEditor *>());
    QPointer<PatchEditor> editor = hex->findChild<PatchEditor *>();
    auto *input = editor->findChild<QLineEdit *>("patchInput");
    auto *apply = editor->findChild<QPushButton *>("patchApply");
    QVERIFY(input && apply);
    input->setText("2a");
    QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), OpenTimeoutMs);
    QCOMPARE(*hex->byteAt(Base), original);
    if (const auto capture = qEnvironmentVariable("NEVERD_PATCH_CAPTURE_DIR");
        !capture.isEmpty()) {
      QDir().mkpath(capture);
      QVERIFY(bench.window->grab().save(
          capture + (dark ? "/hex-byte-preview-dark.png"
                          : "/hex-byte-preview-light.png")));
    }
    QTest::keyClick(input, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(!editor, OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(hex->byteAt(Base), std::optional<quint8>(0x2a),
                              OpenTimeoutMs);
    QTRY_VERIFY(bench.session.hasBytePatch(Base));
    hex->setCurrent(Base);
    hex->setFocus();
    QTest::keyClick(hex, Qt::Key_F2);
    QTRY_VERIFY(hex->findChild<PatchEditor *>());
    editor = hex->findChild<PatchEditor *>();
    input = editor->findChild<QLineEdit *>("patchInput");
    input->setText("55");
    QTest::keyClick(input, Qt::Key_Escape);
    QTRY_VERIFY(!editor);
    QCOMPARE(hex->byteAt(Base), std::optional<quint8>(0x2a));
    bench.session.editBytePatch("patch_revert", {{"address", hexAddress(Base)}},
                                bench.session.epoch());
    QTRY_COMPARE_WITH_TIMEOUT(hex->byteAt(Base),
                              std::optional<quint8>(original), OpenTimeoutMs);
    QTRY_VERIFY(!bench.session.hasBytePatch(Base));
  }

  void assemblyEditorRequiresOverlapConsentAndAppliesPaddedPreview() {
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("assembly-patch.bin"));
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    PatchEditor editor(bench.session, Base, "assemble", "mov eax, 1",
                       bench.window.get());
    editor.show();
    auto *apply = editor.findChild<QPushButton *>("patchApply");
    auto *preview = editor.findChild<QPlainTextEdit *>("patchPreview");
    auto *input = editor.findChild<QLineEdit *>("patchInput");
    QTRY_VERIFY_WITH_TIMEOUT(
        preview->toPlainText().contains("enable replacement"), OpenTimeoutMs);
    QVERIFY(!apply->isEnabled());
    auto *overlap = editor.findChildren<QCheckBox *>().at(1);
    overlap->setChecked(true);
    QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), OpenTimeoutMs);
    QVERIFY(preview->toPlainText().contains("b8010000009090"));
    input->setText("ret");
    overlap->setChecked(false);
    QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), OpenTimeoutMs);
    QVERIFY(preview->toPlainText().contains("c390"));
    QSignalSpy applied(&editor, &PatchEditor::applied);
    QTest::keyClick(input, Qt::Key_Return);
    // The committed request is already fixed. Do not accept edits that would
    // disappear when its asynchronous completion closes the editor.
    QVERIFY(!input->isEnabled());
    QVERIFY(!overlap->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(applied.count(), 1, OpenTimeoutMs);
    QTRY_VERIFY(bench.session.hasBytePatch(Base));
  }

  void bytePatchEditorRejectsStalePreview() {
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("stale-patch.bin"));
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    PatchEditor editor(bench.session, Base, "bytes", "2a", bench.window.get());
    editor.show();
    auto *apply = editor.findChild<QPushButton *>("patchApply");
    QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), OpenTimeoutMs);
    bool done = false;
    bench.session.editBytePatch(
        "patch_apply",
        {{"address", hexAddress(Base)}, {"kind", "bytes"}, {"bytes", "55"}},
        bench.session.epoch(), [&] { done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, OpenTimeoutMs);
    QSignalSpy applied(&editor, &PatchEditor::applied);
    QTest::mouseClick(apply, Qt::LeftButton);
    QVERIFY(!apply->isEnabled());
    QCOMPARE(applied.count(), 0);
    QVERIFY(editor.findChild<QPlainTextEdit *>("patchPreview")
                ->toPlainText()
                .contains("input changed"));
  }

  void nativePatchValueAndLargePreview_data() {
    QTest::addColumn<QString>("value");
    QTest::addColumn<QString>("expected");
    QTest::addColumn<bool>("dark");
    for (bool dark : {false, true}) {
      const auto suffix = dark ? "-dark" : "-light";
      QTest::newRow(qPrintable(QStringLiteral("leading-zero-decimal") + suffix))
          << QStringLiteral("042") << QStringLiteral("b82a000000") << dark;
      QTest::newRow(qPrintable(QStringLiteral("decimal-eight") + suffix))
          << QStringLiteral("08") << QStringLiteral("b808000000") << dark;
      QTest::newRow(qPrintable(QStringLiteral("hexadecimal") + suffix))
          << QStringLiteral("0x2a") << QStringLiteral("b82a000000") << dark;
      QTest::newRow(qPrintable(QStringLiteral("large-fill") + suffix))
          << QString() << QString() << dark;
    }
  }

  void nativePatchValueAndLargePreview() {
    QFETCH(QString, value);
    QFETCH(QString, expected);
    QFETCH(bool, dark);
    const auto worker = qEnvironmentVariable("NEVERD_PATCH_NATIVE_WORKER");
    if (worker.isEmpty())
      QSKIP("NEVERD_PATCH_NATIVE_WORKER is not configured");
    auto &theme = Theme::instance();
    const auto previous = theme.mode();
    const auto restore = qScopeGuard([&] { theme.setMode(previous); });
    theme.setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("patch-preview.raw"));
    QFile binary(path);
    QVERIFY(binary.open(QIODevice::WriteOnly));
    const auto bytes =
        QByteArray::fromHex("b807000000c3") + QByteArray(4090, '\xcc');
    QCOMPARE(binary.write(bytes), bytes.size());
    binary.close();
    Workbench bench(worker);
    LoadOptions options;
    options.loader = QStringLiteral("binary");
    options.processor = QStringLiteral("x86_64");
    options.platform = QStringLiteral("sysv");
    options.base = 0x1000;
    options.analysis = false;
    bench.session.open(path, options);
    QTRY_VERIFY2_WITH_TIMEOUT(bench.session.loaded(),
                              qPrintable(bench.session.lastError()),
                              NativeOpenTimeoutMs);
    auto *hex = bench.window->findChild<HexView *>();
    QVERIFY(hex);
    if (auto *dock = bench.dockFor(hex))
      dock->raise();
    hex->setCurrent(options.base, value.isEmpty() ? bytes.size() : 1);
    hex->setFocus();
    QTRY_VERIFY(hex->hasFocus());
    QTRY_VERIFY(hex->byteAt(options.base).has_value());
    QString preview;
    bool confirmed = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, &answer, [&] {
      auto *active = QApplication::activeModalWidget();
      if (auto *input = qobject_cast<QInputDialog *>(active)) {
        input->setTextValue(value);
        input->accept();
      } else if (auto *dialog = qobject_cast<QMessageBox *>(active);
                 dialog && dialog->objectName() ==
                               QStringLiteral("bytePatchConfirmation")) {
        preview = dialog->text();
        answer.stop();
        if (const auto capture =
                qEnvironmentVariable("NEVERD_PATCH_CAPTURE_DIR");
            !capture.isEmpty()) {
          QDir().mkpath(capture);
          QVERIFY(dialog->grab().save(capture + "/patch-confirmation-" +
                                      QTest::currentDataTag() + ".png"));
        }
        confirmed = true;
        dialog->reject();
      }
    });
    answer.start(5);
    const auto action = value.isEmpty() ? ActionId::EditPatchFillNops
                                        : ActionId::EditPatchConstant;
    QTRY_VERIFY(bench.action(action)->isEnabled());
    if (value.isEmpty()) {
      // Let the initial navigation settle before selecting the complete range.
      hex->setCurrent(options.base, bytes.size());
      QCOMPARE(hex->byteSelection(),
               (std::optional(std::pair{
                   options.base, options.base + Address(bytes.size()) - 1})));
    }
    bench.action(action)->trigger();
    QTRY_VERIFY2_WITH_TIMEOUT(confirmed, qPrintable(bench.session.lastError()),
                              NativeOpenTimeoutMs);
    if (value.isEmpty()) {
      QVERIFY2(
          preview.size() < 4096,
          "A large patch must not expand every byte in the confirmation label");
      QVERIFY2(preview.contains(QString::number(bytes.size())),
               qPrintable(preview));
    } else {
      QVERIFY2(preview.contains(expected), qPrintable(preview));
    }
    QVERIFY(!bench.session.hasBytePatches());
    QCOMPARE(readAll(path), bytes);
  }

  void narrowHexViewScrollsToItsText() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    for (auto *dock :
         bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->uniqueName() == QLatin1String("hex-1"))
        dock->raise();
    auto *hex = bench.window->findChild<HexView *>();
    QVERIFY(hex);
    QTRY_VERIFY(hex->isVisible());
    hex->setCurrent(Base);
    QTRY_VERIFY_WITH_TIMEOUT(hex->byteAt(Base).has_value(), OpenTimeoutMs);
    // A pane narrower than a row scrolls sideways to the text column, which
    // it used to cut off.
    hex->setFixedWidth(360);
    auto *sideways = hex->horizontalScrollBar();
    QTRY_VERIFY(sideways->maximum() > 0);
    QVERIFY(sideways->isVisible());
    // Keys keep the byte they reach in view.
    sideways->setValue(0);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    hex->setFocus();
    QTRY_VERIFY(hex->hasFocus());
    for (int byte = 0; byte < 15; ++byte)
      QTest::keyClick(hex, Qt::Key_Right);
    QCOMPARE(hex->currentAddress(), std::optional<Address>(Base + 15));
    QVERIFY(sideways->value() > 0);
    // A click on the last character at the end of the row takes its byte.
    sideways->setValue(sideways->maximum());
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const qreal character = metrics.horizontalAdvance(QLatin1Char('0'));
    const QPoint last(int(hex->viewport()->width() -
                          ::neverd::limits::gui_config::kHexViewMargin -
                          character / 2),
                      int(std::ceil(metrics.lineSpacing())) / 2);
    QTest::mouseClick(hex->viewport(), Qt::LeftButton, Qt::NoModifier, last);
    QVERIFY(hex->currentAddress());
    QCOMPARE(*hex->currentAddress() % 16, Address(15));
  }

  void hexViewShownBeforeOpeningLoadsItsBytes() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    // A restored desktop can show the hex view before the regions arrive.
    for (auto *dock :
         bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->uniqueName() == QLatin1String("hex-1"))
        dock->raise();
    auto *hex = bench.window->findChild<HexView *>();
    QVERIFY(hex);
    QTRY_VERIFY(hex->isVisible());
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(hex->byteAt(Base).has_value(), OpenTimeoutMs);
    QVERIFY(hex->isVisible());

    // Shift and the arrows select bytes, which copy as hex text.
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    hex->setCurrent(Base);
    hex->setFocus();
    QTRY_VERIFY(hex->hasFocus());
    for (int i = 0; i < 3; ++i)
      QTest::keyClick(hex, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(hex->selection(),
             (std::optional<std::pair<Address, Address>>({Base, Base + 3})));
    QStringList bytes;
    for (Address at = Base; at <= Base + 3; ++at) {
      QTRY_VERIFY(hex->byteAt(at).has_value());
      bytes.append(QStringLiteral("%1")
                       .arg(*hex->byteAt(at), 2, 16, QLatin1Char('0'))
                       .toUpper());
    }
    clearClipboard();
    QTest::keyClick(hex, Qt::Key_C, Qt::ControlModifier);
    QTRY_COMPARE(clipboardText(), bytes.join(' '));
    clearClipboard();
    bench.action(ActionId::EditCopy)->trigger();
    QTRY_COMPARE(clipboardText(), bytes.join(' '));
    // A move without Shift ends the selection.
    QTest::keyClick(hex, Qt::Key_Left);
    QVERIFY(!hex->selection());
  }

  void stringReferencesAndHexTextEncodings() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);

    // The reference search lists instructions with the strings they use and
    // opens ready for a query.
    bench.action(ActionId::SearchStringReferences)->trigger();
    ChooserView *references = nullptr;
    for (auto *view : bench.window->findChildren<ChooserView *>())
      if (view->model().kind() == ChooserKind::StringReferences)
        references = view;
    QVERIFY(references);
    QTRY_VERIFY(references->findChild<QLineEdit *>()->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(references->model().total(), 2, OpenTimeoutMs);
    QTRY_COMPARE(references->model().rowObject(1).value("text").toString(),
                 QStringLiteral("Wide"));
    QCOMPARE(references->model().addressAt(1),
             std::optional<Address>(Base + 0x73));
    // Its cross references are those of the string, not the instruction.
    QCOMPARE(references->model().referenceAddressAt(1),
             std::optional<Address>(Base + 0x3108));
    references->setFilterText(QString::fromUtf8("\u6587"));
    QTRY_COMPARE(references->model().total(), 1);

    // In the Strings list, Ctrl+X lists the selected string's references.
    bench.action(ActionId::ViewStrings)->trigger();
    ChooserView *strings = nullptr;
    for (auto *view : bench.window->findChildren<ChooserView *>())
      if (view->model().kind() == ChooserKind::Strings)
        strings = view;
    QVERIFY(strings);
    QTRY_COMPARE_WITH_TIMEOUT(strings->model().total(), 2, OpenTimeoutMs);
    QTRY_VERIFY(strings->model().addressAt(1).has_value());
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    strings->table()->setFocus();
    strings->table()->setCurrentIndex(strings->model().index(1, 0));
    QTRY_VERIFY(strings->table()->hasFocus());
    QString title;
    QTimer::singleShot(0, [&title] {
      if (auto *dialog = visibleDialog(QStringLiteral("referencesDialog"))) {
        title = dialog->windowTitle();
        dialog->close();
      }
    });
    QTest::keyClick(strings->table(), Qt::Key_X, Qt::ControlModifier);
    QTRY_VERIFY(!title.isEmpty());
    QVERIFY2(
        title.contains(QStringLiteral("FFFF800012343108"), Qt::CaseInsensitive),
        qPrintable(title));

    // The hex view's text column reads the bytes in a chosen encoding.
    for (auto *dock :
         bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->uniqueName() == QLatin1String("hex-1"))
        dock->raise();
    auto *hex = bench.window->findChild<HexView *>();
    QVERIFY(hex);
    QTRY_VERIFY(hex->isVisible());
    hex->setCurrent(Base + 0x3108);
    QTRY_COMPARE_WITH_TIMEOUT(hex->textAt(Base + 0x3109),
                              std::optional<QString>(QStringLiteral(".")),
                              OpenTimeoutMs);
    hex->setTextEncoding(QStringLiteral("utf-16le"));
    QTRY_COMPARE_WITH_TIMEOUT(hex->textAt(Base + 0x3108),
                              std::optional<QString>(QStringLiteral("W")),
                              OpenTimeoutMs);
    QCOMPARE(hex->textAt(Base + 0x3109), std::optional<QString>(QString()));
    QCOMPARE(QSettings().value(QStringLiteral("hex/textEncoding")).toString(),
             QStringLiteral("utf-16le"));
    // An encoding the engine does not know falls back to ASCII.
    hex->setTextEncoding(QStringLiteral("klingon"));
    QTRY_COMPARE_WITH_TIMEOUT(hex->textEncoding(), QString(), OpenTimeoutMs);
    QVERIFY(!QSettings().contains(QStringLiteral("hex/textEncoding")));
  }

  void browseListsLoadWithoutTypingAndReloadWithTheProject_data() {
    QTest::addColumn<int>("kind");
    QTest::addColumn<int>("action");
    struct Case {
      const char *Name;
      ChooserKind Kind;
      ActionId Action;
    };
    for (const auto &row : {
             Case{"functions", ChooserKind::Functions, ActionId::ViewFunctions},
             Case{"names", ChooserKind::Names, ActionId::ViewNames},
             Case{"strings", ChooserKind::Strings, ActionId::ViewStrings},
             Case{"segments", ChooserKind::Segments, ActionId::ViewSegments},
             Case{"imports", ChooserKind::Imports, ActionId::ViewImports},
             Case{"exports", ChooserKind::Exports, ActionId::ViewExports},
             Case{"string-references", ChooserKind::StringReferences,
                  ActionId::SearchStringReferences},
         })
      QTest::newRow(row.Name) << int(row.Kind) << int(row.Action);
  }

  void browseListsLoadWithoutTypingAndReloadWithTheProject() {
    QFETCH(int, kind);
    QFETCH(int, action);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    const QString input =
        writeFixture(directory, QStringLiteral("pseudocode-import.bin"));
    bench.window->openFile(input);
    QTRY_VERIFY_WITH_TIMEOUT(
        bench.session.indexReady() && !bench.session.busy(), OpenTimeoutMs);
    QVERIFY(bench.action(ActionId(action))->isEnabled());
    bench.action(ActionId(action))->trigger();
    ChooserView *list = nullptr;
    for (auto *candidate : bench.window->findChildren<ChooserView *>())
      if (candidate->model().kind() == ChooserKind(kind))
        list = candidate;
    QVERIFY(list);
    QTRY_VERIFY_WITH_TIMEOUT(list->model().total() > 0 &&
                                 list->model().addressAt(0).has_value(),
                             OpenTimeoutMs);
    QVERIFY(!list->model().index(0, 0).data().toString().isEmpty());
    QVERIFY(list->model().filter().isEmpty());
    bench.action(ActionId::FileClose)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(list->model().total(), 0);
    bench.window->openFile(input);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(list->model().total() > 0 &&
                                 list->model().addressAt(0).has_value(),
                             OpenTimeoutMs);
    QVERIFY(list->model().filter().isEmpty());
  }

  void jumpMatchesRetireWhenTheExpressionChanges() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("fixture.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(
        bench.session.indexReady() && !bench.session.busy(), OpenTimeoutMs);
    const auto oldHistory = QSettings().value(QStringLiteral("jump/history"));
    const auto restore = qScopeGuard([&] {
      if (oldHistory.isValid())
        QSettings().setValue(QStringLiteral("jump/history"), oldHistory);
      else
        QSettings().remove(QStringLiteral("jump/history"));
    });
    const QString recent = hexAddress(Base + 0x40);
    QSettings().setValue(QStringLiteral("jump/history"), QStringList{recent});
    JumpDialog dialog(bench.session, bench.window.get());
    auto *input = dialog.findChild<QLineEdit *>();
    auto *matches = dialog.findChild<QListWidget *>();
    QVERIFY(input && matches);
    QCOMPARE(matches->count(), 1);
    QCOMPARE(matches->item(0)->text(), recent);
    input->setText(QStringLiteral("function_1"));
    QTRY_VERIFY_WITH_TIMEOUT(matches->count() > 1, OpenTimeoutMs);
    QVERIFY(matches->item(0)->text().startsWith(QStringLiteral("function_1")));
    matches->setCurrentRow(0);
    input->clear();
    QTRY_COMPARE_WITH_TIMEOUT(matches->count(), 1, OpenTimeoutMs);
    QCOMPARE(matches->item(0)->text(), recent);
    input->setText(QStringLiteral("function_2"));
    QVERIFY2(!matches->currentItem(),
             "The previous expression kept its jump target.");
  }

  void viewsFollowTheSessionAndNavigation() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentItem(),
                              std::optional<Address>(Base), OpenTimeoutMs);

    // The function window pages rows from the worker.
    auto *functions = bench.functions();
    QVERIFY(functions);
    QTRY_COMPARE_WITH_TIMEOUT(functions->model().total(), 600, OpenTimeoutMs);
    QTRY_COMPARE(functions->model().rowObject(0).value("name").toString(),
                 QStringLiteral("function_0"));
    QCOMPARE(functions->model().addressAt(0), std::optional<Address>(Base));
    functions->setFilterText(QStringLiteral("function_599"));
    QTRY_COMPARE(functions->model().total(), 1);
    QTRY_COMPARE(functions->model().rowObject(0).value("name").toString(),
                 QStringLiteral("function_599"));
    QCOMPARE(functions->model().addressAt(0),
             std::optional<Address>(Base + 0x2570));
    functions->setFilterText({});
    QTRY_COMPARE(functions->model().total(), 600);
    // Rows that arrive before the regions name their segments once the
    // regions do: only the Segment column repaints.
    QTRY_VERIFY(functions->model().rowCount() > 0);
    QSignalSpy repaint(&functions->model(), &QAbstractItemModel::dataChanged);
    functions->model().addressSpaceChanged();
    QCOMPARE(repaint.size(), 1);
    QCOMPARE(repaint.first().at(0).toModelIndex().column(), 1);
    QCOMPARE(repaint.first().at(1).toModelIndex().column(), 1);

    // Navigation records history and synchronizes the hex view.
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x140));
    QTRY_COMPARE(disassembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    auto *hex = bench.window->findChild<HexView *>();
    QVERIFY(hex);
    QTRY_COMPARE(hex->currentAddress(), std::optional<Address>(Base + 0x140));
    QVERIFY(disassembly->canGoBack());
    disassembly->goBack();
    QTRY_COMPARE(disassembly->currentItem(), std::optional<Address>(Base));
    QVERIFY(disassembly->canGoForward());
    disassembly->goForward();
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x140));

    // Pseudocode pages every line of the function.
    bench.action(ActionId::ViewPseudocode)->trigger();
    CodeView *pseudocode = nullptr;
    QTRY_VERIFY((pseudocode = bench.codeView(QStringLiteral("source"))) !=
                nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(
        pseudocode->text()->allText().contains(QStringLiteral("code line 699")),
        OpenTimeoutMs);
    QCOMPARE(pseudocode->text()->function(),
             std::optional<Address>(Base + 0x140));

    // An IR row the user selects takes the disassembly cursor along; Tab
    // moves the focus there too.
    bench.action(ActionId::ViewLowIR)->trigger();
    CodeView *ir = nullptr;
    QTRY_VERIFY((ir = bench.codeView(QStringLiteral("low"))) != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(ir->text()->lineCount() > 3, OpenTimeoutMs);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    ir->text()->setFocus();
    QTRY_VERIFY(ir->text()->hasFocus());
    ir->text()->setCursorLine(3);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x143));
    QVERIFY(ir->text()->hasFocus());
    bench.action(ActionId::JumpPseudocode)->trigger();
    QTRY_VERIFY(disassembly->isAncestorOf(QApplication::focusWidget()));
    QCOMPARE(disassembly->currentItem(), std::optional<Address>(Base + 0x143));

    // Copy takes the lines selected in the window holding the focus, also in
    // an IR window switched to pseudocode, which is not the pseudocode
    // window; it used to copy the disassembly.
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    ir->setRepresentation(QStringLiteral("source"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !ir->text()->loading() && ir->text()->lineCount() > 3, OpenTimeoutMs);
    // Two windows showing pseudocode are lettered apart.
    QCOMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
             QStringLiteral("Pseudocode-A"));
    QCOMPARE(bench.dockTitle(QStringLiteral("ir-a")),
             QStringLiteral("Pseudocode-B"));
    ir->text()->setFocus();
    QTRY_VERIFY(ir->text()->hasFocus());
    ir->text()->setCursorLine(1);
    QTest::keyClick(ir->text(), Qt::Key_Down, Qt::ShiftModifier);
    QTest::keyClick(ir->text(), Qt::Key_End, Qt::ShiftModifier);
    const QString lines = QStringLiteral("// code line 1\n// code line 2");
    clearClipboard();
    QTest::keyClick(ir->text(), Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(clipboardText(), lines);
    clearClipboard();
    bench.action(ActionId::EditCopy)->trigger();
    QCOMPARE(clipboardText(), lines);

    // C opens at the definition: the includes and declarations before it fold
    // into one line, which Keypad + expands and Keypad - folds again, and
    // which copies as the lines it stands for.
    pseudocode->setRepresentation(QStringLiteral("llvmc"));
    QCOMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
             QStringLiteral("LLVM C-A"));
    QCOMPARE(bench.dockTitle(QStringLiteral("ir-a")),
             QStringLiteral("Pseudocode-A"));
    auto *code = pseudocode->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() == 702,
                             OpenTimeoutMs);
    QVERIFY(code->allText().startsWith(QStringLiteral("#include <stdint.h>")));
    QCOMPARE(code->foldableCount(), 0);
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    code->setCursorLine(0);
    clearClipboard();
    QTest::keyClick(code, Qt::Key_C, Qt::ControlModifier);
    const QString declaration =
        QStringLiteral("typedef struct QDomNode QDomNode;");
    const QString linked = QStringLiteral(
        "extern int Bar_ctor() __asm__(\"_ZN3BarC1Ev\"); /* Bar::Bar() */");
    const QString globals =
        QStringLiteral("/* neverd.image: 0x20 */\nint64_t dso_handle = 0x20;\n"
                       "extern uint64_t qword_10; /* 0x10 */\n");
    QCOMPARE(clipboardText(), QStringLiteral("#include <stdint.h>\n") +
                                  declaration + QLatin1Char('\n') + linked +
                                  QLatin1Char('\n') + globals);
    QTest::keyClick(code, Qt::Key_Plus, Qt::KeypadModifier);
    QCOMPARE(code->lineCount(), 708);
    QTest::keyClick(code, Qt::Key_Minus, Qt::KeypadModifier);
    QCOMPARE(code->lineCount(), 702);
    QVERIFY(code->preludeFolded());

    // A type the prelude declares opens at its declaration, expanding the
    // prelude; C's own types are not declarations.
    QCOMPARE(code->declarationLine(QStringLiteral("QDomNode")),
             std::optional<int>(1));
    QVERIFY(!code->declarationLine(QStringLiteral("uint64_t")));
    QVERIFY(code->goToDeclaration(QStringLiteral("QDomNode")));
    QVERIFY(!code->preludeFolded());
    QCOMPARE(code->lineCount(), 708);
    // A global goes to its address, under whatever name C gives it there.
    QCOMPARE(code->objectAddress(QStringLiteral("dso_handle")),
             std::optional<Address>(0x20));
    QCOMPARE(code->objectAddress(QStringLiteral("qword_10")),
             std::optional<Address>(0x10));
    QVERIFY(!code->objectAddress(QStringLiteral("QDomNode")));
    QVERIFY(!code->objectAddress(QStringLiteral("Bar_ctor")));
    // A C++ function links by the mangled symbol its label names, which is
    // what navigation looks up.
    QCOMPARE(code->linkedSymbol(QStringLiteral("Bar_ctor")),
             std::optional<QString>(QStringLiteral("_ZN3BarC1Ev")));
    QVERIFY(!code->linkedSymbol(QStringLiteral("QDomNode")));
    QCOMPARE(code->currentToken(), QStringLiteral("QDomNode"));
    code->setPreludeFolded(true);
    QCOMPARE(code->lineCount(), 702);

    // A list copies its selected rows, by key or from the Edit menu.
    auto &model = functions->model();
    functions->table()->setFocus();
    QTRY_VERIFY(functions->table()->hasFocus());
    functions->table()->selectionModel()->select(
        QItemSelection(model.index(0, 0),
                       model.index(1, model.columnCount() - 1)),
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    clearClipboard();
    QTest::keyClick(functions->table(), Qt::Key_C, Qt::ControlModifier);
    const QString rows = clipboardText();
    QCOMPARE(rows.count(QLatin1Char('\n')), 1);
    QVERIFY2(rows.startsWith(QStringLiteral("function_0\t")), qPrintable(rows));
    QVERIFY2(rows.contains(QStringLiteral("\nfunction_1\t")), qPrintable(rows));
    clearClipboard();
    bench.action(ActionId::EditCopy)->trigger();
    QCOMPARE(clipboardText(), rows);

    // The graph of the current function.
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewToggleGraph)->trigger();
    QVERIFY(disassembly->graphMode());
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->graph()->nodes().size(),
                              qsizetype(2), OpenTimeoutMs);
    bench.action(ActionId::ViewToggleGraph)->trigger();
    QVERIFY(!disassembly->graphMode());

    // Unicode comments save atomically beside the binary.
    bench.session.setComment(Base + 0x140, QString::fromUtf8("中文 تعليق"));
    QTRY_VERIFY(bench.session.dirty());
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path + ".neverd/annotations.json"),
                             OpenTimeoutMs);
    QTRY_VERIFY(!bench.session.dirty());
    QVERIFY(QString::fromUtf8(readAll(path + ".neverd/annotations.json"))
                .contains(QString::fromUtf8("中文 تعليق")));

    // Renames reach the function window.
    bench.session.rename(Base + 0x140, QStringLiteral("renamed"));
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(20).value("name").toString(),
        QStringLiteral("renamed"), OpenTimeoutMs);
    QTRY_VERIFY(bench.session.canUndo());
    bench.session.undo();
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(20).value("name").toString(),
        QStringLiteral("function_20"), OpenTimeoutMs);

    // P starts a function inside another; the cursor stays on its
    // instruction under the new function's header.
    const Address loose = Base + 0x148;
    disassembly->navigate(loose);
    QTRY_COMPARE(disassembly->currentItem(), std::optional<Address>(loose));
    QTRY_VERIFY(bench.action(ActionId::EditCreateFunction)->isEnabled());
    bench.action(ActionId::EditCreateFunction)->trigger();
    const QString looseName = QStringLiteral("sub_FFFF800012340148");
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(21).value("name").toString(), looseName,
        OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentFunction(),
                              std::optional<Address>(loose), OpenTimeoutMs);
    QCOMPARE(disassembly->currentItem(), std::optional<Address>(loose));
    QTRY_VERIFY(bench.action(ActionId::EditDeleteFunction)->isEnabled());
    QVERIFY(!bench.action(ActionId::EditCreateFunction)->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path + ".neverd/functions.json"),
                             OpenTimeoutMs);
    // Deleting it gives the instruction back to the function around it, and
    // undo brings the new function back.
    bench.action(ActionId::EditDeleteFunction)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(21).value("name").toString(),
        QStringLiteral("function_21"), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentFunction(),
                              std::optional<Address>(Base + 0x140),
                              OpenTimeoutMs);
    bench.session.undo();
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(21).value("name").toString(), looseName,
        OpenTimeoutMs);

    // Alt+P opens IDA's Edit function dialog on the function the cursor is
    // in, with its name and bounds; what the user states there is an edit of
    // the history, kept beside the input.
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentFunction(),
                              std::optional<Address>(Base + 0x140),
                              OpenTimeoutMs);
    QTRY_VERIFY(bench.action(ActionId::EditFunction)->isEnabled());
    const auto statedRows = [&] {
      return QJsonDocument::fromJson(readAll(path + ".neverd/functions.json"))
          .array();
    };
    const auto statesNoReturn = [&] {
      for (const auto &row : statedRows())
        if (addressValue(row.toObject().value("addr")) == Base + 0x140)
          return row.toObject().value("noreturn").toBool(false);
      return false;
    };
    // Answers the dialog once it is active: \p check sees its fields, then
    // \p accept closes it with OK, else Cancel.
    const auto answerEditor =
        [&](std::function<void(FunctionEditDialog *)> check, bool accept) {
          auto *editor = new QTimer(&bench.session);
          connect(editor, &QTimer::timeout, editor, [editor, check, accept] {
            auto *dialog = qobject_cast<FunctionEditDialog *>(
                QApplication::activeModalWidget());
            if (!dialog || !dialog->isActiveWindow())
              return;
            editor->stop();
            editor->deleteLater();
            check(dialog);
            if (accept)
              QTest::mouseClick(
                  dialog->findChild<QPushButton *>(QStringLiteral("ok")),
                  Qt::LeftButton);
            else
              dialog->reject();
          });
          editor->start(10);
        };
    bool sawFunction = false;
    answerEditor(
        [&](FunctionEditDialog *dialog) {
          auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("name"));
          auto *start = dialog->findChild<QLineEdit *>(QStringLiteral("start"));
          auto *noReturn =
              dialog->findChild<QCheckBox *>(QStringLiteral("noreturn"));
          sawFunction =
              name && name->text() == QStringLiteral("function_20") && start &&
              start->text().endsWith(QStringLiteral("FFFF800012340140")) &&
              noReturn && !noReturn->isChecked() && noReturn->isEnabled();
          if (noReturn)
            noReturn->setChecked(true);
        },
        true);
    disassembly->setFocus();
    QTRY_VERIFY(disassembly->hasFocus());
    QTest::keyClick(disassembly, Qt::Key_P, Qt::AltModifier);
    QTRY_VERIFY_WITH_TIMEOUT(sawFunction, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(statesNoReturn(), OpenTimeoutMs);
    // Ctrl+E edits the function a row of the Functions window lists, as
    // IDA's list windows do, over the window's Ctrl+E (Jump to entry point).
    bool sawStated = false;
    answerEditor(
        [&](FunctionEditDialog *dialog) {
          auto *noReturn =
              dialog->findChild<QCheckBox *>(QStringLiteral("noreturn"));
          sawStated = noReturn && noReturn->isChecked();
        },
        false);
    // The closed dialog leaves no window active.
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    functions->table()->setFocus();
    QTRY_VERIFY(functions->table()->hasFocus());
    // The edit reloads the list; the row must be back before its key.
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(20).value("name").toString(),
        QStringLiteral("function_20"), OpenTimeoutMs);
    functions->table()->setCurrentIndex(functions->model().index(20, 0));
    QTest::keyClick(functions->table(), Qt::Key_E, Qt::ControlModifier);
    QTRY_VERIFY_WITH_TIMEOUT(sawStated, OpenTimeoutMs);
    QCOMPARE(disassembly->currentFunction(),
             std::optional<Address>(Base + 0x140));
    // Undo takes the statement back.
    QTRY_VERIFY(bench.session.canUndo());
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(!statesNoReturn(), OpenTimeoutMs);

    // D makes the data under the cursor a value, the next size each time
    // (stderr's qword gives way to a byte, then a word); U shows its bytes as
    // bytes, and undo takes U back.  Each commits at once.
    const Address object = Base + 0x3200;
    const auto items = [&] {
      return QString::fromUtf8(readAll(path + ".neverd/items.json"));
    };
    disassembly->navigate(object);
    disassembly->focusContent();
    QTRY_COMPARE(disassembly->currentItem(), std::optional<Address>(object));
    QTRY_VERIFY(bench.action(ActionId::EditDefineData)->isEnabled());
    bench.action(ActionId::EditDefineData)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(items().contains(QLatin1String("\"byte\"")),
                             OpenTimeoutMs);
    bench.action(ActionId::EditDefineData)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(items().contains(QLatin1String("\"word\"")),
                             OpenTimeoutMs);
    bench.action(ActionId::EditUndefine)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(items().contains(QLatin1String("\"undefined\"")),
                             OpenTimeoutMs);
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(items().contains(QLatin1String("\"word\"")),
                             OpenTimeoutMs);

    // H shows the number of the operand under the cursor, or else the
    // line's last number, in decimal at once; _ changes its sign and # goes
    // back to the listing's number.  Each commits at once.
    const Address instruction = Base + 0x51;
    const auto operandFormat = [&] {
      const auto rows =
          QJsonDocument::fromJson(readAll(path + ".neverd/operands.json"))
              .array();
      for (const auto &row : rows)
        for (const auto &entry : row.toObject().value("operands").toArray())
          return entry.toObject();
      return QJsonObject();
    };
    const auto shown = [&](const char *operands) {
      return disassembly->listing()->currentLineText().contains(
          QLatin1String(operands));
    };
    disassembly->navigate(instruction);
    disassembly->focusContent();
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(instruction));
    QTRY_VERIFY(shown("rsp, 20h"));
    QTRY_VERIFY(bench.action(ActionId::EditOperandDecimal)->isEnabled());
    bench.action(ActionId::EditOperandDecimal)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(operandFormat().value("base").toString(),
                              QStringLiteral("decimal"), OpenTimeoutMs);
    QCOMPARE(operandFormat().value("operand").toInt(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(shown("rsp, 32"), OpenTimeoutMs);
    bench.action(ActionId::EditOperandNegate)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(operandFormat().value("negate").toBool(),
                             OpenTimeoutMs);
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(!operandFormat().value("negate").toBool(),
                             OpenTimeoutMs);
    QCOMPARE(operandFormat().value("base").toString(),
             QStringLiteral("decimal"));
    bench.action(ActionId::EditOperandNumber)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(operandFormat().isEmpty(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(shown("rsp, 20h"), OpenTimeoutMs);

    // A restarted worker reopens the file where the user was.  The worker
    // writes the sidecar before it answers, and an edit in flight asks to be
    // saved first.
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.dirty(), OpenTimeoutMs);
    const auto place = disassembly->currentItem();
    QVERIFY(place && *place != bench.session.entryAddress());
    bench.session.restart();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentItem(), place, OpenTimeoutMs);
    QVERIFY2(bench.session.lastError().isEmpty(),
             qPrintable(bench.session.lastError()));
  }

  void editFunctionRenamesAndStatesInTwoSteps() {
    // IDA's Edit function, offered by the disassembly's and the Functions
    // window's context menus.  The frame sizes and the flags NeverD does not
    // keep are shown disabled; a new name and the flags are two steps of the
    // history, undone and redone in turn, and a name the worker refuses
    // applies neither.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    auto *functions = bench.functions();
    QTRY_COMPARE_WITH_TIMEOUT(functions->model().total(), 600, OpenTimeoutMs);
    const Address entry = Base + 0x140;
    const auto listedName = [&] {
      return functions->model().rowObject(20).value("name").toString();
    };
    QTRY_COMPARE_WITH_TIMEOUT(listedName(), QStringLiteral("function_20"),
                              OpenTimeoutMs);
    disassembly->navigate(entry);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentFunction(),
                              std::optional<Address>(entry), OpenTimeoutMs);
    QAction *edit = bench.action(ActionId::EditFunction);
    QTRY_VERIFY(edit->isEnabled());

    // The enabled entries of the context menu \p open shows, then closed.
    const auto offered = [&](const std::function<void()> &open) {
      QStringList texts;
      QTimer probe;
      connect(&probe, &QTimer::timeout, this, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu)
          return;
        probe.stop();
        for (const QAction *action : menu->actions())
          if (action->isEnabled() && !action->isSeparator())
            texts << action->text();
        menu->close();
      });
      probe.start(5);
      open();
      return texts;
    };
    QVERIFY(offered([&] {
              emit disassembly->contextMenuRequested(
                  disassembly->mapToGlobal(QPoint(40, 40)));
            }).contains(edit->text()));
    const QModelIndex row = functions->model().index(20, 0);
    functions->table()->scrollTo(row);
    QVERIFY(offered([&] {
              emit functions->table()->customContextMenuRequested(
                  functions->table()->visualRect(row).center());
            }).contains(edit->text()));

    const auto stated = [&] {
      for (const auto &row :
           QJsonDocument::fromJson(readAll(path + ".neverd/functions.json"))
               .array())
        if (addressValue(row.toObject().value("addr")) == entry)
          return row.toObject();
      return QJsonObject();
    };
    // Answers the dialog once it is active: \p check reads and sets its
    // fields, then OK closes it.
    const auto answerEditor =
        [&](std::function<void(FunctionEditDialog *)> check) {
          auto *editor = new QTimer(&bench.session);
          connect(editor, &QTimer::timeout, editor, [editor, check] {
            auto *dialog = qobject_cast<FunctionEditDialog *>(
                QApplication::activeModalWidget());
            if (!dialog || !dialog->isActiveWindow())
              return;
            editor->stop();
            editor->deleteLater();
            check(dialog);
            QTest::mouseClick(
                dialog->findChild<QPushButton *>(QStringLiteral("ok")),
                Qt::LeftButton);
          });
          editor->start(10);
        };
    const auto field = [](FunctionEditDialog *dialog, const char *name) {
      return dialog->findChild<QLineEdit *>(QLatin1String(name));
    };
    const auto flag = [](FunctionEditDialog *dialog, const char *name) {
      return dialog->findChild<QCheckBox *>(QLatin1String(name));
    };

    QStringList unmodeled;
    int shownOnly = 0;
    answerEditor([&](FunctionEditDialog *dialog) {
      for (const QCheckBox *box : dialog->findChildren<QCheckBox *>())
        if (!box->isEnabled())
          unmodeled << box->text();
      for (const QLineEdit *line : dialog->findChildren<QLineEdit *>())
        shownOnly += line->isReadOnly();
      field(dialog, "name")->setText(QStringLiteral("parse_header"));
      flag(dialog, "library")->setChecked(true);
      flag(dialog, "static")->setChecked(true);
    });
    edit->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(listedName(), QStringLiteral("parse_header"),
                              OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(stated().value("library").toBool() &&
                                 stated().value("static").toBool(),
                             OpenTimeoutMs);
    QCOMPARE(unmodeled, (QStringList{QStringLiteral("&Far function"),
                                     QStringLiteral("&BP based frame"),
                                     QStringLiteral("BP e&quals to SP"),
                                     QStringLiteral("Fuzzy SP"),
                                     QStringLiteral("Outlined code")}));
    // The bounds and the four frame sizes.
    QCOMPARE(shownOnly, 6);

    // Undo takes the flags back, then the name; redo makes them again.
    QTRY_VERIFY(bench.session.canUndo());
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(stated().isEmpty(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(listedName(), QStringLiteral("parse_header"),
                              OpenTimeoutMs);
    QTRY_VERIFY(bench.session.canUndo());
    bench.session.undo();
    QTRY_COMPARE_WITH_TIMEOUT(listedName(), QStringLiteral("function_20"),
                              OpenTimeoutMs);
    QTRY_VERIFY(bench.session.canRedo());
    bench.session.redo();
    QTRY_COMPARE_WITH_TIMEOUT(listedName(), QStringLiteral("parse_header"),
                              OpenTimeoutMs);
    QVERIFY(stated().isEmpty());
    QTRY_VERIFY(bench.session.canRedo());
    bench.session.redo();
    QTRY_VERIFY_WITH_TIMEOUT(stated().value("library").toBool(), OpenTimeoutMs);

    // A name the worker refuses, here one in an automatic form, is refused
    // with the flag.
    QSignalSpy messages(&bench.session, &Session::message);
    answerEditor([&](FunctionEditDialog *dialog) {
      field(dialog, "name")->setText(QStringLiteral("sub_10"));
      flag(dialog, "noreturn")->setChecked(true);
    });
    edit->trigger();
    const auto refused = [&] {
      for (const auto &arguments : messages)
        if (arguments.at(0).toString().contains(
                QLatin1String("is an automatic name")))
          return true;
      return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(refused(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(listedName(), QStringLiteral("parse_header"),
                              OpenTimeoutMs);
    QVERIFY(!stated().contains(QLatin1String("noreturn")));

    // Default takes a stated color away.
    bench.session.editFunction(entry, {{"color", "#20A0FF"}});
    QTRY_COMPARE_WITH_TIMEOUT(stated().value("color").toString(),
                              QStringLiteral("#20A0FF"), OpenTimeoutMs);
    QString shownColor;
    answerEditor([&](FunctionEditDialog *dialog) {
      auto *color = dialog->findChild<QPushButton *>(QStringLiteral("color"));
      shownColor = color->text();
      for (QAction *action : color->menu()->actions())
        if (action->text() == QStringLiteral("&Default"))
          action->trigger();
    });
    edit->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!stated().contains(QLatin1String("color")),
                             OpenTimeoutMs);
    QCOMPARE(shownColor, QStringLiteral("#20A0FF"));
    QVERIFY(stated().value("library").toBool());
  }

  void sessionChangesAreExplicit() {
    QTemporaryDir directory;
    const auto first = writeFixture(directory, QStringLiteral("first.bin"));
    const auto second = writeFixture(directory, QStringLiteral("second.bin"));
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(first);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    session.setComment(Base, QStringLiteral("first revision"));
    session.setComment(Base, QStringLiteral("second revision"));
    session.save();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(first + ".neverd/annotations.json"),
                             OpenTimeoutMs);
    QTRY_VERIFY(!session.dirty());
    QVERIFY(readAll(first + ".neverd/annotations.json")
                .contains("second revision"));

    // Opening another file with unsaved edits asks first.
    session.setComment(Base + 0x140, QStringLiteral("comment on A"));
    QTRY_VERIFY(session.dirty());
    QSignalSpy confirmation(&session, &Session::transitionRequested);
    session.open(second);
    QCOMPARE(confirmation.size(), 1);
    QCOMPARE(session.filePath(), first);
    session.resolveTransition(QStringLiteral("cancel"));
    QCOMPARE(session.filePath(), first);
    session.open(second);
    QCOMPARE(confirmation.size(), 2);
    session.resolveTransition(QStringLiteral("save"));
    QTRY_COMPARE_WITH_TIMEOUT(session.filePath(), second, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QVERIFY(!session.dirty());
    QVERIFY(
        readAll(first + ".neverd/annotations.json").contains("comment on A"));

    // An interrupted external read gets a terminal reply.
    QSignalSpy replies(&session, &Session::externalResponse);
    session.externalQuery(QStringLiteral("external-restart"),
                          QStringLiteral("metadata"), {}, {});
    session.restart();
    QTRY_COMPARE(replies.size(), 1);
    QCOMPARE(replies.first().at(0).toString(),
             QStringLiteral("external-restart"));
    const auto status = replies.first().at(1).toJsonObject();
    QVERIFY2(status.value("status").toString() == QLatin1String("ok") ||
                 status.value("error").toObject().value("code").toString() ==
                     QLatin1String("worker_stopped"),
             QJsonDocument(status).toJson().constData());
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);

    // Quitting with unsaved edits saves first when asked to.
    session.setComment(Base, QStringLiteral("keep before quit"));
    QTRY_VERIFY(session.dirty());
    QVERIFY(!session.requestQuit());
    QSignalSpy approved(&session, &Session::quitApproved);
    session.resolveTransition(QStringLiteral("save"));
    QTRY_COMPARE_WITH_TIMEOUT(approved.size(), 1, OpenTimeoutMs);
    QVERIFY(!session.dirty());
    QVERIFY(readAll(second + ".neverd/annotations.json")
                .contains("keep before quit"));
  }

  void editsForAnOlderSessionAreRefused() {
    QTemporaryDir directory;
    const auto first = writeFixture(directory, QStringLiteral("first.bin"));
    const auto second = writeFixture(directory, QStringLiteral("second.bin"));
    const auto third = writeFixture(directory, QStringLiteral("third.bin"));
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(first);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    const quint64 oldEpoch = session.epoch();
    QSignalSpy confirmation(&session, &Session::transitionRequested);
    session.open(second);
    session.open(third);
    QTRY_COMPARE_WITH_TIMEOUT(session.filePath(), third, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QCOMPARE(confirmation.size(), 0);
    QVERIFY(session.epoch() != oldEpoch);
    QSignalSpy messages(&session, &Session::message);
    session.setComment(Base, QStringLiteral("must not enter new project"),
                       oldEpoch);
    QVERIFY(!session.dirty());
    QVERIFY(!messages.isEmpty());
    QCOMPARE(messages.last().at(1).toInt(), 1);
  }

  void cancellingViewReadsStillCompletesExternalQueries() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("external.bin"));
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QObject view;
    bool viewReplied = false;
    session.read(QStringLiteral("decompile"),
                 {{"address", hexAddress(Base)}, {"representation", "low"}},
                 &view, [&](const QJsonObject &) { viewReplied = true; });
    QSignalSpy replies(&session, &Session::externalResponse);
    session.externalQuery(QStringLiteral("external-survives-cancel"),
                          QStringLiteral("metadata"), {}, {});
    session.cancelReads();
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, OpenTimeoutMs);
    QCOMPARE(replies.first().first().toString(),
             QStringLiteral("external-survives-cancel"));
    QCOMPARE(replies.first().at(1).toJsonObject().value("status").toString(),
             QStringLiteral("ok"));
    QTest::qWait(200);
    QVERIFY(!viewReplied);
    replies.clear();
    session.externalQuery(QStringLiteral("exact-stale"),
                          QStringLiteral("metadata"), {},
                          QStringLiteral("invalid-revision"));
    QTRY_COMPARE(replies.size(), 1);
    QCOMPARE(replies.first()
                 .at(1)
                 .toJsonObject()
                 .value("error")
                 .toObject()
                 .value("code")
                 .toString(),
             QStringLiteral("stale_revision"));
  }

  void cancellingViewReadsPreservesAnExternalAnalysisSnapshot() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    QStringList diagnostics;
    connect(&session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    session.open(
        writeFixture(directory, QStringLiteral("pseudocode-parallel.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QObject view;
    // Hold the project dispatcher busy so the replica's snapshot remains
    // queued when Cancel is pressed, rather than relying on a timing race.
    session.queries().subscribe(
        {"decompile",
         {{"address", hexAddress(Base)}, {"representation", "source"}}},
        &view, [](const QJsonObject &) {});
    QTRY_VERIFY_WITH_TIMEOUT(
        diagnostics.join('\n').contains("fixture parallel decompile " +
                                        hexAddress(Base) + " started"),
        OpenTimeoutMs);
    QSignalSpy replies(&session, &Session::externalResponse);
    session.externalQuery(
        "external-analysis", "decompile",
        {{"address", hexAddress(Base + 32)}, {"representation", "source"}}, {});
    QTRY_COMPARE_WITH_TIMEOUT(session.queries().pendingReadCount(), 2, 1000);
    session.cancelReads();
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, OpenTimeoutMs);
    QCOMPARE(replies.front()[0].toString(),
             QStringLiteral("external-analysis"));
    QCOMPARE(replies.front()[1].toJsonObject().value("status").toString(),
             QStringLiteral("ok"));
  }

  void saveBeforeQuitRejectsALateEdit() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("transition.bin"));
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    session.setComment(Base, QStringLiteral("accepted before quit"));
    QTRY_VERIFY(session.dirty());
    QVERIFY(!session.requestQuit());
    QSignalSpy approved(&session, &Session::quitApproved);
    session.resolveTransition(QStringLiteral("save"));
    session.setComment(Base + 0x10,
                       QStringLiteral("late edit after save intent"));
    QTRY_COMPARE_WITH_TIMEOUT(approved.size(), 1, OpenTimeoutMs);
    QVERIFY(!session.dirty());
    const auto bytes = readAll(path + ".neverd/annotations.json");
    QVERIFY(bytes.contains("accepted before quit"));
    QVERIFY(!bytes.contains("late edit after save intent"));
  }

  void cDefinesInstructionsAndDatabaseKeepsThem_data() {
    QTest::addColumn<bool>("native");
    QTest::newRow("fixture") << false;
    if (!qEnvironmentVariable("NEVERD_CODE_NAV_WORKER").isEmpty() &&
        !qEnvironmentVariable("NEVERD_MAKE_CODE_FILE").isEmpty())
      QTest::newRow("native") << true;
  }

  void cDefinesInstructionsAndDatabaseKeepsThem() {
    QFETCH(bool, native);
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("make-code.bin"));
    const auto worker = native ? qEnvironmentVariable("NEVERD_CODE_NAV_WORKER")
                               : QString::fromLocal8Bit(TEST_WORKER);
    if (native)
      QVERIFY(QFile::copy(qEnvironmentVariable("NEVERD_MAKE_CODE_FILE"), path));
    else
      writeFixture(directory, QStringLiteral("make-code.bin"));
    const auto database = ProjectDatabase::pathFor(path);
    const Address entry = native ? Address(0x400078) : Base;
    const Address start = native ? entry + 6 : Base + 0x2700;
    {
      Workbench bench(worker);
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      bench.window->activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
      auto *view = bench.window->disassembly();
      view->navigate(start);
      view->focusContent();
      QTRY_COMPARE(view->currentItem(), std::optional<Address>(start));
      QTRY_VERIFY(view->listing()->hasFocus());
      QTRY_VERIFY(bench.action(ActionId::EditDefineCode)->isEnabled());
      QCOMPARE(bench.action(ActionId::EditDefineCode)->shortcut(),
               QKeySequence("C"));
      QTest::keyClick(view->listing(), Qt::Key_C);
      const auto rows = [&] {
        return QJsonDocument::fromJson(readAll(path + ".neverd/items.json"))
            .array();
      };
      QTRY_COMPARE_WITH_TIMEOUT(rows().size(), 3, OpenTimeoutMs);
      QTRY_VERIFY(view->listing()->currentLineText().contains("nop"));
      QCOMPARE(view->currentItem(), std::optional<Address>(start));
      QVERIFY(!view->currentFunction());
      QTRY_VERIFY(bench.session.canUndo());
      bench.session.undo();
      QTRY_VERIFY_WITH_TIMEOUT(rows().isEmpty(), OpenTimeoutMs);
      QTRY_VERIFY(view->listing()->currentLineText().contains("db"));
      QTRY_VERIFY(bench.session.canRedo());
      bench.session.redo();
      QTRY_COMPARE_WITH_TIMEOUT(rows().size(), 3, OpenTimeoutMs);
      view->navigate(start + 1);
      QTRY_COMPARE(view->currentItem(), std::optional<Address>(start + 1));
      QTRY_VERIFY(view->listing()->currentLineText().contains("add"));
      if (const auto capture =
              qEnvironmentVariable("NEVERD_CODE_NAV_CAPTURE_DIR");
          !capture.isEmpty()) {
        QDir().mkpath(capture);
        QVERIFY(bench.window->grab().save(
            capture +
            (native ? "/make-code-native.png" : "/make-code-fixture.png")));
      }
      QSignalSpy saved(&bench.session, &Session::databaseSaved);
      bench.session.packDatabase(database);
      QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, OpenTimeoutMs);
      QVERIFY(saved.first().first().toBool());
      // C is not consumed as a data definition in the source window.
      view->navigate(entry);
      QTRY_COMPARE(view->currentItem(), std::optional<Address>(entry));
      QTRY_COMPARE_WITH_TIMEOUT(view->currentFunction(),
                                std::optional<Address>(entry), OpenTimeoutMs);
      QTRY_VERIFY(bench.action(ActionId::ViewPseudocode)->isEnabled());
      bench.action(ActionId::ViewPseudocode)->trigger();
      auto *source = bench.codeView(QStringLiteral("source"));
      QVERIFY(source);
      QTRY_VERIFY(source->isVisible() && !source->text()->loading());
      source->text()->window()->activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(source->text()->window()));
      source->text()->setFocus();
      QTRY_VERIFY(source->text()->hasFocus());
      QTRY_VERIFY(!bench.action(ActionId::EditDefineCode)->isEnabled());
    }
    {
      Workbench bench(worker);
      bench.window->openFile(database);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      auto *view = bench.window->disassembly();
      view->navigate(start + 1);
      QTRY_COMPARE(view->currentItem(), std::optional<Address>(start + 1));
      QTRY_VERIFY(view->listing()->currentLineText().contains("add"));
      QVERIFY(!view->currentFunction());
    }
  }

  /// The rows of the bookmarks window, which Jump to mark opens.
  static QStringList bookmarkTexts(Workbench &bench) {
    bench.action(ActionId::JumpMark)->trigger();
    for (auto *view : bench.window->findChildren<ChooserView *>())
      if (view->model().kind() == ChooserKind::Bookmarks) {
        QStringList texts;
        for (int row = 0; row < view->model().rowCount(); ++row)
          texts << view->model()
                       .data(view->model().index(row, 1), Qt::DisplayRole)
                       .toString();
        return texts;
      }
    return {};
  }

  /// Mark the current position as \p text, as Mark position does.
  bool markPosition(Workbench &bench, const QString &text) {
    QTimer answer;
    bool accepted = false;
    connect(&answer, &QTimer::timeout, &answer, [&] {
      if (auto *dialog =
              qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
        dialog->setTextValue(text);
        accepted = true;
        dialog->accept();
      }
    });
    answer.start(5);
    if (!QTest::qWaitFor(
            [&] { return bench.action(ActionId::EditBookmark)->isEnabled(); },
            OpenTimeoutMs))
      return false;
    bench.action(ActionId::EditBookmark)->trigger();
    return QTest::qWaitFor([&] { return accepted; }, OpenTimeoutMs);
  }

  QString commentAt(Workbench &bench, Address address) {
    QString comment;
    bool read = false;
    bench.session.read(QStringLiteral("resolve"),
                       {{"query", hexAddress(address)}}, &bench.session,
                       [&](const QJsonObject &payload) {
                         comment = payload.value("comment").toString();
                         read = true;
                       });
    if (!QTest::qWaitFor([&] { return read; }, OpenTimeoutMs))
      return QStringLiteral("(no reply)");
    return comment;
  }

  void projectFilesTravelWithTheBinary() {
    // Saving keeps a binary's project in JSON files beside it and writes no
    // database: a copy of the binary with the shared files, as a git clone
    // has, opens the same analysis.  Where the user was is the user's own,
    // in a file a repository ignores.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("shared.bin"));
    {
      Workbench bench;
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      auto *view = bench.window->disassembly();
      view->navigate(Base + 0x140);
      QTRY_COMPARE(view->currentItem(), std::optional<Address>(Base + 0x140));
      QVERIFY(markPosition(bench, QString::fromUtf8("共享标记")));
      QCOMPARE(bookmarkTexts(bench),
               QStringList{QString::fromUtf8("共享标记")});
      // The file the command line's bookmarks command keeps too.
      const auto bookmarks = readAll(path + ".neverd/bookmarks.json");
      QVERIFY2(bookmarks.startsWith("[\n  {\n    \"addr\": \"" +
                                    hexAddress(Base + 0x140).toUtf8() +
                                    "\",\n    \"name\": \""),
               bookmarks.constData());
      QVERIFY(bookmarks.contains(QString::fromUtf8("共享标记").toUtf8()));
      QVERIFY(!bench.action(ActionId::FileSave)->isEnabled());
      bench.session.setComment(Base + 0x140, QStringLiteral("shared comment"));
      QTRY_VERIFY(bench.session.dirty());
      QVERIFY(bench.action(ActionId::FileSave)->isEnabled());
      bench.session.save();
      QTRY_VERIFY_WITH_TIMEOUT(!bench.session.dirty(), OpenTimeoutMs);
      QVERIFY(!bench.action(ActionId::FileSave)->isEnabled());
      QVERIFY(readAll(path + ".neverd/annotations.json")
                  .startsWith("[\n  {\n    \"addr\": "));
      QVERIFY(bench.window->close());
    }
    QVERIFY(QDir(directory.path())
                .entryList({QStringLiteral("*.nddb")}, QDir::Files)
                .isEmpty());
    const auto workbench =
        QJsonDocument::fromJson(readAll(path + ".neverd/local/workbench.json"))
            .object();
    QCOMPARE(workbench.value("location").toObject().value("address").toString(),
             hexAddress(Base + 0x140));

    // The project is one folder beside the binary.  It keeps each user's
    // own files in local/, which its own .gitignore leaves out, and asks
    // git to merge the rest row by row.
    const QDir project(path + ".neverd");
    QVERIFY(readAll(project.filePath(QStringLiteral(".gitignore")))
                .contains("/local/"));
    QVERIFY(readAll(project.filePath(QStringLiteral(".gitattributes")))
                .contains("*.json merge=neverd"));
    QVERIFY(QFileInfo(project.filePath(QStringLiteral("local"))).isDir());
    QVERIFY(QDir(directory.path())
                .entryList({QStringLiteral("shared.bin.neverd-*")}, QDir::Files)
                .isEmpty());

    // The clone: the binary and the project folder as git carries it,
    // without local/.
    QTemporaryDir clone;
    const auto copy = clone.filePath(QStringLiteral("shared.bin"));
    QVERIFY(QFile::copy(path, copy));
    QVERIFY(QDir().mkpath(copy + ".neverd"));
    for (const auto &name :
         project.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot))
      QVERIFY(QFile::copy(project.filePath(name),
                          QDir(copy + ".neverd").filePath(name)));
    {
      Workbench bench;
      bench.window->openFile(copy);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      QCOMPARE(commentAt(bench, Base + 0x140),
               QStringLiteral("shared comment"));
      QCOMPARE(bookmarkTexts(bench),
               QStringList{QString::fromUtf8("共享标记")});
      QVERIFY(!bench.session.dirty());
      QVERIFY(bench.window->close());
    }
    // With the user's own workbench file, the place comes back too.
    QFile::remove(copy + ".neverd/local/workbench.json");
    QVERIFY(QFile::copy(path + ".neverd/local/workbench.json",
                        copy + ".neverd/local/workbench.json"));
    Workbench bench;
    bench.window->openFile(copy);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(bench.window->disassembly()->currentItem(),
                              std::optional<Address>(Base + 0x140),
                              OpenTimeoutMs);
  }

  void anOpenedDatabaseSavesBackAndAsksBeforeLosingEdits() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("packed.bin"));
    const auto database = directory.filePath(QStringLiteral("team.nddb"));
    {
      Workbench bench;
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      QVERIFY(bench.action(ActionId::FilePackDatabase)->isEnabled());
      QSignalSpy saved(&bench.session, &Session::databaseSaved);
      bench.session.packDatabase(database);
      QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, OpenTimeoutMs);
      QVERIFY(saved.first().first().toBool());
    }
    const auto databaseRenames = [&] {
      QString error;
      const auto contents = ProjectDatabase::read(database, &error);
      return contents ? contents->sidecars.value(
                            QStringLiteral(".neverd-renames.json"))
                      : QByteArray();
    };
    Workbench bench;
    bench.window->openFile(database);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.session.openedFromDatabase());
    // Opening it changes nothing that needs saving.
    QVERIFY(!bench.session.dirty());
    QVERIFY(!bench.action(ActionId::FileSave)->isEnabled());
    // An edit is kept beside the unpacked copy at once, and the database
    // lacks it until Save writes it back.
    bench.session.rename(Base + 0x140, QStringLiteral("first_name"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.databaseOutOfDate(), OpenTimeoutMs);
    QVERIFY(bench.session.dirty());
    QVERIFY(bench.action(ActionId::FileSave)->isEnabled());
    QVERIFY(!databaseRenames().contains("first_name"));
    QSignalSpy saved(&bench.session, &Session::databaseSaved);
    bench.session.save();
    QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, OpenTimeoutMs);
    QVERIFY(saved.first().first().toBool());
    QTRY_VERIFY(!bench.session.dirty());
    QVERIFY(databaseRenames().contains("first_name"));
    QVERIFY(!QFileInfo::exists(ProjectDatabase::pathFor(path)));

    // Quitting asks before such an edit is lost; discarding it leaves the
    // database as it was.
    bench.session.rename(Base + 0x140, QStringLiteral("second_name"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.databaseOutOfDate(), OpenTimeoutMs);
    QString question;
    QTimer answer;
    connect(&answer, &QTimer::timeout, &answer, [&] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        question = box->text();
        box->button(QMessageBox::Discard)->click();
      }
    });
    answer.start(5);
    QSignalSpy approved(&bench.session, &Session::quitApproved);
    QVERIFY(!bench.session.requestQuit());
    QTRY_COMPARE_WITH_TIMEOUT(approved.size(), 1, OpenTimeoutMs);
    answer.stop();
    QVERIFY2(question.contains(QStringLiteral("team.nddb")),
             qPrintable(question));
    QVERIFY(databaseRenames().contains("first_name"));
    QVERIFY(!databaseRenames().contains("second_name"));
  }

  void projectFilesChangedOnDiskAreTaken() {
    // git pull, another tool or an AI agent changes the project's files
    // while the project is open: the workbench takes them as they are, and
    // a comment not saved yet stays on top of them.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("pulled.bin"));
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    // An edit of our own leaves the undo history that a pull overtakes.
    bench.session.rename(Base + 0x140, QStringLiteral("ours"));
    QTRY_VERIFY_WITH_TIMEOUT(
        readAll(path + ".neverd/renames.json").contains("\"ours\""),
        OpenTimeoutMs);
    bench.session.setComment(Base, QStringLiteral("staged here"));
    QTRY_VERIFY(bench.session.dirty());

    // A teammate's rename and comment arrive, and a bookmark.
    const auto replace = [](const QString &file, const QByteArray &text) {
      QSaveFile out(file);
      return out.open(QIODevice::WriteOnly) && out.write(text) == text.size() &&
             out.commit();
    };
    const auto rows = [](const QJsonObject &row) {
      return QJsonDocument(QJsonArray{row}).toJson();
    };
    const QString at = hexAddress(Base + 0x140);
    QVERIFY(replace(path + ".neverd/renames.json",
                    rows({{"addr", at},
                          {"original", "function_20"},
                          {"renamed", "theirs"}})));
    QVERIFY(replace(path + ".neverd/annotations.json",
                    rows({{"addr", at}, {"text", "their note"}})));
    QVERIFY(
        replace(path + ".neverd/bookmarks.json",
                rows({{"addr", at}, {"name", "their mark"}, {"note", ""}})));
    const auto resolved = [&](Address address) {
      QJsonObject result;
      bool read = false;
      bench.session.read(QStringLiteral("resolve"),
                         {{"query", hexAddress(address)}}, &bench.session,
                         [&](const QJsonObject &payload) {
                           result = payload;
                           read = true;
                         });
      QTest::qWaitFor([&] { return read; }, OpenTimeoutMs);
      return result;
    };
    QTRY_COMPARE_WITH_TIMEOUT(
        resolved(Base + 0x140).value("comment").toString(),
        QStringLiteral("their note"), OpenTimeoutMs);
    QCOMPARE(resolved(Base).value("comment").toString(),
             QStringLiteral("staged here"));
    QVERIFY(bench.session.dirty());
    QTRY_COMPARE(bookmarkTexts(bench),
                 QStringList{QStringLiteral("their mark")});
    auto *log = bench.window->findChild<OutputWindow *>()
                    ->findChild<QPlainTextEdit *>();
    QVERIFY(log);
    QTRY_VERIFY(log->toPlainText().contains(
        QStringLiteral("changed on disk and were loaded")));

    // Edits go on from the files as they are now, and saving keeps both.
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.dirty(), OpenTimeoutMs);
    const auto notes = readAll(path + ".neverd/annotations.json");
    QVERIFY2(notes.contains("their note") && notes.contains("staged here"),
             notes.constData());
    bench.session.rename(Base + 0x140, QStringLiteral("after_pull"));
    QTRY_VERIFY_WITH_TIMEOUT(
        readAll(path + ".neverd/renames.json").contains("after_pull"),
        OpenTimeoutMs);
    QVERIFY2(bench.session.lastError().isEmpty(),
             qPrintable(bench.session.lastError()));
  }

  void filesBesideTheBinaryMoveIntoItsProjectFolder() {
    // An older NeverD kept the project as files beside the binary: opening
    // it moves them into the binary's project folder, and they read as
    // before.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("older.bin"));
    const auto rows = [](const QJsonObject &row) {
      return QJsonDocument(QJsonArray{row}).toJson();
    };
    const QString at = hexAddress(Base + 0x140);
    const auto write = [](const QString &file, const QByteArray &text) {
      QFile out(file);
      return out.open(QIODevice::WriteOnly) && out.write(text) == text.size();
    };
    QVERIFY(write(path + ".neverd-annotations.json",
                  rows({{"addr", at}, {"text", "older note"}})));
    QVERIFY(write(path + ".neverd-bookmarks.json",
                  rows({{"addr", at}, {"name", "older mark"}, {"note", ""}})));
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!QFileInfo::exists(path + ".neverd-annotations.json"));
    QVERIFY(!QFileInfo::exists(path + ".neverd-bookmarks.json"));
    QVERIFY(readAll(path + ".neverd/annotations.json").contains("older note"));
    QCOMPARE(commentAt(bench, Base + 0x140), QStringLiteral("older note"));
    QCOMPARE(bookmarkTexts(bench), QStringList{QStringLiteral("older mark")});
  }

  void bookmarksFromBeforeProjectsKeptThemStillShow() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("legacy.bin"));
    // A database from before keeps its bookmarks and debug file choices in
    // its state alone.
    const auto database = directory.filePath(QStringLiteral("legacy.nddb"));
    QCOMPARE(ProjectDatabase::save(
                 database, path,
                 {{QStringLiteral("bookmarks"),
                   QJsonDocument(QJsonArray{QJsonObject{
                                     {"address", hexAddress(Base + 0x140)},
                                     {"text", "from the database"}}})
                       .toJson(QJsonDocument::Compact)},
                  {QStringLiteral("dwarf-input.json"), R"({"mode":"none"})"}}),
             QString());
    {
      Workbench bench;
      bench.window->openFile(database);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      QCOMPARE(bookmarkTexts(bench),
               QStringList{QStringLiteral("from the database")});
      // Its choices now have a file beside the unpacked copy too, which the
      // database lacks without anything to save: opening changed nothing.
      QCOMPARE(QJsonDocument::fromJson(
                   readAll(bench.session.filePath() + ".neverd/debug.json"))
                   .object()
                   .value("dwarf")
                   .toObject()
                   .value("mode")
                   .toString(),
               QStringLiteral("none"));
      QVERIFY(!bench.session.dirty());
    }
    // This machine's settings kept a binary's bookmarks.
    QSettings().setValue(
        QStringLiteral("bookmarks/") +
            QString::fromLatin1(QCryptographicHash::hash(
                                    path.toUtf8(), QCryptographicHash::Sha1)
                                    .toHex()),
        QVariantList{QVariantMap{{"address", hexAddress(Base)},
                                 {"text", "from the settings"}}});
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(bookmarkTexts(bench),
             QStringList{QStringLiteral("from the settings")});
    QVERIFY(!QFileInfo::exists(path + ".neverd/bookmarks.json"));
    // The next mark keeps them all with the project.
    auto *view = bench.window->disassembly();
    view->navigate(Base + 0x140);
    view->focusContent();
    QTRY_COMPARE(view->currentItem(), std::optional<Address>(Base + 0x140));
    QVERIFY(markPosition(bench, QStringLiteral("made now")));
    const auto rows =
        QJsonDocument::fromJson(readAll(path + ".neverd/bookmarks.json"))
            .array();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).toObject().value("name").toString(),
             QStringLiteral("from the settings"));
    QCOMPARE(rows.at(1).toObject().value("name").toString(),
             QStringLiteral("made now"));
    // Marking a marked address again renames its bookmark.
    QVERIFY(markPosition(bench, QStringLiteral("renamed mark")));
    QCOMPARE(bookmarkTexts(bench),
             (QStringList{QStringLiteral("from the settings"),
                          QStringLiteral("renamed mark")}));
  }

  void generatedViewsStayClosedAfterReopening_data() {
    QTest::addColumn<QString>("representation");
    QTest::addColumn<int>("action");
    QTest::addColumn<bool>("floating");
    for (const auto &[name, command] :
         {std::pair{"source", ActionId::ViewPseudocode},
          std::pair{"llvmc", ActionId::ViewLLVMC},
          std::pair{"low", ActionId::ViewLowIR},
          std::pair{"med", ActionId::ViewMedIR},
          std::pair{"high", ActionId::ViewHighIR},
          std::pair{"llvm", ActionId::ViewLLVMIR}})
      for (const bool floating : {false, true})
        QTest::newRow((QByteArray(name) + (floating ? "-floating" : "-docked"))
                          .constData())
            << QString::fromLatin1(name) << int(command) << floating;
  }

  void generatedViewsStayClosedAfterReopening() {
    QFETCH(QString, representation);
    QFETCH(int, action);
    QFETCH(bool, floating);
    QTemporaryDir directory;
    LoadDialogAcceptor loads;
    const auto path = writeFixture(directory, "restore-generated.bin");
    const auto database = directory.filePath("generated.nddb");
    const auto layout = directory.filePath("desktop.json");
    const auto command = ActionId(action);
    const Address function = Base + 0x140;
    const auto viewsClosed = [] {
      // Floating docks need not be QObject children of the main window.
      int count = 0;
      for (auto *widget : QApplication::allWidgets())
        if (auto *view = qobject_cast<CodeView *>(widget)) {
          ++count;
          if (view->isVisible() || view->text()->function() ||
              view->text()->loading())
            return false;
        }
      return count == 3;
    };
    {
      Workbench bench;
      bench.window->setLayoutPath(layout);
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      bench.window->disassembly()->navigate(function);
      QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                   std::optional<Address>(function));
      QTRY_VERIFY(ListingViewTestAccess::settled(
          *bench.window->disassembly()->listing()));
      bench.action(command)->trigger();
      auto *view = bench.codeView(representation);
      QVERIFY(view);
      QTRY_VERIFY(!view->text()->loading() &&
                  !view->text()->allText().isEmpty());
      auto *dock = bench.dockFor(view);
      QVERIFY(dock);
      if (floating)
        dock->setFloating(true);
      QTRY_VERIFY(view->isVisible());
      // Save more than the old fixed pair, including independent documents
      // with the same representation. The factory must restore all their IDs.
      for (int i = 0; i < 2; ++i) {
        auto *assembly = bench.window->disassembly();
        QVERIFY(activateTestWindow(assembly->window()));
        assembly->focusContent();
        bench.action(command)->trigger();
      }
      QSignalSpy saved(&bench.session, &Session::databaseSaved);
      bench.session.packDatabase(database);
      QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, OpenTimeoutMs);
      QVERIFY(saved.first().first().toBool());
      QVERIFY(bench.window->close());
    }
    QVERIFY(QFileInfo::exists(layout));
    {
      // Restart without a file first, then restore the binary's own desktop.
      Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
      bench.window->setLayoutPath(layout);
      bench.window->initializeLayout();
      bench.window->show();
      QTRY_VERIFY(viewsClosed());
      bench.action(ActionId::WindowsLoadDesktop)->trigger();
      QTRY_VERIFY(viewsClosed());
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      QTRY_VERIFY(viewsClosed());
      QTRY_VERIFY(ListingViewTestAccess::settled(
          *bench.window->disassembly()->listing()));
      bench.action(command)->trigger();
      auto *view = bench.codeView(representation);
      QVERIFY(view);
      QTRY_VERIFY(view->isVisible() && !view->text()->loading() &&
                  !view->text()->allText().isEmpty());
      QCOMPARE(view->text()->function(), std::optional<Address>(function));
      const auto generated = view->text()->allText();
      bench.action(ActionId::WindowsLoadDesktop)->trigger();
      QTRY_VERIFY(view->isVisible());
      QCOMPARE(view->text()->allText(), generated);
      // Switching projects also closes the old generated view.
      const auto other = writeFixture(directory, "next-generated.bin");
      bench.window->openFile(other);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded() &&
                                   bench.session.filePath() == other,
                               OpenTimeoutMs);
      QTRY_VERIFY(viewsClosed());
    }
    {
      // A packed database can restore a different desktop after startup.
      Workbench bench;
      bench.window->openFile(database);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      QTRY_VERIFY(viewsClosed());
      QTRY_VERIFY(ListingViewTestAccess::settled(
          *bench.window->disassembly()->listing()));
      bench.action(command)->trigger();
      auto *view = bench.codeView(representation);
      QVERIFY(view);
      QTRY_VERIFY(view->isVisible() && !view->text()->loading() &&
                  !view->text()->allText().isEmpty());
      QCOMPARE(view->text()->function(), std::optional<Address>(function));
      bench.session.closeFile();
      QTRY_VERIFY(!bench.session.loaded());
      QTRY_VERIFY(viewsClosed());
    }
  }

  void searchResultsStayClosedAfterReopening_data() {
    QTest::addColumn<bool>("floating");
    QTest::newRow("docked") << false;
    QTest::newRow("floating") << true;
  }

  void searchResultsStayClosedAfterReopening() {
    QFETCH(bool, floating);
    QTemporaryDir directory;
    LoadDialogAcceptor loads;
    const auto path = writeFixture(directory, "restore-search.bin");
    const auto layout = directory.filePath("desktop.json");
    const auto results = []() -> ChooserView * {
      for (auto *widget : QApplication::allWidgets())
        if (auto *view = qobject_cast<ChooserView *>(widget);
            view && view->model().kind() == ChooserKind::SearchResults)
          return view;
      return nullptr;
    };
    const auto search = [&](Workbench &bench) {
      QTimer input;
      connect(&input, &QTimer::timeout, &input, [] {
        if (auto *dialog = qobject_cast<QInputDialog *>(
                QApplication::activeModalWidget())) {
          dialog->setTextValue(QStringLiteral("string 7"));
          dialog->accept();
        }
      });
      input.start(10);
      bench.action(ActionId::SearchText)->trigger();
      QTRY_VERIFY(results() && results()->model().total() > 0);
      QTRY_VERIFY(results()->isVisible());
    };
    {
      Workbench bench;
      bench.window->setLayoutPath(layout);
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      search(bench);
      QVERIFY(results());
      auto *dock = bench.dockFor(results());
      QVERIFY(dock);
      if (floating)
        dock->setFloating(true);
      bench.action(ActionId::WindowsSaveDesktop)->trigger();
      const int count = results()->model().total();
      bench.action(ActionId::WindowsLoadDesktop)->trigger();
      QTRY_VERIFY(results()->isVisible());
      QCOMPARE(results()->model().total(), count);
      QVERIFY(bench.window->close());
    }
    {
      Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
      bench.window->setLayoutPath(layout);
      bench.window->initializeLayout();
      bench.window->show();
      QVERIFY(results());
      QTRY_VERIFY(!results()->isVisible());
      bench.action(ActionId::WindowsLoadDesktop)->trigger();
      QTRY_VERIFY(!results()->isVisible());
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      QTRY_VERIFY(!results()->isVisible());
      QCOMPARE(results()->model().total(), 0);
      search(bench);
      bench.session.closeFile();
      QTRY_VERIFY(!bench.session.loaded());
      QTRY_VERIFY(!results()->isVisible());
      QCOMPARE(results()->model().total(), 0);
    }
  }

  void databaseCarriesTheProject() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("project.bin"));
    const auto database = ProjectDatabase::pathFor(path);
    {
      Workbench bench;
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      bench.window->disassembly()->navigate(Base + 0x140);
      QTRY_COMPARE(bench.window->disassembly()->currentItem(),
                   std::optional<Address>(Base + 0x140));
      bench.action(ActionId::ViewPseudocode)->trigger();
      auto *view = bench.codeView(QStringLiteral("source"));
      QVERIFY(view);
      QTRY_VERIFY(view->isVisible() && !view->text()->loading());
      bench.session.setComment(Base + 0x140, QStringLiteral("packed comment"));
      QTRY_VERIFY(bench.session.dirty());
      // Packing commits the staged comment first.
      QSignalSpy saved(&bench.session, &Session::databaseSaved);
      bench.session.packDatabase(database);
      QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, OpenTimeoutMs);
      QVERIFY(saved.first().first().toBool());
      QVERIFY(QFileInfo::exists(database));
      QTRY_VERIFY(!bench.session.dirty());
    }
    // The database alone reopens the project: input, comment and location.
    // Its header identifies it, so a copy without the suffix opens too.
    for (const auto &name :
         {QStringLiteral("moved.nddb"), QStringLiteral("moved.db")}) {
      QTemporaryDir moved;
      const auto copy = moved.filePath(name);
      QVERIFY(QFile::copy(database, copy));
      Workbench bench;
      QVERIFY(dropFile(bench.window.get(), copy));
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      QCOMPARE(bench.session.projectPath(), copy);
      QCOMPARE(bench.session.databasePath(), copy);
      QVERIFY(bench.session.filePath() != path);
      QTRY_COMPARE_WITH_TIMEOUT(bench.window->disassembly()->currentItem(),
                                std::optional<Address>(Base + 0x140),
                                OpenTimeoutMs);
      QTest::qWait(250);
      if (auto *view = bench.codeView(QStringLiteral("source"))) {
        QVERIFY(!view->isVisible());
        QVERIFY(!view->text()->function());
      }
      QString comment;
      bench.session.read(QStringLiteral("resolve"),
                         {{"query", hexAddress(Base + 0x140)}}, &bench.session,
                         [&](const QJsonObject &payload) {
                           comment = payload.value("comment").toString();
                         });
      QTRY_COMPARE(comment, QStringLiteral("packed comment"));
    }
  }

  void contributionsRegisterRunAndUnload() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("contrib.bin"));
    const auto manifest = directory.filePath(QStringLiteral("manifest.json"));
    {
      QFile file(manifest);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(
          R"({"schema_version": 1, "namespace": "sample", "version": "1.0",
        "contributions": [{"id": "sample:selected-code", "title": "Selected instructions",
        "kind": "panel", "query": {"operation": "disasm",
        "payload": {"address": "${address}", "limit": 3}}}]})");
    }
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!session.contributions().isEmpty(), OpenTimeoutMs);
    const auto builtIn = session.contributions().size();
    session.registerContributions(manifest);
    QTRY_COMPARE(session.contributions().size(), builtIn + 1);
    QSignalSpy results(&session, &Session::contributionResult);
    session.executeContribution(QStringLiteral("sample:selected-code"), Base);
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, OpenTimeoutMs);
    const auto result = results.first().first().toJsonObject();
    QCOMPARE(result.value("contribution_id").toString(),
             QStringLiteral("sample:selected-code"));
    QCOMPARE(result.value("result").toObject().value("items").toArray().size(),
             3);
    session.unregisterContributions(QStringLiteral("sample"));
    QTRY_COMPARE(session.contributions().size(), builtIn);
  }
};

QTEST_MAIN(WorkbenchTests)
#include "WorkbenchTests.moc"
