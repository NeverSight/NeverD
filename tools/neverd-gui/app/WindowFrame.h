//===- WindowFrame.h - Owned window frame and resize gestures ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Application frame ownership across native window transitions.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_WINDOWFRAME_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_WINDOWFRAME_H

#include <QByteArray>
#include <QMargins>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <memory>

class QAbstractNativeEventFilter;
class QApplication;
class QDialog;
class QScreen;
class QTimer;
class QWidget;
class QWindow;

namespace neverd::gui {

/// Application-painted top-level frame, title bar, edges and shadow. Native
/// APIs provide moving, resizing and window states without choosing appearance.
class WindowFrame final : public QObject {
public:
  /// Before the application object exists: a window that takes its native
  /// window early, as a dialog polished before it first shows does in
  /// attach(), keeps it to itself.  Qt otherwise gives every other child of
  /// its parent a native window too, and GNOME's Wayland composites the
  /// workbench's title bar, toolbars, docks and status bar as subsurfaces
  /// that stop updating once the dialog closes. Also selects application
  /// dialogs and menus before Qt initializes their platform helpers.
  static void prepareNativeWindows();

  /// Give \p window its own frame.  Call before the window first shows, as
  /// its native window takes the frame when it is created.  A \p titleBar,
  /// which the frame then places across the window's top, is for a window
  /// whose layout leaves no room for one.
  static WindowFrame *attach(QWidget &window, QWidget *titleBar = nullptr);

  /// Give every dialog its own frame and a TitleBar as it first shows,
  /// except popups and dialogs that are frameless already.
  static void frameDialogs(QApplication &app);

  /// Make \p dialog at least \p width wide, the width its title bar needs to
  /// show the whole title centered.  A message box sizes itself to its text
  /// once it shows, so its text takes the missing width; the box measures
  /// that text unwrapped and wraps it only past a width limit, so only a text
  /// narrower unwrapped than the title is widened.
  static void fitTitle(QDialog &dialog, int width);

  /// The edges a press at \p at resizes in a window of \p size whose visible
  /// part lies \p shadow inside it.  Within a corner's reach a press resizes
  /// both edges.
  static Qt::Edges edgesAt(QPoint at, QSize size, QMargins shadow);

  /// Before the first Show, restore geometry without resuming full screen.
  bool restoreWindowedGeometry(const QByteArray &geometry);
  /// Persist the normal or maximized geometry from before F11.
  QByteArray saveWindowedGeometry() const;

  /// Apply a state after any native full-screen exit has restored its screen.
  void setWindowState(Qt::WindowStates states);
  /// Keep the state and normal geometry from before entering full screen.
  void toggleFullScreen();

  ~WindowFrame() override;

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  WindowFrame(QWidget &window, QWindow &handle, QWidget *titleBar);
  bool resizable() const;
  Qt::Edges resizeEdgesAt(QPoint at) const;
  void syncWindowState();
  void restoreAfterFullScreen();
  /// Draw the shadow outside the visible window and its surface inside.
  void paint();
  /// Give the shadow band its size, which a maximized or full-screen window
  /// does without, and keep room for the title bar.  The band follows the
  /// state the window system applied (states_), not the one QWidget requests:
  /// a Wayland window that changed its geometry for a state the compositor
  /// had not entered yet committed buffers for neither state. Windows also
  /// uses Qt's state signal for its geometry-based frameless maximization.
  void updateMargins();
  void placeTitleBar();
  void showEdges(Qt::Edges edges);
  void prepareResize();
  void resizeTo(QPoint globalPosition);
  void finishResize();

  QWidget &window_;
  QWindow &handle_;
  std::unique_ptr<QAbstractNativeEventFilter> nativeResizeFilter_;
  QPointer<QWidget> titleBar_;
  /// The window's own contents margins, inside the frame's.
  const QMargins content_;
  /// The shadow band.
  QMargins margins_;
  /// The window states the window system last applied.
  Qt::WindowStates states_;
  Qt::WindowStates beforeFullScreen_ = Qt::WindowNoState;
  QByteArray geometryBeforeFullScreen_;
  QByteArray startupGeometry_;
  bool maximizeOnShow_ = false;
  QPointer<QScreen> fullScreenScreen_;
  QRect fullScreenAvailable_;
  QRect fullScreenScreenGeometry_;
  QRect normalBeforeFullScreen_;
  QMetaObject::Connection screenGeometryConnection_;
  QTimer *restoreTimer_ = nullptr;
  bool restoreMaximized_ = false;
  bool restoreScreenTimedOut_ = false;
  Qt::Edges shown_;
  Qt::Edges resizing_;
  QPoint resizeOrigin_;
  QRect resizeGeometry_;
};

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_WINDOWFRAME_H
