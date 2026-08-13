//===- AllPlatform_OptStress244RTTests.cpp - FP abs/neg/sqrt/min-max =====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Single-instruction FP operations: abs/neg (sign-bit manipulation), sqrt
// (`fsqrt`/`vsqrt`/`sqrtsd`), copysign, and min/max via comparison-select.
// These map to dedicated FP opcodes on every target and are folded back to an
// integer, so a lift that mishandles the sign-bit/bitcast paths shows up as a
// return mismatch.  All builtins used lower inline (no libm calls).
//
//   * fabschain - running |x| accumulation.
//   * fsqrtsum  - sum of sqrt of non-negative values.
//   * fminmaxt  - min/max via ternary comparisons.
//   * fnegcond  - conditional negate.
//   * fcopysign - copy sign between values.
//   * fmuladd2  - a*b - c*d with sign tricks.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress244RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress244RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress244RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress244RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress244RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress244RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress244RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress244RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress244TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Running |x| accumulation.
    {p+"_fabschain",
     t+" "+p+"_fabschain("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    double d=(double)(int)h/100000.0; double r=__builtin_fabs(d)+__builtin_fabs(d*0.5);\n"
     "    acc=acc*131u+(unsigned)(int)(r*128.0)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress244", 2},

    // Sum of sqrt of non-negative values.
    {p+"_fsqrtsum",
     t+" "+p+"_fsqrtsum("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    double d=(double)(h>>8); double r=__builtin_sqrt(d);\n"
     "    acc=acc*131u+(unsigned)(int)(r*16.0)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress244", 2},

    // Min/max via ternary comparisons.
    {p+"_fminmaxt",
     t+" "+p+"_fminmaxt("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    double x=(double)(int)h/1000.0, y=(double)(int)(h*7u)/1000.0;\n"
     "    double mn=x<y?x:y, mx=x>y?x:y;\n"
     "    acc=acc*131u+(unsigned)(int)((mx-mn)*4.0)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress244", 2},

    // Conditional negate.
    {p+"_fnegcond",
     t+" "+p+"_fnegcond("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    double d=(double)(int)(h>>5)/4096.0; if(h&1u) d=-d; d+=0.25;\n"
     "    acc=acc*131u+(unsigned)(int)(d*512.0)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress244", 2},

    // Copy sign between values.
    {p+"_fcopysign",
     t+" "+p+"_fcopysign("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    double x=(double)(int)h/2048.0, y=(double)(int)(h<<7)/2048.0;\n"
     "    double r=__builtin_copysign(x,y);\n"
     "    acc=acc*131u+(unsigned)(int)(r*32.0)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress244", 2},

    // a*b - c*d combined with abs.
    {p+"_fmuladd2",
     t+" "+p+"_fmuladd2("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    double aa=(double)(short)h/256.0, bb=(double)(short)(h>>8)/256.0;\n"
     "    double cc=(double)(short)(h>>12)/256.0, dd=(double)(short)(h>>20)/256.0;\n"
     "    double r=__builtin_fabs(aa*bb-cc*dd);\n"
     "    acc=acc*131u+(unsigned)(int)(r*64.0)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress244", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress244TC("x64o244", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress244TC("x86o244", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress244TC("a64o244", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress244TC("armo244", "int");

INSTANTIATE_TEST_SUITE_P(OptStress244, X64OptStress244RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress244, X86OptStress244RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress244, A64OptStress244RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress244, ARM32OptStress244RT, ::testing::ValuesIn(kARM), rtTCName);
