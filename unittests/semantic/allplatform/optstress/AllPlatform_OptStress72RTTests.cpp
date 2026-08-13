//===- AllPlatform_OptStress72RTTests.cpp - more struct return -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Follow-up to the #471 multi-register struct-by-value return fix: lock it in
// and exercise the adjacent shapes the OptStress71 probe did not reach.
//
//   * hfa4f - HFA struct{float a,b,c,d}: AArch64 S0-S3 (four V registers);
//             x86-64 packs a,b into XMM0 and c,d into XMM1 (two SSE eightbytes).
//   * hfa2f - HFA struct{float a,b}: AArch64 S0,S1 (two V registers); x86-64
//             packs both floats into one XMM0 — a single-register return there,
//             the negative control for the AArch64 two-register HFA path.
//   * retfd - struct{float a; double b}: not homogeneous, so AArch64 returns it
//             in X0,X1 (GP pair, the float/double bits); x86-64 in XMM0,XMM1.
//   * accfld - a struct-returning maker called in a loop, its double field
//              accumulated across iterations (struct return as a loop-carried
//              consumer).
//
// Each folds to a single integer return; no libm, no 64-bit divide helper.
// All four targets, -O2.  i386/ARM32 return these via the sret pointer.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress72RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress72RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress72RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress72RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress72RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress72RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress72RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress72RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress72TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 4-float HFA: AArch64 S0-S3 ; x86-64 XMM0(a,b),XMM1(c,d).
    {p+"_hfa4f",
     "struct "+p+"F4{ float a,b,c,d; };\n"
     "static struct "+p+"F4 "+p+"_mk(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_hfa4f("+t+" a){ struct "+p+"F4 r="+p+"_mk((unsigned)a);\n"
     "  return ("+t+")(int)(r.a*r.b + r.c*r.d); }\n"
     "static struct "+p+"F4 "+p+"_mk(unsigned s){\n"
     "  s=s*1103515245u+12345u; float a=(float)((s>>8)&0x7f);\n"
     "  s=s*1103515245u+12345u; float b=(float)((s>>8)&0x3f);\n"
     "  s=s*1103515245u+12345u; float c=(float)((s>>8)&0x7f);\n"
     "  s=s*1103515245u+12345u; float d=(float)((s>>8)&0x3f);\n"
     "  struct "+p+"F4 r={a,b,c,d}; return r; }\n",
     {0x61u}, "OptStress72", 2},

    // 2-float HFA: AArch64 S0,S1 ; x86-64 single packed XMM0.
    {p+"_hfa2f",
     "struct "+p+"F2{ float a,b; };\n"
     "static struct "+p+"F2 "+p+"_mk(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_hfa2f("+t+" a){ struct "+p+"F2 r="+p+"_mk((unsigned)a);\n"
     "  return ("+t+")(int)(r.a*r.b + r.a - r.b); }\n"
     "static struct "+p+"F2 "+p+"_mk(unsigned s){\n"
     "  s=s*1103515245u+12345u; float a=(float)((s>>8)&0xff);\n"
     "  s=s*1103515245u+12345u; float b=(float)((s>>8)&0x7f)+1.0f;\n"
     "  struct "+p+"F2 r={a,b}; return r; }\n",
     {0x62u}, "OptStress72", 2},

    // {float,double} (not an HFA): AArch64 X0,X1 ; x86-64 XMM0,XMM1.
    {p+"_retfd",
     "struct "+p+"FD{ float a; double b; };\n"
     "static struct "+p+"FD "+p+"_mk(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_retfd("+t+" a){ struct "+p+"FD r="+p+"_mk((unsigned)a);\n"
     "  return ("+t+")(long long)((double)r.a + r.b); }\n"
     "static struct "+p+"FD "+p+"_mk(unsigned s){\n"
     "  s=s*1103515245u+12345u; float a=(float)((s>>8)&0xffff);\n"
     "  s=s*1103515245u+12345u; double b=(double)((s>>8)&0xff)+0.5;\n"
     "  struct "+p+"FD r={a,b}; return r; }\n",
     {0x63u}, "OptStress72", 2},

    // Struct-returning maker called in a loop; its double field accumulated.
    {p+"_accfld",
     "struct "+p+"AD{ double x; long y; };\n"
     "static struct "+p+"AD "+p+"_mk(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_accfld("+t+" a){ unsigned s=(unsigned)a; double acc=0; long t=0;\n"
     "  for(int i=0;i<80;i++){ s=s*1103515245u+12345u;\n"
     "    struct "+p+"AD r="+p+"_mk(s); acc+=r.x; t+=r.y; }\n"
     "  return ("+t+")((long long)acc + t); }\n"
     "static struct "+p+"AD "+p+"_mk(unsigned s){\n"
     "  double x=(double)((s>>8)&0x3f); long y=(long)((s>>16)&0x1f);\n"
     "  struct "+p+"AD r={x,y}; return r; }\n",
     {0x64u}, "OptStress72", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress72TC("x64o72", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress72TC("x86o72", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress72TC("a64o72", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress72TC("armo72", "int");

INSTANTIATE_TEST_SUITE_P(OptStress72, X64OptStress72RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress72, X86OptStress72RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress72, A64OptStress72RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress72, ARM32OptStress72RT, ::testing::ValuesIn(kARM), rtTCName);
