//===- AllPlatform_OptStress339RTTests.cpp - non-adjacent two-table -----===//
//
// Runtime-selected two-table dispatch whose two label tables are NOT laid out
// back-to-back in rodata: `tbl = cond ? A : B; goto *tbl[idx]` where an
// alignment gap (or unrelated read-only data) separates A and B.  OptStress331
// covers the ADJACENT layout, which merges into one contiguous base and reads
// both halves with a single strided scan.  When the tables are non-adjacent
// that merge is impossible — the run count stops at the gap — so the dispatch
// previously degenerated to an indirect tail call to a garbage address.
//
// The resolver now reads each table's own code-pointer run independently and
// lays the two target sets out lower-table-first (positions [0,N) = the lower
// table, [N,2N) = the higher), which the emitter dispatches with the same
// `idx + (selectedArm ? N : 0)` switch as the adjacent form.  This is sound
// regardless of the gap: the runtime target is always one of the 2N recovered
// code pointers, whichever base the select produced.
//
//   * twosplit    - 8-way per table, B forced to a 128-byte boundary (-O2).
//   * twosplitneg - inverted condition (`cond ? B : A`), select/mask polarity.
//   * twosplit16  - 16-way per table (32 recovered targets), wider mask.
//   * twosplitos  - same 8-way shape at -Os (entry scale folded into index).
//
// Integer in / integer out, static label tables, LCG-seeded, folded single
// return; case bodies stay libcall-free on the 32-bit targets (no i64 div /
// variable shift).  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress339RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress339RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress339RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress339RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress339RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress339RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress339RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress339RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress339TC(const char *prefix,
                                                   const char *T) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // 8-way two-table computed goto with B aligned to a 128-byte boundary, so a
    // padding gap separates the two tables in rodata (non-adjacent runs).
    {p+"_twosplit",
     t+" "+p+"_twosplit("+t+" a){\n"
     "  static void *const A[]={&&a0,&&a1,&&a2,&&a3,&&a4,&&a5,&&a6,&&a7};\n"
     "  __attribute__((aligned(128))) static void *const B[]=\n"
     "      {&&b0,&&b1,&&b2,&&b3,&&b4,&&b5,&&b6,&&b7};\n"
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
     {0x1234u}, "OptStress339", 2, "-O2"},

    // Inverted condition (`cond ? B : A`): the higher table is the false arm, so
    // the synthesized selector must add the offset on the opposite polarity.
    {p+"_twosplitneg",
     t+" "+p+"_twosplitneg("+t+" a){\n"
     "  static void *const A[]={&&n0,&&n1,&&n2,&&n3,&&n4,&&n5,&&n6,&&n7};\n"
     "  __attribute__((aligned(128))) static void *const B[]=\n"
     "      {&&m0,&&m1,&&m2,&&m3,&&m4,&&m5,&&m6,&&m7};\n"
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
     {0x2345u}, "OptStress339", 2, "-O2"},

    // 16-way per table (32 recovered targets): a wider prescaled index mask over
    // two non-adjacent runs.
    {p+"_twosplit16",
     t+" "+p+"_twosplit16("+t+" a){\n"
     "  static void *const A[]={&&g0,&&g1,&&g2,&&g3,&&g4,&&g5,&&g6,&&g7,\n"
     "                          &&g8,&&g9,&&ga,&&gb,&&gc,&&gd,&&ge,&&gf};\n"
     "  __attribute__((aligned(256))) static void *const B[]=\n"
     "      {&&h0,&&h1,&&h2,&&h3,&&h4,&&h5,&&h6,&&h7,\n"
     "       &&h8,&&h9,&&ha,&&hb,&&hc,&&hd,&&he,&&hf};\n"
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
     {0x4567u}, "OptStress339", 2, "-O2"},

    // Same 8-way shape at -Os: the size optimizer folds the entry scale into the
    // index and frequently spills one table base across the select.
    {p+"_twosplitos",
     t+" "+p+"_twosplitos("+t+" a){\n"
     "  static void *const A[]={&&o0,&&o1,&&o2,&&o3,&&o4,&&o5,&&o6,&&o7};\n"
     "  __attribute__((aligned(128))) static void *const B[]=\n"
     "      {&&q0,&&q1,&&q2,&&q3,&&q4,&&q5,&&q6,&&q7};\n"
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
     {0x3456u}, "OptStress339", 2, "-Os"},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress339TC("x64o339", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress339TC("x86o339", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress339TC("a64o339", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress339TC("armo339", "int");

INSTANTIATE_TEST_SUITE_P(OptStress339, X64OptStress339RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress339, X86OptStress339RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress339, A64OptStress339RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress339, ARM32OptStress339RT, ::testing::ValuesIn(kARM), rtTCName);
