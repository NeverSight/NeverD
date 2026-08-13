//===- AllPlatform_DivConstRTTests.cpp - constant divisor magic -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Division / remainder by a COMPILE-TIME CONSTANT.  clang -O2 never emits a
// real div for a constant divisor: it lowers `x / k` into a multiply-high by a
// magic constant plus shifts (and, for signed, a sign-correction add of the
// dividend's sign bit).  This exercises a different and historically buggy
// path than runtime div (X64_DivMulEdge):
//   x86    : `mul`/`imul` whose high half lands in RDX:RAX, then shr/sar (+sign)
//   AArch64: `umulh`/`smulh` (64x64->high 64), then lsr/asr + sign-correction add
//   ARM32  : `umull`/`smull` (32x32->64), extract high word, shifts
// The multiply-high + sign-correction has produced wrong results before
// (RDX:RAX folding, UMULH/SMULH per-lane, sub-register aliasing of the high
// half), so a loop of chained constant divisions is a strong probe.
//
// On x64/a64 the value type is 64-bit (long) -> 64x64->128 multiply-high;
// on ARM32 it is 32-bit (int) -> 32x32->64 multiply-high, so neither path
// needs a runtime division/multiply library helper.  Every function folds its
// state into a 32-bit-sensitive integer return so the ARM32 path (R0 = low 32
// bits) still detects high-half errors.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64DivConstRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64DivConstRT, Verify) { roundTripX64(GetParam()); }

class A64DivConstRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64DivConstRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32DivConstRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32DivConstRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off

static std::vector<RoundTripTC> makeDivTC(const char *prefix, const char *T,
                                          int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Unsigned division by several odd constants (pure umulh/mul-high + shift).
    {p+"_udiv",
     t+" "+p+"_udiv("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for (int i=0;i<256;i++){\n"
     "    unsigned v=(unsigned)(a*(i+1)+i*7);\n"
     "    acc += v/3u + v/7u + v/11u + v/100u + v/1000u + v/65537u; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "DivConst", opt, fl},

    // Unsigned remainder by the same constants (magic-mul + mul-back + sub).
    {p+"_umod",
     t+" "+p+"_umod("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for (int i=0;i<256;i++){\n"
     "    unsigned v=(unsigned)(a*(i+2)+i*5);\n"
     "    acc += v%3u + v%7u + v%11u + v%100u + v%1000u + v%65537u; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "DivConst", opt, fl},

    // Signed division by positive constants: needs the sign-correction add
    // (dividend>>(bits-1)) before the arithmetic shift right.
    {p+"_sdiv",
     t+" "+p+"_sdiv("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=0;i<256;i++){\n"
     "    int v=(int)(a*(i*131+7)) - (int)(a*97);\n"
     "    acc += v/3 + v/7 + v/11 + v/100 + v/1000; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "DivConst", opt, fl},

    // Signed remainder: result sign follows the dividend (extra correction).
    {p+"_smod",
     t+" "+p+"_smod("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=0;i<256;i++){\n"
     "    int v=(int)(a*(i*97+3)) - (int)(a*131);\n"
     "    acc += v%3 + v%7 + v%11 + v%100 + v%1000; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x4455667ULL}, "DivConst", opt, fl},

    // Signed division by powers of two: lowered to add-sign-bit + asr, NOT a
    // plain shift, so a missing correction silently rounds the wrong way for
    // negatives.
    {p+"_spow2",
     t+" "+p+"_spow2("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=0;i<256;i++){\n"
     "    int v=(int)(a*(i*53+1)) - (int)(a*200);\n"
     "    acc += v/2 + v/4 + v/8 + v/16 + v/256 + v/1024; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "DivConst", opt, fl},

    // Signed division/modulo by NEGATIVE constants (magic constant is negative,
    // shift direction and final negate differ).
    {p+"_sneg",
     t+" "+p+"_sneg("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=0;i<256;i++){\n"
     "    int v=(int)(a*(i*61+5)) - (int)(a*150);\n"
     "    acc += v/(-3) + v/(-7) + v/(-100) + v%(-9) + v%(-1000); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "DivConst", opt, fl},

    // Decimal digit sum: classic chained /10 and %10 (long magic-mul chain,
    // loop-carried quotient).  Strongly stresses the mul-high path repeatedly.
    {p+"_digsum",
     t+" "+p+"_digsum("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for (int i=0;i<256;i++){\n"
     "    unsigned n=(unsigned)(a*(i+1)+i);\n"
     "    unsigned s=0; while(n){ s+=n%10u; n/=10u; }\n"
     "    acc += s; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x778899AULL}, "DivConst", opt, fl},

    // Base-N conversion combining quotient and remainder (div and mod of the
    // SAME operands by the same constant — clang shares one magic multiply).
    {p+"_divmod",
     t+" "+p+"_divmod("+t+" a) {\n"
     "  unsigned acc=0;\n"
     "  for (int i=0;i<256;i++){\n"
     "    unsigned v=(unsigned)(a*(i*7+1));\n"
     "    unsigned q=v/97u, r=v%97u;\n"
     "    acc += q*31u + r; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x88990ABULL}, "DivConst", opt, fl},

    // Mixed signed/unsigned constant divisions in one expression (forces both
    // smulh and umulh paths live at once).
    {p+"_mixsign",
     t+" "+p+"_mixsign("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=0;i<256;i++){\n"
     "    int sv=(int)(a*(i*17+1)) - (int)(a*80);\n"
     "    unsigned uv=(unsigned)(a*(i*23+9));\n"
     "    acc += sv/13 + (int)(uv/13u) + sv%17 + (int)(uv%17u); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x99AABBCULL}, "DivConst", opt, fl},

    // is-divisible test feeding a conditional accumulate (mod==0 -> flag -> add).
    {p+"_divisible",
     t+" "+p+"_divisible("+t+" a) {\n"
     "  int acc=0;\n"
     "  for (int i=0;i<300;i++){\n"
     "    unsigned v=(unsigned)(a+i);\n"
     "    if (v%3u==0) acc+=1; if (v%5u==0) acc+=2; if (v%7u==0) acc+=4;\n"
     "    if (v%15u==0) acc+=8; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1020304ULL}, "DivConst", opt, fl},
  };
}

static const std::vector<RoundTripTC> kX64Div =
    makeDivTC("x64dc", "long", 2, "");
static const std::vector<RoundTripTC> kA64Div =
    makeDivTC("a64dc", "long", 2, "");
static const std::vector<RoundTripTC> kARM32Div =
    makeDivTC("armdc", "int", 2, "");

// clang-format on

INSTANTIATE_TEST_SUITE_P(DivConst, X64DivConstRT,
                         ::testing::ValuesIn(kX64Div), rtTCName);
INSTANTIATE_TEST_SUITE_P(DivConst, A64DivConstRT,
                         ::testing::ValuesIn(kA64Div), rtTCName);
INSTANTIATE_TEST_SUITE_P(DivConst, ARM32DivConstRT,
                         ::testing::ValuesIn(kARM32Div), rtTCName);
