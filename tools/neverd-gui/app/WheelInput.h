//===- WheelInput.h - Text-view wheel and trackpad input -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep partial wheel movement until it reaches a text row or font point.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_WHEELINPUT_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_WHEELINPUT_H

#include <QPoint>

class QWheelEvent;

namespace neverd::gui {

class WheelInput final {
public:
  /// Whether the event measures a movement in pixels: a finger on a touchpad
  /// or a touch mouse, which the platform reports in phases or marks as its
  /// own.  A wheel turns in notches and reports them as angles; the pixels
  /// some platforms send along are the compositor's scroll distance for a
  /// notch (Wayland from Qt 6.12, where a notch is 10 or 15 of them) or a
  /// guess at a line (Cocoa), no measure of a view's rows.
  static bool measuresPixels(const QWheelEvent &event);
  /// Positive deltas move left/up: horizontal pixels and vertical text rows.
  QPoint scroll(const QWheelEvent &event, int lineHeight);
  /// Consume Control/Command wheel input, including empty gesture boundaries.
  bool zoomCodeFont(const QWheelEvent &event, int lineHeight);

private:
  void resetScroll();
  void resetZoom();

  QPoint pixels_, angles_;
  int zoomPixels_ = 0, zoomAngles_ = 0;
};

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_WHEELINPUT_H
