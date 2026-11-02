//===- WindowFrameMac.mm - Cocoa frame integration ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Cocoa hosts frameless windows; NeverD paints their controls and shadow.
//===----------------------------------------------------------------------===//

#include "WindowFramePlatform.h"

#import <AppKit/AppKit.h>
#include <QGuiApplication>
#include <QWindow>

namespace neverd::gui::platform {
std::unique_ptr<QAbstractNativeEventFilter>
createResizeFilter(QWindow &, std::function<Qt::Edges(QPoint)>,
                   std::function<void()>) {
  return {};
}

void restoreForResize(QWindow &) {}

void configureFrame(QWindow &window) {
  if (QGuiApplication::platformName() != QLatin1String("cocoa"))
    return;
  NSWindow *native = reinterpret_cast<NSView *>(window.winId()).window;
  native.hasShadow = NO;
}

void setShadowMargins(QWindow &, const QMargins &) {}
} // namespace neverd::gui::platform
