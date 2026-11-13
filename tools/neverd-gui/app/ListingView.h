//===- ListingView.h - Interactive disassembly listing --------------------===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declare the revision-aware disassembly viewport and its observations.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_LISTINGVIEW_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_LISTINGVIEW_H

#include "Address.h"
#include "PaintCheck.h"
#include "StyledText.h"
#include "Theme.h"
#include "ViewSearch.h"
#include "WheelInput.h"

#include <QAbstractScrollArea>
#include <QJsonObject>
#include <QPointer>
#include <QRegion>
#include <QTimer>
#include <QVarLengthArray>
#include <QVector>
#include <deque>
#include <limits>
#include <optional>

namespace neverd::gui {

class AddressSpace;
class Session;

/// The disassembly listing: an address-ordered text view of the whole image
/// as the worker formats it.  Only a window of lines around the viewport is
/// held; scrolling pages more from the worker, and the scroll bar maps to the
/// linear address space like a classic disassembler's.
class ListingView final : public QAbstractScrollArea {
  Q_OBJECT
public:
  struct Line {
    Address item = 0, address = 0;
    Address itemSize = 0;
    int sub = 0, cls = 0;
    QString kind, prefix, flow, function;
    std::optional<Address> target, functionAddress;
    StyledLine styled;
    /// The row of a page that this line was read from.  A line is what its
    /// row says and no more, so a row that comes again as it was keeps the
    /// line read from it, with the layout its text has taken.
    QJsonObject row;
  };

  ListingView(Session &session, const AddressSpace &space,
              QWidget *parent = nullptr);

  /// Show \p address with its item's first content line under the cursor.
  void jumpTo(Address address);
  /// Reposition on a loaded item without fetching an unchanged listing.
  /// False if its rows are stale, incomplete or a different jump is pending.
  bool revealItem(Address address);
  /// A function identity already confirmed by the worker for a loaded item.
  /// No result while the listing's project, revision or generation is stale.
  std::optional<Address> loadedFunctionAt(Address address) const;
  /// Address of the cursor line, if any line is loaded.
  std::optional<Address> currentAddress() const;
  /// Item address of the cursor line.
  std::optional<Address> currentItem() const;
  /// Item address of the first visible line, if any line is loaded.
  std::optional<Address> topItem() const;
  std::optional<Address> currentFunction() const;
  QString currentFunctionName() const;
  /// Navigation target of the token under the cursor, else of the line.
  std::optional<Address> operandTarget() const;
  /// The operand under the cursor on an instruction line, counted from 0;
  /// none off the operands.
  std::optional<int> currentOperand() const;
  /// Identifier token under the cursor.
  QString currentToken() const;
  QString currentLineText() const;
  QString selectedText() const;
  /// What Copy takes: the selection; else, as in IDA, the word under the
  /// cursor, which the highlight marks; nothing on blank space.
  QString copyText() const;
  /// Inclusive byte bounds of selected items, or the current item. Only
  /// worker-supplied extents from the current listing may define this range.
  std::optional<std::pair<Address, Address>> byteSelection() const;
  const Line *cursorLine() const;
  QRect cursorViewportRect() const;
  void setShowPrefixes(bool show);
  void setOpcodeBytes(int count);
  int opcodeBytes() const { return opcodeBytes_; }
  /// Refetch the loaded window (names, comments or analysis changed).
  void refresh();
  /// Discard cached rows, selection and pending reads for a closed image.
  void clear();
  /// Find \p text forward from the cursor among the loaded lines.
  bool findInLoaded(const QString &text, bool forward);
  /// Searchable loaded rows, including the currently visible address prefixes.
  QString searchText() const;
  bool searchReady() const { return rowsCurrent(); }
  std::pair<int, int> searchSelection() const;
  bool selectSearchRange(int begin, int length);
  void setSearchHighlight(const ViewSearchQuery &query);
  QString searchSeed() const;
  /// A jump was requested and its lines have not arrived yet.
  bool jumpPending() const { return pendingJump_.has_value(); }

signals:
  /// The cursor moved to another line.
  void locationChanged(neverd::gui::Address address);
  /// Follow a reference (double click or Enter).
  void navigateRequested(neverd::gui::Address target);
  void contextMenuRequested(const QPoint &globalPosition);
  /// Current lines of a newly opened file were painted for the first time.
  void contentPainted();
  /// The lines of a requested jump arrived (or the jump failed).
  void jumpSettled();
  void searchDocumentChanged();
  void cursorChanged();

protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *event) override;
  bool viewportEvent(QEvent *event) override;
  void focusInEvent(QFocusEvent *event) override;
  void focusOutEvent(QFocusEvent *event) override;
  void scrollContentsBy(int dx, int dy) override;

private:
  friend struct ListingViewTestAccess;
  bool rowsCurrent() const;
  enum class Fetch { Jump, Before, After, Refresh };
  /// Where a refresh puts the cursor back: the line of its item at \p sub,
  /// or for a content line its place among the item's content lines, which
  /// header lines added or removed above it (a function created or deleted
  /// there) do not change.  A cursor in view stays in view.
  struct CursorPlace {
    Address item = 0;
    int sub = 0;
    int content = -1;
    bool visible = false;
  };
  void request(Fetch kind, Address address, int sub, int before, int after,
               std::optional<CursorPlace> cursor = {});
  void accept(Fetch kind, const QJsonObject &payload, quint64 serial,
              std::optional<CursorPlace> cursor, Address jumpAddress);
  void ensureLoaded();
  void placeJump(int anchor, Address address);
  void trim();
  void scrollLines(int delta);
  void moveCursor(int line, int column, bool extend = false);
  void ensureCursorVisible();
  void updateScrollBar();
  void updateMetrics();
  int visibleLines() const;
  int lineAt(int y) const;
  int columnAt(const Line &line, int x) const;
  /// Columns [first, second) of the address-prefix word whose glyphs \p x
  /// falls on: the region name or the address.
  std::optional<std::pair<int, int>> prefixWordAt(const Line &line,
                                                  int x) const;
  /// Select the prefix word under \p x on the cursor line, as IDA highlights
  /// a clicked address for Ctrl+C. False off a word.
  bool selectPrefixWord(int x);
  int firstColumn(const Line &line) const;
  qreal columnX(const Line &line, int column) const;
  int textLeft() const;
  void rebuildArrowEdges();
  void paintArrows(QPainter &painter, int first, int last);
  void showHint(const QPoint &position);
  void emitLocation();

