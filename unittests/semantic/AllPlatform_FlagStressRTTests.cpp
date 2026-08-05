//===- AllPlatform_FlagStressRTTests.cpp - flag/compare stress ----*- C++ -*-===//
//
// Aggressive roundtrip stress for flag-consuming patterns, the historically
// most bug-prone area of the lifter/optimizer (bugs #32, #48, #60, #147-149):
//   * multiple independent comparisons consumed in the same basic block
//   * SETcc-style 0/1 results summed together
//   * flags produced by non-SUB ops (ADD / AND / shift) feeding conditions
//   * signed vs unsigned comparison disambiguation with high-bit values
//   * three-way (spaceship) comparisons
//
// A wrong flag source silently changes control flow / selected value, so
// these are the most important semantic-equivalence guards.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FlagStressRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FlagStressRT, Verify) { roundTripX64(GetParam()); }

class A64FlagStressRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FlagStressRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32FlagStressRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FlagStressRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

// Shared flag-stress patterns; `T` is the integer type (long / int).
static std::vector<RoundTripTC> makeFlagTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Two independent min-selects in one block (bug #147 class).
    {p+"_multi_cmov",
     t+" "+p+"_multi_cmov("+t+" a, "+t+" b, "+t+" c, "+t+" d) {\n"
     "  "+t+" x = a < b ? a : b;\n"
     "  "+t+" y = c < d ? c : d;\n"
     "  return x * 1000 + y;\n"
     "}\n",
     {7, 3, 9, 2}, "FlagStress"},

    // Same operands compared two ways, both consumed.
    {p+"_lt_and_gt",
     t+" "+p+"_lt_and_gt("+t+" a, "+t+" b) {\n"
     "  "+t+" lt = (a < b) ? 100 : 0;\n"
     "  "+t+" gt = (a > b) ? 10 : 0;\n"
     "  return lt + gt;\n"
     "}\n",
     {5, 5}, "FlagStress"},

    // Three-way / spaceship: (a>b) - (a<b).
    {p+"_spaceship",
     t+" "+p+"_spaceship("+t+" a, "+t+" b) {\n"
     "  return (a > b) - (a < b);\n"
     "}\n",
     {3, 8}, "FlagStress"},

    // SETcc sum: three boolean comparisons added.
    {p+"_setcc_sum",
     t+" "+p+"_setcc_sum("+t+" a, "+t+" b, "+t+" c) {\n"
     "  return (a < b) + (b < c) + (a < c);\n"
     "}\n",
     {1, 2, 3}, "FlagStress"},

    // Flag from AND (TEST) feeding a select.
    {p+"_and_cond",
     t+" "+p+"_and_cond("+t+" a, "+t+" b) {\n"
     "  return (a & b) ? a : b;\n"
     "}\n",
     {0xF0, 0x0F}, "FlagStress"},

    // Flag from ADD feeding a select.
    {p+"_add_cond",
     t+" "+p+"_add_cond("+t+" a, "+t+" b) {\n"
     "  return (a + b) != 0 ? a : b;\n"
     "}\n",
     {5, (uint64_t)(int64_t)-5}, "FlagStress"},

    // Flag from shift feeding a select.
    {p+"_shl_cond",
     t+" "+p+"_shl_cond("+t+" a) {\n"
     "  return ((a << 1) > 0) ? 1 : -1;\n"
     "}\n",
     {0x40000000}, "FlagStress"},

    // Two-stage clamp: two compares, two branches.
    {p+"_clamp_branch",
     t+" "+p+"_clamp_branch("+t+" v, "+t+" lo, "+t+" hi) {\n"
     "  if (v < lo) return lo;\n"
     "  if (v > hi) return hi;\n"
     "  return v;\n"
     "}\n",
     {150, 10, 100}, "FlagStress"},

    // Signed vs unsigned compare disambiguation (negative input).
    {p+"_signed_vs_unsigned",
     t+" "+p+"_signed_vs_unsigned("+t+" a, "+t+" b) {\n"
     "  "+t+" su = (a < b);\n"
     "  "+t+" uu = ((unsigned "+t+")a < (unsigned "+t+")b);\n"
     "  return su * 10 + uu;\n"
     "}\n",
     {(uint64_t)(int64_t)-1, 1}, "FlagStress"},

    // Unsigned greater-than loop condition with high-bit values (bug #60).
    {p+"_ugt_loop",
     "unsigned "+t+" "+p+"_ugt_loop(unsigned "+t+" a, unsigned "+t+" b) {\n"
     "  unsigned "+t+" cnt = 0;\n"
     "  while (a > b) { a -= b; cnt++; }\n"
     "  return cnt;\n"
     "}\n",
     {100, 7}, "FlagStress"},

    // Chained relational with && (multiple flags, short-circuit).
    {p+"_range_check",
     t+" "+p+"_range_check("+t+" x, "+t+" lo, "+t+" hi) {\n"
     "  return (x >= lo && x <= hi) ? 1 : 0;\n"
     "}\n",
     {50, 0, 100}, "FlagStress"},

    // Nested ternary using overlapping comparisons.
    {p+"_sign3",
     t+" "+p+"_sign3("+t+" a) {\n"
     "  return a > 0 ? 1 : (a < 0 ? -1 : 0);\n"
     "}\n",
     {(uint64_t)(int64_t)-42}, "FlagStress"},
  };
}

static const std::vector<RoundTripTC> kX64Flag = makeFlagTC("x64fs", "long");
static const std::vector<RoundTripTC> kA64Flag = makeFlagTC("a64fs", "long");
static const std::vector<RoundTripTC> kARM32Flag = makeFlagTC("armfs", "int");

// clang-format on

INSTANTIATE_TEST_SUITE_P(FlagStress, X64FlagStressRT,
                         ::testing::ValuesIn(kX64Flag), rtTCName);
INSTANTIATE_TEST_SUITE_P(FlagStress, A64FlagStressRT,
                         ::testing::ValuesIn(kA64Flag), rtTCName);
INSTANTIATE_TEST_SUITE_P(FlagStress, ARM32FlagStressRT,
                         ::testing::ValuesIn(kARM32Flag), rtTCName);
