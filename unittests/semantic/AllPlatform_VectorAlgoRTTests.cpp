//===- AllPlatform_VectorAlgoRTTests.cpp - vectorizable algo RT --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Realistic algorithms that clang auto-vectorizes at -O2.  These are
// high-yield bug probes: the vectorized forms exercise per-lane NEON/SSE
// lifting (horizontal reductions, widening multiply-add, saturating ops,
// per-lane min/max/abs, variable shifts) which historically had the most
// lift gaps.  Each test runs the full binary -> lift -> MedIR -> LLVM IR ->
// obj -> binary roundtrip and compares Unicorn execution of the original vs
// the recompiled code.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgoRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgoRT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgoRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgoRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgoRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgoRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

// `T` is the integer return/arg type (long for 64-bit arches, int for ARM32).
// Array fills avoid shift-by-count UB by masking the shift amount to 31, which
// is valid for both 32- and 64-bit `T`.
static std::vector<RoundTripTC> makeVecTC(const char *prefix, const char *T,
                                          int opt) {
  std::string p = prefix, t = T;
  return {
    // Horizontal sum of a derived 16-element int array.  Vectorizes to a
    // packed add loop + horizontal reduction (paddd/addv/vadd + reduce).
    {p+"_arrsum",
     t+" "+p+"_arrsum("+t+" a) {\n"
     "  int v[16]; "+t+" s = 0;\n"
     "  for (int i = 0; i < 16; i++) v[i] = (int)(a * (i + 1)) ^ (i * 7);\n"
     "  for (int i = 0; i < 16; i++) s += v[i];\n"
     "  return s;\n"
     "}\n",
     {0x123456789ABCDEFULL}, "VectorAlgo", opt},

    // Max reduction over a derived array -> smax/pmaxsd/vmax + reduce.
    {p+"_arrmax",
     t+" "+p+"_arrmax("+t+" a) {\n"
     "  int v[16]; int m = -2000000000;\n"
     "  for (int i = 0; i < 16; i++) v[i] = (int)(a * (i + 3)) ^ (i * 13);\n"
     "  for (int i = 0; i < 16; i++) if (v[i] > m) m = v[i];\n"
     "  return m;\n"
     "}\n",
     {0xCAFEBABEULL}, "VectorAlgo", opt},

    // Min reduction over absolute values -> abs + smin + reduce.
    {p+"_absmin",
     t+" "+p+"_absmin("+t+" a) {\n"
     "  int v[16]; int m = 2000000000;\n"
     "  for (int i = 0; i < 16; i++) {\n"
     "    int x = (int)(a * (i + 5)) ^ (i * 11);\n"
     "    if (x < 0) x = -x;\n"
     "    v[i] = x;\n"
     "  }\n"
     "  for (int i = 0; i < 16; i++) if (v[i] < m) m = v[i];\n"
     "  return m;\n"
     "}\n",
     {0x0F1E2D3C4B5A6978ULL}, "VectorAlgo", opt},

    // Sum of absolute differences -> psadbw / sabd-style.
    {p+"_sad",
     t+" "+p+"_sad("+t+" a, "+t+" b) {\n"
     "  int x[16], y[16]; "+t+" s = 0;\n"
     "  for (int i = 0; i < 16; i++) {\n"
     "    x[i] = (int)((a >> ((i * 4) & 31)) & 0xFF);\n"
     "    y[i] = (int)((b >> ((i * 4) & 31)) & 0xFF);\n"
     "  }\n"
     "  for (int i = 0; i < 16; i++) {\n"
     "    int d = x[i] - y[i]; if (d < 0) d = -d; s += d;\n"
     "  }\n"
     "  return s;\n"
     "}\n",
     {0x1122334455667788ULL, 0x8877665544332211ULL}, "VectorAlgo", opt},

    // Widening multiply-accumulate over two derived arrays -> mla/pmaddwd.
    {p+"_wsum",
     t+" "+p+"_wsum("+t+" a) {\n"
     "  short x[16]; "+t+" s = 0;\n"
     "  for (int i = 0; i < 16; i++) x[i] = (short)((a * (i + 1)) ^ (i * 3));\n"
     "  for (int i = 0; i < 16; i++) s += (int)x[i] * (int)x[i];\n"
     "  return s;\n"
     "}\n",
     {0xDEADBEEFULL}, "VectorAlgo", opt},

    // Total population count over the bytes of a derived array -> cnt/popcnt.
    {p+"_popcnt",
     t+" "+p+"_popcnt("+t+" a) {\n"
     "  unsigned char v[32]; int s = 0;\n"
     "  for (int i = 0; i < 32; i++) v[i] = (unsigned char)((a * (i + 7)) ^ (i * 5));\n"
     "  for (int i = 0; i < 32; i++) { unsigned char b = v[i];\n"
     "    while (b) { s += b & 1; b >>= 1; } }\n"
     "  return s;\n"
     "}\n",
     {0x123456789ABCDEFULL}, "VectorAlgo", opt},

    // Clamp every element to [-50, 50] then sum -> smin+smax (per-lane).
    {p+"_clampsum",
     t+" "+p+"_clampsum("+t+" a) {\n"
     "  int v[16]; "+t+" s = 0;\n"
     "  for (int i = 0; i < 16; i++) v[i] = (int)(a * (i + 2)) ^ (i * 9);\n"
     "  for (int i = 0; i < 16; i++) {\n"
     "    int x = v[i]; if (x < -50) x = -50; if (x > 50) x = 50; s += x;\n"
     "  }\n"
     "  return s;\n"
     "}\n",
     {0x55AA55AA55AA55AAULL}, "VectorAlgo", opt},

    // Float dot product of two derived arrays -> fmul/fadd/faddp/fma.
    {p+"_fdot",
     t+" "+p+"_fdot("+t+" a, "+t+" b) {\n"
     "  float x[8], y[8]; float s = 0.0f;\n"
     "  for (int i = 0; i < 8; i++) {\n"
     "    x[i] = (float)(int)((a >> ((i * 4) & 31)) & 0xF) - 8.0f;\n"
     "    y[i] = (float)(int)((b >> ((i * 4) & 31)) & 0xF) - 8.0f;\n"
     "  }\n"
     "  for (int i = 0; i < 8; i++) s += x[i] * y[i];\n"
     "  return ("+t+")s;\n"
     "}\n",
     {0x123456ULL, 0x654321ULL}, "VectorAlgo", opt},
  };
}

static const std::vector<RoundTripTC> kX64Vec = makeVecTC("x64va", "long", 2);
static const std::vector<RoundTripTC> kA64Vec = makeVecTC("a64va", "long", 2);
static const std::vector<RoundTripTC> kARM32Vec = makeVecTC("armva", "int", 2);

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo, X64VectorAlgoRT,
                         ::testing::ValuesIn(kX64Vec), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo, A64VectorAlgoRT,
                         ::testing::ValuesIn(kA64Vec), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo, ARM32VectorAlgoRT,
                         ::testing::ValuesIn(kARM32Vec), rtTCName);
