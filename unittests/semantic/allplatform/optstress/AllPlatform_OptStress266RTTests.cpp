//===- AllPlatform_OptStress266RTTests.cpp - nested/multi switch -O0 =====//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Nested-loop and multi-switch dispatch at -O0 — direct hardening of the #509
// fix (ARM32 -O0 switch whose index shared a GPR with an inner loop counter, so
// a stale `counter < N` guard narrowed the table bound).  These pile more loop
// counters and switches onto the same register file at -O0 so several stale
// guards compete with the real index bound, across all four targets and several
// guard polarities (`<`, `<=`, `!=`) and table sizes.
//
//   * trisw   - triple-nested loops, innermost counter drives the index.
//   * sw2idx  - index combines two loop counters.
//   * twosw   - two switches in one loop body (independent indices).
//   * swle    - 9-way switch (h%9) -> inclusive-compare bound, nested in a loop.
//   * swinner - a case body contains its own loop (register reuse after dispatch).
//   * swdo    - do-while loop containing a switch.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress266RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress266RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress266RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress266RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress266RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress266RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress266RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress266RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress266TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  std::string sw7 =
     "      switch(v){ case 0:r=acc+1u;break; case 1:r=acc^h;break; case 2:r=acc*3u;break;\n"
     "        case 3:r=acc>>1;break; case 4:r=acc+h;break; case 5:r=~acc;break;\n"
     "        case 6:r=acc*5u;break; default:r=acc-1u;break; }\n";
  return {
    // Triple-nested loops, innermost counter drives the index.
    {p+"_trisw",
     t+" "+p+"_trisw("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<12;i++){ h=h*1103515245u+12345u;\n"
     "   for(int j=0;j<4;j++){\n"
     "    for(int k=0;k<3;k++){ unsigned v=(h>>(k*3+j))&7u; unsigned r;\n"
     +sw7+
     "      acc=acc*131u+r+(unsigned)(j*k); } } }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress266", 0},

    // Index combines two loop counters.
    {p+"_sw2idx",
     t+" "+p+"_sw2idx("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<20;i++){ h=h*1103515245u+12345u;\n"
     "   for(int j=0;j<6;j++){ unsigned v=((h>>j)+(unsigned)(j*2))&7u; unsigned r;\n"
     +sw7+
     "     acc=acc*131u+r; } }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress266", 0},

    // Two switches in one loop body (independent indices).
    {p+"_twosw",
     t+" "+p+"_twosw("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int j=0;j<160;j++){ h=h*1103515245u+12345u;\n"
     "    unsigned v=h&7u, w=(h>>8)&3u; unsigned r;\n"
     +sw7+
     "    unsigned r2; switch(w){ case 0:r2=h+2u;break; case 1:r2=h*7u;break;\n"
     "      case 2:r2=h>>4;break; default:r2=~h;break; }\n"
     "    acc=acc*131u + r + r2*31u; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress266", 0},

    // 9-way switch (h%9) -> inclusive-compare bound, nested in a loop.
    {p+"_swle",
     t+" "+p+"_swle("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int j=0;j<160;j++){ h=h*1103515245u+12345u; unsigned v=h%9u; unsigned r;\n"
     "    switch(v){ case 0:r=acc+1u;break; case 1:r=acc^h;break; case 2:r=acc*3u;break;\n"
     "      case 3:r=acc>>1;break; case 4:r=acc+h;break; case 5:r=~acc;break;\n"
     "      case 6:r=acc*5u;break; case 7:r=acc+0x77u;break; case 8:r=acc-h;break;\n"
     "      default:r=acc;break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress266", 0},

    // A case body contains its own loop (register reuse after dispatch).
    {p+"_swinner",
     t+" "+p+"_swinner("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<80;i++){ h=h*1103515245u+12345u; unsigned v=h&7u; unsigned r=acc;\n"
     "    switch(v){ case 0: for(int k=0;k<3;k++) r=r*2u+(unsigned)k; break;\n"
     "      case 1: r^=h; break; case 2: r=r*3u; break;\n"
     "      case 3: for(int k=0;k<2;k++) r=r+h+(unsigned)k; break;\n"
     "      case 4: r+=h; break; case 5: r=~r; break; case 6: r=r*5u; break;\n"
     "      default: r-=h; break; }\n"
     "    acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress266", 0},

    // do-while loop containing a switch.
    {p+"_swdo",
     t+" "+p+"_swdo("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; int n=0;\n"
     "  do { h=h*1103515245u+12345u; int j=0;\n"
     "    do { unsigned v=(h>>(j*2))&7u; unsigned r;\n"
     +sw7+
     "      acc=acc*131u+r; j++; } while(j<5);\n"
     "    n++; } while(n<40);\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress266", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress266TC("x64o266", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress266TC("x86o266", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress266TC("a64o266", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress266TC("armo266", "int");

INSTANTIATE_TEST_SUITE_P(OptStress266, X64OptStress266RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress266, X86OptStress266RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress266, A64OptStress266RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress266, ARM32OptStress266RT, ::testing::ValuesIn(kARM), rtTCName);
