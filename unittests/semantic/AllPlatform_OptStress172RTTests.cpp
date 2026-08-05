//===- AllPlatform_OptStress172RTTests.cpp - LCP / diff-array / rain water ===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain base+index copies and folds a result that depends only on the
// bytes + control flow (never an absolute VA), so nothing touches the deferred
// i386/ARM32 PIC rodata *interior*-pointer model (#477/#487); every probe runs
// on all four targets.
//
//   * lcp        - longest common prefix of several rodata "strings": scan
//                  columns top-to-bottom and stop at the first column where the
//                  rows disagree.  Pins a vertical multi-string prefix scan
//                  (distinct from the edit/LCS/KMP string shapes in #146/#129).
//   * diffarray  - difference-array range updates: add a delta over [l,r] by two
//                  endpoint bumps, then a single prefix pass materialises the
//                  array.  Pins the difference-array range-add idiom (distinct
//                  from the Fenwick #129, segment tree #168 and sqrt-block #169
//                  point structures).
//   * rainwater  - trapping rain water by the two-pointer running-max method.
//                  Pins a bidirectional max-bounded accumulation (distinct from
//                  the Kadane #136 and window/peak scans in #160).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress172RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress172RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress172RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress172RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress172RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress172RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress172RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress172RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress172TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // longest common prefix of 4 rodata rows (vertical column scan).
    {p+"_lcp",
     "static const unsigned char "+p+"_lp[32]={2,5,9,1,7,4,3,8,2,5,9,1,6,4,3,8,2,5,9,7,7,4,3,8,2,5,1,1,7,4,3,8};\n"
     +t+" "+p+"_lcp("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned lcp=0u;\n"
     "    for(int col=0;col<8;col++){ unsigned c0=((unsigned)"+p+"_lp[col]^((s>>col)&1u))&15u; int same=1;\n"
     "      for(int k=1;k<4;k++){ unsigned ck=((unsigned)"+p+"_lp[k*8+col]^((s>>(k+col))&1u))&15u; if(ck!=c0){ same=0; break; } }\n"
     "      if(same) lcp++; else break; }\n"
     "    acc=acc*131u+lcp; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x30u}, "OptStress172", 2},

    // difference-array range adds, then a prefix pass materialises the array.
    {p+"_diffarray",
     "static const unsigned char "+p+"_da[16]={3,5,0,7,9,2,12,4,6,1,14,8,10,3,5,11};\n"
     +t+" "+p+"_diffarray("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int d[17]; for(int i=0;i<17;i++) d[i]=0;\n"
     "    for(int q=0;q<8;q++){ unsigned l=((unsigned)"+p+"_da[q*2]^((s>>q)&7u))%16u;\n"
     "      unsigned r=l+(((unsigned)"+p+"_da[q*2+1]^((s>>(q+1))&7u))%(16u-l)); int val=(int)(((s>>q)&15u)+1u);\n"
     "      d[l]+=val; d[r+1]-=val; }\n"
     "    int cur=0; unsigned hh=0u; for(int i=0;i<16;i++){ cur+=d[i]; hh=hh*131u+(unsigned)cur; }\n"
     "    acc=acc*131u+hh; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x41u}, "OptStress172", 2},

    // trapping rain water via the two-pointer running-max method.
    {p+"_rainwater",
     "static const unsigned char "+p+"_rw[16]={6,2,9,1,12,4,7,3,11,0,14,5,8,2,10,1};\n"
     +t+" "+p+"_rainwater("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned h[16]; for(int i=0;i<16;i++) h[i]=((unsigned)"+p+"_rw[i]^((s>>(i&7))&3u))&15u;\n"
     "    int lo=0, hi=15; unsigned lmax=0u, rmax=0u, water=0u;\n"
     "    while(lo<hi){ if(h[lo]<=h[hi]){ if(h[lo]>=lmax) lmax=h[lo]; else water+=lmax-h[lo]; lo++; }\n"
     "      else { if(h[hi]>=rmax) rmax=h[hi]; else water+=rmax-h[hi]; hi--; } acc=acc*131u+water+(unsigned)lo+(unsigned)hi; }\n"
     "    acc=acc*131u+water; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x72u}, "OptStress172", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress172TC("x64o172", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress172TC("x86o172", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress172TC("a64o172", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress172TC("armo172", "int");

INSTANTIATE_TEST_SUITE_P(OptStress172, X64OptStress172RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress172, X86OptStress172RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress172, A64OptStress172RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress172, ARM32OptStress172RT, ::testing::ValuesIn(kARM), rtTCName);
