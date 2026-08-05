//===- AllPlatform_OptStress10RTTests.cpp - opt-stress probes ---*-C++*-=//
//
// Optimizer-stress roundtrip probes that reach shapes the OptStress1-9 series
// did not, deliberately picked to hit paths that have historically hidden
// optimizer / lift semantic bugs:
//
//   * sumsq    - sum of squares over a stack int array.  At -O3 the reduction
//                auto-vectorizes (SSE2 pmuludq / NEON mul + horizontal add),
//                stressing SIMD reduction lifting inside a real loop body.
//   * dotp     - dot product of two stack arrays (-O3 multiply-accumulate
//                vectorization with two parallel loads).
//   * maxabs   - max of absolute values (-O3 pabsd / abs + pmaxsd / smax
//                vectorized reduction).
//   * cntpred  - count elements above a runtime threshold (-O3 compare-mask
//                reduction; the predicate count narrows a vector mask to an int).
//   * satsum   - running saturating sum clamped to 16 bits: a loop-carried
//                dependency that stays scalar with a cmov / csel clamp.
//   * mix64    - splitmix64 mixing with byte extraction: on i386 / arm32 the
//                64x64->64 multiplies and 64-bit shifts lower to inline 32-bit
//                sequences (never a libcall), with sub-register byte reads.
//   * prefix   - in-place prefix sum (carried scan) then a hash fold.
//   * histmod  - histogram scatter into a small stack array by a runtime index
//                (computed-index store/reload, not vectorized).
//
// Every kernel is integer-only, folds to a single integer return, and lowers
// to no runtime helper, so all four targets are checked native vs lifted.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress10RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress10RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress10RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress10RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress10RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress10RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress10RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress10RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress10TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // sum of squares: -O3 auto-vectorized integer reduction.
    {p+"_sumsq",
     t+" "+p+"_sumsq("+t+" a){\n"
     "  unsigned e[32]; unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<32;i++){ x=x*1103515245u+12345u; e[i]=(x>>9)&0xffffu; }\n"
     "  unsigned s=0; for(int i=0;i<32;i++) s+=e[i]*e[i];\n"
     "  return ("+t+")(unsigned)s; }\n",
     {0x4cULL}, "OptStress10", 3},

    // dot product of two arrays: -O3 multiply-accumulate vectorization.
    {p+"_dotp",
     t+" "+p+"_dotp("+t+" a){\n"
     "  unsigned u[24],v[24]; unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<24;i++){ x=x*1103515245u+12345u; u[i]=(x>>10)&0xfffu;\n"
     "    x=x*1103515245u+12345u; v[i]=(x>>10)&0xfffu; }\n"
     "  unsigned s=0; for(int i=0;i<24;i++) s+=u[i]*v[i];\n"
     "  return ("+t+")(unsigned)s; }\n",
     {0x9bULL}, "OptStress10", 3},

    // max of absolute values: -O3 abs + signed-max vectorized reduction.
    {p+"_maxabs",
     t+" "+p+"_maxabs("+t+" a){\n"
     "  int e[32]; unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<32;i++){ x=x*1103515245u+12345u; e[i]=(int)((x>>7)&0x1ffffu)-0x10000; }\n"
     "  int m=0; for(int i=0;i<32;i++){ int v=e[i]<0?-e[i]:e[i]; if(v>m) m=v; }\n"
     "  return ("+t+")(unsigned)m; }\n",
     {0xa7ULL}, "OptStress10", 3},

    // count elements above a runtime threshold: -O3 compare-mask reduction.
    {p+"_cntpred",
     t+" "+p+"_cntpred("+t+" a){\n"
     "  unsigned e[40]; unsigned x=(unsigned)a|1u;\n"
     "  unsigned thr=((unsigned)a&0xffffu)|0x100u;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u; e[i]=(x>>8)&0xffffu; }\n"
     "  unsigned cnt=0; for(int i=0;i<40;i++) cnt += (e[i]>thr)?1u:0u;\n"
     "  return ("+t+")(unsigned)(cnt*2654435761u); }\n",
     {0x35ULL}, "OptStress10", 3},

    // running saturating sum clamped to 16 bits: loop-carried scalar cmov clamp.
    {p+"_satsum",
     t+" "+p+"_satsum("+t+" a){\n"
     "  unsigned e[32]; unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<32;i++){ x=x*1103515245u+12345u; e[i]=(x>>16)&0x1fffu; }\n"
     "  unsigned s=0; for(int i=0;i<32;i++){ s+=e[i]; if(s>0xffffu) s=0xffffu; }\n"
     "  return ("+t+")(unsigned)s; }\n",
     {0x6dULL}, "OptStress10", 2},

    // splitmix64 mixing with byte extraction (wide 64-bit on 32-bit targets).
    {p+"_mix64",
     t+" "+p+"_mix64("+t+" a){\n"
     "  unsigned long long z=(unsigned long long)(unsigned)a + 0x9e3779b97f4a7c15ULL;\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<16;i++){\n"
     "    z += 0x9e3779b97f4a7c15ULL;\n"
     "    unsigned long long w=z;\n"
     "    w=(w^(w>>30))*0xbf58476d1ce4e5b9ULL;\n"
     "    w=(w^(w>>27))*0x94d049bb133111ebULL;\n"
     "    w=w^(w>>31);\n"
     "    h=h*131u+(unsigned)(w&0xffu)+(unsigned)((w>>32)&0xffu); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "OptStress10", 2},

    // in-place prefix sum (carried scan) then a hash fold.
    {p+"_prefix",
     t+" "+p+"_prefix("+t+" a){\n"
     "  unsigned e[32]; unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<32;i++){ x=x*1103515245u+12345u; e[i]=(x>>12)&0xffu; }\n"
     "  for(int i=1;i<32;i++) e[i]+=e[i-1];\n"
     "  unsigned h=0; for(int i=0;i<32;i++) h=h*131u+e[i];\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x29ULL}, "OptStress10", 3},

    // histogram scatter into a small stack array by a runtime index.
    {p+"_histmod",
     t+" "+p+"_histmod("+t+" a){\n"
     "  unsigned bin[8]={0,0,0,0,0,0,0,0}; unsigned x=(unsigned)a|1u;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u; bin[(x>>13)&7u]+=((x>>3)&0xfu)+1u; }\n"
     "  unsigned h=0; for(int i=0;i<8;i++) h=h*131u+bin[i];\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x57ULL}, "OptStress10", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress10TC("x64o10", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress10TC("x86o10", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress10TC("a64o10", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress10TC("armo10", "int");

INSTANTIATE_TEST_SUITE_P(OptStress10, X64OptStress10RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress10, X86OptStress10RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress10, A64OptStress10RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress10, ARM32OptStress10RT, ::testing::ValuesIn(kARM), rtTCName);
