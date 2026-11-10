#include "TitleBar.h"

#include "../Config.h"
#include "DevicePixels.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QScopedValueRollback>
#include <QStyle>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace neverd::gui {
namespace {
/// An inactive window's icon fades as its text dims.
constexpr double InactiveOpacity = 0.6;
/// The style sheet dims the menus of an inactive window by this property.
constexpr char ActiveProperty[] = "active";

/// The window controls a window of \p flags offers.  Without
/// CustomizeWindowHint a dialog offers Close and other windows all three, as
/// window managers do.
struct Controls {
  bool minimize;
  bool maximize;
  bool close;
};

Controls controlsFor(Qt::WindowFlags flags) {
  if (!(flags & Qt::CustomizeWindowHint)) {
    const Qt::WindowFlags type = flags & Qt::WindowType_Mask;
    const bool window = type != Qt::Dialog && type != Qt::Tool;
    return {window || bool(flags & Qt::WindowMinimizeButtonHint),
            window || bool(flags & Qt::WindowMaximizeButtonHint), true};
  }
  return {bool(flags & Qt::WindowMinimizeButtonHint),
          bool(flags & Qt::WindowMaximizeButtonHint),
          bool(flags & Qt::WindowCloseButtonHint)};
}

/// The application icon's space, hidden in full screen.
int leftReserve(const QWidget &window, bool withIcon) {
  if (window.isFullScreen())
    return 0;
  return withIcon ? ::neverd::limits::gui_config::kTitleBarIconSlot : 0;
}

/// The width the menus need, in either direction.  QMenuBar mirrors its items
/// within its current width before measuring them, so a right-to-left bar
/// narrower than its items reports almost no width and never grows.
int menusWidth(const QMenuBar &menus) {
  const QStyle *style = menus.style();
  int width =
      2 * (style->pixelMetric(QStyle::PM_MenuBarPanelWidth, nullptr, &menus) +
           style->pixelMetric(QStyle::PM_MenuBarHMargin, nullptr, &menus));
  const int spacing =
      style->pixelMetric(QStyle::PM_MenuBarItemSpacing, nullptr, &menus);
  for (QAction *action : menus.actions())
    if (action->isVisible())
      width += spacing + menus.actionGeometry(action).width();
  return width;
}

QColor chrome(const char *name) {
  return Theme::instance().chrome(QLatin1String(name));
}

QColor titleText(const QWidget &window) {
  return chrome(window.isActiveWindow() ? "TitleBarText"
                                        : "TitleBarInactiveText");
}

/// A button that repaints as the pointer enters and leaves it.
class HoverButton : public QAbstractButton {
public:
  HoverButton(QWidget &window, QWidget *parent)
      : QAbstractButton(parent), window_(window) {
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
  }

protected:
  bool event(QEvent *event) override {
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave ||
        event->type() == QEvent::HoverEnter ||
        event->type() == QEvent::HoverLeave)
      update();
    return QAbstractButton::event(event);
  }
  bool lit() const { return underMouse() || isDown(); }

  QWidget &window_;
};

/// A window control: a light wash under the pointer, red behind Close.
class WindowButton final : public HoverButton {
public:
  using Kind = TitleBar::WindowControl;
  WindowButton(Kind kind, QWidget &window, QWidget *parent)
      : HoverButton(window, parent), kind_(kind) {}
  QSize sizeHint() const override {
    return {::neverd::limits::gui_config::kTitleBarButtonWidth,
            ::neverd::limits::gui_config::kTitleBarHeight};
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    TitleBar::paintWindowControl(painter, *this, window_, kind_);
  }

private:
  const Kind kind_;
};

