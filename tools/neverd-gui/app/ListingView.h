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
#include "StyledText.h"
#include "ViewSearch.h"
#include "WheelInput.h"

#include <QAbstractScrollArea>
#include <QPointer>
#include <QTimer>
#include <deque>
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
  void paintArrows(QPainter &painter, int first, int last);
  void showHint(const QPoint &position);
  void emitLocation();

  WheelInput wheel_;
  Session &session_;
  const AddressSpace &space_;
  std::deque<Line> lines_;
  bool atStart_ = false, atEnd_ = false;
  bool fetchingBefore_ = false, fetchingAfter_ = false;
  quint64 serial_ = 0;
  quint64 contentSerial_ = 0;
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
};

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_LISTINGVIEW_H
