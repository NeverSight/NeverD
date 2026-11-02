//===- AArch64FloatConversionCases.h - FP oracles --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent expected result/exception bits for architectural edge cases.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TEST_EMULATION_AARCH64_FLOAT_CONVERSION_CASES_H
#define NEVERD_TEST_EMULATION_AARCH64_FLOAT_CONVERSION_CASES_H

#include <cstdint>

namespace neverd::emulation::test {
struct AArch64FloatConversionCase {
  uint64_t Input;
  uint32_t Control;
  uint32_t Output;
  uint32_t Raised;
};
inline constexpr AArch64FloatConversionCase FloatNarrowingCases[] = {
    {0, 0, 0, 0},
    {0x8000000000000000, 0, 0x80000000, 0},
    {0x3ff0000000000000, 0x00c00000, 0x3f800000, 0},
    {0x3ff0000010000000, 0, 0x3f800000, 0x10},
    {0x3ff0000010000000, 0x00400000, 0x3f800001, 0x10},
    {0x3ff0000010000000, 0x00800000, 0x3f800000, 0x10},
    {0x3ff0000010000000, 0x00c00000, 0x3f800000, 0x10},
    {0xbff0000010000000, 0x00800000, 0xbf800001, 0x10},
    {0xbff0000010000000, 0x00400000, 0xbf800000, 0x10},
    {0x47effffff0000000, 0, 0x7f800000, 0x14},
    {0x47effffff0000000, 0x00c00000, 0x7f7fffff, 0x10},
    {0x7fefffffffffffff, 0x00c00000, 0x7f7fffff, 0x14},
    {0x7ff0000000000000, 0x03c00000, 0x7f800000, 0},
    {0xfff0000000000000, 0x03c00000, 0xff800000, 0},
    {0x3690000000000000, 0, 0, 0x18},
    {0x3690000000000000, 0x00400000, 1, 0x18},
    {0x36a0000000000000, 0, 1, 0},
    {0x36a0000000000000, 0x01000000, 0, 8},
    {0x380fffffffffffff, 0, 0x00800000, 0x18},
    {0x380fffffffffffff, 0x00800000, 0x007fffff, 0x18},
    {0x380fffffffffffff, 0x01400000, 0, 8},
    {0xb80fffffffffffff, 0x01800000, 0x80000000, 8},
    {0x3810000000000000, 0x01000000, 0x00800000, 0},
    {1, 0x00400000, 1, 0x18},
    {1, 0x01400000, 0, 0x80},
    {0x8000000000000001, 0x01800000, 0x80000000, 0x80},
    {0x7ff0000000000001, 0, 0x7fc00000, 1},
    {0x7ff8123456789abc, 0, 0x7fc091a2, 0},
    {0xfff0123456789abc, 0, 0xffc091a2, 1},
    {0xfff0123456789abc, 0x02000000, 0x7fc00000, 1},
    {0xfff8123456789abc, 0x02000000, 0x7fc00000, 0},
};
} // namespace neverd::emulation::test
#endif // NEVERD_TEST_EMULATION_AARCH64_FLOAT_CONVERSION_CASES_H
