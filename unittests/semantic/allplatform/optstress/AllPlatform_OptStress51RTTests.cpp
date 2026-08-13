//===- AllPlatform_OptStress51RTTests.cpp - rodata width × subreg -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Intersect the two high-yield areas — rodata constant-table loads (constant-
// pool redirect) and sub-register partial-write merges — by reading a `.rodata`
// table at mixed element widths (signed/unsigned i8/i16/i32) straight into
// narrow 8/16-bit accumulators that are then read at full width across blocks:
//
//   * sexttbl   - signed-char rodata table, sign-extended into a running sum
//                 (SEXT of a sub-word rodata load).
//   * subacc8   - rodata byte table value folded into an 8-bit accumulator each
//                 iteration, the parent read wide at loop exit (#430 reverse
//                 sub-register gate driven by a rodata load, not a stack byte).
//   * mixrw     - the SAME rodata table read as i32 and as overlapping i8/i16
//                 (mixed-width loads of one table).
//   * widenchain- i8 rodata -> i16 -> i32 widening accumulation chain.
//   * sboxsext  - signed sbox, sign-extended, drives a signed-compare select.
//   * halfacc   - rodata half (i16) table folded into a 16-bit accumulator,
//                 parent read wide (sub-half store aliasing a wider read).
//
// All integer, tables are `static const`, fold to one return, no float /
// 64-bit divide helper.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress51RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress51RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress51RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress51RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress51RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress51RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress51RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress51RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress51TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // signed-char rodata table, sign-extended into a running sum.
    {p+"_sexttbl",
     "static const signed char T[16]={-7,3,-11,2,13,-5,17,-9,"
     "19,-1,-23,8,-29,4,31,-6};\n"
     +t+" "+p+"_sexttbl("+t+" a){\n"
     "  unsigned s=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    acc += (int)T[(s>>5)&15u]; acc ^= acc<<3; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x61u}, "OptStress51", 2},

    // rodata byte value folded into an 8-bit accumulator, parent read wide.
    {p+"_subacc8",
     "static const unsigned char T[16]={0x11,0x83,0x2c,0x47,0xfe,0x05,0x9a,0x6b,"
     "0xc3,0x10,0x7d,0xa8,0x3f,0x52,0xee,0x21};\n"
     +t+" "+p+"_subacc8("+t+" a){\n"
     "  unsigned s=(unsigned)a; unsigned char acc=0; unsigned h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    acc=(unsigned char)(acc*31u+T[(s>>6)&15u]);\n"
     "    h=h*131u+(unsigned)acc; }\n"
     "  return ("+t+")h; }\n",
     {0x62u}, "OptStress51", 2},

    // The same rodata table read as i32 and as overlapping i8/i16.
    {p+"_mixrw",
     "static const unsigned T[8]={0x01020304u,0x05060708u,0x090a0b0cu,"
     "0x0d0e0f10u,0x11121314u,0x15161718u,0x191a1b1cu,0x1d1e1f20u};\n"
     +t+" "+p+"_mixrw("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  const unsigned char *b=(const unsigned char*)T;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned w=T[(s>>5)&7u]; unsigned char by=b[(s>>9)&31u];\n"
     "    h=h*131u+w+(unsigned)by; h^=h>>13; }\n"
     "  return ("+t+")h; }\n",
     {0x63u}, "OptStress51", 2},

    // i8 rodata -> i16 -> i32 widening accumulation chain.
    {p+"_widenchain",
     "static const signed char T[16]={5,-9,12,-3,7,-15,22,-1,"
     "-8,11,-20,6,-14,9,-2,18};\n"
     +t+" "+p+"_widenchain("+t+" a){\n"
     "  unsigned s=(unsigned)a; int h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    signed char c=T[(s>>5)&15u]; short w=(short)(c*100); int x=(int)w*7;\n"
     "    h=h*31+x; h^=h>>9; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x64u}, "OptStress51", 2},

    // signed sbox, sign-extended, drives a signed-compare select.
    {p+"_sboxsext",
     "static const signed char S[16]={-100,50,-25,75,-60,40,-90,30,"
     "-10,80,-45,20,-70,60,-5,15};\n"
     +t+" "+p+"_sboxsext("+t+" a){\n"
     "  unsigned s=(unsigned)a; int h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    int v=(int)S[(s>>5)&15u]; h += (v<0)?(-v):(v*2); h^=h<<2; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x65u}, "OptStress51", 2},

    // rodata half table folded into a 16-bit accumulator, parent read wide.
    {p+"_halfacc",
     "static const unsigned short T[16]={0x1234,0x8001,0x00ff,0xabcd,"
     "0x7fff,0x0010,0xfedc,0x5555,0xaaaa,0x0001,0xc0de,0x3333,"
     "0xface,0x0f0f,0x9999,0x4242};\n"
     +t+" "+p+"_halfacc("+t+" a){\n"
     "  unsigned s=(unsigned)a; unsigned short acc=0; unsigned h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    acc=(unsigned short)(acc*257u+T[(s>>5)&15u]);\n"
     "    h=h*131u+(unsigned)acc; }\n"
     "  return ("+t+")h; }\n",
     {0x66u}, "OptStress51", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress51TC("x64o51", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress51TC("x86o51", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress51TC("a64o51", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress51TC("armo51", "int");

INSTANTIATE_TEST_SUITE_P(OptStress51, X64OptStress51RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress51, X86OptStress51RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress51, A64OptStress51RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress51, ARM32OptStress51RT, ::testing::ValuesIn(kARM), rtTCName);
