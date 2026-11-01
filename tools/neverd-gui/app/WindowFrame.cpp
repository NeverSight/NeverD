//===- WindowFrame.cpp - Owned window frame and resize gestures -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep the application frame consistent with native window transitions.
//===----------------------------------------------------------------------===//

#include "WindowFrame.h"

#include "../Config.h"
#include "Theme.h"
#include "TitleBar.h"
#include "WindowFramePlatform.h"

#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <algorithm>
#include <utility>

namespace neverd::gui {
namespace {
/// How far along an edge from a corner a press resizes both edges.
constexpr int CornerReach =
    ::neverd::limits::gui_config::kWindowFrameCornerReach;

Qt::CursorShape shapeFor(Qt::Edges edges) {
  const bool horizontal = edges & (Qt::LeftEdge | Qt::RightEdge);
  const bool vertical = edges & (Qt::TopEdge | Qt::BottomEdge);
  if (!vertical)
    return Qt::SizeHorCursor;
  if (!horizontal)
    return Qt::SizeVerCursor;
  // Top left and bottom right resize along the falling diagonal.
  const bool left = edges & Qt::LeftEdge;
  const bool top = edges & Qt::TopEdge;
  return left == top ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
}

/// Whether \p dialog takes its own frame: a dialog window that is about to
/// show for the first time and is neither a popup, such as the command
/// palette, nor frameless already.
bool takesFrame(const QDialog &dialog) {
  if (!dialog.isWindow() || dialog.isVisible() ||
      dialog.findChild<TitleBar *>(Qt::FindDirectChildrenOnly))
    return false;
  const Qt::WindowFlags flags = dialog.windowFlags();
  const Qt::WindowFlags type = flags & Qt::WindowType_Mask;
  return !(flags & Qt::FramelessWindowHint) && type != Qt::Popup &&
         type != Qt::ToolTip && type != Qt::SplashScreen;
}

/// Frames each dialog as it is first polished: Qt has created its native
/// window by then, but shows it only after the dialog has taken its size.
class DialogFrames final : public QObject {
public:
  using QObject::QObject;

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    if (event->type() == QEvent::Polish)
      if (auto *dialog = qobject_cast<QDialog *>(object);
          dialog && takesFrame(*dialog)) {
        // The frame resizes the dialog from its corners.
        dialog->setSizeGripEnabled(false);
        auto *bar = new TitleBar(*dialog);
        if (WindowFrame::attach(*dialog, bar))
          WindowFrame::fitTitle(*dialog, bar->sizeHint().width());
      }
    return QObject::eventFilter(object, event);
  }
};
} // namespace

void WindowFrame::prepareNativeWindows() {
  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QCoreApplication::setAttribute(Qt::AA_DontUseNativeMenuBar);
}

WindowFrame *WindowFrame::attach(QWidget &window, QWidget *titleBar) {
  // A native window that has not shown yet takes the flag as it is, without
  // being created again.
  window.overrideWindowFlags(window.windowFlags() | Qt::FramelessWindowHint |
                             Qt::NoDropShadowWindowHint);
  // Qt then paints no background of the window's own; the frame paints it
  // inside the shadow.
  window.setAttribute(Qt::WA_TranslucentBackground);
  // Qt keeps the format a created window had, so QtWayland would still
  // mark the whole surface opaque and GNOME would not draw what lies
  // under the shadow.
  if (QWindow *handle = window.windowHandle()) {
    QSurfaceFormat format = handle->requestedFormat();
    format.setAlphaBufferSize(8);
    handle->setFormat(format);
    // Windows chooses layered composition when the native flags change.
    // Set the alpha format first, including for dialogs with an early HWND.
    handle->setFlags(window.windowFlags());
  }
  // The native window exists from here, so its margins can be set before
  // the compositor first sizes it.  A dialog's parent keeps its children
  // without native windows of their own (prepareNativeWindows).
  window.winId();
  QWindow *handle = window.windowHandle();
  if (!handle)
    return nullptr;
  return new WindowFrame(window, *handle, titleBar);
}

