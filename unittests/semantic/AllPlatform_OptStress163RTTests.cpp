//===- AllPlatform_OptStress163RTTests.cpp - mono runs / heap PQ / Warshall =//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through a plain base+index copy and folds a result that depends only on the
// bytes + control flow (never an absolute VA), so nothing touches the deferred
// i386/ARM32 PIC rodata *interior*-pointer model (#477/#487); every probe runs
// on all four targets.
//
//   * monoruns - ascending/descending run census of a rodata array: track the
//                current and longest monotone run in each direction.  Pins a
//                run-direction state scan (distinct from the equal-value
//                run-length encoder in #147).
//   * heapq    - binary min-heap built by per-element sift-up insertion, then
//                drained by sift-down extract-min (a heapsort).  Pins a sift-up
//                insert + sift-down extract pair (distinct from the build-only
//                sift-down heapify in #144).
//   * warshall - transitive-closure of a rodata adjacency given as per-row bit
//                masks: `if (i->k) row[i] |= row[k]`.  Pins a bitwise reachability
//                fixpoint (distinct from the Bellman-Ford relaxation in #148).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress163RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress163RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress163RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress163RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress163RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress163RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress163RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress163RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress163TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // ascending/descending run census of a rodata array (run-direction scan).
    {p+"_monoruns",
     "static const unsigned char "+p+"_mr[32]={3,5,7,2,4,6,8,1,9,5,3,7,7,2,2,6,4,8,1,9,9,3,5,5,0,4,8,2,6,1,7,3};\n"
     +t+" "+p+"_monoruns("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned ar[32]; for(int i=0;i<32;i++) ar[i]=(unsigned)"+p+"_mr[i]^((s>>(i&7))&3u);\n"
     "    unsigned asc=1u,desc=1u,maxasc=1u,maxdesc=1u,ascruns=1u;\n"
     "    for(int i=1;i<32;i++){ if(ar[i]>ar[i-1]){ asc++; if(asc>maxasc) maxasc=asc; desc=1u; }\n"
     "      else if(ar[i]<ar[i-1]){ desc++; if(desc>maxdesc) maxdesc=desc; asc=1u; ascruns++; }\n"
     "      else { asc=1u; desc=1u; } acc=acc*131u+asc+desc; }\n"
     "    acc=acc*131u+maxasc+maxdesc+ascruns; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x27u}, "OptStress163", 2},

    // binary min-heap: per-element sift-up insert then sift-down extract-min.
    {p+"_heapq",
     "static const unsigned char "+p+"_hq[24]={37,12,180,5,99,46,213,8,71,150,23,64,131,2,97,55,188,30,118,77,14,200,41,160};\n"
     +t+" "+p+"_heapq("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned h[24]; int n=0;\n"
     "    for(int i=0;i<24;i++){ unsigned v=(unsigned)"+p+"_hq[i]^((s>>(i&7))&7u); int ci=n; h[n++]=v;\n"
     "      while(ci>0){ int par=(ci-1)/2; if(h[par]>h[ci]){ unsigned tt=h[par];h[par]=h[ci];h[ci]=tt; ci=par; } else break; } acc=acc*131u+h[0]; }\n"
     "    unsigned prev=0u,ordered=1u;\n"
     "    while(n>0){ unsigned mn=h[0]; h[0]=h[--n]; int ci=0;\n"
     "      while(1){ int l=2*ci+1,r=2*ci+2,sm=ci; if(l<n && h[l]<h[sm]) sm=l; if(r<n && h[r]<h[sm]) sm=r;\n"
     "        if(sm!=ci){ unsigned tt=h[ci];h[ci]=h[sm];h[sm]=tt; ci=sm; } else break; }\n"
     "      if(mn<prev) ordered=0u; prev=mn; acc=acc*131u+mn; }\n"
     "    acc=acc*131u+ordered; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x38u}, "OptStress163", 2},

    // transitive-closure of a rodata bit-adjacency (Warshall row OR).
    {p+"_warshall",
     "static const unsigned char "+p+"_wr[8]={0x02,0x0C,0x10,0x40,0x80,0x01,0x24,0x08};\n"
     +t+" "+p+"_warshall("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned adj[8]; for(int i=0;i<8;i++) adj[i]=((unsigned)"+p+"_wr[i]^((s>>(i&7))&1u))&0xFFu;\n"
     "    for(int k=0;k<8;k++) for(int i=0;i<8;i++){ if((adj[i]>>k)&1u) adj[i]|=adj[k]; }\n"
     "    unsigned total=0u; for(int i=0;i<8;i++){ unsigned x=adj[i]; while(x){ x&=x-1u; total++; } acc=acc*131u+adj[i]; }\n"
     "    acc=acc*131u+total; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x69u}, "OptStress163", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress163TC("x64o163", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress163TC("x86o163", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress163TC("a64o163", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress163TC("armo163", "int");

INSTANTIATE_TEST_SUITE_P(OptStress163, X64OptStress163RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress163, X86OptStress163RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress163, A64OptStress163RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress163, ARM32OptStress163RT, ::testing::ValuesIn(kARM), rtTCName);
