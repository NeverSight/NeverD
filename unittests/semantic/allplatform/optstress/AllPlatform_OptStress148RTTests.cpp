//===- AllPlatform_OptStress148RTTests.cpp - union-find / topo / Bellman =//
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
//   * unionf  - disjoint-set union over a rodata edge list with path compression
//               and union by rank: each edge folds two trees, find walks to the
//               root and re-points the path.  Pins a forest with amortized path
//               flattening (distinct from any linear scan or the BFS in #132).
//   * topo    - Kahn topological order over a rodata DAG edge list: in-degrees
//               are counted, zero-degree nodes drained through an array queue,
//               relaxing successors.  Pins an in-degree drain (distinct from the
//               BFS level order in #132 and the Floyd chase in #142).
//   * bellman - Bellman-Ford shortest paths over a rodata weighted edge list:
//               |V| relaxation sweeps fold `dist[u]+w` into `dist[v]`.  Pins an
//               edge-relaxation fixed-point (distinct from any single-source
//               frontier walk or greedy selection).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress148RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress148RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress148RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress148RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress148RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress148RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress148RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress148RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress148TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // disjoint-set union over a rodata edge list (path compression + rank).
    {p+"_unionf",
     "static const unsigned char "+p+"_edges[32]={\n"
     "0,3,1,3,2,4,3,5,4,6,5,7,6,8,9,11, 10,12,11,13,12,14,13,15,0,7,1,8,2,9,3,10};\n"
     +t+" "+p+"_unionf("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned par[16],rnk[16];\n"
     "    for(int i=0;i<16;i++){ par[i]=(unsigned)i; rnk[i]=0u; }\n"
     "    for(int ed=0;ed<16;ed++){\n"
     "      unsigned x=((unsigned)"+p+"_edges[2*ed]+((s>>(ed&7))&1u))&15u;\n"
     "      unsigned y=((unsigned)"+p+"_edges[2*ed+1]+((s>>((ed+3)&7))&1u))&15u;\n"
     "      unsigned rx=x; while(par[rx]!=rx) rx=par[rx];\n"
     "      while(par[x]!=rx){ unsigned nx=par[x]; par[x]=rx; x=nx; }\n"
     "      unsigned ry=y; while(par[ry]!=ry) ry=par[ry];\n"
     "      while(par[y]!=ry){ unsigned ny=par[y]; par[y]=ry; y=ny; }\n"
     "      if(rx!=ry){ if(rnk[rx]<rnk[ry]){ unsigned tt=rx; rx=ry; ry=tt; } par[ry]=rx; if(rnk[rx]==rnk[ry]) rnk[rx]++; }\n"
     "      acc=acc*131u+rx+ry*7u; }\n"
     "    unsigned comps=0u;\n"
     "    for(int i=0;i<16;i++){ unsigned r=(unsigned)i; while(par[r]!=r) r=par[r]; if(r==(unsigned)i) comps++; acc=acc*131u+r; }\n"
     "    acc=acc*131u+comps; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x18u}, "OptStress148", 2},

    // Kahn topological order over a rodata DAG edge list (in-degree drain).
    {p+"_topo",
     "static const unsigned char "+p+"_te[24]={\n"
     "0,3,1,3,2,4,3,5,4,6,5,7, 6,8,0,2,1,4,2,5,7,9,8,10};\n"
     +t+" "+p+"_topo("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned indeg[12]; for(int i=0;i<12;i++) indeg[i]=0u;\n"
     "    for(int ed=0;ed<12;ed++){ if(((s>>(ed&7))&1u)==0u){ unsigned v=(unsigned)"+p+"_te[2*ed+1]; if(v<12u) indeg[v]++; } }\n"
     "    unsigned q[12]; int head=0,tail=0;\n"
     "    for(int i=0;i<12;i++) if(indeg[i]==0u) q[tail++]=(unsigned)i;\n"
     "    unsigned processed=0u,ordsum=0u;\n"
     "    while(head<tail){ unsigned u=q[head++]; processed++; ordsum=ordsum*7u+u;\n"
     "      for(int ed=0;ed<12;ed++){ if(((s>>(ed&7))&1u)==0u && (unsigned)"+p+"_te[2*ed]==u){ unsigned v=(unsigned)"+p+"_te[2*ed+1];\n"
     "        if(v<12u && indeg[v]>0u){ indeg[v]--; if(indeg[v]==0u) q[tail++]=v; } } }\n"
     "      acc=acc*131u+u; }\n"
     "    acc=acc*131u+processed+ordsum; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x2Eu}, "OptStress148", 2},

    // Bellman-Ford shortest paths over a rodata weighted edge list.
    {p+"_bellman",
     "static const unsigned char "+p+"_be[36]={\n"
     "0,1,4,0,2,7,1,2,2,1,3,5, 2,3,1,3,4,3,4,5,6,0,5,9, 5,6,2,3,6,8,6,7,4,0,7,11};\n"
     +t+" "+p+"_bellman("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned dist[8]; for(int i=0;i<8;i++) dist[i]=9999u; dist[0]=0u;\n"
     "    for(int pass=0;pass<8;pass++){\n"
     "      for(int ed=0;ed<12;ed++){ unsigned u=(unsigned)"+p+"_be[3*ed]&7u, v=(unsigned)"+p+"_be[3*ed+1]&7u,\n"
     "        w=((unsigned)"+p+"_be[3*ed+2]+((s>>(ed&7))&1u))&15u;\n"
     "        if(dist[u]+w<dist[v]) dist[v]=dist[u]+w; acc=acc*131u+dist[v]; } }\n"
     "    for(int i=0;i<8;i++) acc=acc*131u+dist[i]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Bu}, "OptStress148", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress148TC("x64o148", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress148TC("x86o148", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress148TC("a64o148", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress148TC("armo148", "int");

INSTANTIATE_TEST_SUITE_P(OptStress148, X64OptStress148RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress148, X86OptStress148RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress148, A64OptStress148RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress148, ARM32OptStress148RT, ::testing::ValuesIn(kARM), rtTCName);
