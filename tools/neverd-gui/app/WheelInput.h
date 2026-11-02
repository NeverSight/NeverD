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
