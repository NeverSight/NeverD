//===- AllPlatform_VectorAlgo2RTTests.cpp - more vectorizable algos -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Second batch of realistic clang -O2 auto-vectorized algorithms used as
// high-yield lift bug probes.  These exercise lift paths that the first batch
// (AllPlatform_VectorAlgoRTTests.cpp) did not: widening subtract, saturating
// byte arithmetic, i16 widening multiply-accumulate (dot product), rounding
// average, byte reversal, interleave/zip, running max, and zero-byte counting.
// Each runs the full binary -> lift -> MedIR -> LLVM IR -> obj -> binary
// roundtrip and compares Unicorn execution of the original vs recompiled code.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo2RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo2RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo2RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo2RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo2RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo2RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec2TC(const char *prefix, const char *T,
                                           int opt) {
  std::string p = prefix, t = T;
  return {
    // Widening difference sum: sum of (short)x[i] - (short)y[i] as int.  Drives
    // ssubl/usubl/vsubl widening subtract + horizontal reduce.
    {p+"_wdiff",
     t+" "+p+"_wdiff("+t+" a, "+t+" b) {\n"
     "  short x[16], y[16]; "+t+" s = 0;\n"
     "  for (int i = 0; i < 16; i++) {\n"
     "    x[i] = (short)((a * (i + 1)) ^ (i * 7));\n"
     "    y[i] = (short)((b * (i + 2)) ^ (i * 5));\n"
     "  }\n"
     "  for (int i = 0; i < 16; i++) s += (int)x[i] - (int)y[i];\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL, 0x7654321ULL}, "VectorAlgo2", opt},

    // Saturating unsigned byte add then sum -> paddusb/uqadd/vqadd.
    {p+"_satb",
     t+" "+p+"_satb("+t+" a, "+t+" b) {\n"
     "  unsigned char x[32], y[32]; int s = 0;\n"
     "  for (int i = 0; i < 32; i++) {\n"
     "    x[i] = (unsigned char)((a * (i + 3)) ^ (i * 11));\n"
     "    y[i] = (unsigned char)((b * (i + 5)) ^ (i * 13));\n"
     "  }\n"
     "  for (int i = 0; i < 32; i++) {\n"
     "    int v = (int)x[i] + (int)y[i]; if (v > 255) v = 255; s += v;\n"
     "  }\n"
     "  return s;\n"
     "}\n",
     {0xABCDEF1ULL, 0x1FEDCBAULL}, "VectorAlgo2", opt},

    // i16 dot product -> smlal/pmaddwd widening multiply-add.
    {p+"_dot16",
     t+" "+p+"_dot16("+t+" a, "+t+" b) {\n"
     "  short x[16], y[16]; "+t+" s = 0;\n"
     "  for (int i = 0; i < 16; i++) {\n"
     "    x[i] = (short)((a * (i + 1)) ^ (i * 3));\n"
     "    y[i] = (short)((b * (i + 2)) ^ (i * 9));\n"
     "  }\n"
     "  for (int i = 0; i < 16; i++) s += (int)x[i] * (int)y[i];\n"
     "  return s;\n"
     "}\n",
     {0xCAFE1ULL, 0xBEEF2ULL}, "VectorAlgo2", opt},

    // Rounding average of two byte arrays then sum -> pavgb/urhadd/vrhadd.
    {p+"_avg",
     t+" "+p+"_avg("+t+" a, "+t+" b) {\n"
     "  unsigned char x[32], y[32]; int s = 0;\n"
     "  for (int i = 0; i < 32; i++) {\n"
     "    x[i] = (unsigned char)((a * (i + 2)) ^ (i * 7));\n"
     "    y[i] = (unsigned char)((b * (i + 4)) ^ (i * 9));\n"
     "  }\n"
     "  for (int i = 0; i < 32; i++) s += ((int)x[i] + (int)y[i] + 1) >> 1;\n"
     "  return s;\n"
     "}\n",
     {0x55667788ULL, 0x99AABBCCULL}, "VectorAlgo2", opt},

    // Running (prefix) max over a derived array, then return last -> smax chain.
    {p+"_pmax",
     t+" "+p+"_pmax("+t+" a) {\n"
     "  int v[16]; int m = -2000000000; "+t+" s = 0;\n"
     "  for (int i = 0; i < 16; i++) v[i] = (int)(a * (i + 1)) ^ (i * 17);\n"
     "  for (int i = 0; i < 16; i++) { if (v[i] > m) m = v[i]; s += m; }\n"
     "  return s;\n"
     "}\n",
     {0x13572468ULL}, "VectorAlgo2", opt},

    // Count of zero low-nibbles across a derived array -> cmeq + reduce.
    {p+"_cntz",
     t+" "+p+"_cntz("+t+" a) {\n"
     "  unsigned char v[32]; int s = 0;\n"
     "  for (int i = 0; i < 32; i++) v[i] = (unsigned char)((a * (i + 1)) ^ (i * 3));\n"
     "  for (int i = 0; i < 32; i++) if ((v[i] & 0x0F) == 0) s++;\n"
     "  return s;\n"
     "}\n",
     {0x2468ACE0ULL}, "VectorAlgo2", opt},

    // Byte-reverse each 32-bit word then sum -> rev/bswap/vrev.
    {p+"_revsum",
     t+" "+p+"_revsum("+t+" a) {\n"
     "  unsigned w[16]; "+t+" s = 0;\n"
     "  for (int i = 0; i < 16; i++) w[i] = (unsigned)(a * (i + 1)) ^ (unsigned)(i * 2654435761u);\n"
     "  for (int i = 0; i < 16; i++) {\n"
     "    unsigned x = w[i];\n"
     "    x = ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) | ((x >> 8) & 0xFF00) | (x >> 24);\n"
     "    s += (int)x;\n"
     "  }\n"
     "  return s;\n"
     "}\n",
     {0x0BADF00DULL}, "VectorAlgo2", opt},

    // Min/max range (max - min) over absolute values -> smin+smax reduce combo.
    {p+"_range",
     t+" "+p+"_range("+t+" a) {\n"
     "  int v[16]; int lo = 2000000000, hi = -2000000000;\n"
     "  for (int i = 0; i < 16; i++) {\n"
     "    int x = (int)(a * (i + 3)) ^ (i * 11); if (x < 0) x = -x; v[i] = x;\n"
     "  }\n"
     "  for (int i = 0; i < 16; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }\n"
     "  return ("+t+")(hi - lo);\n"
     "}\n",
     {0x33442255ULL}, "VectorAlgo2", opt},
  };
}

static const std::vector<RoundTripTC> kX64Vec2 = makeVec2TC("x64v2", "long", 2);
static const std::vector<RoundTripTC> kA64Vec2 = makeVec2TC("a64v2", "long", 2);
static const std::vector<RoundTripTC> kARM32Vec2 = makeVec2TC("armv2", "int", 2);

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo2, X64VectorAlgo2RT,
                         ::testing::ValuesIn(kX64Vec2), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo2, A64VectorAlgo2RT,
                         ::testing::ValuesIn(kA64Vec2), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo2, ARM32VectorAlgo2RT,
                         ::testing::ValuesIn(kARM32Vec2), rtTCName);
