//===- X64_RclVarCountRTTests.cpp - RCL/RCR variable count (incl. 0) -*- C++ -*-=//
//
// RCL/RCR rotate the operand THROUGH CF as a (width+1)-bit quantity.  The
// existing coverage drives only the immediate `$1` form; the variable `%cl`
// form -- and in particular a runtime count of ZERO with CF pre-set -- has no
// coverage.  Count 0 must leave the operand AND every flag unchanged, but the
// per-count carry-injection shift (CfIn = CF << (cnt-1) for RCL, CF <<
// (width-cnt) for RCR) goes out of range at cnt==0; a recompiled variable shift
// masks the amount (x86 shl/shr mask to 5/6 bits) and would wrongly OR the
// carry bit into the result instead of producing zero.  These probes pre-set CF
// with stc/clc and rotate by a %cl of 0 / 1 / 4 so any count-0 divergence (or
// per-count value error) shows up against the native run.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RclVarCountRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RclVarCountRT, Verify) { roundTripX64(GetParam()); }

// MN=rcl/rcr, SETC=stc/clc seeds CF.  cnt comes in via "c".  Fold the rotated
// value and the resulting CF (via setc) into disjoint fields.
#define RC_FN(MN, SETC) \
  "unsigned long f(unsigned long a,unsigned long cnt){\n" \
  "  unsigned x=(unsigned)a; unsigned char cf;\n" \
  "  __asm__ volatile(\"" SETC "\\n\\t" MN "l %%cl,%0\\n\\tsetc %1\"\n" \
  "    :\"+r\"(x),\"=q\"(cf):\"c\"((unsigned char)cnt):\"cc\");\n" \
  "  return ((unsigned long)x)|((unsigned long)(cf&1)<<32);}\n"

// clang-format off
static const std::vector<RoundTripTC> kX64 = {
  // ===== RCL, CF pre-set with stc. =====
  // count 0: operand and CF must be UNCHANGED (the bug: result ORs CF into a
  // high bit, CF flips).  a has bit31/bit0 clear so an injected carry shows.
  {"rcl_cnt0_cf1", RC_FN("rcl","stc"), {0x12345678ULL, 0}, "RclVar"},
  {"rcl_cnt0_cf0", RC_FN("rcl","clc"), {0x12345678ULL, 0}, "RclVar"},
  // count 1 / 4: real rotates (controls).
  {"rcl_cnt1_cf1", RC_FN("rcl","stc"), {0x80000000ULL, 1}, "RclVar"},
  {"rcl_cnt1_cf0", RC_FN("rcl","clc"), {0x80000001ULL, 1}, "RclVar"},
  {"rcl_cnt4_cf1", RC_FN("rcl","stc"), {0x12345678ULL, 4}, "RclVar"},

  // ===== RCR, CF pre-set with stc. =====
  {"rcr_cnt0_cf1", RC_FN("rcr","stc"), {0x12345678ULL, 0}, "RclVar"},
  {"rcr_cnt0_cf0", RC_FN("rcr","clc"), {0x12345678ULL, 0}, "RclVar"},
  {"rcr_cnt1_cf1", RC_FN("rcr","stc"), {0x00000001ULL, 1}, "RclVar"},
  {"rcr_cnt1_cf0", RC_FN("rcr","clc"), {0x00000002ULL, 1}, "RclVar"},
  {"rcr_cnt4_cf1", RC_FN("rcr","stc"), {0x12345678ULL, 4}, "RclVar"},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(RclVar, X64RclVarCountRT, ::testing::ValuesIn(kX64),
                         rtTCName);
