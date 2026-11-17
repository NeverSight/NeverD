//===- WheelInput.cpp - Text-view wheel and trackpad input ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prefer pixel movement and accumulate sub-row and sub-notch input.
//===----------------------------------------------------------------------===//

#include "WheelInput.h"

#include "Theme.h"

#include <QApplication>
#include <QWheelEvent>
#include <algorithm>

namespace neverd::gui {
namespace {
// QWheelEvent angles are eighths of a degree; a wheel notch is 15 degrees.
constexpr int AngleUnitsPerNotch = 120;
} // namespace

bool WheelInput::measuresPixels(const QWheelEvent &event) {
  if (event.pixelDelta().isNull())
    return false;
  return event.phase() != Qt::NoScrollPhase ||
         event.source() == Qt::MouseEventSynthesizedBySystem ||
         event.angleDelta().isNull();
}

void WheelInput::resetScroll() {
  pixels_ = {};
  angles_ = {};
}

void WheelInput::resetZoom() { zoomPixels_ = zoomAngles_ = 0; }

QPoint WheelInput::scroll(const QWheelEvent &event, int lineHeight) {
  if (event.phase() == Qt::ScrollBegin)
    resetScroll();
  QPoint delta;
  if (measuresPixels(event)) {
    angles_ = {};
    pixels_ += event.pixelDelta();
    delta = {pixels_.x(), pixels_.y() / lineHeight};
    pixels_.setX(0);
    pixels_.ry() %= lineHeight;
  } else if (!event.angleDelta().isNull()) {
    pixels_ = {};
    // As many lines for a notch as Qt's own lists scroll rows: the desktop's
    // setting, three where it has none.
    const int lines = std::max(1, QApplication::wheelScrollLines());
    angles_ += {event.angleDelta().x() * lines * lineHeight,
                event.angleDelta().y() * lines};
    delta = {angles_.x() / AngleUnitsPerNotch,
             angles_.y() / AngleUnitsPerNotch};
    angles_.rx() %= AngleUnitsPerNotch;
    angles_.ry() %= AngleUnitsPerNotch;
  }
  if (event.phase() == Qt::ScrollEnd)
    resetScroll();
  return delta;
}

bool WheelInput::zoomCodeFont(const QWheelEvent &event, int lineHeight) {
  if (!(event.modifiers() & Qt::ControlModifier)) {
    resetZoom();
    return false;
  }
  resetScroll();
  if (event.phase() == Qt::ScrollBegin)
    resetZoom();
  int steps = 0;
  if (const int angle = event.angleDelta().y()) {
    zoomPixels_ = 0;
    zoomAngles_ += angle;
    steps = zoomAngles_ / AngleUnitsPerNotch;
    zoomAngles_ %= AngleUnitsPerNotch;
  } else if (const int pixels = event.pixelDelta().y()) {
    zoomAngles_ = 0;
    zoomPixels_ += pixels;
    steps = zoomPixels_ / lineHeight;
    zoomPixels_ %= lineHeight;
  }
  if (event.phase() == Qt::ScrollEnd)
    resetZoom();
  if (steps)
    Theme::instance().zoomCodeFont(steps);
  return true;
}

} // namespace neverd::gui
