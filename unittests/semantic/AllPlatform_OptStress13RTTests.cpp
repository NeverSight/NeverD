//===- AllPlatform_OptStress13RTTests.cpp - opt-stress probes --*-C++*-=//
//
// Optimizer-stress roundtrip probes orthogonal to OptStress1-12 / SubRegMix,
// aimed at the NeverD hand-written MedIR passes (LowToMed sub-register SSA,
// MedFlags, MedPropagation) that have repeatedly hidden semantic bugs:
//
//   * predsum   - a sum of mixed signed/unsigned predicates (setcc/cset
//                 materialization + flag-polarity folding in one expression).
//   * subwmix   - mixed 8/16/32-bit signed+unsigned views of one 32-bit value
//                 recombined (sub-register aliasing: write wide, read narrow).
//   * divsign   - signed and unsigned 32-bit div+rem of the same operands
//                 (idiv/div + srem/urem, MULHI/RDX modeling, INT_MIN edge).
//   * cmovchain - a long data-dependent select chain reusing compare flags
//                 (cmov/csel cascades feeding the next compare).
//   * mulhi_sbb - high 32 bits of a 32x32->64 product folded into a 64-bit
//                 subtract-with-borrow accumulator (lane-crossing borrow).
//   * word16    - 16-bit add/sub/mul/rotate accumulation (x86 partial-register
//                 AX writes leaving the high half of EAX; AAPCS uxth/sxth).
//
// Every kernel is integer-only, folds to a single integer return and lowers to
// no runtime helper, so all four targets are checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress13RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress13RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress13RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress13RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress13RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress13RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress13RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress13RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress13TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Sum of mixed signed/unsigned predicates folded with the stream value.
    {p+"_predsum",
     t+" "+p+"_predsum("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u; int s=(int)x;\n"
     "    unsigned pr=(x<0x80000000u)+(unsigned)(s<0)+(x>0x1000u)\n"
     "      +(unsigned)(s> -100)+((x&0xffu)<=0x7fu)+(unsigned)(s>=0);\n"
     "    h=h*131u+pr*7u+x; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x4cULL}, "OptStress13", 2},

    // Mixed 8/16/32-bit signed+unsigned views of one value recombined.
    {p+"_subwmix",
     t+" "+p+"_subwmix("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; int h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u;\n"
     "    signed char b=(signed char)x; short s=(short)(x>>3);\n"
     "    unsigned char ub=(unsigned char)(x>>5);\n"
     "    unsigned short us=(unsigned short)(x>>11);\n"
     "    h=h*131+(int)b+(int)s-(int)ub+(int)us; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "OptStress13", 2},

    // Signed and unsigned 32-bit div+rem of the same operands.
    {p+"_divsign",
     t+" "+p+"_divsign("+t+" a){\n"
     "  int x=(int)a|1; unsigned h=0;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245+12345;\n"
     "    int d=((x>>3)|1); int q=x/d, r=x%d;\n"
     "    unsigned ud=((unsigned)d)|1u; unsigned uq=(unsigned)x/ud, ur=(unsigned)x%ud;\n"
     "    h=h*131u+(unsigned)q+(unsigned)r+uq+ur; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa7ULL}, "OptStress13", 2},

    // Long data-dependent select chain reusing compare flags.
    {p+"_cmovchain",
     t+" "+p+"_cmovchain("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; int v=0, h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u; int tt=(int)x;\n"
     "    v=(tt&1)?(v+tt):(v-tt);\n"
     "    v=(v<0)?-v:v;\n"
     "    v=(v>1000000)?(v>>1):v;\n"
     "    v=((tt>>8)&1)?(v^0x5a5a):v;\n"
     "    h=h*131+v; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x35ULL}, "OptStress13", 2},

    // High 32 bits of a 32x32->64 product folded into a 64-bit borrow chain.
    {p+"_mulhi_sbb",
     t+" "+p+"_mulhi_sbb("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, y=(unsigned)a^0x9e3779b9u, h=0;\n"
     "  unsigned long long acc=0xC000000000000000ull;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245u+12345u; y=y*2654435761u+1u;\n"
     "    unsigned long long pr=(unsigned long long)x*(unsigned long long)y;\n"
     "    acc-=((unsigned long long)(unsigned)(pr>>32)<<4)+(unsigned)pr;\n"
     "    h=h*131u+(unsigned)acc+(unsigned)(acc>>32); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress13", 2},

    // 16-bit add/sub/mul/rotate accumulation (partial-register width).
    {p+"_word16",
     t+" "+p+"_word16("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned short w=(unsigned short)a; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned short m=(unsigned short)(x>>7);\n"
     "    w=(unsigned short)(w+m);\n"
     "    w=(unsigned short)(w*(unsigned short)(m|1u));\n"
     "    w=(unsigned short)(w-(unsigned short)(x>>13));\n"
     "    unsigned r=((x>>4)&15u)|1u;\n"
     "    w=(unsigned short)((w<<r)|(w>>(16-r)));\n"
     "    h=h*131u+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "OptStress13", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress13TC("x64o13", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress13TC("x86o13", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress13TC("a64o13", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress13TC("armo13", "int");

INSTANTIATE_TEST_SUITE_P(OptStress13, X64OptStress13RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress13, X86OptStress13RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress13, A64OptStress13RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress13, ARM32OptStress13RT, ::testing::ValuesIn(kARM), rtTCName);