void WindowFrame::fitTitle(QDialog &dialog, int width) {
  auto *box = qobject_cast<QMessageBox *>(&dialog);
  auto *text = box ? box->findChild<QLabel *>(QStringLiteral("qt_msgbox_label"))
                   : nullptr;
  if (!text || !dialog.layout()) {
    dialog.setMinimumWidth(std::max(dialog.minimumWidth(), width));
    return;
  }
  // Measure the text unwrapped, as the box does.  A rich text wraps from the
  // start: measured wrapped, its widest word would have become the text's
  // minimum width, and the box, which reads that minimum as the unwrapped
  // width, would have shown the text unwrapped and cut off at the title's
  // width.
  const bool wraps = text->wordWrap();
  text->setWordWrap(false);
  dialog.layout()->activate();
  const int missing = width - dialog.layout()->totalMinimumSize().width();
  if (missing > 0)
    text->setMinimumWidth(text->minimumSizeHint().width() + missing);
  text->setWordWrap(wraps);
}

void WindowFrame::frameDialogs(QApplication &app) {
  app.installEventFilter(new DialogFrames(&app));
}

WindowFrame::WindowFrame(QWidget &window, QWindow &handle, QWidget *titleBar)
    : QObject(&window), window_(window), handle_(handle), titleBar_(titleBar),
      content_(window.contentsMargins()), states_(handle.windowStates()) {
  window.installEventFilter(this);
  handle.installEventFilter(this);
  nativeResizeFilter_ = platform::createResizeFilter(
      handle, [this](QPoint at) { return resizeEdgesAt(at); },
      [this] { prepareResize(); });
  platform::configureFrame(handle);
  // Qt implements frameless Windows maximization by changing geometry, so
  // there need not be a native WindowStateChange event. Wayland and Cocoa
  // still need their native confirmation before changing frame margins.
  if (QGuiApplication::platformName() == QLatin1String("windows"))
    connect(&handle, &QWindow::windowStateChanged, this,
            &WindowFrame::syncWindowState);
  restoreTimer_ = new QTimer(this);
  restoreTimer_->setSingleShot(true);
  connect(restoreTimer_, &QTimer::timeout, this, [this] {
    restoreScreenTimedOut_ = true;
    restoreAfterFullScreen();
  });
  const auto watchScreen = [this](QScreen *screen) {
    disconnect(screenGeometryConnection_);
    if (screen)
      screenGeometryConnection_ =
          connect(screen, &QScreen::availableGeometryChanged, this,
                  &WindowFrame::restoreAfterFullScreen);
  };
  watchScreen(handle.screen());
  connect(&handle, &QWindow::screenChanged, this, [this, watchScreen] {
    watchScreen(handle_.screen());
    platform::configureFrame(handle_);
    updateMargins();
    restoreAfterFullScreen();
  });
  updateMargins();
}

WindowFrame::~WindowFrame() {
  nativeResizeFilter_.reset();
  finishResize();
  showEdges({});
}

bool WindowFrame::restoreWindowedGeometry(const QByteArray &geometry) {
  if (!window_.restoreGeometry(geometry))
    return false;
  startupGeometry_.clear();
  maximizeOnShow_ = window_.isMaximized();
  if (window_.isFullScreen() || window_.isMaximized()) {
    const QRect normal = window_.normalGeometry();
    window_.setWindowState(Qt::WindowNoState);
    window_.setGeometry(normal);
    // Start a frameless window normally before maximizing. On Windows the
    // hidden maximized path uses native maximization, whereas Qt's later
    // frameless restore expects the rectangle saved by its visible path.
    startupGeometry_ = geometry;
  }
  // QWidget defers hidden geometry changes until Show. This native window
  // already exists, and its pending resize events can arrive while startup
  // waits for Quick Start or the worker. Keep both rectangles in sync so an
  // old native size cannot replace the restored widget geometry.
  handle_.setGeometry(window_.geometry());
  return true;
}

QByteArray WindowFrame::saveWindowedGeometry() const {
  if (!startupGeometry_.isEmpty())
    return startupGeometry_;
  if ((window_.isFullScreen() || restoreMaximized_) &&
      !geometryBeforeFullScreen_.isEmpty())
    return geometryBeforeFullScreen_;
  return window_.saveGeometry();
}

