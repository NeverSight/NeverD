//===- AllPlatform_OptStress58RTTests.cpp - data-structure kernels -*-C++*-=//
//
// Data-structure-heavy whole-program kernels (distinct from the VM / FSM / sort
// / CRC / matrix shapes of #464-#466): binary search, max-heap sift, open-
// addressing hash probing, Dijkstra over an adjacency matrix, a rodata trie
// walk, and a sorted-array merge.  These drive recursion-free pointer/index
// arithmetic over rodata tables and computed stack arrays through the optimizer
// to flush remaining addressing / CFG / width miscompiles.
//
//   * bsearch  - binary search of computed keys over a sorted rodata array.
//   * heapify  - build a max-heap in a stack array, sift-down, fold.
//   * hashtbl  - open-addressing (linear-probe) insert + lookup in a stack array.
//   * dijkstra - shortest paths over a rodata adjacency matrix (stack dist/seen).
//   * trie     - prefix-tree walk over a rodata node array (index-linked).
//   * merge    - merge two sorted rodata arrays into a stack array, fold.
//
// All integer, arrays filled with computed values (never memset/memcpy), indices
// bounded, fold to one return, no float / 64-bit divide helper.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress58RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress58RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress58RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress58RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress58RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress58RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress58RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress58RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress58TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Binary search of computed keys over a sorted rodata array.
    {p+"_bsearch",
     "static const unsigned S[32]={3,9,14,21,27,33,40,48,55,61,68,74,80,"
     "89,95,102,110,118,125,131,140,148,155,163,170,178,185,193,200,210,"
     "221,233};\n"
     +t+" "+p+"_bsearch("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<240;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned key=(s>>4)%240u; int lo=0,hi=31,found=-1;\n"
     "    while(lo<=hi){ int mid=(lo+hi)>>1;\n"
     "      if(S[mid]==key){ found=mid; break; }\n"
     "      else if(S[mid]<key) lo=mid+1; else hi=mid-1; }\n"
     "    h=h*131u+(unsigned)(found+1)+(found>=0?S[found]:lo); }\n"
     "  return ("+t+")h; }\n",
     {0x61u}, "OptStress58", 2},

    // Build a max-heap in a stack array, sift-down, fold the heap.
    {p+"_heapify",
     t+" "+p+"_heapify("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<60;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned A[31];\n"
     "    for(int i=0;i<31;i++){ s=s*1103515245u+12345u; A[i]=(s>>12)&0xffff; }\n"
     "    for(int i=31/2-1;i>=0;i--){ int r=i;\n"
     "      for(;;){ int l=2*r+1,rr=2*r+2,b=r;\n"
     "        if(l<31&&A[l]>A[b])b=l; if(rr<31&&A[rr]>A[b])b=rr;\n"
     "        if(b==r)break; unsigned tmp=A[r];A[r]=A[b];A[b]=tmp; r=b; } }\n"
     "    for(int i=0;i<31;i++) h=h*131u+A[i]; h^=h>>13; }\n"
     "  return ("+t+")h; }\n",
     {0x62u}, "OptStress58", 2},

    // Open-addressing (linear-probe) insert + lookup in a stack array.  The
    // empty sentinel is `0xffff0000|i` (a computed per-slot value) so the init
    // is a store loop, not a memset(0xFF) idiom call the harness cannot run.
    {p+"_hashtbl",
     t+" "+p+"_hashtbl("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<50;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned slot[37]; for(int i=0;i<37;i++) slot[i]=0xffff0000u|(unsigned)i;\n"
     "    for(int n=0;n<24;n++){ s=s*1103515245u+12345u; unsigned k=((s>>7)&0x3ff)+1u;\n"
     "      unsigned idx=k%37u;\n"
     "      for(int probe=0;probe<37;probe++){ unsigned j=(idx+probe)%37u;\n"
     "        if(slot[j]>=0xffff0000u||slot[j]==k){ slot[j]=k; break; } } }\n"
     "    unsigned cnt=0; for(int i=0;i<37;i++) if(slot[i]<0xffff0000u){cnt++;h=h*131u+slot[i];}\n"
     "    h=h*7u+cnt; }\n"
     "  return ("+t+")h; }\n",
     {0x63u}, "OptStress58", 2},

    // Dijkstra shortest paths over a rodata adjacency matrix.
    {p+"_dijkstra",
     "static const unsigned char G[8][8]={"
     "{0,4,0,0,0,0,8,0},{4,0,8,0,0,0,11,0},{0,8,0,7,0,4,0,2},"
     "{0,0,7,0,9,14,0,0},{0,0,0,9,0,10,0,0},{0,0,4,14,10,0,2,0},"
     "{8,11,0,0,0,2,0,1},{0,0,2,0,0,0,1,0}};\n"
     +t+" "+p+"_dijkstra("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<60;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned dist[8]; unsigned char seen[8];\n"
     "    for(int i=0;i<8;i++){ dist[i]=9999u; seen[i]=0; }\n"
     "    unsigned src=(s>>5)&7u; dist[src]=0;\n"
     "    for(int c=0;c<8;c++){ int u=-1; unsigned best=99999u;\n"
     "      for(int i=0;i<8;i++) if(!seen[i]&&dist[i]<best){best=dist[i];u=i;}\n"
     "      if(u<0)break; seen[u]=1;\n"
     "      for(int v=0;v<8;v++){ unsigned w=G[u][v];\n"
     "        if(w&&!seen[v]&&dist[u]+w<dist[v]) dist[v]=dist[u]+w; } }\n"
     "    for(int i=0;i<8;i++) h=h*131u+dist[i]; h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0x64u}, "OptStress58", 2},

    // Prefix-tree walk over a rodata node array (index-linked children).
    {p+"_trie",
     "static const unsigned char TC[20]={'a','b','c','d','e','t','o','n',"
     "'r','s','i','x','y','z','m','p','q','u','v','w'};\n"
     "static const unsigned char TN[20]={1,2,3,0,0,7,8,0,10,0,0,13,0,0,16,0,"
     "0,19,0,0};\n"
     "static const unsigned char TV[20]={11,22,33,44,55,66,77,88,99,12,23,34,"
     "45,56,67,78,89,91,13,24};\n"
     +t+" "+p+"_trie("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<200;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned node=(s>>5)%20u; unsigned acc=0;\n"
     "    for(int step=0;step<6;step++){\n"
     "      acc=acc*31u+TC[node]+TV[node];\n"
     "      unsigned nx=TN[node]; if(nx==0||nx>=20u){ node=(node*7u+1u)%20u; }\n"
     "      else node=nx; }\n"
     "    h=h*131u+acc; }\n"
     "  return ("+t+")h; }\n",
     {0x65u}, "OptStress58", 2},

    // Merge two sorted rodata arrays, folding the merged sequence directly (no
    // bulk array copy, which clang would lower to a memcpy the harness cannot
    // run) so the merge ordering across two rodata tables is exercised.
    {p+"_merge",
     "static const unsigned P[12]={2,5,8,11,15,19,24,30,37,45,54,64};\n"
     "static const unsigned Q[12]={1,3,9,12,16,20,25,31,38,46,55,65};\n"
     +t+" "+p+"_merge("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int it=0;it<150;it++){ s=s*1103515245u+12345u;\n"
     "    unsigned off=(s>>6)&7u, acc=0; int i=0,j=0;\n"
     "    while(i<12&&j<12){ if(P[i]+off<=Q[j]) acc=acc*31u+P[i++];\n"
     "      else acc=acc*31u+Q[j++]; }\n"
     "    while(i<12) acc=acc*31u+P[i++];\n"
     "    while(j<12) acc=acc*31u+Q[j++];\n"
     "    h=h*131u+acc+off; }\n"
     "  return ("+t+")h; }\n",
     {0x66u}, "OptStress58", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress58TC("x64o58", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress58TC("x86o58", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress58TC("a64o58", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress58TC("armo58", "int");

INSTANTIATE_TEST_SUITE_P(OptStress58, X64OptStress58RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress58, X86OptStress58RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress58, A64OptStress58RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress58, ARM32OptStress58RT, ::testing::ValuesIn(kARM), rtTCName);
