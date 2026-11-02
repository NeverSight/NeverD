#include "LibraryCodeView.h"

#include "ResourceLimits.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString summaryText(QString text) {
  for (auto &c : text)
    if (!c.isPrint())
      c = QChar(' ');
  return text.simplified().replace("*/", "* /").left(512);
}
} // namespace

void LibraryCodeView::reset(const QString &original,
                            const QVariantList &mappings,
                            const QJsonArray &regions, qint64 byteOffset,
                            bool keepFolded, bool sourceBlocks) {
  if (!keepFolded)
    folded_.clear();
  original_ = original;
  originalMappings_ = mappings;
  regions_.clear();
  blockStructure_.clear();
  sourceLines_ = {0};
  for (int i = 0; i < original_.size(); ++i)
    if (original_[i] == '\n')
      sourceLines_.push_back(i + 1);
  const QByteArray bytes = original.toUtf8();
  auto position = [&](const QJsonValue &value) -> int {
    // Reject absent, fractional, negative and overflowing offsets. No rounding
    // a byte in the middle of a multibyte character onto a Qt position.
    const auto number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || byteOffset < 0 ||
        byteOffset > ::neverd::limits::gui::kMaxLibraryViewByteOffset ||
        number < byteOffset || number - byteOffset > bytes.size() ||
        number != static_cast<double>(static_cast<qint64>(number)))
      return -1;
    const auto local = static_cast<qsizetype>(number - byteOffset);
    if (local < bytes.size() &&
        (static_cast<unsigned char>(bytes[local]) & 0xc0) == 0x80)
      return -1;
    return QString::fromUtf8(bytes.constData(), local).size();
  };
  QSet<QString> ids;
  qsizetype spanCount = 0;
  if (regions.size() <=
      ::neverd::limits::gui::kLibraryCodeViewResetRegionsSizeLimit)
    for (const auto &value : regions) {
      const auto object = value.toObject();
      Region region;
      region.details = object.toVariantMap();
      region.id = object["id"].toString();
      region.library = !object.contains("kind");
      region.summary = summaryText(object["display_name"].toString());
      region.available = !region.id.isEmpty() && !region.summary.isEmpty() &&
                         object["foldable"].toBool() &&
                         object["mapping_status"] == "mapped";
      const auto spans = object["spans"].toArray();
      spanCount += spans.size();
      region.available &=
          !spans.isEmpty() &&
          spans.size() <=
              ::neverd::limits::gui::
                  kLibraryCodeViewResetRegionAvailableSpansSizeLimit &&
          spanCount <= ::neverd::limits::gui::
                           kLibraryCodeViewResetRegionAvailableSpanCountLimit;
      if (region.available)
        for (const auto &entry : spans) {
          const auto span = entry.toObject();
          const int begin = position(span["begin_byte"]);
          const int end = position(span["end_byte"]);
          if (begin < 0 || end <= begin) {
            region.available = false;
            break;
          }
          region.spans.push_back({begin, end});
        }
      std::sort(region.spans.begin(), region.spans.end(),
                [](const auto &a, const auto &b) { return a.begin < b.begin; });
      for (qsizetype i = 1; i < region.spans.size(); ++i)
        if (region.spans[i].begin < region.spans[i - 1].end)
          region.available = false;
      if (ids.contains(region.id)) {
        region.available = false;
        for (auto &previous : regions_)
          if (previous.id == region.id)
            previous.available = false;
      }
      ids.insert(region.id);
      regions_.push_back(std::move(region));
    }
  // A partial page and an ambiguous range remain readable in full. Never
  // choose the first overlapping region based on its wire order.
  struct Interval {
    Span span;
    qsizetype region;
  };
  QVector<Interval> intervals;
  for (qsizetype i = 0; i < regions_.size(); ++i)
    if (regions_[i].available)
      for (const auto &span : regions_[i].spans)
        intervals.push_back({span, i});
  std::sort(intervals.begin(), intervals.end(),
            [](auto a, auto b) { return a.span.begin < b.span.begin; });
  int furthest = -1;
  qsizetype owner = 0;
  for (const auto &interval : intervals) {
    if (interval.span.begin < furthest) {
      regions_[owner].available = false;
      regions_[interval.region].available = false;
    }
    if (interval.span.end > furthest) {
      furthest = interval.span.end;
      owner = interval.region;
    }
  }
  // Local brace folds are presentation ranges, not worker semantic evidence.
  // Keep the existing validation and wire budget for library regions intact.
  if (sourceBlocks)
    addSourceBlocks();
  QSet<QString> valid;
  for (const auto &region : regions_)
    if (region.available)
      valid.insert(region.id);
  folded_.intersect(valid);
  rebuild();
}

