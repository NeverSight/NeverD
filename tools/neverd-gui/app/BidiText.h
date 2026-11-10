//===- BidiText.h - Left-to-right runs in any language --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep paths in their own order inside right-to-left text.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_BIDI_TEXT_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_BIDI_TEXT_H

#include <QChar>
#include <QGuiApplication>
#include <QString>

namespace neverd::gui {

/// Keeps \p text, such as a path, in left-to-right order where it stands in
/// right-to-left text: a Unix path's leading slash otherwise moves to its end
/// ("mnt/data/") in an Arabic interface.  The isolate marks draw nothing, and
/// a left-to-right interface gets \p text unchanged.
inline QString leftToRight(const QString &text) {
  if (!QGuiApplication::isRightToLeft() || text.isEmpty())
    return text;
  // LEFT-TO-RIGHT ISOLATE and POP DIRECTIONAL ISOLATE.
  return QChar(0x2066) + text + QChar(0x2069);
}

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_BIDI_TEXT_H
