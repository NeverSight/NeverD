//===- AArch64_NativeDecodeParityLoadStoreTests.cpp - load/store parity -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "AArch64_NativeDecodeParityTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::a64_parity_test;

// Load/store register (unsigned immediate offset): all size x opc combinations
// (covering the byte/half/word/dword widths, the sign-extending loads, and the
// declined PRFM/UNALLOCATED slots), crossed with base/transfer register and
// scaled-displacement samples.  V==1 (FP/SIMD) is not generated here since it
// must be declined; the random sweep exercises that path.
TEST_F(A64NativeParity, LoadStoreUImmSweep) {
  const va_t A = 0x700000;
  const uint32_t Imm12Samples[] = {0u, 1u, 2u, 8u, 0x555u, 0xFFFu};
  for (uint32_t Size = 0; Size < 4; ++Size)
    for (uint32_t Opc = 0; Opc < 4; ++Opc)
      for (uint32_t Rt : {0u, 1u, 30u, 31u})
        for (uint32_t Rn : {0u, 9u, 31u})
          for (uint32_t Imm : Imm12Samples) {
            uint32_t W = (Size << 30) | 0x39000000u | (Opc << 22) |
                         ((Imm & 0xFFF) << 10) | (Rn << 5) | Rt;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
}
// Load/store pair: opc (32/64/declined) x mode (post/offset/pre/declined) x
// load-store x register x signed imm7 samples.
TEST_F(A64NativeParity, LoadStorePairSweep) {
  const va_t A = 0x900000;
  const uint32_t Imm7[] = {0u, 1u, 2u, 0x3Fu, 0x40u, 0x7Eu, 0x7Fu};
  for (uint32_t Opc = 0; Opc < 4; ++Opc)
    for (uint32_t Mode = 0; Mode < 4; ++Mode) // 0=STNP(declined),1,2,3
      for (uint32_t L = 0; L < 2; ++L)
        for (uint32_t Rt : {0u, 1u, 31u})
          for (uint32_t Rt2 : {2u, 30u})
            for (uint32_t Rn : {0u, 31u})
              for (uint32_t Imm : Imm7) {
                uint32_t W = (Opc << 30) | (0x5u << 27) | (Mode << 23) |
                             (L << 22) | ((Imm & 0x7F) << 15) | (Rt2 << 10) |
                             (Rn << 5) | Rt;
                std::string D = checkOne(O, W, A);
                EXPECT_TRUE(D.empty()) << D;
              }
}
// FP/SIMD load/store (unsigned immediate offset): size x opc (covering the
// B/H/S/D scalar widths, the Q form, load/store, and the declined size!=0 Q
// slots) x transfer register x displacement.
TEST_F(A64NativeParity, FpLoadStoreSweep) {
  const va_t A = 0xD00000;
  const uint32_t Imm12Samples[] = {0u, 1u, 2u, 8u, 0xFFFu};
  for (uint32_t Size = 0; Size < 4; ++Size)
    for (uint32_t Opc = 0; Opc < 4; ++Opc)
      for (uint32_t Rt : {0u, 1u, 31u})
        for (uint32_t Rn : {0u, 9u, 31u})
          for (uint32_t Imm : Imm12Samples) {
            uint32_t W = (Size << 30) | 0x3D000000u | (Opc << 22) |
                         ((Imm & 0xFFF) << 10) | (Rn << 5) | Rt;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
}
// FP/SIMD load/store register, non-unsigned-offset forms: register-offset,
// unscaled (LDUR/STUR), and pre/post-index, over size x opc (B/H/S/D/Q widths,
// the Q-with-size!=0 declines) x sub-form.
TEST_F(A64NativeParity, FpLoadStoreMiscSweep) {
  const va_t A = 0xE80000;
  // Register offset (bit21==1, [11:10]==10): sweep size, opc, option, S.
  for (uint32_t Size = 0; Size < 4; ++Size)
    for (uint32_t Opc = 0; Opc < 4; ++Opc)
      for (uint32_t Option = 0; Option < 8; ++Option)
        for (uint32_t S = 0; S < 2; ++S)
          for (uint32_t Rt : {0u, 31u}) {
            uint32_t W = (Size << 30) | (0x1Cu << 24) | (Opc << 22) |
                         (1u << 21) | (2u << 16) | (Option << 13) | (S << 12) |
                         (2u << 10) | (1u << 5) | Rt;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
  // Unscaled / pre / post (bit21==0, [11:10] in 00/01/11): sweep size, opc,
  // sub-form, imm9 samples.
  for (uint32_t Size = 0; Size < 4; ++Size)
    for (uint32_t Opc = 0; Opc < 4; ++Opc)
      for (uint32_t Op4 : {0u, 1u, 2u, 3u})
        for (uint32_t Imm9 : {0u, 1u, 0xFFu, 0x100u, 0x1FFu})
          for (uint32_t Rt : {0u, 31u}) {
            uint32_t W = (Size << 30) | (0x1Cu << 24) | (Opc << 22) |
                         ((Imm9 & 0x1FF) << 12) | (Op4 << 10) | (1u << 5) | Rt;
            std::string D = checkOne(O, W, A);
            EXPECT_TRUE(D.empty()) << D;
          }
}

// FP/SIMD load/store pair (S/D/Q) over opc x mode x load-store x imm7 samples.
TEST_F(A64NativeParity, FpLoadStorePairSweep) {
  const va_t A = 0x980000;
  const uint32_t Imm7[] = {0u, 1u, 2u, 0x3Fu, 0x40u, 0x7Eu, 0x7Fu};
  for (uint32_t Opc = 0; Opc < 4; ++Opc)         // 11 declined
    for (uint32_t Mode = 0; Mode < 4; ++Mode)    // 0=STNP/LDNP declined
      for (uint32_t L = 0; L < 2; ++L)
        for (uint32_t Rt : {0u, 31u})
          for (uint32_t Rt2 : {2u, 30u})
            for (uint32_t Rn : {0u, 31u})
              for (uint32_t Imm : Imm7) {
                uint32_t W = (Opc << 30) | (0x5u << 27) | (1u << 26) |
                             (Mode << 23) | (L << 22) | ((Imm & 0x7F) << 15) |
                             (Rt2 << 10) | (Rn << 5) | Rt;
                std::string D = checkOne(O, W, A);
                EXPECT_TRUE(D.empty()) << D;
              }
}

} // namespace
