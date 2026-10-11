#include "ListingView.h"

#include "AddressSpace.h"
#include "Session.h"
#include "Theme.h"
#include "WorkerCodes.h"

#include <QApplication>
#include <QClipboard>
#include <QHelpEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTextLine>
#include <QToolTip>
#include <algorithm>
#include <unordered_map>

namespace neverd::gui {
namespace {
constexpr int PageLines = 240;
constexpr int PrefetchMargin = 80;
constexpr int MaxLoadedLines = 6000;
constexpr int JumpContextBefore = 48;
constexpr int ScrollResolution = 1 << 20;
constexpr int ScrollDebounceMs = 15;
constexpr int WheelLines = 3;
constexpr int MaxArrowLevels = 10;
constexpr int HintLines = 12;
constexpr int TextMargin = 6;
// A line's body runs to about this many characters after its prefix.
constexpr int BodyChars = 120;
constexpr const char *ContentKinds[] = {"insn", "data"};

bool contentKind(const QString &kind) {
  for (const char *content : ContentKinds)
    if (kind == QLatin1String(content))
      return true;
  return false;
}
} // namespace

ListingView::ListingView(Session &session, const AddressSpace &space,
                         QWidget *parent)
    : QAbstractScrollArea(parent), session_(session), space_(space) {
  setFocusPolicy(Qt::StrongFocus);
  setFrameShape(QFrame::NoFrame);
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  viewport()->setCursor(Qt::IBeamCursor);
  viewport()->setMouseTracking(true);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scrollJump_.setSingleShot(true);
  scrollJump_.setInterval(ScrollDebounceMs);
  connect(&scrollJump_, &QTimer::timeout, this, [this] {
    request(Fetch::Jump, pendingScrollAddress_, 0, PageLines / 2, PageLines);
  });
  connect(verticalScrollBar(), &QScrollBar::actionTriggered, this,
          [this](int action) {
            if (updatingScrollBar_)
              return;
            switch (action) {
            case QAbstractSlider::SliderSingleStepAdd:
              scrollLines(1);
              break;
            case QAbstractSlider::SliderSingleStepSub:
              scrollLines(-1);
              break;
            case QAbstractSlider::SliderPageStepAdd:
              scrollLines(visibleLines() - 1);
              break;
            case QAbstractSlider::SliderPageStepSub:
              scrollLines(-(visibleLines() - 1));
              break;
            case QAbstractSlider::SliderToMinimum:
              jumpTo(space_.first());
              break;
            case QAbstractSlider::SliderToMaximum:
              jumpTo(space_.last());
              break;
            case QAbstractSlider::SliderMove: {
              if (space_.empty())
                break;
              const auto position = verticalScrollBar()->sliderPosition();
              const long double fraction =
                  static_cast<long double>(position) / ScrollResolution;
              pendingScrollAddress_ = space_.addressAt(
                  static_cast<Address>(fraction * space_.total()));
              scrollJump_.start();
              return; // Keep the dragged slider position.
            }
            default:
              break;
            }
            QTimer::singleShot(0, this, &ListingView::updateScrollBar);
          });
  connect(&Theme::instance(), &Theme::changed, this, [this] {
    ++styleStamp_;
    updateMetrics();
    viewport()->update();
  });
  connect(&session_, &Session::generationChanged, this, &ListingView::refresh);
  connect(&session_, &Session::opened, this,
          [this] { contentReported_ = false; });
  connect(&session_, &Session::revisionChanged, this, &ListingView::refresh);
  updateMetrics();
}

//===----------------------------------------------------------------------===//
// Data
//===----------------------------------------------------------------------===//

void ListingView::jumpTo(Address address) {
  scrollJump_.stop();
  pendingJump_ = address;
  request(Fetch::Jump, address, 0, JumpContextBefore, PageLines);
}

std::optional<Address> ListingView::topItem() const {
  if (lines_.empty())
    return std::nullopt;
  return lines_[std::clamp<std::size_t>(top_, 0, lines_.size() - 1)].item;
}

void ListingView::refresh() {
  if (!session_.loaded())
    return;
  // A refresh must not supersede a jump that has not arrived yet.
  if (pendingJump_) {
    request(Fetch::Jump, *pendingJump_, 0, JumpContextBefore, PageLines);
    return;
  }
  if (lines_.empty())
    return;
  const auto &top = lines_[std::clamp<std::size_t>(top_, 0, lines_.size() - 1)];
  std::optional<CursorPlace> cursor;
  if (const auto *line = cursorLine()) {
    cursor = CursorPlace{line->item, line->sub};
    cursor->visible =
        cursorLine_ >= top_ && cursorLine_ < top_ + visibleLines();
    if (contentKind(line->kind)) {
      cursor->content = 0;
      for (int i = cursorLine_ - 1; i >= 0 && lines_[i].item == line->item; --i)
        cursor->content += contentKind(lines_[i].kind);
    }
  }
  request(Fetch::Refresh, top.item, top.sub, JumpContextBefore, PageLines,
          cursor);
}

void ListingView::request(Fetch kind, Address address, int sub, int before,
                          int after, std::optional<CursorPlace> cursor) {
  if (!session_.loaded())
    return;
  if (kind == Fetch::Jump || kind == Fetch::Refresh) {
    ++serial_;
    fetchingBefore_ = fetchingAfter_ = false;
  } else if (kind == Fetch::Before) {
    fetchingBefore_ = true;
  } else {
    fetchingAfter_ = true;
  }
  const quint64 serial = serial_;
  QJsonObject payload{{"address", hexAddress(address)},
                      {"sub", sub},
                      {"before", before},
                      {"after", after}};
  if (opcodeBytes_ > 0)
    payload["opcode_bytes"] = opcodeBytes_;
  session_.read(
      QStringLiteral("listing"), payload, this,
      [this, kind, serial, cursor, address](const QJsonObject &result) {
        accept(kind, result, serial, cursor, address);
      },
      [this, kind, serial](const QString &, const QString &message) {
        if (serial != serial_)
          return;
        if (kind == Fetch::Jump) {
          pendingJump_.reset();
          emit jumpSettled();
        }
        if (kind == Fetch::Before)
          fetchingBefore_ = false;
        else if (kind == Fetch::After)
          fetchingAfter_ = false;
        emit session_.message(message, 1);
      });
}

void ListingView::accept(Fetch kind, const QJsonObject &payload, quint64 serial,
                         std::optional<CursorPlace> cursor,
                         Address jumpAddress) {
  if (serial != serial_)
    return;
  const bool settledJump = kind == Fetch::Jump;
  if (settledJump)
    pendingJump_.reset();
  const auto rows = payload.value("lines").toArray();
  std::deque<Line> fresh;
  for (const auto &value : rows) {
    const auto row = value.toObject();
    Line line;
    line.item = addressValue(row.value("item")).value_or(0);
    line.address = addressValue(row.value("address")).value_or(line.item);
    line.sub = row.value("sub").toInt();
    line.cls = row.value("cls").toInt();
    line.kind = row.value("kind").toString();
    line.prefix = row.value("prefix").toString();
    line.flow = row.value("flow").toString();
    line.function = row.value("function").toString();
    line.target = addressValue(row.value("target"));
    line.functionAddress = addressValue(row.value("function_address"));
    line.styled.setFromWorker(row.value("text").toString(),
                              row.value("spans").toArray());
    fresh.push_back(std::move(line));
  }
  const int anchor =
      std::clamp(payload.value("anchor").toInt(), 0, int(fresh.size()));
  generation_ = payload.value("generation").toString();
  const auto identity = [](const Line &line) {
    return std::pair{line.item, line.sub};
  };
  switch (kind) {
  case Fetch::Jump:
  case Fetch::Refresh: {
    lines_ = std::move(fresh);
    atStart_ = payload.value("at_start").toBool();
    atEnd_ = payload.value("at_end").toBool();
    anchor_.reset();
    if (lines_.empty()) {
      top_ = cursorLine_ = 0;
      break;
    }
    if (kind == Fetch::Jump) {
      // Put the cursor on the item's instruction or data line, keeping its
      // header lines visible above it.
      int content = std::min(anchor, int(lines_.size()) - 1);
      const Address item = lines_[content].item;
      for (int i = content; i < int(lines_.size()) && lines_[i].item == item;
           ++i)
        if (contentKind(lines_[i].kind) &&
            (lines_[i].address <= jumpAddress || i == content)) {
          content = i;
          if (lines_[i].address >= jumpAddress)
            break;
        }
      cursorLine_ = content;
      cursorColumn_ = 0;
      top_ = std::max(0, std::min(anchor, cursorLine_ - visibleLines() / 3));
      if (cursorLine_ - top_ >= visibleLines() - 1)
        top_ = std::max(0, cursorLine_ - visibleLines() / 3);
    } else {
      top_ = std::min(anchor, int(lines_.size()) - 1);
      cursorLine_ = top_;
      if (cursor) {
        std::optional<int> bySub, byContent;
        for (int i = 0, content = 0; i < int(lines_.size()); ++i) {
          if (lines_[i].item != cursor->item)
            continue;
          if (lines_[i].sub == cursor->sub && !bySub)
            bySub = i;
          if (contentKind(lines_[i].kind) && content++ == cursor->content)
            byContent = i;
        }
        if (const auto line = byContent ? byContent : bySub) {
          cursorLine_ = *line;
          if (cursor->visible && cursorLine_ < top_)
            top_ = cursorLine_;
          else if (cursor->visible && cursorLine_ >= top_ + visibleLines())
            top_ = cursorLine_ - visibleLines() + 1;
        }
      }
    }
    break;
  }
  case Fetch::Before: {
    fetchingBefore_ = false;
    atStart_ = payload.value("at_start").toBool();
    if (lines_.empty())
      break;
    const auto first = identity(lines_.front());
    int count = anchor;
    // Keep only lines strictly before the loaded window.
    while (count > 0 && identity(fresh[count - 1]) >= first &&
           fresh[count - 1].item == first.first)
      --count;
    for (int i = count - 1; i >= 0; --i)
      lines_.push_front(std::move(fresh[i]));
    top_ += count;
    cursorLine_ += count;
    if (anchor_)
      anchor_->first += count;
    break;
  }
  case Fetch::After: {
    fetchingAfter_ = false;
    atEnd_ = payload.value("at_end").toBool();
    if (lines_.empty())
      break;
    const auto last = identity(lines_.back());
    for (int i = anchor; i < int(fresh.size()); ++i) {
      if (fresh[i].item == last.first && fresh[i].sub <= last.second)
        continue;
      lines_.push_back(std::move(fresh[i]));
    }
    break;
  }
  }
  trim();
  updateScrollBar();
  viewport()->update();
  // The next page goes out before the work of views that follow the
  // location: decompiling a long function must not hold the listing at the
  // end of its first page.
  ensureLoaded();
  emitLocation();
  if (settledJump)
    emit jumpSettled();
}

void ListingView::ensureLoaded() {
  if (lines_.empty() || !session_.loaded())
    return;
  if (!fetchingBefore_ && !atStart_ && top_ < PrefetchMargin) {
    const auto &first = lines_.front();
    request(Fetch::Before, first.item, first.sub, PageLines, 0);
  }
  const int bottom = top_ + visibleLines();
  if (!fetchingAfter_ && !atEnd_ &&
      bottom + PrefetchMargin > int(lines_.size())) {
    const auto &last = lines_.back();
    request(Fetch::After, last.item, last.sub + 1, 0, PageLines);
  }
}

void ListingView::trim() {
  while (int(lines_.size()) > MaxLoadedLines) {
    const int above = top_;
    const int below = int(lines_.size()) - (top_ + visibleLines());
    if (above > below) {
      lines_.pop_front();
      --top_;
      --cursorLine_;
      if (anchor_)
        --anchor_->first;
      atStart_ = false;
    } else {
      lines_.pop_back();
      atEnd_ = false;
    }
  }
  cursorLine_ = std::clamp(cursorLine_, 0, std::max(0, int(lines_.size()) - 1));
  top_ = std::clamp(top_, 0, std::max(0, int(lines_.size()) - 1));
}

//===----------------------------------------------------------------------===//
// Geometry
//===----------------------------------------------------------------------===//

void ListingView::updateMetrics() {
  const QFont font = Theme::instance().codeFont();
  viewport()->setFont(font);
  const QFontMetricsF metrics(font);
  charWidth_ = metrics.horizontalAdvance(QLatin1Char('M'));
  lineHeight_ = int(std::ceil(metrics.lineSpacing()));
  ascent_ = int(std::ceil(metrics.ascent()));
  arrowsWidth_ = int(std::ceil(charWidth_ * 7));
  prefixChars_ = 0;
  for (const auto &line : lines_)
    prefixChars_ = std::max(prefixChars_, int(line.prefix.size()));
  verticalScrollBar()->setSingleStep(1);
}

int ListingView::visibleLines() const {
  return std::max(1, viewport()->height() / std::max(1, lineHeight_));
}

int ListingView::textLeft() const {
  int prefixChars = showPrefixes_ ? prefixChars_ : 0;
  if (showPrefixes_ && !prefixChars) {
    // Before the first page arrives, reserve a typical 64-bit prefix.
    prefixChars = 22;
  }
  return arrowsWidth_ + TextMargin +
         int(std::ceil((prefixChars ? prefixChars + 1 : 0) * charWidth_)) -
         horizontalScrollBar()->value();
}

int ListingView::lineAt(int y) const {
  return top_ + std::max(0, y) / std::max(1, lineHeight_);
}

int ListingView::columnAt(const Line &line, int x) const {
  if (showPrefixes_ && x < textLeft()) {
    QTextLayout prefix(line.prefix + QLatin1Char(' '),
                       Theme::instance().codeFont());
    prefix.beginLayout();
    const auto row = prefix.createLine();
    prefix.endLayout();
    const int left = arrowsWidth_ + TextMargin - horizontalScrollBar()->value();
    return row.xToCursor(x - left) + firstColumn(line);
  }
  auto &layout = line.styled.layout(
      Theme::instance().codeFont(), styleStamp_,
      [](int role) { return Theme::instance().listingRole(role); });
  if (layout.lineCount() == 0)
    return 0;
  return layout.lineAt(0).xToCursor(x - textLeft());
}

int ListingView::firstColumn(const Line &line) const {
  return showPrefixes_ ? -int(line.prefix.size()) - 1 : 0;
}

qreal ListingView::columnX(const Line &line, int column) const {
  if (column < 0)
    return arrowsWidth_ + TextMargin - horizontalScrollBar()->value() +
           QFontMetricsF(Theme::instance().codeFont())
               .horizontalAdvance((line.prefix + QLatin1Char(' '))
                                      .left(column - firstColumn(line)));
  auto &layout = line.styled.layout(
      Theme::instance().codeFont(), styleStamp_,
      [](int role) { return Theme::instance().listingRole(role); });
  return textLeft() +
         (layout.lineCount() ? layout.lineAt(0).cursorToX(column) : 0);
}

void ListingView::resizeEvent(QResizeEvent *event) {
  QAbstractScrollArea::resizeEvent(event);
  ensureLoaded();
  updateScrollBar();
}

void ListingView::updateScrollBar() {
  updatingScrollBar_ = true;
  // Lines scroll sideways only as far as their bodies outrun the view; with
  // no file there is nothing to scroll.
  auto *across = horizontalScrollBar();
  const int content = space_.empty() ? 0
                                     : textLeft() + across->value() +
                                           int(charWidth_ * BodyChars);
  across->setRange(0, std::max(0, content - viewport()->width()));
  across->setPageStep(viewport()->width());
  auto *bar = verticalScrollBar();
  if (space_.empty() || lines_.empty()) {
    bar->setRange(0, 0);
  } else {
    bar->setRange(0, ScrollResolution);
    const auto &top =
        lines_[std::clamp<std::size_t>(top_, 0, lines_.size() - 1)];
    const long double fraction =
        static_cast<long double>(space_.linearOf(top.address)) /
        std::max<Address>(1, space_.total());
    const int bottomIndex =
        std::min(int(lines_.size()) - 1, top_ + visibleLines());
    const Address span = lines_[bottomIndex].address > top.address
                             ? lines_[bottomIndex].address - top.address
                             : 1;
    bar->setPageStep(
        std::clamp(int(static_cast<long double>(span) * ScrollResolution /
                       std::max<Address>(1, space_.total())),
                   1, ScrollResolution / 8));
    if (!bar->isSliderDown())
      bar->setValue(int(fraction * ScrollResolution));
  }
  updatingScrollBar_ = false;
}

void ListingView::scrollLines(int delta) {
  if (lines_.empty())
    return;
  top_ = std::clamp(top_ + delta, 0,
                    std::max(0, int(lines_.size()) - visibleLines()));
  ensureLoaded();
  updateScrollBar();
  viewport()->update();
}

//===----------------------------------------------------------------------===//
// Cursor and selection
//===----------------------------------------------------------------------===//

const ListingView::Line *ListingView::cursorLine() const {
  if (cursorLine_ < 0 || cursorLine_ >= int(lines_.size()))
    return nullptr;
  return &lines_[cursorLine_];
}

std::optional<Address> ListingView::currentAddress() const {
  if (const auto *line = cursorLine())
    return line->address;
  return std::nullopt;
}

std::optional<Address> ListingView::currentItem() const {
  if (const auto *line = cursorLine())
    return line->item;
  return std::nullopt;
}

std::optional<Address> ListingView::currentFunction() const {
  if (const auto *line = cursorLine())
    return line->functionAddress;
  return std::nullopt;
}

QString ListingView::currentFunctionName() const {
  if (const auto *line = cursorLine())
    return line->function;
  return {};
}

QString ListingView::currentToken() const {
  if (const auto *line = cursorLine())
    return line->styled.tokenAt(cursorColumn_);
  return {};
}

QString ListingView::currentLineText() const {
  if (const auto *line = cursorLine())
    return (showPrefixes_ ? line->prefix + QLatin1Char(' ') : QString()) +
           line->styled.text;
  return {};
}

std::optional<Address> ListingView::operandTarget() const {
  const auto *line = cursorLine();
  if (!line)
    return std::nullopt;
  if (const auto *span = line->styled.spanAt(cursorColumn_);
      span && span->address)
    return span->address;
  // A name just before the cursor (after a click at the end of a word).
  if (cursorColumn_ > 0)
    if (const auto *span = line->styled.spanAt(cursorColumn_ - 1);
        span && span->address)
      return span->address;
  return line->target;
}

std::optional<int> ListingView::currentOperand() const {
  const auto *line = cursorLine();
  if (!line)
    return std::nullopt;
  // The operands follow the mnemonic; a comma outside brackets starts the
  // next one.
  bool operands = false;
  int depth = 0, index = 0;
  std::optional<int> under;
  for (const auto &span : line->styled.spans) {
    const auto role = static_cast<ListingRole>(span.role);
    if (role == ListingRole::Mnemonic || role == ListingRole::FlowMnemonic) {
      operands = true;
      continue;
    }
    if (!operands)
      continue;
    if (role == ListingRole::Comment || role == ListingRole::AutoComment ||
        role == ListingRole::Xref)
      break;
    if (cursorColumn_ >= span.start && cursorColumn_ < span.start + span.length)
      under = index;
    if (role == ListingRole::Punctuation)
      for (const QChar c : line->styled.text.mid(span.start, span.length)) {
        if (c == QLatin1Char('['))
          ++depth;
        else if (c == QLatin1Char(']'))
          --depth;
        else if (c == QLatin1Char(',') && depth == 0)
          ++index;
      }
  }
  return under;
}

QString ListingView::selectedText() const {
  if (lines_.empty())
    return {};
  const auto cursor = std::pair{cursorLine_, cursorColumn_};
  if (!anchor_ || *anchor_ == cursor)
    return currentLineText();
  const auto first = std::min(*anchor_, cursor);
  const auto last = std::max(*anchor_, cursor);
  QStringList text;
  for (int i = std::max(0, first.first);
       i <= last.first && i < int(lines_.size()); ++i) {
    const auto &line = lines_[i];
    const QString row =
        (showPrefixes_ ? lines_[i].prefix + QLatin1Char(' ') : QString()) +
        lines_[i].styled.text;
    const int begin =
        i == first.first
            ? std::clamp(first.second - firstColumn(line), 0, int(row.size()))
            : 0;
    const int end =
        i == last.first
            ? std::clamp(last.second - firstColumn(line), 0, int(row.size()))
            : int(row.size());
    text.append(row.mid(begin, end - begin));
  }
  return text.join(QLatin1Char('\n'));
}

void ListingView::moveCursor(int line, int column, bool extend) {
  if (lines_.empty())
    return;
  if (extend && !anchor_)
    anchor_ = std::pair{cursorLine_, cursorColumn_};
  else if (!extend)
    anchor_.reset();
  cursorLine_ = std::clamp(line, 0, int(lines_.size()) - 1);
  cursorColumn_ = std::clamp(column, firstColumn(lines_[cursorLine_]),
                             int(lines_[cursorLine_].styled.text.size()));
  ensureCursorVisible();
  emitLocation();
  viewport()->update();
}

void ListingView::ensureCursorVisible() {
  if (cursorLine_ < top_)
    top_ = cursorLine_;
  else if (cursorLine_ >= top_ + visibleLines())
    top_ = cursorLine_ - visibleLines() + 1;
  ensureLoaded();
  updateScrollBar();
}

void ListingView::emitLocation() {
  const auto address = currentAddress();
  const auto function = currentFunction();
  if (!address || (address == lastEmitted_ && function == lastFunction_))
    return;
  lastEmitted_ = address;
  lastFunction_ = function;
  emit locationChanged(*address);
}

void ListingView::setShowPrefixes(bool show) {
  if (show == showPrefixes_)
    return;
  showPrefixes_ = show;
  anchor_.reset();
  cursorColumn_ = std::max(0, cursorColumn_);
  viewport()->update();
}

void ListingView::setOpcodeBytes(int count) {
  if (count == opcodeBytes_)
    return;
  opcodeBytes_ = count;
  refresh();
}

bool ListingView::findInLoaded(const QString &text, bool forward) {
  if (text.isEmpty() || lines_.empty())
    return false;
  const int count = int(lines_.size());
  for (int step = 1; step <= count; ++step) {
    const int index = forward ? cursorLine_ + step : cursorLine_ - step;
    if (index < 0 || index >= count)
      break;
    const int column =
        lines_[index].styled.text.indexOf(text, 0, Qt::CaseInsensitive);
    if (column >= 0) {
      moveCursor(index, column);
      highlight_ = text;
      return true;
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Painting
//===----------------------------------------------------------------------===//

void ListingView::paintEvent(QPaintEvent *) {
  QPainter painter(viewport());
  const auto &theme = Theme::instance();
  const QRect area = viewport()->rect();
  painter.fillRect(area, theme.color(ColorRole::ListingBackground));
  painter.fillRect(QRect(0, 0, arrowsWidth_, area.height()),
                   theme.color(ColorRole::ArrowsBackground));
  if (lines_.empty())
    return;
  const QFont font = theme.codeFont();
  painter.setFont(font);
  int prefixChars = 0;
  for (int i = top_;
       i < std::min<int>(lines_.size(), top_ + visibleLines() + 1); ++i)
    prefixChars = std::max(prefixChars, int(lines_[i].prefix.size()));
  if (prefixChars != prefixChars_ && prefixChars) {
    prefixChars_ = prefixChars;
  }
  const int left = textLeft();
  const int prefixLeft =
      arrowsWidth_ + TextMargin - horizontalScrollBar()->value();
  const auto colorOf = [&theme](int role) { return theme.listingRole(role); };
  const int first = top_;
  const int last = std::min<int>(lines_.size() - 1, top_ + visibleLines());
  const auto cursor = std::pair{cursorLine_, cursorColumn_};
  const auto selectionFirst = anchor_ ? std::min(*anchor_, cursor) : cursor;
  const auto selectionLast = anchor_ ? std::max(*anchor_, cursor) : cursor;
  painter.setClipRect(
      QRect(arrowsWidth_, 0, area.width() - arrowsWidth_, area.height()));
  for (int i = first; i <= last; ++i) {
    const auto &line = lines_[i];
    const int y = (i - top_) * lineHeight_;
    const QRect row(arrowsWidth_, y, area.width() - arrowsWidth_, lineHeight_);
    if (i == cursorLine_)
      painter.fillRect(row, theme.color(ColorRole::ListingCurrentLine));
    const bool selected = anchor_ && selectionFirst != selectionLast &&
                          i >= selectionFirst.first && i <= selectionLast.first;
    const int begin = selected && i == selectionFirst.first
                          ? selectionFirst.second
                          : firstColumn(line);
    const int end = selected && i == selectionLast.first
                        ? selectionLast.second
                        : int(line.styled.text.size());
    if (selected && begin < 0) {
      const qreal x0 = columnX(line, std::max(begin, firstColumn(line)));
      const qreal x1 = columnX(line, std::min(end, 0));
      painter.fillRect(QRectF(x0, y, x1 - x0, lineHeight_),
                       theme.color(ColorRole::ListingSelection));
    }
    if (showPrefixes_) {
      painter.setPen(theme.prefixColor(line.cls));
      painter.drawText(QPointF(prefixLeft, y + ascent_), line.prefix);
    }
    auto &layout = line.styled.layout(font, styleStamp_, colorOf);
    if (!highlight_.isEmpty() && layout.lineCount()) {
      const QTextLine textLine = layout.lineAt(0);
      for (const int at : tokenOccurrences(line.styled.text, highlight_)) {
        const qreal x0 = textLine.cursorToX(at);
        const qreal x1 = textLine.cursorToX(at + int(highlight_.size()));
        painter.fillRect(QRectF(left + x0, y, x1 - x0, lineHeight_),
                         theme.color(ColorRole::ListingHighlight));
      }
    }
    QList<QTextLayout::FormatRange> selection;
    if (selected && end > 0) {
      QTextLayout::FormatRange range;
      range.start = std::max(0, begin);
      range.length = std::max(0, end - range.start);
      range.format.setBackground(theme.color(ColorRole::ListingSelection));
      selection.append(range);
    }
    layout.draw(&painter, QPointF(left, y), selection);
    if (i == cursorLine_ && hasFocus() && layout.lineCount()) {
      const qreal x = columnX(line, cursorColumn_);
      painter.fillRect(QRectF(x, y + 1, 2, lineHeight_ - 2),
                       theme.color(ColorRole::ListingCursor));
    }
  }
  painter.setClipping(false);
  paintArrows(painter, first, last);
  if (!contentReported_) {
    contentReported_ = true;
    emit contentPainted();
  }
}

void ListingView::paintArrows(QPainter &painter, int first, int last) {
  const auto &theme = Theme::instance();
  struct Arrow {
    int from, to;
    bool conditional, current;
    int level = 0;
  };
  // First content line of each loaded item that a branch can land on.
  std::unordered_map<Address, int> landing;
  for (int i = 0; i < int(lines_.size()); ++i) {
    const auto &line = lines_[i];
    if ((line.kind == QLatin1String("label") ||
         line.kind == QLatin1String("insn") ||
         line.kind == QLatin1String("header")) &&
        !landing.count(line.item))
      landing.emplace(line.item, i);
  }
  QVector<Arrow> arrows;
  const int count = int(lines_.size());
  for (int i = 0; i < count; ++i) {
    const auto &line = lines_[i];
    if (!line.target || (line.flow != QLatin1String("jump") &&
                         line.flow != QLatin1String("cjump")))
      continue;
    int to;
    if (auto it = landing.find(*line.target); it != landing.end())
      to = it->second;
    else
      to = *line.target < line.item ? -1 : count;
    const int low = std::min(i, to), high = std::max(i, to);
    if (high < first || low > last)
      continue;
    arrows.push_back({i, to, line.flow == QLatin1String("cjump"),
                      i == cursorLine_ || to == cursorLine_});
  }
  std::sort(arrows.begin(), arrows.end(), [](const Arrow &a, const Arrow &b) {
    return std::abs(a.to - a.from) < std::abs(b.to - b.from);
  });
  QVector<QVector<std::pair<int, int>>> occupied(MaxArrowLevels);
  for (auto &arrow : arrows) {
    const int low = std::min(arrow.from, arrow.to);
    const int high = std::max(arrow.from, arrow.to);
    arrow.level = MaxArrowLevels - 1;
    for (int level = 0; level < MaxArrowLevels; ++level) {
      bool free = true;
      for (const auto &[a, b] : occupied[level])
        if (!(high < a || low > b)) {
          free = false;
          break;
        }
      if (free) {
        arrow.level = level;
        break;
      }
    }
    occupied[arrow.level].append({low, high});
  }
  painter.setRenderHint(QPainter::Antialiasing, false);
  const qreal spacing =
      std::max<qreal>(3, (arrowsWidth_ - 8) / qreal(MaxArrowLevels));
  const auto yOf = [&](int index) -> qreal {
    if (index < 0)
      return 0;
    if (index >= count)
      return viewport()->height();
    return (index - top_) * lineHeight_ + lineHeight_ / 2.0;
  };
  // Current arrows draw last, on top.
  std::stable_sort(
      arrows.begin(), arrows.end(),
      [](const Arrow &a, const Arrow &b) { return !a.current && b.current; });
  for (const auto &arrow : arrows) {
    const qreal x = arrowsWidth_ - 6 - arrow.level * spacing;
    const qreal y0 = std::clamp<qreal>(yOf(arrow.from), -lineHeight_,
                                       viewport()->height() + lineHeight_);
    const qreal y1 = std::clamp<qreal>(yOf(arrow.to), -lineHeight_,
                                       viewport()->height() + lineHeight_);
    QPen pen(arrow.current       ? theme.color(ColorRole::ArrowCurrent)
             : arrow.conditional ? theme.color(ColorRole::ArrowConditional)
                                 : theme.color(ColorRole::ArrowJump));
    pen.setWidthF(arrow.current ? 2.0 : 1.0);
    if (arrow.conditional)
      pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    const qreal right = arrowsWidth_ - 1;
    QPainterPath path;
    path.moveTo(right, y0);
    path.lineTo(x, y0);
    path.lineTo(x, y1);
    if (arrow.to >= 0 && arrow.to < count)
      path.lineTo(right - 4, y1);
    painter.drawPath(path);
    if (arrow.to >= 0 && arrow.to < count) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(pen.color());
      const QPointF head[] = {
          {right, y1}, {right - 5, y1 - 3.5}, {right - 5, y1 + 3.5}};
      painter.drawPolygon(head, 3);
      painter.setBrush(Qt::NoBrush);
    }
  }
}

//===----------------------------------------------------------------------===//
// Input
//===----------------------------------------------------------------------===//

void ListingView::keyPressEvent(QKeyEvent *event) {
  const bool shift = event->modifiers() & Qt::ShiftModifier;
  const bool control = event->modifiers() & Qt::ControlModifier;
  switch (event->key()) {
  case Qt::Key_Up:
    moveCursor(cursorLine_ - 1, cursorColumn_, shift);
    return;
  case Qt::Key_Down:
    moveCursor(cursorLine_ + 1, cursorColumn_, shift);
    return;
  case Qt::Key_PageUp:
    top_ = std::max(0, top_ - visibleLines() + 1);
    moveCursor(cursorLine_ - visibleLines() + 1, cursorColumn_, shift);
    return;
  case Qt::Key_PageDown:
    top_ = std::min(std::max(0, int(lines_.size()) - visibleLines()),
                    top_ + visibleLines() - 1);
    moveCursor(cursorLine_ + visibleLines() - 1, cursorColumn_, shift);
    return;
  case Qt::Key_Left:
    moveCursor(cursorLine_, cursorColumn_ - 1, shift);
    return;
  case Qt::Key_Right:
    moveCursor(cursorLine_, cursorColumn_ + 1, shift);
    return;
  case Qt::Key_Home:
    if (control)
      jumpTo(space_.first());
    else
      moveCursor(cursorLine_, 0, shift);
    return;
  case Qt::Key_End:
    if (control)
      jumpTo(space_.last());
    else if (const auto *line = cursorLine())
      moveCursor(cursorLine_, int(line->styled.text.size()), shift);
    return;
  default:
    break;
  }
  if (event->matches(QKeySequence::Copy)) {
    QApplication::clipboard()->setText(selectedText());
    return;
  }
  QAbstractScrollArea::keyPressEvent(event);
}

void ListingView::mousePressEvent(QMouseEvent *event) {
  setFocus(Qt::MouseFocusReason);
  if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton)
    return;
  const int index = lineAt(int(event->position().y()));
  if (index >= int(lines_.size()))
    return;
  const int column = columnAt(lines_[index], int(event->position().x()));
  const bool extend = event->modifiers() & Qt::ShiftModifier;
  if (event->button() == Qt::LeftButton) {
    moveCursor(index, column, extend);
    selecting_ = true;
    highlight_ = lines_[index].styled.tokenAt(column);
  } else if (index != cursorLine_ || !anchor_) {
    moveCursor(index, column);
  }
  viewport()->update();
}

void ListingView::mouseMoveEvent(QMouseEvent *event) {
  if (!selecting_ || !(event->buttons() & Qt::LeftButton)) {
    // Hovering over a name shows the pointer hand like a hyperlink.
    const int index = lineAt(int(event->position().y()));
    bool link = false;
    if (index < int(lines_.size())) {
      const int column = columnAt(lines_[index], int(event->position().x()));
      const auto *span = lines_[index].styled.spanAt(column);
      link =
          span && span->address && (event->modifiers() & Qt::ControlModifier);
    }
    viewport()->setCursor(link ? Qt::PointingHandCursor : Qt::IBeamCursor);
    return;
  }
  const int index = std::clamp(lineAt(int(event->position().y())), 0,
                               std::max(0, int(lines_.size()) - 1));
  if (lines_.empty())
    return;
  const int column = columnAt(lines_[index], int(event->position().x()));
  if (!anchor_)
    anchor_ = std::pair{cursorLine_, cursorColumn_};
  cursorLine_ = index;
  cursorColumn_ = column;
  ensureCursorVisible();
  viewport()->update();
}

void ListingView::mouseReleaseEvent(QMouseEvent *event) {
  selecting_ = false;
  if (anchor_ && *anchor_ == std::pair{cursorLine_, cursorColumn_})
    anchor_.reset();
  if ((event->modifiers() & Qt::ControlModifier) &&
      event->button() == Qt::LeftButton)
    if (const auto target = operandTarget())
      emit navigateRequested(*target);
  emitLocation();
}

void ListingView::mouseDoubleClickEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton)
    return;
  if (const auto target = operandTarget())
    emit navigateRequested(*target);
}

