#pragma once

#include <QMargins>
#include <QObject>
#include <QPointer>

class QApplication;
class QDialog;
class QWidget;
class QWindow;

namespace neverd::gui {

/// The frame of a top-level window that draws its own title bar, as Visual
/// Studio Code's windows do: a TitleBar inside the window takes the place of
/// the system's.  On Linux the window manager adds no frame at all and the
/// window resizes from every edge and corner; under Wayland, where GNOME
/// draws no shadow for such a window, it also draws a soft shadow outside its
/// visible edge, and that band resizes it too.  On Windows and macOS the
/// system frame stays without its title bar, so the system still draws the
/// window's edges and shadow, snaps and resizes it, and on macOS draws the
/// traffic lights (WindowFramePlatform.h).  Windows keep the system's whole
/// frame elsewhere and when the setting ui/nativeTitleBar is on.
class WindowFrame final : public QObject {
public:
  enum class Caption { UserPreference, Client };

  /// Before the application object exists: a window that takes its native
  /// window early, as a dialog polished before it first shows does in
  /// attach(), keeps it to itself.  Qt otherwise gives every other child of
  /// its parent a native window too, and GNOME's Wayland composites the
  /// workbench's title bar, toolbars, docks and status bar as subsurfaces
  /// that stop updating once the dialog closes.
  static void prepareNativeWindows();

  /// Whether top-level windows draw their own frame.
  static bool enabled();

  /// Give \p window its own frame.  Call before the window first shows, as
  /// its native window takes the frame when it is created.  A \p titleBar,
  /// which the frame then places across the window's top, is for a window
  /// whose layout leaves no room for one.  Returns null where windows keep
  /// the window manager's frame. Caption::Client keeps an interactive client
  /// title bar even when the user prefers native captions elsewhere.
  static WindowFrame *attach(QWidget &window, QWidget *titleBar = nullptr,
                             Caption caption = Caption::UserPreference);

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

  ~WindowFrame() override;

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  WindowFrame(QWidget &window, QWindow &handle, QWidget *titleBar,
              bool frameless, bool drawsShadow);
  bool resizable() const;
  /// Draw the shadow outside the visible window and its surface inside.
  void paint();
  /// Give the shadow band its size, which a maximized or full-screen window
  /// does without, and keep room for the title bar.  The band follows the
  /// state the window system applied (states_), not the one QWidget requests:
  /// a Wayland window that changed its geometry for a state the compositor
  /// had not entered yet committed buffers for neither state.
  void updateMargins();
  void placeTitleBar();
  void showEdges(Qt::Edges edges);

  QWidget &window_;
  QWindow &handle_;
  QPointer<QWidget> titleBar_;
  /// Without any system frame, rather than without its title bar alone.
  const bool frameless_;
  const bool drawsShadow_;
  /// The window's own contents margins, inside the frame's.
  const QMargins content_;
  /// The shadow band.
  QMargins margins_;
  /// What a maximized window keeps clear above its title bar.
  QMargins insets_;
  /// The window states the window system last applied.
  Qt::WindowStates states_;
  Qt::Edges shown_;
};

} // namespace neverd::gui
