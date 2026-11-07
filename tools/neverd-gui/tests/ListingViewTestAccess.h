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
#include <QJsonArray>
#include <QJsonObject>
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
    view.rebuildArrowEdges();
    view.cursorLine_ = 0;
    view.cursorColumn_ = column;
  }
  /// The word the highlight marks.
  static QString highlight(const ListingView &view) { return view.highlight_; }
  enum class Page { Replace, Before, After };
  static void acceptRows(ListingView &view, const QJsonArray &rows, Page page) {
    const auto kind = page == Page::Before  ? ListingView::Fetch::Before
                      : page == Page::After ? ListingView::Fetch::After
                                            : ListingView::Fetch::Refresh;
    view.accept(kind,
                {{"lines", rows},
                 {"anchor", page == Page::Before ? rows.size() : 0},
                 {"generation", view.generation_},
                 {"at_start", true},
                 {"at_end", true}},
                view.serial_, {}, 0);
  }
  static QJsonArray edges(const ListingView &view) {
    QJsonArray result;
    for (const auto &edge : view.arrowEdges_)
      result.append(QJsonArray{edge.from, edge.to, edge.conditional});
    return result;
  }
  // Independent row-by-row lookup, including duplicate item rows and targets
  // outside the current window. No production endpoint index is consulted.
  static QJsonArray expectedEdges(const ListingView &view) {
    QJsonArray result;
    const int count = int(view.lines_.size());
    for (int from = 0; from < count; ++from) {
      const auto &line = view.lines_[from];
      if (!line.target || (line.flow != QLatin1String("jump") &&
                           line.flow != QLatin1String("cjump")))
        continue;
      int to = *line.target < line.item ? -1 : count;
      for (int row = 0; row < count; ++row) {
        const auto &candidate = view.lines_[row];
        if (candidate.item == *line.target &&
            (candidate.kind == QLatin1String("header") ||
             candidate.kind == QLatin1String("label") ||
             candidate.kind == QLatin1String("insn"))) {
          to = row;
          break;
        }
      }
      result.append(QJsonArray{from, to, line.flow == QLatin1String("cjump")});
    }
    return result;
  }
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
