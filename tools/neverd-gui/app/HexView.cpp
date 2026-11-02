#include "HexView.h"

#include "../Config.h"
#include "../ResourceLimits.h"
#include "AddressSpace.h"
#include "PatchEditor.h"
#include "Session.h"
#include "Theme.h"

#include <QActionGroup>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>
#include <algorithm>
#include <limits>

namespace neverd::gui {
namespace {
constexpr int BytesPerRow = 16;
/// The most bytes one copy takes: half the chunk cache, so that its bytes
/// stay loaded until the copy completes.
constexpr Address MaxCopyBytes =
    Address(::neverd::limits::gui::kHexViewMaxChunks / 2) *
    ::neverd::limits::gui::kHexViewChunkBytes;
// Columns: address, two spaces, 16 hex bytes ("xx " each, extra space after
// eight), two spaces, 16 characters.
constexpr int HexGroup = 8;
constexpr char TextEncodingKey[] = "hex/textEncoding";
/// What the text column shows for a byte that starts no shown character.
constexpr char HiddenByte = '.';

Address chunkBase(Address address) {
  return address & ~Address(::neverd::limits::gui::kHexViewChunkBytes - 1);
}

/// Where a row's bytes start: after the margin, the address and two spaces.
qreal bytesLeft(int bitness, qreal charWidth) {
  return ::neverd::limits::gui_config::kHexViewMargin +
         ((bitness == 64 ? 16 : 8) + 2) * charWidth;
}
} // namespace

HexView::HexView(Session &session, const AddressSpace &space, QWidget *parent)
    : QAbstractScrollArea(parent), session_(session), space_(space) {
  setFocusPolicy(Qt::StrongFocus);
  setFrameShape(QFrame::NoFrame);
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  textEncoding_ = QSettings().value(TextEncodingKey).toString();
  updateMetrics();
  connect(&Theme::instance(), &Theme::changed, this, [this] {
    updateMetrics();
    viewport()->update();
  });
  connect(&session_, &Session::opened, this, [this] {
    clearChunks();
    loadEncodings();
  });
  connect(&session_, &Session::bytePatchesChanged, viewport(),
          qOverload<>(&QWidget::update));
  connect(&session_, &Session::revisionChanged, this, &HexView::clearChunks);
  connect(&session_, &Session::unloaded, this, [this] {
    current_.reset();
    anchor_.reset();
    addressRow_.reset();
    pendingCopy_.reset();
    clearChunks();
  });
}

void HexView::clearChunks() {
  ++serial_;
  chunks_.clear();
  order_.clear();
  pending_.clear();
  updateRange();
  viewport()->update();
}

void HexView::loadEncodings() {
  session_.read(
      QStringLiteral("string_encodings"), {}, this,
      [this](const QJsonObject &payload) {
        encodings_.clear();
        for (const auto &value : payload.value("items").toArray()) {
          const auto encoding = value.toObject();
          const auto spelling = encoding.value("spelling").toString();
          encodings_.append(
              {encoding.value("name").toString(),
               spelling.isEmpty() ? QStringLiteral("ASCII") : spelling,
               encoding.value("legacy").toBool()});
        }
      },
      [](const QString &, const QString &) {});
}

void HexView::setTextEncoding(const QString &name) {
  if (name == textEncoding_)
    return;
  textEncoding_ = name;
  QSettings settings;
  if (name.isEmpty())
    settings.remove(TextEncodingKey);
  else
    settings.setValue(TextEncodingKey, name);
  clearChunks();
}

void HexView::setExportAction(QAction *action) {
  ExportAction = action;
  addAction(action);
}

void HexView::contextMenuEvent(QContextMenuEvent *event) {
  setFocus(Qt::MouseFocusReason);
  QMenu menu(this);
  emit populateContextMenu(&menu);
  if (ExportAction) {
    menu.addAction(ExportAction);
    menu.addSeparator();
  }
  auto *encodings = menu.addMenu(tr("Text encoding"));
  auto *group = new QActionGroup(encodings);
  const auto add = [&](const QString &label, const QString &name) {
    auto *action = encodings->addAction(label);
    action->setCheckable(true);
    action->setChecked(textEncoding_ == name);
    group->addAction(action);
    connect(action, &QAction::triggered, this,
            [this, name] { setTextEncoding(name); });
  };
  // Plain ASCII needs no engine; Unicode encodings come before code pages.
  add(QStringLiteral("ASCII"), {});
  for (const auto &encoding : encodings_)
    if (!encoding.legacy && encoding.label != QLatin1String("ASCII"))
      add(encoding.label, encoding.name);
  encodings->addSeparator();
  for (const auto &encoding : encodings_)
    if (encoding.legacy)
      add(encoding.label, encoding.name);
  menu.exec(event->globalPos());
}

void HexView::updateMetrics() {
  const QFontMetricsF metrics(Theme::instance().codeFont());
  charWidth_ = metrics.horizontalAdvance(QLatin1Char('0'));
  lineHeight_ = int(std::ceil(metrics.lineSpacing()));
  ascent_ = int(std::ceil(metrics.ascent()));
  updateRange();
}

int HexView::visibleRows() const {
  return std::max(1, viewport()->height() / std::max(1, lineHeight_));
}

void HexView::buildRows() {
  rows_.clear();
  rowCount_ = 0;
  for (const auto &region : space_.regions()) {
    if (region.end <= region.start)
      continue;
    const Address start = region.start & ~Address(BytesPerRow - 1);
    const Address last = (region.end - 1) & ~Address(BytesPerRow - 1);
    // Regions sharing or abutting a row extend the run they meet.
    if (!rows_.isEmpty() && start <= rows_.back().end) {
      rows_.back().end = std::max(rows_.back().end, last + BytesPerRow);
      continue;
    }
    rows_.append({start, last + BytesPerRow, 0});
  }
  for (auto &run : rows_) {
    run.first = rowCount_;
    rowCount_ += qint64((run.end - run.start) / BytesPerRow);
  }
}

qint64 HexView::rowOf(Address address) const {
  for (const auto &run : rows_)
    if (address >= run.start && address < run.end)
      return run.first + qint64((address - run.start) / BytesPerRow);
  return 0;
}

std::optional<Address> HexView::addressOfRow(qint64 row) const {
  const auto run = std::upper_bound(
      rows_.begin(), rows_.end(), row,
      [](qint64 value, const RowRun &r) { return value < r.first; });
  if (run == rows_.begin())
    return std::nullopt;
  const auto &found = *std::prev(run);
  const Address address =
      found.start + Address(row - found.first) * BytesPerRow;
  return address < found.end ? std::optional(address) : std::nullopt;
}

void HexView::updateRange() {
  buildRows();
  const qint64 rows = totalRows();
  auto *bar = verticalScrollBar();
  bar->setRange(0,
                int(std::min<qint64>(std::max<qint64>(0, rows - visibleRows()),
                                     std::numeric_limits<int>::max())));
  bar->setPageStep(visibleRows());
  bar->setSingleStep(1);
  // A pane narrower than a row scrolls sideways to the text column.
  const qreal rowWidth = bytesLeft(session_.bitness(), charWidth_) +
                         (BytesPerRow * 3 + 2 + BytesPerRow) * charWidth_ +
                         ::neverd::limits::gui_config::kHexViewMargin;
  auto *horizontal = horizontalScrollBar();
  horizontal->setRange(
      0, std::max(0, int(std::ceil(rowWidth)) - viewport()->width()));
  horizontal->setPageStep(viewport()->width());
  horizontal->setSingleStep(int(std::ceil(charWidth_)));
}

void HexView::resizeEvent(QResizeEvent *event) {
  QAbstractScrollArea::resizeEvent(event);
  updateRange();
}

void HexView::scrollContentsBy(int, int) { viewport()->update(); }

Address HexView::chunkKey(Address address) const {
  const Address base = chunkBase(address);
  const auto *region = space_.regionOf(address);
  return region && region->start > base ? region->start : base;
}

const HexView::Chunk *HexView::chunk(Address key) const {
  if (auto it = chunks_.constFind(key); it != chunks_.cend())
    return &*it;
  request(key);
  return nullptr;
}

void HexView::request(Address key) const {
  if (pending_.contains(key) || !session_.loaded())
    return;
  // Reading stops at the first unmapped byte, so a chunk ends with its
  // region: bytes past a gap belong to the next region's chunk.
  Address end = chunkBase(key) + ::neverd::limits::gui::kHexViewChunkBytes;
  if (const auto *region = space_.regionOf(key))
    end = std::min(end, region->end);
  const Address base = key;
  pending_.insert(base);
  const quint64 serial = serial_;
  auto *self = const_cast<HexView *>(this);
  QJsonObject request{{"address", hexAddress(base)},
                      {"size", qint64(end - base)}};
  // A character split across two chunks decodes as two hidden halves.
  if (!textEncoding_.isEmpty())
    request.insert("text_encoding", textEncoding_);
  session_.read(
      QStringLiteral("bytes"), request, self,
      [self, base, serial](const QJsonObject &payload) {
        if (serial != self->serial_)
          return;
        self->pending_.remove(base);
        Chunk chunk;
        chunk.data =
            QByteArray::fromHex(payload.value("data").toString().toLatin1());
        chunk.mapped = int(chunk.data.size());
        for (const auto &cell : payload.value("cells").toArray())
          chunk.cells.append(cell.isNull() ? QString(QLatin1Char(HiddenByte))
                                           : cell.toString());
        self->chunks_.insert(base, chunk);
        self->order_.push_front(base);
        while (int(self->order_.size()) >
               ::neverd::limits::gui::kHexViewMaxChunks) {
          self->chunks_.remove(self->order_.back());
          self->order_.pop_back();
        }
        self->viewport()->update();
        self->completeCopy();
      },
      [self, base, serial](const QString &code, const QString &) {
        if (serial != self->serial_)
          return;
        self->pending_.remove(base);
        // An encoding this engine does not know, or an engine that decodes
        // no text, reads as ASCII again.
        if (code == QLatin1String("unsupported_encoding") ||
            code == QLatin1String("unsupported"))
          self->setTextEncoding({});
      });
}

void HexView::addressSpaceChanged() {
  updateRange();
  if (current_)
    setCurrent(*current_, currentSize_);
  else
    viewport()->update();
}

std::optional<quint8> HexView::byteAt(Address address) const {
  const auto it = chunks_.constFind(chunkKey(address));
  if (it == chunks_.cend())
    return std::nullopt;
  const int offset = int(address - it.key());
  if (offset >= it->mapped)
    return std::nullopt;
  return static_cast<quint8>(it->data.at(offset));
}

std::optional<QString> HexView::textAt(Address address) const {
  const auto it = chunks_.constFind(chunkKey(address));
  if (it == chunks_.cend())
    return std::nullopt;
  const int offset = int(address - it.key());
  if (offset >= it->mapped)
    return std::nullopt;
  if (!it->cells.isEmpty())
    return it->cells.at(offset);
  const auto value = static_cast<unsigned char>(it->data.at(offset));
  return QString(value >= 0x20 && value < 0x7f ? QChar(value)
                                               : QLatin1Char(HiddenByte));
}

void HexView::setCurrent(Address address, int size) {
  // The disassembly echoes a move it follows; only a move elsewhere ends the
  // selection.
  if (current_ != address) {
    anchor_.reset();
    addressRow_.reset();
  }
  current_ = address;
  currentSize_ = std::max(1, size);
  const qint64 row = rowOf(address);
  auto *bar = verticalScrollBar();
  if (row < bar->value() || row >= bar->value() + visibleRows())
    bar->setValue(int(std::max<qint64>(0, row - visibleRows() / 3)));
  viewport()->update();
}

void HexView::paintEvent(QPaintEvent *) {
  QPainter painter(viewport());
  const auto &theme = Theme::instance();
  painter.fillRect(viewport()->rect(), theme.color(ColorRole::HexBackground));
  if (space_.empty())
    return;
  painter.translate(-horizontalScrollBar()->value(), 0);
  painter.setFont(theme.codeFont());
  const int digits = session_.bitness() == 64 ? 16 : 8;
  const qreal hexLeft =
      ::neverd::limits::gui_config::kHexViewMargin + (digits + 2) * charWidth_;
  const qreal asciiLeft = hexLeft + (BytesPerRow * 3 + 2) * charWidth_;
  const qint64 first = verticalScrollBar()->value();
  const auto Selected = selection();
  for (int i = 0; i <= visibleRows(); ++i) {
    const auto rowAddress = addressOfRow(first + i);
    if (!rowAddress)
      break;
    const int y = i * lineHeight_;
    if (addressRow_ == rowAddress)
      painter.fillRect(QRectF(::neverd::limits::gui_config::kHexViewMargin, y,
                              digits * charWidth_, lineHeight_),
                       theme.color(ColorRole::ListingSelection));
    painter.setPen(theme.color(ColorRole::HexAddress));
    painter.drawText(
        QPointF(::neverd::limits::gui_config::kHexViewMargin, y + ascent_),
        displayAddress(*rowAddress, digits));
    const auto isCurrent = [&](Address address) {
      return current_ && address >= *current_ &&
             address < *current_ + Address(currentSize_);
    };
    const auto isSelected = [&](Address address) {
      return Selected && address >= Selected->first &&
             address <= Selected->second;
    };
    // Highlights go first: a wide character's glyph spans the next cells.
    for (int b = 0; b < BytesPerRow; ++b) {
      const Address address = *rowAddress + b;
      const bool current = isCurrent(address);
      const bool patched = session_.hasBytePatch(address);
      if ((!current && !isSelected(address) && !patched) ||
          !space_.regionOf(address))
        continue;
      const QColor fill =
          theme.color(patched   ? (current || isSelected(address)
                                       ? ColorRole::ListingPatchedSelection
                                       : ColorRole::HexPatched)
                      : current ? ColorRole::HexCurrent
                                : ColorRole::ListingSelection);
      const qreal x = hexLeft + (b * 3 + (b >= HexGroup ? 1 : 0)) * charWidth_;
      painter.fillRect(
          QRectF(x - charWidth_ / 4, y, charWidth_ * 2.5, lineHeight_), fill);
      painter.fillRect(
          QRectF(asciiLeft + b * charWidth_, y, charWidth_, lineHeight_), fill);
    }
    for (int b = 0; b < BytesPerRow; ++b) {
      const Address address = *rowAddress + b;
      // A byte no region maps stays blank, as between two sections.
      if (!space_.regionOf(address))
        continue;
      const qreal x = hexLeft + (b * 3 + (b >= HexGroup ? 1 : 0)) * charWidth_;
      const qreal ax = asciiLeft + b * charWidth_;
      const Address key = chunkKey(address);
      const Chunk *source = chunk(key);
      const int offset = int(address - key);
      const bool mapped = source && offset < source->mapped;
      const bool current =
          isCurrent(address) && !session_.hasBytePatch(address);
      if (!source) {
        painter.setPen(theme.color(ColorRole::HexUnmapped));
        painter.drawText(QPointF(x, y + ascent_), QStringLiteral(".."));
        continue;
      }
      if (!mapped) {
        painter.setPen(theme.color(ColorRole::HexUnmapped));
        painter.drawText(QPointF(x, y + ascent_), QStringLiteral("??"));
        continue;
      }
      const auto value = static_cast<unsigned char>(source->data.at(offset));
      painter.setPen(current ? theme.color(ColorRole::HexCurrentText)
                     : value ? theme.color(ColorRole::HexByte)
                             : theme.color(ColorRole::HexZero));
      if (session_.hasBytePatch(address))
        painter.drawLine(QPointF(x, y + lineHeight_ - 1),
                         QPointF(x + 2 * charWidth_, y + lineHeight_ - 1));
      painter.drawText(
          QPointF(x, y + ascent_),
          QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0')).toUpper());
      painter.setPen(current ? theme.color(ColorRole::HexCurrentText)
                             : theme.color(ColorRole::HexAscii));
      if (!source->cells.isEmpty()) {
        // A character's continuation bytes show nothing of their own.
        if (const auto &cell = source->cells.at(offset); !cell.isEmpty())
          painter.drawText(QPointF(ax, y + ascent_), cell);
        continue;
      }
      const QChar character = value >= 0x20 && value < 0x7f
                                  ? QChar(value)
                                  : QLatin1Char(HiddenByte);
      painter.drawText(QPointF(ax, y + ascent_), QString(character));
    }
  }
}

void HexView::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_F2 && event->modifiers() == Qt::NoModifier) {
    editByte();
    event->accept();
    return;
  }
  if (event->matches(QKeySequence::Copy)) {
    copySelection();
    return;
  }
  if (!current_) {
    QAbstractScrollArea::keyPressEvent(event);
    return;
  }
  Address next = *current_;
  switch (event->key()) {
  case Qt::Key_Left:
    next = next ? next - 1 : 0;
    break;
  case Qt::Key_Right:
    next += 1;
    break;
  case Qt::Key_Up:
    next = next >= BytesPerRow ? next - BytesPerRow : next;
    break;
  case Qt::Key_Down:
    next += BytesPerRow;
    break;
  case Qt::Key_PageUp:
    next = next >= Address(BytesPerRow * visibleRows())
               ? next - BytesPerRow * visibleRows()
               : 0;
    break;
  case Qt::Key_PageDown:
    next += Address(BytesPerRow * visibleRows());
    break;
  default:
    QAbstractScrollArea::keyPressEvent(event);
    return;
  }
  if (!space_.regionOf(next))
    return;
  moveCurrent(next, event->modifiers() & Qt::ShiftModifier);
  // A pane scrolled sideways keeps the byte in view in one of its columns.
  auto *horizontal = horizontalScrollBar();
  const int byte = int(next % BytesPerRow);
  const qreal hex = bytesLeft(session_.bitness(), charWidth_) +
                    (byte * 3 + (byte >= HexGroup ? 1 : 0)) * charWidth_;
  const qreal text = bytesLeft(session_.bitness(), charWidth_) +
                     (BytesPerRow * 3 + 2 + byte) * charWidth_;
  const auto shown = [&](qreal left, qreal width) {
    return left >= horizontal->value() &&
           left + width <= horizontal->value() + viewport()->width();
  };
  if (!shown(hex, 2 * charWidth_) && !shown(text, charWidth_))
    horizontal->setValue(hex < horizontal->value()
                             ? int(std::floor(hex))
                             : int(std::ceil(hex + 2 * charWidth_)) -
                                   viewport()->width());
  emit locationChanged(next);
}

