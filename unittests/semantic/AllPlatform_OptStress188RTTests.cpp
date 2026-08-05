//===- AllPlatform_OptStress188RTTests.cpp - BFS flood / Morton / Rabin-Karp =//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
// Neighbour steps are written inline (no local const dx/dy tables) so no extra
// rodata symbol is materialised.
//
//   * bfsflood  - 4-neighbour BFS flood fill of a rodata-seeded grid with walls,
//                 counting reachable cells from a seed via a ring queue.  Pins a
//                 queue-driven graph traversal (distinct from the DFS/union-find
//                 #177 and Floyd chase).
//   * morton    - Morton (Z-order) code: interleave two 4-bit lanes and decode
//                 back, checking the round-trip.  Pins a bit-interleave shuffle
//                 (distinct from the bit-reversal #178 and Gray code).
//   * rabinkarp - Rabin-Karp substring census by polynomial rolling hash with a
//                 verify pass.  Pins a rolling-hash window (distinct from the
//                 Z-function #180 and KMP).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress188RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress188RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress188RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress188RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress188RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress188RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress188RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress188RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress188TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 4-neighbour BFS flood fill of a grid with walls (ring queue).
    {p+"_bfsflood",
     "static const unsigned char "+p+"_bg[36]={5,2,9,1,6,4,7,3,8,0,5,2,9,1,6,4,7,3,8,0,5,2,9,1,6,4,7,3,8,0,5,2,9,1,6,4};\n"
     +t+" "+p+"_bfsflood("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned wall[36]; for(int i=0;i<36;i++) wall[i]=(((unsigned)"+p+"_bg[i]^((s>>(i&7))&7u))&1u);\n"
     "    int q[36]; int qh=0, qt=0; unsigned vis[36]; for(int i=0;i<36;i++) vis[i]=0u;\n"
     "    int start=(int)(s%36u); if(!wall[start]){ vis[start]=1u; q[qt++]=start; }\n"
     "    unsigned count=0u, fold=0u;\n"
     "    while(qh<qt){ int cur=q[qh++]; count++; int r=cur/6, c=cur%6; fold=fold*131u+(unsigned)cur;\n"
     "      if(r>0){ int ni=(r-1)*6+c; if(!wall[ni]&&!vis[ni]){ vis[ni]=1u; q[qt++]=ni; } }\n"
     "      if(r<5){ int ni=(r+1)*6+c; if(!wall[ni]&&!vis[ni]){ vis[ni]=1u; q[qt++]=ni; } }\n"
     "      if(c>0){ int ni=r*6+(c-1); if(!wall[ni]&&!vis[ni]){ vis[ni]=1u; q[qt++]=ni; } }\n"
     "      if(c<5){ int ni=r*6+(c+1); if(!wall[ni]&&!vis[ni]){ vis[ni]=1u; q[qt++]=ni; } } }\n"
     "    acc=acc*131u+count*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x44u}, "OptStress188", 2},

    // Morton (Z-order) code: interleave two 4-bit lanes and decode back.
    {p+"_morton",
     "static const unsigned char "+p+"_mo[16]={37,12,58,4,29,61,7,44,18,53,2,40,25,9,49,31};\n"
     +t+" "+p+"_morton("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned fold=0u;\n"
     "    for(int i=0;i<16;i++){ unsigned xy=((unsigned)"+p+"_mo[i]^((s>>(i&7))&255u)); unsigned xx=xy&15u, yy=(xy>>4)&15u;\n"
     "      unsigned m=0u; for(int b=0;b<4;b++){ m|=((xx>>b)&1u)<<(2*b); m|=((yy>>b)&1u)<<(2*b+1); }\n"
     "      unsigned dx=0u, dy=0u; for(int b=0;b<4;b++){ dx|=((m>>(2*b))&1u)<<b; dy|=((m>>(2*b+1))&1u)<<b; }\n"
     "      fold=fold*131u+m+((dx==xx&&dy==yy)?7u:0u); }\n"
     "    acc=acc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x53u}, "OptStress188", 2},

    // Rabin-Karp substring census by polynomial rolling hash + verify.
    {p+"_rabinkarp",
     "static const unsigned char "+p+"_rk[24]={1,2,1,3,1,2,1,2,3,1,2,1,1,2,3,1,2,1,3,1,2,1,2,3};\n"
     +t+" "+p+"_rabinkarp("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0, B=131u, M=1000003u;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned arr[24]; for(int i=0;i<24;i++) arr[i]=((unsigned)"+p+"_rk[i]^((s>>(i&7))&1u))&7u;\n"
     "    int W=4; int pstart=(int)(s%21u);\n"
     "    unsigned phash=0u; for(int i=0;i<W;i++) phash=(phash*B+arr[pstart+i])%M;\n"
     "    unsigned wh=0u, pw=1u; for(int i=0;i<W;i++){ wh=(wh*B+arr[i])%M; if(i<W-1) pw=(pw*B)%M; }\n"
     "    unsigned matches=0u;\n"
     "    for(int i=0;i+W<=24;i++){ if(i>0) wh=(((wh+M-(arr[i-1]*pw)%M)%M)*B+arr[i+W-1])%M;\n"
     "      if(wh==phash){ int ok=1; for(int k=0;k<W;k++) if(arr[i+k]!=arr[pstart+k]){ ok=0; break; } if(ok) matches++; } }\n"
     "    acc=acc*131u+matches*131u+phash+(unsigned)pstart; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x84u}, "OptStress188", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress188TC("x64o188", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress188TC("x86o188", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress188TC("a64o188", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress188TC("armo188", "int");

INSTANTIATE_TEST_SUITE_P(OptStress188, X64OptStress188RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress188, X86OptStress188RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress188, A64OptStress188RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress188, ARM32OptStress188RT, ::testing::ValuesIn(kARM), rtTCName);
