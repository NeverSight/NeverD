//===- Reader.h - Bounded SEA resource locations --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded SEA resource locations.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Error.h"
#include "neverd/web/SEA.h"

#include <algorithm>
#include <optional>

namespace neverd::web::sea_detail {
struct Range {
  uint64_t Offset = 0, Size = 0;
  uint64_t end() const { return Offset + Size; }
};
inline void within(Range R, uint64_t Size) {
  if (R.Offset > Size || R.Size > Size - R.Offset)
    throw Error("sea_range_out_of_bounds");
}
inline bool contains(Range A, Range B) {
  return B.Offset >= A.Offset && B.Offset - A.Offset <= A.Size &&
         B.Size <= A.Size - (B.Offset - A.Offset);
}
inline bool overlaps(Range A, Range B) {
  return A.Size && B.Size && A.Offset < B.end() && B.Offset < A.end();
}
inline uint64_t number(std::string_view B, uint64_t At, unsigned Width) {
  within({At, Width}, B.size());
  uint64_t V = 0;
  for (unsigned I = 0; I < Width; ++I)
    V |= uint64_t(uint8_t(B[At + I])) << (8 * I);
  return V;
}
inline bool fixedName(std::string_view B, uint64_t At, unsigned Width,
                      std::string_view Name) {
  within({At, Width}, B.size());
  const auto Field = B.substr(At, Width);
  return Field.starts_with(Name) &&
         std::all_of(Field.begin() + Name.size(), Field.end(),
                     [](char C) { return !C; });
}
inline bool powerOfTwo(uint64_t N) { return N && !(N & (N - 1)); }
inline uint64_t align4(uint64_t N) {
  within({N, 3}, UINT64_MAX);
  return (N + 3) & ~uint64_t(3);
}
struct Reader {
  Blob Bytes;
  std::string read(Range R) const {
    within(R, Bytes.size());
    return Bytes.read(R.Offset, R.Size);
  }
  uint64_t integer(uint64_t At, unsigned Width) const {
    return number(read({At, Width}), 0, Width);
  }
};
struct Location {
  Range Resource;
  std::string Format, Architecture;
};
struct Mapping {
  Range File, Virtual;
};
/// A selected range must have exactly one consistent physical/virtual owner.
inline void requireMapping(Range File, Range Virtual,
                           const std::vector<Mapping> &Mappings) {
  unsigned Owners = 0;
  for (const auto &M : Mappings) {
    if (!overlaps(M.File, File) && !overlaps(M.Virtual, Virtual))
      continue;
    if (!contains(M.File, File) || !contains(M.Virtual, Virtual) ||
        File.Offset - M.File.Offset != Virtual.Offset - M.Virtual.Offset)
      throw Error("sea_ambiguous_mapping");
    ++Owners;
  }
  if (Owners != 1)
    throw Error("sea_ambiguous_mapping");
}
Location locateELF(const Reader &R);
Location locateMachO(const Reader &R);
Location locatePE(const Reader &R);
Location locate(const Reader &R, std::string_view Profile);
} // namespace neverd::web::sea_detail