std::optional<Address> HexView::addressAt(const QPointF &position) const {
  const int digits = session_.bitness() == 64 ? 16 : 8;
  const qreal hexLeft =
      ::neverd::limits::gui_config::kHexViewMargin + (digits + 2) * charWidth_;
  const qreal asciiLeft = hexLeft + (BytesPerRow * 3 + 2) * charWidth_;
  const int rowIndex = int(position.y()) / std::max(1, lineHeight_);
  const auto rowAddress = addressOfRow(verticalScrollBar()->value() + rowIndex);
  if (!rowAddress)
    return std::nullopt;
  const qreal x = position.x() + horizontalScrollBar()->value();
  int byte = -1;
  if (x >= asciiLeft)
    byte = int((x - asciiLeft) / charWidth_);
  else if (x >= hexLeft) {
    qreal column = (x - hexLeft) / charWidth_;
    if (column >= HexGroup * 3)
      column -= 1;
    byte = int(column / 3);
  }
  if (byte < 0 || byte >= BytesPerRow)
    return std::nullopt;
  const Address address = *rowAddress + Address(byte);
  if (!space_.regionOf(address))
    return std::nullopt;
  return address;
}

std::optional<Address> HexView::rowAddressAt(const QPointF &position) const {
  const int digits = session_.bitness() == 64 ? 16 : 8;
  const qreal left = ::neverd::limits::gui_config::kHexViewMargin;
  const qreal x = position.x() + horizontalScrollBar()->value();
  if (x < left || x >= left + digits * charWidth_)
    return std::nullopt;
  return addressOfRow(verticalScrollBar()->value() +
                      int(position.y()) / std::max(1, lineHeight_));
}

