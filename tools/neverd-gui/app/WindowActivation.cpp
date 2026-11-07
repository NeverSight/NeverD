//===- WindowActivation.cpp - Handing the focus to another workbench ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "WindowActivation.h"

#include <QGuiApplication>
#include <QWindow>
#include <qguiapplication_platform.h>
#include <qpa/qplatformwindow_p.h>

namespace neverd::gui {
namespace {
/// What Qt's Wayland shell reads for the next activation, as a launcher sets it
/// for a process it starts.
constexpr char TokenVariable[] = "XDG_ACTIVATION_TOKEN";
} // namespace

void requestActivationToken(QWindow *window, QObject *context,
                            std::function<void(const QString &token)> done) {
#if QT_CONFIG(wayland)
  using QNativeInterface::QWaylandApplication;
  using QNativeInterface::Private::QWaylandWindow;
  auto *surface = window ? window->nativeInterface<QWaylandWindow>() : nullptr;
  auto *application =
      qGuiApp ? qGuiApp->nativeInterface<QWaylandApplication>() : nullptr;
  if (surface && application) {
    // Without the compositor's support Qt answers with an empty token.
    QObject::connect(
        surface, &QWaylandWindow::xdgActivationTokenCreated, context,
        [done = std::move(done)](const QString &token) { done(token); },
        Qt::SingleShotConnection);
    surface->requestXdgActivationToken(application->lastInputSerial());
    return;
  }
#else
  Q_UNUSED(window);
  Q_UNUSED(context);
#endif
  done({});
}

void useActivationToken(const QString &token) {
  if (!token.isEmpty())
    qputenv(TokenVariable, token.toUtf8());
}

} // namespace neverd::gui
