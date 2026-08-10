//===- AllPlatform_OptStress54RTTests.cpp - rodata access edge -*-C++*-=//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Edge shapes for rodata constant-pool redirect beyond plain indexed loads:
// volatile-qualified tables, sentinel-guarded entries, union-packed bytes,
// address-of-element pointers, switch-scattered table offsets, and XOR-mix
// with a stack buffer.
//
//   * voltab  - `volatile const` table forces real memory loads each step.
//   * senttab - sentinel-marked sparse table, skip zero slots.
//   * unipak  - union-packed rodata, read bytes/words through `.b`/`.w`.
//   * addrof  - `&T[k]` address-of-element then indirect index.
//   * cashtab - switch on hash picks different table regions.
//   * xormix  - rodata table XOR-mixed with a stack ring buffer.
//
// All integer, fold to one return, no float / 64-bit divide helper.
// All four targets, -O2.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64OptStress54RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64OptStress54RT, Verify) { roundTripX64(GetParam()); }
class X86OptStress54RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86OptStress54RT, Verify) { roundTripX86(GetParam()); }
class A64OptStress54RT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64OptStress54RT, Verify) { roundTripAArch64(GetParam()); }
class ARM32OptStress54RT : public SemanticRoundTripFixture,
                           public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32OptStress54RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeOptStress54TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    {p+"_voltab",
     "static volatile const unsigned T[16]={7,3,11,2,13,5,17,9,19,1,23,8,29,4,31,6};\n"
     +t+" "+p+"_voltab("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<180;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>5)&15u; h=h*131u+T[j]+T[(j+3)&15u]; h^=h>>9; }\n"
     "  return ("+t+")h; }\n",
     {0x71u}, "OptStress54", 2},

    {p+"_senttab",
     "static const unsigned T[16]={0,11,0,22,0,33,0,44,0,55,0,66,0,77,0,88};\n"
     +t+" "+p+"_senttab("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<170;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>6)&15u; unsigned v=T[j]; if(v) h=h*131u+v+T[(j+7)&15u]; }\n"
     "  return ("+t+")h; }\n",
     {0x72u}, "OptStress54", 2},

    {p+"_unipak",
     "static const union { unsigned w[8]; unsigned char b[32]; } U={"
     "{0x01020304u,0x05060708u,0x090a0b0cu,0x0d0e0f10u,"
     "0x11121314u,0x15161718u,0x191a1b1cu,0x1d1e1f20u}};\n"
     +t+" "+p+"_unipak("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<160;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>5)&31u; h=h*131u+U.b[j]+U.w[(j>>2)&7u]; h^=h>>7; }\n"
     "  return ("+t+")h; }\n",
     {0x73u}, "OptStress54", 2},

    {p+"_addrof",
     "static const unsigned T[16]={101,202,303,404,505,606,707,808,"
     "909,111,222,333,444,555,666,777};\n"
     +t+" "+p+"_addrof("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u, h=0;\n"
     "  for(int i=0;i<175;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>4)&15u; const unsigned *q=&T[j];\n"
     "    h=h*131u+*q+q[1]; h^=h>>11; }\n"
     "  return ("+t+")h; }\n",
     {0x74u}, "OptStress54", 2},

    {p+"_cashtab",
     "static const unsigned T[32]={3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3,"
     "11,22,33,44,55,66,77,88,99,10,20,30,40,50,60,70};\n"
     +t+" "+p+"_cashtab("+t+" a){\n"
     "  unsigned s=(unsigned)a, h=0;\n"
     "  for(int i=0;i<190;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned c=(s>>4)&7u, off;\n"
     "    switch(c){ case 0: off=0; break; case 1: off=4; break;\n"
     "      case 2: off=8; break; case 3: off=12; break;\n"
     "      case 4: off=16; break; default: off=20; break; }\n"
     "    unsigned j=(s>>7)&7u; h=h*131u+T[off+j]+T[(off+j+1)&31u]; }\n"
     "  return ("+t+")h; }\n",
     {0x75u}, "OptStress54", 2},

    {p+"_xormix",
     "static const unsigned char K[16]={0x6a,0x2f,0x15,0x8c,0x3b,0x91,0x47,0xe2,"
     "0x5d,0x0a,0x73,0xb8,0x29,0xf4,0x61,0xce};\n"
     +t+" "+p+"_xormix("+t+" a){\n"
     "  unsigned s=(unsigned)a|1u; unsigned char r[16];\n"
     "  for(int i=0;i<16;i++){ s=s*1103515245u+12345u; r[i]=(unsigned char)(s>>9); }\n"
     "  unsigned h=0;\n"
     "  for(int i=0;i<200;i++){ s=s*1103515245u+12345u;\n"
     "    unsigned j=(s>>5)&15u; r[j]^=K[j]; h=h*131u+r[j]+K[(j+5)&15u]; }\n"
     "  return ("+t+")h; }\n",
     {0x76u}, "OptStress54", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeOptStress54TC("x64o54", "long");
static const std::vector<RoundTripTC> kX86 = makeOptStress54TC("x86o54", "int");
static const std::vector<RoundTripTC> kA64 = makeOptStress54TC("a64o54", "long");
static const std::vector<RoundTripTC> kARM = makeOptStress54TC("armo54", "int");

INSTANTIATE_TEST_SUITE_P(OptStress54, X64OptStress54RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress54, X86OptStress54RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress54, A64OptStress54RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(OptStress54, ARM32OptStress54RT, ::testing::ValuesIn(kARM), rtTCName);
