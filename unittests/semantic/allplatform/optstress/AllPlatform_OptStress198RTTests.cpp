//===- AllPlatform_OptStress198RTTests.cpp - derange / mobius / selection ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * derange   - derangement count DP: D[0]=1, D[1]=0, D[i]=(i-1)(D[i-1]+D[i-2])
//                 summed for rodata/seed-derived n.  Pins a second-order linear
//                 recurrence fold (distinct from Catalan #135 and Stirling #173).
//   * mobius    - Möbius function mu(n) via trial division over rodata primes:
//                 square-free check and parity of distinct prime factors.  Pins a
//                 small-prime factorization scan (distinct from totient #130/#165).
//   * selection - selection-sort swap census over a rodata-seeded array: repeated
//                 min-scan swaps.  Pins an O(n^2) in-place reorder with swap count
//                 (distinct from shell/gnome/cocktail/comb sorts in #184/#189).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress198RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress198RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress198RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress198RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress198RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress198RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress198RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress198RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress198TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // derangement count DP summed for rodata/seed-derived n.
    {p+"_derange",
     "static const unsigned char "+p+"_dr[8]={3,7,11,5,13,2,9,17};\n"
     +t+" "+p+"_derange("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned n=4u+(((((unsigned)"+p+"_dr[s&7u])<<2)^s)%9u);\n"
     "    unsigned D[12]; D[0]=1u; D[1]=0u; unsigned sum=D[0]+D[1];\n"
     "    for(unsigned i=2u;i<=n && i<12u;i++){ D[i]=(i-1u)*(D[i-1u]+D[i-2u]);\n"
     "      sum=sum*131u+D[i]; }\n"
     "    acc=acc*131u+sum+n; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x19u}, "OptStress198", 2},

    // Möbius function via trial division over rodata primes.
    {p+"_mobius",
     "static const unsigned char "+p+"_pr[10]={2,3,5,7,11,13,17,19,23,29};\n"
     +t+" "+p+"_mobius("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned n=30u+(((((unsigned)"+p+"_pr[s&7u])<<3)^s)%970u);\n"
     "    unsigned x=n, mu=1u, sq=0u, fac=0u;\n"
     "    for(int i=0;i<10;i++){ unsigned p=(unsigned)"+p+"_pr[i]; if(p*p>x) break;\n"
     "      if(x%p==0u){ fac++; unsigned cnt=0u; while(x%p==0u){ x/=p; cnt++; }\n"
     "        if(cnt>1u) sq=1u; mu=(unsigned)(-(int)mu); } }\n"
     "    if(x>1u){ fac++; mu=(unsigned)(-(int)mu); }\n"
     "    if(sq) mu=0u; acc=acc*131u+mu*1311u+fac+n; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Au}, "OptStress198", 2},

    // selection-sort swap census over a rodata-seeded array.
    {p+"_selection",
     "static const unsigned char "+p+"_sel[12]={41,7,63,19,55,3,72,28,11,49,36,22};\n"
     +t+" "+p+"_selection("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned A[12]; for(int i=0;i<12;i++) A[i]=((unsigned)"+p+"_sel[i])^((s>>(i&3))&7u);\n"
     "    unsigned swaps=0u, fold=0u; int n=12;\n"
     "    for(int i=0;i<n-1;i++){ int min=i;\n"
     "      for(int j=i+1;j<n;j++) if(A[j]<A[min]) min=j;\n"
     "      if(min!=i){ unsigned t=A[i]; A[i]=A[min]; A[min]=t; swaps++; }\n"
     "      fold=fold*131u+A[i]; }\n"
     "    acc=acc*131u+swaps*1311u+fold+A[n-1]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x1Bu}, "OptStress198", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress198TC("x64o198", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress198TC("x86o198", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress198TC("a64o198", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress198TC("armo198", "int");

INSTANTIATE_TEST_SUITE_P(OptStress198, X64OptStress198RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress198, X86OptStress198RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress198, A64OptStress198RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress198, ARM32OptStress198RT, ::testing::ValuesIn(kARM), rtTCName);
