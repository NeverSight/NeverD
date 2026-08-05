//===- AllPlatform_VectorAlgo10RTTests.cpp - double FP algos ----*- C++ -*-===//
//
// Tenth batch of clang -O2 algorithm probes.  VectorAlgo8 was float32; this one
// targets DOUBLE-precision (f64) element-wise vectorization: per-lane
// FMUL/FADD/FSUB, FMIN/FMAX, FABS, FP compare + select, FDIV and FSQRT on
// arrays of double.  clang lowers these to mulpd/addpd/divpd/sqrtpd (x86),
// fmul/fadd/fdiv v.2d + fsqrt v.2d (AArch64), and — since NEON has no f64 vector
// ops — VFP scalar chains vmul.f64/vadd.f64/vdiv.f64/vsqrt.f64 (ARM32).
//
// Every double holds a small integer (< 2^53, exact); division/sqrt results are
// scaled+truncated and are bit-identical between original and recompiled code
// because both run the same IEEE hardware instructions.  Results fold to an
// exact integer return value.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64VectorAlgo10RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64VectorAlgo10RT, Verify) { roundTripX64(GetParam()); }

class A64VectorAlgo10RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64VectorAlgo10RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32VectorAlgo10RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32VectorAlgo10RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeVec10TC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Per-lane FMUL + FADD: r[i] = v[i]*w[i] + v[i].
    {p+"_dfdot",
     t+" "+p+"_dfdot("+t+" a) {\n"
     "  double v[48], w[48], r[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++){ v[i]=(double)((int)(a*(i+1))&0xFFFF);\n"
     "    w[i]=(double)((int)(a*(i+3))&0x7F); }\n"
     "  for (int i=0;i<48;i++) r[i] = v[i]*w[i] + v[i];\n"
     "  for (int i=0;i<48;i++) s ^= (int)r[i] + i;\n"
     "  return s;\n"
     "}\n",
     {0x1234567ULL}, "VectorAlgo10", opt, fl},

    // Per-lane multiply-add: r[i] = v[i]*w[i] + u[i].
    {p+"_dfmla",
     t+" "+p+"_dfmla("+t+" a) {\n"
     "  double v[48], w[48], u[48], r[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++){ v[i]=(double)((int)(a*(i+1))&0xFFF);\n"
     "    w[i]=(double)((int)(a*(i+3))&0x7F); u[i]=(double)((int)(a*(i+5))&0x3F); }\n"
     "  for (int i=0;i<48;i++) r[i] = v[i]*w[i] + u[i];\n"
     "  for (int i=0;i<48;i++) s += (int)r[i];\n"
     "  return s;\n"
     "}\n",
     {0x2233445ULL}, "VectorAlgo10", opt, fl},

    // Per-lane FSUB scaled: r[i] = (v[i]-w[i])*2.
    {p+"_dfsub",
     t+" "+p+"_dfsub("+t+" a) {\n"
     "  double v[48], w[48], r[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++){ v[i]=(double)((int)(a*(i+1))&0x1FFFF);\n"
     "    w[i]=(double)((int)(a*(i+2))&0xFFFF); }\n"
     "  for (int i=0;i<48;i++) r[i] = (v[i]-w[i])*2.0;\n"
     "  for (int i=0;i<48;i++) s += (int)r[i];\n"
     "  return s;\n"
     "}\n",
     {0x3344556ULL}, "VectorAlgo10", opt, fl},

    // Per-lane FMAX/FMIN: r[i] = max(v,w) - min(v,w) = |v-w|.
    {p+"_dfminmax",
     t+" "+p+"_dfminmax("+t+" a) {\n"
     "  double v[48], w[48], r[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++){ v[i]=(double)((int)(a*(i+1))&0xFFFF);\n"
     "    w[i]=(double)((int)(a*(i+7))&0xFFFF); }\n"
     "  for (int i=0;i<48;i++){ double mx=v[i]>w[i]?v[i]:w[i];\n"
     "    double mn=v[i]<w[i]?v[i]:w[i]; r[i]=mx-mn; }\n"
     "  for (int i=0;i<48;i++) s += (int)r[i];\n"
     "  return s;\n"
     "}\n",
     {0x4455667ULL}, "VectorAlgo10", opt, fl},

    // Per-lane FABS: r[i] = fabs(v[i]-w[i]) -> andpd mask.
    {p+"_dfabs",
     t+" "+p+"_dfabs("+t+" a) {\n"
     "  double v[48], w[48], r[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++){ v[i]=(double)((int)(a*(i+1))&0x1FFFF);\n"
     "    w[i]=(double)((int)(a*(i+3))&0x1FFFF); }\n"
     "  for (int i=0;i<48;i++){ double d=v[i]-w[i]; r[i]=d<0?-d:d; }\n"
     "  for (int i=0;i<48;i++) s += (int)r[i];\n"
     "  return s;\n"
     "}\n",
     {0x5566778ULL}, "VectorAlgo10", opt, fl},

    // Per-lane FP compare + select: r[i] = (v<w) ? v+w : v-w.
    {p+"_dfcmpsel",
     t+" "+p+"_dfcmpsel("+t+" a) {\n"
     "  double v[48], w[48], r[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++){ v[i]=(double)((int)(a*(i+1))&0xFFFF);\n"
     "    w[i]=(double)((int)(a*(i+5))&0xFFFF); }\n"
     "  for (int i=0;i<48;i++) r[i] = (v[i]<w[i]) ? (v[i]+w[i]) : (v[i]-w[i]);\n"
     "  for (int i=0;i<48;i++) s ^= (int)r[i] + i;\n"
     "  return s;\n"
     "}\n",
     {0x6677889ULL}, "VectorAlgo10", opt, fl},

    // Per-lane FDIV: r[i] = v[i]/w[i], scaled (IEEE divide is deterministic).
    {p+"_dfdiv",
     t+" "+p+"_dfdiv("+t+" a) {\n"
     "  double v[48], w[48], r[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++){ v[i]=(double)((int)(a*(i+1))&0xFFFFF);\n"
     "    w[i]=(double)(((int)(a*(i+3))&0x7F)|1); }\n"
     "  for (int i=0;i<48;i++) r[i] = v[i]/w[i];\n"
     "  for (int i=0;i<48;i++) s += (int)(r[i]*8.0);\n"
     "  return s;\n"
     "}\n",
     {0x778899AULL}, "VectorAlgo10", opt, fl},

    // Per-lane FSQRT: r[i] = sqrt(v[i]), scaled (IEEE sqrt is deterministic).
    {p+"_dfsqrt",
     t+" "+p+"_dfsqrt("+t+" a) {\n"
     "  double v[48], r[48]; int s = 0;\n"
     "  for (int i=0;i<48;i++) v[i]=(double)((int)(a*(i+1))&0xFFFFF);\n"
     "  for (int i=0;i<48;i++) r[i] = __builtin_sqrt(v[i]);\n"
     "  for (int i=0;i<48;i++) s += (int)(r[i]*16.0);\n"
     "  return s;\n"
     "}\n",
     {0x88990ABULL}, "VectorAlgo10", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Vec10 =
    makeVec10TC("x64v10", "long", 2, "-msse4.2 -fno-math-errno -fno-trapping-math");
static const std::vector<RoundTripTC> kA64Vec10 =
    makeVec10TC("a64v10", "long", 2, "-fno-math-errno -fno-trapping-math");
static const std::vector<RoundTripTC> kARM32Vec10 =
    makeVec10TC("armv10v", "int", 2, "-fno-math-errno -fno-trapping-math");

// clang-format on

INSTANTIATE_TEST_SUITE_P(VectorAlgo10, X64VectorAlgo10RT,
                         ::testing::ValuesIn(kX64Vec10), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo10, A64VectorAlgo10RT,
                         ::testing::ValuesIn(kA64Vec10), rtTCName);
INSTANTIATE_TEST_SUITE_P(VectorAlgo10, ARM32VectorAlgo10RT,
                         ::testing::ValuesIn(kARM32Vec10), rtTCName);
