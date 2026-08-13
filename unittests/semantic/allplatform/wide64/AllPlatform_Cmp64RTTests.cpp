//===- AllPlatform_Cmp64RTTests.cpp - 64-bit compare predicate probes -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Bisects the WideHash64 i386 min/max divergence: isolates the 64-bit signed
// and unsigned compare predicates (cmp lo; sbb hi; setcc) from the conditional
// select that consumes them.  The setcc kernel hashes raw 0/1 predicate values
// (no select), the select kernel uses min/max.  Whichever fails localizes the
// bug to the compare lowering vs the cmov lowering.  All four targets.
//
//===----------------------------------------------------------------------===//

#include "SemanticRoundTripFixture.h"

class X64Cmp64RT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X64Cmp64RT, Verify) { roundTripX64(GetParam()); }

class X86Cmp64RT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(X86Cmp64RT, Verify) { roundTripX86(GetParam()); }

class A64Cmp64RT : public SemanticRoundTripFixture,
                   public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(A64Cmp64RT, Verify) { roundTripAArch64(GetParam()); }

class ARM32Cmp64RT : public SemanticRoundTripFixture,
                     public ::testing::WithParamInterface<RoundTripTC> {};
TEST_P(ARM32Cmp64RT, Verify) { roundTripARM32(GetParam()); }

// clang-format off
static std::vector<RoundTripTC> makeC64TC(const char *prefix, const char *T) {
  std::string p = prefix, t = T;
  return {
    // Raw 64-bit compare predicates hashed as 0/1 (setcc, no select).
    {p+"_setcc",
     t+" "+p+"_setcc("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<12;i++){ long long x=((long long)(a+i)<<32)|(unsigned)(a*5+i);\n"
     "    long long y=((long long)(a*2-i)<<32)|(unsigned)(a+i*9);\n"
     "    unsigned long long ux=(unsigned long long)x, uy=(unsigned long long)y;\n"
     "    acc=acc*131u+(unsigned)(x<y); acc=acc*131u+(unsigned)(x>y);\n"
     "    acc=acc*131u+(unsigned)(x<=y); acc=acc*131u+(unsigned)(x>=y);\n"
     "    acc=acc*131u+(unsigned)(ux<uy); acc=acc*131u+(unsigned)(ux>uy);\n"
     "    acc=acc*131u+(unsigned)(x==y); acc=acc*131u+(unsigned)(x!=y); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x19ULL}, "Cmp64", 2},

    // 64-bit signed min/max via select (cmp lo; sbb hi; cmovcc).
    {p+"_minmax",
     t+" "+p+"_minmax("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<12;i++){ long long x=((long long)(a+i)<<32)|(unsigned)(a*5+i);\n"
     "    long long y=((long long)(a*2-i)<<32)|(unsigned)(a+i*9);\n"
     "    long long mn=x<y?x:y, mx=x<y?y:x;\n"
     "    acc=acc*131u+(unsigned)(mn^(mn>>32)); acc=acc*131u+(unsigned)(mx^(mx>>32)); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x19ULL}, "Cmp64", 2},

    // 64-bit unsigned min/max via select (cmp lo; sbb hi; cmovb/cmova).
    {p+"_uminmax",
     t+" "+p+"_uminmax("+t+" a){\n"
     "  unsigned acc=0;\n"
     "  for(int i=0;i<12;i++){ unsigned long long x=((unsigned long long)(a+i)<<32)|(unsigned)(a*5+i);\n"
     "    unsigned long long y=((unsigned long long)(a*2-i)<<32)|(unsigned)(a+i*9);\n"
     "    unsigned long long mn=x<y?x:y, mx=x<y?y:x;\n"
     "    acc=acc*131u+(unsigned)(mn^(mn>>32)); acc=acc*131u+(unsigned)(mx^(mx>>32)); }\n"
     "  return ("+t+")acc;\n"
     "}\n",
     {0x19ULL}, "Cmp64", 2},
  };
}
// clang-format on

static const std::vector<RoundTripTC> kX64 = makeC64TC("x64c64", "long");
static const std::vector<RoundTripTC> kX86 = makeC64TC("x86c64", "int");
static const std::vector<RoundTripTC> kA64 = makeC64TC("a64c64", "long");
static const std::vector<RoundTripTC> kARM = makeC64TC("armc64", "int");

INSTANTIATE_TEST_SUITE_P(Cmp64, X64Cmp64RT, ::testing::ValuesIn(kX64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Cmp64, X86Cmp64RT, ::testing::ValuesIn(kX86), rtTCName);
INSTANTIATE_TEST_SUITE_P(Cmp64, A64Cmp64RT, ::testing::ValuesIn(kA64), rtTCName);
INSTANTIATE_TEST_SUITE_P(Cmp64, ARM32Cmp64RT, ::testing::ValuesIn(kARM), rtTCName);
