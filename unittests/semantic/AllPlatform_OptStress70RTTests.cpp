//===- AllPlatform_OptStress70RTTests.cpp - FP switch / tail call -*-C++*-=//
//
// FP control-flow shapes exercising the call/return-ABI machinery: a `switch`
// returning distinct floating-point constants (lowered to a rodata table the
// callee loads into the FP return register with no FLOAT_* producer — the #470
// fix teaches inferReturnType that a value loaded/zero-extended into XMM0/V0/D0
// is a floating-point return, and the emitter narrows the wide call result into
// an 8-byte FP-return-register slice) and an FP-returning tail call.  Each folds
// to one integer return.  All four targets, -O2.
//
//   * fpsw     - switch returning distinct double constants, accumulated.
//   * fptail   - FP-returning tail call (callee result is the caller's result).
//
// (A small struct with floating-point fields returned by value —
// `struct{long;double}` SysV INTEGER+SSE, `struct{double;double}` two-SSE,
// AArch64 HFA `struct{float;float;float}` — is split across two return registers
// on x86-64/AArch64; NeverD recovers it as a single <2 x i64>, dropping the
// second register.  i386/ARM32 return it through memory and round-trip cleanly.
// Tracked as open bug O-6 in the Unicorn unsupported-instructions doc #470.)
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress70RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress70RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress70RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress70RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress70RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress70RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress70RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress70RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress70TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // switch returning distinct double constants, accumulated.
    {p+"_fpsw",
     "static double "+p+"_pick(unsigned) __attribute__((noinline));\n"
     +t+" "+p+"_fpsw("+t+" a){ unsigned s=(unsigned)a; double acc=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    acc += "+p+"_pick((s>>5)&7u); }\n"
     "  return ("+t+")(long long)acc; }\n"
     "static double "+p+"_pick(unsigned k){ switch(k){\n"
     "  case 0: return 1.5; case 1: return 2.25; case 2: return 3.125;\n"
     "  case 3: return 4.0625; case 4: return 5.5; case 5: return 6.75;\n"
     "  case 6: return 7.875; default: return 8.0; } }\n",
     {0x74u}, "OptStress70", 2},

    // FP-returning tail call: the helper's FP result is the caller's result.
    {p+"_fptail",
     "static double "+p+"_g(double) __attribute__((noinline));\n"
     "static double "+p+"_f(double x){ return "+p+"_g(x*2.0 + 1.0); }\n"
     +t+" "+p+"_fptail("+t+" a){ unsigned s=(unsigned)a; double acc=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    acc += "+p+"_f((double)((s>>8)&0x3f)); }\n"
     "  return ("+t+")(long long)acc; }\n"
     "static double "+p+"_g(double x){ return x*0.5 + 3.0; }\n",
     {0x75u}, "OptStress70", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress70TC("x64o70", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress70TC("x86o70", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress70TC("a64o70", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress70TC("armo70", "int");

INSTANTIATE_TEST_SUITE_P(OptStress70, X64OptStress70RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress70, X86OptStress70RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress70, A64OptStress70RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress70, ARM32OptStress70RT, ::testing::ValuesIn(kARM), rtTCName);