void HexView::moveCurrent(Address address, bool extend) {
  const std::optional<Address> anchor =
      extend ? (anchor_ ? anchor_ : current_) : std::nullopt;
  setCurrent(address, 1);
  anchor_ = anchor;
}

std::optional<std::pair<Address, Address>> HexView::selection() const {
  if (!anchor_ || !current_ || *anchor_ == *current_)
    return std::nullopt;
  return std::pair{std::min(*anchor_, *current_),
                   std::max(*anchor_, *current_)};
}

void HexView::mousePressEvent(QMouseEvent *event) {
  setFocus(Qt::MouseFocusReason);
  // A plain click on a row's address goes to the row and selects the address
  // for Ctrl+C, as a click on a disassembly line's prefix does.
  if (const auto row = rowAddressAt(event->position());
      row &&
      !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
    for (int b = 0; b < BytesPerRow; ++b)
      if (const Address address = *row + Address(b); space_.regionOf(address)) {
        moveCurrent(address, false);
        addressRow_ = row;
        emit locationChanged(address);
        return;
      }
    return;
  }
  const auto address = addressAt(event->position());
  if (!address)
    return;
  addressRow_.reset();
  // A press starts a selection a drag extends; Shift extends the current one.
  const bool extend = event->modifiers() & Qt::ShiftModifier;
  moveCurrent(*address, extend);
  if (!extend)
    anchor_ = *address;
  emit locationChanged(*address);
}

