//===- AllPlatform_VectorAlgo4RTTests.cpp - more vectorizable algos -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Fourth batch of realistic clang -O2 auto-vectorized algorithms used as
// high-yield lift bug probes.  Exercises lift paths the first three batches did
// not: per-byte population count (SIMD shift/mask bit-trick), Q15 fixed-point
// rounding multiply (pmulhrsw / sqrdmulh / smull+shrn), abs-then-max reduction,
// XOR reduction with shuffle fold tail, masked odd-element accumulate
// (select/blend), 4x4 matrix transpose weighted sum (zip/unpck/trn), signed
// scaled arithmetic right shift, and unsigned-min reduction (pminud / uminv).
// Each runs the full binary -> lift -> MedIR -> LLVM IR -> obj -> binary
// roundtrip and compares Unicorn execution of the original vs recompiled code.
//
// x86 cases compile with -msse4.2 so clang selects the richer SSSE3/SSE4.1
// instruction set (pshufb, pmulld, pblendvb, pcmpeqq, pabsd, pminud, ...)
// instead of falling back to scalar code -- this widens lift coverage further.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo4RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo4RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo4RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo4RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo4RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo4RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec4TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Per-byte population count via SWAR bit-trick then sum.  Drives per-byte
    // SIMD shifts + masks + adds (pshufb LUT on x86 sse, vcnt on NEON).
    {p+"_pcnt",
     t+" "+p+"_pcnt("+t+" a) {\n"
     "  unsigned char v[64]; int s = 0;\n"
     "  for (int i = 0; i < 64; i++) v[i] = (unsigned char)((a * (i + 1)) ^ (i * 7));\n"
     "  for (int i = 0; i < 64; i++) {\n"
     "    unsigned char x = v[i];\n"
     "    x = x - ((x >> 1) & 0x55);\n"
     "    x = (x & 0x33) + ((x >> 2) & 0x33);\n"
     "    x = (x + (x >> 4)) & 0x0F;\n"
     "    s += x;\n"
     "  }\n"
     "  return s;\n"
     "}\n",
     {0x123456789ABCULL}, "VectorAlgo4", opt, fl},

    // i16 Q15 fixed-point rounding multiply: (x*y + 0x4000) >> 15, then sum.
    // Drives pmulhrsw / sqrdmulh / smull+rounding-shift-narrow.
    {p+"_q15",
     t+" "+p+"_q15("+t+" a, "+t+" b) {\n"
     "  short x[16], y[16]; int s = 0;\n"
     "  for (int i = 0; i < 16; i++) {\n"
     "    x[i] = (short)((a * (i + 1)) ^ (i * 13));\n"
     "    y[i] = (short)((b * (i + 2)) ^ (i * 7));\n"
     "  }\n"
     "  for (int i = 0; i < 16; i++) s += ((int)x[i] * (int)y[i] + 0x4000) >> 15;\n"
     "  return s;\n"
     "}\n",
     {0xCAFE1ULL, 0xBEEF2ULL}, "VectorAlgo4", opt, fl},

    // Maximum of absolute values over an int array -> pabsd + pmaxsd + reduce.
    {p+"_maxabs",
     t+" "+p+"_maxabs("+t+" a) {\n"
     "  int v[32]; int m = 0;\n"
     "  for (int i = 0; i < 32; i++) v[i] = (int)(a * (i + 1)) ^ (i * 0x9E3779B1);\n"
     "  for (int i = 0; i < 32; i++) { int x = v[i]; if (x < 0) x = -x; if (x > m) m = x; }\n"
     "  return ("+t+")m;\n"
     "}\n",
     {0x13572468ULL}, "VectorAlgo4", opt, fl},

    // XOR reduction over a u32 array -> pxor full-width + shuffle fold tail.
    {p+"_xorred",
     t+" "+p+"_xorred("+t+" a) {\n"
     "  unsigned v[32]; unsigned x = 0;\n"
     "  for (int i = 0; i < 32; i++) v[i] = (unsigned)(a * (i + 1)) ^ (unsigned)(i * 2654435761u);\n"
     "  for (int i = 0; i < 32; i++) x ^= v[i];\n"
     "  return (int)x;\n"
     "}\n",
     {0x0BADF00DULL}, "VectorAlgo4", opt, fl},

    // Masked accumulate: sum only the odd elements -> and-mask + select/blend.
    {p+"_condodd",
     t+" "+p+"_condodd("+t+" a) {\n"
     "  int v[32]; int s = 0;\n"
     "  for (int i = 0; i < 32; i++) v[i] = (int)(a * (i + 1)) ^ (i * 3);\n"
     "  for (int i = 0; i < 32; i++) if (v[i] & 1) s += v[i];\n"
     "  return s;\n"
     "}\n",
     {0x2468ACE0ULL}, "VectorAlgo4", opt, fl},

    // 4x4 int matrix transpose, then position-weighted sum -> zip/unpck/trn.
    // The weighting makes the transpose observable in the return value.
    {p+"_wtrans",
     t+" "+p+"_wtrans("+t+" a) {\n"
     "  int m[4][4], tr[4][4]; int s = 0;\n"
     "  for (int i = 0; i < 4; i++)\n"
     "    for (int j = 0; j < 4; j++)\n"
     "      m[i][j] = (int)(a * (i * 4 + j + 1)) ^ ((i + 1) * (j + 3));\n"
     "  for (int i = 0; i < 4; i++)\n"
     "    for (int j = 0; j < 4; j++)\n"
     "      tr[j][i] = m[i][j];\n"
     "  for (int i = 0; i < 4; i++)\n"
     "    for (int j = 0; j < 4; j++)\n"
     "      s += tr[i][j] * (i * 4 + j + 1);\n"
     "  return s;\n"
     "}\n",
     {0x33442255ULL}, "VectorAlgo4", opt, fl},

    // Signed scaled arithmetic right shift: (v*5 - 3) >> 2 per int, then sum.
    // Drives signed packed multiply + add + arithmetic (sign-propagating) shift.
    {p+"_sclshr",
     t+" "+p+"_sclshr("+t+" a) {\n"
     "  int v[32]; int s = 0;\n"
     "  for (int i = 0; i < 32; i++) v[i] = (int)(a * (i + 1)) ^ (i * 11);\n"
     "  for (int i = 0; i < 32; i++) s += (v[i] * 5 - 3) >> 2;\n"
     "  return s;\n"
     "}\n",
     {0x55667788ULL}, "VectorAlgo4", opt, fl},

    // Unsigned minimum reduction over a u32 array -> pminud / uminv.
    {p+"_umin",
     t+" "+p+"_umin("+t+" a) {\n"
     "  unsigned v[32]; unsigned m = 0xFFFFFFFFu;\n"
     "  for (int i = 0; i < 32; i++) v[i] = (unsigned)(a * (i + 1)) ^ (unsigned)(i * 40503u);\n"
     "  for (int i = 0; i < 32; i++) if (v[i] < m) m = v[i];\n"
     "  return (int)m;\n"
     "}\n",
     {0xABCDEF12ULL}, "VectorAlgo4", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec4 = makeVec4TC("x64v4", "long", 2, "-msse4.2");
static const std::vector<RoundTripTC> kA64Vec4 = makeVec4TC("a64v4", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Vec4 = makeVec4TC("armv4", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo4, X64VectorAlgo4RT,
                         ::testing::ValuesIn(kX64Vec4), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo4, A64VectorAlgo4RT,
                         ::testing::ValuesIn(kA64Vec4), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo4, ARM32VectorAlgo4RT,
                         ::testing::ValuesIn(kARM32Vec4), rtTCName);
