//===- AllPlatform_RodataHoist3RTTests.cpp - struct/3D rodata tables -*- C++ -=//
//
// Deeper edge cases for hoisted-rodata-table redirection.  After #374 handled a
// base nested under multi-dimensional indexing, these stress address trees that
// also carry a *constant* addend (a struct field offset) and triple nesting:
//   * array-of-struct  tab[i].field  -> base + i*stride + field_off (a small
//     constant the decomposer must NOT mistake for the table base)
//   * two fields of the same struct table (offsets 0 and 8)
//   * 3-D table tab[i][j][k]         -> base + i*s0 + j*s1 + k
//   * non-power-of-2 element stride (12-byte struct)
// A regression degrades the table read to an absolute inttoptr that reads
// unmapped memory in the recompiled image (table reads 0/garbage).
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RodataHoist3RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RodataHoist3RT, Verify) { roundTripX64(GetParam()); }

class A64RodataHoist3RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64RodataHoist3RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32RodataHoist3RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32RodataHoist3RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeTC(const char *prefix, const char *T,
                                       int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // array-of-struct, one field: base + i*12 + 4 (constant field offset must
    // not be taken as the base).  Stack array store poisons the guard.
    {p+"_structfield",
     t+" "+p+"_structfield("+t+" a){\n"
     "  unsigned char in[40];\n"
     "  for(int i=0;i<40;i++) in[i]=(unsigned char)((a+i*5)>>1);\n"
     "  struct S{unsigned x,y,z;};\n"
     "  static const struct S tab[8]={{1,2,3},{4,5,6},{7,8,9},{10,11,12},\n"
     "    {13,14,15},{16,17,18},{19,20,21},{22,23,24}};\n"
     "  unsigned long acc=0;\n"
     "  for(int i=0;i<40;i++) acc=acc*131u+tab[in[i]&7].y;\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x13579BDULL}, "RodataHoist3", opt, fl},

    // two fields of the same struct table (offsets 0 and 8).
    {p+"_structtwo",
     t+" "+p+"_structtwo("+t+" a){\n"
     "  unsigned char in[40];\n"
     "  for(int i=0;i<40;i++) in[i]=(unsigned char)((a*(i+1))>>2);\n"
     "  struct S{unsigned x,y,z;};\n"
     "  static const struct S tab[8]={{1,2,3},{4,5,6},{7,8,9},{10,11,12},\n"
     "    {13,14,15},{16,17,18},{19,20,21},{22,23,24}};\n"
     "  unsigned long acc=0;\n"
     "  for(int i=0;i<40;i++){ unsigned k=in[i]&7;\n"
     "    acc=acc*131u+tab[k].x*3u+tab[k].z; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2468ACEULL}, "RodataHoist3", opt, fl},

    // 3-D table tab[4][4][4]: base + i*64 + j*16 + k (triple nesting).
    {p+"_tab3d",
     t+" "+p+"_tab3d("+t+" a){\n"
     "  unsigned char in[40];\n"
     "  for(int i=0;i<40;i++) in[i]=(unsigned char)((a+i*7)>>1);\n"
     "  static const unsigned char tab[4][4][4]={\n"
     "    {{1,2,3,4},{5,6,7,8},{9,10,11,12},{13,14,15,16}},\n"
     "    {{17,18,19,20},{21,22,23,24},{25,26,27,28},{29,30,31,32}},\n"
     "    {{33,34,35,36},{37,38,39,40},{41,42,43,44},{45,46,47,48}},\n"
     "    {{49,50,51,52},{53,54,55,56},{57,58,59,60},{61,62,63,64}}};\n"
     "  unsigned long acc=0;\n"
     "  for(int n=0;n<40;n++){ unsigned v=in[n]; unsigned i=v&3,j=(v>>2)&3,k=(v>>4)&3;\n"
     "    acc=acc*131u+tab[i][j][k]; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "RodataHoist3", opt, fl},

    // array-of-struct with a 2-int (8-byte) struct: base + i*8 + 4.
    {p+"_struct8",
     t+" "+p+"_struct8("+t+" a){\n"
     "  unsigned char in[40];\n"
     "  for(int i=0;i<40;i++) in[i]=(unsigned char)((a*(i+2))>>2);\n"
     "  struct P{unsigned lo,hi;};\n"
     "  static const struct P tab[8]={{10,11},{20,21},{30,31},{40,41},\n"
     "    {50,51},{60,61},{70,71},{80,81}};\n"
     "  unsigned long acc=0;\n"
     "  for(int i=0;i<40;i++){ unsigned k=in[i]&7;\n"
     "    acc=acc*131u+tab[k].hi-tab[k].lo; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x55AA33CULL}, "RodataHoist3", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeTC("x64rh3", "long", 2, "");
static const std::vector<RoundTripTC> kA64 = makeTC("a64rh3", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeTC("armrh3", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(RodataHoist3, X64RodataHoist3RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(RodataHoist3, A64RodataHoist3RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(RodataHoist3, ARM32RodataHoist3RT,
                         ::testing::ValuesIn(kARM), rtTCName);
