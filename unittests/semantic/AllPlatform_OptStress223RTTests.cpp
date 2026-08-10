//===- AllPlatform_OptStress223RTTests.cpp - mixed int/FP calls in loops ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Adversarial probes for the call-return routing fixed in OptStress221: a loop
// that mixes INT-returning and FP-returning calls (so the integer return
// register and the FP return register are BOTH live across the loop), nested FP
// calls, an int-returning callee whose argument is computed in the FP register
// (the exact shape that misrouted), and an FP call result consumed by a branch.
// These pin the discrimination between "the call returns int (FP register is
// caller scratch)" and "the call returns FP (its result is the carried value)".
//
//   * mixcall - one int-call and one fp-call per iteration, both folded in.
//   * intfparg - int-returning callee taking a `double` argument computed in
//                the FP register each iteration (the OptStress221 shape, direct).
//   * nestfp   - nested FP calls f(g(x)) in the loop.
//   * fpbr     - FP call result drives a comparison/branch.
//   * fpacc2   - two independent FP accumulators each fed by its own fp-call.
//   * intacc_fp- int accumulator fed by an int-call while an FP scratch value
//                is recomputed each iteration (no fp-call: pure caller scratch).
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress223RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress223RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress223RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress223RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress223RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress223RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress223RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress223RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress223TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // An int-call and an fp-call in the same loop: RAX and XMM0 both live.
    {p+"_mixcall",
     "static int "+p+"_fi(int) __attribute__((noinline));\n"
     "static double "+p+"_fd(double) __attribute__((noinline));\n"
     +t+" "+p+"_mixcall("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    int a="+p+"_fi((int)(h>>3));\n"
     "    double b="+p+"_fd((double)((h>>11)&0xff));\n"
     "    acc=acc*131u+(unsigned)a+(unsigned)(int)b+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_fi(int v){ return v*7+3; }\n"
     "static double "+p+"_fd(double v){ return v*2.0 - 1.0; }\n",
     {0x12345u}, "OptStress223", 2},

    // Int-returning callee whose argument is a double formed in the FP register
    // each iteration -- the exact shape that misrouted in OptStress221.
    {p+"_intfparg",
     "static int "+p+"_g(double) __attribute__((noinline));\n"
     +t+" "+p+"_intfparg("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    int r="+p+"_g((double)((h>>5)&0x3ff)+0.5);\n"
     "    acc=acc*131u+(unsigned)r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_g(double v){ return (int)(v*3.0) ^ 0x5A; }\n",
     {0x23456u}, "OptStress223", 2},

    // Nested FP calls f(g(x)) in the loop.
    {p+"_nestfp",
     "static double "+p+"_g(double) __attribute__((noinline));\n"
     "static double "+p+"_f(double) __attribute__((noinline));\n"
     +t+" "+p+"_nestfp("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    double r="+p+"_f("+p+"_g((double)((h>>7)&0x7f)));\n"
     "    acc=acc*131u+(unsigned)(int)r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static double "+p+"_g(double v){ return v*1.5 + 2.0; }\n"
     "static double "+p+"_f(double v){ return v*0.5 - 3.0; }\n",
     {0x34567u}, "OptStress223", 2},

    // FP call result drives a comparison/branch.
    {p+"_fpbr",
     "static double "+p+"_fd(double) __attribute__((noinline));\n"
     +t+" "+p+"_fpbr("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    double r="+p+"_fd((double)((int)h>>9));\n"
     "    if(r>0.0) acc+=(unsigned)(int)r; else acc-=(unsigned)(int)(-r);\n"
     "    acc=acc*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static double "+p+"_fd(double v){ return v*0.75 - 10.0; }\n",
     {0x45678u}, "OptStress223", 2},

    // Two independent FP accumulators, each fed by its own fp-call.
    {p+"_fpacc2",
     "static double "+p+"_p(double) __attribute__((noinline));\n"
     "static double "+p+"_q(double) __attribute__((noinline));\n"
     +t+" "+p+"_fpacc2("+t+" x){ unsigned h=(unsigned)x; double s1=0,s2=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    s1+="+p+"_p((double)((h>>4)&0x3f));\n"
     "    s2+="+p+"_q((double)((h>>12)&0x3f)); }\n"
     "  return ("+t+")(long long)(s1*2.0 + s2); }\n"
     "static double "+p+"_p(double v){ return v + 0.5; }\n"
     "static double "+p+"_q(double v){ return v*1.5 - 0.25; }\n",
     {0x56789u}, "OptStress223", 2},

    // Int accumulator fed by an int-call while a pure FP scratch value is
    // recomputed each iteration (no fp-call): locks the dead-scratch case.
    {p+"_intacc_fp",
     "static int "+p+"_fi(int) __attribute__((noinline));\n"
     +t+" "+p+"_intacc_fp("+t+" x){ unsigned h=(unsigned)x; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    double scratch=(double)((h>>6)&0xff)*1.25;\n"
     "    int r="+p+"_fi((int)scratch);\n"
     "    acc=acc*131u+(unsigned)r+(unsigned)(int)scratch+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n"
     "static int "+p+"_fi(int v){ return v*11+5; }\n",
     {0x6789Au}, "OptStress223", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress223TC("x64o223", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress223TC("x86o223", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress223TC("a64o223", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress223TC("armo223", "int");

INSTANTIATE_TEST_SUITE_P(OptStress223, X64OptStress223RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress223, X86OptStress223RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress223, A64OptStress223RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress223, ARM32OptStress223RT, ::testing::ValuesIn(kARM), rtTCName);
