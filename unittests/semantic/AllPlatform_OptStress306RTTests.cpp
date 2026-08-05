//===- AllPlatform_OptStress306RTTests.cpp - -O0 param-width inference ---===//
//
// -O0 sink-difference dual of OptStress305: the SAME parameter-width inference
// kernels compiled at -O0.  Low optimization changes the lift surface that
// MedTypePass type inference + MedCallingConv argument typing must reconstruct:
// at -O2 each incoming argument lives in its parameter register across the call,
// whereas at -O0 clang spills every argument to a stack slot in the prologue and
// RELOADS it at each use, and the sub-word params are re-extended (movzx/movsx on
// x86, uxt*/sxt* on arm) off that slot.  A mis-inferred parameter width/sign
// therefore surfaces through a different instruction shape than 305 (cf. the -O0
// duals in #508/#509/#512 and the 299/302/304 pairs).
//
//   * pchar  - signed char + unsigned char params (sign- vs zero-extend).
//   * pshort - short + unsigned short params.
//   * pmix   - char/short/int/long long in one signature.
//   * preuse - 5 mixed-width params, each read twice (forces -O0 frame reloads).
//   * pll    - long long + int params, returns long long (reg/stack pairs on 32b).
//   * pptr   - const-pointer param over a file-scope global array (.bss escape).
//
// The entry function is DEFINED FIRST so it lands at the emulation entry
// (CODE_BASE); the helpers follow.  All 64-bit math is multiply/shift/add/logic
// only (no 64-bit division) to stay libcall-free on i386/arm32.  Deterministic
// (LCG-seeded).  All four targets, -O0.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress306RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress306RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress306RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress306RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress306RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress306RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress306RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress306RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress306TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // signed char + unsigned char params: sign- vs zero-extend off the spill slot.
    {p+"_pchar",
     "static int "+p+"_hc(signed char x, unsigned char y) __attribute__((noinline));\n"
     +t+" "+p+"_pchar("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<60;i++){ w=w*1103515245u+12345u;\n"
     "    acc += "+p+"_hc((signed char)(w>>5),(unsigned char)(w>>13)); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_hc(signed char x, unsigned char y){\n"
     "  return (int)x*7 - (int)y*3 + ((int)x ^ (int)y); }\n",
     {0x1234u}, "OptStress306", Opt},

    // short + unsigned short params.
    {p+"_pshort",
     "static int "+p+"_hs(short x, unsigned short y) __attribute__((noinline));\n"
     +t+" "+p+"_pshort("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<60;i++){ w=w*1103515245u+12345u;\n"
     "    acc = acc*131 + "+p+"_hs((short)(w>>3),(unsigned short)(w>>11)); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_hs(short x, unsigned short y){\n"
     "  return (int)x + (int)y*5 - (((int)x*(int)y) >> 3); }\n",
     {0x2345u}, "OptStress306", Opt},

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
     {0x3456u}, "OptStress306", Opt},

    // 5 mixed-width params, each read twice in the helper body: at -O0 clang spills
    // every incoming arg to a frame slot and reloads at each use (movzx/movsx off
    // [ebp-k] on i386).  The 8-param pmany shape from 305 hits a separate i386 -O0
    // stack-slot bug and is kept at -O2 only; this case still exercises the -O0
    // sub-word reload path without that trigger.
    {p+"_preuse",
     "static long long "+p+"_hr(signed char a,unsigned short b,int c,long long d,short e)"
     " __attribute__((noinline));\n"
     +t+" "+p+"_preuse("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    long long dv=((long long)(unsigned)w<<7)^((long long)i);\n"
     "    acc = acc*131 + "+p+"_hr((signed char)(w>>5),(unsigned short)(w>>9),\n"
     "      (int)w,dv,(short)(w>>3)); }\n"
     "  return ("+t+")(acc ^ (acc>>29)); }\n"
     "static long long "+p+"_hr(signed char a,unsigned short b,int c,long long d,short e){\n"
     "  return (long long)a*7+(long long)b*11+(long long)c*13+d+(long long)e*17\n"
     "    +(long long)a+(long long)b; }\n",
     {0x4567u}, "OptStress306", Opt},

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
     {0x5678u}, "OptStress306", Opt},

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
     {0x6789u}, "OptStress306", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress306TC("x64o306", "long", 0);
static const std::vector<RoundTripTC> kX86 = makeOptStress306TC("x86o306", "int", 0);
static const std::vector<RoundTripTC> kA64 = makeOptStress306TC("a64o306", "long", 0);
static const std::vector<RoundTripTC> kARM = makeOptStress306TC("armo306", "int", 0);

INSTANTIATE_TEST_SUITE_P(OptStress306, X64OptStress306RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress306, X86OptStress306RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress306, A64OptStress306RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress306, ARM32OptStress306RT, ::testing::ValuesIn(kARM), rtTCName);
