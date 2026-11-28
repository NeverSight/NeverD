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

#include "neverd/support/SHA512.h"
#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"

namespace neverd::web {
class SHA512 {
  ::neverd::SHA512 Hash;
  uint64_t Bytes = 0;

public:
  explicit SHA512(bool Use384 = false) : Hash(Use384) {}
  void update(std::string_view Data) {
    if (Data.size() > Limits::HardInputBytes - Bytes)
      throw Error("integrity_byte_budget_exceeded");
    Hash.update(Data);
    Bytes += Data.size();
  }
  std::string result() const { return Hash.result(); }
};
} // namespace neverd::web

#endif // NEVERD_WEB_PACKAGES_SHA512_H
