//===- AllPlatform_FloatAlgoRTTests.cpp - scalar FP algorithms --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Scalar floating-point algorithms exercised end-to-end.  clang -O2 lowers
// these to scalar FP instructions whose lift has historically been fragile:
//   - FP compare -> conditional select (minss/maxss, fcmp+fcsel, vcmp+vsel):
//     a flag-analog path distinct from the integer flag optimizer
//   - int<->float and float<->double conversions (cvt*, scvtf/fcvtzs, vcvt)
//   - sign-bit manipulation (fabs/fneg/copysign = bit AND/OR/XOR on FP bits)
//   - float vs double width confusion (movss/movsd, S vs D registers)
//
// The fixture reads the INTEGER return register (RAX/X0/R0), so every function
// folds its FP state into an integer.  Inputs are bounded and NaN-free so the
// final float->int conversion stays in range and is deterministic; the original
// (clang) and recompiled (NeverD) machine code run the same IEEE operations, so
// the integer return must match bit-for-bit.  No libm calls: sqrt inlines as a
// hardware fsqrt under -O2 -fno-math-errno, and no transcendental builtins are
// used.  Returns fold to 32-bit-sensitive values for the ARM32 path.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FloatAlgoRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FloatAlgoRT, Verify) { roundTripX64(GetParam()); }

