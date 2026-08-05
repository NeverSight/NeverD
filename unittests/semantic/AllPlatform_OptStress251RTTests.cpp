//===- AllPlatform_OptStress251RTTests.cpp - -O0 global/pointer forms ====//
//
// More read-only global access shapes at -O0, where i386/ARM32 spill the PIC GOT
// base (and any induction pointer) to the stack so the SSA-based base/string
// detectors that work at -O2 are stressed differently (the axis that surfaced
// the #507 GOTOFF double-reference and rodata-string-truncation bugs).  Covers
// reverse/negative index walks, genuine C-string induction walks, const pointer
// arrays into other globals, switch-to-string-pointer tables, and nested global
// structs.
//
//   * grev     - reverse-order table walk (base + (n-1-i)*stride).
//   * gstrwalk - genuine C-string length+hash via an induction pointer.
//   * gparr    - const pointer array whose elements point into other globals.
//   * gswstr   - switch returning const string pointers, hashed.
//   * gnested  - nested global struct with an inner array member.
//   * gcross   - two const tables read with cross indices.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O0.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress251RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress251RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress251RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress251RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress251RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress251RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress251RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress251RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress251TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Reverse-order table walk (base + (n-1-i)*stride): negative stride pointer.
    {p+"_grev",
     "static const unsigned RV[8]={2654435761u,40503u,2246822519u,3266489917u,668265263u,374761393u,3332679571u,2147483647u};\n"
     +t+" "+p+"_grev("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int rep=0;rep<24;rep++){ h=h*1103515245u+12345u;\n"
     "    for(int i=7;i>=0;i--) acc=acc*131u+RV[i]+h; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress251", 0},

    // Genuine C-string length + hash via an induction pointer (p walks to NUL).
    {p+"_gstrwalk",
     "static const char SW[]=\"the_quick_brown_fox_0123456789_jumps\";\n"
     +t+" "+p+"_gstrwalk("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int rep=0;rep<48;rep++){ h=h*1103515245u+12345u;\n"
     "    const char *p2=SW; unsigned hh=h;\n"
     "    while(*p2){ hh=hh*131u+(unsigned)(unsigned char)*p2; p2++; }\n"
     "    acc=acc*131u+hh+(unsigned)(p2-SW); }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress251", 0},

    // const pointer array whose elements point into other const globals.
    {p+"_gparr",
     "static const unsigned E0[3]={11u,22u,33u};\n"
     "static const unsigned E1[3]={101u,202u,303u};\n"
     "static const unsigned E2[3]={1001u,2002u,3003u};\n"
     "static const unsigned *const EP[3]={E0,E1,E2};\n"
     +t+" "+p+"_gparr("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned k=(h>>4)%3u, j=(h>>7)%3u; acc=acc*131u+EP[k][j]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress251", 0},

    // switch returning const string pointers, then hashed (jump table + .str).
    {p+"_gswstr",
     "static const char *pick(unsigned k){ switch(k&3u){\n"
     "  case 0: return \"alpha\"; case 1: return \"bravo\";\n"
     "  case 2: return \"charlie\"; default: return \"delta\"; } }\n"
     +t+" "+p+"_gswstr("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u; const char *s=pick(h>>6);\n"
     "    unsigned hh=0; while(*s){ hh=hh*131u+(unsigned)(unsigned char)*s; s++; }\n"
     "    acc=acc*131u+hh; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress251", 0},

    // Nested global struct with an inner array member.
    {p+"_gnested",
     "static const struct N{ unsigned id; unsigned vals[4]; } NS[3]={\n"
     "  {1,{10u,20u,30u,40u}},{2,{100u,200u,300u,400u}},{3,{1000u,2000u,3000u,4000u}}};\n"
     +t+" "+p+"_gnested("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<96;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned k=(h>>5)%3u, j=(h>>9)&3u;\n"
     "    acc=acc*131u+NS[k].id+NS[k].vals[j]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress251", 0},

    // Two const tables read with cross indices in one accumulator.
    {p+"_gcross",
     "static const unsigned CA[5]={3u,5u,7u,11u,13u};\n"
     "static const unsigned CB[5]={101u,103u,107u,109u,113u};\n"
     +t+" "+p+"_gcross("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned k=(h>>3)%5u, j=(h>>11)%5u;\n"
     "    acc=acc*131u+CA[k]*CB[j]+CB[k]+CA[j]; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress251", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress251TC("x64o251", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress251TC("x86o251", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress251TC("a64o251", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress251TC("armo251", "int");

INSTANTIATE_TEST_SUITE_P(OptStress251, X64OptStress251RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress251, X86OptStress251RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress251, A64OptStress251RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress251, ARM32OptStress251RT, ::testing::ValuesIn(kARM), rtTCName);