/// The command center: a quiet field with a magnifier and the open file's
/// name, which runs a command when clicked.
class CommandCenter final : public HoverButton {
public:
  using HoverButton::HoverButton;

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    {
      // The field's outline on whole device pixels.
      DevicePixels device(painter);
      painter.setPen(QPen(
          chrome(lit() ? "CommandCenterHoverBorder" : "CommandCenterBorder"),
          device.width(1)));
      painter.setBrush(chrome(lit() ? "CommandCenterHover" : "CommandCenter"));
      const qreal radius =
          device.length(::neverd::limits::gui_config::kTitleBarCenterRadius);
      painter.drawRoundedRect(device.outline(rect(), 1), radius, radius);
    }
    // The magnifier and the name, centered together; a long name keeps both
    // of its ends.
    const QFontMetrics metrics(font());
    const QString name = metrics.elidedText(
        text(), Qt::ElideMiddle,
        width() - 2 * ::neverd::limits::gui_config::kTitleBarCenterPadding -
            ::neverd::limits::gui_config::kTitleBarLensSize -
            ::neverd::limits::gui_config::kTitleBarLensGap);
    const int content = ::neverd::limits::gui_config::kTitleBarLensSize +
                        ::neverd::limits::gui_config::kTitleBarLensGap +
                        metrics.horizontalAdvance(name);
    const bool reversed = isRightToLeft();
    const double start = std::round((width() - content) / 2.0);
    // The magnifier leads the name: on its left, or on its right in a
    // right-to-left language.
    const double left =
        reversed
            ? width() - start - ::neverd::limits::gui_config::kTitleBarLensSize
            : start;
    const double middle = height() / 2.0;
    const QColor ink = titleText(window_);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, 1.2));
    const QRectF lens(left + 0.5, middle - 5.5, 8, 8);
    painter.drawEllipse(lens);
    painter.drawLine(
        QPointF(lens.right() - 1.2, lens.bottom() - 1.2),
        QPointF(left + ::neverd::limits::gui_config::kTitleBarLensSize - 0.5,
                middle + 5.5));
    painter.setPen(ink);
    const int nameWidth = metrics.horizontalAdvance(name);
    const double nameLeft =
        reversed
            ? left - ::neverd::limits::gui_config::kTitleBarLensGap - nameWidth
            : left + ::neverd::limits::gui_config::kTitleBarLensSize +
                  ::neverd::limits::gui_config::kTitleBarLensGap;
    painter.drawText(QRectF(nameLeft, 0, nameWidth, height()),
                     Qt::AlignAbsolute | Qt::AlignLeft | Qt::AlignVCenter,
                     name);
  }
};
/// A window control's glyph at a fractional scale, its one-pixel strokes on
/// whole device pixels: the same lines in a box \p origin's glyph size wide.
void paintSnappedGlyph(QPainter &painter, QPoint origin, const QColor &glyph,
                       TitleBar::WindowControl control, bool maximized) {
  constexpr int size = ::neverd::limits::gui_config::kTitleBarGlyphSize;
  const auto stroke = [&](int x, int y, int width, int height,
                          Qt::Edges edges) {
    paintDeviceBox(painter, QRect(origin + QPoint(x, y), QSize(width, height)),
                   DeviceBox{QColor(), glyph, 1, edges});
  };
  const Qt::Edges all =
      Qt::TopEdge | Qt::LeftEdge | Qt::RightEdge | Qt::BottomEdge;
  switch (control) {
  case TitleBar::WindowControl::Minimize:
    stroke(0, 5, size, 1, Qt::TopEdge);
    break;
  case TitleBar::WindowControl::Maximize:
    if (maximized) {
      // Restore: the window, and the edges of the one behind it.
      stroke(0, 2, size - 2, size - 2, all);
      stroke(2, 0, size - 2, size - 2, Qt::TopEdge | Qt::RightEdge);
      stroke(2, 0, size - 2, 2, Qt::LeftEdge);
      stroke(size - 2, 0, 2, size - 2, Qt::BottomEdge);
    } else {
      stroke(0, 0, size, size, all);
    }
    break;
  case TitleBar::WindowControl::Close: {
    DevicePixels device(painter);
    const QRect box = device.rect(QRect(origin, QSize(size, size)));
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(glyph, device.width(1)));
    const QPointF topLeft = box.topLeft();
    const QPointF bottomRight = topLeft + QPointF(box.width(), box.height());
    painter.drawLine(topLeft, bottomRight);
    painter.drawLine(QPointF(bottomRight.x(), topLeft.y()),
                     QPointF(topLeft.x(), bottomRight.y()));
    break;
  }
  }
}
} // namespace

