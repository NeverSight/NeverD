//===- AllPlatform_OptStress14RTTests.cpp - opt-stress probes --*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Optimizer-stress roundtrip probes orthogonal to OptStress1-13 / SubRegMix,
// aimed at multi-word carry/borrow chains, SWAR byte lanes, signed-overflow
// saturation and funnel shifts -- idioms that exercise NeverD's hand-written
// carry/flag and sub-register MedIR passes harder than scalar arithmetic:
//
//   * add96     - a 96-bit (three-word) add-with-carry accumulator (carry
//                 threaded across two word boundaries; adc/sbb chains).
//   * swarbyte  - SWAR byte-lane add/average inside a 32-bit word (carry-blocked
//                 partial adds + masked recombination).
//   * satacc    - signed saturating accumulate via XOR overflow detection
//                 (acc^s)&(v^s) sign test feeding a clamp select.
//   * absneg    - abs / negate / signum with the INT_MIN edge (neg of INT_MIN).
//   * smulhi    - high 32 bits of a *signed* 32x32->64 product (imul EDX) folded
//                 with a low-half shift.
//   * funnel    - data-dependent funnel left/right (shld/shrd) over a register
//                 pair by a [1,31] count.
//
// Every kernel is integer-only, folds to a single integer return and lowers to
// no runtime helper, so all four targets are checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress14RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress14RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress14RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress14RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress14RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress14RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress14RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress14RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress14TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 96-bit add-with-carry accumulator (carry across two word boundaries).
    {p+"_add96",
     t+" "+p+"_add96("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned c0=0,c1=0,c2=0,h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u;\n"
     "    unsigned a0=x, a1=(x>>1)|1u, a2=x>>2;\n"
     "    unsigned long long s=(unsigned long long)c0+a0; c0=(unsigned)s;\n"
     "    unsigned cy=(unsigned)(s>>32);\n"
     "    s=(unsigned long long)c1+a1+cy; c1=(unsigned)s; cy=(unsigned)(s>>32);\n"
     "    c2=c2+a2+cy;\n"
     "    h=h*131u+c0+c1+c2; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x4cULL}, "OptStress14", 2},

    // SWAR byte-lane add (carry-blocked) and average inside a 32-bit word.
    {p+"_swarbyte",
     t+" "+p+"_swarbyte("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u; unsigned y=x^0x55555555u;\n"
     "    unsigned s=((x&0x7f7f7f7fu)+(y&0x7f7f7f7fu))^((x^y)&0x80808080u);\n"
     "    unsigned avg=(x&y)+(((x^y)>>1)&0x7f7f7f7fu);\n"
     "    h=h*131u+s+avg; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "OptStress14", 2},

    // Signed saturating accumulate via XOR overflow detection.
    {p+"_satacc",
     t+" "+p+"_satacc("+t+" a){\n"
     "  int x=(int)a|1, acc=0; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245+12345;\n"
     "    int v=x>>4; int s=acc+v;\n"
     "    if(((acc^s)&(v^s))<0) s=(v<0)?(-2147483647-1):2147483647;\n"
     "    acc=s; h=h*131u+(unsigned)acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa7ULL}, "OptStress14", 2},

    // abs / negate / signum with the INT_MIN edge.
    {p+"_absneg",
     t+" "+p+"_absneg("+t+" a){\n"
     "  int x=(int)a|1; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245+12345;\n"
     "    int v=x>>3; int av=(v<0)?-v:v; int nv=-v; int sg=(v>0)-(v<0);\n"
     "    h=h*131u+(unsigned)av+(unsigned)nv+(unsigned)sg; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x35ULL}, "OptStress14", 2},

    // High 32 bits of a signed 32x32->64 product folded with a low-half shift.
    {p+"_smulhi",
     t+" "+p+"_smulhi("+t+" a){\n"
     "  int x=(int)a|1, y=(int)(a^0x5bd1e995); unsigned h=0;\n"
     "  for(int i=0;i<40;i++){ x=x*1103515245+12345; y=y*16807+1;\n"
     "    long long pr=(long long)x*(long long)y;\n"
     "    int hi=(int)(pr>>32), lo=(int)pr;\n"
     "    h=h*131u+(unsigned)hi+(unsigned)(lo>>16); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress14", 2},

    // Data-dependent funnel left/right over a register pair by a [1,31] count.
    {p+"_funnel",
     t+" "+p+"_funnel("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u, y=(unsigned)(a^0xdeadbeefu), h=0;\n"
     "  for(int i=0;i<48;i++){ x=x*1103515245u+12345u; y=y*2654435761u+1u;\n"
     "    unsigned r=((x>>5)&31u)|1u;\n"
     "    unsigned f=(x<<r)|(y>>(32-r));\n"
     "    unsigned g=(y>>r)|(x<<(32-r));\n"
     "    h=h*131u+f+g; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "OptStress14", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress14TC("x64o14", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress14TC("x86o14", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress14TC("a64o14", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress14TC("armo14", "int");

INSTANTIATE_TEST_SUITE_P(OptStress14, X64OptStress14RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress14, X86OptStress14RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress14, A64OptStress14RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress14, ARM32OptStress14RT, ::testing::ValuesIn(kARM), rtTCName);
