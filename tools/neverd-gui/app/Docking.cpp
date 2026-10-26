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
#include "TitleBar.h"
#include "WindowFrame.h"

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDebug>
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
#include <QToolButton>
#include <QVariantAnimation>
#include <QWindow>
#include <algorithm>
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
#include <kddockwidgets/qtwidgets/Platform.h>
#include <kddockwidgets/qtwidgets/ViewFactory.h>
#include <kddockwidgets/qtwidgets/views/FloatingWindow.h>
#include <kddockwidgets/qtwidgets/views/Group.h>
#include <kddockwidgets/qtwidgets/views/Separator.h>
#include <kddockwidgets/qtwidgets/views/TabBar.h>
#include <kddockwidgets/qtwidgets/views/TitleBar.h>

namespace neverd::gui {
namespace {
constexpr char FloatingLocked[] = "neverdFloatingLocked";

// WindowFrame owns native moving and resizing. Let the docking library handle
// client drags on every desktop instead of installing a second Windows frame
// handler that turns unlocked docking gestures into system window moves.
class DockPlatform final : public KDDockWidgets::QtWidgets::Platform {
public:
  bool supportsAeroSnap() const override { return false; }
};

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
  auto *view = KDDockWidgets::QtCommon::View_qt::asQWidget(draggable->asView());
  auto *floating = qobject_cast<KDDockWidgets::QtWidgets::FloatingWindow *>(
      view ? view->window() : nullptr);
  // Locking prevents accidental drag docking; explicit Dock actions still work.
  if (floating && floating->property(FloatingLocked).toBool())
    return false;
  if (!onWayland() || !floating || !draggable->isWindow())
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
    // The library's local proxy can pick the native macOS style, bypassing
    // our Fusion metrics and shrinking the close glyph. Inherit the workbench
    // style; its animation hint keeps the library's tab-removal workaround.
    setStyle(nullptr);
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

/// Floating panes use the same window-control size and drawing as TitleBar.
/// Keep the docking library's button base for its safe close-event lifetime.
class DockWindowButton final : public KDDockWidgets::QtWidgets::Button {
public:
  DockWindowButton(KDDockWidgets::TitleBarButtonType type, QWidget *parent)
      : Button(parent), type_(type) {
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover);
  }

  QSize sizeHint() const override {
    return {::neverd::limits::gui_config::kTitleBarButtonWidth,
            ::neverd::limits::gui_config::kTitleBarHeight};
  }

protected:
  void paintEvent(QPaintEvent *) override {
    using Kind = neverd::gui::TitleBar::WindowControl;
    using Type = KDDockWidgets::TitleBarButtonType;
    QPainter painter(this);
    switch (type_) {
    case Type::Close:
      neverd::gui::TitleBar::paintWindowControl(painter, *this, *window(),
                                                Kind::Close);
      break;
    case Type::Minimize:
      neverd::gui::TitleBar::paintWindowControl(painter, *this, *window(),
                                                Kind::Minimize);
      break;
    case Type::Maximize:
      neverd::gui::TitleBar::paintWindowControl(painter, *this, *window(),
                                                Kind::Maximize);
      break;
    default:
      if (isDown() || underMouse())
        painter.fillRect(
            rect(), Theme::instance().chrome(isDown() ? "WindowControlPressed"
                                                      : "WindowControlHover"));
      constexpr int size = ::neverd::limits::gui_config::kTitleBarIconSize;
      neverd::gui::icon("restore").paint(
          &painter,
          QRect((width() - size) / 2, (height() - size) / 2, size, size));
      break;
    }
  }

private:
  KDDockWidgets::TitleBarButtonType type_;
};

/// One drawn caption provides docking and window controls. Keep the dock
/// button alongside Maximize: the library's optional maximize button replaces
/// the dock button, making the existing return action disappear.
class DockTitleBar final : public KDDockWidgets::QtWidgets::TitleBar {
public:
  using TitleBar::TitleBar;

