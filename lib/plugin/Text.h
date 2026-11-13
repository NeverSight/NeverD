//===- Text.h - Version-one plugin text limits -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// UTF-16 units in validated UTF-8 JSON strings, preserving v1 Qt limits.
///
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_PLUGIN_TEXT_H
#define NEVERD_LIB_PLUGIN_TEXT_H

#include <cstddef>
#include <string_view>

namespace neverd::plugin::detail {
inline size_t codePoints(std::string_view Text) {
  size_t Count = 0;
  for (unsigned char C : Text)
    Count += (C & 0xc0) != 0x80;
  return Count;
}
inline size_t textUnits(std::string_view Text) {
  size_t Units = 0;
  for (unsigned char C : Text)
    Units += ((C & 0xc0) != 0x80) + (C >= 0xf0);
  return Units;
}
inline std::string_view textPrefix(std::string_view Text, size_t Limit) {
  size_t Units = 0, I = 0;
  while (I < Text.size()) {
    const auto C = static_cast<unsigned char>(Text[I]);
    const size_t Next = C >= 0xf0 ? 2 : 1;
    if (Units + Next > Limit)
      break;
    Units += Next;
    I += C < 0x80 ? 1 : C < 0xe0 ? 2 : C < 0xf0 ? 3 : 4;
  }
  return Text.substr(0, I);
}
} // namespace neverd::plugin::detail

#endif // NEVERD_LIB_PLUGIN_TEXT_H