void TitleBar::paintWindowControl(QPainter &painter,
                                  const QAbstractButton &button,
                                  const QWidget &window,
                                  WindowControl control) {
  const bool close = control == WindowControl::Close;
  // The fill reaches every device pixel of the button at any scale.
  if (button.isDown())
    paintDeviceBox(
        painter, button.rect(),
        DeviceBox{chrome(close ? "WindowClosePressed" : "WindowControlPressed"),
                  QColor(), 0});
  else if (button.underMouse())
    paintDeviceBox(
        painter, button.rect(),
        DeviceBox{chrome(close ? "WindowCloseHover" : "WindowControlHover"),
                  QColor(), 0});
  const QColor glyph = close && (button.underMouse() || button.isDown())
                           ? chrome("WindowCloseText")
                           : titleText(window);
  painter.setPen(QPen(glyph, 1));
  // Whole pixels, so the glyph's one-pixel strokes stay sharp.
  const double left = std::round(
      (button.width() - ::neverd::limits::gui_config::kTitleBarGlyphSize) /
      2.0);
  const double top = std::round(
      (button.height() - ::neverd::limits::gui_config::kTitleBarGlyphSize) /
      2.0);
  if (DevicePixels::snaps(painter)) {
    paintSnappedGlyph(painter, QPoint(int(left), int(top)), glyph, control,
                      window.isMaximized());
    return;
  }
  switch (control) {
  case WindowControl::Minimize:
    painter.drawLine(
        QPointF(left, top + 5.5),
        QPointF(left + ::neverd::limits::gui_config::kTitleBarGlyphSize,
                top + 5.5));
    break;
  case WindowControl::Maximize:
    if (window.isMaximized()) {
      // Restore: the window, and the one behind it.
      painter.drawRect(QRectF(left + 0.5, top + 2.5, 7, 7));
      const QPointF back[] = {{left + 2.5, top + 2.5},
                              {left + 2.5, top + 0.5},
                              {left + 9.5, top + 0.5},
                              {left + 9.5, top + 7.5},
                              {left + 7.5, top + 7.5}};
      painter.drawPolyline(back, std::size(back));
    } else {
      painter.drawRect(QRectF(left + 0.5, top + 0.5, 9, 9));
    }
    break;
  case WindowControl::Close:
    painter.setRenderHint(QPainter::Antialiasing);
    painter.drawLine(
        QPointF(left, top),
        QPointF(left + ::neverd::limits::gui_config::kTitleBarGlyphSize,
                top + ::neverd::limits::gui_config::kTitleBarGlyphSize));
    painter.drawLine(
        QPointF(left + ::neverd::limits::gui_config::kTitleBarGlyphSize, top),
        QPointF(left, top + ::neverd::limits::gui_config::kTitleBarGlyphSize));
    break;
  }
}

TitleBar::TitleBar(QWidget &window, QMenuBar *menuBar)
    : QWidget(&window), window_(window), menuBar_(menuBar),
      minimize_(new WindowButton(WindowButton::Kind::Minimize, window, this)),
      maximize_(new WindowButton(WindowButton::Kind::Maximize, window, this)),
      close_(new WindowButton(WindowButton::Kind::Close, window, this)) {
  setObjectName(QStringLiteral("titleBar"));
  if (menuBar) {
    menuBar->setParent(this);
    commandCenter_ = new CommandCenter(window, this);
    commandCenter_->setObjectName(QStringLiteral("commandCenter"));
    connect(commandCenter_, &QAbstractButton::clicked, this, [this] {
      if (!command_)
        return;
      const QScopedValueRollback<QPointer<QWidget>> clicked(clicked_,
                                                            commandCenter_);
      command_->trigger();
    });
  }
  minimize_->setObjectName(QStringLiteral("minimizeButton"));
  maximize_->setObjectName(QStringLiteral("maximizeButton"));
  close_->setObjectName(QStringLiteral("closeButton"));
  connect(minimize_, &QAbstractButton::clicked, &window,
          &QWidget::showMinimized);
  connect(maximize_, &QAbstractButton::clicked, this,
          &TitleBar::toggleMaximized);
  connect(close_, &QAbstractButton::clicked, &window, &QWidget::close);
  connect(&Theme::instance(), &Theme::changed, this, &TitleBar::repaintAll);
  window.installEventFilter(this);
  retranslateUi();
  updateActive();
  updateWindowState();
}

void TitleBar::setCommandCenterText(const QString &text,
                                    const QString &toolTip) {
  if (!commandCenter_)
    return;
  commandCenter_->setText(text);
  commandCenter_->setToolTip(toolTip);
  commandCenter_->setAccessibleName(text);
  commandCenter_->update();
}

