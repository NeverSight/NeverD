//===- AllPlatform_OptStress177RTTests.cpp - Floyd / union-find / count-sort =//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain base+index copies and folds a result that depends only on the
// bytes + control flow (never an absolute VA), so nothing touches the deferred
// i386/ARM32 PIC rodata *interior*-pointer model (#477/#487); every probe runs
// on all four targets.
//
//   * floyd     - Floyd tortoise/hare cycle detection over a rodata-seeded
//                 functional graph: meet point, then cycle entry mu and length
//                 lambda.  Pins the two-speed pointer chase (distinct from every
//                 array-index scan).
//   * unionfind - connected-component count via union by linking roots, with
//                 find done by parent walking.  Pins a disjoint-set forest
//                 (distinct from the bitmask membership scans in #173).
//   * countsort - counting sort: histogram the values then materialise the
//                 stable sorted order from the bucket counts.  Pins a
//                 distribution (non-comparison) sort (distinct from the
//                 partition #175 and the order-predicate DP #176).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress177RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress177RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress177RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress177RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress177RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress177RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress177RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress177RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress177TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Floyd tortoise/hare on a functional graph: meet, then mu and lambda.
    {p+"_floyd",
     "static const unsigned char "+p+"_fc[16]={3,9,1,12,6,2,14,7,0,11,5,15,8,4,13,10};\n"
     +t+" "+p+"_floyd("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned nxt[16]; for(int i=0;i<16;i++) nxt[i]=((unsigned)"+p+"_fc[i]^((s>>(i&7))&15u))&15u;\n"
     "    unsigned start=s&15u; unsigned tort=nxt[start], hare=nxt[nxt[start]];\n"
     "    while(tort!=hare){ tort=nxt[tort]; hare=nxt[nxt[hare]]; }\n"
     "    unsigned mu=0u; tort=start; while(tort!=hare){ tort=nxt[tort]; hare=nxt[hare]; mu++; }\n"
     "    unsigned lam=1u; hare=nxt[tort]; while(tort!=hare){ hare=nxt[hare]; lam++; }\n"
     "    acc=acc*131u+mu*100u+lam*7u+tort; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x37u}, "OptStress177", 2},

    // disjoint-set forest: union by root linking, find by parent walking.
    {p+"_unionfind",
     "static const unsigned char "+p+"_uf[24]={0,1,2,3,1,4,5,6,3,7,8,9,4,10,6,11,7,0,9,2,10,5,11,8};\n"
     +t+" "+p+"_unionfind("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int par[12]; for(int i=0;i<12;i++) par[i]=i;\n"
     "    for(int e=0;e<12;e++){ int x=(int)(((unsigned)"+p+"_uf[e*2]^((s>>(e&7))&3u))%12u); int y=(int)(((unsigned)"+p+"_uf[e*2+1]^((s>>(e&3))&3u))%12u);\n"
     "      int rx=x; while(par[rx]!=rx) rx=par[rx]; int ry=y; while(par[ry]!=ry) ry=par[ry];\n"
     "      if(rx!=ry) par[rx]=ry; }\n"
     "    unsigned comp=0u; for(int i=0;i<12;i++){ int r=i; while(par[r]!=r) r=par[r]; if(r==i) comp++; }\n"
     "    acc=acc*131u+comp; for(int i=0;i<12;i++) acc=acc*131u+(unsigned)par[i]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x48u}, "OptStress177", 2},

    // counting sort: histogram then materialise the stable sorted order.
    {p+"_countsort",
     "static const unsigned char "+p+"_cs[20]={12,3,15,7,1,9,12,4,8,3,15,0,6,11,2,9,14,5,10,7};\n"
     +t+" "+p+"_countsort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned cnt[16]; for(int i=0;i<16;i++) cnt[i]=0u;\n"
     "    for(int i=0;i<20;i++){ unsigned v=((unsigned)"+p+"_cs[i]^((s>>(i&7))&3u))&15u; cnt[v]++; }\n"
     "    unsigned sorted[20]; int idx=0; for(unsigned v=0;v<16u;v++){ for(unsigned c=0;c<cnt[v];c++) sorted[idx++]=v; }\n"
     "    unsigned hh=0u; for(int i=0;i<idx;i++) hh=hh*131u+sorted[i]; acc=acc*131u+hh+(unsigned)idx; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x79u}, "OptStress177", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress177TC("x64o177", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress177TC("x86o177", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress177TC("a64o177", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress177TC("armo177", "int");

INSTANTIATE_TEST_SUITE_P(OptStress177, X64OptStress177RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress177, X86OptStress177RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress177, A64OptStress177RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress177, ARM32OptStress177RT, ::testing::ValuesIn(kARM), rtTCName);
