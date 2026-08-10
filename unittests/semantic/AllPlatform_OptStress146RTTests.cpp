//===- AllPlatform_OptStress146RTTests.cpp - editdist / LCS / LIS =//
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
//   * editd - Levenshtein edit distance between two rodata strings via a rolling
//             one-row DP: each cell takes the min of delete/insert/substitute
//             with the diagonal carried in a scalar.  Pins a three-way min DP
//             recurrence (distinct from the single-window Z-array in #142 and
//             the KMP failure table in #129).
//   * lcs   - longest common subsequence of two rodata strings via a rolling
//             one-row DP: a match extends the diagonal, a mismatch carries the
//             max of the two neighbours.  Pins a match/extend DP (distinct from
//             the edit-distance min recurrence above and any prefix scan).
//   * lis   - longest increasing subsequence of a rodata array via the O(n^2)
//             DP: each element extends the best strictly-smaller predecessor.
//             Pins a quadratic predecessor-chain DP (distinct from any single
//             monotonic-stack or counting pass).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress146RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress146RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress146RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress146RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress146RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress146RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress146RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress146RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress146TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Levenshtein edit distance between two rodata strings (rolling one-row DP).
    {p+"_editd",
     "static const unsigned char "+p+"_sa[12]={7,2,9,2,5,1,8,2,3,6,2,4};\n"
     "static const unsigned char "+p+"_sb[12]={2,9,4,2,5,7,2,1,3,2,6,4};\n"
     +t+" "+p+"_editd("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned A[12],B[12];\n"
     "    for(int i=0;i<12;i++){ A[i]=(unsigned)"+p+"_sa[i]^((s>>(i&7))&1u); B[i]=(unsigned)"+p+"_sb[i]^((s>>((i+3)&7))&1u); }\n"
     "    unsigned dp[13];\n"
     "    for(int j=0;j<=12;j++) dp[j]=(unsigned)j;\n"
     "    for(int i=1;i<=12;i++){ unsigned prev=dp[0]; dp[0]=(unsigned)i;\n"
     "      for(int j=1;j<=12;j++){ unsigned cur=dp[j];\n"
     "        unsigned cost=(A[i-1]==B[j-1])?0u:1u;\n"
     "        unsigned del=dp[j]+1u, ins=dp[j-1]+1u, sub=prev+cost;\n"
     "        unsigned m=del<ins?del:ins; if(sub<m) m=sub; dp[j]=m;\n"
     "        prev=cur; acc=acc*131u+dp[j]; } }\n"
     "    acc=acc*131u+dp[12]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x16u}, "OptStress146", 2},

    // longest common subsequence of two rodata strings (rolling one-row DP).
    {p+"_lcs",
     "static const unsigned char "+p+"_la[12]={3,1,4,1,5,9,2,6,5,3,5,8};\n"
     "static const unsigned char "+p+"_lb[12]={1,4,1,5,9,2,6,5,8,9,7,3};\n"
     +t+" "+p+"_lcs("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned A[12],B[12];\n"
     "    for(int i=0;i<12;i++){ A[i]=(unsigned)"+p+"_la[i]^((s>>(i&7))&1u); B[i]=(unsigned)"+p+"_lb[i]^((s>>((i+5)&7))&1u); }\n"
     "    unsigned dp[13]; for(int j=0;j<=12;j++) dp[j]=0u;\n"
     "    for(int i=1;i<=12;i++){ unsigned prev=0u;\n"
     "      for(int j=1;j<=12;j++){ unsigned cur=dp[j];\n"
     "        if(A[i-1]==B[j-1]) dp[j]=prev+1u;\n"
     "        else dp[j]=(dp[j]>dp[j-1])?dp[j]:dp[j-1];\n"
     "        prev=cur; acc=acc*131u+dp[j]; } }\n"
     "    acc=acc*131u+dp[12]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x27u}, "OptStress146", 2},

    // longest increasing subsequence of a rodata array (O(n^2) DP).
    {p+"_lis",
     "static const unsigned char "+p+"_seq[20]={9,2,15,7,3,20,11,5,18,1,14,8,12,4,17,6,19,10,13,16};\n"
     +t+" "+p+"_lis("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s, best=0u;\n"
     "    unsigned v[20],len[20];\n"
     "    for(int i=0;i<20;i++) v[i]=(unsigned)"+p+"_seq[i]^((s>>(i&7))&3u);\n"
     "    for(int i=0;i<20;i++){ len[i]=1u;\n"
     "      for(int j=0;j<i;j++) if(v[j]<v[i] && len[j]+1u>len[i]) len[i]=len[j]+1u;\n"
     "      if(len[i]>best) best=len[i]; acc=acc*131u+len[i]; }\n"
     "    acc=acc*131u+best; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x38u}, "OptStress146", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress146TC("x64o146", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress146TC("x86o146", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress146TC("a64o146", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress146TC("armo146", "int");

INSTANTIATE_TEST_SUITE_P(OptStress146, X64OptStress146RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress146, X86OptStress146RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress146, A64OptStress146RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress146, ARM32OptStress146RT, ::testing::ValuesIn(kARM), rtTCName);
