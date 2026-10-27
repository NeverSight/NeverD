#pragma once

#include "Address.h"
#include "LibraryCodeView.h"
#include "StyledText.h"
#include "ViewSearch.h"
#include "WheelInput.h"

#include "neverd/support/IntegerLiterals.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <optional>

class QComboBox;
class QLabel;
class QToolButton;
class QMenu;

namespace neverd::gui {

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
  };

  CodeText(Session &session, QWidget *parent = nullptr);

  void load(Address function, const QString &representation,
            std::optional<QJsonObject> options = std::nullopt);
  const QJsonObject &sourceOptions() const { return sourceOptions_; }
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
  void request(int offset, quint64 serial);
  void appendPage(const QJsonObject &payload, int offset);
  /// Load the function's source without symbolic simplification, saying
  /// \p why in the status line when it arrives.
  void showOrdinary(const QString &why);
  /// The library regions of the loaded function, its prelude and epilogue.
  QJsonArray foldRegions() const;
  /// The source line where the epilogue begins.
  int epilogueLine() const;
  /// The token at a line and column; in LLVM IR, a name with its sigil.
  QString tokenAt(int line, int column) const;
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
  QString representation_, status_, highlight_;
  ViewSearchQuery SearchHighlight;
  /// Why the source on its way is not the one asked for (showOrdinary).
  QString statusNote_;
  QJsonObject sourceOptions_, renderedOptions_;
  quint64 serial_ = 0;
  quint64 DocumentVersion = 0;
  bool inComment_ = false, loading_ = false, foldAfterLoad_ = false;
  bool complete_ = false;
  int pageLines_ = 0;
  /// A checked name-only refresh keeps the painted document until every
  /// replacement page arrives. Cancellation discards these pages together.
  std::optional<QVector<QJsonObject>> refreshPages_;
  bool interrupted_ = false;
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
  /// The language the first page names, and how many declarations it shows
  /// as C because that language could not spell them.
  QString pageLanguage_;
  int unread_ = 0;
  /// The source names of every page by their spelling, and those spellings
  /// by their first character, longest first.
  QHash<QString, SourceName> sourceNames_;
  struct EditName {
    int begin, end;
    QJsonObject target;
  };
  QVector<EditName> editNames_;
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
