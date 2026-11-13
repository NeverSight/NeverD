#pragma once

#include "Address.h"
#include "LibraryCodeView.h"
#include "PaintCheck.h"
#include "StyledText.h"
#include "Theme.h"
#include "ViewSearch.h"
#include "WheelInput.h"

#include "neverd/support/IntegerLiterals.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegion>
#include <QTimer>
#include <QVarLengthArray>
#include <QVector>
#include <QWidget>
#include <optional>
#include <tuple>

class QComboBox;
class QLabel;
class QPainter;
class QToolButton;
class QMenu;

namespace neverd::gui {

class PageBytes;
class Session;

/// The source choices every function's code opens with: whether source is
/// simplified symbolically, which removes mixed Boolean-arithmetic
/// obfuscation at a cost.  The user's settings keep it; a code window may
/// still choose otherwise for the function it shows.
class SourceDefaults final : public QObject {
  Q_OBJECT
public:
  static SourceDefaults &instance();
  bool symbolic() const;
  void setSymbolic(bool enabled);
  /// The options a function opens with in \p representation: none in an IR.
  static QJsonObject options(const QString &representation);

signals:
  void changed();

private:
  SourceDefaults() = default;
};

/// Text of one function in a code representation: C pseudocode (native or
/// through LLVM) or a textual IR.  Lines are fetched in pages and colored
/// locally; explicit navigation uses the worker's instruction mappings.
class CodeText final : public QAbstractScrollArea {
  Q_OBJECT
public:
  struct Line {
    StyledLine styled;
    QVector<Address> addresses;
    /// The line shows a folded summary.
    bool folded = false;
    int sourceLine = 0;
    QString block;
    bool blockFolded = false;
    /// The line defines a goto label, mapped to the block it labels.
    bool label = false;
    /// Whether a comment was open where the line begins, and where it ends,
    /// as highlightLine colored its text.
    bool commentBefore = false, commentAfter = false;
    /// The dialect highlightLine colored its text in while the text had no
    /// source names, where the spans are still those: such a line is colored
    /// by its text, that dialect and the comment open where it begins alone.
    /// ColoredOtherwise for a line colored among source names, and for one
    /// whose spans are a folded summary's or a message's.
    static constexpr qint8 ColoredOtherwise = -1;
    qint8 coloredIn = ColoredOtherwise;
  };

  CodeText(Session &session, QWidget *parent = nullptr);

