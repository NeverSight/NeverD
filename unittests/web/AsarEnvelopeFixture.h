//===- AsarEnvelopeFixture.h - Bounded ASAR archive extraction test fixtures -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded ASAR archive extraction test fixtures.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace neverd::web::test {
inline void asarU32(std::string &S, size_t At, uint32_t V) {
  for (unsigned I = 0; I < 4; ++I)
    S.at(At + I) = char(V >> (I * 8));
}
inline std::string asarBytes(std::string_view JSON,
                             std::string_view Data = {}) {
  const uint32_t Padded = (uint32_t(JSON.size()) + 3) & ~3U;
  std::string S(16 + Padded, '\0');
  asarU32(S, 0, 4);
  asarU32(S, 4, Padded + 8);
  asarU32(S, 8, Padded + 4);
  asarU32(S, 12, uint32_t(JSON.size()));
  S.replace(16, JSON.size(), JSON);
  S += Data;
  return S;
}
} // namespace neverd::web::test
