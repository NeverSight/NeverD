//===- ListingViewTestAccess.h - Listing observation for tests -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Same-build observation of listing context and branch-arrow painting.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_TESTS_LISTINGVIEWTESTACCESS_H
#define NEVERD_TOOLS_NEVERD_GUI_TESTS_LISTINGVIEWTESTACCESS_H

#include "ListingView.h"

#include <QImage>
#include <QPainter>
#include <utility>

namespace neverd::gui {
// Same-build observation of the loaded context and the production arrow
// painter. The workbench never includes this test header.
struct ListingViewTestAccess {
  static bool settled(const ListingView &view) {
    return view.rowsCurrent() && !view.fetchingBefore_ && !view.fetchingAfter_;
  }
  static int rows(const ListingView &view) { return int(view.lines_.size()); }
  static std::pair<Address, int> first(const ListingView &view) {
    const auto &line = view.lines_.front();
    return {line.item, line.sub};
  }
  static std::pair<Address, int> last(const ListingView &view) {
    const auto &line = view.lines_.back();
    return {line.item, line.sub};
  }
  /// Shows only \p line, with the cursor at \p column of it.
  static void showOnly(ListingView &view, ListingView::Line line, int column) {
    view.lines_.assign(1, std::move(line));
    view.cursorLine_ = 0;
    view.cursorColumn_ = column;
  }
  /// The word the highlight marks.
  static QString highlight(const ListingView &view) { return view.highlight_; }
  static QImage arrows(ListingView &view) {
    QImage image(view.arrowsWidth_, view.viewport()->height(),
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    QPainter painter(&image);
    view.paintArrows(painter, view.top_, view.top_ + view.visibleLines());
    return image;
  }
};
} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_TESTS_LISTINGVIEWTESTACCESS_H