void LibraryCodeView::addSourceBlocks() {
  struct Open {
    int position, ordinal;
  };
  QVector<Open> stack;
  QVector<Region> blocks;
  QString structure;
  int ordinal = 0;
  int metadataOrdinal = 0;
  const int size = int(original_.size());
  for (int i = 0; i < size;) {
    const QChar c = original_[i];
    if (QStringView(original_).mid(i, 2) == QLatin1String("//")) {
      const int end = int(original_.indexOf('\n', i + 2));
      i = end < 0 ? size : end;
      continue;
    }
    if (QStringView(original_).mid(i, 2) == QLatin1String("/*")) {
      const int begin = i;
      int depth = 1;
      i += 2;
      while (i < size && depth) {
        const auto pair = QStringView(original_).mid(i, 2);
        if (pair == QLatin1String("/*")) {
          ++depth;
          i += 2;
        } else if (pair == QLatin1String("*/")) {
          --depth;
          i += 2;
        } else {
          ++i;
        }
      }
      if (depth)
        return;
      if (QStringView(original_).mid(begin).startsWith(
              QLatin1String("/* neverd.exception:"))) {
        if (++metadataOrdinal + ordinal >
            ::neverd::limits::gui::kMaxLibraryViewOrdinal)
          return;
        const int newline = int(original_.indexOf('\n', begin));
        const auto closingLine = std::upper_bound(sourceLines_.cbegin(),
                                                  sourceLines_.cend(), i - 1) -
                                 1;
        if (newline >= 0 && newline < *closingLine - 1) {
          Region detail;
          detail.id =
              QStringLiteral("source-exception:%1").arg(metadataOrdinal);
          detail.header = begin;
          detail.summary = QStringLiteral(" …\n");
          detail.spans = {{newline, *closingLine}};
          detail.library = false;
          detail.available = true;
          const QString header = original_.mid(begin, newline - begin);
          const QString body = original_.mid(newline, *closingLine - newline);
          detail.details = {
              {"id", detail.id},
              {"kind", "source-metadata"},
              {"default_detail", header.contains("status=complete,") &&
                                     body.contains("fallback_regions=0\n")}};
          blocks.append(std::move(detail));
        }
      }
      continue;
    }
    // C++ raw strings, including their optional encoding prefix.
    if (c == 'R' && i + 1 < size && original_[i + 1] == '"') {
      const int open = int(original_.indexOf('(', i + 2));
      if (open < 0 || open - i - 2 > 16)
        return;
      const auto delimiter = original_.mid(i + 2, open - i - 2);
      for (const auto ch : delimiter)
        if (ch.isSpace() || ch == '\\' || ch == ')')
          return;
      const QString terminator = ")" + delimiter + "\"";
      const int end = int(original_.indexOf(terminator, open + 1));
      if (end < 0)
        return;
      i = end + int(terminator.size());
      continue;
    }
    // Rust raw strings (r"...", r#"..."# and byte-string prefixes).
    if (c == 'r') {
      int quote = i + 1;
      while (quote < size && original_[quote] == '#')
        ++quote;
      if (quote < size && original_[quote] == '"') {
        const QString terminator = "\"" + QString(quote - i - 1, '#');
        const int end = int(original_.indexOf(terminator, quote + 1));
        if (end < 0)
          return;
        i = end + int(terminator.size());
        continue;
      }
    }
    if (c == '"' || c == '\'' || c == '`') {
      int end = i + 1;
      while (end < size && original_[end] != c) {
        if (c == '\'' && original_[end] == '\n')
          break;
        if (c != '`' && original_[end] == '\\')
          end += 2;
        else
          ++end;
      }
      if (end < size && original_[end] == c) {
        i = end + 1;
        continue;
      }
      // An apostrophe may introduce a Rust lifetime rather than a character.
      if (c != '\'')
        return;
    }
    if (c == '{') {
      if (++ordinal + metadataOrdinal >
          ::neverd::limits::gui::kMaxLibraryViewOrdinal)
        return;
      stack.append({i, ordinal});
      structure += '{';
    } else if (c == '}') {
      if (stack.isEmpty())
        return;
      const auto open = stack.takeLast();
      structure += '}';
      const int firstNewline = int(original_.indexOf('\n', open.position));
      const auto closingLine =
          std::upper_bound(sourceLines_.cbegin(), sourceLines_.cend(), i) - 1;
      // Keep both the header and closing line visible. In particular an
      // `} else {` header must retain its own arrow, tokens and row mapping.
      if (firstNewline >= 0 && firstNewline < *closingLine - 1) {
        Region block;
        block.id = QStringLiteral("source-block:%1").arg(open.ordinal);
        block.header = open.position;
        block.summary = QStringLiteral(" /* … */\n");
        block.spans = {{firstNewline, *closingLine}};
        block.library = false;
        block.available = true;
        block.details = {{"id", block.id}, {"kind", "source-block"}};
        const auto headerLine =
            std::upper_bound(sourceLines_.cbegin(), sourceLines_.cend(),
                             open.position) -
            1;
        const QString header =
            original_.mid(*headerLine, open.position - *headerLine + 1);
        const int end = int(original_.indexOf('\n', i));
        const QString closing =
            original_.mid(i, end < 0 ? size - i : end - i).trimmed();
        static const QRegularExpression frameHeader(QStringLiteral(
            R"(^\s*(?:_Alignas|alignas)\([0-9]+\) struct frame_[0-9A-Fa-f]+ \{$)"));
        block.details["default_detail"] =
            !stack.isEmpty() && frameHeader.match(header).hasMatch() &&
            closing == QLatin1String("} stack_storage;");
        blocks.append(std::move(block));
      }
    }
    ++i;
  }
  if (!stack.isEmpty())
    return;
  blockStructure_ = structure;
  const int semanticRegions = int(regions_.size());
  for (auto &block : blocks) {
    const auto body = block.spans.front();
    for (int index = 0; index < semanticRegions; ++index) {
      const auto &region = regions_[index];
      if (region.id == block.id)
        block.available = false;
      if (region.available)
        for (const auto &span : region.spans) {
          const bool overlaps = body.begin < span.end && span.begin < body.end;
          const bool contained =
              (body.begin <= span.begin && body.end >= span.end) ||
              (span.begin <= body.begin && span.end >= body.end);
          if (overlaps && !contained)
            block.available = false;
        }
    }
    regions_.append(std::move(block));
  }
}

