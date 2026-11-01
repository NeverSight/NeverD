//===- WindowFramePlatform.h - Native frame integration -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Platform integration for application-painted top-level windows.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_WINDOWFRAMEPLATFORM_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_WINDOWFRAMEPLATFORM_H

#include <QAbstractNativeEventFilter>
#include <QMargins>
#include <QPoint>
#include <functional>
#include <memory>

class QWindow;

namespace neverd::gui::platform {
/// Disable platform decoration after creating or moving a native window.
void configureFrame(QWindow &window);
/// Leave native maximization before restoring the frame for an edge drag.
void restoreForResize(QWindow &window);
/// Tell the compositor which part of the surface is our shadow.
void setShadowMargins(QWindow &window, const QMargins &margins);
/// Give the native window system first refusal of resize presses, before
/// client widgets can consume them. The frame owns both the filter and policy.
std::unique_ptr<QAbstractNativeEventFilter>
createResizeFilter(QWindow &window, std::function<Qt::Edges(QPoint)> edgesAt,
                   std::function<void()> prepareResize);
} // namespace neverd::gui::platform

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_WINDOWFRAMEPLATFORM_H
