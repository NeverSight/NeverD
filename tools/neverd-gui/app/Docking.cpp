//===- Docking.cpp - Workbench docking presentation ----------------------===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep docking chrome and native floating-window interactions consistent.
//===----------------------------------------------------------------------===//

#include "Docking.h"

#include "../Config.h"
#include "Icons.h"
#include "Theme.h"
#include "WindowFrame.h"

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHoverEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QStyle>
#include <QTimer>
#include <QVariantAnimation>
#include <QWindow>
#include <cmath>
#include <kddockwidgets/Config.h>
#include <kddockwidgets/KDDockWidgets.h>
#include <kddockwidgets/core/DockWidget.h>
#include <kddockwidgets/core/Draggable_p.h>
#include <kddockwidgets/core/FloatingWindow.h>
#include <kddockwidgets/core/MainWindow.h>
#include <kddockwidgets/core/Separator.h>
#include <kddockwidgets/core/TabBar.h>
#include <kddockwidgets/core/TitleBar.h>
#include <kddockwidgets/qtcommon/View.h>
#include <kddockwidgets/qtwidgets/ViewFactory.h>
#include <kddockwidgets/qtwidgets/views/FloatingWindow.h>
#include <kddockwidgets/qtwidgets/views/Group.h>
#include <kddockwidgets/qtwidgets/views/Separator.h>
#include <kddockwidgets/qtwidgets/views/TabBar.h>
#include <kddockwidgets/qtwidgets/views/TitleBar.h>

namespace neverd::gui {
namespace {
bool onWayland() {
  return QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
}

// A pointer that only crosses a separator does not light it up.
constexpr int SeparatorHoverDelayMs = 300;
constexpr int SeparatorFadeInMs = 150;
constexpr int SeparatorFadeOutMs = 100;
// Docked windows reach the window's sides and bottom.  The gap above the
// first row of tabs keeps inactive tabs apart from the toolbar of their color.
constexpr QMargins DockAreaMargins(0, 5, 0, 0);

QPointer<KDDockWidgets::QtWidgets::FloatingWindow> DockDragSource;
Qt::WindowStates DockDragWindowState;

bool beginDockDrag(KDDockWidgets::Core::Draggable *draggable) {
  if (!onWayland())
    return true;
  auto *view = KDDockWidgets::QtCommon::View_qt::asQWidget(draggable->asView());
  auto *floating = qobject_cast<KDDockWidgets::QtWidgets::FloatingWindow *>(
      view ? view->window() : nullptr);
  if (!floating || !draggable->isWindow())
    return true;
  DockDragSource = floating;
  DockDragWindowState = floating->windowHandle()->windowStates();
  // Wayland leaves the source in place while QDrag moves its snapshot. It
  // would cover both the workbench's drop targets and their preview. Minimize
  // only after QDrag has its pointer serial and snapshot; unlike hide/show,
  // this retains the surface and compositor-owned position.
  // A pane dragged within a multi-group floating window still needs that
  // window's other groups as drop targets, so only whole-window drags do this.
  const auto source = DockDragSource;
  QTimer::singleShot(0, floating, [source] {
    if (source && source == DockDragSource)
      source->windowHandle()->setWindowStates(DockDragWindowState |
                                              Qt::WindowMinimized);
  });
  return true;
}

void endDockDrag() {
  const auto source = DockDragSource;
  DockDragSource.clear();
  if (source && !source->floatingWindow()->beingDeleted()) {
    source->windowHandle()->setWindowStates(DockDragWindowState);
    // Removing WindowMinimized alone does not unminimize a Wayland toplevel.
    source->windowHandle()->requestActivate();
  }
}

/// A docked window draws no frame: neighbors meet at a separator's hairline.
class DockGroup final : public KDDockWidgets::QtWidgets::Group {
public:
  using Group::Group;

protected:
  void paintEvent(QPaintEvent *) override {}
};

/// The hairline between docked windows.  It lights up in the focus color
/// while it is hovered or dragged.
class DockSeparator final : public KDDockWidgets::QtWidgets::Separator {
public:
  DockSeparator(KDDockWidgets::Core::Separator *controller,
                KDDockWidgets::Core::View *parent)
      : Separator(controller, parent) {
    hoverDelay_.setSingleShot(true);
    hoverDelay_.setInterval(SeparatorHoverDelayMs);
    connect(&hoverDelay_, &QTimer::timeout, this,
            [this] { setHighlighted(true); });
    fade_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&fade_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
              highlight_ = value.toReal();
              update();
            });
    connect(&Theme::instance(), &Theme::changed, this, [this] { update(); });
  }

protected:
  void paintEvent(QPaintEvent *) override {
    const auto &theme = Theme::instance();
    const auto &config = KDDockWidgets::Config::self();
    // The layout's gap, on which the grab area is centered.
    const int line = config.layoutSpacing();
    const int inset = (config.separatorThickness() - line) / 2;
    QPainter painter(this);
    painter.fillRect(horizontal() ? QRect(0, inset, width(), line)
                                  : QRect(inset, 0, line, height()),
                     theme.chrome(QStringLiteral("DockSeparator")));
    if (highlight_ <= 0)
      return;
    QColor accent = theme.chrome(QStringLiteral("DockSeparatorHover"));
    accent.setAlphaF(accent.alphaF() * highlight_);
    const qreal start =
        inset +
        (line - ::neverd::limits::gui_config::kDockingSeparatorHighlightWidth) /
            2;
    const QRectF bar =
        horizontal()
            ? QRectF(
                  0, start, width(),
                  ::neverd::limits::gui_config::kDockingSeparatorHighlightWidth)
            : QRectF(
                  start, 0,
                  ::neverd::limits::gui_config::kDockingSeparatorHighlightWidth,
                  height());
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(accent);
    painter.drawRoundedRect(
        bar, ::neverd::limits::gui_config::kDockingSeparatorHighlightWidth / 2,
        ::neverd::limits::gui_config::kDockingSeparatorHighlightWidth / 2);
  }

