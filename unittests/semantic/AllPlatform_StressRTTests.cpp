//===- AllPlatform_StressRTTests.cpp - Cross-platform stress RT -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Stress tests that exercise complex patterns across all platforms:
// large functions, deep nesting, many variables, carry chains, etc.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64StressRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64StressRT, Verify) { roundTripX64(GetParam()); }

class A64StressRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64StressRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32StressRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32StressRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static const char *kBubblePass =
  "long bubble_pass(long packed) {\n"
  "  int a = (int)(packed & 0xFF);\n"
  "  int b = (int)((packed >> 8) & 0xFF);\n"
  "  int c = (int)((packed >> 16) & 0xFF);\n"
  "  int d = (int)((packed >> 24) & 0xFF);\n"
  "  if (a > b) { int t = a; a = b; b = t; }\n"
  "  if (b > c) { int t = b; b = c; c = t; }\n"
  "  if (c > d) { int t = c; c = d; d = t; }\n"
  "  if (a > b) { int t = a; a = b; b = t; }\n"
  "  if (b > c) { int t = b; b = c; c = t; }\n"
  "  if (a > b) { int t = a; a = b; b = t; }\n"
  "  return (long)(unsigned)(a | (b << 8) | (c << 16) | (d << 24));\n"
  "}\n";

static const char *kBubblePass32 =
  "int bubble_pass(int packed) {\n"
  "  int a = packed & 0xFF;\n"
  "  int b = (packed >> 8) & 0xFF;\n"
  "  int c = (packed >> 16) & 0xFF;\n"
  "  int d = (packed >> 24) & 0xFF;\n"
  "  if (a > b) { int t = a; a = b; b = t; }\n"
  "  if (b > c) { int t = b; b = c; c = t; }\n"
  "  if (c > d) { int t = c; c = d; d = t; }\n"
  "  if (a > b) { int t = a; a = b; b = t; }\n"
  "  if (b > c) { int t = b; b = c; c = t; }\n"
  "  if (a > b) { int t = a; a = b; b = t; }\n"
  "  return a | (b << 8) | (c << 16) | (d << 24);\n"
  "}\n";

static const char *kMultiChain =
  "long multi_chain(long a, long b) {\n"
  "  long x = a + b;\n"
  "  long y = a - b;\n"
  "  long z = x * y;\n"
  "  long w = z ^ (x + y);\n"
  "  long v = w | (x & y);\n"
  "  return v + (z >> 4);\n"
  "}\n";

static const char *kMultiChain32 =
  "int multi_chain(int a, int b) {\n"
  "  int x = a + b;\n"
  "  int y = a - b;\n"
  "  int z = x * y;\n"
  "  int w = z ^ (x + y);\n"
  "  int v = w | (x & y);\n"
  "  return v + (z >> 4);\n"
  "}\n";

static const char *kDeepNest =
  "long deep_nest(long a, long b, long c) {\n"
  "  long r = 0;\n"
  "  if (a > 0) {\n"
  "    if (b > 0) {\n"
  "      if (c > 0) r = a + b + c;\n"
  "      else r = a + b - c;\n"
  "    } else {\n"
  "      if (c > 0) r = a - b + c;\n"
  "      else r = a - b - c;\n"
  "    }\n"
  "  } else {\n"
  "    if (b > 0) {\n"
  "      if (c > 0) r = b + c - a;\n"
  "      else r = b - c - a;\n"
  "    } else {\n"
  "      r = -(a + b + c);\n"
  "    }\n"
  "  }\n"
  "  return r;\n"
  "}\n";

static const char *kDeepNest32 =
  "int deep_nest(int a, int b, int c) {\n"
  "  int r = 0;\n"
  "  if (a > 0) {\n"
  "    if (b > 0) {\n"
  "      if (c > 0) r = a + b + c;\n"
  "      else r = a + b - c;\n"
  "    } else {\n"
  "      if (c > 0) r = a - b + c;\n"
  "      else r = a - b - c;\n"
  "    }\n"
  "  } else {\n"
  "    if (b > 0) {\n"
  "      if (c > 0) r = b + c - a;\n"
  "      else r = b - c - a;\n"
  "    } else {\n"
  "      r = -(a + b + c);\n"
  "    }\n"
  "  }\n"
  "  return r;\n"
  "}\n";

static const std::vector<RoundTripTC> kX64Stress = {
  {"x64_bubble_sort", kBubblePass, {0x03010204}, "X64Stress"},
  {"x64_multi_chain", kMultiChain, {42, 17}, "X64Stress"},
  {"x64_deep_nest_ppp", kDeepNest, {10, 20, 30}, "X64Stress"},
  {"x64_deep_nest_ppn", kDeepNest, {10, 20, (uint64_t)(int64_t)-30}, "X64Stress"},
  {"x64_deep_nest_pnp", kDeepNest, {10, (uint64_t)(int64_t)-20, 30}, "X64Stress"},
  {"x64_deep_nest_npp", kDeepNest, {(uint64_t)(int64_t)-10, 20, 30}, "X64Stress"},
  {"x64_deep_nest_nnn", kDeepNest, {(uint64_t)(int64_t)-10, (uint64_t)(int64_t)-20, (uint64_t)(int64_t)-30}, "X64Stress"},
};

static const std::vector<RoundTripTC> kA64Stress = {
  {"a64_bubble_sort", kBubblePass, {0x03010204}, "A64Stress"},
  {"a64_multi_chain", kMultiChain, {42, 17}, "A64Stress"},
  {"a64_deep_nest_ppp", kDeepNest, {10, 20, 30}, "A64Stress"},
  {"a64_deep_nest_nnn", kDeepNest, {(uint64_t)(int64_t)-10, (uint64_t)(int64_t)-20, (uint64_t)(int64_t)-30}, "A64Stress"},
};

static const std::vector<RoundTripTC> kARM32Stress = {
  {"arm_bubble_sort", kBubblePass32, {0x03010204}, "ARM32Stress"},
  {"arm_multi_chain", kMultiChain32, {42, 17}, "ARM32Stress"},
  {"arm_deep_nest_ppp", kDeepNest32, {10, 20, 30}, "ARM32Stress"},
  {"arm_deep_nest_nnn", kDeepNest32, {(uint64_t)(int32_t)-10, (uint64_t)(int32_t)-20, (uint64_t)(int32_t)-30}, "ARM32Stress"},
};

// clang-format on

INSTANTIATE_TEST_SUITE_P(X64Stress, X64StressRT,
                         ::testing::ValuesIn(kX64Stress), rtTCName);
INSTANTIATE_TEST_SUITE_P(A64Stress, A64StressRT,
                         ::testing::ValuesIn(kA64Stress), rtTCName);
INSTANTIATE_TEST_SUITE_P(ARM32Stress, ARM32StressRT,
                         ::testing::ValuesIn(kARM32Stress), rtTCName);
