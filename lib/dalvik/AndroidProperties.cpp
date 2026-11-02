//===- AndroidProperties.cpp - Android property value conversion ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared API 28 signed property integer conversion with caller defaults.
//===----------------------------------------------------------------------===//
#include "neverd/dalvik/AndroidProperties.h"

#include <charconv>

namespace neverd::mobile::dalvik {
int32_t androidPropertyInt(std::string_view text, int32_t fallback) {
  // API 28 Android base ParseInt selects hexadecimal from the original prefix,
  // then accepts leading C whitespace/signs but no trailing characters.
  const bool hex = text.starts_with("0x") || text.starts_with("0X");
  while (!text.empty() && std::string_view(" \t\n\r\f\v").find(text[0]) !=
                              std::string_view::npos)
    text.remove_prefix(1);
  const bool negative = !text.empty() && text[0] == '-';
  if (!text.empty() && (text[0] == '+' || text[0] == '-'))
    text.remove_prefix(1);
  if (hex)
    text.remove_prefix(2);
  if (text.empty())
    return fallback;
  uint64_t magnitude = 0;
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(),
                                      magnitude, hex ? 16 : 10);
  if (error != std::errc() || end != text.data() + text.size() ||
      magnitude > uint64_t(INT32_MAX) + negative)
    return fallback;
  return negative ? -int64_t(magnitude) : int64_t(magnitude);
}
} // namespace neverd::mobile::dalvik
