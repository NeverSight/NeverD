//===- AllPlatform_OptStress63RTTests.cpp - FP numeric kernels --*-C++*-=//
//
// Floating-point numeric kernels using only +,-,*,/ on double (no libm call:
// Newton iteration replaces sqrt/recip).  Drives the FP lift + FP-ABI + FP
// constant-pool paths that differ per target — x87 on i386, SSE2 on x86-64,
// FP/NEON on AArch64, VFP on ARM32 — an area with frequent recent fixes (FP
// return ABI, i386 x87 stack params, constant-pool mapping).  Each kernel
// reduces its double accumulator to an integer return so the harness compares
// a single value; a wrong FP opcode/width surfaces as a mismatch while ULP
// noise is truncated away.
//
//   * horner  - Horner polynomial evaluation with double coefficients.
//   * newton  - Newton-Raphson reciprocal / sqrt (pure mul/sub).
//   * welford - running mean + variance (Welford) over a stream.
//   * cmul    - complex multiply + magnitude-squared chain.
//   * dotp    - double dot product with running normalization.
//   * det3    - 3x3 matrix determinant (mul/sub heavy).
//
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress63RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress63RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress63RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress63RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress63RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress63RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress63RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress63RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress63TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Horner polynomial evaluation with double coefficients.
    {p+"_horner",
     t+" "+p+"_horner("+t+" a){\n"
     "  unsigned s=(unsigned)a; double acc=0;\n"
     "  const double C[6]={0.5,-1.25,0.75,-0.125,2.0,-0.0625};\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    double x=((double)((s>>8)&0xff))/64.0 - 2.0; double r=C[0];\n"
     "    r=r*x+C[1]; r=r*x+C[2]; r=r*x+C[3]; r=r*x+C[4]; r=r*x+C[5];\n"
     "    acc += r*r; }\n"
     "  return ("+t+")(long long)(acc*16.0); }\n",
     {0xB1u}, "OptStress63", 2},

    // Newton-Raphson reciprocal / sqrt (pure mul/sub, no libm).
    {p+"_newton",
     t+" "+p+"_newton("+t+" a){\n"
     "  unsigned s=(unsigned)a; double acc=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    double v=((double)((s>>7)&0x3ff))+1.0;\n"
     "    double y=0.05; for(int k=0;k<6;k++) y=y*(2.0-v*y);\n"     // 1/v
     "    double g=v*0.25; for(int k=0;k<8;k++) g=0.5*(g+v/g);\n"   // sqrt(v) via div
     "    acc += y*1000.0 + g; }\n"
     "  return ("+t+")(long long)(acc*8.0); }\n",
     {0xB2u}, "OptStress63", 2},

    // Running mean + variance (Welford) over a stream.
    {p+"_welford",
     t+" "+p+"_welford("+t+" a){\n"
     "  unsigned s=(unsigned)a; double mean=0,m2=0; long n=0;\n"
     "  for(int i=0;i<300;i++){ s=s*1103515245u+12345u;\n"
     "    double x=((double)((s>>5)&0xffff))*0.125 - 2048.0; n++;\n"
     "    double d=x-mean; mean+=d/(double)n; double d2=x-mean; m2+=d*d2; }\n"
     "  double var=m2/(double)n;\n"
     "  return ("+t+")(long long)(mean*4.0 + var); }\n",
     {0xB3u}, "OptStress63", 2},

    // Complex multiply + magnitude-squared chain.
    {p+"_cmul",
     t+" "+p+"_cmul("+t+" a){\n"
     "  unsigned s=(unsigned)a; double ar=1.0,ai=0.0,acc=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    double br=((double)((s>>6)&0xff))/128.0-1.0;\n"
     "    double bi=((double)((s>>16)&0xff))/128.0-1.0;\n"
     "    double nr=ar*br-ai*bi; double ni=ar*bi+ai*br;\n"
     "    ar=nr; ai=ni; double mag=ar*ar+ai*ai;\n"
     "    if(mag>4.0){ ar/=2.0; ai/=2.0; } acc+=mag; }\n"
     "  return ("+t+")(long long)(acc*32.0); }\n",
     {0xB4u}, "OptStress63", 2},

    // Double dot product with running normalization.
    {p+"_dotp",
     t+" "+p+"_dotp("+t+" a){\n"
     "  unsigned s=(unsigned)a; double acc=0;\n"
     "  for(int it=0;it<120;it++){ s=s*1103515245u+12345u;\n"
     "    double u[6],v[6],du=0,dv=0,dot=0;\n"
     "    for(int i=0;i<6;i++){ s=s*1103515245u+12345u;\n"
     "      u[i]=((double)((s>>9)&0xff))-128.0; v[i]=((double)((s>>17)&0xff))-128.0;\n"
     "      du+=u[i]*u[i]; dv+=v[i]*v[i]; }\n"
     "    for(int i=0;i<6;i++) dot+=u[i]*v[i];\n"
     "    double den=du*dv+1.0; acc += dot*dot/den; }\n"
     "  return ("+t+")(long long)(acc*64.0); }\n",
     {0xB5u}, "OptStress63", 2},

    // 3x3 matrix determinant (mul/sub heavy).
    {p+"_det3",
     t+" "+p+"_det3("+t+" a){\n"
     "  unsigned s=(unsigned)a; double acc=0;\n"
     "  for(int it=0;it<150;it++){ double m[9];\n"
     "    for(int i=0;i<9;i++){ s=s*1103515245u+12345u; m[i]=((double)((s>>11)&0x3f))-32.0; }\n"
     "    double d = m[0]*(m[4]*m[8]-m[5]*m[7])\n"
     "             - m[1]*(m[3]*m[8]-m[5]*m[6])\n"
     "             + m[2]*(m[3]*m[7]-m[4]*m[6]);\n"
     "    acc += d/16.0; }\n"
     "  return ("+t+")(long long)acc; }\n",
     {0xB6u}, "OptStress63", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress63TC("x64o63", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress63TC("x86o63", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress63TC("a64o63", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress63TC("armo63", "int");

INSTANTIATE_TEST_SUITE_P(OptStress63, X64OptStress63RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress63, X86OptStress63RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress63, A64OptStress63RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress63, ARM32OptStress63RT, ::testing::ValuesIn(kARM), rtTCName);
