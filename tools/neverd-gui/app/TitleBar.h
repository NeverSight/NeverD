#pragma once

#include <QPointer>
#include <QWidget>
#include <optional>

class QAbstractButton;
class QAction;
class QMenuBar;
class QPainter;

namespace neverd::gui {

/// The title bar of a window that draws its own frame (WindowFrame), after
/// Visual Studio Code's custom title bar.  A bar with menus is the workbench
/// window's: the application icon, the menus, a command center that names the
/// open file and runs a command, and the minimize, maximize and close
/// buttons.  A bar without menus is a dialog's: its title in the middle and
/// the buttons its window flags ask for, Close alone by default.  Dragging
/// the bar's empty parts moves the window, and double-clicking them maximizes
/// or restores a window that can be maximized.  The bar dims while the window
/// is inactive; full screen hides the icon and the buttons.
class TitleBar final : public QWidget {
  Q_OBJECT
public:
  /// A bar at the top of \p window that shows \p menuBar, if any, and takes
  /// it.
  explicit TitleBar(QWidget &window, QMenuBar *menuBar = nullptr);

  /// Show \p text in the command center, with \p toolTip over it.
  void setCommandCenterText(const QString &text, const QString &toolTip);
  /// The command the command center runs when clicked.
  void setCommandCenterAction(QAction *action);
  /// The command center while a click on it runs its command, so that what
  /// the command opens can drop from it; null otherwise.
  QWidget *clickedCommandCenter() const { return clicked_; }

  enum class WindowControl { Minimize, Maximize, Close };
  /// Shared window-control drawing for workbench, dialogs and floating panes.
  static void paintWindowControl(QPainter &painter,
                                 const QAbstractButton &button,
                                 const QWidget &window, WindowControl control);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

protected:
  bool event(QEvent *event) override;
  bool eventFilter(QObject *object, QEvent *event) override;
  void changeEvent(QEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;

private:
  /// Place the buttons at the right and, with menus, the menus at the left
  /// and the command center in the middle of the bar or in the room the menus
  /// leave.
  void arrange();
  bool maximizable() const;
  void toggleMaximized();
  /// Follow the window into and out of the maximized and full-screen states.
  void updateWindowState();
  /// Dim the bar while the window is inactive.
  void updateActive();
  void retranslateUi();
  void repaintAll();

  QWidget &window_;
  QMenuBar *menuBar_;
  QAbstractButton *commandCenter_ = nullptr;
  QAbstractButton *minimize_;
  QAbstractButton *maximize_;
  QAbstractButton *close_;
  QPointer<QAction> command_;
  QPointer<QWidget> clicked_;
  /// Where the buttons begin; a dialog's title stays left of them.
  int buttonsLeft_ = 0;
  /// Where a press on the bar's empty part began a possible window move.
  std::optional<QPoint> pressAt_;
};

} // namespace neverd::gui
