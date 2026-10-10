//===- BunContainer.h - Bun native container locations -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bun native container locations.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Bun.h"
#include "neverd/web/Error.h"

namespace neverd::web::bun_detail {
struct Range {
  uint64_t Offset = 0, Size = 0;
  uint64_t end() const { return Offset + Size; }
};

inline void within(Range R, uint64_t Size) {
  if (R.Offset > Size || R.Size > Size - R.Offset)
    throw Error("bun_range_out_of_bounds");
}

inline bool overlaps(Range A, Range B) {
  return A.Size && B.Size && A.Offset < B.end() && B.Offset < A.end();
}

inline uint64_t number(std::string_view Bytes, size_t Offset, size_t Width) {
  within({Offset, Width}, Bytes.size());
  uint64_t Value = 0;
  for (size_t I = 0; I < Width; ++I)
    Value |= uint64_t(uint8_t(Bytes[Offset + I])) << (8 * I);
  return Value;
}

struct Reader {
  Blob Bytes;
  std::string read(Range R) const {
    within(R, Bytes.size());
    return Bytes.read(R.Offset, R.Size);
  }
  uint64_t integer(uint64_t Offset, size_t Width) const {
    return number(read({Offset, Width}), 0, Width);
  }
};

struct Container {
  Range Graph;
  std::string Format, Platform, Architecture;
};

Container locate(const Reader &R);
} // namespace neverd::web::bun_detail