  void enterEvent(QEnterEvent *event) override {
    Separator::enterEvent(event);
    // The cursors of Qt's own splitters and of GTK's panes.
    setCursor(horizontal() ? Qt::SplitVCursor : Qt::SplitHCursor);
    if (pressed_)
      setHighlighted(true);
    else
      hoverDelay_.start();
  }

  void leaveEvent(QEvent *event) override {
    Separator::leaveEvent(event);
    hoverDelay_.stop();
    if (!pressed_)
      setHighlighted(false);
  }

  void mousePressEvent(QMouseEvent *event) override {
    Separator::mousePressEvent(event);
    if (event->button() != Qt::LeftButton)
      return;
    pressed_ = true;
    hoverDelay_.stop();
    setHighlighted(true);
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    Separator::mouseMoveEvent(event);
    // The layout ends a drag whose release it never saw.
    if (pressed_ && !KDDockWidgets::Core::Separator::isResizing())
      release(event->position().toPoint());
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    Separator::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton)
      release(event->position().toPoint());
  }

private:
  bool horizontal() const { return width() > height(); }

  void release(QPoint position) {
    pressed_ = false;
    if (!rect().contains(position))
      setHighlighted(false);
  }

  void setHighlighted(bool on) {
    const qreal target = on ? 1 : 0;
    if (target == target_)
      return;
    target_ = target;
    fade_.stop();
    fade_.setStartValue(highlight_);
    fade_.setEndValue(target);
    // A fade that reverses midway takes only as long as the way back.
    const int duration = on ? SeparatorFadeInMs : SeparatorFadeOutMs;
    fade_.setDuration(qRound(duration * std::abs(target - highlight_)));
    fade_.start();
  }

  QTimer hoverDelay_;
  QVariantAnimation fade_;
  qreal highlight_ = 0;
  qreal target_ = 0;
  bool pressed_ = false;
};

/// A row of docked tabs.  Only the current tab and the one under the pointer
/// show their close buttons, as in Visual Studio Code: a cross on every tab
/// competes with the titles.  A hidden button keeps its room, so the tabs stay
/// put when it appears.
class DockTabBar final : public KDDockWidgets::QtWidgets::TabBar {
public:
  DockTabBar(KDDockWidgets::Core::TabBar *controller, QWidget *parent)
      : TabBar(controller, parent) {
    connect(this, &QTabBar::currentChanged, this,
            [this] { updateCloseButtons(); });
  }

protected:
  void contextMenuEvent(QContextMenuEvent *event) override {
    const int index = event->reason() == QContextMenuEvent::Keyboard
                          ? currentIndex()
                          : tabAt(event->pos());
    auto *dock = tabBar()->dockWidgetAt(index);
    if (!dock || !dock->isFloating() || !dock->hasPreviousDockedLocation())
      return;
    bool dockBack = false;
    {
      QMenu menu(this);
      auto *restore = menu.addAction(
          QCoreApplication::translate("Docking", "Dock back into main window"));
      restore->setObjectName(QStringLiteral("dockBack"));
      dockBack = menu.exec(event->globalPos()) == restore;
    }
    // Docking can delete this tab bar and its floating parent.
    if (dockBack)
      dock->setFloating(false);
  }

