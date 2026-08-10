//===- AllPlatform_OptStress275RTTests.cpp - sub-word width mixing -O0 ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// 8/16/32-bit width mixing at -O0 — the dual of the -O2 sub-register probe
// OptStress227.  At -O0 clang emits explicit movzbl/movswl/movb sub-register
// moves and narrow wrap-around arithmetic instead of folding everything to
// 32-bit, which stresses the lifter's SUBBYTES / INT_ZEXT / INT_SEXT modeling
// and narrow truncation on the un-cleaned form.
//
//   * w8163    - read the same word as u8/u16 and signed char/short.
//   * wrap     - narrow (u8 / u16) wrap-around running accumulators.
//   * sext     - sign-extension chains (i8->i16->i32 and back).
//   * bmerge   - byte extraction + merge into a word, half-word swap.
//   * mul16    - 16-bit multiply discarding the high half, low-byte slice.
//   * selbyte  - branch selects a byte / signed short into the accumulator.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit-or-narrower ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress275RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress275RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress275RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress275RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress275RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress275RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress275RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress275RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress275TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // read the same word as u8/u16 and signed char/short.
    {p+"_w8163",
     t+" "+p+"_w8163("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned char b=(unsigned char)h; unsigned short s=(unsigned short)(h>>8);\n"
     "    signed char sb=(signed char)h; short ss=(short)(h>>8);\n"
     "    acc=acc*131u + b + s + (unsigned)(int)sb + (unsigned)(int)ss; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress275", 0},

    // narrow (u8 / u16) wrap-around running accumulators.
    {p+"_wrap",
     t+" "+p+"_wrap("+t+" a){ unsigned h=(unsigned)a; unsigned char b=0; unsigned short s=0; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    b=(unsigned char)(b*31u + (h&0xffu)); s=(unsigned short)(s*131u + (h>>8));\n"
     "    acc=acc*131u + b + s; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress275", 0},

    // sign-extension chains (i8->i16->i32 and narrowing back).
    {p+"_sext",
     t+" "+p+"_sext("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    signed char c=(signed char)h; short s=(short)c; int w=(int)s;\n"
     "    short s2=(short)(h>>16); int w2=(int)s2; signed char c2=(signed char)s2;\n"
     "    acc=acc*131 + w + w2 + (int)c2; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress275", 0},

    // byte extraction + merge into a word, half-word swap.
    {p+"_bmerge",
     t+" "+p+"_bmerge("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned b0=h&0xffu,b1=(h>>8)&0xffu,b2=(h>>16)&0xffu,b3=(h>>24)&0xffu;\n"
     "    unsigned w=(b3<<24)|(b2<<16)|(b1<<8)|b0;\n"
     "    unsigned hw=((h&0xffffu)<<16)|((h>>16)&0xffffu);\n"
     "    acc=acc*131u + w + hw; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress275", 0},

    // 16-bit multiply discarding the high half, low-byte slice.
    {p+"_mul16",
     t+" "+p+"_mul16("+t+" a){ unsigned h=(unsigned)a; unsigned short acc=1; unsigned out=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned short x=(unsigned short)h; acc=(unsigned short)(acc*x + 1u);\n"
     "    unsigned char lo=(unsigned char)acc; out=out*131u + acc + lo; }\n"
     "  return ("+t+")out; }\n",
     {0x56789u}, "OptStress275", 0},

    // branch selects a byte / signed short into the accumulator.
    {p+"_selbyte",
     t+" "+p+"_selbyte("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned char pick; if(h&1u) pick=(unsigned char)(h>>3); else pick=(unsigned char)(h>>11);\n"
     "    short sx; if(h&2u) sx=(short)(h>>5); else sx=(short)-(int)(h>>13);\n"
     "    acc=acc*131u + pick + (unsigned)(int)sx; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress275", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress275TC("x64o275", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress275TC("x86o275", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress275TC("a64o275", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress275TC("armo275", "int");

INSTANTIATE_TEST_SUITE_P(OptStress275, X64OptStress275RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress275, X86OptStress275RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress275, A64OptStress275RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress275, ARM32OptStress275RT, ::testing::ValuesIn(kARM), rtTCName);
