//===- AllPlatform_FPRoundConvRTTests.cpp - FP round-mode convert --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// All-platform roundtrip probing of floating-point round-to-integral and
// round-and-convert idioms that map to dedicated rounding-mode instructions:
//
//   * floorf   -> AArch64 FCVTMS / ARM32 VCVTM / x86 ROUNDSS(floor)+CVTTSS2SI
//   * ceilf    -> AArch64 FCVTPS / ARM32 VCVTP / x86 ROUNDSS(ceil)+CVTTSS2SI
//   * truncf   -> AArch64 FCVTZS / ARM32 VCVT   / x86 CVTTSS2SI
//   * nearbyintf -> AArch64 FCVTNS / ARM32 VCVTN / x86 CVTSS2SI(round-even)
//
// Each kernel runs a serial LCG (so the loop stays scalar — no autovectorized
// packed ROUNDPS/FRINTM) deriving a fractional float per iteration, applies a
// round-and-convert, and folds the integer results to one value for a bit-exact
// original-vs-lifted compare.  Values stay well inside int range so the
// conversion never saturates (saturation edge cases are covered by FPToIntSat).
//
// These per-mode rounding conversions are otherwise uncovered all-platform: the
// AArch64 FCVT?S forms only had a single-platform probe, and the ARM32 VCVT?
// (ARMv8 round-and-convert) forms had no roundtrip coverage at all.  x86 needs
// SSE4.1 for ROUNDSS; ARM32 needs an ARMv8 baseline for VCVT?, run under the
// MAX CPU model.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPRoundConvRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPRoundConvRT, Verify) { roundTripX64(GetParam()); }
class X86FPRoundConvRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FPRoundConvRT, Verify) { roundTripX86(GetParam()); }
class A64FPRoundConvRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPRoundConvRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32FPRoundConvRT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FPRoundConvRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeFPRoundConvTC(const char *prefix, const char *T,
                                                  const char *flags,
                                                  const char *targetOverride,
                                                  int ucCpu) {
  std::string p = prefix, t = T, fl = flags, tov = targetOverride;
  return {
    // floorf -> round toward -inf, then convert to int.
    {p+"_floor",
     t+" "+p+"_floor("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)((h>>9)&0x3FFF)*0.1875f-1500.0f;\n"
     "    acc=acc*131+(int)__builtin_floorf(x); }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "FPRoundConv", 2, fl, false, tov, ucCpu},

    // ceilf -> round toward +inf, then convert to int.
    {p+"_ceil",
     t+" "+p+"_ceil("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)((h>>9)&0x3FFF)*0.1875f-1500.0f;\n"
     "    acc=acc*131+(int)__builtin_ceilf(x); }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "FPRoundConv", 2, fl, false, tov, ucCpu},

    // truncf -> round toward zero, then convert to int.
    {p+"_trunc",
     t+" "+p+"_trunc("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)((h>>9)&0x3FFF)*0.1875f-1500.0f;\n"
     "    acc=acc*131+(int)__builtin_truncf(x); }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "FPRoundConv", 2, fl, false, tov, ucCpu},

    // nearbyintf -> round to nearest, ties to even, then convert to int.
    {p+"_nearbyint",
     t+" "+p+"_nearbyint("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)((h>>9)&0x3FFF)*0.1875f-1500.0f;\n"
     "    acc=acc*131+(int)__builtin_nearbyintf(x); }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "FPRoundConv", 2, fl, false, tov, ucCpu},

    // Two scaled rounds combined (floor of one scale + ceil of another).
    {p+"_scaled",
     t+" "+p+"_scaled("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<40;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)((h>>11)&0xFFF)*0.5f-1000.0f;\n"
     "    int lo=(int)__builtin_floorf(x*0.5f);\n"
     "    int hi=(int)__builtin_ceilf(x*0.25f);\n"
     "    acc=acc*131+lo+hi; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "FPRoundConv", 2, fl, false, tov, ucCpu},

    // Per-iteration rounding mode selected by the running value (all four modes
    // exercised on one dynamic dispatch chain).
    {p+"_mix",
     t+" "+p+"_mix("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u;\n"
     "    float x=(float)(int)((h>>9)&0x3FFF)*0.1875f-1500.0f; int r;\n"
     "    switch(i&3){ case 0: r=(int)__builtin_floorf(x); break;\n"
     "      case 1: r=(int)__builtin_ceilf(x); break;\n"
     "      case 2: r=(int)__builtin_truncf(x); break;\n"
     "      default: r=(int)__builtin_nearbyintf(x); break; }\n"
     "    acc=acc*131+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "FPRoundConv", 2, fl, false, tov, ucCpu},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 =
    makeFPRoundConvTC("x64frc", "long", "-msse4.1 -fno-math-errno", "", -1);
static const std::vector<RoundTripTC> kX86 = [] {
  auto Cases =
      makeFPRoundConvTC("x86frc", "int", "-msse4.1 -fno-math-errno", "", -1);
  for (auto &TC : Cases) {
    // Keep this floating-point regression focused on round/conversion
    // semantics.  Clang otherwise lowers this single source switch into two
    // i386 GOTOFF consumers of one table; that shared-table recovery limitation
    // is intentionally outside this floating-point regression.
    if (TC.Name == "x86frc_mix")
      TC.ExtraFlags += " -fno-jump-tables";
  }
  return Cases;
}();
static const std::vector<RoundTripTC> kA64 =
    makeFPRoundConvTC("a64frc", "long", "-fno-math-errno", "", -1);
static const std::vector<RoundTripTC> kARM = makeFPRoundConvTC(
    "armfrc", "int", "-march=armv8-a -mfpu=neon-fp-armv8 -fno-math-errno",
    "armv8-linux-gnueabihf", UC_CPU_ARM_MAX);

INSTANTIATE_TEST_SUITE_P(FPRoundConv, X64FPRoundConvRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPRoundConv, X86FPRoundConvRT,
                         ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPRoundConv, A64FPRoundConvRT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPRoundConv, ARM32FPRoundConvRT,
                         ::testing::ValuesIn(kARM), rtTCName);
