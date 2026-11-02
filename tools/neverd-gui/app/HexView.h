#pragma once

#include "Address.h"
#include "WheelInput.h"

#include <QAbstractScrollArea>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <list>
#include <optional>

class QAction;
class QMenu;

namespace neverd::gui {

class AddressSpace;
class Session;
class PatchEditor;

/// The hex dump: rows of sixteen bytes at addresses that are multiples of
/// sixteen, over every mapped region, fetched from the worker in cached
/// chunks.  Regions sharing a row share it; bytes no region maps are blank. The
/// current item is highlighted and follows the synchronized disassembly view.
/// The text column reads the bytes as ASCII or, decoded by the engine, in a
/// chosen text encoding.
class HexView final : public QAbstractScrollArea {
  Q_OBJECT
public:
  HexView(Session &session, const AddressSpace &space,
          QWidget *parent = nullptr);

  /// Show \p address, highlighting \p size bytes; a move elsewhere clears
  /// the selection.
  void setCurrent(Address address, int size = 1);
  /// The selected bytes, first and last, when more than the current one are
  /// selected: by dragging, shift-clicking or Shift and the arrow keys.
  std::optional<std::pair<Address, Address>> selection() const;
  std::optional<std::pair<Address, Address>> byteSelection() const;
  /// Copy the row address a click selected, else the selected bytes, or the
  /// current item's, as hex text (`48 89 5C 24 08`) once every one of them
  /// has loaded.
  void copySelection();
  void setExportAction(QAction *action);
  void editByte();
  std::optional<Address> currentAddress() const { return current_; }
  QRect cursorViewportRect() const;
  /// Map rows again after the regions of the address space changed.
  void addressSpaceChanged();
  /// The byte at \p address once it is loaded and mapped.
  std::optional<quint8> byteAt(Address address) const;
  /// What the text column shows at \p address once it is loaded: a
  /// character, nothing for a character's later bytes, or a dot.
  std::optional<QString> textAt(Address address) const;
  /// The engine encoding the text column reads, empty for plain ASCII.
  QString textEncoding() const { return textEncoding_; }
  /// Read the text column in \p name, or as ASCII when it is empty; the
  /// choice is kept for later sessions.
  void setTextEncoding(const QString &name);

signals:
  void populateContextMenu(QMenu *menu);
  void locationChanged(neverd::gui::Address address);

protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void scrollContentsBy(int dx, int dy) override;
  void contextMenuEvent(QContextMenuEvent *event) override;

private:
  struct Chunk {
    QByteArray data;
    int mapped = 0;
    /// With a text encoding, the text shown at each byte: a character at its
    /// first byte, nothing at the rest, and a dot where none is shown.
    QStringList cells;
  };
  struct TextEncoding {
    QString name, label;
    bool legacy = false;
  };
  /// A run of consecutive rows: the 16-byte lines from \c start to \c end
  /// that regions touch, and the index of its first row.
  struct RowRun {
    Address start = 0, end = 0;
    qint64 first = 0;
  };
  void buildRows();
  qint64 totalRows() const { return rowCount_; }
  /// Row index of \p address.
  qint64 rowOf(Address address) const;
  std::optional<Address> addressOfRow(qint64 row) const;
  /// Where the chunk holding \p address starts: its 4 KiB block, clipped to
  /// the start of the address's region, so that a chunk reads one region.
  Address chunkKey(Address address) const;
  const Chunk *chunk(Address key) const;
  void request(Address key) const;
  void clearChunks();
  void loadEncodings();
  /// The byte drawn at \p position, if any.
  std::optional<Address> addressAt(const QPointF &position) const;
  /// The row whose address \p position is on, if it is on its digits.
  std::optional<Address> rowAddressAt(const QPointF &position) const;
  /// Move the current byte, extending the selection from where it started.
  void moveCurrent(Address address, bool extend);
  /// Put the pending copy on the clipboard once all its bytes are loaded.
  void completeCopy();
  void updateRange();
  void updateMetrics();
  int visibleRows() const;

  WheelInput wheel_;
  Session &session_;
  QAction *ExportAction = nullptr;
  const AddressSpace &space_;
  mutable QHash<Address, Chunk> chunks_;
  mutable std::list<Address> order_;
  mutable QSet<Address> pending_;
  std::optional<Address> current_;
  int currentSize_ = 1;
  /// Where the selection started; it ends at the current byte.
  std::optional<Address> anchor_;
  /// The row whose address a click selected; Ctrl+C copies it.
  std::optional<Address> addressRow_;
  /// The bytes a copy waits for, first and last.
  std::optional<std::pair<Address, Address>> pendingCopy_;
  QString textEncoding_;
  QVector<TextEncoding> encodings_;
  QVector<RowRun> rows_;
  qint64 rowCount_ = 0;
  qreal charWidth_ = 8;
  int lineHeight_ = 16, ascent_ = 12;
  quint64 serial_ = 0;
};

} // namespace neverd::gui
