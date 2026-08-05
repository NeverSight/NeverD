//===- AllPlatform_OptStress168RTTests.cpp - seg-tree / ternary / Zeckendorf =//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain base+index copies and folds a result that depends only on the
// bytes + control flow (never an absolute VA), so nothing touches the deferred
// i386/ARM32 PIC rodata *interior*-pointer model (#477/#487); every probe runs
// on all four targets.
//
//   * segtree  - iterative array segment tree over a rodata leaf row: bottom-up
//                build, point updates that re-fold the path to the root, and
//                half-open range-sum queries climbing both endpoints.  Pins a
//                segment-tree range structure (distinct from the Fenwick/BIT in
//                #129, which carries prefix sums in a single flat array).
//   * ternsrch - ternary search for the peak of a unimodal rodata array: probe
//                two interior thirds each step and discard the losing outer
//                third.  Pins a thirds-narrowing search (distinct from the
//                halving binary search in #136/#144 and the peak find in #160).
//   * zeck     - Zeckendorf decomposition: greedily subtract the largest Fibonacci
//                number not exceeding each rodata value, recording the used-term
//                bitmask.  Pins a greedy Fibonacci representation (distinct from
//                the modular Fibonacci recurrence in #153).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress168RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress168RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress168RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress168RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress168RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress168RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress168RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress168RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress168TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // iterative segment tree: build, point update, half-open range-sum query.
    {p+"_segtree",
     "static const unsigned char "+p+"_sg[16]={7,3,12,5,9,1,14,6,2,11,8,4,13,0,10,15};\n"
     +t+" "+p+"_segtree("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned tr[32]; for(int i=0;i<32;i++) tr[i]=0u;\n"
     "    for(int i=0;i<16;i++) tr[16+i]=((unsigned)"+p+"_sg[i]^((s>>(i&7))&3u))&63u;\n"
     "    for(int i=15;i>=1;i--) tr[i]=tr[2*i]+tr[2*i+1];\n"
     "    for(int q=0;q<8;q++){ unsigned pos=(s>>(q&7))&15u, val=(s>>q)&31u;\n"
     "      int idx=16+(int)pos; tr[idx]=val; for(idx/=2; idx>=1; idx/=2) tr[idx]=tr[2*idx]+tr[2*idx+1];\n"
     "      unsigned l=(unsigned)(q&7), r=8u+(unsigned)(q&7), sum=0u; int li=16+(int)l, ri=16+(int)r;\n"
     "      while(li<ri){ if(li&1) sum+=tr[li++]; if(ri&1) sum+=tr[--ri]; li/=2; ri/=2; }\n"
     "      acc=acc*131u+sum+tr[1]; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Cu}, "OptStress168", 2},

    // ternary search for the peak of a unimodal rodata array (thirds narrowing).
    {p+"_ternsrch",
     "static const unsigned char "+p+"_tn[24]={2,0,3,1,2,3,0,1,3,2,1,0,2,3,1,0,3,1,2,0,1,3,2,0};\n"
     +t+" "+p+"_ternsrch("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[24]; for(int i=0;i<24;i++){ unsigned bump=(unsigned)i*(23u-(unsigned)i);\n"
     "      arr[i]=bump*4u+(((unsigned)"+p+"_tn[i]^((s>>(i&7))&1u))&3u); }\n"
     "    int lo=0, hi=23;\n"
     "    while(hi-lo>2){ int m1=lo+(hi-lo)/3, m2=hi-(hi-lo)/3;\n"
     "      if(arr[m1]<arr[m2]) lo=m1+1; else hi=m2-1; acc=acc*131u+(unsigned)lo+(unsigned)hi+arr[m1]; }\n"
     "    unsigned best=0u; for(int i=lo;i<=hi;i++) if(arr[i]>best) best=arr[i];\n"
     "    acc=acc*131u+best+(unsigned)lo; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Du}, "OptStress168", 2},

    // Zeckendorf decomposition: greedy largest-Fibonacci subtraction, term mask.
    {p+"_zeck",
     "static const unsigned char "+p+"_zk[20]={0x9e,0x37,0xc1,0x5a,0x2f,0xe8,0x73,0x14,0xab,0x60,0xdd,0x06,0x99,0x42,0xbf,0x28,0x4d,0xf2,0x81,0x3c};\n"
     +t+" "+p+"_zeck("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned fib[16]; fib[0]=1u; fib[1]=2u; for(int i=2;i<16;i++) fib[i]=fib[i-1]+fib[i-2];\n"
     "    for(int q=0;q<20;q++){ unsigned n=(((unsigned)"+p+"_zk[q]<<3)|((s>>(q&7))&7u))+1u; unsigned rep=0u, terms=0u;\n"
     "      for(int i=15;i>=0;i--){ if(fib[i]<=n){ n-=fib[i]; rep|=(1u<<i); terms++; } }\n"
     "      acc=acc*131u+rep+terms+n; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Eu}, "OptStress168", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress168TC("x64o168", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress168TC("x86o168", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress168TC("a64o168", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress168TC("armo168", "int");

INSTANTIATE_TEST_SUITE_P(OptStress168, X64OptStress168RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress168, X86OptStress168RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress168, A64OptStress168RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress168, ARM32OptStress168RT, ::testing::ValuesIn(kARM), rtTCName);
