//===- AllPlatform_RodataHoist2RTTests.cpp - rodata table edges --*- C++ -*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Follow-on edge cases for the #372 hoisted-rodata-table redirection
// (`tryResolveIndexedGlobalPtr` + `isReadOnlyDataSymbol`).  The original probes
// covered 1-byte and 4-byte element tables; these stress element/index shapes
// whose GEP scaling the redirect must get right while a stack array's stores
// still poison the StoredConstBases guard (so the symbol-keyed bypass is the
// only thing keeping the table redirected):
//   * 8-byte (qword) element table          -> index scaled by 8
//   * 2-D const table  tbl[r][c]            -> base + row*stride + col
//   * const table read at a *computed* (subtracted) index -> base + (k-j)
//   * two element widths (byte + qword) hoisted in one loop
// A regression (table read degrades to inttoptr<abs> in the recompiled image)
// reads 0/garbage and diverges from the native run.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64RodataHoist2RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64RodataHoist2RT, Verify) { roundTripX64(GetParam()); }

class A64RodataHoist2RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64RodataHoist2RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32RodataHoist2RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32RodataHoist2RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeTC(const char *prefix, const char *T,
                                       int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // 8-byte element table: index scaled by 8.  Stack array store poisons guard.
    {p+"_qwtab",
     t+" "+p+"_qwtab("+t+" a){\n"
     "  unsigned char in[40];\n"
     "  for(int i=0;i<40;i++) in[i]=(unsigned char)((a+i*5)>>1);\n"
     "  static const unsigned long long tab[8]={\n"
     "    0x1111111111111111ULL,0x2222222222222222ULL,0x3333333333333333ULL,\n"
     "    0x4444444444444444ULL,0x5555555555555555ULL,0x6666666666666666ULL,\n"
     "    0x7777777777777777ULL,0x89ABCDEF01234567ULL};\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<40;i++) acc=acc*131u+tab[in[i]&7];\n"
     "  return ("+t+")((unsigned)acc ^ (unsigned)(acc>>32));\n"
     "}\n",
     {0x13579BDULL}, "RodataHoist2", opt, fl},

    // 2-D const table tbl[8][4]: address = base + row*4 + col (two scaled idx).
    {p+"_tab2d",
     t+" "+p+"_tab2d("+t+" a){\n"
     "  unsigned char in[40];\n"
     "  for(int i=0;i<40;i++) in[i]=(unsigned char)((a*(i+1))>>2);\n"
     "  static const unsigned tab[8][4]={\n"
     "    {1,2,3,4},{5,6,7,8},{9,10,11,12},{13,14,15,16},\n"
     "    {17,18,19,20},{21,22,23,24},{25,26,27,28},{29,30,31,32}};\n"
     "  unsigned long acc=0;\n"
     "  for(int i=0;i<40;i++){ unsigned r=in[i]&7, c=(in[i]>>3)&3;\n"
     "    acc=acc*131u+tab[r][c]; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2468ACEULL}, "RodataHoist2", opt, fl},

    // const table read at a *computed* index (k - j): the index is a subtraction
    // so the address operand is base + (k-j), exercising a non-trivial index.
    {p+"_subidx",
     t+" "+p+"_subidx("+t+" a){\n"
     "  unsigned char in[40];\n"
     "  for(int i=0;i<40;i++) in[i]=(unsigned char)((a+i*3)>>1);\n"
     "  static const unsigned char tbl[]=\"ABCDEFGHIJKLMNOPQRSTUVWXYZ\"\n"
     "    \"abcdefghijklmnopqrstuvwxyz0123456789+/\";\n"
     "  unsigned long acc=0;\n"
     "  for(int i=0;i<40;i++){ unsigned k=in[i]&63, j=(unsigned)i&7;\n"
     "    unsigned idx=(k>=j)?(k-j):0; acc=acc*31u+tbl[idx&63]; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x3344556ULL}, "RodataHoist2", opt, fl},

    // Mixed element widths: a byte table and a qword table both hoisted in one
    // loop alongside a stack array — each base independently symbol-recognized.
    {p+"_mixwidth",
     t+" "+p+"_mixwidth("+t+" a){\n"
     "  unsigned char in[48];\n"
     "  for(int i=0;i<48;i++) in[i]=(unsigned char)((a*(i+2))>>3);\n"
     "  static const unsigned char b[16]={2,3,5,7,11,13,17,19,\n"
     "    23,29,31,37,41,43,47,53};\n"
     "  static const unsigned long long q[8]={\n"
     "    0x0102030405060708ULL,0x1112131415161718ULL,0x2122232425262728ULL,\n"
     "    0x3132333435363738ULL,0x4142434445464748ULL,0x5152535455565758ULL,\n"
     "    0x6162636465666768ULL,0x7172737475767778ULL};\n"
     "  unsigned long long acc=0;\n"
     "  for(int i=0;i<48;i++){ unsigned idx=in[i]&63;\n"
     "    acc=acc*131u+b[idx&15]+q[idx&7]; }\n"
     "  return ("+t+")((unsigned)acc ^ (unsigned)(acc>>32));\n"
     "}\n",
     {0x1A2B3C4ULL}, "RodataHoist2", opt, fl},

    // signed short (2-byte) element table: scaled by 2 + sign-extending load.
    {p+"_swtab",
     t+" "+p+"_swtab("+t+" a){\n"
     "  unsigned char in[40];\n"
     "  for(int i=0;i<40;i++) in[i]=(unsigned char)((a+i*7)>>2);\n"
     "  static const short tab[16]={-1000,-500,-100,-10,-1,0,1,10,\n"
     "    100,500,1000,2000,-2000,-3000,3000,32000};\n"
     "  long acc=0;\n"
     "  for(int i=0;i<40;i++) acc+=tab[in[i]&15];\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x55AA33CULL}, "RodataHoist2", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeTC("x64rh2", "long", 2, "");
static const std::vector<RoundTripTC> kA64 = makeTC("a64rh2", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makeTC("armrh2", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(RodataHoist2, X64RodataHoist2RT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(RodataHoist2, A64RodataHoist2RT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(RodataHoist2, ARM32RodataHoist2RT,
                         ::testing::ValuesIn(kARM), rtTCName);
