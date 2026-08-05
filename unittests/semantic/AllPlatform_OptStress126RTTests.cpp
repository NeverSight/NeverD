//===- AllPlatform_OptStress126RTTests.cpp - LCS / witness / bitonic shapes =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * lcs    - longest-common-subsequence DP over two rodata strings: a rolling
//              one-row table `dp[j]=max(dp[j],dp[j-1]+match)`.  Pins a max-of-two
//              DP recurrence reading two rodata arrays (distinct from edit
//              distance which uses min-of-three).
//   * witness- Miller-Rabin compositeness witness loop over rodata (n,a) pairs:
//              write n-1 as d*2^r then a modular exponentiation chain under a
//              CONSTANT modulus.  Pins a bit-scan + square-and-multiply on rodata
//              operands (distinct from the standalone modpow batch).
//   * bitonic- bitonic merge network over a rodata-seeded stack vector: compare-
//              exchange stages at strides 1,2,4,...  Pins a fixed-stride butterfly
//              compare-swap network (distinct from the WHT add/sub butterfly).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress126RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress126RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress126RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress126RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress126RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress126RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress126RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress126RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress126TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // longest-common-subsequence DP over two rodata strings (rolling row, max-of-two).
    {p+"_lcs",
     "static const unsigned char "+p+"_sa[12]={3,9,1,7,12,5,2,14,6,10,4,8};\n"
     "static const unsigned char "+p+"_sb[12]={9,1,7,3,5,12,14,2,10,6,8,4};\n"
     +t+" "+p+"_lcs("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned prev[13], cur[13]; for(int j=0;j<=12;j++) prev[j]=0u;\n"
     "    for(int i=1;i<=12;i++){ cur[0]=0u; unsigned ca=("+p+"_sa[i-1]^(s&3u))&0xFu;\n"
     "      for(int j=1;j<=12;j++){ unsigned cb=("+p+"_sb[j-1]^((s>>2)&3u))&0xFu;\n"
     "        unsigned add=(ca==cb)?prev[j-1]+1u:0u; cur[j]=prev[j]>add?prev[j]:add;\n"
     "        if(cur[j]<prev[j-1]) cur[j]=prev[j-1]; }\n"
     "      for(int j=0;j<=12;j++) prev[j]=cur[j]; }\n"
     "    acc=acc*131u+prev[12]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Fu}, "OptStress126", 2},

    // Miller-Rabin witness loop over rodata (n,a) pairs (modular exponentiation).
    {p+"_witness",
     "static const unsigned char "+p+"_nn[12]={\n"
     "91,57,121,85,143,77,169,65, 187,55,209,49};\n"
     "static const unsigned char "+p+"_aa[12]={\n"
     "2,3,2,5,2,7,3,2, 5,3,2,7};\n"
     +t+" "+p+"_witness("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i<12;i++){ unsigned n="+p+"_nn[i]+((s>>(i&7))&3u); if(n<4u){ acc=acc*131u+1u; continue; }\n"
     "      unsigned d=n-1u, r=0u; while((d&1u)==0u){ d>>=1u; r++; }\n"
     "      unsigned base=("+p+"_aa[i]&7u)+2u, x=1u, e=d;\n"
     "      while(e){ if(e&1u) x=(x*base)%n; base=(base*base)%n; e>>=1u; }\n"
     "      unsigned comp=(x!=1u && x!=n-1u)?1u:0u; acc=acc*131u+comp+r; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x8Bu}, "OptStress126", 2},

    // bitonic merge compare-exchange network over a rodata-seeded stack vector.
    {p+"_bitonic",
     "static const unsigned char "+p+"_v[16]={5,12,30,18,7,44,21,9,33,16,52,3,27,14,40,8};\n"
     +t+" "+p+"_bitonic("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned x[16]; for(int i=0;i<16;i++) x[i]="+p+"_v[i]^((s>>(i&7))&7u);\n"
     "    for(int k=2;k<=16;k<<=1){\n"
     "      for(int j=k>>1;j>0;j>>=1){\n"
     "        for(int i=0;i+j<16;i+=k){\n"
     "          unsigned a0=x[i], b0=x[i+j]; unsigned dir=((i/k)&1u)?1u:0u;\n"
     "          if((!dir && a0>b0)||(dir && a0<b0)){ x[i]=b0; x[i+j]=a0; } } } }\n"
     "    for(int i=0;i<16;i++) acc=acc*131u+x[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x61u}, "OptStress126", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress126TC("x64o126", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress126TC("x86o126", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress126TC("a64o126", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress126TC("armo126", "int");

INSTANTIATE_TEST_SUITE_P(OptStress126, X64OptStress126RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress126, X86OptStress126RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress126, A64OptStress126RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress126, ARM32OptStress126RT, ::testing::ValuesIn(kARM), rtTCName);
