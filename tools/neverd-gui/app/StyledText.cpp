#include "StyledText.h"

#include "../Config.h"

#include <QFontMetricsF>
#include <QHash>
#include <QTextCharFormat>
#include <algorithm>
#include <cmath>

namespace neverd::gui {
namespace {

/// Whether a fixed-width listing draws \p code two columns wide; the worker
/// aligns columns by the same table.
bool isWideCharacter(char32_t code) {
#define NEVERD_WIDE_CHARACTERS(First, Last)                                    \
  if (code >= (First) && code <= (Last))                                       \
    return true;
#include "neverd/support/WideCharacters.def"
  return false;
}
} // namespace

bool isTokenCharacter(QChar c) {
  return c.isLetterOrNumber() || c == QLatin1Char('_') ||
         c == QLatin1Char('$') || c == QLatin1Char('@') ||
         c == QLatin1Char('?') || c == QLatin1Char('.');
}

namespace {
/// Whether the character at \p at continues a word of \p text.
bool continuesToken(const QString &text, qsizetype at, TokenDots dots) {
  const QChar c = text.at(at);
  if (c != QLatin1Char('.') || dots == TokenDots::Join)
    return isTokenCharacter(c);
  return at > 0 && at + 1 < text.size() && text.at(at - 1).isDigit() &&
         text.at(at + 1).isDigit();
}
} // namespace

void StyledLine::setFromWorker(const QString &line,
                               const QJsonArray &workerSpans) {
  text = line;
  spans.clear();
  spans.reserve(workerSpans.size());
  layout_.reset();
  // Worker offsets count UTF-8 bytes.  For ASCII both encodings agree; other
  // text maps each byte offset to its UTF-16 position.
  bool ascii = true;
  for (const QChar c : line)
    if (c.unicode() >= 0x80) {
      ascii = false;
      break;
    }
  QVector<int> byteToUtf16;
  if (!ascii) {
    const QByteArray utf8 = line.toUtf8();
    byteToUtf16.resize(utf8.size() + 1);
    int utf16 = 0;
    for (int byte = 0; byte < utf8.size();) {
      const auto lead = static_cast<unsigned char>(utf8[byte]);
      const int width = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
      for (int k = 0; k < width && byte + k < utf8.size(); ++k)
        byteToUtf16[byte + k] = utf16;
      byte += width;
      utf16 += width == 4 ? 2 : 1;
    }
    byteToUtf16[utf8.size()] = utf16;
  }
  const auto position = [&](int byte) {
    if (ascii)
      return std::clamp(byte, 0, int(line.size()));
    return byteToUtf16.value(std::clamp(byte, 0, int(byteToUtf16.size()) - 1));
  };
  for (const auto &value : workerSpans) {
    const auto entry = value.toArray();
    if (entry.size() < 3)
      continue;
    const int begin = position(entry.at(0).toInt());
    const int end = position(entry.at(0).toInt() + entry.at(1).toInt());
    StyledSpan span{begin, end - begin, entry.at(2).toInt(), std::nullopt};
    if (entry.size() > 3)
      span.address = addressValue(entry.at(3));
    if (span.length > 0)
      spans.append(span);
  }
}

const StyledSpan *StyledLine::spanAt(int position) const {
  for (const auto &span : spans)
    if (position >= span.start && position < span.start + span.length)
      return &span;
  return nullptr;
}

QString StyledLine::tokenAt(int position, TokenDots dots) const {
  if (position < 0 || position > text.size())
    return {};
  const auto [begin, end] = tokenRange(text, position, dots);
  return text.mid(begin, end - begin);
}

QTextLayout &StyledLine::layout(const QFont &font, quint64 stamp,
                                const ColorOf &colorOf) const {
  if (layout_ && stamp_ == stamp)
    return *layout_;
  layout_ = std::make_unique<QTextLayout>(text, font);
  layout_->setCacheEnabled(true);
  QList<QTextLayout::FormatRange> formats;
  formats.reserve(spans.size() + 1);
  QTextLayout::FormatRange base;
  base.start = 0;
  base.length = text.size();
  base.format.setForeground(colorOf(0));
  formats.append(base);
  for (const auto &span : spans) {
    QTextLayout::FormatRange range;
    range.start = span.start;
    range.length = span.length;
    range.format.setForeground(colorOf(span.role));
    formats.append(range);
  }
  layout_->setFormats(formats);
  QTextOption option;
  option.setWrapMode(QTextOption::NoWrap);
  layout_->setTextOption(option);
  const QFontMetricsF metrics(font);
  const auto layOut = [this, &metrics] {
    layout_->beginLayout();
    QTextLine line = layout_->createLine();
    layout_->endLayout();
    if (line.isValid()) {
      // A script fallback can have a taller ascent than the code font. Keep
      // the row's baseline fixed when a comment introduces that fallback.
      line.setPosition(QPointF(0, metrics.ascent() - line.ascent()));
    }
  };
  layOut();
  // Text beyond ASCII keeps to the fixed-width grid: every character takes
  // its columns, two for a wide one (WideCharacters.def), whichever font
  // draws it.  A fallback font draws a script's characters at its own
  // widths, and with them the spaces and punctuation Qt itemizes into the
  // same run, so each character's measured advance is corrected.
  if (std::any_of(text.begin(), text.end(),
                  [](QChar c) { return c.unicode() >= 0x80; })) {
    const qreal column = metrics.horizontalAdvance(QLatin1Char('M'));
    const QTextLine line = layout_->lineAt(0);
    bool corrected = false;
    for (qsizetype i = 0; i < text.size();) {
      const bool pair = text.at(i).isHighSurrogate() && i + 1 < text.size() &&
                        text.at(i + 1).isLowSurrogate();
      const char32_t code =
          pair ? QChar::surrogateToUcs4(text.at(i), text.at(i + 1))
               : text.at(i).unicode();
      const int units = pair ? 2 : 1;
      const qreal advance =
          line.cursorToX(int(i + units)) - line.cursorToX(int(i));
      const qreal expected = (isWideCharacter(code) ? 2 : 1) * column;
      if (std::abs(advance - expected) >
          ::neverd::limits::gui_config::kStyledTextGridTolerance) {
        QTextLayout::FormatRange range;
        range.start = int(i);
        range.length = units;
        range.format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        range.format.setFontLetterSpacing(expected - advance);
        formats.append(range);
        corrected = true;
      }
      i += units;
    }
    if (corrected) {
      layout_->setFormats(formats);
      layOut();
    }
  }
  stamp_ = stamp;
  return *layout_;
}

std::pair<int, int> tokenRange(const QString &text, int position,
                               TokenDots dots) {
  if (position < 0 || position > text.size())
    return {0, 0};
  int begin = position, end = position;
  while (begin > 0 && continuesToken(text, begin - 1, dots))
    --begin;
  while (end < text.size() && continuesToken(text, end, dots))
    ++end;
  return {begin, end};
}

QStringList registerFamily(const QString &word) {
  static const QHash<QString, QStringList> families = [] {
    QHash<QString, QStringList> byName;
    // A name two architectures share (`sp`) joins both families; a listing
    // spells only its own architecture's registers.
    const auto add = [&byName](QStringList family) {
      for (const auto &name : QStringList(family))
        for (const auto &other : byName.value(name))
          if (!family.contains(other))
            family.append(other);
      for (const auto &name : family)
        byName.insert(name, family);
    };
    // x86: a 64-bit register with its 32-, 16- and 8-bit parts.
    for (const char *letter : {"a", "b", "c", "d"})
      add({QStringLiteral("r%1x").arg(QLatin1String(letter)),
           QStringLiteral("e%1x").arg(QLatin1String(letter)),
           QStringLiteral("%1x").arg(QLatin1String(letter)),
           QStringLiteral("%1l").arg(QLatin1String(letter)),
           QStringLiteral("%1h").arg(QLatin1String(letter))});
    for (const char *base : {"si", "di", "bp", "sp"})
      add({QStringLiteral("r%1").arg(QLatin1String(base)),
           QStringLiteral("e%1").arg(QLatin1String(base)), QLatin1String(base),
           QStringLiteral("%1l").arg(QLatin1String(base))});
    add({QStringLiteral("rip"), QStringLiteral("eip"), QStringLiteral("ip")});
    for (int n = 8; n <= 15; ++n)
      add({QStringLiteral("r%1").arg(n), QStringLiteral("r%1d").arg(n),
           QStringLiteral("r%1w").arg(n), QStringLiteral("r%1b").arg(n)});
    // AArch64: a general register's X and W views, and a vector register's.
    for (int n = 0; n <= 30; ++n)
      add({QStringLiteral("x%1").arg(n), QStringLiteral("w%1").arg(n)});
    add({QStringLiteral("x29"), QStringLiteral("fp")});
    add({QStringLiteral("x30"), QStringLiteral("lr")});
    add({QStringLiteral("sp"), QStringLiteral("wsp")});
    add({QStringLiteral("xzr"), QStringLiteral("wzr")});
    for (int n = 0; n <= 31; ++n)
      add({QStringLiteral("v%1").arg(n), QStringLiteral("q%1").arg(n),
           QStringLiteral("d%1").arg(n), QStringLiteral("s%1").arg(n),
           QStringLiteral("h%1").arg(n), QStringLiteral("b%1").arg(n)});
    return byName;
  }();
  return families.value(word);
}

QVector<std::pair<int, int>> highlightRanges(const StyledLine &line,
                                             const QString &word,
                                             std::optional<int> registerRole,
                                             TokenDots dots) {
  QVector<std::pair<int, int>> ranges;
  for (const int at : tokenOccurrences(line.text, word, dots))
    ranges.append({at, at + int(word.size())});
  const QStringList family =
      registerRole ? registerFamily(word) : QStringList();
  if (family.size() < 2)
    return ranges;
  for (const auto &span : line.spans)
    if (span.role == *registerRole) {
      const QString name = line.text.mid(span.start, span.length);
      if (name != word && family.contains(name))
        ranges.append({span.start, span.start + span.length});
    }
  return ranges;
}

QVector<int> tokenOccurrences(const QString &text, const QString &token,
                              TokenDots dots) {
  QVector<int> result;
  if (token.isEmpty())
    return result;
  for (qsizetype at = text.indexOf(token); at >= 0;
       at = text.indexOf(token, at + 1)) {
    const bool startOk = at == 0 || !continuesToken(text, at - 1, dots);
    const qsizetype after = at + token.size();
    const bool endOk =
        after >= text.size() || !continuesToken(text, after, dots);
    if (startOk && endOk)
      result.append(int(at));
  }
  return result;
}

} // namespace neverd::gui
