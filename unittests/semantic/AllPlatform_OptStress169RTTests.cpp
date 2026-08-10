//===- AllPlatform_OptStress169RTTests.cpp - interp-search / sqrt-dec / pancake//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain base+index copies and folds a result that depends only on the
// bytes + control flow (never an absolute VA), so nothing touches the deferred
// i386/ARM32 PIC rodata *interior*-pointer model (#477/#487); every probe runs
// on all four targets.
//
//   * interpsrch - interpolation search over a strictly increasing rodata-built
//                  array: probe at a value-proportional position rather than the
//                  midpoint, then narrow.  Pins a value-interpolated search
//                  (distinct from the halving binary search in #136/#144 and the
//                  signal interpolations in #102/#105/#110).
//   * sqrtdec    - sqrt-decomposition range-sum with point updates: keep a block
//                  sum per sqrt(N) chunk and walk whole blocks where aligned, odd
//                  elements one at a time.  Pins a block-decomposition range
//                  structure (distinct from the Fenwick/BIT in #129 and the
//                  segment tree in #168).
//   * pancake    - pancake sort: repeatedly flip the prefix to drive the running
//                  maximum to the tail.  Pins a prefix-reversal sort (distinct
//                  from the quicksort #151, radix #147, insertion #144 and
//                  bitonic #126 sorts).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress169RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress169RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress169RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress169RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress169RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress169RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress169RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress169RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress169TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // interpolation search over a strictly increasing rodata-built array.
    {p+"_interpsrch",
     "static const unsigned char "+p+"_is[24]={5,2,7,1,4,6,3,8,2,5,1,7,4,3,6,2,8,1,5,3,7,2,4,6};\n"
     +t+" "+p+"_interpsrch("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[24]; unsigned cur=0u; for(int i=0;i<24;i++){ cur+=((unsigned)"+p+"_is[i]&7u)+1u; arr[i]=cur; }\n"
     "    unsigned target=(s>>3)%(cur+8u); int lo=0, hi=23, found=-1;\n"
     "    while(lo<=hi && target>=arr[lo] && target<=arr[hi]){\n"
     "      int pos; if(arr[hi]==arr[lo]) pos=lo; else pos=lo+(int)(((target-arr[lo])*(unsigned)(hi-lo))/(arr[hi]-arr[lo]));\n"
     "      if(pos<lo) pos=lo; if(pos>hi) pos=hi;\n"
     "      if(arr[pos]==target){ found=pos; break; } else if(arr[pos]<target) lo=pos+1; else hi=pos-1;\n"
     "      acc=acc*131u+(unsigned)pos+(unsigned)lo+(unsigned)hi; }\n"
     "    acc=acc*131u+(unsigned)(found+1)+target; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Du}, "OptStress169", 2},

    // sqrt-decomposition range-sum with point updates (per-block sums).
    {p+"_sqrtdec",
     "static const unsigned char "+p+"_sd[16]={9,3,12,5,7,1,14,6,2,11,8,4,13,0,10,15};\n"
     +t+" "+p+"_sqrtdec("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[16]; for(int i=0;i<16;i++) arr[i]=((unsigned)"+p+"_sd[i]^((s>>(i&7))&3u))&63u;\n"
     "    unsigned blk[4]; for(int b=0;b<4;b++){ unsigned sm=0u; for(int j=0;j<4;j++) sm+=arr[b*4+j]; blk[b]=sm; }\n"
     "    for(int q=0;q<8;q++){ unsigned pos=(s>>(q&7))&15u, val=(s>>q)&31u;\n"
     "      blk[pos>>2]=blk[pos>>2]-arr[pos]+val; arr[pos]=val;\n"
     "      unsigned l=(unsigned)(q&7), r=8u+(unsigned)(q&7), sum=0u, i=l;\n"
     "      while(i<r){ if((i&3u)==0u && i+4u<=r){ sum+=blk[i>>2]; i+=4u; } else { sum+=arr[i]; i++; } }\n"
     "      acc=acc*131u+sum+blk[0]+blk[3]; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Eu}, "OptStress169", 2},

    // pancake sort: drive the running max to the tail via prefix reversals.
    {p+"_pancake",
     "static const unsigned char "+p+"_pk[16]={4,12,1,9,6,15,2,11,7,0,13,5,10,3,14,8};\n"
     +t+" "+p+"_pancake("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[16]; for(int i=0;i<16;i++) arr[i]=((unsigned)"+p+"_pk[i]^((s>>(i&7))&7u))&63u;\n"
     "    unsigned flips=0u;\n"
     "    for(int n=16;n>1;n--){ int mi=0; for(int i=1;i<n;i++) if(arr[i]>arr[mi]) mi=i;\n"
     "      if(mi!=n-1){ if(mi>0){ int l=0,r=mi; while(l<r){ unsigned tt=arr[l];arr[l]=arr[r];arr[r]=tt;l++;r--; } flips++; }\n"
     "        int l=0,r=n-1; while(l<r){ unsigned tt=arr[l];arr[l]=arr[r];arr[r]=tt;l++;r--; } flips++; }\n"
     "      acc=acc*131u+arr[n-1]+flips; }\n"
     "    for(int i=0;i<16;i++) acc=acc*131u+arr[i]; acc=acc*131u+flips; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Fu}, "OptStress169", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress169TC("x64o169", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress169TC("x86o169", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress169TC("a64o169", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress169TC("armo169", "int");

INSTANTIATE_TEST_SUITE_P(OptStress169, X64OptStress169RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress169, X86OptStress169RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress169, A64OptStress169RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress169, ARM32OptStress169RT, ::testing::ValuesIn(kARM), rtTCName);
