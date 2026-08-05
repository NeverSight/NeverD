//===- AllPlatform_VectorAlgo5RTTests.cpp - more vectorizable algos -*- C++ -*-===//
//
// Fifth batch of realistic clang -O2 auto-vectorized algorithms used as
// high-yield lift bug probes.  Targets lift paths the first four batches did
// not stress: signed i8 dot product (smull/smlal .8h widening MAC chains),
// per-element leading-zero count reduction (clz vector), saturating narrow
// pack (sqxtn/uqxtn), per-element bit reversal (rbit), a 3-tap blur filter
// (shift + ext shuffles), simultaneous min & max reduction (sminv/smaxv /
// pminsd/pmaxsd), array interleave (zip/unpck), and signed multiply-high.
// Each runs the full binary -> lift -> MedIR -> LLVM IR -> obj -> binary
// roundtrip and compares Unicorn execution of the original vs recompiled code.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo5RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo5RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo5RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo5RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo5RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo5RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec5TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Signed i8 dot product -> smull/smlal .8h widening multiply-accumulate.
    {p+"_dotp8",
     t+" "+p+"_dotp8("+t+" a, "+t+" b) {\n"
     "  signed char x[64], y[64]; int s = 0;\n"
     "  for (int i = 0; i < 64; i++) {\n"
     "    x[i] = (signed char)((a * (i + 1)) ^ (i * 5));\n"
     "    y[i] = (signed char)((b * (i + 3)) ^ (i * 11));\n"
     "  }\n"
     "  for (int i = 0; i < 64; i++) s += (int)x[i] * (int)y[i];\n"
     "  return s;\n"
     "}\n",
     {0xCAFE1ULL, 0xBEEF2ULL}, "VectorAlgo5", opt, fl},

    // Sum of per-element leading-zero counts over a u32 array -> clz vector.
    {p+"_clzsum",
     t+" "+p+"_clzsum("+t+" a) {\n"
     "  unsigned v[32]; int s = 0;\n"
     "  for (int i = 0; i < 32; i++) v[i] = (unsigned)(a * (i + 1)) ^ (unsigned)(i * 2654435761u);\n"
     "  for (int i = 0; i < 32; i++) { unsigned x = v[i]; int c = 0;\n"
     "    while (c < 32 && !(x & 0x80000000u)) { c++; x <<= 1; } s += c; }\n"
     "  return s;\n"
     "}\n",
     {0x00010203ULL}, "VectorAlgo5", opt, fl},

    // Saturating narrow: clamp each int to signed char range then sum.
    // -> packsswb/sqxtn saturating narrow.
    {p+"_satpack",
     t+" "+p+"_satpack("+t+" a) {\n"
     "  int v[32]; int s = 0;\n"
     "  for (int i = 0; i < 32; i++) v[i] = (int)(a * (i + 1)) ^ (i * 7);\n"
     "  for (int i = 0; i < 32; i++) { int x = v[i];\n"
     "    if (x > 127) x = 127; if (x < -128) x = -128; s += x; }\n"
     "  return s;\n"
     "}\n",
     {0x12345ULL}, "VectorAlgo5", opt, fl},

    // 3-tap blur filter: out[i] = (a[i-1] + 2*a[i] + a[i+1]) >> 2, then sum.
    // Drives element-shifted (ext / palignr) shuffles + per-lane shifts.
    {p+"_filter3",
     t+" "+p+"_filter3("+t+" a) {\n"
     "  int v[34]; int s = 0;\n"
     "  for (int i = 0; i < 34; i++) v[i] = (int)(a * (i + 1)) ^ (i * 9);\n"
     "  for (int i = 1; i < 33; i++) s += (v[i-1] + 2*v[i] + v[i+1]) >> 2;\n"
     "  return s;\n"
     "}\n",
     {0x2BCDEULL}, "VectorAlgo5", opt, fl},

    // Simultaneous min & max reduction, return (max - min) -> sminv/smaxv.
    {p+"_range",
     t+" "+p+"_range("+t+" a) {\n"
     "  int v[32]; int mn = 0x7fffffff, mx = -0x7fffffff - 1;\n"
     "  for (int i = 0; i < 32; i++) v[i] = (int)(a * (i + 1)) ^ (i * 0x9E3779B1);\n"
     "  for (int i = 0; i < 32; i++) { if (v[i] < mn) mn = v[i]; if (v[i] > mx) mx = v[i]; }\n"
     "  return ("+t+")(mx - mn);\n"
     "}\n",
     {0x13572468ULL}, "VectorAlgo5", opt, fl},

    // Interleave two arrays then weighted sum -> zip/unpck.
    {p+"_interleave",
     t+" "+p+"_interleave("+t+" a, "+t+" b) {\n"
     "  int x[16], y[16], z[32]; int s = 0;\n"
     "  for (int i = 0; i < 16; i++) { x[i] = (int)(a*(i+1))^(i*3); y[i] = (int)(b*(i+1))^(i*5); }\n"
     "  for (int i = 0; i < 16; i++) { z[2*i] = x[i]; z[2*i+1] = y[i]; }\n"
     "  for (int i = 0; i < 32; i++) s += z[i] * (i + 1);\n"
     "  return s;\n"
     "}\n",
     {0xAABBULL, 0xCCDDULL}, "VectorAlgo5", opt, fl},

    // Signed multiply-high: (x*y) >> 16 per i16, then sum -> pmulhw / smull+shrn.
    {p+"_mulhis",
     t+" "+p+"_mulhis("+t+" a, "+t+" b) {\n"
     "  short x[16], y[16]; int s = 0;\n"
     "  for (int i = 0; i < 16; i++) { x[i] = (short)((a*(i+1))^(i*13)); y[i] = (short)((b*(i+2))^(i*7)); }\n"
     "  for (int i = 0; i < 16; i++) s += ((int)x[i] * (int)y[i]) >> 16;\n"
     "  return s;\n"
     "}\n",
     {0xCAFE1ULL, 0xBEEF2ULL}, "VectorAlgo5", opt, fl},

    // Per-byte nibble swap (x<<4 | x>>4) then sum -> per-lane shift + or.
    {p+"_nibswap",
     t+" "+p+"_nibswap("+t+" a) {\n"
     "  unsigned char v[64]; int s = 0;\n"
     "  for (int i = 0; i < 64; i++) v[i] = (unsigned char)((a * (i + 1)) ^ (i * 7));\n"
     "  for (int i = 0; i < 64; i++) { unsigned char x = v[i]; s += (unsigned char)((x << 4) | (x >> 4)); }\n"
     "  return s;\n"
     "}\n",
     {0x123456789ABCULL}, "VectorAlgo5", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec5 = makeVec5TC("x64v5", "long", 2, "-msse4.2");
static const std::vector<RoundTripTC> kA64Vec5 = makeVec5TC("a64v5", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec5 = makeVec5TC("armv5", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo5, X64VectorAlgo5RT,
                         ::testing::ValuesIn(kX64Vec5), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo5, A64VectorAlgo5RT,
                         ::testing::ValuesIn(kA64Vec5), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo5, ARM32VectorAlgo5RT,
                         ::testing::ValuesIn(kARM32Vec5), rtTCName);
