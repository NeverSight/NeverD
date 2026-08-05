//===- AllPlatform_OptStress190RTTests.cpp - Kruskal / Prim / N-Queens =====//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * kruskal  - Kruskal minimum spanning tree: selection-sort a rodata edge
//                list by weight then union endpoints, summing accepted weights.
//                Pins an edge-sorted greedy forest merge (distinct from the bare
//                union-find #148/#177 and the frontier MSTs below / Dijkstra).
//   * prim     - Prim minimum spanning tree over a rodata weight matrix via an
//                O(V^2) nearest-frontier scan.  Pins a key-relax frontier select
//                (distinct from Kruskal's edge sort and from the shortest-path
//                relax of Dijkstra #58/#114 — here the key is the edge weight,
//                not an accumulated distance).
//   * nqueens  - N-Queens solution census on a 6x6 board with one rodata/seed
//                forbidden cell, by explicit-stack backtracking (no recursion).
//                Pins a constraint backtracking search with column/diagonal
//                checks (a placement-tree walk unlike any sort or DP).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress190RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress190RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress190RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress190RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress190RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress190RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress190RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress190RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress190TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Kruskal MST: selection-sort a rodata edge list by weight, union endpoints.
    {p+"_kruskal",
     "static const unsigned char "+p+"_ke[24]={0,1,7, 1,2,9, 2,3,5, 3,4,8, 4,5,6, 5,0,4, 0,2,3, 1,4,11};\n"
     +t+" "+p+"_kruskal("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned eu[8],ev[8],ew[8]; int E=8;\n"
     "    for(int i=0;i<8;i++){ eu[i]=(unsigned)"+p+"_ke[i*3+0]; ev[i]=(unsigned)"+p+"_ke[i*3+1];\n"
     "      ew[i]=((unsigned)"+p+"_ke[i*3+2]^((s>>(i&7))&3u))&63u; }\n"
     "    for(int i=0;i<E;i++){ int m=i; for(int j=i+1;j<E;j++) if(ew[j]<ew[m]) m=j;\n"
     "      if(m!=i){ unsigned t1=eu[i];eu[i]=eu[m];eu[m]=t1; unsigned t2=ev[i];ev[i]=ev[m];ev[m]=t2;\n"
     "        unsigned t3=ew[i];ew[i]=ew[m];ew[m]=t3; } }\n"
     "    int par[6]; for(int i=0;i<6;i++) par[i]=i;\n"
     "    unsigned total=0u, used=0u;\n"
     "    for(int i=0;i<E;i++){ int ra=(int)eu[i]; while(par[ra]!=ra) ra=par[ra];\n"
     "      int rb=(int)ev[i]; while(par[rb]!=rb) rb=par[rb];\n"
     "      if(ra!=rb){ par[ra]=rb; total+=ew[i]; used++; } acc=acc*131u+total*7u+used; }\n"
     "    acc=acc*131u+total+used; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xA1u}, "OptStress190", 2},

    // Prim MST over a rodata weight matrix via an O(V^2) nearest-frontier scan.
    {p+"_prim",
     "static const unsigned char "+p+"_pw[36]={\n"
     "0,7,9,14,8,11, 7,0,10,15,5,12, 9,10,0,11,13,6, 14,15,11,0,9,7, 8,5,13,9,0,10, 11,12,6,7,10,0};\n"
     +t+" "+p+"_prim("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned w[36]; for(int i=0;i<36;i++) w[i]=1u+(((unsigned)"+p+"_pw[i]^((s>>(i&7))&3u))&31u);\n"
     "    unsigned dist[6]; unsigned done[6]; for(int i=0;i<6;i++){ dist[i]=9999u; done[i]=0u; }\n"
     "    dist[0]=0u; unsigned total=0u;\n"
     "    for(int step=0;step<6;step++){ int u=-1; unsigned best=99999u;\n"
     "      for(int i=0;i<6;i++) if(!done[i] && dist[i]<best){ best=dist[i]; u=i; }\n"
     "      if(u<0) break; done[u]=1u; total+=dist[u];\n"
     "      for(int v=0;v<6;v++){ if(v==u) continue; unsigned wt=w[u*6+v];\n"
     "        if(!done[v] && wt<dist[v]) dist[v]=wt; }\n"
     "      acc=acc*131u+(unsigned)u*7u+total; }\n"
     "    acc=acc*131u+total; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xA2u}, "OptStress190", 2},

    // N-Queens census on a 6x6 board with one forbidden cell (stack backtrack).
    {p+"_nqueens",
     "static const unsigned char "+p+"_nq[4]={2,5,1,4};\n"
     +t+" "+p+"_nqueens("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int N=6; int br=(int)(((unsigned)"+p+"_nq[s&3u]+ (s>>3))%6u); int bc=(int)((s>>5)%6u);\n"
     "    int pos[6]; for(int i=0;i<6;i++) pos[i]=-1;\n"
     "    unsigned solutions=0u; int r=0;\n"
     "    while(r>=0){ int c=pos[r]+1; int placed=0;\n"
     "      while(c<N){ int ok=1; if(r==br && c==bc) ok=0;\n"
     "        for(int k=0; ok && k<r; k++){ int ck=pos[k];\n"
     "          if(ck==c || (r-k)==(c-ck) || (r-k)==(ck-c)){ ok=0; } }\n"
     "        if(ok){ pos[r]=c; placed=1; break; } c++; }\n"
     "      if(placed){ if(r==N-1){ solutions++; } else { r++; pos[r]=-1; } }\n"
     "      else { pos[r]=-1; r--; } }\n"
     "    acc=acc*131u+solutions*131u+(unsigned)(br*6+bc); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xA3u}, "OptStress190", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress190TC("x64o190", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress190TC("x86o190", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress190TC("a64o190", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress190TC("armo190", "int");

INSTANTIATE_TEST_SUITE_P(OptStress190, X64OptStress190RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress190, X86OptStress190RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress190, A64OptStress190RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress190, ARM32OptStress190RT, ::testing::ValuesIn(kARM), rtTCName);
