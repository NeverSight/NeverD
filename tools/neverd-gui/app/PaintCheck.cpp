//===- PaintCheck.cpp - Check views that repaint parts of themselves ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps the picture a view's paints add up to and compares it with the view
/// painted afresh.
//===----------------------------------------------------------------------===//

#include "PaintCheck.h"

#include <QBackingStore>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainter>
#include <QWidget>
#include <cmath>
#include <cstring>

namespace neverd::gui {
namespace {

struct CheckState {
  bool enabled = false;
  quint64 checks = 0;
  quint64 stale = 0;
  QImage kept, afresh;
};

CheckState &state() {
  static CheckState checked;
  return checked;
}

} // namespace

void PaintCheck::setEnabled(bool enabled) {
  auto &checked = state();
  checked.enabled = enabled;
  checked.checks = checked.stale = 0;
  checked.kept = checked.afresh = {};
}

bool PaintCheck::enabled() { return state().enabled; }
quint64 PaintCheck::checks() { return state().checks; }
quint64 PaintCheck::stalePaints() { return state().stale; }
QImage PaintCheck::lastKept() { return state().kept; }
QImage PaintCheck::lastAfresh() { return state().afresh; }

void PaintCheck::painted(const QWidget &widget, const QPainter &painter,
                         const QRegion &region, bool settled, Picture &picture,
                         const Paint &paint) {
  auto &checked = state();
  if (!checked.enabled)
    return;
  QBackingStore *store = widget.backingStore();
  QPaintDevice *device = store ? store->paintDevice() : nullptr;
  // A grab or a render paints a picture of its own and leaves what the
  // window keeps as it was.
  if (!device || !painter.paintEngine() ||
      painter.paintEngine()->paintDevice() != device)
    return;
  // Where a pixel of the screen is no whole number of the widget's, the edge
  // of a region rounds as the window's store alone knows.
  const qreal scale = device->devicePixelRatio();
  if (scale != std::round(scale))
    return;
  const QSize extent = widget.size() * int(scale);
  if (extent.isEmpty())
    return;
  // A window that resizes the widget paints all of it again.
  if (picture.image_.size() != extent ||
      picture.image_.devicePixelRatio() != scale) {
    picture.image_ = QImage(extent, QImage::Format_RGB32);
    picture.image_.setDevicePixelRatio(scale);
    picture.image_.fill(Qt::magenta);
    picture.whole_ = false;
  }
  const auto painting = [&](QImage &image, const QRegion &clip, bool afresh) {
    QPainter again(&image);
    again.setFont(widget.font());
    again.setLayoutDirection(widget.layoutDirection());
    again.setClipRegion(clip);
    paint(again, clip, afresh);
  };
  painting(picture.image_, region, false);
  if (!picture.whole_) {
    // Until the window has painted all of the widget once, part of the
    // picture is no one's.
    picture.whole_ = (QRegion(widget.rect()) - region).isEmpty();
    if (!picture.whole_)
      return;
  }
  // A change still to be painted is in neither picture alike.
  if (!settled)
    return;
  QImage afresh(extent, QImage::Format_RGB32);
  afresh.setDevicePixelRatio(scale);
  afresh.fill(Qt::magenta);
  painting(afresh, widget.rect(), true);
  ++checked.checks;
  if (picture.image_ == afresh)
    return;
  ++checked.stale;
  checked.kept = picture.image_;
  checked.afresh = afresh;
  int row = 0;
  while (row < extent.height() - 1 &&
         !std::memcmp(picture.image_.constScanLine(row),
                      afresh.constScanLine(row), size_t(afresh.bytesPerLine())))
    ++row;
  qCritical("PaintCheck: %s keeps a stale row of pixels at %d of %dx%d",
            widget.parentWidget()
                ? widget.parentWidget()->metaObject()->className()
                : widget.metaObject()->className(),
            row, extent.width(), extent.height());
  // What follows is compared with what is shown from here on.
  picture.image_ = afresh;
}

} // namespace neverd::gui
