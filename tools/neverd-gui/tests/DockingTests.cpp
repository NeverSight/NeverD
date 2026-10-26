//===- DockingTests.cpp - Floating pane drag regressions ------------------===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the production docking chrome with native window drag targets.
//===----------------------------------------------------------------------===//

#include "Docking.h"
#include "Icons.h"
#include "NativeWindowStates.h"
#include "SettingsKeys.h"
#include "Theme.h"
#include "WindowFrame.h"

#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <chrono>
#include <future>
#include <kddockwidgets/KDDockWidgets.h>
#include <kddockwidgets/core/DropArea.h>
#include <kddockwidgets/core/DropIndicatorOverlay.h>
#include <kddockwidgets/core/Group.h>
#include <kddockwidgets/core/MainWindow.h>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <kddockwidgets/qtwidgets/views/MainWindow.h>
#include <kddockwidgets/qtwidgets/views/TabBar.h>
#include <kddockwidgets/qtwidgets/views/TitleBar.h>
#include <thread>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(Q_OS_MACOS)
#include <ApplicationServices/ApplicationServices.h>
#endif

using namespace neverd::gui;
namespace KD = KDDockWidgets;

namespace {
bool onWayland() {
  return QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
}

bool canPostPointerEvents() {
#ifdef Q_OS_MACOS
  if (QGuiApplication::platformName() == QLatin1String("cocoa"))
    return CGPreflightPostEventAccess();
#endif
  return true;
}

QString quoted(const QString &text) {
  const QByteArray json =
      QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
  return QString::fromUtf8(json.mid(1, json.size() - 2));
}

/// Only enabled by the disposable GNOME test compositor. QtTest alone cannot
/// provide the trusted pointer serial that Wayland requires to start QDrag.
bool shell(const QString &script) {
  QProcess process;
  process.start(QStringLiteral("gdbus"),
                {QStringLiteral("call"), QStringLiteral("--session"),
                 QStringLiteral("--dest"), QStringLiteral("org.gnome.Shell"),
                 QStringLiteral("--object-path"),
                 QStringLiteral("/org/gnome/Shell"), QStringLiteral("--method"),
                 QStringLiteral("org.gnome.Shell.Eval"), script});
  if (!process.waitForFinished() || process.exitCode() != 0)
    return false;
  const QByteArray result = process.readAllStandardOutput();
  if (!result.startsWith("(true,"))
    qWarning() << result;
  return result.startsWith("(true,");
}

QString nativeWindow(const QWidget &window) {
  return QStringLiteral(
             "global.get_window_actors().map(a => a.meta_window)"
             ".filter(w => w.get_pid() === %1 && w.get_title() === %2).at(-1)")
      .arg(QCoreApplication::applicationPid())
      .arg(quoted(window.windowTitle()));
}

bool place(QWidget &window, QPoint position) {
  if (!onWayland()) {
    window.move(position);
    window.raise();
    window.activateWindow();
    return QTest::qWaitForWindowActive(&window);
  }
  return shell(
      QStringLiteral(
          "(() => { const w = %1; if (!w) throw Error('missing window');"
          "Main.overview.hide(); w.move_frame(true, %2, %3);"
          "Main.activateWindow(w); })()")
          .arg(nativeWindow(window))
          .arg(position.x())
          .arg(position.y()));
}

class Pointer {
  QPointer<QWidget> Pressed;

public:
  bool click(QWidget &widget) {
    const QPoint point = widget.rect().center();
    if (!move(widget, point, false))
      return false;
    QTest::qWait(50);
    if (!button(widget, point, true))
      return false;
    QTest::qWait(50);
    return button(widget, point, false);
  }

  bool move(QWidget &widget, QPoint point, bool held) {
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
      QWidget *receiver = Pressed ? Pressed.data() : &widget;
      const QPoint global = widget.mapToGlobal(point);
      QMouseEvent event(QEvent::MouseMove, receiver->mapFromGlobal(global),
                        global, Qt::NoButton,
                        held ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(receiver, &event);
      return true;
    }
    if (onWayland()) {
      QWidget *window = widget.window();
      const QPoint local = widget.mapTo(window, point) +
                           QPoint(window->windowHandle()->frameMargins().left(),
                                  window->windowHandle()->frameMargins().top());
      const qreal scale = window->devicePixelRatioF();
      return shell(
          QStringLiteral(
              "(() => { const w = %1; if (!w) throw Error('missing window');"
              "const r = w.get_buffer_rect(); "
              "global.neverdTestPointer.notify_absolute_motion("
              "imports.gi.GLib.get_monotonic_time(), r.x + %2, r.y + %3); })()")
              .arg(nativeWindow(*window))
              .arg(qRound(local.x() * scale))
              .arg(qRound(local.y() * scale)));
    }
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QLatin1String("windows")) {
      // Send motion through the same native input queue as the buttons. A
      // cursor warp can change position without advancing a system move loop.
      QWidget *window = widget.window();
      const QPoint local = widget.mapTo(window, point);
      const qreal scale = window->devicePixelRatioF();
      POINT native{qRound(local.x() * scale), qRound(local.y() * scale)};
      if (!ClientToScreen(reinterpret_cast<HWND>(window->winId()), &native))
        return false;
      const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
      const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
      if (width <= 1 || height <= 1)
        return false;
      // Win32 absolute mouse coordinates span the inclusive 16-bit range.
      constexpr qint64 absoluteMaximum = 65535;
      INPUT input{};
      input.type = INPUT_MOUSE;
      input.mi.dx =
          static_cast<LONG>((native.x - GetSystemMetrics(SM_XVIRTUALSCREEN)) *
                            absoluteMaximum / (width - 1));
      input.mi.dy =
          static_cast<LONG>((native.y - GetSystemMetrics(SM_YVIRTUALSCREEN)) *
                            absoluteMaximum / (height - 1));
      input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                         MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE_NOCOALESCE;
      return SendInput(1, &input, sizeof(input)) == 1;
    }
#endif
    const QPoint global = widget.mapToGlobal(point);
    if (QGuiApplication::platformName() == QLatin1String("xcb")) {
      QCursor::setPos(global);
      return true;
    }
#ifdef Q_OS_MACOS
    // Cocoa's QCursor::setPos posts an ordinary mouse-moved event. Sending it
    // before a held drag would inject a second, incompatible event type.
    if (QGuiApplication::platformName() == QLatin1String("cocoa"))
      return nativeMouse(held ? kCGEventLeftMouseDragged : kCGEventMouseMoved,
                         global);
#endif
    QCursor::setPos(global);
    QWidget *receiver = Pressed ? Pressed.data() : &widget;
    QMouseEvent event(QEvent::MouseMove, receiver->mapFromGlobal(global),
                      global, Qt::NoButton,
                      held ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(receiver, &event);
    return true;
  }

