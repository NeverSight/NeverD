//===- FlowLayout.cpp - Direction-aware wrapping controls ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve each control's size while wrapping rows in the reading direction.
//===----------------------------------------------------------------------===//

#include "FlowLayout.h"

#include <QStyle>
#include <QWidget>
#include <algorithm>

namespace neverd::gui {

FlowLayout::FlowLayout(QWidget *parent) : QLayout(parent) {
  setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout() {
  while (QLayoutItem *item = takeAt(0))
    delete item;
}

void FlowLayout::addItem(QLayoutItem *item) { items_.append(item); }

int FlowLayout::count() const { return int(items_.size()); }

QLayoutItem *FlowLayout::itemAt(int index) const {
  return index >= 0 && index < items_.size() ? items_.at(index) : nullptr;
}

QLayoutItem *FlowLayout::takeAt(int index) {
  return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const { return {}; }

bool FlowLayout::hasHeightForWidth() const { return true; }

int FlowLayout::heightForWidth(int width) const {
  return arrange(QRect(0, 0, width, 0), false);
}

QSize FlowLayout::minimumSize() const {
  // As narrow as its widest item, which then has a row to itself.
  QSize size;
  for (const QLayoutItem *item : items_)
    if (!item->isEmpty())
      size = size.expandedTo(item->minimumSize());
  const QMargins margins = contentsMargins();
  return size + QSize(margins.left() + margins.right(),
                      margins.top() + margins.bottom());
}

QSize FlowLayout::sizeHint() const {
  // Every item on one row.
  int width = 0, height = 0;
  bool first = true;
  for (const QLayoutItem *item : items_) {
    if (item->isEmpty())
      continue;
    const QSize hint = item->sizeHint();
    width += hint.width() + (first ? 0 : horizontalSpacing());
    height = std::max(height, hint.height());
    first = false;
  }
  const QMargins margins = contentsMargins();
  return {width + margins.left() + margins.right(),
          height + margins.top() + margins.bottom()};
}

void FlowLayout::setGeometry(const QRect &rect) {
  QLayout::setGeometry(rect);
  arrange(rect, true);
}

int FlowLayout::arrange(const QRect &rect, bool place) const {
  const QMargins margins = contentsMargins();
  const QRect area = rect.marginsRemoved(margins);
  // The items of each row, a new row where the next item would not fit.
  struct Row {
    QList<QLayoutItem *> items;
    int height = 0;
  };
  QList<Row> rows(1);
  int x = area.x();
  for (QLayoutItem *item : items_) {
    if (item->isEmpty())
      continue;
    const QSize hint = item->sizeHint();
    if (!rows.back().items.isEmpty() && x + hint.width() > area.right() + 1) {
      rows.append(Row());
      x = area.x();
    }
    rows.back().items.append(item);
    rows.back().height = std::max(rows.back().height, hint.height());
    x += hint.width() + horizontalSpacing();
  }
  if (rows.front().items.isEmpty())
    return margins.top() + margins.bottom();
  int y = area.y();
  for (const Row &row : rows) {
    if (place) {
      // Align the integer centres of the inclusive QRect extents. Rounding
      // half the height difference instead shifts even-height items up one
      // pixel beside an odd-height item.
      int left = area.x();
      const int center = y + (row.height - 1) / 2;
      for (QLayoutItem *item : row.items) {
        const QSize hint = item->sizeHint();
        const QRect logical(QPoint(left, center - (hint.height() - 1) / 2),
                            hint);
        const auto direction = parentWidget()
                                   ? parentWidget()->layoutDirection()
                                   : Qt::LeftToRight;
        item->setGeometry(QStyle::visualRect(direction, area, logical));
        left += hint.width() + horizontalSpacing();
      }
    }
    y += row.height + verticalSpacing();
  }
  return y - verticalSpacing() - area.y() + margins.top() + margins.bottom();
}

int FlowLayout::horizontalSpacing() const {
  if (spacing() >= 0)
    return spacing();
  const QWidget *owner = parentWidget();
  return owner ? owner->style()->layoutSpacing(QSizePolicy::DefaultType,
                                               QSizePolicy::DefaultType,
                                               Qt::Horizontal, nullptr, owner)
               : 6;
}

int FlowLayout::verticalSpacing() const {
  if (spacing() >= 0)
    return spacing();
  const QWidget *owner = parentWidget();
  return owner ? owner->style()->layoutSpacing(QSizePolicy::DefaultType,
                                               QSizePolicy::DefaultType,
                                               Qt::Vertical, nullptr, owner)
               : 6;
}

} // namespace neverd::gui