class A64FloatAlgoRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FloatAlgoRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32FloatAlgoRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FloatAlgoRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeFloatTC(const char *prefix, const char *T,
                                            int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Horner polynomial evaluation: pure fmul/fadd dependency chain (float).
    {p+"_horner",
     t+" "+p+"_horner("+t+" a) {\n"
     "  float acc=0.0f;\n"
     "  for (int i=0;i<128;i++){\n"
     "    float x=(float)((int)(a*(i+1))%97)*0.125f;\n"
     "    float r=0.0f;\n"
     "    for (int k=0;k<6;k++) r=r*x+(float)(k+1);\n"
     "    acc+=r; }\n"
     "  return ("+t+")(int)(acc);\n"
     "}\n",
     {0x1234567ULL}, "FloatAlgo", opt, fl},

    // Simultaneous float min/max reduction: two fcmp+fcsel per element.
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a) {\n"
     "  float mn=1e30f, mx=-1e30f;\n"
     "  for (int i=0;i<200;i++){\n"
     "    float x=(float)((int)(a*(i*7+1))%2003 - 1000);\n"
     "    if (x<mn) mn=x; if (x>mx) mx=x; }\n"
     "  return ("+t+")(int)(mx-mn);\n"
     "}\n",
     {0x2233445ULL}, "FloatAlgo", opt, fl},

    // Two-sided float clamp in a loop (compare+select chain), summed.
    {p+"_clamp",
     t+" "+p+"_clamp("+t+" a) {\n"
     "  float s=0.0f;\n"
     "  for (int i=0;i<200;i++){\n"
     "    float x=(float)((int)(a*(i+1))%4000 - 2000)*0.25f;\n"
     "    float lo=-150.0f, hi=275.0f;\n"
     "    if (x<lo) x=lo; if (x>hi) x=hi; s+=x; }\n"
     "  return ("+t+")(int)s;\n"
     "}\n",
     {0x3344556ULL}, "FloatAlgo", opt, fl},

    // Compare-swap sort of 4 floats (Batcher network): dense fcmp+fcsel.
    {p+"_sort4",
     t+" "+p+"_sort4("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=0;i<128;i++){\n"
     "    float v0=(float)((int)(a*(i+1))%211 - 105);\n"
     "    float v1=(float)((int)(a*(i+2))%211 - 105);\n"
     "    float v2=(float)((int)(a*(i+3))%211 - 105);\n"
     "    float v3=(float)((int)(a*(i+5))%211 - 105);\n"
     "    float t0,t1;\n"
     "    if(v0>v1){t0=v0;v0=v1;v1=t0;} if(v2>v3){t0=v2;v2=v3;v3=t0;}\n"
     "    if(v0>v2){t0=v0;v0=v2;v2=t0;} if(v1>v3){t1=v1;v1=v3;v3=t1;}\n"
     "    if(v1>v2){t0=v1;v1=v2;v2=t0;}\n"
     "    acc += (int)(v0 + v1*2 + v2*3 + v3*4); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "FloatAlgo", opt, fl},

    // int->float dot product accumulation, scaled back to int.
    {p+"_dotacc",
     t+" "+p+"_dotacc("+t+" a) {\n"
     "  float acc=0.0f;\n"
     "  for (int i=0;i<256;i++){\n"
     "    float x=(float)((int)(a*(i+1))%127)*0.5f;\n"
     "    float y=(float)((int)(a*(i+3))%63)*0.25f;\n"
     "    acc += x*y; }\n"
     "  return ("+t+")(int)acc;\n"
     "}\n",
     {0x5566778ULL}, "FloatAlgo", opt, fl},

    // double-precision Horner + sqrt (fsqrt scalar), folded to int.
    {p+"_dsqrt",
     t+" "+p+"_dsqrt("+t+" a) {\n"
     "  double acc=0.0;\n"
     "  for (int i=0;i<128;i++){\n"
     "    double x=(double)((int)(a*(i+1))%5000)+1.0;\n"
     "    double r=x*0.5 + 3.0;\n"
     "    acc += __builtin_sqrt(r); }\n"
     "  return ("+t+")(int)acc;\n"
     "}\n",
     {0x6677889ULL}, "FloatAlgo", opt, "-fno-math-errno"},

    // copysign / fabs / neg combinations: sign-bit manipulation on FP bits.
    {p+"_sign",
     t+" "+p+"_sign("+t+" a) {\n"
     "  float s=0.0f;\n"
     "  for (int i=0;i<200;i++){\n"
     "    float x=(float)((int)(a*(i*5+1))%777 - 388)*0.5f;\n"
     "    float y=(float)((int)(a*(i*3+2))%51 - 25);\n"
     "    float z=__builtin_copysignf(__builtin_fabsf(x), y);\n"
     "    s += (z<0)? -z : z*0.5f; s += -x; }\n"
     "  return ("+t+")(int)s;\n"
     "}\n",
     {0x778899AULL}, "FloatAlgo", opt, fl},

    // float<->double mixed conversions in a chain (cvtss2sd/cvtsd2ss).
    {p+"_mixconv",
     t+" "+p+"_mixconv("+t+" a) {\n"
     "  double acc=0.0;\n"
     "  for (int i=0;i<200;i++){\n"
     "    float f=(float)((int)(a*(i+1))%333)*0.125f;\n"
     "    double d=(double)f * 1.5;\n"
     "    float g=(float)d + 0.25f;\n"
     "    acc += (double)g; }\n"
     "  return ("+t+")(int)acc;\n"
     "}\n",
     {0x88990ABULL}, "FloatAlgo", opt, fl},

    // FP comparison results combined with boolean ops feeding a counter.
    {p+"_cmpcomb",
     t+" "+p+"_cmpcomb("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=0;i<256;i++){\n"
     "    float x=(float)((int)(a*(i+1))%200 - 100);\n"
     "    float y=(float)((int)(a*(i+2))%200 - 100);\n"
     "    if ((x<y) && (x*x < 2500.0f)) acc+=1;\n"
     "    if ((x>y) || (y < -50.0f)) acc+=2;\n"
     "    if ((x<=y) != (x>=y)) acc+=4; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x99AABBCULL}, "FloatAlgo", opt, fl},

    // float->int rounding via cast (truncation toward zero) + accumulate; also
    // negative values to exercise signed conversion direction.
    {p+"_trunc",
     t+" "+p+"_trunc("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=0;i<256;i++){\n"
     "    float x=(float)((int)(a*(i*9+1))%9999 - 5000)*0.3333f;\n"
     "    acc += (int)x; acc += (int)(x*2.0f) - (int)(x*0.5f); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1020304ULL}, "FloatAlgo", opt, fl},

    // unsigned int -> float conversion (scvtf vs ucvtf; cvtsi2ss zero-extend).
    {p+"_ucvt",
     t+" "+p+"_ucvt("+t+" a) {\n"
     "  float acc=0.0f;\n"
     "  for (int i=0;i<256;i++){\n"
     "    unsigned u=(unsigned)(a*(i+1)) | 0x80000000u;\n"
     "    float x=(float)u * (1.0f/65536.0f);\n"
     "    acc += x - (float)(int)(x); }\n"
     "  return ("+t+")(int)(acc*100.0f);\n"
     "}\n",
     {0x2030405ULL}, "FloatAlgo", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Float =
    makeFloatTC("x64fa", "long", 2, "");
static const std::vector<RoundTripTC> kA64Float =
    makeFloatTC("a64fa", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Float =
    makeFloatTC("armfa", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(FloatAlgo, X64FloatAlgoRT,
                         ::testing::ValuesIn(kX64Float), rtTCName);
INSTANTIATE_TEST_SUITE_P(FloatAlgo, A64FloatAlgoRT,
                         ::testing::ValuesIn(kA64Float), rtTCName);
INSTANTIATE_TEST_SUITE_P(FloatAlgo, ARM32FloatAlgoRT,
                         ::testing::ValuesIn(kARM32Float), rtTCName);
