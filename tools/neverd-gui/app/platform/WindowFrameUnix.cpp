// Wayland excludes our shadow from the compositor's window geometry.

#include "WindowFramePlatform.h"

#include <QWindow>
#include <qpa/qplatformwindow_p.h>

namespace neverd::gui::platform {
std::unique_ptr<QAbstractNativeEventFilter>
createResizeFilter(QWindow &, std::function<Qt::Edges(QPoint)>,
                   std::function<void()>) {
  return {};
}

void configureFrame(QWindow &) {}
void restoreForResize(QWindow &) {}

void setShadowMargins(QWindow &window, const QMargins &margins) {
#if QT_CONFIG(wayland)
  // The compositor sizes, places and tiles the visible window, not its
  // shadow.
  if (auto *wayland =
          window.nativeInterface<QNativeInterface::Private::QWaylandWindow>())
    wayland->setCustomMargins(margins);
#else
  Q_UNUSED(window);
  Q_UNUSED(margins);
#endif
}

} // namespace neverd::gui::platform
