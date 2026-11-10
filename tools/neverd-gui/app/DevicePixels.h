//===- DevicePixels.h - Boxes and lines on whole device pixels ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Paint a control's box and the workbench's thin lines on whole device
/// pixels, so that they stay sharp at fractional display scales.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_DEVICE_PIXELS_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_DEVICE_PIXELS_H

#include <QColor>
#include <QRect>
#include <QRectF>
#include <Qt>

class QPainter;

namespace neverd::gui {

/// Switches a painter to device pixels for as long as it lives, so that what
/// it paints next lands on whole pixels of the screen.
///
/// At a fractional scale such as 125%, Qt places a widget at a fraction of a
/// device pixel, and a line one logical pixel wide covers a pixel and a
/// quarter: drawn there, it spreads over two device pixels at partial
/// strength.  Here a rectangle's sides round to the nearest pixel edge, as
/// Qt rounds the area each widget may paint, and a line takes the whole device
/// pixels in its scaled width, at least one, as a web browser draws a border.
/// Where each logical pixel already covers whole device pixels, as at 100% or
/// 200%, the painter keeps its coordinates and nothing moves; so does a
/// painter that rotates or shears.
class DevicePixels {
public:
  explicit DevicePixels(QPainter &painter);
  ~DevicePixels();
  DevicePixels(const DevicePixels &) = delete;
  DevicePixels &operator=(const DevicePixels &) = delete;

  /// The device pixels that \p rect, in the painter's coordinates before the
  /// switch, covers once its sides round to the nearest pixel edge.
  QRect rect(const QRect &rect) const;
  /// The device pixels a line \p width logical pixels wide fills: as many as
  /// its scaled width holds whole, and at least one.
  int width(int width) const;
  /// \p length, a radius or an offset, in the coordinates painted in.
  qreal length(qreal length) const { return length * scale_; }
  /// The outline a pen \p width logical pixels wide strokes just inside
  /// \p rect, through the middle of its line, for drawRect or
  /// drawRoundedRect with a pen width(\p width) wide.
  QRectF outline(const QRect &rect, int width) const;
  /// Whether the painter paints in device pixels now, which it does only at a
  /// fractional scale or offset.
  bool snaps() const { return snaps_; }
  /// Whether a DevicePixels would switch \p painter to device pixels.
  static bool snaps(const QPainter &painter);

private:
  QPainter &painter_;
  qreal scale_ = 1;
  qreal dx_ = 0;
  qreal dy_ = 0;
  bool snaps_ = false;
};

/// A control's box: a fill, and a border of one width and color along some or
/// all of its sides, with round corners.  It is the box a style sheet rule
/// with `background`, `border` and `border-radius` draws, and a side with an
/// invisible border still keeps its width, as a transparent style sheet
/// border does.
struct DeviceBox {
  QColor fill;
  QColor border;
  int borderWidth = 1;
  Qt::Edges edges = Qt::TopEdge | Qt::LeftEdge | Qt::RightEdge | Qt::BottomEdge;
  qreal radius = 0;
};

/// Paints \p box over \p rect, in \p painter's coordinates, on whole device
/// pixels (DevicePixels).  At a whole scale it paints exactly what the style
/// sheet rule paints.
void paintDeviceBox(QPainter &painter, const QRect &rect, const DeviceBox &box);

/// Fills \p line, which runs along \p orientation and is as thick as its
/// other side, on whole device pixels: its ends round to the nearest pixel
/// edge, its near side lies where a box's side would, and it is as thick as a
/// border of its width (DevicePixels).
void fillDeviceLine(QPainter &painter, const QRect &line,
                    Qt::Orientation orientation, const QColor &color);

/// Paints a separator's hairline, \p line, as fillDeviceLine does, and, when
/// \p bar shows, a rounded bar \p barWidth logical pixels across centered on
/// it: a dock's or a sidebar's resize handle under the pointer.
void paintDeviceSeparator(QPainter &painter, const QRect &line,
                          Qt::Orientation orientation, const QColor &color,
                          qreal barWidth, const QColor &bar);

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_DEVICE_PIXELS_H