  void load(Address function, const QString &representation,
            std::optional<QJsonObject> options = std::nullopt);
  const QJsonObject &sourceOptions() const { return sourceOptions_; }
  const QJsonObject &recoveryReport() const { return RecoveryReport; }
  const QJsonObject &recoveryOptions() const { return RecoveryOptions; }
  void setSourceOptions(const QJsonObject &options);
  /// The options a function opens with in this representation.
  QJsonObject defaultSourceOptions() const;
  void clear();
  /// Stop obsolete requests/pages, including an in-flight analysis process
  /// when no other view still subscribes to its result.
  void cancel();
  /// Cancel as the user does: a symbolic simplification of a function that
  /// shows no source yet gives way to its ordinary source.
  void cancelTransformation();
  std::optional<Address> function() const { return function_; }
  QString representation() const { return representation_; }
  /// The assembly-selected address on the cursor line, or its primary mapping.
  std::optional<Address> currentAddress() const;
  QRect cursorViewportRect() const;
  QVector<Address> currentAddresses() const;
  /// Select the first row mapped to \p address, waiting for all pages and
  /// expanding its fold if needed. A new load or cursor move cancels the wait.
  void selectAddress(Address address);
  /// Follow the disassembly's cursor to \p address: move to the first shown
  /// row mapped to it and mark the others, without unfolding, waiting or
  /// reporting an instruction no row maps. Neither move says locationChanged.
  void revealAddress(Address address);
  /// Keep the cursor's row and column and the first shown row as the place
  /// to come back to at \p address, the jump history's stop for it.
  void keepPlace(Address address);
  /// Go back to the place kept at \p address, as Hex-Rays' Esc does: now, or
  /// once the text on its way completes. Only the same function, shown the
  /// same way, has that place; false otherwise.
  bool returnToPlace(Address address);
  /// The number the margin shows for the line Back returns to at
  /// \p address, when the place kept there is in \p function and the code is
  /// shown as it was then.
  std::optional<int> keptLine(Address address, Address function) const;
  QString currentToken() const;
  /// The selected source name's image address or declared-local identity.
  std::optional<QJsonObject> renameTarget() const;
  /// Occurrences of one declared local in the complete, unfolded source.
  struct LocalReference {
    int line = 0, column = 0;
    int begin = 0, end = 0;
    QString text;
  };
  QVector<LocalReference> localReferences(const QJsonObject &target) const;
  /// Reveal and select an occurrence, expanding any fold that contains it.
  bool selectLocalReference(const QJsonObject &target,
                            const LocalReference &reference);
  /// Changes when a load or clear replaces this source document. Hiding a
  /// complete document cancels requests without replacing its contents.
  quint64 documentVersion() const { return DocumentVersion; }
  /// The layouts its lines took from the lines they replaced, which read the
  /// same (rebuildLines).
  quint64 layoutsKept() const { return layoutsKept_; }
  /// The pages it took whose rows were those of the page it showed there,
  /// which it had read already.
  quint64 pageRowsKept() const { return pageRowsKept_; }
  /// The lines that took the spans of the line that read the same in their
  /// place, colored as they are to be (rebuildLines).
  quint64 spansKept() const { return spansKept_; }
  /// The lines a rename's answer brought that took the layout made ready for
  /// them while the rename was with the worker (makeReadyFor).
  quint64 readyLinesTaken() const { return readyLinesTaken_; }
  /// The renames it read ahead of the worker's answer (readAheadOf).
  quint64 readsAhead() const { return readsAhead_; }
  /// It reads as a rename with the worker leaves it, which the worker has
  /// not answered yet.
  bool aheadOfTheWorker() const { return ahead_.has_value(); }
  /// The renames read ahead whose rows were painted as their lines were
  /// about to read and then again as the pages made them: none, where a line
  /// is painted ahead as the text then holds it.
  quint64 rowsShownTwice() const { return rowsShownTwice_; }
  /// All pages of this document arrived successfully. Error text and a
  /// cancelled or partially loaded document are not exportable source.
  bool complete() const { return complete_ && !loading_ && !interrupted_; }
  /// The original source row under the cursor, independent of folding.
  std::optional<QJsonObject> commentTarget() const;
  /// The integer literal under the cursor in source code the worker edits,
  /// as a number edit names it: its source line, that line's text as the
  /// engine wrote it, and its index among the line's literals, counted as
  /// neverd/support/IntegerLiterals.h counts them.
  struct NumberTarget {
    int line = 0;
    QString anchor;
    int index = 0;
    /// The literal as the engine wrote it, which the worker spells the
    /// other bases from.
    IntegerLiteral literal;
    LiteralSyntax syntax = LiteralSyntax::C;
    /// The base it reads in now.
    LiteralRadix shown = LiteralRadix::Decimal;
    /// The base the user gave it; Number where the decompiler chose.
    LiteralRadix chosen = LiteralRadix::Number;
  };
  std::optional<NumberTarget> numberTarget() const;
  QString selectedText() const;
  /// What Copy takes: the selection, a folded summary as the code it stands
  /// for, or, as in IDA, the name or word under the cursor, which the
  /// highlight marks; nothing on blank space.
  QString copyText() const;
  /// The source name or token under the cursor; none in a folded summary.
  QString cursorWord() const;
  QString allText() const;
  bool findText(const QString &text, bool forward);
  /// Source coordinates, independent of the current fold projection.
  std::pair<int, int> searchSelection() const;
  bool selectSearchRange(int begin, int length);
  void setSearchHighlight(const ViewSearchQuery &query);
  QString searchSeed() const;
  /// Move the cursor to \p line and reveal it.
  void setCursorLine(int line) { moveCursor(line, 0, false); }
  /// Select the whole text, as Ctrl+A does, without moving the view.
  void selectAll();
  /// The displayed line the cursor is on.
  int cursorLine() const { return cursorLine_; }
  int cursorColumn() const { return cursorColumn_; }
  /// The bracket the cursor is on and the one it pairs with, as shown lines
  /// and columns, which Hex-Rays marks together; none off a bracket, for one
  /// in a string or comment, or while its partner is not in the text.
  struct BracketPair {
    int line = 0, column = 0, partnerLine = 0, partnerColumn = 0;
  };
  std::optional<BracketPair> bracketPair() const;
  int lineCount() const { return int(lines_.size()); }
  /// Recognized library operations that can fold into one-line summaries.
  int foldableCount() const { return library_.foldableCount(); }
  bool anyFolded() const { return library_.anyFolded(); }
  /// Some recognized library operation shows as its summary.
  bool libraryFolded() const { return library_.libraryFolded(); }
  /// Code with the declarations before its definition: C's includes and
  /// declarations, or what an LLVM IR function refers to.
  bool hasPrelude() const;
  bool preludeFolded() const;
  void setPreludeFolded(bool folded);
  /// LLVM IR with the attribute groups and metadata after its definition.
  bool hasEpilogue() const;
  bool epilogueFolded() const;
  void setEpilogueFolded(bool folded);
  /// The source line that declares type or macro \p name in this code: a
  /// typedef, struct, union, enum or #define.  Functions are named in the
  /// binary and navigate there instead.  In LLVM IR, the line that defines a
  /// global (`@name`), type, value or block (`%name`), attribute group (`#0`)
  /// or metadata (`!4`).
  std::optional<int> declarationLine(const QString &name) const;
  /// Show the declaration of \p name, expanding the fold that holds it.
  bool goToDeclaration(const QString &name);
  /// The block address the goto label \p name stands for, with the source
  /// line that defines it, when the shown code defines that label.
  std::optional<std::pair<Address, int>>
  addressLabel(const QString &name) const;
  /// Where the disassembly shows the function's frame part \p name names:
  /// the function's address for the frame record HighC declares and its
  /// storage, and for a member of it the instruction the cursor's line, else
  /// the first line that reads or writes it, maps to.
  std::optional<Address> frameAddress(const QString &name) const;
  /// The image address the global name at a line and column of LLVM IR
  /// stands for there, as the engine resolved it: a function's entry, or the
  /// byte a constant offset from an image mirror reaches.
  std::optional<Address> globalReferenceAt(int line, int column) const;
  /// What hovering that global shows: the comments over its declaration,
  /// which say what it is, the declaration, and where a double-click goes.
  /// Empty where no global stands for an image address.
  QString globalHoverText(int line, int column) const;
  /// Show the declaration of the global last activated as \p name, which the
  /// binary does not name; false when this code declares none.
  bool showUnresolvedDeclaration(const QString &name);
  /// The symbol a function declaration links \p name to with an assembler
  /// label: a C++ function reads by its stem and links by its mangled name.
  std::optional<QString> linkedSymbol(const QString &name) const;
  /// The image address of a global the code declares, as the comment its
  /// writer puts beside the declaration names it.
  std::optional<Address> objectAddress(const QString &name) const;
  void setFolded(bool folded);
  /// Fold the header's block, or the innermost block containing the cursor.
  bool setCurrentBlockFolded(bool folded);
  const QString &status() const { return status_; }
  /// Pages of the current function are still arriving.
  bool loading() const { return loading_; }
  bool interrupted() const { return interrupted_; }
  /// The language the loaded code reads in, "c", "rust" or "go"; empty for
  /// an IR or before the first page.
  QString language() const;
  /// A source language's name such as `core::fmt::write`, which splitting
  /// the text into identifiers would not find whole.
  struct SourceName {
    /// The C identifier the C view spells for it.
    QString identifier;
    QString symbol;
    std::optional<Address> address;
  };
  /// The source name at a line and column, and how the text spells it.
  std::optional<std::pair<QString, SourceName>> sourceNameAt(int line,
                                                             int column) const;

signals:
  /// The cursor moved to a row mapped to \p address, not by selectAddress or
  /// revealAddress following the disassembly.
  void locationChanged(neverd::gui::Address address);
  void cursorChanged();
  void addressSelected(neverd::gui::Address address, bool mapped);
  /// A name was double-clicked; the owner resolves and navigates.
  void nameActivated(const QString &name);
  /// A global the code declares at an image address was double-clicked.
  void objectActivated(neverd::gui::Address address);
  /// A place in this function was double-clicked: \p address is where the
  /// disassembly goes, the block a goto label names or the function's frame.
  void labelActivated(neverd::gui::Address address);
  void statusChanged();
  void foldingChanged();
  void contextMenuRequested(const QPoint &globalPosition);
  /// The language the code reads in changed with a new function.
  void languageChanged();

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
  void scrollContentsBy(int dx, int dy) override;
  bool event(QEvent *event) override;
  bool viewportEvent(QEvent *event) override;

private:
  /// What load does before it asks for the first page; the serial of the
  /// read it begins.  Where the pages are \p here already and about to be
  /// taken (takePages), no one is told of a load before they are: its status
  /// and what it folds are said once, as it stands with them.
  quint64 beginLoad(Address function, const QString &representation,
                    std::optional<QJsonObject> options, bool here);
  void request(int offset, quint64 serial);
  /// Takes a page of the text being read, at \p offset: the offset of the
  /// page to take next, or none once the text is whole or cannot be.
  std::optional<int> takePage(const QJsonObject &payload, int offset);
  /// Takes \p pages, the whole text of the function it shows, as a read of
  /// it again brings them, and paints the rows that changed: those rows.
  QRegion takePages(const QVector<QJsonObject> &pages);
  void appendPage(const QJsonObject &payload, int offset);
  /// Adds the source names of a page, \p payload, to those the text has: by
  /// their spelling in the page's \p bytes, which the page names from
  /// \p pageOffset.
  void addSourceNames(const QJsonObject &payload, const PageBytes &bytes,
                      qint64 pageOffset);
  /// Load the function's source without symbolic simplification, saying
  /// \p why in the status line when it arrives.
  void showOrdinary(const QString &why);
  /// The library regions of the loaded function, its prelude and epilogue.
  QJsonArray foldRegions() const;
  /// The source line where the epilogue begins.
  int epilogueLine() const;
  /// The token at a line and column; in LLVM IR, a name with its sigil.
  QString tokenAt(int line, int column) const;
  /// How this page's words take dots: pseudocode, in any of its languages,
  /// splits member accesses; IR keeps dotted names.
  TokenDots tokenDots() const;
  /// A double-click in LLVM IR: a global goes to the image address it stands
  /// for, or else to what the binary names so; any other name goes to the
  /// line that defines it.
  void activateLLVMToken();
  /// Whether goto labels name their blocks' addresses, as HighC writes them.
  bool labelsNameAddresses() const;
  /// Show the definition of the label \p name at \p sourceLine, expanding
  /// the fold that holds it, without moving the disassembly.
  void goToLabel(const QString &name, int sourceLine, Address address);
  /// Display lines from the source, folded where the user asked.
  void rebuildLines(bool append = false);
  /// Position in the displayed text of a line and column.
  int displayPosition(int line, int column) const;
  QString regionAt(const QPoint &position) const;
  QString blockAt(const QPoint &position) const;
  void updateMouseCursor(const QPoint &position);
  void setRegionFolded(const QString &id, bool folded);
  void highlightLine(Line &line, bool &inComment) const;
  void updateMetrics();
  void updateRange();
  /// What every row of the viewport is painted with, beside the theme.
  struct RowFrame {
    int width = 0, height = 0, lineHeight = 0, left = 0, gutterChars = 0;
    qreal charWidth = 0;
    quint64 style = 0;
    bool operator==(const RowFrame &) const = default;
  };
  /// A fill under a row's text, from one column to another.
  struct RowFill {
    int first = 0, last = 0;
    ColorRole role = ColorRole::ListingHighlight;
    bool operator==(const RowFill &) const = default;
  };
  /// All that one row of the viewport is painted from, beside its frame and
  /// the theme.  paintRow is given this and nothing of the view, so that two
  /// rows which compare equal paint the same pixels: a row is painted again
  /// only where this changed (showChanges).  What a row paints from belongs
  /// here, or its row is left as it was painted.
  struct Row {
    /// The number in the margin, one more than the line's own; 0 for a row
    /// below the text, which shows the background alone, and UnknownRow for
    /// one of which nothing is known.
    int number = 0;
    QString text;
    QVector<StyledSpan> spans;
    /// The cursor's line; else, a marked or a folded one.
    bool current = false, tinted = false;
    /// The line heads a block, which is folded or open.
    bool block = false, blockFolded = false;
    /// The fills under the text, in the order they are painted.
    QVarLengthArray<RowFill, 4> fills;
    /// The columns boxed as a pair of brackets.
    QVarLengthArray<int, 2> brackets;
    /// The selected columns; a length below zero where none are.
    int selectionStart = 0, selectionLength = -1;
    /// The column the text cursor is drawn at, or -1.
    int caret = -1;
    bool operator==(const Row &) const = default;
  };
  /// What the rows of one paint share of the view's state.
  struct RowsContext;
  RowFrame rowFrame() const;
  RowsContext rowsContext() const;
  /// What the line \p index is painted from; a row below the text for none.
  Row rowAt(int index, const RowsContext &context) const;
  /// Paints a line over its row's background, which starts at \p y, with
  /// the layout of its text.
  static void paintRow(QPainter &painter, const RowFrame &frame, const Row &row,
                       int y, QTextLayout &layout);
  /// Paints the rows \p region touches over their background: each from what
  /// the viewport was last told it shows where \p told, else from what the
  /// view holds now, \p painted then taking what each row it painted was
  /// painted from, by the row's place in the viewport.
  void paintRows(QPainter &painter, const QRegion &region, bool told,
                 QVector<Row> *painted = nullptr) const;
  /// The number of a row of which nothing is known.
  static constexpr int UnknownRow = -1;
  /// Says that what the view shows changed: every such change ends here, as
  /// it ended in an update of the whole viewport.  What changed is looked for
  /// once the one who says so is done (showChanges), so that it may change
  /// more after saying it, as it might before an update painted.
  void updateView();
  /// Has the viewport paint the rows whose painting changed since it was last
  /// told what they show, and no other.
  void showChanges();
  /// Whether every row is painted from what the viewport was last told: not
  /// so after a change that no one said (updateView), which the view looks
  /// for as it paints.  \p painted holds what the rows just painted were
  /// painted from, the others being unknown.
  bool rowsKnown(const QVector<Row> &painted) const;
  int visibleLines() const;
  int lineAt(int y) const;
  int columnAt(int line, int x) const;
  int textLeft() const;
  void moveCursor(int line, int column, bool extend);