void WindowFrame::setWindowState(Qt::WindowStates states) {
  restoreTimer_->stop();
  restoreMaximized_ = false;
  restoreScreenTimedOut_ = false;
  if (states.testFlag(Qt::WindowFullScreen) && !window_.isFullScreen()) {
    geometryBeforeFullScreen_ = window_.saveGeometry();
    beforeFullScreen_ = window_.windowState() & Qt::WindowMaximized;
    normalBeforeFullScreen_ = window_.normalGeometry();
    fullScreenScreen_ = handle_.screen();
    if (fullScreenScreen_) {
      fullScreenAvailable_ = fullScreenScreen_->availableGeometry();
      fullScreenScreenGeometry_ = fullScreenScreen_->geometry();
    }
    if (QGuiApplication::platformName() == QLatin1String("windows") &&
        beforeFullScreen_.testFlag(Qt::WindowMaximized)) {
      // Qt shares one saved rectangle between frameless maximization and
      // full screen. Enter through normal size to retain the restore frame.
      window_.showNormal();
      window_.showFullScreen();
      return;
    }
  } else if (window_.isFullScreen() && states == Qt::WindowMaximized &&
             (QGuiApplication::platformName() == QLatin1String("cocoa") ||
              QGuiApplication::platformName() == QLatin1String("windows"))) {
    // Cocoa restores the window before the Dock's work area. Requesting
    // maximization in the same transition makes Qt zoom against that temporary
    // area and overwrite the normal geometry with the old maximized frame.
    // Windows shares the saved geometry for frameless maximization and full
    // screen, then clears it on full-screen exit. Passing through normal size
    // also lets its next maximization retain the right restore geometry.
    restoreMaximized_ = true;
    window_.setWindowState(Qt::WindowNoState);
    restoreTimer_->start(
        ::neverd::limits::gui_config::kWindowFrameScreenRestoreTimeoutMs);
    return;
  }
  window_.setWindowState(states);
}

void WindowFrame::toggleFullScreen() {
  if (restoreMaximized_)
    return;
  setWindowState(window_.isFullScreen()
                     ? beforeFullScreen_
                     : window_.windowState() | Qt::WindowFullScreen);
}

void WindowFrame::restoreAfterFullScreen() {
  if (!restoreMaximized_ || states_.testFlag(Qt::WindowFullScreen))
    return;
  QScreen *screen = handle_.screen();
  if (!restoreScreenTimedOut_ && screen && screen == fullScreenScreen_ &&
      screen->geometry() == fullScreenScreenGeometry_ &&
      screen->availableGeometry() != fullScreenAvailable_)
    return;
  restoreMaximized_ = false;
  restoreTimer_->stop();
  if (!states_.testFlag(Qt::WindowMaximized) &&
      normalBeforeFullScreen_.isValid())
    window_.setGeometry(normalBeforeFullScreen_);
  window_.setWindowState(Qt::WindowMaximized);
}

Qt::Edges WindowFrame::edgesAt(QPoint at, QSize size, QMargins shadow) {
  const QRect window(QPoint(), size);
  const QRect visible = window.marginsRemoved(shadow);
  constexpr int inset = ::neverd::limits::gui_config::kWindowFrameResizeInset;
  const QRect inner = visible.adjusted(inset, inset, -inset, -inset);
  if (!window.contains(at) || inner.contains(at))
    return {};
  Qt::Edges edges;
  if (at.x() < inner.left())
    edges |= Qt::LeftEdge;
  else if (at.x() > inner.right())
    edges |= Qt::RightEdge;
  if (at.y() < inner.top())
    edges |= Qt::TopEdge;
  else if (at.y() > inner.bottom())
    edges |= Qt::BottomEdge;
  if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
    if (at.y() < visible.top() + CornerReach)
      edges |= Qt::TopEdge;
    else if (at.y() > visible.bottom() - CornerReach)
      edges |= Qt::BottomEdge;
  }
  if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
    if (at.x() < visible.left() + CornerReach)
      edges |= Qt::LeftEdge;
    else if (at.x() > visible.right() - CornerReach)
      edges |= Qt::RightEdge;
  }
  return edges;
}

bool WindowFrame::resizable() const {
  if (states_ & (Qt::WindowFullScreen | Qt::WindowMinimized))
    return false;
  // Linux window managers own maximized geometry and restore it
  // asynchronously. Keep their maximized edges inactive; Windows has a
  // native sizing loop, and Cocoa can infer zoom from a manual size limit.
  if (states_ & Qt::WindowMaximized) {
    const auto platform = QGuiApplication::platformName();
    if (platform != QLatin1String("windows") &&
        platform != QLatin1String("cocoa"))
      return false;
  }
  return window_.minimumSize() != window_.maximumSize();
}

