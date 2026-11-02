//===- FocusSidebar.cpp - Collapsible focus navigation ------------------===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A small triangle toggles navigation without resizing the analysis panes.
//===----------------------------------------------------------------------===//

#include "FocusSidebar.h"

#include "../Config.h"
#include "Theme.h"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {
namespace config = ::neverd::limits::gui_config;

FocusSidebar::FocusSidebar(QWidget &host, QWidget &area, QWidget &content,
                           const QString &title)
    : QWidget(&host), host_(host), area_(&area),
      toggle_(new QToolButton(&host)), resizeHandle_(new QWidget(&host)),
      preferredWidth_(config::kFocusSidebarPreferredWidth) {
  setObjectName(QStringLiteral("focusFunctionsSidebar"));
  setAccessibleName(title);
  setFocusPolicy(Qt::NoFocus);
  auto *layout = new QVBoxLayout(this);
  const int border = config::kFocusSidebarBorderWidth;
  // Keep the triangle in the same place when open, clear of the function rows.
  layout->setContentsMargins(config::kFocusSidebarToggleWidth + border, border,
                             config::kDockingSeparatorLineWidth, border);
  layout->setSpacing(0);
  layout->addWidget(&content, 1);
  resizeHandle_->setObjectName(QStringLiteral("focusSidebarResizeHandle"));
  resizeHandle_->setAccessibleName(title);
  resizeHandle_->setFocusPolicy(Qt::TabFocus);
  resizeHandle_->setCursor(Qt::SplitHCursor);
  resizeHandle_->installEventFilter(this);
  toggle_->setObjectName(QStringLiteral("focusSidebarToggle"));
  toggle_->setAccessibleName(title);
  toggle_->setToolTip(title);
  toggle_->setFocusPolicy(Qt::TabFocus);
  toggle_->setCheckable(true);
  toggle_->setProperty("glyphToggle", true);
  toggle_->setArrowType(Qt::RightArrow);
  connect(toggle_, &QToolButton::toggled, this, [this](bool open) {
    if (open)
      reveal();
    else
      dismiss();
  });
  connect(&Theme::instance(), &Theme::changed, this, [this] {
    update();
    resizeHandle_->update();
  });
  host_.installEventFilter(this);
  area.installEventFilter(this);
  hide();
  resizeHandle_->hide();
  place();
  toggle_->show();
  toggle_->raise();
}

FocusSidebar::~FocusSidebar() {
  host_.removeEventFilter(this);
  if (area_)
    area_->removeEventFilter(this);
  delete toggle_.data();
  delete resizeHandle_;
}

void FocusSidebar::place() {
  if (!area_ || !toggle_)
    return;
  const QRect available(area_->mapTo(&host_, QPoint()), area_->size());
  const bool expanded = host_.isMaximized() || host_.isFullScreen();
  // Leave the restored window's resize grip clear of the navigation button.
  const int left = expanded ? 0
                            : std::max(available.left(),
                                       host_.contentsMargins().left() +
                                           config::kWindowFrameResizeInset);
  // The handle must stay clear of the frame's resize band, even at the maximum
  // width. Cocoa lets a zoomed window resize from an edge; putting the sidebar
  // handle there would resize the window instead of shrinking the sidebar.
  const int right = std::min(available.right() + 1,
                             host_.width() - host_.contentsMargins().right() -
                                 config::kWindowFrameResizeInset);
  const int maximumWidth = std::max(0, right - left);
  const int minimumWidth =
      std::min(config::kFocusSidebarMinimumWidth, maximumWidth);
  const int previousWidth = width();
  setGeometry(left, available.top(),
              std::clamp(preferredWidth_, minimumWidth, maximumWidth),
              available.height());
  // Like the dock separator, the grab area overlaps both neighbors while
  // only its hairline takes space in the layout.
  const int inset = (config::kDockingSeparatorGrabWidth -
                     config::kDockingSeparatorLineWidth) /
                    2;
  resizeHandle_->setGeometry(x() + width() -
                                 config::kDockingSeparatorLineWidth - inset,
                             y(), config::kDockingSeparatorGrabWidth, height());
  resizeHandle_->raise();
  if (width() != previousWidth)
    emit widthChanged(width());
  toggle_->setGeometry(
      left, available.center().y() - config::kFocusSidebarToggleHeight / 2,
      config::kFocusSidebarToggleWidth, config::kFocusSidebarToggleHeight);
  if (isVisible()) {
    raise();
    resizeHandle_->raise();
  }
  toggle_->raise();
}

