//===- AllPlatform_FPRoundConvDblRTTests.cpp - double round-convert ------===//
//
// All-platform roundtrip probing of *double*-precision round-and-convert
// idioms — a distinct lift path from the float forms (8-byte FP lanes, separate
// emitter handling) and the family where #506 previously found a scalar
// fixed-point conversion miscompile.  Covers both signed and unsigned results:
//
//   * (T)floor(d)      -> AArch64 FCVTMS / ARM32 VCVTM.S32.F64 / x86 ROUNDSD+CVTTSD2SI
//   * (T)ceil(d)       -> AArch64 FCVTPS / ARM32 VCVTP.S32.F64 / x86 ROUNDSD+CVTTSD2SI
//   * (T)trunc(d)      -> AArch64 FCVTZS / ARM32 VCVT.S32.F64  / x86 CVTTSD2SI
//   * (T)nearbyint(d)  -> AArch64 FRINTI / ARM32 VRINTR.F64    / x86 ROUNDSD(even)
//   * (unsigned)floor  -> AArch64 FCVTMU / ARM32 VCVTM.U32.F64 (unsigned convert)
//   * (unsigned)ceil   -> AArch64 FCVTPU / ARM32 VCVTP.U32.F64 (unsigned convert)
//
// A serial LCG keeps the loop scalar (no packed conversion); values stay inside
// the destination integer range, and unsigned forms feed only positive inputs.
// x86 needs SSE4.1 for ROUNDSD; ARM32 needs an ARMv8 FP baseline for the
// VCVT?.F64 forms, run under the MAX CPU model.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPRoundConvDblRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPRoundConvDblRT, Verify) { roundTripX64(GetParam()); }
class X86FPRoundConvDblRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FPRoundConvDblRT, Verify) { roundTripX86(GetParam()); }
class A64FPRoundConvDblRT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPRoundConvDblRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32FPRoundConvDblRT : public SemanticRoundTripFixture,
                              public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FPRoundConvDblRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeFPRConvDblTC(const char *prefix, const char *T,
                                                 const char *flags,
                                                 const char *targetOverride,
                                                 int ucCpu) {
  std::string p = prefix, t = T, fl = flags, tov = targetOverride;
  return {
    // floor(double) -> signed convert.
    {p+"_floor",
     t+" "+p+"_floor("+t+" a){ unsigned h=(unsigned)a; "+t+" acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*1103515245u+12345u;\n"
     "    double x=(double)(int)((h>>7)&0x1FFFF)*0.013-700.0;\n"
     "    acc=acc*131+("+t+")__builtin_floor(x); }\n"
     "  return acc; }\n",
     {0x1357u}, "FPRoundConvDbl", 2, fl, false, tov, ucCpu},

    // ceil(double) -> signed convert.
    {p+"_ceil",
     t+" "+p+"_ceil("+t+" a){ unsigned h=(unsigned)a; "+t+" acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*1103515245u+12345u;\n"
     "    double x=(double)(int)((h>>7)&0x1FFFF)*0.013-700.0;\n"
     "    acc=acc*131+("+t+")__builtin_ceil(x); }\n"
     "  return acc; }\n",
     {0x2468u}, "FPRoundConvDbl", 2, fl, false, tov, ucCpu},

    // trunc(double) -> signed convert.
    {p+"_trunc",
     t+" "+p+"_trunc("+t+" a){ unsigned h=(unsigned)a; "+t+" acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*1103515245u+12345u;\n"
     "    double x=(double)(int)((h>>7)&0x1FFFF)*0.013-700.0;\n"
     "    acc=acc*131+("+t+")__builtin_trunc(x); }\n"
     "  return acc; }\n",
     {0x369Cu}, "FPRoundConvDbl", 2, fl, false, tov, ucCpu},

    // nearbyint(double) -> round to nearest even, signed convert.
    {p+"_nearbyint",
     t+" "+p+"_nearbyint("+t+" a){ unsigned h=(unsigned)a; "+t+" acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*1103515245u+12345u;\n"
     "    double x=(double)(int)((h>>7)&0x1FFFF)*0.013-700.0;\n"
     "    acc=acc*131+("+t+")__builtin_nearbyint(x); }\n"
     "  return acc; }\n",
     {0x48ACu}, "FPRoundConvDbl", 2, fl, false, tov, ucCpu},

    // floor(double) -> *unsigned* convert (positive inputs only).
    {p+"_flooru",
     t+" "+p+"_flooru("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*1103515245u+12345u;\n"
     "    double x=(double)((h>>10)&0x3FFFFu)*0.07+5.0;\n"
     "    acc=acc*131u+(unsigned)__builtin_floor(x); }\n"
     "  return ("+t+")acc; }\n",
     {0x5AB0u}, "FPRoundConvDbl", 2, fl, false, tov, ucCpu},

    // ceil(double) -> *unsigned* convert (positive inputs only).
    {p+"_ceilu",
     t+" "+p+"_ceilu("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*1103515245u+12345u;\n"
     "    double x=(double)((h>>10)&0x3FFFFu)*0.07+5.0;\n"
     "    acc=acc*131u+(unsigned)__builtin_ceil(x); }\n"
     "  return ("+t+")acc; }\n",
     {0x6BC4u}, "FPRoundConvDbl", 2, fl, false, tov, ucCpu},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 =
    makeFPRConvDblTC("x64frcd", "long", "-msse4.1 -fno-math-errno", "", -1);
static const std::vector<RoundTripTC> kX86 =
    makeFPRConvDblTC("x86frcd", "int", "-msse4.1 -fno-math-errno", "", -1);
static const std::vector<RoundTripTC> kA64 =
    makeFPRConvDblTC("a64frcd", "long", "-fno-math-errno", "", -1);
static const std::vector<RoundTripTC> kARM = makeFPRConvDblTC(
    "armfrcd", "int", "-march=armv8-a -mfpu=fp-armv8 -fno-math-errno",
    "armv8-linux-gnueabihf", UC_CPU_ARM_MAX);

INSTANTIATE_TEST_SUITE_P(FPRoundConvDbl, X64FPRoundConvDblRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPRoundConvDbl, X86FPRoundConvDblRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPRoundConvDbl, A64FPRoundConvDblRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPRoundConvDbl, ARM32FPRoundConvDblRT, ::testing::ValuesIn(kARM), rtTCName);
