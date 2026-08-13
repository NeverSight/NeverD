//===- AllPlatform_OptStress130RTTests.cpp - sieve / Pascal / totient ====//
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
//   * sieve  - Sieve of Eratosthenes marking sweep over a stack bitset with the
//              primes folded through a rodata gather.  Pins a `j+=i` multiples-
//              marking sweep (distinct from the Miller-Rabin per-number modpow
//              witness in #126 - this is a batch composite mark, no division).
//   * pascal - Pascal's triangle rolled in one row modulo a rodata prime, with a
//              per-row binomial gather.  Pins an additive 2D recurrence
//              `row[c]+=row[c-1]` (distinct from the max-of-two knapsack DP).
//   * totient- Euler's totient by trial division using a rodata prime list,
//              `phi-=phi/p` while stripping each factor.  Pins a divisor-stripping
//              factorization (distinct from the gcd recurrence in #128 jacobi).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress130RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress130RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress130RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress130RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress130RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress130RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress130RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress130RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress130TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Sieve of Eratosthenes marking sweep + rodata gather of the primes found.
    {p+"_sieve",
     "static const unsigned char "+p+"_pr[16]={\n"
     "2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53};\n"
     +t+" "+p+"_sieve("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned comp[64]; for(int i=0;i<64;i++) comp[i]=0u;\n"
     "    unsigned lim=40u+((s>>4)&15u);\n"
     "    for(unsigned i=2u;i*i<lim;i++) if(!comp[i]) for(unsigned j=i*i;j<lim;j+=i) comp[j]=1u;\n"
     "    unsigned cnt=0u;\n"
     "    for(unsigned i=2u;i<lim;i++) if(!comp[i]){ cnt++; acc=acc*131u+i+"+p+"_pr[cnt&15u]; }\n"
     "    acc=acc*131u+cnt; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Bu}, "OptStress130", 2},

    // Pascal's triangle rolled in one row modulo a rodata prime, binomial gather.
    {p+"_pascal",
     "static const unsigned char "+p+"_mod[8]={251,241,239,233,229,227,223,211};\n"
     +t+" "+p+"_pascal("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned m="+p+"_mod[(s>>3)&7u]; unsigned row[24];\n"
     "    row[0]=1u; for(int i=1;i<24;i++) row[i]=0u;\n"
     "    for(int r=1;r<24;r++){ for(int c=r;c>0;c--) row[c]=(row[c]+row[c-1])%m;\n"
     "      acc=acc*131u+row[(s>>(r&7))&15u]; }\n"
     "    for(int i=0;i<24;i++) acc=acc*131u+row[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x97u}, "OptStress130", 2},

    // Euler totient via trial division over a rodata prime list (factor strip).
    {p+"_totient",
     "static const unsigned char "+p+"_pr2[12]={2,3,5,7,11,13,17,19,23,29,31,37};\n"
     +t+" "+p+"_totient("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int q=0;q<24;q++){ unsigned n=1000u+((s>>(q&15))&1023u); unsigned phi=n, m=n;\n"
     "      for(int pi=0;pi<12;pi++){ unsigned pr="+p+"_pr2[pi]; if(pr*pr>m) break;\n"
     "        if(m%pr==0u){ phi-=phi/pr; while(m%pr==0u) m/=pr; } }\n"
     "      if(m>1u) phi-=phi/m; acc=acc*131u+phi; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x39u}, "OptStress130", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress130TC("x64o130", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress130TC("x86o130", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress130TC("a64o130", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress130TC("armo130", "int");

INSTANTIATE_TEST_SUITE_P(OptStress130, X64OptStress130RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress130, X86OptStress130RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress130, A64OptStress130RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress130, ARM32OptStress130RT, ::testing::ValuesIn(kARM), rtTCName);
