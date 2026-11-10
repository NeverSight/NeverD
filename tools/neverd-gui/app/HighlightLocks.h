//===- HighlightLocks.h - Locked highlight colors ---------------*- C++ -*-===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declare the words IDA's Lock highlight keeps colored while the cursor's
/// highlight moves on.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_HIGHLIGHTLOCKS_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_HIGHLIGHTLOCKS_H

#include "../Config.h"
#include "Theme.h"

#include <QObject>
#include <QRectF>
#include <QString>
#include <array>
#include <optional>

class QPainter;

namespace neverd::gui {

/// Up to kHighlightLockColors words that stay highlighted in every view, each
/// in its own color. A register's lock covers the registers that share it
/// (`rax` with `eax`).
class HighlightLocks final : public QObject {
  Q_OBJECT
public:
  static constexpr int Colors =
      ::neverd::limits::gui_config::kHighlightLockColors;

  using QObject::QObject;

  /// The word locked in color \p slot; empty while the color is free.
  QString word(int slot) const;
  /// The color locking \p word or a register sharing it.
  std::optional<int> slotOf(const QString &word) const;
  /// Unlock \p word, or lock it in the first free color, as IDA's
  /// Ctrl+Alt+0 does. A word with every color taken stays as it is.
  void toggle(const QString &word);
  /// Unlock color \p slot, or lock \p word in it, as IDA's Ctrl+Alt+1..8
  /// do. A word another color locks moves to this one.
  void toggleColor(int slot, const QString &word);
  void clear();

signals:
  void changed();

private:
  std::array<QString, Colors> words_;
};

/// The theme color painting lock color \p slot.
ColorRole highlightLockRole(int slot);

/// Marks \p box behind a highlighted word in \p role's color: a locked word,
/// or the cursor's highlight and search matches (ListingHighlight), which a
/// high contrast theme outlines instead of filling.
void paintHighlight(QPainter &painter, const QRectF &box, ColorRole role);

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_HIGHLIGHTLOCKS_H
