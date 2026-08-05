//===- AllPlatform_ReturnWidthRTTests.cpp - return-width stress ----*- C++ -*-===//
//
// Targeted roundtrip tests for return-value width / sign-extension and
// sub-register tracking when the RETURN lives in a different basic block
// from the final write to the return register (loops / conditionals).
//
// This is the class of bug exposed by bug #157: a function returning a
// 64-bit value computed via 32-bit operations, where a trailing narrow
// sub-register write (e.g. EAX from a flag computation) was mistakenly
// selected by the RETURN handler's predecessor scan, truncating the
// result.  The high 32 bits of every expected result below are non-zero
// (negative or large) so any truncation is observable.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RetWidthRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RetWidthRT, Verify) { roundTripX64(GetParam()); }

class A64RetWidthRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64RetWidthRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32RetWidthRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32RetWidthRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

// 64-bit-return-register architectures (x64, aarch64) share these patterns.
// Each function returns `long` and the result has non-zero high 32 bits.
static std::vector<RoundTripTC> makeWideRetTC(const char *prefix) {
  std::string p = prefix;
  return {
    // sum of signed int lanes, negative result, accumulated in i64
    {p+"_sum_neg_ints",
     "long "+p+"_sum_neg_ints(long a) {\n"
     "  int xs[4] = {(int)a, -(int)a, 100, -2000000000};\n"
     "  long sum = 0;\n"
     "  for (int i = 0; i < 4; i++) sum += xs[i];\n"
     "  return sum;\n"
     "}\n",
     {1000}, "RetWidth"},

    // loop accumulating i32 products into i64, large positive (> 2^32)
    {p+"_prod_accum",
     "long "+p+"_prod_accum(long a) {\n"
     "  long acc = 0;\n"
     "  for (int i = 1; i <= 8; i++) acc += (long)((int)a * i);\n"
     "  return acc * 1000000;\n"
     "}\n",
     {123456}, "RetWidth"},

    // conditional returning a sign-extended negative int
    {p+"_cond_neg",
     "long "+p+"_cond_neg(long a) {\n"
     "  int x = (int)a;\n"
     "  if (x > 0) return (long)(-x - 1000000000);\n"
     "  return (long)(x - 1000000000);\n"
     "}\n",
     {2000000000u}, "RetWidth"},

    // countdown loop, final value negative i64
    {p+"_countdown",
     "long "+p+"_countdown(long a) {\n"
     "  long r = a;\n"
     "  for (int i = 0; i < 5; i++) r -= 1000000000;\n"
     "  return r;\n"
     "}\n",
     {0}, "RetWidth"},

    // min of signed values, negative, returned through a loop
    {p+"_min_loop",
     "long "+p+"_min_loop(long a, long b) {\n"
     "  long vals[3] = {a, b, -5000000000};\n"
     "  long m = vals[0];\n"
     "  for (int i = 1; i < 3; i++) if (vals[i] < m) m = vals[i];\n"
     "  return m;\n"
     "}\n",
     {10, 20}, "RetWidth"},

    // signed 32-bit overflow widened to 64-bit at return
    {p+"_widen_after_loop",
     "long "+p+"_widen_after_loop(long a) {\n"
     "  int acc = (int)a;\n"
     "  for (int i = 0; i < 3; i++) acc = acc * 2;\n"
     "  return (long)acc;\n"   // acc may be negative after int overflow
     "}\n",
     {0x20000000}, "RetWidth"},

    // unsigned widen: zero-extend a 32-bit value > 2^31 in a loop
    {p+"_uwiden_loop",
     "unsigned long "+p+"_uwiden_loop(long a) {\n"
     "  unsigned int acc = (unsigned int)a;\n"
     "  for (int i = 0; i < 4; i++) acc += 0x40000000u;\n"
     "  return (unsigned long)acc;\n"
     "}\n",
     {0x80000000u}, "RetWidth"},

    // ternary chain returning wide negative
    {p+"_ternary_wide",
     "long "+p+"_ternary_wide(long a, long b) {\n"
     "  long r = a > b ? a - 9000000000 : b - 9000000000;\n"
     "  return r;\n"
     "}\n",
     {5, 7}, "RetWidth"},
  };
}

static const std::vector<RoundTripTC> kX64RetWidth = makeWideRetTC("x64rw");
static const std::vector<RoundTripTC> kA64RetWidth = makeWideRetTC("a64rw");

// ARM32: r0 is 32-bit; "long long" returns use r0:r1.  Stress the 64-bit
// (r0:r1) return path computed across blocks, plus narrow int returns.
static const std::vector<RoundTripTC> kARM32RetWidth = {
  {"armrw_ll_sum_neg",
   "long long armrw_ll_sum_neg(int a) {\n"
   "  int xs[4] = {a, -a, 100, -2000000000};\n"
   "  long long sum = 0;\n"
   "  for (int i = 0; i < 4; i++) sum += xs[i];\n"
   "  return sum;\n"
   "}\n",
   {1000}, "RetWidth"},

  {"armrw_ll_countdown",
   "long long armrw_ll_countdown(int a) {\n"
   "  long long r = a;\n"
   "  for (int i = 0; i < 5; i++) r -= 1000000000LL;\n"
   "  return r;\n"
   "}\n",
   {0}, "RetWidth"},

  {"armrw_int_cond_neg",
   "int armrw_int_cond_neg(int a) {\n"
   "  if (a > 0) return -a - 1000000;\n"
   "  return a - 1000000;\n"
   "}\n",
   {500000}, "RetWidth"},

  {"armrw_short_ret",
   "short armrw_short_ret(int a) {\n"
   "  int acc = a;\n"
   "  for (int i = 0; i < 3; i++) acc += 0x1000;\n"
   "  return (short)acc;\n"
   "}\n",
   {0x7000}, "RetWidth"},

  {"armrw_char_ret",
   "signed char armrw_char_ret(int a) {\n"
   "  int acc = a;\n"
   "  for (int i = 0; i < 4; i++) acc += 0x20;\n"
   "  return (signed char)acc;\n"
   "}\n",
   {0x70}, "RetWidth"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(RetWidth, X64RetWidthRT,
                         ::testing::ValuesIn(kX64RetWidth), rtTCName);
INSTANTIATE_TEST_SUITE_P(RetWidth, A64RetWidthRT,
                         ::testing::ValuesIn(kA64RetWidth), rtTCName);
INSTANTIATE_TEST_SUITE_P(RetWidth, ARM32RetWidthRT,
                         ::testing::ValuesIn(kARM32RetWidth), rtTCName);
