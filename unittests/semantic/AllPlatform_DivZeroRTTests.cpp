//===- AllPlatform_DivZeroRTTests.cpp - AArch64/ARM32 div edge --*- C++ -*-===//
//
// AArch64 and ARM32 SDIV/UDIV do NOT trap: divide-by-zero yields 0 and the
// signed INT_MIN / -1 overflow wraps to INT_MIN.  LLVM's sdiv/udiv, however,
// make both cases undefined behaviour (poison), so a naive INT_SDIV/INT_DIV
// lift can be miscompiled by the optimizer.  These probes feed b=0 and the
// INT_MIN/-1 overflow at runtime so the roundtrip exposes any divergence; x86
// is excluded because its div-by-zero faults (#DE) instead of returning a value.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64DivZeroRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64DivZeroRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32DivZeroRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32DivZeroRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  {"sdiv_x_zero",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"sdiv %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b));return r;}\n",
   {100, 0}, "DivZero"},
  {"udiv_x_zero",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"udiv %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b));return (long)r;}\n",
   {100, 0}, "DivZero"},
  {"sdiv_w_zero",
   "long f(long a,long b){int r;"
   "__asm__ volatile(\"sdiv %w0,%w1,%w2\":\"=r\"(r):\"r\"((int)a),\"r\"((int)b));"
   "return (long)r;}\n",
   {100, 0}, "DivZero"},
  {"udiv_w_zero",
   "long f(long a,long b){unsigned r;"
   "__asm__ volatile(\"udiv %w0,%w1,%w2\":\"=r\"(r):\"r\"((unsigned)a),\"r\"((unsigned)b));"
   "return (long)r;}\n",
   {100, 0}, "DivZero"},
  // INT64_MIN / -1 wraps to INT64_MIN (no overflow trap).
  {"sdiv_x_ovf",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"sdiv %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b));return r;}\n",
   {0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL}, "DivZero"},
  // INT32_MIN / -1 wraps to INT32_MIN.
  {"sdiv_w_ovf",
   "long f(long a,long b){int r;"
   "__asm__ volatile(\"sdiv %w0,%w1,%w2\":\"=r\"(r):\"r\"((int)a),\"r\"((int)b));"
   "return (long)r;}\n",
   {0x80000000ULL, 0xFFFFFFFFULL}, "DivZero"},
  // Control: normal division still correct.
  {"sdiv_x_ctrl",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"sdiv %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b));return r;}\n",
   {100, 7}, "DivZero"},
  // Divisor is a compile-time-visible 0: the optimizer can prove the divisor,
  // so a naive `sdiv %a, 0` would be poison/UB-exploited.  Must still yield 0.
  {"sdiv_const_zero",
   "long f(long a){long z=0,r;"
   "__asm__ volatile(\"sdiv %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(z));return r;}\n",
   {100}, "DivZero", 1},
  {"udiv_const_zero",
   "long f(long a){unsigned long z=0,r;"
   "__asm__ volatile(\"udiv %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(z));return (long)r;}\n",
   {100}, "DivZero", 1},
};

static const std::vector<RoundTripTC> kARM32 = {
  {"sdiv_zero",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"sdiv %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b));return r;}\n",
   {100, 0}, "DivZero"},
  {"udiv_zero",
   "int f(int a,int b){unsigned r;"
   "__asm__ volatile(\"udiv %0,%1,%2\":\"=r\"(r):\"r\"((unsigned)a),\"r\"((unsigned)b));"
   "return (int)r;}\n",
   {100, 0}, "DivZero"},
  {"sdiv_ovf",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"sdiv %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b));return r;}\n",
   {0x80000000ULL, 0xFFFFFFFFULL}, "DivZero"},
  {"sdiv_ctrl",
   "int f(int a,int b){int r;"
   "__asm__ volatile(\"sdiv %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b));return r;}\n",
   {100, 7}, "DivZero"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(DivZero, A64DivZeroRT, ::testing::ValuesIn(kA64),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(DivZero, ARM32DivZeroRT, ::testing::ValuesIn(kARM32),
                         rtTCName);
