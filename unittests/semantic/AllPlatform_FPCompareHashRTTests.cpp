//===- AllPlatform_FPCompareHashRTTests.cpp - FP compare flag folding -C++-===//
//
// Adversarial probes for the #395 class: scalar FP compares over special values
// (NaN / +-Inf / +-0 / normal) whose results clang fuses so a single compare
// feeds several SETcc/Jcc reading different flag bits (ZF/CF/PF).  Mixing the
// predicates and routing them through both branchless hashes and real branches
// stresses the MedFlags SETCC (Pass 2) and COND_BR (Pass 1) folds, which must
// keep an FP-derived flag unfolded even when a preceding SETcc's partial-
// register merge sits between the consumer and the compare.  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPCmpHashRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPCmpHashRT, Verify) { roundTripX64(GetParam()); }

class X86FPCmpHashRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FPCmpHashRT, Verify) { roundTripX86(GetParam()); }

class A64FPCmpHashRT : public SemanticRoundTripFixture,
                       public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPCmpHashRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32FPCmpHashRT : public SemanticRoundTripFixture,
                         public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FPCmpHashRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeFPCmpHashTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Single-precision: every ordered/unordered relation over the 6x6 special
    // grid.  This is the exact shape that exposed #395 (one ucomiss feeding
    // setae+setne+setb, each preceded by an xor-zero of its byte target).
    {p+"_flt",
     t+" "+p+"_flt("+t+" a){\n"
     "  unsigned s[6]={0x7FC00000u,0x7F800000u,0xFF800000u,0,0x80000000u,0x40490FDBu};\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<6;i++){ float x; __builtin_memcpy(&x,&s[i],4);\n"
     "    for(int j=0;j<6;j++){ float y; __builtin_memcpy(&y,&s[j],4);\n"
     "      acc=acc*3u+(x<y)+2u*(x==y)+4u*(x>y)+8u*(x!=y); } }\n"
     "  return ("+t+")(unsigned long)(acc+(unsigned)(a&0)); }\n",
     {0ULL}, "FPCmpHash", 2},

    // Double-precision variant of the same grid.  unsigned long long so the
    // 64-bit bit patterns survive on ILP32 targets (i386/arm32 long is 32-bit).
    {p+"_dbl",
     t+" "+p+"_dbl("+t+" a){\n"
     "  unsigned long long s[6]={0x7FF8000000000001ULL,0x7FF0000000000000ULL,\n"
     "    0xFFF0000000000000ULL,0ULL,0x8000000000000000ULL,0x400921FB54442D18ULL};\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<6;i++){ double x; __builtin_memcpy(&x,&s[i],8);\n"
     "    for(int j=0;j<6;j++){ double y; __builtin_memcpy(&y,&s[j],8);\n"
     "      acc=acc*3u+(x<y)+2u*(x==y)+4u*(x>y)+8u*(x!=y); } }\n"
     "  return ("+t+")(unsigned long)(acc+(unsigned)(a&0)); }\n",
     {0ULL}, "FPCmpHash", 2},

    // >= / <= emphasis: setae/setbe read CF (and CF|ZF), the carry side of the
    // same one-compare-many-setcc fusion.
    {p+"_gele",
     t+" "+p+"_gele("+t+" a){\n"
     "  unsigned s[6]={0x7FC00000u,0x7F800000u,0xFF800000u,0,0x80000000u,0x40490FDBu};\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<6;i++){ float x; __builtin_memcpy(&x,&s[i],4);\n"
     "    for(int j=0;j<6;j++){ float y; __builtin_memcpy(&y,&s[j],4);\n"
     "      acc=acc*3u+(x>=y)+2u*(x<=y)+4u*(x<y)+8u*(x>y); } }\n"
     "  return ("+t+")(unsigned long)(acc+(unsigned)(a&0)); }\n",
     {0ULL}, "FPCmpHash", 2},

    // Real branches on FP compares: forces COND_BR (Pass 1) flag folding over
    // FP-derived flags, with divergent accumulator updates per arm.
    {p+"_branch",
     t+" "+p+"_branch("+t+" a){\n"
     "  unsigned s[6]={0x7FC00000u,0x7F800000u,0xFF800000u,0,0x80000000u,0x40490FDBu};\n"
     "  unsigned acc=1;\n"
     "  for(int i=0;i<6;i++){ float x; __builtin_memcpy(&x,&s[i],4);\n"
     "    for(int j=0;j<6;j++){ float y; __builtin_memcpy(&y,&s[j],4);\n"
     "      if(x<y) acc=acc*3u+1u; else acc=acc*5u+7u;\n"
     "      if(x==y) acc^=0x9E3779B9u;\n"
     "      if(x!=y) acc=acc*2u+3u; } }\n"
     "  return ("+t+")(unsigned long)(acc+(unsigned)(a&0)); }\n",
     {0ULL}, "FPCmpHash", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeFPCmpHashTC("x64fch", "long");
static const std::vector<RoundTripTC> kX86 = makeFPCmpHashTC("x86fch", "int");
static const std::vector<RoundTripTC> kA64 = makeFPCmpHashTC("a64fch", "long");
static const std::vector<RoundTripTC> kARM = makeFPCmpHashTC("armfch", "int");

INSTANTIATE_TEST_SUITE_P(FPCmpHash, X64FPCmpHashRT,
                         ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPCmpHash, X86FPCmpHashRT,
                         ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPCmpHash, A64FPCmpHashRT,
                         ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPCmpHash, ARM32FPCmpHashRT,
                         ::testing::ValuesIn(kARM), rtTCName);
