//===- AllPlatform_VectorAlgo51RTTests.cpp - i64 element-wise NEON/SSE ----===//
//
// Fifty-first batch of clang -O3 vector probes targeting 64-bit-ELEMENT vector
// arithmetic — the lane width that is native on x86-64 (PADDQ/PSUBQ/PXOR/PSLLQ)
// and AArch64 (.2d) but is emulated on ARM32 NEON as D-register PAIRS, which is
// exactly the machinery that recently miscompiled: #532 bug① (mixed fresh/stale
// D-half tracking on a wide-read reconstruction) and b49d8f6 (vmov.i64 shift-by-
// bitwidth UB).  No prior batch drove a pure i64-element auto-vectorized loop on
// all three targets, so this is the first all-target guardrail for that lane.
//
//   * _addxor64 : element-wise i64 add + xor chain reduced (VADD.i64 / VEOR).
//   * _shift64  : element-wise i64 const-rotate / shift-xor (VSHL.i64/VSHR.u64).
//   * _andor64  : element-wise i64 and/or/xor with a shifted operand.
//   * _mixed64  : i64 add with a 64-bit immediate (NEON i64 constant pool) THEN
//                 a 32-bit op on the low lane + recombine — mixed-width lane
//                 manipulation, the #532 bug① shape, plus the #531 i64 pool.
//
// i64 LCG fill is composed from two u32 halves (no i64 multiply); all shifts are
// by constants and there is no i64 divide, so ARM32 stays libcall-free.  Each
// kernel folds to one exact integer.  x64 uses -msse4.2; a64/arm32 use the
// default NEON baseline.  Three targets (i386 skipped, no native i64 SIMD).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo51RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo51RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo51RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo51RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo51RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo51RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec51TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Element-wise i64 add + xor chain (VADD.i64 / VEOR; ARM32 D-pair lanes).
    {p+"_addxor64",
     t+" "+p+"_addxor64("+t+" a){\n"
     "  unsigned long long x[64], y[64]; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<64;i++){ s=s*1664525u+1013904223u; unsigned lo=s;\n"
     "    s=s*1664525u+1013904223u; unsigned hi=s; x[i]=((unsigned long long)hi<<32)|lo;\n"
     "    s=s*1664525u+1013904223u; lo=s; s=s*1664525u+1013904223u; hi=s;\n"
     "    y[i]=((unsigned long long)hi<<32)|lo; }\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<64;i++){ unsigned long long v=x[i]+y[i]; v^=x[i]; acc+=v; acc^=v; }\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo51", opt, fl},

    // Element-wise i64 const rotate + shift-xor chain (VSHL.i64 / VSHR.u64).
    {p+"_shift64",
     t+" "+p+"_shift64("+t+" a){\n"
     "  unsigned long long x[64]; unsigned s=(unsigned)a^0x9e3779b9u;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u; unsigned lo=s;\n"
     "    s=s*1103515245u+12345u; unsigned hi=s; x[i]=((unsigned long long)hi<<32)|lo; }\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<64;i++){ unsigned long long v=x[i]; v=(v<<13)|(v>>51);\n"
     "    v^=(v>>7); v+=(v<<5); acc+=v; }\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x2345678ULL}, "VectorAlgo51", opt, fl},

    // Element-wise i64 and/or/xor with a shifted operand (VAND/VORR/VEOR).
    {p+"_andor64",
     t+" "+p+"_andor64("+t+" a){\n"
     "  unsigned long long x[64], y[64]; unsigned s=(unsigned)a+0x77u;\n"
     "  for(int i=0;i<64;i++){ s=s*1664525u+1013904223u; unsigned lo=s;\n"
     "    s=s*1664525u+1013904223u; unsigned hi=s; x[i]=((unsigned long long)hi<<32)|lo;\n"
     "    s=s*1664525u+1013904223u; lo=s; s=s*1664525u+1013904223u; hi=s;\n"
     "    y[i]=((unsigned long long)hi<<32)|lo; }\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<64;i++){ unsigned long long t=(x[i]&y[i])|(x[i]^(y[i]<<1));\n"
     "    acc^=t; acc+=(t>>17); }\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x3456789ULL}, "VectorAlgo51", opt, fl},

    // i64 add with a 64-bit immediate (NEON i64 constant pool) then a 32-bit op
    // on the low lane + recombine: mixed-width lane manipulation (#532 bug① shape
    // + #531 i64 pool) — the most fragile combination in this family.
    {p+"_mixed64",
     t+" "+p+"_mixed64("+t+" a){\n"
     "  unsigned long long x[64]; unsigned s=(unsigned)a+0x55u;\n"
     "  for(int i=0;i<64;i++){ s=s*22695477u+1u; unsigned lo=s;\n"
     "    s=s*22695477u+1u; unsigned hi=s; x[i]=((unsigned long long)hi<<32)|lo; }\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<64;i++){ unsigned long long v=x[i] + 0x0123456789abcdefULL;\n"
     "    unsigned low=(unsigned)v ^ 0xdeadbeefu;\n"
     "    v=((unsigned long long)(unsigned)(v>>32)<<32)|low; acc^=v; acc+=(v<<3); }\n"
     "  return ("+t+")(acc ^ (acc>>32));\n"
     "}\n",
     {0x456789AULL}, "VectorAlgo51", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec51 =
    makeVec51TC("x64v51", "long", 3, "-msse4.2");
static const std::vector<RoundTripTC> kA64Vec51 =
    makeVec51TC("a64v51", "long", 3, "");
static const std::vector<RoundTripTC> kARM32Vec51 =
    makeVec51TC("armv51", "int", 3, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo51, X64VectorAlgo51RT,
                         ::testing::ValuesIn(kX64Vec51), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo51, A64VectorAlgo51RT,
                         ::testing::ValuesIn(kA64Vec51), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo51, ARM32VectorAlgo51RT,
                         ::testing::ValuesIn(kARM32Vec51), rtTCName);
