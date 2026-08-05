//===- AllPlatform_OptStress150RTTests.cpp - knapsack / coin / subset-sum =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * knap   - 0/1 knapsack value maximization over rodata weight/value pairs: a
//              one-row DP swept from high capacity down so each item is used at
//              most once.  Pins a reverse-capacity max DP (distinct from the
//              edit/LCS/LIS min-and-extend DPs in #146).
//   * coin   - coin-change way counting over rodata denominations: an unbounded
//              forward DP accumulates the number of ways to reach each amount.
//              Pins an additive count DP (distinct from the knapsack max DP above
//              and from any greedy change-making).
//   * subset - subset-sum reachability over a rodata element set: a 0/1 reverse
//              sweep marks which target totals become reachable as each element
//              is offered at most once.  Pins a boolean reachability lattice
//              (distinct from the knapsack value max and coin way-count DPs).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress150RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress150RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress150RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress150RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress150RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress150RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress150RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress150RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress150TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 0/1 knapsack value maximization over rodata weight/value pairs.
    {p+"_knap",
     "static const unsigned char "+p+"_w[12]={3,5,2,7,4,6,1,8,5,3,9,2};\n"
     "static const unsigned char "+p+"_v[12]={40,90,21,77,33,66,15,88,45,30,99,12};\n"
     +t+" "+p+"_knap("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned dp[25]; for(int c=0;c<25;c++) dp[c]=0u;\n"
     "    for(int i=0;i<12;i++){ unsigned wi=((unsigned)"+p+"_w[i]&15u)+1u, vi=(unsigned)"+p+"_v[i]^((s>>(i&7))&7u);\n"
     "      for(int c=24;c>=(int)wi;c--){ unsigned cand=dp[c-(int)wi]+vi; if(cand>dp[c]) dp[c]=cand; acc=acc*131u+dp[c]; } }\n"
     "    acc=acc*131u+dp[24]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Au}, "OptStress150", 2},

    // coin-change way counting over rodata denominations (additive DP).
    {p+"_coin",
     "static const unsigned char "+p+"_den[6]={1,2,5,7,11,13};\n"
     +t+" "+p+"_coin("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned ways[31]; for(int i=0;i<31;i++) ways[i]=0u; ways[0]=1u;\n"
     "    for(int k=0;k<6;k++){ unsigned c=(unsigned)"+p+"_den[k]+((s>>(k&7))&1u);\n"
     "      for(int amt=(int)c;amt<=30;amt++){ ways[amt]+=ways[amt-(int)c]; acc=acc*131u+ways[amt]; } }\n"
     "    acc=acc*131u+ways[30]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Bu}, "OptStress150", 2},

    // subset-sum reachability over a rodata element set (0/1 reverse boolean DP).
    {p+"_subset",
     "static const unsigned char "+p+"_set[12]={3,5,2,7,4,6,1,8,5,3,9,2};\n"
     +t+" "+p+"_subset("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned reach[25]; for(int i=0;i<25;i++) reach[i]=0u; reach[0]=1u;\n"
     "    for(int i=0;i<12;i++){ unsigned e=((unsigned)"+p+"_set[i]&15u)+((s>>(i&7))&1u);\n"
     "      if(e==0u) e=1u; if(e>24u) e=24u;\n"
     "      for(int amt=24;amt>=(int)e;amt--){ if(reach[amt-(int)e]) reach[amt]=1u; acc=acc*131u+reach[amt]; } }\n"
     "    unsigned cnt=0u; for(int i=0;i<=24;i++){ if(reach[i]) cnt++; acc=acc*131u+reach[i]*(unsigned)(i+1); }\n"
     "    acc=acc*131u+cnt; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x5Eu}, "OptStress150", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress150TC("x64o150", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress150TC("x86o150", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress150TC("a64o150", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress150TC("armo150", "int");

INSTANTIATE_TEST_SUITE_P(OptStress150, X64OptStress150RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress150, X86OptStress150RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress150, A64OptStress150RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress150, ARM32OptStress150RT, ::testing::ValuesIn(kARM), rtTCName);