QVector<LibraryCodeView::Block> LibraryCodeView::blocks() const {
  QVector<Block> result;
  for (const auto &region : regions_) {
    if (!region.available || region.header < 0)
      continue;
    const int position = displayPosition(region.header);
    // A parent fold may hide this header. Do not put the child's arrow on
    // the parent's summary, or on a recognized library summary.
    if (originalPosition(position) != region.header)
      continue;
    const int line = int(std::upper_bound(displayLines_.cbegin(),
                                          displayLines_.cend(), position) -
                         displayLines_.cbegin()) -
                     1;
    result.append({region.id, line, isFolded(region.id)});
  }
  return result;
}

QSet<QString> LibraryCodeView::foldedBlocks() const {
  QSet<QString> result;
  for (const auto &region : regions_)
    if (region.header >= 0 && isFolded(region.id))
      result.insert(region.id);
  return result;
}

QSet<QString> LibraryCodeView::defaultDetailBlocks() const {
  QSet<QString> result;
  for (const auto &region : regions_)
    if (region.available && region.details.value("default_detail").toBool())
      result.insert(region.id);
  return result;
}

QString LibraryCodeView::blockAt(int position) const {
  const int source = originalPosition(position);
  const Region *best = nullptr;
  for (const auto &region : regions_)
    if (region.available && region.header >= 0 && source >= region.header &&
        source < region.spans.front().end &&
        (!best || region.header > best->header))
      best = &region;
  return best ? best->id : QString();
}