  bool event(QEvent *event) override {
    const bool handled = TabBar::event(event);
    switch (event->type()) {
    case QEvent::HoverEnter:
    case QEvent::HoverMove:
      setHoveredTab(
          tabAt(static_cast<QHoverEvent *>(event)->position().toPoint()));
      break;
    case QEvent::HoverLeave:
      setHoveredTab(-1);
      break;
    case QEvent::Show:
      updateCloseButtons();
      break;
    default:
      break;
    }
    return handled;
  }

  void tabInserted(int index) override {
    TabBar::tabInserted(index);
    updateCloseButtons();
  }

  void tabRemoved(int index) override {
    TabBar::tabRemoved(index);
    // The tab that slid under the pointer is the hovered one now.
    hovered_ = tabAt(mapFromGlobal(QCursor::pos()));
    updateCloseButtons();
  }

private:
  void setHoveredTab(int index) {
    if (index == hovered_)
      return;
    hovered_ = index;
    updateCloseButtons();
  }

  void updateCloseButtons() {
    const auto side = static_cast<QTabBar::ButtonPosition>(style()->styleHint(
        QStyle::SH_TabBar_CloseButtonPosition, nullptr, this));
    for (int i = 0; i < count(); ++i)
      if (QWidget *button = tabButton(i, side))
        button->setVisible(i == currentIndex() || i == hovered_);
  }

  int hovered_ = -1;
};

/// One drawn caption provides docking and window controls. Keep the dock
/// button alongside Maximize: the library's optional maximize button replaces
/// the dock button, making the existing return action disappear.
class DockTitleBar final : public KDDockWidgets::QtWidgets::TitleBar {
public:
  using TitleBar::TitleBar;

protected:
  void init() override {
    TitleBar::init();
    if (!titleBar()->floatingWindow())
      return;
    maximize_ = new KDDockWidgets::QtWidgets::Button(this);
    maximize_->setObjectName(QStringLiteral("floatingMaximizeButton"));
    m_minimizeButton->setObjectName(QStringLiteral("floatingMinimizeButton"));
    m_layout->insertWidget(m_layout->indexOf(m_floatButton), maximize_);
    connect(maximize_, &QAbstractButton::clicked, this, [this] {
      titleBar()->onMaximizeClicked();
      updateMaximize();
    });
    QWidget::window()->installEventFilter(this);
    connect(&Theme::instance(), &Theme::changed, this,
            [this] { updateMaximize(); });
    updateMaximize();
    if (onWayland() && m_dockWidgetIcon) {
      // Wayland's compositor move and QDrag docking are separate operations.
      // The caption and tab dock; the icon moves the window on the desktop.
      m_dockWidgetIcon->setObjectName(
          QStringLiteral("floatingWindowMoveHandle"));
      m_dockWidgetIcon->setToolTip(
          QCoreApplication::translate("Docking", "Move window"));
      m_dockWidgetIcon->setCursor(Qt::SizeAllCursor);
      m_dockWidgetIcon->installEventFilter(this);
    }
  }

  bool eventFilter(QObject *object, QEvent *event) override {
    if (object == QWidget::window() &&
        (event->type() == QEvent::WindowStateChange ||
         event->type() == QEvent::LanguageChange))
      updateMaximize();
    if (object == m_dockWidgetIcon &&
        event->type() == QEvent::MouseButtonPress &&
        static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton)
      if (auto *handle = QWidget::window()->windowHandle())
        return handle->startSystemMove();
    return TitleBar::eventFilter(object, event);
  }

private:
  void updateMaximize() {
    if (!maximize_)
      return;
    const bool maximized = QWidget::window()->isMaximized();
    maximize_->setIcon(icon(maximized ? "restore" : "maximize"));
    const auto text =
        maximized
            ? QCoreApplication::translate("neverd::gui::TitleBar", "Restore")
            : QCoreApplication::translate("neverd::gui::TitleBar", "Maximize");
    maximize_->setToolTip(text);
    maximize_->setAccessibleName(text);
  }

