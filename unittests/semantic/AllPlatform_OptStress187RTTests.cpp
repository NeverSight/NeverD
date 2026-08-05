//===- AllPlatform_OptStress187RTTests.cpp - divsigma / bisect / next-gtr ===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * divsigma  - divisor-sum (sigma) sieve by additive striding, then an
//                 abundance census (proper-divisor sum vs the value).  Pins an
//                 additive multiples sweep that accumulates per index (distinct
//                 from the primality-marking sieve #181 and the modpow #116).
//   * bisect    - integer cube-root by bisection: narrow [lo,hi] on mid*mid*mid
//                 vs target.  Pins a monotone-predicate binary narrowing on a
//                 computed value (distinct from the array binary search #185).
//   * nextgtr   - next-greater-element via a monotonic decreasing index stack.
//                 Pins the monotonic-stack resolve-on-pop idiom (distinct from
//                 the monotonic deque window-min #179).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress187RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress187RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress187RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress187RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress187RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress187RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress187RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress187RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress187TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // divisor-sum (sigma) sieve by additive striding, then abundance census.
    {p+"_divsigma",
     "static const unsigned char "+p+"_ds[8]={37,12,58,4,29,61,7,44};\n"
     +t+" "+p+"_divsigma("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned N=24u+(((unsigned)"+p+"_ds[s&7u]^(s&15u))%16u);\n"
     "    unsigned sig[40]; for(unsigned i=0;i<40u;i++) sig[i]=0u;\n"
     "    for(unsigned i=1u;i<N;i++) for(unsigned j=i;j<N;j+=i) sig[j]+=i;\n"
     "    unsigned abundant=0u, fold=0u;\n"
     "    for(unsigned i=1u;i<N;i++){ if(sig[i]-i>i) abundant++; fold=fold*131u+sig[i]; }\n"
     "    acc=acc*131u+fold+abundant*131u+N; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x43u}, "OptStress187", 2},

    // integer cube-root by bisection on mid*mid*mid vs target.
    {p+"_bisect",
     "static const unsigned char "+p+"_bi[8]={211,97,143,38,176,52,9,250};\n"
     +t+" "+p+"_bisect("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned target=(((unsigned)"+p+"_bi[s&7u]^(s&255u))*977u+(s&0xFFFFu))%1000000u;\n"
     "    unsigned lo=0u, hi=128u;\n"
     "    while(lo<hi){ unsigned mid=(lo+hi+1u)>>1; if(mid*mid*mid<=target) lo=mid; else hi=mid-1u; }\n"
     "    acc=acc*131u+lo*1000u+(target&1023u); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x52u}, "OptStress187", 2},

    // next-greater-element via a monotonic decreasing index stack.
    {p+"_nextgtr",
     "static const unsigned char "+p+"_ng[20]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31,16,52,3,47};\n"
     +t+" "+p+"_nextgtr("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[20]; for(int i=0;i<20;i++) arr[i]=((unsigned)"+p+"_ng[i]^((s>>(i&7))&7u))&63u;\n"
     "    unsigned res[20]; int stk[20]; int sp=0;\n"
     "    for(int i=0;i<20;i++){ while(sp>0 && arr[stk[sp-1]]<arr[i]){ res[stk[sp-1]]=arr[i]; sp--; } stk[sp++]=i; }\n"
     "    while(sp>0){ res[stk[sp-1]]=0u; sp--; }\n"
     "    unsigned fold=0u; for(int i=0;i<20;i++) fold=fold*131u+res[i]; acc=acc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x83u}, "OptStress187", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress187TC("x64o187", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress187TC("x86o187", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress187TC("a64o187", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress187TC("armo187", "int");

INSTANTIATE_TEST_SUITE_P(OptStress187, X64OptStress187RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress187, X86OptStress187RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress187, A64OptStress187RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress187, ARM32OptStress187RT, ::testing::ValuesIn(kARM), rtTCName);
