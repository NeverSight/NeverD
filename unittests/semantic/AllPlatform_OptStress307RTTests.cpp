//===- AllPlatform_OptStress307RTTests.cpp - -O0 wide arg lists ----------===//
//
// -O0 kernels stressing the OUTGOING-argument area of a call with a long /
// wide argument list — the caller-side dual of the parameter-width probes
// 305/306, and the area where #515 found that an i386 -O0 caller spills the
// outgoing-argument base (a copy of ESP) to a frame slot and reloads it before
// the call, so recoverCallAbi's stack-store scan must follow that spill/reload
// to place every argument (a single-arg truncation otherwise drops the rest).
//
// At -O0 clang materialises each outgoing argument into a stack slot via a base
// pointer rather than a tight `push` sequence, and a large argument list makes
// it park and reload that base — exercised here on every target so the
// equivalent x86-64 (>6 integer args), AArch64 (>8), and ARM (>4) stack-arg
// constructions are covered, not just i386 where #515 first surfaced:
//
//   * a8i   - 8 int args (x64 6 reg + 2 stack; i386 all stack; ARM 4 reg + 4).
//   * a10i  - 10 int args (forces stack args on *every* target, incl. AArch64).
//   * a6ll  - 6 long long args (wide; register pairs / 8-byte slots on 32-bit).
//   * amixw - alternating int / long long args (mixed reg-class + slot widths).
//   * aptr  - two (pointer,count) pairs over a file-scope global (.bss escape).
//   * achain- 7-arg call whose arg0 is the *previous* call's return value (the
//             result-register-as-next-argument reaching path).
//
// The entry function is DEFINED FIRST so it lands at the emulation entry
// (CODE_BASE); the helpers follow.  All 64-bit math is multiply-by-constant /
// shift / add / logic only (no 64-bit division) to stay libcall-free on
// i386/arm32.  Deterministic (LCG-seeded).  All four targets, -O0.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress307RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress307RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress307RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress307RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress307RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress307RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress307RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress307RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress307TC(const char *prefix,
                                                   const char *T, int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // 8 int args: x64 spills 2 to the stack, i386 all 8, ARM the tail 4.
    {p+"_a8i",
     "static int "+p+"_h8(int a,int b,int c,int d,int e,int f,int g,int h) __attribute__((noinline));\n"
     +t+" "+p+"_a8i("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    acc = acc*131 + "+p+"_h8((int)w,(int)(w>>1),(int)(w>>2),(int)(w>>3),\n"
     "      (int)(w>>4),(int)(w>>5),(int)(w>>6),(int)(w>>7)); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_h8(int a,int b,int c,int d,int e,int f,int g,int h){\n"
     "  return a*2 - b*3 + c*5 - d*7 + e*11 - f*13 + g*17 - h*19; }\n",
     {0x1111u}, "OptStress307", Opt},

    // 10 int args: forces outgoing stack arguments on every target, AArch64
    // (8 register args) included.
    {p+"_a10i",
     "static int "+p+"_h10(int a,int b,int c,int d,int e,int f,int g,int h,int i,int j) __attribute__((noinline));\n"
     +t+" "+p+"_a10i("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int k=0;k<40;k++){ w=w*1103515245u+12345u;\n"
     "    acc = acc*131 + "+p+"_h10((int)w,(int)(w>>1),(int)(w>>2),(int)(w>>3),(int)(w>>4),\n"
     "      (int)(w>>5),(int)(w>>6),(int)(w>>7),(int)(w>>8),(int)(w>>9)); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_h10(int a,int b,int c,int d,int e,int f,int g,int h,int i,int j){\n"
     "  return a - b + c - d + e - f + g - h + i - j + (a^j) + (b&c); }\n",
     {0x2222u}, "OptStress307", Opt},

    // 6 long long args: wide outgoing arguments (register pairs / 8-byte slots
    // on 32-bit), a large stack-argument area built before the call.
    {p+"_a6ll",
     "static long long "+p+"_hll(long long a,long long b,long long c,long long d,long long e,long long f) __attribute__((noinline));\n"
     +t+" "+p+"_a6ll("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<32;i++){ w=w*1103515245u+12345u;\n"
     "    long long x=((long long)(unsigned)w<<20) ^ ((long long)(int)w);\n"
     "    acc = acc*131 + "+p+"_hll(x, x^1234567LL, x*3, x>>7, x<<5, x^(x>>17)); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static long long "+p+"_hll(long long a,long long b,long long c,long long d,long long e,long long f){\n"
     "  return a + b*3 - c + d*5 + e - f*7; }\n",
     {0x3333u}, "OptStress307", Opt},

    // Alternating int / long long args: mixed register-class and slot-width
    // assignment in one outgoing list.
    {p+"_amixw",
     "static long long "+p+"_hmw(int a,long long b,int c,long long d,int e,long long f) __attribute__((noinline));\n"
     +t+" "+p+"_amixw("+t+" a){ unsigned w=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<32;i++){ w=w*1103515245u+12345u;\n"
     "    long long b=((long long)(unsigned)w<<19) ^ ((long long)i*2654435761LL);\n"
     "    acc = acc*131 + "+p+"_hmw((int)w, b, (int)(w>>3), b^0x55557777LL, (int)(w>>7), b*3); }\n"
     "  return ("+t+")(acc ^ (acc>>31)); }\n"
     "static long long "+p+"_hmw(int a,long long b,int c,long long d,int e,long long f){\n"
     "  return (long long)a + b - (long long)c*5 + d*3 - (long long)e + f; }\n",
     {0x4444u}, "OptStress307", Opt},

    // Two (pointer,count) pairs over a file-scope global array (.bss escape):
    // pointer + int arguments interleaved across the outgoing list.
    {p+"_aptr",
     "static unsigned "+p+"_garr[24];\n"
     "static unsigned "+p+"_hp(const unsigned* p,int n,const unsigned* q,int m) __attribute__((noinline));\n"
     +t+" "+p+"_aptr("+t+" a){ unsigned w=(unsigned)a|1u;\n"
     "  for(int i=0;i<24;i++){ w=w*1103515245u+12345u; "+p+"_garr[i]=w; }\n"
     "  unsigned long long acc=0;\n"
     "  for(int k=1;k<=12;k++){ acc = acc*1000003ULL + "+p+"_hp("+p+"_garr,k,"+p+"_garr+12,k); }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static unsigned "+p+"_hp(const unsigned* p,int n,const unsigned* q,int m){ unsigned s=0;\n"
     "  for(int i=0;i<n;i++) s=s*131u+p[i];\n"
     "  for(int i=0;i<m;i++) s=s*7u+q[i];\n"
     "  return s; }\n",
     {0x5555u}, "OptStress307", Opt},

    // 7-arg call whose arg0 is the PREVIOUS call's return value: the result
    // register flows straight into the next call's arg0 (no intervening move),
    // exercising the result-register-as-reaching-argument path with a long
    // outgoing list behind it.
    {p+"_achain",
     "static int "+p+"_hc(int a,int b,int c,int d,int e,int f,int g) __attribute__((noinline));\n"
     +t+" "+p+"_achain("+t+" a){ unsigned w=(unsigned)a|1u; int acc=0;\n"
     "  for(int i=0;i<40;i++){ w=w*1103515245u+12345u;\n"
     "    acc = "+p+"_hc(acc,(int)w,(int)(w>>2),(int)(w>>4),(int)(w>>6),(int)(w>>8),i); }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_hc(int a,int b,int c,int d,int e,int f,int g){\n"
     "  return a*131 + b - c + d*3 - e + f*5 - g; }\n",
     {0x6666u}, "OptStress307", Opt},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress307TC("x64o307", "long", 0);
static const std::vector<RoundTripTC> kX86 = makeOptStress307TC("x86o307", "int", 0);
static const std::vector<RoundTripTC> kA64 = makeOptStress307TC("a64o307", "long", 0);
static const std::vector<RoundTripTC> kARM = makeOptStress307TC("armo307", "int", 0);

INSTANTIATE_TEST_SUITE_P(OptStress307, X64OptStress307RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress307, X86OptStress307RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress307, A64OptStress307RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress307, ARM32OptStress307RT, ::testing::ValuesIn(kARM), rtTCName);