  /// What every row of the viewport is painted with, beside the theme.
  struct RowFrame {
    int width = 0, height = 0, lineHeight = 0, ascent = 0, arrowsWidth = 0;
    /// Where the bodies and the address prefixes begin.
    int left = 0, prefixLeft = 0;
    bool prefixes = false;
    quint64 style = 0;
    bool operator==(const RowFrame &) const = default;
  };
  /// A fill under a row's text from one column to another; a column below
  /// zero lies in the address prefix.
  struct RowFill {
    int first = 0, last = 0;
    ColorRole role = ColorRole::ListingHighlight;
    bool operator==(const RowFill &) const = default;
  };
  /// All that one row of the viewport is painted from to the right of the
  /// arrows, beside its frame and the theme.  paintRow is given this and
  /// nothing of the view, so that two rows which compare equal paint the same
  /// pixels: a row is painted again only where this changed (showChanges).
  /// What a row paints from belongs here, or its row is left as it was
  /// painted.
  struct Row {
    /// A line; else a row below the lines, which shows the background alone;
    /// else one of which nothing is known.
    enum class Shows : quint8 { Line, Nothing, Unknown };
    Shows shows = Shows::Nothing;
    QString prefix;
    int cls = 0;
    QString text;
    QVector<StyledSpan> spans;
    /// The cursor's line; an item whose bytes a patch changed.
    bool current = false, patched = false;
    /// The fills are a search's matches, painted under the prefix; else they
    /// mark the highlighted words, over it.
    bool searched = false;
    QVarLengthArray<RowFill, 4> fills;
    /// The selected columns [first, second) of a selected row.
    bool selected = false;
    int selectionBegin = 0, selectionEnd = 0;
    /// The column the text cursor is drawn at, or NoCaret.
    int caret = NoCaret;
    bool operator==(const Row &) const = default;
  };
  static constexpr int NoCaret = std::numeric_limits<int>::min();
  /// One branch arrow as the gutter draws it: the heights it leaves and
  /// lands at, the column it runs down, and what it looks like.
  struct GutterArrow {
    qreal from = 0, to = 0;
    int level = 0;
    bool conditional = false, current = false, lands = false;
    bool operator==(const GutterArrow &) const = default;
  };
  /// All that the arrows' gutter is painted from, beside the frame and the
  /// theme: its arrows in the order they are drawn.
  using Gutter = QVector<GutterArrow>;
  /// What the rows of one paint share of the view's state.
  struct RowsContext;
  /// The address prefixes are as wide as the widest among the lines in view:
  /// takes that width, as each paint took it.
  void settlePrefix();
  RowFrame rowFrame() const;
  RowsContext rowsContext() const;
  /// What the line \p index is painted from; a row below the lines for none.
  Row rowAt(int index, const RowsContext &context) const;
  /// The arrows among the lines \p first to \p last.
  Gutter gutterArrows(int first, int last) const;
  /// Where a column of \p row lies across the viewport.
  static qreal columnX(const RowFrame &frame, const Row &row,
                       QTextLayout &layout, int column);
  /// Paints a line over its row's background, which starts at \p y, with
  /// the layout of its text.
  static void paintRow(QPainter &painter, const RowFrame &frame, const Row &row,
                       int y, QTextLayout &layout);
  /// Paints the gutter over its background; its arrows alone.
  static void paintGutter(QPainter &painter, const RowFrame &frame,
                          const Gutter &gutter);
  static void paintGutterArrows(QPainter &painter, int arrowsWidth,
                                const Gutter &gutter);
  /// Paints what \p region touches of the rows and of the gutter, each over
  /// its background: from what the viewport was last told it shows where
  /// \p told, else from what the view holds now, \p painted then taking what
  /// each row it painted was painted from, by the row's place in the viewport.
  void paintRows(QPainter &painter, const QRegion &region, bool told,
                 QVector<Row> *painted = nullptr) const;
  /// Says that what the view shows changed: every such change ends here, as
  /// it ended in an update of the whole viewport.  What changed is looked for
  /// once the one who says so is done (showChanges), so that it may change
  /// more after saying it, as it might before an update painted.
  void updateView();
  /// Has the viewport paint the rows whose painting changed since it was last
  /// told what they show, and no other.
  void showChanges();
  /// Whether all of the viewport is painted from what it was last told: not
  /// so after a change that no one said (updateView), which the view looks
  /// for as it paints.  \p painted holds what the rows just painted were
  /// painted from, the others being unknown.
  bool rowsKnown(const QVector<Row> &painted) const;

