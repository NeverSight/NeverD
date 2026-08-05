//===- AllPlatform_OptStress337RTTests.cpp - two-table goto × i64 pair ---===//
//
// Extends #531's runtime-selected two-table computed-goto recovery
// (NdOpSwitchRecovery merges two adjacent rodata label tables A∪B and rebuilds
// `tbl = cond ? A : B; goto *tbl[idx]` as one switch over the merged byte-offset
// index) into a dimension OptStress331 never drove: a loop-carried i64
// accumulator THREADED through every case body of the merged 2N-target dispatch.
// This is the intersection of #531 (two-table merge) and #524-#528/#533 (i64
// register PAIRS on 32-bit targets — i386 EDX:EAX, ARM32 R1:R0 — with PHI/
// liveness across a multi-way branch + back edge).  Plus the first two-table
// coverage at -Oz (OptStress331 was -O2/-Os only).
//
//   * _twoll    : 8-way two-table, each of the 16 merged arms applies a distinct
//                 i64 op to the loop-carried acc (touching the high word).
//   * _twollhi  : both the table-select cond AND the dispatch index are derived
//                 from the loop-carried i64 acc's halves (cond from high half,
//                 index from low half) — both pair registers feed the dispatch.
//   * _twozz    : the _twoll shape at -Oz (more aggressive base spilling / entry-
//                 scale folding than -Os).
//   * _two16ll  : 16-way per table (32 merged targets) with the i64 acc threaded
//                 — a wide merge under i64-pair liveness.
//
// libcall-free on i386/ARM32: i64 add/sub/xor/and + CONSTANT shifts and
// i64×small-const only (no i64 divide, no variable i64 shift).  Power-of-two
// index masks.  Deterministic LCG seed; folds both i64 halves into one return so
// a dropped/duplicated high word surfaces.  x64/a64 (single 64-bit reg) are
// controls; i386/ARM32 (register pairs) are the targets.  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress337RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress337RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress337RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress337RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress337RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress337RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress337RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress337RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress337TC(const char *prefix,
                                                   const char *T,
                                                   bool InclHiSel = true) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // 8-way two-table, i64 acc threaded through all 16 merged arms.
    {p+"_twoll",
     t+" "+p+"_twoll("+t+" a){\n"
     "  static void *const A[]={&&a0,&&a1,&&a2,&&a3,&&a4,&&a5,&&a6,&&a7};\n"
     "  static void *const B[]={&&b0,&&b1,&&b2,&&b3,&&b4,&&b5,&&b6,&&b7};\n"
     "  unsigned long long acc=(unsigned long long)(unsigned)a|1ull;\n"
     "  unsigned w=(unsigned)a^0x9e3779b9u;\n"
     "  for(int i=0;i<150;i++){ w=w*1103515245u+12345u;\n"
     "    void *const *tb=(w&0x10000u)?A:B; goto *tb[(w>>5)&7];\n"
     "    a0: acc+=(unsigned long long)w<<11; goto c; a1: acc^=(unsigned long long)w;     goto c;\n"
     "    a2: acc-=(unsigned long long)w<<3;  goto c; a3: acc+=(unsigned long long)w*3u;  goto c;\n"
     "    a4: acc^=(unsigned long long)w<<32; goto c; a5: acc-=(unsigned long long)w<<5;  goto c;\n"
     "    a6: acc+=(unsigned long long)w<<40; goto c; a7: acc^=acc>>13;                   goto c;\n"
     "    b0: acc+=(unsigned long long)w;     goto c; b1: acc^=(unsigned long long)w<<2;  goto c;\n"
     "    b2: acc-=(unsigned long long)w<<7;  goto c; b3: acc+=(unsigned long long)w*5u;  goto c;\n"
     "    b4: acc^=(unsigned long long)(w&0xffffu)<<48; goto c; b5: acc-=(unsigned long long)w;goto c;\n"
     "    b6: acc+=(unsigned long long)w<<17; goto c; b7: acc^=(unsigned long long)w*7u;  goto c;\n"
     "    c: acc^=acc>>29; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x1234u}, "OptStress337", 2, "-O2"},

    // The _twoll shape at -Oz (aggressive base spilling / entry-scale folding).
    {p+"_twozz",
     t+" "+p+"_twozz("+t+" a){\n"
     "  static void *const A[]={&&z0,&&z1,&&z2,&&z3,&&z4,&&z5,&&z6,&&z7};\n"
     "  static void *const B[]={&&y0,&&y1,&&y2,&&y3,&&y4,&&y5,&&y6,&&y7};\n"
     "  unsigned long long acc=(unsigned long long)(unsigned)a|1ull;\n"
     "  unsigned w=(unsigned)a^0xa5a5a5a5u;\n"
     "  for(int i=0;i<120;i++){ w=w*22695477u+1u;\n"
     "    void *const *tb=(w&0x20000u)?A:B; goto *tb[(w>>6)&7];\n"
     "    z0: acc+=(unsigned long long)w<<8;  goto e; z1: acc^=(unsigned long long)w;     goto e;\n"
     "    z2: acc-=(unsigned long long)w<<4;  goto e; z3: acc+=(unsigned long long)w*3u;  goto e;\n"
     "    z4: acc^=(unsigned long long)w<<32; goto e; z5: acc-=(unsigned long long)w<<2;  goto e;\n"
     "    z6: acc+=(unsigned long long)w<<40; goto e; z7: acc^=acc>>11;                   goto e;\n"
     "    y0: acc+=(unsigned long long)w;     goto e; y1: acc^=(unsigned long long)w<<3;  goto e;\n"
     "    y2: acc-=(unsigned long long)w<<6;  goto e; y3: acc+=(unsigned long long)w*5u;  goto e;\n"
     "    y4: acc^=(unsigned long long)(w&0xffu)<<56; goto e; y5: acc-=(unsigned long long)w;goto e;\n"
     "    y6: acc+=(unsigned long long)w<<19; goto e; y7: acc^=(unsigned long long)w*7u;  goto e;\n"
     "    e: acc^=acc>>23; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x3456u}, "OptStress337", 2, "-Oz"},

    // 16-way per table (32 merged targets) with the i64 acc threaded.
    {p+"_two16ll",
     t+" "+p+"_two16ll("+t+" a){\n"
     "  static void *const A[]={&&g0,&&g1,&&g2,&&g3,&&g4,&&g5,&&g6,&&g7,\n"
     "                          &&g8,&&g9,&&ga,&&gb,&&gc,&&gd,&&ge,&&gf};\n"
     "  static void *const B[]={&&h0,&&h1,&&h2,&&h3,&&h4,&&h5,&&h6,&&h7,\n"
     "                          &&h8,&&h9,&&ha,&&hb,&&hc,&&hd,&&he,&&hf};\n"
     "  unsigned long long acc=(unsigned long long)(unsigned)a|7ull;\n"
     "  unsigned w=(unsigned)a^0xdeadbeefu;\n"
     "  for(int i=0;i<160;i++){ w=w*1664525u+1013904223u;\n"
     "    void *const *tb=(w&0x8000u)?A:B; goto *tb[(w>>10)&15];\n"
     "    g0:acc+=(unsigned long long)w;goto f;        g1:acc^=(unsigned long long)w<<1;goto f;\n"
     "    g2:acc-=(unsigned long long)w<<3;goto f;     g3:acc+=(unsigned long long)w*3u;goto f;\n"
     "    g4:acc^=(unsigned long long)w<<32;goto f;    g5:acc+=(unsigned long long)w<<5;goto f;\n"
     "    g6:acc-=(unsigned long long)w;goto f;        g7:acc^=acc>>7;goto f;\n"
     "    g8:acc+=(unsigned long long)w<<40;goto f;    g9:acc-=(unsigned long long)w<<2;goto f;\n"
     "    ga:acc^=(unsigned long long)w*5u;goto f;     gb:acc+=(unsigned long long)w<<13;goto f;\n"
     "    gc:acc^=(unsigned long long)w<<48;goto f;    gd:acc-=(unsigned long long)w<<4;goto f;\n"
     "    ge:acc+=(unsigned long long)w*9u;goto f;     gf:acc^=(unsigned long long)w<<21;goto f;\n"
     "    h0:acc-=(unsigned long long)w;goto f;        h1:acc+=(unsigned long long)w<<1;goto f;\n"
     "    h2:acc^=(unsigned long long)w<<6;goto f;     h3:acc-=(unsigned long long)w*3u;goto f;\n"
     "    h4:acc+=(unsigned long long)w<<34;goto f;    h5:acc^=(unsigned long long)w<<2;goto f;\n"
     "    h6:acc+=(unsigned long long)w;goto f;        h7:acc-=acc>>9;goto f;\n"
     "    h8:acc^=(unsigned long long)w<<41;goto f;    h9:acc+=(unsigned long long)w<<3;goto f;\n"
     "    ha:acc-=(unsigned long long)w*5u;goto f;     hb:acc^=(unsigned long long)w<<15;goto f;\n"
     "    hc:acc+=(unsigned long long)(w&0xffffu)<<48;goto f; hd:acc^=(unsigned long long)w<<5;goto f;\n"
     "    he:acc-=(unsigned long long)w*7u;goto f;     hf:acc+=(unsigned long long)w<<25;goto f;\n"
     "    f: acc^=acc>>27; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n",
     {0x4567u}, "OptStress337", 2, "-O2"},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress337TC("x64o337", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress337TC("x86o337", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress337TC("a64o337", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress337TC("armo337", "int");

INSTANTIATE_TEST_SUITE_P(OptStress337, X64OptStress337RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress337, X86OptStress337RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress337, A64OptStress337RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress337, ARM32OptStress337RT, ::testing::ValuesIn(kARM), rtTCName);