  bool button(QWidget &widget, QPoint point, bool pressed) {
    // An offscreen widget has no OS window to receive native mouse input,
    // including when the test host itself is Windows or macOS.
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
      if (pressed) {
        Pressed = &widget;
        QTest::mousePress(&widget, Qt::LeftButton, Qt::NoModifier, point);
      } else {
        QWidget *receiver = Pressed ? Pressed.data() : &widget;
        QTest::mouseRelease(receiver, Qt::LeftButton, Qt::NoModifier,
                            receiver->mapFromGlobal(widget.mapToGlobal(point)));
        Pressed.clear();
      }
      return true;
    }
    if (QGuiApplication::platformName() == QLatin1String("xcb")) {
      QProcess process;
      process.start(
          QStringLiteral("xdotool"),
          {pressed ? QStringLiteral("mousedown") : QStringLiteral("mouseup"),
           QStringLiteral("1")});
      return process.waitForFinished() && process.exitCode() == 0;
    }
    if (onWayland())
      return shell(QStringLiteral("global.neverdTestPointer.notify_button("
                                  "imports.gi.GLib.get_monotonic_time(), 1, "
                                  "imports.gi.Clutter.ButtonState.%1)")
                       .arg(pressed ? QStringLiteral("PRESSED")
                                    : QStringLiteral("RELEASED")));
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QLatin1String("windows")) {
      INPUT input{};
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
      return SendInput(1, &input, sizeof(input)) == 1;
    }
#elif defined(Q_OS_MACOS)
    if (QGuiApplication::platformName() == QLatin1String("cocoa"))
      return nativeMouse(pressed ? kCGEventLeftMouseDown : kCGEventLeftMouseUp,
                         QCursor::pos());
#endif
    if (pressed) {
      Pressed = &widget;
      QTest::mousePress(&widget, Qt::LeftButton, Qt::NoModifier, point);
    } else {
      QWidget *receiver = Pressed ? Pressed.data() : &widget;
      QTest::mouseRelease(receiver, Qt::LeftButton, Qt::NoModifier,
                          receiver->mapFromGlobal(widget.mapToGlobal(point)));
      Pressed.clear();
    }
    return true;
  }

#ifdef Q_OS_MACOS
  std::future<std::pair<bool, bool>> finishNativeDrag(QPoint destination) {
    // Cocoa tracks a system move or resize in an AppKit loop that does not
    // service Qt timers. Deliver motion and release independently of that loop.
    // Only copied screen coordinates cross threads; widgets stay on the GUI
    // thread.
    const QPoint origin = QCursor::pos();
    const int steps =
        qEnvironmentVariableIntValue("NEVERD_TRACE_DRAG_STEPS") == 8 ? 8 : 1;
    return std::async(std::launch::async, [origin, destination, steps] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      bool moved = true;
      for (int i = 1; i <= steps; ++i) {
        moved &= nativeMouse(kCGEventLeftMouseDragged,
                             origin + (destination - origin) * i / steps);
        if (steps == 8)
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
      if (steps == 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      const bool released = nativeMouse(kCGEventLeftMouseUp, destination);
      return std::make_pair(moved, released);
    });
  }

private:
  static bool nativeMouse(CGEventType type, QPoint point) {
    CGEventRef event = CGEventCreateMouseEvent(
        nullptr, type, CGPointMake(point.x(), point.y()), kCGMouseButtonLeft);
    if (!event)
      return false;
    if (qEnvironmentVariableIsSet("NEVERD_TRACE_NATIVE_DRAG"))
      qInfo() << "native-post" << int(type) << point
              << CGEventGetTimestamp(event)
              << CGEventGetIntegerValueField(event, kCGMouseEventDeltaX)
              << CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return true;
  }
#endif
};

struct Desktop {
  KD::QtWidgets::MainWindow Window{QStringLiteral("drag-workbench")};
  KD::QtWidgets::DockWidget Target{QStringLiteral("target")};
  KD::QtWidgets::DockWidget Source{QStringLiteral("source")};

