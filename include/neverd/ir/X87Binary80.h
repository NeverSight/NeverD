//===- X87Binary80.h - Raw x87 extended-precision values --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One raw encoding classifier for numerical semantic owners. Decoding retains
/// unsupported encodings; each instruction decides whether to admit them.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X87BINARY80_H
#define NEVERD_IR_X87BINARY80_H

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace neverd {

using X87Binary80Bytes = std::array<uint8_t, 10>;

struct X87Binary80 {
  uint64_t Significand = 0;
  uint16_t SignExponent = 0;

  unsigned exponent() const { return SignExponent & 0x7fffU; }
  bool negative() const { return (SignExponent & 0x8000U) != 0; }
  bool integerBit() const { return (Significand >> 63) != 0; }
  bool isZero() const { return exponent() == 0 && Significand == 0; }
  bool isSubnormal() const {
    return exponent() == 0 && Significand != 0 && !integerBit();
  }
  bool isInfinity() const {
    return exponent() == 0x7fffU && Significand == UINT64_C(0x8000000000000000);
  }
  bool isNaN() const {
    return exponent() == 0x7fffU && Significand != UINT64_C(0x8000000000000000);
  }
  bool isCanonical() const {
    return exponent() == 0 ? !integerBit() : integerBit();
  }

  X87Binary80Bytes bytes() const {
    X87Binary80Bytes Result{};
    for (unsigned I = 0; I != 8; ++I)
      Result[I] = Significand >> (I * 8);
    Result[8] = SignExponent;
    Result[9] = SignExponent >> 8;
    return Result;
  }
};

inline std::optional<X87Binary80>
decodeX87Binary80(std::span<const uint8_t> Bytes) {
  if (Bytes.size() != 10)
    return std::nullopt;
  X87Binary80 Result;
  for (unsigned I = 0; I != 8; ++I)
    Result.Significand |= uint64_t(Bytes[I]) << (I * 8);
  Result.SignExponent = uint16_t(Bytes[8]) | (uint16_t(Bytes[9]) << 8);
  return Result;
}

} // namespace neverd

#endif // NEVERD_IR_X87BINARY80_H
