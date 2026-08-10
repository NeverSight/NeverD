//===- AllPlatform_EdgeCaseTests.cpp - Edge case tests --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests numeric edge cases across all three platforms: overflow, underflow,
// max/min values, zero, sign bit, and boundary conditions.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

// --- x86_64 ---
class X64EdgeRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64EdgeRT, Verify) { roundTripX64(GetParam()); }

// --- AArch64 ---
class A64EdgeRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64EdgeRT, Verify) { roundTripAArch64(GetParam()); }

// --- ARM32 ---
class ARM32EdgeRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32EdgeRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

// ============================================================
// x86_64 edge cases
// ============================================================
static const std::vector<RoundTripTC> kX64Edge = {
  {"x64_add_max", "long x64_add_max(long a) { return a + 1; }\n", {0x7FFFFFFFFFFFFFFFULL}, "Edge"},
  {"x64_sub_min", "long x64_sub_min(long a) { return a - 1; }\n", {0x8000000000000000ULL}, "Edge"},
  {"x64_neg_min", "long x64_neg_min(long a) { return -a; }\n", {0x8000000000000000ULL}, "Edge"},
  {"x64_not_zero", "long x64_not_zero(long a) { return ~a; }\n", {0}, "Edge"},
  {"x64_xor_self", "long x64_xor_self(long a) { return a ^ a; }\n", {0xDEADBEEFULL}, "Edge"},
  {"x64_and_zero", "long x64_and_zero(long a, long b) { return a & 0; }\n", {0xFFFFFFFFFFFFFFFFULL, 0}, "Edge"},
  {"x64_or_all_ones", "long x64_or_all(long a) { return a | 0xFFFFFFFFFFFFFFFFULL; }\n", {42}, "Edge"},
  {"x64_mul_zero", "long x64_mul0(long a) { return a * 0; }\n", {42}, "Edge"},
  {"x64_mul_one", "long x64_mul1(long a) { return a * 1; }\n", {42}, "Edge"},
  {"x64_mul_neg1", "long x64_mul_neg1(long a) { return a * (0xFFFFFFFFFFFFFFFFULL); }\n", {42}, "Edge"},
  {"x64_shl_0", "long x64_shl0(long a) { return a << 0; }\n", {42}, "Edge"},
  {"x64_add_0", "long x64_add0(long a) { return a + 0; }\n", {42}, "Edge"},
  {"x64_sub_0", "long x64_sub0(long a) { return a - 0; }\n", {42}, "Edge"},
  {"x64_div_1", "long x64_div1(long a) { return a / 1; }\n", {42}, "Edge"},
  {"x64_mod_1", "long x64_mod1(long a) { return a % 1; }\n", {42}, "Edge"},
  {"x64_eq_max", "long x64_eq_max(long a) { return a == 0x7FFFFFFFFFFFFFFFLL; }\n", {0x7FFFFFFFFFFFFFFFULL}, "Edge"},
  {"x64_lt_max", "long x64_lt_max(long a) { return a < 0x7FFFFFFFFFFFFFFFLL; }\n", {0x7FFFFFFFFFFFFFFEULL}, "Edge"},
  {"x64_abs_max_neg",
   "long x64_abs_max_neg(long a) {\n"
   "  return a == (long)0x8000000000000000ULL ? 0x7FFFFFFFFFFFFFFFLL : (a < 0 ? -a : a);\n"
   "}\n",
   {0x8000000000000000ULL}, "Edge"},
};

// ============================================================
// AArch64 edge cases
// ============================================================
static const std::vector<RoundTripTC> kA64Edge = {
  {"a64_add_max", "long a64_add_max(long a) { return a + 1; }\n", {0x7FFFFFFFFFFFFFFFULL}, "Edge"},
  {"a64_sub_min", "long a64_sub_min(long a) { return a - 1; }\n", {0x8000000000000000ULL}, "Edge"},
  {"a64_neg_min", "long a64_neg_min(long a) { return -a; }\n", {0x8000000000000000ULL}, "Edge"},
  {"a64_not_zero", "long a64_not_zero(long a) { return ~a; }\n", {0}, "Edge"},
  {"a64_xor_self", "long a64_xor_self(long a) { return a ^ a; }\n", {0xDEADBEEFULL}, "Edge"},
  {"a64_mul_zero", "long a64_mul0(long a) { return a * 0; }\n", {42}, "Edge"},
  {"a64_mul_one", "long a64_mul1(long a) { return a * 1; }\n", {42}, "Edge"},
  {"a64_add_0", "long a64_add0(long a) { return a + 0; }\n", {42}, "Edge"},
  {"a64_div_1", "long a64_div1(long a) { return a / 1; }\n", {42}, "Edge"},
  {"a64_mod_1", "long a64_mod1(long a) { return a % 1; }\n", {42}, "Edge"},
  {"a64_eq_max", "long a64_eq_max(long a) { return a == 0x7FFFFFFFFFFFFFFFLL; }\n", {0x7FFFFFFFFFFFFFFFULL}, "Edge"},
};

// ============================================================
// ARM32 edge cases
// ============================================================
static const std::vector<RoundTripTC> kARM32Edge = {
  {"arm_add_max", "int arm_add_max(int a) { return a + 1; }\n", {0x7FFFFFFF}, "Edge"},
  {"arm_sub_min", "int arm_sub_min(int a) { return a - 1; }\n", {0x80000000}, "Edge"},
  {"arm_neg_min", "int arm_neg_min(int a) { return -a; }\n", {0x80000000}, "Edge"},
  {"arm_not_zero", "int arm_not_zero(int a) { return ~a; }\n", {0}, "Edge"},
  {"arm_xor_self", "int arm_xor_self(int a) { return a ^ a; }\n", {0xDEADBEEF}, "Edge"},
  {"arm_mul_zero", "int arm_mul0(int a) { return a * 0; }\n", {42}, "Edge"},
  {"arm_mul_one", "int arm_mul1(int a) { return a * 1; }\n", {42}, "Edge"},
  {"arm_add_0", "int arm_add0(int a) { return a + 0; }\n", {42}, "Edge"},
  {"arm_div_1", "int arm_div1(int a) { return a / 1; }\n", {42}, "Edge"},
  {"arm_mod_1", "int arm_mod1(int a) { return a % 1; }\n", {42}, "Edge"},
  {"arm_eq_self", "int arm_eq_self(int a) { return a == a; }\n", {42}, "Edge"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(Edge, X64EdgeRT, ::testing::ValuesIn(kX64Edge), rtTCName);
INSTANTIATE_TEST_SUITE_P(Edge, A64EdgeRT, ::testing::ValuesIn(kA64Edge), rtTCName);
INSTANTIATE_TEST_SUITE_P(Edge, ARM32EdgeRT, ::testing::ValuesIn(kARM32Edge), rtTCName);
