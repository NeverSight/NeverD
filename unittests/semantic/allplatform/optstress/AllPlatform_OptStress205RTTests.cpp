//===- AllPlatform_OptStress205RTTests.cpp - FP / mixed arg overflow =====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Calling-ABI probes for floating-point and mixed int+FP argument lists that
// overflow the FP parameter registers (x86-64 XMM0-7, AArch64 V0-7) onto the
// stack -- the FP dual of the integer overflow-stack-argument path.  i386 passes
// every argument on the stack (x87/SSE compute); ARM32 softfp passes doubles in
// core register pairs then the stack.  Each callee is noinline so clang keeps a
// real ABI call; the entry folds the FP result to one integer return.
//
//   * fp12   - 12 double args (overflow the FP regs), summed -> (int).
//   * fpf12  - 12 float args summed -> (int).
//   * fpmix  - 12 args alternating int/double overflowing both register files.
//   * hfa4d  - a struct of 4 doubles by value (AArch64 HFA in V0-3; stack else).
//   * fpretm - 10 double args summed and returned as a double (FP return path).
//   * dmixo  - 5 ints + 8 doubles: ints fit registers, doubles overflow.
//
// VFP/SSE add + FP->int convert only (cortex-a15 has hardware FP, so no
// __aeabi_dadd libcall); -O2, all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress205RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress205RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress205RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress205RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress205RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress205RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress205RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress205RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress205TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 12 double args overflowing the FP parameter registers, summed.
    {p+"_fp12",
     "double "+p+"_d12(double,double,double,double,double,double,\n"
     "  double,double,double,double,double,double) __attribute__((noinline));\n"
     +t+" "+p+"_fp12("+t+" x){ double v=(double)(int)x;\n"
     "  return ("+t+")(int)"+p+"_d12(v,v+1,v+2,v+3,v+4,v+5,v+6,v+7,v+8,v+9,v+10,v+11); }\n"
     "double "+p+"_d12(double a,double b,double c,double d,double e,double f,\n"
     "  double g,double h,double i,double j,double k,double l){\n"
     "  return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+9*i+10*j+11*k+12*l; }\n",
     {0x10ULL}, "OptStress205", 2},

    // 12 float args summed.
    {p+"_fpf12",
     "float "+p+"_f12(float,float,float,float,float,float,\n"
     "  float,float,float,float,float,float) __attribute__((noinline));\n"
     +t+" "+p+"_fpf12("+t+" x){ float v=(float)(int)x;\n"
     "  return ("+t+")(int)"+p+"_f12(v,v+1,v+2,v+3,v+4,v+5,v+6,v+7,v+8,v+9,v+10,v+11); }\n"
     "float "+p+"_f12(float a,float b,float c,float d,float e,float f,\n"
     "  float g,float h,float i,float j,float k,float l){\n"
     "  return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+9*i+10*j+11*k+12*l; }\n",
     {0x10ULL}, "OptStress205", 2},

    // 12 args alternating int/double overflowing both register files.
    {p+"_fpmix",
     "double "+p+"_m12(int,double,int,double,int,double,\n"
     "  int,double,int,double,int,double) __attribute__((noinline));\n"
     +t+" "+p+"_fpmix("+t+" x){ int n=(int)x; double v=(double)n;\n"
     "  return ("+t+")(int)"+p+"_m12(n,v+1,n+2,v+3,n+4,v+5,n+6,v+7,n+8,v+9,n+10,v+11); }\n"
     "double "+p+"_m12(int a,double b,int c,double d,int e,double f,\n"
     "  int g,double h,int i,double j,int k,double l){\n"
     "  return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+9*i+10*j+11*k+12*l; }\n",
     {0x10ULL}, "OptStress205", 2},

    // struct of 4 doubles by value (AArch64 HFA in V0-3; stack/memory elsewhere).
    {p+"_hfa4d",
     "typedef struct{double a,b,c,d;}"+p+"_H4;\n"
     "double "+p+"_sh4("+p+"_H4 s,double e) __attribute__((noinline));\n"
     +t+" "+p+"_hfa4d("+t+" x){ double v=(double)(int)x; "+p+"_H4 s;\n"
     "  s.a=v; s.b=v+1; s.c=v+2; s.d=v+3;\n"
     "  return ("+t+")(int)"+p+"_sh4(s,v+4); }\n"
     "double "+p+"_sh4("+p+"_H4 s,double e){\n"
     "  return s.a+2*s.b+3*s.c+4*s.d+5*e; }\n",
     {0x10ULL}, "OptStress205", 2},

    // 10 double args summed and returned as a double (FP return path).
    {p+"_fpretm",
     "double "+p+"_r10(double,double,double,double,double,\n"
     "  double,double,double,double,double) __attribute__((noinline));\n"
     +t+" "+p+"_fpretm("+t+" x){ double v=(double)(int)x;\n"
     "  return ("+t+")(int)"+p+"_r10(v,v+1,v+2,v+3,v+4,v+5,v+6,v+7,v+8,v+9); }\n"
     "double "+p+"_r10(double a,double b,double c,double d,double e,\n"
     "  double f,double g,double h,double i,double j){\n"
     "  return a+b+c+d+e+f+g+h+i+j; }\n",
     {0x10ULL}, "OptStress205", 2},

    // 5 ints (fit registers) + 8 doubles (overflow the FP regs).
    {p+"_dmixo",
     "double "+p+"_x13(int,int,int,int,int,double,double,double,double,\n"
     "  double,double,double,double) __attribute__((noinline));\n"
     +t+" "+p+"_dmixo("+t+" x){ int n=(int)x; double v=(double)n;\n"
     "  return ("+t+")(int)"+p+"_x13(n,n+1,n+2,n+3,n+4,\n"
     "    v+5,v+6,v+7,v+8,v+9,v+10,v+11,v+12); }\n"
     "double "+p+"_x13(int a,int b,int c,int d,int e,double f,double g,double h,\n"
     "  double i,double j,double k,double l,double m){\n"
     "  return a+b+c+d+e+f+2*g+3*h+4*i+5*j+6*k+7*l+8*m; }\n",
     {0x10ULL}, "OptStress205", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress205TC("x64o205", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress205TC("x86o205", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress205TC("a64o205", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress205TC("armo205", "int");

INSTANTIATE_TEST_SUITE_P(OptStress205, X64OptStress205RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress205, X86OptStress205RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress205, A64OptStress205RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress205, ARM32OptStress205RT, ::testing::ValuesIn(kARM), rtTCName);
