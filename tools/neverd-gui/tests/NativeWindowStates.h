//===- NativeWindowStates.h - Applied native states -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe completed window-system transitions instead of QWidget requests.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_TESTS_NATIVEWINDOWSTATES_H
#define NEVERD_TOOLS_NEVERD_GUI_TESTS_NATIVEWINDOWSTATES_H

#include <QEvent>
#include <QGuiApplication>
#include <QObject>
#include <QPoint>
#include <QWindow>
#include <functional>

class QWidget;

namespace neverd::gui::test {

bool setNativeFullScreen(QWidget &Window, bool FullScreen);
bool setNativeWindowState(QWidget &Window, Qt::WindowStates States,
                          const std::function<void()> &Apply = {});
#ifdef Q_OS_MACOS
bool nativeWindowIsMinimized(QWidget &Window);
bool reopenNativeApplication();
bool hasNativeApplicationModalWindow();
void quitNativeApplication();
bool canPostNativePointerEvents();
bool moveNativePointer(QPoint Position);
#endif

class NativeWindowStates final : public QObject {
public:
  explicit NativeWindowStates(QWindow &window) : states(window.windowStates()) {
    window.installEventFilter(this);
    // Qt implements frameless Windows maximization with geometry changes,
    // without necessarily delivering a native WindowStateChange event.
    if (QGuiApplication::platformName() == QLatin1String("windows"))
      connect(&window, &QWindow::windowStateChanged, this,
              [this, &window] { record(window); });
  }

  bool is(Qt::WindowStates expected) const { return same(states, expected); }

  static bool same(Qt::WindowStates lhs, Qt::WindowStates rhs) {
    return effective(lhs) == effective(rhs);
  }

  Qt::WindowStates states;
  int count = 0;

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    if (event->type() == QEvent::WindowStateChange)
      record(*static_cast<QWindow *>(object));
    return false;
  }

private:
  void record(const QWindow &window) {
    states = window.windowStates();
    ++count;
  }

  static Qt::WindowState effective(Qt::WindowStates states) {
    if (states & Qt::WindowMinimized)
      return Qt::WindowMinimized;
    if (states & Qt::WindowFullScreen)
      return Qt::WindowFullScreen;
    if (states & Qt::WindowMaximized)
      return Qt::WindowMaximized;
    return Qt::WindowNoState;
  }
};
} // namespace neverd::gui::test

#endif // NEVERD_TOOLS_NEVERD_GUI_TESTS_NATIVEWINDOWSTATES_H
