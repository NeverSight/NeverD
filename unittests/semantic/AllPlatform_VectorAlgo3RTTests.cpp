//===- AllPlatform_VectorAlgo3RTTests.cpp - more vectorizable algos *- C++ -*-===//
//
// Third batch of realistic clang -O2 auto-vectorized algorithms used as
// high-yield lift bug probes.  Exercises lift paths the first two batches did
// not: sum-of-absolute-differences (SAD), widening sum-of-squares, signed
// multiply-high, per-element clamp (min+max), threshold counting, unsigned
// saturating subtract, signed saturating add, and even/odd deinterleave.
// Each runs the full binary -> lift -> MedIR -> LLVM IR -> obj -> binary
// roundtrip and compares Unicorn execution of the original vs recompiled code.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo3RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo3RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo3RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo3RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo3RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo3RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec3TC(const char *prefix, const char *T,
                                           int opt) {
  std::string p = prefix, t = T;
  return {
    // Sum of absolute differences over byte arrays -> psadbw / [su]abd+reduce.
    {p+"_sad",
     t+" "+p+"_sad("+t+" a, "+t+" b) {\n"
     "  unsigned char x[32], y[32]; int s = 0;\n"
     "  for (int i = 0; i < 32; i++) {\n"
     "    x[i] = (unsigned char)((a * (i + 1)) ^ (i * 7));\n"
     "    y[i] = (unsigned char)((b * (i + 2)) ^ (i * 5));\n"
     "  }\n"
     "  for (int i = 0; i < 32; i++) { int d = (int)x[i] - (int)y[i]; s += d < 0 ? -d : d; }\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL, 0x7654321ULL}, "VectorAlgo3", opt},

    // Widening sum of squares -> smull/pmullw same-operand widening multiply.
    {p+"_ssq",
     t+" "+p+"_ssq("+t+" a) {\n"
     "  short v[16]; int s = 0;\n"
     "  for (int i = 0; i < 16; i++) v[i] = (short)(((a * (i + 1)) ^ (i * 13)) & 0x3FF) - 512;\n"
     "  for (int i = 0; i < 16; i++) s += (int)v[i] * (int)v[i];\n"
     "  return s;\n"
     "}\n",
     {0xABCDEF1ULL}, "VectorAlgo3", opt},

    // Signed multiply-high of i16 products -> pmulhw / smull+shrn.
    {p+"_mulhi",
     t+" "+p+"_mulhi("+t+" a, "+t+" b) {\n"
     "  short x[16], y[16]; int s = 0;\n"
     "  for (int i = 0; i < 16; i++) {\n"
     "    x[i] = (short)((a * (i + 1)) ^ (i * 3));\n"
     "    y[i] = (short)((b * (i + 2)) ^ (i * 9));\n"
     "  }\n"
     "  for (int i = 0; i < 16; i++) s += ((int)x[i] * (int)y[i]) >> 16;\n"
     "  return s;\n"
     "}\n",
     {0xCAFE1ULL, 0xBEEF2ULL}, "VectorAlgo3", opt},

    // Per-element clamp to [-100,100] then sum -> pmaxsd+pminsd / smax+smin.
    {p+"_clampel",
     t+" "+p+"_clampel("+t+" a) {\n"
     "  int v[16]; int s = 0;\n"
     "  for (int i = 0; i < 16; i++) v[i] = (int)(a * (i + 1)) ^ (i * 17);\n"
     "  for (int i = 0; i < 16; i++) { int x = v[i]; if (x < -100) x = -100; if (x > 100) x = 100; s += x; }\n"
     "  return s;\n"
     "}\n",
     {0x13572468ULL}, "VectorAlgo3", opt},

    // Count of elements above a threshold -> cmpgt + mask + reduce.
    {p+"_thresh",
     t+" "+p+"_thresh("+t+" a) {\n"
     "  int v[32]; int s = 0;\n"
     "  for (int i = 0; i < 32; i++) v[i] = (int)(a * (i + 1)) ^ (i * 3);\n"
     "  for (int i = 0; i < 32; i++) if (v[i] > 1000) s++;\n"
     "  return s;\n"
     "}\n",
     {0x2468ACE0ULL}, "VectorAlgo3", opt},

    // Unsigned saturating subtract on bytes then sum -> psubusb/uqsub/vqsub.
    {p+"_usub",
     t+" "+p+"_usub("+t+" a, "+t+" b) {\n"
     "  unsigned char x[32], y[32]; int s = 0;\n"
     "  for (int i = 0; i < 32; i++) {\n"
     "    x[i] = (unsigned char)((a * (i + 2)) ^ (i * 7));\n"
     "    y[i] = (unsigned char)((b * (i + 4)) ^ (i * 9));\n"
     "  }\n"
     "  for (int i = 0; i < 32; i++) { int d = (int)x[i] - (int)y[i]; if (d < 0) d = 0; s += d; }\n"
     "  return s;\n"
     "}\n",
     {0x55667788ULL, 0x99AABBCCULL}, "VectorAlgo3", opt},

    // Signed saturating add to [-128,127] then sum -> paddsb/sqadd/vqadd.
    {p+"_ssat",
     t+" "+p+"_ssat("+t+" a, "+t+" b) {\n"
     "  signed char x[32], y[32]; int s = 0;\n"
     "  for (int i = 0; i < 32; i++) {\n"
     "    x[i] = (signed char)((a * (i + 1)) ^ (i * 5));\n"
     "    y[i] = (signed char)((b * (i + 3)) ^ (i * 11));\n"
     "  }\n"
     "  for (int i = 0; i < 32; i++) { int v = (int)x[i] + (int)y[i]; if (v > 127) v = 127; if (v < -128) v = -128; s += v; }\n"
     "  return s;\n"
     "}\n",
     {0x0BADF00DULL, 0xFEEDFACEULL}, "VectorAlgo3", opt},

    // Even/odd deinterleave difference sum -> uzp / hsub pattern.
    {p+"_evenodd",
     t+" "+p+"_evenodd("+t+" a) {\n"
     "  int v[32]; int s = 0;\n"
     "  for (int i = 0; i < 32; i++) v[i] = (int)(((a * (i + 1)) ^ (i * 11)) & 0x1FFF) - 4096;\n"
     "  for (int i = 0; i < 16; i++) s += v[2*i] - v[2*i+1];\n"
     "  return s;\n"
     "}\n",
     {0x33442255ULL}, "VectorAlgo3", opt},
  };
}

static const std::vector<RoundTripTC> kX64Vec3 = makeVec3TC("x64v3", "long", 2);
static const std::vector<RoundTripTC> kA64Vec3 = makeVec3TC("a64v3", "long", 2);
static const std::vector<RoundTripTC> kARM32Vec3 = makeVec3TC("armv3", "int", 2);

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo3, X64VectorAlgo3RT,
                         ::testing::ValuesIn(kX64Vec3), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo3, A64VectorAlgo3RT,
                         ::testing::ValuesIn(kA64Vec3), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo3, ARM32VectorAlgo3RT,
                         ::testing::ValuesIn(kARM32Vec3), rtTCName);
