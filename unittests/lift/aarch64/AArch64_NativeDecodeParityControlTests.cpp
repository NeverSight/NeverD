//===- AArch64_NativeDecodeParityControlTests.cpp - control flow and condition parity -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "AArch64_NativeDecodeParityTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::a64_parity_test;

// Control flow: B/BL immediate range, B.cond over every condition (+ the BC.cond
// decline), CBZ/CBNZ, TBZ/TBNZ over the bit-position range, and RET/BR/BLR over
// the register field.
TEST_F(A64NativeParity, ControlFlowSweep) {
  const va_t A = 0x800000;
  const uint32_t Imm26[] = {0u, 1u, 0x21u, 0x1FFFFFFu, 0x2000000u, 0x3FFFFFFu};
  for (uint32_t Imm : Imm26) {
    EXPECT_TRUE(checkOne(O, 0x14000000u | (Imm & 0x3FFFFFF), A).empty());
    EXPECT_TRUE(checkOne(O, 0x94000000u | (Imm & 0x3FFFFFF), A).empty());
  }
  // B.cond / BC.cond across cond (bits[3:0]) and o0 (bit4).
  for (uint32_t Cond = 0; Cond < 16; ++Cond)
    for (uint32_t O0 = 0; O0 < 2; ++O0)
      for (uint32_t Imm19 : {0u, 1u, 0x3FFFFu, 0x7FFFFu}) {
        uint32_t W = 0x54000000u | ((Imm19 & 0x7FFFF) << 5) | (O0 << 4) | Cond;
        EXPECT_TRUE(checkOne(O, W, A).empty());
      }
  // CBZ/CBNZ (sf, op) x register x imm19 samples.
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t Rt : {0u, 1u, 31u})
        for (uint32_t Imm19 : {0u, 1u, 0x7FFFFu}) {
          uint32_t W = (Sf << 31) | 0x34000000u | (Op << 24) |
                       ((Imm19 & 0x7FFFF) << 5) | Rt;
          EXPECT_TRUE(checkOne(O, W, A).empty());
        }
  // TBZ/TBNZ (b5, op) x bit-low x register x imm14 samples.
  for (uint32_t B5 = 0; B5 < 2; ++B5)
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t B40 = 0; B40 < 32; B40 += 7)
        for (uint32_t Rt : {0u, 31u})
          for (uint32_t Imm14 : {0u, 1u, 0x3FFFu}) {
            uint32_t W = (B5 << 31) | 0x36000000u | (Op << 24) | (B40 << 19) |
                         ((Imm14 & 0x3FFF) << 5) | Rt;
            EXPECT_TRUE(checkOne(O, W, A).empty());
          }
  // RET/BR/BLR over Rn.
  for (uint32_t Rn = 0; Rn < 32; ++Rn) {
    EXPECT_TRUE(checkOne(O, 0xD65F0000u | (Rn << 5), A).empty());
    EXPECT_TRUE(checkOne(O, 0xD61F0000u | (Rn << 5), A).empty());
    EXPECT_TRUE(checkOne(O, 0xD63F0000u | (Rn << 5), A).empty());
  }
}
// Conditional compare (CCMP/CCMN, register and immediate) over sf/op/o2 x cond
// x Rn/Rm/imm5 x nzcv, plus the reserved o3/o1 bits (must be declined).
TEST_F(A64NativeParity, CondCompareSweep) {
  const va_t A = 0xC80000;
  for (uint32_t Sf = 0; Sf < 2; ++Sf)
    for (uint32_t Op = 0; Op < 2; ++Op)
      for (uint32_t O2 = 0; O2 < 2; ++O2)     // reg / imm form
        for (uint32_t O3 = 0; O3 < 2; ++O3)   // reserved bit10
          for (uint32_t O1 = 0; O1 < 2; ++O1) // reserved bit4
            for (uint32_t Cond = 0; Cond < 16; ++Cond)
              for (uint32_t RmImm : {0u, 5u, 31u})
                for (uint32_t Rn : {0u, 31u})
                  for (uint32_t Nzcv : {0u, 0xFu}) {
                    uint32_t W = (Sf << 31) | (Op << 30) | (1u << 29) |
                                 (0xD2u << 21) | (RmImm << 16) | (Cond << 12) |
                                 (O2 << 11) | (O3 << 10) | (Rn << 5) | (O1 << 4) |
                                 Nzcv;
                    std::string D = checkOne(O, W, A);
                    EXPECT_TRUE(D.empty()) << D;
                  }
}

// BRK #imm16 over a range of immediates (id-only lift, but the strict-subset
// and single-operand detail are still locked).
TEST_F(A64NativeParity, BrkSweep) {
  const va_t A = 0xD80000;
  for (uint32_t Imm : {0u, 1u, 0x100u, 0x800u, 0xDEADu, 0xFFFFu}) {
    uint32_t W = 0xD4200000u | (Imm << 5);
    std::string D = checkOne(O, W, A);
    EXPECT_TRUE(D.empty()) << D;
  }
}

} // namespace
