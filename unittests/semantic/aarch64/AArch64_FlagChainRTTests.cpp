//===- AArch64_FlagChainRTTests.cpp - AArch64 flag chain RT ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests AArch64 complex flag-chain patterns: CCMP, CSEL, CSINC, CSINV, CSNEG,
// conditional compare sequences, and multi-condition branches.
// These exercise the optimizer's flag folding and condition-code tracking.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64FlagChainRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FlagChainRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kA64FlagChain = {
  // ========== Basic CSEL patterns ==========
  {"csel_eq",
   "long csel_eq(long a, long b) {\n"
   "  return a == 0 ? b : a;\n"
   "}\n",
   {0, 42}, "A64FlagChain"},

  {"csel_ne",
   "long csel_ne(long a, long b) {\n"
   "  return a != 0 ? a : b;\n"
   "}\n",
   {0, 42}, "A64FlagChain"},

  {"csel_lt",
   "long csel_lt(long a, long b) {\n"
   "  return a < b ? a : b;\n"
   "}\n",
   {10, 20}, "A64FlagChain"},

  {"csel_gt",
   "long csel_gt(long a, long b) {\n"
   "  return a > b ? a : b;\n"
   "}\n",
   {20, 10}, "A64FlagChain"},

  // ========== CSINC patterns ==========
  {"csinc_eq",
   "long csinc_eq(long a, long b) {\n"
   "  return a == 0 ? b + 1 : a;\n"
   "}\n",
   {0, 41}, "A64FlagChain"},

  {"csinc_ne_zero",
   "long csinc_ne_zero(long a) {\n"
   "  return a != 0 ? 1 : 0;\n"
   "}\n",
   {42}, "A64FlagChain"},

  {"csinc_ne_zero_false",
   "long csinc_ne_zero_false(long a) {\n"
   "  return a != 0 ? 1 : 0;\n"
   "}\n",
   {0}, "A64FlagChain"},

  // ========== CSINV patterns ==========
  {"csinv_eq",
   "long csinv_eq(long a, long b) {\n"
   "  return a == 0 ? ~b : a;\n"
   "}\n",
   {0, 0}, "A64FlagChain"},

  // ========== CSNEG patterns ==========
  {"csneg",
   "long csneg(long a, long b) {\n"
   "  return a > 0 ? a : -a;\n"
   "}\n",
   {(uint64_t)(int64_t)-42}, "A64FlagChain"},

  // ========== Compound conditions (CCMP) ==========
  {"ccmp_and",
   "long ccmp_and(long a, long b) {\n"
   "  return (a > 0 && b > 0) ? 1 : 0;\n"
   "}\n",
   {10, 20}, "A64FlagChain"},

  {"ccmp_and_false1",
   "long ccmp_and_false1(long a, long b) {\n"
   "  return (a > 0 && b > 0) ? 1 : 0;\n"
   "}\n",
   {(uint64_t)(int64_t)-1, 20}, "A64FlagChain"},

  {"ccmp_and_false2",
   "long ccmp_and_false2(long a, long b) {\n"
   "  return (a > 0 && b > 0) ? 1 : 0;\n"
   "}\n",
   {10, (uint64_t)(int64_t)-1}, "A64FlagChain"},

  {"ccmp_or",
   "long ccmp_or(long a, long b) {\n"
   "  return (a == 0 || b == 0) ? 1 : 0;\n"
   "}\n",
   {0, 42}, "A64FlagChain"},

  {"ccmp_or_false",
   "long ccmp_or_false(long a, long b) {\n"
   "  return (a == 0 || b == 0) ? 1 : 0;\n"
   "}\n",
   {1, 2}, "A64FlagChain"},

  // ========== Multi-branch chains ==========
  {"multi_branch_3way",
   "long multi_branch_3way(long a) {\n"
   "  if (a < 0) return -1;\n"
   "  if (a > 0) return 1;\n"
   "  return 0;\n"
   "}\n",
   {(uint64_t)(int64_t)-42}, "A64FlagChain"},

  {"multi_branch_3way_pos",
   "long multi_branch_3way_pos(long a) {\n"
   "  if (a < 0) return -1;\n"
   "  if (a > 0) return 1;\n"
   "  return 0;\n"
   "}\n",
   {42}, "A64FlagChain"},

  {"multi_branch_3way_zero",
   "long multi_branch_3way_zero(long a) {\n"
   "  if (a < 0) return -1;\n"
   "  if (a > 0) return 1;\n"
   "  return 0;\n"
   "}\n",
   {0}, "A64FlagChain"},

  // ========== Range check (compound condition) ==========
  {"range_check",
   "long range_check(long a) {\n"
   "  return (a >= 10 && a <= 100) ? 1 : 0;\n"
   "}\n",
   {50}, "A64FlagChain"},

  {"range_check_below",
   "long range_check_below(long a) {\n"
   "  return (a >= 10 && a <= 100) ? 1 : 0;\n"
   "}\n",
   {5}, "A64FlagChain"},

  {"range_check_above",
   "long range_check_above(long a) {\n"
   "  return (a >= 10 && a <= 100) ? 1 : 0;\n"
   "}\n",
   {200}, "A64FlagChain"},

  // ========== Unsigned comparison patterns ==========
  {"ucmp_hi_lo",
   "long ucmp_hi_lo(long a, long b) {\n"
   "  return (unsigned long)a > (unsigned long)b ? a : b;\n"
   "}\n",
   {0xFFFFFFFFFFFFFF00ULL, 0x100}, "A64FlagChain"},

  // ========== Division quotient + remainder ==========
  {"a64_divmod",
   "long a64_divmod(long a, long b) {\n"
   "  long q = a / b;\n"
   "  long r = a % b;\n"
   "  return q ^ r;\n"
   "}\n",
   {123456, 1000}, "A64FlagChain"},

  {"a64_udivmod",
   "long a64_udivmod(long a, long b) {\n"
   "  unsigned long q = (unsigned long)a / (unsigned long)b;\n"
   "  unsigned long r = (unsigned long)a % (unsigned long)b;\n"
   "  return (long)(q ^ r);\n"
   "}\n",
   {1000000, 7}, "A64FlagChain"},

  // ========== Absolute value patterns (CSNEG/CSEL) ==========
  {"abs_val",
   "long abs_val(long a) {\n"
   "  return a < 0 ? -a : a;\n"
   "}\n",
   {(uint64_t)(int64_t)-42}, "A64FlagChain"},

  {"abs_val_pos",
   "long abs_val_pos(long a) {\n"
   "  return a < 0 ? -a : a;\n"
   "}\n",
   {42}, "A64FlagChain"},

  // ========== Saturated add/sub (multi-condition) ==========
  {"sat_add_i64",
   "long sat_add_i64(long a, long b) {\n"
   "  long r = a + b;\n"
   "  if (a > 0 && b > 0 && r < 0) return 0x7FFFFFFFFFFFFFFFLL;\n"
   "  if (a < 0 && b < 0 && r > 0) return (long)0x8000000000000000ULL;\n"
   "  return r;\n"
   "}\n",
   {100, 200}, "A64FlagChain"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(A64FlagChain, A64FlagChainRT,
                         ::testing::ValuesIn(kA64FlagChain), rtTCName);
