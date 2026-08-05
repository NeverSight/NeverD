//===- AllPlatform_OptStress138RTTests.cpp - orient / Manhattan / Bresenham =//
//
// Green guardrails for three more rodata access SHAPES, all address-independent
// (the folded result depends only on the bytes in the globals + the control
// flow, never on an absolute VA) and all reached by pure index arithmetic from
// the array base (`tab[idx]`, never an interior pointer), so none touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487) and every
// probe runs on all four targets.
//
//   * orient - 2D orientation predicate over rodata point triples: the signed
//              cross product `(b-a)x(c-a)`, counting left/right turns.  Pins a
//              signed geometric determinant (distinct from any unsigned hash).
//   * sqdist - all-pairs squared-Euclidean distance over rodata points with a
//              running minimum (closest-pair-by-L2^2).  Pins a `dx*dx+dy*dy`
//              metric sweep in pure unsigned modular arithmetic (distinct from
//              the min-plus closure and from any abs-based metric).
//   * bres   - Bresenham line rasterization between rodata endpoints with the
//              integer error-term stepping `e2=2*err`.  Pins a two-axis error
//              accumulator march (distinct from any DDA or table walk).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress138RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress138RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress138RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress138RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress138RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress138RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress138RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress138RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress138TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 2D orientation (signed cross product) over rodata point triples.
    {p+"_orient",
     "static const unsigned char "+p+"_pts[24]={\n"
     "10,10, 40,12, 70,30, 90,60, 80,95, 50,98, 20,80, 8,50,\n"
     "30,40, 60,35, 75,70, 25,65};\n"
     +t+" "+p+"_orient("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s, lefts=0u;\n"
     "    for(int i=0;i<12;i++){ int ax="+p+"_pts[(i%12)*2], ay="+p+"_pts[(i%12)*2+1];\n"
     "      int bx="+p+"_pts[((i+1)%12)*2], by="+p+"_pts[((i+1)%12)*2+1];\n"
     "      int cx="+p+"_pts[((i+2)%12)*2], cy="+p+"_pts[((i+2)%12)*2+1];\n"
     "      int o=(bx-ax)*(cy-ay)-(by-ay)*(cx-ax);\n"
     "      if(o>0) lefts++; acc=acc*131u+(unsigned)(o+100000); }\n"
     "    acc=acc*131u+lefts; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x21u}, "OptStress138", 2},

    // all-pairs squared-Euclidean distance over rodata points with running min.
    {p+"_sqdist",
     "static const unsigned char "+p+"_pts[20]={\n"
     "12,40, 55,18, 33,77, 80,25, 60,60, 5,90, 95,50, 45,5, 70,85, 20,30};\n"
     +t+" "+p+"_sqdist("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s, mind=0xFFFFFFFFu;\n"
     "    for(int i=0;i<10;i++) for(int j=i+1;j<10;j++){\n"
     "      unsigned dx=(unsigned)"+p+"_pts[i*2]-(unsigned)"+p+"_pts[j*2];\n"
     "      unsigned dy=(unsigned)"+p+"_pts[i*2+1]-(unsigned)"+p+"_pts[j*2+1];\n"
     "      unsigned d=dx*dx+dy*dy+((s>>((i+j)&7))&1u);\n"
     "      if(d<mind) mind=d; acc=acc*131u+d; }\n"
     "    acc=acc*131u+mind; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x52u}, "OptStress138", 2},

    // Bresenham line rasterization between rodata endpoints (error-term march).
    {p+"_bres",
     "static const unsigned char "+p+"_ep[16]={\n"
     "2,3,30,20, 5,25,28,4, 1,1,27,27, 15,2,3,26};\n"
     +t+" "+p+"_bres("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<96;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    for(int seg=0;seg<4;seg++){\n"
     "      int x0="+p+"_ep[seg*4], y0="+p+"_ep[seg*4+1];\n"
     "      int x1="+p+"_ep[seg*4+2], y1="+p+"_ep[seg*4+3];\n"
     "      int dx=x1-x0; if(dx<0) dx=-dx; int dy=y1-y0; if(dy<0) dy=-dy;\n"
     "      int sx=x0<x1?1:-1, sy=y0<y1?1:-1, err=dx-dy, x=x0, y=y0, guard=0;\n"
     "      while(guard<80){ acc=acc*131u+(unsigned)(x*32+y);\n"
     "        if(x==x1 && y==y1) break; int e2=2*err;\n"
     "        if(e2>-dy){ err-=dy; x+=sx; } if(e2<dx){ err+=dx; y+=sy; } guard++; } }\n"
     "    out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x6Eu}, "OptStress138", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress138TC("x64o138", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress138TC("x86o138", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress138TC("a64o138", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress138TC("armo138", "int");

INSTANTIATE_TEST_SUITE_P(OptStress138, X64OptStress138RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress138, X86OptStress138RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress138, A64OptStress138RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress138, ARM32OptStress138RT, ::testing::ValuesIn(kARM), rtTCName);
