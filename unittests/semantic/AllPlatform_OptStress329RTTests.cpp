//===- AllPlatform_OptStress329RTTests.cpp - computed-goto edge variants -===//
//
// Hardens the #530 pre-scaled computed-goto table recovery against edge shapes
// beyond the plain loop: case bodies with a noinline call that clobbers the
// index register, an index carrying an offset inside the mask (`((w>>k)+c)&7`),
// a 32-way table (wider pre-scaled mask), and two goto sites sharing one label
// table.  Each still resolves on the relocation-run signature.
//
// A runtime-SELECTED table base (`tbl = cond ? A : B; goto *tbl[idx]`) is a
// distinct deferred limitation — the base register is a CMOV/select of two
// constant tables, so it cannot fold to one table address; correct recovery is
// a 16-target two-table indirect dispatch that needs indirectbr / synthetic-
// selector modelling (precise root cause in todo.md).  It is NOT probed here, on
// all platforms / both scale forms, to keep the suite green.
//
// Integer in / integer out, function-local rodata label tables, LCG-seeded,
// folded single return; 32-bit targets stay libcall-free.  All four targets,
// mixed -O2 / -Os / -Oz.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress329RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress329RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress329RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress329RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress329RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress329RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress329RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress329RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress329TC(const char *prefix,
                                                   const char *T) {
  std::string p = prefix, t = T;
  std::vector<RoundTripTC> v = {
    // Case bodies call a noinline helper that clobbers the index register.
    {p+"_cgcall",
     "static int "+p+"_h(int x,int y) __attribute__((noinline));\n"
     +t+" "+p+"_cgcall("+t+" a){\n"
     "  static const void *const lab[8]={&&L0,&&L1,&&L2,&&L3,&&L4,&&L5,&&L6,&&L7};\n"
     "  unsigned w=(unsigned)a^0x33u; long long acc=(unsigned)a;\n"
     "  for(int i=0;i<64;i++){ w=w*22695477u+1u; int x=(int)w;\n"
     "    goto *lab[(w>>4)&7];\n"
     "    L0: acc+="+p+"_h(x,i); goto C; L1: acc-="+p+"_h(x,3); goto C;\n"
     "    L2: acc^=(long long)"+p+"_h(x>>2,x); goto C;\n"
     "    L3: acc+=(long long)"+p+"_h(x,x)*2; goto C;\n"
     "    L4: acc-=(long long)(int)w; goto C; L5: acc^=(long long)w<<5; goto C;\n"
     "    L6: acc+="+p+"_h(i,x); goto C; L7: acc+=w&0x7f; goto C;\n"
     "    C: acc^=acc>>19; }\n"
     "  return ("+t+")(acc ^ (acc>>32)); }\n"
     "static int "+p+"_h(int x,int y){ return x*31 + y*7 - (x^y); }\n",
     {0x2345u}, "OptStress329", 2, "-Oz"},

    // Index carries an offset inside the mask: `((w>>4)+5)&7` — the masked value
    // is still the terminal index, so recovery must not invert past the mask.
    {p+"_cgoff",
     t+" "+p+"_cgoff("+t+" a){\n"
     "  static const void *const lab[8]={&&L0,&&L1,&&L2,&&L3,&&L4,&&L5,&&L6,&&L7};\n"
     "  unsigned w=(unsigned)a+0x9u, acc=0;\n"
     "  for(int i=0;i<80;i++){ w=w*1664525u+1013904223u;\n"
     "    goto *lab[((w>>4)+5u)&7];\n"
     "    L0: acc+=w; goto C; L1: acc-=w; goto C; L2: acc^=w; goto C;\n"
     "    L3: acc=~acc; goto C; L4: acc+=w*5u; goto C; L5: acc^=w<<3; goto C;\n"
     "    L6: acc-=w>>1; goto C; L7: acc+=w&0x3f; goto C;\n"
     "    C: acc^=acc>>11; }\n"
     "  return ("+t+")acc; }\n",
     {0x3456u}, "OptStress329", 2, "-Os"},

    // 32-way table — a wider pre-scaled mask ((2^5-1)<<k).
    {p+"_cg32",
     t+" "+p+"_cg32("+t+" a){\n"
     "  static const void *const L[32]={\n"
     "   &&N0,&&N1,&&N2,&&N3,&&N4,&&N5,&&N6,&&N7,&&N8,&&N9,&&NA,&&NB,&&NC,&&ND,&&NE,&&NF,\n"
     "   &&N10,&&N11,&&N12,&&N13,&&N14,&&N15,&&N16,&&N17,&&N18,&&N19,&&N1A,&&N1B,&&N1C,&&N1D,&&N1E,&&N1F};\n"
     "  unsigned w=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<96;i++){ w=w*1103515245u+12345u;\n"
     "    goto *L[(w>>3)&31];\n"
     "    N0:acc+=w;goto C;N1:acc-=w;goto C;N2:acc^=w;goto C;N3:acc+=w*3u;goto C;\n"
     "    N4:acc-=w*5u;goto C;N5:acc^=w<<2;goto C;N6:acc+=w>>1;goto C;N7:acc-=w>>2;goto C;\n"
     "    N8:acc+=w*9u;goto C;N9:acc^=w<<4;goto C;NA:acc-=w&0xff;goto C;NB:acc+=w^0x5a;goto C;\n"
     "    NC:acc^=w>>3;goto C;ND:acc-=w*7u;goto C;NE:acc+=w&7;goto C;NF:acc=~acc;goto C;\n"
     "    N10:acc+=w*13u;goto C;N11:acc^=w<<5;goto C;N12:acc-=w>>4;goto C;N13:acc+=w*17u;goto C;\n"
     "    N14:acc^=w*19u;goto C;N15:acc-=w<<1;goto C;N16:acc+=w>>5;goto C;N17:acc^=w&0x3f;goto C;\n"
     "    N18:acc+=w*23u;goto C;N19:acc-=w^0x33;goto C;N1A:acc^=w<<6;goto C;N1B:acc+=w>>6;goto C;\n"
     "    N1C:acc-=w*29u;goto C;N1D:acc+=w&0x1f;goto C;N1E:acc^=w*31u;goto C;N1F:acc-=w>>7;goto C;\n"
     "    C: acc^=acc>>13; }\n"
     "  return ("+t+")acc; }\n",
     {0x4567u}, "OptStress329", 2, "-O2"},

    // Two goto sites share one label table (the table base + reloc run is reused
    // by both dispatches).
    {p+"_cgshare",
     t+" "+p+"_cgshare("+t+" a){\n"
     "  static const void *const lab[8]={&&L0,&&L1,&&L2,&&L3,&&L4,&&L5,&&L6,&&L7};\n"
     "  unsigned w=(unsigned)a|1u, acc=0;\n"
     "  for(int i=0;i<80;i++){ w=w*214013u+2531011u;\n"
     "    if(i&1) goto *lab[(w>>6)&7]; else goto *lab[(w>>9)&7];\n"
     "    L0: acc+=w; goto C; L1: acc-=w; goto C; L2: acc^=w; goto C;\n"
     "    L3: acc+=w*3u; goto C; L4: acc-=w*5u; goto C; L5: acc^=w<<2; goto C;\n"
     "    L6: acc+=w>>1; goto C; L7: acc-=w>>2; goto C;\n"
     "    C: acc^=acc>>7; }\n"
     "  return ("+t+")acc; }\n",
     {0x5678u}, "OptStress329", 2, "-O2"},
  };
  return v;
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress329TC("x64o329", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress329TC("x86o329", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress329TC("a64o329", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress329TC("armo329", "int");

INSTANTIATE_TEST_SUITE_P(OptStress329, X64OptStress329RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress329, X86OptStress329RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress329, A64OptStress329RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress329, ARM32OptStress329RT, ::testing::ValuesIn(kARM), rtTCName);