  WheelInput wheel_;
  Session &session_;
  const AddressSpace &space_;
  std::deque<Line> lines_;
  struct ArrowEdge {
    int from, to;
    bool conditional;
  };
  // Endpoints depend only on the bounded loaded rows, not the viewport or
  // cursor. Refresh them with those rows; choose visible lanes while painting.
  QVector<ArrowEdge> arrowEdges_;
  bool atStart_ = false, atEnd_ = false;
  bool fetchingBefore_ = false, fetchingAfter_ = false;
  quint64 serial_ = 0;
  quint64 contentSerial_ = 0;
  /// The revision the last jump or refresh was asked at, and whether a
  /// generation moved while it was on its way (generationChanged).
  QString requestedRevision_;
  bool generationBehind_ = false;
  /// The rows were asked for ahead of the others that hear of a new revision.
  bool askedAhead_ = false;
  int top_ = 0;
  // Body-relative columns; negative positions select the address prefix.
  int cursorLine_ = 0, cursorColumn_ = 0;
  std::optional<std::pair<int, int>> anchor_; // selection anchor (line, col)
  bool selecting_ = false;
  QString highlight_;
  ViewSearchQuery SearchHighlight;
  QString generation_;
  QString revision_;
  bool showPrefixes_ = true;
  int opcodeBytes_ = 0;
  // Metrics.
  qreal charWidth_ = 8;
  int lineHeight_ = 16, ascent_ = 12;
  int arrowsWidth_ = 0, prefixChars_ = 0;
  quint64 styleStamp_ = 1;
  QTimer scrollJump_;
  Address pendingScrollAddress_ = 0;
  bool updatingScrollBar_ = false;
  std::optional<Address> lastEmitted_;
  /// The function of the last emitted location: the same address in a
  /// function created or deleted there is a new location.
  std::optional<Address> lastFunction_;
  bool contentReported_ = false;
  std::optional<Address> pendingJump_;
  /// What showChanges last told the viewport it shows.  The window keeps
  /// those pixels of a row and of the gutter until one of them changes.
  RowFrame knownFrame_;
  QVector<Row> knownRows_;
  Gutter knownGutter_;
  /// updateView was called and showChanges is still to look.
  bool viewChanged_ = false;
  /// showChanges has asked for rows that no paint has painted yet: until
  /// someone says a change, the next paint finds them as they were told.
  bool rowsTold_ = false;
  /// The pixels showChanges has asked the viewport to paint, all told: what
  /// a change has the view paint, where a test holds it to few rows.  A
  /// window paints more for reasons of its own, as when the dock that holds
  /// the view lays itself out again.
  quint64 pixelsAsked_ = 0;
  /// What showChanges had painted again since a page was taken, for the
  /// one that took it to paint at once.
  QRegion rowsUpdated_;
  /// Lines that kept what an earlier page had read for them, for tests to
  /// count.
  quint64 linesKept_ = 0;
  PaintCheck::Picture paintChecked_;
};

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_LISTINGVIEW_H
