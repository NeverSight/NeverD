//===- AllPlatform_VectorAlgo8RTTests.cpp - FP vectorizable algos -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Eighth batch of realistic clang -O2 algorithms used as high-yield lift bug
// probes.  Unlike the earlier seven (all integer), this batch targets
// FLOATING-POINT element-wise vectorization: per-lane FMUL/FADD/FSUB,
// FMIN/FMAX, FABS, FP compare + select, FDIV and FSQRT.  Each algorithm stores
// float results back into an array (which clang -O2 turns into packed FP ops:
// mulps/addps on x86, fmul/fadd v.4s on AArch64, vmul.f32/vadd.f32 on ARM32),
// then folds the array with an INTEGER reduction so the return value is exact.
//
// To keep results deterministic and free of rounding noise, every float value
// is a small integer (< 2^24) so it is represented exactly; division/sqrt
// results are scaled+truncated and are bit-identical between original and
// recompiled code because both run the same IEEE hardware instructions.
//
// Each runs the full binary -> lift -> MedIR -> LLVM IR -> obj -> binary
// roundtrip and compares Unicorn execution of the original vs recompiled code.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo8RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo8RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo8RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo8RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo8RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo8RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec8TC(const char *prefix, const char *T,
                                           int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Per-lane FMUL + FADD: r[i] = v[i]*w[i] + v[i] -> mulps + addps.
    {p+"_fdot",
     t+" "+p+"_fdot("+t+" a) {\n"
     "  float v[64], w[64], r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ v[i]=(float)((int)(a*(i+1))&0xFF);\n"
     "    w[i]=(float)((int)(a*(i+3))&0x7F); }\n"
     "  for (int i=0;i<64;i++) r[i] = v[i]*w[i] + v[i];\n"
     "  for (int i=0;i<64;i++) s ^= (int)r[i] + i;\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo8", opt, fl},

    // Per-lane multiply-add (FMA idiom): r[i] = v[i]*w[i] + u[i].
    {p+"_fmla",
     t+" "+p+"_fmla("+t+" a) {\n"
     "  float v[64], w[64], u[64], r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ v[i]=(float)((int)(a*(i+1))&0xFF);\n"
     "    w[i]=(float)((int)(a*(i+3))&0x7F); u[i]=(float)((int)(a*(i+5))&0x3F); }\n"
     "  for (int i=0;i<64;i++) r[i] = v[i]*w[i] + u[i];\n"
     "  for (int i=0;i<64;i++) s += (int)r[i];\n"
     "  return s;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo8", opt, fl},

    // Per-lane FSUB scaled: r[i] = (v[i]-w[i])*2 -> subps + mulps.
    {p+"_fsub",
     t+" "+p+"_fsub("+t+" a) {\n"
     "  float v[64], w[64], r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ v[i]=(float)((int)(a*(i+1))&0x1FF);\n"
     "    w[i]=(float)((int)(a*(i+2))&0xFF); }\n"
     "  for (int i=0;i<64;i++) r[i] = (v[i]-w[i])*2.0f;\n"
     "  for (int i=0;i<64;i++) s += (int)r[i];\n"
     "  return s;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo8", opt, fl},

    // Per-lane FMAX/FMIN: r[i] = max(v,w) - min(v,w) = |v-w|.
    {p+"_fminmax",
     t+" "+p+"_fminmax("+t+" a) {\n"
     "  float v[64], w[64], r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ v[i]=(float)((int)(a*(i+1))&0xFF);\n"
     "    w[i]=(float)((int)(a*(i+7))&0xFF); }\n"
     "  for (int i=0;i<64;i++){ float mx=v[i]>w[i]?v[i]:w[i];\n"
     "    float mn=v[i]<w[i]?v[i]:w[i]; r[i]=mx-mn; }\n"
     "  for (int i=0;i<64;i++) s += (int)r[i];\n"
     "  return s;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo8", opt, fl},

    // Per-lane FABS: r[i] = fabs(v[i]-w[i]) -> andps mask.
    {p+"_fabs",
     t+" "+p+"_fabs("+t+" a) {\n"
     "  float v[64], w[64], r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ v[i]=(float)((int)(a*(i+1))&0x1FF);\n"
     "    w[i]=(float)((int)(a*(i+3))&0x1FF); }\n"
     "  for (int i=0;i<64;i++){ float d=v[i]-w[i]; r[i]=d<0?-d:d; }\n"
     "  for (int i=0;i<64;i++) s += (int)r[i];\n"
     "  return s;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo8", opt, fl},

    // Per-lane FP compare + select: r[i] = (v<w) ? v+w : v-w.
    {p+"_fcmpsel",
     t+" "+p+"_fcmpsel("+t+" a) {\n"
     "  float v[64], w[64], r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ v[i]=(float)((int)(a*(i+1))&0xFF);\n"
     "    w[i]=(float)((int)(a*(i+5))&0xFF); }\n"
     "  for (int i=0;i<64;i++) r[i] = (v[i]<w[i]) ? (v[i]+w[i]) : (v[i]-w[i]);\n"
     "  for (int i=0;i<64;i++) s ^= (int)r[i] + i;\n"
     "  return s;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo8", opt, fl},

    // Per-lane FDIV: r[i] = v[i]/w[i], scaled (IEEE divide is deterministic).
    {p+"_fdiv",
     t+" "+p+"_fdiv("+t+" a) {\n"
     "  float v[64], w[64], r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++){ v[i]=(float)((int)(a*(i+1))&0x3FF);\n"
     "    w[i]=(float)(((int)(a*(i+3))&0x7F)|1); }\n"
     "  for (int i=0;i<64;i++) r[i] = v[i]/w[i];\n"
     "  for (int i=0;i<64;i++) s += (int)(r[i]*8.0f);\n"
     "  return s;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo8", opt, fl},

    // Per-lane FSQRT: r[i] = sqrt(v[i]), scaled (IEEE sqrt is deterministic).
    {p+"_fsqrt",
     t+" "+p+"_fsqrt("+t+" a) {\n"
     "  float v[64], r[64]; int s = 0;\n"
     "  for (int i=0;i<64;i++) v[i]=(float)((int)(a*(i+1))&0xFFF);\n"
     "  for (int i=0;i<64;i++) r[i] = __builtin_sqrtf(v[i]);\n"
     "  for (int i=0;i<64;i++) s += (int)(r[i]*16.0f);\n"
     "  return s;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo8", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec8 =
    makeVec8TC("x64v8", "long", 2, "-msse4.2 -fno-math-errno -fno-trapping-math");
static const std::vector<RoundTripTC> kA64Vec8 =
    makeVec8TC("a64v8", "long", 2, "-fno-math-errno -fno-trapping-math");
static const std::vector<RoundTripTC> kARM32Vec8 =
    makeVec8TC("armv8v", "int", 2, "-fno-math-errno -fno-trapping-math");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo8, X64VectorAlgo8RT,
                         ::testing::ValuesIn(kX64Vec8), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo8, A64VectorAlgo8RT,
                         ::testing::ValuesIn(kA64Vec8), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo8, ARM32VectorAlgo8RT,
                         ::testing::ValuesIn(kARM32Vec8), rtTCName);
