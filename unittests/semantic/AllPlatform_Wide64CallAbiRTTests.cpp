//===- AllPlatform_Wide64CallAbiRTTests.cpp - i64 call ABI ------*-C++*-=//
//
// Roundtrip probes for passing and returning 64-bit (`long long`) values across
// noinline call boundaries.  The #408-#414 call-ABI rework used only integer
// register-width (and FP) arguments; a 64-bit value occupies a register *pair*
// (AArch32 AAPCS r0:r1 / r2:r3 with the even-register alignment rule, i386
// EDX:EAX return + 8-byte stack slots) which the recovery path must keep paired.
//
// Every helper sticks to 64-bit add / sub / and / or / xor / compare and
// *constant* shifts only (a variable i64 shift or i64 multiply/divide would
// lower to __ashldi3 / __muldi3 / __divdi3, which the bare-metal harness cannot
// resolve); 64-bit values are assembled from two 32-bit halves with a constant
// `<<32`.  Each kernel folds to one integer return, compiled -O2, checked native
// vs lifted on all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Wide64CallAbiRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Wide64CallAbiRT, Verify) { roundTripX64(GetParam()); }
class X86Wide64CallAbiRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86Wide64CallAbiRT, Verify) { roundTripX86(GetParam()); }
class A64Wide64CallAbiRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64Wide64CallAbiRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32Wide64CallAbiRT : public SemanticRoundTripFixture,
                             public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32Wide64CallAbiRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeWide64CallTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 64-bit args + 64-bit return through a loop-carried accumulator.
    {p+"_acc",
     "static long long "+p+"_a64(long long,long long) __attribute__((noinline));\n"
     +t+" "+p+"_acc("+t+" a){\n"
     "  unsigned lo=(unsigned)a, hi=(unsigned)a^0x9e3779b9u;\n"
     "  long long acc=(long long)(((unsigned long long)hi<<32)|lo);\n"
     "  for(int i=0;i<12;i++){\n"
     "    unsigned yl=(unsigned)a*(unsigned)(i+1), yh=(unsigned)a+(unsigned)i*0x77u;\n"
     "    long long y=(long long)(((unsigned long long)yh<<32)|yl);\n"
     "    acc="+p+"_a64(acc,y); }\n"
     "  unsigned h=(unsigned)acc ^ (unsigned)((unsigned long long)acc>>32);\n"
     "  return ("+t+")(unsigned)h; }\n"
     "static long long "+p+"_a64(long long x,long long y){\n"
     "  return x + y + (x>>11) - (y<<3); }\n",
     {0x41ULL}, "Wide64Call", 2},

    // Mixed (int, long long, int) args: exercises the AArch32 even-register
    // alignment pad (a->r0, b->r2:r3, c->stack) and i386 stack ordering.
    {p+"_mix",
     "static long long "+p+"_m64(int,long long,int) __attribute__((noinline));\n"
     +t+" "+p+"_mix("+t+" a){\n"
     "  long long acc=0; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<10;i++){\n"
     "    unsigned bl=s*2654435761u, bh=s^0x55555555u;\n"
     "    long long b=(long long)(((unsigned long long)bh<<32)|bl);\n"
     "    acc^="+p+"_m64((int)s, b, i); s=s*1103515245u+12345u; }\n"
     "  unsigned h=(unsigned)acc ^ (unsigned)((unsigned long long)acc>>32);\n"
     "  return ("+t+")(unsigned)h; }\n"
     "static long long "+p+"_m64(int a,long long b,int c){\n"
     "  long long r=b+(long long)a-(long long)c; r^=(b>>13); r+=((long long)c<<20); return r; }\n",
     {0x53ULL}, "Wide64Call", 2},

    // Helper returns a 64-bit value built from a seed; caller consumes both
    // halves (EDX:EAX on i386, r0:r1 on AArch32).
    {p+"_ret",
     "static long long "+p+"_mk64(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_ret("+t+" a){\n"
     "  unsigned h=0;\n"
     "  for(unsigned i=0;i<12;i++){\n"
     "    long long v="+p+"_mk64((unsigned)a+i*0x101u);\n"
     "    h=h*131u+(unsigned)v + (unsigned)((unsigned long long)v>>32); }\n"
     "  return ("+t+")(unsigned)h; }\n"
     "static long long "+p+"_mk64(unsigned x){\n"
     "  unsigned lo=x*2654435761u+1u, hi=x^0x9e3779b9u;\n"
     "  return (long long)(((unsigned long long)hi<<32)|lo); }\n",
     {0x67ULL}, "Wide64Call", 2},

    // 64-bit args + narrow (int) return: helper compares two 64-bit values.
    {p+"_cmp",
     "static int "+p+"_c64(long long,long long) __attribute__((noinline));\n"
     +t+" "+p+"_cmp("+t+" a){\n"
     "  unsigned h=0; unsigned s=(unsigned)a|1u;\n"
     "  for(int i=0;i<12;i++){\n"
     "    unsigned xl=s, xh=s^0x1234u, yl=s*7u, yh=s+(unsigned)i;\n"
     "    long long x=(long long)(((unsigned long long)xh<<32)|xl);\n"
     "    long long y=(long long)(((unsigned long long)yh<<32)|yl);\n"
     "    h=h*131u+(unsigned)("+p+"_c64(x,y)+2); s=s*1103515245u+12345u; }\n"
     "  return ("+t+")(unsigned)h; }\n"
     "static int "+p+"_c64(long long a,long long b){ return a<b?-1:(a>b?1:0); }\n",
     {0x71ULL}, "Wide64Call", 2},

    // Three 64-bit args: a->r0:r1, b->r2:r3, c->stack on AArch32; all stack on
    // i386 — stresses the stack-slot recovery for a 64-bit argument.
    {p+"_three",
     "static long long "+p+"_g3(long long,long long,long long) __attribute__((noinline));\n"
     +t+" "+p+"_three("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<8;i++){\n"
     "    long long x=(long long)(((unsigned long long)(s^0xaau)<<32)|(s*3u));\n"
     "    long long y=(long long)(((unsigned long long)(s+9u)<<32)|(s*5u));\n"
     "    long long z=(long long)(((unsigned long long)(s^0x77u)<<32)|(s*7u));\n"
     "    acc+="+p+"_g3(x,y,z); s=s*1103515245u+12345u; }\n"
     "  unsigned h=(unsigned)acc ^ (unsigned)((unsigned long long)acc>>32);\n"
     "  return ("+t+")(unsigned)h; }\n"
     "static long long "+p+"_g3(long long a,long long b,long long c){\n"
     "  return (a^b) + (b&c) - (a|c) + (a>>5) + (c<<2); }\n",
     {0x29ULL}, "Wide64Call", 2},

    // Struct with a 64-bit field passed by value.
    {p+"_struct",
     "struct "+p+"W{ long long x; int y; };\n"
     "static long long "+p+"_hw(struct "+p+"W) __attribute__((noinline));\n"
     +t+" "+p+"_struct("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; long long acc=0;\n"
     "  for(int i=0;i<8;i++){\n"
     "    struct "+p+"W w; w.x=(long long)(((unsigned long long)(s^0x9eu)<<32)|(s*131u)); w.y=(int)s+i;\n"
     "    acc^="+p+"_hw(w); s=s*1103515245u+12345u; }\n"
     "  unsigned h=(unsigned)acc ^ (unsigned)((unsigned long long)acc>>32);\n"
     "  return ("+t+")(unsigned)h; }\n"
     "static long long "+p+"_hw(struct "+p+"W w){ return w.x + (long long)w.y - (w.x>>17); }\n",
     {0x9bULL}, "Wide64Call", 2},
  };
}
// clang-format on

// All combinations run on all four targets: an i386/ARM32 64-bit value threaded
// from a call's i64 result into the next call's i64 argument (acc), or one of
// three i64 stack arguments (three), used to drop the high half.  That is now
// modeled — a call proven to return i64 defines both the low and high return
// register (modelCallWideIntReturn, forced by callee return-type inference), so
// the high 32 bits reach the next call instead of resolving to a stale value.
static const std::vector<RoundTripTC> kX64 = makeWide64CallTC("x64w64", "long");
static const std::vector<RoundTripTC> kX86 = makeWide64CallTC("x86w64", "int");
static const std::vector<RoundTripTC> kA64 = makeWide64CallTC("a64w64", "long");
static const std::vector<RoundTripTC> kARM = makeWide64CallTC("armw64", "int");

INSTANTIATE_TEST_SUITE_P(Wide64Call, X64Wide64CallAbiRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Wide64Call, X86Wide64CallAbiRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Wide64Call, A64Wide64CallAbiRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Wide64Call, ARM32Wide64CallAbiRT, ::testing::ValuesIn(kARM), rtTCName);
