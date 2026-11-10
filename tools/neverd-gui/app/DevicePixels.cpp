//===- DevicePixels.cpp - Boxes and lines on whole device pixels ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Snap boxes and lines to device pixels, and paint a box the way Qt's style
/// sheet engine paints one with a single border width, color and radius.
//===----------------------------------------------------------------------===//

#include "DevicePixels.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QTransform>
#include <array>
#include <cmath>

namespace neverd::gui {
namespace {

// The sides of a box, numbered as Qt's style sheet engine numbers them.
enum Side { Top, Right, Bottom, Left };

bool shows(const QColor &color) { return color.isValid() && color.alpha() > 0; }

/// The corners' radius as the style sheet engine keeps it (qNormalizeRadii):
/// none at all where two corners would not fit along a side.  A size of no
/// radius is zero wide, not the invalid size, whose sides are -1.
QSizeF cornerRadius(const QRect &box, qreal radius) {
  if (radius <= 0 || 2 * radius > box.width() || 2 * radius > box.height())
    return {0, 0};
  return {radius, radius};
}

// The painting below follows Qt 6.8's style sheet engine step for step
// (QRenderRule::borderClip and drawBackground, and qDrawBorder, qDrawEdge and
// qDrawRoundedCorners for a solid border), so that a box painted here at a
// whole scale matches, pixel for pixel, the rule it replaces.

/// The outline a rounded box's fill takes, through the middle of each side's
/// border, whose \p widths may differ.
QPainterPath fillOutline(const QRectF &rect, const std::array<int, 4> &widths,
                         const QSizeF &r) {
  QPainterPath path;
  qreal y = rect.y() + widths[Top] / 2.0;
  path.moveTo(rect.x() + r.width(), y);
  path.lineTo(rect.right() - r.width(), y);
  qreal x = rect.right() - widths[Right] / 2.0;
  path.arcTo(x - 2 * r.width() + widths[Right], y,
             r.width() * 2 - widths[Right], r.height() * 2 - widths[Top], 90,
             -90);
  path.lineTo(x, rect.bottom() - r.height());
  y = rect.bottom() - widths[Bottom] / 2.0;
  path.arcTo(x - 2 * r.width() + widths[Right],
             y - 2 * r.height() + widths[Bottom], r.width() * 2 - widths[Right],
             r.height() * 2 - widths[Bottom], 0, -90);
  path.lineTo(rect.x() + r.width(), y);
  x = rect.left() + widths[Left] / 2.0;
  path.arcTo(x, rect.bottom() - 2 * r.height() + widths[Bottom] / 2.0,
             r.width() * 2 - widths[Left], r.height() * 2 - widths[Bottom], 270,
             -90);
  path.lineTo(x, rect.top() + r.height());
  path.arcTo(x, rect.top() + widths[Top] / 2.0, r.width() * 2 - widths[Left],
             r.height() * 2 - widths[Top], 180, -90);
  path.closeSubpath();
  return path;
}

/// One side of a border; a side whose neighbors it does not paint over meets
/// them on a diagonal.
void paintSide(QPainter &painter, qreal x1, qreal y1, qreal x2, qreal y2,
               qreal dw1, qreal dw2, Side side, const QColor &color) {
  painter.save();
  const qreal width = (side == Top || side == Bottom) ? y2 - y1 : x2 - x1;
  painter.setPen(Qt::NoPen);
  painter.setBrush(color);
  if (width == 1 || (dw1 == 0 && dw2 == 0)) {
    painter.drawRect(QRectF(x1, y1, x2 - x1, y2 - y1));
  } else {
    std::array<QPointF, 4> quad;
    switch (side) {
    case Top:
      quad = {QPointF(x1, y1), QPointF(x1 + dw1, y2), QPointF(x2 - dw2, y2),
              QPointF(x2, y1)};
      break;
    case Bottom:
      quad = {QPointF(x1 + dw1, y1), QPointF(x1, y2), QPointF(x2, y2),
              QPointF(x2 - dw2, y1)};
      break;
    case Left:
      quad = {QPointF(x1, y1), QPointF(x1, y2), QPointF(x2, y2 - dw2),
              QPointF(x2, y1 + dw1)};
      break;
    case Right:
      quad = {QPointF(x1, y1 + dw1), QPointF(x1, y2 - dw2), QPointF(x2, y2),
              QPointF(x2, y1)};
      break;
    }
    painter.drawConvexPolygon(quad.data(), static_cast<int>(quad.size()));
  }
  painter.restore();
}

/// The two rounded corners at the ends of one side of a border.
void paintCorners(QPainter &painter, qreal x1, qreal y1, qreal x2, qreal y2,
                  const QSizeF &r1, const QSizeF &r2, Side side,
                  const QColor &color) {
  const qreal pw = (side == Top || side == Bottom) ? y2 - y1 : x2 - x1;
  const qreal half = pw / 2;
  painter.save();
  painter.setBrush(Qt::NoBrush);
  QPen pen(QBrush(color), pw, Qt::SolidLine, Qt::FlatCap);
  pen.setCapStyle(Qt::SquareCap);
  painter.setPen(pen);
  switch (side) {
  case Top:
    if (!r1.isEmpty())
      painter.drawArc(QRectF(x1 - r1.width() + half, y1 + half,
                             2 * r1.width() - pw, 2 * r1.height() - pw),
                      135 * 16, -45 * 16);
    if (!r2.isEmpty())
      painter.drawArc(QRectF(x2 - r2.width() + half, y1 + half,
                             2 * r2.width() - pw, 2 * r2.height() - pw),
                      45 * 16, 45 * 16);
    break;
  case Bottom:
    if (!r1.isEmpty())
      painter.drawArc(QRectF(x1 - r1.width() + half,
                             y2 - 2 * r1.height() + half, 2 * r1.width() - pw,
                             2 * r1.height() - pw),
                      -90 * 16, -45 * 16);
    if (!r2.isEmpty())
      painter.drawArc(QRectF(x2 - r2.width() + half,
                             y2 - 2 * r2.height() + half, 2 * r2.width() - pw,
                             2 * r2.height() - pw),
                      -90 * 16, 45 * 16);
    break;
  case Left:
    if (!r1.isEmpty())
      painter.drawArc(QRectF(x1 + half, y1 - r1.height() + half,
                             2 * r1.width() - pw, 2 * r1.height() - pw),
                      135 * 16, 45 * 16);
    if (!r2.isEmpty())
      painter.drawArc(QRectF(x1 + half, y2 - r2.height() + half,
                             2 * r2.width() - pw, 2 * r2.height() - pw),
                      180 * 16, 45 * 16);
    break;
  case Right:
    if (!r1.isEmpty())
      painter.drawArc(QRectF(x2 - 2 * r1.width() + half,
                             y1 - r1.height() + half, 2 * r1.width() - pw,
                             2 * r1.height() - pw),
                      45 * 16, -45 * 16);
    if (!r2.isEmpty())
      painter.drawArc(QRectF(x2 - 2 * r2.width() + half,
                             y2 - r2.height() + half, 2 * r2.width() - pw,
                             2 * r2.height() - pw),
                      315 * 16, 45 * 16);
    break;
  }
  painter.restore();
}

/// A border of one color whose sides are \p widths wide, bottom first and top
/// last, as the style sheet engine paints them in order of precedence.
void paintBorder(QPainter &painter, const QRect &rect,
                 const std::array<int, 4> &widths, const QColor &color,
                 const QSizeF &radius) {
  const QRectF box(rect);
  const QSizeF &tl = radius, &tr = radius, &bl = radius, &br = radius;
  // A side paints over the end of its neighbor when the neighbor has no
  // border or both are one opaque color; otherwise they meet on a diagonal.
  const auto over = [&](Side neighbor) {
    return widths[neighbor] == 0 || color.alpha() == 255;
  };
  if (widths[Bottom] > 0) {
    const qreal dw1 = (bl.width() > 0 || over(Left)) ? 0 : widths[Left];
    const qreal dw2 = (br.width() > 0 || over(Right)) ? 0 : widths[Right];
    const qreal x1 = box.x() + bl.width();
    const qreal y1 = box.y() + box.height() - widths[Bottom];
    const qreal x2 = box.x() + box.width() - br.width();
    const qreal y2 = box.y() + box.height();
    paintSide(painter, x1, y1, x2, y2, dw1, dw2, Bottom, color);
    if (bl.width() > 0 || br.width() > 0)
      paintCorners(painter, x1, y1, x2, y2, bl, br, Bottom, color);
  }
  if (widths[Right] > 0) {
    const qreal dw1 = (tr.height() > 0 || over(Top)) ? 0 : widths[Top];
    const qreal dw2 = (br.height() > 0 || over(Bottom)) ? 0 : widths[Bottom];
    const qreal x1 = box.x() + box.width() - widths[Right];
    const qreal y1 = box.y() + tr.height();
    const qreal x2 = box.x() + box.width();
    const qreal y2 = box.y() + box.height() - br.height();
    paintSide(painter, x1, y1, x2, y2, dw1, dw2, Right, color);
    if (tr.height() > 0 || br.height() > 0)
      paintCorners(painter, x1, y1, x2, y2, tr, br, Right, color);
  }
  if (widths[Left] > 0) {
    const qreal dw1 = (tl.height() > 0 || over(Top)) ? 0 : widths[Top];
    const qreal dw2 = (bl.height() > 0 || over(Bottom)) ? 0 : widths[Bottom];
    const qreal x1 = box.x();
    const qreal y1 = box.y() + tl.height();
    const qreal x2 = box.x() + widths[Left];
    const qreal y2 = box.y() + box.height() - bl.height();
    paintSide(painter, x1, y1, x2, y2, dw1, dw2, Left, color);
    if (tl.height() > 0 || bl.height() > 0)
      paintCorners(painter, x1, y1, x2, y2, tl, bl, Left, color);
  }
  if (widths[Top] > 0) {
    const qreal dw1 = (tl.width() > 0 || over(Left)) ? 0 : widths[Left];
    const qreal dw2 = (tr.width() > 0 || over(Right)) ? 0 : widths[Right];
    const qreal x1 = box.x() + tl.width();
    const qreal y1 = box.y();
    const qreal x2 = box.left() + box.width() - tr.width();
    const qreal y2 = box.y() + widths[Top];
    paintSide(painter, x1, y1, x2, y2, dw1, dw2, Top, color);
    if (tl.width() > 0 || tr.width() > 0)
      paintCorners(painter, x1, y1, x2, y2, tl, tr, Top, color);
  }
}

} // namespace

bool DevicePixels::snaps(const QPainter &painter) {
  const QTransform device = painter.deviceTransform();
  const auto whole = [](qreal value) { return value == std::round(value); };
  // Where every logical pixel already covers whole device pixels, the painter
  // stays as it is, so that a box comes out exactly as the style sheet's,
  // whose curves Qt rasterizes a little differently in other coordinates.
  return device.type() <= QTransform::TxScale && device.m11() > 0 &&
         qFuzzyCompare(device.m11(), device.m22()) &&
         !(whole(device.m11()) && whole(device.dx()) && whole(device.dy()));
}

DevicePixels::DevicePixels(QPainter &painter) : painter_(painter) {
  painter_.save();
  if (snaps(painter_)) {
    const QTransform device = painter_.deviceTransform();
    snaps_ = true;
    scale_ = device.m11();
    dx_ = device.dx();
    dy_ = device.dy();
    painter_.setWorldTransform(device.inverted() * painter_.worldTransform());
  }
}

QRectF DevicePixels::outline(const QRect &rect, int width) const {
  const qreal inset = this->width(width) / 2.0;
  return QRectF(this->rect(rect)).adjusted(inset, inset, -inset, -inset);
}

DevicePixels::~DevicePixels() { painter_.restore(); }

QRect DevicePixels::rect(const QRect &rect) const {
  if (!snaps_)
    return rect;
  const int left = qRound(rect.x() * scale_ + dx_);
  const int top = qRound(rect.y() * scale_ + dy_);
  const int right = qRound((rect.x() + rect.width()) * scale_ + dx_);
  const int bottom = qRound((rect.y() + rect.height()) * scale_ + dy_);
  return QRect(left, top, right - left, bottom - top);
}

int DevicePixels::width(int width) const {
  if (!snaps_ || width <= 0)
    return width;
  // Allow for the rounding error of a scale such as 1.1 times 10.
  const qreal scaled = width * scale_;
  return scaled < 1 ? 1 : static_cast<int>(std::floor(scaled + 1e-6));
}

void paintDeviceBox(QPainter &painter, const QRect &rect,
                    const DeviceBox &box) {
  const bool fill = shows(box.fill);
  const bool border =
      shows(box.border) && box.borderWidth > 0 && box.edges != Qt::Edges();
  if (rect.isEmpty() || (!fill && !border))
    return;
  DevicePixels device(painter);
  const QRect area = device.rect(rect);
  if (area.isEmpty())
    return;
  const int width = box.borderWidth > 0 ? device.width(box.borderWidth) : 0;
  const std::array<int, 4> widths = {box.edges & Qt::TopEdge ? width : 0,
                                     box.edges & Qt::RightEdge ? width : 0,
                                     box.edges & Qt::BottomEdge ? width : 0,
                                     box.edges & Qt::LeftEdge ? width : 0};
  // As the style sheet rounds a box's corners whichever sides have a border.
  const QSizeF radius = cornerRadius(area, device.length(box.radius));
  if (fill) {
    if (radius.isEmpty()) {
      painter.fillRect(area, box.fill);
    } else {
      painter.setRenderHint(QPainter::Antialiasing);
      painter.fillPath(fillOutline(QRectF(area), widths, radius), box.fill);
    }
  }
  if (border) {
    painter.setRenderHint(QPainter::Antialiasing);
    paintBorder(painter, area, widths, box.border, radius);
  }
}

void fillDeviceLine(QPainter &painter, const QRect &line,
                    Qt::Orientation orientation, const QColor &color) {
  if (line.isEmpty() || !shows(color))
    return;
  DevicePixels device(painter);
  QRect run = device.rect(line);
  if (orientation == Qt::Horizontal)
    run.setHeight(device.width(line.height()));
  else
    run.setWidth(device.width(line.width()));
  painter.fillRect(run, color);
}

void paintDeviceSeparator(QPainter &painter, const QRect &line,
                          Qt::Orientation orientation, const QColor &color,
                          qreal barWidth, const QColor &bar) {
  fillDeviceLine(painter, line, orientation, color);
  if (line.isEmpty() || !shows(bar))
    return;
  const bool across = orientation == Qt::Horizontal;
  DevicePixels device(painter);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setBrush(bar);
  if (!device.snaps()) {
    const qreal start =
        (across ? line.top() : line.left()) +
        ((across ? line.height() : line.width()) - barWidth) / 2;
    painter.drawRoundedRect(
        across ? QRectF(line.left(), start, line.width(), barWidth)
               : QRectF(start, line.top(), barWidth, line.height()),
        barWidth / 2, barWidth / 2);
    return;
  }
  // The bar keeps the hairline's middle: both take whole pixels of the same
  // parity, the bar as many as its scaled width holds.
  const QRect run = device.rect(line);
  const int thickness = device.width(across ? line.height() : line.width());
  int width = device.width(qRound(barWidth));
  if ((width - thickness) % 2 != 0)
    --width;
  const int start = (across ? run.top() : run.left()) + (thickness - width) / 2;
  painter.drawRoundedRect(across
                              ? QRectF(run.left(), start, run.width(), width)
                              : QRectF(start, run.top(), width, run.height()),
                          width / 2.0, width / 2.0);
}

} // namespace neverd::gui
