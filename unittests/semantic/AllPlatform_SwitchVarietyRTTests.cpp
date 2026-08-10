//===- AllPlatform_SwitchVarietyRTTests.cpp - switch lowering forms -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Roundtrip probing of the many shapes clang lowers a `switch` into, beyond the
// dense PC-relative word table the SwitchTable / SwitchDispatch / SwitchModulo
// suites already cover.  These stress the jump-table resolver and CFG builder on
// forms that recent rounds (#398/#400/#403) showed are the live bug area:
// sparse cases (binary-search / if-chain), fall-through arms with no break,
// signed switches with negative case labels, GCC case-range labels, a switch
// nested inside another switch, and duplicate case bodies clang folds to a
// shared target.  Each kernel takes one scalar arg and returns a value-dependent
// hash compiled at -O2; the roundtrip compares native vs lifted across targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64SwVarRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64SwVarRT, Verify) { roundTripX64(GetParam()); }
class X86SwVarRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86SwVarRT, Verify) { roundTripX86(GetParam()); }
class A64SwVarRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64SwVarRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32SwVarRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32SwVarRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeSwVarTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Sparse cases far apart — clang emits a binary-search / if-chain (not a
    // jump table) since a dense table would be enormous.
    {p+"_sparse",
     t+" "+p+"_sparse("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<80;i++){\n"
     "    unsigned k=acc & 0x3FFFu;\n"
     "    switch(k){\n"
     "    case 0: acc+=0x9E3779B9u; break;\n"
     "    case 7: acc^=acc<<13; break;\n"
     "    case 100: acc*=2654435761u; break;\n"
     "    case 999: acc-=acc>>5; break;\n"
     "    case 4096: acc=(acc<<7)|(acc>>25); break;\n"
     "    case 8191: acc+=0x85EBCA6Bu; break;\n"
     "    case 12345: acc^=acc>>17; break;\n"
     "    default: acc=acc*31u+(unsigned)i; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x1234ULL}, "SwVar", 2},

    // Fall-through arms (no break) accumulate across consecutive cases — the CFG
    // builder must keep the fall edges, not synthesize a break.
    {p+"_fallthru",
     t+" "+p+"_fallthru("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<96;i++){\n"
     "    unsigned k=acc & 7u; unsigned s=0;\n"
     "    switch(k){\n"
     "    case 0: s+=1u;\n"
     "    case 1: s+=10u;\n"
     "    case 2: s+=100u;\n"
     "    case 3: s+=1000u; break;\n"
     "    case 4: s+=2u;\n"
     "    case 5: s+=20u; break;\n"
     "    default: s+=7u;\n"
     "    }\n"
     "    acc=acc*2654435761u + s + (unsigned)i;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x55ULL}, "SwVar", 2},

    // Signed switch with negative case labels — the resolver's normalization must
    // handle a negative base (case -3 maps to table index 0).
    {p+"_signed",
     t+" "+p+"_signed("+t+" a){\n"
     "  int acc=(int)a|1;\n"
     "  for(int i=0;i<96;i++){\n"
     "    int k=(int)((acc>>3) % 7) - 3;\n"  // -3..3
     "    switch(k){\n"
     "    case -3: acc+=0x9E37u; break;\n"
     "    case -2: acc^=acc<<11; break;\n"
     "    case -1: acc-=12345; break;\n"
     "    case 0:  acc=(acc<<5)|((unsigned)acc>>27); break;\n"
     "    case 1:  acc*=131; break;\n"
     "    case 2:  acc^=acc>>9; break;\n"
     "    case 3:  acc+=i*7; break;\n"
     "    }\n"
     "    acc+=i*3;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)(unsigned)acc; }\n",
     {0x9ULL}, "SwVar", 2},

    // GCC case-range labels collapse contiguous spans to one arm — clang lowers
    // the span to a range check the resolver must not mistake for a table bound.
    {p+"_ranges",
     t+" "+p+"_ranges("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<96;i++){\n"
     "    unsigned k=acc & 31u;\n"
     "    switch(k){\n"
     "    case 0 ... 7:   acc+=0x9E3779B9u; break;\n"
     "    case 8 ... 15:  acc^=acc<<13; break;\n"
     "    case 16 ... 23: acc*=2654435761u; break;\n"
     "    case 24 ... 30: acc-=acc>>5; break;\n"
     "    default:        acc^=0xA5A5A5A5u; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x77ULL}, "SwVar", 2},

    // A switch nested inside another switch — two jump tables in one function,
    // each must be resolved independently with the right bound.
    {p+"_nested",
     t+" "+p+"_nested("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<96;i++){\n"
     "    switch(acc & 3u){\n"
     "    case 0:\n"
     "      switch((acc>>2)&3u){\n"
     "      case 0: acc+=1u; break; case 1: acc+=2u; break;\n"
     "      case 2: acc+=4u; break; default: acc+=8u; break; }\n"
     "      acc^=0x11u; break;\n"
     "    case 1:\n"
     "      switch((acc>>4)&3u){\n"
     "      case 0: acc*=3u; break; case 1: acc*=5u; break;\n"
     "      case 2: acc*=7u; break; default: acc*=11u; break; }\n"
     "      acc^=0x22u; break;\n"
     "    case 2: acc=(acc<<3)|(acc>>29); break;\n"
     "    default: acc-=0x9E3779B9u; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x3ULL}, "SwVar", 2},

    // Duplicate case bodies — clang folds identical arms to one shared target,
    // producing a jump table with repeated entries the resolver must keep.
    {p+"_dupbody",
     t+" "+p+"_dupbody("+t+" a){\n"
     "  unsigned acc=(unsigned)a|1u;\n"
     "  for(int i=0;i<96;i++){\n"
     "    unsigned k=acc & 15u;\n"
     "    switch(k){\n"
     "    case 0: case 3: case 6: case 9:  acc+=0x9E3779B9u; break;\n"
     "    case 1: case 4: case 7: case 10: acc^=acc<<13; break;\n"
     "    case 2: case 5: case 8: case 11: acc*=2654435761u; break;\n"
     "    default: acc-=acc>>5; break;\n"
     "    }\n"
     "    acc+=(unsigned)i*131u;\n"
     "  }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0xBEEFULL}, "SwVar", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeSwVarTC("x64swv", "long");
static const std::vector<RoundTripTC> kX86 = makeSwVarTC("x86swv", "int");
static const std::vector<RoundTripTC> kA64 = makeSwVarTC("a64swv", "long");
static const std::vector<RoundTripTC> kARM = makeSwVarTC("armswv", "int");

INSTANTIATE_TEST_SUITE_P(SwVar, X64SwVarRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwVar, X86SwVarRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwVar, A64SwVarRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(SwVar, ARM32SwVarRT, ::testing::ValuesIn(kARM), rtTCName);
