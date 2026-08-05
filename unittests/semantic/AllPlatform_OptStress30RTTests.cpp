//===- AllPlatform_OptStress30RTTests.cpp - opt-stress probes --*-C++*-=//
//
// Follow-up to OptStress29's #442 (a jump-table case computing an 8-bit
// accumulator in a scratch register, transferred to the accumulator register by
// a full-width move across the loop latch, lost its update because the cross-
// block partial-write merge skipped loop blocks).  These probe sibling shapes of
// that class: a 16-bit scratch-transfer accumulator, one register written at
// different widths across cases, a memory-resident accumulator touched at mixed
// widths in a switch, an accumulator that must survive a call inside a case, a
// nested switch, and a switch picking a narrow return value.
//
//   * sw16scratch - 16-bit accumulator updated through scratch + transfer.
//   * swmixwidth  - same register written 8/16/32-bit across cases.
//   * swmemacc    - memory accumulator touched at mixed widths in a switch.
//   * swcallacc   - sub-register accumulator surviving a call inside a case.
//   * swnested    - nested switch updating sub-register accumulators.
//   * swretnarrow - switch selecting an 8/16-bit value summed in the loop.
//
// Integer-only, single integer return, no 64-bit divide; all four targets -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress30RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress30RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress30RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress30RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress30RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress30RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress30RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress30RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress30TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 16-bit accumulator updated through a switch (scratch + transfer analog).
    {p+"_sw16scratch",
     t+" "+p+"_sw16scratch("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned short acc=0; unsigned h=0;\n"
     "  for(int i=0;i<120;i++){ s=s*1103515245u+12345u;\n"
     "    switch(s&3u){\n"
     "      case 0: acc=(unsigned short)(acc+(s>>11)); break;\n"
     "      case 1: acc=(unsigned short)(acc-(s>>13)); break;\n"
     "      case 2: acc=(unsigned short)(acc^(s>>9)); break;\n"
     "      default: acc=(unsigned short)(acc*3u+1u); break; }\n"
     "    h=h*131u+acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x41ULL}, "OptStress30", 2},

    // One register region written at 8/16/32-bit across cases.
    {p+"_swmixwidth",
     t+" "+p+"_swmixwidth("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned acc=0; unsigned h=0;\n"
     "  for(int i=0;i<120;i++){ s=s*1103515245u+12345u;\n"
     "    switch(s&3u){\n"
     "      case 0: acc=(acc&0xffffff00u)|((acc+(s>>3))&0xffu); break;\n"
     "      case 1: acc=(acc&0xffff0000u)|((acc+(s>>5))&0xffffu); break;\n"
     "      case 2: acc=acc^(s>>1); break;\n"
     "      default: acc=(acc&0xffffff00u)|(((s>>7)^acc)&0xffu); break; }\n"
     "    h=h*131u+acc; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x53ULL}, "OptStress30", 2},

    // Memory-resident accumulator touched at mixed widths in a switch.
    {p+"_swmemacc",
     t+" "+p+"_swmemacc("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char buf[4]={0}; unsigned h=0;\n"
     "  for(int i=0;i<120;i++){ s=s*1103515245u+12345u;\n"
     "    switch(s&3u){\n"
     "      case 0: buf[0]=(unsigned char)(buf[0]+(s>>3)); break;\n"
     "      case 1: buf[1]=(unsigned char)(buf[1]^(s>>5)); break;\n"
     "      case 2: { unsigned short *p=(unsigned short*)buf; p[1]=(unsigned short)(p[1]+(s>>7)); } break;\n"
     "      default: buf[3]=(unsigned char)(buf[3]-(s>>9)); break; }\n"
     "    h=h*131u+buf[0]+buf[1]*7u+buf[2]*17u+buf[3]*131u; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x6dULL}, "OptStress30", 2},

    // Sub-register accumulator that must survive a call inside one case.
    {p+"_swcallacc",
     "static unsigned "+p+"_mix(unsigned x) __attribute__((noinline));\n"
     +t+" "+p+"_swcallacc("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char acc=0; unsigned h=0;\n"
     "  for(int i=0;i<100;i++){ s=s*1103515245u+12345u;\n"
     "    switch(s&3u){\n"
     "      case 0: acc=(unsigned char)(acc+(s>>3)); break;\n"
     "      case 1: acc=(unsigned char)(acc^"+p+"_mix(s)); break;\n"
     "      case 2: acc=(unsigned char)(acc-(s>>7)); break;\n"
     "      default: acc=(unsigned char)(acc+1); break; }\n"
     "    h=h*131u+acc; }\n"
     "  return ("+t+")(unsigned)h; }\n"
     "static unsigned "+p+"_mix(unsigned x){ x^=x>>15; x*=2654435761u; return x>>24; }\n",
     {0xb2ULL}, "OptStress30", 2},

    // Nested switch updating two sub-register accumulators.
    {p+"_swnested",
     t+" "+p+"_swnested("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char a8=0; unsigned short a16=0; unsigned h=0;\n"
     "  for(int i=0;i<100;i++){ s=s*1103515245u+12345u;\n"
     "    switch(s&3u){\n"
     "      case 0:\n"
     "        switch((s>>2)&1u){ case 0: a8=(unsigned char)(a8+(s>>5)); break; default: a16=(unsigned short)(a16+a8); break; }\n"
     "        break;\n"
     "      case 1: a16=(unsigned short)(a16^(s>>7)); break;\n"
     "      case 2: a8=(unsigned char)(a8-(s>>9)); break;\n"
     "      default: a8=(unsigned char)(a8^a16); break; }\n"
     "    h=h*131u+a8+a16*7u; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x18ULL}, "OptStress30", 2},

    // Switch selecting a narrow (8/16-bit) value summed across the loop.
    {p+"_swretnarrow",
     t+" "+p+"_swretnarrow("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<120;i++){ s=s*1103515245u+12345u; int v;\n"
     "    switch(s&3u){\n"
     "      case 0: v=(signed char)(s>>4); break;\n"
     "      case 1: v=(unsigned char)(s>>6); break;\n"
     "      case 2: v=(short)(s>>3); break;\n"
     "      default: v=(int)(unsigned short)(s>>8); break; }\n"
     "    h=h*131u+(unsigned)v; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x2fULL}, "OptStress30", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress30TC("x64o30", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress30TC("x86o30", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress30TC("a64o30", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress30TC("armo30", "int");

INSTANTIATE_TEST_SUITE_P(OptStress30, X64OptStress30RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress30, X86OptStress30RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress30, A64OptStress30RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress30, ARM32OptStress30RT, ::testing::ValuesIn(kARM), rtTCName);