void ListingView::wheelEvent(QWheelEvent *event) {
  if (event->modifiers() & Qt::ControlModifier) {
    Theme::instance().zoomCodeFont(event->angleDelta().y() > 0 ? 1 : -1);
    return;
  }
  const int steps = event->angleDelta().y() / 120;
  if (steps)
    scrollLines(-steps * WheelLines);
  else if (const int pixels = event->pixelDelta().y())
    scrollLines(-pixels / std::max(1, lineHeight_));
  if (event->angleDelta().x())
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() -
                                    event->angleDelta().x());
}

void ListingView::contextMenuEvent(QContextMenuEvent *event) {
  emit contextMenuRequested(event->globalPos());
}

bool ListingView::viewportEvent(QEvent *event) {
  if (event->type() == QEvent::ToolTip) {
    showHint(static_cast<QHelpEvent *>(event)->pos());
    return true;
  }
  return QAbstractScrollArea::viewportEvent(event);
}

void ListingView::focusInEvent(QFocusEvent *event) {
  QAbstractScrollArea::focusInEvent(event);
  viewport()->update();
}

void ListingView::focusOutEvent(QFocusEvent *event) {
  QAbstractScrollArea::focusOutEvent(event);
  viewport()->update();
}

void ListingView::showHint(const QPoint &position) {
  const int index = lineAt(position.y());
  if (index >= int(lines_.size())) {
    QToolTip::hideText();
    return;
  }
  const int column = columnAt(lines_[index], position.x());
  const auto *span = lines_[index].styled.spanAt(column);
  if (!span || !span->address) {
    QToolTip::hideText();
    return;
  }
  const QPoint global = viewport()->mapToGlobal(position);
  const Address target = *span->address;
  session_.read(
      QStringLiteral("listing"),
      {{"address", hexAddress(target)}, {"before", 0}, {"after", HintLines}},
      this,
      [this, global](const QJsonObject &payload) {
        QStringList rows;
        for (const auto &value : payload.value("lines").toArray()) {
          const auto row = value.toObject();
          const auto kind = row.value("kind").toString();
          if (kind == QLatin1String("blank") && rows.isEmpty())
            continue;
          rows.append(row.value("text").toString().toHtmlEscaped());
        }
        if (rows.isEmpty())
          return;
        const QString html =
            QStringLiteral("<pre style=\"font-family:'%1'; margin:0\">%2</pre>")
                .arg(Theme::instance().codeFont().family(),
                     rows.join(QLatin1Char('\n')));
        QToolTip::showText(global, html, viewport());
      },
      [](const QString &, const QString &) {});
}

} // namespace neverd::gui