  Desktop() {
    Window.setWindowTitle(QStringLiteral("Docking test workbench"));
    const QSize screen = Window.screen()->availableGeometry().size();
    Window.resize(std::min(950, screen.width() * 3 / 5),
                  std::min(650, screen.height() * 2 / 3));
    WindowFrame::attach(Window);
    Target.setTitle(QStringLiteral("Target pane"));
    Target.setWidget(new QLabel(QStringLiteral("Target")));
    Source.setTitle(QStringLiteral("Source pane"));
    Source.setIcon(QApplication::style()->standardIcon(QStyle::SP_FileIcon));
    auto *source = new QPushButton(QStringLiteral("Source"));
    source->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    Source.setWidget(source);
    Window.addDockWidget(&Target, KD::Location_OnLeft);
    Target.addDockWidgetAsTab(&Source);
    Window.show();
    Source.setFloating(true);
    Source.QWidget::window()->resize(std::min(450, screen.width() / 3),
                                     std::min(350, screen.height() / 2));
  }

  KD::QtWidgets::TitleBar *titleBar() const {
    for (auto *bar :
         Source.QWidget::window()->findChildren<KD::QtWidgets::TitleBar *>())
      if (bar->isVisible())
        return bar;
    return nullptr;
  }

  QAbstractButton *lockButton() const {
    return Source.QWidget::window()->findChild<QAbstractButton *>(
        QStringLiteral("floatingLockButton"));
  }
};
} // namespace

