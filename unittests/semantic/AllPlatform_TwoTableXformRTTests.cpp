//===- AllPlatform_TwoTableXformRTTests.cpp - adjacent PIC table -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Differential roundtrip probing of the shape the SwitchXform / CFGLoop suites
// deliberately left out (§15.2 adjacent-unguarded-pic-table-note): a single
// function body that holds *two or more* jump tables which, at -O2, each prove
// their index range and drop the `cmp` range guard — leaving two (or three)
// PC-relative jump tables laid out back-to-back in `.rodata`.  Their
// `R_X86_64_PC32` / `R_AARCH64_PREL32` entries then form one continuous run in
// the loader's RelCodeReloc slot set, so the resolver's run-length count over-
// reads the first table's entry count past its boundary and into the second
// (`countRelCodeRelocRun` in lib/ir/low/JumpTableResolver.cpp).  The first
// dispatch then latches bogus successor edges (each an entry of the *second*
// table decoded relative to the *first* base) → wrong arm → value mismatch or
// UC_ERR_WRITE_UNMAPPED.
//
// Each kernel keeps every dispatch index inside its switch's defined range (so
// the present-but-never-taken default's UB path never diverges the two runs)
// and folds to a single value-dependent integer return.  The fixture compiles
// the C natively, emulates it, lifts + recompiles with NeverD, emulates the
// result, and asserts the return values match bit-for-bit across all four
// targets at both -O0 (explicit guards) and -O2 (guard-eliminated raw tables).
//
// Shapes:
//   * twomachine  — two loop-carried state machines back-to-back, each a dense
//     unguarded dispatch on a small state var (the canonical two-table shape);
//   * threemachine — three back-to-back, so the run must be truncated twice
//     (table1 at table2's base, table2 at table3's base);
//   * twomodulo   — two adjacent `switch(x % N)` modulo tables (the modulus,
//     not a cmp, bounds each index → both tables unguarded and adjacent);
//   * splitmachine — one state machine whose dispatch is followed by an
//     independent dense switch on a different value, both unguarded.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64TwoTableRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64TwoTableRT, Verify) { roundTripX64(GetParam()); }
class X86TwoTableRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86TwoTableRT, Verify) { roundTripX86(GetParam()); }
class A64TwoTableRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64TwoTableRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32TwoTableRT : public SemanticRoundTripFixture,
                        public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32TwoTableRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeTwoTableTC(const char *prefix, const char *T,
                                               int Opt) {
  std::string p = prefix, t = T;
  return {
    // Two loop-carried state machines in one function.  Each `while(st) switch
    // (st)` is a dense dispatch on a state var only ever assigned a small case
    // label, so -O2 proves the range and drops the guard.  The two PIC tables
    // land adjacent in rodata: table1's run count must stop at table2's base or
    // the first machine dispatches on table2's (mis-based) entries.
    {p+"_twomachine",
     t+" "+p+"_twomachine("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  unsigned st=1; int g=0;\n"
     "  while(st!=0u && g++<4000){\n"
     "    switch(st){\n"
     "    case 1: x=x*1103515245u+12345u; h^=x; st=2; break;\n"
     "    case 2: h=h*31u+x; st=((x>>8)&1u)?3u:4u; break;\n"
     "    case 3: h+=x>>3; st=((x>>9)&1u)?4u:5u; break;\n"
     "    case 4: h^=x<<1; st=((x>>10)&1u)?2u:5u; break;\n"
     "    case 5: h-=x>>5; st=((h>>4)&7u)==0u?0u:1u; break;\n"
     "    default: st=0u; break;\n"
     "    }\n"
     "  }\n"
     "  unsigned st2=1; int g2=0;\n"
     "  while(st2!=0u && g2++<4000){\n"
     "    switch(st2){\n"
     "    case 1: x=x*22695477u+1u; h^=x; st2=2; break;\n"
     "    case 2: h=h*33u+x; st2=((x>>7)&1u)?3u:4u; break;\n"
     "    case 3: h+=x>>2; st2=((x>>11)&1u)?4u:5u; break;\n"
     "    case 4: h^=x<<2; st2=((x>>12)&1u)?2u:5u; break;\n"
     "    case 5: h-=x>>6; st2=((h>>3)&7u)==0u?0u:1u; break;\n"
     "    default: st2=0u; break;\n"
     "    }\n"
     "  }\n"
     "  return ("+t+")(unsigned long)(h+(unsigned)(g+g2)); }\n",
     {0x5AULL}, "TwoTable", Opt},

    // Three back-to-back state machines: the run truncation must fire twice, so
    // table1 stops at table2's base and table2 stops at table3's base (a single
    // truncation would still merge table2 into table3).
    {p+"_threemachine",
     t+" "+p+"_threemachine("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  unsigned st=1; int g=0;\n"
     "  while(st!=0u && g++<3000){\n"
     "    switch(st){\n"
     "    case 1: x=x*1103515245u+12345u; h^=x; st=2; break;\n"
     "    case 2: h=h*31u+x; st=((x>>8)&1u)?3u:4u; break;\n"
     "    case 3: h+=x>>3; st=((x>>9)&1u)?4u:5u; break;\n"
     "    case 4: h^=x<<1; st=((x>>10)&1u)?2u:5u; break;\n"
     "    case 5: h-=x>>5; st=((h>>4)&7u)==0u?0u:1u; break;\n"
     "    default: st=0u; break;\n"
     "    }\n"
     "  }\n"
     "  unsigned s2=1; int g2=0;\n"
     "  while(s2!=0u && g2++<3000){\n"
     "    switch(s2){\n"
     "    case 1: x=x*22695477u+1u; h^=x; s2=2; break;\n"
     "    case 2: h=h*33u+x; s2=((x>>7)&1u)?3u:4u; break;\n"
     "    case 3: h+=x>>2; s2=((x>>11)&1u)?4u:5u; break;\n"
     "    case 4: h^=x<<2; s2=((x>>12)&1u)?2u:5u; break;\n"
     "    case 5: h-=x>>6; s2=((h>>3)&7u)==0u?0u:1u; break;\n"
     "    default: s2=0u; break;\n"
     "    }\n"
     "  }\n"
     "  unsigned s3=1; int g3=0;\n"
     "  while(s3!=0u && g3++<3000){\n"
     "    switch(s3){\n"
     "    case 1: x=x*214013u+2531011u; h^=x; s3=2; break;\n"
     "    case 2: h=h*29u+x; s3=((x>>6)&1u)?3u:4u; break;\n"
     "    case 3: h+=x>>4; s3=((x>>13)&1u)?4u:5u; break;\n"
     "    case 4: h^=x<<3; s3=((x>>14)&1u)?2u:5u; break;\n"
     "    case 5: h-=x>>7; s3=((h>>5)&7u)==0u?0u:1u; break;\n"
     "    default: s3=0u; break;\n"
     "    }\n"
     "  }\n"
     "  return ("+t+")(unsigned long)(h+(unsigned)(g+g2+g3)); }\n",
     {0x3CULL}, "TwoTable", Opt},

    // Two adjacent `switch(x % N)` modulo tables in one loop: the modulus (not
    // a cmp) bounds each index, so both dispatches are unguarded and their PIC
    // tables sit adjacent in rodata — the run over-read applies to modulo
    // tables exactly as to state machines.
    {p+"_twomodulo",
     t+" "+p+"_twomodulo("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned k1=acc % 7u; unsigned s1;\n"
     "    switch(k1){\n"
     "    case 0: s1=0x9E3779B9u; break; case 1: s1=0x85EBCA6Bu; break;\n"
     "    case 2: s1=0xC2B2AE35u; break; case 3: s1=0x27D4EB2Fu; break;\n"
     "    case 4: s1=0x165667B1u; break; case 5: s1=0xD3A2646Cu; break;\n"
     "    case 6: s1=0xFD7046C5u; break; default: s1=1u; break;\n"
     "    }\n"
     "    acc=acc*s1 + (unsigned)i;\n"
     "    unsigned k2=acc % 6u; unsigned s2;\n"
     "    switch(k2){\n"
     "    case 0: s2=3u; break; case 1: s2=5u; break;\n"
     "    case 2: s2=7u; break; case 3: s2=11u; break;\n"
     "    case 4: s2=13u; break; case 5: s2=17u; break;\n"
     "    default: s2=1u; break;\n"
     "    }\n"
     "    acc=acc*s2 + 0x9E37u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x424242ULL}, "TwoTable", Opt},

    // State machine immediately followed by an independent dense switch on a
    // different value: the machine's back-edge table and the trailing switch's
    // table are both unguarded and adjacent, but the second is not loop-carried
    // (a distinct layout from twomachine's two loops).
    {p+"_splitmachine",
     t+" "+p+"_splitmachine("+t+" a){\n"
     "  unsigned x=(unsigned)a|1u; unsigned h=0;\n"
     "  unsigned st=1; int g=0;\n"
     "  while(st!=0u && g++<4000){\n"
     "    switch(st){\n"
     "    case 1: x=x*1103515245u+12345u; h^=x; st=2; break;\n"
     "    case 2: h=h*31u+x; st=((x>>8)&1u)?3u:4u; break;\n"
     "    case 3: h+=x>>3; st=((x>>9)&1u)?4u:5u; break;\n"
     "    case 4: h^=x<<1; st=((x>>10)&1u)?2u:5u; break;\n"
     "    case 5: h-=x>>5; st=((h>>4)&7u)==0u?0u:1u; break;\n"
     "    default: st=0u; break;\n"
     "    }\n"
     "  }\n"
     "  for(int i=0;i<64;i++){\n"
     "    unsigned k=(h>>(i&7))%8u;\n"
     "    switch(k){\n"
     "    case 0: h+=0x9E37u; break; case 1: h^=h<<5; break;\n"
     "    case 2: h*=3u; break;      case 3: h-=h>>3; break;\n"
     "    case 4: h+=0x1111u; break; case 5: h^=0x2222u; break;\n"
     "    case 6: h+=h<<2; break;    default: h^=0xA5u; break;\n"
     "    }\n"
     "    h+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)(h+(unsigned)g); }\n",
     {0x77ULL}, "TwoTable", Opt},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64O2 = makeTwoTableTC("x64tt", "long", 2);
static const std::vector<RoundTripTC> kX86O2 = makeTwoTableTC("x86tt", "int", 2);
static const std::vector<RoundTripTC> kA64O2 = makeTwoTableTC("a64tt", "long", 2);
static const std::vector<RoundTripTC> kARMO2 = makeTwoTableTC("armtt", "int", 2);
static const std::vector<RoundTripTC> kX64O0 = makeTwoTableTC("x64tt0", "long", 0);
static const std::vector<RoundTripTC> kX86O0 = makeTwoTableTC("x86tt0", "int", 0);
static const std::vector<RoundTripTC> kA64O0 = makeTwoTableTC("a64tt0", "long", 0);
static const std::vector<RoundTripTC> kARMO0 = makeTwoTableTC("armtt0", "int", 0);

INSTANTIATE_TEST_SUITE_P(TwoTable, X64TwoTableRT, ::testing::ValuesIn(kX64O2), rtTCName);
INSTANTIATE_TEST_SUITE_P(TwoTable, X86TwoTableRT, ::testing::ValuesIn(kX86O2), rtTCName);
INSTANTIATE_TEST_SUITE_P(TwoTable, A64TwoTableRT, ::testing::ValuesIn(kA64O2), rtTCName);
INSTANTIATE_TEST_SUITE_P(TwoTable, ARM32TwoTableRT, ::testing::ValuesIn(kARMO2), rtTCName);
INSTANTIATE_TEST_SUITE_P(TwoTableO0, X64TwoTableRT, ::testing::ValuesIn(kX64O0), rtTCName);
INSTANTIATE_TEST_SUITE_P(TwoTableO0, X86TwoTableRT, ::testing::ValuesIn(kX86O0), rtTCName);
INSTANTIATE_TEST_SUITE_P(TwoTableO0, A64TwoTableRT, ::testing::ValuesIn(kA64O0), rtTCName);
INSTANTIATE_TEST_SUITE_P(TwoTableO0, ARM32TwoTableRT, ::testing::ValuesIn(kARMO0), rtTCName);
