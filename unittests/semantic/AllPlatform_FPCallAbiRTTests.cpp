//===- AllPlatform_FPCallAbiRTTests.cpp - FP argument call ABI -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The call-ABI recovery probes added in #405-#413 deliberately passed only
// integer arguments.  These probes cross the function boundary with floating
// point values, which use a *separate* argument-register file the integer
// recovery never touches:
//   - x86-64 SysV: FP args in XMM0-7, FP return in XMM0
//   - AArch64 AAPCS: FP args in D0-7/S0-7, FP return in D0/S0
//   - i386 cdecl: FP args on the stack, FP return in ST0 (x87)
//   - ARM32 softfp: FP args in the integer registers R0-R3 (hardware VFP math)
// Each kernel folds its FP result into one integer return so the harness can
// compare native vs lifted; compiled at -O2 on all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPCallAbiRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPCallAbiRT, Verify) { roundTripX64(GetParam()); }
class X86FPCallAbiRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FPCallAbiRT, Verify) { roundTripX86(GetParam()); }
class A64FPCallAbiRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPCallAbiRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32FPCallAbiRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FPCallAbiRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeFPCallTC(const char *prefix, const char *T,
                                             const char *flags) {
  std::string p = prefix, t = T, f = flags;
  std::vector<RoundTripTC> v = {
    // Two double args (XMM0/XMM1 ; D0/D1), double return (XMM0/D0).
    {p+"_dd",
     "static double "+p+"_h2(double,double) __attribute__((noinline));\n"
     +t+" "+p+"_dd("+t+" a){ double x=(double)a;\n"
     "  double r="+p+"_h2(x*1.25, x*0.5+3.0); return ("+t+")r; }\n"
     "static double "+p+"_h2(double a,double b){ return a*b + a - b*2.0; }\n",
     {0x47ULL}, "FPCall", 2, f},

    // Two float args, float return (narrow SS/S-register lanes).
    {p+"_ff",
     "static float "+p+"_hf(float,float) __attribute__((noinline));\n"
     +t+" "+p+"_ff("+t+" a){ float x=(float)a;\n"
     "  float r="+p+"_hf(x*1.5f, x-2.0f); return ("+t+")r; }\n"
     "static float "+p+"_hf(float a,float b){ return a*b + a/b - b; }\n",
     {0x35ULL}, "FPCall", 2, f},

    // Interleaved int + FP args: forces the integer arg index (RDI/X0) and the
    // FP arg index (XMM0/D0) to advance independently.
    {p+"_mix",
     "static double "+p+"_hm(double,int,double) __attribute__((noinline));\n"
     +t+" "+p+"_mix("+t+" a){ double x=(double)a; int k=(int)a & 7;\n"
     "  double r="+p+"_hm(x*2.0, k, x+0.5); return ("+t+")r; }\n"
     "static double "+p+"_hm(double a,int k,double c){ return a*c + (double)k*a - c; }\n",
     {0x29ULL}, "FPCall", 2, f},

    // Six double args spanning XMM0-5 / D0-5.
    {p+"_many",
     "static double "+p+"_h6(double,double,double,double,double,double)"
     " __attribute__((noinline));\n"
     +t+" "+p+"_many("+t+" a){ double x=(double)a;\n"
     "  double r="+p+"_h6(x,x+1.0,x+2.0,x+3.0,x+4.0,x+5.0);\n"
     "  return ("+t+")r; }\n"
     "static double "+p+"_h6(double a,double b,double c,double d,double e,double g){\n"
     "  return ((((a*2.0+b)*1.5+c)*1.25+d)*1.125+e)*1.0625+g; }\n",
     {0x13ULL}, "FPCall", 2, f},

    // FP accumulator loop: the loop-carried double return feeds the next call's
    // first FP argument every iteration.
    {p+"_acc",
     "static double "+p+"_ha(double,double) __attribute__((noinline));\n"
     +t+" "+p+"_acc("+t+" a){ double acc=(double)a;\n"
     "  for(int i=0;i<8;i++) acc="+p+"_ha(acc,(double)i+1.0);\n"
     "  return ("+t+")acc; }\n"
     "static double "+p+"_ha(double a,double b){ return a*0.5 + b*b - a/b; }\n",
     {0x07ULL}, "FPCall", 2, f},

    // Nested FP calls: an inner FP return becomes the outer call's FP argument.
    {p+"_chain",
     "static double "+p+"_g1(double) __attribute__((noinline));\n"
     "static double "+p+"_g2(double,double) __attribute__((noinline));\n"
     +t+" "+p+"_chain("+t+" a){ double x=(double)a;\n"
     "  double r="+p+"_g2("+p+"_g1(x*1.5), x+7.0); return ("+t+")r; }\n"
     "static double "+p+"_g1(double a){ return a*a - a*0.25 + 1.0; }\n"
     "static double "+p+"_g2(double a,double b){ return a/b + b - a*0.5; }\n",
     {0x21ULL}, "FPCall", 2, f},
  };
  return v;
}
// clang-format on

// Every FP-call form is now covered on all four targets: i386's 4th+ FP stack
// argument (#417) and ARM AAPCS-VFP's `float` arguments in the single-width S
// registers s0,s1,.. (#419 — the S/D register-bank FP-argument model) are both
// recovered.  The `_ff` ARM case in particular exercises the high-half S
// registers (s1 = D0's upper word) that alias no D-register argument slot.
static const std::vector<RoundTripTC> kX64 = makeFPCallTC("x64fpc", "long", "");
static const std::vector<RoundTripTC> kX86 = makeFPCallTC("x86fpc", "int", "");
static const std::vector<RoundTripTC> kA64 = makeFPCallTC("a64fpc", "long", "");
static const std::vector<RoundTripTC> kARM =
    makeFPCallTC("armfpc", "int", "-mfloat-abi=softfp -mfpu=vfpv3 -fno-math-errno");

INSTANTIATE_TEST_SUITE_P(FPCall, X64FPCallAbiRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPCall, X86FPCallAbiRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPCall, A64FPCallAbiRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPCall, ARM32FPCallAbiRT, ::testing::ValuesIn(kARM), rtTCName);