void TitleBar::setCommandCenterAction(QAction *action) {
  if (command_ && commandCenter_)
    disconnect(command_, nullptr, commandCenter_, nullptr);
  command_ = action;
  if (!commandCenter_)
    return;
  const auto updateDescription = [this] {
    commandCenter_->setAccessibleDescription(command_ ? command_->toolTip()
                                                      : QString());
  };
  if (action)
    connect(action, &QAction::changed, commandCenter_, updateDescription);
  updateDescription();
}

QSize TitleBar::sizeHint() const {
  const int height = ::neverd::limits::gui_config::kTitleBarHeight;
  const int buttons = 3 * ::neverd::limits::gui_config::kTitleBarButtonWidth;
  if (!menuBar_)
    return {leftReserve(window_, false) +
                2 * ::neverd::limits::gui_config::kTitleBarTitlePadding +
                fontMetrics().horizontalAdvance(window_.windowTitle()) +
                buttons,
            height};
  return {leftReserve(window_, true) + menusWidth(*menuBar_) +
              2 * ::neverd::limits::gui_config::kTitleBarGap +
              ::neverd::limits::gui_config::kTitleBarCenterMaxWidth + buttons,
          height};
}

QSize TitleBar::minimumSizeHint() const {
  const int buttons = 3 * ::neverd::limits::gui_config::kTitleBarButtonWidth;
  return {leftReserve(window_, menuBar_ != nullptr) +
              ::neverd::limits::gui_config::kTitleBarTitlePadding + buttons,
          ::neverd::limits::gui_config::kTitleBarHeight};
}

bool TitleBar::event(QEvent *event) {
  // The menus' width changes with their language and the window's menus, and
  // the bar mirrors with the language's direction.
  if (event->type() == QEvent::LayoutRequest ||
      event->type() == QEvent::LayoutDirectionChange)
    arrange();
  return QWidget::event(event);
}

bool TitleBar::eventFilter(QObject *object, QEvent *event) {
  if (object == &window_) {
    if (event->type() == QEvent::ActivationChange)
      updateActive();
    else if (event->type() == QEvent::WindowStateChange)
      updateWindowState();
    else if (event->type() == QEvent::WindowTitleChange)
      update();
  }
  return QWidget::eventFilter(object, event);
}

void TitleBar::changeEvent(QEvent *event) {
  if (event->type() == QEvent::LanguageChange)
    retranslateUi();
  QWidget::changeEvent(event);
}

void TitleBar::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton)
    pressAt_ = event->position().toPoint();
  QWidget::mousePressEvent(event);
}

void TitleBar::mouseMoveEvent(QMouseEvent *event) {
  // The window moves once the pointer leaves the press's click distance, so
  // a double click still reaches the bar.
  if (pressAt_ && event->buttons() & Qt::LeftButton &&
      (event->position().toPoint() - *pressAt_).manhattanLength() >=
          QApplication::startDragDistance()) {
    pressAt_.reset();
    if (QWindow *handle = window_.windowHandle())
      handle->startSystemMove();
    return;
  }
  QWidget::mouseMoveEvent(event);
}

void TitleBar::mouseReleaseEvent(QMouseEvent *event) {
  pressAt_.reset();
  QWidget::mouseReleaseEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton || window_.isFullScreen()) {
    QWidget::mouseDoubleClickEvent(event);
    return;
  }
  pressAt_.reset();
  if (maximizable())
    toggleMaximized();
}

void TitleBar::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.fillRect(rect(), chrome("TitleBar"));
  if (!menuBar_) {
    // A dialog's title, centered in the bar while it fits beside the
    // buttons.
    const int start = leftReserve(window_, false) +
                      ::neverd::limits::gui_config::kTitleBarTitlePadding;
    const int room = buttonsLeft_ -
                     ::neverd::limits::gui_config::kTitleBarTitlePadding -
                     start;
    const QString title =
        fontMetrics().elidedText(window_.windowTitle(), Qt::ElideRight, room);
    const int titleWidth = fontMetrics().horizontalAdvance(title);
    const int x = std::clamp(
        (width() - titleWidth) / 2, start,
        std::max(start,
                 buttonsLeft_ -
                     ::neverd::limits::gui_config::kTitleBarTitlePadding -
                     titleWidth));
    painter.setPen(titleText(window_));
    painter.drawText(QStyle::visualRect(layoutDirection(), rect(),
                                        QRect(x, 0, titleWidth, height())),
                     Qt::AlignAbsolute | Qt::AlignLeft | Qt::AlignVCenter,
                     title);
    return;
  }
  if (window_.isFullScreen())
    return;
  if (!window_.isActiveWindow())
    painter.setOpacity(InactiveOpacity);
  window_.windowIcon().paint(
      &painter,
      QStyle::visualRect(
          layoutDirection(), rect(),
          QRect((::neverd::limits::gui_config::kTitleBarIconSlot -
                 ::neverd::limits::gui_config::kTitleBarIconSize) /
                    2,
                (height() - ::neverd::limits::gui_config::kTitleBarIconSize) /
                    2,
                ::neverd::limits::gui_config::kTitleBarIconSize,
                ::neverd::limits::gui_config::kTitleBarIconSize)));
}

