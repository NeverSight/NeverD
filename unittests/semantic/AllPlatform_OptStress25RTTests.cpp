//===- AllPlatform_OptStress25RTTests.cpp - opt-stress probes --*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// OptStress 21-24 drove the partial-write -> wide-parent merge through the
// entry-seed (#430), wide-read-return (#431) and single-join diamond (#24)
// shapes.  This round pushes the same machinery into control-flow shapes those
// rounds never built, plus the sign/zero-extension and variable-shift corners
// the user keeps flagging:
//
//   * breakcarry - a loop-carried 16-bit value partially written inside an
//                  inner loop that `break`s, carrying the partial value out the
//                  back edge of the inner loop to a wide read in the outer body.
//   * switchhalf - a loop-carried 16-bit value written by every arm of a 4-way
//                  switch, read wide after the switch join (a 4-edge phi for the
//                  partial-written parent, not the 2-edge diamond of #24).
//   * deepnest   - two partial registers (16- and 8-bit) updated in a 3-level
//                  nested if, with one arm's 16-bit write reading the other 8-bit
//                  value, both read wide after every merge.
//   * gotomerge  - a 16-bit partial write reached through forward `goto`s into a
//                  shared block, stressing partial-parent merge under a CFG the
//                  structurer rebuilds from irreducible-looking edges.
//   * sextladder - signed narrow/widen ladder (char/short/int sign-extended back
//                  to the full width: SXTB/SXTH/SXTW / MOVSXD) over negatives.
//   * varshift   - data-dependent logical-left, logical-right and arithmetic-
//                  right shifts (the last over negatives) plus a full-width
//                  variable left shift, all with masked counts.
//
// Every kernel is integer-only, folds to a single integer return and lowers to
// no runtime helper (no 64-bit divide / no float), so all four targets are
// checked native vs lifted at -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress25RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress25RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress25RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress25RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress25RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress25RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress25RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress25RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress25TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 16-bit value partially written in an inner loop, carried out via break.
    {p+"_breakcarry",
     t+" "+p+"_breakcarry("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  unsigned short w=(unsigned short)a;\n"
     "  for(int i=0;i<40;i++){\n"
     "    for(int j=0;j<8;j++){ s=s*1103515245u+12345u;\n"
     "      w=(unsigned short)(w*3u+(unsigned short)(s>>9));\n"
     "      if((s>>28)&7u) break; }\n"
     "    h=h*131u+(unsigned)w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x53ULL}, "OptStress25", 2},

    // 16-bit value written by all four arms of a switch, read wide after join.
    {p+"_switchhalf",
     t+" "+p+"_switchhalf("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0; unsigned short w=(unsigned short)a;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    switch((s>>26)&3u){\n"
     "      case 0: w=(unsigned short)(w+(unsigned short)(s>>7)); break;\n"
     "      case 1: w=(unsigned short)(w*5u+1u); break;\n"
     "      case 2: w=(unsigned short)(w^(unsigned short)s); break;\n"
     "      default: w=(unsigned short)(w*3u); break; }\n"
     "    h=h*131u+(unsigned)w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xb1ULL}, "OptStress25", 2},

    // Two partial registers (16- and 8-bit) updated in a 3-level nested if.
    {p+"_deepnest",
     t+" "+p+"_deepnest("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  unsigned short w=(unsigned short)a; unsigned char c=(unsigned char)(a>>3);\n"
     "  for(int i=0;i<40;i++){ s=s*1103515245u+12345u;\n"
     "    if((s>>31)&1u){\n"
     "      if((s>>30)&1u){ c=(unsigned char)(c+(unsigned char)(s>>5)); }\n"
     "      else { w=(unsigned short)(w^(unsigned short)(s>>11)); }\n"
     "    } else {\n"
     "      if((s>>29)&1u){ w=(unsigned short)(w+(unsigned short)c); } }\n"
     "    h=h*131u+(unsigned)w+(unsigned)c; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x2fULL}, "OptStress25", 2},

    // 16-bit partial write reached through forward gotos into a shared block.
    {p+"_gotomerge",
     t+" "+p+"_gotomerge("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0; unsigned short w=(unsigned short)a;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    if((s>>30)&1u) goto upd2;\n"
     "    w=(unsigned short)(w+(unsigned short)(s>>8));\n"
     "    if((s>>29)&1u) goto done;\n"
     "  upd2:\n"
     "    w=(unsigned short)(w*3u+1u);\n"
     "  done:\n"
     "    h=h*131u+(unsigned)w; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0xc8ULL}, "OptStress25", 2},

    // Signed narrow/widen ladder: sign-extend char/short/int back to full width.
    {p+"_sextladder",
     t+" "+p+"_sextladder("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; "+t+" acc=0;\n"
     "  for(int i=0;i<48;i++){ s=s*1103515245u+12345u;\n"
     "    "+t+" v=("+t+")(int)s;\n"
     "    signed char b=(signed char)v;\n"
     "    short hh=(short)(v>>3);\n"
     "    int w=(int)(s*3u);\n"
     "    acc += ("+t+")b - ("+t+")hh + ("+t+")w;\n"
     "    acc ^= ("+t+")(b<0? -1: 0); }\n"
     "  return ("+t+")(acc*131); }\n",
     {0x74ULL}, "OptStress25", 2},

    // Data-dependent logical/arithmetic shifts with masked counts.
    {p+"_varshift",
     t+" "+p+"_varshift("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<60;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned n=(s>>2)&31u; unsigned u=s|1u; int si=(int)s;\n"
     "    unsigned lsl=u<<n; unsigned lsr=u>>n; int asr=si>>n;\n"
     "    "+t+" tsh=("+t+")a << (n & 15u);\n"
     "    h=h*131u+lsl+lsr+(unsigned)asr+(unsigned)tsh; }\n"
     "  return ("+t+")(unsigned)h; }\n",
     {0x1dULL}, "OptStress25", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress25TC("x64o25", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress25TC("x86o25", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress25TC("a64o25", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress25TC("armo25", "int");

INSTANTIATE_TEST_SUITE_P(OptStress25, X64OptStress25RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress25, X86OptStress25RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress25, A64OptStress25RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress25, ARM32OptStress25RT, ::testing::ValuesIn(kARM), rtTCName);
