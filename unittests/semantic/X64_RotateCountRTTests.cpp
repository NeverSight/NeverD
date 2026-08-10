//===- X64_RotateCountRTTests.cpp - 8/16-bit rotate count modulo -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// x86 ROL/ROR/RCL/RCR mask the count to 5 bits (0x1F) for 8/16/32-bit operands
// (6 bits / 0x3F for 64-bit), but for BYTE and WORD operands the hardware then
// takes a SECOND reduction the lifter was missing (Intel SDM):
//   * ROL/ROR : tempCOUNT = (COUNT & 0x1F) MOD operand_size   (MOD 8 / MOD 16)
//   * RCL/RCR : tempCOUNT = (COUNT & 0x1F) MOD (operand_size+1) (MOD 9 / MOD 17,
//               the carry flag adds one position to the rotation cycle)
//
// Without the second step, a byte rotate by e.g. 9 fed `x << 9` into NeverD's
// saturating INT_LEFT (over-shift -> 0), dropping the high half of the rotate
// and returning only `x >> 7` instead of ROL(x, 1).  32/64-bit are unaffected
// (the 5/6-bit mask already yields a count < operand size).
//
// Every existing rotate test uses 32/64-bit operands or a count < width, so the
// missing MOD was invisible (classic weak-test masking).  These probes pin the
// exact instruction with inline asm and use counts >= the operand width.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RotateCountRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RotateCountRT, Verify) { roundTripX64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // ===== ROL byte, count 9 -> (9 & 0x1F) MOD 8 = 1.  Buggy lifter over-shifts
  // x<<9 to 0 and returns only x>>7. =====
  {"rolb_9_mod8",
   "long f(long a){unsigned char x=(unsigned char)a;"
   "__asm__ volatile(\"rolb $9,%0\":\"+r\"(x)::\"cc\");"
   "return (unsigned long)x;}\n",
   {0xB3}, "RotateCount", 0},

  // ===== ROR byte, count 11 -> 11 MOD 8 = 3. =====
  {"rorb_11_mod8",
   "long f(long a){unsigned char x=(unsigned char)a;"
   "__asm__ volatile(\"rorb $11,%0\":\"+r\"(x)::\"cc\");"
   "return (unsigned long)x;}\n",
   {0xB3}, "RotateCount", 0},

  // ===== ROL word, count 17 -> 17 MOD 16 = 1. =====
  {"rolw_17_mod16",
   "long f(long a){unsigned short x=(unsigned short)a;"
   "__asm__ volatile(\"rolw $17,%0\":\"+r\"(x)::\"cc\");"
   "return (unsigned long)x;}\n",
   {0xB3C5}, "RotateCount", 0},

  // ===== ROR word, count 20 -> 20 MOD 16 = 4. =====
  {"rorw_20_mod16",
   "long f(long a){unsigned short x=(unsigned short)a;"
   "__asm__ volatile(\"rorw $20,%0\":\"+r\"(x)::\"cc\");"
   "return (unsigned long)x;}\n",
   {0xB3C5}, "RotateCount", 0},

  // ===== ROR byte by CL=13 -> 13 MOD 8 = 5 (register-count path). =====
  {"rorb_cl13_mod8",
   "long f(long a){unsigned char x=(unsigned char)a;"
   "__asm__ volatile(\"rorb %%cl,%0\":\"+r\"(x):\"c\"((unsigned char)13):\"cc\");"
   "return (unsigned long)x;}\n",
   {0xB3}, "RotateCount", 0},

  // ===== RCL byte through carry, count 10 -> 10 MOD 9 = 1 (CF cleared). =====
  {"rclb_10_mod9",
   "long f(long a){unsigned char x=(unsigned char)a;"
   "__asm__ volatile(\"clc\\n\\trclb $10,%0\":\"+r\"(x)::\"cc\");"
   "return (unsigned long)x;}\n",
   {0xB3}, "RotateCount", 0},

  // ===== RCR byte through carry, count 11 -> 11 MOD 9 = 2 (CF set). =====
  {"rcrb_11_mod9",
   "long f(long a){unsigned char x=(unsigned char)a;"
   "__asm__ volatile(\"stc\\n\\trcrb $11,%0\":\"+r\"(x)::\"cc\");"
   "return (unsigned long)x;}\n",
   {0xB3}, "RotateCount", 0},

  // ===== Control: ROL byte by 3 (< width, no second MOD) — must stay correct. =====
  {"rolb_3_small",
   "long f(long a){unsigned char x=(unsigned char)a;"
   "__asm__ volatile(\"rolb $3,%0\":\"+r\"(x)::\"cc\");"
   "return (unsigned long)x;}\n",
   {0xB3}, "RotateCount", 0},

  // ===== Control: ROL dword by 33 -> masks to 1 (32-bit already correct). =====
  {"roll_33_mask32",
   "long f(long a){unsigned int x=(unsigned int)a;"
   "__asm__ volatile(\"roll $33,%0\":\"+r\"(x)::\"cc\");"
   "return (unsigned long)x;}\n",
   {0x80000001ULL}, "RotateCount", 0},

  // ===== Control: ROL byte by 16 -> 16 MOD 8 = 0 (identity edge). =====
  {"rolb_16_identity",
   "long f(long a){unsigned char x=(unsigned char)a;"
   "__asm__ volatile(\"rolb $16,%0\":\"+r\"(x)::\"cc\");"
   "return (unsigned long)x;}\n",
   {0xB3}, "RotateCount", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(RotateCount, X64RotateCountRT,
                         ::testing::ValuesIn(kX64), rtTCName);
