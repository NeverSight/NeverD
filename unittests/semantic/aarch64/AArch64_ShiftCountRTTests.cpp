//===- AArch64_ShiftCountRTTests.cpp - variable shift count modulo --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Guardrail closing the cross-platform "shift/rotate count modulo" theme
// (#285 x86 BT, #286 ARM32 register shift, #287 x86 rotate).  AArch64's
// variable (register) shifts LSLV/LSRV/ASRV/RORV take the amount modulo the
// datasize: 32 for the W (32-bit) form, 64 for the X (64-bit) form.  The lifter
// already masks the amount (`& 63` / `& 31`) before the saturating shift op, so
// these probes confirm large counts round-trip correctly and lock that in
// against regressions.  Counts are runtime inline-asm inputs so they cannot be
// constant-folded, and each uses an amount >= the datasize.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64ShiftCountRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64ShiftCountRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // LSLV 64-bit, count 65 -> 65 MOD 64 = 1.
  {"lslv_x_65",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"lsl %0,%1,%2\":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b):);"
   "return (long)r;}\n",
   {0x0123456789ABCDEFULL, 65}, "ShiftCount", 0},

  // LSLV 32-bit, count 33 -> 33 MOD 32 = 1.
  {"lslv_w_33",
   "long f(long a,long b){unsigned int r;"
   "__asm__ volatile(\"lsl %w0,%w1,%w2\":\"=r\"(r):\"r\"((unsigned int)a),\"r\"((unsigned int)b):);"
   "return (long)r;}\n",
   {0x80000001ULL, 33}, "ShiftCount", 0},

  // LSRV 64-bit, count 66 -> 66 MOD 64 = 2.
  {"lsrv_x_66",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"lsr %0,%1,%2\":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b):);"
   "return (long)r;}\n",
   {0xFEDCBA9876543210ULL, 66}, "ShiftCount", 0},

  // ASRV 32-bit, count 40 -> 40 MOD 32 = 8, sign-extends a negative input.
  {"asrv_w_40_neg",
   "long f(long a,long b){int r;"
   "__asm__ volatile(\"asr %w0,%w1,%w2\":\"=r\"(r):\"r\"((int)a),\"r\"((int)b):);"
   "return (long)r;}\n",
   {0x80000000ULL, 40}, "ShiftCount", 0},

  // RORV 64-bit, count 70 -> 70 MOD 64 = 6.
  {"rorv_x_70",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"ror %0,%1,%2\":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b):);"
   "return (long)r;}\n",
   {0x0123456789ABCDEFULL, 70}, "ShiftCount", 0},

  // LSLV 64-bit, count 64 -> 64 MOD 64 = 0 (identity edge).
  {"lslv_x_64_identity",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"lsl %0,%1,%2\":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b):);"
   "return (long)r;}\n",
   {0x0123456789ABCDEFULL, 64}, "ShiftCount", 0},

  // Control: small count (< datasize) — must stay correct.
  {"lslv_x_5_small",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"lsl %0,%1,%2\":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b):);"
   "return (long)r;}\n",
   {0x0123456789ABCDEFULL, 5}, "ShiftCount", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(ShiftCount, AArch64ShiftCountRT,
                         ::testing::ValuesIn(kA64), rtTCName);
