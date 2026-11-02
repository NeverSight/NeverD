//===- ViewSearch.cpp - Literal view search ------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared literal matching for the find dialog and both text renderers.
//===----------------------------------------------------------------------===//

#include "ViewSearch.h"

namespace neverd::gui {

QVector<int> ViewSearchQuery::matches(QStringView text) const {
  QVector<int> result;
  if (Text.isEmpty())
    return result;
  const auto word = [text](qsizetype at) {
    if (at < 0 || at >= text.size())
      return false;
    const QChar c = text[at];
    char32_t codepoint = c.unicode();
    if (c.isHighSurrogate() && at + 1 < text.size() &&
        text[at + 1].isLowSurrogate())
      codepoint = QChar::surrogateToUcs4(c, text[at + 1]);
    else if (c.isLowSurrogate() && at > 0 && text[at - 1].isHighSurrogate())
      codepoint = QChar::surrogateToUcs4(text[at - 1], c);
    return QChar::isLetterOrNumber(codepoint) || QChar::isMark(codepoint) ||
           codepoint == U'_';
  };
  for (qsizetype at = text.indexOf(Text, 0, Case); at >= 0;
       at = text.indexOf(Text, at + Text.size(), Case)) {
    const auto end = at + Text.size();
    if (!WholeWords || (!word(at - 1) && !word(end)))
      result.append(int(at));
  }
  return result;
}

} // namespace neverd::gui
