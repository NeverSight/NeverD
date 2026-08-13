//===- AllPlatform_OptStress240RTTests.cpp - FP arith/compare/convert ====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Floating-point arithmetic, comparisons, selects and int<->FP conversions,
// folded back to an integer return so the harness can compare it in the GP
// return register.  FP lift has a long bug history (#502 FP-return routing,
// FP flag folding); this exercises scalar + auto-vectorized float math, the
// ordered/unordered compare flag path, and `(int)` truncation on every
// platform.  Only 32-bit int<->float/double conversions are used so i386 and
// ARM32 stay free of the 64-bit FP-conversion libcalls (`__fixdfdi` etc.).
//
//   * fsum    - float array sum, scaled to int.
//   * fdot    - float dot product.
//   * fcmpsel - branch/select on float comparisons (ordered).
//   * fminmax - float min/max reduction, range to int.
//   * fhorner - polynomial (Horner) evaluation.
//   * fcvtrt  - int -> float -> arithmetic -> int round trip.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress240RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress240RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress240RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress240RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress240RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress240RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress240RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress240RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress240TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Float array sum, scaled to int.
    {p+"_fsum",
     t+" "+p+"_fsum("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<70;it++){ h=h*1103515245u+12345u;\n"
     "    float x[16]; for(int k=0;k<16;k++){ h=h*1664525u+1013904223u; x[k]=(float)(int)(h>>8)/65536.0f; }\n"
     "    float s=0; for(int k=0;k<16;k++) s+=x[k];\n"
     "    acc=acc*131u+(unsigned)(int)(s*16.0f)+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress240", 2},

    // Float dot product.
    {p+"_fdot",
     t+" "+p+"_fdot("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<70;it++){ h=h*1103515245u+12345u;\n"
     "    float u[16],v[16];\n"
     "    for(int k=0;k<16;k++){ h=h*1664525u+1013904223u; u[k]=(float)(short)(h>>8); v[k]=(float)(short)(h>>20); }\n"
     "    float d=0; for(int k=0;k<16;k++) d+=u[k]*v[k];\n"
     "    acc=acc*131u+(unsigned)(int)(d/4096.0f)+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress240", 2},

    // Branch/select on float comparisons (ordered).
    {p+"_fcmpsel",
     t+" "+p+"_fcmpsel("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<70;it++){ h=h*1103515245u+12345u;\n"
     "    double s=0;\n"
     "    for(int k=0;k<16;k++){ h=h*1664525u+1013904223u; double x=(double)(int)(h>>9)/32768.0;\n"
     "      s += (x>1.0)? x*2.0 : (x<-1.0? x*0.5 : x+0.25); }\n"
     "    acc=acc*131u+(unsigned)(int)(s*64.0)+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress240", 2},

    // Float min/max reduction, range scaled to int.
    {p+"_fminmax",
     t+" "+p+"_fminmax("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<70;it++){ h=h*1103515245u+12345u;\n"
     "    float mn=1e30f,mx=-1e30f;\n"
     "    for(int k=0;k<16;k++){ h=h*1664525u+1013904223u; float x=(float)(int)h/100000.0f;\n"
     "      if(x<mn)mn=x; if(x>mx)mx=x; }\n"
     "    acc=acc*131u+(unsigned)(int)((mx-mn)*8.0f)+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress240", 2},

    // Polynomial (Horner) evaluation.
    {p+"_fhorner",
     t+" "+p+"_fhorner("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<70;it++){ h=h*1103515245u+12345u;\n"
     "    double x=(double)(int)(h>>16)/32768.0; double r=0;\n"
     "    double c[6]={1.5,-2.25,0.75,-0.125,3.0,-0.5};\n"
     "    for(int k=0;k<6;k++) r=r*x+c[k];\n"
     "    acc=acc*131u+(unsigned)(int)(r*256.0)+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress240", 2},

    // int -> float -> arithmetic -> int round trip.
    {p+"_fcvtrt",
     t+" "+p+"_fcvtrt("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<70;it++){ h=h*1103515245u+12345u;\n"
     "    int s=0;\n"
     "    for(int k=0;k<16;k++){ h=h*1664525u+1013904223u; int v=(int)(h>>12);\n"
     "      float f=(float)v*1.5f+0.5f; s+=(int)f; }\n"
     "    acc=acc*131u+(unsigned)s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress240", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress240TC("x64o240", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress240TC("x86o240", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress240TC("a64o240", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress240TC("armo240", "int");

INSTANTIATE_TEST_SUITE_P(OptStress240, X64OptStress240RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress240, X86OptStress240RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress240, A64OptStress240RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress240, ARM32OptStress240RT, ::testing::ValuesIn(kARM), rtTCName);