void HexView::mouseMoveEvent(QMouseEvent *event) {
  if (!(event->buttons() & Qt::LeftButton) || !anchor_)
    return;
  if (const auto address = addressAt(event->position());
      address && address != current_) {
    current_ = *address;
    currentSize_ = 1;
    viewport()->update();
  }
}

void HexView::copySelection() {
  if (addressRow_) {
    QGuiApplication::clipboard()->setText(
        displayAddress(*addressRow_, session_.bitness() == 64 ? 16 : 8));
    return;
  }
  const auto selected = byteSelection();
  if (!selected)
    return;
  const auto range = *selected;
  if (range.second - range.first >= MaxCopyBytes) {
    emit session_.message(tr("Copy takes at most %1 bytes; select fewer.")
                              .arg(qulonglong(MaxCopyBytes)),
                          2);
    return;
  }
  pendingCopy_ = range;
  completeCopy();
}

std::optional<std::pair<Address, Address>> HexView::byteSelection() const {
  if (const auto selected = selection())
    return selected;
  if (!current_ || currentSize_ <= 0 ||
      Address(currentSize_ - 1) >
          std::numeric_limits<Address>::max() - *current_)
    return std::nullopt;
  return std::pair{*current_, *current_ + Address(currentSize_) - 1};
}

void HexView::completeCopy() {
  if (!pendingCopy_)
    return;
  const auto [first, last] = *pendingCopy_;
  QString text;
  text.reserve(int((last - first + 1) * 3));
  for (Address address = first;; ++address) {
    const auto value = byteAt(address);
    if (!value) {
      // The rest loads first; an unmapped byte cannot be copied at all.
      if (!space_.regionOf(address)) {
        pendingCopy_.reset();
        emit session_.message(
            tr("The selection holds bytes no segment maps; nothing was "
               "copied."),
            2);
        return;
      }
      for (Address key = chunkKey(address); key <= last;
           key = chunkKey(chunkBase(key) +
                          ::neverd::limits::gui::kHexViewChunkBytes))
        if (!chunks_.contains(key))
          request(key);
      return;
    }
    if (!text.isEmpty())
      text += QLatin1Char(' ');
    text += QStringLiteral("%1").arg(*value, 2, 16, QLatin1Char('0')).toUpper();
    if (address == last)
      break;
  }
  pendingCopy_.reset();
  QGuiApplication::clipboard()->setText(text);
}

