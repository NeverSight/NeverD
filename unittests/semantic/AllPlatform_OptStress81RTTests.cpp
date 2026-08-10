//===- AllPlatform_OptStress81RTTests.cpp - -O3 SIMD reductions --*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O3 auto-vectorized kernels that lower to the less-common SIMD shapes the
// VectorAlgo suites do not drive head-on, exercising the SIMD lift across all
// four targets (SSE2 on x86, NEON on AArch64 / ARM32-cortex-a15):
//
//   * argmax  - index of the maximum element (compare + blend of a lane-index
//               vector, then a horizontal max/index reduction).
//   * sad     - sum of absolute differences (psadbw on x86, uabd + uadalp on
//               NEON) folded to a scalar.
//   * runlen  - longest run of a predicate over a stream (vectorized compare +
//               select-accumulate).
//   * cmpcnt  - count elements satisfying several packed comparisons at once.
//
// All integer, fold to one return, no float / 64-bit divide / libcall.
// All four targets, -O3.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress81RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress81RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress81RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress81RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress81RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress81RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress81RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress81RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress81TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Index of the maximum element (compare + index-blend + horizontal reduce).
    {p+"_argmax",
     t+" "+p+"_argmax("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int v[64]; int h=0;\n"
     "  for(int i=0;i<64;i++){ s=s*1103515245u+12345u; v[i]=(int)(s>>8)-(int)0x800000; }\n"
     "  for(int r=0;r<32;r++){\n"
     "    int best=v[0], bi=0;\n"
     "    for(int i=1;i<64;i++) if(v[i]>best){ best=v[i]; bi=i; }\n"
     "    h=h*131+best+bi*7; v[bi]=-v[bi]; }\n"
     "  return ("+t+")h; }\n",
     {0xF1u}, "OptStress81", 3},

    // Sum of absolute differences over two streams (psadbw / uabd+uadalp).
    {p+"_sad",
     t+" "+p+"_sad("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char x[128], y[128]; unsigned h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u; x[i]=(unsigned char)(s>>5);\n"
     "    s=s*1103515245u+12345u; y[i]=(unsigned char)(s>>7); }\n"
     "  for(int r=0;r<32;r++){ unsigned sum=0;\n"
     "    for(int i=0;i<128;i++){ int d=(int)x[i]-(int)y[i]; sum+=(unsigned)(d<0?-d:d); }\n"
     "    h=h*131u+sum; x[r&127]^=(unsigned char)sum; }\n"
     "  return ("+t+")h; }\n",
     {0xF2u}, "OptStress81", 3},

    // Longest run of a predicate over a stream (vectorized compare + select-acc).
    {p+"_runlen",
     t+" "+p+"_runlen("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int v[96]; unsigned h=0;\n"
     "  for(int i=0;i<96;i++){ s=s*1103515245u+12345u; v[i]=(int)(s>>9); }\n"
     "  for(int r=0;r<24;r++){ int cur=0,best=0;\n"
     "    for(int i=0;i<96;i++){ cur=(v[i]>(int)r*1000-48000)?cur+1:0; if(cur>best)best=cur; }\n"
     "    h=h*131u+(unsigned)best; }\n"
     "  return ("+t+")h; }\n",
     {0xF3u}, "OptStress81", 3},

    // Count elements satisfying several packed comparisons at once.
    {p+"_cmpcnt",
     t+" "+p+"_cmpcnt("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int v[128]; unsigned h=0;\n"
     "  for(int i=0;i<128;i++){ s=s*1103515245u+12345u; v[i]=(int)(s>>6); }\n"
     "  for(int r=0;r<24;r++){ int lo=(int)r*1000-12000, hi=lo+40000;\n"
     "    unsigned c1=0,c2=0,c3=0;\n"
     "    for(int i=0;i<128;i++){ int x=v[i];\n"
     "      c1+=(x>lo); c2+=(x<hi); c3+=((x&7)==r%7); }\n"
     "    h=h*131u+c1*5u+c2*3u+c3*7u; }\n"
     "  return ("+t+")h; }\n",
     {0xF4u}, "OptStress81", 3},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress81TC("x64o81", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress81TC("x86o81", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress81TC("a64o81", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress81TC("armo81", "int");

INSTANTIATE_TEST_SUITE_P(OptStress81, X64OptStress81RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress81, X86OptStress81RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress81, A64OptStress81RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress81, ARM32OptStress81RT, ::testing::ValuesIn(kARM), rtTCName);
