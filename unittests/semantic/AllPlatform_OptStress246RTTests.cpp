//===- AllPlatform_OptStress246RTTests.cpp - fixed-point cross-word mul ===//
//
// Fixed-point (Q-format) multiplies: a 32x32->64 product followed by a
// SUB-WORD constant shift, so the result spans the boundary between the two
// halves of the product (bits 16..47, 15..46, 24..55 ...).  clang lowers this
// to `mul`+`shrd` (i386 EDX:EAX), `umull/smull`+`lsr/orr` (ARM32), `umulh/mul`
// fold (AArch64) or a 64-bit imul+shr (x64).  Extracting a window that crosses
// the high/low product boundary is the historically fragile mul-high path
// (RDX:RAX folding, smull high+low recombine) taken to a finer granularity
// than the whole-high-word DivConst/Wide64Mul probes.
//
// Both operands are forced to 32 bits before the widening multiply, so every
// target performs an exact 32x32->64 multiply with no 128-bit / libcall path.
//
//   * q16u   - unsigned Q16.16 multiply  (>>16 of an unsigned product).
//   * q16s   - signed Q16.16 multiply    (>>16 of a signed product; sign bits).
//   * mulhiu - whole high word           (>>32).
//   * q24u   - unsigned Q8.24            (>>24).
//   * q15dsp - signed Q15 MAC accumulator(>>15, DSP-style).
//   * recombo- xor of low word with a shifted high window.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress246RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress246RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress246RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress246RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress246RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress246RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress246RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress246RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress246TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Unsigned Q16.16 multiply: bits 16..47 of an unsigned 32x32 product.
    {p+"_q16u",
     t+" "+p+"_q16u("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h, y=h*2654435761u+(unsigned)i;\n"
     "    unsigned r=(unsigned)(((unsigned long long)x*y)>>16);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress246", 2},

    // Signed Q16.16 multiply: sign of the product flows through the window.
    {p+"_q16s",
     t+" "+p+"_q16s("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)h, y=(int)(h*40503u)-(int)i;\n"
     "    int r=(int)(((long long)x*y)>>16);\n"
     "    acc=acc*131u+(unsigned)r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress246", 2},

    // Whole high word (>>32): classic mul-high.
    {p+"_mulhiu",
     t+" "+p+"_mulhiu("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h^0x9e3779b9u, y=h+0x7f4a7c15u;\n"
     "    unsigned r=(unsigned)(((unsigned long long)x*y)>>32);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress246", 2},

    // Unsigned Q8.24: bits 24..55 of the product.
    {p+"_q24u",
     t+" "+p+"_q24u("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h*65599u, y=h+(unsigned)(i*7);\n"
     "    unsigned r=(unsigned)(((unsigned long long)x*y)>>24);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress246", 2},

    // Signed Q15 multiply-accumulate (DSP-style >>15).
    {p+"_q15dsp",
     t+" "+p+"_q15dsp("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int x=(int)(short)h, y=(int)(short)(h>>16);\n"
     "    acc += (int)(((long long)x*y)>>15); }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x56789u}, "OptStress246", 2},

    // xor of the low product word with a shifted high window.
    {p+"_recombo",
     t+" "+p+"_recombo("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned x=h, y=h*2246822519u;\n"
     "    unsigned long long pr=(unsigned long long)x*y;\n"
     "    unsigned lo=(unsigned)pr, hi=(unsigned)(pr>>32);\n"
     "    unsigned r=lo ^ (hi*0x9e3779b9u) ^ (unsigned)(pr>>20);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress246", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress246TC("x64o246", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress246TC("x86o246", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress246TC("a64o246", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress246TC("armo246", "int");

INSTANTIATE_TEST_SUITE_P(OptStress246, X64OptStress246RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress246, X86OptStress246RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress246, A64OptStress246RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress246, ARM32OptStress246RT, ::testing::ValuesIn(kARM), rtTCName);