void HexView::wheelEvent(QWheelEvent *event) {
  if (wheel_.zoomCodeFont(*event, lineHeight_))
    return;
  QAbstractScrollArea::wheelEvent(event);
}

QRect HexView::cursorViewportRect() const {
  if (!current_)
    return {};
  const int byte = int(*current_ % BytesPerRow);
  const qreal x = bytesLeft(session_.bitness(), charWidth_) +
                  (byte * 3 + (byte >= HexGroup ? 1 : 0)) * charWidth_ -
                  horizontalScrollBar()->value();
  return QRect(qRound(x),
               int(rowOf(*current_) - verticalScrollBar()->value()) *
                   lineHeight_,
               qRound(2 * charWidth_), lineHeight_);
}

void HexView::editByte() {
  if (!current_ || session_.readOnly() || !session_.keepsBytePatches())
    return;
  const auto byte = byteAt(*current_);
  if (!byte)
    return;
  if (auto *old = viewport()->findChild<PatchEditor *>())
    old->deleteLater();
  auto *editor =
      new PatchEditor(session_, *current_, "bytes",
                      QStringLiteral("%1").arg(*byte, 2, 16, QLatin1Char('0')),
                      viewport(), true);
  connect(editor, &PatchEditor::cancelled, this, [this, editor] {
    editor->deleteLater();
    setFocus();
  });
  connect(editor, &PatchEditor::applied, this, [this, editor] {
    editor->deleteLater();
    setFocus();
    if (current_)
      moveCurrent(*current_ + 1, false);
  });
  editor->openAt(cursorViewportRect());
}

} // namespace neverd::gui
