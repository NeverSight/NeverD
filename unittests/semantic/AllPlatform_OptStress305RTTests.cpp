//===- AllPlatform_OptStress305RTTests.cpp - param-width inference probe -===//
//
// -O2 kernels stressing the PARAMETER side of integer type inference — the
// argument-register dual of OptStress303/304 (which probed the return side).
// Each entry calls `noinline` helpers whose parameters are sub-word (signed/
// unsigned char, short) or mixed-width (int / long long / pointer), so the
// lifter (MedTypePass type inference + MedCallingConv argument typing) must
// reconstruct each incoming argument's exact width and signedness:
//
//   * pchar  - signed char + unsigned char params (sign- vs zero-extend in regs).
//   * pshort - short + unsigned short params.
//   * pmix   - char/short/int/long long in one signature (reg-class assignment).
//   * pmany  - 8 mixed-width params: the tail spills to the stack (sub-word slot).
//   * pll    - long long + int params, returns long long (reg/stack pairs on 32b).
//   * pptr   - const-pointer param over a file-scope global array (.bss escape).
//
// A mis-inferred parameter width/sign (e.g. treating a `signed char` arg as a
// full register, or zero- instead of sign-extending) makes the recompiled code
// diverge from the original.  The entry function is DEFINED FIRST so it lands at
// the emulation entry (CODE_BASE); the helpers follow.  All 64-bit math is
// multiply/shift/add/logic only (no 64-bit division) to stay libcall-free on
// i386/arm32.  Deterministic (LCG-seeded).  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress305RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress305RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress305RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress305RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress305RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress305RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress305RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress305RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress305TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // signed char + unsigned char params: sign- vs zero-extend in arg regs.
    {p+"_pchar",
     "static int "+p+"_hc(signed char x, unsigned char y) __attribute__((noinline));\n"
     +t+" "+p+"_pchar("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<60;i++){ w=w*1103515245u+12345u;\n"
     "    acc += "+p+"_hc((signed char)(w>>5),(unsigned char)(w>>13)); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_hc(signed char x, unsigned char y){\n"
     "  return (int)x*7 - (int)y*3 + ((int)x ^ (int)y); }\n",
     {0x1234u}, "OptStress305", Opt},

    // short + unsigned short params.
    {p+"_pshort",
     "static int "+p+"_hs(short x, unsigned short y) __attribute__((noinline));\n"
     +t+" "+p+"_pshort("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<60;i++){ w=w*1103515245u+12345u;\n"
     "    acc = acc*131 + "+p+"_hs((short)(w>>3),(unsigned short)(w>>11)); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_hs(short x, unsigned short y){\n"
     "  return (int)x + (int)y*5 - (((int)x*(int)y) >> 3); }\n",
     {0x2345u}, "OptStress305", Opt},

    // char/short/int/long long mixed in one signature (reg-class assignment).
    {p+"_pmix",
     "static long long "+p+"_hm(signed char a,short b,int c,long long d) __attribute__((noinline));\n"
     +t+" "+p+"_pmix("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<48;i++){ w=w*1103515245u+12345u;\n"
     "    long long d=((long long)(unsigned)w<<19)^((long long)i*2654435761LL);\n"
     "    acc += "+p+"_hm((signed char)(w>>2),(short)(w>>7),(int)w,d); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_hm(signed char a,short b,int c,long long d){\n"
     "  return (long long)a*1000003LL + (long long)b*131 + (long long)c + (d^(d>>17)); }\n",
     {0x3456u}, "OptStress305", Opt},

    // 8 mixed-width params: the tail spills to the stack (sub-word stack slot).
    {p+"_pmany",
     "static long long "+p+"_h8(int a,short b,unsigned char c,long long d,int e,"
     "unsigned short f,signed char g,long long h) __attribute__((noinline));\n"
     +t+" "+p+"_pmany("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    acc = acc*131 + "+p+"_h8((int)w,(short)(w>>3),(unsigned char)(w>>5),\n"
     "      ((long long)(unsigned)w<<7),(int)(w>>2),(unsigned short)(w>>9),\n"
     "      (signed char)(w>>17),(((long long)(unsigned)w<<11)^i)); }\n"
     "  return ("+t+")(acc ^ (acc>>29)); }\n"
     "static long long "+p+"_h8(int a,short b,unsigned char c,long long d,int e,"
     "unsigned short f,signed char g,long long h){\n"
     "  return (long long)a + (long long)b*3 + (long long)c*5 + d + (long long)e*7\n"
     "    + (long long)f*11 + (long long)g*13 + h; }\n",
     {0x4567u}, "OptStress305", Opt},

    // long long + int params, returns long long (reg/stack pairs on 32-bit):
    // the 64-bit arg is built fresh each step and the 64-bit return accumulated.
    {p+"_pll",
     "static long long "+p+"_hl(long long x,int n) __attribute__((noinline));\n"
     +t+" "+p+"_pll("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<24;i++){ w=w*1103515245u+12345u;\n"
     "    long long x=((long long)(unsigned)w<<21) ^ ((long long)(int)w);\n"
     "    acc = acc*131 + "+p+"_hl(x, (i&7)+1); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_hl(long long x,int n){\n"
     "  long long r = x ^ ((long long)n * 0x9E3779B1LL);\n"
     "  r = r*131 + (x >> 17); r = r ^ (r << 13);\n"
     "  return r - (long long)n; }\n",
     {0x5678u}, "OptStress305", Opt},

    // const-pointer param over a file-scope global array (.bss escape).
    {p+"_pptr",
     "static unsigned "+p+"_garr[16];\n"
     "static unsigned "+p+"_hp(const unsigned* arr,int n) __attribute__((noinline));\n"
     +t+" "+p+"_pptr("+t+" a){ unsigned w=(unsigned)a|1u;\n"
     "  for(int i=0;i<16;i++){ w=w*1103515245u+12345u; "+p+"_garr[i]=w; }\n"
     "  unsigned long long acc=0;\n"
     "  for(int k=1;k<=16;k++){ acc = acc*1000003ULL + "+p+"_hp("+p+"_garr,k); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static unsigned "+p+"_hp(const unsigned* arr,int n){ unsigned s=0;\n"
     "  for(int i=0;i<n;i++) s=s*131u+arr[i]; return s; }\n",
     {0x6789u}, "OptStress305", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress305TC("x64o305", "long", 2);
static const std::vector<RoundTripTC> kX86 = makeOptStress305TC("x86o305", "int", 2);
static const std::vector<RoundTripTC> kA64 = makeOptStress305TC("a64o305", "long", 2);
static const std::vector<RoundTripTC> kARM = makeOptStress305TC("armo305", "int", 2);

INSTANTIATE_TEST_SUITE_P(OptStress305, X64OptStress305RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress305, X86OptStress305RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress305, A64OptStress305RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress305, ARM32OptStress305RT, ::testing::ValuesIn(kARM), rtTCName);
