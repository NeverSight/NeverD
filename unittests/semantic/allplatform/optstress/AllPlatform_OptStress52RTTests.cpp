//===- AllPlatform_OptStress52RTTests.cpp - vec const-pool + tail -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Loops that clang -O2 auto-vectorizes using a `.rodata` CONSTANT VECTOR (weight
// tables, polynomial coefficients, fixed convolution kernels, splat masks) plus
// a scalar remainder, intersecting vectorization with the constant-pool
// redirect: the SIMD body loads the constant vector from rodata while the scalar
// tail reads the SAME constants element-wise.  A prime trip count forces a real
// scalar tail after the vector main loop.
//
//   * wsum   - fixed weight table dot-with-self (vector mul-add + scalar tail).
//   * poly   - Horner polynomial with constant coefficients over an array.
//   * conv   - fixed 5-tap convolution kernel (sliding window).
//   * sad    - sum of absolute differences vs a constant reference vector.
//   * clampw - per-element constant lower/upper clamp then weighted sum.
//   * fircmp - two constant kernels, pick the larger filtered value per element.
//
// All integer, the input array seeds from the LCG, constants are `static const`,
// fold to one return, no float / 64-bit divide helper.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress52RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress52RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress52RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress52RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress52RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress52RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress52RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress52RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress52TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Fixed weight table applied to a seeded array, summed (vectorizable).
    {p+"_wsum",
     "static const int W[19]={3,-1,4,1,-5,9,2,-6,5,3,-5,8,9,-7,9,3,-2,3,8};\n"
     +t+" "+p+"_wsum("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int x[19];\n"
     "  for(int i=0;i<19;i++){ s=s*1103515245u+12345u; x[i]=(int)(s>>16)-32768; }\n"
     "  int acc=0; for(int i=0;i<19;i++) acc+=x[i]*W[i];\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x71u}, "OptStress52", 2},

    // Horner polynomial with constant coefficients over a seeded array.
    {p+"_poly",
     "static const int C[8]={7,3,11,5,13,2,17,9};\n"
     +t+" "+p+"_poly("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=0;\n"
     "  for(int i=0;i<53;i++){ s=s*1103515245u+12345u; int v=(int)(s>>20)&255;\n"
     "    int r=0; for(int k=0;k<8;k++) r=r*v+C[k]; acc=acc*31+r; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x72u}, "OptStress52", 2},

    // Fixed 5-tap convolution kernel over a sliding window.
    {p+"_conv",
     "static const int K[5]={1,-4,6,-4,1};\n"
     +t+" "+p+"_conv("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int x[37];\n"
     "  for(int i=0;i<37;i++){ s=s*1103515245u+12345u; x[i]=(int)(s>>18)&1023; }\n"
     "  int acc=0; for(int i=0;i<33;i++){ int r=0;\n"
     "    for(int k=0;k<5;k++) r+=x[i+k]*K[k]; acc=acc*31+r; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x73u}, "OptStress52", 2},

    // Sum of absolute differences vs a constant reference vector.
    {p+"_sad",
     "static const int R[23]={10,20,30,40,50,60,70,80,90,100,110,120,"
     "130,140,150,160,170,180,190,200,210,220,230};\n"
     +t+" "+p+"_sad("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int x[23];\n"
     "  for(int i=0;i<23;i++){ s=s*1103515245u+12345u; x[i]=(int)(s>>20)&255; }\n"
     "  int acc=0; for(int i=0;i<23;i++){ int d=x[i]-R[i]; acc+=(d<0)?-d:d; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x74u}, "OptStress52", 2},

    // Per-element constant clamp then weighted sum.
    {p+"_clampw",
     "static const int LO[16]={0,5,10,0,20,0,30,5,0,15,0,25,10,0,35,5};\n"
     "static const int HI[16]={100,90,80,120,70,110,60,95,130,75,140,65,"
     "85,150,55,105};\n"
     +t+" "+p+"_clampw("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int x[16];\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; x[i]=(int)(s>>19)&255; }\n"
     "  int acc=0; for(int i=0;i<16;i++){ int v=x[i];\n"
     "    if(v<LO[i])v=LO[i]; if(v>HI[i])v=HI[i]; acc+=v*(i+1); }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x75u}, "OptStress52", 2},

    // Two constant kernels, pick the larger filtered value per element.
    {p+"_fircmp",
     "static const int A[31]={1,2,1,3,1,2,1,4,1,2,1,3,1,2,1,5,"
     "1,2,1,3,1,2,1,4,1,2,1,3,1,2,1};\n"
     "static const int B[31]={2,1,3,1,2,1,4,1,2,1,3,1,2,1,5,1,"
     "2,1,3,1,2,1,4,1,2,1,3,1,2,1,2};\n"
     +t+" "+p+"_fircmp("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int x[31];\n"
     "  for(int i=0;i<31;i++){ s=s*1103515245u+12345u; x[i]=(int)(s>>21)&127; }\n"
     "  int acc=0; for(int i=0;i<31;i++){ int u=x[i]*A[i], w=x[i]*B[i];\n"
     "    acc+=(u>w)?u:w; } return ("+t+")(unsigned)acc; }\n",
     {0x76u}, "OptStress52", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress52TC("x64o52", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress52TC("x86o52", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress52TC("a64o52", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress52TC("armo52", "int");

INSTANTIATE_TEST_SUITE_P(OptStress52, X64OptStress52RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress52, X86OptStress52RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress52, A64OptStress52RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress52, ARM32OptStress52RT, ::testing::ValuesIn(kARM), rtTCName);