  void addToolbar(QWidget &toolbar) {
    toolbar.setParent(this);
    toolbar.setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_layout->insertWidget(m_layout->indexOf(moveArea_), &toolbar);
    toolbar_ = &toolbar;
    captionFocusPolicy_ = focusPolicy();
    // A toolbar button must act on the current pane. Mouse focus on the
    // caption otherwise activates the floating window's first dock instead.
    setFocusPolicy(Qt::TabFocus);
    toolbar.installEventFilter(this);
    // Keep a control-sized patch available for moving even on small screens.
    moveArea_->setMinimumWidth(
        ::neverd::limits::gui_config::kTitleBarButtonWidth);
    toolbar.show();
  }

protected:
  void init() override {
    TitleBar::init();
    if (!titleBar()->floatingWindow())
      return;
    maximize_ =
        new DockWindowButton(KDDockWidgets::TitleBarButtonType::Maximize, this);
    maximize_->setObjectName(QStringLiteral("floatingMaximizeButton"));
    m_minimizeButton->setObjectName(QStringLiteral("floatingMinimizeButton"));
    m_closeButton->setObjectName(QStringLiteral("floatingCloseButton"));
    m_layout->insertWidget(m_layout->indexOf(m_floatButton), maximize_);
    m_layout->setContentsMargins(
        ::neverd::limits::gui_config::kTitleBarTitlePadding, 0, 0, 0);
    m_layout->setSpacing(0);
    lock_ = new QToolButton(this);
    lock_->setObjectName(QStringLiteral("floatingLockButton"));
    lock_->setCheckable(true);
    lock_->setAutoRaise(true);
    // The padlock opens and closes with the state; no fill repeats it.
    lock_->setProperty("glyphToggle", true);
    lock_->setFixedSize(::neverd::limits::gui_config::kTitleBarHeight,
                        ::neverd::limits::gui_config::kTitleBarHeight);
    m_layout->insertWidget(m_layout->indexOf(m_minimizeButton), lock_);
    moveArea_ = new QWidget(this);
    moveArea_->setObjectName(QStringLiteral("floatingWindowMoveArea"));
    moveArea_->installEventFilter(this);
    // Keep the drag region in the same layout as the controls. Updating an
    // overlay only while painting lets its old bounds cover buttons after
    // a resize, before the next paint arrives.
    for (int i = 0; i < m_layout->count(); ++i)
      if (m_layout->itemAt(i)->spacerItem()) {
        delete m_layout->takeAt(i);
        m_layout->insertWidget(i, moveArea_, 1);
        break;
      }
    moveArea_->show();
    connect(lock_, &QToolButton::toggled, this, [this](bool locked) {
      QWidget::window()->setProperty(FloatingLocked, locked);
      updateLock();
    });
    lock_->setChecked(true);
    updateLock();
    connect(maximize_, &QAbstractButton::clicked, this, [this] {
      titleBar()->onMaximizeClicked();
      updateMaximize();
    });
    QWidget::window()->installEventFilter(this);
    connect(&Theme::instance(), &Theme::changed, this, [this] {
      updateMaximize();
      updateLock();
      update();
    });
    updateMaximize();
    if (m_dockWidgetIcon)
      m_dockWidgetIcon->hide();
  }

  bool eventFilter(QObject *object, QEvent *event) override {
    if (object == toolbar_ && event->type() == QEvent::ParentChange &&
        toolbar_->parentWidget() != this) {
      toolbar_->removeEventFilter(this);
      toolbar_.clear();
      setFocusPolicy(captionFocusPolicy_);
      moveArea_->setMinimumWidth(0);
    }
    if (object == moveArea_) {
      if (qEnvironmentVariableIsSet("NEVERD_TRACE_NATIVE_DRAG") &&
          (event->type() == QEvent::MouseButtonPress ||
           event->type() == QEvent::MouseButtonDblClick ||
           event->type() == QEvent::MouseButtonRelease ||
           event->type() == QEvent::MouseMove))
        qInfo() << "drag-area event" << event->type()
                << static_cast<QMouseEvent *>(event)->globalPosition()
                << QWidget::window()->geometry()
                << QWidget::window()->isActiveWindow();
      if (event->type() == QEvent::MouseButtonDblClick &&
          static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        titleBar()->onMaximizeClicked();
        return true;
      }
      if (event->type() == QEvent::MouseButtonPress &&
          static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        if (auto *handle = QWidget::window()->windowHandle()) {
          if (qEnvironmentVariableIsSet("NEVERD_TRACE_NATIVE_DRAG"))
            qInfo() << "system-move begin" << handle->geometry();
          const bool started = handle->startSystemMove();
          if (qEnvironmentVariableIsSet("NEVERD_TRACE_NATIVE_DRAG"))
            qInfo() << "system-move end" << started << handle->geometry();
        }
        return true;
      }
    }
    if (object == QWidget::window() &&
        (event->type() == QEvent::WindowStateChange ||
         event->type() == QEvent::LanguageChange ||
         event->type() == QEvent::ActivationChange)) {
      updateMaximize();
      updateLock();
      update();
      for (auto *button : findChildren<QAbstractButton *>())
        button->update();
    }
    return TitleBar::eventFilter(object, event);
  }

