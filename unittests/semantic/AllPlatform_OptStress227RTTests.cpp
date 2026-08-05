//===- AllPlatform_OptStress227RTTests.cpp - sub-register width mixing ===//
//
// Breadth probes for partial/sub-register tracking: the SAME logical value is
// repeatedly read and written at 8/16/32-bit widths inside a loop, then read
// back widened.  This is the family that exposed the worst correctness bugs
// (#157f high-byte regs lost, #153 phi alloca clobber, #154 narrow-reg phi
// fallback) -- sub-word truncation, sign-extension and byte reassembly across
// straight-line and branched control flow.
//
//   * bytemix  - decompose into 4 bytes, recombine with an 8-bit wrap + 16-bit.
//   * swap16   - swap the two 16-bit halves and recombine.
//   * signext  - sign-extend an i8 and i16 slice and sum widened.
//   * bytebuild- shift a branch-selected byte into a 32-bit accumulator.
//   * u8loop   - inner loop mutating an 8-bit value (rotate + xor) that wraps.
//   * mix16    - 16-bit multiply/xor that discards the carry out of 16 bits.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress227RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress227RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress227RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress227RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress227RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress227RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress227RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress227RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress227TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Decompose into 4 bytes, recombine with an 8-bit wrap plus a 16-bit slice.
    {p+"_bytemix",
     t+" "+p+"_bytemix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned b0=h&0xffu, b1=(h>>8)&0xffu, b2=(h>>16)&0xffu, b3=(h>>24)&0xffu;\n"
     "    unsigned char r=(unsigned char)(b0+b1+b2+b3);\n"
     "    unsigned short s=(unsigned short)(h ^ (h>>16));\n"
     "    acc=acc*131u+(unsigned)r+(unsigned)s+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress227", 2},

    // Swap the two 16-bit halves and recombine.
    {p+"_swap16",
     t+" "+p+"_swap16("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned short lo=(unsigned short)h, hi=(unsigned short)(h>>16);\n"
     "    unsigned r=((unsigned)lo<<16)|(unsigned)hi;\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress227", 2},

    // Sign-extend an i8 and i16 slice and sum widened.
    {p+"_signext",
     t+" "+p+"_signext("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    signed char sb=(signed char)h; short sh=(short)(h>>8);\n"
     "    int v=(int)sb + (int)sh;\n"
     "    acc=acc*131u+(unsigned)v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress227", 2},

    // Shift a branch-selected byte into a 32-bit accumulator.
    {p+"_bytebuild",
     t+" "+p+"_bytebuild("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned char b;\n"
     "    if(h&1u) b=(unsigned char)(h>>3); else b=(unsigned char)(h>>11);\n"
     "    unsigned w=(acc<<8)|(unsigned)b;\n"
     "    acc=w*131u+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress227", 2},

    // Inner loop mutating an 8-bit value (rotate + xor) that wraps.
    {p+"_u8loop",
     t+" "+p+"_u8loop("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned char c=(unsigned char)h;\n"
     "    for(int j=0;j<7;j++){ c=(unsigned char)((c<<1)|(c>>7)); c^=(unsigned char)(h>>j); }\n"
     "    acc=acc*131u+(unsigned)c+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress227", 2},

    // 16-bit multiply/xor discarding the carry out of 16 bits.
    {p+"_mix16",
     t+" "+p+"_mix16("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned short x=(unsigned short)h, y=(unsigned short)(h>>13);\n"
     "    unsigned short s=(unsigned short)((unsigned)x*(unsigned)y + ((unsigned)x^(unsigned)y));\n"
     "    acc=acc*131u+(unsigned)s+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress227", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress227TC("x64o227", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress227TC("x86o227", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress227TC("a64o227", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress227TC("armo227", "int");

INSTANTIATE_TEST_SUITE_P(OptStress227, X64OptStress227RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress227, X86OptStress227RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress227, A64OptStress227RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress227, ARM32OptStress227RT, ::testing::ValuesIn(kARM), rtTCName);