Qt::Edges WindowFrame::resizeEdgesAt(QPoint at) const {
  auto edges =
      resizable() ? edgesAt(at, window_.size(), margins_) : Qt::Edges();
  if (window_.minimumWidth() == window_.maximumWidth())
    edges &= ~(Qt::LeftEdge | Qt::RightEdge);
  if (window_.minimumHeight() == window_.maximumHeight())
    edges &= ~(Qt::TopEdge | Qt::BottomEdge);
  return edges;
}

void WindowFrame::syncWindowState() {
  const auto states = handle_.windowStates();
  if (states == states_)
    return;
  // Cocoa can infer a maximized state when a drag reaches its size limit,
  // including a notification delivered after setGeometry returns. Keep the
  // held gesture so it can shrink back; full screen and minimization end it.
  if (states & (Qt::WindowFullScreen | Qt::WindowMinimized))
    finishResize();
  states_ = states;
  updateMargins();
  // Finish outside Cocoa's native state notification, after Qt has consumed it.
  if (restoreMaximized_)
    QTimer::singleShot(0, this, &WindowFrame::restoreAfterFullScreen);
}

bool WindowFrame::eventFilter(QObject *object, QEvent *event) {
  if (object == &window_) {
    if (event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse)
      finishResize();
    // Some systems make the native window's frame only as it first shows.
    if (event->type() == QEvent::Show)
      platform::configureFrame(handle_);
    else if (event->type() == QEvent::ShowToParent) {
      // QWidget sends ShowToParent after its native window has been shown.
      // Do not enter the hidden native-maximize path on the first Show.
      startupGeometry_.clear();
      if (std::exchange(maximizeOnShow_, false))
        setWindowState(Qt::WindowMaximized);
    } else if (event->type() == QEvent::Resize)
      placeTitleBar();
    else if (event->type() == QEvent::Paint)
      paint();
    return false;
  }
  if (object != &handle_)
    return false;
  // QWidget reports a requested state at once, before the window system
  // applies it; the native window receives this event only when the window
  // system has applied a state, such as with a Wayland configure.
  if (event->type() == QEvent::WindowStateChange) {
    syncWindowState();
    return false;
  }
  if (event->type() == QEvent::Leave) {
    showEdges({});
    return false;
  }
  if (event->type() != QEvent::MouseMove &&
      event->type() != QEvent::MouseButtonPress &&
      event->type() != QEvent::MouseButtonRelease)
    return false;
  auto *mouse = static_cast<QMouseEvent *>(event);
  if (resizing_) {
    if (event->type() == QEvent::MouseMove)
      resizeTo(mouse->globalPosition().toPoint());
    else if (event->type() == QEvent::MouseButtonRelease &&
             mouse->button() == Qt::LeftButton) {
      resizeTo(mouse->globalPosition().toPoint());
      finishResize();
    }
    return true;
  }
  const auto edges = resizeEdgesAt(mouse->position().toPoint());
  if (event->type() == QEvent::MouseMove) {
    if (mouse->buttons() == Qt::NoButton)
      showEdges(edges);
    return false;
  }
  if (event->type() != QEvent::MouseButtonPress ||
      mouse->button() != Qt::LeftButton || !edges)
    return false;
  prepareResize();
  if (!handle_.startSystemResize(edges)) {
    // Cocoa does not implement QWindow::startSystemResize. Keep the same
    // visible frame and bounds with an ordinary mouse grab there.
    resizing_ = edges;
    resizeOrigin_ = mouse->globalPosition().toPoint();
    resizeGeometry_ = window_.geometry();
    window_.grabMouse(shapeFor(edges));
  }
  return true;
}

void WindowFrame::prepareResize() {
  showEdges({});
  // AppKit also calls a manually sized window zoomed at its size ceiling.
  // The client resize path can shrink it directly, preserving the opposite
  // edges; restoring first would add shadow margins and move those edges.
  if (!(states_ & Qt::WindowMaximized) ||
      QGuiApplication::platformName() == QLatin1String("cocoa"))
    return;
  // Continue from the visible maximized rectangle, rather than jumping back
  // to the earlier normal size. The restored shadow lies outside this area.
  const QRect visible = window_.geometry().marginsRemoved(margins_);
  platform::restoreForResize(handle_);
  window_.showNormal();
  syncWindowState();
  platform::configureFrame(handle_);
  window_.setGeometry(visible.marginsAdded(margins_));
}

