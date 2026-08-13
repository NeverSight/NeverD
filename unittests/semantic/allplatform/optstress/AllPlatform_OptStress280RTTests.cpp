//===- AllPlatform_OptStress280RTTests.cpp - FP + memory mix at -O0 ======//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Scalar floating point interleaved with memory (arrays / structs / unions) at
// -O0 — combines the FP subsystem (OptStress258/271) with the memory model on
// the store-everything -O0 form, where every float lives in a frame slot and
// each FP op is load / convert / op / store.  This stresses FP load/store width,
// int<->FP conversion through memory, FP fields in structs, and float/int union
// punning together.
//
//   * farr     - float array reduce: running sum + min + max via branches.
//   * fstruct  - {float,int} struct per-field RMW in a loop.
//   * fcvtmem  - int array -> float compute -> int back, all through memory.
//   * dmix2    - double array, branch on FP sign, accumulate.
//   * fhash    - float<->uint union punning used as a hash mixer.
//   * fselchain- chained FP-compare select (three-way min) over a recurrence.
//
// Integer in / integer out (FP folded to one integer return), LCG-seeded, all
// four targets, -O0.  Magnitudes bounded so (int) truncation is defined; only
// float/double + signed int casts, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress280RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress280RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress280RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress280RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress280RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress280RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress280RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress280RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress280TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // float array reduce: running sum + min + max via branches.
    {p+"_farr",
     t+" "+p+"_farr("+t+" a){ unsigned h=(unsigned)a; float buf[16]; unsigned acc=0;\n"
     "  for(int j=0;j<16;j++){ h=h*1103515245u+12345u; buf[j]=(float)((int)(h&0xffffu)-0x8000); }\n"
     "  float sum=0.0f,mn=buf[0],mx=buf[0];\n"
     "  for(int j=0;j<16;j++){ sum+=buf[j]*0.5f; if(buf[j]<mn) mn=buf[j]; if(buf[j]>mx) mx=buf[j]; }\n"
     "  acc=(unsigned)(int)(sum+mn+mx); return ("+t+")acc; }\n",
     {0x12345u}, "OptStress280", 0},

    // {float,int} struct per-field RMW in a loop.
    {p+"_fstruct",
     "struct FI{ float f; int i; };\n"
     +t+" "+p+"_fstruct("+t+" a){ unsigned h=(unsigned)a; struct FI s; s.f=1.0f; s.i=0; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    s.f=s.f*0.5f+(float)(int)(h&0x3ffu); s.i+=(int)(h>>10);\n"
     "    if(s.f>4096.0f) s.f=s.f*0.01f;\n"
     "    acc=acc*131u+(unsigned)(int)s.f+(unsigned)s.i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress280", 0},

    // int array -> float compute -> int back, all through memory.
    {p+"_fcvtmem",
     t+" "+p+"_fcvtmem("+t+" a){ unsigned h=(unsigned)a; int iv[8]; float fv[8]; unsigned acc=0;\n"
     "  for(int j=0;j<8;j++){ h=h*1103515245u+12345u; iv[j]=(int)(h&0x7fffu)-0x4000; }\n"
     "  for(int j=0;j<8;j++) fv[j]=(float)iv[j]*1.25f;\n"
     "  for(int j=0;j<8;j++) acc=acc*131u+(unsigned)(int)fv[j];\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress280", 0},

    // double array, branch on FP sign, accumulate.
    {p+"_dmix2",
     t+" "+p+"_dmix2("+t+" a){ unsigned h=(unsigned)a; double buf[8]; unsigned acc=0;\n"
     "  for(int j=0;j<8;j++){ h=h*1103515245u+12345u; buf[j]=(double)((int)(h&0xffffu)-0x8000)*0.1; }\n"
     "  double s=0.0;\n"
     "  for(int j=0;j<8;j++){ if(buf[j]>0.0) s+=buf[j]; else s-=buf[j]*0.5; }\n"
     "  acc=(unsigned)(int)(s*4.0); return ("+t+")acc; }\n",
     {0x45678u}, "OptStress280", 0},

    // float<->uint union punning used as a hash mixer.
    {p+"_fhash",
     t+" "+p+"_fhash("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    union{ float f; unsigned u; } w; w.u=(h&0x3fffffffu)|0x3f000000u; float f=w.f*1.5f+1.0f; w.f=f;\n"
     "    acc=acc*131u+(w.u>>11); }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress280", 0},

    // chained FP-compare select (three-way min) over a recurrence.
    {p+"_fselchain",
     t+" "+p+"_fselchain("+t+" a){ unsigned h=(unsigned)a; float acc=0.0f;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)(h&0x3ffu), y=(float)(int)((h>>10)&0x3ffu), z=(float)(int)((h>>20)&0x3ffu);\n"
     "    float m=(x<y)?((x<z)?x:z):((y<z)?y:z);\n"
     "    acc=acc*0.5f+m; }\n"
     "  return ("+t+")(int)acc; }\n",
     {0x6789Au}, "OptStress280", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress280TC("x64o280", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress280TC("x86o280", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress280TC("a64o280", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress280TC("armo280", "int");

INSTANTIATE_TEST_SUITE_P(OptStress280, X64OptStress280RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress280, X86OptStress280RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress280, A64OptStress280RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress280, ARM32OptStress280RT, ::testing::ValuesIn(kARM), rtTCName);
