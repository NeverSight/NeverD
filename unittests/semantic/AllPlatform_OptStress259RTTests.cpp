//===- AllPlatform_OptStress259RTTests.cpp - bitfield structs at -O0 =====//
//
// Bitfield structs at -O0 — OptStress241 drove dense bitfield shift/mask chains
// at -O2; this is the -O0 counterpart, where clang emits the load / shift / mask
// / sign-extend / or-back sequence explicitly per field with every intermediate
// spilled to the frame, instead of folding it.  That unfolded form is where
// sub-register width and sign-extension lift bugs hide.
//
//   * bf_pack   - pack several fields into one word, read each back, accumulate.
//   * bf_signed - signed bitfields (sign-extend on read).
//   * bf_cross  - fields straddling byte boundaries (odd bit offsets).
//   * bf_rmw    - read-modify-write one field, others preserved.
//   * bf_arr    - array of bitfield structs, run-indexed.
//   * bf_mix    - mixed field widths + a plain member, combined.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All four
// targets, -O0.  Storage units are `unsigned`/`int` (32-bit on every target),
// so only 32-bit ops — i386/ARM32 stay libcall-free.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress259RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress259RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress259RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress259RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress259RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress259RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress259RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress259RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress259TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Pack several fields into one word, read each back, accumulate.
    {p+"_bfpack",
     "struct BP{ unsigned a:3; unsigned b:5; unsigned c:7; unsigned d:1; unsigned e:16; };\n"
     +t+" "+p+"_bfpack("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    struct BP s; s.a=h&7u; s.b=(h>>3)&31u; s.c=(h>>8)&127u; s.d=(h>>15)&1u; s.e=(h>>16)&0xffffu;\n"
     "    acc=acc*131u + s.a + s.b*2u + s.c*3u + s.d*4u + s.e; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress259", 0},

    // Signed bitfields (sign-extend on read).
    {p+"_bfsigned",
     "struct BS{ int a:4; int b:6; int c:13; int d:9; };\n"
     +t+" "+p+"_bfsigned("+t+" a){ unsigned h=(unsigned)a; int acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    struct BS s; s.a=(int)(h&15u); s.b=(int)((h>>4)&63u); s.c=(int)((h>>10)&0x1fffu); s.d=(int)((h>>23)&0x1ffu);\n"
     "    acc=acc*131 + s.a + s.b + s.c + s.d; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress259", 0},

    // Fields straddling byte boundaries (odd bit offsets).
    {p+"_bfcross",
     "struct BC{ unsigned a:6; unsigned b:6; unsigned c:6; unsigned d:6; unsigned e:8; };\n"
     +t+" "+p+"_bfcross("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    struct BC s; s.a=h&63u; s.b=(h>>6)&63u; s.c=(h>>12)&63u; s.d=(h>>18)&63u; s.e=(h>>24)&0xffu;\n"
     "    acc=acc*131u + s.a + s.b*2u + s.c*3u + s.d*4u + s.e*5u; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress259", 0},

    // Read-modify-write one field, others preserved.
    {p+"_bfrmw",
     "struct BR{ unsigned a:10; unsigned b:11; unsigned c:11; };\n"
     +t+" "+p+"_bfrmw("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  struct BR s; s.a=1u; s.b=2u; s.c=3u;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    if(h&1u) s.a=(s.a+h)&0x3ffu; else s.b=(s.b^h)&0x7ffu;\n"
     "    s.c=(s.c+(h>>5))&0x7ffu;\n"
     "    acc=acc*131u + s.a + s.b + s.c; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress259", 0},

    // Array of bitfield structs, run-indexed.
    {p+"_bfarr",
     "struct BA{ unsigned k:12; unsigned v:20; };\n"
     +t+" "+p+"_bfarr("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  struct BA t[8];\n"
     "  for(int j=0;j<8;j++){ t[j].k=(unsigned)(j*7+1); t[j].v=(unsigned)(j*131+9); }\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u; unsigned idx=h&7u;\n"
     "    t[idx].k=(t[idx].k+h)&0xfffu; t[idx].v=(t[idx].v^(h>>3))&0xfffffu;\n"
     "    acc=acc*131u + t[idx].k + t[idx].v; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress259", 0},

    // Mixed field widths + a plain member, combined.
    {p+"_bfmix",
     "struct BM{ unsigned char tag; unsigned a:4; unsigned b:20; unsigned c:8; unsigned short w; };\n"
     +t+" "+p+"_bfmix("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<128;i++){ h=h*1103515245u+12345u;\n"
     "    struct BM s; s.tag=(unsigned char)(h&0xffu); s.a=(h>>8)&15u; s.b=(h>>12)&0xfffffu; s.c=(h>>4)&0xffu; s.w=(unsigned short)(h>>16);\n"
     "    acc=acc*131u + s.tag + s.a*2u + s.b + s.c*3u + s.w; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress259", 0},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress259TC("x64o259", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress259TC("x86o259", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress259TC("a64o259", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress259TC("armo259", "int");

INSTANTIATE_TEST_SUITE_P(OptStress259, X64OptStress259RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress259, X86OptStress259RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress259, A64OptStress259RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress259, ARM32OptStress259RT, ::testing::ValuesIn(kARM), rtTCName);