class DockingTests : public QObject {
  Q_OBJECT
  QTemporaryDir Settings;

private slots:
  void initTestCase() {
    QVERIFY(Settings.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("NeverDTests"));
    QCoreApplication::setApplicationName(QStringLiteral("DockingTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       Settings.path());
    Theme::instance().apply();
    configureDocking();
    if (onWayland() && qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER")) {
      QVERIFY(shell(QStringLiteral(
          "Main.overview.hide(); global.neverdTestPointer = "
          "imports.gi.Clutter.get_default_backend().get_default_seat()"
          ".create_virtual_device(imports.gi.Clutter.InputDeviceType.POINTER_"
          "DEVICE); global.neverdTestPointer.notify_absolute_motion("
          "imports.gi.GLib.get_monotonic_time(), global.stage.width / 2, "
          "global.stage.height / 2);")));
      // A pointer left at (0, 0) can reopen GNOME's overview via its hot
      // corner after startup, consuming the first window-control gestures.
      QTest::qWait(750); // Finish leaving the compositor's startup overview.
    }
  }

  void floatingCaption_data() {
    QTest::addColumn<bool>("nativeCaption");
    QTest::newRow("custom") << false;
    QTest::newRow("native-preference") << true;
  }

  void floatingCaption() {
    QFETCH(bool, nativeCaption);
    QSettings().setValue(settings::NativeTitleBar, nativeCaption);
    Desktop desktop;
    auto *floating = desktop.Source.QWidget::window();
    QVERIFY(QTest::qWaitForWindowExposed(floating));
    QVERIFY(desktop.titleBar());
    auto *maximize = floating->findChild<QAbstractButton *>(
        QStringLiteral("floatingMaximizeButton"));
    auto *minimize = floating->findChild<QAbstractButton *>(
        QStringLiteral("floatingMinimizeButton"));
    QVERIFY(maximize && maximize->isVisible());
    QVERIFY(minimize && minimize->isVisible());
    QVERIFY(desktop.lockButton() && desktop.lockButton()->isChecked());
    auto *close = floating->findChild<QAbstractButton *>(
        QStringLiteral("floatingCloseButton"));
    QVERIFY(close && close->isVisible());
    // The full window controls remain usable, unlike tiny dock-tool buttons.
    QVERIFY(close->width() >= 40);
    QVERIFY(close->height() >= 28);
    // A shrink can be followed by input before the next paint. A stale drag
    // region must not cover any controls in that interval.
    floating->setUpdatesEnabled(false);
    for (const int width : {900, 450}) {
      floating->resize(width, 350);
      QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
      for (auto *button : {maximize, minimize, close, desktop.lockButton()})
        QCOMPARE(
            floating->childAt(button->mapTo(floating, button->rect().center())),
            button);
    }
    floating->setUpdatesEnabled(true);
    if (onWayland()) {
      QVERIFY(floating->windowFlags() & Qt::FramelessWindowHint);
      QCOMPARE(floating->windowHandle()->frameMargins(), QMargins());
    }
    // The padlock shows the lock, closed while locked and open once unlocked,
    // over the caption's own color rather than a checked fill.
    auto *lock = desktop.lockButton();
    auto *moveArea = floating->findChild<QWidget *>(
        QStringLiteral("floatingWindowMoveArea"));
    QVERIFY(moveArea);
    const auto lockOverCaption = [&] {
      const QImage window = floating->grab().toImage();
      const auto at = [&](QWidget *widget, QPoint point) {
        return window.pixelColor(widget->mapTo(floating, point) *
                                 floating->devicePixelRatio());
      };
      return at(lock, QPoint(2, lock->height() / 2)) ==
             at(moveArea, moveArea->rect().center());
    };
    QCOMPARE(lock->icon().cacheKey(), icon(QStringLiteral("lock")).cacheKey());
    QVERIFY(lockOverCaption());
    lock->setChecked(false);
    QCOMPARE(lock->icon().cacheKey(),
             icon(QStringLiteral("lock_open")).cacheKey());
    QVERIFY(lockOverCaption());
    lock->setChecked(true);
    QSettings().remove(settings::NativeTitleBar);
  }

  void customFloatingMoveAndResize() {
    if (!onWayland() || !qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER"))
      QSKIP("Uses the isolated compositor for trusted move/resize input.");
    Desktop desktop;
    QWidget *floating = desktop.Source.QWidget::window();
    QVERIFY(QTest::qWaitForWindowExposed(&desktop.Window));
    QVERIFY(QTest::qWaitForWindowExposed(floating));
    QTest::qWait(250);
    QVERIFY(place(*floating, QPoint(1100, 180)));
    QTest::qWait(100);
    QVERIFY(
        shell(QStringLiteral("global.neverdBeforeMove = (%1).get_frame_rect();")
                  .arg(nativeWindow(*floating))));
    Pointer pointer;
    auto *handle = floating->findChild<QWidget *>(
        QStringLiteral("floatingWindowMoveArea"));
    QVERIFY(handle && handle->isVisible());
    const QPoint start = handle->mapTo(floating, handle->rect().center());
    QVERIFY(pointer.move(*floating, start, false));
    QTest::qWait(50);
    QVERIFY(pointer.button(*floating, start, true));
    QTest::qWait(50);
    QVERIFY(pointer.move(*floating, start + QPoint(80, 60), true));
    QTest::qWait(100);
    QVERIFY(pointer.button(*floating, start, false));
    QVERIFY(shell(QStringLiteral("(() => {const r = (%1).get_frame_rect();"
                                 "if(r.x === global.neverdBeforeMove.x && "
                                 "r.y === global.neverdBeforeMove.y) throw "
                                 "Error('window did not move');})()")
                      .arg(nativeWindow(*floating))));
    const QSize size = floating->size();
    const QPoint corner = floating->rect().bottomRight() - QPoint(2, 2);
    QVERIFY(pointer.move(*floating, corner, false));
    QTest::qWait(50);
    QVERIFY(pointer.button(*floating, corner, true));
    QTest::qWait(50);
    QVERIFY(pointer.move(*floating, corner + QPoint(80, 60), true));
    QTest::qWait(100);
    QVERIFY(pointer.button(*floating, corner, false));
    QTRY_VERIFY(floating->width() > size.width() &&
                floating->height() > size.height());
    QVERIFY(desktop.Source.isFloating());
  }

  void floatingEdgesResizeWithoutDocking_data() {
    QTest::addColumn<int>("edges");
    QTest::addColumn<bool>("locked");
    for (bool locked : {false, true})
      for (auto [name, edges] :
           {std::pair{"left", Qt::Edges(Qt::LeftEdge)},
            {"right", Qt::Edges(Qt::RightEdge)},
            {"top", Qt::Edges(Qt::TopEdge)},
            {"bottom", Qt::Edges(Qt::BottomEdge)},
            {"top-left", Qt::TopEdge | Qt::LeftEdge},
            {"top-right", Qt::TopEdge | Qt::RightEdge},
            {"bottom-left", Qt::BottomEdge | Qt::LeftEdge},
            {"bottom-right", Qt::BottomEdge | Qt::RightEdge}})
        QTest::newRow(qPrintable(QString::fromLatin1(name) +
                                 (locked ? "-locked" : "-unlocked")))
            << int(edges) << locked;
  }

  void doubleClickDetachStartsLocked() {
    Desktop desktop;
    desktop.Source.setFloating(false);
    QTRY_VERIFY(!desktop.Source.isFloating());
    auto *bar = desktop.Window.findChild<KD::QtWidgets::TabBar *>();
    QVERIFY(bar);
    QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier,
                       bar->tabRect(bar->currentIndex()).center());
    QTRY_VERIFY(desktop.Source.isFloating());
    QVERIFY(desktop.lockButton() && desktop.lockButton()->isChecked());
  }

  void explicitDockIgnoresLock() {
    if (!canPostPointerEvents())
      QSKIP("Cocoa native mouse input requires event-posting permission.");
    Desktop desktop;
    auto *floating = desktop.Source.QWidget::window();
    QVERIFY(QTest::qWaitForWindowExposed(floating));
    QTest::qWait(250);
    QVERIFY(place(*floating, QPoint(100, 100)));
    QTest::qWait(100);
    QVERIFY(desktop.lockButton() && desktop.lockButton()->isChecked());
    QAbstractButton *dock = nullptr;
    for (auto *button : desktop.titleBar()->findChildren<QAbstractButton *>())
      if (button->isVisible() &&
          button->toolTip() == QStringLiteral("Dock window"))
        dock = button;
    QVERIFY(dock && dock->isEnabled());
    Pointer pointer;
    QVERIFY(pointer.click(*dock));
    QTRY_VERIFY(!desktop.Source.isFloating());
    QCOMPARE(desktop.Source.QWidget::window(), &desktop.Window);
    QVERIFY(desktop.Source.widget()->isVisible());
  }

  void lockedDragsStayFloating_data() {
    QTest::addColumn<bool>("tab");
    QTest::newRow("caption") << false;
    QTest::newRow("tab") << true;
  }

  void lockedDragsStayFloating() {
    if (!canPostPointerEvents())
      QSKIP("Cocoa native mouse input requires event-posting permission.");
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
      QSKIP("Dragging needs a native window manager.");
    if (onWayland() && !qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER"))
      QSKIP("Wayland needs the isolated compositor's native pointer.");
    QFETCH(bool, tab);
    Desktop desktop;
    auto *floating = desktop.Source.QWidget::window();
    QVERIFY(QTest::qWaitForWindowExposed(&desktop.Window));
    QVERIFY(QTest::qWaitForWindowExposed(floating));
    QTest::qWait(250);
    QVERIFY(place(desktop.Window, QPoint(20, 50)));
    QVERIFY(place(*floating, QPoint(320, 250)));
    QTest::qWait(100);
    QVERIFY(desktop.lockButton() && desktop.lockButton()->isChecked());
    QWidget *source = floating->findChild<QWidget *>(
        QStringLiteral("floatingWindowMoveArea"));
    QVERIFY(source && source->isVisible());
    QPoint start = source->rect().center();
    if (tab) {
      auto *tabs = floating->findChild<KD::QtWidgets::TabBar *>();
      QVERIFY(tabs);
      source = tabs;
      start = tabs->tabRect(tabs->currentIndex()).center();
    }
    const QPoint before = floating->pos();
    qInfo() << "locked-drag before" << tab << floating->geometry()
            << source->mapToGlobal(start) << source->rect()
            << desktop.Window.mapToGlobal(desktop.Window.rect().center())
            << floating->screen()->availableGeometry()
            << floating->isActiveWindow() << QApplication::activeWindow();
    if (onWayland())
      QVERIFY(shell(
          QStringLiteral("global.neverdBeforeMove = (%1).get_frame_rect();")
              .arg(nativeWindow(*floating))));
    Pointer pointer;
    auto *area = desktop.Window.mainWindow()->dropArea();
    bool moved = false;
    bool preview = false;
    bool released = false;
    QVERIFY(pointer.move(*source, start, false));
    QTest::qWait(50);
    QVERIFY(pointer.button(*source, start, true));
#ifdef Q_OS_MACOS
    QTimer observePreview;
    connect(&observePreview, &QTimer::timeout, &desktop.Window,
            [&] { preview |= area->dropIndicatorOverlay()->isVisible(); });
    observePreview.start(50);
    auto completion = pointer.finishNativeDrag(
        desktop.Window.mapToGlobal(desktop.Window.rect().center()));
    QTRY_VERIFY_WITH_TIMEOUT(completion.wait_for(std::chrono::milliseconds(
                                 0)) == std::future_status::ready,
                             5000);
    const auto completed = completion.get();
    moved = completed.first;
    released = completed.second;
    observePreview.stop();
#else
    QTimer::singleShot(50, &desktop.Window, [&] {
      moved =
          pointer.move(desktop.Window, desktop.Window.rect().center(), true);
    });
    QTimer::singleShot(250, &desktop.Window, [&] {
      preview = area->dropIndicatorOverlay()->isVisible();
      released = pointer.button(desktop.Window, QPoint(), false);
    });
#endif
    QTRY_VERIFY_WITH_TIMEOUT(released, 5000);
    QTest::qWait(150);
    qInfo() << "locked-drag after" << tab << floating->geometry()
            << QCursor::pos() << floating->isActiveWindow();
    QVERIFY(moved);
    QVERIFY(!preview);
    QVERIFY(desktop.Source.isFloating());
    QCOMPARE(desktop.Source.QWidget::window(), floating);
    QVERIFY(desktop.lockButton()->isChecked());
    if (!tab) {
      if (onWayland())
        QVERIFY(shell(
            QStringLiteral("(() => {const r = (%1).get_frame_rect();"
                           "if (r.x === global.neverdBeforeMove.x && "
                           "r.y === global.neverdBeforeMove.y) "
                           "throw Error('locked caption cannot move');})()")
                .arg(nativeWindow(*floating))));
      else
        QTRY_VERIFY(floating->pos() != before);
    }
  }

  void floatingEdgesResizeWithoutDocking() {
    if (!canPostPointerEvents())
      QSKIP("Cocoa native mouse input requires event-posting permission.");
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
      QSKIP("Resizing needs native windows.");
    if (onWayland() && !qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER"))
      QSKIP("Wayland needs the isolated compositor's native pointer.");
    QFETCH(int, edges);
    QFETCH(bool, locked);
    Desktop desktop;
    QPointer<QWidget> floating = desktop.Source.QWidget::window();
    QVERIFY(QTest::qWaitForWindowExposed(&desktop.Window));
    QVERIFY(QTest::qWaitForWindowExposed(floating));
    QTest::qWait(250);
    QVERIFY(place(desktop.Window, QPoint(20, 50)));
    QVERIFY(place(*floating, QPoint(320, 250)));
    QTest::qWait(100);
    QVERIFY(desktop.lockButton() && desktop.lockButton()->isChecked());
    Pointer pointer;
    if (!locked) {
      QVERIFY(pointer.click(*desktop.lockButton()));
      QTRY_VERIFY(!desktop.lockButton()->isChecked());
    }
    const QSize size = floating->size();
    // Hit the painted edge, not the transparent outer shadow. Windows passes
    // fully transparent pixels through to whichever window lies underneath.
    const QMargins shadow = floating->contentsMargins();
    QPoint start = floating->rect().center();
    QPoint delta;
    if (edges & Qt::LeftEdge) {
      start.setX(shadow.left() + 1);
      delta.setX(-60);
    } else if (edges & Qt::RightEdge) {
      start.setX(size.width() - shadow.right() - 2);
      delta.setX(60);
    }
    if (edges & Qt::TopEdge) {
      start.setY(shadow.top() + 1);
      delta.setY(-60);
    } else if (edges & Qt::BottomEdge) {
      start.setY(size.height() - shadow.bottom() - 2);
      delta.setY(60);
    }
    auto *area = desktop.Window.mainWindow()->dropArea();
    QSignalSpy floatingChanges(&desktop.Source,
                               &KD::QtWidgets::DockWidget::isFloatingChanged);
    bool preview = false;
    bool moved = false;
    bool released = false;
    QVERIFY(pointer.move(*floating, start, false));
    QTest::qWait(50);
    QVERIFY(pointer.button(*floating, start, true));
#ifdef Q_OS_MACOS
    QTimer observePreview;
    connect(&observePreview, &QTimer::timeout, &desktop.Window,
            [&] { preview |= area->dropIndicatorOverlay()->isVisible(); });
    observePreview.start(50);
    auto completion =
        pointer.finishNativeDrag(floating->mapToGlobal(start + delta));
    QTRY_VERIFY_WITH_TIMEOUT(completion.wait_for(std::chrono::milliseconds(
                                 0)) == std::future_status::ready,
                             5000);
    const auto completed = completion.get();
    moved = completed.first;
    released = completed.second;
    observePreview.stop();
#else
    QTimer::singleShot(50, &desktop.Window, [&] {
      if (floating)
        moved = pointer.move(*floating, start + delta, true);
    });
    QTimer::singleShot(250, &desktop.Window, [&] {
      preview = area->dropIndicatorOverlay()->isVisible();
      released = pointer.button(desktop.Window, QPoint(), false);
    });
#endif
    QTRY_VERIFY_WITH_TIMEOUT(released, 5000);
    QTest::qWait(200);
    QVERIFY(moved);
    QVERIFY(!preview);
    QVERIFY(desktop.Source.isFloating());
    QCOMPARE(floatingChanges.size(), 0);
    QVERIFY(floating);
    if (delta.x())
      QTRY_VERIFY(floating->width() > size.width());
    else
      QCOMPARE(floating->width(), size.width());
    if (delta.y())
      QTRY_VERIFY(floating->height() > size.height());
    else
      QCOMPARE(floating->height(), size.height());
    QVERIFY(!area->dropIndicatorOverlay()->isVisible());
  }

  void floatingWindowStates() {
    if (!canPostPointerEvents())
      QSKIP("Cocoa native mouse input requires event-posting permission.");
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
      QSKIP("Requires a native window manager.");
    Desktop desktop;
    QWidget *floating = desktop.Source.QWidget::window();
    QVERIFY(QTest::qWaitForWindowExposed(&desktop.Window));
    QVERIFY(QTest::qWaitForWindowExposed(floating));
    QTest::qWait(250);
    QVERIFY(place(*floating, QPoint(100, 100)));
    QTest::qWait(100);
    const QSize normalSize = floating->size();
    auto *maximize = floating->findChild<QAbstractButton *>(
        QStringLiteral("floatingMaximizeButton"));
    QVERIFY(maximize && maximize->isVisible());
    QSignalSpy maximizedClicks(maximize, &QAbstractButton::clicked);
    Pointer pointer;
    int clicks = 0;
    for (bool maximized : {true, false}) {
      const QPoint center = maximize->rect().center();
      QVERIFY(pointer.move(*maximize, center, false));
      QTest::qWait(50);
      QVERIFY(pointer.button(*maximize, center, true));
      QTest::qWait(50);
      QVERIFY(pointer.button(*maximize, center, false));
      ++clicks;
      QTRY_COMPARE(maximizedClicks.size(), clicks);
      QTRY_COMPARE(floating->isMaximized(), maximized);
      // Qt reports the new state before the compositor finishes moving the
      // caption buttons to their final screen positions.
      QTest::qWait(500);
    }
    QTRY_COMPARE(floating->size(), normalSize);
    auto *minimize = floating->findChild<QAbstractButton *>(
        QStringLiteral("floatingMinimizeButton"));
    QVERIFY(minimize && minimize->isVisible());
    QSignalSpy minimizedClicks(minimize, &QAbstractButton::clicked);
    const QPoint center = minimize->rect().center();
    QVERIFY(pointer.move(*minimize, center, false));
    QTest::qWait(50);
    QVERIFY(pointer.button(*minimize, center, true));
    QTest::qWait(50);
    QVERIFY(pointer.button(*minimize, center, false));
    QTRY_COMPARE(minimizedClicks.size(), 1);
    QTest::qWait(500);
    // Wayland does not report the minimized state back to the Qt client.
    // Verify the actual compositor window instead of QWidget's cached state.
    if (onWayland())
      QVERIFY(shell(QStringLiteral("if (!(%1).minimized) "
                                   "throw Error('window did not minimize');")
                        .arg(nativeWindow(*floating))));
    else
      QTRY_VERIFY(floating->isMinimized());
    floating->showNormal();
    // Restore as a user selecting the window in the desktop would. Wayland
    // can reject an activation request from an unfocused client.
    QVERIFY(place(*floating, QPoint(100, 100)));
    QTRY_VERIFY(!floating->isMinimized());
    QVERIFY(QTest::qWaitForWindowExposed(floating));
    if (onWayland())
      QTRY_VERIFY(shell(QStringLiteral("if ((%1).minimized) "
                                       "throw Error('window did not restore');")
                            .arg(nativeWindow(*floating))));
    QTRY_COMPARE(floating->size(), normalSize);
    test::NativeWindowStates applied(*floating->windowHandle());
#ifdef Q_OS_MACOS
    QVERIFY(neverd::gui::test::setNativeFullScreen(*floating, true));
#else
    floating->showFullScreen();
#endif
    QTRY_VERIFY_WITH_TIMEOUT(applied.is(Qt::WindowFullScreen), 10000);
    QTRY_VERIFY(floating->isFullScreen());
#ifdef Q_OS_MACOS
    QVERIFY(neverd::gui::test::setNativeFullScreen(*floating, false));
#else
    floating->showNormal();
#endif
    QTRY_VERIFY_WITH_TIMEOUT(applied.is(Qt::WindowNoState), 10000);
    QTRY_VERIFY(!floating->isFullScreen());
    QTRY_COMPARE(floating->size(), normalSize);
    QVERIFY(desktop.titleBar());
    QVERIFY(desktop.Source.isFloating());
    QVERIFY(desktop.Source.widget()->isVisible());
  }

  void dragIntoWorkbench_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<bool>("tab");
    QTest::addColumn<int>("location");
    QTest::addColumn<QString>("placement");
    for (const auto &placement :
         {QStringLiteral("separate"), QStringLiteral("overlap"),
          QStringLiteral("maximized")})
      for (bool dark : {false, true})
        for (bool tab : {false, true})
          for (auto location :
               {KD::DropLocation_Center, KD::DropLocation_Right,
                KD::DropLocation_Bottom, KD::DropLocation_None}) {
            const auto name = QStringLiteral("%1-%2-%3-%4")
                                  .arg(dark ? "dark" : "light")
                                  .arg(tab ? "tab" : "caption")
                                  .arg(static_cast<int>(location))
                                  .arg(placement);
            QTest::newRow(qPrintable(name))
                << dark << tab << static_cast<int>(location) << placement;
          }
  }

  void dragIntoWorkbench() {
    if (!canPostPointerEvents())
      QSKIP("Cocoa native mouse input requires event-posting permission.");
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
      QSKIP("Dragging needs native windows; run under X11, Wayland, Windows or "
            "macOS.");
    if (onWayland() && !qEnvironmentVariableIsSet("NEVERD_TEST_GNOME_POINTER"))
      QSKIP("Wayland needs the isolated compositor's native pointer.");
    if (QGuiApplication::platformName() == QLatin1String("xcb") &&
        QStandardPaths::findExecutable(QStringLiteral("xdotool")).isEmpty())
      QSKIP("X11 native pointer input needs xdotool.");
    QFETCH(bool, dark);
    QFETCH(bool, tab);
    QFETCH(int, location);
    QFETCH(QString, placement);
    Theme::instance().setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    Theme::instance().apply();
    Desktop desktop;
    QWidget *floating = desktop.Source.QWidget::window();
    QVERIFY(QTest::qWaitForWindowExposed(&desktop.Window));
    QVERIFY(QTest::qWaitForWindowExposed(floating));
    QTest::qWait(250); // Allow the compositor's initial placement to finish.
    QVERIFY(place(desktop.Window, QPoint(20, 50)));
    const qreal scale = onWayland() ? desktop.Window.devicePixelRatioF() : 1;
    QVERIFY(
        place(*floating,
              placement != QLatin1String("separate")
                  ? QPoint(20 + qRound(300 * scale), 50 + qRound(200 * scale))
                  : QPoint(qRound(desktop.Window.width() * scale) + 60, 180)));
    if (placement == QLatin1String("maximized")) {
      floating->showMaximized();
      QTRY_VERIFY(floating->isMaximized());
      QTest::qWait(250);
    }
    QTest::qWait(100);
    const QPointer<QWindow> native = floating->windowHandle();
    const auto originalStates = native->windowStates();
    if (onWayland())
      QVERIFY(shell(
          QStringLiteral("global.neverdBeforeDrag = (%1).get_frame_rect();")
              .arg(nativeWindow(*floating))));
    QVERIFY(desktop.lockButton() && desktop.lockButton()->isChecked());
    Pointer pointer;
    QVERIFY(pointer.click(*desktop.lockButton()));
    QTRY_VERIFY(!desktop.lockButton()->isChecked());
    QWidget *source = desktop.titleBar();
    QVERIFY(source);
    auto *caption =
        source->findChild<QWidget *>(QStringLiteral("floatingWindowMoveArea"));
    QVERIFY(caption);
    QPoint start = caption->geometry().center();
    if (tab) {
      auto *tabs = floating->findChild<KD::QtWidgets::TabBar *>();
      QVERIFY(tabs);
      source = tabs;
      start = tabs->tabRect(tabs->currentIndex()).center();
    }
    QVERIFY(pointer.move(*source, start, false));
    QTest::qWait(50);
    QVERIFY(pointer.button(*source, start, true));
    bool moved = false;
    bool preview = false;
    bool released = false;
    QPoint destination;
    auto *area = desktop.Window.mainWindow()->dropArea();
    QString dragState;
    auto recordDrag = [&](const char *phase) {
      QDebug state(&dragState);
      state << phase << "grabber" << QWidget::mouseGrabber() << "buttons"
            << QGuiApplication::mouseButtons() << "drop"
            << area->currentDropLocation() << "overlay"
            << area->dropIndicatorOverlay()->isVisible() << "cursor"
            << QCursor::pos() << "source" << floating->geometry() << "target"
            << desktop.Window.geometry() << "destination"
            << desktop.Window.mapToGlobal(destination);
#ifdef Q_OS_WIN
      state << "native button" << bool(GetKeyState(VK_LBUTTON) & 0x8000)
            << "physical button" << bool(GetAsyncKeyState(VK_LBUTTON) & 0x8000);
#endif
    };
    // Queue the whole gesture before dispatching the press: it may enter a
    // native move loop. Separate timers also run in Wayland's QDrag loop.
    QTimer::singleShot(50, &desktop.Window, [&] {
      moved = pointer.move(*source, start + QPoint(0, 50), true);
    });
    QTimer::singleShot(150, &desktop.Window, [&] {
      recordDrag("before target motion");
      moved &=
          pointer.move(desktop.Window, desktop.Window.rect().center(), true);
    });
    QTimer::singleShot(350, &desktop.Window, [&] {
      recordDrag("before indicator motion");
      destination = location == KD::DropLocation_None
                        ? QPoint(100, 100)
                        : desktop.Window.mapFromGlobal(
                              area->dropIndicatorOverlay()->posForIndicator(
                                  static_cast<KD::DropLocation>(location)));
      moved &= pointer.move(desktop.Window, destination, true);
    });
    QTimer::singleShot(550, &desktop.Window, [&] {
      preview = area->currentDropLocation() == location;
      if (!preview)
        qWarning() << "Drop target mismatch: requested" << location << "actual"
                   << int(area->currentDropLocation()) << "cursor"
                   << QCursor::pos() << "destination"
                   << desktop.Window.mapToGlobal(destination) << "workbench"
                   << desktop.Window.geometry() << "floating"
                   << floating->geometry() << "overlay visible"
                   << area->dropIndicatorOverlay()->isVisible();
      const QString captures = qEnvironmentVariable("NEVERD_SHORTCUT_CAPTURES");
      if (!captures.isEmpty()) {
        QDir().mkpath(captures);
        const QString name = QString::fromLatin1(QTest::currentDataTag());
        desktop.Window.grab().save(captures + "/" + name + "-preview.png");
        floating->grab().save(captures + "/" + name + "-floating.png");
        if (onWayland())
          QProcess::execute(
              QStringLiteral("gdbus"),
              {QStringLiteral("call"), QStringLiteral("--session"),
               QStringLiteral("--dest"),
               QStringLiteral("org.gnome.Shell.Screenshot"),
               QStringLiteral("--object-path"),
               QStringLiteral("/org/gnome/Shell/Screenshot"),
               QStringLiteral("--method"),
               QStringLiteral("org.gnome.Shell.Screenshot.Screenshot"),
               QStringLiteral("false"), QStringLiteral("false"),
               captures + "/" + name + "-desktop.png"});
        else
          desktop.Window.screen()->grabWindow(0).save(captures + "/" + name +
                                                      "-desktop.png");
      }
      recordDrag("before release");
      released = pointer.button(desktop.Window, destination, false);
    });
    QTRY_VERIFY_WITH_TIMEOUT(released, 5000);
    // Native input is queued. A cancelled drag is still floating before its
    // release arrives; wait for dispatch before destroying its drop target.
    // Wayland ends QDrag through the data-device protocol, without updating
    // Qt's ordinary mouse-button state. Its hidden preview marks completion.
    if (!onWayland())
      QTRY_COMPARE(QGuiApplication::mouseButtons(),
                   Qt::MouseButtons(Qt::NoButton));
    QTRY_VERIFY(!area->dropIndicatorOverlay()->isVisible());
    QVERIFY(moved);
    QVERIFY2(preview, qPrintable(dragState));
    QTRY_COMPARE(desktop.Source.isFloating(),
                 location == KD::DropLocation_None);
    QVERIFY(desktop.Source.widget()->isVisible());
    if (location == KD::DropLocation_None) {
      QCOMPARE(desktop.Source.QWidget::window(), floating);
      QTRY_VERIFY(!floating->isMinimized());
      QTest::qWait(300);
      QCOMPARE(floating->windowHandle(), native.data());
      if (onWayland()) {
        QCOMPARE(native->windowStates(), originalStates);
        QVERIFY(shell(
            QStringLiteral(
                "(() => {const w = %1;"
                "if(w.minimized) throw Error('still minimized');"
                "const r = w.get_frame_rect(), b = global.neverdBeforeDrag;"
                "if(r.x !== b.x || r.y !== b.y || r.width !== b.width || "
                "r.height !== b.height)"
                "throw Error('cancel changed native geometry');"
                "})()")
                .arg(nativeWindow(*floating))));
      }
      auto *button = qobject_cast<QPushButton *>(desktop.Source.widget());
      QSignalSpy clicks(button, &QPushButton::clicked);
      const QPoint center = button->rect().center();
      QVERIFY(pointer.move(*button, center, false));
      QTest::qWait(50);
      QVERIFY(pointer.button(*button, center, true));
      QTest::qWait(50);
      QVERIFY(pointer.button(*button, center, false));
      QTRY_COMPARE(clicks.size(), 1);
    } else {
      QCOMPARE(desktop.Source.QWidget::window(), &desktop.Window);
      QCOMPARE(desktop.Source.group() == desktop.Target.group(),
               location == KD::DropLocation_Center);
    }
  }
};

int main(int argc, char **argv) {
  WindowFrame::prepareNativeWindows();
  QApplication app(argc, argv);
  DockingTests tests;
  return QTest::qExec(&tests, argc, argv);
}

#include "DockingTests.moc"