  WheelInput wheel_;
  Session &session_;
  QVector<Line> lines_;
  QVector<int> lineStarts_;
  // The function's complete source text and its row mappings; library
  // folding projects them, copy and export use them unchanged.
  QString source_;
  QVariantList sourceRows_;
  /// What a line takes of a source row: the line the row names, as a row
  /// read by itself names it and as the folded projection reads it, and the
  /// instructions it maps to.
  struct SourceRow {
    int line = -1, projectedLine = 0;
    QVector<Address> addresses;
  };
  /// The rows of sourceRows_, in its order.
  QVector<SourceRow> rowLines_;
  /// A page's rows as the worker gave them and as they are read from that.
  /// A page read again after an edit mostly brings the rows it brought, the
  /// same array: they are read once.
  struct PageRows {
    QJsonArray given;
    QVariantList read;
    QVector<SourceRow> lines;
  };
  /// The pages' rows of the text being loaded or shown, and those of the
  /// text shown before it, until the new one is whole.
  QVector<PageRows> pageRows_, pageRowsBefore_;
  quint64 pageRowsKept_ = 0;
  QJsonArray regions_;
  qint64 byteOffset_ = 0;
  qint64 sourceBytes_ = 0;
  int renderedChars_ = 0, renderedRows_ = 0, widestLine_ = 0;
  bool regionsValid_ = false;
  LibraryCodeView library_;
  std::optional<Address> function_;
  std::optional<Address> pendingAddress_, selectedAddress_;
  /// Where the cursor and the view were when the same function loads again,
  /// after an edit: the source lines of the cursor and of the first shown
  /// line, and the cursor's column. The first pages clamp both, so the
  /// complete text puts them back.
  struct SourcePlace {
    int cursorLine = 0, cursorColumn = 0, topLine = 0;
  };
  std::optional<SourcePlace> placeAfterLoad_;
  /// The places keepPlace kept, newest last, with the text they were in.
  struct KeptPlace {
    Address address = 0, function = 0;
    QString representation;
    QJsonObject options;
    SourcePlace place;
  };
  QVector<KeptPlace> keptPlaces_;
  /// The place kept at \p address, if the code is shown as it was then.
  const KeptPlace *keptPlace(Address address) const;
  SourcePlace currentPlace() const;
  /// The first shown line at or after a source line.
  int shownLine(int sourceLine) const;
  QString representation_, status_, highlight_;
  ViewSearchQuery SearchHighlight;
  /// Why the source on its way is not the one asked for (showOrdinary).
  QString statusNote_;
  QJsonObject sourceOptions_, renderedOptions_;
  QJsonObject RecoveryReport, RecoveryOptions;
  quint64 serial_ = 0;
  quint64 DocumentVersion = 0;
  bool inComment_ = false, loading_ = false, foldAfterLoad_ = false;
  bool complete_ = false;
  int pageLines_ = 0;
  /// A checked name-only refresh keeps the painted document until every
  /// replacement page arrives. Cancellation discards these pages together.
  std::optional<QVector<QJsonObject>> refreshPages_;
  bool interrupted_ = false;
  /// The page was read again ahead of the others that hear of a new revision
  /// (Session::shownRevisionChanged): the word that follows reads it no more.
  bool readAhead_ = false;
  /// The lines shown before the page was read again, whose layouts the lines
  /// that read the same take (rebuildLines); let go of when it is whole.
  QList<Line> linesBefore_;
  quint64 layoutsKept_ = 0;
  quint64 spansKept_ = 0;
  /// A line of the viewport as a rename on its way will write it, if the
  /// worker takes the rename as the view reads it: its text with the new name
  /// in each place the old one is linked, colored and laid out.
  struct ReadyLine {
    int index = 0;
    StyledLine styled;
    /// A comment is open where it ends, as its text was colored.
    bool commentAfter = false;
  };
  /// Those lines, made while the worker reads the rename
  /// (Session::renaming).  The line the answer brings in the place of one
  /// takes its layout where it reads the same under the same spans, which is
  /// then the layout it would build (rebuildLines); any other is laid out as
  /// it is painted, as before.  Let go of once the text is whole again.
  QVector<ReadyLine> readyLines_;
  quint64 readyLinesTaken_ = 0;
  /// Makes ready the lines of the viewport that \p address being called
  /// \p name will write, the first \p most of them.
  void makeReadyFor(Address address, const QString &name, int most);
  /// The pages of the text it shows, as they were taken.
  QVector<QJsonObject> pages_;
  /// A rename with the worker that the text reads ahead of.
  struct Ahead {
    Address address = 0;
    QString name;
    /// The pages it showed before, which it shows again where the worker
    /// refuses the name.
    QVector<QJsonObject> before;
    /// Someone asked for the text to be read again meanwhile, with these
    /// options where they gave some: it is, once the worker has answered.
    bool read = false;
    std::optional<QJsonObject> options;
  };
  std::optional<Ahead> ahead_;
  /// The pages being taken are all here (takePages): none is shown by itself
  /// while the next is on its way.
  bool pagesHere_ = false;
  quint64 readsAhead_ = 0, rowsShownTwice_ = 0;
  /// Reads ahead where the pages say what a rename makes of them; a test
  /// turns it off to see the lines made ready alone.
  bool showsAhead_ = true;
  /// A rename has left for the worker (Session::renaming): the text reads as
  /// the pages say the rename leaves it, where they do (pagesRenamed), and
  /// its lines are made ready where they do not (makeReadyFor).
  void readAheadOf(Address address, const QString &name);
  /// The worker refused that rename (Session::renameRefused).
  void takeBack(Address address, const QString &name);
  bool foldPreludeAfterLoad_ = true;
  bool foldDetailsAfterLoad_ = true;
  QSet<QString> blocksAfterLoad_;
  QString blockStructureAfterLoad_;
  /// The first page's prelude: lines and end_byte.
  QJsonObject prelude_;
  /// The first page's epilogue: lines and begin_byte.
  QJsonObject epilogue_;
  bool foldEpilogueAfterLoad_ = true;
  /// The image addresses the global names of each source line stand for, in
  /// the order the line spells them (LLVM IR).
  struct GlobalReference {
    QString name;
    Address address = 0;
  };
  QHash<int, QVector<GlobalReference>> references_;
  /// The global last activated by its name, whose declaration shows if the
  /// binary does not name it.
  QString unresolvedDeclaration_;
  /// Hint the type of the import a call at \p line and \p column names, and
  /// where its arguments go, as Hex-Rays' hint does, once the worker answers;
  /// false where no call is named.
  bool hintFunction(int line, int column, const QPoint &globalPosition);
  /// This document's hints by the name asked about, empty for a name that
  /// names no function.
  QHash<QString, QString> functionHints_;
  quint64 hintsVersion_ = 0;
  /// The hint on its way, which another hover, a move off its name or a
  /// scroll drops, and the rectangle of its name.
  quint64 hintSerial_ = 0;
  QRect hintRect_;
  /// The language the first page names, and how many declarations it shows
  /// as C because that language could not spell them.
  QString pageLanguage_;
  int unread_ = 0;
  /// The source names of every page by their spelling, and those spellings
  /// by their first character, longest first.
  QHash<QString, SourceName> sourceNames_;
  /// A name an edit can name, where the text spells it: what the page links
  /// it by, as the page gave it, and its spelling.
  struct EditName {
    int begin, end;
    QJsonObject target;
    QString name;
  };
  QVector<EditName> editNames_;
  /// What an edit names \p name by: what the page links it by, and how the
  /// text spells it.
  static QJsonObject targetOf(const EditName &name);
  bool editMetadata_ = false;
  QHash<QChar, QVector<QString>> sourceNameIndex_;
  /// The length of a source name at \p position of \p text, or 0.
  int sourceNameLength(const QString &text, int position) const;
  /// What the code declares, indexed on first use.
  struct Declarations {
    /// Types and macros, by their source lines.
    QHash<QString, int> types;
    /// Functions declared with an assembler label: source line and symbol.
    QHash<QString, std::pair<int, QString>> linked;
    /// Globals declared at image addresses.
    QHash<QString, Address> objects;
  };
  const Declarations &declarations() const;
  mutable std::optional<Declarations> declarations_;
  int cursorLine_ = 0, cursorColumn_ = 0;
  /// Changes whenever the shown lines do, so the bracket pair found for a
  /// cursor place is found again.
  quint64 linesStamp_ = 0;
  mutable std::optional<BracketPair> brackets_;
  mutable std::tuple<int, int, quint64> bracketsAt_{-1, -1, 0};
  std::optional<std::pair<int, int>> anchor_;
  bool selecting_ = false;
  /// Extend the selection to a point of the viewport, as dragging does; a
  /// point beyond an edge reaches the line at that edge.
  void dragTo(const QPoint &position);
  /// Ctrl+A during a load selects the whole text once the last page is in.
  bool selectAllAfterLoad_ = false;
  /// Scrolls the text while a drag holds the mouse beyond an edge.
  QTimer dragScroll_;
  QPoint dragPoint_;
  /// The cursor follows the disassembly, which needs no word of the move.
  bool following_ = false;
  QVector<int> marked_;
  qreal charWidth_ = 8;
  int lineHeight_ = 16, ascent_ = 12, gutterChars_ = 4;
  quint64 styleStamp_ = 1;
  /// What showChanges last told the viewport its rows show.  The window keeps
  /// those pixels, or a paint of the rows that changed is on its way.
  RowFrame knownFrame_;
  QVector<Row> knownRows_;
  /// updateView was called and showChanges is still to look.
  bool viewChanged_ = false;
  /// showChanges has asked for rows that no paint has painted yet: until
  /// someone says another change, what it told the viewport is what there is
  /// to paint.
  bool rowsTold_ = false;
  /// The pixels showChanges has asked the viewport to paint, all told
  /// (ListingView::pixelsAsked_).
  quint64 pixelsAsked_ = 0;
  /// The rows showChanges had painted again since a page read ahead of a new
  /// revision began to be taken (Session::shownRevisionAsked).
  QRegion rowsUpdated_;
  /// The picture its paints add up to, kept where a test checks them.
  PaintCheck::Picture paintChecked_;
  friend struct CodeTextTestAccess;
};

/// A pseudocode or IR window: the representation selector over a CodeText.
class CodeView final : public QWidget {
  Q_OBJECT
public:
  CodeView(Session &session, const QString &representation,
           QWidget *parent = nullptr);
  CodeText *text() const { return text_; }
  void showFunction(Address function);
  void setRepresentation(const QString &representation);
  void addSourceActions(QMenu &menu);
  /// The representation the selector shows, loaded or still to load.
  QString representation() const;
  /// The view stays on its function instead of following the disassembly.
  bool locked() const;
  /// Window title of a representation, such as "Pseudocode".
  static QString titleOf(const QString &representation);
  /// The representation that shows a function in its own language, which
  /// F5 and Tab open.
  static QString pseudocodeRepresentation() { return QStringLiteral("source"); }
  /// Whether a representation shows source code rather than an IR: the
  /// pseudocode window holds it.
  static bool isSource(const QString &representation);
  /// The language other than C that pseudocode in the function's own
  /// language chose for the loaded function, such as "Rust"; empty for C and
  /// for any other representation.
  QString chosenLanguage() const;

signals:
  void representationChanged(const QString &representation);
  /// chosenLanguage() changed.
  void languageChanged();

private:
  void updateStatus();
  /// The representations the loaded program offers: C beside Pseudocode
  /// only for a program with C++, Rust or Go code, whose Pseudocode reads those
  /// functions in their own language.
  void updateRepresentations();
  Session &session_;
  QComboBox *selector_;
  QToolButton *fold_, *lock_, *optimize_;
  QLabel *status_;
  CodeText *text_;
};

} // namespace neverd::gui
