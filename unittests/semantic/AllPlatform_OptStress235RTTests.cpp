//===- AllPlatform_OptStress235RTTests.cpp - mixed-stride addressing =====//
//
// Non-power-of-two strides (2D arrays, arrays of padded structs, prime-step
// walks) force real index multiplies (imul / madd / mla) and byte-precise GEP
// arithmetic on stack frames.  This stresses frame-relative addressing
// (#158/#229), induction-pointer recovery (#485/#490/#501) and the stride
// multiply path together, all on stack data so every platform round-trips.
//
//   * arr2d   - int[6][7] (stride 7) row/col/diagonal reductions.
//   * aos     - array of {int,short,char} (padded stride) field gather.
//   * stride3 - prime-step walk with wraparound over a flat int[].
//   * colmaj  - column-major traversal of a 2D array.
//   * tri     - triangular (row-dependent length) nested walk.
//   * gather  - data-derived index drives the next 2D row (nested index).
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress235RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress235RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress235RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress235RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress235RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress235RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress235RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress235RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress235TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // int[6][7]: stride-7 fill then row/col/diagonal reductions.
    {p+"_arr2d",
     t+" "+p+"_arr2d("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<90;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned m[6][7];\n"
     "    for(int r=0;r<6;r++) for(int c=0;c<7;c++){ h=h*1664525u+1013904223u; m[r][c]=h>>8; }\n"
     "    unsigned s=0;\n"
     "    for(int r=0;r<6;r++) s+=m[r][(r*2u)%7];\n"
     "    for(int c=0;c<7;c++) s^=m[(c*3u)%6][c];\n"
     "    for(int d=0;d<6;d++) s+=m[d][d];\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress235", 2},

    // Array of padded structs: field gather by computed index.
    {p+"_aos",
     "struct "+p+"_e{ unsigned x; unsigned short y; unsigned char z; };\n"
     +t+" "+p+"_aos("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<90;it++){ h=h*1103515245u+12345u;\n"
     "    struct "+p+"_e e[10];\n"
     "    for(int k=0;k<10;k++){ h=h*1664525u+1013904223u;\n"
     "      e[k].x=h; e[k].y=(unsigned short)(h>>11); e[k].z=(unsigned char)(h>>21); }\n"
     "    unsigned s=0;\n"
     "    for(int q=0;q<10;q++){ unsigned idx=(h>>q)%10u;\n"
     "      s+=e[idx].x ^ ((unsigned)e[idx].y*3u) ^ ((unsigned)e[idx].z*5u); }\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress235", 2},

    // Prime-step (stride 3) walk with wraparound over a flat array.
    {p+"_stride3",
     t+" "+p+"_stride3("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<90;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned buf[20];\n"
     "    for(int k=0;k<20;k++){ h=h*1664525u+1013904223u; buf[k]=h>>9; }\n"
     "    unsigned s=0,pos=h%20u;\n"
     "    for(int k=0;k<20;k++){ s+=buf[pos]*(unsigned)(k+1); pos=(pos+3u)%20u; }\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress235", 2},

    // Column-major traversal of a 2D array.
    {p+"_colmaj",
     t+" "+p+"_colmaj("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<90;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned m[5][9];\n"
     "    for(int r=0;r<5;r++) for(int c=0;c<9;c++){ h=h*1664525u+1013904223u; m[r][c]=h>>10; }\n"
     "    unsigned s=0;\n"
     "    for(int c=0;c<9;c++) for(int r=0;r<5;r++) s=s*3u+m[r][c];\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress235", 2},

    // Triangular (row-dependent length) nested walk.
    {p+"_tri",
     t+" "+p+"_tri("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<90;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned m[8][8];\n"
     "    for(int r=0;r<8;r++) for(int c=0;c<8;c++){ h=h*1664525u+1013904223u; m[r][c]=h>>12; }\n"
     "    unsigned s=0;\n"
     "    for(int r=0;r<8;r++) for(int c=0;c<=r;c++) s+=m[r][c]*(unsigned)(c+1);\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress235", 2},

    // Data-derived index drives the next 2D row (nested index).
    {p+"_gather",
     t+" "+p+"_gather("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<90;it++){ h=h*1103515245u+12345u;\n"
     "    unsigned m[6][6];\n"
     "    for(int r=0;r<6;r++) for(int c=0;c<6;c++){ h=h*1664525u+1013904223u; m[r][c]=h>>7; }\n"
     "    unsigned s=0,row=h%6u;\n"
     "    for(int k=0;k<12;k++){ unsigned col=(s+(unsigned)k)%6u; unsigned v=m[row][col];\n"
     "      s+=v; row=(v>>3)%6u; }\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress235", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress235TC("x64o235", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress235TC("x86o235", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress235TC("a64o235", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress235TC("armo235", "int");

INSTANTIATE_TEST_SUITE_P(OptStress235, X64OptStress235RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress235, X86OptStress235RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress235, A64OptStress235RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress235, ARM32OptStress235RT, ::testing::ValuesIn(kARM), rtTCName);
