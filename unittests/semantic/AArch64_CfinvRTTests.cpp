//===- AArch64_CfinvRTTests.cpp - CFINV complement carry flag ---*- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 `CFINV` (FEAT_FlagM, ARMv8.4) complements the carry flag and leaves
// N/Z/V untouched:  PSTATE.C = NOT PSTATE.C.  It is the last of the FlagM /
// FlagM2 condition-flag ops to get coverage (XAFLAG/AXFLAG/RMIF/SETF8/16 are
// done).  The lifter models it as a single BOOL_NOT on the modelled CFLAG, so
// the recompiled image must (a) actually invert C and (b) NOT disturb the other
// three flags — a no-op mis-lift (opaque `cfinv` that touches only hardware
// NZCV, disconnected from the model) would leave C unchanged through the model.
//
// Each probe seeds NZCV with a modelled producer (`cmp`), runs `cfinv`, then
// reads the flag(s) back with `cset` (a modelled consumer) folded into the
// return value.  Unicorn's default AArch64 CPU is MAX (FlagM present), so the
// roundtrip is native.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class A64CfinvRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CfinvRT, Verify) { roundTripAArch64(GetParam()); }

// cmp a,b sets C = (a >=u b); cfinv flips it; cset cs reads the inverted C.
#define CFINV_C_FN \
  "long f(long a,long b){unsigned long c;" \
  "__asm__ volatile(\"cmp %1,%2\\n\\tcfinv\\n\\tcset %w0,cs\"" \
  ":\"=r\"(c):\"r\"(a),\"r\"(b):\"cc\");return (long)c;}\n"

// clang-format off
static const std::vector<RoundTripTC> kA64 = {
  // cmp(5,3): C=1 (no borrow) -> cfinv -> C=0 -> cset cs => 0.
  {"cfinv_c1to0", CFINV_C_FN, {5, 3}, "Cfinv", 0, "-march=armv8.4-a"},

  // cmp(3,5): C=0 (borrow) -> cfinv -> C=1 -> cset cs => 1.
  {"cfinv_c0to1", CFINV_C_FN, {3, 5}, "Cfinv", 0, "-march=armv8.4-a"},

  // Equal: cmp(7,7): C=1 -> cfinv -> 0.
  {"cfinv_eq", CFINV_C_FN, {7, 7}, "Cfinv", 0, "-march=armv8.4-a"},

  // Double cfinv restores the original C (=1 for cmp(9,2)) -> cset cs => 1.
  {"cfinv_double",
   "long f(long a,long b){unsigned long c;"
   "__asm__ volatile(\"cmp %1,%2\\n\\tcfinv\\n\\tcfinv\\n\\tcset %w0,cs\""
   ":\"=r\"(c):\"r\"(a),\"r\"(b):\"cc\");return (long)c;}\n",
   {9, 2}, "Cfinv", 0, "-march=armv8.4-a"},

  // CFINV must touch ONLY C: seed NZCV with cmp, flip C, read all four flags.
  // cmp(INT64_MIN,1): N=0,Z=0,C=1,V=1 -> after cfinv: N=0,Z=0,C=0,V=1 => bit3=8.
  // A mis-lift that drops the invert leaves C=1 -> bit2 set => 0b1100=12.
  {"cfinv_only_c",
   "long f(long a,long b){unsigned int n,z,c,v;"
   "__asm__ volatile(\"cmp %4,%5\\n\\tcfinv\\n\\t\"\n"
   "\"cset %w0,mi\\n\\tcset %w1,eq\\n\\tcset %w2,cs\\n\\tcset %w3,vs\""
   ":\"=&r\"(n),\"=&r\"(z),\"=&r\"(c),\"=&r\"(v):\"r\"(a),\"r\"(b):\"cc\");"
   "return (long)n|((long)z<<1)|((long)c<<2)|((long)v<<3);}\n",
   {0x8000000000000000ULL, 1ULL}, "Cfinv", 0, "-march=armv8.4-a"},

  // Preserve N/Z too: cmp(-1,1): N=1,Z=0,C=0,V=0 -> after cfinv: N=1,Z=0,C=1,V=0
  // => 0b0101=5.  Pins N stays set and C goes 0->1.
  {"cfinv_keep_nz",
   "long f(long a,long b){unsigned int n,z,c,v;"
   "__asm__ volatile(\"cmp %4,%5\\n\\tcfinv\\n\\t\"\n"
   "\"cset %w0,mi\\n\\tcset %w1,eq\\n\\tcset %w2,cs\\n\\tcset %w3,vs\""
   ":\"=&r\"(n),\"=&r\"(z),\"=&r\"(c),\"=&r\"(v):\"r\"(a),\"r\"(b):\"cc\");"
   "return (long)n|((long)z<<1)|((long)c<<2)|((long)v<<3);}\n",
   {0xFFFFFFFFFFFFFFFFULL, 1ULL}, "Cfinv", 0, "-march=armv8.4-a"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(Cfinv, A64CfinvRT, ::testing::ValuesIn(kA64), rtTCName);
