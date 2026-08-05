//===- AllPlatform_ConstTableWalkRTTests.cpp - rodata access patterns -*-C++-*-=//
//
// #397 follow-up: stress the constant-pool / rodata redirection across the
// access shapes clang reaches under register pressure once a local const table
// is hoisted — 2D tables, negative-stride walks, signed (sign-extending) loads,
// byte/short/long element widths, and two tables walked at once.  Each keeps a
// long hash plus an index compare live so clang prefers an induction pointer
// over `arr[i]`, the exact condition that left the recompiled object reading the
// original VA before the induction-pointer redirect.  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64CTWalkRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64CTWalkRT, Verify) { roundTripX64(GetParam()); }
class X86CTWalkRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86CTWalkRT, Verify) { roundTripX86(GetParam()); }
class A64CTWalkRT : public SemanticRoundTripFixture,
                    public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64CTWalkRT, Verify) { roundTripAArch64(GetParam()); }
class ARM32CTWalkRT : public SemanticRoundTripFixture,
                      public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32CTWalkRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeCTWalkTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // 2D const table reached row-by-row (row base induction + col index).
    {p+"_t2d",
     t+" "+p+"_t2d("+t+" a){\n"
     "  static const unsigned m[4][4]={{2654435761u,40503u,2246822519u,3266489917u},\n"
     "    {668265263u,374761393u,3332679571u,2147483647u},{16807u,48271u,69621u,1u},\n"
     "    {1103515245u,12345u,1013904223u,1u}};\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<4;i++)for(int j=0;j<4;j++){\n"
     "    unsigned w=m[i][j]+(unsigned)((i<j)?7:11);\n"
     "    acc=acc*31u+w+(acc>>13); }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x77ULL}, "CTWalk", 2},

    // Negative-stride walk (table consumed back to front).
    {p+"_trev",
     t+" "+p+"_trev("+t+" a){\n"
     "  static const unsigned s[8]={9u,99u,999u,9999u,99999u,999999u,9999999u,99999999u};\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=7;i>=0;i--)for(int j=0;j<8;j++){\n"
     "    unsigned v=s[i]^(s[j]+(unsigned)(i>j?3:5));\n"
     "    acc=acc*131u+v; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x5AULL}, "CTWalk", 2},

    // Signed short table — sign-extending loads off a rodata base.
    {p+"_ssht",
     t+" "+p+"_ssht("+t+" a){\n"
     "  static const short s[8]={-32768,-1000,-1,0,1,1000,32767,-12345};\n"
     "  int acc=(int)a;\n"
     "  for(int i=0;i<8;i++)for(int j=0;j<8;j++){\n"
     "    int v=(int)s[i]*(int)s[j]+((i<j)?-7:13);\n"
     "    acc=acc*7+v; }\n"
     "  return ("+t+")(long)acc; }\n",
     {0x3ULL}, "CTWalk", 2},

    // Byte table — sub-word loads, accumulated with a running compare.
    {p+"_btab",
     t+" "+p+"_btab("+t+" a){\n"
     "  static const unsigned char b[12]={0xDE,0xAD,0xBE,0xEF,0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF};\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<12;i++)for(int j=0;j<12;j++){\n"
     "    unsigned v=(unsigned)b[i]+(unsigned)b[j]*((i<j)?2u:3u);\n"
     "    acc=acc*17u+v; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0x9ULL}, "CTWalk", 2},

    // Two const tables walked simultaneously (two induction pointers).
    {p+"_twoarr",
     t+" "+p+"_twoarr("+t+" a){\n"
     "  static const unsigned x[6]={1u,2u,4u,8u,16u,32u};\n"
     "  static const unsigned y[6]={100u,200u,300u,400u,500u,600u};\n"
     "  unsigned acc=(unsigned)a;\n"
     "  for(int i=0;i<6;i++)for(int j=0;j<6;j++){\n"
     "    unsigned v=x[i]*y[j]+x[j]+y[i]+((i<j)?9u:13u);\n"
     "    acc=acc*31u+v; }\n"
     "  return ("+t+")(unsigned long)acc; }\n",
     {0xCULL}, "CTWalk", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeCTWalkTC("x64ctw", "long");
static const std::vector<RoundTripTC> kX86 = makeCTWalkTC("x86ctw", "int");
static const std::vector<RoundTripTC> kA64 = makeCTWalkTC("a64ctw", "long");
static const std::vector<RoundTripTC> kARM = makeCTWalkTC("armctw", "int");

INSTANTIATE_TEST_SUITE_P(CTWalk, X64CTWalkRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CTWalk, X86CTWalkRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(CTWalk, A64CTWalkRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(CTWalk, ARM32CTWalkRT, ::testing::ValuesIn(kARM), rtTCName);
