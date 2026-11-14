//===- AgentWindowFrame.cpp - Application-painted Quick window
//--------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reuse the desktop's platform hooks, palette and frame metrics. The adapter
/// translates QWindow input; QML paints the caption, controls and shadow.
//===----------------------------------------------------------------------===//

#include "AgentWindowFrame.h"

#include "../../neverd-gui/Config.h"
#include "../../neverd-gui/app/WindowFramePlatform.h"

#include <QCursor>
#include <QEvent>
#include <QGuiApplication>
#include <QMargins>
#include <QMouseEvent>
#include <QStyleHints>
#include <QWindow>
#include <algorithm>

namespace neverd::agent {
namespace {
namespace metrics = ::neverd::limits::gui_config;
namespace platform = ::neverd::gui::platform;
struct ChromeColor {
  const char *Name;
  const char *Light;
  const char *Dark;
};
constexpr ChromeColor ChromeColors[] = {
#define NEVERD_CHROME_COLOR(Name, Light, Dark, LightHighContrast,              \
                            DarkHighContrast)                                  \
  {#Name, Light, Dark},
#include "../../neverd-gui/app/ChromeColors.def"
};
constexpr ChromeColor ThemeColors[] = {
#define NEVERD_THEME_COLOR(Name, Light, Dark, LightHighContrast,               \
                           DarkHighContrast)                                   \
  {#Name, Light, Dark},
#include "../../neverd-gui/app/ThemeColors.def"
};
} // namespace

AgentWindowFrame::AgentWindowFrame(QObject *parent) : QObject(parent) {}

AgentWindowFrame::~AgentWindowFrame() {
  NativeFilter.reset();
  finishManualGesture();
  showEdges({});
}

void AgentWindowFrame::attach(QWindow &window) {
  Window = &window;
  window.create();
  window.installEventFilter(this);
  NativeFilter = platform::createResizeFilter(
      window, [this](QPoint point) { return edgesAt(point); },
      [this] { prepareResize(); });
  connect(&window, &QWindow::windowStateChanged, this,
          &AgentWindowFrame::updateState);
  updateState();
}

int AgentWindowFrame::shadowMargin() const {
  return Window && Window->windowStates() &
                       (Qt::WindowMaximized | Qt::WindowFullScreen)
             ? 0
             : metrics::kWindowFrameShadowExtent;
}

int AgentWindowFrame::titleHeight() const { return metrics::kTitleBarHeight; }
int AgentWindowFrame::buttonWidth() const {
  return metrics::kTitleBarButtonWidth;
}
int AgentWindowFrame::dragDistance() const {
  return QGuiApplication::styleHints()->startDragDistance();
}
bool AgentWindowFrame::maximized() const {
  return Window && Window->windowStates().testFlag(Qt::WindowMaximized);
}

QColor AgentWindowFrame::chromeColor(const QString &name, bool light) const {
  for (const auto &color : ChromeColors)
    if (name == QLatin1String(color.Name))
      return QColor(QLatin1String(light ? color.Light : color.Dark));
  qWarning("Unknown Agent chrome color: %s", qPrintable(name));
  return {};
}

QColor AgentWindowFrame::themeColor(const QString &name, bool light) const {
  for (const auto &color : ThemeColors)
    if (name == QLatin1String(color.Name))
      return QColor(QLatin1String(light ? color.Light : color.Dark));
  qWarning("Unknown Agent theme color: %s", qPrintable(name));
  return {};
}

void AgentWindowFrame::updateState() {
  platform::configureFrame(*Window);
  const int margin = shadowMargin();
  platform::setShadowMargins(*Window, QMargins(margin, margin, margin, margin));
  emit stateChanged();
}

void AgentWindowFrame::minimize() { Window->showMinimized(); }

void AgentWindowFrame::toggleMaximized() {
  if (maximized())
    Window->showNormal();
  else
    Window->showMaximized();
}

void AgentWindowFrame::startMove() {
  if (!Window->startSystemMove()) {
    GestureOrigin = QCursor::pos();
    GestureGeometry = Window->geometry();
    Moving = true;
    Window->setMouseGrabEnabled(true);
  }
}

Qt::Edges AgentWindowFrame::edgesAt(QPoint point) const {
  if (!Window ||
      Window->windowStates() & (Qt::WindowMinimized | Qt::WindowFullScreen))
    return {};
  const int margin = shadowMargin();
  const QRect bounds(QPoint(), Window->size());
  const QRect visible = bounds.adjusted(margin, margin, -margin, -margin);
  constexpr int inset = metrics::kWindowFrameResizeInset;
  constexpr int corner = metrics::kWindowFrameCornerReach;
  const QRect inner = visible.adjusted(inset, inset, -inset, -inset);
  if (!bounds.contains(point) || inner.contains(point))
    return {};
  Qt::Edges edges;
  if (point.x() < inner.left())
    edges |= Qt::LeftEdge;
  else if (point.x() > inner.right())
    edges |= Qt::RightEdge;
  if (point.y() < inner.top())
    edges |= Qt::TopEdge;
  else if (point.y() > inner.bottom())
    edges |= Qt::BottomEdge;
  if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
    if (point.y() < visible.top() + corner)
      edges |= Qt::TopEdge;
    else if (point.y() > visible.bottom() - corner)
      edges |= Qt::BottomEdge;
  }
  if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
    if (point.x() < visible.left() + corner)
      edges |= Qt::LeftEdge;
    else if (point.x() > visible.right() - corner)
      edges |= Qt::RightEdge;
  }
  return edges;
}

