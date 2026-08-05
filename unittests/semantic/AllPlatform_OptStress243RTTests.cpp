//===- AllPlatform_OptStress243RTTests.cpp - fixed-point FP<->int convert =//
//
// Locks down the #506 AArch64 `fcvtzs/fcvtzu Wd, Dn, #fbits` source-width bug
// across both directions and widths: float vs double source, signed vs
// unsigned, several fraction-bit counts.  clang folds `(int)(d*2^n)` into
// `fcvtzs w,d,#n` and `(double)i/2^n` into `scvtf d,w,#n`; x86/ARM32 lower the
// same C to scalar cvt sequences.  Only 32-bit integer ends are used so i386
// and ARM32 avoid the 64-bit FP-conversion libcalls.
//
//   * f2i_d8  - (int)(double * 256).
//   * f2u_d10 - (unsigned)(double * 1024).
//   * f2i_s8  - (int)(float * 256)   [float source].
//   * i2f_d9  - (double)i / 512  then back to int.
//   * u2f_s6  - (float)(unsigned)i / 64 then back to int.
//   * mixfx   - both directions chained.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress243RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress243RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress243RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress243RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress243RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress243RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress243RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress243RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress243TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // (int)(double * 256)  -> fcvtzs w, d, #8
    {p+"_f2i_d8",
     t+" "+p+"_f2i_d8("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    double d=(double)(int)h/65536.0; int v=(int)(d*256.0);\n"
     "    acc=acc*131u+(unsigned)v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress243", 2},

    // (unsigned)(double * 1024)  -> fcvtzu w, d, #10
    {p+"_f2u_d10",
     t+" "+p+"_f2u_d10("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    double d=(double)(h>>4)/1048576.0; unsigned v=(unsigned)(d*1024.0);\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress243", 2},

    // (int)(float * 256)  -> fcvtzs w, s, #8  [float source]
    {p+"_f2i_s8",
     t+" "+p+"_f2i_s8("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    float f=(float)(int)h/65536.0f; int v=(int)(f*256.0f);\n"
     "    acc=acc*131u+(unsigned)v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress243", 2},

    // (double)i / 512  -> scvtf d, w, #9 ; then truncate back.
    {p+"_i2f_d9",
     t+" "+p+"_i2f_d9("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    double d=(double)(int)h/512.0; int v=(int)(d*3.0);\n"
     "    acc=acc*131u+(unsigned)v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress243", 2},

    // (float)(unsigned)i / 64  -> ucvtf s, w, #6 ; then truncate back.
    {p+"_u2f_s6",
     t+" "+p+"_u2f_s6("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    float f=(float)(h>>2)/64.0f; unsigned v=(unsigned)(f*5.0f);\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress243", 2},

    // Both directions chained through a fixed-point accumulator.
    {p+"_mixfx",
     t+" "+p+"_mixfx("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<120;i++){ h=h*1103515245u+12345u;\n"
     "    double d=(double)(int)(h>>3)/4096.0; d=d*1.25+0.5;\n"
     "    int q=(int)(d*16384.0); double e=(double)q/16384.0;\n"
     "    int v=(int)(e*100.0);\n"
     "    acc=acc*131u+(unsigned)v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress243", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress243TC("x64o243", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress243TC("x86o243", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress243TC("a64o243", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress243TC("armo243", "int");

INSTANTIATE_TEST_SUITE_P(OptStress243, X64OptStress243RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress243, X86OptStress243RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress243, A64OptStress243RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress243, ARM32OptStress243RT, ::testing::ValuesIn(kARM), rtTCName);
