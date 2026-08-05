//===- AllPlatform_OptStress327RTTests.cpp - pre-scaled computed goto ----===//
//
// Roots out the KNOWN-OPEN ① jump-table-recovery limitation flagged by
// OptStress326: the size/speed optimizer lowers a `goto *lab[idx]` whose label
// table holds pointer-width entries into an indirect jump whose index register
// already carries the BYTE OFFSET — the entry-size scale folded into a shift +
// contiguous-but-shifted mask (`shr idx,3; and idx,0x38; jmp *(table,idx)`,
// addressing scale 1).  The table address is materialised in a register by a
// rip-relative `lea`.  Without an INT_MULT/INT_LEFT scale op in the address the
// resolver could not anchor the table, so the dispatch degraded to a tail call
// and the case bodies were never recovered (recompiled `FETCH_UNMAPPED`).
//
// The fix recovers the table from the pre-scaled form, gated on a run of
// absolute code-pointer relocations at the folded base (the verifiable label-
// table signature), and records the entry-size as the index stride so the case
// labels become the byte offsets {0, size, 2*size, ...} the dispatch compares.
//
// x86-64 / i386 are the targets under test (SIB `(base,index,1)` addressing);
// aarch64 / arm32 use PC-relative dispatch and are controls.  Integer in / out,
// function-local rodata label table, LCG-seeded, folded single return; 32-bit
// targets stay libcall-free (no i64 div / variable shift).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress327RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress327RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress327RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress327RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress327RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress327RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress327RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress327RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress327TC(const char *prefix,
                                                   const char *T) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // 8-way computed goto, index `(w>>6)&7`.  At -Os clang folds the *8 scale
    // into `(w>>3)&0x38` and addresses `*(table,idx)` with scale 1 — the
    // pre-scaled form the fix recovers.
    {p+"_cg8os",
     t+" "+p+"_cg8os("+t+" a){\n"
     "  static const void *const lab[8]={&&L0,&&L1,&&L2,&&L3,&&L4,&&L5,&&L6,&&L7};\n"
     "  unsigned w=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<80;i++){ w=w*1103515245u+12345u;\n"
     "    goto *lab[(w>>6)&7];\n"
     "    L0: acc+=w; goto C; L1: acc-=w; goto C; L2: acc^=w; goto C;\n"
     "    L3: acc+=w*3u; goto C; L4: acc-=w*5u; goto C; L5: acc^=w<<2; goto C;\n"
     "    L6: acc+=w>>1; goto C; L7: acc-=w>>2; goto C;\n"
     "    C: acc^=acc>>7; }\n"
     "  return ("+t+")acc; }\n",
     {0x1357u}, "OptStress327", 2, "-Os"},

    // Same shape at -O2 (the form OptStress201 already covers for the scale-8
    // path; here the index expression makes clang choose the pre-scaled form).
    {p+"_cg8o2",
     t+" "+p+"_cg8o2("+t+" a){\n"
     "  static const void *const lab[8]={&&L0,&&L1,&&L2,&&L3,&&L4,&&L5,&&L6,&&L7};\n"
     "  unsigned w=(unsigned)a^0x55u, acc=0;\n"
     "  for(int i=0;i<80;i++){ w=w*1664525u+1013904223u;\n"
     "    goto *lab[(w>>9)&7];\n"
     "    L0: acc+=w; goto C; L1: acc^=w<<1; goto C; L2: acc-=w; goto C;\n"
     "    L3: acc+=w*7u; goto C; L4: acc^=w>>2; goto C; L5: acc-=w*3u; goto C;\n"
     "    L6: acc+=w&0xff; goto C; L7: acc^=w>>5; goto C;\n"
     "    C: acc=(acc>>3)|(acc<<29); }\n"
     "  return ("+t+")acc; }\n",
     {0x2468u}, "OptStress327", 2, "-O2"},

    // 8-way at -Oz: size optimizer most aggressively folds the scale into the
    // masked pre-scaled index.
    {p+"_cg8oz",
     t+" "+p+"_cg8oz("+t+" a){\n"
     "  static const void *const lab[8]={&&L0,&&L1,&&L2,&&L3,&&L4,&&L5,&&L6,&&L7};\n"
     "  unsigned w=(unsigned)a+0x9u, acc=(unsigned)a;\n"
     "  for(int i=0;i<72;i++){ w=w*22695477u+1u;\n"
     "    goto *lab[(w>>5)&7];\n"
     "    L0: acc+=w; goto C; L1: acc-=w; goto C; L2: acc^=w; goto C;\n"
     "    L3: acc=~acc; goto C; L4: acc+=w*5u; goto C; L5: acc^=w<<3; goto C;\n"
     "    L6: acc-=w>>1; goto C; L7: acc+=w&0x3f; goto C;\n"
     "    C: acc^=acc>>11; }\n"
     "  return ("+t+")acc; }\n",
     {0x369cu}, "OptStress327", 2, "-Oz"},

    // 16-way computed goto, index `(w>>4)&15` → pre-scaled `(w>>1)&0x78` (mask
    // 0x78 = (2^4-1)<<3), exercising a wider pre-scaled mask.
    {p+"_cg16",
     t+" "+p+"_cg16("+t+" a){\n"
     "  static const void *const lab[16]={&&M0,&&M1,&&M2,&&M3,&&M4,&&M5,&&M6,&&M7,\n"
     "                                    &&M8,&&M9,&&MA,&&MB,&&MC,&&MD,&&ME,&&MF};\n"
     "  unsigned w=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<96;i++){ w=w*1103515245u+12345u;\n"
     "    goto *lab[(w>>4)&15];\n"
     "    M0: acc+=w; goto D; M1: acc-=w; goto D; M2: acc^=w; goto D;\n"
     "    M3: acc+=w*3u; goto D; M4: acc-=w*5u; goto D; M5: acc^=w<<2; goto D;\n"
     "    M6: acc+=w>>1; goto D; M7: acc-=w>>2; goto D; M8: acc+=w*9u; goto D;\n"
     "    M9: acc^=w<<4; goto D; MA: acc-=w&0xff; goto D; MB: acc+=w^0x5a; goto D;\n"
     "    MC: acc^=w>>3; goto D; MD: acc-=w*7u; goto D; ME: acc+=i; goto D;\n"
     "    MF: acc=~acc; goto D;\n"
     "    D: acc^=acc>>13; }\n"
     "  return ("+t+")acc; }\n",
     {0x4812u}, "OptStress327", 2, "-O2"},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress327TC("x64o327", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress327TC("x86o327", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress327TC("a64o327", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress327TC("armo327", "int");

INSTANTIATE_TEST_SUITE_P(OptStress327, X64OptStress327RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress327, X86OptStress327RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress327, A64OptStress327RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress327, ARM32OptStress327RT, ::testing::ValuesIn(kARM), rtTCName);
