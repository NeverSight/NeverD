#pragma once

#include "Address.h"
#include "LibraryCodeView.h"
#include "StyledText.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>
#include <QWidget>
#include <optional>

class QComboBox;
class QLabel;
class QToolButton;

namespace neverd::gui {

class Session;

/// Text of one function in a code representation: C pseudocode (native or
/// through LLVM) or a textual IR.  Lines are fetched in pages and colored
/// locally; explicit navigation uses the worker's instruction mappings.
class CodeText final : public QAbstractScrollArea {
  Q_OBJECT
public:
  struct Line {
    StyledLine styled;
    QVector<Address> addresses;
    /// The line shows a folded library operation.
    bool folded = false;
  };

  CodeText(Session &session, QWidget *parent = nullptr);

  void load(Address function, const QString &representation);
  void clear();
  /// Stop obsolete requests/pages, including an in-flight analysis process
  /// when no other view still subscribes to its result.
  void cancel();
  std::optional<Address> function() const { return function_; }
  QString representation() const { return representation_; }
  /// The assembly-selected address on the cursor line, or its primary mapping.
  std::optional<Address> currentAddress() const;
  /// Select the first row mapped to \p address, waiting for all pages and
  /// expanding its fold if needed. A new load or cursor move cancels the wait.
  void selectAddress(Address address);
  QString currentToken() const;
  /// The selected source name's image address or declared-local identity.
  std::optional<QJsonObject> renameTarget() const;
  /// The original source row under the cursor, independent of folding.
  std::optional<QJsonObject> commentTarget() const;
  QString selectedText() const;
  QString allText() const;
  bool findText(const QString &text, bool forward);
  /// Move the cursor to \p line and reveal it.
  void setCursorLine(int line) { moveCursor(line, 0, false); }
  int lineCount() const { return int(lines_.size()); }
  /// Recognized library operations that can fold into one-line summaries.
  int foldableCount() const { return library_.foldableCount(); }
  bool anyFolded() const { return library_.anyFolded(); }
  /// Some recognized library operation shows as its summary.
  bool libraryFolded() const { return library_.libraryFolded(); }
  /// C code with the includes and declarations before its definition.
  bool hasPrelude() const;
  bool preludeFolded() const;
  void setPreludeFolded(bool folded);
  /// The source line that declares type or macro \p name in this code: a
  /// typedef, struct, union, enum or #define.  Functions are named in the
  /// binary and navigate there instead.
  std::optional<int> declarationLine(const QString &name) const;
  /// Show the declaration of \p name, expanding the prelude that holds it.
  bool goToDeclaration(const QString &name);
  /// The symbol a function declaration links \p name to with an assembler
  /// label: a C++ function reads by its stem and links by its mangled name.
  std::optional<QString> linkedSymbol(const QString &name) const;
  /// The image address of a global the code declares, as the comment its
  /// writer puts beside the declaration names it.
  std::optional<Address> objectAddress(const QString &name) const;
  void setFolded(bool folded);
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
  void locationChanged(neverd::gui::Address address);
  void addressSelected(neverd::gui::Address address, bool mapped);
  /// A name was double-clicked; the owner resolves and navigates.
  void nameActivated(const QString &name);
  /// A global the code declares at an image address was double-clicked.
  void objectActivated(neverd::gui::Address address);
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

private:
  void request(int offset, quint64 serial);
  void appendPage(const QJsonObject &payload, int offset);
  /// The library regions of the loaded function and its prelude.
  QJsonArray foldRegions() const;
  /// Display lines from the source, folded where the user asked.
  void rebuildLines(bool append = false);
  /// Position in the displayed text of a line and column.
  int displayPosition(int line, int column) const;
  QString regionAt(const QPoint &position) const;
  void highlightLine(Line &line, bool &inComment) const;
  void updateMetrics();
  void updateRange();
  int visibleLines() const;
  int lineAt(int y) const;
  int columnAt(int line, int x) const;
  int textLeft() const;
  void moveCursor(int line, int column, bool extend);

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
  QString representation_, status_, highlight_;
  quint64 serial_ = 0;
  bool inComment_ = false, loading_ = false, foldAfterLoad_ = false;
  bool interrupted_ = false;
  bool foldPreludeAfterLoad_ = true;
  /// The first page's prelude: lines and end_byte.
  QJsonObject prelude_;
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
  QToolButton *fold_, *lock_;
  QLabel *status_;
  CodeText *text_;
};

} // namespace neverd::gui
