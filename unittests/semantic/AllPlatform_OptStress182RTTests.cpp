//===- AllPlatform_OptStress182RTTests.cpp - 2D prefix / transpose / spiral =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Green guardrails for three more rodata access SHAPES.  Each reads its rodata
// through plain forward base+index copies (symbol always referenced at offset 0;
// the row-major index r*C+c is a runtime value, so the relocation stays at
// offset 0 like the column scan in #172) and folds a result that depends only on
// the bytes + control flow (never an absolute VA), so nothing touches the
// deferred i386/ARM32 PIC rodata *interior*-pointer model (#477/#487); every
// probe runs on all four targets.
//
//   * prefix2d  - 2-D integral image (summed-area table) with a zero border, then
//                 O(1) rectangle-sum queries.  Pins a 2-D prefix structure
//                 (distinct from the 1-D difference array #172).
//   * transpose - square-matrix transpose plus a trace fold.  Pins an index-swap
//                 gather m[j*N+i] (distinct from the bit-reversal scatter #178).
//   * spiral    - clockwise spiral traversal of a grid via four shrinking edges.
//                 Pins a boundary-walk order (distinct from row/column scans).
//
// Integer in / integer out, file-scope const (rodata) arrays, LCG-seeded,
// folded to one integer return; no float / 64-bit divide / libcall.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress182RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress182RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress182RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress182RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress182RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress182RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress182RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress182RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress182TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 2-D integral image with zero border, then rectangle-sum queries.
    {p+"_prefix2d",
     "static const unsigned char "+p+"_g2[36]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3,2,3,8,4,6,2,6,4,3,3,8,3,2,7,9,5,0,2,8,8};\n"
     +t+" "+p+"_prefix2d("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    int P[49]; for(int i=0;i<49;i++) P[i]=0;\n"
     "    for(int r=0;r<6;r++) for(int c=0;c<6;c++){ int v=(int)(((unsigned)"+p+"_g2[r*6+c]^((s>>((r+c)&7))&3u))&15u);\n"
     "      P[(r+1)*7+(c+1)]=v+P[r*7+(c+1)]+P[(r+1)*7+c]-P[r*7+c]; }\n"
     "    unsigned fold=0u;\n"
     "    for(int q=0;q<6;q++){ int r1=(int)((s>>q)%5u), c1=(int)((s>>(q+1))%5u); int r2=r1+1+(int)((s>>(q+2))%(6u-(unsigned)r1-1u)); int c2=c1+1+(int)((s>>(q+3))%(6u-(unsigned)c1-1u));\n"
     "      int sum=P[r2*7+c2]-P[r1*7+c2]-P[r2*7+c1]+P[r1*7+c1]; fold=fold*131u+(unsigned)sum; }\n"
     "    acc=acc*131u+fold+(unsigned)P[48]; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x3Cu}, "OptStress182", 2},

    // square-matrix transpose plus a trace fold.
    {p+"_transpose",
     "static const unsigned char "+p+"_mt[36]={5,2,7,3,9,4,6,8,1,0,3,5,7,2,9,4,6,1,8,3,5,0,2,7,4,9,1,6,3,8,2,5,7,0,4,9};\n"
     +t+" "+p+"_transpose("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned m[36]; for(int i=0;i<6;i++) for(int j=0;j<6;j++) m[i*6+j]=((unsigned)"+p+"_mt[i*6+j]^((s>>((i+j)&7))&3u))&15u;\n"
     "    unsigned tr=0u; for(int i=0;i<6;i++) tr+=m[i*6+i];\n"
     "    unsigned fold=0u; for(int i=0;i<6;i++) for(int j=0;j<6;j++) fold=fold*131u+m[j*6+i];\n"
     "    acc=acc*131u+fold+tr*7u; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x4Du}, "OptStress182", 2},

    // clockwise spiral traversal via four shrinking edges.
    {p+"_spiral",
     "static const unsigned char "+p+"_sp[36]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36};\n"
     +t+" "+p+"_spiral("+t+" a){\n"
     "  unsigned s=(unsigned)a, out=0;\n"
     "  for(int it=0;it<128;it++){ s=s*1103515245u+12345u; unsigned acc=s;\n"
     "    unsigned g[36]; for(int i=0;i<6;i++) for(int j=0;j<6;j++) g[i*6+j]=((unsigned)"+p+"_sp[i*6+j]^((s>>((i+j)&7))&7u))&63u;\n"
     "    int top=0,bot=5,left=0,right=5; unsigned fold=0u;\n"
     "    while(top<=bot && left<=right){\n"
     "      for(int c=left;c<=right;c++) fold=fold*131u+g[top*6+c]; top++;\n"
     "      for(int r=top;r<=bot;r++) fold=fold*131u+g[r*6+right]; right--;\n"
     "      if(top<=bot){ for(int c=right;c>=left;c--) fold=fold*131u+g[bot*6+c]; bot--; }\n"
     "      if(left<=right){ for(int r=bot;r>=top;r--) fold=fold*131u+g[r*6+left]; left++; } }\n"
     "    acc=acc*131u+fold; out=out*1311u+acc; }\n"
     "  return ("+t+")out; }\n",
     {0x7Eu}, "OptStress182", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress182TC("x64o182", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress182TC("x86o182", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress182TC("a64o182", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress182TC("armo182", "int");

INSTANTIATE_TEST_SUITE_P(OptStress182, X64OptStress182RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress182, X86OptStress182RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress182, A64OptStress182RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress182, ARM32OptStress182RT, ::testing::ValuesIn(kARM), rtTCName);