void TitleBar::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  arrange();
}

void TitleBar::arrange() {
  // Lay the bar out left to right, then mirror it for a right-to-left
  // language: the window controls lead on the left, the icon and the menus on
  // the right.
  const auto place = [this](QWidget *child, const QRect &leftToRight) {
    child->setGeometry(
        QStyle::visualRect(layoutDirection(), rect(), leftToRight));
  };
  int right = width();
  for (QAbstractButton *button : {close_, maximize_, minimize_}) {
    if (button->isHidden())
      continue;
    right -= ::neverd::limits::gui_config::kTitleBarButtonWidth;
    place(button,
          QRect(right, 0, ::neverd::limits::gui_config::kTitleBarButtonWidth,
                height()));
  }
  buttonsLeft_ = right;
  if (!menuBar_)
    return;
  const int left = leftReserve(window_, true);
  const int menusHeight = menuBar_->sizeHint().height();
  const int menuWidth = std::clamp(
      menusWidth(*menuBar_), 0,
      std::max(0, right - left - ::neverd::limits::gui_config::kTitleBarGap));
  place(menuBar_,
        QRect(left, (height() - menusHeight) / 2, menuWidth, menusHeight));
  const int start =
      left + menuWidth + ::neverd::limits::gui_config::kTitleBarGap;
  const int end = right - ::neverd::limits::gui_config::kTitleBarGap;
  const int centerWidth = std::min(
      {::neverd::limits::gui_config::kTitleBarCenterMaxWidth,
       width() * ::neverd::limits::gui_config::kTitleBarCenterWidthPercent /
           100,
       end - start});
  if (centerWidth < ::neverd::limits::gui_config::kTitleBarCenterMinWidth) {
    commandCenter_->hide();
    return;
  }
  const int x =
      std::clamp((width() - centerWidth) / 2, start, end - centerWidth);
  place(commandCenter_,
        QRect(x,
              (height() - ::neverd::limits::gui_config::kTitleBarCenterHeight) /
                  2,
              centerWidth,
              ::neverd::limits::gui_config::kTitleBarCenterHeight));
  commandCenter_->show();
}

bool TitleBar::maximizable() const {
  return controlsFor(window_.windowFlags()).maximize &&
         window_.minimumSize() != window_.maximumSize();
}

void TitleBar::toggleMaximized() {
  if (window_.isMaximized())
    window_.showNormal();
  else
    window_.showMaximized();
}

void TitleBar::updateWindowState() {
  // Full screen hides the controls on every desktop.
  const bool shown = !window_.isFullScreen();
  const Controls controls = controlsFor(window_.windowFlags());
  minimize_->setVisible(controls.minimize && shown);
  maximize_->setVisible(maximizable() && shown);
  close_->setVisible(controls.close && shown);
  retranslateUi();
  arrange();
  repaintAll();
}

void TitleBar::updateActive() {
  setProperty(ActiveProperty, window_.isActiveWindow());
  // The menus' color hangs on this property in the style sheet.
  if (menuBar_) {
    menuBar_->style()->unpolish(menuBar_);
    menuBar_->style()->polish(menuBar_);
  }
  repaintAll();
}

void TitleBar::retranslateUi() {
  const auto describe = [](QAbstractButton *button, const QString &text) {
    button->setToolTip(text);
    button->setAccessibleName(text);
  };
  describe(minimize_, tr("Minimize"));
  describe(maximize_, window_.isMaximized() ? tr("Restore") : tr("Maximize"));
  describe(close_, tr("Close"));
}

void TitleBar::repaintAll() {
  update();
  for (QWidget *child : findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
    child->update();
}

} // namespace neverd::gui
