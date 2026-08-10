//===- AllPlatform_OptStress277RTTests.cpp - rodata access at -O0 ========//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// File-scope const-global access at -O0 — the dual of the -O2 OptStress248.  At
// -O0 i386/ARM32 PIC reach globals through explicit GOT-base + GOTOFF / literal-
// pool sequences that #507 showed are bug-prone (the GOT base is spilled to the
// frame and reloaded, fields use disp@GOTOFF, induction pointers spill).  This
// probe stresses those paths with 2D tables, const pointer arrays into other
// globals, const struct arrays, induction-pointer table walks, and a string.
//
//   * tbl2d   - 2D const lookup table walked by computed row/col.
//   * pptr    - const pointer array whose elements point into other globals.
//   * gstruct - const struct array with mixed-width fields gathered by index.
//   * pwalk   - induction-pointer (q++) walk over a const u32 table.
//   * negwalk - reverse induction-pointer (q--) walk from the table end.
//   * strsum  - string-literal byte processing.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress277RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress277RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress277RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress277RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress277RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress277RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress277RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress277RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress277TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 2D const lookup table walked by computed row/col indices.
    {p+"_tbl2d",
     "static const unsigned U2[4][5]={{2654435761u,40503u,2246822519u,3266489917u,97u},\n"
     "  {668265263u,374761393u,3332679571u,2147483647u,193u},\n"
     "  {389u,769u,1543u,3079u,6151u},{12289u,24593u,49157u,98317u,196613u}};\n"
     +t+" "+p+"_tbl2d("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<64;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned r=(h>>5)&3u, c=(h>>9)%5u; acc=acc*131u+U2[r][c]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress277", 0},

    // const pointer array whose elements point into other const globals.
    {p+"_pptr",
     "static const unsigned B0[3]={11u,22u,33u};\n"
     "static const unsigned B1[3]={101u,202u,303u};\n"
     "static const unsigned B2[3]={1001u,2002u,3003u};\n"
     "static const unsigned *const QS[3]={B0,B1,B2};\n"
     +t+" "+p+"_pptr("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<48;i++){ h=h*1103515245u+12345u;\n"
     "    unsigned k=(h>>4)%3u, j=(h>>7)%3u; acc=acc*131u+QS[k][j]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress277", 0},

    // const struct array with mixed-width fields gathered by index.
    {p+"_gstruct",
     "struct Q{ unsigned char b; short s; int w; };\n"
     "static const struct Q QR[5]={{1,1000,100000},{2,-2000,-200000},\n"
     "  {3,3000,300000},{255,-32768,-2000000000},{128,32767,2000000000}};\n"
     +t+" "+p+"_gstruct("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<80;i++){ h=h*1103515245u+12345u; int k=(int)((h>>8)%5u);\n"
     "    acc=acc*131u+(unsigned)QR[k].b+(unsigned)(int)QR[k].s+(unsigned)QR[k].w+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress277", 0},

    // induction-pointer (q++) walk over a const u32 table.
    {p+"_pwalk",
     "static const unsigned WK[8]={40503u,40504u,40505u,40506u,40507u,40508u,40509u,40510u};\n"
     +t+" "+p+"_pwalk("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int rep=0;rep<48;rep++){ h=h*1103515245u+12345u;\n"
     "    const unsigned *q=WK; unsigned s=0;\n"
     "    for(int i=0;i<8;i++){ s=s*131u+(*q ^ (h>>(i&7))); q++; }\n"
     "    acc=acc*131u+s; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress277", 0},

    // reverse induction-pointer (q--) walk starting at the table end.
    {p+"_negwalk",
     "static const unsigned WV[8]={1u,2u,4u,8u,16u,32u,64u,128u};\n"
     +t+" "+p+"_negwalk("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int rep=0;rep<48;rep++){ h=h*1103515245u+12345u;\n"
     "    const unsigned *q=&WV[7]; unsigned s=0;\n"
     "    for(int i=0;i<8;i++){ s=s*131u+(*q + (h&7u)); q--; }\n"
     "    acc=acc*131u+s; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress277", 0},

    // string-literal byte processing (char rodata walked by index).
    {p+"_strsum",
     "static const char SG[]=\"NeverD lifts every capstone opcode, then checks it.\";\n"
     +t+" "+p+"_strsum("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0; int n=(int)sizeof(SG)-1;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u; int idx=(int)((h>>3)%(unsigned)n);\n"
     "    acc=acc*131u+(unsigned)(unsigned char)SG[idx]+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress277", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress277TC("x64o277", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress277TC("x86o277", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress277TC("a64o277", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress277TC("armo277", "int");

INSTANTIATE_TEST_SUITE_P(OptStress277, X64OptStress277RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress277, X86OptStress277RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress277, A64OptStress277RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress277, ARM32OptStress277RT, ::testing::ValuesIn(kARM), rtTCName);
