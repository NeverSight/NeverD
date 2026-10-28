//===- ListingView.cpp - Interactive disassembly listing ------------------===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Fetch, navigate and paint the workbench's disassembly rows.
//===----------------------------------------------------------------------===//

#include "ListingView.h"

#include "../Config.h"
#include "../ResourceLimits.h"
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
#include <iterator>
#include <limits>
#include <unordered_map>

namespace neverd::gui {
namespace {
constexpr int JumpContextBefore = 48;
constexpr int ScrollResolution = 1 << 20;
constexpr int HintLines = 12;
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
  connect(&session_.highlightLocks(), &HighlightLocks::changed, viewport(),
          qOverload<>(&QWidget::update));
  connect(&session_, &Session::bytePatchesChanged, viewport(),
          qOverload<>(&QWidget::update));
  setFrameShape(QFrame::NoFrame);
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  viewport()->setCursor(Qt::IBeamCursor);
  viewport()->setMouseTracking(true);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  scrollJump_.setSingleShot(true);
  scrollJump_.setInterval(::neverd::limits::gui::kListingViewScrollDebounceMs);
  connect(&scrollJump_, &QTimer::timeout, this, [this] {
    request(Fetch::Jump, pendingScrollAddress_, 0,
            ::neverd::limits::gui::kListingViewPageLines / 2,
            ::neverd::limits::gui::kListingViewPageLines);
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
  connect(&session_, &Session::generationChanged, this, [this] {
    // A reply can publish the generation of the rows it just delivered.
    if (generation_ != session_.generation())
      refresh();
  });
  connect(&session_, &Session::opened, this, &ListingView::clear);
  connect(&session_, &Session::unloaded, this, &ListingView::clear);
  connect(&session_, &Session::revisionChanged, this, [this] {
    // Local pseudocode names do not change assembly. Adopt the new revision
    // only for settled rows; a pending semantic refresh must still complete.
    if (session_.queries().localNamesOnly() && !pendingJump_ &&
        contentSerial_ == serial_ && generation_ == session_.generation())
      revision_ = session_.revision();
    else
      refresh();
  });
  updateMetrics();
}

//===----------------------------------------------------------------------===//
// Data
//===----------------------------------------------------------------------===//

void ListingView::clear() {
  // Advance before cancellation: a queued completion must not repopulate
  // the closed image, even before the worker finishes stopping.
  ++serial_;
  scrollJump_.stop();
  session_.queries().unsubscribeOwner(this);
  lines_.clear();
  pendingJump_.reset();
  pendingScrollAddress_ = 0;
  fetchingBefore_ = fetchingAfter_ = false;
  atStart_ = atEnd_ = false;
  top_ = cursorLine_ = cursorColumn_ = 0;
  anchor_.reset();
  selecting_ = false;
  highlight_.clear();
  generation_.clear();
  revision_.clear();
  lastEmitted_.reset();
  lastFunction_.reset();
  contentReported_ = false;
  // The arrow gutter is a font metric; only the prefix width follows the rows.
  prefixChars_ = 0;
  updatingScrollBar_ = true;
  horizontalScrollBar()->setRange(0, 0);
  verticalScrollBar()->setRange(0, 0);
  updatingScrollBar_ = false;
  QToolTip::hideText();
  viewport()->setCursor(Qt::IBeamCursor);
  viewport()->update();
  emit searchDocumentChanged();
}

void ListingView::jumpTo(Address address) {
  scrollJump_.stop();
  pendingJump_ = address;
  // A new view needs its viewport before adjacent functions. Ordinary
  // prefetch fills both margins after these current rows have arrived.
  const bool firstPage = lines_.empty();
  const int pageLines = ::neverd::limits::gui::kListingViewPageLines;
  request(Fetch::Jump, address, 0, firstPage ? 0 : JumpContextBefore,
          firstPage ? std::min(visibleLines(), pageLines) : pageLines);
}

bool ListingView::rowsCurrent() const {
  return session_.loaded() && !pendingJump_ && contentSerial_ == serial_ &&
         revision_ == session_.revision() &&
         generation_ == session_.generation();
}

std::optional<Address> ListingView::loadedFunctionAt(Address address) const {
  if (!rowsCurrent())
    return std::nullopt;
  for (const auto &line : lines_)
    if (line.item == address && line.functionAddress)
      return line.functionAddress;
  return std::nullopt;
}

bool ListingView::revealItem(Address address) {
  if (!rowsCurrent())
    return false;
  const auto item =
      std::find_if(lines_.cbegin(), lines_.cend(), [address](const Line &line) {
        return line.item == address;
      });
  if (item == lines_.cend())
    return false;
  const int anchor = int(std::distance(lines_.cbegin(), item));
  // A page may start partway through a large item's rows. It cannot supply
  // that item's ordinary jump position without consulting the worker.
  if (lines_[anchor].sub != 0)
    return false;
  scrollJump_.stop();
  const bool selected = anchor_.has_value();
  const int oldTop = top_, oldLine = cursorLine_, oldColumn = cursorColumn_;
  anchor_.reset();
  placeJump(anchor, address);
  updateScrollBar();
  if (selected || oldTop != top_ || oldLine != cursorLine_ ||
      oldColumn != cursorColumn_)
    viewport()->update();
  ensureLoaded();
  emitLocation();
  emit jumpSettled();
  return true;
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
    request(Fetch::Jump, *pendingJump_, 0, JumpContextBefore,
            ::neverd::limits::gui::kListingViewPageLines);
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
  // Keep the loaded context instead of shrinking to a jump-sized window and
  // immediately prefetching it again. Both paints would route the same visible
  // branches through different gutter columns. A bounded refresh can discard
  // distant cached rows, but must leave a prefetch margin around the viewport.
  const int margin = ::neverd::limits::gui_config::kListingViewPrefetchMargin;
  const int limit = ::neverd::limits::gui::kListingViewRefreshContextLines;
  const int before = std::clamp(top_, margin, limit);
  const int after = std::clamp(int(lines_.size()) - top_,
                               std::min(visibleLines() + margin, limit), limit);
  request(Fetch::Refresh, top.item, top.sub, before, after, cursor);
}

void ListingView::request(Fetch kind, Address address, int sub, int before,
                          int after, std::optional<CursorPlace> cursor) {
  if (!session_.loaded())
    return;
  if (kind == Fetch::Jump || kind == Fetch::Refresh) {
    ++serial_;
    fetchingBefore_ = fetchingAfter_ = false;
    // The old rows are no longer a searchable document while this request
    // is pending. Retire matches at the same boundary as their row serial.
    emit searchDocumentChanged();
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
  if (kind == Fetch::Jump && lines_.empty())
    payload["complete_anchor_item"] = true;
  if (kind == Fetch::Jump || kind == Fetch::Refresh)
    payload["recover_address"] = hexAddress(cursor ? cursor->item : address);
  if (opcodeBytes_ > 0)
    payload["opcode_bytes"] = opcodeBytes_;
  session_.read(
      QStringLiteral("listing"), payload, this,
      [this, kind, serial, cursor, address](const QJsonObject &result) {
        accept(kind, result, serial, cursor, address);
      },
      [this, kind, serial](const QString &, const QString &message) {
        if (serial != serial_ || !session_.loaded())
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
      },
      kind == Fetch::Jump || kind == Fetch::Refresh
          ? QueryService::QuerySpec::Visible
          : QueryService::QuerySpec::Normal);
}

void ListingView::accept(Fetch kind, const QJsonObject &payload, quint64 serial,
                         std::optional<CursorPlace> cursor,
                         Address jumpAddress) {
  if (serial != serial_ || !session_.loaded())
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
    line.itemSize = addressValue(row.value("item_size")).value_or(0);
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
  const auto generation = payload.value("generation").toString();
  if (generation != generation_ || revision_ != session_.revision())
    contentSerial_ = 0;
  const auto identity = [](const Line &line) {
    return std::pair{line.item, line.sub};
  };
  switch (kind) {
  case Fetch::Jump:
  case Fetch::Refresh: {
    generation_ = generation;
    revision_ = session_.revision();
    contentSerial_ = serial;
    lines_ = std::move(fresh);
    atStart_ = payload.value("at_start").toBool();
    atEnd_ = payload.value("at_end").toBool();
    anchor_.reset();
    if (lines_.empty()) {
      top_ = cursorLine_ = 0;
      break;
    }
    if (kind == Fetch::Jump) {
      placeJump(anchor, jumpAddress);
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
  emit searchDocumentChanged();
  if (settledJump)
    emit jumpSettled();
}

void ListingView::placeJump(int anchor, Address address) {
  // Put the cursor on the item's instruction or data line, keeping its
  // header lines visible above it. Cached and fetched jumps share this rule.
  int content = std::min(anchor, int(lines_.size()) - 1);
  const Address item = lines_[content].item;
  for (int i = content; i < int(lines_.size()) && lines_[i].item == item; ++i)
    if (contentKind(lines_[i].kind) &&
        (lines_[i].address <= address || i == content)) {
      content = i;
      if (lines_[i].address >= address)
        break;
    }
  cursorLine_ = content;
  cursorColumn_ = 0;
  top_ = std::max(0, std::min(anchor, cursorLine_ - visibleLines() / 3));
  if (cursorLine_ - top_ >= visibleLines() - 1)
    top_ = std::max(0, cursorLine_ - visibleLines() / 3);
}

void ListingView::ensureLoaded() {
  if (lines_.empty() || !session_.loaded())
    return;
  if (!fetchingBefore_ && !atStart_ &&
      top_ < ::neverd::limits::gui_config::kListingViewPrefetchMargin) {
    const auto &first = lines_.front();
    request(Fetch::Before, first.item, first.sub,
            ::neverd::limits::gui::kListingViewPageLines, 0);
  }
  const int bottom = top_ + visibleLines();
  if (!fetchingAfter_ && !atEnd_ &&
      bottom + ::neverd::limits::gui_config::kListingViewPrefetchMargin >
          int(lines_.size())) {
    const auto &last = lines_.back();
    request(Fetch::After, last.item, last.sub + 1, 0,
            ::neverd::limits::gui::kListingViewPageLines);
  }
}

void ListingView::trim() {
  while (int(lines_.size()) >
         ::neverd::limits::gui::kListingViewMaxLoadedLines) {
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
  return arrowsWidth_ + ::neverd::limits::gui_config::kListingViewTextMargin +
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
    const int left = arrowsWidth_ +
                     ::neverd::limits::gui_config::kListingViewTextMargin -
                     horizontalScrollBar()->value();
    return row.xToCursor(x - left) + firstColumn(line);
  }
  auto &layout = line.styled.layout(
      Theme::instance().codeFont(), styleStamp_,
      [](int role) { return Theme::instance().listingRole(role); });
  if (layout.lineCount() == 0)
    return 0;
  return layout.lineAt(0).xToCursor(x - textLeft());
}

std::optional<std::pair<int, int>>
ListingView::prefixWordAt(const Line &line, int x) const {
  const int column = columnAt(line, x);
  if (column >= 0 || x < arrowsWidth_)
    return std::nullopt;
  const int base = firstColumn(line);
  const auto [begin, end] = tokenRange(line.prefix, column - base);
  // The arrow gutter, the colon and the gap before the body hold no word.
  if (begin == end || x < columnX(line, base + begin) ||
      x >= columnX(line, base + end))
    return std::nullopt;
  return std::pair{base + begin, base + end};
}

bool ListingView::selectPrefixWord(int x) {
  if (cursorLine_ < 0 || cursorLine_ >= int(lines_.size()))
    return false;
  const auto word = prefixWordAt(lines_[cursorLine_], x);
  if (!word)
    return false;
  anchor_ = std::pair{cursorLine_, word->first};
  cursorColumn_ = word->second;
  viewport()->update();
  return true;
}

int ListingView::firstColumn(const Line &line) const {
  return showPrefixes_ ? -int(line.prefix.size()) - 1 : 0;
}

qreal ListingView::columnX(const Line &line, int column) const {
  if (column < 0)
    return arrowsWidth_ + ::neverd::limits::gui_config::kListingViewTextMargin -
           horizontalScrollBar()->value() +
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
  const int content =
      space_.empty() || lines_.empty()
          ? 0
          : textLeft() + across->value() + int(charWidth_ * BodyChars);
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
  // The operands follow the mnemonic; a comma outside brackets, braces and
  // parentheses starts the next one, as the worker counts them (`{r4, lr}`).
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
        if (c == QLatin1Char('[') || c == QLatin1Char('{') ||
            c == QLatin1Char('('))
          ++depth;
        else if ((c == QLatin1Char(']') || c == QLatin1Char('}') ||
                  c == QLatin1Char(')')) &&
                 depth > 0)
          --depth;
        else if (c == QLatin1Char(',') && depth == 0)
          ++index;
      }
  }
  return under;
}

std::optional<std::pair<Address, Address>> ListingView::byteSelection() const {
  if (!rowsCurrent() || lines_.empty())
    return std::nullopt;
  const auto cursor = std::pair{cursorLine_, cursorColumn_};
  const auto first = anchor_ ? std::min(*anchor_, cursor) : cursor;
  const auto last = anchor_ ? std::max(*anchor_, cursor) : cursor;
  Address begin = std::numeric_limits<Address>::max(), end = 0;
  for (int i = first.first; i <= last.first; ++i) {
    // A selection ending at the next line's start excludes that line.
    if (i != first.first && i == last.first &&
        last.second == firstColumn(lines_[i]))
      break;
    const auto &line = lines_[i];
    if (!line.itemSize ||
        line.itemSize - 1 > std::numeric_limits<Address>::max() - line.item)
      return std::nullopt;
    begin = std::min(begin, line.item);
    end = std::max(end, line.item + line.itemSize - 1);
  }
  return std::pair{begin, end};
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

QString ListingView::copyText() const {
  if (anchor_ && *anchor_ != std::pair{cursorLine_, cursorColumn_})
    return selectedText();
  // A prefix word copies once a click selects it; the body's highlight
  // marks the word.
  return currentToken();
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
  emit cursorChanged();
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
  emit searchDocumentChanged();
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

QString ListingView::searchText() const {
  QString text;
  bool first = true;
  for (const auto &line : lines_) {
    if (!first)
      text += u'\n';
    first = false;
    if (showPrefixes_)
      text += line.prefix + u' ';
    text += line.styled.text;
  }
  return text;
}

std::pair<int, int> ListingView::searchSelection() const {
  const auto position = [this](int row, int column) {
    int at = 0;
    for (int i = 0; i < row && i < int(lines_.size()); ++i)
      at += lines_[i].styled.text.size() - firstColumn(lines_[i]) + 1;
    return row >= 0 && row < int(lines_.size())
               ? at + column - firstColumn(lines_[row])
               : at;
  };
  const int cursor = position(cursorLine_, cursorColumn_);
  const int anchor =
      anchor_ ? position(anchor_->first, anchor_->second) : cursor;
  return {std::min(cursor, anchor), std::max(cursor, anchor)};
}

QString ListingView::searchSeed() const {
  const auto cursor = std::pair{cursorLine_, cursorColumn_};
  const QString selected =
      anchor_ && *anchor_ != cursor ? selectedText() : QString();
  return !selected.isEmpty() && !selected.contains(u'\n') ? selected
                                                          : currentToken();
}

void ListingView::setSearchHighlight(const ViewSearchQuery &query) {
  SearchHighlight = query;
  viewport()->update();
}

bool ListingView::selectSearchRange(int begin, int length) {
  if (!rowsCurrent() || lines_.empty() || begin < 0 || length <= 0)
    return false;
  const auto place = [this](int at) -> std::optional<std::pair<int, int>> {
    for (int i = 0; i < int(lines_.size()); ++i) {
      const int size = lines_[i].styled.text.size() - firstColumn(lines_[i]);
      if (at <= size)
        return std::pair{i, at + firstColumn(lines_[i])};
      at -= size + 1;
    }
    return std::nullopt;
  };
  const auto first = place(begin);
  const auto last = place(begin + length);
  if (!first || !last)
    return false;
  anchor_ = *first;
  cursorLine_ = last->first;
  cursorColumn_ = last->second;
  ensureCursorVisible();
  const qreal left = columnX(lines_[first->first], first->second);
  const qreal right = columnX(lines_[last->first], last->second);
  const qreal gutter =
      arrowsWidth_ + ::neverd::limits::gui_config::kListingViewTextMargin;
  if (left < gutter || right - left > viewport()->width() - gutter)
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() +
                                    int(left - gutter));
  else if (right > viewport()->width())
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() +
                                    int(right - viewport()->width()));
  emitLocation();
  viewport()->update();
  return true;
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
  const int prefixLeft = arrowsWidth_ +
                         ::neverd::limits::gui_config::kListingViewTextMargin -
                         horizontalScrollBar()->value();
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
    const bool patched =
        contentKind(line.kind) &&
        session_.hasBytePatch(line.item, std::max<Address>(1, line.itemSize));
    if (i == cursorLine_)
      painter.fillRect(row, theme.color(ColorRole::ListingCurrentLine));
    // The active row must reveal a committed patch without moving the cursor.
    if (patched) {
      painter.fillRect(row, theme.color(ColorRole::ListingPatched));
      painter.setPen(theme.color(ColorRole::ListingPlain));
      painter.drawText(QPointF(arrowsWidth_, y + ascent_), QStringLiteral("*"));
    }
    if (!SearchHighlight.Text.isEmpty()) {
      const QString text =
          (showPrefixes_ ? line.prefix + u' ' : QString()) + line.styled.text;
      for (const int at : SearchHighlight.matches(text)) {
        const qreal x0 = columnX(line, at + firstColumn(line));
        const qreal x1 =
            columnX(line, at + SearchHighlight.Text.size() + firstColumn(line));
        painter.fillRect(QRectF(x0, y, x1 - x0, lineHeight_),
                         theme.color(ColorRole::ListingHighlight));
      }
    }
    const bool selected = anchor_ && selectionFirst != selectionLast &&
                          i >= selectionFirst.first && i <= selectionLast.first;
    const int begin = selected && i == selectionFirst.first
                          ? selectionFirst.second
                          : firstColumn(line);
    const int end = selected && i == selectionLast.first
                        ? selectionLast.second
                        : int(line.styled.text.size());
    const auto selectionColor =
        theme.color(patched ? ColorRole::ListingPatchedSelection
                            : ColorRole::ListingSelection);
    if (selected && begin < 0) {
      const qreal x0 = columnX(line, std::max(begin, firstColumn(line)));
      const qreal x1 = columnX(line, std::min(end, 0));
      painter.fillRect(QRectF(x0, y, x1 - x0, lineHeight_), selectionColor);
    }
    if (showPrefixes_) {
      painter.setPen(theme.prefixColor(line.cls));
      painter.drawText(QPointF(prefixLeft, y + ascent_), line.prefix);
    }
    auto &layout = line.styled.layout(font, styleStamp_, colorOf);
    if (SearchHighlight.Text.isEmpty() && layout.lineCount()) {
      const QTextLine textLine = layout.lineAt(0);
      // Locked words keep their colors under the cursor's highlight.
      const auto mark = [&](const QString &word, ColorRole role) {
        for (const auto &[first, last] :
             highlightRanges(line.styled, word,
                             static_cast<int>(ListingRole::Register))) {
          const qreal x0 = textLine.cursorToX(first);
          const qreal x1 = textLine.cursorToX(last);
          painter.fillRect(QRectF(left + x0, y, x1 - x0, lineHeight_),
                           theme.color(role));
        }
      };
      const auto &locks = session_.highlightLocks();
      for (int slot = 0; slot < HighlightLocks::Colors; ++slot)
        if (const QString word = locks.word(slot); !word.isEmpty())
          mark(word, highlightLockRole(slot));
      if (!highlight_.isEmpty())
        mark(highlight_, ColorRole::ListingHighlight);
    }
    QList<QTextLayout::FormatRange> selection;
    if (selected && end > 0) {
      QTextLayout::FormatRange range;
      range.start = std::max(0, begin);
      range.length = std::max(0, end - range.start);
      range.format.setBackground(selectionColor);
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
  if (!contentReported_ && rowsCurrent()) {
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
  QVector<QVector<std::pair<int, int>>> occupied(
      ::neverd::limits::gui::kListingViewMaxArrowLevels);
  for (auto &arrow : arrows) {
    const int low = std::min(arrow.from, arrow.to);
    const int high = std::max(arrow.from, arrow.to);
    arrow.level = ::neverd::limits::gui::kListingViewMaxArrowLevels - 1;
    for (int level = 0;
         level < ::neverd::limits::gui::kListingViewMaxArrowLevels; ++level) {
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
  const qreal spacing = std::max<qreal>(
      3, (arrowsWidth_ - 8) /
             qreal(::neverd::limits::gui::kListingViewMaxArrowLevels));
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
  // The highlight follows the cursor, as IDA's does, so Copy takes the word
  // it marks.
  const auto move = [&](int line, int column) {
    moveCursor(line, column, shift);
    highlight_ = currentToken();
  };
  switch (event->key()) {
  case Qt::Key_Up:
    move(cursorLine_ - 1, cursorColumn_);
    return;
  case Qt::Key_Down:
    move(cursorLine_ + 1, cursorColumn_);
    return;
  case Qt::Key_PageUp:
    top_ = std::max(0, top_ - visibleLines() + 1);
    move(cursorLine_ - visibleLines() + 1, cursorColumn_);
    return;
  case Qt::Key_PageDown:
    top_ = std::min(std::max(0, int(lines_.size()) - visibleLines()),
                    top_ + visibleLines() - 1);
    move(cursorLine_ + visibleLines() - 1, cursorColumn_);
    return;
  case Qt::Key_Left:
    move(cursorLine_, cursorColumn_ - 1);
    return;
  case Qt::Key_Right:
    move(cursorLine_, cursorColumn_ + 1);
    return;
  case Qt::Key_Home:
    if (control)
      jumpTo(space_.first());
    else
      move(cursorLine_, 0);
    return;
  case Qt::Key_End:
    if (control)
      jumpTo(space_.last());
    else if (const auto *line = cursorLine())
      move(cursorLine_, int(line->styled.text.size()));
    return;
  default:
    break;
  }
  if (event->matches(QKeySequence::Copy)) {
    // Blank space leaves the clipboard as it was, as in IDA.
    if (const QString text = copyText(); !text.isEmpty())
      QApplication::clipboard()->setText(text);
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
    highlight_ = lines_[index].styled.tokenAt(column);
    // The context menu's Copy then takes the word the click is on.
    if (selectPrefixWord(int(event->position().x())))
      emitLocation();
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
  const bool pressed = selecting_;
  selecting_ = false;
  if (anchor_ && *anchor_ == std::pair{cursorLine_, cursorColumn_})
    anchor_.reset();
  // A plain click on the address prefix selects the word under it, so Ctrl+C
  // copies the address. Selecting on release leaves a drag its own range.
  if (pressed && !anchor_ && event->button() == Qt::LeftButton &&
      !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier)))
    selectPrefixWord(int(event->position().x()));
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
  if (wheel_.zoomCodeFont(*event, lineHeight_))
    return;
  const QPoint delta = wheel_.scroll(*event, lineHeight_);
  if (delta.y())
    scrollLines(-delta.y());
  horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
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
  if (!session_.loaded())
    return;
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
  const auto serial = serial_;
  const Address target = *span->address;
  session_.read(
      QStringLiteral("listing"),
      {{"address", hexAddress(target)}, {"before", 0}, {"after", HintLines}},
      this,
      [this, global, serial](const QJsonObject &payload) {
        if (serial != serial_ || !session_.loaded())
          return;
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

QRect ListingView::cursorViewportRect() const {
  const int x = cursorLine_ >= 0 && cursorLine_ < lines_.size()
                    ? qRound(columnX(lines_[cursorLine_], cursorColumn_))
                    : textLeft();
  return QRect(x, (cursorLine_ - top_) * lineHeight_, 1, lineHeight_);
}

} // namespace neverd::gui