  QAbstractButton *maximize_ = nullptr;
};

/// Wayland needs our resizable frame around the library's docking caption.
/// The other desktops already use the library's client caption and resizing.
class DockFloatingWindow final
    : public KDDockWidgets::QtWidgets::FloatingWindow {
public:
  using FloatingWindow::FloatingWindow;

protected:
  bool event(QEvent *event) override {
    if (event->type() == QEvent::Polish && onWayland() && !frame_)
      frame_ =
          WindowFrame::attach(*this, nullptr, WindowFrame::Caption::Client);
    return FloatingWindow::event(event);
  }

  void paintEvent(QPaintEvent *) override {
    if (frame_)
      return;
    QPainter painter(this);
    painter.setPen(
        Theme::instance().chrome(QStringLiteral("FloatingWindowBorder")));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
  }

private:
  WindowFrame *frame_ = nullptr;
};

class DockViewFactory final : public KDDockWidgets::QtWidgets::ViewFactory {
public:
  KDDockWidgets::Core::View *
  createTitleBar(KDDockWidgets::Core::TitleBar *controller,
                 KDDockWidgets::Core::View *parent) const override {
    return new DockTitleBar(controller, parent);
  }

  KDDockWidgets::Core::View *
  createGroup(KDDockWidgets::Core::Group *controller,
              KDDockWidgets::Core::View *parent) const override {
    return new DockGroup(controller,
                         KDDockWidgets::QtCommon::View_qt::asQWidget(parent));
  }

  KDDockWidgets::Core::View *
  createSeparator(KDDockWidgets::Core::Separator *controller,
                  KDDockWidgets::Core::View *parent) const override {
    return new DockSeparator(controller, parent);
  }

  KDDockWidgets::Core::View *
  createTabBar(KDDockWidgets::Core::TabBar *controller,
               KDDockWidgets::Core::View *parent) const override {
    return new DockTabBar(controller,
                          KDDockWidgets::QtCommon::View_qt::asQWidget(parent));
  }

  KDDockWidgets::Core::View *
  createFloatingWindow(KDDockWidgets::Core::FloatingWindow *controller,
                       KDDockWidgets::Core::MainWindow *parent,
                       Qt::WindowFlags flags) const override {
    auto *window =
        qobject_cast<QMainWindow *>(KDDockWidgets::QtCommon::View_qt::asQWidget(
            parent ? parent->view() : nullptr));
    return new DockFloatingWindow(controller, window, flags);
  }
};
} // namespace

void configureDocking() {
  KDDockWidgets::initFrontend(KDDockWidgets::FrontendType::QtWidgets);
  auto &config = KDDockWidgets::Config::self();
  // Every docked window carries a closable tab, as in classic disassemblers.
  // The drawn floating caption remains a docking drag source on every desktop.
  config.setFlags(KDDockWidgets::Config::Flag_AlwaysShowTabs |
                  KDDockWidgets::Config::Flag_HideTitleBarWhenTabsVisible |
                  KDDockWidgets::Config::Flag_AlwaysTitleBarWhenFloating |
                  KDDockWidgets::Config::Flag_TabsHaveCloseButton |
                  KDDockWidgets::Config::Flag_AllowReorderTabs |
                  KDDockWidgets::Config::Flag_TitleBarHasMinimizeButton |
                  KDDockWidgets::Config::Flag_TitleBarIsFocusable);
  // Setting the thickness resets the spacing too, so it comes first.
  config.setSeparatorThickness(
      ::neverd::limits::gui_config::kDockingSeparatorGrabWidth);
  config.setLayoutSpacing(
      ::neverd::limits::gui_config::kDockingSeparatorLineWidth);
  config.setViewFactory(new DockViewFactory);
  config.setDragAboutToStartFunc(beginDockDrag);
  config.setDragEndedFunc(endDockDrag);
}

QMargins dockAreaMargins() { return DockAreaMargins; }

} // namespace neverd::gui
