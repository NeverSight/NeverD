//===- WindowFrameTests.cpp - Native Agent frame checks -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check the Windows resize contract through native hit testing and real
/// maximize/restore transitions, independently of the QML decoration.
//===----------------------------------------------------------------------===//

#include "platform/AgentWindowFrame.h"

#include <QGuiApplication>
#include <QWindow>
#include <QtTest>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace neverd::agent;

class WindowFrameTests : public QObject {
  Q_OBJECT
private:
  static LRESULT hit(QWindow &window, QPoint point) {
    const auto handle = reinterpret_cast<HWND>(window.winId());
    POINT position{qRound(point.x() * window.devicePixelRatio()),
                   qRound(point.y() * window.devicePixelRatio())};
    ClientToScreen(handle, &position);
    return SendMessageW(handle, WM_NCHITTEST, 0,
                        MAKELPARAM(position.x, position.y));
  }

private slots:
  void framelessWindowRetainsNativeResizeEdges() {
    QWindow window;
    window.setFlags(Qt::Window | Qt::FramelessWindowHint |
                    Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus);
    window.setMinimumSize({500, 300});
    window.setGeometry(80, 80, 900, 640);
    AgentWindowFrame frame;
    frame.attach(window);
    const auto style =
        GetWindowLongPtrW(reinterpret_cast<HWND>(window.winId()), GWL_STYLE);
    QVERIFY(style & WS_THICKFRAME);
    QVERIFY(!(style & WS_CAPTION));
    QCOMPARE(hit(window, {1, 1}), LRESULT(HTTOPLEFT));
    QCOMPARE(hit(window, {450, 1}), LRESULT(HTTOP));
    QCOMPARE(hit(window, {1, 320}), LRESULT(HTLEFT));
    QCOMPARE(hit(window, {898, 320}), LRESULT(HTRIGHT));
    QCOMPARE(hit(window, {450, 638}), LRESULT(HTBOTTOM));
    QCOMPARE(hit(window, {898, 638}), LRESULT(HTBOTTOMRIGHT));
    QCOMPARE(hit(window, {450, 320}), LRESULT(HTCLIENT));
  }

  void maximizeRestoreKeepsControlsAndResizeAdmission() {
    QWindow window;
    window.setFlags(Qt::Window | Qt::FramelessWindowHint |
                    Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus);
    window.setMinimumSize({500, 300});
    window.setGeometry(80, 80, 900, 640);
    AgentWindowFrame frame;
    frame.attach(window);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    frame.toggleMaximized();
    QTRY_VERIFY(frame.maximized());
    QCOMPARE(frame.shadowMargin(), 0);
    QCOMPARE(hit(window, {1, window.height() / 2}), LRESULT(HTLEFT));
    frame.toggleMaximized();
    QTRY_VERIFY(!frame.maximized());
    QVERIFY(frame.shadowMargin() > 0);
    QCOMPARE(window.size(), QSize(900, 640));
    frame.minimize();
    QTRY_VERIFY(window.windowStates().testFlag(Qt::WindowMinimized));
    window.showNormal();
    QTRY_VERIFY(!window.windowStates().testFlag(Qt::WindowMinimized));
    window.close();
  }
};

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  WindowFrameTests tests;
  return QTest::qExec(&tests, argc, argv);
}

#include "WindowFrameTests.moc"
