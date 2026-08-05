//===- AllPlatform_OptStress248RTTests.cpp - global / rodata symbolize ===//
//
// Pointer/global symbolization patterns adjacent to the #473-476 fixes (global
// addresses stored into pointer arrays, escaping/returned global pointers).
// These read globals through shapes the lift must recognize as pointers-into-
// rodata/data and redirect onto the embedded image rather than the original VA:
//   * 2D const table indexed by computed row/col (base + i*stride + j).
//   * const pointer array whose elements point INTO other globals (ps[k] = &g).
//   * string-literal byte processing (char rodata walked by index).
//   * switch dispatch -> jump table over a const block.
//   * const struct array with mixed-width fields gathered by index.
// The roundtrip links + packs every .rodata/.data section, so a mis-symbolized
// base reads stale/unmapped memory and shows up as a return mismatch.
//
//   * tbl2d   - 2D const lookup table walk.
//   * pptr    - const pointer array into other globals.
//   * strsum  - string-literal hash.
//   * jtab    - switch -> jump table over a const block.
//   * gstruct - const struct array gather.
//   * gmix    - const table + pointer array combined.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress248RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress248RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress248RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress248RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress248RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress248RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress248RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress248RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress248TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 2D const lookup table walked by computed row/col indices.
    {p+"_tbl2d",
     "static const unsigned T2[4][4]={{2654435761u,40503u,2246822519u,3266489917u},\n"
     "  {668265263u,374761393u,3332679571u,2147483647u},\n"
     "  {97u,193u,389u,769u},{1543u,3079u,6151u,12289u}};\n"
     +t+" "+p+"_tbl2d("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned r=(h>>5)&3u, c=(h>>9)&3u;\n"
     "    acc=acc*131u+T2[r][c]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress248", 2},

    // const pointer array whose elements point into other const globals.
    {p+"_pptr",
     "static const unsigned A0[3]={11u,22u,33u};\n"
     "static const unsigned A1[3]={101u,202u,303u};\n"
     "static const unsigned A2[3]={1001u,2002u,3003u};\n"
     "static const unsigned *const PS[3]={A0,A1,A2};\n"
     +t+" "+p+"_pptr("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned k=(h>>4)%3u, j=(h>>7)%3u;\n"
     "    acc=acc*131u+PS[k][j]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress248", 2},

    // string-literal byte processing (char rodata walked by index).
    {p+"_strsum",
     "static const char MSG[]=\"The quick brown fox jumps over 13 lazy dogs!\";\n"
     +t+" "+p+"_strsum("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  int n=(int)sizeof(MSG)-1;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int idx=(int)((h>>3)%(unsigned)n);\n"
     "    acc=acc*131u+(unsigned)(unsigned char)MSG[idx]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress248", 2},

    // switch dispatch -> jump table over a const block.
    {p+"_jtab",
     t+" "+p+"_jtab("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned v=0; switch((h>>6)&7u){\n"
     "      case 0: v=h*3u+1u; break; case 1: v=h^0x55u; break;\n"
     "      case 2: v=h+97u; break; case 3: v=h*7u; break;\n"
     "      case 4: v=h>>2; break; case 5: v=h*131u+5u; break;\n"
     "      case 6: v=(h&0xffffu)*9u; break; default: v=~h; }\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress248", 2},

    // const struct array with mixed-width fields gathered by index.
    {p+"_gstruct",
     "struct R{ unsigned char b; short s; int w; };\n"
     "static const struct R RS[5]={{1,1000,100000},{2,-2000,-200000},\n"
     "  {3,3000,300000},{255,-32768,-2000000000},{128,32767,2000000000}};\n"
     +t+" "+p+"_gstruct("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<80;i++){ h=h*1103515245u+12345u;\n"
     "    int k=(int)((h>>8)%5u);\n"
     "    acc=acc*131u+(unsigned)RS[k].b+(unsigned)(int)RS[k].s\n"
     "        +(unsigned)RS[k].w+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress248", 2},

    // const table + pointer array combined in one accumulator.
    {p+"_gmix",
     "static const unsigned TT[6]={9u,99u,999u,9999u,99999u,999999u};\n"
     "static const unsigned *const PP=TT;\n"
     +t+" "+p+"_gmix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned k=(h>>3)%6u; acc=acc*131u+TT[k]+PP[(k+1u)%6u]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress248", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress248TC("x64o248", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress248TC("x86o248", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress248TC("a64o248", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress248TC("armo248", "int");

INSTANTIATE_TEST_SUITE_P(OptStress248, X64OptStress248RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress248, X86OptStress248RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress248, A64OptStress248RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress248, ARM32OptStress248RT, ::testing::ValuesIn(kARM), rtTCName);
