//===- SHA512.h - Streaming SHA-384 and SHA-512 -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Streaming SHA-384 and SHA-512 digests.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_SHA512_H
#define NEVERD_SUPPORT_SHA512_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace neverd {
class SHA512 {
  std::array<uint64_t, 8> State;
  std::array<uint8_t, 128> Pending{};
  uint64_t Bytes = 0, HighBytes = 0;
  std::size_t Used = 0;
  bool Use384;
  void block(const uint8_t *Data);

public:
  explicit SHA512(bool Use384 = false);
  void update(std::string_view Data);
  /// Return raw digest bytes without consuming the stream.
  std::string result() const;
};
} // namespace neverd
#endif // NEVERD_SUPPORT_SHA512_H
