//===- AllPlatform_OptStress135RTTests.cpp - coin-change / LIS / Catalan ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * coin   - unbounded coin-change WAYS count DP over rodata denominations:
//              `dp[x]+=dp[x-coin]`.  Pins an additive count DP with an affine
//              inner index (distinct from the max-of-two 0/1 knapsack in #128).
//   * lis    - longest-increasing-subsequence O(n^2) DP over a rodata sequence:
//              `dp[i]=max(dp[i],dp[j]+1)` for j<i with val[j]<val[i].  Pins an
//              all-pairs predecessor max-DP (distinct from any rolling-row DP).
//   * catalan- Catalan numbers by the convolution recurrence `C[n]=sum C[i]
//              C[n-1-i]` modulo a rodata prime.  Pins a self-convolution DP
//              (distinct from Pascal's additive triangle in #130).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress135RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress135RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress135RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress135RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress135RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress135RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress135RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress135RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress135TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // unbounded coin-change ways count DP over rodata denominations (affine inner).
    {p+"_coin",
     "static const unsigned char "+p+"_den[8]={1,2,5,10,3,7,4,6};\n"
     +t+" "+p+"_coin("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned dp[32]; for(int i=0;i<32;i++) dp[i]=0u; dp[0]=1u;\n"
     "    unsigned target=20u+((s>>4)&7u);\n"
     "    for(int c=0;c<8;c++){ unsigned coin="+p+"_den[c]+((s>>(c&7))&1u); if(coin==0u) coin=1u;\n"
     "      for(unsigned x=coin;x<32u;x++) dp[x]=(dp[x]+dp[x-coin])&0xFFFFu; }\n"
     "    for(int i=0;i<32;i++) acc=acc*131u+dp[i]; acc=acc*131u+dp[target&31u];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x33u}, "OptStress135", 2},

    // longest-increasing-subsequence O(n^2) predecessor max-DP over rodata seq.
    {p+"_lis",
     "static const unsigned char "+p+"_seq[16]={\n"
     "8,3,12,5,9,1,14,7, 2,11,6,15,4,10,13,0};\n"
     +t+" "+p+"_lis("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned val[16], dp[16];\n"
     "    for(int i=0;i<16;i++){ val[i]="+p+"_seq[i]^((s>>(i&7))&7u); dp[i]=1u; }\n"
     "    for(int i=0;i<16;i++) for(int j=0;j<i;j++)\n"
     "      if(val[j]<val[i] && dp[j]+1u>dp[i]) dp[i]=dp[j]+1u;\n"
     "    unsigned best=0u;\n"
     "    for(int i=0;i<16;i++){ if(dp[i]>best) best=dp[i]; acc=acc*131u+dp[i]; }\n"
     "    acc=acc*131u+best; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x5Au}, "OptStress135", 2},

    // Catalan numbers by self-convolution recurrence modulo a rodata prime.
    {p+"_catalan",
     "static const unsigned char "+p+"_mod[8]={251,241,239,233,229,227,223,211};\n"
     +t+" "+p+"_catalan("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned m="+p+"_mod[(s>>3)&7u]; unsigned C[20]; C[0]=1u;\n"
     "    for(int n=1;n<20;n++){ unsigned sum=0u;\n"
     "      for(int i=0;i<n;i++) sum=(sum+C[i]*C[n-1-i])%m;\n"
     "      C[n]=sum; acc=acc*131u+C[n]; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Eu}, "OptStress135", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress135TC("x64o135", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress135TC("x86o135", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress135TC("a64o135", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress135TC("armo135", "int");

INSTANTIATE_TEST_SUITE_P(OptStress135, X64OptStress135RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress135, X86OptStress135RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress135, A64OptStress135RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress135, ARM32OptStress135RT, ::testing::ValuesIn(kARM), rtTCName);