void WindowFrame::resizeTo(QPoint globalPosition) {
  const QPoint delta = globalPosition - resizeOrigin_;
  QRect next = resizeGeometry_;
  if (resizing_ & (Qt::LeftEdge | Qt::RightEdge)) {
    const int width =
        std::clamp(resizeGeometry_.width() +
                       (resizing_ & Qt::LeftEdge ? -delta.x() : delta.x()),
                   window_.minimumWidth(), window_.maximumWidth());
    next.setWidth(width);
    if (resizing_ & Qt::LeftEdge)
      next.moveRight(resizeGeometry_.right());
  }
  if (resizing_ & (Qt::TopEdge | Qt::BottomEdge)) {
    const int height =
        std::clamp(resizeGeometry_.height() +
                       (resizing_ & Qt::TopEdge ? -delta.y() : delta.y()),
                   window_.minimumHeight(), window_.maximumHeight());
    next.setHeight(height);
    if (resizing_ & Qt::TopEdge)
      next.moveBottom(resizeGeometry_.bottom());
  }
  window_.setGeometry(next);
}

void WindowFrame::finishResize() {
  if (!resizing_)
    return;
  resizing_ = {};
  if (QWidget::mouseGrabber() == &window_)
    window_.releaseMouse();
}

void WindowFrame::paint() {
  const auto &theme = Theme::instance();
  const QRect visible = window_.rect().marginsRemoved(margins_);
  QPainter painter(&window_);
  // Clear what an earlier frame left in the shadow; the style sheet's
  // background reaches the window through its palette.
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.fillRect(window_.rect(), Qt::transparent);
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
  painter.fillRect(visible, window_.palette().brush(window_.backgroundRole()));
  if (margins_.isNull())
    return;
  // Rings fading away from the edge, then the edge's hairline over them.
  const QColor shadow = theme.chrome(QStringLiteral("WindowShadow"));
  const QRectF edge = QRectF(visible).adjusted(-0.5, -0.5, 0.5, 0.5);
  for (int ring = ::neverd::limits::gui_config::kWindowFrameShadowExtent;
       ring >= 1; --ring) {
    const double fade =
        1.0 - double(ring - 1) /
                  ::neverd::limits::gui_config::kWindowFrameShadowExtent;
    QColor color = shadow;
    color.setAlphaF(shadow.alphaF() * fade * fade);
    painter.setPen(QPen(color, 1));
    painter.drawRect(edge.adjusted(-ring, -ring, ring, ring));
  }
  painter.setPen(QPen(theme.chrome(QStringLiteral("WindowEdge")), 1));
  painter.drawRect(edge);
}

void WindowFrame::updateMargins() {
  const bool framed = !(states_ & (Qt::WindowMaximized | Qt::WindowFullScreen));
  const QMargins margins =
      framed ? QMargins(::neverd::limits::gui_config::kWindowFrameShadowExtent,
                        ::neverd::limits::gui_config::kWindowFrameShadowExtent,
                        ::neverd::limits::gui_config::kWindowFrameShadowExtent,
                        ::neverd::limits::gui_config::kWindowFrameShadowExtent)
             : QMargins();
  if (margins != margins_) {
    margins_ = margins;
    platform::setShadowMargins(handle_, margins);
    window_.update();
  }
  const int titleHeight = titleBar_ ? titleBar_->sizeHint().height() : 0;
  const QMargins contents = content_ + margins + QMargins(0, titleHeight, 0, 0);
  if (window_.contentsMargins() != contents)
    window_.setContentsMargins(contents);
  placeTitleBar();
}

void WindowFrame::placeTitleBar() {
  if (!titleBar_)
    return;
  const QRect visible = window_.rect().marginsRemoved(margins_);
  titleBar_->setGeometry(visible.left(), visible.top(), visible.width(),
                         titleBar_->sizeHint().height());
  titleBar_->raise();
  titleBar_->show();
}

void WindowFrame::showEdges(Qt::Edges edges) {
  if (edges == shown_)
    return;
  if (shown_)
    QGuiApplication::restoreOverrideCursor();
  if (edges)
    QGuiApplication::setOverrideCursor(shapeFor(edges));
  shown_ = edges;
}

} // namespace neverd::gui
