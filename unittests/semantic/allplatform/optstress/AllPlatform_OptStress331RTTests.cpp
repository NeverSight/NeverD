//===- AllPlatform_OptStress331RTTests.cpp - runtime-selected table base ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Two-table indirect dispatch: a computed goto whose label table is chosen at
// runtime — `tbl = cond ? A : B; goto *tbl[idx]`.  clang lowers the base select
// to a CMOV / CSEL / conditional-MOV between two adjacent code-pointer tables in
// rodata, then loads the target from `selected_base[idx]`.  The resolver merges
// the two adjacent tables into one and the emitter rebuilds the runtime base
// select as a single switch over the merged byte-offset index, so neither the
// dispatch nor any case body is lost (previously the whole function degenerated
// to an indirect tail call to a garbage address).
//
//   * twotbl    - 8-way per table, cond on a bit of the LCG state (-O2).
//   * twotblneg - condition inverted (`cond ? B : A`) — exercises the select /
//                 mask polarity (the higher table is the false / ~M arm).
//   * twotblos  - same shape at -Os (size optimizer folds the entry scale into
//                 the index and routinely spills one table base).
//   * twotbl16  - 16-way per table (32 merged targets), wider prescaled mask.
//   * twotbloff - the index carries an in-table offset (`(idx+3)&7`), checking
//                 the merge does not retract through the mask.
//
// Integer in / integer out, stack-local, LCG-seeded, folded single return; the
// case bodies stay libcall-free on the 32-bit targets (no i64 div / var shift).
// All four targets, mixed -O2 / -Os.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress331RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress331RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress331RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress331RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress331RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress331RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress331RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress331RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress331TC(const char *prefix,
                                                   const char *T) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // 8-way two-table computed goto: the label table is selected at runtime.
    {p+"_twotbl",
     t+" "+p+"_twotbl("+t+" a){\n"
     "  static void *const A[]={&&a0,&&a1,&&a2,&&a3,&&a4,&&a5,&&a6,&&a7};\n"
     "  static void *const B[]={&&b0,&&b1,&&b2,&&b3,&&b4,&&b5,&&b6,&&b7};\n"
     "  unsigned w=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<150;i++){ w=w*1103515245u+12345u;\n"
     "    void *const *tb=(w&0x10000u)?A:B; goto *tb[(w>>5)&7];\n"
     "    a0: acc+=w;    goto c; a1: acc^=w<<1; goto c;\n"
     "    a2: acc-=w>>1; goto c; a3: acc+=w*3u; goto c;\n"
     "    a4: acc^=w>>3; goto c; a5: acc+=w<<2; goto c;\n"
     "    a6: acc-=w;    goto c; a7: acc^=w;    goto c;\n"
     "    b0: acc+=w>>2; goto c; b1: acc-=w<<1; goto c;\n"
     "    b2: acc^=w*5u; goto c; b3: acc+=w>>4; goto c;\n"
     "    b4: acc^=w<<3; goto c; b5: acc-=w>>5; goto c;\n"
     "    b6: acc+=w*7u; goto c; b7: acc^=w>>6; goto c;\n"
     "    c: acc^=acc>>5; }\n"
     "  return ("+t+")acc; }\n",
     {0x1234u}, "OptStress331", 2, "-O2"},

    // Inverted condition (`cond ? B : A`): the higher table is the false arm, so
    // the synthesized selector must add the offset on the opposite polarity.
    {p+"_twotblneg",
     t+" "+p+"_twotblneg("+t+" a){\n"
     "  static void *const A[]={&&n0,&&n1,&&n2,&&n3,&&n4,&&n5,&&n6,&&n7};\n"
     "  static void *const B[]={&&m0,&&m1,&&m2,&&m3,&&m4,&&m5,&&m6,&&m7};\n"
     "  unsigned w=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<150;i++){ w=w*1103515245u+12345u;\n"
     "    void *const *tb=(w&0x40000u)?B:A; goto *tb[(w>>9)&7];\n"
     "    n0: acc+=w;    goto d; n1: acc^=w<<2; goto d;\n"
     "    n2: acc-=w>>2; goto d; n3: acc+=w*5u; goto d;\n"
     "    n4: acc^=w>>1; goto d; n5: acc+=w<<1; goto d;\n"
     "    n6: acc-=w*3u; goto d; n7: acc^=w>>7; goto d;\n"
     "    m0: acc+=w>>3; goto d; m1: acc-=w<<3; goto d;\n"
     "    m2: acc^=w*9u; goto d; m3: acc+=w>>6; goto d;\n"
     "    m4: acc^=w<<4; goto d; m5: acc-=w>>4; goto d;\n"
     "    m6: acc+=w*11u;goto d; m7: acc^=w>>8; goto d;\n"
     "    d: acc^=acc>>7; }\n"
     "  return ("+t+")acc; }\n",
     {0x2345u}, "OptStress331", 2, "-O2"},

    // Size optimizer: the entry scale is folded into the index and one table
    // base is frequently spilled to the stack across the select.
    {p+"_twotblos",
     t+" "+p+"_twotblos("+t+" a){\n"
     "  static void *const A[]={&&o0,&&o1,&&o2,&&o3,&&o4,&&o5,&&o6,&&o7};\n"
     "  static void *const B[]={&&q0,&&q1,&&q2,&&q3,&&q4,&&q5,&&q6,&&q7};\n"
     "  unsigned w=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<120;i++){ w=w*22695477u+1u;\n"
     "    void *const *tb=(w&0x20000u)?A:B; goto *tb[(w>>6)&7];\n"
     "    o0: acc+=w;    goto e; o1: acc^=w<<1; goto e;\n"
     "    o2: acc-=w>>1; goto e; o3: acc+=w*3u; goto e;\n"
     "    o4: acc^=w>>3; goto e; o5: acc+=w<<2; goto e;\n"
     "    o6: acc-=w;    goto e; o7: acc^=w;    goto e;\n"
     "    q0: acc+=w>>2; goto e; q1: acc-=w<<1; goto e;\n"
     "    q2: acc^=w*5u; goto e; q3: acc+=w>>4; goto e;\n"
     "    q4: acc^=w<<3; goto e; q5: acc-=w>>5; goto e;\n"
     "    q6: acc+=w*7u; goto e; q7: acc^=w>>6; goto e;\n"
     "    e: acc^=acc>>5; }\n"
     "  return ("+t+")acc; }\n",
     {0x3456u}, "OptStress331", 2, "-Os"},

    // 16-way per table (32 merged targets): a wider prescaled index mask.
    {p+"_twotbl16",
     t+" "+p+"_twotbl16("+t+" a){\n"
     "  static void *const A[]={&&g0,&&g1,&&g2,&&g3,&&g4,&&g5,&&g6,&&g7,\n"
     "                          &&g8,&&g9,&&ga,&&gb,&&gc,&&gd,&&ge,&&gf};\n"
     "  static void *const B[]={&&h0,&&h1,&&h2,&&h3,&&h4,&&h5,&&h6,&&h7,\n"
     "                          &&h8,&&h9,&&ha,&&hb,&&hc,&&hd,&&he,&&hf};\n"
     "  unsigned w=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<160;i++){ w=w*1664525u+1013904223u;\n"
     "    void *const *tb=(w&0x8000u)?A:B; goto *tb[(w>>10)&15];\n"
     "    g0:acc+=w;goto f;     g1:acc^=w<<1;goto f;  g2:acc-=w>>1;goto f;\n"
     "    g3:acc+=w*3u;goto f;  g4:acc^=w>>3;goto f;  g5:acc+=w<<2;goto f;\n"
     "    g6:acc-=w;goto f;     g7:acc^=w;goto f;     g8:acc+=w>>2;goto f;\n"
     "    g9:acc-=w<<1;goto f;  ga:acc^=w*5u;goto f;  gb:acc+=w>>4;goto f;\n"
     "    gc:acc^=w<<3;goto f;  gd:acc-=w>>5;goto f;  ge:acc+=w*7u;goto f;\n"
     "    gf:acc^=w>>6;goto f;\n"
     "    h0:acc-=w;goto f;     h1:acc+=w<<1;goto f;  h2:acc^=w>>1;goto f;\n"
     "    h3:acc-=w*3u;goto f;  h4:acc+=w>>3;goto f;  h5:acc^=w<<2;goto f;\n"
     "    h6:acc+=w;goto f;     h7:acc-=w;goto f;     h8:acc^=w>>2;goto f;\n"
     "    h9:acc+=w<<1;goto f;  ha:acc-=w*5u;goto f;  hb:acc^=w>>4;goto f;\n"
     "    hc:acc+=w<<3;goto f;  hd:acc^=w>>5;goto f;  he:acc-=w*7u;goto f;\n"
     "    hf:acc+=w>>6;goto f;\n"
     "    f: acc^=acc>>9; }\n"
     "  return ("+t+")acc; }\n",
     {0x4567u}, "OptStress331", 2, "-O2"},

    // The index carries an in-table offset so the recovery must not retract
    // through the mask (the merged switch dispatches on the masked byte offset).
    {p+"_twotbloff",
     t+" "+p+"_twotbloff("+t+" a){\n"
     "  static void *const A[]={&&s0,&&s1,&&s2,&&s3,&&s4,&&s5,&&s6,&&s7};\n"
     "  static void *const B[]={&&u0,&&u1,&&u2,&&u3,&&u4,&&u5,&&u6,&&u7};\n"
     "  unsigned w=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<150;i++){ w=w*214013u+2531011u;\n"
     "    void *const *tb=(w&0x4000u)?A:B; goto *tb[((w>>7)+3)&7];\n"
     "    s0: acc+=w;    goto g; s1: acc^=w<<1; goto g;\n"
     "    s2: acc-=w>>1; goto g; s3: acc+=w*3u; goto g;\n"
     "    s4: acc^=w>>3; goto g; s5: acc+=w<<2; goto g;\n"
     "    s6: acc-=w;    goto g; s7: acc^=w;    goto g;\n"
     "    u0: acc+=w>>2; goto g; u1: acc-=w<<1; goto g;\n"
     "    u2: acc^=w*5u; goto g; u3: acc+=w>>4; goto g;\n"
     "    u4: acc^=w<<3; goto g; u5: acc-=w>>5; goto g;\n"
     "    u6: acc+=w*7u; goto g; u7: acc^=w>>6; goto g;\n"
     "    g: acc^=acc>>5; }\n"
     "  return ("+t+")acc; }\n",
     {0x5678u}, "OptStress331", 2, "-O2"},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress331TC("x64o331", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress331TC("x86o331", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress331TC("a64o331", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress331TC("armo331", "int");

INSTANTIATE_TEST_SUITE_P(OptStress331, X64OptStress331RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress331, X86OptStress331RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress331, A64OptStress331RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress331, ARM32OptStress331RT, ::testing::ValuesIn(kARM), rtTCName);
