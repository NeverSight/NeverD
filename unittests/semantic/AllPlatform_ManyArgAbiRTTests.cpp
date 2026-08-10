//===- AllPlatform_ManyArgAbiRTTests.cpp - arg-overflow call ABI -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Call-ABI probes for floating-point argument lists that overflow the FP
// parameter registers in ways the earlier call probes never reached.  FPCallAbi
// topped out at 6 FP params (XMM0-5 / V0-5), so the *FP overflow to the stack*
// path (>8 doubles or >8 floats spilling past XMM7 / V7) was never exercised,
// nor a mix where both the integer and the FP register files overflow at once.
// A lifter that drops the overflow FP args, or mis-orders int vs FP stack
// slots, mis-sums.
//
//   * sumd10  - 10 doubles: 8 in XMM/V regs + 2 spilled to the overflow area.
//   * sumf12  - 12 floats: register file overflow at float width.
//   * mix_id  - 8 ints + 6 doubles: integer regs overflow (x86-64) alongside FP.
//   * mix_di  - 6 doubles + 10 ints: FP regs fit, integer regs overflow to stack
//               interleaved after the FP block.
//   * sumd8   - exactly 8 doubles: fills XMM0-7 / V0-7 with no overflow (the
//               boundary case, a green guard for the others).
//
// Every callee has external linkage + noinline so clang keeps a standard-ABI
// call (no IPA arg elision).  Each kernel folds the FP result to an integer
// return; inputs are small integers widened to double, so native and lifted run
// identical IEEE ops and the integer return matches bit-for-bit.  No libm /
// runtime helper is emitted.  -O2.
//
// SCOPE — all four platforms.  x86-64/AArch64 exercise FP argument-register
// overflow (XMM0-7 / V0-7); arm32 soft-float passes/returns FP through core
// integer registers (r0:r1 / stack); i386 cdecl passes ALL FP arguments on the
// stack (#428 two-pass recoverCallAbi clears register-scan false positives for
// cdecl callees whose CalleeRegArgs == 0 after forwarder promotion).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64ManyArgAbiRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64ManyArgAbiRT, Verify) { roundTripX64(GetParam()); }
class A64ManyArgAbiRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64ManyArgAbiRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32ManyArgAbiRT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32ManyArgAbiRT, Verify) { roundTripARM32(GetParam()); }
class X86ManyArgAbiRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86ManyArgAbiRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeManyArgAbiTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 10 doubles: 8 in FP regs + 2 overflow to the stack.
    {p+"_sumd10",
     "double "+p+"_d10(double,double,double,double,double,double,double,double,"
     "double,double) __attribute__((noinline));\n"
     +t+" "+p+"_sumd10("+t+" a){\n"
     "  double b=(double)(unsigned)((unsigned)a&0xffffu);\n"
     "  double r="+p+"_d10(b,b+1,b+2,b+3,b+4,b+5,b+6,b+7,b+8,b+9);\n"
     "  return ("+t+")(int)r; }\n"
     "double "+p+"_d10(double a,double b,double c,double d,double e,double f,"
     "double g,double h,double i,double j){\n"
     "  return a*1+b*2+c*3+d*4+e*5+f*6+g*7+h*8+i*9+j*10; }\n",
     {0x41ULL}, "ManyArgAbi", 2},

    // 12 floats: register file overflow at float width.
    {p+"_sumf12",
     "float "+p+"_f12(float,float,float,float,float,float,float,float,float,"
     "float,float,float) __attribute__((noinline));\n"
     +t+" "+p+"_sumf12("+t+" a){\n"
     "  float b=(float)(unsigned)((unsigned)a&0x3ffu);\n"
     "  float r="+p+"_f12(b,b+1,b+2,b+3,b+4,b+5,b+6,b+7,b+8,b+9,b+10,b+11);\n"
     "  return ("+t+")(int)r; }\n"
     "float "+p+"_f12(float a,float b,float c,float d,float e,float f,float g,"
     "float h,float i,float j,float k,float l){\n"
     "  return a+b*2+c*3+d*4+e*5+f*6+g*7+h*8+i*9+j*10+k*11+l*12; }\n",
     {0x55ULL}, "ManyArgAbi", 2},

    // 8 ints + 6 doubles: integer regs overflow (x86-64) alongside the FP block.
    {p+"_mix_id",
     "long "+p+"_id(int,int,int,int,int,int,int,int,double,double,double,double,"
     "double,double) __attribute__((noinline));\n"
     +t+" "+p+"_mix_id("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; double b=(double)(u&0x7ffu);\n"
     "  long r="+p+"_id((int)u,(int)(u+1),(int)(u+2),(int)(u+3),(int)(u+4),"
     "(int)(u+5),(int)(u+6),(int)(u+7),b,b+1,b+2,b+3,b+4,b+5);\n"
     "  return ("+t+")(unsigned)r; }\n"
     "long "+p+"_id(int a,int b,int c,int d,int e,int f,int g,int h,double p,"
     "double q,double r,double s,double t,double u){\n"
     "  long is=a+2*b+3*c+4*d+5*e+6*f+7*g+8*h;\n"
     "  double fs=p*1+q*2+r*3+s*4+t*5+u*6;\n"
     "  return is+(long)fs; }\n",
     {0x9bULL}, "ManyArgAbi", 2},

    // 6 doubles + 10 ints: FP fits, integer regs overflow to the stack.
    {p+"_mix_di",
     "long "+p+"_di(double,double,double,double,double,double,int,int,int,int,"
     "int,int,int,int,int,int) __attribute__((noinline));\n"
     +t+" "+p+"_mix_di("+t+" a){\n"
     "  unsigned u=(unsigned)a|1u; double b=(double)(u&0x3ffu);\n"
     "  long r="+p+"_di(b,b+1,b+2,b+3,b+4,b+5,(int)u,(int)(u+1),(int)(u+2),"
     "(int)(u+3),(int)(u+4),(int)(u+5),(int)(u+6),(int)(u+7),(int)(u+8),(int)(u+9));\n"
     "  return ("+t+")(unsigned)r; }\n"
     "long "+p+"_di(double p,double q,double r,double s,double t,double u,"
     "int a,int b,int c,int d,int e,int f,int g,int h,int i,int j){\n"
     "  double fs=p*1+q*2+r*3+s*4+t*5+u*6;\n"
     "  long is=a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+9*i+10*j;\n"
     "  return (long)fs+is; }\n",
     {0xa7ULL}, "ManyArgAbi", 2},

    // Exactly 8 doubles: fills XMM0-7 / V0-7, no overflow (boundary guard).
    {p+"_sumd8",
     "double "+p+"_d8(double,double,double,double,double,double,double,double)"
     " __attribute__((noinline));\n"
     +t+" "+p+"_sumd8("+t+" a){\n"
     "  double b=(double)(unsigned)((unsigned)a&0xffffu);\n"
     "  double r="+p+"_d8(b,b+1,b+2,b+3,b+4,b+5,b+6,b+7);\n"
     "  return ("+t+")(int)r; }\n"
     "double "+p+"_d8(double a,double b,double c,double d,double e,double f,"
     "double g,double h){\n"
     "  return a*1+b*2+c*3+d*4+e*5+f*6+g*7+h*8; }\n",
     {0x6dULL}, "ManyArgAbi", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeManyArgAbiTC("x64ma", "long");
static const std::vector<RoundTripTC> kA64 = makeManyArgAbiTC("a64ma", "long");
static const std::vector<RoundTripTC> kARM = makeManyArgAbiTC("armma", "int");
static const std::vector<RoundTripTC> kX86 = makeManyArgAbiTC("x86ma", "int");

INSTANTIATE_TEST_SUITE_P(ManyArgAbi, X64ManyArgAbiRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(ManyArgAbi, A64ManyArgAbiRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(ManyArgAbi, ARM32ManyArgAbiRT, ::testing::ValuesIn(kARM), rtTCName);
INSTANTIATE_TEST_SUITE_P(ManyArgAbi, X86ManyArgAbiRT, ::testing::ValuesIn(kX86), rtTCName);
