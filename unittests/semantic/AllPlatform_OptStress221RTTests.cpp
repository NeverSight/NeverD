//===- AllPlatform_OptStress221RTTests.cpp - mixed-class struct-arg ABI ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// By-value struct arguments whose fields fall in DIFFERENT classification
// classes -- the x86-64 SysV eightbyte split (INTEGER vs SSE), AArch64's
// "composite <=16B in GP regs vs HFA in V regs" rule, and the i386/ARM32
// all-on-stack path.  OptStress203 only covered HOMOGENEOUS structs (all the
// same field type); the mixed-class small structs below force the harder
// classify-then-reconstruct path:
//
//   * id   - {int,double}: INTEGER eightbyte + SSE eightbyte (RDI + XMM0).
//   * di   - {double,int}: SSE eightbyte + INTEGER eightbyte (XMM0 + RDI).
//   * ff   - {float,float}: one all-float eightbyte PACKED into one XMM (x64)
//            / HFA S0,S1 (AArch64).
//   * fi   - {float,int}: one MIXED eightbyte -> one GP register (x64).
//   * csi  - {char,short,int}: one INTEGER eightbyte, sub-field reconstruction.
//   * iid  - {int,int,double}: 16B straddle, two eightbytes (a|b) + double.
//
// The callee folds every field to one integer; the entry builds the struct
// from an LCG seed in a loop and accumulates, so the result is value-only
// (independent of any absolute address).  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress221RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress221RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress221RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress221RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress221RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress221RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress221RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress221RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress221TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // {int,double}: INTEGER eightbyte (a) + SSE eightbyte (b).
    {p+"_id",
     "typedef struct{int a; double b;}"+p+"_ID;\n"
     "int "+p+"_fid("+p+"_ID s) __attribute__((noinline));\n"
     +t+" "+p+"_id("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_ID s; s.a=(int)h; s.b=(double)(int)(h>>3)+0.5;\n"
     "    acc=acc*131u+(unsigned)"+p+"_fid(s)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "int "+p+"_fid("+p+"_ID s){ return s.a ^ ((int)s.b * 7); }\n",
     {0x12345u}, "OptStress221", 2},

    // {double,int}: SSE eightbyte (a) + INTEGER eightbyte (b).
    {p+"_di",
     "typedef struct{double a; int b;}"+p+"_DI;\n"
     "int "+p+"_fdi("+p+"_DI s) __attribute__((noinline));\n"
     +t+" "+p+"_di("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_DI s; s.a=(double)(int)h-2.5; s.b=(int)(h>>5);\n"
     "    acc=acc*131u+(unsigned)"+p+"_fdi(s)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "int "+p+"_fdi("+p+"_DI s){ return ((int)s.a * 3) ^ s.b; }\n",
     {0x23456u}, "OptStress221", 2},

    // {float,float}: one all-float eightbyte packed into one XMM (x64) / HFA.
    {p+"_ff",
     "typedef struct{float a,b;}"+p+"_FF;\n"
     "int "+p+"_fff("+p+"_FF s) __attribute__((noinline));\n"
     +t+" "+p+"_ff("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_FF s; s.a=(float)(int)(h>>8); s.b=(float)(int)(h>>2)+1.0f;\n"
     "    acc=acc*131u+(unsigned)"+p+"_fff(s)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "int "+p+"_fff("+p+"_FF s){ return (int)s.a ^ ((int)s.b * 5); }\n",
     {0x34567u}, "OptStress221", 2},

    // {float,int}: one MIXED eightbyte -> one GP register on x64.
    {p+"_fi",
     "typedef struct{float a; int b;}"+p+"_FI;\n"
     "int "+p+"_ffi("+p+"_FI s) __attribute__((noinline));\n"
     +t+" "+p+"_fi("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_FI s; s.a=(float)(int)(h>>7); s.b=(int)h;\n"
     "    acc=acc*131u+(unsigned)"+p+"_ffi(s)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "int "+p+"_ffi("+p+"_FI s){ return ((int)s.a * 9) ^ s.b; }\n",
     {0x45678u}, "OptStress221", 2},

    // {char,short,int}: one INTEGER eightbyte; sub-field reconstruction.
    {p+"_csi",
     "typedef struct{char a; short b; int c;}"+p+"_CSI;\n"
     "int "+p+"_fcsi("+p+"_CSI s) __attribute__((noinline));\n"
     +t+" "+p+"_csi("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_CSI s; s.a=(char)h; s.b=(short)(h>>3); s.c=(int)(h>>7);\n"
     "    acc=acc*131u+(unsigned)"+p+"_fcsi(s)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "int "+p+"_fcsi("+p+"_CSI s){ return ((int)s.a) ^ ((int)s.b * 3) ^ (s.c * 5); }\n",
     {0x56789u}, "OptStress221", 2},

    // {int,int,double}: 16B, two eightbytes (a|b in GP) + double in SSE.
    {p+"_iid",
     "typedef struct{int a,b; double c;}"+p+"_IID;\n"
     "int "+p+"_fiid("+p+"_IID s) __attribute__((noinline));\n"
     +t+" "+p+"_iid("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    "+p+"_IID s; s.a=(int)h; s.b=(int)(h>>4); s.c=(double)(int)(h>>9)-3.5;\n"
     "    acc=acc*131u+(unsigned)"+p+"_fiid(s)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "int "+p+"_fiid("+p+"_IID s){ return s.a ^ (s.b * 3) ^ ((int)s.c * 5); }\n",
     {0x6789Au}, "OptStress221", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress221TC("x64o221", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress221TC("x86o221", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress221TC("a64o221", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress221TC("armo221", "int");

INSTANTIATE_TEST_SUITE_P(OptStress221, X64OptStress221RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress221, X86OptStress221RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress221, A64OptStress221RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress221, ARM32OptStress221RT, ::testing::ValuesIn(kARM), rtTCName);
