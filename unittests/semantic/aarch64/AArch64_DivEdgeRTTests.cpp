//===- AArch64_DivEdgeRTTests.cpp - AArch64 SDIV/UDIV edge cases -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 integer division has defined (non-trapping) edge cases that x86 does
// not:
//   * division by zero        -> 0
//   * signed INT_MIN / -1      -> INT_MIN (no overflow trap)
// LLVM's sdiv/udiv treat these as poison, so a lifted `sdiv`/`udiv` must still
// reproduce the hardware result.  The divisor is a runtime inline-asm input so
// the value cannot be constant-folded.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64DivEdgeRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64DivEdgeRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // 64-bit sdiv by 0 -> 0.
  {"sdiv_by_zero",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"sdiv %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):);return r;}\n",
   {100, 0}, "DivEdge", 0},

  // 64-bit udiv by 0 -> 0.
  {"udiv_by_zero",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"udiv %0,%1,%2\":\"=r\"(r):\"r\"((unsigned long)a),\"r\"((unsigned long)b):);"
   "return (long)r;}\n",
   {100, 0}, "DivEdge", 0},

  // signed INT64_MIN / -1 -> INT64_MIN (no trap).
  {"sdiv_intmin_neg1",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"sdiv %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):);return r;}\n",
   {0x8000000000000000ULL, (uint64_t)(-1)}, "DivEdge", 0},

  // 32-bit sdiv by 0 -> 0.
  {"sdiv32_by_zero",
   "long f(long a,long b){int r;"
   "__asm__ volatile(\"sdiv %w0,%w1,%w2\":\"=r\"(r):\"r\"((int)a),\"r\"((int)b):);"
   "return (long)r;}\n",
   {12345, 0}, "DivEdge", 0},

  // 32-bit INT32_MIN / -1 -> INT32_MIN.
  {"sdiv32_intmin_neg1",
   "long f(long a,long b){int r;"
   "__asm__ volatile(\"sdiv %w0,%w1,%w2\":\"=r\"(r):\"r\"((int)a),\"r\"((int)b):);"
   "return (long)r;}\n",
   {0x80000000ULL, (uint64_t)(-1)}, "DivEdge", 0},

  // Control: normal division still correct.
  {"sdiv_normal",
   "long f(long a,long b){long r;"
   "__asm__ volatile(\"sdiv %0,%1,%2\":\"=r\"(r):\"r\"(a),\"r\"(b):);return r;}\n",
   {1000, 7}, "DivEdge", 0},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(DivEdge, AArch64DivEdgeRT, ::testing::ValuesIn(kA64),
                         rtTCName);
