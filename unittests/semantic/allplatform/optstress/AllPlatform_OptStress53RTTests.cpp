//===- AllPlatform_OptStress53RTTests.cpp - rodata index variants -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Follow-on to #457–#461 constant-pool coverage: these stress rodata table
// indexing shapes beyond a plain `T[i]` — a non-zero table base (`&T[k]`),
// runtime-scrambled indices, reverse / modulo walks, and chained byte shuffles.
// Each must still redirect the table segment while keeping index arithmetic
// as genuine integers (no collision with relocation-target VAs).
//
//   * subtab  - index through `&T[4]` (base inside the segment, not at start).
//   * offtab  - `T[(i + run_off) & 15]` with a data-dependent offset.
//   * revtab  - reverse walk `T[15 - (i & 15)]`.
//   * modtab  - non-power-of-2 modulo index into a 13-element table.
//   * shuf2   - byte permutation table selects index into a second table.
//   * runbase - loop-carried running offset wraps into table bounds.
//
// All integer, arrays seed from the LCG, fold to one return, no float /
// 64-bit divide helper.  All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress53RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress53RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress53RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress53RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress53RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress53RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress53RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress53RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress53TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Index through a pointer into the middle of the table.
    {p+"_subtab",
     "static const unsigned T[16]={11,22,33,44,55,66,77,88,"
     "99,10,20,30,40,50,60,70};\n"
     +t+" "+p+"_subtab("+t+" a){\n"
     "  const unsigned *p=&T[4]; unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<180;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>5)&11u; h=h*131u+p[j]+T[(j+3)&15u]; h^=h>>9; }\n"
     "  return ("+t+")h; }\n",
     {0x61u}, "OptStress53", 2},

    // Runtime offset added to the index before masking.
    {p+"_offtab",
     "static const unsigned T[16]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};\n"
     +t+" "+p+"_offtab("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, off=0, h=0;\n"
     "  for(int i=0;i<170;i++){ s=s*1103515245u+12345u;\n"
     "    off=(off+(s>>7))&15u; unsigned j=((s>>4)+off)&15u;\n"
     "    h=h*131u+T[j]+T[(j+off)&15u]; h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0x62u}, "OptStress53", 2},

    // Reverse indexing from the table end.
    {p+"_revtab",
     "static const unsigned T[16]={101,202,303,404,505,606,707,808,"
     "909,111,222,333,444,555,666,777};\n"
     +t+" "+p+"_revtab("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>6)&15u; h=h*131u+T[15-j]+T[(15-j+1)&15u]; }\n"
     "  return ("+t+")h; }\n",
     {0x63u}, "OptStress53", 2},

    // Non-power-of-2 modulo index into a 13-element table.
    {p+"_modtab",
     "static const unsigned T[13]={2,3,5,7,11,13,17,19,23,29,31,37,41};\n"
     +t+" "+p+"_modtab("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<190;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=((s>>5)*7u+3u)%13u; h=h*131u+T[j]+T[(j+5)%13u]; h^=h>>7; }\n"
     "  return ("+t+")h; }\n",
     {0x64u}, "OptStress53", 2},

    // Byte shuffle table picks index into a second table.
    {p+"_shuf2",
     "static const unsigned char P[16]={5,2,9,0,13,7,1,14,"
     "3,11,6,8,15,4,10,12};\n"
     "static const unsigned T[16]={17,29,31,37,41,43,47,53,"
     "59,61,67,71,73,79,83,89};\n"
     +t+" "+p+"_shuf2("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>5)&15u; unsigned k=P[j]&15u;\n"
     "    h=h*131u+T[k]+T[P[(k+1)&15u]&15u]; h^=h>>13; }\n"
     "  return ("+t+")h; }\n",
     {0x65u}, "OptStress53", 2},

    // Loop-carried running offset wraps into table bounds.
    {p+"_runbase",
     "static const unsigned T[16]={6,12,18,24,30,36,42,48,"
     "54,60,66,72,78,84,90,96};\n"
     +t+" "+p+"_runbase("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, base=0, h=0;\n"
     "  for(int i=0;i<175;i++){ s=s*1103515245u+12345u;\n"
     "    base=(base+(s>>9))&15u; unsigned j=(base+(s>>4))&15u;\n"
     "    h=h*131u+T[j]+T[(base+j)&15u]; base=(base+T[j])&15u; }\n"
     "  return ("+t+")h; }\n",
     {0x66u}, "OptStress53", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress53TC("x64o53", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress53TC("x86o53", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress53TC("a64o53", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress53TC("armo53", "int");

INSTANTIATE_TEST_SUITE_P(OptStress53, X64OptStress53RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress53, X86OptStress53RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress53, A64OptStress53RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress53, ARM32OptStress53RT, ::testing::ValuesIn(kARM), rtTCName);
