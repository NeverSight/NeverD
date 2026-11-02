//===- WindowFrameWindows.cpp - Windows frame integration ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep Windows window management without system-drawn edges or shadows.
//===----------------------------------------------------------------------===//

#include "WindowFramePlatform.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QPointer>
#include <QWindow>
#include <QtMath>
#include <utility>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

// The DWM SDK header uses Windows types.
#include <dwmapi.h>

namespace neverd::gui::platform {
namespace {
class ResizeFilter final : public QAbstractNativeEventFilter {
  QPointer<QWindow> window_;
  std::function<Qt::Edges(QPoint)> edgesAt_;
  std::function<void()> prepareResize_;

public:
  ResizeFilter(QWindow &window, std::function<Qt::Edges(QPoint)> edgesAt,
               std::function<void()> prepareResize)
      : window_(&window), edgesAt_(std::move(edgesAt)),
        prepareResize_(std::move(prepareResize)) {
    QCoreApplication::instance()->installNativeEventFilter(this);
  }
  ~ResizeFilter() override {
    QCoreApplication::instance()->removeNativeEventFilter(this);
  }
  bool nativeEventFilter(const QByteArray &type, void *message,
                         qintptr *result) override {
    if (type != "windows_generic_MSG")
      return false;
    const auto *msg = static_cast<const MSG *>(message);
    if ((msg->message != WM_NCHITTEST && msg->message != WM_NCLBUTTONDOWN) ||
        !window_ || !window_->handle() ||
        msg->hwnd != reinterpret_cast<HWND>(window_->winId()))
      return false;
    POINT position{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
    if (!ScreenToClient(msg->hwnd, &position))
      return false;
    const qreal scale = window_->devicePixelRatio();
    const auto edges = edgesAt_(
        QPoint(qFloor(position.x / scale), qFloor(position.y / scale)));
    if (!edges)
      return false;
    if (msg->message == WM_NCLBUTTONDOWN) {
      // Restore before DefWindowProc starts its native sizing loop. Edge
      // presses are non-client messages and bypass the Qt mouse filter.
      prepareResize_();
      return false;
    }
    if (edges & Qt::TopEdge)
      *result = edges & Qt::LeftEdge    ? HTTOPLEFT
                : edges & Qt::RightEdge ? HTTOPRIGHT
                                        : HTTOP;
    else if (edges & Qt::BottomEdge)
      *result = edges & Qt::LeftEdge    ? HTBOTTOMLEFT
                : edges & Qt::RightEdge ? HTBOTTOMRIGHT
                                        : HTBOTTOM;
    else
      *result = edges & Qt::LeftEdge ? HTLEFT : HTRIGHT;
    return true;
  }
};
} // namespace

std::unique_ptr<QAbstractNativeEventFilter>
createResizeFilter(QWindow &window, std::function<Qt::Edges(QPoint)> edgesAt,
                   std::function<void()> prepareResize) {
  if (QGuiApplication::platformName() != QLatin1String("windows"))
    return {};
  return std::make_unique<ResizeFilter>(window, std::move(edgesAt),
                                        std::move(prepareResize));
}

void restoreForResize(QWindow &window) {
  if (QGuiApplication::platformName() != QLatin1String("windows"))
    return;
  const auto hwnd = reinterpret_cast<HWND>(window.winId());
  // Qt's frameless showNormal() restores geometry without clearing Windows'
  // own maximized state, which would prevent the native sizing loop.
  if (IsZoomed(hwnd))
    ShowWindow(hwnd, SW_RESTORE);
}

void configureFrame(QWindow &window) {
  if (QGuiApplication::platformName() != QLatin1String("windows"))
    return;
  const auto hwnd = reinterpret_cast<HWND>(window.winId());
  // FramelessWindowHint removes WS_THICKFRAME as well as the decoration.
  // Keep the native resize capability: Qt's frameless WM_NCCALCSIZE handling
  // still leaves the complete rectangle available for our painted chrome.
  if (!(window.flags() & Qt::MSWindowsFixedSizeDialogHint) &&
      window.minimumSize() != window.maximumSize() &&
      window.visibility() != QWindow::FullScreen) {
    const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!(style & WS_THICKFRAME)) {
      SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_THICKFRAME);
      SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                       SWP_NOACTIVATE);
    }
  }
  const DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
  DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy,
                        sizeof(policy));
  const MARGINS margins{};
  DwmExtendFrameIntoClientArea(hwnd, &margins);
}

void setShadowMargins(QWindow &, const QMargins &) {}
} // namespace neverd::gui::platform
