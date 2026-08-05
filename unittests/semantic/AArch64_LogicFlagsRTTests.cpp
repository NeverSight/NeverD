//===- AArch64_LogicFlagsRTTests.cpp - AArch64 logical NZCV ----*- C++ -*-===//
//
// AArch64 flag-setting logical ops (ANDS/BICS, and their TST alias) set N/Z
// from the result and CLEAR C and V to 0.  The roundtrip harness only compares
// return values, so these probes pre-set C/V (via cmp / an overflowing adds)
// then fold the post-op C/V into the result with cset, exposing any flag the
// lifter forgets to clear.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class AArch64LogicFlagsRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(AArch64LogicFlagsRT, Verify) { roundTripAArch64(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // BICS must clear C (cmp sets C=1; BICS should reset it to 0).
  {"bics_clears_c",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"cmp %1,%1\\n\\tbics x3,%1,%2\\n\\tcset %0,cs\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"x3\",\"cc\");return r;}\n",
   {0x00000000000000FFULL, 0x000000000000000FULL}, "LogicFlags"},
  // BICS must clear V (an overflowing adds sets V=1; BICS should reset it to 0).
  {"bics_clears_v",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"mov w4,#0x7fffffff\\n\\tadds w4,w4,#1\\n\\t"
   "bics x3,%1,%2\\n\\tcset %0,vs\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"x3\",\"x4\",\"cc\");return r;}\n",
   {0x00000000000000FFULL, 0x000000000000000FULL}, "LogicFlags"},
  // BICS N/Z still correct (result 0xF0 -> N=0,Z=0; cset eq=0).
  {"bics_nz",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"bics x3,%1,%2\\n\\tcset %0,eq\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"x3\",\"cc\");return r;}\n",
   {0x000000000000000FULL, 0x000000000000000FULL}, "LogicFlags"},

  // ANDS / TST controls (already clear C/V correctly).
  {"ands_clears_c",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"cmp %1,%1\\n\\tands x3,%1,%2\\n\\tcset %0,cs\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"x3\",\"cc\");return r;}\n",
   {0x00000000000000FFULL, 0x000000000000000FULL}, "LogicFlags"},
  {"tst_clears_c",
   "long f(long a,long b){unsigned long r;"
   "__asm__ volatile(\"cmp %1,%1\\n\\ttst %1,%2\\n\\tcset %0,cs\""
   ":\"=r\"(r):\"r\"(a),\"r\"(b):\"cc\");return r;}\n",
   {0x00000000000000FFULL, 0x000000000000000FULL}, "LogicFlags"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(LogicFlags, AArch64LogicFlagsRT,
                         ::testing::ValuesIn(kA64), rtTCName);
