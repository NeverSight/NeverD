//===- AllPlatform_OptStress22RTTests.cpp - opt-stress probes --*-C++*-=//
//
// OptStress 1-21 hammered straight-line and single-loop register/flag/stack
// patterns.  This round targets the two corners the NeverD hand-written MedIR
// passes have repeatedly mishandled but that earlier probes only grazed:
//
//   * nestw16   - a 16-bit accumulator carried across a *nested* inner loop and
//                 read wide at the outer level.  The loop-carried narrow phi must
//                 survive the inner back-edge and the outer wide read must observe
//                 {stale-upper | zext(phi)} (mergeLoopCarriedPartialReads in a
//                 nested setting, not just the single-loop popcount idiom).
//   * nestb8    - the 8-bit version of nestw16 (DIL/CL-style byte carried across
//                 a nested loop).
//   * carry3    - an explicit 96-bit add/sub carry+borrow chain (three 32-bit
//                 limbs).  Carry/borrow flags materialized and re-consumed across
//                 blocks (ADC/SBB / adds-adc lane crossing).
//   * flagreuse - one comparison consumed three ways (branch, select, and a
//                 second materialization) so MedFlags must not fold the polarity
//                 or drop a re-use.
//   * bytelane  - four signed-char lanes pulled from one 32-bit word, mixed and
//                 repacked entirely in registers (sub-byte sign extension without
//                 memory, defeats the OptStress21 stack-resident variant).
//   * funnel    - data-dependent funnel shifts ((x<<n)|(y>>(32-n)), n in 1..31)
//                 that clang lowers to SHLD/SHRD on x86 and ext/orr-shift on ARM.
//
// Every kernel is integer-only, folds to a single integer return and lowers to
// no runtime helper (no 64-bit divide / no float), so all four targets are
// checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress22RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress22RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress22RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress22RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress22RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress22RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress22RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress22RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress22TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 16-bit accumulator carried across a nested inner loop, read wide outside.
    {p+"_nestw16",
     t+" "+p+"_nestw16("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  unsigned short w=(unsigned short)(a^0x1357u);\n"
     "  for(int i=0;i<14;i++){\n"
     "    for(int j=0;j<7;j++){ s=s*1103515245u+12345u;\n"
     "      w=(unsigned short)(w*3u+(unsigned short)(s>>9)); }\n"
     "    h=h*131u+(unsigned)w; w=(unsigned short)(w^(unsigned short)i); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x4cULL}, "OptStress22", 2},

    // 8-bit accumulator carried across a nested inner loop, read wide outside.
    {p+"_nestb8",
     t+" "+p+"_nestb8("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  unsigned char c=(unsigned char)(a^0x5au);\n"
     "  for(int i=0;i<16;i++){\n"
     "    for(int j=0;j<5;j++){ s=s*1103515245u+12345u;\n"
     "      c=(unsigned char)(c*5u+(unsigned char)(s>>11)); }\n"
     "    h=h*131u+(unsigned)c; c=(unsigned char)(c+(unsigned char)i); }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x9bULL}, "OptStress22", 2},

    // Explicit 96-bit add/sub carry+borrow chain across three 32-bit limbs.
    {p+"_carry3",
     t+" "+p+"_carry3("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u;\n"
     "  unsigned l0=s, l1=s^0x9e3779b9u, l2=0x1234u; unsigned h=0;\n"
     "  for(int i=0;i<50;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned a0=s, a1=s*2654435761u, a2=(s>>3);\n"
     "    unsigned long long t0=(unsigned long long)l0+a0; l0=(unsigned)t0;\n"
     "    unsigned c1=(unsigned)(t0>>32);\n"
     "    unsigned long long t1=(unsigned long long)l1+a1+c1; l1=(unsigned)t1;\n"
     "    l2=l2+a2+(unsigned)(t1>>32);\n"
     "    unsigned long long d0=(unsigned long long)l0-(unsigned)(s>>1); l0=(unsigned)d0;\n"
     "    unsigned bo=(unsigned)((d0>>32)&1u);\n"
     "    l1=l1-(unsigned)(s>>2)-bo;\n"
     "    h=h*131u+l0+l1+l2; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xa7ULL}, "OptStress22", 2},

    // One comparison consumed by a branch, a select, and a re-materialization.
    {p+"_flagreuse",
     t+" "+p+"_flagreuse("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; int acc=0; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    int x=(int)s, y=(int)(s*2654435761u);\n"
     "    int c=x<y;\n"
     "    if(c) acc+=x; else acc-=y;\n"
     "    acc += c ? (x^y) : (x&y);\n"
     "    acc = (x<y) ? acc+3 : acc-2;\n"
     "    h=h*131u+(unsigned)acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x35ULL}, "OptStress22", 2},

    // Four signed-char lanes pulled from one word, mixed and repacked in regs.
    {p+"_bytelane",
     t+" "+p+"_bytelane("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0; unsigned w=s;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u; w^=s;\n"
     "    signed char b0=(signed char)w, b1=(signed char)(w>>8);\n"
     "    signed char b2=(signed char)(w>>16), b3=(signed char)(w>>24);\n"
     "    int m=(int)b0-(int)b1+(int)b2-(int)b3;\n"
     "    unsigned char r0=(unsigned char)(b0+b3), r1=(unsigned char)(b1-b2);\n"
     "    w=((unsigned)r1<<24)|((unsigned)r0<<16)\n"
     "      |((unsigned)(unsigned char)b2<<8)|(unsigned char)b3;\n"
     "    h=h*131u+(unsigned)m+w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress22", 2},

    // Data-dependent funnel shifts (SHLD/SHRD on x86), count in 1..31.
    {p+"_funnel",
     t+" "+p+"_funnel("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned x=s, y=s^0xdeadbeefu; unsigned h=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned n=((s>>5)&31u)|1u;\n"
     "    unsigned f=(x<<n)|(y>>(32-n));\n"
     "    unsigned g=(y>>n)|(x<<(32-n));\n"
     "    x=f^(s>>1); y=g+(s<<1);\n"
     "    h=h*131u+f+g; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x13ULL}, "OptStress22", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress22TC("x64o22", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress22TC("x86o22", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress22TC("a64o22", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress22TC("armo22", "int");

INSTANTIATE_TEST_SUITE_P(OptStress22, X64OptStress22RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress22, X86OptStress22RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress22, A64OptStress22RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress22, ARM32OptStress22RT, ::testing::ValuesIn(kARM), rtTCName);
