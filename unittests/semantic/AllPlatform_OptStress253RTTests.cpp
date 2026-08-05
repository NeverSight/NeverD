//===- AllPlatform_OptStress253RTTests.cpp - constant div/mod (magic) ====//
//
// Signed/unsigned division and modulo by compile-time constants, which clang
// lowers to magic-multiply + shift sequences (no runtime divide, no libcall).
// The magic constants and the high-half multiply (mulhu/mulhs, umulh, smmul)
// flow through the value tracker / sub-register modeling differently from a
// plain `idiv`, and the quotient/remainder reconstruction stresses fold paths.
//
//   * divc    - unsigned x/k for several constants, summed.
//   * modc    - unsigned x%k for several constants.
//   * sdivc   - signed x/k including negative divisors.
//   * smodc   - signed x%k with negative dividends.
//   * digsum  - decimal decomposition via repeated /10 and %10.
//   * dmid    - (x/k)*k + x%k == x identity threaded through the accumulator.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.  Only 32-bit ops, so i386/ARM32 stay libcall-free
// (constant divide is always a multiply).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress253RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress253RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress253RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress253RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress253RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress253RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress253RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress253RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress253TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Unsigned division by several constants, summed (each is a magic multiply).
    {p+"_divc",
     t+" "+p+"_divc("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; unsigned x=h^0x9e3779b9u;\n"
     "    unsigned s=x/3u+x/5u+x/7u+x/9u+x/11u+x/13u+x/255u+x/65535u;\n"
     "    acc=acc*131u+s+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress253", 2},

    // Unsigned modulo by several constants.
    {p+"_modc",
     t+" "+p+"_modc("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; unsigned x=h+0x7f4a7c15u;\n"
     "    unsigned s=x%3u+x%7u+x%10u+x%100u+x%255u+x%1000u+x%4096u;\n"
     "    acc=acc*131u+s+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress253", 2},

    // Signed division by constants including negative divisors.
    {p+"_sdivc",
     t+" "+p+"_sdivc("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; int x=(int)h;\n"
     "    int s=x/3+x/7+x/(-3)+x/(-7)+x/100+x/1000;\n"
     "    acc=acc*131+s+i; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x34567u}, "OptStress253", 2},

    // Signed modulo with negative dividends (truncation toward zero).
    {p+"_smodc",
     t+" "+p+"_smodc("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; int x=(int)h;\n"
     "    int s=x%3+x%7+x%10+x%100+x%256+x%1000;\n"
     "    acc=acc*131+s+i; }\n"
     "  return ("+t+")(unsigned)acc; }\n",
     {0x45678u}, "OptStress253", 2},

    // Decimal decomposition: repeated /10 and %10 (digit sum + alternating).
    {p+"_digsum",
     t+" "+p+"_digsum("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u; unsigned x=h; unsigned dsum=0,alt=0; int sign=1;\n"
     "    for(int d=0;d<10&&x;d++){ unsigned dig=x%10u; x/=10u; dsum+=dig; alt+=(unsigned)(sign*(int)dig); sign=-sign; }\n"
     "    acc=acc*131u+dsum*7u+alt; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress253", 2},

    // (x/k)*k + x%k == x identity threaded through the accumulator.
    {p+"_dmid",
     t+" "+p+"_dmid("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; unsigned x=h^0xa5a5a5a5u;\n"
     "    unsigned k=((h>>3)&7u)*7u+3u;\n"  // k in {3,10,17,24,31,38,45,52}
     "    unsigned q=x/k, r=x%k; unsigned recon=q*k+r;\n"
     "    acc=acc*131u+(recon==x?q:0xdeadu)+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress253", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress253TC("x64o253", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress253TC("x86o253", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress253TC("a64o253", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress253TC("armo253", "int");

INSTANTIATE_TEST_SUITE_P(OptStress253, X64OptStress253RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress253, X86OptStress253RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress253, A64OptStress253RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress253, ARM32OptStress253RT, ::testing::ValuesIn(kARM), rtTCName);
