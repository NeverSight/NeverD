//===- AllPlatform_CFGLoopXformRTTests.cpp - CFG/loop xform probing -*-C++*-=//
//
// Differential roundtrip probing of control-flow / loop *structuring* shapes
// the existing CFGStress / CondLoop suites do not exercise, aimed at the
// lift -> Med -> High control-flow reconstruction (CFStructurer, HighLoopRecovery,
// MedToHigh PHI-copy insertion) and the recompile backend's re-lowering of the
// recovered structured form.  Each kernel takes one scalar argument, is bounded
// (terminates for every input), folds to a single integer return, and emits no
// runtime helper, so all four targets are checked native-vs-lifted at both -O0
// (explicit branches, spills) and -O2 (rotated loops, cmov/csel).
//
// Novel shapes vs. CFGStress (tailrec/mut3/brk2/multiexit/duff/contlbl) and
// CondLoop (branchless flag idioms):
//   * irreducible CFG — a goto into the middle of a loop body creates a
//     two-entry strongly-connected region (no natural loop header), the
//     canonical shape that defeats natural-loop recovery without node splitting
//     / goto emission;
//   * multi-latch loop — two distinct back-edges to one header (two goto-top
//     paths), so the header carries more than one latch predecessor;
//   * headerless infinite loop — for(;;) whose only exits are two breaks (no
//     loop condition to recover, exit found only from the break edges);
//   * do/while continue — continue targets the *bottom* condition test, not the
//     top (distinct latch shape from for/while continue);
//   * early-return + break to a PHI join — one arm returns, another breaks to a
//     post-loop block whose live value is a PHI over break vs fallthrough;
//   * switch-in-loop break mix — switch-break (fall to tail), loop-continue
//     (skip tail) and loop-break (goto out) in one dispatch, so the `break`
//     keyword is overloaded between the switch and the loop.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CFGLoopRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CFGLoopRT, Verify) { roundTripX64(GetParam()); }
class X86CFGLoopRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86CFGLoopRT, Verify) { roundTripX86(GetParam()); }
class A64CFGLoopRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CFGLoopRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32CFGLoopRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CFGLoopRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeCFGLoopTC(const char *prefix, const char *T,
                                              int Opt) {
  std::string p = prefix, t = T;
  return {
    // Irreducible CFG: `goto mid` jumps into the middle of the for-body, so the
    // loop's strongly-connected region has two entries (the header `i<60` test
    // and the interior label `mid`) — no single natural-loop header.  The goto
    // predicate reads an input bit so clang cannot fold the edge away; at -O0
    // the machine CFG stays genuinely irreducible.  Recovery must emit correct
    // goto/structured edges and a correct PHI for x/h at `mid` (two preds).
    {p+"_irr",
     t+" "+p+"_irr("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0; int i=0;\n"
     "  if((a>>3)&1) goto mid;\n"
     "  for(i=0;i<60;i++){\n"
     "    x=x*1103515245u+12345u; h=h*31u+x;\n"
     "  mid:\n"
     "    x^=x>>13; h+=x*2654435761u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x4CULL}, "CFGLoop", Opt},

    // Multi-latch loop: two `goto top` back-edges to the same header from
    // different points in the body, so the header has two latch predecessors.
    // HighLoopRecovery must treat both as back-edges of one loop, not two loops.
    {p+"_mlatch",
     t+" "+p+"_mlatch("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0; int i=0;\n"
     "top:\n"
     "  if(i>=100) goto done;\n"
     "  x=x*1103515245u+12345u; h=h*31u+x;\n"
     "  if((x>>5)&1u){ h^=0xABCDu; i++; goto top; }\n"
     "  h+=x>>3; i++; goto top;\n"
     "done:\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0xA7ULL}, "CFGLoop", Opt},

    // Headerless infinite loop: for(;;) with no loop condition, terminated only
    // by two `break`s (a value-triggered exit and a bounded-count exit).  The
    // structurer must synthesize the loop exit purely from the break edges.
    {p+"_inftrue",
     t+" "+p+"_inftrue("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0; int i=0;\n"
     "  for(;;){\n"
     "    x=x*1103515245u+12345u; h=h*31u+x;\n"
     "    if((x&0xFFu)==0x42u) break;\n"
     "    if(i++>=200) break;\n"
     "    h+=x>>7;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)(h+(unsigned)i); }\n",
     {0x35ULL}, "CFGLoop", Opt},

    // do/while with continue: `continue` branches to the bottom `while(++i<...)`
    // test, a different latch position than a for/while continue (which targets
    // the top).  The recovered loop must place the continue edge at the test.
    {p+"_dowc",
     t+" "+p+"_dowc("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0; int i=0;\n"
     "  do{\n"
     "    x=x*1103515245u+12345u;\n"
     "    if((x>>11)&1u){ h^=x; continue; }\n"
     "    h=h*31u+x;\n"
     "  }while(++i<120);\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x6DULL}, "CFGLoop", Opt},

    // Early return + break to a PHI join: one arm returns out of the function,
    // another breaks to the post-loop block where `last` is a PHI (set on break,
    // by the per-iteration fallthrough, or the initial 0).  Recovery must keep
    // the return edge distinct from the break edge and the join PHI correct.
    {p+"_retbrk",
     t+" "+p+"_retbrk("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0; unsigned last=0;\n"
     "  for(int i=0;i<150;i++){\n"
     "    x=x*1103515245u+12345u; h=h*31u+x;\n"
     "    if((x&0x7FFu)==0x123u) return ("+t+")(unsigned long)(h^0xDEADu);\n"
     "    if(((x>>9)&0xFFu)==0x77u){ last=x; break; }\n"
     "    last=h;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)(last*2654435761u + h); }\n",
     {0x51ULL}, "CFGLoop", Opt},

    // switch-in-loop break overload: one case `break`s the switch (falls to the
    // loop tail), one `continue`s the loop (skips the tail), one `goto out`s the
    // loop entirely.  `break` means switch-exit here, distinct from the loop
    // break — the structurer must not conflate the two exit kinds.
    {p+"_swbrk",
     t+" "+p+"_swbrk("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0; int i=0;\n"
     "  for(i=0;i<120;i++){\n"
     "    x=x*1103515245u+12345u;\n"
     "    switch((x>>6)&3u){\n"
     "    case 0: h+=x; break;\n"
     "    case 1: h^=x<<1; continue;\n"
     "    case 2: if((x>>20)&1u) goto out; h-=x; break;\n"
     "    default: h+=x>>2; break;\n"
     "    }\n"
     "    h=h*31u+(unsigned)i;\n"
     "  }\n"
     "out:\n"
     "  return ("+t+")(unsigned long)(h+(unsigned)i); }\n",
     {0x1BULL}, "CFGLoop", Opt},
  };
}

// Second batch: shapes at the switch-loop intersection and deeper structuring
// seams — the intersection of the two prior suites (SwitchXform + this one).
static std::vector<RoundTripTC> makeCFGLoop2TC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;
  return {
    // Flattened control flow: a `while(state!=DONE) switch(state){...}` state
    // machine where each arm assigns the next state — the switch dispatch and
    // the loop back-edge are the SAME cycle, so switch recovery and loop
    // recovery must cooperate.  This is the canonical control-flow-flattening
    // shape (what an obfuscator produces); a hard-cap on iterations guarantees
    // termination even if a state assignment is mis-recovered on the native run.
    {p+"_statemachine",
     t+" "+p+"_statemachine("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0; unsigned st=1; int guard=0;\n"
     "  while(st!=0u && guard++<4000){\n"
     "    switch(st){\n"
     "    case 1: x=x*1103515245u+12345u; h^=x; st=2; break;\n"
     "    case 2: h=h*31u+x; st=((x>>8)&1u)?3u:4u; break;\n"
     "    case 3: h+=x>>3; st=((x>>9)&1u)?4u:5u; break;\n"
     "    case 4: h^=x<<1; st=((x>>10)&1u)?2u:5u; break;\n"
     "    case 5: h-=x>>5; st=((h>>4)&7u)==0u?0u:1u; break;\n"
     "    default: st=0u; break;\n"
     "    }\n"
     "  }\n"
     "  return ("+t+")(unsigned long)(h+(unsigned)guard); }\n",
     {0x77ULL}, "CFGLoop2", Opt},

    // Interlocking irreducible region: two blocks A,B that jump to each other
    // conditionally (A->B and B->A), each also looping — two entangled cycles
    // with no single header, one nesting depth beyond the single `_irr` goto.
    {p+"_irr2",
     t+" "+p+"_irr2("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0; int i=0;\n"
     "  int inB=(a>>2)&1;\n"
     "  if(inB) goto B;\n"
     "A:\n"
     "  x=x*1103515245u+12345u; h+=x; i++;\n"
     "  if(i>=140) goto done;\n"
     "  if((x>>7)&1u) goto B;\n"
     "  goto A;\n"
     "B:\n"
     "  x^=x>>13; h=h*31u+x; i++;\n"
     "  if(i>=140) goto done;\n"
     "  if((x>>8)&1u) goto A;\n"
     "  goto B;\n"
     "done:\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x9BULL}, "CFGLoop2", Opt},

    // 3-way PHI merge after nested if inside a loop: the post-if live value has
    // three reaching definitions (then / else-if / else), merged before the loop
    // tail — insertPhiCopies must place all three copies on the right edges.
    {p+"_phi3",
     t+" "+p+"_phi3("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  for(int i=0;i<160;i++){\n"
     "    x=x*1103515245u+12345u; unsigned v;\n"
     "    if((x>>4)&1u) v=x+0x1111u;\n"
     "    else if((x>>5)&1u) v=x^0x2222u;\n"
     "    else v=x-0x3333u;\n"
     "    h=h*31u+v;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)h; }\n",
     {0x35ULL}, "CFGLoop2", Opt},

    // Short-circuit && / || ladder gating a break: the compound condition lowers
    // to a chain of blocks (each && / || is its own branch), and the whole chain
    // decides one loop exit — the structurer must recover the nested if/or edges.
    {p+"_scladder",
     t+" "+p+"_scladder("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0; int i=0;\n"
     "  for(i=0;i<200;i++){\n"
     "    x=x*1103515245u+12345u; h=h*31u+x;\n"
     "    if(((x>>3)&1u) && ((x>>11)&1u) &&\n"
     "       (((x>>17)&1u) || ((x>>23)&0xFu)==0xAu)) break;\n"
     "    h+=x>>6;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)(h+(unsigned)i); }\n",
     {0x6DULL}, "CFGLoop2", Opt},

    // Multi-increment induction: the loop variable advances by different amounts
    // on different paths (two back-edges with distinct increments), so `i` is a
    // PHI over two increment values — the loop test still bounds it.
    {p+"_multistep",
     t+" "+p+"_multistep("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0; int i=0;\n"
     "  while(i<300){\n"
     "    x=x*1103515245u+12345u; h=h*31u+x;\n"
     "    if((x>>12)&1u) i+=3; else i+=1;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)(h+(unsigned)i); }\n",
     {0x51ULL}, "CFGLoop2", Opt},

    // Loop with a body that itself contains a full inner loop AND an early break
    // out of BOTH via a flag, where the outer post-loop value is a PHI over the
    // normal-exit and the flagged-break-exit — nested loop + shared exit join.
    {p+"_nestbrkjoin",
     t+" "+p+"_nestbrkjoin("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0; unsigned res=0; int hit=0;\n"
     "  for(int i=0;i<40 && !hit;i++){\n"
     "    for(int j=0;j<40;j++){\n"
     "      x=x*1103515245u+12345u; h=h*31u+x;\n"
     "      if(((x>>14)&0x3FFu)==((unsigned)a&0x3FFu)){ res=h^x; hit=1; break; }\n"
     "    }\n"
     "    h+=0x100u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)(hit?res:(h*2654435761u)); }\n",
     {0x1BULL}, "CFGLoop2", Opt},
  };
}

// Third batch: state-machine dispatch on a loop-carried variable whose case
// values have a NON-ZERO minimum and are NOT a power-of-two mask of the value,
// so clang biases the dispatch index by `sub #MIN` with no masking guard (at
// -O2 it proves the range and drops the bound check, leaving a jump table whose
// index is `switch_var - MIN`).  These directly exercise the case-label ↔
// dispatch-value coordinate seam: the emitter dispatches on the post-`sub`
// index, so the recovered labels must be raw table positions, not biased by MIN.
// Each varies the minimum and the number of arms.
//
// (A two-state-machine-in-one-function shape — two adjacent unguarded PIC jump
// tables — is deliberately NOT included: it hits a distinct, harder resolver
// gap where two adjacent tables with no range guard form one continuous
// relocation run so the first table over-reads into the second; see §15.2
// adjacent-unguarded-pic-table-note.)
static std::vector<RoundTripTC> makeCFGLoop3TC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;
  return {
    // Loop-carried state machine with case values in [10,17] (MIN=10): the
    // dispatch index is `st-10`, an unmasked biased subtract.  Recovered labels
    // must be 0..7 in the emitter's post-`sub` coordinate, not 10..17.
    {p+"_statebias10",
     t+" "+p+"_statebias10("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0; unsigned st=10; int g=0;\n"
     "  while(st!=0u && g++<4000){\n"
     "    switch(st){\n"
     "    case 10: x=x*1103515245u+12345u; h^=x; st=11; break;\n"
     "    case 11: h=h*31u+x; st=((x>>8)&1u)?12u:13u; break;\n"
     "    case 12: h+=x>>3; st=((x>>9)&1u)?13u:14u; break;\n"
     "    case 13: h^=x<<1; st=((x>>10)&1u)?11u:15u; break;\n"
     "    case 14: h-=x>>5; st=((x>>11)&1u)?15u:16u; break;\n"
     "    case 15: h+=x*3u; st=((x>>12)&1u)?16u:17u; break;\n"
     "    case 16: h^=x>>2; st=((x>>13)&1u)?11u:17u; break;\n"
     "    case 17: h+=0x9E37u; st=((h>>4)&7u)==0u?0u:10u; break;\n"
     "    default: st=0u; break;\n"
     "    }\n"
     "  }\n"
     "  return ("+t+")(unsigned long)(h+(unsigned)g); }\n",
     {0x5AULL}, "CFGLoop3", Opt},

    // A different non-zero minimum (MIN=3) with 8 dense arms — a distinct bias
    // so the recovered table base and normalization differ from statebias10.
    {p+"_statebias3",
     t+" "+p+"_statebias3("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0; unsigned st=3; int g=0;\n"
     "  while(st!=0u && g++<4000){\n"
     "    switch(st){\n"
     "    case 3: x=x*1103515245u+12345u; h^=x; st=4; break;\n"
     "    case 4: h=h*31u+x; st=((x>>8)&1u)?5u:6u; break;\n"
     "    case 5: h+=x>>3; st=((x>>9)&1u)?6u:7u; break;\n"
     "    case 6: h^=x<<1; st=((x>>10)&1u)?4u:8u; break;\n"
     "    case 7: h-=x>>5; st=((x>>11)&1u)?8u:9u; break;\n"
     "    case 8: h+=x*3u; st=((x>>12)&1u)?9u:10u; break;\n"
     "    case 9: h^=x>>2; st=((x>>13)&1u)?4u:10u; break;\n"
     "    case 10: h+=0x85EBu; st=((h>>4)&7u)==0u?0u:3u; break;\n"
     "    default: st=0u; break;\n"
     "    }\n"
     "  }\n"
     "  return ("+t+")(unsigned long)(h+(unsigned)g); }\n",
     {0x3CULL}, "CFGLoop3", Opt},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64O2 = makeCFGLoopTC("x64cl", "long", 2);
static const std::vector<RoundTripTC> kX86O2 = makeCFGLoopTC("x86cl", "int", 2);
static const std::vector<RoundTripTC> kA64O2 = makeCFGLoopTC("a64cl", "long", 2);
static const std::vector<RoundTripTC> kARMO2 = makeCFGLoopTC("armcl", "int", 2);
static const std::vector<RoundTripTC> kX64O0 = makeCFGLoopTC("x64cl0", "long", 0);
static const std::vector<RoundTripTC> kX86O0 = makeCFGLoopTC("x86cl0", "int", 0);
static const std::vector<RoundTripTC> kA64O0 = makeCFGLoopTC("a64cl0", "long", 0);
static const std::vector<RoundTripTC> kARMO0 = makeCFGLoopTC("armcl0", "int", 0);

INSTANTIATE_TEST_SUITE_P(CFGLoop, X64CFGLoopRT, ::testing::ValuesIn(kX64O2), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop, X86CFGLoopRT, ::testing::ValuesIn(kX86O2), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop, A64CFGLoopRT, ::testing::ValuesIn(kA64O2), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop, ARM32CFGLoopRT, ::testing::ValuesIn(kARMO2), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoopO0, X64CFGLoopRT, ::testing::ValuesIn(kX64O0), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoopO0, X86CFGLoopRT, ::testing::ValuesIn(kX86O0), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoopO0, A64CFGLoopRT, ::testing::ValuesIn(kA64O0), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoopO0, ARM32CFGLoopRT, ::testing::ValuesIn(kARMO0), rtTCName);

static const std::vector<RoundTripTC> kX64B = makeCFGLoop2TC("x64cly", "long", 2);
static const std::vector<RoundTripTC> kX86B = makeCFGLoop2TC("x86cly", "int", 2);
static const std::vector<RoundTripTC> kA64B = makeCFGLoop2TC("a64cly", "long", 2);
static const std::vector<RoundTripTC> kARMB = makeCFGLoop2TC("armcly", "int", 2);
static const std::vector<RoundTripTC> kX64B0 = makeCFGLoop2TC("x64cly0", "long", 0);
static const std::vector<RoundTripTC> kX86B0 = makeCFGLoop2TC("x86cly0", "int", 0);
static const std::vector<RoundTripTC> kA64B0 = makeCFGLoop2TC("a64cly0", "long", 0);
static const std::vector<RoundTripTC> kARMB0 = makeCFGLoop2TC("armcly0", "int", 0);

INSTANTIATE_TEST_SUITE_P(CFGLoop2, X64CFGLoopRT, ::testing::ValuesIn(kX64B), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop2, X86CFGLoopRT, ::testing::ValuesIn(kX86B), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop2, A64CFGLoopRT, ::testing::ValuesIn(kA64B), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop2, ARM32CFGLoopRT, ::testing::ValuesIn(kARMB), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop2O0, X64CFGLoopRT, ::testing::ValuesIn(kX64B0), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop2O0, X86CFGLoopRT, ::testing::ValuesIn(kX86B0), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop2O0, A64CFGLoopRT, ::testing::ValuesIn(kA64B0), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop2O0, ARM32CFGLoopRT, ::testing::ValuesIn(kARMB0), rtTCName);

static const std::vector<RoundTripTC> kX64C = makeCFGLoop3TC("x64clz", "long", 2);
static const std::vector<RoundTripTC> kX86C = makeCFGLoop3TC("x86clz", "int", 2);
static const std::vector<RoundTripTC> kA64C = makeCFGLoop3TC("a64clz", "long", 2);
static const std::vector<RoundTripTC> kARMC = makeCFGLoop3TC("armclz", "int", 2);
static const std::vector<RoundTripTC> kX64C0 = makeCFGLoop3TC("x64clz0", "long", 0);
static const std::vector<RoundTripTC> kX86C0 = makeCFGLoop3TC("x86clz0", "int", 0);
static const std::vector<RoundTripTC> kA64C0 = makeCFGLoop3TC("a64clz0", "long", 0);
static const std::vector<RoundTripTC> kARMC0 = makeCFGLoop3TC("armclz0", "int", 0);

INSTANTIATE_TEST_SUITE_P(CFGLoop3, X64CFGLoopRT, ::testing::ValuesIn(kX64C), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop3, X86CFGLoopRT, ::testing::ValuesIn(kX86C), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop3, A64CFGLoopRT, ::testing::ValuesIn(kA64C), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop3, ARM32CFGLoopRT, ::testing::ValuesIn(kARMC), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop3O0, X64CFGLoopRT, ::testing::ValuesIn(kX64C0), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop3O0, X86CFGLoopRT, ::testing::ValuesIn(kX86C0), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop3O0, A64CFGLoopRT, ::testing::ValuesIn(kA64C0), rtTCName);
INSTANTIATE_TEST_SUITE_P(CFGLoop3O0, ARM32CFGLoopRT, ::testing::ValuesIn(kARMC0), rtTCName);
