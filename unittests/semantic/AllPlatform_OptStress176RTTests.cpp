//===- AllPlatform_OptStress176RTTests.cpp - knapsack / coin-change / LIS ===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain base+index copies and folds a result that depends only on the
// bytes + control flow (never an absolute VA), so nothing touches the deferred
// i386/ARM32 PIC rodata *interior*-pointer model (#477/#487); every probe runs
// on all four targets.
//
//   * knapsack  - 0/1 knapsack via the descending 1-D capacity DP (each item
//                 used at most once); weight and value are both folded from a
//                 single forward rodata byte so the symbol stays at offset 0
//                 (avoids the deferred i386/ARM32 interior-pointer #477).  Pins
//                 the reverse-iteration bounded DP (distinct from the weighted
//                 triangle recurrences in #130/#173).
//   * coinchange- minimum-coins unbounded DP: a forward value sweep relaxes each
//                 denomination repeatedly.  Pins the ascending unbounded DP
//                 (distinct from the 0/1 reverse DP above).
//   * lis       - longest increasing subsequence by the O(n^2) per-element DP.
//                 Pins a prefix-max DP over an order predicate (distinct from the
//                 inversion-pair census #171 and the monotonic-run scan #163).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress176RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress176RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress176RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress176RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress176RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress176RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress176RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress176RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress176TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 0/1 knapsack: descending 1-D capacity DP (w and v folded from one byte).
    {p+"_knapsack",
     "static const unsigned char "+p+"_kp[8]={3,4,2,5,1,6,3,2};\n"
     +t+" "+p+"_knapsack("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned dp[24]; for(int c=0;c<24;c++) dp[c]=0u;\n"
     "    unsigned cap=16u+((s>>3)%8u);\n"
     "    for(int i=0;i<8;i++){ unsigned byte=((unsigned)"+p+"_kp[i]^((s>>(i&7))&1u)); unsigned w=(byte%6u)+1u; unsigned v=(((byte^(s>>(i&3)))%9u))+1u;\n"
     "      for(int c=(int)cap;c>=(int)w;c--){ unsigned cand=dp[c-w]+v; if(cand>dp[c]) dp[c]=cand; } }\n"
     "    acc=acc*131u+dp[cap]; for(int c=0;c<24;c++) acc=acc*131u+dp[c]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x36u}, "OptStress176", 2},

    // minimum-coins unbounded DP (forward value sweep, INF sentinel).
    {p+"_coinchange",
     "static const unsigned char "+p+"_cc[8]={1,3,4,6,2,5,7,3};\n"
     +t+" "+p+"_coinchange("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0, INF=0x3fffffffu;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned coins[4]; for(int i=0;i<4;i++) coins[i]=(((unsigned)"+p+"_cc[i]^((s>>(i&7))&1u))%6u)+1u;\n"
     "    unsigned dp[32]; dp[0]=0u; for(int v=1;v<32;v++) dp[v]=INF;\n"
     "    for(int v=1;v<32;v++) for(int i=0;i<4;i++){ unsigned c=coins[i]; if((unsigned)v>=c && dp[v-c]!=INF && dp[v-c]+1u<dp[v]) dp[v]=dp[v-c]+1u; }\n"
     "    unsigned target=20u+((s>>5)%12u);\n"
     "    acc=acc*131u+(dp[target]==INF?999u:dp[target]); for(int v=0;v<32;v++) acc=acc*131u+(dp[v]==INF?0u:dp[v]); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x47u}, "OptStress176", 2},

    // longest increasing subsequence via the O(n^2) per-element DP.
    {p+"_lis",
     "static const unsigned char "+p+"_li[20]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31,16,52,3,47};\n"
     +t+" "+p+"_lis("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[20]; for(int i=0;i<20;i++) arr[i]=((unsigned)"+p+"_li[i]^((s>>(i&7))&7u))&63u;\n"
     "    unsigned dp[20]; unsigned best=0u;\n"
     "    for(int i=0;i<20;i++){ dp[i]=1u; for(int j=0;j<i;j++) if(arr[j]<arr[i] && dp[j]+1u>dp[i]) dp[i]=dp[j]+1u; if(dp[i]>best) best=dp[i]; }\n"
     "    acc=acc*131u+best; for(int i=0;i<20;i++) acc=acc*131u+dp[i]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x78u}, "OptStress176", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress176TC("x64o176", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress176TC("x86o176", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress176TC("a64o176", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress176TC("armo176", "int");

INSTANTIATE_TEST_SUITE_P(OptStress176, X64OptStress176RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress176, X86OptStress176RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress176, A64OptStress176RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress176, ARM32OptStress176RT, ::testing::ValuesIn(kARM), rtTCName);
