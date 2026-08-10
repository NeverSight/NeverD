//===- AllPlatform_OptStress114RTTests.cpp - greedy / sort / codec shapes --==//
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
//   * dijkstra- single-source shortest paths on a 6x6 rodata weight matrix via
//              greedy min-unvisited selection plus edge relaxation.  Pins a
//              greedy pick + `dist[u]+w<dist[v]` relax over a rodata table
//              (distinct from the Floyd triple-loop DP).
//   * heapsort- binary max-heap sort of a rodata-seeded stack array: sift-down
//              over child indices `2*r+1`/`2*r+2`.  Pins parent/child heap index
//              arithmetic on a runtime array.
//   * base64 - base-64 encode of a rodata message: 24-bit regroups split into
//              four 6-bit indices each gathering a rodata alphabet.  Pins a
//              bit-regroup feeding four alphabet gathers (distinct from the
//              base-36 divmod chain).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress114RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress114RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress114RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress114RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress114RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress114RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress114RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress114RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress114TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Dijkstra single-source shortest paths over a 6x6 rodata weight matrix.
    {p+"_dijkstra",
     "static const unsigned char "+p+"_w[36]={\n"
     "0,7,99,3,99,12, 7,0,4,99,9,99, 99,4,0,6,99,2, 3,99,6,0,8,99, 99,9,99,8,0,5, 12,99,2,99,5,0};\n"
     +t+" "+p+"_dijkstra("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<64;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned src=(s>>3)%6u; unsigned dist[6], vis[6];\n"
     "    for(int i=0;i<6;i++){ dist[i]=200u; vis[i]=0u; }\n"
     "    dist[src]=0u;\n"
     "    for(int n=0;n<6;n++){ unsigned u=6u, best=201u;\n"
     "      for(int i=0;i<6;i++) if(!vis[i] && dist[i]<best){ best=dist[i]; u=(unsigned)i; }\n"
     "      if(u==6u) break; vis[u]=1u;\n"
     "      for(int v=0;v<6;v++){ unsigned w="+p+"_w[u*6u+(unsigned)v];\n"
     "        if(w<99u && dist[u]+w<dist[v]) dist[v]=dist[u]+w; } }\n"
     "    for(int i=0;i<6;i++) acc=acc*131u+dist[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xD1u}, "OptStress114", 2},

    // binary max-heap sort of a rodata-seeded stack array (sift-down).
    {p+"_heapsort",
     "static const unsigned char "+p+"_data[24]={\n"
     "0x3a,0x91,0x47,0xee,0x12,0x8d,0x5b,0xc6, 0x29,0xf0,0x74,0xa3,0x1e,0x6c,0xd8,0x05,\n"
     "0x9f,0x33,0xb7,0x4a,0xe1,0x58,0x82,0x2d};\n"
     +t+" "+p+"_heapsort("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned h[24];\n"
     "    for(int i=0;i<24;i++) h[i]="+p+"_data[i]^((s>>(i&7))&0xFu);\n"
     "    for(int i=24/2-1;i>=0;i--){ int r=i;\n"
     "      for(;;){ int l=2*r+1, rr=2*r+2, big=r;\n"
     "        if(l<24 && h[l]>h[big]) big=l;\n"
     "        if(rr<24 && h[rr]>h[big]) big=rr;\n"
     "        if(big==r) break; unsigned tp=h[r]; h[r]=h[big]; h[big]=tp; r=big; } }\n"
     "    for(int end=24-1;end>0;end--){ unsigned tp=h[0]; h[0]=h[end]; h[end]=tp;\n"
     "      int r=0; for(;;){ int l=2*r+1, rr=2*r+2, big=r;\n"
     "        if(l<end && h[l]>h[big]) big=l;\n"
     "        if(rr<end && h[rr]>h[big]) big=rr;\n"
     "        if(big==r) break; unsigned tt=h[r]; h[r]=h[big]; h[big]=tt; r=big; } }\n"
     "    for(int i=0;i<24;i++) acc=acc*131u+h[i];\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Eu}, "OptStress114", 2},

    // base-64 encode of a rodata message: 24-bit regroup + four alphabet gathers.
    {p+"_base64",
     "static const unsigned char "+p+"_b64[64]={\n"
     "65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80, 81,82,83,84,85,86,87,88,89,90,97,98,99,100,101,102,\n"
     "103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118, 119,120,121,122,48,49,50,51,52,53,54,55,56,57,43,47};\n"
     "static const unsigned char "+p+"_msg[24]={\n"
     "0x4d,0x61,0x6e,0x79,0x20,0x68,0x61,0x6e, 0x64,0x73,0x20,0x6d,0x61,0x6b,0x65,0x20, 0x6c,0x69,0x67,0x68,0x74,0x20,0x77,0x6b};\n"
     +t+" "+p+"_base64("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int i=0;i+3<=24;i+=3){\n"
     "      unsigned b0="+p+"_msg[i]^(s&0xFFu), b1="+p+"_msg[i+1]^((s>>8)&0xFFu), b2="+p+"_msg[i+2]^((s>>16)&0xFFu);\n"
     "      unsigned n=((b0&0xFFu)<<16)|((b1&0xFFu)<<8)|(b2&0xFFu);\n"
     "      unsigned c0="+p+"_b64[(n>>18)&63u], c1="+p+"_b64[(n>>12)&63u];\n"
     "      unsigned c2="+p+"_b64[(n>>6)&63u], c3="+p+"_b64[n&63u];\n"
     "      acc=acc*131u+((c0<<24)|(c1<<16)|(c2<<8)|c3); }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x64u}, "OptStress114", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress114TC("x64o114", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress114TC("x86o114", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress114TC("a64o114", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress114TC("armo114", "int");

INSTANTIATE_TEST_SUITE_P(OptStress114, X64OptStress114RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress114, X86OptStress114RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress114, A64OptStress114RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress114, ARM32OptStress114RT, ::testing::ValuesIn(kARM), rtTCName);
