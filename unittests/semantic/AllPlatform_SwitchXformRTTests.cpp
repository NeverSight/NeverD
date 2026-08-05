//===- AllPlatform_SwitchXformRTTests.cpp - switch xform probing -*-C++*-=//
//
// Differential roundtrip probing of switch / jump-table transformation shapes
// the existing Switch* suites do not exercise, aimed at the lift -> Med -> High
// switch recovery (NdOpSwitchRecovery / MedToHigh) and the LLVM emitter's
// jump-table lowering (MedLLVMEmitter::emitJumpTableSwitch), then back through
// the recompile backend.  Each kernel takes one scalar argument, keeps every
// dispatch index inside the switch's defined range (so out-of-range UB never
// diverges the two runs), and returns a value-dependent hash.  The fixture
// compiles the C natively, emulates it, lifts + recompiles with NeverD, and
// emulates the result, asserting the return values match bit-for-bit across all
// four targets.
//
// Novel shapes vs. SwitchVariety / SwitchTable / SwitchModulo / ComputedGoto:
//   * 64-bit switch value with case labels above 2^32 (wide index recovery);
//   * jump-table-to-return: each arm returns a distinct constant (the RETURN
//     arm of lowerSwitchFromJumpTable, not an assign-to-r0 arm);
//   * Duff's device: a switch whose arms fall through into a do/while loop body
//     (the switch and the loop share blocks — hostile to CFG structuring);
//   * default label placed *between* case labels (mid-list default);
//   * two independent switches on the same value back-to-back (per-insn JT keying);
//   * unreachable-default dense switch (clang drops the bound -> raw table).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SwXformRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SwXformRT, Verify) { roundTripX64(GetParam()); }
class X86SwXformRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86SwXformRT, Verify) { roundTripX86(GetParam()); }
class A64SwXformRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64SwXformRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32SwXformRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32SwXformRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeSwXformTC(const char *prefix, const char *T,
                                              int Opt) {
  std::string p = prefix, t = T;
  return {
    // Jump-table-to-return: every arm returns a distinct constant with no shared
    // epilogue.  Each case block is a single RETURN, so lowerSwitchFromJumpTable
    // must recover the RETURN arm (not the assign-to-r0 arm) for every entry.
    {p+"_ret8",
     t+" "+p+"_ret8("+t+" a){\n"
     "  unsigned k=(unsigned)a & 7u;\n"
     "  switch(k){\n"
     "  case 0: return ("+t+")0x11111111;\n"
     "  case 1: return ("+t+")0x22222222;\n"
     "  case 2: return ("+t+")0x33333333;\n"
     "  case 3: return ("+t+")0x44444444;\n"
     "  case 4: return ("+t+")0x55555555;\n"
     "  case 5: return ("+t+")0x66666666;\n"
     "  case 6: return ("+t+")0x77777777;\n"
     "  default: return ("+t+")0x0BADF00D;\n"
     "  }\n"
     "}\n",
     {5ULL}, "SwXform", Opt},

    // Default label sits between two case labels in source order.  clang keeps
    // the default arm wherever it is written; the recovery must not assume the
    // default is the last block.
    {p+"_middef",
     t+" "+p+"_middef("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned k=acc & 7u;\n"
     "    switch(k){\n"
     "    case 0: acc+=0x9E3779B9u; break;\n"
     "    case 1: acc^=acc<<13; break;\n"
     "    case 2: acc*=2654435761u; break;\n"
     "    default: acc=acc*31u+0x1234u; break;\n"
     "    case 5: acc-=acc>>5; break;\n"
     "    case 6: acc^=acc>>17; break;\n"
     "    case 7: acc+=0x85EBCA6Bu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x1234ULL}, "SwXform", Opt},

    // Two independent switches on the SAME value, back-to-back in the same
    // iteration.  Each dispatch is a separate INDIR_BR at a distinct address, so
    // jump-table resolution keyed by instruction address must recover both.
    {p+"_twoswitch",
     t+" "+p+"_twoswitch("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<72;i++){\n"
     "    unsigned k=acc & 7u;\n"
     "    switch(k){\n"
     "    case 0: acc+=0x9E37u; break; case 1: acc^=acc<<7; break;\n"
     "    case 2: acc*=3u; break;    case 3: acc-=acc>>3; break;\n"
     "    case 4: acc+=0x1111u; break; case 5: acc^=0x2222u; break;\n"
     "    case 6: acc+=acc<<2; break; default: acc^=0xA5u; break;\n"
     "    }\n"
     "    switch(k){\n"
     "    case 0: acc^=0x11u; break; case 1: acc+=0x33u; break;\n"
     "    case 2: acc-=0x55u; break; case 3: acc^=0x77u; break;\n"
     "    case 4: acc+=0x99u; break; case 5: acc^=0xBBu; break;\n"
     "    case 6: acc-=0xDDu; break; default: acc+=0xFFu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x99ULL}, "SwXform", Opt},

    // Duff's device: the switch arms fall through into a do/while loop body, so
    // the switch cases and the loop share blocks.  This is the canonical shape
    // that defeats naive CFG structuring (the switch has no clean per-arm block).
    {p+"_duff",
     t+" "+p+"_duff("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u; unsigned n=(acc & 15u)+1u; unsigned s=0;\n"
     "  unsigned cnt=(n+3u)/4u; unsigned r=n & 3u;\n"
     "  switch(r){\n"
     "  case 0: do{ s+=acc; acc=acc*1664525u+1013904223u;\n"
     "  case 3:      s^=acc; acc=(acc<<13)|(acc>>19);\n"
     "  case 2:      s+=acc>>3; acc*=2654435761u;\n"
     "  case 1:      s^=acc<<1; acc+=0x9E3779B9u;\n"
     "          }while(--cnt);\n"
     "  }\n"
     "  return ("+t+")(unsigned long)(s^acc); }\n",
     {0x2ULL}, "SwXform", Opt},

    // Unreachable-default dense switch: with a default that cannot be reached the
    // compiler drops the range check, leaving a raw jump table.  The resolver
    // must bound the table by its entry run alone.  The index is reduced mod 7 so
    // it is always in [0,7) — every value hits a real case, so the original's UB
    // (unreachable) path is never taken on either side.
    {p+"_nodefault",
     t+" "+p+"_nodefault("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned k=acc % 7u; unsigned s;\n"
     "    switch(k){\n"
     "    case 0: s=0x9E3779B9u; break; case 1: s=0x85EBCA6Bu; break;\n"
     "    case 2: s=0xC2B2AE35u; break; case 3: s=0x27D4EB2Fu; break;\n"
     "    case 4: s=0x165667B1u; break; case 5: s=0xD3A2646Cu; break;\n"
     "    case 6: s=0xFD7046C5u; break; default: __builtin_unreachable();\n"
     "    }\n"
     "    acc=acc*s + (unsigned)i;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x424242ULL}, "SwXform", Opt},
  };
}

// Second batch: shapes that stress table-vs-branch selection, deep nesting, and
// narrow / mid-bit dispatch indices.
static std::vector<RoundTripTC> makeSwXform2TC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;
  return {
    // Lookup-table switch: each arm assigns one constant to the same variable,
    // which clang -O2 lowers to a `.rodata` value table indexed by the selector
    // (no code jump table).  Exercises the rodata-load path, not INDIR_BR.
    {p+"_lut",
     t+" "+p+"_lut("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned k=acc & 15u; unsigned m;\n"
     "    switch(k){\n"
     "    case 0:  m=3u; break;  case 1:  m=5u; break;\n"
     "    case 2:  m=7u; break;  case 3:  m=11u; break;\n"
     "    case 4:  m=13u; break; case 5:  m=17u; break;\n"
     "    case 6:  m=19u; break; case 7:  m=23u; break;\n"
     "    case 8:  m=29u; break; case 9:  m=31u; break;\n"
     "    case 10: m=37u; break; case 11: m=41u; break;\n"
     "    case 12: m=43u; break; case 13: m=47u; break;\n"
     "    case 14: m=53u; break; default: m=59u; break;\n"
     "    }\n"
     "    acc=acc*m + (unsigned)i;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x314159ULL}, "SwXform2", Opt},

    // Switch nested inside the DEFAULT arm of an outer switch — the inner jump
    // table is reached only via the outer default edge, so structuring must keep
    // the inner switch under the default rather than hoist it.
    {p+"_nestdef",
     t+" "+p+"_nestdef("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    switch(acc & 3u){\n"
     "    case 0: acc+=0x9E37u; break;\n"
     "    case 1: acc^=acc<<7; break;\n"
     "    default:\n"
     "      switch((acc>>2)&7u){\n"
     "      case 0: acc+=1u; break; case 1: acc*=3u; break;\n"
     "      case 2: acc^=0xFFu; break; case 3: acc-=acc>>4; break;\n"
     "      case 4: acc+=0x1234u; break; case 5: acc^=acc<<5; break;\n"
     "      case 6: acc*=5u; break; default: acc-=0x777u; break; }\n"
     "      break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x2718ULL}, "SwXform2", Opt},

    // Dense 20-way switch — a larger jump table than the small shapes, dispatched
    // on a value reduced mod 20 so every index is in range.
    {p+"_dense20",
     t+" "+p+"_dense20("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<60;i++){\n"
     "    unsigned k=acc % 20u;\n"
     "    switch(k){\n"
     "    case 0: acc+=1u; break;      case 1: acc^=2u; break;\n"
     "    case 2: acc*=3u; break;      case 3: acc-=4u; break;\n"
     "    case 4: acc+=5u; break;      case 5: acc^=6u; break;\n"
     "    case 6: acc*=7u; break;      case 7: acc-=8u; break;\n"
     "    case 8: acc+=9u; break;      case 9: acc^=10u; break;\n"
     "    case 10: acc*=11u; break;    case 11: acc-=12u; break;\n"
     "    case 12: acc+=13u; break;    case 13: acc^=14u; break;\n"
     "    case 14: acc*=15u; break;    case 15: acc-=16u; break;\n"
     "    case 16: acc+=17u; break;    case 17: acc^=18u; break;\n"
     "    case 18: acc*=19u; break;    default: acc-=20u; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*7u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0xC0FFEEULL}, "SwXform2", Opt},

    // Mid-bit dispatch index: the switch selects on bits [3:6] of the running
    // value, so the index is a shifted-and-masked slice rather than the low bits
    // — the resolver's index recovery must trace the shift+mask, not assume the
    // raw register is the index.
    {p+"_midbits",
     t+" "+p+"_midbits("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned k=(acc>>3) & 7u;\n"
     "    switch(k){\n"
     "    case 0: acc+=0x9E3779B9u; break; case 1: acc^=acc<<13; break;\n"
     "    case 2: acc*=2654435761u; break; case 3: acc-=acc>>5; break;\n"
     "    case 4: acc+=0x85EBCA6Bu; break; case 5: acc^=acc>>17; break;\n"
     "    case 6: acc=(acc<<7)|(acc>>25); break; default: acc+=0xA5A5u; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x9ABCULL}, "SwXform2", Opt},
  };
}

// 64-bit switch value with case labels above 2^32 — only meaningful for a 64-bit
// return type / value, so restricted to the `long` targets.  Recovery paths that
// truncate a switch case value to 32 bits would mis-route these.
static std::vector<RoundTripTC> makeSw64TC(const char *prefix) {
  std::string p = prefix;
  return {
    {p+"_wide64",
     "long "+p+"_wide64(long a){\n"
     "  unsigned long acc=(unsigned long)a|1UL;\n"
     "  for(int i=0;i<72;i++){\n"
     "    unsigned long k=(acc & 3UL) + 0x100000000UL;\n"
     "    switch(k){\n"
     "    case 0x100000000UL: acc+=0x9E3779B97F4A7C15UL; break;\n"
     "    case 0x100000001UL: acc^=acc<<21; break;\n"
     "    case 0x100000002UL: acc*=0xC2B2AE3D27D4EB4FUL; break;\n"
     "    case 0x100000003UL: acc-=acc>>7; break;\n"
     "    }\n"
     "    acc+=(unsigned long)i*0x9E3779B9UL;\n"
     "  }\n"
     "  return (long)acc; }\n",
     {0xDEADBEEFULL}, "SwXform", 2},
  };
}

// Third batch: shapes that force a NON-dense, non-base-0 lowering — the earlier
// batches all dispatch on the low bits of a value (a base-0 dense table).  Here
// the switch value is signed with a negative minimum (subtract-min bias with a
// negative base), sparse (clang builds a binary-search comparison tree, not a
// jump table — the HighIfChainToSwitch recovery path, distinct from
// JumpTableResolver), or split into two far-apart dense clusters (two biased
// jump tables in one function).  Each index is kept inside the case set so the
// original's default (present, never taken) UB path never diverges the runs.
static std::vector<RoundTripTC> makeSwXform3TC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;
  return {
    // Signed switch with negative case labels, k in [-4,3].  clang biases the
    // dense range by +4 (an add, not the plain low-bits mask of the earlier
    // shapes) before indexing, so recovery must trace a signed subtract-min with
    // a NEGATIVE minimum — a case the index/bound logic never sees at base 0.
    {p+"_signeg",
     t+" "+p+"_signeg("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    int k=(int)(acc & 7u) - 4;\n"
     "    switch(k){\n"
     "    case -4: acc+=0x9E3779B9u; break; case -3: acc^=acc<<13; break;\n"
     "    case -2: acc*=2654435761u; break; case -1: acc-=acc>>5; break;\n"
     "    case 0:  acc+=0x85EBCA6Bu; break; case 1:  acc^=acc>>17; break;\n"
     "    case 2:  acc=(acc<<7)|(acc>>25); break; case 3: acc+=0xA5A5u; break;\n"
     "    default: acc^=0xDEADu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x1357ULL}, "SwXform3", Opt},

    // Sparse switch: 8 labels spread across [3,35003] (density ~ 1/4375).  Below
    // the jump-table density threshold, clang lowers this to a binary-search
    // comparison tree of INT_EQUAL / range compares — exercising if-chain-to-
    // switch recovery and plain compare lifting rather than an INDIR_BR table.
    {p+"_sparse",
     t+" "+p+"_sparse("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned k=(acc & 7u)*5000u + 3u;\n"
     "    switch(k){\n"
     "    case 3:     acc+=0x9E3779B9u; break; case 5003:  acc^=acc<<13; break;\n"
     "    case 10003: acc*=2654435761u; break; case 15003: acc-=acc>>5; break;\n"
     "    case 20003: acc+=0x85EBCA6Bu; break; case 25003: acc^=acc>>17; break;\n"
     "    case 30003: acc=(acc<<7)|(acc>>25); break;\n"
     "    case 35003: acc+=0xA5A5u; break;\n"
     "    default:    acc^=0xDEADu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x2468ULL}, "SwXform3", Opt},

    // Two dense clusters far apart: {0,1,2,3} and {1004,1005,1006,1007}.  clang
    // splits on the cluster then emits a jump table per cluster, the second
    // biased by 1004 — so the function carries two INDIR_BR tables with
    // distinct non-zero bases, each keyed by its own dispatch address.
    {p+"_cluster",
     t+" "+p+"_cluster("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<96;i++){\n"
     "    unsigned sel=acc & 7u;\n"
     "    unsigned k=(sel<4u)?sel:(1000u+sel);\n"
     "    switch(k){\n"
     "    case 0: acc+=0x9E37u; break;    case 1: acc^=acc<<7; break;\n"
     "    case 2: acc*=3u; break;         case 3: acc-=acc>>3; break;\n"
     "    case 1004: acc+=0x1111u; break; case 1005: acc^=0x2222u; break;\n"
     "    case 1006: acc+=acc<<2; break;  case 1007: acc^=0xA5u; break;\n"
     "    default: acc^=0xBEEFu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x9ABCULL}, "SwXform3", Opt},
  };
}

// Fourth batch: shapes that stress the resolver's recursion / register-aliasing
// and the emitter's table extent — the three prior fixes in this suite were a
// nested-switch shared-register bound alias, a deep esp-chain recursion, and a
// narrow-constant APInt assertion, so these deliberately push on those seams:
//   * deepnest  — three switch levels nested in one arm, every level dispatching
//     on a slice of the SAME running accumulator (so all three table indices
//     alias one value across the block — the hostile case for the shared-reg
//     index-bound logic refined for two levels);
//   * biglut    — a ~140-arm dense jump table (case bodies cycle through eight
//     distinct op kinds so clang keeps a CODE jump table, not a rodata value
//     lookup), forcing a large table whose extent bound must include every
//     entry; on ARM32 >128 entries overflow a TBB byte table into a TBH
//     halfword table (a distinct compact-table path);
//   * chained   — switch #1 produces a value that becomes switch #2's selector
//     in the same iteration, so two dispatches are data-dependent (the index of
//     the second table is the loaded result of the first).
static std::vector<RoundTripTC> makeSwXform4TC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;

  // Build a dense N-arm switch whose case bodies cycle through eight genuinely
  // distinct operation kinds (add / xor-shift / mul / sub-shift / rotate / etc.)
  // keyed by the arm index, so clang lowers it to a real code jump table rather
  // than a rodata value table.  The selector is reduced mod N so every index is
  // in range and the (present) default is never taken.
  const int N = 140;
  std::string big;
  big += t + " " + p + "_biglut(" + t + " a){\n";
  big += "  unsigned acc=(unsigned)a|1u;\n";
  big += "  for(int i=0;i<64;i++){\n";
  big += "    unsigned k=acc % " + std::to_string(N) + "u;\n";
  big += "    switch(k){\n";
  for (int j = 0; j < N; ++j) {
    std::string c = std::to_string(j);
    std::string body;
    switch (j % 8) {
    case 0: body = "acc+=0x9E3779B9u+" + c + "u;"; break;
    case 1: body = "acc^=acc<<((" + c + "u&7u)+1u);"; break;
    case 2: body = "acc*=(2u*" + c + "u+3u);"; break;
    case 3: body = "acc-=acc>>((" + c + "u&7u)+1u);"; break;
    case 4: body = "acc+=0x85EBCA6Bu^" + c + "u;"; break;
    case 5: body = "acc^=acc>>((" + c + "u&7u)+3u);"; break;
    case 6: body = "acc=(acc<<5)|(acc>>27);acc+=" + c + "u;"; break;
    default: body = "acc+=(" + c + "u*2654435761u);"; break;
    }
    big += "    case " + c + ": " + body + " break;\n";
  }
  big += "    default: acc^=0xDEADu; break;\n";
  big += "    }\n";
  big += "    acc+=(unsigned)i*131u;\n";
  big += "  }\n";
  big += "  return (" + t + ")(unsigned long)acc; }\n";

  return {
    // Three levels of switch nested in a single arm, each dispatching on a
    // different bit-slice of the same accumulator — all three table indices
    // alias one value live across the dispatch block.
    {p+"_deepnest",
     t+" "+p+"_deepnest("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    switch(acc & 3u){\n"
     "    case 0: acc+=0x9E37u; break;\n"
     "    case 1: acc^=acc<<7; break;\n"
     "    case 2:\n"
     "      switch((acc>>2)&3u){\n"
     "      case 0: acc+=1u; break;\n"
     "      case 1:\n"
     "        switch((acc>>4)&3u){\n"
     "        case 0: acc*=3u; break;      case 1: acc-=acc>>4; break;\n"
     "        case 2: acc+=0x1234u; break; default: acc^=0xFFu; break;\n"
     "        }\n"
     "        break;\n"
     "      case 2: acc^=0x55u; break; default: acc-=0x777u; break;\n"
     "      }\n"
     "      break;\n"
     "    default: acc=acc*31u+0x1234u; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x1BADULL}, "SwXform4", Opt},

    {p+"_biglut", big, {0x33CCULL}, "SwXform4", Opt},

    // Data-dependent chained dispatch: switch #1 assigns a selector `t`, and
    // switch #2 dispatches on `t & 7` in the same iteration — the second table's
    // index is the result the first switch produced.
    {p+"_chained",
     t+" "+p+"_chained("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned t;\n"
     "    switch(acc & 7u){\n"
     "    case 0: t=0x9E37u; break; case 1: t=acc<<3; break;\n"
     "    case 2: t=acc*3u; break;  case 3: t=acc>>2; break;\n"
     "    case 4: t=acc+7u; break;  case 5: t=acc^0x55u; break;\n"
     "    case 6: t=acc-9u; break;  default: t=0xA5u; break;\n"
     "    }\n"
     "    switch(t & 7u){\n"
     "    case 0: acc^=0x11u; break; case 1: acc+=0x33u; break;\n"
     "    case 2: acc-=0x55u; break; case 3: acc^=0x77u; break;\n"
     "    case 4: acc+=0x99u; break; case 5: acc^=0xBBu; break;\n"
     "    case 6: acc-=0xDDu; break; default: acc+=0xFFu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x77E1ULL}, "SwXform4", Opt},
  };
}

// Fifth batch: shapes complementary to biglut — a mid-size dense table (~64
// arms) that on AArch64 selects the BYTE compact table (`ldrb [base,idx]`, the
// unscaled-index sibling of biglut's halfword `ldrh [base,idx,lsl #1]`), a
// fall-through chain where cases share blocks (no break), and a signed 64-bit
// sparse switch (a binary-search tree over 64-bit signed labels).
static std::vector<RoundTripTC> makeSwXform5TC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;

  // ~64-arm dense switch with small case bodies so the per-arm offset stays
  // under a byte: on AArch64 this selects the byte offset table (ldrb), the
  // path biglut's halfword table does not exercise.
  const int N = 64;
  std::string mid;
  mid += t + " " + p + "_midbyte(" + t + " a){\n";
  mid += "  unsigned acc=(unsigned)a|1u;\n";
  mid += "  for(int i=0;i<64;i++){\n";
  mid += "    unsigned k=acc % " + std::to_string(N) + "u;\n";
  mid += "    switch(k){\n";
  for (int j = 0; j < N; ++j) {
    std::string c = std::to_string(j);
    std::string body;
    switch (j % 4) {
    case 0: body = "acc+=" + c + "u;"; break;
    case 1: body = "acc^=" + c + "u;"; break;
    case 2: body = "acc-=" + c + "u;"; break;
    default: body = "acc+=" + std::to_string(j * 7 + 1) + "u;"; break;
    }
    mid += "    case " + c + ": " + body + " break;\n";
  }
  mid += "    default: acc^=0xDEADu; break;\n";
  mid += "    }\n";
  mid += "    acc+=(unsigned)i*131u;\n";
  mid += "  }\n";
  mid += "  return (" + t + ")(unsigned long)acc; }\n";

  return {
    {p+"_midbyte", mid, {0x5151ULL}, "SwXform5", Opt},

    // Fall-through chain: consecutive cases with no `break` fall into the next,
    // so the case blocks are shared/chained (like Duff's device but without the
    // loop) — the structuring must not duplicate or drop the shared tails.
    {p+"_fallthru",
     t+" "+p+"_fallthru("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned s=0; unsigned k=acc & 7u;\n"
     "    switch(k){\n"
     "    case 0: s+=0x11u;\n"
     "    case 1: s+=0x22u;\n"
     "    case 2: s+=0x33u;\n"
     "    case 3: s+=0x44u; break;\n"
     "    case 4: s+=0x55u;\n"
     "    case 5: s+=0x66u;\n"
     "    default: s+=0x77u;\n"
     "    }\n"
     "    acc=acc*31u + s + (unsigned)i;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x6262ULL}, "SwXform5", Opt},

    // Index loaded from a read-only global array indexed by the running value,
    // so the switch selector is a memory load (not a masked register) — the
    // index-tracing must follow the load, not stop at a fresh register.
    {p+"_memidx",
     "static const unsigned char "+p+"_mtab[16]="
     "{3,1,4,1,5,2,6,5,3,0,7,4,2,6,0,7};\n"+
     t+" "+p+"_memidx("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned k="+p+"_mtab[acc & 15u];\n"
     "    switch(k){\n"
     "    case 0: acc+=0x9E37u; break; case 1: acc^=acc<<7; break;\n"
     "    case 2: acc*=3u; break;      case 3: acc-=acc>>3; break;\n"
     "    case 4: acc+=0x1111u; break; case 5: acc^=0x2222u; break;\n"
     "    case 6: acc+=acc<<2; break;  default: acc^=0xA5u; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x7373ULL}, "SwXform5", Opt},
  };
}

// Sixth batch: dispatch shapes whose index is not a plain mask of the running
// value — non-contiguous case labels (a dense table with default-filled holes,
// exercising the kept-slot / EntryIndices sparse-label path), a loop-carried
// PHI selector (the index is a back-edge PHI, not a fresh mask), and a signed
// sparse switch spanning negatives (a binary-search tree of signed compares).
static std::vector<RoundTripTC> makeSwXform6TC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;
  return {
    // Non-contiguous case labels {0,3,6,9,12}: clang builds a dense table over
    // [0,12] with the 8 holes routed to default.  The index always hits a real
    // case, so default is never taken, but the recovered table must fill the
    // holes with the default edge rather than mis-labelling or truncating.
    {p+"_gaps",
     t+" "+p+"_gaps("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned k=(acc % 5u)*3u;\n"
     "    switch(k){\n"
     "    case 0:  acc+=0x9E3779B9u; break; case 3:  acc^=acc<<13; break;\n"
     "    case 6:  acc*=2654435761u; break; case 9:  acc-=acc>>5; break;\n"
     "    case 12: acc+=0x85EBCA6Bu; break; default: acc^=0xDEADu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x4242ULL}, "SwXform6", Opt},

    // Loop-carried PHI selector: `sel` is dispatched on and then updated for the
    // next iteration, so the switch index is a back-edge PHI value (not a fresh
    // mask of acc) — the index recovery must anchor on the PHI, not assume the
    // masked value is freshly computed in the dispatch block.
    {p+"_loopcarry",
     t+" "+p+"_loopcarry("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u; unsigned sel=(acc&7u)+1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    switch(sel & 7u){\n"
     "    case 0: acc+=0x9E37u; break; case 1: acc^=acc<<7; break;\n"
     "    case 2: acc*=3u; break;      case 3: acc-=acc>>3; break;\n"
     "    case 4: acc+=0x1111u; break; case 5: acc^=0x2222u; break;\n"
     "    case 6: acc+=acc<<2; break;  default: acc^=0xA5u; break;\n"
     "    }\n"
     "    sel=sel*5u+acc;\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x1515ULL}, "SwXform6", Opt},

    // Signed sparse switch spanning negatives {-100,-50,0,50,100}: below the
    // jump-table density threshold and signed, so clang builds a binary-search
    // tree of SIGNED comparisons — exercising signed compare lifting and the
    // if-chain-to-switch recovery over a negative-through-positive span.
    {p+"_negrange",
     t+" "+p+"_negrange("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    int k=(int)(acc % 5u)*50 - 100;\n"
     "    switch(k){\n"
     "    case -100: acc+=0x9E3779B9u; break; case -50: acc^=acc<<13; break;\n"
     "    case 0:    acc*=2654435761u; break; case 50:  acc-=acc>>5; break;\n"
     "    case 100:  acc+=0x85EBCA6Bu; break; default:  acc^=0xDEADu; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x9090ULL}, "SwXform6", Opt},
  };
}

// Seventh batch: index-provenance shapes that combine the two just-fixed seams.
// bigbyte is a ~120-arm dense table sized so AArch64 keeps a BYTE compact table
// yet close enough to the halfword boundary to stress the width choice; wideidx
// dispatches on a mid-bit slice `(v>>5)&0x7f` into a 128-arm table (a shifted
// index feeding a large table); spillnest is a two-level nested switch at every
// opt level so both the outer and inner index spill/reload and share registers
// (Bug ② scenario, one level deeper).
static std::vector<RoundTripTC> makeSwXform7TC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;

  auto denseBody = [](int j) -> std::string {
    std::string c = std::to_string(j);
    switch (j % 6) {
    case 0: return "acc+=" + c + "u;";
    case 1: return "acc^=(" + c + "u<<1);";
    case 2: return "acc-=" + c + "u;";
    case 3: return "acc+=(acc>>2)+" + c + "u;";
    case 4: return "acc^=(acc<<1)^" + c + "u;";
    default: return "acc+=(" + std::to_string(j * 3 + 1) + "u);";
    }
  };

  // ~120-arm dense table: small case bodies keep AArch64 on the byte compact
  // table, just under the >127-arm halfword threshold biglut crossed.
  const int NB = 120;
  std::string bb;
  bb += t + " " + p + "_bigbyte(" + t + " a){\n";
  bb += "  unsigned acc=(unsigned)a|1u;\n";
  bb += "  for(int i=0;i<64;i++){\n";
  bb += "    unsigned k=acc % " + std::to_string(NB) + "u;\n";
  bb += "    switch(k){\n";
  for (int j = 0; j < NB; ++j)
    bb += "    case " + std::to_string(j) + ": " + denseBody(j) + " break;\n";
  bb += "    default: acc^=0xDEADu; break;\n    }\n";
  bb += "    acc+=(unsigned)i*131u;\n  }\n";
  bb += "  return (" + t + ")(unsigned long)acc; }\n";

  // 128-arm table dispatched on a mid-bit slice (v>>5)&0x7f — a shifted index
  // feeding a large table (index recovery must trace the shift + mask).
  const int NW = 128;
  std::string wi;
  wi += t + " " + p + "_wideidx(" + t + " a){\n";
  wi += "  unsigned acc=(unsigned)a|1u;\n";
  wi += "  for(int i=0;i<64;i++){\n";
  wi += "    unsigned k=(acc>>5) & 0x7Fu;\n";
  wi += "    switch(k){\n";
  for (int j = 0; j < NW; ++j)
    wi += "    case " + std::to_string(j) + ": " + denseBody(j) + " break;\n";
  wi += "    default: acc^=0xBEEFu; break;\n    }\n";
  wi += "    acc+=(unsigned)i*131u;\n  }\n";
  wi += "  return (" + t + ")(unsigned long)acc; }\n";

  return {
    {p+"_bigbyte", bb, {0x1234ULL}, "SwXform7", Opt},
    {p+"_wideidx", wi, {0x5678ULL}, "SwXform7", Opt},

    // Two-level nested switch, both dispatching on slices of the same value, so
    // at -O0 both indices spill to the frame and reload into shared registers —
    // the Bug ② scenario extended to a nested pair.
    {p+"_spillnest",
     t+" "+p+"_spillnest("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    switch(acc & 7u){\n"
     "    case 0: acc+=0x9E37u; break; case 1: acc^=acc<<7; break;\n"
     "    case 2: acc*=3u; break;      case 3: acc-=acc>>3; break;\n"
     "    case 4:\n"
     "      switch((acc>>3) & 7u){\n"
     "      case 0: acc+=0x11u; break; case 1: acc+=0x33u; break;\n"
     "      case 2: acc-=0x55u; break; case 3: acc^=0x77u; break;\n"
     "      case 4: acc+=0x99u; break; case 5: acc^=0xBBu; break;\n"
     "      case 6: acc-=0xDDu; break; default: acc+=0xFFu; break;\n"
     "      }\n"
     "      break;\n"
     "    case 5: acc^=0x2222u; break; case 6: acc+=acc<<2; break;\n"
     "    default: acc^=0xA5u; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x4321ULL}, "SwXform7", Opt},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64O2 = makeSwXformTC("x64swx", "long", 2);
static const std::vector<RoundTripTC> kX86O2 = makeSwXformTC("x86swx", "int", 2);
static const std::vector<RoundTripTC> kA64O2 = makeSwXformTC("a64swx", "long", 2);
static const std::vector<RoundTripTC> kARMO2 = makeSwXformTC("armswx", "int", 2);
static const std::vector<RoundTripTC> kX64O0 = makeSwXformTC("x64swx0", "long", 0);
static const std::vector<RoundTripTC> kX86O0 = makeSwXformTC("x86swx0", "int", 0);
static const std::vector<RoundTripTC> kA64O0 = makeSwXformTC("a64swx0", "long", 0);
static const std::vector<RoundTripTC> kARMO0 = makeSwXformTC("armswx0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform, X64SwXformRT, ::testing::ValuesIn(kX64O2), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform, X86SwXformRT, ::testing::ValuesIn(kX86O2), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform, A64SwXformRT, ::testing::ValuesIn(kA64O2), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform, ARM32SwXformRT, ::testing::ValuesIn(kARMO2), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXformO0, X64SwXformRT, ::testing::ValuesIn(kX64O0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXformO0, X86SwXformRT, ::testing::ValuesIn(kX86O0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXformO0, A64SwXformRT, ::testing::ValuesIn(kA64O0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXformO0, ARM32SwXformRT, ::testing::ValuesIn(kARMO0), rtTCName);

static const std::vector<RoundTripTC> kX64W = makeSw64TC("x64swx");
static const std::vector<RoundTripTC> kA64W = makeSw64TC("a64swx");
INSTANTIATE_TEST_SUITE_P(SwXform64, X64SwXformRT, ::testing::ValuesIn(kX64W), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform64, A64SwXformRT, ::testing::ValuesIn(kA64W), rtTCName);

static const std::vector<RoundTripTC> kX64B = makeSwXform2TC("x64swy", "long", 2);
static const std::vector<RoundTripTC> kX86B = makeSwXform2TC("x86swy", "int", 2);
static const std::vector<RoundTripTC> kA64B = makeSwXform2TC("a64swy", "long", 2);
static const std::vector<RoundTripTC> kARMB = makeSwXform2TC("armswy", "int", 2);
static const std::vector<RoundTripTC> kX64B0 = makeSwXform2TC("x64swy0", "long", 0);
static const std::vector<RoundTripTC> kX86B0 = makeSwXform2TC("x86swy0", "int", 0);
static const std::vector<RoundTripTC> kA64B0 = makeSwXform2TC("a64swy0", "long", 0);
static const std::vector<RoundTripTC> kARMB0 = makeSwXform2TC("armswy0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform2, X64SwXformRT, ::testing::ValuesIn(kX64B), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2, X86SwXformRT, ::testing::ValuesIn(kX86B), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2, A64SwXformRT, ::testing::ValuesIn(kA64B), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2, ARM32SwXformRT, ::testing::ValuesIn(kARMB), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2O0, X64SwXformRT, ::testing::ValuesIn(kX64B0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2O0, X86SwXformRT, ::testing::ValuesIn(kX86B0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2O0, A64SwXformRT, ::testing::ValuesIn(kA64B0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2O0, ARM32SwXformRT, ::testing::ValuesIn(kARMB0), rtTCName);

static const std::vector<RoundTripTC> kX64C = makeSwXform3TC("x64swz", "long", 2);
static const std::vector<RoundTripTC> kX86C = makeSwXform3TC("x86swz", "int", 2);
static const std::vector<RoundTripTC> kA64C = makeSwXform3TC("a64swz", "long", 2);
static const std::vector<RoundTripTC> kARMC = makeSwXform3TC("armswz", "int", 2);
static const std::vector<RoundTripTC> kX64C0 = makeSwXform3TC("x64swz0", "long", 0);
static const std::vector<RoundTripTC> kX86C0 = makeSwXform3TC("x86swz0", "int", 0);
static const std::vector<RoundTripTC> kA64C0 = makeSwXform3TC("a64swz0", "long", 0);
static const std::vector<RoundTripTC> kARMC0 = makeSwXform3TC("armswz0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform3, X64SwXformRT, ::testing::ValuesIn(kX64C), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3, X86SwXformRT, ::testing::ValuesIn(kX86C), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3, A64SwXformRT, ::testing::ValuesIn(kA64C), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3, ARM32SwXformRT, ::testing::ValuesIn(kARMC), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3O0, X64SwXformRT, ::testing::ValuesIn(kX64C0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3O0, X86SwXformRT, ::testing::ValuesIn(kX86C0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3O0, A64SwXformRT, ::testing::ValuesIn(kA64C0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform3O0, ARM32SwXformRT, ::testing::ValuesIn(kARMC0), rtTCName);

static const std::vector<RoundTripTC> kX64D = makeSwXform4TC("x64sww", "long", 2);
static const std::vector<RoundTripTC> kX86D = makeSwXform4TC("x86sww", "int", 2);
static const std::vector<RoundTripTC> kA64D = makeSwXform4TC("a64sww", "long", 2);
static const std::vector<RoundTripTC> kARMD = makeSwXform4TC("armsww", "int", 2);
static const std::vector<RoundTripTC> kX64D0 = makeSwXform4TC("x64sww0", "long", 0);
static const std::vector<RoundTripTC> kX86D0 = makeSwXform4TC("x86sww0", "int", 0);
static const std::vector<RoundTripTC> kA64D0 = makeSwXform4TC("a64sww0", "long", 0);
static const std::vector<RoundTripTC> kARMD0 = makeSwXform4TC("armsww0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform4, X64SwXformRT, ::testing::ValuesIn(kX64D), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4, X86SwXformRT, ::testing::ValuesIn(kX86D), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4, A64SwXformRT, ::testing::ValuesIn(kA64D), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4, ARM32SwXformRT, ::testing::ValuesIn(kARMD), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4O0, X64SwXformRT, ::testing::ValuesIn(kX64D0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4O0, X86SwXformRT, ::testing::ValuesIn(kX86D0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4O0, A64SwXformRT, ::testing::ValuesIn(kA64D0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform4O0, ARM32SwXformRT, ::testing::ValuesIn(kARMD0), rtTCName);

static const std::vector<RoundTripTC> kX64E = makeSwXform5TC("x64swv", "long", 2);
static const std::vector<RoundTripTC> kX86E = makeSwXform5TC("x86swv", "int", 2);
static const std::vector<RoundTripTC> kA64E = makeSwXform5TC("a64swv", "long", 2);
static const std::vector<RoundTripTC> kARME = makeSwXform5TC("armswv", "int", 2);
static const std::vector<RoundTripTC> kX64E0 = makeSwXform5TC("x64swv0", "long", 0);
static const std::vector<RoundTripTC> kX86E0 = makeSwXform5TC("x86swv0", "int", 0);
static const std::vector<RoundTripTC> kA64E0 = makeSwXform5TC("a64swv0", "long", 0);
static const std::vector<RoundTripTC> kARME0 = makeSwXform5TC("armswv0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform5, X64SwXformRT, ::testing::ValuesIn(kX64E), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5, X86SwXformRT, ::testing::ValuesIn(kX86E), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5, A64SwXformRT, ::testing::ValuesIn(kA64E), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5, ARM32SwXformRT, ::testing::ValuesIn(kARME), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5O0, X64SwXformRT, ::testing::ValuesIn(kX64E0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5O0, X86SwXformRT, ::testing::ValuesIn(kX86E0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5O0, A64SwXformRT, ::testing::ValuesIn(kA64E0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform5O0, ARM32SwXformRT, ::testing::ValuesIn(kARME0), rtTCName);

static const std::vector<RoundTripTC> kX64F = makeSwXform6TC("x64swu", "long", 2);
static const std::vector<RoundTripTC> kX86F = makeSwXform6TC("x86swu", "int", 2);
static const std::vector<RoundTripTC> kA64F = makeSwXform6TC("a64swu", "long", 2);
static const std::vector<RoundTripTC> kARMF = makeSwXform6TC("armswu", "int", 2);
static const std::vector<RoundTripTC> kX64F0 = makeSwXform6TC("x64swu0", "long", 0);
static const std::vector<RoundTripTC> kX86F0 = makeSwXform6TC("x86swu0", "int", 0);
static const std::vector<RoundTripTC> kA64F0 = makeSwXform6TC("a64swu0", "long", 0);
static const std::vector<RoundTripTC> kARMF0 = makeSwXform6TC("armswu0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform6, X64SwXformRT, ::testing::ValuesIn(kX64F), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6, X86SwXformRT, ::testing::ValuesIn(kX86F), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6, A64SwXformRT, ::testing::ValuesIn(kA64F), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6, ARM32SwXformRT, ::testing::ValuesIn(kARMF), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6O0, X64SwXformRT, ::testing::ValuesIn(kX64F0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6O0, X86SwXformRT, ::testing::ValuesIn(kX86F0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6O0, A64SwXformRT, ::testing::ValuesIn(kA64F0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform6O0, ARM32SwXformRT, ::testing::ValuesIn(kARMF0), rtTCName);

static const std::vector<RoundTripTC> kX64G = makeSwXform7TC("x64swt", "long", 2);
static const std::vector<RoundTripTC> kX86G = makeSwXform7TC("x86swt", "int", 2);
static const std::vector<RoundTripTC> kA64G = makeSwXform7TC("a64swt", "long", 2);
static const std::vector<RoundTripTC> kARMG = makeSwXform7TC("armswt", "int", 2);
static const std::vector<RoundTripTC> kX64G0 = makeSwXform7TC("x64swt0", "long", 0);
static const std::vector<RoundTripTC> kX86G0 = makeSwXform7TC("x86swt0", "int", 0);
static const std::vector<RoundTripTC> kA64G0 = makeSwXform7TC("a64swt0", "long", 0);
static const std::vector<RoundTripTC> kARMG0 = makeSwXform7TC("armswt0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform7, X64SwXformRT, ::testing::ValuesIn(kX64G), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7, X86SwXformRT, ::testing::ValuesIn(kX86G), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7, A64SwXformRT, ::testing::ValuesIn(kA64G), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7, ARM32SwXformRT, ::testing::ValuesIn(kARMG), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7O0, X64SwXformRT, ::testing::ValuesIn(kX64G0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7O0, X86SwXformRT, ::testing::ValuesIn(kX86G0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7O0, A64SwXformRT, ::testing::ValuesIn(kA64G0), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform7O0, ARM32SwXformRT, ::testing::ValuesIn(kARMG0), rtTCName);
