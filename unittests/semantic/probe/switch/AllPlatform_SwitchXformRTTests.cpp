//===- AllPlatform_SwitchXformRTTests.cpp - switch xform probing -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
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
// The suite is split across three TUs by dispatch shape.  This one carries the
// base-0 dense shapes, whose selector is a mask of the running value:
//   * 64-bit switch value with case labels above 2^32 (wide index recovery);
//   * jump-table-to-return: each arm returns a distinct constant (the RETURN
//     arm of lowerSwitchFromJumpTable, not an assign-to-r0 arm);
//   * Duff's device: a switch whose arms fall through into a do/while loop body
//     (the switch and the loop share blocks — hostile to CFG structuring);
//   * default label placed *between* case labels (mid-list default);
//   * two independent switches on the same value back-to-back (per-insn JT
//     keying);
//   * unreachable-default dense switch (clang drops the bound -> raw table);
//   * rodata value-table (LUT) lowering, a switch nested in an outer default
//     arm, a dense 20-way table, and a mid-bit (shift+mask) dispatch index.
//
// Sibling TUs of the same target: AllPlatform_SwitchXformSparseRTTests.cpp
// (sparse / signed / non-contiguous label sets) and
// AllPlatform_SwitchXformTableRTTests.cpp (large + compact tables, deep
// nesting / spilled indices, index provenance).
//
//===----------------------------------------------------------------------===//

#include "SwitchXformRTFixture.h"

TEST_P(X64SwXformRT, Verify) { roundTripX64(GetParam()); }
TEST_P(X86SwXformRT, Verify) { roundTripX86(GetParam()); }
TEST_P(A64SwXformRT, Verify) { roundTripAArch64(GetParam()); }
TEST_P(ARM32SwXformRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeSwXformTC(const char *prefix, const char *T,
                                              int Opt) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> Cases = {
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
  if (p == "armswx" && Opt == 2)
    for (RoundTripTC &TC : Cases)
      if (TC.Name == "armswx_twoswitch")
        TC.RecoveredSwitch = RecoveredSwitchExpectation::Required;
  return Cases;
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
// clang-format on

static const std::vector<RoundTripTC> kX64O2 =
    makeSwXformTC("x64swx", "long", 2);
static const std::vector<RoundTripTC> kX86O2 =
    makeSwXformTC("x86swx", "int", 2);
static const std::vector<RoundTripTC> kA64O2 =
    makeSwXformTC("a64swx", "long", 2);
static const std::vector<RoundTripTC> kARMO2 =
    makeSwXformTC("armswx", "int", 2);
static const std::vector<RoundTripTC> kX64O0 =
    makeSwXformTC("x64swx0", "long", 0);
static const std::vector<RoundTripTC> kX86O0 =
    makeSwXformTC("x86swx0", "int", 0);
static const std::vector<RoundTripTC> kA64O0 =
    makeSwXformTC("a64swx0", "long", 0);
static const std::vector<RoundTripTC> kARMO0 =
    makeSwXformTC("armswx0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform, X64SwXformRT, ::testing::ValuesIn(kX64O2),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform, X86SwXformRT, ::testing::ValuesIn(kX86O2),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform, A64SwXformRT, ::testing::ValuesIn(kA64O2),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform, ARM32SwXformRT, ::testing::ValuesIn(kARMO2),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXformO0, X64SwXformRT, ::testing::ValuesIn(kX64O0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXformO0, X86SwXformRT, ::testing::ValuesIn(kX86O0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXformO0, A64SwXformRT, ::testing::ValuesIn(kA64O0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXformO0, ARM32SwXformRT, ::testing::ValuesIn(kARMO0),
                         rtTCName);

static const std::vector<RoundTripTC> kX64W = makeSw64TC("x64swx");
static const std::vector<RoundTripTC> kA64W = makeSw64TC("a64swx");
INSTANTIATE_TEST_SUITE_P(SwXform64, X64SwXformRT, ::testing::ValuesIn(kX64W),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform64, A64SwXformRT, ::testing::ValuesIn(kA64W),
                         rtTCName);

static const std::vector<RoundTripTC> kX64B =
    makeSwXform2TC("x64swy", "long", 2);
static const std::vector<RoundTripTC> kX86B =
    makeSwXform2TC("x86swy", "int", 2);
static const std::vector<RoundTripTC> kA64B =
    makeSwXform2TC("a64swy", "long", 2);
static const std::vector<RoundTripTC> kARMB =
    makeSwXform2TC("armswy", "int", 2);
static const std::vector<RoundTripTC> kX64B0 =
    makeSwXform2TC("x64swy0", "long", 0);
static const std::vector<RoundTripTC> kX86B0 =
    makeSwXform2TC("x86swy0", "int", 0);
static const std::vector<RoundTripTC> kA64B0 =
    makeSwXform2TC("a64swy0", "long", 0);
static const std::vector<RoundTripTC> kARMB0 =
    makeSwXform2TC("armswy0", "int", 0);

INSTANTIATE_TEST_SUITE_P(SwXform2, X64SwXformRT, ::testing::ValuesIn(kX64B),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2, X86SwXformRT, ::testing::ValuesIn(kX86B),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2, A64SwXformRT, ::testing::ValuesIn(kA64B),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2, ARM32SwXformRT, ::testing::ValuesIn(kARMB),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2O0, X64SwXformRT, ::testing::ValuesIn(kX64B0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2O0, X86SwXformRT, ::testing::ValuesIn(kX86B0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2O0, A64SwXformRT, ::testing::ValuesIn(kA64B0),
                         rtTCName);
INSTANTIATE_TEST_SUITE_P(SwXform2O0, ARM32SwXformRT,
                         ::testing::ValuesIn(kARMB0), rtTCName);
