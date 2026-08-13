//===- AllPlatform_OptStress218RTTests.cpp - runtime-divisor div/mod =====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Division / remainder by a RUNTIME (data-dependent) divisor — the real
// `idiv`/`div` (x86), `sdiv`/`udiv` + `msub` (AArch64) and `sdiv`/`udiv`
// (ARM32 hwdiv) instructions, NOT the magic-multiply a constant divisor lowers
// to (that path is DivConst).  A loop chains signed and unsigned quotients and
// remainders of loop-carried, full-range (often negative) dividends so the
// sign-extension feeding the dividend (CDQ/CQO, `sxtw`, sign-spread) and the
// quotient/remainder pairing are both exercised every iteration.  The divisor
// is always forced into a safe magnitude `>= 2` (never 0, never -1) so no
// iteration hits the INT_MIN/-1 overflow trap.  64-bit on x64/a64, 32-bit on
// i386/ARM32 so neither path needs a 64-bit divide library helper.
//
//   * sdivmod - signed quotient+remainder by a positive runtime divisor.
//   * udivmod - unsigned quotient+remainder (zero-extended dividend).
//   * negdiv  - signed division by a NEGATIVE runtime divisor (result rounds
//               toward zero; sign of remainder follows the dividend).
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress218RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress218RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress218RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress218RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress218RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress218RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress218RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress218RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
// T = return type; ST = signed divide type; UT = unsigned divide type.
static std::vector<RoundTripTC> makeOptStress218TC(const char *prefix,
                                                   const char *T, const char *ST,
                                                   const char *UT) {
  std::string p = prefix, t = T, st = ST, ut = UT;
  return {
    // Signed quotient + remainder by a positive runtime divisor [2,129].
    {p+"_sdivmod",
     t+" "+p+"_sdivmod("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a^0xABCDEF0123456789ULL;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ "+st+" v=("+st+")h;\n"
     "    "+st+" d=("+st+")(("+st+")((h>>17)&0x7F)+2);\n"
     "    acc=acc*131u+(unsigned)(v/d)+(unsigned)(v%d)*7u;\n"
     "    h=h*6364136223846793005ULL+1u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x1234567u}, "OptStress218", 2},

    // Unsigned quotient + remainder by a runtime divisor [2,1025].
    {p+"_udivmod",
     t+" "+p+"_udivmod("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a*0xD1B54A32D192ED03ULL+1u;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ "+ut+" v=("+ut+")h;\n"
     "    "+ut+" d=("+ut+")((h>>13)&0x3FF)+2u;\n"
     "    acc=acc*131u+(unsigned)(v/d)+(unsigned)(v%d)*7u;\n"
     "    h=h*6364136223846793005ULL+1u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x7654321u}, "OptStress218", 2},

    // Signed division by a NEGATIVE runtime divisor [-129,-2].
    {p+"_negdiv",
     t+" "+p+"_negdiv("+t+" a){\n"
     "  unsigned long long h=(unsigned long long)a^0xF0E1D2C3B4A59687ULL;\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ "+st+" v=("+st+")h;\n"
     "    "+st+" d=("+st+")(-(("+st+")((h>>19)&0x7F)+2));\n"
     "    acc=acc*131u+(unsigned)(v/d)+(unsigned)(v%d)*7u;\n"
     "    h=h*6364136223846793005ULL+1442695040888963407ULL+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x9ABCDEFu}, "OptStress218", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 =
    makeOptStress218TC("x64o218", "long", "long", "unsigned long");
static const std::vector<RoundTripTC> kX86 =
    makeOptStress218TC("x86o218", "int", "int", "unsigned");
static const std::vector<RoundTripTC> kA64 =
    makeOptStress218TC("a64o218", "long", "long", "unsigned long");
static const std::vector<RoundTripTC> kARM =
    makeOptStress218TC("armo218", "int", "int", "unsigned");

INSTANTIATE_TEST_SUITE_P(OptStress218, X64OptStress218RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress218, X86OptStress218RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress218, A64OptStress218RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress218, ARM32OptStress218RT, ::testing::ValuesIn(kARM), rtTCName);
