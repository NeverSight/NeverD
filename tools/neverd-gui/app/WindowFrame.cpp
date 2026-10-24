#include "WindowFrame.h"

#include "../Config.h"
#include "SettingsKeys.h"
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
#include <QSettings>
#include <QSurfaceFormat>
#include <QWidget>
#include <QWindow>
#include <algorithm>

namespace neverd::gui {
namespace {
/// How far inside the visible edge a press still resizes the window.
constexpr int InnerEdge = 4;
/// How far along an edge from a corner a press resizes both edges.
constexpr int CornerReach = 16;

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
}

bool WindowFrame::enabled() {
  if (platform::frameKind() == platform::FrameKind::System)
    return false;
  return !QSettings().value(settings::NativeTitleBar).toBool();
}

WindowFrame *WindowFrame::attach(QWidget &window, QWidget *titleBar,
                                 Caption caption) {
  if (platform::frameKind() == platform::FrameKind::System ||
      (caption == Caption::UserPreference && !enabled()))
    return nullptr;
  const bool frameless =
      platform::frameKind() == platform::FrameKind::Frameless;
  const bool drawsShadow = frameless && platform::drawsShadow();
  // A native window that has not shown yet takes the flag as it is, without
  // being created again.
  if (frameless) {
    window.overrideWindowFlags(window.windowFlags() | Qt::FramelessWindowHint);
    if (QWindow *handle = window.windowHandle())
      handle->setFlags(handle->flags() | Qt::FramelessWindowHint);
  }
  // Qt then paints no background of the window's own; the frame paints it
  // inside the shadow.
  if (drawsShadow) {
    window.setAttribute(Qt::WA_TranslucentBackground);
    // Qt keeps the format a created window had, so QtWayland would still
    // mark the whole surface opaque and GNOME would not draw what lies
    // under the shadow.
    if (QWindow *handle = window.windowHandle()) {
      QSurfaceFormat format = handle->requestedFormat();
      format.setAlphaBufferSize(8);
      handle->setFormat(format);
    }
  }
  // The native window exists from here, so its margins can be set before
  // the compositor first sizes it.  A dialog's parent keeps its children
  // without native windows of their own (prepareNativeWindows).
  window.winId();
  QWindow *handle = window.windowHandle();
  if (!handle)
    return nullptr;
  return new WindowFrame(window, *handle, titleBar, frameless, drawsShadow);
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
  if (enabled())
    app.installEventFilter(new DialogFrames(&app));
}

WindowFrame::WindowFrame(QWidget &window, QWindow &handle, QWidget *titleBar,
                         bool frameless, bool drawsShadow)
    : QObject(&window), window_(window), handle_(handle), titleBar_(titleBar),
      frameless_(frameless), drawsShadow_(drawsShadow),
      content_(window.contentsMargins()), states_(handle.windowStates()) {
  window.installEventFilter(this);
  handle.installEventFilter(this);
  if (!frameless) {
    platform::removeCaption(handle);
    // The system frame's size follows the screen's scale.
    connect(&handle, &QWindow::screenChanged, this, [this] {
      platform::removeCaption(handle_);
      updateMargins();
    });
  }
  updateMargins();
}

WindowFrame::~WindowFrame() { showEdges({}); }

Qt::Edges WindowFrame::edgesAt(QPoint at, QSize size, QMargins shadow) {
  const QRect window(QPoint(), size);
  const QRect visible = window.marginsRemoved(shadow);
  const QRect inner =
      visible.adjusted(InnerEdge, InnerEdge, -InnerEdge, -InnerEdge);
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
  if (states_ &
      (Qt::WindowMaximized | Qt::WindowFullScreen | Qt::WindowMinimized))
    return false;
  return window_.minimumSize() != window_.maximumSize();
}

bool WindowFrame::eventFilter(QObject *object, QEvent *event) {
  if (object == &window_) {
    // Some systems make the native window's frame only as it first shows.
    if (event->type() == QEvent::Show && !frameless_)
      platform::removeCaption(handle_);
    else if (event->type() == QEvent::Resize)
      placeTitleBar();
    else if (event->type() == QEvent::Paint && drawsShadow_)
      paint();
    return false;
  }
  if (object != &handle_)
    return false;
  // QWidget reports a requested state at once, before the window system
  // applies it; the native window receives this event only when the window
  // system has applied a state, such as with a Wayland configure.
  if (event->type() == QEvent::WindowStateChange) {
    states_ = handle_.windowStates();
    updateMargins();
    return false;
  }
  if (event->type() == QEvent::Leave) {
    showEdges({});
    return false;
  }
  if (event->type() != QEvent::MouseMove &&
      event->type() != QEvent::MouseButtonPress)
    return false;
  auto *mouse = static_cast<QMouseEvent *>(event);
  Qt::Edges edges = resizable() ? edgesAt(mouse->position().toPoint(),
                                          window_.size(), margins_)
                                : Qt::Edges();
  // The system frame resizes the window from its other edges.
  if (!(edges & platform::ownResizeEdges()))
    edges = {};
  if (event->type() == QEvent::MouseMove) {
    if (mouse->buttons() == Qt::NoButton)
      showEdges(edges);
    return false;
  }
  if (mouse->button() != Qt::LeftButton || !edges)
    return false;
  showEdges({});
  return handle_.startSystemResize(edges);
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
      drawsShadow_ && framed
          ? QMargins(::neverd::limits::gui_config::kWindowFrameShadowExtent,
                     ::neverd::limits::gui_config::kWindowFrameShadowExtent,
                     ::neverd::limits::gui_config::kWindowFrameShadowExtent,
                     ::neverd::limits::gui_config::kWindowFrameShadowExtent)
          : QMargins();
  if (margins != margins_) {
    margins_ = margins;
    platform::setShadowMargins(handle_, margins);
    window_.update();
  }
  // A window made full screen from maximized is both; full screen has no
  // system border past the screen's edge.
  insets_ =
      !frameless_ && (states_ & (Qt::WindowMaximized | Qt::WindowFullScreen)) ==
                         Qt::WindowMaximized
          ? platform::maximizedInsets(handle_)
          : QMargins();
  const int titleHeight = titleBar_ ? titleBar_->sizeHint().height() : 0;
  const QMargins contents =
      content_ + margins + insets_ + QMargins(0, titleHeight, 0, 0);
  if (window_.contentsMargins() != contents)
    window_.setContentsMargins(contents);
  placeTitleBar();
}

void WindowFrame::placeTitleBar() {
  if (!titleBar_)
    return;
  const QRect visible = window_.rect().marginsRemoved(margins_ + insets_);
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