void FocusSidebar::reveal() {
  if (!host_.isVisible() || !area_)
    return;
  if (!isVisible())
    previousFocus_ = QApplication::focusWidget();
  place();
  show();
  raise();
  resizeHandle_->show();
  resizeHandle_->raise();
  const QSignalBlocker blocker(toggle_);
  toggle_->setChecked(true);
  toggle_->setArrowType(Qt::LeftArrow);
  toggle_->raise();
}

void FocusSidebar::dismiss() {
  auto *focus = QApplication::focusWidget();
  const bool restore = focus &&
                       (isAncestorOf(focus) || focus == resizeHandle_) &&
                       host_.isActiveWindow();
  hide();
  resizeHandle_->hide();
  if (toggle_) {
    const QSignalBlocker blocker(toggle_);
    toggle_->setChecked(false);
    toggle_->setArrowType(Qt::RightArrow);
    toggle_->raise();
  }
  if (restore && previousFocus_ && previousFocus_->isVisible())
    previousFocus_->setFocus();
  previousFocus_.clear();
}

bool FocusSidebar::eventFilter(QObject *object, QEvent *event) {
  if (object == resizeHandle_) {
    switch (event->type()) {
    case QEvent::Paint: {
      QPainter painter(resizeHandle_);
      const int line = config::kDockingSeparatorLineWidth;
      const int inset = (config::kDockingSeparatorGrabWidth - line) / 2;
      const auto &theme = Theme::instance();
      painter.fillRect(QRect(inset, 0, line, resizeHandle_->height()),
                       theme.chrome(QStringLiteral("DockSeparator")));
      if (resizing_ || resizeHandle_->underMouse() ||
          resizeHandle_->hasFocus()) {
        const qreal highlight = config::kDockingSeparatorHighlightWidth;
        const qreal start = inset + (line - highlight) / 2;
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.chrome(QStringLiteral("DockSeparatorHover")));
        painter.drawRoundedRect(
            QRectF(start, 0, highlight, resizeHandle_->height()), highlight / 2,
            highlight / 2);
      }
      return true;
    }
    case QEvent::MouseButtonPress: {
      auto *mouse = static_cast<QMouseEvent *>(event);
      if (mouse->button() != Qt::LeftButton)
        break;
      resizing_ = true;
      resizeStartX_ = mouse->globalPosition().x();
      resizeStartWidth_ = width();
      resizeHandle_->update();
      return true;
    }
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease: {
      auto *mouse = static_cast<QMouseEvent *>(event);
      const bool release = event->type() == QEvent::MouseButtonRelease;
      if (!resizing_ || (release && mouse->button() != Qt::LeftButton))
        break;
      preferredWidth_ = resizeStartWidth_ +
                        qRound(mouse->globalPosition().x() - resizeStartX_);
      place();
      if (release) {
        resizing_ = false;
        preferredWidth_ = width();
      }
      resizeHandle_->update();
      return true;
    }
    case QEvent::KeyPress: {
      const int key = static_cast<QKeyEvent *>(event)->key();
      if (key != Qt::Key_Left && key != Qt::Key_Right)
        break;
      preferredWidth_ = width() + (key == Qt::Key_Left ? -1 : 1) *
                                      config::kFocusSidebarKeyboardResizeStep;
      place();
      return true;
    }
    case QEvent::Hide:
    case QEvent::UngrabMouse:
      resizing_ = false;
      [[fallthrough]];
    case QEvent::Enter:
    case QEvent::Leave:
    case QEvent::FocusIn:
    case QEvent::FocusOut:
      resizeHandle_->update();
      break;
    default:
      break;
    }
  }
  if (object == &host_ || object == area_) {
    switch (event->type()) {
    case QEvent::Resize:
    case QEvent::Move:
    case QEvent::LayoutRequest:
    case QEvent::WindowStateChange:
      place();
      break;
    default:
      break;
    }
  }
  return QWidget::eventFilter(object, event);
}

void FocusSidebar::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.fillRect(rect(), Theme::instance().chrome(QStringLiteral("Window")));
  painter.setPen(Theme::instance().chrome(QStringLiteral("Border")));
  painter.drawRect(rect().adjusted(0, 0, -config::kFocusSidebarBorderWidth,
                                   -config::kFocusSidebarBorderWidth));
}

} // namespace neverd::gui
