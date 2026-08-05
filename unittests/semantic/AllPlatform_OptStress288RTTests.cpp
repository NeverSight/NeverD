//===- AllPlatform_OptStress288RTTests.cpp - branchless/bitcount probe ====//
//
// -O2 integer kernels stressing branchless selection, min/max/clamp ladders,
// bit-counting intrinsics and constant strength-reduced multiplies:
//
//   * selmix   - branchless conditional select chains (CMOV / CSEL / sel).
//   * minmax   - signed + unsigned min / max / clamp ladder.
//   * absacc   - overflow-safe abs (unsigned domain) + |delta| accumulation.
//   * bitcnt   - popcount / clz / ctz mixing (inputs forced nonzero).
//   * cmpchain - chained 0/1 comparison masks (set-on-condition).
//   * mulconst - multiply-by-constant chains + 64-bit high-multiply fold.
//
// clz/ctz inputs are OR'd with 1 to stay UB-free, abs is taken in the unsigned
// domain to avoid INT_MIN overflow, and there is no division -- so the native
// and lifted builds agree bit-for-bit.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress288RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress288RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress288RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress288RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress288RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress288RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress288RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress288RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress288TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // branchless conditional select chains (CMOV / CSEL / sel).
    {p+"_selmix",
     t+" "+p+"_selmix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h, y=h>>7;\n"
     "    unsigned m=(x<y)?(x+1u):(y^0x55u);\n"
     "    unsigned n=((int)x<0)?(x>>1):(x<<1);\n"
     "    unsigned q=((x&3u)==0u)?m:((x&3u)==1u)?n:((x&3u)==2u)?(m^n):(m+n);\n"
     "    acc=acc*131u+q+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress288", 2},

    // signed + unsigned min / max / clamp ladder.
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    int s=(int)h; unsigned u=h&0xFFFFu;\n"
     "    int c=s<-1000?-1000:(s>1000?1000:s);\n"
     "    unsigned um=u<200u?200u:(u>800u?800u:u);\n"
     "    int mx=s>(int)u?s:(int)u; int mn=s<(int)u?s:(int)u;\n"
     "    acc=acc*131u+(unsigned)c+um+(unsigned)mx+(unsigned)mn+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress288", 2},

    // overflow-safe abs (unsigned domain) + |delta| accumulation.
    {p+"_absacc",
     t+" "+p+"_absacc("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    int s=(int)h; unsigned as=(s<0)?(unsigned)(0u-(unsigned)s):(unsigned)s;\n"
     "    unsigned lo=h>>5; unsigned d=(h>lo)?(h-lo):(lo-h);\n"
     "    acc=acc*131u+as+d+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress288", 2},

    // popcount / clz / ctz mixing (inputs forced nonzero).
    {p+"_bitcnt",
     t+" "+p+"_bitcnt("+t+" a){ unsigned h=(unsigned)a|1u; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u; unsigned v=h|1u;\n"
     "    unsigned pc=(unsigned)__builtin_popcount(v);\n"
     "    unsigned lz=(unsigned)__builtin_clz(v);\n"
     "    unsigned tz=(unsigned)__builtin_ctz(v);\n"
     "    acc=acc*131u+pc*7u+lz*3u+tz+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress288", 2},

    // chained 0/1 comparison masks (set-on-condition).
    {p+"_cmpchain",
     t+" "+p+"_cmpchain("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned a0=h&0xFFu,a1=(h>>8)&0xFFu,a2=(h>>16)&0xFFu;\n"
     "    unsigned c=(a0<a1)+(a1<a2)*2u+(a0==a2)*4u+(((int)h<0)?8u:0u)+((a0>a2)?16u:0u);\n"
     "    acc=acc*131u+c+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress288", 2},

    // multiply-by-constant chains + 64-bit high-multiply fold.
    {p+"_mulconst",
     t+" "+p+"_mulconst("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned m1=h*9u+h*100u-h*7u;\n"
     "    unsigned hi=(unsigned)(((unsigned long long)h*2654435761ull)>>32);\n"
     "    acc=acc*131u+m1+hi+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress288", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress288TC("x64o288", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress288TC("x86o288", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress288TC("a64o288", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress288TC("armo288", "int");

INSTANTIATE_TEST_SUITE_P(OptStress288, X64OptStress288RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress288, X86OptStress288RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress288, A64OptStress288RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress288, ARM32OptStress288RT, ::testing::ValuesIn(kARM), rtTCName);
