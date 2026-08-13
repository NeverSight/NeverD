//===- AllPlatform_OptStress128RTTests.cpp - CRT / Jacobi / knapsack shapes -=//
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
//   * crt    - Chinese Remainder reconstruction from rodata (modulus, residue)
//              triples via Garner's sequential formula (products stay < 2^32).
//              Pins a sequential CRT combine over rodata modulus/residue pairs.
//   * jacobi - Jacobi symbol (a|n) over rodata (a,n) pairs: the quadratic-
//              reciprocity loop with `(a,n)=(n%a,a)` and sign flips.  Pins a
//              variable-trip gcd-style recurrence with conditional sign tracking.
//   * knap   - 0/1 knapsack DP over rodata (weight,value) pairs: the forward
//              sweep `dp[s]=max(dp[s],dp[s-w]+v)`.  Pins a max-of-two forward DP
//              (distinct from subset-sum reachability which only ORs bits).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress128RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress128RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress128RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress128RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress128RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress128RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress128RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress128RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress128TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Chinese Remainder reconstruction from rodata (modulus,residue) triples.
    {p+"_crt",
     "static const unsigned char "+p+"_mod[9]={7,11,13,17,19,23,29,31,37};\n"
     "static const unsigned char "+p+"_res[9]={3,5,2,8,4,6,1,9,7};\n"
     +t+" "+p+"_crt("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned x=0u, M=1u;\n"
     "    for(int i=0;i<9;i++){ unsigned m="+p+"_mod[i], r="+p+"_res[i]^((s>>(i&7))&1u);\n"
     "      unsigned t2=((r+M-x%m)*2654435761u)%m; x+=t2*M; M*=m; }\n"
     "    acc=acc*131u+(x&0xFFFFu)+M; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Cu}, "OptStress128", 2},

    // Jacobi symbol (a|n) over rodata (a,n) pairs (quadratic reciprocity loop).
    {p+"_jacobi",
     "static const unsigned char "+p+"_aa[16]={\n"
     "15,33,57,91,21,45,77,13, 39,63,85,11,27,51,73,9};\n"
     "static const unsigned char "+p+"_nn[16]={\n"
     "7,11,13,17,19,23,29,31, 37,41,43,47,53,59,61,67};\n"
     +t+" "+p+"_jacobi("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i<16;i++){ unsigned a0="+p+"_aa[i]+((s>>(i&7))&3u);\n"
     "      unsigned n0="+p+"_nn[i]|1u; a0%=n0; int sign=1;\n"
     "      while(a0){ if(a0&1u){ if((n0&3u)==3u && (a0&3u)==3u) sign=-sign;\n"
     "        if((n0&7u)==3u || (n0&7u)==5u) sign=-sign; unsigned t=a0; a0=n0%a0; n0=t; }\n"
     "        else { if((n0&7u)==3u || (n0&7u)==5u) sign=-sign; a0>>=1; } }\n"
     "      acc=acc*131u+(sign>0?1u:0u)+n0; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x33u}, "OptStress128", 2},

    // 0/1 knapsack DP over rodata (weight,value) pairs (forward max-of-two).
    {p+"_knap",
     "static const unsigned char "+p+"_wv[32]={\n"
     "3,7, 5,11, 2,5, 8,13, 4,9, 6,12, 1,3, 7,15, 2,6, 5,10, 9,18, 3,8, 6,14, 4,7, 8,16, 1,2};\n"
     +t+" "+p+"_knap("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned cap=40u+((s>>4)&7u); unsigned dp[64]; for(int i=0;i<64;i++) dp[i]=0u;\n"
     "    for(int i=0;i<16;i++){ unsigned w="+p+"_wv[i*2]+1u, v="+p+"_wv[i*2+1];\n"
     "      for(int sm=63;sm>=(int)w;sm--){ unsigned alt=dp[sm-w]+v; if(alt>dp[sm]) dp[sm]=alt; } }\n"
     "    for(int i=0;i<64;i++) acc=acc*131u+dp[i]; acc=acc*131u+dp[cap&63u];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x8Fu}, "OptStress128", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress128TC("x64o128", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress128TC("x86o128", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress128TC("a64o128", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress128TC("armo128", "int");

INSTANTIATE_TEST_SUITE_P(OptStress128, X64OptStress128RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress128, X86OptStress128RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress128, A64OptStress128RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress128, ARM32OptStress128RT, ::testing::ValuesIn(kARM), rtTCName);
