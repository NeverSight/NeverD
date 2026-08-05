//===- AllPlatform_OptStress193RTTests.cpp - hull / 2D-Kadane / max-rect ===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0)
// and folds a result that depends only on the bytes + control flow (never an
// absolute VA), so nothing touches the deferred i386/ARM32 PIC rodata
// *interior*-pointer model (#477/#487); every probe runs on all four targets.
//
//   * convexhull - Andrew monotone-chain convex hull of rodata points: sort by
//                  (x,y) then build lower+upper chains, popping on a non-left
//                  turn (signed cross product).  Pins a sort-then-stack hull walk
//                  (distinct from the bare orientation triple #138).
//   * maxsubmat  - maximum-sum submatrix by fixing a row band, compressing the
//                  columns, and running 1-D Kadane on the band.  Pins a 2-D
//                  reduction wrapping Kadane (distinct from the 1-D Kadane #136).
//   * maxrect    - largest rectangle in a rodata histogram via a monotonic index
//                  stack resolving each bar's span on pop.  Pins a max-area
//                  monotonic-stack reduce (distinct from the next-greater resolve
//                  #187 — here the popped bar yields width*height, not a value).
//
// Cross products and column sums stay small (coords/heights < 64, <= 10 terms)
// so every intermediate fits a 32-bit int; no 64-bit widening is required.
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress193RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress193RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress193RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress193RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress193RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress193RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress193RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress193RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress193TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Andrew monotone-chain convex hull of rodata points.
    {p+"_convexhull",
     "static const unsigned char "+p+"_ch[20]={4,1,9,3,2,7,12,8,6,2, 11,11,1,9,8,5,14,6,3,13};\n"
     +t+" "+p+"_convexhull("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int N=10; int px[10], py[10], idx[10];\n"
     "    for(int i=0;i<10;i++){ px[i]=(int)((unsigned)"+p+"_ch[i*2]^((s>>(i&7))&3u));\n"
     "      py[i]=(int)((unsigned)"+p+"_ch[i*2+1]^((s>>((i+3)&7))&3u)); idx[i]=i; }\n"
     "    for(int i=0;i<N;i++){ int m=i; for(int j=i+1;j<N;j++){ int b=idx[j], c=idx[m];\n"
     "        if(px[b]<px[c] || (px[b]==px[c] && py[b]<py[c])) m=j; }\n"
     "      int tmp=idx[i]; idx[i]=idx[m]; idx[m]=tmp; }\n"
     "    int hull[24]; int hs=0;\n"
     "    for(int k=0;k<N;k++){ int pt=idx[k];\n"
     "      while(hs>=2){ int o=hull[hs-2], q=hull[hs-1];\n"
     "        int cr=(px[q]-px[o])*(py[pt]-py[o])-(py[q]-py[o])*(px[pt]-px[o]);\n"
     "        if(cr<=0) hs--; else break; } hull[hs++]=pt; }\n"
     "    int lower=hs+1;\n"
     "    for(int k=N-2;k>=0;k--){ int pt=idx[k];\n"
     "      while(hs>=lower){ int o=hull[hs-2], q=hull[hs-1];\n"
     "        int cr=(px[q]-px[o])*(py[pt]-py[o])-(py[q]-py[o])*(px[pt]-px[o]);\n"
     "        if(cr<=0) hs--; else break; } hull[hs++]=pt; }\n"
     "    unsigned hc=(unsigned)(hs-1), fold=0u;\n"
     "    for(int i=0;i<hs;i++) fold=fold*131u+(unsigned)(px[hull[i]]*31+py[hull[i]]);\n"
     "    acc=acc*131u+hc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xD1u}, "OptStress193", 2},

    // maximum-sum submatrix via row-band compression + 1-D Kadane.
    {p+"_maxsubmat",
     "static const unsigned char "+p+"_sm[36]={\n"
     "40,12,55,4,29,61, 7,44,18,53,2,40, 25,9,49,31,16,52, 3,47,22,60,11,38, 6,50,27,14,58,1, 33,19,45,8,36,23};\n"
     +t+" "+p+"_maxsubmat("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int R=6,C=6; int M[36];\n"
     "    for(int i=0;i<36;i++) M[i]=(int)((unsigned)"+p+"_sm[i]^((s>>(i&7))&7u))-32;\n"
     "    int best=-100000;\n"
     "    for(int top=0; top<R; top++){ int temp[6]; for(int c=0;c<C;c++) temp[c]=0;\n"
     "      for(int bot=top; bot<R; bot++){\n"
     "        for(int c=0;c<C;c++) temp[c]+=M[bot*C+c];\n"
     "        int cur=0, b=-100000;\n"
     "        for(int c=0;c<C;c++){ cur+=temp[c]; if(cur>b) b=cur; if(cur<0) cur=0; }\n"
     "        if(b>best) best=b; } }\n"
     "    acc=acc*131u+(unsigned)(best+100000); out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xD2u}, "OptStress193", 2},

    // largest rectangle in a rodata histogram via a monotonic index stack.
    {p+"_maxrect",
     "static const unsigned char "+p+"_hr[16]={6,2,5,4,5,1,6,3, 7,2,4,6,3,5,2,7};\n"
     +t+" "+p+"_maxrect("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int N=16; int h[16]; for(int i=0;i<16;i++) h[i]=(int)((unsigned)"+p+"_hr[i]^((s>>(i&7))&7u));\n"
     "    int stk[17]; int sp=0; unsigned best=0u;\n"
     "    for(int i=0;i<=N;i++){ int cur=(i==N)?0:h[i];\n"
     "      while(sp>0 && h[stk[sp-1]]>=cur){ int ht=h[stk[sp-1]]; sp--;\n"
     "        int width=(sp==0)? i : (i-stk[sp-1]-1);\n"
     "        unsigned area=(unsigned)ht*(unsigned)width; if(area>best) best=area; }\n"
     "      stk[sp++]=i; }\n"
     "    acc=acc*131u+best; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0xD3u}, "OptStress193", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress193TC("x64o193", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress193TC("x86o193", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress193TC("a64o193", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress193TC("armo193", "int");

INSTANTIATE_TEST_SUITE_P(OptStress193, X64OptStress193RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress193, X86OptStress193RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress193, A64OptStress193RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress193, ARM32OptStress193RT, ::testing::ValuesIn(kARM), rtTCName);
