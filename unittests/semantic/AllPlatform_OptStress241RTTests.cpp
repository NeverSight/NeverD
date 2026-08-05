//===- AllPlatform_OptStress241RTTests.cpp - bitfield structs ===========//
//
// Bitfield reads/writes generate dense shift/mask/sign-extend chains that the
// optimizer loves to mis-fold (sub-word width, sign extension on signed
// bitfields, cross-byte field placement).  All containers are 32-bit so no
// 64-bit shift/divide libcalls appear on i386/ARM32.
//
//   * bfpack  - several unsigned fields packed into one word, set+read+sum.
//   * bfsign  - signed bitfields (sign extension on read).
//   * bfcross - fields straddling byte boundaries.
//   * bfupd   - read-modify-write individual fields in place.
//   * bfunion - union of a bitfield struct and a plain word.
//   * bfarr   - array of bitfield structs gathered by index.
//
// Integer in / integer out, LCG-seeded, folded to one integer return.  All
// four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress241RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress241RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress241RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress241RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress241RT : public SemanticRoundTripFixture,
                          public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress241RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress241RT : public SemanticRoundTripFixture,
                            public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress241RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress241TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Unsigned fields packed into one 32-bit word.
    {p+"_bfpack",
     "struct "+p+"_p{ unsigned a:5; unsigned b:7; unsigned c:11; unsigned d:9; };\n"
     +t+" "+p+"_bfpack("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<150;i++){ h=h*1103515245u+12345u; struct "+p+"_p s;\n"
     "    s.a=h; s.b=h>>5; s.c=h>>12; s.d=h>>23;\n"
     "    unsigned r=s.a + s.b*3u + s.c*5u + s.d*7u;\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x12345u}, "OptStress241", 2},

    // Signed bitfields: read sign-extends.
    {p+"_bfsign",
     "struct "+p+"_s{ signed a:6; signed b:13; signed c:13; };\n"
     +t+" "+p+"_bfsign("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<150;i++){ h=h*1103515245u+12345u; struct "+p+"_s s;\n"
     "    s.a=(int)h; s.b=(int)(h>>6); s.c=(int)(h>>19);\n"
     "    int r=(int)s.a + (int)s.b - (int)s.c;\n"
     "    acc=acc*131u+(unsigned)r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x23456u}, "OptStress241", 2},

    // Fields straddling byte boundaries.
    {p+"_bfcross",
     "struct "+p+"_c{ unsigned a:3; unsigned b:10; unsigned c:6; unsigned d:13; };\n"
     +t+" "+p+"_bfcross("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<150;i++){ h=h*1103515245u+12345u; struct "+p+"_c s;\n"
     "    s.a=h; s.b=h>>3; s.c=h>>13; s.d=h>>19;\n"
     "    unsigned r=(s.d<<3)^(s.c<<2)^(s.b<<1)^s.a;\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x34567u}, "OptStress241", 2},

    // Read-modify-write individual fields in place.
    {p+"_bfupd",
     "struct "+p+"_u{ unsigned a:8; unsigned b:8; unsigned c:8; unsigned d:8; };\n"
     +t+" "+p+"_bfupd("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<150;i++){ h=h*1103515245u+12345u; struct "+p+"_u s;\n"
     "    s.a=h; s.b=h>>8; s.c=h>>16; s.d=h>>24;\n"
     "    s.a=(unsigned)(s.a+s.d); s.c=(unsigned)(s.c^s.b); s.b=(unsigned)(s.b*3u);\n"
     "    unsigned r=s.a|(s.b<<8)|(s.c<<16)|(s.d<<24);\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x45678u}, "OptStress241", 2},

    // Union of a bitfield struct and a plain word.
    {p+"_bfunion",
     "union "+p+"_n{ struct{ unsigned lo:12; unsigned mid:8; unsigned hi:12; } f; unsigned w; };\n"
     +t+" "+p+"_bfunion("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int i=0;i<150;i++){ h=h*1103515245u+12345u; union "+p+"_n u; u.w=h;\n"
     "    unsigned r=u.f.lo + u.f.mid*9u + u.f.hi*17u; u.f.mid=(unsigned)(u.f.mid+1u);\n"
     "    r^=u.w;\n"
     "    acc=acc*131u+r+(unsigned)i; }\n"
     "  return ("+t+")acc; }\n",
     {0x56789u}, "OptStress241", 2},

    // Array of bitfield structs gathered by index.
    {p+"_bfarr",
     "struct "+p+"_e{ unsigned t:4; unsigned v:20; unsigned f:8; };\n"
     +t+" "+p+"_bfarr("+t+" a){ unsigned h=(unsigned)a; unsigned acc=0;\n"
     "  for(int it=0;it<90;it++){ h=h*1103515245u+12345u; struct "+p+"_e e[8];\n"
     "    for(int k=0;k<8;k++){ h=h*1664525u+1013904223u; e[k].t=h; e[k].v=h>>4; e[k].f=h>>24; }\n"
     "    unsigned s=0; for(int q=0;q<8;q++){ unsigned idx=(h>>q)&7u;\n"
     "      s+=e[idx].t + e[idx].v*3u + e[idx].f*5u; }\n"
     "    acc=acc*131u+s+(unsigned)it; }\n"
     "  return ("+t+")acc; }\n",
     {0x6789Au}, "OptStress241", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress241TC("x64o241", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress241TC("x86o241", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress241TC("a64o241", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress241TC("armo241", "int");

INSTANTIATE_TEST_SUITE_P(OptStress241, X64OptStress241RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress241, X86OptStress241RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress241, A64OptStress241RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress241, ARM32OptStress241RT, ::testing::ValuesIn(kARM), rtTCName);
