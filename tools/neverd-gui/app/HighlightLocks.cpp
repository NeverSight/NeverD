//===- HighlightLocks.cpp - Locked highlight colors -----------------------===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Lock and unlock highlight colors the way IDA's Search > Lock highlight
/// commands do.
//===----------------------------------------------------------------------===//

#include "HighlightLocks.h"

#include "StyledText.h"

#include <algorithm>

namespace neverd::gui {

QString HighlightLocks::word(int slot) const {
  return slot >= 0 && slot < Colors ? words_[slot] : QString();
}

std::optional<int> HighlightLocks::slotOf(const QString &word) const {
  if (word.isEmpty())
    return std::nullopt;
  const QStringList family = registerFamily(word);
  for (int slot = 0; slot < Colors; ++slot)
    if (!words_[slot].isEmpty() &&
        (words_[slot] == word || family.contains(words_[slot])))
      return slot;
  return std::nullopt;
}

void HighlightLocks::toggle(const QString &word) {
  if (const auto slot = slotOf(word)) {
    words_[*slot].clear();
    emit changed();
    return;
  }
  if (word.isEmpty())
    return;
  for (auto &locked : words_)
    if (locked.isEmpty()) {
      locked = word;
      emit changed();
      return;
    }
}

void HighlightLocks::toggleColor(int slot, const QString &word) {
  if (slot < 0 || slot >= Colors)
    return;
  if (!words_[slot].isEmpty()) {
    words_[slot].clear();
    emit changed();
    return;
  }
  if (word.isEmpty())
    return;
  if (const auto other = slotOf(word))
    words_[*other].clear();
  words_[slot] = word;
  emit changed();
}

static_assert(static_cast<int>(ColorRole::HighlightLock8) -
                      static_cast<int>(ColorRole::HighlightLock1) + 1 ==
                  HighlightLocks::Colors,
              "ThemeColors.def holds one HighlightLock role per color");

ColorRole highlightLockRole(int slot) {
  return static_cast<ColorRole>(
      static_cast<int>(ColorRole::HighlightLock1) +
      std::clamp(slot, 0, HighlightLocks::Colors - 1));
}

void HighlightLocks::clear() {
  if (std::all_of(words_.begin(), words_.end(),
                  [](const QString &word) { return word.isEmpty(); }))
    return;
  for (auto &word : words_)
    word.clear();
  emit changed();
}

} // namespace neverd::gui
