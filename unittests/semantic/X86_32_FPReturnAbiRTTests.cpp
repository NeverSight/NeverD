//===- X86_32_FPReturnAbiRTTests.cpp - i386 external st0 FP return -*-C++*-=//
//
// i386 cdecl returns a floating-point value through the x87 top-of-stack (st0),
// not XMM0: an external callee loads its result onto st0 (`fldl`) before `ret`
// and the caller reads it back with `fstp [mem]`.  NeverD modeled every i386 FP
// return as the XMM0 vector return (clang's internal convention for *static*
// functions), so an external FP-returning call lost its result (the caller's
// st0 read was never connected to the call).  #427 models the st0 return:
// inferReturnType marks the x87 return convention, declareFunc declares a scalar
// FP return so LLVM lowers it to st0, modelCallX87Return reconnects the caller's
// post-call st0 read to the call result, and a narrower `float` result is
// fp-extended into the 8-byte st0 register so the caller's fstp width conversion
// reads the right value.  These probes call external (noinline) FP-returning
// functions and fold the result to an integer so native and lifted emulation
// compare a scalar.  -O2.
//
// SCOPE — `double` and `float` st0 returns with up to two FP arguments.  One
// i386 follow-up is not covered yet (tracked in the Unicorn unsupported-instructions doc):
// recovering many / mixed FP arguments on the cdecl stack (the
// AllPlatform_ManyArgAbi kernels), the i386 FP stack-argument recovery, which is
// distinct from and orthogonal to the st0 return modeled here.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X86FPReturnAbiRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FPReturnAbiRT, Verify) { roundTripX86(GetParam()); }

// clang-format off
static const std::vector<RoundTripTC> kX86FPRet = {
  // One double argument, double return through st0.
  {"x86fr_d1",
   "double x86fr_f1(double) __attribute__((noinline));\n"
   "int x86fr_d1(int a){ double r=x86fr_f1((double)(a&0x7fff)); return (int)r; }\n"
   "double x86fr_f1(double x){ return x*3.0 + 1.0; }\n",
   {0x123ULL}, "X86FPReturn", 2},

  // Two double arguments, double return through st0.
  {"x86fr_d2",
   "double x86fr_f2(double,double) __attribute__((noinline));\n"
   "int x86fr_d2(int a){ double b=(double)(a&0x7fff);\n"
   "  double r=x86fr_f2(b,b+2.0); return (int)r; }\n"
   "double x86fr_f2(double x,double y){ return x*5.0 + y*7.0; }\n",
   {0x55ULL}, "X86FPReturn", 2},

  // One float argument, float return through st0 (4-byte st0 width).
  {"x86fr_f1",
   "float x86fr_ff(float) __attribute__((noinline));\n"
   "int x86fr_f1(int a){ float r=x86fr_ff((float)(a&0x7ff)); return (int)r; }\n"
   "float x86fr_ff(float x){ return x*2.0f + 3.0f; }\n",
   {0x77ULL}, "X86FPReturn", 2},
};
// clang-format on

INSTANTIATE_TEST_SUITE_P(X86FPReturn, X86FPReturnAbiRT,
                         ::testing::ValuesIn(kX86FPRet), rtTCName);
