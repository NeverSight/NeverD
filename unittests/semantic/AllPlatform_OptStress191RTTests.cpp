//===- AllPlatform_OptStress191RTTests.cpp - matchain / eggdrop / palinsub ==//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * matchain - matrix-chain multiplication order DP: dp[i][j] minimises over a
//                split point k the cost dp[i][k]+dp[k+1][j]+p[i-1]*p[k]*p[j].
//                Pins a triangular DP with an inner SPLIT loop (the O(n^3)
//                interval recurrence, distinct from the 1-D rolling knapsack/coin
//                /edit/LCS/LIS DPs and the 2-D grid DPs elsewhere).
//   * eggdrop  - egg-drop trials DP: dp[e][f]=1+min_x max(dp[e-1][x-1],dp[e][f-x]).
//                Pins a min-of-max interval recurrence (distinct from matchain's
//                additive split and from every pure-min/pure-max DP).
//   * palinsub - longest palindromic SUBSEQUENCE DP over a rodata string: equal
//                ends extend the inner interval by two, else take the better
//                shrink.  Pins an expand/shrink triangular DP (distinct from the
//                Manacher palindrome-SUBSTRING radii #151 and from LCS #126/#146).
//
// Triangular DPs fill only cells with i<=j and read shorter intervals already
// written, so the scratch tables need no full zero-init (only the diagonal /
// boundary base cases), keeping the codegen free of a memset libcall.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress191RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress191RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress191RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress191RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress191RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress191RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress191RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress191RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress191TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // matrix-chain multiplication order DP (triangular, inner split loop).
    {p+"_matchain",
     "static const unsigned char "+p+"_mc[8]={3,5,2,6,4,7,3,5};\n"
     +t+" "+p+"_matchain("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned dim[8]; for(int i=0;i<8;i++) dim[i]=1u+(((unsigned)"+p+"_mc[i]^((s>>(i&7))&3u))&7u);\n"
     "    unsigned dp[8][8]; int n=7;\n"
     "    for(int i=1;i<=n;i++) dp[i][i]=0u;\n"
     "    for(int len=2; len<=n; len++){\n"
     "      for(int i=1; i+len-1<=n; i++){ int j=i+len-1; unsigned best=0xFFFFFFFFu;\n"
     "        for(int k=i;k<j;k++){ unsigned c=dp[i][k]+dp[k+1][j]+dim[i-1]*dim[k]*dim[j];\n"
     "          if(c<best) best=c; } dp[i][j]=best; } }\n"
     "    acc=acc*131u+dp[1][n]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xB1u}, "OptStress191", 2},

    // egg-drop trials DP: dp[e][f]=1+min_x max(dp[e-1][x-1], dp[e][f-x]).
    {p+"_eggdrop",
     "static const unsigned char "+p+"_eg[4]={3,7,2,5};\n"
     +t+" "+p+"_eggdrop("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int E=3; int F=12 + (int)(((unsigned)"+p+"_eg[s&3u]+s)%9u);\n"
     "    unsigned dp[4][24];\n"
     "    for(int e=0;e<=E;e++) dp[e][0]=0u;\n"
     "    for(int f=1;f<=F;f++) dp[1][f]=(unsigned)f;\n"
     "    for(int e=2;e<=E;e++){\n"
     "      for(int f=1;f<=F;f++){ unsigned best=0xFFFFu;\n"
     "        for(int x=1;x<=f;x++){ unsigned br=dp[e-1][x-1]; unsigned wr=dp[e][f-x];\n"
     "          unsigned worst=br>wr?br:wr; unsigned tr=1u+worst; if(tr<best) best=tr; }\n"
     "        dp[e][f]=best; } }\n"
     "    acc=acc*131u+dp[E][F]+(unsigned)F; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xB2u}, "OptStress191", 2},

    // longest palindromic SUBSEQUENCE DP (expand on equal ends, else shrink).
    {p+"_palinsub",
     "static const unsigned char "+p+"_ps[12]={1,2,3,2,1,3,2,1,2,3,1,2};\n"
     +t+" "+p+"_palinsub("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned c[12]; for(int i=0;i<12;i++) c[i]=((unsigned)"+p+"_ps[i]^((s>>(i&7))&1u))&3u;\n"
     "    unsigned dp[12][12]; int n=12;\n"
     "    for(int i=0;i<n;i++) dp[i][i]=1u;\n"
     "    for(int len=2; len<=n; len++){\n"
     "      for(int i=0; i+len-1<n; i++){ int j=i+len-1;\n"
     "        if(c[i]==c[j]) dp[i][j]=(len==2? 2u : dp[i+1][j-1]+2u);\n"
     "        else { unsigned x=dp[i+1][j], y=dp[i][j-1]; dp[i][j]=x>y?x:y; } } }\n"
     "    acc=acc*131u+dp[0][n-1]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xB3u}, "OptStress191", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress191TC("x64o191", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress191TC("x86o191", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress191TC("a64o191", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress191TC("armo191", "int");

INSTANTIATE_TEST_SUITE_P(OptStress191, X64OptStress191RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress191, X86OptStress191RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress191, A64OptStress191RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress191, ARM32OptStress191RT, ::testing::ValuesIn(kARM), rtTCName);
