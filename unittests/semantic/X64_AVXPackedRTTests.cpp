//===- X64_AVXPackedRTTests.cpp - AVX packed FP/int roundtrip tests -------===//
//
// Tests AVX packed arithmetic, logic, and conversion using C expressions
// that naturally compile to AVX instructions. All values are computed
// from function parameters to avoid rodata constant pools.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64AVXPackedRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64AVXPackedRT, Verify) { roundTripX64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kAVXPacked = {

  {"avx_int_add_chain",
   "long avx_int_add_chain(long a, long b) {\n"
   "  long r = a + b;\n"
   "  r += (a ^ b);\n"
   "  r += (a & b);\n"
   "  r += (a | b);\n"
   "  return r;\n"
   "}\n",
   {0x1234, 0x5678}, "AVXPacked", 2, "-mavx"},

  {"avx_fp_simple_add",
   "long avx_fp_simple_add(long a, long b) {\n"
   "  float fa = (float)(int)a, fb = (float)(int)b;\n"
   "  float r = fa + fb;\n"
   "  return (long)(int)r;\n"
   "}\n",
   {15, 7}, "AVXPacked", 2, ""},

  {"avx_fp_simple_mul",
   "long avx_fp_simple_mul(long a, long b) {\n"
   "  float fa = (float)(int)a, fb = (float)(int)b;\n"
   "  float r = fa * fb;\n"
   "  return (long)(int)r;\n"
   "}\n",
   {6, 7}, "AVXPacked", 2, ""},

  {"avx_int_mul_shift",
   "long avx_int_mul_shift(long a, long b) {\n"
   "  long r = (a * b) + ((a << 3) | (b >> 2));\n"
   "  return r;\n"
   "}\n",
   {13, 17}, "AVXPacked", 2, "-mavx"},

  {"avx_conditional",
   "long avx_conditional(long a, long b) {\n"
   "  long max = a > b ? a : b;\n"
   "  long min = a < b ? a : b;\n"
   "  return max + min;\n"
   "}\n",
   {42, 37}, "AVXPacked", 2, "-mavx"},

  {"avx_fp_compare",
   "long avx_fp_compare(long a, long b) {\n"
   "  double da = (double)a, db = (double)b;\n"
   "  return (long)(da > db) + (long)(da == db) + (long)(da < db);\n"
   "}\n",
   {10, 20}, "AVXPacked", 2, "-mavx"},

  {"avx_bitwise_combo",
   "long avx_bitwise_combo(long a, long b) {\n"
   "  return (a & 0xFF) | ((b & 0xFF00) >> 4) ^ ((a + b) & 0xFFFF);\n"
   "}\n",
   {0xABCD, 0x1234}, "AVXPacked", 2, "-mavx"},

  {"avx_abs_diff",
   "long avx_abs_diff(long a, long b) {\n"
   "  long d = a - b;\n"
   "  return d < 0 ? -d : d;\n"
   "}\n",
   {15, 42}, "AVXPacked", 2, "-mavx"},

};

// clang-format on

INSTANTIATE_TEST_SUITE_P(AVXPacked, X64AVXPackedRT,
    ::testing::ValuesIn(kAVXPacked),
    [](const auto &I) { return I.param.Name; });
