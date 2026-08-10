//===- AllPlatform_OptStress286RTTests.cpp - shift/rotate/funnel probe ====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// -O2 integer kernels stressing the shift, rotate and funnel-shift codegen
// paths: constant + variable rotates (ROL/ROR, EXTR #imm), funnel/double shift
// (SHLD/SHRD on x86, EXTR on AArch64, shift+or on ARM32), byte pack/rotate via
// shift+mask, shift-add strength reduction (LEA / add lsl), arithmetic-vs-
// logical shift mixing, and data-dependent variable shifts.
//
//   * rotmix   - constant + variable rotate-left/right mixing (ROL/ROR).
//   * funnel   - funnel shift across two words (SHLD/SHRD / EXTR).
//   * shpack   - byte pack/unpack + byte rotate via shift+mask.
//   * shadd    - shift-add scaled chains (LEA / add lsl strength reduction).
//   * ashrmix  - arithmetic vs logical vs left shift on the same value.
//   * shctz    - data-dependent double variable shifts (all masked 0..31).
//
// Every shift amount is masked to a valid range so the kernels are UB-free and
// the native and lifted builds agree bit-for-bit.  No division.  All four, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress286RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress286RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress286RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress286RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress286RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress286RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress286RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress286RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress286TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // constant + variable rotate-left/right mixing (ROL/ROR).
    {p+"_rotmix",
     t+" "+p+"_rotmix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0x811C9DC5u;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned r1=(h<<13)|(h>>19);\n"
     "    unsigned s=(h>>5)&31u; unsigned r2=(h<<s)|(h>>((32u-s)&31u));\n"
     "    unsigned r3=(acc>>7)|(acc<<25);\n"
     "    acc=(acc^r1)+r2+r3+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress286", 2},

    // funnel shift across two words (SHLD/SHRD / EXTR).
    {p+"_funnel",
     t+" "+p+"_funnel("+t+" a){ unsigned h=(unsigned)a; unsigned lo=0x1234u, hi=0x5678u; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned n=(h>>3)&31u;\n"
     "    unsigned f=(hi<<n)|(lo>>((32u-n)&31u));\n"
     "    lo=hi; hi=h; acc=acc*131u+f+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress286", 2},

    // byte pack/unpack + byte rotate via shift+mask.
    {p+"_shpack",
     t+" "+p+"_shpack("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned b0=h&0xFFu,b1=(h>>8)&0xFFu,b2=(h>>16)&0xFFu,b3=(h>>24)&0xFFu;\n"
     "    unsigned packed=(b3<<24)|(b2<<16)|(b1<<8)|b0;\n"
     "    unsigned rot=(packed<<8)|(packed>>24);\n"
     "    acc=acc*131u+(rot^(b0+b1+b2+b3))+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress286", 2},

    // shift-add scaled chains (LEA / add lsl strength reduction).
    {p+"_shadd",
     t+" "+p+"_shadd("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned tt=h; tt=(tt<<1)+tt; tt=(tt<<2)+h; tt=(tt<<3)-(h>>1);\n"
     "    acc=acc+tt+((acc<<2)+(acc<<1))+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress286", 2},

    // arithmetic vs logical vs left shift on the same value.
    {p+"_ashrmix",
     t+" "+p+"_ashrmix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<100;i++){ h=h*1103515245u+12345u;\n"
     "    int sv=(int)h; unsigned uv=h; unsigned s=((h>>6)&15u)+1u;\n"
     "    unsigned ar=(unsigned)(sv>>s); unsigned lr=uv>>s; unsigned ll=uv<<s;\n"
     "    acc=acc*131u+(ar^lr^ll)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress286", 2},

    // data-dependent double variable shifts (all masked 0..31).
    {p+"_shctz",
     t+" "+p+"_shctz("+t+" a){ unsigned h=(unsigned)a|0x10001u; unsigned acc=0;\n"
     "  for(int i=0;i<92;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned s1=h&31u, s2=(h>>10)&31u;\n"
     "    unsigned v=(h<<s1)>>s2; unsigned w=(h>>s2)<<s1;\n"
     "    acc=acc*131u+(v^w)+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress286", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress286TC("x64o286", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress286TC("x86o286", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress286TC("a64o286", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress286TC("armo286", "int");

INSTANTIATE_TEST_SUITE_P(OptStress286, X64OptStress286RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress286, X86OptStress286RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress286, A64OptStress286RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress286, ARM32OptStress286RT, ::testing::ValuesIn(kARM), rtTCName);
