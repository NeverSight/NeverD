//===- SHA512.h - Bounded SHA-384 and SHA-512 -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Streaming digests for offline package integrity evidence.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_PACKAGES_SHA512_H
#define NEVERD_WEB_PACKAGES_SHA512_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace neverd::web {
class SHA512 {
  std::array<uint64_t, 8> State;
  std::array<uint8_t, 128> Pending{};
  uint64_t Bytes = 0;
  size_t Used = 0;
  bool Use384;
  void block(const uint8_t *Data);

public:
  explicit SHA512(bool Use384 = false);
  void update(std::string_view Data);
  /// Return raw digest bytes without consuming the stream.
  std::string result() const;
};
} // namespace neverd::web
#endif // NEVERD_WEB_PACKAGES_SHA512_H
