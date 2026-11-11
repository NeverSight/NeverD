//===- SymMBAProductColumn.cpp - Exact parity of masked products ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "SymMBAProductColumn.h"

#include "Limits.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>

namespace neverd::symbolic::detail {
namespace {

// Both multiplicands lose at least one bit. One native word therefore holds
// an entire truth row at byte precision. Tile shorter rows through the word
// so rotating by an offset always wraps at the right power of two.
constexpr unsigned MaxBits = limits::kMaxAffineResidueTableBits - 2;
static_assert((1u << MaxBits) <= 64);
constexpr auto ProductRows = [] {
  std::array<std::array<uint64_t, 1u << MaxBits>, MaxBits + 1> Rows{};
  for (unsigned Bits = 1; Bits <= MaxBits; ++Bits)
    for (unsigned X = 0; X < (1u << Bits); ++X)
      for (unsigned Y = 0; Y < 64; ++Y)
        Rows[Bits][X] |= uint64_t(((X * Y) >> (Bits - 1)) & 1) << Y;
  return Rows;
}();

constexpr auto BitPlanes = [] {
  std::array<uint64_t, MaxBits> Planes{};
  for (unsigned I = 0; I < MaxBits; ++I)
    for (unsigned Y = 0; Y < 64; ++Y)
      Planes[I] |= uint64_t((Y >> I) & 1) << Y;
  return Planes;
}();

} // namespace

std::optional<AffineProductColumn>
readAffineProductColumn(unsigned Count, unsigned A, unsigned B,
                        llvm::ArrayRef<std::pair<unsigned, unsigned>> Offsets,
                        llvm::function_ref<bool(size_t)> Charge) {
  assert(A && B && A + B < Count &&
         Count <= limits::kMaxAffineResidueTableBits);
  if (!Charge(Offsets.size()))
    return std::nullopt;
  unsigned DX = 0, DY = 0;
  for (auto [X, Y] : Offsets) {
    DX |= X;
    DY |= Y;
  }

  // Fractional input bits below every offset's alignment cannot carry into
  // a multiplicand. Enumerate the remaining low Y bits explicitly and read
  // all high Y values together in the row. X needs only Count-B bits.
  const unsigned FixedX = std::min(A, unsigned(std::countr_zero(DX)));
  const unsigned FixedY = std::min(B, unsigned(std::countr_zero(DY)));
  const unsigned LowY = B - FixedY;
  const unsigned Bits = Count - A - B;
  const unsigned Mask = (1u << Bits) - 1;
  const unsigned NumRows = 1u << (Count - B - FixedX + LowY);
  AffineProductColumn Result;
  uint64_t Origin = 0;
  unsigned RowSlopes = 0;
  for (unsigned I = 0; I < NumRows; ++I) {
    if (!Charge(1 + Offsets.size()))
      return std::nullopt;
    const unsigned X = (I >> LowY) << FixedX;
    const unsigned Y = (I & ((1u << LowY) - 1)) << FixedY;
    uint64_t Row = 0;
    for (auto [XO, YO] : Offsets) {
      const auto Product = ProductRows[Bits][((X + XO) >> A) & Mask];
      Row ^= std::rotr(Product, int(((Y + YO) >> B) & Mask));
    }

    if (!I) {
      Origin = Row;
      Result.Constant = (Row & 1) != 0;
      uint64_t Expected = Result.Constant ? ~uint64_t(0) : 0;
      for (unsigned J = 0; J < Bits; ++J)
        if (((Row >> (1u << J)) ^ Row) & 1) {
          Result.YBits |= 1u << (B + J);
          Expected ^= BitPlanes[J];
        }
      if (Expected != Row)
        return std::nullopt;
    } else {
      // A unit row precedes every non-unit row which uses that coordinate.
      if (std::has_single_bit(I) && ((Row ^ Origin) & 1))
        RowSlopes |= I;
      if (Row != ((std::popcount(RowSlopes & I) & 1) ? ~Origin : Origin))
        return std::nullopt;
    }
  }
  Result.XBits = (RowSlopes >> LowY) << FixedX;
  Result.YBits |= (RowSlopes & ((1u << LowY) - 1)) << FixedY;
  return Result;
}

} // namespace neverd::symbolic::detail
