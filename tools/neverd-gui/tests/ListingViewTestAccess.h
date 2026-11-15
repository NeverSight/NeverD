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
  /// The pixels the view has asked its viewport to paint, all told.
  static quint64 pixelsAsked(const ListingView &view) {
    return view.pixelsAsked_;
  }
  /// How many jumps and refreshes the view has asked for.
  static quint64 requests(const ListingView &view) { return view.serial_; }
  /// Lines that kept what an earlier page had read for them.
  static quint64 linesKept(const ListingView &view) { return view.linesKept_; }
  /// Reads of the lines in view that were taken ahead of a refresh.
  static quint64 visiblePagesTaken(const ListingView &view) {
    return view.visiblePagesTaken_;
  }
  /// Lines read ahead of a refresh that the refresh then read otherwise.
  static quint64 linesReadOtherwise(const ListingView &view) {
    return view.linesReadOtherwise_;
  }
  /// As a refresh asked for and not yet answered leaves the view: the lines
  /// in view may be read ahead of it.
  static void refreshAsked(ListingView &view) { ++view.serial_; }
  /// What the view would ask for as the lines in view.
  static std::optional<QJsonObject> visibleRead(const ListingView &view) {
    return view.visibleRead();
  }
  /// Gives the view \p rows, of \p asked asked for, as the lines in view
  /// read ahead of the refresh asked for last, or of the one asked for
  /// \p refreshesSince before it.
  static void acceptVisible(ListingView &view, const QJsonArray &rows,
                            int asked, bool atEnd = false, int anchor = 0,
                            int refreshesSince = 0) {
    view.acceptVisible({{"lines", rows}, {"anchor", anchor}, {"at_end", atEnd}},
                       asked, view.serial_ - quint64(refreshesSince));
  }
  /// The line the first row of the viewport shows, and the cursor's.
  static int top(const ListingView &view) { return view.top_; }
  static int cursorRow(const ListingView &view) { return view.cursorLine_; }
  static int lineHeight(const ListingView &view) { return view.lineHeight_; }
  static QString text(const ListingView &view, int row) {
    return view.lines_[std::size_t(row)].styled.text;
  }
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
  /// Marks \p word, as nothing of the view's does: no one is told.
  static void highlightUnsaid(ListingView &view, const QString &word) {
    view.highlight_ = word;
  }
  static void scroll(ListingView &view, int lines) { view.scrollLines(lines); }
  /// Has the line \p from jump to the line \p to, as nothing of the view's
  /// does: no one is told.
  static void arrowUnsaid(ListingView &view, int from, int to) {
    view.lines_[std::size_t(from)].flow = QStringLiteral("jump");
    view.lines_[std::size_t(from)].target = view.lines_[std::size_t(to)].item;
    view.rebuildArrowEdges();
  }
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
  /// Takes \p rows as the lines a refresh reads again: the first row of the
  /// viewport and the cursor stay on their lines.
  static void acceptRowsAtCursor(ListingView &view, const QJsonArray &rows) {
    std::optional<ListingView::CursorPlace> cursor;
    if (const auto *line = view.cursorLine()) {
      cursor = ListingView::CursorPlace{line->item, line->sub};
      cursor->visible = true;
    }
    view.accept(ListingView::Fetch::Refresh,
                {{"lines", rows},
                 {"anchor", view.top_},
                 {"generation", view.generation_},
                 {"at_start", true},
                 {"at_end", true}},
                view.serial_, cursor, 0);
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
