//===- AllPlatform_OptStress122RTTests.cpp - topo / WHT / trellis shapes ---==//
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
//   * toposort- Kahn topological sort of a 6-node rodata DAG adjacency matrix:
//              in-degree counting then repeated ready-node extraction.  Pins an
//              in-degree array driven by a 2D rodata adjacency (distinct from the
//              Dijkstra/Prim greedy frontier selection).
//   * wht    - in-place Walsh-Hadamard transform of a rodata-seeded stack vector:
//              the strided butterfly `x[j]+x[j+len]`/`x[j]-x[j+len]`.  Pins a
//              log-stage add/sub butterfly network (counted, fixed-stride).
//   * viterbi- minimum-cost trellis DP with a 4x4 rodata transition-cost table
//              and a rodata observation stream: per-state min over predecessors.
//              Pins a stage-by-stage min-of-predecessors DP over a rodata table.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress122RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress122RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress122RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress122RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress122RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress122RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress122RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress122RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress122TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Kahn topological sort over a 6-node rodata DAG adjacency matrix.
    {p+"_toposort",
     "static const unsigned char "+p+"_adj[36]={\n"
     "0,1,1,0,1,0, 0,0,1,1,0,1, 0,0,0,1,0,1, 0,0,0,0,1,1, 0,0,0,0,0,1, 0,0,0,0,0,0};\n"
     +t+" "+p+"_toposort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned indeg[6]; for(int i=0;i<6;i++) indeg[i]=0u;\n"
     "    for(int i=0;i<6;i++) for(int j=0;j<6;j++) if("+p+"_adj[i*6+j]) indeg[j]++;\n"
     "    unsigned done[6]; for(int i=0;i<6;i++) done[i]=0u; unsigned ord=0u;\n"
     "    for(int step=0;step<6;step++){ int u=-1;\n"
     "      for(int i=0;i<6;i++) if(!done[i] && indeg[i]==0u){ u=i; break; }\n"
     "      if(u<0) break; done[u]=1u; ord++;\n"
     "      acc=acc*131u+(unsigned)u*7u+ord+((s>>(step&7))&1u);\n"
     "      for(int j=0;j<6;j++) if("+p+"_adj[u*6+j] && indeg[j]>0u) indeg[j]--; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x70u}, "OptStress122", 2},

    // in-place Walsh-Hadamard transform of a rodata-seeded stack vector.
    {p+"_wht",
     "static const unsigned char "+p+"_v[16]={5,12,30,18,7,44,21,9,33,16,52,3,27,14,40,8};\n"
     +t+" "+p+"_wht("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int x[16]; for(int i=0;i<16;i++) x[i]=(int)("+p+"_v[i]^((s>>(i&7))&7u));\n"
     "    for(int len=1;len<16;len<<=1){\n"
     "      for(int i=0;i<16;i+=len<<1){\n"
     "        for(int j=i;j<i+len;j++){ int u=x[j], w=x[j+len]; x[j]=u+w; x[j+len]=u-w; } } }\n"
     "    for(int i=0;i<16;i++) acc=acc*131u+(unsigned)(x[i]&0xFFFF);\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x57u}, "OptStress122", 2},

    // minimum-cost trellis DP with a 4x4 rodata transition table + rodata obs.
    {p+"_viterbi",
     "static const unsigned char "+p+"_tc[16]={2,5,1,7, 4,0,6,3, 1,8,2,5, 6,3,7,1};\n"
     "static const unsigned char "+p+"_obs[24]={\n"
     "1,3,0,2,1,2,3,1, 0,3,2,1,3,0,1,2, 1,2,0,3,2,1,3,0};\n"
     +t+" "+p+"_viterbi("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned dp[4]; for(int i=0;i<4;i++) dp[i]=(s>>(i*3))&7u;\n"
     "    for(int t2=0;t2<24;t2++){ unsigned ndp[4];\n"
     "      for(int cur=0;cur<4;cur++){ unsigned best=0xFFFFu;\n"
     "        for(int prev=0;prev<4;prev++){ unsigned cand=dp[prev]+"+p+"_tc[prev*4+cur];\n"
     "          if(cand<best) best=cand; }\n"
     "        unsigned em=("+p+"_obs[t2]^(unsigned)cur)&3u; ndp[cur]=best+em; }\n"
     "      for(int i=0;i<4;i++) dp[i]=ndp[i]; acc=acc*131u+dp[0]+dp[1]+dp[2]+dp[3]; }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x71u}, "OptStress122", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress122TC("x64o122", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress122TC("x86o122", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress122TC("a64o122", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress122TC("armo122", "int");

INSTANTIATE_TEST_SUITE_P(OptStress122, X64OptStress122RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress122, X86OptStress122RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress122, A64OptStress122RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress122, ARM32OptStress122RT, ::testing::ValuesIn(kARM), rtTCName);
