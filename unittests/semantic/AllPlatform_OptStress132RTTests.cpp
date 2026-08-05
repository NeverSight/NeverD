//===- AllPlatform_OptStress132RTTests.cpp - BFS / edit-dist / Rabin-Karp =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * bfs    - breadth-first traversal over an 8x8 rodata adjacency matrix using
//              an explicit FIFO ring, distances folded.  Pins a queue-driven
//              level-order walk (distinct from the LIFO union-find forest walk
//              and from any recursive descent).
//   * edit   - Levenshtein edit distance (min-of-three DP) over two rodata
//              strings with a rolling two-row table.  Pins the
//              `min(del,ins,sub)` recurrence (explicitly distinct from the
//              max-of-two LCS DP in #126).
//   * rabin  - Rabin-Karp rolling polynomial hash window over a rodata text,
//              counting hash hits against a rodata pattern hash.  Pins a
//              `h=(h-out*pw)*B+in` sliding hash (distinct from the KMP prefix
//              automaton in #129).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress132RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress132RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress132RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress132RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress132RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress132RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress132RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress132RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress132TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // breadth-first traversal over an 8x8 rodata adjacency matrix (FIFO ring).
    {p+"_bfs",
     "static const unsigned char "+p+"_adj[64]={\n"
     "0,1,0,0,1,0,0,0, 1,0,1,0,0,1,0,0, 0,1,0,1,0,0,1,0, 0,0,1,0,1,0,0,1,\n"
     "1,0,0,1,0,1,0,0, 0,1,0,0,1,0,1,0, 0,0,1,0,0,1,0,1, 0,0,0,1,0,0,1,0};\n"
     +t+" "+p+"_bfs("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned dist[8]; for(int i=0;i<8;i++) dist[i]=99u;\n"
     "    unsigned queue[8], head=0u, tail=0u, start=(s>>3)&7u;\n"
     "    dist[start]=0u; queue[tail++]=start;\n"
     "    while(head<tail){ unsigned u=queue[head++];\n"
     "      for(unsigned v=0;v<8;v++){ if("+p+"_adj[u*8u+v] && dist[v]==99u){\n"
     "        dist[v]=dist[u]+1u; if(tail<8u) queue[tail++]=v; } } }\n"
     "    for(int i=0;i<8;i++) acc=acc*131u+dist[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x24u}, "OptStress132", 2},

    // Levenshtein edit distance (min-of-three rolling DP) over two rodata strings.
    {p+"_edit",
     "static const unsigned char "+p+"_sa[12]={3,9,1,7,12,5,2,14,6,10,4,8};\n"
     "static const unsigned char "+p+"_sb[12]={9,1,7,3,5,12,2,14,10,6,8,4};\n"
     +t+" "+p+"_edit("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned prev[13], cur[13]; for(int j=0;j<=12;j++) prev[j]=(unsigned)j;\n"
     "    for(int i=1;i<=12;i++){ cur[0]=(unsigned)i; unsigned ca=("+p+"_sa[i-1]^(s&3u))&15u;\n"
     "      for(int j=1;j<=12;j++){ unsigned cb=("+p+"_sb[j-1]^((s>>2)&3u))&15u;\n"
     "        unsigned cost=(ca==cb)?0u:1u;\n"
     "        unsigned d1=prev[j]+1u, d2=cur[j-1]+1u, d3=prev[j-1]+cost;\n"
     "        unsigned m=d1<d2?d1:d2; if(d3<m) m=d3; cur[j]=m; }\n"
     "      for(int j=0;j<=12;j++) prev[j]=cur[j]; }\n"
     "    acc=acc*131u+prev[12]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x57u}, "OptStress132", 2},

    // Rabin-Karp rolling polynomial hash window over a rodata text.
    {p+"_rabin",
     "static const unsigned char "+p+"_pat[8]={5,9,2,7,5,9,2,7};\n"
     "static const unsigned char "+p+"_txt[40]={\n"
     "5,9,2,7,5,9,2,7, 3,5,9,2,7,5,9,2, 7,8,5,9,2,7,5,9, 2,7,1,5,9,2,7,5, 9,2,7,4,5,9,2,7};\n"
     +t+" "+p+"_rabin("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned ph=0u, h=0u, pw=1u;\n"
     "    for(int i=0;i<8;i++) ph=ph*257u+("+p+"_pat[i]^((s>>(i&7))&1u));\n"
     "    for(int i=0;i<8;i++) h=h*257u+("+p+"_txt[i]^((s>>(i&7))&1u));\n"
     "    for(int i=0;i<7;i++) pw*=257u;\n"
     "    unsigned matches=(h==ph)?1u:0u;\n"
     "    for(int i=8;i<40;i++){ unsigned oc="+p+"_txt[i-8]^((s>>((i-8)&7))&1u);\n"
     "      unsigned ic="+p+"_txt[i]^((s>>(i&7))&1u);\n"
     "      h=(h-oc*pw)*257u+ic; if(h==ph) matches++; acc=acc*131u+h; }\n"
     "    acc=acc*131u+matches+ph; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Bu}, "OptStress132", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress132TC("x64o132", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress132TC("x86o132", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress132TC("a64o132", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress132TC("armo132", "int");

INSTANTIATE_TEST_SUITE_P(OptStress132, X64OptStress132RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress132, X86OptStress132RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress132, A64OptStress132RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress132, ARM32OptStress132RT, ::testing::ValuesIn(kARM), rtTCName);