void LibraryCodeView::setFoldedBlocks(const QSet<QString> &ids) {
  for (const auto &region : regions_)
    if (region.header >= 0 && region.available) {
      if (ids.contains(region.id))
        folded_.insert(region.id);
      else
        folded_.remove(region.id);
    }
  rebuild();
}

QVariantList LibraryCodeView::regions() const {
  QVariantList result;
  for (const auto &region : regions_) {
    auto details = region.details;
    details["available"] = region.available;
    details["folded"] = folded_.contains(region.id);
    result.push_back(details);
  }
  return result;
}

int LibraryCodeView::foldableCount() const {
  return std::count_if(
      regions_.begin(), regions_.end(),
      [](const auto &region) { return region.available && region.library; });
}

bool LibraryCodeView::libraryFolded() const {
  return std::any_of(regions_.begin(), regions_.end(), [&](const auto &region) {
    return region.library && folded_.contains(region.id);
  });
}

bool LibraryCodeView::canFold() const {
  return std::any_of(regions_.begin(), regions_.end(),
                     [](const auto &region) { return region.available; });
}

bool LibraryCodeView::hasRegion(const QString &id) const {
  return std::any_of(regions_.begin(), regions_.end(), [&](const auto &region) {
    return region.available && region.id == id;
  });
}

int LibraryCodeView::displayLine(int sourceLine) const {
  if (sourceLine < 0 || sourceLine >= sourceLines_.size())
    return -1;
  const int at = displayPosition(sourceLines_[sourceLine]);
  return int(std::upper_bound(displayLines_.cbegin(), displayLines_.cend(),
                              at) -
             displayLines_.cbegin()) -
         1;
}

void LibraryCodeView::toggleRegion(const QString &id) {
  setRegionFolded(id, !folded_.contains(id));
}

bool LibraryCodeView::unfoldSourceLine(int sourceLine) {
  if (sourceLine < 0 || sourceLine >= sourceLines_.size())
    return false;
  const int begin = sourceLines_[sourceLine];
  // The row's text, without its newline: a block folded under a visible
  // header begins at the header's newline.
  const int end = sourceLine + 1 < sourceLines_.size()
                      ? std::max(begin + 1, sourceLines_[sourceLine + 1] - 1)
                      : int(original_.size());
  bool changed = false;
  for (const auto &region : regions_)
    if (folded_.contains(region.id))
      for (const auto &span : region.spans)
        if (span.begin < end && span.end > begin) {
          folded_.remove(region.id);
          changed = true;
          break;
        }
  if (changed)
    rebuild();
  return changed;
}

void LibraryCodeView::setRegionFolded(const QString &id, bool folded) {
  for (const auto &region : regions_)
    if (region.id == id && region.available) {
      if (folded)
        folded_.insert(id);
      else
        folded_.remove(id);
      rebuild();
      return;
    }
}

void LibraryCodeView::setFolded(bool folded) {
  for (const auto &region : regions_)
    if (region.available && region.library) {
      if (folded)
        folded_.insert(region.id);
      else
        folded_.remove(region.id);
    }
  rebuild();
}

