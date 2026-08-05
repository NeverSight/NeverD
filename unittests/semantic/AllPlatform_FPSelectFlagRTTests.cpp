//===- AllPlatform_FPSelectFlagRTTests.cpp - FP compare CMOV/select -*-C++-===//
//
// #395 sibling probes for the SELECT (Pass 3) and COND_BR (Pass 1) flag folds:
// an FP compare's ZF/CF feeds a CMOVcc/branch while an *integer* compare also
// lives in the same block.  The MedFlags SELECT fold walks back for an
// INT_EQUAL/INT_SLESS flag setter; it must not walk past the FP compare's
// BOOL_OR flag to a stale earlier integer CMP and fold the conditional to the
// wrong operands.  Special values (NaN/+-Inf/+-0) make any ordered/unordered
// mismatch observable.  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64FPSelRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64FPSelRT, Verify) { roundTripX64(GetParam()); }

class X86FPSelRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86FPSelRT, Verify) { roundTripX86(GetParam()); }

class A64FPSelRT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64FPSelRT, Verify) { roundTripAArch64(GetParam()); }

class ARM32FPSelRT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32FPSelRT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeFPSelTC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // FP-compare selects (CMOVcc reading ucomiss ZF/CF) interleaved with an
    // integer-compare select in the same block.  `a` is runtime so the integer
    // compare is not folded away, keeping an INT_SUB flag setter in the block
    // for the SELECT fold to (wrongly) latch onto if the FP guard fails.
    {p+"_sel",
     t+" "+p+"_sel("+t+" a){\n"
     "  unsigned s[6]={0x7FC00000u,0x7F800000u,0xFF800000u,0,0x80000000u,0x40490FDBu};\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<6;i++){ float x; __builtin_memcpy(&x,&s[i],4);\n"
     "    for(int j=0;j<6;j++){ float y; __builtin_memcpy(&y,&s[j],4);\n"
     "      unsigned ic=((int)(a+i)<(int)(a+j))?100u:200u;\n"
     "      unsigned c1=(x<y)?3u:5u;\n"
     "      unsigned c2=(x==y)?7u:11u;\n"
     "      unsigned c3=(x>y)?13u:17u;\n"
     "      unsigned c4=(x!=y)?19u:23u;\n"
     "      acc=acc*3u+ic+c1+c2+c3+c4; } }\n"
     "  return ("+t+")(unsigned long)(acc+(unsigned)(a&0)); }\n",
     {0ULL}, "FPSel", 2},

    // Double-precision selects, same structure.
    {p+"_seld",
     t+" "+p+"_seld("+t+" a){\n"
     "  unsigned long long s[6]={0x7FF8000000000001ULL,0x7FF0000000000000ULL,\n"
     "    0xFFF0000000000000ULL,0ULL,0x8000000000000000ULL,0x400921FB54442D18ULL};\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<6;i++){ double x; __builtin_memcpy(&x,&s[i],8);\n"
     "    for(int j=0;j<6;j++){ double y; __builtin_memcpy(&y,&s[j],8);\n"
     "      unsigned ic=((int)(a+i)<(int)(a+j))?100u:200u;\n"
     "      unsigned c1=(x<y)?3u:5u;\n"
     "      unsigned c2=(x==y)?7u:11u;\n"
     "      unsigned c3=(x>=y)?13u:17u;\n"
     "      unsigned c4=(x!=y)?19u:23u;\n"
     "      acc=acc*3u+ic+c1+c2+c3+c4; } }\n"
     "  return ("+t+")(unsigned long)(acc+(unsigned)(a&0)); }\n",
     {0ULL}, "FPSel", 2},

    // Branch form: integer Jcc and FP Jcc share the block; exercises the
    // COND_BR (Pass 1) fold's FP guard the same way.
    {p+"_br",
     t+" "+p+"_br("+t+" a){\n"
     "  unsigned s[6]={0x7FC00000u,0x7F800000u,0xFF800000u,0,0x80000000u,0x40490FDBu};\n"
     "  unsigned acc=1;\n"
     "  for(int i=0;i<6;i++){ float x; __builtin_memcpy(&x,&s[i],4);\n"
     "    for(int j=0;j<6;j++){ float y; __builtin_memcpy(&y,&s[j],4);\n"
     "      if((int)(a+i)<(int)(a+j)) acc+=100u; else acc+=200u;\n"
     "      if(x<y) acc=acc*3u+1u; else acc=acc*5u+7u;\n"
     "      if(x==y) acc^=0x9E3779B9u; } }\n"
     "  return ("+t+")(unsigned long)(acc+(unsigned)(a&0)); }\n",
     {0ULL}, "FPSel", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeFPSelTC("x64fs", "long");
static const std::vector<RoundTripTC> kX86 = makeFPSelTC("x86fs", "int");
static const std::vector<RoundTripTC> kA64 = makeFPSelTC("a64fs", "long");
static const std::vector<RoundTripTC> kARM = makeFPSelTC("armfs", "int");

INSTANTIATE_TEST_SUITE_P(FPSel, X64FPSelRT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPSel, X86FPSelRT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPSel, A64FPSelRT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(FPSel, ARM32FPSelRT, ::testing::ValuesIn(kARM), rtTCName);
