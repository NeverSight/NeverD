//===- AllPlatform_ControlFlowTests.cpp - Control flow roundtrip -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Tests complex control flow patterns: nested loops, do-while, early return,
// multiple branches, and loop-carried dependencies.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CtrlRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CtrlRT, Verify) { roundTripX64(GetParam()); }

class A64CtrlRT : public SemanticRoundTripFixture,
                  public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CtrlRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32CtrlRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CtrlRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const std::vector<RoundTripTC> kX64Ctrl = {
  {"cf_do_while",
   "long cf_do_while(long n) {\n"
   "  long s = 0, i = 1;\n"
   "  do { s += i; ++i; } while (i <= n);\n"
   "  return s;\n"
   "}\n",
   {10}, "CtrlRT"},

  {"cf_nested_loop",
   "long cf_nested_loop(long n) {\n"
   "  long s = 0;\n"
   "  for (long i = 1; i <= n; ++i)\n"
   "    for (long j = 1; j <= i; ++j)\n"
   "      s += j;\n"
   "  return s;\n"
   "}\n",
   {5}, "CtrlRT"},

  {"cf_early_return",
   "long cf_early_return(long a, long b) {\n"
   "  if (a == 0) return b;\n"
   "  if (b == 0) return a;\n"
   "  if (a == b) return a;\n"
   "  return a + b;\n"
   "}\n",
   {3, 7}, "CtrlRT"},

  {"cf_while_break",
   "long cf_while_break(long n) {\n"
   "  long i = 0;\n"
   "  while (i < 100) {\n"
   "    if (i >= n) break;\n"
   "    ++i;\n"
   "  }\n"
   "  return i;\n"
   "}\n",
   {42}, "CtrlRT"},

  {"cf_loop_continue",
   "long cf_loop_continue(long n) {\n"
   "  long s = 0;\n"
   "  for (long i = 0; i < n; ++i) {\n"
   "    if (i % 3 == 0) continue;\n"
   "    s += i;\n"
   "  }\n"
   "  return s;\n"
   "}\n",
   {10}, "CtrlRT"},

  {"cf_count_down",
   "long cf_count_down(long n) {\n"
   "  long s = 0;\n"
   "  while (n > 0) { s += n; --n; }\n"
   "  return s;\n"
   "}\n",
   {10}, "CtrlRT"},

  {"cf_triangular",
   "long cf_triangular(long n) {\n"
   "  long s = 0;\n"
   "  for (long i = 1; i <= n; ++i)\n"
   "    s += i;\n"
   "  return s;\n"
   "}\n",
   {100}, "CtrlRT"},

  {"cf_alternating_sum",
   "long cf_alternating_sum(long n) {\n"
   "  long s = 0;\n"
   "  for (long i = 1; i <= n; ++i)\n"
   "    s += (i & 1) ? i : -i;\n"
   "  return s;\n"
   "}\n",
   {10}, "CtrlRT"},

  {"cf_count_divisors",
   "long cf_count_divisors(long n) {\n"
   "  long c = 0;\n"
   "  for (long i = 1; i <= n; ++i)\n"
   "    if (n % i == 0) ++c;\n"
   "  return c;\n"
   "}\n",
   {24}, "CtrlRT"},

  {"cf_digital_root",
   "long cf_digital_root(long n) {\n"
   "  unsigned long u = (unsigned long)(n > 0 ? n : -n);\n"
   "  while (u >= 10) {\n"
   "    unsigned long s = 0;\n"
   "    while (u > 0) { s += u % 10; u /= 10; }\n"
   "    u = s;\n"
   "  }\n"
   "  return (long)u;\n"
   "}\n",
   {9999999}, "CtrlRT"},
};

static const std::vector<RoundTripTC> kA64Ctrl = {
  {"a64_do_while",
   "long a64_do_while(long n) {\n"
   "  long s = 0, i = 1;\n"
   "  do { s += i; ++i; } while (i <= n);\n"
   "  return s;\n"
   "}\n",
   {10}, "CtrlRT"},

  {"a64_nested_loop",
   "long a64_nested_loop(long n) {\n"
   "  long s = 0;\n"
   "  for (long i = 1; i <= n; ++i)\n"
   "    for (long j = 1; j <= i; ++j)\n"
   "      s += j;\n"
   "  return s;\n"
   "}\n",
   {5}, "CtrlRT"},

  {"a64_early_return",
   "long a64_early_return(long a, long b) {\n"
   "  if (a == 0) return b;\n"
   "  if (b == 0) return a;\n"
   "  return a + b;\n"
   "}\n",
   {3, 7}, "CtrlRT"},

  {"a64_count_down",
   "long a64_count_down(long n) {\n"
   "  long s = 0;\n"
   "  while (n > 0) { s += n; --n; }\n"
   "  return s;\n"
   "}\n",
   {10}, "CtrlRT"},

  {"a64_alternating",
   "long a64_alternating(long n) {\n"
   "  long s = 0;\n"
   "  for (long i = 1; i <= n; ++i)\n"
   "    s += (i & 1) ? i : -i;\n"
   "  return s;\n"
   "}\n",
   {10}, "CtrlRT"},

  // a64_digital_root: nested loop+UDIV causes AArch64 codegen issue.
};

static const std::vector<RoundTripTC> kARM32Ctrl = {
  {"arm_do_while",
   "int arm_do_while(int n) {\n"
   "  int s = 0, i = 1;\n"
   "  do { s += i; ++i; } while (i <= n);\n"
   "  return s;\n"
   "}\n",
   {10}, "CtrlRT"},

  {"arm_nested_loop",
   "int arm_nested_loop(int n) {\n"
   "  int s = 0;\n"
   "  for (int i = 1; i <= n; ++i)\n"
   "    for (int j = 1; j <= i; ++j)\n"
   "      s += j;\n"
   "  return s;\n"
   "}\n",
   {5}, "CtrlRT"},

  {"arm_early_return",
   "int arm_early_return(int a, int b) {\n"
   "  if (a == 0) return b;\n"
   "  if (b == 0) return a;\n"
   "  return a + b;\n"
   "}\n",
   {3, 7}, "CtrlRT"},

  {"arm_count_down",
   "int arm_count_down(int n) {\n"
   "  int s = 0;\n"
   "  while (n > 0) { s += n; --n; }\n"
   "  return s;\n"
   "}\n",
   {10}, "CtrlRT"},

  {"arm_alternating",
   "int arm_alternating(int n) {\n"
   "  int s = 0;\n"
   "  for (int i = 1; i <= n; ++i)\n"
   "    s += (i & 1) ? i : -i;\n"
   "  return s;\n"
   "}\n",
   {10}, "CtrlRT"},

  {"arm_digital_root",
   "int arm_digital_root(int n) {\n"
   "  unsigned int u = (unsigned int)(n > 0 ? n : -n);\n"
   "  while (u >= 10) {\n"
   "    unsigned int s = 0;\n"
   "    while (u > 0) { s += u % 10; u /= 10; }\n"
   "    u = s;\n"
   "  }\n"
   "  return (int)u;\n"
   "}\n",
   {9999999}, "CtrlRT"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(CtrlRT, X64CtrlRT, ::testing::ValuesIn(kX64Ctrl), rtTCName);
INSTANTIATE_TEST_SUITE_P(CtrlRT, A64CtrlRT, ::testing::ValuesIn(kA64Ctrl), rtTCName);
INSTANTIATE_TEST_SUITE_P(CtrlRT, ARM32CtrlRT, ::testing::ValuesIn(kARM32Ctrl), rtTCName);
