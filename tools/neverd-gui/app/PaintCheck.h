//===- PaintCheck.h - Check views that repaint parts of themselves --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A view that repaints only the rows whose painting changed leaves the rest
/// to what its window already shows of them.  PaintCheck keeps, for such a
/// view, the picture its paints add up to, and compares it after each paint
/// with all of the view painted afresh.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_PAINT_CHECK_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_PAINT_CHECK_H

#include <QImage>
#include <QRegion>
#include <QtGlobal>
#include <functional>

class QPainter;
class QWidget;

namespace neverd::gui {

/// Checks the views that repaint parts of themselves.  It is off unless a
/// test turns it on: a checked paint is painted twice more.
class PaintCheck {
public:
  static void setEnabled(bool enabled);
  static bool enabled();
  /// The picture the paints of one view add up to, which the view keeps.
  class Picture {
    friend class PaintCheck;
    QImage image_;
    /// Every pixel of it has been painted.
    bool whole_ = false;
  };
  /// How a view paints the part of itself a region covers: as it has just
  /// painted it for its window, or \p afresh, from nothing but what it holds.
  using Paint = std::function<void(QPainter &, const QRegion &, bool afresh)>;
  /// A view calls this in its paint event, once \p painter has painted
  /// \p region of \p widget.  Where that painted what the window keeps of the
  /// widget, rather than a picture someone grabs of it, \p paint paints the
  /// same region into \p picture, as the window's store took it.  A view that
  /// is \p settled, which knows of no change it has yet to paint, is then
  /// painted afresh, all of it, and the two are compared.  A difference is a
  /// row the view left as an earlier state of it was painted: it counts as a
  /// stale paint and is said as a critical message.
  ///
  /// Both pictures are painted the way an image is, so the window's own
  /// pixels, whose text a screen's subpixels may sharpen, are not what is
  /// compared: the paints that reach the window are.
  static void painted(const QWidget &widget, const QPainter &painter,
                      const QRegion &region, bool settled, Picture &picture,
                      const Paint &paint);
  /// The paints compared since the check was turned on.
  static quint64 checks();
  /// Those after which the picture was not the view painted afresh.
  static quint64 stalePaints();
  /// The picture kept and the one painted afresh at the last stale paint, for
  /// a test to save.
  static QImage lastKept();
  static QImage lastAfresh();
};

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_PAINT_CHECK_H