void AgentWindowFrame::showEdges(Qt::Edges edges) {
  if (edges == ShownEdges)
    return;
  if (ShownEdges)
    QGuiApplication::restoreOverrideCursor();
  if (edges) {
    const bool horizontal = edges & (Qt::LeftEdge | Qt::RightEdge);
    const bool vertical = edges & (Qt::TopEdge | Qt::BottomEdge);
    Qt::CursorShape shape = horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor;
    if (horizontal && vertical)
      shape = bool(edges & Qt::LeftEdge) == bool(edges & Qt::TopEdge)
                  ? Qt::SizeFDiagCursor
                  : Qt::SizeBDiagCursor;
    QGuiApplication::setOverrideCursor(shape);
  }
  ShownEdges = edges;
}

void AgentWindowFrame::prepareResize() {
  showEdges({});
  if (!maximized())
    return;
  const QRect visible = Window->geometry();
  platform::restoreForResize(*Window);
  Window->showNormal();
  updateState();
  const int margin = shadowMargin();
  Window->setGeometry(visible.adjusted(-margin, -margin, margin, margin));
}

void AgentWindowFrame::finishManualGesture() {
  if (!Moving && !Resizing)
    return;
  Moving = false;
  Resizing = {};
  if (Window)
    Window->setMouseGrabEnabled(false);
}

bool AgentWindowFrame::eventFilter(QObject *object, QEvent *event) {
  if (object != Window)
    return false;
  if (event->type() == QEvent::Show)
    updateState();
  if (event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse) {
    finishManualGesture();
    showEdges({});
  }
  if (event->type() == QEvent::Leave)
    showEdges({});
  if (event->type() != QEvent::MouseMove &&
      event->type() != QEvent::MouseButtonPress &&
      event->type() != QEvent::MouseButtonRelease)
    return false;
  auto *mouse = static_cast<QMouseEvent *>(event);
  if (Moving || Resizing) {
    const QPoint delta = mouse->globalPosition().toPoint() - GestureOrigin;
    QRect next = GestureGeometry;
    if (Moving) {
      next.translate(delta);
    } else {
      if (Resizing & (Qt::LeftEdge | Qt::RightEdge)) {
        next.setWidth(
            std::clamp(GestureGeometry.width() +
                           (Resizing & Qt::LeftEdge ? -delta.x() : delta.x()),
                       Window->minimumWidth(), Window->maximumWidth()));
        if (Resizing & Qt::LeftEdge)
          next.moveRight(GestureGeometry.right());
      }
      if (Resizing & (Qt::TopEdge | Qt::BottomEdge)) {
        next.setHeight(
            std::clamp(GestureGeometry.height() +
                           (Resizing & Qt::TopEdge ? -delta.y() : delta.y()),
                       Window->minimumHeight(), Window->maximumHeight()));
        if (Resizing & Qt::TopEdge)
          next.moveBottom(GestureGeometry.bottom());
      }
    }
    Window->setGeometry(next);
    if (event->type() == QEvent::MouseButtonRelease)
      finishManualGesture();
    return true;
  }
  const auto edges = edgesAt(mouse->position().toPoint());
  if (event->type() == QEvent::MouseMove && mouse->buttons() == Qt::NoButton)
    showEdges(edges);
  if (event->type() != QEvent::MouseButtonPress ||
      mouse->button() != Qt::LeftButton || !edges)
    return false;
  prepareResize();
  if (!Window->startSystemResize(edges)) {
    Resizing = edges;
    GestureOrigin = mouse->globalPosition().toPoint();
    GestureGeometry = Window->geometry();
    Window->setMouseGrabEnabled(true);
  }
  return true;
}
} // namespace neverd::agent
