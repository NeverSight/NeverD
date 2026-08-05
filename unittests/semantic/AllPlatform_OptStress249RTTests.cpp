//===- AllPlatform_OptStress249RTTests.cpp - global symbolize at -O0/-O1 =//
//
// The OptStress248 global/rodata symbolization patterns rerun at -O1 (and a few
// at -O0) as a sink differential.  Optimization level changes how clang
// materializes a global's base: -O2 under register pressure prefers an
// induction pointer (`p=&g; p+=stride`), while -O0/-O1 emit a fresh base load
// per access (i386 GOTOFF, ARM32 literal pool, x86-64 rip, AArch64 adrp+add).
// Each base shape is a separate symbolization path, so locking the same reads
// at a lower opt level guards the ones -O2 folding hides — the exact axis the
// "constant-pool mapping" / rodata-induction fixes live on.
//
//   * tbl2d1  - 2D const table walk            (-O1).
//   * pptr1   - const pointer array into globals(-O1).
//   * strsum1 - string-literal hash            (-O1).
//   * jtab1   - switch -> jump table           (-O1).
//   * gstruct0- const struct array gather      (-O0, explicit base loads).
//   * pwalk0  - induction pointer over a table (-O0).
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress249RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress249RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress249RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress249RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress249RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress249RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress249RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress249RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress249TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 2D const lookup table walk at -O1.
    {p+"_tbl2d1",
     "static const unsigned U2[4][4]={{2654435761u,40503u,2246822519u,3266489917u},\n"
     "  {668265263u,374761393u,3332679571u,2147483647u},\n"
     "  {97u,193u,389u,769u},{1543u,3079u,6151u,12289u}};\n"
     +t+" "+p+"_tbl2d1("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned r=(h>>5)&3u, c=(h>>9)&3u;\n"
     "    acc=acc*131u+U2[r][c]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress249", 1},

    // const pointer array into other globals at -O1.
    {p+"_pptr1",
     "static const unsigned B0[3]={11u,22u,33u};\n"
     "static const unsigned B1[3]={101u,202u,303u};\n"
     "static const unsigned B2[3]={1001u,2002u,3003u};\n"
     "static const unsigned *const QS[3]={B0,B1,B2};\n"
     +t+" "+p+"_pptr1("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned k=(h>>4)%3u, j=(h>>7)%3u;\n"
     "    acc=acc*131u+QS[k][j]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress249", 1},

    // string-literal hash at -O1.
    {p+"_strsum1",
     "static const char TXT[]=\"pack my box with five dozen liquor jugs 0123456789\";\n"
     +t+" "+p+"_strsum1("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  int n=(int)sizeof(TXT)-1;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    int idx=(int)((h>>3)%(unsigned)n);\n"
     "    acc=acc*131u+(unsigned)(unsigned char)TXT[idx]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress249", 1},

    // switch -> jump table at -O1.
    {p+"_jtab1",
     t+" "+p+"_jtab1("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned v=0; switch((h>>6)&7u){\n"
     "      case 0: v=h*3u+1u; break; case 1: v=h^0x55u; break;\n"
     "      case 2: v=h+97u; break; case 3: v=h*7u; break;\n"
     "      case 4: v=h>>2; break; case 5: v=h*131u+5u; break;\n"
     "      case 6: v=(h&0xffffu)*9u; break; default: v=~h; }\n"
     "    acc=acc*131u+v+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress249", 1},

    // const struct array gather at -O0 (explicit per-access base loads).
    {p+"_gstruct0",
     "struct Q{ unsigned char b; short s; int w; };\n"
     "static const struct Q QQ[5]={{1,1000,100000},{2,-2000,-200000},\n"
     "  {3,3000,300000},{255,-32768,-2000000000},{128,32767,2000000000}};\n"
     +t+" "+p+"_gstruct0("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<80;i++){ h=h*1103515245u+12345u;\n"
     "    int k=(int)((h>>8)%5u);\n"
     "    acc=acc*131u+(unsigned)QQ[k].b+(unsigned)(int)QQ[k].s\n"
     "        +(unsigned)QQ[k].w+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress249", 0},

    // induction pointer walking a const table at -O0.
    {p+"_pwalk0",
     "static const unsigned WK[8]={2654435761u,40503u,2246822519u,3266489917u,\n"
     "  668265263u,374761393u,3332679571u,2147483647u};\n"
     +t+" "+p+"_pwalk0("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int rep=0;rep<16;rep++){ h=h*1103515245u+12345u;\n"
     "    const unsigned *q=WK;\n"
     "    for(int i=0;i<8;i++){ acc=acc*131u+(*q)+h; q++; } }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress249", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress249TC("x64o249", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress249TC("x86o249", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress249TC("a64o249", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress249TC("armo249", "int");

INSTANTIATE_TEST_SUITE_P(OptStress249, X64OptStress249RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress249, X86OptStress249RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress249, A64OptStress249RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress249, ARM32OptStress249RT, ::testing::ValuesIn(kARM), rtTCName);
