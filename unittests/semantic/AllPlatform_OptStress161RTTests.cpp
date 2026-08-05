//===- AllPlatform_OptStress161RTTests.cpp - spiral / rotate90 / flood fill =//
//
// Green guardrails for three more rodata access SHAPES.  Each copies its rodata
// grid into a stack buffer with a plain base+index loop and folds a result that
// depends only on the bytes + control flow (never an absolute VA), so nothing
// touches the deferred i386/ARM32 PIC rodata *interior*-pointer model
// (#477/#487); every probe runs on all four targets.
//
//   * spiral   - clockwise spiral traversal of a rodata 5x5 grid by shrinking the
//                top/bottom/left/right borders.  Pins a boundary-shrink traversal
//                (distinct from any row-major or transpose walk).
//   * rotate90 - 90-degree rotation of a rodata 4x4 matrix into a new buffer via
//                the `dst[c][N-1-r]=src[r][c]` index map.  Pins a coordinate
//                remap (distinct from the spiral border walk above).
//   * flood    - flood-fill connected-component count on a rodata 5x5 color grid
//                using an explicit stack and a visited bitmask.  Pins a 4-neighbour
//                grid DFS (distinct from the graph union-find/topo in #148).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress161RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress161RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress161RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress161RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress161RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress161RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress161RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress161RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress161TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // clockwise spiral traversal of a rodata 5x5 grid (border shrink).
    {p+"_spiral",
     "static const unsigned char "+p+"_sp[25]={3,7,1,9,4,12,2,8,5,0,11,6,14,10,13,1,7,3,9,2,6,8,4,0,5};\n"
     +t+" "+p+"_spiral("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned g[25]; for(int i=0;i<25;i++) g[i]=(unsigned)"+p+"_sp[i]^((s>>(i&7))&7u);\n"
     "    int top=0,bot=4,lft=0,rgt=4; unsigned ord=0u;\n"
     "    while(top<=bot && lft<=rgt){\n"
     "      for(int c=lft;c<=rgt;c++) ord=ord*31u+g[top*5+c]; top++;\n"
     "      for(int r=top;r<=bot;r++) ord=ord*31u+g[r*5+rgt]; rgt--;\n"
     "      if(top<=bot){ for(int c=rgt;c>=lft;c--) ord=ord*31u+g[bot*5+c]; bot--; }\n"
     "      if(lft<=rgt){ for(int r=bot;r>=top;r--) ord=ord*31u+g[r*5+lft]; lft++; }\n"
     "      acc=acc*131u+ord; }\n"
     "    acc=acc*131u+ord; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x25u}, "OptStress161", 2},

    // 90-degree rotation of a rodata 4x4 matrix (coordinate remap).
    {p+"_rotate90",
     "static const unsigned char "+p+"_r9[16]={3,7,1,9,4,12,2,8,5,0,11,6,14,10,13,1};\n"
     +t+" "+p+"_rotate90("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned m[16],m2[16]; for(int i=0;i<16;i++) m[i]=(unsigned)"+p+"_r9[i]^((s>>(i&7))&7u);\n"
     "    for(int r=0;r<4;r++) for(int c=0;c<4;c++) m2[c*4+(3-r)]=m[r*4+c];\n"
     "    for(int i=0;i<16;i++) acc=acc*131u+m2[i]*(unsigned)(i+1);\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x36u}, "OptStress161", 2},

    // flood-fill connected-component count on a rodata 5x5 color grid.
    {p+"_flood",
     "static const unsigned char "+p+"_fl[25]={1,1,2,2,3,1,0,0,2,3,0,0,2,2,3,1,1,2,0,0,3,3,3,0,1};\n"
     +t+" "+p+"_flood("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned g[25]; for(int i=0;i<25;i++) g[i]=((unsigned)"+p+"_fl[i]^((s>>(i&7))&1u))&3u;\n"
     "    unsigned visited=0u, regions=0u;\n"
     "    for(int start=0;start<25;start++){ if((visited>>start)&1u) continue; unsigned color=g[start]; regions++;\n"
     "      int stk[25]; int sp=0; stk[sp++]=start; visited|=(1u<<start); unsigned sz=0u;\n"
     "      while(sp>0){ int cell=stk[--sp]; sz++; int r=cell/5,c=cell%5; int nb[4];\n"
     "        nb[0]=(r>0)?cell-5:-1; nb[1]=(r<4)?cell+5:-1; nb[2]=(c>0)?cell-1:-1; nb[3]=(c<4)?cell+1:-1;\n"
     "        for(int d=0;d<4;d++){ int nc=nb[d]; if(nc>=0 && !((visited>>nc)&1u) && g[nc]==color){ visited|=(1u<<nc); stk[sp++]=nc; } }\n"
     "        acc=acc*131u+(unsigned)cell; }\n"
     "      acc=acc*131u+regions*100u+sz; }\n"
     "    acc=acc*131u+regions; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x67u}, "OptStress161", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress161TC("x64o161", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress161TC("x86o161", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress161TC("a64o161", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress161TC("armo161", "int");

INSTANTIATE_TEST_SUITE_P(OptStress161, X64OptStress161RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress161, X86OptStress161RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress161, A64OptStress161RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress161, ARM32OptStress161RT, ::testing::ValuesIn(kARM), rtTCName);