void LibraryCodeView::rebuild() {
  struct Fold {
    Span span;
    QString id, summary;
    bool block;
  };
  QVector<Fold> folds;
  for (const auto &region : regions_)
    if (region.available && folded_.contains(region.id))
      for (const auto &span : region.spans)
        folds.push_back({span, region.id, region.summary, region.header >= 0});
  std::sort(folds.begin(), folds.end(), [](const auto &a, const auto &b) {
    if (a.span.begin != b.span.begin)
      return a.span.begin < b.span.begin;
    if (a.span.end != b.span.end)
      return a.span.end > b.span.end;
    return a.block && !b.block;
  });
  text_.clear();
  segments_.clear();
  int cursor = 0;
  auto append = [&](int begin, int end, const QString &text, const QString &id,
                    bool block = false) {
    if (text.isEmpty())
      return;
    const int start = text_.size();
    text_ += text;
    segments_.push_back(
        {start, static_cast<int>(text_.size()), begin, end, id, block});
  };
  for (const auto &fold : folds) {
    // Keep a hidden child's folded state, but project only the outermost
    // folded range. Expanding the parent reveals the child as it was.
    if (fold.span.begin < cursor)
      continue;
    append(cursor, fold.span.begin,
           original_.mid(cursor, fold.span.begin - cursor), {});
    append(fold.span.begin, fold.span.end,
           fold.block ? fold.summary : "/* " + fold.summary + " … */", fold.id,
           fold.block);
    cursor = fold.span.end;
  }
  append(cursor, original_.size(), original_.mid(cursor), {});

  displayLines_ = {0};
  for (int i = 0; i < text_.size(); ++i)
    if (text_[i] == '\n')
      displayLines_.push_back(i + 1);
  mappings_.clear();
  for (const auto &entry : originalMappings_) {
    auto row = entry.toMap();
    const int line = row["line"].toInt();
    if (line < 0 || line >= sourceLines_.size())
      continue;
    const auto next = std::upper_bound(segments_.cbegin(), segments_.cend(),
                                       sourceLines_[line],
                                       [](int at, const Segment &segment) {
                                         return at < segment.sourceBegin;
                                       });
    // A hidden statement's address is not evidence for its visible branch
    // header. Explicit navigation expands the original row before selecting
    // it; library-operation summaries retain their existing mapping behavior.
    if (next != segments_.cbegin() && (next - 1)->block)
      continue;
    const int at = displayPosition(sourceLines_[line]);
    row["line"] =
        std::upper_bound(displayLines_.begin(), displayLines_.end(), at) -
        displayLines_.begin() - 1;
    mappings_.push_back(row);
  }
  emit changed();
}

int LibraryCodeView::displayPosition(int position) const {
  const auto next = std::upper_bound(
      segments_.cbegin(), segments_.cend(), position,
      [](int at, const Segment &segment) { return at < segment.sourceBegin; });
  if (next == segments_.cbegin() || position >= original_.size())
    return int(text_.size());
  const auto &segment = *(next - 1);
  return segment.region.isEmpty()
             ? segment.begin + position - segment.sourceBegin
             : segment.begin;
}

int LibraryCodeView::sourcePosition(int position, bool end) const {
  position = std::clamp(position, 0, static_cast<int>(text_.size()));
  const auto next = std::upper_bound(
      segments_.cbegin(), segments_.cend(), position,
      [](int at, const Segment &segment) { return at < segment.begin; });
  if (next == segments_.cbegin() || position == text_.size())
    return int(original_.size());
  const auto &segment = *(next - 1);
  if (position == segment.begin)
    return segment.sourceBegin;
  return segment.region.isEmpty()
             ? segment.sourceBegin + position - segment.begin
             : (end ? segment.sourceEnd : segment.sourceBegin);
}

QVector<QPair<int, int>> LibraryCodeView::foldedRanges() const {
  QVector<QPair<int, int>> ranges;
  for (const auto &segment : segments_)
    if (!segment.region.isEmpty())
      ranges.append({segment.begin, segment.end});
  return ranges;
}

QString LibraryCodeView::regionAt(int position) const {
  // A block summary ends its header's row; that newline is not the summary.
  for (const auto &segment : segments_)
    if (position >= segment.begin &&
        position < segment.end - (segment.block ? 1 : 0))
      return segment.region;
  return {};
}

int LibraryCodeView::sourceLineAt(int position) const {
  const int at = sourcePosition(position, false);
  return int(std::upper_bound(sourceLines_.cbegin(), sourceLines_.cend(), at) -
             sourceLines_.cbegin()) -
         1;
}

QString LibraryCodeView::originalSelection(int begin, int end) const {
  if (begin >= end)
    return {};
  const int start = sourcePosition(begin, false);
  return original_.mid(start, sourcePosition(end, true) - start);
}

void LibraryCodeView::copySelection(int begin, int end) const {
  if (auto *clipboard = QGuiApplication::clipboard())
    clipboard->setText(originalSelection(begin, end));
}
