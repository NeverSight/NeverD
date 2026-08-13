//===- AllPlatform_PtrWalkRTTests.cpp - pointer-walk addressing -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Pointer-walking loops with a RUNTIME trip count so clang keeps them rolled
// (and emits real post-/pre-indexed loads and stores instead of unrolling them
// into explicit-offset accesses).  This is the family that surfaced the #390
// ARM32 post-index address bug; these probes cover every width, both walk
// directions, dual-pointer copies, and strided access across all four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64PtrWalkRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64PtrWalkRT, Verify) { roundTripX64(GetParam()); }

class X86PtrWalkRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86PtrWalkRT, Verify) { roundTripX86(GetParam()); }

class A64PtrWalkRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64PtrWalkRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32PtrWalkRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32PtrWalkRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makePWTC(const char *prefix, const char *T,
                                         int opt, const char *flags) {
  std::string p = prefix, t = T, fl = flags;
  return {
    // Forward post-increment sum (word stride), runtime bound.
    {p+"_fwd",
     t+" "+p+"_fwd("+t+" a){\n"
     "  int b[40]; for(int i=0;i<40;i++) b[i]=(int)(a+i*7);\n"
     "  int n=(int)(a&15)+12; int* q=b; unsigned acc=0;\n"
     "  for(int i=0;i<n;i++) acc=acc*131u+(unsigned)*q++;\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x1234567ULL}, "PtrWalk", opt, fl},

    // Reverse post-decrement walk.
    {p+"_rev",
     t+" "+p+"_rev("+t+" a){\n"
     "  int b[40]; for(int i=0;i<40;i++) b[i]=(int)(a+i*5);\n"
     "  int n=(int)(a&15)+12; int* q=&b[n-1]; unsigned acc=0;\n"
     "  for(int i=0;i<n;i++) acc=acc*131u+(unsigned)*q--;\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x2233445ULL}, "PtrWalk", opt, fl},

    // Dual-pointer copy-with-transform (two writebacks).
    {p+"_copy",
     t+" "+p+"_copy("+t+" a){\n"
     "  int s[40],d[40]; for(int i=0;i<40;i++) s[i]=(int)(a*(i+1));\n"
     "  int n=(int)(a&7)+16; int* sp=s; int* dp=d;\n"
     "  for(int i=0;i<n;i++) *dp++ = *sp++ + i*3;\n"
     "  unsigned h=0; for(int i=0;i<n;i++) h=h*131u+(unsigned)d[i];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x3344556ULL}, "PtrWalk", opt, fl},

    // Byte-stride post-increment.
    {p+"_byte",
     t+" "+p+"_byte("+t+" a){\n"
     "  unsigned char b[80]; for(int i=0;i<80;i++) b[i]=(unsigned char)(a+i*3);\n"
     "  int n=(int)(a&31)+24; unsigned char* q=b; unsigned h=0;\n"
     "  for(int i=0;i<n;i++) h=h*131u+*q++;\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x4455667ULL}, "PtrWalk", opt, fl},

    // Half-word stride post-increment with sign extension.
    {p+"_half",
     t+" "+p+"_half("+t+" a){\n"
     "  short b[60]; for(int i=0;i<60;i++) b[i]=(short)(a+i*11-300);\n"
     "  int n=(int)(a&15)+20; short* q=b; int acc=0;\n"
     "  for(int i=0;i<n;i++) acc += (int)*q++;\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x5566778ULL}, "PtrWalk", opt, fl},

    // Strided (p += 2) reading two lanes per step.
    {p+"_stride2",
     t+" "+p+"_stride2("+t+" a){\n"
     "  int b[64]; for(int i=0;i<64;i++) b[i]=(int)(a+i*13);\n"
     "  int n=(int)(a&7)+12; int* q=b; long acc=0;\n"
     "  for(int i=0;i<n;i++){ acc += (long)q[0]*3 + q[1]; q+=2; }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x6677889ULL}, "PtrWalk", opt, fl},

    // Accumulate-into-memory via post-increment store (read-modify-write walk).
    {p+"_rmw",
     t+" "+p+"_rmw("+t+" a){\n"
     "  int b[48]; for(int i=0;i<48;i++) b[i]=(int)(a+i);\n"
     "  int n=(int)(a&15)+16; int* q=b; int run=0;\n"
     "  for(int i=0;i<n;i++){ run += *q; *q = run; q++; }\n"
     "  unsigned h=0; for(int i=0;i<n;i++) h=h*131u+(unsigned)b[i];\n"
     "  return ("+t+")h;\n"
     "}\n",
     {0x778899AULL}, "PtrWalk", opt, fl},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makePWTC("x64pw", "long", 2, "");
static const std::vector<RoundTripTC> kX86 = makePWTC("x86pw", "int", 2, "");
static const std::vector<RoundTripTC> kA64 = makePWTC("a64pw", "long", 2, "");
static const std::vector<RoundTripTC> kARM = makePWTC("armpw", "int", 2, "");

INSTANTIATE_TEST_SUITE_P(PtrWalk, X64PtrWalkRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(PtrWalk, X86PtrWalkRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(PtrWalk, A64PtrWalkRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(PtrWalk, ARM32PtrWalkRT, ::testing::ValuesIn(kARM), rtTCName);
