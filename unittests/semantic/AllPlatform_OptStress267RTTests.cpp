//===- AllPlatform_OptStress267RTTests.cpp - goto / irreducible CFG -O0 ==//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// goto-based and irreducible-ish control flow at -O0 — stresses CFG recovery
// where clang emits explicit forward/backward branches with shared tails and
// loop back-edges that don't come from structured loops, the form most likely
// to trip block-boundary / loop-header detection at -O0.
//
//   * gotoloop  - a loop built from a label + backward goto.
//   * gotostate - a goto-threaded state machine (switch-free dispatch).
//   * gototail  - several paths converging on one shared goto tail.
//   * gotobreak - forward gotos breaking out of nested loops.
//   * gotoreent - a back-edge re-entering the loop body below its head.
//   * gotomix   - goto dispatch combined with a switch.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress267RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress267RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress267RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress267RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress267RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress267RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress267RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress267RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress267TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // A loop built from a label + backward goto.
    {p+"_gotoloop",
     t+" "+p+"_gotoloop("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; int i=0;\n"
     " loop: h=h*1103515245u+12345u; acc=acc*131u + (h^(h>>13)); i++; if(i<200) goto loop;\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress267", 0},

    // A goto-threaded state machine (switch-free dispatch).
    {p+"_gotostate",
     t+" "+p+"_gotostate("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; int st=0,n=0;\n"
     " again: h=h*1103515245u+12345u;\n"
     "  if(st==0){ acc+=h; st=1; goto step; }\n"
     "  if(st==1){ acc^=h; st=2; goto step; }\n"
     "  acc+=h*3u; st=0;\n"
     " step: n++; if(n<200) goto again;\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress267", 0},

    // Several paths converging on one shared goto tail.
    {p+"_gototail",
     t+" "+p+"_gototail("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned r;\n"
     "    if(h&1u){ r=h*3u; goto done; }\n"
     "    if(h&2u){ r=h^0x55u; goto done; }\n"
     "    if(h&4u){ r=h>>2; goto done; }\n"
     "    r=~h;\n"
     "   done: acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress267", 0},

    // Forward gotos breaking out of nested loops.
    {p+"_gotobreak",
     t+" "+p+"_gotobreak("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<40;i++){\n"
     "    for(int j=0;j<8;j++){ h=h*1103515245u+12345u;\n"
     "      if((h&15u)==7u) goto next; acc=acc*131u + (h>>3); }\n"
     "   next: acc=acc*7u + (unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress267", 0},

    // A back-edge re-entering the loop body below its head.
    {p+"_gotoreent",
     t+" "+p+"_gotoreent("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; int i=0;\n"
     "  if(a&1) goto mid;\n"
     " top: h=h*1103515245u+12345u; acc+=h;\n"
     " mid: h=h*2654435761u+1u; acc^=h; i++; if(i<200) goto top;\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress267", 0},

    // goto dispatch combined with a switch.
    {p+"_gotomix",
     t+" "+p+"_gotomix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned r;\n"
     "    switch(h&3u){ case 0:r=h+1u;break; case 1:r=h*3u; goto tail; case 2:r=~h;break; default:r=h>>1; }\n"
     "    r+=0x10u;\n"
     "   tail: acc=acc*131u+r; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress267", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress267TC("x64o267", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress267TC("x86o267", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress267TC("a64o267", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress267TC("armo267", "int");

INSTANTIATE_TEST_SUITE_P(OptStress267, X64OptStress267RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress267, X86OptStress267RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress267, A64OptStress267RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress267, ARM32OptStress267RT, ::testing::ValuesIn(kARM), rtTCName);
