//===- AllPlatform_OptStress270RTTests.cpp - 2D / pointer aggregates -O0 =//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Multi-dimensional arrays, pointer-to-pointer, and struct-of-arrays at -O0 —
// every element access is a fresh frame-relative address computation with an
// explicit row*stride+col multiply, and pointer-to-pointer adds a second load
// before the data load.  This stresses frame addressing, index multiplies, and
// nested loads that -O2 would fold but -O0 leaves explicit.
//
//   * mat2d   - 2D array row*stride+col indexing, walked in both orders.
//   * ptr2ptr - pointer-to-pointer: row pointers into a flat buffer.
//   * soa     - struct-of-arrays gather with a run index.
//   * tri     - triangular (data-dependent inner bound) 2D walk.
//   * colmaj  - column-major stride walk (stride = row count).
//   * blk     - blocked / tiled 2D accumulation.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Only 32-bit ops, so i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress270RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress270RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress270RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress270RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress270RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress270RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress270RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress270RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress270TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 2D array row*stride+col indexing, walked row-major then column-major.
    {p+"_mat2d",
     t+" "+p+"_mat2d("+t+" a){ unsigned h=(unsigned)a; unsigned m[6][7]; unsigned acc=0;\n"
     "  for(int r=0;r<6;r++) for(int c=0;c<7;c++){ h=h*1103515245u+12345u; m[r][c]=h; }\n"
     "  for(int r=0;r<6;r++) for(int c=0;c<7;c++) acc=acc*131u + m[r][c];\n"
     "  for(int c=0;c<7;c++) for(int r=0;r<6;r++) acc=acc*7u + m[r][c];\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress270", 0},

    // Pointer-to-pointer: row pointers into a flat buffer.
    {p+"_ptr2ptr",
     t+" "+p+"_ptr2ptr("+t+" a){ unsigned h=(unsigned)a; unsigned buf[24]; unsigned *rows[4]; unsigned acc=0;\n"
     "  for(int j=0;j<24;j++){ h=h*1103515245u+12345u; buf[j]=h; }\n"
     "  for(int r=0;r<4;r++) rows[r]=&buf[r*6];\n"
     "  for(int i=0;i<160;i++){ h=h*1103515245u+12345u; unsigned r=h%4u, c=(h>>3)%6u;\n"
     "    acc=acc*131u + rows[r][c]; rows[r][c]+=h>>5; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress270", 0},

    // Struct-of-arrays gather with a run index.
    {p+"_soa",
     "struct SOA{ unsigned k[16]; unsigned v[16]; };\n"
     +t+" "+p+"_soa("+t+" a){ unsigned h=(unsigned)a; struct SOA s; unsigned acc=0;\n"
     "  for(int j=0;j<16;j++){ s.k[j]=(unsigned)(j*7+1); s.v[j]=(unsigned)(j*131+9); }\n"
     "  for(int i=0;i<200;i++){ h=h*1103515245u+12345u; unsigned idx=h&15u;\n"
     "    acc=acc*131u + s.k[idx]*3u + s.v[idx]; s.v[idx]^=h>>4; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress270", 0},

    // Triangular (data-dependent inner bound) 2D walk.
    {p+"_tri",
     t+" "+p+"_tri("+t+" a){ unsigned h=(unsigned)a; unsigned m[8][8]; unsigned acc=0;\n"
     "  for(int r=0;r<8;r++) for(int c=0;c<8;c++){ h=h*1103515245u+12345u; m[r][c]=h; }\n"
     "  for(int r=0;r<8;r++) for(int c=0;c<=r;c++) acc=acc*131u + m[r][c] + m[c][r];\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress270", 0},

    // Column-major stride walk (stride = row count).
    {p+"_colmaj",
     t+" "+p+"_colmaj("+t+" a){ unsigned h=(unsigned)a; unsigned buf[40]; unsigned acc=0;\n"
     "  for(int j=0;j<40;j++){ h=h*1103515245u+12345u; buf[j]=h; }\n"
     "  for(int c=0;c<8;c++) for(int r=0;r<5;r++) acc=acc*131u + buf[r*8+c];\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress270", 0},

    // Blocked / tiled 2D accumulation.
    {p+"_blk",
     t+" "+p+"_blk("+t+" a){ unsigned h=(unsigned)a; unsigned m[8][8]; unsigned acc=0;\n"
     "  for(int r=0;r<8;r++) for(int c=0;c<8;c++){ h=h*1103515245u+12345u; m[r][c]=h; }\n"
     "  for(int br=0;br<8;br+=4) for(int bc=0;bc<8;bc+=4)\n"
     "    for(int r=br;r<br+4;r++) for(int c=bc;c<bc+4;c++) acc=acc*131u + m[r][c];\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress270", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress270TC("x64o270", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress270TC("x86o270", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress270TC("a64o270", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress270TC("armo270", "int");

INSTANTIATE_TEST_SUITE_P(OptStress270, X64OptStress270RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress270, X86OptStress270RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress270, A64OptStress270RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress270, ARM32OptStress270RT, ::testing::ValuesIn(kARM), rtTCName);