  QSize sizeHint() const override {
    QSize size = TitleBar::sizeHint();
    if (titleBar()->floatingWindow())
      size.setHeight(::neverd::limits::gui_config::kTitleBarHeight);
    return size;
  }

  void paintEvent(QPaintEvent *event) override {
    if (!lock_) {
      TitleBar::paintEvent(event);
      return;
    }
    QPainter painter(this);
    const auto &theme = Theme::instance();
    // The tabs identify each pane; leave its floating caption blank.
    painter.fillRect(rect(), theme.chrome("TitleBar"));
  }

  void mouseDoubleClickEvent(QMouseEvent *event) override {
    if (lock_ && lock_->isChecked() && event->button() == Qt::LeftButton) {
      titleBar()->onMaximizeClicked();
      return;
    }
    TitleBar::mouseDoubleClickEvent(event);
  }

private:
  void updateLock() {
    if (!lock_)
      return;
    const bool locked = lock_->isChecked();
    lock_->setIcon(neverd::gui::icon(locked ? "lock" : "lock_open"));
    const auto text =
        locked
            ? QCoreApplication::translate("Docking", "Unlock floating window")
            : QCoreApplication::translate("Docking", "Lock floating window");
    lock_->setToolTip(text);
    lock_->setAccessibleName(text);
    moveArea_->setAccessibleName(
        QCoreApplication::translate("Docking", "Move window"));
    moveArea_->setAttribute(Qt::WA_TransparentForMouseEvents, !locked);
  }

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
  QPointer<QWidget> toolbar_;
  Qt::FocusPolicy captionFocusPolicy_ = Qt::NoFocus;
  QToolButton *lock_ = nullptr;
  QWidget *moveArea_ = nullptr;
};

/// Floating panes share the application frame and its resize hit area.
class DockFloatingWindow final
    : public KDDockWidgets::QtWidgets::FloatingWindow {
public:
  using FloatingWindow::FloatingWindow;

protected:
  bool event(QEvent *event) override {
    if (event->type() == QEvent::Polish && !frame_)
      frame_ = WindowFrame::attach(*this);
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
  QAbstractButton *
  createTitleBarButton(QWidget *parent,
                       KDDockWidgets::TitleBarButtonType type) const override {
    auto *bar = qobject_cast<KDDockWidgets::QtWidgets::TitleBar *>(parent);
    if (bar && bar->titleBar()->floatingWindow())
      return new DockWindowButton(type, parent);
    return ViewFactory::createTitleBarButton(parent, type);
  }

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
  if (!KDDockWidgets::Core::Platform::hasInstance())
    new DockPlatform;
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
  config.setInternalFlags(
      config.internalFlags() |
      KDDockWidgets::Config::InternalFlag_UseTransparentFloatingWindow);
  // Setting the thickness resets the spacing too, so it comes first.
  config.setSeparatorThickness(
      ::neverd::limits::gui_config::kDockingSeparatorGrabWidth);
  config.setLayoutSpacing(
      ::neverd::limits::gui_config::kDockingSeparatorLineWidth);
  config.setViewFactory(new DockViewFactory);
  config.setDragAboutToStartFunc(beginDockDrag);
  config.setDragEndedFunc(endDockDrag);
}

bool addFloatingWindowToolbar(QWidget &window, QWidget &toolbar) {
  auto *floating =
      qobject_cast<KDDockWidgets::QtWidgets::FloatingWindow *>(&window);
  if (!floating)
    return false;
  auto *caption = KDDockWidgets::QtCommon::View_qt::asQWidget(
      floating->floatingWindow()->titleBar()->view());
  // configureDocking installs this caption for every floating window.
  static_cast<DockTitleBar *>(caption)->addToolbar(toolbar);
  return true;
}

QMargins dockAreaMargins() { return DockAreaMargins; }

} // namespace neverd::gui
